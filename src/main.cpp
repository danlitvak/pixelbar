// Window, message loop, input and z-order. The column is a per-pixel-alpha layered tool
// window that keeps itself just above the desktop: under every app window, over the
// wallpaper, and still there after Win+D.
#include "pixelbar.h"

#include <dwmapi.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")

namespace {

const wchar_t kClass[] = L"PixelbarWindow";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunName[] = L"pixelbar";

constexpr UINT WM_PB_APPBAR = WM_APP + 3;  // shell notifications for the reserved strip
constexpr UINT WM_PB_FRAME = WM_APP + 4;   // the frame pump: draw now, a refresh just began

enum { TIMER_TICK = 1, TIMER_ANIM, TIMER_ZORDER, TIMER_ZORDER_SOON, TIMER_DUMP };
enum { CMD_SETTINGS = 1, CMD_EDIT, CMD_RELOAD, CMD_AUTOSTART, CMD_EXIT };

struct App {
  HWND hwnd = nullptr;
  HDC memDC = nullptr;
  HBITMAP dib = nullptr;
  HGDIOBJ oldBmp = nullptr;
  uint32_t* bits = nullptr;
  Canvas canvas;                   // the window's pixels
  std::vector<uint32_t> layoutPx;  // the flat layout RenderUI draws, which Compose moves onto canvas
  Canvas layout;
  int layoutRows = 0;
  RECT work{};
  UiOutput ui;
  int hoverId = HIT_NONE, hoverIndex = -1;
  int pressId = HIT_NONE, pressIndex = -1;
  bool tracking = false, renderQueued = false, animOn = false;
  bool pumping = false;  // motion running: the frame pump draws every refresh
  int animInterval = 0;
  MediaState media;
  AudioState audio;
  SysState sys;
  std::vector<Agent> agents;
  UsageState usage;
  WeatherState weather;
  CalendarState calendar;
  SlackState slack;
  AiToday ai;
  bool settings = false, autostart = false, icalSet = false, slackSet = false;
  std::string flash;
  int64_t flashUntil = 0;
  FileStamp cfgStamp;
  UINT taskbarCreated = 0;
  bool appbar = false;  // registered with the shell as an app bar
  RECT barRect{};       // the strip it reserves
  std::wstring dumpPath;   // --dump <file.bmp>: render one frame to a file instead of the desktop
  std::wstring recordDir;  // --record <dir>: save every frame of the first seconds, motion included
  bool demo = false;       // --demo: made-up data in every widget, for screenshots
  int64_t startMs = 0;
} g;

// Dump and record runs draw offscreen and leave the live column (and its strip) alone.
bool Offscreen() { return !g.dumpPath.empty() || !g.recordDir.empty(); }

// Windows' "Animation effects" setting (Accessibility > Visual effects) turns motion off.
bool MotionOn() {
  BOOL on = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
  return on != FALSE;
}

// ---- frame pump -----------------------------------------------------------------
// While blocks move, a thread waits for each vertical blank of the column's monitor and asks
// the UI thread for one frame, so motion lands on every refresh instead of whenever a coarse
// timer or a data update fires. Faster panels get every other blank (60-75 fps is plenty).

HANDLE g_pumpOn = nullptr;  // manual-reset event, set while motion runs
std::atomic<HMONITOR> g_pumpMonitor{nullptr};
std::atomic<bool> g_framePending{false};

IDXGIOutput* OutputFor(IDXGIFactory1* f, HMONITOR m) {
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
    IDXGIOutput* o = nullptr;
    for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
      DXGI_OUTPUT_DESC d{};
      if (SUCCEEDED(o->GetDesc(&d)) && d.Monitor == m) {
        a->Release();
        return o;
      }
      o->Release();
    }
    a->Release();
  }
  return nullptr;
}

void PumpLoop(HWND hwnd) {
  IDXGIFactory1* factory = nullptr;
  CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
  IDXGIOutput* output = nullptr;
  HMONITOR outputMonitor = nullptr;
  int64_t lastPost = 0;
  for (;;) {
    WaitForSingleObject(g_pumpOn, INFINITE);
    HMONITOR want = g_pumpMonitor.load();
    if (want != outputMonitor) {
      if (output) output->Release();
      output = factory && want ? OutputFor(factory, want) : nullptr;
      outputMonitor = want;
    }
    int64_t t0 = NowMs();
    if (!output || FAILED(output->WaitForVBlank())) Sleep(16);
    if (NowMs() - t0 < 2) Sleep(14);  // no real blank to wait on (display asleep): don't spin
    int64_t t = NowMs();
    if (t - lastPost < 12) continue;
    lastPost = t;
    if (!g_framePending.exchange(true)) PostMessageW(hwnd, WM_PB_FRAME, 0, 0);
  }
}

// ---- placement ----------------------------------------------------------------

struct Mon {
  HMONITOR h;
  RECT rc, work;
  bool primary;
};

BOOL CALLBACK EnumMon(HMONITOR h, HDC, LPRECT, LPARAM p) {
  MONITORINFO mi{sizeof mi};
  if (GetMonitorInfoW(h, &mi))
    ((std::vector<Mon>*)p)->push_back({h, mi.rcMonitor, mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
  return TRUE;
}

const Mon* PickMonitor(const std::vector<Mon>& mons) {
  if (mons.empty()) return nullptr;
  std::vector<const Mon*> byX;
  for (auto& m : mons) byX.push_back(&m);
  std::sort(byX.begin(), byX.end(), [](const Mon* a, const Mon* b) {
    return a->rc.left != b->rc.left ? a->rc.left < b->rc.left : a->rc.top < b->rc.top;
  });
  const std::string& sel = g_cfg.monitor;
  if (sel == "rightmost") return byX.back();
  if (sel == "primary")
    for (auto m : byX)
      if (m->primary) return m;
  int idx = atoi(sel.c_str());
  if (idx >= 1 && idx <= (int)byX.size()) return byX[idx - 1];
  return byX.front();
}

void AppBarRemove() {
  if (!g.appbar) return;
  APPBARDATA abd{sizeof abd};
  abd.hWnd = g.hwnd;
  SHAppBarMessage(ABM_REMOVE, &abd);
  g.appbar = false;
  g.barRect = RECT{};
}

// Claims a full-height strip on the monitor's left edge, the way the taskbar claims its
// edge: maximized and snapped windows then stop at its right side.
RECT AppBarPlace(const RECT& mon, int width) {
  RECT want{mon.left, mon.top, mon.left + width, mon.bottom};
  APPBARDATA abd{sizeof abd};
  abd.hWnd = g.hwnd;
  if (!g.appbar) {
    abd.uCallbackMessage = WM_PB_APPBAR;
    if (!SHAppBarMessage(ABM_NEW, &abd)) return want;
    g.appbar = true;
  }
  abd.uEdge = ABE_LEFT;
  abd.rc = want;
  SHAppBarMessage(ABM_QUERYPOS, &abd);  // the shell moves it clear of the taskbar and other bars
  abd.rc.right = abd.rc.left + width;
  if (!EqualRect(&abd.rc, &g.barRect)) {
    SHAppBarMessage(ABM_SETPOS, &abd);
    g.barRect = abd.rc;
  }
  return g.barRect;
}

void Layout() {
  std::vector<Mon> mons;
  EnumDisplayMonitors(nullptr, nullptr, EnumMon, (LPARAM)&mons);
  const Mon* m = PickMonitor(mons);
  if (!m) return;
  g_pumpMonitor = m->h;
  UINT dx = 96, dy = 96;
  GetDpiForMonitor(m->h, MDT_EFFECTIVE_DPI, &dx, &dy);
  int scale = g_cfg.scale > 0 ? g_cfg.scale : std::max(1, (int)std::lround(dx / 96.0));
  MONITORINFO mi{sizeof mi};
  if (g_cfg.reserve && !Offscreen()) {
    RECT bar = AppBarPlace(m->rc, g_cfg.marginX * 2 + g_cfg.width * scale);
    // Our own strip is now outside the work area, so sit in the strip, clear of the taskbar.
    GetMonitorInfoW(m->h, &mi);
    g.work = RECT{bar.left, std::max(bar.top, mi.rcWork.top), bar.right, std::min(bar.bottom, mi.rcWork.bottom)};
  } else {
    AppBarRemove();
    GetMonitorInfoW(m->h, &mi);
    g.work = mi.rcWork;
  }
  int artW = g_cfg.width;
  int artH = std::max(64, (int)(g.work.bottom - g.work.top - 2 * g_cfg.marginY) / scale);

  if (!g.memDC) g.memDC = CreateCompatibleDC(nullptr);
  if (g.dib) {
    SelectObject(g.memDC, g.oldBmp);
    DeleteObject(g.dib);
    g.dib = nullptr;
    g.bits = nullptr;
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = artW * scale;
  bi.bmiHeader.biHeight = -(artH * scale);
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  g.dib = CreateDIBSection(g.memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!g.dib) return;
  g.bits = (uint32_t*)bits;
  g.oldBmp = SelectObject(g.memDC, g.dib);
  g.canvas.Reset(artW, artH, scale, g.bits);
  g.layoutPx.assign((size_t)artW * scale * artH * scale, 0);
  g.layout.Reset(artW, artH, scale, g.layoutPx.data());
  g.layoutRows = artH;
  g.ui = UiOutput{};
}

// ---- z-order: directly above the desktop window -------------------------------

bool IsDesktop(HWND h) {
  wchar_t cls[32] = {};
  GetClassNameW(h, cls, 32);
  if (wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0) return false;
  return FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
}

BOOL CALLBACK FindDesktopProc(HWND h, LPARAM p) {
  if (IsDesktop(h)) {
    *(HWND*)p = h;
    return FALSE;
  }
  return TRUE;
}

bool Covers(HWND h, const RECT& ours) {
  if (!IsWindowVisible(h) || IsIconic(h)) return false;
  if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) return false;
  int cloaked = 0;
  DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
  if (cloaked) return false;
  RECT r, i;
  return GetWindowRect(h, &r) && IntersectRect(&i, &r, &ours);
}

void KeepAboveDesktop() {
  HWND desk = nullptr;
  EnumWindows(FindDesktopProc, (LPARAM)&desk);
  if (!desk) desk = FindWindowW(L"Progman", nullptr);
  if (!desk) return;
  RECT ours;
  GetWindowRect(g.hwnd, &ours);
  bool wrong = false;
  // Hidden under the desktop (Win+D raises it)?
  for (HWND x = GetWindow(g.hwnd, GW_HWNDPREV); x && !wrong; x = GetWindow(x, GW_HWNDPREV)) wrong = x == desk;
  // Or sitting on top of an app window that overlaps us?
  for (HWND x = GetWindow(g.hwnd, GW_HWNDNEXT); x && x != desk && !wrong; x = GetWindow(x, GW_HWNDNEXT))
    wrong = Covers(x, ours);
  if (!wrong) return;
  HWND above = GetWindow(desk, GW_HWNDPREV);
  SetWindowPos(g.hwnd, above ? above : HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
}

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
  SetTimer(g.hwnd, TIMER_ZORDER_SOON, 150, nullptr);
}

// ---- drawing ------------------------------------------------------------------

// --record: the whole window as an 8-bit frame per render (premultiplied white over black is
// just the blue channel), named by ms since launch, plus one line per frame in frames.csv.
void RecordFrame(int64_t now) {
  int64_t t = now - g.startMs;
  if (t > 3000) return;
  int w = g.canvas.pw, h = g.canvas.ph, stride = (w + 3) & ~3;
  RGBQUAD pal[256];
  for (int i = 0; i < 256; i++) pal[i] = {(BYTE)i, (BYTE)i, (BYTE)i, 0};
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof ih;
  ih.biWidth = w;
  ih.biHeight = -h;
  ih.biPlanes = 1;
  ih.biBitCount = 8;
  ih.biCompression = BI_RGB;
  ih.biClrUsed = 256;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof fh + sizeof ih + sizeof pal;
  fh.bfSize = fh.bfOffBits + (DWORD)stride * h;
  std::vector<uint8_t> px((size_t)stride * h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) px[(size_t)y * stride + x] = (uint8_t)(g.bits[(size_t)y * w + x] & 0xFF);
  wchar_t name[32];
  swprintf(name, 32, L"\\f%05lld.bmp", (long long)t);
  HANDLE f = CreateFileW((g.recordDir + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD wrote;
  WriteFile(f, &fh, sizeof fh, &wrote, nullptr);
  WriteFile(f, &ih, sizeof ih, &wrote, nullptr);
  WriteFile(f, pal, sizeof pal, &wrote, nullptr);
  WriteFile(f, px.data(), (DWORD)px.size(), &wrote, nullptr);
  CloseHandle(f);
  char line[64];
  int len = snprintf(line, sizeof line, "%lld,%d,%d,%d\n", (long long)t, g.ui.top, g.ui.height, g.ui.motion ? 1 : 0);
  HANDLE log = CreateFileW((g.recordDir + L"\\frames.csv").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) return;
  WriteFile(log, line, (DWORD)len, &wrote, nullptr);
  CloseHandle(log);
}

void Render() {
  g.renderQueued = false;
  if (!g.bits) return;
  int64_t now = NowMs(), shown = now;
  if (g.demo) {
    shown = DemoFill(now - g.startMs, g.media, g.audio, g.sys, g.agents, g.usage, g.weather, g.calendar, g.slack, g.ai);
  } else {
    g.media = MediaSnapshot();
    g.agents = AgentsSnapshot();
    g.usage = UsageSnapshot();
    g.weather = WeatherSnapshot();
    g.calendar = CalendarSnapshot();
    g.slack = SlackSnapshot();
    g.ai = TodaySnapshot();
    AudioPoll(g.audio);
  }
  if (g.flashUntil && now > g.flashUntil) {
    g.flash.clear();
    g.flashUntil = 0;
  }
  UiInput in;
  in.now = shown;
  in.media = &g.media;
  in.audio = &g.audio;
  in.sys = &g.sys;
  in.agents = &g.agents;
  in.usage = &g.usage;
  in.weather = &g.weather;
  in.calendar = &g.calendar;
  in.slack = &g.slack;
  in.ai = &g.ai;
  in.hoverId = g.hoverId;
  in.hoverIndex = g.hoverIndex;
  in.hovering = g.tracking;
  in.settings = g.settings;
  in.autostart = g.autostart;
  in.icalSet = g.icalSet;
  in.slackSet = g.slackSet;
  in.flash = g.flash;
  g.layout.Clear(g.layoutRows + 16);  // rows below were never drawn
  UiOutput out;
  RenderUI(g.layout, in, out);
  g.layoutRows = out.rows;
  Compose(g.layout, g.canvas, out, now, !g.dumpPath.empty() || !MotionOn());

  // The window keeps its full height and the column moves inside it; empty pixels pass
  // clicks through to the desktop.
  SIZE size{g.canvas.pw, g.canvas.ph};
  POINT dst{g.work.left + g_cfg.marginX, g.work.top + g_cfg.marginY};
  POINT src{0, 0};
  BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(g.hwnd, nullptr, &dst, &size, g.memDC, &src, 0, &bf, ULW_ALPHA);
  g.ui = std::move(out);
  if (!g.recordDir.empty()) RecordFrame(now);

  // Motion draws on every refresh through the pump; content animates at the configured rate.
  if (g.ui.motion != g.pumping) {
    g.pumping = g.ui.motion;
    if (g.pumping)
      SetEvent(g_pumpOn);
    else
      ResetEvent(g_pumpOn);
  }
  int interval = std::max(15, 1000 / g_cfg.fps);
  bool anim = !g.pumping && (g.ui.animating || g.flashUntil);
  if (anim && (!g.animOn || g.animInterval != interval)) {
    SetTimer(g.hwnd, TIMER_ANIM, interval, nullptr);
    g.animOn = true;
    g.animInterval = interval;
  } else if (!anim && g.animOn) {
    KillTimer(g.hwnd, TIMER_ANIM);
    g.animOn = false;
  }
}

void RequestRender() {
  if (g.renderQueued || g.pumping) return;  // while pumping, the next refresh draws it anyway
  g.renderQueued = true;
  PostMessageW(g.hwnd, WM_PB_RENDER, 0, 0);
}

// Writes the column as it would look on a black desktop, for checking layout offscreen.
void Dump() {
  Render();
  int w = g.canvas.pw, h = std::max(1, g.ui.height) * g.canvas.s;
  const uint32_t* rows = g.bits + (size_t)g.ui.top * g.canvas.s * w;
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof ih;
  ih.biWidth = w;
  ih.biHeight = -h;
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof fh + sizeof ih;
  fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
  std::vector<uint32_t> px((size_t)w * h);
  for (size_t i = 0; i < px.size(); i++) px[i] = rows[i] | 0xFF000000u;  // premultiplied over black
  HANDLE f = CreateFileW(g.dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD wrote;
  WriteFile(f, &fh, sizeof fh, &wrote, nullptr);
  WriteFile(f, &ih, sizeof ih, &wrote, nullptr);
  WriteFile(f, px.data(), (DWORD)(px.size() * 4), &wrote, nullptr);
  CloseHandle(f);
}

void ScheduleTick() {
  // Land just after each wall-clock second so the clock turns over on time.
  int64_t now = NowMs();
  SetTimer(g.hwnd, TIMER_TICK, (UINT)(1000 - now % 1000 + 5), nullptr);
}

// ---- settings -----------------------------------------------------------------

bool AutostartEnabled() {
  wchar_t buf[MAX_PATH * 2];
  DWORD size = sizeof buf;
  return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
  if (on) {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    std::wstring v = L"\"" + std::wstring(exe, n) + L"\"";
    RegSetValueExW(k, kRunName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(k, kRunName);
  }
  RegCloseKey(k);
}

bool HasSecret(const wchar_t* name) {
  std::string v;
  bool set = SecretGet(name, v);
  SecureZeroMemory(v.data(), v.size());
  return set;
}

void RefreshSettingsState() {
  g.autostart = AutostartEnabled();
  g.icalSet = HasSecret(kSecretIcal);
  g.slackSet = HasSecret(kSecretSlack);
}

void Flash(std::string text) {
  g.flash = std::move(text);
  g.flashUntil = NowMs() + 6000;
}

void ApplyConfigChange() {
  SaveConfig(g_cfg);
  Stat(ConfigPath(), g.cfgStamp);
  Layout();
  AgentsRescan();
  OnlineKick();
}

std::string ReadClipboardText() {
  std::string out;
  if (!OpenClipboard(g.hwnd)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (auto* p = (const wchar_t*)GlobalLock(h)) {
      out = Narrow(p);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  size_t a = out.find_first_not_of(" \t\r\n"), b = out.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

void ClearClipboard() {
  if (!OpenClipboard(g.hwnd)) return;
  EmptyClipboard();
  CloseClipboard();
}

// The link and token never pass through a text field: copy them, click PASTE.
void PasteSecret(int which) {
  std::string v = ReadClipboardText();
  bool ok = false;
  if (which == 0) {
    bool web = v.rfind("https://", 0) == 0 || v.rfind("webcal://", 0) == 0;
    ok = web && v.find(' ') == std::string::npos &&
         (v.find(".ics") != std::string::npos || v.find("/ical/") != std::string::npos);
    if (ok) ok = SecretSet(kSecretIcal, v);
    Flash(ok ? "ICAL LINK SAVED  CLIPBOARD CLEARED" : "THE CLIPBOARD HAS NO ICAL LINK");
  } else {
    ok = (v.rfind("xoxp-", 0) == 0 || v.rfind("xoxb-", 0) == 0 || v.rfind("xoxe.xoxp-", 0) == 0) &&
         v.find(' ') == std::string::npos;
    if (ok) ok = SecretSet(kSecretSlack, v);
    Flash(ok ? "SLACK TOKEN SAVED  CLIPBOARD CLEARED" : "THE CLIPBOARD HAS NO SLACK TOKEN (XOXP-...)");
  }
  SecureZeroMemory(v.data(), v.size());
  if (ok) {
    ClearClipboard();
    OnlineKick();
  }
  RefreshSettingsState();
}

void ReloadConfig() {
  Stat(ConfigPath(), g.cfgStamp);
  Config c;
  if (!LoadConfig(c)) return;
  ConfigCommit(c);
  Layout();
  AgentsRescan();
  OnlineKick();
  RequestRender();
}

// ---- input --------------------------------------------------------------------

const Hit* HitAt(int px, int py) {
  int s = std::max(1, g.canvas.s), x = px / s, y = py / s;
  for (auto it = g.ui.hits.rbegin(); it != g.ui.hits.rend(); ++it)
    if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return &*it;
  return nullptr;
}

void SetVolumeFromX(int px) {
  RECT r = g.ui.volBar;
  if (r.right <= r.left) return;
  float f = (px / (float)g.canvas.s - r.left) / (float)(r.right - r.left);
  AudioSetVolume(std::clamp(f, 0.f, 1.f));
  RequestRender();
}

void Open(const std::wstring& target) {
  AllowSetForegroundWindow(ASFW_ANY);
  ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OnClick(const Hit& h, int px) {
  switch (h.id) {
    case HIT_PREV: MediaCommand(MediaCmd::Prev); break;
    case HIT_PLAY: MediaCommand(MediaCmd::PlayPause); break;
    case HIT_NEXT: MediaCommand(MediaCmd::Next); break;
    case HIT_SEEK: {
      RECT r = g.ui.seekBar;
      double f = std::clamp((px / (double)g.canvas.s - r.left) / std::max<LONG>(1, r.right - r.left), 0.0, 1.0);
      MediaCommand(MediaCmd::Seek, (int64_t)(f * g.media.durMs));
      break;
    }
    case HIT_MUTE: AudioToggleMute(); break;
    case HIT_AGENT:
      if (h.index >= 0 && h.index < (int)g.agents.size() && !g.agents[h.index].link.empty())
        Open(g.agents[h.index].link);
      break;
    case HIT_SLACK:
      if (h.index >= 0 && h.index < (int)g.slack.msgs.size() && !g.slack.msgs[h.index].link.empty())
        Open(Widen(g.slack.msgs[h.index].link));
      break;
    case HIT_OPEN_SETTINGS:
      g.settings = true;
      RefreshSettingsState();
      break;
    case HIT_S_DONE: g.settings = false; break;
    case HIT_S_AUTOSTART:
      SetAutostart(!AutostartEnabled());
      RefreshSettingsState();
      break;
    case HIT_S_PASTE: PasteSecret(h.index); break;
    case HIT_S_CLEAR:
      SecretClear(h.index == 0 ? kSecretIcal : kSecretSlack);
      Flash(h.index == 0 ? "ICAL LINK REMOVED" : "SLACK TOKEN REMOVED");
      RefreshSettingsState();
      OnlineKick();
      break;
    case HIT_S_CONFIG: Open(ConfigPath()); break;
    default:
      if (ApplySettingsHit(h)) ApplyConfigChange();
  }
  RequestRender();
}

void ShowMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"pixelbar");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, CMD_SETTINGS, L"Settings");
  AppendMenuW(m, MF_STRING, CMD_EDIT, L"Edit config file");
  AppendMenuW(m, MF_STRING, CMD_RELOAD, L"Reload");
  AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), CMD_AUTOSTART, L"Start with Windows");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, CMD_EXIT, L"Exit");
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g.hwnd);  // so the menu closes when you click elsewhere
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g.hwnd, nullptr);
  PostMessageW(g.hwnd, WM_NULL, 0, 0);
  DestroyMenu(m);
  switch (cmd) {
    case CMD_SETTINGS:
      g.settings = true;
      RefreshSettingsState();
      RequestRender();
      break;
    case CMD_EDIT: Open(ConfigPath()); break;
    case CMD_RELOAD: ReloadConfig(); break;
    case CMD_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
    case CMD_EXIT: DestroyWindow(g.hwnd); break;
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_TIMER:
      switch (wp) {
        case TIMER_TICK: {
          if (!g.demo) SysSample(g.sys);
          FileStamp st;
          if (Stat(ConfigPath(), st) && !(st == g.cfgStamp)) ReloadConfig();
          ScheduleTick();
          if (!g.pumping) Render();
          break;
        }
        case TIMER_ANIM: Render(); break;
        case TIMER_ZORDER: KeepAboveDesktop(); break;
        case TIMER_ZORDER_SOON:
          KillTimer(hwnd, TIMER_ZORDER_SOON);
          KeepAboveDesktop();
          break;
        case TIMER_DUMP:
          if (!g.dumpPath.empty()) Dump();
          DestroyWindow(hwnd);
          break;
      }
      return 0;
    case WM_PB_REFRESH: RequestRender(); return 0;
    case WM_PB_RENDER: Render(); return 0;
    case WM_PB_FRAME:
      g_framePending = false;
      if (g.pumping) Render();
      return 0;
    case WM_PB_APPBAR:
      if (wp == ABN_POSCHANGED) {  // the taskbar or another bar moved: re-claim the strip
        Layout();
        Render();
      }
      return 0;
    case WM_WINDOWPOSCHANGED:
      if (g.appbar) {
        APPBARDATA abd{sizeof abd};
        abd.hWnd = hwnd;
        SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
      }
      break;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
      if (!g.tracking) {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        g.tracking = true;
        RequestRender();
      }
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (g.pressId == HIT_VOL && GetCapture() == hwnd) SetVolumeFromX(x);
      const Hit* h = HitAt(x, y);
      int id = h ? h->id : HIT_NONE, idx = h ? h->index : -1;
      if (id != g.hoverId || idx != g.hoverIndex) {
        g.hoverId = id;
        g.hoverIndex = idx;
        RequestRender();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g.tracking = false;
      g.hoverId = HIT_NONE;
      g.hoverIndex = -1;
      RequestRender();
      return 0;
    case WM_SETCURSOR: {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(hwnd, &pt);
      const Hit* h = HitAt(pt.x, pt.y);
      SetCursor(LoadCursorW(nullptr, h && h->id != HIT_MUSIC ? IDC_HAND : IDC_ARROW));
      return TRUE;
    }
    case WM_LBUTTONDOWN: {
      const Hit* h = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      g.pressId = h ? h->id : HIT_NONE;
      g.pressIndex = h ? h->index : -1;
      if (g.pressId == HIT_VOL) {
        SetCapture(hwnd);
        SetVolumeFromX(GET_X_LPARAM(lp));
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      bool dragged = GetCapture() == hwnd;
      if (dragged) ReleaseCapture();
      const Hit* h = HitAt(x, y);
      if (!dragged && h && h->id == g.pressId && h->index == g.pressIndex) {
        Hit copy = *h;  // OnClick re-renders, which replaces the hit list
        OnClick(copy, x);
      }
      g.pressId = HIT_NONE;
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &pt);
      const Hit* h = HitAt(pt.x, pt.y);
      bool musicArea = h && (h->id == HIT_MUSIC || h->id == HIT_VOL || h->id == HIT_MUTE || h->id == HIT_PREV ||
                             h->id == HIT_PLAY || h->id == HIT_NEXT || h->id == HIT_SEEK);
      if (musicArea && g.audio.ok) {
        int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (notches == 0) notches = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        AudioSetVolume(g.audio.volume + 0.02f * notches);
        RequestRender();
      }
      return 0;
    }
    case WM_RBUTTONUP: ShowMenu(); return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      Layout();
      Render();
      KeepAboveDesktop();
      return 0;
    case WM_SETTINGCHANGE:
      if (wp == SPI_SETWORKAREA) {
        Layout();
        Render();
      }
      return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_ENDSESSION:
      if (wp) AppBarRemove();
      return 0;
    case WM_DESTROY:
      AppBarRemove();  // hand the strip back to other windows
      PostQuitMessage(0);
      return 0;
  }
  if (g.taskbarCreated && msg == g.taskbarCreated) {
    // Explorer restarted: the desktop window is new and app bar registrations are gone.
    g.appbar = false;
    g.barRect = RECT{};
    Layout();
    Render();
    KeepAboveDesktop();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// Closes a running copy so a rebuilt exe (or a second launch) takes over cleanly.
void CloseExisting() {
  HWND old = FindWindowW(kClass, nullptr);
  if (!old) return;
  DWORD pid = 0;
  GetWindowThreadProcessId(old, &pid);
  HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
  PostMessageW(old, WM_CLOSE, 0, 0);
  if (ph) {
    WaitForSingleObject(ph, 5000);
    CloseHandle(ph);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::wstring args = cmdLine ? cmdLine : L"";
  bool dumpSettings = args.find(L"--settings") != std::wstring::npos;
  auto pathArg = [&](const wchar_t* flag) {
    std::wstring v;
    if (size_t d = args.find(flag); d != std::wstring::npos) {
      v = args.substr(d + wcslen(flag));
      if (size_t end = v.find(L" --"); end != std::wstring::npos) v.resize(end);
      v.erase(std::remove(v.begin(), v.end(), L'"'), v.end());
      v.erase(v.find_last_not_of(L' ') + 1);  // launchers may leave a trailing space
    }
    return v;
  };
  g.dumpPath = pathArg(L"--dump ");
  g.recordDir = pathArg(L"--record ");
  g.demo = args.find(L"--demo") != std::wstring::npos;
  g.startMs = NowMs();
  if (!Offscreen()) CloseExisting();
  if (args.find(L"--quit") != std::wstring::npos) return 0;
  if (args.find(L"--autostart-on") != std::wstring::npos) SetAutostart(true);
  if (args.find(L"--autostart-off") != std::wstring::npos) SetAutostart(false);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Config cfg;
  if (LoadConfig(cfg)) ConfigCommit(cfg);
  Stat(ConfigPath(), g.cfgStamp);

  // An offscreen run gets its own class, so a live copy starting meanwhile can't close it.
  const wchar_t* cls = Offscreen() ? L"PixelbarOffscreen" : kClass;
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = cls;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  g.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"pixelbar", WS_POPUP, 0, 0, 1,
                           1, nullptr, nullptr, inst, nullptr);
  if (!g.hwnd) return 1;
  g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  g_pumpOn = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread(PumpLoop, g.hwnd).detach();

  Layout();
  if (!g.demo) {  // the demo touches no logs, network or devices
    SysInit();
    SysSample(g.sys);
    MediaStart(g.hwnd, 56);
    AgentsStart(g.hwnd);
    OnlineStart(g.hwnd);
    TodayStart(g.hwnd);
  }
  if (dumpSettings) {
    g.settings = true;
    RefreshSettingsState();
  }
  Render();
  if (!Offscreen()) {
    ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    KeepAboveDesktop();
  } else {
    SetTimer(g.hwnd, TIMER_DUMP, g.dumpPath.empty() ? 3100 : 6000, nullptr);
  }

  ScheduleTick();
  SetTimer(g.hwnd, TIMER_ZORDER, 750, nullptr);
  HWINEVENTHOOK fg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  if (fg) UnhookWinEvent(fg);
  ExitProcess(0);  // worker threads are detached; they end with the process
}
