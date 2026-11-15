// Lost - in-game settings panel (F1) and lost.toml (settings.cpp).
#pragma once

#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
}

namespace lost::settings {
void SetConfigPath(const std::filesystem::path& path);  // the SDK's lost.toml next to the exe
void CreatePanel(rex::ui::ImGuiDrawer* drawer);
bool Save();  // write the current settings to lost.toml
}  // namespace lost::settings
