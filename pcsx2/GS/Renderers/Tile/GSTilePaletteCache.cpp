// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTilePaletteCache.h"

#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"

#include <cstring>

namespace
{
// FNV-1a over the words: 64 bits so an equal hash almost always means an equal
// palette, with the compare on the words behind it deciding for certain.
u64 HashWords(const u32* words, u32 count)
{
	u64 h = 0xCBF29CE484222325ull;
	for (u32 i = 0; i < count; i++)
	{
		h ^= words[i];
		h *= 0x100000001B3ull;
	}
	return h;
}
} // namespace

GSTilePaletteCache::GSTilePaletteCache() = default;

GSTilePaletteCache::~GSTilePaletteCache()
{
	Clear();
}

u64 GSTilePaletteCache::ContentId(const u32* clut, u32 entries)
{
	return HashWords(clut, entries);
}

GSTexture* GSTilePaletteCache::Lookup(const u32* clut, u32 entries, u32 read_gen, u64* content_id)
{
	pxAssert(entries == 16 || entries == 256);

	if (GSTexture* memo = LookupMemo(entries, read_gen, content_id))
		return memo;

	return LookupByHash(clut, entries, read_gen, HashWords(clut, entries), content_id);
}

GSTexture* GSTilePaletteCache::Lookup(const u32* clut, u32 entries, u32 read_gen, u64 content_id,
	u64* out_content_id)
{
	pxAssert(entries == 16 || entries == 256);
	// ⚠️ NOT asserted against HashWords here on purpose: the assert would recompute the hash this
	// overload exists to skip, and Devel keeps pxAssert, which is the build every measurement is
	// taken on. The two forms are pinned equal in gs_tilegpu_palette_dedup_tests instead.

	// The memo first, exactly as the hashing form does. It is not a shortcut past the hash here --
	// the caller already paid that -- but past the entry scan and the memcmp, and it is where
	// last_use and pinned_frame get refreshed.
	if (GSTexture* memo = LookupMemo(entries, read_gen, out_content_id))
		return memo;

	return LookupByHash(clut, entries, read_gen, content_id, out_content_id);
}

GSTexture* GSTilePaletteCache::LookupMemo(u32 entries, u32 read_gen, u64* content_id)
{
	if (m_memo_valid && read_gen == m_memo_read_gen && entries == m_memo_entries)
	{
		Entry& me = m_entries[m_memo_slot];
		if (me.alive && me.last_use == m_memo_last_use)
		{
			me.last_use = ++m_use_counter;
			me.pinned_frame = m_frame;
			m_memo_last_use = me.last_use;
			m_hits++;
			if (content_id)
				*content_id = me.hash;
			return me.tex;
		}
	}
	return nullptr;
}

GSTexture* GSTilePaletteCache::LookupByHash(const u32* clut, u32 entries, u32 read_gen, u64 hash,
	u64* content_id)
{
	Entry* lru = nullptr;
	Entry* free_slot = nullptr;
	for (Entry& e : m_entries)
	{
		if (!e.alive)
		{
			if (!free_slot)
				free_slot = &e;
			continue;
		}
		if (e.hash == hash && e.entries == entries && std::memcmp(e.words.data(), clut, entries * sizeof(u32)) == 0)
		{
			e.last_use = ++m_use_counter;
			e.pinned_frame = m_frame;
			m_hits++;
			m_memo_valid = true;
			m_memo_read_gen = read_gen;
			m_memo_entries = entries;
			m_memo_slot = static_cast<u32>(&e - m_entries.data());
			m_memo_last_use = e.last_use;
			if (content_id)
				*content_id = e.hash;
			return e.tex;
		}
		// A palette a recorded-but-unissued draw names may not be recycled into the pool, where the
		// next CreateTexture would take it and overwrite it. Inert with the discipline off.
		if (!Pinned(e) && (!lru || e.last_use < lru->last_use))
			lru = &e;
	}

	if (!free_slot && !lru)
	{
		m_capacity_refusals++;
		return nullptr;
	}
	Entry& e = free_slot ? *free_slot : *lru;
	if (e.alive && e.tex)
	{
		g_gs_device->Recycle(e.tex);
		e.tex = nullptr;
	}
	e.alive = false;

	GSTexture* tex = g_gs_device->CreateTexture(static_cast<int>(entries), 1, 1, GSTexture::Format::Color, false);
	if (!tex)
		return nullptr;
	if (!tex->Update(GSVector4i(0, 0, static_cast<int>(entries), 1), clut, entries * sizeof(u32)))
	{
		g_gs_device->Recycle(tex);
		return nullptr;
	}

	e.alive = true;
	e.hash = hash;
	e.entries = entries;
	e.tex = tex;
	e.last_use = ++m_use_counter;
	e.pinned_frame = m_frame;
	std::memcpy(e.words.data(), clut, entries * sizeof(u32));
	m_builds++;
	m_memo_valid = true;
	m_memo_read_gen = read_gen;
	m_memo_entries = entries;
	m_memo_slot = static_cast<u32>(&e - m_entries.data());
	m_memo_last_use = e.last_use;
	if (content_id)
		*content_id = hash;
	return tex;
}

void GSTilePaletteCache::Clear()
{
	for (Entry& e : m_entries)
	{
		if (e.tex)
		{
			g_gs_device->Recycle(e.tex);
			e.tex = nullptr;
		}
		e.alive = false;
		e.pinned_frame = 0;
	}
	m_use_counter = 0;
	m_memo_valid = false;
}
