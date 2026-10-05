// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

/// Pieces of the texture upscaler that need no GS, GPU or settings state, so they can be unit
/// tested on their own: the bounded newest-first job queue and the CPU mip chain builder.
namespace GSTextureUpscaleSupport
{
	/// A newest-first queue with a cap on the number of items and on their total cost. It is not
	/// synchronized; the caller holds its own lock around every call.
	///
	/// Push adds the newest item. If that leaves the queue over either cap, the oldest items are
	/// removed until it is back under, and handed to the caller so it can undo whatever it recorded
	/// for them. The item just pushed is never removed, so a single item that is over the cost cap
	/// is still accepted (alone).
	template <typename T>
	class BoundedLifoQueue
	{
	public:
		BoundedLifoQueue(size_t max_items, size_t max_cost)
			: m_max_items(std::max<size_t>(max_items, 1))
			, m_max_cost(max_cost)
		{
		}

		void Push(T item, size_t cost, std::vector<T>* dropped)
		{
			m_items.push_back(Entry{std::move(item), cost});
			m_cost += cost;
			while (m_items.size() > 1 && (m_items.size() > m_max_items || m_cost > m_max_cost))
			{
				m_cost -= m_items.front().cost;
				if (dropped)
					dropped->push_back(std::move(m_items.front().item));
				m_items.pop_front();
			}
		}

		/// Removes and returns the newest item.
		std::optional<T> PopNewest()
		{
			if (m_items.empty())
				return std::nullopt;

			std::optional<T> ret(std::move(m_items.back().item));
			m_cost -= m_items.back().cost;
			m_items.pop_back();
			return ret;
		}

		/// Removes every item, oldest first, into *removed (if non-null).
		void Clear(std::vector<T>* removed)
		{
			if (removed)
			{
				for (Entry& e : m_items)
					removed->push_back(std::move(e.item));
			}
			m_items.clear();
			m_cost = 0;
		}

		bool Empty() const { return m_items.empty(); }
		size_t Size() const { return m_items.size(); }
		size_t Cost() const { return m_cost; }

	private:
		struct Entry
		{
			T item;
			size_t cost;
		};

		std::deque<Entry> m_items;
		size_t m_cost = 0;
		size_t m_max_items;
		size_t m_max_cost;
	};

	/// Mip level count, base included, of an upscaled texture that is built from `requested` guest
	/// levels of a (width x height) texture.
	///
	/// Guest level i is max(1, width >> i) by max(1, height >> i), and upscaling doubles it. A GPU
	/// texture of 2*width by 2*height has levels of max(1, (2*width) >> i) by max(1, (2*height) >> i).
	/// The two agree while neither dimension has been clamped to one pixel (and for non power of two
	/// region widths, only until a level is odd). From the first level where they differ, an
	/// upscaled level would not fit its slot, so the chain stops there. The sampler clamps to the
	/// last level, which only matters for levels already a single pixel on one side.
	inline u32 UpscaledMipLevelCount(u32 width, u32 height, u32 requested)
	{
		if (requested <= 1)
			return 1;

		const u32 big_w = width * 2;
		const u32 big_h = height * 2;
		u32 count = 1;
		for (u32 level = 1; level < requested; level++)
		{
			const u32 guest_w = std::max(width >> level, 1u);
			const u32 guest_h = std::max(height >> level, 1u);
			const u32 slot_w = std::max(big_w >> level, 1u);
			const u32 slot_h = std::max(big_h >> level, 1u);
			if (guest_w * 2 != slot_w || guest_h * 2 != slot_h)
				break;

			count = level + 1;
		}
		return count;
	}

	/// Builds levels 1..total_levels-1 of a mip chain from an RGBA8 base level by 2x2 box filtering.
	/// Level i is max(1, width >> i) by max(1, height >> i), the layout a GPU texture of the base
	/// size has. Where the previous level has an odd size the last row or column is read once, not
	/// twice. Colour and alpha are averaged per channel, with rounding to nearest, as a GPU
	/// generates mips. MipT needs width, height, pitch (bytes) and data (a vector of u8).
	template <typename MipT>
	void BuildBoxMipChain(const u8* base, u32 width, u32 height, u32 pitch, u32 total_levels, std::vector<MipT>* mips)
	{
		mips->clear();
		if (total_levels <= 1 || width == 0 || height == 0)
			return;

		mips->reserve(total_levels - 1);
		u32 prev_w = width;
		u32 prev_h = height;
		u32 prev_pitch = pitch;
		for (u32 level = 1; level < total_levels; level++)
		{
			const u8* prev = (level == 1) ? base : mips->back().data.data();

			MipT mip;
			mip.width = std::max(width >> level, 1u);
			mip.height = std::max(height >> level, 1u);
			mip.pitch = mip.width * 4;
			mip.data.resize(static_cast<size_t>(mip.pitch) * mip.height);

			for (u32 y = 0; y < mip.height; y++)
			{
				const u8* row0 = prev + static_cast<size_t>(std::min(y * 2, prev_h - 1)) * prev_pitch;
				const u8* row1 = prev + static_cast<size_t>(std::min(y * 2 + 1, prev_h - 1)) * prev_pitch;
				u8* out = mip.data.data() + static_cast<size_t>(y) * mip.pitch;
				for (u32 x = 0; x < mip.width; x++)
				{
					const u32 x0 = std::min(x * 2, prev_w - 1) * 4;
					const u32 x1 = std::min(x * 2 + 1, prev_w - 1) * 4;
					for (u32 c = 0; c < 4; c++)
					{
						const u32 sum = static_cast<u32>(row0[x0 + c]) + row0[x1 + c] + row1[x0 + c] + row1[x1 + c];
						out[x * 4 + c] = static_cast<u8>((sum + 2) >> 2);
					}
				}
			}

			prev_w = mip.width;
			prev_h = mip.height;
			prev_pitch = mip.pitch;
			mips->push_back(std::move(mip));
		}
	}
} // namespace GSTextureUpscaleSupport
