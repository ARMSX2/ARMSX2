// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// BlockLut: entries read back the last Reset() value, and on Linux pages only cost memory once written.

#include "arm64/BlockLut-arm64.h"

#include <gtest/gtest.h>

#if defined(__linux__)
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#endif

namespace
{
	// The EE table: one entry per 4 bytes of RAM (32 MB) and the three ROMs (4 MB each).
	constexpr size_t EE_ENTRIES = (44u << 20) / 4;
	constexpr uptr STUB_A = 0x0000007f12345000ull;
	constexpr uptr STUB_B = 0x0000007f6789a000ull;

	bool EveryEntryIs(const BlockLut& lut, uptr fnptr)
	{
		for (size_t i = 0; i < lut.size(); i++)
		{
			if (lut.data()[i].GetFnptr() != fnptr)
				return false;
		}
		return true;
	}

	// A spread of entries: both ends and one in every sixteenth of the table, each in its own page.
	template <typename F>
	void ForSomeEntries(size_t count, F&& f)
	{
		f(0);
		for (size_t i = count / 16; i < count; i += count / 16)
			f(i);
		f(count - 1);
	}

#if defined(__linux__)
	// The private dirty bytes of the mappings that make up [p, p + size), from /proc/self/smaps.
	size_t PrivateDirtyBytes(const void* p, size_t size)
	{
		const unsigned long long lo = reinterpret_cast<uptr>(p);
		const unsigned long long hi = lo + size;
		std::ifstream smaps("/proc/self/smaps");
		std::string line;
		bool inside = false;
		size_t kb = 0;
		while (std::getline(smaps, line))
		{
			unsigned long long start, end;
			if (std::sscanf(line.c_str(), "%llx-%llx ", &start, &end) == 2)
				inside = (start < hi && end > lo);
			else if (inside && line.rfind("Private_Dirty:", 0) == 0)
				kb += std::stoul(line.substr(14));
		}
		return kb * 1024;
	}
#endif
} // namespace

TEST(BlockLut, ResetSetsEveryEntry)
{
	BlockLut lut;
	lut.Resize(EE_ENTRIES);
	ASSERT_NE(lut.data(), nullptr);
	EXPECT_EQ(lut.size(), EE_ENTRIES);
	lut.Reset(STUB_A);
	EXPECT_TRUE(EveryEntryIs(lut, STUB_A));
}

TEST(BlockLut, ResetPutsWrittenEntriesBack)
{
	BlockLut lut;
	lut.Resize(EE_ENTRIES);
	lut.Reset(STUB_A);
	ForSomeEntries(EE_ENTRIES, [&](size_t i) { lut.data()[i].SetFnptr(STUB_B + i); });
	ForSomeEntries(EE_ENTRIES, [&](size_t i) { EXPECT_EQ(lut.data()[i].GetFnptr(), STUB_B + i); });

	lut.Reset(STUB_A);
	EXPECT_TRUE(EveryEntryIs(lut, STUB_A));
}

TEST(BlockLut, ResetToAnotherValueReachesEveryEntry)
{
	BlockLut lut;
	lut.Resize(EE_ENTRIES);
	lut.Reset(STUB_A);
	ForSomeEntries(EE_ENTRIES, [&](size_t i) { lut.data()[i].SetFnptr(STUB_A + i); });

	lut.Reset(STUB_B);
	EXPECT_TRUE(EveryEntryIs(lut, STUB_B)) << "the written entries and the untouched ones";
	lut.Reset(STUB_A);
	EXPECT_TRUE(EveryEntryIs(lut, STUB_A));
}

TEST(BlockLut, ResizingKeepsResetWorking)
{
	BlockLut lut;
	lut.Resize(EE_ENTRIES);
	lut.Reset(STUB_A);
	const BASEBLOCK* const first = lut.data();
	lut.Resize(EE_ENTRIES);
	EXPECT_EQ(lut.data(), first) << "the same size keeps the table";

	// The IOP table's size, which is not a whole number of pattern blocks.
	const size_t iop_entries = (14u << 20) / 4 + 3;
	lut.Resize(iop_entries);
	EXPECT_EQ(lut.size(), iop_entries);
	lut.Reset(STUB_B);
	EXPECT_TRUE(EveryEntryIs(lut, STUB_B));
	lut.data()[iop_entries - 1].SetFnptr(STUB_A);
	lut.Reset(STUB_B);
	EXPECT_EQ(lut.data()[iop_entries - 1].GetFnptr(), STUB_B);
}

#if defined(__linux__)
TEST(BlockLut, OnlyWrittenPagesCostMemory)
{
	BlockLut lut;
	lut.Resize(EE_ENTRIES);
	lut.Reset(STUB_A);
	const size_t bytes = EE_ENTRIES * sizeof(BASEBLOCK);

	ASSERT_TRUE(EveryEntryIs(lut, STUB_A));
	EXPECT_LT(PrivateDirtyBytes(lut.data(), bytes), 1u << 20) << "reading all 88 MB copies none of it";

	size_t written = 0;
	ForSomeEntries(EE_ENTRIES, [&](size_t i) {
		lut.data()[i].SetFnptr(STUB_B);
		written++;
	});
	const size_t dirty = PrivateDirtyBytes(lut.data(), bytes);
	// A write dirties one page of the kernel's, which can be smaller than __pagesize (a 16K build on a 4K-page kernel).
	EXPECT_GE(dirty, written * static_cast<size_t>(sysconf(_SC_PAGESIZE)));
	EXPECT_LT(dirty, 1u << 20) << "a written entry costs its page, not the table";

	lut.Reset(STUB_A);
	EXPECT_LT(PrivateDirtyBytes(lut.data(), bytes), 2 * __pagesize) << "a reset gives the written pages back";
	EXPECT_TRUE(EveryEntryIs(lut, STUB_A));
}
#endif
