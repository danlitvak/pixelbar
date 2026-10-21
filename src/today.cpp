// Today's AI usage, summed from the logs the agents widget already reads: every Claude Code
// transcript (subagents included) and Codex rollout written since local midnight. Each file
// is followed by byte offset, so a line is read once; on start, a binary search finds where
// today begins in logs that run to hundreds of MB.
//   Claude: one assistant message = one model request; its usage repeats on every line the
//           message is split across, so requests are counted once per message id.
//   Codex:  one token_usage_record per model request, deduplicated by response id.
#include "pixelbar.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {

struct FileState {
  bool codex = false, started = false;
  uint64_t offset = 0;                    // bytes consumed
  std::unordered_set<std::string> seen;   // message / response ids counted today
};

std::mutex g_mu;
AiToday g_published;
HWND g_hwnd = nullptr;
std::unordered_map<std::wstring, FileState> g_files;
AiToday g_acc;

int64_t LocalMidnight(int64_t now) {
  SYSTEMTIME lt = LocalTime(now), utc{};
  lt.wHour = lt.wMinute = lt.wSecond = lt.wMilliseconds = 0;
  TzSpecificLocalTimeToSystemTime(nullptr, &lt, &utc);
  FILETIME ft;
  SystemTimeToFileTime(&utc, &ft);
  return FileTimeToUnixMs(ft);
}

int64_t LineTime(std::string_view line) {
  std::string ts;
  return JFindString(line, "timestamp", ts) ? ParseIsoMs(ts) : -1;
}

// Offset of a line at or shortly before the first one written today.
uint64_t FindDayStart(const std::wstring& path, uint64_t size, int64_t dayStart) {
  uint64_t lo = 0, hi = size;
  std::string chunk;
  while (hi - lo > ((uint64_t)1 << 20)) {
    uint64_t mid = lo + (hi - lo) / 2;
    if (!ReadRange(path, mid, (size_t)256 << 10, chunk)) break;
    int64_t t = -1;
    for (size_t p = chunk.find('\n'); p != std::string::npos && t < 0;) {
      size_t e = chunk.find('\n', p + 1);
      if (e == std::string::npos) break;
      t = LineTime(std::string_view(chunk).substr(p + 1, e - p - 1));
      p = e;
    }
    if (t < 0 || t >= dayStart)  // unknown counts as today: reading too much is safe, too little is not
      hi = mid;
    else
      lo = mid;
  }
  return lo;
}

void ClaudeLine(FileState& fs, std::string_view line) {
  bool usage = line.find("\"usage\"") != std::string_view::npos;
  bool prompt = !usage && line.find("\"type\":\"user\"") != std::string_view::npos &&
                line.find("\"tool_result\"") == std::string_view::npos;
  if (!usage && !prompt) return;
  JVal j;
  if (!JParse(line, j)) return;
  int64_t ts = ParseIsoMs(j["timestamp"].str());
  if (ts < g_acc.dayStart) return;
  std::string_view type = j["type"].str();
  if (type == "assistant") {
    const JVal& m = j["message"];
    const JVal& u = m["usage"];
    std::string id(m["id"].str());
    if (!u.isObj() || (!id.empty() && !fs.seen.insert(id).second)) return;
    uint64_t out = (uint64_t)u["output_tokens"].num();
    g_acc.claude.calls++;
    g_acc.claude.out += out;
    // Cache writes are new input too; only reads come back from the cache.
    g_acc.claude.in += (uint64_t)(u["input_tokens"].num() + u["cache_creation_input_tokens"].num());
    g_acc.claude.cached += (uint64_t)u["cache_read_input_tokens"].num();
    g_acc.claudeHours[LocalTime(ts).wHour % 24] += (float)out;
  } else if (type == "user" && !j["isMeta"].flag() && !j["isSidechain"].flag() && !j["isCompactSummary"].flag()) {
    // A turn is something typed: not a subagent's brief, a compaction summary, or a <tagged>
    // message the app injects (slash commands, task notifications, ! shell output).
    const JVal& c = j["message"]["content"];
    std::string_view text = c.t == JVal::Str ? c.str() : c.at(0)["text"].str();
    if (text.empty() || text[0] == '<' || text.rfind("[Request interrupted", 0) == 0) return;
    g_acc.claude.turns++;
  }
}

void CodexLine(FileState& fs, std::string_view line) {
  bool record = line.find("\"token_usage_record\"") != std::string_view::npos;
  bool started = !record && line.find("\"task_started\"") != std::string_view::npos;
  if (!record && !started) return;
  JVal j;
  if (!JParse(line, j)) return;
  int64_t ts = ParseIsoMs(j["timestamp"].str());
  if (ts < g_acc.dayStart) return;
  const JVal& pl = j["payload"];
  if (j["type"].str() == "token_usage_record") {
    const JVal& u = pl["usage"];
    std::string id(pl["response_id"].str());
    if (!u.isObj() || (!id.empty() && !fs.seen.insert(id).second)) return;
    uint64_t out = (uint64_t)u["output_tokens"].num(), input = (uint64_t)u["input_tokens"].num();
    uint64_t cached = std::min(input, (uint64_t)u["cached_input_tokens"].num());  // part of input_tokens
    g_acc.codex.calls++;
    g_acc.codex.out += out;
    g_acc.codex.in += input - cached;
    g_acc.codex.cached += cached;
    g_acc.codexHours[LocalTime(ts).wHour % 24] += (float)out;
  } else if (pl["type"].str() == "task_started") {
    g_acc.codex.turns++;
  }
}

// Reads whatever was appended since the last pass; false when nothing new was complete.
bool Consume(const std::wstring& path, FileState& fs, uint64_t size) {
  bool any = false;
  constexpr size_t kChunk = (size_t)8 << 20;
  while (fs.offset < size) {
    size_t want = (size_t)std::min<uint64_t>(size - fs.offset, kChunk);
    std::string buf;
    if (!ReadRange(path, fs.offset, want, buf) || buf.empty()) break;
    size_t last = buf.rfind('\n');
    if (last == std::string::npos) {
      if (want == kChunk) {  // one line longer than a chunk: a huge tool result, skip it
        fs.offset += want;
        continue;
      }
      break;  // the final line is still being written
    }
    std::string_view sv(buf.data(), last + 1);
    for (size_t p = 0; p < sv.size();) {
      size_t e = sv.find('\n', p);
      std::string_view line = sv.substr(p, e - p);
      if (fs.codex)
        CodexLine(fs, line);
      else
        ClaudeLine(fs, line);
      p = e + 1;
    }
    fs.offset += last + 1;
    any = true;
  }
  return any;
}

void Track(const std::wstring& path, int64_t mtime, bool codex) {
  if (mtime < g_acc.dayStart) return;
  FileState& fs = g_files[path];
  fs.codex = codex;
}

void Discover() {
  std::wstring projects = EnvDir(L"USERPROFILE") + L"\\.claude\\projects";
  ListDir(projects, L"*", [&](const WIN32_FIND_DATAW& d) {
    if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    std::wstring dir = projects + L"\\" + d.cFileName;
    ListDir(dir, L"*", [&](const WIN32_FIND_DATAW& f) {
      std::wstring p = dir + L"\\" + f.cFileName;
      if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        // <session>\subagents\*.jsonl: subagent work counts too
        ListDir(p + L"\\subagents", L"*.jsonl", [&](const WIN32_FIND_DATAW& s) {
          Track(p + L"\\subagents\\" + s.cFileName, FileTimeToUnixMs(s.ftLastWriteTime), false);
        });
        return;
      }
      size_t n = wcslen(f.cFileName);
      if (n > 6 && wcscmp(f.cFileName + n - 6, L".jsonl") == 0) Track(p, FileTimeToUnixMs(f.ftLastWriteTime), false);
    });
  });
  std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& dir, int depth) {
    ListDir(dir, L"*", [&](const WIN32_FIND_DATAW& f) {
      std::wstring p = dir + L"\\" + f.cFileName;
      if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (depth < 3) walk(p, depth + 1);
      } else if (wcsncmp(f.cFileName, L"rollout-", 8) == 0) {
        Track(p, FileTimeToUnixMs(f.ftLastWriteTime), true);
      }
    });
  };
  walk(EnvDir(L"USERPROFILE") + L"\\.codex\\sessions", 0);
}

void Run() {
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // never compete with real work
  int64_t lastDiscover = 0;
  for (;;) {
    int64_t now = NowMs(), midnight = LocalMidnight(now);
    if (midnight != g_acc.dayStart) {  // a new day: start the counts over
      g_files.clear();
      g_acc = AiToday{};
      g_acc.dayStart = midnight;
      lastDiscover = 0;
    }
    if (now - lastDiscover > 10000) {
      Discover();
      lastDiscover = now;
    }
    bool changed = false;
    for (auto& [path, fs] : g_files) {
      FileStamp st;
      if (!Stat(path, st)) continue;
      if (!fs.started) {
        fs.offset = st.size > ((uint64_t)16 << 20) ? FindDayStart(path, st.size, g_acc.dayStart) : 0;
        fs.started = true;
      }
      if (st.size > fs.offset) changed |= Consume(path, fs, st.size);
    }
    bool wasScanning = g_acc.scanning;
    g_acc.scanning = false;  // every file has now been read up to date at least once
    if (changed || wasScanning) {
      {
        std::lock_guard lk(g_mu);
        g_published = g_acc;
      }
      if (g_hwnd) PostMessageW(g_hwnd, WM_PB_REFRESH, 0, 0);
    }
    Sleep(2000);
  }
}

}  // namespace

void TodayStart(HWND notify) {
  g_hwnd = notify;
  std::thread(Run).detach();
}

AiToday TodaySnapshot() {
  std::lock_guard lk(g_mu);
  return g_published;
}
