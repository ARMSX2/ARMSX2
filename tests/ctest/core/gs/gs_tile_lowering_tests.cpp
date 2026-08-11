// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins for the Tile draw-lowering predicate (GSTileDrawLowering.h): the M2 native
// envelope, the first-disqualifier reason, the whole-byte FBMSK exemptions, the SW
// z-write semantics (ZTE=0 disables the write too), and the byte-coverage claims.
// Pure header over PODs — no GSLocalMemory tables, no device.

#include "GS/Renderers/Tile/GSTileDrawLowering.h"

#include <gtest/gtest.h>

namespace
{
GSTileDrawInput BaseInput()
{
	GSTileDrawInput in = {};
	in.prim_class = GS_TRIANGLE_CLASS;
	in.FRAME.FBP = 0;
	in.FRAME.FBW = 10;
	in.FRAME.PSM = PSMCT32;
	in.FRAME.FBMSK = 0;
	in.ZBUF.ZBP = 0x100;
	in.ZBUF.PSM = PSMZ24;
	in.ZBUF.ZMSK = 0;
	in.TEST.ZTE = 1;
	in.TEST.ZTST = ZTST_GEQUAL;
	in.alpha_min = 0;
	in.alpha_max = 255;
	in.vs_expand = true;
	return in;
}
} // namespace

TEST(GSTileLowering, BaselineOpaqueTriangleIsNative)
{
	const GSTileDrawPlan p = gsTileLowerDraw(BaseInput());
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.reason, GSTileFloorReason::None);
	EXPECT_EQ(p.colormask, 0xF);
	EXPECT_EQ(p.fb_claims, kGSTilePlanesAll);
	EXPECT_TRUE(p.z_write);
	EXPECT_TRUE(p.z_test);
	EXPECT_EQ(p.ztst, ZTST_GEQUAL);
	EXPECT_EQ(p.z_claims, GSTilePlaneRGB | GSTilePlaneZ); // Z24 byte coverage
}

TEST(GSTileLowering, EachDisqualifierFloorsWithItsReason)
{
	struct Case
	{
		const char* name;
		void (*mutate)(GSTileDrawInput&);
		GSTileFloorReason reason;
	};
	static const Case cases[] = {
		{"points", [](GSTileDrawInput& in) { in.prim_class = GS_POINT_CLASS; }, GSTileFloorReason::PrimClass},
		{"lines", [](GSTileDrawInput& in) { in.prim_class = GS_LINE_CLASS; }, GSTileFloorReason::PrimClass},
		{"sprite without expand",
			[](GSTileDrawInput& in) {
				in.prim_class = GS_SPRITE_CLASS;
				in.vs_expand = false;
			},
			GSTileFloorReason::SpriteExpandUnavailable},
		{"textured", [](GSTileDrawInput& in) { in.tme = true; }, GSTileFloorReason::Textured},
		{"blend", [](GSTileDrawInput& in) { in.abe = true; }, GSTileFloorReason::Blend},
		{"aa1 triangle", [](GSTileDrawInput& in) { in.aa1 = true; }, GSTileFloorReason::CoverageAA1},
		{"fog", [](GSTileDrawInput& in) { in.fge = true; }, GSTileFloorReason::Fog},
		{"fba", [](GSTileDrawInput& in) { in.fba = true; }, GSTileFloorReason::Fba},
		{"scanmsk", [](GSTileDrawInput& in) { in.scanmsk = 2; }, GSTileFloorReason::ScanMask},
		{"dynamic alpha test",
			[](GSTileDrawInput& in) {
				in.TEST.ATE = 1;
				in.TEST.ATST = ATST_GEQUAL;
				in.TEST.AREF = 128; // straddles the [0,255] alpha range
			},
			GSTileFloorReason::AlphaTest},
		{"date on ct32", [](GSTileDrawInput& in) { in.TEST.DATE = 1; }, GSTileFloorReason::DateTest},
		{"ztst never",
			[](GSTileDrawInput& in) { in.TEST.ZTST = ZTST_NEVER; }, GSTileFloorReason::ZTestNever},
		{"partial byte mask",
			[](GSTileDrawInput& in) { in.FRAME.FBMSK = 0x00000001; }, GSTileFloorReason::FrameMaskPartial},
		{"nothing written",
			[](GSTileDrawInput& in) {
				in.FRAME.FBMSK = 0xFFFFFFFFu;
				in.ZBUF.ZMSK = 1;
			},
			GSTileFloorReason::NothingWritten},
		{"ct16 frame", [](GSTileDrawInput& in) { in.FRAME.PSM = PSMCT16; }, GSTileFloorReason::FramePsm},
		{"z32 depth", [](GSTileDrawInput& in) { in.ZBUF.PSM = PSMZ32; }, GSTileFloorReason::ZbufPsm},
		{"stride zero", [](GSTileDrawInput& in) { in.FRAME.FBW = 0; }, GSTileFloorReason::StrideZero},
	};

	for (const Case& c : cases)
	{
		GSTileDrawInput in = BaseInput();
		c.mutate(in);
		const GSTileDrawPlan p = gsTileLowerDraw(in);
		EXPECT_FALSE(p.native) << c.name;
		EXPECT_EQ(p.reason, c.reason) << c.name;
	}
}

TEST(GSTileLowering, AlwaysPassingTestsStayNative)
{
	// ATE with ATST ALWAYS is not a test; DATE on a 24-bit frame has no destination
	// alpha to test; AA1 on sprites has no effect.
	GSTileDrawInput in = BaseInput();
	in.TEST.ATE = 1;
	in.TEST.ATST = ATST_ALWAYS;
	EXPECT_TRUE(gsTileLowerDraw(in).native);

	in = BaseInput();
	in.FRAME.PSM = PSMCT24;
	in.TEST.DATE = 1;
	EXPECT_TRUE(gsTileLowerDraw(in).native);

	in = BaseInput();
	in.prim_class = GS_SPRITE_CLASS;
	in.aa1 = true;
	EXPECT_TRUE(gsTileLowerDraw(in).native);
}

TEST(GSTileLowering, ZteZeroDisablesWriteAndTest)
{
	// The SW scanline derivation: zm = ZMSK || !ZTE. With ZTE=0 the depth buffer is
	// neither tested nor written, whatever ZMSK and ZTST say — and the Z32 format
	// restriction becomes irrelevant because no ds is touched.
	GSTileDrawInput in = BaseInput();
	in.TEST.ZTE = 0;
	in.ZBUF.PSM = PSMZ32;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_FALSE(p.z_write);
	EXPECT_FALSE(p.z_test);
	EXPECT_EQ(p.ztst, ZTST_ALWAYS);
	EXPECT_EQ(p.z_claims, 0);
}

TEST(GSTileLowering, ZMaskedTestOnlyStillNeedsExactDepthFormat)
{
	GSTileDrawInput in = BaseInput();
	in.ZBUF.ZMSK = 1;
	in.ZBUF.PSM = PSMZ32;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_FALSE(p.native); // still tests against Z32 contents
	EXPECT_EQ(p.reason, GSTileFloorReason::ZbufPsm);

	in.ZBUF.PSM = PSMZ16;
	const GSTileDrawPlan p16 = gsTileLowerDraw(in);
	EXPECT_TRUE(p16.native);
	EXPECT_FALSE(p16.z_write);
	EXPECT_TRUE(p16.z_test);
}

TEST(GSTileLowering, WholeByteMaskMapsToColormaskAndClaims)
{
	GSTileDrawInput in = BaseInput();
	in.FRAME.FBMSK = 0xFF000000u; // alpha byte fully masked
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0x7);
	EXPECT_EQ(p.fb_claims, GSTilePlaneRGB | GSTilePlaneZ);

	in.FRAME.FBMSK = 0x00FFFFFFu; // RGB bytes fully masked
	const GSTileDrawPlan pa = gsTileLowerDraw(in);
	EXPECT_TRUE(pa.native);
	EXPECT_EQ(pa.colormask, 0x8);
	EXPECT_EQ(pa.fb_claims, kGSTilePlanesAlpha | GSTilePlaneZ);
}

TEST(GSTileLowering, Ct24HasNoAlphaChannel)
{
	GSTileDrawInput in = BaseInput();
	in.FRAME.PSM = PSMCT24;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0x7);
	EXPECT_EQ(p.fb_claims, GSTilePlaneRGB | GSTilePlaneZ);
}

TEST(GSTileLowering, DepthOnlyDrawIsNative)
{
	GSTileDrawInput in = BaseInput();
	in.FRAME.FBMSK = 0xFFFFFFFFu;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0);
	EXPECT_EQ(p.fb_claims, 0);
	EXPECT_TRUE(p.z_write);
	// A depth-only draw ignores the frame format entirely.
	in.FRAME.PSM = PSMCT16;
	EXPECT_TRUE(gsTileLowerDraw(in).native);
}

TEST(GSTileLowering, ProvablyPassingAlphaTestDisappears)
{
	GSTileDrawInput in = BaseInput();
	in.TEST.ATE = 1;
	in.TEST.ATST = ATST_GEQUAL;
	in.TEST.AREF = 100;
	in.alpha_min = 100;
	in.alpha_max = 128;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0xF);
	EXPECT_TRUE(p.z_write);
}

TEST(GSTileLowering, ProvablyFailingAlphaTestBecomesAfailMaskEdit)
{
	// The GS idiom: ATST=NEVER + AFAIL=FB_ONLY writes color without depth. The z
	// TEST must survive the transform — only the write is suppressed.
	GSTileDrawInput in = BaseInput();
	in.TEST.ATE = 1;
	in.TEST.ATST = ATST_NEVER;
	in.TEST.AFAIL = AFAIL_FB_ONLY;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0xF);
	EXPECT_FALSE(p.z_write);
	EXPECT_TRUE(p.z_test);
	EXPECT_EQ(p.z_claims, 0);

	in.TEST.AFAIL = AFAIL_ZB_ONLY;
	const GSTileDrawPlan pz = gsTileLowerDraw(in);
	EXPECT_TRUE(pz.native); // depth-only
	EXPECT_EQ(pz.colormask, 0);
	EXPECT_TRUE(pz.z_write);

	in.TEST.AFAIL = AFAIL_RGB_ONLY;
	const GSTileDrawPlan pr = gsTileLowerDraw(in);
	EXPECT_TRUE(pr.native);
	EXPECT_EQ(pr.colormask, 0x7);
	EXPECT_FALSE(pr.z_write);

	// RGB_ONLY degrades to FB_ONLY off 32-bit frames (GetAFAIL).
	in.FRAME.PSM = PSMCT24;
	const GSTileDrawPlan p24 = gsTileLowerDraw(in);
	EXPECT_TRUE(p24.native);
	EXPECT_EQ(p24.colormask, 0x7); // CT24 has no alpha channel anyway
	EXPECT_FALSE(p24.z_write);

	in.FRAME.PSM = PSMCT32;
	in.TEST.AFAIL = AFAIL_KEEP;
	const GSTileDrawPlan pk = gsTileLowerDraw(in);
	EXPECT_FALSE(pk.native); // nothing written at all
	EXPECT_EQ(pk.reason, GSTileFloorReason::NothingWritten);

	// A provable fail through a range, not just NEVER.
	GSTileDrawInput ing = BaseInput();
	ing.TEST.ATE = 1;
	ing.TEST.ATST = ATST_GREATER;
	ing.TEST.AREF = 200;
	ing.TEST.AFAIL = AFAIL_FB_ONLY;
	ing.alpha_min = 128;
	ing.alpha_max = 128;
	const GSTileDrawPlan pg = gsTileLowerDraw(ing);
	EXPECT_TRUE(pg.native);
	EXPECT_FALSE(pg.z_write);
}

TEST(GSTileLowering, ZClaimsFollowByteCoverage)
{
	GSTileDrawInput in = BaseInput();
	in.ZBUF.PSM = PSMZ16;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.z_claims, kGSTilePlanesAll); // 16-bit cells cover every plane's bytes
}
