#pragma once
// D3D-level frame capture ("XD3D"), the input of the native renderer's offline
// replay (docs/NATIVE_RENDERER.md §5 step 2). Recorded by runtime/hooks_native.cpp
// (HC09_D3D_CAPTURE=<swap>) around the GPU-facing functions of the game's Xbox
// D3D while the original D3D keeps running; read by tools/d3d_replay.cpp.
//
// Everything is addressed physically (guest virtual & 0x1FFFFFFF: all guest
// memory is one 512 MiB physical space, runtime.cpp's memory map), including
// the D3D device structure and resource headers.
//
// File (host little-endian): "XD3D", u32 version, u64 swap (D3D Swap calls
// before the frame), u32 device physical address, then records tagged by one byte:
//   'P' u32 page, 4096 bytes          a 4 KiB page, when first touched and again
//                                     whenever its contents changed since
//   'M' u32 n, n x (u32 off, u32 w)   device words changed since the last 'M'
//                                     (offset in bytes, word as in memory: big-endian)
//   'N' u32 device, then as 'M'       the same for another device (D3D's command-buffer
//                                     device, physical address): calls whose r3 is it
//   'S' u32 device, then as 'M'       a device's whole contents (words not listed are
//                                     zero): at the start of every command-buffer
//                                     recording, and for every device after the prologue
//   'C' u32 fn, u32 depth, u32 lr, u32 r[8] (r3..r10), u64 f1, u64 f2, u32 ret, u32 blobs,
//       blobs x (u32 tag, u32 bytes, bytes)
//                                     a call of `fn` (CallId) with its arguments;
//                                     the device ('M') before it is already applied.
//                                     depth: 0 = called by the game, 1+ = by D3D itself;
//                                     lr: the caller
//   'A' u32 fn                        the call `fn` returned: the 'M' just before it is
//                                     the device after the call (draws: the state D3D
//                                     flushed, e.g. SQ_PROGRAM_CNTL from the shaders)
//   'R' u32 fn, u32 value              the return value of the call just completed
//                                     (BeginVertices: the vertex data pointer)
//   'V' u32 vs, u32 variant, u32 dst, u32 bytes, bytes
//                                     vertex shader microcode as patched for the current
//                                     vertex declaration (sub_8232BD98), big-endian dwords.
//                                     dst = where: the shader's own microcode (patched in
//                                     place, lasting) or a copy in the ring (the GPU still
//                                     used the original: this draw only)
//   'K'                               D3D submitted the commands so far to the GPU
//                                     (sub_82328440): pages the draws since the previous
//                                     'K' read were checked again just before it. The GPU
//                                     reads draw inputs after this point, so a renderer
//                                     replaying the capture executes queued work here.
//   'E'                               end
// Pages hold the contents at the time of the record that follows them: a reader
// applies records strictly in order.
// Prologue: right after the header, the latest recording of every command buffer
// recorded since the capture was armed (BeginCommandBuffer .. EndCommandBuffer on
// the recording device, with an 'S' snapshot of that device first and the pages
// its draws read), as ordinary records; then an 'S' for every device.
// Version 6 added 'S' (5 started recordings with an 'M'/'N' of the nonzero words,
// which left a reader's older words in place).
#include <cstdint>

namespace d3dcap {

constexpr uint32_t kVersion = 6;
constexpr uint32_t kDeviceBytes = 22528;

enum CallId : uint32_t
{
    kDrawIndexedVertices = 1,   // r4 prim, r5 BaseVertexIndex, r6 StartIndex, r7 IndexCount
    kDrawVertices = 2,          // r4 prim, r5 StartVertex, r6 VertexCount
    kBeginVertices = 3,         // r4 prim, r5 VertexCount, r6 stride; ret = data pointer
    kEndVertices = 4,           // blob kBlobVertices: the data written since BeginVertices
    kGpuBeginShaderConstantF4 = 5,   // r4 pixel, r5 start, r6 count; ret = data pointer;
                                     // blob kBlobConstants (recorded at the next call)
    kGpuLoadShaderConstantF4Pointer = 6,   // r4 pixel, r5 start, r6 data, r7 count; blob kBlobConstants
    kClearF = 7,                // r4 flags, r5 rect*, r6 colour*, f1 z, r8 stencil (blobs kBlobRect, kBlobColor);
                                // the rectangle (nullptr: the target) is clipped to the viewport
    kResolve = 8,               // r4 flags, r5 rect*, r6 dest texture, r7 point*, r8 level, r9 slice,
                                // r10 colour*, f1 z (blobs kBlobRect, kBlobPoint, kBlobColor, kBlobStack)
    kBeginTiling = 9,           // r4 flags, r5 count, r6 rects*, r7 colour*, f1 z, r9 stencil (kBlobRects, kBlobColor)
    kEndTiling = 10,            // r4 flags, r5 rects*, r6 dest texture, r7 colour*, f1 z, r9 stencil, r10 params
    kSetPredication = 11,       // r4 mask
    kSwap = 12,                 // r4 front buffer texture, r5 scaler params
    kInsertCallback = 13,       // r4 flags, r5 function, r6 context
    kBeginCommandBuffer = 14,
    kEndCommandBuffer = 15,
    kRunCommandBuffer = 16,
    kDrawVerticesUP = 17,       // r4 prim, r5 VertexCount, r6 data, r7 stride (blob kBlobVertices);
                                // its BeginVertices is an inner call (the commit is inlined)
    kCallCount
};
inline const char* CallName(uint32_t id)
{
    static const char* const n[] = { "?", "DrawIndexedVertices", "DrawVertices", "BeginVertices", "EndVertices",
                                     "GpuBeginShaderConstantF4", "GpuLoadShaderConstantF4Pointer", "ClearF", "Resolve",
                                     "BeginTiling", "EndTiling", "SetPredication", "Swap", "InsertCallback",
                                     "BeginCommandBuffer", "EndCommandBuffer", "RunCommandBuffer", "DrawVerticesUP" };
    return id < kCallCount ? n[id] : "?";
}

enum BlobTag : uint32_t
{
    kBlobVertices = 1, kBlobConstants = 2, kBlobRect = 3, kBlobPoint = 4, kBlobColor = 5, kBlobRects = 6, kBlobStack = 7,
};

// ---- the device structure (offsets in bytes; docs/NATIVE_RENDERER.md) --------
namespace dev {
constexpr uint32_t kDirtyVsConst = 0, kDirtyPsConst = 8, kDirtyState = 16, kDirtyFetch = 24, kDirtyMisc = 32, kDirtyHiz = 40;
constexpr uint32_t kFetch = 1152;          // 32 x 24 bytes: fetch constants (registers 0x4800+)
constexpr uint32_t kVsConst = 1920;        // 256 x 16: VS float constants (0x4000+)
constexpr uint32_t kPsConst = 6016;        // 256 x 16: PS float constants (0x4400+)
constexpr uint32_t kBoolLoop = 10112;      // bool (8 words) and loop (32 words) constants (0x4900+)
constexpr uint32_t kFenceBlock = 10896;    // pointer to the GPU-written fence block (+0 last fence done)
constexpr uint32_t kNextFence = 10908;
constexpr uint32_t kIndices = 12436;       // index buffer
constexpr uint32_t kRenderTarget = 12440;  // RT0..RT3
constexpr uint32_t kDepth = 12456;
constexpr uint32_t kStreams = 12460;       // 16 vertex buffers
constexpr uint32_t kStrides = 12528;       // 16 bytes: stride / 4 per stream
constexpr uint32_t kTextures = 12544;      // 26 textures
constexpr uint32_t kViewport = 12648;
constexpr uint32_t kScissor = 12676;
constexpr uint32_t kPixelShader = 12692;
constexpr uint32_t kVertexShader = 12696;
constexpr uint32_t kVertexDecl = 11820;
constexpr uint32_t kSwapCount = 16560;
}  // namespace dev

// Register blocks shadowed in the device: {device offset, first register, count}.
struct ShadowBlock { uint32_t off, reg, count; };
constexpr ShadowBlock kShadowBlocks[] = {
    { 10368, 0x2000, 19 }, { 10444, 0x2100, 21 }, { 10528, 0x2180, 5 }, { 10548, 0x2200, 12 },
    { 10596, 0x2280, 21 }, { 10680, 0x2300, 38 }, { 10832, 0x2380, 16 },
};

}  // namespace d3dcap
