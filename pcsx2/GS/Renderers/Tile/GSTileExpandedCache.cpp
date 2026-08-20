// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTileExpandedCache.h"

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"

#include <algorithm>

GSTileExpandedCache::GSTileExpandedCache() = default;

GSTileExpandedCache::~GSTileExpandedCache()
{
	Clear();
}

GSTexture* GSTileExpandedCache::Lookup(GSTexture* index, u64 index_id, GSTexture* palette, u64 palette_id, u32 levels)
{
	if (!m_serves)
		return nullptr;

	Entry* found = nullptr;
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
		if (e.index_id == index_id && e.palette_id == palette_id)
		{
			found = &e;
			break;
		}
		if (!lru || e.last_use < lru->last_use)
			lru = &e;
	}

	if (found && found->tex)
	{
		found->last_use = ++m_use_counter;
		m_hits++;
		return found->tex;
	}

	if (!found)
	{
		// First sight: record the pair and defer. The marker shares the LRU pool
		// with real entries — it is one-shot by construction when the pair never
		// repeats, so it ages out first (stable entries are re-touched every frame).
		Entry& e = free_slot ? *free_slot : *lru;
		if (e.tex)
		{
			g_gs_device->Recycle(e.tex);
			e.tex = nullptr;
		}
		e.alive = true;
		e.index_id = index_id;
		e.palette_id = palette_id;
		e.last_use = ++m_use_counter;
		e.seen_frame = m_frame;
		m_deferrals++;
		return nullptr;
	}

	if (found->seen_frame == m_frame)
	{
		// Re-seen, but within the marker's own frame: a window with several draws
		// this frame proves nothing about the next one. Keep deferring — building
		// here is what turned per-frame-volatile windows into a build every frame.
		found->last_use = ++m_use_counter;
		m_deferrals++;
		return nullptr;
	}

	// The pair survived a frame boundary, so it earns its expansion.
	const int tw = index->GetWidth();
	const int th = index->GetHeight();

	// A single level renders straight into a render target; a mip pyramid is a
	// plain mipmapped texture the device fills level by level through its scratch
	// (a render target cannot carry levels here).
	GSTexture* tex = (levels <= 1) ?
	                     g_gs_device->CreateRenderTarget(tw, th, GSTexture::Format::Color, false, true) :
	                     g_gs_device->CreateTexture(tw, th, static_cast<int>(levels), GSTexture::Format::Color, true);
	if (!tex)
		return nullptr;

	for (u32 l = 0; l < std::max<u32>(levels, 1); l++)
	{
		if (!g_gs_device->TileExpandPalette(index, palette, tex, l, l))
		{
			// A refusal is treated as permanent — the dominant cause is a device
			// that will never serve it — so draws stop paying a failed attempt
			// each; the fallback (in-shader expansion) is correct either way.
			g_gs_device->Recycle(tex);
			m_serves = false;
			return nullptr;
		}
		m_passes++;
	}

	found->tex = tex;
	found->last_use = ++m_use_counter;
	m_builds++;
	return tex;
}

GSTileExpandedCache::Admission GSTileExpandedCache::ProbeAdmit(u64 index_id, u64 palette_id)
{
	// The scan Lookup runs, with the build legs removed. A pair already carrying a
	// texture is a hit and therefore served; a pair whose marker predates this frame is
	// what Lookup would build now, which is also served from the caller's viewpoint.
	Entry* found = nullptr;
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
		if (e.index_id == index_id && e.palette_id == palette_id)
		{
			found = &e;
			break;
		}
		if (!lru || e.last_use < lru->last_use)
			lru = &e;
	}

	if (!found)
	{
		Entry& e = free_slot ? *free_slot : *lru;
		if (e.tex)
		{
			g_gs_device->Recycle(e.tex);
			e.tex = nullptr;
		}
		e.alive = true;
		e.index_id = index_id;
		e.palette_id = palette_id;
		e.last_use = ++m_use_counter;
		e.seen_frame = m_frame;
		return Admission::Deferred;
	}

	found->last_use = ++m_use_counter;
	return (found->tex == nullptr && found->seen_frame == m_frame) ? Admission::Deferred : Admission::Served;
}

void GSTileExpandedCache::Clear()
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
