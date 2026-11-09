#include "../dev_env.h"
#include "vk_native.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <vector>

#include "../widescreen.h"
#include "common/font_hd.h"
#include "vk_materials.h"
#include "vk_tessellation.h"
#include "vk_motion.h"
#include "vk_scene_effects.h"
#include "vk_shader_compile.h"
#include "xe_glsl.h"
#include "xenos_texture.h"

// The texture cache, samplers, index assembly and per-draw constants follow
// vk_edram.cpp (same decoding, same results); render targets, pipelines and
// resolves are this renderer's own. TODO: share the common parts with vk_edram.

namespace hc09vk {

namespace {

inline uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline int32_t SBits(uint32_t v, int lo, int n) { return int32_t(v << (32 - lo - n)) >> (32 - n); }
inline float AsF(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t Reg(uint32_t i) { return xenos::Reg(i); }

namespace reg = xenos::reg;

TextureOverride g_texOverride;
const uint32_t kColorInfoRegs[4] = { reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO };
const uint32_t kBlendRegs[4] = { reg::RB_BLENDCONTROL0, reg::RB_BLENDCONTROL1, reg::RB_BLENDCONTROL2, reg::RB_BLENDCONTROL3 };

constexpr uint32_t kMaxDraws = 8192;            // per frame slot
constexpr uint32_t kMaxSets = 4096;
constexpr VkDeviceSize kIndexRingBytes = 64ull << 20;
// Index cache (HC09_NATIVE_INDEX_CACHE): triangle-list indices of draws whose index data stays the same, converted
// once, after the slots' rings in the same buffer (one binding). Full: the GPU is waited for and it starts over.
constexpr VkDeviceSize kIndexCacheBytes = 64ull << 20;
// gpu_live's draw-data snapshots (pa 0x01000000-0x01FFFFFF, never allocated by the game) are rewritten host-side
// every frame without write tracking: never cached.
constexpr uint32_t kSnapLo = 0x01000000, kSnapHi = 0x02000000;
// Frames in flight: the CPU records frame n+1 while the GPU draws frame n. Each slot
// has its own command buffers, descriptor pools, constant/index ring regions and the
// staging buffers / images it frees once the GPU is done with it.
constexpr uint32_t kSlots = 2;
constexpr float kClipScale = 8192.0f;   // clip space maps 1:1 to pixels (xe_glsl's vertex shader epilogue)

enum TexKind : uint32_t { kTex2D = 0, kTex3D = 1, kTexCube = 2, kTexKinds = 3, kTexStacked = 3 };

struct ScopeTimer
{
    double& acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~ScopeTimer() { acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};

struct WordsHash
{
    template <class C> size_t operator()(const C& c) const
    {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (auto v : c) { h = (h ^ uint64_t(v)) * 0xFF51AFD7ED558CCDull; h ^= h >> 32; }
        return size_t(h);
    }
    size_t operator()(const std::pair<uint64_t, int>& p) const { return (*this)(std::array<uint64_t, 2>{ p.first, uint64_t(p.second) }); }
};

struct ShaderEntry
{
    VkShaderModule module = VK_NULL_HANDLE;
    TranslatedShader meta;
    bool ok = false;
    // hc09: a vertex shader whose draws are tessellated (vk_tessellation.h): its cut-short copy and the two stages.
    VkShaderModule tessVs = VK_NULL_HANDLE, tessControl = VK_NULL_HANDLE, tessEval = VK_NULL_HANDLE;
};

struct Texture
{
    Image img;
    uint32_t width = 0, height = 0, depth = 1, kind = kTex2D;
    bool stacked = false;
    uint32_t pa = 0, bytes = 0;
    uint64_t epoch = 0;
    uint64_t lastUsed = 0;
    uint32_t extra = 0;   // hc09: mip levels above the game's (a replacement 2^extra times its size)
    uint64_t glyphs = 0;  // hc09: a glyph page's: its sharper copy's version (common/font_hd.h)
};

void MakeMips(std::vector<xenos::Image>& levels, uint32_t maxLevel)
{
    while (levels.size() <= maxLevel)
    {
        const xenos::Image& src = levels.back();
        if (src.width == 1 && src.height == 1) break;
        xenos::Image d;
        d.width = std::max(1u, src.width / 2); d.height = std::max(1u, src.height / 2);
        d.rgba.resize(size_t(d.width) * d.height);
        for (uint32_t y = 0; y < d.height; ++y)
            for (uint32_t x = 0; x < d.width; ++x)
            {
                uint32_t sx0 = std::min(2 * x, src.width - 1), sx1 = std::min(2 * x + 1, src.width - 1);
                uint32_t sy0 = std::min(2 * y, src.height - 1), sy1 = std::min(2 * y + 1, src.height - 1);
                uint32_t p[4] = { src.rgba[size_t(sy0) * src.width + sx0], src.rgba[size_t(sy0) * src.width + sx1],
                                  src.rgba[size_t(sy1) * src.width + sx0], src.rgba[size_t(sy1) * src.width + sx1] };
                uint32_t r = 0;
                for (int c = 0; c < 4; ++c)
                {
                    uint32_t sum = 0;
                    for (uint32_t v : p) sum += (v >> (8 * c)) & 0xFF;
                    r |= ((sum + 2) / 4) << (8 * c);
                }
                d.rgba[size_t(y) * d.width + x] = r;
            }
        levels.push_back(std::move(d));
    }
}

void MemoryBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst)
{
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void ImageLayout(VkCommandBuffer cb, VkImage image, uint32_t levels, uint32_t layers, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkBlendFactor BlendFactor(uint32_t f)
{
    switch (f)
    {
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ZERO;
    }
}
VkBlendOp BlendOp(uint32_t op)
{
    switch (op)
    {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

// Host format of a Xenos colour render-target format (RB_COLOR_INFO bits 16-19).
VkFormat ColorFormat(uint32_t f)
{
    switch (f)
    {
    case 0: case 1: return VK_FORMAT_R8G8B8A8_UNORM;          // k_8_8_8_8 (and _GAMMA: TODO piecewise-linear gamma)
    case 2: case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3: case 12: return VK_FORMAT_R16G16B16A16_SFLOAT;    // 2_10_10_10_FLOAT (7e3): wider
    case 4: return VK_FORMAT_R16G16_SNORM;
    case 5: return VK_FORMAT_R16G16B16A16_SNORM;
    case 6: return VK_FORMAT_R16G16_SFLOAT;
    case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

}  // namespace

struct NativeRenderer::Impl
{
    Device* dev = nullptr;
    Buffer guest, consts, indices;
    RendererStats* stats = nullptr;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderStageFlags drawPushStages = 0;   // the stages the draw index is pushed to
    VkDescriptorPool pool = VK_NULL_HANDLE;
    // Compute: resolves (colour, depth), texture decode and mips (as vk_edram).
    // Resolves: [depth][multisampled].
    VkDescriptorSetLayout resolveSetLayout[2][2] = {}, decodeSetLayout = VK_NULL_HANDLE, mipSetLayout = VK_NULL_HANDLE;
    // k_24_8 depth textures decoded at full precision into R32F (null: the GPU cannot, the RGBA8 high byte instead).
    VkDescriptorSetLayout depthDecodeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout depthDecodePipeLayout = VK_NULL_HANDLE;
    VkPipeline depthDecodePipe = VK_NULL_HANDLE;
    VkPipelineLayout resolvePipeLayout[2][2] = {}, decodePipeLayout = VK_NULL_HANDLE, mipPipeLayout = VK_NULL_HANDLE;
    VkPipeline resolvePipe[2][2] = {}, decodePipe = VK_NULL_HANDLE, mipPipe = VK_NULL_HANDLE;
    // Surfaces get the sample count the game asked for (2x/4x, the console's real
    // MSAA; the 3D scenes use 4x). HC09_NATIVE_MSAA=0: 1x, as the emulated renderers
    // (for comparisons with their reference images); 2: at most 2x (hc09.ini msaa).
    bool msaa = false;
    bool msaaCap2 = false;
    // Resolution scale (HC09_SCALE, hc09.ini scale; NATIVE_RENDERER.md 7.11): render-target images are `scale`
    // times the surfaces' guest size; the game's sizes, coordinates and memory stay 1x.
    uint32_t scale = 1;
    // hc09: anisotropic filtering for mipmapped textures (HC09_ANISOTROPY, hc09.ini Anisotropy; 16 the default, 1 or
    // 0 off), at most what the device allows. The game asks for none, so field lines blur at the broadcast angles.
    float anisotropy = 16.0f;
    VkSampleCountFlagBits boundSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDescriptorPool computePool = VK_NULL_HANDLE;
    uint32_t computeSetsUsed = 0;
    static constexpr uint32_t kComputeSets = 1024;
    VkSampler nearest = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::string pipelineCachePath;
    VkShaderModule rectGs = VK_NULL_HANDLE, nullPs = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
    TranslateOptions translate;
    VkDeviceSize constStride = 0;
    uint32_t constUsed = 0;
    VkDeviceSize indexUsed = 0;
    // The current slot's resources (the others are parked in slots[]).
    VkCommandBuffer cb = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
    bool recording = false, uploading = false, rendering = false;
    bool waitAtCopyResolves = true;
    bool trimConsts = true;   // HC09_NATIVE_TRIM_CONSTS=0: copy every draw's whole constant windows
    std::vector<Buffer> staging;
    std::vector<Image> garbage;
    std::vector<std::pair<uint32_t, uint32_t>> pendingWrites;   // guest ranges resolves write (GPU work not yet waited for)
    struct Slot
    {
        VkCommandBuffer cb = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE, computePool = VK_NULL_HANDLE;
        uint32_t computeSetsUsed = 0;
        std::unordered_map<std::vector<uint64_t>, VkDescriptorSet, WordsHash> descSets;
        std::vector<Buffer> staging;
        std::vector<Image> garbage;
        std::vector<std::pair<uint32_t, uint32_t>> pendingWrites;
        uint64_t value = 0;    // timeline value of the slot's last submit
        bool timed = false;    // its timestamps are still to be read
        // Command buffers for the frame's partial submits (SubmitPartial: the frame goes
        // on recording in the next pair; all are free again once `value` is reached).
        std::vector<VkCommandBuffer> spareCb, spareUp;
        uint32_t tsN = 0;      // timestamps written (HC09_NATIVE_PROF)
        std::vector<uint8_t> tsKinds;   // kind of work between timestamp i and i + 1
        std::vector<int32_t> tsDraw;    // HC09_NATIVE_DRAW_PROF: the draw (index into drawInfo) between i and i + 1, or -1
        std::vector<std::array<uint64_t, 4>> drawInfo;   // vs hash, ps hash, width << 32 | height, samples << 32 | indices
    };
    static constexpr uint32_t kMaxTs = 4096;   // timestamps per slot
    // HC09_NATIVE_DRAW_PROF=1 (implies HC09_NATIVE_PROF): a timestamp after every draw; GPU time summed by shader pair,
    // render area and samples, the top entries printed every 600 frames ([native-draws]).
    bool drawProf = false;
    std::map<std::array<uint64_t, 4>, std::pair<double, uint64_t>> drawTimes;   // key: vs, ps, area, samples -> GPU s, draws
    std::map<std::array<uint64_t, 4>, uint64_t> drawIndices;
    int gpuKind = NativeProfile::kGpuOther;
    // HC09_NATIVE_PROF: a timestamp where the kind of GPU work changes.
    void Mark(int kind)
    {
        Slot& sl = slots[cur];
        if (!tsPool || !recording || kind == gpuKind || sl.tsN + 1 >= kMaxTs) return;
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, kMaxTs * cur + sl.tsN++);
        sl.tsKinds.push_back(uint8_t(gpuKind));
        if (drawProf) sl.tsDraw.push_back(-1);
        gpuKind = kind;
    }
    Slot slots[kSlots];
    uint32_t cur = 0;
    VkDeviceSize constBase = 0, indexBase = 0;   // the current slot's ring regions
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t submitted = 0;   // timeline value of the last submit
    static constexpr uint32_t kSpares = 32;   // partial submits per frame before one waits
    uint32_t nSpare = 0;      // spare command buffers of the current slot in use
    void Park(Slot& sl)
    {
        sl.pool = pool; sl.computePool = computePool; sl.computeSetsUsed = computeSetsUsed;
        std::swap(sl.descSets, descSets); std::swap(sl.staging, staging); std::swap(sl.garbage, garbage);
        std::swap(sl.pendingWrites, pendingWrites);
        lastSet = VK_NULL_HANDLE;
    }
    void Unpark(Slot& sl)
    {
        cb = sl.cb; upload = sl.upload; nSpare = 0; pool = sl.pool; computePool = sl.computePool; computeSetsUsed = sl.computeSetsUsed;
        std::swap(sl.descSets, descSets); std::swap(sl.staging, staging); std::swap(sl.garbage, garbage);
        std::swap(sl.pendingWrites, pendingWrites);
        lastSet = VK_NULL_HANDLE;
        constBase = VkDeviceSize(cur) * constStride * kMaxDraws;
        indexBase = VkDeviceSize(cur) * kIndexRingBytes;
    }
    void WaitValue(uint64_t v)
    {
        if (!v) return;
        VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline;
        wi.pValues = &v;
        vkWaitSemaphores(dev->device, &wi, UINT64_MAX);
    }
    void ReadTimestamps(uint32_t slot)
    {
        Slot& sl = slots[slot];
        std::vector<uint64_t> ts(sl.tsN);
        double period = double(dev->props.limits.timestampPeriod) * 1e-9;
        if (sl.tsN >= 2 && vkGetQueryPoolResults(dev->device, tsPool, kMaxTs * slot, sl.tsN, ts.size() * 8, ts.data(), 8,
                                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            profile.gpuSeconds += double(ts[sl.tsN - 1] - ts[0]) * period;
            for (uint32_t i = 0; i + 1 < sl.tsN && i < sl.tsKinds.size(); ++i)
                if (ts[i + 1] > ts[i]) profile.gpuKind[sl.tsKinds[i]] += double(ts[i + 1] - ts[i]) * period;
            for (uint32_t i = 0; drawProf && i + 1 < sl.tsN && i < sl.tsDraw.size(); ++i)
                if (sl.tsDraw[i] >= 0 && ts[i + 1] >= ts[i])
                {
                    const auto& di = sl.drawInfo[size_t(sl.tsDraw[i])];
                    std::array<uint64_t, 4> key = { di[0], di[1], di[2], di[3] >> 32 };
                    auto& e = drawTimes[key];
                    e.first += double(ts[i + 1] - ts[i]) * period;
                    ++e.second;
                    drawIndices[key] += di[3] & 0xFFFFFFFFu;
                }
        }
        sl.tsN = 0;
        sl.tsKinds.clear();
        sl.tsDraw.clear();
        sl.drawInfo.clear();
    }
    // Texture images by shape, for reuse once retired: an image has an allocation of
    // its own, and creating plus freeing one costs ~0.3 ms, while the textures that
    // resolves rewrite are decoded again every frame (~12 per play frame).
    using ImageKey = std::array<uint32_t, 8>;   // view type, width, height, layers, format, usage, levels, alt format
    std::unordered_map<ImageKey, std::vector<std::pair<Image, uint64_t>>, WordsHash> freeImages;   // with the frame retired
    std::unordered_map<VkImage, ImageKey> imageKeys;   // images NewImage made
    size_t freeCount = 0;
    static constexpr size_t kMaxFreeImages = 96;
    bool NewImage(VkImageViewType vt, uint32_t w, uint32_t h, uint32_t layers, VkFormat format, VkImageUsageFlags usage, Image& out,
                  uint32_t levels, VkFormat alt)
    {
        ImageKey key = { uint32_t(vt), w, h, layers, uint32_t(format), uint32_t(usage), levels, uint32_t(alt) };
        auto it = freeImages.find(key);
        if (it != freeImages.end() && !it->second.empty())
        {
            out = it->second.back().first;
            it->second.pop_back();
            --freeCount;
            return true;
        }
        if (!dev->CreateImage(vt, w, h, layers, format, usage, out, levels, VK_IMAGE_ASPECT_COLOR_BIT, alt)) return false;
        imageKeys[out.image] = key;
        return true;
    }
    // An image the GPU no longer uses: back to the pool (NewImage's) or destroyed. A
    // full pool gives up the image idle the longest (textures the cache evicts must
    // not crowd out the shapes resolves need every frame).
    void RetireImage(Image& img)
    {
        auto it = imageKeys.find(img.image);
        if (it == imageKeys.end()) { dev->DestroyImage(img); return; }
        if (freeCount >= kMaxFreeImages)
        {
            std::vector<std::pair<Image, uint64_t>>* oldestList = nullptr;
            size_t oldestAt = 0;
            for (auto& [k, v] : freeImages)
                for (size_t i = 0; i < v.size(); ++i)
                    if (!oldestList || v[i].second < (*oldestList)[oldestAt].second) { oldestList = &v; oldestAt = i; }
            if (oldestList)
            {
                Image old = (*oldestList)[oldestAt].first;
                oldestList->erase(oldestList->begin() + oldestAt);
                --freeCount;
                imageKeys.erase(old.image);
                dev->DestroyImage(old);
            }
        }
        freeImages[it->second].push_back({ img, frame });
        ++freeCount;
        img = Image{};
    }
    // The current slot's resources free again (its GPU work is done).
    void Recycle()
    {
        for (Buffer& b : staging) dev->DestroyBuffer(b);
        staging.clear();
        constUsed = 0;
        indexUsed = 0;
        vkResetDescriptorPool(dev->device, pool, 0);
        descSets.clear();
        lastSet = VK_NULL_HANDLE;
        for (Image& img : garbage) RetireImage(img);
        garbage.clear();
        pendingWrites.clear();
        if (computeSetsUsed) { vkResetDescriptorPool(dev->device, computePool, 0); computeSetsUsed = 0; }
    }
    std::unordered_map<std::pair<uint64_t, int>, ShaderEntry, WordsHash> shaders;
    std::unordered_map<std::array<uint64_t, 9>, VkPipeline, WordsHash> pipelines;
    std::unordered_map<std::array<uint32_t, 7>, Texture, WordsHash> textures;
    std::unordered_map<uint64_t, VkSampler> samplers;
    // Per fetch slot: the texture and sampler its last fetch constant gave (draws in a row mostly sample the same
    // textures). Valid while no texture has left `textures` since (texGen) and the texture's memory is unwritten
    // (as GetTexture checks). HC09_NATIVE_SLOT_MEMO=0: GetTexture / Sampler for every slot of every draw.
    struct SlotMemo { uint32_t fetch[6] = {}; uint64_t gen = 0; Texture* tex = nullptr; uint8_t filt = 0; VkSampler smp = VK_NULL_HANDLE; };
    SlotMemo slotMemo[32];
    uint64_t texGen = 1;
    bool slotMemoOn = [] { const char* e = Hc09Env("HC09_NATIVE_SLOT_MEMO"); return !e || atoi(e) != 0; }();
    Texture dummy[kTexKinds];
    VkDescriptorImageInfo dummyImages[kTexKinds][32], dummyUImages[kTexKinds][32];
    bool slotTemplate = false;
    uint64_t frame = 0;
    std::vector<std::string> warned;
    std::vector<uint32_t> idxBuf;
    std::vector<uint32_t> idxScratch;
    // Index cache: key = what WriteTriangleIndices reads (the range, its format, offset/clamp, primitive type).
    bool indexCache = true;
    struct IndexKey
    {
        uint32_t base, numIndices, dmaWords, format, off, minI, maxI;
        bool operator==(const IndexKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
    };
    struct IndexKeyHash
    {
        size_t operator()(const IndexKey& k) const
        {
            return WordsHash()(std::array<uint32_t, 7>{ k.base, k.numIndices, k.dmaWords, k.format, k.off, k.minI, k.maxI });
        }
    };
    struct IndexEntry
    {
        uint64_t epoch = 0;
        VkDeviceSize offset = 0;   // in `indices`, after the rings
        bool dynamic = false;      // its memory was rewritten: converted per draw into the ring
        uint64_t lastUsed = 0;     // frame
    };
    std::unordered_map<IndexKey, IndexEntry, IndexKeyHash> indexEntries;
    VkDeviceSize indexCacheUsed = 0;
    uint64_t idxHits = 0, idxConverted = 0, idxDynamic = 0, idxResets = 0;

    // ---- HC09_NATIVE_PROF=1 -----------------------------------------------------------
    bool prof = false;
    NativeProfile profile;
    double excluded = 0;   // GPU wait seconds so far: sections leave them out
    VkQueryPool tsPool = VK_NULL_HANDLE;
    // Section timing: To(s) adds the time since the previous lap, minus the waits in it.
    struct Lap
    {
        Impl& I;
        std::chrono::steady_clock::time_point t;
        double ex;
        explicit Lap(Impl& i) : I(i), ex(i.excluded) { if (I.prof) t = std::chrono::steady_clock::now(); }
        void To(NativeProfile::Section s)
        {
            if (!I.prof) return;
            auto n = std::chrono::steady_clock::now();
            I.profile.sec[s] += std::chrono::duration<double>(n - t).count() - (I.excluded - ex);
            t = n;
            ex = I.excluded;
        }
    };

    // ---- render targets: one image per D3D surface ---------------------------
    struct Target
    {
        Image img;
        VkImageView depthView = VK_NULL_HANDLE, stencilView = VK_NULL_HANDLE;   // depth targets: aspects for the resolve shader
        d3dn::Surface s;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        uint64_t lastUsed = 0;
        bool wide = false;   // hc09: widescreen: holds 3D content this frame (drawn wide), so its resolves do too
    };
    std::unordered_map<uint32_t, Target> targets;   // by surface object address (+ depth flag in bit 0)
    // hc09: ambient occlusion and reflections (vk_scene_effects.h), on the frame's first resolve of the target the
    // 3D scene was drawn into (with its camera from the scene's draws, until then).
    SceneEffects effects;
    // hc09: smooth motion (vk_motion.h): on once the front end gives frame states.
    MotionInterpolator motion;
    bool motionOn = false;
    std::vector<float> motionScratch = std::vector<float>(512 * 4);
    SceneView sceneView;
    const Target* sceneColor = nullptr;
    const Target* sceneDepth = nullptr;
    uint64_t effectsFrame = ~0ull;
    uint64_t gainFrame = ~0ull;   // the frame whose scene made the HDR gain image
    bool frontGain = false;       // the front buffer DecodeFrontBuffer made last is that frame's

    // hc09: the crowd (roadmap 11). The stands are cards (pixel shader kCrowdPs, its fetch slot 1) cut from an atlas
    // of 16 fans the game renders each frame, so every card showing the same fan moves with it. Every other frame
    // the atlas is kept in a ring of kCrowdLayers layers, and each card shows a moment of that history of its own
    // (up to about a second back), mirrored or not and a little lighter or darker (CrowdPatch). hc09.ini
    // Crowd=on (the default) / off, as HC09_CROWD; HC09_CROWD_DUMP=1 (dev): the first captured atlas as
    // build/crowd_atlas.png.
    static constexpr uint64_t kCrowdPs = 0xd77adb6f57ac1922ull;
    static constexpr uint32_t kCrowdLayers = 24, kCrowdLevels = 4;
    bool crowdOn = false;             // Init: the setting, and the device can give the shader gl_PrimitiveID
    uint32_t crowdAtlas = 0;          // the atlas's address, learned from the cards' draws
    Texture crowdHistory;             // the ring (2D array: the cube binding of the slot)
    uint32_t crowdNewest = 0, crowdFilled = 0;
    uint64_t crowdFrame = ~0ull;
    bool crowdDumped = false;

    void CrowdBarrier(uint32_t baseLevel, uint32_t levels, uint32_t layer, uint32_t layers, VkImageLayout from, VkImageLayout to,
                      VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = crowdHistory.img.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseLevel, levels, layer, layers };
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // The atlas's resolve: its pixels (the render target's, before the resolve to memory) into the ring's next layer,
    // with mipmaps.
    void CaptureCrowd(Target* src, const d3dn::ResolveOp& r)
    {
        if (frame == crowdFrame || (frame & 1)) return;
        const uint32_t w = uint32_t(r.x1 - r.x0) * scale, h = uint32_t(r.y1 - r.y0) * scale;
        const uint32_t sx = uint32_t(r.x0) * scale, sy = uint32_t(r.y0) * scale;
        if (!w || !h || sx + w > src->img.width || sy + h > src->img.height) return;
        Image& img = crowdHistory.img;
        if (img.image && (img.width != w || img.height != h || img.format != src->img.format))
        {
            garbage.push_back(img);
            img = Image{};
            crowdFilled = 0;
        }
        const bool fresh = !img.image;
        if (fresh)
        {
            if (!dev->CreateImage(VK_IMAGE_VIEW_TYPE_2D_ARRAY, w, h, kCrowdLayers, src->img.format,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, img, kCrowdLevels))
            {
                img = Image{};
                crowdOn = false;
                Warn("the crowd's history image could not be made; the crowd is drawn as the game draws it");
                return;
            }
            crowdHistory.width = w;
            crowdHistory.height = h;
            crowdHistory.depth = kCrowdLayers;
            crowdHistory.kind = kTexCube;
            crowdNewest = 0;
            crowdFilled = 0;
        }
        crowdFrame = frame;
        EndRendering();
        Begin();
        const uint32_t layer = crowdFilled ? (crowdNewest + 1) % kCrowdLayers : 0;
        MemoryBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (fresh)
            CrowdBarrier(0, kCrowdLevels, 0, kCrowdLayers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        else
            CrowdBarrier(0, kCrowdLevels, layer, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (src->samples != VK_SAMPLE_COUNT_1_BIT)
        {
            VkImageResolve re{};
            re.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            re.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
            re.srcOffset = { int32_t(sx), int32_t(sy), 0 };
            re.extent = { w, h, 1 };
            vkCmdResolveImage(cb, src->img.image, VK_IMAGE_LAYOUT_GENERAL, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &re);
        }
        else
        {
            VkImageCopy co{};
            co.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            co.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
            co.srcOffset = { int32_t(sx), int32_t(sy), 0 };
            co.extent = { w, h, 1 };
            vkCmdCopyImage(cb, src->img.image, VK_IMAGE_LAYOUT_GENERAL, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &co);
        }
        for (uint32_t level = 1; level < kCrowdLevels; ++level)
        {
            CrowdBarrier(level - 1, 1, layer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkImageBlit bl{};
            bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, layer, 1 };
            bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 };
            bl.srcOffsets[1] = { int32_t(std::max(1u, w >> (level - 1))), int32_t(std::max(1u, h >> (level - 1))), 1 };
            bl.dstOffsets[1] = { int32_t(std::max(1u, w >> level)), int32_t(std::max(1u, h >> level)), 1 };
            vkCmdBlitImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                           VK_FILTER_LINEAR);
        }
        CrowdBarrier(0, kCrowdLevels - 1, layer, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        CrowdBarrier(kCrowdLevels - 1, 1, layer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        if (fresh)   // the layers not written yet: sampled only once they are, but kept in the layout the descriptor names
            for (uint32_t l = 0; l < kCrowdLayers; ++l)
                if (l != layer)
                    CrowdBarrier(0, kCrowdLevels, l, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0);
        crowdNewest = layer;
        crowdFilled = std::min(crowdFilled + 1, kCrowdLayers);
        if (!crowdDumped && Hc09Env("HC09_CROWD_DUMP")) DumpCrowd(layer);
    }

    // HC09_CROWD_DUMP (dev): a captured layer, read back (waits for the GPU), as build/crowd_atlas.png.
    void DumpCrowd(uint32_t layer)
    {
        crowdDumped = true;
        const Image& img = crowdHistory.img;
        Buffer buf;
        if (!dev->CreateBuffer(VkDeviceSize(img.width) * img.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf))
            return;
        CrowdBarrier(0, 1, layer, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
        copy.imageExtent = { img.width, img.height, 1 };
        vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &copy);
        CrowdBarrier(0, 1, layer, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        SubmitAndWait();
        xenos::Image out{ img.width, img.height, std::vector<uint32_t>(size_t(img.width) * img.height) };
        memcpy(out.rgba.data(), buf.mapped, out.rgba.size() * 4);
        if (img.format == VK_FORMAT_B8G8R8A8_UNORM)
            for (uint32_t& p : out.rgba) p = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
        xenos::WritePng("build/crowd_atlas.png", out);
        fprintf(stderr, "[crowd] wrote build/crowd_atlas.png (%ux%u, format %d)\n", img.width, img.height, int(img.format));
        dev->DestroyBuffer(buf);
    }

    // The cards' shader: the atlas fetch made xeCrowd, a layer of the history and a mirror and shade per card (the
    // card: its quad, gl_PrimitiveID / 2, in its draw, the index buffer's address hashed into DrawConsts::pad0).
    void CrowdPatch(std::string& glsl)
    {
        const std::string fetch = "texture(tex2d[1], uv, ";
        const size_t at = glsl.find(fetch), mainAt = glsl.find("void main()");
        if (at == std::string::npos || mainAt == std::string::npos || at < mainAt)
        {
            Warn("the crowd's shader is not as expected; the crowd is drawn as the game draws it");
            crowdOn = false;
            return;
        }
        glsl.replace(at, fetch.size(), "xeCrowd(uv, ");
        const std::string fn =
            "// hc09: the crowd (vk_native.cpp, kCrowdPs).\n"
            "vec4 xeCrowd(vec2 uv, float bias)\n"
            "{\n"
            "    uint info = floatBitsToUint(D.pad2);\n"
            "    uint filled = (info >> 8) & 0xFFu;\n"
            "    if (filled == 0u) return texture(tex2d[1], uv, bias);\n"
            "    uint h = (uint(gl_PrimitiveID) >> 1) * 0x9E3779B9u ^ (info >> 16) * 0x85EBCA6Bu;\n"
            "    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;\n"
            "    uint layer = ((info & 0xFFu) + " + std::to_string(kCrowdLayers) + "u - h % filled) % " + std::to_string(kCrowdLayers) + "u;\n"
            "    if (CROWD_MIRROR != 0 && (h & 0x10000u) != 0u) { float cell = floor(uv.x * 16.0); uv.x = (2.0 * cell + 1.0) / 16.0 - uv.x; }\n"
            "    vec4 c = texture(texCube[1], vec3(uv, float(layer)), bias);\n"
            "    c.rgb *= 0.92 + 0.16 * float((h >> 20) & 0xFFu) / 255.0;\n"
            "    if (CROWD_DEBUG != 0)   // each card coloured by how far back it looks\n"
            "    {\n"
            "        float back = float(h % filled) / float(filled);\n"
            "        c.rgb = c.a * clamp(abs(fract(back + vec3(0.0, 0.33, 0.67)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);\n"
            "    }\n"
            "    return c;\n"
            "}\n\n";
        // HC09_CROWD_DEBUG=1 (dev): the cards coloured by their moment of the history.
        glsl.insert(mainAt, "#define CROWD_MIRROR " + std::string(crowdMirror ? "1" : "0") + "\n#define CROWD_DEBUG " +
                                std::string(Hc09Env("HC09_CROWD_DEBUG") ? "1" : "0") + "\n" + fn);
    }
    bool crowdMirror = true;          // per-card mirroring about the card's cell (the atlas: 16 cells in a row, each fan in the middle; HC09_CROWD_MIRROR=0: none)
    // hc09: widescreen (widescreen.h): the squeeze factor (1: off), and the guest ranges that hold resolves of targets
    // with 3D content (Target::wide).
    float squeeze = 1.0f / WidescreenStretch();
    std::vector<std::pair<uint32_t, uint32_t>> wideRanges;
    // A draw of the 3D scenes: its vertex shader reads c156..c159 (the scenes' camera matrix; gpu/scene_camera.h),
    // and they hold a perspective projection.
    bool Perspective(uint32_t vsBase, uint32_t vsSize, const TranslatedShader& m) const
    {
        if (vsSize < 160 || vsBase + 160 > 512) return false;
        bool reads = m.constRelative;
        for (uint32_t i = 156; i < 160 && !reads; ++i) reads = (m.constUsed[i >> 6] >> (i & 63)) & 1;
        if (!reads) return false;
        float c[16];
        for (uint32_t i = 0; i < 16; ++i) c[i] = AsF(Reg(reg::ALU_CONSTANTS + (vsBase + 156) * 4 + i));
        return hc09::gpu::CameraFromConstants(c).valid;
    }
    bool SamplesWide(const xenos::TextureFetch& tf) const
    {
        uint32_t pa = tf.baseAddress() & 0x1FFFFFFFu;
        for (const auto& [base, bytes] : wideRanges)
            if (pa >= base && pa < base + bytes) return true;
        return false;
    }
    d3dn::Surface bound[4], boundDepth;
    Target* bt[4] = {};
    Target* bd = nullptr;
    uint32_t areaW = 0, areaH = 0;   // render area of the bound set
    VkPipeline boundPipe = VK_NULL_HANDLE;
    VkDescriptorSet boundSet = VK_NULL_HANDLE;   // the draw set bound in the command buffer
    // State the command buffer already has (reset with boundPipe: a new command buffer or render pass), so
    // a draw records only what changed: viewport and index buffer once, scissor / blend constants / stencil
    // masks when they differ from the previous draw's.
    struct Recorded
    {
        bool valid = false;
        VkRect2D scissor{};
        float blend[4] = {};
        uint32_t stencilF = 0, stencilB = 0;
    } rec;
    // The previous draw's pipeline key and descriptor set (most draws repeat them).
    static constexpr size_t kPipeKeyWords = 9;
    using PipeKey = std::array<uint64_t, kPipeKeyWords>;
    PipeKey lastPipeKey{};
    VkPipeline lastPipe = VK_NULL_HANDLE;
    std::vector<uint64_t> lastDescKey;
    VkDescriptorSet lastSet = VK_NULL_HANDLE;   // from the current slot's pool (cleared with descSets)

    void Warn(const std::string& what)
    {
        if (std::find(warned.begin(), warned.end(), what) != warned.end()) return;
        warned.push_back(what);
        fprintf(stderr, "[native] %s\n", what.c_str());
    }
    void Skip(const char* why)
    {
        ++stats->skipped;
        ++stats->skipReasons[why];
    }

    // ---- command recording ------------------------------------------------------
    void Begin()
    {
        if (recording) return;
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        recording = true;
        boundPipe = VK_NULL_HANDLE;
        boundSet = VK_NULL_HANDLE;
        rec.valid = false;
        if (tsPool && slots[cur].tsN == 0)   // the slot's first command buffer since its timestamps were read
        {
            vkCmdResetQueryPool(cb, tsPool, kMaxTs * cur, kMaxTs);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, tsPool, kMaxTs * cur);
            slots[cur].tsN = 1;
            slots[cur].tsKinds.clear();
            gpuKind = NativeProfile::kGpuOther;
        }
    }
    VkCommandBuffer UploadCb()
    {
        if (!uploading)
        {
            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResetCommandBuffer(upload, 0);
            vkBeginCommandBuffer(upload, &bi);
            uploading = true;
        }
        return upload;
    }
    void EndRendering()
    {
        if (rendering) vkCmdEndRendering(cb);
        rendering = false;
    }
    // Submit the recorded work; `wait`: wait for it and recycle the slot (mid-frame
    // waits), else (the frame's end) move on to the next slot, waiting only for the
    // frame that used it kSlots frames ago. `why`: the profile's wait reason.
    // `partial`: in the middle of the frame, without waiting: the frame goes on in the
    // slot's next spare command buffers (the slot's rings and sets keep filling).
    void Submit(bool wait, NativeProfile::Section why, bool partial = false)
    {
        EndRendering();
        VkCommandBuffer list[2];
        uint32_t n = 0;
        bool timed = false;
        if (uploading) { vkEndCommandBuffer(upload); list[n++] = upload; uploading = false; }
        if (recording)
        {
            VkMemoryBarrier hb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
            if (tsPool && slots[cur].tsN < kMaxTs)
            {
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, kMaxTs * cur + slots[cur].tsN++);
                slots[cur].tsKinds.push_back(uint8_t(gpuKind));
                timed = true;
            }
            vkEndCommandBuffer(cb);
            list[n++] = cb;
            recording = false;
        }
        if (n)
        {
            uint64_t v = ++submitted;
            VkTimelineSemaphoreSubmitInfo tsi{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            tsi.signalSemaphoreValueCount = 1;
            tsi.pSignalSemaphoreValues = &v;
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.pNext = &tsi;
            si.commandBufferCount = n;
            si.pCommandBuffers = list;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &timeline;
            vkQueueSubmit(dev->queue, 1, &si, VK_NULL_HANDLE);
            slots[cur].value = v;
            slots[cur].timed = slots[cur].timed || timed;
            if (prof) ++profile.submits;
        }
        if (partial)
        {
            Slot& ps = slots[cur];
            cb = ps.spareCb[nSpare];
            upload = ps.spareUp[nSpare];
            ++nSpare;
            return;
        }
        if (!wait)
        {
            Park(slots[cur]);
            cur = (cur + 1) % kSlots;
            Unpark(slots[cur]);
        }
        Slot& sl = slots[cur];
        if (sl.value)
        {
            auto t0 = std::chrono::steady_clock::now();
            WaitValue(sl.value);
            double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            stats->waitSeconds += w;
            ++stats->syncs;
            if (prof)
            {
                profile.sec[why] += w;
                ++profile.waits[why];
                excluded += w;
                if (sl.timed) ReadTimestamps(cur);
            }
            sl.value = 0;
            sl.timed = false;
        }
        cb = sl.cb;   // every command buffer of the slot is free again
        upload = sl.upload;
        nSpare = 0;
        Recycle();
    }
    void SubmitAndWait(NativeProfile::Section why = NativeProfile::kWaitFrame) { Submit(true, why); }
    void SubmitPartial()
    {
        if (nSpare < kSpares) Submit(false, NativeProfile::kWaitFence, true);
        else SubmitAndWait(NativeProfile::kWaitFence);
    }
    // All submitted work done (offline readers, CPU reads of resolved memory).
    void Finish()
    {
        WaitValue(submitted);
        for (uint32_t i = 0; i < kSlots; ++i)
            if (i != cur) slots[i].pendingWrites.clear();
    }
    std::unordered_map<std::vector<uint64_t>, VkDescriptorSet, WordsHash> descSets;
    std::vector<uint64_t> descKeyScratch;

    static bool Overlaps(const std::vector<std::pair<uint32_t, uint32_t>>& w, uint32_t pa, uint32_t bytes)
    {
        for (const auto& [p, n] : w)
            if (pa < p + n && p < pa + bytes) return true;
        return false;
    }
    // Before the CPU reads guest memory a resolve may still be writing: wait for it.
    void WaitForWrites(uint32_t pa, uint32_t bytes)
    {
        for (uint32_t i = 0; i < kSlots; ++i)
            if (i != cur && Overlaps(slots[i].pendingWrites, pa, bytes))
            {
                auto t0 = std::chrono::steady_clock::now();
                WaitValue(slots[i].value);
                double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (prof) { profile.sec[NativeProfile::kWaitTexture] += w; ++profile.waits[NativeProfile::kWaitTexture]; excluded += w; }
                slots[i].pendingWrites.clear();
            }
        if (Overlaps(pendingWrites, pa, bytes)) SubmitAndWait(NativeProfile::kWaitTexture);
    }

    // ---- resolved memory kept on the GPU (HC09_NATIVE_RESOLVE_GPU, default on) --------
    // Resolves into 8888 textures (the format texture decode runs on the GPU) write
    // `vram`, a device-local copy of guest physical memory at the same addresses,
    // instead of guest memory in system RAM: writing guest memory over PCIe and
    // reading it back for the texture decodes and the front buffer was two thirds of
    // the GPU's time. Such a range is GPU-owned: its newest data is in vram. GPU
    // decodes and the front buffer read vram; before anything reads the range on the
    // CPU (a CPU texture decode, frame dumps), it is written back to guest memory
    // (Release). A CPU write to a GPU-owned range (write tracking) ends the ownership:
    // the pages the CPU did not write are written back, the CPU's stay.
    Buffer vram;
    bool resolveGpu = false;
    struct Owned { uint32_t pa, bytes; uint64_t epoch; };
    std::vector<Owned> owned;
    void (*ownedCallback)(uint32_t, uint32_t) = nullptr;
    void CopyRanges(const Buffer& from, const Buffer& to, const std::vector<VkBufferCopy>& regions)
    {
        if (regions.empty()) return;
        EndRendering();
        Begin();
        MemoryBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBuffer(cb, from.buffer, to.buffer, uint32_t(regions.size()), regions.data());
        MemoryBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    // Ownership of owned[i] ends: its pages the CPU has not written since go back to guest memory.
    void Release(size_t i)
    {
        Owned o = owned[i];
        owned.erase(owned.begin() + i);
        std::vector<VkBufferCopy> regions;
        for (uint32_t p = o.pa & ~0xFFFu; p < o.pa + o.bytes; p += 0x1000)
        {
            uint32_t lo = std::max(p, o.pa), hi = std::min(p + 0x1000, o.pa + o.bytes);
            uint64_t e = o.epoch;
            if (xenos::HostWrittenSince(lo, hi - lo, e)) continue;   // the CPU's data is newer
            if (!regions.empty() && regions.back().srcOffset + regions.back().size == lo) regions.back().size += hi - lo;
            else regions.push_back({ lo, lo, hi - lo });
        }
        CopyRanges(vram, guest, regions);
        pendingWrites.emplace_back(o.pa, o.bytes);
        ++stats->resolveWritebacks;
    }
    // Before the CPU reads [pa, pa + bytes): write back the GPU-owned data in it.
    void ReleaseOverlapping(uint32_t pa, uint32_t bytes)
    {
        for (size_t i = owned.size(); i-- > 0;)
            if (pa < owned[i].pa + owned[i].bytes && owned[i].pa < pa + bytes) Release(i);
    }
    // Is [pa, pa + bytes) newest in vram? Ownership that no longer holds (partial
    // overlap, CPU writes since) ends here.
    bool InVram(uint32_t pa, uint32_t bytes)
    {
        bool inside = false;
        for (size_t i = owned.size(); i-- > 0;)
        {
            Owned& o = owned[i];
            if (!(pa < o.pa + o.bytes && o.pa < pa + bytes)) continue;
            uint64_t e = o.epoch;
            if (pa >= o.pa && pa + bytes <= o.pa + o.bytes && !xenos::HostWrittenSince(o.pa, o.bytes, e)) { inside = true; continue; }
            Release(i);
        }
        return inside && !Overlaps(pendingWrites, pa, bytes);
    }
    // A resolve is about to write [pa, pa + bytes) in vram: the range becomes GPU-owned;
    // newly owned memory is first copied from guest memory (the resolve may write only
    // part of it).
    void Acquire(uint32_t pa, uint32_t bytes)
    {
        for (const Owned& o : owned)
        {
            uint64_t e = o.epoch;
            if (pa >= o.pa && pa + bytes <= o.pa + o.bytes && !xenos::HostWrittenSince(o.pa, o.bytes, e)) return;
        }
        ReleaseOverlapping(pa, bytes);
        WaitForWrites(pa, bytes);   // guest memory complete before it is copied up
        CopyRanges(guest, vram, { { pa, pa, bytes } });
        owned.push_back({ pa, bytes, xenos::HostTrackWrites(pa, bytes) });
        ++stats->resolveAcquires;
        if (ownedCallback) ownedCallback(pa, bytes);
    }
    // ---- scaled copies of resolve destinations (resolution scale > 1, NATIVE_RENDERER.md 7.11) ------------
    // Every 8888 resolve at scale s also writes its destination texture at s times its size: the words the
    // resolve writes into guest memory (format, exponent bias, R/B swap, endian), untiled, (pitch * s) x
    // (height * s), in a device buffer of its own. The front buffer (and whole-texture fetches) decode it
    // instead of the guest copy. Valid while the CPU has not written the texture's guest range (epoch) and no
    // resolve without a scaled copy wrote over it; `cover` marks the 8x8 blocks resolves wrote.
    struct ScaledCopy
    {
        Buffer buf;
        uint32_t base = 0, pitch = 0, height = 0, bytes = 0;   // guest texture: base, pitch (texels), height; its tiled range
        uint64_t epoch = 0;
        std::vector<uint8_t> cover;   // (pitch / 8) x ceil(height / 8) blocks
        uint64_t lastUsed = 0;
    };
    std::unordered_map<uint32_t, ScaledCopy> scaledCopies;   // by guest base
    VkDeviceSize scaledBytes = 0;
    bool scaledFetch = true;   // HC09_NATIVE_SCALED_FETCH=0: texture fetches decode the 1x guest copy (the front buffer still scales)
    uint64_t scaledFetches = 0;
    void DropScaled(std::unordered_map<uint32_t, ScaledCopy>::iterator it)
    {
        scaledBytes -= it->second.buf.size;
        staging.push_back(it->second.buf);   // destroyed once the slot's GPU work is done
        scaledCopies.erase(it);
    }
    // A resolve wrote [pa, pa + bytes) without updating these copies (other than `keep`): they are stale.
    void DropScaledOverlapping(uint32_t pa, uint32_t bytes, const ScaledCopy* keep)
    {
        for (auto it = scaledCopies.begin(); it != scaledCopies.end();)
        {
            auto next = std::next(it);
            if (&it->second != keep && it->second.base < pa + bytes && pa < it->second.base + it->second.bytes) DropScaled(it);
            it = next;
        }
    }
    // The scaled copy a resolve writes (nullptr: none - another format, no device memory).
    ScaledCopy* ScaledForResolve(const d3dn::ResolveOp& r)
    {
        if (scale <= 1 || r.destFmt != 6 || r.destBytes != 4 || !r.texHeight || !r.destPitch || (r.destPitch & 7)) return nullptr;
        auto it = scaledCopies.find(r.texBase);
        if (it != scaledCopies.end())
        {
            ScaledCopy& c = it->second;
            if (c.pitch == r.destPitch && c.height == r.texHeight)
            {
                if (xenos::HostWrittenSince(c.base, c.bytes, c.epoch))   // the CPU wrote it since: none of the copy is current
                {
                    std::fill(c.cover.begin(), c.cover.end(), uint8_t(0));
                    c.epoch = xenos::HostTrackWrites(c.base, c.bytes);
                }
                c.lastUsed = frame;
                return &c;
            }
            DropScaled(it);
        }
        ScaledCopy c;
        c.base = r.texBase;
        c.pitch = r.destPitch;
        c.height = r.texHeight;
        c.bytes = r.destPitch * ((r.texHeight + 31) & ~31u) * 4;
        VkDeviceSize size = VkDeviceSize(r.destPitch) * scale * r.texHeight * scale * 4;
        if (!dev->CreateBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, c.buf))
        {
            Warn("no device memory for a scaled copy: resolves stay at 1x");
            return nullptr;
        }
        c.cover.assign(size_t(r.destPitch / 8) * ((r.texHeight + 7) / 8), 0);
        c.epoch = xenos::HostTrackWrites(c.base, c.bytes);
        c.lastUsed = frame;
        scaledBytes += c.buf.size;
        return &(scaledCopies[r.texBase] = std::move(c));
    }
    // A valid scaled copy holding the w x h texels from the texture at `base` (pitch in texels), or nullptr.
    const ScaledCopy* ScaledFor(uint32_t base, uint32_t pitch, uint32_t w, uint32_t h)
    {
        if (scale <= 1) return nullptr;
        auto it = scaledCopies.find(base);
        if (it == scaledCopies.end()) return nullptr;
        ScaledCopy& c = it->second;
        if (c.pitch != pitch || w > c.pitch || h > c.height) return nullptr;
        if (xenos::HostWrittenSince(c.base, c.bytes, c.epoch)) { DropScaled(it); return nullptr; }
        uint32_t bw = c.pitch / 8;
        for (uint32_t by = 0; by < (h + 7) / 8; ++by)
            for (uint32_t bx = 0; bx < (w + 7) / 8; ++bx)
                if (!c.cover[size_t(by) * bw + bx]) return nullptr;
        c.lastUsed = frame;
        return &c;
    }

    VkDescriptorSet ComputeSet(VkDescriptorSetLayout layout)
    {
        if (computeSetsUsed >= kComputeSets) SubmitAndWait(NativeProfile::kWaitCompute);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = computePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev->device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
        ++computeSetsUsed;
        return set;
    }

    // ---- targets ------------------------------------------------------------------
    Target* GetTarget(const d3dn::Surface& s)
    {
        if (!s.valid()) return nullptr;
        uint32_t key = s.key | (s.depth ? 1u : 0u);
        VkSampleCountFlagBits samples = !msaa ? VK_SAMPLE_COUNT_1_BIT : s.msaa == 2 ? (msaaCap2 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_4_BIT)
                                      : s.msaa == 1 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
        auto it = targets.find(key);
        if (it != targets.end())
        {
            Target& t = it->second;
            // Grows (a surface bound with a larger tiling extent) or changes format: a new image.
            if (t.s.format == s.format && t.samples == samples && t.img.width >= s.width * scale && t.img.height >= s.height * scale)
            { t.lastUsed = frame; return &t; }
            EndRendering();
            garbage.push_back(t.img);
            if (t.depthView) vkDestroyImageView(dev->device, t.depthView, nullptr);
            if (t.stencilView) vkDestroyImageView(dev->device, t.stencilView, nullptr);
            targets.erase(it);
        }
        Target& t = targets[key];
        t.s = s;
        t.samples = samples;
        t.lastUsed = frame;
        uint32_t w = std::max(1u, s.width) * scale, h = std::max(1u, s.height) * scale;
        if (s.depth)
        {
            t.format = depthFormat;
            if (!dev->CreateImage(VK_IMAGE_VIEW_TYPE_2D, w, h, 1, t.format,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  t.img, 1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_FORMAT_UNDEFINED, samples))
            { Warn("depth target creation failed"); targets.erase(key); return nullptr; }
            VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = t.img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = t.format;
            vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            vkCreateImageView(dev->device, &vi, nullptr, &t.depthView);
            vi.subresourceRange = { VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
            vkCreateImageView(dev->device, &vi, nullptr, &t.stencilView);
        }
        else
        {
            t.format = ColorFormat(s.format);
            if (s.format > 1) Warn("colour target format " + std::to_string(s.format) + " (host format " + std::to_string(t.format) + ")");
            if (!dev->CreateImage(VK_IMAGE_VIEW_TYPE_2D, w, h, 1, t.format,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  t.img, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_FORMAT_UNDEFINED, samples))
            { Warn("colour target creation failed"); targets.erase(key); return nullptr; }
        }
        if (samples != VK_SAMPLE_COUNT_1_BIT)
            Warn("multisampled target " + std::to_string(w) + "x" + std::to_string(h) + " x" + std::to_string(int(samples)) +
                 (s.depth ? " (depth)" : " (format " + std::to_string(s.format) + ")"));
        // New targets start zeroed (as the EDRAM model at the start of a frame). All
        // render-target images stay in GENERAL layout (attachments, copies, resolves).
        EndRendering();
        Begin();
        VkImageAspectFlags aspect = s.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.img.image;
        b.subresourceRange = { aspect, 0, 1, 0, 1 };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
        if (s.depth)
        {
            VkClearDepthStencilValue z{ 0.0f, 0 };
            vkCmdClearDepthStencilImage(cb, t.img.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
        }
        else
        {
            VkClearColorValue c{};
            vkCmdClearColorImage(cb, t.img.image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
        }
        MemoryBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        return &t;
    }

    // HC09_NATIVE_TRACE_PASSES=<frame>[,<count>]: the target sets, draws and resolves of those frames (dev).
    struct TracePasses { uint64_t from = ~0ull, to = 0; };
    TracePasses tracePasses = [] {
        TracePasses t;
        if (const char* e = Hc09Env("HC09_NATIVE_TRACE_PASSES"))
        {
            unsigned long long f = 0, n = 1;
            if (sscanf(e, "%llu,%llu", &f, &n) >= 1) { t.from = f; t.to = f + n; }
        }
        return t;
    }();
    bool Tracing() const { return frame >= tracePasses.from && frame < tracePasses.to; }
    uint32_t traceDraws = 0, traceDepthOnly = 0, traceSqueezed = 0;
    uint64_t hudFrame = ~0ull;
    uint32_t hudDraws = 0;
    void TraceFlushDraws()
    {
        if (traceDraws) fprintf(stderr, "[passes]   %u draws (%u depth only, %u squeezed)\n", traceDraws, traceDepthOnly, traceSqueezed);
        traceDraws = traceDepthOnly = traceSqueezed = 0;
    }

    void SetTargets(const d3dn::Surface rt[4], const d3dn::Surface& depth)
    {
        bool same = depth == boundDepth;
        for (int i = 0; i < 4; ++i) same = same && rt[i] == bound[i];
        if (same && (bd || !depth.valid())) return;
        if (Tracing())
        {
            TraceFlushDraws();
            std::string s = "[passes] frame " + std::to_string(frame) + " targets:";
            for (int i = 0; i < 4; ++i)
                if (rt[i].valid())
                    s += " c" + std::to_string(i) + "=" + std::to_string(rt[i].width) + "x" + std::to_string(rt[i].height) + " f" +
                         std::to_string(rt[i].format) + " ms" + std::to_string(rt[i].msaa) + " @" + std::to_string(rt[i].edramBase);
            if (depth.valid())
                s += " z=" + std::to_string(depth.width) + "x" + std::to_string(depth.height) + " ms" + std::to_string(depth.msaa) + " @" +
                     std::to_string(depth.edramBase);
            fprintf(stderr, "%s\n", s.c_str());
        }
        EndRendering();
        for (int i = 0; i < 4; ++i) { bound[i] = rt[i]; bt[i] = GetTarget(rt[i]); }
        boundDepth = depth;
        bd = GetTarget(depth);
        // All attachments of a pass share one sample count: the first bound target's;
        // a target with another count is left out (warned; not seen so far).
        boundSamples = VK_SAMPLE_COUNT_1_BIT;
        bool first = true;
        for (Target** t : { &bt[0], &bt[1], &bt[2], &bt[3], &bd })
        {
            if (!*t) continue;
            if (first) { boundSamples = (*t)->samples; first = false; }
            else if ((*t)->samples != boundSamples) { Warn("bound targets with different sample counts"); *t = nullptr; }
        }
        areaW = areaH = 0;
        for (int i = 0; i < 4; ++i)
            if (bt[i]) { areaW = areaW ? std::min(areaW, bt[i]->img.width) : bt[i]->img.width; areaH = areaH ? std::min(areaH, bt[i]->img.height) : bt[i]->img.height; }
        if (bd) { areaW = areaW ? std::min(areaW, bd->img.width) : bd->img.width; areaH = areaH ? std::min(areaH, bd->img.height) : bd->img.height; }
    }

    // Dynamic rendering over the bound targets (load and store everything).
    bool BeginRenderingBound()
    {
        if (rendering) return true;
        if (!areaW || !areaH) return false;
        Begin();
        MemoryBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkRenderingAttachmentInfo ca[4]{};
        uint32_t n = 0;
        for (int i = 0; i < 4; ++i)
        {
            ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            if (bt[i])
            {
                ca[i].imageView = bt[i]->img.view;
                ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                n = uint32_t(i) + 1;
            }
        }
        VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        if (bd)
        {
            da.imageView = bd->img.view;
            da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea = { { 0, 0 }, { areaW, areaH } };
        ri.layerCount = 1;
        ri.colorAttachmentCount = n;
        ri.pColorAttachments = ca;
        ri.pDepthAttachment = bd ? &da : nullptr;
        ri.pStencilAttachment = bd ? &da : nullptr;
        vkCmdBeginRendering(cb, &ri);
        rendering = true;
        boundPipe = VK_NULL_HANDLE;
        boundSet = VK_NULL_HANDLE;
        rec.valid = false;
        return true;
    }

    void Clear(uint32_t flags, const int32_t* rect, const float color[4], float z, uint32_t stencil)
    {
        if (!BeginRenderingBound()) { Skip("clear without targets"); return; }
        VkClearAttachment att[5];
        uint32_t n = 0;
        for (uint32_t i = 0; i < 4; ++i)
            if ((flags & (1u << i)) && bt[i])
            {
                att[n] = {};
                att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                att[n].colorAttachment = i;
                for (int k = 0; k < 4; ++k) att[n].clearValue.color.float32[k] = color[k];
                ++n;
            }
        VkImageAspectFlags ds = ((flags & 0x10) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((flags & 0x20) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (ds && bd)
        {
            att[n] = {};
            att[n].aspectMask = ds;
            att[n].clearValue.depthStencil = { std::clamp(z, 0.0f, 1.0f), stencil & 0xFF };
            ++n;
        }
        if (!n) return;
        int32_t sc = int32_t(scale);   // rectangles in guest pixels
        int32_t x0 = rect ? std::max(0, rect[0] * sc) : 0, y0 = rect ? std::max(0, rect[1] * sc) : 0;
        int32_t x1 = rect ? std::min<int32_t>(rect[2] * sc, int32_t(areaW)) : int32_t(areaW);
        // hc09: widescreen (widescreen.h): a clear of part of a screen-sized target moves with the squeezed draws.
        if (rect && squeeze != 1.0f && bt[0] && bound[0].width >= 1024 && bound[0].height >= 576)
        {
            const int32_t w = int32_t(bound[0].width) * sc;
            const float mid = 0.5f * float(w);
            if (x0 > 0) x0 = int32_t(std::floor(mid + (float(x0) - mid) * squeeze));
            if (x1 < w) x1 = int32_t(std::ceil(mid + (float(x1) - mid) * squeeze));
        }
        int32_t y1 = rect ? std::min<int32_t>(rect[3] * sc, int32_t(areaH)) : int32_t(areaH);
        if (x1 <= x0 || y1 <= y0) return;
        static const bool traceDrawsOn = Hc09Env("HC09_NATIVE_TRACE_DRAWS") != nullptr;
        if (traceDrawsOn && Tracing())
            fprintf(stderr, "[draw] clear flags %X rect %d,%d-%d,%d colour %.2f %.2f %.2f %.2f\n", flags, x0, y0, x1, y1, color[0], color[1], color[2],
                    color[3]);
        VkClearRect cr{ { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } }, 0, 1 };
        Mark(NativeProfile::kGpuOther);
        vkCmdClearAttachments(cb, n, att, 1, &cr);
    }

    // ---- shaders, pipelines, textures ------------------------------------------
    VkShaderModule Module(const std::string& glsl, Stage stage, const char* name)
    {
        std::vector<uint32_t> spirv;
        std::string log;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = CompileGlsl(glsl, stage, name, spirv, &log);
        stats->compileSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!ok) { Warn(std::string("shader ") + name + " failed: " + log); return VK_NULL_HANDLE; }
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spirv.size() * 4;
        ci.pCode = spirv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev->device, &ci, nullptr, &m);
        return m;
    }
    ShaderEntry* Shader(uint64_t hash, const xenos::ShaderCode& code)
    {
        bool vs = code.type == xenos::ShaderType::kVertex;
        auto key = std::make_pair(hash, vs ? 0 : 1);
        auto it = shaders.find(key);
        if (it != shaders.end()) return it->second.ok ? &it->second : nullptr;
        ShaderEntry& e = shaders[key];
        e.meta = TranslateShader(code, translate);
        if (!vs && hash == kCrowdPs && crowdOn && e.meta.error.empty()) CrowdPatch(e.meta.glsl);
        if (!vs && e.meta.error.empty()) MaterialPatch(hash, e.meta.glsl);
        char name[64];
        snprintf(name, sizeof(name), "native_%s_%016llx", vs ? "vs" : "ps", (unsigned long long)hash);
        if (!e.meta.error.empty()) { Warn(std::string("shader ") + name + " failed: " + e.meta.error); return nullptr; }
        // HC09_NATIVE_SHADER_DIR=<dir>: write every translated shader there as <name>.glsl (dev).
        if (const char* dir = Hc09Env("HC09_NATIVE_SHADER_DIR"))
            if (FILE* f = fopen((std::string(dir) + "/" + name + ".glsl").c_str(), "wb"))
            {
                fwrite(e.meta.glsl.data(), 1, e.meta.glsl.size(), f);
                fclose(f);
            }
        e.module = Module(e.meta.glsl, vs ? Stage::kVertex : Stage::kFragment, name);
        if (!e.module) return nullptr;
        if (vs && dev->tessellation && TessellatedVertexShader(hash))
        {
            std::string tvs, tcs, tes;
            if (TessellationShaders(hash, e.meta.glsl, tvs, tcs, tes))
            {
                if (const char* dir = Hc09Env("HC09_NATIVE_SHADER_DIR"))
                    for (auto [suffix, text] : { std::pair<const char*, const std::string*>{ "_tvs", &tvs }, { "_tcs", &tcs }, { "_tes", &tes } })
                        if (FILE* f = fopen((std::string(dir) + "/" + name + suffix + ".glsl").c_str(), "wb"))
                        {
                            fwrite(text->data(), 1, text->size(), f);
                            fclose(f);
                        }
                const std::string base = name;
                e.tessVs = Module(tvs, Stage::kVertex, (base + "_tvs").c_str());
                e.tessControl = Module(tcs, Stage::kTessControl, (base + "_tcs").c_str());
                e.tessEval = Module(tes, Stage::kTessEval, (base + "_tes").c_str());
                if (!e.tessVs || !e.tessControl || !e.tessEval) e.tessVs = e.tessControl = e.tessEval = VK_NULL_HANDLE;
            }
        }
        if (e.meta.unsupported) Warn(std::string("shader ") + name + " uses an untranslated vertex format");
        e.ok = true;
        ++stats->shaders;
        return &e;
    }

    // Pipeline state: everything that is not dynamic, as one key.
    VkPipeline Pipeline(const std::array<uint64_t, 9>& key, VkShaderModule vsMod, VkShaderModule psMod, bool rect, uint32_t cull,
                        uint32_t frontFace, uint64_t texKinds, const VkFormat colorFmt[4], const uint32_t writeMask[4],
                        const uint32_t blend[4], bool hasDepth, uint32_t dc, bool zEnable, bool stencilOn, const ShaderEntry* tess = nullptr)
    {
        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second;
        uint32_t kinds[2] = { uint32_t(texKinds), uint32_t(texKinds >> 32) };
        VkSpecializationMapEntry me[2] = { { 0, 0, 4 }, { 1, 4, 4 } };
        VkSpecializationInfo spec{ 2, me, sizeof(kinds), kinds };
        VkPipelineShaderStageCreateInfo st[4]{};
        uint32_t n = 0;
        st[n++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, tess ? tess->tessVs : vsMod, "main", &spec };
        if (tess)
        {
            st[n++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tess->tessControl, "main", &spec };
            st[n++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, tess->tessEval, "main", &spec };
        }
        if (rect) st[n++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT, rectGs, "main" };
        st[n++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psMod, "main", &spec };
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineTessellationStateCreateInfo ts{ VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
        ts.patchControlPoints = 3;   // the draws' triangles (all are lists by now)
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.depthClampEnable = VK_TRUE;   // near/far clipping by the shader's clip distances
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = cull;
        rs.frontFace = VkFrontFace(frontFace);
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = boundSamples;
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        if (hasDepth)
        {
            ds.depthTestEnable = zEnable;
            ds.depthWriteEnable = zEnable && ((dc >> 2) & 1);
            ds.depthCompareOp = VkCompareOp((dc >> 4) & 7);
            ds.stencilTestEnable = stencilOn;
            bool twoSided = (dc >> 7) & 1;
            uint32_t f[4], b[4];
            for (int i = 0; i < 4; ++i) { f[i] = Bits(dc, 8 + 3 * i, 3); b[i] = twoSided ? Bits(dc, 20 + 3 * i, 3) : f[i]; }
            // Xenos and Vulkan number compare functions and stencil ops alike.
            ds.front = { VkStencilOp(f[1]), VkStencilOp(f[2]), VkStencilOp(f[3]), VkCompareOp(f[0]), 0, 0, 0 };
            ds.back = { VkStencilOp(b[1]), VkStencilOp(b[2]), VkStencilOp(b[3]), VkCompareOp(b[0]), 0, 0, 0 };
        }
        VkPipelineColorBlendAttachmentState att[4]{};
        uint32_t nAtt = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (colorFmt[i] == VK_FORMAT_UNDEFINED) continue;
            nAtt = uint32_t(i) + 1;
            uint32_t bc = blend[i];
            VkPipelineColorBlendAttachmentState& a = att[i];
            a.colorWriteMask = writeMask[i];
            a.srcColorBlendFactor = BlendFactor(Bits(bc, 0, 5));
            a.colorBlendOp = BlendOp(Bits(bc, 5, 3));
            a.dstColorBlendFactor = BlendFactor(Bits(bc, 8, 5));
            a.srcAlphaBlendFactor = BlendFactor(Bits(bc, 16, 5));
            a.alphaBlendOp = BlendOp(Bits(bc, 21, 3));
            a.dstAlphaBlendFactor = BlendFactor(Bits(bc, 24, 5));
            a.blendEnable = !(Bits(bc, 0, 5) == 1 && Bits(bc, 8, 5) == 0 && Bits(bc, 5, 3) == 0 && Bits(bc, 16, 5) == 1 &&
                              Bits(bc, 24, 5) == 0 && Bits(bc, 21, 3) == 0);
        }
        VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cbs.attachmentCount = nAtt;
        cbs.pAttachments = att;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                 VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
        VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 6;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        ri.colorAttachmentCount = nAtt;
        ri.pColorAttachmentFormats = colorFmt;
        ri.depthAttachmentFormat = hasDepth ? depthFormat : VK_FORMAT_UNDEFINED;
        ri.stencilAttachmentFormat = hasDepth ? depthFormat : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.pNext = &ri;
        ci.stageCount = n;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        if (tess) ci.pTessellationState = &ts;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cbs;
        ci.pDynamicState = &dy;
        ci.layout = pipeLayout;
        VkPipeline p = VK_NULL_HANDLE;
        auto t0 = std::chrono::steady_clock::now();
        if (vkCreateGraphicsPipelines(dev->device, pipelineCache, 1, &ci, nullptr, &p) != VK_SUCCESS) Warn("pipeline creation failed");
        stats->pipelineSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        pipelines[key] = p;
        ++stats->pipelines;
        return p;
    }

    bool DecodeOnGpu(Texture& t, const xenos::TextureFetch& tf)
    {
        uint32_t w = tf.width(), h = tf.height();
        uint32_t maxLevel = Bits(tf.dword[4], 6, 4), levels = 1;
        for (uint32_t lw = w, lh = h; levels <= maxLevel && !(lw == 1 && lh == 1); ++levels) { lw = std::max(1u, lw / 2); lh = std::max(1u, lh / 2); }
        if (computeSetsUsed + levels > kComputeSets) return false;
        uint32_t ox0, oy0;
        xenos::PackedBaseOffset(tf, ox0, oy0);
        // Resolution scale: a fetch of a whole resolved texture decodes its scaled copy into an image s times its
        // size (the Texture keeps the guest size: the shaders' texel units, D.texSize).
        const ScaledCopy* sc = scaledFetch && tf.tiled() && !ox0 && !oy0
                                   ? ScaledFor(tf.baseAddress() & 0x1FFFFFFFu, std::max(w, tf.pitchTexels()), w, h) : nullptr;
        uint32_t iw = sc ? w * scale : w, ih = sc ? h * scale : h;
        // A depth texture (k_24_8) is read as the whole 24-bit depth: R32F, one level (the mip shader is RGBA8).
        bool depth = tf.format() == 22 && depthDecodePipe && levels == 1;
        VkFormat fmt = depth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        if (!NewImage(VK_IMAGE_VIEW_TYPE_2D, iw, ih, 1, fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, t.img,
                      levels, depth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UINT))
            return false;
        for (uint32_t l = uint32_t(t.img.levelViews.size()); l < levels; ++l)   // a reused image has them
        {
            VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = t.img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = depth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UINT;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1 };
            VkImageView v = VK_NULL_HANDLE;
            if (vkCreateImageView(dev->device, &vi, nullptr, &v) != VK_SUCCESS) { RetireImage(t.img); return false; }
            t.img.levelViews.push_back(v);
        }
        VkDescriptorSet set = ComputeSet(depth ? depthDecodeSetLayout : decodeSetLayout);
        if (!set) { RetireImage(t.img); return false; }
        uint32_t pitchA = tf.tiled() ? (std::max(w, tf.pitchTexels()) + 31) & ~31u : std::max(w, tf.pitchTexels());
        uint32_t rowsA = tf.tiled() ? (h + oy0 + 31) & ~31u : h + oy0;
        bool fromVram = !sc && resolveGpu && InVram(tf.baseAddress() & 0x1FFFFFFFu, pitchA * rowsA * 4);
        VkDescriptorBufferInfo bMem{ sc ? sc->buf.buffer : fromVram ? vram.buffer : guest.buffer, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo img{ VK_NULL_HANDLE, t.img.levelViews[0], VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr[2]{};
        for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = set; x.descriptorCount = 1; }
        wr[0].dstBinding = 0; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[0].pBufferInfo = &bMem;
        wr[1].dstBinding = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = &img;
        vkUpdateDescriptorSets(dev->device, 2, wr, 0, nullptr);
        uint32_t pitchBlocks = std::max(w, tf.pitchTexels());
        uint32_t ox, oy;
        xenos::PackedBaseOffset(tf, ox, oy);
        uint32_t pc[11] = { tf.baseAddress(), tf.tiled() ? (pitchBlocks + 31) & ~31u : pitchBlocks,
                            (pitchBlocks * 4 + 255) & ~255u, tf.tiled() ? 1u : 0u, w, h, tf.endian(), tf.swizzle(), tf.format(), ox, oy };
        if (sc) { pc[0] = 0; pc[1] = sc->pitch * scale; pc[2] = sc->pitch * scale * 4; pc[3] = 0; pc[4] = iw; pc[5] = ih; }   // linear, scaled
        EndRendering();
        Begin();
        MemoryBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.img.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        Mark(NativeProfile::kGpuDecodes);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, depth ? depthDecodePipe : decodePipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, depth ? depthDecodePipeLayout : decodePipeLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, depth ? depthDecodePipeLayout : decodePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cb, (iw + 7) / 8, (ih + 7) / 8, 1);
        uint32_t sw = iw, sh = ih;
        for (uint32_t l = 1; l < levels; ++l)
        {
            uint32_t lw = std::max(1u, sw / 2), lh = std::max(1u, sh / 2);
            VkImageMemoryBarrier mb = b;
            mb.oldLayout = mb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            mb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 1, 0, 1 };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &mb);
            VkDescriptorSet ms = ComputeSet(mipSetLayout);
            if (!ms) break;
            VkDescriptorImageInfo mi[2] = { { VK_NULL_HANDLE, t.img.levelViews[l - 1], VK_IMAGE_LAYOUT_GENERAL },
                                            { VK_NULL_HANDLE, t.img.levelViews[l], VK_IMAGE_LAYOUT_GENERAL } };
            VkWriteDescriptorSet mw[2]{};
            for (int k = 0; k < 2; ++k)
            {
                mw[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                mw[k].dstSet = ms;
                mw[k].dstBinding = uint32_t(k);
                mw[k].descriptorCount = 1;
                mw[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                mw[k].pImageInfo = &mi[k];
            }
            vkUpdateDescriptorSets(dev->device, 2, mw, 0, nullptr);
            uint32_t mpc[4] = { lw, lh, sw, sh };
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, mipPipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, mipPipeLayout, 0, 1, &ms, 0, nullptr);
            vkCmdPushConstants(cb, mipPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mpc), mpc);
            vkCmdDispatch(cb, (lw + 7) / 8, (lh + 7) / 8, 1);
            sw = lw;
            sh = lh;
        }
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        t.width = w;
        t.height = h;
        t.depth = 1;
        t.kind = kTex2D;
        ++stats->texturesOnGpu;
        if (sc) ++scaledFetches;
        return true;
    }

    Image front;
    bool FrontBuffer(uint32_t base, uint32_t w, uint32_t h)
    {
        if (!w || !h) return false;
        // Resolution scale: the front buffer's scaled copy when its last resolves left one.
        uint32_t pitch = (w + 31) & ~31u;
        const ScaledCopy* sc = ScaledFor(base & 0x1FFFF000u, pitch, w, h);
        uint32_t fw = sc ? w * scale : w, fh = sc ? h * scale : h;
        if (front.image && (front.width != fw || front.height != fh)) { garbage.push_back(front); front = Image{}; }
        if (!front.image &&
            !dev->CreateImage(VK_IMAGE_VIEW_TYPE_2D, fw, fh, 1, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, front, 1,
                              VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_FORMAT_R8G8B8A8_UINT))
            return false;
        VkDescriptorSet set = ComputeSet(decodeSetLayout);
        if (!set) return false;
        bool fromVram = !sc && resolveGpu && InVram(base & 0x1FFFF000u, ((w + 31) & ~31u) * ((h + 31) & ~31u) * 4);
        VkDescriptorBufferInfo bMem{ sc ? sc->buf.buffer : fromVram ? vram.buffer : guest.buffer, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo img{ VK_NULL_HANDLE, front.altView, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr[2]{};
        for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = set; x.descriptorCount = 1; }
        wr[0].dstBinding = 0; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[0].pBufferInfo = &bMem;
        wr[1].dstBinding = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = &img;
        vkUpdateDescriptorSets(dev->device, 2, wr, 0, nullptr);
        uint32_t pc[11] = { base & 0x1FFFF000u, pitch, pitch * 4, 1u, w, h, 0u, 2u | (1u << 3) | (0u << 6) | (5u << 9), 6u, 0u, 0u };
        if (sc) { pc[0] = 0; pc[1] = pitch * scale; pc[2] = pitch * scale * 4; pc[3] = 0; pc[4] = fw; pc[5] = fh; }   // linear, scaled
        EndRendering();
        Begin();
        MemoryBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ImageBarrier(cb, front.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        Mark(NativeProfile::kGpuFront);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipeLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, decodePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cb, (fw + 7) / 8, (fh + 7) / 8, 1);
        ImageBarrier(cb, front.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        return true;
    }
    bool ReadFront(std::vector<uint32_t>& out, uint32_t& w, uint32_t& h)
    {
        if (!front.image) return false;
        Buffer b;
        VkDeviceSize bytes = VkDeviceSize(front.width) * front.height * 4;
        if (!dev->CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, b))
            return false;
        EndRendering();
        Begin();
        VkBufferImageCopy c{};
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageExtent = { front.width, front.height, 1 };
        vkCmdCopyImageToBuffer(cb, front.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffer, 1, &c);
        SubmitAndWait(NativeProfile::kWaitTexture);
        w = front.width;
        h = front.height;
        out.resize(size_t(w) * h);
        memcpy(out.data(), b.mapped, size_t(bytes));
        dev->DestroyBuffer(b);
        return true;
    }

    void InvalidateIndices(uint32_t pa, uint32_t bytes)
    {
        for (auto& [k, e] : indexEntries)
        {
            uint32_t sz = (k.format & 1) ? 4 : 2, lo = (k.base & ~(sz - 1)) & 0x1FFFFFFFu, n = std::min(k.numIndices, k.dmaWords) * sz;
            if (!e.dynamic && lo < pa + bytes && pa < lo + n) e.dynamic = true;
        }
    }
    // hc09: the guest pages resolves have written (render-to-texture: not the game's own images), for the texture dump.
    std::vector<uint64_t> resolvedPages = std::vector<uint64_t>((0x20000000u >> 12) / 64);
    bool Resolved(uint32_t pa, uint32_t bytes) const
    {
        for (uint32_t p = pa >> 12; p <= (pa + std::max(bytes, 1u) - 1) >> 12 && p < (0x20000000u >> 12); ++p)
            if ((resolvedPages[p >> 6] >> (p & 63)) & 1) return true;
        return false;
    }
    void InvalidateTextures(uint32_t pa, uint32_t bytes)
    {
        for (uint32_t p = pa >> 12; p <= (pa + std::max(bytes, 1u) - 1) >> 12 && p < (0x20000000u >> 12); ++p)
            resolvedPages[p >> 6] |= 1ull << (p & 63);
        for (auto it = textures.begin(); it != textures.end();)
        {
            const Texture& t = it->second;
            if (t.pa < pa + bytes && pa < t.pa + t.bytes) { garbage.push_back(t.img); it = textures.erase(it); ++texGen; }
            else ++it;
        }
    }

    bool Upload(Texture& t, uint32_t kind, const std::vector<std::vector<xenos::Image>>& layers)
    {
        uint32_t nLayers = uint32_t(layers.size()), nLevels = uint32_t(layers[0].size());
        const xenos::Image& base = layers[0][0];
        VkImageViewType vt = kind == kTex3D ? VK_IMAGE_VIEW_TYPE_3D : kind == kTexCube ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (!NewImage(vt, base.width, base.height, nLayers, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      t.img, nLevels, VK_FORMAT_R8G8B8A8_UINT))
            return false;
        VkDeviceSize total = 0;
        for (auto& l : layers) for (auto& m : l) total += VkDeviceSize(m.width) * m.height * 4;
        Buffer buf;
        if (!dev->CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf))
            return false;
        std::vector<VkBufferImageCopy> copies;
        VkDeviceSize off = 0;
        for (uint32_t layer = 0; layer < nLayers; ++layer)
            for (uint32_t i = 0; i < layers[layer].size(); ++i)
            {
                const xenos::Image& m = layers[layer][i];
                memcpy(static_cast<uint8_t*>(buf.mapped) + off, m.rgba.data(), size_t(m.width) * m.height * 4);
                VkBufferImageCopy c{};
                c.bufferOffset = off;
                c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, kind == kTex3D ? 0 : layer, 1 };
                c.imageOffset = { 0, 0, int32_t(kind == kTex3D ? layer : 0) };
                c.imageExtent = { m.width, m.height, 1 };
                copies.push_back(c);
                off += VkDeviceSize(m.width) * m.height * 4;
            }
        VkCommandBuffer ucb = UploadCb();
        ImageLayout(ucb, t.img.image, nLevels, t.img.layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(ucb, buf.buffer, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
        ImageLayout(ucb, t.img.image, nLevels, t.img.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        staging.push_back(buf);
        t.width = base.width;
        t.height = base.height;
        t.depth = kind == kTex3D || t.stacked ? nLayers : 1;
        t.kind = kind;
        return true;
    }

    const Texture* GetTexture(const xenos::TextureFetch& tf)
    {
        if (!tf.valid()) return &dummy[kTex2D];
        // Word 6: the texture override's set (the button icons: Xbox or PlayStation), so a switch
        // re-decodes only the textures it concerns.
        std::array<uint32_t, 7> key = { tf.dword[0] & ~0x0007FC00u, tf.dword[1], tf.dword[2], tf.dword[3] & 0x7FFFFu,
                                        tf.dword[4] & 0x3FCu, tf.dword[5] & ~0x1FFu,
                                        g_texOverride.set ? g_texOverride.set(tf) : 0 };
        auto it = textures.find(key);
        if (it != textures.end())
        {
            Texture& c = it->second;
            if (!xenos::HostWrittenSince(c.pa, c.bytes, c.epoch) && (!c.glyphs || hc09::font_hd::Version(c.pa) == c.glyphs))
            {
                c.lastUsed = frame;
                return c.img.image ? &c : &dummy[kTex2D];
            }
            garbage.push_back(c.img);
            textures.erase(it);
            ++texGen;
            ++stats->texturesRedecoded;
        }
        Texture& t = textures[key];
        t.lastUsed = frame;
        t.pa = tf.baseAddress() & 0x1FFFFFFFu;
        t.bytes = xenos::TextureBytes(tf);
        t.epoch = xenos::HostTrackWrites(t.pa, t.bytes);
        ScopeTimer timer{ stats->textureSeconds };
        uint32_t kind = tf.is3D() ? kTex3D : tf.dimension() == 3 || tf.stacked() ? kTexCube : kTex2D;
        t.stacked = tf.stacked();
        // hc09: a page of the game's text's glyphs: its sharper copy in its place (common/font_hd.h).
        const bool glyphPage = kind == kTex2D && tf.dimension() == 1 && tf.layers() == 1 && hc09::font_hd::Version(t.pa) != 0;
        if (kind == kTex2D && tf.dimension() == 1 && (tf.format() == 6 || tf.format() == 22) &&
            !(g_texOverride.dump && g_texOverride.dumping(tf) && !Resolved(t.pa, t.bytes)) &&
            !(g_texOverride.has && g_texOverride.has(tf, key[6])) && !glyphPage && DecodeOnGpu(t, tf))
            return &t;
        if (resolveGpu) ReleaseOverlapping(t.pa, t.bytes);
        WaitForWrites(t.pa, t.bytes);
        uint32_t n = tf.layers();
        std::vector<std::vector<xenos::Image>> layers(n, std::vector<xenos::Image>(1));
        bool replaced = false;   // hc09: an override's image (any size) for the texture's base level
        for (uint32_t l = 0; l < n; ++l)
        {
            std::string why;
            if (!xenos::DecodeTextureLayer(tf, l, layers[l][0], &why)) { Warn("texture not decoded: " + why); return &dummy[kTex2D]; }
            xenos::Image sharp;
            if (glyphPage && hc09::font_hd::Image(t.pa, tf.width(), tf.height(), sharp.rgba, sharp.width, sharp.height, t.glyphs))
            {
                layers[l][0] = std::move(sharp);
                replaced = true;
                t.extra = 0;
                while (t.extra < 4 && (layers[l][0].width >> (t.extra + 1)) >= tf.width() && (layers[l][0].height >> (t.extra + 1)) >= tf.height())
                    ++t.extra;
            }
            else if (n == 1 && kind == kTex2D && g_texOverride.dump && !Resolved(t.pa, t.bytes)) g_texOverride.dump(tf, layers[l][0]);
            // hc09: replace is asked for every single-layer texture (replacements by content hash have no set).
            if (!replaced && n == 1 && g_texOverride.replace && g_texOverride.replace(tf, key[6], layers[l][0]))
            {
                replaced = true;
                // A larger replacement gets a level per doubling more, down to the game's own sizes: shrunk on the
                // screen (or into a render target, as the jerseys' names), it is filtered from them, not aliased.
                t.extra = 0;
                while (t.extra < 4 && (layers[l][0].width >> (t.extra + 1)) >= tf.width() && (layers[l][0].height >> (t.extra + 1)) >= tf.height())
                    ++t.extra;
            }
            if (kind != kTex3D) MakeMips(layers[l], Bits(tf.dword[4], 6, 4) + (replaced ? t.extra : 0));
        }
        if (!Upload(t, kind, layers)) { Warn("texture upload failed"); return &dummy[kTex2D]; }
        if (replaced) { t.width = tf.width(); t.height = tf.height(); }   // the guest size: the shaders' texel units
        ++stats->textures;
        return &t;
    }

    VkSampler Sampler(const xenos::TextureFetch& tf, uint8_t override, uint32_t levels, uint32_t kind, uint32_t extra = 0)
    {
        uint32_t magF = (override & 3) == 3 ? Bits(tf.dword[3], 19, 2) : (override & 3);
        uint32_t minF = ((override >> 2) & 3) == 3 ? Bits(tf.dword[3], 21, 2) : ((override >> 2) & 3);
        uint32_t mipF = ((override >> 4) & 3) == 3 ? Bits(tf.dword[3], 23, 2) : ((override >> 4) & 3);
        uint32_t cx = Bits(tf.dword[0], 10, 3), cy = Bits(tf.dword[0], 13, 3), cz = Bits(tf.dword[0], 16, 3);
        uint32_t minLevel = std::min(Bits(tf.dword[4], 2, 4), levels - 1), maxLevel = std::min(Bits(tf.dword[4], 6, 4), levels - 1);
        if (maxLevel < minLevel) maxLevel = minLevel;
        if (mipF == 2) maxLevel = minLevel;
        // hc09: a larger replacement's levels above the game's (Texture::extra) may be used, the game's base level
        // among them, even when the game samples only its base level.
        if (extra && kind == kTex2D)
        {
            maxLevel = std::min(maxLevel + extra, levels - 1);
            if (mipF == 2) mipF = 1;
        }
        int32_t bias = SBits(tf.dword[4], 12, 10);
        if (kind == kTex3D) { minF = magF; mipF = 2; minLevel = maxLevel = 0; bias = 0; }
        else cz = 2;
        bool borderWhite = Bits(tf.dword[5], 0, 2) == 1;
        // hc09: anisotropy only where there are mips to pick from (not render targets read back as textures).
        bool aniso = anisotropy > 1.0f && minF == 1 && mipF != 2 && maxLevel > minLevel && kind != kTex3D;
        uint64_t key = magF | (minF << 2) | (mipF << 4) | (cx << 6) | (cy << 9) | (uint64_t(minLevel) << 12) | (uint64_t(maxLevel) << 16) |
                       (uint64_t(uint32_t(bias) & 0x3FF) << 20) | (uint64_t(cz) << 30) | (uint64_t(borderWhite) << 33) |
                       (uint64_t(aniso) << 34);
        auto it = samplers.find(key);
        if (it != samplers.end()) return it->second;
        auto addr = [](uint32_t c) {
            return c == 0 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : c == 1 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                   : c == 6 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        };
        VkSamplerCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        ci.magFilter = magF == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.minFilter = minF == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        ci.mipmapMode = mipF == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = addr(cx);
        ci.addressModeV = addr(cy);
        ci.addressModeW = addr(cz);
        ci.borderColor = borderWhite ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        ci.mipLodBias = float(bias) / 32.0f;
        ci.minLod = float(minLevel);
        ci.maxLod = float(maxLevel);
        ci.anisotropyEnable = aniso;
        ci.maxAnisotropy = aniso ? anisotropy : 1.0f;
        VkSampler smp = VK_NULL_HANDLE;
        vkCreateSampler(dev->device, &ci, nullptr, &smp);
        samplers[key] = smp;
        return smp;
    }

    // ---- draws --------------------------------------------------------------------
    // Indices assembled into a triangle list (offset, clamp; as vk_edram / the software).
    void WriteTriangleIndices(const xenos::DrawPacket& d, uint32_t* out)
    {
        uint32_t n = d.numIndices;
        uint32_t minI = Reg(reg::VGT_MIN_VTX_INDX), maxI = Reg(reg::VGT_MAX_VTX_INDX), off = Reg(reg::VGT_INDX_OFFSET);
        auto clampI = [=](uint32_t v) { return std::min(maxI, std::max(minI, (v + off) & 0xFFFFFF)); };
        bool list = d.primType == 4 || d.primType == 8;
        uint32_t count = list ? n / 3 * 3 : n;
        uint32_t* idx = out;
        if (!list) { idxBuf.resize(n); idx = idxBuf.data(); }
        if (d.sourceSelect != 0)
            for (uint32_t i = 0; i < count; ++i) idx[i] = clampI(i);
        else
        {
            uint32_t sz = d.indexSize32 ? 4 : 2, base = (d.dmaBase & ~(sz - 1)) & 0x1FFFFFFFu;
            uint32_t avail = std::min(count, d.dmaWords);
            uint32_t i = 0;
            // Guest physical memory is contiguous: one pointer for the whole range, the
            // endian and clamp choices out of the loop (the loops vectorize). Clamping is
            // a no-op without an offset when [min, max] covers every value of the size.
            if (avail && uint64_t(base) + uint64_t(avail) * sz <= 0x20000000ull)
            {
                const uint8_t* src = xenos::Phys(base);
                bool noClamp = off == 0 && minI == 0 && maxI >= (sz == 2 ? 0xFFFFu : 0xFFFFFFu);
                if (sz == 2)
                {
                    const uint16_t* s16 = reinterpret_cast<const uint16_t*>(src);
                    bool swap = d.dmaEndian == 1 || d.dmaEndian == 2;
                    if (noClamp && swap)
                        for (; i < avail; ++i) { uint32_t r = s16[i]; idx[i] = ((r >> 8) | (r << 8)) & 0xFFFF; }
                    else if (noClamp)
                        for (; i < avail; ++i) idx[i] = s16[i];
                    else if (swap)
                        for (; i < avail; ++i) { uint32_t r = s16[i]; idx[i] = clampI(((r >> 8) | (r << 8)) & 0xFFFF); }
                    else
                        for (; i < avail; ++i) idx[i] = clampI(s16[i]);
                }
                else
                {
                    const uint32_t* s32 = reinterpret_cast<const uint32_t*>(src);
                    auto conv = [&](auto swap) {
                        if (noClamp) for (; i < avail; ++i) idx[i] = swap(s32[i]) & 0xFFFFFF;
                        else for (; i < avail; ++i) idx[i] = clampI(swap(s32[i]));
                    };
                    switch (d.dmaEndian)
                    {
                    case 1: conv([](uint32_t r) { return ((r & 0x00FF00FFu) << 8) | ((r & 0xFF00FF00u) >> 8); }); break;
                    case 2: conv([](uint32_t r) { return xenos::Bswap32(r); }); break;
                    case 3: conv([](uint32_t r) { return (r << 16) | (r >> 16); }); break;
                    default: conv([](uint32_t r) { return r; }); break;
                    }
                }
            }
            for (; i < avail; ++i)
            {
                uint32_t a = (base + i * sz) & 0x1FFFFFFFu, v;
                if (sz == 2)
                {
                    uint16_t raw; memcpy(&raw, xenos::Phys(a), 2);
                    v = (d.dmaEndian == 1 || d.dmaEndian == 2) ? uint16_t((raw >> 8) | (raw << 8)) : raw;
                }
                else
                {
                    uint32_t raw; memcpy(&raw, xenos::Phys(a), 4);
                    v = d.dmaEndian == 1 ? (((raw & 0x00FF00FF) << 8) | ((raw & 0xFF00FF00) >> 8)) : d.dmaEndian == 2 ? xenos::Bswap32(raw)
                      : d.dmaEndian == 3 ? (raw << 16) | (raw >> 16) : raw;
                }
                idx[i] = clampI(v);
            }
            for (uint32_t z = clampI(0); i < count; ++i) idx[i] = z;
        }
        switch (d.primType)
        {
        case 5:
            for (uint32_t i = 1; i + 1 < n; ++i) { *out++ = idx[0]; *out++ = idx[i]; *out++ = idx[i + 1]; }
            break;
        case 6:
            for (uint32_t i = 0; i + 2 < n; ++i)
            {
                if (i & 1) { *out++ = idx[i + 1]; *out++ = idx[i]; }
                else { *out++ = idx[i]; *out++ = idx[i + 1]; }
                *out++ = idx[i + 2];
            }
            break;
        case 13:
            for (uint32_t i = 0; i + 3 < n; i += 4)
            {
                *out++ = idx[i]; *out++ = idx[i + 1]; *out++ = idx[i + 2];
                *out++ = idx[i]; *out++ = idx[i + 2]; *out++ = idx[i + 3];
            }
            break;
        }
    }

    // Offset of the draw's indices in the cache (converted now if new), or ~0: convert into the ring.
    VkDeviceSize CachedIndices(const xenos::DrawPacket& d, VkDeviceSize ibytes)
    {
        if (!indexCache || d.sourceSelect != 0 || !d.dmaWords) return ~VkDeviceSize(0);
        uint32_t sz = d.indexSize32 ? 4 : 2, base = (d.dmaBase & ~(sz - 1)) & 0x1FFFFFFFu;
        uint32_t srcBytes = std::min(d.numIndices, d.dmaWords) * sz;
        if (base < kSnapHi && base + srcBytes > kSnapLo) return ~VkDeviceSize(0);
        IndexKey key{ d.dmaBase, d.numIndices, d.dmaWords, uint32_t(d.indexSize32) | (d.dmaEndian << 1) | (d.primType << 4),
                      Reg(reg::VGT_INDX_OFFSET), Reg(reg::VGT_MIN_VTX_INDX), Reg(reg::VGT_MAX_VTX_INDX) };
        auto it = indexEntries.find(key);
        if (it != indexEntries.end())
        {
            IndexEntry& e = it->second;
            e.lastUsed = frame;
            if (e.dynamic) { ++idxDynamic; return ~VkDeviceSize(0); }
            if (!xenos::HostWrittenSince(base, srcBytes, e.epoch)) { ++idxHits; return e.offset; }
            e.dynamic = true;   // rewritten: from now on per draw (the GPU may still read the cached copy)
            ++idxDynamic;
            return ~VkDeviceSize(0);
        }
        VkDeviceSize need = (ibytes + 255) & ~VkDeviceSize(255);
        if (need > kIndexCacheBytes / 4) return ~VkDeviceSize(0);
        if (indexCacheUsed + need > kIndexCacheBytes)
        {
            // Full: once nothing in flight reads the cache any more, start over.
            SubmitAndWait(NativeProfile::kWaitRing);
            for (uint32_t k = 0; k < kSlots; ++k) WaitValue(slots[k].value);
            indexEntries.clear();
            indexCacheUsed = 0;
            ++idxResets;
            if (!BeginRenderingBound()) return ~VkDeviceSize(0);
        }
        IndexEntry& e = indexEntries[key];
        e.lastUsed = frame;
        e.epoch = xenos::HostTrackWrites(base, srcBytes);
        e.offset = VkDeviceSize(kSlots) * kIndexRingBytes + indexCacheUsed;
        indexCacheUsed += need;
        WriteTriangleIndices(d, reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(indices.mapped) + e.offset));
        ++idxConverted;
        return e.offset;
    }

    void Draw(const xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vsCode, uint64_t psHash, const xenos::ShaderCode* psCode)
    {
        // HC09_NATIVE_SKIP_PS=<hex hash>: leave out the draws with that pixel shader (to see what they are; dev).
        static const uint64_t skipPs = [] { const char* e = Hc09Env("HC09_NATIVE_SKIP_PS"); return e ? strtoull(e, nullptr, 16) : 0ull; }();
        if (skipPs && psHash == skipPs) { Skip("HC09_NATIVE_SKIP_PS"); return; }
        // HC09_NATIVE_SKIP_TARGET=<w>x<h>: leave out the draws into colour targets of that size (dev).
        static const std::pair<uint32_t, uint32_t> skipTarget = [] {
            uint32_t w = 0, h = 0;
            if (const char* e = Hc09Env("HC09_NATIVE_SKIP_TARGET")) sscanf(e, "%ux%u", &w, &h);
            return std::make_pair(w, h);
        }();
        if (skipTarget.first && bt[0] && bound[0].width == skipTarget.first && bound[0].height == skipTarget.second)
        { Skip("HC09_NATIVE_SKIP_TARGET"); return; }
        uint32_t mode = Reg(reg::RB_MODECONTROL) & 7;
        if (mode != 4 && mode != 5) { Skip("edram mode not 4/5"); return; }
        if (!vsCode || d.primType == 1 || d.sourceSelect == 1) { Skip("no VS / points / immediate indices"); return; }
        bool rect = d.primType == 8;
        if (d.primType != 4 && d.primType != 5 && d.primType != 6 && d.primType != 13 && !rect)
        { Warn("primitive type " + std::to_string(d.primType) + " not drawn"); Skip("primitive type"); return; }
        uint32_t colorMask = Reg(reg::RB_COLOR_MASK);
        uint32_t dc = Reg(reg::RB_DEPTHCONTROL);
        bool zEnable = (dc >> 1) & 1, zWrite = (dc >> 2) & 1, stencilOn = dc & 1;
        uint32_t cc = Reg(reg::RB_COLORCONTROL);
        bool colorOn = false;
        VkFormat colorFmt[4];
        uint32_t writeMask[4], blend[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            colorFmt[i] = bt[i] ? bt[i]->format : VK_FORMAT_UNDEFINED;
            uint32_t m = (colorMask >> (4 * i)) & 0xF;
            bool on = mode == 4 && m && psCode && bt[i];
            writeMask[i] = on ? m : 0;
            blend[i] = bt[i] ? Reg(kBlendRegs[i]) : 0;
            colorOn |= on;
        }
        bool hasDepth = bd != nullptr;
        if (!colorOn && !(hasDepth && ((zEnable && zWrite) || stencilOn))) { Skip("writes nothing"); return; }
        if (!d.numIndices) { Skip("empty"); return; }
        if (!BeginRenderingBound()) { Skip("no render targets"); return; }
        if (Tracing()) { ++traceDraws; traceDepthOnly += !colorOn; }
        Lap lap(*this);

        ShaderEntry* vs = Shader(vsHash, *vsCode);
        ShaderEntry* ps = psCode ? Shader(psHash, *psCode) : nullptr;
        if (!vs || (psCode && !ps)) { Skip("shader translation failed"); return; }

        uint32_t n = d.numIndices, triIndices = 0;
        switch (d.primType)
        {
        case 4: case 8: triIndices = n / 3 * 3; break;
        case 5: case 6: triIndices = n >= 3 ? 3 * (n - 2) : 0; break;
        case 13: triIndices = n / 4 * 6; break;
        }
        if (!triIndices) { Skip("no triangles"); return; }
        VkDeviceSize ibytes = VkDeviceSize(triIndices) * 4;

        // The 3D scene: a multisampled target (unless MSAA is off) the size of the screen, not the game's smaller
        // render-to-texture passes.
        if (effects.Enabled() && effectsFrame != frame && zEnable && zWrite && colorOn && bt[0] && bd && bound[0].width >= 1024 &&
            bound[0].height >= 576 && (bd->samples != VK_SAMPLE_COUNT_1_BIT || !msaa))
            NoteSceneCamera();
        lap.To(NativeProfile::kDrawState);
        // Textures first (a decode may wait for the GPU, which recycles the rings).
        const Texture* texOf[32];
        xenos::TextureFetch tfOf[32];
        uint8_t filtOf[32];
        uint32_t slots = vs->meta.textureSlots | (ps ? ps->meta.textureSlots : 0);
        bool memoHit[32];
        for (uint32_t s = 0; s < 32; ++s)
        {
            texOf[s] = &dummy[kTex2D];
            filtOf[s] = 0xFF;
            memoHit[s] = false;
            if (!(slots & (1u << s))) continue;
            tfOf[s] = xenos::ReadTextureFetch(s);
            filtOf[s] = ps && ps->meta.slotFilter[s] != 0xFF ? ps->meta.slotFilter[s] : vs->meta.slotFilter[s];
            SlotMemo& m = slotMemo[s];
            if (slotMemoOn && m.tex && m.gen == texGen && !memcmp(m.fetch, tfOf[s].dword, sizeof(m.fetch)) &&
                !xenos::HostWrittenSince(m.tex->pa, m.tex->bytes, m.tex->epoch))
            {
                m.tex->lastUsed = frame;
                texOf[s] = m.tex;
                memoHit[s] = m.filt == filtOf[s];
                continue;
            }
            texOf[s] = GetTexture(tfOf[s]);
            m.tex = nullptr;
            if (texOf[s] != &dummy[kTex2D])   // a cached texture (GetTexture's other answer is the dummy)
            {
                memcpy(m.fetch, tfOf[s].dword, sizeof(m.fetch));
                m.gen = texGen;
                m.tex = const_cast<Texture*>(texOf[s]);
                m.filt = filtOf[s];
                m.smp = VK_NULL_HANDLE;
            }
        }
        lap.To(NativeProfile::kDrawTextures);
        if (constUsed >= kMaxDraws || indexUsed + ibytes > kIndexRingBytes || descSets.size() >= kMaxSets)
        {
            SubmitAndWait(NativeProfile::kWaitRing);
            if (!BeginRenderingBound()) return;
        }
        if (!rendering && !BeginRenderingBound()) return;   // a texture decode ended the pass
        VkDeviceSize indexAt = CachedIndices(d, ibytes);
        if (indexAt == ~VkDeviceSize(0))
        {
            indexAt = indexBase + indexUsed;
            WriteTriangleIndices(d, reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(indices.mapped) + indexAt));
            indexUsed += (ibytes + 255) & ~VkDeviceSize(255);
        }
        lap.To(NativeProfile::kDrawIndices);

        DrawConsts& C = *reinterpret_cast<DrawConsts*>(static_cast<uint8_t*>(consts.mapped) + constBase + constUsed * constStride);
        // C is write-combined memory: never read back (each read is an uncached access).
        uint32_t vsw = Reg(0x2307), psw = Reg(0x2308);
        uint32_t vsBase = vsw & 0x1FF, vsSize = ((vsw >> 12) & 0x1FF) + 1, psBase = psw & 0x1FF, psSize = ((psw >> 12) & 0x1FF) + 1;
        C.vsBase = vsBase; C.vsSize = vsSize;
        C.psBase = psBase; C.psSize = psSize;
        // Only the constants the shaders read (all of the window with relative addressing).
        auto window = [&](uint32_t base, uint32_t size, const TranslatedShader* m) {
            if (base >= 512) return;
            size = std::min(size, 512 - base);
            if (!trimConsts || m->constRelative)
            {
                xenos::RegRange(reg::ALU_CONSTANTS + base * 4, size * 4, reinterpret_cast<uint32_t*>(C.c[base]));
                return;
            }
            for (uint32_t i = 0; i < size;)   // runs of read constants
            {
                uint64_t w = m->constUsed[i >> 6] >> (i & 63);
                if (!w) { i = (i | 63) + 1; continue; }
                i += uint32_t(__builtin_ctzll(w));
                if (i >= size) break;
                uint32_t j = i;
                while (j < size && j - i < 64 && ((m->constUsed[j >> 6] >> (j & 63)) & 1)) ++j;
                xenos::RegRange(reg::ALU_CONSTANTS + (base + i) * 4, (j - i) * 4, reinterpret_cast<uint32_t*>(C.c[base + i]));
                i = j;
            }
        };
        if (motionOn && vsBase < 512)
        {
            // The VS window blended between simulation ticks (vk_motion.h), then the constants the shader reads.
            const uint32_t size = std::min(vsSize, 512 - vsBase);
            float* w = motionScratch.data();
            xenos::RegRange(reg::ALU_CONSTANTS + vsBase * 4, size * 4, reinterpret_cast<uint32_t*>(w));
            // The draw's kind (its shaders and index range); draws of a kind are told apart by their constants.
            const uint64_t key = vsHash ^ (psHash * 0x100000001B3ull) ^ (uint64_t(d.dmaBase) << 20) ^ (uint64_t(d.numIndices) << 44);
            motion.Process(key, w, size, d.numIndices);
            if (!trimConsts || vs->meta.constRelative)
                std::memcpy(C.c[vsBase], w, size_t(size) * 16);
            else
                for (uint32_t i = 0; i < size; ++i)
                    if ((vs->meta.constUsed[i >> 6] >> (i & 63)) & 1) std::memcpy(C.c[vsBase + i], w + i * 4, 16);
        }
        else
            window(vsBase, vsSize, &vs->meta);
        if (ps) window(psBase, psSize, &ps->meta);
        xenos::RegRange(reg::BOOL_CONSTANTS, 8, C.bools);
        xenos::RegRange(reg::LOOP_CONSTANTS, 32, C.loops);
        xenos::RegRange(reg::FETCH_CONSTANTS, 192, C.fetch);
        uint32_t vte = Reg(reg::PA_CL_VTE_CNTL), su = Reg(reg::PA_SU_SC_MODE_CNTL);
        float half = (Reg(reg::PA_SU_VTX_CNTL) & 1) == 0 ? 0.5f : 0.0f;
        C.vpScale[0] = (vte & 1) ? AsF(Reg(reg::PA_CL_VPORT_XSCALE)) : 1.0f;
        C.vpScale[1] = (vte & 4) ? AsF(Reg(reg::PA_CL_VPORT_YSCALE)) : 1.0f;
        C.vpScale[2] = (vte & 16) ? AsF(Reg(reg::PA_CL_VPORT_ZSCALE)) : 1.0f;
        C.vpScale[3] = 0.0f;
        C.vpOffset[0] = (vte & 2) ? AsF(Reg(reg::PA_CL_VPORT_XOFFSET)) : 0.0f;
        C.vpOffset[1] = (vte & 8) ? AsF(Reg(reg::PA_CL_VPORT_YOFFSET)) : 0.0f;
        C.vpOffset[2] = (vte & 32) ? AsF(Reg(reg::PA_CL_VPORT_ZOFFSET)) : 0.0f;
        C.vpOffset[3] = 0.0f;
        bool squeezed = false;   // hc09: widescreen: the draw is squeezed (its scissor is too, below)
        C.wideClip = 0.0f;
        if (squeeze != 1.0f && bt[0])
        {
            // On targets of the screen's shape, a 3D draw sees wider (its clip x times the squeeze) and makes its
            // targets wide. On the screen-sized targets, the rest (menus, HUD, the 2D the game draws into its 3D
            // scenes) is squeezed; a quad that shows 3D content (full-screen ones, and the panels' backdrops cut from
            // the scene) keeps the vertices on the screen's edges and makes its targets wide.
            const float shape = bound[0].height ? float(bound[0].width) / float(bound[0].height) : 0.0f;
            const bool screenShaped = bound[0].width >= 320 && shape > 1.6f && shape < 1.9f;
            bool wide = false, showsWide = false;
            if (screenShaped && Perspective(vsBase, vsSize, vs->meta)) { C.wideClip = squeeze; wide = true; }
            for (uint32_t s = 0; s < 32 && !wide && !showsWide; ++s)
                if (slots & (1u << s)) showsWide = SamplesWide(tfOf[s]);
            if (bound[0].width >= 1024 && bound[0].height >= 576)
            {
                if (!wide)
                {
                    // Plain: no texture larger than 32x32 (fills; a fade's or bar's ends at the screen's edges stay).
                    bool plain = !showsWide;
                    for (uint32_t s = 0; s < 32 && plain; ++s)
                        if ((slots & (1u << s)) && texOf[s] && (texOf[s]->width > 32 || texOf[s]->height > 32)) plain = false;
                    C.vpScale[3] = plain || showsWide ? -squeeze : squeeze;
                    C.vpOffset[3] = 0.5f * float(bound[0].width);
                    traceSqueezed += Tracing();
                    squeezed = true;
                }
            }
            if (wide || showsWide)
                for (Target* t : { bt[0], bt[1], bt[2], bt[3], bd })
                    if (t) t->wide = true;
        }
        C.winOfs[0] = 0.0f; C.winOfs[1] = 0.0f; C.winOfs[2] = half; C.winOfs[3] = 0.0f;   // native: no window offset (no tiles)
        // At scale s the window position is s times the guest's (the viewport stays (-K, -K, 2K, 2K)).
        C.rtSize[0] = C.rtSize[1] = kClipScale / float(scale); C.rtSize[2] = C.rtSize[3] = float(scale) / kClipScale;
        C.fragScale = 1.0f / float(scale);
        C.vte = ((vte >> 8) & 1) | (((vte >> 9) & 1) << 1) | (((vte >> 10) & 1) << 2);
        uint32_t pc = Reg(reg::SQ_PROGRAM_CNTL);
        C.interpCount = std::min(16u, Bits(pc, 20, 4) + 1);
        C.paramGen = ((pc >> 18) & 1) ? (0x80000000u | Bits(Reg(reg::SQ_CONTEXT_MISC), 8, 8)) : 0;
        C.alpha = (cc & 7) | (((cc >> 3) & 1) << 3);
        C.alphaRef = AsF(Reg(reg::RB_ALPHA_REF));
        for (int i = 0; i < 4; ++i)
        {
            uint32_t ci = Reg(kColorInfoRegs[i]);
            C.rtScale[i] = std::ldexp(1.0f, SBits(ci, 20, 6));
            C.rtBase[i] = 0; C.rtFmt[i] = 0; C.rtBlend[i] = 0; C.rtMask[i] = 0;
            C.blendConst[i] = AsF(Reg(reg::RB_BLEND_RED + i));
        }
        C.surf[0] = C.surf[1] = C.surf[2] = C.surf[3] = 0;
        C.ds[0] = C.ds[1] = C.ds[2] = C.ds[3] = 0;
        uint32_t clipCntl = Reg(reg::PA_CL_CLIP_CNTL);
        bool xyFmt = (vte >> 8) & 1, zFmt = (vte >> 9) & 1, w0Fmt = (vte >> 10) & 1;
        bool clipOn = !((clipCntl >> 16) & 1) && !xyFmt && !zFmt && w0Fmt;
        C.clip[0] = (clipOn ? 1u : 0u) | (((clipCntl >> 19) & 1) << 1);
        C.clip[1] = C.clip[2] = C.clip[3] = 0;

        lap.To(NativeProfile::kDrawConsts);
        // Descriptors (as vk_edram, without the EDRAM buffer).
        if (!slotTemplate)
        {
            for (uint32_t k = 0; k < kTexKinds; ++k)
            {
                VkSampler smp = Sampler(xenos::TextureFetch{}, 0x3F, 1, k);
                for (uint32_t s = 0; s < 32; ++s)
                {
                    dummyImages[k][s] = { smp, dummy[k].img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    dummyUImages[k][s] = { nearest, dummy[k].img.altView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                }
            }
            slotTemplate = true;
        }
        uint64_t texKinds = 0;
        // The set's key: per sampled slot its texture's kind, view and sampler (the other
        // kinds' bindings of the slot are the dummies, fixed by the slot and kind).
        std::vector<uint64_t>& descKey = descKeyScratch;
        descKey.clear();
        const Texture* slotTex[32][kTexKinds];
        VkSampler slotSmp[32][kTexKinds];
        for (uint32_t s = 0; s < 32; ++s)
        {
            const Texture* t = texOf[s];
            C.texSize[s][0] = float(std::max(1u, t->width)); C.texSize[s][1] = float(std::max(1u, t->height));
            uint32_t shaderKind = t->stacked ? kTexStacked : t->kind;
            C.texSize[s][2] = float(shaderKind); C.texSize[s][3] = float(t->depth);
            texKinds |= uint64_t(shaderKind) << (2 * s);
            if (!(slots & (1u << s))) continue;
            uint8_t filt = filtOf[s];
            for (uint32_t k = 0; k < kTexKinds; ++k)
            {
                const Texture* u = t->kind == k ? t : &dummy[k];
                slotTex[s][k] = u;
                if (u == &dummy[k]) slotSmp[s][k] = dummyImages[k][0].sampler;
                else if (memoHit[s] && slotMemo[s].smp) slotSmp[s][k] = slotMemo[s].smp;
                else
                {
                    slotSmp[s][k] = Sampler(tfOf[s], filt == 0xFF ? 0x3F : filt, u->img.mipLevels, k, u->extra);
                    if (slotMemo[s].tex == t) { slotMemo[s].smp = slotSmp[s][k]; slotMemo[s].filt = filt; }
                }
            }
            uint32_t k = t->kind < kTexKinds ? t->kind : 0;
            descKey.insert(descKey.end(), { uint64_t(s) | (uint64_t(k) << 8), uint64_t(slotTex[s][k]->img.view), uint64_t(slotSmp[s][k]) });
        }
        // hc09: the crowd's cards: the atlas's history in the slot's array binding, and which layer is newest, how
        // many are filled, and the draw's key (CrowdPatch) in pad0.
        uint32_t crowdInfo = 0;
        if (crowdOn && psHash == kCrowdPs && (slots & 2u))
        {
            crowdAtlas = texOf[1]->pa;
            if (crowdFilled && crowdHistory.img.image)
            {
                slotTex[1][kTexCube] = &crowdHistory;
                slotSmp[1][kTexCube] = slotSmp[1][kTex2D];
                descKey.insert(descKey.end(), { 1u | (uint64_t(kTexCube) << 8) | (1ull << 32), uint64_t(crowdHistory.img.view),
                                                uint64_t(slotSmp[1][kTexCube]) });
                uint32_t key = d.dmaBase * 0x9E3779B1u;
                key ^= key >> 16;
                crowdInfo = crowdNewest | (crowdFilled << 8) | (key << 16);
            }
        }
        memcpy(&C.pad0, &crowdInfo, sizeof(crowdInfo));
        // hc09: our shading's value for the draw (vk_materials.cpp: the turf's wear).
        if (psHash == kTurfPs)
        {
            uint32_t fetch[32][6];
            for (uint32_t s = 0; s < 32; ++s) memcpy(fetch[s], tfOf[s].dword, sizeof(fetch[s]));
            if (float value; MaterialDrawValue(psHash, fetch, slots, value)) memcpy(&C.pad0, &value, sizeof(value));
        }
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (lastSet && descKey == lastDescKey) set = lastSet;
        else if (auto dit = descSets.find(descKey); dit != descSets.end()) set = dit->second;
        else
        {
            VkDescriptorImageInfo images[kTexKinds][32], uimages[kTexKinds][32];
            memcpy(images, dummyImages, sizeof(images));
            memcpy(uimages, dummyUImages, sizeof(uimages));
            for (uint32_t s = 0; s < 32; ++s)
                if (slots & (1u << s))
                    for (uint32_t k = 0; k < kTexKinds; ++k)
                    {
                        images[k][s] = { slotSmp[s][k], slotTex[s][k]->img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                        uimages[k][s] = { nearest, slotTex[s][k]->img.altView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    }
            VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &setLayout;
            if (vkAllocateDescriptorSets(dev->device, &ai, &set) != VK_SUCCESS) { Skip("descriptor pool exhausted"); return; }
            VkDescriptorBufferInfo bMem{ guest.buffer, 0, VK_WHOLE_SIZE }, bConst{ consts.buffer, 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet w[8]{};
            for (auto& x : w) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = set; x.descriptorCount = 1; }
            w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bMem;
            w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &bConst;
            w[2].dstBinding = 2; w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].descriptorCount = 32; w[2].pImageInfo = images[kTex2D];
            w[3].dstBinding = 4; w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].descriptorCount = 32; w[3].pImageInfo = images[kTex3D];
            w[4].dstBinding = 5; w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[4].descriptorCount = 32; w[4].pImageInfo = images[kTexCube];
            for (uint32_t k = 0; k < kTexKinds; ++k)
            {
                w[5 + k].dstBinding = 6 + k;
                w[5 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[5 + k].descriptorCount = 32;
                w[5 + k].pImageInfo = uimages[k];
            }
            vkUpdateDescriptorSets(dev->device, 8, w, 0, nullptr);
            descSets.emplace(descKey, set);
        }
        if (set != lastSet) { lastSet = set; lastDescKey = descKey; }

        lap.To(NativeProfile::kDrawDesc);
        // Pipeline.
        bool cullFront = (su & 1) && !rect, cullBack = ((su >> 1) & 1) && !rect;
        uint32_t cull = (cullFront ? VK_CULL_MODE_FRONT_BIT : 0) | (cullBack ? VK_CULL_MODE_BACK_BIT : 0);
        uint32_t frontFace = ((su >> 2) & 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        bool zOn = hasDepth && zEnable, sOn = hasDepth && stencilOn;
        uint32_t dsKey = hasDepth ? (dc & ~(zOn ? 0u : 0x76u)) : 0;   // z bits irrelevant when z is off
        // hc09: smoother player models. HC09_TESS_AB=1 (dev): on odd frames only (two screenshots in a row compare).
        static const bool tessAb = Hc09Env("HC09_TESS_AB") != nullptr;
        const ShaderEntry* tess = vs->tessEval && !rect && !(tessAb && frame % 2 == 0) ? vs : nullptr;
        PipeKey key = { vsHash, ps ? psHash : 0, uint64_t(rect) | (uint64_t(cull) << 8) | (uint64_t(frontFace) << 16) | (uint64_t(tess != nullptr) << 40) |
                                                     (uint64_t(hasDepth) << 24) | (uint64_t(zOn) << 25) | (uint64_t(sOn) << 26) |
                                                     (uint64_t(boundSamples) << 32),
                        texKinds, dsKey };
        for (int i = 0; i < 4; ++i) key[5 + i] = uint64_t(colorFmt[i]) | (uint64_t(writeMask[i]) << 32) | (uint64_t(writeMask[i] ? blend[i] : 0) << 36);
        VkPipeline pipe = lastPipe && key == lastPipeKey ? lastPipe
                        : Pipeline(key, vs->module, ps ? ps->module : nullPs, rect, cull, frontFace, texKinds, colorFmt, writeMask, blend,
                                   hasDepth, dc, zOn, sOn, tess);
        if (pipe && pipe != lastPipe) { lastPipe = pipe; lastPipeKey = key; }
        if (!pipe) { Skip("pipeline creation failed"); return; }
        lap.To(NativeProfile::kDrawPipeline);

        // Scissor: the window scissor (guest pixels, times the scale), clamped to the render area (no window offset natively).
        uint32_t tl = Reg(reg::PA_SC_WINDOW_SCISSOR_TL), br = Reg(reg::PA_SC_WINDOW_SCISSOR_BR);
        int32_t sc = int32_t(scale);
        int32_t scx0 = std::max(0, int32_t(Bits(tl, 0, 14)) * sc), scy0 = std::max(0, int32_t(Bits(tl, 16, 14)) * sc);
        int32_t scx1 = std::min<int32_t>(int32_t(Bits(br, 0, 14)) * sc, int32_t(areaW)), scy1 = std::min<int32_t>(int32_t(Bits(br, 16, 14)) * sc, int32_t(areaH));
        if (squeezed)   // the scissor's sides inside the screen move with the draw (widescreen.h)
        {
            const float mid = 0.5f * float(bound[0].width * scale);
            if (scx0 > 0) scx0 = int32_t(std::floor(mid + (float(scx0) - mid) * squeeze));
            if (scx1 < int32_t(bound[0].width * scale)) scx1 = int32_t(std::ceil(mid + (float(scx1) - mid) * squeeze));
        }
        if (scx1 <= scx0 || scy1 <= scy0) { Skip("empty scissor"); return; }
        // HC09_NATIVE_TRACE_PASSES with HC09_NATIVE_TRACE_DRAWS=1: each draw on a screen-sized single-sampled target (dev).
        static const bool traceDrawsOn = Hc09Env("HC09_NATIVE_TRACE_DRAWS") != nullptr;
        // HC09_NATIVE_SKIP_HUD=<a>-<b>: leave out the a-th to b-th draws of each frame on screen-sized single-sampled
        // targets (dev: finding a draw).
        static const std::pair<uint32_t, uint32_t> skipHud = [] {
            uint32_t a = 1, b = 0;
            if (const char* e = Hc09Env("HC09_NATIVE_SKIP_HUD")) sscanf(e, "%u-%u", &a, &b);
            return std::make_pair(a, b);
        }();
        if (bt[0] && bt[0]->samples == VK_SAMPLE_COUNT_1_BIT && bound[0].width >= 1024)
        {
            if (hudFrame != frame) { hudFrame = frame; hudDraws = 0; }
            ++hudDraws;
            if (hudDraws >= skipHud.first && hudDraws <= skipHud.second) { Skip("HC09_NATIVE_SKIP_HUD"); return; }
        }
        if (traceDrawsOn && Tracing() && bt[0] && bound[0].width >= 320)
        {
            std::string tex;
            for (uint32_t s = 0; s < 32; ++s)
                if ((slots & (1u << s)) && texOf[s])
                {
                    char at[16];
                    snprintf(at, sizeof(at), "@%08X", texOf[s]->pa);
                    tex += " " + std::to_string(texOf[s]->width) + "x" + std::to_string(texOf[s]->height) + (SamplesWide(tfOf[s]) ? "w" : "") + at;
                }
            fprintf(stderr, "[draw] %ux%u ms%u vs %016llx ps %016llx sq %d 3d %d colour %d stencil %d z %d%d scissor %d,%d-%d,%d n %u tex%s\n", bound[0].width,
                    bound[0].height, bound[0].msaa, (unsigned long long)vsHash, (unsigned long long)psHash, squeezed, C.wideClip != 0.0f, colorOn, stencilOn, zEnable, zWrite,
                    scx0, scy0, scx1, scy1, d.numIndices, tex.c_str());
        }

        if (pipe != boundPipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); boundPipe = pipe; }
        bool fresh = !rec.valid;
        if (fresh)
        {
            VkViewport vp{ -kClipScale, -kClipScale, 2.0f * kClipScale, 2.0f * kClipScale, 0.0f, 1.0f };
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdBindIndexBuffer(cb, indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            rec.valid = true;
        }
        const uint32_t drawIndex = uint32_t((constBase + constUsed * constStride) / constStride);   // DrawIndexedConsts
        if (set != boundSet) { vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &set, 0, nullptr); boundSet = set; }
        vkCmdPushConstants(cb, pipeLayout, drawPushStages, 0, 4, &drawIndex);
        VkRect2D scissor{ { scx0, scy0 }, { uint32_t(scx1 - scx0), uint32_t(scy1 - scy0) } };
        if (fresh || memcmp(&scissor, &rec.scissor, sizeof(scissor))) { vkCmdSetScissor(cb, 0, 1, &scissor); rec.scissor = scissor; }
        float bc[4];
        for (int i = 0; i < 4; ++i) bc[i] = AsF(Reg(reg::RB_BLEND_RED + i));
        if (fresh || memcmp(bc, rec.blend, sizeof(bc))) { vkCmdSetBlendConstants(cb, bc); memcpy(rec.blend, bc, sizeof(bc)); }
        bool twoSided = (dc >> 7) & 1;
        uint32_t rmF = Reg(reg::RB_STENCILREFMASK) & 0xFFFFFFu, rmB = (twoSided ? Reg(reg::RB_STENCILREFMASK_BF) : rmF) & 0xFFFFFFu;
        if (fresh || rmF != rec.stencilF)
        {
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, Bits(rmF, 0, 8));
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, Bits(rmF, 8, 8));
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, Bits(rmF, 16, 8));
            rec.stencilF = rmF;
        }
        if (fresh || rmB != rec.stencilB)
        {
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, Bits(rmB, 0, 8));
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, Bits(rmB, 8, 8));
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, Bits(rmB, 16, 8));
            rec.stencilB = rmB;
        }
        Mark(NativeProfile::kGpuDraws);
        vkCmdDrawIndexed(cb, triIndices, 1, uint32_t(indexAt / 4), 0, 0);
        if (drawProf && tsPool && recording && this->slots[cur].tsN + 1 < kMaxTs)
        {
            Slot& sl = this->slots[cur];
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool, kMaxTs * cur + sl.tsN++);
            sl.tsKinds.push_back(uint8_t(gpuKind));
            sl.tsDraw.push_back(int32_t(sl.drawInfo.size()));
            sl.drawInfo.push_back({ vsHash, ps ? psHash : 0, uint64_t(areaW) << 32 | areaH, uint64_t(boundSamples) << 32 | triIndices });
        }
        ++constUsed;
        ++stats->draws;
        lap.To(NativeProfile::kDrawRecord);
    }

    // hc09: the camera of a draw of the 3D scene (as the D3D12 renderer: gpu/scene_camera.h, vertex constants
    // c156..c159), with the viewport's depth mapping and rectangle.
    void NoteSceneCamera()
    {
        uint32_t vsBase = Reg(0x2307) & 0x1FF;
        if (vsBase + 160 > 512) return;
        float m[16];
        for (uint32_t i = 0; i < 16; ++i) m[i] = AsF(Reg(reg::ALU_CONSTANTS + (vsBase + 156) * 4 + i));
        hc09::gpu::SceneCamera camera = hc09::gpu::CameraFromConstants(m);
        if (!camera.valid) return;
        uint32_t vte = Reg(reg::PA_CL_VTE_CNTL);
        float xs = (vte & 1) ? AsF(Reg(reg::PA_CL_VPORT_XSCALE)) : 1.0f, xo = (vte & 2) ? AsF(Reg(reg::PA_CL_VPORT_XOFFSET)) : 0.0f;
        float ys = (vte & 4) ? AsF(Reg(reg::PA_CL_VPORT_YSCALE)) : 1.0f, yo = (vte & 8) ? AsF(Reg(reg::PA_CL_VPORT_YOFFSET)) : 0.0f;
        float zs = (vte & 16) ? AsF(Reg(reg::PA_CL_VPORT_ZSCALE)) : 1.0f, zo = (vte & 32) ? AsF(Reg(reg::PA_CL_VPORT_ZOFFSET)) : 0.0f;
        // The effects' shaders map clip y up to pixel rows down, as D3D's viewport does.
        if (!(xs > 0.0f) || !(ys < 0.0f) || zs == 0.0f) return;
        camera.depthA = zo + camera.depthA * zs;
        camera.depthB *= zs;
        camera.sx *= squeeze;   // widescreen: the 3D draws' clip x is squeezed (widescreen.h)
        camera.cx *= squeeze;
        float s = float(scale);
        sceneView.camera = camera;
        sceneView.x = (xo - xs) * s;
        sceneView.y = (yo + ys) * s;
        sceneView.width = 2.0f * xs * s;
        sceneView.height = -2.0f * ys * s;
        sceneColor = bt[0];
        sceneDepth = bd;
    }
    void ApplySceneEffects(Target* src)
    {
        if (!sceneView.camera.valid || effectsFrame == frame || src != sceneColor || bd != sceneDepth || !bd || !bd->depthView) return;
        effectsFrame = frame;
        EndRendering();
        // The passes' descriptor sets (up to 17) must not make ComputeSet submit in the middle of them.
        if (computeSetsUsed + 24 > kComputeSets) SubmitAndWait(NativeProfile::kWaitCompute);
        Begin();
        Mark(NativeProfile::kGpuOther);
        effects.Apply(cb, src->img, src->samples, bd->depthView, sceneView,
                      [this](VkDescriptorSetLayout layout) { return ComputeSet(layout); }, garbage);
        if (float rect[4]; effects.HdrGain(rect)) gainFrame = frame;
        sceneView = {};
    }

    // ---- resolves ----------------------------------------------------------------
    void Resolve(const d3dn::ResolveOp& r)
    {
        Target* src = r.src >= 4 ? bd : bt[r.src & 3];
        if (!src) { Skip("resolve without a source target"); return; }
        if (Tracing())
        {
            TraceFlushDraws();
            fprintf(stderr, "[passes]   resolve %s%u %d,%d-%d,%d -> %08X pitch %u fmt %u tex %ux%u%s%s\n", r.src >= 4 ? "depth" : "colour",
                    r.src & 3, r.x0, r.y0, r.x1, r.y1, r.destBase, r.destPitch, r.destFmt, r.texWidth, r.texHeight, r.copy ? "" : " (no copy)",
                    src == sceneColor && sceneView.camera.valid ? " [scene]" : "");
        }
        if (effects.Enabled() && r.src < 4) ApplySceneEffects(src);
        if (crowdOn && crowdAtlas && r.src < 4 && r.copy && r.destBase == crowdAtlas && r.x0 == 0 && r.y0 == 0) CaptureCrowd(src, r);
        // The rectangle in guest pixels (the source image is `scale` times the surface).
        int32_t x1 = std::min<int32_t>(r.x1, int32_t(src->img.width / scale)), y1 = std::min<int32_t>(r.y1, int32_t(src->img.height / scale));
        if (r.copy && x1 > r.x0 && y1 > r.y0)
        {
            // Resolved 8888 data stays on the GPU (before the descriptor set: Acquire
            // may wait, which recycles the pools).
            uint32_t rows = (uint32_t(y1) + 31) & ~31u, span = r.destPitch * rows * r.destBytes;
            bool toVram = resolveGpu && r.destFmt == 6 && r.destBytes == 4 && uint64_t(r.destBase) + span <= 0x20000000ull;
            if (toVram) Acquire(r.destBase, span);
            else if (resolveGpu) ReleaseOverlapping(r.destBase, span);
            // Resolution scale: the destination's scaled copy (other copies the resolve writes over are stale).
            ScaledCopy* sc = ScaledForResolve(r);
            if (scale > 1) DropScaledOverlapping(r.destBase, span, sc);
            EndRendering();
            Begin();
            MemoryBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            int which = r.src >= 4 ? 1 : 0, ms = src->samples != VK_SAMPLE_COUNT_1_BIT ? 1 : 0;
            VkDescriptorSet set = ComputeSet(resolveSetLayout[which][ms]);
            if (!set) { Skip("resolve descriptor"); return; }
            VkDescriptorBufferInfo bMem{ toVram ? vram.buffer : guest.buffer, 0, VK_WHOLE_SIZE };
            VkDescriptorBufferInfo bScaled{ sc ? sc->buf.buffer : bMem.buffer, 0, VK_WHOLE_SIZE };
            VkDescriptorImageInfo i0{ nearest, which ? src->depthView : src->img.view, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo i1{ nearest, src->stencilView, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[4]{};
            for (auto& x : w) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = set; x.descriptorCount = 1; }
            w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bMem;
            w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &i0;
            w[2].dstBinding = 3; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &bScaled;
            w[3].dstBinding = 2; w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].pImageInfo = &i1;
            vkUpdateDescriptorSets(dev->device, which ? 4 : 3, w, 0, nullptr);
            NativeResolveConsts p{};
            p.x0 = r.x0; p.y0 = r.y0; p.w = uint32_t(x1 - r.x0); p.h = uint32_t(y1 - r.y0);
            p.srcFmt = which ? 0 : src->s.format;
            p.destBase = r.destBase; p.destPitch = r.destPitch; p.destFmt = r.destFmt; p.destBytes = r.destBytes;
            p.endian = r.endian; p.swapRB = r.swapRB; p.destScale = r.destScale;
            p.samples = uint32_t(src->samples);
            p.sampleSel = r.sampleSel;
            p.scale = scale;
            if (sc)
            {
                p.scaled = 1;
                p.scaledPitch = sc->pitch * scale;
                p.scaledRows = sc->height * scale;
                p.destX = r.destX;
                p.destY = r.destY;
            }
            Mark(NativeProfile::kGpuResolves);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipe[which][ms]);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipeLayout[which][ms], 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cb, resolvePipeLayout[which][ms], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
            vkCmdDispatch(cb, (p.w + 7) / 8, (p.h + 7) / 8, 1);
            MemoryBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            if (sc)   // the 8x8 blocks of the texture this resolve wrote
            {
                int32_t bw = int32_t(sc->pitch / 8), bh = int32_t((sc->height + 7) / 8);
                int32_t bx0 = std::max(0, (r.x0 + r.destX) / 8), bx1 = std::min(bw, (x1 + r.destX + 7) / 8);
                int32_t by0 = std::max(0, (r.y0 + r.destY) / 8), by1 = std::min(bh, (y1 + r.destY + 7) / 8);
                for (int32_t by = by0; by < by1; ++by)
                    for (int32_t bx = bx0; bx < bx1; ++bx) sc->cover[size_t(by) * bw + bx] = 1;
            }
            ++stats->resolves;
            if (squeeze != 1.0f)
            {
                // The range holds wide (3D) content now, or no longer does.
                const uint32_t end = r.destBase + span;
                wideRanges.erase(std::remove_if(wideRanges.begin(), wideRanges.end(),
                                                [&](const auto& w) { return w.first < end && r.destBase < w.first + w.second; }),
                                 wideRanges.end());
                if (src->wide)
                {
                    if (wideRanges.size() >= 64) wideRanges.erase(wideRanges.begin());
                    wideRanges.emplace_back(r.destBase, span);
                }
            }
            InvalidateTextures(r.destBase, span);
            InvalidateIndices(r.destBase, span);
            if (waitAtCopyResolves) SubmitAndWait(NativeProfile::kWaitResolve);
            else if (!toVram) pendingWrites.emplace_back(r.destBase, span);
        }
        // Clears of the source after the copy.
        if (r.clearColor || r.clearDepth)
        {
            int32_t rc[4] = { r.x0, r.y0, x1, y1 };
            uint32_t flags = (r.clearColor ? 1u << (r.src & 3) : 0u) | (r.clearDepth ? 0x30u : 0u);
            Clear(flags, rc, r.color, r.z, r.stencil);
        }
    }
};

bool NativeRenderer::Init(Device& dev, const Buffer& guestMemory, std::string* err, bool exact)
{
    impl_ = std::make_unique<Impl>();
    Impl& I = *impl_;
    I.dev = &dev;
    I.guest = guestMemory;
    I.stats = &stats_;
    auto flag = [](const char* e, bool dflt) { return e && *e ? strcmp(e, "0") != 0 : dflt; };
    I.translate.hwSampler = flag(Hc09Env("HC09_GPU_HWSAMPLER"), !exact);
    I.translate.fastMath = flag(Hc09Env("HC09_GPU_FASTMATH"), !exact);
    { const char* e = Hc09Env("HC09_NATIVE_RELAXED"); I.translate.relaxed = exact ? 0u : e ? uint32_t(atoi(e)) : 0u; }
    I.translate.hostRt = true;
    I.msaa = flag(Hc09Env("HC09_NATIVE_MSAA"), true);
    if (const char* e = Hc09Env("HC09_NATIVE_MSAA"); e && *e)
    {
        std::string v = e;
        for (char& ch : v) ch = char(tolower((unsigned char)ch));
        if (v == "off" || v == "no" || v == "false") I.msaa = false;
        I.msaaCap2 = v == "2";
    }
    if (const char* e = Hc09Env("HC09_SCALE"); e && *e) I.scale = uint32_t(std::clamp(atoi(e), 1, 4));
    if (const char* e = Hc09Env("HC09_ANISOTROPY"); e && *e) I.anisotropy = float(std::clamp(atoi(e), 1, 16));
    I.anisotropy = std::min(I.anisotropy, dev.anisotropy);
    I.scaledFetch = flag(Hc09Env("HC09_NATIVE_SCALED_FETCH"), true);
    I.drawProf = flag(Hc09Env("HC09_NATIVE_DRAW_PROF"), false);
    I.prof = flag(Hc09Env("HC09_NATIVE_PROF"), false) || I.drawProf;
    I.trimConsts = flag(Hc09Env("HC09_NATIVE_TRIM_CONSTS"), true);
    I.indexCache = flag(Hc09Env("HC09_NATIVE_INDEX_CACHE"), true);
    I.resolveGpu = flag(Hc09Env("HC09_NATIVE_RESOLVE_GPU"), true);
    if (I.resolveGpu &&
        !dev.CreateBuffer(0x20000000ull, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, I.vram))
    {
        fprintf(stderr, "[native] no device memory for resolved data: resolves write guest memory\n");
        I.resolveGpu = false;
    }
    VkDevice d = dev.device;
    if (I.prof && dev.props.limits.timestampComputeAndGraphics)
    {
        VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = Impl::kMaxTs * kSlots;
        vkCreateQueryPool(d, &qi, nullptr, &I.tsPool);
    }

    // Depth format: D24S8 where it can be an attachment and sampled, else D32S8.
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        I.depthFormat = (fp.optimalTilingFeatures & need) == need ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    }

    // Draw descriptors: as vk_edram without binding 3 (EDRAM).
    VkDescriptorSetLayoutBinding b[8]{};
    // hc09: guest memory and the draw constants are read by the tessellation stages too (vk_tessellation.h).
    const VkShaderStageFlags tessStages = dev.tessellation ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : 0;
    b[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | tessStages, nullptr };
    b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | tessStages, nullptr };
    b[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[3] = { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[4] = { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    for (uint32_t k = 0; k < 3; ++k)
        b[5 + k] = { 6 + k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 8;
    lci.pBindings = b;
    // Update-after-bind pool: the 192 texture bindings count against the update-after-bind limits (Intel: 64
    // samplers per stage otherwise). Descriptors are still all written before the set is bound.
    const bool uab = flag(Hc09Env("HC09_NATIVE_UAB"), true);   // dev A/B switch
    if (uab) lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | tessStages, 0, 4 };
    I.drawPushStages = pcr.stageFlags;
    VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &I.setLayout;
    pci.pushConstantRangeCount = 1;
    pci.pPushConstantRanges = &pcr;
    VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxSets },
                                      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxSets },
                                      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets / 2 * 32 * kTexKinds * 2 } };   // under Intel's 1M update-after-bind descriptors in all pools (x kSlots)
    VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    if (uab) dpi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpi.maxSets = kMaxSets;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorSetLayout(d, &lci, nullptr, &I.setLayout) != VK_SUCCESS || vkCreatePipelineLayout(d, &pci, nullptr, &I.pipeLayout) != VK_SUCCESS)
    { if (err) *err = "descriptor/pipeline layout"; return false; }
    for (Impl::Slot& sl : I.slots)
        if (vkCreateDescriptorPool(d, &dpi, nullptr, &sl.pool) != VK_SUCCESS) { if (err) *err = "descriptor pool"; return false; }

    // Compute: one pool for resolves, texture decodes and mips.
    VkDescriptorPoolSize cps[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Impl::kComputeSets * 2 },
                                    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, Impl::kComputeSets * 2 },
                                    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Impl::kComputeSets * 3 } };
    VkDescriptorPoolCreateInfo cpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    cpi.maxSets = Impl::kComputeSets;
    cpi.poolSizeCount = 3;
    cpi.pPoolSizes = cps;
    for (Impl::Slot& sl : I.slots)
        if (vkCreateDescriptorPool(d, &cpi, nullptr, &sl.computePool) != VK_SUCCESS) { if (err) *err = "compute pool"; return false; }

    VkDeviceSize align = std::max<VkDeviceSize>(16, dev.props.limits.minStorageBufferOffsetAlignment);
    // An array of DrawConsts (the shaders index it with the draw number, DrawIndexedConsts): no offset alignment.
    static_assert(sizeof(DrawConsts) % 16 == 0, "std430 array stride");
    (void)align;
    I.constStride = sizeof(DrawConsts);
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!dev.CreateBuffer(I.constStride * kMaxDraws * kSlots, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, I.consts) ||
        !dev.CreateBuffer(kIndexRingBytes * kSlots + kIndexCacheBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hv, I.indices))
    { if (err) *err = "constant/index buffers"; return false; }

    {
        const char* e = Hc09Env("HC09_PIPELINE_CACHE");
        const char* c = Hc09Env("HC09_CACHE_DIR");   // hc09.exe sets it to its cache folder
        I.pipelineCachePath = e && *e ? e : std::string(c && *c ? c : "hc09-cache") + "/vk_pipelines_native.bin";
        std::vector<char> data;
        if (FILE* f = fopen(I.pipelineCachePath.c_str(), "rb"))
        {
            char buf[1 << 16];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + got);
            fclose(f);
        }
        VkPipelineCacheCreateInfo pcci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        pcci.initialDataSize = data.size();
        pcci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(d, &pcci, nullptr, &I.pipelineCache) != VK_SUCCESS)
        {
            pcci.initialDataSize = 0;
            pcci.pInitialData = nullptr;
            vkCreatePipelineCache(d, &pcci, nullptr, &I.pipelineCache);
        }
    }
    {   // the rectangle geometry shader reads a prefix of the constants: padded to the whole struct for the array
        std::string gs = RectListGeometryShader();
        const std::string last = "vec4 rtSize;";
        size_t at = gs.find(last);
        const size_t pad = (sizeof(DrawConsts) - (offsetof(DrawConsts, rtSize) + 16)) / 16;
        if (at != std::string::npos) gs.insert(at + last.size(), " vec4 xePad[" + std::to_string(pad) + "];");
        I.rectGs = I.Module(DrawIndexedConsts(gs), Stage::kGeometry, "rectlist_gs");
    }
    I.nullPs = I.Module(HostNullPixelShader(), Stage::kFragment, "ps_null_native");
    if (!I.rectGs || !I.nullPs) { if (err) *err = "built-in shaders failed to compile"; return false; }

    auto computePipe = [&](const std::string& glsl, const char* name, const VkDescriptorSetLayoutBinding* binds, uint32_t nb, uint32_t pushBytes,
                           VkDescriptorSetLayout& sl, VkPipelineLayout& pl, VkPipeline& p) {
        VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dl.bindingCount = nb;
        dl.pBindings = binds;
        VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes };
        VkPipelineLayoutCreateInfo plc{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plc.setLayoutCount = 1;
        plc.pSetLayouts = &sl;
        plc.pushConstantRangeCount = 1;
        plc.pPushConstantRanges = &pr;
        VkShaderModule m = I.Module(glsl, Stage::kCompute, name);
        if (!m || vkCreateDescriptorSetLayout(d, &dl, nullptr, &sl) != VK_SUCCESS || vkCreatePipelineLayout(d, &plc, nullptr, &pl) != VK_SUCCESS)
            return false;
        VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main" };
        ci.layout = pl;
        VkResult r = vkCreateComputePipelines(d, I.pipelineCache, 1, &ci, nullptr, &p);
        vkDestroyShaderModule(d, m, nullptr);
        return r == VK_SUCCESS;
    };
    VkDescriptorSetLayoutBinding rc[3] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                           { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                           { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    VkDescriptorSetLayoutBinding rd[4] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                           { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                           { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                           { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    VkDescriptorSetLayoutBinding db[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                           { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    VkDescriptorSetLayoutBinding mb[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                                           { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
    if (!computePipe(NativeResolveShader(false), "native_resolve_color", rc, 3, sizeof(NativeResolveConsts), I.resolveSetLayout[0][0],
                     I.resolvePipeLayout[0][0], I.resolvePipe[0][0]) ||
        !computePipe(NativeResolveShader(true), "native_resolve_depth", rd, 4, sizeof(NativeResolveConsts), I.resolveSetLayout[1][0],
                     I.resolvePipeLayout[1][0], I.resolvePipe[1][0]) ||
        !computePipe(NativeResolveShader(false, true), "native_resolve_color_ms", rc, 3, sizeof(NativeResolveConsts),
                     I.resolveSetLayout[0][1], I.resolvePipeLayout[0][1], I.resolvePipe[0][1]) ||
        !computePipe(NativeResolveShader(true, true), "native_resolve_depth_ms", rd, 4, sizeof(NativeResolveConsts),
                     I.resolveSetLayout[1][1], I.resolvePipeLayout[1][1], I.resolvePipe[1][1]) ||
        !computePipe(TextureDecodeShader(), "texdecode_cs", db, 2, 44, I.decodeSetLayout, I.decodePipeLayout, I.decodePipe) ||
        !computePipe(TextureMipShader(), "texmip_cs", mb, 2, 16, I.mipSetLayout, I.mipPipeLayout, I.mipPipe))
    { if (err) *err = "compute pipelines"; return false; }
    {
        // Full-precision depth textures need R32F as a storage image and with linear filtering.
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_R32_SFLOAT, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((fp.optimalTilingFeatures & need) != need ||
            !computePipe(TextureDecodeShader(true), "texdecode_depth_cs", db, 2, 44, I.depthDecodeSetLayout, I.depthDecodePipeLayout,
                         I.depthDecodePipe))
        {
            I.depthDecodePipe = VK_NULL_HANDLE;
            fprintf(stderr, "[native] depth textures: high byte only (R32F storage/filtering unavailable)\n");
        }
    }

    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = dev.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    for (Impl::Slot& sl : I.slots)
    {
        vkAllocateCommandBuffers(d, &cai, &sl.cb);
        vkAllocateCommandBuffers(d, &cai, &sl.upload);
        sl.spareCb.resize(Impl::kSpares);
        sl.spareUp.resize(Impl::kSpares);
        cai.commandBufferCount = Impl::kSpares;
        vkAllocateCommandBuffers(d, &cai, sl.spareCb.data());
        vkAllocateCommandBuffers(d, &cai, sl.spareUp.data());
        cai.commandBufferCount = 1;
    }
    I.Unpark(I.slots[0]);
    VkSemaphoreTypeCreateInfo sti{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &sti;
    if (vkCreateSemaphore(d, &sci, nullptr, &I.timeline) != VK_SUCCESS) { if (err) *err = "timeline semaphore"; return false; }
    VkSamplerCreateInfo nci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    vkCreateSampler(d, &nci, nullptr, &I.nearest);
    I.effects.Init(dev);
    if (const char* e = Hc09Env("HC09_CROWD"); (!e || atoi(e) != 0) && dev.geometryShader) I.crowdOn = true;
    if (const char* e = Hc09Env("HC09_CROWD_MIRROR")) I.crowdMirror = atoi(e) != 0;
    std::vector<std::vector<xenos::Image>> one(1, std::vector<xenos::Image>(1));
    one[0][0].width = one[0][0].height = 1;
    one[0][0].rgba.assign(1, 0u);
    for (uint32_t k = 0; k < kTexKinds; ++k)
        if (!I.Upload(I.dummy[k], k, one)) { if (err) *err = "dummy texture"; return false; }
    fprintf(stderr, "[native] renderer ready (%s shaders, depth format %s, %s, resolved textures %s, resolution x%u)\n", exact ? "exact" : "fast",
            I.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
            !I.msaa ? "no MSAA" : I.msaaCap2 ? "MSAA at most 2x" : "MSAA as the game asks", I.resolveGpu ? "on the GPU" : "in guest memory",
            I.scale);
    return true;
}

void NativeRenderer::SetTargets(const d3dn::Surface rt[4], const d3dn::Surface& depth)
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (!impl_) return;
    Impl::Lap lap(*impl_);
    impl_->SetTargets(rt, depth);
    lap.To(NativeProfile::kClear);
}
void NativeRenderer::Clear(uint32_t flags, const int32_t* rect, const float color[4], float z, uint32_t stencil)
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (!impl_) return;
    Impl::Lap lap(*impl_);
    impl_->Clear(flags, rect, color, z, stencil);
    lap.To(NativeProfile::kClear);
}
void NativeRenderer::Draw(const xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vs, uint64_t psHash, const xenos::ShaderCode* ps)
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (impl_) impl_->Draw(d, vsHash, vs, psHash, ps);
}
void NativeRenderer::Resolve(const d3dn::ResolveOp& r)
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (!impl_) return;
    Impl::Lap lap(*impl_);
    impl_->Resolve(r);
    lap.To(NativeProfile::kResolve);
}
void NativeRenderer::SetResolveWaits(bool wait)
{
    if (impl_) impl_->waitAtCopyResolves = wait;
}
void NativeRenderer::EndFrame()
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (!impl_) return;
    Impl& I = *impl_;
    Impl::Lap lap(I);
    I.Submit(false, NativeProfile::kWaitFrame);
    lap.To(NativeProfile::kSubmit);
    I.sceneView = {};
    if (I.motionOn) I.motion.Report();
    if (I.squeeze != 1.0f)
        for (auto& [k, t] : I.targets) t.wide = false;
    if (++I.frame % 60 == 0)
        for (auto it = I.textures.begin(); it != I.textures.end();)
        {
            if (it->second.lastUsed + 600 < I.frame) { I.garbage.push_back(it->second.img); it = I.textures.erase(it); ++I.texGen; }
            else ++it;
        }
    if (I.frame % 60 == 0)   // scaled copies unused for 600 frames
        for (auto it = I.scaledCopies.begin(); it != I.scaledCopies.end();)
        {
            auto next = std::next(it);
            if (it->second.lastUsed + 600 < I.frame) I.DropScaled(it);
            it = next;
        }
    if (I.frame % 60 == 0)   // index cache entries unused for 600 frames (their cache space comes back at the next reset)
        for (auto it = I.indexEntries.begin(); it != I.indexEntries.end();)
            it = it->second.lastUsed + 600 < I.frame ? I.indexEntries.erase(it) : std::next(it);
    if (I.drawProf && I.frame % 600 == 0 && !I.drawTimes.empty())
    {
        std::vector<std::pair<double, std::array<uint64_t, 4>>> top;
        double all = 0;
        for (auto& [k, v] : I.drawTimes) { top.push_back({ v.first, k }); all += v.first; }
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        fprintf(stderr, "[native-draws] GPU time in draws over 600 frames: %.2f ms/frame in %zu shader/target kinds; the top:\n",
                1000.0 * all / 600.0, top.size());
        for (size_t i = 0; i < top.size() && i < 15; ++i)
        {
            const auto& k = top[i].second;
            const auto& v = I.drawTimes[k];
            fprintf(stderr, "[native-draws]   %6.2f ms/frame %5.1f draws/frame %7.0f indices/draw  vs %016llX ps %016llX  %ux%u x%llu\n",
                    1000.0 * v.first / 600.0, double(v.second) / 600.0, double(I.drawIndices[k]) / double(v.second ? v.second : 1),
                    (unsigned long long)k[0], (unsigned long long)k[1], uint32_t(k[2] >> 32), uint32_t(k[2]), (unsigned long long)k[3]);
        }
        I.drawTimes.clear();
        I.drawIndices.clear();
    }
    if (I.prof && I.frame % 600 == 0)
    {
        fprintf(stderr, "[native-prof] index cache per frame: %.0f draws from the cache, %.1f converted into it, %.0f rewritten (ring);"
                        " %.1f of %.0f MiB used, %llu resets\n",
                I.idxHits / 600.0, I.idxConverted / 600.0, I.idxDynamic / 600.0, I.indexCacheUsed / 1048576.0,
                kIndexCacheBytes / 1048576.0, (unsigned long long)I.idxResets);
        I.idxHits = I.idxConverted = I.idxDynamic = 0;
        if (I.scale > 1)
        {
            fprintf(stderr, "[native-prof] resolution x%u: %zu scaled copies, %.0f MiB; %.1f fetches per frame decoded from them\n", I.scale,
                    I.scaledCopies.size(), I.scaledBytes / 1048576.0, I.scaledFetches / 600.0);
            I.scaledFetches = 0;
        }
    }
}
const Image* NativeRenderer::DecodeFrontBuffer(uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (!impl_) return nullptr;
    Impl::Lap lap(*impl_);
    bool ok = impl_->FrontBuffer(frontBuffer, width, height);
    impl_->frontGain = ok && impl_->gainFrame == impl_->frame;
    lap.To(NativeProfile::kFront);
    return ok ? &impl_->front : nullptr;
}
HdrGain NativeRenderer::SceneHdrGain() const
{
    HdrGain gain;
    float rect[4];
    if (!impl_ || !impl_->frontGain) return gain;
    gain.image = impl_->effects.HdrGain(rect);
    gain.x = rect[0];
    gain.y = rect[1];
    gain.width = rect[2];
    gain.height = rect[3];
    if (!gain.image) gain = {};
    return gain;
}
uint32_t NativeRenderer::Scale() const { return impl_ ? impl_->scale : 1; }
void NativeRenderer::SetMotionFrame(uint64_t tick, float fraction)
{
    if (!impl_) return;
    impl_->motionOn = true;
    impl_->motion.BeginFrame(impl_->frame, tick, fraction);
}
bool NativeRenderer::ReadFront(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height)
{
    return impl_ && impl_->ReadFront(rgba, width, height);
}
void NativeRenderer::Finish()
{
    if (impl_) impl_->Finish();
}
void NativeRenderer::SyncToGuest(uint32_t pa, uint32_t bytes)
{
    if (!impl_) return;
    if (impl_->resolveGpu) impl_->ReleaseOverlapping(pa & 0x1FFFFFFFu, bytes);
    impl_->SubmitAndWait(NativeProfile::kWaitTexture);
    impl_->Finish();
}
void NativeRenderer::SetOwnedCallback(void (*fn)(uint32_t, uint32_t))
{
    if (impl_) impl_->ownedCallback = fn;
}
uint64_t NativeRenderer::LastSubmit() const { return impl_ ? impl_->submitted : 0; }
bool NativeRenderer::Unsubmitted() const { return impl_ && (impl_->recording || impl_->uploading); }
void NativeRenderer::SubmitNow()
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (impl_ && (impl_->recording || impl_->uploading)) impl_->SubmitPartial();
}
void NativeRenderer::WaitSubmit(uint64_t value) const
{
    if (impl_) impl_->WaitValue(value);
}
const NativeProfile* NativeRenderer::Profile() const { return impl_ && impl_->prof ? &impl_->profile : nullptr; }

void NativeRenderer::Shutdown()
{
    if (!impl_) return;
    Impl& I = *impl_;
    VkDevice d = I.dev->device;
    vkDeviceWaitIdle(d);
    I.effects.Shutdown();
    if (I.crowdHistory.img.image) I.dev->DestroyImage(I.crowdHistory.img);
    I.Park(I.slots[I.cur]);
    for (Impl::Slot& sl : I.slots)
    {
        for (Buffer& b : sl.staging) I.dev->DestroyBuffer(b);
        for (Image& img : sl.garbage) I.dev->DestroyImage(img);
        sl.staging.clear();
        sl.garbage.clear();
    }
    for (auto& [k, v] : I.freeImages)
        for (auto& [img, f] : v) I.dev->DestroyImage(img);
    I.freeImages.clear();
    for (auto& [k, t] : I.textures) I.dev->DestroyImage(t.img);
    for (auto& [k, c] : I.scaledCopies) I.dev->DestroyBuffer(c.buf);
    I.scaledCopies.clear();
    for (auto& [k, t] : I.targets)
    {
        if (t.depthView) vkDestroyImageView(d, t.depthView, nullptr);
        if (t.stencilView) vkDestroyImageView(d, t.stencilView, nullptr);
        I.dev->DestroyImage(t.img);
    }
    I.dev->DestroyImage(I.front);
    for (Texture& t : I.dummy) I.dev->DestroyImage(t.img);
    for (auto& [k, s] : I.samplers) vkDestroySampler(d, s, nullptr);
    if (I.nearest) vkDestroySampler(d, I.nearest, nullptr);
    for (auto& [k, p] : I.pipelines) if (p) vkDestroyPipeline(d, p, nullptr);
    if (I.pipelineCache)
    {
        size_t size = 0;
        vkGetPipelineCacheData(d, I.pipelineCache, &size, nullptr);
        std::vector<char> data(size);
        if (size && vkGetPipelineCacheData(d, I.pipelineCache, &size, data.data()) == VK_SUCCESS)
        {
            std::error_code ec;
            std::filesystem::path path(I.pipelineCachePath);
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
            if (FILE* f = fopen(I.pipelineCachePath.c_str(), "wb")) { fwrite(data.data(), 1, size, f); fclose(f); }
        }
        vkDestroyPipelineCache(d, I.pipelineCache, nullptr);
    }
    for (auto& [k, s] : I.shaders) if (s.module) vkDestroyShaderModule(d, s.module, nullptr);
    if (I.rectGs) vkDestroyShaderModule(d, I.rectGs, nullptr);
    if (I.nullPs) vkDestroyShaderModule(d, I.nullPs, nullptr);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
        {
            if (I.resolvePipe[i][j]) vkDestroyPipeline(d, I.resolvePipe[i][j], nullptr);
            if (I.resolvePipeLayout[i][j]) vkDestroyPipelineLayout(d, I.resolvePipeLayout[i][j], nullptr);
            if (I.resolveSetLayout[i][j]) vkDestroyDescriptorSetLayout(d, I.resolveSetLayout[i][j], nullptr);
        }
    if (I.decodePipe) vkDestroyPipeline(d, I.decodePipe, nullptr);
    if (I.depthDecodePipe) vkDestroyPipeline(d, I.depthDecodePipe, nullptr);
    if (I.depthDecodePipeLayout) vkDestroyPipelineLayout(d, I.depthDecodePipeLayout, nullptr);
    if (I.depthDecodeSetLayout) vkDestroyDescriptorSetLayout(d, I.depthDecodeSetLayout, nullptr);
    if (I.mipPipe) vkDestroyPipeline(d, I.mipPipe, nullptr);
    if (I.decodePipeLayout) vkDestroyPipelineLayout(d, I.decodePipeLayout, nullptr);
    if (I.mipPipeLayout) vkDestroyPipelineLayout(d, I.mipPipeLayout, nullptr);
    if (I.decodeSetLayout) vkDestroyDescriptorSetLayout(d, I.decodeSetLayout, nullptr);
    if (I.mipSetLayout) vkDestroyDescriptorSetLayout(d, I.mipSetLayout, nullptr);
    I.dev->DestroyBuffer(I.consts);
    I.dev->DestroyBuffer(I.vram);
    I.dev->DestroyBuffer(I.indices);
    if (I.timeline) vkDestroySemaphore(d, I.timeline, nullptr);
    if (I.tsPool) vkDestroyQueryPool(d, I.tsPool, nullptr);
    for (Impl::Slot& sl : I.slots)
    {
        vkFreeCommandBuffers(d, I.dev->pool, 1, &sl.cb);
        vkFreeCommandBuffers(d, I.dev->pool, 1, &sl.upload);
        vkFreeCommandBuffers(d, I.dev->pool, Impl::kSpares, sl.spareCb.data());
        vkFreeCommandBuffers(d, I.dev->pool, Impl::kSpares, sl.spareUp.data());
        vkDestroyDescriptorPool(d, sl.pool, nullptr);
        vkDestroyDescriptorPool(d, sl.computePool, nullptr);
    }
    vkDestroyPipelineLayout(d, I.pipeLayout, nullptr);
    vkDestroyDescriptorSetLayout(d, I.setLayout, nullptr);
    impl_.reset();
}

NativeRenderer::NativeRenderer() = default;
NativeRenderer::~NativeRenderer() { Shutdown(); }

void SetTextureOverride(const TextureOverride& o) { g_texOverride = o; }

}  // namespace hc09vk
