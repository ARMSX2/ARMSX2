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
	in.colclamp = true; // clamp mode — the common case; wrap is the M4a guardrail
	// The corpus's dominant blend, the alpha lerp (Cs−Cd)·As + Cd, with the full
	// 0..255 alpha range above making it genuinely dynamic. Inert while abe is
	// false; tests that flip abe on get a real blend, not the zeroed register's
	// (Cs−Cs)·As + Cs, which the M4b collapse rungs correctly eliminate.
	in.ALPHA.A = 0;
	in.ALPHA.B = 1;
	in.ALPHA.C = 0;
	in.ALPHA.D = 1;
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
		{"textured with mipmapping the GPU chain cannot express",
			[](GSTileDrawInput& in) {
				in.tme = true;
				in.tex_fst = true;
				in.tex_mip = true;
				in.tex_mip_fit = false; // e.g. MXL 6 on a 32x32 base
			},
			GSTileFloorReason::TextureMip},
		{"textured from the GPU24 layout",
			[](GSTileDrawInput& in) {
				in.tme = true;
				in.tex_fst = true;
				in.tex_psm = PSGPU24;
			},
			GSTileFloorReason::TexturePsm},
		{"blend", [](GSTileDrawInput& in) { in.abe = true; }, GSTileFloorReason::Blend},
		{"blend wrap",
			[](GSTileDrawInput& in) {
				in.abe = true;
				in.colclamp = false;
			},
			GSTileFloorReason::ColClip},
		{"aa1 triangle", [](GSTileDrawInput& in) { in.aa1 = true; }, GSTileFloorReason::CoverageAA1},
		{"fba on ct32", [](GSTileDrawInput& in) { in.fba = true; }, GSTileFloorReason::Fba},
		{"scanmsk", [](GSTileDrawInput& in) { in.scanmsk = 2; }, GSTileFloorReason::ScanMask},
		// Dither reaches CT32 and CT24 on silicon, so it reaches the whole native
		// envelope, and the native path writes no matrix. Before the SW renderer was
		// fixed to dither those formats the two arms agreed by both being wrong.
		{"dither", [](GSTileDrawInput& in) { in.dthe = true; }, GSTileFloorReason::Dither},
		// A dynamic test only floors when its pass split would reorder the draw's own
		// depth writes: failing fragments still write color here, so color and depth
		// land in different passes, and under GEQUAL with varying depth and possibly
		// overlapping primitives the order of those writes decides the outcome.
		{"dynamic alpha test whose split would reorder depth",
			[](GSTileDrawInput& in) {
				in.TEST.ATE = 1;
				in.TEST.ATST = ATST_GEQUAL;
				in.TEST.AREF = 128; // straddles the [0,255] alpha range
				in.TEST.AFAIL = AFAIL_FB_ONLY;
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
		{"z16 depth under a 32-bit frame",
			[](GSTileDrawInput& in) { in.ZBUF.PSM = PSMZ16; }, GSTileFloorReason::FrameZPairing},
		{"z16s depth under a 32-bit frame",
			[](GSTileDrawInput& in) { in.ZBUF.PSM = PSMZ16S; }, GSTileFloorReason::FrameZPairing},
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

// M3b: the texture gate is per-mechanism, not per-tme. Nearest sampling through the
// GPU sampler is console-exact (gs-texture capture: all 128 nearest cells right in
// every arm), so a nearest-sampled draw goes native in any rtx-served format —
// including palettised, whose palette and TEXA expansion the source builder applies
// CPU-side.
TEST(GSTileLowering, NearestSampledTexturingIsNative)
{
	static const u32 servable[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4,
		PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
	for (const u32 psm : servable)
	{
		GSTileDrawInput in = BaseInput();
		in.tme = true;
		in.tex_fst = true;
		in.tex_psm = static_cast<u8>(psm);
		const GSTileDrawPlan p = gsTileLowerDraw(in);
		EXPECT_TRUE(p.native) << "tex psm " << psm;
		EXPECT_EQ(p.reason, GSTileFloorReason::None) << "tex psm " << psm;
	}
}

// M3g slice 2: STQ sprites run the in-shader coordinate walk natively (the
// scanline's per-pixel trunc(s/q) into 16.16, GLSL divide tightened to IEEE
// rounding). Perspective TRIANGLES still floor — not for the coordinate walk
// (the gs-grad probe scores it 99.9% word-identical to SW), but for Z/coverage
// parity: OutRun's strips ladder overlapping sub-pixel triangles at consecutive
// Z, and interpolated-Z truncation ties resolve differently in the GPU float
// pipeline than in the scanline's double-precision DDA (~10.6k px/frame,
// invariant to the sampling mechanism). Until a Z-exact native path exists the
// floor stands, at either filter.
TEST(GSTileLowering, PerspectiveTrianglesFloorOnZCoverageParity)
{
	GSTileDrawInput in = BaseInput();
	in.tme = true;
	in.tex_fst = false;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_FALSE(p.native);
	EXPECT_EQ(p.reason, GSTileFloorReason::TexturePerspective);

	in.tex_linear = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TexturePerspective);

	in.tex_linear = false;
	in.prim_class = GS_SPRITE_CLASS;
	const GSTileDrawPlan sprite = gsTileLowerDraw(in);
	EXPECT_TRUE(sprite.native);
	EXPECT_EQ(sprite.reason, GSTileFloorReason::None);
}

TEST(GSTileLowering, StqGuardReadsTheTraceInTexels)
{
	// The regression this test exists for: GSVertexTrace finalises a !FST draw's
	// texture range ALREADY SCALED by (1 << TW, 1 << TH), so the guard must compare it
	// against 32766 as it stands. Scaling by the texture size a second time inflated
	// the hull by that factor and floored 11,631 of the 34,199 corpus draws — a third
	// of them — none of which the software rasterizer considers out of range at all.
	// A 1024-wide texture is the worst case for the confusion, so use one.
	constexpr u32 tw = 10, th = 10;
	const GSVector4 q_positive(0.0f, 0.0f, 1.0f, 1.0f);

	// An ordinary draw: 900 texels of a 1024-texel texture. Doubly scaled this reads
	// as 921,600 and floors.
	EXPECT_EQ(gsTileStqGuard(GSVector4(0.0f, 0.0f, 1.0f, 1.0f), GSVector4(900.0f, 900.0f, 1.0f, 1.0f), 0, tw, th),
		GSTileStqGuardNone);

	// Right up to the edge of the 1.15.16 range, still in.
	EXPECT_EQ(gsTileStqGuard(q_positive, GSVector4(32765.0f, 32765.0f, 1.0f, 1.0f), 0, tw, th),
		GSTileStqGuardNone);

	// And over it, per axis.
	EXPECT_EQ(gsTileStqGuard(q_positive, GSVector4(32766.0f, 10.0f, 1.0f, 1.0f), 0, tw, th),
		GSTileStqGuardHullU);
	EXPECT_EQ(gsTileStqGuard(GSVector4(-40000.0f, 0.0f, 1.0f, 1.0f), GSVector4(10.0f, 40000.0f, 1.0f, 1.0f), 0, tw, th),
		GSTileStqGuardHullU | GSTileStqGuardHullV);

	// Q must be provably one-signed: the vertex quotients only bound the interpolated
	// ones when no pole sits inside the primitive. Either sign alone is fine.
	EXPECT_EQ(gsTileStqGuard(GSVector4(0.0f, 0.0f, -4.0f, 1.0f), GSVector4(1.0f, 1.0f, -1.0f, 1.0f), 0, tw, th),
		GSTileStqGuardNone);
	EXPECT_EQ(gsTileStqGuard(GSVector4(0.0f, 0.0f, 0.0f, 1.0f), GSVector4(1.0f, 1.0f, 4.0f, 1.0f), 0, tw, th),
		GSTileStqGuardQPole);
	EXPECT_EQ(gsTileStqGuard(GSVector4(0.0f, 0.0f, -1.0f, 1.0f), GSVector4(1.0f, 1.0f, 1.0f, 1.0f), 0, tw, th),
		GSTileStqGuardQPole);

	// Any NaN bit from the trace, and a texture past the 1024-texel clamp.
	EXPECT_EQ(gsTileStqGuard(q_positive, GSVector4(1.0f, 1.0f, 1.0f, 1.0f), 8, tw, th), GSTileStqGuardNan);
	EXPECT_EQ(gsTileStqGuard(q_positive, GSVector4(1.0f, 1.0f, 1.0f, 1.0f), 0, 11, th), GSTileStqGuardTexSize);
	EXPECT_EQ(gsTileStqGuard(q_positive, GSVector4(1.0f, 1.0f, 1.0f, 1.0f), 0, tw, 11), GSTileStqGuardTexSize);
}

TEST(GSTileLowering, UnsafeStqCoordinatesFloor)
{
	// The range guard: coordinates the scanline's 16.16 format cannot hold (SW
	// rewrites or host-saturates those), or a Q whose sign the trace cannot pin —
	// the route computes the verdict from the vertex trace. Sprites divide per
	// pixel in the shader, so the guard floors them too; triangles attribute to
	// the perspective floor first; UV coordinates never consult it.
	GSTileDrawInput in = BaseInput();
	in.tme = true;
	in.tex_fst = false;
	in.tex_stq_guard = GSTileStqGuardHullU;
	in.prim_class = GS_SPRITE_CLASS;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TextureStqOverflow);

	in.tex_linear = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TextureStqOverflow);

	in.prim_class = GS_TRIANGLE_CLASS;
	const GSTileDrawPlan tri = gsTileLowerDraw(in);
	EXPECT_EQ(tri.reason, GSTileFloorReason::TexturePerspective);
	// The census mask survives being outranked — that is what makes the guard's
	// population measurable before the perspective floor lifts.
	EXPECT_EQ(tri.stq_guard, GSTileStqGuardHullU);

	in.prim_class = GS_SPRITE_CLASS;
	in.tex_fst = true;
	in.tex_linear = false;
	const GSTileDrawPlan uv = gsTileLowerDraw(in);
	EXPECT_TRUE(uv.native);
	EXPECT_EQ(uv.reason, GSTileFloorReason::None);

	// A mip STQ sprite divides per pixel exactly like a plain one, so the range
	// guard floors it the same way (the level shift happens after the divide).
	in.tex_fst = false;
	in.tex_mip = true;
	in.tex_mip_fit = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TextureStqOverflow);
}

// M3g slice 1: bilinear over affine coordinates goes native. The gs-texture capture
// measured the filter itself (1/16-texel truncating snap, 4-bit weight, two nested
// truncating lerps on the post-palette bytes), and the affine classes are the ones
// whose coordinate walk the GPU already reproduces (probe + corpus, M3b). The shader
// reproduces the SW scanline's integer filter — the GPU sampler stays nearest-only.
TEST(GSTileLowering, BilinearAffineSampledTexturingIsNative)
{
	// UV triangles.
	GSTileDrawInput in = BaseInput();
	in.tme = true;
	in.tex_fst = true;
	in.tex_linear = true;
	const GSTileDrawPlan tri = gsTileLowerDraw(in);
	EXPECT_TRUE(tri.native);
	EXPECT_EQ(tri.reason, GSTileFloorReason::None);

	// UV sprites — the census's 371-draw slice, all MMAG=1 sprites.
	in.prim_class = GS_SPRITE_CLASS;
	const GSTileDrawPlan uv_sprite = gsTileLowerDraw(in);
	EXPECT_TRUE(uv_sprite.native);
	EXPECT_EQ(uv_sprite.reason, GSTileFloorReason::None);

	// STQ sprites — the gs-texture probe's own filtered cells.
	in.tex_fst = false;
	const GSTileDrawPlan stq_sprite = gsTileLowerDraw(in);
	EXPECT_TRUE(stq_sprite.native);
	EXPECT_EQ(stq_sprite.reason, GSTileFloorReason::None);
}

TEST(GSTileLowering, TexturedDrawStillFloorsOnTheOtherGates)
{
	// A nearest texture passes the texture gates and then floors on whatever else
	// disqualifies — blend here, so the reason distribution keeps meaning "the first
	// mechanism we lack", not "it was textured".
	GSTileDrawInput in = BaseInput();
	in.tme = true;
	in.tex_fst = true;
	in.abe = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);

	// An inexpressible mip pyramid floors as mip work whatever the filter.
	in = BaseInput();
	in.tme = true;
	in.tex_fst = true;
	in.tex_mip = true;
	in.tex_mip_fit = false;
	in.tex_linear = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TextureMip);
}

// M3g mip slice: mip draws whose level geometry fits a GPU chain go native for
// the classes that already earned it — UV prims and STQ sprites. The fragment
// path reproduces the scanline JIT's LOD machinery (fused-fma log2 polynomial
// on Q, truncating convert, per-pixel level shift of coordinate and wrap
// bounds, per-level fetches, (diff·(lodf>>1))>>15 trilinear mix). STQ
// TRIANGLES keep flooring at the perspective gate: their blocker is Z/coverage
// parity, and a mip texture does not change it — every corpus TEX_MIP draw is
// such a triangle, so the ledger reattributes the class to TEX_PERSPECTIVE.
TEST(GSTileLowering, MipSampledTexturingIsNative)
{
	// UV triangles and sprites, both filters.
	GSTileDrawInput in = BaseInput();
	in.tme = true;
	in.tex_fst = true;
	in.tex_mip = true;
	in.tex_mip_fit = true;
	EXPECT_TRUE(gsTileLowerDraw(in).native);
	in.tex_linear = true;
	EXPECT_TRUE(gsTileLowerDraw(in).native);
	in.prim_class = GS_SPRITE_CLASS;
	EXPECT_TRUE(gsTileLowerDraw(in).native);

	// STQ sprites run the coordinate walk plus the per-pixel LOD formula.
	in.tex_fst = false;
	EXPECT_TRUE(gsTileLowerDraw(in).native);

	// STQ triangles attribute to the perspective floor, not the mip question.
	in.prim_class = GS_TRIANGLE_CLASS;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TexturePerspective);
}

TEST(GSTileLowering, AlwaysPassingTestsStayNative)
{
	// ATE with ATST ALWAYS is not a test; FBA on a 24-bit frame has no alpha byte to
	// set (console-measured, gs-test capture: no effect on any of its five separating
	// cases); AA1 on sprites has no effect.
	GSTileDrawInput in = BaseInput();
	in.TEST.ATE = 1;
	in.TEST.ATST = ATST_ALWAYS;
	EXPECT_TRUE(gsTileLowerDraw(in).native);

	in = BaseInput();
	in.FRAME.PSM = PSMCT24;
	in.fba = true;
	EXPECT_TRUE(gsTileLowerDraw(in).native);

	in = BaseInput();
	in.prim_class = GS_SPRITE_CLASS;
	in.aa1 = true;
	EXPECT_TRUE(gsTileLowerDraw(in).native);
}

TEST(GSTileLowering, DateOn24BitFrameFailsEverything)
{
	// Console-measured (gs-test capture): DATE on a 24-bit frame FAILS every pixel —
	// it does not pass-through as "no alpha to test", even when a real alpha byte is
	// physically present in memory — and a failing DATE stops the depth write too.
	// The whole draw is a provable no-op; GSRendererSW returns early the same way.
	GSTileDrawInput in = BaseInput();
	in.FRAME.PSM = PSMCT24;
	in.TEST.DATE = 1;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_FALSE(p.native);
	EXPECT_EQ(p.reason, GSTileFloorReason::NothingWritten);
	EXPECT_EQ(p.colormask, 0);
	EXPECT_FALSE(p.z_write);
	EXPECT_EQ(p.fb_claims, 0);
	EXPECT_EQ(p.z_claims, 0);
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

	// A masked-but-live test also still needs the hardware frame/Z pairing.
	in.ZBUF.PSM = PSMZ16;
	const GSTileDrawPlan p16 = gsTileLowerDraw(in);
	EXPECT_FALSE(p16.native);
	EXPECT_EQ(p16.reason, GSTileFloorReason::FrameZPairing);
	EXPECT_FALSE(p16.z_write);
	EXPECT_TRUE(p16.z_test);
}

TEST(GSTileLowering, FrameZPairingFollowsPixelSize)
{
	// Console-measured (gs-test capture): FRAME and ZBUF layouts pair by pixel size —
	// a 16-bit depth buffer under a 32-bit frame lands writes at coordinates neither
	// layout agrees on. The native path serves only measured pairings: with the M2
	// frames (32-bit words) that means Z24. A draw touching a single surface has no
	// pairing to violate: depth-only draws keep their 16-bit depth formats.
	GSTileDrawInput in = BaseInput();
	in.ZBUF.PSM = PSMZ16S;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_FALSE(p.native);
	EXPECT_EQ(p.reason, GSTileFloorReason::FrameZPairing);

	in.FRAME.FBMSK = 0xFFFFFFFFu; // depth-only: no colour write, no pairing
	const GSTileDrawPlan pd = gsTileLowerDraw(in);
	EXPECT_TRUE(pd.native);
	EXPECT_EQ(pd.colormask, 0);
	EXPECT_TRUE(pd.z_write);
}

TEST(GSTileLowering, WholeByteMaskMapsToColormaskAndClaims)
{
	GSTileDrawInput in = BaseInput();
	in.FRAME.FBMSK = 0xFF000000u; // alpha byte fully masked
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0x7);
	// The colormask says which channels the GPU writes; the claim says which planes the
	// surface then holds the newest bytes of. They are not the same set, and the claim is
	// the wider one: the Z claim rides along on byte coverage and spans the whole cell, so
	// the unwritten alpha byte -- which the upload gate required to be current before the
	// draw, and which the draw left alone -- is newest here too.
	EXPECT_EQ(p.fb_claims, kGSTilePlanesAll);

	in.FRAME.FBMSK = 0x00FFFFFFu; // RGB bytes fully masked
	const GSTileDrawPlan pa = gsTileLowerDraw(in);
	EXPECT_TRUE(pa.native);
	EXPECT_EQ(pa.colormask, 0x8);
	EXPECT_EQ(pa.fb_claims, kGSTilePlanesAll);
}

// The invariant the two above are instances of, stated on its own because violating it
// does not break a pixel -- it makes every later draw to the surface re-upload its
// footprint, and download it back first. Silent, and it cost 45x on GT4.
TEST(GSTileLowering, ColorClaimsCoverEveryPlaneTheySpan)
{
	for (const u32 psm : {static_cast<u32>(PSMCT32), static_cast<u32>(PSMCT24),
			 static_cast<u32>(PSMCT16), static_cast<u32>(PSMCT16S)})
	{
		for (const u32 fbmsk : {0x00000000u, 0xFF000000u, 0x00FFFFFFu, 0x0000FFFFu})
		{
			GSTileDrawInput in = BaseInput();
			in.FRAME.PSM = psm;
			in.FRAME.FBMSK = fbmsk;
			const GSTileDrawPlan p = gsTileLowerDraw(in);
			if (p.fb_claims == 0)
				continue; // writes no color at all: owns nothing, spans nothing
			EXPECT_EQ(p.fb_claims & gsTileColorPlanesSpannedBy(p.fb_claims),
				gsTileColorPlanesSpannedBy(p.fb_claims))
				<< "psm=" << psm << " fbmsk=" << fbmsk;
		}
	}
}

TEST(GSTileLowering, Ct24HasNoAlphaChannel)
{
	GSTileDrawInput in = BaseInput();
	in.FRAME.PSM = PSMCT24;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0x7);
	// No alpha CHANNEL to write, but the alpha BYTE is still part of the cell this
	// surface now holds the newest copy of -- a Z24 reading or a byte-3 palette view of
	// the same page reads it. Claim it, or every later draw re-uploads the page.
	EXPECT_EQ(p.fb_claims, kGSTilePlanesAll);
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

namespace
{
// A dynamic test: the alpha range straddles AREF, so neither outcome is provable.
GSTileDrawInput DynamicAlphaInput(u32 afail)
{
	GSTileDrawInput in = BaseInput();
	in.TEST.ATE = 1;
	in.TEST.ATST = ATST_GEQUAL;
	in.TEST.AREF = 128;
	in.TEST.AFAIL = afail;
	return in;
}
} // namespace

TEST(GSTileLowering, DynamicAlphaTestWritingNothingOnFailIsOnePassDiscard)
{
	// AFAIL=KEEP: the failing fragments write nothing, which is exactly what the
	// shader's discard does. No split, and the pass carries the comparison.
	const GSTileDrawPlan p = gsTileLowerDraw(DynamicAlphaInput(AFAIL_KEEP));
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass_count, 1);
	EXPECT_EQ(p.pass[0].colormask, 0xF);
	EXPECT_TRUE(p.pass[0].z_write);
	EXPECT_EQ(p.pass[0].atst, ATST_GEQUAL);
	EXPECT_EQ(p.pass[0].aref, 128);
	// The union still describes what a passing fragment writes.
	EXPECT_EQ(p.colormask, 0xF);
	EXPECT_TRUE(p.z_write);
}

TEST(GSTileLowering, DynamicAlphaTestThatEditsNothingDisappears)
{
	// AFAIL=FB_ONLY suppresses the depth write, but this draw does not write depth,
	// so passing and failing fragments write the same channels. There is no test.
	GSTileDrawInput in = DynamicAlphaInput(AFAIL_FB_ONLY);
	in.ZBUF.ZMSK = 1;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass_count, 1);
	EXPECT_EQ(p.pass[0].atst, ATST_ALWAYS);
	EXPECT_EQ(p.pass[0].colormask, 0xF);

	// The mirror: AFAIL=ZB_ONLY suppresses the color write on a depth-only draw.
	GSTileDrawInput iz = DynamicAlphaInput(AFAIL_ZB_ONLY);
	iz.FRAME.FBMSK = 0xFFFFFFFFu;
	const GSTileDrawPlan pz = gsTileLowerDraw(iz);
	ASSERT_TRUE(pz.native);
	EXPECT_EQ(pz.pass_count, 1);
	EXPECT_EQ(pz.pass[0].atst, ATST_ALWAYS);
	EXPECT_TRUE(pz.pass[0].z_write);
}

TEST(GSTileLowering, DynamicAlphaTestCollapsesToDiscardWhenTheEditRemovesEverything)
{
	// AFAIL=ZB_ONLY on a draw that writes no depth leaves the failing fragments with
	// nothing to write, so it is a discard rather than a split.
	GSTileDrawInput in = DynamicAlphaInput(AFAIL_ZB_ONLY);
	in.ZBUF.ZMSK = 1;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass_count, 1);
	EXPECT_EQ(p.pass[0].atst, ATST_GEQUAL);
	EXPECT_EQ(p.pass[0].colormask, 0xF);
	EXPECT_FALSE(p.pass[0].z_write);
}

TEST(GSTileLowering, FbOnlySplitsIntoColorThenDepth)
{
	// Every fragment writes color, only the passing ones write depth — so the first
	// pass runs untested and the second adds depth for the fragments that pass.
	// ZTST=ALWAYS makes the depth outcome independent of write order.
	GSTileDrawInput in = DynamicAlphaInput(AFAIL_FB_ONLY);
	in.TEST.ZTST = ZTST_ALWAYS;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	ASSERT_EQ(p.pass_count, 2);
	EXPECT_EQ(p.pass[0].colormask, 0xF);
	EXPECT_FALSE(p.pass[0].z_write);
	EXPECT_EQ(p.pass[0].atst, ATST_ALWAYS);
	EXPECT_EQ(p.pass[1].colormask, 0);
	EXPECT_TRUE(p.pass[1].z_write);
	EXPECT_EQ(p.pass[1].atst, ATST_GEQUAL);
	// The union is unchanged, so the memory-model claims are unaffected by the split.
	EXPECT_EQ(p.colormask, 0xF);
	EXPECT_TRUE(p.z_write);
}

TEST(GSTileLowering, RgbOnlySplitsByChannelNotByFragment)
{
	// RGB_ONLY: every fragment writes RGB, only the passing ones write A and depth.
	// Splitting by channel keeps RGB in one pass; splitting by fragment would put it
	// in both and composite overlapping primitives out of order.
	GSTileDrawInput in = DynamicAlphaInput(AFAIL_RGB_ONLY);
	in.TEST.ZTST = ZTST_ALWAYS;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	ASSERT_EQ(p.pass_count, 2);
	EXPECT_EQ(p.pass[0].colormask, 0x7);
	EXPECT_FALSE(p.pass[0].z_write);
	EXPECT_EQ(p.pass[0].atst, ATST_ALWAYS);
	EXPECT_EQ(p.pass[1].colormask, 0x8);
	EXPECT_TRUE(p.pass[1].z_write);
	EXPECT_EQ(p.pass[1].atst, ATST_GEQUAL);

	// Off a 32-bit frame RGB_ONLY degrades to FB_ONLY, and with no alpha channel to
	// withhold the split is the color-then-depth one.
	in.FRAME.PSM = PSMCT24;
	in.ZBUF.PSM = PSMZ24;
	const GSTileDrawPlan p24 = gsTileLowerDraw(in);
	ASSERT_TRUE(p24.native);
	ASSERT_EQ(p24.pass_count, 2);
	EXPECT_EQ(p24.pass[0].colormask, 0x7);
	EXPECT_EQ(p24.pass[1].colormask, 0);
	EXPECT_TRUE(p24.pass[1].z_write);
}

TEST(GSTileLowering, ZbOnlySplitsByFragmentWithTheInvertedTest)
{
	// ZB_ONLY is the one case a channel split cannot serve: depth written first is
	// what the color pass would then test against. So the passing fragments go
	// first with everything, and the failing ones follow with depth alone.
	GSTileDrawInput in = DynamicAlphaInput(AFAIL_ZB_ONLY);
	in.TEST.ZTST = ZTST_ALWAYS;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	ASSERT_EQ(p.pass_count, 2);
	EXPECT_EQ(p.pass[0].colormask, 0xF);
	EXPECT_TRUE(p.pass[0].z_write);
	EXPECT_EQ(p.pass[0].atst, ATST_GEQUAL);
	EXPECT_EQ(p.pass[1].colormask, 0);
	EXPECT_TRUE(p.pass[1].z_write);
	EXPECT_EQ(p.pass[1].atst, ATST_LESS); // the complement of GEQUAL
	EXPECT_EQ(p.pass[1].aref, 128);
}

TEST(GSTileLowering, ASplitNeedsTheDepthOutcomeToBeOrderIndependent)
{
	// GEQUAL with varying depth and primitives that may overlap: the scanline walks
	// fragments in primitive order and each depth write can change what the next
	// fragment's test sees, which no two-pass split reproduces.
	GSTileDrawInput in = DynamicAlphaInput(AFAIL_FB_ONLY);
	const GSTileDrawPlan floored = gsTileLowerDraw(in);
	EXPECT_FALSE(floored.native);
	EXPECT_EQ(floored.reason, GSTileFloorReason::AlphaTest);

	// One depth for the whole draw: rewriting a pixel with the depth it already
	// holds still passes GEQUAL, so the order cannot matter.
	GSTileDrawInput flat = in;
	flat.z_constant = true;
	EXPECT_TRUE(gsTileLowerDraw(flat).native);

	// Or the draw's own primitives never touch a pixel twice.
	GSTileDrawInput disjoint = in;
	disjoint.prim_overlap_none = true;
	EXPECT_TRUE(gsTileLowerDraw(disjoint).native);

	// GREATER is not GEQUAL: a second fragment at the same depth is rejected once
	// the first has written, so a flat draw does not rescue it.
	GSTileDrawInput greater = in;
	greater.TEST.ZTST = ZTST_GREATER;
	greater.z_constant = true;
	const GSTileDrawPlan pg = gsTileLowerDraw(greater);
	EXPECT_FALSE(pg.native);
	EXPECT_EQ(pg.reason, GSTileFloorReason::AlphaTest);
}

TEST(GSTileLowering, RgbOnlyWithoutAnAlphaWriteIsFbOnly)
{
	// FBMSK masks the alpha byte, so RGB_ONLY's "withhold alpha" edit is not an edit
	// and the draw takes the color-then-depth split.
	GSTileDrawInput in = DynamicAlphaInput(AFAIL_RGB_ONLY);
	in.TEST.ZTST = ZTST_ALWAYS;
	in.FRAME.FBMSK = 0xFF000000u;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	ASSERT_EQ(p.pass_count, 2);
	EXPECT_EQ(p.pass[0].colormask, 0x7);
	EXPECT_EQ(p.pass[1].colormask, 0);
	EXPECT_TRUE(p.pass[1].z_write);
}

TEST(GSTileLowering, ZClaimsFollowByteCoverage)
{
	// Depth-only, so the Z16 stays native (see FrameZPairingFollowsPixelSize).
	GSTileDrawInput in = BaseInput();
	in.FRAME.FBMSK = 0xFFFFFFFFu;
	in.ZBUF.PSM = PSMZ16;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.z_claims, kGSTilePlanesAll); // 16-bit cells cover every plane's bytes

	// The claim facts stay valid on the floored path too — the floor consults them.
	in.FRAME.FBMSK = 0;
	const GSTileDrawPlan pf = gsTileLowerDraw(in);
	EXPECT_FALSE(pf.native);
	EXPECT_EQ(pf.z_claims, kGSTilePlanesAll);
}

// The colclip trap (M4a): COLCLAMP=0 makes the blend output wrap at 8 bits, which
// fixed-function blending structurally cannot express (gs-blend: the wrap sections
// are Classic's entire residual, a 255-level boundary-flip signature no tolerance
// absorbs). The reason must outrank Blend so that when M4b lifts the read-free
// rungs, wrap draws keep flooring until the in-shader wrap realization lands (M4c).
// Wrap without blending has nothing to wrap: every pre-blend producer clamps to
// 8 bits (the texture function's clamp is console-measured, gs-shade), so an
// unblended COLCLAMP=0 draw stays native.
TEST(GSTileLowering, ColClipOutranksBlendAndRequiresBlending)
{
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.colclamp = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::ColClip);

	in.colclamp = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);

	in.abe = false;
	in.colclamp = false;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.reason, GSTileFloorReason::None);
}

// M4 contract rewrite #1: the pass carries its blend realization. On a native
// plan whose draw does not blend, every blend field must be inert — a pass that
// claims to blend, wrap or read without the machinery behind it would corrupt the
// memo key and the route's read gating silently. Both passes of a split are pinned.
TEST(GSTileLowering, BlendFieldsAreInertOnUnblendedPlans)
{
	GSTileDrawInput in = BaseInput();
	in.TEST.ATE = 1;
	in.TEST.ATST = ATST_GEQUAL;
	in.TEST.AREF = 128;
	in.TEST.AFAIL = AFAIL_RGB_ONLY; // channel split — two passes
	in.prim_overlap_none = true;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	ASSERT_EQ(p.pass_count, 2);
	for (u32 i = 0; i < p.pass_count; i++)
	{
		EXPECT_FALSE(p.pass[i].abe) << "pass " << i;
		EXPECT_FALSE(p.pass[i].colclip_wrap) << "pass " << i;
		EXPECT_FALSE(p.pass[i].rt_read) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_a, 0) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_b, 0) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_c, 0) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_d, 0) << "pass " << i;
		EXPECT_EQ(p.pass[i].afix, 0) << "pass " << i;
	}
}

// M4b rung 1: the exact whole-draw blend collapses. Cv = (((A−B)*C)>>7)+D, and
// three identities eliminate it per-pixel in the GS's integer arithmetic: A==B → D;
// C provably 0 → D; C provably 128 with B==D → A. A collapse to Cs writes the
// shaded source (blend gone); a collapse to Cd drops the RGB writes while alpha
// (never blended) writes through; a collapse to zero would need a fragment-output
// change, so it keeps flooring until rung 3 emits real blend state.
TEST(GSTileLowering, BlendCollapsesThatEliminateTheBlend)
{
	// The alpha lerp (Cs−Cd)·As + Cd with As provably 128 collapses to Cs.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 0;
	in.ALPHA.B = 1;
	in.ALPHA.C = 0;
	in.ALPHA.D = 1;
	in.alpha_min = 128;
	in.alpha_max = 128;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0xF);
	EXPECT_FALSE(p.pass[0].abe);

	// The same lerp with a genuinely variable alpha stays floored on Blend.
	in.alpha_min = 0;
	in.alpha_max = 255;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);

	// The reverse lerp (Cd−Cs)·As + Cs with As 128 collapses to Cd: RGB writes
	// nothing, alpha writes through, and the claims follow the narrowed mask.
	in.ALPHA.A = 1;
	in.ALPHA.B = 0;
	in.ALPHA.D = 0;
	in.alpha_min = 128;
	in.alpha_max = 128;
	p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0x8);
	EXPECT_EQ(p.pass[0].colormask, 0x8);

	// A==B collapses to D whatever C is: D=Cs is a full write...
	in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 1;
	in.ALPHA.B = 1;
	in.ALPHA.C = 0; // As, range still 0..255 — irrelevant under A==B
	in.ALPHA.D = 0;
	p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_EQ(p.colormask, 0xF);
	EXPECT_FALSE(p.pass[0].abe); // eliminated, not realized

	// ...and D=zero writes black, which no collapse can realize — rung 3 emits it
	// as the zero/zero factor pair instead (the difference is zero whatever C, so
	// the carrier bound does not apply and the wide alpha range is irrelevant).
	in.ALPHA.D = 2;
	p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);

	// C=FIX 0 collapses to D even with A != B.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 0;
	in.ALPHA.B = 2;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 0;
	in.ALPHA.D = 0;
	EXPECT_TRUE(gsTileLowerDraw(in).native);

	// C=FIX 128 with B==D collapses to A: Cs·1 + Cd·0 shapes write the source.
	in.ALPHA.FIX = 128;
	in.ALPHA.B = 1;
	in.ALPHA.D = 1;
	EXPECT_TRUE(gsTileLowerDraw(in).native);
	EXPECT_FALSE(gsTileLowerDraw(in).pass[0].abe); // eliminated, not realized

	// FIX between the identities does not collapse, and a fractional factor is
	// not exact under the ROP's round-to-nearest — it waits for the mix rung's
	// round-to-floor offsets.
	in.ALPHA.FIX = 64;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);
}

// M4b rung 3: the provably EXACT fixed-function rows. Written as Cv = Cs·s + Cd·d,
// a row is fixed-function iff both coefficients lie in {0, ±C, 1−C, 1} — and it is
// admitted only when the ROP has nothing to round: the GS truncates the blend
// product where fixed-function rounds to nearest, a half-level mean bias per blend
// that the corpus measured compounding under deep alpha overdraw into visible
// banding (MGS3 35.8% of a frame beyond two levels; SotC 23.6% with a clustered
// blob) while the single-blend gs-blend grid sat at 99.64% worst-1. Exactness
// means: C provably 128 (every factor exactly one or zero), A==B (difference
// zero), or a result provably zero. Fractional C waits for the mix rung's
// round-to-floor offsets — the donor's own realization for those rows.
TEST(GSTileLowering, RungThreeExactFixedFunctionRows)
{
	// The additive glow at full strength: Cs·As + Cd with the trace proving
	// As == 128 everywhere — every factor exactly one, the clamped integer sum on
	// both sides. Native, carried on the pass, and read-free.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.alpha_min = 128;
	in.alpha_max = 128;
	in.ALPHA.B = 2; // (Cs−0)·As + Cd
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);
	EXPECT_FALSE(p.pass[0].rt_read);
	EXPECT_FALSE(p.pass[0].colclip_wrap);
	EXPECT_EQ(p.pass[0].blend_a, 0);
	EXPECT_EQ(p.pass[0].blend_b, 2);
	EXPECT_EQ(p.pass[0].blend_c, 0);
	EXPECT_EQ(p.pass[0].blend_d, 1);

	// The same row through FIX at exactly 128 — the census's 61.9% class.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 128;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].afix, 128);

	// A fractional factor is the biased class — floors until the mix rung, from
	// either carrier.
	in.ALPHA.FIX = 100;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);
	in.ALPHA.C = 0;
	in.alpha_min = 0;
	in.alpha_max = 128;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);

	// The alpha lerp with As constant 128 collapses to Cs before rung 3 ever sees
	// it (rung 1, C==128 with B==D) — the realization stays eliminated.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 128;
	in.alpha_max = 128;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_FALSE(p.pass[0].abe);

	// Ad has no exact carrier: DST_ALPHA is 0..255 where the GS divides by 128.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2;
	in.ALPHA.C = 1;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);

	// D==A at C==128 is Cs·2 — a coefficient past one, accumulation territory.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 128;
	in.alpha_max = 128;
	in.ALPHA.B = 2;
	in.ALPHA.D = 0;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);

	// The always-zero results clamp to black in both arithmetics whatever C does:
	// (0−Cs)·As + 0, alpha range fully variable.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 2;
	in.ALPHA.B = 0;
	in.ALPHA.D = 2;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);

	// A split draw carries the realization on both passes — the channel split's
	// second pass writes colour too, and the two must agree. The blend rides the
	// FIX carrier so the alpha range can stay wide enough to keep the test
	// genuinely dynamic (a constant alpha would prove the test's outcome and
	// dissolve the split).
	in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 128;
	in.TEST.ATE = 1;
	in.TEST.ATST = ATST_GEQUAL;
	in.TEST.AREF = 64;
	in.TEST.AFAIL = AFAIL_RGB_ONLY;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	ASSERT_EQ(p.pass_count, 2);
	for (u32 i = 0; i < p.pass_count; i++)
	{
		EXPECT_TRUE(p.pass[i].abe) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_a, 0) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_b, 2) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_d, 1) << "pass " << i;
		EXPECT_FALSE(p.pass[i].rt_read) << "pass " << i;
	}
}

// The M4a guardrails outrank every collapse: a wrap draw floors ColClip and a
// PABE draw floors Blend even when the algebra would eliminate the blend — the
// collapse rungs are gated on clamp mode and no-PABE until M4c widens them.
TEST(GSTileLowering, CollapsesDeferToWrapAndPabeGuardrails)
{
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 1;
	in.ALPHA.B = 1; // A==B — collapsible on its own terms
	in.ALPHA.D = 0;

	in.colclamp = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::ColClip);

	in.colclamp = true;
	in.pabe = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);
}
