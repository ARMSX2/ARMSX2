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
};

struct GSTileSelectionDecision
{
	bool use_tile = false;
	GSTileSelectionReason reason = GSTileSelectionReason::AutoClassic;
};

constexpr GSTileSelectionDecision DecideHWRendererVariant(
	GSHWRendererVariant variant, bool is_vulkan, bool gamedb_recommends_tile, bool mobile_tiler_profile)
{
	GSTileSelectionDecision decision;

	if (variant == GSHWRendererVariant::Classic)
	{
		decision.use_tile = false;
		decision.reason = GSTileSelectionReason::ExplicitClassic;
	}
	else if (variant == GSHWRendererVariant::Tile)
	{
		// An explicit Tile choice is honoured on any Vulkan device — including desktop and
		// Apple ones, which is what makes a TBDR dev box a legitimate development vehicle.
		// The only veto is the API: the Tile renderer has no non-Vulkan backend.
		decision.use_tile = is_vulkan;
		decision.reason =
			is_vulkan ? GSTileSelectionReason::ExplicitTile : GSTileSelectionReason::TileRequiresVulkan;
	}
	else // Auto
	{
		// Auto promotes only titles the ladder validated, and only on the shape they were
		// validated on. Desktop GPUs stay Classic on Auto even for promoted titles: the
		// promotion evidence is mobile-tiler evidence.
		decision.use_tile = gamedb_recommends_tile && is_vulkan && mobile_tiler_profile;
		decision.reason =
			decision.use_tile ? GSTileSelectionReason::AutoTile : GSTileSelectionReason::AutoClassic;
	}

	return decision;
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
			// no default: a new reason must fail to compile until it is named here
	}
	return "unknown";
}

// The five outcomes, one pin each. The exhaustive 24-case sweep lives in the test file.
static_assert(!DecideHWRendererVariant(GSHWRendererVariant::Classic, true, true, true).use_tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Classic, true, true, true).reason ==
			  GSTileSelectionReason::ExplicitClassic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, true, false, false).use_tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, true, false, false).reason ==
			  GSTileSelectionReason::ExplicitTile);
static_assert(!DecideHWRendererVariant(GSHWRendererVariant::Tile, false, true, true).use_tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, false, true, true).reason ==
			  GSTileSelectionReason::TileRequiresVulkan);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, true).use_tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, true).reason ==
			  GSTileSelectionReason::AutoTile);
static_assert(!DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, false).use_tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, false).reason ==
			  GSTileSelectionReason::AutoClassic);
