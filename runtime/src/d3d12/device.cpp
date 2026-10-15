// SPDX-License-Identifier: GPL-3.0-only
// The D3D12 device and its objects on Apple/Linux hosts. See layer.h.

#include "layer.h"

#include "galaxy/win32_fs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace galaxy::d3d12 {

// ---------------------------------------------------------------------------
// DeviceChild
// ---------------------------------------------------------------------------
template <typename Interface, typename... Ancestors>
DeviceChild<Interface, Ancestors...>::DeviceChild(Device* device) : device_(device) {
    if (device_ != nullptr) device_->AddRef();
}
template <typename Interface, typename... Ancestors>
DeviceChild<Interface, Ancestors...>::~DeviceChild() {
    if (device_ != nullptr) device_->Release();
}
template <typename Interface, typename... Ancestors>
HRESULT DeviceChild<Interface, Ancestors...>::GetDevice(REFIID riid, void** out) {
    if (device_ == nullptr) return E_FAIL;
    return device_->QueryInterface(riid, out);
}

template class DeviceChild<ID3D12Resource, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object>;
template class DeviceChild<ID3D12Heap, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object>;
template class DeviceChild<ID3D12DescriptorHeap, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object>;
template class DeviceChild<ID3D12RootSignature, ID3D12DeviceChild, ID3D12Object>;
template class DeviceChild<ID3D12PipelineState, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object>;
template class DeviceChild<ID3D12CommandAllocator, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object>;
template class DeviceChild<ID3D12GraphicsCommandList, ID3D12CommandList, ID3D12DeviceChild,
                           ID3D12Object>;
template class DeviceChild<ID3D12Fence, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object>;
template class DeviceChild<ID3D12QueryHeap, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object>;
template class DeviceChild<ID3D12CommandQueue, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object>;

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------
FormatInfo format_info(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT: return {16};
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT: return {12};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT: return {8};
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return {8, 1, 1, true, true};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT: return {4};
    case DXGI_FORMAT_D32_FLOAT: return {4, 1, 1, true, false};
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return {4, 1, 1, true, true};
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM: return {2};
    case DXGI_FORMAT_D16_UNORM: return {2, 1, 1, true, false};
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM: return {1};
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM: return {8, 4, 4};
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB: return {16, 4, 4};
    default: return {};
    }
}

DXGI_FORMAT Descriptor::format() const noexcept {
    if (has_view_desc) {
        switch (kind) {
        case DescriptorKind::Srv: return view.srv.Format;
        case DescriptorKind::Uav: return view.uav.Format;
        case DescriptorKind::Rtv: return view.rtv.Format;
        case DescriptorKind::Dsv: return view.dsv.Format;
        default: break;
        }
    }
    return resource != nullptr ? resource->desc().Format : DXGI_FORMAT_UNKNOWN;
}

UINT Descriptor::mip_slice() const noexcept {
    if (!has_view_desc) return 0u;
    if (kind == DescriptorKind::Rtv &&
        view.rtv.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D) {
        return view.rtv.Texture2D.MipSlice;
    }
    if (kind == DescriptorKind::Dsv &&
        view.dsv.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D) {
        return view.dsv.Texture2D.MipSlice;
    }
    return 0u;
}

// ---------------------------------------------------------------------------
// Footprints
// ---------------------------------------------------------------------------
namespace {
constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}
UINT mip_count(const D3D12_RESOURCE_DESC& desc) noexcept {
    return desc.MipLevels == 0u ? 1u : desc.MipLevels;
}
UINT array_count(const D3D12_RESOURCE_DESC& desc) noexcept {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u
           : desc.DepthOrArraySize == 0u                       ? 1u
                                                               : desc.DepthOrArraySize;
}
}  // namespace

SubresourceFootprint subresource_footprint(const D3D12_RESOURCE_DESC& desc,
                                           UINT subresource) noexcept {
    const UINT mips = mip_count(desc);
    const UINT mip = subresource % mips;
    const FormatInfo info = format_info(desc.Format);
    const std::uint32_t width = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(desc.Width >> mip));
    const std::uint32_t height = std::max<std::uint32_t>(1u, desc.Height >> mip);
    const std::uint32_t depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
        ? std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(desc.DepthOrArraySize) >> mip)
        : 1u;
    const std::uint32_t blocks_wide = (width + info.block_width - 1u) / info.block_width;
    const std::uint32_t rows = (height + info.block_height - 1u) / info.block_height;
    const std::uint32_t row_bytes = blocks_wide * std::max<std::uint32_t>(info.bytes_per_block, 1u);
    return {width, height, depth,
            static_cast<std::uint32_t>(align_up(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)),
            rows};
}

void Device::GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT first, UINT count,
                                   UINT64 base_offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts,
                                   UINT* rows, UINT64* row_bytes, UINT64* total_bytes) {
    if (desc == nullptr) return;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (layouts != nullptr && count > 0u) {
            layouts[0].Offset = base_offset;
            layouts[0].Footprint = {DXGI_FORMAT_UNKNOWN, static_cast<UINT>(desc->Width), 1u, 1u,
                                    static_cast<UINT>(align_up(desc->Width, 256u))};
        }
        if (rows != nullptr && count > 0u) rows[0] = 1u;
        if (row_bytes != nullptr && count > 0u) row_bytes[0] = desc->Width;
        if (total_bytes != nullptr) *total_bytes = desc->Width;
        return;
    }
    const FormatInfo info = format_info(desc->Format);
    std::uint64_t offset = base_offset;
    std::uint64_t total = 0u;
    for (UINT i = 0; i < count; ++i) {
        const SubresourceFootprint fp = subresource_footprint(*desc, first + i);
        offset = align_up(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        const std::uint32_t blocks_wide = (fp.width + info.block_width - 1u) / info.block_width;
        if (layouts != nullptr) {
            layouts[i].Offset = offset;
            layouts[i].Footprint = {desc->Format, fp.width, fp.height, fp.depth, fp.row_pitch};
        }
        if (rows != nullptr) rows[i] = fp.rows;
        if (row_bytes != nullptr) {
            row_bytes[i] = static_cast<UINT64>(blocks_wide) * std::max<std::uint32_t>(info.bytes_per_block, 1u);
        }
        const std::uint64_t size = static_cast<std::uint64_t>(fp.row_pitch) * fp.rows * fp.depth;
        total = offset + size - base_offset;
        offset += size;
    }
    if (total_bytes != nullptr) *total_bytes = total;
}

D3D12_RESOURCE_ALLOCATION_INFO Device::GetResourceAllocationInfo(UINT, UINT count,
                                                                 const D3D12_RESOURCE_DESC* descs) {
    D3D12_RESOURCE_ALLOCATION_INFO result{0u, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
    std::uint64_t size = 0u;
    for (UINT i = 0; i < count; ++i) {
        const D3D12_RESOURCE_DESC& desc = descs[i];
        std::uint64_t bytes = 0u;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            bytes = desc.Width;
        } else {
            const UINT subresources = mip_count(desc) * array_count(desc);
            GetCopyableFootprints(&desc, 0u, subresources, 0u, nullptr, nullptr, nullptr, &bytes);
        }
        const bool small = desc.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
                           desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
                           align_up(bytes, D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) <=
                               D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        const std::uint64_t alignment = small ? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT
                                              : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        if (count == 1u) result.Alignment = alignment;
        size = align_up(size, alignment) + align_up(bytes, alignment);
    }
    result.SizeInBytes = size;
    return result;
}

D3D12_HEAP_PROPERTIES Device::GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    properties.CPUPageProperty = type == D3D12_HEAP_TYPE_DEFAULT
        ? D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE
        : D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;  // unified memory
    return properties;
}

// ---------------------------------------------------------------------------
// Address space
// ---------------------------------------------------------------------------
AddressSpace& AddressSpace::instance() {
    static AddressSpace* space = new AddressSpace;  // outlives every resource
    return *space;
}

std::uint64_t AddressSpace::reserve(Resource* resource, std::uint64_t size) {
    const std::unique_lock lock(mutex_);
    const std::uint64_t base = next_;
    next_ = align_up(next_ + std::max<std::uint64_t>(size, 1u), 64u * 1024u);
    ranges_.emplace(base, std::make_pair(resource, size));
    return base;
}

void AddressSpace::release(std::uint64_t base) noexcept {
    const std::unique_lock lock(mutex_);
    ranges_.erase(base);
}

std::pair<Resource*, std::uint64_t> AddressSpace::find(std::uint64_t address) const noexcept {
    const std::shared_lock lock(mutex_);
    auto it = ranges_.upper_bound(address);
    if (it == ranges_.begin()) return {nullptr, 0u};
    --it;
    const std::uint64_t offset = address - it->first;
    if (offset >= std::max<std::uint64_t>(it->second.second, 1u)) return {nullptr, 0u};
    return {it->second.first, offset};
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
Resource::Resource(Device* device, const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap_type,
                   D3D12_RESOURCE_STATES, std::unique_ptr<BackendResource> storage,
                   std::uint64_t)
    : DeviceChild(device), desc_(desc), heap_type_(heap_type), storage_(std::move(storage)),
      gpu_address_(0u) {
    if (is_buffer()) gpu_address_ = AddressSpace::instance().reserve(this, desc_.Width);
}

Resource::~Resource() {
    if (gpu_address_ != 0u) AddressSpace::instance().release(gpu_address_);
}

HRESULT Resource::Map(UINT subresource, const D3D12_RANGE*, void** data) {
    if (!is_buffer() || subresource != 0u || storage_ == nullptr) return E_INVALIDARG;
    if (data != nullptr) *data = storage_->cpu();
    return storage_->cpu() != nullptr ? S_OK : E_FAIL;
}

void Resource::Unmap(UINT, const D3D12_RANGE*) {}

HRESULT Resource::GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) {
    if (properties != nullptr) {
        *properties = {};
        properties->Type = heap_type_;
        properties->CreationNodeMask = 1u;
        properties->VisibleNodeMask = 1u;
    }
    if (flags != nullptr) *flags = D3D12_HEAP_FLAG_NONE;
    return S_OK;
}

HRESULT Device::create_resource(const D3D12_HEAP_PROPERTIES& heap, const D3D12_RESOURCE_DESC& desc,
                                D3D12_RESOURCE_STATES state, REFIID riid, void** out) {
    std::unique_ptr<BackendResource> storage;
    try {
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            storage = backend_->create_buffer(desc.Width);
        } else {
            if (format_info(desc.Format).bytes_per_block == 0u) {
                std::cerr << "[d3d12] unsupported texture format " << desc.Format << '\n';
                return E_INVALIDARG;
            }
            TextureInfo info{desc,
                             (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0,
                             (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0};
            storage = backend_->create_texture(info);
        }
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    if (storage == nullptr) return E_OUTOFMEMORY;
    D3D12_HEAP_TYPE type = heap.Type;
    if (type == D3D12_HEAP_TYPE_CUSTOM) {
        type = heap.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE ? D3D12_HEAP_TYPE_DEFAULT
                                                                           : D3D12_HEAP_TYPE_UPLOAD;
    }
    auto* resource = new Resource(this, desc, type, state, std::move(storage), 0u);
    return hand_out(resource, riid, out);
}

HRESULT Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS,
                                        const D3D12_RESOURCE_DESC* desc,
                                        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE*,
                                        REFIID riid, void** resource) {
    if (heap == nullptr || desc == nullptr) return E_INVALIDARG;
    return create_resource(*heap, *desc, state, riid, resource);
}

HRESULT Device::CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid, void** heap) {
    if (desc == nullptr) return E_INVALIDARG;
    return hand_out(new Heap(this, *desc), riid, heap);
}

HRESULT Device::CreatePlacedResource(ID3D12Heap* heap, UINT64, const D3D12_RESOURCE_DESC* desc,
                                     D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE*,
                                     REFIID riid, void** resource) {
    if (heap == nullptr || desc == nullptr) return E_INVALIDARG;
    const D3D12_HEAP_DESC heap_desc = heap->GetDesc();
    return create_resource(heap_desc.Properties, *desc, state, riid, resource);
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------
DescriptorHeap::DescriptorHeap(Device* device, const D3D12_DESCRIPTOR_HEAP_DESC& desc)
    : DeviceChild(device), desc_(desc), slots_(std::max<UINT>(desc.NumDescriptors, 1u)) {}

HRESULT Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid,
                                     void** heap) {
    if (desc == nullptr) return E_INVALIDARG;
    try {
        return hand_out(new DescriptorHeap(this, *desc), riid, heap);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

namespace {
Resource* as_resource(ID3D12Resource* resource) noexcept {
    return static_cast<Resource*>(resource);
}
}  // namespace

void Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    Descriptor descriptor;
    descriptor.kind = DescriptorKind::Cbv;
    if (desc != nullptr) {
        descriptor.has_view_desc = true;
        descriptor.view.cbv = *desc;
        descriptor.resource = AddressSpace::instance().find(desc->BufferLocation).first;
    }
    *descriptor_at(dest) = descriptor;
}

void Device::CreateShaderResourceView(ID3D12Resource* resource,
                                      const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    Descriptor descriptor;
    descriptor.kind = DescriptorKind::Srv;
    descriptor.resource = as_resource(resource);
    if (desc != nullptr) {
        descriptor.has_view_desc = true;
        descriptor.view.srv = *desc;
    }
    *descriptor_at(dest) = descriptor;
}

void Device::CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource*,
                                       const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    Descriptor descriptor;
    descriptor.kind = DescriptorKind::Uav;
    descriptor.resource = as_resource(resource);
    if (desc != nullptr) {
        descriptor.has_view_desc = true;
        descriptor.view.uav = *desc;
    }
    *descriptor_at(dest) = descriptor;
}

void Device::CreateRenderTargetView(ID3D12Resource* resource,
                                    const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    Descriptor descriptor;
    descriptor.kind = DescriptorKind::Rtv;
    descriptor.resource = as_resource(resource);
    if (desc != nullptr) {
        descriptor.has_view_desc = true;
        descriptor.view.rtv = *desc;
    }
    *descriptor_at(dest) = descriptor;
}

void Device::CreateDepthStencilView(ID3D12Resource* resource,
                                    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    Descriptor descriptor;
    descriptor.kind = DescriptorKind::Dsv;
    descriptor.resource = as_resource(resource);
    if (desc != nullptr) {
        descriptor.has_view_desc = true;
        descriptor.view.dsv = *desc;
    }
    *descriptor_at(dest) = descriptor;
}

void Device::CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    Descriptor descriptor;
    descriptor.kind = DescriptorKind::Sampler;
    if (desc != nullptr) {
        descriptor.has_view_desc = true;
        descriptor.view.sampler = *desc;
    }
    *descriptor_at(dest) = descriptor;
}

void Device::CopyDescriptors(UINT dest_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts,
                             const UINT* dest_sizes, UINT src_ranges,
                             const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                             const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE) {
    UINT dest_range = 0u, dest_index = 0u;
    for (UINT src_range = 0u; src_range < src_ranges; ++src_range) {
        const UINT src_count = src_sizes != nullptr ? src_sizes[src_range] : 1u;
        for (UINT i = 0u; i < src_count; ++i) {
            while (dest_range < dest_ranges &&
                   dest_index >= (dest_sizes != nullptr ? dest_sizes[dest_range] : 1u)) {
                ++dest_range;
                dest_index = 0u;
            }
            if (dest_range >= dest_ranges) return;
            descriptor_at(dest_starts[dest_range])[dest_index] = descriptor_at(src_starts[src_range])[i];
            ++dest_index;
        }
    }
}

void Device::CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                   D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE) {
    std::memmove(descriptor_at(dest), descriptor_at(src), sizeof(Descriptor) * count);
}

// ---------------------------------------------------------------------------
// Root signatures, pipelines, command objects
// ---------------------------------------------------------------------------
HRESULT Device::CreateRootSignature(UINT, const void* blob, SIZE_T size, REFIID riid,
                                    void** signature) {
    RootSignatureLayout layout;
    if (blob == nullptr ||
        !deserialize_root_signature({static_cast<const std::uint8_t*>(blob), size}, layout)) {
        return E_INVALIDARG;
    }
    return hand_out(new RootSignature(this, std::move(layout)), riid, signature);
}

HRESULT PipelineState::GetCachedBlob(ID3DBlob** blob) {
    if (blob != nullptr) *blob = nullptr;
    return E_NOTIMPL;
}

HRESULT Device::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                            REFIID riid, void** pipeline) {
    if (desc == nullptr) return E_INVALIDARG;
    GraphicsPipelineInfo info;
    const auto load = [](const D3D12_SHADER_BYTECODE& code, std::optional<ShaderProgram>& out) {
        if (code.pShaderBytecode == nullptr || code.BytecodeLength == 0u) return true;
        ShaderProgram program;
        if (!decode_shader(code.pShaderBytecode, code.BytecodeLength, program)) return false;
        out = std::move(program);
        return true;
    };
    std::optional<ShaderProgram> geometry;
    if (!load(desc->VS, info.vs) || !load(desc->PS, info.ps) || !load(desc->GS, geometry)) {
        std::cerr << "[d3d12] pipeline shader is not a D3DCompile blob\n";
        return E_INVALIDARG;
    }
    info.has_geometry_shader = geometry.has_value();
    info.blend = desc->BlendState;
    info.sample_mask = desc->SampleMask;
    info.rasterizer = desc->RasterizerState;
    info.depth_stencil = desc->DepthStencilState;
    for (UINT i = 0; i < desc->InputLayout.NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& e = desc->InputLayout.pInputElementDescs[i];
        info.input_layout.push_back({e.SemanticName != nullptr ? e.SemanticName : "",
                                     e.SemanticIndex, e.Format, e.InputSlot,
                                     e.AlignedByteOffset, e.InputSlotClass,
                                     e.InstanceDataStepRate});
    }
    // Resolve D3D12_APPEND_ALIGNED_ELEMENT offsets per slot.
    std::array<UINT, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> next_offset{};
    for (InputElement& e : info.input_layout) {
        if (e.input_slot >= next_offset.size()) return E_INVALIDARG;
        if (e.aligned_byte_offset == D3D12_APPEND_ALIGNED_ELEMENT) {
            e.aligned_byte_offset = next_offset[e.input_slot];
        }
        next_offset[e.input_slot] = e.aligned_byte_offset + format_info(e.format).bytes_per_block;
    }
    info.strip_cut = desc->IBStripCutValue;
    info.topology_type = desc->PrimitiveTopologyType;
    info.num_render_targets = desc->NumRenderTargets;
    for (UINT i = 0; i < 8u; ++i) info.rtv_formats[i] = desc->RTVFormats[i];
    info.dsv_format = desc->DSVFormat;
    info.sample = desc->SampleDesc;
    std::string error;
    std::unique_ptr<BackendPipeline> compiled = backend_->create_pipeline(info, error);
    if (compiled == nullptr) {
        std::cerr << "[d3d12] pipeline creation failed: " << error << '\n';
        return E_INVALIDARG;
    }
    return hand_out(new PipelineState(this, std::move(info), std::move(compiled)), riid,
                    pipeline);
}

HRESULT Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid,
                                   void** queue) {
    if (desc == nullptr) return E_INVALIDARG;
    return hand_out(new CommandQueue(this, *desc), riid, queue);
}

HRESULT Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID riid, void** allocator) {
    return hand_out(new CommandAllocator(this), riid, allocator);
}

HRESULT Device::CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator*,
                                  ID3D12PipelineState* initial, REFIID riid, void** list) {
    return hand_out(new CommandList(this, type, static_cast<PipelineState*>(initial)), riid, list);
}

HRESULT Device::CreateFence(UINT64 initial, D3D12_FENCE_FLAGS, REFIID riid, void** fence) {
    return hand_out(new Fence(this, initial), riid, fence);
}

HRESULT Device::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** heap) {
    if (desc == nullptr) return E_INVALIDARG;
    return hand_out(new QueryHeap(this, *desc), riid, heap);
}

HRESULT Device::CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size) {
    if (data == nullptr) return E_INVALIDARG;
    switch (feature) {
    case D3D12_FEATURE_FEATURE_LEVELS: {
        if (size != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) return E_INVALIDARG;
        auto* levels = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(data);
        levels->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        for (UINT i = 0; i < levels->NumFeatureLevels; ++i) {
            if (levels->pFeatureLevelsRequested[i] <= D3D_FEATURE_LEVEL_12_0) {
                levels->MaxSupportedFeatureLevel =
                    std::max(levels->MaxSupportedFeatureLevel, levels->pFeatureLevelsRequested[i]);
            }
        }
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
        if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) return E_INVALIDARG;
        *static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5*>(data) = {};
        return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
        if (size != sizeof(D3D12_FEATURE_DATA_SHADER_CACHE)) return E_INVALIDARG;
        static_cast<D3D12_FEATURE_DATA_SHADER_CACHE*>(data)->SupportFlags =
            D3D12_SHADER_CACHE_SUPPORT_NONE;
        return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
        if (size != sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) return E_INVALIDARG;
        auto* options = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS*>(data);
        *options = {};
        options->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_2;
        options->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

// ---------------------------------------------------------------------------
// Fences and queues
// ---------------------------------------------------------------------------
UINT64 Fence::GetCompletedValue() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return value_;
}

HRESULT Fence::SetEventOnCompletion(UINT64 value, HANDLE event) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (value_ >= value) {
        lock.unlock();
        if (event != nullptr) SetEvent(event);
        return S_OK;
    }
    if (event == nullptr) {
        // D3D12: a null event blocks until the value is reached.
        changed_.wait(lock, [&] { return value_ >= value; });
        return S_OK;
    }
    waiters_.emplace_back(value, event);
    return S_OK;
}

HRESULT Fence::Signal(UINT64 value) {
    complete(value);
    return S_OK;
}

void Fence::complete(UINT64 value) {
    std::vector<HANDLE> ready;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        value_ = value;  // D3D12 fences may be set lower by the CPU
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            if (it->first <= value_) {
                ready.push_back(it->second);
                it = waiters_.erase(it);
            } else {
                ++it;
            }
        }
    }
    changed_.notify_all();
    for (HANDLE event : ready) SetEvent(event);
}

void Fence::wait(UINT64 value) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return value_ >= value; });
}

void CommandQueue::ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) {
    std::vector<CommandList*> closed;
    closed.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        auto* list = static_cast<CommandList*>(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
        if (list != nullptr && list->closed()) closed.push_back(list);
    }
    if (!closed.empty()) device()->backend().execute(closed, {});
}

HRESULT CommandQueue::Signal(ID3D12Fence* fence, UINT64 value) {
    auto* target = static_cast<Fence*>(fence);
    if (target == nullptr) return E_INVALIDARG;
    target->AddRef();
    device()->backend().after_pending_work([target, value] {
        target->complete(value);
        target->Release();
    });
    return S_OK;
}

HRESULT CommandQueue::Wait(ID3D12Fence* fence, UINT64 value) {
    auto* target = static_cast<Fence*>(fence);
    if (target == nullptr) return E_INVALIDARG;
    target->wait(value);
    return S_OK;
}

HRESULT CommandQueue::GetTimestampFrequency(UINT64* frequency) {
    if (frequency == nullptr) return E_INVALIDARG;
    *frequency = 1'000'000'000ull;  // ResolveQueryData writes nanoseconds
    return S_OK;
}

HRESULT CommandQueue::GetClockCalibration(UINT64* gpu, UINT64* cpu) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    if (gpu != nullptr) {
        *gpu = static_cast<UINT64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }
    if (cpu != nullptr) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        *cpu = static_cast<UINT64>(counter.QuadPart);
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
Device::Device(std::unique_ptr<Backend> backend) : backend_(std::move(backend)) {}

Device::~Device() {
    // Every queued completion must run before the backend goes away.
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    backend_->after_pending_work([&] {
        const std::lock_guard<std::mutex> lock(mutex);
        finished = true;
        done.notify_all();
    });
    std::unique_lock<std::mutex> lock(mutex);
    done.wait(lock, [&] { return finished; });
}

}  // namespace galaxy::d3d12

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
HRESULT WINAPI D3D12CreateDevice(IUnknown*, D3D_FEATURE_LEVEL minimum, REFIID riid, void** device) {
    if (minimum > D3D_FEATURE_LEVEL_12_0) return E_FAIL;
    std::unique_ptr<galaxy::d3d12::Backend> backend = galaxy::d3d12::create_backend();
    if (backend == nullptr) return E_FAIL;
    if (device == nullptr) return S_FALSE;  // capability check only
    return galaxy::d3d12::hand_out(new galaxy::d3d12::Device(std::move(backend)), riid, device);
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID, void** debug) {
    if (debug != nullptr) *debug = nullptr;
    return E_NOINTERFACE;
}
