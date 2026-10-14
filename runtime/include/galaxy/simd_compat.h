// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The SSE2/SSSE3 intrinsics the GX code uses (texture decode, FIFO and index
// kernels). x86 hosts get them from the compiler; AArch64 hosts from
// sse2neon (third_party/sse2neon, MIT), which implements each one with NEON.

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "../../../third_party/sse2neon/sse2neon.h"
#else
#error "galaxy/simd_compat.h: unsupported host architecture"
#endif
