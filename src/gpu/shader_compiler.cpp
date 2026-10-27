#include "gpu/shader_compiler.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <d3dcompiler.h>

#if __has_include(<dxcapi.h>)
#include <dxcapi.h>
#define HC09_HAVE_DXC 1
#endif

namespace hc09::gpu
{
    namespace
    {
        std::atomic<bool> g_dxc{ false };

        const char* D3DCompileTarget(ShaderStage stage)
        {
            switch (stage)
            {
            case ShaderStage::Pixel: return "ps_5_1";
            case ShaderStage::Geometry: return "gs_5_1";
            default: return "vs_5_1";
            }
        }

        ComPtr<ID3DBlob> CompileWithD3DCompile(std::string_view source, ShaderStage stage, const char* entry, const char* name)
        {
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(D3DCompile(source.data(), source.size(), name, nullptr, nullptr, entry, D3DCompileTarget(stage),
                    D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors)))
            {
                std::fprintf(stderr, "[gpu] %s compile failed:\n%s\n", name, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                return nullptr;
            }
            return code;
        }

#ifdef HC09_HAVE_DXC
        DxcCreateInstanceProc g_createInstance = nullptr;

        const wchar_t* DxcTarget(ShaderStage stage)
        {
            switch (stage)
            {
            case ShaderStage::Pixel: return L"ps_6_0";
            case ShaderStage::Geometry: return L"gs_6_0";
            default: return L"vs_6_0";
            }
        }

        // dxcompiler.dll from the executable's folder, which also loads dxil.dll from there to
        // sign its output (D3D12 rejects unsigned DXIL).
        bool LoadDxc()
        {
            wchar_t path[MAX_PATH];
            if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
                return false;
            const std::filesystem::path folder = std::filesystem::path(path).parent_path();
            if (!std::filesystem::exists(folder / L"dxcompiler.dll") || !std::filesystem::exists(folder / L"dxil.dll"))
                return false;
            HMODULE module = LoadLibraryExW((folder / L"dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!module)
                return false;
            g_createInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
            return g_createInstance != nullptr;
        }

        ComPtr<ID3DBlob> CompileWithDxc(std::string_view source, ShaderStage stage, const char* entry, const char* name)
        {
            // A compiler instance is not to be shared between threads.
            thread_local ComPtr<IDxcCompiler3> compiler;
            if (!compiler && FAILED(g_createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
                return nullptr;
            const std::wstring entryW(entry, entry + std::strlen(entry));
            const std::wstring nameW(name, name + std::strlen(name));
            const wchar_t* arguments[] = { nameW.c_str(), L"-E", entryW.c_str(), L"-T", DxcTarget(stage), L"-O1" };
            const DxcBuffer buffer{ source.data(), source.size(), DXC_CP_UTF8 };
            ComPtr<IDxcResult> result;
            HRESULT status = E_FAIL;
            if (FAILED(compiler->Compile(&buffer, arguments, UINT32(std::size(arguments)), nullptr, IID_PPV_ARGS(&result))) ||
                FAILED(result->GetStatus(&status)) || FAILED(status))
            {
                ComPtr<IDxcBlobUtf8> errors;
                if (result)
                    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
                std::fprintf(stderr, "[gpu] %s compile failed:\n%s\n", name, errors ? errors->GetStringPointer() : "");
                return nullptr;
            }
            ComPtr<IDxcBlob> object;
            ComPtr<ID3DBlob> code;
            if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object ||
                FAILED(D3DCreateBlob(object->GetBufferSize(), &code)))
                return nullptr;
            std::memcpy(code->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
            return code;
        }
#endif
    }

    void InitializeShaderCompiler(ID3D12Device* device)
    {
#ifdef HC09_HAVE_DXC
        D3D12_FEATURE_DATA_SHADER_MODEL model{ D3D_SHADER_MODEL_6_0 };
        const bool supported = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) &&
            model.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
        g_dxc = std::getenv("HC09_FXC") == nullptr && supported && LoadDxc();
#else
        (void)device;
#endif
        std::fprintf(stderr, "[gpu] shader compiler: %s\n", g_dxc ? "DXC (shader model 6.0)" : "D3DCompile (shader model 5.1)");
    }

    void FallBackToD3DCompile()
    {
        if (g_dxc.exchange(false))
            std::fprintf(stderr, "[gpu] DXC's shaders were rejected; using D3DCompile\n");
    }

    bool UsingDxc()
    {
        return g_dxc;
    }

    const char* CompilerTag()
    {
        return g_dxc ? "dxil" : "dxbc";
    }

    ComPtr<ID3DBlob> CompileShader(std::string_view source, ShaderStage stage, const char* entry, const char* name)
    {
#ifdef HC09_HAVE_DXC
        if (g_dxc)
            return CompileWithDxc(source, stage, entry, name);
#endif
        return CompileWithD3DCompile(source, stage, entry, name);
    }
}
