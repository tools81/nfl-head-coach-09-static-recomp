// Executes Xenos draws, resolves and swaps with D3D12.
//
// First-light scope: one host render target per EDRAM color base + format (1280x720,
// no MSAA, no depth), shaders translated to HLSL, vertex/index/texture data copied from
// guest memory per draw, resolves copied into textures keyed by destination address, and
// the resolved front buffer drawn to the window at each swap.
//
// Debug: HC09_DUMP_FRAME=<n> prints every draw of frame n (and traces its packets);
// HC09_FIND_DWORDS=<hex,...> searches guest memory for a dword sequence at that frame.

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <set>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gpu/command_processor.h"
#include "gpu/d3d12_context.h"
#include "gpu/draw.h"
#include "gpu/hlsl_translator.h"
#include "gpu/shaders.h"
#include "gpu/texture_cache.h"
#include "kernel/guest.h"

namespace hc09::gpu
{
    extern std::atomic<uint64_t> g_textureBytesHashed, g_textureUploads, g_textureLookups, g_textureChecks;  // texture_cache.cpp

    namespace
    {
        namespace reg
        {
            constexpr uint32_t kSurfaceInfo = 0x2000;
            constexpr uint32_t kColorInfo = 0x2001;
            constexpr uint32_t kDepthInfo = 0x2002;
            constexpr uint32_t kDepthControl = 0x2200;
            constexpr uint32_t kStencilRefMask = 0x210D;
            constexpr uint32_t kDepthClear = 0x231C;
            constexpr uint32_t kColor1Info = 0x2003;
            constexpr uint32_t kColorMask = 0x2104;
            constexpr uint32_t kBlendControl0 = 0x2201;
            constexpr uint32_t kScModeControl = 0x2205;
            constexpr uint32_t kVteControl = 0x2206;
            constexpr uint32_t kModeControl = 0x2208;
            constexpr uint32_t kWindowOffset = 0x2080;
            constexpr uint32_t kWindowScissorTl = 0x2081;
            constexpr uint32_t kWindowScissorBr = 0x2082;
            constexpr uint32_t kIndexOffset = 0x2102;
            constexpr uint32_t kViewportXScale = 0x210F;
            constexpr uint32_t kVsConst = 0x2307;
            constexpr uint32_t kPsConst = 0x2308;
            constexpr uint32_t kCopyControl = 0x2318;
            constexpr uint32_t kCopyDestBase = 0x2319;
            constexpr uint32_t kCopyDestPitch = 0x231A;
            constexpr uint32_t kColorClear = 0x231D;
            constexpr uint32_t kFloatConstants = 0x4000;
            constexpr uint32_t kFetchConstants = 0x4800;
            constexpr uint32_t kBoolConstants = 0x4900;
            constexpr uint32_t kLoopConstants = 0x4908;
        }

        constexpr uint32_t kModeColorDepth = 4;
        constexpr uint32_t kModeDepth = 5;
        constexpr uint32_t kModeCopy = 6;
        constexpr uint32_t kTargetWidth = 1280;
        constexpr uint32_t kTargetHeight = 720;
        // HC09_RESOLUTION_SCALE=<1..4>: render targets (and resolve destinations) at that multiple of
        // the guest's 1280x720. Coordinates from the guest stay in guest pixels and are scaled at
        // the API boundary (viewports, scissors, copy/clear rectangles).
        const uint32_t g_scale = [] {
            const char* value = std::getenv("HC09_RESOLUTION_SCALE");
            return uint32_t(std::clamp(value ? std::atoi(value) : 1, 1, 4));
        }();
        D3D12_BOX Scaled(const D3D12_BOX& box)
        {
            return { box.left * g_scale, box.top * g_scale, box.front, box.right * g_scale, box.bottom * g_scale, box.back };
        }
        D3D12_RECT ScaledRect(const D3D12_BOX& box)
        {
            return { LONG(box.left * g_scale), LONG(box.top * g_scale), LONG(box.right * g_scale), LONG(box.bottom * g_scale) };
        }
        constexpr uint32_t kViewTableSize = kTextureFallbackSlotBase + 32;

        // Can also be set at run time by HC09_DUMP_AFTER_RESOLVE.
        int g_dumpFrame = std::getenv("HC09_DUMP_FRAME") ? std::atoi(std::getenv("HC09_DUMP_FRAME")) : -1;
        // HC09_SCREENSHOT=<frame>[,<frame>...]: frames to save as build/screenshot_<frame>.bmp
        const std::vector<int> g_screenshotFrames = [] {
            std::vector<int> frames;
            if (const char* spec = std::getenv("HC09_SCREENSHOT"))
            {
                for (const char* p = spec; *p;)
                {
                    char* end;
                    frames.push_back(int(std::strtol(p, &end, 10)));
                    if (end == p)
                        break;
                    p = *end == ',' ? end + 1 : end;
                }
            }
            return frames;
        }();
        int g_frame = 0;
        double g_shaderCompileMs = 0.0;  // total host shader compile time (for hitch reports)
        int g_drawInFrame = 0;

        float RegisterFloat(uint32_t index)
        {
            const uint32_t bits = ReadRegister(index);
            float value;
            std::memcpy(&value, &bits, 4);
            return value;
        }

        uint32_t GpuSwap(uint32_t value, uint32_t mode)
        {
            switch (mode & 3)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
            case 2: return __builtin_bswap32(value);
            case 3: return (value << 16) | (value >> 16);
            default: return value;
            }
        }

        uint64_t Mix(uint64_t hash, uint64_t value)
        {
            return (hash ^ value) * 0x100000001B3ull + (hash >> 29);
        }

        // Matches cbuffer DrawState in hlsl_translator.cpp.
        struct DrawState
        {
            uint32_t vsConstBase;
            uint32_t psConstBase;
            uint32_t wIsReciprocal;
            uint32_t xyPreDivided;
            float ndcScale[4];
            float ndcOffset[4];
            uint32_t boolConstants[8];
            uint32_t loopConstants[32];
            float textureSizes[32][4];
            uint32_t alphaFunc;  // 7 = always (alpha test disabled)
            float alphaRef;
            uint32_t vertexIndexOffset;  // VGT_INDX_OFFSET, added to every vertex index
        };

        DXGI_FORMAT ColorFormat(uint32_t format)
        {
            switch (format)
            {
            case 2: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case 3: case 7: case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case 4: return DXGI_FORMAT_R16G16_SNORM;
            case 5: return DXGI_FORMAT_R16G16B16A16_SNORM;
            case 6: return DXGI_FORMAT_R16G16_FLOAT;
            case 14: return DXGI_FORMAT_R32_FLOAT;
            case 15: return DXGI_FORMAT_R32G32_FLOAT;
            default: return DXGI_FORMAT_R8G8B8A8_UNORM;  // 8_8_8_8 and its gamma variant
            }
        }

        D3D12_BLEND BlendFactor(uint32_t factor, bool alpha)
        {
            switch (factor)
            {
            case 0: return D3D12_BLEND_ZERO;
            case 1: return D3D12_BLEND_ONE;
            case 4: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
            case 5: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
            case 6: return D3D12_BLEND_SRC_ALPHA;
            case 7: return D3D12_BLEND_INV_SRC_ALPHA;
            case 8: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
            case 9: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
            case 10: return D3D12_BLEND_DEST_ALPHA;
            case 11: return D3D12_BLEND_INV_DEST_ALPHA;
            case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
            case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
            case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
            default: return D3D12_BLEND_ONE;
            }
        }

        D3D12_BLEND_OP BlendOp(uint32_t op)
        {
            switch (op)
            {
            case 1: return D3D12_BLEND_OP_SUBTRACT;
            case 2: return D3D12_BLEND_OP_MIN;
            case 3: return D3D12_BLEND_OP_MAX;
            case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
            default: return D3D12_BLEND_OP_ADD;
            }
        }

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

        ComPtr<ID3DBlob> CompileHlsl(const std::string& source, const char* target, const char* name)
        {
            ComPtr<ID3DBlob> code, errors;
            const HRESULT hr = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, "main", target,
                D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
            if (FAILED(hr))
            {
                std::fprintf(stderr, "[gpu] %s compile failed:\n%s\n", name, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                return nullptr;
            }
            return code;
        }

        const char* const kRectangleGs = R"(
struct Vertex
{
    float4 interpolators[16] : TEXCOORD0;
    float4 position : SV_Position;
};

// Xenos rectangle lists give three corners; the fourth is opposite the right-angle corner,
// which is the vertex not on the longest edge.
[maxvertexcount(4)]
void main(triangle Vertex input[3], inout TriangleStream<Vertex> stream)
{
    float2 p0 = input[0].position.xy / input[0].position.w;
    float2 p1 = input[1].position.xy / input[1].position.w;
    float2 p2 = input[2].position.xy / input[2].position.w;
    float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
    uint c = 2, a = 0, b = 1;
    if (d12 >= d01 && d12 >= d20) { c = 0; a = 1; b = 2; }
    else if (d20 >= d01) { c = 1; a = 2; b = 0; }
    Vertex d;
    d.position = input[a].position + input[b].position - input[c].position;
    [unroll] for (int i = 0; i < 16; i++)
        d.interpolators[i] = input[a].interpolators[i] + input[b].interpolators[i] - input[c].interpolators[i];
    stream.Append(input[c]);
    stream.Append(input[a]);
    stream.Append(input[b]);
    stream.Append(d);
}
)";

        const char* const kPresentShader = R"(
Texture2D frontBuffer : register(t0);
SamplerState linearClamp : register(s0);

struct Vertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex vs_main(uint id : SV_VertexID)
{
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return v;
}

float4 ps_main(Vertex v) : SV_Target
{
    return float4(frontBuffer.Sample(linearClamp, v.uv).rgb, 1.0);
}
)";

        // Depth resolves: copies the depth plane (24-bit unorm) of the depth buffer into an
        // R32_FLOAT texture, so titles can sample resolved depth (k_24_8 fetches).
        const char* const kDepthCopyShader = R"(
Texture2D<float> depthTexture : register(t0);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float ps_main(float4 position : SV_Position) : SV_Target
{
    return depthTexture.Load(int3(position.xy, 0));
}
)";

        // Same copy from a multisampled depth buffer: the resolve takes sample 0.
        const char* const kDepthCopyMsShader = R"(
Texture2DMS<float> depthTexture : register(t0);

float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float ps_main(float4 position : SV_Position) : SV_Target
{
    return depthTexture.Load(int2(position.xy), 0);
}
)";

        // A shader compile running on the compiler pool; `code` is valid once `done` is set.
        struct PendingCompile
        {
            std::atomic<bool> done{ false };
            ComPtr<ID3DBlob> code;
        };

        struct CompiledShader
        {
            bool ok = false;
            TranslatedShader info;
            ComPtr<ID3DBlob> code;
            std::shared_ptr<PendingCompile> pending;  // compiling in the background
            uint64_t cacheKey = 0;                    // hash of the HLSL (disk cache file name)
        };

        // Background shader compilation: D3DCompile takes 0.1-6 s per translated shader, so new
        // shaders compile on worker threads while the title keeps running (draws needing them are
        // skipped until they are ready). HC09_SYNC_SHADERS=1 compiles on first use instead.
        class ShaderCompilerPool
        {
        public:
            ShaderCompilerPool()
            {
                const unsigned workers = std::max(2u, std::thread::hardware_concurrency() / 2);
                for (unsigned i = 0; i < workers; i++)
                    std::thread([this] { Work(); }).detach();
            }

            void Submit(std::string source, const char* target, std::string name, std::shared_ptr<PendingCompile> result)
            {
                std::lock_guard lock(mutex_);
                jobs_.push_back({ std::move(source), target, std::move(name), std::move(result) });
                ready_.notify_one();
            }

        private:
            struct Job
            {
                std::string source;
                const char* target;
                std::string name;
                std::shared_ptr<PendingCompile> result;
            };

            void Work()
            {
                for (;;)
                {
                    Job job;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, [this] { return !jobs_.empty(); });
                        job = std::move(jobs_.front());
                        jobs_.pop_front();
                    }
                    ComPtr<ID3DBlob> code, errors;
                    if (FAILED(D3DCompile(job.source.data(), job.source.size(), job.name.c_str(), nullptr, nullptr, "main", job.target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors)))
                    {
                        std::fprintf(stderr, "[gpu] %s compile failed:\n%s\n", job.name.c_str(),
                            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                        code.Reset();
                    }
                    job.result->code = code;
                    job.result->done = true;
                }
            }

            std::mutex mutex_;
            std::condition_variable ready_;
            std::deque<Job> jobs_;
        };

        // Compiled shaders persist in build/shadercache/<HLSL hash>.<vs|ps> across sessions.
        std::string ShaderCachePath(uint64_t key, bool pixel)
        {
            char path[96];
            std::snprintf(path, sizeof(path), "build/shadercache/%016llX.%s", (unsigned long long)key, pixel ? "ps" : "vs");
            return path;
        }

        ComPtr<ID3DBlob> LoadCachedShader(uint64_t key, bool pixel)
        {
            FILE* file = std::fopen(ShaderCachePath(key, pixel).c_str(), "rb");
            if (!file)
                return nullptr;
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            ComPtr<ID3DBlob> blob;
            if (size > 0 && SUCCEEDED(D3DCreateBlob(SIZE_T(size), &blob)) &&
                std::fread(blob->GetBufferPointer(), 1, size_t(size), file) != size_t(size))
                blob.Reset();
            std::fclose(file);
            return blob;
        }

        void StoreCachedShader(uint64_t key, bool pixel, ID3DBlob* code)
        {
            CreateDirectoryA("build\\shadercache", nullptr);
            if (FILE* file = std::fopen(ShaderCachePath(key, pixel).c_str(), "wb"))
            {
                std::fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), file);
                std::fclose(file);
            }
        }

        // HC09_GPU_STATS: time spent per phase of Renderer::Draw, printed every 2 s.
        const bool g_phaseStats = std::getenv("HC09_GPU_STATS") != nullptr;
        constexpr const char* kPhaseNames[] = { "shaders", "indices", "pipeline", "constants", "views", "vertices", "textures", "submit", "descriptors" };
        std::chrono::steady_clock::duration g_phaseTime[9]{};
        std::chrono::steady_clock::time_point g_phaseStart, g_phaseReport = std::chrono::steady_clock::now();
        int g_phase = -1;
        uint64_t g_evictedResolveTargets = 0;  // HC09_GPU_STATS

        void Phase(int next)
        {
            if (!g_phaseStats)
                return;
            const auto now = std::chrono::steady_clock::now();
            if (g_phase >= 0)
                g_phaseTime[g_phase] += now - g_phaseStart;
            g_phase = next;
            g_phaseStart = now;
        }

        void ReportPhases()
        {
            if (!g_phaseStats)
                return;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - g_phaseReport).count();
            if (seconds < 2.0)
                return;
            std::fprintf(stderr, "[gpu] draw phases (%% of wall time):");
            for (int i = 0; i < 9; i++)
            {
                std::fprintf(stderr, " %s %.1f", kPhaseNames[i], 100.0 * std::chrono::duration<double>(g_phaseTime[i]).count() / seconds);
                g_phaseTime[i] = {};
            }
            std::fprintf(stderr, " | textures: %llu lookups, %llu checks, %llu uploads, %.1f MB hashed%c",
                (unsigned long long)g_textureLookups.exchange(0), (unsigned long long)g_textureChecks.exchange(0), (unsigned long long)g_textureUploads.exchange(0),
                g_textureBytesHashed.exchange(0) / 1048576.0, 10);
            g_phaseReport = now;
        }

        struct RenderTarget
        {
            ComPtr<ID3D12Resource> resource;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            uint32_t samples = 1;
        };

        // Depth/stencil buffer for one EDRAM depth base (D24S8 and D24FS8 both map to D24S8).
        struct DepthTarget
        {
            ComPtr<ID3D12Resource> resource;  // R24G8_TYPELESS: D24S8 depth view, R24_UNORM_X8 shader view
            D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            uint32_t samples = 1;
        };

        struct ResolveTarget
        {
            ComPtr<ID3D12Resource> resource;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            uint32_t width = 0;
            uint32_t height = 0;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv{};  // depth resolve targets only
            int lastResolved = -1;              // frame of the latest resolve into it
        };

        class Renderer
        {
        public:
            bool Initialize()
            {
                if (!context_.Initialize(kTargetWidth, kTargetHeight))
                    return false;
                textures_ = std::make_unique<TextureCache>(context_);
                return CreateRootSignatures() && CreatePresentPipeline() && CreateNullViews() && CreateDepthCopyPipeline() &&
                    (rectangleGs_ = CompileHlsl(kRectangleGs, "gs_5_1", "rectangle_gs")) != nullptr;
            }

            void Draw(const DrawInfo& draw);
            void Swap(uint32_t frontBuffer);

        private:
            bool CreateRootSignatures();
            bool CreatePresentPipeline();
            const CompiledShader& GetShader(const std::shared_ptr<const ShaderMicrocode>& microcode);
            RenderTarget& GetRenderTarget(uint32_t colorInfo, uint32_t samples = 1);
            DepthTarget& GetDepthTarget(uint32_t depthInfo, uint32_t samples = 1);
            // Samples for draws to a surface: multisampled surfaces used by predicated tiling (the
            // 3D scenes) get real MSAA targets; everything else stays single-sampled.
            uint32_t SampleCount(uint32_t surfaceInfo) const;
            std::set<uint32_t> tiledSurfaces_;  // RB_SURFACE_INFO values seen while tiling
            ID3D12PipelineState* GetPipeline(const CompiledShader& vs, const CompiledShader& ps, DXGI_FORMAT format,
                D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType, bool rectangles, bool colorWrites, uint32_t samples);
            void Resolve();
            void ResolveDepth(uint32_t control);
            bool CreateDepthCopyPipeline();
            bool CreateDepthCopyPipeline(const char* source, ComPtr<ID3D12PipelineState>& pipeline);
            D3D12_BOX ResolveRectangle(uint32_t destWidth, uint32_t destHeight);
            void SaveScreenshot(const char* path);
            void SaveTexture(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, DXGI_FORMAT format,
                uint32_t width, uint32_t height, const char* path);
            void SaveStencil(DepthTarget& target, const char* path);
            void SaveDepth(DepthTarget& target, const char* path);
            void DumpResolves();
            ComPtr<ID3D12Resource> CreateReadbackBuffer(uint64_t size);
            bool PrepareIndices(const DrawInfo& draw, std::vector<uint32_t>& indices, bool& indexed,
                D3D12_PRIMITIVE_TOPOLOGY& topology, D3D12_PRIMITIVE_TOPOLOGY_TYPE& type, bool& rectangles);

            D3D12Context context_;
            std::unique_ptr<TextureCache> textures_;
            ComPtr<ID3D12RootSignature> rootSignature_;
            ComPtr<ID3D12RootSignature> presentRootSignature_;
            ComPtr<ID3D12PipelineState> presentPipeline_;
            ComPtr<ID3D12PipelineState> depthCopyPipeline_;
            ComPtr<ID3D12PipelineState> depthCopyMsPipeline_;  // reads sample 0 of a multisampled depth buffer
            ComPtr<ID3DBlob> rectangleGs_;
            std::unordered_map<uint64_t, CompiledShader> shaders_;
            std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines_;
            std::unordered_map<uint32_t, RenderTarget> renderTargets_;
            std::unordered_map<uint32_t, DepthTarget> depthTargets_;
            // Vertex data already converted into the current upload ring, by guest address and
            // endian mode: draws sharing one vertex buffer reuse a single conversion.
            struct VertexUpload
            {
                UploadAllocation allocation;
                uint32_t dwords = 0;
            };
            std::unordered_map<uint64_t, VertexUpload> vertexUploads_;
            // Sampler tables already written this upload generation, by the hash of their contents
            // (the sampler heap is small; allocating one table per draw forced GPU flushes).
            std::unordered_map<uint64_t, DescriptorRange> samplerTables_;
            // CPU-only templates copied into each draw's view table: kViewTableSize null views
            // (raw buffers, then 2D textures) followed by one null cube view.
            ComPtr<ID3D12DescriptorHeap> nullViews_;
            // The float constants as last uploaded (raw guest bytes) and where they went.
            uint32_t lastConstants_[2048]{};
            UploadAllocation lastConstantsUpload_{};
            uint64_t lastConstantsGeneration_ = ~0ull;
            bool CreateNullViews();
            uint64_t samplerTableGeneration_ = ~0ull;
            uint64_t vertexUploadGeneration_ = ~0ull;
            std::unordered_map<uint32_t, ResolveTarget> resolveTargets_;
            std::vector<std::pair<int, ComPtr<ID3D12Resource>>> retiredTargets_;  // (frame released, resource)
            // Releases resolve targets neither resolved to nor sampled for a while, and stale textures.
            void EvictIdleResources(uint32_t frontBuffer);
            uint32_t skippedDraws_ = 0;
        };

        bool Renderer::CreateRootSignatures()
        {
            D3D12_DESCRIPTOR_RANGE views{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kViewTableSize, 0, 0, 0 };
            D3D12_DESCRIPTOR_RANGE samplers{ D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 32, 0, 0, 0 };
            D3D12_ROOT_PARAMETER parameters[4]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[0].Descriptor.ShaderRegister = 0;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[1].Descriptor.ShaderRegister = 1;
            parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[2].DescriptorTable = { 1, &views };
            parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[3].DescriptorTable = { 1, &samplers };
            D3D12_ROOT_SIGNATURE_DESC desc{ 4, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };

            auto create = [&](const D3D12_ROOT_SIGNATURE_DESC& d, ComPtr<ID3D12RootSignature>& out) {
                ComPtr<ID3DBlob> blob, errors;
                if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
                {
                    std::fprintf(stderr, "[gpu] root signature: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                    return false;
                }
                return SUCCEEDED(context_.device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)));
            };
            if (!create(desc, rootSignature_))
                return false;

            D3D12_DESCRIPTOR_RANGE frontBuffer{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            D3D12_ROOT_PARAMETER presentParameter{};
            presentParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            presentParameter.DescriptorTable = { 1, &frontBuffer };
            presentParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC presentDesc{ 1, &presentParameter, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
            return create(presentDesc, presentRootSignature_);
        }

        bool Renderer::CreatePresentPipeline()
        {
            ComPtr<ID3DBlob> vs, ps, errors;
            if (FAILED(D3DCompile(kPresentShader, std::strlen(kPresentShader), "present", nullptr, nullptr, "vs_main", "vs_5_1", 0, 0, &vs, &errors)) ||
                FAILED(D3DCompile(kPresentShader, std::strlen(kPresentShader), "present", nullptr, nullptr, "ps_main", "ps_5_1", 0, 0, &ps, &errors)))
                return false;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = presentRootSignature_.Get();
            desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            return SUCCEEDED(context_.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&presentPipeline_)));
        }

        bool Renderer::CreateNullViews()
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewTableSize + 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
            if (FAILED(context_.device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&nullViews_))))
                return false;
            const uint32_t viewSize = context_.viewDescriptorSize();
            const D3D12_CPU_DESCRIPTOR_HANDLE start = nullViews_->GetCPUDescriptorHandleForHeapStart();
            for (uint32_t slot = 0; slot <= kViewTableSize + 1; slot++)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
                nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (slot < kMaxVertexStreams)
                {
                    nullDesc.Format = DXGI_FORMAT_R32_TYPELESS;
                    nullDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    nullDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                    nullDesc.Buffer.NumElements = 1;
                }
                else if (slot == kViewTableSize)
                {
                    nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    nullDesc.TextureCube.MipLevels = 1;
                }
                else if (slot == kViewTableSize + 1)
                {
                    nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    nullDesc.Texture3D.MipLevels = 1;
                }
                else
                {
                    nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    nullDesc.Texture2D.MipLevels = 1;
                }
                context_.device()->CreateShaderResourceView(nullptr, &nullDesc, { start.ptr + uint64_t(slot) * viewSize });
            }
            return true;
        }

        const CompiledShader& Renderer::GetShader(const std::shared_ptr<const ShaderMicrocode>& microcode)
        {
            auto& compiled = shaders_[microcode->hash];
            const bool pixel = microcode->type == ShaderType::Pixel;
            // A background compile finished: use it and keep it for later sessions.
            if (compiled.pending && compiled.pending->done)
            {
                compiled.code = compiled.pending->code;
                compiled.ok = compiled.code != nullptr;
                if (compiled.ok)
                    StoreCachedShader(compiled.cacheKey, pixel, compiled.code.Get());
                else
                    compiled.info.error = "compile failed";
                compiled.pending.reset();
            }
            if (compiled.info.hlsl.empty() && compiled.info.error.empty())
            {
                compiled.info = TranslateShader(*microcode);
                char name[64];
                std::snprintf(name, sizeof(name), "%016llX.%s", (unsigned long long)microcode->hash, pixel ? "ps" : "vs");
                if (!compiled.info.valid)
                {
                    std::fprintf(stderr, "[gpu] shader %s not translated: %s\n", name, compiled.info.error.c_str());
                    if (compiled.info.error.empty())
                        compiled.info.error = "invalid";
                    return compiled;
                }
                // The cache key covers the translated source, so translator changes invalidate it.
                compiled.cacheKey = XXH3_64bits(compiled.info.hlsl.data(), compiled.info.hlsl.size()) ^ (pixel ? 1 : 0);
                compiled.code = LoadCachedShader(compiled.cacheKey, pixel);
                if (compiled.code)
                {
                    compiled.ok = true;
                    return compiled;
                }
                static const bool synchronous = std::getenv("HC09_SYNC_SHADERS") != nullptr;
                if (synchronous)
                {
                    const auto compileStart = std::chrono::steady_clock::now();
                    compiled.code = CompileHlsl(compiled.info.hlsl, pixel ? "ps_5_1" : "vs_5_1", name);
                    compiled.ok = compiled.code != nullptr;
                    const double compileMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compileStart).count();
                    g_shaderCompileMs += compileMs;
                    if (std::getenv("HC09_GPU_STATS"))
                        std::fprintf(stderr, "[gpu] frame %d: compiled shader %s in %.0f ms\n", g_frame, name, compileMs);
                    if (compiled.ok)
                        StoreCachedShader(compiled.cacheKey, pixel, compiled.code.Get());
                    else
                        compiled.info.error = "compile failed";
                }
                else
                {
                    static ShaderCompilerPool pool;
                    compiled.pending = std::make_shared<PendingCompile>();
                    pool.Submit(compiled.info.hlsl, pixel ? "ps_5_1" : "vs_5_1", name, compiled.pending);
                }
            }
            return compiled;
        }

        uint32_t Renderer::SampleCount(uint32_t surfaceInfo) const
        {
            static const bool disabled = std::getenv("HC09_NO_MSAA") != nullptr;
            const uint32_t msaa = (surfaceInfo >> 16) & 3;  // 0 1x, 1 2x, 2 4x
            if (disabled || msaa == 0 || !tiledSurfaces_.contains(surfaceInfo))
                return 1;
            return msaa == 1 ? 2 : 4;
        }

        RenderTarget& Renderer::GetRenderTarget(uint32_t colorInfo, uint32_t samples)
        {
            const uint32_t key = (colorInfo & 0xF0FFF) | (samples << 24);  // EDRAM base + format + samples
            RenderTarget& target = renderTargets_[key];
            if (!target.resource)
            {
                target.samples = samples;
                target.format = ColorFormat((colorInfo >> 16) & 0xF);
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = kTargetWidth * g_scale;
                desc.Height = kTargetHeight * g_scale;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = target.format;
                desc.SampleDesc.Count = samples;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_CLEAR_VALUE clear{ target.format, { 0, 0, 0, 0 } };
                context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target.resource));
                target.rtv = context_.AllocateRtv();
                context_.device()->CreateRenderTargetView(target.resource.Get(), nullptr, target.rtv);
                const float black[4] = { 0, 0, 0, 0 };
                context_.list()->ClearRenderTargetView(target.rtv, black, 0, nullptr);
                target.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                std::fprintf(stderr, "[gpu] render target: EDRAM base 0x%X format %u, %u samples\n", colorInfo & 0xFFF,
                    (colorInfo >> 16) & 0xF, samples);
            }
            return target;
        }

        DepthTarget& Renderer::GetDepthTarget(uint32_t depthInfo, uint32_t samples)
        {
            DepthTarget& target = depthTargets_[(depthInfo & 0xFFF) | (samples << 24)];
            if (!target.resource)
            {
                target.samples = samples;
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = kTargetWidth * g_scale;
                desc.Height = kTargetHeight * g_scale;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
                desc.SampleDesc.Count = samples;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                D3D12_CLEAR_VALUE clear{};
                clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                clear.DepthStencil = { 1.0f, 0 };
                context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&target.resource));
                target.dsv = context_.AllocateDsv();
                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
                dsvDesc.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
                context_.device()->CreateDepthStencilView(target.resource.Get(), &dsvDesc, target.dsv);
                context_.list()->ClearDepthStencilView(target.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
            }
            return target;
        }

        ID3D12PipelineState* Renderer::GetPipeline(const CompiledShader& vs, const CompiledShader& ps, DXGI_FORMAT format,
            D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType, bool rectangles, bool colorWrites, uint32_t samples)
        {
            const uint32_t depthControl = ReadRegister(reg::kDepthControl);
            const uint32_t stencilMasks = ReadRegister(reg::kStencilRefMask) & 0xFFFF00;
            const uint32_t cull = ReadRegister(reg::kScModeControl) & 7;
            const uint32_t blend = ReadRegister(reg::kBlendControl0);
            const uint32_t writeMask = colorWrites ? ReadRegister(reg::kColorMask) & 0xF : 0;
            uint64_t key = Mix(0, reinterpret_cast<uintptr_t>(&vs));
            key = Mix(key, (uint64_t(depthControl) << 32) | (uint64_t(stencilMasks) << 3) | cull);
            key = Mix(key, reinterpret_cast<uintptr_t>(&ps));
            key = Mix(key, (uint64_t(format) << 40) | (uint64_t(topologyType) << 32) | blend);
            // PA_CL_CLIP_CNTL bit 16: clip_disable (no clipping, so no depth clipping either).
            const bool clipDisabled = (ReadRegister(0x2204) >> 16) & 1;
            key = Mix(key, (uint64_t(clipDisabled) << 24) | (uint64_t(samples) << 16) | (uint64_t(rectangles) << 8) | writeMask);
            auto& pipeline = pipelines_[key];
            if (pipeline)
                return pipeline.Get();

            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = rootSignature_.Get();
            desc.VS = { vs.code->GetBufferPointer(), vs.code->GetBufferSize() };
            desc.PS = { ps.code->GetBufferPointer(), ps.code->GetBufferSize() };
            if (rectangles)
                desc.GS = { rectangleGs_->GetBufferPointer(), rectangleGs_->GetBufferSize() };

            auto& target = desc.BlendState.RenderTarget[0];
            const uint32_t srcColor = blend & 0x1F, colorOp = (blend >> 5) & 7, dstColor = (blend >> 8) & 0x1F;
            const uint32_t srcAlpha = (blend >> 16) & 0x1F, alphaOp = (blend >> 21) & 7, dstAlpha = (blend >> 24) & 0x1F;
            target.BlendEnable = !(srcColor == 1 && dstColor == 0 && colorOp == 0 && srcAlpha == 1 && dstAlpha == 0 && alphaOp == 0);
            target.SrcBlend = BlendFactor(srcColor, false);
            target.DestBlend = BlendFactor(dstColor, false);
            target.BlendOp = BlendOp(colorOp);
            target.SrcBlendAlpha = BlendFactor(srcAlpha, true);
            target.DestBlendAlpha = BlendFactor(dstAlpha, true);
            target.BlendOpAlpha = BlendOp(alphaOp);
            target.RenderTargetWriteMask = UINT8(writeMask);
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            // PA_SU_SC_MODE_CNTL: cull_front (bit 0), cull_back (bit 1), face (bit 2: 1 = CW is front).
            // The Y flip into D3D's viewport reverses winding, so D3D's "front" is Xenos' back.
            desc.RasterizerState.CullMode = (cull & 3) == 1 ? D3D12_CULL_MODE_FRONT
                : (cull & 3) == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            desc.RasterizerState.FrontCounterClockwise = (cull & 4) ? FALSE : TRUE;
            desc.RasterizerState.DepthClipEnable = clipDisabled ? FALSE : TRUE;
            // RB_DEPTHCONTROL: stencil enable (0), z enable (1), z write (2), z func (4-6),
            // backface stencil enable (7), front stencil func/fail/zpass/zfail (8-19), back (20-31).
            // Xenos compare functions and stencil ops are D3D12's enums minus one.
            auto& ds = desc.DepthStencilState;
            ds.DepthEnable = (depthControl >> 1) & 1;
            static const bool noDepthTest = std::getenv("HC09_NO_DEPTH_TEST") != nullptr;  // debugging
            if (noDepthTest)
                ds.DepthEnable = FALSE;
            ds.DepthWriteMask = ((depthControl >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            ds.DepthFunc = D3D12_COMPARISON_FUNC(((depthControl >> 4) & 7) + 1);
            ds.StencilEnable = depthControl & 1;
            ds.StencilReadMask = UINT8(stencilMasks >> 8);
            ds.StencilWriteMask = UINT8(stencilMasks >> 16);
            auto face = [&](uint32_t shift) {
                D3D12_DEPTH_STENCILOP_DESC op{};
                op.StencilFunc = D3D12_COMPARISON_FUNC(((depthControl >> shift) & 7) + 1);
                op.StencilFailOp = D3D12_STENCIL_OP(((depthControl >> (shift + 3)) & 7) + 1);
                op.StencilPassOp = D3D12_STENCIL_OP(((depthControl >> (shift + 6)) & 7) + 1);
                op.StencilDepthFailOp = D3D12_STENCIL_OP(((depthControl >> (shift + 9)) & 7) + 1);
                return op;
            };
            ds.FrontFace = face(8);
            ds.BackFace = ((depthControl >> 7) & 1) ? face(20) : ds.FrontFace;
            desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
            desc.PrimitiveTopologyType = topologyType;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = format;
            desc.SampleDesc.Count = samples;
            if (FAILED(context_.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
            {
                std::fprintf(stderr, "[gpu] pipeline creation failed\n");
                return nullptr;
            }
            return pipeline.Get();
        }

        bool Renderer::PrepareIndices(const DrawInfo& draw, std::vector<uint32_t>& indices, bool& indexed,
            D3D12_PRIMITIVE_TOPOLOGY& topology, D3D12_PRIMITIVE_TOPOLOGY_TYPE& type, bool& rectangles)
        {
            rectangles = false;
            bool convert = false;
            switch (draw.primitive)
            {
            case PrimitiveType::PointList: topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
            case PrimitiveType::LineList: topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
            case PrimitiveType::LineStrip: topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
            case PrimitiveType::LineLoop: topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; convert = true; break;
            case PrimitiveType::TriangleList: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
            case PrimitiveType::TriangleStrip: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
            case PrimitiveType::TriangleFan:
            case PrimitiveType::Polygon:
            case PrimitiveType::QuadList:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                convert = true;
                break;
            case PrimitiveType::RectangleList:
                topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
                type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                rectangles = true;
                break;
            default:
                return false;
            }

            // HC09_FORCE_INDEXED=1 (debugging): auto-indexed draws use generated index buffers too.
            static const bool forceIndexed = std::getenv("HC09_FORCE_INDEXED") != nullptr;
            indexed = draw.source != IndexSource::Auto || convert || forceIndexed;
            if (!indexed)
                return true;

            // Source indices (as 32-bit; 16-bit restart values widen to 0xFFFFFFFF).
            std::vector<uint32_t> source(draw.indexCount);
            for (uint32_t i = 0; i < draw.indexCount; i++)
            {
                uint32_t value = i;
                if (draw.source == IndexSource::Dma)
                {
                    if (draw.index32)
                    {
                        uint32_t raw;
                        std::memcpy(&raw, GuestToHost(draw.indexAddress + i * 4), 4);
                        value = GpuSwap(raw, draw.indexEndian);
                    }
                    else
                    {
                        uint32_t raw;
                        std::memcpy(&raw, GuestToHost(draw.indexAddress + (i & ~1u) * 2), 4);
                        raw = GpuSwap(raw, draw.indexEndian);
                        value = (i & 1) ? raw >> 16 : raw & 0xFFFF;
                        if (value == 0xFFFF)
                            value = 0xFFFFFFFF;
                    }
                }
                else if (draw.source == IndexSource::Immediate)
                {
                    const uint32_t word = i / 2 < draw.immediateDwords ? draw.immediateIndices[i / 2] : 0;
                    value = (i & 1) ? word >> 16 : word & 0xFFFF;
                }
                source[i] = value;
            }

            switch (draw.primitive)
            {
            case PrimitiveType::TriangleFan:
            case PrimitiveType::Polygon:
                for (uint32_t i = 1; i + 1 < source.size(); i++)
                    indices.insert(indices.end(), { source[0], source[i], source[i + 1] });
                break;
            case PrimitiveType::QuadList:
                for (uint32_t i = 0; i + 3 < source.size(); i += 4)
                    indices.insert(indices.end(), { source[i], source[i + 1], source[i + 2], source[i], source[i + 2], source[i + 3] });
                break;
            case PrimitiveType::LineLoop:
                indices = source;
                if (!source.empty())
                    indices.push_back(source[0]);
                break;
            default:
                indices = std::move(source);
                break;
            }
            return true;
        }

        void Renderer::Draw(const DrawInfo& draw)
        {
            g_phase = -1;
            Phase(0);
            const uint32_t mode = ReadRegister(reg::kModeControl) & 7;
            if (mode == kModeCopy)
            {
                Resolve();
                return;
            }
            // Mode 5 renders depth/stencil only. Colour-masked draws still matter when they
            // write depth or stencil.
            const uint32_t depthControl = ReadRegister(reg::kDepthControl);
            const bool writesDepthStencil = ((depthControl >> 1) & 1 && (depthControl >> 2) & 1) || (depthControl & 1);
            const bool colorWrites = mode == kModeColorDepth && (ReadRegister(reg::kColorMask) & 0xF) != 0;
            if ((mode != kModeColorDepth && mode != kModeDepth) || draw.indexCount == 0 || (!colorWrites && !writesDepthStencil))
                return;

            const auto vsCode = ActiveShader(ShaderType::Vertex);
            const auto psCode = ActiveShader(ShaderType::Pixel);
            if (!vsCode || !psCode)
                return;
            // Shaders still compiling in the background: draws of the tiled 3D scene (redrawn every
            // frame) are skipped meanwhile, but anything else may render content only once (the
            // field and jersey textures built while loading, the interface), so wait for those.
            if (!TilingActive())
            {
                for (const auto* code : { &vsCode, &psCode })
                {
                    const CompiledShader& waiting = GetShader(*code);
                    if (waiting.pending)
                    {
                        while (!waiting.pending->done)
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }
            const CompiledShader& vs = GetShader(vsCode);
            const CompiledShader& ps = GetShader(psCode);
            if (!vs.ok || !ps.ok)
            {
                skippedDraws_++;
                return;
            }

            std::vector<uint32_t> indices;
            bool indexed = false, rectangles = false;
            D3D12_PRIMITIVE_TOPOLOGY topology;
            D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType;
            Phase(1);
            if (!PrepareIndices(draw, indices, indexed, topology, topologyType, rectangles))
            {
                skippedDraws_++;
                return;
            }

            if (TilingActive())
                tiledSurfaces_.insert(ReadRegister(reg::kSurfaceInfo));
            const uint32_t samples = SampleCount(ReadRegister(reg::kSurfaceInfo));
            RenderTarget& target = GetRenderTarget(ReadRegister(reg::kColorInfo), samples);
            DepthTarget& depth = GetDepthTarget(ReadRegister(reg::kDepthInfo), samples);
            Phase(2);
            ID3D12PipelineState* pipeline = GetPipeline(vs, ps, target.format, topologyType, rectangles, colorWrites, samples);
            if (!pipeline)
                return;
            auto* list = context_.list();
            auto* device = context_.device();
            Transition(list, target.resource.Get(), target.state, D3D12_RESOURCE_STATE_RENDER_TARGET);

            // Constants: 512 float4 shared by both stages, plus per-draw state.
            // Per-draw transient allocations. If one of them recycles the upload ring or descriptor
            // heaps (a flush), the earlier ones are invalid: set everything up again once.
            int setupAttempts = 0;
        retrySetup:
            const uint64_t setupGeneration = context_.uploadGeneration();
            Phase(3);
            // Float constants: 2048 big-endian registers, re-uploaded only when they changed.
            const uint32_t* constants = reinterpret_cast<const uint32_t*>(GuestToHost(kRegisterBase + reg::kFloatConstants * 4));
            UploadAllocation floats;
            if (lastConstantsGeneration_ == context_.uploadGeneration() && std::memcmp(constants, lastConstants_, sizeof(lastConstants_)) == 0)
            {
                floats = lastConstantsUpload_;
            }
            else
            {
                floats = context_.Upload(512 * 16);
                std::memcpy(lastConstants_, constants, sizeof(lastConstants_));
                auto* dest = static_cast<uint32_t*>(floats.cpu);
                for (uint32_t i = 0; i < 2048; i++)
                    dest[i] = __builtin_bswap32(lastConstants_[i]);
                lastConstantsUpload_ = floats;
                lastConstantsGeneration_ = context_.uploadGeneration();
            }

            DrawState state{};
            state.vsConstBase = ReadRegister(reg::kVsConst) & 0x1FF;
            state.psConstBase = ReadRegister(reg::kPsConst) & 0x1FF;
            const uint32_t vte = ReadRegister(reg::kVteControl);
            state.wIsReciprocal = ((vte >> 10) & 1) == 0;  // vtx_w0_fmt set: W is plain, the hardware takes 1/W
            state.xyPreDivided = (vte >> 8) & 1;
            float scale[3], offset[3];
            for (int axis = 0; axis < 3; axis++)
            {
                scale[axis] = (vte & (1u << (axis * 2))) ? RegisterFloat(reg::kViewportXScale + axis * 2) : 1.0f;
                offset[axis] = (vte & (2u << (axis * 2))) ? RegisterFloat(reg::kViewportXScale + axis * 2 + 1) : 0.0f;
            }
            // The window offset (used for predicated tiling) is deliberately ignored: targets are
            // kept in screen space and resolves copy screen-space rectangles.
            //
            // Surfaces used by predicated tiling hold one tile in EDRAM, so the title clears them
            // with tile-sized rectangles outside the tiled passes. Here they cover the whole
            // screen: stretch such rectangles to the full target width.
            {
                const uint32_t surfaceInfo = ReadRegister(reg::kSurfaceInfo);
                const uint32_t pitch = surfaceInfo & 0x3FFF;
                const uint32_t msaa = (surfaceInfo >> 16) & 3;  // 0 1x, 1 2x (1x2 samples), 2 4x (2x2 samples)
                // Widest single-sampled surface drawn at each EDRAM colour base: a multisampled
                // surface at the same base only aliases it if that one is (at least) twice as wide.
                static std::unordered_map<uint32_t, uint32_t> singleSampledPitch;
                const uint32_t colorBase = ReadRegister(reg::kColorInfo) & 0xFFF;
                if (msaa == 0)
                    singleSampledPitch[colorBase] = std::max(singleSampledPitch[colorBase], pitch);
                const auto aliased = singleSampledPitch.find(colorBase);
                const bool aliasesSingleSampled = aliased != singleSampledPitch.end() &&
                    aliased->second >= (msaa == 2 ? pitch * 2 : pitch);
                if (TilingActive())
                {
                }
                else if (rectangles && pitch > 0 && pitch < kTargetWidth && tiledSurfaces_.contains(surfaceInfo))
                {
                    const float stretch = float(kTargetWidth) / float(pitch);
                    scale[0] *= stretch;
                    offset[0] *= stretch;
                }
                else if (rectangles && msaa != 0 && pitch > 0 && pitch < kTargetWidth && aliasesSingleSampled)
                {
                    // Fast clears through EDRAM aliasing: the title clears a single-sampled surface
                    // by drawing a rectangle into a multisampled one at the same address, where each
                    // pixel covers 2x2 (4x) or 1x2 (2x) single-sampled pixels. Targets here are
                    // single-sampled, so cover the same area. A genuinely multisampled surface (one
                    // the title also draws normal geometry to, in its own coordinates) is not
                    // stretched.
                    if (msaa == 2)
                    {
                        scale[0] *= 2.0f;
                        offset[0] *= 2.0f;
                    }
                    scale[1] *= 2.0f;
                    offset[1] *= 2.0f;
                }
            }
            state.ndcScale[0] = scale[0] * 2.0f / kTargetWidth;
            state.ndcOffset[0] = offset[0] * 2.0f / kTargetWidth - 1.0f;
            state.ndcScale[1] = -scale[1] * 2.0f / kTargetHeight;
            state.ndcOffset[1] = 1.0f - offset[1] * 2.0f / kTargetHeight;
            state.ndcScale[2] = scale[2];
            state.ndcOffset[2] = offset[2];
            state.ndcScale[3] = float((vte >> 9) & 1);
            // RB_COLORCONTROL: alpha_func (bits 0-2), alpha_test_enable (bit 3); RB_ALPHA_REF.
            const uint32_t colorControl = ReadRegister(0x2202);
            state.alphaFunc = (colorControl >> 3) & 1 ? colorControl & 7 : 7;
            state.alphaRef = RegisterFloat(0x210E);
            state.vertexIndexOffset = ReadRegister(reg::kIndexOffset);
            if (g_frame == g_dumpFrame && state.alphaFunc != 7)
            {
                static std::set<std::pair<uint32_t, uint32_t>> seen;
                if (seen.insert({ colorControl, ReadRegister(0x210E) }).second)
                    std::fprintf(stderr, "[frame] alpha test: color control 0x%08X func %u ref %g (draw %d)\n", colorControl,
                        state.alphaFunc, state.alphaRef, g_drawInFrame);
            }
            for (uint32_t i = 0; i < 8; i++)
                state.boolConstants[i] = ReadRegister(reg::kBoolConstants + i);
            for (uint32_t i = 0; i < 32; i++)
                state.loopConstants[i] = ReadRegister(reg::kLoopConstants + i);

            // Views: vertex streams t0..t15, textures t16..t47.
            const uint32_t cubeMask = vs.info.textureCubeMask | ps.info.textureCubeMask;
            const uint32_t volumeMask = vs.info.textureVolumeMask | ps.info.textureVolumeMask;
            Phase(4);
            const DescriptorRange views = context_.AllocateViews(kViewTableSize);
            const uint32_t viewSize = context_.viewDescriptorSize();
            {
                const D3D12_CPU_DESCRIPTOR_HANDLE nullStart = nullViews_->GetCPUDescriptorHandleForHeapStart();
                device->CopyDescriptorsSimple(kViewTableSize, views.cpu, nullStart, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                for (uint32_t n = 0; n < 32; n++)
                {
                    if ((cubeMask >> n) & 1)
                        device->CopyDescriptorsSimple(1, { views.cpu.ptr + uint64_t(kTextureSlotBase + n) * viewSize },
                            { nullStart.ptr + uint64_t(kViewTableSize) * viewSize }, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    else if ((volumeMask >> n) & 1)
                        device->CopyDescriptorsSimple(1, { views.cpu.ptr + uint64_t(kTextureSlotBase + n) * viewSize },
                            { nullStart.ptr + uint64_t(kViewTableSize + 1) * viewSize }, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                }
            }
            Phase(5);
            // Highest vertex index the draw uses (vertex streams are uploaded only up to it).
            uint32_t maxVertexIndex = draw.indexCount ? draw.indexCount - 1 : 0;
            if (indexed)
            {
                maxVertexIndex = 0;
                for (uint32_t index : indices)
                    if (index != 0xFFFFFFFF)
                        maxVertexIndex = std::max(maxVertexIndex, index);
            }
            const uint32_t baseVertexForUpload = ReadRegister(reg::kIndexOffset);
            if (g_frame == g_dumpFrame)
                std::fprintf(stderr, "[frame]   indices: %s, max vertex index %u, index offset %u\n", indexed ? "indexed" : "auto",
                    maxVertexIndex, baseVertexForUpload);
            for (uint32_t stream = 0; stream < vs.info.vertexFetchConstants.size(); stream++)
            {
                const uint32_t index = vs.info.vertexFetchConstants[stream];
                const uint32_t d0 = ReadRegister(reg::kFetchConstants + index * 2);
                const uint32_t d1 = ReadRegister(reg::kFetchConstants + index * 2 + 1);
                const uint32_t address = PhysicalToGuest(d0 & ~3u);
                // Only the vertices this draw can reach: up to the largest index (or the last
                // vertex of a non-indexed draw), not the whole buffer the fetch constant spans.
                const uint64_t lastVertex = uint64_t(baseVertexForUpload) + maxVertexIndex;
                const uint64_t reach = lastVertex * vs.info.vertexStreamStrides[stream] + vs.info.vertexStreamTails[stream];
                const uint32_t dwords = uint32_t(std::min<uint64_t>({ (d1 >> 2) & 0xFFFFFF, uint64_t(4u << 20), reach }));
                if (dwords == 0)
                    continue;
                // HC09_DUMP_VERTEX_MEMORY=1 (with a frame dump): save each vertex buffer the draw reads,
                // endian-swapped to host order, as build/vbmem_<address>_s<stride dwords>.bin.
                if ((g_frame == g_dumpFrame || (std::getenv("HC09_TRACE_SURFACE") && ReadRegister(reg::kSurfaceInfo) == std::strtoul(std::getenv("HC09_TRACE_SURFACE"), nullptr, 16))) && std::getenv("HC09_DUMP_VERTEX_MEMORY"))
                {
                    static std::set<uint32_t> saved;
                    if (saved.insert(d0 & ~3u).second)
                    {
                        char path[96];
                        std::snprintf(path, sizeof(path), "build/vbmem_%08X_s%u.bin", d0 & ~3u, vs.info.vertexStreamStrides[stream]);
                        if (FILE* file = std::fopen(path, "wb"))
                        {
                            // The whole buffer the fetch constant describes (later draws may reach further).
                            const auto* source = reinterpret_cast<const uint32_t*>(GuestToHost(address));
                            const uint32_t bufferDwords = std::min<uint32_t>((d1 >> 2) & 0xFFFFFF, 4u << 20);
                            for (uint32_t i = 0; i < bufferDwords; i++)
                            {
                                const uint32_t value = GpuSwap(source[i], d1 & 3);
                                std::fwrite(&value, 4, 1, file);
                            }
                            std::fclose(file);
                        }
                    }
                }
                if (vertexUploadGeneration_ != context_.uploadGeneration())
                {
                    vertexUploads_.clear();
                    vertexUploadGeneration_ = context_.uploadGeneration();
                }
                const uint64_t uploadKey = (uint64_t(address) << 2) | (d1 & 3);
                auto cached = vertexUploads_.find(uploadKey);
                UploadAllocation upload;
                if (cached != vertexUploads_.end() && cached->second.dwords >= dwords)
                {
                    upload = cached->second.allocation;
                }
                else
                {
                    upload = context_.Upload(uint64_t(dwords) * 4, 256);
                    if (vertexUploadGeneration_ != context_.uploadGeneration())
                    {
                        // The ring was recycled to make room: everything cached is gone.
                        vertexUploads_.clear();
                        vertexUploadGeneration_ = context_.uploadGeneration();
                    }
                    auto* dest = static_cast<uint32_t*>(upload.cpu);
                    const uint32_t* src = reinterpret_cast<const uint32_t*>(GuestToHost(address));
                    const uint32_t mode = d1 & 3;
                    if (mode == 2)
                    {
                        for (uint32_t i = 0; i < dwords; i++)
                            dest[i] = __builtin_bswap32(src[i]);
                    }
                    else
                    {
                        for (uint32_t i = 0; i < dwords; i++)
                            dest[i] = GpuSwap(src[i], mode);
                    }
                    vertexUploads_[uploadKey] = { upload, dwords };
                }
                D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.Format = DXGI_FORMAT_R32_TYPELESS;
                desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                desc.Buffer.FirstElement = upload.offset / 4;
                desc.Buffer.NumElements = dwords;
                desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                device->CreateShaderResourceView(upload.resource, &desc, { views.cpu.ptr + uint64_t(stream) * viewSize });
            }

            // Textures and samplers for every fetch constant either stage samples.
            Phase(6);
            const uint32_t textureMask = vs.info.textureFetchMask | ps.info.textureFetchMask;
            if (g_frame == g_dumpFrame)
                std::fprintf(stderr, "[frame]   blend 0x%08X color control 0x%08X color mask 0x%08X depth control 0x%08X stencil 0x%08X depth info 0x%08X\n",
                    ReadRegister(reg::kBlendControl0), ReadRegister(0x2202), ReadRegister(reg::kColorMask), ReadRegister(reg::kDepthControl),
                    ReadRegister(reg::kStencilRefMask), ReadRegister(reg::kDepthInfo));
            if (g_frame == g_dumpFrame)
            {
                std::fprintf(stderr, "[frame]   bools");
                for (uint32_t i = 0; i < 8; i++)
                    std::fprintf(stderr, " %08X", ReadRegister(reg::kBoolConstants + i));
                std::fprintf(stderr, "\n");
            }
            uint32_t samplerCount = 0;
            for (uint32_t n = 0; n < 32; n++)
                if (textureMask & (1u << n))
                    samplerCount = n + 1;
            // Unused slots get a valid default so identical tables hash identically.
            D3D12_SAMPLER_DESC samplerDescs[32]{};
            for (uint32_t n = 0; n < 32; n++)
            {
                samplerDescs[n].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                samplerDescs[n].AddressU = samplerDescs[n].AddressV = samplerDescs[n].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                samplerDescs[n].MaxLOD = D3D12_FLOAT32_MAX;
            }
            for (uint32_t n = 0; n < samplerCount; n++)
            {
                if (!(textureMask & (1u << n)))
                    continue;
                const TextureFetch fetch = ReadTextureFetch(n);
                BoundTexture texture;
                const D3D12_SRV_DIMENSION wanted = (cubeMask >> n) & 1 ? D3D12_SRV_DIMENSION_TEXTURECUBE
                    : (volumeMask >> n) & 1 ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
                // HC09_DUMP_TEXTURE_MEMORY=1 (with a frame dump): save each sampled texture's raw guest
                // memory once as build/texmem_<base>_f<format>_<width>x<height>_p<pitch>.bin.
                if (g_frame == g_dumpFrame && std::getenv("HC09_DUMP_TEXTURE_MEMORY"))
                {
                    static std::set<uint32_t> saved;
                    if (saved.insert(fetch.baseAddress()).second)
                    {
                        const bool compressed = fetch.format() == 18 || fetch.format() == 19 || fetch.format() == 20 ||
                            fetch.format() == 49 || fetch.format() == 59;
                        const uint32_t unitBytes = fetch.format() == 18 || fetch.format() == 59 ? 8 : compressed ? 16 : 4;
                        const uint32_t pitchUnits = (std::max(fetch.pitchTexels(), fetch.width()) / (compressed ? 4 : 1) + 31) & ~31u;
                        const uint32_t rows = ((fetch.height() / (compressed ? 4 : 1)) + 31) & ~31u;
                        char path[96];
                        std::snprintf(path, sizeof(path), "build/texmem_%08X_f%u_%ux%u_p%u.bin", fetch.baseAddress(), fetch.format(),
                            fetch.width(), fetch.height(), pitchUnits);
                        if (FILE* file = std::fopen(path, "wb"))
                        {
                            std::fwrite(GuestToHost(PhysicalToGuest(fetch.baseAddress())), 1, size_t(pitchUnits) * rows * unitBytes, file);
                            std::fclose(file);
                        }
                    }
                }
                const bool fetched = textures_->Get(fetch, texture);
                const bool bound = fetched && texture.view.ViewDimension == wanted;
                // The shader samples a cube or 3D texture but the constant holds a 2D one: the
                // hardware samples by the constant, so bind the 2D view in the fallback slot.
                if (fetched && !bound && wanted != D3D12_SRV_DIMENSION_TEXTURE2D &&
                    texture.view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
                {
                    device->CreateShaderResourceView(texture.resource, &texture.view,
                        { views.cpu.ptr + uint64_t(kTextureFallbackSlotBase + n) * viewSize });
                    state.textureSizes[n][0] = float(texture.width);
                    state.textureSizes[n][1] = float(texture.height);
                    state.textureSizes[n][2] = 1.0f;
                    state.textureSizes[n][3] = 2.0f;  // sample the 2D view
                }
                if (g_frame == g_dumpFrame)
                    std::fprintf(stderr, "[frame]   tex %u: base 0x%08X fmt %u %ux%u tiled %d swizzle 0x%03X endian %u dim %u -> %s dxgi %d"
                        " | mips 0x%08X levels %u-%u packed %d filter mag %u min %u mip %u aniso %u bias %d clamp %u/%u\n", n,
                        fetch.baseAddress(), fetch.format(), fetch.width(), fetch.height(), int(fetch.tiled()), fetch.swizzle(),
                        fetch.endian(), fetch.dimension(), bound ? "bound" : "UNBOUND", int(texture.view.Format), fetch.mipAddress(),
                        fetch.mipMinLevel(), fetch.mipMaxLevel(), int(fetch.packedMips()), fetch.magFilter(), fetch.minFilter(),
                        fetch.mipFilter(), fetch.anisoFilter(), fetch.lodBias(), fetch.clampX(), fetch.clampY());
                if (bound)
                {
                    // HC09_DUMP_TEXTURES=<frame>: from that frame on, save each distinct RGBA8 texture sampled (up to 64);
                    // HC09_DUMP_TEXTURES=dump: those sampled in the dump frame.
                    static const char* dumpTexturesSpec = std::getenv("HC09_DUMP_TEXTURES");
                    static const bool dumpTexturesInDumpFrame = dumpTexturesSpec && std::strcmp(dumpTexturesSpec, "dump") == 0;
                    static const int dumpTexturesFrame = dumpTexturesSpec && !dumpTexturesInDumpFrame ? std::atoi(dumpTexturesSpec) : -1;
                    static std::vector<uint32_t> dumpedTextures;
                    const bool dumpTextures = dumpTexturesInDumpFrame ? g_frame == g_dumpFrame : dumpTexturesFrame >= 0 && g_frame >= dumpTexturesFrame;
                    if (dumpTextures && dumpedTextures.size() < 64 && texture.view.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                        std::find(dumpedTextures.begin(), dumpedTextures.end(), fetch.baseAddress()) == dumpedTextures.end())
                    {
                        dumpedTextures.push_back(fetch.baseAddress());
                        char path[64];
                        std::snprintf(path, sizeof(path), "build/texture_%08X.bmp", fetch.baseAddress());
                        D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        SaveTexture(texture.resource, readable, texture.view.Format, texture.width, texture.height, path);
                    }
                    Phase(8);
                    device->CreateShaderResourceView(texture.resource, &texture.view,
                        { views.cpu.ptr + uint64_t(kTextureSlotBase + n) * viewSize });
                    Phase(6);
                    state.textureSizes[n][0] = float(texture.width);
                    state.textureSizes[n][1] = float(texture.height);
                    state.textureSizes[n][2] = float(texture.depth);
                    state.textureSizes[n][3] = texture.stacked ? 1.0f : 0.0f;
                }
                auto address = [](uint32_t clamp) {
                    switch (clamp)
                    {
                    case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                    case 6: case 7: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
                    case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
                    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                    }
                };
                D3D12_SAMPLER_DESC sampler{};
                // Xenos filters: 0 point, 1 linear; mip filter 2 = base map only. Anisotropy n
                // allows up to 2^(n-1):1.
                const bool linear = fetch.minFilter() == 1 || fetch.magFilter() == 1;
                const uint32_t mipFilter = fetch.mipFilter();
                const uint32_t aniso = fetch.anisoFilter();
                if (aniso >= 2 && aniso <= 5)
                {
                    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
                    sampler.MaxAnisotropy = 1u << (aniso - 1);
                }
                else if (linear)
                    sampler.Filter = mipFilter == 0 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                else
                    sampler.Filter = mipFilter == 1 ? D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
                sampler.AddressU = address(fetch.clampX());
                sampler.AddressV = address(fetch.clampY());
                sampler.AddressW = address(fetch.clampZ());
                // Border colour for the clamp-to-border modes (e.g. shadow maps: white = unshadowed).
                const uint32_t border = fetch.borderColor();
                const float borderValue = border == 1 ? 1.0f : 0.0f;
                sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] = borderValue;
                sampler.BorderColor[3] = border == 0 ? 0.0f : 1.0f;
                sampler.MipLODBias = float(fetch.lodBias()) / 32.0f;
                sampler.MinLOD = float(fetch.mipMinLevel());
                sampler.MaxLOD = mipFilter == 2 ? 0.0f : float(fetch.mipMaxLevel());
                // HC09_SAMPLER_TEST=lod0|wrap|point: debugging overrides for every sampler.
                if (static const char* test = std::getenv("HC09_SAMPLER_TEST"); test)
                {
                    if (std::strstr(test, "lod0"))
                        sampler.MinLOD = sampler.MaxLOD = 0.0f;
                    if (std::strstr(test, "wrap"))
                        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    if (std::strstr(test, "point"))
                        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                }
                samplerDescs[n] = sampler;
            }
            const uint32_t tableSize = std::max(samplerCount, 1u);
            if (samplerTableGeneration_ != context_.uploadGeneration())
            {
                samplerTables_.clear();
                samplerTableGeneration_ = context_.uploadGeneration();
            }
            const uint64_t samplerKey = XXH3_64bits(samplerDescs, sizeof(D3D12_SAMPLER_DESC) * tableSize) ^ tableSize;
            DescriptorRange samplers;
            if (auto found = samplerTables_.find(samplerKey); found != samplerTables_.end())
            {
                samplers = found->second;
            }
            else
            {
                Phase(8);
                samplers = context_.AllocateSamplers(tableSize);
                if (samplerTableGeneration_ != context_.uploadGeneration())
                {
                    // Allocation recycled the heap: earlier tables (and views) are gone. The views
                    // for this draw were allocated before the flush, so redo them below.
                    samplerTables_.clear();
                    samplerTableGeneration_ = context_.uploadGeneration();
                }
                for (uint32_t n = 0; n < tableSize; n++)
                    device->CreateSampler(&samplerDescs[n], { samplers.cpu.ptr + uint64_t(n) * context_.samplerDescriptorSize() });
                samplerTables_[samplerKey] = samplers;
                Phase(6);
            }

            // HC09_DUMP_FRAME: for each listed draw, the inputs that place it on screen.
            if (g_frame == g_dumpFrame && draw.indexCount == 6)
            {
                auto constant = [&](uint32_t index) {
                    float v[4];
                    for (int c = 0; c < 4; c++)
                        v[c] = RegisterFloat(reg::kFloatConstants + index * 4 + c);
                    std::fprintf(stderr, "    c%u = (%g, %g, %g, %g)\n", index, v[0], v[1], v[2], v[3]);
                };
                std::fprintf(stderr, "  [inputs] vte 0x%X scale (%g %g %g) offset (%g %g %g) scissor %u,%u-%u,%u blend 0x%08X\n",
                    vte, scale[0], scale[1], scale[2], offset[0], offset[1], offset[2], ReadRegister(reg::kWindowScissorTl) & 0x7FFF, (ReadRegister(reg::kWindowScissorTl) >> 16) & 0x7FFF,
                    ReadRegister(reg::kWindowScissorBr) & 0x7FFF, (ReadRegister(reg::kWindowScissorBr) >> 16) & 0x7FFF, ReadRegister(reg::kBlendControl0));
                for (uint32_t i : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 255u })
                    constant(state.vsConstBase + i);
                constant(state.psConstBase + 255);
                if (!vs.info.vertexFetchConstants.empty())
                {
                    const uint32_t index = vs.info.vertexFetchConstants[0];
                    const uint32_t d0 = ReadRegister(reg::kFetchConstants + index * 2);
                    const uint32_t d1 = ReadRegister(reg::kFetchConstants + index * 2 + 1);
                    for (uint32_t v = 0; v < 6; v++)
                    {
                        std::fprintf(stderr, "    v%u:", v);
                        for (uint32_t i = 0; i < 8; i++)
                        {
                            uint32_t raw;
                            std::memcpy(&raw, GuestToHost(PhysicalToGuest(d0 & ~3u) + (v * 8 + i) * 4), 4);
                            const uint32_t bits = GpuSwap(raw, d1 & 3);
                            float f;
                            std::memcpy(&f, &bits, 4);
                            std::fprintf(stderr, " %08X(%g)", bits, f);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }

            Phase(7);
            auto drawState = context_.Upload(sizeof(DrawState));
            if (context_.uploadGeneration() != setupGeneration && setupAttempts++ < 1)
                goto retrySetup;
            std::memcpy(drawState.cpu, &state, sizeof(state));

            list->SetGraphicsRootSignature(rootSignature_.Get());
            list->SetPipelineState(pipeline);
            list->SetGraphicsRootConstantBufferView(0, floats.gpu);
            list->SetGraphicsRootConstantBufferView(1, drawState.gpu);
            list->SetGraphicsRootDescriptorTable(2, views.gpu);
            list->SetGraphicsRootDescriptorTable(3, samplers.gpu);
            list->OMSetRenderTargets(1, &target.rtv, FALSE, &depth.dsv);
            list->OMSetStencilRef(ReadRegister(reg::kStencilRefMask) & 0xFF);
            const float blendFactor[4] = { 1, 1, 1, 1 };
            list->OMSetBlendFactor(blendFactor);

            D3D12_VIEWPORT viewport{ 0, 0, float(kTargetWidth * g_scale), float(kTargetHeight * g_scale), 0, 1 };
            list->RSSetViewports(1, &viewport);
            const uint32_t tl = ReadRegister(reg::kWindowScissorTl), br = ReadRegister(reg::kWindowScissorBr);
            D3D12_RECT scissor{ LONG(tl & 0x7FFF), LONG((tl >> 16) & 0x7FFF),
                LONG(std::min<uint32_t>(br & 0x7FFF, kTargetWidth)), LONG(std::min<uint32_t>((br >> 16) & 0x7FFF, kTargetHeight)) };
            // Predicated tiling: the first pass renders the whole frame into the screen-sized
            // target (later passes skip their draws), so the per-tile scissor doesn't apply.
            if (InFirstTilePass())
                scissor = { 0, 0, LONG(kTargetWidth), LONG(kTargetHeight) };
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
                scissor = { 0, 0, 0, 0 };
            const D3D12_RECT guestScissor = scissor;
            scissor = { LONG(scissor.left * g_scale), LONG(scissor.top * g_scale), LONG(scissor.right * g_scale),
                LONG(scissor.bottom * g_scale) };
            if (g_frame == g_dumpFrame)
            {
                static std::set<std::tuple<uint32_t, uint32_t, uint32_t, LONG, LONG, LONG, LONG>> seen;
                if (seen.insert({ ReadRegister(reg::kWindowOffset), tl, br, guestScissor.left, guestScissor.top, guestScissor.right, guestScissor.bottom }).second)
                    std::fprintf(stderr, "[frame] scissor: window offset 0x%08X tl 0x%08X br 0x%08X surface 0x%08X -> %ld,%ld-%ld,%ld\n",
                        ReadRegister(reg::kWindowOffset), tl, br, ReadRegister(reg::kSurfaceInfo), scissor.left, scissor.top, scissor.right, scissor.bottom);
            }
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(topology);

            const INT baseVertex = INT(ReadRegister(reg::kIndexOffset));
            if (indexed)
            {
                auto upload = context_.Upload(indices.size() * 4, 4);
                std::memcpy(upload.cpu, indices.data(), indices.size() * 4);
                D3D12_INDEX_BUFFER_VIEW view{ upload.gpu, UINT(indices.size() * 4), DXGI_FORMAT_R32_UINT };
                list->IASetIndexBuffer(&view);
                // VGT_INDX_OFFSET is applied in the vertex shader (vertexIndexOffset): D3D's base
                // vertex is not visible in SV_VertexID, and the shaders fetch vertices by index.
                list->DrawIndexedInstanced(UINT(indices.size()), 1, 0, 0, 0);
            }
            else
            {
                list->DrawInstanced(draw.indexCount, 1, 0, 0);
            }

            // HC09_DRAW_SNAPSHOTS=1 with HC09_DUMP_FRAME: save the render target after every
            // visible draw of that frame as build/draw_<index>.bmp.
            Phase(-1);
            // HC09_DRAW_SNAPSHOTS=1 (every draw), <first>-<last> (draw indices in that range) or
            // s<RB_SURFACE_INFO hex> (draws to that surface).
            static const char* snapshotSpec = std::getenv("HC09_DRAW_SNAPSHOTS");
            static int snapshotFirst = 0, snapshotLast = INT_MAX;
            static uint32_t snapshotSurface = 0;
            static const bool drawSnapshots = [] {
                if (!snapshotSpec)
                    return false;
                if (snapshotSpec[0] == 's')
                    snapshotSurface = uint32_t(std::strtoul(snapshotSpec + 1, nullptr, 16));
                else if (std::strchr(snapshotSpec, '-'))
                    std::sscanf(snapshotSpec, "%d-%d", &snapshotFirst, &snapshotLast);
                return true;
            }();
            // HC09_PROBE_PIXEL=<x>,<y> (with a frame dump): log that pixel of the render target
            // whenever a draw of the dump frame changes it.
            static int probeX = -1, probeY = -1;
            static const bool probe = [] {
                const char* spec = std::getenv("HC09_PROBE_PIXEL");
                return spec && std::sscanf(spec, "%d,%d", &probeX, &probeY) == 2;
            }();
            if (probe && g_frame == g_dumpFrame && target.samples == 1)
            {
                static uint64_t lastValue = ~0ull;
                static ID3D12Resource* lastTarget = nullptr;
                auto readback = CreateReadbackBuffer(256);
                const D3D12_RESOURCE_STATES original = target.state;
                Transition(list, target.resource.Get(), target.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                dst.PlacedFootprint.Footprint = { target.format, 1, 1, 1, 256 };
                D3D12_TEXTURE_COPY_LOCATION src{ target.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                const D3D12_BOX box{ UINT(probeX) * g_scale, UINT(probeY) * g_scale, 0, UINT(probeX) * g_scale + 1, UINT(probeY) * g_scale + 1, 1 };
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                Transition(list, target.resource.Get(), target.state, original);
                context_.Flush();
                uint64_t value = 0;
                void* mapped = nullptr;
                readback->Map(0, nullptr, &mapped);
                std::memcpy(&value, mapped, target.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
                readback->Unmap(0, nullptr);
                if (value != lastValue || target.resource.Get() != lastTarget)
                    std::fprintf(stderr, "[probe] after draw %d: 0x%016llX (target %p)\n", g_drawInFrame - 1,
                        (unsigned long long)value, static_cast<void*>(target.resource.Get()));
                lastValue = value;
                lastTarget = target.resource.Get();
                // The flush recycled transient allocations; later draws allocate their own again.
            }
            // With HC09_TRACE_SURFACE, also save the target after every draw to that surface
            // (build/surface_<n>.bmp, n counting from 0).
            static const uint32_t traceSurface = std::getenv("HC09_TRACE_SURFACE")
                ? uint32_t(std::strtoul(std::getenv("HC09_TRACE_SURFACE"), nullptr, 16)) : 0;
            if (traceSurface && ReadRegister(reg::kSurfaceInfo) == traceSurface && target.samples == 1)
            {
                static int surfaceDraw = 0;
                char path[64];
                std::snprintf(path, sizeof(path), "build/surface_%03d.bmp", surfaceDraw);
                SaveTexture(target.resource.Get(), target.state, target.format, kTargetWidth * g_scale, kTargetHeight * g_scale, path);
                std::snprintf(path, sizeof(path), "build/surface_%03d_depth.bmp", surfaceDraw++);
                SaveDepth(depth, path);
            }
            if (drawSnapshots && g_frame == g_dumpFrame && target.samples == 1 && g_drawInFrame >= snapshotFirst && g_drawInFrame <= snapshotLast &&
                (snapshotSurface == 0 || ReadRegister(reg::kSurfaceInfo) == snapshotSurface))
            {
                char path[64];
                std::snprintf(path, sizeof(path), "build/draw_%03d.bmp", g_drawInFrame);
                SaveTexture(target.resource.Get(), target.state, target.format, kTargetWidth * g_scale, kTargetHeight * g_scale, path);
                // HC09_STENCIL_SNAPSHOTS=1: also save the stencil plane as build/stencil_<index>.bmp.
                if (std::getenv("HC09_STENCIL_SNAPSHOTS"))
                {
                    std::snprintf(path, sizeof(path), "build/stencil_%03d.bmp", g_drawInFrame);
                    SaveStencil(depth, path);
                }
            }
        }

        void Renderer::Resolve()
        {
            const uint32_t control = ReadRegister(reg::kCopyControl);
            const uint32_t source = control & 7;
            if (g_frame == g_dumpFrame)
            {
                const D3D12_BOX rect = ResolveRectangle(kTargetWidth, kTargetHeight);
                std::fprintf(stderr, "[frame] resolve entry: control 0x%08X dest 0x%08X rect %u,%u-%u,%u select-first %d\n", control,
                    ReadRegister(reg::kCopyDestBase), rect.left, rect.top, rect.right, rect.bottom, int(InFirstTilePass()));
            }
            if (source == 4)
            {
                ResolveDepth(control);
                return;
            }
            if (source > 4)
                return;
            const uint32_t colorInfo = source == 0 ? ReadRegister(reg::kColorInfo) : ReadRegister(reg::kColor1Info + source - 1);
            RenderTarget& target = GetRenderTarget(colorInfo, SampleCount(ReadRegister(reg::kSurfaceInfo)));
            auto* list = context_.list();

            uint32_t destBase = ReadRegister(reg::kCopyDestBase) & 0x1FFFFFFF;
            const uint32_t destPitch = ReadRegister(reg::kCopyDestPitch);
            const uint32_t width = std::clamp<uint32_t>(destPitch & 0x3FFF, 1, kTargetWidth);
            const uint32_t height = std::clamp<uint32_t>((destPitch >> 16) & 0x3FFF, 1, kTargetHeight);

            // Predicated tiling resolves each screen strip with the destination base advanced to
            // that strip's tiles inside one surface. Such a base lies inside an existing target of
            // the same size: resolve into that target (the rectangle is already in screen space).
            {
                const uint32_t bytesPerPixel = target.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
                const uint32_t surfaceBytes = ((width + 31) & ~31u) * ((height + 31) & ~31u) * bytesPerPixel;
                // Only a target the first strip resolved into this frame qualifies: targets left from
                // earlier scenes can overlap the strip's address too.
                uint32_t owner = destBase;
                for (const auto& [base, existing] : resolveTargets_)
                    if (base < destBase && destBase < base + surfaceBytes && existing.width == width && existing.height == height &&
                        existing.format == target.format && existing.lastResolved == g_frame && (owner == destBase || base > owner))
                        owner = base;
                destBase = owner;
            }

            ResolveTarget& dest = resolveTargets_[destBase];
            if (!dest.resource || dest.format != target.format || dest.width != width || dest.height != height)
            {
                dest = {};
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = width * g_scale;
                desc.Height = height * g_scale;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = target.format;
                desc.SampleDesc.Count = 1;
                context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dest.resource));
                dest.format = target.format;
                dest.width = width;
                dest.height = height;
                dest.state = D3D12_RESOURCE_STATE_COPY_DEST;
            }

            // The resolve draw's three vertices (fetch constant 0, float2 each) give the
            // rectangle in screen coordinates. Draws ignore the window offset, so with
            // predicated tiling every strip pass renders into the same screen-space target and
            // the rectangle is both the source and the destination position.
            const D3D12_BOX box = ResolveRectangle(width, height);
            // HC09_TRACE_RESOLVES=1: every colour resolve (frame, destination, size, source surface).
            static const bool traceResolves = std::getenv("HC09_TRACE_RESOLVES") != nullptr;
            if (traceResolves)
                std::fprintf(stderr, "[resolve] frame %d dest 0x%08X %ux%u rect %u,%u-%u,%u surface 0x%08X\n", g_frame, destBase, width,
                    height, box.left, box.top, box.right, box.bottom, ReadRegister(reg::kSurfaceInfo));
            if (box.right <= box.left || box.bottom <= box.top)
                return;

            if (target.samples > 1)
            {
                // The resolve's box filter: average the samples of each pixel in the rectangle.
                ComPtr<ID3D12GraphicsCommandList1> list1;
                list->QueryInterface(IID_PPV_ARGS(&list1));
                Transition(list, target.resource.Get(), target.state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                Transition(list, dest.resource.Get(), dest.state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                D3D12_RECT rect = ScaledRect(box);
                list1->ResolveSubresourceRegion(dest.resource.Get(), 0, box.left * g_scale, box.top * g_scale, target.resource.Get(), 0,
                    &rect, target.format, D3D12_RESOLVE_MODE_AVERAGE);
            }
            else
            {
                Transition(list, target.resource.Get(), target.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(list, dest.resource.Get(), dest.state, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION dst{ dest.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION src{ target.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                const D3D12_BOX scaledBox = Scaled(box);
                list->CopyTextureRegion(&dst, scaledBox.left, scaledBox.top, 0, &src, &scaledBox);
            }
            Transition(list, dest.resource.Get(), dest.state,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // HC09_DUMP_AFTER_RESOLVE=<width>x<height>+<frames>[@<min frame>]: dump the frame <frames> after the
            // first colour resolve of that size (for events whose frame number varies between runs).
            if (static const char* afterSpec = std::getenv("HC09_DUMP_AFTER_RESOLVE"); afterSpec && g_dumpFrame < 0)
            {
                unsigned afterWidth = 0, afterHeight = 0;
                int delay = 0, minFrame = 0;
                std::sscanf(afterSpec, "%ux%u+%d@%d", &afterWidth, &afterHeight, &delay, &minFrame);
                if (width == afterWidth && height == afterHeight && g_frame >= minFrame)
                {
                    g_dumpFrame = g_frame + delay;
                    std::fprintf(stderr, "[frame] will dump frame %d\n", g_dumpFrame);
                }
            }
            // HC09_SAVE_RESOLVES=<width>x<height>[@<first frame>-<last frame>]: save every colour resolve of
            // that size (in those frames) as build/resolve_<frame>_<address>.bmp.
            if (static const char* saveSpec = std::getenv("HC09_SAVE_RESOLVES"); saveSpec)
            {
                unsigned saveWidth = 0, saveHeight = 0;
                int firstFrame = 0, lastFrame = INT_MAX;
                std::sscanf(saveSpec, "%ux%u@%d-%d", &saveWidth, &saveHeight, &firstFrame, &lastFrame);
                if (width == saveWidth && height == saveHeight && g_frame >= firstFrame && g_frame <= lastFrame)
                {
                    char path[96];
                    std::snprintf(path, sizeof(path), "build/resolve_%d_%08X.bmp", g_frame, destBase);
                    SaveTexture(dest.resource.Get(), dest.state, dest.format, width * g_scale, height * g_scale, path);
                }
            }
            // RB_COPY_DEST_INFO bit 24: copy_dest_swap (red and blue exchanged in memory).
            dest.lastResolved = g_frame;
            textures_->SetResolved(destBase, dest.resource.Get(), dest.format, width, height, (ReadRegister(0x231B) >> 24) & 1);

            Transition(list, target.resource.Get(), target.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (g_frame == g_dumpFrame)
                std::fprintf(stderr, "[frame] resolve: control 0x%08X dest 0x%08X pitch 0x%08X info 0x%08X clear 0x%08X/0x%08X depth clear 0x%08X depth info 0x%08X tiling %d\n",
                    control, destBase, destPitch, ReadRegister(0x231B), ReadRegister(reg::kColorClear), ReadRegister(0x231E),
                    ReadRegister(0x231C), ReadRegister(reg::kDepthInfo), int(TilingActive()));
            if (g_frame == g_dumpFrame)
                std::fprintf(stderr, "[frame]   rect %u,%u-%u,%u window offset 0x%08X sc mode 0x%08X\n", box.left, box.top,
                    box.right, box.bottom, ReadRegister(reg::kWindowOffset), ReadRegister(reg::kScModeControl));
            // Tile resolves clear their EDRAM tile for the next tile. Targets here hold the whole
            // screen (the first pass rendered it all), so clearing a strip would wipe content
            // that later full-screen resolves still copy.
            const bool tileClear = TilingActive();
            // During tiling every strip resolves its depth before its colour resolve clears the
            // tile. All strips share one screen-sized depth buffer here, so clearing it at an early
            // strip would hand the later strips' depth resolves a cleared buffer: clear the whole
            // buffer only once the last (bottom-right) strip has been resolved.
            const bool lastTile = box.right >= width && box.bottom >= height;
            if (((control >> 9) & 1) && (!tileClear || lastTile))  // depth_clear_enable: RB_DEPTH_CLEAR is depth (24 bits) << 8 | stencil
            {
                // During tiling all drawing happened in the first pass and the frame's own depth
                // clear is tile-sized, so clear the whole depth target: the next frame's first
                // pass then starts from cleared depth everywhere.
                const uint32_t clear = ReadRegister(reg::kDepthClear);
                const D3D12_RECT rect = ScaledRect(box);
                list->ClearDepthStencilView(GetDepthTarget(ReadRegister(reg::kDepthInfo), target.samples).dsv,
                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, float(clear >> 8) / float(0xFFFFFF), UINT8(clear & 0xFF),
                    tileClear ? 0 : 1, tileClear ? nullptr : &rect);
            }
            if (((control >> 8) & 1) && !tileClear)  // color_clear_enable
            {
                // For 32bpp targets the clear value is RB_COLOR_CLEAR_LO, packed in the target's
                // format (8_8_8_8: red in the low byte). RB_COLOR_CLEAR holds the high half for 64bpp.
                const uint32_t clear = ReadRegister(reg::kColorClear + 1);
                const float color[4] = { (clear & 0xFF) / 255.0f, ((clear >> 8) & 0xFF) / 255.0f, ((clear >> 16) & 0xFF) / 255.0f,
                    (clear >> 24) / 255.0f };
                const D3D12_RECT rect = ScaledRect(box);
                list->ClearRenderTargetView(target.rtv, color, 1, &rect);
            }
        }

        bool Renderer::CreateDepthCopyPipeline()
        {
            return CreateDepthCopyPipeline(kDepthCopyShader, depthCopyPipeline_) &&
                CreateDepthCopyPipeline(kDepthCopyMsShader, depthCopyMsPipeline_);
        }

        bool Renderer::CreateDepthCopyPipeline(const char* source, ComPtr<ID3D12PipelineState>& pipeline)
        {
            ComPtr<ID3DBlob> vs, ps, errors;
            if (FAILED(D3DCompile(source, std::strlen(source), "depth_copy", nullptr, nullptr, "vs_main", "vs_5_1", 0, 0, &vs, &errors)) ||
                FAILED(D3DCompile(source, std::strlen(source), "depth_copy", nullptr, nullptr, "ps_main", "ps_5_1", 0, 0, &ps, &errors)))
            {
                std::fprintf(stderr, "[gpu] depth copy shader: %s\n", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                return false;
            }
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = presentRootSignature_.Get();
            desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
            desc.SampleDesc.Count = 1;
            return SUCCEEDED(context_.device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
        }

        // Resolve of the depth buffer: its depth (0..1) into an R32_FLOAT texture at the
        // destination, over the resolve rectangle; then the requested depth clear.
        void Renderer::ResolveDepth(uint32_t control)
        {
            auto* list = context_.list();
            const uint32_t rawDestBase = ReadRegister(reg::kCopyDestBase) & 0x1FFFFFFF;
            const uint32_t destPitch = ReadRegister(reg::kCopyDestPitch);
            const uint32_t width = std::clamp<uint32_t>(destPitch & 0x3FFF, 1, kTargetWidth);
            const uint32_t height = std::clamp<uint32_t>((destPitch >> 16) & 0x3FFF, 1, kTargetHeight);
            DepthTarget& depth = GetDepthTarget(ReadRegister(reg::kDepthInfo), SampleCount(ReadRegister(reg::kSurfaceInfo)));
            const D3D12_BOX box = ResolveRectangle(width, height);

            // Predicated tiling resolves depth strip by strip with the destination advanced into one
            // surface, as for colour: resolve every strip into the surface's single target.
            uint32_t destBase = rawDestBase;
            {
                const uint32_t surfaceBytes = ((width + 31) & ~31u) * ((height + 31) & ~31u) * 4;
                uint32_t owner = destBase;
                for (const auto& [base, existing] : resolveTargets_)
                    if (base < destBase && destBase < base + surfaceBytes && existing.width == width && existing.height == height &&
                        existing.format == DXGI_FORMAT_R32_FLOAT && existing.lastResolved == g_frame && (owner == destBase || base > owner))
                        owner = base;
                destBase = owner;
            }

            ResolveTarget& dest = resolveTargets_[destBase];
            if (!dest.resource || dest.format != DXGI_FORMAT_R32_FLOAT || dest.width != width || dest.height != height)
            {
                dest = {};
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = width * g_scale;
                desc.Height = height * g_scale;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = DXGI_FORMAT_R32_FLOAT;
                desc.SampleDesc.Count = 1;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (FAILED(context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&dest.resource))))
                    return;
                dest.format = DXGI_FORMAT_R32_FLOAT;
                dest.width = width;
                dest.height = height;
                dest.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                dest.rtv = context_.AllocateRtv();
                context_.device()->CreateRenderTargetView(dest.resource.Get(), nullptr, dest.rtv);
            }
            if (g_frame == g_dumpFrame)
                std::fprintf(stderr, "[frame] depth resolve: control 0x%08X dest 0x%08X %ux%u rect %u,%u-%u,%u\n", control, destBase,
                    width, height, box.left, box.top, box.right, box.bottom);
            if (box.right > box.left && box.bottom > box.top)
            {
                Transition(list, depth.resource.Get(), depth.state,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Transition(list, dest.resource.Get(), dest.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                const DescriptorRange view = context_.AllocateViews(1);
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                srv.ViewDimension = depth.samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (depth.samples == 1)
                    srv.Texture2D.MipLevels = 1;
                context_.device()->CreateShaderResourceView(depth.resource.Get(), &srv, view.cpu);
                list->SetGraphicsRootSignature(presentRootSignature_.Get());
                list->SetPipelineState(depth.samples > 1 ? depthCopyMsPipeline_.Get() : depthCopyPipeline_.Get());
                list->SetGraphicsRootDescriptorTable(0, view.gpu);
                list->OMSetRenderTargets(1, &dest.rtv, FALSE, nullptr);
                D3D12_VIEWPORT viewport{ 0, 0, float(width * g_scale), float(height * g_scale), 0, 1 };
                D3D12_RECT scissor = ScaledRect(box);
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawInstanced(3, 1, 0, 0);
                Transition(list, depth.resource.Get(), depth.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
            Transition(list, dest.resource.Get(), dest.state,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            dest.lastResolved = g_frame;
            textures_->SetResolved(destBase, dest.resource.Get(), dest.format, width, height);

            // As for colour resolves: during tiling, clear the shared depth buffer only after the
            // last strip.
            const bool lastTile = box.right >= width && box.bottom >= height;
            if (((control >> 9) & 1) && (!TilingActive() || lastTile))  // depth_clear_enable
            {
                const uint32_t clear = ReadRegister(reg::kDepthClear);
                const D3D12_RECT rect = ScaledRect(box);
                const bool whole = TilingActive();
                list->ClearDepthStencilView(depth.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    float(clear >> 8) / float(0xFFFFFF), UINT8(clear & 0xFF), whole ? 0 : 1, whole ? nullptr : &rect);
            }
        }

        // Bounding box of the resolve draw's rectangle, clamped to the destination size.
        D3D12_BOX Renderer::ResolveRectangle(uint32_t destWidth, uint32_t destHeight)
        {
            const uint32_t d0 = ReadRegister(reg::kFetchConstants), d1 = ReadRegister(reg::kFetchConstants + 1);
            const uint32_t address = PhysicalToGuest(d0 & ~3u);
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (uint32_t i = 0; i < 6; i++)
            {
                uint32_t raw;
                std::memcpy(&raw, GuestToHost(address + i * 4), 4);
                const uint32_t bits = GpuSwap(raw, d1 & 3);
                float value;
                std::memcpy(&value, &bits, 4);
                if (i & 1)
                    minY = std::min(minY, value), maxY = std::max(maxY, value);
                else
                    minX = std::min(minX, value), maxX = std::max(maxX, value);
            }
            auto clamp = [](float value, uint32_t limit) {
                return uint32_t(std::clamp(value, 0.0f, float(limit)));
            };
            const uint32_t limitX = std::min(destWidth, kTargetWidth), limitY = std::min(destHeight, kTargetHeight);
            return { clamp(minX, limitX), clamp(minY, limitY), 0, clamp(maxX, limitX), clamp(maxY, limitY), 1 };
        }

        void Renderer::EvictIdleResources(uint32_t frontBuffer)
        {
            constexpr int kIdleFrames = 1800;  // ~1 minute
            textures_->Evict(g_frame);
            std::erase_if(retiredTargets_, [&](const auto& item) { return g_frame - item.first > 4; });
            if (g_frame % 300 != 0)
                return;
            for (auto it = resolveTargets_.begin(); it != resolveTargets_.end();)
            {
                const int lastUse = std::max(it->second.lastResolved, textures_->ResolvedLastUse(it->first));
                // Depth targets own a render-target descriptor (never freed), and the front buffer is shown every frame.
                if (it->second.rtv.ptr == 0 && it->first != (frontBuffer & 0x1FFFFFFF) && g_frame - lastUse > kIdleFrames)
                {
                    textures_->ForgetResolved(it->first);
                    retiredTargets_.emplace_back(g_frame, std::move(it->second.resource));
                    it = resolveTargets_.erase(it);
                    g_evictedResolveTargets++;
                }
                else
                {
                    ++it;
                }
            }
        }

        void Renderer::Swap(uint32_t frontBuffer)
        {
            ReportPhases();
            EvictIdleResources(frontBuffer);
            // HC09_GPU_STATS: GPU memory and cache sizes every 2 s (they must stay bounded over many games).
            if (g_phaseStats)
            {
                static auto reported = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - reported > std::chrono::seconds(2))
                {
                    reported = std::chrono::steady_clock::now();
                    const auto stats = textures_->GetStats();
                    std::fprintf(stderr, "[gpu] memory: frame %d, video memory %.0f MB, %zu cached textures (%llu evicted), %zu resolve targets "
                        "(%llu evicted), %zu resolved addresses (%llu stale), %llu mismatched resolve fetches\n", g_frame,
                        context_.VideoMemoryUsage() / 1048576.0, stats.textures, (unsigned long long)stats.evictedTextures,
                        resolveTargets_.size(), (unsigned long long)g_evictedResolveTargets, stats.resolved,
                        (unsigned long long)stats.staleResolves, (unsigned long long)stats.mismatchedResolveHits);
                }
            }
            auto* list = context_.list();
            const D3D12_CPU_DESCRIPTOR_HANDLE backBuffer = context_.BackBufferRtv();
            auto resolved = resolveTargets_.find(frontBuffer & 0x1FFFFFFF);
            if (resolved == resolveTargets_.end())
            {
                const float gray[4] = { 0.08f, 0.08f, 0.1f, 1.0f };
                list->ClearRenderTargetView(backBuffer, gray, 0, nullptr);
            }
            else
            {
                ResolveTarget& front = resolved->second;
                Transition(list, front.resource.Get(), front.state,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                const DescriptorRange view = context_.AllocateViews(1);
                context_.device()->CreateShaderResourceView(front.resource.Get(), nullptr, view.cpu);
                list->SetGraphicsRootSignature(presentRootSignature_.Get());
                list->SetPipelineState(presentPipeline_.Get());
                list->SetGraphicsRootDescriptorTable(0, view.gpu);
                list->OMSetRenderTargets(1, &backBuffer, FALSE, nullptr);
                // Keep the frame's aspect ratio in a resized or fullscreen window: the largest
                // centred rectangle of that shape, with black bars around it.
                const float windowWidth = float(context_.width()), windowHeight = float(context_.height());
                const float aspect = float(front.width) / float(front.height);
                float viewWidth = windowWidth, viewHeight = windowWidth / aspect;
                if (viewHeight > windowHeight)
                {
                    viewHeight = windowHeight;
                    viewWidth = windowHeight * aspect;
                }
                if (viewWidth < windowWidth || viewHeight < windowHeight)
                {
                    const float black[4] = { 0, 0, 0, 1 };
                    list->ClearRenderTargetView(backBuffer, black, 0, nullptr);
                }
                const float viewX = std::floor((windowWidth - viewWidth) / 2), viewY = std::floor((windowHeight - viewHeight) / 2);
                D3D12_VIEWPORT viewport{ viewX, viewY, viewWidth, viewHeight, 0, 1 };
                D3D12_RECT scissor{ LONG(viewX), LONG(viewY), LONG(viewX + viewWidth), LONG(viewY + viewHeight) };
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawInstanced(3, 1, 0, 0);
            }
            static const int dumpResolvesFrame = std::getenv("HC09_DUMP_RESOLVES") ? std::atoi(std::getenv("HC09_DUMP_RESOLVES")) : -1;
            if (g_frame == dumpResolvesFrame)
                DumpResolves();
            if (std::find(g_screenshotFrames.begin(), g_screenshotFrames.end(), g_frame) != g_screenshotFrames.end())
            {
                std::fprintf(stderr, "[gpu] screenshot frame %d: front buffer 0x%08X (%s); resolves:", g_frame, frontBuffer,
                    resolved == resolveTargets_.end() ? "not resolved" : "found");
                for (const auto& [address, target] : resolveTargets_)
                    std::fprintf(stderr, " 0x%08X(%ux%u)", address, target.width, target.height);
                std::fprintf(stderr, "; %u draws skipped so far\n", skippedDraws_);
                char path[64];
                std::snprintf(path, sizeof(path), "build/screenshot_%d.bmp", g_frame);
                SaveScreenshot(path);
            }
            context_.Present();
        }

        // Copies the current back buffer to the CPU and writes it as a 32-bit BMP.
        void Renderer::SaveScreenshot(const char* path)
        {
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            SaveTexture(context_.BackBuffer(), state, DXGI_FORMAT_R8G8B8A8_UNORM, context_.width(), context_.height(), path);
        }

        // HC09_DUMP_RESOLVES=<frame>: saves every resolved texture and host render target.
        void Renderer::DumpResolves()
        {
            for (auto& [address, target] : resolveTargets_)
            {
                char path[64];
                std::snprintf(path, sizeof(path), "build/resolve_%08X.bmp", address);
                SaveTexture(target.resource.Get(), target.state, target.format, target.width, target.height, path);
            }
            for (auto& [key, target] : renderTargets_)
            {
                char path[64];
                std::snprintf(path, sizeof(path), "build/rt_%05X.bmp", key);
                SaveTexture(target.resource.Get(), target.state, target.format, kTargetWidth * g_scale, kTargetHeight * g_scale, path);
            }
        }

        // Copies a 2D texture to the CPU and writes it as a 32-bit BMP. RGBA8 is written
        // directly; 10:10:10:2 and half-float targets are converted (floats clamped to 0..1).
        // Saves a depth target's depth plane as a greyscale BMP (0 black .. 1 white), and logs the
        // value range found in the top-left 256x256.
        void Renderer::SaveDepth(DepthTarget& target, const char* path)
        {
            auto* list = context_.list();
            const D3D12_RESOURCE_DESC desc = target.resource->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 total = 0;
            context_.device()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
            auto readback = CreateReadbackBuffer(total);
            if (!readback)
                return;
            const D3D12_RESOURCE_STATES original = target.state;
            Transition(list, target.resource.Get(), target.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION src{ target.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list, target.resource.Get(), target.state, original);
            context_.Flush();
            uint8_t* pixels = nullptr;
            readback->Map(0, nullptr, reinterpret_cast<void**>(&pixels));
            const uint32_t width = uint32_t(desc.Width), height = desc.Height, rowPitch = footprint.Footprint.RowPitch;
            float minDepth = 2.0f, maxDepth = -1.0f;
            if (FILE* file = std::fopen(path, "wb"))
            {
                const uint32_t imageBytes = width * height * 4;
                const uint8_t fileHeader[14] = { 'B', 'M', uint8_t(54 + imageBytes), uint8_t((54 + imageBytes) >> 8),
                    uint8_t((54 + imageBytes) >> 16), uint8_t((54 + imageBytes) >> 24), 0, 0, 0, 0, 54, 0, 0, 0 };
                uint8_t infoHeader[40] = { 40 };
                std::memcpy(infoHeader + 4, &width, 4);
                const int32_t negativeHeight = -int32_t(height);
                std::memcpy(infoHeader + 8, &negativeHeight, 4);
                infoHeader[12] = 1;
                infoHeader[14] = 32;
                std::fwrite(fileHeader, 1, sizeof(fileHeader), file);
                std::fwrite(infoHeader, 1, sizeof(infoHeader), file);
                std::vector<uint8_t> row(width * 4);
                for (uint32_t y = 0; y < height; y++)
                {
                    for (uint32_t x = 0; x < width; x++)
                    {
                        uint32_t texel;
                        std::memcpy(&texel, pixels + uint64_t(y) * rowPitch + x * 4, 4);
                        const float depth = float(texel & 0xFFFFFF) / float(0xFFFFFF);
                        if (x < 256 && y < 256)
                        {
                            minDepth = std::min(minDepth, depth);
                            maxDepth = std::max(maxDepth, depth);
                        }
                        const uint8_t v = uint8_t(depth * 255.0f + 0.5f);
                        row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = v;
                        row[x * 4 + 3] = 255;
                    }
                    std::fwrite(row.data(), 1, row.size(), file);
                }
                std::fclose(file);
            }
            readback->Unmap(0, nullptr);
            std::fprintf(stderr, "[gpu] %s: depth in top-left 256x256 %.4f..%.4f (footprint format %d)\n", path, minDepth, maxDepth,
                int(footprint.Footprint.Format));
        }

        // Saves a depth target's stencil plane as a BMP: bit 0 in red, bit 1 in green, bits 2-7 in blue.
        void Renderer::SaveStencil(DepthTarget& target, const char* path)
        {
            auto* list = context_.list();
            const uint32_t rowPitch = (kTargetWidth * g_scale + 255) & ~255u;
            auto readback = CreateReadbackBuffer(uint64_t(rowPitch) * kTargetHeight * g_scale);
            if (!readback)
                return;
            const D3D12_RESOURCE_STATES original = target.state;
            Transition(list, target.resource.Get(), target.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_TYPELESS, kTargetWidth * g_scale, kTargetHeight * g_scale, 1, rowPitch };
            D3D12_TEXTURE_COPY_LOCATION src{ target.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            src.SubresourceIndex = 1;  // stencil plane
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list, target.resource.Get(), target.state, original);
            context_.Flush();

            uint8_t* pixels = nullptr;
            readback->Map(0, nullptr, reinterpret_cast<void**>(&pixels));
            if (FILE* file = std::fopen(path, "wb"))
            {
                const uint32_t width = kTargetWidth * g_scale, height = kTargetHeight * g_scale;
                const uint32_t imageBytes = width * height * 4;
                const uint8_t fileHeader[14] = { 'B', 'M', uint8_t(54 + imageBytes), uint8_t((54 + imageBytes) >> 8),
                    uint8_t((54 + imageBytes) >> 16), uint8_t((54 + imageBytes) >> 24), 0, 0, 0, 0, 54, 0, 0, 0 };
                uint8_t infoHeader[40] = { 40 };
                std::memcpy(infoHeader + 4, &width, 4);
                const int32_t negativeHeight = -int32_t(height);
                std::memcpy(infoHeader + 8, &negativeHeight, 4);
                infoHeader[12] = 1;
                infoHeader[14] = 32;
                std::fwrite(fileHeader, 1, sizeof(fileHeader), file);
                std::fwrite(infoHeader, 1, sizeof(infoHeader), file);
                std::vector<uint8_t> row(width * 4);
                for (uint32_t y = 0; y < height; y++)
                {
                    for (uint32_t x = 0; x < width; x++)
                    {
                        const uint8_t s = pixels[uint64_t(y) * rowPitch + x];
                        row[x * 4 + 0] = uint8_t(s & 0xFC);        // blue
                        row[x * 4 + 1] = (s & 2) ? 255 : 0;        // green
                        row[x * 4 + 2] = (s & 1) ? 255 : 0;        // red
                        row[x * 4 + 3] = 255;
                    }
                    std::fwrite(row.data(), 1, row.size(), file);
                }
                std::fclose(file);
            }
            readback->Unmap(0, nullptr);
        }

        void Renderer::SaveTexture(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, DXGI_FORMAT format,
            uint32_t width, uint32_t height, const char* path)
        {
            const uint32_t texelBytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
            if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R10G10B10A2_UNORM &&
                format != DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                std::fprintf(stderr, "[gpu] cannot save %s: format %u\n", path, unsigned(format));
                return;
            }
            // The copy takes the whole of mip 0, so the footprint must match the resource (a fetch
            // can describe a smaller texture than the resolve target it samples).
            const D3D12_RESOURCE_DESC desc = resource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                return;
            width = uint32_t(desc.Width);
            height = desc.Height;
            auto* list = context_.list();
            const uint32_t rowPitch = (width * texelBytes + 255) & ~255u;
            auto readback = CreateReadbackBuffer(uint64_t(rowPitch) * height);
            if (!readback)
                return;

            const D3D12_RESOURCE_STATES original = state;
            Transition(list, resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint.Footprint = { format, width, height, 1, rowPitch };
            D3D12_TEXTURE_COPY_LOCATION src{ resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list, resource, state, original);
            context_.Flush();

            uint8_t* pixels = nullptr;
            if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&pixels))) || !pixels)
            {
                std::fprintf(stderr, "[gpu] cannot save %s: readback failed\n", path);
                return;
            }
            FILE* file = std::fopen(path, "wb");
            if (file)
            {
                const uint32_t imageBytes = width * height * 4;
                const uint8_t fileHeader[14] = { 'B', 'M', uint8_t(54 + imageBytes), uint8_t((54 + imageBytes) >> 8),
                    uint8_t((54 + imageBytes) >> 16), uint8_t((54 + imageBytes) >> 24), 0, 0, 0, 0, 54, 0, 0, 0 };
                uint8_t infoHeader[40] = { 40 };
                std::memcpy(infoHeader + 4, &width, 4);
                const int32_t negativeHeight = -int32_t(height);  // top-down rows
                std::memcpy(infoHeader + 8, &negativeHeight, 4);
                infoHeader[12] = 1;
                infoHeader[14] = 32;
                std::fwrite(fileHeader, 1, sizeof(fileHeader), file);
                std::fwrite(infoHeader, 1, sizeof(infoHeader), file);
                auto toByte = [](float value) { return uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
                std::vector<uint8_t> row(width * 4);
                for (uint32_t y = 0; y < height; y++)
                {
                    const uint8_t* source = pixels + uint64_t(y) * rowPitch;
                    for (uint32_t x = 0; x < width; x++)
                    {
                        uint8_t rgb[3];
                        if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
                        {
                            rgb[0] = source[x * 4 + 0], rgb[1] = source[x * 4 + 1], rgb[2] = source[x * 4 + 2];
                        }
                        else if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
                        {
                            uint32_t texel;
                            std::memcpy(&texel, source + x * 4, 4);
                            for (int c = 0; c < 3; c++)
                                rgb[c] = uint8_t(((texel >> (c * 10)) & 0x3FF) >> 2);
                        }
                        else
                        {
                            for (int c = 0; c < 3; c++)
                            {
                                uint16_t half;
                                std::memcpy(&half, source + x * 8 + c * 2, 2);
                                rgb[c] = toByte(PPCHalfToFloat(half));
                            }
                        }
                        row[x * 4 + 0] = rgb[2];  // BGRA
                        row[x * 4 + 1] = rgb[1];
                        row[x * 4 + 2] = rgb[0];
                        row[x * 4 + 3] = 255;
                    }
                    std::fwrite(row.data(), 1, row.size(), file);
                }
                std::fclose(file);
                std::fprintf(stderr, "[gpu] wrote %s\n", path);
            }
            readback->Unmap(0, nullptr);
        }

        ComPtr<ID3D12Resource> Renderer::CreateReadbackBuffer(uint64_t size)
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> buffer;
            context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&buffer));
            return buffer;
        }

        Renderer* g_renderer = nullptr;
        bool g_rendererFailed = false;

        Renderer* GetRenderer()
        {
            if (!g_renderer && !g_rendererFailed && !std::getenv("HC09_NO_RENDER"))
            {
                g_renderer = new Renderer();
                if (!g_renderer->Initialize())
                {
                    std::fprintf(stderr, "[gpu] renderer initialization failed; continuing without graphics\n");
                    delete g_renderer;
                    g_renderer = nullptr;
                    g_rendererFailed = true;
                }
            }
            return g_renderer;
        }

        void DumpDraw(const DrawInfo& draw)
        {
            const auto vs = ActiveShader(ShaderType::Vertex);
            const auto ps = ActiveShader(ShaderType::Pixel);
            std::fprintf(stderr,
                "[frame] draw %d: prim %u source %u count %u%s | mode %u color 0x%08X surface 0x%08X depth 0x%08X "
                "mask 0x%X | vs %016llX ps %016llX | vte 0x%X vport scale (%g %g %g) offset (%g %g %g) | "
                "window offset 0x%X scissor 0x%X-0x%X | consts vs 0x%X ps 0x%X\n",
                g_drawInFrame, uint32_t(draw.primitive), uint32_t(draw.source), draw.indexCount, draw.index32 ? " (32-bit)" : "",
                ReadRegister(0x2208) & 7, ReadRegister(0x2001), ReadRegister(0x2000), ReadRegister(0x2002), ReadRegister(0x2104),
                vs ? (unsigned long long)vs->hash : 0ull, ps ? (unsigned long long)ps->hash : 0ull, ReadRegister(0x2206),
                RegisterFloat(0x210F), RegisterFloat(0x2111), RegisterFloat(0x2113), RegisterFloat(0x2110), RegisterFloat(0x2112),
                RegisterFloat(0x2114), ReadRegister(0x2080), ReadRegister(0x2081), ReadRegister(0x2082), ReadRegister(0x2307),
                ReadRegister(0x2308));
            // VGT_MAX_VTX_INDX (0x2100), VGT_MIN_VTX_INDX (0x2101), VGT_INDX_OFFSET (0x2102),
            // VGT_MULTI_PRIM_IB_RESET_INDX (0x2103), PA_SU_SC_MODE_CNTL (0x2205).
            std::fprintf(stderr, "        vgt: max index %u min index %u index offset %u reset index 0x%X sc mode 0x%08X | depth control 0x%08X depth info 0x%08X color mask 0x%X blend 0x%08X clip 0x%08X\n",
                ReadRegister(0x2100), ReadRegister(0x2101), ReadRegister(0x2102), ReadRegister(0x2103), ReadRegister(0x2205),
                ReadRegister(0x2200), ReadRegister(0x2002), ReadRegister(0x2104), ReadRegister(0x2201), ReadRegister(0x2204));
            // HC09_DUMP_PS_CONSTS / HC09_DUMP_VS_CONSTS=<first>-<last>: that stage's float constants.
            for (const auto& [variable, baseRegister, label] : { std::tuple{ "HC09_DUMP_VS_CONSTS", 0x2307u, "vs" },
                     std::tuple{ "HC09_DUMP_PS_CONSTS", 0x2308u, "ps" } })
            {
                const char* spec = std::getenv(variable);
                if (!spec)
                    continue;
                int first = 0, last = -1;
                std::sscanf(spec, "%d-%d", &first, &last);
                const uint32_t constBase = ReadRegister(baseRegister) & 0x1FF;
                for (int c = first; c <= last && c < 256; c++)
                {
                    const uint32_t reg = 0x4000 + (constBase + uint32_t(c)) * 4;
                    std::fprintf(stderr, "        %s c%d: %g %g %g %g\n", label, c, RegisterFloat(reg), RegisterFloat(reg + 1),
                        RegisterFloat(reg + 2), RegisterFloat(reg + 3));
                }
            }
            for (uint32_t slot = 0; slot < 32; slot++)
            {
                const uint32_t base = 0x4800 + slot * 6;
                const uint32_t d0 = ReadRegister(base);
                if ((d0 & 3) == 2)
                    std::fprintf(stderr, "        tf%u: %08X %08X %08X %08X %08X %08X\n", slot, d0, ReadRegister(base + 1),
                        ReadRegister(base + 2), ReadRegister(base + 3), ReadRegister(base + 4), ReadRegister(base + 5));
            }
            for (uint32_t index = 0; index < 96; index++)
            {
                const uint32_t d0 = ReadRegister(0x4800 + index * 2), d1 = ReadRegister(0x4800 + index * 2 + 1);
                if ((d0 & 3) == 3 && (d1 & 0xFFFFFC))
                {
                    const uint32_t size = (d1 >> 2) & 0xFFFFFF;
                    std::fprintf(stderr, "        vf%u: address 0x%08X endian %u size %u dwords\n", index, d0 & ~3u, d1 & 3, size);
                    // Small buffers in full; with HC09_DUMP_VERTEX_DATA=1 also the start of larger ones.
                    static const bool vertexData = std::getenv("HC09_DUMP_VERTEX_DATA") != nullptr;
                    if (size <= 64 || vertexData)
                    {
                        std::fprintf(stderr, "          data%s:", size > 48 ? " (first 48 dwords)" : "");
                        for (uint32_t i = 0; i < std::min<uint32_t>(size, 48); i++)
                        {
                            uint32_t raw;
                            std::memcpy(&raw, GuestToHost(PhysicalToGuest(d0 & ~3u) + i * 4), 4);
                            const uint32_t value = GpuSwap(raw, d1 & 3);
                            float asFloat;
                            std::memcpy(&asFloat, &value, 4);
                            std::fprintf(stderr, " %08X(%g)", value, asFloat);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
        }

        // HC09_FIND_STRING=<text> (with a frame dump): every case-insensitive occurrence of that text in
        // guest memory, with the surrounding printable bytes.
        void FindString()
        {
            const char* spec = std::getenv("HC09_FIND_STRING");
            if (!spec || !*spec)
                return;
            const size_t length = std::strlen(spec);
            const struct { uint32_t begin, end; } ranges[] = {
                { kPoolHeapBase, kVirtualHeapEnd }, { uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE) },
                { kPhysicalHeapBase, kPhysicalHeapEnd },
            };
            int matches = 0;
            for (const auto& range : ranges)
            {
                const uint8_t* bytes = GuestToHost(range.begin);
                for (uint32_t offset = 0; offset + length <= range.end - range.begin && matches < 200; offset++)
                {
                    if (std::tolower(bytes[offset]) != std::tolower(uint8_t(spec[0])) || _strnicmp(reinterpret_cast<const char*>(bytes + offset), spec, length) != 0)
                        continue;
                    uint32_t start = offset;
                    while (start > 0 && offset - start < 120 && bytes[start - 1] >= 0x20 && bytes[start - 1] < 0x7F)
                        start--;
                    uint32_t end = offset + uint32_t(length);
                    while (end < range.end - range.begin && end - offset < 120 && bytes[end] >= 0x20 && bytes[end] < 0x7F)
                        end++;
                    std::fprintf(stderr, "[find] \"%s\" at 0x%08X: %.*s\n", spec, range.begin + offset, int(end - start),
                        reinterpret_cast<const char*>(bytes + start));
                    matches++;
                    offset = end;
                }
            }
            std::fprintf(stderr, "[find] %d matches for \"%s\"\n", matches, spec);
        }

        void FindDwords()
        {
            FindString();
            const char* spec = std::getenv("HC09_FIND_DWORDS");
            if (!spec)
                return;
            std::vector<uint32_t> pattern;
            for (const char* p = spec; *p;)
            {
                char* end;
                pattern.push_back(__builtin_bswap32(uint32_t(std::strtoul(p, &end, 16))));
                if (end == p)
                    break;
                p = *end == ',' ? end + 1 : end;
            }
            const size_t bytes = pattern.size() * 4;
            const struct { uint32_t begin, end; } ranges[] = {
                { kPoolHeapBase, kVirtualHeapEnd }, { uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE) },
                { kPhysicalHeapBase, kPhysicalHeapEnd },
            };
            for (const auto& range : ranges)
            {
                for (uint32_t address = range.begin; address + bytes <= range.end; address += 4)
                {
                    if (std::memcmp(GuestToHost(address), pattern.data(), bytes) != 0)
                        continue;
                    std::fprintf(stderr, "[find] match at 0x%08X; preceding dwords:", address);
                    for (uint32_t i = 16; i > 0; i--)
                        std::fprintf(stderr, " %08X", __builtin_bswap32(*reinterpret_cast<uint32_t*>(GuestToHost(address - i * 4))));
                    std::fprintf(stderr, "\n");
                }
            }
        }
    }

    bool IsDumpFrame()
    {
        return g_frame == g_dumpFrame;
    }

    void ExecuteDraw(const DrawInfo& draw)
    {
        // HC09_TRACE_SURFACE=<RB_SURFACE_INFO hex>: log every draw to that surface, in any frame.
        static const uint32_t traceSurface = std::getenv("HC09_TRACE_SURFACE")
            ? uint32_t(std::strtoul(std::getenv("HC09_TRACE_SURFACE"), nullptr, 16)) : 0;
        if (g_frame == g_dumpFrame || (traceSurface && ReadRegister(0x2000) == traceSurface))
            DumpDraw(draw);
        g_drawInFrame++;
        if (Renderer* renderer = GetRenderer())
            renderer->Draw(draw);
    }

    int FrameNumber()
    {
        return g_frame;
    }

    void ExecuteSwap(uint32_t frontBuffer, uint32_t width, uint32_t height)
    {
        if (g_frame == g_dumpFrame)
        {
            std::fprintf(stderr, "[frame] swap: front buffer 0x%08X %ux%u\n", frontBuffer, width, height);
            FindDwords();
            // HC09_DUMP_MEMORY=<physical hex>:<size hex>[,...]: save raw memory as build/mem_<addr>.bin.
            if (const char* spec = std::getenv("HC09_DUMP_MEMORY"))
            {
                for (const char* p = spec; *p;)
                {
                    char* end = nullptr;
                    const uint32_t address = uint32_t(std::strtoul(p, &end, 16));
                    const uint32_t size = *end == ':' ? uint32_t(std::strtoul(end + 1, &end, 16)) : 0;
                    char path[64];
                    std::snprintf(path, sizeof(path), "build/mem_%08X.bin", address);
                    if (FILE* file = std::fopen(path, "wb"))
                    {
                        std::fwrite(GuestToHost(PhysicalToGuest(address)), 1, size, file);
                        std::fclose(file);
                    }
                    if (*end != ',')
                        break;
                    p = end + 1;
                }
            }
        }
        // Hitches: frames over 200 ms are logged with the shader compile time spent in them.
        {
            static auto lastSwap = std::chrono::steady_clock::now();
            static double lastCompileMs = 0.0;
            const auto now = std::chrono::steady_clock::now();
            const double frameMs = std::chrono::duration<double, std::milli>(now - lastSwap).count();
            if (frameMs > 200.0)
                std::fprintf(stderr, "[hitch] frame %d took %.0f ms (%.0f ms compiling shaders)\n", g_frame, frameMs,
                    g_shaderCompileMs - lastCompileMs);
            lastSwap = now;
            lastCompileMs = g_shaderCompileMs;
        }
        g_frame++;
        g_drawInFrame = 0;
        if (Renderer* renderer = GetRenderer())
            renderer->Swap(frontBuffer);
    }
}
