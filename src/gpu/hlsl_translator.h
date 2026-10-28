#pragma once

// Translates Xenos shader microcode to HLSL (shader model 5.1, for D3D12).
//
// Resource binding contract with the renderer:
//   b0  cbuffer FloatConstants  float4 c[512]     shared by both stages
//   b1  cbuffer DrawState       see DrawStateLayout in d3d12_renderer.cpp
//   t0..t15   ByteAddressBuffer vertex streams (one per used vertex fetch constant,
//             in the order listed in TranslatedShader::vertexFetchConstants)
//   t16..t47  Texture2D for texture fetch constant n at t(16 + n)
//   s0..s31   SamplerState for texture fetch constant n
// The vertex shader exports 16 float4 interpolators (TEXCOORD0..15) and SV_Position;
// the pixel shader reads them into r0..r15 and writes SV_Target0..3.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/shaders.h"

namespace hc09::gpu
{
    constexpr uint32_t kMaxVertexStreams = 16;
    constexpr uint32_t kTextureSlotBase = 16;
    // Cube and 3D fetches also get a 2D view here, used when the fetch constant holds a 2D
    // texture (the hardware samples by the constant's dimension).
    constexpr uint32_t kTextureFallbackSlotBase = kTextureSlotBase + 32;

    struct TranslatedShader
    {
        bool valid = false;
        std::string error;                         // when !valid
        std::string hlsl;
        std::vector<uint32_t> vertexFetchConstants; // stream slot i reads fetch constant [i]
        std::vector<uint32_t> vertexStreamStrides;  // ... with this vertex stride (dwords)
        std::vector<uint32_t> vertexStreamTails;    // ... reading up to this dword of a vertex
        uint32_t textureFetchMask = 0;              // bit n: texture fetch constant n is sampled
        uint32_t textureCubeMask = 0;               // bit n: ... and declared as a cube map
        uint32_t textureVolumeMask = 0;             // bit n: ... and declared as a 3D texture
        // Per texture fetch constant: the filters its fetch instructions choose, which override the
        // constant's: mag (bits 0-1), min (2-3), mip (4-5), each 3 for the constant's own, and
        // anisotropy (6-8), 7 for the constant's.
        static constexpr uint16_t kFiltersFromConstant = 3 | (3 << 2) | (3 << 4) | (7 << 6);
        std::array<uint16_t, 32> textureFilters = [] {
            std::array<uint16_t, 32> filters;
            filters.fill(kFiltersFromConstant);
            return filters;
        }();
        uint32_t colorTargetMask = 0;               // pixel shader: oC0..oC3 written
        bool writesDepth = false;
    };

    TranslatedShader TranslateShader(const ShaderMicrocode& shader);
}
