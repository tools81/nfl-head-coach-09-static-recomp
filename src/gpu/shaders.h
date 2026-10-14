#pragma once

// Shader microcode the game loads into the GPU (IM_LOAD / IM_LOAD_IMMEDIATE).

#include <cstdint>
#include <memory>
#include <vector>

namespace hc09::gpu
{
    enum class ShaderType : uint32_t
    {
        Vertex = 0,
        Pixel = 1,
    };

    struct ShaderMicrocode
    {
        ShaderType type;
        uint64_t hash;                 // of the microcode dwords
        std::vector<uint32_t> dwords;  // host-endian
    };

    // Registers microcode (deduplicated by hash) and makes it the active shader of its type.
    // When HC09_DUMP_SHADERS is set, each new shader is written to build/shaders/.
    std::shared_ptr<const ShaderMicrocode> LoadShader(ShaderType type, std::vector<uint32_t> dwords);

    std::shared_ptr<const ShaderMicrocode> ActiveShader(ShaderType type);
}
