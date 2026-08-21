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

// The slot index's exact-key find. One entry per pair — every insertion below happens only after a
// miss — so the first match is the only match, which is what the walk it replaced relied on when
// it broke early.
u16 GSTileExpandedCache::FindEntry(u64 hash, u64 index_id, u64 palette_id) const
{
	return m_index.Find(hash, [&](u16 i) {
		const Entry& e = m_entries[i];
		return e.index_id == index_id && e.palette_id == palette_id;
	});
}

// First sight: record the pair. The marker shares the LRU pool with real entries — it is one-shot
// by construction when the pair never repeats, so it ages out first (stable entries are re-touched
// every frame). Taking a slot can recycle the texture in it, so this is under the pin discipline
// exactly as a build is, and refuses the same way when the whole cache is held down.
u16 GSTileExpandedCache::AdmitSlot(u64 hash, u64 index_id, u64 palette_id)
{
	u16 fresh = m_index.FreeSlot();
	const bool from_free = (fresh != SlotIndex::kNil);
	if (!from_free)
		fresh = m_index.Victim([this](u16 i) { return !Pinned(m_entries[i]); });
	if (fresh == SlotIndex::kNil)
	{
		m_capacity_refusals++;
		return SlotIndex::kNil;
	}

	Entry& e = m_entries[fresh];
	if (e.tex)
	{
		g_gs_device->Recycle(e.tex);
		e.tex = nullptr;
	}
	if (!from_free)
	{
		// The evicted pair stops matching before the new one is filed, so the index never carries
		// two slots for one pair.
		m_index.Release(fresh);
	}
	e.alive = true;
	e.index_id = index_id;
	e.palette_id = palette_id;
	e.last_use = ++m_use_counter;
	m_index.Occupy(fresh, hash);
	e.pinned_frame = 0; // a marker names no texture, so nothing is holding it down yet
	e.seen_frame = m_frame;
	return fresh;
}

GSTexture* GSTileExpandedCache::Lookup(GSTexture* index, u64 index_id, GSTexture* palette, u64 palette_id, u32 levels)
{
	if (!m_serves)
		return nullptr;

	const u64 hash = GSTileSlotHash(index_id, palette_id);
	const u16 slot = FindEntry(hash, index_id, palette_id);
	Entry* const found = (slot != SlotIndex::kNil) ? &m_entries[slot] : nullptr;

	if (found && found->tex)
	{
		found->last_use = ++m_use_counter;
		m_index.Touch(slot);
		// An expansion a recorded-but-unissued draw names may not be recycled into the pool,
		// where the next CreateTexture would take it and overwrite it. Inert with the pin
		// discipline off, which is Tile.
		found->pinned_frame = m_pin_frame;
		m_hits++;
		return found->tex;
	}

	if (!found)
	{
		if (AdmitSlot(hash, index_id, palette_id) == SlotIndex::kNil)
			return nullptr;
		m_deferrals++;
		return nullptr;
	}

	if (found->seen_frame == m_frame)
	{
		// Re-seen, but within the marker's own frame: a window with several draws
		// this frame proves nothing about the next one. Keep deferring — building
		// here is what turned per-frame-volatile windows into a build every frame.
		found->last_use = ++m_use_counter;
		m_index.Touch(slot);
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
	m_index.Touch(slot);
	found->pinned_frame = m_pin_frame;
	m_builds++;
	return tex;
}

// Lookup's admission filter, with the expansion draw replaced by the builder's arrangement of one.
// Everything the immediate road decides is decided identically here -- the split is only WHEN the
// texels arrive, which is the caller's problem and not this class's.
GSTexture* GSTileExpandedCache::LookupBuilt(GSTexture* index, u64 index_id, GSTexture* palette,
	u64 palette_id, GSTileExpandBuilder& builder, AdmitWhen when, BuiltOutcome* outcome)
{
	const auto done = [&](BuiltOutcome o, GSTexture* tex) -> GSTexture* {
		if (outcome)
			*outcome = o;
		return tex;
	};
	const bool admit_now = (when == AdmitWhen::FirstSight);

	const u64 hash = GSTileSlotHash(index_id, palette_id);
	u16 slot = FindEntry(hash, index_id, palette_id);
	Entry* found = (slot != SlotIndex::kNil) ? &m_entries[slot] : nullptr;

	if (found && found->tex)
	{
		found->last_use = ++m_use_counter;
		m_index.Touch(slot);
		found->pinned_frame = m_pin_frame;
		m_hits++;
		return done(BuiltOutcome::Hit, found->tex);
	}

	if (!found)
	{
		slot = AdmitSlot(hash, index_id, palette_id);
		if (slot == SlotIndex::kNil)
			return done(BuiltOutcome::RefusedCapacity, nullptr);
		if (!admit_now)
		{
			m_deferrals++;
			return done(BuiltOutcome::Deferred, nullptr);
		}
		found = &m_entries[slot];
	}
	else if (found->seen_frame == m_frame && !admit_now)
	{
		found->last_use = ++m_use_counter;
		m_index.Touch(slot);
		m_deferrals++;
		return done(BuiltOutcome::Deferred, nullptr);
	}

	// The pair survived a frame boundary, so it earns its expansion. Level 0 only on this road.
	GSTexture* tex =
		g_gs_device->CreateRenderTarget(index->GetWidth(), index->GetHeight(), GSTexture::Format::Color, false, true);
	if (!tex)
		return done(BuiltOutcome::Failed, nullptr);
	if (!builder.BuildTileExpansion(index, palette, tex))
	{
		// Not a device refusal: a builder that could not take the op says nothing about whether the
		// device can expand, so Serves() is left alone and the pair keeps its marker for next frame.
		g_gs_device->Recycle(tex);
		return done(BuiltOutcome::Failed, nullptr);
	}
	m_passes++;

	found->tex = tex;
	found->last_use = ++m_use_counter;
	m_index.Touch(slot);
	found->pinned_frame = m_pin_frame;
	m_builds++;
	return done(BuiltOutcome::Built, tex);
}

GSTileExpandedCache::Admission GSTileExpandedCache::ProbeAdmit(u64 index_id, u64 palette_id, AdmitWhen when)
{
	const bool admit_now = (when == AdmitWhen::FirstSight);
	// The lookup Lookup runs, with the build legs removed. A pair already carrying a texture is a
	// hit and therefore served; a pair whose marker predates this frame is what Lookup would build
	// now, which is also served from the caller's viewpoint.
	const u64 hash = GSTileSlotHash(index_id, palette_id);
	const u16 slot = FindEntry(hash, index_id, palette_id);

	if (slot == SlotIndex::kNil)
	{
		// Recording a marker takes a slot, and taking a slot can recycle the texture in it — so
		// this leg is under the pin discipline exactly as Lookup's is, and refuses the same way
		// when the whole cache is held down.
		if (AdmitSlot(hash, index_id, palette_id) == SlotIndex::kNil)
			return Admission::Deferred;
		return admit_now ? Admission::Served : Admission::Deferred;
	}

	Entry& found = m_entries[slot];
	found.last_use = ++m_use_counter;
	m_index.Touch(slot);
	if (found.tex)
		found.pinned_frame = m_pin_frame;
	if (found.tex == nullptr && found.seen_frame == m_frame)
		return admit_now ? Admission::Served : Admission::Deferred;
	return Admission::Served;
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
		e.pinned_frame = 0;
	}
	m_use_counter = 0;
	// Every slot free, no pairs filed, empty recency list — the entries above are all dead, and the
	// index is only ever a mirror of them.
	m_index.Clear();
}
