// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSRendererTile.h"

#include "GS/GSLocalMemory.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/HW/GSDrawLog.h"

#include "common/Timer.h"

MULTI_ISA_UNSHARED_IMPL;

GSRenderer* CURRENT_ISA::makeGSRendererTile(int threads)
{
	return new GSRendererTile(threads);
}

namespace
{
GSTileSurfaceKind KindForPsm(u32 psm)
{
	switch (gsTileSwizzleFamily(psm))
	{
		case GSTileSwizzleFamily::Z32:
		case GSTileSwizzleFamily::Z16:
		case GSTileSwizzleFamily::Z16S:
			return GSTileSurfaceKind::Depth;
		default:
			return GSTileSurfaceKind::Color;
	}
}

// A zero stride makes every row of the footprint derivation degenerate; rather than
// under-claim (which would leave stale GPU truth believed newest), such a layout
// claims and spills the whole page space. The native route floors on it.
GSPageBitmap PagesForTargetRect(const GSTileSurfaceLayout& layout, const GSVector4i& r)
{
	if (layout.bw == 0)
	{
		GSPageBitmap all;
		for (u32 i = 0; i < GS_MAX_PAGES; i++)
			all.set(i);
		return all;
	}
	return GSVramModel::PagesForRect(layout, r);
}

// The bytes of a 32-bit cell one plane's data occupies under one layout — what a
// readback of that plane is allowed to write back into CPU local memory.
u32 PlaneByteMask(u32 plane_idx, const GSTileSurfaceLayout& layout)
{
	const u32 fmt_bytes = (layout.psm == PSMCT24 || layout.psm == PSMZ24) ? 0x00FFFFFFu : 0xFFFFFFFFu;
	u32 plane_bytes = 0;
	switch (1u << plane_idx)
	{
		case GSTilePlaneRGB:
			plane_bytes = 0x00FFFFFFu;
			break;
		case GSTilePlaneAlphaLow:
			plane_bytes = 0x0F000000u;
			break;
		case GSTilePlaneAlphaHigh:
			plane_bytes = 0xF0000000u;
			break;
		case GSTilePlaneZ:
			plane_bytes = 0xFFFFFFFFu;
			break;
		default:
			ASSUME(0);
	}
	return fmt_bytes & plane_bytes;
}

GSDrawLog::TileFallback MapFallbackReason(GSTileFloorReason reason)
{
	switch (reason)
	{
		case GSTileFloorReason::None:
			return GSDrawLog::TileFallbackNone;
		case GSTileFloorReason::PrimClass:
			return GSDrawLog::TileFallbackPrimClass;
		case GSTileFloorReason::SpriteExpandUnavailable:
			return GSDrawLog::TileFallbackSpriteExpand;
		case GSTileFloorReason::Textured:
			return GSDrawLog::TileFallbackTextured;
		case GSTileFloorReason::Blend:
			return GSDrawLog::TileFallbackBlend;
		case GSTileFloorReason::CoverageAA1:
			return GSDrawLog::TileFallbackCoverageAA1;
		case GSTileFloorReason::Fog:
			return GSDrawLog::TileFallbackFog;
		case GSTileFloorReason::Fba:
			return GSDrawLog::TileFallbackFba;
		case GSTileFloorReason::ScanMask:
			return GSDrawLog::TileFallbackScanMask;
		case GSTileFloorReason::AlphaTest:
			return GSDrawLog::TileFallbackAlphaTest;
		case GSTileFloorReason::DateTest:
			return GSDrawLog::TileFallbackDateTest;
		case GSTileFloorReason::ZTestNever:
			return GSDrawLog::TileFallbackZTestNever;
		case GSTileFloorReason::FrameMaskPartial:
			return GSDrawLog::TileFallbackFrameMaskPartial;
		case GSTileFloorReason::NothingWritten:
			return GSDrawLog::TileFallbackNothingWritten;
		case GSTileFloorReason::FramePsm:
			return GSDrawLog::TileFallbackFramePsm;
		case GSTileFloorReason::ZbufPsm:
			return GSDrawLog::TileFallbackZbufPsm;
		case GSTileFloorReason::StrideZero:
			return GSDrawLog::TileFallbackStrideZero;
		case GSTileFloorReason::FootprintExtent:
			return GSDrawLog::TileFallbackFootprintExtent;
		case GSTileFloorReason::FrameZOverlap:
			return GSDrawLog::TileFallbackFrameZOverlap;
		case GSTileFloorReason::ResourceFailure:
			return GSDrawLog::TileFallbackResourceFailure;
		default:
			return GSDrawLog::TileFallbackFloor;
	}
}
} // namespace

GSRendererTile::GSRendererTile(int threads)
	: GSRendererSW(threads)
{
}

void GSRendererTile::Destroy()
{
	m_target_pool.ReleaseAll();
	GSRendererSW::Destroy();
}

void GSRendererTile::Reset(bool hardware_reset)
{
	GSRendererSW::Reset(hardware_reset);
	// Everything falls back to CPU-newest; surface textures die with the reset.
	m_target_pool.ReleaseAll();
	m_vram_model.Reset();
}

void GSRendererTile::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// Present reads local memory, and syncing everything at the frame boundary keeps
	// stable targets synced (no truth is dropped, so nothing re-uploads next frame).
	// TODO(M3): spill only the DISPFB footprints via a GetOutput-precise hook.
	SyncAllTruthToCpu();
	// The model's invariants are cheap enough to police once a frame in Devel.
	pxAssert(m_vram_model.CheckInvariants());
	GSRendererSW::VSync(field, registers_written, idle_frame);
}

// A transfer wrote CPU local memory: shrink or clear GPU truth under it. The planes
// follow byte coverage (gsTilePlanesInvalidatedByWrite), and the rect footprint lets
// edge pages shrink block-wise instead of spilling. Only sub-block overlap forces a
// pull first — the common whole-block transfer stays readback-free.
void GSRendererTile::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	GSRendererSW::InvalidateVideoMem(BITBLTBUF, r);

	const GSTileSurfaceLayout layout{BITBLTBUF.DBP, static_cast<u8>(BITBLTBUF.DBW),
		static_cast<u8>(BITBLTBUF.DPSM), KindForPsm(BITBLTBUF.DPSM)};
	GSVramModel::FootprintForRect(layout, r, m_rect_fp);
	const u8 planes = gsTilePlanesInvalidatedByWrite(BITBLTBUF.DPSM);

	ReadbackModelPages(m_vram_model.SpillBeforeCpuWrite(m_rect_fp, planes));
	m_vram_model.OnCpuWrite(m_rect_fp, planes);
}

// The CPU is about to read this region out of local memory: any page whose newest
// bytes live on the GPU comes back first.
void GSRendererTile::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	const GSTileSurfaceLayout layout{BITBLTBUF.SBP, static_cast<u8>(BITBLTBUF.SBW),
		static_cast<u8>(BITBLTBUF.SPSM), KindForPsm(BITBLTBUF.SPSM)};
	GSVramModel::FootprintForRect(layout, r, m_rect_fp);

	ReadbackModelPages(m_vram_model.ReadbackNeeded(m_rect_fp, kGSTilePlanesAll));

	GSRendererSW::InvalidateLocalMem(BITBLTBUF, r, clut);
}

// A local->local copy is a CPU read of the source and a CPU write of the destination
// as far as the model is concerned (the floor executes it against CPU truth), so both
// sides pull before the base runs.
void GSRendererTile::Move()
{
	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;
	const GSVector4i src_r(m_env.TRXPOS.SSAX, m_env.TRXPOS.SSAY, m_env.TRXPOS.SSAX + w, m_env.TRXPOS.SSAY + h);
	const GSVector4i dst_r(m_env.TRXPOS.DSAX, m_env.TRXPOS.DSAY, m_env.TRXPOS.DSAX + w, m_env.TRXPOS.DSAY + h);

	const GSTileSurfaceLayout src_l{m_env.BITBLTBUF.SBP, static_cast<u8>(m_env.BITBLTBUF.SBW),
		static_cast<u8>(m_env.BITBLTBUF.SPSM), KindForPsm(m_env.BITBLTBUF.SPSM)};
	GSVramModel::FootprintForRect(src_l, src_r, m_rect_fp);
	GSPageBitmap need = m_vram_model.ReadbackNeeded(m_rect_fp, kGSTilePlanesAll);

	const GSTileSurfaceLayout dst_l{m_env.BITBLTBUF.DBP, static_cast<u8>(m_env.BITBLTBUF.DBW),
		static_cast<u8>(m_env.BITBLTBUF.DPSM), KindForPsm(m_env.BITBLTBUF.DPSM)};
	GSVramModel::FootprintForRect(dst_l, dst_r, m_rect_fp);
	const u8 planes = gsTilePlanesInvalidatedByWrite(m_env.BITBLTBUF.DPSM);
	need |= m_vram_model.SpillBeforeCpuWrite(m_rect_fp, planes);

	ReadbackModelPages(need);

	GSRendererSW::Move();

	m_vram_model.OnCpuWrite(m_rect_fp, planes);
}

// The SW rasterizer's own bound derivation (bbox rounding by prim class, AA1
// expansion, exclusive edges), intersected with the scissor. This is the write bound
// of the floor and the drawarea of the native route.
GSVector4i GSRendererTile::ComputeDrawRect() const
{
	GSVector4i bbox;
	if (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_POINT_CLASS)
		bbox = GSVector4i((m_vt.m_min.p + GSVector4(0.5f)).floor().upld((m_vt.m_max.p + GSVector4(0.5f)).floor()));
	else
		bbox = GSVector4i(m_vt.m_min.p.ceil().upld(m_vt.m_max.p.floor()));
	if (PRIM->AA1 && (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_TRIANGLE_CLASS))
		bbox += GSVector4i(-1, -1, 1, 1);
	bbox += GSVector4i(0, 0, 1, 1);

	return bbox.rintersect(m_context->scissor.in);
}

GSTileDrawPlan GSRendererTile::LowerCurrentDraw()
{
	GSTileDrawInput in = {};
	in.prim_class = m_vt.m_primclass;
	in.TEST = m_context->TEST;
	in.FRAME = m_context->FRAME;
	in.ZBUF = m_context->ZBUF;
	in.scanmsk = static_cast<u8>(m_draw_env->SCANMSK.MSK);
	if (!PRIM->TME)
	{
		// Exact for untextured draws: the vertex trace's alpha range. The lowering
		// only consults it after the textured gate, so the textured default is moot.
		const GSVertexTrace::VertexAlpha& alpha = GetAlphaMinMax();
		in.alpha_min = static_cast<u8>(std::clamp(alpha.min, 0, 255));
		in.alpha_max = static_cast<u8>(std::clamp(alpha.max, 0, 255));
	}
	else
	{
		in.alpha_min = 0;
		in.alpha_max = 255;
	}
	in.tme = PRIM->TME;
	in.abe = PRIM->ABE;
	in.aa1 = PRIM->AA1;
	in.fge = PRIM->FGE;
	in.fba = m_context->FBA.FBA;
	in.vs_expand = g_gs_device->Features().vs_expand;
	return gsTileLowerDraw(in);
}

// -- Spill machinery -----------------------------------------------------------------

// Pull the newest bytes of these pages from the GPU into CPU local memory. Per page,
// per plane, whichever surface owns the unsynced truth supplies the bytes of that
// plane's byte window; a page whose planes split across surfaces is pulled from each
// owner under disjoint byte masks. Afterwards every truth plane on the pages is
// synced, and the SW texture cache is told its cached deswizzles went stale.
bool GSRendererTile::ReadbackModelPages(const GSPageBitmap& pages)
{
	if (pages.empty())
		return true;

	struct Bucket
	{
		GSTileSurfaceId id;
		u32 mask;
		GSPageBitmap pages;
	};
	// Tiny in practice: one or two owners per spill.
	Bucket buckets[8];
	u32 num_buckets = 0;
	bool overflowed = false;

	pages.forEachSetPage([&](u32 page) {
		// Gather this page's per-owner byte masks.
		GSTileSurfaceId ids[kGSTilePlaneCount];
		u32 masks[kGSTilePlaneCount];
		u32 n = 0;
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!m_vram_model.Truth(pi).test(page) || m_vram_model.SyncedPages(pi).test(page))
				continue;
			const GSTileSurfaceId owner = m_vram_model.OwnerOf(page, pi);
			pxAssert(owner != kGSTileNoSurface);
			const u32 mask = PlaneByteMask(pi, m_vram_model.Get(owner).layout);
			u32 j = 0;
			for (; j < n; j++)
			{
				if (ids[j] == owner)
				{
					masks[j] |= mask;
					break;
				}
			}
			if (j == n)
			{
				ids[n] = owner;
				masks[n] = mask;
				n++;
			}
		}
		for (u32 j = 0; j < n; j++)
		{
			u32 b = 0;
			for (; b < num_buckets; b++)
			{
				if (buckets[b].id == ids[j] && buckets[b].mask == masks[j])
					break;
			}
			if (b == num_buckets)
			{
				if (num_buckets == std::size(buckets))
				{
					overflowed = true;
					return;
				}
				buckets[num_buckets].id = ids[j];
				buckets[num_buckets].mask = masks[j];
				buckets[num_buckets].pages.clear();
				num_buckets++;
			}
			buckets[b].pages.set(page);
		}
	});

	if (overflowed)
	{
		// More distinct (owner, mask) combinations than the fixed table holds; fall
		// back to per-page pulls. Never observed in practice — counted implicitly by
		// being slow, correct by construction.
		bool ok = true;
		pages.forEachSetPage([&](u32 page) {
			GSPageBitmap one;
			one.set(page);
			for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
			{
				if (!m_vram_model.Truth(pi).test(page) || m_vram_model.SyncedPages(pi).test(page))
					continue;
				const GSTileSurfaceId owner = m_vram_model.OwnerOf(page, pi);
				const GSVramModel::Surface& surf = m_vram_model.Get(owner);
				ok &= m_target_pool.ReadbackPages(m_mem, surf.pool_handle, surf.layout, one,
					PlaneByteMask(pi, surf.layout));
				InvalidateSwTexCache(surf.layout, one);
			}
		});
		m_vram_model.OnReadback(pages);
		return ok;
	}

	bool ok = true;
	for (u32 b = 0; b < num_buckets; b++)
	{
		const GSVramModel::Surface& surf = m_vram_model.Get(buckets[b].id);
		ok &= m_target_pool.ReadbackPages(m_mem, surf.pool_handle, surf.layout, buckets[b].pages, buckets[b].mask);
		InvalidateSwTexCache(surf.layout, buckets[b].pages);
	}
	m_vram_model.OnReadback(pages);
	return ok;
}

void GSRendererTile::InvalidateSwTexCache(const GSTileSurfaceLayout& layout, const GSPageBitmap& pages)
{
	// Over-invalidation is safe (a stale flag only re-deswizzles); bound the touched
	// page rows and let the looper cover them.
	const GSTileTargetPool::Geometry g = GSTileTargetPool::GeometryFor(layout);
	const u32 base = (layout.bp >> 5) & (GS_MAX_PAGES - 1);
	int min_row = INT32_MAX;
	int max_row = -1;
	pages.forEachSetPage([&](u32 page) {
		const int row = static_cast<int>((page + GS_MAX_PAGES - base) & (GS_MAX_PAGES - 1)) / g.ppr;
		min_row = std::min(min_row, row);
		max_row = std::max(max_row, row);
	});
	if (max_row < 0)
		return;

	const GSVector4i rect(0, min_row * g.pgs.y, g.ppr * 64, (max_row + 1) * g.pgs.y);
	const GSOffset off = m_mem.GetOffset(layout.bp, layout.bw, layout.psm);
	m_tc->InvalidatePages(off.pageLooperForRect(rect), layout.psm);
}

void GSRendererTile::SyncAllTruthToCpu()
{
	GSPageBitmap need;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		need |= m_vram_model.Truth(pi).andnot(m_vram_model.SyncedPages(pi));
	ReadbackModelPages(need);
}

// The floor reads AND writes its fb/z footprints (read-modify-write rasterization)
// and reads its texture, all against CPU local memory: pull every plane under them.
// Over-pulling costs a readback; under-pulling costs bytes.
void GSRendererTile::SpillForFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r)
{
	if (m_vram_model.TruthAny().empty())
		return;

	const GSDrawingContext* ctx = m_context;
	GSPageBitmap need;

	{
		const GSTileSurfaceLayout fb_l{ctx->FRAME.Block(), static_cast<u8>(ctx->FRAME.FBW),
			static_cast<u8>(ctx->FRAME.PSM), GSTileSurfaceKind::Color};
		need |= m_vram_model.ReadbackNeeded(PagesForTargetRect(fb_l, r), kGSTilePlanesAll);
	}
	if (plan.z_write || plan.z_test)
	{
		const GSTileSurfaceLayout z_l{ctx->ZBUF.Block(), static_cast<u8>(ctx->FRAME.FBW),
			static_cast<u8>(ctx->ZBUF.PSM), GSTileSurfaceKind::Depth};
		need |= m_vram_model.ReadbackNeeded(PagesForTargetRect(z_l, r), kGSTilePlanesAll);
	}
	if (PRIM->TME)
	{
		const auto tex_pages = [](u32 bp, u32 bw, u32 psm, int tw, int th) {
			const GSTileSurfaceLayout l{bp, static_cast<u8>(bw), static_cast<u8>(psm), KindForPsm(psm)};
			return PagesForTargetRect(l, GSVector4i(0, 0, std::max(tw, 1), std::max(th, 1)));
		};
		const int tw = 1 << ctx->TEX0.TW;
		const int th = 1 << ctx->TEX0.TH;
		need |= m_vram_model.ReadbackNeeded(
			tex_pages(ctx->TEX0.TBP0, ctx->TEX0.TBW, ctx->TEX0.PSM, tw, th), kGSTilePlanesAll);
		// TODO(M2, recorded gap): CLUT loads read CBP pages at TEX0-write time, before
		// any draw-side seam runs; a palette rendered on the GPU and consumed as CLUT
		// would be read stale. No such pattern exists in the corpus; the oracle gate
		// is the detector.
		if (IsMipMapActive())
		{
			const GIFRegMIPTBP1& mip1 = ctx->MIPTBP1;
			const GIFRegMIPTBP2& mip2 = ctx->MIPTBP2;
			const u32 tbp[6] = {static_cast<u32>(mip1.TBP1), static_cast<u32>(mip1.TBP2),
				static_cast<u32>(mip1.TBP3), static_cast<u32>(mip2.TBP4),
				static_cast<u32>(mip2.TBP5), static_cast<u32>(mip2.TBP6)};
			const u32 tbw[6] = {static_cast<u32>(mip1.TBW1), static_cast<u32>(mip1.TBW2),
				static_cast<u32>(mip1.TBW3), static_cast<u32>(mip2.TBW4),
				static_cast<u32>(mip2.TBW5), static_cast<u32>(mip2.TBW6)};
			const int mxl = std::min<int>(static_cast<int>(ctx->TEX1.MXL), 6);
			for (int level = 1; level <= mxl; level++)
			{
				need |= m_vram_model.ReadbackNeeded(
					tex_pages(tbp[level - 1], tbw[level - 1], ctx->TEX0.PSM, tw >> level, th >> level),
					kGSTilePlanesAll);
			}
		}
	}

	ReadbackModelPages(need);
}

// The floor draw's write footprint, observed into the memory model. Claims are
// conservative on purpose: over-claiming a write costs a wasted spill, never bytes,
// because SpillForFloorDraw synced the footprint first. Coverage inside the bbox is
// not rectangular (triangles), so draws use the page-granular flow — block-wise
// shrink is reserved for true rect writers (transfers, moves).
void GSRendererTile::ObserveFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r)
{
	if (r.rempty())
		return;

	const GSDrawingContext* ctx = m_context;

	u8 fb_planes = gsTilePlanesInvalidatedByWrite(ctx->FRAME.PSM);
	const u32 fbmsk = ctx->FRAME.FBMSK;
	if (fbmsk == 0xFFFFFFFFu)
	{
		fb_planes = 0;
	}
	else
	{
		// Whole-byte FBMSK exemptions; partially masked bytes still count as
		// written (and keep the Z byte-alias claim).
		if ((fbmsk & 0xFF000000u) == 0xFF000000u)
			fb_planes &= static_cast<u8>(~kGSTilePlanesAlpha);
		if ((fbmsk & 0x00FFFFFFu) == 0x00FFFFFFu)
			fb_planes &= static_cast<u8>(~GSTilePlaneRGB);
	}
	if (fb_planes != 0)
	{
		const GSTileSurfaceLayout fb_l{ctx->FRAME.Block(), static_cast<u8>(ctx->FRAME.FBW),
			static_cast<u8>(ctx->FRAME.PSM), GSTileSurfaceKind::Color};
		m_vram_model.OnCpuWrite(PagesForTargetRect(fb_l, r), fb_planes);
	}

	// ZTE=0 disables the write as well as the test (the SW scanline derivation).
	if (plan.z_write)
	{
		const GSTileSurfaceLayout z_l{ctx->ZBUF.Block(), static_cast<u8>(ctx->FRAME.FBW),
			static_cast<u8>(ctx->ZBUF.PSM), GSTileSurfaceKind::Depth};
		m_vram_model.OnCpuWrite(PagesForTargetRect(z_l, r), gsTilePlanesInvalidatedByWrite(ctx->ZBUF.PSM));
	}
}

// -- The native route ----------------------------------------------------------------

GSTileSurfaceId GSRendererTile::EnsureSurface(const GSTileSurfaceLayout& layout, const GSVector4i& rect,
	const GSPageBitmap& pages, bool& ok)
{
	const int height = GSTileTargetPool::HeightForPages(layout, pages);
	GSTileSurfaceId id = m_vram_model.FindExact(layout);
	if (id == kGSTileNoSurface)
	{
		const u32 handle = m_target_pool.Allocate(layout, height);
		if (handle == 0)
		{
			ok = false;
			return kGSTileNoSurface;
		}
		id = m_vram_model.Create(layout, rect, handle);
		m_vram_model.GrowResidency(id, pages);
		return id;
	}

	m_vram_model.GrowResidency(id, pages);
	if (!m_target_pool.EnsureHeight(m_vram_model.Get(id).pool_handle, height))
		ok = false;
	return id;
}

// Pages whose texture bytes are not already current for every relevant plane. Before
// re-uploading such a page from CPU memory, any GPU-newest truth on it (any plane, any
// owner) must be pulled first — the caller routes this set through ReadbackNeeded.
GSPageBitmap GSRendererTile::PagesNeedingUpload(GSTileSurfaceId id, const GSPageBitmap& pages, u8 relevant_planes) const
{
	GSPageBitmap need;
	pages.forEachSetPage([&](u32 page) {
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(relevant_planes & (1u << pi)))
				continue;
			if (m_vram_model.OwnerOf(page, pi) != id ||
				m_vram_model.TruthMask(page, pi) != GSVramModel::kFullBlockMask)
			{
				need.set(page);
				return;
			}
		}
	});
	return need;
}

bool GSRendererTile::TryNativeDraw(const GSTileDrawPlan& plan, const GSVector4i& r, GSTileFloorReason& reason)
{
	const GSDrawingContext* ctx = m_context;
	const bool want_rt = plan.colormask != 0;
	const bool want_ds = plan.z_write || plan.z_test;
	pxAssert(want_rt || plan.z_write);

	const GSTileSurfaceLayout fb_l{ctx->FRAME.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->FRAME.PSM), GSTileSurfaceKind::Color};
	const GSTileSurfaceLayout z_l{ctx->ZBUF.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->ZBUF.PSM), GSTileSurfaceKind::Depth};

	// Extent: the draw must stay inside the stride (a wider draw wraps into the next
	// page row) and inside page space (a deep enough draw wraps back to page 0,
	// folding two pixel rows onto one physical page — the texture cannot express it).
	if (r.z > static_cast<int>(ctx->FRAME.FBW) * 64)
	{
		reason = GSTileFloorReason::FootprintExtent;
		return false;
	}
	const auto rows_ok = [&r](const GSTileSurfaceLayout& l) {
		const GSTileTargetPool::Geometry g = GSTileTargetPool::GeometryFor(l);
		const int rows = (r.w + g.pgs.y - 1) / g.pgs.y;
		return rows * g.ppr <= static_cast<int>(GS_MAX_PAGES);
	};
	if ((want_rt && !rows_ok(fb_l)) || (want_ds && !rows_ok(z_l)))
	{
		reason = GSTileFloorReason::FootprintExtent;
		return false;
	}
	pxAssert((fb_l.bp & 31) == 0 && (z_l.bp & 31) == 0); // FBP/ZBP are page units

	const GSPageBitmap fb_pages = want_rt ? GSVramModel::PagesForRect(fb_l, r) : GSPageBitmap();
	const GSPageBitmap z_pages = want_ds ? GSVramModel::PagesForRect(z_l, r) : GSPageBitmap();
	if (fb_pages.intersects(z_pages))
	{
		reason = GSTileFloorReason::FrameZOverlap;
		return false;
	}

	bool ok = true;
	GSTileSurfaceId fb_id = kGSTileNoSurface;
	GSTileSurfaceId z_id = kGSTileNoSurface;
	if (want_rt)
		fb_id = EnsureSurface(fb_l, r, fb_pages, ok);
	if (want_ds)
		z_id = EnsureSurface(z_l, r, z_pages, ok);
	if (!ok)
	{
		reason = GSTileFloorReason::ResourceFailure;
		return false;
	}

	const u32 fb_handle = want_rt ? m_vram_model.Get(fb_id).pool_handle : 0;
	const u32 z_handle = want_ds ? m_vram_model.Get(z_id).pool_handle : 0;

	// The backend requires rt and ds to be the same pixel size.
	if (want_rt && want_ds)
	{
		const int h = std::max(m_target_pool.GetHeight(fb_handle), m_target_pool.GetHeight(z_handle));
		if (!m_target_pool.EnsureHeight(fb_handle, h) || !m_target_pool.EnsureHeight(z_handle, h))
		{
			reason = GSTileFloorReason::ResourceFailure;
			return false;
		}
	}

	// Everything below reads CPU local memory (uploads) or writes it (spill
	// readbacks); queued floor draws own those bytes until drained.
	Sync(8);

	// Destination prep: a footprint page skips its upload only when this surface's
	// texture is already current for every relevant plane; before re-uploading, pull
	// whatever GPU-newest truth sits on the upload pages (any plane, any owner), plus
	// everything the claims will steal from other owners.
	GSPageBitmap up_fb;
	GSPageBitmap up_z;
	if (want_rt)
		up_fb = PagesNeedingUpload(fb_id, fb_pages, plan.fb_claims & kGSTilePlanesColor);
	if (want_ds)
		up_z = PagesNeedingUpload(z_id, z_pages, GSTilePlaneZ);

	GSPageBitmap sync_set = m_vram_model.ReadbackNeeded(up_fb | up_z, kGSTilePlanesAll);
	if (want_rt)
		sync_set |= m_vram_model.SpillBeforeNativeDraw(fb_id, fb_pages, plan.fb_claims);
	if (plan.z_write)
		sync_set |= m_vram_model.SpillBeforeNativeDraw(z_id, z_pages, plan.z_claims);

	if (!ReadbackModelPages(sync_set) ||
		(want_rt && !m_target_pool.UploadPages(m_mem, fb_handle, fb_l, up_fb)) ||
		(want_ds && !m_target_pool.UploadPages(m_mem, z_handle, z_l, up_z)))
	{
		// Nothing has been claimed yet, so the model is consistent and the floor
		// path (which spills again from the model's state) remains correct.
		reason = GSTileFloorReason::ResourceFailure;
		return false;
	}

	SubmitNativeDraw(plan, r,
		want_rt ? m_target_pool.GetTexture(fb_handle) : nullptr,
		want_ds ? m_target_pool.GetTexture(z_handle) : nullptr);

	if (want_rt)
		m_vram_model.OnNativeDraw(fb_id, fb_pages, plan.fb_claims);
	if (plan.z_write)
		m_vram_model.OnNativeDraw(z_id, z_pages, plan.z_claims);

	return true;
}

// Port of GSRendererHW::HandleFlatShadedVertices for the one case the native
// envelope can hit: flat-shaded triangles on a device without last-vertex provoking
// convention. (Sprites resolve their flat color in the expansion shader, and AA1 —
// the other reason Classic runs this — floors.)
void GSRendererTile::FlattenProvokingColor()
{
	if (g_gs_device->Features().provoking_vertex_last)
		return;

	constexpr int n = 3;
	bool prims_flat = true;
	for (u32 i = 0; i < m_index->tail && prims_flat; i += n)
	{
		for (int j = 0; j < n - 1; j++)
		{
			if (m_vertex->buff[m_index->buff[i + j]].RGBAQ.U32[0] != m_vertex->buff[m_index->buff[i + n - 1]].RGBAQ.U32[0])
			{
				prims_flat = false;
				break;
			}
		}
	}
	if (prims_flat)
		return;

	// De-index the vertices using the copy buffer, then give every vertex of each
	// prim the provoking (last) vertex's color.
	while (m_vertex->maxcount < m_index->tail)
		GrowVertexBuffer();
	for (int i = static_cast<int>(m_index->tail) - 1; i >= 0; i--)
	{
		m_vertex->buff_copy[i] = m_vertex->buff[m_index->buff[i]];
		m_index->buff[i] = static_cast<u16>(i);
	}
	std::swap(m_vertex->buff, m_vertex->buff_copy);
	m_vertex->head = m_vertex->next = m_vertex->tail = m_index->tail;

	for (u32 i = 0; i < m_index->tail; i += n)
	{
		for (int j = 0; j < n - 1; j++)
			m_vertex->buff[i + j].RGBAQ.U32[0] = m_vertex->buff[i + n - 1].RGBAQ.U32[0];
	}
}

void GSRendererTile::SubmitNativeDraw(const GSTileDrawPlan& plan, const GSVector4i& r, GSTexture* rt, GSTexture* ds)
{
	const GSDrawingContext* ctx = m_context;
	GSHWDrawConfig conf = {};

	conf.rt = rt;
	conf.ds = ds;

	const bool iip = !IsFlatShaded();
	conf.vs.iip = iip;
	conf.vs.fst = 1;
	conf.ps.iip = iip;
	conf.ps.tfx = 4;
	conf.ps.no_color1 = 1;
	conf.ps.dst_fmt = GSLocalMemory::m_psm[ctx->FRAME.PSM].fmt;
	conf.colormask = GSHWDrawConfig::ColorMaskSelector(plan.colormask);
	if (!rt)
		conf.ps.DisableColorOutput();

	// Depth state + the PS2 z pipeline (EmulateZbuffer's recipe: clamp to the
	// format's max after rasterization, floor the interpolated z to an integer where
	// one z unit is finer than a float32 ULP).
	conf.depth.ztst = ds ? plan.ztst : static_cast<u8>(ZTST_ALWAYS);
	conf.depth.zwe = plan.z_write;
	conf.cb_vs.max_depth = 0xFFFFFFFFu;
	if (ds)
	{
		const u32 max_z = 0xFFFFFFFFu >> (GSLocalMemory::m_psm[ctx->ZBUF.PSM].fmt * 8);
		const bool large_z = static_cast<u32>(GSVector4i(m_vt.m_max.p).z) > max_z;
		const bool flat_z = m_vt.m_eq.z || m_vt.m_primclass == GS_SPRITE_CLASS;
		const bool zfloor_noop = static_cast<u32>(GSVector4i(m_vt.m_min.p).z) >= (1u << 23);
		conf.ps.zfloor = !flat_z && !zfloor_noop && !g_gs_device->Features().no_ps2_z_quantization &&
			(plan.z_write || (plan.z_test && ctx->TEST.ZTST == ZTST_GREATER));
		if (plan.z_write && large_z)
		{
			if (flat_z)
			{
				conf.cb_vs.max_depth = max_z;
			}
			else
			{
				conf.cb_ps.TA_MaxDepth_Af.z = static_cast<float>(max_z) * 0x1p-32f;
				conf.ps.zclamp = 1;
			}
		}
	}

	switch (m_vt.m_primclass)
	{
		case GS_TRIANGLE_CLASS:
			if (!iip)
				FlattenProvokingColor();
			conf.topology = GSHWDrawConfig::Topology::Triangle;
			conf.indices_per_prim = 3;
			conf.nindices = m_index->tail;
			break;
		case GS_SPRITE_CLASS:
			conf.topology = GSHWDrawConfig::Topology::Triangle;
			conf.vs.expand = GSHWDrawConfig::VSExpand::Sprite;
			conf.indices_per_prim = 6;
			conf.nindices = m_index->tail * 3;
			break;
		default:
			ASSUME(0);
	}
	conf.verts = m_vertex->buff;
	conf.nverts = m_vertex->next;
	conf.indices = m_index->buff;

	// Native vertex mapping. No half-pixel-offset hacks by charter, and the console
	// coverage capture pinned this conversion: all three renderers match silicon at
	// native resolution through exactly this formula.
	const GSVector2i ts = (rt ? rt : ds)->GetSize();
	const float sx = 2.0f / static_cast<float>(ts.x << 4);
	const float sy = 2.0f / static_cast<float>(ts.y << 4);
	const float ox = static_cast<float>(static_cast<int>(ctx->XYOFFSET.OFX));
	const float oy = static_cast<float>(static_cast<int>(ctx->XYOFFSET.OFY));
	conf.cb_vs.vertex_scale = GSVector2(sx, sy);
	conf.cb_vs.vertex_offset = GSVector2(ox * sx + (-1.0f / static_cast<float>(ts.x)) + 1.0f,
		oy * sy + (-1.0f / static_cast<float>(ts.y)) + 1.0f);
	conf.cb_ps.ScaleFactor = GSVector4(1.0f / 16.0f, 1.0f, 1.0f, 0.0f);

	conf.scissor = ctx->scissor.in.rintersect(GSVector4i(0, 0, ts.x, ts.y));
	conf.drawarea = r;

	g_gs_device->RenderHW(conf);
}

// -- Draw dispatch -------------------------------------------------------------------

// The register view of the row, from the raw draw context. The Classic recorder
// (GSRendererHW::RecordDrawLogEntry) reads m_cached_ctx because that is what its draw
// consumes; both Tile routes execute against the uncooked context, so that is what
// gets recorded here.
void GSRendererTile::RecordDrawLogEntry() const
{
	const GIFRegTEST& TEST = m_context->TEST;
	const GIFRegALPHA& ALPHA = m_context->ALPHA;

	GSDrawLog::Record rec = {};
	rec.frame = static_cast<u32>(g_perfmon.GetFrame());
	rec.draw = static_cast<u32>(s_n);
	rec.prim_type = static_cast<u8>(PRIM->PRIM);
	rec.prim_count = static_cast<u16>(std::min<u32>(m_index->tail, 0xFFFFu));

	rec.frame_block = m_context->FRAME.Block();
	rec.frame_psm = static_cast<u8>(m_context->FRAME.PSM);
	rec.frame_fbw = static_cast<u8>(m_context->FRAME.FBW);
	rec.frame_fbmsk = m_context->FRAME.FBMSK;

	rec.z_block = m_context->ZBUF.Block();
	rec.z_psm = static_cast<u8>(m_context->ZBUF.PSM);
	rec.z_ztst = static_cast<u8>(TEST.ZTST);

	rec.tex_tbp0 = m_context->TEX0.TBP0;
	rec.tex_psm = static_cast<u8>(m_context->TEX0.PSM);
	rec.tex_tbw = static_cast<u8>(m_context->TEX0.TBW);
	rec.tex_tw = static_cast<u8>(m_context->TEX0.TW);
	rec.tex_th = static_cast<u8>(m_context->TEX0.TH);

	rec.alpha = static_cast<u16>((ALPHA.A << 6) | (ALPHA.B << 4) | (ALPHA.C << 2) | ALPHA.D);
	rec.atst = static_cast<u8>(TEST.ATST);
	rec.afail = static_cast<u8>(TEST.AFAIL);
	rec.datm = static_cast<u8>(TEST.DATM);

	rec.flags = static_cast<u8>((PRIM->TME ? GSDrawLog::FlagTextured : 0) |
								(PRIM->ABE ? GSDrawLog::FlagBlend : 0) |
								(TEST.ATE ? GSDrawLog::FlagAlphaTest : 0) |
								(TEST.DATE ? GSDrawLog::FlagDate : 0) |
								(TEST.ZTE ? GSDrawLog::FlagZTest : 0) |
								(m_context->ZBUF.ZMSK ? GSDrawLog::FlagZMask : 0));

	GSDrawLog::BeginDraw(rec);
}

void GSRendererTile::Draw()
{
	const bool log = GSDrawLog::IsActive();
	u64 record_start = 0;
	if (log) [[unlikely]]
	{
		record_start = Common::Timer::GetCurrentValue();
		RecordDrawLogEntry();
	}

	const GSVector4i r = ComputeDrawRect();
	const GSTileDrawPlan plan = LowerCurrentDraw();
	GSTileFloorReason reason = plan.reason;

	bool native = false;
	if (plan.native)
	{
		// An empty rect draws nothing on either route; count it native for free.
		native = r.rempty() || TryNativeDraw(plan, r, reason);
	}

	if (!native)
	{
		if (!r.rempty())
			SpillForFloorDraw(plan, r);
		ObserveFloorDraw(plan, r);
	}

	// record_ns covers the Tile-side work of the draw — the ledger row, the model
	// flows, and (for native draws) the uploads and submission — never the floor's
	// SW rasterization below.
	u32 record_ns = 0;
	if (log) [[unlikely]]
		record_ns = static_cast<u32>(
			Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - record_start));

	if (!native)
		GSRendererSW::Draw();

	if (log) [[unlikely]]
	{
		GSDrawLog::NoteTileDraw(/*memo_hit=*/false, record_ns, /*pass_id=*/0, MapFallbackReason(reason));
		GSDrawLog::FinishDraw();
	}
}
