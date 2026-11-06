#pragma once

#include <cstdint>
#include <string>

namespace hc09::debug
{
    // Records the calling host thread so its stack can be dumped later.
    void RegisterThread(uint32_t guestThreadId);

    // Suspends every registered thread and prints its host call stack. Recompiled
    // functions are named sub_<guest address>, so this reads as a guest call stack.
    void DumpAllStacks();

    // Prints the call stack captured in `context` (for exception handlers).
    void PrintStackFromContext(const void* context);

    // Prints the calling thread's own call stack.
    void PrintCurrentStack();

    // The call stack captured in `context`, one frame per line, as PrintStackFromContext prints it.
    std::string StackText(const void* context);

    // HC09_TABLE_WRITES=DCHT,PLRM (table_writes.cpp): write-protects those tables' records in every
    // database in guest memory and logs each distinct writer stack. `reason` names the trigger.
    void ArmTableWrites(const char* reason);

    // If HC09_DUMP_STACKS=<seconds> is set, dumps all stacks after that delay.
    // If HC09_WATCH=<guest address>[,...] is set (up to 4), every guest thread gets a
    // hardware write breakpoint on those bytes; each write logs the value and the writer's stack.
    void StartWatchdogFromEnvironment();
}
