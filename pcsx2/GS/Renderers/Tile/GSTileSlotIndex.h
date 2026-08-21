// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/Pcsx2Types.h"

#include <array>
#include <bit>

// The slot bookkeeping the two texture caches (GSTileTextureSource, GSTileExpandedCache) do on
// every draw, taken out of the entry array. Both caches used to answer all three questions below
// with ONE linear walk of 512 entries: which slot holds this key, which slot is free, and which
// occupied slot is the least recently used. That walk is why a per-draw admission probe was the
// largest single symbol in the SD865 GS-thread profile — the source cache's entries are 256 bytes
// each, so the walk streams 128 KB against a 64 KB L1D, and a REFUSAL (the common outcome) walks
// all of it because absence is only proven at the end.
//
// This class answers the same three questions off ~9 KB of side tables that stay resident:
//
//  - identity: a chained hash index over the caller's key hash. A miss touches the bucket head
//    and whatever short chain hangs off it, and touches NO entry — the stored hash is compared
//    first, so an unrelated slot is rejected without the 256-byte entry being read at all.
//  - a free slot: a bitmap, lowest index first.
//  - the victim: an intrusive recency list, oldest end first.
//
// It is deliberately a mirror, not a replacement. The caches keep their `alive` flags and their
// `last_use` stamps; this class must be told about every change to either, and the correctness
// argument is exactly that pairing. In particular list order IS last_use order — which is what
// makes "the oldest unpinned slot in the list" the same slot the old "smallest last_use among
// unpinned entries" scan picked, on every eviction, forever.
//
// The caller owns key comparison. This class never sees a key, only its hash, so the two caches
// share it despite keying on different words (four for a texture window, two for a palette pair).
template <u32 CAPACITY, u32 BUCKETS>
class GSTileSlotIndex
{
	static_assert(CAPACITY < 0xFFFFu, "slot ids are u16 with 0xFFFF reserved as the nil link");
	static_assert(BUCKETS != 0 && (BUCKETS & (BUCKETS - 1)) == 0, "bucket count must be a power of two");
	static_assert((CAPACITY % 64) == 0, "the free bitmap is whole 64-bit words");

public:
	static constexpr u16 kNil = 0xFFFFu;

	GSTileSlotIndex() { Clear(); }

	/// Every slot free, no keys, empty recency list.
	void Clear()
	{
		m_bucket.fill(kNil);
		m_chain.fill(kNil);
		m_hash.fill(0);
		m_older.fill(kNil);
		m_newer.fill(kNil);
		m_free.fill(~static_cast<u64>(0));
		m_newest = kNil;
		m_oldest = kNil;
	}

	/// The occupied slot under `hash` whose key `match(slot)` accepts, or kNil. Const and free of
	/// side effects, which is what lets the const probe road use it. `match` runs only on a slot
	/// whose stored hash is equal, so it is the collision test and not the search.
	template <typename Match>
	u16 Find(u64 hash, Match&& match) const
	{
		for (u16 s = m_bucket[hash & (BUCKETS - 1)]; s != kNil; s = m_chain[s])
		{
			if (m_hash[s] == hash && match(s))
				return s;
		}
		return kNil;
	}

	/// The lowest-numbered free slot, or kNil when every slot is occupied. Lowest-numbered rather
	/// than most-recently-freed on purpose: it is the slot the linear scan this replaces took, and
	/// a fresh entry overwrites every field of the slot it lands in, so the choice is unobservable
	/// — but "unobservable" is a claim about today's code, and matching the old order costs eight
	/// loads.
	u16 FreeSlot() const
	{
		for (u32 w = 0; w < kWords; w++)
		{
			if (m_free[w] != 0)
				return static_cast<u16>(w * 64 + static_cast<u32>(std::countr_zero(m_free[w])));
		}
		return kNil;
	}

	/// The least recently touched occupied slot `evictable(slot)` accepts, walking toward the most
	/// recent, or kNil when it accepts none. Equal to "the smallest last_use among the entries the
	/// old scan would consider" because every last_use write is paired with Touch below.
	template <typename Evictable>
	u16 Victim(Evictable&& evictable) const
	{
		for (u16 s = m_oldest; s != kNil; s = m_newer[s])
		{
			if (evictable(s))
				return s;
		}
		return kNil;
	}

	/// Occupy `slot` under `hash`: it leaves the free set, joins its bucket, and becomes the newest
	/// entry in the recency list — which is where an entry whose last_use was just stamped belongs.
	void Occupy(u16 slot, u64 hash)
	{
		m_free[slot / 64] &= ~(static_cast<u64>(1) << (slot % 64));
		m_hash[slot] = hash;
		const u32 b = static_cast<u32>(hash & (BUCKETS - 1));
		m_chain[slot] = m_bucket[b];
		m_bucket[b] = slot;
		LinkNewest(slot);
	}

	/// Release `slot`: out of its bucket, out of the recency list, back in the free set. The bucket
	/// unlink is a chain walk rather than a tombstone, so a cache that churns for hours degrades to
	/// nothing.
	void Release(u16 slot)
	{
		const u32 b = static_cast<u32>(m_hash[slot] & (BUCKETS - 1));
		u16 prev = kNil;
		for (u16 s = m_bucket[b]; s != kNil; s = m_chain[s])
		{
			if (s == slot)
			{
				if (prev == kNil)
					m_bucket[b] = m_chain[s];
				else
					m_chain[prev] = m_chain[s];
				break;
			}
			prev = s;
		}
		m_chain[slot] = kNil;
		m_hash[slot] = 0;
		Unlink(slot);
		m_free[slot / 64] |= (static_cast<u64>(1) << (slot % 64));
	}

	/// `slot` becomes the most recently used. Pair this with every write of the entry's own
	/// last_use stamp and with nothing else: the two orders agreeing is the whole argument that
	/// eviction picks the same entry it used to.
	void Touch(u16 slot)
	{
		if (m_newest == slot)
			return;
		Unlink(slot);
		LinkNewest(slot);
	}

private:
	static constexpr u32 kWords = CAPACITY / 64;

	void Unlink(u16 slot)
	{
		const u16 older = m_older[slot];
		const u16 newer = m_newer[slot];
		if (older != kNil)
			m_newer[older] = newer;
		else
			m_oldest = newer;
		if (newer != kNil)
			m_older[newer] = older;
		else
			m_newest = older;
		m_older[slot] = kNil;
		m_newer[slot] = kNil;
	}

	void LinkNewest(u16 slot)
	{
		m_older[slot] = m_newest;
		m_newer[slot] = kNil;
		if (m_newest != kNil)
			m_newer[m_newest] = slot;
		else
			m_oldest = slot;
		m_newest = slot;
	}

	std::array<u16, BUCKETS> m_bucket;  ///< head of each hash chain
	std::array<u16, CAPACITY> m_chain;  ///< next slot in the bucket's chain
	std::array<u64, CAPACITY> m_hash;   ///< the key hash a slot is filed under
	std::array<u16, CAPACITY> m_older;  ///< recency list, toward the least recently used
	std::array<u16, CAPACITY> m_newer;  ///< recency list, toward the most recently used
	std::array<u64, kWords> m_free;     ///< set bit = slot is free
	u16 m_newest = kNil;
	u16 m_oldest = kNil;
};

/// The key hash both caches file their slots under. Not a cryptographic anything and not the
/// identity — the identity stays exact key equality on the caller's side, and this only has to
/// spread a few hundred live keys over the buckets. Each word gets its own odd multiplier so the
/// zero words a short key leaves behind (a single-level window's mip pair, a direct-colour
/// window's TEXA) do not collapse distinct keys together, and one finalizer avalanches the low
/// bits the bucket mask reads.
__forceinline_odr u64 GSTileSlotHash(u64 a, u64 b = 0, u64 c = 0, u64 d = 0)
{
	u64 x = a * 0x9E3779B97F4A7C15ULL ^ b * 0xC2B2AE3D27D4EB4FULL ^ c * 0x165667B19E3779F9ULL ^
			d * 0x27D4EB2F165667C5ULL;
	x ^= x >> 33;
	x *= 0xFF51AFD7ED558CCDULL;
	x ^= x >> 29;
	return x;
}
