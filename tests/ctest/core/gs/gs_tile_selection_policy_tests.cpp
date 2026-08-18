// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The hardware-renderer variant decision, swept exhaustively. The input space is small enough
// (4 variants x 2^3 device facts = 32 cases) that the invariants are checked over ALL of it,
// so a future edit cannot open a hole in a case nobody thought to name.

#include "GS/Renderers/Common/GSTileSelectionPolicy.h"

#include <gtest/gtest.h>

namespace
{
constexpr GSHWRendererVariant kVariants[] = {GSHWRendererVariant::Auto, GSHWRendererVariant::Classic,
	GSHWRendererVariant::Tile, GSHWRendererVariant::TileGpu};
constexpr bool kBools[] = {false, true};
} // namespace

// The one hard safety rule: neither the Tile renderer nor TileGpu has a non-Vulkan backend, so
// off Vulkan every combination of settings and device facts must resolve Classic.
TEST(GSTileSelectionPolicy, OffVulkanAlwaysResolvesClassic)
{
	for (const GSHWRendererVariant variant : kVariants)
	{
		for (const bool gamedb : kBools)
		{
			for (const bool tiler : kBools)
			{
				const GSTileSelectionDecision decision =
					DecideHWRendererVariant(variant, /*is_vulkan=*/false, gamedb, tiler);
				EXPECT_EQ(decision.variant, GSHWRendererVariant::Classic);
			}
		}
	}
}

// The decision is RESOLVED: whatever the inputs, the outcome is a concrete implementation,
// never Auto.
TEST(GSTileSelectionPolicy, DecisionNeverResolvesAuto)
{
	for (const GSHWRendererVariant variant : kVariants)
	{
		for (const bool vulkan : kBools)
		{
			for (const bool gamedb : kBools)
			{
				for (const bool tiler : kBools)
				{
					const GSTileSelectionDecision decision =
						DecideHWRendererVariant(variant, vulkan, gamedb, tiler);
					EXPECT_NE(decision.variant, GSHWRendererVariant::Auto);
				}
			}
		}
	}
}

// An explicit user choice always wins: Classic ignores every device fact, Tile and TileGpu on
// Vulkan ignore the GameDB and the GPU profile (that is what makes a desktop or Apple TBDR box
// a legitimate development vehicle for either).
TEST(GSTileSelectionPolicy, ExplicitChoiceIgnoresDeviceFacts)
{
	for (const bool vulkan : kBools)
	{
		for (const bool gamedb : kBools)
		{
			for (const bool tiler : kBools)
			{
				const GSTileSelectionDecision classic =
					DecideHWRendererVariant(GSHWRendererVariant::Classic, vulkan, gamedb, tiler);
				EXPECT_EQ(classic.variant, GSHWRendererVariant::Classic);
				EXPECT_EQ(classic.reason, GSTileSelectionReason::ExplicitClassic);

				const GSTileSelectionDecision tile =
					DecideHWRendererVariant(GSHWRendererVariant::Tile, vulkan, gamedb, tiler);
				EXPECT_EQ(tile.variant, vulkan ? GSHWRendererVariant::Tile : GSHWRendererVariant::Classic);
				EXPECT_EQ(tile.reason, vulkan ? GSTileSelectionReason::ExplicitTile :
												GSTileSelectionReason::TileRequiresVulkan);

				const GSTileSelectionDecision tilegpu =
					DecideHWRendererVariant(GSHWRendererVariant::TileGpu, vulkan, gamedb, tiler);
				EXPECT_EQ(tilegpu.variant,
					vulkan ? GSHWRendererVariant::TileGpu : GSHWRendererVariant::Classic);
				EXPECT_EQ(tilegpu.reason, vulkan ? GSTileSelectionReason::ExplicitTileGpu :
												   GSTileSelectionReason::TileGpuRequiresVulkan);
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
		for (const bool gamedb : kBools)
		{
			for (const bool tiler : kBools)
			{
				const GSTileSelectionDecision decision =
					DecideHWRendererVariant(GSHWRendererVariant::Auto, vulkan, gamedb, tiler);
				const bool expect_tile = gamedb && vulkan && tiler;
				EXPECT_EQ(decision.variant,
					expect_tile ? GSHWRendererVariant::Tile : GSHWRendererVariant::Classic);
				EXPECT_EQ(decision.reason, expect_tile ? GSTileSelectionReason::AutoTile :
														 GSTileSelectionReason::AutoClassic);
			}
		}
	}
}

// The reason is what the renderer identity line logs and the oracle harness parses, so it must
// agree with the decision everywhere: a Tile resolution carries a Tile reason, a TileGpu
// resolution a TileGpu one, a Classic resolution one of the Classic-running reasons, and every
// reason has a printable name.
TEST(GSTileSelectionPolicy, ReasonAgreesWithDecisionEverywhere)
{
	for (const GSHWRendererVariant variant : kVariants)
	{
		for (const bool vulkan : kBools)
		{
			for (const bool gamedb : kBools)
			{
				for (const bool tiler : kBools)
				{
					const GSTileSelectionDecision decision =
						DecideHWRendererVariant(variant, vulkan, gamedb, tiler);
					switch (decision.variant)
					{
						case GSHWRendererVariant::Tile:
							EXPECT_TRUE(decision.reason == GSTileSelectionReason::ExplicitTile ||
										decision.reason == GSTileSelectionReason::AutoTile);
							break;
						case GSHWRendererVariant::TileGpu:
							EXPECT_EQ(decision.reason, GSTileSelectionReason::ExplicitTileGpu);
							break;
						default:
							EXPECT_TRUE(decision.reason == GSTileSelectionReason::ExplicitClassic ||
										decision.reason == GSTileSelectionReason::TileRequiresVulkan ||
										decision.reason == GSTileSelectionReason::TileGpuRequiresVulkan ||
										decision.reason == GSTileSelectionReason::AutoClassic);
							break;
					}
					EXPECT_STRNE(GSTileSelectionReasonName(decision.reason), "unknown");
				}
			}
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
	// wherever the variant decision runs Classic, the setting is consulted.
	for (const bool tiler : {false, true})
	{
		const GSTileSelectionDecision tile =
			DecideHWRendererVariant(GSHWRendererVariant::Tile, /*is_vulkan=*/false, false, tiler);
		EXPECT_EQ(tile.variant, GSHWRendererVariant::Classic);
		EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::OGL, GSHWRendererVariant::Tile));

		const GSTileSelectionDecision tilegpu =
			DecideHWRendererVariant(GSHWRendererVariant::TileGpu, /*is_vulkan=*/false, false, tiler);
		EXPECT_EQ(tilegpu.variant, GSHWRendererVariant::Classic);
		EXPECT_TRUE(GSAccurateBlendingUnitConsulted(GSRendererType::OGL, GSHWRendererVariant::TileGpu));
	}
}
