// hc09: the score bar; see score_bar.h.
//
// A ribbon at the foot of the picture, after a TV broadcast's (see ScoreBarBuild). Its text is Barlow
// Condensed (fonts/, SIL Open Font License), rasterized with stb_truetype at the size the bar has in the window.

#include "score_bar.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "common/broadcast_state.h"
#include "gpu_vk/vk_replay.h"
#include "gpu_vk/vk_swapchain.h"
#include "xenos_texture.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace
{
    using hc09vk::OverlayVtx;

    constexpr int kFirst = 32, kCount = 95;  // the printable ASCII characters

    struct Face
    {
        float px = 0;              // its pixel height
        float cap = 0;             // a capital's height, pixels
        float scale = 0;           // font units to pixels
        stbtt_packedchar chars[kCount];
    };

    struct Font
    {
        bool tried = false, ok = false;
        std::vector<uint8_t> ttf;
        stbtt_fontinfo info{};
        int barHeight = 0;         // what the atlas was made for
        Face big, small, tiny;
        std::vector<uint8_t> atlas;
        int width = 0, height = 0;
        uint64_t version = 0;
    } g_font;

    bool Load()
    {
        if (g_font.tried)
            return g_font.ok;
        g_font.tried = true;
        std::ifstream file(hc09::broadcast::kFontFile, std::ios::binary);
        g_font.ttf.assign(std::istreambuf_iterator<char>(file), {});
        g_font.ok = !g_font.ttf.empty() && stbtt_InitFont(&g_font.info, g_font.ttf.data(), stbtt_GetFontOffsetForIndex(g_font.ttf.data(), 0));
        if (!g_font.ok)
            std::fprintf(stderr, "[scorebar] %s could not be read: no score bar\n", hc09::broadcast::kFontFile);
        return g_font.ok;
    }

    // The atlas for a bar `barHeight` pixels high: the score and clock size, the labels' size, and the Coaching
    // Advantage's.
    bool Bake(int barHeight)
    {
        if (g_font.barHeight == barHeight)
            return true;
        Face* faces[3] = { &g_font.big, &g_font.small, &g_font.tiny };
        const float sizes[3] = { barHeight * 0.70f, barHeight * 0.54f, barHeight * 0.40f };
        for (int height = 256; height <= 4096; height *= 2)
        {
            const int width = 1024;
            std::vector<uint8_t> atlas(size_t(width) * height);
            stbtt_pack_context pc;
            if (!stbtt_PackBegin(&pc, atlas.data(), width, height, 0, 1, nullptr))
                return false;
            stbtt_PackSetOversampling(&pc, 1, 1);
            stbtt_pack_range ranges[3] = {};
            for (int i = 0; i < 3; ++i)
            {
                ranges[i].font_size = sizes[i];
                ranges[i].first_unicode_codepoint_in_range = kFirst;
                ranges[i].num_chars = kCount;
                ranges[i].chardata_for_range = faces[i]->chars;
            }
            const bool packed = stbtt_PackFontRanges(&pc, g_font.ttf.data(), 0, ranges, 3) != 0;
            stbtt_PackEnd(&pc);
            if (!packed)
                continue;
            int x0, y0, x1, y1;
            stbtt_GetCodepointBox(&g_font.info, 'H', &x0, &y0, &x1, &y1);
            for (int i = 0; i < 3; ++i)
            {
                faces[i]->px = sizes[i];
                faces[i]->scale = stbtt_ScaleForPixelHeight(&g_font.info, sizes[i]);
                faces[i]->cap = y1 * faces[i]->scale;
            }
            g_font.atlas = std::move(atlas);
            g_font.width = width;
            g_font.height = height;
            g_font.barHeight = barHeight;
            ++g_font.version;
            return true;
        }
        return false;
    }

    // An atlas slot's size (the logos are fitted into it) and the space around it (nothing bleeds in when filtered).
    constexpr uint32_t kSlotW = 256, kSlotH = 128, kBorder = 2;

    // The atlas's height: the font's, then two rows of pictures (Compose).
    int AtlasHeight() { return g_font.height + 2 * int(kSlotH + 2 * kBorder); }

    uint32_t Rgba(uint32_t rgb, float alpha)
    {
        const uint32_t a = uint32_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
        return ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | (a << 24);
    }

    void Rect(std::vector<OverlayVtx>& out, float x0, float y0, float x1, float y1, uint32_t rgba)
    {
        const OverlayVtx a{ x0, y0, rgba }, b{ x1, y0, rgba }, c{ x1, y1, rgba }, d{ x0, y1, rgba };
        out.insert(out.end(), { a, b, c, a, c, d });
    }

    int Index(char c) { return (c >= kFirst && c < kFirst + kCount) ? c - kFirst : '?' - kFirst; }

    float Kern(const Face& face, char a, char b) { return stbtt_GetCodepointKernAdvance(&g_font.info, a, b) * face.scale; }

    float Width(const Face& face, const std::string& s)
    {
        float w = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            w += face.chars[Index(s[i])].xadvance;
            if (i + 1 < s.size())
                w += Kern(face, s[i], s[i + 1]);
        }
        return w;
    }

    // `s` with its left edge at x and its capitals centred on y.
    void Text(std::vector<OverlayVtx>& out, const Face& face, const std::string& s, float x, float y, uint32_t rgba)
    {
        float pen = std::round(x);
        float baseline = std::round(y + face.cap / 2);
        for (size_t i = 0; i < s.size(); ++i)
        {
            stbtt_aligned_quad q;
            stbtt_GetPackedQuad(face.chars, g_font.width, AtlasHeight(), Index(s[i]), &pen, &baseline, &q, 1);
            if (s[i] != ' ')
            {
                const OverlayVtx a{ q.x0, q.y0, rgba, q.s0, q.t0 }, b{ q.x1, q.y0, rgba, q.s1, q.t0 }, c{ q.x1, q.y1, rgba, q.s1, q.t1 },
                    d{ q.x0, q.y1, rgba, q.s0, q.t1 };
                out.insert(out.end(), { a, b, c, a, c, d });
            }
            if (i + 1 < s.size())
                pen += Kern(face, s[i], s[i + 1]);
        }
    }

    std::string Trim(const std::string& s)
    {
        const size_t a = s.find_first_not_of(' ');
        return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(' ') - a + 1);
    }

    // Light text on dark colours, dark on light ones.
    uint32_t TextOn(uint32_t rgb)
    {
        const float l = 0.2126f * ((rgb >> 16) & 0xFF) + 0.7152f * ((rgb >> 8) & 0xFF) + 0.0722f * (rgb & 0xFF);
        return l > 160.0f ? 0x14181Eu : 0xF4F6F8u;
    }


    // ---- the teams' logos ---------------------------------------------------------------------------------------

    // The game's team logos, by team (TGID 1 .. 32): its 128x128 "small" ones, swappable images in qkl_boot.ast in
    // team order, by content hash (gpu/texture_replacements.h). The bar takes them as the renderer decodes them for
    // the game's own screens (after any replacement, so HC27's new logos too).
    constexpr uint64_t kLogos[32] = {
        0xB18615265D8E92B7, 0x3D87516609E9209A, 0xD870BA40EF7AB5DB, 0xA2C57BF042160006, 0x4D0C2360B3F896FA, 0x7D3CCAEF042B0A77,
        0xB5351947433DF688, 0x5695BF9684F8886E, 0x790DA51D0A004450, 0x801F1B48D4F29B36, 0x72C0C8A00E48547F, 0xFC1881A7129AA603,
        0x99B9865814C0069E, 0xBB973DC788C0476C, 0xDAEE42605EE09C35, 0x0ABD07BFCCDF215F, 0xD818446A0440AA2A, 0x4849D2A237EDAA3F,
        0x3E2FF804292AC0E5, 0x3AE5FAF75E3A07D5, 0x0DD6777226F82FAA, 0x323F2D2D5B528374, 0x762A85F720265012, 0xB297981FB4B83143,
        0x94AA8E58348087DA, 0x181C216DFB917D7C, 0x2061F69B5A560CB2, 0x11BF384F47C617DB, 0xB218C4B9EE8AD7B9, 0x0D0D085CBBBE58AE,
        0x7A916E0854398C66, 0xEDC7E00CF6209B8E,
    };

    // The clipboard's score box's backdrop (a dark grid) and frame, 256x128, by content hash.
    constexpr uint64_t kBoxBackdrop = 0x07BAB00AF7117A49, kBoxFrame = 0xD92C2DA9F9A54C95;
    constexpr int kBackdrop = 33, kFrame = 34;   // their pictures' numbers (the teams' logos: 1 .. 32)

    // A picture's number (0: none).
    int TeamOf(uint64_t hash)
    {
        for (int i = 0; i < 32; ++i)
            if (kLogos[i] == hash)
                return i + 1;
        return hash == kBoxBackdrop ? kBackdrop : hash == kBoxFrame ? kFrame : 0;
    }

    struct Logo
    {
        uint32_t width = 0, height = 0;
        std::vector<uint32_t> rgba;  // premultiplied, cropped to what is not transparent
        uint64_t version = 0;
    };
    std::mutex g_logoMutex;
    Logo g_logos[35];   // the pictures, by number

    // `src` (premultiplied) fitted into w x h by averaging the texels each covers.
    void Resize(const Logo& src, uint32_t w, uint32_t h, std::vector<uint32_t>& out)
    {
        out.assign(size_t(w) * h, 0);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const float fx0 = float(x) * src.width / w, fx1 = float(x + 1) * src.width / w;
                const float fy0 = float(y) * src.height / h, fy1 = float(y + 1) * src.height / h;
                float sum[4] = {}, weight = 0;
                for (uint32_t sy = uint32_t(fy0); sy < std::min(src.height, uint32_t(std::ceil(fy1))); ++sy)
                    for (uint32_t sx = uint32_t(fx0); sx < std::min(src.width, uint32_t(std::ceil(fx1))); ++sx)
                    {
                        const float wx = std::min(fx1, float(sx + 1)) - std::max(fx0, float(sx));
                        const float wy = std::min(fy1, float(sy + 1)) - std::max(fy0, float(sy));
                        const float k = std::max(0.0f, wx) * std::max(0.0f, wy);
                        const uint32_t p = src.rgba[size_t(sy) * src.width + sx];
                        for (int c = 0; c < 4; ++c)
                            sum[c] += k * ((p >> (8 * c)) & 0xFF);
                        weight += k;
                    }
                uint32_t p = 0;
                for (int c = 0; c < 4; ++c)
                    p |= uint32_t(std::clamp(sum[c] / std::max(weight, 1e-6f) + 0.5f, 0.0f, 255.0f)) << (8 * c);
                out[size_t(y) * w + x] = p;
            }
    }

    // The atlas: the font's coverage as premultiplied white, then two rows of two slots: the teams' logos (away, home),
    // the score box's backdrop and frame.
    struct Atlas
    {
        uint64_t fontVersion = ~0ull, versions[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
        uint32_t pictures[4] = { ~0u, ~0u, ~0u, ~0u };
        float logoUv[4][4] = {};     // u0, v0, u1, v1 of each slot's picture
        float logoAspect[4] = {};    // its width / height (0: none)
        uint64_t version = 0;
    } g_atlas;

    void Compose(hc09vk::Swapchain& swapchain, const uint32_t teams[2])
    {
        const uint32_t pictures[4] = { teams[0], teams[1], kBackdrop, kFrame };
        uint64_t versions[4];
        {
            std::lock_guard<std::mutex> lock(g_logoMutex);
            for (int i = 0; i < 4; ++i)
                versions[i] = pictures[i] >= 1 && pictures[i] <= 34 ? g_logos[pictures[i]].version : 0;
        }
        bool same = g_atlas.fontVersion == g_font.version && swapchain.overlayAtlasVersion == g_atlas.version;
        for (int i = 0; i < 4; ++i)
            same = same && g_atlas.pictures[i] == pictures[i] && g_atlas.versions[i] == versions[i];
        if (same)
            return;
        const uint32_t width = uint32_t(g_font.width), height = uint32_t(AtlasHeight());
        std::vector<uint32_t> atlas(size_t(width) * height, 0);
        for (size_t i = 0; i < g_font.atlas.size(); ++i)
            atlas[i] = g_font.atlas[i] * 0x01010101u;
        for (int i = 0; i < 4; ++i)
        {
            g_atlas.logoAspect[i] = 0;
            Logo logo;
            {
                std::lock_guard<std::mutex> lock(g_logoMutex);
                if (pictures[i] >= 1 && pictures[i] <= 34)
                    logo = g_logos[pictures[i]];
            }
            if (!logo.width || !logo.height)
                continue;
            const float scale = std::min({ 1.0f, float(kSlotW) / logo.width, float(kSlotH) / logo.height });
            const uint32_t w = std::max(1u, uint32_t(logo.width * scale + 0.5f)), h = std::max(1u, uint32_t(logo.height * scale + 0.5f));
            std::vector<uint32_t> fitted;
            Resize(logo, w, h, fitted);
            const uint32_t ox = kBorder + (i % 2) * (kSlotW + 2 * kBorder), oy = uint32_t(g_font.height) + kBorder + (i / 2) * (kSlotH + 2 * kBorder);
            for (uint32_t y = 0; y < h; ++y)
                std::copy_n(&fitted[size_t(y) * w], w, &atlas[size_t(oy + y) * width + ox]);
            g_atlas.logoUv[i][0] = float(ox) / width;
            g_atlas.logoUv[i][1] = float(oy) / height;
            g_atlas.logoUv[i][2] = float(ox + w) / width;
            g_atlas.logoUv[i][3] = float(oy + h) / height;
            g_atlas.logoAspect[i] = float(w) / h;
        }
        g_atlas.fontVersion = g_font.version;
        for (int i = 0; i < 4; ++i)
            g_atlas.pictures[i] = pictures[i], g_atlas.versions[i] = versions[i];
        g_atlas.version = swapchain.overlayAtlasVersion + 1;
        swapchain.overlayAtlas = std::move(atlas);
        swapchain.overlayAtlasW = width;
        swapchain.overlayAtlasH = height;
        swapchain.overlayAtlasVersion = g_atlas.version;
    }

}

bool ScoreBarWantsTexture(uint64_t hash) { return hc09::broadcast::Enabled() && TeamOf(hash) != 0; }

void ScoreBarOfferTexture(uint64_t hash, const xenos::Image& image)
{
    const int team = TeamOf(hash);
    if (!team || !image.width || !image.height || image.rgba.size() < size_t(image.width) * image.height)
        return;
    // Cropped to what is not (nearly) transparent, premultiplied.
    uint32_t x0 = image.width, y0 = image.height, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < image.height; ++y)
        for (uint32_t x = 0; x < image.width; ++x)
            if ((image.rgba[size_t(y) * image.width + x] >> 24) > 8)
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    if (x1 <= x0 || y1 <= y0)
        return;
    Logo logo;
    logo.width = x1 - x0;
    logo.height = y1 - y0;
    logo.rgba.resize(size_t(logo.width) * logo.height);
    for (uint32_t y = 0; y < logo.height; ++y)
        for (uint32_t x = 0; x < logo.width; ++x)
        {
            const uint32_t p = image.rgba[size_t(y0 + y) * image.width + x0 + x], a = p >> 24;
            uint32_t q = a << 24;
            for (int c = 0; c < 3; ++c)
                q |= ((((p >> (8 * c)) & 0xFF) * a + 127) / 255) << (8 * c);
            logo.rgba[size_t(y) * logo.width + x] = q;
        }
    std::lock_guard<std::mutex> lock(g_logoMutex);
    Logo& slot = g_logos[team];
    if (slot.width == logo.width && slot.height == logo.height && slot.rgba == logo.rgba)
        return;
    logo.version = slot.version + 1;
    slot = std::move(logo);
}


namespace
{
    // ---- drawing ------------------------------------------------------------------------------------------------

    // A colour times f (darker below 1), and mixed toward white by t.
    uint32_t Shade(uint32_t rgb, float f)
    {
        uint32_t out = 0;
        for (int c = 0; c < 3; ++c)
            out |= uint32_t(std::clamp(((rgb >> (8 * c)) & 0xFF) * f + 0.5f, 0.0f, 255.0f)) << (8 * c);
        return out;
    }
    uint32_t Mix(uint32_t rgb, uint32_t to, float t)
    {
        uint32_t out = 0;
        for (int c = 0; c < 3; ++c)
        {
            const float a = float((rgb >> (8 * c)) & 0xFF), b = float((to >> (8 * c)) & 0xFF);
            out |= uint32_t(a + (b - a) * t + 0.5f) << (8 * c);
        }
        return out;
    }

    // A rectangle with a colour at each corner (top left, top right, bottom right, bottom left).
    void Quad(std::vector<OverlayVtx>& out, float x0, float y0, float x1, float y1, uint32_t tl, uint32_t tr, uint32_t br, uint32_t bl)
    {
        const OverlayVtx a{ x0, y0, tl }, b{ x1, y0, tr }, c{ x1, y1, br }, d{ x0, y1, bl };
        out.insert(out.end(), { a, b, c, a, c, d });
    }

    void Triangle(std::vector<OverlayVtx>& out, float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgba)
    {
        out.insert(out.end(), { OverlayVtx{ ax, ay, rgba }, OverlayVtx{ bx, by, rgba }, OverlayVtx{ cx, cy, rgba } });
    }

    // Text with a soft shadow under it.
    void ShadowedText(std::vector<OverlayVtx>& out, const Face& face, const std::string& s, float x, float y, uint32_t rgb, float opacity,
                      float shadow)
    {
        Text(out, face, s, x + shadow, y + shadow, Rgba(0x000000, 0.55f * opacity));
        Text(out, face, s, x, y, Rgba(rgb, opacity));
    }

    std::string Upper(std::string s)
    {
        for (char& c : s)
            if (c >= 'a' && c <= 'z')
                c = char(c - 'a' + 'A');
        return s;
    }

    void Logo(std::vector<OverlayVtx>& out, int side, float x, float y, float w, float h, float opacity)
    {
        const float* uv = g_atlas.logoUv[side];
        const uint32_t rgba = Rgba(0xFFFFFF, opacity);
        const OverlayVtx a{ x, y, rgba, uv[0], uv[1] }, b{ x + w, y, rgba, uv[2], uv[1] }, c{ x + w, y + h, rgba, uv[2], uv[3] },
            d{ x, y + h, rgba, uv[0], uv[3] };
        out.insert(out.end(), { a, b, c, a, c, d });
    }
}

// The ribbon, after a TV broadcast's: a panel per team (its timeouts as three pips, its logo, its score on its colour),
// a red arrow at the side of the team with the ball, then the quarter, the clock and the play clock, then the down and
// distance (the ball's spot after it, smaller), centred near the bottom of the picture.
namespace
{
    // The clipboard's score box covered with the Coaching Advantage: the box's backdrop and frame (the game's), then,
    // while the game shows its panel, its title and its rows (a label between the away team's bar, filled from the
    // middle out in its colour, and the home team's). Where the box is: x 909 .. 1169, y 35 .. 158 of the game's
    // 1280 x 720 menus (in the 16:9 middle of a wider picture).
    // When there is no Coaching Advantage to show, the last play loops in the box instead, from its start each time
    // the box appears or a new play is kept (vk_replay.h).
    void Box(hc09vk::Swapchain& swapchain, const hc09::broadcast::Scoreboard& board, float x0, float y0, float w, float h)
    {
        std::vector<OverlayVtx>& out = swapchain.overlay;
        const float k = h / 720.0f;
        const float hudX = x0 + (w - h * 16.0f / 9.0f) / 2;
        const float bx0 = std::round(hudX + 909.0f * k), bx1 = std::round(hudX + 1169.0f * k);
        const float by0 = std::round(y0 + 35.0f * k), by1 = std::round(y0 + 158.0f * k);
        // The backdrop and the frame (slots 2 and 3); a plain dark box until the game has drawn them.
        if (g_atlas.logoAspect[2] > 0)
            Logo(out, 2, bx0, by0, bx1 - bx0, by1 - by0, 1.0f);
        else
            Rect(out, bx0, by0, bx1, by1, Rgba(0x10161E, 1.0f));
        static std::chrono::steady_clock::time_point replayStart{};
        static uint64_t replayPlay = ~0ull;
        static std::chrono::steady_clock::time_point lastShown{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastShown > std::chrono::milliseconds(250) || replayPlay != board.play)   // the box has just appeared
            replayStart = now, replayPlay = board.play;
        lastShown = now;
        uint32_t layer = 0;
        if (!board.coaching && hc09vk::ReplayFrameAt(std::chrono::duration<double>(now - replayStart).count(), layer))
        {
            // The play, inside the frame's border, with a small label in its corner.
            const float inset = std::max(2.0f, std::round(4.0f * k));
            const float rx0 = bx0 + inset, ry0 = by0 + inset, rx1 = bx1 - inset, ry1 = by1 - inset;
            swapchain.overlayReplayLayer = float(layer);
            const uint32_t white = Rgba(0xFFFFFF, 1.0f);
            const OverlayVtx a{ rx0, ry0, white, 2.0f, 0.0f }, b{ rx1, ry0, white, 3.0f, 0.0f }, c{ rx1, ry1, white, 3.0f, 1.0f },
                d{ rx0, ry1, white, 2.0f, 1.0f };
            out.insert(out.end(), { a, b, c, a, c, d });
            const Face& face = g_font.tiny;
            const std::string label = "REPLAY";
            const float pad = std::round(face.px * 0.35f), lw = Width(face, label);
            const float lx0 = rx0 + pad, ly0 = ry0 + pad, lx1 = lx0 + lw + 2 * pad, ly1 = ly0 + std::round(face.cap + 2 * pad);
            Rect(out, lx0, ly0, lx1, ly1, Rgba(0xC8102E, 0.92f));
            Text(out, face, label, lx0 + pad, (ly0 + ly1) / 2, Rgba(0xFFFFFF, 1.0f));
        }
        if (g_atlas.logoAspect[3] > 0)
            Logo(out, 3, bx0, by0, bx1 - bx0, by1 - by0, 1.0f);
        if (!board.coaching)
            return;

        // The title, and the rows the game shows, spread down the box.
        const Face& face = g_font.tiny;
        const float bw = bx1 - bx0, bh = by1 - by0, cx = bx0 + bw / 2;
        const float shadow = std::max(1.0f, std::round(face.px / 16));
        const std::string coaching = "COACHING", advantage = " ADVANTAGE";
        const float tw = Width(face, coaching) + Width(face, advantage);
        const float titleY = by0 + bh * 0.17f;
        ShadowedText(out, face, coaching, cx - tw / 2, titleY, 0xF6F7F8, 1.0f, shadow);
        ShadowedText(out, face, advantage, cx - tw / 2 + Width(face, coaching), titleY, 0xB9C0C9, 1.0f, shadow);
        static const char* kLabels[4] = { "OVERALL", "COACH", "KNOWLEDGE", "GAME PLAN" };
        int rows[4], n = 0;
        for (int r = 0; r < 4; ++r)
            if (board.rowShown[r])
                rows[n++] = r;
        if (!n)
            return;
        float labelW = 0;
        for (int i = 0; i < n; ++i)
            labelW = std::max(labelW, Width(face, kLabels[rows[i]]));
        const float gap = std::round(bw * 0.025f), margin = std::round(bw * 0.04f);
        const float barW = std::round((bw - 2 * margin - labelW - 2 * gap) / 2), barH = std::max(3.0f, std::round(bh * 0.10f));
        const float rowTop = by0 + bh * 0.33f, rowStep = (by1 - bh * 0.08f - rowTop) / n;
        const uint32_t colours[2] = { board.away.rgb ? board.away.rgb : 0xB02A30, board.home.rgb ? board.home.rgb : 0x2A4FB0 };
        for (int i = 0; i < n; ++i)
        {
            const int r = rows[i];
            const float y = std::round(rowTop + rowStep * (i + 0.5f));
            const float lw = Width(face, kLabels[r]);
            ShadowedText(out, face, kLabels[r], cx - lw / 2, y, 0xE6E9ED, 1.0f, shadow);
            for (int side = 0; side < 2; ++side)
            {
                // The track, then the fill from the label's side out.
                const float x0b = side == 0 ? bx0 + margin : cx + labelW / 2 + gap, x1b = x0b + barW;
                const float yb0 = std::round(y - barH / 2), yb1 = yb0 + barH;
                Rect(out, x0b, yb0, x1b, yb1, Rgba(0x05070A, 0.85f));
                const float fill = std::round(barW * board.rows[r][side] / 100.0f);
                const float f0 = side == 0 ? x1b - fill : x0b, f1 = side == 0 ? x1b : x0b + fill;
                const uint32_t c = colours[side];
                Quad(out, f0, yb0, f1, yb1, Rgba(Mix(c, 0xFFFFFF, 0.25f), 1.0f), Rgba(Mix(c, 0xFFFFFF, 0.25f), 1.0f), Rgba(Shade(c, 0.75f), 1.0f),
                     Rgba(Shade(c, 0.75f), 1.0f));
                // Segments, as the game's bars have.
                const float step = std::max(3.0f, std::round(barH * 0.9f));
                for (float sx = x0b + step; sx < x1b - 1; sx += step)
                    Rect(out, sx, yb0, sx + std::max(1.0f, std::round(k)), yb1, Rgba(0x000000, 0.35f));
            }
        }
    }
}

void ScoreBarBuild(hc09vk::Swapchain& swapchain, float x0, float y0, float w, float h)
{
    if (!hc09::broadcast::Enabled() || w <= 0 || h <= 0)
        return;
    const hc09::broadcast::Scoreboard board = hc09::broadcast::Snapshot();

    // The replay: recorded while a game's scenes are drawn; each play kept as it ends.
    static uint64_t markedPlay = 0;
    // Not once a play has run 12 seconds (the play clock stopped): its start is kept until the down and distance
    // changes, which after a change of possession (cutscenes) can be much later.
    const bool longPlay = board.playClockChanged != std::chrono::steady_clock::time_point{} &&
                          std::chrono::steady_clock::now() - board.playClockChanged > std::chrono::seconds(13);
    hc09vk::SetReplayWanted((board.shown || board.box > 0) && !longPlay);
    if (board.play != markedPlay)
    {
        markedPlay = board.play;
        hc09vk::ReplayMarkPlay(board.playStart, board.playEnd);
    }

    // Fading in and out over a fifth of a second as the game shows and hides its score bug.
    static auto last = std::chrono::steady_clock::now();
    static float fade = 0;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
    last = now;
    const bool show = board.shown && !(Trim(board.clock).empty() && Trim(board.awayScore).empty());
    fade = std::clamp(fade + (show ? dt : -dt) * 5.0f, 0.0f, 1.0f);
    const float opacity = fade;
    if ((opacity <= 0 && board.box <= 0) || !Load())
        return;

    const int barHeight = std::max(20, int(std::lround(h * 0.052f)));
    if (!Bake(barHeight))
        return;
    const uint32_t teams[2] = { board.away.id, board.home.id };
    Compose(swapchain, teams);
    if (board.box > 0)
        Box(swapchain, board, x0, y0, w, h);
    if (opacity <= 0)
        return;

    const float H = float(barHeight);
    const float pad = std::round(H * 0.32f), gap = std::max(2.0f, std::round(H * 0.06f)), shadow = std::max(1.0f, std::round(H / 30));
    // At the foot of the picture, below the game's own panels there.
    const float bottom = std::round(y0 + h - H * 0.12f), top = bottom - H, middle = top + H / 2;
    const float logoH = std::round(H * 0.80f), pipCol = std::round(H * 0.34f);
    const uint32_t white = 0xF6F7F8;

    // The panels' widths.
    float teamW[2], logoW[2], scoreW[2];
    std::string scores[2] = { Trim(board.awayScore), Trim(board.homeScore) };
    const hc09::broadcast::Team* sides[2] = { &board.away, &board.home };
    for (int s = 0; s < 2; ++s)
    {
        const float aspect = g_atlas.logoAspect[s];
        logoW[s] = aspect > 0 ? std::round(logoH * std::min(aspect, 1.6f)) : std::round(Width(g_font.small, sides[s]->abbreviation));
        scoreW[s] = std::max(std::round(Width(g_font.big, scores[s])), std::round(H * 0.95f));
        teamW[s] = pipCol + logoW[s] + std::round(H * 0.3f) + scoreW[s] + pad;
    }
    const std::string quarter = Upper(Trim(board.quarter)), clock = Trim(board.clock), playClock = Trim(board.playClock);
    std::string down = Trim(board.down), spot;
    if (const size_t on = down.find(" on "); on != std::string::npos)
        spot = Upper(down.substr(on + 4)), down = down.substr(0, on);
    down = Upper(down);
    const float qW = std::round(Width(g_font.small, quarter)), cW = std::round(Width(g_font.big, clock)), pW = std::round(Width(g_font.big, playClock));
    const float divider = std::max(1.0f, std::round(H / 30));
    const float clockPanel = pad + qW + (qW > 0 ? std::round(H * 0.28f) : 0) + cW + (pW > 0 ? std::round(H * 0.28f) * 2 + divider + pW : 0) + pad;
    const float dW = std::round(Width(g_font.big, down)), sW = std::round(Width(g_font.small, spot));
    const float downPanel = down.empty() ? 0 : pad + dW + (sW > 0 ? std::round(H * 0.25f) + sW : 0) + pad;
    const float total = teamW[0] + gap + teamW[1] + gap + clockPanel + (downPanel > 0 ? gap + downPanel : 0);

    std::vector<OverlayVtx>& out = swapchain.overlay;
    const float left = std::round(x0 + (w - total) / 2);
    // The ribbon's frame: dark, a little larger than the panels, which its colour shows between.
    const float frame = std::max(1.0f, std::round(H * 0.04f));
    Rect(out, left - frame, top - frame, left + total + frame, bottom + frame, Rgba(0x06080A, 0.92f * opacity));

    float x = left;
    float panelX[2];
    for (int s = 0; s < 2; ++s)
    {
        const uint32_t c = sides[s]->rgb;
        panelX[s] = x;
        Quad(out, x, top, x + teamW[s], bottom, Rgba(Mix(c, 0xFFFFFF, 0.12f), opacity), Rgba(Shade(c, 0.42f), opacity),
             Rgba(Shade(c, 0.28f), opacity), Rgba(Shade(c, 0.80f), opacity));
        // A sheen along the top half.
        Quad(out, x, top, x + teamW[s], middle, Rgba(0xFFFFFF, 0.10f * opacity), Rgba(0xFFFFFF, 0.04f * opacity), Rgba(0xFFFFFF, 0),
             Rgba(0xFFFFFF, 0));
        // Timeouts: three pips, bright for each left.
        if (const int left3 = s == 0 ? board.awayTimeouts : board.homeTimeouts; left3 >= 0)
        {
            const float pip = std::max(2.0f, std::round(H * 0.11f)), step = std::round(H * 0.20f);
            const float px = std::round(x + (pipCol - pip) / 2);
            for (int i = 0; i < 3; ++i)
            {
                const float py = std::round(middle - pip / 2 + (i - 1) * step);
                Rect(out, px, py, px + pip, py + pip, Rgba(0xFFFFFF, (i < left3 ? 0.95f : 0.22f) * opacity));
            }
        }
        const float lx = x + pipCol;
        if (g_atlas.logoAspect[s] > 0)
        {
            const float lh = logoW[s] / g_atlas.logoAspect[s] < logoH ? std::round(logoW[s] / g_atlas.logoAspect[s]) : logoH;
            Logo(out, s, lx, std::round(middle - lh / 2), logoW[s], lh, opacity);
        }
        else
            ShadowedText(out, g_font.small, sides[s]->abbreviation, lx, middle, TextOn(c), opacity, shadow);
        const float sw = Width(g_font.big, scores[s]);
        ShadowedText(out, g_font.big, scores[s], x + teamW[s] - pad - scoreW[s] + (scoreW[s] - sw) / 2, middle, white, opacity, shadow);
        x += teamW[s] + gap;
    }
    // Possession: a red arrow on the inner edge of that team's panel, pointing at its score.
    if (board.possession == 0 || board.possession == 1)
    {
        const float aw = std::round(H * 0.20f), ah = std::round(H * 0.42f);
        const uint32_t red = Rgba(0xE0312B, opacity);
        if (board.possession == 0)
        {
            const float ex = panelX[0] + teamW[0];
            Triangle(out, ex, middle - ah / 2, ex, middle + ah / 2, ex - aw, middle, red);
        }
        else
        {
            const float ex = panelX[1];
            Triangle(out, ex, middle - ah / 2, ex + aw, middle, ex, middle + ah / 2, red);
        }
    }
    // The quarter, the clock and the play clock.
    Quad(out, x, top, x + clockPanel, bottom, Rgba(0x30353D, opacity), Rgba(0x2A2E35, opacity), Rgba(0x14171B, opacity), Rgba(0x181B20, opacity));
    float cx = x + pad;
    if (qW > 0)
    {
        ShadowedText(out, g_font.small, quarter, cx, middle, 0xC9CED6, opacity, shadow);
        cx += qW + std::round(H * 0.28f);
    }
    ShadowedText(out, g_font.big, clock, cx, middle, white, opacity, shadow);
    cx += cW;
    if (pW > 0)
    {
        cx += std::round(H * 0.28f);
        Rect(out, cx, middle - H * 0.30f, cx + divider, middle + H * 0.30f, Rgba(0xFFFFFF, 0.25f * opacity));
        cx += divider + std::round(H * 0.28f);
        // The play clock turns red in its last five seconds.
        const bool late = playClock.size() >= 2 && playClock[0] == ':' && std::atoi(playClock.c_str() + 1) <= 5;
        ShadowedText(out, g_font.big, playClock, cx, middle, late ? 0xFF4A3D : 0xFFC233, opacity, shadow);
    }
    x += clockPanel;
    // The down and distance, and the ball's spot; a line along its foot in the colour of the team with the ball.
    if (downPanel > 0)
    {
        x += gap;
        Quad(out, x, top, x + downPanel, bottom, Rgba(0x262A31, opacity), Rgba(0x1F2228, opacity), Rgba(0x111316, opacity), Rgba(0x15171B, opacity));
        const uint32_t accent = board.possession == 0 ? board.away.rgb : board.possession == 1 ? board.home.rgb : 0x8A929C;
        Rect(out, x, bottom - std::max(2.0f, std::round(H * 0.08f)), x + downPanel, bottom, Rgba(accent, opacity));
        ShadowedText(out, g_font.big, down, x + pad, middle, white, opacity, shadow);
        if (sW > 0)
            ShadowedText(out, g_font.small, spot, x + pad + dW + std::round(H * 0.25f), middle, 0xB9C0C9, opacity, shadow);
    }
}
