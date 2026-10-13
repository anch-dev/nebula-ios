// SPDX-License-Identifier: GPL-3.0-only
// POSIX flat guest memory: alias mirroring, and (on AArch64) fastmem fault
// emulation for every load/store form the handler decodes, against a fake
// device at the MMIO base and the locked cache.

#include "galaxy/flat_guest_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace flat = galaxy::host::flat_guest_memory;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

void check_eq(std::uint64_t actual, std::uint64_t expected, const char* what) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s: got 0x%llx, expected 0x%llx\n", what,
                     static_cast<unsigned long long>(actual),
                     static_cast<unsigned long long>(expected));
        ++g_failures;
    }
}

void test_aliases() {
    // volatile: the compiler cannot know two aliases share a page, so it may
    // otherwise reorder a load through one alias ahead of a store through another.
    volatile std::byte* const base = flat::guest_base();
    check(base != nullptr, "guest base after initialize");
    std::byte* const mem1 = flat::mem1_host_view();
    std::byte* const mem2 = flat::mem2_host_view();
    mem1[0x1234] = std::byte{0xA5};
    mem1[flat::kMem1Size - 1u] = std::byte{0x5A};
    mem2[0x4321] = std::byte{0x6C};
    for (const std::uint32_t alias : {0x00000000u, 0x80000000u, 0xC0000000u}) {
        check(base[alias + 0x1234u] == std::byte{0xA5}, "MEM1 alias mirrors the host view");
        check(base[alias + flat::kMem1Size - 1u] == std::byte{0x5A}, "MEM1 alias end");
    }
    for (const std::uint32_t alias : {0x10000000u, 0x90000000u, 0xD0000000u}) {
        check(base[alias + 0x4321u] == std::byte{0x6C}, "MEM2 alias mirrors the host view");
    }
    // A write through one alias appears in the others.
    base[0x80000000u + 0x10u] = std::byte{0x11};
    check(base[0xC0000010u] == std::byte{0x11} && mem1[0x10] == std::byte{0x11},
          "writes through an alias are shared");
    check(flat::host_pointer(0x80001234u, 4u) == mem1 + 0x1234, "host_pointer MEM1");
    check(flat::host_pointer(0x90004321u, 1u) == mem2 + 0x4321, "host_pointer MEM2");
    check(flat::host_pointer(0x00000000u, 0u) == nullptr, "zero-sized span rejected");
    check(flat::host_pointer(0xFFFFFFFFu, 2u) == nullptr, "overflow rejected");
    check(flat::host_pointer(0xE0000000u, 1u) == nullptr, "locked cache is not RAM");
    check(flat::host_pointer(0x817FFFFFu, 2u) == nullptr, "span past MEM1 rejected");

    flat::OwnerLease first;
    flat::OwnerLease second;
    check(first.acquire(), "first owner acquires");
    check(!second.acquire(), "second live owner refused");
    check(mem1[0x1234] == std::byte{0}, "owner starts from zeroed RAM");
}

#if defined(__aarch64__)

// Fake device: 64 KiB of registers at 0xCC000000 and the 16 KiB locked cache.
constexpr std::uint32_t kDeviceBase = 0xCC000000u;
constexpr std::uint32_t kDeviceSize = 0x10000u;
constexpr std::uint32_t kCacheBase = 0xE0000000u;
constexpr std::uint32_t kCacheSize = 0x4000u;
std::uint8_t g_device[kDeviceSize];
std::uint8_t g_cache[kCacheSize];
int g_device_reads = 0;
int g_device_writes = 0;
std::vector<std::uint32_t> g_write_sizes;

std::uint8_t* device_bytes(std::uint32_t address, std::uint32_t size) {
    if (address >= kDeviceBase && address - kDeviceBase + size <= kDeviceSize) {
        return g_device + (address - kDeviceBase);
    }
    if (address >= kCacheBase && address - kCacheBase + size <= kCacheSize) {
        return g_cache + (address - kCacheBase);
    }
    return nullptr;
}

bool device_read(void*, std::uint32_t address, std::uint32_t size, std::byte* image) noexcept {
    // Like real MMIO: at most 8 bytes per access; wider ones arrive split.
    if (size > 8u) return false;
    std::uint8_t* const bytes = device_bytes(address, size);
    if (bytes == nullptr) return false;
    std::memcpy(image, bytes, size);
    ++g_device_reads;
    return true;
}

bool device_write(void*, std::uint32_t address, std::uint32_t size,
                  const std::byte* image) noexcept {
    if (size > 8u) return false;
    std::uint8_t* const bytes = device_bytes(address, size);
    if (bytes == nullptr) return false;
    std::memcpy(bytes, image, size);
    ++g_device_writes;
    g_write_sizes.push_back(size);
    return true;
}

void reset_device() {
    for (std::uint32_t i = 0; i < kDeviceSize; ++i) g_device[i] = static_cast<std::uint8_t>(i * 7u + 3u);
    for (std::uint32_t i = 0; i < kCacheSize; ++i) g_cache[i] = static_cast<std::uint8_t>(i ^ 0x5Au);
    g_write_sizes.clear();
}

std::uint64_t device_le(std::uint32_t offset, std::uint32_t size) {
    std::uint64_t value = 0u;
    std::memcpy(&value, g_device + offset, size);
    return value;
}

void test_integer_loads(std::byte* dev) {
    std::uint64_t r = 0;
    asm volatile("ldr %w0, [%1, #8]" : "=r"(r) : "r"(dev) : "memory");
    check_eq(r, device_le(8, 4), "ldr w, [x, #imm]");
    asm volatile("ldr %0, [%1, #16]" : "=r"(r) : "r"(dev) : "memory");
    check_eq(r, device_le(16, 8), "ldr x, [x, #imm]");
    asm volatile("ldrb %w0, [%1, #3]" : "=r"(r) : "r"(dev) : "memory");
    check_eq(r, device_le(3, 1), "ldrb");
    asm volatile("ldrh %w0, [%1, #6]" : "=r"(r) : "r"(dev) : "memory");
    check_eq(r, device_le(6, 2), "ldrh");

    // Signed loads: plant negative values.
    g_device[0x40] = 0x80u;
    g_device[0x42] = 0x34u; g_device[0x43] = 0x92u;
    std::memcpy(g_device + 0x44, "\xF0\xFF\xFF\xFF", 4);
    asm volatile("ldrsb %w0, [%1, #0x40]" : "=r"(r) : "r"(dev) : "memory");
    check_eq(r, 0xFFFFFF80u, "ldrsb w (upper half zero)");
    asm volatile("ldrsb %0, [%1, #0x40]" : "=r"(r) : "r"(dev) : "memory");
    check_eq(r, 0xFFFFFFFFFFFFFF80ull, "ldrsb x");
    asm volatile("ldrsh %0, [%1, #0x42]" : "=r"(r) : "r"(dev) : "memory");
    check_eq(r, 0xFFFFFFFFFFFF9234ull, "ldrsh x");
    asm volatile("ldrsw %0, [%1, #0x44]" : "=r"(r) : "r"(dev) : "memory");
    check_eq(r, 0xFFFFFFFFFFFFFFF0ull, "ldrsw");

    // A W load clears the upper half of a register holding junk.
    r = ~0ull;
    asm volatile("ldr %w0, [%1, #8]" : "+r"(r) : "r"(dev) : "memory");
    check_eq(r, device_le(8, 4), "ldr w zero-extends");

    // Unscaled, pre- and post-index.
    std::byte* p = dev + 0x20;
    asm volatile("ldur %w0, [%1, #-3]" : "=r"(r) : "r"(p) : "memory");
    check_eq(r, device_le(0x1D, 4), "ldur negative offset");
    asm volatile("ldr %w0, [%1], #12" : "=r"(r), "+r"(p) : : "memory");
    check_eq(r, device_le(0x20, 4), "ldr post-index value");
    check(p == dev + 0x2C, "ldr post-index writeback");
    asm volatile("ldr %0, [%1, #-4]!" : "=r"(r), "+r"(p) : : "memory");
    check_eq(r, device_le(0x28, 8), "ldr pre-index value");
    check(p == dev + 0x28, "ldr pre-index writeback");

    // Register offset with each extend.
    std::uint64_t index = 5;
    asm volatile("ldr %w0, [%1, %w2, uxtw #2]" : "=r"(r) : "r"(dev), "r"(index) : "memory");
    check_eq(r, device_le(20, 4), "ldr [x, w, uxtw #2]");
    asm volatile("ldr %0, [%1, %2, lsl #3]" : "=r"(r) : "r"(dev), "r"(index) : "memory");
    check_eq(r, device_le(40, 8), "ldr [x, x, lsl #3]");
    std::int64_t negative = -8;
    std::byte* mid = dev + 0x100;
    asm volatile("ldrh %w0, [%1, %w2, sxtw]" : "=r"(r) : "r"(mid), "r"(negative) : "memory");
    check_eq(r, device_le(0xF8, 2), "ldrh [x, w, sxtw]");
    asm volatile("ldrb %w0, [%1, %2]" : "=r"(r) : "r"(mid), "r"(negative) : "memory");
    check_eq(r, device_le(0xF8, 1), "ldrb [x, x]");
}

void test_integer_stores(std::byte* dev) {
    reset_device();
    const std::uint64_t value = 0x1122334455667788ull;
    asm volatile("str %w0, [%1, #0x200]" : : "r"(value), "r"(dev) : "memory");
    check_eq(device_le(0x200, 4), 0x55667788u, "str w");
    asm volatile("str %0, [%1, #0x208]" : : "r"(value), "r"(dev) : "memory");
    check_eq(device_le(0x208, 8), value, "str x");
    asm volatile("strb %w0, [%1, #0x210]" : : "r"(value), "r"(dev) : "memory");
    check_eq(device_le(0x210, 1), 0x88u, "strb");
    asm volatile("strh %w0, [%1, #0x212]" : : "r"(value), "r"(dev) : "memory");
    check_eq(device_le(0x212, 2), 0x7788u, "strh");
    asm volatile("str wzr, [%0, #0x214]" : : "r"(dev) : "memory");
    check_eq(device_le(0x214, 4), 0u, "str wzr");
    std::byte* p = dev + 0x220;
    asm volatile("stur %w1, [%0, #-1]" : : "r"(p), "r"(value) : "memory");
    check_eq(device_le(0x21F, 4), 0x55667788u, "stur");
    asm volatile("str %w1, [%0], #4" : "+r"(p) : "r"(value) : "memory");
    check(p == dev + 0x224, "str post-index writeback");
    std::uint64_t idx = 3;
    asm volatile("str %w0, [%1, %2, lsl #2]" : : "r"(value), "r"(dev), "r"(idx) : "memory");
    check_eq(device_le(12, 4), 0x55667788u, "str [x, x, lsl #2]");
}

void test_pairs(std::byte* dev) {
    reset_device();
    std::uint64_t a = 0, b = 0;
    asm volatile("ldp %w0, %w1, [%2, #8]" : "=r"(a), "=r"(b) : "r"(dev) : "memory");
    check_eq(a, device_le(8, 4), "ldp w first");
    check_eq(b, device_le(12, 4), "ldp w second");
    asm volatile("ldp %0, %1, [%2, #16]" : "=r"(a), "=r"(b) : "r"(dev) : "memory");
    check_eq(a, device_le(16, 8), "ldp x first");
    check_eq(b, device_le(24, 8), "ldp x second");
    std::memcpy(g_device + 0x80, "\xFE\xFF\xFF\xFF\x01\x00\x00\x00", 8);
    asm volatile("ldpsw %0, %1, [%2, #0x80]" : "=r"(a), "=r"(b) : "r"(dev) : "memory");
    check_eq(a, 0xFFFFFFFFFFFFFFFEull, "ldpsw sign-extends");
    check_eq(b, 1u, "ldpsw second");
    std::byte* p = dev + 0x40;
    asm volatile("ldp %w0, %w1, [%2], #-8" : "=r"(a), "=r"(b), "+r"(p) : : "memory");
    check_eq(a, device_le(0x40, 4), "ldp post-index value");
    check(p == dev + 0x38, "ldp post-index writeback");

    const std::uint64_t x = 0xAAAABBBBCCCCDDDDull;
    const std::uint64_t y = 0x1111222233334444ull;
    asm volatile("stp %0, %1, [%2, #0x100]" : : "r"(x), "r"(y), "r"(dev + 0x200) : "memory");
    check_eq(device_le(0x300, 8), x, "stp x first");
    check_eq(device_le(0x308, 8), y, "stp x second");
    g_write_sizes.clear();
    p += 0x300;
    asm volatile("stp %w1, %w2, [%0, #0x10]!" : "+r"(p) : "r"(x), "r"(y) : "memory");
    check_eq(device_le(0x38 + 0x310, 4), 0xCCCCDDDDu, "stp w pre-index first");
    check_eq(device_le(0x38 + 0x314, 4), 0x33334444u, "stp w pre-index second");
    check((g_write_sizes == std::vector<std::uint32_t>{8u}), "stp w is one 8-byte device write");
}

void test_simd(std::byte* dev) {
    reset_device();
    uint32x4_t v = vdupq_n_u32(0xFFFFFFFFu);
    asm volatile("ldr %s0, [%1, #4]" : "+w"(v) : "r"(dev) : "memory");
    check_eq(vgetq_lane_u32(v, 0), device_le(4, 4), "ldr s value");
    check(vgetq_lane_u32(v, 1) == 0u && vgetq_lane_u32(v, 3) == 0u, "ldr s zeroes the rest");
    v = vdupq_n_u32(0xFFFFFFFFu);
    asm volatile("ldr %d0, [%1, #8]" : "+w"(v) : "r"(dev) : "memory");
    check_eq(vgetq_lane_u64(vreinterpretq_u64_u32(v), 0), device_le(8, 8), "ldr d value");
    check(vgetq_lane_u64(vreinterpretq_u64_u32(v), 1) == 0u, "ldr d zeroes the rest");
    asm volatile("ldr %q0, [%1, #32]" : "=w"(v) : "r"(dev) : "memory");
    check_eq(vgetq_lane_u64(vreinterpretq_u64_u32(v), 0), device_le(32, 8), "ldr q low");
    check_eq(vgetq_lane_u64(vreinterpretq_u64_u32(v), 1), device_le(40, 8), "ldr q high (split)");
    asm volatile("ldr %b0, [%1, #5]" : "=w"(v) : "r"(dev) : "memory");
    check_eq(vgetq_lane_u32(v, 0), device_le(5, 1), "ldr b");
    asm volatile("ldr %h0, [%1, #6]" : "=w"(v) : "r"(dev) : "memory");
    check_eq(vgetq_lane_u32(v, 0), device_le(6, 2), "ldr h");

    const uint32x4_t pattern = {0x01020304u, 0x05060708u, 0x090A0B0Cu, 0x0D0E0F10u};
    g_write_sizes.clear();
    asm volatile("str %q0, [%1, #0x400]" : : "w"(pattern), "r"(dev) : "memory");
    check_eq(device_le(0x400, 8), 0x0506070801020304ull, "str q low");
    check_eq(device_le(0x408, 8), 0x0D0E0F10090A0B0Cull, "str q high");
    check((g_write_sizes == std::vector<std::uint32_t>{4u, 4u, 4u, 4u}),
          "a 16-byte store reaches the device as ascending 4-byte pieces");
    asm volatile("str %s0, [%1, #0x410]" : : "w"(pattern), "r"(dev) : "memory");
    check_eq(device_le(0x410, 4), 0x01020304u, "str s");
    asm volatile("stp %d0, %d1, [%2, #0x20]" : : "w"(pattern), "w"(pattern), "r"(dev + 0x400) : "memory");
    check_eq(device_le(0x420, 8), 0x0506070801020304ull, "stp d first");
    check_eq(device_le(0x428, 8), 0x0506070801020304ull, "stp d second");
    uint32x4_t lo, hi;
    asm volatile("ldp %s0, %s1, [%2, #0x20]" : "=w"(lo), "=w"(hi) : "r"(dev + 0x400) : "memory");
    check_eq(vgetq_lane_u32(lo, 0), 0x01020304u, "ldp s first");
    check_eq(vgetq_lane_u32(hi, 0), 0x05060708u, "ldp s second");
    std::uint64_t idx = 2;
    asm volatile("ldr %s0, [%1, %2, lsl #2]" : "=w"(v) : "r"(dev), "r"(idx) : "memory");
    check_eq(vgetq_lane_u32(v, 0), device_le(8, 4), "ldr s [x, x, lsl #2]");

    // Lane forms keep the other lanes.
    v = vdupq_n_u32(0xEEEEEEEEu);
    asm volatile("ld1 {%0.s}[1], [%1]" : "+w"(v) : "r"(dev + 0x10) : "memory");
    check_eq(vgetq_lane_u32(v, 1), device_le(0x10, 4), "ld1 lane value");
    check(vgetq_lane_u32(v, 0) == 0xEEEEEEEEu && vgetq_lane_u32(v, 2) == 0xEEEEEEEEu,
          "ld1 keeps other lanes");
    std::byte* p = dev + 0x500;
    asm volatile("st1 {%1.h}[3], [%0], #2" : "+r"(p) : "w"(pattern) : "memory");
    check_eq(device_le(0x500, 2), 0x0506u, "st1 lane post-index value");
    check(p == dev + 0x502, "st1 post-index writeback");
}

void test_locked_cache(std::byte* base) {
    std::uint64_t r = 0;
    std::byte* const cache = base + kCacheBase;
    asm volatile("ldr %w0, [%1, #0x100]" : "=r"(r) : "r"(cache) : "memory");
    std::uint64_t expected = 0;
    std::memcpy(&expected, g_cache + 0x100, 4);
    check_eq(r, expected, "locked cache load");
}

void test_faults() {
    reset_device();
    flat::FaultAccessCallbacks callbacks;
    callbacks.read = &device_read;
    callbacks.write = &device_write;
    callbacks.thread_id = flat::current_thread_id();
    flat::install_fault_handler(callbacks);
    std::byte* const base = flat::guest_base();
    std::byte* const dev = base + kDeviceBase;
    test_integer_loads(dev);
    test_integer_stores(dev);
    test_pairs(dev);
    test_simd(dev);
    test_locked_cache(base);
    const flat::FaultStats stats = flat::fault_stats();
    check(stats.unhandled == 0u, flat::last_unhandled_fault_message());
    if (stats.handled_reads < 25u || stats.handled_writes < 10u) {
        std::fprintf(stderr, "reads %llu writes %llu\n",
                     static_cast<unsigned long long>(stats.handled_reads),
                     static_cast<unsigned long long>(stats.handled_writes));
    }
    check(stats.handled_reads >= 25u && stats.handled_writes >= 10u, "faults were counted");
    check(stats.mmio > 0u && stats.locked_cache == 1u, "MMIO and locked-cache counters");
    // RAM accesses never fault.
    const std::uint64_t before = stats.handled_reads;
    volatile std::uint32_t* ram = reinterpret_cast<volatile std::uint32_t*>(base + 0x80003000u);
    *ram = 0x12345678u;
    check(*ram == 0x12345678u && flat::fault_stats().handled_reads == before,
          "RAM through the view does not fault");
}

#endif

}  // namespace

int main() {
    try {
        flat::initialize();
        flat::initialize();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "flat memory initialization failed: %s\n", error.what());
        return 1;
    }
    check(flat::is_initialized(), "initialized");
    test_aliases();
#if defined(__aarch64__)
    test_faults();
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "%d flat memory check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("flat guest memory (POSIX) tests passed");
    return 0;
}
