#include "kernel/heap.h"

#include <cstdio>
#include <cstring>

#include "memory.h"

namespace hc09
{
    namespace
    {
        uint32_t AlignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }
    }

    GuestHeap::GuestHeap(const char* name, uint32_t begin, uint32_t end, uint32_t granularity)
        : name_(name), begin_(begin), end_(end), granularity_(granularity)
    {
        free_.emplace(begin, end - begin);
    }

    uint32_t GuestHeap::Allocate(uint32_t size, uint32_t alignment, bool topDown)
    {
        if (size == 0)
            size = 1;
        size = AlignUp(size, granularity_);
        alignment = alignment < granularity_ ? granularity_ : AlignUp(alignment, granularity_);

        std::lock_guard lock(mutex_);
        auto take = [&](std::map<uint32_t, uint32_t>::iterator it, uint32_t address) {
            const uint32_t blockStart = it->first, blockEnd = it->first + it->second;
            free_.erase(it);
            if (address > blockStart)
                free_.emplace(blockStart, address - blockStart);
            if (address + size < blockEnd)
                free_.emplace(address + size, blockEnd - (address + size));
            used_.emplace(address, size);
            std::memset(GuestToHost(address), 0, size);
            return address;
        };

        if (topDown)
        {
            for (auto it = free_.rbegin(); it != free_.rend(); ++it)
            {
                const uint64_t blockEnd = uint64_t(it->first) + it->second;
                if (blockEnd < size)
                    continue;
                const uint32_t address = uint32_t(blockEnd - size) & ~(alignment - 1);
                if (address >= it->first)
                    return take(std::next(it).base(), address);
            }
        }
        else
        {
            for (auto it = free_.begin(); it != free_.end(); ++it)
            {
                const uint32_t address = AlignUp(it->first, alignment);
                if (address >= it->first && uint64_t(address) + size <= uint64_t(it->first) + it->second)
                    return take(it, address);
            }
        }
        std::fprintf(stderr, "[heap] %s: out of memory allocating 0x%X\n", name_, size);
        return 0;
    }

    bool GuestHeap::AllocateAt(uint32_t address, uint32_t size)
    {
        size = AlignUp(size, granularity_);
        address &= ~(granularity_ - 1);
        std::lock_guard lock(mutex_);
        auto it = free_.upper_bound(address);
        if (it == free_.begin())
            return false;
        --it;
        const uint32_t blockStart = it->first, blockEnd = it->first + it->second;
        if (address + uint64_t(size) > blockEnd)
            return false;
        free_.erase(it);
        if (address > blockStart)
            free_.emplace(blockStart, address - blockStart);
        if (address + size < blockEnd)
            free_.emplace(address + size, blockEnd - (address + size));
        used_.emplace(address, size);
        std::memset(GuestToHost(address), 0, size);
        return true;
    }

    bool GuestHeap::Free(uint32_t address)
    {
        std::lock_guard lock(mutex_);
        auto it = used_.find(address);
        if (it == used_.end())
            return false;
        uint32_t start = it->first, size = it->second;
        used_.erase(it);

        auto next = free_.lower_bound(start);
        if (next != free_.end() && next->first == start + size)
        {
            size += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start)
            {
                start = prev->first;
                size += prev->second;
                free_.erase(prev);
            }
        }
        free_.emplace(start, size);
        return true;
    }

    bool GuestHeap::Query(uint32_t address, uint32_t& allocationBase, uint32_t& allocationSize) const
    {
        std::lock_guard lock(mutex_);
        auto it = used_.upper_bound(address);
        if (it == used_.begin())
            return false;
        --it;
        if (address >= it->first + it->second)
            return false;
        allocationBase = it->first;
        allocationSize = it->second;
        return true;
    }

    GuestHeap& PoolHeap()
    {
        static GuestHeap heap("pool", kPoolHeapBase, kPoolHeapEnd, 0x10);
        return heap;
    }

    GuestHeap& VirtualHeap()
    {
        static GuestHeap heap("virtual", kVirtualHeapBase, kVirtualHeapEnd, 0x1000);
        return heap;
    }

    GuestHeap& PhysicalHeap()
    {
        static GuestHeap heap("physical", kPhysicalHeapBase, kPhysicalHeapEnd, 0x1000);
        return heap;
    }

    GuestHeap* HeapFor(uint32_t address)
    {
        for (GuestHeap* heap : { &PoolHeap(), &VirtualHeap(), &PhysicalHeap() })
            if (heap->Contains(address))
                return heap;
        return nullptr;
    }
}
