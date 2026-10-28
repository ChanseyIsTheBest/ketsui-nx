// Ketsui's idle task thread (2026-10-27, the optimisation round's first profile).
//
// Ketsui runs a few task threads, each `while (!exit[i]) { if (task[i]()) break; }` (sub_829E0400; the task table at
// 0x8302797C: task 0 sub_829E06A8, task 1 sub_822467B0, task 2 sub_82239320, the controller poll). Task 0 calls the
// function pointer at 0x830279A0 when one is set and returns 0 otherwise:
//
//   lis r11,0x8302; lwz r10,0x79A0(r11); cmplwi cr6,r10,0; beq -> li r3,0; blr   (else bctrl r10; ...)
//
// Nothing set it in a whole session (boot, menus, attract mode, a game), so the thread spun on that test: 97 % of a
// core in a Ryujinx profile (XThread1DC01640, every sample in sub_829E06A8), 99.8 % of a core on the PC. On the Xbox
// 360 that is one of six hardware threads; on the Switch it is one of the three cores the game has, shared with
// Renut's render thread.
//
// ketsui_worker_idle_sleep: while the slot is empty, each call first sleeps 1 ms on the host, then runs the recompiled
// function as it is (which returns 0 for an empty slot). The game sees the same values and the same stores, only later:
// a task set into the slot is picked up within 1 ms. Code-checked (ketsui_native.h).
//
// The loop itself spins too while a task is paused: it calls task[i] only while its flag (0x830279A4 + 4 i) is 0, and
// keeps going round while task[i] is set (a whole core again in a PC profile once task 0 slept: 14 % of every busy
// sample in sub_829E0400, task 0 paused). A mid-function hook at the loop's top (app/ketsui_overrides.toml,
// [[midasm_hook]] 0x829E0430: `lwzx r11,r31,r28`) sleeps 1 ms while the task's flag is set; the loop's own test runs
// after it as before.

#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include "cpu/ketsui_native.h"

REXCVAR_DEFINE_BOOL(ketsui_worker_idle_sleep, true, "Ketsui/Enhancements",
                    "Ketsui's task thread 0 (sub_829E06A8) sleeps 1 ms on each call while its task slot (0x830279A0) "
                    "is empty instead of spinning on it (a whole core); the same calls and results")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_829E06A8);

REX_HOOK_RAW(sub_829E06A8) {
  static ketsui::native::Site site{"ketsui_worker_idle_sleep", "task 0 idle sleep", 0x829E06A8, 18,
                                   0x5B7AC398C7CD63DEull};
  constexpr uint32_t kTaskSlot = 0x830279A0;
  if (ketsui::native::ModeOf(site, base) != ketsui::native::kOriginal) {
    const uint8_t* slot = ketsui::native::Host(base, kTaskSlot);
    if ((slot[0] | slot[1] | slot[2] | slot[3]) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  __imp__sub_829E06A8(ctx, base);
}

// [[midasm_hook]] 0x829E0430 (registers r28, r31): r28 + r31 is the task's pause flag.
void KetsuiTaskPauseHook(PPCRegister& r28, PPCRegister& r31) {
  static const bool on = ketsui::native::FlagOn("ketsui_worker_idle_sleep");
  if (!on) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  const auto* flag = kernel->memory()->TranslateVirtual<const uint8_t*>(uint32_t(r28.u32 + r31.u32));
  if ((flag[0] | flag[1] | flag[2] | flag[3]) != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
