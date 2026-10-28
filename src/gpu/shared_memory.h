#pragma once

// Guest physical memory (512 MiB) mirrored in one GPU buffer, so shaders read vertex data and
// texture data from it directly (byte swapping and untiling happen on the GPU).
//
// A page (4 KiB) is copied into the buffer the first time a draw needs it, and again only after
// the title writes to it. Copied pages are write-protected in each of the three windows that
// alias physical memory (see memory.cpp); the title's first write to one raises an access
// violation, which a vectored exception handler answers by lifting the protection and marking
// the page stale. Copies go through the per-frame upload ring, so a draw sees the memory as it
// was when the command processor reached it, as before.
//
// HC09_GPU_MEMORY=0 turns it off: vertices are swapped and textures untiled on the CPU, and
// textures are hashed to notice changes.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <windows.h>

#include "gpu/d3d12_context.h"

namespace hc09::gpu
{
    class SharedMemory
    {
    public:
        static constexpr uint32_t kSize = 0x20000000;
        static constexpr uint32_t kPageShift = 12;

        bool Initialize(D3D12Context& context);
        bool enabled() const { return buffer_ != nullptr; }
        ID3D12Resource* buffer() const { return buffer_.Get(); }

        // Brings physical bytes [address, address + length) up to date in the buffer (recording
        // copies of stale pages) and leaves the buffer shader-readable. With `version`, returns the
        // range's version, which changes whenever a page in it was copied again.
        uint64_t Request(uint32_t address, uint32_t length, bool version = false);

        // HC09_GPU_STATS: pages copied and write faults since the last call.
        struct Stats
        {
            uint64_t pagesCopied = 0;
            uint64_t faults = 0;
        };
        Stats TakeStats();

    private:
        static LONG CALLBACK OnException(EXCEPTION_POINTERS* info);
        // The title wrote to a protected page: lift the protection, mark it stale.
        bool OnWrite(uint32_t page);
        void Protect(uint32_t firstPage, uint32_t count, DWORD protection);
        bool Valid(uint32_t page) const { return (valid_[page >> 6].load(std::memory_order_relaxed) >> (page & 63)) & 1; }

        D3D12Context* context_ = nullptr;
        ComPtr<ID3D12Resource> buffer_;
        D3D12_RESOURCE_STATES state_ = D3D12_RESOURCE_STATE_COMMON;
        SRWLOCK lock_ = SRWLOCK_INIT;
        // Pages whose copy in the buffer is current (and which are therefore write-protected).
        std::unique_ptr<std::atomic<uint64_t>[]> valid_;
        std::vector<uint32_t> versions_;  // per page: the copy generation it was last copied in
        uint32_t generation_ = 0;
        std::vector<uint8_t*> windows_;   // host addresses of the windows onto physical memory
        std::atomic<uint64_t> pagesCopied_{ 0 }, faults_{ 0 };
    };

    // The memory's owner, for the exception handler and for writes the title asks the host OS to
    // make (file reads into guest buffers), which fault in the kernel rather than raising an
    // exception: those lift the protection first.
    SharedMemory* GetSharedMemory();
}
