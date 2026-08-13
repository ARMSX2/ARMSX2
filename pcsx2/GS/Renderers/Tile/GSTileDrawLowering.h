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
// The M2 native envelope is opaque untextured draws: triangles and sprites, no
// blending, no texture, and — since M3c — an alpha test that is either provable from
// the vertex-trace alpha range or realizable as a one- or two-pass split, frame formats whose
// cells upload and read back losslessly (CT32/CT24), depth formats that round-trip
// exactly through a float32 depth buffer AND pair with the frame's pixel size (Z24
// under the 32-bit frames; Z32 floors on float32 storage, Z16/Z16S floor on pairing
// unless the draw is depth-only). Everything else takes the SW floor, tagged with the
// first reason that disqualified it; the per-title distribution of that tag is the
// renderer's primary health metric.
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
	Textured, ///< retired at M3b (kept so old ledgers still decode); tme now gates per-mechanism below
	Blend,
	CoverageAA1,
	Fog, ///< retired at M3d (in-shader integer fog); kept so old ledgers still decode
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
	// M3b texture gates (register-predicate):
	TextureFiltered, ///< retired at M3g slice 1 (in-shader integer bilinear); kept so old ledgers still decode
	TextureMip, ///< mip levels the GPU chain cannot express (deeper than the base pyramid, or an oversized claim)
	TexturePsm, ///< texture format the source builder does not serve (PSGPU24)
	TexturePerspective, ///< STQ triangle — blocked on Z/coverage parity, not the coordinate walk (implemented at M3g slice 2, probe-proven; see the lowering comment)
	// M3b geometry reason:
	TextureFeedback, ///< texture pages intersect the draw's own write footprint (M4)
	// M3g slice 2 range reason:
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
	// The texture envelope (M3b nearest, M3g in-shader coordinates): affine
	// nearest goes through the GPU sampler — console-exact (gs-texture capture) —
	// and bilinear through the in-shader integer filter reproducing the SW
	// scanline (1/16-texel truncating snap, 4-bit weight, nested truncating
	// lerps). For STQ the fragment path recomputes the scanline's per-pixel
	// trunc(s/q) into 16.16 with the GLSL divide tightened to IEEE rounding —
	// sprites run it natively; TRIANGLES still floor, and the blocker is NOT the
	// coordinate walk (gs-grad probe: 99.9% word-identical to SW on the
	// perspective sections through this path). It is depth parity, on OutRun's road
	// and roadside strips, which ladder overlapping triangles at consecutive Z under
	// GEQUAL so a one-unit disagreement decides whole pixels.
	//
	// Most of it has been root-caused, though not fixed. The bulk is the vertex
	// shader's 1/320-pixel boundary nudge: it translates the whole primitive, so
	// interpolated depth rides its own gradient and lands 5 to 16 units past the
	// scanline, one-sided on 40,662 of 40,666 pixels of one OutRun draw. Not the
	// truncating interpolator, and not coverage. The comparator is not involved
	// either: gs-decide measured it as the plain unsigned rule, GEQUAL passing the
	// tie, at 100.00% over 30,720 readings including a ladder built for the question.
	//
	// ⚠️ Dropping the nudge for native draws was tried and REVERTED by measurement.
	// It does everything hoped for on depth (that draw's divergence falls to 3,182
	// pixels at exactly one, balanced), takes the corpus 8/9 to 9/9, and leaves both
	// gs-coverage arms byte-identical — but gs-interp says the nudge is load-bearing
	// for GOURAUD COLOUR: with it, tile is byte-identical to the software renderer
	// on that capture; without it, 22,760 bytes differ and the colour-gradient
	// sections score measurably worse against the console (cgrad-sub 5,216 to 10,112
	// readings differing). Two emulator runs are byte-identical, so that is real.
	// The corpus cannot see it — its native draws are flat-shaded, where the colour
	// truncation is identity. Whatever completes the pixel-corner sample point for
	// colour is currently made up of the vertex conversion AND this nudge together;
	// until that is understood, the nudge cannot move alone. **gs-coverage is the
	// wrong gate for it** — that probe scores which pixels are covered, never what
	// value is interpolated at them.
	//
	// ⚠️ 2026-08-13: THE DEPTH HALF ABOVE IS CLOSED, AND DEPTH IS NO LONGER WHY THIS
	// FLOOR EXISTS. The second half used to read: the GPU evaluates the depth plane
	// in float32 while the scanline walks a float64 DDA, so the two floors part by one
	// wherever the exact value lands on an integer, and closing it needs the plane
	// evaluated in integer arithmetic per primitive. That machinery LANDED (the
	// soft-float walk built by BuildZWalkPayload out of GSComputeTriangleZPlane) and
	// it works: with this floor lifted, gs-interp scores depth 0 of 29,164 readings
	// differing from the software arm — zctrl, zgrad-x and zgrad-sub all exactly zero,
	// same as the floored arm. gs-coverage is byte-identical to software under both
	// arms too, so coverage is not involved either. Anyone re-reading this to plan the
	// unlock should not go looking at depth or the sample point again.
	//
	// What the floor is actually still holding back, measured the same day by running
	// gs-interp / gs-grad / gs-coverage under floored-tile, lifted-tile and software:
	//
	//   1. The GOURAUD residual, and it is a VISIBILITY problem, not a new defect.
	//      The colour-gradient sections are identical between the two tile arms
	//      (cgrad-x 5,198, cgrad-sub 8,116, cgrad-xy 1,034, worst one level) — the
	//      residual is pre-existing and the lift does not touch it. What the lift
	//      changes is who can see it: GT4 goes from 2,612 native draws with 28 gouraud
	//      and ZERO perspective-textured, to 4,442 with 1,858 gouraud, every one of
	//      the 1,830 new perspective draws gouraud-shaded. Flat natives are unchanged
	//      at 2,584. So this floor is the only reason the corpus has ever been able to
	//      claim byte-identity on shading — lift it and a worst-one haze appears
	//      everywhere at once. That is GT4's 18.27% of pixels differing at only 1.99%
	//      above two levels.
	//
	//   2. The perspective TEXTURE COORDINATE, which is the genuinely new defect and
	//      the only section the lift moves in gs-interp: tc-persp 0 to 168 of 12,288,
	//      worst 16. gs-grad agrees and localises it further — 4 differing words to
	//      107, every new case tc-persp or tc-snap, so the 1/16-texel snap is
	//      implicated and not just the divide. The pre-existing tc-affine drift is
	//      unchanged.
	//
	// Why a one-to-two-percent coordinate error reads as a catastrophe on the corpus:
	// every newly-native draw is a palettised, bilinear, modulated triangle (Dirge
	// 1,803 P8 + 132 P4; GT4 1,772 P4 + 46 P8). One texel off in a palettised source
	// fetches an unrelated palette entry, so the capture's worst-16 (its texture is a
	// step-16 ramp) becomes max 204 on Dirge and max 105 on GT4. Fix the coordinate
	// and the large-magnitude class goes with it; the worst-one gouraud haze is what
	// would remain. Corpus cost of lifting TODAY: 13/16 to 8/16.
	//
	// Note for whoever lifts it: TEX_PERSPECTIVE masks what is underneath. With it
	// lifted the census re-sorts to BLEND — on GT4, 48.1% native becomes 81.7% and
	// the entire remaining floor is blending at 17.9%, with frame format, the z-walk
	// envelope and prim class at a tenth of a percent each. Not a depth question.
	// Palette and TEXA expansion are applied CPU-side by the source builder, so
	// every rtx-served format qualifies. Mip goes native for the classes below
	// (M3g mip slice): the fragment path reproduces the scanline JIT's LOD
	// machinery — the fused-fma log2 polynomial on Q scaled by L and offset by K,
	// truncating float→int, per-pixel level shift of both the coordinate and the
	// cooked wrap bounds, per-level texelFetch, and the trilinear mix
	// (diff·(lodf>>1))>>15. The perspective floor OUTRANKS mip now: a mip STQ
	// triangle waits on the same Z/coverage parity as every other STQ triangle,
	// and attributing it to the mip question would hide that. Every corpus
	// TEX_MIP draw (15,847) is such a triangle, so this reorder moves the whole
	// class to TEX_PERSPECTIVE in the ledger and no corpus draw changes path.
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
