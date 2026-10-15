// SPDX-License-Identifier: GPL-3.0-only
// Blobs, D3DCompile/D3DReflect and root-signature serialization for the
// D3D12 layer. See layer.h.

#include "layer.h"

#include "galaxy/gx/shader_translate.h"

#include <d3dcompiler.h>

#include <cstring>
#include <iostream>
#include <string_view>

namespace galaxy::d3d12 {

// ---------------------------------------------------------------------------
// Blobs
// ---------------------------------------------------------------------------
class Blob final : public Unknown<ID3D10Blob> {
public:
    explicit Blob(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return bytes_.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes_.size(); }

private:
    std::vector<std::uint8_t> bytes_;
};

namespace {

ID3DBlob* make_blob(std::vector<std::uint8_t> bytes) { return new Blob(std::move(bytes)); }

ID3DBlob* text_blob(std::string_view text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0u);
    return make_blob(std::move(bytes));
}

// Little-endian writer/reader for the blob formats below.
class Writer {
public:
    void u32(std::uint32_t value) {
        for (unsigned i = 0; i < 4u; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
    }
    void raw(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    void str(std::string_view text) {
        u32(static_cast<std::uint32_t>(text.size()));
        raw(text.data(), text.size());
    }
    std::vector<std::uint8_t> bytes;
};

class Reader {
public:
    Reader(const void* data, std::size_t size)
        : p_(static_cast<const std::uint8_t*>(data)), end_(p_ + size) {}
    bool u32(std::uint32_t& value) {
        if (end_ - p_ < 4) return false;
        value = std::uint32_t{p_[0]} | std::uint32_t{p_[1]} << 8u | std::uint32_t{p_[2]} << 16u |
                std::uint32_t{p_[3]} << 24u;
        p_ += 4;
        return true;
    }
    bool raw(void* out, std::size_t size) {
        if (static_cast<std::size_t>(end_ - p_) < size) return false;
        std::memcpy(out, p_, size);
        p_ += size;
        return true;
    }
    bool str(std::string& text) {
        std::uint32_t size = 0;
        if (!u32(size) || static_cast<std::size_t>(end_ - p_) < size) return false;
        text.assign(reinterpret_cast<const char*>(p_), size);
        p_ += size;
        return true;
    }
    [[nodiscard]] bool done() const noexcept { return p_ == end_; }

private:
    const std::uint8_t* p_;
    const std::uint8_t* end_;
};

constexpr std::uint32_t kShaderMagic = 0x48535847u;      // "GXSH"
constexpr std::uint32_t kSourceShaderMagic = 0x53485847u;  // "GXHS"
constexpr std::uint32_t kRootSignatureMagic = 0x53525847u;  // "GXRS"
constexpr std::uint32_t kFormatVersion = 1u;

}  // namespace

// ---------------------------------------------------------------------------
// Shader blobs
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> encode_shader(const ShaderProgram& program) {
    Writer w;
    w.u32(kShaderMagic);
    w.u32(kFormatVersion);
    w.u32(static_cast<std::uint32_t>(program.stage));
    w.str(program.entry_point);
    w.str(program.label);
    w.u32(static_cast<std::uint32_t>(program.spirv.size()));
    w.raw(program.spirv.data(), program.spirv.size() * sizeof(std::uint32_t));
    // Padded to 256 bytes, so shaders differing only in a constant (SPIR-V
    // deduplicates equal constants) have equal-sized blobs, as DXBC does;
    // the pipeline cache's record format tests rely on it.
    w.u32(0u);  // padding byte count follows
    const std::size_t padding = (256u - (w.bytes.size() % 256u)) % 256u;
    w.bytes[w.bytes.size() - 4u] = static_cast<std::uint8_t>(padding);
    w.bytes.resize(w.bytes.size() + padding, 0u);
    return std::move(w.bytes);
}

namespace {
// "GXHS": HLSL source, compiled when decoded (cmake/hlsl_source_blob.cmake
// writes these for the post-processing passes instead of fxc headers).
bool decode_source_shader(Reader& r, ShaderProgram& program) {
    std::uint32_t version = 0, stage = 0;
    std::string source;
    if (!r.u32(version) || version != kFormatVersion || !r.u32(stage) ||
        stage > static_cast<std::uint32_t>(ShaderStage::Pixel) || !r.str(program.entry_point) ||
        !r.str(program.label) || !r.str(source) || !r.done()) {
        return false;
    }
    program.stage = static_cast<ShaderStage>(stage);
    galaxy::gx::xlate::Options options;
    options.entry_point = program.entry_point;
    options.label = program.label;
    galaxy::gx::xlate::SpirvResult spirv = galaxy::gx::xlate::hlsl_to_spirv(
        source,
        program.stage == ShaderStage::Vertex ? galaxy::gx::xlate::Stage::Vertex
                                             : galaxy::gx::xlate::Stage::Pixel,
        options);
    if (!spirv.ok) {
        std::cerr << "[d3d12] " << program.label << ": " << spirv.log << '\n';
        return false;
    }
    program.spirv = std::move(spirv.words);
    return true;
}
}  // namespace

bool decode_shader(const void* data, std::size_t size, ShaderProgram& program) {
    Reader r(data, size);
    std::uint32_t magic = 0, version = 0, stage = 0, words = 0;
    if (!r.u32(magic)) return false;
    if (magic == kSourceShaderMagic) return decode_source_shader(r, program);
    if (magic != kShaderMagic || !r.u32(version) || version != kFormatVersion ||
        !r.u32(stage) || stage > static_cast<std::uint32_t>(ShaderStage::Compute) ||
        !r.str(program.entry_point) || !r.str(program.label) || !r.u32(words)) {
        return false;
    }
    program.stage = static_cast<ShaderStage>(stage);
    program.spirv.resize(words);
    std::uint32_t padding = 0u;
    if (!r.raw(program.spirv.data(), std::size_t{words} * sizeof(std::uint32_t)) ||
        !r.u32(padding) || padding > 255u) {
        return false;
    }
    std::vector<std::uint8_t> tail(padding);
    return r.raw(tail.data(), padding) && r.done();
}

// ---------------------------------------------------------------------------
// Root signatures
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC& desc) {
    Writer w;
    w.u32(kRootSignatureMagic);
    w.u32(kFormatVersion);
    w.u32(static_cast<std::uint32_t>(desc.Flags));
    w.u32(desc.NumParameters);
    for (UINT i = 0; i < desc.NumParameters; ++i) {
        const D3D12_ROOT_PARAMETER& p = desc.pParameters[i];
        w.u32(static_cast<std::uint32_t>(p.ParameterType));
        w.u32(static_cast<std::uint32_t>(p.ShaderVisibility));
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            w.u32(p.DescriptorTable.NumDescriptorRanges);
            UINT next = 0u;
            for (UINT r = 0; r < p.DescriptorTable.NumDescriptorRanges; ++r) {
                const D3D12_DESCRIPTOR_RANGE& range = p.DescriptorTable.pDescriptorRanges[r];
                const UINT offset = range.OffsetInDescriptorsFromTableStart ==
                                            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                        ? next
                                        : range.OffsetInDescriptorsFromTableStart;
                w.u32(static_cast<std::uint32_t>(range.RangeType));
                w.u32(range.NumDescriptors);
                w.u32(range.BaseShaderRegister);
                w.u32(range.RegisterSpace);
                w.u32(offset);
                next = range.NumDescriptors == UINT_MAX ? offset : offset + range.NumDescriptors;
            }
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            w.u32(p.Constants.ShaderRegister);
            w.u32(p.Constants.RegisterSpace);
            w.u32(p.Constants.Num32BitValues);
            break;
        default:
            w.u32(p.Descriptor.ShaderRegister);
            w.u32(p.Descriptor.RegisterSpace);
            break;
        }
    }
    w.u32(desc.NumStaticSamplers);
    for (UINT i = 0; i < desc.NumStaticSamplers; ++i) {
        w.raw(&desc.pStaticSamplers[i], sizeof(D3D12_STATIC_SAMPLER_DESC));
    }
    return std::move(w.bytes);
}

bool deserialize_root_signature(std::span<const std::uint8_t> bytes, RootSignatureLayout& layout) {
    Reader r(bytes.data(), bytes.size());
    std::uint32_t magic = 0, version = 0, flags = 0, count = 0;
    if (!r.u32(magic) || magic != kRootSignatureMagic || !r.u32(version) ||
        version != kFormatVersion || !r.u32(flags) || !r.u32(count) || count > 64u) {
        return false;
    }
    layout.flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags);
    for (std::uint32_t i = 0; i < count; ++i) {
        RootParameter p;
        std::uint32_t type = 0, visibility = 0;
        if (!r.u32(type) || !r.u32(visibility)) return false;
        p.type = static_cast<D3D12_ROOT_PARAMETER_TYPE>(type);
        p.visibility = static_cast<D3D12_SHADER_VISIBILITY>(visibility);
        if (p.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            std::uint32_t ranges = 0;
            if (!r.u32(ranges) || ranges > 64u) return false;
            for (std::uint32_t k = 0; k < ranges; ++k) {
                std::uint32_t range_type = 0;
                DescriptorRange range{};
                if (!r.u32(range_type) || !r.u32(range.count) || !r.u32(range.base_register) ||
                    !r.u32(range.space) || !r.u32(range.offset)) {
                    return false;
                }
                range.type = static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(range_type);
                p.ranges.push_back(range);
            }
        } else if (p.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if (!r.u32(p.shader_register) || !r.u32(p.space) || !r.u32(p.num_constants)) return false;
        } else {
            if (!r.u32(p.shader_register) || !r.u32(p.space)) return false;
        }
        layout.parameters.push_back(std::move(p));
    }
    std::uint32_t samplers = 0;
    if (!r.u32(samplers) || samplers > 64u) return false;
    layout.static_samplers.resize(samplers);
    for (auto& sampler : layout.static_samplers) {
        if (!r.raw(&sampler, sizeof(sampler))) return false;
    }
    return r.done();
}

// ---------------------------------------------------------------------------
// Reflection (program type only)
// ---------------------------------------------------------------------------
class ShaderReflection final : public Unknown<ID3D12ShaderReflection> {
public:
    explicit ShaderReflection(ShaderStage stage) : stage_(stage) {}
    HRESULT STDMETHODCALLTYPE GetDesc(D3D12_SHADER_DESC* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = {};
        // D3D12_SHVER_*: pixel 0, vertex 1, geometry 2, compute 5; 5.1.
        const UINT type = stage_ == ShaderStage::Pixel    ? 0u
                          : stage_ == ShaderStage::Vertex ? 1u
                          : stage_ == ShaderStage::Geometry ? 2u
                                                            : 5u;
        desc->Version = (type << 16u) | (5u << 4u) | 1u;
        return S_OK;
    }
    ID3D12ShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByIndex(UINT) override {
        return nullptr;
    }
    ID3D12ShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByName(LPCSTR) override {
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetResourceBindingDesc(UINT, D3D12_SHADER_INPUT_BIND_DESC*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetInputParameterDesc(UINT, D3D12_SIGNATURE_PARAMETER_DESC*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetOutputParameterDesc(UINT, D3D12_SIGNATURE_PARAMETER_DESC*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPatchConstantParameterDesc(UINT,
                                                            D3D12_SIGNATURE_PARAMETER_DESC*) override {
        return E_NOTIMPL;
    }
    ID3D12ShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByName(LPCSTR) override {
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetResourceBindingDescByName(LPCSTR,
                                                           D3D12_SHADER_INPUT_BIND_DESC*) override {
        return E_NOTIMPL;
    }
    UINT STDMETHODCALLTYPE GetMovInstructionCount() override { return 0u; }
    UINT STDMETHODCALLTYPE GetMovcInstructionCount() override { return 0u; }
    UINT STDMETHODCALLTYPE GetConversionInstructionCount() override { return 0u; }
    UINT STDMETHODCALLTYPE GetBitwiseInstructionCount() override { return 0u; }
    D3D_PRIMITIVE STDMETHODCALLTYPE GetGSInputPrimitive() override { return D3D_PRIMITIVE_UNDEFINED; }
    BOOL STDMETHODCALLTYPE IsSampleFrequencyShader() override { return FALSE; }
    UINT STDMETHODCALLTYPE GetNumInterfaceSlots() override { return 0u; }
    HRESULT STDMETHODCALLTYPE GetMinFeatureLevel(D3D_FEATURE_LEVEL* level) override {
        if (level != nullptr) *level = D3D_FEATURE_LEVEL_11_0;
        return S_OK;
    }
    UINT STDMETHODCALLTYPE GetThreadGroupSize(UINT* x, UINT* y, UINT* z) override {
        if (x != nullptr) *x = 0u;
        if (y != nullptr) *y = 0u;
        if (z != nullptr) *z = 0u;
        return 0u;
    }
    UINT64 STDMETHODCALLTYPE GetRequiresFlags() override { return 0u; }

private:
    ShaderStage stage_;
};

}  // namespace galaxy::d3d12

using namespace galaxy::d3d12;

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
HRESULT D3DCreateBlob(SIZE_T size, ID3DBlob** blob) {
    if (blob == nullptr) return E_INVALIDARG;
    try {
        *blob = make_blob(std::vector<std::uint8_t>(size));
        return S_OK;
    } catch (const std::bad_alloc&) {
        *blob = nullptr;
        return E_OUTOFMEMORY;
    }
}

HRESULT D3DCompile(const void* source, SIZE_T source_size, const char* source_name,
                   const D3D_SHADER_MACRO* defines, ID3DInclude*, const char* entry_point,
                   const char* target, UINT, UINT, ID3DBlob** code, ID3DBlob** errors) {
    if (code != nullptr) *code = nullptr;
    if (errors != nullptr) *errors = nullptr;
    if (source == nullptr || target == nullptr || entry_point == nullptr || code == nullptr) {
        return E_INVALIDARG;
    }
    ShaderProgram program;
    program.entry_point = entry_point;
    program.label = source_name != nullptr ? source_name : "";
    galaxy::gx::xlate::Stage stage = galaxy::gx::xlate::Stage::Vertex;
    switch (target[0]) {
    case 'v': program.stage = ShaderStage::Vertex; stage = galaxy::gx::xlate::Stage::Vertex; break;
    case 'p': program.stage = ShaderStage::Pixel; stage = galaxy::gx::xlate::Stage::Pixel; break;
    case 'c': program.stage = ShaderStage::Compute; stage = galaxy::gx::xlate::Stage::Compute; break;
    case 'g': program.stage = ShaderStage::Geometry; break;
    default: return E_INVALIDARG;
    }
    if (program.stage != ShaderStage::Geometry) {
        galaxy::gx::xlate::Options options;
        options.entry_point = entry_point;
        options.label = program.label;
        for (const D3D_SHADER_MACRO* m = defines; m != nullptr && m->Name != nullptr; ++m) {
            options.defines.emplace_back(m->Name, m->Definition != nullptr ? m->Definition : "");
        }
        galaxy::gx::xlate::SpirvResult spirv = galaxy::gx::xlate::hlsl_to_spirv(
            std::string_view(static_cast<const char*>(source), source_size), stage, options);
        if (!spirv.ok) {
            if (errors != nullptr) *errors = text_blob(spirv.log);
            return E_FAIL;
        }
        program.spirv = std::move(spirv.words);
    }
    // A geometry shader is kept without SPIR-V: Metal has no geometry stage,
    // and pipelines with one draw their lines and points natively.
    *code = make_blob(encode_shader(program));
    return S_OK;
}

HRESULT D3DReflect(const void* data, SIZE_T size, REFIID riid, void** reflector) {
    if (reflector == nullptr) return E_INVALIDARG;
    *reflector = nullptr;
    ShaderProgram program;
    if (data == nullptr || !decode_shader(data, size, program)) return E_INVALIDARG;
    auto* reflection = new ShaderReflection(program.stage);
    const HRESULT hr = reflection->QueryInterface(riid, reflector);
    reflection->Release();
    return hr;
}

HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                           D3D_ROOT_SIGNATURE_VERSION, ID3DBlob** blob,
                                           ID3DBlob** error) {
    if (error != nullptr) *error = nullptr;
    if (desc == nullptr || blob == nullptr) return E_INVALIDARG;
    *blob = make_blob(serialize_root_signature(*desc));
    return S_OK;
}
