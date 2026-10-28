// Ketsui's late input (futari-nx's futari_late_input and guwange-nx's, for Ketsui's frame; the user, 2026-10-22 and
// 2026-10-27: "apply mushi and guwange's late input").
//
// Ketsui's main loop (sub_8223F908, from the entry point) runs a frame (the game and its draws, about 0.5 ms on the PC),
// Swaps, and then polls D3D's busy test until the frame's fence is written (`while (sub_828E0058(device)) Sleep(0)`),
// reading the controllers now and then while it waits. On the console that fence is written after the flip, at the
// vblank, so the loop runs once per vblank: the frame starts right after a vblank and its image is shown at the next.
// With Renut's banjo_renut_swap_at_vblank the same (the latency trace, ketsui_diag_latency_trace_s, 2026-10-27: guest
// vblank, Clear 0.07 ms later, the frame's controller read 0.08 ms, Swap 0.5 ms; then the wait). The input of a frame
// is therefore about a whole frame old when it is shown, and could be read that much later.
//
// The frame's first controller read after its fence is written (rex::input::SetFrameReadDelayKeyFunction with Renut's
// count of written Swap fences; the reads of the wait loop come before it) waits by what the frame can spare: the
// time to the next vblank less the longest work of the last 64 frames (read to the render thread's arrival at the
// Swap's fence, or the last draw) and a margin (ketsui_late_input_margin_us), at most ketsui_late_input_max_us. The
// main thread starts its frame's game work with that read, so the frame moves later with newer input and still has its
// Swap's fence reached before the vblank it waits for. A frame that ran long (or a loading screen) counts as a whole
// period: no delay until it has left the window; no delay either before 8 frames are measured, or when the read is not
// in the first half of the period after a vblank (a frame running late). On the Switch the guest vblank also follows
// the display's refresh, as late after it as the image still makes the next one (futari_vblank_display_sync,
// futari_vblank_phase_auto: futari-nx's): a delay that made a frame miss its refresh moves the vblank earlier there.
// Swap, vblank and the frame's own work are unchanged; only when the controllers are read moves.
//
// Renut's banjo_late_input (render_thread.cpp) stands aside while this is on.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/frame_latency.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

#if FUTARI_RENUT_RENDERER
namespace renut::gpu::render_thread {
uint64_t SwapFencesWritten();  // src/renut_engine/gpu/backend/render_thread.cpp
}  // namespace renut::gpu::render_thread
#endif

#if FUTARI_RENUT_RENDERER
REXCVAR_DECLARE(bool, futari_late_input);  // src/renut_engine/ketsui_renut_glue.cpp (the shared key name)
#else
// A build without Renut's renderer (the emulated GPU, the PC reference) has no glue file: the key lives here, off
// until Ketsui's own frame is measured.
REXCVAR_DEFINE_BOOL(futari_late_input, false, "Ketsui",
                    "Input lag: the frame's first controller read as late as the frame allows (adaptive)");
#endif

REXCVAR_DEFINE_INT32(ketsui_late_input_margin_us, 3000, "Ketsui/Enhancements",
                     "futari_late_input: microseconds kept free before the vertical blank beyond the longest recent "
                     "frame (larger = fewer missed frames, less gain)")
    .range(500, 16000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ketsui_late_input_max_us, 12000, "Ketsui/Enhancements",
                     "futari_late_input: the longest delay of the frame's controller read, in microseconds")
    .range(0, 15000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// ketsui-nx (2026-10-28): the frame's work ends at the render thread's arrival at the Swap's fence, but the image still
// has its present and the GPU's last work to do before the display's refresh (3-6 ms in handheld mode on the console).
REXCVAR_DEFINE_BOOL(ketsui_late_input_display_guard, true, "Ketsui/Enhancements",
                    "futari_late_input on the Switch: an image not finished on the GPU at its refresh adds 1 ms to the "
                    "margin (up to ketsui_late_input_max_us), 180 images in time in a row take 0.25 ms off again, so "
                    "the controller read never moves so late that frames miss the display")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace ketsui::late_input {
namespace {

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_vblank_ns{0};        // the last guest vblank
std::atomic<uint64_t> g_period_ns{16666667};  // the vblank period, measured
std::atomic<uint64_t> g_last_draw_ns{0};      // the last draw call (the drawing thread)
std::atomic<uint64_t> g_frame_done_ns{0};     // the render thread reached the last Swap's fence (banjo_renut_swap_at_vblank)
uint64_t g_read_ns = 0;                       // when the current frame read its controllers (emulation thread only)
constexpr uint32_t kWindow = 64;
uint64_t g_work_ns[kWindow] = {};  // the last frames' work, read to last draw (emulation thread only)
uint32_t g_work_next = 0;
uint32_t g_work_count = 0;
std::atomic<uint64_t> g_delay_sum_us{0}, g_delays{0}, g_frames{0}, g_work_max_report_ns{0};
// ketsui_late_input_display_guard: the margin's extra, images late at their refresh (the vblank thread writes both).
std::atomic<uint64_t> g_guard_ns{0}, g_late_images{0};
uint32_t g_in_time = 0;  // images in time in a row (vblank thread only)

uint64_t WorkMax() {
  uint64_t m = 0;
  for (uint32_t i = 0; i < g_work_count; ++i) m = std::max(m, g_work_ns[i]);
  return m;
}

// The frame's first controller read after a Swap, on the emulation thread.
uint32_t FrameReadDelay() {
  const uint64_t now = NowNs();
  const uint64_t period = g_period_ns.load(std::memory_order_relaxed);
  // The frame before: its work, from its read to its last draw (all of it before this Swap). None drawn (a loading
  // screen, a frame that skipped its video) or longer than a period: a whole period.
  if (g_read_ns) {
    // Its end: the render thread's arrival at its Swap's fence, which must come before the vblank for the frame to be
    // shown at it (banjo_renut_swap_at_vblank); else its last draw. Not a draw when the render thread reports: Ketsui
    // clears its target before the frame's read, so by this read the newest draw is this frame's own Clear.
    const uint64_t done = g_frame_done_ns.load(std::memory_order_acquire);
    const uint64_t draw = done > g_read_ns ? done : g_last_draw_ns.load(std::memory_order_acquire);
    const uint64_t work = draw > g_read_ns ? std::min(draw - g_read_ns, period) : period;
    g_work_ns[g_work_next] = work;
    g_work_next = (g_work_next + 1) % kWindow;
    g_work_count = std::min(g_work_count + 1, kWindow);
    g_work_max_report_ns.store(std::max(g_work_max_report_ns.load(std::memory_order_relaxed), work),
                               std::memory_order_relaxed);
  }
  static const uint64_t margin = uint64_t(REXCVAR_GET(ketsui_late_input_margin_us)) * 1000;
  static const uint64_t most = uint64_t(REXCVAR_GET(ketsui_late_input_max_us)) * 1000;
  const uint64_t vblank = g_vblank_ns.load(std::memory_order_acquire);
  uint64_t delay = 0;
  if (vblank && g_work_count >= 8 && now >= vblank && now - vblank < period / 2) {
    const uint64_t next = vblank + period;
    const uint64_t need = WorkMax() + margin + g_guard_ns.load(std::memory_order_relaxed);
    if (next > now + need) delay = std::min(next - now - need, most);
  }
  g_read_ns = now + delay;
  g_frames.fetch_add(1, std::memory_order_relaxed);
  if (delay) {
    g_delay_sum_us.fetch_add(delay / 1000, std::memory_order_relaxed);
    g_delays.fetch_add(1, std::memory_order_relaxed);
  }
  return uint32_t(delay / 1000);
}

// What starts a frame: the Swap fences Renut has written at their vblank (banjo_renut_swap_at_vblank), or the Swaps
// before the first (the emulated GPU, or that key off).
uint64_t FrameKey() {
#if FUTARI_RENUT_RENDERER
  if (const uint64_t written = renut::gpu::render_thread::SwapFencesWritten()) return written | (uint64_t(1) << 63);
#endif
  return rex::latency::SwapCount();
}

}  // namespace

// The guest vblank (src/renut_engine/futari_renut_vblank.cpp, the vblank thread): its time and period.
void NoteVblank() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  const uint64_t now = NowNs();
  const uint64_t last = g_vblank_ns.exchange(now, std::memory_order_acq_rel);
  if (last && now > last) {
    const uint64_t interval = now - last;
    // One missed vblank (a stall) is not the period; a smoothed period of the plausible intervals.
    if (interval > 10000000 && interval < 25000000) {
      const uint64_t old = g_period_ns.load(std::memory_order_relaxed);
      g_period_ns.store((old * 15 + interval) / 16, std::memory_order_relaxed);
    }
  }
}

// A new image at the display's refresh (the vblank thread, futari_vblank_display_sync): finished on the GPU or not.
void NoteDisplayed(bool in_time) {
  static const bool guard = REXCVAR_GET(ketsui_late_input_display_guard);
  if (!g_on.load(std::memory_order_relaxed) || !guard) return;
  static const uint64_t most = uint64_t(REXCVAR_GET(ketsui_late_input_max_us)) * 1000;
  const uint64_t extra = g_guard_ns.load(std::memory_order_relaxed);
  if (!in_time) {
    g_guard_ns.store(std::min(extra + 1000000, most), std::memory_order_relaxed);
    g_late_images.fetch_add(1, std::memory_order_relaxed);
    g_in_time = 0;
  } else if (++g_in_time >= 180) {
    g_in_time = 0;
    g_guard_ns.store(extra > 250000 ? extra - 250000 : 0, std::memory_order_relaxed);
  }
}

// A draw call of the game (the drawing thread: Renut's draw hooks).
void NoteDraw() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  g_last_draw_ns.store(NowNs(), std::memory_order_release);
}

// The render thread has reached a Swap's fence: the frame's work is done there (render_thread.cpp).
void NoteFrameDone() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  g_frame_done_ns.store(NowNs(), std::memory_order_release);
}

// After the configuration is read (src/ketsui_app.h).
void Install() {
  if (!REXCVAR_GET(futari_late_input)) return;
  g_on.store(true, std::memory_order_relaxed);
  rex::input::SetFrameReadDelayKeyFunction(&FrameKey);
  rex::input::SetFrameReadDelayFunction(&FrameReadDelay);
  REXLOG_INFO("[latency] late input: the frame's first controller read (after its Swap's fence is written) waits by "
              "what the frame can spare (margin {} us, at most {} us; futari_late_input)",
              REXCVAR_GET(ketsui_late_input_margin_us), REXCVAR_GET(ketsui_late_input_max_us));
}

// For the "[latency]" line (render_thread.cpp's LateInputReport).
std::string Report() {
  const uint64_t frames = g_frames.exchange(0, std::memory_order_relaxed);
  const uint64_t delays = g_delays.exchange(0, std::memory_order_relaxed);
  const uint64_t total = g_delay_sum_us.exchange(0, std::memory_order_relaxed);
  const uint64_t work = g_work_max_report_ns.exchange(0, std::memory_order_relaxed);
  if (!frames) return "late input: no frames";
  const uint64_t late = g_late_images.exchange(0, std::memory_order_relaxed);
  return fmt::format("late input: the controller read delayed {:.1f} ms on average over {} frames ({} delayed; "
                     "longest frame read -> last draw {:.1f} ms; display guard +{:.2f} ms, {} images late at their "
                     "refresh)",
                     double(total) / 1000.0 / double(frames), frames, delays, double(work) / 1e6,
                     double(g_guard_ns.load(std::memory_order_relaxed)) / 1e6, late);
}

}  // namespace ketsui::late_input
