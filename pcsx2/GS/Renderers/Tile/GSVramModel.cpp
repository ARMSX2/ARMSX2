// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSVramModel.h"

#include "GS/GSLocalMemory.h"

#include "common/Assertions.h"

#include <algorithm>

GSVramModel::GSVramModel()
{
	Reset();
}

void GSVramModel::Reset()
{
	for (PlaneState& ps : m_planes)
	{
		ps.truth.clear();
		ps.synced.clear();
		ps.partial.clear();
		ps.masks.clear();
		ps.owner.fill(kGSTileNoSurface);
	}
	m_gen.fill(PageGen{});
	m_gpu_epoch.fill(0);
	m_surfaces.clear();
	m_free_ids.clear();
	m_by_layout.clear();
	m_live_surfaces = 0;
	m_stats = Stats{};
}

// -- Surfaces ---------------------------------------------------------------------

GSTileSurfaceId GSVramModel::Create(const GSTileSurfaceLayout& layout, const GSVector4i& rect, u32 pool_handle)
{
	pxAssert(FindExact(layout) == kGSTileNoSurface);

	GSTileSurfaceId id;
	if (!m_free_ids.empty())
	{
		id = m_free_ids.back();
		m_free_ids.pop_back();
	}
	else
	{
		pxAssert(m_surfaces.size() < kGSTileNoSurface);
		id = static_cast<GSTileSurfaceId>(m_surfaces.size());
		m_surfaces.emplace_back();
	}

	Surface& s = m_surfaces[id];
	s.layout = layout;
	s.pool_handle = pool_handle;
	s.alive = true;
	s.residency = PagesForRect(layout, rect);
	for (GSPageBitmap& v : s.valid)
		v.clear();

	m_by_layout.emplace(layout.pack(), id);
	m_live_surfaces++;
	return id;
}

void GSVramModel::Destroy(GSTileSurfaceId id)
{
	pxAssert(id < m_surfaces.size() && m_surfaces[id].alive);
	Surface& s = m_surfaces[id];

	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		PlaneState& ps = m_planes[pi];
		// Dropping the GPU copy is lossless only when the CPU bytes are equal.
		pxAssert(ps.synced.contains(s.valid[pi]));
		s.valid[pi].forEachSetPage([&ps](u32 page) { ps.owner[page] = kGSTileNoSurface; });
		(s.valid[pi] & ps.partial).forEachSetPage([&ps](u32 page) { ps.masks.erase(static_cast<u16>(page)); });
		ps.truth = ps.truth.andnot(s.valid[pi]);
		ps.synced = ps.synced.andnot(s.valid[pi]);
		ps.partial = ps.partial.andnot(s.valid[pi]);
	}

	m_by_layout.erase(s.layout.pack());
	s = Surface{};
	m_free_ids.push_back(id);
	m_live_surfaces--;
}

GSTileSurfaceId GSVramModel::FindExact(const GSTileSurfaceLayout& layout) const
{
	const auto it = m_by_layout.find(layout.pack());
	return (it != m_by_layout.end()) ? it->second : kGSTileNoSurface;
}

void GSVramModel::GrowResidency(GSTileSurfaceId id, const GSPageBitmap& pages)
{
	pxAssert(id < m_surfaces.size() && m_surfaces[id].alive);
	m_surfaces[id].residency |= pages;
}

const GSVramModel::Surface& GSVramModel::Get(GSTileSurfaceId id) const
{
	pxAssert(id < m_surfaces.size() && m_surfaces[id].alive);
	return m_surfaces[id];
}

// -- Pure footprint helpers ---------------------------------------------------------

GSPageBitmap GSVramModel::PagesForRect(const GSTileSurfaceLayout& layout, const GSVector4i& rect)
{
	GSPageBitmap pages;
	if (rect.rempty())
		return pages;
	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	pages.addPages(off.pageLooperForRect(rect));
	return pages;
}

namespace
{
void AddEdgeBlock(GSVramModel::RectFootprint& out, u32 page, u32 block_bit, bool full)
{
	for (u32 i = 0; i < out.edge_count; i++)
	{
		if (out.edges[i].page == page)
		{
			out.edges[i].blocks |= u32(1) << block_bit;
			if (full)
				out.edges[i].full |= u32(1) << block_bit;
			return;
		}
	}
	if (out.edge_count == GSVramModel::RectFootprint::kMaxEdges)
	{
		out.overflowed = true;
		return;
	}
	GSVramModel::RectFootprint::Edge& e = out.edges[out.edge_count++];
	e.page = static_cast<u16>(page);
	e.blocks = u32(1) << block_bit;
	e.full = full ? (u32(1) << block_bit) : 0;
}

void AccumulateStrip(GSVramModel::RectFootprint& out, const GSOffset& off, const GSVector2i& bs,
	const GSVector4i& strip, const GSVector4i& rect, const GSPageBitmap& interior_pages)
{
	if (strip.rempty())
		return;
	const int bx0 = (strip.left / bs.x) * bs.x;
	const int by0 = (strip.top / bs.y) * bs.y;
	for (int by = by0; by < strip.bottom; by += bs.y)
	{
		for (int bx = bx0; bx < strip.right; bx += bs.x)
		{
			// Full means fully inside the whole write rect, not the strip: a strip
			// block adjoining the interior is covered by the rect even though the
			// strip only sees its rim.
			const bool full = bx >= rect.left && (bx + bs.x) <= rect.right &&
							  by >= rect.top && (by + bs.y) <= rect.bottom;
			const u32 bn = off.bn(bx, by);
			const u32 page = bn >> 5;
			// A footprint big enough to wrap GS memory can revisit a page one wrap
			// pass proved fully covered; an edge entry would then understate the
			// coverage and let a shrink leave stale truth standing.
			if (interior_pages.test(page))
				continue;
			out.pages.set(page);
			AddEdgeBlock(out, page, bn & 31, full);
		}
	}
}
} // namespace

void GSVramModel::FootprintForRect(const GSTileSurfaceLayout& layout, const GSVector4i& rect, RectFootprint& out)
{
	out.pages.clear();
	out.edge_count = 0;
	out.overflowed = false;
	if (rect.rempty())
		return;

	const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
	// Geometry from the offset's own shifts, not GSLocalMemory::m_psm — the latter is
	// runtime-initialized and this helper must stay pure (unit tests run without any
	// GSLocalMemory instance).
	const GSVector2i pgs(1 << off.pageShiftX(), 1 << off.pageShiftY());
	const GSVector2i bs(1 << off.blockShiftX(), 1 << off.blockShiftY());

	// The page grid lives in absolute pixel space (bp shifts which page index a cell
	// maps to, never where the cell boundaries are), so the fully-covered interior is
	// the rect with each edge rounded inward to a page boundary.
	//
	// That equivalence between pixel cells and physical pages holds only for
	// page-aligned bases. A block-aligned bp (transfers: BITBLTBUF's base is block-
	// granular, unlike FRAME/ZBUF) makes every cell straddle two physical pages, so
	// the interior shortcut would mislabel boundary pages as fully covered — those
	// footprints take the exact per-block path for the whole rect instead.
	const bool page_aligned_bp = (layout.bp & 31) == 0;
	const int in_left = ((rect.left + pgs.x - 1) / pgs.x) * pgs.x;
	const int in_top = ((rect.top + pgs.y - 1) / pgs.y) * pgs.y;
	const int in_right = (rect.right / pgs.x) * pgs.x;
	const int in_bottom = (rect.bottom / pgs.y) * pgs.y;

	if (page_aligned_bp && in_left < in_right && in_top < in_bottom)
	{
		const GSVector4i interior(in_left, in_top, in_right, in_bottom);
		GSPageBitmap interior_pages;
		interior_pages.addPages(off.pageLooperForRect(interior));
		out.pages |= interior_pages;

		AccumulateStrip(out, off, bs, GSVector4i(rect.left, rect.top, rect.right, in_top), rect, interior_pages);
		AccumulateStrip(out, off, bs, GSVector4i(rect.left, in_bottom, rect.right, rect.bottom), rect, interior_pages);
		AccumulateStrip(out, off, bs, GSVector4i(rect.left, in_top, in_left, in_bottom), rect, interior_pages);
		AccumulateStrip(out, off, bs, GSVector4i(in_right, in_top, rect.right, in_bottom), rect, interior_pages);
	}
	else
	{
		AccumulateStrip(out, off, bs, rect, rect, GSPageBitmap());
	}
}

// -- Flows ----------------------------------------------------------------------------

GSPageBitmap GSVramModel::SpillBeforeCpuWrite(const RectFootprint& fp, u8 planes) const
{
	GSPageBitmap need;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		if (!(planes & (1u << pi)))
			continue;
		const PlaneState& ps = m_planes[pi];
		const GSPageBitmap unsynced_truth = (fp.pages & ps.truth).andnot(ps.synced);
		if (fp.overflowed)
		{
			// Some edge pages lost their masks: every unsynced truth page in the
			// footprint might have a sub-block overlap. Maximal spill, always safe.
			need |= unsynced_truth;
			continue;
		}
		unsynced_truth.forEachSetPage([&](u32 page) {
			const RectFootprint::Edge* e = fp.FindEdge(page);
			if (!e)
				return; // interior: whole page overwritten, nothing to preserve
			const u32 partial_cover = e->blocks & ~e->full;
			u32 mask = kFullBlockMask;
			if (ps.partial.test(page))
				mask = ps.masks.at(static_cast<u16>(page));
			if (partial_cover & mask)
				need.set(page);
		});
	}
	return need;
}

void GSVramModel::OnCpuWrite(const RectFootprint& fp, u8 planes)
{
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		if (!(planes & (1u << pi)))
			continue;
		PlaneState& ps = m_planes[pi];
		const GSPageBitmap affected = fp.pages & ps.truth;
		affected.forEachSetPage([&](u32 page) {
			const RectFootprint::Edge* e = fp.overflowed ? nullptr : fp.FindEdge(page);
			if (!e)
			{
				if (fp.overflowed)
				{
					// Unknown coverage: only lossless if the spill above ran.
					pxAssert(ps.synced.test(page));
				}
				ClearTruthPage(ps, page, pi);
				return;
			}
			const u32 mask = ps.partial.test(page) ? ps.masks.at(static_cast<u16>(page)) : kFullBlockMask;
			const u32 partial_cover = e->blocks & ~e->full;
			if (partial_cover & mask)
			{
				// Sub-block overlap with live truth: the caller must have synced this
				// page (SpillBeforeCpuWrite listed it), making a full clear lossless.
				pxAssert(ps.synced.test(page));
				ClearTruthPage(ps, page, pi);
				return;
			}
			ShrinkTruthPage(ps, page, e->blocks, pi);
		});
	}
	fp.pages.forEachSetPage([this](u32 page) { m_gen[page].cpu_write++; });
}

void GSVramModel::OnCpuWrite(const GSPageBitmap& pages, u8 planes)
{
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		if (!(planes & (1u << pi)))
			continue;
		PlaneState& ps = m_planes[pi];
		(pages & ps.truth).forEachSetPage([&](u32 page) {
			// This overload cannot see block coverage, so a lossless clear needs the
			// page synced (the floor-draw flow reads back its whole footprint before
			// rasterizing). Partial writers with coverage go through the rect
			// overload, which can shrink instead.
			pxAssert(ps.synced.test(page));
			ClearTruthPage(ps, page, pi);
		});
	}
	pages.forEachSetPage([this](u32 page) { m_gen[page].cpu_write++; });
}

GSPageBitmap GSVramModel::ReadbackNeeded(const RectFootprint& fp, u8 planes)
{
	GSPageBitmap need;
	GSPageBitmap page_granular;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		if (!(planes & (1u << pi)))
			continue;
		const PlaneState& ps = m_planes[pi];
		const GSPageBitmap cand = (fp.pages & ps.truth).andnot(ps.synced);
		page_granular |= cand;
		cand.forEachSetPage([&](u32 page) {
			if (!fp.overflowed && ps.partial.test(page))
			{
				const RectFootprint::Edge* e = fp.FindEdge(page);
				if (e && (e->blocks & ps.masks.at(static_cast<u16>(page))) == 0)
				{
					// The read touches only blocks whose newest data is already on
					// the CPU: page-granular tracking would have pulled this page.
					return;
				}
			}
			need.set(page);
		});
	}
	// The risk-register signal: pages a page-granular model would have spilled that
	// block refinement proved clean. Counted per page (one avoided pull), not per
	// plane.
	m_stats.conflicts_refuted_by_blocks += page_granular.andnot(need).count();
	return need;
}

GSPageBitmap GSVramModel::ReadbackNeeded(const GSPageBitmap& pages, u8 planes) const
{
	GSPageBitmap need;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		if (!(planes & (1u << pi)))
			continue;
		const PlaneState& ps = m_planes[pi];
		need |= (pages & ps.truth).andnot(ps.synced);
	}
	return need;
}

GSTileSurfaceId GSVramModel::SoleGpuOwner(const GSPageBitmap& pages, u8 planes) const
{
	GSTileSurfaceId sole = kGSTileNoSurface;
	bool split = false;
	pages.forEachSetPage([&](u32 page) {
		if (split)
			return;
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(planes & (1u << pi)))
				continue;
			// No owner means the CPU holds this plane's newest bytes, so no GPU texture
			// carries them; a shrunk mask means only part of the page is GPU-newest.
			// Either way there is no single authoritative texture for the window.
			const GSTileSurfaceId owner = m_planes[pi].owner[page];
			if (owner == kGSTileNoSurface || TruthMask(page, pi) != kFullBlockMask)
			{
				split = true;
				return;
			}
			if (sole == kGSTileNoSurface)
				sole = owner;
			else if (owner != sole)
			{
				split = true;
				return;
			}
		}
	});
	return split ? kGSTileNoSurface : sole;
}

void GSVramModel::OnReadback(const GSPageBitmap& pages)
{
	for (PlaneState& ps : m_planes)
		ps.synced |= pages & ps.truth;
}

GSPageBitmap GSVramModel::SpillBeforeNativeDraw(GSTileSurfaceId id, const GSPageBitmap& pages, u8 planes) const
{
	pxAssert(id < m_surfaces.size() && m_surfaces[id].alive);
	GSPageBitmap need;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		if (!(planes & (1u << pi)))
			continue;
		const PlaneState& ps = m_planes[pi];
		need |= ((pages & ps.truth).andnot(ps.synced)).andnot(m_surfaces[id].valid[pi]);
	}
	return need;
}

void GSVramModel::OnNativeDraw(GSTileSurfaceId id, const GSPageBitmap& pages, u8 planes)
{
	pxAssert(id < m_surfaces.size() && m_surfaces[id].alive);
	Surface& s = m_surfaces[id];
	pxAssert(s.residency.contains(pages));

	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		if (!(planes & (1u << pi)))
			continue;
		PlaneState& ps = m_planes[pi];

		const GSPageBitmap steal = (pages & ps.truth).andnot(s.valid[pi]);
		pxAssert(ps.synced.contains(steal));
		steal.forEachSetPage([&](u32 page) {
			const GSTileSurfaceId prev = ps.owner[page];
			pxAssert(prev != kGSTileNoSurface && prev != id);
			m_surfaces[prev].valid[pi].unset(page);
		});

		pages.forEachSetPage([&](u32 page) { ps.owner[page] = id; });
		// A mask exists only for pages in `partial` (the invariant CheckInvariants
		// enforces), so erase only where one can exist — the unguarded form was up
		// to ~8 x footprint no-op hash erases per draw.
		(pages & ps.partial).forEachSetPage([&](u32 page) { ps.masks.erase(static_cast<u16>(page)); });
		ps.partial = ps.partial.andnot(pages);
		s.valid[pi] |= pages;
		ps.truth |= pages;
		ps.synced = ps.synced.andnot(pages);
	}
	pages.forEachSetPage([this](u32 page) { m_gen[page].gpu_write++; });
}

void GSVramModel::StampGpuWrite(const GSPageBitmap& pages, u64 epoch)
{
	pages.forEachSetPage([this, epoch](u32 page) { m_gpu_epoch[page] = std::max(m_gpu_epoch[page], epoch); });
}

u64 GSVramModel::GpuEpochOf(const GSPageBitmap& pages) const
{
	u64 e = 0;
	pages.forEachSetPage([this, &e](u32 page) { e = std::max(e, m_gpu_epoch[page]); });
	return e;
}

// -- Queries -----------------------------------------------------------------------

GSPageBitmap GSVramModel::TruthAny() const
{
	return (m_planes[0].truth | m_planes[1].truth) | (m_planes[2].truth | m_planes[3].truth);
}

u32 GSVramModel::TruthMask(u32 page, u32 plane_idx) const
{
	const PlaneState& ps = m_planes[plane_idx];
	if (!ps.truth.test(page))
		return 0;
	if (ps.partial.test(page))
		return ps.masks.at(static_cast<u16>(page));
	return kFullBlockMask;
}

bool GSVramModel::CheckInvariants() const
{
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		const PlaneState& ps = m_planes[pi];

		GSPageBitmap union_valid;
		for (u32 id = 0; id < m_surfaces.size(); id++)
		{
			const Surface& s = m_surfaces[id];
			if (!s.alive)
				continue;
			if (union_valid.intersects(s.valid[pi]))
				return false; // two owners for one page
			if (!s.residency.contains(s.valid[pi]))
				return false;
			union_valid |= s.valid[pi];

			bool owner_ok = true;
			s.valid[pi].forEachSetPage([&](u32 page) {
				if (ps.owner[page] != id)
					owner_ok = false;
			});
			if (!owner_ok)
				return false;
		}
		if (union_valid != ps.truth)
			return false;
		if (!ps.truth.contains(ps.synced))
			return false;
		if (!ps.truth.contains(ps.partial))
			return false;

		for (u32 page = 0; page < GS_MAX_PAGES; page++)
		{
			if (!ps.truth.test(page) && ps.owner[page] != kGSTileNoSurface)
				return false;
			const bool has_mask = ps.masks.find(static_cast<u16>(page)) != ps.masks.end();
			if (has_mask != ps.partial.test(page))
				return false;
			if (has_mask)
			{
				const u32 mask = ps.masks.at(static_cast<u16>(page));
				if (mask == 0 || mask == kFullBlockMask)
					return false;
			}
		}
	}
	return true;
}

// -- Internals ---------------------------------------------------------------------

void GSVramModel::ClearTruthPage(PlaneState& ps, u32 page, u32 plane_idx)
{
	const GSTileSurfaceId o = ps.owner[page];
	if (o != kGSTileNoSurface)
		m_surfaces[o].valid[plane_idx].unset(page);
	ps.owner[page] = kGSTileNoSurface;
	ps.truth.unset(page);
	ps.synced.unset(page);
	if (ps.partial.test(page))
	{
		ps.partial.unset(page);
		ps.masks.erase(static_cast<u16>(page));
	}
}

void GSVramModel::ShrinkTruthPage(PlaneState& ps, u32 page, u32 written_blocks, u32 plane_idx)
{
	const u32 cur = ps.partial.test(page) ? ps.masks.at(static_cast<u16>(page)) : kFullBlockMask;
	const u32 next = cur & ~written_blocks;
	if (next == 0)
	{
		ClearTruthPage(ps, page, plane_idx);
		return;
	}
	if (next == cur)
		return;
	ps.masks[static_cast<u16>(page)] = next;
	ps.partial.set(page);
	// The synced claim survives a shrink: it now asserts equality only over the
	// remaining truth blocks, whose CPU bytes the write did not touch.
}
