#pragma once
// <immintrin.h> for the renderer sources: the compiler's on x86, sse2neon's
// SSE emulation on AArch64 (galaxy/simd_compat.h).
#if defined(__x86_64__) || defined(__i386__)
#include_next <immintrin.h>
#else
#include "galaxy/simd_compat.h"
#endif
