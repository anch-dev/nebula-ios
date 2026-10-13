# SPDX-License-Identifier: GPL-3.0-only
# Non-Windows build (Linux, macOS, iOS) of the runtime pieces that have been
# ported so far, with their tests. Included from the top-level CMakeLists.txt
# when WIN32 is false. Grows as more of the runtime is ported; see
# docs/APPLE_PORT.md.

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Nebula requires a 64-bit build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
