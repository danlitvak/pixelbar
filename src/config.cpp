#include "pixelbar.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

Config g_cfg;
static std::mutex g_cfgMu;

Config ConfigCopy() {
  std::lock_guard lk(g_cfgMu);
  return g_cfg;
}

void ConfigCommit(const Config& c) {
  std::lock_guard lk(g_cfgMu);
  g_cfg = c;
}

const char* const kSectionNames[] = {"clock", "year",  "media",   "agents", "limits",   "ai",
                                     "calendar", "slack", "weather", "system", "processes"};
const int kSectionCount = (int)std::size(kSectionNames);

std::wstring ConfigPath() { return ExeDir() + L"\\config.json"; }

bool LoadConfig(Config& c) {
  std::wstring path = ConfigPath();
  std::string text;
  if (!ReadAll(path, text) || text.empty()) {
    SaveConfig(Config{});
    c = Config{};
    return true;
  }
  JVal j;
  if (!JParse(text, j) || !j.isObj()) return false;

  Config d;
  auto num = [](const JVal& v, int def, int lo, int hi) {
    return v.t == JVal::Num ? std::clamp((int)v.n, lo, hi) : def;
  };
  auto flag = [](const JVal& v, bool def) { return v.t == JVal::Bool ? v.b : def; };

  if (j["monitor"].t == JVal::Str) d.monitor = j["monitor"].s;
  if (j["monitor"].t == JVal::Num) d.monitor = std::to_string((int)j["monitor"].n);
  d.scale = num(j["scale"], d.scale, 0, 8);
  d.width = num(j["width"], d.width, 200, 800);
  d.marginX = num(j["marginX"], d.marginX, 0, 2000);
  d.marginY = num(j["marginY"], d.marginY, 0, 2000);
  std::string_view va = j["valign"].str();
  d.valign = va == "center" ? 1 : va == "bottom" ? 2 : 0;
  d.reserve = flag(j["reserve"], d.reserve);
  if (j["sections"].isArr()) {
    d.sections.clear();
    for (auto& s : j["sections"].a) {
      std::string name = s.s == "music" ? "media" : s.s;  // renamed
      bool known = std::find(kSectionNames, kSectionNames + kSectionCount, name) != kSectionNames + kSectionCount;
      if (s.t == JVal::Str && known && std::find(d.sections.begin(), d.sections.end(), name) == d.sections.end())
        d.sections.push_back(name);
    }
  }
  const JVal& clock = j["clock"];
  d.clock24h = flag(clock["24h"], flag(j["clock24h"], d.clock24h));
  d.clockSeconds = flag(clock["seconds"], d.clockSeconds);
  d.clockScale = num(clock["dotPitch"], d.clockScale, 3, 6);
  if (j["pingHost"].t == JVal::Str) d.pingHost = j["pingHost"].s;
  const JVal& ag = j["agents"];
  d.agentMax = num(ag["max"], d.agentMax, 1, 20);
  d.doneMinutes = num(ag["doneMinutes"], d.doneMinutes, 0, 24 * 60);
  d.staleMinutes = num(ag["staleMinutes"], d.staleMinutes, 1, 24 * 60);
  d.claude = flag(ag["claude"], d.claude);
  d.codex = flag(ag["codex"], d.codex);
  const JVal& w = j["weather"];
  if (w["place"].t == JVal::Str) d.place = w["place"].s;
  if (w["lat"].t == JVal::Num) d.lat = std::clamp(w["lat"].n, -90.0, 90.0);
  if (w["lon"].t == JVal::Num) d.lon = std::clamp(w["lon"].n, -180.0, 180.0);
  d.fahrenheit = flag(w["fahrenheit"], d.fahrenheit);
  const JVal& cal = j["calendar"];
  d.calDays = num(cal["days"], d.calDays, 1, 14);
  d.calMax = num(cal["max"], d.calMax, 1, 12);
  d.slackMax = num(j["slack"]["max"], d.slackMax, 1, 12);
  const JVal& pr = j["processes"];
  d.procRows = num(pr["max"], d.procRows, 1, 12);
  d.procByCpu = pr["sort"].str() == "cpu";
  d.gpu = flag(j["gpu"], d.gpu);
  d.fps = num(j["fps"], d.fps, 4, 60);
  const JVal& ink = j["ink"];
  d.dim = num(ink["dim"], d.dim, 1, 255);
  d.faint = num(ink["faint"], d.faint, 1, 255);
  d.background = num(ink["background"], d.background, 0, 255);
  c = d;
  return true;
}

bool SaveConfig(const Config& c) {
  std::string sections;
  for (size_t i = 0; i < c.sections.size(); i++) sections += (i ? ", " : "") + JQuote(c.sections[i]);
  static const char* kAlign[] = {"top", "center", "bottom"};
  char buf[4096];
  snprintf(buf, sizeof buf,
           "{\n"
           "  \"monitor\": %s,\n"
           "  \"scale\": %d,\n"
           "  \"width\": %d,\n"
           "  \"marginX\": %d,\n"
           "  \"marginY\": %d,\n"
           "  \"valign\": \"%s\",\n"
           "  \"reserve\": %s,\n"
           "  \"sections\": [%s],\n"
           "  \"clock\": { \"24h\": %s, \"seconds\": %s, \"dotPitch\": %d },\n"
           "  \"agents\": { \"max\": %d, \"doneMinutes\": %d, \"staleMinutes\": %d, \"claude\": %s, \"codex\": %s },\n"
           "  \"weather\": { \"place\": %s, \"lat\": %.4f, \"lon\": %.4f, \"fahrenheit\": %s },\n"
           "  \"calendar\": { \"days\": %d, \"max\": %d },\n"
           "  \"slack\": { \"max\": %d },\n"
           "  \"processes\": { \"max\": %d, \"sort\": \"%s\" },\n"
           "  \"pingHost\": %s,\n"
           "  \"gpu\": %s,\n"
           "  \"fps\": %d,\n"
           "  \"ink\": { \"dim\": %d, \"faint\": %d, \"background\": %d }\n"
           "}\n",
           JQuote(c.monitor).c_str(), c.scale, c.width, c.marginX, c.marginY, kAlign[std::clamp(c.valign, 0, 2)],
           c.reserve ? "true" : "false", sections.c_str(), c.clock24h ? "true" : "false", c.clockSeconds ? "true" : "false", c.clockScale,
           c.agentMax, c.doneMinutes, c.staleMinutes, c.claude ? "true" : "false", c.codex ? "true" : "false",
           JQuote(c.place).c_str(), c.lat, c.lon, c.fahrenheit ? "true" : "false", c.calDays, c.calMax, c.slackMax,
           c.procRows, c.procByCpu ? "cpu" : "ram", JQuote(c.pingHost).c_str(), c.gpu ? "true" : "false", c.fps,
           c.dim, c.faint, c.background);
  return WriteAll(ConfigPath(), buf);
}
