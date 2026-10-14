#pragma once

#include <ppc_recomp_shared.h>

// Guest memory map (32-bit Xbox 360 addresses inside the 4 GiB host reservation).
//
//   0x40000000 - 0x4FFFFFFF  pool allocations (ExAllocatePool*, XamAlloc, host-owned blocks)
//   0x50000000 - 0x6FFFFFFF  virtual allocations (NtAllocateVirtualMemory), thread stacks
//   0x82000000 - 0x8323FFFF  game image (PPC_IMAGE_BASE, PPC_IMAGE_SIZE)
//   0x83240000 - 0x84BE7E5F  host function lookup table (PPC_LOOKUP_FUNC)
//   0xA0000000 - 0xBFFFFFFF  physical allocations (MmAllocatePhysicalMemoryEx)
namespace hc09
{
    extern uint8_t* g_base;

    constexpr uint32_t kPoolHeapBase = 0x40000000;
    constexpr uint32_t kPoolHeapEnd = 0x50000000;
    constexpr uint32_t kVirtualHeapBase = 0x50000000;
    constexpr uint32_t kVirtualHeapEnd = 0x70000000;
    constexpr uint32_t kPhysicalHeapBase = 0xA0000000;
    constexpr uint32_t kPhysicalHeapEnd = 0xC0000000;

    static_assert(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + PPC_CODE_SIZE * 2 <= kPhysicalHeapBase,
        "function lookup table overlaps the physical heap");

    // Reserves the 4 GiB guest space with aliased physical-memory windows (see memory.cpp).
    bool AllocateGuestMemory();

    inline uint8_t* GuestToHost(uint32_t address) { return g_base + address; }
}
