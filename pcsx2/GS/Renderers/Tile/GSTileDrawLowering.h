// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GS.h"
#include "GS/GSRegs.h"
#include "GS/Renderers/Tile/GSTileTypes.h"

#include <algorithm>
#include <cmath>

// The draw-lowering compiler's first rung: decide from the raw register snapshot
// whether one draw can be realized natively, and if so under what write claims. Pure
// function over PODs — no GS state, no device — so the predicate sweeps in ctest.
//
// The native envelope: triangles and sprites, no blending; textures through the GPU
// sampler for affine nearest and through the in-shader integer filter otherwise, with
// STQ coordinates served on sprites and floored on triangles; an alpha test either
// provable from the vertex-trace alpha range or realizable as a one- or two-pass
// split; fog in-shader at the console's integer rule; frame formats whose cells upload
// and read back losslessly (CT32/CT24); and depth formats that round-trip exactly
// through a float32 depth buffer AND pair with the frame's pixel size (Z24 under the
// 32-bit frames; Z32 floors on float32 storage, Z16/Z16S floor on pairing unless the
// draw is depth-only). Everything else takes the SW floor, tagged with the first
// reason that disqualified it; the per-title distribution of that tag is the
// renderer's primary health metric.
//
// ⚠️ The floor is development scaffolding and a correctness backstop, never a shipping
// path: each native/floor handoff on GPU-truth pages costs a synchronous readback, and
// the measured cost curve is U-shaped in native coverage, so a promoted title runs its
// benchmark scenes at zero floor invocations.
//
// Console-measured test-stage rules baked in here (gs-test capture, SCPH-30001):
//   - DATE on a 24-bit frame FAILS every pixel (it does not pass-through as "no
//     alpha to test"), and a failing DATE stops the depth write — the draw is a
//     provable no-op. GSRendererSW returns early the same way.
//   - AFAIL=RGB_ONLY degrades to FB_ONLY off 32-bit frames (GetAFAIL, confirmed).
//   - FBA reaches only destinations that store alpha; on a 24-bit frame it is inert.
//   - FRAME and ZBUF layouts pair by pixel size; crossing them lands writes at
//     coordinates neither layout agrees on, so mismatched pairings never go native.
//
// Two facts here are semantic, not convenience, and are pinned by the unit suite:
//   - ZTE=0 disables the z write as well as the z test (GSRendererSW derives
//     zm = ZMSK || !ZTE — the floor and the native path must claim identically);
//   - claims are BYTE coverage, not meaning: an unmasked color write claims the Z
//     plane of its pages too, because a depth reading of those bytes overlaps every
//     byte the write touched (the mirror of gsTilePlanesInvalidatedByWrite).

enum class GSTileFloorReason : u8
{
	None = 0, ///< native
	// Register-predicate reasons (decided by gsTileLowerDraw):
	PrimClass, ///< points/lines — not in the M2 envelope
	SpriteExpandUnavailable, ///< sprite draw without VS expand support
	Textured, ///< unused — tme gates per-mechanism below; retained so old ledgers decode
	Blend,
	CoverageAA1,
	Fog, ///< unused — fog is in-shader integer; retained so old ledgers decode
	Fba,
	ScanMask,
	Dither, ///< DTHE — the native path writes no dither matrix (see the gate)
	AlphaTest, ///< dynamic ATE whose pass split would reorder the draw's own depth writes
	DateTest, ///< DATE on a format with real destination alpha
	ZTestNever, ///< draw can write nothing — let the floor no-op it
	FrameMaskPartial, ///< FBMSK masks part of a byte — needs the fbmask shader
	NothingWritten, ///< every channel masked and no z write
	FramePsm, ///< frame format outside CT32/CT24
	ZbufPsm, ///< depth format that cannot round-trip through float32 (Z32)
	FrameZPairing, ///< frame and depth pixel sizes differ — unmeasured hardware territory
	StrideZero, ///< FBW=0 — degenerate stride
	// Geometry reasons (decided by the route, which knows the footprints):
	FootprintExtent, ///< draw wider than the stride or deep enough to wrap page space
	FrameZOverlap, ///< FRAME and ZBUF footprints share pages
	ResourceFailure, ///< target pool allocation failed
	// Texture gates (register-predicate):
	TextureFiltered, ///< unused — bilinear is the in-shader integer filter; retained so old ledgers decode
	TextureMip, ///< mip levels the GPU chain cannot express (deeper than the base pyramid, or an oversized claim)
	TexturePsm, ///< texture format the source builder does not serve (PSGPU24)
	TexturePerspective, ///< STQ triangle — the coordinate walk is implemented and probe-proven; this waits on depth parity (see the lowering comment)
	// Texture geometry reason:
	TextureFeedback, ///< texture pages intersect the draw's own write footprint (M4)
	// STQ range reason:
	TextureStqOverflow, ///< STQ coordinates outside the scanline's 16.16 envelope, or Q not provably positive — the floor's host arithmetic stays the authority
	// Depth transcription reason (decided by the route, which builds the payload):
	DepthWalkEnvelope, ///< Z-gradient triangle outside the soft-float walk's envelope (z hull, gradient magnitude, seed shape, or no VS expand)
	Count
};

/// Which clause of the STQ range guard fired on a non-FST textured draw.
///
/// A draw with any bit set floors as TextureStqOverflow. The mask is carried on the
/// plan whatever reason actually floors the draw first, because TexturePerspective
/// outranks this gate: without the mask the guard's real population is invisible in
/// the ledger, and its clauses cannot be told apart when it does show.
enum GSTileStqGuard : u8
{
	GSTileStqGuardNone = 0,
	GSTileStqGuardNan = 1 << 0, ///< an S, T or Q coordinate is NaN
	GSTileStqGuardQPole = 1 << 1, ///< Q reaches or crosses zero — the quotient is unbounded
	GSTileStqGuardHullU = 1 << 2, ///< the |s/q| hull in texels leaves the scanline's 16.16 envelope
	GSTileStqGuardHullV = 1 << 3, ///< the |t/q| hull in texels leaves it
	GSTileStqGuardTexSize = 1 << 4, ///< TW or TH past the 1024-texel clamp
};

/// The STQ range guard, as a pure function of the vertex trace.
///
/// tmin/tmax are GSVertexTrace's m_min.t / m_max.t for a !FST draw and are ALREADY IN
/// TEXELS — the trace finalises the per-vertex quotients scaled by (1 << TW, 1 << TH).
/// Scaling them again is the bug this function exists to make untestable-by-eye no
/// longer: it inflated the hull by the texture size and floored a third of the corpus.
/// GSRendererSW's own overflow test is the authority on the envelope and compares the
/// same values against the same 32766 with no scaling.
///
/// nan_mask is GSVertexTrace::nan.value (S/T/Q bits). tw/th are the raw TEX0 shifts.
inline u8 gsTileStqGuard(const GSVector4& tmin, const GSVector4& tmax, u32 nan_mask, u32 tw, u32 th)
{
	// The rasterizer stores texture coordinates as 1.15.16 fixed point; two texels of
	// slack keeps the bilinear neighbour inside it. Same constant as the SW renderer.
	constexpr float limit = 32766.0f;
	const float hull_u = std::max(std::abs(tmin.x), std::abs(tmax.x));
	const float hull_v = std::max(std::abs(tmin.y), std::abs(tmax.y));
	// A Q range that reaches zero has a pole in it, and then the vertex quotients stop
	// bounding the interpolated ones. Written so a NaN bound answers "not provable".
	const bool q_spans_zero = !(tmin.z > 0.0f || tmax.z < 0.0f);
	return static_cast<u8>(
		(nan_mask != 0 ? GSTileStqGuardNan : 0) |
		(q_spans_zero ? GSTileStqGuardQPole : 0) |
		(!(hull_u < limit) ? GSTileStqGuardHullU : 0) |
		(!(hull_v < limit) ? GSTileStqGuardHullV : 0) |
		((tw > 10 || th > 10) ? GSTileStqGuardTexSize : 0));
}

struct GSTileDrawInput
{
	u32 prim_class; ///< GS_PRIM_CLASS
	GIFRegTEST TEST;
	GIFRegFRAME FRAME;
	GIFRegZBUF ZBUF;
	u8 scanmsk; ///< SCANMSK.MSK
	// Source alpha range of the draw (the vertex-trace fact; exact for untextured
	// draws, which are the only ones that reach the alpha-test logic — tme floors
	// first). Callers without the fact pass the full 0..255 range.
	u8 alpha_min;
	u8 alpha_max;
	bool tme;
	bool abe;
	bool aa1;
	bool fba;
	bool dthe; ///< DTHE.DTHE — dither enable, an environment register
	bool vs_expand; ///< device supports VS sprite expansion
	// Texture gates (meaningful only when tme). tex_linear is the vertex trace's
	// IsLinear — the same criterion the SW scanline uses to pick its filter, so the
	// native path and the floor agree on which draws filter by construction.
	// tex_mip is the state's IsMipMapActive; tex_fst is PRIM.FST.
	bool tex_linear;
	bool tex_mip;
	// Meaningful when tex_mip: the level geometry maps onto a GPU mip chain —
	// min(MXL,6) ≤ max(TW,TH) so every addressed level exists (the GS floors level
	// sizes at one texel exactly like a GPU chain: GetTex0Layer clamps TW/TH−lod at
	// zero), and TW/TH ≤ 10 so the base is a real size (the scanline's oversize
	// cook wraps at ONE texel — modelling that buys nothing). The route computes it.
	bool tex_mip_fit;
	bool tex_fst;
	u8 tex_psm;
	// STQ safety (meaningful when tme && !tex_fst): the route's vertex-trace
	// verdict that the quotient can leave the scanline's 16.16 envelope, that a
	// coordinate is NaN, or that Q is not provably positive. The SW renderer
	// rewrites overflowing CLAMP-mode vertices and saturates the rest with
	// host-dependent conversions; flooring keeps that arithmetic the authority.
	// A GSTileStqGuard mask rather than a bool so the ledger can say WHICH clause
	// fired — the clauses have very different costs to relax.
	u8 tex_stq_guard;
	// Vertex-trace facts the alpha-test split needs. z_constant is m_vt.m_eq.z (every
	// vertex at the same depth); prim_overlap_none is GSState::PrimitiveOverlap()
	// returning NO, i.e. no two of the draw's own primitives touch a pixel twice.
	// Callers without the facts leave both false, which only costs coverage.
	bool z_constant;
	bool prim_overlap_none;
};

/// One realized pass of a native draw, in submission order.
///
/// A draw is one pass unless a dynamic alpha test makes the failing fragments write
/// a different set of channels from the passing ones — then it is two, each with its
/// own write mask and its own comparison. atst is a PS2 ATST value; ALWAYS means the
/// pass applies no test.
struct GSTileDrawPass
{
	u8 colormask = 0;
	bool z_write = false;
	u8 atst = ATST_ALWAYS;
	u8 aref = 0;
};

struct GSTileDrawPlan
{
	bool native = false;
	GSTileFloorReason reason = GSTileFloorReason::None;

	// Effective write facts. Valid whenever the register combination is coherent,
	// so the floor path may consult them too. colormask and z_write are the UNION
	// over the passes below — AFAIL only ever removes writes, so the union is what
	// a passing fragment writes, and that is what the memory-model claims need.
	u8 colormask = 0; ///< wrgba after whole-byte FBMSK exemptions
	u8 fb_claims = 0; ///< planes a native draw claims on FRAME pages (byte coverage)
	u8 z_claims = 0; ///< planes claimed on ZBUF pages when z_write
	bool z_write = false;
	bool z_test = false; ///< fixed-function test does something (ZTST > ALWAYS)
	u8 ztst = ZTST_ALWAYS;

	GSTileDrawPass pass[2];
	u8 pass_count = 1;

	// Census only, never consulted by the route: the STQ guard clauses that fired,
	// carried out even when an earlier reason floored the draw. Zero on FST and
	// untextured draws.
	u8 stq_guard = GSTileStqGuardNone;
};

inline GSTileDrawPlan gsTileLowerDraw(const GSTileDrawInput& in)
{
	GSTileDrawPlan p;

	// Census, set before any floor return so the guard's population survives being
	// outranked by an earlier reason.
	p.stq_guard = (in.tme && !in.tex_fst) ? in.tex_stq_guard : static_cast<u8>(GSTileStqGuardNone);

	// SW-floor z semantics: ZTE=0 turns off the test AND the write.
	p.z_write = in.TEST.ZTE && !in.ZBUF.ZMSK;
	p.z_test = in.TEST.ZTE && in.TEST.ZTST > ZTST_ALWAYS;
	p.ztst = static_cast<u8>(in.TEST.ZTE ? in.TEST.ZTST : static_cast<u32>(ZTST_ALWAYS));

	// Whole-byte FBMSK exemptions; a partially masked byte disqualifies below.
	const u32 fbmsk = in.FRAME.FBMSK;
	bool partial_byte = false;
	u8 cm = 0;
	for (u32 b = 0; b < 4; b++)
	{
		const u32 byte = (fbmsk >> (b * 8)) & 0xFFu;
		if (byte == 0)
			cm |= static_cast<u8>(1u << b);
		else if (byte != 0xFFu)
			partial_byte = true;
	}
	if (in.FRAME.PSM == PSMCT24)
		cm &= 0x7; // no alpha byte in storage

	// DATE on a 24-bit frame fails every pixel and stops the depth write with it
	// (console-measured; a physically-present alpha byte in memory changes nothing).
	// The predicate mirrors GSRendererSW's early return: (PSM & 0xF) catches the
	// Z-swizzle 24-bit frame layout too. Settled before the alpha test, because a
	// draw that writes nothing has no test to realize.
	if (in.TEST.DATE && (in.FRAME.PSM & 0xF) == PSMCT24)
	{
		cm = 0;
		p.z_write = false;
	}

	// An alpha test with a PROVABLE outcome is not a test. Provably passing, it
	// disappears; provably failing, AFAIL is a deterministic write-mask edit (the
	// GS idiom "ATST=NEVER, AFAIL=FB_ONLY" is how games write color without depth).
	// The z TEST is untouched either way — AFAIL suppresses only the z write, and
	// the surviving writes are still depth-gated (the SW scanline's fm/zm shape).
	bool atst_dynamic = false;
	bool atst_split = false; ///< the dynamic test was realized into p.pass[]
	if (in.TEST.ATE && in.TEST.ATST != ATST_ALWAYS)
	{
		enum class Outcome
		{
			Pass,
			Fail,
			Dynamic
		};
		Outcome outcome = Outcome::Dynamic;
		const u32 aref = in.TEST.AREF;
		const u32 lo = in.alpha_min;
		const u32 hi = in.alpha_max;
		switch (in.TEST.ATST)
		{
			case ATST_NEVER:
				outcome = Outcome::Fail;
				break;
			case ATST_LESS:
				outcome = (hi < aref) ? Outcome::Pass : ((lo >= aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_LEQUAL:
				outcome = (hi <= aref) ? Outcome::Pass : ((lo > aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_EQUAL:
				outcome = (lo == aref && hi == aref) ? Outcome::Pass :
						  ((aref < lo || aref > hi) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_GEQUAL:
				outcome = (lo >= aref) ? Outcome::Pass : ((hi < aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_GREATER:
				outcome = (lo > aref) ? Outcome::Pass : ((hi <= aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			case ATST_NOTEQUAL:
				outcome = (aref < lo || aref > hi) ? Outcome::Pass :
						  ((lo == aref && hi == aref) ? Outcome::Fail : Outcome::Dynamic);
				break;
			default:
				break;
		}
		if (outcome == Outcome::Fail)
		{
			switch (in.TEST.GetAFAIL(in.FRAME.PSM))
			{
				case AFAIL_KEEP:
					cm = 0;
					p.z_write = false; // nothing written at all -> NothingWritten below
					break;
				case AFAIL_FB_ONLY:
					p.z_write = false;
					break;
				case AFAIL_ZB_ONLY:
					cm = 0;
					break;
				case AFAIL_RGB_ONLY:
					cm &= 0x7;
					p.z_write = false;
					break;
			}
		}
		else if (outcome == Outcome::Dynamic)
		{
			// A genuinely dynamic test. The native path never reads the destination
			// to resolve one — that machinery is M4 — so the realization is a split
			// of the draw into at most two passes, each with its own write mask.
			//
			// Which split is exact depends on what a FAILING fragment still writes,
			// which is AFAIL after the RGB_ONLY degrade. Three simplifications come
			// first, and they are pure register algebra: an AFAIL edit that removes
			// a write the draw does not make is not an edit at all.
			u32 afail = in.TEST.GetAFAIL(in.FRAME.PSM);
			if (afail == AFAIL_RGB_ONLY && !(cm & 0x8))
				afail = AFAIL_FB_ONLY; // no alpha write to suppress
			const bool fail_writes_same =
				(afail == AFAIL_FB_ONLY && !p.z_write) ||
				(afail == AFAIL_ZB_ONLY && cm == 0);
			const bool fail_writes_nothing =
				afail == AFAIL_KEEP ||
				(afail == AFAIL_FB_ONLY && cm == 0) ||
				(afail == AFAIL_RGB_ONLY && !(cm & 0x7)) ||
				(afail == AFAIL_ZB_ONLY && !p.z_write);

			if (fail_writes_same)
			{
				// Passing and failing fragments write the same channels, so there is
				// nothing for the test to decide. It disappears like a provable one.
			}
			else if (fail_writes_nothing)
			{
				// One pass, failing fragments discarded in the shader.
				atst_split = true;
				p.pass_count = 1;
				p.pass[0] = {cm, p.z_write, static_cast<u8>(in.TEST.ATST), static_cast<u8>(aref)};
			}
			else
			{
				// The two write masks genuinely differ, so the draw splits. Every
				// split below reorders the draw's own fragments relative to the SW
				// scanline, which walks them in primitive order — so each is exact
				// only while the depth outcome does not depend on that order. That
				// is the same condition Classic calls independent_z, and it holds
				// when nothing writes depth, when the test cannot fail, when every
				// vertex sits at one depth under GEQUAL (rewriting a pixel with the
				// depth it already holds still passes), or when the draw's own
				// primitives never touch a pixel twice. Classic's companion
				// independent_rgb is vacuous here: it guards RGB that depends on
				// destination alpha through the blend unit, and blending floors.
				const bool independent_z = !p.z_write || p.ztst == ZTST_ALWAYS ||
					(p.ztst == ZTST_GEQUAL && in.z_constant) || in.prim_overlap_none;
				if (independent_z)
				{
					atst_split = true;
					p.pass_count = 2;
					const u8 atst8 = static_cast<u8>(in.TEST.ATST);
					const u8 aref8 = static_cast<u8>(aref);
					if (afail == AFAIL_ZB_ONLY)
					{
						// Split by fragment: the passing ones write color and depth,
						// the failing ones only depth. A channel split cannot serve
						// this one — depth written first would be what the color
						// pass then tests against.
						static constexpr u8 inverted[] = {ATST_ALWAYS, ATST_NEVER, ATST_GEQUAL, ATST_GREATER,
							ATST_NOTEQUAL, ATST_LESS, ATST_LEQUAL, ATST_EQUAL};
						p.pass[0] = {cm, p.z_write, atst8, aref8};
						p.pass[1] = {0, p.z_write, inverted[atst8 & 7], aref8};
					}
					else if (afail == AFAIL_RGB_ONLY)
					{
						// Split by channel: every fragment writes RGB, so pass one
						// runs with no test at all and pass two adds what only a
						// passing fragment may write. Strictly better than splitting
						// by fragment, which would put RGB in both passes and so
						// composite overlapping primitives out of order.
						p.pass[0] = {static_cast<u8>(cm & 0x7), false, ATST_ALWAYS, 0};
						p.pass[1] = {static_cast<u8>(cm & 0x8), p.z_write, atst8, aref8};
					}
					else // AFAIL_FB_ONLY
					{
						// Split by channel again: color for everyone, depth for the
						// fragments that pass.
						p.pass[0] = {cm, false, ATST_ALWAYS, 0};
						p.pass[1] = {0, p.z_write, atst8, aref8};
					}
				}
				else
				{
					atst_dynamic = true;
				}
			}
		}
	}

	p.colormask = cm;
	u8 fb_claims = 0;
	if (cm & 0x7)
		fb_claims |= GSTilePlaneRGB;
	if (cm & 0x8)
		fb_claims |= kGSTilePlanesAlpha;
	if (fb_claims != 0)
		fb_claims |= GSTilePlaneZ; // byte coverage: written bytes invalidate any depth reading
	// ...and the Z claim spans the whole 32-bit cell, so the surface ends up holding the
	// newest bytes of every color plane, not just the channels the colormask let through.
	// Claim what we span. The upload gate (gsTileColorPlanesSpannedBy, same rule) already
	// made the texture current across that span before the draw, so the wider claim is
	// exactly true -- and claiming LESS than we span is a page the surface can never own,
	// which re-uploads the whole footprint on every later draw and, because the surface
	// still holds unsynced truth there, downloads it back first. That round-trip was 93%
	// of the Tile frame on GT4 (796 readback submits/frame, 250 ms of a 276 ms frame).
	fb_claims |= gsTileColorPlanesSpannedBy(fb_claims);
	p.fb_claims = fb_claims;
	p.z_claims = p.z_write ? gsTilePlanesInvalidatedByWrite(in.ZBUF.PSM) : 0;

	const auto floored = [&p](GSTileFloorReason r) {
		p.native = false;
		p.reason = r;
		return p;
	};

	if (in.prim_class != GS_TRIANGLE_CLASS && in.prim_class != GS_SPRITE_CLASS)
		return floored(GSTileFloorReason::PrimClass);
	if (in.prim_class == GS_SPRITE_CLASS && !in.vs_expand)
		return floored(GSTileFloorReason::SpriteExpandUnavailable);
	// The texture envelope. Affine nearest goes through the GPU sampler, which the
	// gs-texture capture measures console-exact; bilinear goes through the in-shader
	// integer filter reproducing the SW scanline (1/16-texel truncating snap, 4-bit
	// weight, nested truncating lerps); STQ draws recompute the scanline's per-pixel
	// trunc(s/q) into 16.16 with the GLSL divide tightened to IEEE rounding. STQ
	// SPRITES run all of it natively. STQ TRIANGLES floor here, and the blocker is
	// depth parity — not the coordinate walk, which gs-grad scores 99.9%
	// word-identical to software on the perspective sections through this same path.
	//
	// Why depth parity: OutRun's road and roadside strips ladder overlapping triangles
	// at consecutive Z under GEQUAL, so a one-unit disagreement decides whole pixels.
	// The vertex shader's 1/320-pixel boundary nudge translates the whole primitive, so
	// interpolated depth rides its own gradient and lands 5 to 16 units past the
	// scanline, one-sided on 40,662 of 40,666 pixels of one such draw.
	//
	// That nudge is load-bearing and cannot be dropped for native draws. Without it,
	// depth does everything wanted (the same draw falls to 3,182 pixels off by exactly
	// one, balanced), the corpus goes 8/9 to 9/9, and both gs-coverage arms stay
	// byte-identical — but gs-interp shows the nudge is also what makes GOURAUD COLOUR
	// match: with it tile is byte-identical to software on that capture, without it
	// 22,760 bytes differ and the colour-gradient sections score worse against the
	// console. The pixel-corner sample point for colour is currently made of the vertex
	// conversion and the nudge together, so the nudge cannot move alone.
	// ⚠️ gs-coverage is the wrong gate for any of this — it scores which pixels are
	// covered, never what value is interpolated at them.
	//
	// Settled, and not worth re-opening:
	//
	//   - Depth transcription. The soft-float walk (BuildZWalkPayload over
	//     GSComputeTriangleZPlane) reproduces the scanline: with this floor lifted,
	//     gs-interp scores depth 0 of 29,164 readings differing from software.
	//   - The depth comparator. gs-decide measures the plain unsigned rule with GEQUAL
	//     passing the tie, 100.00% over 30,720 readings.
	//   - Coverage. Byte-identical to software under both arms.
	//   - Mip sampling on STQ triangles. With this floor lifted gs-grad's 31 mip
	//     TRIANGLE cases go native (ledger: all 39 mipmapped draws, zero floored) and
	//     score against software lod-select 100.00% of 19,456 words, lod-blend 100.00%
	//     of 6,144, mip addressing 100.00%, lod-xover 99.92%. That spans level
	//     selection at MXL 1/2/6 and the per-pixel trilinear blend — the case a
	//     sprites-only probe structurally cannot reach. Forcing level 0 in the shader
	//     takes those three sections to 0.00% / 25.75% / 68.75%, so the coverage
	//     discriminates a level error rather than agreeing vacuously.
	//     ⚠️ The bar met there is SW-PARITY, not console truth: both arms sit at
	//     76.50% against silicon on lod-blend, so fixing SW's mip defects is a
	//     Tile-gate event.
	//
	// What the floor still holds back, and the shape of each:
	//
	//   1. The gouraud residual, which is a VISIBILITY problem rather than a new
	//      defect. The colour-gradient sections are identical between the floored and
	//      lifted tile arms (worst one level), so the lift does not create it — it
	//      changes who can see it. GT4 goes from 2,612 native draws carrying 28 gouraud
	//      and zero perspective-textured, to 4,442 carrying 1,858 gouraud, every one of
	//      the 1,830 new perspective draws gouraud-shaded; flat natives are unchanged at
	//      2,584. This floor is the only reason the corpus has ever claimed
	//      byte-identity on shading, so lifting it surfaces a worst-one haze everywhere
	//      at once — GT4's 18.27% of pixels differing at 1.99% above two levels.
	//
	//   2. The perspective coordinate. ⚠️ MOSTLY CLOSED — the paragraph that stood here
	//      argued it could not be, and the argument was wrong about which mechanism
	//      dominated. It named the scanline's accumulating four-pixel step, which is
	//      indeed not reproducible in closed form; the actual driver was the vertex
	//      shader's coverage nudge, which is not an arithmetic problem at all. The
	//      coordinate now comes off the primitive's own plane at the integer pixel index
	//      (PS_TILE_STQ, BuildTilePayload), so nudged geometry decides coverage and never
	//      reaches the coordinate. Measured on this corpus with this floor scratch-lifted,
	//      plane off versus on: GT4 OPB 3.62% of pixels differing → 0.44% (2.72% → 0.34%
	//      above two levels), GT4 retail 18.27% → 0.60%, OutRun 4.46% → 0.43%, Armored
	//      Core 18.95% → 5.40%, FlatOut 2 to byte-identical, dumps passing 8/16 → 9/16.
	//      The sprite half is byte-exact on its own instrument: gs-mip-sprites goes from
	//      13,478 pixels across 29 of 85 cases to identical on all 85.
	//
	//      What is left of it IS the accumulating step, and that part of the old argument
	//      stands: the ARM64 scanline walks q += d4.q per four-pixel block, no fragment
	//      shader reproduces a sequential accumulation in closed form, and it is
	//      console-neutral — against silicon the lifted arm differs on 19,273 words of
	//      88,064 where software differs on 19,260, and on tc-persp the lifted arm is the
	//      CLOSER of the two (7,504 vs 7,512). Silicon's own divide moves a
	//      mathematically-constant coordinate on 78% of readings; software misses by
	//      holding still. Byte-identity to a sequential software DDA is not a bar a GPU
	//      can meet, and the plan's within-one-level tolerance is the honest gate there.
	//      Generalise: a residual attributed to the mechanism you can name is not
	//      thereby attributed — this one had two populations and the loud one was
	//      geometric, not arithmetic.
	//
	//   3. A native/floor HANDOFF defect, which is what actually explains the corpus.
	//      Dirge lifted, before the plane: 27.35% of pixels differ, 22.79% by more than
	//      two levels, and 91.3% of those live in horizontal runs of eight pixels or
	//      more (median error 25 levels, whole 597-pixel rows). A boundary flip
	//      scatters as salt and pepper and only 0.4% of these pixels are isolated, so
	//      the damage is draw-level, not sampling. Flooring the mip STQ triangles alone
	//      takes it to 6.45%, across FIFTY-TWO draws of 1,943 natives — and since their
	//      sampling is exact, what those draws do differently is claim PAGES: a mip
	//      draw sources a whole level chain. Two sites walk those levels and must
	//      agree — the readback path over levels 1..MXL off the MIPTBP registers, and
	//      the source claim over 0..MXL through the layer accessor. The remainder is
	//      1,883 palettised gouraud character triangles of 1x1 to 3x3 recorded area,
	//      plus full-width rows they are far too small to have drawn, which is the same
	//      signature. ⚠️ No capture can see this class: probe textures are uploaded once
	//      and never aliased against a render target, which is what a real game does
	//      constantly.
	//
	//      ⚠️ Dirge is the one dump the coordinate plane made WORSE, and the 2x2 that
	//      owns it says the plane is not the cause. Percent of pixels differing by more
	//      than two levels, worst frame (all four agree within 0.02, so the readings are
	//      stable), this floor scratch-lifted throughout:
	//
	//                          mip STQ native   mip STQ floored   difference
	//          plane off           22.79             6.45           16.34
	//          plane on            34.15             7.60           26.55
	//
	//      The plane costs 11.36 points, and 10.21 of them — ninety percent — are
	//      inside those same 52 draws, which are 2.7% of the natives. Everything else
	//      moves 1.15. So the plane does not introduce a defect here; it makes the
	//      page-claim defect above manifest harder, on exactly the draws that already
	//      had it. Reason 2's other five dumps improve by factors of three to thirty
	//      under the same lever, and shipped Dirge (this floor in place) is
	//      byte-identical, so nothing here is live today.
	//
	//      The obvious mechanism is REFUTED and should not be re-proposed: a mip level
	//      is a step function of Q and the plane changes Q, so level selection looks
	//      like the amplifier. Keeping the level on the interpolated Q while the
	//      coordinate stays on the plane moves 34.15 to 33.65 — half a point of the
	//      11.36. The amplification travels through the coordinate reaching different
	//      texels of an already-wrong page, not through which level it picks.
	//
	// Lifting this floor re-sorts the census to BLEND: on GT4, 48.1% native becomes
	// 81.7% with the entire remaining floor blending at 17.9%, and frame format, the
	// z-walk envelope and prim class at a tenth of a percent each. Palette and TEXA
	// expansion are applied CPU-side by the source builder, so every rtx-served format
	// qualifies. Corpus cost of lifting as it stands: 13/16 to 9/16.
	//
	// TexturePerspective outranks TextureMip: a mip STQ triangle waits on the same
	// depth parity as every other STQ triangle, and ordering it the other way would
	// attribute that wait to the mip question. All 15,847 corpus TEX_MIP draws are such
	// triangles, so the ordering moves the whole class in the ledger and changes no
	// draw's path. TextureMip itself floors only pyramids deeper than the base.
	if (in.tme)
	{
		if (in.tex_psm == PSGPU24)
			return floored(GSTileFloorReason::TexturePsm);
		if (!in.tex_fst && in.prim_class == GS_TRIANGLE_CLASS)
			return floored(GSTileFloorReason::TexturePerspective);
		if (in.tex_mip && !in.tex_mip_fit)
			return floored(GSTileFloorReason::TextureMip);
		if (!in.tex_fst && in.tex_stq_guard != GSTileStqGuardNone)
			return floored(GSTileFloorReason::TextureStqOverflow);
	}
	if (in.abe)
		return floored(GSTileFloorReason::Blend);
	if (in.aa1 && in.prim_class == GS_TRIANGLE_CLASS)
		return floored(GSTileFloorReason::CoverageAA1);
	// Fog goes native (M3d): the fragment path blends at the console's own rule,
	// (Ct·F + Cfog·(256−F)) >> 8 with alpha untouched, in integer arithmetic on the
	// scanline's seven-fractional-bit fog factor. Nothing about it constrains the
	// envelope — it edits colour after the texture function and reads no
	// destination — so there is no gate left here.
	// FBA sets the stored alpha's top bit — a destination with no alpha storage makes
	// it inert (console-measured on 24-bit frames; 16-bit frames DO store the bit).
	if (in.fba && (in.FRAME.PSM & 0xF) != PSMCT24)
		return floored(GSTileFloorReason::Fba);
	if (in.scanmsk != 0)
		return floored(GSTileFloorReason::ScanMask);
	// Dither reaches every colour destination on silicon, so it reaches the whole
	// native envelope -- CT32 and CT24 both (gs-dither, SCPH-30001, 2026-08-11:
	// 4080 of 4096 pixels move on each). The scanline adds DM[y & 3][x & 3] to the
	// eight-bit colour; the native path writes no matrix at all.
	//
	// This gate is what keeps the floor honest rather than lucky. Before the SW
	// renderer was fixed to dither those formats, a native dithered draw agreed
	// with the floor by both being wrong, and the capture scored the two arms
	// identically at 50%. Fixing SW turned that into a real divergence -- visible
	// in the probe, invisible in the corpus, which contains no such draw.
	if (in.dthe)
		return floored(GSTileFloorReason::Dither);
	if (atst_dynamic)
		return floored(GSTileFloorReason::AlphaTest);
	// DATE with real destination alpha is a genuinely dynamic test — M4 work. The
	// 24-bit case never reaches here: it zeroed every write above and falls to
	// NothingWritten.
	if (in.TEST.DATE && (in.FRAME.PSM & 0xF) != PSMCT24)
		return floored(GSTileFloorReason::DateTest);
	if (in.TEST.ZTE && in.TEST.ZTST == ZTST_NEVER)
		return floored(GSTileFloorReason::ZTestNever);
	if (partial_byte)
		return floored(GSTileFloorReason::FrameMaskPartial);
	if (p.colormask == 0 && !p.z_write)
		return floored(GSTileFloorReason::NothingWritten);
	if (p.colormask != 0 && in.FRAME.PSM != PSMCT32 && in.FRAME.PSM != PSMCT24)
		return floored(GSTileFloorReason::FramePsm);
	if ((p.z_write || p.z_test) &&
		in.ZBUF.PSM != PSMZ24 && in.ZBUF.PSM != PSMZ16 && in.ZBUF.PSM != PSMZ16S)
		return floored(GSTileFloorReason::ZbufPsm);
	// Hardware pairs the FRAME and ZBUF layouts by pixel size (console-measured:
	// crossing them scatters writes to coordinates neither layout agrees on). The
	// native frames are 32-bit-word formats, so a draw with both surfaces live must
	// pair with Z24; the 16-bit depth formats go native only alongside their matching
	// 16-bit frames (M3+). A single-surface draw has no pairing to violate.
	if (p.colormask != 0 && (p.z_write || p.z_test) && in.ZBUF.PSM != PSMZ24)
		return floored(GSTileFloorReason::FrameZPairing);
	if (in.FRAME.FBW == 0)
		return floored(GSTileFloorReason::StrideZero);

	// Every draw the alpha test did not split is one pass writing what the plan says.
	if (!atst_split)
	{
		p.pass_count = 1;
		p.pass[0] = {p.colormask, p.z_write, ATST_ALWAYS, 0};
	}

	p.native = true;
	return p;
}
