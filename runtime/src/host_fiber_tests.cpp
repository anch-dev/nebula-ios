// SPDX-License-Identifier: GPL-3.0-only
// Host fiber tests: switching, register and FP-control preservation,
// exceptions inside fibers, deep stacks and teardown.

#include "galaxy/host_fiber.h"

#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

using galaxy::host::Fiber;

Fiber g_boot = nullptr;

// --- ping-pong -------------------------------------------------------------

struct PingPong {
    Fiber self = nullptr;
    Fiber next = nullptr;
    int id = 0;
    std::vector<int>* log = nullptr;
};

void ping_pong_entry(void* raw) {
    auto* state = static_cast<PingPong*>(raw);
    for (int round = 0; round < 1000; ++round) {
        state->log->push_back(state->id);
        galaxy::host::fiber_switch(state->next);
    }
    galaxy::host::fiber_switch(g_boot);
    std::abort();
}

void test_ping_pong() {
    std::vector<int> log;
    PingPong states[3];
    Fiber fibers[3];
    for (int i = 0; i < 3; ++i) {
        states[i].id = i;
        states[i].log = &log;
        fibers[i] = galaxy::host::fiber_create(64 * 1024, 256 * 1024, &ping_pong_entry,
                                               &states[i]);
        check(fibers[i] != nullptr, "fiber_create");
    }
    // 0 -> 1 -> 2 -> boot after 2's first round, 0 -> 1 -> 2 ...
    states[0].next = fibers[1];
    states[1].next = fibers[2];
    states[2].next = g_boot;
    for (int round = 0; round < 1000; ++round) {
        galaxy::host::fiber_switch(fibers[0]);
        check(galaxy::host::fiber_current() == g_boot, "back on boot fiber");
    }
    check(log.size() == 3000u, "ping-pong ran every round");
    bool ordered = true;
    for (std::size_t i = 0; i < log.size(); ++i) ordered &= log[i] == static_cast<int>(i % 3u);
    check(ordered, "ping-pong order");
    for (Fiber fiber : fibers) galaxy::host::fiber_delete(fiber);
}

// --- callee-saved integer and vector registers -----------------------------

struct RegisterState {
    Fiber self = nullptr;
    std::uint64_t sum = 0u;
    double fsum = 0.0;
};

__attribute__((noinline)) void clobber_everything(Fiber target) {
    galaxy::host::fiber_switch(target);
}

void register_entry(void* raw) {
    auto* state = static_cast<RegisterState*>(raw);
    // Many live values across each switch force callee-saved registers.
    std::uint64_t a = 1, b = 2, c = 3, d = 5, e = 7, f = 11, g = 13, h = 17, i = 19, j = 23;
    double fa = 1.5, fb = 2.25, fc = 3.125, fd = 4.0625, fe = 5.5, ff = 6.75, fg = 7.875,
           fh = 8.0;
    for (int round = 0; round < 500; ++round) {
        a += b; b ^= c; c += d * 3; d += e; e ^= f << 1; f += g; g += h; h ^= i; i += j; j += a;
        fa = fa * 1.0001 + fb; fb = fb * 0.9999 + fc; fc += fd * 0.5; fd -= fe * 0.25;
        fe = fe * 1.0002 + ff; ff -= fg * 0.125; fg += fh * 0.0625; fh = fh * 0.9998 + fa * 1e-6;
        clobber_everything(g_boot);
    }
    state->sum = a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j;
    state->fsum = fa + fb + fc + fd + fe + ff + fg + fh;
    galaxy::host::fiber_switch(g_boot);
    std::abort();
}

void register_reference(std::uint64_t& sum, double& fsum) {
    std::uint64_t a = 1, b = 2, c = 3, d = 5, e = 7, f = 11, g = 13, h = 17, i = 19, j = 23;
    volatile double fa = 1.5, fb = 2.25, fc = 3.125, fd = 4.0625, fe = 5.5, ff = 6.75,
                    fg = 7.875, fh = 8.0;
    for (int round = 0; round < 500; ++round) {
        a += b; b ^= c; c += d * 3; d += e; e ^= f << 1; f += g; g += h; h ^= i; i += j; j += a;
        fa = fa * 1.0001 + fb; fb = fb * 0.9999 + fc; fc += fd * 0.5; fd -= fe * 0.25;
        fe = fe * 1.0002 + ff; ff -= fg * 0.125; fg += fh * 0.0625; fh = fh * 0.9998 + fa * 1e-6;
    }
    sum = a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j;
    fsum = fa + fb + fc + fd + fe + ff + fg + fh;
}

void test_registers() {
    RegisterState state;
    Fiber fiber = galaxy::host::fiber_create(64 * 1024, 256 * 1024, &register_entry, &state);
    check(fiber != nullptr, "fiber_create (registers)");
    // The boot side also keeps many values live across its switches.
    volatile std::uint64_t boot_x = 0x1234;
    double boot_f = 0.5;
    for (int round = 0; round < 501; ++round) {
        boot_x = boot_x * 31u + 7u;
        boot_f = boot_f * 1.5 + 0.25;
        if (boot_f > 1e6) boot_f = 0.5;
        clobber_everything(fiber);
    }
    std::uint64_t expected_sum = 0;
    double expected_fsum = 0;
    register_reference(expected_sum, expected_fsum);
    check(state.sum == expected_sum, "integer registers survive switches");
    // Same operations in the same order: bit-exact unless an FP register was lost.
    check(state.fsum == expected_fsum, "vector registers survive switches");
    galaxy::host::fiber_delete(fiber);
}

// --- FP control is per fiber ----------------------------------------------

struct FpState {
    std::uint64_t seen_at_start = 0u;
    int rounding_after_resume = 0;
};

void fp_entry(void* raw) {
    auto* state = static_cast<FpState*>(raw);
    state->seen_at_start = galaxy::host::fp_control_get();
    std::fesetround(FE_TOWARDZERO);
    galaxy::host::fiber_switch(g_boot);
    // The boot fiber changed its own mode meanwhile; ours must be unchanged.
    state->rounding_after_resume = std::fegetround();
    galaxy::host::fiber_switch(g_boot);
    std::abort();
}

void test_fp_control() {
    std::fesetround(FE_TONEAREST);
    const std::uint64_t boot_control = galaxy::host::fp_control_get();
    FpState state;
    Fiber fiber = galaxy::host::fiber_create(64 * 1024, 256 * 1024, &fp_entry, &state);
    galaxy::host::fiber_switch(fiber);
    check(state.seen_at_start == boot_control, "new fiber inherits the creator's FP control");
    check(std::fegetround() == FE_TONEAREST, "boot rounding unaffected by the fiber");
    std::fesetround(FE_UPWARD);
    galaxy::host::fiber_switch(fiber);
    check(state.rounding_after_resume == FE_TOWARDZERO, "fiber rounding survives a switch");
    check(std::fegetround() == FE_UPWARD, "boot rounding survives a switch");
    std::fesetround(FE_TONEAREST);
    galaxy::host::fiber_delete(fiber);

    // Flush-to-zero: the smallest normal float halved becomes 0, not subnormal.
    const std::uint64_t saved = galaxy::host::fp_control_get();
    galaxy::host::fp_enable_flush_denormals();
    volatile float tiny = 1.17549435e-38F;
    volatile float half = tiny * 0.5F;
    check(half == 0.0F, "flush-to-zero applies");
    galaxy::host::fp_control_set(saved);
    volatile float half_again = tiny * 0.5F;
    check(half_again != 0.0F, "fp_control_set restores subnormals");
}

// --- exceptions and deep stacks inside a fiber -------------------------------

struct ExceptionState {
    int caught = 0;
    std::string message;
    std::uint64_t depth_sum = 0u;
};

__attribute__((noinline)) std::uint64_t recurse(int depth) {
    volatile char pad[512];
    pad[0] = static_cast<char>(depth);
    if (depth == 0) return pad[0];
    return recurse(depth - 1) + static_cast<unsigned char>(pad[0]);
}

__attribute__((noinline)) void thrower(int value) {
    if (value > 2) throw std::runtime_error("fiber error " + std::to_string(value));
}

void exception_entry(void* raw) {
    auto* state = static_cast<ExceptionState*>(raw);
    for (int i = 0; i < 5; ++i) {
        try {
            thrower(i);
        } catch (const std::runtime_error& error) {
            ++state->caught;
            state->message = error.what();
        }
        galaxy::host::fiber_switch(g_boot);
    }
    // ~1 MiB of frames on a 2 MiB stack.
    state->depth_sum = recurse(2000);
    galaxy::host::fiber_switch(g_boot);
    std::abort();
}

void test_exceptions_and_depth() {
    ExceptionState state;
    Fiber fiber = galaxy::host::fiber_create(256 * 1024, 2 * 1024 * 1024, &exception_entry,
                                             &state);
    for (int i = 0; i < 6; ++i) galaxy::host::fiber_switch(fiber);
    check(state.caught == 2, "exceptions caught inside the fiber");
    check(state.message == "fiber error 4", "exception payload intact");
    std::uint64_t expected = 0;
    for (int depth = 0; depth <= 2000; ++depth) {
        expected += static_cast<unsigned char>(static_cast<char>(depth));
    }
    expected -= static_cast<unsigned char>(0);  // depth 0 contributes its pad value once
    check(state.depth_sum == expected, "deep recursion on a fiber stack");
    galaxy::host::fiber_delete(fiber);
}

// --- many fibers, created and deleted repeatedly ---------------------------

void churn_entry(void* raw) {
    ++*static_cast<int*>(raw);
    galaxy::host::fiber_switch(g_boot);
    std::abort();
}

void test_churn() {
    int ran = 0;
    for (int batch = 0; batch < 20; ++batch) {
        std::vector<Fiber> fibers;
        for (int i = 0; i < 32; ++i) {
            fibers.push_back(galaxy::host::fiber_create(256 * 1024, 8 * 1024 * 1024,
                                                        &churn_entry, &ran));
        }
        for (Fiber fiber : fibers) galaxy::host::fiber_switch(fiber);
        for (Fiber fiber : fibers) galaxy::host::fiber_delete(fiber);
    }
    check(ran == 640, "640 fibers created, run and deleted");
}

}  // namespace

int main() {
    g_boot = galaxy::host::fiber_convert_thread();
    check(g_boot != nullptr, "fiber_convert_thread");
    check(galaxy::host::fiber_current() == g_boot, "fiber_current after convert");
    test_ping_pong();
    test_registers();
    test_fp_control();
    test_exceptions_and_depth();
    test_churn();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d host fiber check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("host fiber tests passed");
    return 0;
}
