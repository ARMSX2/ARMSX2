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

GSTexture* GSTilePaletteCache::Lookup(const u32* clut, u32 entries)
{
	pxAssert(entries == 16 || entries == 256);
	const u64 hash = HashWords(clut, entries);

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
			m_hits++;
			return e.tex;
		}
		if (!lru || e.last_use < lru->last_use)
			lru = &e;
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
	std::memcpy(e.words.data(), clut, entries * sizeof(u32));
	m_builds++;
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
	}
	m_use_counter = 0;
}
