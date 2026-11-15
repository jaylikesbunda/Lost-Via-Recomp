// Lost native renderer: the game's guest D3D hooks (lost_d3d.h fn::). The dispatch follows
// pgr4-native 8ea20d5's (pgr4_d3d_trace.cpp OnHook, MIT; see native/README.md).
//
// Each hook overrides the generated function (REX_HOOK_RAW) and always runs the original first,
// so the guest device stays as the game expects and the Xenos plugin gets the whole stream.
// While the native renderer is live (--lost_native or --lost_native_primary):
//   - every hooked call reports its entry and exit to the push-buffer tracker (d3d_state.h),
//     which decodes the packets the call wrote; the push-buffer functions (MakeSpace, the
//     segment submit) report the cursor's jump instead;
//   - the outermost draw, clear, resolve or Swap reaches the renderer after the original
//     returned, with its arguments at entry; the clears and resolves inside BeginTiling and
//     EndTiling carry that call (TilingCall);
//   - BeginVertices (and BeginIndexedVertices) only reserve: the D3D publishes their packets
//     and the game its vertices at EndVertices, where their draw is queued.
// With the renderer off, the hooks only call the originals.
//
// Swap is the one exception: the port already hooks it for frame timing (port/src/perf.cpp), so
// that hook calls HookSwap() below instead of the original.
#include "d3d_state.h"
#include "hook_args.h"
#include "renderer.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <array>
#include <cstdint>

REXCVAR_DEFINE_INT32(lost_d3d_stats_frames, 150, "Lost native",
                     "Frames per summary line of the native renderer's statistics (0 = off)");

namespace {

namespace nat = lost::native;
using nat::HookFn;
using nat::TilingCall;
using GuestFn = void (*)(PPCContext&, uint8_t*);

enum NativeClass : uint8_t { kNatDraw, kNatPendingDraw, kNatClear, kNatResolve, kNatSwap };

thread_local uint32_t t_native_depth = 0;  // active draw/clear/resolve/Swap calls
thread_local bool t_pending = false;       // a BeginVertices draw waits for EndVertices
thread_local nat::HookArgs t_pending_args;
thread_local TilingCall t_tiling = TilingCall::kNone;

nat::HookArgs EntryArgs(HookFn fn, const PPCContext& ctx) {
  nat::HookArgs a;
  a.fn = fn;
  a.device = ctx.r3.u32;
  const std::array<uint32_t, 8> r = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                     ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  for (size_t i = 0; i < r.size(); ++i) a.r[i] = r[i];
  a.sp = ctx.r1.u32;
  a.f[0] = ctx.f1.f64;
  a.f[1] = ctx.f2.f64;
  a.f[2] = ctx.f3.f64;
  a.f[3] = ctx.f4.f64;
  a.ret = 0;
  a.tiling = t_tiling;
  return a;
}

// A draw BeginVertices reserved that never reached EndVertices (the game took another path):
// its packets were not published, so it is dropped.
void DropPending() {
  if (!t_pending) return;
  t_pending = false;
  static uint32_t dropped = 0;
  if (++dropped <= 8)
    REXLOG_WARN("Lost native: BeginVertices draw at {:08X} without EndVertices; dropped",
                t_pending_args.device);
}

// The original, with the tracker told about its entry and exit.
template <GuestFn kOriginal>
inline void RunTracked(PPCContext& ctx, uint8_t* base, uint32_t fn) {
  const uint32_t device = ctx.r3.u32;
  nat::GuestStateTracker& gs = nat::GuestState();
  gs.Sync(base, device, fn, t_native_depth == 0);
  kOriginal(ctx, base);
  gs.Sync(base, device, fn);
  gs.AfterCall(base, device);
}

template <HookFn kFn, NativeClass kClass, GuestFn kOriginal>
inline void NativeHook(PPCContext& ctx, uint8_t* base) {
  nat::NativeRenderer* const r = nat::NativeRenderer::Live();
  if (!r) {
    kOriginal(ctx, base);
    return;
  }
  constexpr uint32_t kAddr = uint32_t(kFn);
  const bool outer = t_native_depth++ == 0;
  nat::HookArgs a;
  if (outer) {
    DropPending();
    a = EntryArgs(kFn, ctx);
  }
  {
    const uint32_t device = ctx.r3.u32;
    nat::GuestStateTracker& gs = nat::GuestState();
    gs.Sync(base, device, kAddr, outer);
    if (outer && (kClass == kNatDraw || kClass == kNatPendingDraw)) gs.BeginDrawCall();
    kOriginal(ctx, base);
    gs.Sync(base, device, kAddr);
    gs.AfterCall(base, device);
  }
  --t_native_depth;
  if (!outer) return;
  a.ret = ctx.r3.u64;
  if constexpr (kClass == kNatDraw) {
    r->OnDraw(base, a);
  } else if constexpr (kClass == kNatPendingDraw) {
    // BeginVertices returns the vertex pointer (0: failed); BeginIndexedVertices an HRESULT
    // (0: S_OK), its pointers through out-parameters.
    if (kFn == HookFn::kBeginIndexedVertices ? uint32_t(a.ret) == 0 : a.ret != 0) {
      t_pending_args = a;
      t_pending = true;
    }
  } else if constexpr (kClass == kNatClear) {
    r->OnClear(base, a);
  } else if constexpr (kClass == kNatResolve) {
    r->OnResolve(base, a);
  } else {
    static_assert(kClass == kNatSwap);
    r->OnSwap(base, a);
  }
}

// EndVertices: publishes BeginVertices' packets; the draw is queued once the tracker decoded
// them.
template <GuestFn kOriginal>
inline void EndVerticesHook(PPCContext& ctx, uint8_t* base) {
  nat::NativeRenderer* const r = nat::NativeRenderer::Live();
  if (!r) {
    t_pending = false;
    kOriginal(ctx, base);
    return;
  }
  nat::GuestStateTracker& gs = nat::GuestState();
  const uint32_t device = ctx.r3.u32;
  gs.Sync(base, device, nat::fn::kEndVertices, t_native_depth == 0);
  gs.BeginDrawCall();
  kOriginal(ctx, base);
  gs.Sync(base, device, nat::fn::kEndVertices);
  gs.AfterCall(base, device);
  if (t_pending && t_native_depth == 0) {
    t_pending = false;
    if (t_pending_args.device == device) r->OnDraw(base, t_pending_args);
  }
}

// BeginTiling / EndTiling: the clears and resolves they make carry the call.
template <TilingCall kCall, GuestFn kOriginal>
inline void TilingHook(PPCContext& ctx, uint8_t* base, uint32_t fn) {
  if (!nat::NativeRenderer::Live()) {
    kOriginal(ctx, base);
    return;
  }
  const TilingCall outer = t_tiling;
  t_tiling = kCall;
  RunTracked<kOriginal>(ctx, base, fn);
  t_tiling = outer;
}

// The functions that move the push cursor by assignment: decode up to the old place, then go on
// from the new one. Nested in another one (the new segment inside the segment submit), a jump
// hook does not decode at its entry: the outer call decoded everything the game wrote, and what
// lies between that and the cursor now is the submit's own bookkeeping (pointers, not packets),
// which decoded as packets with absurd lengths.
thread_local uint32_t t_jump_depth = 0;

template <GuestFn kOriginal>
inline void JumpHook(PPCContext& ctx, uint8_t* base, uint32_t fn) {
  if (!nat::NativeRenderer::Live()) {
    kOriginal(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  nat::GuestStateTracker& gs = nat::GuestState();
  if (t_jump_depth++ == 0) gs.Sync(base, device, fn, t_native_depth == 0);
  kOriginal(ctx, base);
  --t_jump_depth;
  gs.Jump(base, device, fn);
}

}  // namespace

#define LOST_NATIVE_HOOK(addr, kfn, cls)                            \
  REX_EXTERN(__imp__sub_##addr);                                    \
  REX_HOOK_RAW(sub_##addr) {                                        \
    NativeHook<HookFn::kfn, cls, __imp__sub_##addr>(ctx, base);     \
  }                                                                 \
  static_assert(uint32_t(HookFn::kfn) == 0x##addr##u, "address")

LOST_NATIVE_HOOK(82391FD8, kDrawIndexedVertices, kNatDraw);
LOST_NATIVE_HOOK(82391BF0, kDrawVertices, kNatDraw);
LOST_NATIVE_HOOK(82391668, kDrawVerticesUP, kNatDraw);
LOST_NATIVE_HOOK(823911C8, kBeginVertices, kNatPendingDraw);
LOST_NATIVE_HOOK(823916B0, kBeginIndexedVertices, kNatPendingDraw);
LOST_NATIVE_HOOK(823A2538, kClear, kNatClear);
LOST_NATIVE_HOOK(823A2410, kClearF, kNatClear);
LOST_NATIVE_HOOK(823989D8, kResolve, kNatResolve);

REX_EXTERN(__imp__sub_82391BE0);
REX_HOOK_RAW(sub_82391BE0) { EndVerticesHook<__imp__sub_82391BE0>(ctx, base); }
static_assert(nat::fn::kEndVertices == 0x82391BE0u);

REX_EXTERN(__imp__sub_823A07A8);
REX_HOOK_RAW(sub_823A07A8) {
  TilingHook<TilingCall::kBegin, __imp__sub_823A07A8>(ctx, base, nat::fn::kBeginTiling);
}
static_assert(nat::fn::kBeginTiling == 0x823A07A8u);

REX_EXTERN(__imp__sub_823A0C40);
REX_HOOK_RAW(sub_823A0C40) {
  TilingHook<TilingCall::kEnd, __imp__sub_823A0C40>(ctx, base, nat::fn::kEndTiling);
}
static_assert(nat::fn::kEndTiling == 0x823A0C40u);

REX_EXTERN(__imp__sub_823A4660);
REX_HOOK_RAW(sub_823A4660) { JumpHook<__imp__sub_823A4660>(ctx, base, nat::fn::kMakeSpace); }
static_assert(nat::fn::kMakeSpace == 0x823A4660u);

REX_EXTERN(__imp__sub_823A47D8);
REX_HOOK_RAW(sub_823A47D8) {
  JumpHook<__imp__sub_823A47D8>(ctx, base, nat::fn::kSubmitSegment);
}
static_assert(nat::fn::kSubmitSegment == 0x823A47D8u);

// Called by port/src/perf.cpp's Swap hook, which owns the one REX_HOOK_RAW on sub_82397D70 (it
// measures the frame there). Runs the original with the tracker around it, then the renderer.
REX_EXTERN(__imp__sub_82397D70);
static_assert(nat::fn::kSwap == 0x82397D70u);

namespace lost::native {

void HookSwap(PPCContext& ctx, uint8_t* base) {
  NativeHook<HookFn::kSwap, kNatSwap, __imp__sub_82397D70>(ctx, base);
}

}  // namespace lost::native
