// Ketsui (2026-10-27): the native replacements' registry, code check, verify counters and the 10 s "[hot]" report
// (bejeweled3-nx's bejeweled3_enhancements.cpp, its natives' part; see ketsui_hot_natives.h).

#include "cpu/ketsui_hot_natives.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>

REXCVAR_DECLARE(bool, ketsui_cpu_enhancements);  // src/ketsui_enhancements.cpp
REXCVAR_DECLARE(bool, ketsui_cpu_verify);        // likewise
// All the native replacements at once (each also has its own ketsui_cpu_native_* key), for A/B runs and lockstep
// baselines of the natives against the same build with every other enhancement as it is (bejeweled3_cpu_natives).
REXCVAR_DEFINE_BOOL(ketsui_cpu_natives, true, "Ketsui natives",
                    "The native replacements of hot guest functions (src/cpu/hot, app/generated/hot): false turns all "
                    "of them off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ketsui_hot_report_s, 10, "Ketsui natives",
                     "The \"[hot]\" report of the native replacements' uses and verify checks every N seconds (0 = "
                     "off; the uses are counted only with ketsui_cpu_hook_counters)")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace ketsui::hot {
void ConfigureDispatch(bool verify);  // src/cpu/ketsui_hot_guest.cpp
}  // namespace ketsui::hot

namespace ketsui::enhancements {
namespace {

std::vector<Native*>& Natives() {
  static std::vector<Native*> natives;  // filled at static initialisation (KETSUI_NATIVE)
  return natives;
}

bool g_verify = false;
std::atomic<uint64_t> g_checks{0}, g_differences{0}, g_check_ns{0};

std::mutex g_report_mutex;
std::condition_variable g_report_cv;
bool g_report_stop = false;

void Report(int seconds) {
  uint64_t uses = 0;
  std::string top;
  std::vector<std::pair<uint64_t, const Native*>> used;
  for (Native* n : Natives()) {
    const uint64_t u = n->uses.exchange(0, std::memory_order_relaxed);
    uses += u;
    if (u) used.push_back({u, n});
  }
  std::sort(used.begin(), used.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (size_t k = 0; k < used.size() && k < 8; ++k) {
    top += fmt::format("{}{} {}", k ? ", " : "", used[k].second->key, used[k].first);
  }
  const uint64_t checks = g_checks.exchange(0, std::memory_order_relaxed);
  const uint64_t ns = g_check_ns.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[hot] {} s: {} native calls counted{}{}; verify: {} checks, {} differences in all ({:.1f} ms checking)",
              seconds, uses, top.empty() ? "" : ": ", top, checks, g_differences.load(std::memory_order_relaxed),
              double(ns) / 1e6);
}

}  // namespace

bool RegisterNative(Native* n) {
  Natives().push_back(n);
  return true;
}

void ResolveNatives() {
  const bool master = REXCVAR_GET(ketsui_cpu_enhancements);
  const bool natives = REXCVAR_GET(ketsui_cpu_natives);
  g_verify = REXCVAR_GET(ketsui_cpu_verify);
  g_verify_mode = g_verify;
  size_t on = 0;
  std::string off;
  for (Native* n : Natives()) {
    n->active = master && natives && n->own();
    on += n->active;
    if (!n->active) off += fmt::format("{}{}", off.empty() ? "" : ", ", n->key);
  }
  REXLOG_INFO("[hot] natives: {} of {} on{}{}{}", on, Natives().size(),
              !master ? " (CPU master off)" : !natives ? " (ketsui_cpu_natives off)" : "", off.empty() ? "" : "; off: ",
              off.size() > 400 ? off.substr(0, 400) + " ..." : off);
  ketsui::hot::ConfigureDispatch(g_verify);
  const int seconds = REXCVAR_GET(ketsui_hot_report_s);
  if (seconds > 0 && !Natives().empty()) {
    // Never joined or destroyed: it ends with the process (std::thread::detach throws on the Switch's libnx, and a
    // joinable std::thread destroyed at exit would terminate it).
    static std::thread* report = new std::thread([seconds] {
      std::unique_lock lock(g_report_mutex);
      while (!g_report_cv.wait_for(lock, std::chrono::seconds(seconds), [] { return g_report_stop; })) {
        lock.unlock();
        Report(seconds);
        lock.lock();
      }
    });
    (void)report;
  }
}

bool VerifyMode() { return g_verify; }
void VerifyCheck(uint64_t n) { g_checks.fetch_add(n, std::memory_order_relaxed); }
void VerifyDifference() { g_differences.fetch_add(1, std::memory_order_relaxed); }
void CheckTime(uint64_t ns) { g_check_ns.fetch_add(ns, std::memory_order_relaxed); }

bool GuestCodeMatches(const uint8_t* base, uint32_t address, uint32_t bytes, uint64_t fnv, const char* what) {
#if defined(_WIN32)
  const uint8_t* p = base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  const uint8_t* p = base + address;
#endif
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  if (h != fnv) {
    REXLOG_WARN("[hot] {} at {:08X}: guest code differs (FNV-1a {:016X}, written for {:016X}): recompiled code runs",
                what, address, h, fnv);
    return false;
  }
  return true;
}

}  // namespace ketsui::enhancements
