// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSTargetRebase.h"

#include <gtest/gtest.h>
#include <set>

namespace
{
	using namespace GSTargetRebase;

	Safety SafeState()
	{
		Safety safety;
		safety.clean = true;
		safety.non_shuffle = true;
		safety.zero_width_page_offset = true;
		safety.no_current_source_reference = true;
		safety.no_bound_depth_reference = true;
		safety.existing_color_target = true;
		safety.native_scale = true;
		safety.native_extent_matches_valid = true;
		safety.valid_rgb = true;
		return safety;
	}

	std::set<u32> Words(Layout layout, Rect rect)
	{
		const GSOffset offset = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
		std::set<u32> words;
		for (int y = rect[1]; y < rect[3]; y++)
			for (int x = rect[0]; x < rect[2]; x++)
			{
				const u32 address = offset.pa(x, y);
				// P8 addresses bytes; the C32 and Z32 oracle addresses words.
				words.insert(layout.psm == PSMT8 ? address >> 2 : address);
			}
		return words;
	}

	size_t SharedWords(const std::set<u32>& left, const std::set<u32>& right)
	{
		size_t shared = 0;
		for (const u32 word : left)
			shared += right.contains(word);
		return shared;
	}

	TEST(GSTargetRebase, TombRaiderSuffixRetains57344WordsAndExcludesIncomingAndP8Aliases)
	{
		const Layout existing{0x2f20, 4, PSMCT32};
		const Rect valid{0, 0, 256, 288};
		const Rect drawn{0, 64, 256, 288};
		const Layout incoming{0x2c20, 4, PSMZ32};
		const Rect incoming_draw{0, 0, 256, 224};
		const Plan plan = MakeRebasePlan(existing, valid, drawn, incoming, incoming_draw, SafeState());
		ASSERT_EQ(plan.reason, Reason::Preserve);
		EXPECT_EQ(plan.preserved, drawn);
		EXPECT_EQ(plan.rebased, (Rect{0, 0, 256, 224}));
		EXPECT_EQ(plan.preserved_start, 0x3020u);
		EXPECT_EQ(plan.preserved_end, 0x33a0u);
		EXPECT_EQ(plan.incoming_start, 0x2c20u);
		EXPECT_EQ(plan.incoming_end, 0x2fa0u);

		const auto preserved = Words(existing, drawn);
		const auto rebased = Words({plan.preserved_start, existing.bw, existing.psm}, plan.rebased);
		const auto overwritten = Words(incoming, incoming_draw);
		EXPECT_EQ(preserved.size(), 57344u);
		EXPECT_EQ(rebased, preserved);
		EXPECT_EQ(overwritten.size(), 57344u);
		EXPECT_EQ(SharedWords(preserved, overwritten), 0u);
		EXPECT_EQ(SharedWords(Words(existing, valid), overwritten), 8192u);

		std::set<u32> late_p8;
		for (const u32 bp : {0x2f20u, 0x2f40u, 0x2f60u, 0x2f80u})
		{
			const auto page = Words({bp, 1, PSMT8}, {0, 0, 128, 64});
			late_p8.insert(page.begin(), page.end());
		}
		EXPECT_EQ(late_p8.size(), 8192u);
		EXPECT_EQ(SharedWords(late_p8, overwritten), late_p8.size());
		EXPECT_EQ(SharedWords(late_p8, rebased), 0u);
	}

	class GSTargetRebaseGuard : public ::testing::Test
	{
	protected:
		Layout color{0x1000, 4, PSMCT32};
		Layout depth{0x1200, 4, PSMZ32};
		Rect valid{0, 0, 256, 128};
		Rect drawn{0, 32, 256, 128};
		Rect write{0, 0, 256, 32};

		Reason Check(Safety safety)
		{
			return MakeRebasePlan(color, valid, drawn, depth, write, safety).reason;
		}
	};

	TEST_F(GSTargetRebaseGuard, DirtyShuffleOffsetAndLiveReferencesDecline)
	{
		for (bool Safety::*field : {&Safety::clean, &Safety::non_shuffle,
			&Safety::zero_width_page_offset, &Safety::no_current_source_reference,
			&Safety::no_bound_depth_reference})
		{
			Safety safety = SafeState();
			safety.*field = false;
			EXPECT_EQ(Check(safety), Reason::UnsafeState);
		}
	}

	TEST_F(GSTargetRebaseGuard, PhysicalOverlapInvalidValidityPagesAndWrapDecline)
	{
		const Safety safe = SafeState();
		EXPECT_EQ(MakeRebasePlan(color, valid, drawn, {0x1080, 4, PSMZ32}, write, safe).reason,
			Reason::PhysicalOverlap);
		EXPECT_EQ(MakeRebasePlan(color, valid, {0, 0, 256, 160}, depth, write, safe).reason,
			Reason::InvalidValidity);
		EXPECT_EQ(MakeRebasePlan(color, valid, {0, 33, 256, 128}, depth, write, safe).reason,
			Reason::InvalidPageRect);
		EXPECT_EQ(MakeRebasePlan(color, valid, drawn, depth, {0, 0, 255, 32}, safe).reason,
			Reason::InvalidPageRect);
		EXPECT_EQ(MakeRebasePlan({0x3fe0, 4, PSMCT32}, valid, drawn, depth, write, safe).reason,
			Reason::Wrapped);
		EXPECT_EQ(MakeRebasePlan(color, valid, {0, 0, 0, 0}, depth, write, safe).reason,
			Reason::InvalidValidity);
		EXPECT_EQ(MakeRebasePlan(color, valid, drawn, {0x1200, 2, PSMZ32}, write, safe).reason,
			Reason::WidthMismatch);
		EXPECT_EQ(MakeRebasePlan(color, valid, drawn, {0x1200, 4, PSMCT32}, write, safe).reason,
			Reason::UnsupportedLayout);
	}

	TEST_F(GSTargetRebaseGuard, OnlyNativeColorFullWidthSuffixWithExactAllocationAndRGBIsAccepted)
	{
		Safety safety = SafeState();
		safety.existing_color_target = false;
		EXPECT_EQ(Check(safety), Reason::RequiresColorC32Native);
		safety = SafeState();
		safety.native_scale = false;
		EXPECT_EQ(Check(safety), Reason::RequiresColorC32Native);
		EXPECT_EQ(MakeRebasePlan({0x1000, 4, PSMZ32}, valid, drawn,
			{0x1200, 4, PSMCT32}, write, SafeState()).reason, Reason::UnsupportedLayout);
		for (const Rect suffix : {Rect{64, 32, 256, 128}, Rect{0, 32, 192, 128}, Rect{0, 0, 256, 128}})
			EXPECT_EQ(MakeRebasePlan(color, valid, suffix, depth, write, SafeState()).reason,
				Reason::RequiresFullWidthSuffix);
		safety = SafeState();
		safety.native_extent_matches_valid = false;
		EXPECT_EQ(Check(safety), Reason::RequiresExactAllocation);
		safety = SafeState();
		safety.valid_rgb = false;
		EXPECT_EQ(Check(safety), Reason::RequiresValidRGB);
	}
} // namespace
