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

/// The colour write mask a draw actually gets: the FBMSK channels above, with the alpha
/// test's AFAIL fold on top and with the one carve-out this road still owes.
///
/// The two derivations COMPOSE, neither overrides the other. ATST=NEVER never reaches the
/// fragment stage — every pixel fails it — so AFAIL alone says what a failing pixel still
/// writes; FBMSK then says which of those channels the register lets through. Reading
/// only one of the two is the bug this function exists to prevent: OutRun's world-erasing
/// sprites are ATST NEVER + AFAIL FB_ONLY ("write the frame buffer") carrying
/// FBMSK=0x00FFFFFF ("but only its alpha byte").
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
constexpr u8 gsTileFrameColorWriteMask(u32 fbmsk, u32 fmsk, bool atst_never, u32 afail)
{
	u8 m = gsTileFrameWriteMask(fbmsk, fmsk);
	if (m != 0)
		m |= kGSTileChannelAlpha; // the deferred alpha half: a live draw always writes alpha
	if (atst_never)
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
