// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The hardware-renderer variant decision, swept exhaustively. The input space is small enough
// (3 variants x 2^3 device facts = 24 cases) that the invariants are checked over ALL of it,
// so a future edit cannot open a hole in a case nobody thought to name.

#include "GS/Renderers/Common/GSTileSelectionPolicy.h"

#include <gtest/gtest.h>

namespace
{
constexpr GSHWRendererVariant kVariants[] = {
	GSHWRendererVariant::Auto, GSHWRendererVariant::Classic, GSHWRendererVariant::Tile};
constexpr bool kBools[] = {false, true};
} // namespace

// The one hard safety rule: the Tile renderer has no non-Vulkan backend, so no combination of
// settings and device facts may ever select it off Vulkan.
TEST(GSTileSelectionPolicy, TileNeverSelectedOffVulkan)
{
	for (const GSHWRendererVariant variant : kVariants)
	{
		for (const bool gamedb : kBools)
		{
			for (const bool tiler : kBools)
			{
				const GSTileSelectionDecision decision =
					DecideHWRendererVariant(variant, /*is_vulkan=*/false, gamedb, tiler);
				EXPECT_FALSE(decision.use_tile);
			}
		}
	}
}

// An explicit user choice always wins: Classic ignores every device fact, Tile on Vulkan
// ignores the GameDB and the GPU profile (that is what makes a desktop or Apple TBDR box a
// legitimate Tile development vehicle).
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
				EXPECT_FALSE(classic.use_tile);
				EXPECT_EQ(classic.reason, GSTileSelectionReason::ExplicitClassic);

				const GSTileSelectionDecision tile =
					DecideHWRendererVariant(GSHWRendererVariant::Tile, vulkan, gamedb, tiler);
				EXPECT_EQ(tile.use_tile, vulkan);
				EXPECT_EQ(tile.reason, vulkan ? GSTileSelectionReason::ExplicitTile :
												GSTileSelectionReason::TileRequiresVulkan);
			}
		}
	}
}

// Auto promotes exactly the validated shape: a GameDB-recommended title, on Vulkan, on a
// mobile-tiler GPU profile. Desktop GPUs stay Classic on Auto even for promoted titles.
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
				EXPECT_EQ(decision.use_tile, expect_tile);
				EXPECT_EQ(decision.reason, expect_tile ? GSTileSelectionReason::AutoTile :
														 GSTileSelectionReason::AutoClassic);
			}
		}
	}
}

// The reason is what the renderer identity line logs and the oracle harness parses, so it must
// agree with the decision everywhere: a Tile decision carries a Tile reason, a Classic decision
// a Classic one, and every reason has a printable name.
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
					if (decision.use_tile)
					{
						EXPECT_TRUE(decision.reason == GSTileSelectionReason::ExplicitTile ||
									decision.reason == GSTileSelectionReason::AutoTile);
					}
					else
					{
						EXPECT_TRUE(decision.reason == GSTileSelectionReason::ExplicitClassic ||
									decision.reason == GSTileSelectionReason::TileRequiresVulkan ||
									decision.reason == GSTileSelectionReason::AutoClassic);
					}
					EXPECT_STRNE(GSTileSelectionReasonName(decision.reason), "unknown");
				}
			}
		}
	}
}
