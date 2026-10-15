// SPDX-License-Identifier: GPL-3.0-only
// Command recording for the D3D12 layer. Recording only captures commands
// (descriptor tables by address, render-target descriptors by value, root
// constants by value); the backend executes them. See layer.h.

#include "layer.h"

#include <cstdio>
#include <iostream>

namespace galaxy::d3d12 {

CommandList::CommandList(Device* device, D3D12_COMMAND_LIST_TYPE type, PipelineState* initial)
    : DeviceChild(device), type_(type) {
    if (initial != nullptr) record(cmd::SetPipeline{initial});
}

void CommandList::unsupported(const char* what) {
    static bool reported[8]{};
    (void)reported;
    std::cerr << "[d3d12] unsupported command: " << what << '\n';
}

HRESULT CommandList::Close() {
    if (closed_) return E_FAIL;
    closed_ = true;
    return S_OK;
}

HRESULT CommandList::Reset(ID3D12CommandAllocator*, ID3D12PipelineState* initial) {
    commands_.clear();
    closed_ = false;
    if (initial != nullptr) record(cmd::SetPipeline{static_cast<PipelineState*>(initial)});
    return S_OK;
}

void CommandList::ClearState(ID3D12PipelineState* pipeline) {
    record(cmd::SetPipeline{static_cast<PipelineState*>(pipeline)});
    record(cmd::SetRootSignature{nullptr});
}

void CommandList::DrawInstanced(UINT vertices, UINT instances, UINT first_vertex,
                                UINT first_instance) {
    record(cmd::Draw{false, vertices, instances, first_vertex, 0, first_instance});
}

void CommandList::DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index,
                                       INT base_vertex, UINT first_instance) {
    record(cmd::Draw{true, indices, instances, first_index, base_vertex, first_instance});
}

void CommandList::Dispatch(UINT, UINT, UINT) { unsupported("Dispatch"); }

void CommandList::CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src,
                                   UINT64 src_offset, UINT64 bytes) {
    record(cmd::CopyBufferRegion{static_cast<Resource*>(dst), dst_offset,
                                 static_cast<Resource*>(src), src_offset, bytes});
}

void CommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y,
                                    UINT z, const D3D12_TEXTURE_COPY_LOCATION* src,
                                    const D3D12_BOX* src_box) {
    if (dst == nullptr || src == nullptr) return;
    cmd::CopyTextureRegion copy{*dst, x, y, z, *src, std::nullopt};
    if (src_box != nullptr) copy.src_box = *src_box;
    record(std::move(copy));
}

void CommandList::CopyResource(ID3D12Resource* dst, ID3D12Resource* src) {
    record(cmd::CopyResource{static_cast<Resource*>(dst), static_cast<Resource*>(src)});
}

void CommandList::CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
                            const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64,
                            D3D12_TILE_COPY_FLAGS) {
    unsupported("CopyTiles");
}

void CommandList::ResolveSubresource(ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT) {
    unsupported("ResolveSubresource");
}

void CommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    record(cmd::SetTopology{topology});
}

void CommandList::RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
    record(cmd::SetViewports{std::vector<D3D12_VIEWPORT>(viewports, viewports + count)});
}

void CommandList::RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
    record(cmd::SetScissors{std::vector<D3D12_RECT>(rects, rects + count)});
}

void CommandList::OMSetBlendFactor(const FLOAT factor[4]) {
    cmd::SetBlendFactor command{{1.0f, 1.0f, 1.0f, 1.0f}};
    if (factor != nullptr) command.factor = {factor[0], factor[1], factor[2], factor[3]};
    record(command);
}

void CommandList::OMSetStencilRef(UINT reference) { record(cmd::SetStencilRef{reference}); }

void CommandList::SetPipelineState(ID3D12PipelineState* pipeline) {
    record(cmd::SetPipeline{static_cast<PipelineState*>(pipeline)});
}

void CommandList::ExecuteBundle(ID3D12GraphicsCommandList*) { unsupported("ExecuteBundle"); }

void CommandList::SetComputeRootSignature(ID3D12RootSignature*) {}

void CommandList::SetGraphicsRootSignature(ID3D12RootSignature* signature) {
    record(cmd::SetRootSignature{static_cast<RootSignature*>(signature)});
}

void CommandList::SetComputeRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) {}

void CommandList::SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    record(cmd::SetRootTable{index, descriptor_at(table)});
}

void CommandList::SetComputeRoot32BitConstant(UINT, UINT, UINT) {}

void CommandList::SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT offset) {
    record(cmd::SetRootConstants{index, offset, {value}});
}

void CommandList::SetComputeRoot32BitConstants(UINT, UINT, const void*, UINT) {}

void CommandList::SetGraphicsRoot32BitConstants(UINT index, UINT count, const void* values,
                                                UINT offset) {
    const auto* words = static_cast<const std::uint32_t*>(values);
    record(cmd::SetRootConstants{index, offset, std::vector<std::uint32_t>(words, words + count)});
}

void CommandList::SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}

void CommandList::SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    record(cmd::SetRootView{index, address});
}

void CommandList::SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}

void CommandList::SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    record(cmd::SetRootView{index, address});
}

void CommandList::SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}

void CommandList::SetGraphicsRootUnorderedAccessView(UINT index,
                                                     D3D12_GPU_VIRTUAL_ADDRESS address) {
    record(cmd::SetRootView{index, address});
}

void CommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
    record(cmd::SetIndexBuffer{view != nullptr ? *view : D3D12_INDEX_BUFFER_VIEW{}});
}

void CommandList::IASetVertexBuffers(UINT start, UINT count,
                                     const D3D12_VERTEX_BUFFER_VIEW* views) {
    cmd::SetVertexBuffers command{start, {}};
    for (UINT i = 0; i < count; ++i) {
        command.views.push_back(views != nullptr ? views[i] : D3D12_VERTEX_BUFFER_VIEW{});
    }
    record(std::move(command));
}

void CommandList::SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) {
    unsupported("SOSetTargets");
}

void CommandList::OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* colors,
                                     BOOL single_handle_range,
                                     const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    cmd::SetRenderTargets command;
    for (UINT i = 0; i < count; ++i) {
        const Descriptor* descriptor = single_handle_range
            ? descriptor_at(colors[0]) + i
            : descriptor_at(colors[i]);
        command.colors.push_back(*descriptor);
    }
    if (depth != nullptr) command.depth = *descriptor_at(*depth);
    record(std::move(command));
}

void CommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags,
                                        FLOAT depth, UINT8 stencil, UINT rect_count,
                                        const D3D12_RECT* rects) {
    cmd::ClearDepthStencil command{*descriptor_at(view), flags, depth, stencil, {}};
    if (rects != nullptr) command.rects.assign(rects, rects + rect_count);
    record(std::move(command));
}

void CommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4],
                                        UINT rect_count, const D3D12_RECT* rects) {
    cmd::ClearRenderTarget command{*descriptor_at(view), {color[0], color[1], color[2], color[3]},
                                   {}};
    if (rects != nullptr) command.rects.assign(rects, rects + rect_count);
    record(std::move(command));
}

void CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE,
                                               D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*,
                                               const UINT[4], UINT, const D3D12_RECT*) {
    unsupported("ClearUnorderedAccessViewUint");
}

void CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE,
                                                D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*,
                                                const FLOAT[4], UINT, const D3D12_RECT*) {
    unsupported("ClearUnorderedAccessViewFloat");
}

void CommandList::DiscardResource(ID3D12Resource* resource, const D3D12_DISCARD_REGION*) {
    record(cmd::Discard{static_cast<Resource*>(resource)});
}

void CommandList::ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT first, UINT count,
                                   ID3D12Resource* dst, UINT64 dst_offset) {
    record(cmd::ResolveQueries{first, count, static_cast<Resource*>(dst), dst_offset});
}

void CommandList::ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                                  ID3D12Resource*, UINT64) {
    unsupported("ExecuteIndirect");
}

}  // namespace galaxy::d3d12
