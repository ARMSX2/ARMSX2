// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GSRendererTileGpu.h"

#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
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

// Pages per texture ROW for a sampled window, in the one spelling every road has to share: the
// state row the fragment stage reads, the materialise op that builds a source out of the same
// bytes, and the donor op that reinterprets them out of a target. Two roads disagreeing here means
// one window addressing two different sets of bytes, which looks like plausible texture from the
// wrong place.
//
// GSOffset's own term, bw >> (pageShiftX - 6), which GSTileSwizzleForms::PagesPerRow reads off the
// format's page WIDTH. ⚠️ Width, never "is it paletted": PSMT8H/PSMT4HL/PSMT4HH have a palette and
// CT32's 64-texel pages, so the TBW >> 1 that PSMT8 needs would put every row but the first in the
// wrong page for them.
//
// Plus the direct-colour road's floor at one page, which is not GSOffset's. It predates this
// helper, it only ever applied to PSMCT32/PSMCT24, and a TBW=0 texture is degenerate either way --
// so it stays exactly where it was rather than being tidied into a byte-moving change.
u32 TextureWindowBwPg(u32 psm, u32 tbw)
{
	if (psm == PSMCT32 || psm == PSMCT24)
		return std::max<u32>(tbw, 1);
	return GSTileSwizzleForms::PagesPerRow(psm, tbw);
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

	// Arm the pin discipline. Until a cache is given a non-zero frame it behaves exactly as it does
	// for the Tile renderer, which draws immediately and needs none of this; this renderer records
	// draws and issues them at the plan, so a texture one of them names must survive to the end of
	// that plan.
	AdvanceSourcePinFrame();
}

void GSRendererTileGpu::AdvanceSourcePinFrame()
{
	m_source_frame++;
	m_tex_source.SetFrame(m_source_frame);
	m_palette_cache.SetFrame(m_source_frame);
	m_expand_cache.SetFrame(m_source_frame);
}

GSRendererTileGpu::~GSRendererTileGpu()
{
	ReportPassStructure();
}

void GSRendererTileGpu::Destroy()
{
	// Textures must go back to the device pool while the device is alive; the pool's
	// destructor only checks that they did. The source caches own device textures on the same
	// terms (none yet -- nothing builds into them at this chunk -- but the teardown order is
	// theirs whether they hold anything or not).
	m_tex_source.Clear();
	m_palette_cache.Clear();
	m_expand_cache.Clear();
	ReleaseGpuPalettes();
	m_target_pool.ReleaseAll();
	GSRenderer::Destroy();
}

void GSRendererTileGpu::Reset(bool hardware_reset)
{
	GSRenderer::Reset(hardware_reset);
	// Everything falls back to CPU-newest (the reset rewrote local memory); surface textures
	// die with it, and so does the frame in progress. Device palettes go with the textures they
	// were gathered out of -- the CLUT RAM the reset leaves behind is the CPU's again.
	ReleaseGpuPalettes();
	m_target_pool.ReleaseAll();
	m_vram_model.Reset();
	m_surface_texels.clear();
	m_surface_version.clear();
	// The source caches key on the model's write generations, which the reset just rewound, and
	// the probe's window record on the same. Both die with it or a replay reaching equal counts
	// under different bytes would look current.
	m_tex_source.Clear();
	m_palette_cache.Clear();
	m_expand_cache.Clear();
	m_probe_windows.clear();
	m_probe_prev.clear();
	m_plan_vertices.clear();
	m_plan_indices.clear();
	m_plan_states.clear();
	m_plan_palettes.clear();
	m_plan_draws.clear();
	m_plan_topologies.clear();
	m_plan_blend_keys.clear();
	m_plan_bind_keys.clear();
	m_plan_pending.clear();
	m_plan_passes.clear();
	m_plan_target_pairs.clear();
	m_plan_snapshots.clear();
	m_plan_prep_ops.clear();
	m_plan_page_entries.clear();
	m_plan_targets.clear();
	m_plan_prep_textures.clear();
	m_plan_sources.clear();
	m_plan_tex_sources.clear();
	m_plan_target_surfaces.clear();
	m_plan_target_of_surface.clear();
	m_ring_entries.clear();
	m_ring_versions.clear();
	m_ring_live.fill(0);
	m_epoch = 0;
	AdvanceSourcePinFrame();
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

	// The source probe's frame boundary. This frame's window record becomes what the next frame
	// compares its content stamps against, and the expanded cache's admission clock ticks -- a
	// pair first seen this frame is admitted no earlier than the next one, which is the filter
	// doing its job rather than a refusal. m_frame.src_distinct already holds the number of
	// source slots the frame handed out, counted as they were handed out.
	m_probe_prev.swap(m_probe_windows);
	m_probe_windows.clear();
	m_expand_cache.NextFrame();

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
		pd.src_slot = kNoSourceSlot;
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
//
// A CLUT load's blocks get one more question first: can the palette be read off the GPU instead?
// Where it can, the readback -- and the plan flush in front of it -- does not happen at all, which
// is the whole prize (the flush count and the CLUT stall count are numerically the same population
// on the two GT4 dumps). PreClutLoad reads the deferred blocks back after all if the load's shape
// turns out to be one the gather cannot serve.
void GSRendererTileGpu::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	const GSTileSurfaceLayout layout{BITBLTBUF.SBP, static_cast<u8>(BITBLTBUF.SBW),
		static_cast<u8>(BITBLTBUF.SPSM), KindForPsm(BITBLTBUF.SPSM)};
	const GSPageBitmap pages = GSVramModel::PagesForRect(layout, r);
	if (!clut || !ClutLoadDefer(pages))
	{
		// Bracketed on the clut road so the census counts the same population `stalls[Clut]` does.
		// "Did a target hold these pages" is the question the gather has to ask (it is the only one
		// the plan flush cannot change the answer to), but it is not the same question as "did a
		// readback happen": a page whose bytes the ring already vouches for answers yes to the first
		// and no to the second, and counting those as refusals would inflate the refused column by
		// twenty a frame on GT4 for loads that cost nothing.
		const u32 before = m_frame.stalls[static_cast<u32>(StallSite::Clut)];
		ReadbackToShadow(pages, clut ? StallSite::Clut : StallSite::LocalRead);
		if (clut && m_frame.stalls[static_cast<u32>(StallSite::Clut)] != before) [[unlikely]]
			m_clut_pending.readback = true;
	}
	if (m_pass_sim.IsActive() && !m_pass_sim_in_move) [[unlikely]]
		m_pass_sim.OnCpuRead(PagesForTargetRect(layout, r), clut);
}

// -- the CLUT gather: a palette loaded off a render target ------------------------------------

void GSRendererTileGpu::ClutLoadSlots(const GIFRegTEX0& TEX0, u32& first, u32& count)
{
	const u32 csa = TEX0.CSA & 15;
	first = csa;
	count = (GSLocalMemory::m_psm[TEX0.PSM].pal == 256) ? (16 - csa) : 1;
}

bool GSRendererTileGpu::ClutGatherServesLoad(const GIFRegTEX0& TEX0)
{
	const u32 pal = GSLocalMemory::m_psm[TEX0.PSM].pal;
	if (TEX0.CSM != 0 || (TEX0.CPSM & 0x2) != 0 || pal == 0)
		return false;
	return pal == 16 || TEX0.CSA == 0;
}

bool GSRendererTileGpu::ClutGatherServes()
{
	if (!m_clut_gather_probed)
	{
		m_clut_gather_probed = true;
		bool clut_ok = false;
		// The entry -> word order is a fitted form with its own validity bit: a device whose CLUT
		// forms did not fit must not gather, even where the block forms did. The renderer's own fit
		// is the same function over the same tables -- it exists because the block copy's source
		// rects are a CPU-side answer no shader computes.
		const bool forms_ok = g_gs_device && g_gs_device->TileSwizzleFormsFit(clut_ok);
		m_clut_forms = GSTileSwizzleForms::Fit();
		m_clut_gather_serves = forms_ok && clut_ok && m_clut_forms.valid && m_clut_forms.clut_valid;
		// Deliberately NOT gated on bindless targets, unlike every other road that reads one: the
		// consumer that carries 88% of the population is an image-to-buffer COPY into the frame's
		// palette stream, which asks the device for nothing. Rule 3's N x 1 gather does need the
		// source array, and it is already unreachable without it (ProbeSourceRoad is the gate).
	}
	return m_clut_gather_serves;
}

// One block of a CLUT load, asked the donor road's own owner questions about the pages it is about
// to pull down. The clause bundle is the design probe's, which is DonorForTextureRead's asked of a
// palette's source instead of a texture window: one live owner for every plane of every page,
// holding a pool texture, a page-aligned CT32/CT24 colour layout, resident, with texels actually
// written there, and the palette at or after the owner's base.
//
// ⚠️ The first question is the cheap one and it has to stay that way. sotc runs ~1789 CLUT loads a
// frame and not one of them needs anything; the whole road must cost those loads a single member
// check. It also has to be a question the plan flush cannot change its answer to -- "does any target
// hold these pages newest" is exactly that, where "does a readback need to happen" is not (a synced
// claim the ring vouches for dies with the flush, which is why the design probe had to measure from
// inside ReadbackToShadow and this does not).
bool GSRendererTileGpu::ClutLoadDefer(const GSPageBitmap& pages)
{
	ClutPendingLoad& p = m_clut_pending;
	p.calls++;
	if (pages.empty() || (pages & m_vram_model.TruthAny()).empty())
		return false; // the CPU shadow is authoritative here; today's road early-outs on the same test
	p.stalling = true;
	if (p.refused || !ClutGatherServes())
		return false;

	u32 refusal = kClutRefNone;
	GSTileSurfaceId owner = m_vram_model.SoleGpuOwner(pages, kGSTilePlanesAll);
	if (owner == kGSTileNoSurface)
	{
		refusal = kClutRefMulti;
	}
	else
	{
		const GSVramModel::Surface& s = m_vram_model.Get(owner);
		if (!s.alive || !s.pool_handle)
			refusal = kClutRefDead;
		else if (!HasByteRoad(s.layout))
			refusal = kClutRefLayout;
		else if (!s.residency.contains(pages))
			refusal = kClutRefResidency;
		else if (m_surface_texels.size() <= owner || !pages.andnot(m_surface_texels[owner].filled).empty())
			refusal = kClutRefTexels;
		else if (p.deferred_blocks != 0 && p.owner != owner)
			refusal = kClutRefMixedOwner;
	}
	if (refusal != kClutRefNone)
	{
		p.refused = true;
		p.refusal = refusal;
		return false;
	}

	if (p.deferred_blocks == 0)
	{
		const GSVramModel::Surface& s = m_vram_model.Get(owner);
		p.owner = owner;
		p.owner_bp = s.layout.bp;
		p.owner_bwpg = s.layout.bw;
	}
	p.pages |= pages;
	p.deferred_blocks++;
	return true;
}

// The load is about to run against the CPU's local memory. Two things have to be settled first: a
// load of a shape the sixteen-slot mirror does not model rewrites the CLUT RAM in units it cannot
// name, so the RAM is made whole before it; and blocks whose readback was deferred come back after
// all when the whole load is not one the gather serves, because then the loader below needs them.
void GSRendererTileGpu::PreClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT)
{
	ClutPendingLoad& p = m_clut_pending;
	m_frame.clut_loads++;

	const bool csm1_32 = TEX0.CSM == 0 && (TEX0.CPSM & 0x2) == 0;
	if (!csm1_32 && m_clut_mirror.AnyGpu()) [[unlikely]]
		SyncClutToCpu();

	if (p.deferred_blocks != 0 && (p.refused || !ClutGatherServesLoad(TEX0)))
	{
		// Part of the load is deferrable and part is not, or the shape is not one the gather can
		// serve: the whole load takes the CPU route and the deferred blocks come down now.
		const u32 before = m_frame.stalls[static_cast<u32>(StallSite::Clut)];
		ReadbackToShadow(p.pages, StallSite::Clut);
		if (m_frame.stalls[static_cast<u32>(StallSite::Clut)] != before)
			p.readback = true;
		p.deferred_blocks = 0;
	}
	if (p.deferred_blocks != 0)
		return; // PostClutLoad turns it into a record, and counts it there

	// The census, over the population `stalls[Clut]` counts: a load that cost nothing is not a
	// refusal of anything, and on the titles the gather never serves it is every load there is.
	if (!p.readback)
		m_frame.clut_no_stall++;
	else if (p.refusal != kClutRefNone)
	{
		m_frame.clut_ref_owner++;
		m_frame.clut_ro[p.refusal < kClutRefCount ? p.refusal : 0u]++;
	}
	else
	{
		// The load's own shape is not one the gather serves (CSM2, a 16-bit palette, an eight-bit
		// index at a non-zero CSA) -- or the device does not serve the road at all.
		m_frame.clut_ref_shape++;
	}
}

// The load has happened. TEX0 names its shape and the mirror slots it wrote, so this is where the
// pending classification becomes either a device palette or an ordinary CPU one.
void GSRendererTileGpu::PostClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT)
{
	const ClutPendingLoad p = m_clut_pending;
	m_clut_pending = ClutPendingLoad{};

	// Nothing on the device and nothing deferred: the mirror is all-CPU and stays that way, so the
	// whole road costs this load one branch. That is the case on every title the gather never serves
	// -- sotc's 1789 loads a frame, xenosaga's 1321 -- and it has to stay the case.
	if (m_gpu_palettes.empty() && p.deferred_blocks == 0)
		return;

	const bool csm1_32 = TEX0.CSM == 0 && (TEX0.CPSM & 0x2) == 0;
	if (!csm1_32)
	{
		// PreClutLoad made the RAM whole; the load then rewrote it from CPU bytes, in units the
		// mirror cannot name, so every slot is the CPU's again.
		m_clut_mirror.OnCpuLoad(0, GSTileClutMirror::kSlots);
		RefreshClutLive();
		return;
	}

	u32 first = 0, count = 0;
	ClutLoadSlots(TEX0, first, count);
	if (p.deferred_blocks == 0)
	{
		m_clut_mirror.OnCpuLoad(first, count);
		RefreshClutLive();
		return;
	}

	// The palette lives on the device from here: the CPU's CLUT RAM holds whatever the loader just
	// read out of a stale shadow for these slots, and the mirror says so. Fresh record per load, no
	// lookup structure -- measured, not assumed: on both prize titles every eligible load's identity
	// tuple is new, so a cache could not hit.
	GpuPalette gp;
	gp.handle = m_gpu_palette_next++;
	gp.owner = p.owner;
	gp.cbp = TEX0.CBP;
	gp.entries = GSLocalMemory::m_psm[TEX0.PSM].pal;
	gp.first_slot = first;
	gp.owner_bp = p.owner_bp;
	gp.owner_bwpg = p.owner_bwpg;
	gp.pages = p.pages;
	gp.tex0 = TEX0;
	gp.texclut = TEXCLUT;
	// The identity: (owner surface x owner version x CBP x entries), namespace-tagged. The version
	// is the same counter the donor road's target token keys on, which is what makes "this palette's
	// words" mean the same thing to both roads.
	u64 h = 0xCBF29CE484222325ull;
	for (const u64 part : {static_cast<u64>(p.owner), SurfaceVersion(p.owner), static_cast<u64>(gp.cbp),
			 static_cast<u64>(gp.entries)})
	{
		h ^= part;
		h *= 0x100000001B3ull;
	}
	gp.pal_id = GSTilePaletteCache::ForeignPalId(h);

	m_gpu_palettes.push_back(gp);
	m_clut_mirror.OnGpuLoad(first, count, gp.handle);
	RefreshClutLive();
	m_frame.clut_gathered++;
}

GSRendererTileGpu::GpuPalette* GSRendererTileGpu::FindGpuPalette(u32 handle)
{
	// Handles only ever increase and pruning preserves order, so the vector is sorted by handle.
	const auto it = std::lower_bound(m_gpu_palettes.begin(), m_gpu_palettes.end(), handle,
		[](const GpuPalette& gp, u32 h) { return gp.handle < h; });
	return (it != m_gpu_palettes.end() && it->handle == handle) ? &*it : nullptr;
}

void GSRendererTileGpu::RefreshClutLive()
{
	m_clut_live.clear();
	if (m_gpu_palettes.empty())
		return;
	for (u32 s = 0; s < GSTileClutMirror::kSlots; s++)
	{
		const u32 handle = m_clut_mirror.SlotHandle(s);
		if (handle == GSTileClutMirror::kCpu)
			continue;
		const GpuPalette* gp = FindGpuPalette(handle);
		if (!gp)
			continue;
		const u32 idx = static_cast<u32>(gp - m_gpu_palettes.data());
		if (std::find(m_clut_live.begin(), m_clut_live.end(), idx) == m_clut_live.end())
			m_clut_live.push_back(idx);
	}
}

// A pool texture's pages are ABOUT TO BE written -- by a draw claiming them, or by a seed queued
// just after this. Every live palette gathered out of those pages is losing the only copy of its
// words, so it is CAPTURED first: its blocks are copied into the frame's palette stream now, from
// the op position ahead of the write, and the rest of the plan reads them from there.
//
// It has to be eager rather than at the next consumer, and the corpus said so before the design
// did: GT4 renders its palettes into the frame buffer it then draws over, so a record loses its
// source mid-plan while draws that still read it are yet to come. The capture is a transfer at a
// pass head, not a pass, so paying it per record is affordable in a way a gather pass is not.
//
// No hoist test here, and none is needed: the FIRST write to a record's pages is what runs this, so
// no draw of the open pass can already have written them -- if one had, this would have run then.
void GSRendererTileGpu::NoteClutSourceWritten(GSTileSurfaceId id, const GSPageBitmap& pages)
{
	if (m_clut_live.empty()) [[likely]]
		return;
	for (const u32 idx : m_clut_live)
	{
		GpuPalette& gp = m_gpu_palettes[idx];
		if (gp.source_lost || gp.owner != id || !gp.pages.intersects(pages))
			continue;
		CaptureClutRecordWords(gp);
		gp.source_lost = true;
	}
}

// The surface id has been handed to a NEW surface, so whatever texture a palette on it named holds
// somebody else's pixels now, on every page.
void GSRendererTileGpu::NoteClutSurfaceReplaced(GSTileSurfaceId id)
{
	if (m_clut_live.empty()) [[likely]]
		return;
	for (const u32 idx : m_clut_live)
	{
		GpuPalette& gp = m_gpu_palettes[idx];
		if (gp.owner == id)
			gp.source_lost = true;
	}
}

// A prep op that reads an owner target cannot be hoisted over a draw of the open pass that wrote
// that target's pixels -- the same hazard a donor build has, and the same answer: the draw opens its
// own pass, where the op runs after everything that came before it.
void GSRendererTileGpu::ClutHoistBreak(const GpuPalette& gp, PendingDraw& pd)
{
	if (!DonorHoistCollides(gp.owner, gp.pages))
		return;
	pd.break_before = true;
	m_frame.clut_breaks++;
	BreakOpenPass();
}

bool GSRendererTileGpu::CaptureClutRecordWords(GpuPalette& gp)
{
	if (gp.stream_plan == m_source_frame)
		return true; // already in this plan's stream; one copy serves every consumer of the record
	if (gp.source_lost)
		return false;

	GSTileSwizzleForms::ClutBlockCopy blocks;
	if (!GSTileSwizzleForms::LocateClutBlocks(m_clut_forms, gp.cbp, gp.owner_bp, gp.owner_bwpg, gp.entries, blocks))
		return false;

	// A reservation, not an append: the words are zero until the copy fills them, and no CPU palette
	// ever crosses into the stream for a draw on this record.
	gp.stream_offset = static_cast<u32>(m_plan_palettes.size());
	gp.stream_plan = m_source_frame;
	m_plan_palettes.resize(m_plan_palettes.size() + gp.entries, 0);

	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::ClutBlockCopy;
	op.donor_target = PlanTargetIndex(gp.owner);
	op.bp = gp.stream_offset;
	op.copy_count = blocks.region_count;
	op.copy_w = blocks.w;
	op.copy_h = blocks.h;
	for (u32 b = 0; b < blocks.region_count; b++)
	{
		op.copy_x[b] = blocks.x[b];
		op.copy_y[b] = blocks.y[b];
	}
	m_plan_prep_ops.push_back(op);
	m_frame.clut_copies++;
	return true;
}

bool GSRendererTileGpu::EnsureClutStreamWords(GpuPalette& gp, PendingDraw& pd)
{
	const bool fresh = (gp.stream_plan != m_source_frame);
	if (!CaptureClutRecordWords(gp))
		return false;
	// Only a copy emitted for THIS draw can be hoisted over the open pass; one an earlier draw's
	// capture already emitted has run its test where it was queued.
	if (fresh)
		ClutHoistBreak(gp, pd);
	return true;
}

GSTexture* GSRendererTileGpu::ClutGatherTexture(GpuPalette& gp, PendingDraw& pd)
{
	if (gp.gather_tex)
		return gp.gather_tex;
	if (gp.source_lost)
		return nullptr;
	GSTexture* const tex =
		g_gs_device->CreateRenderTarget(static_cast<int>(gp.entries), 1, GSTexture::Format::Color, false, true);
	if (!tex)
		return nullptr;

	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::ClutGather;
	op.target = PrepTextureIndex(tex);
	op.donor_target = PlanTargetIndex(gp.owner);
	op.bp = gp.cbp;
	op.owner_bp = gp.owner_bp;
	op.owner_bwpg = gp.owner_bwpg;
	m_plan_prep_ops.push_back(op);
	m_frame.clut_gathers++;
	gp.gather_tex = tex;
	ClutHoistBreak(gp, pd);
	return tex;
}

// Which device palette a draw's slots resolve to. A four-bit index reads slot CSA; an eight-bit one
// at CSA 0 reads all sixteen; an eight-bit one at a non-zero CSA reads a window the sixteen-slot
// mirror cannot name, so it takes the always-correct road -- the CPU words, made whole first.
GSRendererTileGpu::GpuPalette* GSRendererTileGpu::ResolveDrawPalette(const GIFRegTEX0& tex0, u32 pal_entries,
	u32& first_texel, bool& synced)
{
	first_texel = 0;
	synced = false;
	if (m_gpu_palettes.empty() || !m_clut_mirror.AnyGpu()) [[likely]]
		return nullptr;

	u32 first, count;
	if (pal_entries == 16)
	{
		first = tex0.CSA & 15;
		count = 1;
	}
	else if (tex0.CSA == 0)
	{
		first = 0;
		count = GSTileClutMirror::kSlots;
	}
	else
	{
		m_frame.clut_draws_sync++;
		SyncClutToCpu();
		synced = true;
		return nullptr;
	}

	const GSTileClutMirror::Resolution r = m_clut_mirror.Resolve(first, count);
	if (r.verdict == GSTileClutMirror::Verdict::Cpu)
		return nullptr;
	if (r.verdict == GSTileClutMirror::Verdict::Mixed)
	{
		m_frame.clut_draws_sync++;
		SyncClutToCpu();
		synced = true;
		return nullptr;
	}

	GpuPalette* const gp = FindGpuPalette(r.handle);
	if (!gp)
	{
		// The mirror names a record nothing holds. Cannot happen (the mirror and the record list are
		// retired together), and if it ever does the CPU road is the safe answer.
		m_frame.clut_draws_sync++;
		SyncClutToCpu();
		synced = true;
		return nullptr;
	}
	if (gp->source_lost && gp->stream_plan != m_source_frame)
	{
		// The owner's texels on the palette's pages were overwritten after the load, and its words
		// were not copied anywhere first: they are gone. Nothing can recover them -- a readback of
		// those pages now would hand back whatever was rendered over them -- so this is counted and
		// said out loud rather than served silently, and the record is retired so it costs the stale
		// CPU words at most once. Expected to be zero: the window between a palette's source being
		// overwritten and its slots being displaced by the next load carries no draw that reads it.
		RetireLostClutRecord(*gp);
		return nullptr;
	}
	first_texel = r.first_texel;
	m_frame.clut_draws++;
	return gp;
}

void GSRendererTileGpu::RetireLostClutRecord(GpuPalette& gp)
{
	m_frame.clut_lost++;
	if (!m_warned_clut_lost)
	{
		m_warned_clut_lost = true;
		Console.Warning("TileGpu: a palette gathered off a render target was read after its source pixels had been "
						"overwritten -- its words are unrecoverable and the CPU's stale CLUT is used instead. "
						"Counted per frame as lost CLUT records.");
	}
	for (u32 s = 0; s < GSTileClutMirror::kSlots; s++)
	{
		if (m_clut_mirror.SlotHandle(s) == gp.handle)
			m_clut_mirror.OnCpuLoad(s, 1);
	}
	RefreshClutLive();
}

// Every device palette's words back into the CPU's CLUT RAM. The seam every genuine CPU consumer of
// the palette goes through -- a savestate, a renderer switch, a load of a shape the mirror cannot
// model -- and exact-Tile's SyncClutToCpu contract: make the words whole, then hand the slots back.
void GSRendererTileGpu::SyncClutToCpu()
{
	// Driven off the MIRROR rather than off a snapshot of the record list, and that is not
	// stylistic: syncing a record reads pages back, a readback flushes the pending plan, and the
	// plan's tail PRUNES the record list -- so any index or pointer taken before the loop is stale
	// inside it. Each pass hands one record's slots back, so at most sixteen passes run.
	for (u32 guard = 0; guard <= GSTileClutMirror::kSlots; guard++)
	{
		u32 handle = GSTileClutMirror::kCpu;
		for (u32 s = 0; s < GSTileClutMirror::kSlots && handle == GSTileClutMirror::kCpu; s++)
			handle = m_clut_mirror.SlotHandle(s);
		if (handle == GSTileClutMirror::kCpu)
			break;
		GpuPalette* const gp = FindGpuPalette(handle);
		if (!gp)
		{
			// The mirror names a record nothing holds -- it cannot, the two are retired together,
			// and if it ever does the CPU road is the safe answer for those slots.
			for (u32 s = 0; s < GSTileClutMirror::kSlots; s++)
			{
				if (m_clut_mirror.SlotHandle(s) == handle)
					m_clut_mirror.OnCpuLoad(s, 1);
			}
			continue;
		}
		SyncClutRecordToCpu(*gp);
	}
	RefreshClutLive();
}

void GSRendererTileGpu::SyncClutRecordToCpu(GpuPalette& gp)
{
	if (gp.source_lost)
	{
		RetireLostClutRecord(gp);
		return;
	}
	m_frame.clut_syncs++;
	// ⚠️ Everything this needs is taken BEFORE the readback: the readback flushes the plan, whose
	// tail may reallocate the record list, and `gp` does not survive that.
	const u32 handle = gp.handle;
	const u32 cbp = gp.cbp;
	const u32 entries = gp.entries;
	const u32 first_slot = gp.first_slot;
	const GSPageBitmap pages = gp.pages;

	// The owner still holds the load's words, so a readback of its pages puts them back in the CPU
	// shadow exactly as the loader read them -- the same bytes today's un-gathered road loads from.
	ReadbackToShadow(pages, StallSite::Clut);

	if (!m_clut_word_map_taken)
	{
		m_clut_word_map_taken = true;
		GSClut::EntryToWordCSM1_32(m_clut_word_i8.data(), m_clut_word_i4.data());
	}
	const u32* const src = reinterpret_cast<const u32*>(m_mem.BlockPtr32(0, 0, cbp, 1));
	std::array<u32, 256> words{};
	for (u32 e = 0; e < entries; e++)
		words[e] = src[(entries == 256) ? m_clut_word_i8[e] : m_clut_word_i4[e]];

	// Only the slots this record still owns: a later load may have taken some of them, and writing
	// those back would clobber a palette that is current.
	for (u32 s = 0; s < GSTileClutMirror::kSlots; s++)
	{
		if (m_clut_mirror.SlotHandle(s) != handle)
			continue;
		const u32 texel = (s - first_slot) * GSTileClutMirror::kEntriesPerSlot;
		m_mem.m_clut.SetEntries32(s * GSTileClutMirror::kEntriesPerSlot, GSTileClutMirror::kEntriesPerSlot,
			words.data() + texel);
		m_clut_mirror.OnCpuLoad(s, 1);
	}
	RefreshClutLive();
}

// Records the mirror no longer names are recycled here and NOWHERE else. Tile prunes at the load,
// and that is unsound under deferred consumption: a draw recorded into the open plan may still name
// a palette whose slots a later load has already displaced, and its gather texture would be handed
// back to the device under it. At the plan tail every recorded op has issued and there are no
// pending draws, so Tile's own predicate becomes correct as written.
void GSRendererTileGpu::PruneGpuPalettes()
{
	if (m_gpu_palettes.empty())
		return;
	const size_t before = m_gpu_palettes.size();
	std::vector<GpuPalette> kept;
	kept.reserve(m_gpu_palettes.size());
	for (GpuPalette& gp : m_gpu_palettes)
	{
		if (m_clut_mirror.References(gp.handle))
		{
			kept.push_back(gp);
			continue;
		}
		if (gp.gather_tex)
			g_gs_device->Recycle(gp.gather_tex);
	}
	m_gpu_palettes.swap(kept);
	m_frame.clut_pruned += static_cast<u32>(before - m_gpu_palettes.size());
	RefreshClutLive();
}

void GSRendererTileGpu::ReleaseGpuPalettes()
{
	for (const GpuPalette& gp : m_gpu_palettes)
	{
		if (gp.gather_tex)
			g_gs_device->Recycle(gp.gather_tex);
	}
	m_gpu_palettes.clear();
	m_clut_live.clear();
	m_clut_mirror.Reset();
	m_clut_pending = ClutPendingLoad{};
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
	if (sources)
	{
		// Materialised sources are a cache of guest bytes, so dropping them costs rebuilds and
		// nothing else. Inert at this chunk (nothing builds into them yet), wired now because a
		// half-connected lifetime is the kind of thing that surfaces as a stale texture later.
		m_tex_source.Clear();
		m_palette_cache.Clear();
		m_expand_cache.Clear();
		m_probe_windows.clear();
		m_probe_prev.clear();
	}
	if (!targets)
		return;
	// The palettes come down with everything else (SyncAllTruthToCpu), and then the textures they
	// were gathered into go back to the device with the targets.
	SyncAllTruthToCpu();
	ReleaseGpuPalettes();
	m_target_pool.ReleaseAll();
	m_vram_model.Reset();
	m_surface_texels.clear();
	m_surface_version.clear();
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
	const auto uncomp = stat([](const MF& f) { return f.prefill_uncomposed; });
	const auto snaps = stat([](const MF& f) { return f.snapshots; });
	const auto self = stat([](const MF& f) { return f.self_reads; });
	const auto binds = stat([](const MF& f) { return f.tex_binds; });
	const auto bindbrk = stat([](const MF& f) { return f.tex_bind_breaks; });
	const auto alias = stat([](const MF& f) { return f.alias_steal_pages; });
	const auto lossy = stat([](const MF& f) { return f.lossy_pages; });
	const auto skipped = stat([](const MF& f) { return f.skipped_draws; });
	const auto flushes = stat([](const MF& f) { return f.flushes; });
	const auto afold_f = stat([](const MF& f) { return f.atst_fold_fail; });
	const auto afold_p = stat([](const MF& f) { return f.atst_fold_pass; });
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
					"version copies %.2f / %u, epochs %.2f / %u, prefilled for uncomposed blocks %.2f / %u)",
		surf.mean, surf.p50, passes.mean, passes.p50, ring.mean, ring.p50, prefill.mean, prefill.p50, versions.mean,
		versions.p50, epochs.mean, epochs.p50, uncomp.mean, uncomp.p50);
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
	Console.WriteLn("  alpha test folded at plan time (constant fragment alpha): all-fail %.2f / %u   "
					"all-pass %.2f / %u",
		afold_f.mean, afold_f.p50, afold_p.mean, afold_p.p50);
	Console.WriteLn("  stalls: upload sub-block %.2f/%u (%.2f pages)  local-read %.2f/%u (%.2f)  clut %.2f/%u (%.2f)  "
					"sync-all %.2f/%u (%.2f)",
		s_up.mean, s_up.p50, p_up.mean, s_rd.mean, s_rd.p50, p_rd.mean, s_cl.mean, s_cl.p50, p_cl.mean, s_all.mean,
		s_all.p50, p_all.mean);

	// Rule 3 as probed. Nothing in this block changed a pixel: it says what a cache of
	// materialised sources WOULD have served, and for the rest, exactly which clause refused.
	const auto tdraws = stat([](const MF& f) { return f.tex_draws; });
	const auto tunsup = stat([](const MF& f) { return f.tex_unsupported; });
	const auto thi = stat([](const MF& f) { return f.tex_hi_draws; });
	const auto elig = stat([](const MF& f) { return f.src_eligible; });
	const auto shit = stat([](const MF& f) { return f.src_hit; });
	const auto sreb = stat([](const MF& f) { return f.src_rebuild; });
	const auto sfresh = stat([](const MF& f) { return f.src_fresh; });
	const auto rregion = stat([](const MF& f) { return f.src_ref_region; });
	const auto smip = stat([](const MF& f) { return f.src_mip_draws; });
	const auto smipl = stat([](const MF& f) { return f.src_mip_live; });
	const auto rcap = stat([](const MF& f) { return f.src_ref_capacity; });
	const auto rpal = stat([](const MF& f) { return f.src_ref_palette; });
	const auto rpin = stat([](const MF& f) { return f.src_ref_pin; });
	const auto sdist = stat([](const MF& f) { return f.src_distinct; });
	const auto scalls = stat([](const MF& f) { return f.src_extra_calls; });
	const auto skept = stat([](const MF& f) { return f.src_stamp_kept; });
	const auto smoved = stat([](const MF& f) { return f.src_stamp_moved; });
	const auto srescued = stat([](const MF& f) { return f.src_stamp_rescued; });
	const auto sopaque = stat([](const MF& f) { return f.src_stamp_opaque; });
	// Per frame, not p50-minus-p50: rescued is a subset of moved within one frame, so the
	// difference is a real per-frame quantity and its median is the one worth printing.
	const auto sredo = stat([](const MF& f) { return f.src_stamp_moved - f.src_stamp_rescued; });
	// Served share of textured draws, the checkpoint's headline: rule 2's binds plus rule 3's
	// eligible draws over the draws in a format either road samples.
	const double served = (tdraws.mean > 0.0) ? (binds.mean + elig.mean) * 100.0 / tdraws.mean : 0.0;
	const double stamp_total = skept.mean + smoved.mean;

	// Rule 3 as BUILT and SERVED, over the whole eligible population -- direct colour materialises to
	// the image a draw samples, paletted to an index image the palette expansion turns into one.
	const auto sbuild = stat([](const MF& f) { return f.src_builds; });
	const auto schit = stat([](const MF& f) { return f.src_cache_hits; });
	const auto scresc = stat([](const MF& f) { return f.src_cache_rescued; });
	const auto scpin = stat([](const MF& f) { return f.src_pin_refusals; });
	const auto sccap = stat([](const MF& f) { return f.src_cap_refusals; });
	const auto scfail = stat([](const MF& f) { return f.src_build_failed; });
	const auto sserved = stat([](const MF& f) { return f.src_served; });
	const auto sbind = stat([](const MF& f) { return f.src_bind_slots; });
	const auto rbind = stat([](const MF& f) { return f.src_ref_bind; });
	// The share of textured draws whose fragment stage did NOT decode ring bytes: rule 2's binds plus
	// the rule-3 draws that were actually served. `src_eligible` above is the admission question's
	// answer, taken before the build could refuse anything -- so this is the honest one.
	const double sampled = (tdraws.mean > 0.0) ? (binds.mean + sserved.mean) * 100.0 / tdraws.mean : 0.0;

	// The donor road: rule 3's build taken off the owning target instead of off the ring. Its served
	// draws are inside src_served above -- the ROAD a draw takes is SOURCE either way -- so these
	// columns are about what did not have to happen for them, not about a different road.
	const auto delig = stat([](const MF& f) { return f.src_donor_eligible; });
	const auto dserved = stat([](const MF& f) { return f.src_donor_served; });
	const auto dbuild = stat([](const MF& f) { return f.src_donor_builds; });
	const auto dhit = stat([](const MF& f) { return f.src_donor_hits; });
	const auto dresc = stat([](const MF& f) { return f.src_donor_rescued; });
	const auto dref = stat([](const MF& f) { return f.src_donor_refused; });
	const auto dbreak = stat([](const MF& f) { return f.src_donor_breaks; });
	const auto ddirect = stat([](const MF& f) { return f.src_donor_direct; });

	const auto pdraws = stat([](const MF& f) { return f.src_pal_draws; });
	const auto pmiss = stat([](const MF& f) { return f.src_pal_missing; });
	const auto ehit = stat([](const MF& f) { return f.src_expand_hits; });
	const auto ebuild = stat([](const MF& f) { return f.src_expand_builds; });
	const auto edefer = stat([](const MF& f) { return f.src_expand_defer; });
	const auto ecap = stat([](const MF& f) { return f.src_expand_cap; });
	const auto efail = stat([](const MF& f) { return f.src_expand_failed; });

	Console.WriteLn("TileGpu source road (rule 3: direct colour and paletted, BUILT and SAMPLED):");
	Console.WriteLn("  textured draws %.2f / %-5u (alpha-byte views %.2f / %u)  rule 2 served %.2f / %-5u  "
					"rule 3 eligible %.2f / %-5u  => %.1f%% of textured draws served",
		tdraws.mean, tdraws.p50, thi.mean, thi.p50, binds.mean, binds.p50, elig.mean, elig.p50, served);
	Console.WriteLn("  eligible by cost: hit %.2f / %-5u  rebuild %.2f / %-5u  first build %.2f / %u",
		shit.mean, shit.p50, sreb.mean, sreb.p50, sfresh.mean, sfresh.p50);
	Console.WriteLn("  refused: format %.2f/%u  region wrap %.2f/%u  palette first sight %.2f/%u  pin %.2f/%u  "
					"capacity %.2f/%u",
		tunsup.mean, tunsup.p50, rregion.mean, rregion.p50, rpal.mean, rpal.p50, rpin.mean, rpin.p50, rcap.mean,
		rcap.p50);
	Console.WriteLn("  mipmapped draws (eligible, served at level 0 as every road serves them today) %.2f/%u, "
					"mipmapping live on %.2f/%u of them",
		smip.mean, smip.p50, smipl.mean, smipl.p50);
	Console.WriteLn("  distinct source windows %.2f / %-4u  bound (image x sampler) slots %.2f / %-4u "
					"(array holds %u, bind-table refusals %.2f/%u)   extra indirect calls from the split %.2f / %u",
		sdist.mean, sdist.p50, sbind.mean, sbind.p50, kMaxSourceSlots, rbind.mean, rbind.p50, scalls.mean,
		scalls.p50);
	Console.WriteLn("  windows vs the frame before: gen stamp held %.2f / %-4u  moved %.2f / %-4u  (%.1f%% held)",
		skept.mean, skept.p50, smoved.mean, smoved.p50, stamp_total > 0.0 ? skept.mean * 100.0 / stamp_total : 0.0);
	Console.WriteLn("  of the moved: same bytes after all %.2f / %-4u  rebuilt %.2f / %-4u  (of those, %.2f/%u "
					"unprovable: GPU-side truth)  => %.1f%% effectively stable",
		srescued.mean, srescued.p50, sredo.mean, sredo.p50, sopaque.mean, sopaque.p50,
		stamp_total > 0.0 ? (skept.mean + srescued.mean) * 100.0 / stamp_total : 0.0);
	Console.WriteLn("  materialised: builds %.2f / %-4u  cache hits %.2f / %-4u  same-bytes rescues %.2f / %u",
		sbuild.mean, sbuild.p50, schit.mean, schit.p50, scresc.mean, scresc.p50);
	Console.WriteLn("  donor (built off the owning target, no compose): eligible %.2f / %-4u  served %.2f / %-4u  "
					"builds %.2f / %-4u  hits %.2f / %-4u  target-identity rescues %.2f / %-4u  refused %.2f/%u  "
					"pass breaks %.2f/%u   (direct-colour windows a copy leg would serve %.2f/%u)",
		delig.mean, delig.p50, dserved.mean, dserved.p50, dbuild.mean, dbuild.p50, dhit.mean, dhit.p50, dresc.mean,
		dresc.p50, dref.mean, dref.p50, dbreak.mean, dbreak.p50, ddirect.mean, ddirect.p50);
	Console.WriteLn("  paletted second stage: draws %.2f / %-4u  expansion hits %.2f / %-4u  passes %.2f / %-4u  "
					"deferred at build %.2f / %-4u  (palette missing %.2f/%u, capacity %.2f/%u, failed %.2f/%u)",
		pdraws.mean, pdraws.p50, ehit.mean, ehit.p50, ebuild.mean, ebuild.p50, edefer.mean, edefer.p50, pmiss.mean,
		pmiss.p50, ecap.mean, ecap.p50, efail.mean, efail.p50);
	Console.WriteLn("  SERVED by rule 3 (the draw sampled the image) %.2f / %-5u  => %.1f%% of textured draws "
					"sampled an image rather than decoding bytes",
		sserved.mean, sserved.p50, sampled);
	Console.WriteLn("  cache refusals: pinned by a recorded draw %.2f / %-4u  every entry pinned %.2f / %-4u  "
					"build failed %.2f / %u   (cache totals: %llu pin, %llu capacity; palette %llu, expand %llu)",
		scpin.mean, scpin.p50, sccap.mean, sccap.p50, scfail.mean, scfail.p50,
		static_cast<unsigned long long>(m_tex_source.PinRefusals()),
		static_cast<unsigned long long>(m_tex_source.CapacityRefusals()),
		static_cast<unsigned long long>(m_palette_cache.CapacityRefusals()),
		static_cast<unsigned long long>(m_expand_cache.CapacityRefusals()));

	// The CLUT gather. A load whose source words were rendered by a native draw stops being read
	// back at all; its words reach the byte road as a block copy into the frame's palette stream and
	// rule 3 as an N x 1 gather. What this block has to show is (a) that the machinery is invisible
	// where it serves nothing -- no-stall is the whole population on sotc and xenosaga -- and (b)
	// that clut stalls above have collapsed to the refused population and nothing else.
	const auto cl = stat([](const MF& f) { return f.clut_loads; });
	const auto cnos = stat([](const MF& f) { return f.clut_no_stall; });
	const auto cgat = stat([](const MF& f) { return f.clut_gathered; });
	const auto crs = stat([](const MF& f) { return f.clut_ref_shape; });
	const auto cro = stat([](const MF& f) { return f.clut_ref_owner; });
	const auto cmulti = stat([](const MF& f) { return f.clut_ro[kClutRefMulti]; });
	const auto cdead = stat([](const MF& f) { return f.clut_ro[kClutRefDead]; });
	const auto clay = stat([](const MF& f) { return f.clut_ro[kClutRefLayout]; });
	const auto cres = stat([](const MF& f) { return f.clut_ro[kClutRefResidency]; });
	const auto ctex = stat([](const MF& f) { return f.clut_ro[kClutRefTexels]; });
	const auto cmix = stat([](const MF& f) { return f.clut_ro[kClutRefMixedOwner]; });
	const auto cd = stat([](const MF& f) { return f.clut_draws; });
	const auto cd3 = stat([](const MF& f) { return f.clut_draws_r3; });
	const auto cdb = stat([](const MF& f) { return f.clut_draws_byte; });
	const auto cds = stat([](const MF& f) { return f.clut_draws_sync; });
	const auto cr3r = stat([](const MF& f) { return f.clut_r3_refused; });
	const auto ccp = stat([](const MF& f) { return f.clut_copies; });
	const auto cgp = stat([](const MF& f) { return f.clut_gathers; });
	const auto cbrk = stat([](const MF& f) { return f.clut_breaks; });
	const auto csy = stat([](const MF& f) { return f.clut_syncs; });
	const auto clost = stat([](const MF& f) { return f.clut_lost; });
	const auto cpr = stat([](const MF& f) { return f.clut_pruned; });
	if (cl.mean > 0.0)
	{
		Console.WriteLn("TileGpu CLUT gather (a palette loaded off a render target):");
		Console.WriteLn("  loads %.2f / %-5u  no-stall %.2f / %-5u  gathered %.2f / %-5u  refused: shape %.2f/%u  "
						"owner %.2f/%u",
			cl.mean, cl.p50, cnos.mean, cnos.p50, cgat.mean, cgat.p50, crs.mean, crs.p50, cro.mean, cro.p50);
		Console.WriteLn("  owner refusals: multi/partial %.2f  dead %.2f  layout %.2f  residency %.2f  texels %.2f  "
						"mixed-owner %.2f",
			cmulti.mean, cdead.mean, clay.mean, cres.mean, ctex.mean, cmix.mean);
		Console.WriteLn("  gathered draws %.2f / %-5u  (rule 3 %.2f / %-4u, byte road %.2f / %-5u, rule-3 refused "
						"%.2f/%u)   mirror could not serve %.2f / %u draws",
			cd.mean, cd.p50, cd3.mean, cd3.p50, cdb.mean, cdb.p50, cr3r.mean, cr3r.p50, cds.mean, cds.p50);
		Console.WriteLn("  block copies %.2f / %-5u  gather passes %.2f / %-4u  pass breaks %.2f / %-4u  "
						"cpu syncs %.2f / %-4u  lost records %.2f / %-4u  pruned %.2f / %u",
			ccp.mean, ccp.p50, cgp.mean, cgp.p50, cbrk.mean, cbrk.p50, csy.mean, csy.p50, clost.mean, clost.p50,
			cpr.mean, cpr.p50);
	}
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
		// A fresh allocation holds whatever the pool left there, and this id may be a slot some
		// earlier surface used: a donor source cached under the old surface's version must not be
		// served for the new one, and neither must a palette gathered off it.
		BumpSurfaceVersion(id);
		NoteClutSurfaceReplaced(id);
		NoteTextureGeometry(id, height);
		return id;
	}
	m_vram_model.GrowResidency(id, pages);
	// Growth copies the texture's content into a taller one on the device now -- before any of
	// this frame's passes are recorded (the plan is built at VSync), so it moves last frame's
	// pixels and every draw of this frame lands in the grown texture.
	const int old_height = Texels(id).height;
	if (!m_target_pool.EnsureHeight(m_vram_model.Get(id).pool_handle, height))
		return kGSTileNoSurface;
	if (old_height != height)
		BumpSurfaceVersion(id); // a re-allocated texture is a different image, whatever it copied
	NoteTextureGeometry(id, height);
	return id;
}

// "This surface's texels changed." The donor road's whole content identity rests on this counter,
// so what bumps it is the specification: every write of a pool texture and nothing else. Those are
// a draw rendering into the surface, a seed filling it out of the ring, and the pool allocating or
// growing it -- three call sites, all in this file. Globally monotonic rather than per-surface, so
// a surface id coming back out of the model's free list cannot reproduce a version its predecessor
// already filed under the same window key.
void GSRendererTileGpu::BumpSurfaceVersion(GSTileSurfaceId id)
{
	if (id == kGSTileNoSurface)
		return;
	if (m_surface_version.size() <= id)
		m_surface_version.resize(id + 1, 0);
	m_surface_version[id] = ++m_surface_version_counter;
}

u64 GSRendererTileGpu::SurfaceVersion(GSTileSurfaceId id) const
{
	return (id != kGSTileNoSurface && m_surface_version.size() > id) ? m_surface_version[id] : 0;
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

// Rule 3's DONOR build road: can one live target serve this window whole, by reinterpretation?
//
// This is the class rule 2 refuses by construction. Rule 2 binds a target only where the READ's own
// layout is the target's, and a palettised read of a colour surface never is -- the game is reading
// the target's BYTES as indices, at a different page geometry, usually at an offset into it. The
// byte road serves it by writing the whole target back into the frame's ring and unswizzling those
// bytes into an index image: two GPU passes and a megabyte of ring slots for bytes that were on the
// GPU the entire time. The device can rearrange them in place instead (GSTileSwizzleForms, the same
// arithmetic ps_tile_reinterpret_index performs), and then the window also has an IDENTITY -- its
// owner's -- where a page fingerprint had nothing to say about it.
//
// Every clause is a proof, because a wrong donor is silent wrong output:
//
//  - every page GPU-newest and NOT SYNCED, which is the clause the corpus had to teach and the one
//    the block below explains at length.
//  - ONE owner for EVERY plane of every page of the window, at whole-page granularity. All planes,
//    not just colour: the reinterpretation reads any byte of the owner's RGBA cell, so the alpha
//    byte has to be the owner's too.
//  - not the draw's own colour or depth target -- that is the feedback road, and this stage has none.
//  - a page-aligned CT32/CT24 colour layout (HasByteRoad), which is what makes the owner's pixel
//    space the guest layout: pool textures are linear from the base page.
//  - the owner's texture actually holding texels on those pages. The model tracks BYTES, and a page
//    whose bytes the surface owns but whose rows nothing has written yet is in perfect order by the
//    model and allocator leftovers to a sampler. Rule 2's own clause, for the same reason.
//  - the window starting at or after the owner's base. The shader computes `blk - dst_bp` unsigned;
//    the page proof implies this, and it is asserted here rather than trusted.
//  - a format the swizzle forms express as an index: PSMT8/PSMT4 and the three alpha-byte views.
bool GSRendererTileGpu::DonorForTextureRead(const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages,
	GSTileSurfaceId fb_id, GSTileSurfaceId z_id, DonorPlan& out)
{
	out = DonorPlan{};
	if (tex_pages.empty())
		return false;

	// ⚠️ Every page has to be truth the owner's TEXTURE actually holds -- GPU-newest and NOT synced.
	// A synced page is one the model says the byte store already has an equal copy of, and on two
	// roads it says that without having moved a byte (the alias steal and the road-less steal, both
	// rowed in the deferred-accuracy ledger). On those pages the target's pixels and the ring's bytes
	// legitimately disagree, so a donor build and the byte road would produce different texels for
	// the same window -- measured on R&C UYA, where it moved both dumps away from the software
	// golden. The Tile renderer's own donor asks the identical question (its `sync_tex` is
	// ReadbackNeeded over the window, and it only reaches the donor when that set is non-empty).
	const GSPageBitmap need = m_vram_model.ReadbackNeeded(tex_pages, kGSTilePlanesAll);
	if (!tex_pages.andnot(need).empty())
		return false;

	const GSTileSurfaceId owner = m_vram_model.SoleGpuOwner(tex_pages, kGSTilePlanesAll);
	if (owner == kGSTileNoSurface || owner == fb_id || owner == z_id)
		return false;

	const GSVramModel::Surface& surf = m_vram_model.Get(owner);
	if (!surf.alive || !surf.pool_handle || !HasByteRoad(surf.layout))
		return false;
	if (!surf.residency.contains(tex_pages))
		return false;
	if (m_surface_texels.size() <= owner || !tex_pages.andnot(m_surface_texels[owner].filled).empty())
		return false;
	if (tex0.TBP0 < surf.layout.bp)
		return false;

	const int idx_fmt = GSTileSwizzleForms::IndexFormatFor(tex0.PSM);
	if (idx_fmt < 0)
	{
		// A direct-colour window one target owns whole that rule 2 did NOT take -- an offset into
		// the target, or a different stride, or a CT32 read of a CT24 owner. Serving it needs a copy
		// leg this road does not have (the reinterpretation only produces index formats), so it
		// keeps the byte road. Counted so the size of that population is a measurement rather than a
		// guess when someone asks whether the leg is worth building.
		m_frame.src_donor_direct++;
		return false;
	}

	out.owner = owner;
	out.src_bp = tex0.TBP0;
	// Pages per texture row, through the one spelling the byte road and the materialise also take.
	// The two roads have to agree here or the same window addresses two different sets of bytes --
	// and this used to be a bare TBW >> 1, correct for PSMT8/PSMT4 and wrong for the alpha-byte
	// views, whose pages are 64 texels wide.
	out.src_bwpg = TextureWindowBwPg(tex0.PSM, tex0.TBW);
	out.owner_bp = surf.layout.bp;
	out.owner_bwpg = surf.layout.bw;
	out.psm = tex0.PSM;
	return true;
}

// A donor build reads an IMAGE, so unlike a materialise it is not free to hoist: it runs at the head
// of its draw's pass, ahead of every draw already in that pass, and one of those may be what wrote
// the pixels it is about to read. The writeback's test, narrowed -- a donor rewrites no ring slot,
// so the slot half of WritebackHoistCollides has nothing to say about it.
bool GSRendererTileGpu::DonorHoistCollides(GSTileSurfaceId owner, const GSPageBitmap& pages) const
{
	if (owner == m_open_color && pages.intersects(m_open_color_written))
		return true;
	if (m_open_z_used && owner == m_open_z && pages.intersects(m_open_z_written))
		return true;
	return false;
}

// Rule 3 of the same texel road -- the cache of materialised texture sources -- asked and
// deliberately not taken. The road itself is not built yet; what is built here is the question it
// will ask, run against the live stream so the answer is measured instead of assumed: for every
// textured draw rule 2 declined, would a source cache serve this window, and if not, which clause
// refused it. Nothing is built, nothing is bound, and the caller goes on to compose the byte road
// exactly as it did before this existed.
//
// Every refusal lands in its own counter. A silent fallthrough would report the road as unusable
// without saying why, and "the road is wrong" and "the road is not being taken" are the two
// answers the checkpoint has to tell apart.
//
// The probe models BOTH tiers of the cache's invalidation predicate, because the first tier alone
// answers the wrong question. A gen stamp says a byte on the window's pages was written; the
// fingerprint says whether any of them changed. On this corpus those are not the same number
// anywhere it matters, and a probe that stopped at the stamp reported churn the cache would not
// have had -- churn that then propagated into every identity derived from it, which is what made
// the palette-admission column read as a wall rather than as a first frame.
u32 GSRendererTileGpu::ProbeSourceRoad(const PendingDraw& pd, const GIFRegTEX0& tex0, const GSPageBitmap& tex_pages,
	bool paletted, bool mip_active, const GSTileTextureSource::ContentToken& donor_token)
{
	const GSDrawingContext* ctx = m_context;

	// The wrap modes a sampler expresses exactly at tw x th. The two REGION modes address a
	// sub-rect of a shared texture page and have no sampler spelling, so they keep the byte road:
	// a performance trade, not an accuracy one, and it is in the bill either way.
	if (ctx->CLAMP.WMS >= CLAMP_REGION_CLAMP || ctx->CLAMP.WMT >= CLAMP_REGION_CLAMP)
	{
		m_frame.src_ref_region++;
		return kNoSourceSlot;
	}
	// Level 0, and a draw declaring a pyramid is served at level 0 like every other one. This was
	// a refusal and is not any more, because it refused a third of the corpus (1595 of
	// rcuya-gameplay's 1608 textured draws, 582 on the SotC gate scene) to buy nothing: NO TileGpu
	// road mipmaps today -- the byte road samples level 0 for every draw whatever TEX1 says -- and
	// rule 3 with maxLod pinned to 0 samples the identical level. Refusing here would have moved
	// those draws from one level-0 road to another level-0 road.
	//
	// What is deferred is therefore not mipmapping-versus-not; it is correct GS LOD selection,
	// which nothing has today and which arrives in M4 as a shader-variant axis (the GS's K/L and
	// MMIN modes are not the hardware's derivative-based LOD). Counted in its own two columns
	// because the second one is the population M4 has to serve: a draw whose pyramid is declared
	// but not active already samples level 0 correctly.
	if (ctx->TEX1.MXL != 0)
	{
		m_frame.src_mip_draws++;
		if (mip_active)
			m_frame.src_mip_live++;
	}

	// Ask the cache. It holds nothing until a later chunk builds into it, so today it answers "no
	// entry" for every window and the frame-to-frame record below is what classifies the draw;
	// from the chunk that builds, this is the authority and the record only adds what the cache
	// cannot know -- which windows a draw already recorded THIS frame has named. The probe hands
	// back the content stamp it compared against, which is the whole invalidation predicate: the
	// sum of the per-page write generations over the window, monotone under both a CPU write and
	// a native draw, so stamp inequality is exactly "some byte under this window moved". Walking
	// those pages is the bulk of what this probe adds to the GS thread, and it happens once.
	const GSTileTextureSource::WindowKey key = GSTileTextureSource::KeyFor(tex0, m_env.TEXA);
	const GSTileTextureSource::ProbeResult pr = m_tex_source.Probe(m_vram_model, tex0, m_env.TEXA, tex_pages);
	const u64 stamp = pr.gen_stamp;

	// The second tier of the same predicate, and on this corpus the load-bearing one. A moved gen
	// stamp says some byte on the window's PAGES was written, not that any texel changed, and
	// games re-upload identical texture data every frame: measured on 11 of the 18 dumps, the
	// stamp holds for exactly nothing, so a road that believed it would rebuild every source every
	// frame, and every identity derived from it -- the palette pairs the expanded cache admits on
	// -- would churn with it and never admit. So when the stamp has moved, ask the pages what
	// their bytes actually are. Lazily, and at most once per draw: only a moved stamp needs the
	// fingerprint, and the library memoises per page off the write generations, so a page
	// rewritten with the bytes it already held is hashed once per frame however many windows read
	// it. This is the tier Lookup runs as RebuildsSamePages (97.7% of GT4's rebuilds), modelled
	// here rather than assumed away.
	//
	// ...unless a DONOR serves the window, in which case the fingerprint is not the tier at all. The
	// bytes under a window whose truth is a rendered target are not in the CPU shadow, so the
	// fingerprint declines and returns nothing, having first hashed nothing useful; what identifies
	// that window is the owning target and its version, and the caller has already worked that out.
	// Taking it here rather than falling back to the declining tier is what retires the whole
	// "unprovable rebuild" class -- 18.4 of SotC's 24.5 believed rebuilds a frame, and all of
	// GT4-OPB's -- and it removes an 8 KB-per-moved-page hash on the way.
	GSTileTextureSource::ContentToken content = donor_token;
	bool content_taken = donor_token.Known();
	const auto ContentStamp = [&]() -> GSTileTextureSource::ContentToken {
		if (!content_taken)
		{
			const u64 h = m_tex_source.ProbePageContent(m_mem, m_vram_model, tex_pages);
			content = GSTileTextureSource::ContentToken{
				h != 0 ? GSTileTextureSource::ContentToken::Source::Pages :
						 GSTileTextureSource::ContentToken::Source::None,
				h};
			content_taken = true;
		}
		return content;
	};
	// Does the window still hold the content this record was taken under? An unknown token on either
	// side is "cannot say" -- the fingerprint declined and no donor answers for it -- and cannot-say
	// is never the same content, which ContentToken's own equality already says.
	const auto SameBytes = [&](const ProbedWindow& p) { return ContentStamp() == p.content; };

	// This frame's record of the window, if some earlier draw already probed it.
	u32 wi = 0;
	while (wi < m_probe_windows.size() && !(m_probe_windows[wi].key == key))
		wi++;

	// Deferred execution against an immediate cache. A draw already recorded this frame names
	// this window's entry, and a rebuild would land IN PLACE, in the same entry and the same
	// texture -- so at VSync that earlier draw would sample bytes it never asked for. The pin
	// discipline refuses instead: this draw takes the byte road and the rebuild happens next
	// frame naturally. Fail-closed by construction, and counted rather than assumed rare.
	//
	// A stamp that moved without the bytes moving is not a rebuild at all: Lookup serves the entry
	// it already has and nothing lands in place, so there is nothing for an earlier draw to be
	// protected from. The pin closes on a window whose bytes really did change under it.
	if (wi < m_probe_windows.size() && m_probe_windows[wi].admitted && m_probe_windows[wi].stamp != stamp &&
		!SameBytes(m_probe_windows[wi]))
	{
		m_frame.src_ref_pin++;
		return kNoSourceSlot;
	}

	// Whether an earlier draw of this frame already had a source built for this window -- which
	// makes this draw a hit whatever the cache's own state was at the top of the frame.
	const bool built_this_frame = (wi < m_probe_windows.size() && m_probe_windows[wi].admitted);

	if (wi == m_probe_windows.size())
	{
		// First sight this frame. Was the window here last frame, and does it still hold the same
		// bytes? That is the whole stability question: a title that genuinely re-writes its
		// sampled pages every frame rebuilds every source every frame, and then the expanded cache
		// never admits and rule 3 serves almost nothing. Answered in two tiers, counted in two
		// columns, because the gap between them is the entire result on this corpus.
		ProbedWindow w;
		w.key = key;
		w.stamp = stamp;
		w.build_id = ++m_probe_build_counter;
		const ProbedWindow* prev = nullptr;
		for (const ProbedWindow& p : m_probe_prev)
		{
			if (p.key == key)
			{
				prev = &p;
				break;
			}
		}
		if (!prev)
		{
			// Nothing to compare against yet. Take the fingerprint anyway: a first CPU build takes
			// one too (it is what the NEXT frame's rebuild is checked against), so a probe that
			// skipped it would model a cheaper cache than the one being built.
			w.content = ContentStamp();
		}
		else
		{
			w.prev_seen = true;
			if (prev->stamp == stamp)
			{
				m_frame.src_stamp_kept++;
				w.prev_current = true;
				w.build_id = prev->build_id;
				// The stamp held, so no byte under the window moved and the fingerprint taken
				// under it still stands. Carried rather than recomputed -- that is the whole
				// point of the cheap tier being first.
				w.content = prev->content;
			}
			else
			{
				m_frame.src_stamp_moved++;
				w.content = ContentStamp();
				if (!w.content.Known())
				{
					// GPU-side truth under the window: the CPU bytes here are not the bytes
					// anything would build from, so the move stands as a rebuild -- believed, not
					// proved, and counted apart so a low rescue rate cannot be read as "the bytes
					// changed" when it means "we could not look".
					m_frame.src_stamp_opaque++;
				}
				else if (w.content == prev->content)
				{
					// The pages were rewritten with the bytes they already held. No rebuild, and
					// -- the part that matters downstream -- the SAME identity, so the palette
					// pairs keyed on it survive the frame boundary and the expanded cache admits.
					m_frame.src_stamp_rescued++;
					w.prev_current = true;
					w.build_id = prev->build_id;
				}
			}
		}
		m_probe_windows.push_back(w);
	}
	else if (m_probe_windows[wi].stamp != stamp)
	{
		// Seen this frame and the stamp has moved since -- either a window nothing has named yet
		// (something refused it) or one whose bytes the pin check above just found unchanged.
		// Same two tiers; not counted in the stamp columns, which ask about the frame before, not
		// about mid-frame churn.
		ProbedWindow& pw = m_probe_windows[wi];
		const bool same = SameBytes(pw);
		pw.stamp = stamp; // either way this is the stamp the entry now carries
		if (!same)
		{
			// Nothing is pinned on it, so the entry is rebuilt under the new bytes and takes a new
			// identity with them.
			pw.content = ContentStamp();
			pw.build_id = ++m_probe_build_counter;
			pw.prev_current = false;
		}
	}
	ProbedWindow& w = m_probe_windows[wi];

	if (paletted)
	{
		// The palette is a second, independent identity: a paletted source holds raw indices and
		// the fused RGBA8 is keyed on (index build id x palette content id), so a palette change
		// re-expands without re-deswizzling. The pair is admitted on SECOND SIGHT across a frame
		// boundary -- a fresh pair defers and this draw takes the byte road for one frame, which
		// is what stops a per-frame-volatile window paying an expansion pass for a draw that runs
		// once. High deferrals beside high hits is the healthy shape, not a fault.
		// The palette's identity, taken where the draw's palette was resolved: a hash of the CPU
		// words for an ordinary palette, and the load's (owner x version x CBP x entries) tuple for
		// one the CLUT gather put on the device, where there are no CPU words to hash at all.
		const u64 pal_id = pd.pal_id;
		// ...except for a window a DONOR serves, where the wait cannot ever end and is measuring the
		// wrong alternative anyway: the index image is the owner target's own pixels, which change
		// every frame by definition, so no pair over it survives a frame boundary -- and the other
		// road is not "expand in the shader", it is "write the whole target back into the ring and
		// unswizzle it again, then expand in the shader". Those draws admit on first sight. The probe
		// and the build must agree, so the same answer goes to both.
		//
		// A GATHERED PALETTE joins them, for the same reason on the other axis: its identity carries
		// the owner's version, which moves every time the game re-renders the palette, so the pair
		// can literally never recur (measured -- gt4 631 of 631 loads new, gt4opb 1259 of 1259) and
		// the alternative road is not the cheap one either.
		const GSTileExpandedCache::AdmitWhen when = (donor_token.Known() || pd.pal_record != 0) ?
														GSTileExpandedCache::AdmitWhen::FirstSight :
														GSTileExpandedCache::AdmitWhen::SecondSight;
		if (m_expand_cache.ProbeAdmit(w.build_id, pal_id, when) == GSTileExpandedCache::Admission::Deferred)
		{
			m_frame.src_ref_palette++;
			return kNoSourceSlot;
		}
	}

	// The frame's working set against the array it has to fit in. Capacity is a hard bound rather
	// than a hope: a frame whose distinct windows outrun the array refuses the overflow to the byte
	// road. This is the WINDOW census -- the bind table below counts (image, sampler) pairs, which is
	// the same population unless one window is sampled through two filters -- and it is checked here
	// so a refusal costs nothing but the probe.
	if (!w.admitted)
	{
		if (m_frame.src_distinct >= kMaxSourceSlots)
		{
			m_frame.src_ref_capacity++;
			return kNoSourceSlot;
		}
		w.admitted = true;
		m_frame.src_distinct++;
	}

	// Served. What serving it would COST is the second column: a hit needs no work at all, a
	// rebuild a deswizzle into the entry it already has, a fresh window a first build. Classified
	// off two frames of record, so the hit column is a lower bound -- a real 512-entry cache also
	// hits on a window last drawn several frames ago.
	m_frame.src_eligible++;
	if (pr.hit || built_this_frame || w.prev_current)
		m_frame.src_hit++;
	else if (pr.would_rebuild || w.prev_seen)
		m_frame.src_rebuild++;
	else
		m_frame.src_fresh++;
	return wi;
}

// The sampler the draw's TEX1/CLAMP asks for. Only the eight combinations rule 3 admits can arrive:
// the probe refuses REGION_CLAMP and REGION_REPEAT because a sampler cannot express them, and
// mipmapping is off (triln 0), which is what pins max LOD to level 0 in the device's sampler cache.
u8 GSRendererTileGpu::SourceSamplerKey(u32 wms, u32 wmt, bool ltf)
{
	GSHWDrawConfig::SamplerSelector ss;
	ss.tau = (wms == CLAMP_REPEAT) ? 1 : 0;
	ss.tav = (wmt == CLAMP_REPEAT) ? 1 : 0;
	ss.biln = ltf ? 1 : 0;
	return ss.key;
}

// This frame's slot for one (image, sampler) pair. The pair rather than the image alone because the
// filtering and the wrap ride in the descriptor: the shader has one index and one fetch site, so two
// draws sampling the same window through different TEX1/CLAMP need two descriptors. Rare and cheap;
// a linear scan is the right shape at this size, and the frame's table is a hard bound.
u32 GSRendererTileGpu::SourceSlotFor(GSTexture* tex, u8 sampler)
{
	for (u32 i = 0; i < m_plan_sources.size(); i++)
	{
		if (m_plan_sources[i].texture == tex && m_plan_sources[i].sampler == sampler)
			return i;
	}
	if (m_plan_sources.size() >= kMaxSourceSlots)
	{
		m_frame.src_ref_bind++;
		return kNoSourceSlot;
	}
	m_plan_sources.push_back(GSDevice::GSTileGpuPassPlan::SourceBind{tex, sampler});
	m_frame.src_bind_slots++;
	return static_cast<u32>(m_plan_sources.size() - 1);
}

// Rule 3's build, and the point where the draw changes road. The cache decides whether this window
// already has an image and whether that image is still current, and on a build it hands the texture
// to the builder, which queues the pass that fills it. With an image in hand the draw takes a bind
// slot, its state row names the source, and its fragment stage samples that image instead of
// decoding ring bytes.
//
// TWO build roads reach here and the difference is where the texels come from. Without a donor the
// window's bytes have already been composed into the frame's ring and the source is unswizzled out
// of them. WITH one, the window's pages belong to a live target and the source is reinterpreted
// straight out of that target's texture -- and the caller has NOT composed anything, because not
// composing is the entire point of that road. The cache is told neither: what it sees is a builder
// and a content token, and the token is what keeps the two identities from meeting.
//
// Every refusal leaves the draw exactly where the caller put it -- on the byte road -- so a refusal
// costs at most a wasted compose. Never a dropped draw. ⚠️ On the donor road the caller must compose
// the window itself when this returns false, since it skipped the compose to get here.
bool GSRendererTileGpu::MaterialiseSourceRoad(PendingDraw& pd, u32 window, const GIFRegTEX0& tex0,
	const GSPageBitmap& tex_pages, u32 epoch, const DonorPlan* donor)
{
	const ProbedWindow& w = m_probe_windows[window];

	// The content identity the cache decides rebuilds by. The probe took it BEFORE the window was
	// composed, which is the only moment a page fingerprint is honest -- after the compose those
	// pages carry bytes a writeback has superseded -- and for a donor window it is the owner's
	// identity, which no compose can disturb. Unknown never matches anything, so a window nothing
	// can vouch for rebuilds rather than serving stale texels.
	const GSTileTextureSource::ContentToken token = w.content;

	SourceMaterialiser builder;
	builder.r = this;
	builder.epoch = epoch;
	builder.donor = donor;

	GSTileTextureSource::BuiltOutcome outcome = GSTileTextureSource::BuiltOutcome::Failed;
	u64 build_id = 0;
	GSTexture* tex =
		m_tex_source.LookupBuilt(m_vram_model, tex0, m_env.TEXA, tex_pages, builder, token, &build_id, &outcome);
	switch (outcome)
	{
		case GSTileTextureSource::BuiltOutcome::Hit:
			(donor ? m_frame.src_donor_hits : m_frame.src_cache_hits)++;
			break;
		case GSTileTextureSource::BuiltOutcome::Rescued:
			(donor ? m_frame.src_donor_rescued : m_frame.src_cache_rescued)++;
			break;
		case GSTileTextureSource::BuiltOutcome::Built: break; // counted by the emitter
		case GSTileTextureSource::BuiltOutcome::RefusedPinned: m_frame.src_pin_refusals++; break;
		case GSTileTextureSource::BuiltOutcome::RefusedCapacity: m_frame.src_cap_refusals++; break;
		case GSTileTextureSource::BuiltOutcome::Failed: m_frame.src_build_failed++; break;
	}
	if (!tex)
	{
		if (donor)
			m_frame.src_donor_refused++;
		return false;
	}

	// Stage two, for a paletted window. What the cache just handed back is an INDEX image -- the
	// palette is not in its key, which is the whole reason one build serves every palette the game
	// cycles through it -- so the colour a draw samples comes from fusing that image with THIS draw's
	// palette. Both halves are content-keyed: the palette on its CLUT words, the fusion on (index
	// build id x palette content id), so a palette change re-expands without re-deswizzling and a
	// window re-uploaded with the bytes it already had keeps both.
	const u32 pal_entries = GSLocalMemory::m_psm[tex0.PSM].pal;
	if (pal_entries > 0)
	{
		m_frame.src_pal_draws++;
		u64 pal_id = 0;
		GSTexture* pal = nullptr;
		if (pd.pal_record != 0)
		{
			// A GATHERED palette has no CPU words to build from -- that is the entire point -- so
			// rule 3's palette texture is the device gather itself, an N x 1 image the executor fills
			// off the owner target. A four-bit draw reading one SLOT of a 256-entry load wants a
			// sixteen-texel view of that image, which this road has no copy leg for: it keeps the
			// byte road, whose fetch addresses the entry bias directly.
			GpuPalette* const gp = FindGpuPalette(pd.pal_record);
			if (gp && pd.pal_bias == 0)
				pal = ClutGatherTexture(*gp, pd);
			pal_id = pd.pal_id;
			if (!pal)
				m_frame.clut_r3_refused++;
		}
		else
		{
			// The same words the byte road reads, from the same place: AccumulateDraw already ran
			// Read32 (CSA, CPSM and TEXA applied there) and appended the expansion to the frame's
			// palette stream. Taking them from there rather than re-reading the CLUT is what makes
			// the two roads' colours the same bytes by construction rather than by coincidence.
			const u32* const clut = m_plan_palettes.data() + pd.pal_offset;
			pal = m_palette_cache.Lookup(clut, pal_entries, m_mem.m_clut.GetReadGeneration(), &pal_id);
			// The namespace the gathered ids are kept out of: the expanded cache fuses
			// (index build id x palette id) pairs, and two ids from different worlds compared equal
			// would fuse two unrelated pairs with nothing to catch it.
			pal_id = ClutCpuPalId(pal_id);
		}
		if (!pal || pal_id == 0 || build_id == 0)
		{
			m_frame.src_pal_missing++;
			if (donor)
				m_frame.src_donor_refused++;
			return false;
		}

		SourceExpander expander;
		expander.r = this;
		GSTileExpandedCache::BuiltOutcome eo = GSTileExpandedCache::BuiltOutcome::Failed;
		// Keyed on the TOKEN, not on whether a donor was handed in: the probe asked the same question
		// off the same token, and the two must give the same answer or a pair the probe admitted
		// defers at the build and the draw is refused for nothing. They differ exactly when a donor
		// was offered and the build then fell back to the ring.
		GSTexture* const expanded = m_expand_cache.LookupBuilt(tex, build_id, pal, pal_id, expander,
			(w.content.source == GSTileTextureSource::ContentToken::Source::Target || pd.pal_record != 0) ?
				GSTileExpandedCache::AdmitWhen::FirstSight :
				GSTileExpandedCache::AdmitWhen::SecondSight,
			&eo);
		switch (eo)
		{
			case GSTileExpandedCache::BuiltOutcome::Hit: m_frame.src_expand_hits++; break;
			case GSTileExpandedCache::BuiltOutcome::Built: break; // counted by EmitExpandOp
			case GSTileExpandedCache::BuiltOutcome::Deferred: m_frame.src_expand_defer++; break;
			case GSTileExpandedCache::BuiltOutcome::RefusedCapacity: m_frame.src_expand_cap++; break;
			case GSTileExpandedCache::BuiltOutcome::Failed: m_frame.src_expand_failed++; break;
		}
		if (!expanded)
		{
			if (donor)
				m_frame.src_donor_refused++;
			return false;
		}
		// From here the draw samples an ordinary RGBA8 image and the road knows nothing about palettes.
		tex = expanded;
	}

	const u32 slot = SourceSlotFor(tex, SourceSamplerKey(pd.wms, pd.wmt, pd.ltf));
	if (slot == kNoSourceSlot)
	{
		if (donor)
			m_frame.src_donor_refused++;
		return false;
	}

	// On the road. The byte road's bit comes off: this draw's fragment stage reads no ring word, so a
	// pass made only of draws like it compiles no byte road at all -- which is where the instruction
	// budget the variants bought is actually spent.
	pd.src_slot = slot;
	pd.road_mask = GSDevice::kGSTileGpuRoadSource;
	m_frame.src_served++;
	if (donor)
		m_frame.src_donor_served++;
	return true;
}

bool GSRendererTileGpu::SourceMaterialiser::BuildTileSource(GSTexture* tex, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA)
{
	// The donor road reads the owner target's texture and no ring word at all, so it goes through
	// neither the epoch nor the page table -- which is also why its op survives a frame that composed
	// nothing (the executor's byte-road gate is a union for exactly that reason).
	if (donor)
		return r->EmitDonorOp(tex, TEX0, *donor);
	return r->EmitMaterialiseOp(tex, TEX0, TEXA, epoch);
}

bool GSRendererTileGpu::SourceExpander::BuildTileExpansion(GSTexture* index, GSTexture* palette, GSTexture* dst)
{
	return r->EmitExpandOp(dst, index, palette);
}

// A prep op names its textures by index into the plan's prep-texture list, so every texture one
// touches has to have a slot in it. Deduped, unlike a materialise destination (which is fresh by
// construction): one palette serves every window drawn through it this frame, and one index image
// serves every palette cycled over it.
u32 GSRendererTileGpu::PrepTextureIndex(GSTexture* tex)
{
	for (u32 i = 0; i < m_plan_prep_textures.size(); i++)
	{
		if (m_plan_prep_textures[i] == tex)
			return i;
	}
	m_plan_prep_textures.push_back(tex);
	return static_cast<u32>(m_plan_prep_textures.size() - 1);
}

// The paletted road's second pass. Queued from inside the expanded cache's build, which
// MaterialiseSourceRoad runs straight after the index build -- so if that build emitted a
// materialise, this op sits behind it in the array, and array order IS execution order at the pass
// head. That is the whole ordering argument: nothing here has to look at what came before.
bool GSRendererTileGpu::EmitExpandOp(GSTexture* dst, GSTexture* index, GSTexture* palette)
{
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::Expand;
	op.target = PrepTextureIndex(dst);
	op.index_texture = PrepTextureIndex(index);
	op.palette_texture = PrepTextureIndex(palette);
	m_plan_prep_ops.push_back(op);
	m_frame.src_expand_builds++;
	return true;
}

bool GSRendererTileGpu::EmitMaterialiseOp(GSTexture* tex, const GIFRegTEX0& tex0, const GIFRegTEXA& texa, u32 epoch)
{
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::Materialise;
	// A source is not a target: the op names it through the plan's prep_textures list, which is
	// this vector, and the target list stays exactly what it was.
	op.target = PrepTextureIndex(tex);
	op.bp = tex0.TBP0;
	// Pages per texture row, through the one spelling every road shares, so the materialise addresses
	// the window exactly the way the byte road would have.
	op.bw = TextureWindowBwPg(tex0.PSM, tex0.TBW);
	op.psm = tex0.PSM;
	op.epoch = epoch;
	// A 24-bit window's alpha byte belongs to whatever else shares those bytes, so TEXA's expansion
	// is baked into the image here, at build time -- which is what the CPU deswizzlers do and what
	// the cache's key already accounts for.
	op.texa = (tex0.PSM == PSMCT24) ? (1u | (texa.AEM ? 2u : 0u) | (static_cast<u32>(texa.TA0) << 8)) : 0u;
	m_plan_prep_ops.push_back(op);
	m_frame.src_builds++;
	return true;
}

// The donor build's op. It names TWO lists -- its destination in prep_textures (a source is nothing's
// render target) and its owner in the frame's target list -- which is why the op carries both indices
// and why PlanTargetIndex is called here: an owner no draw of this frame rendered into would
// otherwise not be in the target list at all, and the executor resolves the op through it.
//
// No epoch, no page entries, no TEXA. The reinterpretation reads the owner's texels and writes
// indices; a paletted window's TEXA rides in the CLUT expansion, exactly as it does for a
// materialised index image.
bool GSRendererTileGpu::EmitDonorOp(GSTexture* tex, const GIFRegTEX0& tex0, const DonorPlan& donor)
{
	GSDevice::GSTileGpuPrepOp op = {};
	op.kind = GSDevice::GSTileGpuPrepKind::Donor;
	op.target = PrepTextureIndex(tex);
	op.donor_target = PlanTargetIndex(donor.owner);
	op.bp = donor.src_bp;
	op.bw = donor.src_bwpg;
	op.psm = donor.psm;
	op.owner_bp = donor.owner_bp;
	op.owner_bwpg = donor.owner_bwpg;
	m_plan_prep_ops.push_back(op);
	m_frame.src_donor_builds++;
	return true;
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

// EmitPrepOp's writeback rule, stated as a pure function so the prefill decision below and the
// op it predicts cannot drift apart. See the declaration for what it means.
u32 gsTileComposableBlocks(const GSTileRingPlaneState (&planes)[kGSTilePlaneCount])
{
	// Who writes back: an owner holding unsynced truth on some plane, with a road to carry it.
	GSTileSurfaceId writers[kGSTilePlaneCount];
	u32 writer_count = 0;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		const GSTileRingPlaneState& p = planes[pi];
		if (p.owner == kGSTileNoSurface || p.truth_mask == 0 || p.synced || !p.byte_road)
			continue;
		u32 w = 0;
		while (w < writer_count && writers[w] != p.owner)
			w++;
		if (w == writer_count)
			writers[writer_count++] = p.owner;
	}

	// What each of them writes: the union over every plane it owns here, synced or not -- that is
	// the mask EmitPrepOp puts on the page entry, and the shader honours no other filter.
	u32 composed = 0;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		for (u32 w = 0; w < writer_count; w++)
		{
			if (writers[w] == planes[pi].owner)
			{
				composed |= planes[pi].truth_mask;
				break;
			}
		}
	}
	return composed;
}

// A ring slot for `page` in the current epoch. Prefill from S unless the page is, in every
// plane, whole-page unsynced truth of a surface with a byte road -- exactly the case where a
// writeback this epoch composes every byte (ComposeRingPages emits it right after asking here).
// A page synced already (either its bytes are in S after a stall readback, or an earlier
// writeback this epoch composed its live slot) prefills too: over the same live slot the memcpy
// runs first and the writeback's blocks land over it, so the extra copy is harmless.
//
// ⚠️ The per-plane test above is a PROXY for the question that actually matters -- "will the
// writebacks this compose emits cover all 32 blocks?" -- and a proxy is only as good as the rule
// it stands in for. Nothing in the two functions makes them move together: the decision is
// per-plane and whole-page, the writeback's mask is per-block and per-OWNER, and a block that is
// neither prefilled nor inside some writeback's mask keeps whatever the executor left in the
// slot, which is zero. That is a wrong colour, not a stale one, and the model counts the page
// composed either way. So the coverage is computed too and both must agree: a slot goes
// unprefilled only where the planned writebacks provably reach every block. Strictly more
// prefill than the proxy alone, never less; the worst case it leaves is a byte one write old
// instead of a byte that was never guest data at all.
u32 GSRendererTileGpu::EnsureRingSlot(u32 page)
{
	GSTileRingPlaneState planes[kGSTilePlaneCount];
	bool needs_prefill = false;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
	{
		const GSTileSurfaceId owner = m_vram_model.OwnerOf(page, pi);
		planes[pi].owner = owner;
		planes[pi].truth_mask = m_vram_model.TruthMask(page, pi);
		planes[pi].synced = m_vram_model.SyncedPages(pi).test(page);
		planes[pi].byte_road = owner != kGSTileNoSurface && HasByteRoad(m_vram_model.Get(owner).layout);
		if (owner == kGSTileNoSurface || planes[pi].truth_mask != GSVramModel::kFullBlockMask ||
			planes[pi].synced || !planes[pi].byte_road)
			needs_prefill = true;
	}
	if (!needs_prefill && gsTileComposableBlocks(planes) != GSVramModel::kFullBlockMask)
	{
		needs_prefill = true;
		m_frame.prefill_uncomposed++;
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
	// A seed overwrites the surface's texels, so a palette gathered out of those pages has to be
	// captured into the frame's stream BEFORE the op that destroys it is queued: array order is
	// execution order at the pass head, and a copy queued after the seed would read what the seed
	// wrote. (A writeback only reads, so it asks nothing.)
	if (kind == GSDevice::GSTileGpuPrepKind::Seed)
		NoteClutSourceWritten(id, pages);
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
		// A seed writes the surface's texture, so anything cached under its identity is stale. (The
		// palettes gathered out of those pages were captured at the top of this function, before the
		// op was queued.)
		BumpSurfaceVersion(id);
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
					// Beyond the table: this owner's writeback is not emitted at all, and
					// EnsureRingSlot could not know that when it decided -- it looked at the
					// planes, not at the table. Force the prefill now, while the slot is still
					// this frame's (the ring is built at plan time, so a later flag still lands).
					// Counted as lost rather than mis-composed, which is only true once the slot
					// carries S's bytes rather than the executor's zeros.
					if (m_ring_live[page] != 0)
						m_ring_entries[m_ring_live[page] - 1].prefill_s = true;
					lossy++;
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
		// Writebacks only, and stated rather than assumed: this reads `op.target` as an index into
		// the plan's TARGET list, and a materialise names its destination in prep_textures instead.
		// The ranges this is asked about today hold nothing else (ComposeRingPages emits writebacks
		// and the materialise is queued after it returns), so this is the contract in code rather
		// than a fix -- but the two index spaces are the kind of thing a later road walks into.
		if (op.kind != GSDevice::GSTileGpuPrepKind::Writeback)
			continue;
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

	// What this draw actually writes. An alpha test every fragment fails still lands whatever
	// AFAIL keeps alive -- Classic's zm/fm derivation, and the idiom SotC uses for its
	// full-screen fades and post sprites: ATE=1 ATST=NEVER AFAIL=FB_ONLY is "colour only, leave
	// depth alone" whatever ZTE/ZMSK say. Missing it deposits those sprites' Z=max into the
	// persistent depth buffer and the whole 3D pass then fails GEQUAL.
	//
	// The same intent also arrives as a COMPARISON that cannot come out either way, and it has to
	// be recognised there too. Katamari Damacy stamps a lower Z into the King's head's rectangle
	// with an untextured sprite carrying vertex alpha 0x00 under ATST=NOTEQUAL AREF=0 -- "0 != 0"
	// is false on every fragment -- plus AFAIL=ZB_ONLY, "a failing fragment still writes depth".
	// Read as a live per-fragment test the fragment stage discards the whole sprite, the stamp
	// never lands, and the head draw behind it (ZTST=GEQUAL at the stamped Z) then fails over
	// every pixel the star covers. So the fold asks the alpha, not just the register.
	const bool alpha_is_constant = gsTileAlphaIsDrawConstant(PRIM->TME, ctx->TEX0.TCC, PRIM->AA1,
		m_vt.m_eq.a == 0xF);
	const u32 const_alpha = static_cast<u32>(m_vt.m_min.c.I32[3]) & 0xFFu;
	const GSTileAlphaTestFold atst_fold = gsTileFoldAlphaTest(ctx->TEST.ATE, ctx->TEST.ATST,
		ctx->TEST.AREF, alpha_is_constant, const_alpha);
	const bool atst_all_fail = (atst_fold == GSTileAlphaTestFold::AllFail);
	if (ctx->TEST.ATE && ctx->TEST.ATST != ATST_NEVER && ctx->TEST.ATST != ATST_ALWAYS &&
		atst_fold != GSTileAlphaTestFold::Varies)
	{
		// The population the constant fold moves off the fragment stage, per frame. NEVER and
		// ALWAYS are excluded: they never reached it in the first place.
		if (atst_all_fail)
			m_frame.atst_fold_fail++;
		else
			m_frame.atst_fold_pass++;
	}
	const u32 afail = ctx->TEST.GetAFAIL(ctx->FRAME.PSM);
	const bool z_write = gsTileDepthWriteSurvives(ctx->TEST.ZTE, ctx->ZBUF.ZMSK, atst_all_fail, afail);
	const bool z_test = ctx->TEST.ZTE && ctx->TEST.ZTST > ZTST_ALWAYS;
	const bool z_used = z_write || z_test;
	// FBMSK per channel, not all-or-nothing. Games use the frame buffer as scratch for one
	// channel at a time -- OutRun 2006 lays three full-screen sprites a frame over the finished
	// world under FBMSK=0x00FFFFFF with a black fragment colour, meaning to rewrite the frame's
	// alpha byte alone, and Beyond Good & Evil draws silhouettes into the buffer that is ON
	// SCREEN under the same mask. Writing the RGB of those draws paints black over the world in
	// the first case and the silhouettes over the picture in the second, and bge's alpha-only
	// full-screen blit at the end of each frame then carries the damage into the other buffer --
	// a ratchet to black over three frames. gsTileFrameColorWriteMask folds the AFAIL modes in
	// on top and holds the deferred-alpha carve-out; a partly masked channel lands whole (the
	// approximation, ledgered).
	const u32 fb_fmsk = GSLocalMemory::m_psm[ctx->FRAME.PSM].fmsk;
	const u8 color_mask = gsTileFrameColorWriteMask(ctx->FRAME.FBMSK, fb_fmsk, atst_all_fail, afail);
	const bool color_written = color_mask != 0;
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
	pd.src_slot = kNoSourceSlot;
	pd.first_prep_op = static_cast<u32>(m_plan_prep_ops.size());

	// Fog. The GS walks the fragment's RGB toward FOGCOL by the per-vertex fog factor F (RGB only,
	// alpha untouched); the vertex stream already carries F, so the draw needs only the enable and
	// the colour. Sprites take F from the second vertex like everything else flat about them.
	pd.fge = PRIM->FGE;
	pd.fogcol = static_cast<u32>(m_env.FOGCOL.FCR) | (static_cast<u32>(m_env.FOGCOL.FCG) << 8) |
				(static_cast<u32>(m_env.FOGCOL.FCB) << 16);

	// The alpha test. A test the fold decided never reaches the fragment stage: every fragment
	// takes the same road, so the write flags above already carry it (an all-fail draw's AFAIL
	// decides what it lands; an all-pass one writes everything). What is left is a genuine
	// per-fragment split, and it is worth emitting only where the two sides actually land
	// differently -- AFAIL=FB_ONLY keeps the whole colour and drops only the depth write, so a
	// draw that writes no depth is unaffected by its own test, which is most of a game's blended
	// geometry.
	//
	// ⚠️ Wrong-fast: the fragment stage discards a failing fragment outright. That is exact for
	// AFAIL=KEEP; for the three modes that still write something on failure it drops that write.
	// The exact form needs the draw split in two -- one draw per side of the test, each with its
	// own write masks and depth mode -- which costs an indirect run split per draw, so it waits
	// until the run structure is measured rather than guessed. Rowed in the deferred-accuracy
	// ledger. The fold above takes the decidable draws out of that population entirely: where the
	// alpha cannot vary there is no split to make, and the answer is exact.
	const bool ate_real = (atst_fold == GSTileAlphaTestFold::Varies);
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
	pd.color_mask = color_mask;

	// Texture inputs. Two address geometries are sampled at this stage: the CT32 one (PSMCT32 and
	// PSMCT24 as direct colour, and the alpha-byte views PSMT8H/PSMT4HL/PSMT4HH as indices in the
	// top bits of the same words) and the PSMT8/PSMT4 one (128-texel pages, a swizzled index into
	// an expanded CLUT). Every other textured draw -- the 16-bit families -- keeps the
	// vertex-colour path until its format is added. The fixed TEX0 folds TW/TH to the used ST
	// range, exactly as ObserveDraw derives its footprint, so the dimensions the shader scales by
	// match the draw.
	pd.tex_enable = false;
	pd.index_format = 0;
	pd.pal_offset = 0;
	GSPageBitmap tex_pages;
	// Did this draw's read window get composed into the ring? Rule 2 and the donor road both leave it
	// uncomposed -- they read an image, not bytes -- and the open pass's read set has to say so, or a
	// later writeback would test itself against a slot nothing read.
	bool composed_tex = false;
	if (PRIM->TME)
	{
		const bool mip = IsMipMapActive();
		const GIFRegTEX0 tex0 = ctx->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), mip);
		const u32 psm = tex0.PSM;
		const bool direct32 = (psm == PSMCT32 || psm == PSMCT24);
		// Every format whose texel is a palette index: PSMT8/PSMT4 and the three alpha-byte views.
		// IndexFormatFor is the one list -- the donor road's reinterpretation already keyed off it,
		// and a second list here is how the two would come to disagree about what is sampleable.
		const int idx_fmt = GSTileSwizzleForms::IndexFormatFor(psm);
		const bool paletted = (idx_fmt >= 0);
		if (direct32 || paletted)
		{
			m_frame.tex_draws++;
			if (paletted && GSLocalMemory::m_psm[psm].pgs.x == GSLocalMemory::m_psm[PSMCT32].pgs.x)
				m_frame.tex_hi_draws++;
			pd.tex_enable = true;
			pd.fst = PRIM->FST;
			pd.tbp0 = tex0.TBP0;
			// Pages per texture row, GSOffset's bw >> (pageShiftX - 6): a 64-texel-wide page (the
			// CT32 family, which the alpha-byte views share) takes TBW, a 128-wide one (PSMT8/PSMT4)
			// halves it. ⚠️ Decided by page WIDTH, never by "is it paletted" -- PSMT8H is both
			// paletted and 64 wide, and halving its TBW would address the wrong page in every row
			// but the first.
			pd.tbw = TextureWindowBwPg(psm, tex0.TBW);
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
			// The state row's index_format: 0 for a direct 32-bit texel, else the shader's format
			// number plus one (1 = PSMT8, 2 = PSMT4, 3 = PSMT8H, 4 = PSMT4HL, 5 = PSMT4HH). The two
			// numberings are pinned together in the gs suite -- nothing links this file to the GLSL,
			// and a renumbering that moved one and not the other would sample PSMT4HL's nibble out of
			// a PSMT8H texture.
			pd.index_format = direct32 ? 0u : static_cast<u32>(idx_fmt) + 1u;
			// A 24-bit texture's texels carry no alpha byte: TEXA supplies it (and AEM makes an
			// all-zero RGB texel transparent). A paletted texture takes TEXA in the CLUT expansion.
			pd.texa = (psm == PSMCT24) ? (1u | (m_env.TEXA.AEM ? 2u : 0u) |
											 (static_cast<u32>(m_env.TEXA.TA0) << 8)) :
										 0u;
			if (paletted)
			{
				// Where this draw's palette words are. The mirror answers for the CLUT slots the draw
				// reads: the CPU's RAM as always, or ONE device palette a gathered load put there --
				// and then no CPU words cross into the frame's stream at all, and the road decision
				// below is what says how the device serves them instead.
				//
				// ⚠️ A resolution the mirror cannot serve whole SYNCS, which reads pages back and so
				// flushes the pending plan -- and that clears the prep-op array this draw's range is
				// anchored in. Nothing has been emitted for the draw yet (the compose is below), so
				// re-anchoring is the whole repair, but it has to happen.
				const u32 pal_entries = GSLocalMemory::m_psm[psm].pal;
				bool clut_synced = false;
				u32 first_texel = 0;
				GpuPalette* const gp = ResolveDrawPalette(tex0, pal_entries, first_texel, clut_synced);
				if (clut_synced) [[unlikely]]
					pd.first_prep_op = static_cast<u32>(m_plan_prep_ops.size());
				if (gp)
				{
					pd.pal_record = gp->handle;
					pd.pal_bias = first_texel;
					pd.pal_id = gp->pal_id;
				}
				else
				{
					// Expand this draw's CLUT to 32-bit RGBA (CSA/CPSM/TEXA applied by Read32) and
					// append it to the frame's palette stream; pal_offset is the word index of entry 0.
					// No dedup yet (wrong-fast): a palette is 16 or 256 words, a full SotC frame well
					// under the ring.
					m_mem.m_clut.Read32(tex0, m_env.TEXA);
					const u32* clut = m_mem.m_clut;
					pd.pal_offset = static_cast<u32>(m_plan_palettes.size());
					m_plan_palettes.insert(m_plan_palettes.end(), clut, clut + pal_entries);
					pd.pal_id = ClutCpuPalId(GSTilePaletteCache::ContentId(clut, pal_entries));
				}
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
				// The road is decided here, so record it here: the pass's shader variant is the OR of
				// its draws' roads, and a road no draw takes is never compiled.
				pd.road_mask = GSDevice::kGSTileGpuRoadTarget;
			}
			else
			{
				pd.road_mask = GSDevice::kGSTileGpuRoadByte;

				// Rule 3, and the DONOR question comes first because it changes what the rest of this
				// block does. A window whose pages one live target owns whole can be reinterpreted
				// straight out of that target's texture, and then nothing has to be composed for it
				// at all -- no writeback of a megabyte of pixels into ring slots only this read would
				// have consumed, and no ring slots. It also supplies the window's content identity,
				// which a page fingerprint cannot: those bytes are not in the CPU shadow.
				DonorPlan donor;
				const bool donor_ok =
					m_bindless_targets && DonorForTextureRead(tex0, tex_pages, fb_id, z_id, donor);
				const GSTileTextureSource::ContentToken donor_token =
					donor_ok ? GSTileTextureSource::TargetToken(donor.owner, SurfaceVersion(donor.owner)) :
							   GSTileTextureSource::ContentToken{};
				// Counted here rather than after the admission question, deliberately: this is the
				// population ONE live target could serve whole, and every later refusal (region wrap,
				// an unadmitted palette pair, the pin) is a separate fact about the same draw. Folding
				// them together would report the road as unavailable when it is merely not reached.
				if (donor_ok)
					m_frame.src_donor_eligible++;

				// The admission question, asked BEFORE the compose and answered against the bytes as
				// they stand -- which is the only moment a page fingerprint is honest, since composing
				// supersedes exactly the bytes it would hash.
				const u32 src_window = m_bindless_targets ?
										   ProbeSourceRoad(pd, tex0, tex_pages, paletted, mip, donor_token) :
										   kNoSourceSlot;

				// The donor build, tried before the compose because skipping the compose IS the win.
				// It reads the owner's image rather than the ring, so unlike a materialise it is not
				// free to hoist to the pass head: a draw already in the open pass may be what wrote
				// the pixels it is about to read, and then this draw opens its own pass -- the
				// writeback's own test and the same precedent.
				const u32 ops_before = static_cast<u32>(m_plan_prep_ops.size());
				const bool served_by_donor = donor_ok && src_window != kNoSourceSlot &&
											 MaterialiseSourceRoad(pd, src_window, tex0, tex_pages, m_epoch, &donor);
				if (served_by_donor)
				{
					pd.epoch = m_epoch;
					if (m_plan_prep_ops.size() != ops_before && DonorHoistCollides(donor.owner, tex_pages))
					{
						pd.break_before = true;
						m_frame.src_donor_breaks++;
						BreakOpenPass();
					}
				}
				else
				{
					// The byte road, and rule 3 off the ring on top of it. The materialise reads the
					// ring at this draw's epoch, so the window's slots have to exist and any writeback
					// composing them has to be queued first: prep ops run at the pass head in array
					// order, and the compose below emits its writebacks before the build. A donor that
					// was offered and then refused downstream lands here too, which is why the compose
					// is on this side of the branch and not above it.
					ComposeForPendingDraw(tex_pages, pd);
					composed_tex = true;
					pd.epoch = m_epoch;
					// A paletted window goes the same way in two stages -- index image, then the
					// palette expansion on top of it -- and both ops are queued from in there, in that
					// order.
					if (src_window != kNoSourceSlot)
						MaterialiseSourceRoad(pd, src_window, tex0, tex_pages, pd.epoch);
				}
			}

			// The gathered palette's words, once the road is known. A rule-3 draw samples the N x 1
			// gather through the expansion and needs nothing here; a BYTE-road one reads its palette out
			// of the frame's stream, and gets it from one image-to-buffer copy of the palette's blocks --
			// not from a gather pass, because at six hundred to twelve hundred CLUT loads a frame a pass
			// each would double the pass count this whole design rests on. A copy lands texels row-major,
			// so the state row carries the mode the fragment stage applies the CSM1 entry order in.
			if (pd.pal_record != 0) [[unlikely]]
			{
				if (pd.road_mask == GSDevice::kGSTileGpuRoadSource)
				{
					m_frame.clut_draws_r3++;
				}
				else
				{
					GpuPalette* const pal_rec = FindGpuPalette(pd.pal_record);
					if (pal_rec && EnsureClutStreamWords(*pal_rec, pd))
					{
						pd.pal_offset = pal_rec->stream_offset;
						pd.pal_mode = (pal_rec->entries == 256) ? 1u : 2u;
						m_frame.clut_draws_byte++;
					}
					else
					{
						// The device cannot serve the words after all -- the source died between the resolve
						// and here, or the forms stopped fitting. Retire the record and take the CPU road,
						// which is where every un-gathered draw already is.
						if (pal_rec)
							RetireLostClutRecord(*pal_rec);
						pd.pal_record = 0;
						pd.pal_bias = 0;
						pd.pal_mode = 0;
						m_mem.m_clut.Read32(tex0, m_env.TEXA);
						const u32* const clut = m_mem.m_clut;
						pd.pal_offset = static_cast<u32>(m_plan_palettes.size());
						m_plan_palettes.insert(m_plan_palettes.end(), clut, clut + GSLocalMemory::m_psm[psm].pal);
					}
				}
			}
		}
		else
		{
			// A format neither road samples -- the 16-bit families, which is all that is left: the
			// draw renders with vertex colour alone today, and rule 3 refuses it on format.
			m_frame.tex_unsupported++;
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
	//
	// ⚠️ "Every channel lands" is gsTileFrameWriteIsTotal, NOT color_mask == RGBA. A channel the
	// mask reports as written can still be preserving bits inside its byte, and a masked draw
	// leaves the rest of the cell holding whatever the texture had -- which is exactly what the
	// seed is for. Only a draw that masks nothing at all may skip it.
	//
	// "No test can reject one" is gsTileColorLandsOnEveryFragment, which reads the SAME fold the
	// write flags did: no alpha test, one that provably passes, or one that provably fails in an
	// AFAIL mode that still writes the frame buffer. Both halves of the fold only strengthen this
	// -- an all-pass draw is as total as ATST=ALWAYS, and an all-fail one is total exactly where
	// ATST=NEVER already was.
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
			gsTileFrameWriteIsTotal(ctx->FRAME.FBMSK, fb_fmsk) &&
			gsTileColorLandsOnEveryFragment(atst_fold, afail))
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
	{
		m_vram_model.OnNativeDraw(fb_id, fb_pages, fb_claims);
		// This draw's pixels land in the surface's texture, so a donor source cached off it is
		// stale from here. Deliberately whole-surface rather than per-page: the identity names the
		// image, and an over-eager bump costs a rebuild where an under-eager one costs a wrong texel.
		BumpSurfaceVersion(fb_id);
		// A palette gathered off these very pages is a different matter and is asked PER PAGE: its
		// words are not a cache that can be rebuilt, they are the only copy, and there is no cheaper
		// road to fall back to -- so the question has to be the exact one. (Only a colour surface can
		// own a palette, which is why the depth claim below asks nothing.)
		NoteClutSourceWritten(fb_id, fb_pages);
	}
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
		BumpSurfaceVersion(z_id);
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
	if (pd.tex_enable && composed_tex)
	{
		// ComposeRingPages gave every page of the read window a live slot, and nothing between
		// there and here closes one (only an upload does, and uploads do not land mid-draw), so
		// m_ring_live still names the slot this draw's shader will read. A rule-2 draw and a
		// donor-served one read no slot at all, so they constrain no later writeback.
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
	// The colour write mask rides the blend key, so the executor's per-draw pipeline pick carries
	// it and a draw whose mask differs only splits the indirect run -- never breaks the pass. A
	// depth-only draw (ATST NEVER + AFAIL ZB_ONLY, or an FBMSK that keeps every stored bit) masks
	// all four channels and still tests and writes depth in its pass.
	blend_key |= GSDevice::GSTileGpuPassPlan::PackNoWrite(pd.color_mask);
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
	{
		// No draws to issue, so nothing can be naming a device palette: the prune's precondition
		// holds here too, and a stretch of nothing but CLUT loads must not accumulate records.
		PruneGpuPalettes();
		return;
	}

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
		// The same two slots, lifted out of the state row for the executor: it splits its indirect
		// runs on the pair, and the state table's layout is not part of that contract
		// (GSTileGpuPassPlan::bind_keys).
		m_plan_bind_keys.resize(m_plan_pending.size());
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
			sr.tex_source = pd.src_slot;
			sr.pal_mode = pd.pal_mode;
			sr.pal_bias = pd.pal_bias;
			sr.pad0_ = 0;
			sr.pad1_ = 0;
			m_plan_bind_keys[i] = GSDevice::GSTileGpuPassPlan::PackBindKey(pd.tex_slot, pd.src_slot);
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
			pass.road_mask = 0;
			for (u32 d = i; d < j; d++)
			{
				const PendingDraw& pd = m_plan_pending[d];
				// The pass's shader variant: the union of the roads its draws take. A pass of nothing
				// but untextured draws lands at zero and gets the smallest program there is.
				pass.road_mask |= pd.road_mask;
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
			// What the rule-3 half of the sampled-binding split COSTS, in calls -- the same
			// arithmetic that priced it before it was taken, now measuring the real thing. The
			// executor cuts a call at a pipeline change (topology or blend key) and at a bind-key
			// change, because a descriptor array index has to be dynamically uniform across the
			// call. Folding the source into that key cuts one more call wherever the source changes
			// and nothing else does -- so only those boundaries are counted, and only inside a
			// pass, which is the one place pass membership is known. Priced at the measured ~237 ns
			// per indirect call, this is what says whether the slot-splitting shape stays
			// affordable.
			for (u32 d = i + 1; d < j; d++)
			{
				if (m_plan_topologies[d] != m_plan_topologies[d - 1] ||
					m_plan_blend_keys[d] != m_plan_blend_keys[d - 1])
					continue; // a new pipeline run, so already a new call
				if (m_plan_pending[d].tex_slot != m_plan_pending[d - 1].tex_slot)
					continue; // already a new call, for the rule-2 slot
				if (m_plan_pending[d].src_slot != m_plan_pending[d - 1].src_slot)
					m_frame.src_extra_calls++;
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
		plan.bind_keys = m_plan_bind_keys;
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
		plan.prep_textures = m_plan_prep_textures;
		plan.sources = m_plan_sources;
		plan.tex_sources = m_plan_tex_sources;
		plan.ring_pages = ring;
		plan.epoch_count = m_epoch + 1;
		plan.palettes = m_plan_palettes;

		g_gs_device->ExecuteTileGpuPassPlan(plan);
	}
	else if (!m_warned_no_executor)
	{
		// The device failed the TileGpu contract at construction, but the renderer was selected
		// and built anyway (there is no fallback yet -- a separate pending decision). From here
		// every frame's plan is silently discarded: without this, that is a black frame and a
		// clean exit code, indistinguishable at every level above this one from working GS
		// output. Say it once, loudly, and name the log line that has the actual missing feature
		// (GSDeviceVK.cpp, device creation).
		m_warned_no_executor = true;
		Console.Error("TileGpu: device contract absent -- every draw is being DISCARDED, output "
					  "will be black. See the \"VK: TileGpu device contract absent (...)\" line "
					  "logged at device creation for which feature is missing.");
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
	m_plan_bind_keys.clear();
	m_plan_pending.clear();
	m_plan_passes.clear();
	m_plan_target_pairs.clear();
	m_plan_snapshots.clear();
	m_plan_prep_ops.clear();
	m_plan_page_entries.clear();
	m_plan_targets.clear();
	m_plan_prep_textures.clear();
	m_plan_sources.clear();
	m_plan_tex_sources.clear();
	m_plan_target_surfaces.clear();
	m_plan_target_of_surface.clear();
	m_ring_entries.clear();
	m_ring_versions.clear();
	m_ring_live.fill(0);
	m_epoch = 0;
	// The plan has been handed to the executor, so every cache entry its draws named is free
	// again: advance the pin clock, which is what releases them. This is the only place that may
	// do it -- a mid-frame flush ends a plan just as VSync does, and the pins have to clear with
	// the plan and not with the video frame.
	AdvanceSourcePinFrame();
	m_run_surface = kGSTileNoSurface;
	m_run_written = GSVector4i::zero();
	BreakOpenPass();
	m_vram_model.ClearAllSynced();
	// The one place a device palette may be recycled. Here, and only here, both conditions Tile's
	// own predicate silently assumes are true by construction: there are no pending draws and every
	// recorded op has issued, so a palette the mirror no longer names is a palette nothing can still
	// be about to sample.
	PruneGpuPalettes();
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
//
// The CLUT is the one piece of truth this does NOT reach on its own. A palette the gather served
// lives in a target's texture and in the sixteen-slot mirror, and the CPU's CLUT RAM holds whatever
// the loader read out of a stale shadow for those slots -- so pulling every page down leaves the
// palette itself wrong. SyncClutToCpu is what makes those words whole (exact-Tile's own contract),
// and it runs first, because it reads pages back through the same road.
void GSRendererTileGpu::SyncAllTruthToCpu()
{
	SyncClutToCpu();
	m_vram_model.ClearAllSynced();
	ReadbackToShadow(m_vram_model.TruthAny(), StallSite::SyncAll);
}
