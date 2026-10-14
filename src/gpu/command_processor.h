#pragma once

// Xenos command processor (CP): consumes PM4 packets the game's Direct3D writes into the
// ring buffer, handles synchronization (fences, waits, interrupts, swaps), and drives the
// vblank interrupt.
//
// The GPU register file is the guest's MMIO window at 0x7FC80000 (register index * 4),
// stored big-endian in guest memory: the game writes it with ordinary stores, and the
// CP polls CP_RB_WPTR there to find new work.
//
// Rendering packets (draws, constants) are parsed and skipped for now.

#include <cstdint>

namespace hc09::gpu
{
    constexpr uint32_t kRegisterBase = 0x7FC80000;
    constexpr uint32_t kRegisterCount = 0x10000;

    namespace reg
    {
        constexpr uint32_t kCpRbWptr = 0x01C5;
        constexpr uint32_t kCpRbRptr = 0x01C4;
        constexpr uint32_t kD1ModeVblankStatus = 0x1951;
    }

    uint32_t ReadRegister(uint32_t index);
    void WriteRegister(uint32_t index, uint32_t value);

    // Physical GPU address -> guest virtual address (physical memory is mapped at 0xA0000000).
    uint32_t PhysicalToGuest(uint32_t address);
    // True once HC09_TRACE_FENCES has triggered (debug tracing window).
    bool FenceTraceActive();
    // True during the first pass of predicated tiling (screen-sized targets render it whole).
    bool InFirstTilePass();
    // True while predicated tiling is selecting individual tiles.
    bool TilingActive();

    // VdInitializeRingBuffer(ptr, size_log2): starts the CP thread.
    void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2);
    // VdEnableRingBufferRPtrWriteBack(ptr, block_size_log2)
    void EnableReadPointerWriteBack(uint32_t address, uint32_t blockSizeLog2);
    // VdSetGraphicsInterruptCallback(callback, user_data): starts the vblank timer.
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // Number of swaps (presents) processed so far.
    uint32_t SwapCount();
}
