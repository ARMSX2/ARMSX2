// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSExactDirtyMapping.h"

#include <gtest/gtest.h>
#include <set>

namespace
{
	using namespace GSExactDirtyMapping;
	using Pixels = std::set<std::pair<int, int>>;

	u32 BlockAddress(Layout layout, int x, int y)
	{
		return GSOffset::fromKnownPSM(layout.bp, layout.bw, PSMCT32).bnNoWrap(x, y);
	}

	std::set<u32> WrittenWords(Layout layout, Rect rect)
	{
		const GSOffset offset = GSOffset::fromKnownPSM(layout.bp, layout.bw, PSMCT32);
		std::set<u32> words;
		for (int y = rect.y; y < rect.w; y++)
			for (int x = rect.x; x < rect.z; x++)
				words.insert(offset.pa(x, y));
		return words;
	}

	Pixels ExpectedPixels(Layout source, Rect write, Layout receiver, Rect valid)
	{
		const auto written = WrittenWords(source, write);
		const GSOffset offset = GSOffset::fromKnownPSM(receiver.bp, receiver.bw, PSMCT32);
		Pixels expected;
		for (int y = valid.y; y < valid.w; y++)
			for (int x = valid.x; x < valid.z; x++)
				if (written.contains(offset.pa(x, y)))
					expected.emplace(x, y);
		return expected;
	}

	void ExpectExactProjection(Layout source, Rect write, Layout receiver, Rect valid, size_t count)
	{
		const Plan plan = MapC32Blocks(source, write, receiver, valid, BlockAddress);
		ASSERT_TRUE(plan.supported);
		Pixels actual;
		for (const Rect rect : plan.rects)
		{
			ASSERT_GE(rect.x, valid.x);
			ASSERT_GE(rect.y, valid.y);
			ASSERT_LE(rect.z, valid.z);
			ASSERT_LE(rect.w, valid.w);
			ASSERT_LT(rect.x, rect.z);
			ASSERT_LT(rect.y, rect.w);
			for (int y = rect.y; y < rect.w; y++)
				for (int x = rect.x; x < rect.z; x++)
					ASSERT_TRUE(actual.emplace(x, y).second) << "duplicate pixel " << x << "," << y;
		}
		const Pixels expected = ExpectedPixels(source, write, receiver, valid);
		EXPECT_EQ(expected.size(), count);
		EXPECT_EQ(actual, expected);
	}

	TEST(GSExactDirtyMapping, NonPageAlignedBaseAndDifferentWidthUsePhysicalAddresses)
	{
		ExpectExactProjection({0x2eb8, 2, PSMCT32}, {0, 0, 128, 128},
			{0x2fa0, 4, PSMCT32}, {0, 0, 256, 256}, 1536);
		ExpectExactProjection({0x2fb8, 1, PSMCT32}, {0, 0, 16, 16},
			{0x2fa0, 4, PSMCT32}, {0, 0, 256, 256}, 256);
	}

	TEST(GSExactDirtyMapping, LegacyRectangleIncludesUnwrittenWords)
	{
		const auto written = WrittenWords({0x2eb8, 2, PSMCT32}, {0, 0, 128, 128});
		const auto legacy = WrittenWords({0x2fa0, 4, PSMCT32}, {0, 24, 128, 56});
		size_t extra = 0;
		for (const u32 word : legacy)
			extra += !written.contains(word);
		EXPECT_EQ(extra, 3840u);
	}

	TEST(GSExactDirtyMapping, SameLayoutDisjointLayoutAndPartialValidityEdges)
	{
		ExpectExactProjection({0x1000, 4, PSMCT32}, {8, 16, 120, 80},
			{0x1000, 4, PSMCT32}, {0, 0, 256, 128}, 112 * 64);
		ExpectExactProjection({0x1000, 1, PSMCT32}, {0, 0, 64, 32},
			{0x3000, 2, PSMCT32}, {0, 0, 128, 64}, 0);
		ExpectExactProjection({0x2eb8, 2, PSMCT32}, {0, 0, 128, 128},
			{0x2fa0, 4, PSMCT32}, {5, 3, 119, 27}, 59 * 13 + 27 * 11);
	}

	TEST(GSExactDirtyMapping, RandomizedWidthsAndBothBaseDeltaSignsMatchWordAddressOracle)
	{
		for (u32 seed = 0; seed < 128; seed++)
		{
			SCOPED_TRACE(seed);
			const u32 bp = 0x800 + (seed * 17) % 512;
			const u32 delta = (seed * 13) % 160;
			const Layout source{bp, 1 + seed % 8, PSMCT32};
			const Layout receiver{bp + delta, 1 + (seed * 7) % 8, PSMCT32};
			const Rect write{int(seed % 4) * 8, int(seed % 3) * 8,
				int(seed % 4) * 8 + 64, int(seed % 3) * 8 + 64};
			const Rect valid{3, 5, 127, 91};
			ExpectExactProjection(source, write, receiver, valid,
				ExpectedPixels(source, write, receiver, valid).size());
			const Rect reverse_write{0, 0, 64, 64};
			const Rect reverse_valid{1, 7, 125, 93};
			ExpectExactProjection(receiver, reverse_write, source, reverse_valid,
				ExpectedPixels(receiver, reverse_write, source, reverse_valid).size());
		}
	}

	TEST(GSExactDirtyMapping, SameLayoutHorizontalPageAliasesStillMatchWordAddressOracle)
	{
		const Layout layout{0x1000, 1, PSMCT32};
		const Rect write{64, 0, 128, 32};
		const Rect valid{0, 0, 64, 96};
		const auto expected = ExpectedPixels(layout, write, layout, valid);
		ASSERT_FALSE(expected.empty());
		ExpectExactProjection(layout, write, layout, valid, expected.size());
		const Rect receiver_alias{0, 0, 128, 64};
		const Rect native_write{0, 32, 64, 64};
		ExpectExactProjection(layout, native_write, layout, receiver_alias,
			ExpectedPixels(layout, native_write, layout, receiver_alias).size());
	}

	void ExpectUnsupported(Layout source, Rect write, Layout receiver, Rect valid)
	{
		const Plan plan = MapC32Blocks(source, write, receiver, valid, BlockAddress);
		EXPECT_FALSE(plan.supported);
		EXPECT_TRUE(plan.rects.empty());
	}

	TEST(GSExactDirtyMapping, UnsupportedLayoutsAndBoundsEmitNoPartialPlan)
	{
		const Layout color{0x1000, 4, PSMCT32};
		const Rect write{0, 0, 64, 32};
		const Rect valid{0, 0, 256, 128};
		ExpectUnsupported({0x1000, 0, PSMCT32}, write, color, valid);
		ExpectUnsupported(color, write, {0x1000, 0, PSMCT32}, valid);
		ExpectUnsupported({0x1000, 4, PSMCT24}, write, color, valid);
		ExpectUnsupported(color, write, {0x1000, 4, PSMZ32}, valid);
		ExpectUnsupported(color, {1, 0, 64, 32}, color, valid);
		ExpectUnsupported(color, {0, 0, 63, 32}, color, valid);
		ExpectUnsupported(color, {0, 0, 0, 32}, color, valid);
		ExpectUnsupported(color, write, color, {0, 0, 0, 32});
		ExpectUnsupported(color, {0, 0, 2048, 4096}, color, valid);
		ExpectUnsupported(color, write, color, {0, 0, 2048, 4096});
		ExpectUnsupported(color, {0, 0, 2056, 8}, color, valid);
		ExpectUnsupported(color, write, color, {-1, 0, 64, 32});
	}

	TEST(GSExactDirtyMapping, PhysicalWrapAndInvalidCallbackEmitNoPartialPlan)
	{
		const Layout color{0x1000, 4, PSMCT32};
		const Rect write{0, 0, 64, 32};
		const Rect valid{0, 0, 256, 128};
		ExpectUnsupported({16383, 1, PSMCT32}, write, color, valid);
		ExpectUnsupported(color, write, {16383, 1, PSMCT32}, write);
		const Plan plan = MapC32Blocks(color, write, {0x1001, 4, PSMCT32}, valid,
			[](Layout, int, int) { return 16384u; });
		EXPECT_FALSE(plan.supported);
		EXPECT_TRUE(plan.rects.empty());
	}
} // namespace
