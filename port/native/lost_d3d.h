// Lost native renderer: the game's guest Xbox 360 D3D (XDK 2.0.6274). Function addresses, device
// fields and object layouts, from the disassembly; how each was found is in
// docs/NATIVE_RENDERER.md, docs/NATIVE_RENDERER_DEVICE.md and docs/NATIVE_RENDERER_RESOURCES.md.
//
// The reference game (GRAW) is the same engine family with a 2006 XDK: its device's push cursor
// was at +0 and its resource header 16 bytes larger. Lost's device is closer to PGR4's 2007 XDK:
// six dirty masks at +0..+40 and the push cursor at +48. The Xenos register map is the same.
#pragma once

#include <cstdint>

namespace lost::native {

// Guest D3D functions the native path hooks (generated sub_X; r3 = the device unless noted).
namespace fn {
// Draws.
constexpr uint32_t kBeginVertices = 0x823911C8;         // (prim, count, stride) -> vertex space
constexpr uint32_t kEndVertices = 0x82391BE0;           // publishes BeginVertices' cursor
constexpr uint32_t kBeginIndexedVertices = 0x823916B0;  // (prim, base, vertices, indices, ...)
constexpr uint32_t kDrawVerticesUP = 0x82391668;        // (prim, count, data, stride)
constexpr uint32_t kDrawVertices = 0x82391BF0;          // (prim, start vertex, vertex count)
constexpr uint32_t kDrawIndexedVertices = 0x82391FD8;   // (prim, base vertex, start, count)
// Clears, resolves, Swap, tiling.
constexpr uint32_t kClearF = 0x823A2410;  // (flags, rect, color float4, z f1, stencil r8)
constexpr uint32_t kClear = 0x823A2538;   // (count, rects, flags, D3DCOLOR, z f1, stencil r9)
constexpr uint32_t kResolve = 0x823989D8;  // (flags, rect, texture, point, level, slice, color,
                                           //  z f1; stencil and params on the stack)
constexpr uint32_t kSwap = 0x82397D70;     // (front buffer descriptor, params)
constexpr uint32_t kBeginTiling = 0x823A07A8;  // (flags, count, rects, color, z f1, stencil r9)
constexpr uint32_t kEndTiling = 0x823A0C40;    // (flags, rects, texture, color, z f1, stencil r9,
                                               //  params r10)
// Push buffer: the two functions that move the cursor to another place by assignment.
constexpr uint32_t kMakeSpace = 0x823A4660;     // new segment from the allocator (or the reserve)
constexpr uint32_t kSubmitSegment = 0x823A47D8;  // submits the segment, next one 32-byte aligned
// Not hooked: the draw flush is inlined into each draw (there is no shared routine).
constexpr uint32_t kSetScissorRect = 0x823901C0;  // rect r4; kept for docs, not hooked
}  // namespace fn

// Device layout (byte offsets from the guest D3D device pointer).
namespace dev {
constexpr uint32_t kPush = 48;       // last written push-buffer dword
constexpr uint32_t kPushEnd = 52;    // end of the current segment
constexpr uint32_t kPushLimit = 56;  // past it, the next write calls MakeSpace
// Six u64 dirty masks. Masks 0..4 are the state blocks (constants, register blocks, shader
// load); mask 5 (+40) only gates the render-target revalidate, so it is never forced.
constexpr uint32_t kDirtyMasks = 0;
constexpr uint32_t kDirtyMaskCount = 5;
constexpr uint32_t kFetchConstants = 1152;  // 0x4800, 32 x 6 dwords
constexpr uint32_t kVsConstants = 1920;     // 0x4000, 256 x vec4
constexpr uint32_t kPsConstants = 6016;     // 0x4400, 256 x vec4
constexpr uint32_t kBoolLoop = 10112;       // 0x4900, 40 dwords
constexpr uint32_t kFlags = 10940;          // u8: bit0 = draws are deferred into the bin list
constexpr uint32_t kShadow2000 = 10368;     // register shadow blocks, as the draw preamble sends
constexpr uint32_t kShadow2100 = 10444;     //   them (16, 21, 3, 12, 21, 38 and 8 registers)
constexpr uint32_t kShadow2180 = 10528;
constexpr uint32_t kShadow2200 = 10548;
constexpr uint32_t kShadow2280 = 10596;
constexpr uint32_t kShadow2300 = 10680;
constexpr uint32_t kShadow2380 = 10832;
// Not GPU registers: the window scissor SetScissorRect (sub_823901C0) keeps in the two dwords
// right after block 0x2000's 16 flushed ones (x | y << 16, 15-bit signed fields), which
// sub_8238DBA8 writes into the stream as PA_SC_WINDOW_SCISSOR_TL/BR.
constexpr uint32_t kScissorTL = 10436;
constexpr uint32_t kScissorBR = 10440;
constexpr uint32_t kIndexBuffer = 12428;  // guest D3D index buffer object
constexpr uint32_t kColorSurfaces = 12432;  // 4 x guest D3D surface object
constexpr uint32_t kDepthSurface = 12448;
constexpr uint32_t kTileCount = 12740;    // BeginTiling's tiles
constexpr uint32_t kTileRects = 12744;    // 15 x {x1, y1, x2, y2}
constexpr uint32_t kTileOrigins = 12984;  // 15 x the rectangle rounded out to 32 pixels
}  // namespace dev

// Resource objects. Lost's D3D resource header is 28 bytes (GRAW's: 12, PGR4's: 24).
constexpr uint32_t kTextureFetchOffset = 28;  // D3DBaseTexture: GPU fetch constant, 6 dwords
constexpr uint32_t kIndexBufferFormat = 0;    // D3DIndexBuffer: bit 31 = 32-bit indices
constexpr uint32_t kIndexBufferAddress = 24;  // virtual address of the indices

}  // namespace lost::native
