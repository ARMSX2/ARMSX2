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
