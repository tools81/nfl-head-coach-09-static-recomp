// hc09: hand-written shading for the turf and the players (roadmap 10); see vk_materials.h.
//
// The game's shaders are translated from the player's disc at run time as always; this only appends code of our own
// to the end of the ones we know, where their colour is final: nothing derived from the game is stored or shipped.
//
// The turf (kTurfPs): the field is one draw, its markings painted into the same shader. Its interpolators hold the
// world position (vin[2], yards: x across the field, y up, z along it, 0 at midfield) and the camera's position
// (vin[3]); the shared lighting constants hold the sun's direction (c204) and colour (c205).
#include "vk_materials.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../dev_env.h"
#include "common/field_surface.h"
#include "gpu/texture_replacements.h"

namespace hc09vk {
namespace {

// Where the code goes: after the game's shader has its colours, before the alpha test and the targets' scale.
constexpr const char* kEpilogue = "    if (killed) discard;\n";

float Setting(const char* value, float fallback)
{
    return value && *value ? std::clamp(float(atof(value)), 0.0f, 2.0f) : fallback;
}

std::vector<uint64_t> TintHashes()
{
    std::vector<uint64_t> hashes;
    if (const char* e = Hc09Env("HC09_SHADER_TINT"))
        for (const char* p = e; *p;)
        {
            char* end = nullptr;
            uint64_t h = strtoull(p, &end, 16);
            if (end == p) break;
            hashes.push_back(h);
            p = *end ? end + 1 : end;
        }
    return hashes;
}

bool Insert(std::string& glsl, const std::string& code)
{
    const size_t at = glsl.rfind(kEpilogue);
    if (at == std::string::npos) return false;
    glsl.insert(at, code);
    return true;
}

// Value noise and its sums, shared by the materials.
const char* kNoise = R"GLSL(
float hcHash(vec2 p) { p = fract(p * vec2(443.897, 441.423)); p += dot(p, p.yx + 19.19); return fract((p.x + p.y) * p.x); }
float hcNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hcHash(i), hcHash(i + vec2(1.0, 0.0)), u.x), mix(hcHash(i + vec2(0.0, 1.0)), hcHash(i + vec2(1.0, 1.0)), u.x), u.y);
}
// How much of a pattern of this size (yards) a pixel of footprint fw can show without shimmering.
float hcShow(float size, float fw) { return 1.0 - smoothstep(0.2 * size, 0.6 * size, fw); }
float hcFbm(vec2 p) { return 0.5 * hcNoise(p) + 0.3 * hcNoise(p * 2.03 + 17.0) + 0.2 * hcNoise(p * 4.11 + 41.0); }
)GLSL";

// The turf: `c` the game's colour, `p` the world position, `eye` the camera's; D.pad2: how worn the field is (0..1).
const char* kTurf = R"GLSL(
// Blade height at p: three sizes of tufts, each only where the pixel is small enough to show it.
float hcBlades(vec2 p, float fw)
{
    return hcShow(0.03, fw) * 0.5 * hcNoise(p / 0.03) + hcShow(0.09, fw) * 0.3 * hcNoise(p / 0.09 + 7.0) +
           hcShow(0.4, fw) * 0.2 * hcNoise(p / 0.4 + 13.0);
}
vec3 hcTurf(vec3 c, vec3 p, vec3 eye, float strength)
{
    float fw = max(max(abs(dFdx(p.x)), abs(dFdy(p.x))), max(abs(dFdx(p.z)), abs(dFdy(p.z))));
    vec3 toEye = eye - p;
    vec3 v = length(toEye) > 1e-4 ? normalize(toEye) : vec3(0.0, 1.0, 0.0);
    vec3 l = cnst(204).xyz;
    l = dot(l, l) > 1e-6 ? normalize(l) : vec3(0.3, 0.85, 0.4);
    if (l.y < 0.0) l = -l;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    // Grass, not paint: green above red and blue.
    float grass = smoothstep(0.004, 0.03, c.g - max(c.r, c.b));
    float paint = 1.0 - grass;

    // Blades: a bump map of the tufts, lit by the sun against the flat field the game lit.
    const float e = 0.008;
    vec2 q = p.xz;
    float h0 = hcBlades(q, fw);
    vec3 n = normalize(vec3(-(hcBlades(q + vec2(e, 0.0), fw) - h0) / e * 0.012, 1.0, -(hcBlades(q + vec2(0.0, e), fw) - h0) / e * 0.012));
    float lit = (0.45 + 0.55 * clamp(dot(n, l), 0.0, 1.0)) / (0.45 + 0.55 * clamp(l.y, 0.0, 1.0));
    float shade = mix(1.0, lit, 0.8);

    // The blade layer: looking through the blades along the view, the ground between them hides at low angles and
    // the lit tips show. Three shells of blades above the ground, each offset along the view by its height.
    vec2 slope = v.xz / max(v.y, 0.2);
    float tips = 0.0;
    for (int k = 1; k <= 3; ++k)
    {
        float height = 0.012 * float(k);
        vec2 at = q - slope * height;
        tips = max(tips, smoothstep(0.55, 0.8, hcNoise(at / 0.02 + float(k) * 3.7)) * (1.0 - 0.25 * float(k - 1)));
    }
    float near = hcShow(0.02, fw);
    float graze = 1.0 - clamp(v.y, 0.0, 1.0);
    shade *= mix(1.0, mix(0.86, 1.1, tips), near * (0.5 + 0.5 * graze));

    // Large-scale variation, so the grass texture's repeats do not line up: brightness and a yellow / blue-green
    // drift over a few yards.
    float macro = hcFbm(q / 3.0) - 0.5;
    float hue = hcFbm(q / 9.0 + 31.0) - 0.5;
    vec3 col = c * (1.0 + macro * 0.14 * (0.4 + 0.6 * grass));
    col = mix(col, col * vec3(1.06, 1.0, 0.86), clamp(hue * 1.6, 0.0, 1.0) * grass);
    col = mix(col, col * vec3(0.94, 1.0, 1.05), clamp(-hue * 1.6, 0.0, 1.0) * grass);
    col *= shade;
    // A soft sheen at grazing angles, as light catches the blades' sides.
    col += pow(graze, 5.0) * 0.05 * vec3(0.85, 0.95, 0.85) * (0.5 + 0.5 * lum) * grass;

    // Wear: between the hash marks and in the middle of the field, in patches, growing through the game.
    float worn = D.pad2;
    float band = exp(-(p.x * p.x) / 30.0);                          // the hash marks are 3.1 yards from the middle
    float field = smoothstep(52.0, 38.0, abs(p.z)) * (0.55 + 0.45 * smoothstep(30.0, 5.0, abs(p.z)));
    float patches = smoothstep(0.62 - 0.32 * worn, 0.86 - 0.3 * worn, hcFbm(q / 1.7 + 5.0) + 0.15 * hcNoise(q / 0.35));
    float wear = clamp(band * field * patches * (0.25 + 0.75 * worn), 0.0, 1.0) * (grass + 0.35 * paint);
    vec3 dirt = vec3(0.42, 0.33, 0.22) * (0.75 + 0.5 * lum) * shade;
    vec3 tired = mix(vec3(lum), col, 0.55) * vec3(1.08, 1.0, 0.78);   // yellowed grass before it gives way
    col = mix(col, tired, clamp(wear * 2.0, 0.0, 1.0) * 0.6);
    col = mix(col, dirt, smoothstep(0.35, 0.9, wear) * 0.8);

    return mix(c, col, strength);
}
)GLSL";

// The players. Each material's shader gives its shading normal partway through, as the vector it dots with the sun's
// direction (c204) to light the surface: hcN is taken there, in the world's axes. The vertex shader gives the
// direction to the camera (the eye, c208, less the world position) in an interpolator: hcV. The game's colour is
// final by the end, and the materials add to it. `c` the game's colour; sun direction and colour as the turf's.
const char* kPlayers = R"GLSL(
vec3 hcN = vec3(0.0, 1.0, 0.0);
float hcLum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 hcSunDir() { vec3 l = cnst(204).xyz; l = dot(l, l) > 1e-6 ? normalize(l) : vec3(0.3, 0.85, 0.4); return l.y < 0.0 ? -l : l; }
vec3 hcSunColour() { vec3 s = cnst(205).rgb; return hcLum(s) > 1e-3 ? clamp(s, 0.0, 2.0) : vec3(1.0); }
vec3 hcView(vec3 toEye) { return dot(toEye, toEye) > 1e-8 ? normalize(toEye) : vec3(0.0, 0.0, 1.0); }
vec3 hcNormal() { return dot(hcN, hcN) > 1e-6 ? normalize(hcN) : vec3(0.0, 1.0, 0.0); }

// Jersey and pants fabric: knit cloth has no sharp highlight but a soft sheen where it turns from the eye (the
// fibres' sides catch the light, Charlie sheen), strongest toward the light, in the cloth's own colour.
vec3 hcFabric(vec3 c, vec3 toEye, float strength)
{
    vec3 n = hcNormal(), v = hcView(toEye), l = hcSunDir();
    float nv = clamp(dot(n, v), 0.0, 1.0), nl = dot(n, l);
    float rim = pow(1.0 - nv, 2.0);
    float lit = clamp((nl + 0.6) / 1.6, 0.0, 1.0);
    vec3 tint = mix(c, vec3(hcLum(c)), 0.35) * 1.5 + 0.02;
    vec3 col = c * (0.95 + 0.05 * rim);
    col += tint * hcSunColour() * rim * (0.25 + 0.75 * lit) * 0.7;
    return mix(c, col, strength);
}

// The helmet's clear coat: the game's own shading already mirrors the stadium's lights in the paint, so the coat
// adds only the sun's small, sharp highlight. (A mirrored stadium on top laid a flat veil over the paint at grazing
// angles, where the view ray carries on to the field: a dark shell around light helmets, a grey one on dark.)
vec3 hcClearCoat(vec3 c, vec3 toEye, float strength)
{
    vec3 n = hcNormal(), v = hcView(toEye), l = hcSunDir();
    // How much the normal turns within the pixel: where it does (the paint's bumps, far away) the highlight spreads
    // and dims instead of sparkling.
    vec3 dn = fwidth(n);
    float rough = clamp(dot(dn, dn) * 6.0, 0.0, 1.0);
    vec3 r = reflect(-v, n);
    vec3 sun = hcSunColour();
    vec3 col = c;
    float rl = clamp(dot(r, l), 0.0, 1.0);
    float shine = mix(300.0, 30.0, rough);
    float spot = pow(rl, shine) * shine / 300.0 * 1.2 + pow(rl, 40.0) * 0.12;
    col += sun * spot * step(0.0, dot(n, l));
    return mix(c, col, strength);
}

// Skin: light enters it and leaves a little way off, reddened by the blood below. Shading softens across the
// terminator (light wraps past where the surface turns from the sun) and thin parts glow when lit from behind.
vec3 hcSkin(vec3 c, vec3 toEye, float strength)
{
    vec3 n = hcNormal(), v = hcView(toEye), l = hcSunDir();
    float nl = dot(n, l), nv = clamp(dot(n, v), 0.0, 1.0);
    vec3 tone = c / (max(max(c.r, c.g), c.b) + 1e-3) * clamp(hcLum(c) * 1.8 + 0.2, 0.25, 0.8);
    float wrap = clamp((nl + 0.45) / 1.45, 0.0, 1.0) - clamp(nl, 0.0, 1.0);   // only past the terminator
    vec3 scatter = vec3(1.0, 0.38, 0.22) * wrap * 0.35;
    float behind = pow(clamp(dot(v, -l), 0.0, 1.0), 4.0) * pow(1.0 - nv, 2.0) * 0.25;
    vec3 sun = hcSunColour();
    vec3 col = c * vec3(1.02, 0.995, 0.98) + tone * sun * (scatter + behind * vec3(1.0, 0.3, 0.15));
    return mix(c, col, strength);
}

// The coaches' skin, seen in close-up: the wrapped light only, and less of it (their shader's interpolator is not
// the direction to the camera all over the face, so the glow from behind broke into the model's triangles, and
// the players' amount tinted a shaded face orange).
vec3 hcCoachSkin(vec3 c, vec3 toEye, float strength)
{
    vec3 n = hcNormal(), l = hcSunDir();
    float nl = dot(n, l);
    vec3 tone = c / (max(max(c.r, c.g), c.b) + 1e-3) * clamp(hcLum(c) * 1.8 + 0.2, 0.25, 0.8);
    float wrap = clamp((nl + 0.45) / 1.45, 0.0, 1.0) - clamp(nl, 0.0, 1.0);
    vec3 col = c * vec3(1.02, 0.995, 0.98) + tone * hcSunColour() * vec3(1.0, 0.38, 0.22) * wrap * 0.12;
    return mix(c, col, strength);
}
)GLSL";

enum class Material { Fabric, ClearCoat, Skin, CoachSkin };
struct PlayerShader
{
    uint64_t hash;
    Material material;
    int normalAt;   // the instruction that dots the shading normal with the sun's direction
    int view;       // the interpolator holding the direction to the camera
};
constexpr PlayerShader kPlayerShaders[] = {
    { 0x9cb95c34ae9d25a7ull, Material::Fabric, 127, 3 },      // uniform
    { 0xe3f8c0a876c20584ull, Material::Fabric, 109, 3 },      // uniform, far from the camera
    { 0xe263d5063e531450ull, Material::ClearCoat, 113, 1 },   // helmet
    { 0x1fac9a5c9d5eef90ull, Material::Skin, 33, 2 },         // skin
    { 0x941750d3ee74f1b9ull, Material::Skin, 35, 2 },         // skin, far from the camera
    // The coaches.
    { 0x146fcacb58c42f1dull, Material::Fabric, 48, 6 },       // shirt and cap
    { 0x164849505f6b7b32ull, Material::Fabric, 55, 5 },       // clothes
    { 0x50557abc53c1c6b8ull, Material::CoachSkin, 81, 3 },    // skin
};

// Before instruction `at`, the vector it dots with c204 into hcN, in c204's (the world's) axes: the instruction's
// first operand, its components paired with the constant's swizzle.
bool CaptureNormal(std::string& glsl, int at)
{
    const std::string head = "{   // " + std::to_string(at) + ":";
    const size_t start = glsl.find(head);
    if (start == std::string::npos) return false;
    const size_t end = glsl.find("{   // ", start + head.size());
    const std::string block = glsl.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t s1 = block.find("vec4 s1 = "), s2 = block.find("vec4 s2 = cnst(204).");
    if (s1 == std::string::npos || s2 == std::string::npos) return false;
    const size_t s1End = block.find(';', s1);
    const std::string operand = block.substr(s1 + 10, s1End - s1 - 10);
    const std::string swizzle = block.substr(s2 + 20, 3);
    std::string code = "{ vec4 hcS = " + operand + ";";
    static const char kFrom[] = "xyz";
    for (int i = 0; i < 3; ++i)
    {
        if (swizzle[i] < 'x' || swizzle[i] > 'z') return false;
        code += std::string(" hcN.") + swizzle[i] + " = hcS." + kFrom[i] + ";";
    }
    code += " }\n            ";
    glsl.insert(start, code);
    return true;
}

const PlayerShader* FindPlayerShader(uint64_t hash)
{
    for (const PlayerShader& s : kPlayerShaders)
        if (s.hash == hash) return &s;
    return nullptr;
}

// HC09_PLAYERS_AB=1 (dev): the players' shading only in every other vertical stripe of 24 pixels, to compare.
const bool kPlayersAb = Hc09Env("HC09_PLAYERS_AB") != nullptr;

bool PlayerPatch(uint64_t hash, std::string& glsl)
{
    const PlayerShader* shader = FindPlayerShader(hash);
    if (!shader) return false;
    static const float strength = Setting(Hc09Env("HC09_PLAYERS"), 1.0f);
    if (strength <= 0.0f) return false;
    const size_t mainAt = glsl.find("void main()");
    if (mainAt == std::string::npos || glsl.rfind(kEpilogue) == std::string::npos) return false;
    std::string patched = glsl;
    if (!CaptureNormal(patched, shader->normalAt))
    {
        fprintf(stderr, "[materials] shader %016llx: no normal at instruction %d, left as it is\n", (unsigned long long)hash,
                shader->normalAt);
        return false;
    }
    patched.insert(patched.find("void main()"), std::string("// hc09: the players (vk_materials.cpp).") + kNoise + kPlayers + "\n");
    static const char* const kCalls[] = { "hcFabric", "hcClearCoat", "hcSkin", "hcCoachSkin" };
    char call[160];
    snprintf(call, sizeof(call), "    oc0.rgb = %s(oc0.rgb, vin[%d].xyz, %.3f%s);\n", kCalls[int(shader->material)], shader->view, strength,
             kPlayersAb ? " * step(0.5, fract(gl_FragCoord.x / 48.0))" : "");
    if (!Insert(patched, call)) return false;
    glsl = std::move(patched);
    return true;
}

struct TurfState
{
    uint32_t field = 0;                                // the field's texture: a new one is a new game
    uint32_t game = 0;                                 // hc09::g_gamesStarted when this game's wear began
    std::chrono::steady_clock::time_point last{};
    double seconds = 0.0;                              // seconds the field has been drawn this game
};
TurfState turf;

}  // namespace

bool MaterialPatch(uint64_t hash, std::string& glsl)
{
    static const std::vector<uint64_t> tint = TintHashes();
    static const bool tintAll = [] { const char* e = Hc09Env("HC09_SHADER_TINT"); return e && !strcmp(e, "all"); }();
    if (tintAll && glsl.find("cnst(204)") != std::string::npos)   // every lit shader a colour of its own, logged
    {
        const uint32_t h = uint32_t(hash ^ (hash >> 29) ^ (hash >> 47)) * 0x9E3779B1u;
        const int r = int(h >> 24), g = int((h >> 16) & 255), b = int((h >> 8) & 255);
        fprintf(stderr, "[tint] %016llx %d %d %d\n", (unsigned long long)hash, r, g, b);
        char code[96];
        snprintf(code, sizeof(code), "    oc0.rgb = vec3(%d, %d, %d) / 255.0;\n", r, g, b);
        return Insert(glsl, code);
    }
    static const char* const kColours[] = { "1, 0, 0", "0, 1, 0", "0, 0, 1", "1, 1, 0", "1, 0, 1", "0, 1, 1", "1, 0.5, 0", "1, 1, 1" };
    for (size_t i = 0; i < tint.size(); ++i)
        if (tint[i] == hash)
            return Insert(glsl, std::string("    oc0.rgb = vec3(") + kColours[i % 8] + ");\n");

    bool patched = false;
    if (hash == kTurfPs)
    {
        static const float strength = Setting(Hc09Env("HC09_TURF"), 1.0f);
        const size_t mainAt = glsl.find("void main()");
        if (strength > 0.0f && mainAt != std::string::npos && glsl.rfind(kEpilogue) != std::string::npos)
        {
            glsl.insert(mainAt, std::string("// hc09: the turf (vk_materials.cpp).") + kNoise + kTurf + "\n");
            char call[128];
            snprintf(call, sizeof(call), "    oc0.rgb = hcTurf(oc0.rgb, vin[2].xyz, vin[3].xyz, %.3f);\n", strength);
            patched = Insert(glsl, call);
        }
    }
    else
        patched = PlayerPatch(hash, glsl);

    // HC09_SHADER_EXPR=<hash>=<GLSL expression>[;<hash>=<expression>...] (dev): that shader's colour is the
    // expression (a vec3; vin[k] are its interpolators, oc0 its colour, and in a player's shader hcN its normal), to
    // see what its inputs hold.
    if (const char* e = Hc09Env("HC09_SHADER_EXPR"))
    {
        std::string all = e;
        for (size_t at = 0; at < all.size();)
        {
            size_t end = all.find(';', at);
            if (end == std::string::npos) end = all.size();
            const std::string item = all.substr(at, end - at);
            at = end + 1;
            const size_t eq = item.find('=');
            if (eq != std::string::npos && strtoull(item.substr(0, eq).c_str(), nullptr, 16) == hash)
                return Insert(glsl, "    oc0.rgb = vec3(" + item.substr(eq + 1) + ");\n") || patched;
        }
    }
    return patched;
}

bool MaterialDrawValue(uint64_t psHash, const uint32_t (*fetch)[6], uint32_t slots, float& value)
{
    if (psHash != kTurfPs) return false;
    // The field's markings (slot 3) tell one stadium and game from the next.
    const uint32_t texture = (slots & 8u) ? fetch[3][1] & 0xFFFFF000u : 0u;
    const auto now = std::chrono::steady_clock::now();
    const double gap = turf.last.time_since_epoch().count() ? std::chrono::duration<double>(now - turf.last).count() : 1e9;
    const uint32_t game = hc09::g_gamesStarted;
    if (texture != turf.field || game != turf.game || gap > 300.0)
    {
        turf.field = texture;
        turf.game = game;
        turf.seconds = 0.0;
        // HC09_TURF_LOG=1 (dev): the content hashes of the field's textures, slots 2 to 6.
        if (Hc09Env("HC09_TURF_LOG"))
        {
            fprintf(stderr, "[turf] field");
            for (uint32_t s = 2; s <= 6; ++s)
            {
                uint64_t h = 0;
                if ((slots >> s) & 1u && hc09::gpu::TextureContentHash(fetch[s], h)) fprintf(stderr, " %u:%016llX", s, (unsigned long long)h);
            }
            fprintf(stderr, "\n");
        }
    }
    else
        turf.seconds += std::min(gap, 0.1);
    turf.last = now;
    // The wear grows with the time the field is on screen: none at kickoff, all of it after 40 minutes (about a
    // game), on grass only. A new game, a new field or 5 minutes without the field start over.
    // HC09_TURF_WEAR=<0..1> (dev): a fixed amount.
    static const float fixed = [] { const char* e = Hc09Env("HC09_TURF_WEAR"); return e && *e ? float(atof(e)) : -1.0f; }();
    value = fixed >= 0.0f ? fixed : float(std::min(1.0, turf.seconds / 2400.0));
    if (hc09::g_fieldTurf == 1) value = 0.0f;   // artificial turf never wears
    return true;
}

}  // namespace hc09vk
