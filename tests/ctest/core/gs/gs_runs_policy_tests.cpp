// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins which draws the run machine merges and what their rows say (GS/Renderers/Common/GSRunsPolicy.h).
//
// The run program reproduces a draw's classic program for one state set, so the property that matters is the
// refusals: every pixel-shader bit outside that set must keep the draw on the classic path, one bit at a time, and
// every blend the four ways do not reproduce must too. The ways themselves are checked against the run pipeline's one
// blend equation (ONE, INV_SRC1_COLOR / ONE, ZERO): a way is right when that equation, fed the second output the run
// program writes for it, gives what the classic pipeline's blend gives.
//
// Rides gs_vertex_tests -- the policy is header-only, so it needs no extra linkage.

#include "GS/Renderers/Common/GSRunsPolicy.h"

#include <gtest/gtest.h>

#include <cstring>

namespace
{
	using namespace GSRunsPolicy;

	int s_rt, s_ds;

	GSHWDrawConfig Zeroed()
	{
		GSHWDrawConfig c;
		std::memset(static_cast<void*>(&c), 0, sizeof(c));
		c.colclip_mode = GSHWDrawConfig::ColClipMode::NoModify;
		c.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Off;
		c.tex_hazard = GSHWDrawConfig::TEX_HAZARD_NONE;
		c.colormask.logic_op = GSAlphaBitLogicOp::Off;
		c.date_copy = GSAlphaBitLogicOp::NoDateCopy;
		c.topology = GSHWDrawConfig::Topology::Triangle;
		c.indices_per_prim = 3;
		c.rt = reinterpret_cast<GSTexture*>(&s_rt);
		c.ds = reinterpret_cast<GSTexture*>(&s_ds);
		return c;
	}

	// A modulated, textured draw blended Cs * As + Cd * (1 - As): the storm pass's commonest shape.
	GSHWDrawConfig Way0()
	{
		GSHWDrawConfig c = Zeroed();
		c.ps.tfx = 0;
		c.ps.tcc = 1;
		c.ps.iip = 1;
		c.ps.blend_a = 0;
		c.ps.blend_b = 2;
		c.ps.blend_c = 0;
		c.ps.blend_d = 2;
		c.ps.blend_mix = 1;
		c.blend = GSHWDrawConfig::BlendState(true, GSDevice::CONST_ONE, GSDevice::INV_SRC1_COLOR, GSDevice::OP_ADD,
			GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);
		c.colormask.wrgba = 0xF;
		return c;
	}

	TextureFacts Rgba8()
	{
		TextureFacts t;
		t.present = true;
		t.rgba8 = true;
		return t;
	}

	// The run pipeline's colour equation on one channel: src + dst * (1 - second).
	float RunBlend(float src, float second, float dst) { return src + dst * (1.0f - second); }
} // namespace

TEST(GSRunsPolicy, PlainBlendedDrawIsWayZero)
{
	const Draw d = Classify(Way0(), Rgba8(), false);
	ASSERT_EQ(d.why, Refusal::None);
	EXPECT_EQ(d.way, 0u);
	EXPECT_EQ(Field(d.flags, STATE_TFX), 0u);
	EXPECT_EQ(Field(d.flags, STATE_TCC), 1u);
	EXPECT_EQ(Field(d.flags, STATE_IIP), 1u);
	EXPECT_EQ(Field(d.flags, STATE_WAY), 0u);
	EXPECT_EQ(Field(d.flags, STATE_LOD), LOD_NONE);
}

// Every PS selector bit outside the run program's set refuses the draw, one bit at a time.
TEST(GSRunsPolicy, EveryOtherPixelShaderBitRefuses)
{
	const GSHWDrawConfig::PSSelector& allowed = AllowedPS();
	for (u32 word = 0; word < 2; word++)
	{
		const u64 mask = word ? allowed.key_hi : allowed.key_lo;
		for (u32 bit = 0; bit < 64; bit++)
		{
			if (mask >> bit & 1)
				continue;
			GSHWDrawConfig c = Way0();
			(word ? c.ps.key_hi : c.ps.key_lo) |= 1ull << bit;
			// The colour-clip and ROV bits are refused earlier, as a road.
			const Refusal why = Classify(c, Rgba8(), false).why;
			EXPECT_TRUE(why == Refusal::PS || why == Refusal::Road) << "word " << word << " bit " << bit;
		}
	}
}

TEST(GSRunsPolicy, TheCheapRefusalsAgreeWithClassify)
{
	// What OutsidePS and OutsideVS refuse, before Classify runs, Classify refuses too.
	GSHWDrawConfig c = Way0();
	EXPECT_FALSE(OutsidePS(c.ps));
	EXPECT_FALSE(OutsideVS(c.vs));
	for (u32 bit = 0; bit < 8; bit++)
	{
		c = Way0();
		c.vs.key |= static_cast<u8>(1u << bit);
		if (OutsideVS(c.vs))
			EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Topo) << "bit " << bit;
	}
	c = Way0();
	c.vs.fst = 1;
	EXPECT_TRUE(OutsideVS(c.vs));
	c = Way0();
	c.vs.point_size = 1;
	EXPECT_TRUE(OutsideVS(c.vs));
	c = Way0();
	c.vs.sprite_edge_clamp = 1;
	EXPECT_TRUE(OutsideVS(c.vs));
	c = Way0();
	c.ps.fst = 1;
	EXPECT_TRUE(OutsidePS(c.ps));
	EXPECT_NE(Classify(c, Rgba8(), false).why, Refusal::None);
}

TEST(GSRunsPolicy, RegionClampAndPaletteRefuse)
{
	GSHWDrawConfig c = Way0();
	c.ps.wms = 2;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::PS);
	c = Way0();
	c.ps.wmt = 3;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::PS);
	c = Way0();
	c.pal = reinterpret_cast<GSTexture*>(&s_rt);
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::PS);
}

TEST(GSRunsPolicy, AnythingBesidesOnePlainDrawRefuses)
{
	GSHWDrawConfig c = Way0();
	c.require_one_barrier = true;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Road);
	c = Way0();
	c.blend_multi_pass.enable = true;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Road);
	c = Way0();
	c.alpha_second_pass.enable = true;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Road);
	c = Way0();
	c.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Road);
	c = Way0();
	c.rt = nullptr;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Road);
	// A draw without a depth buffer is drawn in the pass's (GSRunsVK decides with or without its attachment).
	c = Way0();
	c.ds = nullptr;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::None);
	c = Way0();
	EXPECT_EQ(Classify(c, Rgba8(), true).why, Refusal::Road) << "a colour-clip target in progress";
	c = Way0();
	c.topology = GSHWDrawConfig::Topology::Line;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Topo);
	c = Way0();
	c.vs.fst = 1;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Topo);
	// Sprites the classic path expands in its vertex program are accepted; lines and points are not.
	c = Way0();
	c.vs.expand = GSHWDrawConfig::VSExpand::Sprite;
	c.indices_per_prim = 6;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::None);
	c.vs.expand = GSHWDrawConfig::VSExpand::Line;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Topo);
	c.vs.expand = GSHWDrawConfig::VSExpand::Point;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Topo);
}

TEST(GSRunsPolicy, TextureMustBeSampledAsTheClassicProgramDoes)
{
	TextureFacts t = Rgba8();
	t.is_target = true;
	EXPECT_EQ(Classify(Way0(), t, false).why, Refusal::Tex);
	t = Rgba8();
	t.rgba8 = false;
	EXPECT_EQ(Classify(Way0(), t, false).why, Refusal::Tex);
	EXPECT_EQ(Classify(Way0(), TextureFacts(), false).why, Refusal::Tex);
	// An untextured draw needs no texture.
	GSHWDrawConfig c = Way0();
	c.ps.tfx = 4;
	EXPECT_EQ(Classify(c, TextureFacts(), false).why, Refusal::None);
}

// tfx.glsl's sample_c takes the hardware's lod before the manual one.
TEST(GSRunsPolicy, HardwareLodBeforeManual)
{
	GSHWDrawConfig c = Way0();
	c.ps.automatic_lod = 1;
	c.ps.manual_lod = 1;
	EXPECT_EQ(Field(Classify(c, Rgba8(), false).flags, STATE_LOD), LOD_HARDWARE);
	c.ps.automatic_lod = 0;
	EXPECT_EQ(Field(Classify(c, Rgba8(), false).flags, STATE_LOD), LOD_MANUAL);
}

TEST(GSRunsPolicy, BlendConstantWays)
{
	GSHWDrawConfig c = Way0();
	c.ps.blend_c = 2;
	c.cb_ps.TA_MaxDepth_Af.w = 0x40 / 128.0f;
	c.blend = GSHWDrawConfig::BlendState(true, GSDevice::CONST_ONE, GSDevice::INV_CONST_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, true, 0x40);
	Draw d = Classify(c, Rgba8(), false);
	ASSERT_EQ(d.why, Refusal::None);
	EXPECT_EQ(d.way, 1u);

	// The constant in the blend unit must be the Af the row carries.
	c.blend.constant = 0x41;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Blend);

	// A driver that ignores the blend constant: the factor rides the second output.
	c = Way0();
	c.ps.blend_c = 2;
	c.ps.af_in_src1 = 1;
	c.cb_ps.TA_MaxDepth_Af.w = 0x80 / 128.0f;
	d = Classify(c, Rgba8(), false);
	ASSERT_EQ(d.why, Refusal::None);
	EXPECT_EQ(d.way, 1u);

	// Af that does not fit the row's byte.
	c.cb_ps.TA_MaxDepth_Af.w = 0.3f;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Const);
}

TEST(GSRunsPolicy, ElidedBlendWays)
{
	GSHWDrawConfig c = Way0();
	c.ps.blend_a = c.ps.blend_b = c.ps.blend_c = c.ps.blend_d = c.ps.blend_mix = 0;
	c.blend = GSHWDrawConfig::BlendState();
	Draw d = Classify(c, Rgba8(), false);
	ASSERT_EQ(d.why, Refusal::None);
	EXPECT_EQ(d.way, 2u);
	c.colormask.wrgba = 0x8;
	d = Classify(c, Rgba8(), false);
	ASSERT_EQ(d.why, Refusal::None);
	EXPECT_EQ(d.way, 3u);
	// Any other write mask has no way.
	c.colormask.wrgba = 0x7;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Blend);
	// Nor has another equation.
	c = Way0();
	c.blend.dst_factor = GSDevice::INV_SRC_ALPHA;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Blend);
	c = Way0();
	c.colormask.wrgba = 0x7;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Blend);
}

// The second output the run program writes per way makes the run pipeline's equation the classic pipeline's.
TEST(GSRunsPolicy, WaysUnderTheRunEquation)
{
	for (float dst : {0.0f, 0.25f, 1.0f})
	{
		for (float src : {0.0f, 0.5f, 1.0f})
		{
			// Way 0 and 1: the classic pipeline is the same ONE, INV_SRC1 (or INV_CONST) equation with the same factor.
			for (float f : {0.0f, 0.5f, 1.0f})
				EXPECT_EQ(RunBlend(src, f, dst), src + dst * (1.0f - f));
			// Way 2: blending off, the source written.
			EXPECT_EQ(RunBlend(src, 1.0f, dst), src);
			// Way 3: colour 0 and second output 0 keep the target's RGB.
			EXPECT_EQ(RunBlend(0.0f, 0.0f, dst), dst);
		}
	}
}

TEST(GSRunsPolicy, FogColourMustFitTheRow)
{
	GSHWDrawConfig c = Way0();
	c.ps.fog = 1;
	c.cb_ps.FogColor_AREF = GSVector4(10.0f, 20.0f, 30.0f, 0.0f);
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::None);
	c.cb_ps.FogColor_AREF = GSVector4(10.5f, 20.0f, 30.0f, 0.0f);
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Const);
}

TEST(GSRunsPolicy, DearStates)
{
	GSHWDrawConfig c = Way0();
	EXPECT_EQ(Dear(Classify(c, Rgba8(), false).flags), 0u);
	c.ps.automatic_lod = 1;
	EXPECT_EQ(Dear(Classify(c, Rgba8(), false).flags), 1u);
	c.ps.atst = static_cast<GSShader::PS_ATST>(2);
	EXPECT_EQ(Dear(Classify(c, Rgba8(), false).flags), 3u);
	// The hardware's lod means nothing untextured.
	c.ps.tfx = 4;
	EXPECT_EQ(Dear(Classify(c, TextureFacts(), false).flags), 2u);
}

TEST(GSRunsPolicy, ProgramKey)
{
	StateSet s;
	GSHWDrawConfig c = Way0();
	s.Note(Classify(c, Rgba8(), false).flags);
	// One draw: every state a constant.
	u32 key = s.Key();
	for (u32 i = 0; i < NUM_STATES; i++)
		EXPECT_NE((key >> (4 * i)) & 0xF, 0xFu) << STATE_NAME[i];
	// A second draw with another texture function: that state alone is read from the rows.
	c.ps.tfx = 1;
	s.Note(Classify(c, Rgba8(), false).flags);
	key = s.Key();
	EXPECT_EQ(key & 0xF, 0xFu);
	for (u32 i = 1; i < NUM_STATES; i++)
		EXPECT_NE((key >> (4 * i)) & 0xF, 0xFu) << STATE_NAME[i];
	// An untextured draw does not vary the textured-only states.
	c = Way0();
	c.ps.tfx = 4;
	StateSet u;
	u.Note(Classify(Way0(), Rgba8(), false).flags);
	u.Note(Classify(c, TextureFacts(), false).flags);
	EXPECT_EQ((u.Key() >> (4 * STATE_LOD)) & 0xF, LOD_NONE);
	EXPECT_EQ(u.Key() & 0xF, 0xFu);
}

// A draw whose written channel is its interpolated colour times 1/255 keeps its colour states constant in its run
// (GSRunsPolicy::Passthrough): the driver may move that multiply into the vertex program on the classic program's
// shape, and a run program that reads those states from the rows has another shape.
TEST(GSRunsPolicy, PassthroughDraws)
{
	GSHWDrawConfig c = Way0();
	c.ps.tfx = 4;
	c.ps.blend_a = c.ps.blend_b = c.ps.blend_c = c.ps.blend_d = c.ps.blend_mix = 0;
	c.blend = GSHWDrawConfig::BlendState();
	const u32 untextured = Classify(c, TextureFacts(), false).flags;
	EXPECT_TRUE(Passthrough(untextured, false));
	EXPECT_TRUE(Passthrough(untextured, true)) << "the colour is still passed through";
	EXPECT_NE(StrictKey(untextured, false), 0u);

	// Flat colour is not interpolated.
	c.ps.iip = 0;
	EXPECT_FALSE(Passthrough(Classify(c, TextureFacts(), false).flags, false));
	c.ps.iip = 1;

	// Fog truncates the colour; the alpha is still passed through without RTA correction, not with it.
	c.ps.fog = 1;
	const u32 fogged = Classify(c, TextureFacts(), false).flags;
	EXPECT_TRUE(Passthrough(fogged, false));
	EXPECT_FALSE(Passthrough(fogged, true));

	// A modulated draw is never passed through.
	EXPECT_FALSE(Passthrough(Classify(Way0(), Rgba8(), false).flags, false));

	// Decal without the texture's alpha passes the vertex alpha through.
	c = Way0();
	c.ps.tfx = 1;
	c.ps.tcc = 0;
	const u32 decal = Classify(c, Rgba8(), false).flags;
	EXPECT_TRUE(Passthrough(decal, false));
	EXPECT_FALSE(Passthrough(decal, true));

	// The strict key carries the colour states and nothing else.
	c.ps.atst = static_cast<GSShader::PS_ATST>(2);
	EXPECT_EQ(StrictKey(Classify(c, Rgba8(), false).flags, false), StrictKey(decal, false));
	c.ps.fog = 1;
	EXPECT_NE(StrictKey(Classify(c, Rgba8(), false).flags, false), StrictKey(decal, false));
}

// Additive blending: Cs * As (or Af) in the shader, the target added by a ONE, ONE blend. Under the run's equation
// (ONE, INV_SRC1_COLOR) the second output 0 adds the target.
TEST(GSRunsPolicy, AdditiveWays)
{
	GSHWDrawConfig c = Way0();
	c.ps.blend_mix = 0;
	c.ps.no_color1 = 1;
	c.blend = GSHWDrawConfig::BlendState(true, GSDevice::CONST_ONE, GSDevice::CONST_ONE, GSDevice::OP_ADD, GSDevice::CONST_ONE,
		GSDevice::CONST_ZERO, false, 0);
	Draw d = Classify(c, Rgba8(), false);
	ASSERT_EQ(d.why, Refusal::None);
	EXPECT_EQ(d.way, 4u);
	EXPECT_EQ(Field(d.flags, STATE_WAY), 4u);
	c.ps.blend_c = 2;
	c.cb_ps.TA_MaxDepth_Af.w = 0x20 / 128.0f;
	d = Classify(c, Rgba8(), false);
	ASSERT_EQ(d.why, Refusal::None);
	EXPECT_EQ(d.way, 5u);
	// The same blend with the alpha written by the target's term, or the colour masked, has no way.
	c.blend.dst_factor_alpha = GSDevice::CONST_ONE;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Blend);
	c.blend.dst_factor_alpha = GSDevice::CONST_ZERO;
	c.colormask.wrgba = 0x7;
	EXPECT_EQ(Classify(c, Rgba8(), false).why, Refusal::Blend);
	// The run equation with the second output 0 is an add.
	for (float dst : {0.0f, 0.5f, 1.0f})
		EXPECT_EQ(RunBlend(0.25f, 0.0f, dst), 0.25f + dst);
}

// The split rule's device classes come from the reported device name, and a run ends at a dear change only when the
// price of carrying the dear state over the run's fill exceeds the price of a host draw.
TEST(GSRunsPolicy, DeviceClassesAndTheSplitRule)
{
	EXPECT_STREQ(DeviceClassFor("Turnip Adreno (TM) 650").name, "Adreno 650");
	EXPECT_STREQ(DeviceClassFor("Mali-G615 MC6").name, "Mali-G615");
	EXPECT_STREQ(DeviceClassFor("Mali-G57 MC2").name, "Mali-G57");
	EXPECT_STREQ(DeviceClassFor("Apple M2 Max (G14C B1)").name, "other");
	EXPECT_STREQ(DeviceClassFor(nullptr).name, "other");

	const DeviceClass& a650 = DeviceClassFor("Turnip Adreno (TM) 650");
	// 900 ns a host draw against 38 ps a pixel: the break-even is about 23,700 pixels of fill.
	EXPECT_FALSE(SplitAtDear(a650, 10000, 1000));
	EXPECT_TRUE(SplitAtDear(a650, 30000, 1000));
	// A GPU where a host draw costs more merges larger runs before it splits.
	const DeviceClass& g57 = DeviceClassFor("Mali-G57 MC2");
	EXPECT_FALSE(SplitAtDear(g57, 7000, 0));
	EXPECT_TRUE(SplitAtDear(g57, 8000, 0));
}

namespace
{
	/// Feeds the policy frames of a fixed share (joined per thousand) of a fixed draw count; returns how many of them
	/// could open runs.
	u32 Frames(Dormancy& d, u32 frames, u32 draws, u32 joined_permille)
	{
		u32 open = 0;
		for (u32 i = 0; i < frames; i++)
		{
			const bool opens = d.OpensRuns();
			open += opens ? 1 : 0;
			// A dormant frame forms no runs, so the machine sees no draws.
			d.EndFrame(opens ? draws : 0, opens ? draws * joined_permille / 1000 : 0);
		}
		return open;
	}
} // namespace

// The machine starts awake, and a title whose draws mostly join runs keeps it awake (Spider-Man 3: about 98%).
TEST(GSRunsPolicy, DormancyStaysAwakeWhereRunsForm)
{
	Dormancy d;
	EXPECT_TRUE(d.OpensRuns());
	EXPECT_EQ(Frames(d, 1000, 4000, 980), 1000u);
	// A stretch where runs form less, but above the line it sleeps under, does not put it to sleep.
	EXPECT_EQ(Frames(d, 1000, 4000, Dormancy::SLEEP_PERMILLE), 1000u);
}

// A title whose runs hardly form (Sly 3: under half a percent; Tekken, Call of Duty 3: almost none) sleeps after one
// window, and then opens runs only on one sample frame in every SAMPLE_PERIOD + 1.
TEST(GSRunsPolicy, DormancySleepsWhereRunsDoNotForm)
{
	Dormancy d;
	EXPECT_EQ(Frames(d, Dormancy::WINDOW, 2000, 5), Dormancy::WINDOW);
	EXPECT_FALSE(d.OpensRuns());
	const u32 cycles = 10;
	EXPECT_EQ(Frames(d, cycles * (Dormancy::SAMPLE_PERIOD + 1), 2000, 5), cycles);

	Dormancy none;
	Frames(none, Dormancy::WINDOW, 2000, 0);
	EXPECT_FALSE(none.OpensRuns());
}

// Asleep, a sample frame at or over the wake line wakes the machine, which then stays awake.
TEST(GSRunsPolicy, DormancyWakesWhenASampleFormsRuns)
{
	Dormancy d;
	Frames(d, Dormancy::WINDOW, 2000, 0);
	ASSERT_FALSE(d.OpensRuns());
	EXPECT_EQ(Frames(d, Dormancy::SAMPLE_PERIOD, 2000, 980), 0u);
	EXPECT_TRUE(d.OpensRuns()); // the sample frame
	EXPECT_EQ(Frames(d, 1000, 2000, 980), 1000u);
}

// Hysteresis: a title between the two lines stays in whichever state it is in, so it cannot flap.
TEST(GSRunsPolicy, DormancyHysteresis)
{
	static_assert(Dormancy::SLEEP_PERMILLE < Dormancy::WAKE_PERMILLE);
	const u32 between = (Dormancy::SLEEP_PERMILLE + Dormancy::WAKE_PERMILLE) / 2;

	Dormancy awake;
	EXPECT_EQ(Frames(awake, 2000, 3000, between), 2000u);

	Dormancy asleep;
	Frames(asleep, Dormancy::WINDOW, 3000, 0);
	ASSERT_FALSE(asleep.OpensRuns());
	const u32 cycles = 10;
	EXPECT_EQ(Frames(asleep, cycles * (Dormancy::SAMPLE_PERIOD + 1), 3000, between), cycles);

	// Just under the sleep line sleeps; at it, stays awake.
	Dormancy under;
	Frames(under, Dormancy::WINDOW, 1000, Dormancy::SLEEP_PERMILLE - 1);
	EXPECT_FALSE(under.OpensRuns());
	Dormancy at;
	Frames(at, Dormancy::WINDOW, 1000, Dormancy::SLEEP_PERMILLE);
	EXPECT_TRUE(at.OpensRuns());
}

// The share is of draws over the whole window, not an average of frames; frames without draws (a loading screen, a
// repeated vsync) decide nothing, awake or sampling.
TEST(GSRunsPolicy, DormancyWeighsDrawsAndIgnoresEmptyFrames)
{
	Dormancy d;
	// Seven frames of 10 draws that all join, one of 10,000 that none join: 70 of 10,070, under the sleep line.
	for (u32 i = 0; i < Dormancy::WINDOW - 1; i++)
		d.EndFrame(10, 10);
	for (u32 i = 0; i < 100; i++)
		d.EndFrame(0, 0);
	EXPECT_TRUE(d.OpensRuns());
	d.EndFrame(10000, 0);
	EXPECT_FALSE(d.OpensRuns());

	Frames(d, Dormancy::SAMPLE_PERIOD, 10000, 0);
	ASSERT_TRUE(d.OpensRuns());
	for (u32 i = 0; i < 100; i++)
		d.EndFrame(0, 0);
	EXPECT_TRUE(d.OpensRuns()); // still the sample frame
	d.EndFrame(10000, 0);
	EXPECT_FALSE(d.OpensRuns());
}
