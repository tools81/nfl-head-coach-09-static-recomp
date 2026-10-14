#include "gpu/shaders.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace hc09::gpu
{
    namespace
    {
        std::mutex g_mutex;
        std::unordered_map<uint64_t, std::shared_ptr<const ShaderMicrocode>> g_shaders;
        std::shared_ptr<const ShaderMicrocode> g_active[2];
        const bool g_dump = std::getenv("HC09_DUMP_SHADERS") != nullptr;

        uint64_t Hash(ShaderType type, const std::vector<uint32_t>& dwords)
        {
            uint64_t hash = 0xCBF29CE484222325ull ^ uint64_t(type);  // FNV-1a over the bytes
            for (uint32_t dword : dwords)
            {
                for (int i = 0; i < 4; i++)
                {
                    hash ^= (dword >> (i * 8)) & 0xFF;
                    hash *= 0x100000001B3ull;
                }
            }
            return hash;
        }

        void Dump(const ShaderMicrocode& shader)
        {
            std::filesystem::create_directories("build/shaders");
            char name[64];
            std::snprintf(name, sizeof(name), "build/shaders/%016llX.%s.bin", (unsigned long long)shader.hash,
                shader.type == ShaderType::Vertex ? "vs" : "ps");
            std::ofstream file(name, std::ios::binary);
            for (uint32_t dword : shader.dwords)
            {
                const uint32_t big = __builtin_bswap32(dword);  // keep the console's byte order
                file.write(reinterpret_cast<const char*>(&big), 4);
            }
        }
    }

    std::shared_ptr<const ShaderMicrocode> LoadShader(ShaderType type, std::vector<uint32_t> dwords)
    {
        const uint64_t hash = Hash(type, dwords);
        std::lock_guard lock(g_mutex);
        auto& slot = g_shaders[hash];
        if (!slot)
        {
            auto shader = std::make_shared<ShaderMicrocode>(ShaderMicrocode{ type, hash, std::move(dwords) });
            if (g_dump)
                Dump(*shader);
            slot = std::move(shader);
        }
        g_active[uint32_t(type) & 1] = slot;
        return slot;
    }

    std::shared_ptr<const ShaderMicrocode> ActiveShader(ShaderType type)
    {
        std::lock_guard lock(g_mutex);
        return g_active[uint32_t(type) & 1];
    }
}
