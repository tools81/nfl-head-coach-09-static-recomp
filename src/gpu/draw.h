#pragma once

// A draw as decoded from DRAW_INDX / DRAW_INDX_2. Everything else (shaders, constants,
// render state) is read from the register file when the draw executes.

#include <cstdint>

namespace hc09::gpu
{
    enum class PrimitiveType : uint32_t
    {
        None = 0x00,
        PointList = 0x01,
        LineList = 0x02,
        LineStrip = 0x03,
        TriangleList = 0x04,
        TriangleFan = 0x05,
        TriangleStrip = 0x06,
        TriangleWithWFlags = 0x07,
        RectangleList = 0x08,
        LineLoop = 0x0C,
        QuadList = 0x0D,
        QuadStrip = 0x0E,
        Polygon = 0x15,
    };

    enum class IndexSource : uint32_t
    {
        Dma = 0,        // index buffer in memory
        Immediate = 1,  // indices inline in the packet
        Auto = 2,       // sequential 0..count-1
    };

    struct DrawInfo
    {
        PrimitiveType primitive;
        IndexSource source;
        uint32_t indexCount;
        bool index32;
        uint32_t indexAddress;   // guest address (Dma)
        uint32_t indexEndian;    // GPU swap mode for the index buffer (Dma)
        const uint32_t* immediateIndices = nullptr;  // packed 16-bit pairs (Immediate)
        uint32_t immediateDwords = 0;
    };

    // Executes a draw against the current register state (implemented by the renderer).
    void ExecuteDraw(const DrawInfo& draw);
    // XE_SWAP: present the front buffer at `frontBuffer` (physical address).
    void ExecuteSwap(uint32_t frontBuffer, uint32_t width, uint32_t height);
    // Number of swaps so far (the frame counter used by the HC09_* debug options).
    int FrameNumber();

    // True while processing the frame selected by HC09_DUMP_FRAME (packet tracing).
    bool IsDumpFrame();
}
