// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSPageBitmap pinned against an independent scalar reference (std::bitset<512>):
// directed edges on the lane/word boundaries, randomized op-sequence sweeps, and
// page-set construction pinned against GSOffset's own PageLooper enumeration —
// including the wrap (slowPath) geometry, where a footprint runs past page 511 and
// folds back onto the start of GS memory.

#include "GS/Renderers/Tile/GSPageBitmap.h"

#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <bitset>
#include <random>
#include <vector>

namespace
{
using RefSet = std::bitset<GS_MAX_PAGES>;

bool Matches(const GSPageBitmap& bm, const RefSet& ref)
{
	for (u32 page = 0; page < GS_MAX_PAGES; page++)
	{
		if (bm.test(page) != ref.test(page))
			return false;
	}
	return true;
}

RefSet RefFromLooper(const GSOffset& off, const GSVector4i& rect)
{
	RefSet ref;
	off.loopPages(rect, [&ref](u32 page) { ref.set(page); });
	return ref;
}

// Word (64-bit) and lane (128-bit) boundaries, plus both ends of the space.
constexpr u32 kEdgePages[] = {0, 1, 62, 63, 64, 65, 127, 128, 129, 191, 192, 255, 256, 319, 320, 383, 384, 447, 448, 510, 511};
} // namespace

TEST(GSPageBitmap, DefaultIsEmpty)
{
	const GSPageBitmap bm;
	EXPECT_TRUE(bm.empty());
	EXPECT_EQ(bm.count(), 0u);
	for (u32 page = 0; page < GS_MAX_PAGES; page++)
		EXPECT_FALSE(bm.test(page));
	bm.forEachSetPage([](u32) { FAIL() << "no page should be enumerated from an empty set"; });
}

TEST(GSPageBitmap, SetTestUnsetEveryPage)
{
	for (u32 page = 0; page < GS_MAX_PAGES; page++)
	{
		GSPageBitmap bm;
		bm.set(page);
		EXPECT_FALSE(bm.empty());
		EXPECT_EQ(bm.count(), 1u);
		EXPECT_TRUE(bm.test(page));

		u32 visited = 0;
		bm.forEachSetPage([&](u32 p) {
			EXPECT_EQ(p, page);
			visited++;
		});
		EXPECT_EQ(visited, 1u);

		bm.unset(page);
		EXPECT_TRUE(bm.empty());
	}
}

TEST(GSPageBitmap, EdgePagesPairwise)
{
	for (const u32 a : kEdgePages)
	{
		GSPageBitmap ba;
		ba.set(a);
		for (const u32 b : kEdgePages)
		{
			GSPageBitmap bb;
			bb.set(b);
			EXPECT_EQ(ba.intersects(bb), a == b);
			EXPECT_EQ(ba.contains(bb), a == b);
			EXPECT_EQ(ba == bb, a == b);

			const GSPageBitmap u = ba | bb;
			EXPECT_EQ(u.count(), (a == b) ? 1u : 2u);
			EXPECT_TRUE(u.contains(ba));
			EXPECT_TRUE(u.contains(bb));

			const GSPageBitmap i = ba & bb;
			EXPECT_EQ(i.empty(), a != b);

			const GSPageBitmap d = ba.andnot(bb);
			EXPECT_EQ(d.test(a), a != b);
			EXPECT_FALSE(d.test(b) && a != b);
		}
	}
}

TEST(GSPageBitmap, RandomizedOpsVsBitset)
{
	std::mt19937_64 rng(0x67766f41); // fixed seed: deterministic suite
	std::uniform_int_distribution<u32> page_dist(0, GS_MAX_PAGES - 1);
	std::uniform_int_distribution<u32> op_dist(0, 5);

	GSPageBitmap bm_a, bm_b;
	RefSet ref_a, ref_b;

	for (int iter = 0; iter < 20000; iter++)
	{
		const u32 page = page_dist(rng);
		switch (op_dist(rng))
		{
			case 0:
				bm_a.set(page);
				ref_a.set(page);
				break;
			case 1:
				bm_a.unset(page);
				ref_a.reset(page);
				break;
			case 2:
				bm_b.set(page);
				ref_b.set(page);
				break;
			case 3:
				bm_a |= bm_b;
				ref_a |= ref_b;
				break;
			case 4:
				bm_a &= bm_b;
				ref_a &= ref_b;
				break;
			case 5:
				bm_b = bm_b.andnot(bm_a);
				ref_b &= ~ref_a;
				break;
		}

		ASSERT_TRUE(Matches(bm_a, ref_a)) << "iter " << iter;
		ASSERT_TRUE(Matches(bm_b, ref_b)) << "iter " << iter;
		ASSERT_EQ(bm_a.count(), static_cast<u32>(ref_a.count())) << "iter " << iter;
		ASSERT_EQ(bm_a.empty(), ref_a.none()) << "iter " << iter;
		ASSERT_EQ(bm_a.intersects(bm_b), (ref_a & ref_b).any()) << "iter " << iter;
		ASSERT_EQ(bm_a.contains(bm_b), (ref_b & ~ref_a).none()) << "iter " << iter;
		ASSERT_EQ(bm_a == bm_b, ref_a == ref_b) << "iter " << iter;
	}
}

TEST(GSPageBitmap, ForEachSetPageIsAscendingAndComplete)
{
	std::mt19937_64 rng(0x67766f42);
	std::uniform_int_distribution<u32> page_dist(0, GS_MAX_PAGES - 1);
	std::uniform_int_distribution<u32> pop_dist(0, 128);

	for (int iter = 0; iter < 500; iter++)
	{
		GSPageBitmap bm;
		RefSet ref;
		const u32 pop = pop_dist(rng);
		for (u32 i = 0; i < pop; i++)
		{
			const u32 page = page_dist(rng);
			bm.set(page);
			ref.set(page);
		}

		std::vector<u32> seen;
		bm.forEachSetPage([&](u32 page) { seen.push_back(page); });

		ASSERT_EQ(seen.size(), ref.count()) << "iter " << iter;
		for (size_t i = 0; i < seen.size(); i++)
		{
			ASSERT_TRUE(ref.test(seen[i])) << "iter " << iter;
			if (i > 0)
				ASSERT_LT(seen[i - 1], seen[i]) << "iter " << iter;
		}
	}
}

TEST(GSPageBitmap, FromLooperMatchesReferenceRandomGeometry)
{
	std::mt19937_64 rng(0x67766f43);
	// The looper's page enumeration is format-shaped (page dimensions differ per PSM),
	// so sweep the format families the model will actually see.
	constexpr u32 kPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
	std::uniform_int_distribution<u32> psm_dist(0, std::size(kPsms) - 1);
	std::uniform_int_distribution<u32> bp_dist(0, GS_MAX_BLOCKS - 1);
	std::uniform_int_distribution<u32> bw_dist(1, 32);
	std::uniform_int_distribution<int> pos_dist(0, 2047);
	std::uniform_int_distribution<int> size_dist(1, 1024);

	for (int iter = 0; iter < 2000; iter++)
	{
		const u32 psm = kPsms[psm_dist(rng)];
		const u32 bp = bp_dist(rng);
		const u32 bw = bw_dist(rng);
		const int left = pos_dist(rng);
		const int top = pos_dist(rng);
		const GSVector4i rect(left, top, left + size_dist(rng), top + size_dist(rng));

		const GSOffset off = GSOffset::fromKnownPSM(bp, bw, static_cast<GS_PSM>(psm));

		GSPageBitmap bm;
		bm.addPages(off.pageLooperForRect(rect));

		ASSERT_TRUE(Matches(bm, RefFromLooper(off, rect)))
			<< "iter " << iter << " psm " << psm << " bp " << bp << " bw " << bw;
	}
}

TEST(GSPageBitmap, FromLooperWrapGeometry)
{
	// A footprint starting near the top of GS memory that runs past page 511 and wraps:
	// the resulting set has pages at both ends of the space and must still match the
	// looper's own enumeration bit-for-bit.
	const GSOffset off = GSOffset::fromKnownPSM((GS_MAX_PAGES - 2) * 32, 16, PSMCT32);
	const GSVector4i rect(0, 0, 1024, 512); // 16 pages wide, 16 tall = 256 pages from page 510

	GSPageBitmap bm;
	bm.addPages(off.pageLooperForRect(rect));

	ASSERT_TRUE(Matches(bm, RefFromLooper(off, rect)));
	EXPECT_TRUE(bm.test(GS_MAX_PAGES - 1));
	EXPECT_TRUE(bm.test(0));
}
