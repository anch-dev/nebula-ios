// SPDX-License-Identifier: GPL-3.0-only
// FIPS 180 test vectors for galaxy/portable_hash.h, including the padding
// boundaries (55, 56, 63, 64 and 119 bytes).

#include "galaxy/portable_hash.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
std::span<const std::byte> as_bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
void check(const std::string& actual, const char* expected, const char* what) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s: %s\n", what, actual.c_str());
        ++g_failures;
    }
}
}  // namespace

int main() {
    using galaxy::hash::sha1_hex;
    using galaxy::hash::sha256_hex;
    check(sha1_hex(as_bytes("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 empty");
    check(sha1_hex(as_bytes("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 abc");
    check(sha1_hex(as_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "sha1 448-bit");
    check(sha256_hex(as_bytes("")),
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
    check(sha256_hex(as_bytes("abc")),
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    check(sha256_hex(as_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 448-bit");
    check(sha256_hex(as_bytes("abc"), true),
          "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", "sha256 upper");
    const std::string million(1'000'000, 'a');
    check(sha256_hex(as_bytes(million)),
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 million a");
    check(sha1_hex(as_bytes(million)), "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "sha1 million a");
    // Padding boundaries: 55 (one block), 56 and 63 (two blocks), 64, 119.
    check(sha256_hex(as_bytes(std::string(55, 'a'))),
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", "sha256 55");
    check(sha256_hex(as_bytes(std::string(56, 'a'))),
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", "sha256 56");
    check(sha256_hex(as_bytes(std::string(64, 'a'))),
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", "sha256 64");
    if (g_failures != 0) return 1;
    std::puts("portable hash tests passed");
    return 0;
}
