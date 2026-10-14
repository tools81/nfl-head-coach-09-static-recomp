#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace hc09
{
    // First-fit allocator over a fixed guest address range. Memory is always committed
    // (the whole 4 GiB guest space is committed up front), so reserve/commit are the same
    // operation; allocations are zeroed.
    class GuestHeap
    {
    public:
        GuestHeap(const char* name, uint32_t begin, uint32_t end, uint32_t granularity);

        // Returns 0 on failure. `alignment` is rounded up to the heap granularity.
        uint32_t Allocate(uint32_t size, uint32_t alignment = 0, bool topDown = false);
        // Allocates at a fixed address; fails if any part is in use.
        bool AllocateAt(uint32_t address, uint32_t size);
        bool Free(uint32_t address);

        bool Contains(uint32_t address) const { return address >= begin_ && address < end_; }
        // Start and size of the allocation containing `address`, if any.
        bool Query(uint32_t address, uint32_t& allocationBase, uint32_t& allocationSize) const;

        uint32_t granularity() const { return granularity_; }

    private:
        const char* name_;
        uint32_t begin_;
        uint32_t end_;
        uint32_t granularity_;
        mutable std::mutex mutex_;
        std::map<uint32_t, uint32_t> used_;  // address -> size
        std::map<uint32_t, uint32_t> free_;  // address -> size, coalesced
    };

    GuestHeap& PoolHeap();      // small-granularity pool (ExAllocatePool, XamAlloc)
    GuestHeap& VirtualHeap();   // NtAllocateVirtualMemory
    GuestHeap& PhysicalHeap();  // MmAllocatePhysicalMemoryEx
    GuestHeap* HeapFor(uint32_t address);
}
