// Lost - the in-game settings panel (F1) and lost.toml (docs/SETTINGS.md).
//
// A short, curated list of the SDK's own settings (cvars) plus Lost's, set by name and saved to
// lost.toml next to the exe (the file the SDK loads at start; the SDK's F4 panel lists every
// cvar, this one only what a player needs). Settings marked [restart] apply on the next start.

#include "settings.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "perf.h"

REXCVAR_DEFINE_BOOL(lost_settings_on_start, false, "Lost", "Open the F1 settings panel at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

std::filesystem::path g_config_path;

struct Choice {
  const char* label;
  const char* value;  // as the cvar takes it
};

struct Setting {
  const char* section;
  const char* label;
  const char* cvar;
  bool restart;
  std::vector<Choice> choices;  // empty = bool checkbox
  const char* help;
};

const std::vector<Setting>& Settings() {
  static const std::vector<Setting> list = {
      {"Display", "Fullscreen", "fullscreen", true, {}, "Borderless fullscreen at start."},
      {"Display", "Output resolution", "resolution", true,
       {{"Game default (720p)", ""}, {"1080p", "1080p"}, {"1440p", "1440p"}, {"4K", "4k"}},
       "The 360 video mode the game is told about, and the window size. EXPERIMENTAL: the game "
       "may render at it or only scale up; Internal resolution is the safe way to sharpen."},
      {"Display", "Internal resolution", "resolution_scale", true,
       {{"1x (720p, console)", "1"}, {"2x (1440p)", "2"}, {"3x (2160p)", "3"},
        {"4x (2880p, heavy)", "4"}},
       "Renders every 3D pass at this multiple of the console's resolution."},
      {"Display", "Upscaling / sharpening", "present_effect", true,
       {{"Bilinear", "bilinear"}, {"AMD CAS (sharpen)", "cas"}, {"AMD FSR 1", "fsr"}},
       "How the final image is scaled to the window."},
      {"Display", "Keep 16:9 (letterbox)", "present_letterbox", false, {},
       "Off stretches the picture to the window."},
      {"Graphics", "Anti-aliasing", "swap_post_effect", true,
       {{"Off", "none"}, {"FXAA", "fxaa"}, {"FXAA (extreme)", "fxaa_extreme"}},
       "Post-process anti-aliasing on the final frame."},
      {"Graphics", "Anisotropic filtering", "anisotropic_override", false,
       {{"Game default", "-1"}, {"Off", "0"}, {"2x", "2"}, {"4x", "3"}, {"8x", "4"}, {"16x", "5"}},
       "Sharper textures at oblique angles."},
      {"Frame rate", "Gameplay frame rate", "lost_target_fps", false,
       {{"30 fps (console)", "30"}, {"60 fps", "60"}, {"120 fps", "120"}, {"144 fps", "144"},
        {"Max (uncapped)", "0"}},
       "The console runs gameplay at 30 fps and menus/cinematics at 60. The game's timing is "
       "real-time based, so higher rates keep the speed right; report anything that feels off "
       "(QTEs, physics, animation)."},
      {"Frame rate", "Frame rate cap", "lost_fps_cap", false,
       {{"None", "0"}, {"30 fps", "30"}, {"60 fps", "60"}, {"120 fps", "120"}, {"144 fps", "144"}},
       "Upper limit on presented frames (set automatically with the gameplay frame rate)."},
  };
  return list;
}

// Writes the player settings (the list above) into lost.toml. Not rex::cvar::SaveConfig: that
// writes every flag that isn't at its default, including command-line-only ones (--log_file,
// --user_data_root, a test's --lost_autopilot), which the next normal start would then load.
// Lines for other keys a player added by hand are kept.
bool SaveConfigFile() {
  if (g_config_path.empty()) return false;
  std::vector<std::string> managed;
  for (const Setting& s : Settings()) managed.push_back(s.cvar);
  std::vector<std::string> lines;
  std::vector<std::pair<std::string, std::string>> previous;  // our keys' lines in the file
  if (std::ifstream in(g_config_path); in) {
    for (std::string line; std::getline(in, line);) {
      const size_t eq = line.find('=');
      std::string key = line.substr(0, eq == std::string::npos ? 0 : eq);
      key.erase(key.find_last_not_of(" \t") + 1);
      key.erase(0, key.find_first_not_of(" \t"));
      const bool ours = std::find(managed.begin(), managed.end(), key) != managed.end();
      if (ours) previous.emplace_back(key, line);
      if (!ours && line.rfind("# Lost settings", 0) != 0) lines.push_back(line);
    }
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  if (!lines.empty()) lines.push_back("");
  lines.push_back("# Lost settings (F1 panel and the game's Options page)");
  for (const std::string& name : managed) {
    // A value from this start's command line is not a setting: keep what the file had.
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kCommandLine) {
      for (const auto& [key, line] : previous) {
        if (key == name) lines.push_back(line);
      }
      continue;
    }
    if (!rex::cvar::HasNonDefaultValue(name)) continue;
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
    if (!info) continue;
    const rex::cvar::FlagType type = info->type;
    std::string value = rex::cvar::GetFlagByName(name);
    if (type == rex::cvar::FlagType::String) {
      value = "\"" + value + "\"";
    } else if (type == rex::cvar::FlagType::Boolean) {
      value = (value == "true" || value == "1") ? "true" : "false";
    }
    lines.push_back(name + " = " + value);
  }
  std::ofstream out(g_config_path, std::ios::trunc);
  if (!out) {
    REXLOG_WARN("Lost: cannot write {}", g_config_path.string());
    return false;
  }
  for (const std::string& line : lines) out << line << '\n';
  return true;
}

class SettingsPanel : public rex::ui::ImGuiDialog {
 public:
  explicit SettingsPanel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  std::atomic<bool> visible{false};

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible.load()) return;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::Begin("Lost settings  (F1)", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::End();
      if (!open) visible = false;
      return;
    }
    const char* section = nullptr;
    bool changed = false;
    for (const Setting& s : Settings()) {
      if (!section || std::string(section) != s.section) {
        section = s.section;
        ImGui::Spacing();
        ImGui::SeparatorText(section);
      }
      ImGui::PushID(s.cvar);
      const std::string current = rex::cvar::GetFlagByName(s.cvar);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(s.label);
      if (s.restart) {
        ImGui::SameLine();
        ImGui::TextDisabled("[restart]");
      }
      if (ImGui::IsItemHovered() || ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.help);
      ImGui::SameLine(250);
      ImGui::SetNextItemWidth(280);
      if (s.choices.empty()) {
        bool v = current == "true" || current == "1";
        if (ImGui::Checkbox("##v", &v)) {
          changed |= rex::cvar::SetFlagByName(s.cvar, v ? "true" : "false");
        }
      } else {
        int sel = -1;
        for (size_t i = 0; i < s.choices.size(); ++i) {
          if (current == s.choices[i].value) sel = int(i);
        }
        const char* preview = sel >= 0 ? s.choices[sel].label : current.c_str();
        if (ImGui::BeginCombo("##v", preview)) {
          for (size_t i = 0; i < s.choices.size(); ++i) {
            if (ImGui::Selectable(s.choices[i].label, int(i) == sel)) {
              changed |= rex::cvar::SetFlagByName(s.cvar, s.choices[i].value);
            }
          }
          ImGui::EndCombo();
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.help);
      ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Overlay");
    bool hud = lost::perf::HudVisible();
    if (ImGui::Checkbox("Performance HUD (F2)", &hud)) lost::perf::SetHudVisible(hud);

    if (changed) {
      SaveConfigFile();
      status_ = "Saved to " + g_config_path.filename().string();
    }
    const auto pending = rex::cvar::GetPendingRestartFlags();
    ImGui::Spacing();
    ImGui::Separator();
    if (!pending.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                         "Restart the game to apply %zu change(s).", pending.size());
    }
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());
    if (ImGui::Button("Reset all to defaults")) {
      for (const Setting& s : Settings()) rex::cvar::ResetToDefault(s.cvar);
      SaveConfigFile();
      status_ = "Defaults restored";
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) open = false;
    ImGui::End();
    if (!open) visible = false;
  }

 private:
  std::string status_;
};

SettingsPanel* g_panel = nullptr;

}  // namespace

namespace lost::settings {

void SetConfigPath(const std::filesystem::path& path) { g_config_path = path; }

bool Save() { return SaveConfigFile(); }

void CreatePanel(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer || g_panel) return;
  g_panel = new SettingsPanel(drawer);  // lives for the process
  g_panel->visible = REXCVAR_GET(lost_settings_on_start);
  rex::ui::RegisterBind("bind_lost_settings", "F1", "Open the Lost settings panel",
                        [] { if (g_panel) g_panel->visible = !g_panel->visible.load(); });
  REXLOG_INFO("Lost: settings panel on F1, saved to {}", g_config_path.string());
}

}  // namespace lost::settings
