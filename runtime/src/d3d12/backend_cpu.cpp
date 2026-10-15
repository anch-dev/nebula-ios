// SPDX-License-Identifier: GPL-3.0-only
// The CPU backend of the D3D12 layer: resources are host memory and recorded
// copies, clears, query resolves and presents run immediately on the CPU;
// draws are counted but not rasterized. It lets the layer and the
// renderer's non-pixel paths (uploads, readbacks, fences, frame pacing) run
// where Metal does not exist, such as Linux CI. See layer.h.

#include "layer.h"

#ifndef GALAXY_METAL_BACKEND
#define GALAXY_METAL_BACKEND 0
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace galaxy::d3d12 {
namespace {

class CpuMemory final : public BackendResource {
public:
    explicit CpuMemory(std::size_t size) : bytes_(std::max<std::size_t>(size, 1u)) {}
    std::uint8_t* cpu() noexcept override { return bytes_.data(); }
    std::vector<std::uint8_t>& bytes() noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

// A texture: every subresource laid out as GetCopyableFootprints would.
class CpuTexture final : public BackendResource {
public:
    explicit CpuTexture(const D3D12_RESOURCE_DESC& desc) : desc_(desc) {
        const UINT mips = desc.MipLevels == 0u ? 1u : desc.MipLevels;
        const UINT layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                ? 1u
                                : std::max<UINT>(desc.DepthOrArraySize, 1u);
        std::size_t offset = 0u;
        for (UINT i = 0; i < mips * layers; ++i) {
            const SubresourceFootprint fp = subresource_footprint(desc, i);
            offsets_.push_back(offset);
            footprints_.push_back(fp);
            offset += static_cast<std::size_t>(fp.row_pitch) * fp.rows * fp.depth;
            offset = (offset + 511u) & ~std::size_t{511u};
        }
        bytes_.resize(std::max<std::size_t>(offset, 1u));
    }
    std::uint8_t* cpu() noexcept override { return nullptr; }
    [[nodiscard]] std::uint8_t* subresource(UINT index) noexcept {
        return index < offsets_.size() ? bytes_.data() + offsets_[index] : nullptr;
    }
    [[nodiscard]] const SubresourceFootprint* footprint(UINT index) const noexcept {
        return index < footprints_.size() ? &footprints_[index] : nullptr;
    }
    [[nodiscard]] const D3D12_RESOURCE_DESC& desc() const noexcept { return desc_; }
    std::vector<std::uint8_t>& bytes() noexcept { return bytes_; }

private:
    D3D12_RESOURCE_DESC desc_;
    std::vector<std::size_t> offsets_;
    std::vector<SubresourceFootprint> footprints_;
    std::vector<std::uint8_t> bytes_;
};

class CpuPipeline final : public BackendPipeline {};

CpuTexture* texture_of(Resource* resource) noexcept {
    return resource != nullptr && !resource->is_buffer()
               ? static_cast<CpuTexture*>(resource->storage())
               : nullptr;
}

// A 2D region of texels in some memory, with its row pitch.
struct Region {
    std::uint8_t* base = nullptr;
    std::uint32_t row_pitch = 0;
    std::uint32_t slice_pitch = 0;
};

// The bytes of one texel of `format` for a clear color.
void encode_color(DXGI_FORMAT format, const std::array<float, 4>& color, std::uint8_t* out,
                  std::uint32_t size) {
    std::memset(out, 0, size);
    const auto unorm8 = [](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        for (unsigned i = 0; i < 4u; ++i) out[i] = unorm8(color[i]);
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        out[0] = unorm8(color[2]);
        out[1] = unorm8(color[1]);
        out[2] = unorm8(color[0]);
        out[3] = unorm8(color[3]);
        break;
    case DXGI_FORMAT_R8_UNORM: out[0] = unorm8(color[0]); break;
    case DXGI_FORMAT_R8G8_UNORM: out[0] = unorm8(color[0]); out[1] = unorm8(color[1]); break;
    case DXGI_FORMAT_R32_FLOAT: std::memcpy(out, &color[0], 4); break;
    case DXGI_FORMAT_R32G32_FLOAT: std::memcpy(out, color.data(), 8); break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: std::memcpy(out, color.data(), 16); break;
    default: break;  // other formats clear to zero
    }
}

class CpuBackend final : public Backend {
public:
    const char* name() const noexcept override { return "cpu"; }
    std::wstring adapter_description() const override { return L"Nebula CPU (no rasterizer)"; }
    std::uint64_t video_memory_bytes() const noexcept override { return 4ull << 30u; }

    std::unique_ptr<BackendResource> create_buffer(std::uint64_t size) override {
        return std::make_unique<CpuMemory>(static_cast<std::size_t>(size));
    }
    std::unique_ptr<BackendResource> create_texture(const TextureInfo& info) override {
        return std::make_unique<CpuTexture>(info.desc);
    }
    std::unique_ptr<BackendPipeline> create_pipeline(const GraphicsPipelineInfo&,
                                                     std::string&) override {
        return std::make_unique<CpuPipeline>();
    }

    void execute(std::span<CommandList* const> lists, std::function<void()> done) override {
        for (CommandList* list : lists) {
            for (const cmd::Command& command : list->commands()) run(command);
        }
        if (done) done();
    }
    void after_pending_work(std::function<void()> done) override {
        if (done) done();
    }
    void present(Resource*, UINT) override { ++presents_; }

private:
    void run(const cmd::Command& command) {
        std::visit([this](const auto& c) { apply(c); }, command);
    }

    template <typename T>
    void apply(const T&) {}

    void apply(const cmd::Draw&) { ++draws_; }

    void apply(const cmd::CopyBufferRegion& c) {
        if (c.dst == nullptr || c.src == nullptr || !c.dst->is_buffer() || !c.src->is_buffer()) return;
        std::memmove(c.dst->storage()->cpu() + c.dst_offset, c.src->storage()->cpu() + c.src_offset,
                     static_cast<std::size_t>(c.bytes));
    }

    void apply(const cmd::CopyResource& c) {
        if (c.dst == nullptr || c.src == nullptr) return;
        if (c.dst->is_buffer() && c.src->is_buffer()) {
            std::memcpy(c.dst->storage()->cpu(), c.src->storage()->cpu(),
                        static_cast<std::size_t>(std::min(c.dst->desc().Width, c.src->desc().Width)));
            return;
        }
        CpuTexture* dst = texture_of(c.dst);
        CpuTexture* src = texture_of(c.src);
        if (dst != nullptr && src != nullptr && dst->bytes().size() == src->bytes().size()) {
            std::memcpy(dst->bytes().data(), src->bytes().data(), dst->bytes().size());
        }
    }

    // One side of a texture copy: its memory, pitch and format.
    bool locate(const D3D12_TEXTURE_COPY_LOCATION& location, Region& region, DXGI_FORMAT& format,
                std::uint32_t& width, std::uint32_t& height, std::uint32_t& depth) {
        auto* resource = static_cast<Resource*>(location.pResource);
        if (resource == nullptr) return false;
        if (location.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            if (!resource->is_buffer()) return false;
            const auto& fp = location.PlacedFootprint;
            const FormatInfo info = format_info(fp.Footprint.Format);
            region.base = resource->storage()->cpu() + fp.Offset;
            region.row_pitch = fp.Footprint.RowPitch;
            region.slice_pitch = fp.Footprint.RowPitch *
                                 ((fp.Footprint.Height + info.block_height - 1u) / info.block_height);
            format = fp.Footprint.Format;
            width = fp.Footprint.Width;
            height = fp.Footprint.Height;
            depth = fp.Footprint.Depth;
            return true;
        }
        CpuTexture* texture = texture_of(resource);
        if (texture == nullptr) return false;
        const SubresourceFootprint* fp = texture->footprint(location.SubresourceIndex);
        if (fp == nullptr) return false;
        region.base = texture->subresource(location.SubresourceIndex);
        region.row_pitch = fp->row_pitch;
        region.slice_pitch = fp->row_pitch * fp->rows;
        format = texture->desc().Format;
        width = fp->width;
        height = fp->height;
        depth = fp->depth;
        return true;
    }

    void apply(const cmd::CopyTextureRegion& c) {
        Region dst, src;
        DXGI_FORMAT dst_format{}, src_format{};
        std::uint32_t dw = 0, dh = 0, dd = 0, sw = 0, sh = 0, sd = 0;
        if (!locate(c.dst, dst, dst_format, dw, dh, dd) || !locate(c.src, src, src_format, sw, sh, sd)) {
            return;
        }
        const FormatInfo info = format_info(src_format);
        if (info.bytes_per_block == 0u) return;
        D3D12_BOX box{0u, 0u, 0u, sw, sh, sd};
        if (c.src_box) box = *c.src_box;
        const std::uint32_t bw = info.block_width, bh = info.block_height;
        const std::uint32_t row_bytes = ((box.right - box.left + bw - 1u) / bw) * info.bytes_per_block;
        const std::uint32_t rows = (box.bottom - box.top + bh - 1u) / bh;
        for (std::uint32_t z = 0; z < box.back - box.front; ++z) {
            for (std::uint32_t row = 0; row < rows; ++row) {
                const std::uint8_t* from = src.base + (box.front + z) * src.slice_pitch +
                                           (box.top / bh + row) * src.row_pitch +
                                           (box.left / bw) * info.bytes_per_block;
                std::uint8_t* to = dst.base + (c.dst_z + z) * dst.slice_pitch +
                                   (c.dst_y / bh + row) * dst.row_pitch +
                                   (c.dst_x / bw) * info.bytes_per_block;
                std::memmove(to, from, row_bytes);
            }
        }
    }

    void apply(const cmd::ClearRenderTarget& c) {
        CpuTexture* texture = texture_of(c.target.resource);
        if (texture == nullptr) return;
        const UINT mip = c.target.mip_slice();
        const SubresourceFootprint* fp = texture->footprint(mip);
        if (fp == nullptr) return;
        const FormatInfo info = format_info(texture->desc().Format);
        std::uint8_t texel[16];
        encode_color(c.target.format(), c.color, texel, info.bytes_per_block);
        fill(texture->subresource(mip), *fp, info, texel, c.rects);
    }

    void apply(const cmd::ClearDepthStencil& c) {
        CpuTexture* texture = texture_of(c.target.resource);
        if (texture == nullptr) return;
        const UINT mip = c.target.mip_slice();
        const SubresourceFootprint* fp = texture->footprint(mip);
        if (fp == nullptr) return;
        const FormatInfo info = format_info(texture->desc().Format);
        std::uint8_t texel[8]{};
        if (info.bytes_per_block == 4u && info.stencil) {
            const auto z24 = static_cast<std::uint32_t>(std::lround(std::clamp(c.depth, 0.0f, 1.0f) * 16777215.0f));
            const std::uint32_t value = z24 | (std::uint32_t{c.stencil} << 24u);
            std::memcpy(texel, &value, 4);
        } else if (info.bytes_per_block == 4u) {
            std::memcpy(texel, &c.depth, 4);
        }
        fill(texture->subresource(mip), *fp, info, texel, c.rects);
    }

    static void fill(std::uint8_t* base, const SubresourceFootprint& fp, const FormatInfo& info,
                     const std::uint8_t* texel, const std::vector<D3D12_RECT>& rects) {
        std::vector<D3D12_RECT> areas = rects;
        if (areas.empty()) {
            areas.push_back(D3D12_RECT{0, 0, static_cast<LONG>(fp.width), static_cast<LONG>(fp.height)});
        }
        for (const D3D12_RECT& r : areas) {
            const LONG x0 = std::clamp<LONG>(r.left, 0, static_cast<LONG>(fp.width));
            const LONG x1 = std::clamp<LONG>(r.right, 0, static_cast<LONG>(fp.width));
            const LONG y0 = std::clamp<LONG>(r.top, 0, static_cast<LONG>(fp.height));
            const LONG y1 = std::clamp<LONG>(r.bottom, 0, static_cast<LONG>(fp.height));
            for (LONG y = y0; y < y1; ++y) {
                std::uint8_t* row = base + static_cast<std::size_t>(y) * fp.row_pitch;
                for (LONG x = x0; x < x1; ++x) {
                    std::memcpy(row + static_cast<std::size_t>(x) * info.bytes_per_block, texel,
                                info.bytes_per_block);
                }
            }
        }
    }

    void apply(const cmd::ResolveQueries& c) {
        if (c.dst == nullptr || !c.dst->is_buffer()) return;
        std::memset(c.dst->storage()->cpu() + c.dst_offset, 0, std::size_t{c.count} * 8u);
    }

    std::uint64_t draws_ = 0;
    std::uint64_t presents_ = 0;
};

}  // namespace

std::unique_ptr<Backend> create_cpu_backend() { return std::make_unique<CpuBackend>(); }

#if !GALAXY_METAL_BACKEND
std::unique_ptr<Backend> create_backend() { return create_cpu_backend(); }
#endif

}  // namespace galaxy::d3d12
