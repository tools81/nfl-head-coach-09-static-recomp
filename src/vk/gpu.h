#pragma once
// GPU front end: the Xenos command processor as seen by the CPU.
//
// No drawing happens yet. What this does is everything the game can observe
// from the CPU side: consume the primary ring buffer (and the indirect
// buffers it points at), execute the PM4 packets that write memory or wait
// on it (fences, counters, waits), count swaps, write the read pointer back,
// and raise the graphics interrupt (vblank at 60 Hz, PM4_INTERRUPT on
// request). That is what the renderer's frame pacing, fences and present
// logic block on.
#include <cstdint>

// Guest-visible register file (MMIO). Register index * 4 + kGpuRegBase.
constexpr uint32_t kGpuRegBase  = 0x7FC80000;
constexpr uint32_t kGpuRegCount = 0x5003;

constexpr uint32_t REG_CP_RB_BASE      = 0x01C0;
constexpr uint32_t REG_CP_RB_CNTL      = 0x01C1;
constexpr uint32_t REG_CP_RB_RPTR_ADDR = 0x01C3;
constexpr uint32_t REG_CP_RB_RPTR      = 0x01C4;
constexpr uint32_t REG_CP_RB_WPTR      = 0x01C5;
constexpr uint32_t REG_SCRATCH_UMSK    = 0x01DC;   // bit n: mirror SCRATCH_REGn to memory
constexpr uint32_t REG_SCRATCH_ADDR    = 0x01DD;   // physical address of the mirror (reg*4 added)
constexpr uint32_t REG_SCRATCH_REG0    = 0x0578;   // ..0x057F
constexpr uint32_t REG_RBBM_STATUS     = 0x05D0;
constexpr uint32_t REG_COHER_SIZE_HOST   = 0x0A2F;
constexpr uint32_t REG_COHER_BASE_HOST   = 0x0A30;
constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A31;   // bit 31: coherency action complete
constexpr uint32_t REG_VGT_EVENT_INITIATOR = 0x21F9;
// Display controller / RB registers the driver reads back. Hardware-provided
// values the interrupt handler and D3D init expect to see.
constexpr uint32_t REG_RB_EDRAM_TIMING      = 0x0F00;   // 0x08100748
constexpr uint32_t REG_RB_BC_CONTROL        = 0x0F01;   // 0x0000200E
constexpr uint32_t REG_D1MODE_V_COUNTER     = 0x194C;   // current scanline
constexpr uint32_t REG_D1MODE_INT_STATUS    = 0x1951;   // bit 0: vblank
constexpr uint32_t REG_D1MODE_VIEWPORT_SIZE = 0x1961;   // width<<16 | height

void     GpuInitializeRingBuffer(uint32_t ptr, uint32_t sizeLog2);
void     GpuEnableReadPointerWriteBack(uint32_t ptr, uint32_t blockSizeLog2);
void     GpuSetInterruptCallback(uint32_t callback, uint32_t userData);
// Writes the PM4_XE_SWAP packet VdSwap hands the game (64 dwords at `buf`).
// `ordinal` (the VdSwap call number) goes into an unused payload dword so the
// command processor can tell a fresh packet from one replayed out of stale memory.
void     GpuWriteSwapPacket(uint32_t buf, uint32_t frontBuffer, uint32_t texFormat,
                            uint32_t colorSpace, uint32_t width, uint32_t height, uint32_t ordinal);
uint64_t GpuSwapCount();
// Seconds the command-processor thread has spent idle: ring empty (nothing
// queued by the game) and in WAIT_REG_MEM that had to wait for the CPU.
void     GpuCpIdle(double* ringEmptySeconds, double* memWaitSeconds);
// Diagnostics: report GPU writes that lower the dword at physical address `pa`.
void     GpuWatchPhysical(uint32_t pa);
// VdSetSystemCommandBufferGpuIdentifierAddress: where the kernel's own swap
// packets report progress. Recorded; not used until the kernel-side command
// buffer is modelled.
void     GpuSetSystemIdentifierAddress(uint32_t identifierAddr);
void     GpuStart();     // worker + vblank threads; idempotent
// Native fences (gpu_live.cpp): the CP's EVENT_WRITE_SHD to the dword at physical `pa`
// (D3D's completed-fence word) calls `sink` with the value instead of writing it;
// GpuStoreFenceWord writes such a value (big-endian, as the CP would).
void     GpuRedirectFenceWord(uint32_t pa, void (*sink)(uint32_t value));
void     GpuStoreFenceWord(uint32_t pa, uint32_t value);
