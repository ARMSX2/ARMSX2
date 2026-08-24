// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The hardware-renderer variant decision, swept exhaustively. The input space is small enough
// (4 variants x 2^5 device facts = 128 cases) that the invariants are checked over ALL of it,
// so a future edit cannot open a hole in a case nobody thought to name.

#include "GS/Renderers/Common/GSTileSelectionPolicy.h"

#include <gtest/gtest.h>

namespace
{
	constexpr GSHWRendererVariant kVariants[] = {GSHWRendererVariant::Auto, GSHWRendererVariant::Classic,
		GSHWRendererVariant::Tile, GSHWRendererVariant::TileGpu};
	constexpr bool kBools[] = {false, true};

	// The sweep body, so every test below iterates the same 128 cases and a new input cannot be added
	// to some of them and forgotten in the rest. `fn` takes the decision and the five device facts.
	template <typename F>
	void ForEveryCase(F&& fn)
	{
		for (const GSHWRendererVariant variant : kVariants)
		{
			for (const bool vulkan : kBools)
			{
				for (const bool serves : kBools)
				{
					for (const bool ignore : kBools)
					{
						for (const bool gamedb : kBools)
						{
							for (const bool tiler : kBools)
							{
								fn(DecideHWRendererVariant(variant, vulkan, serves, ignore, gamedb, tiler),
									variant, vulkan, serves, ignore, gamedb, tiler);
							}
						}
					}
				}
			}
		}
	}
} // namespace

// The one hard safety rule: neither the Tile renderer nor TileGpu has a non-Vulkan backend, so
// off Vulkan every combination of settings and device facts must resolve Classic. The contract
// escape does not weaken this — it waives the contract, never the API.
TEST(GSTileSelectionPolicy, OffVulkanAlwaysResolvesClassic)
{
	ForEveryCase([](const GSTileSelectionDecision& decision, GSHWRendererVariant, bool vulkan, bool, bool,
					 bool, bool) {
		if (!vulkan)
			EXPECT_EQ(decision.variant, GSHWRendererVariant::Classic);
	});
}

// The decision is RESOLVED: whatever the inputs, the outcome is a concrete implementation,
// never Auto.
TEST(GSTileSelectionPolicy, DecisionNeverResolvesAuto)
{
	ForEveryCase([](const GSTileSelectionDecision& decision, GSHWRendererVariant, bool, bool, bool, bool,
					 bool) { EXPECT_NE(decision.variant, GSHWRendererVariant::Auto); });
}

// An explicit user choice always wins over the GameDB and the GPU profile: Classic ignores every
// device fact, Tile on Vulkan ignores both (that is what makes a desktop or Apple TBDR box a
// legitimate development vehicle). TileGpu is the exception and has its own test below — it is
// the one variant that also answers to a device CAPABILITY, not just to the API.
TEST(GSTileSelectionPolicy, ExplicitChoiceIgnoresPromotionInputs)
{
	for (const bool vulkan : kBools)
	{
		for (const bool serves : kBools)
		{
			for (const bool ignore : kBools)
			{
				for (const bool gamedb : kBools)
				{
					for (const bool tiler : kBools)
					{
						const GSTileSelectionDecision classic = DecideHWRendererVariant(
							GSHWRendererVariant::Classic, vulkan, serves, ignore, gamedb, tiler);
						EXPECT_EQ(classic.variant, GSHWRendererVariant::Classic);
						EXPECT_EQ(classic.reason, GSTileSelectionReason::ExplicitClassic);

						const GSTileSelectionDecision tile = DecideHWRendererVariant(
							GSHWRendererVariant::Tile, vulkan, serves, ignore, gamedb, tiler);
						EXPECT_EQ(tile.variant, vulkan ? GSHWRendererVariant::Tile : GSHWRendererVariant::Classic);
						EXPECT_EQ(tile.reason, vulkan ? GSTileSelectionReason::ExplicitTile :
														GSTileSelectionReason::TileRequiresVulkan);
					}
				}
			}
		}
	}
}

// TileGpu's full truth table. Two vetoes, in this order: the API, then the device contract.
//
// The contract veto is the fallback that landed 2026-08-24 — a contract-absent device has no
// executor, so TileGpu would build, discard every draw and present black with a clean exit code.
// Classic instead. The escape (TileGpuIgnoreDeviceContract) is fail-closed and forces construction
// for bring-up, and it must NOT be able to resurrect TileGpu off Vulkan.
TEST(GSTileSelectionPolicy, TileGpuAnswersToApiThenContract)
{
	for (const bool vulkan : kBools)
	{
		for (const bool serves : kBools)
		{
			for (const bool ignore : kBools)
			{
				for (const bool gamedb : kBools)
				{
					for (const bool tiler : kBools)
					{
						const GSTileSelectionDecision d = DecideHWRendererVariant(
							GSHWRendererVariant::TileGpu, vulkan, serves, ignore, gamedb, tiler);

						if (!vulkan)
						{
							EXPECT_EQ(d.variant, GSHWRendererVariant::Classic);
							EXPECT_EQ(d.reason, GSTileSelectionReason::TileGpuRequiresVulkan);
						}
						else if (serves)
						{
							EXPECT_EQ(d.variant, GSHWRendererVariant::TileGpu);
							EXPECT_EQ(d.reason, GSTileSelectionReason::ExplicitTileGpu);
						}
						else if (ignore)
						{
							EXPECT_EQ(d.variant, GSHWRendererVariant::TileGpu);
							EXPECT_EQ(d.reason, GSTileSelectionReason::TileGpuContractOverridden);
						}
						else
						{
							EXPECT_EQ(d.variant, GSHWRendererVariant::Classic);
							EXPECT_EQ(d.reason, GSTileSelectionReason::TileGpuRequiresDeviceContract);
						}
					}
				}
			}
		}
	}
}

// The escape is FAIL-CLOSED, pinned on its own so the default cannot drift: with the key off, a
// Vulkan device that fails the contract resolves Classic, and that is the only outcome.
TEST(GSTileSelectionPolicy, ContractEscapeIsFailClosed)
{
	for (const bool gamedb : kBools)
	{
		for (const bool tiler : kBools)
		{
			const GSTileSelectionDecision d = DecideHWRendererVariant(GSHWRendererVariant::TileGpu,
				/*is_vulkan=*/true, /*device_serves_tilegpu=*/false, /*ignore_device_contract=*/false, gamedb,
				tiler);
			EXPECT_EQ(d.variant, GSHWRendererVariant::Classic);
			EXPECT_EQ(d.reason, GSTileSelectionReason::TileGpuRequiresDeviceContract);
		}
	}
}

// The contract fact is TileGpu's alone. A device that cannot serve the executor changes nothing
// about Classic, about the Tile renderer (which runs on the CPU and asks the device for nothing
// beyond the API), or about what Auto promotes.
TEST(GSTileSelectionPolicy, ContractDoesNotDisturbTheOtherVariants)
{
	for (const GSHWRendererVariant variant :
		{GSHWRendererVariant::Auto, GSHWRendererVariant::Classic, GSHWRendererVariant::Tile})
	{
		for (const bool vulkan : kBools)
		{
			for (const bool ignore : kBools)
			{
				for (const bool gamedb : kBools)
				{
					for (const bool tiler : kBools)
					{
						const GSTileSelectionDecision served =
							DecideHWRendererVariant(variant, vulkan, true, ignore, gamedb, tiler);
						const GSTileSelectionDecision absent =
							DecideHWRendererVariant(variant, vulkan, false, ignore, gamedb, tiler);
						EXPECT_EQ(served.variant, absent.variant);
						EXPECT_EQ(served.reason, absent.reason);
					}
				}
			}
		}
	}
}

// Auto promotes exactly the validated shape: a GameDB-recommended title, on Vulkan, on a
// mobile-tiler GPU profile — and it promotes to Tile, never TileGpu (the promotion evidence is
// Tile evidence; TileGpu earns its own promotion input if and when it graduates). Desktop GPUs
// stay Classic on Auto even for promoted titles.
TEST(GSTileSelectionPolicy, AutoPromotesOnlyTheValidatedShape)
{
	for (const bool vulkan : kBools)
	{
		for (const bool serves : kBools)
		{
			for (const bool ignore : kBools)
			{
				for (const bool gamedb : kBools)
				{
					for (const bool tiler : kBools)
					{
						const GSTileSelectionDecision decision = DecideHWRendererVariant(
							GSHWRendererVariant::Auto, vulkan, serves, ignore, gamedb, tiler);
						const bool expect_tile = gamedb && vulkan && tiler;
						EXPECT_EQ(decision.variant,
							expect_tile ? GSHWRendererVariant::Tile : GSHWRendererVariant::Classic);
						EXPECT_EQ(decision.reason, expect_tile ? GSTileSelectionReason::AutoTile :
																 GSTileSelectionReason::AutoClassic);
					}
				}
			}
		}
	}
}

// The reason is what the renderer identity line logs and the oracle harness parses, so it must
// agree with the decision everywhere: a Tile resolution carries a Tile reason, a TileGpu
// resolution one of the two TileGpu-running reasons, a Classic resolution one of the
// Classic-running reasons, and every reason has a printable name.
TEST(GSTileSelectionPolicy, ReasonAgreesWithDecisionEverywhere)
{
	ForEveryCase([](const GSTileSelectionDecision& decision, GSHWRendererVariant, bool, bool, bool, bool,
					 bool) {
		switch (decision.variant)
		{
			case GSHWRendererVariant::Tile:
				EXPECT_TRUE(decision.reason == GSTileSelectionReason::ExplicitTile ||
							decision.reason == GSTileSelectionReason::AutoTile);
				break;
			case GSHWRendererVariant::TileGpu:
				EXPECT_TRUE(decision.reason == GSTileSelectionReason::ExplicitTileGpu ||
							decision.reason == GSTileSelectionReason::TileGpuContractOverridden);
				break;
			default:
				EXPECT_TRUE(decision.reason == GSTileSelectionReason::ExplicitClassic ||
							decision.reason == GSTileSelectionReason::TileRequiresVulkan ||
							decision.reason == GSTileSelectionReason::TileGpuRequiresVulkan ||
							decision.reason == GSTileSelectionReason::TileGpuRequiresDeviceContract ||
							decision.reason == GSTileSelectionReason::AutoClassic);
				break;
		}
		EXPECT_STRNE(GSTileSelectionReasonName(decision.reason), "unknown");
	});
}

// Every reason has a DISTINCT name. The harness parses these, so two reasons sharing a string
// would make two different outcomes indistinguishable in a log.
TEST(GSTileSelectionPolicy, ReasonNamesAreDistinct)
{
	constexpr GSTileSelectionReason kReasons[] = {GSTileSelectionReason::ExplicitClassic,
		GSTileSelectionReason::ExplicitTile, GSTileSelectionReason::TileRequiresVulkan,
		GSTileSelectionReason::AutoClassic, GSTileSelectionReason::AutoTile,
		GSTileSelectionReason::ExplicitTileGpu, GSTileSelectionReason::TileGpuRequiresVulkan,
		GSTileSelectionReason::TileGpuRequiresDeviceContract, GSTileSelectionReason::TileGpuContractOverridden};

	for (const GSTileSelectionReason a : kReasons)
	{
		EXPECT_STRNE(GSTileSelectionReasonName(a), "unknown");
		for (const GSTileSelectionReason b : kReasons)
		{
			if (a == b)
				continue;
			EXPECT_STRNE(GSTileSelectionReasonName(a), GSTileSelectionReasonName(b));
		}
	}
}

// M4a: the blend-accuracy setting has exactly three non-consumers — the SW renderer, and the
// Tile and TileGpu variants where they actually run (Vulkan). TileGpu inherits Tile's charter:
// one shipped blending behavior, decided by the lowering/planner, never a runtime accuracy
// mode. Everything that nags, clamps or cycles AccurateBlendingUnit gates on this predicate,
// so these pins are the whole user-visible contract: an explicit Tile or TileGpu choice off
// Vulkan runs Classic and keeps the setting live, and Auto keeps it live until the GameDB
// promotion input exists (M8 revisits the Auto arm alongside that wiring).
TEST(GSTileSelectionPolicy, BlendAccuracyConsultedOnlyWhereSomethingReadsIt)
{
	// The dead pairs.
	EXPECT_FALSE(GSAccurateBlendingUnitConsulted(GSRendererType::SW, GSHWRendererVariant::Auto));
	EXPECT_FALSE(GSAccurateBlendingUnitConsulted(GSRendererType::SW, GSHWRendererVariant::Classic));
	EXPECT_FALSE(GSAccurateBlendingUnitConsulted(GSRendererType::SW, GSHWRendererVariant::Tile));
	EXPECT_FALSE(GSAccurateBlendingUnitConsulted(GSRendererType::SW, GSHWRendererVariant::TileGpu));
	EXPECT_FALSE(GSAccurateBlendingUnitConsulted(GSRendererType::Null, GSHWRendererVariant::Auto));
	EXPECT_FALSE(GSAccurateBlendingUnitConsulted(GSRendererType::VK, GSHWRendererVariant::Tile));
	EXPECT_FALSE(GSAccurateBlendingUnitConsulted(GSRendererType::VK, GSHWRendererVariant::TileGpu));

	// Classic runs and reads it: every other renderer/variant pairing.
	EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::VK, GSHWRendererVariant::Auto));
	EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::VK, GSHWRendererVariant::Classic));
	EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::OGL, GSHWRendererVariant::Tile));
	EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::Metal, GSHWRendererVariant::Tile));
	EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::DX12, GSHWRendererVariant::Tile));
	EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::OGL, GSHWRendererVariant::TileGpu));
	EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::Metal, GSHWRendererVariant::TileGpu));
	EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::DX12, GSHWRendererVariant::TileGpu));

	// The Tile/TileGpu-off-Vulkan pairings must agree with the selection decision itself:
	// wherever the variant decision runs Classic, the setting is consulted. The contract
	// fallback adds a second such pairing — TileGpu on Vulkan with no executor also runs
	// Classic — but there the RESOLVED renderer is Classic-on-Vulkan, which reads the setting
	// through the Classic arm of the predicate, so nothing new is dead.
	for (const bool tiler : {false, true})
	{
		const GSTileSelectionDecision tile = DecideHWRendererVariant(GSHWRendererVariant::Tile,
			/*is_vulkan=*/false, /*device_serves_tilegpu=*/false, /*ignore_device_contract=*/false, false, tiler);
		EXPECT_EQ(tile.variant, GSHWRendererVariant::Classic);
		EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::OGL, GSHWRendererVariant::Tile));

		const GSTileSelectionDecision tilegpu = DecideHWRendererVariant(GSHWRendererVariant::TileGpu,
			/*is_vulkan=*/false, /*device_serves_tilegpu=*/false, /*ignore_device_contract=*/false, false, tiler);
		EXPECT_EQ(tilegpu.variant, GSHWRendererVariant::Classic);
		EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::OGL, GSHWRendererVariant::TileGpu));
	}
}
