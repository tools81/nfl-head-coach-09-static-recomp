#pragma once
// Texture fetch constants, texture decoding to RGBA8, and a minimal PNG writer.
// Portable C++17 (see xenos.h).
#include <cstdint>
#include <string>
#include <vector>

namespace xenos {

struct TextureFetch
{
    uint32_t dword[6] = {};
    bool valid() const { return (dword[0] & 3) == 2; }
    uint32_t format() const { return dword[1] & 0x3F; }
    uint32_t endian() const { return (dword[1] >> 6) & 3; }
    bool tiled() const { return (dword[0] >> 31) & 1; }
    uint32_t pitchTexels() const { return ((dword[0] >> 22) & 0x1FF) * 32; }
    uint32_t baseAddress() const { return dword[1] & 0xFFFFF000; }
    uint32_t mipAddress() const { return dword[5] & 0xFFFFF000; }
    // 3D (volume) textures: dimension 2; size fields are 11/11/10 bits.
    bool is3D() const { return dimension() == 2; }
    // Stacked (2D array) textures: dimension 2D with the stacked flag (dword 1 bit 10); 13/13-bit size and
    // a 6-bit stack depth. A 3D fetch picks the layer from z (the jersey number digits, the name font).
    bool stacked() const { return dimension() == 1 && ((dword[1] >> 10) & 1); }
    uint32_t width() const { return is3D() ? (dword[2] & 0x7FF) + 1 : (dword[2] & 0x1FFF) + 1; }
    uint32_t height() const { return is3D() ? ((dword[2] >> 11) & 0x7FF) + 1 : ((dword[2] >> 13) & 0x1FFF) + 1; }
    uint32_t depth() const { return is3D() ? ((dword[2] >> 22) & 0x3FF) + 1 : stacked() ? ((dword[2] >> 26) & 0x3F) + 1 : 1; }
    // Layers DecodeTextureLayer accepts: cube faces, stacked layers or 3D slices.
    uint32_t layers() const { return dimension() == 3 ? 6 : depth(); }
    uint32_t dimension() const { return (dword[5] >> 9) & 3; }   // 0 1D, 1 2D, 2 3D, 3 cube
    uint32_t swizzle() const { return (dword[3] >> 1) & 0xFFF; }
    uint32_t signs() const { return (dword[0] >> 2) & 0xFF; }
    bool packedMips() const { return (dword[5] >> 11) & 1; }
};

TextureFetch ReadTextureFetch(uint32_t slot);   // fetch constant group `slot` (0-31)

struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> rgba;   // byte order R,G,B,A in memory (little-endian 0xAABBGGRR)
};

const char* TextureFormatName(uint32_t format);
// Bytes spanned by mip level 0 including tile padding, all six faces for a
// cube, all layers of a stacked texture, all slices for a 3D texture (an upper
// bound; 4 bytes per texel for formats the decoder doesn't know).
uint32_t TextureBytes(const TextureFetch& tf);
// Packed mips (fetch dword 5 bit 11) with a side of at most 16 texels put the
// base level itself in the packed mip tail of its tile: 16 texels down when
// wider than tall, else 16 across, in blocks (Xenia texture_util
// GetPackedMipOffset at level 0). The players' 32x1 skin ramps sit at row 16.
void PackedBaseOffset(const TextureFetch& tf, uint32_t& xBlocks, uint32_t& yBlocks);
// Decode mip level 0 of a 2D texture, of one cube face (`layer` 0-5), of one
// layer of a stacked texture or of one slice of a 3D texture (`layer` = z);
// false (with a reason) if unsupported.
bool DecodeTextureLayer(const TextureFetch& tf, uint32_t layer, Image& out, std::string* why = nullptr);
inline bool DecodeTexture2D(const TextureFetch& tf, Image& out, std::string* why = nullptr)
{
    return DecodeTextureLayer(tf, 0, out, why);
}
// The presented image of an XE_SWAP: the front buffer as the last resolve left
// it (tiled 8888, R/B swapped by the resolve), alpha forced opaque as scanout does.
bool DecodeFrontBuffer(uint32_t frontBuffer, uint32_t width, uint32_t height, Image& out);
bool WritePng(const std::string& path, const Image& img);

// Byte offset of block (x, y) in a tiled 2D surface whose pitch (in blocks) is
// a multiple of 32, with 2^bppLog2 bytes per block.
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, int bppLog2);
// Same for block (x, y, z) of a tiled 3D texture (pitch and height in blocks, multiples of 32).
uint32_t TiledOffset3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitchBlocks, uint32_t heightBlocks, int bppLog2);
// IEEE half <-> float.
float HalfToFloat(uint32_t h);
uint32_t FloatToHalf(float f);

}  // namespace xenos
