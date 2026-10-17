#include "gpu/texture_cache.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gpu/command_processor.h"
#include "gpu/draw.h"
#include "kernel/guest.h"

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
        uint64_t MemoryFingerprint(uint32_t address, uint32_t width, uint32_t height, DXGI_FORMAT format)
        {
            const uint32_t physical = address & 0x1FFFFFFF;
            uint64_t bytes = uint64_t((width + 31) & ~31u) * ((height + 31) & ~31u) * ResolveTexelBytes(format);
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
        bool swapRedBlue)
    {
        Resolved resolved{ resource, format, width, height, swapRedBlue };
        resolved.memoryFingerprint = MemoryFingerprint(address, width, height, format);
        resolved.checkedFrame = FrameNumber();
        resolved.lastUsedFrame = FrameNumber();
        resolved_[address & 0x1FFFFFFF] = resolved;
    }

    bool TextureCache::ResolveStillValid(uint32_t address, Resolved& resolved, int frame)
    {
        if (resolved.checkedFrame == frame)
            return true;
        resolved.checkedFrame = frame;
        if (MemoryFingerprint(address, resolved.width, resolved.height, resolved.format) == resolved.memoryFingerprint)
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

    void TextureCache::ForgetResolved(uint32_t address)
    {
        address &= 0x1FFFFFFF;
        resolved_.erase(address);
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

    bool TextureCache::Get(const TextureFetch& fetch, BoundTexture& out)
    {
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

        // Render-to-texture: a resolve wrote this address. A fetch with another texel size (or a
        // compressed format) cannot be reading that resolve; neither can one after the title has
        // loaded other data over it.
        auto it = resolved_.find(fetch.baseAddress() & 0x1FFFFFFF);
        if (it != resolved_.end() && (cube || volume || stacked || info.compressed || info.blockBytes != ResolveTexelBytes(it->second.format)))
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
        if (check)
        {
            // Large textures: a sampled hash (64 chunks spread over the base level) every frame,
            // which catches whole-image updates such as video, and a full hash every 60 frames.
            constexpr uint64_t kFullHashLimit = 256 * 1024, kChunk = 256, kChunks = 64;
            if (sourceBytes + mipBytes <= kFullHashLimit || entry.resource == nullptr || frame % 60 == int(entry.contentHash % 60) ||
                entry.contentHash == ~0ull)
            {
                hash = fullHash();
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
            entry.contentHash = hash;
            // HC09_DUMP_MIPS=<width>x<height>f<format>[,...]: save every level of matching textures as
            // uploaded (untiled, byte-swapped, rows packed) to build/mip_<base>_<level>_<w>x<h>.bin.
            std::FILE* mipDump[16]{};
            static const char* mipDumpSpec = std::getenv("HC09_DUMP_MIPS");
            if (MatchesTextureSpec(mipDumpSpec, width, height, fetch.format()))
            {
                for (uint32_t level = 0; level < levels && level < 16; level++)
                {
                    char path[96];
                    std::snprintf(path, sizeof(path), "build/mip_%08X_%u_%ux%u.bin", fetch.baseAddress(), level,
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
            Transition(list, entry.resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
            entry.state = D3D12_RESOURCE_STATE_COPY_DEST;
            for (uint32_t faceIndex = 0; faceIndex < faces; faceIndex++)
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
        }
        Transition(list, entry.resource.Get(), entry.state, kShaderReadable);
        entry.state = kShaderReadable;
        out.resource = entry.resource.Get();
        return true;
    }
}
