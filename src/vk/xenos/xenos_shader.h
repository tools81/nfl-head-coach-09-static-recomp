#pragma once
// Xenos shader microcode: decoding, disassembly and a CPU interpreter.
// Portable C++17 (see xenos.h). Instruction formats and ALU semantics follow
// the hardware as documented by Xenia (ucode.h, shader_interpreter.cc;
// BSD-3-Clause), reimplemented here, plus texture fetch.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "xenos.h"

namespace xenos {

// Transcendentals with the same result on every C library: evaluated in double
// and rounded once to float (the correctly rounded value, barring astronomically
// rare ties). The float versions differ in the last ulp between glibc and the
// Windows UCRT, which changed whole pixels on rounding boundaries.
inline float Exp2(float x) { return float(std::exp2(double(x))); }
inline float Log2(float x) { return float(std::log2(double(x))); }
inline float Sin(float x) { return float(std::sin(double(x))); }
inline float Cos(float x) { return float(std::cos(double(x))); }

struct ShaderCode
{
    ShaderType type = ShaderType::kVertex;
    std::vector<uint32_t> dw;   // host-order dwords
};
ShaderCode ShaderFromBigEndian(ShaderType type, const uint8_t* bytes, uint32_t dwords);

std::string Disassemble(const ShaderCode& code);

// Constants visible to one draw (snapshotted from the register file).
struct ShaderConstants
{
    float alu[512][4];
    uint32_t bools[8];
    uint32_t loops[32];
    uint32_t vsBase = 0, vsSize = 512, psBase = 256, psSize = 256;   // SQ_VS_CONST / SQ_PS_CONST windows
    void Snapshot();
};

// Receives exports (vertex position/interpolators, pixel colours).
struct ExportSink
{
    virtual ~ExportSink() = default;
    virtual void Export(uint32_t reg, const float v[4], uint32_t mask) = 0;
};

// One texture fetch as the shader issues it.
struct TextureFetchRequest
{
    uint32_t slot = 0;                  // fetch constant
    float coord[3] = {};                // already swizzled (cube: s, t in [1,2), face)
    uint32_t dimension = 1;             // 0 1D, 1 2D, 2 3D/stacked, 3 cube
    bool unnormalized = false;
    int32_t offsetX = 0, offsetY = 0;   // half texels
    uint32_t magFilter = 3, minFilter = 3, mipFilter = 3;   // 3 = from the fetch constant
    uint32_t volMagFilter = 3;          // between layers of a stacked texture (3 = from the fetch constant)
    bool useComputedLod = true;
    float lodBias = 0.0f;               // instruction bias (fetch constant bias is added by the sampler)
    uint32_t sequence = 0;              // n-th texture fetch of this invocation (pairs fetches across a quad)
    // Register LOD / gradients (setTexLOD, setGradientsH/V): the LOD from these
    // instead of the coordinates' own derivatives (which jump where a shader
    // builds coordinates per cell, e.g. the letters of a jersey name).
    bool useRegisterLod = false, useRegisterGradients = false;
    float registerLod = 0.0f, gradH[3] = {}, gradV[3] = {};
};

struct TextureSampler
{
    virtual ~TextureSampler() = default;
    virtual void Sample(const TextureFetchRequest& request, float out[4]) = 0;
    // getGradients: coarse screen derivatives (ddx(x), ddy(x), ddx(y), ddy(y)) of
    // two values across the pixel quad; `sequence` pairs the query across lanes.
    virtual void Gradients(uint32_t sequence, float x, float y, float out[4]) { out[0] = out[1] = out[2] = out[3] = 0.0f; }
};

// Whether any exec block of the program contains a texture fetch.
bool ShaderHasTextureFetch(const ShaderCode& code);
// Whether the program allocates memory exports (alloc type 3: memexport via eA/eM0-4).
bool ShaderUsesMemExport(const ShaderCode& code);

class ShaderInterpreter
{
public:
    static constexpr uint32_t kTemps = 128;
    float r[kTemps][4];   // temp registers; set inputs before Run (VS: r0.x = vertex index)
    bool killed = false;  // pixel kill executed
    std::string* trace = nullptr;   // if set: one line per executed instruction (results, predicate, a0)

    ShaderInterpreter(const ShaderConstants& c, ExportSink& sink, TextureSampler* sampler)
        : c_(c), sink_(sink), sampler_(sampler) {}
    void Run(const ShaderCode& code);

private:
    const float* Constant(uint32_t index, bool relative, bool useA0, float tmp[4]) const;
    float* Temp(uint32_t index, bool relative);
    int32_t LoopAddress() const;
    void Alu(const uint32_t* i);
    void Fetch(const uint32_t* i);
    void StoreFetch(uint32_t dest, bool rel, uint32_t swz, const float v[4]);

    const ShaderConstants& c_;
    ExportSink& sink_;
    TextureSampler* sampler_;
    ShaderType type_ = ShaderType::kVertex;
    float prevScalar_ = 0;
    bool predicate_ = false;
    int32_t a0_ = 0;
    int loopDepth_ = 0;
    uint32_t loopIter_[4] = {}, loopConst_[4] = {};
    uint32_t vfetchConst_ = 0, vfetchStride_ = 0, vfetchAddressDw_ = 0;
    float traceCoord_[3] = {};          // the last texture fetch's coordinates (trace output)
    uint32_t fetchSeq_ = 0, gradSeq_ = 0;
    float regLod_ = 0.0f, gradH_[3] = {}, gradV_[3] = {};   // setTexLOD / setGradientsH / setGradientsV
    uint32_t addr_ = 0;   // instruction being executed (for the trace)
};

}  // namespace xenos
