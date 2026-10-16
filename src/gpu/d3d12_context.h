#pragma once

// D3D12 device, window and per-frame resources for the renderer.
//
// All recording happens on the command-processor thread. Each frame owns an upload buffer
// and slices of the shader-visible descriptor heaps; they are recycled once the GPU fence
// for that frame has passed.

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>

namespace hc09::gpu
{
    using Microsoft::WRL::ComPtr;

    constexpr uint32_t kFramesInFlight = 3;

    struct UploadAllocation
    {
        void* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
        ID3D12Resource* resource;
        uint64_t offset;
    };

    struct DescriptorRange
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    };

    class D3D12Context
    {
    public:
        bool Initialize(uint32_t width, uint32_t height);

        ID3D12Device* device() const { return device_.Get(); }
        ID3D12GraphicsCommandList* list() const { return list_.Get(); }
        ID3D12CommandQueue* queue() const { return queue_.Get(); }
        ID3D12DescriptorHeap* viewHeap() const { return viewHeap_.Get(); }
        ID3D12DescriptorHeap* samplerHeap() const { return samplerHeap_.Get(); }
        uint32_t viewDescriptorSize() const { return viewIncrement_; }
        uint32_t samplerDescriptorSize() const { return samplerIncrement_; }

        // Per-frame transient allocations (valid until the frame's fence passes).
        UploadAllocation Upload(uint64_t size, uint64_t alignment = 256);
        // Changes whenever the upload ring is recycled (earlier allocations become invalid).
        uint64_t uploadGeneration() const { return uploadGeneration_; }
        DescriptorRange AllocateViews(uint32_t count);
        DescriptorRange AllocateSamplers(uint32_t count);

        // CPU-only descriptor heap slots for render targets / depth stencils.
        D3D12_CPU_DESCRIPTOR_HANDLE AllocateRtv();
        D3D12_CPU_DESCRIPTOR_HANDLE AllocateDsv();

        // Current swap-chain back buffer, transitioned to RENDER_TARGET for the frame.
        ID3D12Resource* BackBuffer() const { return backBuffers_[backBufferIndex_].Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv() const { return backBufferRtvs_[backBufferIndex_]; }
        uint32_t width() const { return width_; }
        uint32_t height() const { return height_; }

        // Ends the frame: transitions the back buffer to PRESENT, submits, presents, and
        // opens the next frame's command list.
        void Present();

        // Submits recorded work and waits for the GPU to go idle.
        void Flush();

        // Bytes of local video memory this process uses (DXGI budget query), or 0 if unknown.
        uint64_t VideoMemoryUsage() const;

    private:
        void BeginFrame();
        void WaitForFrame(uint32_t frame);

        uint32_t width_ = 0;
        uint32_t height_ = 0;
        HWND window_ = nullptr;

        ComPtr<IDXGIFactory4> factory_;
        ComPtr<ID3D12Device> device_;
        ComPtr<ID3D12CommandQueue> queue_;
        ComPtr<IDXGISwapChain3> swapChain_;
        ComPtr<ID3D12Resource> backBuffers_[2];
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtvs_[2]{};
        uint32_t backBufferIndex_ = 0;

        ComPtr<ID3D12CommandAllocator> allocators_[kFramesInFlight];
        ComPtr<ID3D12GraphicsCommandList> list_;
        ComPtr<ID3D12Fence> fence_;
        HANDLE fenceEvent_ = nullptr;
        uint64_t fenceValues_[kFramesInFlight]{};
        uint64_t nextFenceValue_ = 1;
        uint32_t frame_ = 0;

        static constexpr uint64_t kUploadBytesPerFrame = 192ull << 20;
        ComPtr<ID3D12Resource> uploadBuffers_[kFramesInFlight];
        uint8_t* uploadCpu_[kFramesInFlight]{};
        uint64_t uploadOffset_ = 0;
        uint64_t uploadGeneration_ = 0;

        static constexpr uint32_t kViewsPerFrame = 64 * 1024;
        static constexpr uint32_t kSamplersPerFrame = 640;
        ComPtr<ID3D12DescriptorHeap> viewHeap_;
        ComPtr<ID3D12DescriptorHeap> samplerHeap_;
        uint32_t viewIncrement_ = 0;
        uint32_t samplerIncrement_ = 0;
        uint32_t viewOffset_ = 0;
        uint32_t samplerOffset_ = 0;

        static constexpr uint32_t kRtvCapacity = 256;
        ComPtr<ID3D12DescriptorHeap> rtvHeap_;
        ComPtr<ID3D12DescriptorHeap> dsvHeap_;
        uint32_t rtvCount_ = 0;
        uint32_t dsvCount_ = 0;
        uint32_t rtvIncrement_ = 0;
        uint32_t dsvIncrement_ = 0;
    };
}
