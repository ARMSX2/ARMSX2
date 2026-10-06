// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace GSExactDirtyMapping
{
constexpr uint32_t C32 = 0;
constexpr uint32_t BlocksPerPage = 32;
constexpr int PageWidth = 64;
constexpr int PageHeight = 32;
constexpr uint32_t MaxBlocks = 16384; // 4 MiB / 256-byte GS block.

struct Rect
{
	int x, y, z, w;
};

struct Layout
{
	uint32_t bp, bw, psm;
};

struct Plan
{
	bool supported = false;
	std::vector<Rect> rects;
};

inline bool Bounded(Rect rect)
{
	return rect.x >= 0 && rect.y >= 0 && rect.z <= 2048 && rect.w <= 4096 &&
		rect.x < rect.z && rect.y < rect.w;
}

inline bool IsC32(Layout layout)
{
	return layout.psm == C32 && layout.bw > 0 && layout.bw < 64 && layout.bp < MaxBlocks;
}

inline uint64_t BlockCount(Rect rect)
{
	return uint64_t((rect.z - rect.x) / 8) * ((rect.w - rect.y) / 8);
}

inline Rect Intersect(Rect rect, Rect valid)
{
	return {std::max(rect.x, valid.x), std::max(rect.y, valid.y),
		std::min(rect.z, valid.z), std::min(rect.w, valid.w)};
}

inline bool HasUniqueNonWrappingPages(Layout layout, Rect rect)
{
	if (rect.z > static_cast<int>(layout.bw * PageWidth))
		return false;
	const uint64_t last_page = uint64_t((rect.w - 1) / PageHeight) * layout.bw + (rect.z - 1) / PageWidth;
	return layout.bp + (last_page + 1) * BlocksPerPage <= MaxBlocks;
}

inline size_t AppendOrExtendRun(std::vector<Rect>& rects, const std::vector<size_t>& previous, Rect run)
{
	for (const size_t index : previous)
	{
		Rect& rect = rects[index];
		if (rect.x == run.x && rect.z == run.z && rect.w == run.y)
		{
			rect.w = run.w;
			return index;
		}
	}
	rects.push_back(run);
	return rects.size() - 1;
}

template <typename Address>
bool CollectWrittenBlocks(Layout layout, Rect write, Address address, std::bitset<MaxBlocks>& written)
{
	for (int y = write.y; y < write.w; y += 8)
		for (int x = write.x; x < write.z; x += 8)
		{
			const uint32_t block = address(layout, x, y);
			if (block >= MaxBlocks)
				return false;
			written.set(block);
		}
	return true;
}

template <typename Address>
bool MapReceiverBlocks(Layout receiver, Rect blocks, Address address,
	const std::bitset<MaxBlocks>& written, std::vector<Rect>& rects)
{
	std::vector<size_t> previous, current;
	for (int y = blocks.y; y < blocks.w; y += 8)
	{
		current.clear();
		int run = -1;
		for (int x = blocks.x; x <= blocks.z; x += 8)
		{
			bool hit = false;
			if (x < blocks.z)
			{
				const uint32_t block = address(receiver, x, y);
				if (block >= MaxBlocks)
					return false;
				hit = written.test(block);
			}
			if (hit && run < 0)
				run = x;
			if (!hit && run >= 0)
			{
				current.push_back(AppendOrExtendRun(rects, previous, {run, y, x, y + 8}));
				run = -1;
			}
		}
		previous.swap(current);
	}
	return true;
}

// C32 has identical pixel-column ordering inside every 8x8 physical block.
// Only whole source blocks establish ownership of every emitted pixel. Address
// must use GSOffset::bnNoWrap; a page approximation cannot prove block overlap.
template <typename Address>
Plan MapC32Blocks(Layout source, Rect write, Layout receiver, Rect valid, Address address)
{
	if (!IsC32(source) || !IsC32(receiver) || !Bounded(write) || !Bounded(valid) ||
		((write.x | write.y | write.z | write.w) & 7))
		return {};
	const Rect blocks{valid.x & ~7, valid.y & ~7, (valid.z + 7) & ~7, (valid.w + 7) & ~7};
	if (BlockCount(write) > MaxBlocks || BlockCount(blocks) > MaxBlocks)
		return {};

	// Within BW, rows cannot alias other pages. Identical non-wrapping layouts
	// therefore preserve coordinates, so avoid scanning a whole ordinary target.
	if (source.bp == receiver.bp && source.bw == receiver.bw &&
		HasUniqueNonWrappingPages(source, write) && HasUniqueNonWrappingPages(receiver, blocks))
	{
		Plan plan{true, {}};
		const Rect overlap = Intersect(write, valid);
		if (overlap.x < overlap.z && overlap.y < overlap.w)
			plan.rects.push_back(overlap);
		return plan;
	}

	std::bitset<MaxBlocks> written;
	Plan plan;
	if (!CollectWrittenBlocks(source, write, address, written) ||
		!MapReceiverBlocks(receiver, blocks, address, written, plan.rects))
		return {}; // Reject wrapping atomically; the caller has enqueued nothing yet.

	for (Rect& rect : plan.rects)
		rect = Intersect(rect, valid);
	plan.supported = true; // Empty is an exact no-overlap result, not fallback.
	return plan;
}
} // namespace GSExactDirtyMapping
