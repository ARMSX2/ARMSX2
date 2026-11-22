// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// HostSys::DiscardMemory and HostSys::DiscardSharedMemory: the range reads as zeros afterwards, the bytes
// around it are untouched, and on Linux the whole pages in it go back to the host.

#include "common/HostSys.h"
#include "common/BitUtils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <csignal>
#include <memory>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

// Satisfy HostSys's reference to the emulator's fault handler, which lives in the core.
// The protection test faults in a child process without installing this handler.
PageFaultHandler::HandlerResult PageFaultHandler::HandlePageFault(void*, void*, bool)
{
	return HandlerResult::ExecuteNextHandler;
}

namespace
{
	constexpr size_t PAGES = 64;

	bool AllBytesAre(const u8* p, size_t size, u8 value)
	{
		for (size_t i = 0; i < size; i++)
		{
			if (p[i] != value)
				return false;
		}
		return true;
	}

#if defined(__linux__)
	// How many of the pages of the page-aligned [p, p + size) are resident.
	size_t ResidentPages(void* p, size_t size)
	{
		const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
		std::vector<unsigned char> resident((size + page - 1) / page);
		if (mincore(p, size, resident.data()) != 0)
			return static_cast<size_t>(-1);
		size_t count = 0;
		for (const unsigned char r : resident)
			count += r & 1;
		return count;
	}
#endif
} // namespace

#if defined(__linux__)
TEST(HostSysPageSize, MatchesKernelAndAlignsAcrossBoundaries)
{
	const unsigned int page = static_cast<unsigned int>(sysconf(_SC_PAGESIZE));
	EXPECT_EQ(__pagesize, page);
	EXPECT_EQ(__pagemask, page - 1);
	EXPECT_EQ(1u << __pageshift, page);
	EXPECT_EQ(Common::PageAlign(page - 1), page);
	EXPECT_EQ(Common::PageAlign(page), page);
	EXPECT_EQ(Common::PageAlign(page + 1), 2 * page);
}

TEST(HostSysPageSize, ProtectsOneKernelPageWithoutChangingItsNeighbours)
{
	const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
	u8* const p = static_cast<u8*>(mmap(nullptr, 3 * page, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	ASSERT_NE(p, MAP_FAILED);
	std::memset(p, 0x5A, 3 * page);
	HostSys::MemProtect(p + page, __pagesize, PageAccess_ReadOnly());
	EXPECT_EXIT({ *static_cast<volatile u8*>(p + page) = 0; _exit(0); }, ::testing::KilledBySignal(SIGSEGV), "");
	p[page - 1] = 0xA1;
	p[2 * page] = 0xA2;
	EXPECT_EQ(p[page], 0x5A);
	HostSys::MemProtect(p + page, __pagesize, PageAccess_ReadWrite());
	p[page] = 0xA3;
	EXPECT_EQ(p[page - 1], 0xA1);
	EXPECT_EQ(p[2 * page], 0xA2);
	EXPECT_EQ(munmap(p, 3 * page), 0);
}
#endif

TEST(HostSysDiscardMemory, ZeroesTheRangeAndNothingAroundIt)
{
	// Unaligned at both ends: the whole pages inside go back, the partial ones at the ends are zeroed.
	std::vector<u8> buffer(PAGES * __pagesize + 3000, 0xAB);
	const size_t begin = 1000;
	const size_t size = buffer.size() - 2500;
	HostSys::DiscardMemory(buffer.data() + begin, size);

	EXPECT_TRUE(AllBytesAre(buffer.data(), begin, 0xAB));
	EXPECT_TRUE(AllBytesAre(buffer.data() + begin, size, 0));
	EXPECT_TRUE(AllBytesAre(buffer.data() + begin + size, buffer.size() - begin - size, 0xAB));
}

TEST(HostSysDiscardMemory, ARangeInsideOnePageIsZeroedInPlace)
{
	std::vector<u8> buffer(__pagesize, 0xAB);
	HostSys::DiscardMemory(buffer.data() + 10, 100);

	EXPECT_TRUE(AllBytesAre(buffer.data(), 10, 0xAB));
	EXPECT_TRUE(AllBytesAre(buffer.data() + 10, 100, 0));
	EXPECT_TRUE(AllBytesAre(buffer.data() + 110, buffer.size() - 110, 0xAB));
}

#if defined(__linux__)
TEST(HostSysDiscardMemory, GivesTheWholePagesBack)
{
	if (static_cast<size_t>(sysconf(_SC_PAGESIZE)) != __pagesize)
		GTEST_SKIP() << "the kernel's pages are not __pagesize, so DiscardMemory falls back to memset";

	const size_t size = PAGES * __pagesize;
	void* const p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(p, MAP_FAILED);
	std::memset(p, 0x5A, size);
	ASSERT_EQ(ResidentPages(p, size), PAGES);

	HostSys::DiscardMemory(p, size);
	EXPECT_EQ(ResidentPages(p, size), 0u);
	EXPECT_TRUE(AllBytesAre(static_cast<const u8*>(p), size, 0));
	munmap(p, size);
}
#endif

TEST(HostSysDiscardSharedMemory, ZeroesTheRangeInEveryMapping)
{
	const size_t size = PAGES * __pagesize;
	void* const handle = HostSys::CreateSharedMemory(HostSys::GetFileMappingName("pcsx2_discard_test").c_str(), size);
	ASSERT_NE(handle, nullptr);
	std::unique_ptr<SharedMemoryMappingArea> area = SharedMemoryMappingArea::Create(2 * size);
	ASSERT_TRUE(area);
	u8* const a = area->Map(handle, 0, area->BasePointer(), size, PageAccess_ReadWrite());
	u8* const b = area->Map(handle, 0, area->OffsetPointer(size), size, PageAccess_ReadWrite());
	ASSERT_TRUE(a && b);
	std::memset(a, 0xC3, size);

	// Unaligned at both ends, as SysMemory::ZeroData's ranges can be.
	const size_t begin = __pagesize + 100;
	const size_t length = 10 * __pagesize;
	const bool discarded = HostSys::DiscardSharedMemory(handle, begin, length);
#if defined(__linux__)
	EXPECT_TRUE(discarded);
#endif
	if (discarded)
	{
		for (const u8* view : {a, b})
		{
			EXPECT_TRUE(AllBytesAre(view, begin, 0xC3));
			EXPECT_TRUE(AllBytesAre(view + begin, length, 0));
			EXPECT_TRUE(AllBytesAre(view + begin + length, size - begin - length, 0xC3));
		}
	}

	area->Unmap(b, size);
	area->Unmap(a, size);
	area.reset();
	HostSys::DestroySharedMemory(handle);
}
