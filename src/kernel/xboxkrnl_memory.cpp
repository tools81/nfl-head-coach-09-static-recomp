// Memory management: NtAllocateVirtualMemory and friends, physical memory, pools.

#include <cstdio>

#include "kernel/guest.h"
#include "kernel/heap.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kMemCommit = 0x1000;
        constexpr uint32_t kMemReserve = 0x2000;
        constexpr uint32_t kMemDecommit = 0x4000;
        constexpr uint32_t kMemRelease = 0x8000;
        constexpr uint32_t kMemFree = 0x10000;
        constexpr uint32_t kMemPrivate = 0x20000;
        constexpr uint32_t kMemTopDown = 0x100000;
        constexpr uint32_t kMemLargePages = 0x20000000;
        constexpr uint32_t kPageReadWrite = 0x04;

        // Physical addresses are mirrored at 0xA0000000 (64 KiB pages), 0xC0000000 (16 MiB)
        // and 0xE0000000 (4 KiB). Allocations are handed out from the 0xA0000000 view.
        constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;
    }
}

using namespace hc09;

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    const uint32_t basePtr = Arg32(ctx, 0), sizePtr = Arg32(ctx, 1), type = Arg32(ctx, 2);
    uint32_t address = Load<uint32_t>(basePtr);
    const uint32_t size = Load<uint32_t>(sizePtr);
    if (size == 0 || !(type & (kMemCommit | kMemReserve)))
        return Return(ctx, status::kInvalidParameter);

    auto& heap = VirtualHeap();
    const uint32_t pageSize = (type & kMemLargePages) ? 0x10000 : 0x1000;
    const uint32_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);

    if (address != 0)
    {
        // Committing inside an existing reservation needs no work: memory is always backed.
        uint32_t allocBase, allocSize;
        if (heap.Query(address, allocBase, allocSize))
        {
            if (address + alignedSize > allocBase + allocSize)
                return Return(ctx, status::kInvalidParameter);
        }
        else if (!heap.AllocateAt(address, alignedSize))
        {
            return Return(ctx, status::kNoMemory);
        }
        address &= ~(pageSize - 1);
    }
    else
    {
        address = heap.Allocate(alignedSize, pageSize, (type & kMemTopDown) != 0);
        if (address == 0)
            return Return(ctx, status::kNoMemory);
    }
    Store<uint32_t>(basePtr, address);
    Store<uint32_t>(sizePtr, alignedSize);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    const uint32_t basePtr = Arg32(ctx, 0), sizePtr = Arg32(ctx, 1), type = Arg32(ctx, 2);
    const uint32_t address = Load<uint32_t>(basePtr);
    if (type & kMemDecommit)
        return Return(ctx, status::kSuccess);  // nothing to decommit; memory stays backed

    uint32_t allocBase, allocSize;
    if (!(type & kMemRelease) || !VirtualHeap().Query(address, allocBase, allocSize) || allocBase != address)
        return Return(ctx, status::kInvalidParameter);
    VirtualHeap().Free(address);
    if (sizePtr)
        Store<uint32_t>(sizePtr, allocSize);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    const uint32_t address = Arg32(ctx, 0), info = Arg32(ctx, 1);
    // X_MEMORY_BASIC_INFORMATION: base, allocation_base, allocation_protect, region_size,
    // state, protect, type
    uint32_t allocBase = 0, allocSize = 0;
    GuestHeap* heap = HeapFor(address);
    const bool used = heap && heap->Query(address, allocBase, allocSize);
    const bool inImage = address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    if (inImage)
    {
        allocBase = uint32_t(PPC_IMAGE_BASE);
        allocSize = uint32_t(PPC_IMAGE_SIZE);
    }
    const bool mapped = used || inImage;
    Store<uint32_t>(info + 0x00, address & ~0xFFFu);
    Store<uint32_t>(info + 0x04, mapped ? allocBase : 0);
    Store<uint32_t>(info + 0x08, mapped ? kPageReadWrite : 0);
    Store<uint32_t>(info + 0x0C, mapped ? allocBase + allocSize - (address & ~0xFFFu) : 0x1000);
    Store<uint32_t>(info + 0x10, mapped ? kMemCommit : kMemFree);
    Store<uint32_t>(info + 0x14, mapped ? kPageReadWrite : 0);
    Store<uint32_t>(info + 0x18, mapped ? kMemPrivate : 0);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    // (flags, size, protect, min_address, max_address, alignment)
    const uint32_t size = Arg32(ctx, 1), alignment = Arg32(ctx, 5);
    Return(ctx, PhysicalHeap().Allocate(size, alignment < 0x1000 ? 0x1000 : alignment));
}

PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    PhysicalHeap().Free(Arg32(ctx, 1));
}

PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    Return(ctx, Arg32(ctx, 0) & kPhysicalMask);
}

PPC_FUNC(__imp__MmQueryAddressProtect)
{
    const uint32_t address = Arg32(ctx, 0);
    uint32_t allocBase, allocSize;
    GuestHeap* heap = HeapFor(address);
    const bool inImage = address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    Return(ctx, inImage || (heap && heap->Query(address, allocBase, allocSize)) ? kPageReadWrite : 0);
}

PPC_FUNC(__imp__ExAllocatePoolTypeWithTag)
{
    Return(ctx, PoolHeap().Allocate(Arg32(ctx, 0)));
}

PPC_FUNC(__imp__ExAllocatePoolWithTag)
{
    Return(ctx, PoolHeap().Allocate(Arg32(ctx, 0)));
}

PPC_FUNC(__imp__ExFreePool)
{
    PoolHeap().Free(Arg32(ctx, 0));
}

PPC_FUNC(__imp__XamAlloc)
{
    // (flags, size, out_ptr) -> HRESULT-style status
    const uint32_t address = PoolHeap().Allocate(Arg32(ctx, 1));
    Store<uint32_t>(Arg32(ctx, 2), address);
    Return(ctx, address ? 0 : 0x8007000E);  // E_OUTOFMEMORY
}

PPC_FUNC(__imp__XamFree)
{
    PoolHeap().Free(Arg32(ctx, 0));
    Return(ctx, 0);
}
