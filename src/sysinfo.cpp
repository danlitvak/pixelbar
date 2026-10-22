// CPU, per-core load, GPU, RAM, disks, processes, network throughput and latency.
#include "pixelbar.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr size_t kHistory = 320;

uint64_t U64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

void Push(std::vector<float>& h, float v) {
  h.push_back(v);
  if (h.size() > kHistory) h.erase(h.begin(), h.begin() + (h.size() - kHistory));
}

using NtQsiFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
NtQsiFn g_ntqsi = nullptr;

// ---- cpu
uint64_t g_idle = 0, g_kernel = 0, g_user = 0;
uint64_t g_selfBusy = 0;  // this process's CPU time at the last sample
int64_t g_selfAt = 0;

struct CorePerf {
  LARGE_INTEGER idle, kernel, user, dpc, interrupt;
  ULONG interruptCount;
};
std::vector<CorePerf> g_corePrev;

std::string TidyCpuName(std::string s) {
  for (const char* junk : {"(R)", "(TM)", "(tm)", "(r)"}) {
    size_t p;
    while ((p = s.find(junk)) != std::string::npos) s.erase(p, strlen(junk));
  }
  std::string out, word;
  auto flush = [&] {
    if (word.empty()) return;
    bool drop = word == "CPU" || word == "Processor" || word == "with" || word == "Radeon" || word == "Graphics" ||
                word == "Gen" || word == "Intel" || word == "AMD" || word == "Core" || word.rfind("@", 0) == 0 ||
                word.find("GHz") != std::string::npos ||
                (word.size() > 5 && word.compare(word.size() - 5, 5, "-Core") == 0) ||
                (word.size() > 2 && isdigit((unsigned char)word[0]) &&
                 (word.compare(word.size() - 2, 2, "th") == 0 || word.compare(word.size() - 2, 2, "nd") == 0 ||
                  word.compare(word.size() - 2, 2, "rd") == 0 || word.compare(word.size() - 2, 2, "st") == 0));
    if (!drop) {
      if (!out.empty()) out += ' ';
      out += word;
    }
    word.clear();
  };
  for (char c : s) {
    if (c == ' ') flush();
    else word += c;
  }
  flush();
  return out;
}

std::string CpuName() {
  wchar_t buf[256];
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                   RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
    return {};
  return TidyCpuName(Narrow(buf));
}

void SampleCores(std::vector<float>& out) {
  if (!g_ntqsi) return;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  size_t n = std::min<size_t>(si.dwNumberOfProcessors, 64);
  std::vector<CorePerf> cur(n);
  ULONG ret = 0;
  if (g_ntqsi(8 /* SystemProcessorPerformanceInformation */, cur.data(), (ULONG)(n * sizeof(CorePerf)), &ret) != 0)
    return;
  n = std::min<size_t>(n, ret / sizeof(CorePerf));
  out.assign(n, 0.f);
  if (g_corePrev.size() == n) {
    for (size_t i = 0; i < n; i++) {
      int64_t di = cur[i].idle.QuadPart - g_corePrev[i].idle.QuadPart;
      int64_t dk = cur[i].kernel.QuadPart - g_corePrev[i].kernel.QuadPart;
      int64_t du = cur[i].user.QuadPart - g_corePrev[i].user.QuadPart;
      int64_t total = dk + du;  // kernel time includes idle
      out[i] = total > 0 ? std::clamp((float)(total - di) / (float)total, 0.f, 1.f) : 0.f;
    }
  }
  cur.resize(n);
  g_corePrev = std::move(cur);
}

// ---- gpu: NVIDIA's NVML when the driver ships it, else the PDH "3D" engine counter
struct NvmlMemory {
  unsigned long long total, free, used;
};
struct NvmlUtil {
  unsigned gpu, memory;
};
using NvDev = void*;
struct Nvml {
  int (*init)() = nullptr;
  int (*handle)(unsigned, NvDev*) = nullptr;
  int (*name)(NvDev, char*, unsigned) = nullptr;
  int (*temp)(NvDev, int, unsigned*) = nullptr;
  int (*mem)(NvDev, NvmlMemory*) = nullptr;
  int (*power)(NvDev, unsigned*) = nullptr;
  int (*fan)(NvDev, unsigned*) = nullptr;
  int (*util)(NvDev, NvmlUtil*) = nullptr;
  NvDev dev = nullptr;
} g_nv;

bool NvmlInit(std::string& name) {
  HMODULE h = LoadLibraryW(L"nvml.dll");
  if (!h) return false;
  auto get = [&](const char* fn) { return (void*)GetProcAddress(h, fn); };
  g_nv.init = (int (*)())get("nvmlInit_v2");
  g_nv.handle = (int (*)(unsigned, NvDev*))get("nvmlDeviceGetHandleByIndex_v2");
  g_nv.name = (int (*)(NvDev, char*, unsigned))get("nvmlDeviceGetName");
  g_nv.temp = (int (*)(NvDev, int, unsigned*))get("nvmlDeviceGetTemperature");
  g_nv.mem = (int (*)(NvDev, NvmlMemory*))get("nvmlDeviceGetMemoryInfo");
  g_nv.power = (int (*)(NvDev, unsigned*))get("nvmlDeviceGetPowerUsage");
  g_nv.fan = (int (*)(NvDev, unsigned*))get("nvmlDeviceGetFanSpeed");
  g_nv.util = (int (*)(NvDev, NvmlUtil*))get("nvmlDeviceGetUtilizationRates");
  if (!g_nv.init || !g_nv.handle || !g_nv.util || g_nv.init() != 0 || g_nv.handle(0, &g_nv.dev) != 0) {
    g_nv.dev = nullptr;
    return false;
  }
  char buf[96] = {};
  if (g_nv.name && g_nv.name(g_nv.dev, buf, sizeof buf) == 0) {
    name = buf;
    for (const char* junk : {"NVIDIA ", "GeForce "}) {
      size_t p = name.find(junk);
      if (p != std::string::npos) name.erase(p, strlen(junk));
    }
  }
  return true;
}

PDH_HQUERY g_query = nullptr;
PDH_HCOUNTER g_gpuCounter = nullptr;

void PdhGpuInit() {
  if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) return;
  if (PdhAddEnglishCounterW(g_query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &g_gpuCounter) !=
      ERROR_SUCCESS) {
    PdhCloseQuery(g_query);
    g_query = nullptr;
    return;
  }
  PdhCollectQueryData(g_query);
}

bool PdhGpuSample(float& out) {
  if (!g_query || PdhCollectQueryData(g_query) != ERROR_SUCCESS) return false;
  DWORD size = 0, count = 0;
  PDH_STATUS st = PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
  if (st != (PDH_STATUS)PDH_MORE_DATA) {
    out = 0;
    return st == ERROR_SUCCESS || st == (PDH_STATUS)PDH_INVALID_DATA || st == (PDH_STATUS)PDH_NO_DATA;
  }
  std::vector<uint8_t> buf(size);
  auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
  if (PdhGetFormattedCounterArrayW(g_gpuCounter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) !=
      ERROR_SUCCESS)
    return false;
  std::unordered_map<std::wstring, double> perEngine;  // key: luid_..._phys_N_eng_M
  for (DWORD i = 0; i < count; i++) {
    if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
      continue;
    std::wstring name = items[i].szName ? items[i].szName : L"";
    size_t at = name.find(L"luid_");
    perEngine[at == std::wstring::npos ? name : name.substr(at)] += items[i].FmtValue.doubleValue;
  }
  double best = 0;
  for (auto& kv : perEngine) best = std::max(best, kv.second);
  out = (float)std::clamp(best / 100.0, 0.0, 1.0);
  return true;
}

// ---- processes, grouped by image name (Chrome's thirty processes are one row)
struct ProcRaw {  // SYSTEM_PROCESS_INFORMATION, x64 layout
  ULONG NextEntryOffset;
  ULONG NumberOfThreads;
  LARGE_INTEGER WorkingSetPrivateSize;
  ULONG HardFaultCount;
  ULONG NumberOfThreadsHighWatermark;
  ULONGLONG CycleTime;
  LARGE_INTEGER CreateTime;
  LARGE_INTEGER UserTime;
  LARGE_INTEGER KernelTime;
  USHORT NameLength;
  USHORT NameMaximumLength;
  PWSTR NameBuffer;
  LONG BasePriority;
  HANDLE UniqueProcessId;
};
std::vector<uint8_t> g_procBuf;
std::unordered_map<uint64_t, int64_t> g_procPrev;  // (pid, start) -> cpu time, 100 ns
int64_t g_procPrevAt = 0;

void SampleProcesses(std::vector<ProcInfo>& out) {
  if (!g_ntqsi) return;
  if (g_procBuf.empty()) g_procBuf.resize(1 << 20);
  ULONG need = 0;
  LONG st;
  while ((st = g_ntqsi(5 /* SystemProcessInformation */, g_procBuf.data(), (ULONG)g_procBuf.size(), &need)) ==
         (LONG)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */)
    g_procBuf.resize(std::max<size_t>(need, g_procBuf.size()) + (64 << 10));
  if (st != 0) return;

  int64_t now = NowMs();
  double window = (now - g_procPrevAt) * 10000.0 * std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
  std::unordered_map<std::wstring, size_t> index;
  std::unordered_map<uint64_t, int64_t> cur;
  out.clear();
  for (size_t off = 0;;) {
    auto* p = (const ProcRaw*)(g_procBuf.data() + off);
    if (p->NameBuffer && p->NameLength) {
      std::wstring name(p->NameBuffer, p->NameLength / sizeof(wchar_t));
      if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
      std::wstring key = name;
      for (auto& ch : key) ch = (wchar_t)towlower(ch);
      auto [it, fresh] = index.emplace(key, out.size());
      if (fresh) out.push_back({Narrow(name), 0, 0, 0});
      ProcInfo& g = out[it->second];
      g.count++;
      g.mem += (uint64_t)std::max<LONGLONG>(0, p->WorkingSetPrivateSize.QuadPart);
      uint64_t id = ((uint64_t)(uintptr_t)p->UniqueProcessId << 32) ^ (uint64_t)p->CreateTime.QuadPart;
      int64_t cpu = p->UserTime.QuadPart + p->KernelTime.QuadPart;
      cur[id] = cpu;
      auto prev = g_procPrev.find(id);
      if (prev != g_procPrev.end() && g_procPrevAt && window > 0)
        g.cpu += (float)std::clamp((cpu - prev->second) / window, 0.0, 1.0);
    }
    if (!p->NextEntryOffset) break;
    off += p->NextEntryOffset;
  }
  g_procPrev.swap(cur);
  g_procPrevAt = now;
  std::sort(out.begin(), out.end(), [](const ProcInfo& a, const ProcInfo& b) { return a.mem > b.mem; });
}

void SampleDisks(std::vector<DiskInfo>& out) {
  out.clear();
  DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; i++) {
    if (!(mask & (1u << i))) continue;
    wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
    if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
    ULARGE_INTEGER freeToUs, total, totalFree;
    if (!GetDiskFreeSpaceExW(root, &freeToUs, &total, &totalFree) || total.QuadPart == 0) continue;
    out.push_back({std::string(1, (char)('A' + i)) + ":", total.QuadPart, totalFree.QuadPart});
  }
}

// ---- network
struct IfBytes {
  uint64_t in = 0, out = 0;
};
std::unordered_map<uint64_t, IfBytes> g_ifPrev;
int64_t g_netPrevMs = 0;

void SampleNet(SysState& st) {
  PMIB_IF_TABLE2 t = nullptr;
  if (GetIfTable2(&t) != NO_ERROR || !t) return;
  int64_t now = NowMs();
  uint64_t din = 0, dout = 0, bestBytes = 0;
  ULONG bestType = 0;
  std::unordered_map<uint64_t, IfBytes> cur;
  for (ULONG i = 0; i < t->NumEntries; i++) {
    const MIB_IF_ROW2& r = t->Table[i];
    if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface) continue;
    if (r.OperStatus != IfOperStatusUp || r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    uint64_t key = r.InterfaceLuid.Value;
    cur[key] = {r.InOctets, r.OutOctets};
    auto p = g_ifPrev.find(key);
    if (p != g_ifPrev.end()) {
      if (r.InOctets >= p->second.in) din += r.InOctets - p->second.in;
      if (r.OutOctets >= p->second.out) dout += r.OutOctets - p->second.out;
    }
    if (r.InOctets + r.OutOctets > bestBytes) {
      bestBytes = r.InOctets + r.OutOctets;
      bestType = r.Type;
    }
  }
  FreeMibTable(t);
  double dt = (now - g_netPrevMs) / 1000.0;
  if (g_netPrevMs && dt > 0.05) {
    st.down = din / dt;
    st.up = dout / dt;
  }
  g_ifPrev.swap(cur);
  g_netPrevMs = now;
  st.netKind = bestType == IF_TYPE_IEEE80211 ? "WIFI" : bestType == IF_TYPE_ETHERNET_CSMACD ? "ETH" : bestType ? "NET" : "";
}

// ---- latency, on its own thread: ICMP first, TCP connect to :443 where ICMP is filtered
std::atomic<int> g_pingMs{-1};
std::atomic<int64_t> g_pingOkAt{0};

int TcpPing(in_addr addr, int timeoutMs) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return -1;
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(443);
  sa.sin_addr = addr;
  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  connect(s, (sockaddr*)&sa, sizeof sa);
  fd_set w, e;
  FD_ZERO(&w);
  FD_ZERO(&e);
  FD_SET(s, &w);
  FD_SET(s, &e);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  int r = select(0, nullptr, &w, &e, &tv);
  QueryPerformanceCounter(&t1);
  int rtt = -1;
  if (r > 0 && FD_ISSET(s, &w)) rtt = (int)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart);
  closesocket(s);
  return rtt;
}

void PingLoop() {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  HANDLE icmp = IcmpCreateFile();
  for (;;) {
    std::string host = ConfigCopy().pingHost;
    in_addr addr{};
    bool have = InetPtonA(AF_INET, host.c_str(), &addr) == 1;
    if (!have) {
      addrinfo hints{};
      hints.ai_family = AF_INET;
      addrinfo* res = nullptr;
      if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
        addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
        have = true;
        freeaddrinfo(res);
      }
    }
    int rtt = -1;
    if (have && icmp != INVALID_HANDLE_VALUE) {
      char data[8] = "pixelbr";
      alignas(8) char reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 64];
      if (IcmpSendEcho(icmp, addr.S_un.S_addr, data, sizeof data, nullptr, reply, sizeof reply, 1500) > 0) {
        auto* r = (ICMP_ECHO_REPLY*)reply;
        if (r->Status == IP_SUCCESS) rtt = (int)r->RoundTripTime;
      }
    }
    if (rtt < 0 && have) rtt = TcpPing(addr, 1500);
    if (rtt >= 0) {
      g_pingMs = rtt;
      g_pingOkAt = NowMs();
    }
    Sleep(3000);
  }
}

std::string g_gpuName;
int g_tick = 0;

}  // namespace

void SysInit() {
  g_ntqsi = (NtQsiFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
  if (g_cfg.gpu && !NvmlInit(g_gpuName)) PdhGpuInit();
  std::thread(PingLoop).detach();
}

void SysSample(SysState& st) {
  if (st.cpuName.empty()) st.cpuName = CpuName();
  st.threads = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  st.uptimeMs = (int64_t)GetTickCount64();

  FILETIME fi, fk, fu;
  if (GetSystemTimes(&fi, &fk, &fu)) {
    uint64_t idle = U64(fi), kernel = U64(fk), user = U64(fu);
    if (g_kernel) {
      uint64_t di = idle - g_idle, total = (kernel - g_kernel) + (user - g_user);
      st.cpu = total ? std::clamp((float)(total - di) / (float)total, 0.f, 1.f) : 0.f;
      Push(st.cpuHist, st.cpu);
    }
    g_idle = idle;
    g_kernel = kernel;
    g_user = user;
  }
  SampleCores(st.cores);

  st.gpuOk = false;
  if (g_cfg.gpu && g_nv.dev) {
    NvmlUtil u{};
    if (g_nv.util(g_nv.dev, &u) == 0) {
      st.gpuOk = true;
      st.gpu = u.gpu / 100.f;
      st.gpuName = g_gpuName;
      unsigned v = 0;
      st.gpuTemp = g_nv.temp && g_nv.temp(g_nv.dev, 0, &v) == 0 ? (int)v : -1;
      st.gpuFan = g_nv.fan && g_nv.fan(g_nv.dev, &v) == 0 ? (int)v : -1;
      st.gpuWatts = g_nv.power && g_nv.power(g_nv.dev, &v) == 0 ? (int)(v / 1000) : -1;
      NvmlMemory m{};
      if (g_nv.mem && g_nv.mem(g_nv.dev, &m) == 0) {
        st.vramUsed = m.used;
        st.vramTotal = m.total;
      }
    }
  } else if (g_cfg.gpu) {
    float gpu = 0;
    st.gpuOk = PdhGpuSample(gpu);
    st.gpu = gpu;
  }
  if (st.gpuOk) Push(st.gpuHist, st.gpu);

  MEMORYSTATUSEX ms{sizeof ms};
  if (GlobalMemoryStatusEx(&ms)) {
    st.ramTotal = ms.ullTotalPhys;
    st.ramUsed = ms.ullTotalPhys - ms.ullAvailPhys;
  }

  if (g_tick % 2 == 0) SampleProcesses(st.procs);
  if (g_tick % 5 == 0) SampleDisks(st.disks);  // often enough to catch a disk filling in minutes
  g_tick++;

  SampleNet(st);
  Push(st.downHist, (float)st.down);
  Push(st.upHist, (float)st.up);

  st.online = NowMs() - g_pingOkAt.load() < 10000;
  st.pingMs = st.online ? g_pingMs.load() : -1;

  // What the column itself costs, measured the way the process list (and Task Manager)
  // measures everything else: CPU as a share of all logical processors, private working set.
  FILETIME created, exited, kernel, user;
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    uint64_t busy = U64(kernel) + U64(user);
    int64_t now = NowMs();
    if (g_selfAt && now > g_selfAt)
      st.selfCpu = (float)std::clamp((busy - g_selfBusy) / ((now - g_selfAt) * 10000.0 * std::max(1, st.threads)), 0.0, 1.0);
    g_selfBusy = busy;
    g_selfAt = now;
  }
  PROCESS_MEMORY_COUNTERS_EX2 pm{};
  pm.cb = sizeof pm;
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm))
    st.selfMem = pm.PrivateWorkingSetSize;
}
