# SPDX-License-Identifier: GPL-3.0-only
# Non-Windows build (Linux, macOS, iOS) of the runtime pieces that have been
# ported so far, with their tests. Included from the top-level CMakeLists.txt
# when WIN32 is false. Grows as more of the runtime is ported; see
# docs/APPLE_PORT.md.

enable_testing()

set(galaxy_runtime_include "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")

# Platform layer: host fibers and the flat guest view.
add_library(galaxy_host_fiber STATIC runtime/src/host_fiber.cpp)
target_include_directories(galaxy_host_fiber PUBLIC "${galaxy_runtime_include}")

add_library(galaxy_flat_guest_memory STATIC runtime/src/flat_guest_memory_posix.cpp)
target_include_directories(galaxy_flat_guest_memory PUBLIC "${galaxy_runtime_include}")
find_package(Threads REQUIRED)
target_link_libraries(galaxy_flat_guest_memory PUBLIC Threads::Threads)

add_library(galaxy_coop_scheduler STATIC runtime/src/coop_scheduler.cpp)
target_include_directories(
    galaxy_coop_scheduler
    PUBLIC "${galaxy_runtime_include}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
target_link_libraries(galaxy_coop_scheduler PUBLIC galaxy_host_fiber)

# Tests run where the build can execute them (not when cross-compiling to iOS).
set(galaxy_can_run_tests ON)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    set(galaxy_can_run_tests OFF)
endif()

if(galaxy_can_run_tests)
    add_executable(galaxy_host_fiber_tests runtime/src/host_fiber_tests.cpp)
    target_link_libraries(galaxy_host_fiber_tests PRIVATE galaxy_host_fiber)
    add_test(NAME host_fiber COMMAND galaxy_host_fiber_tests)

    add_executable(galaxy_coop_scheduler_tests runtime/src/coop_scheduler_tests.cpp)
    target_link_libraries(galaxy_coop_scheduler_tests PRIVATE galaxy_coop_scheduler)
    add_test(NAME coop_scheduler COMMAND galaxy_coop_scheduler_tests)

    add_executable(galaxy_flat_guest_memory_posix_tests
        runtime/src/flat_guest_memory_posix_tests.cpp)
    target_link_libraries(galaxy_flat_guest_memory_posix_tests PRIVATE galaxy_flat_guest_memory)
    add_test(NAME flat_guest_memory_posix COMMAND galaxy_flat_guest_memory_posix_tests)
endif()

# Win32 vocabulary for POSIX hosts (files, events, timers, threads, window).
add_library(galaxy_win32_posix STATIC
    runtime/src/win32_fs_posix.cpp
    runtime/src/win32_host_posix.cpp)
target_link_libraries(galaxy_win32_posix PUBLIC galaxy_runtime_abi Threads::Threads ${CMAKE_DL_LIBS})

# Portable runtime libraries (same names and sources as on Windows).
add_library(galaxy_native_dsp STATIC runtime/src/native_dsp.cpp)
target_link_libraries(galaxy_native_dsp PUBLIC galaxy_dsp_alu galaxy_runtime_abi)
foreach(name IN ITEMS checkpoint_gate decrementer_deadline frame_cadence_diagnostics
                      host_pointer_events native_input_causality ai_pcm_identity)
    add_library(galaxy_${name} STATIC runtime/src/${name}.cpp)
    target_link_libraries(galaxy_${name} PUBLIC galaxy_runtime_abi)
endforeach()
add_library(galaxy_native_input STATIC
    runtime/src/native_input.cpp
    runtime/src/pointer_control.cpp
    runtime/src/rumble.cpp
    runtime/src/switch_controller.cpp
    runtime/src/tilt.cpp)
target_link_libraries(galaxy_native_input PUBLIC galaxy_runtime_abi)
add_library(galaxy_jaudio_mixer STATIC runtime/src/jaudio_host_mixer.cpp)
target_link_libraries(galaxy_jaudio_mixer PUBLIC galaxy_runtime_abi)
add_library(galaxy_atomic_file STATIC runtime/src/atomic_file.cpp)
target_link_libraries(galaxy_atomic_file PUBLIC galaxy_runtime_abi)
add_library(galaxy_runtime_instance STATIC runtime/src/runtime_instance.cpp)
target_link_libraries(galaxy_runtime_instance PUBLIC galaxy_runtime_abi)
add_library(galaxy_runtime_settings STATIC
    runtime/src/runtime_settings.cpp
    runtime/src/input_bindings.cpp)
target_link_libraries(galaxy_runtime_settings PUBLIC galaxy_runtime_abi galaxy_atomic_file)
add_library(galaxy_runtime_timeline STATIC runtime/src/runtime_timeline.cpp)
target_link_libraries(galaxy_runtime_timeline PUBLIC galaxy_runtime_abi galaxy_win32_posix)
add_library(galaxy_frame_telemetry STATIC runtime/src/frame_telemetry.cpp)
target_link_libraries(galaxy_frame_telemetry PUBLIC galaxy_runtime_abi)

# POSIX Win32 shims and the host (memory, disc, saves, audio, input).

set(galaxy_native_host_sources
    runtime/src/native_host.cpp
    runtime/src/controller_hid.cpp
    runtime/src/controller_hid_posix.cpp
    runtime/src/flat_guest_memory_posix.cpp
    runtime/src/gx/host_window_posix.cpp)
set(galaxy_native_host_libraries
    galaxy_runtime_abi galaxy_runtime_settings galaxy_native_input galaxy_native_dsp
    galaxy_frame_cadence_diagnostics galaxy_ai_pcm_identity galaxy_win32_posix
    galaxy_host_fiber)
if(APPLE)
    list(APPEND galaxy_native_host_libraries "-framework AudioToolbox" "-framework CoreAudio")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        list(APPEND galaxy_native_host_libraries "-framework AudioUnit")
    endif()
endif()
add_library(galaxy_native_host STATIC ${galaxy_native_host_sources})
target_include_directories(galaxy_native_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src")
target_link_libraries(galaxy_native_host PUBLIC ${galaxy_native_host_libraries})
# As in the Windows product build (NEBULA_DSP_MODULE), the DSP is the
# RMGE01_dsp module compiled from the user's game, loaded at run time. (The
# test-support variant keeps upstream's test configuration.)
target_compile_definitions(galaxy_native_host PRIVATE GALAXY_HAVE_NATIVE_DSP=1 GALAXY_NATIVE_DSP_MODULE=1)
# Test-support variant, as on Windows (retired DSP mixer helpers, fault hooks).
add_library(galaxy_native_host_test_support STATIC ${galaxy_native_host_sources})
target_include_directories(galaxy_native_host_test_support PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src")
target_link_libraries(galaxy_native_host_test_support PUBLIC ${galaxy_native_host_libraries})
target_compile_definitions(galaxy_native_host_test_support PRIVATE
    GALAXY_TEST_ALLOW_RETIRED_DSP_MIXER=1 GALAXY_NATIVE_HOST_TEST_SUPPORT=1)

# The upstream unit tests that are portable, registered with CTest.
# galaxy_portable_test(<name> SOURCES ... LIBS ... DEFS ...)
function(galaxy_portable_test name)
    cmake_parse_arguments(arg "" "" "SOURCES;LIBS;DEFS" ${ARGN})
    if(NOT galaxy_can_run_tests)
        return()
    endif()
    if(NOT arg_SOURCES)
        set(arg_SOURCES runtime/src/${name}.cpp)
    endif()
    add_executable(galaxy_${name} ${arg_SOURCES})
    target_link_libraries(galaxy_${name} PRIVATE galaxy_runtime_abi ${arg_LIBS})
    if(arg_DEFS)
        target_compile_definitions(galaxy_${name} PRIVATE ${arg_DEFS})
    endif()
    add_test(NAME ${name} COMMAND galaxy_${name})
endfunction()

foreach(name IN ITEMS
        diagnostic_stick_lease_tests external_dispatch_tests flat_dispatch_tests
        function_async_flight_tests native_audio_queue_tests
        native_checkpoint_policy_tests native_ios_clock_tests native_multiple_gpr_tests
        native_yaz0_span_tests native_zero_tests opening_route_marker_tests
        same_context_rfi_tests simulation_cache_affinity_tests thp_audio_worker_tests
        vi_boundary_tests native_cached_call_tests)
    galaxy_portable_test(${name})
endforeach()
galaxy_portable_test(ai_pcm_identity_tests LIBS galaxy_ai_pcm_identity)
galaxy_portable_test(checkpoint_gate_tests LIBS galaxy_checkpoint_gate)
galaxy_portable_test(native_module_checkpoint_gate_tests LIBS galaxy_checkpoint_gate)
galaxy_portable_test(decrementer_deadline_tests LIBS galaxy_decrementer_deadline)
galaxy_portable_test(dsp_alu_tests LIBS galaxy_dsp_alu)
galaxy_portable_test(frame_cadence_diagnostics_tests LIBS galaxy_frame_cadence_diagnostics)
galaxy_portable_test(jaudio_mixer_tests
    SOURCES runtime/src/jaudio_host_mixer_tests.cpp LIBS galaxy_jaudio_mixer)
galaxy_portable_test(native_input_causality_tests LIBS galaxy_native_input_causality)
galaxy_portable_test(native_thp_video_tests
    SOURCES runtime/src/native_thp_video_tests.cpp runtime/src/native_thp_video.cpp)
galaxy_portable_test(atomic_file_tests LIBS galaxy_atomic_file)
galaxy_portable_test(runtime_instance_tests LIBS galaxy_runtime_instance)
galaxy_portable_test(runtime_settings_tests LIBS galaxy_runtime_settings)
galaxy_portable_test(controls_tests LIBS galaxy_runtime_settings galaxy_native_input)
galaxy_portable_test(runtime_timeline_tests LIBS galaxy_runtime_timeline)
if(APPLE AND TARGET galaxy_runtime_timeline_tests)
    # std::jthread is still behind -fexperimental-library in Apple's libc++.
    target_compile_options(galaxy_runtime_timeline_tests PRIVATE -fexperimental-library)
endif()
galaxy_portable_test(win32_fs_posix_tests SOURCES runtime/src/win32_fs_posix_tests.cpp
    LIBS galaxy_win32_posix)
galaxy_portable_test(portable_hash_tests)
galaxy_portable_test(native_host_tests
    LIBS galaxy_native_host_test_support galaxy_host_pointer_events galaxy_native_input_causality)
if(TARGET galaxy_native_host_tests)
    # CI has no game-derived DSP ucode; the test-support build's retired mixer
    # stands in for it, as upstream's test hook intends.
    set_tests_properties(native_host_tests PROPERTIES
        ENVIRONMENT "GALAXY_ALLOW_RETIRED_DSP_MIXER_FOR_TESTS=1"
        TIMEOUT 1800)
endif()
galaxy_portable_test(native_dsp_tests LIBS galaxy_native_dsp galaxy_runtime_timeline)
galaxy_portable_test(ppc_float_tests LIBS galaxy_ppc_float DEFS SOFTFLOAT_FAST_INT64=1)
if(TARGET galaxy_ppc_float_tests)
    target_include_directories(galaxy_ppc_float_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/softfloat")
endif()
galaxy_portable_test(native_api_tests LIBS galaxy_ppc_float)
galaxy_portable_test(native_bctrl_site_cache_tests LIBS galaxy_ppc_float)
galaxy_portable_test(native_fast_float_tests LIBS galaxy_ppc_float DEFS GALAXY_FAST_FLOAT=1)
galaxy_portable_test(native_hw_float_conversion_tests LIBS galaxy_ppc_float
    DEFS GALAXY_FAST_FLOAT=1)
galaxy_portable_test(native_flat_view_only_tests LIBS galaxy_ppc_float
    DEFS GALAXY_FAST_FLOAT=1 GALAXY_FASTMEM=1 GALAXY_FLAT_VIEW_ONLY=1)
galaxy_portable_test(native_assume_paired_singles_tests LIBS galaxy_ppc_float
    DEFS GALAXY_FAST_FLOAT=1 GALAXY_ASSUME_PAIRED_SINGLES=1)
galaxy_portable_test(native_single_float_arithmetic_tests LIBS galaxy_ppc_float
    DEFS GALAXY_FAST_FLOAT=1 GALAXY_SINGLE_FLOAT_ARITHMETIC=1 GALAXY_ASSUME_PAIRED_SINGLES=1)

# The headers every generated game shard includes must compile for this
# target in each module configuration the translator emits.
set(galaxy_header_check_source "${CMAKE_CURRENT_BINARY_DIR}/generated_header_check.cpp")
file(WRITE "${galaxy_header_check_source}"
    "#include \"galaxy/native_api.h\"\nint galaxy_header_check() { return 0; }\n")
foreach(config IN ITEMS default fast_float fast_float_flat single_float)
    add_library(galaxy_header_check_${config} OBJECT "${galaxy_header_check_source}")
    target_include_directories(galaxy_header_check_${config} PRIVATE
        "${galaxy_runtime_include}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
endforeach()
target_compile_definitions(galaxy_header_check_fast_float PRIVATE
    GALAXY_FAST_FLOAT=1 GALAXY_FASTMEM=1)
target_compile_definitions(galaxy_header_check_fast_float_flat PRIVATE
    GALAXY_FAST_FLOAT=1 GALAXY_FASTMEM=1 GALAXY_FLAT_VIEW_ONLY=1)
target_compile_definitions(galaxy_header_check_single_float PRIVATE
    GALAXY_FAST_FLOAT=1 GALAXY_FASTMEM=1 GALAXY_SINGLE_FLOAT_ARITHMETIC=1
    GALAXY_ASSUME_PAIRED_SINGLES=1)

# The native runtime (native_runtime.cpp). Its renderer, galaxy_gx, is still
# Direct3D 12 only (step 7 of docs/APPLE_PORT.md), so for now the runtime is
# compiled to check the port but not linked into an executable.
set(NEBULA_SOURCE_REVISION "" CACHE STRING "Revision of exported source")
set(NEBULA_SOURCE_REVISION_KIND "unknown" CACHE STRING "Exported revision object type (commit or tree)")
set(NEBULA_SOURCE_GIT_TREE "unknown" CACHE STRING "Git tree of exported source")
set(NEBULA_SOURCE_TREE_SHA256 "" CACHE STRING "Exported source manifest tree SHA256")
set(NEBULA_SOURCE_DIRTY "unknown" CACHE STRING "Exported source dirty state")
mark_as_advanced(NEBULA_SOURCE_REVISION NEBULA_SOURCE_REVISION_KIND NEBULA_SOURCE_GIT_TREE
    NEBULA_SOURCE_TREE_SHA256 NEBULA_SOURCE_DIRTY)
set(galaxy_native_isa_effective "${CMAKE_SYSTEM_PROCESSOR}")
set(GALAXY_NATIVE_ISA "${CMAKE_SYSTEM_PROCESSOR}")
set(NEBULA_DSP_MODULE "retired-mixer")
set(nebula_host_ipo_effective OFF)
set(NEBULA_ID_TARGET_OPTIONS "")
get_directory_property(NEBULA_ID_DIRECTORY_OPTIONS COMPILE_OPTIONS)
configure_file(cmake/runtime_identity_config.cmake.in runtime_identity_config.cmake @ONLY)
add_custom_target(nebula_runtime_identity
    COMMAND "${CMAKE_COMMAND}"
        "-DCONFIG_FILE=${CMAKE_CURRENT_BINARY_DIR}/runtime_identity_config.cmake"
        "-DBUILD_CONFIG=$<CONFIG>"
        "-DOUTPUT_HEADER=${CMAKE_CURRENT_BINARY_DIR}/nebula_build_identity.h"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/runtime_build_identity.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/nebula_build_identity.h"
    VERBATIM)

add_library(galaxy_native_runtime_objects OBJECT
    runtime/src/native_runtime.cpp
    runtime/src/native_thp_video.cpp
    runtime/src/site_hooks.cpp)
add_dependencies(galaxy_native_runtime_objects nebula_runtime_identity)
target_include_directories(galaxy_native_runtime_objects PRIVATE
    "${galaxy_runtime_include}"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src"
    "${CMAKE_CURRENT_BINARY_DIR}")

# HLSL -> MSL translation for the Metal renderer (glslang + SPIRV-Cross,
# fetched at pinned releases). Also built on Linux so CI checks every
# shader translates.
option(GALAXY_SHADER_TRANSLATOR "Build the HLSL to MSL shader translator" ON)
if(GALAXY_SHADER_TRANSLATOR)
    include(FetchContent)
    set(galaxy_khronos_tag vulkan-sdk-1.4.341.0)
    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG ${galaxy_khronos_tag}
        GIT_SHALLOW TRUE)
    FetchContent_Declare(spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG ${galaxy_khronos_tag}
        GIT_SHALLOW TRUE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glslang spirv_cross)

    add_library(galaxy_shader_translate STATIC runtime/src/gx/shader_translate.cpp)
    target_include_directories(galaxy_shader_translate PUBLIC "${galaxy_runtime_include}")
    # glslang's headers declare the HLSL-only setters under this macro.
    target_compile_definitions(galaxy_shader_translate PRIVATE ENABLE_HLSL)
    target_link_libraries(galaxy_shader_translate
        PRIVATE glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits
                spirv-cross-msl spirv-cross-glsl spirv-cross-core)
endif()

# The renderer-independent half of galaxy_gx: FIFO parsing, GX state, shader
# keys and generation, render configuration. The Metal renderer (step 7)
# builds on it.
set(galaxy_uber_embed_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/uber_shader_embed.cpp")
set(galaxy_uber_embed_body "")
foreach(galaxy_uber_stage vs ps)
    set(galaxy_uber_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/gx/uber_${galaxy_uber_stage}.hlsl")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${galaxy_uber_hlsl}")
    file(READ "${galaxy_uber_hlsl}" galaxy_uber_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," galaxy_uber_hex "${galaxy_uber_hex}")
    string(APPEND galaxy_uber_embed_body
        "extern const char galaxy_uber_${galaxy_uber_stage}_hlsl[] = {${galaxy_uber_hex}0x00};\n")
endforeach()
file(CONFIGURE OUTPUT "${galaxy_uber_embed_cpp}"
    CONTENT "namespace galaxy::gx {\n${galaxy_uber_embed_body}}\n"
    @ONLY)

add_library(galaxy_gx_core STATIC
    runtime/src/gx/fifo_parser.cpp
    runtime/src/gx/gx_state.cpp
    runtime/src/gx/shader_keys.cpp
    runtime/src/gx/shader_gen.cpp
    runtime/src/gx/uber_shaders.cpp
    "${galaxy_uber_embed_cpp}"
    runtime/src/gx/render_config.cpp
    runtime/src/gx/transform_history.cpp
    runtime/src/gx/line_point_raster.cpp
    runtime/src/gx/game_asset_extract.cpp)
target_include_directories(galaxy_gx_core PUBLIC "${galaxy_runtime_include}")
target_link_libraries(galaxy_gx_core PUBLIC galaxy_win32_posix galaxy_atomic_file Threads::Threads)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # The texture and FIFO kernels use SSSE3 (pshufb), as on Windows.
    target_compile_options(galaxy_gx_core PUBLIC -mssse3)
endif()

foreach(name IN ITEMS fifo_parser_tests owned_fifo_packet_tests dependency_event_capture_tests
        sorted_dirty_ranges_tests transform_history_tests vs_constants_tests
        game_asset_extract_tests render_config_tests)
    galaxy_portable_test(${name} SOURCES runtime/src/gx/${name}.cpp LIBS galaxy_gx_core)
endforeach()

if(GALAXY_SHADER_TRANSLATOR AND galaxy_can_run_tests)
    if(APPLE)
        enable_language(OBJCXX)
        set(galaxy_msl_check runtime/src/gx/shader_msl_check_metal.mm)
    else()
        set(galaxy_msl_check runtime/src/gx/shader_msl_check_none.cpp)
    endif()
    galaxy_portable_test(shader_gen_tests
        SOURCES runtime/src/gx/shader_gen_tests.cpp ${galaxy_msl_check}
        LIBS galaxy_gx_core galaxy_shader_translate)
    target_include_directories(galaxy_shader_gen_tests PRIVATE runtime/src/gx)
    if(APPLE)
        target_link_libraries(galaxy_shader_gen_tests PRIVATE
            "-framework Metal" "-framework Foundation")
        set_source_files_properties(runtime/src/gx/shader_msl_check_metal.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()
endif()

# Presentation post-processing passes, composed exactly as the Windows build
# composes them for fxc (runtime/src/gx/post_process.hlsl with the vendored
# SMAA and FSR 1 sources). The Metal renderer translates the same sources;
# post_shader_translate_tests checks every pass.
set(galaxy_post_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/post_shaders")
file(MAKE_DIRECTORY "${galaxy_post_dir}")
set(galaxy_post_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/gx/post_process.hlsl")
set(galaxy_smaa_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smaa/SMAA.hlsl")
set(galaxy_ffx_a_h "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1/ffx_a.h")
set(galaxy_ffx_fsr1_h "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1/ffx_fsr1.h")
foreach(galaxy_post_input
        "${galaxy_post_hlsl}" "${galaxy_smaa_hlsl}" "${galaxy_ffx_a_h}" "${galaxy_ffx_fsr1_h}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${galaxy_post_input}")
endforeach()
file(READ "${galaxy_post_hlsl}" galaxy_post_src)
file(READ "${galaxy_smaa_hlsl}" galaxy_smaa_src)
file(READ "${galaxy_ffx_a_h}" galaxy_ffx_a_src)
file(READ "${galaxy_ffx_fsr1_h}" galaxy_ffx_fsr1_src)
set(galaxy_post_list "")
function(galaxy_post_shader name pass preset library stage entry)
    set(text "#define ${pass} 1\n")
    if(NOT preset STREQUAL "-")
        string(APPEND text "#define ${preset} 1\n")
    endif()
    string(APPEND text "#define POST_SECTION 1\n${galaxy_post_src}\n")
    if(library STREQUAL "smaa")
        string(APPEND text "${galaxy_smaa_src}\n")
    elseif(library STREQUAL "fsr")
        string(APPEND text "${galaxy_ffx_a_src}\n#undef POST_SECTION\n#define POST_SECTION 2\n"
            "${galaxy_post_src}\n${galaxy_ffx_fsr1_src}\n")
    endif()
    string(APPEND text "#undef POST_SECTION\n#define POST_SECTION 3\n${galaxy_post_src}\n")
    set(source "${galaxy_post_dir}/${name}.hlsl")
    file(WRITE "${source}.tmp" "${text}")
    configure_file("${source}.tmp" "${source}" COPYONLY)
    set(galaxy_post_list "${galaxy_post_list}    {\"${name}\", \"${source}\", '${stage}', \"${entry}\"},\n"
        PARENT_SCOPE)
endfunction()
galaxy_post_shader(post_fullscreen_vs PASS_RESAMPLE - none v VSMain)
galaxy_post_shader(post_resample_ps PASS_RESAMPLE - none p PSMain)
galaxy_post_shader(post_fxaa_ps PASS_FXAA - none p PSMain)
galaxy_post_shader(post_fsr_easu_ps PASS_FSR_EASU - fsr p PSMain)
galaxy_post_shader(post_fsr_rcas_ps PASS_FSR_RCAS - fsr p PSMain)
galaxy_post_shader(post_smaa_edges_vs PASS_SMAA_EDGES SMAA_PRESET_HIGH smaa v VSMain)
galaxy_post_shader(post_smaa_blend_vs PASS_SMAA_BLEND SMAA_PRESET_HIGH smaa v VSMain)
galaxy_post_shader(post_smaa_blend_ps PASS_SMAA_BLEND SMAA_PRESET_HIGH smaa p PSMain)
foreach(galaxy_smaa_preset LOW MEDIUM HIGH ULTRA)
    string(TOLOWER "${galaxy_smaa_preset}" galaxy_smaa_suffix)
    galaxy_post_shader(post_smaa_edges_ps_${galaxy_smaa_suffix}
        PASS_SMAA_EDGES SMAA_PRESET_${galaxy_smaa_preset} smaa p PSMain)
    galaxy_post_shader(post_smaa_weights_vs_${galaxy_smaa_suffix}
        PASS_SMAA_WEIGHTS SMAA_PRESET_${galaxy_smaa_preset} smaa v VSMain)
    galaxy_post_shader(post_smaa_weights_ps_${galaxy_smaa_suffix}
        PASS_SMAA_WEIGHTS SMAA_PRESET_${galaxy_smaa_preset} smaa p PSMain)
endforeach()
file(CONFIGURE OUTPUT "${galaxy_post_dir}/post_shader_list.h"
    CONTENT "// Generated by cmake/portable_runtime.cmake.\n#pragma once\nstruct PostShaderSource { const char* name; const char* path; char stage; const char* entry; };\ninline constexpr PostShaderSource kPostShaders[] = {\n${galaxy_post_list}};\n"
    @ONLY)

if(GALAXY_SHADER_TRANSLATOR AND galaxy_can_run_tests)
    galaxy_portable_test(post_shader_translate_tests
        SOURCES runtime/src/gx/post_shader_translate_tests.cpp ${galaxy_msl_check}
        LIBS galaxy_shader_translate)
    target_include_directories(galaxy_post_shader_translate_tests PRIVATE
        runtime/src/gx "${galaxy_post_dir}")
    if(APPLE)
        target_link_libraries(galaxy_post_shader_translate_tests PRIVATE
            "-framework Metal" "-framework Foundation")
    endif()
endif()

# The renderer (galaxy_gx) over a Direct3D 12 subset: the upstream D3D12
# renderer sources compile unchanged against runtime/compat/win32 (a
# <Windows.h>, DXGI and D3DCompile for these hosts) and Microsoft's
# DirectX-Headers (MIT, fetched at a pinned release); runtime/src/d3d12
# implements the D3D12 objects over Metal (Apple) or a CPU backend.
if(GALAXY_SHADER_TRANSLATOR)
    FetchContent_Declare(directx_headers
        GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
        GIT_TAG v1.618.2
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(directx_headers)
    if(NOT directx_headers_POPULATED)
        FetchContent_Populate(directx_headers)
    endif()
    set(galaxy_d3d12_includes
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/compat/win32"
        "${directx_headers_SOURCE_DIR}/include"
        "${galaxy_runtime_include}")

    add_library(galaxy_d3d12 STATIC
        runtime/src/d3d12/device.cpp
        runtime/src/d3d12/command_list.cpp
        runtime/src/d3d12/compiler.cpp
        runtime/src/d3d12/dxgi.cpp
        runtime/src/d3d12/backend_cpu.cpp)
    target_include_directories(galaxy_d3d12 PUBLIC ${galaxy_d3d12_includes})
    target_link_libraries(galaxy_d3d12 PUBLIC galaxy_shader_translate galaxy_win32_posix
        Threads::Threads)
    if(APPLE)
        enable_language(OBJCXX)
        set(GALAXY_METAL_BACKEND ON)
        target_sources(galaxy_d3d12 PRIVATE runtime/src/d3d12/backend_metal.mm)
        set_source_files_properties(runtime/src/d3d12/backend_metal.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(galaxy_d3d12 PRIVATE GALAXY_METAL_BACKEND=1)
        target_link_libraries(galaxy_d3d12 PUBLIC
            "-framework Metal" "-framework QuartzCore" "-framework Foundation")
    endif()

    set(galaxy_gx_sources
        runtime/src/gx/gx_shim.cpp
        runtime/src/gx/gx_headers_compile.cpp
        runtime/src/gx/frame_completion.cpp
        runtime/src/gx/renderer_d3d12.cpp
        runtime/src/gx/window_owner.cpp
        runtime/src/gx/texture_cache.cpp
        runtime/src/gx/placed_texture_allocator.cpp
        runtime/src/gx/vertex_loader.cpp
        runtime/src/gx/efb_copy.cpp
        runtime/src/gx/pipeline_cache.cpp
        runtime/src/gx/post_process.cpp
        runtime/src/gx/temporal_upscaler.cpp
        runtime/src/gx/display_modes.cpp
        runtime/src/gx/gx_backend.cpp)
    # The post-processing passes as "bytecode" headers: D3DCompile blobs
    # would need the translator at build time on the build machine, so the
    # headers carry HLSL instead (see runtime/src/d3d12/compiler.cpp).
    set(galaxy_post_headers "")
    file(STRINGS "${galaxy_post_dir}/post_shader_list.h" galaxy_post_entries REGEX "^    \\{")
    foreach(entry IN LISTS galaxy_post_entries)
        string(REGEX MATCH "\\{\"([^\"]+)\", \"([^\"]+)\", '(.)', \"([^\"]+)\"\\}" _ "${entry}")
        set(name "${CMAKE_MATCH_1}")
        set(source "${CMAKE_MATCH_2}")
        set(stage "${CMAKE_MATCH_3}")
        set(entry_point "${CMAKE_MATCH_4}")
        set(header "${galaxy_post_dir}/${name}.h")
        add_custom_command(OUTPUT "${header}"
            COMMAND "${CMAKE_COMMAND}" -DSOURCE=${source} -DHEADER=${header} -DNAME=g_${name}
                -DSTAGE=${stage} -DENTRY=${entry_point}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hlsl_source_blob.cmake"
            DEPENDS "${source}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hlsl_source_blob.cmake"
            VERBATIM)
        list(APPEND galaxy_post_headers "${header}")
    endforeach()

    add_library(galaxy_gx STATIC ${galaxy_gx_sources} ${galaxy_post_headers})
    target_include_directories(galaxy_gx
        PUBLIC ${galaxy_d3d12_includes}
        PRIVATE "${galaxy_post_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smaa"
                "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src")
    target_compile_definitions(galaxy_gx PRIVATE GALAXY_WITH_STREAMLINE_BRIDGE=0)
    target_link_libraries(galaxy_gx PUBLIC galaxy_gx_core galaxy_d3d12 galaxy_native_host
        galaxy_frame_cadence_diagnostics galaxy_runtime_settings galaxy_host_pointer_events
        galaxy_frame_telemetry)
endif()

if(GALAXY_SHADER_TRANSLATOR)
    # The runtime as a library, for an executable's main (Linux/macOS) or the
    # iOS app (step 9), which call galaxy_runtime_main.
    add_library(galaxy_native_runtime STATIC $<TARGET_OBJECTS:galaxy_native_runtime_objects>)
    target_link_libraries(galaxy_native_runtime PUBLIC
        galaxy_native_host galaxy_native_input_causality galaxy_gx galaxy_ppc_float
        galaxy_checkpoint_gate galaxy_decrementer_deadline galaxy_runtime_timeline
        galaxy_jaudio_mixer galaxy_runtime_instance galaxy_coop_scheduler
        galaxy_flat_guest_memory ${CMAKE_DL_LIBS})
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        add_executable(NebulaRuntime runtime/src/runtime_main_posix.cpp)
        target_link_libraries(NebulaRuntime PRIVATE galaxy_native_runtime)
        # The game module resolves its runtime imports from the executable.
        set_target_properties(NebulaRuntime PROPERTIES ENABLE_EXPORTS ON)
    else()
        # The iOS app (docs/APPLE_PORT.md step 9): apple/ios. CI packages
        # Nebula.app as an IPA; the game modules are added on the user's PC.
        enable_language(OBJCXX)
        add_executable(Nebula MACOSX_BUNDLE apple/ios/NebulaApp.mm)
        target_compile_options(Nebula PRIVATE -fobjc-arc)
        target_link_libraries(Nebula PRIVATE galaxy_native_runtime
            "-framework UIKit" "-framework GameController" "-framework AVFoundation"
            "-framework QuartzCore" "-framework Metal" "-framework CoreGraphics" "-framework CoreHaptics"
            "-framework Foundation")
        set_target_properties(Nebula PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/apple/ios/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.nebula.ios"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
            MACOSX_BUNDLE_BUNDLE_VERSION "1")
        # The product environment the Windows launcher applies.
        add_custom_command(TARGET Nebula POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/installer/runtime-env.json"
                "$<TARGET_BUNDLE_DIR:Nebula>/runtime-env.json")
    endif()
endif()

if(GALAXY_SHADER_TRANSLATOR)
    # Upstream renderer tests over the D3D12 layer.
    foreach(name IN ITEMS vertex_loader_tests frame_completion_tests memory_snapshot_tests
            sampler_key_tests xfb_present_selection_tests native_settings_overlay_tests
            window_owner_tests)
        galaxy_portable_test(${name} SOURCES runtime/src/gx/${name}.cpp LIBS galaxy_gx)
    endforeach()
    # These check rasterized pixels, so they need the Metal backend.
    if(APPLE AND GALAXY_METAL_BACKEND)
        foreach(name IN ITEMS post_process_tests efb_conversion_tests line_point_raster_tests)
            galaxy_portable_test(${name} SOURCES runtime/src/gx/${name}.cpp LIBS galaxy_gx)
        endforeach()
    endif()
    if(galaxy_can_run_tests)
        add_executable(galaxy_pipeline_cache_persistence_tests
            runtime/src/gx/pipeline_cache_persistence_tests.cpp
            runtime/src/gx/pipeline_cache.cpp)
        target_compile_definitions(galaxy_pipeline_cache_persistence_tests
            PRIVATE GALAXY_PIPELINE_CACHE_PERSISTENCE_TEST_IO=1)
        target_link_libraries(galaxy_pipeline_cache_persistence_tests PRIVATE galaxy_gx)
        add_test(NAME pipeline_cache_persistence_tests
            COMMAND galaxy_pipeline_cache_persistence_tests)
    endif()
endif()

if(GALAXY_SHADER_TRANSLATOR)
    galaxy_portable_test(d3d12_layer_tests SOURCES runtime/src/d3d12/d3d12_layer_tests.cpp
        LIBS galaxy_d3d12)
endif()
