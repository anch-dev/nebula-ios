// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// SHA-1 and SHA-256 (FIPS 180-4) for hosts without Windows CNG. Used for
// content identities (disc image, boot image, generated modules), not for
// security.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace galaxy::hash {

namespace detail {

constexpr std::uint32_t rotl(std::uint32_t value, unsigned count) noexcept {
    return (value << count) | (value >> (32u - count));
}
constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
    return (value >> count) | (value << (32u - count));
}

// Feeds `bytes` plus the standard padding to `block(const unsigned char*)`.
template <typename Block>
void merkle_damgard(std::span<const std::byte> bytes, Block&& block) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t remaining = bytes.size();
    while (remaining >= 64u) {
        block(data);
        data += 64u;
        remaining -= 64u;
    }
    unsigned char tail[128]{};
    for (std::size_t i = 0; i < remaining; ++i) tail[i] = data[i];
    tail[remaining] = 0x80u;
    const std::size_t tail_length = remaining + 1u + 8u <= 64u ? 64u : 128u;
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8u;
    for (unsigned i = 0; i < 8u; ++i) {
        tail[tail_length - 1u - i] = static_cast<unsigned char>(bits >> (8u * i));
    }
    block(tail);
    if (tail_length == 128u) block(tail + 64);
}

inline std::uint32_t load_be32(const unsigned char* p) noexcept {
    return (std::uint32_t{p[0]} << 24u) | (std::uint32_t{p[1]} << 16u) |
           (std::uint32_t{p[2]} << 8u) | std::uint32_t{p[3]};
}

template <std::size_t Words>
std::string hex(const std::array<std::uint32_t, Words>& state, bool upper) {
    const char* const digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    out.reserve(Words * 8u);
    for (const std::uint32_t word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(digits[(word >> static_cast<unsigned>(shift)) & 0xFu]);
        }
    }
    return out;
}

}  // namespace detail

inline std::array<std::uint32_t, 5> sha1(std::span<const std::byte> bytes) {
    std::array<std::uint32_t, 5> h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    detail::merkle_damgard(bytes, [&](const unsigned char* block) {
        std::uint32_t w[80];
        for (unsigned i = 0; i < 16u; ++i) w[i] = detail::load_be32(block + 4u * i);
        for (unsigned i = 16u; i < 80u; ++i) {
            w[i] = detail::rotl(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (unsigned i = 0; i < 80u; ++i) {
            std::uint32_t f, k;
            if (i < 20u) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40u) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60u) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const std::uint32_t t = detail::rotl(a, 5u) + f + e + k + w[i];
            e = d; d = c; c = detail::rotl(b, 30u); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    });
    return h;
}

inline std::array<std::uint32_t, 8> sha256(std::span<const std::byte> bytes) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    std::array<std::uint32_t, 8> h{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    detail::merkle_damgard(bytes, [&](const unsigned char* block) {
        std::uint32_t w[64];
        for (unsigned i = 0; i < 16u; ++i) w[i] = detail::load_be32(block + 4u * i);
        for (unsigned i = 16u; i < 64u; ++i) {
            const std::uint32_t s0 =
                detail::rotr(w[i - 15u], 7u) ^ detail::rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
            const std::uint32_t s1 =
                detail::rotr(w[i - 2u], 17u) ^ detail::rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
            w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
                      hh = h[7];
        for (unsigned i = 0; i < 64u; ++i) {
            const std::uint32_t s1 = detail::rotr(e, 6u) ^ detail::rotr(e, 11u) ^ detail::rotr(e, 25u);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = detail::rotr(a, 2u) ^ detail::rotr(a, 13u) ^ detail::rotr(a, 22u);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    });
    return h;
}

inline std::string sha1_hex(std::span<const std::byte> bytes, bool upper = false) {
    return detail::hex(sha1(bytes), upper);
}

inline std::string sha256_hex(std::span<const std::byte> bytes, bool upper = false) {
    return detail::hex(sha256(bytes), upper);
}

}  // namespace galaxy::hash
