// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The two roads' MODULATE have to be the same function, including where nothing
// we render reaches.
//
// `GSVector4i::modulate16<shift>(f)` is `sll16<shift + 1>().mul16hs(f)`: the
// shift happens in SIXTEEN-BIT lanes and wraps, and only then does a widening
// multiply take the product's high half. The ARM64 generator used to emit
// `SHL by shift` then `SQDMULH`, which folds the last doubling into the 32-bit
// product instead -- the same function on every lane the shipped selectors
// reach, and a different one the moment a lane's `a << (shift + 1)` overflows a
// signed sixteen-bit value.
//
// ⚠️ Nothing in the archive separates the two, and that is why this is a test and
// not a capture. MODULATE multiplies a texel in 0..255 by a stored colour byte,
// and the blend's first operand is a difference of bytes, so no draw we own
// drives `a` within a factor of thirty of the magnitude where they part.
// gs-shade's MODULATE readings are the console's word on the texture function
// and they sit entirely inside the agreeing domain, so they cannot pick a side
// either. The reference's shape is the one x86 ships and the one the C++
// fallback runs on this host, so the generator was moved to it and all three
// roads now agree rather than two agreeing and the third being
// unreachable-but-different.
//
// The suite pins the contract as a scalar model and then checks BOTH roads
// against it, so a later change to either has to come back here and say why.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/GSVector.h"

#include <gtest/gtest.h>

#include <arm_neon.h>

namespace
{
/// The contract, written once: shift the lane left by `shift + 1` in sixteen
/// bits (wrapping, and the result read back as signed), multiply by `f` at full
/// width, and keep the product's high half.
s16 ModulateModel(s16 a, s16 f, int shift)
{
	const s16 w = static_cast<s16>(static_cast<u16>(a) << (shift + 1));
	return static_cast<s16>((static_cast<s32>(w) * static_cast<s32>(f)) >> 16);
}

/// What the ARM64 generator emits today: SHL by shift + 1, an exact halving
/// (the value is even, because it was just shifted left by at least one), then
/// SQDMULH, whose own doubling puts it back.
s16 GeneratorToday(s16 a, s16 f, int shift)
{
	const int16x8_t va = vdupq_n_s16(a);
	const int16x8_t vf = vdupq_n_s16(f);
	int16x8_t d = (shift == 0) ? vshlq_n_s16(va, 1) : vshlq_n_s16(va, 2);
	d = vshrq_n_s16(d, 1);
	d = vqdmulhq_s16(d, vf);
	return vgetq_lane_s16(d, 0);
}

/// What it emitted before: SHL by shift, then SQDMULH. Kept so the pin names the
/// shape it is guarding against rather than only the one it wants.
s16 GeneratorBefore(s16 a, s16 f, int shift)
{
	const int16x8_t va = vdupq_n_s16(a);
	const int16x8_t vf = vdupq_n_s16(f);
	const int16x8_t d = (shift == 0) ? va : vshlq_n_s16(va, 1);
	return vgetq_lane_s16(vqdmulhq_s16(d, vf), 0);
}

s16 Reference(s16 a, s16 f, int shift)
{
	const GSVector4i va(a | (a << 16), 0, 0, 0);
	const GSVector4i vf(f | (f << 16), 0, 0, 0);
	const GSVector4i r = (shift == 0) ? va.modulate16<0>(vf) : va.modulate16<1>(vf);
	return static_cast<s16>(r.I32[0] & 0xFFFF);
}
} // namespace

// The shipped vector helper is the contract, over the whole signed range.
TEST(SwModulate16Test, TheReferenceIsTheModel)
{
	for (int shift = 0; shift <= 1; shift++)
	{
		for (int a = -32768; a <= 32767; a += 37)
		{
			for (int f = -32768; f <= 32767; f += 1021)
			{
				ASSERT_EQ(Reference(static_cast<s16>(a), static_cast<s16>(f), shift),
					ModulateModel(static_cast<s16>(a), static_cast<s16>(f), shift))
					<< "a=" << a << " f=" << f << " shift=" << shift;
			}
		}
	}
}

// And so is what the generator emits.
TEST(SwModulate16Test, TheGeneratorIsTheModel)
{
	for (int shift = 0; shift <= 1; shift++)
	{
		for (int a = -32768; a <= 32767; a += 37)
		{
			for (int f = -32768; f <= 32767; f += 1021)
			{
				ASSERT_EQ(GeneratorToday(static_cast<s16>(a), static_cast<s16>(f), shift),
					ModulateModel(static_cast<s16>(a), static_cast<s16>(f), shift))
					<< "a=" << a << " f=" << f << " shift=" << shift;
			}
		}
	}
}

// The shape it replaced does NOT satisfy the contract, and this names where: the
// two agree until `a << (shift + 1)` leaves a signed sixteen-bit lane, and part
// on every multiplier above it.
TEST(SwModulate16Test, TheOldGeneratorShapePartsAtEightThousand)
{
	// At shift 1 the threshold is exactly where `a << 2` leaves a signed sixteen-bit
	// lane: |a| < 8192 agrees, and above it the two are different functions.
	int parted_below = 0, parted_above = 0;

	for (int a = -32768; a <= 32767; a++)
	{
		const bool same = GeneratorBefore(static_cast<s16>(a), 16384, 1)
						  == ModulateModel(static_cast<s16>(a), 16384, 1);

		if (same)
			continue;

		if (a >= -8192 && a < 8192)
			parted_below++;
		else
			parted_above++;
	}

	EXPECT_EQ(parted_below, 0) << "the two shapes agree below the wrap, which is why "
								  "no capture could have caught this";
	EXPECT_GT(parted_above, 0) << "and part above it, or this pin is vacuous";

	// The reachable domain, stated as itself: a texel, or a difference of bytes.
	for (int a = -255; a <= 255; a++)
	{
		ASSERT_EQ(GeneratorBefore(static_cast<s16>(a), 16384, 1),
			ModulateModel(static_cast<s16>(a), 16384, 1)) << "a=" << a;
	}
}

#endif // ARCH_ARM64
