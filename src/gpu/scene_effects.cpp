#include "gpu/scene_effects.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "gpu/shader_compiler.h"

namespace hc09::gpu
{
    namespace
    {
        // Root constants b0, laid out as the shaders' cbuffer (float4 rows).
        struct Constants
        {
            float size[2], invSize[2];
            float depthA, depthB, invProj[2];
            float center[2], proj[2];
            float up[3], radius;
            float occlusion, reflections, debug, pad0;
            float blurDirection[2], pad1[2];
        };
        constexpr uint32_t kConstantDwords = sizeof(Constants) / 4;

        const char* const kShaders = R"(
cbuffer Effects : register(b0)
{
    float2 size;
    float2 invSize;
    float depthA;
    float depthB;
    float2 invProj;
    float2 center;
    float2 proj;
    float3 upView;
    float radius;
    float occlusionStrength;
    float reflectionStrength;
    float debugView;
    float pad0;
    float2 blurDirection;
    float2 pad1;
};

#if MSAA
Texture2DMS<float> depthTexture : register(t0);
float RawDepth(int2 p) { return depthTexture.Load(p, 0); }
#else
Texture2D<float> depthTexture : register(t0);
float RawDepth(int2 p) { return depthTexture.Load(int3(p, 0)); }
#endif
Texture2D<float4> input0 : register(t1);
Texture2D<float4> input1 : register(t2);
SamplerState linearClamp : register(s0);

static const float kSky = 0.999999;

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Distance along the view direction (clip w) at a pixel.
float ViewDepth(int2 p)
{
    p = clamp(p, int2(0, 0), int2(size) - 1);
    float denominator = RawDepth(p) - depthA;
    return depthB / (abs(denominator) < 1e-9 ? -1e-9 : denominator);
}

// View-space position (x right, y up, z forward) of a point at pixel coordinates (pixel centres
// at .5) and view distance w.
float3 ViewPosition(float2 pixel, float w)
{
    float2 ndc = float2(pixel.x * invSize.x * 2.0 - 1.0, 1.0 - pixel.y * invSize.y * 2.0);
    return float3((ndc - center) * invProj * w, w);
}

float2 ProjectToPixel(float3 position)
{
    float2 ndc = position.xy * proj / position.z + center;
    return float2((ndc.x + 1.0) * 0.5 * size.x, (1.0 - ndc.y) * 0.5 * size.y);
}

float3 PositionAt(int2 p)
{
    return ViewPosition(float2(p) + 0.5, ViewDepth(p));
}

// The surface normal from the depth of the neighbours, taking on each axis the side that
// continues the surface (not the one across an edge). Faces the camera.
float3 NormalAt(int2 p, float3 position)
{
    float3 left = PositionAt(p + int2(-1, 0)), right = PositionAt(p + int2(1, 0));
    float3 up = PositionAt(p + int2(0, -1)), down = PositionAt(p + int2(0, 1));
    float3 dx = abs(right.z - position.z) < abs(position.z - left.z) ? right - position : position - left;
    float3 dy = abs(down.z - position.z) < abs(position.z - up.z) ? down - position : position - up;
    float3 normal = normalize(cross(dy, dx));
    return dot(normal, position) > 0.0 ? -normal : normal;
}

float Noise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Ambient occlusion: how much of the hemisphere over the surface nearby geometry covers, from 16
// depth samples in a disc of `radius` (view units) around the pixel. Output: occlusion (1 open,
// 0 covered) and the view distance, for the blur.
float4 ps_occlusion(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float raw = RawDepth(p);
    float3 center3 = PositionAt(p);
    if (raw >= kSky)
        return float4(1.0, center3.z, 0.0, 0.0);
    float3 normal = NormalAt(p, center3);
    float pixels = radius * proj.y * 0.5 * size.y / center3.z;
    if (pixels < 2.0)
        return float4(1.0, center3.z, 0.0, 0.0);
    pixels = min(pixels, size.y * 0.15);
    const int kSamples = 16;
    float angle = Noise(position.xy) * 6.2831853;
    float covered = 0.0;
    for (int i = 0; i < kSamples; i++)
    {
        float t = (i + 0.5) / kSamples;
        float a = angle + i * 2.3999632;  // golden angle
        float2 q = position.xy + float2(cos(a), sin(a)) * sqrt(t) * pixels;
        float3 sample3 = PositionAt(int2(q));
        float3 v = sample3 - center3;
        float distance2 = dot(v, v);
        float cosine = dot(v, normal) * rsqrt(distance2 + 1e-6);
        float falloff = saturate(1.0 - distance2 / (radius * radius * 4.0));
        covered += saturate((cosine - 0.15) / 0.85) * falloff;
    }
    return float4(saturate(1.0 - covered * 2.0 / kSamples), center3.z, 0.0, 0.0);
}

// One direction of a depth-aware blur of the occlusion (9 taps).
float2 Blur(int2 p)
{
    float2 middle = input0.Load(int3(p, 0)).rg;
    float sum = middle.r, weights = 1.0;
    for (int i = -4; i <= 4; i++)
    {
        if (i == 0)
            continue;
        int2 q = clamp(p + int2(blurDirection * i), int2(0, 0), int2(size) - 1);
        float2 sample2 = input0.Load(int3(q, 0)).rg;
        float weight = exp(-i * i / 18.0) * exp(-abs(sample2.g - middle.g) / (middle.g * 0.02 + 1e-3));
        sum += sample2.r * weight;
        weights += weight;
    }
    return float2(sum / weights, middle.g);
}

float4 ps_blur(float4 position : SV_Position) : SV_Target
{
    return float4(Blur(int2(position.xy)), 0.0, 0.0);
}

// The second blur direction, multiplied into the scene (blend: destination * source).
float4 ps_composite(float4 position : SV_Position) : SV_Target
{
    float occlusion = debugView == 1.0 ? Blur(int2(position.xy)).r : lerp(1.0, Blur(int2(position.xy)).r, occlusionStrength);
    return float4(occlusion, occlusion, occlusion, 1.0);
}

// Reflections on surfaces facing up: a ray marched through the depth buffer from the surface
// along the mirrored view direction; where it passes behind the scene's depth it hits, fading
// with the distance the ray travelled, so what stands on the surface shows near where it stands.
// Grass is no mirror: the reflected scene (input0, a copy) is drawn dark and with strong contrast,
// so it reads as a shadow cast by the player rather than an image of him. Output premultiplied by
// its opacity, for ps_reflection_composite.
float4 ps_reflections(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    if (RawDepth(p) >= kSky)
        return 0.0;
    float3 origin = PositionAt(p);
    float3 normal = NormalAt(p, origin);
    float facing = saturate((dot(normal, upView) - 0.9) / 0.08);
    if (facing <= 0.0)
        return 0.0;
    float3 view = normalize(origin);
    float3 ray = reflect(view, normal);
    // Stronger at low camera angles, where the reflected shape is longest.
    float grazing = lerp(0.6, 1.0, saturate(1.0 - dot(-view, normal)));

    const int kSteps = 48;
    float stepLength = origin.z * 0.01;
    float3 previous = origin, current = origin + ray * stepLength * Noise(position.xy);
    float3 hit = 0.0;
    bool found = false;
    int step = 0;
    for (; step < kSteps; step++)
    {
        current += ray * stepLength;
        stepLength *= 1.06;
        if (current.z <= 0.0)
            break;
        float2 q = ProjectToPixel(current);
        if (any(q < 0.0) || any(q >= size))
            break;
        float behind = current.z - ViewDepth(int2(q));
        if (behind > 0.0 && behind < max(stepLength * 2.0, current.z * 0.01))
        {
            // Refine between the last point in front and this one.
            float3 front = previous, back = current;
            for (int i = 0; i < 5; i++)
            {
                float3 middle = (front + back) * 0.5;
                if (middle.z - ViewDepth(int2(ProjectToPixel(middle))) > 0.0)
                    back = middle;
                else
                    front = middle;
            }
            hit = back;
            found = true;
            break;
        }
        previous = current;
    }
    if (!found)
        return 0.0;
    float2 pixel = ProjectToPixel(hit);
    if (RawDepth(int2(pixel)) >= kSky)
        return 0.0;
    float2 edges = min(pixel, size - pixel) / (size * 0.08);
    float fade = saturate(min(edges.x, edges.y)) * saturate(1.0 - length(hit - origin) / (radius * 5.0));
    // The reflected colour's luminance, its contrast deepened and brought down to a dark tone.
    float3 color = input0.SampleLevel(linearClamp, pixel * invSize, 0).rgb;
    float luminance = dot(color, float3(0.299, 0.587, 0.114));
    float tone = saturate((luminance - 0.5) * 2.0 + 0.5) * 0.12;
    float alpha = saturate(0.45 * reflectionStrength * grazing) * facing * fade;
    return float4(tone * alpha, tone * alpha, tone * alpha, alpha);
}

// The reflections, softened (a 5x5 tent over every other pixel), over the scene (blend: source
// premultiplied).
float4 ps_reflection_composite(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float4 sum = 0.0;
    float weights = 0.0;
    for (int y = -2; y <= 2; y++)
    {
        for (int x = -2; x <= 2; x++)
        {
            float weight = (3.0 - abs(x)) * (3.0 - abs(y));
            sum += input0.Load(int3(clamp(p + int2(x, y) * 2, int2(0, 0), int2(size) - 1), 0)) * weight;
            weights += weight;
        }
    }
    sum /= weights;
    return debugView == 2.0 ? float4(sum.rgb, 1.0) : sum;
}
)";

        void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after)
        {
            if (state == after)
                return;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = state;
            barrier.Transition.StateAfter = after;
            list->ResourceBarrier(1, &barrier);
            state = after;
        }

        constexpr D3D12_RESOURCE_STATES kReadable =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr DXGI_FORMAT kOcclusionFormat = DXGI_FORMAT_R16G16_FLOAT;  // occlusion, view distance
        constexpr DXGI_FORMAT kReflectionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // premultiplied colour, opacity

        // A strength from hc09.ini [Graphics] <key>, overridden by an environment variable.
        float Strength(const wchar_t* key, const char* variable, float fallback)
        {
            if (const char* value = std::getenv(variable))
                return std::max(0.0f, float(std::atof(value)));
            wchar_t text[32] = {};
            const std::wstring file = (std::filesystem::current_path() / L"hc09.ini").wstring();
            if (GetPrivateProfileStringW(L"Graphics", key, L"", text, DWORD(std::size(text)), file.c_str()) == 0)
                return fallback;
            return std::max(0.0f, float(std::wcstod(text, nullptr)));
        }
    }

    SceneCamera CameraFromConstants(const float* c)
    {
        // c[axis * 4 + component]: the clip-space (x, y, z, w) image of world axis 0..2, then the origin.
        auto row = [c](int component, float out[4]) {
            for (int axis = 0; axis < 4; axis++)
                out[axis] = c[axis * 4 + component];
        };
        float x[4], y[4], z[4], w[4];
        row(0, x), row(1, y), row(2, z), row(3, w);
        auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        const float ww = dot3(w, w);
        SceneCamera camera;
        if (!(ww > 1e-12f) || !std::isfinite(ww))
            return camera;  // no perspective (w does not depend on the position)
        // A perspective projection's z is a*w + b.
        camera.depthA = dot3(z, w) / ww;
        camera.depthB = z[3] - camera.depthA * w[3];
        float residual[3], ex[3], ey[3];
        camera.cx = dot3(x, w) / ww;
        camera.cy = dot3(y, w) / ww;
        for (int i = 0; i < 3; i++)
        {
            residual[i] = z[i] - camera.depthA * w[i];
            ex[i] = x[i] - camera.cx * w[i];
            ey[i] = y[i] - camera.cy * w[i];
        }
        const float scale = std::sqrt(ww);
        camera.sx = std::sqrt(dot3(ex, ex)) / scale;
        camera.sy = std::sqrt(dot3(ey, ey)) / scale;
        if (std::sqrt(dot3(residual, residual)) > 1e-3f * scale * std::fabs(camera.depthA) || std::fabs(camera.depthB) < 1e-6f ||
            camera.sx < 0.05f || camera.sy < 0.05f || std::fabs(dot3(ex, ey)) > 0.05f * camera.sx * camera.sy * ww)
            return camera;
        // World up (axis 1) in view space.
        float up[3] = { ex[1] / camera.sx, ey[1] / camera.sy, w[1] };
        const float length = std::sqrt(dot3(up, up));
        if (!(length > 1e-6f))
            return camera;
        for (int i = 0; i < 3; i++)
            camera.up[i] = up[i] / length;
        camera.valid = true;
        return camera;
    }

    bool SceneEffects::Initialize(D3D12Context& context)
    {
        context_ = &context;
        occlusion_ = Strength(L"AmbientOcclusion", "HC09_SSAO", 1.0f);
        reflections_ = Strength(L"Reflections", "HC09_SSR", 1.0f);
        // HC09_EFFECTS_DEBUG=1 shows the occlusion alone, 2 the reflections alone, 3 applies both to
        // the left half of the screen only and 4 to every other scene (for comparisons).
        debug_ = std::getenv("HC09_EFFECTS_DEBUG") ? std::atoi(std::getenv("HC09_EFFECTS_DEBUG")) : 0;
        if (debug_ == 1)
            reflections_ = 0;
        else if (debug_ == 2)
            occlusion_ = 0;
        if (!enabled())
            return true;

        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, kConstantDwords };
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_DESCRIPTOR_RANGE views{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &views };
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc{ 2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(context.device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_))))
        {
            std::fprintf(stderr, "[effects] root signature failed; ambient occlusion and reflections are off\n");
            occlusion_ = reflections_ = 0;
            return false;
        }
        std::fprintf(stderr, "[effects] ambient occlusion %.2f, reflections %.2f\n", occlusion_, reflections_);
        return true;
    }

    SceneEffects::Pipelines* SceneEffects::GetPipelines(DXGI_FORMAT format, uint32_t samples)
    {
        const uint64_t key = (uint64_t(format) << 8) | samples;
        for (auto& [existing, pipelines] : pipelines_)
            if (existing == key)
                return pipelines.occlusion ? &pipelines : nullptr;
        auto* slot = &pipelines_[0];
        for (auto& entry : pipelines_)
            if (entry.first == 0)
            {
                slot = &entry;
                break;
            }
        *slot = { key, {} };
        Pipelines& pipelines = slot->second;

        const std::string source = std::string(samples > 1 ? "#define MSAA 1\n" : "#define MSAA 0\n") + kShaders;
        ComPtr<ID3DBlob> vs = CompileShader(source, ShaderStage::Vertex, "vs_main", "effects");
        auto create = [&](const char* entry, DXGI_FORMAT target, uint32_t targetSamples, int blend, ComPtr<ID3D12PipelineState>& out) {
            ComPtr<ID3DBlob> ps = CompileShader(source, ShaderStage::Pixel, entry, entry);
            if (!vs || !ps)
                return false;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = rootSignature_.Get();
            desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            auto& rt = desc.BlendState.RenderTarget[0];
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (blend)
            {
                // 1: destination * source colour; 2: premultiplied source over destination. Alpha kept.
                rt.BlendEnable = TRUE;
                rt.SrcBlend = blend == 1 ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE;
                rt.DestBlend = blend == 1 ? D3D12_BLEND_SRC_COLOR : D3D12_BLEND_INV_SRC_ALPHA;
                rt.BlendOp = D3D12_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
                rt.DestBlendAlpha = D3D12_BLEND_ONE;
                rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            }
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = target;
            desc.SampleDesc.Count = targetSamples;
            return SUCCEEDED(context_->device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out)));
        };
        if (!create("ps_occlusion", kOcclusionFormat, 1, 0, pipelines.occlusion) ||
            !create("ps_blur", kOcclusionFormat, 1, 0, pipelines.blur) ||
            !create("ps_composite", format, samples, debug_ == 1 ? 0 : 1, pipelines.composite) ||
            !create("ps_reflections", kReflectionFormat, 1, 0, pipelines.reflections) ||
            !create("ps_reflection_composite", format, samples, debug_ == 2 ? 0 : 2, pipelines.reflectionComposite))
        {
            std::fprintf(stderr, "[effects] pipelines for format %d x%u failed; no effects on that target\n", int(format), samples);
            pipelines = {};
            return nullptr;
        }
        return &pipelines;
    }

    bool SceneEffects::Prepare(Target& target, DXGI_FORMAT format, uint32_t width, uint32_t height, bool renderTarget)
    {
        if (target.resource)
        {
            const D3D12_RESOURCE_DESC existing = target.resource->GetDesc();
            if (existing.Width == width && existing.Height == height && existing.Format == format)
                return true;
        }
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
        target.state = renderTarget ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST;
        // The old resource may still be in use by frames in flight: this happens only when the
        // scene's size or format changes, so wait for the GPU first.
        if (target.resource)
            context_->Flush();
        target.resource.Reset();
        if (FAILED(context_->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, target.state, nullptr,
                IID_PPV_ARGS(&target.resource))))
            return false;
        if (renderTarget)
        {
            if (!target.rtv.ptr)
                target.rtv = context_->AllocateRtv();
            context_->device()->CreateRenderTargetView(target.resource.Get(), nullptr, target.rtv);
        }
        return true;
    }

    void SceneEffects::Pass(ID3D12PipelineState* pipeline, D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12Resource* depth, uint32_t samples,
        ID3D12Resource* input0, DXGI_FORMAT input0Format, ID3D12Resource* input1, const void* constants, uint32_t width,
        uint32_t height)
    {
        auto* list = context_->list();
        auto* device = context_->device();
        const DescriptorRange views = context_->AllocateViews(3);
        const uint32_t increment = context_->viewDescriptorSize();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        srv.ViewDimension = samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (samples == 1)
            srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(depth, &srv, views.cpu);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = input0Format;
        device->CreateShaderResourceView(input0, &srv, { views.cpu.ptr + increment });
        srv.Format = kOcclusionFormat;
        device->CreateShaderResourceView(input1, &srv, { views.cpu.ptr + 2 * increment });

        list->SetGraphicsRootSignature(rootSignature_.Get());
        list->SetPipelineState(pipeline);
        list->SetGraphicsRoot32BitConstants(0, kConstantDwords, constants, 0);
        list->SetGraphicsRootDescriptorTable(1, views.gpu);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{ 0, 0, float(width), float(height), 0, 1 };
        // Split-screen comparison: the scene's colour is changed on the left half only.
        const bool split = debug_ == 3 && rtv.ptr != occlusionRaw_.rtv.ptr && rtv.ptr != occlusionBlurred_.rtv.ptr &&
            rtv.ptr != reflectionTarget_.rtv.ptr;
        const D3D12_RECT scissor{ 0, 0, LONG(split ? width / 2 : width), LONG(height) };
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }

    void SceneEffects::Apply(ID3D12Resource* color, D3D12_RESOURCE_STATES& colorState, D3D12_CPU_DESCRIPTOR_HANDLE colorRtv,
        DXGI_FORMAT format, ID3D12Resource* depth, D3D12_RESOURCE_STATES& depthState, uint32_t samples, uint32_t width,
        uint32_t height, const SceneCamera& camera)
    {
        if (!enabled() || !camera.valid || (debug_ == 4 && (applied_++ & 1)))
            return;
        if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R10G10B10A2_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT)
            return;
        Pipelines* pipelines = GetPipelines(format, samples);
        // The blurred occlusion target is also the (unread) third view of the reflection passes.
        if (!pipelines || !Prepare(occlusionBlurred_, kOcclusionFormat, width, height, true) ||
            (occlusion_ > 0 && !Prepare(occlusionRaw_, kOcclusionFormat, width, height, true)) ||
            (reflections_ > 0 && (!Prepare(sceneCopy_, format, width, height, false) ||
                !Prepare(reflectionTarget_, kReflectionFormat, width, height, true))))
            return;

        Constants constants{};
        constants.size[0] = float(width);
        constants.size[1] = float(height);
        constants.invSize[0] = 1.0f / width;
        constants.invSize[1] = 1.0f / height;
        constants.depthA = camera.depthA;
        constants.depthB = camera.depthB;
        constants.invProj[0] = 1.0f / camera.sx;
        constants.invProj[1] = 1.0f / camera.sy;
        constants.center[0] = camera.cx;
        constants.center[1] = camera.cy;
        constants.proj[0] = camera.sx;
        constants.proj[1] = camera.sy;
        std::memcpy(constants.up, camera.up, sizeof(constants.up));
        constants.radius = 0.6f;  // view units (about yards in the game's scenes)
        constants.occlusion = std::min(occlusion_, 1.0f);
        constants.reflections = reflections_;
        constants.debug = float(debug_);
        if (!loggedCamera_)
        {
            loggedCamera_ = true;
            std::fprintf(stderr, "[effects] first scene %ux%u x%u: depth a %g b %g, scale %g %g, centre %g %g, up %.3f %.3f %.3f\n", width,
                height, samples, camera.depthA, camera.depthB, camera.sx, camera.sy, camera.cx, camera.cy, camera.up[0], camera.up[1],
                camera.up[2]);
        }

        auto* list = context_->list();
        const D3D12_RESOURCE_STATES colorBefore = colorState, depthBefore = depthState;
        Transition(list, depth, depthState, kReadable);
        if (occlusion_ > 0)
        {
            Transition(list, occlusionRaw_.resource.Get(), occlusionRaw_.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            // The occlusion pass reads only depth: its other views are placeholders.
            Pass(pipelines->occlusion.Get(), occlusionRaw_.rtv, depth, samples, occlusionRaw_.resource.Get(), kOcclusionFormat,
                occlusionRaw_.resource.Get(), &constants, width, height);
            Transition(list, occlusionRaw_.resource.Get(), occlusionRaw_.state, kReadable);
            Transition(list, occlusionBlurred_.resource.Get(), occlusionBlurred_.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constants.blurDirection[0] = 1;
            Pass(pipelines->blur.Get(), occlusionBlurred_.rtv, depth, samples, occlusionRaw_.resource.Get(), kOcclusionFormat,
                occlusionRaw_.resource.Get(), &constants, width, height);
            Transition(list, occlusionBlurred_.resource.Get(), occlusionBlurred_.state, kReadable);
            Transition(list, color, colorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constants.blurDirection[0] = 0;
            constants.blurDirection[1] = 1;
            Pass(pipelines->composite.Get(), colorRtv, depth, samples, occlusionBlurred_.resource.Get(), kOcclusionFormat,
                occlusionBlurred_.resource.Get(), &constants, width, height);
        }
        if (reflections_ > 0)
        {
            if (samples > 1)
            {
                Transition(list, color, colorState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                Transition(list, sceneCopy_.resource.Get(), sceneCopy_.state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                list->ResolveSubresource(sceneCopy_.resource.Get(), 0, color, 0, format);
            }
            else
            {
                Transition(list, color, colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(list, sceneCopy_.resource.Get(), sceneCopy_.state, D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyResource(sceneCopy_.resource.Get(), color);
            }
            Transition(list, sceneCopy_.resource.Get(), sceneCopy_.state, kReadable);
            Transition(list, occlusionBlurred_.resource.Get(), occlusionBlurred_.state, kReadable);
            Transition(list, reflectionTarget_.resource.Get(), reflectionTarget_.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Pass(pipelines->reflections.Get(), reflectionTarget_.rtv, depth, samples, sceneCopy_.resource.Get(), format,
                occlusionBlurred_.resource.Get(), &constants, width, height);
            Transition(list, reflectionTarget_.resource.Get(), reflectionTarget_.state, kReadable);
            Transition(list, color, colorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Pass(pipelines->reflectionComposite.Get(), colorRtv, depth, samples, reflectionTarget_.resource.Get(), kReflectionFormat,
                occlusionBlurred_.resource.Get(), &constants, width, height);
        }
        Transition(list, color, colorState, colorBefore);
        Transition(list, depth, depthState, depthBefore);
    }
}
