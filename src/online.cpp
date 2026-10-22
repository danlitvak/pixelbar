// Weather (open-meteo), calendar (a private iCal link) and Slack (search API), fetched on
// one background thread. The iCal link and Slack token are secrets: they live in Windows
// Credential Manager, never in config.json.
#include "pixelbar.h"

#include <winhttp.h>
#include <wincred.h>
#include <icu.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "icu.lib")

// ---- secrets ----------------------------------------------------------------

bool SecretGet(const wchar_t* name, std::string& out) {
  PCREDENTIALW cred = nullptr;
  if (!CredReadW(name, CRED_TYPE_GENERIC, 0, &cred)) return false;
  out.assign((const char*)cred->CredentialBlob, cred->CredentialBlobSize);
  CredFree(cred);
  return !out.empty();
}

bool SecretSet(const wchar_t* name, const std::string& value) {
  CREDENTIALW c{};
  c.Type = CRED_TYPE_GENERIC;
  c.TargetName = const_cast<LPWSTR>(name);
  c.UserName = const_cast<LPWSTR>(L"pixelbar");
  c.CredentialBlobSize = (DWORD)value.size();
  c.CredentialBlob = (LPBYTE)value.data();
  c.Persist = CRED_PERSIST_LOCAL_MACHINE;
  return CredWriteW(&c, 0) != 0;
}

void SecretClear(const wchar_t* name) { CredDeleteW(name, CRED_TYPE_GENERIC, 0); }

namespace {

// ---- http -------------------------------------------------------------------

struct HttpResult {
  DWORD status = 0;
  std::string body, error;
};

HttpResult HttpGet(const std::string& url, const std::wstring& headers = L"") {
  HttpResult r;
  std::wstring wurl = Widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
    r.error = "bad link";
    return r;
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) + std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  HINTERNET s = WinHttpOpen(L"pixelbar/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) {
    r.error = "no network stack";
    return r;
  }
  // No WINHTTP_OPTION_DECOMPRESSION: with it, reads of gzip replies failed with E_ABORT here,
  // and every reply this fetches is small enough to take uncompressed.
  WinHttpSetTimeouts(s, 5000, 8000, 10000, 20000);
  HINTERNET c = WinHttpConnect(s, host.c_str(), uc.nPort, 0);
  HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                  : nullptr;
  if (q && WinHttpSendRequest(q, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1L,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(q, nullptr)) {
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = status;
    std::vector<char> buf(1 << 16);
    for (;;) {
      DWORD got = 0;
      if (!WinHttpReadData(q, buf.data(), (DWORD)buf.size(), &got)) {
        DebugLog("http read failed, error " + std::to_string(GetLastError()));
        break;
      }
      if (got == 0) break;
      r.body.append(buf.data(), got);
      if (r.body.size() > ((size_t)48 << 20)) break;
    }
    if (r.body.empty()) {
      wchar_t raw[4096];
      DWORD rawLen = sizeof raw;
      if (WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw, &rawLen,
                              WINHTTP_NO_HEADER_INDEX))
        DebugLog("empty body, headers: " + Narrow(std::wstring_view(raw, rawLen / sizeof(wchar_t))));
    }
  } else {
    r.error = "offline";
  }
  if (q) WinHttpCloseHandle(q);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  if (r.error.empty() && r.status != 200) r.error = "HTTP " + std::to_string(r.status);
  return r;
}

// ---- civil dates ------------------------------------------------------------

int64_t Days(int64_t y, int m, int d) {  // days since 1970-01-01
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void Civil(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y = (int)(yoe + era * 400 + (m <= 2));
}

int Weekday(int64_t days) { return (int)(((days % 7) + 7 + 4) % 7); }  // 0 = Sunday

int DaysInMonth(int y, int m) { return (int)(Days(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - Days(y, m, 1)); }

// ---- weather ------------------------------------------------------------------

std::mutex g_mu;
HWND g_hwnd = nullptr;
HANDLE g_kick = nullptr;
WeatherState g_weather;
CalendarState g_calendar;
SlackState g_slack;

void FetchWeather(const Config& cfg) {
  char url[1024];
  snprintf(url, sizeof url,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
           "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=auto&forecast_days=7&timeformat=unixtime%s",
           cfg.lat, cfg.lon, cfg.fahrenheit ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
  HttpResult r = HttpGet(url);
  JVal j;
  WeatherState w;
  if (r.error.empty() && JParse(r.body, j) && j["current"].isObj()) {
    const JVal& cur = j["current"];
    w.ok = true;
    w.temp = (float)cur["temperature_2m"].num();
    w.feels = (float)cur["apparent_temperature"].num();
    w.humidity = (int)cur["relative_humidity_2m"].num();
    w.code = (int)cur["weather_code"].num();
    w.wind = (float)cur["wind_speed_10m"].num();
    w.day = cur["is_day"].num(1) != 0;
    const JVal& h = j["hourly"];
    int64_t now = NowMs();
    for (size_t i = 0; i < h["time"].a.size() && w.hours.size() < 24; i++) {
      int64_t at = (int64_t)h["time"].a[i].num() * 1000;
      if (at < now - 3600 * 1000) continue;
      w.hours.push_back({at, (float)h["temperature_2m"].at(i).num(), (int)h["weather_code"].at(i).num(),
                         (int)h["precipitation_probability"].at(i).num(), h["is_day"].at(i).num(1) != 0});
    }
    const JVal& d = j["daily"];
    for (size_t i = 0; i < d["time"].a.size() && w.days.size() < 7; i++)
      w.days.push_back({(int64_t)d["time"].a[i].num() * 1000, (float)d["temperature_2m_max"].at(i).num(),
                        (float)d["temperature_2m_min"].at(i).num(), (int)d["weather_code"].at(i).num(),
                        (int)d["precipitation_probability_max"].at(i).num()});
    w.fetchedAt = now;
  }
  if (!w.ok)
    DebugLog("weather: status " + std::to_string(r.status) + " error '" + r.error + "' bytes " +
             std::to_string(r.body.size()) + " head " + r.body.substr(0, 120));
  std::lock_guard lk(g_mu);
  if (w.ok) {
    g_weather = std::move(w);
  } else {
    g_weather.error = r.error.empty() ? "unreadable reply" : r.error;  // keep showing the last forecast
  }
}

// ---- calendar (iCalendar, RFC 5545) -------------------------------------------

struct Wall {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
};
struct IcsTime {
  Wall w;
  bool date = false, utc = false, ok = false;
  std::string tz;
};

std::string Param(const std::string& params, const char* key) {
  std::string k = std::string(key) + "=";
  size_t p = 0;
  while ((p = params.find(k, p)) != std::string::npos) {
    if (p == 0 || params[p - 1] == ';') {
      size_t v = p + k.size(), e = params.find(';', v);
      std::string out = params.substr(v, e == std::string::npos ? std::string::npos : e - v);
      if (out.size() >= 2 && out.front() == '"' && out.back() == '"') out = out.substr(1, out.size() - 2);
      return out;
    }
    p += k.size();
  }
  return {};
}

IcsTime ParseTime(const std::string& value, const std::string& params) {
  IcsTime t;
  t.tz = Param(params, "TZID");
  if (value.size() < 8) return t;
  t.w.y = atoi(value.substr(0, 4).c_str());
  t.w.mo = atoi(value.substr(4, 2).c_str());
  t.w.d = atoi(value.substr(6, 2).c_str());
  if (value.size() >= 15 && value[8] == 'T') {
    t.w.h = atoi(value.substr(9, 2).c_str());
    t.w.mi = atoi(value.substr(11, 2).c_str());
    t.w.s = atoi(value.substr(13, 2).c_str());
    t.utc = value.size() >= 16 && value[15] == 'Z';
  } else {
    t.date = true;
  }
  t.ok = t.w.y > 1900 && t.w.mo >= 1 && t.w.mo <= 12 && t.w.d >= 1 && t.w.d <= 31;
  return t;
}

int64_t UtcToUnix(const Wall& w) { return ((Days(w.y, w.mo, w.d) * 86400) + w.h * 3600 + w.mi * 60 + w.s) * 1000; }

// Floating times and all-day dates are this PC's wall clock (ICU's default zone came out
// as UTC here, so Windows does this conversion).
int64_t LocalToUnix(const Wall& w) {
  SYSTEMTIME local{(WORD)w.y, (WORD)w.mo, 0, (WORD)w.d, (WORD)w.h, (WORD)w.mi, (WORD)w.s, 0}, utc{};
  if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return UtcToUnix(w);
  return UtcToUnix({utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond});
}

// Wall-clock time in an IANA zone -> unix ms, through the ICU that ships with Windows.
int64_t ToUnix(const Wall& w, const std::string& tz, bool utc) {
  if (utc) return UtcToUnix(w);
  if (tz.empty()) return LocalToUnix(w);
  static std::map<std::string, UCalendar*> cache;  // nullptr: a zone ICU doesn't know
  auto it = cache.find(tz);
  if (it == cache.end()) {
    UErrorCode st = U_ZERO_ERROR;
    std::u16string zone(tz.begin(), tz.end());
    UChar canon[128];
    UBool sys = false;
    ucal_getCanonicalTimeZoneID((const UChar*)zone.c_str(), (int32_t)zone.size(), canon, 128, &sys, &st);
    UCalendar* opened = nullptr;
    if (U_SUCCESS(st)) {
      st = U_ZERO_ERROR;
      opened = ucal_open((const UChar*)zone.c_str(), (int32_t)zone.size(), nullptr, UCAL_GREGORIAN, &st);
      if (U_FAILURE(st)) opened = nullptr;
    }
    it = cache.emplace(tz, opened).first;
  }
  UCalendar* cal = it->second;
  if (!cal) return LocalToUnix(w);
  UErrorCode st = U_ZERO_ERROR;
  ucal_clear(cal);
  ucal_setDateTime(cal, w.y, w.mo - 1, w.d, w.h, w.mi, w.s, &st);
  UDate ms = ucal_getMillis(cal, &st);
  return U_FAILURE(st) ? 0 : (int64_t)ms;
}

int64_t ToUnix(const IcsTime& t) { return ToUnix(t.w, t.date ? std::string() : t.tz, t.utc && !t.date); }

std::string Unescape(const std::string& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); i++) {
    if (v[i] == '\\' && i + 1 < v.size()) {
      char n = v[++i];
      out += (n == 'n' || n == 'N') ? ' ' : n;
    } else {
      out += v[i];
    }
  }
  return out;
}

int64_t ParseDuration(const std::string& v) {  // P1DT2H30M, PT45M, P1W
  int64_t ms = 0, n = 0;
  bool neg = !v.empty() && v[0] == '-';
  for (char ch : v) {
    if (ch >= '0' && ch <= '9') {
      n = n * 10 + (ch - '0');
      continue;
    }
    switch (ch) {
      case 'W': ms += n * 7 * 86400000LL; break;
      case 'D': ms += n * 86400000LL; break;
      case 'H': ms += n * 3600000LL; break;
      case 'M': ms += n * 60000LL; break;
      case 'S': ms += n * 1000LL; break;
    }
    n = 0;
  }
  return neg ? -ms : ms;
}

struct Rule {
  int freq = 0;  // 0 none, 1 daily, 2 weekly, 3 monthly, 4 yearly
  int interval = 1, count = -1;
  bool hasUntil = false;
  IcsTime until;
  std::vector<std::pair<int, int>> byday;  // (ordinal, weekday 0 = Sunday)
  std::vector<int> bymonthday, bymonth;
};

Rule ParseRule(const std::string& v) {
  Rule r;
  static const char* kDays[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  size_t p = 0;
  while (p < v.size()) {
    size_t e = v.find(';', p);
    std::string part = v.substr(p, e == std::string::npos ? std::string::npos : e - p);
    p = e == std::string::npos ? v.size() : e + 1;
    size_t eq = part.find('=');
    if (eq == std::string::npos) continue;
    std::string k = part.substr(0, eq), val = part.substr(eq + 1);
    if (k == "FREQ") r.freq = val == "DAILY" ? 1 : val == "WEEKLY" ? 2 : val == "MONTHLY" ? 3 : val == "YEARLY" ? 4 : -1;
    else if (k == "INTERVAL") r.interval = std::max(1, atoi(val.c_str()));
    else if (k == "COUNT") r.count = atoi(val.c_str());
    else if (k == "UNTIL") {
      r.until = ParseTime(val, "");
      r.hasUntil = r.until.ok;
    } else if (k == "BYDAY" || k == "BYMONTHDAY" || k == "BYMONTH") {
      size_t q = 0;
      while (q < val.size()) {
        size_t c = val.find(',', q);
        std::string item = val.substr(q, c == std::string::npos ? std::string::npos : c - q);
        q = c == std::string::npos ? val.size() : c + 1;
        if (k == "BYDAY" && item.size() >= 2) {
          std::string wd = item.substr(item.size() - 2);
          int ord = item.size() > 2 ? atoi(item.substr(0, item.size() - 2).c_str()) : 0;
          for (int i = 0; i < 7; i++)
            if (wd == kDays[i]) r.byday.push_back({ord, i});
        } else if (k == "BYMONTHDAY") {
          r.bymonthday.push_back(atoi(item.c_str()));
        } else {
          r.bymonth.push_back(atoi(item.c_str()));
        }
      }
    }
  }
  return r;
}

struct Vevent {
  std::string uid, summary;
  IcsTime start, end, recurrenceId;
  int64_t duration = -1;
  bool cancelled = false, hasRule = false;
  Rule rule;
  std::vector<int64_t> exdates;
};

// Every occurrence of `ev` overlapping [from, to), as (start, end).
void Expand(const Vevent& ev, int64_t from, int64_t to, const std::set<std::string>& overridden,
            std::vector<CalEvent>& out) {
  int64_t first = ToUnix(ev.start);
  int64_t len = ev.duration >= 0 ? ev.duration
                : ev.end.ok        ? ToUnix(ev.end) - first
                : ev.start.date    ? 86400000LL
                                   : 0;
  auto emit = [&](int64_t s) {
    if (std::find(ev.exdates.begin(), ev.exdates.end(), s) != ev.exdates.end()) return;
    if (overridden.count(ev.uid + "|" + std::to_string(s))) return;
    if (s + std::max<int64_t>(len, 1) <= from || s >= to) return;
    out.push_back({ev.summary, s, s + len, ev.start.date});
  };
  if (!ev.hasRule || ev.rule.freq <= 0) {
    emit(first);
    return;
  }
  const Rule& r = ev.rule;
  int64_t until = r.hasUntil ? ToUnix(r.until) + (r.until.date ? 86400000LL - 1 : 0) : INT64_MAX;
  int64_t d0 = Days(ev.start.w.y, ev.start.w.mo, ev.start.w.d);
  int64_t lastDay = (to / 86400000LL) + 2;
  int produced = 0;
  // One candidate day; false stops the expansion.
  auto day = [&](int64_t dd) {
    if (dd < d0) return true;
    if (dd > lastDay) return false;
    Wall w = ev.start.w;
    Civil(dd, w.y, w.mo, w.d);
    int64_t s = ToUnix(w, ev.start.date ? std::string() : ev.start.tz, ev.start.utc && !ev.start.date);
    if (s > until) return false;
    if (r.count >= 0 && produced >= r.count) return false;
    produced++;
    emit(s);
    return true;
  };
  for (int step = 0; step < 20000; step++) {
    if (r.freq == 1) {
      if (!day(d0 + (int64_t)step * r.interval)) return;
    } else if (r.freq == 2) {
      int64_t weekStart = d0 - (Weekday(d0) + 6) % 7 + (int64_t)step * 7 * r.interval;  // weeks start Monday
      std::vector<int> days;
      if (r.byday.empty()) days.push_back(Weekday(d0));
      for (auto& bd : r.byday) days.push_back(bd.second);
      std::sort(days.begin(), days.end(), [](int a, int b) { return (a + 6) % 7 < (b + 6) % 7; });
      for (int wd : days)
        if (!day(weekStart + (wd + 6) % 7)) return;
    } else if (r.freq == 3) {
      int months = ev.start.w.mo - 1 + step * r.interval;
      int y = ev.start.w.y + months / 12, m = months % 12 + 1, dim = DaysInMonth(y, m);
      std::vector<int> mdays;
      for (auto& bd : r.byday) {
        if (bd.first == 0) continue;
        int firstWd = Weekday(Days(y, m, 1));
        int firstMatch = 1 + (bd.second - firstWd + 7) % 7;
        int count = (dim - firstMatch) / 7 + 1;
        int nth = bd.first > 0 ? bd.first : count + bd.first + 1;
        if (nth >= 1 && nth <= count) mdays.push_back(firstMatch + (nth - 1) * 7);
      }
      for (int md : r.bymonthday) mdays.push_back(md > 0 ? md : dim + md + 1);
      if (mdays.empty()) mdays.push_back(ev.start.w.d);
      std::sort(mdays.begin(), mdays.end());
      for (int md : mdays)
        if (md >= 1 && md <= dim && !day(Days(y, m, md))) return;
    } else if (r.freq == 4) {
      int y = ev.start.w.y + step * r.interval;
      std::vector<int> months = r.bymonth.empty() ? std::vector<int>{ev.start.w.mo} : r.bymonth;
      for (int m : months)
        if (ev.start.w.d <= DaysInMonth(y, m) && !day(Days(y, m, ev.start.w.d))) return;
    } else {
      return;
    }
  }
}

void ParseIcs(const std::string& text, int64_t from, int64_t to, std::vector<CalEvent>& out) {
  // Unfold continuation lines first.
  std::vector<std::string> lines;
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
    p = e == std::string::npos ? text.size() : e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !lines.empty())
      lines.back() += line.substr(1);
    else
      lines.push_back(std::move(line));
  }
  std::vector<Vevent> events;
  Vevent cur;
  bool inEvent = false;
  int nested = 0;
  for (const std::string& line : lines) {
    if (line == "BEGIN:VEVENT") {
      cur = Vevent{};
      inEvent = true;
      nested = 0;
      continue;
    }
    if (!inEvent) continue;
    if (line.rfind("BEGIN:", 0) == 0) {
      nested++;
      continue;
    }
    if (line.rfind("END:", 0) == 0) {
      if (nested > 0) {
        nested--;
        continue;
      }
      if (line == "END:VEVENT") {
        inEvent = false;
        if (cur.start.ok) events.push_back(std::move(cur));
      }
      continue;
    }
    if (nested > 0) continue;
    size_t colon = std::string::npos;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); i++) {
      if (line[i] == '"') quoted = !quoted;
      if (line[i] == ':' && !quoted) {
        colon = i;
        break;
      }
    }
    if (colon == std::string::npos) continue;
    std::string head = line.substr(0, colon), value = line.substr(colon + 1);
    size_t semi = head.find(';');
    std::string name = head.substr(0, semi), params = semi == std::string::npos ? "" : head.substr(semi + 1);
    if (name == "UID") cur.uid = value;
    else if (name == "SUMMARY") cur.summary = Unescape(value);
    else if (name == "DTSTART") cur.start = ParseTime(value, params);
    else if (name == "DTEND") cur.end = ParseTime(value, params);
    else if (name == "DURATION") cur.duration = ParseDuration(value);
    else if (name == "STATUS") cur.cancelled = value == "CANCELLED";
    else if (name == "RECURRENCE-ID") cur.recurrenceId = ParseTime(value, params);
    else if (name == "RRULE") {
      cur.rule = ParseRule(value);
      cur.hasRule = true;
    } else if (name == "EXDATE") {
      size_t q = 0;
      while (q < value.size()) {
        size_t c = value.find(',', q);
        IcsTime t = ParseTime(value.substr(q, c == std::string::npos ? std::string::npos : c - q), params);
        if (t.ok) cur.exdates.push_back(ToUnix(t));
        q = c == std::string::npos ? value.size() : c + 1;
      }
    }
  }
  // Moved or cancelled single occurrences replace the occurrence their master would make.
  std::set<std::string> overridden;
  for (const Vevent& ev : events)
    if (ev.recurrenceId.ok) overridden.insert(ev.uid + "|" + std::to_string(ToUnix(ev.recurrenceId)));
  for (const Vevent& ev : events) {
    if (ev.cancelled) continue;
    if (ev.recurrenceId.ok) {
      Vevent single = ev;
      single.hasRule = false;
      Expand(single, from, to, {}, out);
    } else {
      Expand(ev, from, to, overridden, out);
    }
  }
}

// One feed's events in [from, to), or why there are none.
bool FetchFeed(std::string link, int64_t from, int64_t to, std::vector<CalEvent>& out, std::string& err) {
  if (link.rfind("webcal://", 0) == 0) link = "https://" + link.substr(9);
  HttpResult r = HttpGet(link);
  bool ok = r.error.empty() && r.body.find("BEGIN:VCALENDAR") != std::string::npos;
  if (ok) {
    ParseIcs(r.body, from, to, out);
  } else {
    err = r.error.empty() ? "not a calendar link" : r.error;
    // Google's "public address" 404s unless the calendar is shared publicly; the secret one works.
    if (r.status == 404 && link.find("/public/basic.ics") != std::string::npos)
      err = "not public: paste its secret address";
    else if (r.status == 404)
      err = "link not found (404)";
    else if (r.status == 401 || r.status == 403)
      err = "link refused (" + std::to_string(r.status) + ")";
  }
  SecureZeroMemory(link.data(), link.size());
  return ok;
}

// Both links (the main calendar, and a second such as Canvas due dates) merge into one list;
// a failing feed shows as a note above the other's events instead of blanking them.
void FetchCalendar(const Config& cfg) {
  CalendarState cs;
  int64_t now = NowMs(), from = now - 86400000LL, to = now + (int64_t)std::max(1, cfg.calDays) * 86400000LL;
  std::vector<CalEvent> evs;
  std::string err;
  bool any = false;
  const wchar_t* const kLinks[] = {kSecretIcal, kSecretIcal2};
  for (int i = 0; i < 2; i++) {
    std::string link, why;
    if (!SecretGet(kLinks[i], link)) continue;
    cs.configured = true;
    if (FetchFeed(std::move(link), from, to, evs, why))
      any = true;
    else if (err.empty())
      err = i ? "second link: " + why : why;
  }
  if (!cs.configured) {
    std::lock_guard lk(g_mu);
    g_calendar = cs;
    return;
  }
  if (any) {
    evs.erase(std::remove_if(evs.begin(), evs.end(),
                             [&](const CalEvent& e) { return std::max(e.end, e.start + 60000) <= now; }),
              evs.end());
    std::sort(evs.begin(), evs.end(), [](const CalEvent& a, const CalEvent& b) {
      return a.start != b.start ? a.start < b.start : a.allDay > b.allDay;
    });
    if (evs.size() > 40) evs.resize(40);
    cs.ok = true;
    cs.events = std::move(evs);
    cs.fetchedAt = now;
    cs.error = err;
  }
  std::lock_guard lk(g_mu);
  if (cs.ok) {
    g_calendar = std::move(cs);
  } else {
    g_calendar.configured = true;
    g_calendar.error = err;
  }
}

// ---- slack --------------------------------------------------------------------

std::string g_slackSelf;
std::unordered_map<std::string, std::string> g_slackUsers;

std::string SlackUser(const std::string& id, const std::wstring& auth) {
  auto it = g_slackUsers.find(id);
  if (it != g_slackUsers.end()) return it->second;
  std::string name = "user";
  HttpResult r = HttpGet("https://slack.com/api/users.info?user=" + UrlEncode(id), auth);
  JVal j;
  if (r.error.empty() && JParse(r.body, j) && j["ok"].flag()) {
    const JVal& u = j["user"];
    std::string_view dn = u["profile"]["display_name"].str();
    name = std::string(!dn.empty() ? dn : !u["real_name"].str().empty() ? u["real_name"].str() : u["name"].str());
  }
  return g_slackUsers[id] = name;  // also caches failures (missing users:read scope)
}

// Slack mrkdwn to plain text: <@U1>, <#C1|name>, <url|label>, &amp; ...
std::string CleanSlack(std::string_view t, const std::wstring& auth) {
  std::string out;
  for (size_t i = 0; i < t.size();) {
    if (t[i] == '<') {
      size_t e = t.find('>', i);
      if (e == std::string_view::npos) {
        out.append(t.substr(i));
        break;
      }
      std::string_view in = t.substr(i + 1, e - i - 1), label, target = in;
      size_t bar = in.find('|');
      if (bar != std::string_view::npos) {
        target = in.substr(0, bar);
        label = in.substr(bar + 1);
      }
      if (!target.empty() && target[0] == '@')
        out += "@" + (label.empty() ? SlackUser(std::string(target.substr(1)), auth) : std::string(label));
      else if (!target.empty() && target[0] == '#')
        out += "#" + std::string(label.empty() ? target.substr(1) : label);
      else if (!target.empty() && target[0] == '!')
        out += label.empty() ? "@" + std::string(target.substr(1, target.find('^') - 1)) : std::string(label);
      else
        out += std::string(label.empty() ? target : label);
      i = e + 1;
      continue;
    }
    if (t.compare(i, 5, "&amp;") == 0) {
      out += '&';
      i += 5;
    } else if (t.compare(i, 4, "&lt;") == 0) {
      out += '<';
      i += 4;
    } else if (t.compare(i, 4, "&gt;") == 0) {
      out += '>';
      i += 4;
    } else {
      out += t[i++];
    }
  }
  return OneLine(out, 400);
}

void FetchSlack() {
  std::string token;
  SlackState ss;
  ss.configured = SecretGet(kSecretSlack, token);
  if (!ss.configured) {
    std::lock_guard lk(g_mu);
    g_slack = ss;
    return;
  }
  std::wstring auth = L"Authorization: Bearer " + Widen(token);
  std::string error;
  JVal j;
  if (g_slackSelf.empty()) {
    HttpResult who = HttpGet("https://slack.com/api/auth.test", auth);
    if (who.error.empty() && JParse(who.body, j) && j["ok"].flag())
      g_slackSelf = std::string(j["user_id"].str());
    else
      error = who.error.empty() ? std::string(j["error"].str()) : who.error;
  }
  if (error.empty()) {
    SYSTEMTIME since = LocalTime(NowMs() - 2 * 86400000LL);
    char query[32];
    snprintf(query, sizeof query, "after:%04d-%02d-%02d", since.wYear, since.wMonth, since.wDay);
    HttpResult r = HttpGet("https://slack.com/api/search.messages?sort=timestamp&sort_dir=desc&count=30&query=" +
                               UrlEncode(query),
                           auth);
    if (r.error.empty() && JParse(r.body, j) && j["ok"].flag()) {
      const JVal& found = j["messages"];
      int own = 0, blank = 0;
      for (const JVal& m : found["matches"].a) {
        own += m["user"].str() == g_slackSelf;
        blank += m["text"].str().empty();
      }
      DebugLog("slack: query '" + std::string(query) + "' total " + std::to_string((int)found["total"].num()) +
               " matches " + std::to_string(found["matches"].a.size()) + " own " + std::to_string(own) + " blank " +
               std::to_string(blank));
      for (const JVal& m : found["matches"].a) {
        SlackMsg msg;
        msg.text = CleanSlack(m["text"].str(), auth);
        if (msg.text.empty()) continue;
        const JVal& ch = m["channel"];
        msg.dm = ch["is_im"].flag();
        msg.mine = m["user"].str() == g_slackSelf;
        std::string name(ch["name"].str());
        // A DM's "name" is the other person's user id.
        bool isId = name.size() > 8 && (name[0] == 'U' || name[0] == 'W') &&
                    std::all_of(name.begin(), name.end(), [](char ch) { return isupper((unsigned char)ch) || isdigit((unsigned char)ch); });
        msg.where = msg.dm ? "@" + (isId ? SlackUser(name, auth) : name) : ch["is_mpim"].flag() ? "GROUP" : "#" + name;
        msg.who = msg.mine ? "you" : std::string(m["username"].str());
        if (msg.who.empty()) msg.who = SlackUser(std::string(m["user"].str()), auth);
        msg.at = (int64_t)(atof(std::string(m["ts"].str()).c_str()) * 1000);
        msg.link = std::string(m["permalink"].str());
        ss.msgs.push_back(std::move(msg));
        if (ss.msgs.size() >= 12) break;
      }
      ss.ok = true;
      ss.fetchedAt = NowMs();
    } else {
      error = r.error.empty() ? std::string(j["error"].str()) : r.error;
      DebugLog("slack: search failed '" + error + "' needed '" + std::string(j["needed"].str()) + "'");
      if (error == "invalid_auth" || error == "token_revoked") g_slackSelf.clear();
    }
  } else {
    DebugLog("slack: auth.test failed '" + error + "'");
  }
  std::lock_guard lk(g_mu);
  if (ss.ok) {
    g_slack = std::move(ss);
  } else {
    g_slack.configured = true;
    g_slack.error = error.empty() ? "unreadable reply" : error;
  }
}

void Run() {
  int64_t nextWeather = 0, nextCal = 0, nextSlack = 0;
  for (;;) {
    Config cfg = ConfigCopy();
    auto wanted = [&](const char* section) {
      return std::find(cfg.sections.begin(), cfg.sections.end(), section) != cfg.sections.end();
    };
    int64_t now = NowMs();
    bool changed = false;
    if (wanted("weather") && now >= nextWeather) {
      FetchWeather(cfg);
      nextWeather = now + (WeatherSnapshot().error.empty() ? 15 : 2) * 60000LL;
      changed = true;
    }
    if (wanted("calendar") && now >= nextCal) {
      FetchCalendar(cfg);
      nextCal = now + (CalendarSnapshot().error.empty() ? 10 : 2) * 60000LL;
      changed = true;
    }
    if (wanted("slack") && now >= nextSlack) {
      FetchSlack();
      nextSlack = now + (SlackSnapshot().error.empty() ? 60 : 180) * 1000LL;
      changed = true;
    }
    if (changed && g_hwnd) PostMessageW(g_hwnd, WM_PB_REFRESH, 0, 0);
    if (WaitForSingleObject(g_kick, 5000) == WAIT_OBJECT_0) nextWeather = nextCal = nextSlack = 0;
  }
}

}  // namespace

void OnlineStart(HWND notify) {
  g_hwnd = notify;
  g_kick = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  // Known up front, so the first frames say LOADING instead of asking for a link that's saved.
  std::string secret;
  g_calendar.configured = SecretGet(kSecretIcal, secret) || SecretGet(kSecretIcal2, secret);
  g_slack.configured = SecretGet(kSecretSlack, secret);
  SecureZeroMemory(secret.data(), secret.size());
  std::thread(Run).detach();
}

void OnlineKick() {
  if (g_kick) SetEvent(g_kick);
}

WeatherState WeatherSnapshot() {
  std::lock_guard lk(g_mu);
  return g_weather;
}

CalendarState CalendarSnapshot() {
  std::lock_guard lk(g_mu);
  return g_calendar;
}

SlackState SlackSnapshot() {
  std::lock_guard lk(g_mu);
  return g_slack;
}
