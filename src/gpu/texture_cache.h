#pragma once

// Guest textures (from texture fetch constants) as D3D12 textures, plus textures produced
// by EDRAM resolves.

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

#include "gpu/d3d12_context.h"
#include "gpu/shared_memory.h"

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
        TextureCache(D3D12Context& context, SharedMemory& sharedMemory) : context_(context), sharedMemory_(sharedMemory) {}

        // Texture for a fetch constant, uploaded/refreshed as needed. Returns false when the
        // format or dimension is not supported yet.
        bool Get(const TextureFetch& fetch, BoundTexture& out);

        // Registers a resolve destination so later fetches from `address` sample it.
        // swapRedBlue: the resolve swapped red and blue on the way to memory (RB_COPY_DEST_INFO copy_dest_swap).
        // memoryTexelBytes: the size of a texel in the format the resolve writes to memory
        // (copy_dest_format), which fetches of the result use; 0 for the target's own.
        void SetResolved(uint32_t address, ID3D12Resource* resource, DXGI_FORMAT format, uint32_t width, uint32_t height,
            bool swapRedBlue = false, uint32_t memoryTexelBytes = 0);
        ID3D12Resource* FindResolved(uint32_t address, uint32_t* width = nullptr, uint32_t* height = nullptr) const;

        // Last frame a fetch sampled the resolve at `address` (-1 if none or unknown).
        int ResolvedLastUse(uint32_t address) const;
        // True when the title has written other data over the resolve at `address` (or there is none):
        // only then can its result be released, since resolves are not written to memory.
        bool ResolvedMemoryChanged(uint32_t address) const;
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
            bool rewritten = false;    // changed since its first upload: hashed in full every frame
            uint64_t memoryVersion = ~0ull;  // guest memory on the GPU: its pages' version when last checked
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
        };

        struct Resolved
        {
            ID3D12Resource* resource;
            DXGI_FORMAT format;
            uint32_t width;
            uint32_t height;
            bool swapRedBlue;
            uint32_t texelBytes = 4;  // in memory
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

        // Replacement images (textures/...png) for swappable front-end images, loaded once.
        struct Replacement
        {
            ComPtr<ID3D12Resource> resource;  // null when the file is missing or unreadable
            uint32_t width = 0, height = 0, levels = 1;
        };
        // The replacement for the texture at `fetch`'s address, if it holds a swappable image
        // and a file for it exists.
        bool GetReplacement(const TextureFetch& fetch, BoundTexture& out);
        // The replacement for any 2D texture by its contents: textures\hashed\<content hash>.png
        // (16 hex digits, as HC09_DUMP_HASHED names the game's images).
        bool GetHashedReplacement(uint64_t contentHash, const TextureFetch& fetch, uint32_t originalLevels, BoundTexture& out);
        bool LoadReplacement(const std::wstring& path, uint32_t originalWidth, uint32_t originalHeight, Replacement& out,
            bool mips = false);

        // Guest memory on the GPU: a compute shader untiles and byte-swaps each level into a
        // staging buffer laid out for CopyTextureRegion.
        struct UntileLevel
        {
            uint32_t source;               // physical address of the stored level (this face's)
            uint32_t pitch, rows;          // stored level, in blocks (rows: a volume's slice height)
            uint32_t offsetX, offsetY;     // packed mip offset, in blocks
            uint32_t slice;                // volumes: the depth slice
            uint32_t blocksWide, blocksHigh;
            uint32_t rowPitch;             // in the staging buffer
            uint64_t staging;              // byte offset in the staging buffer
        };
        bool PrepareUntile();
        // Records the dispatches for `levels` into the staging buffer and leaves it ready to copy from.
        bool Untile(const std::vector<UntileLevel>& levels, uint32_t unitBytes, bool tiled, bool volume, uint32_t endian, uint64_t bytes);

        // Textures from memory already looked up this frame, by fetch constant: their contents are
        // checked once per frame, so a repeat lookup returns the same. Cleared by any resolve.
        struct Recent
        {
            TextureFetch fetch;
            BoundTexture texture;
        };
        void Remember(uint64_t key, const TextureFetch& fetch, const BoundTexture& texture);
        std::unordered_map<uint64_t, Recent> recent_;
        int recentFrame_ = -1;

        D3D12Context& context_;
        SharedMemory& sharedMemory_;
        ComPtr<ID3D12RootSignature> untileRootSignature_;
        ComPtr<ID3D12PipelineState> untilePipeline_;
        int untileState_ = 0;  // 0 not tried, 1 ready, -1 failed (the CPU untiles)
        ComPtr<ID3D12Resource> staging_;
        uint64_t stagingSize_ = 0;
        D3D12_RESOURCE_STATES stagingState_ = D3D12_RESOURCE_STATE_COMMON;
        std::unordered_map<std::wstring, Replacement> replacements_;
        std::unordered_map<uint64_t, std::wstring> hashedFiles_;  // content hash -> replacement file
        bool hashedListed_ = false;
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
