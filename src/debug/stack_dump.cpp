#include "debug/stack_dump.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "memory.h"

namespace hc09::debug
{
    namespace
    {
        struct Entry
        {
            HANDLE handle;
            DWORD hostId;
            uint32_t guestId;
        };

        std::mutex g_mutex;
        std::vector<Entry> g_threads;
        struct Watch
        {
            uint32_t address;
            uint32_t length;  // 1, 2, 4 or 8 bytes
        };
        std::vector<Watch> g_watches;  // guest ranges with write breakpoints (max 4)

        void EnsureSymbols()
        {
            static std::once_flag loaded;
            std::call_once(loaded, [] {
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            });
        }

        void PrintStack(HANDLE process, HANDLE thread, CONTEXT context, std::string* out = nullptr)
        {
            // With `out`, the frames are appended there instead of printed.
            auto emit = [out](const char* format, auto... args) {
                char line[512];
                std::snprintf(line, sizeof(line), format, args...);
                if (out)
                    *out += line;
                else
                    std::fputs(line, stderr);
            };
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;

            alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 256];
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
            for (int depth = 0; depth < 32; depth++)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                    break;
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 displacement = 0;
                if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
                {
                    // Generated code lines locate the guest instruction (see ppc/*.cpp).
                    IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
                    DWORD lineDisplacement = 0;
                    if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line))
                    {
                        const char* file = std::strrchr(line.FileName, '\\');
                        emit("    %s+0x%llX (%s:%lu)\n", symbol->Name, (unsigned long long)displacement,
                            file ? file + 1 : line.FileName, line.LineNumber);
                    }
                    else
                    {
                        emit("    %s+0x%llX\n", symbol->Name, (unsigned long long)displacement);
                    }
                }
                else
                    emit("    0x%016llX\n", (unsigned long long)frame.AddrPC.Offset);
            }
        }

        // Arms DR0..DR3 as write breakpoints on `target` (must not be the caller).
        void ArmWatches(HANDLE target)
        {
            SuspendThread(target);
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            GetThreadContext(target, &context);
            DWORD64* slots[] = { &context.Dr0, &context.Dr1, &context.Dr2, &context.Dr3 };
            for (size_t i = 0; i < g_watches.size() && i < 4; i++)
            {
                // LEN encoding: 00 = 1 byte, 01 = 2, 11 = 4, 10 = 8 (address must be aligned)
                const uint64_t length = g_watches[i].length == 8 ? 2 : g_watches[i].length == 4 ? 3 : g_watches[i].length == 2 ? 1 : 0;
                *slots[i] = reinterpret_cast<DWORD64>(GuestToHost(g_watches[i].address));
                context.Dr7 |= 1ull << (i * 2);              // local enable
                context.Dr7 |= 1ull << (16 + i * 4);         // RW = 01: data writes
                context.Dr7 &= ~(3ull << (18 + i * 4));
                context.Dr7 |= length << (18 + i * 4);
            }
            SetThreadContext(target, &context);
            ResumeThread(target);
        }

        LONG CALLBACK OnWatchHit(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
                return EXCEPTION_CONTINUE_SEARCH;
            const DWORD64 dr6 = info->ContextRecord->Dr6;
            for (size_t i = 0; i < g_watches.size() && i < 4; i++)
            {
                if (!(dr6 & (1ull << i)))
                    continue;
                uint64_t value = 0;
                for (uint32_t b = 0; b < g_watches[i].length; b++)  // big-endian guest bytes
                    value = (value << 8) | GuestToHost(g_watches[i].address)[b];
                // HC09_WATCH_VALUE=<hex>: only writes that leave this value.
                static const char* valueSpec = std::getenv("HC09_WATCH_VALUE");
                if (valueSpec && value != std::strtoull(valueSpec, nullptr, 16))
                    continue;
                EnsureSymbols();
                std::string stack;
                PrintStack(GetCurrentProcess(), GetCurrentThread(), *info->ContextRecord, &stack);
                // Each distinct writer stack is printed once.
                static std::mutex seenMutex;
                static std::vector<std::string> seen;
                {
                    std::lock_guard lock(seenMutex);
                    if (std::find(seen.begin(), seen.end(), stack) != seen.end())
                        continue;
                    seen.push_back(stack);
                }
                std::fprintf(stderr, "[watch] write to 0x%08X, value now 0x%llX, host thread %lu:\n%s",
                    g_watches[i].address, (unsigned long long)value, GetCurrentThreadId(), stack.c_str());
                std::fflush(stderr);
            }
            info->ContextRecord->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // HC09_SAMPLE_THREAD=<guest id>: samples that thread's stack every 2 ms for its whole
    // life, printing each distinct call path (top 8 frames) once.
    void SampleThread(HANDLE thread, uint32_t guestId)
    {
        EnsureSymbols();
        const HANDLE process = GetCurrentProcess();
        std::vector<std::string> seen;
        alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
        for (;;)
        {
            DWORD exitCode = 0;
            if (GetExitCodeThread(thread, &exitCode) && exitCode != STILL_ACTIVE)
                break;
            if (SuspendThread(thread) == DWORD(-1))
                break;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            std::string path;
            if (GetThreadContext(thread, &context))
            {
                STACKFRAME64 frame{};
                frame.AddrPC = { context.Rip, 0, AddrModeFlat };
                frame.AddrFrame = { context.Rbp, 0, AddrModeFlat };
                frame.AddrStack = { context.Rsp, 0, AddrModeFlat };
                for (int depth = 0; depth < 8; depth++)
                {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                            SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                        break;
                    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                    symbol->MaxNameLen = 255;
                    DWORD64 displacement = 0;
                    path += SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) ? symbol->Name : "?";
                    path += " < ";
                }
            }
            ResumeThread(thread);
            if (!path.empty() && std::find(seen.begin(), seen.end(), path) == seen.end())
            {
                seen.push_back(path);
                std::fprintf(stderr, "[sample 0x%X] %s\n", guestId, path.c_str());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::fprintf(stderr, "[sample 0x%X] thread ended; %zu distinct paths\n", guestId, seen.size());
    }

    void RegisterThread(uint32_t guestThreadId)
    {
        HANDLE handle;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
        static const uint32_t sampleId = std::getenv("HC09_SAMPLE_THREAD")
            ? uint32_t(std::strtoul(std::getenv("HC09_SAMPLE_THREAD"), nullptr, 16)) : 0;
        if (sampleId != 0 && guestThreadId == sampleId)
            std::thread(SampleThread, handle, guestThreadId).detach();
        {
            std::lock_guard lock(g_mutex);
            g_threads.push_back({ handle, GetCurrentThreadId(), guestThreadId });
        }
        if (!g_watches.empty())
            std::thread(ArmWatches, handle).join();
    }

    void PrintStackFromContext(const void* context)
    {
        EnsureSymbols();
        PrintStack(GetCurrentProcess(), GetCurrentThread(), *static_cast<const CONTEXT*>(context));
        std::fflush(stderr);
    }

    void DumpAllStacks()
    {
        const HANDLE process = GetCurrentProcess();
        EnsureSymbols();

        std::lock_guard lock(g_mutex);
        for (const auto& entry : g_threads)
        {
            if (entry.hostId == GetCurrentThreadId())
                continue;
            DWORD exitCode = 0;
            if (GetExitCodeThread(entry.handle, &exitCode) && exitCode != STILL_ACTIVE)
                continue;
            if (SuspendThread(entry.handle) == DWORD(-1))
                continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(entry.handle, &context))
            {
                std::fprintf(stderr, "[stacks] guest thread 0x%X (host %lu):\n", entry.guestId, entry.hostId);
                PrintStack(process, entry.handle, context);
            }
            ResumeThread(entry.handle);
        }
        std::fflush(stderr);
    }

    void StartWatchdogFromEnvironment()
    {
        // HC09_WATCH entries: "ADDR" watches one byte, "ADDR:LEN" LEN (1, 2, 4 or 8, aligned)
        // bytes; "*PTR+OFF" watches 8 bytes at (big-endian u32 at PTR) + OFF, resolved 5 seconds
        // after startup. HC09_WATCH_AFTER=<seconds> arms every watch only after that long.
        if (const char* watches = std::getenv("HC09_WATCH"))
        {
            const int armAfter = std::getenv("HC09_WATCH_AFTER") ? std::atoi(std::getenv("HC09_WATCH_AFTER")) : 0;
            struct Deferred
            {
                bool indirect;
                uint32_t value;  // pointer (indirect) or address
                uint32_t offsetOrLength;
            };
            std::vector<Deferred> deferred;
            for (const char* p = watches; *p && g_watches.size() + deferred.size() < 4;)
            {
                char* end;
                if (*p == '*')
                {
                    const uint32_t pointer = uint32_t(std::strtoul(p + 1, &end, 16));
                    const uint32_t offset = *end == '+' ? uint32_t(std::strtoul(end + 1, &end, 16)) : 0;
                    deferred.push_back({ true, pointer, offset });
                }
                else
                {
                    const uint32_t address = uint32_t(std::strtoul(p, &end, 16));
                    const uint32_t length = *end == ':' ? uint32_t(std::strtoul(end + 1, &end, 10)) : 1;
                    if (armAfter > 0)
                        deferred.push_back({ false, address, length });
                    else
                    {
                        g_watches.push_back({ address, length });
                        std::fprintf(stderr, "[watch] 0x%08X (%u bytes)\n", address, length);
                    }
                }
                if (end == p)
                    break;
                p = *end == ',' ? end + 1 : end;
            }
            AddVectoredExceptionHandler(1, OnWatchHit);
            if (!deferred.empty())
            {
                std::thread([deferred, armAfter] {
                    std::this_thread::sleep_for(std::chrono::seconds(std::max(armAfter, 5)));
                    for (const auto& item : deferred)
                    {
                        if (!item.indirect)
                        {
                            g_watches.push_back({ item.value, item.offsetOrLength });
                            std::fprintf(stderr, "[watch] 0x%08X (%u bytes) armed\n", item.value, item.offsetOrLength);
                            continue;
                        }
                        const uint8_t* bytes = GuestToHost(item.value);
                        const uint32_t base = (uint32_t(bytes[0]) << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
                        const uint32_t address = base + item.offsetOrLength;
                        g_watches.push_back({ address, (address & 7) == 0 ? 8u : 1u });
                        std::fprintf(stderr, "[watch] *0x%08X+0x%X = 0x%08X\n", item.value, item.offsetOrLength, address);
                    }
                    std::lock_guard lock(g_mutex);
                    for (const auto& entry : g_threads)
                        ArmWatches(entry.handle);
                }).detach();
            }
        }

        // HC09_THREAD_STATS=1: every 5 s, the CPU share of each registered thread (busiest first).
        if (std::getenv("HC09_THREAD_STATS"))
        {
            std::thread([] {
                std::vector<std::pair<uint32_t, uint64_t>> previous;
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    std::vector<std::pair<uint32_t, uint64_t>> current;
                    {
                        std::lock_guard lock(g_mutex);
                        for (const auto& entry : g_threads)
                        {
                            FILETIME created, exited, kernel, user;
                            if (!GetThreadTimes(entry.handle, &created, &exited, &kernel, &user))
                                continue;
                            const uint64_t total = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                                (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
                            current.push_back({ entry.guestId, total });
                        }
                    }
                    std::vector<std::pair<double, uint32_t>> usage;
                    for (const auto& [id, total] : current)
                    {
                        uint64_t before = 0;
                        for (const auto& [oldId, oldTotal] : previous)
                            if (oldId == id)
                                before = oldTotal;
                        usage.push_back({ (total - before) / 1e7 / 5.0 * 100.0, id });  // 100 ns units
                    }
                    previous = current;
                    std::sort(usage.rbegin(), usage.rend());
                    std::fprintf(stderr, "[threads] CPU %%:");
                    for (size_t i = 0; i < usage.size() && i < 10; i++)
                        if (usage[i].first >= 1.0)
                            std::fprintf(stderr, " 0x%X=%.0f", usage[i].second, usage[i].first);
                    std::fprintf(stderr, "\n");
                }
            }).detach();
        }

        const char* value = std::getenv("HC09_DUMP_STACKS");
        if (value == nullptr)
            return;
        // A comma-separated list of times (seconds after start) dumps once at each.
        std::vector<int> times;
        for (const char* p = value; *p;)
        {
            char* end = nullptr;
            times.push_back(int(std::strtol(p, &end, 10)));
            if (end == p)
                break;
            p = *end == ',' ? end + 1 : end;
        }
        std::thread([times] {
            const auto start = std::chrono::steady_clock::now();
            for (int seconds : times)
            {
                std::this_thread::sleep_until(start + std::chrono::seconds(seconds));
                DumpAllStacks();
            }
        }).detach();
    }
}
