// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "x86/BaseblockEx.h"

class Arm64BlockPageIndex
{
public:
	struct Position
	{
		int page = -1;
		int slot = -1;

		explicit operator bool() const { return page >= 0; }
		bool operator==(const Position&) const = default;
	};

	static constexpr Position Invalid() { return {}; }

	BASEBLOCKEX* UpdateOrInsert(u32 pc, uptr fnptr)
	{
		const u32 page_number = pc >> PAGE_SHIFT;
		const int index = PagePosition(page_number);
		const bool new_page = index == PageCount() || m_pages[index].number != page_number;
		if (new_page)
		{
			m_page_cache.fill({});
			m_pages.insert(m_pages.begin() + index, Page{page_number, 0, {}});
		}

		Page& page = m_pages[index];
		const int slot = LowerBound(page, pc);
		if (slot < page.count && page.storage[slot].startpc == pc)
		{
			page.storage[slot].fnptr = fnptr;
			return &page.storage[slot];
		}

		if (static_cast<size_t>(page.count) == page.storage.size())
			page.storage.emplace_back();
		if (page.count == 0)
			AddLivePage(index, new_page);
		BASEBLOCKEX* const block = page.storage.data() + slot;
		if (slot < page.count)
			std::memmove(block + 1, block, (page.count - slot) * sizeof(*block));
		*block = {};
		block->startpc = pc;
		block->fnptr = fnptr;
		page.count++;
		m_size++;
		return block;
	}

	const BASEBLOCKEX* FindExact(u32 pc) const
	{
		const u32 page_number = pc >> PAGE_SHIFT;
		const int index = PagePosition(page_number);
		if (index == PageCount() || m_pages[index].number != page_number)
			return nullptr;

		const Page& page = m_pages[index];
		const int slot = LowerBound(page, pc);
		return slot < page.count && page.storage[slot].startpc == pc ? &page.storage[slot] : nullptr;
	}

	Position AtOrBefore(u32 pc) const
	{
		const u32 page_number = pc >> PAGE_SHIFT;
		const int index = PagePosition(page_number);
		if (index < PageCount() && m_pages[index].number == page_number)
		{
			const Page& page = m_pages[index];
			const auto first = page.storage.begin();
			const auto next = std::upper_bound(first, first + page.count, pc,
				[](u32 pc, const BASEBLOCKEX& block) { return pc < block.startpc; });
			if (next != first)
				return {index, static_cast<int>(next - first) - 1};
		}
		return LastInOrBeforePage(index - 1);
	}

	// An empty position denotes the start boundary when advancing.
	Position Next(Position position) const
	{
		if (position && position.slot + 1 < m_pages[position.page].count)
			return {position.page, position.slot + 1};

		const int page = position.page + 1;
		if (page < PageCount() && m_pages[page].count != 0)
			return {page, 0};
		const int next = LivePageLowerBound(page);
		return next < m_live_page_count ? Position{m_live_pages[next], 0} : Position{};
	}

	Position Previous(Position position) const
	{
		if (position.slot > 0)
			return {position.page, position.slot - 1};
		return LastInOrBeforePage(position.page - 1);
	}

	const BASEBLOCKEX* Get(Position position) const
	{
		return position ? &m_pages[position.page].storage[position.slot] : nullptr;
	}

	BASEBLOCKEX* Get(Position position)
	{
		return position ? &m_pages[position.page].storage[position.slot] : nullptr;
	}

	// Fastmem fault handling can erase a range. Keep directory entries and
	// allocations stable, and remove emptied pages from the live page list.
	void Erase(Position first, Position last)
	{
		if (first.page == last.page)
		{
			EraseEntries(m_pages[first.page], first.slot, last.slot + 1);
			if (m_pages[first.page].count != 0)
				return;
		}

		const int first_live = LivePageLowerBound(first.page);
		const int last_live = LivePageLowerBound(last.page);
		if (first.page != last.page)
		{
			for (int i = first_live; i <= last_live; i++)
			{
				Page& page = m_pages[m_live_pages[i]];
				EraseEntries(page, i == first_live ? first.slot : 0, i == last_live ? last.slot + 1 : page.count);
			}
		}

		// Only the boundary pages can retain blocks after a range erase.
		const int begin = first_live + (m_pages[first.page].count != 0);
		const int end = last_live + (m_pages[last.page].count == 0);
		if (begin < end)
		{
			int* const data = m_live_pages.data();
			std::memmove(data + begin, data + end, (m_live_page_count - end) * sizeof(*data));
			m_live_page_count -= end - begin;
		}
	}

	void Reset()
	{
		m_pages.clear();
		m_page_cache.fill({});
		m_live_page_count = 0;
		m_size = 0;
	}

	u32 size() const { return m_size; }

private:
	static constexpr unsigned PAGE_SHIFT = 12;

	struct Page
	{
		u32 number;
		int count;
		std::vector<BASEBLOCKEX> storage;
	};

	struct CachedPage
	{
		u32 number = ~0u;
		int position = 0;
	};

	static int LowerBound(const Page& page, u32 pc)
	{
		const auto first = page.storage.begin();
		return static_cast<int>(std::lower_bound(first, first + page.count, pc,
			[](const BASEBLOCKEX& block, u32 pc) { return block.startpc < pc; }) - first);
	}

	int PageCount() const { return static_cast<int>(m_pages.size()); }

	int PagePosition(u32 page) const
	{
		CachedPage& cached = m_page_cache[page % m_page_cache.size()];
		if (cached.number == page)
			return cached.position;

		const int position = static_cast<int>(std::lower_bound(m_pages.begin(), m_pages.end(), page,
			[](const Page& entry, u32 page) { return entry.number < page; }) - m_pages.begin());
		cached = {page, position};
		return position;
	}

	Position LastInOrBeforePage(int page) const
	{
		if (page >= 0 && m_pages[page].count != 0)
			return {page, m_pages[page].count - 1};
		const int position = LivePageLowerBound(page);
		if (position == 0)
			return {};
		const int previous = m_live_pages[position - 1];
		return {previous, m_pages[previous].count - 1};
	}

	int LivePageLowerBound(int page) const
	{
		const auto first = m_live_pages.begin();
		return static_cast<int>(std::lower_bound(first, first + m_live_page_count, page) - first);
	}

	void AddLivePage(int page, bool new_page)
	{
		const int position = LivePageLowerBound(page);
		if (static_cast<size_t>(m_live_page_count) == m_live_pages.size())
			m_live_pages.emplace_back();

		// Inserting a directory entry shifts every page index after it.
		for (int i = m_live_page_count; i > position; i--)
			m_live_pages[i] = m_live_pages[i - 1] + new_page;
		m_live_pages[position] = page;
		m_live_page_count++;
	}

	void EraseEntries(Page& page, int begin, int end)
	{
		if (end < page.count)
		{
			BASEBLOCKEX* const data = page.storage.data();
			std::memmove(data + begin, data + end, (page.count - end) * sizeof(*data));
		}
		page.count -= end - begin;
		m_size -= end - begin;
	}

	std::vector<Page> m_pages;
	// Both hits and insertion positions stay valid until a page enters the directory.
	mutable std::array<CachedPage, 64> m_page_cache{};
	// Keep allocated storage separate from the logical size for fastmem erases.
	std::vector<int> m_live_pages;
	int m_live_page_count = 0;
	u32 m_size = 0;
};
