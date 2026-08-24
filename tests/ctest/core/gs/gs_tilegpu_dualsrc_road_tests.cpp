// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu AS-FACTOR ROAD: how the GS blend coefficient C = As reaches the blend unit on a device
// that has no dual-source blending, and -- the part that has to be exact -- that each road computes
// the same numbers the second colour output computed.
//
// Why there are roads at all. The GS reads the fragment's alpha byte in a 0x80 = 1.0 convention, so
// its As factor is A/128 and reaches 1.99 where the alpha the draw STORES reaches 1.0. Those are two
// different numbers, and the fragment stage has only one alpha channel to put a number in -- which
// is why the factor has ridden as a second colour output at index 1 (SRC1), and why removing that
// output is not a matter of deleting a line. Vulkan forbids both the index-1 declaration and the
// SRC1_* factors when dualSrcBlend is false, and the Mali blob on the RG477V reports it false with a
// hardcoded zero dual-source attachment count, so this is a real exclusion rather than a
// theoretical one.
//
// What is pinned here:
//
//  - The three-state road selection, the shape the scissor road and the pass cap already have,
//    including the asymmetry: a force value can turn the second output OFF anywhere and cannot turn
//    it ON where the device has no feature to turn on.
//
//  - Which of a blend row's two factors is As, read off GSDevice::m_blendMap itself rather than off
//    the ALPHA register's selectors -- the map approximates several equations and only the row says
//    which approximation still reaches for the second source. Including the structural fact the
//    whole design rests on: every row that names a dual-source factor is a C = As row.
//
//  - The road table: which draw takes which road, and the two properties that make it safe --
//    a row whose DESTINATION factor is As never takes the fold (the fragment stage cannot reach Cd),
//    and every road other than the second output leaves no SRC1 factor in the pipeline.
//
//  - The ARITHMETIC of the carrier's restore road, over every alpha byte it can be chosen for. The
//    carrier is As * 255/128 and the alpha blend equation multiplies it by 128/255 to give the
//    stored byte back; that round trip is two float roundings, and a wrong constant or a wrong
//    clamp would be a whole channel out.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

#include <cmath>

namespace
{
	using Road = GSDevice::GSTileGpuDualSrcRoad;

	/// The fragment stage's As factor, transcribed from tilegpu.glsl: the fragment alpha read in the
	/// GS's 0x80 = 1.0 convention, clamped where a byte above 0x80 would take it past one.
	float AsFactor(float alpha)
	{
		return std::min(alpha * (255.0f / 128.0f), 1.0f);
	}

	/// What a UNORM8 colour attachment stores for a float the fragment stage produced.
	int Unorm8(float v)
	{
		const float c = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
		return static_cast<int>(std::lround(c * 255.0f));
	}
} // namespace

// -- which road the device takes -----------------------------------------------------------------

TEST(GSTileGpuDualSrcRoad, ZeroAsksTheDevice)
{
	EXPECT_TRUE(GSDevice::gsTileGpuDualSourceRoad(0, true));
	EXPECT_FALSE(GSDevice::gsTileGpuDualSourceRoad(0, false));
}

TEST(GSTileGpuDualSrcRoad, PositiveForcesTheCarrierAnywhere)
{
	EXPECT_FALSE(GSDevice::gsTileGpuDualSourceRoad(1, true));
	EXPECT_FALSE(GSDevice::gsTileGpuDualSourceRoad(4096, true));
	EXPECT_FALSE(GSDevice::gsTileGpuDualSourceRoad(1, false));
}

TEST(GSTileGpuDualSrcRoad, NegativeCannotConjureTheFeature)
{
	// The asymmetry, and it is the one that matters: asking for the second output on a device that
	// does not have one has to leave the carrier roads standing, not produce a pipeline Vulkan
	// refuses to create.
	EXPECT_TRUE(GSDevice::gsTileGpuDualSourceRoad(-1, true));
	EXPECT_FALSE(GSDevice::gsTileGpuDualSourceRoad(-1, false));
}

// -- which factors a blend row names -------------------------------------------------------------

TEST(GSTileGpuDualSrcTerms, TheRowsThatNameAsAreTheOnesTheMapSays)
{
	// Standard alpha blending, Cs*As + Cd*(1 - As): both factors.
	const u32 both = GSDevice::gsTileGpuDualSrcTerms(0 * 27 + 1 * 9 + 0 * 3 + 1); // 0101
	EXPECT_EQ(both & (GSDevice::kGSTileGpuDualSrcSource | GSDevice::kGSTileGpuDualSrcDest),
		GSDevice::kGSTileGpuDualSrcSource | GSDevice::kGSTileGpuDualSrcDest);
	EXPECT_EQ(both & GSDevice::kGSTileGpuDualSrcSourceInv, 0u);

	// Cs*As + Cd, the accumulation shape: the SOURCE factor only.
	const u32 src = GSDevice::gsTileGpuDualSrcTerms(0 * 27 + 2 * 9 + 0 * 3 + 1); // 0201
	EXPECT_EQ(src, GSDevice::kGSTileGpuDualSrcSource);

	// Cs*(1 - As): the source factor, inverted.
	const u32 inv = GSDevice::gsTileGpuDualSrcTerms(2 * 27 + 0 * 9 + 0 * 3 + 0); // 2000
	EXPECT_EQ(inv, GSDevice::kGSTileGpuDualSrcSource | GSDevice::kGSTileGpuDualSrcSourceInv);

	// Cd*(1 - As): the DESTINATION factor alone.
	const u32 dst = GSDevice::gsTileGpuDualSrcTerms(2 * 27 + 1 * 9 + 0 * 3 + 1); // 2101
	EXPECT_EQ(dst, GSDevice::kGSTileGpuDualSrcDest);

	// C = Ad and C = FIX reach for no second source at all.
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcTerms(0 * 27 + 1 * 9 + 1 * 3 + 1), 0u); // 0111, C = Ad
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcTerms(0 * 27 + 1 * 9 + 2 * 3 + 1), 0u); // 0121, C = FIX
}

TEST(GSTileGpuDualSrcTerms, EveryDualSourceRowIsAnAsRow)
{
	// The structural fact the whole design rests on: if a row that selected Ad or FIX still named a
	// SRC1 factor, the carrier would be putting the wrong number in the alpha channel for it. Walk
	// the map rather than assert it in prose.
	for (u32 a = 0; a < 3; a++)
	{
		for (u32 b = 0; b < 3; b++)
		{
			for (u32 c = 0; c < 3; c++)
			{
				for (u32 d = 0; d < 3; d++)
				{
					const u32 index = a * 27 + b * 9 + c * 3 + d;
					if (GSDevice::gsTileGpuDualSrcTerms(index) != 0)
						EXPECT_EQ(c, 0u) << "blend row " << a << b << c << d << " names As without selecting it";
				}
			}
		}
	}
}

TEST(GSTileGpuDualSrcTerms, OutOfRangeIsNoTerms)
{
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcTerms(81), 0u);
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcTerms(0x7F), 0u);
}

// -- the road table ------------------------------------------------------------------------------

TEST(GSTileGpuDualSrcRoad, ARowWithNoAsFactorTakesNoRoad)
{
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(0, true, true, true, false), Road::None);
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(0, false, true, false, true), Road::None);
}

TEST(GSTileGpuDualSrcRoad, TheFeatureWinsWhereverItExists)
{
	const u32 both = GSDevice::kGSTileGpuDualSrcSource | GSDevice::kGSTileGpuDualSrcDest;
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(both, true, true, false, true), Road::DualSource);
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(GSDevice::kGSTileGpuDualSrcSource, true, false, true, false),
		Road::DualSource);
}

TEST(GSTileGpuDualSrcRoad, AsOnTheSourceAloneFolds)
{
	// The fold touches nothing but the draw's own colour, so nothing else about the draw can refuse
	// it -- not a written alpha channel, not a clamping factor, not an alpha the shader owns.
	const u32 src = GSDevice::kGSTileGpuDualSrcSource;
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(src, false, true, false, true), Road::FoldSource);
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(src, false, false, true, false), Road::FoldSource);
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(src | GSDevice::kGSTileGpuDualSrcSourceInv, false, true, false, false),
		Road::FoldSource);
}

TEST(GSTileGpuDualSrcRoad, AsOnTheDestinationTakesTheAlphaChannel)
{
	const u32 dst = GSDevice::kGSTileGpuDualSrcDest;
	// Alpha not written: the channel write mask throws the carrier away after the blend used it.
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(dst, false, false, false, false), Road::Carrier);
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(dst, false, false, false, true), Road::Carrier);
	// Alpha written and the factor never clamps: the alpha blend equation gives the byte back.
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(dst, false, true, true, false), Road::CarrierRestore);
	// ...but not if the fragment stage owns that alpha, because then there is a value in o_color.a
	// the carrier would be overwriting and the restore would not reproduce.
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(dst, false, true, true, true), Road::CarrierCompanion);
	// ...and not if the factor can clamp, because a clamped carrier cannot be divided back down.
	EXPECT_EQ(GSDevice::gsTileGpuDualSrcRoad(dst, false, true, false, false), Road::CarrierCompanion);
}

TEST(GSTileGpuDualSrcRoad, NoRowWithADestinationFactorEverFolds)
{
	// The substitution's safety property, executed over the whole map and every draw shape: the fold
	// replaces the SOURCE factor with one, so a row whose DESTINATION factor is As must never reach
	// it -- the fragment stage has no Cd to fold into. And on a device with no second output, a row
	// that names As must always come back with a road that does something about it.
	for (u32 index = 0; index < 81; index++)
	{
		const u32 terms = GSDevice::gsTileGpuDualSrcTerms(index);
		if (terms == 0)
			continue;
		for (int shape = 0; shape < 8; shape++)
		{
			const bool writes_alpha = (shape & 1) != 0;
			const bool unclamped = (shape & 2) != 0;
			const bool shaders_alpha = (shape & 4) != 0;
			const Road road = GSDevice::gsTileGpuDualSrcRoad(terms, false, writes_alpha, unclamped, shaders_alpha);
			EXPECT_NE(road, Road::None);
			EXPECT_NE(road, Road::DualSource);
			if (terms & GSDevice::kGSTileGpuDualSrcDest)
				EXPECT_NE(road, Road::FoldSource) << "row " << index << " folds a destination factor";
			else
				EXPECT_EQ(road, Road::FoldSource) << "row " << index << " has nothing but a source factor";
		}
	}
}

// -- the arithmetic ------------------------------------------------------------------------------

TEST(GSTileGpuDualSrcRoad, TheRestoreGivesEveryUnclampedAlphaByteBack)
{
	// The restore road is chosen only where the draw's alpha cannot pass 0x80, so walk exactly that
	// range. carrier = As * 255/128 in the fragment stage, then the alpha blend equation multiplies
	// by the constant 128/255 and the write rounds to a byte. Two float roundings stand between the
	// byte that went in and the byte that comes out, and the whole road rests on them cancelling.
	for (int a = 0; a <= 128; a++)
	{
		const float alpha = static_cast<float>(a) / 255.0f;
		const float carrier = AsFactor(alpha);
		const float restored = carrier * GSDevice::kGSTileGpuAlphaRestore;
		EXPECT_EQ(Unorm8(restored), a) << "alpha byte " << a << " did not survive the carrier";
	}
}

TEST(GSTileGpuDualSrcRoad, TheCarrierIsTheSameNumberTheSecondOutputCarried)
{
	// SRC1_COLOR reads (R1, G1, B1) and SRC_ALPHA reads As for all three channels. The substitution
	// is exact because the second output was a BROADCAST of one scalar -- so the value the carrier
	// puts in o_color.a is bit for bit the value o_blend held, clamp included.
	for (int a = 0; a <= 255; a++)
	{
		const float alpha = static_cast<float>(a) / 255.0f;
		const float second_output = std::min(alpha * (255.0f / 128.0f), 1.0f);
		EXPECT_EQ(AsFactor(alpha), second_output);
		if (a > 128)
			EXPECT_EQ(AsFactor(alpha), 1.0f) << "alpha byte " << a << " should have clamped";
	}
}

TEST(GSTileGpuDualSrcRoad, TheFoldStaysInRangeSoNoClampCanDiffer)
{
	// The fold moves the source multiply from the blend unit into the fragment stage, which is only
	// a substitution while nothing between the two clamps. Vulkan clamps a fixed-point attachment's
	// source components to [0, 1] BEFORE blending, so a fold whose product left that range would be
	// clamped at a different point in the arithmetic than the blend unit clamps at. It cannot: the
	// finished colour is already in range and both factors are too.
	for (int a = 0; a <= 255; a++)
	{
		const float factor = AsFactor(static_cast<float>(a) / 255.0f);
		ASSERT_GE(factor, 0.0f);
		ASSERT_LE(factor, 1.0f);
		for (int c = 0; c <= 255; c++)
		{
			const float colour = static_cast<float>(c) / 255.0f;
			EXPECT_GE(colour * factor, 0.0f);
			EXPECT_LE(colour * factor, 1.0f);
			EXPECT_GE(colour * (1.0f - factor), 0.0f);
			EXPECT_LE(colour * (1.0f - factor), 1.0f);
		}
	}
}

TEST(GSTileGpuDualSrcRoad, TheInvertedFoldIsOneMinusTheFactorNotOneMinusTheAlpha)
{
	// The trap this exists for: ONE_MINUS_SRC1_COLOR is one minus the FACTOR, which is As * 255/128,
	// and not one minus the alpha the draw stores. They differ by very nearly a factor of two, so
	// folding the wrong one is a wrong picture rather than a wrong last bit -- and it is exactly the
	// confusion the alpha carrier invites, since the two now live in the same channel.
	for (int a = 1; a <= 128; a++)
	{
		const float alpha = static_cast<float>(a) / 255.0f;
		EXPECT_NE(1.0f - AsFactor(alpha), 1.0f - alpha);
	}
	// ...and at the ends they do agree, which is why a spot check would have missed it.
	EXPECT_EQ(1.0f - AsFactor(0.0f), 1.0f - 0.0f);
}
