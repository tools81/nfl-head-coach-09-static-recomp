#include "../dev_env.h"
#include "vk_edram.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "vk_shader_compile.h"
#include "xe_glsl.h"
#include "xenos.h"
#include "xenos_texture.h"

namespace hc09vk {

namespace {

inline uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline int32_t SBits(uint32_t v, int lo, int n) { return int32_t(v << (32 - lo - n)) >> (32 - n); }
inline float AsF(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t Reg(uint32_t i) { return xenos::Reg(i); }

namespace reg = xenos::reg;
const uint32_t kColorInfoRegs[4] = { reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO };
const uint32_t kBlendRegs[4] = { reg::RB_BLENDCONTROL0, reg::RB_BLENDCONTROL1, reg::RB_BLENDCONTROL2, reg::RB_BLENDCONTROL3 };

// Per-frame rings: an in-game frame has up to ~6,000 draws; a full ring means
// a submit + wait in the middle of the frame.
constexpr uint32_t kMaxDraws = 8192;
constexpr uint32_t kMaxSets = 4096;   // descriptor sets per frame (shared by draws binding the same textures)
constexpr VkDeviceSize kIndexRingBytes = 64ull << 20;
constexpr VkDeviceSize kEdramBytes = 2048ull * 80 * 16 * 4;
// Clip space maps 1:1 to pixels: viewport (-K, -K, 2K, 2K) gives x_window = x_ndc * K.
constexpr float kClipScale = 8192.0f;
constexpr uint32_t kRenderArea = 8192;

// Adds the scope's duration to `acc` (the renderer's own CPU time, waits included).
struct ScopeTimer
{
    double& acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~ScopeTimer() { acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};

struct ShaderEntry
{
    VkShaderModule module = VK_NULL_HANDLE;
    TranslatedShader meta;
    bool ok = false;
};

// Texture kinds as the shaders see them (DrawConsts::texSize[slot].z), chosen
// by the fetch constant like the software sampler: 3D volumes (bilinear in
// the two nearest slices, linear between, level 0), cube maps (faces as array
// layers, face = floor(coordinate z)), everything else 2D. Stacked textures
// are 2D arrays bound like cube maps; the shaders see kind 3 (kTexStacked).
enum TexKind : uint32_t { kTex2D = 0, kTex3D = 1, kTexCube = 2, kTexKinds = 3, kTexStacked = 3 };

// Hash of a sequence of integers: the keys of the renderer's caches (hash
// maps: a std::map lookup over ~1,000 textures cost ~1 us per draw in tree-node
// cache misses).
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

struct Texture
{
    Image img;
    uint32_t width = 0, height = 0, depth = 1, kind = kTex2D;
    bool stacked = false;          // kind kTexCube holding the layers of a stacked texture
    uint32_t pa = 0, bytes = 0;    // guest memory it was decoded from (level 0, all faces/slices)
    uint64_t epoch = 0;            // xenos::HostTrackWrites at decode: stale once a page is written after it
    uint64_t lastUsed = 0;         // frame number (eviction)
};

// Box-filtered mips from level 0 up to `maxLevel` (xenos_raster.cpp GetTexture).
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

// Global memory dependency between everything before and after (draws,
// resolves and clears all go through storage buffers).
void MemoryBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst)
{
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = ((src & VK_PIPELINE_STAGE_TRANSFER_BIT) ? VK_ACCESS_TRANSFER_WRITE_BIT : 0) |
                      ((src & ~VkPipelineStageFlags(VK_PIPELINE_STAGE_TRANSFER_BIT)) ? VK_ACCESS_SHADER_WRITE_BIT : 0);
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
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

}  // namespace

struct EdramRenderer::Impl
{
    Device* dev = nullptr;
    Buffer guest, edram, consts, indices;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE, resolveSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE, resolvePipeLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE, resolvePool = VK_NULL_HANDLE;
    VkDescriptorSet resolveSet = VK_NULL_HANDLE;
    VkPipeline resolvePipe = VK_NULL_HANDLE;
    // GPU texture decode (textures written by a resolve still pending on the GPU).
    VkDescriptorSetLayout decodeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout decodePipeLayout = VK_NULL_HANDLE;
    VkDescriptorPool decodePool = VK_NULL_HANDLE;
    VkPipeline decodePipe = VK_NULL_HANDLE;
    // k_24_8 depth textures at full precision into R32F (null: the GPU cannot; the RGBA8 high byte instead).
    VkPipeline depthDecodePipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout mipSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mipPipeLayout = VK_NULL_HANDLE;
    VkPipeline mipPipe = VK_NULL_HANDLE;
    uint32_t decodeSetsUsed = 0;
    static constexpr uint32_t kDecodeSets = 512;
    std::vector<std::pair<uint32_t, uint32_t>> pendingWrites;   // resolve destinations not yet waited for
    bool waitAtCopyResolves = true;
    VkQueryPool timestamps = VK_NULL_HANDLE;   // GPU time of each submitted frame command buffer
    // HC09_GPU_PROFILE=1: a timestamp after every draw/resolve; GPU time per
    // pixel shader and the most expensive draws, printed at shutdown.
    VkQueryPool profPool = VK_NULL_HANDLE;
    static constexpr uint32_t kProfQueries = 16384;
    struct ProfEntry { uint64_t ps; uint32_t draw; uint32_t tris; };
    std::vector<ProfEntry> profPending;   // entries for queries 1.. of the command buffer being recorded
    std::map<uint64_t, std::pair<double, uint32_t>> profByPs;   // seconds, draws
    std::vector<std::pair<double, ProfEntry>> profTop;
    uint32_t profDraw = 0;
    void ProfMark(uint64_t ps, uint32_t tris)
    {
        if (!profPool || profPending.size() + 1 >= kProfQueries) return;
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profPool, uint32_t(profPending.size() + 1));
        profPending.push_back({ ps, profDraw, tris });
    }
    // HC09_GPU_CPUPROF=1: CPU time per section of Draw (RendererStats::drawSections).
    bool cpuProf = false;
    std::chrono::steady_clock::time_point sectionStart;
    void Section(int i)
    {
        if (!cpuProf) return;
        auto t = std::chrono::steady_clock::now();
        if (i >= 0) stats->drawSections[i] += std::chrono::duration<double>(t - sectionStart).count();
        sectionStart = t;
    }
    bool timed = false;   // the CPU reads what each copy wrote (replays: resolve PNGs)
    VkShaderModule rectGs = VK_NULL_HANDLE, nullPs = VK_NULL_HANDLE;
    VkDeviceSize constStride = 0;
    uint32_t constUsed = 0;
    VkDeviceSize indexUsed = 0;
    VkCommandBuffer cb = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;   // = cbs[chunk], uploads[chunk]
    // Graphics state bound in `cb` (state persists across render pass instances
    // within a command buffer): binds that would not change it are skipped.
    VkPipeline boundPipe = VK_NULL_HANDLE;
    VkRect2D boundScissor{};
    bool graphicsStatic = false;   // viewport and the index ring are bound
    VkFence fence = VK_NULL_HANDLE;
    // Chunked submission (HC09_GPU_CHUNK=n draws, default 256; 0 = off): the
    // frame's commands go to the GPU in pieces without a wait, so the GPU
    // executes one while the CP records the next, as the console's GPU runs
    // behind its command processor. Every wait (frame end, full ring, a texture
    // a pending resolve wrote) still waits for everything submitted, so what the
    // CPU can observe is unchanged. Each chunk starts with a full memory barrier
    // (EDRAM and guest-memory writes of the previous one).
    static constexpr uint32_t kMaxChunks = 64;
    VkCommandBuffer cbs[kMaxChunks] = {}, uploads[kMaxChunks] = {};
    uint32_t chunk = 0, chunkDraws = 256, drawsInChunk = 0;
    void SubmitChunk()
    {
        EndRendering();
        VkCommandBuffer list[2];
        uint32_t n = 0;
        if (uploading) { vkEndCommandBuffer(upload); list[n++] = upload; uploading = false; }
        if (timestamps) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 2 * chunk + 1);
        vkEndCommandBuffer(cb);
        list[n++] = cb;
        recording = false;
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = n;
        si.pCommandBuffers = list;
        vkQueueSubmit(dev->queue, 1, &si, VK_NULL_HANDLE);
        ++chunk;
        cb = cbs[chunk];
        upload = uploads[chunk];
        drawsInChunk = 0;
    }
    bool recording = false, uploading = false, rendering = false;
    std::vector<Buffer> staging;   // texture uploads waiting for the next submit
    std::unordered_map<std::pair<uint64_t, int>, ShaderEntry, WordsHash> shaders;
    std::unordered_map<std::array<uint64_t, 4>, VkPipeline, WordsHash> pipelines;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;   // saved to pipelineCachePath at shutdown
    std::string pipelineCachePath;
    std::unordered_map<std::array<uint32_t, 7>, Texture, WordsHash> textures;
    std::unordered_map<uint64_t, VkSampler> samplers;
    Texture dummy[kTexKinds];      // 1x1 transparent black of each kind: unused or undecodable slots
    std::vector<Image> garbage;    // replaced/evicted images, destroyed after the next GPU wait
    uint64_t frame = 0;            // EndFrame count
    std::vector<std::string> warned;
    RendererStats* stats = nullptr;
    uint32_t drawCalls = 0;
    std::vector<uint32_t> idxBuf;   // per-draw scratch (fans, strips, quads before assembly)
    // Descriptor image infos of unused texture slots (dummy of each kind and its
    // sampler), the template for every new descriptor set; made at the first draw.
    bool slotTemplate = false;
    VkDescriptorImageInfo dummyImages[kTexKinds][32], dummyUImages[kTexKinds][32];
    std::unordered_map<std::vector<uint64_t>, VkDescriptorSet, WordsHash> descSets;   // per texture binding set (see Draw)
    std::vector<uint64_t> descKey;
    VkSampler nearest = VK_NULL_HANDLE;   // for the RGBA8_UINT views (texelFetch only)
    TranslateOptions translate;   // exact (software-identical) or fast (EdramRenderer::Init)
    std::array<uint32_t, 7> lastLayout{};   // EDRAM layout of the previous draw (see Draw)

    void Skip(const char* why)
    {
        ++stats->skipped;
        ++stats->skipReasons[why];
    }

    void Warn(const std::string& what)
    {
        if (std::find(warned.begin(), warned.end(), what) != warned.end()) return;
        warned.push_back(what);
        fprintf(stderr, "[gpu] %s\n", what.c_str());
    }

    // ---- command recording / submission ------------------------------------
    void Begin()
    {
        if (recording) return;
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        recording = true;
        boundPipe = VK_NULL_HANDLE;   // graphics state of a new command buffer: nothing bound yet
        boundScissor = { { -1, -1 }, { 0, 0 } };
        graphicsStatic = false;
        if (timestamps)
        {
            if (chunk == 0) vkCmdResetQueryPool(cb, timestamps, 0, 2 * kMaxChunks);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 2 * chunk);
        }
        if (chunk > 0)
            MemoryBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        if (profPool)
        {
            vkCmdResetQueryPool(cb, profPool, 0, kProfQueries);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, profPool, 0);
            profPending.clear();
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
    // Submit texture uploads and the recorded frame, wait, recycle the rings.
    void SubmitAndWait()
    {
        EndRendering();
        if (chunk > 0 && !recording) Begin();   // chunks went out without a fence: this one carries it
        VkCommandBuffer list[2];
        uint32_t n = 0;
        if (uploading) { vkEndCommandBuffer(upload); list[n++] = upload; uploading = false; }
        if (recording)
        {
            // Resolves wrote guest memory, which the CPU reads next (texture decoding,
            // resolve PNGs): device writes must be made visible to the host.
            VkMemoryBarrier hb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
            if (timestamps) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 2 * chunk + 1);
            timed = timestamps != VK_NULL_HANDLE;
            vkEndCommandBuffer(cb);
            list[n++] = cb;
            recording = false;
        }
        uint32_t chunks = chunk + 1;   // command buffers with timestamps when `timed`
        chunk = 0;
        cb = cbs[0];
        upload = uploads[0];
        drawsInChunk = 0;
        if (n)
        {
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = n;
            si.pCommandBuffers = list;
            vkResetFences(dev->device, 1, &fence);
            vkQueueSubmit(dev->queue, 1, &si, fence);
            auto t0 = std::chrono::steady_clock::now();
            vkWaitForFences(dev->device, 1, &fence, VK_TRUE, UINT64_MAX);
            stats->waitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ++stats->syncs;
            if (timed)
            {
                uint64_t ts[2 * kMaxChunks];
                if (vkGetQueryPoolResults(dev->device, timestamps, 0, 2 * chunks, 16 * chunks, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    for (uint32_t c = 0; c < chunks; ++c)
                        stats->gpuSeconds += double(ts[2 * c + 1] - ts[2 * c]) * double(dev->props.limits.timestampPeriod) * 1e-9;
                timed = false;
            }
            if (profPool && !profPending.empty())
            {
                std::vector<uint64_t> ts(profPending.size() + 1);
                if (vkGetQueryPoolResults(dev->device, profPool, 0, uint32_t(ts.size()), ts.size() * 8, ts.data(), 8,
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    for (size_t i = 0; i < profPending.size(); ++i)
                    {
                        double sec = double(ts[i + 1] - ts[i]) * double(dev->props.limits.timestampPeriod) * 1e-9;
                        auto& e = profByPs[profPending[i].ps];
                        e.first += sec;
                        ++e.second;
                        profTop.push_back({ sec, profPending[i] });
                    }
                profPending.clear();
            }
        }
        for (Buffer& b : staging) dev->DestroyBuffer(b);
        staging.clear();
        constUsed = 0;
        indexUsed = 0;
        vkResetDescriptorPool(dev->device, pool, 0);
        descSets.clear();
        for (Image& img : garbage) dev->DestroyImage(img);
        garbage.clear();
        pendingWrites.clear();
        if (decodeSetsUsed) { vkResetDescriptorPool(dev->device, decodePool, 0); decodeSetsUsed = 0; }
    }

    bool Pending(uint32_t pa, uint32_t bytes) const
    {
        for (const auto& [p, n] : pendingWrites)
            if (pa < p + n && p < pa + bytes) return true;
        return false;
    }

    // k_8_8_8_8 / k_24_8 2D texture decoded by a compute shader from guest
    // memory into a new image (level 0), with its box-filtered mips made by
    // another (as MakeMips), recorded in the frame's command buffer (after the
    // resolve that wrote it, before the draw that samples it).
    bool DecodeOnGpu(Texture& t, const xenos::TextureFetch& tf)
    {
        uint32_t w = tf.width(), h = tf.height();
        // Levels as MakeMips: up to the fetch constant's max level, stopping at 1x1.
        uint32_t maxLevel = Bits(tf.dword[4], 6, 4), levels = 1;
        for (uint32_t lw = w, lh = h; levels <= maxLevel && !(lw == 1 && lh == 1); ++levels) { lw = std::max(1u, lw / 2); lh = std::max(1u, lh / 2); }
        if (decodeSetsUsed + levels > kDecodeSets) return false;
        // A depth texture (k_24_8) is read as the whole 24-bit depth: R32F, one level (the mip shader is RGBA8).
        bool depth = tf.format() == 22 && depthDecodePipe && levels == 1;
        if (!dev->CreateImage(VK_IMAGE_VIEW_TYPE_2D, w, h, 1, depth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, t.img, levels, VK_IMAGE_ASPECT_COLOR_BIT,
                              depth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UINT))
            return false;
        for (uint32_t l = 0; l < levels; ++l)
        {
            VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = t.img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = depth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UINT;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1 };
            VkImageView v = VK_NULL_HANDLE;
            if (vkCreateImageView(dev->device, &vi, nullptr, &v) != VK_SUCCESS) { dev->DestroyImage(t.img); return false; }
            t.img.levelViews.push_back(v);
        }
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = decodePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &decodeSetLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev->device, &ai, &set) != VK_SUCCESS) { dev->DestroyImage(t.img); return false; }
        ++decodeSetsUsed;
        VkDescriptorBufferInfo bMem{ guest.buffer, 0, VK_WHOLE_SIZE };
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
        EndRendering();
        Begin();
        MemoryBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.img.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, depth ? depthDecodePipe : decodePipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipeLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, decodePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        // Mips: level l from level l - 1.
        uint32_t sw = w, sh = h;
        for (uint32_t l = 1; l < levels; ++l)
        {
            uint32_t lw = std::max(1u, sw / 2), lh = std::max(1u, sh / 2);
            VkImageMemoryBarrier mb = b;
            mb.oldLayout = mb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            mb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 1, 0, 1 };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &mb);
            VkDescriptorSet ms = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo mai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            mai.descriptorPool = decodePool;
            mai.descriptorSetCount = 1;
            mai.pSetLayouts = &mipSetLayout;
            if (vkAllocateDescriptorSets(dev->device, &mai, &ms) != VK_SUCCESS) break;   // pool sized above; not expected
            ++decodeSetsUsed;
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
        return true;
    }

    // XE_SWAP front buffer -> `front` (see EdramRenderer::DecodeFrontBuffer):
    // the texture decode shader with xenos::DecodeFrontBuffer's fetch constant
    // (tiled 8888, pitch rounded to 32, endian 0) and swizzle zyx1.
    Image front;
    bool FrontBuffer(uint32_t base, uint32_t w, uint32_t h)
    {
        if (!w || !h || decodeSetsUsed + 1 > kDecodeSets) return false;
        if (front.image && (front.width != w || front.height != h)) { garbage.push_back(front); front = Image{}; }
        if (!front.image &&
            !dev->CreateImage(VK_IMAGE_VIEW_TYPE_2D, w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, front, 1, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_FORMAT_R8G8B8A8_UINT))
            return false;
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = decodePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &decodeSetLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev->device, &ai, &set) != VK_SUCCESS) return false;
        ++decodeSetsUsed;
        VkDescriptorBufferInfo bMem{ guest.buffer, 0, VK_WHOLE_SIZE };
        VkDescriptorImageInfo img{ VK_NULL_HANDLE, front.altView, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet wr[2]{};
        for (auto& x : wr) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = set; x.descriptorCount = 1; }
        wr[0].dstBinding = 0; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[0].pBufferInfo = &bMem;
        wr[1].dstBinding = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = &img;
        vkUpdateDescriptorSets(dev->device, 2, wr, 0, nullptr);
        uint32_t pitch = (w + 31) & ~31u;
        uint32_t pc[11] = { base & 0x1FFFF000u, pitch, pitch * 4, 1u, w, h, 0u, 2u | (1u << 3) | (0u << 6) | (5u << 9), 6u, 0u, 0u };
        EndRendering();
        Begin();
        // After the resolve that wrote it; the previous frame's blit from the image is done reading.
        MemoryBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ImageBarrier(cb, front.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipeLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, decodePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        ImageBarrier(cb, front.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        return true;
    }

    // Drop decoded textures overlapping [pa, pa + bytes) (a resolve wrote there).
    void InvalidateTextures(uint32_t pa, uint32_t bytes)
    {
        for (auto it = textures.begin(); it != textures.end();)
        {
            const Texture& t = it->second;
            if (t.pa < pa + bytes && pa < t.pa + t.bytes)
            {
                garbage.push_back(t.img);
                it = textures.erase(it);
            }
            else ++it;
        }
    }

    // ---- shaders, pipelines, textures ---------------------------------------
    VkShaderModule Module(const std::string& glsl, Stage stage, const char* name)
    {
        std::vector<uint32_t> spirv;
        std::string log;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = CompileGlsl(glsl, stage, name, spirv, &log);
        stats->compileSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!ok)
        {
            Warn(std::string("shader ") + name + " failed: " + log);
            return VK_NULL_HANDLE;
        }
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
        char name[64];
        snprintf(name, sizeof(name), "%s_%016llx", vs ? "vs" : "ps", (unsigned long long)hash);
        if (!e.meta.error.empty()) { Warn(std::string("shader ") + name + " failed: " + e.meta.error); return nullptr; }
        e.module = Module(e.meta.glsl, vs ? Stage::kVertex : Stage::kFragment, name);
        if (!e.module) return nullptr;
        if (e.meta.unsupported) Warn(std::string("shader ") + name + " uses an untranslated vertex format");
        e.ok = true;
        ++stats->shaders;
        return &e;
    }

    VkPipeline Pipeline(uint64_t vsHash, uint64_t psHash, uint32_t rect, uint32_t cull, uint32_t frontFace, uint64_t texKinds,
                        VkShaderModule vsMod, VkShaderModule psMod)
    {
        std::array<uint64_t, 4> key = { vsHash, psHash, uint64_t(rect) | (uint64_t(cull) << 8) | (uint64_t(frontFace) << 16), texKinds };
        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second;
        uint32_t kinds[2] = { uint32_t(texKinds), uint32_t(texKinds >> 32) };
        VkSpecializationMapEntry me[2] = { { 0, 0, 4 }, { 1, 4, 4 } };
        VkSpecializationInfo spec{ 2, me, sizeof(kinds), kinds };
        VkPipelineShaderStageCreateInfo st[3]{};
        uint32_t n = 0;
        st[n] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsMod, "main", &spec }; ++n;
        if (rect) { st[n] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT, rectGs, "main" }; ++n; }
        st[n] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psMod, "main", &spec }; ++n;
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.depthClampEnable = VK_TRUE;   // no z clipping by Vulkan: the clip distances do the software's near/far clipping
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = cull;
        rs.frontFace = VkFrontFace(frontFace);
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };   // no attachments
        VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.pNext = &ri;
        ci.stageCount = n;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
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

    // `layers[layer][level]`: cube faces, stacked layers, 3D slices (level 0 only) or one 2D layer.
    bool Upload(Texture& t, uint32_t kind, const std::vector<std::vector<xenos::Image>>& layers)
    {
        uint32_t nLayers = uint32_t(layers.size()), nLevels = uint32_t(layers[0].size());
        const xenos::Image& base = layers[0][0];
        VkImageViewType vt = kind == kTex3D ? VK_IMAGE_VIEW_TYPE_3D : kind == kTexCube ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (!dev->CreateImage(vt, base.width, base.height, nLayers, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, t.img, nLevels, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_FORMAT_R8G8B8A8_UINT))
            return false;
        VkDeviceSize total = 0;
        for (auto& l : layers) for (auto& m : l) total += VkDeviceSize(m.width) * m.height * 4;
        Buffer buf;
        if (!dev->CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf))
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
        // Recorded into the upload command buffer, which runs before the frame's.
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

    // The texture of a fetch constant, decoded and mipmapped like the
    // software sampler (runtime/xenos/xenos_raster.cpp GetTexture): every cube
    // face, stacked layer or 3D slice through DecodeTextureLayer, box-filtered mips from
    // level 0 (none for 3D).
    const Texture* GetTexture(const xenos::TextureFetch& tf)
    {
        if (!tf.valid()) return &dummy[kTex2D];
        // HC09_TRACE_TEXRANGE=<lo>-<hi> (hex physical addresses): log each distinct
        // texture whose base lies in the range (which textures sample that memory);
        // HC09_TRACE_TEXSIZE=<w>x<h> only those of that size.
        static const std::pair<uint32_t, uint32_t> traceRange = [] {
            const char* e = Hc09Env("HC09_TRACE_TEXRANGE");
            uint32_t lo = 0, hi = 0;
            if (e && sscanf(e, "%x-%x", &lo, &hi) == 2) return std::make_pair(lo, hi);
            return std::make_pair(1u, 0u);
        }();
        static const std::pair<uint32_t, uint32_t> traceSize = [] {
            const char* e = Hc09Env("HC09_TRACE_TEXSIZE");
            uint32_t w = 0, h = 0;
            if (e && sscanf(e, "%ux%u", &w, &h) == 2) return std::make_pair(w, h);
            return std::make_pair(0u, 0u);
        }();
        if (uint32_t b = tf.baseAddress() & 0x1FFFFFFFu;
            b >= traceRange.first && b < traceRange.second && (!traceSize.first || (tf.width() == traceSize.first && tf.height() == traceSize.second)))
        {
            static std::set<std::array<uint32_t, 6>> seen;
            std::array<uint32_t, 6> k = { tf.dword[0], tf.dword[1], tf.dword[2], tf.dword[3], tf.dword[4], tf.dword[5] };
            if (seen.size() < 64 && seen.insert(k).second)
            {
                fprintf(stderr, "[tex-trace] base pa 0x%08X fmt %u %ux%u %s pitch %u endian %u dim %u swizzle 0x%03X mips 0x%08X (fetch %08X %08X %08X %08X %08X %08X)\n",
                        b, tf.format(), tf.width(), tf.height(), tf.tiled() ? "tiled" : "linear", tf.pitchTexels(), tf.endian(),
                        tf.dimension(), tf.swizzle(), tf.mipAddress(), k[0], k[1], k[2], k[3], k[4], k[5]);
                // The first bytes of its page and of the pages around it (data written one page off).
                for (int32_t d : { -0x1000, 0, 0x1000 })
                {
                    uint32_t p = (b + uint32_t(d)) & 0x1FFFFFFFu;
                    const uint8_t* m = xenos::Phys(p);
                    std::string hex;
                    char h[4];
                    int nz = 0, first = -1;
                    for (int i = 0; i < 0x1000 && m; i += 4)
                        if (m[i] | m[i + 1] | m[i + 2] | m[i + 3]) { ++nz; if (first < 0) first = i; }
                    for (int i = 0; i < 32 && m; ++i) { snprintf(h, sizeof(h), "%02X", m[(first > 0 ? first : 0) + i]); hex += h; if (i % 4 == 3) hex += ' '; }
                    fprintf(stderr, "[tex-trace]   pa 0x%08X: %d non-zero words, from +0x%X: %s\n", p, nz, first, hex.c_str());
                }
            }
        }
        std::array<uint32_t, 7> key = { tf.dword[0] & ~0x0007FC00u, tf.dword[1], tf.dword[2], tf.dword[3] & 0x7FFFFu,
                                        tf.dword[4] & 0x3FCu, tf.dword[5] & ~0x1FFu, 0 };
        auto it = textures.find(key);
        if (it != textures.end())
        {
            Texture& c = it->second;
            if (!xenos::HostWrittenSince(c.pa, c.bytes, c.epoch))   // the CPU has not rewritten it
            {
                c.lastUsed = frame;
                return c.img.image ? &c : &dummy[kTex2D];
            }
            garbage.push_back(c.img);
            textures.erase(it);
            ++stats->texturesRedecoded;
        }
        Texture& t = textures[key];
        t.lastUsed = frame;
        // Track CPU writes before reading: a write from here on marks it stale.
        t.pa = tf.baseAddress() & 0x1FFFFFFFu;
        t.bytes = xenos::TextureBytes(tf);
        t.epoch = xenos::HostTrackWrites(t.pa, t.bytes);
        auto t0 = std::chrono::steady_clock::now();
        struct Timer { std::chrono::steady_clock::time_point t0; double* acc;
                       ~Timer() { *acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); } } timer{ t0, &stats->textureSeconds };
        uint32_t kind = tf.is3D() ? kTex3D : tf.dimension() == 3 || tf.stacked() ? kTexCube : kTex2D;
        t.stacked = tf.stacked();
        // k_8_8_8_8 / k_24_8 2D (render targets and depth resolved to textures, UI): decoded
        // by the GPU straight from guest memory — no CPU decode, no upload, and no
        // wait when a pending resolve wrote it. Same bytes as the CPU decoder.
        if (kind == kTex2D && tf.dimension() == 1 && (tf.format() == 6 || tf.format() == 22) && DecodeOnGpu(t, tf))
            return &t;
        if (Pending(t.pa, t.bytes)) SubmitAndWait();   // written by a resolve the GPU may not have finished
        uint32_t n = tf.layers();
        std::vector<std::vector<xenos::Image>> layers(n, std::vector<xenos::Image>(1));
        for (uint32_t l = 0; l < n; ++l)
        {
            std::string why;
            if (!xenos::DecodeTextureLayer(tf, l, layers[l][0], &why)) { Warn("texture not decoded: " + why); return &dummy[kTex2D]; }
            if (kind != kTex3D) MakeMips(layers[l], Bits(tf.dword[4], 6, 4));
        }
        if (!Upload(t, kind, layers)) { Warn("texture upload failed"); return &dummy[kTex2D]; }
        ++stats->textures;
        double ms = 1e3 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 5.0 && slowLogged < 40)
        {
            ++slowLogged;
            fprintf(stderr, "[gpu] slow texture decode %.1f ms: fmt %u %ux%ux%u dim %u kind %u levels %u pa 0x%08X\n", ms, tf.format(),
                    tf.width(), tf.height(), tf.depth(), tf.dimension(), kind, uint32_t(layers[0].size()), t.pa);
        }
        return &t;
    }
    uint32_t slowLogged = 0;

    VkSampler Sampler(const xenos::TextureFetch& tf, uint8_t override, uint32_t levels, uint32_t kind)
    {
        uint32_t magF = (override & 3) == 3 ? Bits(tf.dword[3], 19, 2) : (override & 3);
        uint32_t minF = ((override >> 2) & 3) == 3 ? Bits(tf.dword[3], 21, 2) : ((override >> 2) & 3);
        uint32_t mipF = ((override >> 4) & 3) == 3 ? Bits(tf.dword[3], 23, 2) : ((override >> 4) & 3);
        uint32_t cx = Bits(tf.dword[0], 10, 3), cy = Bits(tf.dword[0], 13, 3), cz = Bits(tf.dword[0], 16, 3);
        uint32_t minLevel = std::min(Bits(tf.dword[4], 2, 4), levels - 1), maxLevel = std::min(Bits(tf.dword[4], 6, 4), levels - 1);
        if (maxLevel < minLevel) maxLevel = minLevel;
        if (mipF == 2) maxLevel = minLevel;
        int32_t bias = SBits(tf.dword[4], 12, 10);
        if (kind == kTex3D) { minF = magF; mipF = 2; minLevel = maxLevel = 0; bias = 0; }   // software: magnification filter, level 0
        else cz = 2;
        bool borderWhite = Bits(tf.dword[5], 0, 2) == 1;
        uint64_t key = magF | (minF << 2) | (mipF << 4) | (cx << 6) | (cy << 9) | (uint64_t(minLevel) << 12) | (uint64_t(maxLevel) << 16) |
                       (uint64_t(uint32_t(bias) & 0x3FF) << 20) | (uint64_t(cz) << 30) | (uint64_t(borderWhite) << 33);
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
        if (minLevel) Warn("texture with a minimum mip level > 0 (sampled from level 0 when magnified)");
        VkSampler smp = VK_NULL_HANDLE;
        vkCreateSampler(dev->device, &ci, nullptr, &smp);
        samplers[key] = smp;
        return smp;
    }

    // ---- draws --------------------------------------------------------------
    // The draw's indices (offset, clamp to [min, max], indices past the buffer
    // read as 0 — as xenos_raster.cpp) assembled into a triangle list at `out`
    // (the count Draw computed from numIndices and primType). Guest physical
    // memory is one contiguous host range, so the index buffer is read in place;
    // Phys() still sees every page (a frame capture records them).
    void WriteTriangleIndices(const xenos::DrawPacket& d, uint32_t* out)
    {
        uint32_t n = d.numIndices;
        uint32_t minI = Reg(reg::VGT_MIN_VTX_INDX), maxI = Reg(reg::VGT_MAX_VTX_INDX), off = Reg(reg::VGT_INDX_OFFSET);
        auto clampI = [=](uint32_t v) { return std::min(maxI, std::max(minI, (v + off) & 0xFFFFFF)); };
        // Lists go straight to `out`; the other types through idxBuf.
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
            uint64_t end = uint64_t(base) + uint64_t(avail) * sz;
            uint64_t endRead = uint64_t(base) + uint64_t(std::min(n, d.dmaWords)) * sz;   // as the software reads
            const uint8_t* src = nullptr;
            if (avail)
            {
                for (uint64_t p = base & ~0xFFFu; p < endRead; p += 0x1000) xenos::Phys(uint32_t(p & 0x1FFFFFFFu));
                if (end <= 0x20000000ull) src = xenos::Phys(base);
            }
            uint32_t i = 0;
            if (src && sz == 2)
            {
                bool swap = d.dmaEndian == 1 || d.dmaEndian == 2;
                for (; i < avail; ++i)
                {
                    uint16_t raw;
                    memcpy(&raw, src + 2 * size_t(i), 2);
                    idx[i] = clampI(swap ? uint16_t((raw >> 8) | (raw << 8)) : raw);
                }
            }
            else if (src)
            {
                uint32_t e = d.dmaEndian;
                for (; i < avail; ++i)
                {
                    uint32_t raw;
                    memcpy(&raw, src + 4 * size_t(i), 4);
                    idx[i] = clampI(e == 1 ? (((raw & 0x00FF00FF) << 8) | ((raw & 0xFF00FF00) >> 8)) : e == 2 ? xenos::Bswap32(raw)
                                    : e == 3 ? (raw << 16) | (raw >> 16) : raw);
                }
            }
            else
                for (; i < avail; ++i)   // wraps past the end of physical memory: page by page
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
        case 5:   // fan
            for (uint32_t i = 1; i + 1 < n; ++i) { *out++ = idx[0]; *out++ = idx[i]; *out++ = idx[i + 1]; }
            break;
        case 6:   // strip: every other triangle reversed (keeps the winding)
            for (uint32_t i = 0; i + 2 < n; ++i)
            {
                if (i & 1) { *out++ = idx[i + 1]; *out++ = idx[i]; }
                else { *out++ = idx[i]; *out++ = idx[i + 1]; }
                *out++ = idx[i + 2];
            }
            break;
        case 13:   // quads
            for (uint32_t i = 0; i + 3 < n; i += 4)
            {
                *out++ = idx[i]; *out++ = idx[i + 1]; *out++ = idx[i + 2];
                *out++ = idx[i]; *out++ = idx[i + 2]; *out++ = idx[i + 3];
            }
            break;
        }
    }

    void Draw(const xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vsCode, uint64_t psHash,
              const xenos::ShaderCode* psCode)
    {
        ++profDraw;
        Section(-1);
        // Same acceptance as xenos_raster.cpp RasterDraw.
        static const uint32_t limit = [] { const char* e = Hc09Env("HC09_GPU_DRAW_LIMIT"); return e ? uint32_t(atoi(e)) : ~0u; }();
        if (++drawCalls > limit) { Skip("HC09_GPU_DRAW_LIMIT"); return; }
        uint32_t mode = Reg(reg::RB_MODECONTROL) & 7;
        if (mode != 4 && mode != 5) { Skip("edram mode not 4/5"); return; }
        if (!vsCode || d.primType == 1 || d.sourceSelect == 1) { Skip("no VS / points / immediate indices"); return; }
        bool rect = d.primType == 8;
        if (d.primType != 4 && d.primType != 5 && d.primType != 6 && d.primType != 13 && !rect)
        { Warn("primitive type " + std::to_string(d.primType) + " not drawn"); Skip("primitive type"); return; }

        // Render state (as RasterDraw).
        uint32_t si = Reg(reg::RB_SURFACE_INFO), pitch = si & 0x3FFF, msaa = (si >> 16) & 3;
        uint32_t colorMask = Reg(reg::RB_COLOR_MASK);
        uint32_t dc = Reg(reg::RB_DEPTHCONTROL);
        bool zEnable = (dc >> 1) & 1, zWrite = (dc >> 2) & 1, stencilOn = dc & 1;
        uint32_t cc = Reg(reg::RB_COLORCONTROL);
        bool alphaTest = (cc >> 3) & 1;
        bool colorOn = false;
        uint32_t rtFmt[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            uint32_t fmt = Bits(Reg(kColorInfoRegs[i]), 16, 4);
            bool on = mode == 4 && ((colorMask >> (4 * i)) & 0xF) && psCode;
            rtFmt[i] = fmt | (on ? 0x100u : 0u);
            colorOn |= on;
        }
        bool shade = colorOn || alphaTest;
        if (!colorOn && !(zEnable && zWrite) && !stencilOn) { Skip("writes nothing"); return; }
        if (!d.numIndices) { Skip("empty"); return; }

        ShaderEntry* vs = Shader(vsHash, *vsCode);
        ShaderEntry* ps = psCode ? Shader(psHash, *psCode) : nullptr;
        if (!vs || (psCode && !ps)) { Skip("shader translation failed"); return; }
        Section(0);

        // Triangle-list indices (as the software rasterizer: offset, clamp,
        // assembly of fans/strips/quads), written into the index ring once the
        // textures are decoded (a decode may wait for the GPU, which recycles the ring).
        uint32_t n = d.numIndices, triIndices = 0;
        switch (d.primType)
        {
        case 4: case 8: triIndices = n / 3 * 3; break;
        case 5: case 6: triIndices = n >= 3 ? 3 * (n - 2) : 0; break;
        case 13: triIndices = n / 4 * 6; break;
        }
        if (!triIndices) { Skip("no triangles"); return; }
        Section(1);
        VkDeviceSize ibytes = VkDeviceSize(triIndices) * 4;
        // Textures first: decoding one may wait for the GPU, which recycles the constant ring.
        const Texture* texOf[32];
        xenos::TextureFetch tfOf[32];
        uint8_t filtOf[32];
        uint32_t slots = vs->meta.textureSlots | (ps ? ps->meta.textureSlots : 0);
        for (uint32_t s = 0; s < 32; ++s)
        {
            texOf[s] = &dummy[kTex2D];
            filtOf[s] = 0xFF;
            if (!(slots & (1u << s))) continue;
            tfOf[s] = xenos::ReadTextureFetch(s);
            texOf[s] = GetTexture(tfOf[s]);
            filtOf[s] = ps && ps->meta.slotFilter[s] != 0xFF ? ps->meta.slotFilter[s] : vs->meta.slotFilter[s];
        }
        Section(2);
        if (constUsed >= kMaxDraws || indexUsed + ibytes > kIndexRingBytes || descSets.size() >= kMaxSets) SubmitAndWait();
        Section(7);
        WriteTriangleIndices(d, reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(indices.mapped) + indexUsed));
        Section(1);

        // Per-draw constants.
        DrawConsts& C = *reinterpret_cast<DrawConsts*>(static_cast<uint8_t*>(consts.mapped) + constUsed * constStride);
        // ALU constants: only the VS and PS windows (the shaders read c[base + i]
        // for i < size and 0 elsewhere — cnst() / ShaderInterpreter::Constant),
        // straight from the register file into the ring (as ShaderConstants::Snapshot).
        uint32_t vsw = Reg(0x2307), psw = Reg(0x2308);
        C.vsBase = vsw & 0x1FF; C.vsSize = ((vsw >> 12) & 0x1FF) + 1;
        C.psBase = psw & 0x1FF; C.psSize = ((psw >> 12) & 0x1FF) + 1;
        for (auto [base, size] : { std::pair<uint32_t, uint32_t>{ C.vsBase, C.vsSize }, { C.psBase, C.psSize } })
            if (base < 512)
                xenos::RegRange(reg::ALU_CONSTANTS + base * 4, std::min(size, 512 - base) * 4, reinterpret_cast<uint32_t*>(C.c[base]));
        xenos::RegRange(reg::BOOL_CONSTANTS, 8, C.bools);
        xenos::RegRange(reg::LOOP_CONSTANTS, 32, C.loops);
        xenos::RegRange(reg::FETCH_CONSTANTS, 192, C.fetch);
        uint32_t vte = Reg(reg::PA_CL_VTE_CNTL), su = Reg(reg::PA_SU_SC_MODE_CNTL);
        float half = (Reg(reg::PA_SU_VTX_CNTL) & 1) == 0 ? 0.5f : 0.0f;
        int32_t wox = 0, woy = 0;
        if (su & (1u << 16)) { uint32_t wo = Reg(reg::PA_SC_WINDOW_OFFSET); wox = SBits(wo, 0, 15); woy = SBits(wo, 16, 15); }
        C.vpScale[0] = (vte & 1) ? AsF(Reg(reg::PA_CL_VPORT_XSCALE)) : 1.0f;
        C.vpScale[1] = (vte & 4) ? AsF(Reg(reg::PA_CL_VPORT_YSCALE)) : 1.0f;
        C.vpScale[2] = (vte & 16) ? AsF(Reg(reg::PA_CL_VPORT_ZSCALE)) : 1.0f;
        C.vpScale[3] = 0.0f;
        C.vpOffset[0] = (vte & 2) ? AsF(Reg(reg::PA_CL_VPORT_XOFFSET)) : 0.0f;
        C.vpOffset[1] = (vte & 8) ? AsF(Reg(reg::PA_CL_VPORT_YOFFSET)) : 0.0f;
        C.vpOffset[2] = (vte & 32) ? AsF(Reg(reg::PA_CL_VPORT_ZOFFSET)) : 0.0f;
        C.vpOffset[3] = 0.0f;
        C.winOfs[0] = float(wox); C.winOfs[1] = float(woy); C.winOfs[2] = half; C.winOfs[3] = 0.0f;
        C.rtSize[0] = C.rtSize[1] = kClipScale; C.rtSize[2] = C.rtSize[3] = 1.0f / kClipScale;
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
            C.rtBase[i] = ci & 0xFFF;
            C.rtFmt[i] = rtFmt[i];
            C.rtBlend[i] = Reg(kBlendRegs[i]);
            C.rtMask[i] = (colorMask >> (4 * i)) & 0xF;
            C.blendConst[i] = AsF(Reg(reg::RB_BLEND_RED + i));
        }
        uint32_t di = Reg(reg::RB_DEPTH_INFO);
        C.surf[0] = pitch; C.surf[1] = msaa; C.surf[2] = di & 0xFFF; C.surf[3] = (di >> 16) & 1;
        C.ds[0] = dc;
        C.ds[1] = Reg(reg::RB_STENCILREFMASK);
        C.ds[2] = Reg(reg::RB_STENCILREFMASK_BF);
        C.ds[3] = (shade ? kDsShade : 0) | (colorOn ? kDsColorOn : 0);
        uint32_t clipCntl = Reg(reg::PA_CL_CLIP_CNTL);
        bool xyFmt = (vte >> 8) & 1, zFmt = (vte >> 9) & 1, w0Fmt = (vte >> 10) & 1;
        bool clipOn = !((clipCntl >> 16) & 1) && !xyFmt && !zFmt && w0Fmt;
        C.clip[0] = (clipOn ? 1u : 0u) | (((clipCntl >> 19) & 1) << 1);
        C.clip[1] = C.clip[2] = C.clip[3] = 0;
        Section(3);

        // Textures: each slot gets a texture of its kind and dummies of the other
        // two; unused slots dummies only (the template).
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
                slotSmp[s][k] = u == &dummy[k] ? dummyImages[k][0].sampler
                                               : Sampler(tfOf[s], filt == 0xFF ? 0x3F : filt, u->img.mipLevels, k);
                descKey.insert(descKey.end(), { uint64_t(s), uint64_t(u->img.view), uint64_t(slotSmp[s][k]) });
            }
        }

        // Descriptor set: reused by draws binding the same views and samplers
        // until the pool is reset (views are not destroyed before that).
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (auto dit = descSets.find(descKey); dit != descSets.end()) set = dit->second;
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
        VkDescriptorBufferInfo bMem{ guest.buffer, 0, VK_WHOLE_SIZE }, bConst{ consts.buffer, 0, sizeof(DrawConsts) },
            bEdram{ edram.buffer, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[9]{};
        for (auto& x : w) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = set; x.descriptorCount = 1; }
        w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bMem;
        w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; w[1].pBufferInfo = &bConst;
        w[2].dstBinding = 2; w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].descriptorCount = 32; w[2].pImageInfo = images[kTex2D];
        w[3].dstBinding = 3; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &bEdram;
        w[4].dstBinding = 4; w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[4].descriptorCount = 32; w[4].pImageInfo = images[kTex3D];
        w[5].dstBinding = 5; w[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[5].descriptorCount = 32; w[5].pImageInfo = images[kTexCube];
        for (uint32_t k = 0; k < kTexKinds; ++k)
        {
            w[6 + k].dstBinding = 6 + k;
            w[6 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[6 + k].descriptorCount = 32;
            w[6 + k].pImageInfo = uimages[k];
        }
        vkUpdateDescriptorSets(dev->device, 9, w, 0, nullptr);
        descSets.emplace(descKey, set);
        }
        Section(4);

        // Pipeline: culling as the software (rectangle lists are not culled);
        // front = (window-space clockwise) == face_cw.
        bool cullFront = (su & 1) && !rect, cullBack = ((su >> 1) & 1) && !rect;
        uint32_t cull = (cullFront ? VK_CULL_MODE_FRONT_BIT : 0) | (cullBack ? VK_CULL_MODE_BACK_BIT : 0);
        uint32_t frontFace = ((su >> 2) & 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        VkPipeline pipe = Pipeline(vsHash, ps ? psHash : 0, rect, cull, frontFace, texKinds, vs->module, ps ? ps->module : nullPs);
        if (!pipe) { Skip("pipeline creation failed"); return; }
        Section(5);

        // Scissor as the software: window scissor (+ window offset), clamped to the surface pitch.
        uint32_t tl = Reg(reg::PA_SC_WINDOW_SCISSOR_TL), br = Reg(reg::PA_SC_WINDOW_SCISSOR_BR);
        int32_t scx0 = Bits(tl, 0, 14), scy0 = Bits(tl, 16, 14), scx1 = Bits(br, 0, 14), scy1 = Bits(br, 16, 14);
        if (!(tl >> 31)) { scx0 += wox; scx1 += wox; scy0 += woy; scy1 += woy; }
        scx0 = std::max(0, scx0); scy0 = std::max(0, scy0);
        scx1 = std::min<int32_t>(scx1, int32_t(pitch ? pitch : 8192)); scy1 = std::min(scy1, 8192);
        scx1 = std::min<int32_t>(scx1, int32_t(kRenderArea)); scy1 = std::min<int32_t>(scy1, int32_t(kRenderArea));
        if (scx1 <= scx0 || scy1 <= scy0) { Skip("empty scissor"); return; }

        // Record. Interlock orders fragments at the same pixel coordinate only;
        // surfaces with another pitch, MSAA mode or base put the same EDRAM dword
        // at other coordinates (e.g. team select's 640x4x surface over the 1280x1x
        // one at base 0), so a change of layout needs a full memory barrier.
        std::array<uint32_t, 7> layout = { pitch, msaa, ~0u, ~0u, ~0u, ~0u, (zEnable || stencilOn) ? (di & 0xFFF) : ~0u };
        for (int i = 0; i < 4; ++i) if (rtFmt[i] & 0x100) layout[2 + i] = C.rtBase[i] | (rtFmt[i] << 12);
        Begin();
        if (rendering && layout != lastLayout)
        {
            EndRendering();
            MemoryBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            ++stats->barriers;
        }
        lastLayout = layout;
        if (!rendering)
        {
            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { 0, 0 }, { kRenderArea, kRenderArea } };
            ri.layerCount = 1;
            vkCmdBeginRendering(cb, &ri);
            rendering = true;
        }
        if (pipe != boundPipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); boundPipe = pipe; }
        if (!graphicsStatic)
        {
            VkViewport vp{ -kClipScale, -kClipScale, 2.0f * kClipScale, 2.0f * kClipScale, 0.0f, 1.0f };
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdBindIndexBuffer(cb, indices.buffer, 0, VK_INDEX_TYPE_UINT32);   // draws address it by firstIndex
            graphicsStatic = true;
        }
        uint32_t dynOff = uint32_t(constUsed * constStride);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0, 1, &set, 1, &dynOff);
        VkRect2D scissor{ { scx0, scy0 }, { uint32_t(scx1 - scx0), uint32_t(scy1 - scy0) } };
        if (memcmp(&scissor, &boundScissor, sizeof(scissor))) { vkCmdSetScissor(cb, 0, 1, &scissor); boundScissor = scissor; }
        vkCmdDrawIndexed(cb, triIndices, 1, uint32_t(indexUsed / 4), 0, 0);
        ProfMark(ps ? psHash : 0, triIndices / 3);
        indexUsed += (ibytes + 255) & ~VkDeviceSize(255);
        ++constUsed;
        ++stats->draws;
        if (chunkDraws && ++drawsInChunk >= chunkDraws && chunk + 1 < kMaxChunks) SubmitChunk();
        Section(6);
    }

    // ---- resolves (xenos_raster.cpp Resolve on the EDRAM buffer) ----------
    void Resolve(const xenos::ResolveInfo& r)
    {
        EndRendering();
        Begin();
        MemoryBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipeLayout, 0, 1, &resolveSet, 0, nullptr);
        ResolveConsts p{};
        p.x0 = r.x0; p.y0 = r.y0; p.w = uint32_t(r.x1 - r.x0); p.h = uint32_t(r.y1 - r.y0);
        p.base = r.base; p.pitch = r.pitch; p.msaa = r.msaa; p.wide = r.wide;
        p.src = r.src; p.srcFmt = r.srcFmt; p.sampleSel = r.sampleSel;
        p.destBase = r.destBase; p.destPitch = r.destPitch; p.destFmt = r.destFmt; p.destBytes = r.destBytes;
        p.endian = r.endian; p.swapRB = r.swapRB; p.depthBase = r.depthBase; p.colorClear = r.colorClear;
        p.colorClearLo = r.colorClearLo; p.depthClear = r.depthClear; p.destScale = r.destScale;
        uint32_t gx = (p.w + 7) / 8, gy = (p.h + 7) / 8;
        auto pass = [&](uint32_t which, bool barrierBefore) {
            if (barrierBefore) MemoryBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            p.pass = which;
            vkCmdPushConstants(cb, resolvePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
            vkCmdDispatch(cb, gx, gy, 1);
        };
        bool any = false;
        if (r.copy) { pass(0, false); any = true; }
        if (r.clearColor && r.src < 4) { pass(1, any); any = true; }
        if (r.clearDepth) { pass(2, any); any = true; }
        MemoryBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ++stats->resolves;
        ProfMark(~0ull, 0);
        // The CPU reads what a copy wrote (texture decoding, the capture's resolve PNG).
        if (r.copy)
        {
            // Destination span: whole 32x32 tiles of the destination pitch up to the rectangle's bottom.
            uint32_t rows = (uint32_t(r.y1) + 31) & ~31u, span = r.destPitch * rows * r.destBytes;
            InvalidateTextures(r.destBase, span);
            if (waitAtCopyResolves) SubmitAndWait();
            else pendingWrites.emplace_back(r.destBase, span);
        }
    }
};

bool EdramRenderer::Init(Device& dev, const Buffer& guestMemory, std::string* err, bool exact)
{
    if (!dev.pixelInterlock) { if (err) *err = "the device lacks fragment shader pixel interlock / fragment stores / clip distances / fp64"; return false; }
    impl_ = std::make_unique<Impl>();
    Impl& I = *impl_;
    I.dev = &dev;
    I.guest = guestMemory;
    I.stats = &stats_;
    // Exact: shaders compute what the software rasterizer computes. Fast: the GPU's
    // own math and samplers. HC09_GPU_HWSAMPLER / HC09_GPU_FASTMATH force either part.
    auto flag = [](const char* e, bool dflt) { return e && *e ? strcmp(e, "0") != 0 : dflt; };
    I.translate.hwSampler = flag(Hc09Env("HC09_GPU_HWSAMPLER"), !exact);
    I.translate.fastMath = flag(Hc09Env("HC09_GPU_FASTMATH"), !exact);
    VkDevice d = dev.device;

    VkDescriptorSetLayoutBinding b[9]{};
    b[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1,
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[2] = { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[4] = { 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    b[5] = { 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    for (uint32_t k = 0; k < 3; ++k)
        b[6 + k] = { 6 + k, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 9;
    lci.pBindings = b;
    VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pci.setLayoutCount = 1;
    pci.pSetLayouts = &I.setLayout;
    VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxSets * 2 },
                                      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, kMaxSets },
                                      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets * 32 * kTexKinds * 2 } };
    VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = kMaxSets;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorSetLayout(d, &lci, nullptr, &I.setLayout) != VK_SUCCESS ||
        vkCreatePipelineLayout(d, &pci, nullptr, &I.pipeLayout) != VK_SUCCESS ||
        vkCreateDescriptorPool(d, &dpi, nullptr, &I.pool) != VK_SUCCESS)
    { if (err) *err = "descriptor/pipeline layout"; return false; }

    // Resolve compute: guest memory + EDRAM, parameters as push constants.
    VkDescriptorSetLayoutBinding rb[2]{};
    rb[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    rb[1] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lci.bindingCount = 2;
    lci.pBindings = rb;
    VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolveConsts) };
    VkPipelineLayoutCreateInfo rpci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    rpci.setLayoutCount = 1;
    rpci.pSetLayouts = &I.resolveSetLayout;
    rpci.pushConstantRangeCount = 1;
    rpci.pPushConstantRanges = &pcr;
    VkDescriptorPoolSize rsize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo rdpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    rdpi.maxSets = 1;
    rdpi.poolSizeCount = 1;
    rdpi.pPoolSizes = &rsize;
    if (vkCreateDescriptorSetLayout(d, &lci, nullptr, &I.resolveSetLayout) != VK_SUCCESS ||
        vkCreatePipelineLayout(d, &rpci, nullptr, &I.resolvePipeLayout) != VK_SUCCESS ||
        vkCreateDescriptorPool(d, &rdpi, nullptr, &I.resolvePool) != VK_SUCCESS)
    { if (err) *err = "resolve layout"; return false; }

    VkDeviceSize align = std::max<VkDeviceSize>(16, dev.props.limits.minStorageBufferOffsetAlignment);
    I.constStride = (sizeof(DrawConsts) + align - 1) / align * align;
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!dev.CreateBuffer(I.constStride * kMaxDraws, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, I.consts) ||
        !dev.CreateBuffer(kIndexRingBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hv, I.indices) ||
        !dev.CreateBuffer(kEdramBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, I.edram))
    { if (err) *err = "constant/index/EDRAM buffers"; return false; }

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = I.resolvePool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &I.resolveSetLayout;
    if (vkAllocateDescriptorSets(d, &ai, &I.resolveSet) != VK_SUCCESS) { if (err) *err = "resolve descriptor set"; return false; }
    VkDescriptorBufferInfo bMem{ I.guest.buffer, 0, VK_WHOLE_SIZE }, bEdram{ I.edram.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2]{};
    for (auto& x : w) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.dstSet = I.resolveSet; x.descriptorCount = 1; x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
    w[0].dstBinding = 0; w[0].pBufferInfo = &bMem;
    w[1].dstBinding = 3; w[1].pBufferInfo = &bEdram;
    vkUpdateDescriptorSets(d, 2, w, 0, nullptr);

    // Pipeline cache on disk (HC09_PIPELINE_CACHE, default <cache folder>/vk_pipelines.bin):
    // the translated pixel shaders are large, the driver takes seconds per pipeline.
    {
        const char* e = Hc09Env("HC09_PIPELINE_CACHE");
        const char* c = Hc09Env("HC09_CACHE_DIR");   // hc09.exe sets it to its cache folder
        I.pipelineCachePath = e && *e ? e : std::string(c && *c ? c : "hc09-cache") + "/vk_pipelines.bin";
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
            pcci.initialDataSize = 0;   // stale or foreign data: start empty
            pcci.pInitialData = nullptr;
            vkCreatePipelineCache(d, &pcci, nullptr, &I.pipelineCache);
        }
    }
    I.rectGs = I.Module(RectListGeometryShader(), Stage::kGeometry, "rectlist_gs_edram");
    I.nullPs = I.Module(EdramNullPixelShader(I.translate.fastMath), Stage::kFragment, "ps_null_edram");
    VkShaderModule cs = I.Module(EdramResolveShader(), Stage::kCompute, "resolve_cs");
    if (!I.rectGs || !I.nullPs || !cs) { if (err) *err = "built-in shaders failed to compile"; return false; }
    VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
    cpi.layout = I.resolvePipeLayout;
    VkResult cr = vkCreateComputePipelines(d, I.pipelineCache, 1, &cpi, nullptr, &I.resolvePipe);
    vkDestroyShaderModule(d, cs, nullptr);
    if (cr != VK_SUCCESS) { if (err) *err = "resolve pipeline"; return false; }
    {
        VkDescriptorSetLayoutBinding db[2]{};
        db[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        db[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dl.bindingCount = 2;
        dl.pBindings = db;
        VkPushConstantRange dpr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 44 };
        VkPipelineLayoutCreateInfo dpl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        dpl.setLayoutCount = 1;
        dpl.pSetLayouts = &I.decodeSetLayout;
        dpl.pushConstantRangeCount = 1;
        dpl.pPushConstantRanges = &dpr;
        VkDescriptorPoolSize dps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Impl::kDecodeSets },
                                        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, Impl::kDecodeSets * 2 } };
        VkDescriptorPoolCreateInfo dpi2{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi2.maxSets = Impl::kDecodeSets;
        dpi2.poolSizeCount = 2;
        dpi2.pPoolSizes = dps;
        VkShaderModule dcs = I.Module(TextureDecodeShader(), Stage::kCompute, "texdecode_cs");
        if (!dcs || vkCreateDescriptorSetLayout(d, &dl, nullptr, &I.decodeSetLayout) != VK_SUCCESS ||
            vkCreatePipelineLayout(d, &dpl, nullptr, &I.decodePipeLayout) != VK_SUCCESS ||
            vkCreateDescriptorPool(d, &dpi2, nullptr, &I.decodePool) != VK_SUCCESS)
        { if (err) *err = "texture decode pipeline"; return false; }
        VkComputePipelineCreateInfo dci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        dci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, dcs, "main" };
        dci.layout = I.decodePipeLayout;
        VkResult dr = vkCreateComputePipelines(d, I.pipelineCache, 1, &dci, nullptr, &I.decodePipe);
        vkDestroyShaderModule(d, dcs, nullptr);
        if (dr != VK_SUCCESS) { if (err) *err = "texture decode pipeline"; return false; }
        {
            // The depth variant (same layout): needs R32F as a storage image and with linear filtering.
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_R32_SFLOAT, &fp);
            VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            VkShaderModule zcs = (fp.optimalTilingFeatures & need) == need
                                     ? I.Module(TextureDecodeShader(true), Stage::kCompute, "texdecode_depth_cs") : VK_NULL_HANDLE;
            if (zcs)
            {
                dci.stage.module = zcs;
                if (vkCreateComputePipelines(d, I.pipelineCache, 1, &dci, nullptr, &I.depthDecodePipe) != VK_SUCCESS)
                    I.depthDecodePipe = VK_NULL_HANDLE;
                vkDestroyShaderModule(d, zcs, nullptr);
            }
            if (!I.depthDecodePipe) fprintf(stderr, "[gpu] depth textures: high byte only (R32F storage/filtering unavailable)\n");
        }
        // Mip levels: two storage images.
        VkDescriptorSetLayoutBinding mbd[2]{};
        mbd[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        mbd[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        dl.pBindings = mbd;
        VkPushConstantRange mpr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
        dpl.pSetLayouts = &I.mipSetLayout;
        dpl.pPushConstantRanges = &mpr;
        VkShaderModule mcs = I.Module(TextureMipShader(), Stage::kCompute, "texmip_cs");
        if (!mcs || vkCreateDescriptorSetLayout(d, &dl, nullptr, &I.mipSetLayout) != VK_SUCCESS ||
            vkCreatePipelineLayout(d, &dpl, nullptr, &I.mipPipeLayout) != VK_SUCCESS)
        { if (err) *err = "texture mip pipeline"; return false; }
        dci.stage.module = mcs;
        dci.layout = I.mipPipeLayout;
        VkResult mr = vkCreateComputePipelines(d, I.pipelineCache, 1, &dci, nullptr, &I.mipPipe);
        vkDestroyShaderModule(d, mcs, nullptr);
        if (mr != VK_SUCCESS) { if (err) *err = "texture mip pipeline"; return false; }
    }

    if (const char* cp = Hc09Env("HC09_GPU_CPUPROF"); cp && *cp && strcmp(cp, "0")) I.cpuProf = true;
    if (const char* pe = Hc09Env("HC09_GPU_PROFILE"); pe && *pe && strcmp(pe, "0") && dev.props.limits.timestampComputeAndGraphics)
    {
        VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = Impl::kProfQueries;
        vkCreateQueryPool(d, &qci, nullptr, &I.profPool);
    }
    if (dev.props.limits.timestampComputeAndGraphics)
    {
        VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2 * Impl::kMaxChunks;
        vkCreateQueryPool(d, &qci, nullptr, &I.timestamps);
    }
    if (const char* ce = Hc09Env("HC09_GPU_CHUNK")) I.chunkDraws = uint32_t(atoi(ce));
    if (I.profPool) I.chunkDraws = 0;   // per-draw timestamps assume one command buffer per frame
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = dev.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = Impl::kMaxChunks;
    vkAllocateCommandBuffers(d, &cai, I.cbs);
    vkAllocateCommandBuffers(d, &cai, I.uploads);
    I.cb = I.cbs[0];
    I.upload = I.uploads[0];
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(d, &fi, nullptr, &I.fence);

    // EDRAM starts zeroed (xenos::RasterBeginFrame).
    I.Begin();
    vkCmdFillBuffer(I.cb, I.edram.buffer, 0, VK_WHOLE_SIZE, 0);
    MemoryBarrier(I.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    VkSamplerCreateInfo nci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };   // all nearest / repeat
    vkCreateSampler(d, &nci, nullptr, &I.nearest);
    if (I.translate.hwSampler || I.translate.fastMath)
        fprintf(stderr, "[gpu] %s%s%s (not bit-identical to the software reference)\n", I.translate.hwSampler ? "hardware texture filtering" : "",
                I.translate.hwSampler && I.translate.fastMath ? ", " : "", I.translate.fastMath ? "GPU fp32 math" : "");
    // 1x1 transparent black of each kind for unused or undecodable texture slots.
    std::vector<std::vector<xenos::Image>> one(1, std::vector<xenos::Image>(1));
    one[0][0].width = one[0][0].height = 1;
    one[0][0].rgba.assign(1, 0u);
    for (uint32_t k = 0; k < kTexKinds; ++k)
        if (!I.Upload(I.dummy[k], k, one)) { if (err) *err = "dummy texture"; return false; }
    return true;
}

void EdramRenderer::Flush()
{
    if (impl_) impl_->SubmitAndWait();
}

void EdramRenderer::SetResolveWaits(bool wait)
{
    if (impl_) impl_->waitAtCopyResolves = wait;
}

void EdramRenderer::OnFrameEnd(uint32_t, uint32_t, uint32_t)
{
    EndFrame();
}

void EdramRenderer::EndFrame()
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (!impl_) return;
    Impl& I = *impl_;
    I.SubmitAndWait();
    // Evict textures unused for 10 s of frames.
    if (++I.frame % 60 == 0)
        for (auto it = I.textures.begin(); it != I.textures.end();)
        {
            if (it->second.lastUsed + 600 < I.frame)
            {
                I.garbage.push_back(it->second.img);
                it = I.textures.erase(it);
            }
            else ++it;
        }
}

const Image* EdramRenderer::DecodeFrontBuffer(uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (!impl_ || !impl_->FrontBuffer(frontBuffer, width, height)) return nullptr;
    return &impl_->front;
}

void EdramRenderer::OnDraw(const xenos::DrawPacket& d, uint64_t vsHash, const xenos::ShaderCode* vs, uint64_t psHash,
                           const xenos::ShaderCode* ps)
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (impl_) impl_->Draw(d, vsHash, vs, psHash, ps);
}

void EdramRenderer::OnResolve(const xenos::ResolveInfo& r)
{
    ScopeTimer t{ stats_.rendererSeconds };
    if (impl_) impl_->Resolve(r);
}

void EdramRenderer::OnResolved() {}

void EdramRenderer::Shutdown()
{
    if (!impl_) return;
    Impl& I = *impl_;
    VkDevice d = I.dev->device;
    vkDeviceWaitIdle(d);
    if (I.profPool)
    {
        std::vector<std::pair<double, uint64_t>> byPs;
        double total = 0;
        for (auto& [ps, e] : I.profByPs) { byPs.push_back({ e.first, ps }); total += e.first; }
        std::sort(byPs.rbegin(), byPs.rend());
        fprintf(stderr, "[gpu-profile] %.2f ms of GPU time in draws/resolves; by pixel shader:\n", total * 1e3);
        for (size_t i = 0; i < byPs.size() && i < 15; ++i)
            fprintf(stderr, "[gpu-profile]   %8.3f ms  %5u draws  ps %016llx%s\n", byPs[i].first * 1e3, I.profByPs[byPs[i].second].second,
                    (unsigned long long)byPs[i].second, byPs[i].second == ~0ull ? " (resolves)" : byPs[i].second ? "" : " (no pixel shader)");
        std::sort(I.profTop.begin(), I.profTop.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < I.profTop.size() && i < 15; ++i)
            fprintf(stderr, "[gpu-profile]   draw call %u: %.3f ms, %u triangles, ps %016llx\n", I.profTop[i].second.draw,
                    I.profTop[i].first * 1e3, I.profTop[i].second.tris, (unsigned long long)I.profTop[i].second.ps);
        vkDestroyQueryPool(d, I.profPool, nullptr);
    }
    for (Buffer& b : I.staging) I.dev->DestroyBuffer(b);
    for (auto& [k, t] : I.textures) I.dev->DestroyImage(t.img);
    for (Image& img : I.garbage) I.dev->DestroyImage(img);
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
    if (I.resolvePipe) vkDestroyPipeline(d, I.resolvePipe, nullptr);
    if (I.decodePipe) vkDestroyPipeline(d, I.decodePipe, nullptr);
    if (I.depthDecodePipe) vkDestroyPipeline(d, I.depthDecodePipe, nullptr);
    if (I.mipPipe) vkDestroyPipeline(d, I.mipPipe, nullptr);
    if (I.mipPipeLayout) vkDestroyPipelineLayout(d, I.mipPipeLayout, nullptr);
    if (I.mipSetLayout) vkDestroyDescriptorSetLayout(d, I.mipSetLayout, nullptr);
    if (I.decodePool) vkDestroyDescriptorPool(d, I.decodePool, nullptr);
    if (I.decodePipeLayout) vkDestroyPipelineLayout(d, I.decodePipeLayout, nullptr);
    if (I.decodeSetLayout) vkDestroyDescriptorSetLayout(d, I.decodeSetLayout, nullptr);
    I.dev->DestroyBuffer(I.consts);
    I.dev->DestroyBuffer(I.indices);
    I.dev->DestroyBuffer(I.edram);
    if (I.fence) vkDestroyFence(d, I.fence, nullptr);
    if (I.timestamps) vkDestroyQueryPool(d, I.timestamps, nullptr);
    vkFreeCommandBuffers(d, I.dev->pool, Impl::kMaxChunks, I.cbs);
    vkFreeCommandBuffers(d, I.dev->pool, Impl::kMaxChunks, I.uploads);
    vkDestroyDescriptorPool(d, I.pool, nullptr);
    vkDestroyDescriptorPool(d, I.resolvePool, nullptr);
    vkDestroyPipelineLayout(d, I.pipeLayout, nullptr);
    vkDestroyPipelineLayout(d, I.resolvePipeLayout, nullptr);
    vkDestroyDescriptorSetLayout(d, I.setLayout, nullptr);
    vkDestroyDescriptorSetLayout(d, I.resolveSetLayout, nullptr);
    impl_.reset();
}

EdramRenderer::EdramRenderer() = default;
EdramRenderer::~EdramRenderer() { Shutdown(); }

}  // namespace hc09vk
