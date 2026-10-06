// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include "GS/Renderers/HW/GSTextureUpscaler.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

/// Pieces of the texture upscaler that need no GS, GPU or settings state, so they can be unit
/// tested on their own: the bounded newest-first job queue, the scale a texture is upscaled by,
/// the one or two 2x passes that make up a level's upscale, the pass that keeps an upscaled image
/// inside the range of its source, the pass that keeps hard alpha edges, and the CPU mip chain
/// builder.
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

	/// Clamps a 2x upscaled RGBA8 image, per channel, to the range of the four source texels each
	/// output pixel is interpolated from.
	///
	/// dst is (2 * width) by (2 * height), written by an upscaler that shares the centre aligned 2x
	/// bilinear grid: output pixel x reads source position x / 2 - 0.25, between source columns
	/// (x + 1) / 2 - 1 and (x + 1) / 2, and likewise for rows, both clamped to the image.
	///
	/// A sharpening filter overshoots at a hard edge. Beside a step from 113 to 223 the RAISR
	/// engine writes values down to 102 on the dark side and up to 229 on the bright side, which are
	/// colours that neither side of the edge has (Intel's Sharp set, no longer shipped, went to 82
	/// and 252). Sampled with a bilinear filter the extra detail
	/// reads as a halo; sampled with a nearest filter and magnified, which games do to read a flat
	/// colour out of a swatch atlas, every one of those texels becomes a visible block. After this
	/// pass no output pixel is darker than the darkest or brighter than the brightest of the four
	/// texels it comes from. Output that was already inside that range is not touched, and a region
	/// of flat texels whose neighbours are flat in the same colour comes out exactly flat.
	inline void ClampUpscaledToSourceRange(const u8* src, u32 width, u32 height, u32 src_pitch, u8* dst, u32 dst_pitch)
	{
		if (width == 0 || height == 0)
			return;

		const size_t row_bytes = static_cast<size_t>(width) * 4;
		std::vector<u8> lo(row_bytes);
		std::vector<u8> hi(row_bytes);

		for (u32 y = 0; y < height * 2; y++)
		{
			// The two source rows this output row is interpolated from, reduced to a per-column
			// minimum and maximum.
			const u32 below = (y + 1) / 2;
			const u8* row0 = src + static_cast<size_t>(below > 0 ? below - 1 : 0) * src_pitch;
			const u8* row1 = src + static_cast<size_t>(std::min(below, height - 1)) * src_pitch;
			for (size_t i = 0; i < row_bytes; i++)
			{
				lo[i] = std::min(row0[i], row1[i]);
				hi[i] = std::max(row0[i], row1[i]);
			}

			u8* out = dst + static_cast<size_t>(y) * dst_pitch;
			for (u32 x = 0; x < width * 2; x++)
			{
				const u32 right = (x + 1) / 2;
				const size_t c0 = static_cast<size_t>(right > 0 ? right - 1 : 0) * 4;
				const size_t c1 = static_cast<size_t>(std::min(right, width - 1)) * 4;
				u8* px = out + static_cast<size_t>(x) * 4;
				for (size_t c = 0; c < 4; c++)
				{
					const u8 low = std::min(lo[c0 + c], lo[c1 + c]);
					const u8 high = std::max(hi[c0 + c], hi[c1 + c]);
					px[c] = std::min(std::max(px[c], low), high);
				}
			}
		}
	}

	/// A difference in alpha, between source texels an output pixel is interpolated from, that
	/// makes the edge between them a mask edge. Three quarters of 0x80, which is what the PS2 stores
	/// for opaque. A mask is 0 against opaque, a step of 0x80 or more; a gradient has smaller steps.
	/// Black's lightmaps are the nearest gradient found: their steps reach just under 0x40, and the
	/// bands appear once the threshold is 0x38 or lower.
	inline constexpr u32 HARD_ALPHA_STEP = 0x60;

	/// Where a 2x upscale has interpolated across a hard alpha edge, takes the whole pixel, colour and
	/// alpha, from the source texel the output pixel lies inside instead. Everywhere else dst is left
	/// as the upscaler wrote it, which is the bilinear (or filtered) upscale of the source. Same layout
	/// as ClampUpscaledToSourceRange.
	///
	/// An output pixel counts as being on a hard edge when the four source texels it is interpolated
	/// from differ in alpha by HARD_ALPHA_STEP or more.
	///
	/// Alpha is data that games test against exact values and let decide depth writes, so a mask that
	/// is interpolated is a different mask. Katamari Damacy draws the King of All Cosmos's eyes as two
	/// rectangles of alpha 0x80 on alpha 0, tested NOTEQUAL 0 with depth writes, and draws his face
	/// afterwards with a depth test that the eyes' pixels make it fail. Interpolating gave each
	/// rectangle a ring of pixels with small non-zero alpha. They passed the test and kept the face
	/// out, but blended little of the eye over the dark cloud behind it: a dark frame, like glasses.
	/// With the texel's own alpha at the edge, a nearest sampled draw gets the original's mask and
	/// blend alpha under any alpha test. A bilinear sampled draw gets a mask edge half a texel wide
	/// instead of a texel, as it would from any 2x copy of the texture.
	///
	/// Colour comes with it because the colour of a transparent texel is whatever the game stored, and
	/// nothing is meant to show it. In the eyes it is black, against dark grey for the opaque texels.
	/// Interpolation blends that black into the output pixel next to the edge on the opaque side, and
	/// the clamp to the source range cannot stop it, since the black texel is one of the four sources.
	/// With the alpha kept exact that pixel is drawn, a dark outline round each rectangle. A pixel
	/// that is the texel it lies inside is what a nearest sample of the original gives, so it is right
	/// however the game draws the texture: alpha tested, blended, or with alpha ignored, which shows
	/// the transparent texels' colour as it is. Filling the transparent texels with the opaque colour
	/// before the upscale would also hide the outline, but it changes what that last kind of draw shows.
	///
	/// Alpha is not kept as the source texel's everywhere because gradients are also stored in
	/// alpha. Black bakes its lighting into 256x256 lightmaps and reads them with a bilinear sampler
	/// and a blend that scales the framebuffer by that alpha. Replacing the interpolated alpha with
	/// each texel's own made a staircase that the blend turned into broad dark bands.
	inline void KeepHardAlphaEdges2x(const u8* src, u32 width, u32 height, u32 src_pitch, u8* dst, u32 dst_pitch)
	{
		if (width == 0 || height == 0)
			return;

		// Per source column, the smallest and largest alpha of the two source rows an output row
		// is interpolated from.
		std::vector<u8> lo(width);
		std::vector<u8> hi(width);

		for (u32 y = 0; y < height * 2; y++)
		{
			const u32 below = (y + 1) / 2;
			const u8* row0 = src + static_cast<size_t>(below > 0 ? below - 1 : 0) * src_pitch + 3;
			const u8* row1 = src + static_cast<size_t>(std::min(below, height - 1)) * src_pitch + 3;
			for (u32 x = 0; x < width; x++)
			{
				lo[x] = std::min(row0[static_cast<size_t>(x) * 4], row1[static_cast<size_t>(x) * 4]);
				hi[x] = std::max(row0[static_cast<size_t>(x) * 4], row1[static_cast<size_t>(x) * 4]);
			}

			const u8* own = src + static_cast<size_t>(y / 2) * src_pitch;
			u8* out = dst + static_cast<size_t>(y) * dst_pitch;
			for (u32 x = 0; x < width * 2; x++)
			{
				const u32 right = (x + 1) / 2;
				const u32 c0 = right > 0 ? right - 1 : 0;
				const u32 c1 = std::min(right, width - 1);
				const u32 low = std::min(lo[c0], lo[c1]);
				const u32 high = std::max(hi[c0], hi[c1]);
				if (high - low >= HARD_ALPHA_STEP)
					std::memcpy(out + static_cast<size_t>(x) * 4, own + static_cast<size_t>(x / 2) * 4, 4);
			}
		}
	}

	/// One 2x pass: RAISR clamped to the range of its source (the filters overshoot at hard edges,
	/// and a game that reads a flat swatch with a nearest sampler shows every overshooting texel), or
	/// bilinear when the image is too small for RAISR. Bilinear cannot leave that range. On either
	/// road, output pixels on a hard alpha edge are replaced by the source texel they lie inside
	/// (KeepHardAlphaEdges2x), so a mask's edge is exact in colour as well as alpha.
	inline void UpscalePass2x(const GSTextureUpscaler::FilterSet& filters, const u8* src, u32 w, u32 h, u32 src_pitch,
		u8* dst, u32 dst_pitch)
	{
		if (std::min(w, h) >= MIN_RAISR_LEVEL_SIZE)
		{
			GSTextureUpscaler::UpscaleRGBA8x2(filters, src, w, h, src_pitch, dst, dst_pitch);
			ClampUpscaledToSourceRange(src, w, h, src_pitch, dst, dst_pitch);
		}
		else
		{
			GSTextureUpscaler::BilinearRGBA8x2(src, w, h, src_pitch, dst, dst_pitch);
		}

		KeepHardAlphaEdges2x(src, w, h, src_pitch, dst, dst_pitch);
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
