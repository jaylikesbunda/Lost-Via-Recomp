// Lost - frame-time measurement, hitch log, perf HUD (docs/PERF.md).
//
// The prebuilt SDK has its perf counters compiled out, so --perf_log_csv writes nothing; this
// measures from the game's side instead. The game presents through sub_82397D70 (its D3D Swap,
// the only caller of VdSwap). For each frame:
//   work = end of the previous Swap -> start of this one (game simulation + D3D submission)
//   swap = time inside Swap (waiting for a vblank / ring space / the GPU)
//   frame = work + swap (present to present)
// Outputs:
//   - "Perf: ..." summary every --lost_perf_summary_s seconds (fps, median, 1% low, max, hitches,
//     process CPU, work/swap averages);
//   - "Perf hitch: ..." for each frame over --lost_perf_hitch_ms, with a wall-clock timestamp so
//     tools/perf_report.py can line it up with the log (shader/pipeline creation, file opens...);
//   - --lost_perf_csv <file>: one row per frame;
//   - an on-screen HUD (F2 toggles; --lost_perf_hud) and the SDK's F3 overlay stats.
// Also raises the Windows timer resolution (GRAW #11: the default 15.6 ms makes the runtime's
// Sleep(1)s pace vblanks late and in bursts).

#include "perf.h"

#include "crash_report.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(lost_perf_csv, "", "Lost",
                      "Write one CSV row per presented frame (wall time, frame/work/swap us)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(lost_perf_hitch_ms, 50, "Lost",
                     "Log every frame longer than this many milliseconds as a hitch (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(lost_perf_summary_s, 5, "Lost",
                     "Log a frame-time summary every this many seconds (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(lost_perf_hud, true, "Lost", "Show the perf HUD at start (F2 toggles it)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(lost_fps_cap, 0, "Lost",
                     "Present at most this many frames per second (0 = the game's own pacing)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// High frame rate (docs/PERF.md, "Frame rate"): gameplay presents every 2nd vblank and menus /
// in-engine cinematics every vblank, and the game's timing is wall-clock based (a 120 Hz guest
// refresh ran the opening's scripted sequence to the same moment as 60 Hz, at twice the frames).
// So the target is reached by running the guest display at 2x the target and capping presents
// at the target (cinematics would otherwise run at 2x). 30 = the console's own pacing.
REXCVAR_DEFINE_INT32(lost_target_fps, 30, "Lost",
                     "Gameplay frame rate: 30 (console), 60, 120, 144, 0 = as fast as possible")
    .allowed({"30", "60", "120", "144", "0"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(lost_hang_report_s, 20, "Lost",
                     "After this many seconds without a presented frame, write every thread's "
                     "stack to logs/hang-<time>.txt (once; 0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(lost_profile_at_s, 0, "Lost",
                     "Debug: at this many seconds after start, sample the busiest threads for "
                     "lost_profile_s and write logs/profile-<time>.txt (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(lost_profile_s, 10, "Lost", "Debug: length of the lost_profile_at_s profile")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(lost_timer_resolution_us, 500, "Lost",
                     "Windows timer resolution to request at start-up, microseconds (0 = system "
                     "default 15.6 ms)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

using Clock = std::chrono::steady_clock;

struct Frame {
  uint32_t frame_us, work_us, swap_us;
};

constexpr size_t kHistory = 240;  // frames shown in the HUD graph

std::mutex g_mutex;
// Written by the presenting thread, read by the HUD (UI thread) under g_mutex.
std::array<float, kHistory> g_hist_ms{};
size_t g_hist_pos = 0;
std::atomic<uint64_t> g_frames{0};
uint64_t g_hitches = 0;
std::string g_vblank_hz_pending;  // gpu_vblank_hz to set once the GPU plugin has registered it
double g_last_fps = 0, g_last_median = 0, g_last_low = 0, g_last_max = 0, g_last_work = 0,
       g_last_swap = 0, g_last_cpu = 0;
Frame g_last{};

// Presenting-thread state.
Clock::time_point g_prev_swap_end{}, g_window_start{};
std::vector<Frame> g_window;
FILE* g_csv = nullptr;
bool g_csv_tried = false;
uint64_t g_cpu_prev_100ns = 0;

uint32_t Us(Clock::duration d) {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
  return uint32_t(std::clamp<int64_t>(us, 0, 60'000'000));
}

std::string WallStamp() {
  // Same shape as the runtime's log timestamps ("2026-11-14 22:29:00.477").
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, int(ms.count()));
  return buf;
}

// Process CPU time (all threads), 100 ns units.
uint64_t ProcessCpu100ns() {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  return (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
         (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
#else
  return 0;
#endif
}

void Summarise(Clock::duration window_len) {
  if (g_window.empty()) return;
  std::vector<uint32_t> ft;
  ft.reserve(g_window.size());
  double total = 0, work = 0, swap = 0;
  uint32_t mx = 0;
  size_t over33 = 0, over50 = 0;
  for (const Frame& f : g_window) {
    ft.push_back(f.frame_us);
    total += f.frame_us;
    work += f.work_us;
    swap += f.swap_us;
    mx = std::max(mx, f.frame_us);
    over33 += f.frame_us > 34000;
    over50 += f.frame_us > 50000;
  }
  std::sort(ft.begin(), ft.end());
  const size_t n = ft.size();
  const double median = ft[n / 2] / 1000.0;
  const double p99 = ft[std::min(n - 1, n * 99 / 100)] / 1000.0;
  const double fps = n / (total / 1e6);
  const uint64_t cpu = ProcessCpu100ns();
  const double wall_s = std::chrono::duration<double>(window_len).count();
  const double cpu_pct =
      g_cpu_prev_100ns && wall_s > 0 ? (cpu - g_cpu_prev_100ns) / 1e7 / wall_s * 100.0 : 0.0;
  g_cpu_prev_100ns = cpu;
  REXLOG_INFO(
      "Perf: {:.1f} fps avg over {} frames | median {:.2f} ms, 1% low {:.1f} fps ({:.1f} ms), "
      "max {:.1f} ms | >34 ms {} ({:.1f}%), >50 ms {} | work {:.2f} ms, swap {:.2f} ms avg | "
      "process CPU {:.0f}% (100% = 1 core)",
      fps, n, median, 1000.0 / p99, p99, mx / 1000.0, over33, 100.0 * over33 / n, over50,
      work / n / 1000.0, swap / n / 1000.0, cpu_pct);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_last_fps = fps;
  g_last_median = median;
  g_last_low = 1000.0 / p99;
  g_last_max = mx / 1000.0;
  g_last_work = work / n / 1000.0;
  g_last_swap = swap / n / 1000.0;
  g_last_cpu = cpu_pct;
}

void OnFrame(Clock::time_point swap_start, Clock::time_point swap_end) {
  if (!g_vblank_hz_pending.empty() && rex::cvar::SetFlagByName("gpu_vblank_hz", g_vblank_hz_pending)) {
    REXLOG_INFO("Perf: vblank timer {} Hz", g_vblank_hz_pending);
    g_vblank_hz_pending.clear();
  }
  if (!g_csv_tried) {
    g_csv_tried = true;
    const std::string path = REXCVAR_GET(lost_perf_csv);
    if (!path.empty()) {
      g_csv = std::fopen(path.c_str(), "w");
      if (g_csv) std::fputs("frame,wall,frame_us,work_us,swap_us\n", g_csv);
      REXLOG_INFO("Perf: per-frame CSV {}", g_csv ? path : "could not be opened: " + path);
    }
  }
  if (g_prev_swap_end == Clock::time_point{}) {
    g_prev_swap_end = g_window_start = swap_end;
    g_cpu_prev_100ns = ProcessCpu100ns();
    return;
  }
  Frame f;
  f.work_us = Us(swap_start - g_prev_swap_end);
  f.swap_us = Us(swap_end - swap_start);
  f.frame_us = Us(swap_end - g_prev_swap_end);
  g_prev_swap_end = swap_end;
  g_window.push_back(f);
  uint64_t frame_no;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    frame_no = ++g_frames;
    g_hist_ms[g_hist_pos] = f.frame_us / 1000.0f;
    g_hist_pos = (g_hist_pos + 1) % kHistory;
    g_last = f;
  }
  const int32_t hitch_ms = REXCVAR_GET(lost_perf_hitch_ms);
  const bool hitch = hitch_ms > 0 && f.frame_us >= uint32_t(hitch_ms) * 1000;
  std::string wall;
  if (hitch || g_csv) wall = WallStamp();
  if (hitch) {
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      ++g_hitches;
    }
    REXLOG_WARN("Perf hitch: frame {} ended {} took {:.1f} ms (work {:.1f} ms, swap {:.1f} ms)",
                frame_no, wall, f.frame_us / 1000.0, f.work_us / 1000.0, f.swap_us / 1000.0);
  }
  if (g_csv) {
    std::fprintf(g_csv, "%llu,%s,%u,%u,%u\n", (unsigned long long)frame_no, wall.c_str(),
                 f.frame_us, f.work_us, f.swap_us);
    if (frame_no % 120 == 0) std::fflush(g_csv);
  }
  const int32_t every = REXCVAR_GET(lost_perf_summary_s);
  if (every > 0 && swap_end - g_window_start >= std::chrono::seconds(every)) {
    Summarise(swap_end - g_window_start);
    g_window.clear();
    g_window_start = swap_end;
  }
}

// The HUD: top-left, no input, F2 toggles.
class PerfHud : public rex::ui::ImGuiDialog {
 public:
  explicit PerfHud(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  std::atomic<bool> visible{true};

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible.load()) return;
    std::array<float, kHistory> hist;
    size_t pos;
    Frame last;
    double fps, median, low, mx, work, swap, cpu;
    uint64_t hitches;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      hist = g_hist_ms;
      pos = g_hist_pos;
      last = g_last;
      fps = g_last_fps;
      median = g_last_median;
      low = g_last_low;
      mx = g_last_max;
      work = g_last_work;
      swap = g_last_swap;
      cpu = g_last_cpu;
      hitches = g_hitches;
    }
    std::array<float, kHistory> ordered;
    for (size_t i = 0; i < kHistory; ++i) ordered[i] = hist[(pos + i) % kHistory];
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##lost_perf_hud", nullptr, flags)) {
      const float now_ms = last.frame_us / 1000.0f;
      ImGui::Text("%.0f fps  %.1f ms", now_ms > 0 ? 1000.0f / now_ms : 0.0f, now_ms);
      ImGui::Text("avg %.1f fps  1%% low %.1f  max %.0f ms", fps, low, mx);
      ImGui::Text("median %.2f ms  work %.1f  swap %.1f", median, work, swap);
      ImGui::Text("hitches %llu  CPU %.0f%%", (unsigned long long)hitches, cpu);
      ImGui::PlotLines("##ft", ordered.data(), int(kHistory), 0, nullptr, 0.0f, 66.7f,
                       ImVec2(240, 48));
    }
    ImGui::End();
  }
};

PerfHud* g_hud = nullptr;

}  // namespace

namespace {

void SleepUs(int64_t us) {
  if (us <= 0) return;
#if defined(_WIN32)
  // A high-resolution waitable timer sleeps well under a millisecond (Sleep can't).
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -us * 10;
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

// --lost_fps_cap: wait until the next frame is due before presenting. Deadlines step by whole
// periods so sleep jitter doesn't drift the rate; after a long frame the schedule restarts from
// now instead of letting later frames catch up (GRAW's limiter).
void LimitFrameRate() {
  const int32_t cap = REXCVAR_GET(lost_fps_cap);
  static Clock::time_point next{};
  if (cap <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::nanoseconds(1'000'000'000LL / cap);
  Clock::time_point now = Clock::now();
  if (next != Clock::time_point{} && now < next) {
    const int64_t sleep_us =
        std::chrono::duration_cast<std::chrono::microseconds>(next - now).count() - 500;
    SleepUs(sleep_us);
    while ((now = Clock::now()) < next) std::this_thread::yield();
  }
  next = next != Clock::time_point{} && now - next < period ? next + period : now + period;
}

}  // namespace

// The game's D3D Swap (only caller of VdSwap). The native renderer needs this call too, and only
// one REX_HOOK_RAW may own the function, so this hook is the one and it hands the call to the
// native renderer's tracker (lost_d3d_hooks.cpp) when that is built in.
#if defined(LOST_NATIVE_RENDERER)
namespace lost::native {
void HookSwap(PPCContext& ctx, uint8_t* base);
}
#endif

REX_EXTERN(__imp__sub_82397D70);
REX_HOOK_RAW(sub_82397D70) {
  const Clock::time_point start = Clock::now();
  LimitFrameRate();  // its wait counts as "swap" (waiting to present), not game work
#if defined(LOST_NATIVE_RENDERER)
  lost::native::HookSwap(ctx, base);
#else
  __imp__sub_82397D70(ctx, base);
#endif
  OnFrame(start, Clock::now());
}

namespace lost::perf {

void RaiseTimerResolution() {
#if defined(_WIN32)
  const int32_t requested_us = REXCVAR_GET(lost_timer_resolution_us);
  if (requested_us <= 0) return;
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling,
                        sizeof(throttling));
  using SetResolution = LONG(WINAPI*)(ULONG, BOOLEAN, ULONG*);
  auto set_resolution = reinterpret_cast<SetResolution>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution"));
  ULONG current = 0;
  if (set_resolution && set_resolution(ULONG(requested_us) * 10, TRUE, &current) >= 0) {
    REXLOG_INFO("Perf: timer resolution {} us (requested {} us)", current / 10, requested_us);
  } else {
    REXLOG_WARN("Perf: couldn't raise the timer resolution");
  }
#endif
}

void ApplyTargetFrameRate() {
  // 30 = the console's pacing: 60 Hz display, the game presents every 2nd vblank, no cap.
  // Anything else runs the guest display at kFineRefreshHz, so the game's "every 2nd vblank"
  // wait is only ~2 ms and no longer rounds frame times up to whole display intervals (at a
  // 240 Hz display a 9 ms frame took 12.5 ms), and the precise present limiter (LimitFrameRate)
  // sets the rate: the target, or none for 0 ("Max"). The game's timing is wall-clock based
  // (checked in gameplay at 30/60/120), and the vblank thread follows refresh changes, so this
  // applies live.
  // The vblank timer is the GPU plugin's (gpu_vblank_hz, lost-bd833a2-live-refresh-rate); the
  // rate the game is told (video_mode_refresh_rate) is capped at 240 by the stock runtime.
  constexpr int32_t kFineRefreshHz = 960;
  const int32_t target = REXCVAR_GET(lost_target_fps);
  const bool console = target == 30;
  const int32_t refresh = console ? 60 : kFineRefreshHz;
  const int32_t desired_cap = console ? 0 : target;  // 0 (Max) = no cap
  rex::cvar::SetFlagByName("video_mode_refresh_rate", console ? "60" : "240");
  // gpu_vblank_hz belongs to the GPU plugin, which registers it only when it loads (after
  // OnPreSetup): if it isn't there yet, OnFrame applies it on the first frames.
  g_vblank_hz_pending = console ? "0" : std::to_string(refresh);
  if (rex::cvar::SetFlagByName("gpu_vblank_hz", g_vblank_hz_pending)) g_vblank_hz_pending.clear();
  // The cap follows the target unless the player chose another one (F1): it is replaced only
  // when unset, when it was set here before, or when it is above the target.
  static int32_t auto_cap = -1;
  const int32_t cap = REXCVAR_GET(lost_fps_cap);
  if (auto_cap < 0) auto_cap = cap == desired_cap ? cap : 0;  // first call: saved cap == ours
  if (cap == 0 || cap == auto_cap || (desired_cap > 0 && cap > desired_cap)) {
    auto_cap = desired_cap;
    REXCVAR_SET(lost_fps_cap, auto_cap);
  }
  REXLOG_INFO("Perf: target {} fps: video mode {} Hz, vblank timer {} Hz, presents capped at {}",
              target, rex::cvar::GetFlagByName("video_mode_refresh_rate"),
              rex::cvar::GetFlagByName("gpu_vblank_hz"), REXCVAR_GET(lost_fps_cap));
  static bool registered = false;
  if (!registered) {
    registered = true;
    rex::cvar::RegisterChangeCallback("lost_target_fps",
                                      [](std::string_view, std::string_view) { ApplyTargetFrameRate(); });
  }
}

namespace {
// Hang watchdog: frames stop -> one report with every thread's stack (crash_report.cpp).
void Watchdog() {
  const int limit_s = REXCVAR_GET(lost_hang_report_s);
  uint64_t last = 0;
  auto since = Clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const uint64_t frames = g_frames;
    if (frames != last || frames == 0) {
      last = frames;
      since = Clock::now();
      continue;
    }
    if (Clock::now() - since >= std::chrono::seconds(limit_s)) {
      REXLOG_ERROR("Perf: no frame for {} s; writing thread stacks", limit_s);
      const std::string path = graw::crash_report::DumpAllThreads();
      REXLOG_ERROR("Perf: hang report {}", path.empty() ? "failed" : path);
      return;
    }
  }
}
}  // namespace

void StartHangWatchdog() {
  if (REXCVAR_GET(lost_hang_report_s) > 0) std::thread(Watchdog).detach();
  if (const int at = REXCVAR_GET(lost_profile_at_s); at > 0) {
    std::thread([at] {
      std::this_thread::sleep_for(std::chrono::seconds(at));
      REXLOG_INFO("Perf: profiling for {} s", REXCVAR_GET(lost_profile_s));
      REXLOG_INFO("Perf: profile {}", graw::crash_report::ProfileThreads(REXCVAR_GET(lost_profile_s)));
    }).detach();
  }
}

void CreateHud(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer || g_hud) return;
  g_hud = new PerfHud(drawer);  // lives for the process; the drawer owns drawing it
  g_hud->visible = REXCVAR_GET(lost_perf_hud);
  rex::ui::RegisterBind("bind_lost_perf_hud", "F2", "Toggle the Lost perf HUD",
                        [] { if (g_hud) g_hud->visible = !g_hud->visible.load(); });
}

bool HudVisible() { return g_hud && g_hud->visible.load(); }
void SetHudVisible(bool visible) {
  if (g_hud) g_hud->visible = visible;
}

std::string OverlayText() {
  // Native mode's FPS counter: frames over the last ~0.5 s, refreshed twice a second.
  if (!HudVisible()) return {};
  static uint64_t last_frames = 0;
  static Clock::time_point last_time = Clock::now();
  static std::string text;
  const Clock::time_point now = Clock::now();
  const double elapsed = std::chrono::duration<double>(now - last_time).count();
  if (elapsed >= 0.5 || text.empty()) {
    const uint64_t frames = g_frames.load(std::memory_order_relaxed);
    const double fps = elapsed > 0 ? double(frames - last_frames) / elapsed : 0.0;
    last_frames = frames;
    last_time = now;
    text = fmt::format("{:.0f} FPS  {:.1f} MS", fps, fps > 0 ? 1000.0 / fps : 0.0);
  }
  return text;
}

rex::ui::FrameStats Stats() {
  std::lock_guard<std::mutex> lock(g_mutex);
  rex::ui::FrameStats s;
  s.frame_time_ms = g_last.frame_us / 1000.0;
  s.fps = g_last.frame_us ? 1e6 / g_last.frame_us : 0.0;
  s.frame_count = g_frames;
  return s;
}

}  // namespace lost::perf
