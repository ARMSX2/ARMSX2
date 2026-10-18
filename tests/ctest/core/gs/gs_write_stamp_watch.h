// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Checks the write stamps of a GSLocalMemory against its bytes: every page whose bytes changed since
// the watch began must have been marked since. This is the question the texture hash memo relies on,
// asked of one write at a time, so a writer that stores into local memory without marking the pages it
// stored to fails here and not only as a stale texture in a game.
//
// The same question is asked of the 256-byte blocks and the dirty bitmap (GSLocalMemory::HashDirty), which
// the per-block hash cache relies on in the same way. The watch clears the bitmap when it begins, so a bit
// that is set afterwards was set by a mark.

#pragma once

#include "GS/GSLocalMemory.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace GSWriteStamps
{
	/// Bytes of padding for the source buffer of a host-to-local image write. The writers can read past the len
	/// they are handed: an odd-width 4-bit row takes a whole byte for its last pixel, and the odd-width PSMT4
	/// path reads from the start of a slice as if the slice held the whole image. How far depends on the
	/// transfer, so a test pads its buffer instead of relying on what the heap has after it.
	constexpr size_t kSourceSlack = 256;

	class Watch
	{
	public:
		/// Clears the dirty bitmap of `mem`: a fresh memory has it all set.
		explicit Watch(GSLocalMemory& mem)
			: m_mem(mem)
			, m_seq(mem.WriteSeq())
			, m_before(mem.m_vm8, mem.m_vm8 + GSLocalMemory::m_vmsize)
		{
			for (u32 block = 0; block < GS_MAX_BLOCKS; block++)
				mem.TestAndClearHashDirty(block);
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

		/// The blocks whose bytes are not what they were when the watch began.
		std::vector<u32> ChangedBlocks() const
		{
			std::vector<u32> blocks;
			for (u32 block = 0; block < GS_MAX_BLOCKS; block++)
			{
				if (std::memcmp(m_mem.m_vm8 + block * GS_BLOCK_SIZE, m_before.data() + block * GS_BLOCK_SIZE, GS_BLOCK_SIZE) != 0)
					blocks.push_back(block);
			}
			return blocks;
		}

		/// The blocks marked since the watch began, changed or not.
		std::vector<u32> MarkedBlocks() const
		{
			std::vector<u32> blocks;
			for (u32 block = 0; block < GS_MAX_BLOCKS; block++)
			{
				if (m_mem.HashDirty(block))
					blocks.push_back(block);
			}
			return blocks;
		}

		/// The blocks whose bytes changed and that are not marked: writes the block hash cache would not see.
		std::vector<u32> UnmarkedChangedBlocks() const
		{
			std::vector<u32> blocks;
			for (const u32 block : ChangedBlocks())
			{
				if (!m_mem.HashDirty(block))
					blocks.push_back(block);
			}
			return blocks;
		}

		u64 StartSeq() const { return m_seq; }

		/// The first 24 numbers, then a count: a failing block list can be thousands long.
		static std::string Describe(const std::vector<u32>& pages)
		{
			std::string s;
			size_t n = 0;
			for (const u32 page : pages)
			{
				if (n++ == 24)
				{
					char buf[32];
					std::snprintf(buf, sizeof(buf), ",... (%zu in all)", pages.size());
					s += buf;
					break;
				}
				char buf[16];
				std::snprintf(buf, sizeof(buf), "%s%u", s.empty() ? "" : ",", page);
				s += buf;
			}
			return s.empty() ? "none" : s;
		}

	private:
		GSLocalMemory& m_mem;
		u64 m_seq;
		std::vector<u8> m_before;
	};
} // namespace GSWriteStamps
