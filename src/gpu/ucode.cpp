#include "gpu/ucode.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace hc09::gpu::ucode
{
    std::vector<CfInstruction> DecodeControlFlow(const std::vector<uint32_t>& dwords)
    {
        // CF pairs run from dword 0 up to the first instruction slot any exec references.
        std::vector<CfInstruction> program;
        uint32_t pairBound = uint32_t(dwords.size() / 3);
        for (uint32_t pair = 0; pair < pairBound; pair++)
        {
            const uint32_t* p = &dwords[pair * 3];
            const CfInstruction a{ uint64_t(p[0]) | (uint64_t(p[1] & 0xFFFF) << 32) };
            const CfInstruction b{ uint64_t(p[1] >> 16) | (uint64_t(p[2]) << 16) };
            for (const auto& cf : { a, b })
            {
                program.push_back(cf);
                if (cf.IsExec() && cf.execCount() > 0)
                    pairBound = std::min(pairBound, cf.execAddress());
            }
        }
        return program;
    }

    int VectorOperandCount(VectorOpcode op)
    {
        switch (op)
        {
        case VectorOpcode::MulAdd:
        case VectorOpcode::CndEq:
        case VectorOpcode::CndGe:
        case VectorOpcode::CndGt:
        case VectorOpcode::Dot2Add:
            return 3;
        case VectorOpcode::Frac:
        case VectorOpcode::Trunc:
        case VectorOpcode::Floor:
        case VectorOpcode::Max4:
        case VectorOpcode::MovA:
            return 1;
        default:
            return 2;
        }
    }

    int ScalarOperandCount(ScalarOpcode op)
    {
        switch (op)
        {
        case ScalarOpcode::Add:
        case ScalarOpcode::Mul:
        case ScalarOpcode::MulPrev2:
        case ScalarOpcode::Max:
        case ScalarOpcode::Min:
        case ScalarOpcode::Sub:
        case ScalarOpcode::MulConst0:
        case ScalarOpcode::MulConst1:
        case ScalarOpcode::AddConst0:
        case ScalarOpcode::AddConst1:
        case ScalarOpcode::SubConst0:
        case ScalarOpcode::SubConst1:
            return 2;
        case ScalarOpcode::RetainPrev:
        case ScalarOpcode::PredSetClear:
            return 0;
        default:
            return 1;
        }
    }

    const char* VectorOpcodeName(VectorOpcode op)
    {
        static const char* const names[] = {
            "add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad",
            "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4",
            "setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push",
            "kill_eq", "kill_gt", "kill_ge", "kill_ne", "dst", "mova",
        };
        const uint32_t index = uint32_t(op);
        return index < std::size(names) ? names[index] : "vector_op?";
    }

    const char* ScalarOpcodeName(ScalarOpcode op)
    {
        static const char* const names[] = {
            "adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts",
            "sges", "snes", "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp",
            "rsqc", "rsqf", "rsq", "movas", "movas_floor", "subs", "subs_prev", "setp_eq", "setp_ne",
            "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr", "setp_rstr", "kills_eq",
            "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", "scalar_op41?", "mulsc0",
            "mulsc1", "addsc0", "addsc1", "subsc0", "subsc1", "sin", "cos", "retain_prev",
        };
        const uint32_t index = uint32_t(op);
        return index < std::size(names) ? names[index] : "scalar_op?";
    }

    namespace
    {
        const char kComponents[] = "xyzw";

        std::string Format(const char* format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        std::string WriteMask(uint32_t mask)
        {
            std::string text = ".";
            for (int i = 0; i < 4; i++)
                text.push_back(mask & (1u << i) ? kComponents[i] : '_');
            return text;
        }

        // Fetch destination swizzle: 3 bits per component; 4 = 0, 5 = 1, 7 = not written.
        std::string FetchDestSwizzle(uint32_t swizzle)
        {
            std::string text = ".";
            for (int i = 0; i < 4; i++)
            {
                const uint32_t s = (swizzle >> (i * 3)) & 7;
                text.push_back(s < 4 ? kComponents[s] : s == 4 ? '0' : s == 5 ? '1' : '_');
            }
            return text;
        }

        std::string DestName(uint32_t reg, bool exported, bool pixelShader)
        {
            if (!exported)
                return Format("r%u", reg);
            if (pixelShader)
                return reg < 4 ? Format("oC%u", reg) : reg == 61 ? "oDepth" : Format("export%u", reg);
            if (reg == 62)
                return "oPos";
            if (reg == 63)
                return "oPts";
            return reg < 16 ? Format("o%u", reg) : Format("export%u", reg);
        }

        std::string SourceName(const AluInstruction& alu, int i, int components)
        {
            const uint32_t reg = alu.srcReg(i);
            std::string text = alu.srcNegate(i) ? "-" : "";
            if (alu.srcIsTemp(i))
            {
                const bool absolute = (reg & 0x80) != 0;
                std::string name = (reg & 0x40) ? Format("r[%u+aL]", reg & 0x3F) : Format("r%u", reg & 0x3F);
                text += absolute ? "|" + name + "|" : name;
            }
            else
            {
                const bool relative = (i == 1 || i == 2) ? (i == 1 ? alu.const0Relative() : alu.const1Relative())
                                                         : alu.const1Relative();
                std::string name = relative ? Format("c[%u+%s]", reg, alu.addressAbsolute() ? "a0" : "aL") : Format("c%u", reg);
                text += alu.absConstants() ? "|" + name + "|" : name;
            }
            const uint32_t swizzle = alu.srcSwizzle(i);
            text.push_back('.');
            if (components == 4)
            {
                for (uint32_t c = 0; c < 4; c++)
                    text.push_back(kComponents[AluSwizzleComponent(swizzle, c)]);
            }
            else
            {
                text.push_back(kComponents[ScalarComponentA(swizzle)]);
                if (components == 2)
                    text.push_back(kComponents[ScalarComponentB(swizzle)]);
            }
            return text;
        }

        void DisassembleFetch(std::string& out, const FetchInstruction& fetch)
        {
            const std::string pred = fetch.isPredicated() ? (fetch.predCondition() ? "(p) " : "(!p) ") : "";
            switch (fetch.opcode())
            {
            case FetchOpcode::VertexFetch:
                out += Format("      %svfetch%s r%u%s, r%u.%c, vf%u (format %u%s%s, stride %u, offset %u%s, exp %d)\n",
                    pred.c_str(), fetch.vfIsMini() ? "_mini" : "", fetch.dstReg(), FetchDestSwizzle(fetch.dstSwizzle()).c_str(),
                    fetch.srcReg(), kComponents[fetch.vfSrcSwizzle()], fetch.vfConstIndex(), fetch.vfFormat(),
                    fetch.vfFormatSigned() ? " signed" : "", fetch.vfFormatInteger() ? " int" : "", fetch.vfStride(),
                    fetch.vfOffset(), "", fetch.vfExpAdjust());
                break;
            case FetchOpcode::TextureFetch:
            {
                std::string src;
                for (int c = 0; c < 3; c++)
                    src.push_back(kComponents[(fetch.tfSrcSwizzle() >> (c * 2)) & 3]);
                out += Format("      %stfetch r%u%s, r%u.%s, tf%u%s (filters %u/%u/%u, lod bias %d)\n", pred.c_str(),
                    fetch.dstReg(), FetchDestSwizzle(fetch.dstSwizzle()).c_str(), fetch.srcReg(), src.c_str(),
                    fetch.tfConstIndex(), fetch.tfUnnormalized() ? " unnormalized" : "", fetch.tfMagFilter(),
                    fetch.tfMinFilter(), fetch.tfMipFilter(), fetch.tfLodBias());
                break;
            }
            default:
                out += Format("      %sfetch_op%u r%u, r%u, tf%u\n", pred.c_str(), uint32_t(fetch.opcode()), fetch.dstReg(),
                    fetch.srcReg(), fetch.tfConstIndex());
                break;
            }
        }

        void DisassembleAlu(std::string& out, const AluInstruction& alu, bool pixelShader)
        {
            const std::string pred = alu.isPredicated() ? (alu.predCondition() ? "(p) " : "(!p) ") : "";
            const auto vop = alu.vectorOpcode();
            const auto sop = alu.scalarOpcode();
            const ExportMasks masks = ComputeWriteMasks(alu);
            const bool vectorActive = masks.vector != 0 || vop >= VectorOpcode::PredSetEqPush;
            const bool scalarActive = masks.scalar != 0 || (sop != ScalarOpcode::RetainPrev && !alu.isExport());
            // Exports send both results to the vector destination's export register.
            const uint32_t scalarDest = alu.isExport() ? alu.vectorDest() : alu.scalarDest();

            if (vectorActive)
            {
                out += Format("      %s%s%s %s%s", pred.c_str(), VectorOpcodeName(vop), alu.vectorClamp() ? "_sat" : "",
                    DestName(alu.vectorDest(), alu.isExport(), pixelShader).c_str(), WriteMask(masks.vector).c_str());
                for (int i = 1; i <= VectorOperandCount(vop); i++)
                    out += ", " + SourceName(alu, i, 4);
                out += "\n";
            }
            if (scalarActive)
            {
                out += Format("      %s%s%s%s %s%s", vectorActive ? "+ " : "", pred.c_str(), ScalarOpcodeName(sop),
                    alu.scalarClamp() ? "_sat" : "", DestName(scalarDest, alu.isExport(), pixelShader).c_str(),
                    WriteMask(masks.scalar).c_str());
                const int count = ScalarOperandCount(sop);
                if (count > 0)
                    out += ", " + SourceName(alu, 3, count);
                out += "\n";
            }
            if (masks.constant0 | masks.constant1)
            {
                out += Format("      + %s", DestName(alu.vectorDest(), true, pixelShader).c_str());
                if (masks.constant0)
                    out += WriteMask(masks.constant0) + " = 0";
                if (masks.constant1)
                    out += std::string(masks.constant0 ? ", " : "") + WriteMask(masks.constant1) + " = 1";
                out += "\n";
            }
        }
    }

    std::string Disassemble(const std::vector<uint32_t>& dwords, bool pixelShader)
    {
        std::string out;
        const auto program = DecodeControlFlow(dwords);
        for (size_t index = 0; index < program.size(); index++)
        {
            const auto& cf = program[index];
            const auto op = cf.opcode();
            if (cf.IsExec())
            {
                const char* names[] = { "", "exec", "exec_end", "cexec", "cexec_end", "cexec_p", "cexec_p_end", "", "", "",
                    "", "", "", "cexec_pclean", "cexec_pclean_end" };
                std::string condition;
                if (op == CfOpcode::CondExec || op == CfOpcode::CondExecEnd || op == CfOpcode::CondExecPredClean ||
                    op == CfOpcode::CondExecPredCleanEnd)
                    condition = Format(" %sb%u", cf.condition() ? "" : "!", cf.boolAddress());
                else if (op == CfOpcode::CondPredExec || op == CfOpcode::CondPredExecEnd)
                    condition = cf.condition() ? " p" : " !p";
                out += Format("  %02zu %s%s  (slots %u..%u)\n", index, names[uint32_t(op)], condition.c_str(),
                    cf.execAddress(), cf.execAddress() + cf.execCount() - 1);
                for (uint32_t i = 0; i < cf.execCount(); i++)
                {
                    const uint32_t slot = cf.execAddress() + i;
                    if (slot * 3 + 3 > dwords.size())
                    {
                        out += "      <slot out of range>\n";
                        break;
                    }
                    const uint32_t* d = &dwords[slot * 3];
                    if ((cf.execSequence() >> (i * 2)) & 1)
                        DisassembleFetch(out, FetchInstruction{ { d[0], d[1], d[2] } });
                    else
                        DisassembleAlu(out, AluInstruction{ { d[0], d[1], d[2] } }, pixelShader);
                }
                continue;
            }
            switch (op)
            {
            case CfOpcode::Nop: out += Format("  %02zu nop\n", index); break;
            case CfOpcode::Alloc:
            {
                const char* kinds[] = { "none", "position", pixelShader ? "colors" : "interpolators", "memory" };
                out += Format("  %02zu alloc %s, %u\n", index, kinds[cf.allocType()], cf.allocSize() + 1);
                break;
            }
            case CfOpcode::LoopStart: out += Format("  %02zu loop_start i%u -> %u\n", index, cf.loopId(), cf.targetAddress()); break;
            case CfOpcode::LoopEnd: out += Format("  %02zu loop_end i%u -> %u\n", index, cf.loopId(), cf.targetAddress()); break;
            case CfOpcode::CondCall: out += Format("  %02zu call -> %u\n", index, cf.targetAddress()); break;
            case CfOpcode::Return: out += Format("  %02zu ret\n", index); break;
            case CfOpcode::CondJmp: out += Format("  %02zu jmp%s -> %u\n", index, cf.isUnconditional() ? "" : " (cond)", cf.targetAddress()); break;
            case CfOpcode::MarkVsFetchDone: out += Format("  %02zu vs_fetch_done\n", index); break;
            default: out += Format("  %02zu cf_op%u\n", index, uint32_t(op)); break;
            }
        }
        return out;
    }
}
