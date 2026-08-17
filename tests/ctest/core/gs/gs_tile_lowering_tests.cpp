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
	// A benign colour floor so mix-rung admissions in tests clear the saturation
	// guard; the guard's own tests zero it explicitly.
	in.color_min_rgb = 32;
	// prim_overlap_none stays FALSE here, and that is load-bearing for every blend
	// test below: since M4c the read rung takes any equation the read-free rungs
	// refuse, so the only thing left to floor such a row is the unproven overlap.
	// An expectation of BlendOverlap therefore reads "no read-free rung would take
	// this", which is exactly what those tests mean to pin — and the read rung's own
	// test flips the fact on to show the same rows going native.
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
		{"blend", [](GSTileDrawInput& in) { in.abe = true; }, GSTileFloorReason::BlendOverlap},
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

	// The unlock lever (EmuCore/GS/TilePerspectiveNative): with it the triangle
	// goes through to the STQ guard like a sprite does; the guard still floors.
	in.prim_class = GS_TRIANGLE_CLASS;
	in.tex_stq_tri_native = true;
	const GSTileDrawPlan lifted = gsTileLowerDraw(in);
	EXPECT_TRUE(lifted.native);
	EXPECT_EQ(lifted.reason, GSTileFloorReason::None);
	in.tex_stq_guard = GSTileStqGuardHullU;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TextureStqOverflow);
}

// The fast profile's admission (EmuCore/GS/TileFastShading): perspective
// triangles go native onto the SAMPLER leg — the same permission as the walk-era
// unlock to this pure function, differing only in which realization the route
// hands the draw to. The STQ guard still outranks it: an overflowing quotient is
// the software renderer's to rewrite whichever leg would sample it.
TEST(GSTileLowering, FastSampleAdmitsPerspectiveTriangles)
{
	GSTileDrawInput in = BaseInput();
	in.tme = true;
	in.tex_fst = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TexturePerspective);

	in.tex_fast_sample = true;
	const GSTileDrawPlan fast = gsTileLowerDraw(in);
	EXPECT_TRUE(fast.native);
	EXPECT_EQ(fast.reason, GSTileFloorReason::None);

	// A mipmapping STQ triangle floors under the sample admission alone and goes
	// native under the mip admission (the sampler leg's manual-LOD realization);
	// the walk-era unlock is the exact profile's separate road to the same draw.
	in.tex_mip = true;
	in.tex_mip_fit = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TextureMip);
	in.tex_fast_mip = true;
	EXPECT_TRUE(gsTileLowerDraw(in).native);
	// A pyramid deeper than its base floors whatever the profile: no GPU chain
	// expresses it.
	in.tex_mip_fit = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TextureMip);
	in.tex_mip_fit = true;
	in.tex_fast_mip = false;
	in.tex_stq_tri_native = true;
	EXPECT_TRUE(gsTileLowerDraw(in).native);
	in.tex_stq_tri_native = false;
	in.tex_mip = false;

	in.tex_stq_guard = GSTileStqGuardHullU;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::TextureStqOverflow);
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
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

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

TEST(GSTileLowering, FastProfileAdmitsTheOrderDependentSplit)
{
	// The fast profile takes the two-pass split without the order proof — the
	// reorder Classic ships unconditionally — so the largest single floor class
	// (R&C's foliage: dynamic test, differing fail mask, GEQUAL depth, varying z,
	// overlapping primitives) goes native with the same pass shapes the provably
	// safe split builds.
	GSTileDrawInput in = DynamicAlphaInput(AFAIL_FB_ONLY);
	ASSERT_FALSE(gsTileLowerDraw(in).native); // exact floors it (pinned above)

	in.fast_atst = true;
	const GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	ASSERT_EQ(p.pass_count, 2);
	// FB_ONLY: colour for every fragment, depth only for the passing ones.
	EXPECT_EQ(p.pass[0].atst, ATST_ALWAYS);
	EXPECT_FALSE(p.pass[0].z_write);
	EXPECT_EQ(p.pass[1].atst, ATST_GEQUAL);
	EXPECT_TRUE(p.pass[1].z_write);

	// The admission changes nothing the guard was not holding: a provable or
	// nothing-on-fail test keeps its cheaper realization under the flag too.
	GSTileDrawInput keep = DynamicAlphaInput(AFAIL_FB_ONLY);
	keep.FRAME.FBMSK = 0xFFFFFFFFu; // no colour write -> fail writes nothing
	keep.fast_atst = true;
	const GSTileDrawPlan pk = gsTileLowerDraw(keep);
	ASSERT_TRUE(pk.native);
	EXPECT_EQ(pk.pass_count, 1);
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
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

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
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

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
	// C cancels, so the pass carries the canonical constant encoding.
	in.ALPHA.D = 2;
	p = gsTileLowerDraw(in);
	EXPECT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].afix, 128);

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
	// not exact under the ROP's round-to-nearest — it lands on the mix rung,
	// whose round-to-floor offsets are what make the fraction shippable.
	in.ALPHA.FIX = 64;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);
	EXPECT_TRUE(p.pass[0].blend_mix);
	EXPECT_EQ(p.pass[0].afix, 64);
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
	// The register named As, but the admission proved it constant — the pass
	// carries the canonical constant encoding (C=FIX at 128) so the realization
	// never rides the blend map's dual-source factors. The value never needed
	// the second fragment output; the encoding must not either (the Mali blobs
	// report no dual-source blending — r44p1 measured — and an As encoding
	// would floor these rows there for no arithmetic reason).
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].afix, 128);
	EXPECT_EQ(p.pass[0].blend_d, 1);
	// Exact rows are exact: no mix arithmetic, no offsets.
	EXPECT_FALSE(p.pass[0].blend_mix);

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

	// A fractional factor on this row is the biased class, and the row's ROP
	// half is C-free (Cs·C + Cd) — the accumulation rung's territory, not the
	// mix rung's. Floors from either carrier until accu lands.
	in.ALPHA.FIX = 100;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	in.ALPHA.C = 0;
	in.alpha_min = 0;
	in.alpha_max = 128;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// The alpha lerp with As constant 128 collapses to Cs before rung 3 ever sees
	// it (rung 1, C==128 with B==D) — the realization stays eliminated.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 128;
	in.alpha_max = 128;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_FALSE(p.pass[0].abe);

	// Ad has no exact FIXED-FUNCTION carrier: DST_ALPHA is 0..255 where the GS
	// divides by 128. (The read rung has no such problem — it multiplies by the
	// byte and shifts — so what floors this row here is the overlap fact.)
	in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2;
	in.ALPHA.C = 1;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// D==A at C==128 is Cs·2 — a coefficient past one, accumulation territory.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 128;
	in.alpha_max = 128;
	in.ALPHA.B = 2;
	in.ALPHA.D = 0;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// The always-zero results clamp to black in both arithmetics whatever C does:
	// (0−Cs)·As + 0, alpha range fully variable. Zero at every C also means the
	// carrier is free to choose — and the As encoding of this row is dual-source
	// (the map's negated-source shape), so the pass takes the constant form.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 2;
	in.ALPHA.B = 0;
	in.ALPHA.D = 2;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].afix, 128);

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

// The mix rung (M4b, queue-jumped ahead of accu by the overdraw banding
// finding): the fractional-C rows the donor realizes as blend-mix — the shader
// computes the Cs-side term carrying the round-to-floor offset, the ROP adds
// the Cd term through the blend map's factor. Admission is the plain mix
// shapes only ((A,B) ∈ {(0,1),(1,0)} with D != A — D==A stacks the +1 into a
// C+1 coefficient, the donor's A_MAX class), and NARROWER than the donor on
// two corpus-measured grounds (FlatOut 2 / Dirge / OutRun first light): the
// carrier must be a PROVEN CONSTANT ≤ 128 (a variable As transits an 8-bit
// colour channel, quantizing the factor off the /128 grid — banding and
// palette-amplified speckle), and the shader term must provably clear the
// offset at every pixel (a term below it saturates at the blender's [0,1]
// input clamp, the offset dies, and half-integer destination products round
// up — the near-black particle drift). Under both proofs the realization is
// exact, not ±1. The pass lands the constant as FIX; the renderer owns the
// donor's selector rewrite.
TEST(GSTileLowering, MixRungFractionalRows)
{
	// The corpus's dominant blend shape through FIX below 128: the constant
	// rides the blend constant at full float precision.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 64;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);
	EXPECT_TRUE(p.pass[0].blend_mix);
	EXPECT_FALSE(p.pass[0].rt_read);
	EXPECT_FALSE(p.pass[0].colclip_wrap);
	EXPECT_EQ(p.pass[0].blend_a, 0);
	EXPECT_EQ(p.pass[0].blend_b, 1);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].blend_d, 1);
	EXPECT_EQ(p.pass[0].afix, 64);

	// A degenerate As below 128 is a proven constant: it lands as FIX at the
	// proven value — the #90 rule extended to this rung.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 64;
	in.alpha_max = 64;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].blend_mix);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].afix, 64);

	// A genuinely VARIABLE As is refused by this rung even when bounded: its only
	// read-free carriers are 8-bit colour channels (the SRC1 output, or the
	// primary output's alpha), and the /128→/255 requantization is an
	// up-to-half-level error scaled by Cd — measured integrating into OutRun's
	// banding and Dirge's palette-amplified max-108 speckle. Since M4c it falls
	// through to the read rung, which carries it exactly over a paid read; here
	// only the unproven overlap keeps it floored.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 0;
	in.alpha_max = 128;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// The other plain mix shapes, on the degenerate-constant carrier: (0,1,2)
	// and (1,0,2) hand the ROP a ±C·Cd term through the map's subtract ops;
	// (1,0,0) is the reverse lerp, the donor's MIX3.
	const u8 shapes[3][3] = {{0, 1, 2}, {1, 0, 2}, {1, 0, 0}};
	for (const u8* s : shapes)
	{
		in = BaseInput();
		in.abe = true;
		in.alpha_min = 64;
		in.alpha_max = 64;
		in.ALPHA.A = s[0];
		in.ALPHA.B = s[1];
		in.ALPHA.D = s[2];
		p = gsTileLowerDraw(in);
		ASSERT_TRUE(p.native) << "shape " << int(s[0]) << int(s[1]) << int(s[2]);
		EXPECT_TRUE(p.pass[0].blend_mix);
		EXPECT_EQ(p.pass[0].blend_a, s[0]);
		EXPECT_EQ(p.pass[0].blend_b, s[1]);
		EXPECT_EQ(p.pass[0].blend_d, s[2]);
		EXPECT_EQ(p.pass[0].blend_c, 2);
		EXPECT_EQ(p.pass[0].afix, 64);
	}

	// The saturation guard: a colour floor that cannot prove the shader term
	// clears the offset floors the draw — the near-black particle class whose
	// zero-saturated term turns half-integer destination products into a +1
	// per layer (FlatOut's measured drift).
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 64;
	in.color_min_rgb = 0;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	// color_min·C must reach 64 (the offset on the /128 product grid): 32·1 < 64.
	in.color_min_rgb = 32;
	in.ALPHA.FIX = 1;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	// The reverse lerp's term is Cs·(1−C): a large C is the hazard there.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 1;
	in.ALPHA.B = 0;
	in.ALPHA.D = 0;
	in.alpha_min = 127;
	in.alpha_max = 127;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	// Fog edits the colour below any vertex bound; a texture function is
	// unbounded below. Both refuse the proof.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 64;
	in.fge = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	in.fge = false;
	in.tme = true;
	in.tex_fst = true;
	in.tex_psm = PSMCT32;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// FIX above 128 floors (24 corpus draws; no rung is built FOR it).
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 200;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// D==A stacks the +1 onto the C-carrying operand — the A_MAX class floors.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 64;
	in.alpha_max = 64;
	in.ALPHA.D = 0; // (0,1,0): Cs·(As+1) − Cd·As
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	in.ALPHA.A = 1;
	in.ALPHA.B = 0;
	in.ALPHA.D = 1; // (1,0,1): Cd·(As+1) − Cs·As
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// Ad has no exact fixed-function carrier here either — the scale mismatch does
	// not care which read-free rung asks.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 1;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// Wrap and PABE outrank the rung, as they outrank every other.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 64;
	in.alpha_max = 64;
	in.colclamp = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::ColClip);
	in.colclamp = true;
	in.pabe = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);

	// A split draw carries the mix realization on both passes, like rung 3's
	// constant rows. FIX carrier so the alpha range keeps the test dynamic.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 64;
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
		EXPECT_TRUE(p.pass[i].blend_mix) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_c, 2) << "pass " << i;
		EXPECT_EQ(p.pass[i].afix, 64) << "pass " << i;
	}
}

// Rung 9, the read rung (M4c). Everything the read-free rungs refuse lands here
// and goes native with the register's OWN equation carried to the shader — no
// selector rewrite, no constant substitution, no carrier restriction — because
// the realization is integer arithmetic over a destination the pass reads, where
// As, Ad and FIX are equally exact and the /128 scale is a shift.
//
// The one fact that can still floor such a draw is its own primitive overlap:
// the realization reads a single state of the target, so a pixel blended into
// twice would take the pre-draw value the second time.
TEST(GSTileLowering, ReadRungTakesWhatTheReadFreeRungsRefuse)
{
	// The corpus's dominant row, and the one every read-free rung refuses: the
	// alpha lerp on a genuinely variable As.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.alpha_min = 0;
	in.alpha_max = 255;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	in.prim_overlap_none = true;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);
	EXPECT_TRUE(p.pass[0].rt_read);
	EXPECT_FALSE(p.pass[0].blend_mix);
	EXPECT_EQ(p.pass[0].blend_a, 0);
	EXPECT_EQ(p.pass[0].blend_b, 1);
	EXPECT_EQ(p.pass[0].blend_c, 0); // As stays As — no constant substitution
	EXPECT_EQ(p.pass[0].blend_d, 1);

	// Ad rides raw too. This is the row the scale mismatch keeps off the blend
	// unit forever, and the rung that makes it exact rather than approximate.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 1;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].rt_read);
	EXPECT_EQ(p.pass[0].blend_c, 1);

	// FIX above 128 — the class fixed-function cannot carry as a constant at all.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 200;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].rt_read);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].afix, 200);

	// The A_MAX shapes (D==A, coefficient C+1) that the mix rung refuses.
	for (const u8 d : {static_cast<u8>(0), static_cast<u8>(1)})
	{
		in = BaseInput();
		in.abe = true;
		in.alpha_min = 64;
		in.alpha_max = 64;
		in.ALPHA.A = d;
		in.ALPHA.B = static_cast<u32>(1 - d);
		in.ALPHA.D = d;
		in.prim_overlap_none = true;
		p = gsTileLowerDraw(in);
		ASSERT_TRUE(p.native) << "D==A shape " << int(d);
		EXPECT_TRUE(p.pass[0].rt_read) << "D==A shape " << int(d);
	}

	// A texture function or fog refuses the mix rung's saturation proof; the read
	// rung needs no such proof, because nothing saturates in integer arithmetic.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 64;
	in.color_min_rgb = 0;
	in.tme = true;
	in.tex_fst = true;
	in.tex_psm = PSMCT32;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].rt_read);

	// The read-free rungs still win where they apply: a proven-128 carrier stays
	// fixed-function even with overlap proven absent, and pays no read.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 128;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].abe);
	EXPECT_FALSE(p.pass[0].rt_read);

	// ...and so does the mix rung, on the row it was narrowed to.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 64;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].blend_mix);
	EXPECT_FALSE(p.pass[0].rt_read);

	// ⚠️ The measured envelope, and the reason it is narrower than the equation:
	// a TEXTURED draw with a PER-PIXEL factor renders wrong (M4c first light,
	// four corpus ablations). A constant factor on a textured draw is exact, and
	// a per-pixel factor on an untextured draw is exact; only the combination
	// fails, and it fails for both variable carriers.
	for (const u32 carrier : {0u, 1u})
	{
		in = BaseInput();
		in.abe = true;
		in.ALPHA.C = carrier;
		in.alpha_min = 0;
		in.alpha_max = 255;
		in.tme = true;
		in.tex_fst = true;
		in.tex_psm = PSMCT32;
		in.prim_overlap_none = true;
		EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendTexSample)
			<< "carrier " << carrier;
		// The same draw with a constant carrier is admitted — the discriminator is
		// the per-pixel factor, not the texture.
		in.ALPHA.C = 2;
		in.ALPHA.FIX = 100;
		p = gsTileLowerDraw(in);
		ASSERT_TRUE(p.native) << "carrier " << carrier;
		EXPECT_TRUE(p.pass[0].rt_read) << "carrier " << carrier;
	}

	// Wrap and PABE still outrank the rung — both are M4c slices of their own.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 0;
	in.alpha_max = 255;
	in.prim_overlap_none = true;
	in.colclamp = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::ColClip);
	in.colclamp = true;
	in.pabe = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);

	// A split draw carries the read realization on both passes. Only one pass of
	// any split writes RGB, which is what keeps the second pass's stale read
	// harmless — see the independent_rgb note in the lowering.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 0;
	in.alpha_max = 255;
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
		EXPECT_TRUE(p.pass[i].rt_read) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_c, 0) << "pass " << i;
	}
	EXPECT_EQ(p.pass[0].colormask, 0x7);
	EXPECT_EQ(p.pass[1].colormask, 0x8);
}

// The two blend floors' lift levers (EmuCore/GS/TileBlendOverlapNative /
// TileBlendTexSampleNative): dev attribution/perf levers for full-native-coverage
// runs, never a user setting. Both floors' NAMED causes are fixed (re-measured
// 2026-08-16); what keeps them shipped-on is the shared coverage-tie (#30) and
// cross-frame-handoff residues. The lever reproduces the scratch-lift the
// re-measure scored: admission to the read rung's one-barrier realization
// (ordering measured immaterial, #111).
TEST(GSTileLowering, BlendFloorLiftLevers)
{
	// The variable-As lerp with UNPROVEN overlap floors shipped; the overlap
	// lever admits it to the read rung.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.alpha_min = 0;
	in.alpha_max = 255;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	in.blend_overlap_native = true;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].rt_read);

	// The textured per-pixel-factor cell floors shipped even with overlap proven,
	// and the OVERLAP lever must not admit it — the floors are separate, with
	// separate residues, and the levers must bisect them separately.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 0;
	in.alpha_max = 255;
	in.tme = true;
	in.tex_fst = true;
	in.tex_psm = PSMCT32;
	in.prim_overlap_none = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendTexSample);
	in.blend_overlap_native = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendTexSample);
	in.blend_tex_sample_native = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].rt_read);

	// The cell needing both facts lifted: textured per-pixel factor, unproven
	// overlap. Both levers on admits it; dropping the overlap lever floors it on
	// the overlap again (first-reason order).
	in.prim_overlap_none = false;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_TRUE(p.pass[0].rt_read);
	in.blend_overlap_native = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
}

// The Classic-parity blend carrier (EmuCore/GS/TileBlendClassicCarrier, M4e/#135):
// serve the variable-carrier clamp-mode rows READ-FREE at Classic's own realization
// and accuracy class, instead of routing them to the read rung — whose Adreno
// realization is a copy plus a pass break PER DRAW, measured carrying the corpus's
// dominant blend row (the variable-As lerp, 76% of blended draws). The read-free
// legs need no overlap proof (the ROP composites fragments in primitive order), so
// the carrier bypasses the BlendOverlap/BlendTexSample floors for the rows it
// admits. Default off; the shipped path is unchanged.
TEST(GSTileLowering, ClassicCarrierServesTheVariableAsLerpReadFree)
{
	// The dominant row itself: 0101, genuinely variable As, textured, overlap
	// UNPROVEN — everything that floors it shipped. The carrier takes it to the
	// donor's blend-mix with the As factor through SRC1.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.tme = true;
	in.tex_fst = true;
	in.tex_psm = PSMCT32;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Mix);
	EXPECT_TRUE(p.pass[0].blend_mix);
	EXPECT_FALSE(p.pass[0].rt_read);
	EXPECT_TRUE(p.pass[0].blend_src1);
	// Raw selectors — the renderer owns the donor's rewrite, and the As carrier
	// stays As (no constant substitution exists to make).
	EXPECT_EQ(p.pass[0].blend_a, 0);
	EXPECT_EQ(p.pass[0].blend_b, 1);
	EXPECT_EQ(p.pass[0].blend_c, 0);
	EXPECT_EQ(p.pass[0].blend_d, 1);

	// Without the device's dual-source unit the SRC1 transit does not exist, and
	// the row keeps the read rung's path — flooring here on the unproven overlap.
	// (Classic software-blends the same rows on those devices: parity holds in
	// both directions.)
	in.dual_source_blend = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
}

TEST(GSTileLowering, ClassicCarrierAccumulationRowsNeedNoDualSource)
{
	// Cs·As + Cd (0201): the shader computes and truncates the whole term, the
	// ROP adds Cd at factor ONE — no SRC1 anywhere, so no dual-source gate.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2; // (Cs − 0)·As + Cd
	in.tme = true;
	in.tex_fst = true;
	in.tex_psm = PSMCT32;
	in.blend_classic_carrier = true;
	in.dual_source_blend = false;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Accumulation);
	EXPECT_FALSE(p.pass[0].blend_src1);
	EXPECT_FALSE(p.pass[0].rt_read);
	EXPECT_EQ(p.pass[0].blend_c, 0);

	// The FIX twin (0221) with a value the exact rungs refuse (not 128, and the
	// textured term defeats the mix rung's saturation proof) — previously the
	// read rung, now the same accumulation leg.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 64;
	in.tme = true;
	in.tex_fst = true;
	in.tex_psm = PSMCT32;
	in.prim_overlap_none = true;
	{
		GSTileDrawInput off = in;
		off.blend_classic_carrier = false;
		GSTileDrawPlan q = gsTileLowerDraw(off);
		ASSERT_TRUE(q.native);
		EXPECT_TRUE(q.pass[0].rt_read); // the shipped disposition this replaces
	}
	in.blend_classic_carrier = true;
	in.prim_overlap_none = false; // and overlap no longer matters
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Accumulation);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].afix, 64);

	// The reversed shapes (Cd − Cs·C, 2001/2021) ride the same leg: the renderer
	// computes the term positive and the ROP reverse-subtracts.
	in.ALPHA.A = 2;
	in.ALPHA.B = 0;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Accumulation);
}

TEST(GSTileLowering, ClassicCarrierNoRecRowsAreExactAndDeviceFree)
{
	// Cs·As standalone (0202): no Cd anywhere in the equation, so the whole thing
	// runs in the shader's integer arithmetic with the blend unit off — byte-exact,
	// overlap-immune, and admitted on any device. Shipped, this row paid the read
	// rung's ordering for an equation that reads nothing.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2;
	in.ALPHA.D = 2; // (Cs − 0)·As + 0
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	in.blend_classic_carrier = true;
	in.dual_source_blend = false;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::InShaderNoRec);
	EXPECT_FALSE(p.pass[0].blend_src1);
	EXPECT_FALSE(p.pass[0].rt_read);
	EXPECT_EQ(p.pass[0].blend_c, 0);
}

TEST(GSTileLowering, ClassicCarrierHw2RowsSplitTheFactorAcrossOutputs)
{
	// Cd·As (1202): the donor's BLEND_HW2 — the shader splits the 0..2 factor
	// across its two outputs and DST_COLOR factors compose the product. Needs
	// SRC1 for the variable carrier.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 1;
	in.ALPHA.B = 2;
	in.ALPHA.D = 2; // (Cd − 0)·As + 0
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::HwRewrite);
	EXPECT_EQ(p.pass[0].blend_hw, 2); // HWBlendType::SRC_ALPHA_DST_FACTOR
	EXPECT_TRUE(p.pass[0].blend_src1);

	// Without dual source the variable-As shape keeps the read rung's path.
	in.dual_source_blend = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);

	// The FIX twin (1222) rides the blend constant instead and needs nothing.
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 100;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::HwRewrite);
	EXPECT_FALSE(p.pass[0].blend_src1);
	EXPECT_EQ(p.pass[0].afix, 100);
}

TEST(GSTileLowering, ClassicCarrierRefusalsKeepTheReadRung)
{
	// Ad rows stay out entirely: DST_ALPHA divides by 255 where the GS divides by
	// 128 — a factor-of-two class, not a rounding one (the M4 §7 decision). The
	// read rung keeps them exact, and its floors still apply.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 1;
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::BlendOverlap);
	in.prim_overlap_none = true;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Read);
	EXPECT_TRUE(p.pass[0].rt_read);

	// Classic's Basic-level high-alpha rule carries over: a carrier that CAN
	// exceed 128 makes the mix saturate on both sides (measured 40-70 levels on
	// the gs-blend F>128 rows before this clause existed), so it is mixed only
	// where the draw's primitives may overlap — Classic's own saturating shipped
	// shape there, because one read cannot serve overlap — and takes the read
	// rung's exact arithmetic where overlap is proven absent, which is Classic's
	// exact-software-blend disposition for the same cell.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 0;
	in.alpha_max = 255; // 0101, As may exceed 128
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Read);
	in.prim_overlap_none = false;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Mix);

	// The FIX twin of the same rule: F above 128 reads where overlap is proven
	// absent, mixes (saturating, as Classic ships it) where it is not. A proven
	// bound at or below 128 mixes everywhere.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 192;
	in.ALPHA.D = 2; // 0122: (Cs − Cd)·F — the row the probe measured at 70 levels
	in.tme = true;
	in.tex_fst = true;
	in.tex_psm = PSMCT32;
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Read);
	in.prim_overlap_none = false;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Mix);
	in.ALPHA.FIX = 90;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Mix);

	// The A_MAX mix shapes (D==A stacks the +1 onto the C-carrying operand:
	// 0100/1001) are not transcribed — census-zero — and keep the read rung.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.A = 0;
	in.ALPHA.B = 1;
	in.ALPHA.D = 0; // Cs·(As+1) − Cd·As
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	in.prim_overlap_none = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Read);

	// Wrap and PABE outrank the carrier exactly as they outrank every rung.
	in = BaseInput();
	in.abe = true;
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	in.colclamp = false;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::ColClip);
	in.colclamp = true;
	in.pabe = true;
	EXPECT_EQ(gsTileLowerDraw(in).reason, GSTileFloorReason::Blend);
}

TEST(GSTileLowering, ClassicCarrierLeavesTheExactRungsAlone)
{
	// A proven-128 carrier is rung 3's row and stays fixed-function — the carrier
	// sits below the exact rungs, never above them.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.ALPHA.B = 2;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 128;
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::FixedFunction);
	EXPECT_FALSE(p.pass[0].blend_src1);

	// The exact-mix rung's admission (proven constant, term clears) still lands
	// first, re-encoded to FIX exactly as before.
	in = BaseInput();
	in.abe = true;
	in.ALPHA.C = 2;
	in.ALPHA.FIX = 64;
	in.blend_classic_carrier = true;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Mix);
	EXPECT_TRUE(p.pass[0].blend_mix);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_FALSE(p.pass[0].blend_src1);

	// A degenerate As through the carrier lands as the constant it proves (#90):
	// the FIX twin rows are factor-for-factor identical at the proven value, and
	// a constant never needs the SRC1 carrier — so it works without dual source.
	in = BaseInput();
	in.abe = true;
	in.alpha_min = 64;
	in.alpha_max = 64;
	in.tme = true; // defeats the exact-mix saturation proof, so the carrier serves it
	in.tex_fst = true;
	in.tex_psm = PSMCT32;
	in.blend_classic_carrier = true;
	in.dual_source_blend = false;
	p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	EXPECT_EQ(p.pass[0].blend_leg, GSTileBlendLeg::Mix);
	EXPECT_EQ(p.pass[0].blend_c, 2);
	EXPECT_EQ(p.pass[0].afix, 64);
	EXPECT_FALSE(p.pass[0].blend_src1);
}

TEST(GSTileLowering, ClassicCarrierRidesBothPassesOfASplit)
{
	// A split draw carries the carrier realization on both passes, like the read
	// rung does. Only one pass writes RGB; the carrier's C is As, not Ad, so the
	// RGB_ONLY stale-alpha hazard documented at the overlap floor cannot arise.
	GSTileDrawInput in = BaseInput();
	in.abe = true;
	in.TEST.ATE = 1;
	in.TEST.ATST = ATST_GEQUAL;
	in.TEST.AREF = 64;
	in.TEST.AFAIL = AFAIL_RGB_ONLY;
	// The SPLIT still needs the depth outcome to be order-independent — that is
	// the split's own condition, not the blend's. The carrier itself needs no
	// overlap fact. Alpha proven ≤ 128 so the mix leg admits under the
	// no-overlap fact (a may-exceed carrier would take the read rung there).
	in.prim_overlap_none = true;
	in.alpha_max = 128;
	in.blend_classic_carrier = true;
	in.dual_source_blend = true;
	GSTileDrawPlan p = gsTileLowerDraw(in);
	ASSERT_TRUE(p.native);
	ASSERT_EQ(p.pass_count, 2);
	for (u32 i = 0; i < p.pass_count; i++)
	{
		EXPECT_TRUE(p.pass[i].abe) << "pass " << i;
		EXPECT_EQ(p.pass[i].blend_leg, GSTileBlendLeg::Mix) << "pass " << i;
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
