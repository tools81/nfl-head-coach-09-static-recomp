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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../dev_env.h"
#include "common/field_surface.h"
#include "common/weather.h"
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

// The turf's value for the draw (D.pad2, MaterialDrawValue): its fraction how worn the field is (0..1), its whole part
// how wet (rain) or snowed on in percent, plus 128 for snow, 512 at night and 1024 when the footprints in the snow are
// bound (kFootprintSlot: how pressed each spot is, 60 x 120 yards from the field's middle).
const char* kTurfValue = R"GLSL(
float hcWorn() { return fract(D.pad2); }
bool hcFootprintsBound() { return D.pad2 >= 1024.0; }
float hcCode() { float w = floor(D.pad2); return w >= 1024.0 ? w - 1024.0 : w; }
bool hcNight() { return hcCode() >= 512.0; }
float hcWeatherCode() { float w = hcCode(); return w >= 512.0 ? w - 512.0 : w; }
float hcFootprint(vec3 p)
{
    if (!hcFootprintsBound()) return 0.0;
    return textureLod(tex2d[30], p.xz / vec2(60.0, 120.0) + 0.5, 0.0).r;
}
bool hcSnowing() { return hcWeatherCode() >= 128.0; }
float hcWetness() { return hcSnowing() ? 0.0 : hcWeatherCode() / 100.0; }
float hcSnowCover() { return hcSnowing() ? (hcWeatherCode() - 128.0) / 100.0 : 0.0; }
)GLSL";

// The turf: `c` the game's colour, `p` the world position, `eye` the camera's.
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
    float worn = hcWorn();
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

// Rain on the field (roadmap 16), for the day's and the night's field shaders: `c` the colour so far, `p` the world
// position, `eye` the camera's. Water fills the gaps between the blades, so less light scatters back (darker, deeper
// grass) and the film reflects the sky, by the angle (Fresnel), broken up by the blades. Where the ground is low, and
// where the turf is worn to soil, water stands: flat, a mirror of the sky dimpled by the drops, the grass under it
// dark. The puddles spread as the field gets wetter.
const char* kWet = R"GLSL(
vec3 hcWet(vec3 c, vec3 p, vec3 eye)
{
    float wet = hcWetness();
    if (wet <= 0.0) return c;
    float fw = max(max(abs(dFdx(p.x)), abs(dFdy(p.x))), max(abs(dFdx(p.z)), abs(dFdy(p.z))));
    vec3 toEye = eye - p;
    vec3 v = length(toEye) > 1e-4 ? normalize(toEye) : vec3(0.0, 1.0, 0.0);
    vec2 q = p.xz;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    float grass = smoothstep(0.004, 0.03, c.g - max(c.r, c.b));
    // Overcast by day, the floodlit night above the stands.
    vec3 sky = hcNight() ? vec3(0.13, 0.14, 0.16) : vec3(0.58, 0.61, 0.66);

    vec3 col = c * mix(1.0, mix(0.86, 0.7, grass), wet);
    col = mix(vec3(dot(col, vec3(0.299, 0.587, 0.114))), col, 1.0 + 0.2 * wet * grass);
    float nv = clamp(v.y, 0.0, 1.0);
    float film = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
    float blades = mix(0.55, 1.0, hcShow(0.05, fw) * hcNoise(q / 0.05 + 3.0) + (1.0 - hcShow(0.05, fw)) * 0.5);
    col += sky * film * wet * 0.35 * blades;

    // Standing water: in the low patches, more where the turf is worn (between the hash marks, in the middle).
    float worn = hcWorn();
    float band = exp(-(p.x * p.x) / 40.0) * smoothstep(50.0, 35.0, abs(p.z));
    float low = hcFbm(q / 2.5 + 71.0) + 0.1 * hcNoise(q / 0.6 + 9.0) + 0.15 * band * worn;
    float cover = clamp((wet - 0.45) / 0.55, 0.0, 1.0);
    // Shallow at the rim, where blades still break the surface; deeper and whole toward the middle.
    float level = 0.82 - 0.07 * cover;
    float depth = smoothstep(level, level + 0.07, low);
    float tufts = hcShow(0.04, fw) * hcNoise(q / 0.04 + 51.0) + (1.0 - hcShow(0.04, fw)) * 0.5;
    float puddle = clamp(depth * 1.6 - tufts * (1.0 - depth) * 0.9, 0.0, 1.0) * smoothstep(level - max(fw * 0.4, 0.01), level, low) * cover;
    if (puddle > 0.0)
    {
        // The drops' dimples tilt the water a little: the reflection's angle varies across it. The sky is far
        // brighter than the grass it lights, so even the little a puddle mirrors from above reads as grey sheen.
        vec2 dimple = (vec2(hcNoise(q / 0.06), hcNoise(q / 0.06 + 23.0)) - 0.5) * 0.25 * hcShow(0.06, fw);
        vec3 n = normalize(vec3(dimple.x, 1.0, dimple.y));
        float wv = clamp(dot(n, v), 0.0, 1.0);
        float mirror = 0.02 + 0.98 * pow(1.0 - wv, 5.0);
        // The grass under the water, its blades drowned: darker and even.
        vec3 under = c * 0.5;
        under = mix(under, vec3(dot(under, vec3(0.333))) * vec3(0.9, 1.0, 0.85), 0.3);
        // An overcast sky is brightest overhead.
        vec3 r = reflect(-v, n);
        vec3 water = under + sky * (hcNight() ? 1.5 : 2.4) * mirror * mix(0.75, 1.15, clamp(r.y, 0.0, 1.0)) + sky * 0.05 * depth;
        // By night the floodlight towers, beyond the field's corners about 40 degrees up, glint in the water.
        if (hcNight())
            for (int k = 0; k < 4; ++k)
            {
                vec3 tower = vec3((k & 1) != 0 ? 48.0 : -48.0, 52.0, (k & 2) != 0 ? 70.0 : -70.0);
                float glint = pow(clamp(dot(r, normalize(tower - p)), 0.0, 1.0), 400.0);
                water += vec3(1.0, 0.97, 0.9) * glint * 1.5 * depth;
            }
        col = mix(col, water, puddle * 0.8);
    }
    return col;
}
)GLSL";

// Snow settling on the field (roadmap 16), for both field shaders, over what the game draws (its snowy field: the
// grass a dull beige, banks along the sidelines). Drifts in patches that join up as it settles; between the hash
// marks, where the play is, it is trampled to grey slush with the ground showing through; the yard lines are kept
// swept. Lit as the game lit the ground under it (the shadows of players fall on it), with a fine grain and glints.
const char* kSnow = R"GLSL(
vec3 hcSnow(vec3 c, vec3 p, vec3 eye)
{
    float amount = hcSnowCover();
    if (amount <= 0.0) return c;
    float fw = max(max(abs(dFdx(p.x)), abs(dFdy(p.x))), max(abs(dFdx(p.z)), abs(dFdy(p.z))));
    vec3 toEye = eye - p;
    vec3 v = length(toEye) > 1e-4 ? normalize(toEye) : vec3(0.0, 1.0, 0.0);
    vec2 q = p.xz;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    // The light on the ground, against the game's snowy field fully lit (about 0.55).
    float light = mix(clamp(lum / 0.55, 0.4, 1.15), 1.0, 0.3);
    float paint = smoothstep(0.68, 0.78, lum);

    // The drifts' pattern turned off the field's axes and warped (the noise's grid showed as square steps in their
    // edges), with soft edges where the snow thins out.
    vec2 r = mat2(0.8, -0.6, 0.6, 0.8) * q;
    r += 1.6 * (vec2(hcNoise(r / 2.3 + 11.0), hcNoise(r / 2.3 + 29.0)) - 0.5);
    float drift = hcFbm(r / 3.2 + 101.0) + 0.14 * (hcNoise(mat2(0.6, 0.8, -0.8, 0.6) * q / 0.45 + 7.0) - 0.5) * hcShow(0.45, fw);
    float cover = smoothstep(0.0, 0.3, drift - (1.05 - 1.1 * amount));
    // Trampled between the hash marks, most in the middle of the field and as the game wears on.
    float band = exp(-(p.x * p.x) / 45.0) * smoothstep(52.0, 30.0, abs(p.z));
    float trample = clamp(band * (0.7 + 0.3 * hcWorn()), 0.0, 1.0);
    float prints = hcShow(0.25, fw) * hcNoise(q / 0.25 + 33.0);
    cover *= 1.0 - trample * mix(0.55, 0.9, prints);
    // The yard lines kept swept (white on white would vanish): a strip of ground along each 5-yard line, and the
    // paint itself.
    float toLine = abs(fract(p.z / 5.0 + 0.5) - 0.5) * 5.0;
    float swept = (1.0 - smoothstep(0.3, 0.5 + fw, toLine)) * step(abs(p.z), 50.5) * step(abs(p.x), 26.7);
    cover *= 1.0 - 0.85 * max(swept, paint);

    float grain = 0.94 + 0.06 * (hcShow(0.05, fw) * hcNoise(q / 0.05 + 3.0) + (1.0 - hcShow(0.05, fw)) * 0.5);
    vec3 snow = vec3(0.8, 0.82, 0.87) * light * grain;
    snow = mix(snow, mix(c * 0.75, snow * 0.72, 0.55), trample * 0.7);   // slush
    // Glints: a few crystals catch the light toward the eye, only where a pixel is small enough to show one.
    float glint = step(0.985, hcHash(floor(q / 0.03))) * hcShow(0.03, fw) * pow(clamp(v.y, 0.0, 1.0), 0.5) * (1.0 - trample);
    snow += vec3(0.25) * glint * light;
    // Footprints: where something stood, the snow pressed down: a shaded hollow, thinner, packed grey at the bottom.
    float print = clamp(hcFootprint(p), 0.0, 1.0);
    snow = mix(snow, snow * vec3(0.66, 0.68, 0.74), print);
    cover *= 1.0 - 0.3 * print;
    return mix(c, snow, clamp(cover, 0.0, 1.0));
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
    if (hash == kTurfPs || hash == kTurfNightPs)
    {
        // The day's field gets the turf (HC09_TURF) and the weather; the night's, the weather alone.
        static const float strength = Setting(Hc09Env("HC09_TURF"), 1.0f);
        const bool turfToo = hash == kTurfPs && strength > 0.0f;
        const size_t mainAt = glsl.find("void main()");
        if (mainAt != std::string::npos && glsl.rfind(kEpilogue) != std::string::npos)
        {
            glsl.insert(mainAt, std::string("// hc09: the turf (vk_materials.cpp).") + kNoise + kTurfValue + (turfToo ? kTurf : "") + kWet + kSnow + "\n");
            char call[200];
            if (turfToo)
                snprintf(call, sizeof(call),
                         "    oc0.rgb = hcSnow(hcWet(hcTurf(oc0.rgb, vin[2].xyz, vin[3].xyz, %.3f), vin[2].xyz, vin[3].xyz), vin[2].xyz, vin[3].xyz);\n",
                         strength);
            else
                snprintf(call, sizeof(call), "    oc0.rgb = hcSnow(hcWet(oc0.rgb, vin[2].xyz, vin[3].xyz), vin[2].xyz, vin[3].xyz);\n");
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

namespace {
// The turf's value as kTurfValue reads it: the wear, how wet the field is and whether it is night.
float TurfValue(float wear)
{
    // In rain the field is wet from the kickoff and gets wetter over the first 20 minutes it is on screen.
    // HC09_TURF_WET=<0..1> (dev): a fixed amount, whatever the weather.
    static const float fixedWet = [] { const char* e = Hc09Env("HC09_TURF_WET"); return e && *e ? std::clamp(float(atof(e)), 0.0f, 1.0f) : -1.0f; }();
    // In snow it settles: a thin cover at the kickoff, most of the field white after 25 minutes on screen.
    // HC09_TURF_SNOW=<0..1> (dev): a fixed amount, whatever the weather.
    static const float fixedSnow = [] { const char* e = Hc09Env("HC09_TURF_SNOW"); return e && *e ? std::clamp(float(atof(e)), 0.0f, 1.0f) : -1.0f; }();
    float wet = 0.0f, snow = -1.0f;
    if (fixedSnow >= 0.0f)
        snow = fixedSnow;
    else if (fixedWet >= 0.0f)
        wet = fixedWet;
    else if (hc09::g_weather == int(hc09::Weather::Rain))
        wet = float(0.55 + 0.45 * std::min(1.0, turf.seconds / 1200.0));
    else if (hc09::g_weather == int(hc09::Weather::Snow))
        snow = float(0.4 + 0.6 * std::min(1.0, turf.seconds / 1500.0));
    const float code = snow >= 0.0f ? 128.0f + std::floor(snow * 100.0f + 0.5f) : std::floor(wet * 100.0f + 0.5f);
    return code + (hc09::g_nightGame ? 512.0f : 0.0f) + std::clamp(wear, 0.0f, 0.999f);
}
}  // namespace

bool MaterialDrawValue(uint64_t psHash, const uint32_t (*fetch)[6], uint32_t slots, float& value)
{
    if (psHash != kTurfPs && psHash != kTurfNightPs) return false;
    // The field's markings (slot 3) tell one stadium and game from the next. The night's field shader lacks a
    // texture: its draws keep the field the day's last saw.
    const uint32_t texture = psHash == kTurfNightPs ? turf.field : (slots & 8u) ? fetch[3][1] & 0xFFFFF000u : 0u;
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
    float wear = fixed >= 0.0f ? fixed : float(std::min(1.0, turf.seconds / 2400.0));
    if (hc09::g_fieldTurf == 1) wear = 0.0f;   // artificial turf never wears
    value = TurfValue(wear);
    return true;
}

}  // namespace hc09vk
