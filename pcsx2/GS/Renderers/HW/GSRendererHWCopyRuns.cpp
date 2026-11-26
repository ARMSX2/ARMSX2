// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Copy runs: the copy road's palette block copies, taken one after another without Draw().
//
// Games that expand paletted textures on the GS (Call of Duty 2 and 3) draw thousands of one-sprite copies a frame,
// nearly all into the same frame under the same registers, each with its own texture address and palette offset.
// Each one is a whole PS2 draw: Draw()'s prelude, the copy road's decision and the scanline setup, for an 8x8 tile.
//
// The first copy of a run goes through Draw() in full. When the copy road takes it with the palette block copy, it
// arms a run (CopyRunArm()): the registers the draw was decided under, with TEX0's texture address (TBP0), palette
// address (CBP), palette offset (CSA) and palette load control (CLD) cleared, are the run's key. While the run is
// armed the draw executor gives each draw to ContinueCopyRun() instead of its tail. A draw whose registers match the
// key, and whose texture address has no target (looked up only when the address changes, as the decision does),
// is copied there with the setup the arming copy left (GSSwPrimRenderFunctions::ContinuePaletteBlocks()), and the
// bookkeeping the tail, Draw() and the copy road do for it is done in the same order. Any other draw, and any draw
// the palette block copy would not take, goes to the tail as before, and Draw() ends the run.
//
// Why a continuing copy is the copy Draw() would make. What Draw() decides before the copy road reads the
// registers in the key, the vertex trace (computed here as the tail computes it), the texture cache's targets,
// renderer state that only Draw() changes, and settings. The run is armed only under registers for which the
// vertex-dependent parts of that are settled by the registers alone (CopyRunArm() lists them), and the parts
// that read the vertices and are not (the draw rectangle, the filter, the decal, notest) are evaluated per copy,
// as Run() evaluates them. The decision's target lookups: the frame is looked at only for a difficult frame mask
// or a blend, which the key rules out, and the texture is looked up again whenever its address changes. The set
// of targets changes only in a draw that is not a continuing copy (which ends the run), in an upload, readback or
// move, a vsync, a purge or a reset (each of which ends it), and in the invalidation after a copy, which can only
// remove a target or change one that reaches the copy's frame; a copy whose frame a target may reach makes the
// next copy look its texture up again. Renderer state Draw() writes for a copy is either written here as Draw()
// writes it (the draw rectangle, the cached TEX0, the cover test) or is what the arming copy left, unchanged since,
// since nothing but Draw() writes it.
//
// What is not taken: the split front (the hook is on the single object's executor), the burst memo while it is not
// resting, and every debugging aid that watches draws (dumps, labels, the draw log).

#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/HW/GSDrawLog.h"
#include "GS/Renderers/HW/GSTextureCache.h"
#include "GS/GSPerfMon.h"

#include <cstring>
#include <iterator>

void GSRendererHW::MakeCopyRunKey(CopyRunKey& key, const GSDrawingEnvironment& env, const GSDrawingContext& ctx)
{
	GIFRegTEX0 tex0 = ctx.TEX0;
	tex0.TBP0 = 0;
	tex0.CBP = 0;
	tex0.CSA = 0;
	tex0.CLD = 0;

	u64* r = key.regs;
	r[0] = env.PRIM.U64;
	r[1] = ctx.FRAME.U64;
	r[2] = ctx.ZBUF.U64;
	r[3] = ctx.TEST.U64;
	r[4] = ctx.ALPHA.U64;
	r[5] = ctx.CLAMP.U64;
	r[6] = ctx.TEX1.U64;
	r[7] = ctx.SCISSOR.U64;
	r[8] = ctx.XYOFFSET.U64;
	r[9] = ctx.FBA.U64;
	r[10] = tex0.U64;
	r[11] = env.TEXA.U64;
	r[12] = env.DTHE.U64;
	r[13] = env.COLCLAMP.U64;
	r[14] = env.PABE.U64;
	r[15] = env.FOGCOL.U64;
	r[16] = env.SCANMSK.U64;
	r[17] = env.DIMX.U64;
	r[18] = env.TEXCLUT.U64;
}

bool GSRendererHW::SetDrawRect()
{
	m_r = GSVector4i((m_vt.m_min.p.upld(m_vt.m_max.p) + GSVector4::cxpr(0.4f)).round<Round_NearestInt>());
	m_r = m_r.blend8(m_r + GSVector4i::cxpr(0, 0, 1, 1), (m_r.xyxy() == m_r.zwzw()));
	m_r_no_scissor = m_r;
	m_r = m_r.rintersect(m_context->scissor.in);
	return !m_r.rempty();
}

// After a palette block copy Draw() took through the copy road. The run is armed only where nothing Draw() decides
// before the copy can depend on a draw's vertices beyond what ContinueCopyRun() evaluates per copy:
// - a textured UV sprite with no blending and no antialiasing: no texel rounding (it applies to ST), the opaque and
//   no-blend answers, and the colour mask, from the registers;
// - no alpha test and no destination alpha test: no alpha range read, from the vertices or the palette;
// - no depth test that can fail (none, or always): the depth answers from the registers;
// - no mipmapping possible under these registers: IsMipMapActive() reads the vertices' level of detail;
// - the copy road's verdict found no target at the texture and did not look at the frame;
// - nothing that changes or watches the draw: no game hook, no draw skipping, no dumps, labels or draw log, no
//   rewrite of large ST, no palette taken from a GPU target, no colour clip texture to resolve, no shuffle.
void GSRendererHW::CopyRunArm()
{
	const GSDrawingContext& ctx = *m_context;
	const GIFRegPRIM& prim = *PRIM;
	const bool mip_possible = m_mipmap && ctx.TEX1.MXL > 0 && ctx.TEX1.MMIN >= 2 && ctx.TEX1.MMIN <= 5;
	if (!prim.TME || !prim.FST || prim.ABE || prim.AA1 || GSUtil::GetPrimClass(prim.PRIM) != GS_SPRITE_CLASS ||
		ctx.TEST.ATE || ctx.TEST.DATE || (ctx.TEST.ZTE && ctx.TEST.ZTST != ZTST_ALWAYS) || mip_possible ||
		!m_copy_road_no_targets || m_copy_road_frame_checked || m_gsc || m_skip != 0 ||
		GSConfig.SkipDrawEnd != 0 || GSConfig.DumpGSData || GSConfig.DebugLabels || GSDrawLog::IsActive() ||
		GSConfig.UserHacks_RewriteLargeSTCoords || GSConfig.UserHacks_GPUTargetCLUTMode != GSGPUTargetCLUTMode::Disabled ||
		m_mem.m_clut.GetGPUTexture() || g_gs_device->GetColorClipTexture() || m_channel_shuffle ||
		m_split_texture_shuffle_pages)
		return;

	MakeCopyRunKey(m_copy_run_key, *m_draw_env, ctx);
	m_copy_run_tbp0 = FrameMayReachTarget(GSSwPrimRenderBBox(m_vt, ctx.scissor.in)) ? ~0u : ctx.TEX0.TBP0;
	m_copy_run_armed = true;
}

// Whether the copy road's invalidation over `bbox` in the frame may have changed a target, and with it the
// decision's answer at a texture address it found free.
//
// The pages are found from the page grid alone, a range that holds the one GetStartBlockAddress() and
// GetUnwrappedEndBlockAddress() give: a pixel's block is the base plus its page's offset plus a block within the page,
// which carries into the next page only when the base is not on a page boundary.
bool GSRendererHW::FrameMayReachTarget(const GSVector4i& bbox) const
{
	const GSOffset& fb = m_context->offset.fb;
	const GSVector2i pgs = GSLocalMemory::m_psm[fb.psm()].pgs;
	const u32 bw = fb.bw();
	const u32 first = (fb.bp() >> 5) + static_cast<u32>(bbox.top / pgs.y) * bw + static_cast<u32>(bbox.left / pgs.x);
	const u32 last = bbox.rempty() ? first + 1 :
		((fb.bp() + 31) >> 5) + static_cast<u32>((bbox.bottom - 1) / pgs.y) * bw + static_cast<u32>((bbox.right - 1) / pgs.x);
	return GSTextureCache::TargetsMayReach(first << 5, last << 5);
}

bool GSRendererHW::ContinueCopyRun()
{
	GSVector4i bbox;
	if (!PrepareCopyRunCopy(bbox) || !CopyRunCopy(*this, bbox, true))
		return false;
	FinishCopyRunCopy(bbox);
	return true;
}

bool GSRendererHW::PrepareCopyRunCopy(GSVector4i& bbox)
{
	const GSDrawingContext* context = m_context;

	// The burst memo leaves the draw alone (SkipMemoizedDraw() and BurstMemoBeforeCopy() would do nothing).
	if (m_burst_memo_enabled && (!m_burst_memo.resting || m_burst_memo.candidate || BurstMemoWakes()))
		return false;

	CopyRunKey key;
	MakeCopyRunKey(key, *m_draw_env, *context);
	u64 differ = 0;
	for (size_t i = 0; i < std::size(key.regs); i++)
		differ |= key.regs[i] ^ m_copy_run_key.regs[i];
	if (differ || GSDrawLog::IsActive())
		return false;

	// The decision's lookup at the texture, when its address is not the one last found free.
	if (HasDeferredScratchAt(context->TEX0.TBP0))
		return false;
	if (context->TEX0.TBP0 != m_copy_run_tbp0)
	{
		if (g_texture_cache->GetTargetWithSharedBits(context->TEX0.TBP0, context->TEX0.PSM))
			return false;
		m_copy_run_tbp0 = context->TEX0.TBP0;
	}

	// The tail's vertex trace, then what Draw() works out for a copy before the copy road. Each is recomputed by
	// the tail and Draw() if the draw goes to them after all.
	m_vt.Update(m_vertex->buff, m_index->buff, m_vertex->tail, m_index->tail, GS_SPRITE_CLASS);
	m_cached_ctx.TEX0 = context->TEX0;
	if (!SetDrawRect())
		return false;

	// The copy road: the held words under what it reads and writes, then the copy, which refuses (having written
	// nothing) a draw Run() would have given to the rasterizer.
	MaterialiseHeldUnderCopy();
	bbox = GSSwPrimRenderBBox(m_vt, context->scissor.in);
	return true;
}

void GSRendererHW::FinishCopyRunCopy(const GSVector4i& bbox)
{
	if (FrameMayReachTarget(bbox))
		m_copy_run_tbp0 = ~0u;

	// What Draw() does after the copy, then the tail around Draw().
	m_draw_color_dispatch_accepted = false;
	BurstMemoAfterCopy(false, true);
	NoteDisplaySpriteBlit();
	m_env.CTXT[PRIM->CTXT].UpdateScissor();
	m_quad_check_valid = false;
	m_quad_check_valid_shuffle = false;
	m_drawlist.clear();
	m_drawlist_bbox.clear();
	g_perfmon.Put(GSPerfMon::Draw, 1);
	g_perfmon.Put(GSPerfMon::Prim, m_index->tail / 2);
	m_copy_run_copies++;
}
