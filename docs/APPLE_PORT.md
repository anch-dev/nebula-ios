# Apple (iOS / macOS) port — status and plan

Goal: run Nebula on iOS (sideloaded with SideStore/AltStore), with macOS on
Apple Silicon as the stepping stone. Windows stays the reference platform and
must keep building unchanged.

## How builds work on the port

- The recompiler emits **C++** (not x86 code), so the game itself only needs
  clang targeting `arm64-apple-ios`. The headers generated code includes
  (`native_api.h`, `native_fast_paths.h`) now compile for AArch64.
- GitHub Actions (`.github/workflows/apple-port.yml`) builds and tests the
  ported runtime on Linux ARM64 and macOS Apple Silicon, and compiles it for
  iOS. No Mac is needed.
- **Game code never goes to GitHub.** The runtime/app shell is built in CI;
  the game modules are generated and compiled on the user's own PC from their
  own copy, then added to the app before sideloading. (Planned; see step 7.)

On a non-Windows host the top-level `CMakeLists.txt` includes
`cmake/portable_runtime.cmake`, which builds only what has been ported.

## Done

1. **Header portability** — `platform_compat.h` (`getenv_s` shim);
   `native_fast_paths.h` has an AArch64 NEON implementation of the fast-float
   paired-single helpers and a portable `fctiwz`.
2. **Host fibers** — `host_fiber.h/.cpp`: Win32 fibers on Windows; an
   assembly context switch on POSIX x86-64/AArch64 that also switches FP
   control (MXCSR+x87 CW / FPCR), matching `FIBER_FLAG_FLOAT_SWITCH`.
   `coop_scheduler.cpp` uses it. FTZ for fast float: `fp_enable_flush_denormals()`.
3. **Flat guest memory on POSIX** — `flat_guest_memory_posix.cpp`: 4 GiB view
   with MEM1/MEM2 at all six aliases (memfd on Linux, `vm_remap` on
   Apple), any base address (`guest_base()` replaces `kGuestBase` for
   callers), and an **AArch64 fastmem fault handler** (SIGSEGV/SIGBUS) that
   decodes LDR/STR/LDP/STP/LDUR/pre-/post-index/register-offset, SIMD&FP
   scalar forms and LD1/ST1 lanes. Handles SVE signal frames on Linux.

Tests (all pass on x86-64 and on AArch64 under QEMU, with clang and GCC):
`host_fiber_tests`, `coop_scheduler_tests`, `flat_guest_memory_posix_tests`.

## Next steps

4. **Replace `kGuestBase` at call sites** with `flat_guest_memory::guest_base()`:
   `native_host.cpp` (~11292), `native_runtime.cpp` (`coop_ram`, ~16261),
   `flat_guest_read.h`. Use `current_thread_id()` instead of
   `GetCurrentThreadId()` for `FaultAccessCallbacks::thread_id`.
5. **Remaining Win32 in the non-graphics runtime**: `ppc_float.cpp` (SSE fast
   path; add a scalar `#else`), `runtime_timeline.cpp` (QPC, `__cpuid`, MMCSS
   → `mach_absolute_time`/`clock_gettime`), `atomic_file.cpp`
   (`rename`), `runtime_instance.cpp` (named mutex → `flock`),
   `runtime_settings.cpp`/`input_bindings.cpp` (`SRWLOCK` → `std::shared_mutex`),
   `frame_telemetry.cpp`, `native_dsp.cpp`, then `native_runtime.cpp` and
   `native_host.cpp` (window, audio, input glue). Fast float must call
   `fp_enable_flush_denormals()` where Windows sets MXCSR FTZ|DAZ.
6. **Renderer**: a Metal backend beside `renderer_d3d12.cpp` (~11k lines),
   HLSL uber-shaders → MSL. GX parsing/state above it is shared.
7. **Game module pipeline**: generated C++ → arm64 dylib(s) built on the
   user's PC (stock clang + lld, `-target arm64-apple-ios`), injected into the
   IPA before sideloading. Needs a header set that does not depend on the iOS
   SDK; to be verified.
8. **iOS app shell**: Xcode/CMake project, CAMetalLayer view, GameController,
   AVAudioEngine/AudioUnit, touch controls, gyro pointer; built unsigned in CI.

## Risks

- **iOS address space**: the 4 GiB reservation needs the
  `com.apple.developer.kernel.extended-virtual-addressing` entitlement (and
  probably `increased-memory-limit`). Sideloading tools must keep them.
- Fast-float NaN propagation differs between SSE and NEON in edge cases;
  fast float already trades exactness for speed.
