// Lost's per-thread allocation statistics have a fixed 32-record freelist.
// Grow only after sub_82178A58 has tried reclaiming exited threads and failed.
// Overflow records follow the same tree/TLS lifecycle as the inline records.
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace {
std::mutex g_records_mutex;
// Guest record -> owning guest statistics object. Storage remains available for
// reuse until process teardown, just like the 32 records embedded in the static
// object.
std::unordered_map<uint32_t, uint32_t> g_overflow;
} // namespace

REX_EXTERN(__imp__sub_82179450);
REX_EXTERN(__imp__sub_82179430);
REX_EXTERN(__imp__sub_82154E58);

REX_HOOK_RAW(sub_82179450) {
  const uint32_t pool = ctx.r3.u32;
  const bool exhausted_retry = ctx.lr == 0x82178B24;
  __imp__sub_82179450(ctx, base);
  if (ctx.r3.u32 || !exhausted_retry)
    return;
  // This call site is specifically the retry of the statistics object's pool at
  // +1664.
  const uint32_t record = rex::system::kernel_memory()->SystemHeapAlloc(44);
  if (!record) {
    REXLOG_ERROR("Lost thread records: overflow allocation failed");
    return;
  }
  std::memset(base + record, 0, 44);
  {
    std::lock_guard<std::mutex> lock(g_records_mutex);
    g_overflow.emplace(record, pool - 1664);
    REXLOG_WARN("Lost thread records: grew beyond 32 inline records ({} "
                "overflow records)",
                g_overflow.size());
  }
  ctx.r3.u64 = record;
}

REX_HOOK_RAW(sub_82154E58) {
  const uint32_t owner = ctx.r3.u32, record = ctx.r4.u32;
  __imp__sub_82154E58(ctx, base);
  // The original removes the record from the tree and aggregates its counters,
  // but returns only inline records to the freelist. Return our overflow
  // records too.
  bool overflow;
  {
    std::lock_guard<std::mutex> lock(g_records_mutex);
    const auto it = g_overflow.find(record);
    overflow = it != g_overflow.end() && it->second == owner;
  }
  if (!overflow)
    return;
  const PPCContext saved = ctx;
  std::memset(base + record, 0, 8);
  ctx.r3.u64 = owner + 1664;
  ctx.r4.u64 = record;
  __imp__sub_82179430(ctx, base);
  ctx = saved;
}
