// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"

// Which hardware-renderer implementation runs, as one pure function — same charter as
// GSFramebufferFetchPolicy.h: all inputs explicit, no device types, constexpr so every case is
// pinned below at compile time and again by name in gs_tile_selection_policy_tests.cpp.
//
// The Tile renderer is opt-in indefinitely. An explicit user choice always wins: the GameDB
// recommendation only exists so that Auto can promote individually validated titles on the
// device shape they were validated on (Vulkan on a mobile tiler), and it is applied to the
// variant setting only when the user left it on Auto — that merge happens at the GameDB fix
// layer, so by the time this function runs, `variant` is the merged setting and
// `gamedb_recommends_tile` matters only in the Auto arm.

// Why the decision landed where it did. Carried out of the policy so the caller can log the
// specific reason in the renderer identity line the oracle harness parses.
enum class GSTileSelectionReason : u8
{
	ExplicitClassic, // the user chose Classic
	ExplicitTile, // the user chose Tile and the device is Vulkan
	TileRequiresVulkan, // the user chose Tile on a non-Vulkan device; Classic runs instead
	AutoClassic, // Auto: no per-title recommendation, or not the validated device shape
	AutoTile, // Auto: GameDB recommends this title on Vulkan on a mobile tiler
	ExplicitTileGpu, // the user chose TileGpu and the device is Vulkan
	TileGpuRequiresVulkan, // the user chose TileGpu on a non-Vulkan device; Classic runs instead
};

// The RESOLVED variant: which hardware-renderer implementation actually constructs. Never
// Auto — Auto is an input to the decision, not an outcome of it.
struct GSTileSelectionDecision
{
	GSHWRendererVariant variant = GSHWRendererVariant::Classic;
	GSTileSelectionReason reason = GSTileSelectionReason::AutoClassic;
};

constexpr GSTileSelectionDecision DecideHWRendererVariant(
	GSHWRendererVariant variant, bool is_vulkan, bool gamedb_recommends_tile, bool mobile_tiler_profile)
{
	GSTileSelectionDecision decision;

	if (variant == GSHWRendererVariant::Classic)
	{
		decision.variant = GSHWRendererVariant::Classic;
		decision.reason = GSTileSelectionReason::ExplicitClassic;
	}
	else if (variant == GSHWRendererVariant::Tile)
	{
		// An explicit Tile choice is honoured on any Vulkan device — including desktop and
		// Apple ones, which is what makes a TBDR dev box a legitimate development vehicle.
		// The only veto is the API: the Tile renderer has no non-Vulkan backend.
		decision.variant = is_vulkan ? GSHWRendererVariant::Tile : GSHWRendererVariant::Classic;
		decision.reason =
			is_vulkan ? GSTileSelectionReason::ExplicitTile : GSTileSelectionReason::TileRequiresVulkan;
	}
	else if (variant == GSHWRendererVariant::TileGpu)
	{
		// Same rule as Tile: honoured on any Vulkan device, vetoed only by the API. Auto
		// never resolves here — the GameDB promotion evidence is Tile evidence, and TileGpu
		// earns its own promotion input if and when it graduates.
		decision.variant = is_vulkan ? GSHWRendererVariant::TileGpu : GSHWRendererVariant::Classic;
		decision.reason = is_vulkan ? GSTileSelectionReason::ExplicitTileGpu :
									  GSTileSelectionReason::TileGpuRequiresVulkan;
	}
	else // Auto
	{
		// Auto promotes only titles the ladder validated, and only on the shape they were
		// validated on. Desktop GPUs stay Classic on Auto even for promoted titles: the
		// promotion evidence is mobile-tiler evidence.
		const bool use_tile = gamedb_recommends_tile && is_vulkan && mobile_tiler_profile;
		decision.variant = use_tile ? GSHWRendererVariant::Tile : GSHWRendererVariant::Classic;
		decision.reason = use_tile ? GSTileSelectionReason::AutoTile : GSTileSelectionReason::AutoClassic;
	}

	return decision;
}

constexpr const char* GSHWRendererVariantName(GSHWRendererVariant variant)
{
	switch (variant)
	{
		case GSHWRendererVariant::Auto:
			return "auto";
		case GSHWRendererVariant::Classic:
			return "classic";
		case GSHWRendererVariant::Tile:
			return "tile";
		case GSHWRendererVariant::TileGpu:
			return "tilegpu";
			// no default: a new variant must fail to compile until it is named here
	}
	return "unknown";
}

/// Whether the effective configuration consults Classic's blend-accuracy setting
/// (AccurateBlendingUnit) at all. The SW renderer never reads it, and the Tile
/// variant reads it nowhere BY CHARTER (M4): one shipped blending behavior, decided
/// per-draw by the lowering, with accuracy a CI-gated property of the implementation
/// rather than a runtime mode. TileGpu inherits that charter — its blending is
/// planner output, not a runtime accuracy mode — so it is equally inert where it
/// actually runs (Vulkan). Everything that nags, clamps or cycles the setting
/// gates on this so the whole interaction is inert when the setting has no consumer:
/// the GameDB recommendation OSD, the min/max clamps, the unsafe-settings line, and
/// the cycle hotkey.
///
/// `renderer` must be the RESOLVED renderer (Auto put through
/// GSUtil::GetPreferredRenderer()), because the Tile/TileGpu arms require Vulkan by
/// the selection rule above — an explicit Tile or TileGpu choice on a non-Vulkan
/// device runs Classic, and the setting matters again. Auto variant keeps the
/// setting live: it resolves Classic until the GameDB promotion input exists (M8),
/// and this predicate must be revisited alongside that wiring (a promoted title on
/// Auto will run Tile with the setting dead, and this function is where that
/// decision lives).
constexpr bool GSAccurateBlendingUnitConsulted(GSRendererType renderer, GSHWRendererVariant variant)
{
	if (renderer == GSRendererType::SW || renderer == GSRendererType::Null)
		return false;
	if ((variant == GSHWRendererVariant::Tile || variant == GSHWRendererVariant::TileGpu) &&
		renderer == GSRendererType::VK)
		return false;
	return true;
}

constexpr const char* GSTileSelectionReasonName(GSTileSelectionReason reason)
{
	switch (reason)
	{
		case GSTileSelectionReason::ExplicitClassic:
			return "explicit-classic";
		case GSTileSelectionReason::ExplicitTile:
			return "explicit-tile";
		case GSTileSelectionReason::TileRequiresVulkan:
			return "tile-requires-vulkan";
		case GSTileSelectionReason::AutoClassic:
			return "auto-classic";
		case GSTileSelectionReason::AutoTile:
			return "auto-tile";
		case GSTileSelectionReason::ExplicitTileGpu:
			return "explicit-tilegpu";
		case GSTileSelectionReason::TileGpuRequiresVulkan:
			return "tilegpu-requires-vulkan";
			// no default: a new reason must fail to compile until it is named here
	}
	return "unknown";
}

// The seven outcomes, one pin each. The exhaustive 32-case sweep lives in the test file.
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Classic, true, true, true).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Classic, true, true, true).reason ==
			  GSTileSelectionReason::ExplicitClassic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, true, false, false).variant ==
			  GSHWRendererVariant::Tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, true, false, false).reason ==
			  GSTileSelectionReason::ExplicitTile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, false, true, true).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, false, true, true).reason ==
			  GSTileSelectionReason::TileRequiresVulkan);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, true, false, false).variant ==
			  GSHWRendererVariant::TileGpu);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, true, false, false).reason ==
			  GSTileSelectionReason::ExplicitTileGpu);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, false, true, true).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, false, true, true).reason ==
			  GSTileSelectionReason::TileGpuRequiresVulkan);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, true).variant ==
			  GSHWRendererVariant::Tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, true).reason ==
			  GSTileSelectionReason::AutoTile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, false).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, false).reason ==
			  GSTileSelectionReason::AutoClassic);
