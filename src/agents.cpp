// Live state of Claude Code and Codex sessions, read from the logs both apps already write:
//   Claude   ~/.claude/projects/<project>/<session>.jsonl, titles from the desktop app's
//            %APPDATA%/Claude/claude-code-sessions/**/local_*.json
//   Codex    ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl, titles from ~/.codex/session_index.jsonl
// Permission prompts are not in the Claude transcript, so an optional PermissionRequest hook
// drops the request into ~/.claude/pixelbar/perm-*.json (see README).
#include "pixelbar.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

struct Track {
  std::wstring path;
  bool codex = false;
  std::string id, cwd;
  FileStamp stamp;
  bool known = false, metaRead = false;
  AgentState state = AgentState::Done;
  int64_t sinceMs = 0;          // terminal states and waits: when it happened
  int64_t workingSinceMs = 0;   // first time this turn was seen working
  std::string detail;
  bool pendingTool = false;     // last entry is a tool call still running (or awaiting approval)
  std::vector<LimitWindow> limits;  // Codex: rate limits from the newest token_count event
  int64_t limitsAt = 0;
};

struct DesktopMeta {
  std::string hostId, title;
  bool archived = false;
  int64_t turnEdge = 0;  // start of the running turn, or end of the last one
};

struct PermInfo {
  int64_t mtime = 0;
  std::string detail;
};

std::mutex g_mu;
std::vector<Agent> g_snapshot;
HWND g_hwnd = nullptr;
HANDLE g_wake = nullptr;
std::atomic<bool> g_rescan{true};

std::unordered_map<std::wstring, Track> g_tracks;
std::unordered_map<std::wstring, FileStamp> g_metaStamps;
std::unordered_map<std::string, DesktopMeta> g_meta;  // by Claude Code session id
std::unordered_map<std::string, std::string> g_codexNames;
FileStamp g_codexIndexStamp;
std::unordered_map<std::wstring, FileStamp> g_permStamps;
std::unordered_map<std::string, PermInfo> g_perms;  // by Claude Code session id
UsageState g_usage;                                 // guarded by g_mu once published
FileStamp g_planStamp;
std::vector<LimitWindow> g_claudeLimits, g_codexLimits;
int64_t g_claudeLimitsAt = 0, g_codexLimitsAt = 0;
std::wstring g_newestRollout;
FileStamp g_newestRolloutStamp;

std::vector<std::string_view> Lines(std::string_view text, bool partialHead) {
  std::vector<std::string_view> out;
  size_t start = 0;
  if (partialHead) {
    size_t nl = text.find('\n');
    if (nl == std::string_view::npos) return out;
    start = nl + 1;
  }
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    std::string_view line = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) out.push_back(line);
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  return out;
}

std::string ProjectName(const std::string& cwd) {
  std::string p = cwd;
  for (const char* marker : {"\\.claude\\worktrees\\", "/.claude/worktrees/", "\\.codex\\worktrees\\"}) {
    size_t at = p.find(marker);
    if (at != std::string::npos) p.resize(at);
  }
  return Leaf(p);
}

// ---- Claude transcript ------------------------------------------------------

std::string ToolDetail(std::string_view name, const JVal& in) {
  std::string tool(name);
  if (tool.rfind("mcp__", 0) == 0) {
    size_t p = tool.find("__", 5);
    if (p != std::string::npos) tool = tool.substr(p + 2);
  }
  std::string_view arg;
  for (const char* k : {"description", "command", "file_path", "notebook_path", "pattern", "query", "url", "skill",
                        "prompt", "path"}) {
    arg = in[k].str();
    if (!arg.empty()) break;
  }
  std::string a(arg);
  if (name == "Read" || name == "Write" || name == "Edit" || name == "NotebookEdit") a = Leaf(a);
  return a.empty() ? tool : tool + " " + OneLine(a, 120);
}

bool Interactive(std::string_view name, const JVal& in, std::string& what) {
  if (name == "AskUserQuestion") {
    what = OneLine(in["questions"].at(0)["question"].str(), 140);
    if (what.empty()) what = "has a question";
    return true;
  }
  if (name == "ExitPlanMode") {
    what = "plan ready for review";
    return true;
  }
  if (name == "mcp__ccd_session__start_session" && in["initiation"].str() == "offer") {
    what = "new session? " + OneLine(in["title"].str(), 100);
    return true;
  }
  if (name == "mcp__computer-use__request_access") {
    what = "approve computer use";
    return true;
  }
  return false;
}

bool UserText(const JVal& j, std::string_view prefix) {
  const JVal& c = j["message"]["content"];
  if (c.t == JVal::Str) return c.s.rfind(prefix, 0) == 0;
  for (auto& b : c.a)
    if (b["type"].str() == "text" && b["text"].str().substr(0, prefix.size()) == prefix) return true;
  return false;
}

bool IsToolResult(const JVal& j) {
  for (auto& b : j["message"]["content"].a)
    if (b["type"].str() == "tool_result") return true;
  return false;
}

bool IsPrompt(const JVal& j) {
  if (j["isMeta"].flag() || IsToolResult(j)) return false;
  const JVal& c = j["message"]["content"];
  return c.t == JVal::Str || c.isArr();
}

bool DecideClaude(Track& t, const std::vector<std::string_view>& lines) {
  bool decided = false, ended = false;
  std::string ask, recap, lastText;
  int lookback = 0;
  for (size_t k = lines.size(); k-- > 0;) {
    JVal j;
    if (!JParse(lines[k], j) || !j.isObj()) continue;
    if (t.cwd.empty() && !j["cwd"].str().empty()) t.cwd = std::string(j["cwd"].str());
    std::string_view type = j["type"].str();
    int64_t ts = ParseIsoMs(j["timestamp"].str());

    if (decided) {
      // The turn is over; walk back through it for the wrap-up card and the final reply.
      if (++lookback > 150) break;
      if (type == "assistant") {
        bool found = false;
        for (auto& b : j["message"]["content"].a) {
          std::string_view bt = b["type"].str();
          if (bt == "tool_use" && b["name"].str() == "mcp__ccd_turn__wrap_up") {
            ask = std::string(b["input"]["ask"].str());
            recap = std::string(b["input"]["recap"].str());
            found = true;
          } else if (bt == "text" && lastText.empty()) {
            lastText = std::string(b["text"].str());
          }
        }
        if (found) break;
      } else if (type == "user" && IsPrompt(j)) {
        break;
      }
      continue;
    }

    if (type == "assistant") {
      const JVal& msg = j["message"];
      const JVal* tool = nullptr;
      std::string text;
      for (auto& b : msg["content"].a) {
        std::string_view bt = b["type"].str();
        if (bt == "tool_use") tool = &b;
        if (bt == "text" && text.empty()) text = std::string(b["text"].str());
      }
      std::string_view stop = msg["stop_reason"].str();
      if (tool) {
        std::string what;
        if (Interactive((*tool)["name"].str(), (*tool)["input"], what)) {
          t.state = AgentState::NeedsYou;
          t.detail = what;
          t.sinceMs = ts;
        } else {
          t.state = AgentState::Working;
          t.detail = ToolDetail((*tool)["name"].str(), (*tool)["input"]);
          t.pendingTool = true;
        }
        return true;
      }
      if (stop == "end_turn" || stop == "stop_sequence" || stop == "max_tokens" || stop == "refusal") {
        t.state = AgentState::Done;
        t.sinceMs = ts;
        lastText = text;
        decided = ended = true;
        continue;
      }
      t.state = AgentState::Working;
      t.detail = "writing";
      return true;
    }
    if (type == "user") {
      if (j["isMeta"].flag()) continue;
      if (UserText(j, "[Request interrupted by user")) {
        t.state = AgentState::Stopped;
        t.detail = "interrupted";
        t.sinceMs = ts;
      } else {
        t.state = AgentState::Working;
        t.detail = "thinking";
      }
      return true;
    }
    if (type == "api-request") {
      t.state = AgentState::Working;
      t.detail = "thinking";
      return true;
    }
    if (type == "system") {
      std::string_view sub = j["subtype"].str();
      if (sub == "stop_hook_summary" || sub == "turn_duration") {
        t.state = AgentState::Done;
        t.sinceMs = ts;
        decided = ended = true;
      } else if (sub == "compact_boundary") {
        t.state = AgentState::Working;
        t.detail = "compacting";
        return true;
      }
    }
  }
  if (ended) {
    if (!ask.empty()) {
      t.state = AgentState::Asks;
      t.detail = OneLine(ask, 160);
    } else {
      t.detail = OneLine(!recap.empty() ? recap : lastText, 160);
    }
  }
  return decided;
}

// ---- Codex rollout ----------------------------------------------------------

std::string CodexItem(const JVal& it) {
  std::string_view ty = it["type"].str();
  if (ty == "CommandExecution") {
    const JVal& c = it["command"];
    std::string_view cmd = c.isArr() && !c.a.empty() ? c.a.back().str() : c.str();
    return "run " + OneLine(cmd, 120);
  }
  if (ty == "McpToolCall") return std::string(it["tool"].str());
  if (ty == "FileChange") {
    const JVal& ch = it["changes"];
    size_t n = ch.isObj() ? ch.o.size() : 0;
    return n == 1 ? "edit " + Leaf(ch.o[0].first) : "edit " + std::to_string(n) + " files";
  }
  if (ty == "AgentMessage") return OneLine(it["content"].at(0)["text"].str(), 120);
  if (ty == "Extension") return std::string(it["kind"].str()) + " " + OneLine(it["query"].str(), 80);
  if (ty == "SubAgentActivity") return "subagent";
  if (ty == "ContextCompaction") return "compacting";
  if (ty == "ImageView") return "viewing image";
  return {};
}

bool DecideCodex(Track& t, const std::vector<std::string_view>& lines) {
  std::string detail;
  for (size_t k = lines.size(); k-- > 0;) {
    JVal j;
    if (!JParse(lines[k], j) || !j.isObj()) continue;
    std::string_view type = j["type"].str();
    const JVal& pl = j["payload"];
    int64_t ts = ParseIsoMs(j["timestamp"].str());
    if (type == "event_msg") {
      std::string_view pt = pl["type"].str();
      if (pt == "task_complete") {
        double at = pl["completed_at"].num(0);
        t.state = AgentState::Done;
        t.sinceMs = at > 0 ? (int64_t)(at * 1000) : ts;
        t.detail = OneLine(pl["last_agent_message"].str(), 160);
        return true;
      }
      if (pt == "turn_aborted") {
        t.state = AgentState::Stopped;
        t.sinceMs = ts;
        t.detail = "interrupted";
        return true;
      }
      if (pt == "task_started") {
        double at = pl["started_at"].num(0);
        t.state = AgentState::Working;
        t.workingSinceMs = at > 0 ? (int64_t)(at * 1000) : ts;
        t.detail = detail.empty() ? "thinking" : detail;
        return true;
      }
      if (pt == "item_completed" && detail.empty()) detail = CodexItem(pl["item"]);
      continue;
    }
    if (type == "response_item" && detail.empty()) {
      std::string_view rt = pl["type"].str();
      if (rt == "function_call" || rt == "custom_tool_call") detail = std::string(pl["name"].str());
      continue;
    }
    if (type == "session_meta") break;
  }
  if (!detail.empty()) t.detail = detail;
  return false;
}

// The newest rate-limit snapshot Codex logged ("used_percent" per window, reset time).
void ReadCodexLimits(Track& t, const std::vector<std::string_view>& lines) {
  for (size_t k = lines.size(); k-- > 0;) {
    if (lines[k].find("\"rate_limits\"") == std::string_view::npos) continue;
    JVal j;
    if (!JParse(lines[k], j)) continue;
    const JVal& pl = j["payload"];
    const JVal& rl = pl["rate_limits"].isObj() ? pl["rate_limits"] : pl["info"]["rate_limits"];
    if (!rl.isObj()) continue;
    int64_t ts = ParseIsoMs(j["timestamp"].str());
    std::vector<LimitWindow> out;
    for (const char* which : {"primary", "secondary"}) {
      const JVal& w = rl[which];
      if (!w.isObj()) continue;
      LimitWindow lw;
      lw.minutes = (int)w["window_minutes"].num(0);
      lw.pct = (float)w["used_percent"].num(0);
      if (w["resets_at"].t == JVal::Num)
        lw.resetsAt = (int64_t)(w["resets_at"].n * 1000);
      else if (w["resets_in_seconds"].t == JVal::Num)
        lw.resetsAt = ts + (int64_t)(w["resets_in_seconds"].n * 1000);
      out.push_back(lw);
    }
    if (!out.empty() && ts >= t.limitsAt) {
      t.limits = std::move(out);
      t.limitsAt = ts;
    }
    return;
  }
}

// The desktop app samples plan usage into a small history file: fh = five-hour window,
// sd = seven-day window, both percent used. Empty samples mean it couldn't fetch.
void ScanClaudePlan() {
  std::wstring path = EnvDir(L"APPDATA") + L"\\Claude\\plan-usage-history.json";
  FileStamp st;
  if (!Stat(path, st) || st == g_planStamp) return;
  g_planStamp = st;
  std::string text;
  JVal j;
  if (!ReadAll(path, text) || !JParse(text, j)) return;
  const JVal& samples = j["samples"];
  for (size_t i = samples.a.size(); i-- > 0;) {
    const JVal& u = samples.a[i]["u"];
    if (!u.isObj() || u.o.empty()) continue;
    std::vector<LimitWindow> out;
    if (u["fh"].t == JVal::Num) out.push_back({300, (float)u["fh"].n, 0});
    if (u["sd"].t == JVal::Num) out.push_back({10080, (float)u["sd"].n, 0});
    if (out.empty()) continue;
    g_claudeLimits = std::move(out);
    g_claudeLimitsAt = (int64_t)samples.a[i]["t"].num(0);
    return;
  }
}

void ReadCodexMeta(Track& t) {
  t.metaRead = true;
  std::string head;
  if (ReadHead(t.path, (size_t)1 << 20, head)) {
    size_t nl = head.find('\n');
    JVal j;
    if (JParse(std::string_view(head).substr(0, nl), j)) {
      t.id = std::string(j["payload"]["id"].str());
      t.cwd = std::string(j["payload"]["cwd"].str());
    }
  }
  if (t.id.empty()) {
    // rollout-YYYY-MM-DDTHH-MM-SS-<thread id>[_<id>].jsonl
    std::string name = Narrow(t.path.substr(t.path.find_last_of(L'\\') + 1));
    if (name.size() >= 28 + 36) t.id = name.substr(28, 36);
  }
}

// ---- parsing a changed file -------------------------------------------------

void Parse(Track& t) {
  if (t.codex && !t.metaRead) ReadCodexMeta(t);
  bool wasLive = t.known && (t.state == AgentState::Working || t.state == AgentState::NeedsYou);
  t.pendingTool = false;
  t.detail.clear();
  bool decided = false;
  bool first = true;
  for (size_t window : {(size_t)512 << 10, (size_t)6 << 20}) {
    std::string tail;
    uint64_t size = 0;
    if (!ReadTail(t.path, window, tail, &size)) return;
    auto lines = Lines(tail, size > tail.size());
    if (t.codex && first) ReadCodexLimits(t, lines);
    first = false;
    decided = t.codex ? DecideCodex(t, lines) : DecideClaude(t, lines);
    if (decided || size <= tail.size()) break;
  }
  int64_t now = NowMs();
  if (!decided) {
    // Nothing decisive in reach (one enormous tool result, say): judge by recency.
    t.state = now - t.stamp.mtime < 120000 ? AgentState::Working : AgentState::Done;
    t.sinceMs = t.stamp.mtime;
  }
  t.known = true;
  bool live = t.state == AgentState::Working || t.state == AgentState::NeedsYou;
  if (live && !wasLive && !t.codex) t.workingSinceMs = now;
}

// ---- discovery --------------------------------------------------------------

void ScanClaude(int64_t now, int64_t window) {
  std::wstring root = EnvDir(L"USERPROFILE") + L"\\.claude\\projects";
  ListDir(root, L"*", [&](const WIN32_FIND_DATAW& d) {
    if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    std::wstring dir = root + L"\\" + d.cFileName;
    ListDir(dir, L"*.jsonl", [&](const WIN32_FIND_DATAW& f) {
      if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return;
      if (wcsncmp(f.cFileName, L"agent-", 6) == 0) return;  // subagent logs of older versions
      if (now - FileTimeToUnixMs(f.ftLastWriteTime) > window) return;
      std::wstring path = dir + L"\\" + f.cFileName;
      Track& t = g_tracks[path];
      if (t.path.empty()) {
        std::wstring stem = f.cFileName;
        stem.resize(stem.size() - 6);
        t.path = path;
        t.id = Narrow(stem);
      }
    });
  });
}

void ScanDesktopMeta(int64_t now) {
  std::wstring root = EnvDir(L"APPDATA") + L"\\Claude\\claude-code-sessions";
  ListDir(root, L"*", [&](const WIN32_FIND_DATAW& a) {
    if (!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    std::wstring d1 = root + L"\\" + a.cFileName;
    ListDir(d1, L"*", [&](const WIN32_FIND_DATAW& o) {
      if (!(o.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
      std::wstring d2 = d1 + L"\\" + o.cFileName;
      ListDir(d2, L"local_*.json", [&](const WIN32_FIND_DATAW& f) {
        FileStamp st{FileTimeToUnixMs(f.ftLastWriteTime), ((uint64_t)f.nFileSizeHigh << 32) | f.nFileSizeLow};
        if (now - st.mtime > 3 * 86400000LL) return;
        std::wstring path = d2 + L"\\" + f.cFileName;
        FileStamp& seen = g_metaStamps[path];
        if (seen == st) return;
        // These files run to hundreds of KB of tool config; the fields needed lead the object.
        std::string head;
        if (!ReadHead(path, 16384, head)) return;
        std::string cli;
        if (!JFindString(head, "cliSessionId", cli) || cli.empty()) return;
        seen = st;
        DesktopMeta m;
        JFindString(head, "sessionId", m.hostId);
        JFindString(head, "title", m.title);
        JFindBool(head, "isArchived", m.archived);
        double edge = 0;
        if (JFindNumber(head, "lastTurnEdgeAt", edge)) m.turnEdge = (int64_t)edge;
        g_meta[cli] = std::move(m);
      });
    });
  });
}

void ScanPerms(int64_t now) {
  std::wstring dir = EnvDir(L"USERPROFILE") + L"\\.claude\\pixelbar";
  ListDir(dir, L"perm-*.json", [&](const WIN32_FIND_DATAW& f) {
    std::wstring path = dir + L"\\" + f.cFileName;
    FileStamp st{FileTimeToUnixMs(f.ftLastWriteTime), ((uint64_t)f.nFileSizeHigh << 32) | f.nFileSizeLow};
    if (now - st.mtime > 86400000LL) {
      DeleteFileW(path.c_str());
      g_permStamps.erase(path);
      return;
    }
    FileStamp& seen = g_permStamps[path];
    if (seen == st) return;
    seen = st;
    std::string text;
    if (!ReadAll(path, text)) return;
    JVal j;
    std::string sid, detail;
    if (JParse(text, j)) {
      sid = std::string(j["session_id"].str());
      detail = ToolDetail(j["tool_name"].str(), j["tool_input"]);
    } else {
      JFindString(text, "session_id", sid);
      JFindString(text, "tool_name", detail);
    }
    if (sid.empty()) return;
    PermInfo& p = g_perms[sid];
    if (st.mtime >= p.mtime) p = {st.mtime, detail};
  });
}

void ScanCodex(int64_t now, int64_t window) {
  std::wstring home = EnvDir(L"USERPROFILE") + L"\\.codex";
  FileStamp st;
  std::wstring index = home + L"\\session_index.jsonl";
  if (Stat(index, st) && !(st == g_codexIndexStamp)) {
    g_codexIndexStamp = st;
    std::string all;
    if (ReadAll(index, all))
      for (std::string_view line : Lines(all, false)) {
        JVal j;
        if (!JParse(line, j)) continue;
        std::string_view id = j["id"].str(), name = j["thread_name"].str();
        if (!id.empty() && !name.empty()) g_codexNames[std::string(id)] = std::string(name);
      }
  }
  std::wstring newest;
  FileStamp newestStamp;
  std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& dir, int depth) {
    ListDir(dir, L"*", [&](const WIN32_FIND_DATAW& f) {
      std::wstring p = dir + L"\\" + f.cFileName;
      if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (depth < 3) walk(p, depth + 1);
        return;
      }
      size_t n = wcslen(f.cFileName);
      if (wcsncmp(f.cFileName, L"rollout-", 8) != 0 || n < 6 || wcscmp(f.cFileName + n - 6, L".jsonl") != 0) return;
      FileStamp fst{FileTimeToUnixMs(f.ftLastWriteTime), ((uint64_t)f.nFileSizeHigh << 32) | f.nFileSizeLow};
      if (fst.mtime > newestStamp.mtime) {
        newest = p;
        newestStamp = fst;
      }
      if (now - fst.mtime > window) return;
      Track& t = g_tracks[p];
      if (t.path.empty()) {
        t.path = p;
        t.codex = true;
      }
    });
  };
  walk(home + L"\\sessions", 0);

  // Rate limits outlive the sessions shown, so read them from the newest rollout of any age.
  if (!newest.empty() && (newest != g_newestRollout || !(newestStamp == g_newestRolloutStamp))) {
    g_newestRollout = newest;
    g_newestRolloutStamp = newestStamp;
    std::string tail;
    uint64_t size = 0;
    if (ReadTail(newest, (size_t)2 << 20, tail, &size)) {
      Track probe;
      ReadCodexLimits(probe, Lines(tail, size > tail.size()));
      if (!probe.limits.empty() && probe.limitsAt >= g_codexLimitsAt) {
        g_codexLimits = std::move(probe.limits);
        g_codexLimitsAt = probe.limitsAt;
      }
    }
  }
}

// ---- snapshot ---------------------------------------------------------------

int Rank(AgentState s) {
  switch (s) {
    case AgentState::NeedsYou: return 0;
    case AgentState::Asks: return 1;
    case AgentState::Working: return 2;
    case AgentState::Stale: return 3;
    case AgentState::Done: return 4;
    default: return 5;
  }
}

bool Same(const std::vector<Agent>& a, const std::vector<Agent>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (a[i].key != b[i].key || a[i].state != b[i].state || a[i].sinceMs != b[i].sinceMs ||
        a[i].detail != b[i].detail || a[i].title != b[i].title)
      return false;
  return true;
}

void Publish(int64_t now) {
  std::vector<Agent> out;
  for (auto& [path, t] : g_tracks) {
    if (!t.known || (t.codex ? !g_cfg.codex : !g_cfg.claude)) continue;
    Agent a;
    a.codex = t.codex;
    a.key = t.id;
    a.state = t.state;
    a.detail = t.detail;
    a.lastWriteMs = t.stamp.mtime;
    a.sinceMs = t.state == AgentState::Working ? t.workingSinceMs : t.sinceMs;
    if (!t.codex) {
      auto m = g_meta.find(t.id);
      if (m != g_meta.end()) {
        if (m->second.archived) continue;
        a.title = m->second.title;
        if (!m->second.hostId.empty()) a.link = L"claude://code/continue?session=" + Widen(m->second.hostId);
        if (a.state == AgentState::Working && m->second.turnEdge > 0 && m->second.turnEdge <= now)
          a.sinceMs = m->second.turnEdge;
      }
      if (t.pendingTool) {
        auto p = g_perms.find(t.id);
        if (p != g_perms.end() && p->second.mtime >= t.stamp.mtime - 250) {
          a.state = AgentState::NeedsYou;
          a.detail = "approve " + p->second.detail;
          a.sinceMs = p->second.mtime;
        }
      }
    } else {
      auto n = g_codexNames.find(t.id);
      if (n != g_codexNames.end()) a.title = n->second;
      if (!t.id.empty()) a.link = L"codex://threads/" + Widen(t.id);
    }
    if (a.title.empty()) a.title = ProjectName(t.cwd);
    if (a.title.empty()) a.title = t.codex ? "codex" : "claude";
    if (a.sinceMs <= 0) a.sinceMs = a.lastWriteMs;
    if (a.state == AgentState::Working && now - a.lastWriteMs > g_cfg.staleMinutes * 60000LL) {
      a.state = AgentState::Stale;
      a.sinceMs = a.lastWriteMs;
    }
    bool live = a.state == AgentState::NeedsYou || a.state == AgentState::Asks || a.state == AgentState::Working;
    if (!live && now - a.sinceMs > g_cfg.doneMinutes * 60000LL) continue;
    out.push_back(std::move(a));
  }
  std::sort(out.begin(), out.end(), [](const Agent& x, const Agent& y) {
    int rx = Rank(x.state), ry = Rank(y.state);
    if (rx != ry) return rx < ry;
    if (rx >= 4) return x.sinceMs > y.sinceMs;  // finished: newest first
    if (x.title != y.title) return x.title < y.title;
    return x.key < y.key;
  });
  for (auto& [path, t] : g_tracks)
    if (t.codex && !t.limits.empty() && t.limitsAt > g_codexLimitsAt) {
      g_codexLimits = t.limits;
      g_codexLimitsAt = t.limitsAt;
    }
  UsageState usage;
  usage.claude = g_claudeLimits;
  usage.claudeAt = g_claudeLimitsAt;
  usage.codex = g_codexLimits;
  usage.codexAt = g_codexLimitsAt;
  bool changed;
  {
    std::lock_guard lk(g_mu);
    changed = !Same(out, g_snapshot) || usage.claudeAt != g_usage.claudeAt || usage.codexAt != g_usage.codexAt;
    g_snapshot = std::move(out);
    g_usage = std::move(usage);
  }
  if (changed && g_hwnd) PostMessageW(g_hwnd, WM_PB_REFRESH, 0, 0);
}

void Run() {
  int64_t lastScan = 0;
  for (;;) {
    int64_t now = NowMs();
    int64_t window = (int64_t)std::max(g_cfg.doneMinutes, g_cfg.staleMinutes) * 60000 + 120000;
    if (g_rescan.exchange(false) || now - lastScan > 4000) {
      lastScan = now;
      if (g_cfg.claude) {
        ScanDesktopMeta(now);
        ScanClaude(now, window);
        ScanPerms(now);
        ScanClaudePlan();
      }
      if (g_cfg.codex) ScanCodex(now, window);
      for (auto it = g_tracks.begin(); it != g_tracks.end();) {
        if (it->second.known && now - it->second.stamp.mtime > window)
          it = g_tracks.erase(it);
        else
          ++it;
      }
    }
    for (auto& [path, t] : g_tracks) {
      FileStamp st;
      if (!Stat(path, st)) continue;
      if (t.known && st == t.stamp) continue;
      t.stamp = st;
      try {
        Parse(t);
      } catch (...) {
        t.known = false;
      }
    }
    Publish(now);
    WaitForSingleObject(g_wake, 1000);
  }
}

}  // namespace

void AgentsStart(HWND notify) {
  g_hwnd = notify;
  g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::thread(Run).detach();
}

std::vector<Agent> AgentsSnapshot() {
  std::lock_guard lk(g_mu);
  return g_snapshot;
}

UsageState UsageSnapshot() {
  std::lock_guard lk(g_mu);
  return g_usage;
}

void AgentsRescan() {
  g_rescan = true;
  if (g_wake) SetEvent(g_wake);
}
