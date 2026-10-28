#include "gpu/texture_cache.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <wincodec.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "common/edition.h"
#include "gpu/command_processor.h"
#include "gpu/draw.h"
#include "gpu/shader_compiler.h"
#include "gpu/swappable_images.h"
#include "kernel/guest.h"
#include "memory.h"

namespace hc09::gpu
{
    namespace
    {
        struct FormatInfo
        {
            DXGI_FORMAT dxgi;
            uint32_t blockBytes;   // bytes per texel, or per 4x4 block when compressed
            bool compressed;
            uint8_t remap[4];      // DXGI component holding Xenos component X, Y, Z, W
        };

        // True when a comma-separated list of "<width>x<height>f<format>" entries names this texture.
        bool MatchesTextureSpec(const char* spec, uint32_t width, uint32_t height, uint32_t format)
        {
            if (!spec)
                return false;
            char wanted[32];
            const int length = std::snprintf(wanted, sizeof(wanted), "%ux%uf%u", width, height, format);
            for (const char* at = std::strstr(spec, wanted); at; at = std::strstr(at + 1, wanted))
                if ((at == spec || at[-1] == ',') && (at[length] == 0 || at[length] == ','))
                    return true;
            return false;
        }

        bool LookupFormat(uint32_t format, FormatInfo& info)
        {
            switch (format)
            {
            case 2: info = { DXGI_FORMAT_R8_UNORM, 1, false, { 0, 1, 2, 3 } }; return true;             // k_8
            case 3: info = { DXGI_FORMAT_B5G5R5A1_UNORM, 2, false, { 2, 1, 0, 3 } }; return true;       // k_1_5_5_5
            case 4: info = { DXGI_FORMAT_B5G6R5_UNORM, 2, false, { 2, 1, 0, 3 } }; return true;         // k_5_6_5
            case 6: info = { DXGI_FORMAT_R8G8B8A8_UNORM, 4, false, { 0, 1, 2, 3 } }; return true;       // k_8_8_8_8
            case 7: info = { DXGI_FORMAT_R10G10B10A2_UNORM, 4, false, { 0, 1, 2, 3 } }; return true;    // k_2_10_10_10
            case 10: info = { DXGI_FORMAT_R8G8_UNORM, 2, false, { 0, 1, 2, 3 } }; return true;          // k_8_8
            case 15: info = { DXGI_FORMAT_B4G4R4A4_UNORM, 2, false, { 2, 1, 0, 3 } }; return true;      // k_4_4_4_4
            case 18: info = { DXGI_FORMAT_BC1_UNORM, 8, true, { 0, 1, 2, 3 } }; return true;            // k_DXT1
            case 19: info = { DXGI_FORMAT_BC2_UNORM, 16, true, { 0, 1, 2, 3 } }; return true;           // k_DXT2_3
            case 20: info = { DXGI_FORMAT_BC3_UNORM, 16, true, { 0, 1, 2, 3 } }; return true;           // k_DXT4_5
            case 24: info = { DXGI_FORMAT_R16_UNORM, 2, false, { 0, 1, 2, 3 } }; return true;           // k_16
            case 25: info = { DXGI_FORMAT_R16G16_UNORM, 4, false, { 0, 1, 2, 3 } }; return true;        // k_16_16
            case 26: info = { DXGI_FORMAT_R16G16B16A16_UNORM, 8, false, { 0, 1, 2, 3 } }; return true;  // k_16_16_16_16
            case 30: info = { DXGI_FORMAT_R16_FLOAT, 2, false, { 0, 1, 2, 3 } }; return true;           // k_16_FLOAT
            case 31: info = { DXGI_FORMAT_R16G16_FLOAT, 4, false, { 0, 1, 2, 3 } }; return true;        // k_16_16_FLOAT
            case 32: info = { DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false, { 0, 1, 2, 3 } }; return true;  // k_16_16_16_16_FLOAT
            case 36: info = { DXGI_FORMAT_R32_FLOAT, 4, false, { 0, 1, 2, 3 } }; return true;           // k_32_FLOAT
            case 37: info = { DXGI_FORMAT_R32G32_FLOAT, 8, false, { 0, 1, 2, 3 } }; return true;        // k_32_32_FLOAT
            case 38: info = { DXGI_FORMAT_R32G32B32A32_FLOAT, 16, false, { 0, 1, 2, 3 } }; return true; // k_32_32_32_32_FLOAT
            case 49: info = { DXGI_FORMAT_BC5_UNORM, 16, true, { 0, 1, 2, 3 } }; return true;           // k_DXN
            case 59: info = { DXGI_FORMAT_BC4_UNORM, 8, true, { 0, 1, 2, 3 } }; return true;            // k_DXT5A
            default: return false;
            }
        }

        uint32_t GpuSwap(uint32_t value, uint32_t mode)
        {
            switch (mode & 3)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
            case 2: return __builtin_bswap32(value);
            case 3: return (value << 16) | (value >> 16);
            default: return value;
            }
        }

        // Untiled, byte-swapped texels (rows `pitch` bytes apart) as RGBA8 holding Xenos components
        // X, Y, Z, W (before the fetch swizzle). Common formats only.
        bool DecodeTexels(uint32_t format, const FormatInfo& info, const uint8_t* rows, uint32_t pitch, uint32_t width, uint32_t height,
            std::vector<uint8_t>& out)
        {
            out.assign(size_t(width) * height * 4, 0);
            auto put = [&](uint32_t x, uint32_t y, const uint8_t dxgi[4]) {
                if (x < width && y < height)
                    for (int c = 0; c < 4; c++)
                        out[(size_t(y) * width + x) * 4 + c] = dxgi[info.remap[c]];
            };
            auto color565 = [](uint16_t v, uint8_t rgba[4]) {
                rgba[0] = uint8_t((v >> 11) * 255 / 31);
                rgba[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
                rgba[2] = uint8_t((v & 31) * 255 / 31);
                rgba[3] = 255;
            };
            switch (format)
            {
            case 18: case 19: case 20:  // DXT1, DXT2/3, DXT4/5
            {
                const uint32_t blockBytes = info.blockBytes;
                for (uint32_t by = 0; by < (height + 3) / 4; by++)
                    for (uint32_t bx = 0; bx < (width + 3) / 4; bx++)
                    {
                        const uint8_t* block = rows + size_t(by) * pitch + size_t(bx) * blockBytes;
                        const uint8_t* colorBlock = format == 18 ? block : block + 8;
                        const uint16_t c0 = uint16_t(colorBlock[0] | (colorBlock[1] << 8)), c1 = uint16_t(colorBlock[2] | (colorBlock[3] << 8));
                        uint8_t palette[4][4];
                        color565(c0, palette[0]);
                        color565(c1, palette[1]);
                        const bool fourColors = format != 18 || c0 > c1;
                        for (int c = 0; c < 3; c++)
                        {
                            palette[2][c] = uint8_t(fourColors ? (2 * palette[0][c] + palette[1][c]) / 3 : (palette[0][c] + palette[1][c]) / 2);
                            palette[3][c] = uint8_t(fourColors ? (palette[0][c] + 2 * palette[1][c]) / 3 : 0);
                        }
                        palette[2][3] = 255;
                        palette[3][3] = fourColors ? 255 : 0;
                        uint8_t alphas[8];
                        if (format == 20)
                        {
                            alphas[0] = block[0];
                            alphas[1] = block[1];
                            for (int i = 2; i < 8; i++)
                                alphas[i] = block[0] > block[1] ? uint8_t(((8 - i) * block[0] + (i - 1) * block[1]) / 7)
                                    : i < 6 ? uint8_t(((6 - i) * block[0] + (i - 1) * block[1]) / 5) : i == 6 ? 0 : 255;
                        }
                        uint64_t alphaBits = 0;
                        for (int i = 0; i < 6; i++)
                            alphaBits |= uint64_t(block[2 + i]) << (8 * i);
                        const uint32_t indices = colorBlock[4] | (colorBlock[5] << 8) | (colorBlock[6] << 16) | (uint32_t(colorBlock[7]) << 24);
                        for (uint32_t i = 0; i < 16; i++)
                        {
                            uint8_t texel[4];
                            std::memcpy(texel, palette[(indices >> (2 * i)) & 3], 4);
                            if (format == 19)
                                texel[3] = uint8_t(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);
                            else if (format == 20)
                                texel[3] = alphas[(alphaBits >> (3 * i)) & 7];
                            put(bx * 4 + i % 4, by * 4 + i / 4, texel);
                        }
                    }
                return true;
            }
            case 49: case 59:  // DXN (two channels), DXT5A (one): blocks like DXT5's alpha
            {
                const int channels = format == 49 ? 2 : 1;
                for (uint32_t by = 0; by < (height + 3) / 4; by++)
                    for (uint32_t bx = 0; bx < (width + 3) / 4; bx++)
                    {
                        uint8_t values[2][16]{};
                        for (int channel = 0; channel < channels; channel++)
                        {
                            const uint8_t* block = rows + size_t(by) * pitch + size_t(bx) * info.blockBytes + channel * 8;
                            uint8_t levels[8] = { block[0], block[1] };
                            for (int i = 2; i < 8; i++)
                                levels[i] = block[0] > block[1] ? uint8_t(((8 - i) * block[0] + (i - 1) * block[1]) / 7)
                                    : i < 6 ? uint8_t(((6 - i) * block[0] + (i - 1) * block[1]) / 5) : i == 6 ? 0 : 255;
                            uint64_t bits = 0;
                            for (int i = 0; i < 6; i++)
                                bits |= uint64_t(block[2 + i]) << (8 * i);
                            for (int i = 0; i < 16; i++)
                                values[channel][i] = levels[(bits >> (3 * i)) & 7];
                        }
                        for (uint32_t i = 0; i < 16; i++)
                        {
                            const uint8_t texel[4] = { values[0][i], values[1][i], 0, 255 };
                            put(bx * 4 + i % 4, by * 4 + i / 4, texel);
                        }
                    }
                return true;
            }
            case 2: case 3: case 4: case 6: case 15:
                for (uint32_t y = 0; y < height; y++)
                    for (uint32_t x = 0; x < width; x++)
                    {
                        const uint8_t* at = rows + size_t(y) * pitch + size_t(x) * info.blockBytes;
                        const uint16_t v = info.blockBytes == 2 ? uint16_t(at[0] | (at[1] << 8)) : 0;
                        uint8_t texel[4] = { 0, 0, 0, 255 };
                        if (format == 2)
                            texel[0] = at[0];
                        else if (format == 6)
                            std::memcpy(texel, at, 4);
                        else if (format == 4)
                            color565(v, texel);
                        else if (format == 15)  // B4G4R4A4
                            texel[0] = uint8_t(((v >> 8) & 15) * 17), texel[1] = uint8_t(((v >> 4) & 15) * 17),
                            texel[2] = uint8_t((v & 15) * 17), texel[3] = uint8_t((v >> 12) * 17);
                        else  // B5G5R5A1
                            texel[0] = uint8_t(((v >> 10) & 31) * 255 / 31), texel[1] = uint8_t(((v >> 5) & 31) * 255 / 31),
                            texel[2] = uint8_t((v & 31) * 255 / 31), texel[3] = (v >> 15) ? 255 : 0;
                        put(x, y, texel);
                    }
                return true;
            default:
                return false;
            }
        }

        void SavePng(const wchar_t* path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba)
        {
            thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            (void)com;
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            // The PNG encoder takes BGRA (SetPixelFormat changes the format asked for to that).
            WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
            std::vector<uint8_t> bgra(rgba);
            for (size_t i = 0; i < bgra.size(); i += 4)
                std::swap(bgra[i], bgra[i + 2]);
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
                FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
                FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
                FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
                FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
                FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&pixelFormat)) ||
                pixelFormat != GUID_WICPixelFormat32bppBGRA ||
                FAILED(frame->WritePixels(height, width * 4, UINT(bgra.size()), bgra.data())) ||
                FAILED(frame->Commit()) || FAILED(encoder->Commit()))
                std::fprintf(stderr, "[texture] cannot write %ls\n", path);
        }

        // Offset in units of texels (or blocks) of (x, y) in a 2D tiled surface whose
        // width is `pitch` units of `bytesPerUnit` bytes (the standard Xbox 360 2D tiling).
        uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytesPerUnit)
        {
            const uint32_t alignedPitch = (pitch + 31) & ~31u;
            const uint32_t logBpp = (bytesPerUnit >> 2) + ((bytesPerUnit >> 1) >> (bytesPerUnit >> 2));
            const uint32_t macro = ((x >> 5) + (y >> 5) * (alignedPitch >> 5)) << (logBpp + 7);
            const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
            const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
            return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
                (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBpp;
        }

        // Byte offset of block (x, y, z) in a tiled 3D surface whose pitch and height (in
        // blocks) are aligned to 32. Volumes use 32x16x4-block macro tiles of 128 blocks; the
        // bank/pipe/row bits are scattered into the address as for 2D surfaces.
        uint64_t TiledOffset3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height, uint32_t blockBytes)
        {
            const uint32_t log2Bytes = blockBytes == 16 ? 4 : blockBytes == 8 ? 3 : blockBytes == 4 ? 2 : blockBytes == 2 ? 1 : 0;
            const uint64_t macro = ((uint64_t(z >> 2) * (height >> 4) + (y >> 4)) * (pitch >> 5) + (x >> 5)) << 7;
            const uint64_t micro = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
            const uint64_t linear = (macro | micro) << log2Bytes;
            const uint64_t bank = ((y >> 3) ^ (z >> 2)) & 1;
            const uint64_t pipe = ((x >> 3) & 3) ^ (bank << 1);
            return (linear & 0xF) | (((linear >> 4) & 1) << 5) | (((linear >> 5) & 7) << 8) | ((linear >> 8) << 12) |
                (uint64_t(y & 1) << 4) | (pipe << 6) | (bank << 11);
        }

        UINT EncodeMapping(uint32_t swizzle, const uint8_t remap[4])
        {
            UINT components[4];
            for (int i = 0; i < 4; i++)
            {
                const uint32_t select = (swizzle >> (i * 3)) & 7;
                if (select < 4)
                    components[i] = remap[select];
                else if (select == 5)
                    components[i] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
                else
                    components[i] = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
            }
            return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(components[0], components[1], components[2], components[3]);
        }

        void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            if (before == after)
                return;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            list->ResourceBarrier(1, &barrier);
        }

        constexpr D3D12_RESOURCE_STATES kShaderReadable =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // Physical address of a host pointer into one of the windows onto physical memory.
        uint32_t PhysicalOf(const uint8_t* host)
        {
            return uint32_t(host - g_base) & 0x1FFFFFFF;
        }

        // One stored level (or face, or slice) from guest memory on the GPU into the staging
        // buffer: rows of blocks (texels, or 4x4 blocks when compressed), untiled and with the
        // fetch constant's endian swap applied, as TiledOffset2D/TiledOffset3D and the CPU path do.
        // One thread per output word.
        constexpr uint32_t kUntileConstants = 12;
        const char* const kUntileShader = R"(
cbuffer Untile : register(b0)
{
    uint source;      // physical address of the stored level
    uint pitch;       // stored level width, in blocks
    uint rows;        // volumes: rows per slice
    uint unitBytes;   // bytes per block
    uint flags;       // bit 0 tiled, bit 1 3D tiling, bits 2-3 endian swap
    uint offsetX;     // packed mip offset, in blocks
    uint offsetY;
    uint slice;
    uint blocksWide;
    uint blocksHigh;
    uint dest;        // byte offset in the staging buffer
    uint rowPitch;
};

ByteAddressBuffer memory : register(t0);
RWByteAddressBuffer staging : register(u0);

uint TiledOffset2D(uint x, uint y, uint pitchUnits, uint bytesPerUnit)
{
    uint alignedPitch = (pitchUnits + 31) & ~31u;
    uint logBpp = (bytesPerUnit >> 2) + ((bytesPerUnit >> 1) >> (bytesPerUnit >> 2));
    uint macro = ((x >> 5) + (y >> 5) * (alignedPitch >> 5)) << (logBpp + 7);
    uint micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
    uint offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
    return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
        (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBpp;
}

uint TiledOffset3D(uint x, uint y, uint z, uint pitchBlocks, uint height, uint blockBytes)
{
    uint log2Bytes = blockBytes == 16 ? 4 : blockBytes == 8 ? 3 : blockBytes == 4 ? 2 : blockBytes == 2 ? 1 : 0;
    uint macro = (((z >> 2) * (height >> 4) + (y >> 4)) * (pitchBlocks >> 5) + (x >> 5)) << 7;
    uint micro = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
    uint offset = (macro | micro) << log2Bytes;  // not "linear": a keyword to D3DCompile
    uint bank = ((y >> 3) ^ (z >> 2)) & 1;
    uint pipe = ((x >> 3) & 3) ^ (bank << 1);
    return (offset & 0xF) | (((offset >> 4) & 1) << 5) | (((offset >> 5) & 7) << 8) | ((offset >> 8) << 12) |
        ((y & 1) << 4) | (pipe << 6) | (bank << 11);
}

uint BlockAddress(uint x, uint y)
{
    if ((flags & 1) == 0)
        return source + ((y + offsetY) * pitch + x + offsetX) * unitBytes;
    if (flags & 2)
        return source + TiledOffset3D(x, y, slice, pitch, rows, unitBytes);
    return source + TiledOffset2D(x + offsetX, y + offsetY, pitch, unitBytes) * unitBytes;
}

uint LoadSwapped(uint address)
{
    // Shift amounts that depend on the mode (0 leaves the word as it is): DXC turns a constant
    // byte reversal into llvm.bswap, which DXIL does not allow.
    uint v = memory.Load(address);
    uint mode = (flags >> 2) & 3;
    uint bytes = (mode == 1 || mode == 2) ? 8u : 0u;
    uint mask = bytes != 0 ? 0x00FF00FFu : 0xFFFFFFFFu;
    v = ((v & mask) << bytes) | ((v >> bytes) & mask);
    uint halves = mode >= 2 ? 16u : 0u;
    return (v << halves) | (v >> halves);
}

[numthreads(8, 8, 1)]
void cs_untile(uint3 id : SV_DispatchThreadID)
{
    uint rowBytes = blocksWide * unitBytes;
    if (id.x * 4 >= rowBytes || id.y >= blocksHigh)
        return;
    uint value = 0;
    if (unitBytes >= 4)
    {
        value = LoadSwapped(BlockAddress(id.x * 4 / unitBytes, id.y) + (id.x * 4) % unitBytes);
    }
    else
    {
        // Blocks of one or two bytes: gather the word's bytes from their blocks.
        for (uint b = 0; b < 4; b++)
        {
            uint byteX = id.x * 4 + b;
            if (byteX < rowBytes)
            {
                uint address = BlockAddress(byteX / unitBytes, id.y) + byteX % unitBytes;
                value |= ((LoadSwapped(address & ~3u) >> ((address & 3) * 8)) & 0xFF) << (b * 8);
            }
        }
    }
    staging.Store(dest + id.y * rowPitch + id.x * 4, value);
}
)";
    }

    // HC09_GPU_STATS counters: bytes hashed and textures (re)uploaded since the last report.
    std::atomic<uint64_t> g_textureBytesHashed{ 0 }, g_textureUploads{ 0 }, g_textureLookups{ 0 }, g_textureChecks{ 0 };

    TextureFetch ReadTextureFetch(uint32_t index)
    {
        TextureFetch fetch;
        for (uint32_t i = 0; i < 6; i++)
            fetch.words[i] = ReadRegister(0x4800 + index * 6 + i);
        return fetch;
    }

    namespace
    {
        constexpr int kEvictAfterFrames = 1800;  // ~1 minute without being sampled

        uint32_t ResolveTexelBytes(DXGI_FORMAT format)
        {
            return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        }

        // Fingerprint of the guest memory a resolve of this size covers (a tiled surface: 32x32-texel
        // aligned). Large regions are sampled: whole-texture loads change many of the samples.
        uint64_t MemoryFingerprint(uint32_t address, uint32_t width, uint32_t height, uint32_t texelBytes)
        {
            const uint32_t physical = address & 0x1FFFFFFF;
            uint64_t bytes = uint64_t((width + 31) & ~31u) * ((height + 31) & ~31u) * texelBytes;
            bytes = std::min<uint64_t>(bytes, 0x20000000ull - physical);
            const uint8_t* memory = GuestToHost(PhysicalToGuest(physical));
            constexpr uint64_t kChunk = 256, kChunks = 64;
            if (bytes <= kChunk * kChunks)
                return XXH3_64bits(memory, size_t(bytes));
            uint64_t hash = 0;
            const uint64_t step = (bytes - kChunk) / (kChunks - 1);
            for (uint64_t i = 0; i < kChunks; i++)
                hash = XXH3_64bits_withSeed(memory + i * step, kChunk, hash);
            return hash;
        }
    }

    void TextureCache::SetResolved(uint32_t address, ID3D12Resource* resource, DXGI_FORMAT format, uint32_t width, uint32_t height,
        bool swapRedBlue, uint32_t memoryTexelBytes)
    {
        Resolved resolved{ resource, format, width, height, swapRedBlue };
        resolved.texelBytes = memoryTexelBytes ? memoryTexelBytes : ResolveTexelBytes(format);
        resolved.memoryFingerprint = MemoryFingerprint(address, width, height, resolved.texelBytes);
        resolved.checkedFrame = FrameNumber();
        resolved.lastUsedFrame = FrameNumber();
        resolved_[address & 0x1FFFFFFF] = resolved;
        recent_.clear();
    }

    bool TextureCache::ResolveStillValid(uint32_t address, Resolved& resolved, int frame)
    {
        if (resolved.checkedFrame == frame)
            return true;
        resolved.checkedFrame = frame;
        if (MemoryFingerprint(address, resolved.width, resolved.height, resolved.texelBytes) == resolved.memoryFingerprint)
            return true;
        // The title has written other data over this address since the resolve.
        static const bool trace = std::getenv("HC09_GPU_STATS") != nullptr;
        if (trace)
            std::fprintf(stderr, "[texture] resolve at 0x%08X (%ux%u) is stale: the memory now holds other data (frame %d)\n", address,
                resolved.width, resolved.height, frame);
        staleResolves_++;
        ForgetResolved(address);
        return false;
    }

    int TextureCache::ResolvedLastUse(uint32_t address) const
    {
        auto it = resolved_.find(address & 0x1FFFFFFF);
        return it == resolved_.end() ? -1 : it->second.lastUsedFrame;
    }

    bool TextureCache::ResolvedMemoryChanged(uint32_t address) const
    {
        auto it = resolved_.find(address & 0x1FFFFFFF);
        return it == resolved_.end() ||
            MemoryFingerprint(it->first, it->second.width, it->second.height, it->second.texelBytes) != it->second.memoryFingerprint;
    }

    void TextureCache::ForgetResolved(uint32_t address)
    {
        address &= 0x1FFFFFFF;
        resolved_.erase(address);
        recent_.clear();
        if (auto it = composed_.find(address); it != composed_.end())
        {
            retired_.emplace_back(FrameNumber(), std::move(it->second.resource));
            composed_.erase(it);
        }
    }

    void TextureCache::Evict(int frame)
    {
        std::erase_if(retired_, [&](const auto& item) { return frame - item.first > 4; });
        if (frame % 300 != 0)
            return;
        // Resources may still be referenced by command lists in flight: retire, release later.
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (frame - it->second.hashedFrame > kEvictAfterFrames)
            {
                retired_.emplace_back(frame, std::move(it->second.resource));
                it = entries_.erase(it);
                evictedTextures_++;
            }
            else
            {
                ++it;
            }
        }
    }

    ID3D12Resource* TextureCache::FindResolved(uint32_t address, uint32_t* width, uint32_t* height) const
    {
        auto it = resolved_.find(address & 0x1FFFFFFF);
        if (it == resolved_.end())
            return nullptr;
        if (width)
            *width = it->second.width;
        if (height)
            *height = it->second.height;
        return it->second.resource;
    }

    uint32_t TextureCache::ComposeResolvedMips(const TextureFetch& fetch, const Resolved& base, BoundTexture& out)
    {
        FormatInfo info;
        if (fetch.mipAddress() == 0 || fetch.mipMaxLevel() == 0 || !LookupFormat(fetch.format(), info))
            return 1;
        const uint32_t width = fetch.width(), height = fetch.height();
        auto log2Ceil = [](uint32_t value) {
            uint32_t log2 = 0;
            while ((1u << log2) < value)
                log2++;
            return log2;
        };
        const uint32_t log2W = log2Ceil(width), log2H = log2Ceil(height);
        // Levels at or past the packed mip tail share storage; only separately stored levels are
        // composed (each was resolved on its own).
        const uint32_t shorterLog2 = std::min(log2W, log2H);
        const uint32_t packedBase = !fetch.packedMips() ? 32u : shorterLog2 > 4 ? shorterLog2 - 4 : 0u;
        if (packedBase == 0)
            return 1;
        uint32_t sizeMax = 0;
        while ((std::max(width, height) >> (sizeMax + 1)) != 0)
            sizeMax++;
        const uint32_t wantedLevels = std::min({ fetch.mipMaxLevel(), sizeMax, packedBase - 1 }) + 1;

        // Find the levels the title resolved: level 1 at the mip address, each following level after
        // the previous one's storage (power-of-two pitch and rows, 32-block aligned, 4 KiB aligned).
        const uint32_t blockSize = info.compressed ? 4 : 1;
        std::vector<const Resolved*> levels{ &base };
        uint32_t address = fetch.mipAddress() & 0x1FFFFFFF;
        for (uint32_t level = 1; level < wantedLevels; level++)
        {
            auto found = resolved_.find(address);
            if (found == resolved_.end() || found->second.format != base.format || !ResolveStillValid(found->first, found->second, FrameNumber()))
                break;
            found->second.lastUsedFrame = FrameNumber();
            levels.push_back(&found->second);
            uint32_t pitch = (std::max((1u << log2W) >> level, 1u) + blockSize - 1) / blockSize;
            uint32_t rows = (std::max((1u << log2H) >> level, 1u) + blockSize - 1) / blockSize;
            pitch = (pitch + 31) & ~31u;
            rows = (rows + 31) & ~31u;
            address += (pitch * rows * info.blockBytes + 4095) & ~4095u;
        }
        if (levels.size() < 2)
            return 1;

        // The composed texture follows the base's host size (resolution scaling applies).
        const D3D12_RESOURCE_DESC baseDesc = base.resource->GetDesc();
        Composed& composed = composed_[fetch.baseAddress() & 0x1FFFFFFF];
        const uint32_t levelCount = uint32_t(levels.size());
        const int frame = FrameNumber();
        // Replaced textures may still be referenced by command lists in flight: release them a few
        // frames later.
        std::erase_if(retired_, [&](const auto& item) { return frame - item.first > 4; });
        if (!composed.resource || composed.format != base.format || composed.width != baseDesc.Width ||
            composed.height != baseDesc.Height || composed.levels != levelCount)
        {
            if (composed.resource)
                retired_.emplace_back(frame, std::move(composed.resource));
            composed = {};
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc = baseDesc;
            desc.MipLevels = UINT16(levelCount);
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    kShaderReadable, nullptr, IID_PPV_ARGS(&composed.resource))))
                return 1;
            composed.format = base.format;
            composed.width = uint32_t(baseDesc.Width);
            composed.height = baseDesc.Height;
            composed.levels = levelCount;
        }

        // Refresh the copy once per frame (the levels are re-rendered whenever the title updates them).
        // Resolve destinations are kept shader-readable between uses, so each copy borrows the
        // source and returns it in that state.
        if (composed.copiedFrame != frame)
        {
            composed.copiedFrame = frame;
            auto* list = context_.list();
            Transition(list, composed.resource.Get(), kShaderReadable, D3D12_RESOURCE_STATE_COPY_DEST);
            for (uint32_t level = 0; level < levelCount; level++)
            {
                ID3D12Resource* source = levels[level]->resource;
                const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
                const UINT copyWidth = std::min<UINT>(std::max<UINT>(composed.width >> level, 1), UINT(sourceDesc.Width));
                const UINT copyHeight = std::min<UINT>(std::max<UINT>(composed.height >> level, 1), sourceDesc.Height);
                Transition(list, source, kShaderReadable, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{ composed.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                dst.SubresourceIndex = level;
                D3D12_TEXTURE_COPY_LOCATION src{ source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                const D3D12_BOX box{ 0, 0, 0, copyWidth, copyHeight, 1 };
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, kShaderReadable);
            }
            Transition(list, composed.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderReadable);
        }
        out.resource = composed.resource.Get();
        out.view.Texture2D.MipLevels = levelCount;
        return levelCount;
    }

    void TextureCache::Remember(uint64_t key, const TextureFetch& fetch, const BoundTexture& texture)
    {
        recent_[key] = { fetch, texture };
    }

    bool TextureCache::Get(const TextureFetch& fetch, BoundTexture& out)
    {
        const int frameNow = FrameNumber();
        if (recentFrame_ != frameNow)
        {
            recent_.clear();
            recentFrame_ = frameNow;
        }
        const uint64_t recentKey = XXH3_64bits(fetch.words, sizeof(fetch.words));
        if (auto found = recent_.find(recentKey);
            found != recent_.end() && std::memcmp(found->second.fetch.words, fetch.words, sizeof(fetch.words)) == 0)
        {
            g_textureLookups++;
            out = found->second.texture;
            return true;
        }
        const uint32_t format = fetch.format();
        FormatInfo info;
        const bool cube = fetch.dimension() == 3;
        const bool volume = fetch.dimension() == 2;
        // Stacked textures (2D arrays) are kept as 3D textures, one slice per layer.
        const bool stacked = fetch.dimension() == 1 && fetch.stacked() && fetch.stackDepth() > 1;
        // Depth formats (k_24_8, k_24_8_FLOAT) come from depth resolves, stored as R32_FLOAT.
        if ((format == 22 || format == 23) && fetch.dimension() == 1)
        {
            auto it = resolved_.find(fetch.baseAddress() & 0x1FFFFFFF);
            if (it == resolved_.end() || !ResolveStillValid(it->first, it->second, FrameNumber()))
                return false;
            it->second.lastUsedFrame = FrameNumber();
            out = {};
            out.width = it->second.width;
            out.height = it->second.height;
            out.resource = it->second.resource;
            out.view.Format = it->second.format;
            out.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            out.view.Texture2D.MipLevels = 1;
            out.view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
            return true;
        }
        if (fetch.type() != 2 || fetch.dimension() == 0 || !LookupFormat(format, info))
        {
            if (!loggedUnsupported_[format])
            {
                loggedUnsupported_[format] = true;
                std::fprintf(stderr, "[texture] unsupported: format %u dimension %u\n", format, fetch.dimension());
            }
            return false;
        }

        const uint32_t width = volume ? fetch.width3D() : fetch.width();
        const uint32_t height = volume ? fetch.height3D() : fetch.height();
        const uint32_t depth = volume ? fetch.depth3D() : stacked ? fetch.stackDepth() : 1;
        out.depth = depth;
        out.stacked = stacked;
        out.width = width;
        out.height = height;
        out.view = {};
        out.view.Format = info.dxgi;
        if (cube)
        {
            out.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            out.view.TextureCube.MipLevels = 1;
        }
        else if (volume || stacked)
        {
            out.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            out.view.Texture3D.MipLevels = 1;
        }
        else
        {
            out.view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            out.view.Texture2D.MipLevels = 1;
        }
        out.view.Shader4ComponentMapping = EncodeMapping(fetch.swizzle(), info.remap);

        if (!cube && !volume && !stacked && GetReplacement(fetch, out))
        {
            Remember(recentKey, fetch, out);
            return true;
        }

        // Render-to-texture: a resolve wrote this address. A fetch with another texel size than the
        // resolve wrote to memory (or a compressed format) cannot be reading that resolve; neither
        // can one after the title has loaded other data over it. A resolve to a one-channel format
        // (k_8, written from the target's red) is read through the target's red channel.
        auto it = resolved_.find(fetch.baseAddress() & 0x1FFFFFFF);
        if (it != resolved_.end() && (cube || volume || stacked || info.compressed || info.blockBytes != it->second.texelBytes))
        {
            mismatchedResolveHits_++;
            static std::set<uint64_t> logged;
            const uint64_t id = (uint64_t(it->first) << 32) ^ (uint64_t(width) << 16) ^ height ^ (uint64_t(format) << 26);
            if (std::getenv("HC09_GPU_STATS") && logged.size() < 40 && logged.insert(id).second)
                std::fprintf(stderr, "[texture] fetch 0x%08X fmt %u %ux%u does not match the resolve there (%ux%u dxgi %d); sampling memory (frame %d)\n",
                    fetch.baseAddress(), format, width, height, it->second.width, it->second.height, int(it->second.format), FrameNumber());
            it = resolved_.end();
        }
        if (it != resolved_.end() && ResolveStillValid(it->first, it->second, FrameNumber()))
        {
            it->second.lastUsedFrame = FrameNumber();
            out.resource = it->second.resource;
            out.view.Format = it->second.format;
            // The target holds red, green, blue, alpha; memory has red and blue exchanged when
            // the resolve swapped them, and the fetch swizzle applies to the memory order.
            const uint8_t identity[4] = { 0, 1, 2, 3 };
            const uint8_t swapped[4] = { 2, 1, 0, 3 };
            out.view.Shader4ComponentMapping = EncodeMapping(fetch.swizzle(), it->second.swapRedBlue ? swapped : identity);
            ComposeResolvedMips(fetch, it->second, out);
            return true;
        }

        // Source layout in guest memory: rows of blocks (or texels).
        const uint32_t blockSize = info.compressed ? 4 : 1;
        const uint32_t unit = info.blockBytes;
        const uint32_t blocksWide = (width + blockSize - 1) / blockSize;
        const uint32_t blocksHigh = (height + blockSize - 1) / blockSize;
        const uint32_t pitchBlocks = std::max(fetch.pitchTexels() / blockSize, blocksWide);
        const uint32_t sourceRows = fetch.tiled() ? ((blocksHigh + 31) & ~31u) : blocksHigh;
        const uint32_t sourcePitchBlocks = fetch.tiled() ? ((pitchBlocks + 31) & ~31u) : pitchBlocks;
        // Cube maps store their six faces one after another, each aligned to 4 KiB.
        // Volumes store depth slices (as a 3D-tiled surface when tiled); their slice stride uses
        // the height rounded up to 32 blocks.
        const uint32_t faces = cube ? 6 : (volume || stacked) ? depth : 1;
        const uint32_t volumeRows = (blocksHigh + 31) & ~31u;
        const uint64_t faceBytes = volume ? uint64_t(sourcePitchBlocks) * volumeRows * unit
                                          : uint64_t(sourcePitchBlocks) * sourceRows * unit;
        const uint64_t faceStride = volume ? faceBytes : (faceBytes + 4095) & ~4095ull;
        const uint64_t sourceBytes = volume && fetch.tiled()
            ? uint64_t(sourcePitchBlocks) * volumeRows * ((depth + 3) & ~3u) * unit
            : faceStride * (faces - 1) + faceBytes;
        const uint8_t* source = GuestToHost(PhysicalToGuest(fetch.baseAddress()));

        // Mip levels 1..n of 2D textures and cube maps live at the fetch constant's mip address.
        // A stored level's pitch and height are the base size rounded up to a power of two and
        // shifted by the level, aligned to 32x32-block tiles (linear: 256-byte rows); each level
        // (per face) is aligned to 4 KiB and the levels follow each other. With packed mips,
        // every level whose shorter side is 16 texels or less lives inside one stored level at a
        // fixed offset (the packed mip tail).
        uint32_t levels = 1;
        if (!volume && !stacked && fetch.mipAddress() != 0 && fetch.mipMaxLevel() > 0)
        {
            uint32_t sizeMax = 0;
            while ((std::max(width, height) >> (sizeMax + 1)) != 0)
                sizeMax++;
            levels = std::min(fetch.mipMaxLevel(), sizeMax) + 1;
        }
        auto log2Ceil = [](uint32_t value) {
            uint32_t log2 = 0;
            while ((1u << log2) < value)
                log2++;
            return log2;
        };
        const uint32_t log2W = log2Ceil(width), log2H = log2Ceil(height);
        const uint32_t shorterLog2 = std::min(log2W, log2H);
        const uint32_t packedBase = !fetch.packedMips() ? 32u : shorterLog2 > 4 ? shorterLog2 - 4 : 0u;
        struct LevelLayout
        {
            const uint8_t* data;        // the stored level holding this one (face 0)
            uint32_t pitch, rows;       // of the stored level, in blocks
            uint64_t faceStride;
            uint32_t x, y;              // packed offset inside the stored level, in blocks
            uint32_t blocksWide, blocksHigh;
        };
        std::vector<LevelLayout> layouts(levels);
        const uint8_t* mipSource = levels > 1 ? GuestToHost(PhysicalToGuest(fetch.mipAddress())) : nullptr;
        uint64_t mipBytes = 0;
        {
            uint64_t offset = 0;
            LevelLayout stored{ source, sourcePitchBlocks, sourceRows, faceStride, 0, 0, 0, 0 };
            for (uint32_t level = 0; level < levels; level++)
            {
                if (level > 0 && level <= packedBase)
                {
                    // A new stored level (levels past the packed base share its storage).
                    const uint32_t texelPitch = std::max((1u << log2W) >> level, 1u);
                    const uint32_t texelRows = std::max((1u << log2H) >> level, 1u);
                    uint32_t pitch = (texelPitch + blockSize - 1) / blockSize;
                    uint32_t rows = (texelRows + blockSize - 1) / blockSize;
                    if (fetch.tiled())
                    {
                        pitch = (pitch + 31) & ~31u;
                        rows = (rows + 31) & ~31u;
                    }
                    else
                    {
                        pitch = ((pitch * unit + 255) & ~255u) / unit;
                    }
                    const uint64_t stride = (uint64_t(pitch) * rows * unit + 4095) & ~4095ull;
                    stored = { mipSource + offset, pitch, rows, stride, 0, 0, 0, 0 };
                    offset += stride * faces;
                    mipBytes = offset;
                }
                LevelLayout& layout = layouts[level];
                layout = stored;
                layout.blocksWide = (std::max(width >> level, 1u) + blockSize - 1) / blockSize;
                layout.blocksHigh = (std::max(height >> level, 1u) + blockSize - 1) / blockSize;
                if (level >= packedBase)
                {
                    const uint32_t packed = level - packedBase;
                    const bool wide = log2W > log2H;
                    uint32_t x = 0, y = 0;
                    if (packed < 3)
                        (wide ? y : x) = 16u >> packed;
                    else
                        (wide ? x : y) = (1u << ((wide ? log2W : log2H) - packedBase)) >> (packed - 2);
                    layout.x = x / blockSize;
                    layout.y = y / blockSize;
                }
            }
        }
        if (cube)
            out.view.TextureCube.MipLevels = levels;
        else if (!volume && !stacked)
            out.view.Texture2D.MipLevels = levels;

        const uint64_t key = (uint64_t(fetch.baseAddress()) << 32) ^ (uint64_t(format) << 26) ^ (uint64_t(width) << 13) ^ height ^
            (uint64_t(fetch.tiled()) << 63) ^ (uint64_t(fetch.endian()) << 61) ^ (uint64_t(pitchBlocks) << 40) ^ (uint64_t(cube) << 60) ^
            (uint64_t(depth) << 50) ^ (uint64_t(stacked) << 59) ^ (uint64_t(levels) << 54) ^ (uint64_t(fetch.mipAddress()) >> 3);
        Entry& entry = entries_[key];
        auto* list = context_.list();
        if (!entry.resource)
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = (volume || stacked) ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = info.compressed ? blocksWide * 4 : width;
            desc.Height = info.compressed ? blocksHigh * 4 : height;
            desc.DepthOrArraySize = UINT16(faces);
            desc.MipLevels = UINT16(levels);
            desc.Format = info.dxgi;
            desc.SampleDesc.Count = 1;
            if (FAILED(context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&entry.resource))))
            {
                entries_.erase(key);
                return false;
            }
            entry.state = D3D12_RESOURCE_STATE_COPY_DEST;
            entry.contentHash = ~0ull;
        }

        // Hashing large textures on every draw dominates frame time, so a texture's contents
        // are checked once per frame (guest writes to it mid-frame are picked up next frame).
        auto fullHash = [&] {
            uint64_t value = XXH3_64bits(source, sourceBytes);
            g_textureBytesHashed += sourceBytes;
            if (mipBytes)
            {
                value ^= XXH3_64bits(mipSource, mipBytes) * 0x9E3779B97F4A7C15ull;
                g_textureBytesHashed += mipBytes;
            }
            return value;
        };
        const int frame = FrameNumber();
        const bool check = entry.hashedFrame != frame;
        if (check)
            g_textureChecks++;
        entry.hashedFrame = frame;
        uint64_t hash = entry.contentHash;
        if (check && sharedMemory_.enabled())
        {
            // Guest memory on the GPU watches its pages for writes, so a texture is hashed only after
            // its memory was written (the hash still decides: rewriting the same image uploads nothing).
            const uint64_t version = std::max(sharedMemory_.Request(PhysicalOf(source), uint32_t(sourceBytes), true),
                mipBytes ? sharedMemory_.Request(PhysicalOf(mipSource), uint32_t(mipBytes), true) : 0);
            if (version != entry.memoryVersion || entry.contentHash == ~0ull)
                hash = fullHash();
            entry.memoryVersion = version;
        }
        else if (check)
        {
            // Large textures: a sampled hash (64 chunks spread over the base level) every frame,
            // which catches whole-image updates such as video, and a full hash every 60 frames.
            // One that has changed since its first upload is hashed in full every frame: the
            // samples miss small updates such as the text glyph cache (a 256x512 texture the game
            // adds each new character to), whose new text would otherwise show other glyphs.
            constexpr uint64_t kFullHashLimit = 256 * 1024, kChunk = 256, kChunks = 64;
            if (sourceBytes + mipBytes <= kFullHashLimit || entry.resource == nullptr || frame % 60 == int(entry.contentHash % 60) ||
                entry.contentHash == ~0ull || entry.rewritten)
            {
                hash = fullHash();
                static const bool trace = std::getenv("HC09_GPU_STATS") != nullptr;
                if (trace && entry.sampledHash != 0 && hash != entry.contentHash)
                    std::fprintf(stderr, "[texture] 0x%08X %ux%u fmt %u changed unseen by its samples (frame %d)\n",
                        fetch.baseAddress(), width, height, format, frame);
                entry.sampledHash = 0;
            }
            else
            {
                uint64_t sampled = 0;
                const uint64_t step = (sourceBytes - kChunk) / (kChunks - 1);
                for (uint64_t i = 0; i < kChunks; i++)
                    sampled = XXH3_64bits_withSeed(source + i * step, kChunk, sampled);
                if (entry.sampledHash != 0 && sampled != entry.sampledHash)
                    hash = fullHash();  // changed: confirm with a full hash
                entry.sampledHash = sampled;
            }
        }
        g_textureLookups++;
        if (hash != entry.contentHash)
        {
            g_textureUploads++;
            entry.rewritten = entry.contentHash != ~0ull;
            entry.contentHash = hash;
            // HC09_DUMP_MIPS=<width>x<height>f<format>[,...]: save every level of matching textures as
            // uploaded (untiled, byte-swapped, rows packed) to build/mip_<frame>_<base>_<level>_<w>x<h>.bin.
            std::FILE* mipDump[16]{};
            static const char* mipDumpSpec = std::getenv("HC09_DUMP_MIPS");
            if (MatchesTextureSpec(mipDumpSpec, width, height, fetch.format()))
            {
                for (uint32_t level = 0; level < levels && level < 16; level++)
                {
                    char path[96];
                    std::snprintf(path, sizeof(path), "build/mip_%d_%08X_%u_%ux%u.bin", frame, fetch.baseAddress(), level,
                        layouts[level].blocksWide, layouts[level].blocksHigh);
                    mipDump[level] = std::fopen(path, "wb");
                }
            }
            // Some textures (the field's grass detail noise) store levels 1..n that are not downscales of
            // level 0 but fresh full-contrast noise, which makes distant surfaces sparkle and repeat.
            // For 8-bit-per-channel 2D textures, level 1 is compared with a box-filtered level 0, and
            // when they are unrelated every level is rebuilt from level 0 instead.
            // HC09_ORIGINAL_MIPS=1 keeps the stored levels.
            static const bool originalMips = std::getenv("HC09_ORIGINAL_MIPS") != nullptr;
            const bool rebuildCandidate = !originalMips && !info.compressed && levels > 1 && faces == 1 && !volume && !stacked &&
                (info.dxgi == DXGI_FORMAT_R8_UNORM || info.dxgi == DXGI_FORMAT_R8G8_UNORM || info.dxgi == DXGI_FORMAT_R8G8B8A8_UNORM);
            bool rebuild = false;
            std::vector<uint8_t> previous;  // the level just uploaded (rows packed), for candidates
            std::vector<uint8_t> filtered;  // box-filtered previous level
            uint32_t previousWide = 0, previousHigh = 0;
            // HC09_DUMP_HASHED=1: save each 2D texture's base level once per content as
            // build/hashed/<content hash>.png, in the channel order a replacement of it uses.
            static const bool dumpHashedEnabled = std::getenv("HC09_DUMP_HASHED") != nullptr;
            static std::set<uint64_t> dumpedHashes;
            const bool dumpHashed = dumpHashedEnabled && !cube && !volume && !stacked && dumpedHashes.insert(hash).second;
            std::vector<uint8_t> dumpRows;
            Transition(list, entry.resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
            entry.state = D3D12_RESOURCE_STATE_COPY_DEST;
            // With guest memory on the GPU, a compute shader untiles the levels; the CPU still does it
            // for textures whose mips may be rebuilt (it compares them) and for the dumps.
            bool untiled = false;
            const bool dumpingMips = std::any_of(std::begin(mipDump), std::end(mipDump), [](std::FILE* file) { return file != nullptr; });
            if (sharedMemory_.enabled() && !rebuildCandidate && !dumpHashed && !dumpingMips && PrepareUntile())
            {
                std::vector<UntileLevel> jobs;
                uint64_t bytes = 0;
                for (uint32_t faceIndex = 0; faceIndex < faces; faceIndex++)
                {
                    for (uint32_t level = 0; level < levels; level++)
                    {
                        const LevelLayout& layout = layouts[level];
                        UntileLevel job{};
                        if (volume && fetch.tiled())
                        {
                            job.source = PhysicalOf(source);
                            job.pitch = sourcePitchBlocks;
                            job.rows = volumeRows;
                            job.slice = faceIndex;
                        }
                        else
                        {
                            job.source = PhysicalOf(layout.data + layout.faceStride * faceIndex);
                            job.pitch = layout.pitch;
                            job.rows = layout.rows;
                            job.offsetX = layout.x;
                            job.offsetY = layout.y;
                        }
                        job.blocksWide = layout.blocksWide;
                        job.blocksHigh = layout.blocksHigh;
                        job.rowPitch = (layout.blocksWide * unit + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
                        bytes = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
                        job.staging = bytes;
                        bytes += uint64_t(job.rowPitch) * layout.blocksHigh;
                        jobs.push_back(job);
                    }
                }
                untiled = Untile(jobs, unit, fetch.tiled(), volume, fetch.endian(), bytes);
                for (uint32_t i = 0; untiled && i < jobs.size(); i++)
                {
                    const uint32_t faceIndex = i / levels, level = i % levels;
                    D3D12_TEXTURE_COPY_LOCATION dst{ entry.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    dst.SubresourceIndex = (volume || stacked) ? 0 : level + faceIndex * levels;
                    D3D12_TEXTURE_COPY_LOCATION src{ staging_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    src.PlacedFootprint.Offset = jobs[i].staging;
                    src.PlacedFootprint.Footprint = { info.dxgi,
                        info.compressed ? jobs[i].blocksWide * 4 : std::max(width >> level, 1u),
                        info.compressed ? jobs[i].blocksHigh * 4 : std::max(height >> level, 1u), 1, jobs[i].rowPitch };
                    list->CopyTextureRegion(&dst, 0, 0, (volume || stacked) ? faceIndex : 0, &src, nullptr);
                }
            }
            for (uint32_t faceIndex = 0; faceIndex < faces && !untiled; faceIndex++)
            {
                for (uint32_t level = 0; level < levels; level++)
                {
                    // Untile / unswap into an upload buffer laid out for CopyTextureRegion.
                    const LevelLayout& layout = layouts[level];
                    const uint32_t rowBytes = layout.blocksWide * unit;
                    const uint32_t rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
                    const uint8_t* faceSource = layout.data + layout.faceStride * faceIndex;
                    auto upload = context_.Upload(uint64_t(rowPitch) * layout.blocksHigh, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
                    auto* dest = static_cast<uint8_t*>(upload.cpu);
                    for (uint32_t y = 0; y < layout.blocksHigh; y++)
                    {
                        uint8_t* row = dest + uint64_t(y) * rowPitch;
                        if (!fetch.tiled())
                        {
                            std::memcpy(row, faceSource + (uint64_t(y + layout.y) * layout.pitch + layout.x) * unit, rowBytes);
                        }
                        else if (volume)
                        {
                            for (uint32_t x = 0; x < layout.blocksWide; x++)
                                std::memcpy(row + x * unit, source + TiledOffset3D(x, y, faceIndex, sourcePitchBlocks, volumeRows, unit), unit);
                        }
                        else
                        {
                            for (uint32_t x = 0; x < layout.blocksWide; x++)
                                std::memcpy(row + x * unit,
                                    faceSource + uint64_t(TiledOffset2D(x + layout.x, y + layout.y, layout.pitch, unit)) * unit, unit);
                        }
                        if (fetch.endian() != 0)
                        {
                            for (uint32_t i = 0; i + 4 <= rowBytes; i += 4)
                            {
                                uint32_t word;
                                std::memcpy(&word, row + i, 4);
                                word = GpuSwap(word, fetch.endian());
                                std::memcpy(row + i, &word, 4);
                            }
                        }
                        if (dumpHashed && faceIndex == 0 && level == 0)
                            dumpRows.insert(dumpRows.end(), row, row + rowBytes);
                        if (faceIndex == 0 && level < 16 && mipDump[level])
                            std::fwrite(row, 1, rowBytes, mipDump[level]);
                    }
                    if (rebuildCandidate && (level == 0 || rebuild || level == 1))
                    {
                        if (level > 0)
                        {
                            // Average each 2x2 footprint of the previous level (edges clamp on odd sizes).
                            filtered.resize(uint64_t(rowBytes) * layout.blocksHigh);
                            for (uint32_t y = 0; y < layout.blocksHigh; y++)
                            {
                                const uint32_t y0 = std::min(y * 2, previousHigh - 1), y1 = std::min(y * 2 + 1, previousHigh - 1);
                                for (uint32_t x = 0; x < layout.blocksWide; x++)
                                {
                                    const uint32_t x0 = std::min(x * 2, previousWide - 1), x1 = std::min(x * 2 + 1, previousWide - 1);
                                    for (uint32_t c = 0; c < unit; c++)
                                    {
                                        const auto at = [&](uint32_t px, uint32_t py) {
                                            return uint32_t(previous[(uint64_t(py) * previousWide + px) * unit + c]);
                                        };
                                        filtered[(uint64_t(y) * layout.blocksWide + x) * unit + c] =
                                            uint8_t((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
                                    }
                                }
                            }
                            if (level == 1)
                            {
                                // Correlate the stored level 1 with the filtered level 0, per channel; any
                                // channel with real contrast that does not match marks the chain unrelated.
                                for (uint32_t c = 0; c < unit && !rebuild; c++)
                                {
                                    double sumA = 0, sumB = 0, sumAA = 0, sumBB = 0, sumAB = 0;
                                    const uint64_t count = uint64_t(layout.blocksWide) * layout.blocksHigh;
                                    for (uint32_t y = 0; y < layout.blocksHigh; y++)
                                        for (uint32_t x = 0; x < layout.blocksWide; x++)
                                        {
                                            const double a = filtered[(uint64_t(y) * layout.blocksWide + x) * unit + c];
                                            const double b = dest[uint64_t(y) * rowPitch + x * unit + c];
                                            sumA += a, sumB += b, sumAA += a * a, sumBB += b * b, sumAB += a * b;
                                        }
                                    const double varA = sumAA - sumA * sumA / count, varB = sumBB - sumB * sumB / count;
                                    const double cov = sumAB - sumA * sumB / count;
                                    // Standard deviation above 8 (of 255) in both, correlation under 0.25.
                                    if (count >= 64 && varA > 64.0 * count && varB > 64.0 * count && cov < 0.25 * std::sqrt(varA * varB))
                                        rebuild = true;
                                }
                                if (rebuild)
                                    std::fprintf(stderr, "[gpu] texture 0x%08X (%ux%u format %u): stored mips do not match level 0; rebuilding them\n",
                                        fetch.baseAddress(), width, height, fetch.format());
                            }
                            if (rebuild)
                                for (uint32_t y = 0; y < layout.blocksHigh; y++)
                                    std::memcpy(dest + uint64_t(y) * rowPitch, filtered.data() + uint64_t(y) * rowBytes, rowBytes);
                        }
                        previous.resize(uint64_t(rowBytes) * layout.blocksHigh);
                        for (uint32_t y = 0; y < layout.blocksHigh; y++)
                            std::memcpy(previous.data() + uint64_t(y) * rowBytes, dest + uint64_t(y) * rowPitch, rowBytes);
                        previousWide = layout.blocksWide;
                        previousHigh = layout.blocksHigh;
                    }

                    D3D12_TEXTURE_COPY_LOCATION dst{ entry.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    dst.SubresourceIndex = (volume || stacked) ? 0 : level + faceIndex * levels;
                    D3D12_TEXTURE_COPY_LOCATION src{ upload.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    src.PlacedFootprint.Offset = upload.offset;
                    src.PlacedFootprint.Footprint = { info.dxgi,
                        info.compressed ? layout.blocksWide * 4 : std::max(width >> level, 1u),
                        info.compressed ? layout.blocksHigh * 4 : std::max(height >> level, 1u), 1, rowPitch };
                    list->CopyTextureRegion(&dst, 0, 0, (volume || stacked) ? faceIndex : 0, &src, nullptr);
                }
            }
            for (std::FILE* file : mipDump)
                if (file)
                    std::fclose(file);
            std::vector<uint8_t> pixels;
            if (dumpHashed && !DecodeTexels(format, info, dumpRows.data(), blocksWide * unit, width, height, pixels))
                std::fprintf(stderr, "[texture] not dumped %016llX: format %u %ux%u\n", (unsigned long long)hash, format, width, height);
            else if (dumpHashed)
            {
                // As the shader sees it (the fetch swizzle applied), which is how replacements are drawn.
                for (size_t i = 0; i < pixels.size(); i += 4)
                {
                    uint8_t texel[4];
                    for (int c = 0; c < 4; c++)
                    {
                        const uint32_t select = (fetch.swizzle() >> (c * 3)) & 7;
                        texel[c] = select < 4 ? pixels[i + select] : select == 5 ? 255 : 0;
                    }
                    std::memcpy(&pixels[i], texel, 4);
                }
                CreateDirectoryW(L"build\\hashed", nullptr);
                wchar_t path[64];
                std::swprintf(path, 64, L"build\\hashed\\%016llX.png", (unsigned long long)hash);
                SavePng(path, width, height, pixels);
                const auto image = FindSwappableImage(fetch.baseAddress() & 0x1FFFFFFF);
                std::fprintf(stderr, "[texture] dumped %016llX: format %u swizzle 0x%03X %s/%u\n", (unsigned long long)hash, format,
                    fetch.swizzle(), image ? image->library.c_str() : "-", image ? image->id : 0);
            }
        }
        Transition(list, entry.resource.Get(), entry.state, kShaderReadable);
        entry.state = kShaderReadable;
        out.resource = entry.resource.Get();
        // HC09_TRACE_HASHES=<frame>: the content hash of each texture drawn in that frame.
        static const int traceHashesFrame = std::getenv("HC09_TRACE_HASHES") ? std::atoi(std::getenv("HC09_TRACE_HASHES")) : -1;
        if (frame == traceHashesFrame)
        {
            static std::set<uint64_t> traced;
            if (traced.insert(entry.contentHash).second)
                std::fprintf(stderr, "[texture] frame %d draws %016llX (%ux%u format %u)\n", frame,
                    (unsigned long long)entry.contentHash, width, height, format);
        }
        if (!cube && !volume && !stacked)
            GetHashedReplacement(entry.contentHash, fetch, levels, out);
        Remember(recentKey, fetch, out);
        return true;
    }

    bool TextureCache::PrepareUntile()
    {
        if (untileState_ != 0)
            return untileState_ > 0;
        untileState_ = -1;
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, kUntileConstants };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[1].Descriptor = { 0, 0 };
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[2].Descriptor = { 0, 0 };
        const D3D12_ROOT_SIGNATURE_DESC desc{ 3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> blob, errors;
        ComPtr<ID3DBlob> shader = CompileShader(kUntileShader, ShaderStage::Compute, "cs_untile", "untile");
        if (!shader || FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(context_.device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&untileRootSignature_))))
        {
            std::fprintf(stderr, "[texture] untile shader unavailable; textures are untiled on the CPU\n");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = untileRootSignature_.Get();
        pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        if (FAILED(context_.device()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&untilePipeline_))))
        {
            std::fprintf(stderr, "[texture] untile pipeline failed; textures are untiled on the CPU\n");
            return false;
        }
        untileState_ = 1;
        return true;
    }

    bool TextureCache::Untile(const std::vector<UntileLevel>& levels, uint32_t unitBytes, bool tiled, bool volume, uint32_t endian,
        uint64_t bytes)
    {
        auto* list = context_.list();
        if (stagingSize_ < bytes)
        {
            // Grown by doubling; the old buffer may still be in use by command lists in flight.
            if (staging_)
                retired_.emplace_back(FrameNumber(), std::move(staging_));
            uint64_t size = std::max<uint64_t>(stagingSize_, 4ull << 20);
            while (size < bytes)
                size *= 2;
            stagingSize_ = 0;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                    IID_PPV_ARGS(&staging_))))
                return false;
            stagingSize_ = size;
            stagingState_ = D3D12_RESOURCE_STATE_COMMON;
        }
        Transition(list, staging_.Get(), stagingState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        stagingState_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->SetComputeRootSignature(untileRootSignature_.Get());
        list->SetPipelineState(untilePipeline_.Get());
        list->SetComputeRootShaderResourceView(1, sharedMemory_.buffer()->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(2, staging_->GetGPUVirtualAddress());
        const uint32_t flags = (tiled ? 1u : 0u) | (tiled && volume ? 2u : 0u) | (endian << 2);
        for (const UntileLevel& level : levels)
        {
            const uint32_t constants[kUntileConstants] = { level.source, level.pitch, level.rows, unitBytes, flags, level.offsetX,
                level.offsetY, level.slice, level.blocksWide, level.blocksHigh, uint32_t(level.staging), level.rowPitch };
            list->SetComputeRoot32BitConstants(0, kUntileConstants, constants, 0);
            const uint32_t rowWords = (level.blocksWide * unitBytes + 3) / 4;
            list->Dispatch((rowWords + 7) / 8, (level.blocksHigh + 7) / 8, 1);
        }
        Transition(list, staging_.Get(), stagingState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        stagingState_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
        return true;
    }

    bool TextureCache::GetReplacement(const TextureFetch& fetch, BoundTexture& out)
    {
        // HC09_NO_REPLACEMENTS=1 draws the game's own images.
        static const bool disabled = std::getenv("HC09_NO_REPLACEMENTS") != nullptr;
        if (disabled)
            return false;
        const auto image = FindSwappableImage(fetch.baseAddress() & 0x1FFFFFFF);
        if (!image)
            return false;
        const std::wstring name = L"textures\\swappables\\" + std::wstring(image->library.begin(), image->library.end()) +
            L"\\" + std::to_wstring(image->id) + L".png";
        auto [it, inserted] = replacements_.try_emplace(name);
        if (inserted)
        {
            // The edition's own images (hc27\textures\...) come before the shared ones.
            std::wstring path = name;
            if (edition::kModFolder)
            {
                const std::wstring modded = std::wstring(edition::kModFolder) + L"\\" + name;
                if (GetFileAttributesW(modded.c_str()) != INVALID_FILE_ATTRIBUTES)
                    path = modded;
            }
            if (LoadReplacement(path, fetch.width(), fetch.height(), it->second))
                std::fprintf(stderr, "[texture] %ls replaces %s/%u (%ux%u, the game's is %ux%u)\n", path.c_str(), image->library.c_str(),
                    image->id, it->second.width, it->second.height, fetch.width(), fetch.height());
        }
        if (!it->second.resource)
            return false;
        // Sizes stay the game's: shaders scale unnormalized coordinates by them.
        out.resource = it->second.resource.Get();
        out.view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        // The image holds the colours as drawn: the fetch swizzle (which puts a texture's stored
        // channels in order) does not apply to it.
        out.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        return true;
    }

    bool TextureCache::LoadReplacement(const std::wstring& path, uint32_t originalWidth, uint32_t originalHeight, Replacement& out,
        bool mips)
    {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return false;
        // Decode with WIC (any PNG, converted to straight RGBA8).
        thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        (void)com;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT width = 0, height = 0;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
        {
            std::fprintf(stderr, "[texture] cannot read %ls\n", path.c_str());
            return false;
        }
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, UINT(pixels.size()), pixels.data())))
            return false;

        // These front-end images are sampled without mips, so a much larger image would alias:
        // halve it (2x2 box filter) until it is at most twice the game's size.
        while (width > 2 * originalWidth && height > 2 * originalHeight && width > 1 && height > 1)
        {
            const UINT halfWidth = width / 2, halfHeight = height / 2;
            std::vector<uint8_t> half(size_t(halfWidth) * halfHeight * 4);
            for (UINT y = 0; y < halfHeight; y++)
                for (UINT x = 0; x < halfWidth; x++)
                    for (int c = 0; c < 4; c++)
                    {
                        const auto at = [&](UINT px, UINT py) { return uint32_t(pixels[(size_t(py) * width + px) * 4 + c]); };
                        half[(size_t(y) * halfWidth + x) * 4 + c] =
                            uint8_t((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                    }
            pixels.swap(half);
            width = halfWidth;
            height = halfHeight;
        }

        // With mips, the full chain down to 1x1 (2x2 box filter, edges clamped on odd sizes).
        uint32_t levels = 1;
        if (mips)
            while ((std::max(width, height) >> levels) != 0)
                levels++;

        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = UINT16(levels);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        if (FAILED(context_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out.resource))))
            return false;
        auto* list = context_.list();
        UINT levelWidth = width, levelHeight = height;
        for (uint32_t level = 0; level < levels; level++)
        {
            if (level > 0)
            {
                const UINT nextWidth = std::max(levelWidth / 2, 1u), nextHeight = std::max(levelHeight / 2, 1u);
                std::vector<uint8_t> next(size_t(nextWidth) * nextHeight * 4);
                for (UINT y = 0; y < nextHeight; y++)
                    for (UINT x = 0; x < nextWidth; x++)
                        for (int c = 0; c < 4; c++)
                        {
                            const auto at = [&](UINT px, UINT py) {
                                return uint32_t(pixels[(size_t(std::min(py, levelHeight - 1)) * levelWidth + std::min(px, levelWidth - 1)) * 4 + c]);
                            };
                            next[(size_t(y) * nextWidth + x) * 4 + c] =
                                uint8_t((at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                        }
                pixels.swap(next);
                levelWidth = nextWidth;
                levelHeight = nextHeight;
            }
            const uint32_t rowBytes = levelWidth * 4;
            const uint32_t rowPitch = (rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            auto upload = context_.Upload(uint64_t(rowPitch) * levelHeight, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            for (UINT y = 0; y < levelHeight; y++)
                std::memcpy(static_cast<uint8_t*>(upload.cpu) + uint64_t(y) * rowPitch, pixels.data() + size_t(y) * rowBytes, rowBytes);
            D3D12_TEXTURE_COPY_LOCATION dst{ out.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            dst.SubresourceIndex = level;
            D3D12_TEXTURE_COPY_LOCATION src{ upload.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = upload.offset;
            src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, levelWidth, levelHeight, 1, rowPitch };
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Transition(list, out.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderReadable);
        out.width = width;
        out.height = height;
        out.levels = levels;
        return true;
    }

    bool TextureCache::GetHashedReplacement(uint64_t contentHash, const TextureFetch& fetch, uint32_t originalLevels, BoundTexture& out)
    {
        static const bool disabled = std::getenv("HC09_NO_REPLACEMENTS") != nullptr;
        if (disabled)
            return false;
        // The folders are listed once: textures\hashed and the edition's own (which wins).
        if (!hashedListed_)
        {
            hashedListed_ = true;
            auto list = [this](const std::wstring& folder) {
                WIN32_FIND_DATAW found;
                HANDLE search = FindFirstFileW((folder + L"\\*.png").c_str(), &found);
                if (search == INVALID_HANDLE_VALUE)
                    return;
                do
                {
                    wchar_t* end = nullptr;
                    const uint64_t hash = std::wcstoull(found.cFileName, &end, 16);
                    if (end && _wcsicmp(end, L".png") == 0 && end - found.cFileName == 16)
                        hashedFiles_[hash] = folder + L"\\" + found.cFileName;
                } while (FindNextFileW(search, &found));
                FindClose(search);
            };
            list(L"textures\\hashed");
            if (edition::kModFolder)
                list(std::wstring(edition::kModFolder) + L"\\textures\\hashed");
            if (!hashedFiles_.empty())
                std::fprintf(stderr, "[texture] %zu replacements by content hash\n", hashedFiles_.size());
        }
        auto file = hashedFiles_.find(contentHash);
        if (file == hashedFiles_.end())
            return false;
        auto [it, inserted] = replacements_.try_emplace(file->second);
        if (inserted && LoadReplacement(file->second, fetch.width(), fetch.height(), it->second, true))
            std::fprintf(stderr, "[texture] %ls replaces texture %016llX (%ux%u, the game's is %ux%u)\n", file->second.c_str(),
                (unsigned long long)contentHash, it->second.width, it->second.height, fetch.width(), fetch.height());
        if (!it->second.resource)
            return false;
        out.resource = it->second.resource.Get();
        out.view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        out.view.Texture2D.MipLevels = originalLevels > 1 ? it->second.levels : 1;
        // The image holds the colours as drawn (the swizzle already applied, as dumps are saved).
        out.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        return true;
    }
}
