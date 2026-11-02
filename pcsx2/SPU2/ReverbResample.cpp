// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSVector.h"
#include "SPU2/defs.h"

#include <algorithm>

MULTI_ISA_UNSHARED_START

static constexpr u32 NUM_TAPS = 39;
// 39 tap filter, the 0's could be optimized out
static constexpr std::array<s16, 48> filter_down_coefs alignas(32) = {
	-1,
	0,
	2,
	0,
	-10,
	0,
	35,
	0,
	-103,
	0,
	266,
	0,
	-616,
	0,
	1332,
	0,
	-2960,
	0,
	10246,
	16384,
	10246,
	0,
	-2960,
	0,
	1332,
	0,
	-616,
	0,
	266,
	0,
	-103,
	0,
	35,
	0,
	-10,
	0,
	2,
	0,
	-1,
};

static constexpr std::array<s16, 48> make_up_coefs()
{
	std::array<s16, 48> ret = {};

	for (u32 i = 0; i < NUM_TAPS; i++)
	{
		ret[i] = static_cast<s16>(std::clamp<s32>(filter_down_coefs[i] * 2, INT16_MIN, INT16_MAX));
	}

	return ret;
}

static constexpr std::array<s16, 48> filter_up_coefs alignas(32) = make_up_coefs();

s32 __forceinline ReverbDownsample_reference(V_Core& core, bool right)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
	s32 out = 0;

	for (u32 i = 0; i < NUM_TAPS; i++)
	{
		out += core.RevbDownBuf[right][index + i] * filter_down_coefs[i];
	}

	out >>= 15;

	return clamp_mix(out);
}

#if _M_SSE >= 0x501
s32 __forceinline ReverbDownsample_avx(V_Core& core, bool right)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector8i::load<true>(&filter_down_coefs[0]);
	auto s = GSVector8i::load<false>(&core.RevbDownBuf[right][index]);
	auto acc = s.mul16hrs(c);

	c = GSVector8i::load<true>(&filter_down_coefs[16]);
	s = GSVector8i::load<false>(&core.RevbDownBuf[right][index + 16]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector8i::load<true>(&filter_down_coefs[32]);
	s = GSVector8i::load<false>(&core.RevbDownBuf[right][index + 32]);
	acc = acc.adds16(s.mul16hrs(c));

	acc = acc.adds16(acc.ba());

	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);

	return acc.I16[0];
}
#endif

s32 __forceinline ReverbDownsample_sse(V_Core& core, bool right)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector4i::load<true>(&filter_down_coefs[0]);
	auto s = GSVector4i::load<false>(&core.RevbDownBuf[right][index]);
	auto acc = s.mul16hrs(c);

	c = GSVector4i::load<true>(&filter_down_coefs[8]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 8]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[16]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 16]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[24]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 24]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[32]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 32]);
	acc = acc.adds16(s.mul16hrs(c));

	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);

	return acc.I16[0];
}

#if defined(__aarch64__)
#include <arm_neon.h>

// On AArch64 the two resamplers are written in NEON directly. They do what the GSVector4i versions do, tap group by tap
// group and in the same order of saturating adds, with one change of instruction: mul16hrs is a widening multiply and a
// rounding narrowing shift, which vqrdmulhq_s16 does in one instruction. The two differ for exactly one pair of
// operands, -32768 * -32768 (+32768 wraps in the first and saturates to 32767 in the second), and no coefficient of
// either filter is -32768, so they agree for every input.
static_assert(std::ranges::min(filter_down_coefs) > INT16_MIN && std::ranges::min(filter_up_coefs) > INT16_MIN);

// hadds16 applied to a vector with itself, as the SSE-style code does it three times to fold eight lanes into lane 0.
static __forceinline int16x8_t FoldPairs(int16x8_t a)
{
	return vqaddq_s16(vuzp1q_s16(a, a), vuzp2q_s16(a, a));
}

s32 ReverbDownsample(V_Core& core, bool right)
{
	const s16* const src = &core.RevbDownBuf[right][(core.RevbSampleBufPos - NUM_TAPS) & 63];

	int16x8_t acc = vqrdmulhq_s16(vld1q_s16(src), vld1q_s16(&filter_down_coefs[0]));
	acc = vqaddq_s16(acc, vqrdmulhq_s16(vld1q_s16(src + 8), vld1q_s16(&filter_down_coefs[8])));
	acc = vqaddq_s16(acc, vqrdmulhq_s16(vld1q_s16(src + 16), vld1q_s16(&filter_down_coefs[16])));
	acc = vqaddq_s16(acc, vqrdmulhq_s16(vld1q_s16(src + 24), vld1q_s16(&filter_down_coefs[24])));
	acc = vqaddq_s16(acc, vqrdmulhq_s16(vld1q_s16(src + 32), vld1q_s16(&filter_down_coefs[32])));

	acc = FoldPairs(FoldPairs(FoldPairs(acc)));
	return vgetq_lane_s16(acc, 0);
}
#else
s32 ReverbDownsample(V_Core& core, bool right)
{
#if _M_SSE >= 0x501
	return ReverbDownsample_avx(core, right);
#else
	return ReverbDownsample_sse(core, right);
#endif
}
#endif

StereoOut32 __forceinline ReverbUpsample_reference(V_Core& core)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
	s32 l = 0, r = 0;

	for (u32 i = 0; i < NUM_TAPS; i++)
	{
		l += core.RevbUpBuf[0][index + i] * filter_up_coefs[i];
		r += core.RevbUpBuf[1][index + i] * filter_up_coefs[i];
	}

	l >>= 15;
	r >>= 15;

	return {clamp_mix(l), clamp_mix(r)};
}

#if _M_SSE >= 0x501
StereoOut32 __forceinline ReverbUpsample_avx(V_Core& core)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector8i::load<true>(&filter_up_coefs[0]);
	auto l = GSVector8i::load<false>(&core.RevbUpBuf[0][index]);
	auto r = GSVector8i::load<false>(&core.RevbUpBuf[1][index]);

	auto lacc = l.mul16hrs(c);
	auto racc = r.mul16hrs(c);

	c = GSVector8i::load<true>(&filter_up_coefs[16]);
	l = GSVector8i::load<false>(&core.RevbUpBuf[0][index + 16]);
	r = GSVector8i::load<false>(&core.RevbUpBuf[1][index + 16]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector8i::load<true>(&filter_up_coefs[32]);
	l = GSVector8i::load<false>(&core.RevbUpBuf[0][index + 32]);
	r = GSVector8i::load<false>(&core.RevbUpBuf[1][index + 32]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	lacc = lacc.adds16(lacc.ba());
	racc = racc.adds16(racc.ba());

	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);

	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);

	return {lacc.I16[0], racc.I16[0]};
}
#endif

StereoOut32 __forceinline ReverbUpsample_sse(V_Core& core)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector4i::load<true>(&filter_up_coefs[0]);
	auto l = GSVector4i::load<false>(&core.RevbUpBuf[0][index]);
	auto r = GSVector4i::load<false>(&core.RevbUpBuf[1][index]);

	auto lacc = l.mul16hrs(c);
	auto racc = r.mul16hrs(c);

	c = GSVector4i::load<true>(&filter_up_coefs[8]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 8]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 8]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[16]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 16]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 16]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[24]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 24]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 24]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[32]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 32]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 32]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);

	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);

	return {lacc.I16[0], racc.I16[0]};
}

#if defined(__aarch64__)
StereoOut32 ReverbUpsample(V_Core& core)
{
	const u32 index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
	const s16* const l = &core.RevbUpBuf[0][index];
	const s16* const r = &core.RevbUpBuf[1][index];

	int16x8_t c = vld1q_s16(&filter_up_coefs[0]);
	int16x8_t lacc = vqrdmulhq_s16(vld1q_s16(l), c);
	int16x8_t racc = vqrdmulhq_s16(vld1q_s16(r), c);
	for (u32 k = 8; k <= 32; k += 8)
	{
		c = vld1q_s16(&filter_up_coefs[k]);
		lacc = vqaddq_s16(lacc, vqrdmulhq_s16(vld1q_s16(l + k), c));
		racc = vqaddq_s16(racc, vqrdmulhq_s16(vld1q_s16(r + k), c));
	}

	lacc = FoldPairs(FoldPairs(FoldPairs(lacc)));
	racc = FoldPairs(FoldPairs(FoldPairs(racc)));
	return {vgetq_lane_s16(lacc, 0), vgetq_lane_s16(racc, 0)};
}
#else
StereoOut32 ReverbUpsample(V_Core& core)
{
#if _M_SSE >= 0x501
	return ReverbUpsample_avx(core);
#else
	return ReverbUpsample_sse(core);
#endif
}
#endif

MULTI_ISA_UNSHARED_END
