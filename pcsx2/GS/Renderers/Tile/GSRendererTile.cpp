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

GSRendererTile::GSRendererTile(int threads)
	: GSRendererSW(threads)
{
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
	// Every draw takes the floor today, and the ledger row exists to measure exactly
	// that: the fallback column is the per-title floor-rate instrument. record_ns
	// covers only the bookkeeping (currently just this row; later the draw key and
	// page sets), never the draw execution itself.
	if (GSDrawLog::IsActive()) [[unlikely]]
	{
		const u64 record_start = Common::Timer::GetCurrentValue();
		RecordFloorDrawLogEntry();
		const u32 record_ns = static_cast<u32>(
			Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - record_start));

		GSRendererSW::Draw();

		GSDrawLog::NoteTileDraw(/*memo_hit=*/false, record_ns, /*pass_id=*/0, GSDrawLog::TileFallbackFloor);
		GSDrawLog::FinishDraw();
		return;
	}

	GSRendererSW::Draw();
}
