#pragma once
// hc09-recomp runtime: shared declarations for the host-side skeleton that
// backs the recompiled NFL Head Coach 09 code.
// Adapted for this project: the guest address space, the recompiler's context and the functions below come
// from this project's runtime (src/memory.h, src/vk/vk_adapter.cpp).
#include <cstdint>
#include <cstddef>
#include <atomic>

#include <ppc_recomp_shared.h>

// The single 4 GiB guest address space. Guest address A lives at g_base + A.
namespace hc09 { extern uint8_t* g_base; }
using hc09::g_base;

// Convert between guest pointers and host pointers.
inline void* GuestToHost(uint32_t ea) { return ea ? g_base + ea : nullptr; }
inline uint32_t HostToGuest(const void* p)
{
    return p ? uint32_t(reinterpret_cast<const uint8_t*>(p) - g_base) : 0u;
}

// Big-endian guest memory accessors usable anywhere (not just inside PPC_FUNC,
// where the `base` parameter makes the PPC_LOAD_*/PPC_STORE_* macros work).
inline uint8_t  GLoad8 (uint32_t ea) { return *(volatile uint8_t*)(g_base + ea); }
inline uint16_t GLoad16(uint32_t ea) { return __builtin_bswap16(*(volatile uint16_t*)(g_base + ea)); }
inline uint32_t GLoad32(uint32_t ea) { return __builtin_bswap32(*(volatile uint32_t*)(g_base + ea)); }
inline uint64_t GLoad64(uint32_t ea) { return __builtin_bswap64(*(volatile uint64_t*)(g_base + ea)); }
inline void GStore8 (uint32_t ea, uint8_t  v) { *(volatile uint8_t*)(g_base + ea) = v; }
inline void GStore16(uint32_t ea, uint16_t v) { *(volatile uint16_t*)(g_base + ea) = __builtin_bswap16(v); }
inline void GStore32(uint32_t ea, uint32_t v) { *(volatile uint32_t*)(g_base + ea) = __builtin_bswap32(v); }
inline void GStore64(uint32_t ea, uint64_t v) { *(volatile uint64_t*)(g_base + ea) = __builtin_bswap64(v); }

// Name a host thread for Windows tools (nullptr = the calling thread; runtime.cpp).
void HostSetThreadName(void* thread, const char* name);
// A host thread's priority on Windows' scale (-2..2, 15, -15; nullptr = the calling thread; runtime.cpp).
void HostSetThreadPriority(void* thread, int priority);

// Simple bump allocator over a reserved slice of guest memory. Thread-safe.
uint32_t GuestAlloc(uint32_t size, uint32_t align = 16);
// Physical (GPU-visible) memory; returns a 0xA0000000-based virtual address,
// physical address = result & 0x1FFFFFFF.
uint32_t PhysicalAlloc(uint32_t size, uint32_t align = 0x1000);
void     PhysicalFree(uint32_t va);                 // stack-like: frees only the most recent block
// Host code about to write guest memory with a system call (file reads): pages
// write-tracked for the live renderer's caches (xenos::HostTrackWrites) are
// unprotected and marked written first, so the kernel does not fail the write.
void GuestHostWrite(uint32_t va, uint32_t bytes);
// Diagnostics: print a guest backtrace for a context (lr, back-chain).
void DumpGuestBacktrace(const PPCContext& ctx, const char* indent);

// Call a guest function by address from host code (looks up the recompiled
// host implementation through the indirect-call table).
void CallGuest(uint32_t guestAddr, PPCContext& ctx);

// Prepare a fresh PPCContext for a new guest thread: FPSCR seeded, stack set.
void InitThreadContext(PPCContext& ctx, uint32_t stackTop, uint32_t pcr);

// Kernel-object subsystem (threads, sync, handles) — see imports_threads.cpp.
void KernelInitMainThread(PPCContext* mainCtx);   // registers the booting host thread as a guest thread
void KernelWaitAllThreads();   // blocks until every guest thread has exited

// Install a host function pointer into the guest indirect-call lookup table.
void RegisterGuestFunction(uint32_t guest, PPCFunc* host);

// Populate the lookup table from the generated PPCFuncMappings array.
void InstallFunctionTable();

// Logging for import stubs. kind 0 = call trace (throttled), 1 = always.
void ImportLog(const char* name);
// Generated stubs (imports_gen.cpp): ImportLog plus, with HC09_TRACE_STUBS=1, the first
// calls of each stub with r3-r8 and the guest caller (lr) — which unimplemented imports
// the game really uses, and what it passes them.
void StubLog(const char* name, const PPCContext& ctx);

// Boot the recompiled image: set up a thread context + stack and jump to entry.
int BootGame();
