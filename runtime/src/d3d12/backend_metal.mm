// SPDX-License-Identifier: GPL-3.0-only
// The Metal backend of the D3D12 layer (Apple hosts). See layer.h.
//
// Resources are MTLBuffers (shared storage: Apple GPUs share memory with the
// CPU, so every heap type is mappable) and MTLTextures. ExecuteCommandLists
// encodes the recorded commands into one MTLCommandBuffer right away:
// render-target bindings open render passes (load/store, so passes split
// freely around copies), copies use blit encoders, rectangle clears draw.
// Pipelines are generated per D3D12 PSO from the shaders' SPIR-V
// (shader_translate.h) and, per vertex stride, a Metal render pipeline.
// Presentation draws the swap-chain buffer into the app shell's
// CAMetalLayer.

// The Win32 headers and Objective-C both define BOOL (int vs bool), and the
// COM headers define `interface` (as struct), which would turn @interface
// into @struct. The Win32 BOOL is renamed for this file (it stays an int, so
// signatures are unchanged), and the COM macros are dropped before the
// Apple headers.
#define BOOL GALAXY_WIN32_BOOL
#include "layer.h"

#include "galaxy/gx/shader_translate.h"
#include "galaxy/host_platform.h"
#undef BOOL
#undef interface
#undef IN
#undef OUT
#undef CONST
#undef PURE
#undef THIS
#undef THIS_

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <unordered_map>

#ifndef GALAXY_METAL_BACKEND
#define GALAXY_METAL_BACKEND 1
#endif

namespace galaxy::d3d12 {
namespace {

namespace xlate = galaxy::gx::xlate;

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------
MTLPixelFormat pixel_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
    case DXGI_FORMAT_R8G8B8A8_UINT: return MTLPixelFormatRGBA8Uint;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return MTLPixelFormatRGBA8Snorm;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return MTLPixelFormatRGB10A2Unorm;
    case DXGI_FORMAT_R11G11B10_FLOAT: return MTLPixelFormatRG11B10Float;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return MTLPixelFormatRGBA16Float;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return MTLPixelFormatRGBA16Unorm;
    case DXGI_FORMAT_R16G16B16A16_UINT: return MTLPixelFormatRGBA16Uint;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return MTLPixelFormatRGBA32Float;
    case DXGI_FORMAT_R32G32B32A32_UINT: return MTLPixelFormatRGBA32Uint;
    case DXGI_FORMAT_R32G32B32A32_SINT: return MTLPixelFormatRGBA32Sint;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT: return MTLPixelFormatRG32Float;
    case DXGI_FORMAT_R32G32_UINT: return MTLPixelFormatRG32Uint;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT: return MTLPixelFormatRG16Float;
    case DXGI_FORMAT_R16G16_UNORM: return MTLPixelFormatRG16Unorm;
    case DXGI_FORMAT_R16G16_UINT: return MTLPixelFormatRG16Uint;
    case DXGI_FORMAT_R16G16_SNORM: return MTLPixelFormatRG16Snorm;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: return MTLPixelFormatR32Float;
    case DXGI_FORMAT_R32_UINT: return MTLPixelFormatR32Uint;
    case DXGI_FORMAT_R32_SINT: return MTLPixelFormatR32Sint;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM: return MTLPixelFormatRG8Unorm;
    case DXGI_FORMAT_R8G8_UINT: return MTLPixelFormatRG8Uint;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: return MTLPixelFormatR16Float;
    case DXGI_FORMAT_R16_UNORM: return MTLPixelFormatR16Unorm;
    case DXGI_FORMAT_R16_UINT: return MTLPixelFormatR16Uint;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
    case DXGI_FORMAT_R8_UINT: return MTLPixelFormatR8Uint;
    case DXGI_FORMAT_A8_UNORM: return MTLPixelFormatA8Unorm;
    // GX has no stencil; 24-bit depth (not on Apple GPUs) is stored as
    // 32-bit float, which holds every 24-bit unorm value exactly.
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return MTLPixelFormatDepth32Float;
    case DXGI_FORMAT_D16_UNORM: return MTLPixelFormatDepth16Unorm;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM: return MTLPixelFormatBC1_RGBA;
    case DXGI_FORMAT_BC2_UNORM: return MTLPixelFormatBC2_RGBA;
    case DXGI_FORMAT_BC3_UNORM: return MTLPixelFormatBC3_RGBA;
    default: return MTLPixelFormatInvalid;
    }
}

// How a depth format's texels look in D3D12 buffer copies. Metal stores
// them all as 32-bit float, so the packed layouts are converted by shaders.
enum class DepthLayout : std::uint32_t { D24S8 = 0u, D32S8X24 = 1u, Native = 2u };
DepthLayout depth_layout(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return DepthLayout::D24S8;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return DepthLayout::D32S8X24;
    default: return DepthLayout::Native;
    }
}

bool is_depth_format(MTLPixelFormat format) noexcept {
    return format == MTLPixelFormatDepth32Float || format == MTLPixelFormatDepth16Unorm ||
           format == MTLPixelFormatDepth32Float_Stencil8;
}

MTLVertexFormat vertex_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return MTLVertexFormatFloat4;
    case DXGI_FORMAT_R32G32B32_FLOAT: return MTLVertexFormatFloat3;
    case DXGI_FORMAT_R32G32_FLOAT: return MTLVertexFormatFloat2;
    case DXGI_FORMAT_R32_FLOAT: return MTLVertexFormatFloat;
    case DXGI_FORMAT_R32G32B32A32_UINT: return MTLVertexFormatUInt4;
    case DXGI_FORMAT_R32G32B32_UINT: return MTLVertexFormatUInt3;
    case DXGI_FORMAT_R32G32_UINT: return MTLVertexFormatUInt2;
    case DXGI_FORMAT_R32_UINT: return MTLVertexFormatUInt;
    case DXGI_FORMAT_R32_SINT: return MTLVertexFormatInt;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
    case DXGI_FORMAT_R8G8B8A8_UINT: return MTLVertexFormatUChar4;
    case DXGI_FORMAT_R16G16_FLOAT: return MTLVertexFormatHalf2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return MTLVertexFormatHalf4;
    case DXGI_FORMAT_R16G16_SINT: return MTLVertexFormatShort2;
    case DXGI_FORMAT_R16G16_UINT: return MTLVertexFormatUShort2;
    default: return MTLVertexFormatInvalid;
    }
}

MTLBlendFactor blend_factor(D3D12_BLEND blend) noexcept {
    switch (blend) {
    case D3D12_BLEND_ZERO: return MTLBlendFactorZero;
    case D3D12_BLEND_ONE: return MTLBlendFactorOne;
    case D3D12_BLEND_SRC_COLOR: return MTLBlendFactorSourceColor;
    case D3D12_BLEND_INV_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case D3D12_BLEND_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case D3D12_BLEND_INV_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case D3D12_BLEND_DEST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case D3D12_BLEND_INV_DEST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case D3D12_BLEND_DEST_COLOR: return MTLBlendFactorDestinationColor;
    case D3D12_BLEND_INV_DEST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case D3D12_BLEND_SRC_ALPHA_SAT: return MTLBlendFactorSourceAlphaSaturated;
    case D3D12_BLEND_BLEND_FACTOR: return MTLBlendFactorBlendColor;
    case D3D12_BLEND_INV_BLEND_FACTOR: return MTLBlendFactorOneMinusBlendColor;
    case D3D12_BLEND_SRC1_COLOR: return MTLBlendFactorSource1Color;
    case D3D12_BLEND_INV_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
    case D3D12_BLEND_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
    case D3D12_BLEND_INV_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}

bool uses_second_source(D3D12_BLEND blend) noexcept {
    return blend == D3D12_BLEND_SRC1_COLOR || blend == D3D12_BLEND_INV_SRC1_COLOR ||
           blend == D3D12_BLEND_SRC1_ALPHA || blend == D3D12_BLEND_INV_SRC1_ALPHA;
}

MTLBlendOperation blend_operation(D3D12_BLEND_OP op) noexcept {
    switch (op) {
    case D3D12_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
    case D3D12_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case D3D12_BLEND_OP_MIN: return MTLBlendOperationMin;
    case D3D12_BLEND_OP_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction compare_function(D3D12_COMPARISON_FUNC func) noexcept {
    switch (func) {
    case D3D12_COMPARISON_FUNC_NEVER: return MTLCompareFunctionNever;
    case D3D12_COMPARISON_FUNC_LESS: return MTLCompareFunctionLess;
    case D3D12_COMPARISON_FUNC_EQUAL: return MTLCompareFunctionEqual;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL: return MTLCompareFunctionLessEqual;
    case D3D12_COMPARISON_FUNC_GREATER: return MTLCompareFunctionGreater;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL: return MTLCompareFunctionNotEqual;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLSamplerAddressMode address_mode(D3D12_TEXTURE_ADDRESS_MODE mode) noexcept {
    switch (mode) {
    case D3D12_TEXTURE_ADDRESS_MODE_WRAP: return MTLSamplerAddressModeRepeat;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return MTLSamplerAddressModeMirrorClampToEdge;
    case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return MTLSamplerAddressModeClampToZero;
    default: return MTLSamplerAddressModeClampToEdge;
    }
}

MTLPrimitiveType primitive_type(D3D12_PRIMITIVE_TOPOLOGY topology) noexcept {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return MTLPrimitiveTypePoint;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return MTLPrimitiveTypeLine;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return MTLPrimitiveTypeLineStrip;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

std::uint64_t hash_words(std::span<const std::uint32_t> words, std::uint64_t seed) noexcept {
    std::uint64_t hash = 1469598103934665603ull ^ seed;
    for (const std::uint32_t word : words) hash = (hash ^ word) * 1099511628211ull;
    return hash;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
class MetalBuffer final : public BackendResource {
public:
    explicit MetalBuffer(id<MTLBuffer> buffer) : buffer_(buffer) {}
    std::uint8_t* cpu() noexcept override { return static_cast<std::uint8_t*>([buffer_ contents]); }
    id<MTLBuffer> buffer() const noexcept { return buffer_; }

private:
    id<MTLBuffer> buffer_;
};

class MetalTexture final : public BackendResource {
public:
    MetalTexture(id<MTLTexture> texture, const D3D12_RESOURCE_DESC& desc)
        : texture_(texture), desc_(desc) {}
    std::uint8_t* cpu() noexcept override { return nullptr; }
    id<MTLTexture> texture() const noexcept { return texture_; }
    const D3D12_RESOURCE_DESC& desc() const noexcept { return desc_; }

    // A view with another format and/or mip range (SRV/RTV descriptors).
    id<MTLTexture> view(MTLPixelFormat format, NSUInteger first_mip, NSUInteger mips) {
        if (format == MTLPixelFormatInvalid) format = [texture_ pixelFormat];
        if (mips == 0u || first_mip + mips > [texture_ mipmapLevelCount]) {
            mips = [texture_ mipmapLevelCount] - std::min<NSUInteger>(first_mip, [texture_ mipmapLevelCount] - 1u);
        }
        if (format == [texture_ pixelFormat] && first_mip == 0u &&
            mips == [texture_ mipmapLevelCount]) {
            return texture_;
        }
        const std::uint64_t key = (static_cast<std::uint64_t>(format) << 32u) |
                                  (static_cast<std::uint64_t>(first_mip) << 16u) | mips;
        const std::lock_guard<std::mutex> lock(mutex_);
        auto found = views_.find(key);
        if (found != views_.end()) return found->second;
        id<MTLTexture> created = [texture_ newTextureViewWithPixelFormat:format
                                                             textureType:[texture_ textureType]
                                                                  levels:NSMakeRange(first_mip, mips)
                                                                  slices:NSMakeRange(0, [texture_ arrayLength])];
        if (created == nil) created = texture_;
        views_.emplace(key, created);
        return created;
    }

private:
    id<MTLTexture> texture_;
    D3D12_RESOURCE_DESC desc_;
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, id<MTLTexture>> views_;
};

MetalBuffer* buffer_of(Resource* resource) noexcept {
    return resource != nullptr && resource->is_buffer()
               ? static_cast<MetalBuffer*>(resource->storage())
               : nullptr;
}
MetalTexture* texture_of(Resource* resource) noexcept {
    return resource != nullptr && !resource->is_buffer()
               ? static_cast<MetalTexture*>(resource->storage())
               : nullptr;
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------
struct CompiledShader {
    id<MTLFunction> function;
    std::vector<xlate::VertexInput> vertex_inputs;
};

class MetalPipeline final : public BackendPipeline {
public:
    GraphicsPipelineInfo info;
    CompiledShader vs;
    CompiledShader ps;
    bool has_ps = false;
    id<MTLDepthStencilState> depth_state;
    MTLCullMode cull = MTLCullModeNone;
    MTLWinding winding = MTLWindingClockwise;
    MTLTriangleFillMode fill = MTLTriangleFillModeFill;
    MTLDepthClipMode depth_clip = MTLDepthClipModeClip;
    // Vertex slot -> attribute indices fed from it.
    std::array<std::uint32_t, 8> slot_strides_seen{};
    std::mutex mutex;
    std::unordered_map<std::uint64_t, id<MTLRenderPipelineState>> states;  // by strides
};

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------
constexpr NSUInteger kVertexBufferBase = xlate::kVertexBufferIndex;  // slot 0 -> 30, 1 -> 29

class MetalBackend final : public Backend {
public:
    explicit MetalBackend(id<MTLDevice> device)
        : device_(device), queue_([device newCommandQueue]) {}

    const char* name() const noexcept override { return "metal"; }
    std::wstring adapter_description() const override {
        NSString* name = [device_ name];
        std::wstring text;
        for (NSUInteger i = 0; i < [name length]; ++i) text.push_back(static_cast<wchar_t>([name characterAtIndex:i]));
        return text;
    }
    std::uint64_t video_memory_bytes() const noexcept override {
        return static_cast<std::uint64_t>([device_ recommendedMaxWorkingSetSize]);
    }

    std::unique_ptr<BackendResource> create_buffer(std::uint64_t size) override {
        id<MTLBuffer> buffer = [device_ newBufferWithLength:std::max<std::uint64_t>(size, 16u)
                                                     options:MTLResourceStorageModeShared |
                                                             MTLResourceCPUCacheModeDefaultCache];
        if (buffer == nil) return nullptr;
        return std::make_unique<MetalBuffer>(buffer);
    }

    std::unique_ptr<BackendResource> create_texture(const TextureInfo& info) override {
        const D3D12_RESOURCE_DESC& desc = info.desc;
        const MTLPixelFormat format = pixel_format(desc.Format);
        if (format == MTLPixelFormatInvalid) {
            std::cerr << "[metal] no pixel format for DXGI format " << desc.Format << '\n';
            return nullptr;
        }
        MTLTextureDescriptor* d = [[MTLTextureDescriptor alloc] init];
        d.pixelFormat = format;
        d.width = static_cast<NSUInteger>(desc.Width);
        d.height = std::max<NSUInteger>(desc.Height, 1u);
        d.mipmapLevelCount = std::max<NSUInteger>(desc.MipLevels, 1u);
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        if (info.render_target || info.depth_stencil || is_depth_format(format)) {
            d.usage |= MTLTextureUsageRenderTarget;
        }
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            d.textureType = MTLTextureType3D;
            d.depth = std::max<NSUInteger>(desc.DepthOrArraySize, 1u);
        } else if (desc.DepthOrArraySize > 1u) {
            d.textureType = MTLTextureType2DArray;
            d.arrayLength = desc.DepthOrArraySize;
        } else if (desc.SampleDesc.Count > 1u) {
            d.textureType = MTLTextureType2DMultisample;
            d.sampleCount = desc.SampleDesc.Count;
        } else {
            d.textureType = MTLTextureType2D;
        }
        id<MTLTexture> texture = [device_ newTextureWithDescriptor:d];
        if (texture == nil) return nullptr;
        return std::make_unique<MetalTexture>(texture, desc);
    }

    std::unique_ptr<BackendPipeline> create_pipeline(const GraphicsPipelineInfo& info,
                                                     std::string& error) override {
        auto pipeline = std::make_unique<MetalPipeline>();
        pipeline->info = info;
        if (!info.vs || info.vs->spirv.empty()) {
            error = "pipeline without a vertex shader";
            return nullptr;
        }
        bool dual_source = false;
        for (UINT i = 0; i < 8u; ++i) {
            const auto& rt = info.blend.RenderTarget[info.blend.IndependentBlendEnable ? i : 0u];
            dual_source |= rt.BlendEnable && (uses_second_source(rt.SrcBlend) ||
                                              uses_second_source(rt.DestBlend) ||
                                              uses_second_source(rt.SrcBlendAlpha) ||
                                              uses_second_source(rt.DestBlendAlpha));
        }
        const bool points = info.topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        if (!compile(*info.vs, xlate::Stage::Vertex, false, points, pipeline->vs, error)) {
            return nullptr;
        }
        if (info.ps && !info.ps->spirv.empty()) {
            if (!compile(*info.ps, xlate::Stage::Pixel, dual_source, false, pipeline->ps, error)) {
                return nullptr;
            }
            pipeline->has_ps = true;
        }
        // Depth state.
        MTLDepthStencilDescriptor* ds = [[MTLDepthStencilDescriptor alloc] init];
        if (info.depth_stencil.DepthEnable && info.dsv_format != DXGI_FORMAT_UNKNOWN) {
            ds.depthCompareFunction = compare_function(info.depth_stencil.DepthFunc);
            ds.depthWriteEnabled = info.depth_stencil.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        } else {
            ds.depthCompareFunction = MTLCompareFunctionAlways;
            ds.depthWriteEnabled = NO;
        }
        pipeline->depth_state = [device_ newDepthStencilStateWithDescriptor:ds];
        switch (info.rasterizer.CullMode) {
        case D3D12_CULL_MODE_FRONT: pipeline->cull = MTLCullModeFront; break;
        case D3D12_CULL_MODE_BACK: pipeline->cull = MTLCullModeBack; break;
        default: pipeline->cull = MTLCullModeNone; break;
        }
        pipeline->winding = info.rasterizer.FrontCounterClockwise ? MTLWindingCounterClockwise
                                                                  : MTLWindingClockwise;
        pipeline->fill = info.rasterizer.FillMode == D3D12_FILL_MODE_WIREFRAME
                             ? MTLTriangleFillModeLines
                             : MTLTriangleFillModeFill;
        pipeline->depth_clip = info.rasterizer.DepthClipEnable ? MTLDepthClipModeClip
                                                               : MTLDepthClipModeClamp;
        // Build the stride-independent variant now when the layout has no
        // vertex buffers, so errors surface at creation as on D3D12.
        if (info.input_layout.empty()) {
            if (render_state(*pipeline, {}, error) == nil) return nullptr;
        }
        return pipeline;
    }

    void execute(std::span<CommandList* const> lists, std::function<void()> done) override {
        const std::lock_guard<std::mutex> lock(encode_mutex_);
        @autoreleasepool {
            id<MTLCommandBuffer> commands = [queue_ commandBuffer];
            Encoder encoder(*this, commands);
            for (CommandList* list : lists) {
                for (const cmd::Command& command : list->commands()) encoder.run(command);
            }
            encoder.finish();
            submit(commands, std::move(done));
        }
    }

    void after_pending_work(std::function<void()> done) override {
        if (!done) return;
        std::unique_lock<std::mutex> lock(completion_mutex_);
        if (completed_serial_ >= submitted_serial_) {
            lock.unlock();
            done();
            return;
        }
        callbacks_.emplace_back(submitted_serial_, std::move(done));
    }

    void present(Resource* buffer, UINT) override {
        MetalTexture* source = texture_of(buffer);
        if (source == nullptr) return;
        CAMetalLayer* layer = (__bridge CAMetalLayer*)galaxy::host_platform::current_view();
        if (layer == nil || ![layer isKindOfClass:[CAMetalLayer class]]) return;
        const std::lock_guard<std::mutex> lock(encode_mutex_);
        @autoreleasepool {
            if (layer.device != device_) layer.device = device_;
            if (layer.pixelFormat != MTLPixelFormatBGRA8Unorm) layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            const CGSize size = CGSizeMake(static_cast<CGFloat>([source->texture() width]),
                                           static_cast<CGFloat>([source->texture() height]));
            if (!CGSizeEqualToSize(layer.drawableSize, size)) layer.drawableSize = size;
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (drawable == nil) return;
            id<MTLCommandBuffer> commands = [queue_ commandBuffer];
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:present_pipeline()];
            [encoder setFragmentTexture:source->texture() atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            [commands presentDrawable:drawable];
            submit(commands, {});
        }
    }

private:
    friend class Encoder;

    // ----- Shader compilation ---------------------------------------------
    bool compile(const ShaderProgram& program, xlate::Stage stage, bool dual_source, bool points,
                 CompiledShader& out, std::string& error) {
        const std::uint64_t key = hash_words(program.spirv, (dual_source ? 1u : 0u) |
                                                                (points ? 2u : 0u) |
                                                                (stage == xlate::Stage::Pixel ? 4u : 0u));
        {
            const std::lock_guard<std::mutex> lock(library_mutex_);
            auto found = functions_.find(key);
            if (found != functions_.end()) {
                out = found->second;
                return true;
            }
        }
        xlate::Options options;
        options.entry_point = program.entry_point;
        options.label = program.label;
        options.dual_source_blend = dual_source;
        options.points = points;
#if TARGET_OS_IPHONE
        options.platform = xlate::Platform::iOS;
#else
        options.platform = xlate::Platform::macOS;
#endif
        const xlate::Result msl = xlate::spirv_to_msl(program.spirv, stage, options);
        if (!msl.ok) {
            error = program.label + ": " + msl.log;
            return false;
        }
        @autoreleasepool {
            MTLCompileOptions* compile_options = [[MTLCompileOptions alloc] init];
            compile_options.languageVersion = MTLLanguageVersion2_3;
            compile_options.fastMathEnabled = NO;  // GX math is exact-ish
            NSError* failure = nil;
            id<MTLLibrary> library =
                [device_ newLibraryWithSource:[NSString stringWithUTF8String:msl.msl.c_str()]
                                      options:compile_options
                                        error:&failure];
            if (library == nil) {
                error = program.label + ": Metal: " +
                        (failure != nil ? std::string([[failure localizedDescription] UTF8String]) : "?");
                return false;
            }
            out.function = [library newFunctionWithName:[NSString stringWithUTF8String:msl.entry_point.c_str()]];
            if (out.function == nil) {
                error = program.label + ": no function " + msl.entry_point;
                return false;
            }
        }
        out.vertex_inputs = msl.vertex_inputs;
        const std::lock_guard<std::mutex> lock(library_mutex_);
        functions_.emplace(key, out);
        return true;
    }

    // ----- Render pipeline variants -----------------------------------------
    // `strides`: per vertex slot, from the bound vertex buffer views.
    id<MTLRenderPipelineState> render_state(MetalPipeline& pipeline,
                                            const std::array<std::uint32_t, 8>& strides,
                                            std::string& error) {
        std::uint64_t key = 0u;
        for (unsigned slot = 0; slot < 8u; ++slot) key = key * 1315423911u + strides[slot];
        {
            const std::lock_guard<std::mutex> lock(pipeline.mutex);
            auto found = pipeline.states.find(key);
            if (found != pipeline.states.end()) return found->second;
        }
        const GraphicsPipelineInfo& info = pipeline.info;
        MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
        d.vertexFunction = pipeline.vs.function;
        if (pipeline.has_ps) d.fragmentFunction = pipeline.ps.function;
        d.rasterSampleCount = std::max<UINT>(info.sample.Count, 1u);
        d.alphaToCoverageEnabled = info.blend.AlphaToCoverageEnable ? YES : NO;
        switch (info.topology_type) {
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: d.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
        case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: d.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
        default: d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; break;
        }
        for (UINT i = 0; i < info.num_render_targets && i < 8u; ++i) {
            const MTLPixelFormat format = pixel_format(info.rtv_formats[i]);
            d.colorAttachments[i].pixelFormat = format;
            const auto& rt = info.blend.RenderTarget[info.blend.IndependentBlendEnable ? i : 0u];
            MTLColorWriteMask mask = MTLColorWriteMaskNone;
            if (rt.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_RED) mask |= MTLColorWriteMaskRed;
            if (rt.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_GREEN) mask |= MTLColorWriteMaskGreen;
            if (rt.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_BLUE) mask |= MTLColorWriteMaskBlue;
            if (rt.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_ALPHA) mask |= MTLColorWriteMaskAlpha;
            d.colorAttachments[i].writeMask = mask;
            // Metal has no logic operations; such draws write their color
            // unblended (logged once).
            if (rt.LogicOpEnable && !logic_op_reported_.exchange(true)) {
                std::cerr << "[metal] GX logic ops are not supported; drawn unblended\n";
            }
            if (rt.BlendEnable && !rt.LogicOpEnable) {
                d.colorAttachments[i].blendingEnabled = YES;
                d.colorAttachments[i].sourceRGBBlendFactor = blend_factor(rt.SrcBlend);
                d.colorAttachments[i].destinationRGBBlendFactor = blend_factor(rt.DestBlend);
                d.colorAttachments[i].rgbBlendOperation = blend_operation(rt.BlendOp);
                d.colorAttachments[i].sourceAlphaBlendFactor = blend_factor(rt.SrcBlendAlpha);
                d.colorAttachments[i].destinationAlphaBlendFactor = blend_factor(rt.DestBlendAlpha);
                d.colorAttachments[i].alphaBlendOperation = blend_operation(rt.BlendOpAlpha);
            }
        }
        if (info.dsv_format != DXGI_FORMAT_UNKNOWN) {
            d.depthAttachmentPixelFormat = pixel_format(info.dsv_format);
        }
        // Vertex layout: input elements by semantic onto the shader's
        // attributes; buffer slots count down from kVertexBufferBase.
        if (!info.input_layout.empty()) {
            MTLVertexDescriptor* vertex = [MTLVertexDescriptor vertexDescriptor];
            std::array<bool, 8> used{};
            for (const InputElement& e : info.input_layout) {
                std::string semantic = e.semantic_name;
                for (char& c : semantic) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                semantic += std::to_string(e.semantic_index);
                const auto found = std::find_if(
                    pipeline.vs.vertex_inputs.begin(), pipeline.vs.vertex_inputs.end(),
                    [&](const xlate::VertexInput& input) { return input.semantic == semantic; });
                if (found == pipeline.vs.vertex_inputs.end()) continue;  // unused by the shader
                const MTLVertexFormat format = vertex_format(e.format);
                if (format == MTLVertexFormatInvalid || e.input_slot >= 8u) {
                    error = "unsupported vertex element " + semantic;
                    return nil;
                }
                vertex.attributes[found->attribute].format = format;
                vertex.attributes[found->attribute].offset = e.aligned_byte_offset;
                vertex.attributes[found->attribute].bufferIndex = kVertexBufferBase - e.input_slot;
                if (!used[e.input_slot]) {
                    used[e.input_slot] = true;
                    auto* layout = vertex.layouts[kVertexBufferBase - e.input_slot];
                    layout.stride = std::max<std::uint32_t>(strides[e.input_slot], 4u);
                    layout.stepFunction = e.classification == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                              ? MTLVertexStepFunctionPerInstance
                                              : MTLVertexStepFunctionPerVertex;
                    layout.stepRate = e.classification == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                          ? std::max<UINT>(e.instance_step_rate, 1u)
                                          : 1u;
                }
            }
            d.vertexDescriptor = vertex;
        }
        NSError* failure = nil;
        id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d error:&failure];
        if (state == nil) {
            error = info.label + ": " +
                    (failure != nil ? std::string([[failure localizedDescription] UTF8String]) : "?");
            return nil;
        }
        const std::lock_guard<std::mutex> lock(pipeline.mutex);
        pipeline.states.emplace(key, state);
        return state;
    }

    // ----- Samplers ----------------------------------------------------------
    id<MTLSamplerState> sampler(const D3D12_SAMPLER_DESC& desc) {
        std::uint64_t key = 1469598103934665603ull;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&desc);
        for (std::size_t i = 0; i < sizeof(desc); ++i) key = (key ^ bytes[i]) * 1099511628211ull;
        const std::lock_guard<std::mutex> lock(sampler_mutex_);
        auto found = samplers_.find(key);
        if (found != samplers_.end()) return found->second;
        MTLSamplerDescriptor* d = [[MTLSamplerDescriptor alloc] init];
        const unsigned filter = static_cast<unsigned>(desc.Filter);
        const bool anisotropic = (filter & 0x40u) != 0u;
        d.minFilter = ((filter >> 4u) & 1u) || anisotropic ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.magFilter = ((filter >> 2u) & 1u) || anisotropic ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = (filter & 1u) || anisotropic ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        if (desc.MaxLOD <= 0.0f && desc.MinLOD <= 0.0f) d.mipFilter = MTLSamplerMipFilterNotMipmapped;
        d.sAddressMode = address_mode(desc.AddressU);
        d.tAddressMode = address_mode(desc.AddressV);
        d.rAddressMode = address_mode(desc.AddressW);
        d.maxAnisotropy = anisotropic ? std::clamp<UINT>(desc.MaxAnisotropy, 1u, 16u) : 1u;
        d.lodMinClamp = std::max(desc.MinLOD, 0.0f);
        d.lodMaxClamp = std::max(desc.MaxLOD, d.lodMinClamp);
        if ((filter & 0x80u) != 0u) d.compareFunction = compare_function(desc.ComparisonFunc);
        id<MTLSamplerState> state = [device_ newSamplerStateWithDescriptor:d];
        samplers_.emplace(key, state);
        return state;
    }

    // ----- Internal pipelines -------------------------------------------------
    id<MTLLibrary> internal_library() {
        const std::lock_guard<std::mutex> lock(library_mutex_);
        if (internal_library_ != nil) return internal_library_;
        static const char* kSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Fullscreen { float4 position [[position]]; float2 uv; };
vertex Fullscreen fullscreen_vs(uint id [[vertex_id]]) {
    Fullscreen out;
    const float2 uv = float2((id << 1) & 2, id & 2);
    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}
fragment float4 present_fs(Fullscreen in [[stage_in]], texture2d<float> source [[texture(0)]]) {
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    return source.sample(linear, in.uv);
}
struct ClearValues { float4 color; float depth; };
vertex Fullscreen clear_vs(uint id [[vertex_id]], constant ClearValues& v [[buffer(0)]]) {
    Fullscreen out;
    const float2 uv = float2((id << 1) & 2, id & 2);
    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), v.depth, 1.0);
    out.uv = uv;
    return out;
}
struct ClearOut { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };
fragment ClearOut clear_fs(Fullscreen in [[stage_in]], constant ClearValues& v [[buffer(0)]]) {
    ClearOut out; out.c0 = v.color; out.c1 = v.color; out.c2 = v.color; out.c3 = v.color;
    return out;
}
// Depth texels <-> D3D12 copy layouts (mode 0: D24 unorm, stencil byte 0;
// mode 1: D32 float + 32 bits of stencil/padding).
struct DepthCopy { uint x, y, width, height, level, offset_words, row_words, mode; };
kernel void depth_to_buffer(depth2d<float> source [[texture(0)]], device uint* out [[buffer(0)]],
                            constant DepthCopy& p [[buffer(1)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= p.width || id.y >= p.height) return;
    const float depth = source.read(uint2(p.x, p.y) + id, p.level);
    const uint row = p.offset_words + id.y * p.row_words;
    if (p.mode == 0u) {
        out[row + id.x] = uint(rint(saturate(depth) * 16777215.0));
    } else {
        out[row + id.x * 2u] = as_type<uint>(depth);
        out[row + id.x * 2u + 1u] = 0u;
    }
}
struct DepthOut { float depth [[depth(any)]]; };
fragment DepthOut depth_from_buffer(Fullscreen in [[stage_in]], const device uint* words [[buffer(0)]],
                                    constant DepthCopy& p [[buffer(1)]]) {
    const uint2 at = uint2(in.position.xy) - uint2(p.x, p.y);
    const uint row = p.offset_words + at.y * p.row_words;
    DepthOut out;
    out.depth = p.mode == 0u ? float(words[row + at.x] & 0xffffffu) / 16777215.0
                             : as_type<float>(words[row + at.x * 2u]);
    return out;
}
)MSL";
        NSError* failure = nil;
        internal_library_ = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kSource]
                                                  options:nil
                                                    error:&failure];
        if (internal_library_ == nil) {
            std::cerr << "[metal] internal shaders: "
                      << (failure != nil ? [[failure localizedDescription] UTF8String] : "?") << '\n';
        }
        return internal_library_;
    }

    id<MTLRenderPipelineState> present_pipeline() {
        if (present_pipeline_ != nil) return present_pipeline_;
        id<MTLLibrary> library = internal_library();
        MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
        d.vertexFunction = [library newFunctionWithName:@"fullscreen_vs"];
        d.fragmentFunction = [library newFunctionWithName:@"present_fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        present_pipeline_ = [device_ newRenderPipelineStateWithDescriptor:d error:nil];
        return present_pipeline_;
    }

    // A pipeline that writes `color` to the attachments enabled in `write`
    // (and depth when a depth format is given), for rectangle clears.
    id<MTLRenderPipelineState> clear_pipeline(const std::array<MTLPixelFormat, 8>& colors,
                                              std::uint8_t write, MTLPixelFormat depth) {
        std::uint64_t key = depth;
        for (const MTLPixelFormat format : colors) key = key * 31u + format;
        key = key * 257u + write;
        const std::lock_guard<std::mutex> lock(clear_mutex_);
        auto found = clear_pipelines_.find(key);
        if (found != clear_pipelines_.end()) return found->second;
        id<MTLLibrary> library = internal_library();
        MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
        d.vertexFunction = [library newFunctionWithName:@"clear_vs"];
        d.fragmentFunction = [library newFunctionWithName:@"clear_fs"];
        for (unsigned i = 0; i < 8u; ++i) {
            d.colorAttachments[i].pixelFormat = colors[i];
            d.colorAttachments[i].writeMask = (write >> i) & 1u ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        }
        d.depthAttachmentPixelFormat = depth;
        id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d error:nil];
        clear_pipelines_.emplace(key, state);
        return state;
    }

    id<MTLComputePipelineState> depth_to_buffer_pipeline() {
        const std::lock_guard<std::mutex> lock(clear_mutex_);
        if (depth_to_buffer_ == nil) {
            id<MTLFunction> function = [internal_library() newFunctionWithName:@"depth_to_buffer"];
            NSError* failure = nil;
            depth_to_buffer_ = [device_ newComputePipelineStateWithFunction:function error:&failure];
            if (depth_to_buffer_ == nil) {
                std::cerr << "[metal] depth_to_buffer: "
                          << (failure != nil ? [[failure localizedDescription] UTF8String] : "?") << '\n';
            }
        }
        return depth_to_buffer_;
    }

    id<MTLRenderPipelineState> depth_from_buffer_pipeline(MTLPixelFormat depth) {
        const std::lock_guard<std::mutex> lock(clear_mutex_);
        if (depth_from_buffer_ == nil) {
            id<MTLLibrary> library = internal_library();
            MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
            d.vertexFunction = [library newFunctionWithName:@"fullscreen_vs"];
            d.fragmentFunction = [library newFunctionWithName:@"depth_from_buffer"];
            d.depthAttachmentPixelFormat = depth;
            NSError* failure = nil;
            depth_from_buffer_ = [device_ newRenderPipelineStateWithDescriptor:d error:&failure];
            if (depth_from_buffer_ == nil) {
                std::cerr << "[metal] depth_from_buffer: "
                          << (failure != nil ? [[failure localizedDescription] UTF8String] : "?") << '\n';
            }
        }
        return depth_from_buffer_;
    }

    id<MTLDepthStencilState> depth_state(bool write) {
        const std::lock_guard<std::mutex> lock(clear_mutex_);
        __strong id<MTLDepthStencilState>& state = write ? depth_write_always_ : depth_disabled_;
        if (state == nil) {
            MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
            d.depthCompareFunction = MTLCompareFunctionAlways;
            d.depthWriteEnabled = write ? YES : NO;
            state = [device_ newDepthStencilStateWithDescriptor:d];
        }
        return state;
    }

    // ----- Submission ---------------------------------------------------------
    void submit(id<MTLCommandBuffer> commands, std::function<void()> done) {
        std::uint64_t serial = 0;
        {
            const std::lock_guard<std::mutex> lock(completion_mutex_);
            serial = ++submitted_serial_;
            if (done) callbacks_.emplace_back(serial, std::move(done));
        }
        [commands addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
            if ([buffer status] == MTLCommandBufferStatusError) {
                NSError* failure = [buffer error];
                std::cerr << "[metal] command buffer failed: "
                          << (failure != nil ? [[failure localizedDescription] UTF8String] : "?") << '\n';
            }
            this->completed(serial);
        }];
        [commands commit];
    }

    void completed(std::uint64_t serial) {
        std::vector<std::function<void()>> ready;
        {
            const std::lock_guard<std::mutex> lock(completion_mutex_);
            completed_serial_ = std::max(completed_serial_, serial);
            for (auto it = callbacks_.begin(); it != callbacks_.end();) {
                if (it->first <= completed_serial_) {
                    ready.push_back(std::move(it->second));
                    it = callbacks_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& callback : ready) callback();
    }

    // ----- Encoding -----------------------------------------------------------
    class Encoder {
    public:
        Encoder(MetalBackend& backend, id<MTLCommandBuffer> commands)
            : backend_(backend), commands_(commands) {}

        void run(const cmd::Command& command) {
            std::visit([this](const auto& c) { apply(c); }, command);
        }

        void finish() { end_encoders(); }

    private:
        // -- state
        void apply(const cmd::SetPipeline& c) { pipeline_ = c.pipeline; pipeline_dirty_ = true; }
        void apply(const cmd::SetRootSignature& c) {
            if (signature_ != c.signature) {
                signature_ = c.signature;
                tables_.fill(nullptr);
                views_.fill(0u);
                constants_.fill({});
                bindings_dirty_ = true;
            }
        }
        void apply(const cmd::SetRootTable& c) {
            if (c.index < tables_.size()) tables_[c.index] = c.table;
            bindings_dirty_ = true;
        }
        void apply(const cmd::SetRootView& c) {
            if (c.index < views_.size()) views_[c.index] = c.address;
            bindings_dirty_ = true;
        }
        void apply(const cmd::SetRootConstants& c) {
            if (c.index >= constants_.size()) return;
            auto& values = constants_[c.index];
            if (values.size() < c.first + c.values.size()) values.resize(c.first + c.values.size());
            std::copy(c.values.begin(), c.values.end(), values.begin() + c.first);
            bindings_dirty_ = true;
        }
        void apply(const cmd::SetTopology& c) { topology_ = c.topology; }
        void apply(const cmd::SetViewports& c) {
            if (!c.viewports.empty()) viewport_ = c.viewports[0];
            has_viewport_ = !c.viewports.empty();
            viewport_dirty_ = true;
        }
        void apply(const cmd::SetScissors& c) {
            if (!c.rects.empty()) scissor_ = c.rects[0];
            has_scissor_ = !c.rects.empty();
            viewport_dirty_ = true;
        }
        void apply(const cmd::SetBlendFactor& c) { blend_factor_ = c.factor; dynamic_dirty_ = true; }
        void apply(const cmd::SetStencilRef& c) { stencil_ref_ = c.reference; dynamic_dirty_ = true; }
        void apply(const cmd::SetVertexBuffers& c) {
            for (std::size_t i = 0; i < c.views.size() && c.start + i < vertex_buffers_.size(); ++i) {
                vertex_buffers_[c.start + i] = c.views[i];
            }
            vertex_dirty_ = true;
        }
        void apply(const cmd::SetIndexBuffer& c) { index_buffer_ = c.view; }
        void apply(const cmd::SetRenderTargets& c) {
            targets_ = c;
            end_render();  // the next draw opens a pass on the new targets
        }
        void apply(const cmd::Barrier&) {}
        void apply(const cmd::Discard&) {}

        // -- draws
        void apply(const cmd::Draw& c) {
            if (pipeline_ == nullptr || c.count == 0u) return;
            auto* pipeline = static_cast<MetalPipeline*>(pipeline_->compiled());
            if (pipeline == nullptr) return;
            if (!begin_render()) return;
            // Only the slots the pipeline reads select its vertex layout.
            std::array<std::uint32_t, 8> strides{};
            for (const InputElement& e : pipeline->info.input_layout) {
                if (e.input_slot < 8u) strides[e.input_slot] = vertex_buffers_[e.input_slot].StrideInBytes;
            }
            if (pipeline_dirty_ || pipeline != bound_pipeline_ || strides != bound_strides_) {
                std::string error;
                id<MTLRenderPipelineState> state = backend_.render_state(*pipeline, strides, error);
                if (state == nil) {
                    report("pipeline", error);
                    return;
                }
                [render_ setRenderPipelineState:state];
                [render_ setDepthStencilState:pipeline->depth_state];
                [render_ setCullMode:pipeline->cull];
                [render_ setFrontFacingWinding:pipeline->winding];
                [render_ setTriangleFillMode:pipeline->fill];
                [render_ setDepthClipMode:pipeline->depth_clip];
                const auto& raster = pipeline->info.rasterizer;
                [render_ setDepthBias:static_cast<float>(raster.DepthBias) / 16777215.0f
                           slopeScale:raster.SlopeScaledDepthBias
                                clamp:raster.DepthBiasClamp];
                bound_pipeline_ = pipeline;
                bound_strides_ = strides;
                pipeline_dirty_ = false;
            }
            if (viewport_dirty_) apply_viewport();
            if (dynamic_dirty_) {
                [render_ setBlendColorRed:blend_factor_[0] green:blend_factor_[1]
                                     blue:blend_factor_[2] alpha:blend_factor_[3]];
                [render_ setStencilReferenceValue:stencil_ref_];
                dynamic_dirty_ = false;
            }
            if (bindings_dirty_) apply_bindings();
            if (vertex_dirty_) apply_vertex_buffers();
            const MTLPrimitiveType type = primitive_type(topology_);
            const NSUInteger instances = std::max<UINT>(c.instances, 1u);
            if (!c.indexed) {
                [render_ drawPrimitives:type vertexStart:c.first vertexCount:c.count
                          instanceCount:instances baseInstance:c.first_instance];
                return;
            }
            const auto [resource, offset] = AddressSpace::instance().find(index_buffer_.BufferLocation);
            MetalBuffer* indices = buffer_of(resource);
            if (indices == nullptr) return;
            const bool wide = index_buffer_.Format == DXGI_FORMAT_R32_UINT;
            [render_ drawIndexedPrimitives:type
                                indexCount:c.count
                                 indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                               indexBuffer:indices->buffer()
                         indexBufferOffset:offset + std::uint64_t{c.first} * (wide ? 4u : 2u)
                             instanceCount:instances
                                baseVertex:c.base_vertex
                              baseInstance:c.first_instance];
        }

        void apply_viewport() {
            if (has_viewport_) {
                MTLViewport v{viewport_.TopLeftX, viewport_.TopLeftY, viewport_.Width,
                              viewport_.Height, viewport_.MinDepth, viewport_.MaxDepth};
                [render_ setViewport:v];
            }
            // Metal requires the scissor inside the attachment.
            const LONG x0 = std::clamp<LONG>(has_scissor_ ? scissor_.left : 0, 0, pass_width_);
            const LONG y0 = std::clamp<LONG>(has_scissor_ ? scissor_.top : 0, 0, pass_height_);
            const LONG x1 = std::clamp<LONG>(has_scissor_ ? scissor_.right : pass_width_, x0, pass_width_);
            const LONG y1 = std::clamp<LONG>(has_scissor_ ? scissor_.bottom : pass_height_, y0, pass_height_);
            MTLScissorRect rect{static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0),
                                static_cast<NSUInteger>(x1 - x0), static_cast<NSUInteger>(y1 - y0)};
            if (rect.width == 0u || rect.height == 0u) rect = {0, 0, 1, 1};  // empty: draws one clipped pixel at most
            [render_ setScissorRect:rect];
            viewport_dirty_ = false;
        }

        // Binds a buffer address to a stage at a Metal buffer index.
        void bind_buffer(D3D12_SHADER_VISIBILITY visibility, NSUInteger index, std::uint64_t address) {
            const auto [resource, offset] = AddressSpace::instance().find(address);
            MetalBuffer* buffer = buffer_of(resource);
            if (buffer == nullptr) return;
            if (visibility == D3D12_SHADER_VISIBILITY_ALL || visibility == D3D12_SHADER_VISIBILITY_VERTEX) {
                [render_ setVertexBuffer:buffer->buffer() offset:offset atIndex:index];
            }
            if (visibility == D3D12_SHADER_VISIBILITY_ALL || visibility == D3D12_SHADER_VISIBILITY_PIXEL) {
                [render_ setFragmentBuffer:buffer->buffer() offset:offset atIndex:index];
            }
        }
        void bind_texture(D3D12_SHADER_VISIBILITY visibility, NSUInteger index, id<MTLTexture> texture) {
            if (visibility == D3D12_SHADER_VISIBILITY_ALL || visibility == D3D12_SHADER_VISIBILITY_VERTEX) {
                [render_ setVertexTexture:texture atIndex:index];
            }
            if (visibility == D3D12_SHADER_VISIBILITY_ALL || visibility == D3D12_SHADER_VISIBILITY_PIXEL) {
                [render_ setFragmentTexture:texture atIndex:index];
            }
        }
        void bind_sampler(D3D12_SHADER_VISIBILITY visibility, NSUInteger index, id<MTLSamplerState> state) {
            if (visibility == D3D12_SHADER_VISIBILITY_ALL || visibility == D3D12_SHADER_VISIBILITY_VERTEX) {
                [render_ setVertexSamplerState:state atIndex:index];
            }
            if (visibility == D3D12_SHADER_VISIBILITY_ALL || visibility == D3D12_SHADER_VISIBILITY_PIXEL) {
                [render_ setFragmentSamplerState:state atIndex:index];
            }
        }

        id<MTLTexture> texture_for(const Descriptor& descriptor) {
            MetalTexture* texture = texture_of(descriptor.resource);
            if (texture == nullptr) return nil;
            MTLPixelFormat format = MTLPixelFormatInvalid;
            NSUInteger first_mip = 0u, mips = 0u;
            if (descriptor.has_view_desc && descriptor.kind == DescriptorKind::Srv) {
                format = pixel_format(descriptor.view.srv.Format);
                if (descriptor.view.srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
                    first_mip = descriptor.view.srv.Texture2D.MostDetailedMip;
                    mips = descriptor.view.srv.Texture2D.MipLevels == UINT_MAX
                               ? 0u
                               : descriptor.view.srv.Texture2D.MipLevels;
                }
            }
            return texture->view(format, first_mip, mips);
        }

        NSUInteger buffer_index(UINT reg, UINT space) const noexcept {
            return space == 0u ? reg : xlate::kSpace1BufferBase + reg;
        }
        NSUInteger texture_index(UINT reg, UINT space) const noexcept {
            return space == 0u ? reg : 24u + reg;
        }

        void apply_bindings() {
            bindings_dirty_ = false;
            if (signature_ == nullptr) return;
            const RootSignatureLayout& layout = signature_->layout();
            for (const D3D12_STATIC_SAMPLER_DESC& s : layout.static_samplers) {
                D3D12_SAMPLER_DESC desc{};
                desc.Filter = s.Filter;
                desc.AddressU = s.AddressU;
                desc.AddressV = s.AddressV;
                desc.AddressW = s.AddressW;
                desc.MipLODBias = s.MipLODBias;
                desc.MaxAnisotropy = s.MaxAnisotropy;
                desc.ComparisonFunc = s.ComparisonFunc;
                desc.MinLOD = s.MinLOD;
                desc.MaxLOD = s.MaxLOD;
                bind_sampler(s.ShaderVisibility, s.ShaderRegister, backend_.sampler(desc));
            }
            for (std::size_t index = 0; index < layout.parameters.size(); ++index) {
                const RootParameter& p = layout.parameters[index];
                switch (p.type) {
                case D3D12_ROOT_PARAMETER_TYPE_CBV:
                case D3D12_ROOT_PARAMETER_TYPE_SRV:
                case D3D12_ROOT_PARAMETER_TYPE_UAV:
                    if (index < views_.size() && views_[index] != 0u) {
                        bind_buffer(p.visibility, buffer_index(p.shader_register, p.space), views_[index]);
                    }
                    break;
                case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
                    if (index >= constants_.size()) break;
                    std::vector<std::uint32_t> values = constants_[index];
                    values.resize(std::max<std::size_t>(p.num_constants, 1u));
                    const NSUInteger slot = buffer_index(p.shader_register, p.space);
                    const NSUInteger bytes = values.size() * sizeof(std::uint32_t);
                    if (p.visibility == D3D12_SHADER_VISIBILITY_ALL || p.visibility == D3D12_SHADER_VISIBILITY_VERTEX) {
                        [render_ setVertexBytes:values.data() length:bytes atIndex:slot];
                    }
                    if (p.visibility == D3D12_SHADER_VISIBILITY_ALL || p.visibility == D3D12_SHADER_VISIBILITY_PIXEL) {
                        [render_ setFragmentBytes:values.data() length:bytes atIndex:slot];
                    }
                    break;
                }
                case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
                    const Descriptor* table = index < tables_.size() ? tables_[index] : nullptr;
                    if (table == nullptr) break;
                    for (const DescriptorRange& range : p.ranges) {
                        const UINT count = range.count == UINT_MAX ? 16u : range.count;
                        for (UINT i = 0; i < count; ++i) {
                            const Descriptor& d = table[range.offset + i];
                            switch (range.type) {
                            case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
                                if (d.kind == DescriptorKind::Srv && d.resource != nullptr) {
                                    if (d.resource->is_buffer()) {
                                        bind_buffer(p.visibility, buffer_index(range.base_register + i, range.space),
                                                    d.resource->GetGPUVirtualAddress());
                                    } else if (id<MTLTexture> texture = texture_for(d)) {
                                        bind_texture(p.visibility,
                                                     texture_index(range.base_register + i, range.space), texture);
                                    }
                                }
                                break;
                            case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
                                if (d.kind == DescriptorKind::Cbv && d.has_view_desc) {
                                    bind_buffer(p.visibility, buffer_index(range.base_register + i, range.space),
                                                d.view.cbv.BufferLocation);
                                }
                                break;
                            case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
                                if (d.kind == DescriptorKind::Sampler && d.has_view_desc) {
                                    bind_sampler(p.visibility, range.base_register + i,
                                                 backend_.sampler(d.view.sampler));
                                }
                                break;
                            default:
                                break;
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }

        void apply_vertex_buffers() {
            vertex_dirty_ = false;
            for (unsigned slot = 0; slot < vertex_buffers_.size(); ++slot) {
                const D3D12_VERTEX_BUFFER_VIEW& view = vertex_buffers_[slot];
                if (view.BufferLocation == 0u) continue;
                const auto [resource, offset] = AddressSpace::instance().find(view.BufferLocation);
                MetalBuffer* buffer = buffer_of(resource);
                if (buffer == nullptr) continue;
                [render_ setVertexBuffer:buffer->buffer() offset:offset atIndex:kVertexBufferBase - slot];
            }
        }

        // -- render passes
        bool begin_render(const std::optional<std::array<float, 4>>& clear_color = std::nullopt,
                          int clear_target = -1, std::optional<float> clear_depth = std::nullopt) {
            if (render_ != nil && !clear_color && !clear_depth) return true;
            end_encoders();
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass_width_ = 0;
            pass_height_ = 0;
            bool any = false;
            for (std::size_t i = 0; i < targets_.colors.size() && i < 8u; ++i) {
                const Descriptor& d = targets_.colors[i];
                MetalTexture* texture = texture_of(d.resource);
                if (texture == nullptr) continue;
                const NSUInteger mip = d.mip_slice();
                pass.colorAttachments[i].texture = texture->texture();
                pass.colorAttachments[i].level = mip;
                pass.colorAttachments[i].storeAction = MTLStoreActionStore;
                if (clear_color && static_cast<int>(i) == clear_target) {
                    pass.colorAttachments[i].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[i].clearColor =
                        MTLClearColorMake((*clear_color)[0], (*clear_color)[1], (*clear_color)[2], (*clear_color)[3]);
                } else {
                    pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
                }
                pass_width_ = static_cast<LONG>(std::max<NSUInteger>([texture->texture() width] >> mip, 1u));
                pass_height_ = static_cast<LONG>(std::max<NSUInteger>([texture->texture() height] >> mip, 1u));
                any = true;
            }
            if (targets_.depth) {
                MetalTexture* texture = texture_of(targets_.depth->resource);
                if (texture != nullptr) {
                    pass.depthAttachment.texture = texture->texture();
                    pass.depthAttachment.storeAction = MTLStoreActionStore;
                    if (clear_depth) {
                        pass.depthAttachment.loadAction = MTLLoadActionClear;
                        pass.depthAttachment.clearDepth = *clear_depth;
                    } else {
                        pass.depthAttachment.loadAction = MTLLoadActionLoad;
                    }
                    if (!any) {
                        pass_width_ = static_cast<LONG>([texture->texture() width]);
                        pass_height_ = static_cast<LONG>([texture->texture() height]);
                    }
                    any = true;
                }
            }
            if (!any) return false;
            render_ = [commands_ renderCommandEncoderWithDescriptor:pass];
            bound_pipeline_ = nullptr;
            pipeline_dirty_ = true;
            viewport_dirty_ = true;
            dynamic_dirty_ = true;
            bindings_dirty_ = true;
            vertex_dirty_ = true;
            return true;
        }

        void end_render() {
            if (render_ != nil) {
                [render_ endEncoding];
                render_ = nil;
            }
        }
        void end_encoders() {
            end_render();
            end_blit();
            end_compute();
        }
        void end_blit() {
            if (blit_ != nil) {
                [blit_ endEncoding];
                blit_ = nil;
            }
        }
        void end_compute() {
            if (compute_ != nil) {
                [compute_ endEncoding];
                compute_ = nil;
            }
        }
        id<MTLBlitCommandEncoder> blit() {
            end_render();
            end_compute();
            if (blit_ == nil) blit_ = [commands_ blitCommandEncoder];
            return blit_;
        }
        id<MTLComputeCommandEncoder> compute() {
            end_render();
            end_blit();
            if (compute_ == nil) compute_ = [commands_ computeCommandEncoder];
            return compute_;
        }

        struct DepthCopyParams {
            std::uint32_t x, y, width, height, level, offset_words, row_words, mode;
        };

        // A packed-depth texture region -> D3D12 buffer layout.
        void depth_to_buffer(MetalTexture& texture, DepthLayout layout, NSUInteger level, const D3D12_BOX& box,
                             MetalBuffer& buffer, std::uint64_t offset, std::uint32_t row_pitch) {
            id<MTLComputePipelineState> state = backend_.depth_to_buffer_pipeline();
            if (state == nil || (offset % 4u) != 0u || (row_pitch % 4u) != 0u) return;
            const DepthCopyParams p{box.left, box.top, box.right - box.left, box.bottom - box.top,
                                    static_cast<std::uint32_t>(level), static_cast<std::uint32_t>(offset / 4u),
                                    row_pitch / 4u, static_cast<std::uint32_t>(layout)};
            id<MTLComputeCommandEncoder> encoder = compute();
            [encoder setComputePipelineState:state];
            [encoder setTexture:texture.texture() atIndex:0];
            [encoder setBuffer:buffer.buffer() offset:0 atIndex:0];
            [encoder setBytes:&p length:sizeof(p) atIndex:1];
            [encoder dispatchThreads:MTLSizeMake(p.width, p.height, 1)
               threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        }

        // D3D12 buffer layout -> a packed-depth texture region (drawn: Metal
        // cannot write depth textures from compute or blit converted data).
        void depth_from_buffer(MetalTexture& texture, DepthLayout layout, NSUInteger level, MTLOrigin origin,
                               MTLSize size, MetalBuffer& buffer, std::uint64_t offset, std::uint32_t row_pitch) {
            id<MTLRenderPipelineState> state = backend_.depth_from_buffer_pipeline([texture.texture() pixelFormat]);
            if (state == nil || (offset % 4u) != 0u || (row_pitch % 4u) != 0u) return;
            end_encoders();
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.depthAttachment.texture = texture.texture();
            pass.depthAttachment.level = level;
            pass.depthAttachment.loadAction = MTLLoadActionLoad;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            const NSUInteger width = std::max<NSUInteger>([texture.texture() width] >> level, 1u);
            const NSUInteger height = std::max<NSUInteger>([texture.texture() height] >> level, 1u);
            if (origin.x >= width || origin.y >= height) return;
            id<MTLRenderCommandEncoder> encoder = [commands_ renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:state];
            [encoder setDepthStencilState:backend_.depth_state(true)];
            [encoder setViewport:MTLViewport{0.0, 0.0, static_cast<double>(width), static_cast<double>(height), 0.0, 1.0}];
            [encoder setScissorRect:MTLScissorRect{origin.x, origin.y, std::min(size.width, width - origin.x),
                                                   std::min(size.height, height - origin.y)}];
            const DepthCopyParams p{static_cast<std::uint32_t>(origin.x), static_cast<std::uint32_t>(origin.y),
                                    static_cast<std::uint32_t>(size.width), static_cast<std::uint32_t>(size.height),
                                    static_cast<std::uint32_t>(level), static_cast<std::uint32_t>(offset / 4u),
                                    row_pitch / 4u, static_cast<std::uint32_t>(layout)};
            [encoder setFragmentBuffer:buffer.buffer() offset:0 atIndex:0];
            [encoder setFragmentBytes:&p length:sizeof(p) atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        }

        // -- clears
        // Whole-target clears are pass load actions; rectangles draw.
        void apply(const cmd::ClearRenderTarget& c) {
            if (c.rects.empty()) {
                const cmd::SetRenderTargets saved = targets_;
                targets_ = cmd::SetRenderTargets{{c.target}, std::nullopt};
                begin_render(c.color, 0);
                end_render();
                targets_ = saved;
                return;
            }
            clear_rects(c.target, &c.color, std::nullopt, c.rects);
        }
        void apply(const cmd::ClearDepthStencil& c) {
            if ((c.flags & D3D12_CLEAR_FLAG_DEPTH) == 0) return;  // no stencil here
            if (c.rects.empty()) {
                const cmd::SetRenderTargets saved = targets_;
                targets_ = cmd::SetRenderTargets{{}, c.target};
                begin_render(std::nullopt, -1, c.depth);
                end_render();
                targets_ = saved;
                return;
            }
            clear_rects(c.target, nullptr, c.depth, c.rects);
        }
        void clear_rects(const Descriptor& target, const std::array<float, 4>* color,
                         std::optional<float> depth, const std::vector<D3D12_RECT>& rects) {
            const cmd::SetRenderTargets saved = targets_;
            if (color != nullptr) {
                targets_ = cmd::SetRenderTargets{{target}, std::nullopt};
            } else {
                targets_ = cmd::SetRenderTargets{{}, target};
            }
            if (!begin_render()) {
                targets_ = saved;
                return;
            }
            std::array<MTLPixelFormat, 8> formats{};
            formats.fill(MTLPixelFormatInvalid);
            MTLPixelFormat depth_format = MTLPixelFormatInvalid;
            if (color != nullptr) {
                formats[0] = [texture_of(target.resource)->texture() pixelFormat];
            } else {
                depth_format = [texture_of(target.resource)->texture() pixelFormat];
            }
            [render_ setRenderPipelineState:backend_.clear_pipeline(formats, color != nullptr ? 1u : 0u, depth_format)];
            [render_ setDepthStencilState:backend_.depth_state(depth.has_value())];
            [render_ setCullMode:MTLCullModeNone];
            struct { float color[4]; float depth; float pad[3]; } values{};
            if (color != nullptr) std::copy(color->begin(), color->end(), values.color);
            values.depth = depth.value_or(0.0f);
            [render_ setVertexBytes:&values length:sizeof(values) atIndex:0];
            [render_ setFragmentBytes:&values length:sizeof(values) atIndex:0];
            MTLViewport full{0.0, 0.0, static_cast<double>(pass_width_), static_cast<double>(pass_height_), 0.0, 1.0};
            [render_ setViewport:full];
            for (const D3D12_RECT& r : rects) {
                const LONG x0 = std::clamp<LONG>(r.left, 0, pass_width_);
                const LONG y0 = std::clamp<LONG>(r.top, 0, pass_height_);
                const LONG x1 = std::clamp<LONG>(r.right, x0, pass_width_);
                const LONG y1 = std::clamp<LONG>(r.bottom, y0, pass_height_);
                if (x1 <= x0 || y1 <= y0) continue;
                [render_ setScissorRect:MTLScissorRect{static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0),
                                                       static_cast<NSUInteger>(x1 - x0),
                                                       static_cast<NSUInteger>(y1 - y0)}];
                [render_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            end_render();
            targets_ = saved;
        }

        // -- copies
        void apply(const cmd::CopyBufferRegion& c) {
            MetalBuffer* dst = buffer_of(c.dst);
            MetalBuffer* src = buffer_of(c.src);
            if (dst == nullptr || src == nullptr || c.bytes == 0u) return;
            [blit() copyFromBuffer:src->buffer() sourceOffset:c.src_offset toBuffer:dst->buffer()
                 destinationOffset:c.dst_offset size:c.bytes];
        }
        void apply(const cmd::CopyResource& c) {
            if (c.dst == nullptr || c.src == nullptr) return;
            if (c.dst->is_buffer() && c.src->is_buffer()) {
                [blit() copyFromBuffer:buffer_of(c.src)->buffer() sourceOffset:0 toBuffer:buffer_of(c.dst)->buffer()
                     destinationOffset:0 size:std::min(c.dst->desc().Width, c.src->desc().Width)];
                return;
            }
            MetalTexture* dst = texture_of(c.dst);
            MetalTexture* src = texture_of(c.src);
            if (dst != nullptr && src != nullptr) [blit() copyFromTexture:src->texture() toTexture:dst->texture()];
        }
        void apply(const cmd::CopyTextureRegion& c) {
            auto* dst_resource = static_cast<Resource*>(c.dst.pResource);
            auto* src_resource = static_cast<Resource*>(c.src.pResource);
            if (dst_resource == nullptr || src_resource == nullptr) return;
            const bool dst_buffer = c.dst.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            const bool src_buffer = c.src.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            if (src_buffer && !dst_buffer) {
                MetalTexture* texture = texture_of(dst_resource);
                MetalBuffer* buffer = buffer_of(src_resource);
                if (texture == nullptr || buffer == nullptr) return;
                const auto& fp = c.src.PlacedFootprint;
                const FormatInfo info = format_info(fp.Footprint.Format);
                D3D12_BOX box{0, 0, 0, fp.Footprint.Width, fp.Footprint.Height, fp.Footprint.Depth};
                if (c.src_box) box = *c.src_box;
                const NSUInteger mips = std::max<NSUInteger>([texture->texture() mipmapLevelCount], 1u);
                const UINT subresource = c.dst.SubresourceIndex;
                const std::uint64_t rows = (fp.Footprint.Height + info.block_height - 1u) / info.block_height;
                const std::uint64_t offset = fp.Offset + std::uint64_t{box.front} * fp.Footprint.RowPitch * rows +
                                             (box.top / info.block_height) * std::uint64_t{fp.Footprint.RowPitch} +
                                             (box.left / info.block_width) * std::uint64_t{info.bytes_per_block};
                if (const DepthLayout layout = depth_layout(texture->desc().Format);
                    is_depth_format([texture->texture() pixelFormat]) && layout != DepthLayout::Native) {
                    depth_from_buffer(*texture, layout, subresource % mips, MTLOriginMake(c.dst_x, c.dst_y, 0),
                                      MTLSizeMake(box.right - box.left, box.bottom - box.top, 1), *buffer, offset,
                                      fp.Footprint.RowPitch);
                    return;
                }
                [blit() copyFromBuffer:buffer->buffer()
                          sourceOffset:offset
                     sourceBytesPerRow:fp.Footprint.RowPitch
                   sourceBytesPerImage:fp.Footprint.RowPitch * rows
                            sourceSize:MTLSizeMake(box.right - box.left, box.bottom - box.top, box.back - box.front)
                             toTexture:texture->texture()
                      destinationSlice:subresource / mips
                      destinationLevel:subresource % mips
                     destinationOrigin:MTLOriginMake(c.dst_x, c.dst_y, c.dst_z)];
                return;
            }
            if (!src_buffer && dst_buffer) {
                MetalTexture* texture = texture_of(src_resource);
                MetalBuffer* buffer = buffer_of(dst_resource);
                if (texture == nullptr || buffer == nullptr) return;
                const auto& fp = c.dst.PlacedFootprint;
                const FormatInfo info = format_info(fp.Footprint.Format);
                const NSUInteger mips = std::max<NSUInteger>([texture->texture() mipmapLevelCount], 1u);
                const UINT subresource = c.src.SubresourceIndex;
                const NSUInteger level = subresource % mips;
                D3D12_BOX box{0, 0, 0,
                              static_cast<UINT>(std::max<NSUInteger>([texture->texture() width] >> level, 1u)),
                              static_cast<UINT>(std::max<NSUInteger>([texture->texture() height] >> level, 1u)), 1u};
                if (c.src_box) box = *c.src_box;
                const std::uint64_t rows = (fp.Footprint.Height + info.block_height - 1u) / info.block_height;
                const std::uint64_t offset = fp.Offset + std::uint64_t{c.dst_z} * fp.Footprint.RowPitch * rows +
                                             (c.dst_y / info.block_height) * std::uint64_t{fp.Footprint.RowPitch} +
                                             (c.dst_x / info.block_width) * std::uint64_t{info.bytes_per_block};
                if (const DepthLayout layout = depth_layout(texture->desc().Format);
                    is_depth_format([texture->texture() pixelFormat]) && layout != DepthLayout::Native) {
                    depth_to_buffer(*texture, layout, level, box, *buffer, offset, fp.Footprint.RowPitch);
                    return;
                }
                [blit() copyFromTexture:texture->texture()
                            sourceSlice:subresource / mips
                            sourceLevel:level
                           sourceOrigin:MTLOriginMake(box.left, box.top, box.front)
                             sourceSize:MTLSizeMake(box.right - box.left, box.bottom - box.top, box.back - box.front)
                               toBuffer:buffer->buffer()
                      destinationOffset:offset
                 destinationBytesPerRow:fp.Footprint.RowPitch
               destinationBytesPerImage:fp.Footprint.RowPitch * rows];
                return;
            }
            if (!src_buffer && !dst_buffer) {
                MetalTexture* dst = texture_of(dst_resource);
                MetalTexture* src = texture_of(src_resource);
                if (dst == nullptr || src == nullptr) return;
                const NSUInteger src_mips = std::max<NSUInteger>([src->texture() mipmapLevelCount], 1u);
                const NSUInteger dst_mips = std::max<NSUInteger>([dst->texture() mipmapLevelCount], 1u);
                const NSUInteger level = c.src.SubresourceIndex % src_mips;
                D3D12_BOX box{0, 0, 0,
                              static_cast<UINT>(std::max<NSUInteger>([src->texture() width] >> level, 1u)),
                              static_cast<UINT>(std::max<NSUInteger>([src->texture() height] >> level, 1u)), 1u};
                if (c.src_box) box = *c.src_box;
                if (box.right <= box.left || box.bottom <= box.top) return;
                id<MTLTexture> source = src->texture();
                // Copies need equal formats; reinterpret through a view when
                // the D3D12 formats were cast-compatible.
                if ([source pixelFormat] != [dst->texture() pixelFormat]) {
                    source = src->view([dst->texture() pixelFormat], 0u, 0u);
                }
                [blit() copyFromTexture:source
                            sourceSlice:c.src.SubresourceIndex / src_mips
                            sourceLevel:level
                           sourceOrigin:MTLOriginMake(box.left, box.top, box.front)
                             sourceSize:MTLSizeMake(box.right - box.left, box.bottom - box.top, box.back - box.front)
                              toTexture:dst->texture()
                       destinationSlice:c.dst.SubresourceIndex / dst_mips
                       destinationLevel:c.dst.SubresourceIndex % dst_mips
                      destinationOrigin:MTLOriginMake(c.dst_x, c.dst_y, c.dst_z)];
            }
        }
        void apply(const cmd::ResolveQueries& c) {
            MetalBuffer* buffer = buffer_of(c.dst);
            if (buffer == nullptr || c.count == 0u) return;
            [blit() fillBuffer:buffer->buffer() range:NSMakeRange(c.dst_offset, std::size_t{c.count} * 8u) value:0];
        }

        void report(const char* what, const std::string& error) {
            if (reported_ < 8u) {
                ++reported_;
                std::cerr << "[metal] " << what << ": " << error << '\n';
            }
        }

        MetalBackend& backend_;
        id<MTLCommandBuffer> commands_;
        id<MTLRenderCommandEncoder> render_ = nil;
        id<MTLBlitCommandEncoder> blit_ = nil;
        id<MTLComputeCommandEncoder> compute_ = nil;
        LONG pass_width_ = 0;
        LONG pass_height_ = 0;

        PipelineState* pipeline_ = nullptr;
        MetalPipeline* bound_pipeline_ = nullptr;
        std::array<std::uint32_t, 8> bound_strides_{};
        RootSignature* signature_ = nullptr;
        std::array<const Descriptor*, 64> tables_{};
        std::array<std::uint64_t, 64> views_{};
        std::array<std::vector<std::uint32_t>, 64> constants_{};
        D3D12_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        D3D12_VIEWPORT viewport_{};
        D3D12_RECT scissor_{};
        bool has_viewport_ = false;
        bool has_scissor_ = false;
        std::array<float, 4> blend_factor_{1.0f, 1.0f, 1.0f, 1.0f};
        UINT stencil_ref_ = 0u;
        std::array<D3D12_VERTEX_BUFFER_VIEW, 8> vertex_buffers_{};
        D3D12_INDEX_BUFFER_VIEW index_buffer_{};
        cmd::SetRenderTargets targets_;
        bool pipeline_dirty_ = true;
        bool viewport_dirty_ = true;
        bool dynamic_dirty_ = true;
        bool bindings_dirty_ = true;
        bool vertex_dirty_ = true;
        unsigned reported_ = 0u;
    };

    id<MTLDevice> device_;
    id<MTLCommandQueue> queue_;
    std::mutex encode_mutex_;
    std::mutex completion_mutex_;
    std::uint64_t submitted_serial_ = 0u;
    std::uint64_t completed_serial_ = 0u;
    std::deque<std::pair<std::uint64_t, std::function<void()>>> callbacks_;
    std::mutex library_mutex_;
    std::unordered_map<std::uint64_t, CompiledShader> functions_;
    id<MTLLibrary> internal_library_ = nil;
    id<MTLRenderPipelineState> present_pipeline_ = nil;
    std::mutex sampler_mutex_;
    std::unordered_map<std::uint64_t, id<MTLSamplerState>> samplers_;
    std::mutex clear_mutex_;
    std::unordered_map<std::uint64_t, id<MTLRenderPipelineState>> clear_pipelines_;
    id<MTLDepthStencilState> depth_write_always_ = nil;
    id<MTLDepthStencilState> depth_disabled_ = nil;
    id<MTLComputePipelineState> depth_to_buffer_ = nil;
    id<MTLRenderPipelineState> depth_from_buffer_ = nil;
    std::atomic<bool> logic_op_reported_{false};
};

}  // namespace

std::unique_ptr<Backend> create_backend() {
    const char* const choice = std::getenv("GALAXY_D3D12_BACKEND");
    if (choice != nullptr && std::strcmp(choice, "cpu") == 0) return create_cpu_backend();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        std::cerr << "[metal] no Metal device; using the CPU backend (no rendering)\n";
        return create_cpu_backend();
    }
    return std::make_unique<MetalBackend>(device);
}

}  // namespace galaxy::d3d12
