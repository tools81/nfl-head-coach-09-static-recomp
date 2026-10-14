// Disassembles Xenos shader microcode dumped by HC09_DUMP_SHADERS (big-endian .bin files),
// translates it to HLSL and compiles the result with D3DCompile to validate the translator.
// Usage: XenosDisasm [--hlsl] [--flags o0|o1|o3|skip] <file.vs.bin|file.ps.bin>...
// --flags selects the D3DCompile optimization flags (default o3); compile times are reported.

#include <windows.h>
#include <d3dcompiler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gpu/hlsl_translator.h"
#include "gpu/ucode.h"

int main(int argc, char** argv)
{
    bool printHlsl = false;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    int failures = 0;
    for (int arg = 1; arg < argc; arg++)
    {
        if (std::strcmp(argv[arg], "--hlsl") == 0)
        {
            printHlsl = true;
            continue;
        }
        if (std::strcmp(argv[arg], "--flags") == 0 && arg + 1 < argc)
        {
            const std::string name = argv[++arg];
            flags = name == "o0" ? D3DCOMPILE_OPTIMIZATION_LEVEL0 : name == "o1" ? D3DCOMPILE_OPTIMIZATION_LEVEL1
                : name == "skip" ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL3;
            continue;
        }
        const std::string path = argv[arg];
        std::ifstream file(path, std::ios::binary);
        std::vector<uint32_t> dwords;
        uint32_t big;
        while (file.read(reinterpret_cast<char*>(&big), 4))
            dwords.push_back(__builtin_bswap32(big));
        const bool pixel = path.find(".ps.") != std::string::npos;
        std::printf("// %s (%zu dwords)\n%s", path.c_str(), dwords.size(), hc09::gpu::ucode::Disassemble(dwords, pixel).c_str());

        hc09::gpu::ShaderMicrocode shader{ pixel ? hc09::gpu::ShaderType::Pixel : hc09::gpu::ShaderType::Vertex, 0, dwords };
        const auto translated = hc09::gpu::TranslateShader(shader);
        if (!translated.valid)
        {
            std::printf("// translation failed: %s\n\n", translated.error.c_str());
            failures++;
            continue;
        }
        if (printHlsl)
            std::printf("%s\n", translated.hlsl.c_str());

        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const auto start = std::chrono::steady_clock::now();
        const HRESULT hr = D3DCompile(translated.hlsl.data(), translated.hlsl.size(), path.c_str(), nullptr, nullptr, "main",
            pixel ? "ps_5_1" : "vs_5_1", flags, 0, &code, &errors);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (FAILED(hr))
        {
            std::printf("// HLSL compile failed:\n%s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no log)");
            failures++;
        }
        else
        {
            std::printf("// compiled OK (%zu bytes of DXBC) in %.0f ms\n\n", code->GetBufferSize(), ms);
        }
        if (code)
            code->Release();
        if (errors)
            errors->Release();
    }
    return failures;
}
