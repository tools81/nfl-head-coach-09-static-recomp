#include "gpu/hlsl_translator.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "gpu/ucode.h"

namespace hc09::gpu
{
    namespace
    {
        using namespace ucode;

        const char kComponents[] = "xyzw";

        std::string Format(const char* format, ...)
        {
            char buffer[512];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        std::string MaskSuffix(uint32_t mask)
        {
            std::string text = ".";
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i))
                    text.push_back(kComponents[i]);
            return text;
        }

        // Shared HLSL: resources, constants, and helpers used by every translated shader.
        const char* const kPrelude = R"(// Generated from Xenos microcode by hc09 hlsl_translator.
cbuffer FloatConstants : register(b0)
{
    float4 c[512];
};

cbuffer DrawState : register(b1)
{
    uint vsConstBase;
    uint psConstBase;
    uint wIsReciprocal;   // PA_CL_VTE_CNTL.vtx_w0_fmt clear: oPos.w already holds 1/W
    uint xyPreDivided;    // PA_CL_VTE_CNTL.vtx_xy_fmt: oPos.xy already divided by W
    float4 ndcScale;      // xyz: screen-to-NDC scale; w: 1 when oPos.z is already divided
    float4 ndcOffset;
    uint4 boolConstants[2];
    uint4 loopConstants[8];
    float4 textureSizes[32];
    uint alphaFunc;       // RB_COLORCONTROL alpha test function (7 = always / disabled)
    float alphaRef;       // RB_ALPHA_REF
    uint vertexIndexOffset;  // VGT_INDX_OFFSET, added to every vertex index
    uint4 vertexStreams[4];  // per vertex stream: bits 0-1 the endian swap (0 when already swapped),
                             // bits 2-3 the byte offset of its data in the view (views start 16-byte aligned)
};

static const float kFltMax = 3.402823466e+38;

bool BoolConstant(uint index)
{
    return ((boolConstants[index >> 7][(index >> 5) & 3] >> (index & 31)) & 1) != 0;
}

uint LoopConstant(uint index)
{
    return loopConstants[index >> 2][index & 3];
}

float SignExtendNorm(uint value, uint bits)
{
    int v = int(value << (32 - bits)) >> (32 - bits);
    return max(float(v) / float((1u << (bits - 1)) - 1), -1.0);
}

float SignExtend(uint value, uint bits)
{
    return float(int(value << (32 - bits)) >> (32 - bits));
}

// Xenos endian modes: 1 swaps the bytes of each 16-bit half, 2 the bytes of each word, 3 the halves.
// Written with shift amounts that depend on the mode (0 leaves a value as it is): DXC turns a
// constant byte reversal into llvm.bswap, which DXIL does not allow.
uint4 SwapWords(uint4 v, uint mode)
{
    uint bytes = (mode == 1 || mode == 2) ? 8u : 0u;
    uint mask = bytes != 0 ? 0x00FF00FFu : 0xFFFFFFFFu;
    v = ((v & mask) << bytes) | ((v >> bytes) & mask);
    uint halves = mode >= 2 ? 16u : 0u;
    return (v << halves) | (v >> halves);
}

uint StreamEndian(uint stream)
{
    return vertexStreams[stream >> 2][stream & 3] & 3;
}

uint StreamOffset(uint stream)
{
    return vertexStreams[stream >> 2][stream & 3] & 12;
}
)";

        class Translator
        {
        public:
            explicit Translator(const ShaderMicrocode& shader)
                : dwords_(shader.dwords), pixel_(shader.type == ShaderType::Pixel) {}

            TranslatedShader Run()
            {
                program_ = DecodeControlFlow(dwords_);
                bool ended = false;
                for (const auto& cf : program_)
                    ended = ended || cf.IsEnd();
                if (!ended)
                    return Fail("control flow has no exec_end");

                if (IsStraightLine())
                    EmitStraightLine();
                else
                    EmitStateMachine();
                if (!result_.error.empty())
                    return result_;

                result_.hlsl = kPrelude + Declarations() + (pixel_ ? PixelMain() : VertexMain());
                result_.valid = true;
                return result_;
            }

        private:
            TranslatedShader Fail(const std::string& message)
            {
                if (result_.error.empty())
                    result_.error = message;
                return result_;
            }

            void Line(const std::string& text)
            {
                body_ += std::string(indent_ * 4, ' ') + text + "\n";
            }

            // ---- Control flow -----------------------------------------------------------

            bool IsStraightLine() const
            {
                for (const auto& cf : program_)
                {
                    switch (cf.opcode())
                    {
                    case CfOpcode::Nop:
                    case CfOpcode::Exec:
                    case CfOpcode::ExecEnd:
                    case CfOpcode::Alloc:
                    case CfOpcode::MarkVsFetchDone:
                        break;
                    default:
                        return false;
                    }
                    if (cf.IsEnd())
                        return true;
                }
                return true;
            }

            void EmitStraightLine()
            {
                for (const auto& cf : program_)
                {
                    if (cf.IsExec())
                        EmitExecBody(cf);
                    if (cf.IsEnd())
                        break;
                }
            }

            void EmitStateMachine()
            {
                Line("int pc = 0;");
                Line("int loopCount[4] = { 0, 0, 0, 0 };");
                Line("int loopStep[4] = { 0, 0, 0, 0 };");
                Line("int savedAL[4] = { 0, 0, 0, 0 };");
                Line("int loopDepth = 0;");
                Line("int callStack[4] = { 0, 0, 0, 0 };");
                Line("int callDepth = 0;");
                Line("[loop] while (pc >= 0)");
                Line("{");
                indent_++;
                Line("[forcecase] switch (pc)");
                Line("{");
                for (uint32_t index = 0; index < program_.size(); index++)
                {
                    const auto& cf = program_[index];
                    const int next = cf.IsEnd() ? -1 : int(index + 1);
                    Line(Format("case %u:", index));
                    Line("{");
                    indent_++;
                    EmitControlFlowCase(cf, index, next);
                    indent_--;
                    Line("}");
                    if (cf.IsEnd() && index + 1 >= program_.size())
                        break;
                }
                Line("default:");
                Line("    pc = -1;");
                Line("    break;");
                Line("}");
                indent_--;
                Line("}");
            }

            std::string ConditionExpression(const CfInstruction& cf, bool predicateBased)
            {
                if (predicateBased)
                    return cf.condition() ? "p" : "!p";
                return Format("BoolConstant(%u) == %s", cf.boolAddress(), cf.condition() ? "true" : "false");
            }

            void EmitControlFlowCase(const CfInstruction& cf, uint32_t index, int next)
            {
                switch (cf.opcode())
                {
                case CfOpcode::Exec:
                case CfOpcode::ExecEnd:
                    EmitExecBody(cf);
                    break;
                case CfOpcode::CondExec:
                case CfOpcode::CondExecEnd:
                case CfOpcode::CondExecPredClean:
                case CfOpcode::CondExecPredCleanEnd:
                case CfOpcode::CondPredExec:
                case CfOpcode::CondPredExecEnd:
                {
                    const bool predicate = cf.opcode() == CfOpcode::CondPredExec || cf.opcode() == CfOpcode::CondPredExecEnd;
                    Line("if (" + ConditionExpression(cf, predicate) + ")");
                    Line("{");
                    indent_++;
                    EmitExecBody(cf);
                    indent_--;
                    Line("}");
                    break;
                }
                case CfOpcode::LoopStart:
                    // Loop constant: count (bits 0-7), start aL (8-15), signed step (16-23).
                    Line(Format("uint lc = LoopConstant(%u);", cf.loopId()));
                    Line("savedAL[loopDepth] = aL;");
                    Line("loopCount[loopDepth] = int(lc & 0xFF);");
                    Line("loopStep[loopDepth] = int(lc << 8) >> 24;");
                    Line("aL = int((lc >> 8) & 0xFF);");
                    Line("loopDepth++;");
                    Line(Format("if (loopCount[loopDepth - 1] == 0) { loopDepth--; aL = savedAL[loopDepth]; pc = %u; break; }",
                        cf.targetAddress()));
                    break;
                case CfOpcode::LoopEnd:
                    Line("if (--loopCount[loopDepth - 1] > 0)");
                    Line(Format("{ aL += loopStep[loopDepth - 1]; pc = %u; break; }", cf.targetAddress()));
                    Line("loopDepth--;");
                    Line("aL = savedAL[loopDepth];");
                    break;
                case CfOpcode::CondCall:
                    if (cf.isUnconditional())
                        Line(Format("callStack[callDepth++] = %d; pc = %u; break;", next, cf.targetAddress()));
                    else
                    {
                        Line("if (" + ConditionExpression(cf, cf.isPredicated()) + ")");
                        Line(Format("{ callStack[callDepth++] = %d; pc = %u; break; }", next, cf.targetAddress()));
                    }
                    break;
                case CfOpcode::Return:
                    Line("pc = callDepth > 0 ? callStack[--callDepth] : -1;");
                    Line("break;");
                    return;
                case CfOpcode::CondJmp:
                    if (cf.isUnconditional())
                    {
                        Line(Format("pc = %u;", cf.targetAddress()));
                        Line("break;");
                        return;
                    }
                    Line("if (" + ConditionExpression(cf, cf.isPredicated()) + ")");
                    Line(Format("{ pc = %u; break; }", cf.targetAddress()));
                    break;
                default:
                    break;  // nop, alloc, vs_fetch_done
                }
                (void)index;
                Line(Format("pc = %d;", next));
                Line("break;");
            }

            void EmitExecBody(const CfInstruction& cf)
            {
                for (uint32_t i = 0; i < cf.execCount(); i++)
                {
                    const uint32_t slot = cf.execAddress() + i;
                    if (slot * 3 + 3 > dwords_.size())
                    {
                        Fail(Format("instruction slot %u out of range", slot));
                        return;
                    }
                    const uint32_t* d = &dwords_[slot * 3];
                    if ((cf.execSequence() >> (i * 2)) & 1)
                        EmitFetch(FetchInstruction{ { d[0], d[1], d[2] } }, slot);
                    else
                        EmitAlu(AluInstruction{ { d[0], d[1], d[2] } }, slot);
                }
            }

            // ---- Fetch ------------------------------------------------------------------

            uint32_t StreamSlot(uint32_t fetchConstant)
            {
                auto& streams = result_.vertexFetchConstants;
                for (uint32_t i = 0; i < streams.size(); i++)
                    if (streams[i] == fetchConstant)
                        return i;
                if (streams.size() >= kMaxVertexStreams)
                {
                    Fail("too many vertex streams");
                    return 0;
                }
                streams.push_back(fetchConstant);
                result_.vertexStreamStrides.push_back(0);
                result_.vertexStreamTails.push_back(0);
                return uint32_t(streams.size() - 1);
            }

            // Writes `value` (a float4 expression) to the fetch destination with its swizzle.
            void EmitFetchDestination(const FetchInstruction& fetch, const std::string& value)
            {
                const std::string dest = fetch.dstRelative() ? Format("r[%u + aL]", fetch.dstReg()) : Format("r[%u]", fetch.dstReg());
                Line("float4 fetched = " + value + ";");
                for (uint32_t i = 0; i < 4; i++)
                {
                    const uint32_t select = (fetch.dstSwizzle() >> (i * 3)) & 7;
                    if (select == 7)
                        continue;
                    const std::string source = select < 4 ? Format("fetched.%c", kComponents[select]) : select == 5 ? "1.0" : "0.0";
                    Line(Format("%s.%c = %s;", dest.c_str(), kComponents[i], source.c_str()));
                }
            }

            void EmitFetch(const FetchInstruction& fetch, uint32_t slot)
            {
                Line(Format("// slot %u: fetch op %u", slot, uint32_t(fetch.opcode())));
                std::string guard;
                if (fetch.isPredicated())
                    guard = fetch.predCondition() ? "if (p)" : "if (!p)";
                if (!guard.empty())
                    Line(guard);
                Line("{");
                indent_++;
                switch (fetch.opcode())
                {
                case FetchOpcode::VertexFetch:
                    EmitVertexFetch(fetch);
                    break;
                case FetchOpcode::TextureFetch:
                    EmitTextureFetch(fetch);
                    break;
                case FetchOpcode::GetTextureGradients:
                {
                    // Screen-space derivatives of the coordinates: (d/dx of xy, d/dy of xy).
                    const std::string src = fetch.srcRelative() ? Format("r[%u + aL]", fetch.srcReg()) : Format("r[%u]", fetch.srcReg());
                    const uint32_t sx = fetch.tfSrcSwizzle() & 3, sy = (fetch.tfSrcSwizzle() >> 2) & 3;
                    Line(Format("float2 coords = float2(%s.%c, %s.%c);", src.c_str(), kComponents[sx], src.c_str(), kComponents[sy]));
                    EmitFetchDestination(fetch, pixel_ ? "float4(ddx(coords), ddy(coords))" : "float4(0.0, 0.0, 0.0, 0.0)");
                    break;
                }
                case FetchOpcode::SetTextureGradientsHorz:
                case FetchOpcode::SetTextureGradientsVert:
                {
                    // Explicit derivatives (source xy) for later fetches with register gradients.
                    const std::string src = fetch.srcRelative() ? Format("r[%u + aL]", fetch.srcReg()) : Format("r[%u]", fetch.srcReg());
                    const uint32_t sx = fetch.tfSrcSwizzle() & 3, sy = (fetch.tfSrcSwizzle() >> 2) & 3;
                    Line(Format("%s = float2(%s.%c, %s.%c);", fetch.opcode() == FetchOpcode::SetTextureGradientsHorz ? "texGradH" : "texGradV",
                        src.c_str(), kComponents[sx], src.c_str(), kComponents[sy]));
                    break;
                }
                case FetchOpcode::SetTextureLod:
                {
                    // LOD for later fetches that use the register LOD: source component X.
                    const std::string src = fetch.srcRelative() ? Format("r[%u + aL]", fetch.srcReg()) : Format("r[%u]", fetch.srcReg());
                    Line(Format("texLod = %s.%c;", src.c_str(), kComponents[fetch.tfSrcSwizzle() & 3]));
                    break;
                }
                default:
                    Fail(Format("fetch opcode %u not supported", uint32_t(fetch.opcode())));
                    break;
                }
                indent_--;
                Line("}");
            }

            void EmitVertexFetch(const FetchInstruction& fetch)
            {
                if (pixel_)
                {
                    Fail("vertex fetch in a pixel shader");
                    return;
                }
                uint32_t stream, stride;
                std::string index;
                if (fetch.vfIsMini())
                {
                    // Mini fetches reuse the previous full fetch's vertex, stream and stride.
                    if (!lastFullFetch_)
                    {
                        Fail("vfetch_mini without a preceding full fetch");
                        return;
                    }
                    stream = lastStream_;
                    stride = lastStride_;
                    index = "lastVertexIndex";
                }
                else
                {
                    stream = StreamSlot(fetch.vfConstIndex());
                    stride = fetch.vfStride();
                    const std::string src = fetch.srcRelative() ? Format("r[%u + aL]", fetch.srcReg()) : Format("r[%u]", fetch.srcReg());
                    Line(Format("lastVertexIndex = uint(int(%s.%c));", src.c_str(), kComponents[fetch.vfSrcSwizzle()]));
                    index = "lastVertexIndex";
                    lastFullFetch_ = true;
                    lastStream_ = stream;
                    lastStride_ = stride;
                }
                if (stream < result_.vertexStreamStrides.size())
                {
                    result_.vertexStreamStrides[stream] = std::max(result_.vertexStreamStrides[stream], stride);
                    result_.vertexStreamTails[stream] = std::max(result_.vertexStreamTails[stream], fetch.vfOffset() + 4);
                }
                Line(Format("uint address = (%s * %uu + %uu) * 4u + StreamOffset(%uu);", index.c_str(), stride, fetch.vfOffset(), stream));
                const std::string buffer = Format("vertexStream%u", stream);
                // Words in host order: the stream's endian swap applied.
                auto word = [&](int i) {
                    return Format("SwapWords(%s.Load(address + %du).xxxx, StreamEndian(%uu)).x", buffer.c_str(), i * 4, stream);
                };
                auto words = [&](int count) {
                    static const char* const kPad[] = { "", "", ", 0u, 0u", ", 0u", "" };
                    static const char* const kTake[] = { "", "", ".xy", ".xyz", "" };
                    const std::string load = count == 4 ? Format("%s.Load4(address)", buffer.c_str())
                                                        : Format("uint4(%s.Load%d(address)%s)", buffer.c_str(), count, kPad[count]);
                    return Format("SwapWords(%s, StreamEndian(%uu))%s", load.c_str(), stream, kTake[count]);
                };

                const bool isSigned = fetch.vfFormatSigned();
                const bool isInteger = fetch.vfFormatInteger();
                // Integer component of `bits` width: raw integer or normalized fraction.
                auto component = [&](const std::string& raw, uint32_t bits) {
                    if (isInteger)
                        return isSigned ? Format("SignExtend(%s, %u)", raw.c_str(), bits) : Format("float(%s)", raw.c_str());
                    if (isSigned)
                        return Format("SignExtendNorm(%s, %u)", raw.c_str(), bits);
                    return Format("(float(%s) / %u.0)", raw.c_str(), uint32_t((1ull << bits) - 1));
                };

                std::string value;
                bool integerFormat = true;
                switch (fetch.vfFormat())
                {
                case 6:  // 8_8_8_8
                    Line("uint w0 = " + word(0) + ";");
                    value = "float4(" + component("(w0 & 0xFF)", 8) + ", " + component("((w0 >> 8) & 0xFF)", 8) + ", " +
                        component("((w0 >> 16) & 0xFF)", 8) + ", " + component("(w0 >> 24)", 8) + ")";
                    break;
                case 7:  // 2_10_10_10
                    Line("uint w0 = " + word(0) + ";");
                    value = "float4(" + component("(w0 & 0x3FF)", 10) + ", " + component("((w0 >> 10) & 0x3FF)", 10) + ", " +
                        component("((w0 >> 20) & 0x3FF)", 10) + ", " + component("(w0 >> 30)", 2) + ")";
                    break;
                case 10:  // 10_11_11
                    Line("uint w0 = " + word(0) + ";");
                    value = "float4(" + component("(w0 & 0x7FF)", 11) + ", " + component("((w0 >> 11) & 0x7FF)", 11) + ", " +
                        component("(w0 >> 22)", 10) + ", 1.0)";
                    break;
                case 11:  // 11_11_10
                    Line("uint w0 = " + word(0) + ";");
                    value = "float4(" + component("(w0 & 0x3FF)", 10) + ", " + component("((w0 >> 10) & 0x7FF)", 11) + ", " +
                        component("(w0 >> 21)", 11) + ", 1.0)";
                    break;
                case 25:  // 16_16
                    Line("uint w0 = " + word(0) + ";");
                    value = "float4(" + component("(w0 & 0xFFFF)", 16) + ", " + component("(w0 >> 16)", 16) + ", 0.0, 1.0)";
                    break;
                case 26:  // 16_16_16_16
                    Line("uint w0 = " + word(0) + ";");
                    Line("uint w1 = " + word(1) + ";");
                    value = "float4(" + component("(w0 & 0xFFFF)", 16) + ", " + component("(w0 >> 16)", 16) + ", " +
                        component("(w1 & 0xFFFF)", 16) + ", " + component("(w1 >> 16)", 16) + ")";
                    break;
                case 31:  // 16_16_FLOAT
                    integerFormat = false;
                    Line("uint w0 = " + word(0) + ";");
                    value = "float4(f16tof32(w0 & 0xFFFF), f16tof32(w0 >> 16), 0.0, 1.0)";
                    break;
                case 32:  // 16_16_16_16_FLOAT
                    integerFormat = false;
                    Line("uint w0 = " + word(0) + ";");
                    Line("uint w1 = " + word(1) + ";");
                    value = "float4(f16tof32(w0 & 0xFFFF), f16tof32(w0 >> 16), f16tof32(w1 & 0xFFFF), f16tof32(w1 >> 16))";
                    break;
                case 33:  // 32
                    Line("uint w0 = " + word(0) + ";");
                    value = "float4(" + component("w0", 32) + ", 0.0, 0.0, 1.0)";
                    break;
                case 34:  // 32_32
                    Line("uint w0 = " + word(0) + ";");
                    Line("uint w1 = " + word(1) + ";");
                    value = "float4(" + component("w0", 32) + ", " + component("w1", 32) + ", 0.0, 1.0)";
                    break;
                case 35:  // 32_32_32_32
                    Line("uint4 w = " + words(4) + ";");
                    value = "float4(" + component("w.x", 32) + ", " + component("w.y", 32) + ", " + component("w.z", 32) +
                        ", " + component("w.w", 32) + ")";
                    break;
                case 36:  // 32_FLOAT
                    integerFormat = false;
                    value = "float4(asfloat(" + word(0) + "), 0.0, 0.0, 1.0)";
                    break;
                case 37:  // 32_32_FLOAT
                    integerFormat = false;
                    value = "float4(asfloat(" + words(2) + "), 0.0, 1.0)";
                    break;
                case 57:  // 32_32_32_FLOAT
                    integerFormat = false;
                    value = "float4(asfloat(" + words(3) + "), 1.0)";
                    break;
                case 38:  // 32_32_32_32_FLOAT
                    integerFormat = false;
                    value = "asfloat(" + words(4) + ")";
                    break;
                default:
                    Fail(Format("vertex format %u not supported", fetch.vfFormat()));
                    return;
                }
                if (integerFormat && fetch.vfExpAdjust() != 0)
                    value = Format("(%s * %.9g)", value.c_str(), double(std::ldexp(1.0, fetch.vfExpAdjust())));
                EmitFetchDestination(fetch, value);
            }

            void EmitTextureFetch(const FetchInstruction& fetch)
            {
                const uint32_t n = fetch.tfConstIndex();
                result_.textureFetchMask |= 1u << n;
                // The first fetch of a constant that sets its own filters decides them (one sampler
                // serves every fetch of the constant).
                const uint16_t filters = uint16_t(fetch.tfMagFilter() | (fetch.tfMinFilter() << 2) | (fetch.tfMipFilter() << 4) |
                    (fetch.tfAnisoFilter() << 6));
                if (result_.textureFilters[n] == TranslatedShader::kFiltersFromConstant)
                    result_.textureFilters[n] = filters;
                const std::string src = fetch.srcRelative() ? Format("r[%u + aL]", fetch.srcReg()) : Format("r[%u]", fetch.srcReg());
                const uint32_t sx = fetch.tfSrcSwizzle() & 3, sy = (fetch.tfSrcSwizzle() >> 2) & 3;
                if (fetch.tfDimension() == 3)
                {
                    // Cube map: the coordinates are face S and T in [1, 2] and the face index
                    // (as produced by the cube instruction). Rebuild the direction vector.
                    result_.textureCubeMask |= 1u << n;
                    const uint32_t sz = (fetch.tfSrcSwizzle() >> 4) & 3;
                    Line(Format("float sc = (%s.%c - 1.5) * 2.0;", src.c_str(), kComponents[sx]));
                    Line(Format("float tc = (%s.%c - 1.5) * 2.0;", src.c_str(), kComponents[sy]));
                    Line(Format("uint face = uint(clamp(%s.%c, 0.0, 5.0) + 0.5);", src.c_str(), kComponents[sz]));
                    Line("float3 dir = face == 0 ? float3(1.0, -tc, -sc) : face == 1 ? float3(-1.0, -tc, sc) :");
                    Line("    face == 2 ? float3(sc, 1.0, tc) : face == 3 ? float3(sc, -1.0, -tc) :");
                    Line("    face == 4 ? float3(sc, -tc, 1.0) : float3(-sc, -tc, -1.0);");
                    const std::string flat = Format("float2(%s.%c, %s.%c)", src.c_str(), kComponents[sx], src.c_str(), kComponents[sy]);
                    const std::string sample = pixel_ && !fetch.tfUseRegisterLod()
                        ? Format("(textureSizes[%u].w > 1.5 ? texture%u_2d.Sample(sampler%u, %s) : texture%u.Sample(sampler%u, dir))",
                            n, n, n, flat.c_str(), n, n)
                        : Format("(textureSizes[%u].w > 1.5 ? texture%u_2d.SampleLevel(sampler%u, %s, %s) : texture%u.SampleLevel(sampler%u, dir, %s))",
                            n, n, n, flat.c_str(), fetch.tfUseRegisterLod() ? "texLod" : "0.0", n, n,
                            fetch.tfUseRegisterLod() ? "texLod" : "0.0");
                    EmitFetchDestination(fetch, sample);
                    return;
                }
                if (fetch.tfDimension() == 2)
                {
                    // 3D texture: three normalized coordinates.
                    result_.textureVolumeMask |= 1u << n;
                    const uint32_t sz = (fetch.tfSrcSwizzle() >> 4) & 3;
                    Line(Format("float3 uvw = float3(%s.%c, %s.%c, %s.%c);", src.c_str(), kComponents[sx], src.c_str(),
                        kComponents[sy], src.c_str(), kComponents[sz]));
                    // Stacked textures (2D arrays, kept as 3D textures): the layer is the normalized
                    // coordinate times the layer count (or the coordinate itself when unnormalized),
                    // rounded down. Sample that slice's centre (no blending between layers).
                    if (fetch.tfUnnormalized())
                        Line(Format("if (abs(textureSizes[%u].w - 1.0) < 0.5) uvw.z = (floor(uvw.z) + 0.5) / textureSizes[%u].z;", n, n));
                    else
                        Line(Format("if (abs(textureSizes[%u].w - 1.0) < 0.5) uvw.z = (floor(uvw.z * textureSizes[%u].z + 1.5 / 1024.0) + 0.5) / textureSizes[%u].z;",
                            n, n, n));
                    const std::string sample = pixel_ && !fetch.tfUseRegisterLod()
                        ? Format("(textureSizes[%u].w > 1.5 ? texture%u_2d.Sample(sampler%u, uvw.xy) : texture%u.Sample(sampler%u, uvw))",
                            n, n, n, n, n)
                        : Format("(textureSizes[%u].w > 1.5 ? texture%u_2d.SampleLevel(sampler%u, uvw.xy, %s) : texture%u.SampleLevel(sampler%u, uvw, %s))",
                            n, n, n, fetch.tfUseRegisterLod() ? "texLod" : "0.0", n, n, fetch.tfUseRegisterLod() ? "texLod" : "0.0");
                    EmitFetchDestination(fetch, sample);
                    return;
                }
                Line(Format("float2 uv = float2(%s.%c, %s.%c);", src.c_str(), kComponents[sx], src.c_str(), kComponents[sy]));
                if (fetch.tfUnnormalized())
                    Line(Format("uv /= textureSizes[%u].xy;", n));
                if (fetch.tfOffsetX() != 0 || fetch.tfOffsetY() != 0)
                    Line(Format("uv += float2(%d, %d) * 0.5 / textureSizes[%u].xy;", fetch.tfOffsetX(), fetch.tfOffsetY(), n));
                std::string sample;
                if (fetch.tfUseRegisterGradients())
                {
                    sample = Format("texture%u.SampleGrad(sampler%u, uv, texGradH, texGradV)", n, n);
                }
                else if (pixel_ && !fetch.tfUseRegisterLod())
                {
                    sample = fetch.tfLodBias() != 0
                        ? Format("texture%u.SampleBias(sampler%u, uv, %.4f)", n, n, fetch.tfLodBias() / 16.0)
                        : Format("texture%u.Sample(sampler%u, uv)", n, n);
                }
                else
                {
                    sample = Format("texture%u.SampleLevel(sampler%u, uv, %s)", n, n, fetch.tfUseRegisterLod() ? "texLod" : "0.0");
                }
                EmitFetchDestination(fetch, sample);
            }

            // ---- ALU --------------------------------------------------------------------

            // Which relative-constant slot (const_0 / const_1) source i uses: constant sources
            // take slots in order among the sources the instruction reads.
            static int ConstantSlot(const AluInstruction& alu, int i, int vectorOperands, int scalarOperands)
            {
                int slot = 0;
                for (int j = 1; j < i; j++)
                {
                    const bool used = j <= vectorOperands || (j == 3 && scalarOperands > 0);
                    if (used && !alu.srcIsTemp(j))
                        slot++;
                }
                return slot;
            }

            // A source operand's register value as a float4 (before swizzling).
            std::string RawSource(const AluInstruction& alu, int i, int constantSlot)
            {
                const uint32_t reg = alu.srcReg(i);
                std::string value;
                if (alu.srcIsTemp(i))
                {
                    value = (reg & 0x40) ? Format("r[%u + aL]", reg & 0x3F) : Format("r[%u]", reg & 0x3F);
                    if (reg & 0x80)
                        value = "abs(" + value + ")";
                }
                else
                {
                    const bool relative = constantSlot == 0 ? alu.const0Relative() : alu.const1Relative();
                    const std::string base = pixel_ ? "psConstBase" : "vsConstBase";
                    value = relative ? Format("c[%s + %u + %s]", base.c_str(), reg, alu.addressAbsolute() ? "a0" : "aL")
                                     : Format("c[%s + %u]", base.c_str(), reg);
                    if (alu.absConstants())
                        value = "abs(" + value + ")";
                }
                return alu.srcNegate(i) ? "(-" + value + ")" : value;
            }

            void EmitAlu(const AluInstruction& alu, uint32_t slot)
            {
                const auto vop = alu.vectorOpcode();
                const auto sop = alu.scalarOpcode();
                const ExportMasks masks = ComputeWriteMasks(alu);
                const int vectorOperands = VectorOperandCount(vop);
                const int scalarOperands = ScalarOperandCount(sop);

                Line(Format("// slot %u: %s + %s", slot, VectorOpcodeName(vop), ScalarOpcodeName(sop)));
                if (alu.isPredicated())
                    Line(alu.predCondition() ? "if (p)" : "if (!p)");
                Line("{");
                indent_++;

                // Sources, read before any writes (vector and scalar ops co-issue).
                for (int i = 1; i <= vectorOperands; i++)
                {
                    std::string swizzle;
                    for (uint32_t c = 0; c < 4; c++)
                        swizzle.push_back(kComponents[AluSwizzleComponent(alu.srcSwizzle(i), c)]);
                    const int slot = ConstantSlot(alu, i, vectorOperands, scalarOperands);
                    Line(Format("float4 v%d = (%s).%s;", i, RawSource(alu, i, slot).c_str(), swizzle.c_str()));
                }
                if (sop >= ScalarOpcode::MulConst0 && sop <= ScalarOpcode::SubConst1)
                {
                    // Constant-and-temp scalar ops: source 3's register field is a constant
                    // (component from the swizzle's W slot); the temp register index is packed
                    // from the opcode's low bit, swizzle bits 2-5 and the source-3 select bit,
                    // with its component in the swizzle's X slot. The source-3 negate and the
                    // abs-constants flags apply to both operands; the constant is the instruction's
                    // first constant (relative flag 0) only when sources 1 and 2 are both temps.
                    const uint32_t swizzle = alu.srcSwizzle(3);
                    const uint32_t tempReg = (uint32_t(sop) & 1) | (swizzle & 0x3C) | (uint32_t(alu.srcIsTemp(3)) << 1);
                    const bool relative = (alu.srcIsTemp(1) && alu.srcIsTemp(2)) ? alu.const0Relative() : alu.const1Relative();
                    const std::string base = pixel_ ? "psConstBase" : "vsConstBase";
                    std::string constant = relative ? Format("c[%s + %u + %s]", base.c_str(), alu.srcReg(3), alu.addressAbsolute() ? "a0" : "aL")
                                                    : Format("c[%s + %u]", base.c_str(), alu.srcReg(3));
                    std::string temp = Format("r[%u]", tempReg);
                    if (alu.absConstants())
                    {
                        constant = "abs(" + constant + ")";
                        temp = "abs(" + temp + ")";
                    }
                    const char* negate = alu.srcNegate(3) ? "-" : "";
                    Line(Format("float sa = %s(%s).%c;", negate, constant.c_str(), kComponents[ScalarComponentA(swizzle)]));
                    Line(Format("float sb = %s(%s).%c;", negate, temp.c_str(), kComponents[ScalarComponentB(swizzle)]));
                }
                else if (scalarOperands > 0)
                {
                    const std::string raw = RawSource(alu, 3, ConstantSlot(alu, 3, vectorOperands, scalarOperands));
                    Line(Format("float sa = (%s).%c;", raw.c_str(), kComponents[ScalarComponentA(alu.srcSwizzle(3))]));
                    if (scalarOperands == 2)
                        Line(Format("float sb = (%s).%c;", raw.c_str(), kComponents[ScalarComponentB(alu.srcSwizzle(3))]));
                }

                const bool vectorUsed = masks.vector != 0 || IsVectorSideEffect(vop);
                const bool scalarUsed = masks.scalar != 0 || sop != ScalarOpcode::RetainPrev;
                if (vectorUsed)
                    EmitVectorOp(vop);
                if (scalarUsed)
                    EmitScalarOp(sop);
                else
                    Line("float sr = ps;");
                Line("ps = sr;");

                // Writes
                if (alu.isExport())
                {
                    const std::string target = ExportTarget(alu.vectorDest());
                    if (target.empty())
                    {
                        Fail(Format("export to register %u not supported", alu.vectorDest()));
                        indent_--;
                        return;
                    }
                    if (masks.vector)
                        Line(Format("%s%s = %s%s;", target.c_str(), MaskSuffix(masks.vector).c_str(),
                            alu.vectorClamp() ? "saturate(vr)" : "vr", MaskSuffix(masks.vector).c_str()));
                    if (masks.scalar)
                        Line(Format("%s%s = %s;", target.c_str(), MaskSuffix(masks.scalar).c_str(),
                            alu.scalarClamp() ? "saturate(sr)" : "sr"));
                    if (masks.constant0)
                        Line(Format("%s%s = 0.0;", target.c_str(), MaskSuffix(masks.constant0).c_str()));
                    if (masks.constant1)
                        Line(Format("%s%s = 1.0;", target.c_str(), MaskSuffix(masks.constant1).c_str()));
                }
                else
                {
                    if (masks.vector)
                    {
                        const std::string dest = alu.vectorDestRelative() ? Format("r[%u + aL]", alu.vectorDest()) : Format("r[%u]", alu.vectorDest());
                        Line(Format("%s%s = %s%s;", dest.c_str(), MaskSuffix(masks.vector).c_str(),
                            alu.vectorClamp() ? "saturate(vr)" : "vr", MaskSuffix(masks.vector).c_str()));
                    }
                    if (masks.scalar)
                    {
                        const std::string dest = alu.scalarDestRelative() ? Format("r[%u + aL]", alu.scalarDest()) : Format("r[%u]", alu.scalarDest());
                        Line(Format("%s%s = %s;", dest.c_str(), MaskSuffix(masks.scalar).c_str(),
                            alu.scalarClamp() ? "saturate(sr)" : "sr"));
                    }
                }
                indent_--;
                Line("}");
            }

            static bool IsVectorSideEffect(VectorOpcode op)
            {
                return (op >= VectorOpcode::PredSetEqPush && op <= VectorOpcode::KillNe) || op == VectorOpcode::MovA;
            }

            std::string ExportTarget(uint32_t reg)
            {
                if (pixel_)
                {
                    if (reg < 4)
                    {
                        result_.colorTargetMask |= 1u << reg;
                        return Format("oC[%u]", reg);
                    }
                    if (reg == 61)
                    {
                        result_.writesDepth = true;
                        return "oDepth";
                    }
                    return {};
                }
                if (reg == 62)
                    return "oPos";
                if (reg == 63)
                    return "oPts";
                if (reg < 16)
                    return Format("o[%u]", reg);
                return {};
            }

            void EmitVectorOp(VectorOpcode op)
            {
                switch (op)
                {
                case VectorOpcode::Add: Line("float4 vr = v1 + v2;"); break;
                case VectorOpcode::Mul: Line("float4 vr = v1 * v2;"); break;
                case VectorOpcode::Max: Line("float4 vr = max(v1, v2);"); break;
                case VectorOpcode::Min: Line("float4 vr = min(v1, v2);"); break;
                case VectorOpcode::SetEq: Line("float4 vr = float4(v1 == v2);"); break;
                case VectorOpcode::SetGt: Line("float4 vr = float4(v1 > v2);"); break;
                case VectorOpcode::SetGe: Line("float4 vr = float4(v1 >= v2);"); break;
                case VectorOpcode::SetNe: Line("float4 vr = float4(v1 != v2);"); break;
                case VectorOpcode::Frac: Line("float4 vr = frac(v1);"); break;
                case VectorOpcode::Trunc: Line("float4 vr = trunc(v1);"); break;
                case VectorOpcode::Floor: Line("float4 vr = floor(v1);"); break;
                case VectorOpcode::MulAdd: Line("float4 vr = mad(v1, v2, v3);"); break;
                case VectorOpcode::CndEq: Line("float4 vr = (v1 == 0.0) ? v2 : v3;"); break;
                case VectorOpcode::CndGe: Line("float4 vr = (v1 >= 0.0) ? v2 : v3;"); break;
                case VectorOpcode::CndGt: Line("float4 vr = (v1 > 0.0) ? v2 : v3;"); break;
                case VectorOpcode::Dot4: Line("float4 vr = dot(v1, v2).xxxx;"); break;
                case VectorOpcode::Dot3: Line("float4 vr = dot(v1.xyz, v2.xyz).xxxx;"); break;
                case VectorOpcode::Dot2Add: Line("float4 vr = (dot(v1.xy, v2.xy) + v3.x).xxxx;"); break;
                case VectorOpcode::Max4: Line("float4 vr = max(max(v1.x, v1.y), max(v1.z, v1.w)).xxxx;"); break;
                case VectorOpcode::PredSetEqPush:
                case VectorOpcode::PredSetNePush:
                case VectorOpcode::PredSetGtPush:
                case VectorOpcode::PredSetGePush:
                {
                    const char* test = op == VectorOpcode::PredSetEqPush ? "v1.w == 0.0" : op == VectorOpcode::PredSetNePush ? "v1.w != 0.0"
                        : op == VectorOpcode::PredSetGtPush ? "v1.w > 0.0" : "v1.w >= 0.0";
                    Line(Format("bool pushed = (%s) && (v2.w == 0.0);", test));
                    Line("float4 vr = pushed ? 0.0 : (v1.x + 1.0).xxxx;");
                    Line("p = pushed;");
                    break;
                }
                case VectorOpcode::KillEq:
                case VectorOpcode::KillGt:
                case VectorOpcode::KillGe:
                case VectorOpcode::KillNe:
                {
                    const char* compare = op == VectorOpcode::KillEq ? "==" : op == VectorOpcode::KillGt ? ">" : op == VectorOpcode::KillGe ? ">=" : "!=";
                    Line(Format("bool killed = any(v1 %s v2);", compare));
                    Line("float4 vr = killed ? 1.0 : 0.0;");
                    if (pixel_)
                        Line("if (killed) discard;");
                    break;
                }
                case VectorOpcode::Dst: Line("float4 vr = float4(1.0, v1.y * v2.y, v1.z, v2.w);"); break;
                case VectorOpcode::MovA:
                    Line("a0 = clamp(int(floor(v1.w + 0.5)), -256, 255);");
                    Line("float4 vr = v1;");
                    break;
                case VectorOpcode::Cube:
                {
                    // Cube-map face selection. The shader passes the direction as src0 = R.zzxy
                    // (and src1 = R.yxzz); the result is (T, S, 2 * |major axis|, face index).
                    Line("float3 dir = float3(v1.z, v1.w, v1.x);");
                    Line("float3 mag = abs(dir);");
                    Line("float4 vr;");
                    Line("if (mag.z >= mag.x && mag.z >= mag.y)");
                    Line("    vr = float4(-dir.y, dir.z < 0.0 ? -dir.x : dir.x, 2.0 * mag.z, dir.z < 0.0 ? 5.0 : 4.0);");
                    Line("else if (mag.y >= mag.x)");
                    Line("    vr = float4(dir.y < 0.0 ? -dir.z : dir.z, dir.x, 2.0 * mag.y, dir.y < 0.0 ? 3.0 : 2.0);");
                    Line("else");
                    Line("    vr = float4(-dir.y, dir.x < 0.0 ? dir.z : -dir.z, 2.0 * mag.x, dir.x < 0.0 ? 1.0 : 0.0);");
                    break;
                }
                default:
                    Fail(Format("vector opcode %s not supported", VectorOpcodeName(op)));
                    Line("float4 vr = 0.0;");
                    break;
                }
            }

            void EmitScalarOp(ScalarOpcode op)
            {
                switch (op)
                {
                case ScalarOpcode::Add: Line("float sr = sa + sb;"); break;
                case ScalarOpcode::AddPrev: Line("float sr = sa + ps;"); break;
                case ScalarOpcode::Mul: Line("float sr = sa * sb;"); break;
                case ScalarOpcode::MulPrev: Line("float sr = sa * ps;"); break;
                case ScalarOpcode::MulPrev2:
                    Line("float sr = (ps == -kFltMax || isinf(ps) || isnan(ps) || isnan(sb) || sb <= 0.0) ? -kFltMax : sa * ps;");
                    break;
                case ScalarOpcode::Max: Line("float sr = max(sa, sb);"); break;
                case ScalarOpcode::Min: Line("float sr = min(sa, sb);"); break;
                case ScalarOpcode::SetEq: Line("float sr = sa == 0.0 ? 1.0 : 0.0;"); break;
                case ScalarOpcode::SetGt: Line("float sr = sa > 0.0 ? 1.0 : 0.0;"); break;
                case ScalarOpcode::SetGe: Line("float sr = sa >= 0.0 ? 1.0 : 0.0;"); break;
                case ScalarOpcode::SetNe: Line("float sr = sa != 0.0 ? 1.0 : 0.0;"); break;
                case ScalarOpcode::Frac: Line("float sr = frac(sa);"); break;
                case ScalarOpcode::Trunc: Line("float sr = trunc(sa);"); break;
                case ScalarOpcode::Floor: Line("float sr = floor(sa);"); break;
                case ScalarOpcode::Exp: Line("float sr = exp2(sa);"); break;
                case ScalarOpcode::LogClamp: Line("float sr = sa == 0.0 ? -kFltMax : max(log2(sa), -kFltMax);"); break;
                case ScalarOpcode::Log: Line("float sr = log2(sa);"); break;
                case ScalarOpcode::RcpClamp: Line("float sr = clamp(1.0 / sa, -kFltMax, kFltMax);"); break;
                case ScalarOpcode::RcpFf: Line("float sr = 1.0 / sa; if (isinf(sr)) sr = 0.0;"); break;
                case ScalarOpcode::Rcp: Line("float sr = 1.0 / sa;"); break;
                case ScalarOpcode::RsqClamp: Line("float sr = clamp(rsqrt(sa), -kFltMax, kFltMax);"); break;
                case ScalarOpcode::RsqFf: Line("float sr = rsqrt(sa); if (isinf(sr)) sr = 0.0;"); break;
                case ScalarOpcode::Rsq: Line("float sr = rsqrt(sa);"); break;
                case ScalarOpcode::MovA:
                    Line("a0 = clamp(int(floor(sa + 0.5)), -256, 255);");
                    Line("float sr = sa;");
                    break;
                case ScalarOpcode::MovAFloor:
                    Line("a0 = clamp(int(floor(sa)), -256, 255);");
                    Line("float sr = sa;");
                    break;
                case ScalarOpcode::Sub: Line("float sr = sa - sb;"); break;
                case ScalarOpcode::SubPrev: Line("float sr = sa - ps;"); break;
                case ScalarOpcode::PredSetEq: Line("p = sa == 0.0; float sr = p ? 0.0 : 1.0;"); break;
                case ScalarOpcode::PredSetNe: Line("p = sa != 0.0; float sr = p ? 0.0 : 1.0;"); break;
                case ScalarOpcode::PredSetGt: Line("p = sa > 0.0; float sr = p ? 0.0 : 1.0;"); break;
                case ScalarOpcode::PredSetGe: Line("p = sa >= 0.0; float sr = p ? 0.0 : 1.0;"); break;
                case ScalarOpcode::PredSetInv:
                    Line("p = sa == 1.0; float sr = p ? 0.0 : (sa == 0.0 ? 1.0 : sa);");
                    break;
                case ScalarOpcode::PredSetPop:
                    Line("p = (sa - 1.0) <= 0.0; float sr = p ? 0.0 : sa - 1.0;");
                    break;
                case ScalarOpcode::PredSetClear: Line("p = false; float sr = kFltMax;"); break;
                case ScalarOpcode::PredSetRestore: Line("p = sa == 0.0; float sr = sa;"); break;
                case ScalarOpcode::KillEq:
                case ScalarOpcode::KillGt:
                case ScalarOpcode::KillGe:
                case ScalarOpcode::KillNe:
                case ScalarOpcode::KillOne:
                {
                    const char* test = op == ScalarOpcode::KillEq ? "sa == 0.0" : op == ScalarOpcode::KillGt ? "sa > 0.0"
                        : op == ScalarOpcode::KillGe ? "sa >= 0.0" : op == ScalarOpcode::KillNe ? "sa != 0.0" : "sa == 1.0";
                    Line(Format("bool skilled = %s;", test));
                    Line("float sr = skilled ? 1.0 : 0.0;");
                    if (pixel_)
                        Line("if (skilled) discard;");
                    break;
                }
                case ScalarOpcode::MulConst0:
                case ScalarOpcode::MulConst1: Line("float sr = sa * sb;"); break;
                case ScalarOpcode::AddConst0:
                case ScalarOpcode::AddConst1: Line("float sr = sa + sb;"); break;
                case ScalarOpcode::SubConst0:
                case ScalarOpcode::SubConst1: Line("float sr = sa - sb;"); break;
                case ScalarOpcode::Sqrt: Line("float sr = sqrt(sa);"); break;
                case ScalarOpcode::Sin: Line("float sr = sin(sa);"); break;
                case ScalarOpcode::Cos: Line("float sr = cos(sa);"); break;
                case ScalarOpcode::RetainPrev: Line("float sr = ps;"); break;
                default:
                    Fail(Format("scalar opcode %u not supported", uint32_t(op)));
                    Line("float sr = 0.0;");
                    break;
                }
            }

            // ---- Assembly ---------------------------------------------------------------

            std::string Declarations()
            {
                std::string text;
                for (uint32_t i = 0; i < result_.vertexFetchConstants.size(); i++)
                    text += Format("ByteAddressBuffer vertexStream%u : register(t%u);\n", i, i);
                for (uint32_t n = 0; n < 32; n++)
                {
                    if (result_.textureFetchMask & (1u << n))
                    {
                        text += Format("%s<float4> texture%u : register(t%u);\n",
                            (result_.textureCubeMask >> n) & 1 ? "TextureCube" : (result_.textureVolumeMask >> n) & 1 ? "Texture3D" : "Texture2D",
                            n, kTextureSlotBase + n);
                        if (((result_.textureCubeMask | result_.textureVolumeMask) >> n) & 1)
                            text += Format("Texture2D<float4> texture%u_2d : register(t%u);\n", n, kTextureFallbackSlotBase + n);
                        text += Format("SamplerState sampler%u : register(s%u);\n", n, n);
                    }
                }
                return text + "\n";
            }

            std::string CommonLocals()
            {
                return
                    "    float4 r[64];\n"
                    "    [unroll] for (int i = 0; i < 64; i++) r[i] = 0.0;\n"
                    "    float ps = 0.0;\n"
                    "    float texLod = 0.0;\n"
                    "    float2 texGradH = 0.0, texGradV = 0.0;\n"
                    "    bool p = false;\n"
                    "    int a0 = 0;\n"
                    "    int aL = 0;\n";
            }

            std::string VertexMain()
            {
                return
                    "struct VertexOutput\n{\n"
                    "    float4 interpolators[16] : TEXCOORD0;\n"
                    "    float4 position : SV_Position;\n"
                    "};\n\n"
                    "VertexOutput main(uint vertexId : SV_VertexID)\n{\n" +
                    CommonLocals() +
                    "    uint lastVertexIndex = 0;\n"
                    "    r[0] = float4(float(vertexId + vertexIndexOffset), 0.0, 0.0, 0.0);\n"
                    "    float4 o[16];\n"
                    "    [unroll] for (int j = 0; j < 16; j++) o[j] = 0.0;\n"
                    "    float4 oPos = float4(0.0, 0.0, 0.0, 1.0);\n"
                    "    float4 oPts = 0.0;\n\n" +
                    body_ +
                    "\n    // Xenos viewport transform -> D3D clip space (D3D divides by w afterwards).\n"
                    "    float w = wIsReciprocal != 0 ? 1.0 / oPos.w : oPos.w;\n"
                    "    float3 position = oPos.xyz;\n"
                    "    if (xyPreDivided != 0) position.xy *= w;\n"
                    "    if (ndcScale.w != 0.0) position.z *= w;\n"
                    "    VertexOutput output;\n"
                    "    output.interpolators = o;\n"
                    "    output.position = float4(position * ndcScale.xyz + ndcOffset.xyz * w, w);\n"
                    "    return output;\n"
                    "}\n";
            }

            std::string PixelMain()
            {
                return
                    "struct PixelInput\n{\n"
                    "    float4 interpolators[16] : TEXCOORD0;\n"
                    "    float4 position : SV_Position;\n"
                    "};\n\n"
                    "struct PixelOutput\n{\n"
                    "    float4 color[4] : SV_Target0;\n" +
                    std::string(result_.writesDepth ? "    float depth : SV_Depth;\n" : "") +
                    "};\n\n"
                    "PixelOutput main(PixelInput input)\n{\n" +
                    CommonLocals() +
                    "    [unroll] for (int k = 0; k < 16; k++) r[k] = input.interpolators[k];\n"
                    "    float4 oC[4];\n"
                    "    [unroll] for (int m = 0; m < 4; m++) oC[m] = 0.0;\n"
                    "    float4 oDepth = 0.0;\n\n" +
                    body_ +
                    "\n    // Fixed-function alpha test on oC0.a.\n"
                    "    if (alphaFunc != 7)\n"
                    "    {\n"
                    "        const float a = oC[0].a;\n"
                    "        const bool alphaPass = alphaFunc == 1 ? a < alphaRef : alphaFunc == 2 ? a == alphaRef :\n"
                    "            alphaFunc == 3 ? a <= alphaRef : alphaFunc == 4 ? a > alphaRef : alphaFunc == 5 ? a != alphaRef :\n"
                    "            alphaFunc == 6 ? a >= alphaRef : false;\n"
                    "        if (!alphaPass)\n"
                    "            discard;\n"
                    "    }\n"
                    "    PixelOutput output;\n"
                    "    output.color = oC;\n" +
                    std::string(result_.writesDepth ? "    output.depth = oDepth.x;\n" : "") +
                    "    return output;\n"
                    "}\n";
            }

            const std::vector<uint32_t>& dwords_;
            bool pixel_;
            std::vector<CfInstruction> program_;
            TranslatedShader result_;
            std::string body_;
            int indent_ = 1;
            bool lastFullFetch_ = false;
            uint32_t lastStream_ = 0;
            uint32_t lastStride_ = 0;
        };
    }

    TranslatedShader TranslateShader(const ShaderMicrocode& shader)
    {
        return Translator(shader).Run();
    }
}
