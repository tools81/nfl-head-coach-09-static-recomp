#pragma once

// Screen-space ambient occlusion and reflections for the game's 3D scenes.
//
// The 3D scene of a frame is the predicated-tiling pass: its first pass draws the whole scene
// into a multisampled colour target and depth buffer, before the interface is drawn into other
// surfaces. When that pass ends (its first resolve), Apply works on those targets in place:
// ambient occlusion darkens the colour from the depth buffer, and reflections blend in a copy
// of the scene on surfaces that face up (the field).
//
// Both need the camera: the scene's vertex shaders transform world positions by a matrix in
// vertex constants c156..c159 (the view-projection, or an object's world-view-projection).
// Every such matrix shares the projection, which SceneCamera recovers: how depth maps back to
// distance, the field of view, and the world's up direction in view space.
//
// Settings: hc09.ini [Graphics] AmbientOcclusion and Reflections (strength, 0 turns it off),
// overridden by HC09_SSAO and HC09_SSR.

#include <cstdint>
#include <utility>

#include "gpu/d3d12_context.h"

namespace hc09::gpu
{
    struct SceneCamera
    {
        bool valid = false;
        // Depth buffer value d -> view distance w: w = depthB / (d - depthA).
        float depthA = 0, depthB = 0;
        // Clip x = sx * view x + cx * w (and y): the projection's scale and centre.
        float sx = 0, sy = 0, cx = 0, cy = 0;
        float up[3]{};  // world up in view space (x right, y up, z forward)
    };

    // The camera from the four float4 constants c156..c159 (host byte order), each the clip-space
    // image of a world axis (x, y, z) and of the origin. Invalid unless they form a perspective
    // projection.
    SceneCamera CameraFromConstants(const float* constants);

    class SceneEffects
    {
    public:
        bool Initialize(D3D12Context& context);
        bool enabled() const { return occlusion_ > 0 || reflections_ > 0; }

        // Applies the effects to a scene's colour target and depth buffer (width x height pixels,
        // `samples` per pixel). Both are left in the states they came in.
        void Apply(ID3D12Resource* color, D3D12_RESOURCE_STATES& colorState, D3D12_CPU_DESCRIPTOR_HANDLE colorRtv,
            DXGI_FORMAT format, ID3D12Resource* depth, D3D12_RESOURCE_STATES& depthState, uint32_t samples, uint32_t width,
            uint32_t height, const SceneCamera& camera);

    private:
        struct Pipelines
        {
            ComPtr<ID3D12PipelineState> occlusion, blur, composite, reflections, reflectionComposite;
        };
        struct Target
        {
            ComPtr<ID3D12Resource> resource;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        };

        Pipelines* GetPipelines(DXGI_FORMAT format, uint32_t samples);
        bool Prepare(Target& target, DXGI_FORMAT format, uint32_t width, uint32_t height, bool renderTarget);
        void Pass(ID3D12PipelineState* pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12Resource* depth, uint32_t samples,
            ID3D12Resource* input0, DXGI_FORMAT input0Format, ID3D12Resource* input1, const void* constants, uint32_t width,
            uint32_t height);

        D3D12Context* context_ = nullptr;
        float occlusion_ = 0, reflections_ = 0;
        int debug_ = 0;
        bool loggedCamera_ = false;
        uint32_t applied_ = 0;
        ComPtr<ID3D12RootSignature> rootSignature_;
        std::pair<uint64_t, Pipelines> pipelines_[4]{};  // by (format << 8 | samples)
        Target occlusionRaw_, occlusionBlurred_, sceneCopy_, reflectionTarget_;
    };
}
