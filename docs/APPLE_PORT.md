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
  own copy, then added to the app before sideloading. (Planned; see step 8.)

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
   CLOCK_MONOTONIC counter, waitable timers and events from the Win32 shim,
   QoS on Apple) and
   `frame_telemetry` (no-op: the overlay IPC is Windows-only).
   Shared vocabulary lives in `platform_compat.h` (MSVC CRT shims) and
   `win32_portable.h` (SRWLOCK, VK_* and XINPUT_* constants: the runtime's
   key/gamepad formats on every platform).

6. **Host glue** — `native_host.cpp` and `native_runtime.cpp` build on
   POSIX. Rather than rewrite their Win32-shaped logic (the save system's
   crash-safety rules depend on exact Win32 semantics), a POSIX Win32 shim
   provides those semantics:
   - `win32_fs.h` / `win32_fs_posix.cpp`: files, directories, mappings,
     events and waitable timers. NTFS named streams (NAND metadata) are
     extended attributes (`user.galaxy.stream.<name>`) that follow their
     file through renames; share modes are enforced per stream inside the
     process, as NTFS does; delete-on-close; `MoveFileEx` without REPLACE
     never overwrites (`renameat2`/`renamex_np`); file identity is the inode
     (plus its generation on Linux); the volume serial hashes the APFS
     volume UUID.
   - `win32_host_posix.h`: thread priorities (QoS classes on Apple), CPU
     times, `dlopen`-backed module loading (`RMGE01_game.dll` becomes
     `RMGE01_game.dylib` / `.so`), one virtual window and XInput-format pads.
   - `host_platform.h`: what the app shell feeds in — its view and size,
     pointer, keys, up to four pads (GameController maps onto XInput's
     format) — and the rumble it should play.
   - Audio: an AudioUnit sink (RemoteIO on iOS) behind the same interface as
     the XAudio2 sink, with a lock-free ring and 48 kHz resampling.
   - Crashes: fatal signals print the `[crash]` report and a backtrace, then
     fall through to the OS crash reporter (no minidumps).
   - Entry: `galaxy_runtime_main(argc, argv)` (`runtime_main.h`), called by
     `main` on Linux/macOS or by the iOS app's game thread.
   - Windows-only extras are compiled out: the suspend-and-sample profiler
     (use Instruments/perf), MMCSS, timer resolution, processor-group cache
     affinity, the Switch-controller HID reader (GameController covers it)
     and real-Wiimote Bluetooth.

   The upstream host test suite (20k lines: saves, disc, IPC, audio, input)
   passes on Linux and macOS; its only changes are test-side (named streams
   are read through the shim, and Windows-only cases are skipped).

CI (`.github/workflows/apple-port.yml`): 45 upstream + port tests pass on
Linux ARM64 and natively on macOS Apple Silicon; the iOS build compiles; a
Windows MSVC job builds the original runtime and runs its tests to catch
regressions. Locally they also pass on x86-64 and AArch64 under QEMU (the
timeline race test can time out under heavy QEMU load only).

7a. **Renderer groundwork** —
   - *Shaders*: every renderer shader is HLSL. The Metal renderer
     translates them at run time instead of keeping MSL copies:
     `shader_translate.h` runs glslang (HLSL → SPIR-V) and SPIRV-Cross
     (SPIR-V → MSL), fetched at a pinned Vulkan SDK release. Bindings keep
     the D3D12 register numbers (bN → buffer N, tN → texture N, sN →
     sampler N, space1 → buffers from 8), constant buffers keep HLSL
     packing and row-major matrices, interstage values are placed by
     semantic, and dual-source blending maps to `[[color(0), index(1)]]`.
     `shader_gen_tests` (upstream's corpus of ~250 generated shaders plus
     the uber shaders) and `post_shader_translate_tests` (FXAA, FSR 1, SMAA,
     resample) translate every shader; on macOS each result is also
     compiled by Metal. Only the two GX line/point geometry shaders have no
     Metal equivalent.
   - *GX core*: FIFO parsing, GX state, shader keys/generation and render
     configuration build on POSIX (`galaxy_gx_core`) with their upstream
     tests. SSE kernels run on AArch64 through sse2neon (vendored, MIT).

## Next steps

7. **Renderer**: a Metal backend beside `renderer_d3d12.cpp` (~11k lines),
   HLSL uber-shaders → MSL. GX parsing/state above it is shared.
   `native_runtime.cpp` is compiled (`galaxy_native_runtime_objects`) but
   not yet linked: what it still needs is the `galaxy::gx` interface
   (`initialize`, `render_frame`, `pump_messages`, `peek_efb`,
   `present_cached_xfb`, the PE-completion waits, the settings overlay,
   about 40 functions in all) that `galaxy_gx` implements on D3D12.
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
- **Loading the game module**: iOS only runs code that is signed and inside
  the app bundle, so the game dylibs must be added to the IPA *before*
  SideStore/AltStore re-signs it (they sign bundled frameworks). Loading a
  module from the Documents folder at run time will not work.
- Fast-float NaN propagation differs between SSE and NEON in edge cases;
  fast float already trades exactness for speed.
