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
	GSTexture* Lookup(const u32* clut, u32 entries);

	/// Recycles every cached texture (reset / teardown / hot-switch).
	void Clear();

	u64 Hits() const { return m_hits; }
	u64 Builds() const { return m_builds; }

private:
	static constexpr u32 kMaxEntries = 128;

	struct Entry
	{
		u64 hash = 0;
		u64 last_use = 0;
		u32 entries = 0;
		bool alive = false;
		GSTexture* tex = nullptr;
		std::array<u32, 256> words{};
	};

	std::array<Entry, kMaxEntries> m_entries;
	u64 m_use_counter = 0;
	u64 m_hits = 0;
	u64 m_builds = 0;
};
