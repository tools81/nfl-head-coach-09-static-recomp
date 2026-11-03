#pragma once
// HC09_PROFILE: sampling profiler of the game's threads by guest function
// (runtime/profiler.cpp; off by default).
#include <cstdint>

// Called on a thread when it starts: makes it known to the profiler (a no-op
// unless HC09_PROFILE is set). tid: the guest thread id (KThread::tid, named by
// the game later), or 0 for a host thread that only has `name`.
void ProfilerRegisterThread(uint64_t tid, const char* name = nullptr);

// Start the sampler thread if HC09_PROFILE is set. Call once the function
// table is installed (InstallFunctionTable).
void ProfilerStart();
