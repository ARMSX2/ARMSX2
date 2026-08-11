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
