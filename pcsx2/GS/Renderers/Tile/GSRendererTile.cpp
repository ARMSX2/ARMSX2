// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSRendererTile.h"

#include "GS/GSLocalMemory.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/HW/GSDrawLog.h"
#include "GS/Renderers/SW/GSRasterizer.h"

#include "common/Timer.h"

#include <cmath>
#include <cstring>

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
		case GSTileFloorReason::Dither:
			return GSDrawLog::TileFallbackDither;
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
		case GSTileFloorReason::FrameZPairing:
			return GSDrawLog::TileFallbackFrameZPairing;
		case GSTileFloorReason::StrideZero:
			return GSDrawLog::TileFallbackStrideZero;
		case GSTileFloorReason::FootprintExtent:
			return GSDrawLog::TileFallbackFootprintExtent;
		case GSTileFloorReason::FrameZOverlap:
			return GSDrawLog::TileFallbackFrameZOverlap;
		case GSTileFloorReason::ResourceFailure:
			return GSDrawLog::TileFallbackResourceFailure;
		case GSTileFloorReason::TextureFiltered:
			return GSDrawLog::TileFallbackTextureFiltered;
		case GSTileFloorReason::TextureMip:
			return GSDrawLog::TileFallbackTextureMip;
		case GSTileFloorReason::TexturePsm:
			return GSDrawLog::TileFallbackTexturePsm;
		case GSTileFloorReason::TextureFeedback:
			return GSDrawLog::TileFallbackTextureFeedback;
		case GSTileFloorReason::TexturePerspective:
			return GSDrawLog::TileFallbackTexturePerspective;
		case GSTileFloorReason::TextureStqOverflow:
			return GSDrawLog::TileFallbackTextureStqOverflow;
		case GSTileFloorReason::DepthWalkEnvelope:
			return GSDrawLog::TileFallbackDepthWalkEnvelope;
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
	m_tex_source.Clear();
	m_target_pool.ReleaseAll();
	GSRendererSW::Destroy();
}

void GSRendererTile::Reset(bool hardware_reset)
{
	GSRendererSW::Reset(hardware_reset);
	// Everything falls back to CPU-newest; surface textures die with the reset.
	m_tex_source.Clear();
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
	// What the alpha-test pass split needs to know about fragment ordering. Both are
	// only consulted when the split would otherwise reorder depth writes, and
	// PrimitiveOverlap walks the vertex list, so ask for it only then.
	in.z_constant = m_vt.m_eq.z;
	if (m_context->TEST.ATE && m_context->TEST.ATST != ATST_ALWAYS && m_context->TEST.ATST != ATST_NEVER &&
		m_context->TEST.ZTE && !m_context->ZBUF.ZMSK && !m_vt.m_eq.z)
	{
		in.prim_overlap_none = PrimitiveOverlap() == PRIM_OVERLAP_NO;
	}
	in.tme = PRIM->TME;
	in.abe = PRIM->ABE;
	in.aa1 = PRIM->AA1;
	in.fba = m_context->FBA.FBA;
	in.dthe = m_draw_env->DTHE.DTHE;
	in.vs_expand = g_gs_device->Features().vs_expand;
	if (PRIM->TME)
	{
		// The same criteria the SW scanline uses to choose its filter and mip state,
		// so the native path and the floor agree on which draws do what.
		in.tex_linear = m_vt.IsLinear();
		in.tex_mip = IsMipMapActive();
		in.tex_fst = PRIM->FST;
		in.tex_psm = static_cast<u8>(m_context->TEX0.PSM);
		if (in.tex_mip)
		{
			// The level geometry maps onto a GPU mip chain when every addressed
			// level exists: min(MXL,6) ≤ max(TW,TH) (the GS floors level sizes at
			// one texel exactly like the chain does), on a real-sized base.
			const u32 mxl = std::min<u32>(m_context->TEX1.MXL, 6);
			in.tex_mip_fit = m_context->TEX0.TW <= 10 && m_context->TEX0.TH <= 10 &&
							 mxl <= std::max<u32>(m_context->TEX0.TW, m_context->TEX0.TH);
		}
		if (!PRIM->FST)
		{
			// The scanline's 16.16 STQ envelope. The trace's !fst t.xy are the
			// PER-VERTEX quotients S/Q, T/Q (FindMinMax divides), so for a draw
			// whose Q never reaches zero they bound every pixel's quotient — an
			// interpolated s/q is a positively-weighted mediant of the vertex
			// ratios. Floor when any STQ component is NaN, when Q can reach or
			// cross zero (a pole makes the interpolated quotient unbounded), when
			// the quotient hull in texels leaves the format that the scanline
			// stores (SW rewrites CLAMP-mode draws and host-saturates the rest),
			// or when TW/TH exceed the native source's 1024 clamp (the scanline
			// scales its numerator by the raw shift there).
			//
			// ⚠️ The hull is ALREADY IN TEXELS: for !fst the trace scales the
			// quotients by (1 << TW, 1 << TH) as it finalises them, so scaling again
			// here inflated it by the texture size and floored 11,631 of 34,199 corpus
			// draws — a third of them — on coordinates the scanline considers
			// ordinary. GSRendererSW's own overflow test is the authority on this
			// envelope, and it compares the same trace values against the same 32766
			// with no scaling at all.
			in.tex_stq_guard = gsTileStqGuard(m_vt.m_min.t, m_vt.m_max.t, m_vt.nan.value,
				m_context->TEX0.TW, m_context->TEX0.TH);
		}
	}
	return gsTileLowerDraw(in);
}

// -- Spill machinery -----------------------------------------------------------------

// Pull the newest bytes of these pages from the GPU into CPU local memory. Per page,
// per plane, whichever surface owns the unsynced truth supplies the bytes of that
// plane's byte window; a page whose planes split across surfaces is pulled from each
// owner under disjoint byte masks. Afterwards every truth plane on the pages is
// synced, and the SW texture cache is told its cached deswizzles went stale.
//
// A page whose truth a CPU transfer has already shrunk block-wise takes the partial
// path below instead: the pull is restricted to the blocks still owned by the GPU, or
// it would write the texture's stale copy back over the bytes the transfer just gave
// the CPU. That is the gs-sync ordering defect (an upload over a just-drawn region
// came back 0 of 2048 fresh under Tile), and it bit both directions of travel — the
// native draw path makes a partial page CPU-current through this same call before it
// re-uploads the whole page.
bool GSRendererTile::ReadbackModelPages(const GSPageBitmap& pages)
{
	if (pages.empty())
		return true;

	// Split off the pages some plane holds only block-partially; the whole-page
	// bucketing below stays the hot path for everything else.
	GSPageBitmap partial_pages;
	pages.forEachSetPage([&](u32 page) {
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!m_vram_model.Truth(pi).test(page) || m_vram_model.SyncedPages(pi).test(page))
				continue;
			if (m_vram_model.TruthMask(page, pi) != GSVramModel::kFullBlockMask)
				partial_pages.set(page);
		}
	});

	bool partial_ok = true;
	partial_pages.forEachSetPage([&](u32 page) {
		GSPageBitmap one;
		one.set(page);
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!m_vram_model.Truth(pi).test(page) || m_vram_model.SyncedPages(pi).test(page))
				continue;
			const GSTileSurfaceId owner = m_vram_model.OwnerOf(page, pi);
			pxAssert(owner != kGSTileNoSurface);
			const GSVramModel::Surface& surf = m_vram_model.Get(owner);
			partial_ok &= m_target_pool.ReadbackPages(m_mem, surf.pool_handle, surf.layout, one,
				PlaneByteMask(pi, surf.layout), m_vram_model.TruthMask(page, pi));
			InvalidateSwTexCache(surf.layout, one);
		}
	});

	const GSPageBitmap full_pages = pages.andnot(partial_pages);
	if (full_pages.empty())
	{
		m_vram_model.OnReadback(pages);
		return partial_ok;
	}

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

	full_pages.forEachSetPage([&](u32 page) {
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
		bool ok = partial_ok;
		full_pages.forEachSetPage([&](u32 page) {
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

	bool ok = partial_ok;
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

	// Depth transcription: a triangle draw with a live Z gradient must store and
	// compare the SW scanline's WALK, not the GPU interpolator's plane — gs-zgrad
	// measured the difference on 40 of its 47 cases (a +1 rung everywhere the plane
	// holds a fraction, ±1 at the truncated 2^-10 step's integer landings, 342 units
	// on a steep gradient carried in fp32). Draws the walk cannot express floor
	// rather than approximate. Flat-Z draws stay exact without it: their constant is
	// an integer, which survives the float pipeline untouched.
	//
	// Coordinate transcription: a perspective texture coordinate must come off the
	// primitive's own plane at the pixel index rather than off the interpolator,
	// because the vertex shader's 1/320-pixel coverage nudge translates the whole
	// primitive and an interpolated attribute rides its own gradient by the shift the
	// rasterizer realizes. Both planes come out of the same setup and travel in one
	// payload.
	const bool want_zwalk = want_ds && m_vt.m_primclass == GS_TRIANGLE_CLASS && !m_vt.m_eq.z;
	const bool want_stq = PRIM->TME && !PRIM->FST &&
		(m_vt.m_primclass == GS_TRIANGLE_CLASS || m_vt.m_primclass == GS_SPRITE_CLASS);
	if (!BuildTilePayload(want_zwalk, want_stq, reason))
		return false;

	// The texture window's footprint. A window overlapping the draw's own write
	// pages is feedback — M4 territory. (A zero TBW over-claims every page through
	// PagesForTargetRect and floors here too; safe, if mislabeled.)
	//
	// The size the native path samples is the SIZE-FIXED TEX0, not the register
	// claim: GSRendererSW builds its cached texture and cooks its wrap tables from
	// GetSizeFixedTEX0 (shrinking oversized claims to the coordinate range,
	// extending for region windows), and an out-of-range coordinate wraps at THAT
	// size. Sampling at the register size reads different memory texels wherever
	// the two disagree — OutRun's roadside strips measured exactly that. The
	// coordinate NUMERATOR keeps the register scale (ConvertVertexBuffer scales by
	// the raw claim); only storage and wrap follow the fixed size, mirroring SW.
	const bool textured = PRIM->TME;
	const bool mip = textured && IsMipMapActive();
	GIFRegTEX0 fixed_tex0 = {};
	GIFRegTEX0 level_tex0[7] = {};
	u32 mip_levels = 1;
	GSPageBitmap tex_pages;
	if (textured)
	{
		// Under mip the size fix is a no-op (GetSizeFixedTEX0 returns the register
		// view), matching the SW renderer's own call — the register/fixed split
		// collapses and every level keeps its GetTex0Layer geometry.
		fixed_tex0 = ctx->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), mip);
		level_tex0[0] = fixed_tex0;
		if (mip)
			mip_levels = std::min<u32>(ctx->TEX1.MXL, 6) + 1;
		for (u32 i = 1; i < mip_levels; i++)
			level_tex0[i] = GetTex0Layer(i);
		for (u32 i = 0; i < mip_levels; i++)
		{
			const GSTileSurfaceLayout tex_l{level_tex0[i].TBP0, static_cast<u8>(level_tex0[i].TBW),
				static_cast<u8>(level_tex0[i].PSM), KindForPsm(level_tex0[i].PSM)};
			const int tw = 1 << std::min<u32>(level_tex0[i].TW, 10);
			const int th = 1 << std::min<u32>(level_tex0[i].TH, 10);
			tex_pages |= PagesForTargetRect(tex_l, GSVector4i(0, 0, tw, th));
		}
		if (tex_pages.intersects(fb_pages) || tex_pages.intersects(z_pages))
		{
			reason = GSTileFloorReason::TextureFeedback;
			return false;
		}
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
	//
	// "Relevant" is the BYTE SPAN of the claims, never the claims themselves -- see
	// gsTileColorPlanesSpannedBy. A partially masked color write (alpha-only is the
	// common one) renders channels it does not write straight out of whatever the
	// texture already held, and then claims the whole cell on the page's behalf, so
	// gating its upload on the written channels alone republishes stale bytes. The
	// depth side needs no such widening: Z's own span IS the whole cell, so a current
	// Z plane already implies a current cell.
	GSPageBitmap up_fb;
	GSPageBitmap up_z;
	if (want_rt)
		up_fb = PagesNeedingUpload(fb_id, fb_pages, gsTileColorPlanesSpannedBy(plan.fb_claims));
	if (want_ds)
		up_z = PagesNeedingUpload(z_id, z_pages, GSTilePlaneZ);

	GSPageBitmap sync_set = m_vram_model.ReadbackNeeded(up_fb | up_z, kGSTilePlanesAll);
	if (want_rt)
		sync_set |= m_vram_model.SpillBeforeNativeDraw(fb_id, fb_pages, plan.fb_claims);
	if (plan.z_write)
		sync_set |= m_vram_model.SpillBeforeNativeDraw(z_id, z_pages, plan.z_claims);
	if (textured)
		sync_set |= m_vram_model.ReadbackNeeded(tex_pages, kGSTilePlanesAll);

	if (!ReadbackModelPages(sync_set) ||
		(want_rt && !m_target_pool.UploadPages(m_mem, fb_handle, fb_l, up_fb)) ||
		(want_ds && !m_target_pool.UploadPages(m_mem, z_handle, z_l, up_z)))
	{
		// Nothing has been claimed yet, so the model is consistent and the floor
		// path (which spills again from the model's state) remains correct.
		reason = GSTileFloorReason::ResourceFailure;
		return false;
	}

	// The texture source, built from (now-current) CPU local memory with palette and
	// TEXA expansion applied by the readers. The CLUT read buffer refreshes the same
	// way the SW rasterizer's would have. (Standing M2 gap: the palette itself was
	// loaded from CPU bytes at TEX0-write time, before any draw-side seam ran — a
	// GPU-rendered palette would be read stale; the oracle gate is the detector.)
	GSTexture* tex = nullptr;
	if (textured)
	{
		u32 pal_gen = 0;
		if (GSLocalMemory::m_psm[ctx->TEX0.PSM].pal > 0)
		{
			m_mem.m_clut.Read32(ctx->TEX0, m_draw_env->TEXA);
			pal_gen = m_mem.m_clut.GetWriteGeneration();
		}
		tex = m_tex_source.Lookup(m_mem, m_vram_model, fixed_tex0, m_draw_env->TEXA, tex_pages, pal_gen,
			level_tex0, mip_levels);
		if (!tex)
		{
			reason = GSTileFloorReason::ResourceFailure;
			return false;
		}
	}

	SubmitNativeDraw(plan, r, fixed_tex0,
		want_rt ? m_target_pool.GetTexture(fb_handle) : nullptr,
		want_ds ? m_target_pool.GetTexture(z_handle) : nullptr,
		tex);

	if (want_rt)
		m_vram_model.OnNativeDraw(fb_id, fb_pages, plan.fb_claims);
	if (plan.z_write)
		m_vram_model.OnNativeDraw(z_id, z_pages, plan.z_claims);

	return true;
}

// De-index the current draw through the copy buffer, leaving the index buffer as the
// identity mapping. Primitive ORDER is preserved, which is what the depth walk relies
// on: after this, the vertex shader can derive the primitive ordinal from
// gl_VertexIndex alone, and ordinal p still names the p-th kicked triangle.
void GSRendererTile::DeindexVertices()
{
	while (m_vertex->maxcount < m_index->tail)
		GrowVertexBuffer();
	for (int i = static_cast<int>(m_index->tail) - 1; i >= 0; i--)
	{
		m_vertex->buff_copy[i] = m_vertex->buff[m_index->buff[i]];
		m_index->buff[i] = static_cast<u16>(i);
	}
	std::swap(m_vertex->buff, m_vertex->buff_copy);
	m_vertex->head = m_vertex->next = m_vertex->tail = m_index->tail;
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
	DeindexVertices();

	for (u32 i = 0; i < m_index->tail; i += n)
	{
		for (int j = 0; j < n - 1; j++)
			m_vertex->buff[i + j].RGBAQ.U32[0] = m_vertex->buff[i + n - 1].RGBAQ.U32[0];
	}
}

// Build the per-primitive plane payload the fragment walks read: the depth walk's
// blocks (PS_TILE_ZWALK) and the perspective coordinate's blocks (PS_TILE_STQ),
// concatenated into ONE upload — two reservations against the streaming buffer can hit
// a mid-draw flush between them and strand the first one's offset.
//
// Every triangle value comes out of GSRasterizer's own compiled setup
// (GSComputeTriangleZPlane), so it carries the software renderer's exact roundings: the
// fp32 barycentric division, the compiler's fused multiply-adds, the 2^-10 truncation
// of the scan gradient, and the fp32 narrowing the scanline's setup applies to the
// lane-offset gradient. Loading the texture numerators into the same setup takes the
// coordinate gradient off that one division rather than a second, independently-rounded
// one; it cannot disturb the depth values, which are computed from other lanes.
//
// Returns false with `reason` set for draws outside an envelope; those floor.
bool GSRendererTile::BuildTilePayload(bool want_zwalk, bool want_stq, GSTileFloorReason& reason)
{
	m_tile_payload.clear();
	m_zwalk_at = NoWalk;
	m_stq_at = NoWalk;
	if (!want_zwalk && !want_stq)
		return true;

	const auto envelope = [this, &reason](bool ok) {
		if (!ok)
		{
			reason = GSTileFloorReason::DepthWalkEnvelope;
			m_tile_payload.clear();
		}
		return ok;
	};

	// The ordinal varying needs gl_VertexIndex arithmetic (the expand push-constant
	// block) and the payload rides the expand storage-buffer binding; both exist
	// only when the device has VS expand.
	if (!envelope(g_gs_device->Features().vs_expand))
		return false;

	const GSDrawingContext* ctx = m_context;
	const int ofx = static_cast<int>(ctx->XYOFFSET.OFX);
	const int ofy = static_cast<int>(ctx->XYOFFSET.OFY);

	// The coordinate numerator, in the 1/16-texel space the varying carried: the
	// software renderer's own numerator scale (ConvertVertexBuffer's tsize, off the
	// REGISTER claim) divided by 2^12. Both factors are powers of two, so the
	// transported value is the scanline's numerator rescaled rather than a second
	// rounding of the same product, and everything downstream in the shader is
	// unchanged.
	const float num_sx = static_cast<float>(0x10000u << ctx->TEX0.TW) * 0x1p-12f;
	const float num_sy = static_cast<float>(0x10000u << ctx->TEX0.TH) * 0x1p-12f;

	if (m_vt.m_primclass == GS_SPRITE_CLASS)
	{
		// A sprite carries no depth gradient, so the coordinate plane is the only
		// block here — and it is built from the raw vertices rather than through the
		// rasterizer's sprite setup, because what this path removes is the vertex
		// shader's coverage nudge and NOTHING else about which value the fragment
		// sees. The expansion reads the vertex PAIR directly (not through the index
		// buffer), gives all four corners the SECOND vertex's Q, and swaps only each
		// axis' own position and numerator — so S is affine in x alone, T in y alone,
		// and Q is constant, which is the same shape the scanline runs.
		const u32 nprims = m_index->tail / 2;
		m_tile_payload.assign(static_cast<size_t>(nprims) * 12, 0u);
		m_stq_at = 0;

		u32* out = m_tile_payload.data();
		for (u32 p = 0; p < nprims; p++)
		{
			const GSVertex& lt = m_vertex->buff[p * 2 + 0];
			const GSVertex& rb = m_vertex->buff[p * 2 + 1];

			const float x0 = static_cast<float>(static_cast<int>(lt.XYZ.X) - ofx) * (1.0f / 16.0f);
			const float y0 = static_cast<float>(static_cast<int>(lt.XYZ.Y) - ofy) * (1.0f / 16.0f);
			const float x1 = static_cast<float>(static_cast<int>(rb.XYZ.X) - ofx) * (1.0f / 16.0f);
			const float y1 = static_cast<float>(static_cast<int>(rb.XYZ.Y) - ofy) * (1.0f / 16.0f);

			const float s0 = lt.ST.S * num_sx;
			const float t0 = lt.ST.T * num_sy;
			const float s1 = rb.ST.S * num_sx;
			const float t1 = rb.ST.T * num_sy;

			// A zero-extent sprite covers no fragment under either renderer, so this
			// gradient is never evaluated; keep it finite rather than letting a
			// divide by zero into the payload.
			const float dsdx = (x1 != x0) ? ((s1 - s0) / (x1 - x0)) : 0.0f;
			const float dtdy = (y1 != y0) ? ((t1 - t0) / (y1 - y0)) : 0.0f;

			const float blk[12] = {s0, t0, rb.RGBAQ.Q, x0, dsdx, 0.0f, 0.0f, y0, 0.0f, dtdy, 0.0f, 0.0f};
			std::memcpy(out + static_cast<size_t>(p) * 12, blk, sizeof(blk));
		}
		return true;
	}

	const u32 fmt_max = 0xFFFFFFFFu >> (GSLocalMemory::m_psm[ctx->ZBUF.PSM].fmt * 8);
	u32 zmax = 0;
	if (want_zwalk)
	{
		// Hull guards. The truncated step and the fp32 lane offsets under-run the exact
		// plane by at most a few units, so a floor of 8 keeps the walk out of negative
		// territory — where the scanline's store semantics (saturating i32 conversion,
		// unsigned clamp) are not worth transcribing. The high bound keeps every value
		// clear of the zoverflow path (>= 2^31) with margin for overshoot.
		const u32 zmin = static_cast<u32>(GSVector4i(m_vt.m_min.p).z);
		zmax = static_cast<u32>(GSVector4i(m_vt.m_max.p).z);
		if (!envelope(zmin >= 8 && zmax < 0x40000000u))
			return false;
	}

	// The rasterizer's own scissor planes: data.scissor is scissor.in verbatim
	// (GSRendererSW::Draw), and m_fscissor_y is its ywyw() float conversion.
	const GSVector4 fscissor = GSVector4(ctx->scissor.in);
	const GSVector4 fscissor_y = fscissor.ywyw();

	const u32 nprims = m_index->tail / 3;
	const size_t zwords = want_zwalk ? (4 + static_cast<size_t>(nprims) * 20) : 0;
	const size_t swords = want_stq ? (static_cast<size_t>(nprims) * 12) : 0;
	m_tile_payload.assign(zwords + swords, 0u);
	if (want_zwalk)
		m_zwalk_at = 0;
	if (want_stq)
		m_stq_at = static_cast<u32>(zwords / 4);

	u32* zout = m_tile_payload.data();
	u32* sout = m_tile_payload.data() + zwords;
	size_t zc = 0;

	const auto push_u32 = [&](u32 v) { zout[zc++] = v; };
	const auto push_f32 = [&](float v) {
		u32 b;
		std::memcpy(&b, &v, 4);
		zout[zc++] = b;
	};
	const auto push_f64 = [&](double v) {
		u64 b;
		std::memcpy(&b, &v, 8);
		zout[zc++] = static_cast<u32>(b);
		zout[zc++] = static_cast<u32>(b >> 32);
	};

	if (want_zwalk)
	{
		// Header.
		push_f32(fscissor.x);
		push_u32((zmax > fmt_max) ? 1u : 0u); // sel.zclamp, the scanline's own gate
		push_u32(fmt_max);
		push_u32(0u);
	}

	for (u32 p = 0; p < nprims; p++)
	{
		GSVertexSW sw[3];
		for (int k = 0; k < 3; k++)
		{
			const GSVertex& v = m_vertex->buff[m_index->buff[p * 3 + k]];
			// ConvertVertexBuffer's triangle position, scalar and exact: integer
			// subpixels minus the offset, to float (exact under 2^16 magnitude),
			// times the exact 1/16; z widened to double untouched.
			const int x = static_cast<int>(v.XYZ.X) - ofx;
			const int y = static_cast<int>(v.XYZ.Y) - ofy;
			sw[k].p = GSVector4(static_cast<float>(x) * (1.0f / 16.0f),
				static_cast<float>(y) * (1.0f / 16.0f), 0.0f, 0.0f);
			sw[k].p.F64[1] = static_cast<double>(v.XYZ.Z);
			// The coordinate lanes ride the same setup. Raw S/T/Q, matching what the
			// vertex shader put in the varying — NOT the software renderer's q_div
			// pre-division, because the fragment path divides per pixel and the two
			// agree wherever q_div applies (it only fires where Q is constant over the
			// primitive, and a constant divisor commutes with the interpolation).
			sw[k].t = want_stq ? GSVector4(v.ST.S * num_sx, v.ST.T * num_sy, v.RGBAQ.Q, 0.0f)
			                   : GSVector4::zero();
			sw[k].c = GSVector4::zero();
		}

		// A primitive the rasterizer would not draw (degenerate y-order or zero
		// cross product) covers no fragments on the GPU either — the coverage
		// capture certified the conversion — so a zeroed block is never read.
		GSTileZPlane zp;
		if (!GSComputeTriangleZPlane(sw, fscissor_y, zp) || zp.nsections == 0)
		{
			zc += want_zwalk ? 20 : 0;
			continue;
		}

		if (want_stq)
		{
			// Section 0's reference vertex is the origin both terms step from. The
			// plane is the same across sections by construction, so one is enough —
			// this path is not attempting the scanline's row-seed-plus-accumulation
			// walk, only taking the coordinate off the perturbed interpolator.
			const float blk[12] = {
				zp.tseed.x, zp.tseed.y, zp.tseed.z, zp.sec[0].p0x,
				zp.dscan_t.x, zp.dscan_t.y, zp.dscan_t.z, zp.sec[0].p0y,
				zp.dedge_t.x, zp.dedge_t.y, zp.dedge_t.z, 0.0f};
			std::memcpy(sout + static_cast<size_t>(p) * 12, blk, sizeof(blk));
		}

		if (!want_zwalk)
			continue;

		// Per-primitive envelope: the walk's integer arithmetic is exact only while
		// |4M|·quads stays under 2^53; |M| <= 2^34 leaves comfortable margin. Seeds
		// must be the exact vertex integers the analysis relies on.
		const double m1024d = zp.dscan_z * 1024.0; // exact: dscan_z is already M/1024
		if (!envelope(std::abs(m1024d) <= 17179869184.0 /* 2^34 */))
			return false;
		for (int n = 0; n < zp.nsections; n++)
		{
			const double zs = zp.sec[n].zseed;
			if (!envelope(zs >= 0.0 && zs < 4294967296.0 && zs == std::trunc(zs)))
				return false;
		}

		const s64 m1024 = static_cast<s64>(m1024d);
		const u64 m4 = static_cast<u64>((m1024 < 0) ? -m1024 : m1024) * 4;
		const GSTileZPlaneSection& s0 = zp.sec[0];
		const GSTileZPlaneSection& s1 = zp.sec[(zp.nsections == 2) ? 1 : 0];
		// Rows word: the second section's first row in the low half (0xFFFF when
		// single-section, which no fragment row reaches), and the primitive's
		// UNCLIPPED geometric top in the high half, signed and clamped so a wild
		// coordinate saturates instead of wrapping into a row that exists. The top
		// is reserved for the seed-bias mirror (gs-ledger b32224e8ed + 66476f27d9 +
		// 4d7ebb194a): its Y half gates on the primitive's first scanline — the
		// pre-scissor one, because a scissor rejects pixels, it does not reseed an
		// interpolator.
		const u32 boundary = (zp.nsections == 2) ? static_cast<u32>(s1.top) & 0xFFFFu : 0xFFFFu;
		const s32 top_clamped = std::clamp(zp.top_prim, -32768, 32767);
		const u32 rows_word = boundary | (static_cast<u32>(static_cast<u16>(top_clamped)) << 16);

		push_f32(s0.edge_x);
		push_f32(s0.dedge_x);
		push_f32(s0.p0x);
		push_f32(s0.p0y);
		push_f32(s1.edge_x);
		push_f32(s1.dedge_x);
		push_f32(s1.p0x);
		push_f32(s1.p0y);
		push_u32(static_cast<u32>(s0.zseed));
		push_u32(static_cast<u32>(s1.zseed));
		push_u32(rows_word);
		push_f32(zp.dscan_z32);
		push_f64(zp.dscan_z);
		push_u32(static_cast<u32>(m4));
		push_u32(static_cast<u32>(m4 >> 32));
		push_f64(s0.dedge_z);
		push_f64(s1.dedge_z);
	}

	return true;
}

void GSRendererTile::SubmitNativeDraw(const GSTileDrawPlan& plan, const GSVector4i& r, const GIFRegTEX0& fixed_tex0,
	GSTexture* rt, GSTexture* ds, GSTexture* tex)
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
	// The vertex colour reaches the texture function on the SW scanline's own grid:
	// fixed point with seven fractional bits, truncated. Without this the GPU keeps
	// the interpolator's full precision and the UNORM8 store ROUNDS, which is half a
	// level of systematic bias on every Gouraud gradient — measured against the
	// gs-interp console capture, where the tile arm fitted `exact/round` on 99.5% of
	// readings while the scanline fitted a truncating ten-bit DDA. Flat draws are
	// unaffected: their colour is already an integer, so the truncation is identity.
	conf.ps.tile_vcolor = 1;
	conf.ps.dst_fmt = GSLocalMemory::m_psm[ctx->FRAME.PSM].fmt;
	conf.colormask = GSHWDrawConfig::ColorMaskSelector(plan.pass[0].colormask);
	if (!rt)
		conf.ps.DisableColorOutput();

	if (tex)
	{
		// The plain-color source: the backend's "index and AEM expansion already
		// done by the CPU" leg. Nearest draws sample it through the GPU sampler —
		// the exact configuration the gs-texture capture scored 128/128 on nearest
		// cells in every arm. Bilinear draws filter in-shader (PS_TILE_LTF): the
		// same capture measured the console's filter as a 1/16-texel truncating
		// snap with 4-bit nested truncating lerps, which no sampler expresses.
		conf.tex = tex;
		conf.vs.tme = 1;
		conf.vs.fst = PRIM->FST;
		conf.ps.fst = PRIM->FST;
		conf.ps.tfx = ctx->TEX0.TFX;
		conf.ps.tcc = ctx->TEX0.TCC;
		conf.sampler.biln = 0; // the GPU sampler stays nearest-only in every mode

		// Storage and wrap live at the SIZE-FIXED dimensions (what the source
		// builder deswizzled and what the SW scanline wraps at); the STQ
		// coordinate NUMERATOR keeps the register claim's scale, exactly the
		// split the SW renderer runs (ConvertVertexBuffer scales by the raw
		// claim, the wrap tables come from the fixed TEX0).
		const float tw = static_cast<float>(1 << std::min<u32>(fixed_tex0.TW, 10));
		const float th = static_cast<float>(1 << std::min<u32>(fixed_tex0.TH, 10));
		const float reg_tw = static_cast<float>(1u << ctx->TEX0.TW);
		const float reg_th = static_cast<float>(1u << ctx->TEX0.TH);
		const GSVector4 WH(tw, th, tw, th);
		conf.cb_ps.WH = WH;
		conf.cb_ps.HalfTexel = GSVector4(-0.5f, 0.5f).xxyy() / WH.zwzw();
		conf.cb_ps.STScale = GSVector2(1.0f, 1.0f);
		conf.cb_vs.texture_scale = GSVector2((1.0f / 16.0f) / tw, (1.0f / 16.0f) / th);

		const u8 wms = static_cast<u8>(ctx->CLAMP.WMS);
		const u8 wmt = static_cast<u8>(ctx->CLAMP.WMT);
		// The in-shader coordinate walk serves every bilinear draw, every
		// perspective STQ triangle (either filter), and every mip draw: the GPU
		// interpolator's own divide measurably flips texels against the
		// scanline's, so the fragment path recomputes trunc(s/q) at the
		// scanline's rounding — and mip adds per-pixel level selection and
		// bound shifts no sampler state expresses. Affine nearest without mip
		// stays on the GPU sampler, which the gs-texture capture proved exact.
		const bool mip_draw = IsMipMapActive();
		const bool stq_walk = !PRIM->FST && m_vt.m_primclass == GS_TRIANGLE_CLASS;
		if (mip_draw || m_vt.IsLinear() || stq_walk)
		{
			// texelFetch bypasses the sampler, so every wrap mode runs in the
			// shader on integer texel indices, one filter corner at a time. Region
			// bounds travel bit-cast as ints, pre-cooked exactly as the SW
			// scanline's setup cooks them: REGION_CLAMP bounds clamped into the
			// texture, REGION_REPEAT's MINU pre-masked and MAXU raw.
			bool ltf = m_vt.IsLinear();
			if (mip_draw)
			{
				// GetScanlineGlobalData's mip selector, verbatim: the min filter's
				// linear bit takes over when the whole draw minifies; round mode is
				// MMIN's low bit clear, trilinear set; a draw already past MXL
				// collapses to a constant max-level round-off; trilinear pulls the
				// clamp one 16.16 unit under MXL so level+1 always exists; UV
				// coordinates have no Q, so their LOD is the K constant.
				if (m_vt.m_lod.x > 0)
					ltf = (ctx->TEX1.MMIN >> 2) & 1;
				u32 mmin = (ctx->TEX1.MMIN & 1) + 1;
				u32 lcm = ctx->TEX1.LCM;
				int mxl16 = std::min<int>(static_cast<int>(ctx->TEX1.MXL), 6) << 16;
				int k16 = static_cast<int>(ctx->TEX1.K) << 12;
				if (static_cast<int>(m_vt.m_lod.x) >= static_cast<int>(ctx->TEX1.MXL))
				{
					k16 = static_cast<int>(m_vt.m_lod.x) << 16;
					lcm = 1;
					mmin = 1;
				}
				if (mmin == 2)
					mxl16--;
				if (PRIM->FST)
					lcm = 1;
				conf.ps.tile_mip = mmin;
				conf.ps.tile_lcm = lcm;
				if (lcm)
				{
					int lod = std::clamp(k16, 0, mxl16);
					if (mmin == 1)
						lod = static_cast<int>(static_cast<u32>(lod + 0x8000) & 0xffff0000u);
					// The packed constant IS the clamped 16.16 lod: the shader
					// splits it back into (level, fraction) bit-identically.
					const u32 lod_bits = static_cast<u32>(lod);
					std::memcpy(&conf.cb_ps.LODParams.w, &lod_bits, sizeof(lod_bits));
				}
				else
				{
					conf.cb_ps.LODParams.x = static_cast<float>(-(0x10000 << ctx->TEX1.L));
					conf.cb_ps.LODParams.y = static_cast<float>(k16);
					conf.cb_ps.LODParams.z = static_cast<float>(mxl16);
				}
			}
			conf.ps.tile_ltf = ltf;
			conf.ps.tile_nn = !ltf;
			conf.ps.wms = wms;
			conf.ps.wmt = wmt;
			if (!PRIM->FST)
			{
				// ti.zw feeds the in-shader quotient: SW's numerator space is the
				// register claim × 65536, so the varying carries S·16·reg_tw.
				conf.cb_vs.texture_scale = GSVector2((1.0f / 16.0f) / reg_tw, (1.0f / 16.0f) / reg_th);

				// The console's truncated reciprocal belongs to the draws where the
				// scanline actually divides per pixel. Outside a mipmapping draw the
				// scanline treats a sprite — and any primitive whose Q is constant —
				// as affine and divides once per vertex on the CPU, exactly
				// (GetScanlineGlobalData's sel.fst |= m_eq.q || sprite). Truncating
				// there would be console-accurate against a software renderer that
				// is not, which costs the parity bar and buys nothing measurable.
				const bool per_pixel_divide =
					mip_draw || (!m_vt.m_eq.q && m_vt.m_primclass != GS_SPRITE_CLASS);
				const u32 recip_mask = per_pixel_divide ? 0xfffffc00u : 0u;
				std::memcpy(&conf.cb_ps.TileSTQRecip, &recip_mask, sizeof(recip_mask));

				// Per-pixel MMAG/MMIN across the level-of-detail crossing, by the
				// scanline's rule and on the scanline's draws (sel.ltfx): lod is
				// -log2(Q)·2^L + K, so lod > 0 is exactly Q below one constant.
				//
				// Not inside a mipmapping draw. The scanline still resolves that
				// crossing per primitive and carries a TODO saying so, and matching
				// the software renderer is the bar — the console capture put its
				// crossover cases in non-mip modes and has not been asked about the
				// mip one, so going first here would be predicting, not measuring.
				//
				// ARM64-gated for the same reason the scanline is: only that codegen
				// emits the per-pixel choice, so on any other host the software
				// renderer decides per primitive and Tile has to agree with it.
#ifdef ARCH_ARM64
				if (!mip_draw && per_pixel_divide && m_vt.IsFilterCrossover())
				{
					const float K = static_cast<float>(ctx->TEX1.K) / 16.0f;

					conf.ps.tile_ltfx = m_vt.IsCrossoverLinearOnMag() ? 2 : 1;
					conf.cb_ps.TileLtfxQ =
						std::exp2(K / static_cast<float>(1 << ctx->TEX1.L));
				}
#endif
			}

			const int tw_i = static_cast<int>(tw);
			const int th_i = static_cast<int>(th);
			GSVector4i bounds = GSVector4i::zero();
			if (wms == CLAMP_REGION_CLAMP)
			{
				bounds.x = std::min(static_cast<int>(ctx->CLAMP.MINU), tw_i - 1);
				bounds.z = std::min(static_cast<int>(ctx->CLAMP.MAXU), tw_i - 1);
			}
			else if (wms == CLAMP_REGION_REPEAT)
			{
				bounds.x = static_cast<int>(ctx->CLAMP.MINU) & (tw_i - 1);
				bounds.z = static_cast<int>(ctx->CLAMP.MAXU);
			}
			if (wmt == CLAMP_REGION_CLAMP)
			{
				bounds.y = std::min(static_cast<int>(ctx->CLAMP.MINV), th_i - 1);
				bounds.w = std::min(static_cast<int>(ctx->CLAMP.MAXV), th_i - 1);
			}
			else if (wmt == CLAMP_REGION_REPEAT)
			{
				bounds.y = static_cast<int>(ctx->CLAMP.MINV) & (th_i - 1);
				bounds.w = static_cast<int>(ctx->CLAMP.MAXV);
			}
			conf.cb_ps.MinMax = GSVector4::cast(bounds);
		}
		else
		{
			conf.ps.wms = (wms & 2) ? wms : 0;
			conf.ps.wmt = (wmt & 2) ? wmt : 0;
			conf.sampler.tau = (wms == CLAMP_REPEAT);
			conf.sampler.tav = (wmt == CLAMP_REPEAT);
			if (!PRIM->FST)
			{
				// The sampler leg's st is normalized to the register claim; rescale
				// into the fixed-size source the sampler actually addresses.
				conf.cb_ps.STScale = GSVector2(reg_tw / tw, reg_th / th);
			}

			if ((wms | wmt) & 2)
			{
				// Region modes at native scale: the clamp bound sits half a texel inside
				// the last texel (inclusive bound, exclusive size); repeat passes the raw
				// mask/fix words through bit-cast floats. Same recipe as Classic at 1x.
				const GSVector4i clamp(ctx->CLAMP.MINU, ctx->CLAMP.MINV, ctx->CLAMP.MAXU, ctx->CLAMP.MAXV);
				const GSVector4 region_repeat = GSVector4::cast(clamp);
				const GSVector4 region_clamp = (GSVector4(clamp) + GSVector4::cxpr(0.5f, 0.5f, 0.1f, 0.1f)) / WH.xyxy();
				if (wms >= CLAMP_REGION_CLAMP)
				{
					conf.cb_ps.MinMax.x = (wms == CLAMP_REGION_CLAMP) ? region_clamp.x : region_repeat.x;
					conf.cb_ps.MinMax.z = (wms == CLAMP_REGION_CLAMP) ? region_clamp.z : region_repeat.z;
				}
				if (wmt >= CLAMP_REGION_CLAMP)
				{
					conf.cb_ps.MinMax.y = (wmt == CLAMP_REGION_CLAMP) ? region_clamp.y : region_repeat.y;
					conf.cb_ps.MinMax.w = (wmt == CLAMP_REGION_CLAMP) ? region_clamp.w : region_repeat.w;
				}
			}
		}
	}

	// Fog, at the rule the console capture fitted on all 192 of its readings:
	// (Ct·F + Cfog·(256−F)) >> 8 on the colour the texture function produced, alpha
	// left alone. The factor rides the existing per-vertex fog varying — flat across
	// a sprite (the expansion takes the second vertex, which is the one the scanline
	// reads) and interpolated across a triangle even when the shading is flat, which
	// is what the scanline does too.
	if (PRIM->FGE)
	{
		conf.ps.fog = 1;
		conf.ps.tile_fog = 1;
		const GSVector4 fc = GSVector4::rgba32(m_draw_env->FOGCOL.U32[0]);
		conf.cb_ps.FogColor_AREF = fc.blend32<8>(conf.cb_ps.FogColor_AREF);
	}

	// Depth state. A draw carrying a walk payload stores and compares the SW
	// scanline's depth, computed by the fragment from the per-primitive plane
	// (PS_TILE_ZWALK) — that subsumes the zfloor/zclamp recipe (the walk truncates
	// and clamps in integer arithmetic), and it takes depth off the interpolant
	// entirely, so the 1/320-px coverage nudge in the vertex shader no longer
	// touches it (the d586cb2c25 dilemma — nudge load-bearing for colour, poison
	// for depth — dissolves here). Everything else keeps EmulateZbuffer's recipe:
	// clamp to the format's max after rasterization, floor the interpolated z to
	// an integer where one z unit is finer than a float32 ULP.
	const bool zwalk = ds && m_zwalk_at != NoWalk;
	conf.tile_zwalk_at = NoWalk;
	conf.tile_stq_at = NoWalk;
	if (!m_tile_payload.empty())
	{
		conf.tile_payload = m_tile_payload.data();
		conf.tile_payload_size = static_cast<u32>(m_tile_payload.size() / 4);
		conf.vs.tile_prim_ord = 1;
		if (m_stq_at != NoWalk)
		{
			conf.ps.tile_stq = 1;
			conf.tile_stq_at = m_stq_at;
		}
	}
	conf.depth.ztst = ds ? plan.ztst : static_cast<u8>(ZTST_ALWAYS);
	conf.depth.zwe = plan.pass[0].z_write;
	conf.cb_vs.max_depth = 0xFFFFFFFFu;
	if (zwalk)
	{
		conf.ps.tile_zwalk = 1;
		conf.tile_zwalk_at = m_zwalk_at;
	}
	else if (ds)
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

	// The alpha test, as the lowering realized it. A pass that carries a comparison
	// discards its failing fragments outright (PS_AFAIL::KEEP) — the shader's other
	// AFAIL modes all read the destination, which is M4 machinery; the lowering
	// expresses the AFAIL write edits as the second pass's masks instead.
	if (plan.pass[0].atst != ATST_ALWAYS)
	{
		GSHWDrawConfig::PS_ATST ps_atst = GSHWDrawConfig::PS_ATST::NONE;
		float ps_aref = 0.0f;
		GSHWDrawConfig::GetAlphaTestPS(plan.pass[0].atst, plan.pass[0].aref, false, ps_atst, ps_aref);
		conf.ps.atst = ps_atst;
		conf.cb_ps.FogColor_AREF.a = ps_aref;
		conf.ps.afail = GSHWDrawConfig::PS_AFAIL::KEEP;
	}
	if (plan.pass_count > 1)
	{
		conf.alpha_second_pass.ps = conf.ps;
		conf.alpha_second_pass.colormask = GSHWDrawConfig::ColorMaskSelector(plan.pass[1].colormask);
		conf.alpha_second_pass.depth = conf.depth;
		conf.alpha_second_pass.depth.zwe = plan.pass[1].z_write;
		conf.alpha_second_pass.ps_aref = conf.cb_ps.FogColor_AREF.a;
		if (plan.pass[1].atst != ATST_ALWAYS)
		{
			GSHWDrawConfig::PS_ATST ps_atst = GSHWDrawConfig::PS_ATST::NONE;
			float ps_aref = 0.0f;
			GSHWDrawConfig::GetAlphaTestPS(plan.pass[1].atst, plan.pass[1].aref, false, ps_atst, ps_aref);
			conf.alpha_second_pass.ps.atst = ps_atst;
			conf.alpha_second_pass.ps_aref = ps_aref;
			conf.alpha_second_pass.ps.afail = GSHWDrawConfig::PS_AFAIL::KEEP;
		}
		else
		{
			conf.alpha_second_pass.ps.atst = GSHWDrawConfig::PS_ATST::NONE;
		}
		if (plan.pass[1].colormask == 0)
			conf.alpha_second_pass.ps.DisableColorOutput();
		conf.alpha_second_pass.enable = true;
	}

	switch (m_vt.m_primclass)
	{
		case GS_TRIANGLE_CLASS:
			// The walks' ordinal comes from gl_VertexIndex, so a draw carrying either
			// plane needs its indices to be the identity mapping. De-indexing
			// preserves primitive order, which is what keeps ordinal p pointing at
			// the p-th payload block. (Sprites need none of this: their expansion
			// reads the vertex pairs directly and the ordinal falls out of the same
			// arithmetic.)
			if (conf.vs.tile_prim_ord)
				DeindexVertices();
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
	rec.alpha_fix = static_cast<u8>(ALPHA.FIX);
	rec.atst = static_cast<u8>(TEST.ATST);
	rec.afail = static_cast<u8>(TEST.AFAIL);
	rec.aref = static_cast<u8>(TEST.AREF);
	rec.datm = static_cast<u8>(TEST.DATM);

	rec.flags = static_cast<u8>((PRIM->TME ? GSDrawLog::FlagTextured : 0) |
								(PRIM->ABE ? GSDrawLog::FlagBlend : 0) |
								(TEST.ATE ? GSDrawLog::FlagAlphaTest : 0) |
								(TEST.DATE ? GSDrawLog::FlagDate : 0) |
								(TEST.ZTE ? GSDrawLog::FlagZTest : 0) |
								(m_context->ZBUF.ZMSK ? GSDrawLog::FlagZMask : 0));

	const GIFRegTEX1& TEX1 = m_context->TEX1;
	rec.tex_filter = static_cast<u8>((TEX1.MMAG & 1) | ((TEX1.MMIN & 7) << 1) | ((TEX1.MXL & 7) << 4));
	// m_draw_env, not m_env: under the split parser the front object has already run
	// ahead, so m_env is a later frame's answer (the SW recorder's rule, shared here).
	rec.env = static_cast<u8>((PRIM->FGE ? 1 : 0) | (PRIM->FST ? 2 : 0) |
							  ((m_context->TEX0.TCC & 1) << 2) | ((m_context->TEX0.TFX & 3) << 3) |
							  ((m_draw_env->COLCLAMP.CLAMP & 1) << 5) | ((m_draw_env->PABE.PABE & 1) << 6) |
							  (PRIM->AA1 ? 0x80 : 0));
	rec.env2 = static_cast<u8>((m_draw_env->DTHE.DTHE & 1) | (PRIM->IIP ? 2 : 0) |
							   ((m_context->FBA.FBA & 1) << 2));

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
		GSDrawLog::NoteTileDraw(/*memo_hit=*/false, record_ns, /*pass_id=*/0, MapFallbackReason(reason), r,
			plan.stq_guard);
		GSDrawLog::FinishDraw();
	}
}
