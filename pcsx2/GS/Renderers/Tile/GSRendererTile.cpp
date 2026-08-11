// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSRendererTile.h"

#include "GS/Renderers/HW/GSDrawLog.h"
#include "GS/GSPerfMon.h"

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
} // namespace

GSRendererTile::GSRendererTile(int threads)
	: GSRendererSW(threads)
{
}

void GSRendererTile::Reset(bool hardware_reset)
{
	GSRendererSW::Reset(hardware_reset);
	// Everything falls back to CPU-newest; surface textures die with the reset.
	m_vram_model.Reset();
}

void GSRendererTile::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// The model's invariants are cheap enough to police once a frame in Devel.
	pxAssert(m_vram_model.CheckInvariants());
	GSRendererSW::VSync(field, registers_written, idle_frame);
}

// A transfer wrote CPU local memory: shrink or clear GPU truth under it. The planes
// follow byte coverage (gsTilePlanesInvalidatedByWrite), and the rect footprint lets
// edge pages shrink block-wise instead of spilling.
void GSRendererTile::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	GSRendererSW::InvalidateVideoMem(BITBLTBUF, r);

	const GSTileSurfaceLayout layout{BITBLTBUF.DBP, static_cast<u8>(BITBLTBUF.DBW),
		static_cast<u8>(BITBLTBUF.DPSM), KindForPsm(BITBLTBUF.DPSM)};
	GSVramModel::FootprintForRect(layout, r, m_rect_fp);
	const u8 planes = gsTilePlanesInvalidatedByWrite(BITBLTBUF.DPSM);

	const GSPageBitmap need = m_vram_model.SpillBeforeCpuWrite(m_rect_fp, planes);
	// No native draw exists yet, so nothing can be GPU-truth; the readback that
	// makes a sub-block overwrite lossless lands with the native path.
	pxAssert(need.empty());
	(void)need;

	m_vram_model.OnCpuWrite(m_rect_fp, planes);
}

// The CPU is about to read this region out of local memory: any page whose newest
// bytes live on the GPU would have to come back first.
void GSRendererTile::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	const GSTileSurfaceLayout layout{BITBLTBUF.SBP, static_cast<u8>(BITBLTBUF.SBW),
		static_cast<u8>(BITBLTBUF.SPSM), KindForPsm(BITBLTBUF.SPSM)};
	GSVramModel::FootprintForRect(layout, r, m_rect_fp);

	const GSPageBitmap need = m_vram_model.ReadbackNeeded(m_rect_fp, kGSTilePlanesAll);
	pxAssert(need.empty()); // no native draw exists yet; M2d pulls + OnReadback here
	(void)need;

	GSRendererSW::InvalidateLocalMem(BITBLTBUF, r, clut);
}

// A local->local copy is a CPU read of the source and a CPU write of the destination
// as far as the model is concerned (the floor executes it against CPU truth).
void GSRendererTile::Move()
{
	GSRendererSW::Move();

	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;
	const GSVector4i src_r(m_env.TRXPOS.SSAX, m_env.TRXPOS.SSAY, m_env.TRXPOS.SSAX + w, m_env.TRXPOS.SSAY + h);
	const GSVector4i dst_r(m_env.TRXPOS.DSAX, m_env.TRXPOS.DSAY, m_env.TRXPOS.DSAX + w, m_env.TRXPOS.DSAY + h);

	const GSTileSurfaceLayout src_l{m_env.BITBLTBUF.SBP, static_cast<u8>(m_env.BITBLTBUF.SBW),
		static_cast<u8>(m_env.BITBLTBUF.SPSM), KindForPsm(m_env.BITBLTBUF.SPSM)};
	GSVramModel::FootprintForRect(src_l, src_r, m_rect_fp);
	pxAssert(m_vram_model.ReadbackNeeded(m_rect_fp, kGSTilePlanesAll).empty());

	const GSTileSurfaceLayout dst_l{m_env.BITBLTBUF.DBP, static_cast<u8>(m_env.BITBLTBUF.DBW),
		static_cast<u8>(m_env.BITBLTBUF.DPSM), KindForPsm(m_env.BITBLTBUF.DPSM)};
	GSVramModel::FootprintForRect(dst_l, dst_r, m_rect_fp);
	const u8 planes = gsTilePlanesInvalidatedByWrite(m_env.BITBLTBUF.DPSM);
	pxAssert(m_vram_model.SpillBeforeCpuWrite(m_rect_fp, planes).empty());
	m_vram_model.OnCpuWrite(m_rect_fp, planes);
}

// The floor draw's write footprint, observed into the memory model. The rect mirrors
// the SW rasterizer's own bound derivation (bbox rounding by prim class, AA1
// expansion, exclusive edges, scissor intersect). Claims are conservative on purpose:
// over-claiming a write costs a wasted spill once truth can exist, never bytes,
// because the flow reads back before it clears. Coverage inside the bbox is not
// rectangular (triangles), so draws use the page-granular flow — block-wise shrink is
// reserved for true rect writers (transfers, moves).
void GSRendererTile::ObserveFloorDraw()
{
	const GSDrawingContext* context = m_context;

	GSVector4i bbox;
	if (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_POINT_CLASS)
		bbox = GSVector4i((m_vt.m_min.p + GSVector4(0.5f)).floor().upld((m_vt.m_max.p + GSVector4(0.5f)).floor()));
	else
		bbox = GSVector4i(m_vt.m_min.p.ceil().upld(m_vt.m_max.p.floor()));
	if (PRIM->AA1 && (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_TRIANGLE_CLASS))
		bbox += GSVector4i(-1, -1, 1, 1);
	bbox += GSVector4i(0, 0, 1, 1);

	const GSVector4i r = bbox.rintersect(context->scissor.in);
	if (r.rempty())
		return;

	// No native draw exists yet, so no page can be GPU-truth. The native path
	// replaces this assert with the spill flow (ReadbackNeeded over the write and
	// read footprints, pull, OnReadback) before the floor rasterizes.
	pxAssert(m_vram_model.TruthAny().empty());

	u8 fb_planes = gsTilePlanesInvalidatedByWrite(context->FRAME.PSM);
	const u32 fbmsk = context->FRAME.FBMSK;
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
		const GSTileSurfaceLayout fb_l{context->FRAME.Block(), static_cast<u8>(context->FRAME.FBW),
			static_cast<u8>(context->FRAME.PSM), GSTileSurfaceKind::Color};
		m_vram_model.OnCpuWrite(GSVramModel::PagesForRect(fb_l, r), fb_planes);
	}

	if (!context->ZBUF.ZMSK)
	{
		const GSTileSurfaceLayout z_l{context->ZBUF.Block(), static_cast<u8>(context->FRAME.FBW),
			static_cast<u8>(context->ZBUF.PSM), GSTileSurfaceKind::Depth};
		m_vram_model.OnCpuWrite(GSVramModel::PagesForRect(z_l, r), gsTilePlanesInvalidatedByWrite(context->ZBUF.PSM));
	}
}

// The register view of the row, from the raw draw context. The Classic recorder
// (GSRendererHW::RecordDrawLogEntry) reads m_cached_ctx because that is what its draw
// consumes; the floor executes against the uncooked context, so that is what gets
// recorded here.
void GSRendererTile::RecordFloorDrawLogEntry() const
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
	// Every draw takes the floor today, and the ledger row measures exactly that:
	// the fallback column is the per-title floor-rate instrument. record_ns covers
	// the bookkeeping — the ledger row and the model observation (footprints +
	// flows; later the draw key too) — never the draw execution itself.
	if (GSDrawLog::IsActive()) [[unlikely]]
	{
		const u64 record_start = Common::Timer::GetCurrentValue();
		RecordFloorDrawLogEntry();
		ObserveFloorDraw();
		const u32 record_ns = static_cast<u32>(
			Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - record_start));

		GSRendererSW::Draw();

		GSDrawLog::NoteTileDraw(/*memo_hit=*/false, record_ns, /*pass_id=*/0, GSDrawLog::TileFallbackFloor);
		GSDrawLog::FinishDraw();
		return;
	}

	ObserveFloorDraw();
	GSRendererSW::Draw();
}
