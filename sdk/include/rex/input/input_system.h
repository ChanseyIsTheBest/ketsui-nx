#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

 private:
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;

  // Fork addition: when the controllers were last enumerated, in steady_clock nanoseconds. RefreshDevices used
  // to be called on every GetState, SetState and GetCapabilities from the game (several per frame),
  // allocating memory and querying every driver each time. See RefreshDevices.
  std::atomic<int64_t> last_refresh_ns_{0};

  // ketsui-nx (2026-10-27): Ketsui reads its controllers on two guest threads (its main loop and a task thread), and
  // RefreshDevices rebuilt devices_ and device_owners_ under the other thread's reads: both threads crashed freeing
  // the same DeviceInfo (std::string) in devices_.clear(), 2 of 14 PC runs. Held by GetState, SetState,
  // GetCapabilities and GetKeystroke from their RefreshDevices to their return, and by Shutdown.
  std::mutex devices_mutex_;
};

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

// Input lag (futari-nx, 2026-10-16): a game that reads its controllers right after its Swap and then waits for its next
// vertical blank can read them later. The first controller read after each Swap waits this function's answer, in
// microseconds (input_frame_read_delay_us when none is set), before it samples the controllers. Called on the game's
// thread; nullptr removes it.
void SetFrameReadDelayFunction(uint32_t (*delay_us)());
// ketsui-nx (2026-10-27): what starts a frame for the delay above: the first controller read after this function's
// value changes (by default the game's Swap count). A game that polls its controllers while it waits for its frame's
// fence (Ketsui) starts its frame when the fence is written, not at its Swap. nullptr: the Swap count again.
void SetFrameReadDelayKeyFunction(uint64_t (*key)());

}  // namespace rex::input
