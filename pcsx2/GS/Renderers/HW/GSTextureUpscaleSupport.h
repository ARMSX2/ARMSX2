// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include "GS/Renderers/HW/GSTextureUpscaler.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

/// Pieces of the texture upscaler that need no GS, GPU or settings state, so they can be unit
/// tested on their own: the bounded newest-first job queue, the scale a texture is upscaled by,
/// the one or two 2x passes that make up a level's upscale, and the CPU mip chain builder.
namespace GSTextureUpscaleSupport
{
	/// The largest side, in pixels, of a texture that a 4x job upscales by 4. A bigger one gets 2x:
	/// at 1024 a 4x result would be 4096 on a side, 64 MB for one level, and CPU time to match.
	inline constexpr u32 MAX_4X_SOURCE_SIZE = 512;

	/// A level with a side under this has too little for the filter to work on and gets a plain
	/// bilinear 2x pass instead.
	inline constexpr u32 MIN_RAISR_LEVEL_SIZE = 8;

	/// The scale (2 or 4) a texture of this base size is upscaled by. want_4x is the 4x mode being
	/// on; it still only applies to textures of at most MAX_4X_SOURCE_SIZE on both sides.
	inline u32 UpscaleScaleForSize(bool want_4x, u32 width, u32 height)
	{
		return (want_4x && std::max(width, height) <= MAX_4X_SOURCE_SIZE) ? 4 : 2;
	}

	/// One 2x pass: RAISR, or bilinear when the image is too small for it.
	inline void UpscalePass2x(const GSTextureUpscaler::FilterSet& filters, const u8* src, u32 w, u32 h, u32 src_pitch,
		u8* dst, u32 dst_pitch)
	{
		if (std::min(w, h) >= MIN_RAISR_LEVEL_SIZE)
			GSTextureUpscaler::UpscaleRGBA8x2(filters, src, w, h, src_pitch, dst, dst_pitch);
		else
			GSTextureUpscaler::BilinearRGBA8x2(src, w, h, src_pitch, dst, dst_pitch);
	}

	/// Upscales an RGBA8 image by scale, which is 2 or 4. dst is (w * scale) x (h * scale). A 4x
	/// upscale is two 2x passes, the second one run on the first one's result; each pass picks RAISR
	/// or bilinear by the size of the image it is given, so a 4x4 level is bilinear to 8x8 and then
	/// RAISR to 16x16. src and dst must not overlap. Pitches are in bytes.
	inline void UpscaleRGBA8(const GSTextureUpscaler::FilterSet& filters, const u8* src, u32 w, u32 h, u32 src_pitch,
		u32 scale, u8* dst, u32 dst_pitch)
	{
		if (scale != 4)
		{
			UpscalePass2x(filters, src, w, h, src_pitch, dst, dst_pitch);
			return;
		}

		const u32 mid_w = w * 2;
		const u32 mid_h = h * 2;
		const u32 mid_pitch = mid_w * sizeof(u32);
		const std::unique_ptr<u8[]> mid(new u8[static_cast<size_t>(mid_pitch) * mid_h]);
		UpscalePass2x(filters, src, w, h, src_pitch, mid.get(), mid_pitch);
		UpscalePass2x(filters, mid.get(), mid_w, mid_h, mid_pitch, dst, dst_pitch);
	}

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
	/// levels of a (width x height) texture, each upscaled by `scale`.
	///
	/// Guest level i is max(1, width >> i) by max(1, height >> i), and upscaling multiplies it by
	/// scale. A GPU texture of scale*width by scale*height has levels of max(1, (scale*width) >> i)
	/// by max(1, (scale*height) >> i). The two agree while neither dimension has been clamped to one
	/// pixel (and for non power of two region sizes, only until a level is odd). From the first level
	/// where they differ, an upscaled level would not fit its slot, so the chain stops there. The
	/// sampler clamps to the last level, which only matters for levels already a single pixel on one
	/// side.
	inline u32 UpscaledMipLevelCount(u32 width, u32 height, u32 requested, u32 scale)
	{
		if (requested <= 1)
			return 1;

		const u32 big_w = width * scale;
		const u32 big_h = height * scale;
		u32 count = 1;
		for (u32 level = 1; level < requested; level++)
		{
			const u32 guest_w = std::max(width >> level, 1u);
			const u32 guest_h = std::max(height >> level, 1u);
			const u32 slot_w = std::max(big_w >> level, 1u);
			const u32 slot_h = std::max(big_h >> level, 1u);
			if (guest_w * scale != slot_w || guest_h * scale != slot_h)
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
