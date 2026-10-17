#pragma once

// Guest textures (from texture fetch constants) as D3D12 textures, plus textures produced
// by EDRAM resolves.

#include <cstdint>
#include <map>
#include <unordered_map>

#include "gpu/d3d12_context.h"

namespace hc09::gpu
{
    // Decoded texture fetch constant (6 dwords at 0x4800 + 6n).
    struct TextureFetch
    {
        uint32_t words[6];

        uint32_t type() const { return words[0] & 3; }
        uint32_t clampX() const { return (words[0] >> 10) & 7; }
        uint32_t clampY() const { return (words[0] >> 13) & 7; }
        uint32_t clampZ() const { return (words[0] >> 16) & 7; }
        uint32_t pitchTexels() const { return ((words[0] >> 22) & 0x1FF) * 32; }
        bool tiled() const { return (words[0] >> 31) != 0; }
        uint32_t format() const { return words[1] & 0x3F; }
        uint32_t endian() const { return (words[1] >> 6) & 3; }
        uint32_t baseAddress() const { return words[1] & 0xFFFFF000; }  // physical
        bool stacked() const { return (words[1] >> 10) & 1; }                // 2D texture array
        uint32_t stackDepth() const { return (words[2] >> 26) + 1; }        // layers of a stacked texture
        uint32_t width() const { return (words[2] & 0x1FFF) + 1; }      // 2D
        uint32_t height() const { return ((words[2] >> 13) & 0x1FFF) + 1; }
        uint32_t width3D() const { return (words[2] & 0x7FF) + 1; }          // 3D: 11/11/10 bits
        uint32_t height3D() const { return ((words[2] >> 11) & 0x7FF) + 1; }
        uint32_t depth3D() const { return ((words[2] >> 22) & 0x3FF) + 1; }
        uint32_t swizzle() const { return (words[3] >> 1) & 0xFFF; }    // 3 bits per channel
        uint32_t magFilter() const { return (words[3] >> 19) & 3; }
        uint32_t minFilter() const { return (words[3] >> 21) & 3; }
        uint32_t mipFilter() const { return (words[3] >> 23) & 3; }
        uint32_t anisoFilter() const { return (words[3] >> 25) & 7; }  // 0 off, n: up to 2^(n-1):1
        uint32_t mipMinLevel() const { return (words[4] >> 2) & 0xF; }
        uint32_t mipMaxLevel() const { return (words[4] >> 6) & 0xF; }
        // Bits 10-11 are the mag/min anisotropic walk flags; the signed bias (1/32 steps) is bits 12-21.
        int32_t lodBias() const { return int32_t(words[4] << 10) >> 22; }
        bool packedMips() const { return (words[5] >> 11) & 1; }        // small levels share one tile
        uint32_t borderColor() const { return words[5] & 3; }           // 0 transparent black, 1 white, 2/3 opaque black
        uint32_t mipAddress() const { return words[5] & 0xFFFFF000; }   // physical; levels 1..n
        uint32_t dimension() const { return (words[5] >> 9) & 3; }      // 0 1D, 1 2D, 2 3D, 3 cube
    };

    TextureFetch ReadTextureFetch(uint32_t index);

    struct BoundTexture
    {
        ID3D12Resource* resource = nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;    // slices of a 3D or stacked texture
        bool stacked = false;  // sampled with a layer index rather than a normalized depth
    };

    class TextureCache
    {
    public:
        explicit TextureCache(D3D12Context& context) : context_(context) {}

        // Texture for a fetch constant, uploaded/refreshed as needed. Returns false when the
        // format or dimension is not supported yet.
        bool Get(const TextureFetch& fetch, BoundTexture& out);

        // Registers a resolve destination so later fetches from `address` sample it.
        // swapRedBlue: the resolve swapped red and blue on the way to memory (RB_COPY_DEST_INFO copy_dest_swap).
        void SetResolved(uint32_t address, ID3D12Resource* resource, DXGI_FORMAT format, uint32_t width, uint32_t height,
            bool swapRedBlue = false);
        ID3D12Resource* FindResolved(uint32_t address, uint32_t* width = nullptr, uint32_t* height = nullptr) const;

        // Last frame a fetch sampled the resolve at `address` (-1 if none or unknown).
        int ResolvedLastUse(uint32_t address) const;
        // Drops the record of a resolve (its target is being released).
        void ForgetResolved(uint32_t address);
        // Releases textures no draw has sampled for a while (called once per frame).
        void Evict(int frame);

        // HC09_GPU_STATS: cache sizes, and fetches that found a resolve whose size or format differ
        // (the address was resolved to earlier, and the title now keeps a different texture there).
        struct Stats
        {
            size_t textures = 0;
            size_t resolved = 0;
            uint64_t mismatchedResolveHits = 0;  // fetches with a texel size the resolve cannot provide
            uint64_t staleResolves = 0;          // resolves dropped because the title reused their memory
            uint64_t evictedTextures = 0;
        };
        Stats GetStats() const { return { entries_.size(), resolved_.size(), mismatchedResolveHits_, staleResolves_, evictedTextures_ }; }

    private:
        struct Entry
        {
            ComPtr<ID3D12Resource> resource;
            uint64_t contentHash = 0;
            int hashedFrame = -1;  // contents are re-checked at most once per frame
            uint64_t sampledHash = 0;  // large textures: hash of sampled chunks
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
        };

        struct Resolved
        {
            ID3D12Resource* resource;
            DXGI_FORMAT format;
            uint32_t width;
            uint32_t height;
            bool swapRedBlue;
            // Resolves are not written back to guest memory, so the memory under a resolve keeps
            // whatever it held. A fingerprint of it, taken at each resolve, shows when the title
            // has since loaded something else there (the resolve is then stale).
            uint64_t memoryFingerprint = 0;
            int checkedFrame = -1;
            int lastUsedFrame = -1;
        };
        // True when the resolve still describes the memory at `address` (checked once per frame).
        bool ResolveStillValid(uint32_t address, Resolved& resolved, int frame);

        // A render-to-texture whose mip levels the title also renders (each resolved to its place
        // in the mip area): the base and those levels copied into one texture with a mip chain.
        struct Composed
        {
            ComPtr<ID3D12Resource> resource;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            uint32_t width = 0, height = 0, levels = 0;
            int copiedFrame = -1;
        };
        // Number of levels composed for a resolved base (1 when the title rendered no mips).
        uint32_t ComposeResolvedMips(const TextureFetch& fetch, const Resolved& base, BoundTexture& out);

        D3D12Context& context_;
        std::unordered_map<uint64_t, Entry> entries_;
        std::map<uint32_t, Resolved> resolved_;
        std::unordered_map<uint32_t, Composed> composed_;
        std::vector<std::pair<int, ComPtr<ID3D12Resource>>> retired_;  // (frame replaced, resource)
        bool loggedUnsupported_[64]{};
        uint64_t mismatchedResolveHits_ = 0;
        uint64_t staleResolves_ = 0;
        uint64_t evictedTextures_ = 0;
    };
}
