// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the fixed-blend-factor reroute (GS/Renderers/Common/GSBlendConstantPolicy.h): on a driver
// that ignores the blend constant, a draw whose factor is AFIX sends that number through the second
// fragment output instead.
//
// Two halves. The remap itself is checked against the blend table, because the table already
// contains the answer: every fixed-factor row is the As row of the same PS2 equation with SRC1
// swapped for the constant, so remapping a C == 2 row must land exactly on its C == 0 twin. If it
// does not, the rewrite is saying something the blend unit was never asked for.
//
// The other half is the refusals, and it is the half that matters. Every device without the bug bit
// must take byte-identical decisions, and the only device that takes the changed road is one nobody
// here can watch a pixel on. So each guard is named and pinned: what makes the second output
// unavailable, and what makes the value in it something other than AFIX/128.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSBlendConstantPolicy.h"

#include <gtest/gtest.h>

namespace
{
	using BlendState = GSHWDrawConfig::BlendState;

	// The blend table index the renderer computes, from the four ALPHA register fields.
	constexpr u32 BlendIndex(u32 a, u32 b, u32 c, u32 d) { return ((a * 3 + b) * 3 + c) * 3 + d; }

	// The state GSRendererHW::EmulateBlending emits for a hardware-blended draw: the table's two
	// colour factors, ONE/ZERO on alpha, and AFIX riding the blend constant.
	BlendState StateFor(u32 a, u32 b, u32 c, u32 d, u8 afix)
	{
		const HWBlend blend = GSDevice::GetBlend(BlendIndex(a, b, c, d));
		return BlendState(true, blend.src, blend.dst, blend.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, c == 2, afix);
	}

	// A device with the defect, on a plain fixed-factor draw with nothing else claiming the second
	// output.
	GSBlendConstantPolicy::DrawInputs BrokenDriver()
	{
		GSBlendConstantPolicy::DrawInputs in;
		in.broken_blend_constant = true;
		in.dual_source_blend = true;
		in.blend_c = 2;
		return in;
	}
} // namespace

// Katamari Damacy's ball: (Cs - Cd)*F + Cd with AFIX 127, which the table resolves to
// ONE / INV_CONST_COLOR and the backend hands to vkCmdSetBlendConstants as 127/128. This is the
// draw the whole change exists for.
TEST(GSBlendConstantPolicy, KatamariFixedFactorMixMovesToSecondOutput)
{
	const BlendState before = StateFor(0, 1, 2, 1, 127);
	ASSERT_EQ(before.src_factor, GSDevice::CONST_COLOR);
	ASSERT_EQ(before.dst_factor, GSDevice::INV_CONST_COLOR);

	// EmulateBlending's blend-mix arm replaces the source factor with ONE and does that multiply in
	// the shader; only the destination factor reaches the blender as a constant.
	const BlendState mix(true, GSDevice::CONST_ONE, before.dst_factor, before.op, GSDevice::CONST_ONE,
		GSDevice::CONST_ZERO, true, 127);
	EXPECT_TRUE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(mix, BrokenDriver()));

	const BlendState after = GSBlendConstantPolicy::RemapToSecondOutput(mix);
	EXPECT_EQ(after.src_factor, GSDevice::CONST_ONE);
	EXPECT_EQ(after.dst_factor, GSDevice::INV_SRC1_COLOR);
	EXPECT_EQ(after.op, mix.op);
	EXPECT_TRUE(after.enable);
	// Nothing reads the constant any more, so nothing should set it.
	EXPECT_FALSE(after.constant_enable);
	EXPECT_EQ(after.constant, 0u);
}

// The blend table is its own oracle. Row ABCD with C == 2 is row ABCD with C == 0 with the constant
// factors standing in for the SRC1 ones, so remapping the fixed row must reproduce the As row
// exactly -- same op, same factors, on all seventeen equations whose fixed row has a constant.
TEST(GSBlendConstantPolicy, RemapReproducesTheAsRowOfEveryEquation)
{
	u32 rows_with_a_constant = 0;
	for (u32 a = 0; a < 3; a++)
	{
		for (u32 b = 0; b < 3; b++)
		{
			for (u32 d = 0; d < 3; d++)
			{
				const HWBlend fixed = GSDevice::GetBlend(BlendIndex(a, b, 2, d));
				const HWBlend as = GSDevice::GetBlend(BlendIndex(a, b, 0, d));
				if (!GSDevice::IsConstantBlendFactor(fixed.src) && !GSDevice::IsConstantBlendFactor(fixed.dst))
					continue;

				rows_with_a_constant++;
				EXPECT_EQ(fixed.op, as.op) << "equation " << a << b << "2" << d;
				EXPECT_EQ(GSBlendConstantPolicy::RemapFactor(fixed.src), as.src) << "equation " << a << b << "2" << d;
				EXPECT_EQ(GSBlendConstantPolicy::RemapFactor(fixed.dst), as.dst) << "equation " << a << b << "2" << d;
			}
		}
	}
	// Katamari's 0121 is one of the seventeen; the count is pinned so a table edit that drops or
	// adds a constant row has to come past this test.
	EXPECT_EQ(rows_with_a_constant, 17u);
}

// Everything that is not a constant-colour factor comes through the remap untouched, so a state the
// policy declines is a state the policy cannot alter even if it were applied by mistake.
TEST(GSBlendConstantPolicy, RemapTouchesNothingElse)
{
	for (u8 f = 0; f <= GSDevice::CONST_ZERO; f++)
	{
		if (f == GSDevice::CONST_COLOR || f == GSDevice::INV_CONST_COLOR)
			continue;
		EXPECT_EQ(GSBlendConstantPolicy::RemapFactor(f), f) << "factor " << static_cast<u32>(f);
	}

	// The As form of Katamari's equation: already dual-source, and identical after the remap.
	const BlendState as_row = StateFor(0, 1, 0, 1, 0);
	const BlendState after = GSBlendConstantPolicy::RemapToSecondOutput(as_row);
	EXPECT_EQ(after.src_factor, as_row.src_factor);
	EXPECT_EQ(after.dst_factor, as_row.dst_factor);
}

// The refusals. Each of these leaves the constant path exactly as it is today.
TEST(GSBlendConstantPolicy, RefusedWithoutTheBugBit)
{
	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_CONST_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 127);

	GSBlendConstantPolicy::DrawInputs in = BrokenDriver();
	in.broken_blend_constant = false;
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(mix, in));
}

TEST(GSBlendConstantPolicy, RefusedWithoutASecondOutput)
{
	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_CONST_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 127);

	GSBlendConstantPolicy::DrawInputs in = BrokenDriver();
	in.dual_source_blend = false;
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(mix, in));
}

// Above 1.0 the constant path has clamp behaviour of its own, and whether the second output
// reproduces it is a separate question. Out of scope on purpose: 128 in, 129 out.
TEST(GSBlendConstantPolicy, RefusedAboveOne)
{
	GSBlendConstantPolicy::DrawInputs in = BrokenDriver();

	const BlendState at_one(true, GSDevice::CONST_ONE, GSDevice::INV_CONST_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 128);
	EXPECT_TRUE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(at_one, in));

	const BlendState above(true, GSDevice::CONST_ONE, GSDevice::INV_CONST_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 129);
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(above, in));
}

// As and Ad factors are not the fixed one, and the second output already carries As for them.
TEST(GSBlendConstantPolicy, RefusedWhenTheFactorIsNotAfix)
{
	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_CONST_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 127);

	for (u8 c : {0, 1})
	{
		GSBlendConstantPolicy::DrawInputs in = BrokenDriver();
		in.blend_c = c;
		EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(mix, in)) << "blend_c " << static_cast<u32>(c);
	}
}

// A draw with no constant factor has nothing to move, whatever else is true of it.
TEST(GSBlendConstantPolicy, RefusedWithNoConstantFactor)
{
	const BlendState plain(true, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, GSDevice::OP_ADD, GSDevice::CONST_ONE,
		GSDevice::CONST_ZERO, false, 0);
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(plain, BrokenDriver()));

	// Nor does a disabled blend state, even one still carrying a constant.
	const BlendState disabled(false, GSDevice::CONST_ONE, GSDevice::INV_CONST_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 127);
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(disabled, BrokenDriver()));
}

// The second output is already carrying something. Every one of these would still read AFIX/128 out
// of it if the reroute went ahead, and get a different number.
TEST(GSBlendConstantPolicy, RefusedWhenTheSecondOutputIsSpokenFor)
{
	// A blend_hw type that rewrites the second output's RGB always comes with a SRC1 factor in the
	// same state -- BLEND_MIX2's overflow compensation is the shape.
	const BlendState mixed(true, GSDevice::CONST_ONE, GSDevice::SRC1_COLOR, GSDevice::OP_SUBTRACT,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 127);
	EXPECT_TRUE(GSBlendConstantPolicy::ReadsSecondOutput(mixed));
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(mixed, BrokenDriver()));

	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_CONST_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 127);

	// The blend multi-pass second draw shares the pixel shader with the first, so if it reads the
	// second output, rewriting that output would land on it too.
	GSBlendConstantPolicy::DrawInputs multi = BrokenDriver();
	multi.multi_pass_reads_second_output = true;
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(mix, multi));

	// PABE reads the second output's alpha as the source alpha.
	GSBlendConstantPolicy::DrawInputs pabe = BrokenDriver();
	pabe.pabe = true;
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(mix, pabe));

	// And the no-dual-source substitution is already using the factor for something else.
	GSBlendConstantPolicy::DrawInputs in_alpha = BrokenDriver();
	in_alpha.blend_factor_in_alpha = true;
	EXPECT_FALSE(GSBlendConstantPolicy::CanRouteFixedFactorToSecondOutput(mix, in_alpha));
}
