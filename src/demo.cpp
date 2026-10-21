// --demo: made-up data for every widget, so screenshots show the column fully working
// without anyone's real messages, calendar or sessions. It arrives in stages over the
// first second and a bit, the way real data does, so a --record of it shows the launch
// cascade and the springs too. The clock is pinned to a fixed afternoon so images repeat.
#include "pixelbar.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int64_t kMin = 60000, kHour = 60 * kMin, kDay = 24 * kHour;
constexpr uint64_t kMB = 1ull << 20, kGB = 1ull << 30;

int64_t LocalMs(int y, int mo, int d, int h, int mi, int s) {
  SYSTEMTIME lt{(WORD)y, (WORD)mo, 0, (WORD)d, (WORD)h, (WORD)mi, (WORD)s, 0}, utc{};
  TzSpecificLocalTimeToSystemTime(nullptr, &lt, &utc);
  FILETIME ft;
  SystemTimeToFileTime(&utc, &ft);
  return FileTimeToUnixMs(ft);
}

// A ringed planet, Atkinson-dithered to 1 bit like real covers are.
std::vector<uint8_t> Cover(int n) {
  std::vector<float> g((size_t)n * n);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      double fx = (x + 0.5) / n * 2 - 1, fy = (y + 0.5) / n * 2 - 1, v = 0;
      double px = fx + 0.04, py = fy - 0.02, r2 = px * px + py * py, R = 0.52;
      bool planet = r2 < R * R;
      if (planet) {
        double z = std::sqrt(R * R - r2) / R;
        v = std::clamp(0.08 + 0.95 * (-0.45 * px / R - 0.55 * py / R + 0.7 * z), 0.0, 1.0);
      }
      double a = 0.38, rx = fx * std::cos(a) + fy * std::sin(a), ry = (-fx * std::sin(a) + fy * std::cos(a)) * 3.4;
      double rr = std::sqrt(rx * rx + ry * ry);
      if (rr > 0.7 && rr < 0.92 && (ry > 0 || !planet)) v = std::max(v, 0.82 - 0.5 * std::abs(rr - 0.81) / 0.11);
      uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
      if (v == 0 && h % 89 == 0) v = 1;  // stars
      g[(size_t)y * n + x] = (float)v;
    }
  std::vector<uint8_t> out((size_t)n * n);
  auto add = [&](int x, int y, float e) {
    if (x >= 0 && x < n && y < n) g[(size_t)y * n + x] += e;
  };
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      float v = g[(size_t)y * n + x];
      bool on = v > 0.5f;
      out[(size_t)y * n + x] = on;
      float e = (v - (on ? 1.f : 0.f)) / 8;
      add(x + 1, y, e), add(x + 2, y, e), add(x - 1, y + 1, e), add(x, y + 1, e), add(x + 1, y + 1, e), add(x, y + 2, e);
    }
  return out;
}

// Smooth made-up history, oldest first.
std::vector<float> Wave(int n, double base, double amp, double seed) {
  std::vector<float> v(n);
  for (int i = 0; i < n; i++) {
    double t = i + seed * 37;
    v[i] = (float)std::max(0.0, base + amp * (0.55 * std::sin(t * 0.07) + 0.3 * std::sin(t * 0.23 + seed) +
                                             0.15 * std::sin(t * 0.61 + 2 * seed)));
  }
  return v;
}

}  // namespace

int64_t DemoFill(int64_t t, MediaState& media, AudioState& audio, SysState& sys, std::vector<Agent>& agents,
                 UsageState& usage, WeatherState& weather, CalendarState& calendar, SlackState& slack, AiToday& ai) {
  const int64_t now = LocalMs(2026, 10, 21, 14, 32, 5) + t, today = LocalMs(2026, 10, 21, 0, 0, 0);
  auto at = [&](int day, int h, int m) { return today + day * kDay + h * kHour + m * kMin; };
  const double sec = t / 1000.0;

  media = MediaState{};
  if (t >= 150) {
    static const std::vector<uint8_t> cover = Cover(56);
    media.present = media.playing = true;
    media.title = "Neon Static";
    media.artist = "Low Orbit";
    media.album = "Signal Fade";
    media.app = "SPOTIFY";
    media.durMs = 3 * kMin + 56000;
    media.posMs = 1 * kMin + 42000 + t;
    media.posStampMs = 0;
    media.canPrev = media.canNext = media.canPlayPause = media.canSeek = true;
    media.art = cover;
    media.artSize = 56;
  }
  audio.ok = true;
  audio.muted = false;
  audio.volume = 0.64f;
  audio.peak = (float)(0.45 + 0.35 * std::sin(sec * 9.1) * std::sin(sec * 3.7 + 1));

  agents.clear();
  if (t >= 100) {
    auto add = [&](const char* title, const char* detail, AgentState s, bool codex, int64_t since) {
      Agent a;
      a.key = title;
      a.title = title;
      a.detail = detail;
      a.state = s;
      a.codex = codex;
      a.sinceMs = now - since;
      agents.push_back(std::move(a));
    };
    add("pixelbar", "Bash  build.ps1 -Run", AgentState::NeedsYou, false, 12000);
    add("portfolio-site", "Edit  src/app/projects/page.tsx", AgentState::Working, false, 4 * kMin);
    add("fpga-synth", "run  make sim TOP=voice", AgentState::Working, true, 11 * kMin);
    add("ray-tracer", "BVH in; the test scene renders in 6 s, down from 41 s", AgentState::Done, false, 18 * kMin);
    add("course-notes", "Summarized lecture 12 into notes/ece-12.md", AgentState::Done, true, 34 * kMin);
  }

  usage = UsageState{};
  usage.claude = {{300, 42, now + 2 * kHour + 13 * kMin}, {10080, 18, now + 4 * kDay}};
  usage.codex = {{300, 9, now + 3 * kHour}, {10080, 27, now + 5 * kDay}};
  usage.claudeAt = usage.codexAt = now - 2 * kMin;

  ai = AiToday{};
  ai.scanning = t < 1150;
  if (!ai.scanning) {
    ai.claude = {23, 412, 3100000, 186000000, 512000};
    ai.codex = {9, 188, 1400000, 24600000, 133000};
    const float claude[24] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 52, 71, 48, 12, 63, 40};
    const float codex[24] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 21, 33, 0, 26, 12};
    for (int h = 0; h < 24; h++) {
      ai.claudeHours[h] = claude[h] * 1000;
      ai.codexHours[h] = codex[h] * 1000;
    }
  }

  weather = WeatherState{};
  if (t >= 380) {
    weather.ok = true;
    weather.fetchedAt = now;
    weather.temp = 11, weather.feels = 9, weather.wind = 14, weather.humidity = 84, weather.code = 61;
    const float temps[8] = {11, 11, 10, 10, 9, 9, 8, 8};
    const int codes[8] = {61, 61, 51, 3, 3, 2, 2, 3};
    for (int i = 0; i < 8; i++) weather.hours.push_back({at(0, 14 + i, 0), temps[i], codes[i], 40, 14 + i < 18});
    const float hi[6] = {12, 13, 14, 15, 13, 12}, lo[6] = {7, 8, 8, 9, 9, 7};
    const int dcodes[6] = {61, 3, 2, 0, 63, 3};
    for (int i = 0; i < 6; i++) weather.days.push_back({at(i, 0, 0), hi[i], lo[i], dcodes[i], 30});
  }

  calendar = CalendarState{};
  calendar.configured = true;
  if (t >= 640) {
    calendar.ok = true;
    calendar.fetchedAt = now;
    calendar.events = {{"Deep work: pixel font", at(0, 14, 0), at(0, 15, 0)},
                       {"Design review", at(0, 15, 0), at(0, 16, 0)},
                       {"Climbing", at(0, 17, 30), at(0, 19, 0)},
                       {"FPGA lab", at(1, 9, 0), at(1, 12, 0)},
                       {"Coffee with Sam", at(1, 12, 30), at(1, 13, 0)},
                       {"Hackathon kickoff", at(2, 18, 0), at(2, 21, 0)}};
  }

  slack = SlackState{};
  slack.configured = true;
  if (t >= 900) {
    slack.ok = true;
    slack.fetchedAt = now;
    slack.msgs = {{"#launch", "maya", "shipped! the new onboarding flow is live", "", false, false, now - 6 * kMin},
                  {"@alex", "you", "yep, right after the review", "", true, true, now - 21 * kMin},
                  {"@alex", "alex", "can you look at the pixel font PR?", "", true, false, now - 24 * kMin},
                  {"#design", "kim", "new icon set is up in Figma", "", false, false, now - 65 * kMin},
                  {"#general", "sam", "lunch at 1?", "", false, false, now - 2 * kHour}};
  }

  const int n = 300;
  sys.cpuName = "i9-9900K";
  sys.netKind = "ETH";
  sys.threads = 16;
  sys.cpuHist = Wave(n, 0.24, 0.16, 1 + sec * 0.05);
  sys.cpu = sys.cpuHist.back();
  sys.cores.resize(16);
  for (int i = 0; i < 16; i++)
    sys.cores[i] = (float)std::clamp(0.25 + 0.22 * std::sin(i * 1.7 + sec * 2.3) + 0.12 * std::sin(i * 0.6 - sec), 0.02, 1.0);
  sys.gpuOk = true;
  sys.gpuName = "RTX 2080 SUPER";
  sys.gpuHist = Wave(n, 0.34, 0.18, 4 + sec * 0.05);
  sys.gpu = sys.gpuHist.back();
  sys.gpuTemp = 56, sys.gpuFan = 38, sys.gpuWatts = 112;
  sys.vramUsed = 3 * kGB + 100 * kMB, sys.vramTotal = 8 * kGB;
  sys.ramUsed = 18 * kGB + 410 * kMB, sys.ramTotal = 32 * kGB - 100 * kMB;
  sys.disks = {{"C:", 931 * kGB, 312 * kGB}, {"D:", 1863 * kGB, 1020 * kGB}};
  sys.downHist = Wave(n, 1.6e6, 1.3e6, 7 + sec * 0.05);
  sys.upHist = Wave(n, 2.2e5, 1.8e5, 9 + sec * 0.05);
  sys.down = sys.downHist.back(), sys.up = sys.upHist.back();
  sys.pingMs = 8;
  sys.online = true;
  sys.uptimeMs = 3 * kDay + 6 * kHour + 11 * kMin;
  sys.procs.clear();
  if (t >= 250)
    sys.procs = {{"chrome", 38, 3100 * kMB, 0.024f}, {"Code", 12, 1400 * kMB, 0.011f}, {"Spotify", 7, 420 * kMB, 0.004f},
                 {"Discord", 6, 380 * kMB, 0.003f},  {"obs64", 1, 310 * kMB, 0.019f}};
  sys.selfCpu = 0.0012f;
  sys.selfMem = 27 * kMB + 600 * 1024;
  return now;
}
