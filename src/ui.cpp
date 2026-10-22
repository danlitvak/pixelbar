// Layout and drawing of the column. Coordinates are art pixels (1 px at 100% scaling).
// Text is the 5x7 pixel font at 1x (7 px caps, 10 px lines); the clock and the current
// temperature are dot-matrix renderings of the same glyphs.
#include "pixelbar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr Ink kHi{255, 255};
constexpr Ink kBlack{0, 255};
Ink Mid() { return {255, (uint8_t)g_cfg.dim}; }
Ink Lo() { return {255, (uint8_t)g_cfg.faint}; }
Ink Alpha(int a) { return {255, (uint8_t)std::clamp(a, 0, 255)}; }

constexpr int kGap = 10;  // between sections
constexpr int kArt = 56;  // album art, px

// The lowest a section may reach this frame (the settings footer sits below). Lists give up
// rows rather than run past it.
int g_bottom = 1 << 30;
bool Room(int y) { return y + kCap <= g_bottom; }
int g_head = -1;  // where the current section's header ends, for its Block

const char* const kIconPlay = "#......|###....|#####..|#######|#####..|###....|#......";
const char* const kIconPause = "##.##|##.##|##.##|##.##|##.##|##.##|##.##";
const char* const kIconNext = "#....##|##...##|###..##|####.##|###..##|##...##|#....##";
const char* const kIconPrev = "##....#|##...##|##..###|##.####|##..###|##...##|##....#";
const char* const kIconCheck = "......#|.....##|#...##.|##.##..|.###...|..#....";
const char* const kIconStop = "#####|#...#|#...#|#...#|#####";
const char* const kIconQuiet = "#.#.#|.....|#...#|.....|#.#.#";
const char* const kIconAsk = "##.|..#|.#.|...|.#.";
const char* const kIconNote = "..#####|..#...#|..#...#|..#...#|.##..##|###.###|.#...#.";

// Weather, 11x9.
const char* const kWxSun =
    ".....#.....|.#.......#.|....###....|...#####...|##.#####.##|...#####...|....###....|.#.......#.|.....#.....";
const char* const kWxMoon =
    "...####....|.###.......|.##........|##.........|##.........|##.........|.##........|.###.......|...####....";
const char* const kWxCloud =
    "...........|...........|....###....|..##...#...|.#......##.|#.........#|#.........#|.#########.|...........";
const char* const kWxPartly =
    ".#..#......|..##.......|.#..####...|...#....#..|..#......##|.#........#|.#........#|..########.|...........";
const char* const kWxRain =
    "....###....|..##...#...|.#......##.|#.........#|#.........#|.#########.|.#...#...#.|#...#...#..|...........";
const char* const kWxHeavy =
    "....###....|..##...#...|.#......##.|#.........#|#.........#|.#########.|.#..#..#..#|#..#..#..#.|.#..#..#...";
const char* const kWxDrizzle =
    "....###....|..##...#...|.#......##.|#.........#|#.........#|.#########.|..#.....#..|...........|.....#.....";
const char* const kWxSnow =
    "....###....|..##...#...|.#......##.|#.........#|#.........#|.#########.|.#...#...#.|...#...#...|.#...#...#.";
const char* const kWxThunder =
    "....###....|..##...#...|.#......##.|#.........#|#.........#|.#########.|.....##....|....##.....|.....#.....";
const char* const kWxFog =
    "...........|###.######.|...........|.######.###|...........|####.######|...........|.#####.####|...........";
const char* const kLedDegree = ".#.|#.#|.#.";

struct Wx {
  const char* icon;
  const char* text;
};

Wx Weather(int code, bool day) {
  if (code <= 1) return {day ? kWxSun : kWxMoon, code == 0 ? "Clear" : "Mostly clear"};
  if (code == 2) return {day ? kWxPartly : kWxCloud, "Partly cloudy"};
  if (code == 3) return {kWxCloud, "Overcast"};
  if (code == 45 || code == 48) return {kWxFog, "Fog"};
  if (code >= 51 && code <= 57) return {kWxDrizzle, "Drizzle"};
  if (code == 61 || code == 80) return {kWxRain, "Light rain"};
  if (code == 63 || code == 81) return {kWxRain, "Rain"};
  if (code == 65 || code == 82) return {kWxHeavy, "Heavy rain"};
  if (code == 66 || code == 67) return {kWxRain, "Freezing rain"};
  if (code >= 71 && code <= 77) return {kWxSnow, "Snow"};
  if (code == 85 || code == 86) return {kWxSnow, "Snow showers"};
  if (code >= 95) return {kWxThunder, "Thunderstorm"};
  return {kWxCloud, "Cloudy"};
}

double Ease(double t) { return t * t * (3 - 2 * t); }

int TextR(Canvas& c, int right, int y, std::string_view s, FontId f, Ink ink) {
  int w = TextWidth(s, f);
  c.Text(right - w, y, s, f, ink);
  return right - w;
}

// Label, a dotted rule, and an optional note at the right edge.
int Header(Canvas& c, int y, std::string_view label, std::string_view right = {}, int reserve = 0) {
  int x = c.Text(0, y, label, FontId::Caps, Mid()) + 6;
  int end = c.w - reserve;
  if (!right.empty()) end = TextR(c, end, y, right, FontId::Caps, Lo()) - 6;
  x += x & 1;
  c.Dotted(x, y + 3, end - x, Lo());
  if (g_head < 0) g_head = y + kCap + 6;
  return y + kCap + 6;
}

std::string Clock2(int64_t ms) {
  long long s = (long long)std::max<int64_t>(ms, 0) / 1000;
  char b[24];
  if (s >= 3600)
    snprintf(b, sizeof b, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
  else
    snprintf(b, sizeof b, "%lld:%02lld", s / 60, s % 60);
  return b;
}

std::string HourMin(int64_t unixMs) {
  SYSTEMTIME t = LocalTime(unixMs);
  char b[16];
  if (g_cfg.clock24h)
    snprintf(b, sizeof b, "%02d:%02d", t.wHour, t.wMinute);
  else
    snprintf(b, sizeof b, "%d:%02d%s", t.wHour % 12 ? t.wHour % 12 : 12, t.wMinute, t.wHour < 12 ? "a" : "p");
  return b;
}

std::string Rate(double bps) {
  char b[32];
  if (bps < 1000)
    snprintf(b, sizeof b, "%.0f B/s", bps);
  else if (bps < 1000 * 1024)
    snprintf(b, sizeof b, bps < 100 * 1024 ? "%.1f KB/s" : "%.0f KB/s", bps / 1024);
  else if (bps < 1000.0 * 1024 * 1024)
    snprintf(b, sizeof b, bps < 100.0 * 1024 * 1024 ? "%.1f MB/s" : "%.0f MB/s", bps / (1024 * 1024));
  else
    snprintf(b, sizeof b, "%.2f GB/s", bps / (1024.0 * 1024 * 1024));
  return b;
}

std::string Pct(double frac) { return std::to_string((int)std::lround(std::clamp(frac, 0.0, 1.0) * 100)) + "%"; }

std::string Degrees(float t) { return std::to_string((int)std::lround(t)) + "\xC2\xB0"; }

// n equal cells spread over exactly w px, the gaps absorbing the remainder.
void Cells(Canvas& c, int x, int y, int w, int h, int n, int gap, double litFrac) {
  int lit = (int)std::lround(std::clamp(litFrac, 0.0, 1.0) * n);
  for (int i = 0; i < n; i++) {
    int x0 = x + (int)((int64_t)i * (w + gap) / n), x1 = x + (int)((int64_t)(i + 1) * (w + gap) / n) - gap;
    c.Fill(x0, y, x1 - x0, h, i < lit ? kHi : Lo());
  }
}

// Label, a detail in a fixed column, a dotted leader and a value at the right edge.
int Metric(Canvas& c, int y, std::string_view label, std::string_view detail, std::string_view value,
           Ink valueInk = kHi) {
  c.Text(0, y, label, FontId::Caps, Mid());
  int vx = value.empty() ? c.w : TextR(c, c.w, y, value, FontId::Text, valueInk);
  int x = 30;
  if (!detail.empty()) x += c.Text(x, y, FitText(detail, FontId::Text, vx - x - 8), FontId::Text, Lo()) + 4;
  x += x & 1;
  if (!value.empty()) c.Dotted(x, y + 6, vx - 4 - x, Lo());
  return y + kLine;
}

// Filled area under a continuous line; newest sample on the right.
void Graph(Canvas& c, int x, int y, int w, int h, const std::vector<float>& hist) {
  int n = (int)hist.size(), prevTop = -1;
  for (int i = 0; i < w; i++) {
    int cx = x + w - 1 - i;
    if (i >= n) {
      if (cx % 2 == 0) c.Pixel(cx, y + h - 1, Alpha(g_cfg.faint / 2));
      continue;
    }
    int colH = std::max(1, (int)std::lround(std::clamp(hist[n - 1 - i], 0.f, 1.f) * h));
    int top = y + h - colH;
    c.Fill(cx, top, 1, colH, Alpha(g_cfg.faint * 2 / 3));
    int a = prevTop < 0 ? top : std::min(top, prevTop), b = prevTop < 0 ? top : std::max(top, prevTop);
    c.Fill(cx, a, 1, b - a + 1, kHi);
    prevTop = top;
  }
}

float WindowMax(const std::vector<float>& h, int w, float floor) {
  float m = floor;
  for (int i = 0; i < w && i < (int)h.size(); i++) m = std::max(m, h[h.size() - 1 - i]);
  return m;
}

int LinkLine(Canvas& c, int y, std::string_view text, const UiInput& in, UiOutput& out) {
  bool hover = in.hoverId == HIT_OPEN_SETTINGS;
  int w = c.Text(0, y, text, FontId::Caps, hover ? kHi : Lo());
  out.hits.push_back({{0, y - 2, w, y + kCap + 2}, HIT_OPEN_SETTINGS, 0});
  return y + kLine;
}

// ---- clock -------------------------------------------------------------------

int IsoWeek(const SYSTEMTIME& t) {
  auto days = [](int y, int m, int d) {  // days since 1970-01-01
    int64_t yy = y - (m <= 2);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400, yoe = yy - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  };
  int wd = t.wDayOfWeek == 0 ? 7 : t.wDayOfWeek;
  int64_t thursday = days(t.wYear, t.wMonth, t.wDay) + 4 - wd;
  int year = t.wYear;
  if (thursday < days(year, 1, 1)) year--;
  else if (thursday >= days(year + 1, 1, 1)) year++;
  return (int)((thursday - days(year, 1, 1)) / 7 + 1);
}

int Clock(Canvas& c, int y, int64_t now) {
  SYSTEMTIME t = LocalTime(now);
  char hm[16];
  if (g_cfg.clock24h)
    snprintf(hm, sizeof hm, "%02d:%02d", t.wHour, t.wMinute);
  else
    snprintf(hm, sizeof hm, "%d:%02d", t.wHour % 12 ? t.wHour % 12 : 12, t.wMinute);
  int pitch = std::clamp(g_cfg.clockScale, 3, 6), h = 7 * pitch - 1;
  int x = c.Led(0, y, hm, pitch, kHi, Alpha(16)) + pitch + 3;
  if (!g_cfg.clock24h) c.Text(x, y, t.wHour < 12 ? "AM" : "PM", FontId::Caps, Mid());
  if (g_cfg.clockSeconds) {
    char ss[4];
    snprintf(ss, sizeof ss, "%02d", t.wSecond);
    c.Text(x, y + h - kCap, ss, FontId::Text, Lo());
  }
  static const char* kDays[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
  static const char* kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  char date[24], week[24];
  snprintf(date, sizeof date, "%d %s %d", t.wDay, kMonths[(t.wMonth + 11) % 12], t.wYear);
  snprintf(week, sizeof week, "WEEK %d", IsoWeek(t));
  const char* lines[3] = {kDays[t.wDayOfWeek % 7], date, week};
  Ink inks[3] = {Mid(), kHi, Lo()};
  for (int i = 2, ly = y + h - kCap; i >= 0 && ly >= y; i--, ly -= kLine)
    TextR(c, c.w, ly, lines[i], FontId::Caps, inks[i]);
  return y + h;
}

// ---- media -------------------------------------------------------------------

void Marquee(Canvas& c, int x, int y, int w, const std::string& text, Ink ink, int64_t now, UiOutput& out) {
  int tw = TextWidth(text, FontId::Text);
  if (tw <= w) {
    c.Text(x, y, text, FontId::Text, ink);
    return;
  }
  // Rest, ease across, rest, ease back: the title reads in full without a constant crawl.
  int dist = tw - w;
  double travel = dist / 24.0 * 1000 + 700, hold0 = 2600, hold1 = 1600;
  double cycle = hold0 + travel + hold1 + travel;
  double t = std::fmod((double)now, cycle), p;
  if (t < hold0)
    p = 0;
  else if (t < hold0 + travel)
    p = Ease((t - hold0) / travel);
  else if (t < hold0 + travel + hold1)
    p = 1;
  else
    p = 1 - Ease((t - hold0 - travel - hold1) / travel);
  c.SetClip(x, y - 1, w, kLine + 1);
  c.Text(x - (int)std::lround(p * dist), y, text, FontId::Text, ink);
  c.ResetClip();
  out.animating = true;
}

// Four bars that follow the real output level, each with its own wobble.
void EqBars(Canvas& c, int x, int y, int h, float peak, int64_t now) {
  static float level[4] = {};
  static const double kFreq[4] = {0.0071, 0.0113, 0.0089, 0.0131};
  float drive = std::pow(std::clamp(peak, 0.f, 1.f), 0.6f);
  for (int i = 0; i < 4; i++) {
    float wobble = 0.55f + 0.45f * (float)std::fabs(std::sin(now * kFreq[i] + i * 1.7));
    float target = drive * wobble;
    level[i] += (target - level[i]) * (target > level[i] ? 0.65f : 0.18f);
    int bh = std::clamp((int)std::lround(level[i] * h), 1, h);
    c.Fill(x + i * 3, y + h - bh, 2, bh, kHi);
  }
}

void Button(Canvas& c, int x, int y, const char* icon, int id, bool enabled, const UiInput& in, UiOutput& out) {
  const Glyph& g = IconGlyph(icon);
  constexpr int bw = 16, bh = 11;
  if (enabled && in.hoverId == id) c.Fill(x, y, bw, bh, Alpha(40));
  c.Icon(x + (bw - g.w) / 2, y + (bh - g.h) / 2, icon, enabled ? kHi : Lo());
  if (enabled) out.hits.push_back({{x, y, x + bw, y + bh}, id, 0});
}

void Volume(Canvas& c, int y, const AudioState& au, const UiInput& in, UiOutput& out) {
  if (!au.ok) return;
  constexpr int kSegs = 10, kSegW = 3;
  int barW = kSegs * (kSegW + 1) - 1, bx = c.w - barW;
  std::string label = au.muted ? "MUTE" : std::to_string((int)std::lround(au.volume * 100));
  int lx = TextR(c, bx - 5, y + 2, label, FontId::Caps, in.hoverId == HIT_MUTE ? kHi : Lo());
  out.hits.push_back({{lx - 3, y, bx - 2, y + 11}, HIT_MUTE, 0});
  int lit = (int)std::lround(au.volume * kSegs);
  bool hover = in.hoverId == HIT_VOL;
  for (int i = 0; i < kSegs; i++) {
    int h = 2 + i * 7 / (kSegs - 1);  // a wedge, quiet to loud
    bool on = i < lit && !au.muted;
    c.Fill(bx + i * (kSegW + 1), y + 9 - h, kSegW, h, on ? kHi : (hover ? Mid() : Lo()));
  }
  out.volBar = {bx, y, bx + barW, y + 11};
  out.hits.push_back({out.volBar, HIT_VOL, 0});
}

int Media(Canvas& c, int y, const UiInput& in, UiOutput& out) {
  const MediaState& m = *in.media;
  const AudioState& au = *in.audio;
  std::string src = m.present ? m.app + (m.playing ? "" : "  PAUSED") : "";
  y = Header(c, y, "MEDIA", src, m.playing ? 16 : 0);
  if (m.playing) {
    EqBars(c, c.w - 11, y - kCap - 6, kCap, au.peak, in.now);
    out.animating = true;
  }
  int top = y;
  if (m.present && m.artSize == kArt && m.art.size() == (size_t)kArt * kArt) {
    for (int yy = 0; yy < kArt; yy++)
      for (int xx = 0; xx < kArt; xx++)
        if (m.art[(size_t)yy * kArt + xx]) c.Pixel(xx, y + yy, kHi);
  } else {
    c.Frame(0, y, kArt, kArt, Lo());
    const Glyph& g = IconGlyph(kIconNote);
    c.Icon((kArt - g.w) / 2, y + (kArt - g.h) / 2, kIconNote, m.present ? Mid() : Lo());
  }
  int tx = kArt + 8, tw = c.w - tx;
  if (m.present) {
    Marquee(c, tx, y, tw, m.title.empty() ? std::string("Unknown title") : m.title, kHi, in.now, out);
    c.Text(tx, y + 11, FitText(m.artist, FontId::Text, tw), FontId::Text, Mid());
    if (!m.album.empty() && m.album != m.title)
      c.Text(tx, y + 22, FitText(m.album, FontId::Text, tw), FontId::Text, Lo());
  } else {
    c.Text(tx, y, "Nothing playing", FontId::Text, Mid());
  }
  out.hits.push_back({{0, top, c.w, top + kArt}, HIT_MUSIC, 0});

  int64_t dur = m.durMs, pos = m.posMs;
  if (m.playing && m.posStampMs > 0) pos += in.now - m.posStampMs;
  pos = std::clamp<int64_t>(pos, 0, std::max<int64_t>(dur, 0));
  int py = y + 33, lw = TextWidth("00:00", FontId::Text);
  int bx = tx + lw + 5, bw = c.w - bx - lw - 5;
  if (m.present && dur > 0) {
    c.Text(tx, py, Clock2(pos), FontId::Text, Mid());
    TextR(c, c.w, py, Clock2(dur), FontId::Text, Mid());
    int fill = (int)((int64_t)bw * pos / dur);
    c.Fill(bx, py + 3, bw, 1, in.hoverId == HIT_SEEK ? Mid() : Lo());
    c.Fill(bx, py + 3, fill, 1, kHi);
    c.Fill(bx + std::min(fill, bw - 1), py, 1, kCap, kHi);
    out.seekBar = {bx, py - 3, bx + bw, py + kCap + 3};
    if (m.canSeek) out.hits.push_back({out.seekBar, HIT_SEEK, 0});
  } else {
    c.Dotted(bx, py + 3, bw, Lo());
  }

  int cy = y + kArt - 11;
  Button(c, tx - 4, cy, kIconPrev, HIT_PREV, m.canPrev, in, out);
  Button(c, tx + 14, cy, m.playing ? kIconPause : kIconPlay, HIT_PLAY, m.present && m.canPlayPause, in, out);
  Button(c, tx + 32, cy, kIconNext, HIT_NEXT, m.canNext, in, out);
  Volume(c, cy, au, in, out);
  return y + kArt;
}

// ---- agents ------------------------------------------------------------------

const char* StateWord(AgentState s) {
  switch (s) {
    case AgentState::NeedsYou: return "NEEDS YOU";
    case AgentState::Asks: return "ASKS";
    case AgentState::Working: return "WORKING";
    case AgentState::Stale: return "QUIET";
    case AgentState::Done: return "DONE";
    case AgentState::Stopped: return "STOPPED";
  }
  return "";
}

void StateIcon(Canvas& c, int x, int y, AgentState s, int64_t now, UiOutput& out) {
  switch (s) {
    case AgentState::Working: {
      // A short tail chasing round a 5x5 ring.
      static const int8_t kRing[16][2] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {4, 1}, {4, 2}, {4, 3},
                                          {4, 4}, {3, 4}, {2, 4}, {1, 4}, {0, 4}, {0, 3}, {0, 2}, {0, 1}};
      static const uint8_t kTail[5] = {255, 190, 130, 85, 50};
      int head = (int)((now / 70) % 16);
      for (auto& p : kRing) c.Pixel(x + 1 + p[0], y + 1 + p[1], Alpha(24));
      for (int i = 0; i < 5; i++) {
        auto& p = kRing[(head - i + 16) % 16];
        c.Pixel(x + 1 + p[0], y + 1 + p[1], Alpha(kTail[i]));
      }
      out.animating = true;
      break;
    }
    case AgentState::NeedsYou:
    case AgentState::Asks:
      c.Fill(x, y, 7, 7, (now / 600) % 2 ? Mid() : kHi);
      if (s == AgentState::NeedsYou) {
        c.Fill(x + 3, y + 1, 1, 3, kBlack);
        c.Pixel(x + 3, y + 5, kBlack);
      } else {
        c.Icon(x + 2, y + 1, kIconAsk, kBlack);
      }
      out.animating = true;
      break;
    case AgentState::Done: c.Icon(x, y, kIconCheck, Mid()); break;
    case AgentState::Stopped: c.Icon(x + 1, y + 1, kIconStop, Lo()); break;
    case AgentState::Stale: c.Icon(x + 1, y + 1, kIconQuiet, Lo()); break;
  }
}

int Agents(Canvas& c, int y, const UiInput& in, UiOutput& out) {
  const auto& list = *in.agents;
  int waiting = 0, working = 0;
  for (auto& a : list) {
    waiting += a.state == AgentState::NeedsYou || a.state == AgentState::Asks;
    working += a.state == AgentState::Working;
  }
  std::string summary;
  if (waiting) summary = std::to_string(waiting) + " WAITING";
  if (working) summary += (summary.empty() ? "" : "  ") + std::to_string(working) + " WORKING";
  y = Header(c, y, "AGENTS", summary);
  if (list.empty()) {
    c.Text(0, y, "NO ACTIVE SESSIONS", FontId::Caps, Lo());
    return y + kCap;
  }
  int n = std::min((int)list.size(), g_cfg.agentMax);
  constexpr int tx = 11;
  for (int i = 0; i < n; i++) {
    const Agent& a = list[i];
    bool live = a.state == AgentState::NeedsYou || a.state == AgentState::Asks || a.state == AgentState::Working;
    bool urgent = a.state == AgentState::NeedsYou || a.state == AgentState::Asks;
    if (!a.link.empty() && in.hoverId == HIT_AGENT && in.hoverIndex == i) c.Fill(0, y - 2, c.w, 21, Alpha(22));
    StateIcon(c, 0, y, a.state, in.now, out);
    int rx = TextR(c, c.w, y, FormatAge(in.now - a.sinceMs), FontId::Caps, Lo());
    rx = TextR(c, rx - 5, y, StateWord(a.state), FontId::Caps, urgent ? kHi : Mid());
    c.Text(tx, y, FitText(a.title, FontId::Text, rx - tx - 8), FontId::Text, live ? kHi : Mid());
    int x = tx + c.Text(tx, y + kLine, a.codex ? "CODEX" : "CLAUDE", FontId::Caps, Lo()) + 5;
    if (!a.detail.empty())
      c.Text(x, y + kLine, FitText(a.detail, FontId::Text, c.w - x), FontId::Text, urgent ? kHi : Mid());
    if (!a.link.empty()) out.hits.push_back({{0, y - 2, c.w, y + 19}, HIT_AGENT, i});
    y += 22;
  }
  if ((int)list.size() > n) {
    c.Text(tx, y, "+" + std::to_string((int)list.size() - n) + " MORE", FontId::Caps, Lo());
    return y + kCap;
  }
  return y - (22 - 17);
}

// ---- limits ------------------------------------------------------------------

std::string WindowName(int minutes) {
  if (minutes == 300) return "5H";
  if (minutes == 10080) return "7D";
  if (minutes % 1440 == 0 && minutes) return std::to_string(minutes / 1440) + "D";
  if (minutes % 60 == 0 && minutes) return std::to_string(minutes / 60) + "H";
  return std::to_string(minutes) + "M";
}

int LimitRow(Canvas& c, int y, const char* who, const std::vector<LimitWindow>& ws, int64_t at, int64_t now) {
  c.Text(0, y, who, FontId::Caps, Mid());
  int x = 48;
  for (size_t i = 0; i < ws.size() && i < 2; i++) {
    const LimitWindow& w = ws[i];
    bool reset = w.resetsAt && w.resetsAt <= now;  // the window rolled over since it was measured
    float pct = reset ? 0.f : w.pct;
    x += c.Text(x, y, WindowName(w.minutes), FontId::Caps, Lo()) + 4;
    Cells(c, x, y + 1, 49, 5, 10, 1, pct / 100.0);
    x += 53;
    Ink ink = pct >= 80 ? kHi : Mid();
    x += c.Text(x, y, std::to_string((int)std::lround(pct)) + "%", FontId::Text, ink) + 10;
  }
  const LimitWindow* next = nullptr;
  for (auto& w : ws)
    if (w.resetsAt > now && (!next || w.resetsAt < next->resetsAt)) next = &w;
  if (next)
    TextR(c, c.w, y, "RESETS " + FormatAge(next->resetsAt - now), FontId::Caps, Lo());
  else if (at && now - at > 20 * 60000)
    TextR(c, c.w, y, FormatAge(now - at) + " AGO", FontId::Caps, Lo());
  return y + kLine;
}

int Limits(Canvas& c, int y, const UiInput& in) {
  const UsageState& u = *in.usage;
  y = Header(c, y, "LIMITS");
  if (u.claude.empty() && u.codex.empty()) {
    c.Text(0, y, "NO USAGE DATA YET", FontId::Caps, Lo());
    return y + kCap;
  }
  if (!u.claude.empty()) y = LimitRow(c, y, "CLAUDE", u.claude, u.claudeAt, in.now);
  if (!u.codex.empty()) y = LimitRow(c, y, "CODEX", u.codex, u.codexAt, in.now);
  return y - (kLine - kCap);
}

// ---- year --------------------------------------------------------------------

int64_t CivilDays(int y, int m, int d) {  // days since 1970-01-01
  int64_t yy = y - (m <= 2);
  int64_t era = (yy >= 0 ? yy : yy - 399) / 400, yoe = yy - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

// Every day of the year as one cell, a column per week (Monday on top), like a
// contribution graph: gone days filled, today lit, the rest waiting.
int Year(Canvas& c, int y, const UiInput& in) {
  SYSTEMTIME t = LocalTime(in.now);
  int64_t jan1 = CivilDays(t.wYear, 1, 1);
  int days = (int)(CivilDays(t.wYear + 1, 1, 1) - jan1);
  int today = (int)(CivilDays(t.wYear, t.wMonth, t.wDay) - jan1);  // 0-based
  double frac = (today + (t.wHour * 3600 + t.wMinute * 60 + t.wSecond) / 86400.0) / days;
  int firstRow = (int)(((jan1 % 7) + 7 + 3) % 7);  // Monday = 0 (1970-01-01 was a Thursday)
  int cols = (firstRow + days + 6) / 7;

  char note[48];
  snprintf(note, sizeof note, "DAY %d OF %d", today + 1, days);
  y = Header(c, y, std::to_string(t.wYear), note);

  char pct[8], left[16];
  snprintf(pct, sizeof pct, "%d%%", std::min(99, (int)(frac * 100)));
  snprintf(left, sizeof left, "%dD", days - today - 1);
  int lw = std::max(TextWidth(pct, FontId::Text), TextWidth(left, FontId::Caps));
  int pitch = std::clamp((c.w - lw - 4 + 1) / cols, 2, 6), cell = pitch - 1;  // grid + numbers fill the width

  static const char kMonths[] = "JFMAMJJASOND";
  for (int m = 0; m < 12; m++) {
    int col = (firstRow + (int)(CivilDays(t.wYear, m + 1, 1) - jan1)) / 7;
    char s[2] = {kMonths[m], 0};
    c.Text(col * pitch, y, s, FontId::Caps, m + 1 == t.wMonth ? Mid() : Lo());
  }
  y += kLine;
  for (int i = 0; i < days; i++) {
    int col = (firstRow + i) / 7, row = (firstRow + i) % 7;
    Ink ink = i < today ? Mid() : i == today ? kHi : Alpha(g_cfg.faint / 2);
    c.Fill(col * pitch, y + row * pitch, cell, cell, ink);
  }
  int gridH = 7 * pitch - 1, ty = y + (gridH - kCap - kLine) / 2;
  TextR(c, c.w, ty, pct, FontId::Text, kHi);
  TextR(c, c.w, ty + kLine, left, FontId::Caps, Lo());
  return y + gridH;
}

// ---- AI usage today ----------------------------------------------------------

std::string Count(uint64_t n) {
  char b[24];
  if (n < 1000)
    snprintf(b, sizeof b, "%llu", (unsigned long long)n);
  else if (n < 1000000)
    snprintf(b, sizeof b, n < 10000 ? "%.1fK" : "%.0fK", n / 1e3);
  else if (n < 1000000000)
    snprintf(b, sizeof b, n < 100000000 ? "%.1fM" : "%.0fM", n / 1e6);
  else
    snprintf(b, sizeof b, "%.2fB", n / 1e9);
  return b;
}

int AiTodaySection(Canvas& c, int y, const UiInput& in) {
  const AiToday& a = *in.ai;
  y = Header(c, y, "AI TODAY",
             a.scanning ? std::string("READING LOGS") : Count(a.claude.Total() + a.codex.Total()) + " TOKENS");

  // One row per app under right-aligned column heads. Of the tokens, IN is input not read
  // from the cache (prompts, tool results, cache writes) and CACHE is context re-read from it.
  static const char* const kCols[] = {"TURNS", "CALLS", "IN", "CACHE", "OUT", "TOTAL"};
  constexpr int kLeft = 40, kN = 6;
  auto right = [&](int i) { return kLeft + (c.w - kLeft) * (i + 1) / kN; };
  for (int i = 0; i < kN; i++) TextR(c, right(i), y, kCols[i], FontId::Caps, Lo());
  y += kLine;
  auto row = [&](const char* who, const AiSide& s) {
    c.Text(0, y, who, FontId::Caps, Mid());
    const std::string v[kN] = {std::to_string(s.turns), std::to_string(s.calls), Count(s.in),
                               Count(s.cached),        Count(s.out),            Count(s.Total())};
    for (int i = 0; i < kN; i++) TextR(c, right(i), y, v[i], FontId::Text, i == kN - 1 ? kHi : Mid());
    y += kLine;
  };
  row("CLAUDE", a.claude);
  row("CODEX", a.codex);
  y += 3;

  // Output tokens by hour, Claude under Codex, one bar per hour of the day; a tick every six
  // hours and a bar under the current one stand in for an axis.
  constexpr int kBarH = 12;
  float top = 1;
  for (int h = 0; h < 24; h++) top = std::max(top, a.claudeHours[h] + a.codexHours[h]);
  auto px = [&](float v) { return v > 0 ? std::max(1, (int)std::lround(v / top * kBarH)) : 0; };
  int nowHour = LocalTime(in.now).wHour;
  for (int h = 0; h < 24; h++) {
    int x0 = (int)((int64_t)h * (c.w + 2) / 24), x1 = (int)((int64_t)(h + 1) * (c.w + 2) / 24) - 2;
    int hx = px(a.codexHours[h]), hc = std::min(px(a.claudeHours[h]), kBarH - hx);  // any use shows
    if (h > nowHour || hc + hx == 0) {
      c.Fill(x0, y + kBarH - 1, x1 - x0, 1, Alpha(g_cfg.faint / 2));
    } else {
      c.Fill(x0, y + kBarH - hc, x1 - x0, hc, kHi);
      c.Fill(x0, y + kBarH - hc - hx, x1 - x0, hx, Mid());
    }
    if (h % 6 == 0) c.Fill(x0, y + kBarH + 1, 1, 2, Lo());
    if (h == nowHour) c.Fill(x0, y + kBarH + 1, x1 - x0, 1, kHi);
  }
  return y + kBarH + 3;
}

// ---- calendar ----------------------------------------------------------------

int Calendar(Canvas& c, int y, const UiInput& in, UiOutput& out) {
  const CalendarState& cs = *in.calendar;
  SYSTEMTIME today = LocalTime(in.now);
  auto sameDay = [](const SYSTEMTIME& a, const SYSTEMTIME& b) {
    return a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay;
  };
  int left = 0;
  for (auto& e : cs.events) left += sameDay(LocalTime(e.start), today) && e.start > in.now;
  y = Header(c, y, "CALENDAR", left ? std::to_string(left) + " MORE TODAY" : std::string());
  if (!cs.configured) return LinkLine(c, y, "PASTE AN ICAL LINK IN SETTINGS", in, out) - (kLine - kCap);
  if (!cs.error.empty()) {
    c.Text(0, y, FitText(cs.error, FontId::Caps, c.w), FontId::Caps, cs.events.empty() ? Mid() : Lo());
    y += kLine;
  }
  if (cs.events.empty() && cs.error.empty()) {
    c.Text(0, y, cs.ok ? "NOTHING COMING UP" : "LOADING", FontId::Caps, Lo());
    return y + kCap;
  }
  static const char* kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  int shown = 0;
  SYSTEMTIME lastDay = today;
  bool nextMarked = false;
  for (const CalEvent& e : cs.events) {
    if (shown >= g_cfg.calMax) break;
    bool running = e.start <= in.now && in.now < e.end;
    SYSTEMTIME d = LocalTime(std::max(e.start, in.now));
    bool newDay = !sameDay(d, lastDay);
    if (shown > 0 && !Room(y + (newDay ? kLine : 0))) break;
    if (newDay) {
      char label[32];
      SYSTEMTIME tomorrow = LocalTime(in.now + 86400000LL);
      if (sameDay(d, tomorrow))
        snprintf(label, sizeof label, "TOMORROW");
      else
        snprintf(label, sizeof label, "%s %d", kDays[d.wDayOfWeek % 7], d.wDay);
      c.Text(0, y, label, FontId::Caps, Lo());
      y += kLine;
      lastDay = d;
    }
    std::string when = running ? "NOW" : e.allDay ? "ALL DAY" : HourMin(e.start);
    c.Text(0, y, when, running ? FontId::Caps : FontId::Text, running ? kHi : Mid());
    std::string right;
    if (running && !e.allDay) right = "TIL " + HourMin(e.end);
    else if (!running && !nextMarked && e.start - in.now < 12 * 3600000LL) right = "IN " + FormatAge(e.start - in.now);
    if (!running) nextMarked = true;
    int rx = right.empty() ? c.w : TextR(c, c.w, y, right, FontId::Caps, Lo());
    c.Text(44, y, FitText(e.title.empty() ? "(no title)" : e.title, FontId::Text, rx - 44 - 6), FontId::Text,
           running || shown == 0 ? kHi : Mid());
    y += kLine;
    shown++;
  }
  return y - (kLine - kCap);
}

// ---- slack -------------------------------------------------------------------

int Slack(Canvas& c, int y, const UiInput& in, UiOutput& out) {
  const SlackState& ss = *in.slack;
  y = Header(c, y, "SLACK");
  if (!ss.configured) return LinkLine(c, y, "PASTE A SLACK TOKEN IN SETTINGS", in, out) - (kLine - kCap);
  if (!ss.error.empty()) {
    c.Text(0, y, FitText(ss.error, FontId::Caps, c.w), FontId::Caps, ss.msgs.empty() ? Mid() : Lo());
    y += kLine;
  }
  if (ss.msgs.empty() && ss.error.empty()) {
    c.Text(0, y, ss.ok ? "QUIET SINCE YESTERDAY" : "LOADING", FontId::Caps, Lo());
    return y + kCap;
  }
  int n = std::min((int)ss.msgs.size(), g_cfg.slackMax);
  for (int i = 0; i < n && (i == 0 || Room(y)); i++) {
    const SlackMsg& m = ss.msgs[i];
    if (in.hoverId == HIT_SLACK && in.hoverIndex == i) c.Fill(0, y - 2, c.w, kLine + 1, Alpha(22));
    int rx = TextR(c, c.w, y, FormatAge(in.now - m.at), FontId::Caps, Lo());
    c.Text(0, y, FitText(m.where, FontId::Text, 52), FontId::Text, Lo());
    c.Text(56, y, FitText(m.who, FontId::Text, 46), FontId::Text, m.mine ? Lo() : Mid());
    c.Text(106, y, FitText(m.text, FontId::Text, rx - 106 - 6), FontId::Text, m.mine ? Mid() : m.dm ? kHi : Mid());
    if (!m.link.empty()) out.hits.push_back({{0, y - 2, c.w, y + kLine - 1}, HIT_SLACK, i});
    y += kLine;
  }
  return y - (kLine - kCap);
}

// ---- weather -----------------------------------------------------------------

int WeatherSection(Canvas& c, int y, const UiInput& in) {
  const WeatherState& w = *in.weather;
  std::string note = g_cfg.place;
  if (!w.error.empty() && w.fetchedAt) note += "  " + FormatAge(in.now - w.fetchedAt) + " OLD";
  y = Header(c, y, "WEATHER", note);
  if (!w.fetchedAt) {
    c.Text(0, y, w.error.empty() ? "LOADING" : "WEATHER " + w.error, FontId::Caps, Lo());
    return y + kCap;
  }
  Wx now = Weather(w.code, w.day);
  c.LedIcon(0, y + 2, now.icon, 2, kHi, Alpha(14));
  std::string t = std::to_string((int)std::lround(w.temp));
  int x = 28 + c.Led(28, y, t, 3, kHi, Alpha(16));
  c.LedIcon(x + 3, y, kLedDegree, 3, kHi, Alpha(0));
  int tx = x + 16;
  c.Text(tx, y + 1, now.text, FontId::Text, kHi);
  char line[96];
  snprintf(line, sizeof line, "FEELS %s  %d%%  %d %s", Degrees(w.feels).c_str(), w.humidity, (int)std::lround(w.wind),
           g_cfg.fahrenheit ? "MPH" : "KM/H");
  c.Text(tx, y + 12, line, FontId::Caps, Mid());
  if (!w.days.empty())
    TextR(c, c.w, y + 1, "H " + Degrees(w.days[0].hi) + "  L " + Degrees(w.days[0].lo), FontId::Caps, Lo());
  y += 20 + 8;

  // Next hours.
  int cols = 8, colW = c.w / cols;
  for (int i = 0; i < cols && i < (int)w.hours.size(); i++) {
    const WeatherHour& h = w.hours[i];
    SYSTEMTIME lt = LocalTime(h.at);
    char label[8];
    if (i == 0)
      snprintf(label, sizeof label, "NOW");
    else if (g_cfg.clock24h)
      snprintf(label, sizeof label, "%02d", lt.wHour);
    else
      snprintf(label, sizeof label, "%d%s", lt.wHour % 12 ? lt.wHour % 12 : 12, lt.wHour < 12 ? "A" : "P");
    int cx = i * colW + colW / 2;
    std::string temp = Degrees(h.temp);
    c.Text(cx - TextWidth(label, FontId::Caps) / 2, y, label, FontId::Caps, i == 0 ? Mid() : Lo());
    int x = cx - (13 + TextWidth(temp, FontId::Text)) / 2;  // icon and temperature side by side
    c.Icon(x, y + 9, Weather(h.code, h.day).icon, i == 0 ? kHi : Mid());
    c.Text(x + 13, y + 10, temp, FontId::Text, Mid());
  }
  y += 18 + 7;

  // Next days.
  static const char* kDays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  int dcols = std::min(6, (int)w.days.size());
  if (dcols > 0) {
    int dw = c.w / dcols;
    for (int i = 0; i < dcols; i++) {
      const WeatherDay& d = w.days[i];
      SYSTEMTIME lt = LocalTime(d.at + 12 * 3600000LL);
      const char* label = i == 0 ? "TODAY" : kDays[lt.wDayOfWeek % 7];
      int cx = i * dw + dw / 2;
      std::string hi = Degrees(d.hi), lo = Degrees(d.lo);
      int tw = TextWidth(hi, FontId::Text) + 4 + TextWidth(lo, FontId::Text);
      bool icon = tw + 13 <= dw - 2;  // a deep-frost "-12° -15°" keeps its numbers, drops the icon
      int lx = cx - (tw + (icon ? 13 : 0)) / 2;
      c.Text(cx - TextWidth(label, FontId::Caps) / 2, y, label, FontId::Caps, i == 0 ? Mid() : Lo());
      if (icon) {
        c.Icon(lx, y + 9, Weather(d.code, true).icon, Mid());
        lx += 13;
      }
      lx += c.Text(lx, y + 10, hi, FontId::Text, Mid()) + 4;
      c.Text(lx, y + 10, lo, FontId::Text, Lo());
    }
    y += 18;
  }
  return y;
}

// ---- system ------------------------------------------------------------------

void NetGraph(Canvas& c, int x, int y, int w, int hDown, int hUp, const std::vector<float>& down,
              const std::vector<float>& up) {
  float md = WindowMax(down, w, 64 * 1024), mu = WindowMax(up, w, 32 * 1024);
  int axis = y + hDown;
  c.Dotted(x, axis, w, Alpha(g_cfg.faint / 2));
  int nd = (int)down.size(), nu = (int)up.size();
  for (int i = 0; i < w; i++) {
    int cx = x + w - 1 - i;
    if (i < nd) {
      int hd = (int)std::lround(std::clamp(down[nd - 1 - i] / md, 0.f, 1.f) * hDown);
      if (hd > 0) {
        c.Fill(cx, axis - hd, 1, hd, Alpha(g_cfg.faint * 2 / 3));
        c.Pixel(cx, axis - hd, kHi);
      }
    }
    if (i < nu) {
      int hu = (int)std::lround(std::clamp(up[nu - 1 - i] / mu, 0.f, 1.f) * hUp);
      if (hu > 0) c.Fill(cx, axis + 1, 1, hu, Mid());
    }
  }
}

int System(Canvas& c, int y, const UiInput& in, UiOutput& out) {
  const SysState& s = *in.sys;
  int64_t up = s.uptimeMs / 60000;
  char upText[32];
  snprintf(upText, sizeof upText, "UP %lldD %lldH", (long long)(up / 1440), (long long)((up / 60) % 24));
  y = Header(c, y, "SYSTEM", upText);

  y = Metric(c, y, "CPU", s.cpuName + "  " + std::to_string(s.threads) + " THREADS", Pct(s.cpu));
  Graph(c, 0, y, c.w, 20, s.cpuHist);
  y += 20 + 3;
  int n = (int)s.cores.size();
  for (int i = 0; i < n; i++) {  // one cell per logical core, spread over the full width
    int x0 = (int)((int64_t)i * (c.w + 2) / n), x1 = (int)((int64_t)(i + 1) * (c.w + 2) / n) - 2;
    int fh = (int)std::lround(std::clamp(s.cores[i], 0.f, 1.f) * 8);
    c.Fill(x0, y, x1 - x0, 8 - fh, Lo());
    c.Fill(x0, y + 8 - fh, x1 - x0, fh, kHi);
  }
  y += 8 + 6;

  if (s.gpuOk) {
    std::string detail = s.gpuName;
    if (s.gpuTemp >= 0) detail += "  " + std::to_string(s.gpuTemp) + "\xC2\xB0" "C";
    if (s.gpuWatts >= 0) detail += "  " + std::to_string(s.gpuWatts) + "W";
    if (s.gpuFan >= 0) detail += "  FAN " + std::to_string(s.gpuFan) + "%";
    y = Metric(c, y, "GPU", detail, Pct(s.gpu));
    Graph(c, 0, y, c.w, 14, s.gpuHist);
    y += 14 + 6;
    if (s.vramTotal) {
      double frac = (double)s.vramUsed / s.vramTotal;
      y = Metric(c, y, "VRAM", FormatBytes(s.vramUsed) + " / " + FormatBytes(s.vramTotal), Pct(frac));
      Cells(c, 0, y, c.w, 3, 60, 1, frac);
      y += 3 + 6;
    }
  }

  double ram = (double)s.ramUsed / std::max<uint64_t>(1, s.ramTotal);
  y = Metric(c, y, "RAM", FormatBytes(s.ramUsed) + " / " + FormatBytes(s.ramTotal), Pct(ram));
  Cells(c, 0, y, c.w, 3, 60, 1, ram);
  y += 3 + 6;

  for (const DiskInfo& d : s.disks) {
    double used = 1.0 - (double)d.free / d.total;
    // Under the alarm line the row flashes: a full disk takes builds, git and agents down with it.
    bool low = g_cfg.diskWarnGB && d.free < ((uint64_t)g_cfg.diskWarnGB << 30);
    bool lit = low && (in.now / 400) % 2 == 0;
    if (lit) c.Fill(0, y - 2, c.w, kLine + 4 + 3, Alpha(40));
    std::string detail = (low ? "LOW  " : "") + FormatBytes(d.free) + " FREE OF " + FormatBytes(d.total);
    y = Metric(c, y, d.root, detail, Pct(used), lit || (!low && used >= 0.9) ? kHi : low ? Lo() : Mid());
    Cells(c, 0, y, c.w, 3, 60, 1, used);
    y += 3 + 6;
    if (low) out.animating = true;
  }

  std::string ping = s.online ? (s.pingMs >= 0 ? std::to_string(s.pingMs) + " ms" : "") : "OFFLINE";
  y = Metric(c, y, "NET", s.online ? s.netKind : "", ping);
  c.Text(0, y, "\xE2\x86\x93 " + Rate(s.down), FontId::Text, kHi);
  TextR(c, c.w, y, "\xE2\x86\x91 " + Rate(s.up), FontId::Text, Mid());
  y += kLine + 2;
  NetGraph(c, 0, y, c.w, 14, 8, s.downHist, s.upHist);
  return y + 23;
}

// ---- processes ---------------------------------------------------------------

int Processes(Canvas& c, int y, const UiInput& in, UiOutput& out) {
  const SysState& s = *in.sys;
  const char* sortLabel = g_cfg.procByCpu ? "BY CPU" : "BY RAM";
  int sx = c.w - TextWidth(sortLabel, FontId::Caps);
  y = Header(c, y, "PROCESSES", {}, c.w - sx + 6);
  c.Text(sx, y - kCap - 6, sortLabel, FontId::Caps, in.hoverId == HIT_PROC_SORT ? kHi : Lo());
  out.hits.push_back({{sx - 3, y - kCap - 8, c.w, y - 4}, HIT_PROC_SORT, 0});

  std::vector<ProcInfo> procs = s.procs;
  if (g_cfg.procByCpu)
    std::stable_sort(procs.begin(), procs.end(), [](const ProcInfo& a, const ProcInfo& b) { return a.cpu > b.cpu; });
  int n = std::min((int)procs.size(), g_cfg.procRows);
  if (n == 0) {
    c.Text(0, y, "SAMPLING", FontId::Caps, Lo());
    return y + kCap;
  }
  double top = g_cfg.procByCpu ? std::max(0.01f, procs[0].cpu) : (double)std::max<uint64_t>(1, procs[0].mem);
  for (int i = 0; i < n && (i == 0 || Room(y)); i++) {
    const ProcInfo& p = procs[i];
    int x = c.Text(0, y, FitText(p.name, FontId::Text, 100), FontId::Text, i == 0 ? kHi : Mid());
    if (p.count > 1) c.Text(x + 4, y, "x" + std::to_string(p.count), FontId::Text, Lo());
    double share = (g_cfg.procByCpu ? p.cpu : (double)p.mem) / top;
    c.Fill(138, y + 3, 60, 1, Lo());
    c.Fill(138, y + 2, std::max(1, (int)std::lround(share * 60)), 3, i == 0 ? kHi : Mid());
    char cpu[16];
    snprintf(cpu, sizeof cpu, "%.0f%%", p.cpu * 100);
    int cx = TextR(c, c.w, y, cpu, FontId::Text, g_cfg.procByCpu ? kHi : Lo());
    TextR(c, std::min(cx - 8, 268), y, FormatBytes(p.mem), FontId::Text, g_cfg.procByCpu ? Lo() : kHi);
    y += kLine;
  }
  return y - (kLine - kCap);
}

// ---- settings ----------------------------------------------------------------

enum Opt {
  O_24H, O_SECONDS, O_DOTS, O_PLACE, O_UNITS, O_AGENT_ROWS, O_KEEP_DONE, O_CAL_DAYS, O_CAL_ROWS,
  O_SLACK_ROWS, O_PROC_ROWS, O_PROC_SORT, O_MONITOR, O_ALIGN, O_WIDTH, O_DIM, O_FAINT, O_FPS, O_RESERVE,
  O_DISK_WARN,
};

struct Place {
  const char* name;
  double lat, lon;
};
const Place kPlaces[] = {{"Vancouver", 49.2827, -123.1207}, {"UBC", 49.2606, -123.2460},
                         {"Burnaby", 49.2488, -122.9805},   {"Richmond", 49.1666, -123.1336},
                         {"North Vancouver", 49.3200, -123.0724}, {"Surrey", 49.1913, -122.8490},
                         {"Victoria", 48.4284, -123.3656},  {"Whistler", 50.1163, -122.9574}};

std::string OptValue(int o) {
  const Config& c = g_cfg;
  static const char* kAlign[] = {"TOP", "CENTER", "BOTTOM"};
  switch (o) {
    case O_24H: return c.clock24h ? "ON" : "OFF";
    case O_SECONDS: return c.clockSeconds ? "ON" : "OFF";
    case O_DOTS: return std::to_string(c.clockScale);
    case O_PLACE: return c.place;
    case O_UNITS: return c.fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C";
    case O_AGENT_ROWS: return std::to_string(c.agentMax);
    case O_KEEP_DONE: return c.doneMinutes ? FormatAge(c.doneMinutes * 60000LL) : "NO";
    case O_CAL_DAYS: return std::to_string(c.calDays);
    case O_CAL_ROWS: return std::to_string(c.calMax);
    case O_SLACK_ROWS: return std::to_string(c.slackMax);
    case O_PROC_ROWS: return std::to_string(c.procRows);
    case O_PROC_SORT: return c.procByCpu ? "CPU" : "RAM";
    case O_MONITOR: return c.monitor;
    case O_ALIGN: return kAlign[std::clamp(c.valign, 0, 2)];
    case O_WIDTH: return std::to_string(c.width);
    case O_DIM: return std::to_string(c.dim);
    case O_FAINT: return std::to_string(c.faint);
    case O_FPS: return std::to_string(c.fps);
    case O_RESERVE: return c.reserve ? "ON" : "OFF";
    case O_DISK_WARN: return c.diskWarnGB ? "UNDER " + std::to_string(c.diskWarnGB) + " GB" : "OFF";
  }
  return {};
}

void OptStep(Config& c, int o, int dir) {
  auto step = [&](int& v, int by, int lo, int hi) { v = std::clamp(v + dir * by, lo, hi); };
  switch (o) {
    case O_24H: c.clock24h = !c.clock24h; break;
    case O_SECONDS: c.clockSeconds = !c.clockSeconds; break;
    case O_DOTS: step(c.clockScale, 1, 3, 6); break;
    case O_PLACE: {
      int n = (int)std::size(kPlaces), cur = -1;
      for (int i = 0; i < n; i++)
        if (c.place == kPlaces[i].name) cur = i;
      int next = cur < 0 ? 0 : (cur + dir + n) % n;
      c.place = kPlaces[next].name;
      c.lat = kPlaces[next].lat;
      c.lon = kPlaces[next].lon;
      break;
    }
    case O_UNITS: c.fahrenheit = !c.fahrenheit; break;
    case O_AGENT_ROWS: step(c.agentMax, 1, 1, 12); break;
    case O_KEEP_DONE: step(c.doneMinutes, 15, 0, 240); break;
    case O_CAL_DAYS: step(c.calDays, 1, 1, 14); break;
    case O_CAL_ROWS: step(c.calMax, 1, 1, 12); break;
    case O_SLACK_ROWS: step(c.slackMax, 1, 1, 12); break;
    case O_PROC_ROWS: step(c.procRows, 1, 1, 12); break;
    case O_PROC_SORT: c.procByCpu = !c.procByCpu; break;
    case O_MONITOR: {
      static const char* kMon[] = {"leftmost", "primary", "rightmost"};
      int cur = 0;
      for (int i = 0; i < 3; i++)
        if (c.monitor == kMon[i]) cur = i;
      c.monitor = kMon[(cur + dir + 3) % 3];
      break;
    }
    case O_ALIGN: c.valign = (c.valign + dir + 3) % 3; break;
    case O_WIDTH: step(c.width, 20, 240, 480); break;
    case O_DIM: step(c.dim, 10, 60, 250); break;
    case O_FAINT: step(c.faint, 6, 18, 140); break;
    case O_FPS: step(c.fps, 3, 6, 30); break;
    case O_RESERVE: c.reserve = !c.reserve; break;
    case O_DISK_WARN: {
      static const int kSteps[] = {0, 2, 5, 10, 20, 50};
      int n = (int)std::size(kSteps), i = 0;
      while (i + 1 < n && kSteps[i] < c.diskWarnGB) i++;
      c.diskWarnGB = kSteps[std::clamp(i + dir, 0, n - 1)];
      break;
    }
  }
}

// A framed text button whose frame ends at `right`; returns the frame's left edge.
int ButtonR(Canvas& c, int right, int y, std::string_view label, int id, int index, const UiInput& in,
            UiOutput& out) {
  bool hover = in.hoverId == id && in.hoverIndex == index;
  int x0 = right - TextWidth(label, FontId::Caps) - 6;
  c.Frame(x0, y - 2, right - x0, kCap + 4, hover ? kHi : Lo());
  c.Text(x0 + 3, y, label, FontId::Caps, hover ? kHi : Mid());
  out.hits.push_back({{x0 - 1, y - 3, right + 1, y + kCap + 3}, id, index});
  return x0;
}

// "LABEL ......... < VALUE >"; both arrows step the value (and flip toggles).
int StepRow(Canvas& c, int y, const char* label, const std::string& value, int decId, int incId, int index,
            const UiInput& in, UiOutput& out) {
  c.Text(8, y, label, FontId::Caps, Mid());
  bool hiDec = in.hoverId == decId && in.hoverIndex == index, hiInc = in.hoverId == incId && in.hoverIndex == index;
  int x = TextR(c, c.w, y, ">", FontId::Text, hiInc ? kHi : Lo());
  out.hits.push_back({{x - 4, y - 2, c.w, y + kCap + 2}, incId, index});
  int vx = TextR(c, x - 6, y, value, FontId::Caps, kHi);
  int lx = TextR(c, vx - 6, y, "<", FontId::Text, hiDec ? kHi : Lo());
  out.hits.push_back({{lx - 4, y - 2, vx - 2, y + kCap + 2}, decId, index});
  int dx = 8 + TextWidth(label, FontId::Caps) + 6;
  dx += dx & 1;
  c.Dotted(dx, y + 6, lx - 6 - dx, Alpha(g_cfg.faint / 2));
  return y + 11;
}

int OptRow(Canvas& c, int y, const char* label, int opt, const UiInput& in, UiOutput& out) {
  return StepRow(c, y, label, OptValue(opt), HIT_S_DEC, HIT_S_INC, opt, in, out);
}

int SecretRow(Canvas& c, int y, const char* label, bool set, int index, const UiInput& in, UiOutput& out) {
  c.Text(8, y, label, FontId::Caps, Mid());
  int x = c.w;
  if (set) x = ButtonR(c, x, y, "CLEAR", HIT_S_CLEAR, index, in, out) - 5;
  x = ButtonR(c, x, y, "PASTE", HIT_S_PASTE, index, in, out);
  TextR(c, x - 8, y, set ? "SAVED" : "NOT SET", FontId::Caps, set ? kHi : Lo());
  return y + 13;
}

int Subhead(Canvas& c, int y, const char* label) {
  c.Text(0, y, label, FontId::Caps, Lo());
  return y + 11;
}

int Settings(Canvas& c, int y, const UiInput& in, UiOutput& out) {
  int done = ButtonR(c, c.w, y, "DONE", HIT_S_DONE, 0, in, out);
  y = Header(c, y, "SETTINGS", {}, c.w - done + 6);
  if (!in.flash.empty()) {
    c.Text(0, y, in.flash, FontId::Caps, kHi);
    y += 13;
  }

  y = Subhead(c, y, "WIDGETS");
  std::vector<int> order;
  for (const std::string& s : g_cfg.sections)
    for (int i = 0; i < kSectionCount; i++)
      if (s == kSectionNames[i]) order.push_back(i);
  for (int i = 0; i < kSectionCount; i++)
    if (std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i);
  for (int idx : order) {
    bool on = std::find(g_cfg.sections.begin(), g_cfg.sections.end(), kSectionNames[idx]) != g_cfg.sections.end();
    bool hover = in.hoverId == HIT_S_TOGGLE && in.hoverIndex == idx;
    c.Frame(8, y, 7, 7, hover ? kHi : Mid());
    if (on) c.Fill(10, y + 2, 3, 3, kHi);
    std::string name = std::string(kSectionNames[idx]) == "ai" ? "AI TODAY" : kSectionNames[idx];
    int w = c.Text(20, y, name, FontId::Caps, on ? kHi : Lo());
    out.hits.push_back({{6, y - 2, 20 + w + 4, y + kCap + 2}, HIT_S_TOGGLE, idx});
    if (on) {
      bool hu = in.hoverId == HIT_S_UP && in.hoverIndex == idx, hd = in.hoverId == HIT_S_DOWN && in.hoverIndex == idx;
      int dx = TextR(c, c.w, y, "\xE2\x86\x93", FontId::Text, hd ? kHi : Lo());
      out.hits.push_back({{dx - 4, y - 2, c.w, y + kCap + 2}, HIT_S_DOWN, idx});
      int ux = TextR(c, dx - 10, y, "\xE2\x86\x91", FontId::Text, hu ? kHi : Lo());
      out.hits.push_back({{ux - 4, y - 2, dx - 5, y + kCap + 2}, HIT_S_UP, idx});
    }
    y += 11;
  }
  y += 6;
  y = Subhead(c, y, "CLOCK");
  y = OptRow(c, y, "24-HOUR", O_24H, in, out);
  y = OptRow(c, y, "SECONDS", O_SECONDS, in, out);
  y = OptRow(c, y, "DOT SIZE", O_DOTS, in, out);
  y += 6;
  y = Subhead(c, y, "WEATHER");
  y = OptRow(c, y, "PLACE", O_PLACE, in, out);
  y = OptRow(c, y, "UNITS", O_UNITS, in, out);
  y += 6;
  y = Subhead(c, y, "AGENTS");
  y = OptRow(c, y, "ROWS", O_AGENT_ROWS, in, out);
  y = OptRow(c, y, "KEEP FINISHED FOR", O_KEEP_DONE, in, out);
  y += 6;
  y = Subhead(c, y, "CALENDAR");
  y = SecretRow(c, y, "ICAL LINK", in.icalSet, 0, in, out);
  y = SecretRow(c, y, "SECOND LINK", in.ical2Set, 2, in, out);
  y = OptRow(c, y, "DAYS AHEAD", O_CAL_DAYS, in, out);
  y = OptRow(c, y, "ROWS", O_CAL_ROWS, in, out);
  y += 6;
  y = Subhead(c, y, "SLACK");
  y = SecretRow(c, y, "USER TOKEN", in.slackSet, 1, in, out);
  y = OptRow(c, y, "ROWS", O_SLACK_ROWS, in, out);
  y += 6;
  y = Subhead(c, y, "PROCESSES");
  y = OptRow(c, y, "ROWS", O_PROC_ROWS, in, out);
  y = OptRow(c, y, "SORT", O_PROC_SORT, in, out);
  y += 6;
  y = Subhead(c, y, "SYSTEM");
  y = OptRow(c, y, "LOW DISK ALARM", O_DISK_WARN, in, out);
  y += 6;
  y = Subhead(c, y, "DISPLAY");
  y = OptRow(c, y, "MONITOR", O_MONITOR, in, out);
  y = OptRow(c, y, "RESERVE SPACE", O_RESERVE, in, out);
  y = OptRow(c, y, "ALIGN", O_ALIGN, in, out);
  y = OptRow(c, y, "WIDTH", O_WIDTH, in, out);
  y = OptRow(c, y, "TEXT DIM", O_DIM, in, out);
  y = OptRow(c, y, "TEXT FAINT", O_FAINT, in, out);
  y = OptRow(c, y, "ANIMATION FPS", O_FPS, in, out);
  y = StepRow(c, y, "START WITH WINDOWS", in.autostart ? "ON" : "OFF", HIT_S_AUTOSTART, HIT_S_AUTOSTART, 0, in, out);
  y += 6;
  bool hiCfg = in.hoverId == HIT_S_CONFIG;
  int w = c.Text(0, y, "OPEN CONFIG FILE", FontId::Caps, hiCfg ? kHi : Lo());
  out.hits.push_back({{0, y - 2, w + 2, y + kCap + 2}, HIT_S_CONFIG, 0});
  return y + kCap;
}

}  // namespace

bool ApplySettingsHit(const Hit& h) {
  Config c = g_cfg;
  auto& s = c.sections;
  if (h.index < 0) return false;
  if (h.id == HIT_S_TOGGLE && h.index < kSectionCount) {
    auto it = std::find(s.begin(), s.end(), kSectionNames[h.index]);
    if (it != s.end())
      s.erase(it);
    else
      s.push_back(kSectionNames[h.index]);
  } else if ((h.id == HIT_S_UP || h.id == HIT_S_DOWN) && h.index < kSectionCount) {
    auto it = std::find(s.begin(), s.end(), kSectionNames[h.index]);
    if (it == s.end()) return false;
    size_t i = (size_t)(it - s.begin());
    size_t j = h.id == HIT_S_UP ? (i == 0 ? 0 : i - 1) : std::min(i + 1, s.size() - 1);
    if (i == j) return false;
    std::swap(s[i], s[j]);
  } else if (h.id == HIT_S_DEC || h.id == HIT_S_INC) {
    OptStep(c, h.index, h.id == HIT_S_INC ? 1 : -1);
  } else if (h.id == HIT_PROC_SORT) {
    c.procByCpu = !c.procByCpu;
  } else {
    return false;
  }
  ConfigCommit(c);
  return true;
}

namespace {

// What kind of content a section shows, coarsely. When it changes, new content has arrived
// and fades in; any other change (a number ticking, a row added) only moves.
size_t Mode(const std::string& s, const UiInput& in) {
  auto feed = [](bool configured, bool ok, const std::string& error, bool empty) -> size_t {
    return !configured ? 1 : !empty ? 2 : !error.empty() ? 3 : ok ? 4 : 5;
  };
  if (s == "media") return in.media->present ? std::hash<std::string>{}(in.media->title + '\n' + in.media->artist) | 1 : 0;
  if (s == "agents") return in.agents->empty() ? 1 : 2;
  if (s == "ai") return in.ai->scanning ? 1 : 2;
  if (s == "weather") return in.weather->fetchedAt ? 2 : in.weather->error.empty() ? 1 : 3;
  if (s == "calendar") {
    const CalendarState& k = *in.calendar;
    return feed(k.configured, k.ok, k.error, k.events.empty());
  }
  if (s == "slack") {
    const SlackState& k = *in.slack;
    return feed(k.configured, k.ok, k.error, k.msgs.empty());
  }
  if (s == "processes") return in.sys->procs.empty() ? 1 : 2;
  return 0;
}

}  // namespace

void RenderUI(Canvas& c, const UiInput& in, UiOutput& out) {
  TextCacheTrim();
  out = UiOutput{};
  g_bottom = c.h - kGap - kCap;
  int y = 0;
  if (in.settings) {
    y = Settings(c, 3, in, out);  // room for the DONE frame above the first line
    out.blocks.push_back({"settings", 0, y, 0, 0, 0, 0, out.hits.size()});
  } else {
    bool first = true;
    for (const std::string& s : g_cfg.sections) {
      int start = first ? 0 : y + kGap;
      if (start > c.h - kLine) break;  // out of room on this monitor
      size_t hitsBefore = out.hits.size();
      bool animBefore = out.animating;
      g_head = -1;
      int end;
      if (s == "clock")
        end = Clock(c, start, in.now);
      else if (s == "year")
        end = Year(c, start, in);
      else if (s == "ai")
        end = AiTodaySection(c, start, in);
      else if (s == "media")
        end = Media(c, start, in, out);
      else if (s == "agents")
        end = Agents(c, start, in, out);
      else if (s == "limits")
        end = Limits(c, start, in);
      else if (s == "calendar")
        end = Calendar(c, start, in, out);
      else if (s == "slack")
        end = Slack(c, start, in, out);
      else if (s == "weather")
        end = WeatherSection(c, start, in);
      else if (s == "system")
        end = System(c, start, in, out);
      else if (s == "processes")
        end = Processes(c, start, in, out);
      else
        continue;
      if (end > g_bottom) {  // it ran off the monitor: leave it out whole rather than sliced
        c.ClearRows(start, c.h);
        out.hits.resize(hitsBefore);
        out.animating = animBefore;
        break;
      }
      out.blocks.push_back({s, start, end, g_head > start ? g_head - start : 0, first ? 0 : kGap, Mode(s, in),
                            hitsBefore, out.hits.size()});
      y = end;
      first = false;
    }
    // Settings stay one hover away; the row is kept so the column never changes height, and
    // its right side carries what the column itself costs.
    int fy = y + kGap;
    size_t hitsBefore = out.hits.size();
    if (in.hovering) {
      bool hover = in.hoverId == HIT_OPEN_SETTINGS;
      int w = c.Text(0, fy, "SETTINGS", FontId::Caps, hover ? kHi : Lo());
      out.hits.push_back({{0, fy - 3, w + 3, fy + kCap + 3}, HIT_OPEN_SETTINGS, 0});
    }
    if (const SysState& sys = *in.sys; sys.selfMem) {
      char cpu[16];
      snprintf(cpu, sizeof cpu, sys.selfCpu < 0.1f ? "%.1f%%" : "%.0f%%", sys.selfCpu * 100);
      int rx = TextR(c, c.w, fy, FormatBytes(sys.selfMem), FontId::Text, Mid());
      rx = TextR(c, rx - 4, fy, "RAM", FontId::Caps, Lo());
      rx = TextR(c, rx - 8, fy, cpu, FontId::Text, Mid());
      rx = TextR(c, rx - 4, fy, "CPU", FontId::Caps, Lo());
      TextR(c, rx - 8, fy, "PIXELBAR", FontId::Caps, Lo());
    }
    y = std::min(fy + kCap + 2, c.h);
    out.blocks.push_back({"footer", fy, y, 0, kGap, 0, hitsBefore, out.hits.size()});
  }
  out.rows = std::min(y, c.h);
}
