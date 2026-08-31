// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"
#include "GS/Renderers/Tile/GSPageBitmap.h"

// The jointly-owned contract header where the Tile renderer's three pillars meet: the
// memory model produces facts, the draw-lowering compiler consumes them and produces
// plans, the pass graph consumes plans. POD types and pure constexpr helpers only —
// no GS state, no device, no allocation, so every pillar stays unit-testable without
// a GPU.
//
// Growth discipline: adding a type or field is normal; changing the meaning or shape
// of an existing one is a counted contract rewrite (the M2 kill clause budgets two).

// The data planes of GS memory the model tracks independently. The color split
// mirrors the texture cache's proven target tracking: RGB is pixel bits 0-23,
// alpha-low is bits 24-27, alpha-high is bits 28-31 — FBMSK masks at bit granularity,
// and destination-alpha tests care exactly about bit 31 versus the rest. 16-bit
// formats carry alpha-low == alpha-high (one MSB feeds both nibbles' worth of state);
// 24-bit formats have no alpha planes at all; depth formats track the single Z plane.
enum GSTilePlane : u8
{
	GSTilePlaneRGB = 1 << 0,
	GSTilePlaneAlphaLow = 1 << 1,
	GSTilePlaneAlphaHigh = 1 << 2,
	GSTilePlaneZ = 1 << 3,
};

static constexpr u32 kGSTilePlaneCount = 4;
static constexpr u8 kGSTilePlanesAlpha = GSTilePlaneAlphaLow | GSTilePlaneAlphaHigh;
static constexpr u8 kGSTilePlanesColor = GSTilePlaneRGB | kGSTilePlanesAlpha;
static constexpr u8 kGSTilePlanesAll = kGSTilePlanesColor | GSTilePlaneZ;

/// Map GSUtil::GetChannelMask's 4-bit rgba mask onto planes. A whole-alpha-byte
/// writer (every transfer format with alpha) covers both alpha planes.
constexpr u8 gsTilePlanesFromChannelMask(u32 chan_mask)
{
	return static_cast<u8>(((chan_mask & 0x7) ? GSTilePlaneRGB : 0) |
						   ((chan_mask & 0x8) ? kGSTilePlanesAlpha : 0));
}

/// Planes a CPU-side write in format psm invalidates on the pages it covers. This is
/// BYTE coverage, not meaning: color and depth planes alias the same physical bytes,
/// so a CT32 write kills a Z surface's truth on the page even though it "writes
/// color". The only exemptions are formats that leave whole bytes untouched — 24-bit
/// writes never touch the alpha byte, and the byte-3 palette views never touch bytes
/// 0-2. The Z plane appears in every set because a depth reading of the cell overlaps
/// every byte any write can touch.
constexpr u8 gsTilePlanesInvalidatedByWrite(u32 psm)
{
	switch (psm)
	{
		case PSMCT24:
		case PSMZ24:
			return GSTilePlaneRGB | GSTilePlaneZ;
		case PSMT8H:
		case PSMT4HL:
		case PSMT4HH:
			return kGSTilePlanesAlpha | GSTilePlaneZ;
		default:
			return kGSTilePlanesAll;
	}
}

/// The color planes whose BYTES any of `claims` covers — what a surface's texture must
/// already hold correctly before a native draw may render into it and take those claims.
///
/// Claims and byte spans are not the same set, and the gap is load-bearing. A native
/// color draw always claims the Z plane alongside the channels its colormask writes
/// (byte coverage: written bytes invalidate any depth reading of them), and the Z plane
/// spans the whole 32-bit cell. So the surface becomes the page's newest-bytes holder
/// across every byte, while the GPU only wrote the masked-in channels — and a later
/// Z-plane readback hands the entire cell back to CPU memory. Whatever the draw did not
/// write must therefore have been current in the texture beforehand.
constexpr u8 gsTileColorPlanesSpannedBy(u8 claims)
{
	return (claims & GSTilePlaneZ) ? kGSTilePlanesColor : static_cast<u8>(claims & kGSTilePlanesColor);
}

/// A 4-bit rgba channel mask: bit 0 = R, 1 = G, 2 = B, 3 = A. The GS stores a 32-bit
/// frame cell one channel per byte, so this is also a byte mask over the cell, which is
/// what lets FBMSK map onto a raster pipeline's per-channel colour write mask.
static constexpr u8 kGSTileChannelsRGB = 0x7;
static constexpr u8 kGSTileChannelAlpha = 0x8;
static constexpr u8 kGSTileChannelsRGBA = 0xF;

/// Whether FRAME.FBMSK leaves every bit the frame format stores writable — the exact
/// "this draw overwrites the cell" precondition. Callers that need to prove a draw
/// covers a page in full (the seed skip) must ask THIS, not the channel mask below: a
/// channel the mask reports as written may still be preserving bits inside its byte.
constexpr bool gsTileFrameWriteIsTotal(u32 fbmsk, u32 fmsk)
{
	return (fbmsk & fmsk) == 0;
}

/// Whether FRAME.FBMSK preserves every bit the frame format stores — the draw lands no
/// colour at all, whatever its channel mask says.
constexpr bool gsTileFrameWritesNothing(u32 fbmsk, u32 fmsk)
{
	return (fbmsk & fmsk) == fmsk;
}

/// The colour channels a draw with this FBMSK lands, as a 4-bit rgba mask, one channel at
/// a time: a channel is preserved only where FBMSK covers EVERY BIT THE FORMAT STORES in
/// it, and written otherwise.
///
/// The format's bits, not the byte's — that is the whole difference for a 16-bit frame,
/// which keeps five bits per colour channel. PSMCT16 under FBMSK=0x000000F8 preserves the
/// entire red channel as far as guest memory is concerned, and a rule that asked for a
/// literal 0xFF byte would write red back over bits the GS keeps. The same rule settles
/// the all-or-nothing edge: a mask that keeps every stored bit writes no channel at all,
/// even where a byte outside the format reads as writable (PSMCT24 + FBMSK=0x00FFFFFF).
///
/// A channel the format stores NOTHING in — a 24-bit frame's alpha, an 8H frame's RGB — is
/// NOT written. The GS lands no bits there, so neither may we.
///
/// It used to ride along with the channels the format does store, on the premise that such
/// a byte was not guest data because writeback masks it off under the format's own byte
/// mask. Two things retire that premise. Writeback is not the image's only reader: a later
/// TCC=1 sample, a rule-2 target bind and the in-pass destination read all take the byte as
/// it stands, and a 24-bit draw's fragment alpha sitting in it is junk to every one of them.
/// And surface-identity containment puts a 24-bit view and a 32-bit one in ONE surface,
/// whose writeback carries the CONTAINER's byte mask — which does not mask that byte off, so
/// the junk reaches guest memory. On GT4 the byte in question is a PSMT8H palette index, so
/// the damage surfaces as wrong colours on draws unrelated to the one that wrote it.
///
/// The rule cannot silence a draw that still lands something. A mask empty under it is one
/// where every channel the format DOES store is fully masked, which is bit for bit the
/// gsTileFrameWritesNothing question asked above.
///
/// ⚠️ A channel the mask covers only PARTLY is reported as written — the whole channel
/// lands, and the bits inside it the GS would have kept are lost. A raster pipeline masks
/// at channel granularity and the GS masks at bit granularity, so the sub-channel cases
/// have no exact expression here; they are an approximation, rowed in the
/// deferred-accuracy ledger. The corpus population is small and cosmetic (a 1-LSB alpha
/// in Beyond Good & Evil, an alpha MSB in Xenosaga) next to the whole-byte population
/// this exists to serve, which is where the alpha-mask idiom lives.
constexpr u8 gsTileFrameWriteMask(u32 fbmsk, u32 fmsk)
{
	if (gsTileFrameWritesNothing(fbmsk, fmsk))
		return 0;
	u8 m = 0;
	for (u32 b = 0; b < 4; b++)
	{
		const u32 stored = fmsk & (0xFFu << (b * 8));
		if (stored != 0 && (fbmsk & stored) != stored)
			m |= static_cast<u8>(1u << b);
	}
	return m;
}

/// Whether gsTileFrameWriteMask is EXACT for this FBMSK — no byte is partly masked, so
/// the channel mask reproduces the GS's write bit for bit. Census and ledger use; the
/// route never branches on it (an inexact mask still writes, it just writes too much).
constexpr bool gsTileFrameWriteMaskIsExact(u32 fbmsk, u32 fmsk)
{
	if (gsTileFrameWritesNothing(fbmsk, fmsk))
		return true;
	for (u32 b = 0; b < 4; b++)
	{
		const u32 byte = (fbmsk >> (b * 8)) & 0xFFu;
		const u32 stored = (fmsk >> (b * 8)) & 0xFFu;
		if ((byte & stored) != 0 && (byte & stored) != stored)
			return false;
	}
	return true;
}

/// What the alpha test does to EVERY fragment of a draw, where the plan can decide that
/// before the draw runs. A test whose outcome is the same for all of them is not a
/// per-fragment test at all: it is a statement about what the draw writes, and belongs in
/// the write flags rather than in the fragment stage.
enum class GSTileAlphaTestFold : u8
{
	Varies = 0,  ///< the test can accept one fragment and reject the next: it has to be emitted
	AllPass = 1, ///< nothing is rejected — ATE off, ATST ALWAYS, or a comparison that provably holds
	AllFail = 2, ///< everything is rejected: AFAIL alone says what the draw still lands
};

/// Decide the alpha test at plan time, from the INTERVAL the draw's fragment alpha can
/// take. `alpha_min`/`alpha_max` are that interval inclusive, in 0..255; a caller that
/// cannot bound the alpha passes the whole range and gets Varies out of every comparison.
///
/// ATST=NEVER and ATST=ALWAYS decide themselves. What this adds is the SAME intent written
/// as a comparison that cannot come out either way — which games do write, and which
/// costs a whole character when it is read as a live test instead.
///
/// An interval rather than a single value, because the draws that need this most are
/// TEXTURED. Katamari Damacy's is the easy shape: it composites the King's head out of two
/// sprites over one rectangle, the first stamping a lower Z into it, the second drawing the
/// head with ZTST=GEQUAL against that Z. The stamp is untextured with vertex alpha 0x00,
/// asks for ATST=NOTEQUAL AREF=0 — "0 != 0" is false, so every fragment fails — and carries
/// AFAIL=ZB_ONLY, "a failing fragment still writes depth". One alpha value decides it.
///
/// R&C UYA's exhaust flare is the shape a single value CANNOT decide, and it is a whole
/// effect rather than a detail. The flare is one additively-blended 351-triangle strip
/// sampling a paletted glow texture under ATST=GEQUAL AREF=128 AFAIL=RGB_ONLY — "a fragment
/// that fails still writes its RGB, it just must not touch the alpha byte or depth", the
/// standard idiom for an additive effect that must not disturb the frame's alpha or Z. Its
/// palette tops out at alpha 128 and its vertex alpha at 64, so its MODULATE output tops out
/// at (128 × 64) >> 7 = 64: no fragment can reach the reference. Decided as an interval the
/// draw folds to AllFail, RGB_ONLY paints the flare and drops the alpha and depth writes;
/// read as a live test the fragment stage discards every fragment and the flare is gone.
///
/// ⚠️ The comparisons are the software renderer's (GSState::TryAlphaTest's GetResult), the
/// shared Tile lowering's (GSTileDrawLowering.h) and tilegpu.glsl's fragment stage, which
/// are the same list. All of them have to stay the same list; a boundary that drifts here
/// changes some other game's draw and nothing says so.
constexpr GSTileAlphaTestFold gsTileFoldAlphaTest(bool ate, u32 atst, u32 aref, u32 alpha_min, u32 alpha_max)
{
	using Fold = GSTileAlphaTestFold;
	if (!ate || atst == ATST_ALWAYS)
		return Fold::AllPass;
	if (atst == ATST_NEVER)
		return Fold::AllFail;
	if (alpha_min > alpha_max)
		return Fold::Varies; // an interval that says nothing decides nothing

	const u32 lo = alpha_min;
	const u32 hi = alpha_max;
	switch (atst)
	{
		case ATST_LESS:
			return (hi < aref) ? Fold::AllPass : ((lo >= aref) ? Fold::AllFail : Fold::Varies);
		case ATST_LEQUAL:
			return (hi <= aref) ? Fold::AllPass : ((lo > aref) ? Fold::AllFail : Fold::Varies);
		case ATST_EQUAL:
			return (lo == aref && hi == aref) ? Fold::AllPass :
												((aref < lo || aref > hi) ? Fold::AllFail : Fold::Varies);
		case ATST_GEQUAL:
			return (lo >= aref) ? Fold::AllPass : ((hi < aref) ? Fold::AllFail : Fold::Varies);
		case ATST_GREATER:
			return (lo > aref) ? Fold::AllPass : ((hi <= aref) ? Fold::AllFail : Fold::Varies);
		case ATST_NOTEQUAL:
			return (aref < lo || aref > hi) ? Fold::AllPass :
											  ((lo == aref && hi == aref) ? Fold::AllFail : Fold::Varies);
		default:
			return Fold::Varies;
	}
}

/// The GS blend equation packed for a fragment stage that evaluates it itself, out of a draw's
/// state row. The console computes, per colour channel and in integer,
///
///     Cv = ((A - B) * C) >> 7 + D
///
/// with A, B, D selecting Cs / Cd / zero, and C a plain byte in which 0x80 is 1.0 (so 0xFF reaches
/// 1.99, which is exactly what a fixed-function factor cannot express). Read off the software
/// renderer's scanline, which is the oracle for this: its `modulate16<1>` is a left-shift-by-2 and
/// a signed high multiply against `alpha << 7`, i.e. an arithmetic `(x * C) >> 7`.
///
/// ⚠️ What rides in the row is NOT the register's three selectors but the COEFFICIENTS they add up
/// to, and that is a size decision, not a style one. A - B is linear in Cs and Cd with coefficients
/// in {-1, 0, +1}, and D is one of the two or zero, so the whole equation is
///
///     Cv = ((ka*Cs + kb*Cd) * C) >> 7 + ds*Cs + dd*Cd
///
/// which the fragment stage evaluates with no selector chains at all. Spelt as selectors it cost
/// 828 SPIR-V words on the largest single-geometry variant, against a budget with 556 words of
/// headroom; the Adreno 650's instruction-size cliff is what makes that the difference between a
/// road and no road.
///
/// The same packing swallows the two shapes that are not the closed form:
///   - A == B leaves ka and kb both zero, so the multiply contributes nothing and Cv is D. (Which
///     is also why the expressibility classifier can gate on A != B before it looks at anything.)
///   - A **24-bit destination** has no alpha byte, and the console takes C = Ad there as exactly
///     1.0 rather than as whatever the unstored byte holds (the scanline skips the multiply
///     outright). The packer turns that into the constant factor 0x80, and 0x80 >> 7 is 1 exactly.
constexpr u32 kGSTileBlendKaShift = 0;    ///< coefficient of Cs in (A - B), biased by +1
constexpr u32 kGSTileBlendKbShift = 2;    ///< coefficient of Cd in (A - B), biased by +1
constexpr u32 kGSTileBlendDsBit = 1u << 4;  ///< D is Cs
constexpr u32 kGSTileBlendDdBit = 1u << 5;  ///< D is Cd
constexpr u32 kGSTileBlendCShift = 6;     ///< 0 = As, 1 = Ad, 2 = the constant below
constexpr u32 kGSTileBlendFixShift = 8;   ///< the constant factor, when C says so
constexpr u32 kGSTileBlendEnable = 1u << 16; ///< blend at all; clear means Cv = Cs
constexpr u32 kGSTileBlendWrap = 1u << 17;   ///< COLCLAMP = 0: the result wraps mod 256 instead of clamping
constexpr u32 kGSTileBlendPabe = 1u << 18;   ///< PABE: the blend applies only where the source alpha's bit 7 is set
/// Bit 20: quantise this draw's output to what a 16-bit frame stores, before anything reads it back.
/// Not part of the blend equation -- it is what the console does at the WRITE -- but it rides in the
/// same word because it is the same per-draw decision the fragment stage takes at the same point.
constexpr u32 kGSTileBlendQuant16 = 1u << 20;
/// Bit 21: this draw's alpha test VARIES per fragment under AFAIL=RGB_ONLY, and it writes no depth,
/// so a fragment that fails must land its RGB and keep the destination's alpha rather than being
/// discarded. A per-FRAGMENT write mask, which is why it rides the same arm the bit-granular FBMSK
/// merge does; see gsTileGpuAfailKeepsAlpha for why the depth clause is not a conservatism.
constexpr u32 kGSTileBlendAfailKeepAlpha = 1u << 21;
/// Bit 22: a device with no dual-source blending has no second colour output to hand the As blend
/// factor min(As * 255/128, 1) to, so o_color.a CARRIES it and the blend unit reads it as SRC_ALPHA.
/// Written at the very end of the fragment stage, below the byte tail, because that is where the
/// blend unit would have taken it -- and the alpha the draw would have stored is either masked off
/// by the pipeline, given back by the alpha blend equation, or written by a companion draw. Which
/// of the three is GSDevice::GSTileGpuDualSrcRoad's answer.
constexpr u32 kGSTileBlendAlphaCarrier = 1u << 22;

/// Whether a draw's failing fragments can be served exactly by keeping the destination's alpha.
///
/// ⚠️ The interesting part is the DEPTH clause, and it is not caution -- it is the whole boundary.
/// Under AFAIL=RGB_ONLY a failing fragment lands its RGB, keeps the destination's alpha and does
/// NOT write depth. Discarding it, which is what this renderer does today, gets two of those three
/// right by accident: no alpha write and no depth write. The single error is the missing RGB.
///
/// So writing that RGB is only free where the draw writes no depth ANYWAY. On a depth-writing draw,
/// keeping the fragment alive to land its colour also sends it through the depth stage, and the
/// console does not let it write there -- per-fragment depth masking, which a colour read cannot do
/// and gl_FragDepth would buy at the cost of early-Z for the whole pass. Serving those would trade a
/// missing colour for a wrong depth, which is not a repair.
///
/// The corpus splits almost entirely the wrong way for us: of ~1258 varying non-KEEP AFAIL draws a
/// frame, 1012 are RGB_ONLY and all but 18 of those write depth. Eighteen a frame, over three dumps,
/// is what this serves; the rest is rowed.
constexpr bool gsTileGpuAfailKeepsAlpha(GSTileAlphaTestFold fold, u32 afail, bool depth_write)
{
	return fold == GSTileAlphaTestFold::Varies && afail == AFAIL_RGB_ONLY && !depth_write;
}

/// The GS alpha comparison that accepts exactly the fragments this one rejects.
constexpr u8 gsTileInvertAtst(u8 atst)
{
	constexpr u8 inverted[8] = {ATST_ALWAYS, ATST_NEVER, ATST_GEQUAL, ATST_GREATER, ATST_NOTEQUAL,
		ATST_LESS, ATST_LEQUAL, ATST_EQUAL};
	return inverted[atst & 7];
}

/// Why a draw whose alpha test genuinely varies under a non-KEEP AFAIL could not be split at all,
/// for the census. A refused draw is the only one left on the discarding road.
enum class GSTileAlphaSplitRefusal : u8
{
	None = 0,
	/// ZTST=ALWAYS with a depth write. Both split shapes give one of their two passes no depth
	/// write, and with no depth TEST either that pass carries no depth ATTACHMENT -- which its
	/// partner needs, and which a render pass cannot gain or lose in the middle. There is no
	/// "test always, write nothing" depth variant to lend it.
	NoDepthTest,
	/// AFAIL=FB_ONLY, refused on POLICY rather than on impossibility (EmuCore/GS/TileGpuSplitRefuseFbOnly).
	/// This class's second half writes no colour byte at all -- it exists to carry the depth write --
	/// so it re-rasterizes the whole draw's fill for a depth write, and it is also the class whose
	/// first half paints the WHOLE colour with no test, which is where MGS3's and AC3's regressions
	/// come from. See the key.
	RefusedFbOnly,
};

/// One pass of a draw the alpha test splits: the channels it writes, whether it writes depth, and
/// the comparison it runs. `atst == ATST_ALWAYS` means no test at all.
struct GSTileAlphaSplitPass
{
	u8 color_mask;
	bool z_write;
	u8 atst;
	u8 aref;
};

/// How a draw's alpha test is realized: one pass (`pass[0]` is the draw as it always was) or two.
struct GSTileAlphaSplit
{
	u8 pass_count;
	GSTileAlphaSplitPass pass[2];
	GSTileAlphaSplitRefusal refusal;
	/// The two passes land exactly the channels the console lands for every fragment -- that is the
	/// contract, and both split shapes keep it. This says the ORDER they land in is not the
	/// console's: the draw's own primitives overlap in a way the proof below could not rule out, so
	/// the failing fragments arrive after the passing ones rather than interleaved with them. It is
	/// the approximation Classic ships as PASS_THEN_FAIL, and it is strictly less wrong than the
	/// discard it replaces, which drops those fragments outright.
	bool reorders_fragments;
	/// Whether the ONE-DRAW road -- a live test answered by discarding the fragments that fail it --
	/// drops a write the console makes for this draw. Computed whatever the lever says, because it
	/// is the population the road exists for and an OFF arm has to report the same number an ON arm
	/// does or the two are not comparable.
	bool discards_a_write;
};

/// How to realize an alpha test the fold could not decide, under an AFAIL that still writes
/// something when a fragment fails.
///
/// ⚠️ THE DEFECT THIS EXISTS TO CLOSE. A raster fragment stage answers a live alpha test by
/// DISCARDING the failing fragment, which is exact for AFAIL=KEEP and wrong for the other three:
/// under RGB_ONLY the console paints the failing fragment's colour and drops only its alpha and
/// depth writes, under FB_ONLY it paints the whole colour and drops the depth write, under ZB_ONLY
/// it writes depth and drops the colour. A discard drops all of it. So "Varies" is not the
/// conservative answer to "what is this draw's alpha test" — it is a SECOND APPROXIMATION, and the
/// caller that widens an alpha bound it cannot compute walks straight into it. That is how LEGO
/// Star Wars lost both of its floor-reflection draws (48,590 of 52,408 diverging pixels) the day
/// the CLUT gather started leaving palettes on the device.
///
/// The exact realization needs nothing from the device: no dual-source blending, no in-pass
/// destination read. It is a DRAW SPLIT, and it is the one Classic ships as `split_rgb_only`
/// (GSRendererHW.cpp) and the shared Tile lowering ships as `atst_split` (GSTileDrawLowering.h).
/// Under RGB_ONLY every fragment writes RGB and only the passing ones write A and Z, so one pass
/// with the test OFF writing RGB and one with the test ON writing A and Z is exact — and strictly
/// better than splitting by pass/fail, which would put RGB in both passes and composite
/// overlapping primitives out of order. FB_ONLY is the same split with the whole colour in the
/// first pass; ZB_ONLY is the one that cannot be split by channel, because the depth its first
/// pass writes is what the second would then test against, so it splits by FRAGMENT on the
/// inverted comparison instead.
///
/// Both halves of the ORDER PROOF are Classic's (`independent_z` / `independent_rgb`) and both ask
/// about the draw's OWN fragments, because a split walks them twice:
///
///  - `independent_z`. The colour pass writes no depth, so a fragment it lands is tested against
///    the depth the draw FOUND rather than against what an earlier primitive of the same draw
///    wrote. It holds when nothing writes depth, when the test is ALWAYS, when every vertex sits
///    at one depth under GEQUAL (rewriting a pixel with the depth it already holds still passes),
///    or when no two of the draw's primitives touch a pixel.
///  - `independent_colour`. The alpha byte moves to the second pass, so anything reading
///    DESTINATION ALPHA in the first — a C=Ad blend factor, and on this renderer the DATE test —
///    reads what the draw found rather than what an earlier primitive of the same draw wrote.
///
/// ⚠️ AND WHERE THE PROOF FAILS THE SPLIT STILL HAPPENS. It is labelled `reorders_fragments` and
/// nothing else changes, because the alternatives were measured and this is the least wrong of
/// them. The proof cannot be met on a triangle draw -- the overlap test answers YES or UNKNOWN for
/// every one of them on the whole corpus -- and LEGO Star Wars' floor reflection is a triangle
/// draw, so a road that refused on it would be a road that does not fix the defect it was written
/// for. Whole-frame mean absolute error against Classic, four frames, this arm vs refusing:
/// LEGO Star Wars 8.377 -> 1.764, R&C UYA gameplay 5.088 -> 4.717, FlatOut 2 12.381 -> 11.140,
/// R&C UYA effects 7.561 -> 7.208, SotC 6.248 -> 6.190, dirge and GoW II and GT4 smaller, ac3
/// 0.3869 -> 0.3884. ⚠️ MGS3 is the one that goes the other way, 5.455 -> 8.011, and it is booked:
/// 44.5 FB_ONLY triangle draws a frame whose own primitives occlude each other, so the colour pass
/// paints fragments a later primitive of the same draw would have hidden. Two other realizations
/// were measured and are worse than this one on every title including MGS3 -- Classic's pass/fail
/// (MGS3 14.772, SotC 7.540, LEGO 4.400) and the same channel split with the depth pass emitted
/// first (MGS3 15.390, LEGO 4.484). The refusal is kept for the one shape no split expresses at
/// all, and `reorders_fragments` is what a future per-draw rule for MGS3's population hangs off.
///
/// ⚠️ AND ONE CLASS IS REFUSED ON POLICY, not on impossibility: the REGISTER's AFAIL=FB_ONLY, under
/// `refuse_fb_only` (EmuCore/GS/TileGpuSplitRefuseFbOnly, default on). It is the only scoping on
/// the table that makes speed AND accuracy better at once. Its second half writes no colour byte at
/// all -- it re-rasterizes the draw's whole fill to carry a depth write -- and its first half paints
/// the WHOLE colour with no test, which over-paints wherever the draw's own primitives occlude each
/// other. That over-paint IS the MGS3 regression this file books above (6.617 -> 11.926 over 122,611
/// px a frame) and AC3's (exact agreement 49.5% -> 2.44%), and both close when the class is refused.
/// What it gives back is FlatOut 2's 19.809 -> 14.513 and SotC's 4.551 -> 4.213 over their changed
/// masks. RGB_ONLY and ZB_ONLY keep splitting -- including the RGB_ONLY draw whose alpha the write
/// mask already took, which the algebra below calls FB_ONLY and which is LEGO Star Wars' floor
/// reflection. The predicate reads the register for that one reason and the refusal says so.
///
/// `color_mask` and `z_write` are the draw's own, after FBMSK and after the fold's AFAIL edit; the
/// two returned passes always union back to them.
constexpr GSTileAlphaSplit gsTileGpuPlanAlphaSplit(GSTileAlphaTestFold fold, u32 atst, u32 aref, u32 afail,
	u8 color_mask, bool z_write, bool z_test, bool independent_z, bool independent_colour, bool lever,
	bool refuse_fb_only)
{
	const bool varies = (fold == GSTileAlphaTestFold::Varies);
	// The one-draw answer first, and it is today's unchanged: the draw's own mask and depth write,
	// carrying a live test only where the two sides of it land differently at all.
	bool live_test = false;
	if (varies)
	{
		switch (afail)
		{
			case AFAIL_FB_ONLY: live_test = z_write; break;
			case AFAIL_ZB_ONLY: live_test = color_mask != 0; break;
			default: live_test = true; break; // KEEP writes nothing on failure; RGB_ONLY drops alpha
		}
	}
	GSTileAlphaSplit s{1,
		{{color_mask, z_write, static_cast<u8>(live_test ? atst : ATST_ALWAYS),
			 static_cast<u8>(live_test ? aref : 0u)},
			{}},
		GSTileAlphaSplitRefusal::None, false, false};
	if (!varies || afail == AFAIL_KEEP)
		return s; // under KEEP the discard IS what the console does

	// What a FAILING fragment still writes, after the register algebra that turns an AFAIL edit
	// removing a write the draw does not make into no edit at all. The shared Tile lowering makes
	// the same three simplifications, in the same order.
	u32 af = afail;
	if (af == AFAIL_RGB_ONLY && !(color_mask & kGSTileChannelAlpha))
		af = AFAIL_FB_ONLY; // no alpha write to suppress
	const bool fail_writes_same =
		(af == AFAIL_FB_ONLY && !z_write) || (af == AFAIL_ZB_ONLY && color_mask == 0);
	const bool fail_writes_nothing =
		(af == AFAIL_FB_ONLY && color_mask == 0) ||
		(af == AFAIL_RGB_ONLY && !(color_mask & kGSTileChannelsRGB)) ||
		(af == AFAIL_ZB_ONLY && !z_write);
	// The one-draw road drops a write only where it actually emits a discarding test AND the
	// failing fragment still had something to write. A draw whose two sides write the same channels
	// never got a test in the first place (FB_ONLY with no depth write, ZB_ONLY with no colour), so
	// it is not in this population -- OutRun 2006's 19.88 varying draws a frame are all that shape.
	s.discards_a_write = live_test && !fail_writes_nothing;
	if (!lever)
		return s;
	if (fail_writes_same)
	{
		// Passing and failing fragments write the same channels, so the test decides nothing and
		// the DISCARD is the only thing it can still do — which is exactly the wrong thing. It goes
		// away entirely, for one draw and no split. (The population is an RGB_ONLY draw whose alpha
		// channel is masked off and which writes no depth: the shape gsTileGpuAfailKeepsAlpha
		// serves where a destination read is admitted, and nothing serves where it is not.)
		s.pass[0].atst = ATST_ALWAYS;
		s.pass[0].aref = 0;
		return s;
	}
	if (fail_writes_nothing)
		return s; // the discard is exact

	if (z_write && !z_test)
	{
		// One pass of either split shape writes no depth, and with no depth TEST either it carries
		// no depth attachment at all. Nothing here can express that, so this one draw keeps the
		// discard -- the only case that does.
		s.refusal = GSTileAlphaSplitRefusal::NoDepthTest;
		return s;
	}
	if (afail == AFAIL_FB_ONLY && refuse_fb_only)
	{
		// The one class the split is refused on POLICY. Two costs, one predicate. The second half
		// writes NO colour byte (pass[1] is {0, z_write}), so it re-rasterizes the draw's whole fill
		// to carry a depth write and pays the full fragment bill for it -- SotC +1.00 ms and MGS3
		// +2.19 ms of SD865 frame time. And the first half paints the WHOLE colour with no test at
		// all, which over-paints wherever the draw's own primitives occlude each other: that is
		// MGS3's 6.617 -> 11.926 mean-abs against Classic and AC3's 49.5% -> 2.44% exact agreement,
		// the split round's only two wrong-way titles. Refusing hands the draw back to the discarding
		// road, which loses the failing fragment's colour -- FlatOut 2's 19.809 -> 14.513 and SotC's
		// 4.551 -> 4.213 are what that costs.
		//
		// ⚠️ ASKED OF THE REGISTER'S AFAIL, NOT OF `af`, AND THAT IS NOT A SLIP. The two differ on
		// exactly one shape -- an AFAIL=RGB_ONLY draw whose alpha the write mask already took, which
		// the algebra above rewrites to FB_ONLY -- and that shape is LEGO STAR WARS' FLOOR
		// REFLECTION, the draw this whole road was written for (FBMSK=0xFF000000, depth-writing,
		// 8.543 -> 1.764 whole-frame mean-abs against Classic). Refusing on `af` takes it back out,
		// which is a 4.8x accuracy loss on the corpus's largest repair for no measured device time:
        // legosw is 11 splits a frame, not 124. The two classes are structurally IDENTICAL here
		// (whole colour in an untested first pass, depth alone in the second), so there is no
		// derivation that separates them -- the register is the only thing that does, and which side
		// of it a title falls on was MEASURED, per title, in the split's price record.
		s.refusal = GSTileAlphaSplitRefusal::RefusedFbOnly;
		return s;
	}

	// A draw that writes no depth has no depth ordering to lose, whatever the caller could prove
	// about the clauses it can see. Folded in here rather than left to every caller, because a
	// caller that forgets it mislabels draws that were never at risk.
	const bool z_order_ok = independent_z || !z_write;

	s.pass_count = 2;
	const u8 atst8 = static_cast<u8>(atst);
	const u8 aref8 = static_cast<u8>(aref);
	if (af == AFAIL_ZB_ONLY)
	{
		// By FRAGMENT, and it is the only road for this mode: the depth a channel split's first
		// pass writes is what its second would then test against.
		s.reorders_fragments = !z_order_ok;
		s.pass[0] = {color_mask, z_write, atst8, aref8};
		s.pass[1] = {0, z_write, gsTileInvertAtst(atst8), aref8};
	}
	else if (af == AFAIL_RGB_ONLY)
	{
		// By CHANNEL: RGB for every fragment, then what only a passing fragment may write.
		s.reorders_fragments = !(z_order_ok && independent_colour);
		s.pass[0] = {static_cast<u8>(color_mask & kGSTileChannelsRGB), false, ATST_ALWAYS, 0};
		s.pass[1] = {static_cast<u8>(color_mask & kGSTileChannelAlpha), z_write, atst8, aref8};
	}
	else // AFAIL_FB_ONLY
	{
		// By channel again: the whole colour for everyone, depth for the fragments that pass.
		s.reorders_fragments = !z_order_ok;
		s.pass[0] = {color_mask, false, ATST_ALWAYS, 0};
		s.pass[1] = {0, z_write, atst8, aref8};
	}
	return s;
}

/// The source-alpha bound the blend classifier may use for a C=As draw.
///
/// The classifier asks whether the blend factor can exceed 1, which is a question about the draw's
/// POST-texture-function alpha, and on a palettised draw the answer is folded out of the CPU's
/// decoded CLUT. The CLUT gathers leave that RAM stale for every slot a device palette wrote, so a
/// scan of it answers off the WRONG palette. Where the mirror says the draw's own slots are not
/// CPU-current the bound therefore widens to the whole range — which is what Classic does with the
/// same fact (GSRendererHW::CalcAlphaMinMax(0, 255) when the CLUT has a GPU texture).
///
/// ⚠️ Widening is sound HERE and unsound at the alpha-test fold, and the difference is what the two
/// fallbacks DO. This one admits the draw to a destination read it may not need: it costs a read and
/// never a pixel. The fold's fallback hands the draw a live DISCARDING test, which under an AFAIL
/// that still writes something drops writes the console makes — so that site owes a sound road for
/// a varying test before it may widen, and this one does not.
constexpr u32 gsTileGpuReaderSourceAlphaMax(bool palettised, bool clut_cpu_current, u32 scanned_max)
{
	return (palettised && !clut_cpu_current) ? 255u : scanned_max;
}

/// Pack the ALPHA register (plus COLCLAMP, PABE and the frame format) into the row. `fmt` is
/// `GSLocalMemory::m_psm[FRAME.PSM].fmt` -- 0 = 32-bit, 1 = 24-bit, 2 = 16-bit.
constexpr u32 gsTileGpuPackBlend(bool abe, u32 a, u32 b, u32 c, u32 d, u32 fix, bool colclamp, bool pabe, u32 fmt)
{
	int ka = 0, kb = 0;
	if (a == 0)
		ka++;
	else if (a == 1)
		kb++;
	if (b == 0)
		ka--;
	else if (b == 1)
		kb--;

	// C = Ad on a destination that stores no alpha is exactly 1.0, which is the constant 0x80.
	u32 c_mode = c;
	u32 c_fix = fix;
	if (c == 1 && fmt == 1)
	{
		c_mode = 2;
		c_fix = 0x80;
	}

	u32 v = (static_cast<u32>(ka + 1) << kGSTileBlendKaShift) | (static_cast<u32>(kb + 1) << kGSTileBlendKbShift) |
			((c_mode & 3u) << kGSTileBlendCShift) | ((c_fix & 0xFFu) << kGSTileBlendFixShift);
	if (d == 0)
		v |= kGSTileBlendDsBit;
	else if (d == 1)
		v |= kGSTileBlendDdBit;
	if (abe)
		v |= kGSTileBlendEnable;
	if (!colclamp)
		v |= kGSTileBlendWrap;
	if (pabe)
		v |= kGSTileBlendPabe;
	return v;
}

/// One colour channel of the blend, evaluated from the packed row exactly as the fragment stage
/// evaluates it. Held against gsTileGpuBlendChannel below, which is held against the scanline --
/// so the chain from the software renderer to the shader is closed by two unit tests rather than
/// by a reading of two files side by side.
constexpr int gsTileGpuBlendChannelPacked(int cs, int cd, u32 blend, int as, int ad)
{
	const int ka = static_cast<int>((blend >> kGSTileBlendKaShift) & 3u) - 1;
	const int kb = static_cast<int>((blend >> kGSTileBlendKbShift) & 3u) - 1;
	const u32 c_mode = (blend >> kGSTileBlendCShift) & 3u;
	const int C = (c_mode == 0) ? as : ((c_mode == 1) ? ad : static_cast<int>((blend >> kGSTileBlendFixShift) & 0xFFu));
	const int D = ((blend & kGSTileBlendDsBit) ? cs : 0) + ((blend & kGSTileBlendDdBit) ? cd : 0);
	const int v = (((ka * cs + kb * cd) * C) >> 7) + D;
	// COLCLAMP as ONE clamp rather than two paths, which is how the fragment stage spells it:
	// clamping to 0..255 makes the mask a no-op, and widening the clamp past the equation's own
	// range (which is [-510, 765]) makes the mask the wrap.
	const int lo = (blend & kGSTileBlendWrap) ? -1024 : 0;
	const int hi = 255 - lo;
	return ((v < lo) ? lo : ((v > hi) ? hi : v)) & 0xFF;
}

/// One colour channel of the GS blend in the register's own terms -- the SEMANTIC model, the thing
/// the software renderer is compared against. The packed form above is what actually runs; this is
/// what says what it means.
///
/// `a_sel`/`b_sel`/`d_sel` are the ALPHA register's encodings (0 = Cs, 1 = Cd, 2 = zero); `c_sel` is
/// 0 = As, 1 = Ad, 2 = FIX. `dest24` says the destination stores no alpha byte.
constexpr int gsTileGpuBlendChannel(
	int cs, int cd, u32 a_sel, u32 b_sel, u32 c_sel, u32 d_sel, int as, int ad, int fix, bool colclamp, bool dest24)
{
	const int A = (a_sel == 0) ? cs : ((a_sel == 1) ? cd : 0);
	const int B = (b_sel == 0) ? cs : ((b_sel == 1) ? cd : 0);
	const int D = (d_sel == 0) ? cs : ((d_sel == 1) ? cd : 0);
	int v;
	if (dest24 && c_sel == 1)
	{
		v = (A - B) + D;
	}
	else
	{
		const int C = (c_sel == 0) ? as : ((c_sel == 1) ? ad : fix);
		v = (((A - B) * C) >> 7) + D;
	}
	if (!colclamp)
		return v & 0xFF;
	return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

/// What TEST.DATE means for a draw, once the frame format has had its say.
///
/// The destination-alpha test passes a pixel when the destination alpha's bit 7 equals DATM. On a
/// 24-bit frame there is no alpha bit at all, and the console's answer is NOT "everything passes":
/// ⚠️ **nothing passes — the draw is ignored**, for BOTH values of DATM. That was measured on a PS2
/// and it is what the software renderer does (`GSRendererSW::GetScanlineGlobalData`, "When the
/// format is 24bit (Z or C), DATE ceases to function"); a renderer that reasoned its way to
/// "no alpha means no test" would draw the whole thing.
///
/// `trbpp` is `GSLocalMemory::m_psm[FRAME.PSM].trbpp`. It is the right question to ask because the
/// two 24-bit frame formats are PSMCT24 and PSMZ24 and those are exactly the two the software
/// renderer's own `(PSM & 0xF) == PSMCT24` names.
enum class GSTileDateFold : u8
{
	Off = 0,           ///< no destination-alpha test
	PassOnAlphaClear,  ///< DATM 0: the pixel passes where the destination alpha's bit 7 is clear
	PassOnAlphaSet,    ///< DATM 1: ...and where it is set
	DropDraw,          ///< a 24-bit frame: nothing passes, whatever DATM says
};

constexpr GSTileDateFold gsTileFoldDate(bool date, u32 datm, u32 trbpp)
{
	if (!date)
		return GSTileDateFold::Off;
	if (trbpp == 24)
		return GSTileDateFold::DropDraw;
	return (datm != 0) ? GSTileDateFold::PassOnAlphaSet : GSTileDateFold::PassOnAlphaClear;
}

/// The fold as the state row carries it: 0 off, 1 = pass on alpha bit 7 clear, 2 = on set. DropDraw
/// never reaches a row -- the draw does not happen -- so it is the caller's job to check for it
/// first, and the value here is the one that lands if it does not.
constexpr u32 gsTileGpuDateRow(GSTileDateFold fold)
{
	return (fold == GSTileDateFold::PassOnAlphaClear) ? 1u : ((fold == GSTileDateFold::PassOnAlphaSet) ? 2u : 0u);
}

/// What a CT16/CT16S frame actually stores, applied to a target's expanded RGBA8 bytes.
///
/// A TileGpu colour target is an RGBA8 image whatever the guest format is, so a 16-bit frame's
/// pixels live there expanded: five bits per colour channel at the top of a byte, one alpha bit as
/// 0x80. Everything that puts bytes INTO such a target already agrees on that -- the writeback packs
/// `byte >> 3` and the seed unpacks `bits << 3`, and the software renderer's WriteFrame16 truncates
/// the same way (`fs & 0x00f800f8` / `& 0x8000f800`) -- but the DRAW did not: it wrote a full eight
/// bits per channel, so the image held precision the console never stored, and everything that read
/// the target back (a destination blend, DATE, a TCC=1 sample) saw it.
///
/// ⚠️ TRUNCATION, not rounding. The console drops the low bits; it does not round to the nearest
/// representable value. The one rounding step in the chain is float -> byte, which happens before
/// this and is the UNORM8 store's own.
///
/// ⚠️ This is the WRITE. A draw whose result the blend unit still has to touch must NOT be quantised
/// here -- the console quantises the blend's OUTPUT, not its source -- which is why the state row's
/// bit is set per draw rather than per target.
constexpr u32 gsTileGpuQuantise5551(u32 rgba8)
{
	return rgba8 & 0x80F8F8F8u;
}

/// Whether a frame format stores fewer bits than the target's RGBA8 image holds, so a draw landing
/// in it has to say so. `fmt` is `GSLocalMemory::m_psm[FRAME.PSM].fmt`: 2 is the 16-bit family.
constexpr bool gsTileGpuFrameQuantises(u32 fmt)
{
	return fmt == 2;
}

/// FBMSK as a KEEP mask on the target's expanded RGBA8 bytes: the bits of the destination the draw
/// must leave alone, per channel, at BIT granularity.
///
/// It is just `FBMSK & fmsk` for every frame format, and that is worth stating because the 16-bit
/// case looks like it should need work and does not. The console packs FBMSK by the very same 5551
/// packing it packs the colour with, so bit k of a stored 5-bit channel is bit k of the byte the
/// expanded target holds; `fmsk` (0x80F8F8F8 for PSMCT16) already names exactly those bits, and the
/// bits it drops are the ones the format does not store and nothing may keep.
constexpr u32 gsTileGpuFrameKeepMask(u32 fbmsk, u32 fmsk)
{
	return fbmsk & fmsk;
}

/// Whether the draw's depth write survives its own alpha test. ZTE and ZMSK have the final
/// word; on top of them, a draw every fragment of which fails writes depth only in the one
/// AFAIL mode that says a failing fragment still does.
constexpr bool gsTileDepthWriteSurvives(bool zte, bool zmsk, bool atst_all_fail, u32 afail)
{
	return zte && !zmsk && !(atst_all_fail && afail != AFAIL_ZB_ONLY);
}

/// Whether every fragment of the draw lands its colour — the alpha test's half of "this
/// sprite provably overwrites the page", which is what lets the seed skip drop the bytes
/// that were there before. FBMSK's half is gsTileFrameWriteIsTotal, and both must hold.
///
/// ⚠️ AFAIL_RGB_ONLY is counted here as it always has been, and on a 32-bit frame it does
/// leave the alpha byte alone — so such a draw covers the page's colour but not its alpha.
/// Pre-existing, and the FBMSK half is what keeps it harmless today: the corpus's only
/// all-fail RGB_ONLY sprites are OutRun's eight, all at FBMSK=0xFF000000, which
/// gsTileFrameWriteIsTotal refuses before this is even asked. Named rather than quietly
/// changed, because changing it moves pixels on a draw nothing has measured.
///
/// Re-measured when the fold widened from a constant alpha to an interval, which is exactly
/// the change that could have populated this: over the whole 18-dump corpus every draw that
/// reaches the seed skip with an all-fail verdict is ATST=NEVER + AFAIL=FB_ONLY (SotC 227
/// a frame, OutRun 8 and 4, MGS3 2) — the population that was already there. The interval
/// adds none, so the carve-out above is unchanged rather than merely still unmeasured.
constexpr bool gsTileColorLandsOnEveryFragment(GSTileAlphaTestFold fold, u32 afail)
{
	if (fold == GSTileAlphaTestFold::AllPass)
		return true;
	if (fold == GSTileAlphaTestFold::AllFail)
		return afail == AFAIL_FB_ONLY || afail == AFAIL_RGB_ONLY;
	return false;
}

/// The colour write mask a draw actually gets: the FBMSK channels above, with the alpha
/// test's AFAIL fold on top.
///
/// The two derivations COMPOSE, neither overrides the other. An alpha test every fragment
/// fails never reaches the fragment stage, so AFAIL alone says what a failing pixel still
/// writes; FBMSK then says which of those channels the register lets through. Reading
/// only one of the two is the bug this function exists to prevent: OutRun's world-erasing
/// sprites are ATST NEVER + AFAIL FB_ONLY ("write the frame buffer") carrying
/// FBMSK=0x00FFFFFF ("but only its alpha byte").
///
/// `atst_all_fail` is gsTileFoldAlphaTest's AllFail. It was ATST=NEVER alone when this was
/// written; a comparison that provably fails is the same statement and takes the same road.
///
/// FBMSK's ALPHA byte is honoured per channel like its RGB half — the channel is dropped
/// exactly where the mask covers every bit the frame format stores in it. It was deferred
/// (Ace Combat 5's white band across the sky, measured 2026-08-22 at `5cf3112226`) because
/// nothing then seeded a TileGpu target's alpha independently of its colour, so preserving
/// alpha exposed unseeded pool bytes to a later TCC=1 read. The 16-bit colour road
/// (`aead2dc772..2767b5c667`) built the alias-pass roads that establish a target's alpha and
/// the seed path started giving targets real alpha bytes, at which point the trade flipped
/// sign: ac5 goes 3.798 → 2.026 mean-abs against the software golden with alpha honoured, at
/// `c4b39749f9`. A PARTIAL alpha mask still writes the whole channel — that approximation is
/// the same one the RGB half makes, and stays rowed in the deferred-accuracy ledger.
constexpr u8 gsTileFrameColorWriteMask(u32 fbmsk, u32 fmsk, bool atst_all_fail, u32 afail)
{
	u8 m = gsTileFrameWriteMask(fbmsk, fmsk);
	if (atst_all_fail)
	{
		// Every pixel fails, so AFAIL alone decides what a failing pixel still lands.
		if (afail == AFAIL_KEEP || afail == AFAIL_ZB_ONLY)
			m = 0;
		else if (afail == AFAIL_RGB_ONLY)
			m &= kGSTileChannelsRGB;
	}
	return m;
}

// Whether a surface holds color or depth data — the two swizzle universes of GS
// memory, and on the GPU side the two attachment types.
enum class GSTileSurfaceKind : u8
{
	Color = 0,
	Depth = 1,
};

// Layout identity of one materialized GPU view of an interval of GS memory: where it
// starts (bp, in blocks), its stride (bw, in 64-pixel page columns), and how its bits
// are arranged (psm) + what they mean (kind). Packs injectively into one u32, so the
// exact-layout index is a perfect-hash probe, never a scan.
struct GSTileSurfaceLayout
{
	u32 bp; // 0..GS_MAX_BLOCKS-1 (14 bits)
	u8 bw; // pages per row, FRAME.FBW units, 1..32 (6 bits)
	u8 psm; // GS_PSM (6 bits)
	GSTileSurfaceKind kind;

	constexpr u32 pack() const
	{
		return bp | (static_cast<u32>(bw) << 14) | (static_cast<u32>(psm) << 20) |
			   (static_cast<u32>(kind) << 26);
	}

	constexpr bool operator==(const GSTileSurfaceLayout& o) const { return pack() == o.pack(); }
	constexpr bool operator!=(const GSTileSurfaceLayout& o) const { return pack() != o.pack(); }
};

static_assert(GS_MAX_BLOCKS == 16384, "layout pack() assumes 14-bit block pointers");

// The eight page-swizzle universes of GSLocalMemory, one per swizzle table. Two
// layouts in the same family address identical page geometry with identical
// intra-page arrangement, so a same-family alias is a view or a page remap, never a
// data conversion. The CT32 family deliberately contains the palette-view formats
// (T8H/T4HL/T4HH): they read CT32-shaped pages through a channel window.
enum class GSTileSwizzleFamily : u8
{
	CT32,
	CT16,
	CT16S,
	T8,
	T4,
	Z32,
	Z16,
	Z16S,
	Invalid,
};

constexpr GSTileSwizzleFamily gsTileSwizzleFamily(u32 psm)
{
	switch (psm)
	{
		case PSMCT32:
		case PSMCT24:
		case PSMT8H:
		case PSMT4HL:
		case PSMT4HH:
			return GSTileSwizzleFamily::CT32;
		case PSMCT16:
			return GSTileSwizzleFamily::CT16;
		case PSMCT16S:
			return GSTileSwizzleFamily::CT16S;
		case PSMT8:
			return GSTileSwizzleFamily::T8;
		case PSMT4:
			return GSTileSwizzleFamily::T4;
		case PSMZ32:
		case PSMZ24:
			return GSTileSwizzleFamily::Z32;
		case PSMZ16:
			return GSTileSwizzleFamily::Z16;
		case PSMZ16S:
			return GSTileSwizzleFamily::Z16S;
		default:
			return GSTileSwizzleFamily::Invalid;
	}
}

/// Storage bits per pixel (24-bit formats occupy 32).
constexpr u32 gsTileStorageBpp(u32 psm)
{
	switch (gsTileSwizzleFamily(psm))
	{
		case GSTileSwizzleFamily::CT32:
		case GSTileSwizzleFamily::Z32:
			return 32;
		case GSTileSwizzleFamily::CT16:
		case GSTileSwizzleFamily::CT16S:
		case GSTileSwizzleFamily::Z16:
		case GSTileSwizzleFamily::Z16S:
			return 16;
		case GSTileSwizzleFamily::T8:
			return 8;
		case GSTileSwizzleFamily::T4:
			return 4;
		default:
			return 0;
	}
}

/// Pixel height of one guest page under `psm`. Every format the target pool backs has 64-pixel-wide
/// pages, so the height is the only geometry number a road holding nothing but a PSM has to work
/// out — which the TileGpu byte road's executor is, twice: the writeback's workgroup count and the
/// seed's scissor both come off it. Unit-pinned against GSLocalMemory's own page size, because a
/// disagreement writes half a page or twice one.
constexpr u32 gsTilePageHeight(u32 psm)
{
	switch (gsTileSwizzleFamily(psm))
	{
		case GSTileSwizzleFamily::CT32:
		case GSTileSwizzleFamily::Z32:
			return 32;
		case GSTileSwizzleFamily::CT16:
		case GSTileSwizzleFamily::CT16S:
		case GSTileSwizzleFamily::Z16:
		case GSTileSwizzleFamily::Z16S:
		case GSTileSwizzleFamily::T8:
			return 64;
		case GSTileSwizzleFamily::T4:
			return 128;
		default:
			return 0;
	}
}

/// The TileGpu byte road's shader variant for a format a COLOUR surface can carry. The writeback
/// and the seed compile one program per swizzle universe, because the block table, the page height
/// and the cell width are all baked in; CT32 and CT24 share one (identical addresses — only the
/// byte mask differs), and so do PSMZ32 and PSMZ24.
///
/// ⚠️ The Z32 universe is here because a game may set FRAME.PSM to a depth format and render
/// ordinary colour into it — Ace Combat 5's occlusion probe and Beyond Good & Evil's post-process
/// chain both do, and both then read those bytes back through a PSMCT32 texture window. That
/// surface is a COLOUR surface (its kind comes from FRAME, not from the PSM), and its program is
/// the CT32 one with the depth block XOR: same page geometry, same column table, one extra term.
/// This says nothing about a real Z BUFFER — a Depth-kind surface is refused by the predicate
/// below whatever its PSM, because there is no shader that turns a depth attachment into bytes.
///
/// A format the road does not serve gets an out-of-range index rather than 0, deliberately: a
/// format that slips past the admission test below then compiles nothing, instead of running the
/// CT32 program over 16-bit bytes.
static constexpr u32 kGSTileByteRoadFormats = 4;

constexpr u32 gsTileByteRoadFormat(u32 psm)
{
	switch (psm)
	{
		case PSMCT32:
		case PSMCT24:
			return 0;
		case PSMCT16:
			return 1;
		case PSMCT16S:
			return 2;
		case PSMZ32:
		case PSMZ24:
			return 3;
		default:
			return kGSTileByteRoadFormats;
	}
}

/// The bytes of each 32-bit guest cell a surface's WRITEBACK writes, as the shader's `byte_mask`
/// push constant. A PSMCT24 surface stores nothing in the alpha byte, so its writeback leaves that
/// byte to whoever does hold it — which, when nobody on the GPU does, means the ring slot's PREFILL
/// from the CPU shadow. That is why the prefill decision has to ask this and not just which BLOCKS
/// the writebacks reach: a page a CT24 surface covers whole is still a quarter uncovered by bytes,
/// and an unprefilled slot would hand its previous tenant's alpha to the next reader.
///
/// One rule, one place: EmitPrepOp puts this on the op and EnsureRingSlot's coverage test reads the
/// same function, so the two cannot come apart. (They did: the coverage test was per BLOCK only, and
/// every CT24-composed page shipped an unwritten alpha byte.)
constexpr u32 gsTileWritebackByteMask(u32 psm)
{
	return (psm == PSMCT24) ? 0x00FFFFFFu : 0xFFFFFFFFu;
}

/// The writeback compute workgroup's edge, in WORDS of the guest page. The device injects it into
/// tilegpu_writeback.glsl as TILEGPU_WB_WG and divides the page by it here, so the shader's
/// local_size and the dispatch's group count cannot disagree — a per-device retune is this one
/// number and nothing else.
///
/// 16, not 8, on the store path's evidence: the pass is store-bound, and a 64-invocation workgroup
/// on a part whose wave is 128 leaves half of every wave idle. An Adreno 650 microbenchmark of the
/// exact address pattern measured 16x16 at −11.7% against 8x8, and −16.0% once the same workgroup
/// staged its stores through shared memory as uvec4s, which is what the shader does. The shape is
/// fitted to Adreno; Mali's 16-lane subgroups are a named, unmeasured risk on the same number.
///
/// It must be a multiple of 8 and divide both page word extents (64x32 and 32x64), so that a
/// workgroup covers whole 8x8-word blocks — the shader's shared-memory stage is per block, and half
/// a block in a workgroup would have no one to gather it.
static constexpr u32 kGSTileWritebackGroupDim = 16;

/// The writeback compute dispatch for ONE page of `psm`, in workgroups of kGSTileWritebackGroupDim
/// square invocations.
///
/// A 32-bit page is 64x32 texels and one word each, so the grid is the page. A 16-bit page is 64x64
/// texels of half a word each, and the pass covers it one WORD per invocation — the two texels eight
/// apart that columnTable16 puts in the same word — so the grid is 32x64. Pairing them is not an
/// optimisation: the alternative is two invocations read-modify-writing one word from different
/// workgroups, with no ordering between them.
struct GSTileDispatch2D
{
	u32 x, y;
};

constexpr GSTileDispatch2D gsTileWritebackGroups(u32 psm)
{
	const u32 words_x = (gsTileStorageBpp(psm) == 32) ? 64u : 32u;
	return GSTileDispatch2D{words_x / kGSTileWritebackGroupDim, gsTilePageHeight(psm) / kGSTileWritebackGroupDim};
}

// The page word extents both arms carry, and the block granularity the shader's shared-memory stage
// needs. A dim that fails either of these compiles a dispatch that covers part of a page.
static_assert(kGSTileWritebackGroupDim % 8 == 0, "a writeback workgroup must cover whole 8x8-word blocks");
static_assert(32 % kGSTileWritebackGroupDim == 0 && 64 % kGSTileWritebackGroupDim == 0,
	"a writeback workgroup must tile both page word extents (64x32 and 32x64) exactly");

/// A 32-bit RGBA guest word packed to the 16-bit colour formats' A1B5G5R5: the top five bits of
/// each colour byte, and the alpha bit from the MSB of the alpha byte. This is
/// GSLocalMemory::WriteFrame16's arithmetic — the software renderer's own 16-bit frame store — and
/// the unit suite holds the two together.
constexpr u16 gsTilePack5551(u32 c)
{
	const u32 rb = c & 0x00F800F8u;
	const u32 ga = c & 0x8000F800u;
	return static_cast<u16>((ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3));
}

/// The inverse, as a FRAME buffer read takes it: five bits back to the top of each byte, low bits
/// zero, and the alpha bit to 0x80 or 0x00. That is GSLocalMemory::Expand16To32 under the pool's
/// own TEXA pin (AEM = 0, TA0 = 0, TA1 = 0x80) — the same pin UploadPages uses for Z16 — and it is
/// the exact inverse of the pack above, so a cell that goes out through one comes back through the
/// other unchanged.
///
/// ⚠️ NOT a texture read: a game's TEXA can put anything in the alpha byte and AEM can make an
/// all-zero cell transparent. This is the byte road's frame-buffer road only.
constexpr u32 gsTileUnpack5551(u16 c)
{
	return ((c & 0x8000u) ? 0x80000000u : 0u) | (static_cast<u32>(c & 0x7C00u) << 9) |
		   (static_cast<u32>(c & 0x03E0u) << 6) | (static_cast<u32>(c & 0x001Fu) << 3);
}

/// The 32-bit CLUT word a CSM1 loader reads out of a 16-bit surface, from the two RGBA8 cells that
/// hold it. A guest word of a PSMCT16/PSMCT16S surface is two cells, and in that surface's texel
/// space they are the texel at (x, y) and the one at (x + 8, y) — so a gather off a 16-bit owner
/// fetches two texels per palette entry and packs each back to the halfword the console stored.
///
/// The pack is gsTilePack5551 above, which is GSLocalMemory::WriteFrame16 verbatim, so the word this
/// produces is the word GSClut's own loader would have read out of the guest bytes the readback road
/// would have written. That is the whole correctness argument for the 16-bit gather, and the suite
/// pins it end to end against a real GSClut load rather than against this function.
constexpr u32 gsTileClut16Word(u32 low_cell, u32 high_cell)
{
	return static_cast<u32>(gsTilePack5551(low_cell)) | (static_cast<u32>(gsTilePack5551(high_cell)) << 16);
}

/// Whether a surface can travel the TileGpu byte road — the writeback that reswizzles its finished
/// pixels into guest bytes and the seed that reads bytes back into it. A COLOUR surface in one of
/// the four swizzle universes the road has a program for, with a page-aligned base: both shaders
/// derive a page from (row, col) off the base page, and pool page (row, col) IS physical page base
/// + row*bw + col.
///
/// ⚠️ "Colour surface" is about the KIND, not the PSM, and the two come apart in both directions: a
/// FRAME whose PSM is PSMZ32 is a colour surface (kind comes from FRAME) and does travel, while a
/// real Z buffer is a Depth surface and does not, whatever its PSM. Depth surfaces have no
/// writeback shader in any format — a standing separate gap, because their pixels are a depth
/// attachment rather than guest bytes — so truth that moves through one is counted lossy.
constexpr bool gsTileSurfaceHasByteRoad(const GSTileSurfaceLayout& layout)
{
	return layout.kind == GSTileSurfaceKind::Color && (layout.bp & 31) == 0 &&
		   gsTileByteRoadFormat(layout.psm) < kGSTileByteRoadFormats;
}

/// The DEPTH seed's shader variant, one per (page geometry, value width) pair a Z buffer can have.
///
/// Its own enumeration and not `gsTileByteRoadFormat`'s, for two reasons. The road is one-way — a
/// depth attachment can be filled FROM guest bytes but there is still no shader that turns one back
/// INTO bytes — so the two must be able to admit different format sets; and a Z buffer reaches four
/// formats the colour road never carries (PSMZ16/PSMZ16S, and the 24-bit value width, which is a
/// value mask rather than a byte mask here).
///
/// ⚠️ A Z buffer's PSM is a COLOUR format whenever FRAME's is a depth one: GSState swaps the 0x30
/// bit (`GIFRegHandlerZBUF`, the Powerdrome behaviour), so the two families both appear on a
/// Depth-kind surface and both are listed. The pairs differ only by the depth block XOR the shader
/// applies, exactly as PSMZ32 differs from PSMCT32 everywhere else.
static constexpr u32 kGSTileDepthRoadFormats = 8;

constexpr u32 gsTileDepthRoadFormat(u32 psm)
{
	switch (psm)
	{
		case PSMCT32:
			return 0;
		case PSMCT24:
			return 1;
		case PSMCT16:
			return 2;
		case PSMCT16S:
			return 3;
		case PSMZ32:
			return 4;
		case PSMZ24:
			return 5;
		case PSMZ16:
			return 6;
		case PSMZ16S:
			return 7;
		default:
			return kGSTileDepthRoadFormats;
	}
}

/// Whether a DEPTH surface can be re-seeded out of guest bytes: a page-aligned base (the seed
/// derives a page from (row, col) off the base page, and pool page (row, col) IS physical page base
/// + row*bw + col only then) in one of the eight formats above.
///
/// The mirror of `gsTileSurfaceHasByteRoad` for the other kind and the other direction. Deliberately
/// NOT folded into it: that predicate answers "writeback AND seed", and a depth surface has only the
/// seed half. Everything that asks about writing a surface back into bytes must go on refusing Depth.
constexpr bool gsTileSurfaceHasDepthSeedRoad(const GSTileSurfaceLayout& layout)
{
	return layout.kind == GSTileSurfaceKind::Depth && (layout.bp & 31) == 0 &&
		   gsTileDepthRoadFormat(layout.psm) < kGSTileDepthRoadFormats;
}

/// Whether bytes a surface holds may be read back into a DEPTH attachment as that Z buffer's own
/// depth: a COLOUR surface whose pixel space IS the Z buffer's — same base, same stride, same
/// storage width, so its cells and the Z buffer's are the same cells and a whole-page claim by it is
/// a whole-page rewrite of the very words the Z buffer stores. The swizzle universes may differ (a
/// colour twin under the depth block XOR is the whole point: that is how "clear Z by drawing zero
/// over its address" arrives), because the seed reads each Z cell through the Z buffer's own
/// swizzle whatever wrote it.
///
/// Another DEPTH surface never qualifies, whatever its layout. Its pixels are a depth attachment and
/// no shader carries them into bytes, so what the ring would hold for it is the CPU shadow's stale
/// copy. The compose counts that as lossy while it happens — but it also marks the page SYNCED, and
/// from then on the model's own answer is "the store has these bytes". This clause is what keeps a
/// later seed from acting on that.
///
/// ⚠️ This is NARROWER than "the model says those bytes are the game's", and deliberately. The model
/// tracks bytes per PAGE and a draw that covers part of a page claims all of it, so byte truth that
/// travelled through a FOREIGN pixel space carries whole-page claims that were never whole-page
/// writes — and, worse, may have passed through a lossy sync that dropped the real Z on the floor
/// while marking the page composed. OutRun 2006 renders 16-bit colour over its own Z buffer's
/// address (same base, same stride, 64x64 pages against the Z buffer's 64x32), so the model hands
/// the depth road a page of colour and the car's own fragments then fail GEQUAL against it. Refusing
/// the foreign view leaves that page exactly as it was before this road existed: the depth image
/// keeps it, and it is counted lossy.
constexpr bool gsTileDepthSeedSourceMatches(const GSTileSurfaceLayout& owner, const GSTileSurfaceLayout& z)
{
	return owner.kind == GSTileSurfaceKind::Color && owner.bp == z.bp && owner.bw == z.bw &&
		   gsTileStorageBpp(owner.psm) == gsTileStorageBpp(z.psm);
}

/// The NARROWER question the roads that reinterpret an owner's texture ask: is this surface's pixel
/// space a CT32-shaped window — 64x32 pages of 8x8 blocks, one word per texel?
///
/// The donor build, the CLUT block copy and a rule-2 bind all read an owner through CT32's block
/// and column forms, so they must go on refusing a 16-bit owner even though the byte road carries
/// one. Two predicates rather than one, deliberately: sharing a predicate is exactly how a 16-bit
/// target would come to be addressed through the 32-bit forms and silently produce texels out of
/// the wrong bytes. Always a subset of the byte road.
constexpr bool gsTileSurfaceHasCt32PixelSpace(const GSTileSurfaceLayout& layout)
{
	return layout.kind == GSTileSurfaceKind::Color && (layout.bp & 31) == 0 &&
		   (layout.psm == PSMCT32 || layout.psm == PSMCT24);
}

/// Which of the predicate above's three tests fails FIRST. Census only -- nothing decides on it.
///
/// A refusal counter that says no more than "the layout clause refused it" cannot separate a 16-bit
/// colour owner, which a wider gather could be built to serve, from an unaligned base or a depth
/// surface, which it could not. That separation is the whole input to "is a 16-bit CLUT gather worth
/// building", so it is measured rather than reasoned about.
///
/// ⚠️ Lives here, immediately below the predicate, because its clause ORDER has to match it: a
/// reordering above with no matching edit here silently renames every count. The pair's contract is
/// that this returns None exactly when the predicate returns true.
enum GSTileCt32Clause : u32
{
	kGSTileCt32ClauseNone = 0,
	kGSTileCt32ClauseKind, ///< not a colour surface at all (a depth surface)
	kGSTileCt32ClauseBase, ///< colour, but its base is not page-aligned
	kGSTileCt32ClausePsm, ///< colour and page-aligned, but the format is not CT32/CT24
	kGSTileCt32ClauseCount
};

constexpr GSTileCt32Clause gsTileSurfaceCt32PixelSpaceClause(const GSTileSurfaceLayout& layout)
{
	if (layout.kind != GSTileSurfaceKind::Color)
		return kGSTileCt32ClauseKind;
	if ((layout.bp & 31) != 0)
		return kGSTileCt32ClauseBase;
	if (layout.psm != PSMCT32 && layout.psm != PSMCT24)
		return kGSTileCt32ClausePsm;
	return kGSTileCt32ClauseNone;
}

/// The WIDER question the CLUT gather asks once it has a 16-bit road: is this surface's pixel space
/// one the gather has a copy geometry AND a fetch arm for? That is the CT32 pair the two functions
/// above admit, plus the two 16-bit colour formats — the ones GSTileSwizzleForms::LocateClutBlocks16
/// serves.
///
/// ⚠️ A THIRD predicate, not a widening of gsTileSurfaceHasCt32PixelSpace, and the pair above keeps
/// its own contract untouched (the clause function returns None exactly when the narrow predicate is
/// true, and that must go on being exactly the narrow one). The narrow predicate has three other
/// callers — the donor build and the rule-2 bind — that read an owner's texture through CT32 block
/// and column forms, and a 16-bit owner reaching one of those produces texels out of the wrong bytes
/// with no error anywhere. Always a subset of the byte road.
constexpr bool gsTileSurfaceHasClutGatherPixelSpace(const GSTileSurfaceLayout& layout)
{
	return layout.kind == GSTileSurfaceKind::Color && (layout.bp & 31) == 0 &&
	       (layout.psm == PSMCT32 || layout.psm == PSMCT24 || layout.psm == PSMCT16 || layout.psm == PSMCT16S);
}

// Cross-layout alias resolution tiers, cheapest first. M2 classifies every alias and
// implements only Spill (always correct, counted); the cheaper tiers get realized as
// their consumers land.
enum class GSTileAliasTier : u8
{
	OffsetView, // same family, same stride, page-aligned bases: a rectangle offset
	PageRemap, // same family, page-aligned bases: GPU copy with per-page-row remap
	Convert, // same storage bpp, different family: existing convert shaders
	Spill, // anything else: flush the cone, read intersecting pages back to truth
};

constexpr GSTileAliasTier gsTileClassifyAlias(const GSTileSurfaceLayout& a, const GSTileSurfaceLayout& b)
{
	// A block-offset (sub-page) base means one view starts mid-page, where the
	// intra-page swizzle makes a rectangle view or page remap unexpressible.
	const bool page_aligned = ((a.bp | b.bp) & 31) == 0;
	if (gsTileSwizzleFamily(a.psm) == gsTileSwizzleFamily(b.psm))
	{
		if (!page_aligned)
			return GSTileAliasTier::Spill;
		return (a.bw == b.bw) ? GSTileAliasTier::OffsetView : GSTileAliasTier::PageRemap;
	}
	if (gsTileStorageBpp(a.psm) == gsTileStorageBpp(b.psm) && gsTileStorageBpp(a.psm) != 0)
		return GSTileAliasTier::Convert;
	return GSTileAliasTier::Spill;
}

// Surface handles are dense small integers so per-page owner tables stay one u16.
using GSTileSurfaceId = u16;
static constexpr GSTileSurfaceId kGSTileNoSurface = 0xFFFF;
