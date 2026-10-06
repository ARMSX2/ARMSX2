// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Checks the write stamps of a GSLocalMemory against its bytes: every page whose bytes changed since
// the watch began must have been marked since. This is the question the texture hash memo relies on,
// asked of one write at a time, so a writer that stores into local memory without marking the pages it
// stored to fails here and not only as a stale texture in a game.

#pragma once

#include "GS/GSLocalMemory.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace GSWriteStamps
{
	class Watch
	{
	public:
		explicit Watch(const GSLocalMemory& mem)
			: m_mem(mem)
			, m_seq(mem.WriteSeq())
			, m_before(mem.m_vm8, mem.m_vm8 + GSLocalMemory::m_vmsize)
		{
		}

		/// The pages whose bytes are not what they were when the watch began.
		std::vector<u32> ChangedPages() const
		{
			std::vector<u32> pages;
			for (u32 page = 0; page < GS_MAX_PAGES; page++)
			{
				if (std::memcmp(m_mem.m_vm8 + page * GS_PAGE_SIZE, m_before.data() + page * GS_PAGE_SIZE, GS_PAGE_SIZE) != 0)
					pages.push_back(page);
			}
			return pages;
		}

		/// The pages marked since the watch began, changed or not.
		std::vector<u32> MarkedPages() const
		{
			std::vector<u32> pages;
			for (u32 page = 0; page < GS_MAX_PAGES; page++)
			{
				if (m_mem.PageStamp(page) > m_seq)
					pages.push_back(page);
			}
			return pages;
		}

		/// The pages whose bytes changed and that nothing marked: writes the memo would not see.
		std::vector<u32> UnmarkedChangedPages() const
		{
			std::vector<u32> pages;
			for (const u32 page : ChangedPages())
			{
				if (m_mem.PageStamp(page) <= m_seq)
					pages.push_back(page);
			}
			return pages;
		}

		u64 StartSeq() const { return m_seq; }

		static std::string Describe(const std::vector<u32>& pages)
		{
			std::string s;
			for (const u32 page : pages)
			{
				char buf[16];
				std::snprintf(buf, sizeof(buf), "%s%u", s.empty() ? "" : ",", page);
				s += buf;
			}
			return s.empty() ? "none" : s;
		}

	private:
		const GSLocalMemory& m_mem;
		u64 m_seq;
		std::vector<u8> m_before;
	};
} // namespace GSWriteStamps
