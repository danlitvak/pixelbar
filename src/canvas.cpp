// Framebuffer, the pixel font and text. Everything is drawn in "art pixels"; one art pixel
// is an s x s block of physical pixels (1 at 100% scaling), so the column stays crisp.
// Large numbers and icons use a dot-matrix rendering of the same glyphs (see Canvas::Led).
#include "pixelbar.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace {

struct Def {
  char32_t cp;
  const char* rows;  // '|' between rows, '#' lit
};

// 5x7 caps, 5-row x-height, 2-row descenders. Digits are all 5 wide so numbers that
// tick (clock, percentages) never shift.
const Def kFont[] = {
    {U' ', "...|...|...|...|...|...|..."},
    {U'!', "#|#|#|#|#|.|#"},
    {U'"', "#.#|#.#|...|...|...|...|..."},
    {U'#', ".#.#.|.#.#.|#####|.#.#.|#####|.#.#.|.#.#."},
    {U'$', "..#..|.####|#.#..|.###.|..#.#|####.|..#.."},
    {U'%', "##...|##..#|...#.|..#..|.#...|#..##|...##"},
    {U'&', ".##..|#..#.|#.#..|.#...|#.#.#|#..#.|.##.#"},
    {U'\'', "#|#|.|.|.|.|."},
    {U'(', "..#|.#.|#..|#..|#..|.#.|..#"},
    {U')', "#..|.#.|..#|..#|..#|.#.|#.."},
    {U'*', ".....|..#..|#.#.#|.###.|#.#.#|..#..|....."},
    {U'+', ".....|..#..|..#..|#####|..#..|..#..|....."},
    {U',', "..|..|..|..|..|.#|.#|#."},
    {U'-', "....|....|....|####|....|....|...."},
    {U'.', ".|.|.|.|.|.|#"},
    {U'/', "....#|....#|...#.|..#..|.#...|#....|#...."},
    {U'0', ".###.|#...#|#..##|#.#.#|##..#|#...#|.###."},
    {U'1', "..#..|.##..|..#..|..#..|..#..|..#..|.###."},
    {U'2', ".###.|#...#|....#|...#.|..#..|.#...|#####"},
    {U'3', "#####|...#.|..#..|...#.|....#|#...#|.###."},
    {U'4', "...#.|..##.|.#.#.|#..#.|#####|...#.|...#."},
    {U'5', "#####|#....|####.|....#|....#|#...#|.###."},
    {U'6', "..##.|.#...|#....|####.|#...#|#...#|.###."},
    {U'7', "#####|....#|...#.|..#..|.#...|.#...|.#..."},
    {U'8', ".###.|#...#|#...#|.###.|#...#|#...#|.###."},
    {U'9', ".###.|#...#|#...#|.####|....#|...#.|.##.."},
    {U':', ".|.|#|.|.|#|."},
    {U';', "..|..|.#|..|..|.#|.#|#."},
    {U'<', "...#|..#.|.#..|#...|.#..|..#.|...#"},
    {U'=', ".....|.....|#####|.....|#####|.....|....."},
    {U'>', "#...|.#..|..#.|...#|..#.|.#..|#..."},
    {U'?', ".###.|#...#|....#|...#.|..#..|.....|..#.."},
    {U'@', ".###.|#...#|#.###|#.#.#|#.###|#....|.###."},
    {U'A', ".###.|#...#|#...#|#####|#...#|#...#|#...#"},
    {U'B', "####.|#...#|#...#|####.|#...#|#...#|####."},
    {U'C', ".###.|#...#|#....|#....|#....|#...#|.###."},
    {U'D', "####.|#...#|#...#|#...#|#...#|#...#|####."},
    {U'E', "#####|#....|#....|####.|#....|#....|#####"},
    {U'F', "#####|#....|#....|####.|#....|#....|#...."},
    {U'G', ".###.|#...#|#....|#.###|#...#|#...#|.####"},
    {U'H', "#...#|#...#|#...#|#####|#...#|#...#|#...#"},
    {U'I', "###|.#.|.#.|.#.|.#.|.#.|###"},
    {U'J', "..###|...#.|...#.|...#.|...#.|#..#.|.##.."},
    {U'K', "#...#|#..#.|#.#..|##...|#.#..|#..#.|#...#"},
    {U'L', "#....|#....|#....|#....|#....|#....|#####"},
    {U'M', "#...#|##.##|#.#.#|#.#.#|#...#|#...#|#...#"},
    {U'N', "#...#|#...#|##..#|#.#.#|#..##|#...#|#...#"},
    {U'O', ".###.|#...#|#...#|#...#|#...#|#...#|.###."},
    {U'P', "####.|#...#|#...#|####.|#....|#....|#...."},
    {U'Q', ".###.|#...#|#...#|#...#|#.#.#|#..#.|.##.#"},
    {U'R', "####.|#...#|#...#|####.|#.#..|#..#.|#...#"},
    {U'S', ".###.|#...#|#....|.###.|....#|#...#|.###."},
    {U'T', "#####|..#..|..#..|..#..|..#..|..#..|..#.."},
    {U'U', "#...#|#...#|#...#|#...#|#...#|#...#|.###."},
    {U'V', "#...#|#...#|#...#|#...#|#...#|.#.#.|..#.."},
    {U'W', "#...#|#...#|#...#|#.#.#|#.#.#|#.#.#|.#.#."},
    {U'X', "#...#|#...#|.#.#.|..#..|.#.#.|#...#|#...#"},
    {U'Y', "#...#|#...#|.#.#.|..#..|..#..|..#..|..#.."},
    {U'Z', "#####|....#|...#.|..#..|.#...|#....|#####"},
    {U'[', "###|#..|#..|#..|#..|#..|###"},
    {U'\\', "#....|#....|.#...|..#..|...#.|....#|....#"},
    {U']', "###|..#|..#|..#|..#|..#|###"},
    {U'^', "..#..|.#.#.|#...#|.....|.....|.....|....."},
    {U'_', ".....|.....|.....|.....|.....|.....|.....|#####"},
    {U'`', "#.|.#|..|..|..|..|.."},
    {U'a', ".....|.....|.###.|....#|.####|#...#|.####"},
    {U'b', "#....|#....|#.##.|##..#|#...#|#...#|####."},
    {U'c', ".....|.....|.###.|#....|#....|#...#|.###."},
    {U'd', "....#|....#|.##.#|#..##|#...#|#...#|.####"},
    {U'e', ".....|.....|.###.|#...#|#####|#....|.###."},
    {U'f', "..##|.#..|###.|.#..|.#..|.#..|.#.."},
    {U'g', ".....|.....|.####|#...#|#...#|#...#|.####|....#|.###."},
    {U'h', "#....|#....|#.##.|##..#|#...#|#...#|#...#"},
    {U'i', "#|.|#|#|#|#|#"},
    {U'j', "..#|...|..#|..#|..#|..#|..#|#.#|.#."},
    {U'k', "#...|#...|#..#|#.#.|##..|#.#.|#..#"},
    {U'l', "#.|#.|#.|#.|#.|#.|.#"},
    {U'm', ".....|.....|##.#.|#.#.#|#.#.#|#.#.#|#.#.#"},
    {U'n', ".....|.....|#.##.|##..#|#...#|#...#|#...#"},
    {U'o', ".....|.....|.###.|#...#|#...#|#...#|.###."},
    {U'p', ".....|.....|####.|#...#|#...#|#...#|####.|#....|#...."},
    {U'q', ".....|.....|.####|#...#|#...#|#...#|.####|....#|....#"},
    {U'r', "....|....|#.##|##..|#...|#...|#..."},
    {U's', ".....|.....|.####|#....|.###.|....#|####."},
    {U't', "....|.#..|###.|.#..|.#..|.#..|..##"},
    {U'u', ".....|.....|#...#|#...#|#...#|#..##|.##.#"},
    {U'v', ".....|.....|#...#|#...#|#...#|.#.#.|..#.."},
    {U'w', ".....|.....|#...#|#...#|#.#.#|#.#.#|.#.#."},
    {U'x', ".....|.....|#...#|.#.#.|..#..|.#.#.|#...#"},
    {U'y', ".....|.....|#...#|#...#|#...#|#...#|.####|....#|.###."},
    {U'z', ".....|.....|#####|...#.|..#..|.#...|#####"},
    {U'{', "..##|.#..|.#..|#...|.#..|.#..|..##"},
    {U'|', "#|#|#|#|#|#|#|#|#"},
    {U'}', "##..|..#.|..#.|...#|..#.|..#.|##.."},
    {U'~', ".....|.....|.#...|#.#.#|...#.|.....|....."},
    {0x00B7, ".|.|.|#|.|.|."},
    {0x2026, ".....|.....|.....|.....|.....|.....|#.#.#"},
    {0x2191, "..#..|.###.|#.#.#|..#..|..#..|..#..|..#.."},
    {0x2193, "..#..|..#..|..#..|..#..|#.#.#|.###.|..#.."},
    {0x00B0, ".#.|#.#|.#.|...|...|...|..."},
};

Glyph ParseRows(const char* rows) {
  Glyph g;
  int r = 0, x = 0, w = 0;
  for (const char* p = rows;; p++) {
    if (*p == '|' || *p == 0) {
      w = std::max(w, x);
      r++;
      x = 0;
      if (*p == 0 || r >= 16) break;
      continue;
    }
    if (*p == '#' && x < 16) g.bits[r] |= (uint16_t)(1u << x);
    x++;
  }
  g.w = (uint8_t)w;
  g.h = (uint8_t)r;
  return g;
}

using GlyphMap = std::unordered_map<char32_t, Glyph>;

const GlyphMap& Font() {
  static const GlyphMap font = [] {
    GlyphMap m;
    for (auto& d : kFont) m[d.cp] = ParseRows(d.rows);
    return m;
  }();
  return font;
}

// Typographic punctuation the font doesn't draw, mapped to what it does. 0 drops the char.
char32_t Simplify(char32_t c) {
  switch (c) {
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x00B4: return U'\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0x00AB: case 0x00BB: return U'"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return U'-';
    case 0x00A0: case 0x2007: case 0x202F: case 0x3000: return U' ';
    case 0x2022: case 0x2219: case 0x25CF: case 0x30FB: return 0x00B7;
    case 0x00D7: return U'x';
    case 0x2190: return U'<';
    case 0x2192: return U'>';
    case 0x00F8: return U'o';
    case 0x00D8: return U'O';
    case 0x0142: return U'l';
    case 0x0141: return U'L';
    case 0x0111: return U'd';
    case 0x0110: return U'D';
    case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFE0E: case 0xFE0F: case 0xFEFF: return 0;
  }
  if (c >= 0x2000 && c <= 0x200A) return U' ';
  if ((c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x1F1E6 && c <= 0x1F1FF)) return 0;
  return c;
}

// Accented Latin letters drawn as their base letter (Beyonce, Sigur Ros).
char32_t FoldAccent(char32_t c) {
  if (c < 0xC0 || c > 0x24F) {
    if (!(c >= 0x1E00 && c <= 0x1EFF)) return c;
  }
  wchar_t in[1] = {(wchar_t)c};
  wchar_t out[8] = {};
  int n = FoldStringW(MAP_COMPOSITE, in, 1, out, 8);
  return (n >= 1 && out[0] < 0x80) ? (char32_t)out[0] : c;
}

struct Prepared {
  std::vector<const Glyph*> glyphs;
  std::vector<uint32_t> srcEnd;  // source byte offset after each glyph
  bool fallback = false;
  int fbW = 0, fbH = 0, fbDy = 0;
  std::vector<uint8_t> fb;
  int width = 0;  // no trailing gap
};

std::unordered_map<std::string, Prepared> g_cache;

bool DecodeNext(std::string_view s, size_t& i, char32_t& cp) {
  if (i >= s.size()) return false;
  auto b = [&](size_t k) { return (uint32_t)(unsigned char)s[k]; };
  uint32_t c = b(i);
  if (c < 0x80) { cp = c; i += 1; return true; }
  if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (b(i + 1) & 0x3F); i += 2; return true; }
  if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0x0F) << 12) | ((b(i + 1) & 0x3F) << 6) | (b(i + 2) & 0x3F); i += 3; return true; }
  if ((c >> 3) == 30 && i + 3 < s.size()) {
    cp = ((c & 0x07) << 18) | ((b(i + 1) & 0x3F) << 12) | ((b(i + 2) & 0x3F) << 6) | (b(i + 3) & 0x3F);
    i += 4;
    return true;
  }
  cp = 0xFFFD;
  i += 1;
  return true;
}

// Text the pixel font can't draw (Hebrew, CJK, Cyrillic ...) is rasterised by GDI with
// anti-aliasing off, at the same cap height, so it still sits on the pixel grid.
void RenderFallback(std::string_view s, FontId f, Prepared& p) {
  std::wstring w;
  size_t i = 0;
  char32_t cp;
  while (DecodeNext(s, i, cp)) {
    char32_t c = Simplify(cp);
    if (c == 0) continue;
    if (c >= 0x10000) {
      c -= 0x10000;
      w += (wchar_t)(0xD800 + (c >> 10));
      w += (wchar_t)(0xDC00 + (c & 0x3FF));
    } else {
      w += (wchar_t)c;
    }
  }
  if (f == FontId::Caps && !w.empty()) CharUpperBuffW(w.data(), (DWORD)w.size());
  p.fallback = true;
  if (w.empty()) return;

  HDC dc = CreateCompatibleDC(nullptr);
  HFONT font = CreateFontW(-10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HGDIOBJ oldFont = SelectObject(dc, font);
  RECT r{0, 0, 0, 0};
  DrawTextW(dc, w.c_str(), (int)w.size(), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  TEXTMETRICW tm{};
  GetTextMetricsW(dc, &tm);
  int bw = std::max(1, (int)r.right), bh = std::max(1, (int)r.bottom);

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = bw;
  bi.bmiHeader.biHeight = -bh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp && bits) {
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)bw * bh * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rr{0, 0, bw, bh};
    DrawTextW(dc, w.c_str(), (int)w.size(), &rr, DT_SINGLELINE | DT_NOPREFIX | DT_LEFT | DT_TOP);
    GdiFlush();
    const uint32_t* src = (const uint32_t*)bits;
    p.fb.resize((size_t)bw * bh);
    int maxX = 0;
    for (int y = 0; y < bh; y++)
      for (int x = 0; x < bw; x++) {
        bool on = ((src[y * bw + x] >> 8) & 0xFF) > 127;
        p.fb[(size_t)y * bw + x] = on;
        if (on) maxX = std::max(maxX, x + 1);
      }
    p.fbW = bw;
    p.fbH = bh;
    p.width = maxX;
    p.fbDy = kCap - (int)tm.tmAscent;
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
  }
  SelectObject(dc, oldFont);
  DeleteObject(font);
  DeleteDC(dc);
}

const Prepared& Prepare(std::string_view s, FontId f) {
  std::string key(1, f == FontId::Caps ? 'C' : 'T');
  key += s;
  auto it = g_cache.find(key);
  if (it != g_cache.end()) return it->second;

  Prepared p;
  const GlyphMap& font = Font();
  bool ok = true;
  size_t i = 0;
  char32_t cp;
  while (DecodeNext(s, i, cp)) {
    char32_t c = FoldAccent(Simplify(cp));
    if (c == 0) continue;
    if (f == FontId::Caps && c >= U'a' && c <= U'z') c -= 32;
    auto g = font.find(c);
    if (g == font.end()) {
      ok = false;
      break;
    }
    p.glyphs.push_back(&g->second);
    p.srcEnd.push_back((uint32_t)i);
  }
  if (ok) {
    for (const Glyph* g : p.glyphs) p.width += g->w + 1;
    if (p.width > 0) p.width--;
  } else {
    p.glyphs.clear();
    p.srcEnd.clear();
    RenderFallback(s, f, p);
  }
  return g_cache.emplace(std::move(key), std::move(p)).first->second;
}

}  // namespace

void TextCacheTrim() {
  if (g_cache.size() > 1500) g_cache.clear();
}

const Glyph& IconGlyph(const char* rows) {
  static std::unordered_map<const char*, Glyph> cache;
  auto it = cache.find(rows);
  if (it != cache.end()) return it->second;
  return cache.emplace(rows, ParseRows(rows)).first->second;
}

void Canvas::Reset(int artW, int artH, int scale, uint32_t* pixels) {
  w = artW;
  h = artH;
  s = scale;
  pw = artW * scale;
  ph = artH * scale;
  px = pixels;
  ResetClip();
}

void Canvas::Clear(int artRows) {
  int rows = std::clamp(artRows, 0, h) * s;
  if (px) memset(px, 0, (size_t)pw * rows * 4);
}

void Canvas::ClearRows(int from, int to) {
  from = std::clamp(from, 0, h);
  to = std::clamp(to, from, h);
  if (px) memset(px + (size_t)from * s * pw, 0, (size_t)pw * (to - from) * s * 4);
}

void Canvas::Backfill(int from, int to, Ink c) {
  if (!px || c.a == 0) return;
  from = std::clamp(from, 0, h);
  to = std::clamp(to, from, h);
  uint32_t v = (uint32_t)c.lum * c.a / 255;
  uint32_t fill = ((uint32_t)c.a << 24) | (v << 16) | (v << 8) | v;
  for (size_t i = (size_t)pw * from * s, n = (size_t)pw * to * s; i < n; i++)
    if (px[i] == 0) px[i] = fill;
}

void Canvas::SetClip(int x, int y, int cw, int ch) {
  clip.left = std::max(0, x);
  clip.top = std::max(0, y);
  clip.right = std::min(w, x + cw);
  clip.bottom = std::min(h, y + ch);
}

void Canvas::Fill(int x, int y, int fw, int fh, Ink c) {
  if (!px || c.a == 0 || fw <= 0 || fh <= 0) return;
  int x0 = std::max(x, (int)clip.left), y0 = std::max(y, (int)clip.top);
  int x1 = std::min(x + fw, (int)clip.right), y1 = std::min(y + fh, (int)clip.bottom);
  if (x0 >= x1 || y0 >= y1) return;
  uint32_t a = c.a, v = (uint32_t)c.lum * a / 255, inv = 255 - a;
  uint32_t solid = (a << 24) | (v << 16) | (v << 8) | v;
  for (int py = y0 * s; py < y1 * s; py++) {
    uint32_t* row = px + (size_t)py * pw;
    for (int i = x0 * s; i < x1 * s; i++) {
      if (a == 255) {
        row[i] = solid;
        continue;
      }
      // Monochrome, premultiplied: every channel carries the same value.
      uint32_t d = row[i];
      uint32_t na = a + ((d >> 24) * inv + 127) / 255;
      uint32_t nv = v + ((d & 0xFF) * inv + 127) / 255;
      row[i] = (na << 24) | (nv << 16) | (nv << 8) | nv;
    }
  }
}

void Canvas::Frame(int x, int y, int fw, int fh, Ink c) {
  Fill(x, y, fw, 1, c);
  Fill(x, y + fh - 1, fw, 1, c);
  Fill(x, y + 1, 1, fh - 2, c);
  Fill(x + fw - 1, y + 1, 1, fh - 2, c);
}

void Canvas::Dotted(int x, int y, int dw, Ink c, int step) {
  for (int i = 0; i < dw; i += step) Pixel(x + i, y, c);
}

void Canvas::Icon(int x, int y, const char* rows, Ink c) {
  const Glyph& g = IconGlyph(rows);
  for (int r = 0; r < g.h; r++)
    for (int col = 0; col < g.w; col++)
      if (g.bits[r] & (1u << col)) Fill(x + col, y + r, 1, 1, c);
}

int Canvas::Text(int x, int y, std::string_view s, FontId f, Ink c) {
  const Prepared& p = Prepare(s, f);
  if (p.fallback) {
    for (int yy = 0; yy < p.fbH; yy++)
      for (int xx = 0; xx < p.fbW; xx++)
        if (p.fb[(size_t)yy * p.fbW + xx]) Fill(x + xx, y + yy + p.fbDy, 1, 1, c);
    return p.width;
  }
  int cx = x;
  for (const Glyph* g : p.glyphs) {
    for (int r = 0; r < g->h; r++) {
      uint32_t bits = g->bits[r];
      for (int col = 0; bits;) {  // one Fill per horizontal run
        if (!(bits & 1)) {
          bits >>= 1;
          col++;
          continue;
        }
        int run = 0;
        while (bits & 1) {
          bits >>= 1;
          run++;
        }
        Fill(cx + col, y + r, run, 1, c);
        col += run;
      }
    }
    cx += g->w + 1;
  }
  return p.width;
}

int Canvas::Led(int x, int y, std::string_view text, int pitch, Ink on, Ink off) {
  const GlyphMap& font = Font();
  int dot = std::max(1, pitch - 1), cx = x;
  for (char ch : text) {
    auto g = font.find((char32_t)(unsigned char)ch);
    if (g == font.end()) continue;
    const Glyph& gl = g->second;
    for (int r = 0; r < 7; r++)
      for (int col = 0; col < gl.w; col++)
        Fill(cx + col * pitch, y + r * pitch, dot, dot, (gl.bits[r] >> col) & 1 ? on : off);
    cx += (gl.w + 1) * pitch;
  }
  return std::max(0, cx - x - pitch);
}

void Canvas::LedIcon(int x, int y, const char* rows, int pitch, Ink on, Ink off) {
  const Glyph& g = IconGlyph(rows);
  int dot = std::max(1, pitch - 1);
  for (int r = 0; r < g.h; r++)
    for (int col = 0; col < g.w; col++)
      Fill(x + col * pitch, y + r * pitch, dot, dot, (g.bits[r] >> col) & 1 ? on : off);
}

int TextWidth(std::string_view s, FontId f) { return Prepare(s, f).width; }

int LedWidth(std::string_view text, int pitch) {
  const GlyphMap& font = Font();
  int w = 0;
  for (char ch : text) {
    auto g = font.find((char32_t)(unsigned char)ch);
    if (g != font.end()) w += (g->second.w + 1) * pitch;
  }
  return std::max(0, w - pitch);
}

std::string FitText(std::string_view s, FontId f, int maxW) {
  const Prepared& p = Prepare(s, f);
  if (p.width <= maxW) return std::string(s);
  static const char kEllipsis[] = "\xE2\x80\xA6";
  int ew = TextWidth(kEllipsis, f);
  if (!p.fallback) {
    int w = 0;
    size_t cut = 0;
    for (size_t i = 0; i < p.glyphs.size(); i++) {
      int gw = p.glyphs[i]->w + 1;
      if (w + gw + ew > maxW) break;
      w += gw;
      cut = p.srcEnd[i];
    }
    std::string out(s.substr(0, cut));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + kEllipsis;
  }
  std::wstring w = Widen(s);
  size_t n = (size_t)((double)w.size() * maxW / std::max(1, p.width));
  for (; n > 0; n--) {
    std::string cand = Narrow(std::wstring_view(w).substr(0, n)) + kEllipsis;
    if (TextWidth(cand, f) <= maxW) return cand;
  }
  return kEllipsis;
}
