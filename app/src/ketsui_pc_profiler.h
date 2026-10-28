#pragma once

// PC sampling profiler (cvar ketsui_pc_profile_s); see ketsui_pc_profiler.cpp. Does nothing on the Switch, where the
// SDK's sampler (sdk/src/ui/switch_perf.cpp) does the same job.
namespace ketsui::pc_profiler {

void Start();
void Stop();

}  // namespace ketsui::pc_profiler
