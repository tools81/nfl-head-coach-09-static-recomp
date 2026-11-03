// Shader store and frame capture.
//
// HC09_CAPTURE_FRAME=<n>: record the frame that starts after the n-th XE_SWAP
// (n = GPU swap count). Writes <HC09_CAPTURE_DIR, default "captures">/frame_<n>/:
//   draws.txt        every draw/resolve with its render state, shaders,
//                    vertex and texture fetch constants
//   shaders/{vs,ps}_<hash>.bin   microcode as loaded (big-endian dwords)
//
// HC09_RENDER_RANGE=<a>-<b>: also rasterize every frame after swap a and before
// swap b, quietly (no output). The software renderer otherwise only runs on
// captured frames, so anything the GPU renders once and keeps in memory (e.g.
// textures baked while a level loads) would never exist for later captures.
//
// HC09_RENDER_AFTER_LOAD=1: render the first heavy frame after a light one
// (e.g. the first scene frame after a loading screen), where games render
// textures once and keep them (shadow/light maps, composited field markings).
// Draws of a frame following a light frame (< 200 draws) are buffered as
// register snapshots; once the frame passes 1,000 draws the buffer is rendered
// and the rest of the frame is rendered live. Light frames cost a copy.
//
// HC09_WATCH_VFETCH=<vs hash>:<vertex fetch slot>[,...]: when that vertex
// shader draws, ask the host to report CPU writes to the vertex buffer bound to
// that fetch slot (who fills a stream the GPU reads).
//
// HC09_TRACE_RESOLVES=1: log the first resolve to each destination address
// (swap number, size, format) — finds the frames that produce a texture.
//
//   replay.bin       everything needed to render the frame again offline with
//                    hc09-replay (tools/xenos_replay.cpp): register deltas per
//                    event, shader loads, draw packets, and every 4 KiB page of
//                    GPU memory the renderer touched, as it was at first touch.
//                    Game-derived data: lives under captures/ (never committed).
//
// replay.bin format (host little-endian): "XRPL", u32 version (1), u64 swap
// number, then records tagged by one byte:
//   'P' u32 page address, 4096 bytes        'R' u32 n, n x (u32 reg, u32 value)
//   'S' u32 type, u32 guest address, u32 dwords, dwords x 4 bytes (as in memory)
//   'D' u32 x 8: primType sourceSelect indexSize32 numIndices dmaBase dmaWords dmaEndian packetAddr
//   'F' u32 front buffer, u32 width, u32 height        'E' end
// Pages hold first-touch contents, so a replayer loads all of them before
// executing the other records in order.
#include "../dev_env.h"
#include "xenos.h"
#include "xenos_raster.h"
#include "xenos_shader.h"
#include "xenos_texture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

namespace xenos {
namespace {

struct Shader
{
    ShaderType type;
    uint64_t hash;
    std::vector<uint8_t> ucode;   // big-endian, as in memory
    uint32_t guestAddress;
    ShaderCode code;              // host-order dwords for the interpreter
};

std::mutex g_mu;
std::map<uint64_t, Shader> g_shaders;
uint64_t g_activeVs = 0, g_activePs = 0;
DrawObserver* g_observer = nullptr;   // second renderer (SetDrawObserver)
DrawObserver* g_live = nullptr;       // live renderer (SetLiveObserver): every draw of the game
bool g_softwareRaster = true;

uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

float RegF(uint32_t index) { uint32_t v = HostReg(index); float f; memcpy(&f, &v, 4); return f; }

// ---- replay recording -------------------------------------------------------
struct Recorder
{
    FILE* f = nullptr;
    std::vector<uint32_t> shadow;   // register values already in the file
    std::vector<bool> pageSeen;     // per 4 KiB page of the 512 MiB physical space

    void U32(uint32_t v) { fwrite(&v, 4, 1, f); }
    bool Open(const std::string& path, uint64_t swap)
    {
        f = fopen(path.c_str(), "wb");
        if (!f) return false;
        shadow.assign(kRegisterCount, 0);
        pageSeen.assign(0x20000, false);
        fwrite("XRPL", 1, 4, f);
        U32(1);
        fwrite(&swap, 8, 1, f);
        return true;
    }
    void Page(uint32_t pa)
    {
        uint32_t p = (pa & 0x1FFFFFFF) >> 12;
        if (pageSeen[p]) return;
        pageSeen[p] = true;
        fputc('P', f);
        U32(p << 12);
        fwrite(HostPhys(p << 12), 1, 4096, f);
    }
    void Range(uint32_t pa, uint32_t bytes)
    {
        pa &= 0x1FFFFFFF;
        bytes = std::min<uint32_t>(bytes, 0x20000000 - pa);
        for (uint32_t a = pa & ~0xFFFu; a < pa + bytes; a += 0x1000) Page(a);
    }
    void Regs()
    {
        std::vector<uint32_t> delta;
        for (uint32_t i = 0; i < kRegisterCount; ++i)
        {
            uint32_t v = HostReg(i);
            if (v != shadow[i]) { shadow[i] = v; delta.push_back(i); delta.push_back(v); }
        }
        if (delta.empty()) return;
        fputc('R', f);
        U32(uint32_t(delta.size() / 2));
        fwrite(delta.data(), 4, delta.size(), f);
    }
    void Shader(ShaderType type, uint32_t guestAddress, const std::vector<uint8_t>& ucode)
    {
        Regs();
        fputc('S', f);
        U32(uint32_t(type)); U32(guestAddress); U32(uint32_t(ucode.size() / 4));
        fwrite(ucode.data(), 1, ucode.size(), f);
    }
    // Pages a draw may read beyond what this renderer touches today (whole
    // vertex/index buffers, full textures and their mip chains), so replays
    // stay valid as the renderer learns more.
    void Prefetch(const DrawPacket& d)
    {
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            uint32_t g[6];
            for (int i = 0; i < 6; ++i) g[i] = HostReg(reg::FETCH_CONSTANTS + slot * 6 + i);
            if ((g[0] & 3) == 2)
            {
                TextureFetch tf;
                memcpy(tf.dword, g, sizeof(g));
                uint32_t bytes = TextureBytes(tf);
                Range(tf.baseAddress(), bytes);
                if (tf.mipAddress()) Range(tf.mipAddress(), bytes);
            }
            else
                for (int v = 0; v < 3; ++v)
                    if ((g[v * 2] & 3) == 3) Range(g[v * 2] & 0xFFFFFFFC, ((g[v * 2 + 1] >> 2) & 0xFFFFFF) * 4);
        }
        if (d.sourceSelect == 0) Range(d.dmaBase, d.dmaWords * (d.indexSize32 ? 4 : 2));
    }
    void Draw(const DrawPacket& d)
    {
        Regs();
        Prefetch(d);
        fputc('D', f);
        const uint32_t v[8] = { d.primType, d.sourceSelect, d.indexSize32, d.numIndices, d.dmaBase, d.dmaWords, d.dmaEndian, d.packetAddr };
        fwrite(v, 4, 8, f);
    }
    void Frame(uint32_t frontBuffer, uint32_t width, uint32_t height)
    {
        Regs();
        uint32_t pitch = (width + 31) & ~31u;
        Range(frontBuffer, pitch * ((height + 31) & ~31u) * 4);
        fputc('F', f);
        U32(frontBuffer); U32(width); U32(height);
    }
    void Close()
    {
        if (!f) return;
        fputc('E', f);
        fclose(f);
        f = nullptr;
    }
} g_rec;
// The thread recording replay.bin (the one that opened it: the command processor
// or the replay tool). Phys() is also called by other threads (the native
// renderer's thread and its texture decodes): their pages are not part of the
// emulated frame, and writing them would interleave records in the file.
thread_local bool t_recOwner = false;

// ---- register override and buffered draws ------------------------------------
thread_local const uint32_t* g_regOverride = nullptr;   // per thread (SetThreadRegisterFile)

struct BufferedDraw
{
    DrawPacket d;
    uint64_t vs, ps;
    std::vector<uint32_t> regs;
};
std::vector<BufferedDraw> g_buffered;
constexpr uint32_t kLightFrameDraws = 200, kHeavyFrameDraws = 1000;

// ---- capture state ----------------------------------------------------------
struct Capture
{
    long target = -2;           // swap number being captured / next to capture (-1 = none left)
    std::vector<long> targets;  // HC09_CAPTURE_FRAME=a,b,c (ascending)
    size_t next = 0;
    bool active = false, done = false;
    bool configured = false, recordReplay = true;
    long renderFrom = -1, renderTo = -1;   // HC09_RENDER_RANGE
    bool traceResolves = false;
    std::vector<std::pair<uint64_t, uint32_t>> watchVfetch;   // (vs hash, fetch slot)
    std::map<uint32_t, bool> watched;
    bool afterLoad = false;          // HC09_RENDER_AFTER_LOAD
    uint32_t frameDraws = 0, prevFrameDraws = ~0u;
    bool buffering = false, renderingLive = false;
    std::map<uint32_t, bool> resolvesSeen;
    std::string baseDir, dir;
    FILE* out = nullptr;
    uint32_t draws = 0, copies = 0, textures = 0;
    std::map<uint64_t, bool> shadersWritten;
    std::map<std::string, bool> texturesWritten;
} g_cap;

// Decode and write every texture bound to the current draw (once per
// address/format/size within the capture).
void DumpBoundTextures(FILE* o)
{
    for (uint32_t slot = 0; slot < 32; ++slot)
    {
        TextureFetch tf = ReadTextureFetch(slot);
        if (!tf.valid()) continue;
        char name[96];
        snprintf(name, sizeof(name), "tex_%08X_%s_%ux%u.png", tf.baseAddress(), TextureFormatName(tf.format()),
                 tf.width(), tf.height());
        if (g_cap.texturesWritten[name]) continue;
        g_cap.texturesWritten[name] = true;
        Image img;
        std::string why;
        if (!DecodeTexture2D(tf, img, &why)) { fprintf(o, "  (tex[%u] not dumped: %s)\n", slot, why.c_str()); continue; }
        // Stacked textures: every layer, one below the other.
        for (uint32_t l = 1; tf.stacked() && l < tf.depth(); ++l)
        {
            Image layer;
            if (!DecodeTextureLayer(tf, l, layer, &why)) break;
            img.rgba.insert(img.rgba.end(), layer.rgba.begin(), layer.rgba.end());
            img.height += layer.height;
        }
        if (WritePng(g_cap.dir + "/" + name, img)) ++g_cap.textures;
    }
}

void InitCapture()
{
    if (g_cap.target != -2) return;
    if (g_cap.configured) {}
    else if (const char* t = Hc09Env("HC09_CAPTURE_FRAME"))
        for (const char* p = t; *p;)
        {
            char* end;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            g_cap.targets.push_back(v);
            p = *end ? end + 1 : end;
        }
    std::sort(g_cap.targets.begin(), g_cap.targets.end());
    g_cap.target = g_cap.targets.empty() ? -1 : g_cap.targets[0];
    g_cap.done = g_cap.targets.empty();
    if (g_cap.configured) return;
    if (const char* r = Hc09Env("HC09_RENDER_RANGE"))
    {
        char* e;
        g_cap.renderFrom = strtol(r, &e, 10);
        g_cap.renderTo = *e ? strtol(e + 1, nullptr, 10) : -1;
        fprintf(stderr, "[xenos] rendering every frame from swap %ld to %ld quietly\n", g_cap.renderFrom, g_cap.renderTo);
    }
    if (const char* t = Hc09Env("HC09_TRACE_RESOLVES")) g_cap.traceResolves = atoi(t) != 0;
    if (const char* w = Hc09Env("HC09_WATCH_VFETCH"))
        for (const char* p = w; *p;)
        {
            char* e;
            uint64_t h = strtoull(p, &e, 16);
            if (*e != ':') break;
            uint32_t slot = uint32_t(strtoul(e + 1, &e, 10));
            g_cap.watchVfetch.push_back({ h, slot });
            fprintf(stderr, "[xenos] watching vertex fetch %u of vs %016llx\n", slot, (unsigned long long)h);
            p = *e ? e + 1 : e;
        }
    if (const char* t = Hc09Env("HC09_RENDER_AFTER_LOAD")) g_cap.afterLoad = atoi(t) != 0;
    const char* d = Hc09Env("HC09_CAPTURE_DIR");
    g_cap.baseDir = d ? d : "captures";
}

void WriteShaderFile(uint64_t hash)
{
    if (!hash || g_cap.shadersWritten[hash]) return;
    auto it = g_shaders.find(hash);
    if (it == g_shaders.end()) return;
    g_cap.shadersWritten[hash] = true;
    char name[64];
    snprintf(name, sizeof(name), "%s_%016llx.bin", it->second.type == ShaderType::kVertex ? "vs" : "ps",
             (unsigned long long)hash);
    std::string path = g_cap.dir + "/shaders/" + name;
    if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(it->second.ucode.data(), 1, it->second.ucode.size(), f); fclose(f); }
    ShaderCode code = ShaderFromBigEndian(it->second.type, it->second.ucode.data(), uint32_t(it->second.ucode.size() / 4));
    if (FILE* f = fopen((path.substr(0, path.size() - 4) + ".txt").c_str(), "w"))
    {
        std::string text = Disassemble(code);
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

const char* kColorFormats[16] = { "8888", "8888_GAMMA", "2_10_10_10", "2_10_10_10_FLOAT", "16_16", "16_16_16_16",
                                  "16_16_FLOAT", "16_16_16_16_FLOAT", "?8", "?9", "2_10_10_10_AS_10x4", "?11",
                                  "2_10_10_10_FLOAT_AS_16x4", "?13", "32_FLOAT", "32_32_FLOAT" };

void LogState(FILE* o)
{
    uint32_t surf = HostReg(reg::RB_SURFACE_INFO);
    fprintf(o, "  rt: pitch %u msaa %ux", surf & 0x3FFF, 1u << ((surf >> 16) & 3));
    const uint32_t colorRegs[4] = { reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO, reg::RB_COLOR3_INFO };
    uint32_t mask = HostReg(reg::RB_COLOR_MASK);
    for (int i = 0; i < 4; ++i)
    {
        if (!((mask >> (i * 4)) & 0xF)) continue;
        uint32_t ci = HostReg(colorRegs[i]);
        fprintf(o, " | color%d base %u fmt %s mask %X", i, ci & 0xFFF, kColorFormats[(ci >> 16) & 0xF], (mask >> (i * 4)) & 0xF);
    }
    uint32_t di = HostReg(reg::RB_DEPTH_INFO), dc = HostReg(reg::RB_DEPTHCONTROL);
    fprintf(o, " | depth base %u fmt %s ctl %08X\n", di & 0xFFF, ((di >> 16) & 1) ? "D24FS8" : "D24S8", dc);
    fprintf(o, "  vp: vte %08X scale (%g,%g,%g) offset (%g,%g,%g) window_offset %08X scissor %08X-%08X\n",
            HostReg(reg::PA_CL_VTE_CNTL), RegF(reg::PA_CL_VPORT_XSCALE), RegF(reg::PA_CL_VPORT_YSCALE),
            RegF(reg::PA_CL_VPORT_ZSCALE), RegF(reg::PA_CL_VPORT_XOFFSET), RegF(reg::PA_CL_VPORT_YOFFSET),
            RegF(reg::PA_CL_VPORT_ZOFFSET), HostReg(reg::PA_SC_WINDOW_OFFSET),
            HostReg(reg::PA_SC_WINDOW_SCISSOR_TL), HostReg(reg::PA_SC_WINDOW_SCISSOR_BR));
    fprintf(o, "  om: blend0 %08X colorctl %08X modectl %08X su_sc %08X clip %08X stencil ref/mask %06X bf %06X\n",
            HostReg(reg::RB_BLENDCONTROL0), HostReg(reg::RB_COLORCONTROL), HostReg(reg::RB_MODECONTROL),
            HostReg(reg::PA_SU_SC_MODE_CNTL), HostReg(reg::PA_CL_CLIP_CNTL), HostReg(reg::RB_STENCILREFMASK) & 0xFFFFFF,
            HostReg(reg::RB_STENCILREFMASK_BF) & 0xFFFFFF);
    auto sz = [](uint64_t h) { auto it = g_shaders.find(h); return it == g_shaders.end() ? 0u : uint32_t(it->second.ucode.size() / 4); };
    fprintf(o, "  sq: vs %016llx (%u dw) ps %016llx (%u dw) program_cntl %08X context_misc %08X\n",
            (unsigned long long)g_activeVs, sz(g_activeVs), (unsigned long long)g_activePs, sz(g_activePs),
            HostReg(reg::SQ_PROGRAM_CNTL), HostReg(reg::SQ_CONTEXT_MISC));
    // Fetch constants: 32 groups of 6 dwords; a group is one texture fetch or three vertex fetches.
    for (uint32_t slot = 0; slot < 32; ++slot)
    {
        uint32_t d[6];
        for (int i = 0; i < 6; ++i) d[i] = HostReg(reg::FETCH_CONSTANTS + slot * 6 + i);
        if ((d[0] & 3) == 2)   // texture
        {
            TextureFetch tf;
            memcpy(tf.dword, d, sizeof(d));
            uint32_t w = tf.width(), h = tf.height();
            fprintf(o, "  tex[%u]: fmt %u %ux%u%s dim %u %s pitch %u base 0x%08X mips 0x%08X endian %u swizzle %03X\n",
                    slot, d[1] & 0x3F, w, h, tf.is3D() || tf.stacked() ? ((tf.stacked() ? " stacked x" : "x") + std::to_string(tf.depth())).c_str() : "", (d[5] >> 9) & 3, (d[0] >> 31) ? "tiled" : "linear",
                    ((d[0] >> 22) & 0x1FF) * 32, d[1] & 0xFFFFF000, d[5] & 0xFFFFF000, (d[1] >> 6) & 3, (d[3] >> 1) & 0xFFF);
        }
        else
            for (int v = 0; v < 3; ++v)
                if ((d[v * 2] & 3) == 3)
                    fprintf(o, "  vtx[%u]: base 0x%08X size %u bytes endian %u\n", slot * 3 + v,
                            d[v * 2] & 0xFFFFFFFC, ((d[v * 2 + 1] >> 2) & 0xFFFFFF) * 4, d[v * 2 + 1] & 3);
    }
}

}  // namespace

const char* PrimitiveName(uint32_t p)
{
    switch (p)
    {
    case 0x01: return "PointList";   case 0x02: return "LineList";      case 0x03: return "LineStrip";
    case 0x04: return "TriangleList"; case 0x05: return "TriangleFan";  case 0x06: return "TriangleStrip";
    case 0x07: return "TriWithWFlags"; case 0x08: return "RectangleList"; case 0x0C: return "LineLoop";
    case 0x0D: return "QuadList";    case 0x0E: return "QuadStrip";     case 0x0F: return "Polygon";
    default:   return "?";
    }
}

void LoadShader(ShaderType type, const uint8_t* ucode, uint32_t dwords, uint32_t guestAddress)
{
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t h = Fnv1a(ucode, size_t(dwords) * 4) ^ uint64_t(type);
    auto it = g_shaders.find(h);
    if (it == g_shaders.end())
    {
        g_shaders[h] = Shader{ type, h, std::vector<uint8_t>(ucode, ucode + size_t(dwords) * 4), guestAddress,
                               ShaderFromBigEndian(type, ucode, dwords) };
        // Memory export (eA/eM) is not implemented: say so the first time such a shader appears,
        // with its disassembly, because whatever it writes will be missing.
        if (ShaderUsesMemExport(g_shaders[h].code))
        {
            fprintf(stderr, "[xenos] swap %llu: %s shader %016llx (guest 0x%08X, %u dwords) uses memexport\n",
                    (unsigned long long)HostSwapCount(), type == ShaderType::kVertex ? "vertex" : "pixel",
                    (unsigned long long)h, guestAddress, dwords);
            std::error_code ec;
            std::filesystem::create_directories("captures/memexport", ec);
            char name[96];
            snprintf(name, sizeof(name), "captures/memexport/%s_%016llx.txt", type == ShaderType::kVertex ? "vs" : "ps",
                     (unsigned long long)h);
            if (FILE* f = fopen(name, "w"))
            {
                std::string text = Disassemble(g_shaders[h].code);
                fwrite(text.data(), 1, text.size(), f);
                fclose(f);
            }
        }
    }
    (type == ShaderType::kVertex ? g_activeVs : g_activePs) = h;
    if (g_cap.active && g_rec.f) g_rec.Shader(type, guestAddress, g_shaders[h].ucode);
}

uint32_t Reg(uint32_t index)
{
    if (g_regOverride) return index < kRegisterCount ? g_regOverride[index] : 0;
    return HostReg(index);
}

void RegRange(uint32_t first, uint32_t count, uint32_t* out)
{
    if (first >= kRegisterCount) count = 0;
    else if (first + count > kRegisterCount) { for (uint32_t i = kRegisterCount - first; i < count; ++i) out[i] = 0; count = kRegisterCount - first; }
    if (g_regOverride) { memcpy(out, g_regOverride + first, count * 4); return; }
    bool be = false;
    if (const uint32_t* file = HostRegFile(&be))
    {
        if (be) for (uint32_t i = 0; i < count; ++i) out[i] = Bswap32(file[first + i]);
        else memcpy(out, file + first, count * 4);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) out[i] = HostReg(first + i);
}

void SetThreadRegisterFile(const uint32_t* regs) { g_regOverride = regs; }

const uint8_t* Phys(uint32_t physicalAddress)
{
    if (t_recOwner && g_rec.f) g_rec.Page(physicalAddress);
    return HostPhys(physicalAddress);
}

void SetDrawObserver(DrawObserver* observer, bool softwareRaster)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_observer = observer;
    g_softwareRaster = softwareRaster || !observer;
}

void SetLiveObserver(DrawObserver* observer)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_live = observer;
}

void CaptureConfigure(const long* frames, size_t count, const std::string& dir, bool recordReplay)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_cap = Capture{};
    g_cap.configured = true;
    g_cap.recordReplay = recordReplay;
    g_cap.targets.assign(frames, frames + count);
    g_cap.baseDir = dir;
    InitCapture();
}

void Draw(const DrawPacket& d)
{
    std::lock_guard<std::mutex> lk(g_mu);
    InitCapture();
    ++g_cap.frameDraws;
    // HC09_TRACE_VS=<hash>: the first draws with that vertex shader (where the packet was).
    static const uint64_t traceVs = [] { const char* e = Hc09Env("HC09_TRACE_VS"); return e ? strtoull(e, nullptr, 16) : 0ull; }();
    if (traceVs && (g_activeVs ^ uint64_t(ShaderType::kVertex)) == traceVs)
    {
        static int n = 0;
        if (n++ < 20)
            fprintf(stderr, "[xenos] swap %llu: vs %016llx draw prim %u, %u indices, packet in %s at 0x%08X\n",
                    (unsigned long long)HostSwapCount(), (unsigned long long)traceVs, d.primType, d.numIndices, d.where, d.packetAddr);
    }
    for (auto& [hash, slot] : g_cap.watchVfetch)
    {
        if (hash != g_activeVs) continue;
        uint32_t d0 = HostReg(reg::FETCH_CONSTANTS + slot * 2), d1 = HostReg(reg::FETCH_CONSTANTS + slot * 2 + 1);
        uint32_t base = d0 & 0x1FFFFFFC, bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
        if ((d0 & 3) != 3 || g_cap.watched[base]) continue;
        g_cap.watched[base] = true;
        fprintf(stderr, "[xenos] swap %llu: vs %016llx vf%u = pa 0x%08X, %u bytes -> watching CPU writes\n",
                (unsigned long long)HostSwapCount(), (unsigned long long)hash, slot, base, bytes);
        HostWatchWrites(base, bytes);
    }
    if (g_cap.traceResolves && (HostReg(reg::RB_MODECONTROL) & 7) == 6)
    {
        uint32_t base = HostReg(reg::RB_COPY_DEST_BASE), pitch = HostReg(reg::RB_COPY_DEST_PITCH);
        if (!g_cap.resolvesSeen[base])
        {
            g_cap.resolvesSeen[base] = true;
            fprintf(stderr, "[xenos] swap %llu: first resolve to 0x%08X (pitch %u height %u fmt %u, copy_control %08X)\n",
                    (unsigned long long)HostSwapCount(), base, pitch & 0x3FFF, (pitch >> 16) & 0x3FFF,
                    (HostReg(reg::RB_COPY_DEST_INFO) >> 7) & 0x3F, HostReg(reg::RB_COPY_CONTROL));
        }
    }
    if (g_live)
    {
        if ((HostReg(reg::RB_MODECONTROL) & 7) == 6)
        {
            ResolveInfo r;
            if (RasterDecodeResolve(r, nullptr)) g_live->OnResolve(r);
            g_live->OnResolved();
        }
        else
        {
            auto vs = g_shaders.find(g_activeVs), ps = g_shaders.find(g_activePs);
            g_live->OnDraw(d, g_activeVs, vs == g_shaders.end() ? nullptr : &vs->second.code, g_activePs,
                           ps == g_shaders.end() ? nullptr : &ps->second.code);
        }
    }
    if (!g_cap.active)
    {
        auto render = [&](const DrawPacket& p, uint64_t vsHash, uint64_t psHash) {
            auto vs = g_shaders.find(vsHash), ps = g_shaders.find(psHash);
            RasterDraw(p, vs == g_shaders.end() ? nullptr : &vs->second.code, ps == g_shaders.end() ? nullptr : &ps->second.code,
                       std::string(), nullptr);
        };
        long swaps = long(HostSwapCount());
        if (g_cap.renderFrom >= 0 && swaps >= g_cap.renderFrom && swaps < g_cap.renderTo) render(d, g_activeVs, g_activePs);
        else if (g_cap.renderingLive) render(d, g_activeVs, g_activePs);
        else if (g_cap.buffering)
        {
            BufferedDraw b{ d, g_activeVs, g_activePs, std::vector<uint32_t>(kRegisterCount) };
            for (uint32_t i = 0; i < kRegisterCount; ++i) b.regs[i] = HostReg(i);
            g_buffered.push_back(std::move(b));
            if (g_cap.frameDraws > kHeavyFrameDraws)
            {
                fprintf(stderr, "[xenos] swap %ld: heavy frame after a light one, rendering it\n", swaps);
                RasterNewFrame();
                for (const BufferedDraw& bd : g_buffered)
                {
                    g_regOverride = bd.regs.data();
                    render(bd.d, bd.vs, bd.ps);
                }
                g_regOverride = nullptr;
                g_buffered.clear();
                g_cap.buffering = false;
                g_cap.renderingLive = true;
            }
        }
        return;
    }
    if (g_rec.f) g_rec.Draw(d);
    uint32_t mode = HostReg(reg::RB_MODECONTROL) & 7;
    FILE* o = g_cap.out;
    if (mode == 6)   // EdramMode::kCopy: a resolve
    {
        ++g_cap.copies;
        uint32_t cc = HostReg(reg::RB_COPY_CONTROL), pitch = HostReg(reg::RB_COPY_DEST_PITCH);
        fprintf(o, "#%u RESOLVE (%s, %u indices) copy_control %08X (src %u, sample %u, clear color %u depth %u, cmd %u)\n",
                g_cap.draws + g_cap.copies, PrimitiveName(d.primType), d.numIndices, cc, cc & 7, (cc >> 4) & 7,
                (cc >> 8) & 1, (cc >> 9) & 1, (cc >> 20) & 3);
        fprintf(o, "  dest base 0x%08X pitch %u height %u info %08X (fmt %u endian %u) slice %08X clear color %08X depth %08X\n",
                HostReg(reg::RB_COPY_DEST_BASE), pitch & 0x3FFF, (pitch >> 16) & 0x3FFF, HostReg(reg::RB_COPY_DEST_INFO),
                (HostReg(reg::RB_COPY_DEST_INFO) >> 7) & 0x3F, HostReg(reg::RB_COPY_DEST_INFO) & 7,
                HostReg(reg::RB_COPY_SURFACE_SLICE), HostReg(reg::RB_COLOR_CLEAR), HostReg(reg::RB_DEPTH_CLEAR));
        LogState(o);
        std::string note;
        if (g_observer && !g_softwareRaster && g_observer->PerformsResolves())
        {
            ResolveInfo r;
            if (RasterDecodeResolve(r, &note))
            {
                g_observer->OnResolve(r);
                RasterResolveDone(r, g_cap.dir, &note);
            }
            g_observer->OnResolved();
        }
        else
        {
            if (g_observer) g_observer->OnDraw(d, 0, nullptr, 0, nullptr);   // hands its render targets back to EDRAM
            RasterDraw(d, nullptr, nullptr, g_cap.dir, &note);
            if (g_observer) g_observer->OnResolved();
        }
        if (!note.empty()) fprintf(o, "  => %s\n", note.c_str());
        return;
    }
    ++g_cap.draws;
    fprintf(o, "#%u DRAW %s(%u) %u indices, %s", g_cap.draws + g_cap.copies, PrimitiveName(d.primType), d.primType,
            d.numIndices, d.sourceSelect == 2 ? "auto" : d.sourceSelect == 0 ? "indexed" : "immediate");
    if (d.sourceSelect == 0)
        fprintf(o, " (%s-bit, base 0x%08X, %u words, endian %u, offset %u)", d.indexSize32 ? "32" : "16", d.dmaBase,
                d.dmaWords, d.dmaEndian, HostReg(reg::VGT_INDX_OFFSET));
    fprintf(o, " edram_mode %u @%s 0x%08X\n", mode, d.where, d.packetAddr);
    LogState(o);
    if (HostReg(reg::RB_COLOR_MASK)) DumpBoundTextures(o);
    WriteShaderFile(g_activeVs);
    WriteShaderFile(g_activePs);
    {
        auto vs = g_shaders.find(g_activeVs), ps = g_shaders.find(g_activePs);
        const ShaderCode* vsCode = vs == g_shaders.end() ? nullptr : &vs->second.code;
        const ShaderCode* psCode = ps == g_shaders.end() ? nullptr : &ps->second.code;
        RasterStats before = RasterGetStats();
        RasterSetDrawNumber(g_cap.draws + g_cap.copies);
        std::string note;
        if (g_observer) g_observer->OnDraw(d, g_activeVs, vsCode, g_activePs, psCode);
        if (g_softwareRaster) RasterDraw(d, vsCode, psCode, g_cap.dir, &note);
        else note = "rendered by the GPU backend";
        const RasterStats& after = RasterGetStats();
        fprintf(o, "  => %u triangles, %u pixels%s%s\n", after.triangles - before.triangles, after.pixels - before.pixels,
                note.empty() ? "" : "; ", note.c_str());
    }
}

void FrameEnd(uint32_t frontBuffer, uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lk(g_mu);
    InitCapture();
    if (g_live) g_live->OnFrameEnd(frontBuffer, width, height);
    long swaps = long(HostSwapCount());   // already counts this swap
    if (g_cap.afterLoad && !g_cap.active)
    {
        if (g_cap.renderingLive) fprintf(stderr, "[xenos] swap %ld: post-load frame rendered (%u draws)\n", swaps, g_cap.frameDraws);
        g_cap.renderingLive = false;
        g_buffered.clear();
        g_cap.buffering = g_cap.frameDraws < kLightFrameDraws;   // the next frame follows a light one
    }
    g_cap.prevFrameDraws = g_cap.frameDraws;
    g_cap.frameDraws = 0;
    if (!g_cap.active && g_cap.renderFrom >= 0 && swaps >= g_cap.renderFrom && swaps <= g_cap.renderTo)
    {
        RasterNewFrame();
        if (swaps % 100 == 0) fprintf(stderr, "[xenos] quiet render: swap %ld\n", swaps);
    }
    if (g_cap.active)
    {
        fprintf(g_cap.out, "== frame end: XE_SWAP front buffer 0x%08X %ux%u; %u draws, %u resolves, %zu shaders known\n",
                frontBuffer, width, height, g_cap.draws, g_cap.copies, g_shaders.size());
        if (g_rec.f) g_rec.Frame(frontBuffer, width, height);
        if (g_observer) g_observer->OnFrameEnd(frontBuffer, width, height);   // a GPU renderer finishes the frame
        Image img;
        if (DecodeFrontBuffer(frontBuffer, width, height, img)) WritePng(g_cap.dir + "/frame.png", img);
        g_rec.Close();
        t_recOwner = false;
        const RasterStats& st = RasterGetStats();
        fprintf(g_cap.out, "== raster: %u draws, %u triangles (%u clipped away, %u culled), %u pixels, %u resolves, %u skipped\n",
                st.draws, st.triangles, st.clipped, st.culled, st.pixels, st.resolves, st.skipped);
        fclose(g_cap.out);
        g_cap.out = nullptr;
        g_cap.active = false;
        fprintf(stderr, "[xenos] captured frame %ld: %u draws, %u resolves, %u textures -> %s\n", g_cap.target,
                g_cap.draws, g_cap.copies, g_cap.textures, g_cap.dir.c_str());
        // Next target.
        ++g_cap.next;
        g_cap.done = g_cap.next >= g_cap.targets.size();
        g_cap.target = g_cap.done ? -1 : g_cap.targets[g_cap.next];
        return;
    }
    while (!g_cap.done && swaps > g_cap.target)   // already past it
    {
        ++g_cap.next;
        g_cap.done = g_cap.next >= g_cap.targets.size();
        g_cap.target = g_cap.done ? -1 : g_cap.targets[g_cap.next];
    }
    if (!g_cap.done && g_cap.target >= 0 && swaps == g_cap.target)
    {
        g_cap.draws = g_cap.copies = g_cap.textures = 0;
        g_cap.shadersWritten.clear();
        g_cap.texturesWritten.clear();
        RasterBeginFrame();
        g_cap.dir = g_cap.baseDir + "/frame_" + std::to_string(g_cap.target);
        std::error_code ec;
        std::filesystem::create_directories(g_cap.dir + "/shaders", ec);
        g_cap.out = fopen((g_cap.dir + "/draws.txt").c_str(), "w");
        if (!g_cap.out) { fprintf(stderr, "[xenos] cannot write %s\n", g_cap.dir.c_str()); g_cap.done = true; return; }
        g_cap.active = true;
        if (g_cap.recordReplay && (t_recOwner = g_rec.Open(g_cap.dir + "/replay.bin", uint64_t(g_cap.target))))
            for (uint64_t h : { g_activeVs, g_activePs })
            {
                auto it = g_shaders.find(h);
                if (it != g_shaders.end()) g_rec.Shader(it->second.type, it->second.guestAddress, it->second.ucode);
            }
        fprintf(g_cap.out, "== frame after swap %ld\n", g_cap.target);
        fprintf(stderr, "[xenos] capturing frame %ld -> %s\n", g_cap.target, g_cap.dir.c_str());
    }
}

}  // namespace xenos
