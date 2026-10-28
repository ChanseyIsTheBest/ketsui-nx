// Ketsui: profile collection of the PGO-instrumented Switch build (KETSUI_PGO=generate), see ketsui_pgo.cpp.
// In every other build these do nothing.

#pragma once

namespace ketsui::pgo {

// Picks this session's file (pgo/ketsui_pgo_<N>.stream next to the executable) and starts the 2-minute dumps.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
// The last dump, then the dump thread ends.
void Stop();

}  // namespace ketsui::pgo
