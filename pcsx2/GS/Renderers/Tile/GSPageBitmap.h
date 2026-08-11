// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"

#include "common/Assertions.h"

#include <bit>

// A set of GS memory pages as one 512-bit bitmap (GS_MAX_PAGES bits in eight 64-bit
// words, one cache line). This is the currency of the Tile renderer's interval memory
// model: every draw footprint, validity plane and truth plane is one of these, and
// provable independence between any two footprints is a single intersects() call.
// Pure data — no GS state, no device, no allocation — so the whole type is exercisable
// in ctest against a scalar reference.
//
// Storage is u64 words rather than GSVector4i on purpose: the GSVector union-punning
// idiom is only defined under the core's -fno-strict-aliasing, and this header is
// consumed by TUs outside that flag (unit tests today, tools tomorrow). The bulk ops
// below are straight-line word ops over one cache line — the vectorizer turns them
// into the same NEON the union would have hand-written, without the UB.
class alignas(64) GSPageBitmap
{
	static constexpr u32 kWords = GS_MAX_PAGES / 64;

	u64 m_w[kWords];

public:
	__forceinline GSPageBitmap()
	{
		clear();
	}

	__forceinline void clear()
	{
		for (u32 i = 0; i < kWords; i++)
			m_w[i] = 0;
	}

	__forceinline void set(u32 page)
	{
		pxAssert(page < GS_MAX_PAGES);
		m_w[page >> 6] |= u64(1) << (page & 63);
	}

	__forceinline void unset(u32 page)
	{
		pxAssert(page < GS_MAX_PAGES);
		m_w[page >> 6] &= ~(u64(1) << (page & 63));
	}

	__forceinline bool test(u32 page) const
	{
		pxAssert(page < GS_MAX_PAGES);
		return (m_w[page >> 6] >> (page & 63)) & 1;
	}

	/// Accumulate every page enumerated by a GSOffset::PageLooper (or anything else with
	/// the same loopPages contract). Templated so this header stays free of GSLocalMemory.
	template <typename PageLooper>
	__forceinline void addPages(const PageLooper& looper)
	{
		looper.loopPages([this](u32 page) { set(page); });
	}

	__forceinline bool empty() const
	{
		u64 acc = 0;
		for (u32 i = 0; i < kWords; i++)
			acc |= m_w[i];
		return acc == 0;
	}

	__forceinline bool intersects(const GSPageBitmap& o) const
	{
		u64 acc = 0;
		for (u32 i = 0; i < kWords; i++)
			acc |= m_w[i] & o.m_w[i];
		return acc != 0;
	}

	/// True when every page of o is also in this set.
	__forceinline bool contains(const GSPageBitmap& o) const
	{
		u64 acc = 0;
		for (u32 i = 0; i < kWords; i++)
			acc |= o.m_w[i] & ~m_w[i];
		return acc == 0;
	}

	__forceinline u32 count() const
	{
		u32 n = 0;
		for (u32 i = 0; i < kWords; i++)
			n += std::popcount(m_w[i]);
		return n;
	}

	/// Visit every set page in ascending order. Fn: void(*)(u32).
	template <typename Fn>
	void forEachSetPage(Fn&& fn) const
	{
		for (u32 i = 0; i < kWords; i++)
		{
			u64 bits = m_w[i];
			const u32 base = i * 64;
			while (bits)
			{
				fn(base + std::countr_zero(bits));
				bits &= bits - 1;
			}
		}
	}

	__forceinline GSPageBitmap operator|(const GSPageBitmap& o) const
	{
		GSPageBitmap r(NoInit{});
		for (u32 i = 0; i < kWords; i++)
			r.m_w[i] = m_w[i] | o.m_w[i];
		return r;
	}

	__forceinline GSPageBitmap operator&(const GSPageBitmap& o) const
	{
		GSPageBitmap r(NoInit{});
		for (u32 i = 0; i < kWords; i++)
			r.m_w[i] = m_w[i] & o.m_w[i];
		return r;
	}

	/// this & ~o.
	__forceinline GSPageBitmap andnot(const GSPageBitmap& o) const
	{
		GSPageBitmap r(NoInit{});
		for (u32 i = 0; i < kWords; i++)
			r.m_w[i] = m_w[i] & ~o.m_w[i];
		return r;
	}

	__forceinline GSPageBitmap& operator|=(const GSPageBitmap& o)
	{
		for (u32 i = 0; i < kWords; i++)
			m_w[i] |= o.m_w[i];
		return *this;
	}

	__forceinline GSPageBitmap& operator&=(const GSPageBitmap& o)
	{
		for (u32 i = 0; i < kWords; i++)
			m_w[i] &= o.m_w[i];
		return *this;
	}

	__forceinline bool operator==(const GSPageBitmap& o) const
	{
		u64 acc = 0;
		for (u32 i = 0; i < kWords; i++)
			acc |= m_w[i] ^ o.m_w[i];
		return acc == 0;
	}

	__forceinline bool operator!=(const GSPageBitmap& o) const
	{
		return !(*this == o);
	}

private:
	struct NoInit
	{
	};

	explicit __forceinline GSPageBitmap(NoInit)
	{
	}
};

static_assert(GS_MAX_PAGES == 512, "GSPageBitmap hardcodes the 4 MB / 8 KB page geometry");
static_assert(sizeof(GSPageBitmap) == 64, "one cache line");
