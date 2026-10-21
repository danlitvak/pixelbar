// pixelbar: a white-on-black pixel status column for the Windows desktop.
// Shared declarations for every translation unit.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr UINT WM_PB_REFRESH = WM_APP + 1;  // a worker thread has new data
constexpr UINT WM_PB_RENDER = WM_APP + 2;   // coalesced repaint request

// ---- util.cpp ---------------------------------------------------------------

std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);
int64_t NowMs();  // unix epoch, milliseconds
int64_t FileTimeToUnixMs(const FILETIME& ft);
int64_t ParseIsoMs(std::string_view s);  // "2026-10-20T13:45:58.570Z" -> unix ms, 0 if malformed
std::wstring ExeDir();
std::wstring EnvDir(const wchar_t* name);
bool ReadHead(const std::wstring& path, size_t max, std::string& out);
bool ReadTail(const std::wstring& path, size_t max, std::string& out, uint64_t* fileSize = nullptr);
bool ReadAll(const std::wstring& path, std::string& out);
bool ReadRange(const std::wstring& path, uint64_t offset, size_t len, std::string& out);
bool WriteAll(const std::wstring& path, std::string_view data);
std::string Leaf(std::string_view path);
std::string OneLine(std::string_view s, size_t maxBytes);  // collapse whitespace, drop markdown marks
std::string FormatAge(int64_t ms);                         // 12s, 4m, 1h05, 2d
std::string FormatBytes(uint64_t bytes);                   // 512 MB, 4.2 GB
void DebugLog(std::string_view line);                      // only with PIXELBAR_DEBUG set
std::string UrlEncode(std::string_view s);
SYSTEMTIME LocalTime(int64_t unixMs);

struct FileStamp {
  int64_t mtime = 0;
  uint64_t size = 0;
  bool operator==(const FileStamp&) const = default;
};
bool Stat(const std::wstring& path, FileStamp& st);

template <class F>
void ListDir(const std::wstring& dir, const wchar_t* pattern, F&& fn) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((dir + L"\\" + pattern).c_str(), FindExInfoBasic, &fd,
                              FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const wchar_t* n = fd.cFileName;
    if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
    fn(fd);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

// ---- json.cpp ---------------------------------------------------------------

struct JVal {
  enum Type : uint8_t { Null, Bool, Num, Str, Arr, Obj };
  Type t = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<JVal> a;
  std::vector<std::pair<std::string, JVal>> o;

  const JVal& operator[](std::string_view key) const;  // null value when missing
  const JVal& at(size_t i) const;
  std::string_view str() const { return t == Str ? std::string_view(s) : std::string_view(); }
  double num(double def = 0) const { return t == Num ? n : def; }
  bool flag(bool def = false) const { return t == Bool ? b : def; }
  bool isObj() const { return t == Obj; }
  bool isArr() const { return t == Arr; }
};
bool JParse(std::string_view text, JVal& out);
// Reads the JSON string value of the first `"key":` in a (possibly truncated) document.
bool JFindString(std::string_view text, std::string_view key, std::string& out);
bool JFindNumber(std::string_view text, std::string_view key, double& out);
bool JFindBool(std::string_view text, std::string_view key, bool& out);
std::string JQuote(std::string_view s);  // JSON string literal, quotes included

// ---- config.cpp -------------------------------------------------------------

struct Config {
  std::string monitor = "leftmost";  // leftmost | rightmost | primary | 1..n (left to right)
  int scale = 0;                     // physical px per art px; 0 = from monitor DPI
  int width = 300;                   // art px
  int marginX = 24, marginY = 28;    // physical px from the work-area edge
  int valign = 0;                    // 0 top, 1 center, 2 bottom
  bool reserve = true;               // claim the strip as an app bar so windows maximize beside it
  std::vector<std::string> sections{"clock",    "year",  "media",   "agents", "limits",   "ai",
                                    "calendar", "slack", "weather", "system", "processes"};
  bool clock24h = true, clockSeconds = true;
  int clockScale = 4;  // px between the clock's dots
  std::string pingHost = "1.1.1.1";
  int agentMax = 6, doneMinutes = 45, staleMinutes = 15;
  bool claude = true, codex = true;
  bool gpu = true;
  std::string place = "Vancouver";
  double lat = 49.2827, lon = -123.1207;
  bool fahrenheit = false;
  int calDays = 3, calMax = 6;
  int slackMax = 5;
  int procRows = 5;
  bool procByCpu = false;
  int fps = 15;
  int dim = 150, faint = 56, background = 1;  // alpha of white on the black desktop
};
extern Config g_cfg;  // written only on the UI thread; other threads read ConfigCopy()
extern const char* const kSectionNames[];
extern const int kSectionCount;
std::wstring ConfigPath();
bool LoadConfig(Config& c);
bool SaveConfig(const Config& c);
Config ConfigCopy();
void ConfigCommit(const Config& c);

// ---- canvas.cpp -------------------------------------------------------------

struct Ink {
  uint8_t lum;  // 255 white, 0 black
  uint8_t a;
};
enum class FontId { Text, Caps };  // the 5x7 pixel font; Caps draws lowercase as capitals

struct Glyph {
  uint8_t w = 0, h = 0;
  uint16_t bits[16] = {};  // bit x of row y
};

struct Canvas {
  int w = 0, h = 0, s = 1;  // art-pixel size and physical px per art px
  int pw = 0, ph = 0;       // physical size
  uint32_t* px = nullptr;   // premultiplied BGRA, top-down, pw * ph
  RECT clip{};

  void Reset(int artW, int artH, int scale, uint32_t* pixels);
  void Clear(int artRows);            // zero the top artRows rows
  void ClearRows(int from, int to);   // zero art rows [from, to)
  void Backfill(int from, int to, Ink c);  // paint c under every still-empty pixel of art rows [from, to)
  void Pixel(int x, int y, Ink c) { Fill(x, y, 1, 1, c); }
  void Fill(int x, int y, int w, int h, Ink c);
  void Frame(int x, int y, int w, int h, Ink c);
  void Dotted(int x, int y, int w, Ink c, int step = 2);
  void Icon(int x, int y, const char* rows, Ink c);
  int Text(int x, int y, std::string_view utf8, FontId f, Ink c);
  // Dot-matrix rendering of the 5x7 font: each cell a dot x dot square on a `pitch` grid,
  // unlit cells drawn with `off` so every character sits on a visible panel.
  int Led(int x, int y, std::string_view text, int pitch, Ink on, Ink off);
  void LedIcon(int x, int y, const char* rows, int pitch, Ink on, Ink off);
  void SetClip(int x, int y, int w, int h);
  void ResetClip() { clip = RECT{0, 0, w, h}; }
};
int TextWidth(std::string_view utf8, FontId f);
int LedWidth(std::string_view text, int pitch);
std::string FitText(std::string_view utf8, FontId f, int maxW);  // trims with an ellipsis
const Glyph& IconGlyph(const char* rows);
void TextCacheTrim();
constexpr int kCap = 7;    // cap height of the text font
constexpr int kLine = 10;  // line pitch, descenders included

// ---- media.cpp (SMTC on its own MTA thread) ---------------------------------

struct MediaState {
  bool present = false, playing = false;
  std::string title, artist, album, app;
  int64_t posMs = 0, durMs = 0, posStampMs = 0;  // position measured at posStampMs
  bool canPrev = false, canNext = false, canPlayPause = false, canSeek = false;
  std::vector<uint8_t> art;  // artSize * artSize, 1 = lit
  int artSize = 0;
  uint64_t artVersion = 0;
};
enum class MediaCmd { PlayPause, Next, Prev, Seek };
void MediaStart(HWND notify, int artSize);
MediaState MediaSnapshot();
void MediaCommand(MediaCmd c, int64_t arg = 0);

// ---- audio.cpp (Core Audio, UI thread) --------------------------------------

struct AudioState {
  bool ok = false, muted = false;
  float volume = 0, peak = 0;
};
void AudioPoll(AudioState& st);
void AudioSetVolume(float v);
void AudioToggleMute();

// ---- sysinfo.cpp ------------------------------------------------------------

struct DiskInfo {
  std::string root;
  uint64_t total = 0, free = 0;
};
struct ProcInfo {
  std::string name;
  int count = 0;
  uint64_t mem = 0;  // private working set, summed over the group
  float cpu = 0;     // share of the whole machine
};
struct SysState {
  std::string cpuName, netKind;
  int threads = 0;
  float cpu = 0, gpu = 0;
  bool gpuOk = false;
  std::string gpuName;
  int gpuTemp = -1, gpuFan = -1, gpuWatts = -1;
  uint64_t vramUsed = 0, vramTotal = 0;
  std::vector<float> cores;
  std::vector<float> cpuHist, gpuHist, downHist, upHist;  // oldest first
  uint64_t ramUsed = 0, ramTotal = 0;
  std::vector<DiskInfo> disks;
  std::vector<ProcInfo> procs;  // every process name, heaviest memory first
  double down = 0, up = 0;      // bytes per second
  int pingMs = -1;
  bool online = false;
  int64_t uptimeMs = 0;
  float selfCpu = 0;     // pixelbar itself, measured like the process list: share of the machine
  uint64_t selfMem = 0;  // and private working set
};
void SysInit();
void SysSample(SysState& st);

// ---- agents.cpp -------------------------------------------------------------

enum class AgentState { NeedsYou, Asks, Working, Stale, Done, Stopped };
struct Agent {
  std::string key, title, detail;
  bool codex = false;
  AgentState state = AgentState::Done;
  int64_t sinceMs = 0, lastWriteMs = 0;
  std::wstring link;  // deep link that opens the session in its app
};
struct LimitWindow {
  int minutes = 0;      // 300 = five hours, 10080 = a week
  float pct = 0;        // used, 0-100
  int64_t resetsAt = 0;  // unix ms, 0 if unknown
};
struct UsageState {
  std::vector<LimitWindow> claude, codex;
  int64_t claudeAt = 0, codexAt = 0;  // when each was measured
};
void AgentsStart(HWND notify);
std::vector<Agent> AgentsSnapshot();
UsageState UsageSnapshot();
void AgentsRescan();

// ---- today.cpp (today's AI usage, summed from the same logs) ----------------

struct AiSide {
  int turns = 0, calls = 0;              // prompts you sent, model requests made
  uint64_t in = 0, cached = 0, out = 0;  // tokens: input not read from cache, cache reads, output
  uint64_t Total() const { return in + cached + out; }
};
struct AiToday {
  AiSide claude, codex;
  float claudeHours[24] = {}, codexHours[24] = {};  // output tokens by local hour
  int64_t dayStart = 0;
  bool scanning = true;  // still reading back through today's logs
};
void TodayStart(HWND notify);
AiToday TodaySnapshot();

// ---- online.cpp (weather, calendar, Slack; one worker thread) ---------------

struct WeatherHour {
  int64_t at = 0;
  float temp = 0;
  int code = 0, rainPct = 0;
  bool day = true;
};
struct WeatherDay {
  int64_t at = 0;
  float hi = 0, lo = 0;
  int code = 0, rainPct = 0;
};
struct WeatherState {
  bool ok = false;
  std::string error;
  float temp = 0, feels = 0, wind = 0;
  int code = 0, humidity = 0;
  bool day = true;
  std::vector<WeatherHour> hours;
  std::vector<WeatherDay> days;
  int64_t fetchedAt = 0;
};
struct CalEvent {
  std::string title;
  int64_t start = 0, end = 0;
  bool allDay = false;
};
struct CalendarState {
  bool configured = false, ok = false;
  std::string error;
  std::vector<CalEvent> events;  // upcoming and running, soonest first
  int64_t fetchedAt = 0;
};
struct SlackMsg {
  std::string where, who, text, link;  // where: #channel, @person for a DM, or GROUP
  bool dm = false, mine = false;
  int64_t at = 0;
};
struct SlackState {
  bool configured = false, ok = false;
  std::string error;
  std::vector<SlackMsg> msgs;  // newest first
  int64_t fetchedAt = 0;
};
void OnlineStart(HWND notify);
void OnlineKick();  // refetch everything now (settings or secrets changed)
WeatherState WeatherSnapshot();
CalendarState CalendarSnapshot();
SlackState SlackSnapshot();
// Secrets live in Windows Credential Manager, never in config.json.
bool SecretGet(const wchar_t* name, std::string& out);
bool SecretSet(const wchar_t* name, const std::string& value);
void SecretClear(const wchar_t* name);
constexpr const wchar_t* kSecretIcal = L"pixelbar:ical";
constexpr const wchar_t* kSecretSlack = L"pixelbar:slack";

// ---- demo.cpp ---------------------------------------------------------------

// --demo: overwrites every widget's state with made-up data, staged over the first second
// after launch (t = ms since then); returns the pinned "now" the widgets should show.
int64_t DemoFill(int64_t t, MediaState& media, AudioState& audio, SysState& sys, std::vector<Agent>& agents,
                 UsageState& usage, WeatherState& weather, CalendarState& calendar, SlackState& slack, AiToday& ai);

// ---- ui.cpp -----------------------------------------------------------------

enum HitId {
  HIT_NONE,
  HIT_PREV, HIT_PLAY, HIT_NEXT, HIT_SEEK, HIT_VOL, HIT_MUTE, HIT_MUSIC,
  HIT_AGENT, HIT_SLACK, HIT_PROC_SORT, HIT_OPEN_SETTINGS,
  // settings panel
  HIT_S_DONE, HIT_S_TOGGLE, HIT_S_UP, HIT_S_DOWN, HIT_S_DEC, HIT_S_INC,
  HIT_S_PASTE, HIT_S_CLEAR, HIT_S_AUTOSTART, HIT_S_CONFIG,
};
struct Hit {
  RECT r;  // art px
  int id;
  int index;
};
struct UiInput {
  int64_t now = 0;
  const MediaState* media = nullptr;
  const AudioState* audio = nullptr;
  const SysState* sys = nullptr;
  const std::vector<Agent>* agents = nullptr;
  const UsageState* usage = nullptr;
  const WeatherState* weather = nullptr;
  const CalendarState* calendar = nullptr;
  const SlackState* slack = nullptr;
  const AiToday* ai = nullptr;
  int hoverId = HIT_NONE, hoverIndex = -1;
  bool hovering = false;  // pointer anywhere over the column
  bool settings = false, autostart = false, icalSet = false, slackSet = false;
  std::string flash;  // one-line confirmation shown in the settings panel
};
// Rows of the layout that move and fade as one: a section, the settings footer, or the whole
// settings panel.
struct Block {
  std::string key;
  int y0 = 0, y1 = 0;          // its rows in the layout canvas
  int head = 0;                // header rows, which stay put when the body fades in afresh
  int gap = 0;                 // empty rows above it
  size_t mode = 0;             // what kind of content it shows; a change fades the body in again
  size_t hit0 = 0, hit1 = 0;   // its range of UiOutput::hits
};
struct UiOutput {
  int rows = 0;                // layout canvas rows RenderUI drew
  int top = 0, height = 0;     // window canvas rows the column occupies (set by Compose)
  bool animating = false;      // content animates (spinner, marquee): redraw at the configured fps
  bool motion = false;         // blocks moving or fading: redraw every frame
  std::vector<Hit> hits;
  std::vector<Block> blocks;
  RECT seekBar{}, volBar{};
};
// Lays the column out flat, top-aligned, on `c`, as out.blocks.
void RenderUI(Canvas& c, const UiInput& in, UiOutput& out);
// motion.cpp: stacks the blocks onto the window canvas from the valign edge, easing height
// changes and dithering new content in; shifts the hits to match. `still` skips all motion.
void Compose(const Canvas& layout, Canvas& win, UiOutput& out, int64_t now, bool still);
// Applies a settings-panel click to g_cfg; true when the config changed and should be saved.
bool ApplySettingsHit(const Hit& h);
