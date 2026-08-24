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
/// A channel the format stores NOTHING in — a 24-bit frame's alpha — rides along with the
/// ones it does store. Those bytes are not guest data (writeback masks them off under the
/// format's own byte mask), so reading "no bits here" as "fully masked" would take alpha
/// out of every 24-bit draw for nothing. It cannot keep a draw alive on its own: when
/// every channel the format DOES store is masked, nothing guest-visible lands and the
/// mask is empty.
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
		if (stored == 0 || (fbmsk & stored) != stored)
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

/// Whether the alpha the test compares is the same for every fragment of the draw.
///
/// Two independent reasons it can vary, and neither may be present. The TEXTURE supplies
/// the alpha when it is sampled and TEX0.TCC says its alpha is used — with TCC=0 every
/// texture function passes the vertex alpha through untouched, which is what makes an
/// untextured draw and a TCC=0 one the same case. And AA1 modulates a fragment's alpha by
/// its coverage, per pixel, on the primitive's edges.
///
/// `vertex_alpha_uniform` is the vertex trace's alpha equality bit — the ALPHA component
/// alone (GSVertexTrace's m_eq.a), not whole-RGBA equality: a gouraud-shaded draw whose
/// colour ramps while its alpha holds still is as decidable as a flat one.
constexpr bool gsTileAlphaIsDrawConstant(bool tme, bool tcc, bool aa1, bool vertex_alpha_uniform)
{
	return vertex_alpha_uniform && !aa1 && !(tme && tcc);
}

/// Decide the alpha test at plan time, where the fragment alpha allows it.
///
/// ATST=NEVER and ATST=ALWAYS decide themselves. What this adds is the SAME intent written
/// as a comparison that cannot come out either way — which games do write, and which
/// costs a whole character when it is read as a live test instead.
///
/// Katamari Damacy composites the King's head out of two sprites over one rectangle: the
/// first stamps a lower Z into it, the second draws the head with ZTST=GEQUAL against that
/// Z. The stamp is untextured with vertex alpha 0x00, asks for ATST=NOTEQUAL AREF=0 — "0 !=
/// 0" is false, so every fragment fails — and carries AFAIL=ZB_ONLY, "a failing fragment
/// still writes depth". The fragment stage's approximation discards a failing fragment
/// outright, so read as a live test the stamp deposits no depth and the head behind it then
/// fails its own GEQUAL over every pixel the star covers.
///
/// ⚠️ The comparisons are the software renderer's (GSState::TryAlphaTest's GetResult with
/// the vertex-trace alpha range collapsed to one value) and tilegpu.glsl's fragment stage,
/// which are the same list. All three have to stay the same list; a boundary that drifts
/// here changes some other game's draw and nothing says so.
constexpr GSTileAlphaTestFold gsTileFoldAlphaTest(bool ate, u32 atst, u32 aref, bool alpha_is_constant, u32 alpha)
{
	if (!ate || atst == ATST_ALWAYS)
		return GSTileAlphaTestFold::AllPass;
	if (atst == ATST_NEVER)
		return GSTileAlphaTestFold::AllFail;
	if (!alpha_is_constant)
		return GSTileAlphaTestFold::Varies;

	bool pass;
	switch (atst)
	{
		case ATST_LESS: pass = alpha < aref; break;
		case ATST_LEQUAL: pass = alpha <= aref; break;
		case ATST_EQUAL: pass = alpha == aref; break;
		case ATST_GEQUAL: pass = alpha >= aref; break;
		case ATST_GREATER: pass = alpha > aref; break;
		case ATST_NOTEQUAL: pass = alpha != aref; break;
		default: return GSTileAlphaTestFold::Varies;
	}
	return pass ? GSTileAlphaTestFold::AllPass : GSTileAlphaTestFold::AllFail;
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
constexpr bool gsTileColorLandsOnEveryFragment(GSTileAlphaTestFold fold, u32 afail)
{
	if (fold == GSTileAlphaTestFold::AllPass)
		return true;
	if (fold == GSTileAlphaTestFold::AllFail)
		return afail == AFAIL_FB_ONLY || afail == AFAIL_RGB_ONLY;
	return false;
}

/// The colour write mask a draw actually gets: the FBMSK channels above, with the alpha
/// test's AFAIL fold on top and with the one carve-out this road still owes.
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
/// ⚠️ **FBMSK's ALPHA byte is deliberately NOT honoured yet.** A draw that lands anything
/// lands alpha, and only AFAIL RGB_ONLY takes it away — exactly what this renderer did
/// before FBMSK was per channel at all. That is a staging decision with a measurement
/// behind it, not an oversight. Preserving a target's alpha exposes whatever its pool
/// image already held there, and nothing seeds a TileGpu target's alpha independently of
/// its colour, so a later TCC=1 read of that target samples stale bytes. Ace Combat 5 is
/// where it was measured: 248 draws a frame at FBMSK=0xFF000000 over cloud buffers whose
/// alpha is established by a PSMCT16S alias pass and a self-read pass, neither of which
/// this road serves. Honouring the alpha half there puts a white band across the sky and
/// takes the frame from 0.021 to 0.080 mean-abs against the software golden, while every
/// dump the RGB half repairs is unaffected by it either way. So alpha follows AFAIL alone
/// until targets carry a seeded alpha. Rowed in the deferred-accuracy ledger, and pinned
/// by `AlphaHalfOfFbmskIsDeferred` — that test is what says the carve-out can go, the day
/// the seeding lands.
constexpr u8 gsTileFrameColorWriteMask(u32 fbmsk, u32 fmsk, bool atst_all_fail, u32 afail)
{
	u8 m = gsTileFrameWriteMask(fbmsk, fmsk);
	if (m != 0)
		m |= kGSTileChannelAlpha; // the deferred alpha half: a live draw always writes alpha
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

/// The writeback compute dispatch for ONE page of `psm`, in workgroups of 8x8 invocations.
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
	return GSTileDispatch2D{words_x / 8u, gsTilePageHeight(psm) / 8u};
}

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
