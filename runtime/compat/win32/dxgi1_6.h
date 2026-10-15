// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The DXGI subset the renderer uses, for Apple/Linux hosts. DirectX-Headers
// (MIT) supply DXGI_FORMAT and the common structs; the factory, adapter,
// output and swap-chain interfaces are declared here with only the methods
// the renderer calls, and are implemented by the Metal backend
// (runtime/src/gx/metal/). Interface identifiers are local to this build.

#include <Windows.h>
#include <directx/dxgicommon.h>
#include <directx/dxgiformat.h>

#include "galaxy/win32_gui_posix.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
typedef UINT DXGI_USAGE;
#define DXGI_USAGE_SHADER_INPUT 0x00000010UL
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x00000020UL
#define DXGI_CREATE_FACTORY_DEBUG 0x1
#define DXGI_MWA_NO_WINDOW_CHANGES (1 << 0)
#define DXGI_MWA_NO_ALT_ENTER (1 << 1)
#define DXGI_MWA_NO_PRINT_SCREEN (1 << 2)
#define DXGI_PRESENT_TEST 0x00000001UL
#define DXGI_PRESENT_DO_NOT_WAIT 0x00000008UL
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0001L)
#define DXGI_ERROR_WAS_STILL_DRAWING ((HRESULT)0x887A000AL)
#define DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ((HRESULT)0x887A0022L)

typedef enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4
} DXGI_SWAP_EFFECT;

typedef enum DXGI_SWAP_CHAIN_FLAG {
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE = 4,
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 64,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048
} DXGI_SWAP_CHAIN_FLAG;

typedef enum DXGI_SCALING {
    DXGI_SCALING_STRETCH = 0,
    DXGI_SCALING_NONE = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2
} DXGI_SCALING;

typedef enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3
} DXGI_ALPHA_MODE;

typedef enum DXGI_MODE_SCANLINE_ORDER {
    DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED = 0,
    DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE = 1
} DXGI_MODE_SCANLINE_ORDER;

typedef enum DXGI_MODE_SCALING {
    DXGI_MODE_SCALING_UNSPECIFIED = 0,
    DXGI_MODE_SCALING_CENTERED = 1,
    DXGI_MODE_SCALING_STRETCHED = 2
} DXGI_MODE_SCALING;

typedef enum DXGI_MODE_ROTATION {
    DXGI_MODE_ROTATION_UNSPECIFIED = 0,
    DXGI_MODE_ROTATION_IDENTITY = 1
} DXGI_MODE_ROTATION;

typedef enum DXGI_GPU_PREFERENCE {
    DXGI_GPU_PREFERENCE_UNSPECIFIED = 0,
    DXGI_GPU_PREFERENCE_MINIMUM_POWER = 1,
    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE = 2
} DXGI_GPU_PREFERENCE;

typedef enum DXGI_MEMORY_SEGMENT_GROUP {
    DXGI_MEMORY_SEGMENT_GROUP_LOCAL = 0,
    DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1
} DXGI_MEMORY_SEGMENT_GROUP;

typedef enum DXGI_FEATURE {
    DXGI_FEATURE_PRESENT_ALLOW_TEARING = 0
} DXGI_FEATURE;

typedef enum DXGI_ADAPTER_FLAG {
    DXGI_ADAPTER_FLAG_NONE = 0,
    DXGI_ADAPTER_FLAG_REMOTE = 1,
    DXGI_ADAPTER_FLAG_SOFTWARE = 2
} DXGI_ADAPTER_FLAG;

// ---------------------------------------------------------------------------
// Structures
// ---------------------------------------------------------------------------
typedef struct DXGI_MODE_DESC {
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
} DXGI_MODE_DESC;

typedef struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
} DXGI_SWAP_CHAIN_DESC1;

typedef struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC {
    DXGI_RATIONAL RefreshRate;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
    BOOL Windowed;
} DXGI_SWAP_CHAIN_FULLSCREEN_DESC;

typedef struct DXGI_ADAPTER_DESC1 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
    UINT Flags;
} DXGI_ADAPTER_DESC1;

typedef struct DXGI_OUTPUT_DESC {
    WCHAR DeviceName[32];
    RECT DesktopCoordinates;
    BOOL AttachedToDesktop;
    DXGI_MODE_ROTATION Rotation;
    HMONITOR Monitor;
} DXGI_OUTPUT_DESC;

typedef struct DXGI_FRAME_STATISTICS {
    UINT PresentCount;
    UINT PresentRefreshCount;
    UINT SyncRefreshCount;
    LARGE_INTEGER SyncQPCTime;
    LARGE_INTEGER SyncGPUTime;
} DXGI_FRAME_STATISTICS;

typedef struct DXGI_QUERY_VIDEO_MEMORY_INFO {
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 AvailableForReservation;
    UINT64 CurrentReservation;
} DXGI_QUERY_VIDEO_MEMORY_INFO;

// ---------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------
struct IDXGIOutput;
struct IDXGIAdapter1;

MIDL_INTERFACE("aec22fb8-76f3-4639-9be0-28eb43a67a2e")
IDXGIObject : public IUnknown {};
__CRT_UUID_DECL(IDXGIObject, 0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e)

MIDL_INTERFACE("ae02eedb-c735-4690-8d52-5a8dc20213aa")
IDXGIOutput : public IDXGIObject {
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* desc) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(
        const DXGI_MODE_DESC* mode_to_match, DXGI_MODE_DESC* closest_match,
        IUnknown* concerned_device) = 0;
};
__CRT_UUID_DECL(IDXGIOutput, 0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa)

MIDL_INTERFACE("2411e7e1-12ac-4ccf-bd14-9798e8534dc0")
IDXGIAdapter : public IDXGIObject {
    virtual HRESULT STDMETHODCALLTYPE EnumOutputs(UINT output, IDXGIOutput** out) = 0;
};
__CRT_UUID_DECL(IDXGIAdapter, 0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0)

MIDL_INTERFACE("29038f61-3839-4626-91fd-086879011a05")
IDXGIAdapter1 : public IDXGIAdapter {
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* desc) = 0;
};
__CRT_UUID_DECL(IDXGIAdapter1, 0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05)

MIDL_INTERFACE("645967a4-1392-4310-a798-8053ce3e93fd")
IDXGIAdapter3 : public IDXGIAdapter1 {
    virtual HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(
        UINT node_index, DXGI_MEMORY_SEGMENT_GROUP group,
        DXGI_QUERY_VIDEO_MEMORY_INFO* info) = 0;
};
__CRT_UUID_DECL(IDXGIAdapter3, 0x645967a4, 0x1392, 0x4310, 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd)

MIDL_INTERFACE("310d36a0-d2e7-4c0a-aa04-6a9d23b8886a")
IDXGISwapChain : public IDXGIObject {
    virtual HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void** surface) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* target) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) = 0;
};
__CRT_UUID_DECL(IDXGISwapChain, 0x310d36a0, 0xd2e7, 0x4c0a, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a)

MIDL_INTERFACE("790a45f7-0d42-4876-983a-0a55cfe6f4aa")
IDXGISwapChain1 : public IDXGISwapChain {
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) = 0;
};
__CRT_UUID_DECL(IDXGISwapChain1, 0x790a45f7, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa)

MIDL_INTERFACE("a8be2ac4-199f-4946-b331-79599fb98de7")
IDXGISwapChain2 : public IDXGISwapChain1 {
    virtual HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) = 0;
    virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() = 0;
};
__CRT_UUID_DECL(IDXGISwapChain2, 0xa8be2ac4, 0x199f, 0x4946, 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7)

MIDL_INTERFACE("94d99bdb-f1f8-4ab0-b236-7da0170edab1")
IDXGISwapChain3 : public IDXGISwapChain2 {
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() = 0;
};
__CRT_UUID_DECL(IDXGISwapChain3, 0x94d99bdb, 0xf1f8, 0x4ab0, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1)

MIDL_INTERFACE("7b7166ec-21c7-44ae-b21a-c9ae321ae369")
IDXGIFactory : public IDXGIObject {
    virtual HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) = 0;
};
__CRT_UUID_DECL(IDXGIFactory, 0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69)

MIDL_INTERFACE("770aae78-f26f-4dba-a829-253c83d1b387")
IDXGIFactory1 : public IDXGIFactory {
    virtual HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT adapter, IDXGIAdapter1** out) = 0;
};
__CRT_UUID_DECL(IDXGIFactory1, 0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87)

MIDL_INTERFACE("50c83a1c-e072-4c48-87b0-3630fa36a6d0")
IDXGIFactory2 : public IDXGIFactory1 {
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
        IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        IDXGIOutput* restrict_to_output, IDXGISwapChain1** swap_chain) = 0;
};
__CRT_UUID_DECL(IDXGIFactory2, 0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0)

MIDL_INTERFACE("25483823-cd46-4c7d-86ca-47aa95b837bd")
IDXGIFactory3 : public IDXGIFactory2 {};
__CRT_UUID_DECL(IDXGIFactory3, 0x25483823, 0xcd46, 0x4c7d, 0x86, 0xca, 0x47, 0xaa, 0x95, 0xb8, 0x37, 0xbd)

MIDL_INTERFACE("1bc6ea02-ef36-464f-bf0c-21ca39e5168a")
IDXGIFactory4 : public IDXGIFactory3 {
    // The software adapter; here the one adapter there is.
    virtual HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** adapter) = 0;
};
__CRT_UUID_DECL(IDXGIFactory4, 0x1bc6ea02, 0xef36, 0x464f, 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a)

MIDL_INTERFACE("7632e1f5-ee65-4dca-87fd-84cd75f8838d")
IDXGIFactory5 : public IDXGIFactory4 {
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
        DXGI_FEATURE feature, void* data, UINT data_size) = 0;
};
__CRT_UUID_DECL(IDXGIFactory5, 0x7632e1f5, 0xee65, 0x4dca, 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d)

MIDL_INTERFACE("c1b6694f-ff09-44a9-b03c-77900a0a1d17")
IDXGIFactory6 : public IDXGIFactory5 {
    virtual HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(
        UINT adapter, DXGI_GPU_PREFERENCE preference, REFIID riid, void** out) = 0;
};
__CRT_UUID_DECL(IDXGIFactory6, 0xc1b6694f, 0xff09, 0x44a9, 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17)

// Implemented by the Metal backend.
HRESULT CreateDXGIFactory1(REFIID riid, void** factory);
HRESULT CreateDXGIFactory2(UINT flags, REFIID riid, void** factory);
