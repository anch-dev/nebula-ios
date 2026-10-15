// SPDX-License-Identifier: GPL-3.0-only
// The D3D12 layer end to end: device, descriptor heaps, a render target,
// clears (whole and rectangle), a texture-to-buffer readback, fences and,
// on a rasterizing backend (Metal), a draw with root constants, a vertex
// buffer, a sampled texture and an uploaded texture.

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAILED: %s\n", what);
    }
}

constexpr UINT kSize = 8u;

struct Context {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    HANDLE event = nullptr;

    void submit_and_wait() {
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fence_value);
        fence->SetEventOnCompletion(fence_value, event);
        WaitForSingleObject(event, 10000);
        check(fence->GetCompletedValue() >= fence_value, "fence reaches the signaled value");
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }
};

ComPtr<ID3D12Resource> make_buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&buffer))),
          "buffer creation");
    return buffer;
}

ComPtr<ID3D12Resource> make_texture(ID3D12Device* device, bool render_target) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kSize;
    desc.Height = kSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    if (render_target) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;
    check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&texture))),
          "texture creation");
    return texture;
}

// Reads the texture back; returns RGBA8 rows of kSize texels.
std::vector<std::uint8_t> read_back(Context& context, ID3D12Resource* texture) {
    const UINT pitch = 256u;
    ComPtr<ID3D12Resource> readback =
        make_buffer(context.device.Get(), pitch * kSize, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    context.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    context.submit_and_wait();
    std::vector<std::uint8_t> pixels(kSize * kSize * 4u);
    void* mapped = nullptr;
    check(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "readback map");
    if (mapped != nullptr) {
        for (UINT y = 0; y < kSize; ++y) {
            std::memcpy(pixels.data() + y * kSize * 4u, static_cast<std::uint8_t*>(mapped) + y * pitch,
                        kSize * 4u);
        }
    }
    readback->Unmap(0, nullptr);
    return pixels;
}

bool texel_is(const std::vector<std::uint8_t>& pixels, UINT x, UINT y, std::uint8_t r,
              std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    const std::uint8_t* p = pixels.data() + (y * kSize + x) * 4u;
    const auto near = [](int v, int want) { return v >= want - 2 && v <= want + 2; };
    if (near(p[0], r) && near(p[1], g) && near(p[2], b) && near(p[3], a)) return true;
    std::printf("  texel (%u,%u) = %u %u %u %u, want %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3],
                r, g, b, a);
    return false;
}

ComPtr<ID3DBlob> compile(const char* source, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "test", nullptr, nullptr, "main",
                                  target, D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0, &code, &errors);
    if (FAILED(hr)) {
        std::printf("compile error: %s\n",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    }
    check(SUCCEEDED(hr), "D3DCompile");
    return code;
}

const char* kVs = R"(
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(VSIn v) { VSOut o; o.pos = float4(v.pos, 0.5, 1.0); o.uv = v.uv; return o; }
)";
const char* kPs = R"(
cbuffer Tint : register(b0) { float4 tint; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return tex.Sample(samp, uv) * tint;
}
)";

void draw_test(Context& c) {
    // Upload a solid texture (0,128,255,255) through a placed footprint.
    ComPtr<ID3D12Resource> texture = make_texture(c.device.Get(), false);
    ComPtr<ID3D12Resource> upload = make_buffer(c.device.Get(), 256u * kSize, D3D12_HEAP_TYPE_UPLOAD);
    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    for (UINT y = 0; y < kSize; ++y) {
        for (UINT x = 0; x < kSize; ++x) {
            std::uint8_t* p = static_cast<std::uint8_t*>(mapped) + y * 256u + x * 4u;
            p[0] = 0; p[1] = 128; p[2] = 255; p[3] = 255;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, 256u};
    c.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Root signature: b0 constants (4), table t0, static sampler s0.
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 4};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{2, params, 1, &sampler,
                                 D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> serialized, errors;
    check(SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)),
          "root signature serialization");
    ComPtr<ID3D12RootSignature> signature;
    check(SUCCEEDED(c.device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                  serialized->GetBufferSize(),
                                                  IID_PPV_ARGS(&signature))),
          "root signature creation");

    ComPtr<ID3DBlob> vs = compile(kVs, "vs_5_1");
    ComPtr<ID3DBlob> ps = compile(kPs, "ps_5_1");
    if (!vs || !ps) return;
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = signature.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.InputLayout = {layout, 2};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    check(SUCCEEDED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline))),
          "pipeline creation");
    if (!pipeline) return;

    // A quad over the left half: x in [-1, 0].
    const float vertices[] = {-1, 1, 0, 0,  0, 1, 1, 0,  -1, -1, 0, 1,
                              0, 1, 1, 0,   0, -1, 1, 1,  -1, -1, 0, 1};
    ComPtr<ID3D12Resource> vb = make_buffer(c.device.Get(), sizeof(vertices), D3D12_HEAP_TYPE_UPLOAD);
    vb->Map(0, nullptr, &mapped);
    std::memcpy(mapped, vertices, sizeof(vertices));

    ComPtr<ID3D12DescriptorHeap> srv_heap, rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap));
    c.device->CreateShaderResourceView(texture.Get(), nullptr,
                                       srv_heap->GetCPUDescriptorHandleForHeapStart());
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
    ComPtr<ID3D12Resource> target = make_texture(c.device.Get(), true);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    c.device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    const float black[4] = {0, 0, 0, 1};
    c.list->ClearRenderTargetView(rtv, black, 0, nullptr);
    c.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, kSize, kSize, 0, 1};
    D3D12_RECT scissor{0, 0, kSize, kSize};
    c.list->RSSetViewports(1, &viewport);
    c.list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
    c.list->SetDescriptorHeaps(1, heaps);
    c.list->SetGraphicsRootSignature(signature.Get());
    c.list->SetPipelineState(pipeline.Get());
    const float tint[4] = {1.0f, 1.0f, 0.5f, 1.0f};
    c.list->SetGraphicsRoot32BitConstants(0, 4, tint, 0);
    c.list->SetGraphicsRootDescriptorTable(1, srv_heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_VERTEX_BUFFER_VIEW vbv{vb->GetGPUVirtualAddress(), sizeof(vertices), 16};
    c.list->IASetVertexBuffers(0, 1, &vbv);
    c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c.list->DrawInstanced(6, 1, 0, 0);
    const std::vector<std::uint8_t> pixels = read_back(c, target.Get());
    check(texel_is(pixels, 1, 4, 0, 128, 128, 255), "draw: texture times root-constant tint");
    check(texel_is(pixels, 6, 4, 0, 0, 0, 255), "draw: right half untouched");
}

}  // namespace

int main() {
    Context c;
    check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device))),
          "device creation");
    if (!c.device) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(c.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.queue))), "queue");
    check(SUCCEEDED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&c.allocator))),
          "allocator");
    check(SUCCEEDED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                c.allocator.Get(), nullptr, IID_PPV_ARGS(&c.list))),
          "command list");
    check(SUCCEEDED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence))), "fence");
    c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Whole and rectangle clears, read back.
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(SUCCEEDED(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap))), "RTV heap");
    ComPtr<ID3D12Resource> target = make_texture(c.device.Get(), true);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    c.device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    const float red[4] = {1, 0, 0, 1};
    const float green[4] = {0, 1, 0, 1};
    c.list->ClearRenderTargetView(rtv, red, 0, nullptr);
    const D3D12_RECT corner{0, 0, 2, 2};
    c.list->ClearRenderTargetView(rtv, green, 1, &corner);
    const std::vector<std::uint8_t> pixels = read_back(c, target.Get());
    check(texel_is(pixels, 5, 5, 255, 0, 0, 255), "whole-target clear");
    check(texel_is(pixels, 1, 1, 0, 255, 0, 255), "rectangle clear inside");
    check(texel_is(pixels, 2, 2, 255, 0, 0, 255), "rectangle clear outside");

    // Draws need a rasterizing backend.
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && SUCCEEDED(adapter->GetDesc1(&desc)) &&
        std::wstring(desc.Description).find(L"no rasterizer") == std::wstring::npos) {
        draw_test(c);
    } else {
        std::printf("note: CPU backend; draw test skipped\n");
    }

    CloseHandle(c.event);
    std::printf("%d D3D12 layer failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
