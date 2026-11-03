#pragma once
// Xenos (Xbox 360 GPU) draw-side model: register decoding, shaders, frame
// capture and (later) texture decoding, shader interpretation and software
// rasterization to PNG.
//
// Portable C++17 only — no OS headers. The host runtime provides register and
// physical-memory access through the two hooks below, so this module can move
// into a native Windows build unchanged.
//
// Register indices, bitfield layouts and formats follow the public Xenos
// documentation as captured in Xenia (BSD-3-Clause, (c) Ben Vanik and
// contributors), used here as a hardware reference.
#include <cstdint>
#include <string>

namespace xenos {

// Portable byte swap (no compiler builtins, so MSVC builds this module too).
inline uint32_t Bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

// ---- host hooks (implemented by the runtime, or by tools/xenos_replay) ------
uint32_t HostReg(uint32_t index);                       // register file value
// The whole register file (kRegisterCount words) for bulk reads, or nullptr;
// `bigEndian`: the words are stored byte-swapped (the runtime keeps them in guest memory).
const uint32_t* HostRegFile(bool* bigEndian);
const uint8_t* HostPhys(uint32_t physicalAddress);      // GPU physical memory
uint64_t HostSwapCount();                               // frames presented so far
// Diagnostics: report CPU writes to this physical range (HC09_WATCH_VFETCH).
// A host without write tracking may ignore it.
void HostWatchWrites(uint32_t physicalAddress, uint32_t bytes);
// Write tracking for caches of data decoded from guest memory (the live
// renderer's textures). HostTrackWrites makes CPU writes to the range's pages
// observable from now on and returns the write epoch to compare against;
// HostWrittenSince tells whether any of the pages was written after `epoch`;
// when none was, it may advance `epoch` (so the next check is O(1) until the
// next tracked write anywhere).
// A host without tracking (offline replays: memory never changes) returns 0 / false.
uint64_t HostTrackWrites(uint32_t physicalAddress, uint32_t bytes);
bool HostWrittenSince(uint32_t physicalAddress, uint32_t bytes, uint64_t& epoch);

constexpr uint32_t kRegisterCount = 0x5003;

// Physical memory as seen by this module: HostPhys, plus page recording for
// frame replays while a capture is active. Module code uses this, never
// HostPhys directly. Accesses must not cross a 4 KiB page.
const uint8_t* Phys(uint32_t physicalAddress);
// Register file as seen by the renderer: HostReg, or a snapshot while
// re-rendering buffered draws. Renderer code uses this, never HostReg directly.
uint32_t Reg(uint32_t index);
// Reg(first) .. Reg(first + count - 1) into `out`, in one pass (snapshots of constants).
void RegRange(uint32_t first, uint32_t count, uint32_t* out);
// The calling thread reads registers from `regs` (kRegisterCount host-endian
// values) instead of the live register file; nullptr restores it. The live
// renderer's thread renders from a copy kept in step with the CP's writes.
void SetThreadRegisterFile(const uint32_t* regs);

// Capture targets set by a tool instead of HC09_CAPTURE_FRAME/HC09_CAPTURE_DIR.
// `recordReplay`: also write replay.bin (off when replaying one).
void CaptureConfigure(const long* frames, size_t count, const std::string& dir, bool recordReplay);

// ---- registers --------------------------------------------------------------
namespace reg {
constexpr uint32_t RB_SURFACE_INFO        = 0x2000;
constexpr uint32_t RB_COLOR_INFO          = 0x2001;
constexpr uint32_t RB_DEPTH_INFO          = 0x2002;
constexpr uint32_t RB_COLOR1_INFO         = 0x2003;
constexpr uint32_t RB_COLOR2_INFO         = 0x2004;
constexpr uint32_t RB_COLOR3_INFO         = 0x2005;
constexpr uint32_t PA_SC_WINDOW_OFFSET    = 0x2080;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_TL= 0x2081;
constexpr uint32_t PA_SC_WINDOW_SCISSOR_BR= 0x2082;
constexpr uint32_t VGT_MAX_VTX_INDX       = 0x2100;
constexpr uint32_t VGT_MIN_VTX_INDX       = 0x2101;
constexpr uint32_t VGT_INDX_OFFSET        = 0x2102;
constexpr uint32_t RB_COLOR_MASK          = 0x2104;
constexpr uint32_t RB_BLEND_RED           = 0x2105;   // ..0x2108 RB_BLEND_ALPHA (constant blend colour)
constexpr uint32_t RB_STENCILREFMASK_BF   = 0x210C;
constexpr uint32_t RB_STENCILREFMASK      = 0x210D;
constexpr uint32_t RB_ALPHA_REF           = 0x210E;
constexpr uint32_t PA_CL_VPORT_XSCALE     = 0x210F;
constexpr uint32_t PA_CL_VPORT_XOFFSET    = 0x2110;
constexpr uint32_t PA_CL_VPORT_YSCALE     = 0x2111;
constexpr uint32_t PA_CL_VPORT_YOFFSET    = 0x2112;
constexpr uint32_t PA_CL_VPORT_ZSCALE     = 0x2113;
constexpr uint32_t PA_CL_VPORT_ZOFFSET    = 0x2114;
constexpr uint32_t SQ_PROGRAM_CNTL        = 0x2180;
constexpr uint32_t SQ_CONTEXT_MISC        = 0x2181;
constexpr uint32_t SQ_INTERPOLATOR_CNTL   = 0x2182;
constexpr uint32_t VGT_DMA_BASE           = 0x21FA;
constexpr uint32_t VGT_DMA_SIZE           = 0x21FB;
constexpr uint32_t VGT_DRAW_INITIATOR     = 0x21FC;
constexpr uint32_t RB_DEPTHCONTROL        = 0x2200;
constexpr uint32_t RB_BLENDCONTROL0       = 0x2201;
constexpr uint32_t RB_COLORCONTROL        = 0x2202;
constexpr uint32_t PA_CL_CLIP_CNTL        = 0x2204;
constexpr uint32_t PA_SU_SC_MODE_CNTL     = 0x2205;
constexpr uint32_t PA_CL_VTE_CNTL         = 0x2206;
constexpr uint32_t RB_MODECONTROL         = 0x2208;
constexpr uint32_t RB_BLENDCONTROL1       = 0x2209;
constexpr uint32_t RB_BLENDCONTROL2       = 0x220A;
constexpr uint32_t RB_BLENDCONTROL3       = 0x220B;
constexpr uint32_t PA_SU_POINT_SIZE       = 0x2280;
constexpr uint32_t PA_SC_AA_CONFIG        = 0x2301;
constexpr uint32_t PA_SU_VTX_CNTL         = 0x2302;
constexpr uint32_t RB_COPY_CONTROL        = 0x2318;
constexpr uint32_t RB_COPY_DEST_BASE      = 0x2319;
constexpr uint32_t RB_COPY_DEST_PITCH     = 0x231A;
constexpr uint32_t RB_COPY_DEST_INFO      = 0x231B;
constexpr uint32_t RB_DEPTH_CLEAR         = 0x231D;
constexpr uint32_t RB_COLOR_CLEAR         = 0x231E;
constexpr uint32_t RB_COLOR_CLEAR_LO      = 0x231F;
constexpr uint32_t RB_COPY_SURFACE_SLICE  = 0x2323;
constexpr uint32_t ALU_CONSTANTS          = 0x4000;   // 512 float4 (VS 0-255, PS 256-511)
constexpr uint32_t FETCH_CONSTANTS        = 0x4800;   // 32 texture fetch x 6 dwords (= 96 vertex fetch x 2)
constexpr uint32_t BOOL_CONSTANTS         = 0x4900;
constexpr uint32_t LOOP_CONSTANTS         = 0x4908;
}  // namespace reg

enum class ShaderType : uint32_t { kVertex = 0, kPixel = 1 };

// A draw as the command processor decoded it from DRAW_INDX / DRAW_INDX_2.
struct DrawPacket
{
    uint32_t primType = 0;      // VGT_DRAW_INITIATOR.prim_type
    uint32_t sourceSelect = 0;  // 0 DMA (indexed), 1 immediate, 2 auto-index
    uint32_t indexSize32 = 0;   // 1 = 32-bit indices
    uint32_t numIndices = 0;
    uint32_t dmaBase = 0, dmaWords = 0, dmaEndian = 0;   // indexed draws
    const char* where = "";     // "ring"/"ib" for diagnostics
    uint32_t packetAddr = 0;
};

// ---- entry points called by the command processor -------------------------
// IM_LOAD / IM_LOAD_IMMEDIATE: shader microcode (big-endian dwords as in memory).
void LoadShader(ShaderType type, const uint8_t* ucodeBigEndian, uint32_t dwords, uint32_t guestAddress);
void Draw(const DrawPacket& draw);
// A frame boundary (XE_SWAP executed).
void FrameEnd(uint32_t frontBufferAddress, uint32_t width, uint32_t height);

const char* PrimitiveName(uint32_t primType);

}  // namespace xenos
