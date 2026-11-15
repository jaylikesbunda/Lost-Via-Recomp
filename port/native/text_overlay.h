// Lost native renderer: a small text readout (the FPS counter) drawn on the back buffer after the
// frame, with rectangle clears only (no pipeline, works in every back buffer format).
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <functional>
#include <string>

namespace lost::native {

// The text to show each frame ("" = nothing). Called on the native render thread.
void SetOverlayTextProvider(std::function<std::string()> provider);

// Draws the provider's text in the top-left corner of the render target `rtv` (w x h pixels),
// which must be in the RENDER_TARGET state. Characters: 0-9 . F P S M and space.
void DrawTextOverlay(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w,
                     uint32_t h);

}  // namespace lost::native
