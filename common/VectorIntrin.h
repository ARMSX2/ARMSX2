// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Includes appropriate intrinsic header based on platform.

#pragma once

#include "common/Pcsx2Defs.h"

#if defined(ARCH_X86)

#ifdef _MSC_VER
#include <intrin.h>
#endif

#if defined(__AVX2__)
#define _M_SSE 0x501
#elif defined(__AVX__)
#define _M_SSE 0x500
#elif defined(__SSE4_1__)
#define _M_SSE 0x401
#else
#error PCSX2 requires compiling for at least SSE 4.1
#endif

// Starting with AVX, processors have fast unaligned loads
// Reduce code duplication by not compiling multiple versions
#if _M_SSE >= 0x500
#define FAST_UNALIGNED 1
#else
#define FAST_UNALIGNED 0
#endif

#include <xmmintrin.h>
#include <emmintrin.h>
#include <tmmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>

#elif defined(ARCH_ARM64)

// AArch64 has no aligned/unaligned load distinction to begin with: LDR Q and LD1
// take any address, and GSVector4i::load<aligned> ignores its own parameter and
// emits the same instruction either way. Leaving this undefined made the callers
// pay for a distinction that does not exist — a runtime address-and-pitch test
// per texture upload, dispatching into three template instantiations that
// compile to identical code, and for 32/16-bit columns a worse load strategy
// (eight combining 64-bit loads instead of four 128-bit loads and a swizzle).
#define FAST_UNALIGNED 1

#include <arm_neon.h>
#endif

#ifdef __APPLE__
#include <stdlib.h> // alloca
#else
#include <malloc.h> // alloca
#endif
