// Lost - Magma UI probe (docs/HANDOFF.md, "Native Options menu"). Off unless --lost_debug_ui.
//
// Every 500 ms, looks at Magma's PageStack singleton (0x8299AC54: +4/+8 vector of 8-byte entries
// {Page*, u8 visible, s8 layer}, +0x10 current index). When the set of pages changes it dumps each
// page and its elements (Area +0x1C: vector<Element*>, begin +0x20 / end +0x24; Element +4 = Id)
// as raw words, following heap pointers one level, so the structures can be read from a log.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <mutex>
#include <set>
#include <tuple>

#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

REXCVAR_DEFINE_STRING(lost_debug_heap_dump, "", "Lost",
                      "Debug: write committed guest memory 0x40000000-0x60000000 to this file "
                      "(records: u32 guest addr, u32 size, bytes) at lost_debug_heap_dump_at s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(lost_debug_heap_dump_at, 35, "Lost", "Seconds after start for lost_debug_heap_dump")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(lost_debug_ui, false, "Lost", "Log Magma UI pages and elements as they open")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

uint8_t* g_base = nullptr;

uint32_t L32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, g_base + a, 4);
  return rex::byte_swap(v);
}

bool IsHeap(uint32_t a) {
  return a >= 0x30000000u && a < 0xC0000000u && !(a >= 0x80000000u && a < 0xA0000000u) && (a & 3) == 0;
}
bool IsImage(uint32_t a) { return a >= 0x82000000u && a < 0x829F0000u; }

std::string Words(uint32_t a, int n) {
  std::string s;
  for (int i = 0; i < n; ++i) s += fmt::format(" {:08X}", L32(a + i * 4));
  return s;
}

std::string Ascii(uint32_t a, int n) {
  std::string s;
  for (int i = 0; i < n; ++i) {
    char c = char(g_base[a + i]);
    s += (c >= 0x20 && c < 0x7F) ? c : '.';
  }
  return s;
}

void DumpObject(const char* what, uint32_t a, int depth) {
  if (!IsHeap(a)) return;
  REXLOG_INFO("UI {} {:08X}:{}  |{}|", what, a, Words(a, 24), Ascii(a, 96));
  if (depth <= 0) return;
  for (int i = 1; i < 24; ++i) {
    uint32_t p = L32(a + i * 4);
    if (IsHeap(p) && p != a) {
      REXLOG_INFO("UI   +{:02X} -> {:08X}:{}  |{}|", i * 4, p, Words(p, 16), Ascii(p, 64));
    }
  }
}

void DumpPage(uint32_t page) {
  DumpObject("page", page, 1);
  uint32_t begin = L32(page + 0x20), end = L32(page + 0x24);
  if (!IsHeap(begin) || end < begin || end - begin > 4 * 512) return;
  REXLOG_INFO("UI page {:08X}: {} elements", page, (end - begin) / 4);
  for (uint32_t e = begin; e < end; e += 4) DumpObject("element", L32(e), 1);
}

void Probe() {
  g_base = rex::system::kernel_memory()->virtual_membase();
  std::vector<uint32_t> last;
  REXLOG_INFO("UI probe started");
  for (int tick = 0;; ++tick) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    uint32_t stack = L32(0x8299AC54);
    if (tick % 10 == 0 && stack) {
      REXLOG_INFO("UI singleton {:08X}:{}", stack, IsHeap(stack) ? Words(stack, 8) : std::string());
    }
    if (!IsHeap(stack)) continue;
    uint32_t begin = L32(stack + 4), end = L32(stack + 8);
    if (!IsHeap(begin) || end < begin || end - begin > 8 * 64) continue;
    std::vector<uint32_t> pages;
    for (uint32_t p = begin; p < end; p += 8) pages.push_back(L32(p));
    if (pages == last) continue;
    last = pages;
    REXLOG_INFO("UI pagestack {:08X}: {} pages, current {}", stack, pages.size(), L32(stack + 0x10));
    for (uint32_t p = begin; p < end; p += 8) {
      REXLOG_INFO("UI  entry page={:08X} vis={} layer={}", L32(p), g_base[p + 4],
                  int(int8_t(g_base[p + 5])));
    }
    // Dump only the newest page (the one just pushed).
    if (!pages.empty()) DumpPage(pages.back());
  }
}

}  // namespace

// Area::FindElement(area r3, Id r4) -> Element* (sub_82556A18, area.cpp): log each distinct
// (area, id, caller) once, with the element found.
REX_EXTERN(__imp__sub_82556A18);
REX_HOOK_RAW(sub_82556A18) {
  const uint32_t area = ctx.r3.u32, id = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_82556A18(ctx, base);
  if (!REXCVAR_GET(lost_debug_ui)) return;
  static std::mutex m;
  static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
  std::lock_guard lock(m);
  if (seen.size() < 4000 && seen.insert({area, id, lr}).second) {
    REXLOG_INFO("UI find area={:08X} id={:08X} lr={:08X} -> {:08X}", area, id, lr, ctx.r3.u32);
  }
}

namespace {
void HeapDump() {
#if defined(_WIN32)
  const std::string path = REXCVAR_GET(lost_debug_heap_dump);
  std::this_thread::sleep_for(std::chrono::seconds(REXCVAR_GET(lost_debug_heap_dump_at)));
  uint8_t* membase = rex::system::kernel_memory()->virtual_membase();
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  uint64_t total = 0;
  for (uint64_t g = 0x40000000; g < 0x60000000;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(membase + g, &mbi, sizeof(mbi))) break;
    uint64_t end = std::min<uint64_t>(uint64_t((uint8_t*)mbi.BaseAddress - membase) + mbi.RegionSize,
                                      0x60000000);
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      uint32_t hdr[2] = {uint32_t(g), uint32_t(end - g)};
      std::fwrite(hdr, 4, 2, f);
      std::fwrite(membase + g, 1, end - g, f);
      total += end - g;
    }
    g = end;
  }
  std::fclose(f);
  REXLOG_INFO("UI heap dump: {} MB to {}", total >> 20, path);
#endif
}
}  // namespace

void LostStartUiProbe() {
  if (REXCVAR_GET(lost_debug_ui)) std::thread(Probe).detach();
  if (!REXCVAR_GET(lost_debug_heap_dump).empty()) std::thread(HeapDump).detach();
}
