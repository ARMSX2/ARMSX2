// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <algorithm>
#include <array>
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

constexpr uint8_t C32BlockOrder[4][8] = {
	{0, 1, 4, 5, 16, 17, 20, 21},
	{2, 3, 6, 7, 18, 19, 22, 23},
	{8, 9, 12, 13, 24, 25, 28, 29},
	{10, 11, 14, 15, 26, 27, 30, 31},
};

inline uint32_t C32BlockNumber(Layout layout, int x, int y)
{
	return layout.bp + BlocksPerPage * ((y / PageHeight) * layout.bw + x / PageWidth) +
		C32BlockOrder[(y / 8) & 3][(x / 8) & 7];
}

constexpr auto C32RowPrefixes = [] {
	std::array<std::array<uint32_t, 9>, 4> prefixes{};
	for (int row = 0; row < 4; row++)
		for (int x = 0; x < 8; x++)
			prefixes[row][x + 1] = prefixes[row][x] | (uint32_t(1) << C32BlockOrder[row][x]);
	return prefixes;
}();

inline Rect CoveringBlocks(Rect valid)
{
	return {valid.x & ~7, valid.y & ~7, (valid.z + 7) & ~7, (valid.w + 7) & ~7};
}

inline bool ValidWrite(Layout source, Rect write)
{
	return IsC32(source) && Bounded(write) && !((write.x | write.y | write.z | write.w) & 7) &&
		BlockCount(write) <= MaxBlocks;
}

struct WrittenPages
{
	Layout source{};
	Rect write{};
	bool supported = false;
	// The sentinel page makes extraction at the top of GS memory safe.
	std::array<uint32_t, MaxBlocks / BlocksPerPage + 1> masks{};
	uint32_t first_block = 0, last_block = 0;
};

inline uint32_t SourcePageMask(Rect write, int page_x, int page_y)
{
	const int left = std::max(0, write.x / 8 - page_x * 8);
	const int right = std::min(8, write.z / 8 - page_x * 8);
	const int top = std::max(0, write.y / 8 - page_y * 4);
	const int bottom = std::min(4, write.w / 8 - page_y * 4);
	uint32_t mask = 0;
	for (int row = top; row < bottom; row++)
		mask |= C32RowPrefixes[row][right] ^ C32RowPrefixes[row][left];
	return mask;
}

inline void StorePhysicalPage(WrittenPages& written, uint32_t first, uint32_t mask)
{
	const uint32_t page = first / BlocksPerPage, phase = first % BlocksPerPage;
	written.masks[page] |= mask << phase;
	if (phase != 0)
		written.masks[page + 1] |= mask >> (BlocksPerPage - phase);
}

inline WrittenPages BuildWrittenC32Blocks(Layout source, Rect write)
{
	WrittenPages written;
	written.source = source;
	written.write = write;
	if (!ValidWrite(source, write))
		return written;
	written.first_block = C32BlockNumber(source, write.x, write.y);
	written.last_block = C32BlockNumber(source, write.z - 8, write.w - 8);
	// C32 block order increases in both coordinates, including x beyond BW.
	if (written.last_block >= MaxBlocks)
		return written;
	for (int y = write.y / PageHeight; y <= (write.w - 1) / PageHeight; y++)
		for (int x = write.x / PageWidth; x <= (write.z - 1) / PageWidth; x++)
			StorePhysicalPage(written, source.bp + BlocksPerPage * (y * source.bw + x), SourcePageMask(write, x, y));
	written.supported = true;
	return written;
}

inline uint32_t ExtractReceiverPage(const WrittenPages& written, Layout receiver, int page)
{
	const uint32_t first = receiver.bp + page * BlocksPerPage;
	const uint32_t k = first / BlocksPerPage, r = first % BlocksPerPage;
	return r == 0 ? written.masks[k] :
		(written.masks[k] >> r) | (written.masks[k + 1] << (BlocksPerPage - r));
}

struct RunMerger
{
	std::vector<Rect>& rects;
	std::vector<size_t> previous, current;
	size_t cursor = 0;

	void Append(Rect run)
	{
		while (cursor < previous.size() && rects[previous[cursor]].x < run.x)
			cursor++;
		if (cursor < previous.size())
		{
			const size_t index = previous[cursor];
			Rect& rect = rects[index];
			if (rect.x == run.x && rect.z == run.z && rect.w == run.y)
			{
				rect.w = run.w;
				current.push_back(index);
				cursor++;
				return;
			}
		}
		rects.push_back(run);
		current.push_back(rects.size() - 1);
	}

	void FinishRow()
	{
		previous.swap(current);
		current.clear();
		cursor = 0;
	}
};

struct RowRuns
{
	RunMerger& merger;
	int y, start = -1;

	void Hit(bool hit, int x)
	{
		if (hit && start < 0)
			start = x;
		if (!hit && start >= 0)
		{
			merger.Append({start, y, x, y + 8});
			start = -1;
		}
	}
};

inline void MapPageRow(RowRuns& runs, uint32_t mask, int left, int right)
{
	if (mask == 0 || mask == UINT32_MAX)
	{
		runs.Hit(mask != 0, left);
		return;
	}
	for (int x = left; x < right; x += 8)
		runs.Hit(mask & (uint32_t(1) << C32BlockOrder[(runs.y / 8) & 3][(x / 8) & 7]), x);
}

inline void MapBlockRow(Layout receiver, Rect blocks, const WrittenPages& written,
	int begin_x, int end_x, int y, RunMerger& merger)
{
	RowRuns runs{merger, y};
	for (int x = begin_x; x <= end_x; x++)
	{
		const uint32_t mask = ExtractReceiverPage(written, receiver, (y / PageHeight) * receiver.bw + x);
		MapPageRow(runs, mask, std::max(blocks.x, x * PageWidth), std::min(blocks.z, (x + 1) * PageWidth));
	}
	runs.Hit(false, std::min(blocks.z, (end_x + 1) * PageWidth));
	merger.FinishRow();
}

inline void MapReceiverPages(Layout receiver, Rect blocks, const WrittenPages& written, std::vector<Rect>& rects)
{
	if (written.last_block < receiver.bp)
		return;
	const int first = int(std::max(written.first_block, receiver.bp) - receiver.bp) / BlocksPerPage;
	const int last = int(written.last_block - receiver.bp) / BlocksPerPage;
	const int first_x = blocks.x / PageWidth, last_x = (blocks.z - 1) / PageWidth;
	if (last < first_x)
		return;
	const int bw = static_cast<int>(receiver.bw);
	const int first_y = std::max(blocks.y / PageHeight, first > last_x ? (first - last_x + bw - 1) / bw : 0);
	const int last_y = std::min((blocks.w - 1) / PageHeight, (last - first_x) / bw);
	RunMerger merger{rects, {}, {}};
	for (int page_y = first_y; page_y <= last_y; page_y++)
	{
		const int begin_x = std::max(first_x, first - page_y * bw);
		const int end_x = std::min(last_x, last - page_y * bw);
		for (int y = std::max(blocks.y, page_y * PageHeight); y < std::min(blocks.w, (page_y + 1) * PageHeight); y += 8)
			MapBlockRow(receiver, blocks, written, begin_x, end_x, y, merger);
	}
}

inline Plan MapNonWrappingWritten(const WrittenPages& written, Layout receiver, Rect valid, Rect blocks)
{
	if (!written.supported)
		return {};
	Plan plan{true, {}};
	MapReceiverPages(receiver, blocks, written, plan.rects);
	for (Rect& rect : plan.rects)
		rect = Intersect(rect, valid);
	return plan;
}

inline Plan MapValidatedC32Blocks(Layout source, Rect write, Layout receiver, Rect valid,
	Rect blocks, const WrittenPages* prepared)
{
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

	if (C32BlockNumber(receiver, blocks.z - 8, blocks.w - 8) >= MaxBlocks)
		return {};
	if (prepared)
		return MapNonWrappingWritten(*prepared, receiver, valid, blocks);
	return MapNonWrappingWritten(BuildWrittenC32Blocks(source, write), receiver, valid, blocks);
}

inline Plan MapWrittenC32Blocks(const WrittenPages& written, Layout receiver, Rect valid)
{
	if (!written.supported || !IsC32(receiver) || !Bounded(valid))
		return {};
	const Rect blocks = CoveringBlocks(valid);
	if (BlockCount(blocks) > MaxBlocks)
		return {};
	return MapValidatedC32Blocks(written.source, written.write, receiver, valid, blocks, &written);
}

inline Plan MapC32Blocks(Layout source, Rect write, Layout receiver, Rect valid)
{
	if (!ValidWrite(source, write) || !IsC32(receiver) || !Bounded(valid))
		return {};
	const Rect blocks = CoveringBlocks(valid);
	if (BlockCount(blocks) > MaxBlocks)
		return {};
	return MapValidatedC32Blocks(source, write, receiver, valid, blocks, nullptr);
}
} // namespace GSExactDirtyMapping
