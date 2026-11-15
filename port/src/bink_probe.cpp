// Lost - Bink video probe (docs/HANDOFF.md, "Bink videos don't show").
//
// The movie player starts two worker threads at sub_8287FCD0 (via XAPI startup). Each loops:
//   while (WaitForSingleObject(ctx->quit_event /*+8*/, ctx->timeout /*+4*/) != WAIT_OBJECT_0) {
//     WaitForSingleObject(ctx->mutex /*+20*/, INFINITE); switch (ctx->state /*+0*/) ...
//   }
// On boot they exit at once, without a single kernel wait in the log. This hook logs the
// context block and the first wait's result. Off unless --lost_debug_bink.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

REXCVAR_DEFINE_BOOL(lost_debug_bink, false, "Lost", "Log the Bink worker threads' context and waits")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return rex::byte_swap(v);
}
}  // namespace

// Frame-plane sampler: the first movie (Logo_Ubisoft_1280x720.bik) allocates its planes with
// MmAllocatePhysicalMemoryEx at fixed addresses on this build (trace, 2026-11-14): Y 0xF8FEF000
// (0xE1000 = 1280x720), two chroma planes 0xF8FAB000 / 0xF8F67000 (0x44000), and 0xF8BCF000
// (0x398000). Every 500 ms log how many bytes are non-zero and a checksum, to tell "never
// decoded" (all zero / unchanging) from "decoded but not shown".
#include <thread>
namespace {
struct Plane { const char* name; uint32_t addr, size; };
constexpr Plane kPlanes[] = {{"Y", 0xF8FEF000u, 0xE1000u}, {"C1", 0xF8FAB000u, 0x44000u},
                             {"C2", 0xF8F67000u, 0x44000u}, {"buf", 0xF8BCF000u, 0x398000u}};
void Sampler() {
  uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::string line;
    for (const Plane& p : kPlanes) {
      const uint8_t* m = base + p.addr;
      uint32_t nonzero = 0;
      uint64_t sum = 0;
      for (uint32_t k = 0; k < p.size; k += 64) { nonzero += m[k] != 0; sum = sum * 31 + m[k]; }
      line += fmt::format(" {}:{}/{} {:016X}", p.name, nonzero, p.size / 64, sum);
    }
    REXLOG_INFO("Bink planes t={}ms{}", (i + 1) * 500, line);
  }
}
}  // namespace
void LostStartBinkSampler() {
  if (REXCVAR_GET(lost_debug_bink)) std::thread(Sampler).detach();
}

// The player's per-frame update (sub_8235FFF8): after BinkWait-like sub_8287E9E0 it calls the
// object's vtable[3] (+12); false => frame-ready byte +28 = 0 and the draw (sub_82360A28) only
// clears to black. Log the object's fields after each update when they change.
REX_EXTERN(__imp__sub_8235FFF8);
REX_HOOK_RAW(sub_8235FFF8) {
  const uint32_t o = ctx.r3.u32;
  __imp__sub_8235FFF8(ctx, base);
  if (!REXCVAR_GET(lost_debug_bink) || !o) return;
  const uint32_t vt = Load32(base, o);
  const uint32_t m3 = vt ? Load32(base, vt + 12) : 0;
  const uint32_t bink = Load32(base, o + 12);
  const uint8_t f28 = base[o + 28], f29 = base[o + 29], f52 = base[o + 52], f57 = base[o + 57],
                f80 = base[o + 80];
  const uint32_t s40 = Load32(base, o + 40), n64 = Load32(base, o + 64), n68 = Load32(base, o + 68);
  static thread_local uint64_t last = ~0ull;
  static thread_local int count = 0;
  const uint64_t key = (uint64_t(f28) << 56) ^ (uint64_t(f29) << 48) ^ (uint64_t(s40) << 40) ^
                       (uint64_t(f80) << 32) ^ n68;
  ++count;
  if (key != last || count % 120 == 0) {
    last = key;
    REXLOG_INFO("Bink update obj={:08X} vt={:08X} vt[3]={:08X} bink={:08X} ret={:08X} +28={} "
                "+29={} +40={} +52={} +57={} +64={} +68={} +80={} (call {})",
                o, vt, m3, bink, ctx.r3.u32, f28, f29, s40, f52, f57, n64, n68, f80, count);
  }
}

// Draw_Bink_textures (sub_823610C8): r3 = device, r4 = texture set (+84 = current index, then
// 32-byte entries of Y/cR/cB/A texture pointers), r5/r6 = width/height. Shaders come from the
// global at 0x82995044 (+0 pixel, +8 vertex). Log them and the Y texture's header the first
// few times.
REX_EXTERN(__imp__sub_823610C8);
REX_HOOK_RAW(sub_823610C8) {
  if (REXCVAR_GET(lost_debug_bink)) {
    static int logged = 0;
    if (logged < 3) {
      ++logged;
      const uint32_t set = ctx.r4.u32;
      const uint32_t idx = Load32(base, set + 84);
      const uint32_t e = set + idx * 32;
      const uint32_t y = Load32(base, e), cr = Load32(base, e + 4), cb = Load32(base, e + 8);
      std::string hdr;
      for (int k = 0; k < 13 && y; ++k) hdr += fmt::format(" {:08X}", Load32(base, y + k * 4));
      REXLOG_INFO("Bink draw dev={:08X} set={:08X} idx={} Y={:08X} cR={:08X} cB={:08X} w={} h={} "
                  "ps={:08X} vs={:08X} | Y hdr:{}",
                  ctx.r3.u32, set, idx, y, cr, cb, ctx.r5.s32, ctx.r6.s32, Load32(base, 0x82995044),
                  Load32(base, 0x8299504C), hdr);
      for (uint32_t obj : {Load32(base, 0x8299504C), Load32(base, 0x82995044)}) {
        std::string d;
        for (int k = 0; k < 24 && obj; ++k) d += fmt::format(" {:08X}", Load32(base, obj + k * 4));
        REXLOG_INFO("  shader obj {:08X}:{}", obj, d);
      }
    }
  }
  __imp__sub_823610C8(ctx, base);
}

// The worker thread entry.
REX_EXTERN(__imp__sub_8287FCD0);
REX_HOOK_RAW(sub_8287FCD0) {
  const uint32_t c = ctx.r3.u32;
  if (REXCVAR_GET(lost_debug_bink) && c) {
    REXLOG_INFO("Bink worker start ctx={:08X}: state={} timeout={:08X} quit={:08X} w3={:08X} "
                "w4={:08X} mutex={:08X} w6={:08X} w7={:08X}",
                c, Load32(base, c + 0), Load32(base, c + 4), Load32(base, c + 8),
                Load32(base, c + 12), Load32(base, c + 16), Load32(base, c + 20),
                Load32(base, c + 24), Load32(base, c + 28));
  }
  __imp__sub_8287FCD0(ctx, base);
  if (REXCVAR_GET(lost_debug_bink)) {
    REXLOG_INFO("Bink worker ctx={:08X} returned {:08X}, state now {}", c, ctx.r3.u32,
                c ? Load32(base, c + 0) : 0);
  }
}

// WaitForSingleObject(handle, ms): log the worker's waits (only on threads that hooked above
// would be ideal; log handle/timeout/result for the quit events seen there instead).
REX_EXTERN(__imp__sub_8237FCB0);
REX_HOOK_RAW(sub_8237FCB0) {
  const uint32_t handle = ctx.r3.u32, ms = ctx.r4.u32;
  __imp__sub_8237FCB0(ctx, base);
  if (REXCVAR_GET(lost_debug_bink) && ms != 0xFFFFFFFFu) {
    static thread_local int logged = 0;
    if (logged < 4) {
      ++logged;
      REXLOG_INFO("WaitForSingleObject({:08X}, {}) -> {:08X}", handle, ms, ctx.r3.u32);
    }
  }
}
