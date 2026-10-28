// Ketsui (2026-10-27): the registry of the native replacements of hot guest functions (bejeweled3-nx's
// bejeweled3_enhancements.h, its natives' part, for src/cpu/ketsui_hot_guest.h and the localized copies of
// tools/localize_native.py). Each native is a CPU enhancement with its own key (KETSUI_NATIVE, ketsui_cpu_native_* for
// the copies), resolved with the CPU master (ketsui_cpu_enhancements) by ResolveNatives(), which
// futari::enhancements::Apply() (src/ketsui_enhancements.cpp) calls before any guest thread runs.
#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

namespace ketsui::enhancements {

struct Native {
  const char* key;
  const char* what;
  bool (*own)();
  // Kept from bejeweled3-nx's struct (its renderer enhancements); Ketsui's natives leave them false.
  bool ring = false;
  bool gpu = false;
  bool active = false;  // own key AND the CPU master (ResolveNatives)
  std::atomic<uint64_t> uses{0};
  void Use(uint64_t n = 1) { uses.fetch_add(n, std::memory_order_relaxed); }
  std::atomic<uint32_t> sampled{0};
  void UseSampled() {
    const uint32_t n = sampled.load(std::memory_order_relaxed) + 1;
    sampled.store(n, std::memory_order_relaxed);
    if ((n & 255) == 0) Use(256);
  }
};
bool RegisterNative(Native* n);

// Resolves every registered native's `active` (its key and the CPU master), sets verify mode (ketsui_cpu_verify) and
// the hooks' dispatch (ketsui::hot::ConfigureDispatch), logs the "[hot] natives" line and starts the 10 s "[hot]"
// report. Once, from futari::enhancements::Apply().
void ResolveNatives();

// Verify mode (ketsui_cpu_verify): every call of a guarded native is checked against the recompiled original.
bool VerifyMode();
inline bool g_verify_mode = false;
void VerifyCheck(uint64_t n = 1);
void VerifyDifference();
// Time spent in guarded calls (ketsui::hot::Check), for the "[hot]" report.
void CheckTime(uint64_t ns);

// True when the loaded guest code at `address` (`bytes` long) has the FNV-1a 64 hash a native was written from
// (tools/localize_native.py). Logs a warning naming `what` when it does not.
bool GuestCodeMatches(const uint8_t* base, uint32_t address, uint32_t bytes, uint64_t fnv, const char* what);

}  // namespace ketsui::enhancements

#define KETSUI_NATIVE(VAR, KEY, DESC)                                                     \
  REXCVAR_DEFINE_BOOL(KEY, true, "Ketsui natives", DESC);                                 \
  ::ketsui::enhancements::Native VAR{#KEY, DESC, [] { return REXCVAR_GET(KEY); }};       \
  [[maybe_unused]] static const bool VAR##_registered_ = ::ketsui::enhancements::RegisterNative(&VAR)
