// SPDX-License-Identifier: GPL-3.0-only
// HLSL -> SPIR-V (glslang) -> MSL (SPIRV-Cross). See shader_translate.h.

#include "galaxy/gx/shader_translate.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_msl.hpp>

#include <cctype>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace galaxy::gx::xlate {
namespace {

void initialize_glslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

EShLanguage glslang_stage(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return EShLangVertex;
    case Stage::Pixel: return EShLangFragment;
    case Stage::Compute: return EShLangCompute;
    }
    return EShLangVertex;
}

spv::ExecutionModel execution_model(Stage stage) {
    switch (stage) {
    case Stage::Vertex: return spv::ExecutionModelVertex;
    case Stage::Pixel: return spv::ExecutionModelFragment;
    case Stage::Compute: return spv::ExecutionModelGLCompute;
    }
    return spv::ExecutionModelVertex;
}

// "TEXCOORD3" -> {"TEXCOORD", 3}; "COLOR" -> {"COLOR", 0}.
std::pair<std::string, unsigned> split_semantic(const std::string& semantic) {
    std::size_t digits = semantic.size();
    while (digits > 0u && std::isdigit(static_cast<unsigned char>(semantic[digits - 1u]))) {
        --digits;
    }
    std::string base = semantic.substr(0, digits);
    for (char& c : base) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    unsigned index = 0u;
    for (std::size_t i = digits; i < semantic.size(); ++i) {
        index = index * 10u + static_cast<unsigned>(semantic[i] - '0');
    }
    return {base, index};
}

// Interstage location for a semantic: fixed for the GX varyings, a stable
// hash for anything else, so separately compiled stages agree.
std::uint32_t interstage_location(const std::string& semantic) {
    const auto [base, index] = split_semantic(semantic);
    if (base == "COLOR") return index;               // 0..1
    if (base == "TEXCOORD") return 2u + index;       // 2..17
    std::uint32_t hash = 2166136261u;
    for (const char c : base) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    return 18u + (hash % 7u) * 2u + index;           // 18..31
}

// glslang splits an IO array ("float3 uv[8] : TEXCOORD") into one variable
// per element, each carrying the array's semantic. Gives every variable its
// full semantic ("TEXCOORD0".."TEXCOORD7"): an element's index is its
// location's distance from the first element's.
struct IoVariable {
    spirv_cross::ID id;
    std::uint32_t location;
    std::string semantic;
};
std::vector<IoVariable> io_variables(
    const spirv_cross::Compiler& compiler,
    const spirv_cross::SmallVector<spirv_cross::Resource>& resources) {
    std::vector<IoVariable> variables;
    std::map<std::string, std::uint32_t> first_location;
    for (const auto& resource : resources) {
        IoVariable variable{resource.id,
                            compiler.get_decoration(resource.id, spv::DecorationLocation),
                            compiler.get_decoration_string(resource.id, spv::DecorationUserSemantic)};
        if (variable.semantic.empty()) continue;
        auto [it, inserted] = first_location.emplace(variable.semantic, variable.location);
        if (!inserted && variable.location < it->second) it->second = variable.location;
        variables.push_back(std::move(variable));
    }
    for (IoVariable& variable : variables) {
        // Element i of an array declared at SEMANTICn has SEMANTIC(n + i);
        // a plain "COLOR" on a single variable is COLOR0.
        const std::uint32_t element = variable.location - first_location[variable.semantic];
        const auto [base, index] = split_semantic(variable.semantic);
        variable.semantic = base + std::to_string(index + element);
    }
    return variables;
}

std::string preamble(const Options& options) {
    // D3DCOMPILE_PACK_MATRIX_ROW_MAJOR. glslang's HLSL front end reverses
    // the sense of this pragma internally to match SPIR-V.
    std::string text = "#pragma pack_matrix(row_major)\n";
    for (const auto& [name, value] : options.defines) {
        text += "#define " + name + (value.empty() ? std::string{} : " " + value) + "\n";
    }
    return text;
}

}  // namespace

SpirvResult hlsl_to_spirv(std::string_view hlsl, Stage stage, const Options& options) {
    SpirvResult result;
    initialize_glslang();
    try {
        const EShLanguage language = glslang_stage(stage);
        glslang::TShader shader(language);
        const char* const sources[] = {hlsl.data()};
        const int lengths[] = {static_cast<int>(hlsl.size())};
        const char* const names[] = {options.label.empty() ? "shader" : options.label.c_str()};
        shader.setStringsWithLengthsAndNames(sources, lengths, names, 1);
        const std::string pre = preamble(options);
        shader.setPreamble(pre.c_str());
        shader.setEntryPoint(options.entry_point.c_str());
        shader.setSourceEntryPoint(options.entry_point.c_str());
        shader.setEnvInput(glslang::EShSourceHlsl, language, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        // Keeps HLSL semantics in the SPIR-V (UserSemantic decorations).
        shader.setEnvTargetHlslFunctionality1();
        const auto messages = static_cast<EShMessages>(
            EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
        if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
            result.log = std::string("HLSL: ") + shader.getInfoLog() + shader.getInfoDebugLog();
            return result;
        }
        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages)) {
            result.log = std::string("link: ") + program.getInfoLog() + program.getInfoDebugLog();
            return result;
        }
        glslang::SpvOptions spv_options;
        spv_options.disableOptimizer = true;
        spv_options.validate = false;
        glslang::GlslangToSpv(*program.getIntermediate(language), result.words, &spv_options);
        if (result.words.empty()) {
            result.log = "SPIR-V generation produced no code";
            return result;
        }
        result.ok = true;
    } catch (const std::exception& error) {
        result.log += std::string("SPIR-V: ") + error.what();
        result.ok = false;
    }
    return result;
}

Result spirv_to_msl(std::span<const std::uint32_t> words, Stage stage, const Options& options) {
    Result result;
    try {
        spirv_cross::CompilerMSL msl(words.data(), words.size());
        spirv_cross::CompilerMSL::Options msl_options;
        msl_options.platform = options.platform == Platform::iOS
            ? spirv_cross::CompilerMSL::Options::iOS
            : spirv_cross::CompilerMSL::Options::macOS;
        msl_options.set_msl_version(2, 3);
        msl_options.pad_fragment_output_components = true;
        msl_options.enable_point_size_builtin = options.points;
        msl_options.enable_point_size_default = options.points;
        msl_options.default_point_size = 1.0f;
        msl.set_msl_options(msl_options);
        spirv_cross::CompilerGLSL::Options common = msl.get_common_options();
        common.vertex.flip_vert_y = false;  // D3D and Metal share clip space
        msl.set_common_options(common);

        const spv::ExecutionModel model = execution_model(stage);
        msl.set_entry_point(options.entry_point, model);
        const spirv_cross::ShaderResources resources = msl.get_shader_resources();

        // Bindings by D3D12 register (see the header).
        std::set<std::pair<std::uint32_t, std::uint32_t>> bound;
        auto bind = [&](const spirv_cross::Resource& resource, bool storage_image) {
            const std::uint32_t set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
            const std::uint32_t binding = msl.get_decoration(resource.id, spv::DecorationBinding);
            if (!bound.insert({set, binding + (storage_image ? 0x10000u : 0u)}).second) return;
            spirv_cross::MSLResourceBinding entry;
            entry.stage = model;
            entry.desc_set = set;
            entry.binding = binding;
            entry.msl_buffer = set == 0u ? binding : kSpace1BufferBase + binding;
            // UAV textures (uN) follow the SRVs (tN) in Metal's texture table.
            entry.msl_texture = (storage_image ? 16u : 0u) + binding + (set == 0u ? 0u : 24u);
            entry.msl_sampler = binding;
            msl.add_msl_resource_binding(entry);
        };
        for (const auto& r : resources.uniform_buffers) bind(r, false);
        for (const auto& r : resources.storage_buffers) bind(r, false);
        for (const auto& r : resources.separate_images) bind(r, false);
        for (const auto& r : resources.separate_samplers) bind(r, false);
        for (const auto& r : resources.sampled_images) bind(r, false);
        for (const auto& r : resources.storage_images) bind(r, true);

        if (stage == Stage::Vertex) {
            for (const IoVariable& input : io_variables(msl, resources.stage_inputs)) {
                result.vertex_inputs.push_back({input.location, input.semantic});
            }
        }
        // Interstage values by semantic.
        if (stage != Stage::Compute) {
            const auto& interstage =
                stage == Stage::Vertex ? resources.stage_outputs : resources.stage_inputs;
            std::map<std::uint32_t, std::string> used;
            for (const IoVariable& value : io_variables(msl, interstage)) {
                if (split_semantic(value.semantic).first.rfind("SV_", 0) == 0) continue;
                const std::uint32_t location = interstage_location(value.semantic);
                const auto [it, inserted] = used.emplace(location, value.semantic);
                if (!inserted) {
                    result.log = "interstage semantics " + it->second + " and " +
                                 value.semantic + " share location " + std::to_string(location);
                    return result;
                }
                msl.set_decoration(value.id, spv::DecorationLocation, location);
            }
        }
        if (stage == Stage::Pixel && options.dual_source_blend) {
            // SV_Target1 is the second blend source: location 0, index 1.
            for (const auto& output : resources.stage_outputs) {
                if (msl.get_decoration(output.id, spv::DecorationLocation) != 1u) continue;
                msl.set_decoration(output.id, spv::DecorationLocation, 0u);
                msl.set_decoration(output.id, spv::DecorationIndex, 1u);
            }
        }
        if (stage == Stage::Compute) {
            for (unsigned axis = 0; axis < 3u; ++axis) {
                result.threads[axis] =
                    msl.get_execution_mode_argument(spv::ExecutionModeLocalSize, axis);
            }
        }

        result.msl = msl.compile();
        result.entry_point = msl.get_cleansed_entry_point_name(options.entry_point, model);
        result.ok = true;
    } catch (const std::exception& error) {
        result.log += std::string("MSL: ") + error.what();
        result.ok = false;
    }
    return result;
}

Result hlsl_to_msl(std::string_view hlsl, Stage stage, const Options& options) {
    const SpirvResult spirv = hlsl_to_spirv(hlsl, stage, options);
    if (!spirv.ok) {
        Result result;
        result.log = spirv.log;
        return result;
    }
    return spirv_to_msl(spirv.words, stage, options);
}

}  // namespace galaxy::gx::xlate
