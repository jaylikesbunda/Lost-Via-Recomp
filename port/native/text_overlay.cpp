// Lost native renderer: the FPS readout (text_overlay.h).
#include "text_overlay.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace lost::native {
namespace {

std::mutex g_mutex;
std::function<std::string()> g_provider;

// 5x7 glyphs, one row per byte (bit 4 = leftmost column).
struct Glyph {
  char c;
  uint8_t rows[7];
};
constexpr Glyph kGlyphs[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
};

const uint8_t* GlyphRows(char c) {
  for (const Glyph& g : kGlyphs)
    if (g.c == c) return g.rows;
  return nullptr;  // space and anything unknown
}

}  // namespace

void SetOverlayTextProvider(std::function<std::string()> provider) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_provider = std::move(provider);
}

void DrawTextOverlay(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w,
                     uint32_t h) {
  std::function<std::string()> provider;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    provider = g_provider;
  }
  const std::string text = provider ? provider() : std::string{};
  if (text.empty() || !w || !h) return;
  const LONG cell = LONG(std::max<uint32_t>(2, h / 360));  // 2 px at 720p, 3 at 1080p
  const LONG margin = 4 * cell, pad = 2 * cell;
  const LONG advance = 6 * cell;
  const LONG box_w = pad * 2 + LONG(text.size()) * advance - cell;
  const LONG box_h = pad * 2 + 7 * cell;
  const D3D12_RECT box = {margin, margin, std::min<LONG>(LONG(w), margin + box_w),
                          std::min<LONG>(LONG(h), margin + box_h)};
  const float background[4] = {0.02f, 0.02f, 0.02f, 1.0f};
  list->ClearRenderTargetView(rtv, background, 1, &box);

  // One rectangle per horizontal run of lit cells.
  std::vector<D3D12_RECT> rects;
  rects.reserve(text.size() * 10);
  for (size_t i = 0; i < text.size(); ++i) {
    const uint8_t* rows = GlyphRows(text[i]);
    if (!rows) continue;
    const LONG x0 = margin + pad + LONG(i) * advance, y0 = margin + pad;
    for (LONG row = 0; row < 7; ++row) {
      for (LONG col = 0; col < 5;) {
        if (!(rows[row] & (0x10 >> col))) {
          ++col;
          continue;
        }
        LONG end = col;
        while (end < 5 && (rows[row] & (0x10 >> end))) ++end;
        const D3D12_RECT r = {x0 + col * cell, y0 + row * cell, x0 + end * cell,
                              y0 + (row + 1) * cell};
        if (r.right <= LONG(w) && r.bottom <= LONG(h)) rects.push_back(r);
        col = end;
      }
    }
  }
  if (rects.empty()) return;
  const float foreground[4] = {1.0f, 1.0f, 0.35f, 1.0f};
  // Keep each driver clear small: some D3D12 drivers fail on the large rectangle
  // arrays produced by a whole line of text.
  constexpr size_t kRectsPerClear = 4;
  for (size_t first = 0; first < rects.size(); first += kRectsPerClear) {
    const UINT count = UINT(std::min(kRectsPerClear, rects.size() - first));
    list->ClearRenderTargetView(rtv, foreground, count, rects.data() + first);
  }
}

}  // namespace lost::native
