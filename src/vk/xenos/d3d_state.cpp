#include "../dev_env.h"
#include "d3d_state.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace d3dn {

using namespace d3dcap;

namespace {
// The block sub_8232B958 flushes (dirty bits 49-54 of +32): 4 registers per bit
// from 0x2388, shadowed right after the bool/loop constants.
constexpr ShadowBlock kExtraBlock = { 10272, 0x2388, 24 };
// dev+10436 / +10440 hold the window scissor (PA_SC_WINDOW_SCISSOR_TL/BR, written
// straight into the ring by sub_82324070), not registers 0x2011 / 0x2012.
constexpr uint32_t kWindowScissor = 10436;
}  // namespace

// Shadow's blocks: kShadowBlocks (0-6), the extra block (7), the window scissor (8), fetch
// constants in 8 groups of 24 dwords (9-16), bool/loop constants (17).
namespace {
constexpr uint32_t kBlockExtra = 7, kBlockWindow = 8, kBlockFetch = 9, kFetchGroup = 24, kBlockBool = 17;
}  // namespace

uint32_t RegisterState::ShadowInto(const uint8_t* d, uint32_t* regs, Cache& c, uint32_t visit)
{
    if (c.raw.empty()) c.raw.resize(kDeviceBytes);
    if (!c.valid) visit = ~0u;
    // A block whose device bytes are unchanged since the last call keeps its registers.
    auto same = [&](uint32_t off, uint32_t bytes) {
        if (c.valid && !memcmp(c.raw.data() + off, d + off, bytes)) return true;
        memcpy(c.raw.data() + off, d + off, bytes);
        return false;
    };
    uint32_t changed = 0;
    for (uint32_t k = 0; k < std::size(kShadowBlocks); ++k)
    {
        const ShadowBlock& b = kShadowBlocks[k];
        if (!(visit & (1u << k)) || same(b.off, 4 * b.count)) continue;
        for (uint32_t i = 0; i < b.count; ++i) regs[b.reg + i] = BE32(d + b.off + 4 * i);
        changed |= 1u << k;
    }
    // The extra block overlaps the last one (0x2388-0x238F): decoded again after it.
    bool extraSame = !(visit & (1u << kBlockExtra)) || same(kExtraBlock.off, 4 * kExtraBlock.count);
    if (!extraSame || (changed & (1u << (std::size(kShadowBlocks) - 1))))
    {
        for (uint32_t i = 0; i < kExtraBlock.count; ++i) regs[kExtraBlock.reg + i] = BE32(d + kExtraBlock.off + 4 * i);
        changed |= 1u << kBlockExtra;
    }
    regs[0x2011] = regs[0x2012] = 0;   // inside block 0's range: not registers (kWindowScissor)
    // The window scissor's bytes lie inside block 0's (as 0x2011/0x2012): compared on their own, always
    // (sub_82324070 writes them straight into the ring: no dirty bit).
    if (!c.valid || memcmp(c.window, d + kWindowScissor, 8))
    {
        memcpy(c.window, d + kWindowScissor, 8);
        regs[xenos::reg::PA_SC_WINDOW_OFFSET] = 0;
        regs[xenos::reg::PA_SC_WINDOW_SCISSOR_TL] = BE32(d + kWindowScissor);
        regs[xenos::reg::PA_SC_WINDOW_SCISSOR_BR] = BE32(d + kWindowScissor + 4);
        changed |= 1u << kBlockWindow;
    }
    for (uint32_t g = 0; g < 192 / kFetchGroup; ++g)
    {
        uint32_t off = dev::kFetch + 4 * kFetchGroup * g;
        if (!(visit & (1u << (kBlockFetch + g))) || same(off, 4 * kFetchGroup)) continue;
        for (uint32_t i = 0; i < kFetchGroup; ++i) regs[xenos::reg::FETCH_CONSTANTS + kFetchGroup * g + i] = BE32(d + off + 4 * i);
        changed |= 1u << (kBlockFetch + g);
    }
    if ((visit & (1u << kBlockBool)) && !same(dev::kBoolLoop, 4 * 40))
    {
        for (uint32_t i = 0; i < 40; ++i) regs[xenos::reg::BOOL_CONSTANTS + i] = BE32(d + dev::kBoolLoop + 4 * i);
        changed |= 1u << kBlockBool;
    }
    c.valid = true;
    return changed;
}

// Which blocks D3D's draw flush writes for its dirty masks (sub_8233D760 and the other draws; one register per bit,
// the highest bit first, sub_8232B898): +16 bits 0-11 registers 0x220B..0x2200 (block 3), 12-16 0x2184..0x2180 (block
// 2), 17-20 the shaders (sub_8232C9D0, which itself writes 0x2001, 0x2180-0x2182 and 0x2208), 21-41 0x2114..0x2100
// (block 1), 42-57 0x200F..0x2000 (block 0; 0x2010-0x2012 have no bit); +24 bits 0-31 fetch constants slot 31..0
// (sub_8232BAD8), 34-54 0x2294..0x2280 (block 4); +32 bits 0-37 0x2325..0x2300 (block 5), 38-45 0x2387..0x2380 (block
// 6), 49-54 the extra block, 4 registers per bit (sub_8232B958), 56 the bool/loop constants. Any other bit: every block.
uint32_t RegisterState::VisitOf(uint64_t m16, uint64_t m24, uint64_t m32)
{
    constexpr uint64_t k16 = (1ull << 58) - 1, k24 = 0xFFFFFFFFull | (0x1FFFFFull << 34),
                       k32 = ((1ull << 46) - 1) | (0x3Full << 49) | (1ull << 56);
    if ((m16 & ~k16) || (m24 & ~k24) || (m32 & ~k32)) return ~0u;
    uint32_t v = 1u << kBlockWindow;
    bool shaders = (m16 >> 17) & 0xF;
    if ((m16 & (0xFFFFull << 42)) || shaders) v |= 1u << 0;
    if (m16 & (0x1FFFFFull << 21)) v |= 1u << 1;
    if ((m16 & (0x1Full << 12)) || shaders) v |= 1u << 2;
    if ((m16 & 0xFFFull) || shaders) v |= 1u << 3;
    if (m24 & (0x1FFFFFull << 34)) v |= 1u << 4;
    if (m32 & ((1ull << 38) - 1)) v |= 1u << 5;
    if (m32 & (0xFFull << 38)) v |= 1u << 6;
    if (m32 & (0x3Full << 49)) v |= 1u << kBlockExtra;
    for (uint32_t g = 0; g < 8; ++g)   // slots 4g..4g+3 = bits 31-4g .. 28-4g
        if ((m24 >> (28 - 4 * g)) & 0xF) v |= 1u << (kBlockFetch + g);
    if (m32 & (1ull << 56)) v |= 1u << kBlockBool;
    return v;
}

void RegisterState::NoteDirty(const uint8_t* d)
{
    pend_[0] |= BE64(d + dev::kDirtyState);
    pend_[1] |= BE64(d + dev::kDirtyFetch);
    pend_[2] |= BE64(d + dev::kDirtyMisc);
}

void RegisterState::Shadow(const uint8_t* d, uint32_t visit)
{
    blocksChanged_ |= ShadowInto(d, regs_.data(), cache_, visit);
}

void RegisterState::Check(const uint8_t* d, uint32_t visit, uint64_t m16, uint64_t m24, uint64_t m32)
{
    if (chk_.empty()) chk_ = regs_;
    ShadowInto(d, chk_.data(), chkCache_, ~0u);
    // The shadowed registers (blocks 0-6, the extra block, the window, fetch and bool/loop constants).
    auto differ = [&](uint32_t lo, uint32_t n, const char* what) {
        for (uint32_t i = lo; i < lo + n; ++i)
            if (regs_[i] != chk_[i])
            {
                if (++statMismatch <= 40)
                    fprintf(stderr, "[native] dirty-mask check: draw %llu, %s register 0x%04X is 0x%08X, a full compare gives 0x%08X"
                                    " (masks +16 %016llx +24 %016llx +32 %016llx, blocks read %05X)\n",
                            (unsigned long long)statDraws, what, i, regs_[i], chk_[i], (unsigned long long)m16, (unsigned long long)m24,
                            (unsigned long long)m32, visit);
                regs_[i] = chk_[i];   // continue from the full compare's state (one report per miss)
                blocksChanged_ = ~0u;
                return true;
            }
        return false;
    };
    for (const ShadowBlock& b : kShadowBlocks) differ(b.reg, b.count, "block");
    differ(kExtraBlock.reg, kExtraBlock.count, "extra block");
    differ(xenos::reg::PA_SC_WINDOW_OFFSET, 3, "window");
    differ(xenos::reg::FETCH_CONSTANTS, 192, "fetch");
    differ(xenos::reg::BOOL_CONSTANTS, 40, "bool/loop");
}

void RegisterState::TouchConstants(uint32_t first, uint32_t count)
{
    for (uint32_t k = first; k < first + count && k < 512; ++k) constTouched_[k >> 6] |= 1ull << (k & 63);
}

void RegisterState::TakeChanged(std::vector<std::pair<uint32_t, uint32_t>>& ranges)
{
    ranges.clear();
    // The shadow blocks that changed (Shadow); 0x2011/0x2012 (zeroed with the window) stay with them.
    uint32_t ch = blocksChanged_;
    for (uint32_t k = 0; k < std::size(kShadowBlocks); ++k)
        if (ch & (1u << k)) ranges.emplace_back(kShadowBlocks[k].reg, kShadowBlocks[k].reg + kShadowBlocks[k].count - 1);
    if (ch & (1u << kBlockExtra)) ranges.emplace_back(kExtraBlock.reg, kExtraBlock.reg + kExtraBlock.count - 1);
    if (ch & (1u << kBlockWindow)) ranges.emplace_back(xenos::reg::PA_SC_WINDOW_OFFSET, xenos::reg::PA_SC_WINDOW_SCISSOR_BR);
    ranges.emplace_back(xenos::reg::VGT_INDX_OFFSET, xenos::reg::VGT_INDX_OFFSET);
    for (uint32_t g = 0; g < 192 / kFetchGroup; ++g)
        if (ch & (1u << (kBlockFetch + g)))
            ranges.emplace_back(xenos::reg::FETCH_CONSTANTS + kFetchGroup * g, xenos::reg::FETCH_CONSTANTS + kFetchGroup * g + kFetchGroup - 1);
    if (ch & (1u << kBlockBool)) ranges.emplace_back(xenos::reg::BOOL_CONSTANTS, xenos::reg::BOOL_CONSTANTS + 39);
    blocksChanged_ = 0;
    // Runs of touched constants, skipping untouched 64-constant words (called for every draw).
    for (uint32_t k = 0; k < 512;)
    {
        uint64_t bits = constTouched_[k >> 6] & (~0ull << (k & 63));
        if (!bits) { k = ((k >> 6) + 1) << 6; continue; }
        uint32_t first = (k & ~63u) + uint32_t(std::countr_zero(bits));
        k = first;
        while (k + 1 < 512 && ((constTouched_[(k + 1) >> 6] >> ((k + 1) & 63)) & 1)) ++k;
        ranges.emplace_back(xenos::reg::ALU_CONSTANTS + 4 * first, xenos::reg::ALU_CONSTANTS + 4 * k + 3);
        ++k;
    }
    memset(constTouched_, 0, sizeof(constTouched_));
}

void RegisterState::PointerConstants(bool pixel, uint32_t start, uint32_t count, uint32_t address)
{
    uint32_t first = start + (pixel ? 256 : 0);
    TouchConstants(first, count);
    for (uint32_t i = 0; i < count && first + i < 512; ++i) ext_[first + i] = address + 16 * i;
}

void RegisterState::Reset(const uint8_t* d)
{
    static const bool masked = [] { const char* e = Hc09Env("HC09_NATIVE_DIRTY"); return !e || atoi(e) != 0; }();
    static const bool check = [] { const char* e = Hc09Env("HC09_NATIVE_DIRTY_CHECK"); return e && atoi(e) != 0; }();
    masked_ = masked;
    check_ = check;
    TouchConstants(0, 512);
    memset(ext_, 0, sizeof(ext_));
    cache_.valid = chkCache_.valid = false;
    chk_.clear();
    full_ = true;
    pend_[0] = pend_[1] = pend_[2] = 0;
    blocksChanged_ = ~0u;
    Shadow(d);
    for (uint32_t i = 0; i < 1024; ++i)
    {
        regs_[xenos::reg::ALU_CONSTANTS + i] = BE32(d + dev::kVsConst + 4 * i);
        regs_[xenos::reg::ALU_CONSTANTS + 1024 + i] = BE32(d + dev::kPsConst + 4 * i);
    }
}

// sub_8232C7E0(dev, header, codeBase): the shader's literal constants. Header
// (the shader blob header kept in the object) +20 = offset of a table:
// +16 table bytes, from +20 records {u16 first constant (PS + 256), u16 dwords,
// u32 data offset from codeBase}, each sent as one LOAD_ALU_CONSTANT.
void RegisterState::Literals(uint32_t header, uint32_t codeBase, const GuestMemory& mem)
{
    uint32_t off = mem.Load32(header + 20);
    if (!off) return;
    uint32_t t = header + off, rec = t + 20, end = rec + mem.Load32(t + 16);
    for (int guard = 0; rec < end && guard < 512; ++guard)
    {
        uint32_t w = mem.Load32(rec), first = w >> 16, dwords = w & 0xFFFF, data = mem.Load32(rec + 4);
        if (!dwords) break;
        rec += 8;
        TouchConstants(first, (dwords + 3) / 4);
        for (uint32_t k = first; k < first + (dwords + 3) / 4 && k < 512; ++k) ext_[k] = 0;
        for (uint32_t i = 0; i < dwords && 4 * first + i < 2048; ++i)
            regs_[xenos::reg::ALU_CONSTANTS + 4 * first + i] = mem.Load32(codeBase + data + 4 * i);
    }
}

void RegisterState::Draw(const uint8_t* pre, const uint8_t* post, uint32_t indexOffset, const GuestMemory& mem)
{
    // Float constants: the 4-constant groups marked dirty are sent from the
    // shadow (sub_8232BC38: bit 63 = constants 0-3); the others keep what the
    // GPU has, which may come from the ring (RingConstants).
    for (int ps = 0; ps < 2; ++ps)
    {
        uint64_t mask = BE64(pre + (ps ? dev::kDirtyPsConst : dev::kDirtyVsConst));
        uint32_t src = ps ? dev::kPsConst : dev::kVsConst;
        while (mask)   // set bits only, group 0 (bit 63) first
        {
            uint32_t k = uint32_t(__builtin_clzll(mask));
            mask &= ~(1ull << (63 - k));
            TouchConstants(256 * ps + 4 * k, 4);
            for (uint32_t i = 0; i < 4; ++i) ext_[256 * ps + 4 * k + i] = 0;
            for (uint32_t i = 0; i < 16; ++i)
                regs_[xenos::reg::ALU_CONSTANTS + 1024 * ps + 16 * k + i] = BE32(post + src + 64 * k + 4 * i);
        }
    }
    // Shaders bound since the last draw (sub_8232C9D0, dirty bits of +16:
    // 0x80000 vertex shader, 0x100000 pixel shader) load their literal constants.
    uint64_t state = BE64(pre + dev::kDirtyState);
    if (uint32_t ps = BE32(post + dev::kPixelShader); ps && (state & 0x100000)) Literals(ps + 40, mem.Load32(ps + 24), mem);
    if (uint32_t vs = BE32(post + dev::kVertexShader); vs && (state & 0x80000)) Literals(vs + 872, mem.Load32(vs + 32), mem);
    // The blocks D3D flushes now: its dirty masks since the last draw.
    uint64_t m16 = pend_[0] | state, m24 = pend_[1] | BE64(pre + dev::kDirtyFetch), m32 = pend_[2] | BE64(pre + dev::kDirtyMisc);
    uint32_t visit = full_ || !masked_ ? ~0u : VisitOf(m16, m24, m32);
    pend_[0] = pend_[1] = pend_[2] = 0;
    full_ = false;
    ++statDraws;
    statFull += visit == ~0u;
    statBlocks += std::popcount(visit & ((1u << (kBlockBool + 1)) - 1));
    Shadow(post, visit);
    if (check_) Check(post, visit, m16, m24, m32);
    regs_[xenos::reg::VGT_INDX_OFFSET] = indexOffset;
}

void RegisterState::RingConstants(bool pixel, uint32_t start, uint32_t count, const uint8_t* data)
{
    TouchConstants(start + (pixel ? 256 : 0), count);
    for (uint32_t k = start + (pixel ? 256 : 0); k < start + (pixel ? 256 : 0) + count && k < 512; ++k) ext_[k] = 0;
    uint32_t first = xenos::reg::ALU_CONSTANTS + 4 * (start + (pixel ? 256 : 0));
    for (uint32_t i = 0; i < 4 * count && first + i < xenos::reg::ALU_CONSTANTS + 2048; ++i) regs_[first + i] = BE32(data + 4 * i);
}

}  // namespace d3dn

namespace d3dn {

Surface ReadSurface(uint32_t object, bool depth, const GuestMemory& mem)
{
    Surface s;
    if (!object) return s;
    s.key = object;
    s.depth = depth;
    uint32_t si = mem.Load32(object + 24), info = mem.Load32(object + 28), size = mem.Load32(object + 36);
    s.width = ((size >> 18) & 0x3FFF) + 1;
    s.height = ((size >> 3) & 0x7FFF) + 1;
    s.msaa = (si >> 16) & 3;
    s.edramBase = info & 0xFFF;
    s.format = depth ? (info >> 16) & 1 : (info >> 16) & 0xF;
    return s;
}

bool DecodeResolve(uint32_t flags, const int32_t* rect, const uint32_t f[6], const int32_t* point, const float* color, float z,
                   uint32_t stencil, uint32_t sourceMsaa, ResolveOp& r)
{
    r = ResolveOp{};
    r.src = flags & 7;
    uint32_t frag = (flags >> 4) & 7;
    if (!frag) frag = sourceMsaa == 2 ? 7 : sourceMsaa == 1 ? 5 : 1;   // the surface's own samples
    r.sampleSel = frag - 1;
    r.clearColor = (flags & 0x100) != 0 && r.src < 4;
    r.clearDepth = (flags & 0x200) != 0;
    if (color) for (int i = 0; i < 4; ++i) r.color[i] = color[i];
    r.z = z;
    r.stencil = stencil;
    int32_t expBias = int32_t(flags) >> 26;   // D3DRESOLVE_EXPONENTBIAS (signed 6 bits)
    r.destScale = float(expBias >= 0 ? double(1u << expBias) : 1.0 / double(1u << -expBias));

    // Destination from the texture's fetch constant (as sub_82322A98 builds RB_COPY_DEST_*).
    uint32_t texFmt = f[1] & 0x3F;
    bool is3D = ((f[5] >> 9) & 3) == 2;
    uint32_t w = is3D ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1, h = is3D ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
    r.destPitch = ((f[0] >> 22) & 0x1FF) * 32;
    r.endian = (f[1] >> 6) & 3;
    switch (texFmt)   // texture format -> colour format of the copy
    {
    case 54: r.destFmt = 7; break; case 55: r.destFmt = 16; break; case 56: r.destFmt = 17; break;
    case 27: r.destFmt = 30; break; case 28: r.destFmt = 31; break; case 29: r.destFmt = 32; break;
    case 22: case 23: r.destFmt = 6; break;
    default: r.destFmt = texFmt; break;
    }
    r.swapRB = ((f[3] >> 1) & 7) != 0;   // swizzle x not taken from x
    switch (r.destFmt)
    {
    case 2: r.destBytes = 1; break;
    case 4: case 10: case 24: r.destBytes = 2; break;
    case 6: case 7: case 25: case 31: case 36: r.destBytes = 4; break;
    case 26: case 32: case 37: r.destBytes = 8; break;
    default: r.destBytes = 0; break;
    }
    if (r.src >= 4) r.destBytes = 4;   // depth: the raw 24:8 value

    int32_t x1 = 0, y1 = 0, x2 = int32_t(w), y2 = int32_t(h);
    if (rect) { x1 = rect[0]; y1 = rect[1]; x2 = rect[2]; y2 = rect[3]; }
    int32_t px = point ? point[0] : 0, py = point ? point[1] : 0;
    r.x0 = std::max(0, x1) & ~7;
    r.y0 = std::max(0, y1) & ~7;
    r.x1 = (x2 + 7) & ~7;
    r.y1 = (y2 + 7) & ~7;
    // The destination base moves by the point's offset from the rectangle, in whole
    // 32x32 tiles: pixel (x, y) of the source lands at tiledOffset(x, y) from it.
    uint32_t bpp = r.destBytes ? r.destBytes : 4;
    r.destBase = ((f[1] & 0xFFFFF000u) + uint32_t((py - y1) * int32_t(r.destPitch) + (px - x1) * 32) * bpp) & 0x1FFFFFFFu;
    r.destHeight = uint32_t(int32_t(h) + y1 - py);
    r.texBase = (f[1] & 0xFFFFF000u) & 0x1FFFFFFFu;
    r.texWidth = w;
    r.texHeight = h;
    r.destX = px - x1;
    r.destY = py - y1;
    r.copy = !(flags & 0x2000) && r.destBytes != 0;
    return r.x1 > r.x0 && r.y1 > r.y0 && (r.copy || r.clearColor || r.clearDepth);
}

}  // namespace d3dn
