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

4. **Fixed guest base removed** — runtime call sites use
   `flat_guest_memory::guest_base()`; the fault handler's thread id comes from
   `current_thread_id()`.
5. **Non-graphics runtime layers** — POSIX implementations of
   `atomic_file` (O_EXCL temp + fsync + rename), `runtime_instance` (flock),
   `runtime_settings`/`input_bindings` (Apple: ~/Library/Application Support,
   Linux: XDG; locale shims), `runtime_timeline` (mach_absolute_time /
   CLOCK_MONOTONIC counter, `host_event` waits, QoS on Apple) and
   `frame_telemetry` (no-op: the overlay IPC is Windows-only).
   Shared vocabulary lives in `platform_compat.h` (MSVC CRT shims) and
   `win32_portable.h` (SRWLOCK, VK_* and XINPUT_* constants: the runtime's
   key/gamepad formats on every platform).

CI (`.github/workflows/apple-port.yml`): 42 upstream + port tests pass on
Linux ARM64 and natively on macOS Apple Silicon; the iOS build compiles; a
Windows MSVC job builds the original runtime and runs its tests to catch
regressions. Locally they also pass on x86-64 and AArch64 under QEMU (the
timeline race test can time out under heavy QEMU load only).

## Next steps

6. **Host glue**: `native_runtime.cpp` (~33k lines) and `native_host.cpp`
   still use Win32 for the window, audio (XAudio2), controllers (XInput/HID),
   module loading (`LoadLibrary` of RMGE01*.dll), crash handling and
   the idle wait (`CreateWaitableTimerExW` → `host_event`). Split each into a
   platform-neutral core plus a Windows backend and an Apple backend
   (AVAudioEngine/AudioUnit, GameController, `dlopen`).
7. **Renderer**: a Metal backend beside `renderer_d3d12.cpp` (~11k lines),
   HLSL uber-shaders → MSL. GX parsing/state above it is shared.
8. **Game module pipeline**: generated C++ → arm64 dylib(s) built on the
   user's PC (stock clang + lld, `-target arm64-apple-ios`), injected into the
   IPA before sideloading. Needs a header set that does not depend on the iOS
   SDK; to be verified.
9. **iOS app shell**: Xcode/CMake project, CAMetalLayer view, touch controls,
   gyro pointer; built unsigned in CI.

## Risks

- **iOS address space**: the 4 GiB reservation needs the
  `com.apple.developer.kernel.extended-virtual-addressing` entitlement (and
  probably `increased-memory-limit`). Sideloading tools must keep them.
- Fast-float NaN propagation differs between SSE and NEON in edge cases;
  fast float already trades exactness for speed.
