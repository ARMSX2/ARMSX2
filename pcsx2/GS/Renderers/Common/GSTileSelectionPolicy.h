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
	TileGpuRequiresDeviceContract, // Vulkan, but the device fails the TileGpu contract; Classic runs instead
	TileGpuContractOverridden, // ...and TileGpuIgnoreDeviceContract forced TileGpu anyway
};

// The RESOLVED variant: which hardware-renderer implementation actually constructs. Never
// Auto — Auto is an input to the decision, not an outcome of it.
struct GSTileSelectionDecision
{
	GSHWRendererVariant variant = GSHWRendererVariant::Classic;
	GSTileSelectionReason reason = GSTileSelectionReason::AutoClassic;
};

// `device_serves_tilegpu` is the device's own answer to GSDevice::TileGpuExecutorAvailable(), which
// is readable here because the device is created before the variant is chosen. No default: this file
// takes every input explicitly, and a defaulted device fact is one a new call site can forget.
//
// `ignore_device_contract` is EmuCore/GS/TileGpuIgnoreDeviceContract, the bring-up escape. It waives
// the contract only -- never the API -- so it can never resurrect TileGpu off Vulkan.
constexpr GSTileSelectionDecision DecideHWRendererVariant(GSHWRendererVariant variant, bool is_vulkan,
	bool device_serves_tilegpu, bool ignore_device_contract, bool gamedb_recommends_tile,
	bool mobile_tiler_profile)
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
		// TileGpu has TWO vetoes where Tile has one. The API veto is the same: there is no
		// non-Vulkan backend. The second is the device contract — TileGpu is the only variant
		// whose renderer cannot function on a device that fails a capability probe, because it
		// has no fallback road of its own: the executor simply refuses the plan and every draw
		// is discarded, which reaches the user as a black frame and a clean exit code.
		//
		// So a contract-absent device resolves Classic. Decided 2026-08-24, during the Mali
		// bring-up: the RG477V's blob failed one contract term and the whole suite arm rendered
		// nothing, three rounds running, byte-identically, with the "TileGpu renderer active"
		// banner still printing. Running the renderer the user did not ask for is strictly
		// better than running nothing. (GSDeviceVK's contract-evaluation comment recorded this
		// as pending; this is where it was answered.)
		//
		// Auto never resolves here — the GameDB promotion evidence is Tile evidence, and TileGpu
		// earns its own promotion input if and when it graduates.
		if (!is_vulkan)
		{
			decision.variant = GSHWRendererVariant::Classic;
			decision.reason = GSTileSelectionReason::TileGpuRequiresVulkan;
		}
		else if (device_serves_tilegpu)
		{
			decision.variant = GSHWRendererVariant::TileGpu;
			decision.reason = GSTileSelectionReason::ExplicitTileGpu;
		}
		else if (ignore_device_contract)
		{
			// The bring-up escape: build it anyway, on a device known not to serve it, because
			// the pipeline creations and the validation output are the point of that run and the
			// frame is not. Its own reason so the caller can say which of the two TileGpu
			// resolutions this is.
			decision.variant = GSHWRendererVariant::TileGpu;
			decision.reason = GSTileSelectionReason::TileGpuContractOverridden;
		}
		else
		{
			decision.variant = GSHWRendererVariant::Classic;
			decision.reason = GSTileSelectionReason::TileGpuRequiresDeviceContract;
		}
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
		case GSTileSelectionReason::TileGpuRequiresDeviceContract:
			return "tilegpu-requires-device-contract";
		case GSTileSelectionReason::TileGpuContractOverridden:
			return "tilegpu-contract-overridden";
			// no default: a new reason must fail to compile until it is named here
	}
	return "unknown";
}

// The nine outcomes, one pin each. The exhaustive 128-case sweep lives in the test file.
// Argument order: variant, is_vulkan, device_serves_tilegpu, ignore_device_contract,
// gamedb_recommends_tile, mobile_tiler_profile.
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Classic, true, true, false, true, true).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Classic, true, true, false, true, true).reason ==
			  GSTileSelectionReason::ExplicitClassic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, true, false, false, false, false).variant ==
			  GSHWRendererVariant::Tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, true, false, false, false, false).reason ==
			  GSTileSelectionReason::ExplicitTile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, false, true, false, true, true).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Tile, false, true, false, true, true).reason ==
			  GSTileSelectionReason::TileRequiresVulkan);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, true, true, false, false, false).variant ==
			  GSHWRendererVariant::TileGpu);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, true, true, false, false, false).reason ==
			  GSTileSelectionReason::ExplicitTileGpu);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, false, true, false, true, true).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, false, true, false, true, true).reason ==
			  GSTileSelectionReason::TileGpuRequiresVulkan);
// Vulkan, contract absent: Classic by default, TileGpu only under the bring-up escape.
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, true, false, false, false, false).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, true, false, false, false, false).reason ==
			  GSTileSelectionReason::TileGpuRequiresDeviceContract);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, true, false, true, false, false).variant ==
			  GSHWRendererVariant::TileGpu);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, true, false, true, false, false).reason ==
			  GSTileSelectionReason::TileGpuContractOverridden);
// ...and the escape never beats the API veto.
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, false, false, true, false, false).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::TileGpu, false, false, true, false, false).reason ==
			  GSTileSelectionReason::TileGpuRequiresVulkan);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, false, true, true).variant ==
			  GSHWRendererVariant::Tile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, false, true, true).reason ==
			  GSTileSelectionReason::AutoTile);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, false, true, false).variant ==
			  GSHWRendererVariant::Classic);
static_assert(DecideHWRendererVariant(GSHWRendererVariant::Auto, true, true, false, true, false).reason ==
			  GSTileSelectionReason::AutoClassic);
