// Texture decoding (mip 0 of 2D textures) and a minimal PNG writer.
//
// Layout facts follow the Xenos documentation as captured in Xenia
// (texture_address.h, xenos.h; BSD-3-Clause): 32x32-block macro tiles,
// bank/pipe interleaving, pitch in 32-texel units, per-fetch endian swap and
// component swizzle.
#include "xenos.h"
#include "xenos_texture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace xenos {
namespace {

struct FormatInfo { uint32_t blockW, blockH, bytesPerBlock; const char* name; };

FormatInfo Info(uint32_t f)
{
    switch (f)
    {
    case 2:  return { 1, 1, 1, "8" };
    case 3:  return { 1, 1, 2, "1_5_5_5" };
    case 4:  return { 1, 1, 2, "5_6_5" };
    case 6:  return { 1, 1, 4, "8_8_8_8" };
    case 7:  return { 1, 1, 4, "2_10_10_10" };
    case 10: return { 1, 1, 2, "8_8" };
    case 15: return { 1, 1, 2, "4_4_4_4" };
    case 18: return { 4, 4, 8, "DXT1" };
    case 19: return { 4, 4, 16, "DXT2_3" };
    case 20: return { 4, 4, 16, "DXT4_5" };
    case 22: return { 1, 1, 4, "24_8" };
    case 24: return { 1, 1, 2, "16" };
    case 25: return { 1, 1, 4, "16_16" };
    case 26: return { 1, 1, 8, "16_16_16_16" };
    case 31: return { 1, 1, 4, "16_16_FLOAT" };
    case 32: return { 1, 1, 8, "16_16_16_16_FLOAT" };
    case 36: return { 1, 1, 4, "32_FLOAT" };
    case 37: return { 1, 1, 8, "32_32_FLOAT" };
    case 49: return { 4, 4, 16, "DXN" };
    case 50: return { 1, 1, 4, "8_8_8_8_AS_16_16_16_16" };
    case 51: return { 4, 4, 8, "DXT1_AS_16_16_16_16" };
    case 52: return { 4, 4, 16, "DXT2_3_AS_16_16_16_16" };
    case 53: return { 4, 4, 16, "DXT4_5_AS_16_16_16_16" };
    case 58: return { 4, 4, 8, "DXT3A" };
    case 59: return { 4, 4, 8, "DXT5A" };
    default: return { 0, 0, 0, "unsupported" };
    }
}

int Log2(uint32_t v) { int l = 0; while ((1u << l) < v) ++l; return l; }

}  // namespace

uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, int bppLog2)
{
    uint32_t outerBlocks = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
    uint32_t innerBlocks = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t oi = (outerBlocks | innerBlocks) << bppLog2;
    uint32_t bank = (y >> 4) & 1;
    uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) |
           (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
}

// 3D tiling: macro tiles of 32x16x4 blocks laid out x-fastest, then y, then z;
// inside, 8x8x4 micro tiles with z[1:0], y[2:1], x[2:0] linear. Micro tile
// position within the macro tile goes to the bank (y[3] ^ z[2]) and pipe
// (x[4:3], bit 1 flipped by the bank) bits, y[0] to address bit 4.
uint32_t TiledOffset3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitchBlocks, uint32_t heightBlocks, int bppLog2)
{
    uint32_t outerBlocks = ((((z >> 2) * (heightBlocks >> 4) + (y >> 4)) * (pitchBlocks >> 5)) + (x >> 5)) << 7;
    uint32_t innerBlocks = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
    uint32_t oi = (outerBlocks | innerBlocks) << bppLog2;
    uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1;
    uint32_t pipe = ((x >> 3) & 3) ^ (bank << 1);
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) |
           (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
}

float HalfToFloat(uint32_t h)
{
    uint32_t sign = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float f;
    if (e == 0) f = std::ldexp(float(m), -24);
    else if (e == 31) f = m ? NAN : INFINITY;
    else f = std::ldexp(float(m | 0x400), int(e) - 25);
    return sign ? -f : f;
}

uint32_t FloatToHalf(float f)
{
    uint32_t sign = std::signbit(f) ? 0x8000u : 0;
    float a = std::fabs(f);
    if (std::isnan(a)) return sign | 0x7E00;
    if (a >= 65520.0f) return sign | 0x7C00;              // overflow -> inf
    if (a < std::ldexp(1.0f, -14))                         // denormal
        return sign | uint32_t(std::lround(a * std::ldexp(1.0f, 24)));
    int e;
    float m = std::frexp(a, &e);                           // a = m * 2^e, m in [0.5, 1)
    uint32_t mant = uint32_t(std::lround((m * 2.0f - 1.0f) * 1024.0f));
    int exp = e - 1 + 15;
    if (mant == 1024) { mant = 0; ++exp; }
    if (exp >= 31) return sign | 0x7C00;
    return sign | (uint32_t(exp) << 10) | mant;
}

namespace {

void EndianSwap(uint8_t* p, uint32_t n, uint32_t endian)
{
    switch (endian)
    {
    case 1: for (uint32_t i = 0; i + 1 < n; i += 2) std::swap(p[i], p[i + 1]); break;
    case 2: for (uint32_t i = 0; i + 3 < n; i += 4) { std::swap(p[i], p[i + 3]); std::swap(p[i + 1], p[i + 2]); } break;
    case 3: for (uint32_t i = 0; i + 3 < n; i += 4) { std::swap(p[i], p[i + 2]); std::swap(p[i + 1], p[i + 3]); } break;
    default: break;
    }
}

uint32_t Pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return r | (g << 8) | (b << 16) | (uint32_t(a) << 24); }

// Components x,y,z,w -> RGBA through the fetch swizzle (3 bits each: 0-3
// select x-w, 4 = 0, 5 = 1).
uint32_t Swizzle(const uint8_t c[4], uint32_t swz)
{
    uint8_t o[4];
    for (int i = 0; i < 4; ++i)
    {
        uint32_t s = (swz >> (3 * i)) & 7;
        o[i] = s < 4 ? c[s] : s == 4 ? 0 : s == 5 ? 255 : c[i];
    }
    return Pack(o[0], o[1], o[2], o[3]);
}

uint8_t Expand(uint32_t v, int bits) { return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

void Color565(uint16_t c, uint8_t out[3])
{
    out[0] = Expand((c >> 11) & 31, 5);
    out[1] = Expand((c >> 5) & 63, 6);
    out[2] = Expand(c & 31, 5);
}

// BC1 colour block (8 bytes, little-endian after the endian swap) -> 16 texels x,y,z,w.
void DecodeBc1Color(const uint8_t* b, uint8_t texels[16][4], bool fourColorOnly)
{
    uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
    uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
    uint8_t pal[4][4];
    Color565(c0, pal[0]); Color565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || fourColorOnly)
    {
        for (int k = 0; k < 3; ++k)
        {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    }
    else
    {
        for (int k = 0; k < 3; ++k) { pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2); pal[3][k] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    for (int i = 0; i < 16; ++i) memcpy(texels[i], pal[(idx >> (2 * i)) & 3], 4);
}

void DecodeBc3Alpha(const uint8_t* b, uint8_t texels[16][4], int channel)
{
    uint8_t a0 = b[0], a1 = b[1], pal[8];
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
    else { for (int i = 1; i < 5; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5); pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) texels[i][channel] = pal[(bits >> (3 * i)) & 7];
}

}  // namespace

TextureFetch ReadTextureFetch(uint32_t slot)
{
    TextureFetch tf;
    for (int i = 0; i < 6; ++i) tf.dword[i] = Reg(reg::FETCH_CONSTANTS + slot * 6 + i);
    return tf;
}

const char* TextureFormatName(uint32_t format) { return Info(format).name; }

// Distance between array layers / cube faces of mip level 0: the row pitch
// times the height in blocks padded to the 32-block tile, 4 KiB aligned.
uint64_t LayerStride(const TextureFetch& tf, const FormatInfo& fi)
{
    uint32_t bw = (tf.width() + fi.blockW - 1) / fi.blockW, bh = (tf.height() + fi.blockH - 1) / fi.blockH;
    uint32_t pitchBlocks = std::max(bw, tf.pitchTexels() / fi.blockW);
    uint64_t rowBytes = tf.tiled() ? uint64_t((pitchBlocks + 31) & ~31u) * fi.bytesPerBlock
                                   : (pitchBlocks * fi.bytesPerBlock + 255) & ~255u;
    return (rowBytes * ((bh + 31) & ~31u) + 4095) & ~uint64_t(4095);
}

uint32_t TextureBytes(const TextureFetch& tf)
{
    FormatInfo fi = Info(tf.format());
    if (!fi.bytesPerBlock) fi = { 1, 1, 4, "" };
    uint64_t bytes = LayerStride(tf, fi) * (tf.is3D() ? ((tf.depth() + 3) & ~3u) : tf.layers());
    return uint32_t(std::min<uint64_t>(bytes, 64u << 20));
}

void PackedBaseOffset(const TextureFetch& tf, uint32_t& xBlocks, uint32_t& yBlocks)
{
    xBlocks = yBlocks = 0;
    if (!tf.packedMips() || tf.is3D()) return;
    int lw = Log2(tf.width()), lh = Log2(tf.height());
    if (std::min(lw, lh) > 4) return;
    FormatInfo fi = Info(tf.format());
    if (!fi.bytesPerBlock) return;
    if (lw > lh) yBlocks = 16 / fi.blockH;
    else xBlocks = 16 / fi.blockW;
}

bool DecodeTextureLayer(const TextureFetch& tf, uint32_t layer, Image& out, std::string* why)
{
    FormatInfo fi = Info(tf.format());
    if (!fi.bytesPerBlock) { if (why) *why = "format " + std::to_string(tf.format()) + " not supported"; return false; }
    bool vol = tf.is3D();
    if (tf.dimension() == 0) { if (why) *why = "1D textures not supported"; return false; }
    if (layer >= tf.layers()) { if (why) *why = "layer out of range"; return false; }
    uint32_t w = tf.width(), h = tf.height();
    uint32_t bw = (w + fi.blockW - 1) / fi.blockW, bh = (h + fi.blockH - 1) / fi.blockH;
    uint32_t pitchBlocks = tf.pitchTexels() / fi.blockW;
    if (pitchBlocks < bw) pitchBlocks = bw;
    int bppLog2 = Log2(fi.bytesPerBlock);
    uint32_t rowBytes = 0;
    if (tf.tiled()) pitchBlocks = (pitchBlocks + 31) & ~31u;
    else rowBytes = (pitchBlocks * fi.bytesPerBlock + 255) & ~255u;

    out.width = w; out.height = h;
    out.rgba.assign(size_t(w) * h, 0);
    // Cube faces and stacked layers are whole level-0 slices apart; 3D slices interleave within the tiling.
    uint32_t base = tf.baseAddress() + (vol ? 0u : uint32_t(LayerStride(tf, fi) * layer)), swz = tf.swizzle();
    uint32_t heightBlocks = (bh + 31) & ~31u;
    uint32_t ox, oy;
    PackedBaseOffset(tf, ox, oy);
    uint8_t blk[16];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            uint32_t px = bx + ox, py = by + oy;
            uint32_t off = vol ? (tf.tiled() ? TiledOffset3D(bx, by, layer, pitchBlocks, heightBlocks, bppLog2)
                                             : (layer * heightBlocks + by) * rowBytes + bx * fi.bytesPerBlock)
                               : tf.tiled() ? TiledOffset2D(px, py, pitchBlocks, bppLog2) : py * rowBytes + px * fi.bytesPerBlock;
            memcpy(blk, Phys(base + off), fi.bytesPerBlock);
            EndianSwap(blk, fi.bytesPerBlock, tf.endian());
            uint8_t texels[16][4] = {};
            uint32_t f = tf.format();
            if (fi.blockW == 1)
            {
                uint8_t c[4] = { 0, 0, 0, 255 };
                uint32_t v32 = blk[0] | (blk[1] << 8) | (fi.bytesPerBlock > 2 ? (blk[2] << 16) | (uint32_t(blk[3]) << 24) : 0);
                uint32_t hi32 = fi.bytesPerBlock > 4 ? blk[4] | (blk[5] << 8) | (blk[6] << 16) | (uint32_t(blk[7]) << 24) : 0;
                auto u8 = [](float f) { return uint8_t(std::lround((f > 1.0f ? 1.0f : (f >= 0.0f ? f : 0.0f)) * 255.0f)); };
                switch (f)
                {
                case 2:  c[0] = c[1] = c[2] = c[3] = blk[0]; break;
                case 3:  c[0] = Expand(v32 & 31, 5); c[1] = Expand((v32 >> 5) & 31, 5); c[2] = Expand((v32 >> 10) & 31, 5); c[3] = (v32 >> 15) & 1 ? 255 : 0; break;
                case 4:  c[0] = Expand(v32 & 31, 5); c[1] = Expand((v32 >> 5) & 63, 6); c[2] = Expand((v32 >> 11) & 31, 5); break;
                case 6: case 50: c[0] = blk[0]; c[1] = blk[1]; c[2] = blk[2]; c[3] = blk[3]; break;
                case 7:  c[0] = uint8_t((v32 & 1023) >> 2); c[1] = uint8_t(((v32 >> 10) & 1023) >> 2); c[2] = uint8_t(((v32 >> 20) & 1023) >> 2); c[3] = Expand(v32 >> 30, 2); break;
                case 10: c[0] = blk[0]; c[1] = blk[1]; c[2] = 0; c[3] = 255; break;
                case 15: c[0] = Expand(v32 & 15, 4); c[1] = Expand((v32 >> 4) & 15, 4); c[2] = Expand((v32 >> 8) & 15, 4); c[3] = Expand((v32 >> 12) & 15, 4); break;
                case 22: c[0] = c[1] = c[2] = uint8_t(v32 >> 24); c[3] = 255; break;   // depth (high byte) as grey
                // Wider formats are reduced to 8 bits per channel (clamped).
                case 24: c[0] = uint8_t(v32 >> 8); break;
                case 25: c[0] = uint8_t(v32 >> 8); c[1] = uint8_t(v32 >> 24); break;
                case 26: c[0] = uint8_t(v32 >> 8); c[1] = uint8_t(v32 >> 24); c[2] = uint8_t(hi32 >> 8); c[3] = uint8_t(hi32 >> 24); break;
                case 31: c[0] = u8(HalfToFloat(v32 & 0xFFFF)); c[1] = u8(HalfToFloat(v32 >> 16)); break;
                case 32: c[0] = u8(HalfToFloat(v32 & 0xFFFF)); c[1] = u8(HalfToFloat(v32 >> 16));
                         c[2] = u8(HalfToFloat(hi32 & 0xFFFF)); c[3] = u8(HalfToFloat(hi32 >> 16)); break;
                case 36: { float f; memcpy(&f, &v32, 4); c[0] = u8(f); break; }
                case 37: { float f, g; memcpy(&f, &v32, 4); memcpy(&g, &hi32, 4); c[0] = u8(f); c[1] = u8(g); break; }
                }
                uint32_t px = (f == 22) ? Pack(c[0], c[1], c[2], 255) : Swizzle(c, swz);
                if (bx < w && by < h) out.rgba[size_t(by) * w + bx] = px;
                continue;
            }
            switch (f)
            {
            case 18: case 51: DecodeBc1Color(blk, texels, false); break;
            case 19: case 52:
                DecodeBc1Color(blk + 8, texels, true);
                for (int i = 0; i < 16; ++i) texels[i][3] = Expand((blk[i / 2] >> (4 * (i & 1))) & 15, 4);
                break;
            case 20: case 53: DecodeBc1Color(blk + 8, texels, true); DecodeBc3Alpha(blk, texels, 3); break;
            case 58: for (int i = 0; i < 16; ++i) { uint8_t a = Expand((blk[i / 2] >> (4 * (i & 1))) & 15, 4); texels[i][0] = texels[i][1] = texels[i][2] = texels[i][3] = a; } break;
            case 59: DecodeBc3Alpha(blk, texels, 0); for (int i = 0; i < 16; ++i) texels[i][1] = texels[i][2] = texels[i][3] = texels[i][0]; break;
            case 49:   // DXN (BC5): two BC3 alpha blocks, X then Y
                DecodeBc3Alpha(blk, texels, 0);
                DecodeBc3Alpha(blk + 8, texels, 1);
                for (int i = 0; i < 16; ++i) { texels[i][2] = 0; texels[i][3] = 255; }
                break;
            }
            for (uint32_t ty = 0; ty < 4; ++ty)
                for (uint32_t tx = 0; tx < 4; ++tx)
                {
                    uint32_t x = bx * 4 + tx, y = by * 4 + ty;
                    if (x < w && y < h) out.rgba[size_t(y) * w + x] = Swizzle(texels[ty * 4 + tx], swz);
                }
        }
    return true;
}

// ---- PNG (RGBA8, stored deflate blocks) ----------------------------------------
namespace {
uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
void Put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
void Chunk(FILE* f, const char* type, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> buf;
    Put32(buf, uint32_t(data.size()));
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    uint32_t crc = Crc32(buf.data() + 4, buf.size() - 4);
    Put32(buf, crc);
    fwrite(buf.data(), 1, buf.size(), f);
}
}  // namespace

bool DecodeFrontBuffer(uint32_t frontBuffer, uint32_t width, uint32_t height, Image& out)
{
    if (!width || !height) return false;
    // Read back with swizzle zyxw (the resolve swapped R and B).
    TextureFetch tf;
    uint32_t pitch = (width + 31) & ~31u;
    tf.dword[0] = 2 | (1u << 31) | ((pitch / 32) << 22);
    tf.dword[1] = 6 | ((frontBuffer & 0x1FFFFFFF) & 0xFFFFF000);
    tf.dword[2] = (width - 1) | ((height - 1) << 13);
    tf.dword[3] = 0x60Au << 1;
    tf.dword[5] = 1u << 9;
    if (!DecodeTexture2D(tf, out)) return false;
    for (auto& p : out.rgba) p |= 0xFF000000u;   // scanout ignores alpha
    return true;
}

bool WritePng(const std::string& path, const Image& img)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    Put32(ihdr, img.width); Put32(ihdr, img.height);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    Chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    raw.reserve(size_t(img.height) * (img.width * 4 + 1));
    for (uint32_t y = 0; y < img.height; ++y)
    {
        raw.push_back(0);
        const uint8_t* row = reinterpret_cast<const uint8_t*>(img.rgba.data() + size_t(y) * img.width);
        raw.insert(raw.end(), row, row + img.width * 4);
    }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw.size() || pos == 0;)
    {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    Put32(z, (b << 16) | a);
    Chunk(f, "IDAT", z);
    Chunk(f, "IEND", {});
    fclose(f);
    return true;
}

}  // namespace xenos
