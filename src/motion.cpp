// Motion: RenderUI lays the column out flat on a scratch canvas as a list of blocks (one per
// section, the settings footer, or the whole settings panel); Compose moves them onto the
// window. Blocks are stacked from the alignment edge by their displayed heights, and each
// displayed height follows the real one on a critically damped spring, so a section that
// fills in pushes its neighbours aside instead of making them jump. New content fades in
// through an ordered dither: on launch (or switching to or from settings) the blocks cascade
// in from the top with a small rise; a block whose content changes kind (LOADING to data, a
// new track) dithers its body in again. Content that only moved never re-fades.
#include "pixelbar.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {

constexpr double kOmega = 34;   // spring rate, 1/s: a few lines settle in ~150 ms, no overshoot
constexpr int kFadeMs = 200;    // one block's dither fade
constexpr int kCascadeMs = 240; // launch: the last block starts fading this long after the first
constexpr int kRise = 5;        // launch: px a block rises while it fades in

// Order in which an 8x8 tile of art pixels lights up (Bayer), so a fade is an even dissolve.
constexpr uint8_t kBayer[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},    {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21}};

struct State {
  double h = 0, v = 0;   // displayed height, gap included, and its rate (px/s)
  int64_t fadeAt = 0;    // when the current fade starts (may be ahead, in a cascade); 0 = none
  int fadeFrom = 0;      // rows above this (the header) skip the fade
  bool rise = false;
  size_t mode = 0;
};
std::unordered_map<std::string, State> g_state;
int64_t g_last = 0;

double EaseOut(double t) { return 1 - (1 - t) * (1 - t); }

// Exact step of a critically damped spring toward `target`: stable for any dt, never overshoots
// from rest, and keeps its velocity when the target moves mid-flight.
void Spring(State& s, double target, double dt) {
  double x = s.h - target, c = s.v + kOmega * x, e = std::exp(-kOmega * dt);
  s.h = target + (x + c * dt) * e;
  s.v = (s.v - kOmega * c * dt) * e;
  if (std::abs(s.h - target) < 0.4 && std::abs(s.v) < 8) {
    s.h = target;
    s.v = 0;
  }
}

}  // namespace

void Compose(const Canvas& src, Canvas& dst, UiOutput& out, int64_t now, bool still) {
  double dt = g_last ? std::clamp((now - g_last) / 1000.0, 0.0, 0.034) : 0;  // a stall never teleports
  g_last = now;
  int n = (int)out.blocks.size();

  // A scene change (launch, or to or from settings) shares no block with the last frame: the
  // new blocks cascade in at full size rather than each growing from nothing.
  bool scene = true;
  for (const Block& b : out.blocks)
    if (g_state.count(b.key)) scene = false;
  if (scene) g_state.clear();
  std::unordered_map<std::string, State> next;
  int stagger = n > 1 ? std::min(28, kCascadeMs / (n - 1)) : 0;

  std::vector<State*> st(n);
  for (int i = 0; i < n; i++) {
    const Block& b = out.blocks[i];
    double target = b.y1 - b.y0 + b.gap;
    auto old = g_state.find(b.key);
    State& s = next[b.key];
    if (old != g_state.end()) {
      s = old->second;
      if (b.mode != s.mode && (!s.fadeAt || now - s.fadeAt >= kFadeMs)) {  // new content, not mid-fade
        s.fadeAt = now;
        s.fadeFrom = b.head;
        s.rise = false;
      }
    } else if (scene) {
      s.h = target;
      s.fadeAt = now + (int64_t)i * stagger;
      s.rise = true;
    } else {  // joins the column: grows from nothing as it fades in
      s.fadeAt = now;
    }
    s.mode = b.mode;
    if (still) {
      s.h = target;
      s.v = 0;
      s.fadeAt = 0;
    } else {
      Spring(s, target, dt);
    }
    if (s.fadeAt && now - s.fadeAt >= kFadeMs) s.fadeAt = 0;
    out.motion |= s.fadeAt != 0 || s.h != target;
  }
  g_state = std::move(next);  // blocks that left the column are forgotten
  for (int i = 0; i < n; i++) st[i] = &g_state[out.blocks[i].key];

  double total = 0;
  for (State* s : st) total += s->h;
  double top = g_cfg.valign == 2 ? dst.h - total : g_cfg.valign == 1 ? (dst.h - total) / 2 : 0;

  dst.Clear(dst.h);
  const int s = dst.s, pw = dst.pw;
  double at = top;
  for (int i = 0; i < n; i++) {
    const Block& b = out.blocks[i];
    const State& m = *st[i];
    int slot = (int)std::lround(at), slotEnd = (int)std::lround(at + m.h);
    at += m.h;
    int rows = std::clamp(slotEnd - slot - b.gap, 0, b.y1 - b.y0);  // a growing block shows its top first
    double f = m.fadeAt ? std::clamp((now - m.fadeAt) / (double)kFadeMs, 0.0, 1.0) : 1.0;
    int rise = m.rise ? (int)std::lround((1 - EaseOut(f)) * kRise) : 0;
    int y = slot + b.gap + rise, shift = y - b.y0;

    for (size_t h = b.hit0; h < b.hit1 && h < out.hits.size(); h++) {
      RECT& r = out.hits[h].r;
      r.top += shift;
      r.bottom = std::min<LONG>(r.bottom + shift, y + rows);  // nothing clickable that isn't shown
      if (r.bottom <= r.top) r = RECT{};
    }
    if (b.key == "media") {
      OffsetRect(&out.seekBar, 0, shift);
      OffsetRect(&out.volBar, 0, shift);
    }

    // Pixels whose Bayer rank is below `lit` show; the 16 most recently lit show at half
    // strength. `lit` runs to 80, so that soft edge has cleared by the end of the fade.
    int lit = (int)(EaseOut(f) * 80);
    for (int r = 0; r < rows; r++) {
      int dy = y + r;
      if (dy < 0 || dy >= dst.h) continue;
      bool plain = !m.fadeAt || r < m.fadeFrom;
      const uint8_t* rank = kBayer[r & 7];
      for (int k = 0; k < s; k++) {
        const uint32_t* from = src.px + ((size_t)(b.y0 + r) * s + k) * pw;
        uint32_t* to = dst.px + ((size_t)dy * s + k) * pw;
        if (plain) {
          memcpy(to, from, (size_t)pw * 4);
          continue;
        }
        for (int x = 0; x < pw; x++) {
          uint32_t p = from[x];
          int t = rank[(x / s) & 7];
          if (!p || t >= lit) continue;
          to[x] = t >= lit - 16 ? (p >> 1) & 0x7F7F7F7Fu : p;
        }
      }
    }
  }
  out.top = std::clamp((int)std::lround(top), 0, dst.h);
  out.height = std::clamp((int)std::lround(top + total), out.top, dst.h) - out.top;
  // A near-invisible backing so clicks between pixels still land on the column.
  dst.Backfill(out.top, out.top + out.height, {0, (uint8_t)g_cfg.background});
}
