// lost - Lost: Via Domus (Xbox 360) on ReXGlue.

#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

#include <rex/rex_app.h>
#include <rex/system/kernel_state.h>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

#include "autopilot.h"
#include "crash_report.h"
#include "perf.h"
#if defined(LOST_NATIVE_RENDERER)
#include "../native/text_overlay.h"
#endif
#include "settings.h"

void LostStartBinkSampler();  // bink_probe.cpp
void LostStartUiProbe();      // ui_probe.cpp

// Writes the loaded executable image (already decrypted and decompressed by the runtime's XEX
// loader) to a file right after load, for the offline tools (tools/find_missed_functions.py,
// tools/check_switch_tables.py). No retail key is involved. The file is game data: keep it in
// private/, never commit it.
REXCVAR_DEFINE_STRING(lost_dump_image, "", "Lost",
                      "Write the loaded executable image (0x82000000, 0x9F0000 bytes) to this path "
                      "after load, then keep running")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

class LostApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<LostApp>(new LostApp(ctx, "lost",
        PPCImageConfig));
  }

 protected:
  // One place for saves and the shader cache however the game is started: the SDK defaults
  // (Documents\lost, cache inside it) unless --user_data_root / --cache_root say otherwise. The
  // game data defaults to a "game" folder next to the exe (packaged) or the repo's port\assets
  // (dev build in port\out\build\<preset>), so lost.exe also starts by double-click.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    lost::settings::SetConfigPath(paths.config_path);  // lost.toml, for the F1 panel
    if (!paths.game_data_root.empty()) return;
    const auto exe_dir = paths.config_path.parent_path();
    std::error_code ec;
    for (const auto& candidate : {exe_dir / "game", exe_dir / ".." / ".." / ".." / "assets"}) {
      if (std::filesystem::exists(candidate / "default.xex", ec)) {
        paths.game_data_root = std::filesystem::weakly_canonical(candidate, ec);
        return;
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // logs/crash-<time>.txt + .dmp with the guest stack on any fault (from graw360-recomp).
    graw::crash_report::InstallCrashReporter();
    // 0.5 ms Windows timer resolution: the runtime paces vblanks with Sleep(1) (perf.cpp).
    lost::perf::RaiseTimerResolution();
    lost::perf::ApplyTargetFrameRate();
    lost::perf::StartHangWatchdog();
    // Without a GPU plugin the runtime ignores every Vd* call and the game never draws.
    // --gpu_plugin on the command line still takes precedence.
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    REXLOG_INFO("Lost: GPU plugin '{}'", config.gpu_plugin);

    // Every controller (and the keyboard) drives player 1, the signed-in profile that saves: a
    // virtual XInput device (e.g. a Logitech or DS4 driver) would otherwise take slot 0 and push
    // the real pad to player 2 (GRAW lesson). Plus the scripted controller for unattended
    // tests (--lost_autopilot, autopilot.h).
    if (config.input_factory) {
      config.input_factory = [base = config.input_factory](bool tool_mode) {
        auto input = base(tool_mode);
        if (auto* system = dynamic_cast<rex::input::InputSystem*>(input.get())) {
          system->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
          if (auto autopilot = graw::autopilot::Create()) system->AddDriver(std::move(autopilot));
        }
        return input;
      };
    }
  }

  // Perf HUD (F2) and the SDK's F3 overlay frame stats (perf.cpp).
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    lost::perf::CreateHud(drawer);
#if defined(LOST_NATIVE_RENDERER)
    // The native renderer owns the window's swapchain, where the ImGui HUD can't draw: it shows
    // the same FPS readout itself (F2 toggles both).
    lost::native::SetOverlayTextProvider([] { return lost::perf::OverlayText(); });
#endif
    lost::settings::CreatePanel(drawer);
    SetGuestFrameStats([] { return lost::perf::Stats(); });
  }

  void OnPostLoadXexImage() override {
    LostStartBinkSampler();
    LostStartUiProbe();
    const std::string path = REXCVAR_GET(lost_dump_image);
    if (path.empty()) return;
    constexpr uint32_t kBase = 0x82000000, kSize = 0x9F0000;
    const uint8_t* image = rex::system::kernel_memory()->virtual_membase() + kBase;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(image, 1, kSize, f);
      std::fclose(f);
      REXLOG_INFO("Lost: wrote loaded image ({} bytes) to {}", kSize, path);
    } else {
      REXLOG_ERROR("Lost: cannot write image dump to {}", path);
    }
  }
};
