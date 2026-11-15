// Lost native renderer: the host values the vendored renderer expects the game's port to provide.
//
// GRAW's port supplied these from its frame-timing module (port/src/frame_timing.cpp) to split a
// frame's time into the limiter's wait and the guest's wait for push-buffer ring space. Lost
// measures frames in port/src/perf.cpp instead, which does not attribute those waits yet, so
// they report 0 here and the renderer's CSV columns stay empty until it does.
//
// The vendored renderer declares them at global scope (native/renderer.cpp:47-48).
#include <cstdint>

uint64_t LostTakeLimitWaitNs() { return 0; }
uint64_t LostTakeRingWaitNs() { return 0; }
