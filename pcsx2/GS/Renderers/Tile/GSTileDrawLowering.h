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
	// Blend gates (M4a):
	ColClip, ///< COLCLAMP=0 on a blended draw — wrap semantics need the in-shader blend (M4c)
	BlendOverlap, ///< the read rung's equation is admissible but the draw's own primitives may overlap — one snapshot of the destination cannot serve them (M4c)
	BlendTexSample, ///< read rung, textured draw with a per-pixel factor — floors on a PRE-EXISTING SAMPLING divergence the blend floor had been hiding, not on anything the blend does (M4c)
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
	// The blend equation's registers (M4). ALPHA carries the A/B/C/D selectors and the
	// FIX byte; consulted only when abe. colclamp is COLCLAMP.CLAMP — clamp (1) versus
	// 8-bit wrap (0) on the blend output; pabe is PABE.PABE, the per-pixel gate on the
	// source alpha's MSB. M4a only guards on colclamp; the selector algebra is M4b.
	GIFRegALPHA ALPHA;
	bool colclamp;
	bool pabe;
	u8 scanmsk; ///< SCANMSK.MSK
	// The POST-TEXTURE-FUNCTION alpha range of the draw (GetAlphaMinMax: the vertex
	// trace with TFX/TEXA/CLUT folded in — Classic's alpha_c0 classification). This
	// is the alpha the alpha test compares, and the alpha the blend's As factor and
	// PABE read (pre-FBA, console-measured, gs-alpha2), so one fact serves all
	// three. Callers without the fact pass the full 0..255 range, which only costs
	// coverage.
	u8 alpha_min;
	u8 alpha_max;
	// The vertex trace's colour floor: min over the draw's vertices of
	// min(R, G, B), meaningful for untextured draws (a texture function makes the
	// shaded colour unbounded below). The mix rung's saturation guard consumes it;
	// callers without the fact leave 0, which only costs coverage. fge is
	// PRIM.FGE — fog edits the colour after the texture function, below any
	// vertex bound, so the guard refuses fogged draws.
	u8 color_min_rgb;
	bool fge;
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
///
/// M4 contract rewrite #1 of the budgeted two: the pass carries its blend
/// realization. abe=false means the pass writes the shaded source untouched and
/// every blend field below is meaningless. The selectors are POST-COLLAPSE values
/// (rung 1 rewrites them before the plan is emitted), so two register states that
/// collapse to the same equation produce identical passes — which is what the memo
/// key needs. Populated from M4b; until then every blended draw floors before a
/// plan reaches the route, and the suite pins the fields inert on native plans.
struct GSTileDrawPass
{
	u8 colormask = 0;
	bool z_write = false;
	u8 atst = ATST_ALWAYS;
	u8 aref = 0;
	// Blend realization (M4). Cv = (((A − B) * C) >> 7) + D, console rule.
	bool abe = false; ///< pass blends
	bool colclip_wrap = false; ///< COLCLAMP=0 — the blend output wraps at 8 bits (M4c)
	bool rt_read = false; ///< realization reads the destination (rung 9 — the paid path)
	u8 blend_a = 0; ///< A selector, post-collapse (0 Cs, 1 Cd, 2 zero)
	u8 blend_b = 0; ///< B selector, post-collapse
	u8 blend_c = 0; ///< C selector, post-collapse (0 As, 1 Ad, 2 FIX)
	u8 blend_d = 0; ///< D selector, post-collapse
	u8 afix = 0; ///< ALPHA.FIX byte, meaningful when blend_c == 2
	/// M4 contract rewrite #2 of the budgeted two: the realization is the donor's
	/// blend-mix — the shader computes the Cs-side term with the ∓124/256
	/// round-to-floor offsets and the ROP carries the Cd term. The selectors above
	/// stay RAW; the renderer owns the donor's selector rewrite and the per-device
	/// As carrier (SRC1, or factor-in-alpha where the alpha channel is free).
	bool blend_mix = false;
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

	// Blend resolution, rung 1 of the M4 lattice (m4-blending.md §4): the exact
	// whole-draw collapses that make the blend disappear. Cv = (((A−B)*C)>>7)+D,
	// and three algebraic identities eliminate it, each exact per-pixel in the
	// GS's integer arithmetic:
	//   A==B          → Cv = D   (the difference is zero whatever C is)
	//   C provably 0  → Cv = D
	//   C provably 128, B==D → Cv = A   (×128>>7 is the identity on the signed
	//                                    difference, so Cv = A − B + D = A)
	// C is provable when it is FIX, or when it is As and the post-texture-function
	// alpha classification is degenerate. A result of Cs means the blend is simply
	// gone — the draw writes the shaded source it was going to write. A result of
	// Cd means the RGB writes are no-ops (alpha is never blended and still
	// writes): Classic's BLEND_CD, realized as a colormask edit rather than blend
	// state, which is why this sits before the alpha-test split builds its pass
	// masks. A result of zero still needs a fragment-output change, so it stays
	// with the emitting rungs (M4b rung 3).
	//
	// Ordering, per the M4a guardrail: wrap draws are NOT collapsed in M4b even
	// where the algebra is clamp-independent — ColClip floors them all until the
	// wrap realization lands (M4c). PABE draws keep flooring too (zero corpus
	// draws; the trace-range skips arrive with the general PABE work).
	bool blend_active = in.abe;
	bool blend_pass = false; ///< rung 3 or the mix rung admitted — the passes carry the realization
	bool blend_mix = false; ///< the mix rung's realization (rung 6), not rung 3's exact rows
	bool blend_read = false; ///< the read rung's realization (rung 9) — the whole equation in-shader
	if (in.abe && in.colclamp && !in.pabe)
	{
		int c_value = -1; // the C factor's value when provable, else -1
		if (in.ALPHA.C == 2)
			c_value = static_cast<int>(in.ALPHA.FIX);
		else if (in.ALPHA.C == 0 && in.alpha_min == in.alpha_max)
			c_value = in.alpha_min;

		u32 result = 4; // a selector 0..2, or 4 = no collapse
		if (in.ALPHA.A == in.ALPHA.B || c_value == 0)
			result = in.ALPHA.D;
		else if (c_value == 128 && in.ALPHA.B == in.ALPHA.D)
			result = in.ALPHA.A;

		if (result == 0) // Cs — the blend is gone
			blend_active = false;
		else if (result == 1) // Cd — RGB writes nothing; alpha writes through
		{
			cm &= 0x8;
			blend_active = false;
		}
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
				// primitives never touch a pixel twice.
				//
				// Classic's companion independent_rgb guards RGB that depends on the
				// destination ALPHA, which a channel split writes in the other pass.
				// It needs no clause here even now that blending goes native: only
				// one pass of any split writes RGB, and the only realization that can
				// read destination alpha at all — the read rung — admits nothing
				// whose primitives overlap. So the blending pass reads the pre-draw
				// alpha, which is exactly what the console's single pass reads when
				// each pixel is touched once.
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
	if (blend_active)
	{
		// The wrap guardrail, ahead of everything else the blend decides: COLCLAMP=0
		// makes the blend output wrap at 8 bits, which fixed-function blending
		// structurally cannot express (gs-blend: the wrap sections are Classic's
		// entire residual, and the failure signature is a 255-level boundary flip no
		// tolerance absorbs). ColClip must outrank Blend so that as M4b lifts the
		// read-free rungs, wrap draws keep flooring until the in-shader wrap
		// realization lands (M4c) — without this ordering they would go silently
		// wrong the moment the blend gate opened. Wrap without blending has nothing
		// to wrap: every pre-blend producer clamps to 8 bits (the texture function's
		// clamp is console-measured, gs-shade), so plain COLCLAMP=0 stays native.
		if (!in.colclamp)
			return floored(GSTileFloorReason::ColClip);
		// PABE floors before any rung opens: its per-pixel gate rides inside the
		// blend equation and no fixed-function realization carries it (zero corpus
		// draws; the trace-range skips arrive with the general PABE work).
		if (in.pabe)
			return floored(GSTileFloorReason::Blend);

		// Rung 3 — the provably EXACT fixed-function rows (M4b). Write the console
		// equation Cv = (((A−B)·C)>>7)+D as Cv = Cs·s + Cd·d: s = C·([A=0]−[B=0])
		// + [D=0], d = C·([A=1]−[B=1]) + [D=1]. A row is fixed-function iff both
		// coefficients lie in {0, ±C, 1−C, 1} with one blend op carrying the signs
		// — which fails exactly where D==A stacks the +1 onto the operand already
		// carrying +C (coefficient C+1: the accumulation rungs' territory), plus
		// the one Cd·C row whose donor encoding is a shader trick rather than a
		// plain factor pair (A=1,B=2,D=2 — rung 5's Cd·Alpha shortcut).
		//
		// ⚠️ Fixed-function admits only a C the ROP cannot ROUND: the GS TRUNCATES
		// the blend product where the ROP rounds to nearest, a +1/2-level mean
		// bias per blend that single-blend instruments cannot see — the gs-blend
		// grid scored the naked encoding 99.64% worst-1, and the corpus then
		// measured the same encoding compounding under MGS3/SotC's deep alpha
		// overdraw into 24–36% of a frame beyond two levels, visible banding.
		// Classic never ships that shape either: its realization for the
		// fractional-C rows is blend-mix with the ∓124/256 round-to-floor offsets,
		// and those rows wait here for that rung. What remains exact: A==B (the
		// difference is zero whatever C — the zero/zero factor pair); a C provably
		// 128, where every factor is exactly one or zero and the sum of integers
		// clamps identically on both sides (As via the trace's degenerate range,
		// FIX by value — the census: 61.9% of FIX draws sit exactly at 128); and
		// the always-zero results (A=2, D=2), which clamp to black in both
		// arithmetics regardless of C. Ad stays out: DST_ALPHA is 0..255 where
		// the GS divides by 128 (the §7 scale mismatch).
		const u8 ba = in.ALPHA.A, bb = in.ALPHA.B, bc = in.ALPHA.C, bd = in.ALPHA.D;
		const bool c_128 = (bc == 2 && in.ALPHA.FIX == 128) ||
		                   (bc == 0 && in.alpha_min == 128 && in.alpha_max == 128);
		const bool zero_result = (ba == 2 && bd == 2 && bc != 1);
		const bool c_ok = (ba == bb) || c_128 || zero_result;
		const bool row_ok = (bd != ba || ba == 2 || ba == bb) && !(ba == 1 && bb == 2 && bd == 2);
		if (!c_ok || !row_ok)
		{
			// Rung 6, queue-jumped (the overdraw banding finding): blend-mix, the
			// donor's realization for the fractional-C rows — the shader computes
			// the Cs-side term biased by the round-to-floor offset so the ROP's
			// round-to-nearest lands on the GS's truncation, and the ROP carries
			// the Cd term through the map's factor. Admission is the four plain
			// mix shapes — (A,B) ∈ {(0,1),(1,0)} with D != A, because D==A stacks
			// the +1 onto the C-carrying operand (coefficient C+1, the donor's
			// A_MAX compensation class, adopted only against a census row) — and
			// it is NARROWER than the donor's, because the corpus measured both of
			// the donor's residual classes integrating into visible banding under
			// particle overdraw (2026-08-14, FlatOut 2 / Dirge / OutRun first
			// light):
			//
			// 1. The carrier must be a PROVEN CONSTANT ≤ 128 — FIX by value, or
			//    As by degenerate trace range, landed as FIX. A variable As
			//    reaches the ROP only through an 8-bit colour channel (the SRC1
			//    output, or the primary output's alpha), which quantizes the
			//    factor from the GS's /128 grid onto the attachment's /255 grid:
			//    an up-to-half-level error scaled by Cd, one-sided per operand
			//    class — OutRun's banding, and Dirge's max-108 speckle through
			//    the known palette-index amplification. The blend constant
			//    carries the same number as full-precision floats. The
			//    variable-As arm waits for an exact carrier.
			//
			// 2. The shader-side term must provably CLEAR THE OFFSET at every
			//    pixel: the blender clamps its colour inputs to [0,1], so a term
			//    below the offset saturates at zero, the offset is destroyed,
			//    and a half-integer destination product rounds up — once per
			//    layer (FlatOut's near-black smoke, +1 per overdraw). The term
			//    is Cs·C/128 (or Cs·(128−C)/128 for the reverse lerp), so the
			//    guard is trace-min-RGB · effective-C ≥ 64 — provable only for
			//    untextured, unfogged draws (a texture function or the fog blend
			//    makes the shaded colour unbounded below).
			//
			// Under both proofs the realization is EXACT on a full-precision
			// blend unit, not ±1: the operands sit on the 1/128 grid, the offset
			// (127/256) converts the single round to a floor, and the gs-blend
			// probe under tile is the per-device instrument (this box: every
			// admitted grid case byte-identical). Ad stays out (the §7 scale
			// mismatch); PABE and wrap floored above.
			const bool mix_row = ((ba == 0 && bb == 1) || (ba == 1 && bb == 0)) && bd != ba;
			const bool degenerate_as = bc == 0 && in.alpha_min == in.alpha_max;
			u32 cval = 0;
			if (bc == 2 && in.ALPHA.FIX <= 128)
				cval = in.ALPHA.FIX;
			else if (degenerate_as && in.alpha_min <= 128)
				cval = in.alpha_min;
			const bool reverse_lerp = ba == 1 && bd == 0; // the MIX3 shape: term Cs·(1−C)
			const u32 eff_c = reverse_lerp ? (128u - cval) : cval;
			const bool term_clears = !in.tme && !in.fge &&
			                         static_cast<u32>(in.color_min_rgb) * eff_c >= 64;
			if (!mix_row || cval == 0 || !term_clears)
			{
				// Rung 9 — the read rung, the one rung that pays and the reason the
				// lattice is total. The fragment shader evaluates the register's own
				// equation in the console's integer arithmetic over a destination the
				// pass has read, so it is byte-exact by construction rather than by
				// admission: every selector, every carrier, and the whole 0..255
				// destination-alpha scale that keeps Ad out of the fixed-function
				// rungs are all just integers here. Nothing about the EQUATION can
				// disqualify a draw from this rung.
				//
				// What can disqualify it is the destination not standing still. The
				// realization reads ONE state of the target — a barrier before the
				// draw where the device has honest texture barriers, a copy of the
				// draw area where it does not — so it is the true destination only
				// while no two of the draw's own primitives touch the same pixel. An
				// overlapping draw would blend its second primitive against the
				// pre-draw value instead of against the first primitive's result,
				// which is silent, plausible, and wrong. Those draws keep flooring
				// under their own reason, so the census can size the class that an
				// ordered in-tile read (or a per-primitive split) would buy — the
				// mechanism question the M4c follow-on decides on measured numbers.
				//
				// The overlap fact is conservative in the same direction: the trace
				// proves NO only for sprites it has walked and for single primitives,
				// and answers UNKNOWN for multi-primitive triangle draws, which floor.
				if (!in.prim_overlap_none)
					return floored(GSTileFloorReason::BlendOverlap);
				// ⚠️ Floors a class this rung REACHES but does not break. A textured
				// draw whose factor varies per pixel renders wrong, for both variable
				// carriers — and the cause is a pre-existing SAMPLING divergence that
				// the blend floor had been hiding, not anything the blend does.
				//
				// Attribution (MGS3, the 4-draw minimal repro, 2026-08-14). The
				// differing pixels sit inside exactly the two admitted draws, and:
				//   * the ALPHA channel is byte-identical, so the factor the shader
				//     feeds the blend is the same value the software arm uses;
				//   * only 3.5% of covered pixels differ, and 124 of those 125 have a
				//     differing neighbour — connected runs, not a scattered residue;
				//   * they sit where the image has TWICE the local gradient of the
				//     pixels that match (median 33 against 16), i.e. at texture
				//     detail, which is where a texel-boundary decision is decided.
				// An arithmetic fault in the blend is independent of local image
				// gradient and would move the flat 96.5% too. A sampling fault looks
				// exactly like this. These are bilinear FST sprites on a palettised
				// texture — the shape of the open tc-sprite half-texel finding.
				//
				// Four corpus runs bracket it, and the blend is in none of the
				// brackets:
				//   * whole rung off       → corpus exactly at the pinned baseline
				//   * FIX carrier only     → exactly the baseline, textured included
				//   * untextured only      → exactly the baseline, every carrier
				//   * per-primitive barrier instead of one, overlap gate removed
				//                          → strictly WORSE, so ordering is not it
				// What survives is one cell: textured AND the factor is per-fragment.
				// A constant factor on the same textured draws is byte-exact, which
				// exercises the destination read and every line of the integer
				// arithmetic — so neither of those is the fault — and an untextured
				// draw with a per-pixel source alpha is byte-exact too.
				//
				// ⚠️ The gs-blend capture is STRUCTURALLY BLIND to this: all 32,768 of
				// its cases are untextured, so it scores tile byte-identical to sw
				// with the rung wide open and damaging nine corpus dumps. The capture
				// that gates blending cannot gate this rung; the class needs an
				// instrument of its own before the admission widens.
				if (in.tme && bc != 2)
					return floored(GSTileFloorReason::BlendTexSample);
				blend_read = true;
			}
			else
				blend_mix = true;
		}
		// Admission only — the fields land on the passes at the end of the
		// function, after the single-pass fill that would otherwise reset them.
		blend_pass = true;
	}
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

	// The admitted blend realization, on every pass — after the fills above so a
	// single-pass rebuild cannot reset it. A/B/D are the register's own (rung 1
	// either eliminated the blend entirely or left them untouched), and a pass
	// that writes no colour carries them inertly.
	//
	// C is re-encoded on the FIXED-FUNCTION rungs and only there. Every row those
	// rungs admit is C-degenerate — C provably one (the 128 proof), C cancelled
	// (A==B), or a result provably zero at every C — so a register that named a
	// variable carrier (As, Ad) lands on the passes as the constant it proves,
	// C=FIX at 128. The value never needed a carrier, and the As encoding must not
	// reach the renderer: the blend map realizes it through dual-source (SRC1)
	// factors, which the Mali blobs do not support (r44p1 reports no dual-source
	// blending), and an encoding-only SRC1 dependency would floor these rows there
	// — a readback event — for no arithmetic reason. The constant twin rows are
	// factor-for-factor identical at the proven value on every device. The read
	// rung keeps its raw carrier, for the reason given at its leg below.
	if (blend_pass)
	{
		// Rung 3's rows are C-degenerate and land as C=FIX at 128 (the #90 rule).
		// A mix row keeps its RAW selectors — the renderer owns the donor's
		// selector rewrite — except that a degenerate As still lands as the
		// constant it proves, extending #90's device-uniformity to this rung:
		// the FIX twin rows are factor-for-factor identical at the proven value,
		// and a constant never needs the dual-source carrier.
		const bool variable_carrier = in.ALPHA.C != 2;
		const bool degenerate_as = in.ALPHA.C == 0 && in.alpha_min == in.alpha_max;
		for (u32 i = 0; i < p.pass_count; i++)
		{
			p.pass[i].abe = true;
			p.pass[i].blend_mix = blend_mix;
			p.pass[i].rt_read = blend_read;
			p.pass[i].blend_a = static_cast<u8>(in.ALPHA.A);
			p.pass[i].blend_b = static_cast<u8>(in.ALPHA.B);
			p.pass[i].blend_d = static_cast<u8>(in.ALPHA.D);
			if (blend_read)
			{
				// Nothing is re-encoded on the read rung: the shader evaluates the
				// register's own carrier in integer arithmetic, where As, Ad and FIX
				// are all equally exact. The #90 constant rewrite exists to keep a
				// fixed-function row off the dual-source unit; there is no unit here
				// to keep it off, and rewriting Ad to a constant would be a lie.
				p.pass[i].blend_c = static_cast<u8>(in.ALPHA.C);
				p.pass[i].afix = static_cast<u8>(in.ALPHA.FIX);
			}
			else if (!blend_mix)
			{
				p.pass[i].blend_c = variable_carrier ? 2 : static_cast<u8>(in.ALPHA.C);
				p.pass[i].afix = variable_carrier ? 128 : static_cast<u8>(in.ALPHA.FIX);
			}
			else
			{
				// The mix carrier is a proven constant (admission) and lands as
				// FIX: the renderer feeds the blend constant, which carries the
				// number in full float precision — never the 8-bit transit a
				// second fragment output would impose.
				p.pass[i].blend_c = 2;
				p.pass[i].afix = static_cast<u8>(degenerate_as ? in.alpha_min : in.ALPHA.FIX);
			}
		}
	}

	p.native = true;
	return p;
}
