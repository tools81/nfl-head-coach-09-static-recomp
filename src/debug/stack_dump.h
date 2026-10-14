#pragma once

#include <cstdint>

namespace hc09::debug
{
    // Records the calling host thread so its stack can be dumped later.
    void RegisterThread(uint32_t guestThreadId);

    // Suspends every registered thread and prints its host call stack. Recompiled
    // functions are named sub_<guest address>, so this reads as a guest call stack.
    void DumpAllStacks();

    // Prints the call stack captured in `context` (for exception handlers).
    void PrintStackFromContext(const void* context);

    // If HC09_DUMP_STACKS=<seconds> is set, dumps all stacks after that delay.
    // If HC09_WATCH=<guest address>[,...] is set (up to 4), every guest thread gets a
    // hardware write breakpoint on those bytes; each write logs the value and the writer's stack.
    void StartWatchdogFromEnvironment();
}
