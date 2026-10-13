// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Host fibers: the subset of the Win32 fiber API the cooperative scheduler
// uses, with the same semantics on every platform.
//
// - Windows: CreateFiberEx / SwitchToFiber with FIBER_FLAG_FLOAT_SWITCH.
// - POSIX (Linux, macOS, iOS) on x86-64 and AArch64: a small assembly context
//   switch that saves the callee-saved registers and the floating-point
//   control state (MXCSR and the x87 control word, or FPCR), so each fiber
//   keeps its own rounding and flush-to-zero mode exactly as a float-switching
//   Win32 fiber does.
//
// A fiber's entry function must never return; like a Win32 fiber procedure,
// returning would end the thread. Here it aborts with a message instead.
// Exceptions must not propagate out of the entry function.

#include <cstddef>
#include <cstdint>

namespace galaxy::host {

using FiberEntry = void (*)(void* parameter);
using Fiber = void*;

// Turns the calling thread into its first fiber and returns its handle. Call
// once per thread before creating or switching fibers on it.
[[nodiscard]] Fiber fiber_convert_thread();

// Creates a fiber that runs `entry(parameter)` when first switched to.
// `stack_commit` is a hint (Windows commits it up front); `stack_reserve` is
// the stack size. Returns nullptr on failure.
[[nodiscard]] Fiber fiber_create(std::size_t stack_commit, std::size_t stack_reserve,
                                 FiberEntry entry, void* parameter) noexcept;

// Saves the running fiber and resumes `target`. Returns when another fiber
// switches back to the caller.
void fiber_switch(Fiber target) noexcept;

// The running fiber on this thread (nullptr before fiber_convert_thread).
[[nodiscard]] Fiber fiber_current() noexcept;

// Frees a fiber that is not running. Freeing the running fiber is an error.
void fiber_delete(Fiber fiber) noexcept;

// Host floating-point control state (x86-64: MXCSR; AArch64: FPCR), as an
// opaque value to save and restore.
[[nodiscard]] std::uint64_t fp_control_get() noexcept;
void fp_control_set(std::uint64_t value) noexcept;

// Flushes subnormal inputs and outputs to zero on the calling thread
// (MXCSR FTZ|DAZ, or FPCR.FZ), the mode the fast-float game module runs in.
void fp_enable_flush_denormals() noexcept;

// Last platform error code for diagnostics (GetLastError or errno).
[[nodiscard]] unsigned long last_error() noexcept;

}  // namespace galaxy::host
