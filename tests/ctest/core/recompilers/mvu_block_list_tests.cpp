// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// microBlockManager's full-compare list must stay a single chain from head
// to tail across lookups.
//
// A block whose entry state needs an exact match lives on a singly linked
// list with a head (fBlockList) and a tail (fBlockEnd). search() moves the
// block it finds to the front, and add() appends new blocks at the tail. A
// hit on the LAST link moves the tail to the front, so the tail pointer has
// to move to its predecessor; if it keeps pointing at the old tail, which is
// now the head, the next add() writes its new block after the head and cuts
// off every link that followed the head.
//
// The cut-off blocks are unreachable from the list: search() can't find them,
// so the next compile of the same entry state makes a duplicate; reset() and
// the destructor walk the list from the head, so they never free them.
//
// The manager is driven through test hooks in pcsx2/arm64/microVU-arm64.cpp
// (microVU-arm64.h's __fi definitions can't safely cross TUs; same pattern as
// mvu_clear_occupied_tests). A block is identified by a key, and its address
// is the block's identity: a lookup that returns a different address than the
// block's first add() is a duplicate.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <random>
#include <vector>

using u32 = std::uint32_t;

namespace mvu_test_hooks
{
	void* BlockListCreate();
	void BlockListDestroy(void* bm);
	void BlockListReset(void* bm);
	const void* BlockListAdd(void* bm, u32 key);
	const void* BlockListSearch(void* bm, u32 key);
	u32 BlockListAddedCount(void* bm);
	u32 BlockListWalkLength(void* bm, bool* end_is_last);
} // namespace mvu_test_hooks

namespace recompiler_tests {

namespace {

namespace hook = mvu_test_hooks;

struct ManagerUnderTest
{
	void* bm = hook::BlockListCreate();
	// key -> the address add() first returned for it.
	std::map<u32, const void*> blocks;

	~ManagerUnderTest() { hook::BlockListDestroy(bm); }

	const void* Add(u32 key)
	{
		const void* p = hook::BlockListAdd(bm, key);
		auto it = blocks.find(key);
		if (it == blocks.end())
			blocks.emplace(key, p);
		return p;
	}

	// Every block added so far is on the list, the tail is the list's last
	// link, and a lookup of each key returns the block the key first got. The
	// lookups reorder the list, so they run last.
	void CheckIntact(const char* what)
	{
		bool endIsLast = false;
		const u32 walked = hook::BlockListWalkLength(bm, &endIsLast);
		ASSERT_EQ(walked, blocks.size()) << what << ": a walk from the head must reach every block";
		ASSERT_EQ(hook::BlockListAddedCount(bm), blocks.size()) << what << ": duplicate blocks were added";
		ASSERT_TRUE(endIsLast || walked == 0) << what << ": the tail pointer is not the last link";
		for (const auto& [key, ptr] : blocks)
		{
			ASSERT_EQ(hook::BlockListSearch(bm, key), ptr) << what << ": block " << key << " was lost or duplicated";
		}
	}
};

} // namespace

// The reported shape: promote the tail, then append.
TEST(MvuBlockList, AppendAfterPromotingTheLastBlockKeepsEveryBlock)
{
	ManagerUnderTest m;
	const void* a = m.Add(1);
	const void* b = m.Add(2);
	const void* c = m.Add(3);
	ASSERT_NE(a, b);
	ASSERT_NE(b, c);

	// C is the tail; the hit moves it to the front.
	ASSERT_EQ(hook::BlockListSearch(m.bm, 3), c);

	bool endIsLast = false;
	ASSERT_EQ(hook::BlockListWalkLength(m.bm, &endIsLast), 3u);
	EXPECT_TRUE(endIsLast) << "the tail pointer must follow the tail to its new position";

	const void* d = m.Add(4);
	ASSERT_NE(d, nullptr);

	EXPECT_EQ(hook::BlockListWalkLength(m.bm, &endIsLast), 4u) << "the append cut off the links behind the old head";
	EXPECT_TRUE(endIsLast);
	EXPECT_EQ(hook::BlockListAddedCount(m.bm), 4u);

	EXPECT_EQ(hook::BlockListSearch(m.bm, 1), a);
	EXPECT_EQ(hook::BlockListSearch(m.bm, 2), b);
	EXPECT_EQ(hook::BlockListSearch(m.bm, 3), c);
	EXPECT_EQ(hook::BlockListSearch(m.bm, 4), d);

	// Compiling the same entry states again finds the blocks; it adds none.
	EXPECT_EQ(m.Add(1), a);
	EXPECT_EQ(m.Add(2), b);
	EXPECT_EQ(hook::BlockListAddedCount(m.bm), 4u);
	m.CheckIntact("after re-adding");
}

// Every position a lookup can promote from, in every list length from one to
// six, followed by an append and a second round of lookups.
TEST(MvuBlockList, PromotingAnyPositionThenAppendingKeepsEveryBlock)
{
	for (u32 n = 1; n <= 6; n++)
	{
		for (u32 hit = 1; hit <= n; hit++)
		{
			ManagerUnderTest m;
			for (u32 key = 1; key <= n; key++)
				m.Add(key);

			ASSERT_EQ(hook::BlockListSearch(m.bm, hit), m.blocks[hit]);
			m.CheckIntact("after the promotion");
			if (::testing::Test::HasFatalFailure())
				return;

			// A second promotion straight after: the list is in a new order.
			ASSERT_EQ(hook::BlockListSearch(m.bm, n - hit + 1), m.blocks[n - hit + 1]);
			m.Add(n + 1);
			m.CheckIntact("after the append");
			if (::testing::Test::HasFatalFailure())
				return;

			m.Add(n + 2);
			m.CheckIntact("after a second append");
			if (::testing::Test::HasFatalFailure())
				return;
		}
	}
}

// reset() walks the list from the head. After a promote-then-append sequence
// it must reach every block, and the manager must work again afterwards.
TEST(MvuBlockList, ResetReachesEveryBlockAfterPromotions)
{
	ManagerUnderTest m;
	for (u32 key = 1; key <= 5; key++)
		m.Add(key);
	ASSERT_NE(hook::BlockListSearch(m.bm, 5), nullptr);
	m.Add(6);

	bool endIsLast = false;
	// What reset() can free is exactly what a walk from the head reaches.
	EXPECT_EQ(hook::BlockListWalkLength(m.bm, &endIsLast), 6u) << "blocks no walk reaches are never freed by reset()";

	hook::BlockListReset(m.bm);
	EXPECT_EQ(hook::BlockListWalkLength(m.bm, &endIsLast), 0u);
	EXPECT_EQ(hook::BlockListAddedCount(m.bm), 0u);
	EXPECT_EQ(hook::BlockListSearch(m.bm, 5), nullptr);

	m.blocks.clear();
	m.Add(7);
	m.Add(8);
	ASSERT_NE(hook::BlockListSearch(m.bm, 8), nullptr);
	m.Add(9);
	m.CheckIntact("after reuse");
}

// A long random mix of lookups and adds over a small set of keys, so every
// promotion position and every promote-then-append order comes up. After each
// step the list must hold exactly the blocks added so far.
TEST(MvuBlockList, RandomLookupsAndAddsNeverLoseABlock)
{
	constexpr u32 kKeys = 12;
	for (u32 seed = 0; seed < 8; seed++)
	{
		std::mt19937 rng(0xB10C1157u + seed);
		ManagerUnderTest m;
		std::vector<bool> known(kKeys + 1, false);

		for (u32 step = 0; step < 600; step++)
		{
			const u32 key = 1 + rng() % kKeys;
			if (rng() % 3 == 0)
			{
				const void* got = hook::BlockListSearch(m.bm, key);
				if (known[key])
					ASSERT_EQ(got, m.blocks[key]) << "seed " << seed << " step " << step << ": block " << key << " lost";
				else
					ASSERT_EQ(got, nullptr) << "seed " << seed << " step " << step;
			}
			else
			{
				const void* got = m.Add(key);
				ASSERT_EQ(got, m.blocks[key]) << "seed " << seed << " step " << step << ": block " << key << " duplicated";
				known[key] = true;
			}

			// The walk and tail checks leave the list as they found it only
			// when they don't search, so look at the structure directly here
			// and run the full lookup sweep occasionally.
			bool endIsLast = false;
			const u32 walked = hook::BlockListWalkLength(m.bm, &endIsLast);
			ASSERT_EQ(walked, m.blocks.size()) << "seed " << seed << " step " << step << ": links were cut off";
			ASSERT_TRUE(endIsLast || walked == 0) << "seed " << seed << " step " << step << ": the tail pointer is stale";
			ASSERT_EQ(hook::BlockListAddedCount(m.bm), m.blocks.size()) << "seed " << seed << " step " << step;
			if (step % 50 == 49)
			{
				m.CheckIntact("sweep");
				if (::testing::Test::HasFatalFailure())
					return;
			}
		}
	}
}

} // namespace recompiler_tests
