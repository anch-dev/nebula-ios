// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// HLSL -> Metal Shading Language, for the Metal renderer.
//
// Every shader the renderer uses is HLSL: the uber shaders, the shaders
// ShaderGenerator specializes per draw at run time, the EFB conversion and
// UI shaders and the post-processing passes. Rather than keep a second copy
// of each in MSL, the Metal renderer translates them, at run time, with
// glslang (HLSL -> SPIR-V) and SPIRV-Cross (SPIR-V -> MSL); Metal then
// compiles the MSL source. The HLSL stays the single source of truth.
//
// Resource binding follows the D3D12 register numbers, so the renderer can
// bind Metal resources where the root signature put the D3D12 ones:
//   space0  bN -> [[buffer(N)]]      tN -> [[texture(N)]]   sN -> [[sampler(N)]]
//   space1  tN -> [[buffer(kSpace1BufferBase + N)]]  (structured buffers,
//           e.g. the matrix palette)
// Vertex attributes are numbered in input-declaration order and reported
// with their semantics. Interstage values are located by semantic (COLOR0,
// COLOR1, TEXCOORD0-7, ...), so a vertex and pixel shader generated
// separately always agree.
//
// Matrices in constant buffers are row-major, and constant-buffer offsets
// follow HLSL packing, as the D3D12 path compiles them
// (D3DCOMPILE_PACK_MATRIX_ROW_MAJOR): the same constant bytes work for both.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galaxy::gx::xlate {

enum class Stage : std::uint8_t { Vertex, Pixel, Compute };

enum class Platform : std::uint8_t { macOS, iOS };

inline constexpr std::uint32_t kSpace1BufferBase = 8u;
// The renderer's vertex buffer index: above every constant/structured
// buffer the shaders use.
inline constexpr std::uint32_t kVertexBufferIndex = 30u;

struct Options {
    std::string entry_point = "main";
    // name -> value, as /D on the HLSL compiler command line.
    std::vector<std::pair<std::string, std::string>> defines;
    // The pixel shader's SV_Target1 is the second source of dual-source
    // blending (Metal: [[color(0), index(1)]]), not a second render target.
    bool dual_source_blend = false;
    Platform platform = Platform::iOS;
    // Name for diagnostics.
    std::string label;
};

struct VertexInput {
    std::uint32_t attribute = 0;  // [[attribute(N)]]
    std::string semantic;         // e.g. "POSITION", "COLOR1", "TEXCOORD3"
};

struct Result {
    bool ok = false;
    std::string msl;
    // The MSL entry point's function name (SPIR-V Cross renames "main").
    std::string entry_point;
    std::vector<VertexInput> vertex_inputs;  // Stage::Vertex only
    // Compute: threads per threadgroup from [numthreads].
    std::uint32_t threads[3] = {1u, 1u, 1u};
    // Errors (and warnings) from either step.
    std::string log;
};

[[nodiscard]] Result hlsl_to_msl(std::string_view hlsl, Stage stage, const Options& options);

}  // namespace galaxy::gx::xlate
