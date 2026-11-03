// Xenos shader microcode decoding, disassembly and interpretation.
//
// Program layout: control-flow (CF) instructions are 48 bits, packed two per
// three dwords from the start of the program. An exec CF runs `count` ALU or
// fetch instructions (three dwords each) starting at instruction `address`
// (counted in 3-dword units from the start of the program); a 2-bit-per-
// instruction `sequence` field marks fetches (bit 0). Semantics follow the
// hardware as documented by Xenia (BSD-3-Clause).
#include "xenos_shader.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace xenos {

namespace {

inline uint32_t Bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline int32_t SBits(uint32_t v, int lo, int n) { return int32_t(v << (32 - lo - n)) >> (32 - n); }
inline float AsF(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t AsU(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float FlushDenormal(float f) { uint32_t u = AsU(f); return (u & 0x7F800000) ? f : AsF(u & 0x80000000); }
inline float Saturate(float f) { return f > 1.0f ? 1.0f : (f >= 0.0f ? f : 0.0f); }   // NaN -> 0

// Xenos half: no inf/NaN (exponent 31 is an ordinary exponent); denormals flush to 0.
float XenosHalf(uint16_t h)
{
    uint32_t m = h & 0x3FF, e = (h >> 10) & 0x1F;
    uint32_t sign = uint32_t(h & 0x8000) << 16;
    if (!e) return AsF(sign);
    return AsF(sign | ((e + 112) << 23) | (m << 13));
}

struct Cf { uint32_t d0, d1; uint32_t op() const { return Bits(d1, 12, 4); } };
Cf GetCf(const std::vector<uint32_t>& dw, uint32_t index)
{
    size_t p = 3 * (index >> 1);
    if (p + 2 >= dw.size()) return { 0, 0 };
    if (index & 1) return { (dw[p + 1] >> 16) | (dw[p + 2] << 16), dw[p + 2] >> 16 };
    return { dw[p], dw[p + 1] & 0xFFFF };
}

bool CfIsExec(uint32_t op) { return (op >= 1 && op <= 6) || op == 13 || op == 14; }
bool CfEnds(uint32_t op) { return op == 2 || op == 4 || op == 6 || op == 14; }

const char* kCfNames[16] = { "nop", "exec", "exece", "cexec", "cexece", "cexec_pred", "cexece_pred", "loop",
                             "endloop", "ccall", "ret", "cjmp", "alloc", "cexec_pred_clean", "cexece_pred_clean",
                             "vfetch_end" };
const char* kVecNames[32] = { "add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad",
                              "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push",
                              "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq", "kill_gt", "kill_ge",
                              "kill_ne", "dst", "maxa", "op30", "op31" };
const char* kScaNames[64] = { "adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts",
                              "sges", "snes", "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp",
                              "rsqc", "rsqf", "rsq", "maxas", "maxasf", "subs", "subs_prev", "setp_eq", "setp_ne",
                              "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr", "setp_rstr", "kills_eq",
                              "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", "op41", "mulsc0", "mulsc1",
                              "addsc0", "addsc1", "subsc0", "subsc1", "sin", "cos", "retain_prev" };
// Operand components used per vector opcode (src1, src2, src3).
const uint8_t kVecOperands[32][3] = {
    { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 },
    { 15, 0, 0 },  { 15, 0, 0 },  { 15, 0, 0 },  { 15, 15, 15 }, { 15, 15, 15 }, { 15, 15, 15 }, { 15, 15, 15 },
    { 15, 15, 0 }, { 7, 7, 0 },   { 3, 3, 1 },   { 15, 15, 0 }, { 15, 0, 0 },  { 9, 9, 0 },  { 9, 9, 0 },  { 9, 9, 0 },
    { 9, 9, 0 },   { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 6, 10, 0 }, { 15, 15, 0 }, { 0, 0, 0 },
    { 0, 0, 0 } };
// Scalar opcode operand count (0, 1 or 2 = const+temp form) and whether the single operand uses two components.
const uint8_t kScaOperands[64] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 2, 2, 2, 2, 2, 2, 1, 1, 0 };
const bool kScaTwoComponent[64] = { true, false, true, false, true, true, true, false, false, false, false, false, false,
                                    false, false, false, false, false, false, false, false, false, false, true, true, true };

inline uint32_t SwizzledComponent(uint32_t swz, uint32_t i) { return ((swz >> (2 * i)) + i) & 3; }

std::string SwizzleText(uint32_t swz)
{
    std::string s = ".";
    for (uint32_t i = 0; i < 4; ++i) s += "xyzw"[SwizzledComponent(swz, i)];
    return s == ".xyzw" ? "" : s;
}

}  // namespace

ShaderCode ShaderFromBigEndian(ShaderType type, const uint8_t* b, uint32_t dwords)
{
    ShaderCode c;
    c.type = type;
    c.dw.resize(dwords);
    for (uint32_t i = 0; i < dwords; ++i)
        c.dw[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) | (uint32_t(b[i * 4 + 2]) << 8) | b[i * 4 + 3];
    return c;
}

// ---- disassembler -------------------------------------------------------------
std::string Disassemble(const ShaderCode& code)
{
    std::string out;
    char line[256];
    const auto& dw = code.dw;
    for (uint32_t cfi = 0; cfi < 2 * (dw.size() / 3); ++cfi)
    {
        Cf cf = GetCf(dw, cfi);
        uint32_t op = cf.op();
        snprintf(line, sizeof(line), "cf%-3u %s", cfi, kCfNames[op]);
        out += line;
        if (CfIsExec(op))
        {
            uint32_t addr = Bits(cf.d0, 0, 12), count = Bits(cf.d0, 12, 3), seq = Bits(cf.d0, 16, 12);
            snprintf(line, sizeof(line), " addr %u count %u", addr, count);
            out += line;
            if (op == 3 || op == 4 || op == 13 || op == 14)
            { snprintf(line, sizeof(line), " if b%u == %u", Bits(cf.d1, 2, 8), Bits(cf.d1, 10, 1)); out += line; }
            if (op == 5 || op == 6) { snprintf(line, sizeof(line), " if p == %u", Bits(cf.d1, 10, 1)); out += line; }
            out += "\n";
            for (uint32_t k = 0; k < count; ++k)
            {
                uint32_t ia = addr + k;
                if (3 * ia + 2 >= dw.size()) { out += "      <out of range>\n"; continue; }
                const uint32_t* in = &dw[3 * ia];
                if ((seq >> (2 * k)) & 1)
                {
                    uint32_t fop = Bits(in[0], 0, 5);
                    uint32_t dst = Bits(in[0], 12, 6), src = Bits(in[0], 5, 6), cidx = Bits(in[0], 20, 5);
                    if (fop == 0)
                        snprintf(line, sizeof(line), "    %3u vfetch%s r%u, r%u.%c, vf%u fmt %u stride %u offset %d dswz %03X%s\n", ia,
                                 Bits(in[1], 30, 1) ? "_mini" : "", dst, src, "xyzw"[Bits(in[0], 30, 2)],
                                 cidx * 3 + Bits(in[0], 25, 2), Bits(in[1], 16, 6), Bits(in[2], 0, 8), SBits(in[2], 8, 23),
                                 Bits(in[1], 0, 12), Bits(in[1], 13, 1) ? " int" : "");
                    else
                        snprintf(line, sizeof(line), "    %3u tfetch op%u r%u, r%u.%c%c%c, tf%u dim %u dswz %03X%s\n", ia, fop, dst,
                                 src, "xyzw"[Bits(in[0], 26, 2)], "xyzw"[Bits(in[0], 28, 2)], "xyzw"[Bits(in[0], 30, 2)],
                                 cidx, Bits(in[2], 14, 2), Bits(in[1], 0, 12), Bits(in[0], 25, 1) ? " unnorm" : "");
                    out += line;
                }
                else
                {
                    uint32_t vop = Bits(in[2], 24, 5), sop = Bits(in[0], 26, 6);
                    bool exp = Bits(in[0], 15, 1);
                    auto src = [&](int n) {
                        uint32_t reg = Bits(in[2], 16 - 8 * (n - 1), 8);
                        bool temp = Bits(in[2], 31 - (n - 1), 1);
                        uint32_t swz = Bits(in[1], 16 - 8 * (n - 1), 8);
                        bool neg = Bits(in[1], 26 - (n - 1), 1);
                        char b[48];
                        if (temp) snprintf(b, sizeof(b), "%s%sr%u%s%s", neg ? "-" : "", (reg & 0x80) ? "|" : "", reg & 0x3F,
                                           (reg & 0x40) ? "[aL]" : "", SwizzleText(swz).c_str());
                        else snprintf(b, sizeof(b), "%sc%u%s", neg ? "-" : "", reg, SwizzleText(swz).c_str());
                        return std::string(b);
                    };
                    uint32_t vm = Bits(in[0], 16, 4), sm = Bits(in[0], 20, 4);
                    std::string dst = exp ? "export" + std::to_string(Bits(in[0], 0, 6)) : "r" + std::to_string(Bits(in[0], 0, 6));
                    snprintf(line, sizeof(line), "    %3u %s%s %s.%x", ia, Bits(in[1], 28, 1) ? (Bits(in[1], 27, 1) ? "(p) " : "(!p) ") : "",
                             kVecNames[vop], dst.c_str(), vm);
                    out += line;
                    for (int n = 1; n <= 3; ++n) if (kVecOperands[vop][n - 1]) out += ", " + src(n);
                    if (sm || sop != 50)
                    {
                        snprintf(line, sizeof(line), "  +  %s %s%u.%x", kScaNames[sop] ? kScaNames[sop] : "?",
                                 exp ? "export" : "r", exp ? Bits(in[0], 0, 6) : Bits(in[0], 8, 6), sm);
                        out += line;
                        if (kScaOperands[sop]) out += ", " + src(3);
                    }
                    out += "\n";
                }
            }
            if (CfEnds(op)) break;
        }
        else
        {
            if (op == 12) { snprintf(line, sizeof(line), " type %u size %u", Bits(cf.d1, 9, 2), Bits(cf.d0, 0, 3)); out += line; }
            else if (op == 7 || op == 8 || op == 9 || op == 11) { snprintf(line, sizeof(line), " addr %u", Bits(cf.d0, 0, 13)); out += line; }
            out += "\n";
        }
    }
    return out;
}

// Number of control-flow instructions: they occupy the start of the program up
// to the first ALU/fetch instruction any exec block points at (scanning further
// would decode instruction words as CF), and end at an exec with the end flag.
uint32_t CfCount(const std::vector<uint32_t>& dw)
{
    uint32_t limit = 2 * uint32_t(dw.size() / 3);
    for (uint32_t cfi = 0; cfi < limit; ++cfi)
    {
        Cf cf = GetCf(dw, cfi);
        if (CfIsExec(cf.op()))
        {
            limit = std::min(limit, 2 * Bits(cf.d0, 0, 12));
            if (CfEnds(cf.op())) return std::min(limit, cfi + 1);
        }
    }
    return limit;
}

bool ShaderHasTextureFetch(const ShaderCode& code)
{
    const auto& dw = code.dw;
    for (uint32_t cfi = 0, n = CfCount(dw); cfi < n; ++cfi)
    {
        Cf cf = GetCf(dw, cfi);
        if (!CfIsExec(cf.op())) continue;
        uint32_t addr = Bits(cf.d0, 0, 12), count = Bits(cf.d0, 12, 3), seq = Bits(cf.d0, 16, 12);
        for (uint32_t k = 0; k < count; ++k)
            if (((seq >> (2 * k)) & 1) && 3 * (addr + k) + 2 < dw.size() && Bits(dw[3 * (addr + k)], 0, 5) == 1) return true;
    }
    return false;
}

bool ShaderUsesMemExport(const ShaderCode& code)
{
    const auto& dw = code.dw;
    for (uint32_t cfi = 0, n = CfCount(dw); cfi < n; ++cfi)
    {
        Cf cf = GetCf(dw, cfi);
        if (cf.op() == 12 && Bits(cf.d1, 9, 2) == 3) return true;
    }
    return false;
}

// ---- constants -------------------------------------------------------------------
void ShaderConstants::Snapshot()
{
    static_assert(sizeof(alu) == 512 * 4 * 4, "alu layout");
    RegRange(reg::ALU_CONSTANTS, 512 * 4, reinterpret_cast<uint32_t*>(alu));
    RegRange(reg::BOOL_CONSTANTS, 8, bools);
    RegRange(reg::LOOP_CONSTANTS, 32, loops);
    uint32_t vs = Reg(0x2307), ps = Reg(0x2308);
    vsBase = vs & 0x1FF; vsSize = ((vs >> 12) & 0x1FF) + 1;
    psBase = ps & 0x1FF; psSize = ((ps >> 12) & 0x1FF) + 1;
}

// ---- interpreter -------------------------------------------------------------------
int32_t ShaderInterpreter::LoopAddress() const
{
    if (loopDepth_ <= 0 || loopDepth_ > 4) return 0;
    uint32_t lc = loopConst_[loopDepth_ - 1];
    int32_t v = int32_t(loopIter_[loopDepth_ - 1]) * SBits(lc, 16, 8) + int32_t(Bits(lc, 8, 8));
    return std::min(256, std::max(-256, v));
}

float* ShaderInterpreter::Temp(uint32_t index, bool relative)
{
    return r[(int32_t(index) + (relative ? LoopAddress() : 0)) & (kTemps - 1)];
}

const float* ShaderInterpreter::Constant(uint32_t index, bool relative, bool useA0, float tmp[4]) const
{
    int32_t i = int32_t(index) + (relative ? (useA0 ? a0_ : LoopAddress()) : 0);
    uint32_t base = type_ == ShaderType::kVertex ? c_.vsBase : c_.psBase;
    uint32_t size = type_ == ShaderType::kVertex ? c_.vsSize : c_.psSize;
    if (i < 0 || uint32_t(i) >= size || base + uint32_t(i) >= 512) { tmp[0] = tmp[1] = tmp[2] = tmp[3] = 0; return tmp; }
    if (trace && relative)
    {
        char b[120];
        const float* c = c_.alu[base + i];
        snprintf(b, sizeof(b), "      c%d (relative %s) = (%g %g %g %g)\n", i, useA0 ? "a0" : "aL", c[0], c[1], c[2], c[3]);
        *trace += b;
    }
    return c_.alu[base + i];
}

void ShaderInterpreter::StoreFetch(uint32_t dest, bool rel, uint32_t swz, const float v[4])
{
    float* d = Temp(dest, rel);
    for (uint32_t i = 0; i < 4; ++i)
    {
        uint32_t s = (swz >> (3 * i)) & 7;
        if (s <= 3) d[i] = v[s];
        else if (s == 4) d[i] = 0.0f;
        else if (s == 5) d[i] = 1.0f;
        // 7 = keep
    }
}

void ShaderInterpreter::Fetch(const uint32_t* in)
{
    uint32_t op = Bits(in[0], 0, 5);
    uint32_t dst = Bits(in[0], 12, 6), src = Bits(in[0], 5, 6), dswz = Bits(in[1], 0, 12);
    bool dstRel = Bits(in[0], 18, 1), srcRel = Bits(in[0], 11, 1);
    if (Bits(in[1], 31, 1) && Bits(in[2], 31, 1) != uint32_t(predicate_)) return;   // predicated
    float result[4] = { 0, 0, 0, 0 };
    if (op == 0)   // vertex fetch
    {
        bool mini = Bits(in[1], 30, 1);
        if (!mini)
        {
            vfetchConst_ = Bits(in[0], 20, 5) * 3 + Bits(in[0], 25, 2);
            vfetchStride_ = Bits(in[2], 0, 8);
            float idxF = Temp(src, srcRel)[Bits(in[0], 30, 2)] + (Bits(in[1], 15, 1) ? 0.5f : 0.0f);
            uint32_t d0 = Reg(reg::FETCH_CONSTANTS + vfetchConst_ * 2);
            vfetchAddressDw_ = vfetchStride_ * uint32_t(int32_t(std::floor(idxF))) + (d0 >> 2);
        }
        uint32_t d0 = Reg(reg::FETCH_CONSTANTS + vfetchConst_ * 2), d1 = Reg(reg::FETCH_CONSTANTS + vfetchConst_ * 2 + 1);
        uint32_t bufStart = d0 >> 2, bufEnd = bufStart + Bits(d1, 2, 24), endian = d1 & 3;
        uint32_t fmt = Bits(in[1], 16, 6);
        bool isSigned = Bits(in[1], 12, 1), normalized = !Bits(in[1], 13, 1), noZero = Bits(in[1], 14, 1) == 1;
        uint32_t data[4] = {};
        uint32_t a = uint32_t(int32_t(vfetchAddressDw_) + SBits(in[2], 8, 23));
        for (uint32_t k = 0; k < 4; ++k)
        {
            uint32_t ad = a + k;
            if (ad >= bufStart && ad < bufEnd)
            {
                // The GPU loads the dword little-endian, then applies the
                // fetch constant's swap: 0 none, 1 8in16, 2 8in32, 3 16in32.
                uint32_t raw;
                memcpy(&raw, Phys(ad * 4), 4);
                switch (endian)
                {
                case 1: data[k] = ((raw & 0x00FF00FF) << 8) | ((raw & 0xFF00FF00) >> 8); break;
                case 2: data[k] = Bswap32(raw); break;
                case 3: data[k] = (raw << 16) | (raw >> 16); break;
                default: data[k] = raw; break;
                }
            }
        }
        uint32_t widths[4] = {}, offsets[4] = {}, packed = 0, words[2] = { data[0], data[0] };
        switch (fmt)
        {
        case 6:  packed = 15; widths[0] = widths[1] = widths[2] = widths[3] = 8; offsets[1] = 8; offsets[2] = 16; offsets[3] = 24; break;
        case 7:  packed = 15; widths[0] = widths[1] = widths[2] = 10; widths[3] = 2; offsets[1] = 10; offsets[2] = 20; offsets[3] = 30; break;
        case 16: packed = 7; widths[0] = widths[1] = 11; widths[2] = 10; offsets[1] = 11; offsets[2] = 22; break;
        case 17: packed = 7; widths[0] = 10; widths[1] = widths[2] = 11; offsets[1] = 10; offsets[2] = 21; break;
        case 25: packed = 3; widths[0] = widths[1] = 16; offsets[1] = 16; break;
        case 26: packed = 15; widths[0] = widths[1] = widths[2] = widths[3] = 16; offsets[1] = offsets[3] = 16; words[1] = data[1]; break;
        case 32: result[3] = XenosHalf(uint16_t(data[1] >> 16)); result[2] = XenosHalf(uint16_t(data[1])); [[fallthrough]];
        case 31: result[1] = XenosHalf(uint16_t(data[0] >> 16)); result[0] = XenosHalf(uint16_t(data[0])); break;
        case 33: case 34: case 35:
            for (int k = 0; k < 4; ++k)
            {
                if (isSigned) { result[k] = float(int32_t(data[k])); if (normalized) result[k] = noZero ? (result[k] + 0.5f) / 2147483647.5f : result[k] / 2147483647.0f; }
                else { result[k] = float(data[k]); if (normalized) result[k] /= 4294967295.0f; }
            }
            break;
        case 36: case 37: case 38: case 57: for (int k = 0; k < 4; ++k) result[k] = AsF(data[k]); break;
        default: break;
        }
        for (uint32_t k = 0; k < 4; ++k)
        {
            if (!(packed & (1u << k))) continue;
            uint32_t w = widths[k], word = words[k >> 1];
            if (isSigned)
            {
                result[k] = float(int32_t(word << (32 - (w + offsets[k]))) >> (32 - w));
                if (normalized)
                    result[k] = noZero ? (result[k] + 0.5f) * 2.0f / float((1u << w) - 1)
                                       : std::max(-1.0f, result[k] / float((1u << (w - 1)) - 1));
            }
            else
            {
                result[k] = float((word >> offsets[k]) & ((1u << w) - 1));
                if (normalized) result[k] /= float((1u << w) - 1);
            }
        }
        int32_t expAdj = SBits(in[1], 24, 6);
        if (expAdj) for (float& v : result) v = std::ldexp(v, expAdj);
        StoreFetch(dst, dstRel, dswz, result);
        return;
    }
    if (op >= 24 && op <= 26)   // setTexLOD, setGradientsH, setGradientsV: register state, no result
    {
        const float* s = Temp(src, srcRel);
        uint32_t sswz = Bits(in[0], 26, 6);
        float v[3] = { s[sswz & 3], s[(sswz >> 2) & 3], s[(sswz >> 4) & 3] };
        if (op == 24) regLod_ = v[0];
        else memcpy(op == 25 ? gradH_ : gradV_, v, sizeof(v));
        if (trace)
        {
            char b[120];
            snprintf(b, sizeof(b), "%3u fetch op%u (%g %g %g)\n", addr_, op, v[0], v[1], v[2]);
            *trace += b;
        }
        return;
    }
    if (op == 18 && sampler_)   // getGradients: quad derivatives of the source's first two components
    {
        const float* s = Temp(src, srcRel);
        uint32_t sswz = Bits(in[0], 26, 6);
        sampler_->Gradients(gradSeq_++, s[sswz & 3], s[(sswz >> 2) & 3], result);
    }
    if (op == 1 && sampler_)   // texture fetch
    {
        const float* s = Temp(src, srcRel);
        uint32_t sswz = Bits(in[0], 26, 6);
        TextureFetchRequest r;
        r.slot = Bits(in[0], 20, 5);
        r.coord[0] = s[sswz & 3]; r.coord[1] = s[(sswz >> 2) & 3]; r.coord[2] = s[(sswz >> 4) & 3];
        memcpy(traceCoord_, r.coord, sizeof(traceCoord_));
        r.dimension = Bits(in[2], 14, 2);
        r.unnormalized = Bits(in[0], 25, 1);
        r.offsetX = SBits(in[2], 16, 5); r.offsetY = SBits(in[2], 21, 5);
        r.magFilter = Bits(in[1], 12, 2); r.minFilter = Bits(in[1], 14, 2); r.mipFilter = Bits(in[1], 16, 2);
        r.volMagFilter = Bits(in[1], 24, 2);
        r.useComputedLod = Bits(in[1], 28, 1);
        r.useRegisterLod = Bits(in[1], 29, 1);
        r.useRegisterGradients = Bits(in[2], 0, 1);
        r.registerLod = regLod_;
        memcpy(r.gradH, gradH_, sizeof(gradH_));
        memcpy(r.gradV, gradV_, sizeof(gradV_));
        r.lodBias = float(SBits(in[2], 2, 7)) / 16.0f;
        r.sequence = fetchSeq_++;
        sampler_->Sample(r, result);
    }
    StoreFetch(dst, dstRel, dswz, result);   // getCompTexLOD / getWeights / getBCF: zeros
    if (trace)
    {
        char b[200];
        const float* d = Temp(dst, dstRel);
        snprintf(b, sizeof(b), "%3u fetch op%u -> r%u (%g %g %g %g) at (%g %g %g)\n", addr_, op, dst, d[0], d[1], d[2], d[3],
                 traceCoord_[0], traceCoord_[1], traceCoord_[2]);
        *trace += b;
    }
}

void ShaderInterpreter::Alu(const uint32_t* in)
{
    if (Bits(in[1], 28, 1) && Bits(in[1], 27, 1) != uint32_t(predicate_))   // predicated
    {
        if (trace) { char b[48]; snprintf(b, sizeof(b), "%3u (skipped: predicate)\n", addr_); *trace += b; }
        return;
    }
    uint32_t vop = Bits(in[2], 24, 5), sop = Bits(in[0], 26, 6);
    bool exportData = Bits(in[0], 15, 1);
    uint32_t vmask = Bits(in[0], 16, 4), smask = Bits(in[0], 20, 4);
    uint32_t vResMask = exportData ? (vmask & ~smask) : vmask;
    uint32_t sResMask = exportData ? (smask & ~vmask) : smask;
    bool absConsts = Bits(in[0], 7, 1);
    bool constRelA0 = Bits(in[1], 29, 1);
    bool c0Rel = Bits(in[1], 31, 1), c1Rel = Bits(in[1], 30, 1);
    auto srcIsTemp = [&](int n) { return Bits(in[2], 31 - (n - 1), 1) != 0; };
    auto srcReg = [&](int n) { return Bits(in[2], 16 - 8 * (n - 1), 8); };
    auto srcSwz = [&](int n) { return Bits(in[1], 16 - 8 * (n - 1), 8); };
    auto srcNeg = [&](int n) { return Bits(in[1], 26 - (n - 1), 1) != 0; };
    auto constRel = [&](int n) {
        if (n == 1) return c0Rel;
        if (n == 2) return srcIsTemp(1) ? c0Rel : c1Rel;
        return (srcIsTemp(1) && srcIsTemp(2)) ? c0Rel : c1Rel;
    };

    float vres[4] = { 0, 0, 0, 0 };
    if (vResMask || (vop >= 20 && vop <= 27) || vop == 29)
    {
        float ops[3][4] = {};
        for (int n = 1; n <= 3; ++n)
        {
            if (!kVecOperands[vop][n - 1]) continue;
            float tmp[4];
            const float* p;
            bool absVal;
            uint32_t reg = srcReg(n);
            if (srcIsTemp(n)) { p = Temp(reg & 0x3F, reg & 0x40); absVal = reg & 0x80; }
            else { p = Constant(reg, constRel(n), constRelA0, tmp); absVal = absConsts; }
            uint32_t swz = srcSwz(n);
            for (uint32_t j = 0; j < 4; ++j)
            {
                uint32_t u = AsU(FlushDenormal(p[SwizzledComponent(swz, j)]));
                if (absVal) u &= 0x7FFFFFFF;
                if (srcNeg(n)) u ^= 0x80000000;
                ops[n - 1][j] = AsF(u);
            }
        }
        auto mul = [](float a, float b) { return (a != 0.0f && b != 0.0f) ? a * b : 0.0f; };   // 0 * inf = 0
        bool rep = false;
        float (*o)[4] = ops;
        switch (vop)
        {
        case 0: for (int i = 0; i < 4; ++i) vres[i] = o[0][i] + o[1][i]; break;
        case 1: for (int i = 0; i < 4; ++i) vres[i] = mul(o[0][i], o[1][i]); break;
        case 2: for (int i = 0; i < 4; ++i) vres[i] = std::isgreaterequal(o[0][i], o[1][i]) ? o[0][i] : o[1][i]; break;
        case 3: for (int i = 0; i < 4; ++i) vres[i] = std::isless(o[0][i], o[1][i]) ? o[0][i] : o[1][i]; break;
        case 4: for (int i = 0; i < 4; ++i) vres[i] = float(o[0][i] == o[1][i]); break;
        case 5: for (int i = 0; i < 4; ++i) vres[i] = float(std::isgreater(o[0][i], o[1][i])); break;
        case 6: for (int i = 0; i < 4; ++i) vres[i] = float(std::isgreaterequal(o[0][i], o[1][i])); break;
        case 7: for (int i = 0; i < 4; ++i) vres[i] = float(o[0][i] != o[1][i]); break;
        case 8: for (int i = 0; i < 4; ++i) vres[i] = o[0][i] - std::floor(o[0][i]); break;
        case 9: for (int i = 0; i < 4; ++i) vres[i] = std::trunc(o[0][i]); break;
        case 10: for (int i = 0; i < 4; ++i) vres[i] = std::floor(o[0][i]); break;
        case 11: for (int i = 0; i < 4; ++i) vres[i] = mul(o[0][i], o[1][i]) + o[2][i]; break;
        case 12: for (int i = 0; i < 4; ++i) vres[i] = o[0][i] == 0.0f ? o[1][i] : o[2][i]; break;
        case 13: for (int i = 0; i < 4; ++i) vres[i] = std::isgreaterequal(o[0][i], 0.0f) ? o[1][i] : o[2][i]; break;
        case 14: for (int i = 0; i < 4; ++i) vres[i] = std::isgreater(o[0][i], 0.0f) ? o[1][i] : o[2][i]; break;
        case 15: vres[0] = 0; for (int i = 0; i < 4; ++i) vres[0] += mul(o[0][i], o[1][i]); rep = true; break;
        case 16: vres[0] = 0; for (int i = 0; i < 3; ++i) vres[0] += mul(o[0][i], o[1][i]); rep = true; break;
        case 17: vres[0] = mul(o[0][0], o[1][0]) + mul(o[0][1], o[1][1]) + o[2][0]; rep = true; break;
        case 18:
        {
            float x = o[0][2], y = o[0][3], z = o[0][0];
            float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
            if (az >= ax && az >= ay) { bool n = z < 0; vres[0] = -y; vres[1] = n ? -x : x; vres[2] = z; vres[3] = n ? 5.0f : 4.0f; }
            else if (ay >= ax) { bool n = y < 0; vres[0] = n ? -z : z; vres[1] = x; vres[2] = y; vres[3] = n ? 3.0f : 2.0f; }
            else { bool n = x < 0; vres[0] = -y; vres[1] = n ? z : -z; vres[2] = x; vres[3] = n ? 1.0f : 0.0f; }
            vres[2] *= 2.0f;
            break;
        }
        case 19:
        {
            float m = o[0][0];
            for (int i = 1; i < 4; ++i) if (!std::isgreaterequal(m, o[0][i])) m = o[0][i];
            vres[0] = m; rep = true; break;
        }
        case 20: predicate_ = o[0][3] == 0.0f && o[1][3] == 0.0f; vres[0] = (o[0][0] == 0.0f && o[1][0] == 0.0f) ? 0.0f : o[0][0] + 1.0f; rep = true; break;
        case 21: predicate_ = o[0][3] == 0.0f && o[1][3] != 0.0f; vres[0] = (o[0][0] == 0.0f && o[1][0] != 0.0f) ? 0.0f : o[0][0] + 1.0f; rep = true; break;
        case 22: predicate_ = o[0][3] == 0.0f && std::isgreater(o[1][3], 0.0f); vres[0] = (o[0][0] == 0.0f && std::isgreater(o[1][0], 0.0f)) ? 0.0f : o[0][0] + 1.0f; rep = true; break;
        case 23: predicate_ = o[0][3] == 0.0f && std::isgreaterequal(o[1][3], 0.0f); vres[0] = (o[0][0] == 0.0f && std::isgreaterequal(o[1][0], 0.0f)) ? 0.0f : o[0][0] + 1.0f; rep = true; break;
        case 24: case 25: case 26: case 27:
        {
            bool k = false;
            for (int i = 0; i < 4; ++i)
                k |= vop == 24 ? o[0][i] == o[1][i] : vop == 25 ? std::isgreater(o[0][i], o[1][i])
                   : vop == 26 ? std::isgreaterequal(o[0][i], o[1][i]) : o[0][i] != o[1][i];
            vres[0] = float(k); rep = true;
            if (k) killed = true;
            break;
        }
        case 28: vres[0] = 1.0f; vres[1] = mul(o[0][1], o[1][1]); vres[2] = o[0][2]; vres[3] = o[1][3]; break;
        case 29:
            a0_ = int32_t(std::floor(std::min(255.0f, std::max(-256.0f, o[0][3])) + 0.5f));
            for (int i = 0; i < 4; ++i) vres[i] = std::isgreaterequal(o[0][i], o[1][i]) ? o[0][i] : o[1][i];
            break;
        default: break;
        }
        if (rep) vres[1] = vres[2] = vres[3] = vres[0];
    }

    // Scalar operands: one register (component w, plus x for two-component ops)
    // or, for the *sc0/sc1 forms, a constant (w) and a temp (x).
    float sops[2] = { 0, 0 };
    uint32_t nops = kScaOperands[sop < 64 ? sop : 0];
    if (sop >= 51) nops = 0;
    if (nops == 1)
    {
        float tmp[4];
        const float* p;
        bool absVal;
        uint32_t reg = srcReg(3);
        if (srcIsTemp(3)) { p = Temp(reg & 0x3F, reg & 0x40); absVal = reg & 0x80; }
        else { p = Constant(reg, constRel(3), constRelA0, tmp); absVal = absConsts; }
        uint32_t count = kScaTwoComponent[sop] ? 2 : 1;
        for (uint32_t k = 0; k < count; ++k)
        {
            uint32_t u = AsU(FlushDenormal(p[SwizzledComponent(srcSwz(3), (3 + k) & 3)]));
            if (absVal) u &= 0x7FFFFFFF;
            if (srcNeg(3)) u ^= 0x80000000;
            sops[k] = AsF(u);
        }
    }
    else if (nops == 2)
    {
        float tmp[4];
        const float* cp = Constant(srcReg(3), constRel(3), constRelA0, tmp);
        uint32_t tempReg = (sop & 1) | (Bits(in[2], 31 - 2, 1) << 1) | (srcSwz(3) & 0x3C);
        const float* tp = Temp(tempReg, false);
        sops[0] = FlushDenormal(cp[SwizzledComponent(srcSwz(3), 3)]);
        sops[1] = FlushDenormal(tp[SwizzledComponent(srcSwz(3), 0)]);
        for (float& v : sops)
        {
            uint32_t u = AsU(v);
            if (absConsts) u &= 0x7FFFFFFF;   // the constant/temp pair shares the abs/negate flags
            if (srcNeg(3)) u ^= 0x80000000;
            v = AsF(u);
        }
    }
    float& ps = prevScalar_;
    auto smul = [](float a, float b) { return (a != 0.0f && b != 0.0f) ? a * b : 0.0f; };
    switch (sop)
    {
    case 0: case 44: case 45: ps = sops[0] + sops[1]; break;
    case 1: ps = sops[0] + ps; break;
    case 2: case 42: case 43: ps = smul(sops[0], sops[1]); break;
    case 3: ps = smul(sops[0], ps); break;
    case 4: ps = (ps == -FLT_MAX || !std::isfinite(ps) || !std::isfinite(sops[1]) || sops[1] <= 0.0f) ? -FLT_MAX : smul(sops[0], ps); break;
    case 5: ps = std::isgreaterequal(sops[0], sops[1]) ? sops[0] : sops[1]; break;
    case 6: ps = std::isless(sops[0], sops[1]) ? sops[0] : sops[1]; break;
    case 7: ps = float(sops[0] == 0.0f); break;
    case 8: ps = float(std::isgreater(sops[0], 0.0f)); break;
    case 9: ps = float(std::isgreaterequal(sops[0], 0.0f)); break;
    case 10: ps = float(sops[0] != 0.0f); break;
    case 11: ps = sops[0] - std::floor(sops[0]); break;
    case 12: ps = std::trunc(sops[0]); break;
    case 13: ps = std::floor(sops[0]); break;
    case 14: ps = Exp2(sops[0]); break;
    case 15: ps = Log2(sops[0]); if (ps == -INFINITY) ps = -FLT_MAX; break;
    case 16: ps = Log2(sops[0]); break;
    case 17: ps = 1.0f / sops[0]; if (ps == -INFINITY) ps = -FLT_MAX; else if (ps == INFINITY) ps = FLT_MAX; break;
    case 18: ps = 1.0f / sops[0]; if (ps == -INFINITY) ps = -0.0f; else if (ps == INFINITY) ps = 0.0f; break;
    case 19: ps = 1.0f / sops[0]; break;
    case 20: ps = 1.0f / std::sqrt(sops[0]); if (ps == -INFINITY) ps = -FLT_MAX; else if (ps == INFINITY) ps = FLT_MAX; break;
    case 21: ps = 1.0f / std::sqrt(sops[0]); if (ps == -INFINITY) ps = -0.0f; else if (ps == INFINITY) ps = 0.0f; break;
    case 22: ps = 1.0f / std::sqrt(sops[0]); break;
    case 23: a0_ = int32_t(std::floor(std::min(255.0f, std::max(-256.0f, sops[0])) + 0.5f)); ps = std::isgreaterequal(sops[0], sops[1]) ? sops[0] : sops[1]; break;
    case 24: a0_ = int32_t(std::floor(std::min(255.0f, std::max(-256.0f, sops[0])))); ps = std::isgreaterequal(sops[0], sops[1]) ? sops[0] : sops[1]; break;
    case 25: case 46: case 47: ps = sops[0] - sops[1]; break;
    case 26: ps = sops[0] - ps; break;
    case 27: predicate_ = sops[0] == 0.0f; ps = float(!predicate_); break;
    case 28: predicate_ = sops[0] != 0.0f; ps = float(!predicate_); break;
    case 29: predicate_ = std::isgreater(sops[0], 0.0f); ps = float(!predicate_); break;
    case 30: predicate_ = std::isgreaterequal(sops[0], 0.0f); ps = float(!predicate_); break;
    case 31: predicate_ = sops[0] == 1.0f; ps = predicate_ ? 0.0f : (sops[0] == 0.0f ? 1.0f : sops[0]); break;
    case 32: { float n = sops[0] - 1.0f; predicate_ = n <= 0.0f; ps = predicate_ ? 0.0f : n; break; }
    case 33: predicate_ = false; ps = FLT_MAX; break;
    case 34: predicate_ = sops[0] == 0.0f; ps = predicate_ ? 0.0f : sops[0]; break;
    case 35: ps = float(sops[0] == 0.0f); if (ps != 0.0f) killed = true; break;
    case 36: ps = float(std::isgreater(sops[0], 0.0f)); if (ps != 0.0f) killed = true; break;
    case 37: ps = float(std::isgreaterequal(sops[0], 0.0f)); if (ps != 0.0f) killed = true; break;
    case 38: ps = float(sops[0] != 0.0f); if (ps != 0.0f) killed = true; break;
    case 39: ps = float(sops[0] == 1.0f); if (ps != 0.0f) killed = true; break;
    case 40: ps = std::sqrt(sops[0]); break;
    case 48: ps = Sin(sops[0]); break;
    case 49: ps = Cos(sops[0]); break;
    default: break;   // retain_prev
    }

    if (Bits(in[0], 24, 1)) for (float& v : vres) v = Saturate(v);
    float sres = Bits(in[0], 25, 1) ? Saturate(ps) : ps;
    if (trace)
    {
        char b[200];
        snprintf(b, sizeof(b), "%3u vec%u (%g %g %g %g) mask %X | sca%u %g mask %X | p %d a0 %d%s\n", addr_, vop, vres[0], vres[1],
                 vres[2], vres[3], vResMask, sop, sres, sResMask, int(predicate_), a0_, exportData ? " export" : "");
        *trace += b;
    }
    if (exportData)
    {
        uint32_t c0Mask = Bits(in[0], 14, 1) ? (15 & ~(vmask | smask)) : 0;   // scalar_dest_rel in export = write 0s
        uint32_t c1Mask = vmask & smask;
        float v[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            uint32_t b = 1u << i;
            v[i] = (vResMask & b) ? vres[i] : (sResMask & b) ? sres : (c1Mask & b) ? 1.0f : 0.0f;
        }
        uint32_t mask = vResMask | sResMask | c0Mask | c1Mask;
        if (mask) sink_.Export(Bits(in[0], 0, 6), v, mask);
        return;
    }
    if (vResMask)
    {
        float* d = Temp(Bits(in[0], 0, 6), Bits(in[0], 6, 1));
        for (int i = 0; i < 4; ++i) if (vResMask & (1u << i)) d[i] = vres[i];
    }
    if (sResMask)
    {
        float* d = Temp(Bits(in[0], 8, 6), Bits(in[0], 14, 1));
        for (int i = 0; i < 4; ++i) if (sResMask & (1u << i)) d[i] = sres;
    }
}

void ShaderInterpreter::Run(const ShaderCode& code)
{
    type_ = code.type;
    prevScalar_ = 0; predicate_ = false; a0_ = 0; loopDepth_ = 0; killed = false; fetchSeq_ = 0; gradSeq_ = 0;
    regLod_ = 0.0f; for (int i = 0; i < 3; ++i) gradH_[i] = gradV_[i] = 0.0f;
    uint32_t callStack[4]; int callDepth = 0;
    const auto& dw = code.dw;
    uint32_t cfCount = 2 * uint32_t(dw.size() / 3);
    auto boolBit = [&](uint32_t addr) { return (c_.bools[(addr >> 5) & 7] >> (addr & 31)) & 1; };
    uint32_t guard = 0;
    for (uint32_t cfi = 0; cfi < cfCount && guard < 100000; ++guard)
    {
        Cf cf = GetCf(dw, cfi);
        uint32_t op = cf.op(), next = cfi + 1;
        if (CfIsExec(op))
        {
            bool run = true;
            if (op == 3 || op == 4 || op == 13 || op == 14) run = Bits(cf.d1, 10, 1) == boolBit(Bits(cf.d1, 2, 8));
            if (op == 5 || op == 6) run = Bits(cf.d1, 10, 1) == uint32_t(predicate_);
            if (run)
            {
                uint32_t addr = Bits(cf.d0, 0, 12), count = Bits(cf.d0, 12, 3), seq = Bits(cf.d0, 16, 12);
                for (uint32_t k = 0; k < count; ++k)
                {
                    uint32_t ia = addr + k;
                    if (3 * ia + 2 >= dw.size()) break;
                    addr_ = ia;
                    if ((seq >> (2 * k)) & 1) Fetch(&dw[3 * ia]); else Alu(&dw[3 * ia]);
                }
            }
            if (CfEnds(op)) break;
        }
        else switch (op)
        {
        case 7:   // loop start
        {
            uint32_t lc = c_.loops[Bits(cf.d0, 16, 5)];
            if (loopDepth_ >= 4) { next = Bits(cf.d0, 0, 13); break; }
            if (!Bits(cf.d0, 13, 1)) loopIter_[loopDepth_] = 0;
            if (loopIter_[loopDepth_] >= Bits(lc, 0, 8)) { next = Bits(cf.d0, 0, 13); break; }
            loopConst_[loopDepth_] = lc;
            ++loopDepth_;
            break;
        }
        case 8:   // loop end
        {
            if (loopDepth_ <= 0) break;
            uint32_t it = ++loopIter_[loopDepth_ - 1];
            bool predBreak = Bits(cf.d0, 21, 1);
            if (it < Bits(loopConst_[loopDepth_ - 1], 0, 8) && (!predBreak || Bits(cf.d1, 10, 1) != uint32_t(predicate_)))
                next = Bits(cf.d0, 0, 13);
            else --loopDepth_;
            break;
        }
        case 9: case 11:   // conditional call / jump
        {
            bool take = Bits(cf.d0, 13, 1);
            if (!take) take = Bits(cf.d0, 14, 1) ? Bits(cf.d1, 10, 1) == uint32_t(predicate_)
                                                  : Bits(cf.d1, 10, 1) == boolBit(Bits(cf.d1, 2, 8));
            if (!take) break;
            if (op == 9) { if (callDepth >= 4) break; callStack[callDepth++] = cfi + 1; }
            next = Bits(cf.d0, 0, 13);
            break;
        }
        case 10: if (callDepth) next = callStack[--callDepth]; break;
        default: break;   // nop, alloc, mark_vs_fetch_done
        }
        cfi = next;
    }
}

}  // namespace xenos
