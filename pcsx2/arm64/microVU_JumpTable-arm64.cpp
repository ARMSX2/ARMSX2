// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "microVU_JumpTable-arm64.h"

#include "common/Assertions.h"
#include "common/HostSys.h"

#include <mutex>
#include <vector>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <sys/mman.h>
#endif

namespace mVUJumpTable
{
	namespace
	{
		static_assert((kBytes & (__pagesize - 1)) == 0, "a table covers whole pages");

		// Virtual address space only until a table is written.
		constexpr size_t kTablesPerSlab = 256;
		constexpr size_t kSlabBytes = kBytes * kTablesPerSlab;

		std::mutex s_mutex;
		std::vector<void*> s_free;
		u8* s_slab = nullptr;
		size_t s_slabUsed = kTablesPerSlab;

		u8* MapSlab()
		{
#ifdef _WIN32
			void* const slab = VirtualAlloc(nullptr, kSlabBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
			void* const slab = mmap(nullptr, kSlabBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#if defined(__linux__) && defined(MADV_NOHUGEPAGE)
			// (a huge page would make one written entry cost 2 MiB)
			if (slab != MAP_FAILED)
				madvise(slab, kSlabBytes, MADV_NOHUGEPAGE);
#endif
			pxAssertRel(slab != MAP_FAILED, "Out of address space for microVU's jump tables.");
#endif
			pxAssertRel(slab, "Out of address space for microVU's jump tables.");
			return static_cast<u8*>(slab);
		}
	} // namespace

	void* Alloc()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (!s_free.empty())
		{
			void* const table = s_free.back();
			s_free.pop_back();
			return table;
		}
		if (s_slabUsed == kTablesPerSlab)
		{
			s_slab = MapSlab();
			s_slabUsed = 0;
		}
		return s_slab + kBytes * s_slabUsed++;
	}

	void Free(void* table)
	{
		if (!table)
			return;
		HostSys::DiscardMemory(table, kBytes);
		std::lock_guard<std::mutex> lock(s_mutex);
		s_free.push_back(table);
	}
} // namespace mVUJumpTable
