// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the requested-versus-emulated alpha mask rule (GS/Renderers/HW/GSDrawAlphaMask.h).
//
// The exact alpha drop clears the alpha byte of FBMSK, which is right for the shader and the
// barrier and wrong for anything asking what the draw asked for. Getting that backwards is not a
// hypothetical: reading the RTA de-correction decision off the cleared flag left a target alpha
// scaled where it used to de-correlate, and moved 41% of the pixels in two of Beyond Good and
// Evil's frames by a unit or two of colour.
//
// So the cases that matter are the two ends: with nothing dropped the rule has to reproduce the
// live config pair exactly, and with something dropped it has to hand back the byte the drop took
// rather than the zero the config now holds.
//
// Rides gs_vertex_tests -- the rule is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/HW/GSDrawAlphaMask.h"

#include <gtest/gtest.h>

using namespace GSDrawAlphaMask;

TEST(GSDrawAlphaMask, NothingDroppedReproducesTheLiveConfig)
{
	// This is the expression the call sites used before the rule existed, and every draw that
	// drops nothing -- which is nearly all of them -- still has to take exactly this road.
	for (u32 a = 0; a <= 0xFF; a++)
	{
		EXPECT_EQ(AsRequested(NothingDropped, true, a), a) << "alpha mask " << a;
		EXPECT_EQ(AsRequested(NothingDropped, false, a), 0u) << "alpha mask " << a;
	}
}

TEST(GSDrawAlphaMask, ADroppedByteSurvivesTheClearedFlag)
{
	// After the drop the config pair says "no mask at all". The draw still asked for one.
	EXPECT_EQ(AsRequested(0x80, false, 0), 0x80u);
	EXPECT_EQ(AsRequested(0x01, false, 0), 0x01u);

	// And it wins over a config that has since been filled in with something else.
	EXPECT_EQ(AsRequested(0x80, true, 0x0F), 0x80u);
}

TEST(GSDrawAlphaMask, ADroppedZeroIsStillADroppedValue)
{
	// NothingDropped is -1, not 0, so a recorded zero has to stay distinguishable from "no record".
	// The drop rule never records a zero -- it refuses a mask that holds nothing back -- but the
	// sentinel is what makes that a property of the caller rather than of the encoding.
	EXPECT_EQ(AsRequested(0, true, 0x80), 0u);
	EXPECT_EQ(AsRequested(NothingDropped, true, 0x80), 0x80u);
}

TEST(GSDrawAlphaMask, PartialIsNeitherEnd)
{
	EXPECT_FALSE(IsPartial(0x00));
	EXPECT_FALSE(IsPartial(0xFF));

	EXPECT_TRUE(IsPartial(0x80));
	EXPECT_TRUE(IsPartial(0x01));
	EXPECT_TRUE(IsPartial(0x7F));
	EXPECT_TRUE(IsPartial(0xFE));
}

TEST(GSDrawAlphaMask, PartialAgreesWithTheExpressionItReplaced)
{
	// partial_fbmask in DetermineAlphaScaling was `ps.fbmask && FbMask.a != 0xFF && FbMask.a != 0`.
	for (u32 a = 0; a <= 0xFF; a++)
	{
		for (bool masks : {false, true})
		{
			const bool before = masks && a != 0xFF && a != 0;
			EXPECT_EQ(IsPartial(AsRequested(NothingDropped, masks, a)), before)
				<< "alpha mask " << a << " masks " << masks;
		}
	}
}

TEST(GSDrawAlphaMask, ADroppedPartialMaskStillReadsAsPartial)
{
	// The whole point: the drop only ever takes a partially masked alpha byte, so every draw it
	// acts on must still answer "yes, partial" afterwards.
	EXPECT_TRUE(IsPartial(AsRequested(0x80, false, 0)));
	EXPECT_TRUE(IsPartial(AsRequested(0x01, false, 0)));
}

// The other half of the same distinction: what the SHADER has to do once the drop has taken the
// mask away. Dropping the mask takes the draw off the masked-write road, and that road quantizes
// the colour to integers on all four channels before it merges the destination in. Leaving that
// behind moved 41% of the pixels in two of Beyond Good and Evil's frames up by a unit or two of
// colour -- signed one way, because what was lost was a downward truncation.

TEST(GSDrawAlphaMask, ADropToNothingHasToQuantizeOnItsOwn)
{
	// The shape the exact alpha drop produces: the draw asked for an alpha-only mask (nibble 0x8)
	// and ends up with no mask at all.
	EXPECT_TRUE(NeedsColorQuantize(0x8, 0x0));
}

TEST(GSDrawAlphaMask, ADrawThatDroppedNothingQuantizesOnlyWhenItAlreadyDid)
{
	// Every draw that drops nothing passes the same nibble twice, and none of them may acquire the
	// bit: with a mask it is already on the road, without one it never was.
	for (u32 nibble = 0; nibble <= 0xF; nibble++)
		EXPECT_FALSE(NeedsColorQuantize(nibble, nibble)) << "nibble " << nibble;
}

TEST(GSDrawAlphaMask, ADropThatLeavesAnotherChannelMaskedStaysOnTheRoad)
{
	// The road is per-draw, not per-channel. As long as one channel is still masked the shader
	// runs it, and quantizing again would be the same work twice.
	EXPECT_FALSE(NeedsColorQuantize(0xC, 0x4));
	EXPECT_FALSE(NeedsColorQuantize(0xF, 0x7));
}

TEST(GSDrawAlphaMask, TheBitIsExactlyLeavingTheRoad)
{
	// Stated over every pair, so the rule is the definition rather than a set of examples.
	for (u32 requested = 0; requested <= 0xF; requested++)
	{
		for (u32 emulated = 0; emulated <= 0xF; emulated++)
		{
			const bool was_on_the_road = requested != 0;
			const bool is_on_the_road = emulated != 0;
			EXPECT_EQ(NeedsColorQuantize(requested, emulated), was_on_the_road && !is_on_the_road)
				<< "requested " << requested << " emulated " << emulated;
		}
	}
}

// The third rule: who owns the primary colour output's alpha byte. On a GPU with no dual-source
// blend unit the blend-mix factor substitution hands the blend unit its factor through that byte,
// which is only sound when nothing else is keeping it. Its own guard asked ps.fbmask -- true until
// the exact alpha drop started clearing that flag on a draw that is still writing a particular
// alpha byte on purpose.

TEST(GSDrawAlphaMask, AnUnmaskedDrawLeavesItsAlphaByteFree)
{
	// The road's whole population: no mask anywhere, nothing asked for. It must stay open, or the
	// Mali blend-mix substitution stops working for the draws it was built for.
	EXPECT_FALSE(AlphaOutputIsSpokenFor(false, 0));
}

TEST(GSDrawAlphaMask, AShaderMaskClaimsTheAlphaByte)
{
	// The pre-existing half of the guard, restated: any channel still masked in the shader.
	EXPECT_TRUE(AlphaOutputIsSpokenFor(true, 0));
	EXPECT_TRUE(AlphaOutputIsSpokenFor(true, 0x80));
}

TEST(GSDrawAlphaMask, ADroppedDrawStillClaimsTheAlphaByte)
{
	// The hole. The shader carries no mask -- the drop cleared it -- and the byte it writes is
	// exactly the one the mask was protecting. Before this rule the factor road saw an unmasked
	// draw and took the byte.
	EXPECT_TRUE(AlphaOutputIsSpokenFor(false, 0x80));
	EXPECT_TRUE(AlphaOutputIsSpokenFor(false, 0x7F));
	EXPECT_TRUE(AlphaOutputIsSpokenFor(false, 0x01));
}

TEST(GSDrawAlphaMask, TheClaimIsExactlyTheTwoMasksTogether)
{
	// Stated over every pair, and tied to AsRequested so the two rules cannot drift: a draw that
	// dropped nothing is spoken for on exactly the old condition, and one that dropped something is
	// always spoken for.
	for (u32 alpha_mask = 0; alpha_mask <= 0xFF; alpha_mask++)
	{
		for (int shader_masks = 0; shader_masks <= 1; shader_masks++)
		{
			const bool masks = shader_masks != 0;
			// A draw that dropped nothing is spoken for exactly when the shader still masks
			// something, which is the guard the road always had.
			EXPECT_EQ(AlphaOutputIsSpokenFor(masks, AsRequested(NothingDropped, masks, alpha_mask)), masks)
				<< "undropped, mask " << alpha_mask;
		}

		if (alpha_mask == 0)
			continue;

		EXPECT_TRUE(AlphaOutputIsSpokenFor(false, AsRequested(static_cast<int>(alpha_mask), false, 0)))
			<< "dropped " << alpha_mask;
	}
}
