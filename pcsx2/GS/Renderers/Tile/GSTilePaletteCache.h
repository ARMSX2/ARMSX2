// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>

class GSTexture;

// The Tile renderer's palettes on the device: an N×1 RGBA8 texture whose texel i is
// palette entry i as GSClut::Read32 expanded it (16-bit entries already widened
// through TEXA, the CSA read group already applied), N being 16 for a four-bit index
// and 256 for an eight-bit one. The fragment shader indexes it with the texel it
// fetched from an index texture — the "expand every filter corner through the CLUT,
// then blend the colours" order the gs-idxfilt console capture measured, which is
// also what the software scanline does — so the index texture is palette-independent
// and one texture serves every palette a game ever cycles through it.
//
// Keyed on CONTENT, never on the CLUT's registers or its write generation: two loads
// of the same bytes are the same palette and one load of different bytes is not,
// whatever CBP said. A hit compares the words, so a hash collision costs a build and
// never serves the wrong colours. Textures are immutable once built (a palette in
// use by a recorded draw is never updated in place); eviction recycles through the
// device, which defers the destruction past the fence.
class GSTilePaletteCache
{
public:
	GSTilePaletteCache();
	~GSTilePaletteCache();

	/// The palette texture for these words, built if absent, or null on allocation
	/// failure. `entries` is 16 or 256; the words are the CLUT read buffer.
	/// `read_gen` is GSClut::GetReadGeneration() — while it holds still the buffer's
	/// bytes are bit-identical, so a repeat call skips the hash and the entry scan
	/// entirely and returns the last texture. Content keying is unchanged: the memo
	/// is only ever a shortcut to the same answer the full path would give.
	///
	/// `content_id`, when given, receives the palette's content identity: the FNV-64
	/// of its words. CONTENT-derived rather than build-instance-derived on purpose —
	/// GT4 rotates more palettes per frame than the cache holds, so the same words
	/// rebuild into recycled slots every frame, and an instance id would make every
	/// rebuild look like a new palette to derived caches. The id is hash-only (no
	/// byte verify behind it): a derived cache keyed on it accepts the same 64-bit
	/// collision stance as Classic's hash cache, which has keyed palettes this way
	/// for years. Within a pair the entry count adds nothing (the index texture's
	/// format fixes it). This cache's OWN lookups still verify words exactly.
	GSTexture* Lookup(const u32* clut, u32 entries, u32 read_gen, u64* content_id = nullptr);

	/// The content id Lookup would hand back for these words, computed without touching
	/// the cache or the device. Same function, same value: a caller that only needs the
	/// palette's identity (to key a derived cache, or to ask whether a pair would be
	/// admitted) pays the hash and nothing else.
	static u64 ContentId(const u32* clut, u32 entries);

	/// The pin discipline's clock; zero (the default, and what the Tile renderer leaves it at)
	/// turns it off and every branch below is inert. A renderer that RECORDS draws and issues them
	/// later sets it, and advances it when the recorded plan has executed. A palette an unissued
	/// draw names is then never evicted or recycled out from under it — the half this cache did not
	/// already have, since its textures are immutable once built and so can never be rebuilt in
	/// place. See GSTileTextureSource::SetFrame for the whole argument.
	void SetFrame(u64 frame) { m_frame = frame; }

	/// Recycles every cached texture (reset / teardown / hot-switch).
	void Clear();

	u64 Hits() const { return m_hits; }
	u64 Builds() const { return m_builds; }
	/// Lookups refused because every entry is pinned by this frame. Zero is the expected number;
	/// anything else says the frame's palette working set has outgrown the cache.
	u64 CapacityRefusals() const { return m_capacity_refusals; }

private:
	// 128 capacity-thrashed on GT4 (198 rebuilds per frame, each a texture create
	// plus a 1 KB upload, for palettes the title rotates through every frame). A
	// palette texture is N×1 texels, so capacity here is nearly free on both sides.
	static constexpr u32 kMaxEntries = 1024;

	struct Entry
	{
		u64 hash = 0;
		u64 last_use = 0;
		u64 pinned_frame = 0; ///< the frame a caller was handed this entry in; 0 = never
		u32 entries = 0;
		bool alive = false;
		GSTexture* tex = nullptr;
		std::array<u32, 256> words{};
	};

	bool Pinned(const Entry& e) const { return m_frame != 0 && e.pinned_frame == m_frame; }

	std::array<Entry, kMaxEntries> m_entries;
	u64 m_use_counter = 0;
	u64 m_hits = 0;
	u64 m_builds = 0;
	u64 m_frame = 0;
	u64 m_capacity_refusals = 0;

	// Last-answer memo: valid while the CLUT read buffer provably has not changed
	// (same read generation + entry count) and the slot provably still holds the
	// same palette (its last_use is the one this memo stamped — a slot can only be
	// rebuilt after becoming LRU, which changes last_use first).
	bool m_memo_valid = false;
	u32 m_memo_read_gen = 0;
	u32 m_memo_entries = 0;
	u32 m_memo_slot = 0;
	u64 m_memo_last_use = 0;
};
