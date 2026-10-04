// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the two-pass road for (Cd - Cs) * As and (Cs - Cd) * As with As above 1.0 on a device that
// reads the destination through a copy of the render target (GSRendererHW::EmulateBlending).
//
// The fixed-function blend unit clamps a factor to 1.0, so a draw whose alpha runs above 128 was
// sent to software blending, which on that road costs a render-target copy and a render pass break
// per draw. Black has 1,376 of them per frame. The road replaces the copy with two hardware passes
// over the same geometry:
//   pass 1  the blend mix the draw takes where As <= 1: the shader outputs Cs * min(As, 1) and the
//           unit subtracts it from Cd * min(As, 1), so the result is min(As, 1) * (Cd - Cs);
//   pass 2  Cd1 * max(0, As - 1) + Cd1, which rescales what pass 1 left where As > 1. It runs
//           pass 1's shader with the software-blend bits cleared (BlendMultiPass::ApplyTo).
// These tests check the configuration the backend is handed: what the two passes are, which draws
// get them, and that every guard sends the draw back to the software blend it had before.
//
// Rides gs_vertex_tests through gs_hw_draw_harness.h, the renderer on the deviceless None backend.

#include "gs_hw_draw_harness.h"

using namespace GSHWDrawHarness;

namespace
{
	constexpr u32 kFBP = 0x40;

	struct Draw
	{
		u32 a = 1; // PS2 ALPHA: A = Cd
		u32 b = 0; //            B = Cs
		u32 c = 0; //            C = As
		u32 d = 2; //            D = 0
		u8 alpha_min = 0x40;
		u8 alpha_max = 0xE0;
		u32 fpsm = PSMCT32;
		u32 ztst = ZTST_ALWAYS;
		bool zwe = false;
		u32 clamp = 1;
		bool pabe = false;
		bool dthe = false;
		bool alpha_test_keep = false;
		bool alpha_test_split = false;
	};

	class GSOverOneTwoPass : public Fixture
	{
	protected:
		void SetUp() override
		{
			Fixture::SetUp();
			GSConfig.AccurateBlendingUnit = AccBlendLevel::Basic;
		}

		/// The render-target-copy road: no texture barrier, no framebuffer fetch, no per-primitive
		/// copy. Adreno under Turnip lands here.
		void CopyRoad()
		{
			GSDevice::FeatureSupport& f = m_device->MutableFeatures();
			f.texture_barrier = false;
			f.multidraw_fb_copy = false;
			f.framebuffer_fetch = false;
			f.dual_source_blend = true;
		}

		/// A four-triangle strip with alpha alternating between the two bounds. The cheap overlap test
		/// takes one quad's worth of triangles for non-overlapping and anything longer for
		/// PRIM_OVERLAP_UNKNOWN, which is what Black's strips are. The alpha range is not a single
		/// value, so As stays per fragment rather than turning into a fixed factor.
		void Strip(const Draw& dr)
		{
			Packet p;
			Environment(p, kFBP, dr.fpsm);

			GIFReg r = {};
			r.ZBUF.ZBP = 0x100;
			r.ZBUF.PSM = PSMZ32;
			r.ZBUF.ZMSK = dr.zwe ? 0 : 1;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);

			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = dr.ztst;
			if (dr.alpha_test_keep)
			{
				r.TEST.ATE = 1;
				r.TEST.ATST = ATST_GEQUAL;
				r.TEST.AREF = 0x80;
				r.TEST.AFAIL = AFAIL_KEEP;
			}
			else if (dr.alpha_test_split)
			{
				r.TEST.ATE = 1;
				r.TEST.ATST = ATST_GEQUAL;
				r.TEST.AREF = 0x80;
				r.TEST.AFAIL = AFAIL_RGB_ONLY;
			}
			p.Reg(GIF_A_D_REG_TEST_1, r);

			r.U64 = 0;
			r.ALPHA.A = dr.a;
			r.ALPHA.B = dr.b;
			r.ALPHA.C = dr.c;
			r.ALPHA.D = dr.d;
			p.Reg(GIF_A_D_REG_ALPHA_1, r);

			r.U64 = 0;
			r.COLCLAMP.CLAMP = dr.clamp;
			p.Reg(GIF_A_D_REG_COLCLAMP, r);

			r.U64 = 0;
			r.PABE.PABE = dr.pabe;
			p.Reg(GIF_A_D_REG_PABE, r);

			if (dr.dthe)
			{
				r.U64 = 0;
				r.DTHE.DTHE = 1;
				p.Reg(GIF_A_D_REG_DTHE, r);
			}

			const u8 lo = dr.alpha_min, hi = dr.alpha_max;
			for (int i = 0; i < 6; i++)
				p.VertexRGBA((i / 2) * 32 * 16, (i & 1) * 64 * 16, 1, 0, 0, 0x40, 0x60, 0x20, (i & 1) ? hi : lo);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_TRIANGLESTRIP;
			prim.IIP = 1;
			prim.ABE = 1;
			const u32 before = m_device->m_draws;
			p.Send(*m_gs, prim);
			ASSERT_EQ(m_device->m_draws, before + 1) << "the draw never reached the backend";
		}

		void ExpectTwoPass(GSDevice::BlendOp pass1_op)
		{
			const GSHWDrawConfig::BlendState& b1 = m_device->m_blend;
			EXPECT_TRUE(b1.enable);
			// Pass 1: the blend mix. The shader has multiplied Cs by the clamped As already, so the
			// source factor is ONE, and the destination factor is the second output, which the
			// attachment clamps to 1.0. Alpha is written here like any hardware blend writes it.
			EXPECT_EQ(b1.src_factor, GSDevice::CONST_ONE);
			EXPECT_EQ(b1.dst_factor, GSDevice::SRC1_COLOR);
			EXPECT_EQ(b1.op, pass1_op);
			EXPECT_EQ(b1.src_factor_alpha, GSDevice::CONST_ONE);
			EXPECT_EQ(b1.dst_factor_alpha, GSDevice::CONST_ZERO);
			EXPECT_FALSE(b1.constant_enable);

			// A = Cs, B = 0, C = As, D = 0, mixed: 2 where the unit reverse-subtracts, 1 where it
			// subtracts. blend_hw stays 0 so the shader writes Cs * min(As, 1) and As as they are.
			EXPECT_EQ(m_device->m_ps.blend_mix, pass1_op == GSDevice::OP_REV_SUBTRACT ? 2u : 1u);
			EXPECT_EQ(m_device->m_ps.blend_a, 0u);
			EXPECT_EQ(m_device->m_ps.blend_b, 2u);
			EXPECT_EQ(m_device->m_ps.blend_c, 0u);
			EXPECT_EQ(m_device->m_ps.blend_d, 2u);
			EXPECT_EQ(m_device->m_ps.blend_hw, 0u);
			EXPECT_FALSE(m_device->m_ps.no_color1) << "pass 1's destination factor reads the second output";

			// No destination read: this is what the road exists to avoid.
			EXPECT_FALSE(m_device->m_require_one_barrier);
			EXPECT_FALSE(m_device->m_require_full_barrier);

			// Pass 2: Cd1 * max(0, As - 1) + Cd1. The shader writes max(0, As - 1) as the colour
			// (blend_hw type 2), DST_COLOR scales Cd1 by it, CONST_ONE adds Cd1 back. It keeps the
			// destination's alpha and has no use for the second output.
			const GSHWDrawConfig::BlendMultiPass& mp = m_device->m_blend_multi_pass;
			ASSERT_TRUE(mp.enable);
			EXPECT_TRUE(mp.blend.enable);
			EXPECT_EQ(mp.blend_hw, static_cast<u8>(HWBlendType::SRC_ALPHA_DST_FACTOR));
			EXPECT_EQ(mp.blend.src_factor, GSDevice::DST_COLOR);
			EXPECT_EQ(mp.blend.dst_factor, GSDevice::CONST_ONE);
			EXPECT_EQ(mp.blend.op, GSDevice::OP_ADD);
			EXPECT_EQ(mp.blend.src_factor_alpha, GSDevice::CONST_ZERO);
			EXPECT_EQ(mp.blend.dst_factor_alpha, GSDevice::CONST_ONE);
			EXPECT_FALSE(mp.blend.constant_enable);
			EXPECT_TRUE(mp.no_color1);
			EXPECT_EQ(mp.dither, 0u) << "dither would be added to the multiplier";
			EXPECT_TRUE(mp.clear_sw_blend) << "type 2 is only the multiplier with the software-blend bits clear";

			// The shader pass 2 runs is a different one from pass 1's, and differs in exactly the
			// fields ApplyTo sets and nothing else.
			GSHWDrawConfig::PSSelector ps2 = m_device->m_ps;
			mp.ApplyTo(ps2);
			EXPECT_EQ(ps2.blend_a, 0u);
			EXPECT_EQ(ps2.blend_b, 0u);
			EXPECT_EQ(ps2.blend_d, 0u);
			EXPECT_EQ(ps2.blend_mix, 0u);
			EXPECT_EQ(ps2.blend_hw, static_cast<u32>(HWBlendType::SRC_ALPHA_DST_FACTOR));
			EXPECT_TRUE(ps2.no_color1);
			GSHWDrawConfig::PSSelector same1 = m_device->m_ps, same2 = ps2;
			for (GSHWDrawConfig::PSSelector* p : {&same1, &same2})
			{
				p->blend_a = p->blend_b = p->blend_d = p->blend_mix = 0;
				p->blend_hw = 0;
				p->no_color1 = 0;
				p->dither = 0;
			}
			EXPECT_EQ(same1.key_lo, same2.key_lo);
			EXPECT_EQ(same1.key_hi, same2.key_hi);
			EXPECT_TRUE(ps2.key_lo != m_device->m_ps.key_lo || ps2.key_hi != m_device->m_ps.key_hi)
				<< "pass 2 would reuse pass 1's shader and pipeline";
		}

		/// The draw the road left alone: software blending, which on this road is one destination
		/// copy, and no second pass.
		void ExpectPromoted()
		{
			EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
			EXPECT_FALSE(m_device->m_blend.enable);
			EXPECT_TRUE(m_device->m_require_one_barrier);
		}
	};
} // namespace

TEST_F(GSOverOneTwoPass, CdMinusCsTakesTwoPassesOnTheCopyRoad)
{
	BringUp();
	CopyRoad();
	Strip({});
	ExpectTwoPass(GSDevice::OP_REV_SUBTRACT);
}

TEST_F(GSOverOneTwoPass, CsMinusCdTakesTwoPassesWithTheOtherOperation)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.a = 0;
	dr.b = 1;
	Strip(dr);
	ExpectTwoPass(GSDevice::OP_SUBTRACT);
}

// A draw whose alpha never passes 1.0 never needed the promotion, and takes the hardware blend mix
// it always took.
TEST_F(GSOverOneTwoPass, AlphaAtOrBelowOneIsNotTouched)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.alpha_max = 0x80;
	Strip(dr);
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
	EXPECT_TRUE(m_device->m_blend.enable);
	EXPECT_EQ(m_device->m_ps.blend_mix, 2u);
	EXPECT_FALSE(m_device->m_require_one_barrier);
}

// A flat alpha becomes the fixed factor Af, which this road does not cover.
TEST_F(GSOverOneTwoPass, AFlatAlphaIsNotTheRoadsShape)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.alpha_min = dr.alpha_max = 0xE0;
	Strip(dr);
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

// Every device that can read the destination without a copy keeps what it had.
TEST_F(GSOverOneTwoPass, TheTextureBarrierRoadIsNotTouched)
{
	BringUp();
	GSDevice::FeatureSupport& f = m_device->MutableFeatures();
	f.texture_barrier = true;
	f.multidraw_fb_copy = false;
	f.framebuffer_fetch = false;
	Strip({});
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
	EXPECT_FALSE(m_device->m_blend.enable);
}

TEST_F(GSOverOneTwoPass, ThePerPrimitiveCopyRoadIsNotTouched)
{
	BringUp();
	GSDevice::FeatureSupport& f = m_device->MutableFeatures();
	f.texture_barrier = false;
	f.multidraw_fb_copy = true;
	f.framebuffer_fetch = false;
	Strip({});
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
	EXPECT_FALSE(m_device->m_blend.enable);
}

TEST_F(GSOverOneTwoPass, FramebufferFetchIsNotTouched)
{
	BringUp();
	GSDevice::FeatureSupport& f = m_device->MutableFeatures();
	f.texture_barrier = false;
	f.multidraw_fb_copy = false;
	f.framebuffer_fetch = true;
	Strip({});
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

// Pass 1 needs the second colour output.
TEST_F(GSOverOneTwoPass, NoDualSourceBlendMeansNoRoad)
{
	BringUp();
	CopyRoad();
	m_device->MutableFeatures().dual_source_blend = false;
	Strip({});
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

// Metal does not run GSHWDrawConfig::blend_multi_pass.
TEST_F(GSOverOneTwoPass, MetalDoesNotRunTheSecondPass)
{
	m_device_api = RenderAPI::Metal;
	BringUp();
	CopyRoad();
	Strip({});
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

// Minimum never promoted these draws, so there is nothing for the road to replace.
TEST_F(GSOverOneTwoPass, MinimumBlendingKeepsTheHardwareMix)
{
	GSConfig.AccurateBlendingUnit = AccBlendLevel::Minimum;
	BringUp();
	CopyRoad();
	Strip({});
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
	EXPECT_TRUE(m_device->m_blend.enable);
}

// Each of these puts a shader-side effect on the colour, or a second run of the same fragment, that
// pass 2 would apply to its multiplier or get wrong. The draw keeps its software blend.
TEST_F(GSOverOneTwoPass, ColourClampOffKeepsThePromotion)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.clamp = 0;
	Strip(dr);
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

TEST_F(GSOverOneTwoPass, PerPixelAlphaBlendKeepsThePromotion)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.pabe = true;
	Strip(dr);
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

TEST_F(GSOverOneTwoPass, ADitheredSixteenBitTargetKeepsThePromotion)
{
	GSConfig.Dithering = 1;
	BringUp();
	CopyRoad();
	Draw dr;
	dr.fpsm = PSMCT16;
	dr.dthe = true;
	Strip(dr);
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

TEST_F(GSOverOneTwoPass, ASixteenBitTargetKeepsThePromotion)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.fpsm = PSMCT16;
	Strip(dr);
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

TEST_F(GSOverOneTwoPass, ForcedDitherKeepsThePromotion)
{
	GSConfig.Dithering = 3;
	BringUp();
	CopyRoad();
	Strip({});
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

// An alpha test that discards the fragment (AFAIL_KEEP) discards it in both passes. One that keeps
// part of it runs a second pass of its own, which this road does not combine with.
TEST_F(GSOverOneTwoPass, AnAlphaTestThatDiscardsStillTakesTheRoad)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.alpha_test_keep = true;
	Strip(dr);
	ASSERT_EQ(m_device->m_alpha_test, GSHWDrawConfig::AlphaTestMode::KEEP);
	ExpectTwoPass(GSDevice::OP_REV_SUBTRACT);
}

TEST_F(GSOverOneTwoPass, AnAlphaTestWithASecondPassKeepsThePromotion)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.alpha_test_split = true;
	Strip(dr);
	ASSERT_NE(m_device->m_alpha_test, GSHWDrawConfig::AlphaTestMode::NONE);
	ASSERT_NE(m_device->m_alpha_test, GSHWDrawConfig::AlphaTestMode::KEEP);
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

// Pass 2 inherits the depth state. With depth writes off it tests against the buffer pass 1 saw,
// whatever the test. With them on, GEQUAL still passes against the fragment's own z and ALWAYS has
// no test, but GREATER fails against it.
TEST_F(GSOverOneTwoPass, DepthTestWithoutDepthWriteTakesTheRoad)
{
	BringUp();
	CopyRoad();
	for (const u32 ztst : {ZTST_GEQUAL, ZTST_GREATER})
	{
		Draw dr;
		dr.ztst = ztst;
		dr.zwe = false;
		Strip(dr);
		ExpectTwoPass(GSDevice::OP_REV_SUBTRACT);
		EXPECT_FALSE(m_device->m_depth.zwe);
	}
}

TEST_F(GSOverOneTwoPass, GreaterEqualWithDepthWriteTakesTheRoad)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.ztst = ZTST_GEQUAL;
	dr.zwe = true;
	Strip(dr);
	ExpectTwoPass(GSDevice::OP_REV_SUBTRACT);
	EXPECT_TRUE(m_device->m_depth.zwe);
}

TEST_F(GSOverOneTwoPass, GreaterWithDepthWriteKeepsThePromotion)
{
	BringUp();
	CopyRoad();
	Draw dr;
	dr.ztst = ZTST_GREATER;
	dr.zwe = true;
	Strip(dr);
	EXPECT_FALSE(m_device->m_blend_multi_pass.enable);
}

// The existing multi-pass roads keep the first pass's software-blend bits: Cs*(Alpha + 1) style
// second passes read them. Only a pass that asks for them cleared gets them cleared.
TEST(GSBlendMultiPass, ApplyToLeavesTheSoftwareBlendBitsUnlessAskedTo)
{
	GSHWDrawConfig::PSSelector ps = {};
	ps.blend_a = 1;
	ps.blend_b = 2;
	ps.blend_c = 1;
	ps.blend_d = 2;
	ps.blend_mix = 2;
	ps.blend_hw = static_cast<u32>(HWBlendType::BMIX1_ALPHA_HIGH_ONE);
	ps.dither = 2;
	ps.no_color1 = 0;

	GSHWDrawConfig::BlendMultiPass mp = {};
	mp.enable = true;
	mp.blend_hw = static_cast<u8>(HWBlendType::SRC_ONE_DST_FACTOR);
	mp.no_color1 = true;
	mp.dither = 0;

	GSHWDrawConfig::PSSelector kept = ps;
	mp.ApplyTo(kept);
	EXPECT_EQ(kept.blend_a, 1u);
	EXPECT_EQ(kept.blend_b, 2u);
	EXPECT_EQ(kept.blend_c, 1u);
	EXPECT_EQ(kept.blend_d, 2u);
	EXPECT_EQ(kept.blend_mix, 2u);
	EXPECT_EQ(kept.blend_hw, static_cast<u32>(HWBlendType::SRC_ONE_DST_FACTOR));
	EXPECT_EQ(kept.dither, 0u);
	EXPECT_TRUE(kept.no_color1);

	mp.clear_sw_blend = true;
	GSHWDrawConfig::PSSelector cleared = ps;
	mp.ApplyTo(cleared);
	EXPECT_EQ(cleared.blend_a, 0u);
	EXPECT_EQ(cleared.blend_b, 0u);
	EXPECT_EQ(cleared.blend_d, 0u);
	EXPECT_EQ(cleared.blend_mix, 0u);
	// The blend factor selector C is not a software-blend bit: the shader still reads it to pick
	// As or Af for the multiplier.
	EXPECT_EQ(cleared.blend_c, 1u);
	EXPECT_TRUE(cleared.key_lo != kept.key_lo || cleared.key_hi != kept.key_hi);
}
