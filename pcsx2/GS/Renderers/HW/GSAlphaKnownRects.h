// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "GS/GSVector.h"
#include "GS/Renderers/HW/GSAlphaKnownBits.h"

/// Alpha bits known inside rectangles of a render target, beside the whole-target pair.
///
/// The pair in GSAlphaKnownBits learns only from a write that covers the target's whole valid
/// rect. A game that clears an offset or letterboxed area never makes one, and the valid rect
/// grows and shrinks with lookups and overlap trimming the game does not control. Here a write
/// that fills its own rectangle without gaps establishes knowledge in that rectangle, wherever it
/// sits. A later write that misses the rectangle leaves it alone; one that fills all of it
/// rewrites it; one that touches part of it keeps only the bits it agrees on.
///
/// The rectangles describe the pixels of one texture allocation under one target layout (Key).
/// When either changes they are forgotten. So is anything the texture cache writes into the
/// texture other than a draw, except a local memory upload, which erodes them like a partial
/// write.
///
/// Kept in its own header so it can be tested without a GS device.
namespace GSAlphaKnownRects
{
	using GSAlphaKnownBits::Known;

	/// Up to this many rectangles per target. A frame area and one more is what the titles that
	/// need this use; a third cover is rare and evicts the oldest.
	inline constexpr u32 MaxRects = 2;

	/// Which pixels the rectangles describe: the texture allocation (GSTexture::GetLease) and the
	/// target's TBP0, TBW and PSM.
	struct Key
	{
		u64 texture = 0;
		u32 layout = 0;

		constexpr bool operator==(const Key& r) const { return texture == r.texture && layout == r.layout; }
	};

	/// A draw's write to the alpha byte.
	struct Write
	{
		GSVector4i rect; ///< every pixel the draw can write, in the target's unscaled pixels
		u8 written; ///< the alpha bits the mask lets through
		u8 src_lo; ///< the fragment alpha range
		u8 src_hi;
		bool covers; ///< every pixel of `rect` takes the write
	};

	/// Two claims about the same pixels, merged: every bit either knows, except bits they disagree
	/// on, which neither can be trusted for.
	inline constexpr Known Combine(Known a, Known b)
	{
		const u8 conflict = static_cast<u8>(a.bits & b.bits & (a.value ^ b.value));
		const u8 bits = static_cast<u8>((a.bits | b.bits) & ~conflict);
		return {bits, static_cast<u8>((a.value | b.value) & bits)};
	}

	class Set
	{
	public:
		u32 Size() const { return m_count; }

		void Clear() { m_count = 0; }

		/// What every pixel of `r` is known to hold: the rectangles that contain `r`, combined.
		Known Inside(const Key& key, const GSVector4i& r) const
		{
			Known k = Known::Nothing();
			if (!(key == m_key))
				return k;

			bool any = false;
			for (u32 i = 0; i < m_count; i++)
			{
				if (!m_rects[i].rect.rcontains(r))
					continue;
				k = any ? Combine(k, m_rects[i].known) : m_rects[i].known;
				any = true;
			}
			return k;
		}

		/// After a draw wrote `w`. `texture_rect` is the texture's whole unscaled rect: pixels
		/// outside it are not stored, so no claim is made about them.
		void AfterWrite(const Key& key, const Write& w, const GSVector4i& texture_rect)
		{
			if (!(key == m_key))
			{
				m_count = 0;
				m_key = key;
			}
			if (m_count == 0 && !w.covers)
				return;

			const GSVector4i r = w.rect.rintersect(texture_rect);
			if (r.rempty())
				return;

			m_clock++;
			for (u32 i = 0; i < m_count; i++)
			{
				Entry& e = m_rects[i];
				if (!e.rect.rintersects(r))
					continue;

				const bool rewrites = w.covers && r.rcontains(e.rect);
				e.known = GSAlphaKnownBits::AfterWrite(e.known, w.written, w.src_lo, w.src_hi, rewrites);
				if (rewrites)
					e.stamp = m_clock;
			}
			DropEmpty();

			if (!w.covers)
				return;

			// A cover inside a rectangle that survived keeps what that rectangle knows and starts
			// none of its own: small gapless sprites inside a frame (letterbox bars, HUD panels)
			// would otherwise evict the frame.
			for (u32 i = 0; i < m_count; i++)
			{
				if (m_rects[i].rect.rcontains(r))
					return;
			}

			const Known fresh = GSAlphaKnownBits::AfterWrite(Known::Nothing(), w.written, w.src_lo, w.src_hi, true);
			if (fresh.bits == 0)
				return;

			// Rectangles inside the new one that know no more than it does add nothing.
			for (u32 i = 0; i < m_count;)
			{
				if (r.rcontains(m_rects[i].rect) && m_rects[i].known == fresh)
					Remove(i);
				else
					i++;
			}

			u32 slot = m_count;
			if (m_count == MaxRects)
			{
				slot = 0;
				for (u32 i = 1; i < m_count; i++)
				{
					if (m_rects[i].stamp < m_rects[slot].stamp)
						slot = i;
				}
			}
			else
			{
				m_count++;
			}
			m_rects[slot] = {r, fresh, m_clock};
		}

		/// After local memory holding alpha in [lo, hi] was written over `r` of the texture.
		void AfterUpload(const GSVector4i& r, u8 lo, u8 hi)
		{
			for (u32 i = 0; i < m_count; i++)
			{
				if (m_rects[i].rect.rintersects(r))
					m_rects[i].known = GSAlphaKnownBits::AfterUpload(m_rects[i].known, lo, hi, false);
			}
			DropEmpty();
		}

		/// Keeps only the alpha bits the target's format stores.
		void KeepBits(u8 bits)
		{
			for (u32 i = 0; i < m_count; i++)
			{
				m_rects[i].known.bits &= bits;
				m_rects[i].known.value &= bits;
			}
			DropEmpty();
		}

		/// After the texture was reallocated with its pixels copied in place from the origin:
		/// the rectangles move to the new allocation, cut to the part that was copied.
		void Moved(const Key& from, const Key& to, const GSVector4i& copied)
		{
			if (!(from == m_key))
			{
				m_count = 0;
				return;
			}
			m_key = to;
			for (u32 i = 0; i < m_count;)
			{
				m_rects[i].rect = m_rects[i].rect.rintersect(copied);
				if (m_rects[i].rect.rempty())
					Remove(i);
				else
					i++;
			}
		}

	private:
		struct Entry
		{
			GSVector4i rect;
			Known known;
			u32 stamp;
		};

		void Remove(u32 i)
		{
			m_rects[i] = m_rects[--m_count];
		}

		void DropEmpty()
		{
			for (u32 i = 0; i < m_count;)
			{
				if (m_rects[i].known.bits == 0)
					Remove(i);
				else
					i++;
			}
		}

		Entry m_rects[MaxRects];
		u32 m_count = 0;
		u32 m_clock = 0;
		Key m_key;
	};

	/// What a masked draw over `r` may rely on, and whether the rectangles supplied any of it.
	struct ForDraw
	{
		Known known;
		bool from_rects;
	};

	/// The whole-target pair answers alone when it knows every bit in `masked`, as it always has.
	/// Otherwise it is combined with the rectangles that contain the draw.
	inline ForDraw KnownForDraw(Known whole, const Set& rects, const Key& key, const GSVector4i& r, u8 masked)
	{
		if ((whole.bits & masked) == masked || rects.Size() == 0)
			return {whole, false};

		const Known k = Combine(whole, rects.Inside(key, r));
		return {k, (k.bits & masked) == masked};
	}
} // namespace GSAlphaKnownRects
