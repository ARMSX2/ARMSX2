// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GSRendererTileGpu.h"

#include "GS/Renderers/Tile/GSTileTypes.h"
#include "GS/GSLocalMemory.h"

#include "common/Console.h"
#include "common/Assertions.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

namespace
{
// Color/depth kind from the swizzle family. The Tile renderer's KindForPsm is a file-local
// helper (anonymous namespace) there, not exported — replicated rather than linked.
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

// A zero stride degenerates every row of the page derivation; claim the whole page space
// rather than under-claim (the same conservative choice the Tile renderer's wrapper makes).
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

// The bytes of a 32-bit cell one plane's data occupies under one layout — what a readback of
// that plane may write into CPU local memory (the Tile renderer's PlaneByteMask, replicated).
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

// Whether the target pool can back a layout: every pool format has 64-pixel-wide pages
// (the 32/16-bit colour and depth families); the 8/4-bit index formats do not, and a zero
// stride has no page geometry at all.
bool PoolSupports(const GSTileSurfaceLayout& layout)
{
	return layout.bw != 0 && gsTileStorageBpp(layout.psm) >= 16;
}

// Whether a surface can travel the byte road (writeback / seed): a colour surface in the CT32
// swizzle universe with a page-aligned base -- the two shaders address it with the CT32 forms
// and pool page (row, col) == physical page base + row*bw + col. Depth and 16-bit surfaces
// exist in the model but have no road yet: truth that moves through them is counted lossy.
bool HasByteRoad(const GSTileSurfaceLayout& layout)
{
	return layout.kind == GSTileSurfaceKind::Color && (layout.psm == PSMCT32 || layout.psm == PSMCT24) &&
		   (layout.bp & 31) == 0;
}
} // namespace

// Stage-1 pin: the back-record/back-thread machinery stays off for this variant whatever
// GSBackThreadMode says — its own front end is the threading story (revisited only if a
// measurement asks).
GSRendererTileGpu::GSRendererTileGpu()
	: GSRenderer(/*allow_back_records=*/false)
{
	// The pass sim is a cross-check, not a planner input: nothing the executor consumes is
	// derived from it. So it is a lever, read once here like the Tile renderer reads its own --
	// a mid-run flip needs a renderer restart.
	m_pass_sim.SetActive(GSConfig.TileGpuPassSim);

	// Asked once, before any draw: rule 2 is served by work this renderer DOES NOT DO, so a device
	// that cannot bind targets has to be known before the first read window is (not) composed.
	m_bindless_targets = g_gs_device && g_gs_device->TileGpuBindlessTargets();
}

GSRendererTileGpu::~GSRendererTileGpu()
{
	ReportPassStructure();
}

void GSRendererTileGpu::Destroy()
{
	// Textures must go back to the device pool while the device is alive; the pool's
	// destructor only checks that they did.
	m_target_pool.ReleaseAll();
	GSRenderer::Destroy();
}

void GSRendererTileGpu::Reset(bool hardware_reset)
{
	GSRenderer::Reset(hardware_reset);
	// Everything falls back to CPU-newest (the reset rewrote local memory); surface textures
	// die with it, and so does the frame in progress.
	m_target_pool.ReleaseAll();
	m_vram_model.Reset();
	m_surface_texels.clear();
	m_plan_vertices.clear();
	m_plan_indices.clear();
	m_plan_states.clear();
	m_plan_palettes.clear();
	m_plan_draws.clear();
	m_plan_topologies.clear();
	m_plan_blend_keys.clear();
	m_plan_tex_slots.clear();
	m_plan_pending.clear();
	m_plan_passes.clear();
	m_plan_target_pairs.clear();
	m_plan_snapshots.clear();
	m_plan_prep_ops.clear();
	m_plan_page_entries.clear();
	m_plan_targets.clear();
	m_plan_tex_sources.clear();
	m_plan_target_surfaces.clear();
	m_plan_target_of_surface.clear();
	m_ring_entries.clear();
	m_ring_versions.clear();
	m_ring_live.fill(0);
	m_epoch = 0;
	// The surfaces the open pass named died with the model, so its ids would alias whatever takes
	// them next.
	BreakOpenPass();
}

void GSRendererTileGpu::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// Close the frame's open pass and push its stats before the base presents (the order the
	// Tile renderer's observer uses — the model is independent of presentation, but the frame
	// boundary is here).
	if (m_pass_sim.IsActive()) [[unlikely]]
		m_pass_sim.OnVSync();

	// Structure the frame's accumulated draws into a pass plan and render it through the
	// device executor, before the base presents — so the targets the executor drew into are
	// ready when the base's present path calls GetOutput. The frame boundary itself syncs
	// nothing: targets persist, and the ring the plan build consumed is gone (every synced
	// claim with it -- BuildAndExecutePlan drops them).
	MaterialiseDisplayBuffers();
	BuildAndExecutePlan();

	m_frame.surfaces_live = m_vram_model.LiveSurfaces();
	m_model_frames.push_back(m_frame);
	m_frame = ModelFrame{};

	GSRenderer::VSync(field, registers_written, idle_frame);

	// Nothing consumes the transfer log yet; clear it so it cannot grow without bound.
	m_draw_transfers.clear();
}

void GSRendererTileGpu::Draw()
{
	if (m_pass_sim.IsActive()) [[unlikely]]
		ObserveDraw();
	AccumulateDraw();
}

// Every enabled display buffer's texture must hold the display rect before the frontend reads it.
// Two ways it can fail to: a page nothing has ever drawn into this surface (the texture rectangle
// is allocated whole, its untouched rows hold whatever the allocator left), and a page some other
// surface has taken byte truth of since (the texels here are stale even though the bytes are
// perfectly accounted for). Both are invisible to the memory model, which is about bytes, and the
// present is the one road that reads texels instead.
//
// The repair is an ordinary seed, appended to the frame's plan as a pending draw with no geometry:
// it carries the prep ops and nothing else -- the writebacks that compose the pages' bytes into the
// ring, then the seed that reads them into the texture -- so the pass runs those and draws zero
// indices. It also gives a display buffer that only the CPU ever writes -- an FMV, a menu blitted in
// by transfers -- its first surface, which is why the surface is created here rather than looked up.
//
// Fired from VSync after every draw of the frame has been accumulated and before the plan is built,
// so the pseudo-draw is always last and always breaks its pass: the writebacks below are hoisted
// over nothing but the seed they feed, and the collision test the mid-frame road needs
// (WritebackHoistCollides) has nothing to decide here.
void GSRendererTileGpu::MaterialiseDisplayBuffers()
{
	for (int i = 0; i < 2; i++)
	{
		const auto& fb = PCRTCDisplays.PCRTCDisplays[i];
		if (!fb.enabled || fb.FBW == 0)
			continue;
		const GSVector2i size = PCRTCDisplays.GetFramebufferSize(i);
		if (size.x <= 0 || size.y <= 0)
			continue;

		const GSTileSurfaceLayout disp_l{static_cast<u32>(fb.Block()), static_cast<u8>(fb.FBW),
			static_cast<u8>(fb.PSM), GSTileSurfaceKind::Color};
		if (!PoolSupports(disp_l) || !HasByteRoad(disp_l))
			continue;
		const GSVector4i rect(0, 0, size.x, size.y);
		const GSPageBitmap pages = GSVramModel::PagesForRect(disp_l, rect);
		const GSTileSurfaceId id = EnsureSurface(disp_l, rect, pages);
		if (id == kGSTileNoSurface)
			continue;

		// Pages the texture spans but does not hold: never filled, or filled and since superseded.
		GSPageBitmap need = PagesNeedingSeed(id, pages, kGSTilePlanesColor) | pages.andnot(Texels(id).filled);
		need &= Texels(id).pages;
		if (need.empty())
			continue;

		// The op range opens BEFORE the compose, not between it and the seed. The executor runs
		// exactly the ops each pass names -- [first_prep_op, +prep_op_count) of the pass, which
		// BuildAndExecutePlan takes from its draws -- so an op emitted before this pseudo-draw's
		// range starts is inside no pass's range at all, and is silently never run. That is what
		// the writebacks ComposeRingPages emits here were: dropped, leaving the seed below to read
		// ring slots holding nothing but their S prefill while the model counted the bytes
		// composed. Opening the range first puts the compose and the seed in one range, in the
		// order they must run.
		PendingDraw pd = {};
		pd.tex_source = kGSTileNoSurface;
		pd.tex_slot = GSDevice::GSTileGpuPassPlan::kNoTexSlot;
		pd.first_prep_op = static_cast<u32>(m_plan_prep_ops.size());
		ComposeRingPages(need);
		EmitPrepOp(GSDevice::GSTileGpuPrepKind::Seed, id, need);
		pd.prep_op_count = static_cast<u32>(m_plan_prep_ops.size()) - pd.first_prep_op;
		if (pd.prep_op_count == 0)
			continue; // nothing was emitted at all, so this bail strands nothing
		Texels(id).filled |= need;
		m_frame.seed_breaks++;

		pd.color_surface = id;
		pd.z_surface = kGSTileNoSurface;
		pd.break_before = true; // the seed must land after every draw of the frame
		pd.rect = rect;
		pd.scissor = rect;
		pd.epoch = m_epoch;

		GSDevice::GSTileGpuIndirectDraw draw = {};
		draw.instance_count = 1;
		draw.index_count = 0; // the pass exists for its prep ops
		draw.first_index = static_cast<u32>(m_plan_indices.size());
		draw.vertex_offset = static_cast<s32>(m_plan_vertices.size());
		draw.state_index = static_cast<u32>(m_plan_draws.size());
		pd.draw_index = draw.state_index;
		m_plan_draws.push_back(draw);
		m_plan_topologies.push_back(GSDevice::GSTileGpuTopology::Triangle);
		m_plan_blend_keys.push_back(0);
		m_plan_pending.push_back(pd);

		// This pseudo-draw broke the pass, so the tracking that answers "what has the open pass
		// already done" must reset with it, exactly as AccumulateDraw's breaks do. Nothing reads
		// it between here and BuildAndExecutePlan's own reset, so it cannot be observed today --
		// it keeps the reset-on-break invariant true for whatever reads it next.
		BreakOpenPass();
	}
}

GSTexture* GSRendererTileGpu::GetOutput(int i, float& scale, int& y_offset)
{
	// The display buffer is whichever colour surface holds the PCRTC display base's pages: the
	// exact display layout first (the composite draws it under the same FBW/PSM it is scanned
	// out with), else any live colour surface at that base (a CT32 render scanned out as CT24
	// is the same bytes). Wrong-fast: scale 1, no y offset (no upscale, no wrapped display
	// buffer yet). No surface -> nothing to present: a display buffer written only by the CPU
	// (uploads, moves) needs the byte-road present, not built yet.
	const int index = i >= 0 ? i : 1;
	const auto& fb = PCRTCDisplays.PCRTCDisplays[index];
	const GSVector2i size = PCRTCDisplays.GetFramebufferSize(i);
	if (fb.framebufferRect.rempty() || fb.FBW == 0 || size.x < 0 || size.y < 0)
		return nullptr;

	const GSTileSurfaceLayout disp_l{static_cast<u32>(fb.Block()), static_cast<u8>(fb.FBW), static_cast<u8>(fb.PSM),
		GSTileSurfaceKind::Color};
	GSTileSurfaceId id = m_vram_model.FindExact(disp_l);
	if (id == kGSTileNoSurface)
	{
		for (u32 k = 0; k < m_vram_model.SurfaceSlots(); k++)
		{
			const GSVramModel::Surface& s = m_vram_model.Get(static_cast<GSTileSurfaceId>(k));
			if (s.alive && s.layout.kind == GSTileSurfaceKind::Color && s.layout.bp == disp_l.bp)
			{
				id = static_cast<GSTileSurfaceId>(k);
				break;
			}
		}
	}
	if (id == kGSTileNoSurface)
		return nullptr;

	const GSVramModel::Surface& surf = m_vram_model.Get(id);
	if (!surf.pool_handle)
		return nullptr;
	scale = 1.0f;
	y_offset = 0;
	return m_target_pool.GetTexture(surf.pool_handle);
}

bool GSRendererTileGpu::IsCoverageAlphaSupported()
{
	return false;
}

// A host->local transfer's destination. Fires BEFORE GSState writes the shadow, so: (1) any
// live ring slot for the pages captures the page's current bytes as a version copy and closes,
// keeping every already-planned reader on the bytes it saw (the "version" upload mechanism);
// (2) blocks only a target holds newest that this write only PARTIALLY overwrites are pulled
// down first (the one stall road -- the whole-block/whole-page common case shrinks or clears
// the target's claim for free); (3) the model records the CPU write. Order matters: closing
// the slots first also drops their synced claims, so the spill question is asked against
// truth the CPU shadow does NOT hold (a claim the ring vouched for is not one S can shrink
// against). Footprints are exact through GSOffset, DBW=0 included (the address math folds
// every row onto one page column, which is what the hardware does with it).
void GSRendererTileGpu::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	const GSTileSurfaceLayout layout{BITBLTBUF.DBP, static_cast<u8>(BITBLTBUF.DBW),
		static_cast<u8>(BITBLTBUF.DPSM), KindForPsm(BITBLTBUF.DPSM)};
	const u8 planes = gsTilePlanesInvalidatedByWrite(BITBLTBUF.DPSM);
	GSVramModel::RectFootprint fp;
	GSVramModel::FootprintForRect(layout, r, fp);
	SupersedeRingSlots(fp.pages);
	if (!m_vram_model.SpillBeforeCpuWrite(fp, planes).empty())
	{
		// This write partially overwrites blocks only a target holds, so it takes the stall road
		// -- and a stall FLUSHES the pending plan, which retires every synced claim the ring
		// vouched for (those slots are spent, and their bytes were never in S). Retiring a claim
		// can only ADD pages to the spill set, so the set that actually has to come down is the
		// one asked for on the far side of the flush. Answering from this side leaves the pages
		// whose claim the flush retired unpulled, and OnCpuWrite below then clears truth the CPU
		// shadow never received -- silent in Release, its synced assert in Devel. So: ask once to
		// learn a stall is coming, flush, then ask again for the real set.
		FlushPendingPlan();
		ReadbackToShadow(m_vram_model.SpillBeforeCpuWrite(fp, planes), StallSite::UploadSubBlock);
	}
	m_vram_model.OnCpuWrite(fp, planes);
	if (m_pass_sim.IsActive() && !m_pass_sim_in_move) [[unlikely]]
		m_pass_sim.OnUpload(PagesForTargetRect(layout, r));
}

// A local->host readback's source (or a CLUT load's): a CPU consumer of the pages. Anything a
// target holds newest comes down first -- the genuine crossing, and on this road a stall. The
// clut flag splits the on-GPU palette-gather road from a genuine readback in the pass model.
void GSRendererTileGpu::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	const GSTileSurfaceLayout layout{BITBLTBUF.SBP, static_cast<u8>(BITBLTBUF.SBW),
		static_cast<u8>(BITBLTBUF.SPSM), KindForPsm(BITBLTBUF.SPSM)};
	ReadbackToShadow(GSVramModel::PagesForRect(layout, r), clut ? StallSite::Clut : StallSite::LocalRead);
	if (m_pass_sim.IsActive() && !m_pass_sim_in_move) [[unlikely]]
		m_pass_sim.OnCpuRead(PagesForTargetRect(layout, r), clut);
}

// A local->local move. Observed as OnMove first — matching the Tile renderer's order, where
// OnMove sees the pass state before the move's own read/write halves touch it — then the
// base performs the real guest-memory copy. That base Move calls InvalidateLocalMem(src) and
// InvalidateVideoMem(dst) in turn, which the memory model needs (the source comes down if a
// target holds it, the destination write shrinks or clears claims); OnMove has already
// accounted both halves for the pass sim, so m_pass_sim_in_move suppresses only the sim
// forwards for the copy's duration.
void GSRendererTileGpu::Move()
{
	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;
	const GSVector4i src_r(m_env.TRXPOS.SSAX, m_env.TRXPOS.SSAY, m_env.TRXPOS.SSAX + w, m_env.TRXPOS.SSAY + h);
	const GSVector4i dst_r(m_env.TRXPOS.DSAX, m_env.TRXPOS.DSAY, m_env.TRXPOS.DSAX + w, m_env.TRXPOS.DSAY + h);

	const GSTileSurfaceLayout src_l{m_env.BITBLTBUF.SBP, static_cast<u8>(m_env.BITBLTBUF.SBW),
		static_cast<u8>(m_env.BITBLTBUF.SPSM), KindForPsm(m_env.BITBLTBUF.SPSM)};
	const GSTileSurfaceLayout dst_l{m_env.BITBLTBUF.DBP, static_cast<u8>(m_env.BITBLTBUF.DBW),
		static_cast<u8>(m_env.BITBLTBUF.DPSM), KindForPsm(m_env.BITBLTBUF.DPSM)};

	if (m_pass_sim.IsActive()) [[unlikely]]
		m_pass_sim.OnMove(PagesForTargetRect(src_l, src_r), PagesForTargetRect(dst_l, dst_r));

	m_pass_sim_in_move = true;
	GSRenderer::Move();
	m_pass_sim_in_move = false;
}

// The close/switch seam Classic reads its texture cache back on: everything a target holds
// newest comes down. A savestate taken with UserHacks_ReadTCOnClose lands here too.
void GSRendererTileGpu::ReadbackTextureCache()
{
	SyncAllTruthToCpu();
}

// A purge drops the resident targets (device loss, a config change that rebuilds them). Truth
// comes down first so nothing is lost; the model then forgets every surface.
void GSRendererTileGpu::PurgeTextureCache(bool sources, bool targets, bool hash_cache)
{
	if (!targets)
		return;
	SyncAllTruthToCpu();
	m_target_pool.ReleaseAll();
	m_vram_model.Reset();
	m_surface_texels.clear();
}

GSVector4i GSRendererTileGpu::ComputeDrawRect() const
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

// The Tile renderer's PassSimObserveDraw, ported to the plain GSRenderer base. Every input
// reads from base state; the only substitutions are (a) the draw rect and page/kind helpers,
// replicated above, and (b) the z-write/z-test booleans, derived here from the registers using
// the SW-floor semantics (ZTE gates both) rather than pulled from a full draw lowering — the
// lowering's later z-write refinements are a stage-1.5 concern, when the decode-compare harness
// makes the whole vertex/state input byte-identical to the shipping decode.
void GSRendererTileGpu::ObserveDraw()
{
	const GSVector4i r = ComputeDrawRect();
	if (r.rempty())
		return;

	const GSDrawingContext* ctx = m_context;

	const GSTileSurfaceLayout fb_l{ctx->FRAME.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->FRAME.PSM), GSTileSurfaceKind::Color};
	const GSPageBitmap fb_pages = PagesForTargetRect(fb_l, r);
	GSPageBitmap fb_written;
	if (ctx->FRAME.FBMSK != 0xFFFFFFFFu)
		fb_written = fb_pages;

	// SW-floor z semantics: ZTE=0 turns off the test AND the write.
	const bool z_write = ctx->TEST.ZTE && !ctx->ZBUF.ZMSK;
	const bool z_test = ctx->TEST.ZTE && ctx->TEST.ZTST > ZTST_ALWAYS;

	GSPageBitmap z_pages;
	GSPageBitmap z_written;
	const bool z_used = z_write || z_test;
	if (z_used)
	{
		const GSTileSurfaceLayout z_l{ctx->ZBUF.Block(), static_cast<u8>(ctx->FRAME.FBW),
			static_cast<u8>(ctx->ZBUF.PSM), GSTileSurfaceKind::Depth};
		z_pages = PagesForTargetRect(z_l, r);
		if (z_write)
			z_written = z_pages;
	}

	GSPageBitmap tex_pages;
	GSPageBitmap core_pages;
	bool has_core = false;
	bool identity_feedback = false;
	if (PRIM->TME)
	{
		const bool mip = IsMipMapActive();
		const GIFRegTEX0 fixed_tex0 =
			ctx->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), mip);
		// The in-pass-read shape: the sampling maps pixel-to-pixel — the UV bbox lands within
		// one texel of the draw rect. An input-attachment read serves same-pixel reads of ANY
		// attachment bound to the pass (it has no coordinate argument), so the sampled window's
		// base does NOT have to be this draw's own target: the ping-pong post chain (write A,
		// draw into B sampling A at 1:1) is exactly this shape.
		if (!mip && PRIM->FST)
		{
			const int u0 = static_cast<int>(std::floor(m_vt.m_min.t.x));
			const int v0 = static_cast<int>(std::floor(m_vt.m_min.t.y));
			const int u1 = static_cast<int>(std::ceil(m_vt.m_max.t.x));
			const int v1 = static_cast<int>(std::ceil(m_vt.m_max.t.y));
			identity_feedback = std::abs(u0 - r.x) <= 1 && std::abs(v0 - r.y) <= 1 &&
								std::abs(u1 - r.z) <= 1 && std::abs(v1 - r.w) <= 1;
		}
		const u32 mip_levels = mip ? (std::min<u32>(ctx->TEX1.MXL, 6) + 1) : 1u;
		for (u32 i = 0; i < mip_levels; i++)
		{
			const GIFRegTEX0 lvl = (i == 0) ? fixed_tex0 : GetTex0Layer(i);
			const GSTileSurfaceLayout tex_l{
				lvl.TBP0, static_cast<u8>(lvl.TBW), static_cast<u8>(lvl.PSM), KindForPsm(lvl.PSM)};
			const int tw = 1 << std::min<u32>(lvl.TW, 10);
			const int th = 1 << std::min<u32>(lvl.TH, 10);
			tex_pages |= PagesForTargetRect(tex_l, GSVector4i(0, 0, tw, th));
		}
		if (mip_levels == 1 && ctx->CLAMP.WMS <= CLAMP_CLAMP && ctx->CLAMP.WMT <= CLAMP_CLAMP)
		{
			const int tw = 1 << std::min<u32>(fixed_tex0.TW, 10);
			const int th = 1 << std::min<u32>(fixed_tex0.TH, 10);
			int cx0 = static_cast<int>(std::floor(m_vt.m_min.t.x)) + 1;
			int cy0 = static_cast<int>(std::floor(m_vt.m_min.t.y)) + 1;
			int cx1 = static_cast<int>(std::floor(m_vt.m_max.t.x));
			int cy1 = static_cast<int>(std::floor(m_vt.m_max.t.y));
			const bool u_ok = (ctx->CLAMP.WMS == CLAMP_CLAMP) || (cx0 >= 0 && cx1 <= tw);
			const bool v_ok = (ctx->CLAMP.WMT == CLAMP_CLAMP) || (cy0 >= 0 && cy1 <= th);
			cx0 = std::max(cx0, 0);
			cy0 = std::max(cy0, 0);
			cx1 = std::min(cx1, tw);
			cy1 = std::min(cy1, th);
			if (u_ok && v_ok && cx0 < cx1 && cy0 < cy1)
			{
				const GSTileSurfaceLayout tex_l{fixed_tex0.TBP0, static_cast<u8>(fixed_tex0.TBW),
					static_cast<u8>(fixed_tex0.PSM), KindForPsm(fixed_tex0.PSM)};
				core_pages = PagesForTargetRect(tex_l, GSVector4i(cx0, cy0, cx1, cy1));
				has_core = true;
			}
		}
	}

	// -- the in-pass reader classifier (the crossover's per-tier policy fork) ----------
	// Which draws need the raster-order read of their own pixel, beyond pixel-identity
	// feedback (classified in the sim): blend equations and tests fixed-function cannot
	// express. Alpha range comes from the vertex trace; when it is not valid the classifier
	// assumes the full range (a census leans conservative). The dual-source class is kept
	// apart because Adreno has dual-source blending and Mali-G615 does not — that flag is
	// where the two tiers' reader populations diverge. DATE on 24-bit targets is skipped:
	// destination alpha reads back as a constant there, so no read is needed.
	u32 reader_flags = 0;
	if (!fb_written.empty())
	{
		if (ctx->TEST.DATE && GSLocalMemory::m_psm[ctx->FRAME.PSM].trbpp != 24)
			reader_flags |= GSTilePassSim::ReaderDate;
		if (PRIM->ABE)
		{
			const GIFRegALPHA& al = ctx->ALPHA;
			const bool uses_cd = (al.A == 1) || (al.B == 1) || (al.D == 1) || (al.C == 1);
			if (uses_cd && al.A != al.B)
			{
				// Cv = (A - B) * C >> 7 + D with the destination involved. A == B degenerates
				// to Cv = D (source-only or destination passthrough), which fixed-function
				// always expresses — hence the gate above.
				if (m_draw_env->COLCLAMP.CLAMP == 0)
					reader_flags |= GSTilePassSim::ReaderWrap;
				if (m_draw_env->PABE.PABE)
					reader_flags |= GSTilePassSim::ReaderPabe;
				if (al.D == al.A)
					reader_flags |= GSTilePassSim::ReaderCoeffGt1; // accumulation: a coefficient is 1 + C
				if (al.C == 1)
					reader_flags |= GSTilePassSim::ReaderAdFactor;
				else if (al.C == 2)
				{
					if (al.FIX > 0x80)
						reader_flags |= GSTilePassSim::ReaderFacGt1;
					// FIX <= 0x80 maps onto the constant blend factor: expressible.
				}
				else if (al.C == 0)
				{
					// C=As: the blend factor is the draw's POST-texture-function source alpha,
					// so the range must fold TFX/TEXA/CLUT — GetAlphaMinMax does exactly that
					// (the base trace's Update only zeroes m_alpha.valid; the renderer computes
					// the range lazily, which the plain-GSRenderer base does not do for us). A
					// palettised draw needs the CLUT's decoded read buffer current first or the
					// scan reads the previous draw's palette; there is no GPU-resident palette on
					// this road yet, so the Tile feeder's conservative not-cpu-current branch does
					// not apply here.
					if (PRIM->TME && GSLocalMemory::m_psm[ctx->TEX0.PSM].pal > 0)
						m_mem.m_clut.Read32(ctx->TEX0, m_draw_env->TEXA);
					const int amax = GetAlphaMinMax().max;
					if (amax > 0x80)
						reader_flags |= GSTilePassSim::ReaderFacGt1;
					else
						reader_flags |= GSTilePassSim::ReaderAsDualSource;
				}
			}
		}
	}

	// Does this draw land any alpha in memory? Only the alpha bits the target FORMAT keeps
	// count — PSMCT24 stores none at all, PSMCT16 keeps one — and FBMSK masks them off from
	// there. A draw that stores no alpha can take the doubled-alpha trick whatever else
	// touches the surface, because the doubled value never outlives the blend.
	const u32 fmt_alpha = GSLocalMemory::m_psm[ctx->FRAME.PSM].fmsk & 0xFF000000u;
	const bool stores_alpha = !fb_written.empty() && (fmt_alpha & ~ctx->FRAME.FBMSK) != 0;

	m_pass_sim.OnDraw(fb_written, z_written, fb_pages | z_pages, tex_pages, core_pages, has_core,
		identity_feedback, PRIM->TME && PRIM->FST, reader_flags, stores_alpha, ctx->FRAME.Block(),
		ctx->FRAME.PSM, ctx->ZBUF.Block(), ctx->ZBUF.PSM, z_used,
		static_cast<u32>(std::max(m_vertex->tail, m_vertex->next)));
}

// The accumulated per-frame pass structure, mean / p50 at teardown -- the offline
// -tilepasssim arm's aggregate (minus the Tile-only GIF-stream tail), then the memory model's
// own traffic. These are the structural counters the stage-1 gate reads against the design
// values, over draws AND the stream's memory events, so every break reason is measured rather
// than zero by construction.
void GSRendererTileGpu::ReportPassStructure()
{
	ReportModelTraffic();
	if (!m_pass_sim.IsActive())
	{
		// Say so rather than print nothing: a missing section reads as "the sim found nothing",
		// and the whole point of the numbers above is that they are compared against these.
		if (!m_model_frames.empty())
			Console.WriteLn("  (pass-structure cross-check off -- EmuCore/GS/TileGpuPassSim, gsrunner -tilepasssim)");
		return;
	}
	const std::vector<GSTilePassSim::FrameStats>& frames = m_pass_sim.Frames();
	if (frames.empty())
		return;

	const double n = static_cast<double>(frames.size());
	const auto stat = [&frames, n](auto get) {
		std::vector<u32> v(frames.size());
		double sum = 0.0;
		for (size_t i = 0; i < frames.size(); i++)
		{
			v[i] = get(frames[i]);
			sum += v[i];
		}
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		struct
		{
			double mean;
			u32 p50;
		} r{sum / n, v[v.size() / 2]};
		return r;
	};
	using FS = GSTilePassSim::FrameStats;
	const auto brk = [&stat](GSTilePassSim::BreakReason why) {
		return stat([why](const FS& f) { return f.breaks[static_cast<u32>(why)]; });
	};
	const auto tier = [&stat](auto get, u32 t) {
		return stat([get, t](const FS& f) { return get(f, t); });
	};

	const auto draws = stat([](const FS& f) { return f.draws; });
	const auto verts = stat([](const FS& f) { return f.verts; });
	const auto passes = stat([](const FS& f) { return f.passes; });
	const auto snaps = stat([](const FS& f) { return f.snapshots; });
	const auto snap_pages = stat([](const FS& f) { return f.snapshot_pages; });
	const auto biggest = stat([](const FS& f) { return f.draws_in_biggest_pass; });
	const auto inpass = stat([](const FS& f) { return f.feedback_inpass; });
	const auto identity = stat([](const FS& f) { return f.feedback_identity; });
	const auto fruns = stat([](const FS& f) { return f.feedback_runs; });
	const auto upfree = stat([](const FS& f) { return f.uploads_free; });
	const auto upver = stat([](const FS& f) { return f.uploads_versionable; });
	const auto updraw = stat([](const FS& f) { return f.uploads_drawable; });
	const auto palgather = stat([](const FS& f) { return f.palette_gathers; });
	const auto cpupages = stat([](const FS& f) { return f.cpu_read_pages; });
	const auto b_feedback = brk(GSTilePassSim::BreakReason::Feedback);
	const auto b_cap = brk(GSTilePassSim::BreakReason::TargetCap);
	const auto b_upload = brk(GSTilePassSim::BreakReason::Upload);
	const auto b_move = brk(GSTilePassSim::BreakReason::MoveHazard);
	const auto b_cpu = brk(GSTilePassSim::BreakReason::CpuRead);
	const auto rdrsA = tier([](const FS& f, u32 t) { return f.readers[t]; }, 0);
	const auto rdrsB = tier([](const FS& f, u32 t) { return f.readers[t]; }, 1);
	const auto dvertsB = tier([](const FS& f, u32 t) { return f.declared_verts[t]; }, 1);
	const auto rvertsB = tier([](const FS& f, u32 t) { return f.reader_verts[t]; }, 1);
	const auto tas = stat([](const FS& f) { return f.trick_as_draws; });
	const auto tlive = stat([](const FS& f) { return f.trick_as_on_live; });
	const auto tresc = stat([](const FS& f) { return f.trick_as_mask_rescued; });
	const double eligible = tas.mean - tlive.mean + tresc.mean;

	Console.WriteLn("TileGpu pass structure over %u frames (GS-semantic MINIMUM, %u pairs/pass; "
					"draws + stream memory events; mean / p50 per frame):",
		static_cast<u32>(frames.size()), GSTilePassSim::kMaxTargetPairs);
	Console.WriteLn("  draws %9.2f / %-6u  verts %9.2f / %-8u  biggest pass %6.2f / %u draws", draws.mean,
		draws.p50, verts.mean, verts.p50, biggest.mean, biggest.p50);
	Console.WriteLn("  passes %8.2f / %-6u  snapshots %5.2f / %-4u (%.2f / %u pages)", passes.mean,
		passes.p50, snaps.mean, snaps.p50, snap_pages.mean, snap_pages.p50);
	Console.WriteLn("  breaks: feedback %.2f/%u  target-cap %.2f/%u  upload %.2f/%u  move %.2f/%u  "
					"cpu-read %.2f/%u",
		b_feedback.mean, b_feedback.p50, b_cap.mean, b_cap.p50, b_upload.mean, b_upload.p50, b_move.mean,
		b_move.p50, b_cpu.mean, b_cpu.p50);
	Console.WriteLn("  feedback served in-pass: core proof %.2f / %u   pixel-identity %.2f / %u   "
					"offset residue %.2f runs",
		inpass.mean, inpass.p50, identity.mean, identity.p50, fruns.mean);
	Console.WriteLn("  uploads (never break): free %.2f / %u   versionable %.2f / %u   draw-realized "
					"%.2f / %u   palette gathers %.2f / %u   cpu-read pages %.2f / %u",
		upfree.mean, upfree.p50, upver.mean, upver.p50, updraw.mean, updraw.p50, palgather.mean,
		palgather.p50, cpupages.mean, cpupages.p50);
	Console.WriteLn("  in-pass readers/frame: tierA(dual-src) %.2f / %u   tierB(Mali) %.2f / %u   "
					"tierB declared %.2f verts, reader %.2f verts",
		rdrsA.mean, rdrsA.p50, rdrsB.mean, rdrsB.p50, dvertsB.mean, rvertsB.mean);
	Console.WriteLn("  doubled-alpha trick (tierB): As-class %.2f draws/frame; disqualified %.2f, "
					"rescued %.2f -> eligible %.2f",
		tas.mean, tlive.mean, tresc.mean, eligible);
}

// The memory model's per-frame traffic: what the byte road actually moved, and every crossing
// the design says should be rare. Mean / p50 over presented frames.
void GSRendererTileGpu::ReportModelTraffic()
{
	if (m_model_frames.empty())
		return;
	const double n = static_cast<double>(m_model_frames.size());
	const auto stat = [this, n](auto get) {
		std::vector<u32> v(m_model_frames.size());
		double sum = 0.0;
		for (size_t i = 0; i < m_model_frames.size(); i++)
		{
			v[i] = get(m_model_frames[i]);
			sum += v[i];
		}
		std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
		struct
		{
			double mean;
			u32 p50;
		} r{sum / n, v[v.size() / 2]};
		return r;
	};
	using MF = ModelFrame;
	const auto surf = stat([](const MF& f) { return f.surfaces_live; });
	const auto passes = stat([](const MF& f) { return f.passes; });
	const auto ring = stat([](const MF& f) { return f.ring_pages; });
	const auto prefill = stat([](const MF& f) { return f.ring_prefill; });
	const auto versions = stat([](const MF& f) { return f.ring_versions; });
	const auto epochs = stat([](const MF& f) { return f.epochs; });
	const auto wbo = stat([](const MF& f) { return f.writeback_ops; });
	const auto wbp = stat([](const MF& f) { return f.writeback_pages; });
	const auto sdo = stat([](const MF& f) { return f.seed_ops; });
	const auto sdp = stat([](const MF& f) { return f.seed_pages; });
	const auto wbrk = stat([](const MF& f) { return f.writeback_breaks; });
	const auto sbrk = stat([](const MF& f) { return f.seed_breaks; });
	const auto dbrk = stat([](const MF& f) { return f.date_breaks; });
	const auto snaps = stat([](const MF& f) { return f.snapshots; });
	const auto self = stat([](const MF& f) { return f.self_reads; });
	const auto binds = stat([](const MF& f) { return f.tex_binds; });
	const auto bindbrk = stat([](const MF& f) { return f.tex_bind_breaks; });
	const auto alias = stat([](const MF& f) { return f.alias_steal_pages; });
	const auto lossy = stat([](const MF& f) { return f.lossy_pages; });
	const auto skipped = stat([](const MF& f) { return f.skipped_draws; });
	const auto flushes = stat([](const MF& f) { return f.flushes; });
	const auto st = [&stat](StallSite s) {
		return stat([s](const MF& f) { return f.stalls[static_cast<u32>(s)]; });
	};
	const auto stp = [&stat](StallSite s) {
		return stat([s](const MF& f) { return f.stall_pages[static_cast<u32>(s)]; });
	};
	const auto s_up = st(StallSite::UploadSubBlock), s_rd = st(StallSite::LocalRead), s_cl = st(StallSite::Clut),
			   s_all = st(StallSite::SyncAll);
	const auto p_up = stp(StallSite::UploadSubBlock), p_rd = stp(StallSite::LocalRead), p_cl = stp(StallSite::Clut),
			   p_all = stp(StallSite::SyncAll);

	Console.WriteLn("TileGpu memory model over %u frames (mean / p50 per frame):", static_cast<u32>(m_model_frames.size()));
	Console.WriteLn("  surfaces live %6.2f / %-4u  passes %7.2f / %-5u  ring pages %7.2f / %-5u (prefilled %.2f / %u, "
					"version copies %.2f / %u, epochs %.2f / %u)",
		surf.mean, surf.p50, passes.mean, passes.p50, ring.mean, ring.p50, prefill.mean, prefill.p50, versions.mean,
		versions.p50, epochs.mean, epochs.p50);
	Console.WriteLn("  writebacks %6.2f / %-4u ops, %8.2f / %-5u pages, %.2f / %u pass breaks   seeds %6.2f / %-4u ops, "
					"%8.2f / %-5u pages, %.2f / %u pass breaks",
		wbo.mean, wbo.p50, wbp.mean, wbp.p50, wbrk.mean, wbrk.p50, sdo.mean, sdo.p50, sdp.mean, sdp.p50, sbrk.mean,
		sbrk.p50);
	Console.WriteLn("  target binds (rule 2: the read came off a resident target, no bytes composed) %.2f / %u draws, "
					"%.2f / %u pass breaks",
		binds.mean, binds.p50, bindbrk.mean, bindbrk.p50);
	Console.WriteLn("  self-reads (snapshot semantics) %.2f / %u   DATE: %.2f / %u pass breaks, %.2f / %u snapshots   "
					"alias-steal pages %.2f / %u   lossy pages (no byte road) %.2f / %u   skipped draws %.2f / %u   "
					"mid-frame flushes %.2f / %u",
		self.mean, self.p50, dbrk.mean, dbrk.p50, snaps.mean, snaps.p50, alias.mean, alias.p50, lossy.mean,
		lossy.p50, skipped.mean, skipped.p50, flushes.mean, flushes.p50);
	Console.WriteLn("  stalls: upload sub-block %.2f/%u (%.2f pages)  local-read %.2f/%u (%.2f)  clut %.2f/%u (%.2f)  "
					"sync-all %.2f/%u (%.2f)",
		s_up.mean, s_up.p50, p_up.mean, s_rd.mean, s_rd.p50, p_rd.mean, s_cl.mean, s_cl.p50, p_cl.mean, s_all.mean,
		s_all.p50, p_all.mean);
}


// -- the memory model's per-draw road ---------------------------------------------------------

GSTileSurfaceId GSRendererTileGpu::EnsureSurface(const GSTileSurfaceLayout& layout, const GSVector4i& rect,
	const GSPageBitmap& pages)
{
	if (!PoolSupports(layout))
		return kGSTileNoSurface;
	const int height = GSTileTargetPool::HeightForPages(layout, pages);
	GSTileSurfaceId id = m_vram_model.FindExact(layout);
	if (id == kGSTileNoSurface)
	{
		const u32 handle = m_target_pool.Allocate(layout, height);
		if (handle == 0)
			return kGSTileNoSurface;
		id = m_vram_model.Create(layout, rect, handle);
		m_vram_model.GrowResidency(id, pages);
		Texels(id) = {}; // a fresh texture holds nothing; see the member's note
		NoteTextureGeometry(id, height);
		return id;
	}
	m_vram_model.GrowResidency(id, pages);
	// Growth copies the texture's content into a taller one on the device now -- before any of
	// this frame's passes are recorded (the plan is built at VSync), so it moves last frame's
	// pixels and every draw of this frame lands in the grown texture.
	if (!m_target_pool.EnsureHeight(m_vram_model.Get(id).pool_handle, height))
		return kGSTileNoSurface;
	NoteTextureGeometry(id, height);
	return id;
}

// The page set the surface's texture rectangle spans, refreshed when the pool grows it. Growth
// keeps the pixels it had, so `filled` survives; the new rows are simply not in it yet.
void GSRendererTileGpu::NoteTextureGeometry(GSTileSurfaceId id, int height)
{
	SurfaceTexels& t = Texels(id);
	if (t.height == height)
		return;
	const GSVramModel::Surface& surf = m_vram_model.Get(id);
	const GSVector2i dim = m_target_pool.GetTexture(surf.pool_handle)->GetSize();
	t.pages = GSVramModel::PagesForRect(surf.layout, GSVector4i(0, 0, dim.x, dim.y));
	t.height = height;
}

GSRendererTileGpu::SurfaceTexels& GSRendererTileGpu::Texels(GSTileSurfaceId id)
{
	if (m_surface_texels.size() <= id)
		m_surface_texels.resize(id + 1);
	return m_surface_texels[id];
}

// Tile's PagesNeedingUpload: pages whose texture bytes are not already current for every
// relevant plane -- another owner, no owner (CPU-newest), or a shrunk block mask.
GSPageBitmap GSRendererTileGpu::PagesNeedingSeed(GSTileSurfaceId id, const GSPageBitmap& pages, u8 planes) const
{
	GSPageBitmap need;
	pages.forEachSetPage([&](u32 page) {
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!(planes & (1u << pi)))
				continue;
			if (m_vram_model.OwnerOf(page, pi) != id || m_vram_model.TruthMask(page, pi) != GSVramModel::kFullBlockMask)
			{
				need.set(page);
				return;
			}
		}
	});
	return need;
}

// Rule 2 of the design record's texel road: "sole owner is a live surface, same format, 1:1 -> bind
// the surface". Every clause below is a proof the two roads would read the same texel, so that the
// only difference the bind makes is FRESHNESS -- the target's live pixels instead of bytes composed
// from it earlier. Anything that cannot be proved takes the byte road, which is slower and never
// wrong; that is the whole reason the image road can be built one case at a time.
GSTileSurfaceId GSRendererTileGpu::TargetForTextureRead(const GSTileSurfaceLayout& tex_l, u32 tw, u32 th,
	const GSPageBitmap& tex_pages, GSTileSurfaceId fb_id, GSTileSurfaceId z_id) const
{
	if (!m_bindless_targets || tex_pages.empty())
		return kGSTileNoSurface;

	// All-or-nothing by construction: one live surface holds whole-page truth of every colour
	// plane on every page of the window. A window split across owners, or part CPU-newest, or with
	// a block mask shrunk by an upload, has no single authoritative texture and is refused here.
	const GSTileSurfaceId src = m_vram_model.SoleGpuOwner(tex_pages, kGSTilePlanesColor);
	if (src == kGSTileNoSurface || src == fb_id || src == z_id)
		return kGSTileNoSurface;

	const GSVramModel::Surface& surf = m_vram_model.Get(src);
	if (!surf.alive || !surf.pool_handle)
		return kGSTileNoSurface;

	// The pool's pixel space is the guest layout only for a page-aligned CT32/CT24 colour surface
	// (HasByteRoad's conditions, for the same reason the writeback and seed shaders need them), and
	// it is the READ's pixel space only when base and stride agree exactly. Then target pixel
	// (u, v) is guest texel (u, v) -- no offset, no restride, no region arithmetic.
	if (!HasByteRoad(surf.layout) || surf.layout.bp != tex_l.bp || surf.layout.bw != tex_l.bw)
		return kGSTileNoSurface;
	// Byte-compatible formats. Equal PSM is the whole story except for a CT32 target read as CT24:
	// same bytes, and the draw's TEXA already supplies the alpha the read must not take from the
	// target's own alpha channel. A CT24 target read as CT32 is refused -- its alpha bytes belong
	// to whoever else writes that page, and the target's texture does not hold them.
	if (!(surf.layout.psm == tex_l.psm || (surf.layout.psm == PSMCT32 && tex_l.psm == PSMCT24)))
		return kGSTileNoSurface;

	// The model tracks BYTES; the sampler reads TEXELS. A page whose bytes the surface owns but
	// whose texture rows nothing has written yet would sample allocator leftovers, which is exactly
	// what SurfaceTexels exists to know about.
	if (m_surface_texels.size() <= src || !tex_pages.andnot(m_surface_texels[src].filled).empty())
		return kGSTileNoSurface;

	// ...and the window has to be inside the image. The pool only ever grows a target, so a fit
	// proved here still holds when the pass runs.
	const GSTexture* tex = m_target_pool.GetTexture(surf.pool_handle);
	if (!tex)
		return kGSTileNoSurface;
	const GSVector2i dim = tex->GetSize();
	if (static_cast<int>(tw) > dim.x || static_cast<int>(th) > dim.y)
		return kGSTileNoSurface;

	return src;
}

u32 GSRendererTileGpu::PlanTargetIndex(GSTileSurfaceId id)
{
	if (m_plan_target_of_surface.size() <= id)
		m_plan_target_of_surface.resize(id + 1, GSDevice::GSTileGpuPassPlan::kNoTarget);
	if (m_plan_target_of_surface[id] == GSDevice::GSTileGpuPassPlan::kNoTarget)
	{
		m_plan_target_of_surface[id] = static_cast<u32>(m_plan_target_surfaces.size());
		m_plan_target_surfaces.push_back(id);
	}
	return m_plan_target_of_surface[id];
}

// A ring slot for `page` in the current epoch. Prefill from S unless the page is, in every
// plane, whole-page unsynced truth of a surface with a byte road -- exactly the case where a
// writeback this epoch composes every byte (ComposeRingPages emits it right after asking here).
// A page synced already (either its bytes are in S after a stall readback, or an earlier
// writeback this epoch composed its live slot) prefills too: over the same live slot the memcpy
// runs first and the writeback's blocks land over it, so the extra copy is harmless.
u32 GSRendererTileGpu::EnsureRingSlot(u32 page)
{
	bool needs_prefill = false;
	for (u32 pi = 0; pi < kGSTilePlaneCount && !needs_prefill; pi++)
	{
		const GSTileSurfaceId owner = m_vram_model.OwnerOf(page, pi);
		if (owner == kGSTileNoSurface || m_vram_model.TruthMask(page, pi) != GSVramModel::kFullBlockMask ||
			m_vram_model.SyncedPages(pi).test(page) || !HasByteRoad(m_vram_model.Get(owner).layout))
			needs_prefill = true;
	}

	if (m_ring_live[page] != 0)
	{
		RingEntry& e = m_ring_entries[m_ring_live[page] - 1];
		e.prefill_s |= needs_prefill;
		return m_ring_live[page] - 1;
	}
	RingEntry e = {};
	e.page = static_cast<u16>(page);
	e.epoch_first = static_cast<u16>(m_epoch);
	e.epoch_last = 0xFFFF; // open
	e.prefill_s = needs_prefill;
	m_ring_entries.push_back(e);
	m_ring_live[page] = static_cast<u16>(m_ring_entries.size());
	m_frame.ring_pages++;
	return static_cast<u32>(m_ring_entries.size() - 1);
}

void GSRendererTileGpu::SupersedeRingSlots(const GSPageBitmap& pages)
{
	bool cut = false;
	GSPageBitmap closed;
	pages.forEachSetPage([&](u32 page) {
		if (m_ring_live[page] == 0)
			return;
		RingEntry& e = m_ring_entries[m_ring_live[page] - 1];
		e.epoch_last = static_cast<u16>(m_epoch);
		if (e.prefill_s)
		{
			// The bytes an already-planned reader must see are about to be overwritten in S:
			// keep them. (A slot the GPU composes entirely needs nothing -- its writebacks read
			// targets, not S.)
			e.versioned = true;
			e.version_offset = static_cast<u32>(m_ring_versions.size());
			m_ring_versions.resize(m_ring_versions.size() + GS_PAGE_SIZE);
			std::memcpy(m_ring_versions.data() + e.version_offset, m_mem.vm8() + page * GS_PAGE_SIZE, GS_PAGE_SIZE);
			m_frame.ring_versions++;
		}
		m_ring_live[page] = 0;
		closed.set(page);
		cut = true;
	});
	if (cut)
	{
		// The closed slots' synced claims die with them: the next reader of these pages this
		// frame composes a fresh slot (its writebacks re-run into it).
		m_vram_model.ClearSynced(closed);
		m_epoch++;
	}
}

void GSRendererTileGpu::EmitPrepOp(GSDevice::GSTileGpuPrepKind kind, GSTileSurfaceId id, const GSPageBitmap& pages)
{
	if (pages.empty())
		return;
	const GSVramModel::Surface& surf = m_vram_model.Get(id);
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = kind;
	op.target = PlanTargetIndex(id);
	op.bp = surf.layout.bp;
	op.bw = surf.layout.bw;
	op.psm = surf.layout.psm;
	op.byte_mask = (surf.layout.psm == PSMCT24) ? 0x00FFFFFFu : 0xFFFFFFFFu;
	op.epoch = m_epoch;
	op.first_page_entry = static_cast<u32>(m_plan_page_entries.size());
	pages.forEachSetPage([&](u32 page) {
		u32 mask = GSVramModel::kFullBlockMask;
		if (kind == GSDevice::GSTileGpuPrepKind::Writeback)
		{
			// The blocks this surface holds newest on the page, over the planes it owns there
			// (they agree except after a CPU write that shrank one plane's claim differently --
			// then the union over-writes bytes of the other plane, a bounded imprecision noted
			// in the design record).
			mask = 0;
			for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
			{
				if (m_vram_model.OwnerOf(page, pi) == id)
					mask |= m_vram_model.TruthMask(page, pi);
			}
			if (mask == 0)
				return;
		}
		m_plan_page_entries.push_back(GSDevice::GSTileGpuPageEntry{static_cast<u16>(page), 0, mask});
	});
	op.page_entry_count = static_cast<u32>(m_plan_page_entries.size()) - op.first_page_entry;
	if (op.page_entry_count == 0)
		return;
	m_plan_prep_ops.push_back(op);
	if (kind == GSDevice::GSTileGpuPrepKind::Writeback)
	{
		m_frame.writeback_ops++;
		m_frame.writeback_pages += op.page_entry_count;
	}
	else
	{
		m_frame.seed_ops++;
		m_frame.seed_pages += op.page_entry_count;
	}
}

// Every byte of `pages` reachable through the ring for the current epoch: a slot per page,
// prefilled from S where any byte is CPU-newest, and a writeback of every surface holding
// unsynced truth on them (in the ring's read-modify-write order, one op per owner). Marks what
// the ring now holds synced. Owners without a byte road contribute nothing (the slot keeps its
// prefill: stale where they held newest) -- counted, and warned once, never silent.
void GSRendererTileGpu::ComposeRingPages(const GSPageBitmap& pages)
{
	if (pages.empty())
		return;
	pages.forEachSetPage([&](u32 page) { EnsureRingSlot(page); });

	const GSPageBitmap need = m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll);
	if (need.empty())
		return;

	// Bucket by owner. Tiny in practice (one or two owners per read window).
	struct Bucket
	{
		GSTileSurfaceId id;
		GSPageBitmap pages;
	};
	Bucket buckets[8];
	u32 num_buckets = 0;
	u32 lossy = 0;
	need.forEachSetPage([&](u32 page) {
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!m_vram_model.Truth(pi).test(page) || m_vram_model.SyncedPages(pi).test(page))
				continue;
			const GSTileSurfaceId owner = m_vram_model.OwnerOf(page, pi);
			pxAssert(owner != kGSTileNoSurface);
			if (!HasByteRoad(m_vram_model.Get(owner).layout))
			{
				lossy++;
				continue;
			}
			u32 b = 0;
			for (; b < num_buckets; b++)
			{
				if (buckets[b].id == owner)
					break;
			}
			if (b == num_buckets)
			{
				if (num_buckets == std::size(buckets))
				{
					lossy++; // beyond the table: counted as lost rather than mis-composed
					continue;
				}
				buckets[num_buckets].id = owner;
				buckets[num_buckets].pages.clear();
				num_buckets++;
			}
			buckets[b].pages.set(page);
		}
	});

	for (u32 b = 0; b < num_buckets; b++)
		EmitPrepOp(GSDevice::GSTileGpuPrepKind::Writeback, buckets[b].id, buckets[b].pages);

	if (lossy)
	{
		m_frame.lossy_pages += lossy;
		if (!m_warned_lossy)
		{
			m_warned_lossy = true;
			Console.Warning("TileGpu: page truth held by a surface without a byte road (depth / 16-bit / unaligned) "
							"was read or stolen -- the bytes are stale there. Counted per frame as lossy pages.");
		}
	}

	// What the ring now holds (or, for the lossy part, what we will treat as held): synced.
	m_vram_model.OnReadback(need);
}

// Truth on `pages` is about to become unreachable and no byte road can carry it: count it and say
// so once. Two roads reach here -- a target whose own layout has no writeback shader, and the depth
// plane, which has none at all.
void GSRendererTileGpu::NoteLossyPages(const GSPageBitmap& pages)
{
	if (pages.empty())
		return;
	m_frame.lossy_pages += pages.count();
	if (!m_warned_lossy)
	{
		m_warned_lossy = true;
		Console.Warning("TileGpu: page truth moved through a surface without a byte road (depth / 16-bit / unaligned) "
						"-- the bytes are stale there. Counted per frame as lossy pages.");
	}
}

// A surface without a byte road is about to take `pages`: the model's steal invariant needs the
// previous owners' truth marked synced, but no bytes actually move -- the previous owners' pixels
// are lost to the byte store, and the taker's texture keeps whatever it held.
void GSRendererTileGpu::LossySteal(const GSPageBitmap& pages)
{
	if (pages.empty())
		return;
	NoteLossyPages(pages);
	m_vram_model.OnReadback(m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll));
}

// Compose `pages` into the ring for the draw being accumulated, and open a new pass if the
// writebacks that took cannot run at the head of the open one.
//
// A writeback the plan build leaves inside the open pass runs at that pass's head, ahead of every
// draw already in it. Where it collides with one of them -- reading a surface they have rendered
// into, or rewriting a ring slot they have sampled -- the hoist composes stale bytes and hands them
// backwards, and the draw opens its own pass instead, exactly as a seed does. Where it collides with
// none of them the hoist is provably invisible and the pass stays whole: pass count is the currency
// of this design.
void GSRendererTileGpu::ComposeForPendingDraw(const GSPageBitmap& pages, PendingDraw& pd)
{
	const u32 ops_before = static_cast<u32>(m_plan_prep_ops.size());
	ComposeRingPages(pages);
	if (m_plan_prep_ops.size() != ops_before && WritebackHoistCollides(ops_before))
	{
		pd.break_before = true;
		m_frame.writeback_breaks++;
		BreakOpenPass();
	}
}

// One draw's colour and depth surfaces claim the same pages, so only one of them can be the
// holder and the other's byte truth on them is dropped. Same model move as a road-less steal --
// synced without moving bytes -- but a different accuracy story, so a counter and a warning of
// its own: this one is about GS memory layout (how tightly the game packs FRAME against ZBUF),
// not about which surfaces this stage can write back yet.
void GSRendererTileGpu::AliasSteal(const GSPageBitmap& pages)
{
	if (pages.empty())
		return;
	m_frame.alias_steal_pages += pages.count();
	if (!m_warned_alias)
	{
		m_warned_alias = true;
		Console.Warning("TileGpu: a draw's colour and depth footprints share pages -- the game packs FRAME and ZBUF "
						"close enough that one draw writes both through the same pages. Both surfaces claim them and "
						"only one can hold them, so the colour surface's byte truth there is dropped. Counted per "
						"frame as alias-steal pages.");
	}
	m_vram_model.OnReadback(m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll));
}

void GSRendererTileGpu::BreakOpenPass()
{
	m_open_color = kGSTileNoSurface;
	m_open_z = kGSTileNoSurface;
	m_open_z_used = false;
	m_open_z_write = false;
	m_open_z_test = false;
	m_open_color_written.clear();
	m_open_z_written.clear();
	m_open_read.clear();
	m_open_tex_count = 0;
}

// A writeback the plan build leaves inside the open pass runs at that pass's head, ahead of every
// draw already in it. Two things make that wrong, and nothing else does:
//
//  - the writeback reads the target's pixels, so a draw of the open pass that renders into THAT
//    surface on a page the writeback takes would have its pixels composed before it drew them;
//  - the writeback rewrites the page's live ring slot in place, so a draw of the open pass that
//    samples that page would retroactively read the new bytes.
//
// The slot half is compared by SLOT, and it has to be. An epoch comparison proves nothing --
// OnNativeDraw drops a page's synced bit without advancing the epoch, so the page is re-composed
// into the same live slot at the same epoch, which is the case the unconditional break was
// written for. A page comparison is sound but pessimistic the other way: an upload between the
// earlier read and this writeback closes the page's slot and bumps the epoch, so the writeback
// composes a NEW slot and the earlier read keeps the bytes it saw. So the read set remembers the
// slot each page was read through and this compares it against the live one, which is the slot
// EnsureRingSlot just handed the writeback.
//
// One conservatism left: the read set is the whole composed read window (the size-fixed TEX0
// footprint), not the pages the draw's coordinates actually reach inside it.
bool GSRendererTileGpu::WritebackHoistCollides(u32 first_op) const
{
	for (u32 k = first_op; k < m_plan_prep_ops.size(); k++)
	{
		const GSDevice::GSTileGpuPrepOp& op = m_plan_prep_ops[k];
		GSPageBitmap p;
		for (u32 e = op.first_page_entry; e < op.first_page_entry + op.page_entry_count; e++)
			p.set(m_plan_page_entries[e].page);

		bool hit = false;
		(p & m_open_read).forEachSetPage([&](u32 page) { hit |= (m_open_read_slot[page] == m_ring_live[page]); });
		if (hit)
			return true;

		const GSTileSurfaceId src = m_plan_target_surfaces[op.target];
		if (src == m_open_color && p.intersects(m_open_color_written))
			return true;
		if (m_open_z_used && src == m_open_z && p.intersects(m_open_z_written))
			return true;
	}
	return false;
}

// The pass plan's geometry and the memory model's traffic, one flushed batch at a time. Copies
// this draw's vertices and indices into the frame streams, drives the model for its texture
// read and its target footprints, and records an indexed indirect draw plus the PendingDraw
// (its surfaces, transform inputs and prep-op range, resolved at the plan build). Skips
// empty-rect draws exactly as ObserveDraw does, so the plan and the pass model count the same
// draws. Indices are 0-based within the batch; the draw's vertex_offset rebases them into the
// frame vertex stream.
void GSRendererTileGpu::AccumulateDraw()
{
	const GSVector4i r = ComputeDrawRect();
	if (r.rempty())
		return;

	// Wrong-fast geometry. Triangles, lines and points are drawn at their native footprint: the
	// index buffer is already a topology list (strips and fans expanded at decode), so it copies
	// straight in and selects the matching executor pipeline. Sprites are two opposite corners we
	// synthesise into a quad (four corners, two triangles), so they ride the triangle pipeline.
	// The pass model still sees every draw through ObserveDraw, so accounting is unaffected --
	// only the submitted geometry is shaped here.
	GSDevice::GSTileGpuTopology topology;
	switch (m_vt.m_primclass)
	{
		case GS_TRIANGLE_CLASS:
		case GS_SPRITE_CLASS:
			topology = GSDevice::GSTileGpuTopology::Triangle;
			break;
		case GS_LINE_CLASS:
			topology = GSDevice::GSTileGpuTopology::Line;
			break;
		case GS_POINT_CLASS:
			topology = GSDevice::GSTileGpuTopology::Point;
			break;
		default:
			return;
	}
	const bool is_sprite = (m_vt.m_primclass == GS_SPRITE_CLASS);

	const u32 vcount = m_vertex->tail;
	const u32 icount = m_index->tail;
	if (vcount == 0 || icount == 0)
		return;

	const GSDrawingContext* ctx = m_context;

	// -- the model: where this draw lands and what it reads ---------------------------------
	const GSTileSurfaceLayout fb_l{ctx->FRAME.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->FRAME.PSM), GSTileSurfaceKind::Color};
	const GSTileSurfaceLayout z_l{ctx->ZBUF.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->ZBUF.PSM), GSTileSurfaceKind::Depth};

	// What this draw actually writes. An alpha test of NEVER fails every pixel, and AFAIL then
	// says which channels the failing pixel still lands in -- Classic's zm/fm derivation, and the
	// idiom SotC uses for its full-screen fades and post sprites: ATE=1 ATST=NEVER AFAIL=FB_ONLY
	// is "colour only, leave depth alone" whatever ZTE/ZMSK say. Missing it deposits those
	// sprites' Z=max into the persistent depth buffer and the whole 3D pass then fails GEQUAL.
	const bool atst_never = ctx->TEST.ATE && ctx->TEST.ATST == ATST_NEVER;
	const u32 afail = ctx->TEST.GetAFAIL(ctx->FRAME.PSM);
	const bool z_write = ctx->TEST.ZTE && !ctx->ZBUF.ZMSK && !(atst_never && afail != AFAIL_ZB_ONLY);
	const bool z_test = ctx->TEST.ZTE && ctx->TEST.ZTST > ZTST_ALWAYS;
	const bool z_used = z_write || z_test;
	const bool color_written = !(atst_never && (afail == AFAIL_KEEP || afail == AFAIL_ZB_ONLY)) &&
							   (ctx->FRAME.FBMSK & GSLocalMemory::m_psm[ctx->FRAME.PSM].fmsk) !=
								   GSLocalMemory::m_psm[ctx->FRAME.PSM].fmsk;
	if (!color_written && !z_write)
		return; // nothing lands anywhere: a no-op draw

	if (!PoolSupports(fb_l) || (z_used && !PoolSupports(z_l)))
	{
		// An 8/4-bit or zero-stride target: no surface can back it on this road yet. The draw
		// is dropped (its bytes never land) -- counted, and the pass model still saw it.
		m_frame.skipped_draws++;
		return;
	}

	// The destination-alpha test. On a 24-bit target there is no alpha to test and the GS passes
	// nothing (Classic's finding, tested on hardware) -- the draw is a no-op. Otherwise the draw
	// reads its own target's alpha before writing: served from a pass snapshot, so it must not
	// read pixels this pass already wrote (see the break below).
	u32 date = 0;
	if (ctx->TEST.DATE)
	{
		if (GSLocalMemory::m_psm[ctx->FRAME.PSM].trbpp == 24)
			return;
		date = 1u + static_cast<u32>(ctx->TEST.DATM);
	}
	const GSPageBitmap fb_pages = GSVramModel::PagesForRect(fb_l, r);
	GSPageBitmap z_pages;
	if (z_used)
		z_pages = GSVramModel::PagesForRect(z_l, r);

	// Targets: the surfaces this draw renders into (created or grown here; nothing moves yet).
	// Only the 32/16-bit colour and depth families have pool geometry: an 8/4-bit or
	// zero-stride target has no surface on this road, and the draw is dropped before it can
	// touch the model -- counted; the pass model still saw it.
	const GSTileSurfaceId fb_id = EnsureSurface(fb_l, r, fb_pages);
	if (fb_id == kGSTileNoSurface)
	{
		m_frame.skipped_draws++;
		return;
	}
	GSTileSurfaceId z_id = kGSTileNoSurface;
	if (z_used)
	{
		z_id = EnsureSurface(z_l, r, z_pages);
		if (z_id == kGSTileNoSurface)
		{
			m_frame.skipped_draws++;
			return;
		}
	}

	// Which pass is open for this draw? The plan build groups on the colour+depth surface pair and
	// the depth mode, so a draw that differs from the pass's first draw in any of them starts a new
	// pass and nothing the earlier draws did constrains its prep ops. Mirror of the grouping in
	// BuildAndExecutePlan -- the two must stay in step.
	if (fb_id != m_open_color || z_used != m_open_z_used || (z_used && z_id != m_open_z) ||
		z_write != m_open_z_write || z_test != m_open_z_test)
	{
		BreakOpenPass();
	}

	PendingDraw pd = {};
	pd.tex_source = kGSTileNoSurface;
	pd.tex_slot = GSDevice::GSTileGpuPassPlan::kNoTexSlot;
	pd.first_prep_op = static_cast<u32>(m_plan_prep_ops.size());

	// Fog. The GS walks the fragment's RGB toward FOGCOL by the per-vertex fog factor F (RGB only,
	// alpha untouched); the vertex stream already carries F, so the draw needs only the enable and
	// the colour. Sprites take F from the second vertex like everything else flat about them.
	pd.fge = PRIM->FGE;
	pd.fogcol = static_cast<u32>(m_env.FOGCOL.FCR) | (static_cast<u32>(m_env.FOGCOL.FCG) << 8) |
				(static_cast<u32>(m_env.FOGCOL.FCB) << 16);

	// The alpha test. ATST=NEVER and ATST=ALWAYS never reach the fragment stage: every fragment
	// takes the same road, so the write flags above already carry them (NEVER's AFAIL decides what
	// the draw writes; ALWAYS writes everything). What is left is a genuine per-fragment split, and
	// it is worth emitting only where the two sides actually land differently -- AFAIL=FB_ONLY
	// keeps the whole colour and drops only the depth write, so a draw that writes no depth is
	// unaffected by its own test, which is most of a game's blended geometry.
	//
	// ⚠️ Wrong-fast: the fragment stage discards a failing fragment outright. That is exact for
	// AFAIL=KEEP; for the three modes that still write something on failure it drops that write.
	// The exact form needs the draw split in two -- one draw per side of the test, each with its
	// own write masks and depth mode -- which costs an indirect run split per draw, so it waits
	// until the run structure is measured rather than guessed. Rowed in the deferred-accuracy
	// ledger.
	const bool ate_real = ctx->TEST.ATE && ctx->TEST.ATST != ATST_ALWAYS && ctx->TEST.ATST != ATST_NEVER;
	bool needs_test = false;
	if (ate_real)
	{
		switch (afail)
		{
			case AFAIL_FB_ONLY: needs_test = z_write; break;
			case AFAIL_ZB_ONLY: needs_test = color_written; break;
			default: needs_test = true; break; // KEEP writes nothing on failure; RGB_ONLY drops alpha
		}
	}
	if (needs_test)
	{
		pd.atst = static_cast<u32>(ctx->TEST.ATST) + 1;
		pd.aref = ctx->TEST.AREF;
	}
	// A draw that fails every pixel into RGB_ONLY writes colour without its alpha byte.
	pd.alpha_written = !(atst_never && afail == AFAIL_RGB_ONLY);

	// Texture inputs. Two address geometries are sampled at this stage: the direct 32-bit families
	// (PSMCT32/PSMCT24 -- one page/block/column geometry, no CLUT) and the paletted index formats
	// (PSMT8/PSMT4 -- a swizzled index into an expanded CLUT). Every other textured draw (16-bit,
	// the alpha-byte views) keeps the vertex-colour path until its format is added. The fixed TEX0
	// folds TW/TH to the used ST range, exactly as ObserveDraw derives its footprint, so the
	// dimensions the shader scales by match the draw.
	pd.tex_enable = false;
	pd.index_format = 0;
	pd.pal_offset = 0;
	GSPageBitmap tex_pages;
	if (PRIM->TME)
	{
		const bool mip = IsMipMapActive();
		const GIFRegTEX0 tex0 = ctx->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), mip);
		const u32 psm = tex0.PSM;
		const bool direct32 = (psm == PSMCT32 || psm == PSMCT24);
		const bool paletted = (psm == PSMT8 || psm == PSMT4);
		if (direct32 || paletted)
		{
			pd.tex_enable = true;
			pd.fst = PRIM->FST;
			pd.tbp0 = tex0.TBP0;
			// Pages per texture row: a CT32 page is 64 texels wide (bwpg = TBW), a paletted page is
			// 128 (bwpg = TBW>>1) -- GSOffset's bw >> (pageShiftX - 6).
			pd.tbw = direct32 ? std::max<u32>(tex0.TBW, 1) : (tex0.TBW >> 1);
			pd.tw = 1u << std::min<u32>(tex0.TW, 10);
			pd.th = 1u << std::min<u32>(tex0.TH, 10);
			pd.tfx = tex0.TFX;
			pd.tcc = tex0.TCC;
			pd.wms = ctx->CLAMP.WMS;
			pd.wmt = ctx->CLAMP.WMT;
			// The REGION wrap modes address a sub-rect of a shared texture page; the pair means
			// min/max under REGION_CLAMP and mask/or under REGION_REPEAT. GetSizeFixedTEX0 has
			// already extended TW/TH to cover whatever the region reaches.
			pd.region_u = static_cast<u32>(ctx->CLAMP.MINU) | (static_cast<u32>(ctx->CLAMP.MAXU) << 16);
			pd.region_v = static_cast<u32>(ctx->CLAMP.MINV) | (static_cast<u32>(ctx->CLAMP.MAXV) << 16);
			// LINEAR or NEAREST, from the same per-draw decision the software floor and the HW
			// renderer take: the vertex trace resolves TEX1's MMAG/MMIN against the LOD range the
			// primitive actually spans, and applies the user's filtering override. A crossover
			// primitive (the two filters differ and the LOD crossing falls inside it) takes one
			// filter for the whole draw here, which is what IsLinear already picked.
			pd.ltf = m_vt.IsLinear();
			pd.index_format = direct32 ? 0u : (psm == PSMT8 ? 1u : 2u);
			// A 24-bit texture's texels carry no alpha byte: TEXA supplies it (and AEM makes an
			// all-zero RGB texel transparent). A paletted texture takes TEXA in the CLUT expansion.
			pd.texa = (psm == PSMCT24) ? (1u | (m_env.TEXA.AEM ? 2u : 0u) |
											 (static_cast<u32>(m_env.TEXA.TA0) << 8)) :
										 0u;
			if (paletted)
			{
				// Expand this draw's CLUT to 32-bit RGBA (CSA/CPSM/TEXA applied by Read32) and append
				// it to the frame's palette stream; pal_offset is the word index of entry 0. No dedup
				// yet (wrong-fast): a palette is 16 or 256 words, a full SotC frame well under the ring.
				m_mem.m_clut.Read32(tex0, m_env.TEXA);
				const u32* clut = m_mem.m_clut;
				const u32 pal_entries = GSLocalMemory::m_psm[psm].pal;
				pd.pal_offset = static_cast<u32>(m_plan_palettes.size());
				m_plan_palettes.insert(m_plan_palettes.end(), clut, clut + pal_entries);
			}

			// The read window: the whole (size-fixed) texture under its own layout. Composed into
			// the ring for this epoch -- prefilled from S where CPU-newest, written back from any
			// target holding it newest -- BEFORE this draw's own claims move anything (a draw
			// sampling pages it also renders reads the pre-pass bytes: snapshot semantics).
			const GSTileSurfaceLayout tex_l{tex0.TBP0, static_cast<u8>(tex0.TBW), static_cast<u8>(psm), KindForPsm(psm)};
			tex_pages = GSVramModel::PagesForRect(tex_l, GSVector4i(0, 0, static_cast<int>(pd.tw), static_cast<int>(pd.th)));

			// Rule 2 first. When one resident target owns the whole window at this exact layout the
			// fragment stage samples it directly, and the byte road is not asked for anything: no
			// ring slot, no writeback, no epoch. That is where the structural saving is -- a
			// composite reading last frame's 3D buffer stops reswizzling a megabyte per frame into
			// bytes only it will read -- and it is also where the staleness goes, because the
			// target IS the newest copy while a composed slot is only as new as its writeback.
			pd.tex_source = TargetForTextureRead(tex_l, pd.tw, pd.th, tex_pages, fb_id, z_id);
			if (pd.tex_source != kGSTileNoSurface)
			{
				PlanTargetIndex(pd.tex_source); // the executor resolves the bind through the target list
				m_frame.tex_binds++;
			}
			else
			{
				ComposeForPendingDraw(tex_pages, pd);
				pd.epoch = m_epoch;
			}
		}
	}

	// Seeds: pages the target textures do not hold newest are filled out of the ring before this
	// draw's pass, so the draw opens a new pass. Only colour surfaces have a road; a depth
	// surface's missing pages stay whatever its texture held (lossy, counted).
	// A colour draw claims every colour plane and the Z plane on its pages (Tile's rule: the
	// written bytes invalidate any depth reading, and the surface holds the newest bytes of the
	// whole cell once seeded across the span). The seed gate uses the colour span -- minus the
	// pages this draw provably overwrites in full: a single sprite that writes every fragment
	// (no test can reject one, no blend reads the destination, every channel lands) covers each
	// page inside its rect entirely, so those pages need no bytes brought in first. This is what
	// keeps SotC's page-column clears (and every game's frame clear) from re-seeding what they
	// are about to overwrite.
	const u8 fb_claims = kGSTilePlanesAll;
	if (color_written)
	{
		GSPageBitmap seed = PagesNeedingSeed(fb_id, fb_pages, kGSTilePlanesColor);
		// Plus whatever of this surface's texture has never been filled. The byte model would say
		// those pages are fine -- their bytes live in the CPU shadow -- but the present reads the
		// texture, so an unfilled page reaches the screen as allocator leftovers. Paid once per
		// surface: after this the whole residency is materialised.
		seed |= Texels(fb_id).pages.andnot(Texels(fb_id).filled);
		GSPageBitmap covered;
		if (!seed.empty() && m_vt.m_primclass == GS_SPRITE_CLASS && icount == 2 && !PRIM->ABE && date == 0 && !z_test &&
			(ctx->FRAME.FBMSK & GSLocalMemory::m_psm[ctx->FRAME.PSM].fmsk) == 0 &&
			(!ctx->TEST.ATE || ctx->TEST.ATST == ATST_ALWAYS ||
				(atst_never && (afail == AFAIL_FB_ONLY || afail == AFAIL_RGB_ONLY))))
		{
			GSVramModel::RectFootprint fp;
			GSVramModel::FootprintForRect(fb_l, r, fp);
			if (!fp.overflowed)
			{
				covered = fp.pages;
				for (u32 e = 0; e < fp.edge_count; e++)
				{
					if (fp.edges[e].full != GSVramModel::kFullBlockMask)
						covered.unset(fp.edges[e].page);
				}
				// Whatever another surface held on a fully covered page is dead bytes: mark it
				// synced without moving anything, so the claim below can take the page.
				m_vram_model.OnReadback(m_vram_model.ReadbackNeeded(seed & covered, kGSTilePlanesAll));
				seed = seed.andnot(covered);
			}
		}
		if (!seed.empty())
		{
			if (HasByteRoad(fb_l))
			{
				// Whatever holds those pages' bytes now composes into the ring first (other
				// targets' writebacks, S prefill) and is marked synced -- which is also what lets
				// the claim below steal them losslessly. Then the seed reads the composed slots
				// into the target.
				ComposeRingPages(seed);
				EmitPrepOp(GSDevice::GSTileGpuPrepKind::Seed, fb_id, seed);
				pd.break_before = true;
				m_frame.seed_breaks++;
				BreakOpenPass();
				Texels(fb_id).filled |= seed;
			}
			else
			{
				LossySteal(seed);
			}
		}
		// A page this draw covers in full ends up holding texels whether it was seeded or not.
		Texels(fb_id).filled |= covered;
	}
	u8 z_claims = 0;
	if (z_used)
	{
		// Two different things share these pages and only ONE of them is lost. The depth texture
		// should hold their newest Z for a test or a write and there is no depth road to bring it
		// in, so pages it does not hold read as whatever its texture has: lossy, counted. But the
		// COLOUR truth other surfaces hold on those same pages is perfectly serviceable, and the
		// depth claim below is about to take it -- so it gets composed into the ring first, the
		// spill a road-having surface has always owed.
		//
		// ⚠️ This used to mark the whole set synced and move nothing, which is a LIE the model then
		// tells every later reader of those pages: "the ring has these bytes", and the reader
		// samples whatever the CPU shadow's prefill left. It stayed invisible only while some other
		// read of the same pages happened to write them back first; removing one such read (the
		// resident-target bind) is how FlatOut 2 surfaced it.
		const GSPageBitmap z_seed = PagesNeedingSeed(z_id, z_pages, GSTilePlaneZ);
		NoteLossyPages(z_seed);
		ComposeForPendingDraw(z_seed, pd);
		if (z_write)
			z_claims = gsTilePlanesInvalidatedByWrite(ctx->ZBUF.PSM);
	}

	// Self-read accounting: the read window intersects this draw's own colour target's pages.
	if (pd.tex_enable && !(tex_pages & fb_pages).empty())
		m_frame.self_reads++;

	// The claims: this draw's target textures now hold the newest bytes of its footprint. A
	// depth-only draw writes no colour and claims none.
	if (color_written)
		m_vram_model.OnNativeDraw(fb_id, fb_pages, fb_claims);
	if (z_write)
	{
		// The depth claim is asked what it steals HERE, between the two claims, and not with the
		// seeds above -- because the colour claim that just ran is what creates the steal. Games
		// pack FRAME and ZBUF back to back, so the two page footprints of ONE draw routinely share
		// the boundary page (Dirge of Cerberus: FRAME pages 224-280, ZBUF starting at 280, one page
		// shared per draw), and a few alias them outright (FlatOut 2 puts a 16-page FRAME and its
		// ZBUF at the same base; GT4's online beta offsets ZBUF one page into a 126-page FRAME, so
		// 112 pages are shared). The colour claim has just made itself the holder of those pages,
		// and the depth claim takes them straight back.
		//
		// Nothing that pre-dates the draw is lost -- both claims describe this same draw's own
		// output -- but the model names one holder per page per plane, so the colour surface's
		// version of those bytes is. Marked synced without moving any (the depth surface has no
		// byte road to move them onto in any case) and counted, never silent.
		//
		// The colour claim needs no such question of its own, and the depth claim needed none
		// before the colour claim ran: every claim set that contains a colour plane also contains
		// Z and vice versa (fb_claims is all four, z_claims is gsTilePlanesInvalidatedByWrite),
		// and so does every set a CPU write clears, so no surface can come to hold one of a page's
		// planes while another holds the rest -- which is the only way the seed gates above could
		// miss a steal. Measured: the pre-claim query is empty on all 18 corpus dumps. If it ever
		// stops being, the model's synced assert says so, and the answer is a ComposeRingPages of
		// the pages it names, before the claims.
		AliasSteal(m_vram_model.SpillBeforeNativeDraw(z_id, z_pages, z_claims));
		m_vram_model.OnNativeDraw(z_id, z_pages, z_claims);
	}

	// A DATE draw reads the pass snapshot; if the run of draws into this surface since the last
	// break has written pixels under it, the snapshot would be stale for those, so it opens a new
	// pass. Disjoint column sprites (SotC's depth-of-field composite) share one snapshot.
	if (m_run_surface != fb_id)
	{
		m_run_surface = fb_id;
		m_run_written = GSVector4i::zero();
	}
	if (date != 0 && !m_run_written.rempty() && !m_run_written.rintersect(r).rempty())
	{
		pd.break_before = true;
		m_run_written = GSVector4i::zero();
		m_frame.date_breaks++;
		BreakOpenPass();
	}
	if (pd.break_before)
		m_run_written = GSVector4i::zero();
	m_run_written = m_run_written.rempty() ? r : m_run_written.runion(r);
	pd.date = date;

	// The rule-2 bind takes its slot HERE, not where the source was chosen, because the slot
	// numbering belongs to the pass and everything above may still have broken the pass. Two things
	// can stop the bind joining the open pass, and both are answered by breaking it: the open pass
	// renders into the very surface this draw wants to sample (a pass cannot have one image as
	// attachment and texture at once), or its bind table is already full. A break is always legal --
	// it costs a pass, never correctness -- and after it the table is empty and the attachments are
	// this draw's own, which the eligibility test already excluded.
	if (pd.tex_source != kGSTileNoSurface)
	{
		bool need_break = m_open_color != kGSTileNoSurface &&
						  (pd.tex_source == m_open_color || (m_open_z_used && pd.tex_source == m_open_z));
		if (!need_break)
		{
			u32 s = 0;
			while (s < m_open_tex_count && m_open_tex_src[s] != pd.tex_source)
				s++;
			need_break = (s == m_open_tex_count && m_open_tex_count == m_open_tex_src.size());
		}
		if (need_break)
		{
			pd.break_before = true;
			m_frame.tex_bind_breaks++;
			m_run_written = r;
			BreakOpenPass();
		}
		u32 slot = 0;
		while (slot < m_open_tex_count && m_open_tex_src[slot] != pd.tex_source)
			slot++;
		if (slot == m_open_tex_count)
			m_open_tex_src[m_open_tex_count++] = pd.tex_source;
		pd.tex_slot = slot;
	}

	// This draw is now part of the open pass (its own, if anything above broke the previous one):
	// remember the pages it renders into and the ring pages it samples, so a later draw's
	// writeback can be tested against it. Last word in the function on pass structure -- nothing
	// below here can still break the pass.
	m_open_color = fb_id;
	m_open_z = z_id;
	m_open_z_used = z_used;
	m_open_z_write = z_write;
	m_open_z_test = z_test;
	if (color_written)
		m_open_color_written |= fb_pages;
	if (z_write)
		m_open_z_written |= z_pages;
	if (pd.tex_enable && pd.tex_source == kGSTileNoSurface)
	{
		// ComposeRingPages gave every page of the read window a live slot, and nothing between
		// there and here closes one (only an upload does, and uploads do not land mid-draw), so
		// m_ring_live still names the slot this draw's shader will read. A rule-2 draw reads no
		// slot at all, so it constrains no later writeback.
		m_open_read |= tex_pages;
		tex_pages.forEachSetPage([&](u32 page) { m_open_read_slot[page] = m_ring_live[page]; });
	}

	pd.color_surface = fb_id;
	pd.z_surface = z_id;
	pd.z_used = z_used;
	pd.z_write = z_write;
	pd.z_test = z_test;
	pd.ofx = static_cast<s32>(ctx->XYOFFSET.OFX);
	pd.ofy = static_cast<s32>(ctx->XYOFFSET.OFY);
	pd.rect = r;
	pd.scissor = ctx->scissor.in;
	pd.prep_op_count = static_cast<u32>(m_plan_prep_ops.size()) - pd.first_prep_op;
	// Make sure the surfaces are in the plan's target list even when no prep op named them.
	PlanTargetIndex(fb_id);
	if (z_used)
		PlanTargetIndex(z_id);

	// -- the geometry --------------------------------------------------------------------------
	GSDevice::GSTileGpuIndirectDraw draw = {};
	draw.instance_count = 1;
	draw.first_index = static_cast<u32>(m_plan_indices.size());
	draw.vertex_offset = static_cast<s32>(m_plan_vertices.size());
	draw.state_index = static_cast<u32>(m_plan_draws.size());

	if (!is_sprite)
	{
		// Triangle, line or point: the index list is already the topology the pipeline consumes
		// (3 indices per triangle, 2 per line, 1 per point). Copy vertices and indices straight.
		draw.index_count = icount;
		m_plan_vertices.insert(m_plan_vertices.end(), m_vertex->buff, m_vertex->buff + vcount);
		m_plan_indices.insert(m_plan_indices.end(), m_index->buff, m_index->buff + icount);
	}
	else
	{
		// Sprite corner synthesis. Each sprite is a pair of opposite corners (index[0], index[1]);
		// the GS draws it flat, taking colour, Q, Z and fog from the second vertex, while the
		// texture coordinates interpolate between the two corners (Classic's Lines2Sprites). Build
		// four corners off the second vertex, giving each its own XY and the matching UV / ST
		// pair — u from the corner's x side, v from its y side — and emit two triangles. Indices
		// are batch-local (corner s*4..s*4+3); the draw's vertex_offset rebases them into the
		// frame stream. No culling in the executor pipeline, so corner winding is free.
		const u16* RESTRICT index = m_index->buff;
		const GSVertex* RESTRICT verts = m_vertex->buff;
		const u32 sprite_count = icount / 2;
		draw.index_count = sprite_count * 6;
		m_plan_vertices.reserve(m_plan_vertices.size() + sprite_count * 4);
		m_plan_indices.reserve(m_plan_indices.size() + sprite_count * 6);
		for (u32 s = 0; s < sprite_count; s++)
		{
			const GSVertex& v0 = verts[index[s * 2 + 0]];
			const GSVertex& v1 = verts[index[s * 2 + 1]];

			GSVertex c[4] = {v1, v1, v1, v1};
			c[0].XYZ.X = v0.XYZ.X; c[0].XYZ.Y = v0.XYZ.Y; // top-left
			c[1].XYZ.X = v1.XYZ.X; c[1].XYZ.Y = v0.XYZ.Y; // top-right
			c[2].XYZ.X = v0.XYZ.X; c[2].XYZ.Y = v1.XYZ.Y; // bottom-left
			c[3].XYZ.X = v1.XYZ.X; c[3].XYZ.Y = v1.XYZ.Y; // bottom-right
			c[0].U = v0.U; c[0].V = v0.V; c[0].ST.S = v0.ST.S; c[0].ST.T = v0.ST.T;
			c[1].U = v1.U; c[1].V = v0.V; c[1].ST.S = v1.ST.S; c[1].ST.T = v0.ST.T;
			c[2].U = v0.U; c[2].V = v1.V; c[2].ST.S = v0.ST.S; c[2].ST.T = v1.ST.T;
			m_plan_vertices.push_back(c[0]);
			m_plan_vertices.push_back(c[1]);
			m_plan_vertices.push_back(c[2]);
			m_plan_vertices.push_back(c[3]);

			const u16 b = static_cast<u16>(s * 4);
			m_plan_indices.push_back(b + 0);
			m_plan_indices.push_back(b + 1);
			m_plan_indices.push_back(b + 2);
			m_plan_indices.push_back(b + 2);
			m_plan_indices.push_back(b + 1);
			m_plan_indices.push_back(b + 3);
		}
	}
	m_plan_draws.push_back(draw);
	m_plan_topologies.push_back(topology);
	// The blend: the GS ALPHA equation Cv = (A - B) * C + D as a fixed-function key (the executor
	// maps it through GSDevice::m_blendMap; As is the shader's dual-source alpha, FIX the blend
	// constant, Ad the target's alpha). ABE off, or A == B with D == Cs, is no blend at all.
	u32 blend_key = 0;
	if (PRIM->ABE && color_written)
	{
		const GIFRegALPHA& al = ctx->ALPHA;
		const bool identity = (al.A == al.B) && (al.D == 0); // (X - X)*C + Cs == Cs
		if (!identity)
			blend_key = GSDevice::GSTileGpuPassPlan::kBlendEnable | (al.A * 27u + al.B * 9u + al.C * 3u + al.D) |
						((al.C == 2) ? (static_cast<u32>(al.FIX) << 8) : 0u);
	}
	// A depth-only draw (ATST NEVER + AFAIL ZB_ONLY, or a fully masked FBMSK) rides a pipeline
	// whose colour write mask is off: it still tests and writes depth in its pass. A NEVER draw
	// into AFAIL RGB_ONLY masks the alpha channel alone.
	if (!color_written)
		blend_key |= GSDevice::GSTileGpuPassPlan::kNoColorWrite;
	else if (!pd.alpha_written)
		blend_key |= GSDevice::GSTileGpuPassPlan::kNoAlphaWrite;
	m_plan_blend_keys.push_back(blend_key);
	pd.draw_index = draw.state_index;
	m_plan_pending.push_back(pd);
}

// Group the accumulated draws into passes, resolve targets and state rows, build the ring, and
// submit. One FRAME/ZBUF pair per pass (break on target change, depth mode change, or a prep op);
// scale 1. Consumes everything: the plan streams, the ring, and the model's synced claims (the
// ring the claims vouched for is gone once submitted).
void GSRendererTileGpu::BuildAndExecutePlan()
{
	if (m_plan_pending.empty())
		return;

	if (g_gs_device->TileGpuExecutorAvailable())
	{
		// 1. Resolve the plan's target list: pool textures as they are NOW (any growth this
		//    frame has already happened, so these are the textures every draw lands in).
		m_plan_targets.resize(m_plan_target_surfaces.size());
		for (u32 k = 0; k < m_plan_target_surfaces.size(); k++)
			m_plan_targets[k] = m_target_pool.GetTexture(m_vram_model.Get(m_plan_target_surfaces[k]).pool_handle);

		// 2. The per-draw state row: the HW tfx VertexScale/VertexOffset transform against the
		//    resolved colour target's size (scale 1), plus the z enables and the texture block.
		//    Row i serves draw i.
		m_plan_states.resize(m_plan_pending.size());
		// The same slot, lifted out of the state row for the executor: it splits its indirect runs
		// on it, and the state table's layout is not part of that contract (GSTileGpuPassPlan::
		// tex_slots).
		m_plan_tex_slots.resize(m_plan_pending.size());
		for (u32 i = 0; i < m_plan_pending.size(); i++)
		{
			const PendingDraw& pd = m_plan_pending[i];
			const GSVector2i dim = m_plan_targets[m_plan_target_of_surface[pd.color_surface]]->GetSize();
			const float sx = 2.0f / static_cast<float>(dim.x << 4);
			const float sy = 2.0f / static_cast<float>(dim.y << 4);
			const float ox2 = -1.0f / static_cast<float>(dim.x);
			const float oy2 = -1.0f / static_cast<float>(dim.y);
			StateRow& sr = m_plan_states[i];
			sr.vertex_scale[0] = sx;
			sr.vertex_scale[1] = sy;
			sr.vertex_offset[0] = static_cast<float>(pd.ofx) * sx + ox2 + 1.0f;
			sr.vertex_offset[1] = static_cast<float>(pd.ofy) * sy + oy2 + 1.0f;
			sr.z_write = pd.z_write ? 1u : 0u;
			sr.z_test = pd.z_test ? 1u : 0u;
			sr.tex_enable = pd.tex_enable ? 1u : 0u;
			sr.fst = pd.fst ? 1u : 0u;
			sr.tbp0 = pd.tbp0;
			sr.tbw = pd.tbw;
			sr.tw = pd.tw;
			sr.th = pd.th;
			sr.tfx = pd.tfx;
			sr.tcc = pd.tcc;
			sr.wms = pd.wms;
			sr.wmt = pd.wmt;
			sr.index_format = pd.index_format;
			sr.pal_offset = pd.pal_offset;
			sr.epoch = pd.epoch;
			sr.date = pd.date;
			sr.ofx = pd.ofx;
			sr.ofy = pd.ofy;
			sr.sc_x0 = pd.scissor.x;
			sr.sc_y0 = pd.scissor.y;
			sr.sc_x1 = pd.scissor.z;
			sr.sc_y1 = pd.scissor.w;
			sr.fge = pd.fge ? 1u : 0u;
			sr.fogcol = pd.fogcol;
			sr.atst = pd.atst;
			sr.aref = pd.aref;
			sr.texa = pd.texa;
			sr.region_u = pd.region_u;
			sr.region_v = pd.region_v;
			sr.ltf = pd.ltf ? 1u : 0u;
			sr.tex_target = pd.tex_slot;
			m_plan_tex_slots[i] = pd.tex_slot;
			sr.pad0_ = 0;
		}

		// 3. Group contiguous draws sharing a colour+depth surface pair and depth mode into one
		//    pass. The depth pipeline is per-pass fixed function, so z-write and z-test must be
		//    uniform across the pass (a ZTST=ALWAYS write sprite and a ZTST=GEQUAL 3D draw share
		//    a depth buffer in SotC). Each pass carries the prep ops its draws emitted, in order
		//    (each draw's ops are contiguous and appended in draw order, so the range is [first
		//    draw's first, last draw's last]) and the executor runs the whole range BEFORE the
		//    pass opens -- so an op belonging to any draw but the first is hoisted over the draws
		//    ahead of it. Accumulation decides whether that is allowed, per op: a seed always
		//    breaks (it is a fragment pass over the target that has to land between the draws),
		//    and a writeback breaks only where it collides with what the open pass has already
		//    written or sampled (WritebackHoistCollides). A non-breaking writeback therefore
		//    belongs to a later draw and is hoisted BY DESIGN, having been proved invisible.
		u32 i = 0;
		while (i < m_plan_draws.size())
		{
			const PendingDraw& first = m_plan_pending[i];
			u32 j = i + 1;
			while (j < m_plan_draws.size())
			{
				const PendingDraw& pd = m_plan_pending[j];
				if (pd.break_before || pd.color_surface != first.color_surface || pd.z_used != first.z_used ||
					(pd.z_used && pd.z_surface != first.z_surface) || pd.z_write != first.z_write ||
					pd.z_test != first.z_test)
					break;
				j++;
			}
			const PendingDraw& last = m_plan_pending[j - 1];

			GSDevice::GSTileGpuTargetPair tp = {};
			tp.frame_target = m_plan_target_of_surface[first.color_surface];
			tp.zbuf_target = first.z_used ? m_plan_target_of_surface[first.z_surface] : GSDevice::GSTileGpuPassPlan::kNoTarget;

			GSDevice::GSTileGpuPass pass = {};
			pass.first_draw = i;
			pass.draw_count = j - i;
			pass.first_target_pair = static_cast<u32>(m_plan_target_pairs.size());
			pass.target_pair_count = 1;
			pass.first_prep_op = first.first_prep_op;
			pass.prep_op_count = (last.first_prep_op + last.prep_op_count) - first.first_prep_op;
			pass.declares_self_read = false;

			// The pass's rule-2 bind table, rebuilt by walking its draws in order and appending each
			// source the first time it appears. That is exactly how accumulation numbered the slots
			// (its m_open_tex_src list resets on the same breaks this grouping does), so the slot a
			// state row carries has to come out the same -- asserted, because a silent disagreement
			// here would sample the wrong target rather than fail.
			pass.first_tex_source = static_cast<u32>(m_plan_tex_sources.size());
			pass.tex_source_count = 0;
			for (u32 d = i; d < j; d++)
			{
				const PendingDraw& pd = m_plan_pending[d];
				if (pd.tex_source == kGSTileNoSurface)
					continue;
				const u32 ti = m_plan_target_of_surface[pd.tex_source];
				u32 slot = 0;
				while (slot < pass.tex_source_count && m_plan_tex_sources[pass.first_tex_source + slot] != ti)
					slot++;
				if (slot == pass.tex_source_count)
				{
					m_plan_tex_sources.push_back(ti);
					pass.tex_source_count++;
				}
				pxAssertMsg(slot == pd.tex_slot, "TileGpu rule-2 slot disagrees with the pass's bind table");
			}
			// A pass with a DATE draw snapshots its colour target before opening.
			pass.first_snapshot = static_cast<u32>(m_plan_snapshots.size());
			pass.snapshot_count = 0;
			for (u32 d = i; d < j; d++)
			{
				if (m_plan_pending[d].date != 0)
				{
					const GSVector2i tsz = m_plan_targets[tp.frame_target]->GetSize();
					m_plan_snapshots.push_back(GSDevice::GSTileGpuSnapshotCopy{tp.frame_target, GSVector4i(0, 0, tsz.x, tsz.y)});
					pass.snapshot_count = 1;
					m_frame.snapshots++;
					break;
				}
			}
			// GS depth grows towards the viewer: a real test is GEQUAL, a write-only draw is ALWAYS,
			// and the write follows ZMSK independently of the test. z_used == (z_write || z_test).
			pass.depth_mode = !first.z_used ? GSDevice::GSTileGpuDepthMode::None
				: (first.z_test ? (first.z_write ? GSDevice::GSTileGpuDepthMode::TestWrite
												 : GSDevice::GSTileGpuDepthMode::TestNoWrite)
								: GSDevice::GSTileGpuDepthMode::WriteAlways);

			m_plan_target_pairs.push_back(tp);
			m_plan_passes.push_back(pass);
			i = j;
		}
		m_frame.passes += static_cast<u32>(m_plan_passes.size());

		// 3b. Backstop for the framebuffer-fits-its-attachments clamp. The executor renders each
		//     pass into min(colour, depth) of the pair (a framebuffer may not be bigger than the
		//     attachment it carries), so a draw whose bottom falls below that minimum height would
		//     be silently clipped. It cannot: both surfaces grew to cover every draw's footprint
		//     (EnsureSurface), so min(colour,depth) height >= every draw's bottom by construction,
		//     and this loop never fires. It is a guard against a future grouping change breaking
		//     that invariant. (Width is the layout's stride, shared across a pair by FBW.)
		for (const GSDevice::GSTileGpuPass& pass : m_plan_passes)
		{
			const GSDevice::GSTileGpuTargetPair& tp = m_plan_target_pairs[pass.first_target_pair];
			if (tp.zbuf_target == GSDevice::GSTileGpuPassPlan::kNoTarget)
				continue; // no depth attachment, no clamp
			const int lim_h = std::min(m_plan_targets[tp.frame_target]->GetSize().y,
				m_plan_targets[tp.zbuf_target]->GetSize().y);
			for (u32 d = pass.first_draw; d < pass.first_draw + pass.draw_count; d++)
			{
				if (m_plan_pending[d].rect.w > lim_h)
				{
					Console.Error("TileGpu: draw %u bottom %d exceeds pass target height %d -- clamp would clip it",
						d, m_plan_pending[d].rect.w, lim_h);
					pxAssertMsg(false, "TileGpu pass draw exceeds min(colour,depth) target height");
				}
			}
		}

		// 4. The ring: one entry per (page, epoch range), its source bytes resolved now -- the
		//    CPU shadow as it stands at the end of the frame for a live slot, or the version copy
		//    taken when an upload superseded it. Slots nobody prefills are the GPU's to compose.
		std::vector<GSDevice::GSTileGpuRingPage> ring;
		ring.reserve(m_ring_entries.size());
		for (const RingEntry& e : m_ring_entries)
		{
			GSDevice::GSTileGpuRingPage rp = {};
			rp.page = e.page;
			rp.epoch_first = e.epoch_first;
			rp.epoch_last = (e.epoch_last == 0xFFFF) ? static_cast<u16>(m_epoch) : e.epoch_last;
			rp.src = !e.prefill_s ? nullptr
					 : e.versioned ? static_cast<const void*>(m_ring_versions.data() + e.version_offset)
								   : static_cast<const void*>(m_mem.vm8() + static_cast<u32>(e.page) * GS_PAGE_SIZE);
			if (rp.src)
				m_frame.ring_prefill++;
			ring.push_back(rp);
		}
		m_frame.epochs = std::max(m_frame.epochs, m_epoch + 1);

		// 5. Assemble the plan over the frame's streams and submit. The spans are CPU-side
		//    views alive until the clear below; the executor stages what it needs during the
		//    synchronous call.
		GSDevice::GSTileGpuPassPlan plan;
		plan.passes = m_plan_passes;
		plan.draws = m_plan_draws;
		plan.topologies = m_plan_topologies;
		plan.blend_keys = m_plan_blend_keys;
		plan.tex_slots = m_plan_tex_slots;
		plan.target_pairs = m_plan_target_pairs;
		plan.snapshots = m_plan_snapshots;
		plan.prep_ops = m_plan_prep_ops;
		plan.page_entries = m_plan_page_entries;
		plan.state_table = m_plan_states.data();
		plan.state_stride = sizeof(StateRow);
		plan.state_count = static_cast<u32>(m_plan_states.size());
		plan.vertices = std::span<const u8>(reinterpret_cast<const u8*>(m_plan_vertices.data()),
			m_plan_vertices.size() * sizeof(GSVertex));
		plan.vertex_stride = sizeof(GSVertex);
		plan.indices = m_plan_indices;
		plan.targets = m_plan_targets;
		plan.tex_sources = m_plan_tex_sources;
		plan.ring_pages = ring;
		plan.epoch_count = m_epoch + 1;
		plan.palettes = m_plan_palettes;

		g_gs_device->ExecuteTileGpuPassPlan(plan);
	}

	// The plan is submitted (or the device does not serve it); reset the per-frame streams and
	// the ring. Every synced claim referred to slots that are now spent.
	m_plan_vertices.clear();
	m_plan_indices.clear();
	m_plan_states.clear();
	m_plan_palettes.clear();
	m_plan_draws.clear();
	m_plan_topologies.clear();
	m_plan_blend_keys.clear();
	m_plan_tex_slots.clear();
	m_plan_pending.clear();
	m_plan_passes.clear();
	m_plan_target_pairs.clear();
	m_plan_snapshots.clear();
	m_plan_prep_ops.clear();
	m_plan_page_entries.clear();
	m_plan_targets.clear();
	m_plan_tex_sources.clear();
	m_plan_target_surfaces.clear();
	m_plan_target_of_surface.clear();
	m_ring_entries.clear();
	m_ring_versions.clear();
	m_ring_live.fill(0);
	m_epoch = 0;
	m_run_surface = kGSTileNoSurface;
	m_run_written = GSVector4i::zero();
	BreakOpenPass();
	m_vram_model.ClearAllSynced();
}

// -- the stall road ------------------------------------------------------------------------

void GSRendererTileGpu::FlushPendingPlan()
{
	if (m_plan_pending.empty())
		return;
	m_frame.flushes++;
	BuildAndExecutePlan();
}

// Pull whatever of `pages` a target holds newest down into the CPU shadow. The targets must
// hold every draw so far first, so the pending plan is flushed (a mid-frame submission,
// counted) -- which also drops every ring-synced claim, so the question "what does S not
// have" is asked afterwards. Then the pool's synchronous readback per owner surface, masked to
// the planes and blocks each holds. The one road on which the CPU waits for the GPU.
void GSRendererTileGpu::ReadbackToShadow(const GSPageBitmap& pages, StallSite site)
{
	// Nothing any target holds newest: no crossing (a CLUT load or a local read of plain CPU
	// memory, the common case). Asked against truth rather than the unsynced subset, because a
	// ring-synced claim is not one S can serve.
	if (pages.empty() || (pages & m_vram_model.TruthAny()).empty())
		return;

	FlushPendingPlan();
	const GSPageBitmap need = m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll);
	if (need.empty())
		return;
	m_frame.stalls[static_cast<u32>(site)]++;
	m_frame.stall_pages[static_cast<u32>(site)] += need.count();

	need.forEachSetPage([&](u32 page) {
		GSPageBitmap one;
		one.set(page);
		for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		{
			if (!m_vram_model.Truth(pi).test(page) || m_vram_model.SyncedPages(pi).test(page))
				continue;
			const GSTileSurfaceId owner = m_vram_model.OwnerOf(page, pi);
			pxAssert(owner != kGSTileNoSurface);
			const GSVramModel::Surface& surf = m_vram_model.Get(owner);
			if (!surf.pool_handle)
				continue;
			m_target_pool.ReadbackPages(m_mem, surf.pool_handle, surf.layout, one, PlaneByteMask(pi, surf.layout),
				m_vram_model.TruthMask(page, pi));
		}
	});
	m_vram_model.OnReadback(need);
}

// Whole-of-truth: every page any target holds newest comes down. Synced claims are dropped
// first because on this road "synced" may mean the ring holds the bytes, and the ring is not
// the CPU shadow.
void GSRendererTileGpu::SyncAllTruthToCpu()
{
	m_vram_model.ClearAllSynced();
	ReadbackToShadow(m_vram_model.TruthAny(), StallSite::SyncAll);
}
