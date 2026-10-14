// Video driver (Vd*) exports: ring buffer setup, interrupts, display mode, swaps.

#include <bit>
#include <cstdio>

#include "gpu/command_processor.h"
#include "kernel/guest.h"
#include "kernel/heap.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kDisplayWidth = 1280;
        constexpr uint32_t kDisplayHeight = 720;
        constexpr uint32_t kSwapReservedDwords = 64;  // the caller reserves this much ring space

        uint32_t StackArg32(const PPCContext& ctx, int index)
        {
            return Load<uint32_t>(ctx.r1.u32 + 0x54 + (index - 8) * 8);
        }

        uint32_t Type3Header(uint32_t opcode, uint32_t payloadDwords)
        {
            return (3u << 30) | ((payloadDwords - 1) << 16) | (opcode << 8);
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__VdInitializeEngines)
{
    Return(ctx, 1);
}

PPC_FUNC(__imp__VdShutdownEngines)
{
}

PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    // (ring_buffer_physical_address, size_log2)
    gpu::InitializeRingBuffer(Arg32(ctx, 0), Arg32(ctx, 1));
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    // (write_back_physical_address, block_size_log2)
    gpu::EnableReadPointerWriteBack(Arg32(ctx, 0), Arg32(ctx, 1));
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    gpu::SetInterruptCallback(Arg32(ctx, 0), Arg32(ctx, 1));
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
}

PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    // (out_ptr0, out_ptr1): opaque values, as on retail kernels
    Store<uint32_t>(Arg32(ctx, 0), 0xBEEF0000);
    Store<uint32_t>(Arg32(ctx, 1), 0xBEEF0001);
}

PPC_FUNC(__imp__VdQueryVideoMode)
{
    // X_VIDEO_MODE: display_width, display_height, is_interlaced, is_widescreen, is_hi_def,
    // refresh_rate (float), video_standard, unknown_0x8a, unknown_0x01, reserved[3]
    const uint32_t mode = Arg32(ctx, 0);
    std::memset(GuestToHost(mode), 0, 0x30);
    Store<uint32_t>(mode + 0x00, kDisplayWidth);
    Store<uint32_t>(mode + 0x04, kDisplayHeight);
    Store<uint32_t>(mode + 0x08, 0);
    Store<uint32_t>(mode + 0x0C, 1);
    Store<uint32_t>(mode + 0x10, 1);
    Store<uint32_t>(mode + 0x14, std::bit_cast<uint32_t>(60.0f));
    Store<uint32_t>(mode + 0x18, 1);  // NTSC-M
    Store<uint32_t>(mode + 0x1C, 0x4A);
    Store<uint32_t>(mode + 0x20, 0x01);
}

PPC_FUNC(__imp__VdQueryVideoFlags)
{
    Return(ctx, 0x7);  // widescreen | width >= 1024 | height >= 720
}

PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    // X_DISPLAY_INFO (0x58 bytes): front buffer size, formats, window and refresh data
    const uint32_t info = Arg32(ctx, 0);
    std::memset(GuestToHost(info), 0, 0x58);
    Store<uint16_t>(info + 0x00, kDisplayWidth);
    Store<uint16_t>(info + 0x02, kDisplayHeight);
    Store<uint16_t>(info + 0x10, kDisplayWidth);   // display window width
    Store<uint16_t>(info + 0x12, kDisplayHeight);  // display window height
    Store<uint32_t>(info + 0x40, 60);              // refresh rate
    Store<uint16_t>(info + 0x48, kDisplayWidth);   // actual display width
    Store<uint16_t>(info + 0x4A, kDisplayHeight);  // actual display height
}

PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    // (out_type, out_power)
    Store<uint32_t>(Arg32(ctx, 0), 2);
    Store<uint32_t>(Arg32(ctx, 1), std::bit_cast<uint32_t>(2.22222233f));
}

PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    // 12 arguments in this XDK: (source_xy, source_wh, scale_factors, filter, ..., dest,
    // dest_capacity_dwords) with dest and capacity in stack slots 10 and 11. The caller
    // copies `return value` dwords from dest into its command buffer. The display scaler
    // is not emulated, so the commands are PM4 type-2 no-ops.
    const uint32_t dest = StackArg32(ctx, 10), capacity = StackArg32(ctx, 11);
    for (uint32_t i = 0; i < capacity; i++)
        Store<uint32_t>(dest + i * 4, 0x80000000);
    Return(ctx, capacity);
}

PPC_FUNC(__imp__VdSetDisplayMode)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__VdPersistDisplay)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__VdEnableDisableClockGating)
{
}

PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)
{
    Return(ctx, 1);
}

PPC_FUNC(__imp__VdRetrainEDRAM)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__VdRetrainEDRAMWorker)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__VdSwap)
{
    // (buffer_ptr, fetch_ptr, unk, unk, unk, frontbuffer_ptr, texture_format_ptr,
    //  color_space_ptr, width_ptr, height_ptr). The game reserves 64 dwords at buffer_ptr;
    // fill them with one XE_SWAP packet so the CP stays in sync.
    const uint32_t buffer = Arg32(ctx, 0);
    const uint32_t frontBufferPtr = Arg32(ctx, 5);
    const uint32_t widthPtr = StackArg32(ctx, 8), heightPtr = StackArg32(ctx, 9);
    std::memset(GuestToHost(buffer), 0, kSwapReservedDwords * 4);
    Store<uint32_t>(buffer + 0x00, Type3Header(0x64, kSwapReservedDwords - 1));
    Store<uint32_t>(buffer + 0x04, 0x53574150);  // 'SWAP'
    Store<uint32_t>(buffer + 0x08, frontBufferPtr ? Load<uint32_t>(frontBufferPtr) : 0);
    Store<uint32_t>(buffer + 0x0C, widthPtr ? Load<uint32_t>(widthPtr) : kDisplayWidth);
    Store<uint32_t>(buffer + 0x10, heightPtr ? Load<uint32_t>(heightPtr) : kDisplayHeight);
    Return(ctx, 0);
}
