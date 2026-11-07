// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the arithmetic of the stencil counter road (GS/Renderers/Common/GSStencilCounter.h): which
// primitive colours count as a step of which sign, the mask a 16-bit target applies first, and that
// a stencil kept with wrapping increments and decrements resolves to the same byte as the colour
// wrap it replaces, whatever the order and depth of the steps.
//
// Rides gs_vertex_tests.

#include "GS/Renderers/Common/GSStencilCounter.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

using GSStencilCounter::Sign;
using GSStencilCounter::Step;

namespace
{
	// Classifies a primitive of grey level (r, g, b) written to `rgb_mask` through the shader's `mask`.
	Sign Grey(u8 r, u8 g, u8 b, u8 rgb_mask, u8 mask, Step& step, bool& have)
	{
		return GSStencilCounter::Classify(GSStencilCounter::StepOf(r, g, b, rgb_mask, mask), step, have);
	}
} // namespace

TEST(GSStencilCounter, ShadowOfTheColossusSteps)
{
	// +1 and 255 (= -1) on a 32-bit target, whichever comes first.
	Step step;
	bool have = false;
	EXPECT_EQ(Grey(1, 1, 1, 7, 0xFF, step, have), Sign::Plus);
	EXPECT_EQ(step.c[0], 1);
	EXPECT_EQ(Grey(255, 255, 255, 7, 0xFF, step, have), Sign::Minus);

	have = false;
	EXPECT_EQ(Grey(255, 255, 255, 7, 0xFF, step, have), Sign::Plus);
	EXPECT_EQ(step.c[2], 255);
	EXPECT_EQ(Grey(1, 1, 1, 7, 0xFF, step, have), Sign::Minus);
}

TEST(GSStencilCounter, SpiderMan3StepsOnASixteenBitTarget)
{
	// 0x0b and 0xf9 are +8 and -8 once the low three bits go; 0 adds nothing.
	Step step;
	bool have = false;
	EXPECT_EQ(Grey(0x0b, 0x0b, 0x0b, 7, 0xF8, step, have), Sign::Plus);
	EXPECT_EQ(step.c[1], 8);
	EXPECT_EQ(Grey(0xf9, 0xf9, 0xf9, 7, 0xF8, step, have), Sign::Minus);
	EXPECT_EQ(Grey(0, 0, 0, 7, 0xF8, step, have), Sign::Zero);
	// Channels that differ only below the mask are the same step.
	EXPECT_EQ(Grey(0x0b, 0x09, 0x0f, 7, 0xF8, step, have), Sign::Plus);
	// Without the mask, 0x0b and 0xf9 are not negatives of each other.
	have = false;
	EXPECT_EQ(Grey(0x0b, 0x0b, 0x0b, 7, 0xFF, step, have), Sign::Plus);
	EXPECT_EQ(Grey(0xf9, 0xf9, 0xf9, 7, 0xFF, step, have), Sign::NotACounter);
}

TEST(GSStencilCounter, OnlyWrittenChannelsCount)
{
	// A counter in green alone: red and blue are masked off, whatever they hold.
	Step step;
	bool have = false;
	EXPECT_EQ(Grey(0x00, 0x01, 0x40, 2, 0xFF, step, have), Sign::Plus);
	EXPECT_EQ(Grey(0x7f, 0xff, 0x00, 2, 0xFF, step, have), Sign::Minus);
	EXPECT_EQ(Grey(0x12, 0x00, 0x34, 2, 0xFF, step, have), Sign::Zero);
	EXPECT_EQ(step.c[0], 0);
	EXPECT_EQ(step.c[2], 0);
}

TEST(GSStencilCounter, ChannelsMayStepByDifferentAmountsWithOneSign)
{
	Step step;
	bool have = false;
	EXPECT_EQ(Grey(1, 2, 0, 7, 0xFF, step, have), Sign::Plus);
	EXPECT_EQ(Grey(255, 254, 0, 7, 0xFF, step, have), Sign::Minus);
	// One channel up while another goes down is two counts, not one.
	EXPECT_EQ(Grey(1, 254, 0, 7, 0xFF, step, have), Sign::NotACounter);
	// A channel that steps in one primitive and stands still in another is not one count either.
	EXPECT_EQ(Grey(1, 0, 0, 7, 0xFF, step, have), Sign::NotACounter);
}

TEST(GSStencilCounter, RefusesWhatIsNotACounter)
{
	Step step;
	bool have = false;
	EXPECT_EQ(Grey(1, 1, 1, 7, 0xFF, step, have), Sign::Plus);
	EXPECT_EQ(Grey(2, 2, 2, 7, 0xFF, step, have), Sign::NotACounter);
	EXPECT_EQ(Grey(254, 254, 254, 7, 0xFF, step, have), Sign::NotACounter);
}

TEST(GSStencilCounter, HalfWayIsItsOwnNegative)
{
	Step step;
	bool have = false;
	EXPECT_EQ(Grey(128, 128, 128, 7, 0xFF, step, have), Sign::Plus);
	EXPECT_EQ(Grey(128, 128, 128, 7, 0xFF, step, have), Sign::Plus);
}

TEST(GSStencilCounter, StencilResolveEqualsColourWrapAtAnyDepth)
{
	// The colour road adds every step to the channel mod 256; the stencil road keeps
	// n_plus - n_minus mod 256 in a byte that wraps both ways, then adds step * k once.
	std::mt19937 rng(20261106);
	for (const u8 step : {u8{1}, u8{8}, u8{3}, u8{128}, u8{255}})
	{
		for (int trial = 0; trial < 200; trial++)
		{
			const u8 cd = static_cast<u8>(rng());
			const int layers = static_cast<int>(rng() % 700); // past 256 both ways
			u8 colour = cd;
			u8 stencil = 0;
			for (int i = 0; i < layers; i++)
			{
				if (rng() & 1)
				{
					colour = static_cast<u8>(colour + step);
					stencil = static_cast<u8>(stencil + 1); // INCR_WRAP
				}
				else
				{
					colour = static_cast<u8>(colour + static_cast<u8>(256 - step));
					stencil = static_cast<u8>(stencil - 1); // DECR_WRAP
				}
			}
			ASSERT_EQ(GSStencilCounter::Resolve(cd, step, stencil), colour)
				<< "step " << int(step) << " cd " << int(cd) << " layers " << layers;
		}
	}
}

TEST(GSStencilCounter, SixteenBitTargetMaskFollowsTheShader)
{
	GSHWDrawConfig::PSSelector ps;
	ps.key_hi = 0;
	ps.key_lo = 0;
	EXPECT_EQ(GSStencilCounter::SourceColourMask(ps), 0xFF);

	ps.dst_fmt = GSLocalMemory::PSM_FMT_16;
	EXPECT_EQ(GSStencilCounter::SourceColourMask(ps), 0xF8);

	// Software blend path (the counter's Cs * AF): still masked.
	ps.blend_a = 0;
	ps.blend_b = 2;
	ps.blend_c = 2;
	ps.blend_d = 2;
	EXPECT_EQ(GSStencilCounter::SourceColourMask(ps), 0xF8);

	// The hardware-blend path with blend_hw set leaves the colour alone, as tfx.glsl does.
	ps.blend_b = 0;
	ps.blend_d = 0;
	ps.blend_hw = 1;
	EXPECT_EQ(GSStencilCounter::SourceColourMask(ps), 0xFF);

	// Dither 3 (no dither, 16-bit unmasked) keeps all eight bits.
	ps.blend_hw = 0;
	ps.dither = 3;
	EXPECT_EQ(GSStencilCounter::SourceColourMask(ps), 0xFF);
}
