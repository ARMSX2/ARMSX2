// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSVramModel pinned against a naive per-page/per-block reference model. The
// reference derives every footprint by brute-force block iteration (no interior/
// strip decomposition) and applies the flow semantics with per-block bookkeeping —
// independent logic for the same contract. Directed cases cover the alias tiers,
// the steal/spill/shrink corners and the sidecar refutation; the randomized sweep
// drives interleaved surfaces, native draws, CPU writes and readbacks through both
// models and compares every observable after every op.

#include "GS/Renderers/Tile/GSVramModel.h"

#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <random>
#include <vector>

namespace
{
constexpr GSTileSurfaceLayout Layout(u32 bp, u8 bw, u8 psm, GSTileSurfaceKind kind = GSTileSurfaceKind::Color)
{
	return GSTileSurfaceLayout{bp, bw, psm, kind};
}

// Brute-force footprint: every block whose cell intersects the rect, full when the
// rect covers the whole cell. The independent reference for FootprintForRect.
struct RefFootprint
{
	std::map<u32, std::pair<u32, u32>> per_page; // page -> (touched, full)

	static RefFootprint For(const GSTileSurfaceLayout& layout, const GSVector4i& rect)
	{
		RefFootprint ref;
		if (rect.rempty())
			return ref;
		const GSOffset off = GSOffset::fromKnownPSM(layout.bp, layout.bw, static_cast<GS_PSM>(layout.psm));
		// Block dims from the offset's shifts, like the code under test: the m_psm
		// table is runtime-initialized and absent in a unit-test process.
		const GSVector2i bs(1 << off.blockShiftX(), 1 << off.blockShiftY());
		for (int by = (rect.top / bs.y) * bs.y; by < rect.bottom; by += bs.y)
		{
			for (int bx = (rect.left / bs.x) * bs.x; bx < rect.right; bx += bs.x)
			{
				const bool full = bx >= rect.left && (bx + bs.x) <= rect.right &&
								  by >= rect.top && (by + bs.y) <= rect.bottom;
				const u32 bn = off.bn(bx, by);
				auto& e = ref.per_page[bn >> 5];
				e.first |= u32(1) << (bn & 31);
				if (full)
					e.second |= u32(1) << (bn & 31);
			}
		}
		return ref;
	}
};

// The naive model: per plane, per page, an owner + truth block mask + synced flag,
// with flow decisions taken per block.
struct NaiveModel
{
	struct Page
	{
		GSTileSurfaceId owner = kGSTileNoSurface;
		u32 mask = 0;
		bool synced = false;
	};
	std::array<std::array<Page, GS_MAX_PAGES>, kGSTilePlaneCount> planes;

	GSPageBitmap SpillBeforeCpuWrite(const RefFootprint& fp, u8 pl) const
	{
		GSPageBitmap need;
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(pl & (1u << pi)))
				continue;
			for (const auto& [page, cover] : fp.per_page)
			{
				const Page& p = planes[pi][page];
				if (p.owner == kGSTileNoSurface || p.synced)
					continue;
				if ((cover.first & ~cover.second) & p.mask)
					need.set(page);
			}
		}
		return need;
	}

	void OnCpuWrite(const RefFootprint& fp, u8 pl)
	{
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(pl & (1u << pi)))
				continue;
			for (const auto& [page, cover] : fp.per_page)
			{
				Page& p = planes[pi][page];
				if (p.owner == kGSTileNoSurface)
					continue;
				if ((cover.first & ~cover.second) & p.mask)
				{
					// sub-block overlap: the flow spilled it, clearing is lossless
					p = Page{};
					continue;
				}
				p.mask &= ~cover.first;
				if (p.mask == 0)
					p = Page{};
			}
		}
	}

	GSPageBitmap ReadbackNeeded(const RefFootprint& fp, u8 pl) const
	{
		GSPageBitmap need;
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(pl & (1u << pi)))
				continue;
			for (const auto& [page, cover] : fp.per_page)
			{
				const Page& p = planes[pi][page];
				if (p.owner != kGSTileNoSurface && !p.synced && (cover.first & p.mask))
					need.set(page);
			}
		}
		return need;
	}

	void OnReadback(const GSPageBitmap& pages)
	{
		for (auto& plane : planes)
		{
			pages.forEachSetPage([&plane](u32 page) {
				if (plane[page].owner != kGSTileNoSurface)
					plane[page].synced = true;
			});
		}
	}

	GSPageBitmap SpillBeforeNativeDraw(GSTileSurfaceId id, const GSPageBitmap& pages, u8 pl) const
	{
		GSPageBitmap need;
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(pl & (1u << pi)))
				continue;
			pages.forEachSetPage([&](u32 page) {
				const Page& p = planes[pi][page];
				if (p.owner != kGSTileNoSurface && p.owner != id && !p.synced)
					need.set(page);
			});
		}
		return need;
	}

	void OnNativeDraw(GSTileSurfaceId id, const GSPageBitmap& pages, u8 pl)
	{
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(pl & (1u << pi)))
				continue;
			pages.forEachSetPage([&](u32 page) {
				planes[pi][page] = Page{id, GSVramModel::kFullBlockMask, false};
			});
		}
	}

	void Destroy(GSTileSurfaceId id)
	{
		for (auto& plane : planes)
		{
			for (Page& p : plane)
			{
				if (p.owner == id)
					p = Page{};
			}
		}
	}
};

void ExpectModelsMatch(const GSVramModel& fast, const NaiveModel& naive, int tag)
{
	ASSERT_TRUE(fast.CheckInvariants()) << "op " << tag;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		for (u32 page = 0; page < GS_MAX_PAGES; page++)
		{
			const NaiveModel::Page& n = naive.planes[pi][page];
			ASSERT_EQ(fast.TruthMask(page, pi), n.mask) << "op " << tag << " plane " << pi << " page " << page;
			ASSERT_EQ(fast.OwnerOf(page, pi), n.owner) << "op " << tag << " plane " << pi << " page " << page;
			const bool fast_synced = fast.SyncedPages(pi).test(page);
			ASSERT_EQ(fast_synced, n.owner != kGSTileNoSurface && n.synced)
				<< "op " << tag << " plane " << pi << " page " << page;
		}
	}
}

RefFootprint RefFor(const GSTileSurfaceLayout& l, const GSVector4i& r)
{
	return RefFootprint::For(l, r);
}
} // namespace

TEST(GSTileTypes, AliasTierClassification)
{
	const auto ct32 = Layout(0, 8, PSMCT32);
	// Same family (CT24 shares CT32 pages), same stride, page-aligned: a view.
	EXPECT_EQ(gsTileClassifyAlias(ct32, Layout(32, 8, PSMCT24)), GSTileAliasTier::OffsetView);
	EXPECT_EQ(gsTileClassifyAlias(ct32, Layout(0, 8, PSMT8H)), GSTileAliasTier::OffsetView);
	// Same family, different stride: rows land on different pages, remap.
	EXPECT_EQ(gsTileClassifyAlias(ct32, Layout(0, 16, PSMCT32)), GSTileAliasTier::PageRemap);
	// Block-offset base: sub-page start, no view or remap can express it.
	EXPECT_EQ(gsTileClassifyAlias(ct32, Layout(1, 8, PSMCT32)), GSTileAliasTier::Spill);
	// Same storage width, different family: a conversion.
	EXPECT_EQ(gsTileClassifyAlias(ct32, Layout(0, 8, PSMZ32, GSTileSurfaceKind::Depth)), GSTileAliasTier::Convert);
	EXPECT_EQ(gsTileClassifyAlias(Layout(0, 8, PSMCT16), Layout(0, 8, PSMCT16S)), GSTileAliasTier::Convert);
	EXPECT_EQ(gsTileClassifyAlias(Layout(0, 8, PSMZ16, GSTileSurfaceKind::Depth), Layout(0, 8, PSMCT16)), GSTileAliasTier::Convert);
	// Different storage widths: only a spill is correct.
	EXPECT_EQ(gsTileClassifyAlias(ct32, Layout(0, 8, PSMCT16)), GSTileAliasTier::Spill);
	EXPECT_EQ(gsTileClassifyAlias(Layout(0, 8, PSMT8), Layout(0, 8, PSMT4)), GSTileAliasTier::Spill);
}

TEST(GSTileTypes, PlaneMapping)
{
	EXPECT_EQ(gsTilePlanesFromChannelMask(0xF), kGSTilePlanesColor);
	EXPECT_EQ(gsTilePlanesFromChannelMask(0x7), GSTilePlaneRGB); // 24-bit formats
	EXPECT_EQ(gsTilePlanesFromChannelMask(0x8), kGSTilePlanesAlpha); // T8H/T4HL/T4HH
	EXPECT_EQ(GSVramModel::PlaneIndex(GSTilePlaneZ), 3u);
}

// A native color draw takes the Z plane with whatever channels it writes, and Z spans
// the whole 32-bit cell -- so the surface must already hold every color channel, not
// just the written ones, or the claim republishes bytes the GPU never had. Gran Turismo
// 4 measured this as 27.8% of the frame: an alpha-only sprite rendered into a surface
// whose RGB was two draws stale, and the whole cell came back on the Z-plane readback.
TEST(GSTileTypes, ClaimsSpanWholeCellWheneverZRidesAlong)
{
	// The lowering's own shape: any color write claims Z for byte coverage.
	EXPECT_EQ(gsTileColorPlanesSpannedBy(kGSTilePlanesAlpha | GSTilePlaneZ), kGSTilePlanesColor);
	EXPECT_EQ(gsTileColorPlanesSpannedBy(GSTilePlaneRGB | GSTilePlaneZ), kGSTilePlanesColor);
	EXPECT_EQ(gsTileColorPlanesSpannedBy(kGSTilePlanesAll), kGSTilePlanesColor);

	// Without the Z claim there is no widening: a plane spans only its own bytes.
	EXPECT_EQ(gsTileColorPlanesSpannedBy(kGSTilePlanesAlpha), kGSTilePlanesAlpha);
	EXPECT_EQ(gsTileColorPlanesSpannedBy(GSTilePlaneRGB), GSTilePlaneRGB);
	EXPECT_EQ(gsTileColorPlanesSpannedBy(0), 0);

	// Z alone still spans the cell: the answer is about bytes, not about which plane
	// carried the claim.
	EXPECT_EQ(gsTileColorPlanesSpannedBy(GSTilePlaneZ), kGSTilePlanesColor);
}

TEST(GSVramModel, FootprintMatchesBruteForce)
{
	std::mt19937_64 rng(0x67766f51); // fixed seed: deterministic suite
	constexpr u32 kPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMZ32, PSMZ16};
	std::uniform_int_distribution<u32> psm_dist(0, std::size(kPsms) - 1);
	std::uniform_int_distribution<u32> bp_dist(0, GS_MAX_BLOCKS - 1);
	std::uniform_int_distribution<u32> bw_dist(1, 32);
	std::uniform_int_distribution<int> pos_dist(0, 1023);
	std::uniform_int_distribution<int> size_dist(1, 512);

	GSVramModel::RectFootprint fp;
	for (int iter = 0; iter < 1500; iter++)
	{
		const u8 psm = static_cast<u8>(kPsms[psm_dist(rng)]);
		// 8- and 4-bit formats have 128-wide pages, so their TBW is even on real
		// hardware (bw 1 would mean a zero page stride, where PageLooper's
		// monotonic-row fast path is legitimately inexact).
		u8 bw = static_cast<u8>(bw_dist(rng));
		if (psm == PSMT8 || psm == PSMT4)
			bw = static_cast<u8>((bw + 1) & ~1);
		const GSTileSurfaceLayout l = Layout(bp_dist(rng), bw, psm);
		const int left = pos_dist(rng);
		const int top = pos_dist(rng);
		const GSVector4i rect(left, top, left + size_dist(rng), top + size_dist(rng));

		GSVramModel::FootprintForRect(l, rect, fp);
		ASSERT_FALSE(fp.overflowed) << "iter " << iter;

		// Same pages as the page-granular helper (which is PageLooper-backed).
		const GSPageBitmap looper_pages = GSVramModel::PagesForRect(l, rect);
		if (!(fp.pages == looper_pages))
		{
			std::string only_fp, only_looper;
			fp.pages.andnot(looper_pages).forEachSetPage([&](u32 p) { only_fp += std::to_string(p) + " "; });
			looper_pages.andnot(fp.pages).forEachSetPage([&](u32 p) { only_looper += std::to_string(p) + " "; });
			FAIL() << "iter " << iter << " bp " << l.bp << " bw " << int(l.bw) << " psm " << int(l.psm)
				   << " rect " << rect.left << "," << rect.top << "," << rect.right << "," << rect.bottom
				   << " | only in block-path: " << only_fp << " | only in looper: " << only_looper;
		}

		const RefFootprint ref = RefFor(l, rect);
		for (const auto& [page, cover] : ref.per_page)
		{
			ASSERT_TRUE(fp.pages.test(page)) << "iter " << iter << " page " << page;
			const GSVramModel::RectFootprint::Edge* e = fp.FindEdge(page);
			if (e)
			{
				ASSERT_EQ(e->blocks, cover.first) << "iter " << iter << " page " << page;
				ASSERT_EQ(e->full, cover.second) << "iter " << iter << " page " << page;
			}
			else
			{
				// No edge entry means the whole page is covered: every block present
				// and fully inside the rect.
				ASSERT_EQ(cover.first, GSVramModel::kFullBlockMask) << "iter " << iter << " page " << page;
				ASSERT_EQ(cover.second, GSVramModel::kFullBlockMask) << "iter " << iter << " page " << page;
			}
		}
	}
}

TEST(GSVramModel, SurfaceLifecycleAndLookup)
{
	GSVramModel m;
	const auto l1 = Layout(0, 8, PSMCT32);
	const auto l2 = Layout(0x1000, 8, PSMZ32, GSTileSurfaceKind::Depth);

	EXPECT_EQ(m.FindExact(l1), kGSTileNoSurface);
	const GSTileSurfaceId a = m.Create(l1, GSVector4i(0, 0, 512, 448), 1);
	const GSTileSurfaceId b = m.Create(l2, GSVector4i(0, 0, 512, 448), 2);
	EXPECT_NE(a, b);
	EXPECT_EQ(m.FindExact(l1), a);
	EXPECT_EQ(m.FindExact(l2), b);
	EXPECT_EQ(m.LiveSurfaces(), 2u);
	EXPECT_TRUE(m.CheckInvariants());

	m.Destroy(a);
	EXPECT_EQ(m.FindExact(l1), kGSTileNoSurface);
	EXPECT_EQ(m.LiveSurfaces(), 1u);
	const GSTileSurfaceId a2 = m.Create(l1, GSVector4i(0, 0, 64, 32), 3);
	EXPECT_EQ(a2, a); // slot reuse
	EXPECT_TRUE(m.CheckInvariants());
}

TEST(GSVramModel, NativeDrawReadbackFloorRoundTrip)
{
	GSVramModel m;
	const auto l = Layout(0, 8, PSMCT32);
	const GSVector4i rect(0, 0, 512, 448);
	const GSTileSurfaceId id = m.Create(l, rect, 1);
	const GSPageBitmap pages = GSVramModel::PagesForRect(l, rect);

	// Native draw: truth appears, CPU is stale.
	EXPECT_TRUE(m.SpillBeforeNativeDraw(id, pages, kGSTilePlanesColor).empty());
	m.OnNativeDraw(id, pages, kGSTilePlanesColor);
	EXPECT_TRUE(m.CheckInvariants());
	EXPECT_EQ(m.Truth(0).count(), pages.count());
	EXPECT_EQ(m.TruthMask(0, 0), GSVramModel::kFullBlockMask);
	EXPECT_EQ(m.Gen(0).gpu_write, 1u);

	// A CPU read of the region needs everything pulled; after the pull, nothing.
	EXPECT_EQ(m.ReadbackNeeded(pages, kGSTilePlanesColor) == pages, true);
	m.OnReadback(pages);
	EXPECT_TRUE(m.ReadbackNeeded(pages, kGSTilePlanesColor).empty());
	EXPECT_TRUE(m.CheckInvariants());

	// Floor draw over the region (already synced): truth falls back to the CPU.
	m.OnCpuWrite(pages, kGSTilePlanesColor);
	EXPECT_TRUE(m.Truth(0).empty());
	EXPECT_TRUE(m.TruthAny().empty());
	EXPECT_EQ(m.Gen(0).cpu_write, 1u);
	EXPECT_TRUE(m.CheckInvariants());

	// Synced pages can be dropped losslessly on destroy.
	m.OnNativeDraw(id, pages, kGSTilePlanesColor);
	m.OnReadback(pages);
	m.Destroy(id);
	EXPECT_TRUE(m.TruthAny().empty());
	EXPECT_TRUE(m.CheckInvariants());
}

TEST(GSVramModel, StealBetweenAliasedSurfaces)
{
	GSVramModel m;
	// Two color views over the same pages (different stride: a PageRemap alias).
	const auto la = Layout(0, 8, PSMCT32);
	const auto lb = Layout(0, 16, PSMCT32);
	const GSVector4i ra(0, 0, 512, 448);
	const GSVector4i rb(0, 0, 1024, 224);
	const GSTileSurfaceId a = m.Create(la, ra, 1);
	const GSTileSurfaceId b = m.Create(lb, rb, 2);
	const GSPageBitmap pa = GSVramModel::PagesForRect(la, ra);

	m.OnNativeDraw(a, pa, kGSTilePlanesColor);

	// B wants to draw over A's truth: the steal set is exactly A's unsynced pages.
	GSPageBitmap steal = m.SpillBeforeNativeDraw(b, pa, kGSTilePlanesColor);
	EXPECT_EQ(steal == pa, true);
	m.OnReadback(steal);
	EXPECT_TRUE(m.SpillBeforeNativeDraw(b, pa, kGSTilePlanesColor).empty());

	m.OnNativeDraw(b, pa, kGSTilePlanesColor);
	EXPECT_TRUE(m.CheckInvariants());
	EXPECT_EQ(m.OwnerOf(0, 0), b);
	EXPECT_TRUE(m.Get(a).valid[0].empty());
	EXPECT_EQ(m.Get(b).valid[0].count(), pa.count());

	// A redraws itself: no spill against its own (stolen-away) history.
	EXPECT_TRUE(m.SpillBeforeNativeDraw(b, pa, kGSTilePlanesColor).empty());
}

TEST(GSVramModel, BlockShrinkAndSidecarRefutation)
{
	GSVramModel m;
	const auto l = Layout(0, 2, PSMCT32); // 2 pages per row, page = 64x32, block = 8x8
	const GSVector4i page0(0, 0, 64, 32);
	const GSTileSurfaceId id = m.Create(l, GSVector4i(0, 0, 128, 32), 1);
	m.OnNativeDraw(id, GSVramModel::PagesForRect(l, page0), kGSTilePlanesColor);

	// A block-aligned CPU write over the left half of the page: no sub-block
	// overlap, so no spill — truth shrinks in place.
	const GSVector4i left_half(0, 0, 32, 32);
	GSVramModel::RectFootprint fp;
	GSVramModel::FootprintForRect(l, left_half, fp);
	EXPECT_TRUE(m.SpillBeforeCpuWrite(fp, kGSTilePlanesColor).empty());
	m.OnCpuWrite(fp, kGSTilePlanesColor);
	EXPECT_TRUE(m.CheckInvariants());

	const u32 mask_after = m.TruthMask(0, 0);
	EXPECT_NE(mask_after, 0u);
	EXPECT_NE(mask_after, GSVramModel::kFullBlockMask);

	// A read of the just-written half is refuted by the sidecar (the CPU already
	// holds the newest bytes there); a read of the other half still needs the pull.
	const u64 refuted_before = m.GetStats().conflicts_refuted_by_blocks;
	GSVramModel::FootprintForRect(l, left_half, fp);
	EXPECT_TRUE(m.ReadbackNeeded(fp, kGSTilePlanesColor).empty());
	EXPECT_EQ(m.GetStats().conflicts_refuted_by_blocks, refuted_before + 1);

	GSVramModel::FootprintForRect(l, GSVector4i(32, 0, 64, 32), fp);
	EXPECT_FALSE(m.ReadbackNeeded(fp, kGSTilePlanesColor).empty());

	// A sub-block writer over live truth must spill first.
	const GSVector4i jagged(36, 4, 49, 23);
	GSVramModel::FootprintForRect(l, jagged, fp);
	const GSPageBitmap need = m.SpillBeforeCpuWrite(fp, kGSTilePlanesColor);
	EXPECT_FALSE(need.empty());
	m.OnReadback(need);
	m.OnCpuWrite(fp, kGSTilePlanesColor);
	EXPECT_TRUE(m.CheckInvariants());
	EXPECT_TRUE(m.Truth(0).empty()); // sub-block overlap clears the whole page
}

TEST(GSVramModel, OverflowedFootprintFallsBackToMaximalSpill)
{
	GSVramModel m;
	const auto l = Layout(0, 2, PSMCT32);
	const GSVector4i page0(0, 0, 64, 32);
	const GSTileSurfaceId id = m.Create(l, page0, 1);
	const GSPageBitmap pages = GSVramModel::PagesForRect(l, page0);
	m.OnNativeDraw(id, pages, kGSTilePlanesColor);

	// A hand-built overflowed footprint: coverage unknown, so the flow must demand
	// the maximal spill and then clear whole pages.
	GSVramModel::RectFootprint fp;
	fp.pages = pages;
	fp.edge_count = 0;
	fp.overflowed = true;

	const GSPageBitmap need = m.SpillBeforeCpuWrite(fp, kGSTilePlanesColor);
	EXPECT_EQ(need == pages, true);
	m.OnReadback(need);
	m.OnCpuWrite(fp, kGSTilePlanesColor);
	EXPECT_TRUE(m.Truth(0).empty());
	EXPECT_TRUE(m.CheckInvariants());
}

TEST(GSVramModel, RandomizedFlowsVsNaiveModel)
{
	std::mt19937_64 rng(0x67766f52);

	// A pool of deliberately overlapping layouts so aliasing and steals happen.
	const GSTileSurfaceLayout kLayouts[] = {
		Layout(0, 8, PSMCT32),
		Layout(0, 16, PSMCT32),
		Layout(0x600, 8, PSMCT24),
		Layout(0x600, 8, PSMCT16),
		Layout(0xC00, 8, PSMZ32, GSTileSurfaceKind::Depth),
		Layout(0xC00, 8, PSMZ16, GSTileSurfaceKind::Depth),
	};
	constexpr u32 kLayoutCount = std::size(kLayouts);

	auto planes_for = [](const GSTileSurfaceLayout& l) -> u8 {
		if (l.kind == GSTileSurfaceKind::Depth)
			return GSTilePlaneZ;
		if (l.psm == PSMCT24)
			return GSTilePlaneRGB;
		return kGSTilePlanesColor;
	};

	GSVramModel fast;
	NaiveModel naive;
	std::array<GSTileSurfaceId, kLayoutCount> ids;
	ids.fill(kGSTileNoSurface);

	std::uniform_int_distribution<u32> layout_dist(0, kLayoutCount - 1);
	std::uniform_int_distribution<u32> op_dist(0, 9);
	std::uniform_int_distribution<int> pos_dist(0, 767);
	std::uniform_int_distribution<int> size_dist(1, 320);

	GSVramModel::RectFootprint fp;
	for (int op = 0; op < 1500; op++)
	{
		const u32 li = layout_dist(rng);
		const GSTileSurfaceLayout& l = kLayouts[li];
		const u8 pl = planes_for(l);
		const int left = pos_dist(rng);
		const int top = pos_dist(rng);
		const GSVector4i rect(left, top, left + size_dist(rng), top + size_dist(rng));

		switch (op_dist(rng))
		{
			case 0: // create
			{
				if (ids[li] != kGSTileNoSurface)
					break;
				ids[li] = fast.Create(l, rect, li + 1);
				break;
			}
			case 1: // destroy (spill everything it owns first, as the renderer would)
			{
				if (ids[li] == kGSTileNoSurface)
					break;
				GSPageBitmap owned;
				for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
					owned |= fast.Get(ids[li]).valid[pi];
				fast.OnReadback(owned);
				naive.OnReadback(owned);
				fast.Destroy(ids[li]);
				naive.Destroy(ids[li]);
				ids[li] = kGSTileNoSurface;
				break;
			}
			case 2:
			case 3: // native draw flow
			{
				if (ids[li] == kGSTileNoSurface)
					break;
				const GSPageBitmap pages = GSVramModel::PagesForRect(l, rect);
				fast.GrowResidency(ids[li], pages);
				const GSPageBitmap need = fast.SpillBeforeNativeDraw(ids[li], pages, pl);
				ASSERT_EQ(need == naive.SpillBeforeNativeDraw(ids[li], pages, pl), true) << "op " << op;
				fast.OnReadback(need);
				naive.OnReadback(need);
				fast.OnNativeDraw(ids[li], pages, pl);
				naive.OnNativeDraw(ids[li], pages, pl);
				break;
			}
			case 4:
			case 5:
			case 6: // CPU write flow (transfer under this layout's format)
			{
				GSVramModel::FootprintForRect(l, rect, fp);
				ASSERT_FALSE(fp.overflowed) << "op " << op;
				const RefFootprint ref = RefFor(l, rect);
				const GSPageBitmap need = fast.SpillBeforeCpuWrite(fp, pl);
				ASSERT_EQ(need == naive.SpillBeforeCpuWrite(ref, pl), true) << "op " << op;
				fast.OnReadback(need);
				naive.OnReadback(need);
				fast.OnCpuWrite(fp, pl);
				naive.OnCpuWrite(ref, pl);
				break;
			}
			case 7:
			case 8: // CPU read probe, sometimes committed
			{
				GSVramModel::FootprintForRect(l, rect, fp);
				const RefFootprint ref = RefFor(l, rect);
				const GSPageBitmap need = fast.ReadbackNeeded(fp, pl);
				ASSERT_EQ(need == naive.ReadbackNeeded(ref, pl), true) << "op " << op;
				if (op & 1)
				{
					fast.OnReadback(need);
					naive.OnReadback(need);
				}
				break;
			}
			case 9: // floor-draw flow: spill whole footprint, then page-granular clear
			{
				const GSPageBitmap pages = GSVramModel::PagesForRect(l, rect);
				const GSPageBitmap need = fast.ReadbackNeeded(pages, pl);
				fast.OnReadback(need);
				naive.OnReadback(need);
				fast.OnCpuWrite(pages, pl);
				const RefFootprint whole = [&pages]() {
					RefFootprint r;
					pages.forEachSetPage([&r](u32 page) {
						r.per_page[page] = {GSVramModel::kFullBlockMask, GSVramModel::kFullBlockMask};
					});
					return r;
				}();
				naive.OnCpuWrite(whole, pl);
				break;
			}
		}

		ExpectModelsMatch(fast, naive, op);
		if (::testing::Test::HasFatalFailure())
			return;
	}
}
