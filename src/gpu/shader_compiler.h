#pragma once

// HLSL to D3D12 bytecode. DXC (DXIL, shader model 6.0) when dxcompiler.dll and dxil.dll sit next
// to the executable (the build copies them from the Windows SDK) and the device supports shader
// model 6.0; otherwise D3DCompile (DXBC, shader model 5.1), which is up to 30 times slower on the
// translated shaders. HC09_FXC=1 forces D3DCompile.
//
// A pipeline cannot mix DXBC and DXIL, so every shader of the session comes from one compiler,
// and compiled shaders cached on disk are kept apart by CompilerTag().

#include <string_view>

#include <d3dcommon.h>

#include "gpu/d3d12_context.h"

namespace hc09::gpu
{
    enum class ShaderStage
    {
        Vertex,
        Pixel,
        Geometry,
        Compute,
    };

    // Chooses the compiler; call once with the device before compiling.
    void InitializeShaderCompiler(ID3D12Device* device);

    // Switches to D3DCompile for the rest of the session (DXC's output was rejected).
    void FallBackToD3DCompile();

    bool UsingDxc();

    // "dxil" or "dxbc": the kind of bytecode CompileShader returns.
    const char* CompilerTag();

    // Thread-safe. Prints the errors and returns null on failure.
    ComPtr<ID3DBlob> CompileShader(std::string_view source, ShaderStage stage, const char* entry, const char* name);
}
