// Lost - the port's settings as rows in the game's own Options page (docs/HANDOFF.md).
//
// The Options page is a Magma "area" loaded from the game data at boot (id 0x9AB9A8D7). Its
// Subtitles row is a right-aligned label (a game text widget) plus a focusable value text ("On").
// When the page has loaded, the game's own element cloner (ElementCloningVisitor, the code Magma
// uses for instancing) copies that row once per setting. Each copy gets a new id, is moved into
// place by shifting the rects in its keyframe states, gets its own label and value strings, and
// is linked into the page's up/down focus chain, so it looks, animates and navigates like the
// game's rows. While a copy has focus, Y / left / right cycle its value (those buttons are hidden
// from the game then); the cvar is set and saved to lost.toml, as with the F1 panel.
//
// Magma layouts used (this build; tools/dis.py, tools/ui_probe in docs/HANDOFF.md):
//   Area/Page     +0x04 id, +0x20/+0x24 vector<Element*>, +0x34 selected focusable
//   Element       +0x00 vtable, +0x04 id, +0x08 widget, +0x0C element state
//   Focusable     Element + 0x1C up id, +0x20 down id, +0x3C up Element*, +0x40 down Element*
//   ElementState  +0x04/+0x08 vector<Keyframe*>; Keyframe +0x04 widget state
//   Text state    (vtable 0x8206A9B8) +0x0C ARGB, +0x20 int16 left, right, top, bottom (720p px)
//   Value state   (vtable 0x8206A8E0) +0x0C ARGB, +0x20 int16 x, y
//   Widget        +0x04 current (animated) state, of the kinds above
//   Game text     (vtable 0x8210A928) std::wstring at +0x14 (data/ptr, +0x10 size, +0x14 cap)
//   Magma text    (vtable 0x8206AE90) std::wstring at +0x10

#include "native_options.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/types.h>

#include "settings.h"

REXCVAR_DEFINE_BOOL(lost_native_options, true, "Lost",
                    "Add the port's settings to the game's own Options page")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82557880);  // Area::AddElement(area, element)
REX_EXTERN(__imp__sub_82564708);  // Magma package load (links resolved on return)
REX_EXTERN(__imp__sub_82380EF0);  // XInputGetState(user, XINPUT_STATE*)
REX_EXTERN(sub_8259EE50);         // ElementCloningVisitor::ElementCloningVisitor(this, area, area)
REX_EXTERN(sub_8259C748);         // ElementCloningVisitor::Clone(this, element) -> Element*
REX_EXTERN(sub_82590C68);         // ElementCloningVisitor::~ElementCloningVisitor(this)
REX_EXTERN(sub_82557880);
REX_EXTERN(sub_82364F68);         // malloc(size)
REX_EXTERN(__imp__sub_82556FA0);  // Area::~Area(area)

namespace {

constexpr uint32_t kOptionsArea = 0x9AB9A8D7;
constexpr uint32_t kSubtitlesLabel = 0x283E9D04;
constexpr uint32_t kSubtitlesValue = 0x6E7D938C;  // focusable text ("On")
constexpr uint32_t kBrightness = 0xAED7DA34;      // focusable slider
constexpr uint32_t kMusic = 0xB8386C25;           // focusable slider
constexpr uint32_t kTextStateVt = 0x8206A9B8;
constexpr uint32_t kValueStateVt = 0x8206A8E0;
constexpr uint32_t kIdBase = 0x10570000;
constexpr int kSubtitlesY = 529;  // top of the Subtitles row
constexpr size_t kValueLen = 7;   // the clone's inline string capacity

struct Choice {
  const char* value;  // as the cvar takes it
  const char16_t* text;
};

struct Row {
  const char16_t* label;
  const char* cvar;
  bool restart;
  int y;  // top of the row, 1280x720 pixels
  std::vector<Choice> choices;
  // filled at injection
  uint32_t label_el = 0, value_el = 0;
  std::u16string shown;
};

// Two rows under Brightness in the Display tile, three under Subtitles. (No V-Sync row: the
// SDK's `vsync` flag is guest vblank emulation, not display sync; the presenter never waits.) Values stay <= 7
// characters (the clone's inline string).
std::array<Row, 5> g_rows = {{
    {u"Frame rate", "lost_target_fps", false, 200,
     {{"30", u"30 fps"}, {"60", u"60 fps"}, {"120", u"120 fps"}, {"144", u"144 fps"},
      {"0", u"Max"}}},
    {u"Resolution", "resolution_scale", true, 240,
     {{"1", u"720p"}, {"2", u"1440p"}, {"3", u"4K"}, {"4", u"5K"}}},
    {u"Anti-aliasing", "swap_post_effect", true, kSubtitlesY + 36,
     {{"none", u"Off"}, {"fxaa", u"FXAA"}, {"fxaa_extreme", u"FXAA HQ"}}},
    {u"Sharpening", "present_effect", true, kSubtitlesY + 72,
     {{"bilinear", u"Off"}, {"cas", u"CAS"}, {"fsr", u"FSR"}}},
    {u"Texture filter", "anisotropic_override", false, kSubtitlesY + 108,
     {{"-1", u"Default"}, {"2", u"2x"}, {"3", u"4x"}, {"4", u"8x"}, {"5", u"16x"}}},
}};
uint32_t g_note_el = 0;  // "Restart to apply", shown while a restart setting is pending
bool g_note_shown = false;
std::vector<uint32_t> g_note_states;  // keyframe states where the note is opaque
constexpr const char16_t* kNote = u"Restart to apply";

// The game ships English, French, German, Spanish and Italian; the console language picks the
// words (user_language: 1 en, 3 de, 4 fr, 5 es, 6 it). Values stay within kValueLen.
struct Translation {
  const char16_t* en;
  const char16_t* fr;
  const char16_t* de;
  const char16_t* es;
  const char16_t* it;
};
constexpr Translation kTranslations[] = {
    {u"Frame rate", u"Fréquence", u"Bildrate", u"Fotogramas", u"Fotogrammi"},
    {u"Resolution", u"Résolution", u"Auflösung", u"Resolución", u"Risoluzione"},
    {u"Anti-aliasing", u"Anticrénelage", u"Kantenglättung", u"Antialiasing",
     u"Antialiasing"},
    {u"Texture filter", u"Filtrage", u"Texturfilter", u"Filtro texturas", u"Filtro texture"},
    {u"Sharpening", u"Netteté", u"Schärfe", u"Nitidez", u"Nitidezza"},
    {u"Restart to apply", u"Redémarrer pour appliquer", u"Neustart erforderlich",
     u"Reiniciar para aplicar", u"Riavvia per applicare"},
    {u"On", u"Oui", u"An", u"Sí", u"Sì"},
    {u"Off", u"Non", u"Aus", u"No", u"No"},
    {u"Default", u"Défaut", u"Normal", u"Normal", u"Normale"},
    {u"Max", u"Max", u"Max", u"Máx.", u"Max"},
};

std::u16string Localize(const char16_t* en) {
  static const int lang = [] {
    switch (std::atoi(rex::cvar::GetFlagByName("user_language").c_str())) {
      case 4: return 1;
      case 3: return 2;
      case 5: return 3;
      case 6: return 4;
      default: return 0;
    }
  }();
  for (const Translation& t : kTranslations) {
    if (std::u16string_view(t.en) == en) {
      const char16_t* all[] = {t.en, t.fr, t.de, t.es, t.it};
      return all[lang];
    }
  }
  return en;
}

uint8_t* g_base = nullptr;
uint32_t g_area = 0;
std::atomic<bool> g_ready{false};
bool g_injected = false;
std::mutex g_mutex;

uint32_t L32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, g_base + a, 4);
  return rex::byte_swap(v);
}
void S32(uint32_t a, uint32_t v) {
  v = rex::byte_swap(v);
  std::memcpy(g_base + a, &v, 4);
}
int16_t L16(uint32_t a) {
  uint16_t v;
  std::memcpy(&v, g_base + a, 2);
  return int16_t(rex::byte_swap(v));
}
void S16(uint32_t a, int16_t v) {
  uint16_t u = rex::byte_swap(uint16_t(v));
  std::memcpy(g_base + a, &u, 2);
}

uint32_t FindElement(uint32_t area, uint32_t id) {
  for (uint32_t p = L32(area + 0x20); p < L32(area + 0x24); p += 4) {
    uint32_t e = L32(p);
    if (e && L32(e + 4) == id) return e;
  }
  return 0;
}

// The game's CRT malloc (sub_82364F68; free is sub_82365030). A string the game will destroy
// must come from its own heap: destroying the page frees the buffers through free().
uint32_t GuestMalloc(PPCContext& ctx, uint8_t* base, uint32_t size) {
  const uint64_t r3 = ctx.r3.u64;
  ctx.r3.u64 = size;
  sub_82364F68(ctx, base);
  const uint32_t p = ctx.r3.u32;
  ctx.r3.u64 = r3;
  return p;
}

// Writes an MSVC std::wstring at `s` (union data/ptr, +0x10 size, +0x14 capacity): inline when it
// fits, else into a buffer from the game's heap (needs ctx; without it the text is cut to fit).
void SetWString(uint32_t s, std::u16string text, PPCContext* ctx = nullptr, uint8_t* base = nullptr) {
  uint32_t cap = L32(s + 0x14);
  if (cap < 7 || cap > 0x1000) cap = 7;  // a never-built string: treat as empty inline
  uint32_t data = cap > 7 ? L32(s) : s;
  if (text.size() > cap) {
    const uint32_t want = (uint32_t(text.size()) + 8) & ~7u;
    const uint32_t p = ctx ? GuestMalloc(*ctx, base, (want + 1) * 2) : 0;
    if (p) {
      cap = want;
      data = p;
      S32(s, data);
    } else {
      text.resize(cap);
    }
  }
  const uint32_t len = uint32_t(text.size());
  S32(s + 0x14, cap);
  for (uint32_t i = 0; i < len; ++i) S16(data + i * 2, int16_t(text[i]));
  S16(data + len * 2, 0);
  S32(s + 0x10, len);
}

// Moves every keyframe state (and the widget's current state) of an element; `grow_left`
// widens a text rect to the left (its text is right-aligned).
void MoveElement(uint32_t e, int dx, int dy, int grow_left = 0) {
  std::vector<uint32_t> states;
  if (const uint32_t es = L32(e + 0x0C)) {
    for (uint32_t p = L32(es + 4); p < L32(es + 8); p += 4) states.push_back(L32(L32(p) + 4));
  }
  states.push_back(L32(L32(e + 8) + 4));
  for (uint32_t st : states) {
    if (!st) continue;
    if (L32(st) == kTextStateVt) {
      S16(st + 0x20, int16_t(L16(st + 0x20) + dx - grow_left));
      S16(st + 0x22, int16_t(L16(st + 0x22) + dx));
      S16(st + 0x24, int16_t(L16(st + 0x24) + dy));
      S16(st + 0x26, int16_t(L16(st + 0x26) + dy));
    } else if (L32(st) == kValueStateVt) {
      S16(st + 0x20, int16_t(L16(st + 0x20) + dx));
      S16(st + 0x22, int16_t(L16(st + 0x22) + dy));
    } else {
      REXLOG_WARN("Lost options: unknown state vtable {:08X} on element {:08X}", L32(st), e);
    }
  }
}

void ShowNote(bool show) {
  for (uint32_t st : g_note_states) {
    S32(st + 0x0C, (L32(st + 0x0C) & 0x00FFFFFFu) | (show ? 0xFF000000u : 0u));
  }
  g_note_shown = show;
}

// Clones `e` into the area with the game's own cloner and adds it under a new id. Runs on the
// thread that loaded the page, with a scratch frame below the current guest stack pointer.
uint32_t CloneElement(PPCContext& ctx, uint8_t* base, uint32_t e, uint32_t new_id) {
  const uint64_t r1 = ctx.r1.u64;
  ctx.r1.u64 = (r1 - 0x200) & ~0xFull;
  const uint32_t visitor = ctx.r1.u32 + 0x100;
  ctx.r3.u64 = visitor;
  ctx.r4.u64 = g_area;
  ctx.r5.u64 = g_area;
  sub_8259EE50(ctx, base);
  ctx.r3.u64 = visitor;
  ctx.r4.u64 = e;
  sub_8259C748(ctx, base);
  const uint32_t clone = ctx.r3.u32;
  ctx.r3.u64 = visitor;
  sub_82590C68(ctx, base);
  if (clone) {
    S32(clone + 4, new_id);
    ctx.r3.u64 = g_area;
    ctx.r4.u64 = clone;
    sub_82557880(ctx, base);
  }
  ctx.r1.u64 = r1;
  return clone;
}

// a.down = b, b.up = a (focusables).
void Link(uint32_t a, uint32_t b) {
  S32(a + 0x20, L32(b + 4));
  S32(a + 0x40, b);
  S32(b + 0x1C, L32(a + 4));
  S32(b + 0x3C, a);
}

void Inject(PPCContext& ctx, uint8_t* base) {
  const uint32_t label = FindElement(g_area, kSubtitlesLabel);
  const uint32_t value = FindElement(g_area, kSubtitlesValue);
  const uint32_t bright = FindElement(g_area, kBrightness);
  const uint32_t music = FindElement(g_area, kMusic);
  if (!label || !value || !bright || !music || L32(bright + 0x40) != music ||
      L32(value + 0x40) != bright) {
    REXLOG_WARN("Lost options: Options page layout not recognised, leaving it alone");
    return;
  }
  uint32_t id = kIdBase;
  for (Row& row : g_rows) {
    row.label_el = CloneElement(ctx, base, label, id++);
    row.value_el = CloneElement(ctx, base, value, id++);
    if (!row.label_el || !row.value_el) {
      REXLOG_ERROR("Lost options: cloning the Subtitles row failed");
      return;
    }
    MoveElement(row.label_el, 0, row.y - kSubtitlesY);
    MoveElement(row.value_el, 0, row.y - kSubtitlesY);
    SetWString(L32(row.label_el + 8) + 0x14, Localize(row.label), &ctx, base);
    SetWString(L32(row.value_el + 8) + 0x10, std::u16string(kValueLen, u' '));
  }
  // Right-aligned above the Back / Accept / Toggle hints.
  g_note_el = CloneElement(ctx, base, label, id++);
  if (g_note_el) {
    MoveElement(g_note_el, 1165 - 572, 470 - kSubtitlesY, 160);
    // Laid out once with its text (a right-aligned text keeps the offset of its first layout);
    // Refresh shows or hides it through the alpha of its keyframe states.
    SetWString(L32(g_note_el + 8) + 0x14, Localize(kNote), &ctx, base);
    if (const uint32_t es = L32(g_note_el + 0x0C)) {
      for (uint32_t p = L32(es + 4); p < L32(es + 8); p += 4) {
        const uint32_t st = L32(L32(p) + 4);
        if (st && (L32(st + 0x0C) >> 24) != 0) g_note_states.push_back(st);
      }
    }
    ShowNote(false);
  }
  // Focus: Brightness, Frame rate, Resolution, Music ... Subtitles, Anti-aliasing, Texture
  // filter, V-Sync, (wrap to) Brightness.
  Link(bright, g_rows[0].value_el);
  Link(g_rows[0].value_el, g_rows[1].value_el);
  Link(g_rows[1].value_el, music);
  Link(value, g_rows[2].value_el);
  for (size_t i = 2; i + 1 < g_rows.size(); ++i) Link(g_rows[i].value_el, g_rows[i + 1].value_el);
  Link(g_rows.back().value_el, bright);
  g_ready = true;
  REXLOG_INFO("Lost options: added {} settings to the Options page", g_rows.size());
}

int CurrentChoice(const Row& row) {
  std::string v = rex::cvar::GetFlagByName(row.cvar);
  if (v == "1" && std::string(row.cvar) == "vsync") v = "true";
  if (v == "0" && std::string(row.cvar) == "vsync") v = "false";
  for (size_t i = 0; i < row.choices.size(); ++i) {
    if (v == row.choices[i].value) return int(i);
  }
  return -1;
}

void Refresh() {
  bool pending = false;
  for (Row& row : g_rows) {
    const int c = CurrentChoice(row);
    std::u16string text = Localize(c >= 0 ? row.choices[c].text : u"Custom");
    // Fixed length: Magma lays a text out once for its length, and a later change of length
    // hangs its renderer (a 4 -> 5 character change froze the game). Values are left-aligned,
    // so trailing spaces don't show.
    text.resize(kValueLen, u' ');
    if (text != row.shown) {
      SetWString(L32(row.value_el + 8) + 0x10, text);
      row.shown = text;
    }
  }
  for (const std::string& name : rex::cvar::GetPendingRestartFlags()) {
    for (const Row& row : g_rows) pending |= row.restart && name == row.cvar;
  }
  if (g_note_el && pending != g_note_shown) ShowNote(pending);
}

// The row whose value text has focus on the visible Options page, or null.
Row* FocusedRow() {
  const uint32_t selected = L32(g_area + 0x34);
  for (Row& row : g_rows) {
    if (selected != row.value_el) continue;
    const uint32_t state = L32(L32(row.value_el + 8) + 4);  // animated: alpha 0 when hidden
    return state && (L32(state + 0x0C) >> 24) >= 0x80 ? &row : nullptr;
  }
  return nullptr;
}

void Step(Row& row, int dir) {
  const int n = int(row.choices.size());
  const int c = CurrentChoice(row);
  const int next = c < 0 ? 0 : (c + dir + n) % n;
  if (rex::cvar::SetFlagByName(row.cvar, row.choices[next].value)) {
    lost::settings::Save();
    REXLOG_INFO("Lost options: {} = {}", row.cvar, row.choices[next].value);
  }
}

}  // namespace

// Area::AddElement: remember the Options page when its Subtitles row is added.
REX_HOOK_RAW(sub_82557880) {
  const uint32_t area = ctx.r3.u32, element = ctx.r4.u32;
  __imp__sub_82557880(ctx, base);
  if (g_area || !REXCVAR_GET(lost_native_options)) return;
  g_base = base;
  if (L32(area + 4) == kOptionsArea && element && L32(element + 4) == kSubtitlesValue) {
    g_area = area;
  }
}

// Area::~Area: the front-end package (with the Options page) is unloaded when a game starts
// and loaded again for the menus. Forget the page and its rows before they are freed; the next
// load of the page adds them again.
REX_HOOK_RAW(sub_82556FA0) {
  if (g_area && ctx.r3.u32 == g_area) {
    std::lock_guard lock(g_mutex);
    g_ready = false;
    g_area = 0;
    g_injected = false;
    for (Row& row : g_rows) {
      row.label_el = row.value_el = 0;
      row.shown.clear();
    }
    g_note_el = 0;
    g_note_states.clear();
    g_note_shown = false;
    REXLOG_INFO("Lost options: Options page unloaded");
  }
  __imp__sub_82556FA0(ctx, base);
}

// After a Magma package has loaded and its links are resolved: add the rows, once.
REX_HOOK_RAW(sub_82564708) {
  __imp__sub_82564708(ctx, base);
  if (!g_area || g_injected) return;
  std::lock_guard lock(g_mutex);
  if (g_injected) return;
  g_injected = true;
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64;
  Inject(ctx, base);
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
}

// The game's pad read (once per frame and user). Keeps the value texts current, and while one of
// the rows has focus turns Y / d-pad / stick left-right into value changes the game doesn't see.
REX_HOOK_RAW(sub_82380EF0) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_82380EF0(ctx, base);
  if (!g_ready || user != 0 || ctx.r3.u32 != 0) return;
  std::lock_guard lock(g_mutex);
  if (!g_ready) return;
  static uint16_t prev_buttons = 0;
  static int prev_stick = 0;
  Refresh();
  Row* row = FocusedRow();
  uint16_t buttons = uint16_t(L16(state + 4));
  const int16_t lx = L16(state + 8);
  const int stick = lx > 16000 ? 1 : lx < -16000 ? -1 : 0;
  if (row) {
    constexpr uint16_t kLeft = 0x0004, kRight = 0x0008, kY = 0x8000;
    const uint16_t pressed = buttons & ~prev_buttons;
    if (pressed & (kRight | kY) || (stick == 1 && prev_stick != 1)) Step(*row, +1);
    if (pressed & kLeft || (stick == -1 && prev_stick != -1)) Step(*row, -1);
    S16(state + 4, int16_t(buttons & ~(kLeft | kRight | kY)));
    S16(state + 8, 0);
  }
  prev_buttons = buttons;
  prev_stick = stick;
}
