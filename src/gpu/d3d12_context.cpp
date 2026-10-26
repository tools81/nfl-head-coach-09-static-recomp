#include "gpu/d3d12_context.h"

#include "common/edition.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <set>
#include <string>
#include <vector>
#include <thread>

namespace hc09::gpu
{
    namespace
    {
        // Client area size as last reported by WM_SIZE; the render thread resizes the swap chain
        // to match at the start of a frame.
        std::atomic<uint32_t> g_clientWidth{ 0 }, g_clientHeight{ 0 };

        // Borderless fullscreen on the window's monitor, toggled with F11 or Alt+Enter.
        void ToggleFullscreen(HWND window)
        {
            static bool fullscreen = false;
            static RECT windowedRect{};
            static LONG_PTR windowedStyle = 0;
            if (!fullscreen)
            {
                windowedStyle = GetWindowLongPtrW(window, GWL_STYLE);
                GetWindowRect(window, &windowedRect);
                MONITORINFO monitor{ sizeof(monitor) };
                GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
                SetWindowLongPtrW(window, GWL_STYLE, (windowedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
                const RECT& r = monitor.rcMonitor;
                SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                    SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            }
            else
            {
                SetWindowLongPtrW(window, GWL_STYLE, windowedStyle);
                SetWindowPos(window, nullptr, windowedRect.left, windowedRect.top, windowedRect.right - windowedRect.left,
                    windowedRect.bottom - windowedRect.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOZORDER);
            }
            fullscreen = !fullscreen;
        }

        LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_CLOSE:
                std::fprintf(stderr, "[gpu] window closed\n");
                std::fflush(stderr);
                ExitProcess(0);
            case WM_SIZE:
                if (wParam != SIZE_MINIMIZED && LOWORD(lParam) > 0 && HIWORD(lParam) > 0)
                {
                    g_clientWidth = LOWORD(lParam);
                    g_clientHeight = HIWORD(lParam);
                }
                break;
            case WM_KEYDOWN:
                if (wParam == VK_F11 && !(lParam & (1 << 30)))
                {
                    ToggleFullscreen(window);
                    return 0;
                }
                break;
            case WM_SYSKEYDOWN:
                if (wParam == VK_RETURN && (lParam & (1 << 29)) && !(lParam & (1 << 30)))
                {
                    ToggleFullscreen(window);
                    return 0;
                }
                break;
            case WM_SYSCHAR:
                if (wParam == VK_RETURN)
                    return 0;  // no beep for Alt+Enter
                break;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }

        // Creates the window on its own thread, which then pumps its messages forever.
        HWND CreateGameWindow(uint32_t width, uint32_t height)
        {
            std::promise<HWND> created;
            auto future = created.get_future();
            std::thread([&created, width, height] {
                WNDCLASSW windowClass{};
                windowClass.lpfnWndProc = WindowProc;
                windowClass.hInstance = GetModuleHandleW(nullptr);
                windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))  /* IDC_ARROW */;
                windowClass.lpszClassName = L"HC09Window";
                RegisterClassW(&windowClass);

                RECT rect{ 0, 0, LONG(width), LONG(height) };
                AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
                HWND window = CreateWindowExW(0, windowClass.lpszClassName, edition::kWindowTitle, WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                    windowClass.hInstance, nullptr);
                ShowWindow(window, SW_SHOW);
                created.set_value(window);

                MSG message;
                while (GetMessageW(&message, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }).detach();
            return future.get();
        }

        bool Check(HRESULT hr, const char* what)
        {
            if (FAILED(hr))
                std::fprintf(stderr, "[d3d12] %s failed: 0x%08lX\n", what, hr);
            return SUCCEEDED(hr);
        }
    }

    bool D3D12Context::Initialize(uint32_t width, uint32_t height)
    {
        width_ = width;
        height_ = height;

        if (std::getenv("HC09_D3D12_DEBUG"))
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                debug->EnableDebugLayer();
        }
        if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory_)), "CreateDXGIFactory1") ||
            !Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "D3D12CreateDevice"))
            return false;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!Check(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), "CreateCommandQueue"))
            return false;

        window_ = CreateGameWindow(width, height);
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = width;
        swapDesc.Height = height;
        swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> swapChain;
        if (!Check(factory_->CreateSwapChainForHwnd(queue_.Get(), window_, &swapDesc, nullptr, nullptr, &swapChain), "CreateSwapChainForHwnd"))
            return false;
        swapChain.As(&swapChain_);
        factory_->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);  // Alt+Enter is borderless fullscreen

        auto makeHeap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool shaderVisible, ComPtr<ID3D12DescriptorHeap>& heap) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = type;
            desc.NumDescriptors = count;
            desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            return Check(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
        };
        if (!makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewsPerFrame * kFramesInFlight, true, viewHeap_) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplersPerFrame * kFramesInFlight, true, samplerHeap_) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCapacity, false, rtvHeap_) ||
            !makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kRtvCapacity, false, dsvHeap_))
            return false;
        viewIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        samplerIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        rtvIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        dsvIncrement_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        for (uint32_t i = 0; i < 2; i++)
        {
            swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
            backBufferRtvs_[i] = AllocateRtv();
            device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, backBufferRtvs_[i]);
        }

        D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = kUploadBytesPerFrame;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (uint32_t i = 0; i < kFramesInFlight; i++)
        {
            if (!Check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i])), "CreateCommandAllocator") ||
                !Check(device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffers_[i])), "upload buffer"))
                return false;
            uploadBuffers_[i]->Map(0, nullptr, reinterpret_cast<void**>(&uploadCpu_[i]));
        }
        if (!Check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr, IID_PPV_ARGS(&list_)), "CreateCommandList") ||
            !Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence"))
            return false;
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        list_->Close();
        frame_ = 0;
        BeginFrame();
        std::fprintf(stderr, "[d3d12] initialized %ux%u\n", width, height);
        return true;
    }

    void D3D12Context::WaitForFrame(uint32_t frame)
    {
        if (fence_->GetCompletedValue() < fenceValues_[frame])
        {
            fence_->SetEventOnCompletion(fenceValues_[frame], fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    void D3D12Context::BeginFrame()
    {
        // Follow the window's client size: wait for the GPU, then resize the swap chain buffers.
        const uint32_t clientWidth = g_clientWidth, clientHeight = g_clientHeight;
        if (clientWidth && clientHeight && (clientWidth != width_ || clientHeight != height_))
        {
            for (uint32_t i = 0; i < kFramesInFlight; i++)
                WaitForFrame(i);
            for (auto& buffer : backBuffers_)
                buffer.Reset();
            if (SUCCEEDED(swapChain_->ResizeBuffers(2, clientWidth, clientHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 0)))
            {
                width_ = clientWidth;
                height_ = clientHeight;
            }
            for (uint32_t i = 0; i < 2; i++)
            {
                swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
                device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, backBufferRtvs_[i]);
            }
        }
        WaitForFrame(frame_);
        allocators_[frame_]->Reset();
        list_->Reset(allocators_[frame_].Get(), nullptr);
        uploadOffset_ = 0;
        uploadGeneration_++;
        viewOffset_ = 0;
        samplerOffset_ = 0;
        backBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();

        ID3D12DescriptorHeap* heaps[] = { viewHeap_.Get(), samplerHeap_.Get() };
        list_->SetDescriptorHeaps(2, heaps);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = BackBuffer();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list_->ResourceBarrier(1, &barrier);
    }

    void D3D12Context::Present()
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = BackBuffer();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list_->ResourceBarrier(1, &barrier);
        list_->Close();

        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        swapChain_->Present(0, 0);
        // HC09_D3D12_DEBUG: print debug layer messages (each distinct one once) to stderr.
        static ComPtr<ID3D12InfoQueue> infoQueue;
        static bool infoQueueChecked = false;
        if (!infoQueueChecked)
        {
            infoQueueChecked = true;
            if (std::getenv("HC09_D3D12_DEBUG"))
                device_.As(&infoQueue);
        }
        if (infoQueue)
        {
            static std::set<std::string> seen;
            const UINT64 count = infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < count; i++)
            {
                SIZE_T size = 0;
                infoQueue->GetMessage(i, nullptr, &size);
                std::vector<char> buffer(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (SUCCEEDED(infoQueue->GetMessage(i, message, &size)) && seen.size() < 200 &&
                    seen.insert(std::string(message->pDescription, message->DescriptionByteLength)).second)
                    std::fprintf(stderr, "[d3d12] %.*s\n", int(message->DescriptionByteLength), message->pDescription);
            }
            infoQueue->ClearStoredMessages();
        }
        fenceValues_[frame_] = nextFenceValue_++;
        queue_->Signal(fence_.Get(), fenceValues_[frame_]);

        frame_ = (frame_ + 1) % kFramesInFlight;
        BeginFrame();
    }

    void D3D12Context::Flush()
    {
        list_->Close();
        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        const uint64_t value = nextFenceValue_++;
        queue_->Signal(fence_.Get(), value);
        fence_->SetEventOnCompletion(value, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
        allocators_[frame_]->Reset();
        list_->Reset(allocators_[frame_].Get(), nullptr);
        ID3D12DescriptorHeap* heaps[] = { viewHeap_.Get(), samplerHeap_.Get() };
        list_->SetDescriptorHeaps(2, heaps);
    }

    uint64_t D3D12Context::VideoMemoryUsage() const
    {
        ComPtr<IDXGIAdapter3> adapter;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!factory_ || FAILED(factory_->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
            FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return 0;
        return info.CurrentUsage;
    }

    UploadAllocation D3D12Context::Upload(uint64_t size, uint64_t alignment)
    {
        uploadOffset_ = (uploadOffset_ + alignment - 1) & ~(alignment - 1);
        if (uploadOffset_ + size > kUploadBytesPerFrame)
        {
            // Out of transient space: let the GPU catch up and start over in this frame.
            Flush();
            uploadOffset_ = 0;
        uploadGeneration_++;
            viewOffset_ = 0;
            samplerOffset_ = 0;
        }
        UploadAllocation allocation{ uploadCpu_[frame_] + uploadOffset_,
            uploadBuffers_[frame_]->GetGPUVirtualAddress() + uploadOffset_, uploadBuffers_[frame_].Get(), uploadOffset_ };
        uploadOffset_ += size;
        return allocation;
    }

    DescriptorRange D3D12Context::AllocateViews(uint32_t count)
    {
        if (viewOffset_ + count > kViewsPerFrame)
        {
            Flush();
            uploadOffset_ = 0;
        uploadGeneration_++;
            viewOffset_ = 0;
            samplerOffset_ = 0;
        }
        const uint32_t index = frame_ * kViewsPerFrame + viewOffset_;
        viewOffset_ += count;
        DescriptorRange range;
        range.cpu.ptr = viewHeap_->GetCPUDescriptorHandleForHeapStart().ptr + uint64_t(index) * viewIncrement_;
        range.gpu.ptr = viewHeap_->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(index) * viewIncrement_;
        return range;
    }

    DescriptorRange D3D12Context::AllocateSamplers(uint32_t count)
    {
        if (samplerOffset_ + count > kSamplersPerFrame)
        {
            Flush();
            uploadOffset_ = 0;
        uploadGeneration_++;
            viewOffset_ = 0;
            samplerOffset_ = 0;
        }
        const uint32_t index = frame_ * kSamplersPerFrame + samplerOffset_;
        samplerOffset_ += count;
        DescriptorRange range;
        range.cpu.ptr = samplerHeap_->GetCPUDescriptorHandleForHeapStart().ptr + uint64_t(index) * samplerIncrement_;
        range.gpu.ptr = samplerHeap_->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(index) * samplerIncrement_;
        return range;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::AllocateRtv()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += uint64_t(rtvCount_++ % kRtvCapacity) * rtvIncrement_;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE D3D12Context::AllocateDsv()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += uint64_t(dsvCount_++ % kRtvCapacity) * dsvIncrement_;
        return handle;
    }
}
