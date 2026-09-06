// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the per-target alpha known-bits rules (GS/Renderers/HW/GSAlphaKnownBits.h).
//
// The pair exists because the alpha min/max range cannot carry a bit through a mask, and the whole
// value of it is that a masked write leaves the masked bits' knowledge alone. Every case below is
// one of the maintenance sites the renderer has: a masked write, an unmasked full-cover write, an
// unmasked partial-cover write, an upload, a valid-rect growth.
//
// Rides gs_vertex_tests -- the rules are header-only constexpr, so they need no extra linkage.

#include "GS/Renderers/HW/GSAlphaKnownBits.h"

#include <gtest/gtest.h>

using GSAlphaKnownBits::Known;

namespace
{
	// A source alpha the draw produces as a single value.
	Known WriteConst(Known prev, u8 written, u8 src, bool full_cover)
	{
		return GSAlphaKnownBits::AfterWrite(prev, written, src, src, full_cover);
	}
} // namespace

TEST(GSAlphaKnownBits, ConstantBitsOfASingleValueIsEveryBit)
{
	EXPECT_EQ(GSAlphaKnownBits::ConstantBits(0x80, 0x80), 0xFF);
	EXPECT_EQ(GSAlphaKnownBits::ConstantBits(0x00, 0x00), 0xFF);
}

TEST(GSAlphaKnownBits, ConstantBitsAreThoseAboveTheTopDifferingBit)
{
	// 0..127 never sets bit 7 and says nothing about the rest.
	EXPECT_EQ(GSAlphaKnownBits::ConstantBits(0x00, 0x7F), 0x80);
	// 128..255 is the same shape one binade up.
	EXPECT_EQ(GSAlphaKnownBits::ConstantBits(0x80, 0xFF), 0x80);
	// A range spanning 128 knows nothing at all.
	EXPECT_EQ(GSAlphaKnownBits::ConstantBits(0x00, 0xFF), 0x00);
}

TEST(GSAlphaKnownBits, ConstantBitsIsNotEndpointAgreement)
{
	// Both endpoints have bit 0 clear, but 0x01 lies between them. Comparing endpoints bit by bit
	// would call bit 0 constant and be wrong, which is the trap this whole helper exists to avoid.
	EXPECT_EQ(GSAlphaKnownBits::ConstantBits(0x00, 0x02) & 0x01, 0x00);
}

TEST(GSAlphaKnownBits, MaskedWritePreservesKnowledgeOfTheMaskedBits)
{
	// Xenosaga's shape: the target is known to hold bit 7 set, and the draw writes the low seven
	// bits with bit 7 masked off. Bit 7 must survive, whatever the source alpha is.
	const Known prev{0x80, 0x80};
	const Known after = GSAlphaKnownBits::AfterWrite(prev, 0x7F, 0x00, 0xFF, false);
	EXPECT_EQ(after.bits & 0x80, 0x80);
	EXPECT_EQ(after.value & 0x80, 0x80);
}

TEST(GSAlphaKnownBits, MaskedWritePreservesKnowledgeUnderFullCoverToo)
{
	const Known prev{0x80, 0x00};
	const Known after = GSAlphaKnownBits::AfterWrite(prev, 0x7F, 0x00, 0xFF, true);
	EXPECT_EQ(after.bits & 0x80, 0x80);
	EXPECT_EQ(after.value & 0x80, 0x00);
}

TEST(GSAlphaKnownBits, FullCoverWriteEstablishesTheSourcesConstantBits)
{
	// Nothing known before; a full-cover unmasked write of a single value makes the whole byte known.
	EXPECT_EQ(WriteConst(Known::Nothing(), 0xFF, 0x80, true), (Known{0xFF, 0x80}));
	// And it overwrites what was known, rather than intersecting with it.
	EXPECT_EQ(WriteConst(Known{0xFF, 0x00}, 0xFF, 0x80, true), (Known{0xFF, 0x80}));
}

TEST(GSAlphaKnownBits, PartialCoverWriteClearsBitsTheSourceDisagreesOn)
{
	// The pixels the draw missed still hold 0, the ones it hit hold 128, so bit 7 is now mixed
	// across the surface. The other seven bits are zero either way and stay known.
	EXPECT_EQ(WriteConst(Known{0xFF, 0x00}, 0xFF, 0x80, false), (Known{0x7F, 0x00}));
	// Nothing survives when the source disagrees on every bit.
	EXPECT_EQ(WriteConst(Known{0xFF, 0x00}, 0xFF, 0xFF, false), Known::Nothing());
}

TEST(GSAlphaKnownBits, PartialCoverWriteKeepsBitsTheSourceAgreesOn)
{
	// Writing the value the target already holds everywhere changes nothing, covered or not.
	EXPECT_EQ(WriteConst(Known{0xFF, 0x80}, 0xFF, 0x80, false), (Known{0xFF, 0x80}));
	// And it keeps only the bits that agree.
	EXPECT_EQ(WriteConst(Known{0xFF, 0x81}, 0xFF, 0x80, false), (Known{0xFE, 0x80}));
}

TEST(GSAlphaKnownBits, PartialCoverWriteOfANonConstantSourceKnowsNothing)
{
	EXPECT_EQ(GSAlphaKnownBits::AfterWrite(Known{0xFF, 0x00}, 0xFF, 0x00, 0xFF, false), Known::Nothing());
}

TEST(GSAlphaKnownBits, FullUploadReplacesWhatWasKnown)
{
	EXPECT_EQ(GSAlphaKnownBits::AfterUpload(Known{0xFF, 0x00}, 0x80, 0x80, true), (Known{0xFF, 0x80}));
	// An upload of a wide range clears the knowledge outright.
	EXPECT_EQ(GSAlphaKnownBits::AfterUpload(Known{0xFF, 0x00}, 0x00, 0xFF, true), Known::Nothing());
}

TEST(GSAlphaKnownBits, PartialUploadIntersects)
{
	// The upload landed somewhere in the valid rect; only bits both descriptions agree on survive.
	EXPECT_EQ(GSAlphaKnownBits::AfterUpload(Known{0xFF, 0x80}, 0x80, 0x80, false), (Known{0xFF, 0x80}));
	EXPECT_EQ(GSAlphaKnownBits::AfterUpload(Known{0xFF, 0x00}, 0x80, 0x80, false), (Known{0x7F, 0x00}));
	EXPECT_EQ(GSAlphaKnownBits::AfterUpload(Known{0xFF, 0x00}, 0xFF, 0xFF, false), Known::Nothing());
	EXPECT_EQ(GSAlphaKnownBits::AfterUpload(Known{0xFF, 0x00}, 0x00, 0x7F, false), (Known{0x80, 0x00}));
}

TEST(GSAlphaKnownBits, GrowthKeepsOnlyBitsKnownToBeZero)
{
	// The pixels the valid rect gained hold the zero the allocation was cleared to.
	EXPECT_EQ(GSAlphaKnownBits::AfterGrow(Known{0xFF, 0x00}), (Known{0xFF, 0x00}));
	EXPECT_EQ(GSAlphaKnownBits::AfterGrow(Known{0xFF, 0x80}), (Known{0x7F, 0x00}));
	EXPECT_EQ(GSAlphaKnownBits::AfterGrow(Known::Nothing()), Known::Nothing());
}

TEST(GSAlphaKnownBits, MaskIsIdentityOnlyWhenBothSidesAgreeOnEveryMaskedBit)
{
	const Known target{0x80, 0x80};
	// Source alpha 128..128: bit 7 set, same as the target. Masking it is the identity.
	EXPECT_TRUE(GSAlphaKnownBits::MaskIsIdentity(target, 0x80, 0x80, 0x80));
	// Source alpha 0: the mask is doing real work.
	EXPECT_FALSE(GSAlphaKnownBits::MaskIsIdentity(target, 0x80, 0x00, 0x00));
	// Source alpha not constant on the masked bit.
	EXPECT_FALSE(GSAlphaKnownBits::MaskIsIdentity(target, 0x80, 0x00, 0xFF));
	// Target does not know the masked bit.
	EXPECT_FALSE(GSAlphaKnownBits::MaskIsIdentity(Known::Nothing(), 0x80, 0x80, 0x80));
	// Nothing is masked, so there is nothing to drop.
	EXPECT_FALSE(GSAlphaKnownBits::MaskIsIdentity(target, 0x00, 0x80, 0x80));
}

TEST(GSAlphaKnownBits, MaskIsIdentityForKatamarisComplementMask)
{
	// Katamari masks the low seven bits and writes the top one. Those seven have been zero since
	// the target was created and nothing writes them, so a source alpha of exactly 128 is a drop.
	const Known target{0x7F, 0x00};
	EXPECT_TRUE(GSAlphaKnownBits::MaskIsIdentity(target, 0x7F, 0x80, 0x80));
	EXPECT_FALSE(GSAlphaKnownBits::MaskIsIdentity(target, 0x7F, 0x81, 0x81));
	// 0x80..0xFF is constant on bit 7 only, which is not one of the masked bits.
	EXPECT_FALSE(GSAlphaKnownBits::MaskIsIdentity(target, 0x7F, 0x80, 0xFF));
}

TEST(GSAlphaKnownBits, RangeAdmitsRejectsAPairTheRangeCannotContain)
{
	// Range says every pixel is under 128; a pair claiming bit 7 set contradicts it.
	EXPECT_FALSE(GSAlphaKnownBits::RangeAdmits(0, 127, Known{0x80, 0x80}));
	EXPECT_TRUE(GSAlphaKnownBits::RangeAdmits(0, 127, Known{0x80, 0x00}));
	// The wide range xenosaga's target sits at admits either polarity.
	EXPECT_TRUE(GSAlphaKnownBits::RangeAdmits(0, 255, Known{0x80, 0x80}));
	// An exact range has to match the pair bit for bit.
	EXPECT_TRUE(GSAlphaKnownBits::RangeAdmits(128, 128, Known{0x80, 0x80}));
	EXPECT_FALSE(GSAlphaKnownBits::RangeAdmits(128, 128, Known{0x80, 0x00}));
	// A value carrying bits outside its own known set is malformed.
	EXPECT_FALSE(GSAlphaKnownBits::RangeAdmits(0, 255, Known{0x80, 0x81}));
}

TEST(GSAlphaKnownBits, EveryWriteResultIsWellFormed)
{
	// The value never carries a bit the pair does not claim to know, over every shape of input.
	for (unsigned prev_bits = 0; prev_bits < 256; prev_bits += 17)
	{
		for (unsigned prev_value = 0; prev_value < 256; prev_value += 23)
		{
			const Known prev{static_cast<u8>(prev_bits), static_cast<u8>(prev_bits & prev_value)};
			for (unsigned written = 0; written < 256; written += 29)
			{
				for (unsigned lo = 0; lo < 256; lo += 31)
				{
					for (unsigned hi = lo; hi < 256; hi += 37)
					{
						for (bool full_cover : {false, true})
						{
							const Known after = GSAlphaKnownBits::AfterWrite(prev, static_cast<u8>(written),
								static_cast<u8>(lo), static_cast<u8>(hi), full_cover);
							EXPECT_EQ(after.value & ~after.bits, 0);
							// Knowledge of a bit the mask held back is never invented, only kept.
							const u8 held = static_cast<u8>(~written);
							EXPECT_EQ(after.bits & held & ~prev.bits, 0);
							EXPECT_EQ((after.value ^ prev.value) & after.bits & held & prev.bits, 0);
						}
					}
				}
			}
		}
	}
}

TEST(GSAlphaKnownBits, FBABoundsTheRangeItStraddles)
{
	using GSAlphaKnownBits::AfterFBA;

	// Below 128 the OR is a shift, so both endpoints move together.
	EXPECT_EQ(AfterFBA(0x00, 0x40).lo, 0x80);
	EXPECT_EQ(AfterFBA(0x00, 0x40).hi, 0xC0);

	// At or above 128 the OR does nothing.
	EXPECT_EQ(AfterFBA(0x90, 0xC0).lo, 0x90);
	EXPECT_EQ(AfterFBA(0x90, 0xC0).hi, 0xC0);

	// Straddling 128 is where ORing the endpoints stops being a bound: 0x00..0x80 maps to
	// 0x80..0xFF, and 0x64..0xC8 maps to an inverted 0xE4..0xC8.
	EXPECT_EQ(AfterFBA(0x00, 0x80).lo, 0x80);
	EXPECT_EQ(AfterFBA(0x00, 0x80).hi, 0xFF);
	EXPECT_EQ(AfterFBA(0x64, 0xC8).lo, 0x80);
	EXPECT_EQ(AfterFBA(0x64, 0xC8).hi, 0xFF);

	// The point of the bound: every value the draw can write is inside it, so no bit outside the
	// ones that really are constant gets claimed.
	for (int lo = 0; lo <= 0xFF; lo++)
	{
		for (int hi = lo; hi <= 0xFF; hi++)
		{
			const GSAlphaKnownBits::Range r = AfterFBA(static_cast<u8>(lo), static_cast<u8>(hi));
			ASSERT_LE(r.lo, r.hi) << lo << ".." << hi;
			const u8 constant = GSAlphaKnownBits::ConstantBits(r.lo, r.hi);
			for (int a = lo; a <= hi; a++)
			{
				const u8 written = static_cast<u8>(a) | 0x80;
				ASSERT_GE(written, r.lo) << lo << ".." << hi << " a " << a;
				ASSERT_LE(written, r.hi) << lo << ".." << hi << " a " << a;
				ASSERT_EQ(written & constant, r.lo & constant) << lo << ".." << hi << " a " << a;
			}
		}
	}
}
