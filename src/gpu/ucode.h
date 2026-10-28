#pragma once

// Xenos (Xbox 360 GPU) shader microcode decoding.
//
// A shader is a control-flow program followed by 96-bit instruction slots:
//   - Control flow (CF) instructions are 48 bits, packed two per 3 dwords from dword 0.
//   - EXEC-type CF instructions run `count` slots starting at slot `address`
//     (slot n = dwords 3n..3n+2); a 2-bit-per-slot sequence says fetch vs ALU.
//   - Fetch slots are vertex or texture fetches; ALU slots co-issue a vector and a
//     scalar operation.

#include <cstdint>
#include <string>
#include <vector>

namespace hc09::gpu::ucode
{
    // ---- Control flow -------------------------------------------------------------------

    enum class CfOpcode : uint32_t
    {
        Nop = 0,
        Exec = 1,
        ExecEnd = 2,
        CondExec = 3,
        CondExecEnd = 4,
        CondPredExec = 5,
        CondPredExecEnd = 6,
        LoopStart = 7,
        LoopEnd = 8,
        CondCall = 9,
        Return = 10,
        CondJmp = 11,
        Alloc = 12,
        CondExecPredClean = 13,
        CondExecPredCleanEnd = 14,
        MarkVsFetchDone = 15,
    };

    struct CfInstruction
    {
        uint64_t bits;  // 48 significant bits

        uint32_t Field(int shift, int width) const { return uint32_t((bits >> shift) & ((1ull << width) - 1)); }
        CfOpcode opcode() const { return CfOpcode(Field(44, 4)); }

        bool IsExec() const
        {
            switch (opcode())
            {
            case CfOpcode::Exec: case CfOpcode::ExecEnd: case CfOpcode::CondExec: case CfOpcode::CondExecEnd:
            case CfOpcode::CondPredExec: case CfOpcode::CondPredExecEnd: case CfOpcode::CondExecPredClean:
            case CfOpcode::CondExecPredCleanEnd:
                return true;
            default:
                return false;
            }
        }
        bool IsEnd() const
        {
            const auto op = opcode();
            return op == CfOpcode::ExecEnd || op == CfOpcode::CondExecEnd || op == CfOpcode::CondPredExecEnd ||
                op == CfOpcode::CondExecPredCleanEnd;
        }

        // Exec family
        uint32_t execAddress() const { return Field(0, 12); }
        uint32_t execCount() const { return Field(12, 3); }
        uint32_t execSequence() const { return Field(16, 12); }
        uint32_t boolAddress() const { return Field(34, 8); }  // CondExec*, CondCall, CondJmp
        bool condition() const { return Field(42, 1) != 0; }   // CondExec*, CondPredExec, CondCall, CondJmp

        // Loops, calls, jumps
        uint32_t targetAddress() const { return Field(0, 13); }
        uint32_t loopId() const { return Field(16, 5); }
        bool isUnconditional() const { return Field(13, 1) != 0; }
        bool isPredicated() const { return Field(14, 1) != 0; }

        // Alloc: type 1 = position, 2 = interpolators (VS) / colors (PS), 3 = memory export
        uint32_t allocSize() const { return Field(0, 3); }
        uint32_t allocType() const { return Field(41, 2); }
    };

    // Decodes CF instructions until (and including) the pair holding the first *_END.
    std::vector<CfInstruction> DecodeControlFlow(const std::vector<uint32_t>& dwords);

    // ---- Fetch --------------------------------------------------------------------------

    enum class FetchOpcode : uint32_t
    {
        VertexFetch = 0,
        TextureFetch = 1,
        GetTextureBorderColorFrac = 16,
        GetTextureComputedLod = 17,
        GetTextureGradients = 18,
        GetTextureWeights = 19,
        SetTextureLod = 24,
        SetTextureGradientsHorz = 25,
        SetTextureGradientsVert = 26,
    };

    struct FetchInstruction
    {
        uint32_t d[3];

        static uint32_t Bits(uint32_t value, int shift, int width) { return (value >> shift) & ((1u << width) - 1); }
        FetchOpcode opcode() const { return FetchOpcode(Bits(d[0], 0, 5)); }

        uint32_t srcReg() const { return Bits(d[0], 5, 6); }
        bool srcRelative() const { return Bits(d[0], 11, 1) != 0; }
        uint32_t dstReg() const { return Bits(d[0], 12, 6); }
        bool dstRelative() const { return Bits(d[0], 18, 1) != 0; }
        uint32_t dstSwizzle() const { return Bits(d[1], 0, 12); }  // 3 bits per component
        bool isPredicated() const { return Bits(d[1], 31, 1) != 0; }  // pred_select
        bool predCondition() const { return Bits(d[2], 31, 1) != 0; }

        // Vertex fetch
        uint32_t vfConstIndex() const { return Bits(d[0], 20, 5) * 3 + Bits(d[0], 25, 2); }
        uint32_t vfSrcSwizzle() const { return Bits(d[0], 30, 2); }  // component of src reg holding the index
        bool vfFormatSigned() const { return Bits(d[1], 12, 1) != 0; }  // format_comp_all
        bool vfFormatInteger() const { return Bits(d[1], 13, 1) != 0; } // num_format_all
        uint32_t vfFormat() const { return Bits(d[1], 16, 6); }
        int32_t vfExpAdjust() const { return int32_t(Bits(d[1], 23, 6) << 26) >> 26; }
        bool vfIsMini() const { return Bits(d[1], 30, 1) != 0; }
        uint32_t vfStride() const { return Bits(d[2], 0, 8); }   // dwords
        uint32_t vfOffset() const { return Bits(d[2], 8, 23); }  // dwords

        // Texture fetch
        uint32_t tfConstIndex() const { return Bits(d[0], 20, 5); }
        bool tfUnnormalized() const { return Bits(d[0], 25, 1) != 0; }
        uint32_t tfSrcSwizzle() const { return Bits(d[0], 26, 6); }  // 2 bits per component (xyz)
        uint32_t tfMagFilter() const { return Bits(d[1], 12, 2); }
        uint32_t tfMinFilter() const { return Bits(d[1], 14, 2); }
        uint32_t tfMipFilter() const { return Bits(d[1], 16, 2); }
        uint32_t tfAnisoFilter() const { return Bits(d[1], 18, 3); }  // 7: the fetch constant's
        bool tfUseComputedLod() const { return Bits(d[1], 28, 1) != 0; }
        bool tfUseRegisterLod() const { return Bits(d[1], 29, 1) != 0; }
        bool tfUseRegisterGradients() const { return Bits(d[2], 0, 1) != 0; }
        int32_t tfLodBias() const { return int32_t(Bits(d[2], 2, 7) << 25) >> 25; }  // 1/16 steps
        uint32_t tfDimension() const { return Bits(d[2], 14, 2); }  // 0 1D, 1 2D, 2 3D/stacked, 3 cube
        int32_t tfOffsetX() const { return int32_t(Bits(d[2], 16, 5) << 27) >> 27; }
        int32_t tfOffsetY() const { return int32_t(Bits(d[2], 21, 5) << 27) >> 27; }
        int32_t tfOffsetZ() const { return int32_t(Bits(d[2], 26, 5) << 27) >> 27; }
    };

    // ---- ALU ----------------------------------------------------------------------------

    enum class VectorOpcode : uint32_t
    {
        Add = 0, Mul = 1, Max = 2, Min = 3, SetEq = 4, SetGt = 5, SetGe = 6, SetNe = 7,
        Frac = 8, Trunc = 9, Floor = 10, MulAdd = 11, CndEq = 12, CndGe = 13, CndGt = 14,
        Dot4 = 15, Dot3 = 16, Dot2Add = 17, Cube = 18, Max4 = 19,
        PredSetEqPush = 20, PredSetNePush = 21, PredSetGtPush = 22, PredSetGePush = 23,
        KillEq = 24, KillGt = 25, KillGe = 26, KillNe = 27, Dst = 28, MovA = 29,
    };

    enum class ScalarOpcode : uint32_t
    {
        Add = 0, AddPrev = 1, Mul = 2, MulPrev = 3, MulPrev2 = 4, Max = 5, Min = 6,
        SetEq = 7, SetGt = 8, SetGe = 9, SetNe = 10, Frac = 11, Trunc = 12, Floor = 13,
        Exp = 14, LogClamp = 15, Log = 16, RcpClamp = 17, RcpFf = 18, Rcp = 19,
        RsqClamp = 20, RsqFf = 21, Rsq = 22, MovA = 23, MovAFloor = 24, Sub = 25, SubPrev = 26,
        PredSetEq = 27, PredSetNe = 28, PredSetGt = 29, PredSetGe = 30, PredSetInv = 31,
        PredSetPop = 32, PredSetClear = 33, PredSetRestore = 34,
        KillEq = 35, KillGt = 36, KillGe = 37, KillNe = 38, KillOne = 39, Sqrt = 40,
        MulConst0 = 42, MulConst1 = 43, AddConst0 = 44, AddConst1 = 45, SubConst0 = 46, SubConst1 = 47,
        Sin = 48, Cos = 49, RetainPrev = 50,
    };

    struct AluInstruction
    {
        uint32_t d[3];

        static uint32_t Bits(uint32_t value, int shift, int width) { return (value >> shift) & ((1u << width) - 1); }

        uint32_t vectorDest() const { return Bits(d[0], 0, 6); }
        bool vectorDestRelative() const { return Bits(d[0], 6, 1) != 0; }
        bool absConstants() const { return Bits(d[0], 7, 1) != 0; }
        uint32_t scalarDest() const { return Bits(d[0], 8, 6); }
        bool scalarDestRelative() const { return Bits(d[0], 14, 1) != 0; }
        bool isExport() const { return Bits(d[0], 15, 1) != 0; }
        uint32_t vectorWriteMask() const { return Bits(d[0], 16, 4); }
        uint32_t scalarWriteMask() const { return Bits(d[0], 20, 4); }
        bool vectorClamp() const { return Bits(d[0], 24, 1) != 0; }
        bool scalarClamp() const { return Bits(d[0], 25, 1) != 0; }
        ScalarOpcode scalarOpcode() const { return ScalarOpcode(Bits(d[0], 26, 6)); }

        // Sources are numbered 1..3: vector ops read 1, 2 (and 3 for 3-operand ops); the
        // scalar op reads source 3.
        uint32_t srcSwizzle(int i) const { return Bits(d[1], (3 - i) * 8, 8); }
        bool srcNegate(int i) const { return Bits(d[1], 24 + (3 - i), 1) != 0; }
        bool isPredicated() const { return Bits(d[1], 28, 1) != 0; }
        bool predCondition() const { return Bits(d[1], 27, 1) != 0; }
        bool addressAbsolute() const { return Bits(d[1], 29, 1) != 0; }
        bool const1Relative() const { return Bits(d[1], 30, 1) != 0; }
        bool const0Relative() const { return Bits(d[1], 31, 1) != 0; }
        uint32_t srcReg(int i) const { return Bits(d[2], (3 - i) * 8, 8); }
        VectorOpcode vectorOpcode() const { return VectorOpcode(Bits(d[2], 24, 5)); }
        bool srcIsTemp(int i) const { return Bits(d[2], 32 - i, 1) != 0; }  // src1 = bit 31
    };

    // Swizzle helpers. ALU swizzles are relative: component i selects ((s >> 2i) + i) & 3.
    inline uint32_t AluSwizzleComponent(uint32_t swizzle, uint32_t i) { return ((swizzle >> (i * 2)) + i) & 3; }
    // Scalar ops read one component (from the W slot) or two (W slot, then X slot).
    inline uint32_t ScalarComponentA(uint32_t swizzle) { return ((swizzle >> 6) + 3) & 3; }
    inline uint32_t ScalarComponentB(uint32_t swizzle) { return swizzle & 3; }

    // Effective write masks. For exports the two masks combine: vector-only components get
    // the vector result, scalar-only components the scalar result (into the same export
    // register), components in both get 1.0, and components in neither get 0.0 when bit 14
    // (scalar_dest_rel) is set.
    struct ExportMasks
    {
        uint32_t vector;
        uint32_t scalar;
        uint32_t constant0;
        uint32_t constant1;
    };

    inline ExportMasks ComputeWriteMasks(const AluInstruction& alu)
    {
        const uint32_t v = alu.vectorWriteMask(), s = alu.scalarWriteMask();
        if (!alu.isExport())
            return { v, s, 0, 0 };
        return { v & ~s, s & ~v, alu.scalarDestRelative() ? (~(v | s) & 0xF) : 0, v & s };
    }

    int VectorOperandCount(VectorOpcode op);
    int ScalarOperandCount(ScalarOpcode op);  // 0, 1 or 2 components of source 3
    const char* VectorOpcodeName(VectorOpcode op);
    const char* ScalarOpcodeName(ScalarOpcode op);

    // Readable listing of a whole shader.
    std::string Disassemble(const std::vector<uint32_t>& dwords, bool pixelShader);
}
