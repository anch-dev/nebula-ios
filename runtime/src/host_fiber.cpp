// SPDX-License-Identifier: GPL-3.0-only
#include "galaxy/host_fiber.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#define GALAXY_HOST_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define GALAXY_HOST_AARCH64 1
#else
#error "host fibers support x86-64 and AArch64 hosts"
#endif

namespace galaxy::host {

// ---------------------------------------------------------------------------
// Floating-point control state
// ---------------------------------------------------------------------------

#if GALAXY_HOST_X86_64
std::uint64_t fp_control_get() noexcept { return _mm_getcsr(); }
void fp_control_set(std::uint64_t value) noexcept {
    _mm_setcsr(static_cast<unsigned int>(value));
}
void fp_enable_flush_denormals() noexcept {
    // FTZ (bit 15) | DAZ (bit 6).
    _mm_setcsr(_mm_getcsr() | 0x8040u);
}
#elif defined(_MSC_VER)
#error "Windows on ARM is not a supported host"
#else
std::uint64_t fp_control_get() noexcept {
    std::uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}
void fp_control_set(std::uint64_t value) noexcept {
    __asm__ volatile("msr fpcr, %0" : : "r"(value));
}
void fp_enable_flush_denormals() noexcept {
    // FPCR.FZ (bit 24) flushes subnormal inputs and results of single and
    // double precision arithmetic, the AArch64 equivalent of FTZ|DAZ.
    fp_control_set(fp_control_get() | (std::uint64_t{1} << 24));
}
#endif

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Windows: native fibers
// ---------------------------------------------------------------------------

namespace {
struct WinStart {
    FiberEntry entry;
    void* parameter;
};

void WINAPI win_fiber_start(void* raw) {
    // The start record is only needed until the fiber first runs.
    auto* start = static_cast<WinStart*>(raw);
    const FiberEntry entry = start->entry;
    void* const parameter = start->parameter;
    delete start;
    entry(parameter);
    std::fputs("galaxy: host fiber entry returned\n", stderr);
    std::abort();
}
}  // namespace

Fiber fiber_convert_thread() {
    return ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
}

Fiber fiber_create(std::size_t stack_commit, std::size_t stack_reserve, FiberEntry entry,
                   void* parameter) noexcept {
    auto* start = new (std::nothrow) WinStart{entry, parameter};
    if (start == nullptr) return nullptr;
    void* const fiber = CreateFiberEx(stack_commit, stack_reserve, FIBER_FLAG_FLOAT_SWITCH,
                                      &win_fiber_start, start);
    if (fiber == nullptr) delete start;
    return fiber;
}

void fiber_switch(Fiber target) noexcept { SwitchToFiber(target); }

Fiber fiber_current() noexcept {
    // GetCurrentFiber returns garbage (0x1E00) on a thread that was never
    // converted; IsThreadAFiber guards it.
    return IsThreadAFiber() ? GetCurrentFiber() : nullptr;
}

void fiber_delete(Fiber fiber) noexcept { DeleteFiber(fiber); }

unsigned long last_error() noexcept { return GetLastError(); }

#else

// ---------------------------------------------------------------------------
// POSIX: assembly context switch
// ---------------------------------------------------------------------------

namespace detail {

struct PosixFiber {
    void* sp = nullptr;            // saved stack pointer while not running
    void* stack_base = nullptr;    // mmap base (guard page first); null for a thread fiber
    std::size_t stack_bytes = 0u;  // whole mapping, guard included
    FiberEntry entry = nullptr;
    void* parameter = nullptr;
};

}  // namespace detail

namespace {
using detail::PosixFiber;
thread_local PosixFiber* t_current = nullptr;
}  // namespace
}  // namespace galaxy::host

extern "C" {
// Saves the callee-saved registers and FP control on the current stack, stores
// the stack pointer to *save_sp, then restores the same from new_sp.
void galaxy_fiber_swap(void** save_sp, void* new_sp);
// First return address of a new fiber: calls galaxy_fiber_start(fiber).
void galaxy_fiber_trampoline();

[[noreturn]] void galaxy_fiber_start(galaxy::host::detail::PosixFiber* fiber) {
    fiber->entry(fiber->parameter);
    std::fputs("galaxy: host fiber entry returned\n", stderr);
    std::abort();
}
}

#if defined(__APPLE__)
#define GALAXY_ASM_BEGIN(name) \
    ".text\n.private_extern _" #name "\n.globl _" #name "\n.p2align 4\n_" #name ":\n"
#define GALAXY_ASM_END(name) ""
#else
#define GALAXY_ASM_BEGIN(name)                                                   \
    ".text\n.hidden " #name "\n.globl " #name "\n.type " #name ",%function\n" \
    ".p2align 4\n" #name ":\n"
#define GALAXY_ASM_END(name) ".size " #name ", .-" #name "\n"
#endif

#if GALAXY_HOST_X86_64
// System V x86-64. Frame (low to high): MXCSR(4) x87 CW(2) pad(2), r15, r14,
// r13, r12, rbx, rbp, return address. 64 bytes.
__asm__(
    GALAXY_ASM_BEGIN(galaxy_fiber_swap)
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $8, %rsp\n"
    "    stmxcsr (%rsp)\n"
    "    fnstcw 4(%rsp)\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    ldmxcsr (%rsp)\n"
    "    fldcw 4(%rsp)\n"
    "    addq $8, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    GALAXY_ASM_END(galaxy_fiber_swap)
    GALAXY_ASM_BEGIN(galaxy_fiber_trampoline)
    "    .cfi_startproc\n"
    "    .cfi_undefined %rip\n"
    "    movq %r12, %rdi\n"
    "    callq *%r13\n"
    "    ud2\n"
    "    .cfi_endproc\n"
    GALAXY_ASM_END(galaxy_fiber_trampoline));
#else
// AAPCS64 (Linux and Apple). Frame: x19..x28, x29, x30 (96 bytes), d8..d15
// (64 bytes), FPCR (8 bytes), pad (8 bytes). 176 bytes. x18 is left alone:
// Apple reserves it.
__asm__(
    GALAXY_ASM_BEGIN(galaxy_fiber_swap)
    "    sub sp, sp, #176\n"
    "    stp x19, x20, [sp, #0]\n"
    "    stp x21, x22, [sp, #16]\n"
    "    stp x23, x24, [sp, #32]\n"
    "    stp x25, x26, [sp, #48]\n"
    "    stp x27, x28, [sp, #64]\n"
    "    stp x29, x30, [sp, #80]\n"
    "    stp d8, d9, [sp, #96]\n"
    "    stp d10, d11, [sp, #112]\n"
    "    stp d12, d13, [sp, #128]\n"
    "    stp d14, d15, [sp, #144]\n"
    "    mrs x9, fpcr\n"
    "    str x9, [sp, #160]\n"
    "    mov x9, sp\n"
    "    str x9, [x0]\n"
    "    mov sp, x1\n"
    "    ldr x9, [sp, #160]\n"
    "    msr fpcr, x9\n"
    "    ldp x19, x20, [sp, #0]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x23, x24, [sp, #32]\n"
    "    ldp x25, x26, [sp, #48]\n"
    "    ldp x27, x28, [sp, #64]\n"
    "    ldp x29, x30, [sp, #80]\n"
    "    ldp d8, d9, [sp, #96]\n"
    "    ldp d10, d11, [sp, #112]\n"
    "    ldp d12, d13, [sp, #128]\n"
    "    ldp d14, d15, [sp, #144]\n"
    "    add sp, sp, #176\n"
    "    ret\n"
    GALAXY_ASM_END(galaxy_fiber_swap)
    GALAXY_ASM_BEGIN(galaxy_fiber_trampoline)
    "    .cfi_startproc\n"
    "    .cfi_undefined x30\n"
    "    mov x0, x19\n"
    "    blr x20\n"
    "    brk #0\n"
    "    .cfi_endproc\n"
    GALAXY_ASM_END(galaxy_fiber_trampoline));
#endif

namespace galaxy::host {
namespace {

std::size_t page_size() noexcept {
    static const std::size_t size = [] {
        const long value = sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<std::size_t>(value) : std::size_t{16384};
    }();
    return size;
}

}  // namespace

Fiber fiber_convert_thread() {
    if (t_current != nullptr) return t_current;
    t_current = new PosixFiber{};
    return t_current;
}

Fiber fiber_create(std::size_t /*stack_commit*/, std::size_t stack_reserve, FiberEntry entry,
                   void* parameter) noexcept {
    const std::size_t page = page_size();
    const std::size_t usable = (stack_reserve + page - 1u) / page * page;
    const std::size_t bytes = usable + page;  // one guard page below the stack
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* const base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (base == MAP_FAILED) return nullptr;
    if (mprotect(base, page, PROT_NONE) != 0) {
        munmap(base, bytes);
        return nullptr;
    }

    auto* fiber = new (std::nothrow) PosixFiber{};
    if (fiber == nullptr) {
        munmap(base, bytes);
        return nullptr;
    }
    fiber->stack_base = base;
    fiber->stack_bytes = bytes;
    fiber->entry = entry;
    fiber->parameter = parameter;

    // The new fiber inherits the creator's FP control, as a Win32
    // float-switching fiber does.
    const std::uint64_t fp_control = fp_control_get();
    auto top = reinterpret_cast<std::uintptr_t>(base) + bytes;
    top &= ~std::uintptr_t{15};
    auto* frame = reinterpret_cast<std::uint64_t*>(top);

#if GALAXY_HOST_X86_64
    frame -= 8;  // 64-byte frame; after `ret` the stack is 16-byte aligned
    std::uint16_t x87_control;
    __asm__ volatile("fnstcw %0" : "=m"(x87_control));
    frame[0] = static_cast<std::uint32_t>(fp_control) |
               (static_cast<std::uint64_t>(x87_control) << 32);
    frame[1] = 0u;                                                  // r15
    frame[2] = 0u;                                                  // r14
    frame[3] = reinterpret_cast<std::uint64_t>(&galaxy_fiber_start);  // r13
    frame[4] = reinterpret_cast<std::uint64_t>(fiber);               // r12
    frame[5] = 0u;                                                  // rbx
    frame[6] = 0u;                                                  // rbp
    frame[7] = reinterpret_cast<std::uint64_t>(&galaxy_fiber_trampoline);
#else
    frame -= 22;  // 176 bytes
    for (int i = 0; i < 22; ++i) frame[i] = 0u;
    frame[0] = reinterpret_cast<std::uint64_t>(fiber);                // x19
    frame[1] = reinterpret_cast<std::uint64_t>(&galaxy_fiber_start);   // x20
    frame[10] = 0u;                                                   // x29
    frame[11] = reinterpret_cast<std::uint64_t>(&galaxy_fiber_trampoline);  // x30
    frame[20] = fp_control;                                           // FPCR
#endif
    fiber->sp = frame;
    return fiber;
}

void fiber_switch(Fiber target) noexcept {
    PosixFiber* const self = t_current;
    auto* const next = static_cast<PosixFiber*>(target);
    if (self == nullptr || next == nullptr) {
        std::fputs("galaxy: fiber_switch without a converted thread\n", stderr);
        std::abort();
    }
    if (next == self) return;
    t_current = next;
    galaxy_fiber_swap(&self->sp, next->sp);
}

Fiber fiber_current() noexcept { return t_current; }

void fiber_delete(Fiber fiber) noexcept {
    auto* const target = static_cast<PosixFiber*>(fiber);
    if (target == nullptr) return;
    if (target == t_current) {
        // ConvertThreadToFiber's fiber deleting itself ends the thread on
        // Windows; here it only releases the record.
        t_current = nullptr;
    }
    if (target->stack_base != nullptr) munmap(target->stack_base, target->stack_bytes);
    delete target;
}

unsigned long last_error() noexcept { return static_cast<unsigned long>(errno); }

#endif

}  // namespace galaxy::host
