// SPDX-License-Identifier: GPL-3.0-only
// DXGI for the D3D12 layer: one factory, one adapter (the backend's GPU),
// one output (the app shell's screen) and a swap chain whose buffers are
// ordinary render-target textures shown by the backend. See layer.h.

#include "layer.h"

#include "galaxy/win32_gui_posix.h"

#include <algorithm>
#include <cwchar>
#include <iostream>

namespace galaxy::d3d12 {
namespace {

class Output final : public Unknown<IDXGIOutput, IDXGIObject> {
public:
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = {};
        std::wcsncpy(desc->DeviceName, L"\\\\.\\DISPLAY1", 31);
        desc->DesktopCoordinates =
            RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        desc->AttachedToDesktop = TRUE;
        desc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        desc->Monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* wanted,
                                                      DXGI_MODE_DESC* closest,
                                                      IUnknown*) override {
        if (wanted == nullptr || closest == nullptr) return E_INVALIDARG;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        (void)EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode);
        *closest = *wanted;
        closest->Width = mode.dmPelsWidth;
        closest->Height = mode.dmPelsHeight;
        closest->RefreshRate = DXGI_RATIONAL{mode.dmDisplayFrequency, 1u};
        if (closest->Format == DXGI_FORMAT_UNKNOWN) closest->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return S_OK;
    }
};

class Adapter final : public Unknown<IDXGIAdapter3, IDXGIAdapter1, IDXGIAdapter, IDXGIObject> {
public:
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT index, IDXGIOutput** output) override {
        if (output == nullptr) return E_INVALIDARG;
        *output = nullptr;
        if (index != 0u) return DXGI_ERROR_NOT_FOUND;
        *output = new Output;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = {};
        const std::wstring name = probe().adapter_description();
        std::wcsncpy(desc->Description, name.c_str(), 127);
        desc->VendorId = 0x106Bu;  // Apple
        desc->DedicatedVideoMemory = static_cast<SIZE_T>(probe().video_memory_bytes());
        desc->AdapterLuid = LUID{0x4e42u, 0};
        desc->Flags = 0u;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT, DXGI_MEMORY_SEGMENT_GROUP,
                                                   DXGI_QUERY_VIDEO_MEMORY_INFO* info) override {
        if (info == nullptr) return E_INVALIDARG;
        *info = {};
        info->Budget = probe().video_memory_bytes();
        return S_OK;
    }

private:
    Backend& probe() {
        if (backend_ == nullptr) backend_ = create_backend();
        return *backend_;
    }
    std::unique_ptr<Backend> backend_;
};

class Factory final : public Unknown<IDXGIFactory6, IDXGIFactory5, IDXGIFactory4, IDXGIFactory3,
                                     IDXGIFactory2, IDXGIFactory1, IDXGIFactory, IDXGIObject> {
public:
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override {
        if (adapter == nullptr) return E_INVALIDARG;
        *adapter = nullptr;
        if (index != 0u) return DXGI_ERROR_NOT_FOUND;
        *adapter = new Adapter;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** adapter) override {
        if (adapter == nullptr) return E_INVALIDARG;
        *adapter = nullptr;
        return hand_out(new Adapter, riid, adapter);
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE,
                                                         REFIID riid, void** adapter) override {
        if (adapter == nullptr) return E_INVALIDARG;
        *adapter = nullptr;
        if (index != 0u) return DXGI_ERROR_NOT_FOUND;
        return hand_out(new Adapter, riid, adapter);
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void* data,
                                                  UINT size) override {
        if (feature == DXGI_FEATURE_PRESENT_ALLOW_TEARING && data != nullptr &&
            size == sizeof(BOOL)) {
            *static_cast<BOOL*>(data) = FALSE;
            return S_OK;
        }
        return E_INVALIDARG;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* queue, HWND window,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                     IDXGIOutput*,
                                                     IDXGISwapChain1** swap_chain) override;
};

// The swap chain's buffers are render-target textures; Present shows the
// current one through the backend and advances. The frame-latency
// waitable is an auto-reset event set as each present completes.
class SwapChain final : public Unknown<IDXGISwapChain3, IDXGISwapChain2, IDXGISwapChain1,
                                       IDXGISwapChain, IDXGIObject> {
public:
    SwapChain(Device* device, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc)
        : device_(device), window_(window), desc_(desc) {
        device_->AddRef();
        latency_event_ = CreateEventW(nullptr, FALSE, TRUE, nullptr);
        if (desc_.Width == 0u || desc_.Height == 0u) {
            RECT client{};
            if (GetClientRect(window_, &client)) {
                desc_.Width = static_cast<UINT>(std::max<LONG>(client.right, 1));
                desc_.Height = static_cast<UINT>(std::max<LONG>(client.bottom, 1));
            }
        }
    }
    ~SwapChain() override {
        release_buffers();
        if (latency_event_ != nullptr && !handed_out_) CloseHandle(latency_event_);
        device_->Release();
    }

    HRESULT create_buffers() {
        release_buffers();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC resource{};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource.Width = desc_.Width;
        resource.Height = desc_.Height;
        resource.DepthOrArraySize = 1;
        resource.MipLevels = 1;
        resource.Format = desc_.Format;
        resource.SampleDesc = {1, 0};
        resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (UINT i = 0; i < std::max<UINT>(desc_.BufferCount, 1u); ++i) {
            ID3D12Resource* buffer = nullptr;
            const HRESULT hr = device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffer));
            if (FAILED(hr)) return hr;
            static_cast<Resource*>(buffer)->swap_chain_index = i;
            buffers_.push_back(static_cast<Resource*>(buffer));
        }
        current_ = 0u;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) override {
        if ((flags & DXGI_PRESENT_TEST) != 0u) return S_OK;
        if (buffers_.empty()) return DXGI_ERROR_INVALID_CALL;
        device_->backend().present(buffers_[current_], sync_interval);
        const HANDLE event = latency_event_;
        device_->backend().after_pending_work([event] {
            if (event != nullptr) SetEvent(event);
        });
        current_ = (current_ + 1u) % static_cast<UINT>(buffers_.size());
        ++present_count_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** surface) override {
        if (surface == nullptr || index >= buffers_.size()) return DXGI_ERROR_INVALID_CALL;
        return buffers_[index]->QueryInterface(riid, surface);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override {
        // The shell's view is the screen; there is no exclusive mode.
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override {
        if (fullscreen != nullptr) *fullscreen = FALSE;
        if (target != nullptr) *target = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height,
                                            DXGI_FORMAT format, UINT flags) override {
        for (Resource* buffer : buffers_) {
            // GetBuffer references must have been released (DXGI rule).
            if (buffer->AddRef() > 2u) {
                buffer->Release();
                return DXGI_ERROR_INVALID_CALL;
            }
            buffer->Release();
        }
        if (count != 0u) desc_.BufferCount = count;
        if (format != DXGI_FORMAT_UNKNOWN) desc_.Format = format;
        desc_.Flags = flags;
        RECT client{};
        if ((width == 0u || height == 0u) && GetClientRect(window_, &client)) {
            width = width != 0u ? width : static_cast<UINT>(std::max<LONG>(client.right, 1));
            height = height != 0u ? height : static_cast<UINT>(std::max<LONG>(client.bottom, 1));
        }
        if (width != 0u) desc_.Width = width;
        if (height != 0u) desc_.Height = height;
        return create_buffers();
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override {
        if (output == nullptr) return E_INVALIDARG;
        *output = new Output;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override {
        if (stats == nullptr) return E_INVALIDARG;
        *stats = {};
        stats->PresentCount = present_count_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = desc_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT) override { return S_OK; }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        // The caller owns (and closes) the returned handle, after draining
        // the GPU; shim events cannot be duplicated, so it gets this one.
        handed_out_ = true;
        return latency_event_;
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return current_; }

private:
    void release_buffers() {
        for (Resource* buffer : buffers_) buffer->Release();
        buffers_.clear();
    }

    Device* device_;
    HWND window_;
    DXGI_SWAP_CHAIN_DESC1 desc_;
    std::vector<Resource*> buffers_;
    UINT current_ = 0u;
    UINT present_count_ = 0u;
    HANDLE latency_event_ = nullptr;
    bool handed_out_ = false;
};

HRESULT Factory::CreateSwapChainForHwnd(IUnknown* queue, HWND window,
                                        const DXGI_SWAP_CHAIN_DESC1* desc,
                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                        IDXGISwapChain1** swap_chain) {
    if (queue == nullptr || desc == nullptr || swap_chain == nullptr) return E_INVALIDARG;
    *swap_chain = nullptr;
    ID3D12CommandQueue* command_queue = nullptr;
    if (FAILED(queue->QueryInterface(__uuidof(ID3D12CommandQueue),
                                     reinterpret_cast<void**>(&command_queue)))) {
        return E_INVALIDARG;
    }
    Device* device = static_cast<CommandQueue*>(command_queue)->device();
    command_queue->Release();
    auto* chain = new SwapChain(device, window, *desc);
    const HRESULT hr = chain->create_buffers();
    if (FAILED(hr)) {
        chain->Release();
        return hr;
    }
    *swap_chain = chain;
    return S_OK;
}

}  // namespace
}  // namespace galaxy::d3d12

HRESULT CreateDXGIFactory2(UINT, REFIID riid, void** factory) {
    if (factory == nullptr) return E_INVALIDARG;
    return galaxy::d3d12::hand_out(new galaxy::d3d12::Factory, riid, factory);
}

HRESULT CreateDXGIFactory1(REFIID riid, void** factory) {
    return CreateDXGIFactory2(0u, riid, factory);
}
