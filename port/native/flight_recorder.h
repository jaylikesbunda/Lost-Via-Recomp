// Lost native renderer: the draw flight recorder's request channel.
//
// With --lost_native_glitch_catch, the draw renderer keeps compact per-draw records (shaders,
// pipeline kind, each bound texture's guest address/format/size/sign and descriptor index) for
// the last kFrames frames. When the glitch catcher sees an object flash (a few screen blocks jump
// away and come back within 1-4 frames) it asks for those records around the flash, and the
// renderer writes <prefix>draws-<label>.txt at the end of a frame on its own thread.
#pragma once

#include <cstdint>
#include <string>

namespace lost::native::flight {

constexpr uint32_t kFrames = 720;  // ~6 s at 120 fps: covers an F12 mark (the catcher keeps 450 frames)

// From any thread: dump native frames [from, to] (inclusive) as <prefix>draws-<label>.txt.
void Request(uint64_t from, uint64_t to, uint64_t label);
// On the render thread: takes the pending request once frame `to` is complete (current > to).
bool Take(uint64_t current_frame, uint64_t* from, uint64_t* to, uint64_t* label);

}  // namespace lost::native::flight
