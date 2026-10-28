#include "gpu/shared_memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "memory.h"

namespace hc09::gpu
{
    namespace
    {
        constexpr uint32_t kPages = SharedMemory::kSize >> SharedMemory::kPageShift;
        constexpr uint32_t kPageBytes = 1u << SharedMemory::kPageShift;
        constexpr uint32_t kFirstWindow = 0xA0000000;
        constexpr D3D12_RESOURCE_STATES kReadable =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        SharedMemory* g_sharedMemory = nullptr;

        void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES after)
        {
            if (state == after)
                return;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = state;
            barrier.Transition.StateAfter = after;
            list->ResourceBarrier(1, &barrier);
            state = after;
        }
    }

    SharedMemory* GetSharedMemory()
    {
        return g_sharedMemory;
    }

    bool SharedMemory::Initialize(D3D12Context& context)
    {
        context_ = &context;
        if (const char* setting = std::getenv("HC09_GPU_MEMORY"); setting && std::strcmp(setting, "0") == 0)
        {
            std::fprintf(stderr, "[gpu] guest memory on the GPU: off (HC09_GPU_MEMORY=0)\n");
            return false;
        }
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(context.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&buffer_))))
        {
            std::fprintf(stderr, "[gpu] guest memory on the GPU: no room for the 512 MiB buffer; using the CPU\n");
            return false;
        }
        valid_ = std::make_unique<std::atomic<uint64_t>[]>(kPages / 64);
        versions_.assign(kPages, 0);
        // The windows onto physical memory (0xE0000000 is left unmapped by HC09_NO_E_WINDOW).
        for (uint32_t window : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
            if (window != 0xE0000000u || !std::getenv("HC09_NO_E_WINDOW"))
                windows_.push_back(GuestToHost(window));
        g_sharedMemory = this;
        g_beforeExternalWrite = [](uint32_t address, uint32_t length) {
            if (address >= kFirstWindow && length != 0)
                for (uint64_t page = (address & (kSize - 1)) >> kPageShift,
                              last = ((uint64_t(address & (kSize - 1)) + length - 1) >> kPageShift);
                     page <= last && page < kPages; page++)
                    g_sharedMemory->OnWrite(uint32_t(page));
        };
        // Called before the handlers already installed (guest fault reports).
        AddVectoredExceptionHandler(1, OnException);
        std::fprintf(stderr, "[gpu] guest memory on the GPU: on\n");
        return true;
    }

    LONG CALLBACK SharedMemory::OnException(EXCEPTION_POINTERS* info)
    {
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        // ExceptionInformation: [0] 1 for a write, [1] the address.
        if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 || record->ExceptionInformation[0] != 1 ||
            !g_sharedMemory)
            return EXCEPTION_CONTINUE_SEARCH;
        const uintptr_t address = record->ExceptionInformation[1];
        const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
        if (address < base + kFirstWindow || address >= base + 0x100000000ull)
            return EXCEPTION_CONTINUE_SEARCH;
        const uint32_t page = uint32_t((address - base) & (kSize - 1)) >> kPageShift;
        if (g_sharedMemory->OnWrite(page))
            return EXCEPTION_CONTINUE_EXECUTION;
        // Not a watched page: another thread may have lifted the protection while this one
        // waited for the lock (then the write succeeds when retried), or it is a real fault.
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) && memory.State == MEM_COMMIT &&
            memory.Protect == PAGE_READWRITE)
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool SharedMemory::OnWrite(uint32_t page)
    {
        AcquireSRWLockExclusive(&lock_);
        const bool watched = Valid(page);
        if (watched)
        {
            valid_[page >> 6].fetch_and(~(1ull << (page & 63)), std::memory_order_relaxed);
            Protect(page, 1, PAGE_READWRITE);
            faults_++;
        }
        ReleaseSRWLockExclusive(&lock_);
        return watched;
    }

    void SharedMemory::Protect(uint32_t firstPage, uint32_t count, DWORD protection)
    {
        for (uint8_t* window : windows_)
        {
            DWORD previous;
            VirtualProtect(window + (uint64_t(firstPage) << kPageShift), uint64_t(count) << kPageShift, protection, &previous);
        }
    }

    uint64_t SharedMemory::Request(uint32_t address, uint32_t length, bool version)
    {
        if (!buffer_ || length == 0)
            return 0;
        address &= kSize - 1;
        const uint32_t first = address >> kPageShift;
        const uint32_t last = uint32_t(std::min<uint64_t>(uint64_t(address) + length - 1, kSize - 1) >> kPageShift);

        // Usually every page is current already: check 64 at a time without the lock.
        bool stale = false;
        for (uint32_t word = first >> 6; word <= (last >> 6) && !stale; word++)
        {
            uint64_t mask = ~0ull;
            if (word == (first >> 6))
                mask &= ~0ull << (first & 63);
            if (word == (last >> 6))
                mask &= ~0ull >> (63 - (last & 63));
            stale = (valid_[word].load(std::memory_order_relaxed) & mask) != mask;
        }
        if (stale)
        {
            AcquireSRWLockExclusive(&lock_);
            generation_++;
            for (uint32_t page = first; page <= last;)
            {
                if (Valid(page))
                {
                    page++;
                    continue;
                }
                uint32_t end = page + 1;
                while (end <= last && !Valid(end))
                    end++;
                const uint32_t count = end - page;
                const uint64_t bytes = uint64_t(count) << kPageShift;
                // Protected before the copy: a write from here on faults (and waits for the lock),
                // so the copy holds everything written before it and a later write marks it stale.
                Protect(page, count, PAGE_READONLY);
                const UploadAllocation upload = context_->Upload(bytes, 256);
                std::memcpy(upload.cpu, windows_[0] + (uint64_t(page) << kPageShift), size_t(bytes));
                auto* list = context_->list();
                Transition(list, buffer_.Get(), state_, D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyBufferRegion(buffer_.Get(), uint64_t(page) << kPageShift, upload.resource, upload.offset, bytes);
                for (uint32_t p = page; p < end; p++)
                {
                    valid_[p >> 6].fetch_or(1ull << (p & 63), std::memory_order_relaxed);
                    versions_[p] = generation_;
                }
                pagesCopied_ += count;
                page = end;
            }
            Transition(context_->list(), buffer_.Get(), state_, kReadable);
            ReleaseSRWLockExclusive(&lock_);
        }

        if (!version)
            return 0;
        uint32_t newest = 0;
        for (uint32_t page = first; page <= last; page++)
            newest = std::max(newest, versions_[page]);
        return newest;
    }

    SharedMemory::Stats SharedMemory::TakeStats()
    {
        return { pagesCopied_.exchange(0), faults_.exchange(0) };
    }
}
