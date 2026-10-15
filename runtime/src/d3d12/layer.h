// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// A Direct3D 12 subset for Apple/Linux hosts, so the renderer
// (runtime/src/gx/renderer_d3d12.cpp and its caches) runs unchanged there.
//
// The D3D12 object model (resources, descriptor heaps, root signatures,
// pipeline state, command recording, queues and fences) is implemented here,
// in portable C++. Executing recorded work is a backend's job: Metal on
// Apple hosts (backend_metal.mm), a CPU backend elsewhere (backend_cpu.cpp:
// copies, clears and mapping run; draws do not) so the layer and the
// renderer's non-pixel paths are testable on Linux.
//
// Shaders are HLSL compiled by D3DCompile to SPIR-V (compiler.cpp); the
// Metal backend generates MSL per pipeline (galaxy/gx/shader_translate.h).

#include "com.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace galaxy::d3d12 {

class Backend;
class Resource;

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------
struct FormatInfo {
    std::uint32_t bytes_per_block = 0;  // 0: unsupported
    std::uint32_t block_width = 1;
    std::uint32_t block_height = 1;
    bool depth = false;
    bool stencil = false;
};
[[nodiscard]] FormatInfo format_info(DXGI_FORMAT format) noexcept;

// ---------------------------------------------------------------------------
// Backend objects
// ---------------------------------------------------------------------------
// Memory for a buffer, or a texture's storage, owned by the backend.
class BackendResource {
public:
    virtual ~BackendResource() = default;
    // Buffers: CPU-visible bytes (every heap type on these hosts, which
    // share memory with the GPU). Textures: null.
    [[nodiscard]] virtual std::uint8_t* cpu() noexcept = 0;
};

// One compiled graphics pipeline.
class BackendPipeline {
public:
    virtual ~BackendPipeline() = default;
};

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
class Resource final : public DeviceChild<ID3D12Resource, ID3D12Pageable, ID3D12DeviceChild,
                                          ID3D12Object> {
public:
    Resource(Device* device, const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap_type,
             D3D12_RESOURCE_STATES initial_state, std::unique_ptr<BackendResource> storage,
             std::uint64_t gpu_address);
    ~Resource() override;

    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE* read_range,
                                  void** data) override;
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE* written_range) override;
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
        return gpu_address_;
    }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX*, const void*, UINT,
                                                 UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*, UINT, UINT, UINT,
                                                  const D3D12_BOX*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties,
                                                D3D12_HEAP_FLAGS* flags) override;

    [[nodiscard]] const D3D12_RESOURCE_DESC& desc() const noexcept { return desc_; }
    [[nodiscard]] bool is_buffer() const noexcept {
        return desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    }
    [[nodiscard]] D3D12_HEAP_TYPE heap_type() const noexcept { return heap_type_; }
    [[nodiscard]] BackendResource* storage() const noexcept { return storage_.get(); }
    // Swap-chain buffers are presented from; set by the swap chain.
    std::uint32_t swap_chain_index = UINT32_MAX;

private:
    D3D12_RESOURCE_DESC desc_;
    D3D12_HEAP_TYPE heap_type_;
    std::unique_ptr<BackendResource> storage_;
    std::uint64_t gpu_address_;
};

// Placed resources share an ID3D12Heap's address range on Windows; here a
// heap only records its description and placed resources own their memory
// (the renderer never aliases placed resources).
class Heap final : public DeviceChild<ID3D12Heap, ID3D12Pageable, ID3D12DeviceChild,
                                      ID3D12Object> {
public:
    Heap(Device* device, const D3D12_HEAP_DESC& desc) : DeviceChild(device), desc_(desc) {}
    D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }

private:
    D3D12_HEAP_DESC desc_;
};

// The process-wide GPU virtual address space for buffers.
class AddressSpace {
public:
    static AddressSpace& instance();
    std::uint64_t reserve(Resource* resource, std::uint64_t size);
    void release(std::uint64_t base) noexcept;
    // The buffer containing `address` and the offset into it.
    [[nodiscard]] std::pair<Resource*, std::uint64_t> find(std::uint64_t address) const noexcept;

private:
    mutable std::shared_mutex mutex_;
    std::map<std::uint64_t, std::pair<Resource*, std::uint64_t>> ranges_;  // base -> (buffer, size)
    std::uint64_t next_ = 0x0000'0010'0000'0000ull;
};

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------
enum class DescriptorKind : std::uint8_t { None, Cbv, Srv, Uav, Rtv, Dsv, Sampler };

// A descriptor: what D3D12 writes into a heap slot. Plain data, copied by
// CopyDescriptors; it does not keep its resource alive (as in D3D12).
struct Descriptor {
    DescriptorKind kind = DescriptorKind::None;
    bool has_view_desc = false;
    Resource* resource = nullptr;
    union {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
        D3D12_RENDER_TARGET_VIEW_DESC rtv;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
        D3D12_SAMPLER_DESC sampler;
    } view{};

    // The view's format (the resource's for a null view description).
    [[nodiscard]] DXGI_FORMAT format() const noexcept;
    // The mip level a render-target/depth view writes.
    [[nodiscard]] UINT mip_slice() const noexcept;
};
static_assert(std::is_trivially_copyable_v<Descriptor>);

class DescriptorHeap final
    : public DeviceChild<ID3D12DescriptorHeap, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    DescriptorHeap(Device* device, const D3D12_DESCRIPTOR_HEAP_DESC& desc);
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    // Both handles are addresses of the slots: descriptor tables are read
    // when the recorded work executes, as on a GPU.
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override {
        return {reinterpret_cast<SIZE_T>(slots_.data())};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override {
        return {reinterpret_cast<UINT64>(slots_.data())};
    }

private:
    D3D12_DESCRIPTOR_HEAP_DESC desc_;
    std::vector<Descriptor> slots_;
};

[[nodiscard]] inline Descriptor* descriptor_at(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
    return reinterpret_cast<Descriptor*>(handle.ptr);
}
[[nodiscard]] inline const Descriptor* descriptor_at(D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
    return reinterpret_cast<const Descriptor*>(handle.ptr);
}

// ---------------------------------------------------------------------------
// Root signatures
// ---------------------------------------------------------------------------
struct DescriptorRange {
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    UINT count;  // UINT_MAX: unbounded
    UINT base_register;
    UINT space;
    UINT offset;  // in descriptors from the table start
};

struct RootParameter {
    D3D12_ROOT_PARAMETER_TYPE type;
    D3D12_SHADER_VISIBILITY visibility;
    UINT shader_register = 0;  // root CBV/SRV/UAV and constants
    UINT space = 0;
    UINT num_constants = 0;
    std::vector<DescriptorRange> ranges;  // tables
};

struct RootSignatureLayout {
    std::vector<RootParameter> parameters;
    std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers;
    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
};

// D3D12SerializeRootSignature's blob format on these hosts.
[[nodiscard]] std::vector<std::uint8_t> serialize_root_signature(
    const D3D12_ROOT_SIGNATURE_DESC& desc);
[[nodiscard]] bool deserialize_root_signature(std::span<const std::uint8_t> bytes,
                                              RootSignatureLayout& layout);

class RootSignature final
    : public DeviceChild<ID3D12RootSignature, ID3D12DeviceChild, ID3D12Object> {
public:
    RootSignature(Device* device, RootSignatureLayout layout)
        : DeviceChild(device), layout_(std::move(layout)) {}
    [[nodiscard]] const RootSignatureLayout& layout() const noexcept { return layout_; }

private:
    RootSignatureLayout layout_;
};

// ---------------------------------------------------------------------------
// Shaders (D3DCompile blobs)
// ---------------------------------------------------------------------------
enum class ShaderStage : std::uint32_t { Vertex = 0, Pixel = 1, Geometry = 2, Compute = 3 };

struct ShaderProgram {
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry_point;
    std::string label;
    // SPIR-V; empty for a geometry shader, which Metal cannot run.
    std::vector<std::uint32_t> spirv;
};

[[nodiscard]] std::vector<std::uint8_t> encode_shader(const ShaderProgram& program);
[[nodiscard]] bool decode_shader(const void* data, std::size_t size, ShaderProgram& program);

// ---------------------------------------------------------------------------
// Pipeline state
// ---------------------------------------------------------------------------
struct InputElement {
    std::string semantic_name;
    UINT semantic_index;
    DXGI_FORMAT format;
    UINT input_slot;
    UINT aligned_byte_offset;
    D3D12_INPUT_CLASSIFICATION classification;
    UINT instance_step_rate;
};

struct GraphicsPipelineInfo {
    std::optional<ShaderProgram> vs;
    std::optional<ShaderProgram> ps;
    bool has_geometry_shader = false;
    D3D12_BLEND_DESC blend{};
    UINT sample_mask = 0xFFFFFFFFu;
    D3D12_RASTERIZER_DESC rasterizer{};
    D3D12_DEPTH_STENCIL_DESC depth_stencil{};
    std::vector<InputElement> input_layout;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    UINT num_render_targets = 0;
    std::array<DXGI_FORMAT, 8> rtv_formats{};
    DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sample{1, 0};
    std::string label;
};

class PipelineState final
    : public DeviceChild<ID3D12PipelineState, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    PipelineState(Device* device, GraphicsPipelineInfo info,
                  std::unique_ptr<BackendPipeline> compiled)
        : DeviceChild(device), info_(std::move(info)), compiled_(std::move(compiled)) {}
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** blob) override;
    [[nodiscard]] const GraphicsPipelineInfo& info() const noexcept { return info_; }
    [[nodiscard]] BackendPipeline* compiled() const noexcept { return compiled_.get(); }

private:
    GraphicsPipelineInfo info_;
    std::unique_ptr<BackendPipeline> compiled_;
};

// ---------------------------------------------------------------------------
// Recorded commands
// ---------------------------------------------------------------------------
namespace cmd {
struct SetPipeline { PipelineState* pipeline; };
struct SetRootSignature { RootSignature* signature; };
struct SetRootTable { UINT index; const Descriptor* table; };
struct SetRootView { UINT index; std::uint64_t address; };
struct SetRootConstants { UINT index; UINT first; std::vector<std::uint32_t> values; };
struct SetTopology { D3D12_PRIMITIVE_TOPOLOGY topology; };
struct SetViewports { std::vector<D3D12_VIEWPORT> viewports; };
struct SetScissors { std::vector<D3D12_RECT> rects; };
struct SetBlendFactor { std::array<float, 4> factor; };
struct SetStencilRef { UINT reference; };
struct SetVertexBuffers { UINT start; std::vector<D3D12_VERTEX_BUFFER_VIEW> views; };
struct SetIndexBuffer { D3D12_INDEX_BUFFER_VIEW view; };
// Render-target descriptors are read when recorded (D3D12 semantics).
struct SetRenderTargets {
    std::vector<Descriptor> colors;
    std::optional<Descriptor> depth;
};
struct ClearRenderTarget { Descriptor target; std::array<float, 4> color; std::vector<D3D12_RECT> rects; };
struct ClearDepthStencil {
    Descriptor target;
    D3D12_CLEAR_FLAGS flags;
    float depth;
    std::uint8_t stencil;
    std::vector<D3D12_RECT> rects;
};
struct Draw {
    bool indexed;
    UINT count;
    UINT instances;
    UINT first;  // vertex or index
    INT base_vertex;
    UINT first_instance;
};
struct CopyBufferRegion {
    Resource* dst;
    UINT64 dst_offset;
    Resource* src;
    UINT64 src_offset;
    UINT64 bytes;
};
struct CopyTextureRegion {
    D3D12_TEXTURE_COPY_LOCATION dst;
    UINT dst_x, dst_y, dst_z;
    D3D12_TEXTURE_COPY_LOCATION src;
    std::optional<D3D12_BOX> src_box;
};
struct CopyResource { Resource* dst; Resource* src; };
struct Barrier {};
struct Discard { Resource* resource; };
struct ResolveQueries { UINT first; UINT count; Resource* dst; UINT64 dst_offset; };
using Command = std::variant<SetPipeline, SetRootSignature, SetRootTable, SetRootView,
                             SetRootConstants, SetTopology, SetViewports, SetScissors,
                             SetBlendFactor, SetStencilRef, SetVertexBuffers, SetIndexBuffer,
                             SetRenderTargets, ClearRenderTarget, ClearDepthStencil, Draw,
                             CopyBufferRegion, CopyTextureRegion, CopyResource, Barrier, Discard,
                             ResolveQueries>;
}  // namespace cmd

class CommandAllocator final : public DeviceChild<ID3D12CommandAllocator, ID3D12Pageable,
                                                  ID3D12DeviceChild, ID3D12Object> {
public:
    using DeviceChild::DeviceChild;
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
};

class CommandList final : public DeviceChild<ID3D12GraphicsCommandList, ID3D12CommandList,
                                             ID3D12DeviceChild, ID3D12Object> {
public:
    CommandList(Device* device, D3D12_COMMAND_LIST_TYPE type, PipelineState* initial);

    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return type_; }
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* allocator,
                                    ID3D12PipelineState* initial) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pipeline) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT vertices, UINT instances, UINT first_vertex,
                                         UINT first_instance) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index,
                                                INT base_vertex, UINT first_instance) override;
    void STDMETHODCALLTYPE Dispatch(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset,
                                            ID3D12Resource* src, UINT64 src_offset,
                                            UINT64 bytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x,
                                             UINT y, UINT z,
                                             const D3D12_TEXTURE_COPY_LOCATION* src,
                                             const D3D12_BOX* src_box) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* dst, ID3D12Resource* src) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
                                     const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64,
                                     D3D12_TILE_COPY_FLAGS) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource*, UINT, ID3D12Resource*, UINT,
                                              DXGI_FORMAT) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT count, const D3D12_RECT* rects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT factor[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT reference) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pipeline) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT, const D3D12_RESOURCE_BARRIER*) override {}
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList*) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT, ID3D12DescriptorHeap* const*) override {}
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature*) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* signature) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT index,
                                                          D3D12_GPU_DESCRIPTOR_HANDLE table) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT index, UINT value,
                                                        UINT offset) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT, UINT, const void*, UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT index, UINT count,
                                                         const void* values, UINT offset) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT index,
                                                             D3D12_GPU_VIRTUAL_ADDRESS address) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT index,
                                                             D3D12_GPU_VIRTUAL_ADDRESS address) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT index,
                                                              D3D12_GPU_VIRTUAL_ADDRESS address) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT count,
                                              const D3D12_VERTEX_BUFFER_VIEW* views) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT count,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* colors,
                                              BOOL single_handle_range,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* depth) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view,
                                                 D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                                 UINT8 stencil, UINT rect_count,
                                                 const D3D12_RECT* rects) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view,
                                                 const FLOAT color[4], UINT rect_count,
                                                 const D3D12_RECT* rects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE,
                                                        ID3D12Resource*, const UINT[4], UINT,
                                                        const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE,
                                                         ID3D12Resource*, const FLOAT[4], UINT,
                                                         const D3D12_RECT*) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* resource,
                                           const D3D12_DISCARD_REGION* region) override;
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override {}
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override {}
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                            UINT first, UINT count, ID3D12Resource* dst,
                                            UINT64 dst_offset) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) override {}
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                                           ID3D12Resource*, UINT64) override;

    [[nodiscard]] const std::vector<cmd::Command>& commands() const noexcept { return commands_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    void unsupported(const char* what);
    template <typename T>
    void record(T&& command) {
        commands_.emplace_back(std::forward<T>(command));
    }

    D3D12_COMMAND_LIST_TYPE type_;
    std::vector<cmd::Command> commands_;
    bool closed_ = false;
};

// ---------------------------------------------------------------------------
// Fences, queues, queries
// ---------------------------------------------------------------------------
class Fence final
    : public DeviceChild<ID3D12Fence, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    Fence(Device* device, UINT64 initial) : DeviceChild(device), value_(initial) {}
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override;
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override;
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override;
    // Completion from the backend (any thread).
    void complete(UINT64 value);
    // Blocks until the fence reaches `value` (queue Wait).
    void wait(UINT64 value);

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    UINT64 value_;
    std::vector<std::pair<UINT64, HANDLE>> waiters_;
};

class QueryHeap final
    : public DeviceChild<ID3D12QueryHeap, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    QueryHeap(Device* device, const D3D12_QUERY_HEAP_DESC& desc)
        : DeviceChild(device), desc_(desc) {}

private:
    D3D12_QUERY_HEAP_DESC desc_;
};

class CommandQueue final
    : public DeviceChild<ID3D12CommandQueue, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    CommandQueue(Device* device, const D3D12_COMMAND_QUEUE_DESC& desc)
        : DeviceChild(device), desc_(desc) {}
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT,
                                              const D3D12_TILED_RESOURCE_COORDINATE*,
                                              const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT,
                                              const D3D12_TILE_RANGE_FLAGS*, const UINT*,
                                              const UINT*, D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
                                            ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
                                            const D3D12_TILE_REGION_SIZE*,
                                            D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count,
                                               ID3D12CommandList* const* lists) override;
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override;
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }

private:
    D3D12_COMMAND_QUEUE_DESC desc_;
};

// ---------------------------------------------------------------------------
// Backend interface
// ---------------------------------------------------------------------------
struct TextureInfo {
    D3D12_RESOURCE_DESC desc;
    bool render_target;
    bool depth_stencil;
};

class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual std::wstring adapter_description() const = 0;
    [[nodiscard]] virtual std::uint64_t video_memory_bytes() const noexcept = 0;

    [[nodiscard]] virtual std::unique_ptr<BackendResource> create_buffer(std::uint64_t size) = 0;
    [[nodiscard]] virtual std::unique_ptr<BackendResource> create_texture(
        const TextureInfo& info) = 0;
    // Null with `error` set on failure.
    [[nodiscard]] virtual std::unique_ptr<BackendPipeline> create_pipeline(
        const GraphicsPipelineInfo& info, std::string& error) = 0;

    // Executes the closed lists in order. `done` runs (on any thread) once
    // the work has completed; the lists are retained until then.
    virtual void execute(std::span<CommandList* const> lists, std::function<void()> done) = 0;
    // Runs `done` once all previously executed work has completed.
    virtual void after_pending_work(std::function<void()> done) = 0;

    // Shows a swap-chain buffer (a texture resource) in the app shell's view.
    virtual void present(Resource* buffer, UINT sync_interval) = 0;
    // The view's refresh rate in hertz (0 unknown).
    [[nodiscard]] virtual double refresh_rate_hz() const noexcept { return 60.0; }
};

// The platform's backend: Metal on Apple hosts, CPU elsewhere (or with
// GALAXY_D3D12_BACKEND=cpu).
[[nodiscard]] std::unique_ptr<Backend> create_backend();
[[nodiscard]] std::unique_ptr<Backend> create_cpu_backend();

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------
class Device final : public Object<ID3D12Device, ID3D12Object> {
public:
    explicit Device(std::unique_ptr<Backend> backend);
    ~Device() override;

    [[nodiscard]] Backend& backend() noexcept { return *backend_; }

    UINT STDMETHODCALLTYPE GetNodeCount() override { return 1u; }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid,
                                                 void** queue) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid,
                                                     void** allocator) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** pipeline) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
        const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                                ID3D12CommandAllocator* allocator,
                                                ID3D12PipelineState* initial, REFIID riid,
                                                void** list) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void* data,
                                                  UINT size) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                                   REFIID riid, void** heap) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override {
        return sizeof(Descriptor);
    }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT node_mask, const void* blob, SIZE_T size,
                                                  REFIID riid, void** signature) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* resource,
                                                    const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource*,
                                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* resource,
                                                  const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* resource,
                                                  const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* desc,
                                         D3D12_CPU_DESCRIPTOR_HANDLE dest) override;
    void STDMETHODCALLTYPE CopyDescriptors(UINT dest_ranges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts,
                                           const UINT* dest_sizes, UINT src_ranges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                                           const UINT* src_sizes,
                                           D3D12_DESCRIPTOR_HEAP_TYPE type) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE src,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE type) override;
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(
        UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC* descs) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT,
                                                                    D3D12_HEAP_TYPE type) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap,
                                                      D3D12_HEAP_FLAGS flags,
                                                      const D3D12_RESOURCE_DESC* desc,
                                                      D3D12_RESOURCE_STATES state,
                                                      const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                                      void** resource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid,
                                         void** heap) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* heap, UINT64 offset,
                                                   const D3D12_RESOURCE_DESC* desc,
                                                   D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                                   void** resource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC*,
                                                     D3D12_RESOURCE_STATES,
                                                     const D3D12_CLEAR_VALUE*, REFIID,
                                                     void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*,
                                                 DWORD, LPCWSTR, HANDLE*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable* const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable* const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid,
                                          void** fence) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT first,
                                                 UINT count, UINT64 base_offset,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts,
                                                 UINT* rows, UINT64* row_bytes,
                                                 UINT64* total_bytes) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, REFIID riid,
                                              void** heap) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*,
                                                     ID3D12RootSignature*, REFIID,
                                                     void**) override {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*,
                                             D3D12_TILE_SHAPE*, UINT*, UINT,
                                             D3D12_SUBRESOURCE_TILING*) override {}
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return LUID{0x4e42u, 0}; }

private:
    HRESULT create_resource(const D3D12_HEAP_PROPERTIES& heap, const D3D12_RESOURCE_DESC& desc,
                            D3D12_RESOURCE_STATES state, REFIID riid, void** out);

    std::unique_ptr<Backend> backend_;
};

// Subresource layout of a texture in a buffer (GetCopyableFootprints rules:
// rows aligned to 256 bytes, subresources to 512).
struct SubresourceFootprint {
    std::uint32_t width, height, depth;
    std::uint32_t row_pitch;
    std::uint32_t rows;  // block rows
};
[[nodiscard]] SubresourceFootprint subresource_footprint(const D3D12_RESOURCE_DESC& desc,
                                                         UINT subresource) noexcept;

}  // namespace galaxy::d3d12
