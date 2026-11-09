// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+
//
// Storage for microVU's jump tables (arm64/microVU_JumpTable-arm64.h). A table reads as zeros until an entry is
// written, a table handed out again holds nothing of its last user's, and on Linux only the pages actually
// written are resident.

#include <gtest/gtest.h>

#include "common/Pcsx2Defs.h"

#if defined(_M_ARM64) || defined(__aarch64__)

#include "arm64/microVU_JumpTable-arm64.h"

#include <cstring>
#include <set>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{
	size_t ResidentPages(const void* table)
	{
#if defined(__linux__)
		// mincore() reports one byte per page of the kernel's, which can be smaller than __pagesize (a 16K build on a
		// 4K-page kernel).
		const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
		std::vector<unsigned char> pages((mVUJumpTable::kBytes + page - 1) / page);
		EXPECT_EQ(mincore(const_cast<void*>(table), mVUJumpTable::kBytes, pages.data()), 0);
		size_t resident = 0;
		for (unsigned char p : pages)
			resident += p & 1;
		return resident;
#else
		(void)table;
		return 0;
#endif
	}

	bool AllZero(const void* table)
	{
		const u8* const b = static_cast<const u8*>(table);
		for (size_t i = 0; i < mVUJumpTable::kBytes; i++)
			if (b[i])
				return false;
		return true;
	}

	void Mark(void* table, size_t offset) { static_cast<u8*>(table)[offset] = 0x5a; }
} // namespace

TEST(MvuJumpTable, ANewTableIsZerosAndNothingResident)
{
	void* const t = mVUJumpTable::Alloc();
#if defined(__linux__)
	EXPECT_EQ(ResidentPages(t), 0u) << "the table was written when it was allocated";
#endif
	EXPECT_TRUE(AllZero(t));
	mVUJumpTable::Free(t);
}

TEST(MvuJumpTable, OnlyTheWrittenPageBecomesResident)
{
	void* const t = mVUJumpTable::Alloc();
	Mark(t, __pagesize + 8);
#if defined(__linux__)
	EXPECT_EQ(ResidentPages(t), 1u);
#endif
	mVUJumpTable::Free(t);
}

// The reset that frees tables is followed by compiles that take them again: a table must not bring its old entries with it.
TEST(MvuJumpTable, AFreedTableComesBackEmptyAndUnresident)
{
	std::vector<void*> tables;
	for (int i = 0; i < 8; i++)
	{
		void* const t = mVUJumpTable::Alloc();
		std::memset(t, 0xff, mVUJumpTable::kBytes);
		tables.push_back(t);
	}
	for (void* t : tables)
		mVUJumpTable::Free(t);
	for (int i = 0; i < 8; i++)
	{
		void* const t = mVUJumpTable::Alloc();
#if defined(__linux__)
		EXPECT_EQ(ResidentPages(t), 0u) << "a freed table kept its pages";
#endif
		EXPECT_TRUE(AllZero(t)) << "a freed table kept its contents";
		mVUJumpTable::Free(t);
	}
}

TEST(MvuJumpTable, TablesAreDistinctAcrossSlabs)
{
	std::vector<void*> tables;
	std::set<void*> seen;
	for (int i = 0; i < 700; i++)
	{
		void* const t = mVUJumpTable::Alloc();
		EXPECT_TRUE(seen.insert(t).second) << "a table was handed out twice";
		EXPECT_EQ(reinterpret_cast<uptr>(t) & (__pagesize - 1), 0u) << "a table not on a page boundary";
		tables.push_back(t);
	}
	for (void* t : tables)
		mVUJumpTable::Free(t);
}

#endif // arm64
