// Lost - frame-time measurement, hitch log, perf HUD (perf.cpp).
#pragma once

#include <string>

#include <rex/ui/overlay/debug_overlay.h>

namespace rex::ui {
class ImGuiDrawer;
}

namespace lost::perf {
void RaiseTimerResolution();
void ApplyTargetFrameRate();  // --lost_target_fps -> guest refresh + present cap (before setup)
void StartHangWatchdog();  // --lost_hang_report_s: thread stacks when frames stop
void CreateHud(rex::ui::ImGuiDrawer* drawer);
rex::ui::FrameStats Stats();  // for the SDK's F3 debug overlay
bool HudVisible();
std::string OverlayText();  // native mode's FPS counter text ("" while the HUD is off)
void SetHudVisible(bool visible);
}  // namespace lost::perf
