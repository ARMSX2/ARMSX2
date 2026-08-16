// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Tile/GSTileTypes.h"

#include <array>
#include <unordered_map>
#include <vector>

// The Tile renderer's interval memory model. Ground truth stays where it always was:
// CPU local memory (m_vm8) is maintained by transfers and by the SW floor. What this
// class tracks, exactly, is which of the 512 GS pages currently have their newest
// data on the GPU instead — in which surface, and for which planes — so that
//   - a floor draw or CPU readback knows the exact page set it must spill first,
//   - a native draw knows what to upload and what it may steal,
//   - any two footprints can be proved independent in one bitmap intersect.
//
// Pure component: no GSTexture, no device. Surfaces carry an opaque pool handle for
// the target pool; everything here runs in ctest against a naive per-page reference.
//
// Per plane, each page is in one of three states, encoded by two bitmaps:
//   CPU-newest:  not in truth. The CPU bytes are authoritative (the initial state).
//   GPU-newest:  in truth, not in synced. The owner surface's texture holds the
//                newest bytes; the CPU copy is stale.
//   Synced:      in truth and in synced. GPU and CPU hold equal bytes (after a
//                readback-for-read); the owner surface's validity is retained.
//
// Truth is page-granular from the draw side by construction: a native draw uploads
// its destination pages before rendering into them, so after the draw the whole page
// is current in the texture. The block-mask sidecar exists for the CPU-write side
// only: a transfer that partially covers a truth page shrinks that page's truth to
// the untouched blocks instead of forcing a readback, and readback/conflict probes
// refine their answers through the mask (each refutation is counted — that counter
// is the risk register's "page-granular false aliasing" early signal).
//
// Invariants (Devel-asserted via CheckInvariants, pinned by the unit suite):
//   - per plane, surface valid bitmaps are pairwise disjoint and truth is their OR;
//   - per plane, owner[page] != none exactly for truth pages, and matches valid;
//   - synced is a subset of truth; partial is a subset of truth; a partial page's
//     mask is never 0 and never full.
class GSVramModel
{
public:
	static constexpr u32 kBlocksPerPage = 32; // 8 KB page / 256 B block, every format
	static constexpr u32 kFullBlockMask = 0xFFFFFFFFu;

	struct Surface
	{
		GSTileSurfaceLayout layout = {};
		u32 pool_handle = 0; // opaque target-pool key; 0 in pure/unit-test use
		bool alive = false;
		GSPageBitmap residency; // pages this surface's texture materializes
		std::array<GSPageBitmap, kGSTilePlaneCount> valid; // newest-data pages per plane
	};

	// Per-page generations, bumped on every write from either side. Consumers arrive
	// with memoization (M3) and the pass graph (M5); maintained from the start so the
	// draw key can rely on them being honest.
	struct PageGen
	{
		u32 cpu_write = 0;
		u32 gpu_write = 0;
	};

	// The page set (and per-page block coverage) of one rectangle under one layout.
	// Edge pages are the pages the rect covers only partially: `blocks` holds every
	// block the rect touches, `full` the subset it covers completely — their
	// difference is the sub-block overlap that decides whether truth can shrink
	// in place or must be spilled first. Capacity-bounded: on overflow the flows
	// fall back to maximal spill + whole-page invalidation (always safe).
	struct RectFootprint
	{
		static constexpr u32 kMaxEdges = 256;

		GSPageBitmap pages;
		u32 edge_count = 0;
		bool overflowed = false;
		struct Edge
		{
			u16 page;
			u32 blocks;
			u32 full;
		};
		std::array<Edge, kMaxEdges> edges;

		const Edge* FindEdge(u32 page) const
		{
			for (u32 i = 0; i < edge_count; i++)
			{
				if (edges[i].page == page)
					return &edges[i];
			}
			return nullptr;
		}
	};

	GSVramModel();

	/// Full invalidate: every page CPU-newest, no surfaces. The Defrost path.
	void Reset();

	// -- Surfaces ---------------------------------------------------------------

	GSTileSurfaceId Create(const GSTileSurfaceLayout& layout, const GSVector4i& rect, u32 pool_handle);
	/// Requires every valid page be synced (spill first) — Devel-asserted; the pages
	/// fall back to CPU-newest, which is lossless exactly because they were synced.
	void Destroy(GSTileSurfaceId id);
	GSTileSurfaceId FindExact(const GSTileSurfaceLayout& layout) const;
	void GrowResidency(GSTileSurfaceId id, const GSPageBitmap& pages);

	const Surface& Get(GSTileSurfaceId id) const;
	u32 LiveSurfaces() const { return m_live_surfaces; }

	// -- Pure footprint helpers ---------------------------------------------------

	/// Page set of rect under layout (page-granular, no edge refinement).
	static GSPageBitmap PagesForRect(const GSTileSurfaceLayout& layout, const GSVector4i& rect);
	/// Page set plus block masks for the partially covered edge pages.
	static void FootprintForRect(const GSTileSurfaceLayout& layout, const GSVector4i& rect, RectFootprint& out);

	// -- Flows ----------------------------------------------------------------------
	// Rect-taking entry points refine page edges through the block sidecar; the
	// bitmap-taking ones are the conservative page-granular fallbacks.

	/// Pages whose truth blocks the rect only PARTIALLY covers (sub-block overlap):
	/// the caller must read these back (they become synced) before OnCpuWrite, or the
	/// unwritten bytes of those blocks would be lost. Whole-page and whole-block
	/// overwrites never appear here — the common transfer stays spill-free.
	GSPageBitmap SpillBeforeCpuWrite(const RectFootprint& fp, u8 planes) const;

	/// A CPU-side write (transfer or floor draw) landed on these pages: shrink or
	/// clear GPU truth. Edge pages shrink block-wise through the sidecar; interior
	/// pages clear outright. Any remaining sub-block overlap must have been synced
	/// via SpillBeforeCpuWrite (Devel-asserted).
	void OnCpuWrite(const RectFootprint& fp, u8 planes);
	void OnCpuWrite(const GSPageBitmap& pages, u8 planes);

	/// Pages a CPU read of fp must pull from the GPU first: GPU-newest pages whose
	/// truth blocks intersect the read. Partial-page refutations are counted.
	GSPageBitmap ReadbackNeeded(const RectFootprint& fp, u8 planes);
	GSPageBitmap ReadbackNeeded(const GSPageBitmap& pages, u8 planes) const;

	/// The pull happened: those pages' CPU bytes now equal the GPU bytes.
	void OnReadback(const GSPageBitmap& pages);

	/// Truth pages in the footprint owned by OTHER surfaces and not yet synced: the
	/// caller must read them back before drawing natively into id (the steal is then
	/// lossless). Self-owned pages never appear — a target redraws itself freely.
	GSPageBitmap SpillBeforeNativeDraw(GSTileSurfaceId id, const GSPageBitmap& pages, u8 planes) const;

	/// A native draw wrote these full pages of surface id (the destination was
	/// uploaded-current before the draw, so truth is whole-page). Steals pages from
	/// other owners — which must be synced (Devel-asserted).
	void OnNativeDraw(GSTileSurfaceId id, const GSPageBitmap& pages, u8 planes);

	/// The GPU work that last wrote these pages belongs to submission `epoch` — an
	/// opaque, monotonic value the caller takes from the device (the fence counter of
	/// the command buffer the draw was recorded into). Kept beside truth rather than
	/// folded into it, because "is the newest data on the GPU" and "has the GPU
	/// FINISHED producing it" are different questions with different prices: a page
	/// whose epoch the device has already retired can be pulled without draining
	/// whatever is being recorded now, and a page whose epoch IS the recording buffer
	/// cannot. Stamped by the renderer right after OnNativeDraw; the model never reads
	/// it, it only carries it. 0 = never written natively (or a device with no epochs).
	void StampGpuWrite(const GSPageBitmap& pages, u64 epoch);
	/// Highest epoch over the set — the submission every one of these pages waits on.
	u64 GpuEpochOf(const GSPageBitmap& pages) const;
	u64 GpuEpoch(u32 page) const { return m_gpu_epoch[page]; }

	/// The ONE live surface holding GPU-newest truth for every page of `pages` on every
	/// plane in `planes`, at whole-page block granularity — or kGSTileNoSurface when the
	/// window is split across owners, when any plane of any page is CPU-newest, or when
	/// any page's truth has been shrunk to a block subset.
	///
	/// Deliberately all-or-nothing. The caller is asking "can I read these bytes off the
	/// GPU instead of draining them to the CPU", and that needs a single texture which is
	/// authoritative for the WHOLE window: a window half of which is CPU-newest is not
	/// one, and neither is a window spanning two targets. Answering "mostly" would mean
	/// stitching sources, and a stitch that gets one page wrong is exactly the silent
	/// wrong-output this model exists to make impossible.
	GSTileSurfaceId SoleGpuOwner(const GSPageBitmap& pages, u8 planes) const;

	// -- Queries -------------------------------------------------------------------

	static constexpr u32 PlaneIndex(GSTilePlane plane) { return std::countr_zero(static_cast<u32>(plane)); }

	const GSPageBitmap& Truth(u32 plane_idx) const { return m_planes[plane_idx].truth; }
	const GSPageBitmap& SyncedPages(u32 plane_idx) const { return m_planes[plane_idx].synced; }
	GSPageBitmap TruthAny() const;
	GSTileSurfaceId OwnerOf(u32 page, u32 plane_idx) const { return m_planes[plane_idx].owner[page]; }
	/// 0 when the page has no truth; kFullBlockMask when fully truth.
	u32 TruthMask(u32 page, u32 plane_idx) const;
	const PageGen& Gen(u32 page) const { return m_gen[page]; }

	struct Stats
	{
		u64 conflicts_refuted_by_blocks = 0;
		u64 pages_spilled_for_cpu_write = 0;
		u64 pages_spilled_for_steal = 0;
	};
	const Stats& GetStats() const { return m_stats; }

	/// Devel/test consistency check of every invariant in the class comment.
	bool CheckInvariants() const;

private:
	struct PlaneState
	{
		GSPageBitmap truth;
		GSPageBitmap synced;
		GSPageBitmap partial; // truth pages with a (non-full) block mask
		std::unordered_map<u16, u32> masks; // page -> truth block mask, present iff partial
		std::array<GSTileSurfaceId, GS_MAX_PAGES> owner;
	};

	void ClearTruthPage(PlaneState& ps, u32 page, u32 plane_idx);
	void ShrinkTruthPage(PlaneState& ps, u32 page, u32 written_blocks, u32 plane_idx);

	std::array<PlaneState, kGSTilePlaneCount> m_planes;
	std::array<PageGen, GS_MAX_PAGES> m_gen;
	std::array<u64, GS_MAX_PAGES> m_gpu_epoch{};
	std::vector<Surface> m_surfaces;
	std::vector<GSTileSurfaceId> m_free_ids;
	std::unordered_map<u32, GSTileSurfaceId> m_by_layout; // layout.pack() -> id
	u32 m_live_surfaces = 0;
	Stats m_stats;
};
