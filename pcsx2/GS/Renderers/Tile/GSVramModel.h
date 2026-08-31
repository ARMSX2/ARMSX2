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
// A second bitmap, `shadow`, records the same equality from the CPU shadow's side only,
// because `synced` cannot: four of the five things that set it are bookkeeping fictions a
// renderer makes to keep the steal invariant satisfied without moving a byte, they go
// stale at a submit, and the bitmap cannot tell them apart — so a per-frame byte ring has
// to drop the whole thing when it submits. `shadow` is set by OnShadowFilled alone, which
// means the CPU really has the bytes, and ClearSynced/ClearAllSynced leave it alone.
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
//   - synced is a subset of truth; shadow is a subset of truth; partial is a subset
//     of truth; a partial page's mask is never 0 and never full.
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
	/// Ids are dense slots [0, SurfaceSlots()); a slot may be dead (Get(id).alive false).
	u32 SurfaceSlots() const { return static_cast<u32>(m_surfaces.size()); }

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
	///
	/// `ignore_synced` answers the same question with the synced bit left out, exactly as
	/// ReadbackNeeded's does and for the same reason: that is what the ordinary form returns on
	/// the far side of a byte-store flush (the flush drops every synced claim), and a superset of
	/// what it returns on this side. So a caller can learn on THIS side what the set would be over
	/// there, without flushing to find out.
	GSPageBitmap SpillBeforeCpuWrite(const RectFootprint& fp, u8 planes, bool ignore_synced = false) const;

	/// A CPU-side write (transfer or floor draw) landed on these pages: shrink or
	/// clear GPU truth. Edge pages shrink block-wise through the sidecar; interior
	/// pages clear outright. Any remaining sub-block overlap must have been synced
	/// via SpillBeforeCpuWrite (Devel-asserted).
	void OnCpuWrite(const RectFootprint& fp, u8 planes);
	void OnCpuWrite(const GSPageBitmap& pages, u8 planes);

	/// A CPU-side write landed on these pages and its bytes have ALREADY been given to the surface
	/// that holds them — so no truth moves and nothing is lost, only the write generation advances.
	/// TileGpu's upload merge is the one caller: it stages the transfer's bytes into the byte store,
	/// merges the surface's remaining bytes in on the GPU and reads the whole page back into the
	/// surface's texture, so the surface still holds the page's newest bytes when the write is done.
	/// Calling OnCpuWrite for such a page instead would clear truth the CPU shadow does not have.
	void OnCpuWriteServedOnGpu(const GSPageBitmap& pages);

	/// Pages a CPU read of fp must pull from the GPU first: GPU-newest pages whose
	/// truth blocks intersect the read. Partial-page refutations are counted.
	///
	/// `ignore_synced` answers the same question with the synced bit left out. That is
	/// what the ordinary form returns on the far side of a byte-store flush -- a renderer
	/// whose store is a per-frame ring drops every synced claim when it submits -- and it is
	/// a superset of what the ordinary form returns on this side. So an EMPTY answer in this
	/// mode proves the pull is empty whether the flush happens or not, which is the one thing
	/// a caller can decide before flushing. It is a GATE only: never the pull set (a flush that
	/// finds an empty plan returns without clearing anything, and then the ordinary question
	/// still honours the claims and returns less).
	GSPageBitmap ReadbackNeeded(const RectFootprint& fp, u8 planes, bool ignore_synced = false);
	GSPageBitmap ReadbackNeeded(const GSPageBitmap& pages, u8 planes) const;

	/// The pull happened: those pages' CPU bytes now equal the GPU bytes.
	void OnReadback(const GSPageBitmap& pages);

	/// Forget that these pages were synced (truth and ownership untouched): the copy the
	/// synced bit vouched for no longer exists. The Tile renderer never needs this — its
	/// synced copy is CPU local memory, which persists — but a renderer whose byte store is
	/// a per-frame ring (TileGpu) drops every synced claim at the frame boundary and any page's
	/// claim when its ring slot is superseded mid-frame, so the next reader composes it again.
	void ClearSynced(const GSPageBitmap& pages);
	void ClearAllSynced();

	/// The pull's OWN receipt: these pages' bytes are in the CPU shadow — and unlike the synced
	/// claim, ClearSynced and ClearAllSynced do not drop it.
	///
	/// Both bits say "GPU and CPU hold equal bytes", so the reason there are two is worth stating.
	/// `synced` is set from five places that mean five different things: the ring holds the page,
	/// these bytes are dead, these bytes are dropped, the seed will overwrite them — and, once,
	/// the CPU shadow really has them. The first four are fictions that keep the steal invariant
	/// satisfied without moving a byte, they all go stale at a submit, and nothing in the bitmap
	/// distinguishes them, so a renderer whose byte store is a per-frame ring is right to drop the
	/// lot when it submits. The fifth is not a fiction and does not go stale at a submit: the CPU
	/// shadow keeps its bytes exactly as CPU local memory does.
	///
	/// So a CLUT readback that flushes the plan on its way in stops destroying its own previous
	/// pull's receipt before asking whether it has to pull again. Retired by OnNativeDraw, in the
	/// same statement that clears `synced`, and by anything that takes truth off the page — which
	/// is sound because OnNativeDraw is the one place GPU truth is created.
	void OnShadowFilled(const GSPageBitmap& pages);

	/// Of `pages`, the ones whose bytes must actually come down: a page whose shadow claim still
	/// stands on every plane that holds GPU truth for it is already in the CPU shadow, byte for
	/// byte, and pulling it again fetches what is there.
	///
	/// Deliberately NOT folded into ReadbackNeeded. ReadbackNeeded answers the caller's ASK, and a
	/// caller spends that answer on more than the transfer — TileGpu stamps its stall census and
	/// its upload merge's "this page was CPU-read recently" mark from it, and the merge is the one
	/// thing this claim must not move. The ask stays whole; only the download narrows.
	GSPageBitmap ShadowMissing(const GSPageBitmap& pages) const;

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

	/// Why SoleGpuOwner refused, for a caller sizing what a narrower question would
	/// serve. Three causes share one refusal today and they are not equally hard: one
	/// is a stitch across GPU and CPU bytes, one needs a record naming two sources, and
	/// the third is only the question being asked in whole pages when the caller cares
	/// about blocks.
	enum class SoleOwnerRefusal : u8
	{
		kServed = 0, ///< it did not refuse
		kBlockSubset, ///< one owner everywhere; some page's truth is a block subset
		kNoOwner, ///< some plane of some page is entirely CPU-newest (or the window is empty)
		kTwoOwners, ///< two live surfaces across the window
	};

	/// SoleGpuOwner, and why. Same answer; `why` also decides how far the scan runs,
	/// because a cause is a property of the WHOLE window and the first obstruction in
	/// page order is only the first one. Without it the scan stops there, which is what
	/// the texture-read path wants over a window of hundreds of pages.
	GSTileSurfaceId SoleGpuOwner(const GSPageBitmap& pages, u8 planes, SoleOwnerRefusal& why) const;

	/// SoleGpuOwner asked of the footprint's BLOCKS instead of whole pages: the one live
	/// surface holding GPU-newest truth for every block `fp` names, on every plane in
	/// `planes`.
	///
	/// Same all-or-nothing rule, and for the same reason — this is still one texture that
	/// has to be authoritative for everything the caller will read. What moves is the
	/// unit. A caller reading four blocks out of a page does not care that the page's
	/// other twenty-eight went back to the CPU, and the whole-page question makes it
	/// drain a page whose bytes it never touches. An overflowed footprint carries no
	/// block detail, so it falls back to the whole-page question.
	GSTileSurfaceId SoleGpuOwnerOfBlocks(const RectFootprint& fp, u8 planes) const;
	/// ...and why it refused, on the same terms as the page-granular overload. kBlockSubset here
	/// means the footprint's OWN blocks are not all one owner's GPU truth, which is the case no
	/// narrowing of the unit can reach.
	GSTileSurfaceId SoleGpuOwnerOfBlocks(const RectFootprint& fp, u8 planes, SoleOwnerRefusal& why) const;

	// -- Queries -------------------------------------------------------------------

	static constexpr u32 PlaneIndex(GSTilePlane plane) { return std::countr_zero(static_cast<u32>(plane)); }

	const GSPageBitmap& Truth(u32 plane_idx) const { return m_planes[plane_idx].truth; }
	const GSPageBitmap& SyncedPages(u32 plane_idx) const { return m_planes[plane_idx].synced; }
	const GSPageBitmap& ShadowPages(u32 plane_idx) const { return m_planes[plane_idx].shadow; }
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
		GSPageBitmap shadow; // synced, but claimed by the CPU shadow alone: ClearSynced spares it
		GSPageBitmap partial; // truth pages with a (non-full) block mask
		std::unordered_map<u16, u32> masks; // page -> truth block mask, present iff partial
		std::array<GSTileSurfaceId, GS_MAX_PAGES> owner;
	};

	void ClearTruthPage(PlaneState& ps, u32 page, u32 plane_idx);
	void ShrinkTruthPage(PlaneState& ps, u32 page, u32 written_blocks, u32 plane_idx);
	/// The one spelling of the sole-owner scan, for all three entry points above. `fp`
	/// narrows the truth each page must hold from every block down to the ones the
	/// footprint names; `why` turns the early exit off and classifies the refusal.
	GSTileSurfaceId SoleGpuOwnerScan(
		const GSPageBitmap& pages, u8 planes, const RectFootprint* fp, SoleOwnerRefusal* why) const;

	std::array<PlaneState, kGSTilePlaneCount> m_planes;
	std::array<PageGen, GS_MAX_PAGES> m_gen;
	std::array<u64, GS_MAX_PAGES> m_gpu_epoch{};
	std::vector<Surface> m_surfaces;
	std::vector<GSTileSurfaceId> m_free_ids;
	std::unordered_map<u32, GSTileSurfaceId> m_by_layout; // layout.pack() -> id
	u32 m_live_surfaces = 0;
	Stats m_stats;
};
