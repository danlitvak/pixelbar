#include "pixelbar.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

std::wstring Widen(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string Narrow(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

int64_t FileTimeToUnixMs(const FILETIME& ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
}

int64_t NowMs() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  return FileTimeToUnixMs(ft);
}

static int64_t DaysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

int64_t ParseIsoMs(std::string_view s) {
  auto num = [&](size_t pos, size_t len, int& out) {
    if (pos + len > s.size()) return false;
    int v = 0;
    for (size_t i = 0; i < len; i++) {
      char c = s[pos + i];
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    out = v;
    return true;
  };
  int Y, M, D, h, m, sec;
  if (!num(0, 4, Y) || !num(5, 2, M) || !num(8, 2, D) || !num(11, 2, h) || !num(14, 2, m) ||
      !num(17, 2, sec))
    return 0;
  int64_t ms = 0;
  size_t p = 19;
  if (p < s.size() && s[p] == '.') {
    p++;
    int digits = 0;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
      if (digits < 3) {
        ms = ms * 10 + (s[p] - '0');
        digits++;
      }
      p++;
    }
    while (digits++ < 3) ms *= 10;
  }
  int64_t offsetMin = 0;
  if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
    int oh = 0, om = 0;
    num(p + 1, 2, oh);
    num(p + 4, 2, om);
    offsetMin = (int64_t)(oh * 60 + om) * (s[p] == '-' ? -1 : 1);
  }
  int64_t days = DaysFromCivil(Y, M, D);
  return ((days * 86400 + h * 3600 + m * 60 + sec) - offsetMin * 60) * 1000 + ms;
}

std::wstring ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
  std::wstring p(buf, n);
  size_t cut = p.find_last_of(L"\\/");
  return cut == std::wstring::npos ? p : p.substr(0, cut);
}

std::wstring EnvDir(const wchar_t* name) {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)std::size(buf));
  if (n == 0 || n >= std::size(buf)) return {};
  return std::wstring(buf, n);
}

static HANDLE OpenShared(const std::wstring& path) {
  return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static bool ReadSpan(HANDLE h, uint64_t offset, size_t len, std::string& out) {
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)offset;
  if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
  out.resize(len);
  size_t got = 0;
  while (got < len) {
    DWORD chunk = (DWORD)std::min<size_t>(len - got, 1u << 24), read = 0;
    if (!ReadFile(h, out.data() + got, chunk, &read, nullptr) || read == 0) break;
    got += read;
  }
  out.resize(got);
  return true;
}

bool ReadHead(const std::wstring& path, size_t max, std::string& out) {
  HANDLE h = OpenShared(path);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = ReadSpan(h, 0, max, out);
  CloseHandle(h);
  return ok;
}

bool ReadTail(const std::wstring& path, size_t max, std::string& out, uint64_t* fileSize) {
  HANDLE h = OpenShared(path);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(h, &size) != 0;
  if (ok) {
    uint64_t total = (uint64_t)size.QuadPart;
    uint64_t start = total > max ? total - max : 0;
    ok = ReadSpan(h, start, (size_t)(total - start), out);
    if (fileSize) *fileSize = total;
  }
  CloseHandle(h);
  return ok;
}

bool ReadAll(const std::wstring& path, std::string& out) {
  return ReadTail(path, (size_t)64 << 20, out);
}

bool ReadRange(const std::wstring& path, uint64_t offset, size_t len, std::string& out) {
  HANDLE h = OpenShared(path);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = ReadSpan(h, offset, len, out);
  CloseHandle(h);
  return ok;
}

// Writes beside the target and swaps it in, so a reader never sees half a file.
bool WriteAll(const std::wstring& path, std::string_view data) {
  std::wstring tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
  CloseHandle(h);
  if (!ok) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Appends to %TEMP%\pixelbar-debug.log, only when PIXELBAR_DEBUG is set.
void DebugLog(std::string_view line) {
  static const bool on = GetEnvironmentVariableW(L"PIXELBAR_DEBUG", nullptr, 0) > 0;
  if (!on) return;
  std::wstring path = EnvDir(L"TEMP") + L"\\pixelbar-debug.log";
  HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::string s(line);
  s += "\r\n";
  DWORD wrote;
  WriteFile(h, s.data(), (DWORD)s.size(), &wrote, nullptr);
  CloseHandle(h);
}

std::string FormatBytes(uint64_t bytes) {
  double v = (double)bytes, gb = 1024.0 * 1024 * 1024;
  char b[32];
  if (v >= gb)
    snprintf(b, sizeof b, v >= 100 * gb ? "%.0f GB" : "%.1f GB", v / gb);
  else if (v >= 1024.0 * 1024)
    snprintf(b, sizeof b, "%.0f MB", v / (1024.0 * 1024));
  else
    snprintf(b, sizeof b, "%.0f KB", v / 1024.0);
  return b;
}

std::string UrlEncode(std::string_view s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

// Local wall time for a unix ms, using the DST rule in force on that date.
SYSTEMTIME LocalTime(int64_t unixMs) {
  ULARGE_INTEGER u;
  u.QuadPart = (ULONGLONG)((unixMs + 11644473600000LL) * 10000LL);
  FILETIME ft{u.LowPart, u.HighPart};
  SYSTEMTIME utc{}, local{};
  FileTimeToSystemTime(&ft, &utc);
  SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
  return local;
}

bool Stat(const std::wstring& path, FileStamp& st) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
  st.mtime = FileTimeToUnixMs(d.ftLastWriteTime);
  st.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
  return true;
}

std::string Leaf(std::string_view path) {
  while (!path.empty() && (path.back() == '\\' || path.back() == '/')) path.remove_suffix(1);
  size_t cut = path.find_last_of("\\/");
  return std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
}

std::string OneLine(std::string_view s, size_t maxBytes) {
  std::string out;
  bool pendingSpace = false;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + len > s.size()) break;
    if (len == 1) {
      i++;
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        pendingSpace = !out.empty();
        continue;
      }
      if (c == '*' || c == '`') continue;  // emphasis, code ticks
      bool wordStart = out.empty() || pendingSpace || out.back() == ' ';
      if (wordStart && c == '#') continue;                                          // headings, mid-text too
      if (out.empty() && (c == '>' || c == '-' || c == '|')) continue;              // quote, bullet
      if (c < 0x20) continue;
    } else {
      i += len;
    }
    if (out.size() + len + (pendingSpace ? 1 : 0) > maxBytes) break;
    if (pendingSpace) {
      out += ' ';
      pendingSpace = false;
    }
    out.append(s.data() + i - len, len);
  }
  return out;
}

std::string FormatAge(int64_t ms) {
  long long s = (long long)std::max<int64_t>(ms, 0) / 1000;
  char b[24];
  if (s < 60)
    snprintf(b, sizeof b, "%llds", s);
  else if (s < 3600)
    snprintf(b, sizeof b, "%lldm", s / 60);
  else if (s < 86400)
    snprintf(b, sizeof b, "%lldh%02lld", s / 3600, (s / 60) % 60);
  else
    snprintf(b, sizeof b, "%lldd", s / 86400);
  return b;
}
