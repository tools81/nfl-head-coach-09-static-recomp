// hc09: smoother player models (roadmap 13); see vk_tessellation.h.
//
// The game's vertex shaders are translated from the player's disc as always. For the models we know, a tessellated
// draw runs that translation as its vertex shader, cut short at the guest's clip position, and two stages of our own
// after it. The players' vertex shaders share the game's lighting constants: the model's skinned position goes to the
// clip position through c156-c159 (one column each) and to the world through c180-c183, its normal to the world
// through c188-c190 (the inverse transpose of c180-c182). Each shader's interpolators hold the world normal in a slot
// of its own (some also the eye, c208, less the world position).
//
// Phong tessellation (Boubekeur and Alexa, 2008): a point of the split triangle, at barycentric b, is the flat point
// p = sum b_i P_i pulled toward sum b_i pi_i(p), pi_i the projection on the tangent plane of corner i (P_i, N_i). Done
// in the model: the corners' positions come back from their clip positions through the inverse of c156-c159, their
// normals from the world through c180-c182 transposed (the model's scale is uniform, so tangent planes stay tangent).
// The clip position is linear in the model position, so the flat point's clip position is the corners' interpolated
// and the offset adds through c156-c158.
#include "vk_tessellation.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "../dev_env.h"

namespace hc09vk {
namespace {

struct TessShader
{
    uint64_t hash;
    int normal;   // the interpolator with the world normal (xyz)
    int view;     // the interpolator with the eye (c208) less the world position (xyz), moved with the point; -1: none
};

constexpr TessShader kTessShaders[] = {
    { 0x590ce12dae1d6099ull, 2, 1 },    // helmets (and facemasks)
    { 0xf006680e5784b107ull, 1, -1 },   // uniforms (its direction to the eye is normalized)
};

const TessShader* Find(uint64_t hash)
{
    for (const TessShader& s : kTessShaders)
        if (s.hash == hash) return &s;
    return nullptr;
}

// HC09_TESS (dev): how far points move toward the curved surface, 0 (off, the default) to 1; 0.75 is Phong
// tessellation's usual shape factor (1 bulges flat-sided parts). Off by default: the game's player models proved dense
// enough that their outlines hardly change, and seams where a model's normals split open pinholes.
float Strength()
{
    static const float s = [] {
        const char* e = Hc09Env("HC09_TESS");
        return e && *e ? std::clamp(float(atof(e)), 0.0f, 1.0f) : 0.0f;
    }();
    return s;
}

// HC09_TESS_PIXELS: the length of the split edges' pieces on the screen, in the game's pixels (6 by default).
float PiecePixels()
{
    static const float p = [] {
        const char* e = Hc09Env("HC09_TESS_PIXELS");
        return e && *e ? std::clamp(float(atof(e)), 1.0f, 64.0f) : 6.0f;
    }();
    return p;
}

constexpr const char* kMain = "void main()";
constexpr const char* kWindow = "    // Xenos viewport transform";

std::string Fixed(float v)
{
    char s[32];
    snprintf(s, sizeof(s), "%.4f", v);
    return s;
}

}  // namespace

bool TessellatedVertexShader(uint64_t vsHash)
{
    return Find(vsHash) && Strength() > 0.0f;
}

bool TessellationShaders(uint64_t vsHash, const std::string& vsGlsl, std::string& vs, std::string& tcs, std::string& tes)
{
    const TessShader* shader = Find(vsHash);
    const size_t main = vsGlsl.find(kMain), window = vsGlsl.find(kWindow), end = vsGlsl.rfind('}');
    if (!shader || main == std::string::npos || window == std::string::npos || end == std::string::npos || window < main || end < window)
        return false;
    const std::string prefix = vsGlsl.substr(0, main);
    // The vertex shader's window transform: it reads `pos` (the guest's clip position) and writes gl_Position, xeZ and
    // the clip distances.
    const std::string toWindow = vsGlsl.substr(window, end - window);

    // The vertex shader: as translated, its clip position passed on as it is.
    vs = prefix + "layout(location = 17) out vec4 xeTessPos;\n" + vsGlsl.substr(main, window - main) +
         "    xeTessPos = pos;\n"
         "    gl_Position = pos;\n"
         "}\n";

    // The control shader: the corners passed on; each edge split into pieces of about PiecePixels() on the screen,
    // from its own two corners only, so the two triangles sharing an edge split it alike (no cracks).
    std::string declarations;   // the prefix without the vertex shader's outputs
    for (size_t at = 0; at < prefix.size();)
    {
        size_t eol = prefix.find('\n', at);
        if (eol == std::string::npos) eol = prefix.size() - 1;
        const std::string line = prefix.substr(at, eol + 1 - at);
        at = eol + 1;
        if (line.rfind("layout(location", 0) == 0 || line.rfind("out gl_PerVertex", 0) == 0) continue;
        declarations += line;
    }
    tcs = declarations +
          "layout(vertices = 3) out;\n"
          "layout(location = 0) in vec4 vo[][16];\n"
          "layout(location = 17) in vec4 vPos[];\n"
          "layout(location = 0) out vec4 tcO[][16];\n"
          "layout(location = 17) out vec4 tcPos[];\n"
          "float xeEdgeLevel(vec4 a, vec4 b)\n"
          "{\n"
          "    if (a.w <= 1e-4 || b.w <= 1e-4) return 1.0;   // behind the eye: left as it is\n"
          "    vec2 d = (a.xy / a.w - b.xy / b.w) * D.vpScale.xy;   // the game's pixels\n"
          "    return clamp(length(d) / " + Fixed(PiecePixels()) + ", 1.0, 8.0);\n"
          "}\n"
          "void main()\n"
          "{\n"
          "    tcO[gl_InvocationID] = vo[gl_InvocationID];\n"
          "    tcPos[gl_InvocationID] = vPos[gl_InvocationID];\n"
          "    if (gl_InvocationID == 0)\n"
          "    {\n"
          "        gl_TessLevelOuter[0] = xeEdgeLevel(vPos[1], vPos[2]);\n"
          "        gl_TessLevelOuter[1] = xeEdgeLevel(vPos[2], vPos[0]);\n"
          "        gl_TessLevelOuter[2] = xeEdgeLevel(vPos[0], vPos[1]);\n"
          "        gl_TessLevelInner[0] = max(gl_TessLevelOuter[0], max(gl_TessLevelOuter[1], gl_TessLevelOuter[2]));\n"
          "    }\n"
          "}\n";

    // The evaluation shader: the point's interpolators and clip position, moved by Phong tessellation, then the
    // vertex shader's window transform.
    const std::string n = std::to_string(shader->normal), v = std::to_string(shader->view);
    tes = prefix +
          "layout(triangles, equal_spacing, cw) in;\n"
          "layout(location = 0) in vec4 tcO[][16];\n"
          "layout(location = 17) in vec4 tcPos[];\n"
          "void main()\n"
          "{\n"
          "    cBase = D.vsBase; cSize = D.vsSize;\n"
          "    vec3 tb = gl_TessCoord;\n"
          "    for (int k = 0; k < 16; ++k) o[k] = tb.x * tcO[0][k] + tb.y * tcO[1][k] + tb.z * tcO[2][k];\n"
          "    vec4 pos = tb.x * tcPos[0] + tb.y * tcPos[1] + tb.z * tcPos[2];\n"
          "    mat4 tMvp = mat4(cnst(156), cnst(157), cnst(158), cnst(159));\n"
          "    mat4 tToModel = inverse(tMvp);\n"
          "    mat3 tToWorld = mat3(cnst(180).xyz, cnst(181).xyz, cnst(182).xyz);\n"
          "    vec3 tP[3], tN[3];\n"
          "    for (int i = 0; i < 3; ++i)\n"
          "    {\n"
          "        vec4 m = tToModel * tcPos[i];\n"
          "        tP[i] = m.xyz / m.w;\n"
          "        vec3 tn = transpose(tToWorld) * tcO[i][" + n + "].xyz;\n"
          "        tN[i] = dot(tn, tn) > 1e-12 ? normalize(tn) : vec3(0.0);\n"
          "    }\n"
          "    vec3 tp = tb.x * tP[0] + tb.y * tP[1] + tb.z * tP[2];\n"
          "    vec3 tq = vec3(0.0);\n"
          "    for (int i = 0; i < 3; ++i) tq += tb[i] * (tp - dot(tp - tP[i], tN[i]) * tN[i]);\n"
          "    vec3 tdm = (tq - tp) * " + Fixed(Strength()) + ";\n"
          "    pos += tMvp * vec4(tdm, 0.0);\n" +
          (shader->view >= 0 ? "    o[" + v + "].xyz -= tToWorld * tdm;\n" : std::string()) +
          toWindow + "}\n";
    return true;
}

}  // namespace hc09vk
