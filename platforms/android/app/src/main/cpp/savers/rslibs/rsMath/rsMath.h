/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of rsMath.
 *
 * rsMath is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * rsMath is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef RSMATH_H
#define RSMATH_H

#include <math.h>
#include <random>
#include <stdlib.h>

#ifdef __SSE__
#include <xmmintrin.h>
#endif

#include "rsMatrix.h"
#include "rsQuat.h"
#include "rsTrigonometry.h"
#include "rsVec.h"
#include "rsVec4.h"

#define RS_EPSILON 0.000001f
#define RS_PIo2 1.57079632679f
#define RS_PI 3.14159265359f
#define RS_PIx2 6.28318530718f
#define RS_DEG2RAD 0.0174532925f
#define RS_RAD2DEG 57.2957795131f

 // Useful random number functions.
 // These seed themselves, so there is nothing to initialize; a caller's srand()
 // no longer has any effect on them.
 //
 // The engine is thread_local rather than a plain static. microcosm runs two
 // worker threads, and a shared std::mt19937 would be a data race, where the
 // rand() this replaced was already per-thread on MSVC and locked in glibc.
inline std::mt19937&
rsRandGen()
{
	static thread_local std::mt19937 gen(std::random_device{}());
	return gen;
}

inline int
rsRandi(int x)
{
	// A caller can reach x <= 0 through an unclamped setting - lattice computes
	// rsRandi(11 - dPathrand) straight from a registry value - where rand() % x
	// was a division by zero and uniform_int_distribution(0, x - 1) is equally
	// undefined. Zero is the only answer that could ever be in range, so return
	// it rather than let a bad setting take the saver down.
	if (x <= 1)
		return 0;
	// [0, x), as rand() % x was, but without its bias for ranges that do not
	// divide RAND_MAX evenly.
	return std::uniform_int_distribution<int>(0, x - 1)(rsRandGen());
}

inline float
rsRandf(float x)
{
	// Scaling a canonical [0, 1) value keeps the old contract for every x.
	// uniform_real_distribution(0, x) would be undefined for a negative x, and
	// callers do pass one: lattice computes rsRandf(150 - dSpeed) from an
	// unclamped registry value.
	return x * std::uniform_real_distribution<float>(0.0f, 1.0f)(rsRandGen());
}

inline float
rsSqrtf(const float& x)
{
#ifdef __SSE__
	return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
#else
	//return powf(x, 0.5f);
	return sqrtf(x);
#endif
}

inline float
rsInvSqrtf(const float& x)
{
#ifdef __SSE__
	return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
#else
	//return 1.0f / powf(x, 0.5f);
	return 1.0f / sqrtf(x);
#endif
}

/*typedef union {
	float f;
	int i;
} float_or_int;

inline float rsInvSqrtf(const float& x){
	float_or_int tmp;
	tmp.f = x;
	tmp.i = 0x5f3759df - (tmp.i >> 1);
	return tmp.f * (1.5f - 0.5f * x * tmp.f * tmp.f);
}*/

#endif
