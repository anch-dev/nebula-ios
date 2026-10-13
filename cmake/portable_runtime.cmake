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
add_library(galaxy_host_event STATIC runtime/src/host_event.cpp)
target_link_libraries(galaxy_host_event PUBLIC galaxy_runtime_abi Threads::Threads)
add_library(galaxy_atomic_file STATIC runtime/src/atomic_file.cpp)
target_link_libraries(galaxy_atomic_file PUBLIC galaxy_runtime_abi)
add_library(galaxy_runtime_instance STATIC runtime/src/runtime_instance.cpp)
target_link_libraries(galaxy_runtime_instance PUBLIC galaxy_runtime_abi)
add_library(galaxy_runtime_settings STATIC
    runtime/src/runtime_settings.cpp
    runtime/src/input_bindings.cpp)
target_link_libraries(galaxy_runtime_settings PUBLIC galaxy_runtime_abi galaxy_atomic_file)
add_library(galaxy_runtime_timeline STATIC runtime/src/runtime_timeline.cpp)
target_link_libraries(galaxy_runtime_timeline PUBLIC galaxy_runtime_abi galaxy_host_event)
add_library(galaxy_frame_telemetry STATIC runtime/src/frame_telemetry.cpp)
target_link_libraries(galaxy_frame_telemetry PUBLIC galaxy_runtime_abi)

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
