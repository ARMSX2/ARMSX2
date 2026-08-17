// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSRendererTile.h"

#include "GS/GSLocalMemory.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/HW/GSDrawLog.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/Tile/GSTileOracle.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/Tile/GSTileTexWalk.h"

#include "common/Console.h"
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
		case GSTileFloorReason::ColClip:
			return GSDrawLog::TileFallbackColClip;
		case GSTileFloorReason::BlendOverlap:
			return GSDrawLog::TileFallbackBlendOverlap;
		case GSTileFloorReason::BlendTexSample:
			return GSDrawLog::TileFallbackBlendTexSample;
		case GSTileFloorReason::DevLimit:
			return GSDrawLog::TileFallbackFloor;
		default:
			return GSDrawLog::TileFallbackFloor;
	}
}
} // namespace

GSRendererTile::GSRendererTile(int threads)
	: GSRendererSW(threads)
{
}

GSRendererTile::~GSRendererTile()
{
	if (m_sw_verts)
		_aligned_free(m_sw_verts);
}

GSVertexSW* GSRendererTile::SwVerts(u32 count)
{
	if (count > m_sw_verts_cap)
	{
		if (m_sw_verts)
			_aligned_free(m_sw_verts);
		m_sw_verts_cap = std::max<u32>(count, 1024);
		m_sw_verts = static_cast<GSVertexSW*>(_aligned_malloc(sizeof(GSVertexSW) * m_sw_verts_cap, VECTOR_ALIGNMENT));
		if (!m_sw_verts)
			m_sw_verts_cap = 0;
	}
	return m_sw_verts;
}

void GSRendererTile::Destroy()
{
	ReportReadbackCensus();
	ReportMemoCensus();
	if (m_oracle.native_draws != 0)
	{
		// States coverage and nothing more. "Complete" does NOT mean the run's frames are
		// the software renderer's: measured, an oracle run lands within 4 px (SotC), 13 px
		// (Dirge) and 30 px (GT4 OPB) of a software run, and the residues are known
		// findings on pages outside any compared footprint. Claiming the stronger thing
		// here is how the next reader inherits a control that was never proven.
		Console.WriteLn("Tile oracle coverage: %u of %u native draws compared%s", m_oracle.compared_draws,
			m_oracle.native_draws,
			m_oracle.compared_draws == m_oracle.native_draws
				? ""
				: " ⚠️ INCOMPLETE — the uncompared draws kept their native result");
	}
	m_tex_source.Clear();
	m_palette_cache.Clear();
	m_expand_cache.Clear();
	ReleaseGpuPalettes();
	m_clut_download.reset();
	m_target_pool.ReleaseAll();
	GSRendererSW::Destroy();
}

void GSRendererTile::Reset(bool hardware_reset)
{
	GSRendererSW::Reset(hardware_reset);
	// Everything falls back to CPU-newest; surface textures die with the reset. The
	// CLUT RAM the base reset cleared is CPU-authoritative again by definition.
	m_tex_source.Clear();
	m_palette_cache.Clear();
	m_expand_cache.Clear();
	ReleaseGpuPalettes();
	m_clut_deferred = {};
	m_target_pool.ReleaseAll();
	m_vram_model.Reset();
}

void GSRendererTile::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// The frame boundary itself syncs nothing. Present pulls its own footprint
	// (GetOutput below), every other CPU reader comes through a demand seam
	// (Transfer/LocalRead/Move/floor/native), and the whole-of-truth sync survives
	// only where a complete CPU image IS the contract (ReadbackTextureCache:
	// savestate, renderer switch, teardown). Truth a frame wrote and nothing reads
	// stays GPU-resident indefinitely — that unread set was the vsync-all bill; how
	// much of it re-surfaces at the demand seams is the census's question, per
	// title.
	// The model's invariants are cheap enough to police once a frame in Devel.
	pxAssert(m_vram_model.CheckInvariants());
	m_readback_frames++;
	m_expand_cache.NextFrame();
	GSRendererSW::VSync(field, registers_written, idle_frame);
}

GSTexture* GSRendererTile::GetOutput(int i, float& scale, int& y_offset)
{
	// The presenter deswizzles the display circuit straight out of local memory
	// (GSRendererSW::GetOutput), so exactly its footprint must be CPU-current before
	// the base runs. The rects mirror the base's derivation, including the 2048
	// address wrap that splits the read into up to four pieces. Page granularity
	// makes the base's outward block alignment a no-op here — a block never spans
	// pages — so the pieces cover exactly what rtx reads. Ordering: the base
	// flushed the rasterizer (Sync in GSRendererSW::VSync) before Merge calls this,
	// so no SW draw is in flight over these pages.
	const int index = i >= 0 ? i : 1;
	const GSPCRTCRegs::PCRTCDisplay& fb = PCRTCDisplays.PCRTCDisplays[index];
	const GSVector2i fb_size = PCRTCDisplays.GetFramebufferSize(i);
	const GSVector4i fb_rect = PCRTCDisplays.GetFramebufferRect(i);
	if (!fb_rect.rempty() && fb.FBW != 0 && fb_size.x >= 0 && fb_size.y >= 0)
	{
		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[fb.PSM];
		const int w = fb.FBW * 64;
		const int h = fb_size.y;
		const int off_x = (fb_rect.x & 0x7ff) & ~(psm.bs.x - 1);
		const int off_x_end = ((fb_rect.x & 0x7ff) + (psm.bs.x - 1)) & ~(psm.bs.x - 1);
		const int off_y = (fb_rect.y & 0x7ff) & ~(psm.bs.y - 1);
		const int off_y_end = ((fb_rect.y & 0x7ff) + (psm.bs.y - 1)) & ~(psm.bs.y - 1);
		GSVector4i r(off_x, off_y, w + off_x_end, h + off_y_end);
		GSVector4i rh(off_x, off_y, w + off_x_end, (h + off_y_end) & 0x7FF);
		GSVector4i rw(off_x, off_y, (w + off_x_end) & 0x7FF, h + off_y_end);
		const bool h_wrap = (r.bottom >= 2048);
		const bool w_wrap = (r.right >= 2048);
		if (h_wrap)
		{
			r.bottom = 2048;
			rw.bottom = 2048;
			rh.top = 0;
		}
		if (w_wrap)
		{
			r.right = 2048;
			rh.right = 2048;
			rw.left = 0;
		}

		const GSTileSurfaceLayout layout{static_cast<u32>(fb.Block()), static_cast<u8>(fb.FBW),
			static_cast<u8>(fb.PSM), KindForPsm(fb.PSM)};
		GSPageBitmap pages = GSVramModel::PagesForRect(layout, r);
		if (w_wrap)
			pages |= GSVramModel::PagesForRect(layout, rw);
		if (h_wrap)
			pages |= GSVramModel::PagesForRect(layout, rh);
		if (h_wrap && w_wrap)
			pages |= GSVramModel::PagesForRect(layout, GSVector4i(rw.left, rh.top, rw.right, rh.bottom));
		// All planes: the read is of raw bytes, so newer GPU truth on these
		// addresses is what the screen shows regardless of which plane wrote it.
		ReadbackModelPages(m_vram_model.ReadbackNeeded(pages, kGSTilePlanesAll), ReadbackSite::VSyncDisplay);
	}
	return GSRendererSW::GetOutput(i, scale, y_offset);
}

// Where the readback bill goes, per seam, averaged over the run. Reported once at
// teardown rather than per frame: the interesting quantity is the steady-state rate,
// and a per-frame line would itself be I/O on the thread under investigation.
//
// Gated on the draw ledger because this is an attribution instrument and shares its
// discipline — the perf protocol already treats a ledger-enabled arm as unmeasurable,
// so nothing here can leak into an A/B.
void GSRendererTile::ReportReadbackCensus()
{
	if (!GSDrawLog::IsActive() || m_readback_frames == 0)
		return;

	static constexpr const char* kSiteNames[] = {
		"transfer-write", "cpu-read", "move", "vsync-all", "vsync-display", "floor-draw", "native-draw", "oracle"};
	static_assert(std::size(kSiteNames) == static_cast<u32>(ReadbackSite::Count));

	const double frames = static_cast<double>(m_readback_frames);
	u32 total_drains = 0, total_pages = 0;
	u32 total_pending = 0, total_inflight = 0, total_quiescent = 0;

	Console.WriteLn("Tile readback census over %u frames (per frame):", m_readback_frames);
	Console.WriteLn("  (drains split by what the pulled pages waited on: pending = work still in the buffer");
	Console.WriteLn("   being recorded, the drain is structural; in-flight = a submitted buffer, a wait on it");
	Console.WriteLn("   alone would do; quiescent = already retired, a copy waiting only on itself would do)");
	for (u32 i = 0; i < static_cast<u32>(ReadbackSite::Count); i++)
	{
		const ReadbackCounters& c = m_readback[i];
		if (c.calls == 0)
			continue;
		// pages/drain is the batching headroom: 1.0 means every page cost its own stall.
		Console.WriteLn("  %-14s %8.2f drains  %8.2f pages  %8.2f calls  (%.2f pages/drain)   pending %6.2f  in-flight %6.2f  quiescent %6.2f",
			kSiteNames[i], c.drains / frames, c.pages / frames, c.calls / frames,
			c.drains ? static_cast<double>(c.pages) / static_cast<double>(c.drains) : 0.0,
			c.drains_pending / frames, c.drains_inflight / frames, c.drains_quiescent / frames);
		if (static_cast<ReadbackSite>(i) != ReadbackSite::Oracle)
		{
			total_drains += c.drains;
			total_pages += c.pages;
			total_pending += c.drains_pending;
			total_inflight += c.drains_inflight;
			total_quiescent += c.drains_quiescent;
		}
	}
	Console.WriteLn("  %-14s %8.2f drains  %8.2f pages   (oracle excluded)                    pending %6.2f  in-flight %6.2f  quiescent %6.2f",
		"TOTAL", total_drains / frames, total_pages / frames,
		total_pending / frames, total_inflight / frames, total_quiescent / frames);
	Console.WriteLn("  out-of-band copies (readbacks served without draining the frame's buffer): %.2f per frame",
		static_cast<double>(m_target_pool.OutOfBandCopies()) / frames);

	const NativeSyncReasons& n = m_native_sync;
	if (n.upload_pages | n.steal_pages | n.tex_pages | n.tex_served)
	{
		Console.WriteLn("  native-draw reasons (one stall serves their union; sole = the only reason present):");
		Console.WriteLn("    upload-source %8.2f pages  %8.2f sole", n.upload_pages / frames, n.upload_sole / frames);
		Console.WriteLn("    steal-alias   %8.2f pages  %8.2f sole", n.steal_pages / frames, n.steal_sole / frames);
		Console.WriteLn("    tex-source-rt %8.2f pages  %8.2f sole", n.tex_pages / frames, n.tex_sole / frames);
		// Served draws never reach the reason columns, so without this line a fully
		// served title is indistinguishable from one that never renders to texture.
		Console.WriteLn("    tex-served    %8.2f draws   %8.2f device builds  (%8.2f reinterpreted draws, %8.2f builds; %8.2f direct draws)",
			n.tex_served / frames, static_cast<double>(m_tex_source.DonorBuilds()) / frames,
			n.tex_reinterpreted / frames, static_cast<double>(m_tex_source.ReinterpretBuilds()) / frames, n.tex_direct / frames);
		// The partition of the render-to-texture bill. refuse_format with a sole owner
		// already found is the interesting row: those bytes ARE on the GPU, in a texture
		// we own, in the wrong arrangement -- a conversion, not a fetch.
		Console.WriteLn("      refused: mip %.2f  no-owner %.2f  format %.2f  layout %.2f  geometry %.2f  self-target %.2f",
			n.refuse_mip / frames, n.refuse_no_owner / frames, n.refuse_format / frames,
			n.refuse_layout / frames, n.refuse_geometry / frames, n.refuse_self_target / frames);
		Console.WriteLn("      of the format refusals, %.2f start at the owner's own base",
			n.refuse_format_same_base / frames);
		if (n.feedback_admitted)
			Console.WriteLn("      feedback admitted (sampled core disjoint from write): %.2f draws per frame, %.2f core-served off the owner",
				n.feedback_admitted / frames, n.tex_core_served / frames);
	}
	if (m_expand_cache.Builds() | m_expand_cache.Hits() | m_expand_cache.Deferrals())
	{
		// The palette-expanded source cache (fast profile only). passes/frame is the
		// CLUT-thrash column: a title cycling palettes faster than the cache holds
		// shows up here before it shows up anywhere else. The parent caches print
		// beside it because an expansion build has two possible drivers — a fresh
		// (source, palette) PAIR over stable parents (capacity or cycling), or a
		// parent that itself rebuilt (source volatility) — and only the ratio of
		// these columns says which.
		Console.WriteLn("  palette-expanded sources: %8.2f hits  %8.2f builds  %8.2f passes  %8.2f deferrals per frame%s",
			static_cast<double>(m_expand_cache.Hits()) / frames,
			static_cast<double>(m_expand_cache.Builds()) / frames,
			static_cast<double>(m_expand_cache.Passes()) / frames,
			static_cast<double>(m_expand_cache.Deferrals()) / frames,
			m_expand_cache.Serves() ? "" : "  (device refused; in-shader fallback)");
		Console.WriteLn("    parents: index %8.2f hits  %8.2f builds   palette %8.2f hits  %8.2f builds per frame",
			static_cast<double>(m_tex_source.Hits()) / frames,
			static_cast<double>(m_tex_source.Builds()) / frames,
			static_cast<double>(m_palette_cache.Hits()) / frames,
			static_cast<double>(m_palette_cache.Builds()) / frames);
		if (m_tex_source.RebuildsSameBytes() | m_tex_source.RebuildsNewBytes() | m_tex_source.RebuildsSamePages())
		{
			// Stamp churn tiers, cheapest refusal first: same-pages rebuilds are
			// refused by the raw page hashes before the deswizzle runs at all;
			// same-bytes rebuilds paid their deswizzle (a page changed outside
			// the window's texels) but skipped the upload; new-bytes rebuilds
			// paid everything because the texels really moved.
			Console.WriteLn("    index rebuilds: %8.2f same-pages (deswizzle skipped)  %8.2f same-bytes (upload skipped)  %8.2f new-bytes per frame",
				static_cast<double>(m_tex_source.RebuildsSamePages()) / frames,
				static_cast<double>(m_tex_source.RebuildsSameBytes()) / frames,
				static_cast<double>(m_tex_source.RebuildsNewBytes()) / frames);
		}
	}
	{
		const ClutCensus& c = m_clut_census;
		// The wait bill the drain table cannot see: out-of-band readbacks wait on
	// their own fence, deliberately outside the drain counters — and that wall
	// time carried an entire regression invisibly once (the feedback admission's
	// native/floor ping-pong: +45 OOB waits/frame at ~0.23 ms each on the F4
	// SotC scene, while drains and command-buffer waits both FELL).
	Console.WriteLn("  oob-readback waits %8.2f /frame, %8.2f ms/frame   drain wall %8.2f ms/frame",
		static_cast<double>(g_gs_device->GetOobWaitCalls()) / frames,
		static_cast<double>(g_gs_device->GetOobWaitNs()) / frames / 1e6,
		static_cast<double>(m_target_pool.DrainWallNs()) / frames / 1e6);
	Console.WriteLn("  palette loads %8.2f  deferred %8.2f  gathered on device %8.2f  refused: shape %.2f mixed %.2f",
			c.loads / frames, c.deferred / frames, c.gathered / frames, c.refused_shape / frames, c.refused_mixed / frames);
		Console.WriteLn("  device-palette draws %8.2f (%8.2f direct)  palettes gathered %8.2f  mixed-provenance syncs %.2f  synced back %.2f (%.2f drained)",
			c.gpu_palette_draws / frames, c.direct_palette_draws / frames, c.materialized / frames, c.mixed_syncs / frames,
			c.sync_backs / frames, c.sync_back_drains / frames);
	}
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

	ReadbackModelPages(m_vram_model.SpillBeforeCpuWrite(m_rect_fp, planes), ReadbackSite::Transfer);
	m_vram_model.OnCpuWrite(m_rect_fp, planes);
}

// The CPU is about to read this region out of local memory: any page whose newest
// bytes live on the GPU comes back first — except the blocks of a CLUT load that
// sit on ONE colour surface, whose readback is deferred so PostClutLoad can gather
// the palette out of that surface's texture on the device instead (a game that
// renders its palette and then loads it: GT4, seventy times a frame). PreClutLoad
// reads them back after all if the load's shape is not one the gather serves.
void GSRendererTile::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	const GSTileSurfaceLayout layout{BITBLTBUF.SBP, static_cast<u8>(BITBLTBUF.SBW),
		static_cast<u8>(BITBLTBUF.SPSM), KindForPsm(BITBLTBUF.SPSM)};
	GSVramModel::FootprintForRect(layout, r, m_rect_fp);

	GSPageBitmap need = m_vram_model.ReadbackNeeded(m_rect_fp, kGSTilePlanesAll);
	if (clut && !need.empty() && m_clut_gather_serves && GSConfig.TileGpuClut)
	{
		const GSTileSurfaceId owner = m_vram_model.SoleGpuOwner(need, kGSTilePlanesAll);
		bool deferrable = owner != kGSTileNoSurface;
		if (deferrable)
		{
			const GSVramModel::Surface& s = m_vram_model.Get(owner);
			deferrable = s.layout.kind == GSTileSurfaceKind::Color &&
						 (s.layout.psm == PSMCT32 || s.layout.psm == PSMCT24) && s.pool_handle != 0 &&
						 s.residency.contains(need);
		}
		if (deferrable && (m_clut_deferred.blocks == 0 || m_clut_deferred.owner == owner))
		{
			m_clut_deferred.pages |= need;
			m_clut_deferred.owner = owner;
			m_clut_deferred.blocks++;
			need = GSPageBitmap();
		}
		else if (m_clut_deferred.blocks != 0)
		{
			// Part of this load is deferrable and part is not: the whole load takes the
			// CPU route (PreClutLoad reads the deferred blocks back).
			m_clut_deferred.mixed = true;
		}
	}
	ReadbackModelPages(need, ReadbackSite::LocalRead);

	GSRendererSW::InvalidateLocalMem(BITBLTBUF, r, clut);
}

// -- The palette on the device -----------------------------------------------------

namespace
{
	// The slots a CSM1 32-bit load writes: an eight-bit palette fills slots CSA..15
	// (WriteCLUT_T32_I8_CSM1's loop), a four-bit one fills slot CSA.
	void ClutLoadSlots(const GIFRegTEX0& TEX0, u32& first, u32& count)
	{
		const u32 csa = TEX0.CSA & 15;
		if (GSLocalMemory::m_psm[TEX0.PSM].pal == 256)
		{
			first = csa;
			count = 16 - csa;
		}
		else
		{
			first = csa;
			count = 1;
		}
	}

	// The load shapes the device gather serves: CSM1, a 32-bit palette, and either a
	// four-bit index (one slot) or an eight-bit one at CSA 0 (all sixteen).
	bool GatherServesLoad(const GIFRegTEX0& TEX0)
	{
		const u32 pal = GSLocalMemory::m_psm[TEX0.PSM].pal;
		if (TEX0.CSM != 0 || (TEX0.CPSM & 0x2) != 0 || pal == 0)
			return false;
		return pal == 16 || TEX0.CSA == 0;
	}
} // namespace

void GSRendererTile::PreClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT)
{
	m_clut_census.loads++;
	const bool csm1_32 = TEX0.CSM == 0 && (TEX0.CPSM & 0x2) == 0;

	// A load of any other shape writes the CLUT RAM in units the sixteen-slot mirror
	// does not model (16-bit halves, CSM2 strips): make the CPU RAM whole first, so
	// what the load leaves behind is CPU-authoritative everywhere.
	if (!csm1_32 && m_clut_mirror.AnyUnsynced())
		SyncClutToCpu();

	if (m_clut_deferred.blocks == 0)
		return;
	m_clut_census.deferred++;

	// The deferred blocks become a device gather in PostClutLoad only if the whole
	// load is one the gather serves; otherwise the CPU load that follows needs the
	// bytes, so they come back now.
	if (m_clut_deferred.mixed || !GatherServesLoad(TEX0) || !m_clut_gather_serves)
	{
		if (m_clut_deferred.mixed)
			m_clut_census.refused_mixed++;
		else
			m_clut_census.refused_shape++;
		ReadbackModelPages(m_clut_deferred.pages, ReadbackSite::LocalRead);
		m_clut_deferred = {};
	}
}

void GSRendererTile::PostClutLoad(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT)
{
	u32 first, count;
	const bool csm1_32 = TEX0.CSM == 0 && (TEX0.CPSM & 0x2) == 0;
	if (!csm1_32)
	{
		// PreClutLoad made the RAM whole; the load then rewrote it from CPU bytes.
		m_clut_mirror.OnCpuLoad(0, GSTileClutMirror::kSlots);
		PruneGpuPalettes();
		return;
	}
	ClutLoadSlots(TEX0, first, count);

	if (m_clut_deferred.blocks == 0)
	{
		m_clut_mirror.OnCpuLoad(first, count);
		PruneGpuPalettes();
		return;
	}

	// The palette lives on the device from here. Its record names the owner surface,
	// the source pages and their content stamp, and the load's registers; the texture
	// is gathered LAZILY — a draw whose palette source cannot have moved samples the
	// owner directly through the CLUT's word order (PS_TILE_DIRECT_PAL, no pass of its
	// own), and only a source about to be overwritten, or a draw that would read its
	// own target, or a CPU consumer, makes the gather happen (MaterializePalette).
	// Where the device cannot address a target in the fragment shader at all, the
	// gather happens now.
	const DeferredClutBlocks deferred = m_clut_deferred;
	m_clut_deferred = {};
	const GSVramModel::Surface& owner = m_vram_model.Get(deferred.owner);
	GpuPalette gp = {};
	gp.handle = m_gpu_palette_next++;
	gp.tex = nullptr;
	gp.entries = GSLocalMemory::m_psm[TEX0.PSM].pal;
	gp.owner = deferred.owner;
	gp.pages = deferred.pages;
	gp.stamp = GSTileTextureSource::GenStamp(m_vram_model, deferred.pages);
	gp.cbp = TEX0.CBP;
	gp.owner_bp = owner.layout.bp;
	gp.owner_bwpg = owner.layout.bw;
	m_gpu_palettes.push_back(gp);
	m_clut_census.gathered++;
	if (!DirectPaletteServes() && !MaterializePalette(m_gpu_palettes.back()))
	{
		// The device serves neither the direct leg nor the gather: read the blocks back
		// and load again from current bytes; from here on the route is never offered.
		m_gpu_palettes.pop_back();
		m_clut_gather_serves = false;
		ReadbackModelPages(deferred.pages, ReadbackSite::LocalRead);
		m_mem.m_clut.WriteLoad(TEX0, TEXCLUT);
		m_clut_mirror.OnCpuLoad(first, count);
		PruneGpuPalettes();
		return;
	}
	m_clut_mirror.OnGpuLoad(first, count, gp.handle);
	PruneGpuPalettes();
}

bool GSRendererTile::DirectSamplingServes()
{
	if (m_direct_probed)
		return m_direct_serves;
	m_direct_probed = true;
	bool clut_ok = false;
	m_direct_serves = g_gs_device->TileSwizzleFormsFit(clut_ok);
	m_direct_clut_serves = m_direct_serves && clut_ok;
	return m_direct_serves;
}

bool GSRendererTile::DirectPaletteServes()
{
	DirectSamplingServes();
	return m_direct_clut_serves;
}

// The gather, on demand: the palette's source words, in the loaders' own order, out
// of the owner texture into an N×1 palette texture — the same texels the CPU palette
// cache would have uploaded from Read32, had the bytes been on the CPU. Only valid
// while the source pages hold the bytes the load read, which is why it is called
// before anything overwrites them.
bool GSRendererTile::MaterializePalette(GpuPalette& gp)
{
	if (gp.tex)
		return true;
	const GSVramModel::Surface& owner = m_vram_model.Get(gp.owner);
	GSTexture* owner_tex = (owner.alive && owner.pool_handle) ? m_target_pool.GetTexture(owner.pool_handle) : nullptr;
	if (!owner_tex)
		return false;
	// (The pages' content stamp may have moved without the owner TEXTURE moving —
	// another surface writing the same page numbers — which is why the stamp gates
	// only the direct leg, and this gather keys on the owner's texture alone.)
	GSTexture* pal = g_gs_device->CreateRenderTarget(static_cast<int>(gp.entries), 1, GSTexture::Format::Color, false, true);
	if (!pal)
		return false;
	GSDevice::TileClutGatherParams params = {};
	params.cbp = gp.cbp;
	params.dst_bp = gp.owner_bp;
	params.dst_bwpg = gp.owner_bwpg;
	params.entries = gp.entries;
	if (!g_gs_device->TileClutFromTarget(owner_tex, pal, params))
	{
		g_gs_device->Recycle(pal);
		return false;
	}
	gp.tex = pal;
	m_clut_census.materialized++;
	return true;
}

// Every ungathered device palette whose source lies under `pages` is gathered NOW —
// called before a native draw renders into, or uploads over, those pages, because
// they are the only two writers of a pool texture and the palette's bytes are about
// to stop existing there.
void GSRendererTile::MaterializePalettesOn(const GSPageBitmap& pages)
{
	for (GpuPalette& gp : m_gpu_palettes)
	{
		if (!gp.tex && gp.pages.intersects(pages))
			MaterializePalette(gp);
	}
}

GSRendererTile::PaletteChoice GSRendererTile::ResolvePalette(const GIFRegTEX0& TEX0, bool allow_sync,
	GSTileSurfaceId draw_fb, GSTileSurfaceId draw_z, bool allow_direct)
{
	PaletteChoice choice;
	const u32 pal = GSLocalMemory::m_psm[TEX0.PSM].pal;
	if (pal == 0 || !m_clut_mirror.AnyGpu())
		return choice;

	// The slots the draw reads: a four-bit index reads slot CSA; an eight-bit one reads
	// group (idx >> 4) | CSA — all sixteen at CSA 0, a scattered subset otherwise, which
	// the CPU route handles once the RAM is whole.
	u32 first, count;
	if (pal == 16)
	{
		first = TEX0.CSA & 15;
		count = 1;
	}
	else if (TEX0.CSA == 0)
	{
		first = 0;
		count = 16;
	}
	else
	{
		if (allow_sync)
			SyncClutToCpu();
		choice.cpu_current = m_clut_mirror.CpuCurrent(0, GSTileClutMirror::kSlots);
		return choice;
	}

	const GSTileClutMirror::Resolution r = m_clut_mirror.Resolve(first, count);
	if (r.verdict == GSTileClutMirror::Verdict::Gpu)
	{
		GpuPalette* gp = FindGpuPalette(r.handle);
		if (gp && !gp->tex && allow_sync)
		{
			// Direct: the draw samples the owner texture through the CLUT's word order,
			// no gather — when the source still holds the load's bytes and the draw does
			// not write that surface (a read of its own target is a feedback loop).
			const GSVramModel::Surface& owner = m_vram_model.Get(gp->owner);
			GSTexture* owner_tex = (owner.alive && owner.pool_handle) ? m_target_pool.GetTexture(owner.pool_handle) : nullptr;
			const bool source_intact = owner_tex && GSTileTextureSource::GenStamp(m_vram_model, gp->pages) == gp->stamp;
			const bool not_own_target = gp->owner != draw_fb && gp->owner != draw_z;
			if (allow_direct && source_intact && not_own_target && DirectPaletteServes())
			{
				choice.gpu = owner_tex;
				choice.direct = true;
				choice.direct_params = GSVector4i(static_cast<int>(gp->cbp), static_cast<int>(gp->owner_bp),
					static_cast<int>(gp->owner_bwpg),
					static_cast<int>(((gp->entries == 256) ? 0x100u : 0u) | r.first_texel));
			}
			else
			{
				MaterializePalette(*gp);
			}
		}
		if (!choice.direct && gp && gp->tex)
		{
			GSTexture* tex = gp->tex;
			if (tex->GetWidth() != static_cast<int>(pal))
				tex = GpuPaletteViewFor(r.handle, r.first_texel);
			choice.gpu = tex;
		}
	}
	else if (r.verdict == GSTileClutMirror::Verdict::Mixed && allow_sync)
	{
		SyncClutToCpu();
		m_clut_census.mixed_syncs++;
	}
	choice.cpu_current = m_clut_mirror.CpuCurrent(first, count);
	if (!choice.gpu && !choice.cpu_current && allow_sync)
	{
		// A device slot with no palette texture behind it (a lost view): sync and fall
		// to the CPU route rather than bind nothing.
		SyncClutToCpu();
		choice.cpu_current = m_clut_mirror.CpuCurrent(first, count);
	}
	return choice;
}

void GSRendererTile::SyncClutToCpu()
{
	if (!m_clut_mirror.AnyUnsynced())
		return;

	if (!m_clut_download || m_clut_download->GetWidth() < 256)
		m_clut_download = g_gs_device->CreateDownloadTexture(256, 1, GSTexture::Format::Color);
	if (!m_clut_download)
		return;

	for (GpuPalette& gp : m_gpu_palettes)
	{
		bool wanted = false;
		m_clut_mirror.ForEachUnsyncedSlot([&](u32, u32 handle, u32) { wanted |= (handle == gp.handle); });
		if (!wanted)
			continue;
		if (!MaterializePalette(gp))
			continue;

		const GSVector4i rc(0, 0, static_cast<int>(gp.entries), 1);
		g_gs_device->HintReadbackSource(gp.tex);
		bool ok;
		if (GSConfig.TileOutOfBandReadback && m_clut_download->CopyFromCompletedTexture(rc, gp.tex, rc, 0, true))
		{
			ok = true;
		}
		else
		{
			m_clut_download->CopyFromTexture(rc, gp.tex, rc, 0, true);
			m_clut_download->Flush();
			m_clut_census.sync_back_drains++;
			ok = true;
		}
		if (!ok || !m_clut_download->Map(rc))
			continue;
		const u32* words = reinterpret_cast<const u32*>(m_clut_download->GetMapPointer());
		m_clut_mirror.ForEachUnsyncedSlot([&](u32 slot, u32 handle, u32 texel) {
			if (handle == gp.handle)
				m_mem.m_clut.SetEntries32(slot * GSTileClutMirror::kEntriesPerSlot, GSTileClutMirror::kEntriesPerSlot, words + texel);
		});
		m_clut_download->Unmap();
		m_clut_mirror.MarkSynced(gp.handle);
		m_clut_census.sync_backs++;
	}
}

GSRendererTile::GpuPalette* GSRendererTile::FindGpuPalette(u32 handle)
{
	for (GpuPalette& gp : m_gpu_palettes)
		if (gp.handle == handle)
			return &gp;
	return nullptr;
}

GSTexture* GSRendererTile::GpuPaletteViewFor(u32 handle, u32 first_texel)
{
	for (const GpuPaletteView& v : m_gpu_palette_views)
		if (v.handle == handle && v.first_texel == first_texel)
			return v.tex;
	GpuPalette* gp = FindGpuPalette(handle);
	if (!gp || !MaterializePalette(*gp))
		return nullptr;
	GSTexture* src = gp->tex;
	GSTexture* tex = g_gs_device->CreateTexture(16, 1, 1, GSTexture::Format::Color, true);
	if (!tex)
		return nullptr;
	g_gs_device->CopyRect(src, tex, GSVector4i(static_cast<int>(first_texel), 0, static_cast<int>(first_texel) + 16, 1), 0, 0);
	m_gpu_palette_views.push_back({handle, first_texel, tex});
	return tex;
}

void GSRendererTile::PruneGpuPalettes()
{
	for (size_t i = 0; i < m_gpu_palettes.size();)
	{
		if (m_clut_mirror.References(m_gpu_palettes[i].handle))
		{
			i++;
			continue;
		}
		const u32 handle = m_gpu_palettes[i].handle;
		if (m_gpu_palettes[i].tex)
			g_gs_device->Recycle(m_gpu_palettes[i].tex);
		m_gpu_palettes.erase(m_gpu_palettes.begin() + static_cast<std::ptrdiff_t>(i));
		for (size_t v = 0; v < m_gpu_palette_views.size();)
		{
			if (m_gpu_palette_views[v].handle == handle)
			{
				g_gs_device->Recycle(m_gpu_palette_views[v].tex);
				m_gpu_palette_views.erase(m_gpu_palette_views.begin() + static_cast<std::ptrdiff_t>(v));
			}
			else
				v++;
		}
	}
}

void GSRendererTile::ReleaseGpuPalettes()
{
	for (const GpuPalette& gp : m_gpu_palettes)
		if (gp.tex)
			g_gs_device->Recycle(gp.tex);
	m_gpu_palettes.clear();
	for (const GpuPaletteView& v : m_gpu_palette_views)
		g_gs_device->Recycle(v.tex);
	m_gpu_palette_views.clear();
	m_clut_mirror.Reset();
}

// The close/switch seam Classic reads its texture cache back on: everything the GPU
// holds newest comes down, palettes included.
void GSRendererTile::ReadbackTextureCache()
{
	SyncAllTruthToCpu(true);
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

	ReadbackModelPages(need, ReadbackSite::Move);

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

// Everything the pure lowering reads, gathered off the current draw. Split from the
// lowering itself because the two halves have very different costs and only one of
// them is memoizable: this half consults the vertex trace and the CLUT and must run
// on every draw, while gsTileLowerDraw is a pure function of what it returns.
//
// The struct is memset rather than aggregate-initialized so its PADDING is defined
// too — the memo compares inputs as bytes, and `= {}` says nothing about the gaps.
GSTileDrawInput GSRendererTile::BuildLoweringInput()
{
	GSTileDrawInput in;
	std::memset(&in, 0, sizeof(in));
	in.prim_class = m_vt.m_primclass;
	in.TEST = m_context->TEST;
	in.FRAME = m_context->FRAME;
	in.ZBUF = m_context->ZBUF;
	in.scanmsk = static_cast<u8>(m_draw_env->SCANMSK.MSK);
	// The post-texture-function alpha classification, textured draws included:
	// CalcAlphaMinMax folds TFX, TEXA and the CLUT into the trace range — the same
	// shared machinery Classic's alpha_c0 vector rides. Both consumers want exactly
	// this alpha: the alpha test reads the texture function's output, and so do the
	// blend's As factor and PABE (pre-FBA, console-measured, gs-alpha2). The
	// coverage-alpha 128-fudge inside it never fires here — the SW-derived override
	// reports coverage alpha supported.
	{
		// ⚠️ The palettised leg scans the CLUT's DECODED read buffer, which is only
		// current after Read32 — Classic refreshes it at this exact point for the
		// same reason ("required to compute TryAlphaTest"). Without this, the scan
		// reads the PREVIOUS palettised draw's palette and the classification is
		// unsound: first light was provable-alpha-test verdicts firing off the wrong
		// palette, 6.5% of a Katamari frame at max 224. Cheap when clean — Read32
		// early-outs on an unchanged palette, and the floor's own later call
		// becomes a no-op.
		// A palette the device loaded and the CPU has not synced cannot be scanned:
		// the classification goes conservative (the alpha test then lowers dynamically
		// and PABE/blend rungs see the full range) rather than paying a readback for a
		// verdict. The floor, if this draw floors, syncs before it samples.
		const bool palettised = PRIM->TME && GSLocalMemory::m_psm[m_context->TEX0.PSM].pal > 0;
		if (palettised && !ResolvePalette(m_context->TEX0, false, kGSTileNoSurface, kGSTileNoSurface).cpu_current)
		{
			in.alpha_min = 0;
			in.alpha_max = 255;
		}
		else
		{
			if (palettised)
				m_mem.m_clut.Read32(m_context->TEX0, m_draw_env->TEXA);
			const GSVertexTrace::VertexAlpha& alpha = GetAlphaMinMax();
			in.alpha_min = static_cast<u8>(std::clamp(alpha.min, 0, 255));
			in.alpha_max = static_cast<u8>(std::clamp(alpha.max, 0, 255));
		}
	}
	// What the lowering needs to know about fragment ordering. PrimitiveOverlap
	// walks the vertex list, so ask only where an admission actually turns on the
	// answer — two rungs do. The alpha-test split needs it when the split would
	// otherwise reorder the draw's own depth writes; the read rung needs it on
	// every blended draw, because its realization reads ONE state of the
	// destination and that is the true destination only for a pixel blended into
	// once.
	in.z_constant = m_vt.m_eq.z;
	const bool split_wants_overlap =
		m_context->TEST.ATE && m_context->TEST.ATST != ATST_ALWAYS && m_context->TEST.ATST != ATST_NEVER &&
		m_context->TEST.ZTE && !m_context->ZBUF.ZMSK && !m_vt.m_eq.z;
	m_prim_overlap = PRIM_OVERLAP_UNKNOW;
	if (split_wants_overlap || PRIM->ABE)
	{
		m_prim_overlap = PrimitiveOverlap();
		in.prim_overlap_none = m_prim_overlap == PRIM_OVERLAP_NO;
	}
	in.fast_atst = GSConfig.TileFastShading && !GSConfig.TileExactAlphaTest;
	// The carrier implies both blend admissions. A carrier without them leaves a
	// residue of blend draws flooring BETWEEN carrier-collapsed native draws, and
	// under the plane depth leg that mixes walk-form z (the floor's) with
	// plane-form z (the natives') inside one coplanar stack — the ±1 the plane
	// contract allows cancels within a realization, not across the seam, so whole
	// layers flip (OutRun-0812: FLIP 0.376 vs 0.044 with the admissions on).
	// The pins remain independent so the ceiling brackets can still measure the
	// read rung without the carrier; only the reverse combination is closed off.
	in.blend_overlap_native = GSConfig.TileBlendOverlapNative || GSConfig.TileBlendClassicCarrier;
	in.blend_tex_sample_native = GSConfig.TileBlendTexSampleNative || GSConfig.TileBlendClassicCarrier;
	in.blend_classic_carrier = GSConfig.TileBlendClassicCarrier;
	in.dual_source_blend = g_gs_device->Features().dual_source_blend && !GSConfig.TileBlendNoDualSource;
	in.tme = PRIM->TME;
	in.abe = PRIM->ABE;
	// The blend equation's registers (M4). ALPHA and PABE are only consulted under
	// abe; COLCLAMP is the M4a wrap guardrail. Environment registers come from
	// m_draw_env for the same reason SCANMSK's does.
	in.ALPHA = m_context->ALPHA;
	in.colclamp = m_draw_env->COLCLAMP.CLAMP;
	in.pabe = m_draw_env->PABE.PABE;
	in.aa1 = PRIM->AA1;
	in.fba = m_context->FBA.FBA;
	in.dthe = m_draw_env->DTHE.DTHE;
	in.fge = PRIM->FGE;
	// The vertex trace's colour floor (min over vertices of min(R,G,B)) — the mix
	// rung's saturation guard. Interpolation cannot go below the vertex minimum,
	// and the shader's stored-byte truncation cannot either.
	in.color_min_rgb = static_cast<u8>(std::min({m_vt.m_min.c.I32[0] & 0xFF,
		m_vt.m_min.c.I32[1] & 0xFF, m_vt.m_min.c.I32[2] & 0xFF}));
	in.vs_expand = g_gs_device->Features().vs_expand;
	if (PRIM->TME)
	{
		// The same criteria the SW scanline uses to choose its filter and mip state,
		// so the native path and the floor agree on which draws do what.
		in.tex_linear = m_vt.IsLinear();
		in.tex_mip = IsMipMapActive();
		in.tex_fst = PRIM->FST;
		in.tex_stq_tri_native = GSConfig.TilePerspectiveNative;
		in.tex_fast_sample = GSConfig.TileFastShading && !GSConfig.TileExactTexCoord;
		in.tex_fast_mip = in.tex_fast_sample;
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
	return in;
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
bool GSRendererTile::ReadbackModelPages(const GSPageBitmap& pages, ReadbackSite site)
{
	if (pages.empty())
		return true;

	ReadbackCounters& rc = m_readback[static_cast<u32>(site)];
	rc.calls++;
	rc.pages += static_cast<u32>(pages.count());
	u32* const drains = &rc.drains;

	// Classify BEFORE pulling: the pull itself advances the device's completed epoch,
	// so read afterwards every drain would look quiescent. The class is per call — the
	// pages of one call are pulled together, so they wait on the newest epoch among
	// them.
	const u32 drains_before = rc.drains;
	const u64 page_epoch = m_vram_model.GpuEpochOf(pages);
	const u64 submit_epoch = g_gs_device->GetSubmitEpoch();
	const u64 done_epoch = g_gs_device->GetCompletedSubmitEpoch();
	const int epoch_class = (page_epoch <= done_epoch) ? 2 : (page_epoch < submit_epoch ? 1 : 0);
	const auto attribute_drains = [&]() {
		const u32 n = rc.drains - drains_before;
		if (epoch_class == 0)
			rc.drains_pending += n;
		else if (epoch_class == 1)
			rc.drains_inflight += n;
		else
			rc.drains_quiescent += n;
	};

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
				PlaneByteMask(pi, surf.layout), m_vram_model.TruthMask(page, pi), drains);
			InvalidateSwTexCache(surf.layout, one);
		}
	});

	const GSPageBitmap full_pages = pages.andnot(partial_pages);
	if (full_pages.empty())
	{
		m_vram_model.OnReadback(pages);
		attribute_drains();
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
					PlaneByteMask(pi, surf.layout), GSVramModel::kFullBlockMask, drains);
				InvalidateSwTexCache(surf.layout, one);
			}
		});
		m_vram_model.OnReadback(pages);
		attribute_drains();
		return ok;
	}

	bool ok = partial_ok;
	for (u32 b = 0; b < num_buckets; b++)
	{
		const GSVramModel::Surface& surf = m_vram_model.Get(buckets[b].id);
		ok &= m_target_pool.ReadbackPages(m_mem, surf.pool_handle, surf.layout, buckets[b].pages, buckets[b].mask,
			GSVramModel::kFullBlockMask, drains);
		InvalidateSwTexCache(surf.layout, buckets[b].pages);
	}
	m_vram_model.OnReadback(pages);
	attribute_drains();
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

void GSRendererTile::SyncAllTruthToCpu(bool palettes)
{
	GSPageBitmap need;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		need |= m_vram_model.Truth(pi).andnot(m_vram_model.SyncedPages(pi));
	ReadbackModelPages(need, ReadbackSite::VSyncAll);
	// The CLUT RAM is not part of a savestate (Defrost resets it and the next TEX0 write
	// reloads it from memory) and the presenter never reads it, so the frame boundary
	// leaves device palettes where they are; the close/switch seam pulls them too.
	if (palettes)
		SyncClutToCpu();
}

// The floor reads AND writes its fb/z footprints (read-modify-write rasterization)
// and reads its texture, all against CPU local memory: pull every plane under them.
// Over-pulling costs a readback; under-pulling costs bytes.
void GSRendererTile::SpillForFloorDraw(const GSTileDrawPlan& plan, const GSVector4i& r)
{
	// The floor samples through the CPU's CLUT: a palette the device loaded comes down
	// first (a readback of its texels, not of any page).
	if (PRIM->TME && GSLocalMemory::m_psm[m_context->TEX0.PSM].pal > 0 && m_clut_mirror.AnyUnsynced())
		SyncClutToCpu();

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

	// The floor's spill, attributed to its row like the native path's: pages
	// whether or not anything drained (the copy is paid either way), drains off
	// the counter. This is what makes the floor's arrangement cost separable in
	// the ledger instead of only visible in the teardown census totals.
	const u32 drains_before = m_readback[static_cast<u32>(ReadbackSite::FloorDraw)].drains;
	ReadbackModelPages(need, ReadbackSite::FloorDraw);
	if (GSDrawLog::IsActive())
	{
		const u32 drains = m_readback[static_cast<u32>(ReadbackSite::FloorDraw)].drains - drains_before;
		GSDrawLog::NoteTileSync(static_cast<u32>(need.count()), drains, GSDrawLog::TileSyncFloorSpill);
	}
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

// -- The per-draw lockstep oracle ------------------------------------------------------
//
// The instrument #76 was missing. Frame scoring says how much of a frame is wrong; it
// cannot say WHICH DRAW made it wrong, and inferring that from percentages is how the
// campaign twice wrote a mechanism onto a class that measurement later excluded.
//
// Per native draw, with the oracle armed:
//
//   1. sync the draw's inputs to CPU truth and snapshot the write footprint
//   2. run the native draw exactly as the real path would
//   3. read its result back and keep it
//   4. put the footprint back the way it was, and run GSRendererSW over it
//   5. compare the two results AGAINST THE SNAPSHOT, not just against each other
//
// Step 4 is what makes the ledger readable all the way down instead of only at its
// first row: every draw is judged from the state it actually started in, so a
// divergence is authored by that draw and never inherited. Step 5's three-way split
// separates "the two arms computed different values" from "one arm did not write here
// at all" -- a shading bug and a coverage-or-claim bug, which one percentage cannot
// tell apart.
//
// ⚠️ Step 1 is the one place the instrument touches the run it is measuring. It only
// ever moves bytes GPU->CPU, so it can cost time and cannot corrupt -- but the honest
// treatment is to record it rather than argue it: every row carries how many pages the
// sync had to pull, and a row that pulled ZERO is perturbation-free by construction.
// (Ablations that forced synchronisation have already produced one confounded number in
// this campaign; the fix is to make the perturbation visible per row.)

void GSRendererTile::OracleSnapshot::Capture(const u8* vm8, const GSPageBitmap& pgs)
{
	pages = pgs;
	bytes.resize(static_cast<size_t>(pgs.count()) * GS_PAGE_SIZE);
	size_t at = 0;
	pgs.forEachSetPage([&](u32 page) {
		std::memcpy(bytes.data() + at, vm8 + static_cast<size_t>(page) * GS_PAGE_SIZE, GS_PAGE_SIZE);
		at += GS_PAGE_SIZE;
	});
}

void GSRendererTile::OracleSnapshot::Restore(u8* vm8) const
{
	size_t at = 0;
	pages.forEachSetPage([&](u32 page) {
		std::memcpy(vm8 + static_cast<size_t>(page) * GS_PAGE_SIZE, bytes.data() + at, GS_PAGE_SIZE);
		at += GS_PAGE_SIZE;
	});
}

// SubmitNativeDraw de-indexes and flattens in place, so the buffer the SW arm would see
// afterwards is not the draw the game issued.
void GSRendererTile::OracleSaveVertices()
{
	const u32 vcount = std::max(m_vertex->tail, m_vertex->next);
	m_oracle.verts.assign(m_vertex->buff, m_vertex->buff + vcount);
	m_oracle.indices.assign(m_index->buff, m_index->buff + m_index->tail);
	m_oracle.v_head = m_vertex->head;
	m_oracle.v_tail = m_vertex->tail;
	m_oracle.v_next = m_vertex->next;
	m_oracle.i_tail = m_index->tail;
}

void GSRendererTile::OracleRestoreVertices()
{
	// De-indexing swaps buff with buff_copy and can grow both; write through whichever
	// pointer is live now, which is always at least as large as what was saved.
	std::memcpy(m_vertex->buff, m_oracle.verts.data(), m_oracle.verts.size() * sizeof(GSVertex));
	std::memcpy(m_index->buff, m_oracle.indices.data(), m_oracle.indices.size() * sizeof(u16));
	m_vertex->head = m_oracle.v_head;
	m_vertex->tail = m_oracle.v_tail;
	m_vertex->next = m_oracle.v_next;
	m_index->tail = m_oracle.i_tail;
}

void GSRendererTile::OracleReadPixels(const GSVector4i& r, u32 bp, u32 bw, u32 psm, std::vector<u32>& out) const
{
	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
	const int w = r.width();
	const int h = r.height();
	out.resize(static_cast<size_t>(std::max(w, 0)) * static_cast<size_t>(std::max(h, 0)));
	size_t at = 0;
	for (int y = r.y; y < r.w; y++)
	{
		for (int x = r.x; x < r.z; x++)
			out[at++] = (m_mem.*p.rp)(x, y, bp, bw);
	}
}

// A raw restore writes local memory behind the SW rasterizer's texture cache, which
// keys deswizzled copies on these very pages. ReadbackModelPages does this for itself;
// the oracle's restores have to as well or a later floor draw samples a stale copy.
void GSRendererTile::OracleInvalidateFootprint()
{
	InvalidateSwTexCache(m_oracle.fb_l, m_oracle.fp);
	if (m_oracle.z_l.psm != m_oracle.fb_l.psm || m_oracle.z_l.bp != m_oracle.fb_l.bp)
		InvalidateSwTexCache(m_oracle.z_l, m_oracle.fp);
}

bool GSRendererTile::OracleBeginDraw(const GSTileDrawPlan& plan, const GSVector4i& r)
{
	const GSDrawingContext* ctx = m_context;

	m_oracle.fb_l = GSTileSurfaceLayout{ctx->FRAME.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->FRAME.PSM), GSTileSurfaceKind::Color};
	m_oracle.z_l = GSTileSurfaceLayout{ctx->ZBUF.Block(), static_cast<u8>(ctx->FRAME.FBW),
		static_cast<u8>(ctx->ZBUF.PSM), GSTileSurfaceKind::Depth};

	// The compared footprint is every page either arm could write. Over-including only
	// costs a memcpy of pages that then compare equal; under-including hides a defect.
	m_oracle.fp = PagesForTargetRect(m_oracle.fb_l, r);
	if (plan.z_write || plan.z_test)
		m_oracle.fp |= PagesForTargetRect(m_oracle.z_l, r);

	// ⚠️ There was a refusal here for footprints over 128 pages, justified as "a
	// pathological layout (zero stride) claims all 512 pages, and such a draw floors on
	// the native route anyway". BOTH halves were wrong, and it cost a day.
	//
	// A zero-stride draw floors as StrideZero before the route is reached, so the
	// pathological case the guard named cannot arrive. What DID arrive is every ordinary
	// full-screen draw: a 640x448 32-bit target is 1.14 MB, which is 140 pages. So the
	// guard silently excluded the largest draws in the frame -- the ones most worth
	// comparing -- and because a refusal leaves the draw to run natively with nothing
	// restored afterwards, an oracle run stopped being a software-equivalent run.
	// Measured on Dirge: 8 of 72 native draws uncompared, which is why the run's frames
	// were not the software frames (task #118).
	//
	// The footprint is bounded by the 512-page address space, so the worst case is 4 MB
	// per snapshot. An instrument that already reads back every draw can afford that.
	// The coverage counters below exist so this class of hole can never be silent again:
	// natives submitted and natives compared are reported side by side, and a gap is a
	// defect in the instrument rather than a quiet omission from its ledger.

	// Drain the floor queue FIRST. The software rasterizer is asynchronous whenever it
	// has worker threads -- a floored draw returns long before its pixels exist -- and
	// the native route drains it (Sync(8)) after this function has already snapshotted
	// `pre`. Those pixels then land in CPU memory on the native side of the comparison
	// and nowhere on the software side, so the previous draw's output is charged to this
	// draw as thousands of pixels the native path "wrote" and software "did not".
	//
	// Measured on GT4 OPB: fourteen of twenty-seven divergent draws were this, up to 7128
	// pixels at the maximum delta, every one of them immediately preceded by a floored
	// draw over the same rectangle. The ledger was nondeterministic run to run while the
	// frames it was scoring were byte-identical across three runs -- which is the tell,
	// and the reason a lockstep instrument has to own its own quiescence rather than
	// inherit whatever the renderer happened to have flushed.
	Sync(10);

	// Inputs to CPU truth, so the SW arm reads exactly what the native arm will sample
	// and blend against. Counted, because this is the instrument's only footprint on
	// the run it measures.
	//
	// Syncing the whole COMPARED footprint here as well was tried -- on the theory that
	// the native route's own input sync, which runs after this point over a different
	// page set, could publish a page `pre` had missed. It pulled nothing on GT4 OPB and
	// left the ledger byte-identical, so it is not here: the drain above was the entire
	// mechanism, and an inert readback in an instrument is a future misattribution.
	GSPageBitmap before;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		before |= m_vram_model.Truth(pi).andnot(m_vram_model.SyncedPages(pi));
	SpillForFloorDraw(plan, r);
	GSPageBitmap after;
	for (u32 pi = 0; pi < kGSTilePlaneCount; pi++)
		after |= m_vram_model.Truth(pi).andnot(m_vram_model.SyncedPages(pi));
	m_oracle.sync_pages = before.andnot(after).count();

	m_oracle.pre.Capture(m_mem.vm8(), m_oracle.fp);
	OracleSaveVertices();
	m_oracle.armed = true;
	return true;
}

void GSRendererTile::OracleAbandonDraw()
{
	// The native route bailed. It never reached SubmitNativeDraw, so the vertex buffer
	// is untouched -- restoring it anyway keeps that a property of this function rather
	// than of a call ordering somewhere else.
	if (!m_oracle.armed)
		return;
	OracleRestoreVertices();
	m_oracle.armed = false;
}

void GSRendererTile::OracleCompareDraw(const GSTileDrawPlan& plan, const GSVector4i& r)
{
	if (!m_oracle.armed)
		return;
	m_oracle.armed = false;

	const GSDrawingContext* ctx = m_context;
	u8* const vm8 = m_mem.vm8();

	// The native arm's result, in CPU local memory. Bytes the draw did not claim keep
	// their pre values, which is exactly what makes an unclaimed write show up below as
	// sw_only rather than silently as agreement.
	//
	// Captured twice, on either side of the readback. The native arm is two steps and
	// they fail differently: the draw can render the wrong thing, and the readback can
	// publish bytes CPU memory never held. Both land in `gpu` as one number, and a
	// readback-authored difference wears the costume of a loud rasterization bug --
	// thousands of pixels, gpu_only, maximum delta -- on a draw whose shader wrote no
	// such channel. Differencing the two snapshots below tells them apart per byte.
	m_oracle.raw.Capture(vm8, m_oracle.fp);
	ReadbackModelPages(m_vram_model.ReadbackNeeded(m_oracle.fp, kGSTilePlanesAll), ReadbackSite::Oracle);
	m_oracle.gpu.Capture(vm8, m_oracle.fp);

	// The SW arm, over the same inputs.
	m_oracle.pre.Restore(vm8);
	OracleInvalidateFootprint();
	OracleRestoreVertices();
	GSRendererSW::Draw();
	Sync(9);
	m_oracle.sw.Capture(vm8, m_oracle.fp);

	// Handing the footprint back to the CPU here -- OnCpuWrite over `fp`, so the model
	// stops recording the surface as its owner and the next native draw must re-upload
	// from the corrected bytes -- was tried and is NOT here, because it moved no pixels.
	//
	// The reasoning was sound and the measurement still said no: two CPU writes happen
	// above behind the model's back (the restore and the software arm), a readback marks
	// pages `synced` without clearing truth or ownership, and PagesNeedingUpload consults
	// ownership -- so in principle the next draw skips its upload and renders onto a
	// texture still holding the native result. Measured on Dirge and GT4 OPB, an oracle
	// run's frames are pixel-identical with and without it. Recorded rather than kept:
	// an instrument that carries machinery no measurement justifies is a future
	// misattribution, and the next reader would take it for load-bearing.

	// Three passes over the rect, one per memory state. vm8 holds the SW result now.
	const bool want_c = plan.colormask != 0 || ctx->FRAME.FBMSK != 0xFFFFFFFFu;
	const bool want_z = plan.z_write;
	const u32 fb_bp = ctx->FRAME.Block();
	const u32 fbw = ctx->FRAME.FBW;
	const u32 fb_psm = ctx->FRAME.PSM;
	const u32 z_bp = ctx->ZBUF.Block();
	const u32 z_psm = ctx->ZBUF.PSM;

	if (want_c)
		OracleReadPixels(r, fb_bp, fbw, fb_psm, m_oracle.c_sw);
	if (want_z)
		OracleReadPixels(r, z_bp, fbw, z_psm, m_oracle.z_sw);

	m_oracle.pre.Restore(vm8);
	if (want_c)
		OracleReadPixels(r, fb_bp, fbw, fb_psm, m_oracle.c_pre);
	if (want_z)
		OracleReadPixels(r, z_bp, fbw, z_psm, m_oracle.z_pre);

	m_oracle.gpu.Restore(vm8);
	OracleInvalidateFootprint();
	if (want_c)
		OracleReadPixels(r, fb_bp, fbw, fb_psm, m_oracle.c_gpu);
	if (want_z)
		OracleReadPixels(r, z_bp, fbw, z_psm, m_oracle.z_gpu);

	GSTileOracle::Row row = {};
	row.frame = static_cast<u32>(g_perfmon.GetFrame());
	row.draw = static_cast<u32>(s_n);
	row.prim_type = static_cast<u8>(PRIM->PRIM);
	row.prim_count = static_cast<u16>(std::min<u32>(m_oracle.i_tail, 0xFFFFu));
	row.rect_x = static_cast<s16>(r.x);
	row.rect_y = static_cast<s16>(r.y);
	row.rect_z = static_cast<s16>(r.z);
	row.rect_w = static_cast<s16>(r.w);
	row.fb_bp = fb_bp;
	row.fb_bw = static_cast<u8>(fbw);
	row.fb_psm = static_cast<u8>(fb_psm);
	row.fbmsk = ctx->FRAME.FBMSK;
	row.colormask = plan.colormask;
	row.z_bp = z_bp;
	row.z_psm = static_cast<u8>(z_psm);
	row.z_write = plan.z_write ? 1 : 0;
	row.ztst = plan.ztst;
	row.shape = static_cast<u8>((PRIM->TME ? 1 : 0) | (PRIM->IIP ? 2 : 0) | (PRIM->ABE ? 4 : 0) |
								(PRIM->FGE ? 8 : 0) | ((PRIM->TME && IsMipMapActive()) ? 16 : 0) |
								(PRIM->FST ? 32 : 0) | (ctx->TEST.ATE ? 64 : 0) | (ctx->TEST.DATE ? 128 : 0));
	row.tex_psm = static_cast<u8>(ctx->TEX0.PSM);
	row.sync_pages = static_cast<u16>(std::min<u32>(m_oracle.sync_pages, 0xFFFFu));
	row.fp_pages = static_cast<u16>(m_oracle.fp.count());

	// Whole-footprint byte residual, alongside the rect comparison. If the rect agrees
	// and this does not, an arm wrote OUTSIDE the rect it declared -- a different bug
	// from anything the pixel tallies can express, and one nothing else would notice.
	{
		size_t at = 0;
		m_oracle.fp.forEachSetPage([&](u32) {
			const u8* a = m_oracle.sw.bytes.data() + at;
			const u8* b = m_oracle.gpu.bytes.data() + at;
			const u8* c = m_oracle.raw.bytes.data() + at;
			const u8* p = m_oracle.pre.bytes.data() + at;
			u32 n = 0;
			for (u32 i = 0; i < GS_PAGE_SIZE; i++)
			{
				const bool differs = a[i] != b[i];
				const bool moved_by_readback = b[i] != c[i];
				const bool moved_by_sync = c[i] != p[i];
				n += differs ? 1u : 0u;
				row.rb_bytes += moved_by_readback ? 1u : 0u;
				row.rb_diff_bytes += (differs && moved_by_readback) ? 1u : 0u;
				row.sync_bytes += moved_by_sync ? 1u : 0u;
				row.sync_diff_bytes += (differs && moved_by_sync) ? 1u : 0u;
			}
			if (n != 0)
			{
				row.diff_pages++;
				row.diff_bytes += n;
			}
			at += GS_PAGE_SIZE;
		});
	}

	if (want_c)
	{
		row.colour = GSTileOracle::Compare(m_oracle.c_pre.data(), m_oracle.c_sw.data(), m_oracle.c_gpu.data(),
			r.width(), r.height(), r.x, r.y, GSTileOracle::MetricForPsm(fb_psm));
	}
	if (want_z)
	{
		row.depth = GSTileOracle::Compare(m_oracle.z_pre.data(), m_oracle.z_sw.data(), m_oracle.z_gpu.data(),
			r.width(), r.height(), r.x, r.y, GSTileOracle::Metric::Raw);
	}

	GSTileOracle::AddRow(row);

	if (!row.diverged())
		return;

	// Per-pixel detail for the first few divergent draws. Row order is causal order
	// here, so the first few are the ones attribution actually reads.
	u32 budget = GSTileOracle::OpenPixelDetail();
	if (budget == 0)
		return;

	const auto dump = [&](bool depth, const std::vector<u32>& pre, const std::vector<u32>& sw,
						  const std::vector<u32>& gpu) {
		if (pre.empty())
			return;
		size_t at = 0;
		for (int y = r.y; y < r.w && budget != 0; y++)
		{
			for (int x = r.x; x < r.z && budget != 0; x++, at++)
			{
				if (sw[at] == gpu[at])
					continue;
				GSTileOracle::AddPixel(row.frame, row.draw, depth, x, y, pre[at], sw[at], gpu[at]);
				budget--;
			}
		}
	};
	dump(false, m_oracle.c_pre, m_oracle.c_sw, m_oracle.c_gpu);
	dump(true, m_oracle.z_pre, m_oracle.z_sw, m_oracle.z_gpu);
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

	// A blend pass whose realization carries the As factor through the second
	// fragment output needs the device's dual-source unit (the Mali r44p1 class:
	// dualSrcBlend is a constant zero in the blob's .rodata, decomp-measured).
	// The lowering already refuses those legs when its dual_source_blend input is
	// false — the same Features() bit this checks — so this is a backstop against
	// the two ever disagreeing, not a live gate. The Classic-parity carrier
	// (TileBlendClassicCarrier) is the one admission that sets blend_src1: its
	// SRC1 transit quantizes the factor onto the /255 grid, bounded at one level
	// (the 2026-08-14 exhaustive walk) — the Classic-parity class, and exactly
	// what Classic ships through the same unit. The read rung and the no-Cd/
	// accumulation legs never set it: their carriers stay in shader arithmetic
	// and dual source is irrelevant to them.
	if (plan.pass[0].abe && plan.pass[0].blend_src1 && !g_gs_device->Features().dual_source_blend)
	{
		reason = GSTileFloorReason::Blend;
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
	// Coordinate transcription: every textured triangle and sprite takes its texture
	// coordinate off the SW scanline's WALK, replayed per fragment from the
	// primitive's own edge (PS_TILE_TWALK) — the same channel as the colour walk
	// below. Not the interpolator (the vertex shader's 1/320-pixel coverage nudge
	// translates the whole primitive and an interpolated attribute rides its own
	// gradient by the shift the rasterizer realizes), and not a plane at the pixel
	// index either: the scanline seeds each row at its span's first pixel and steps
	// in blocks of four — a truncating integer step under FST, an accumulating
	// float one under perspective — and a sprite seeds each row by adding the row
	// step once per row, so the value at a pixel depends on where its span and its
	// primitive began, which no plane evaluation reproduces. Measured before this
	// existed: a scaled bilinear blit (R&C, 416 rows into 448) came out 3,900 pixels
	// a sixteenth of a weight off per frame from the row accumulation alone.
	// The fast profile sheds a walk class for its GPU-native realization — the
	// interpolator's varyings and the fallback legs below, all live code — traded
	// under the perceptual gate rather than byte-identity. A want turned off here
	// also skips that class's payload construction, which is most of the walks'
	// CPU bill. Depth's lever is separate from the umbrella: TileFastDepthClassic
	// is the attribution control (Classic's float realization); the shipping fast
	// depth is the plane-exact integer leg, which lands as its own selector.
	const bool fast_colour = GSConfig.TileFastShading && !GSConfig.TileExactColour;
	const bool fast_tex = GSConfig.TileFastShading && !GSConfig.TileExactTexCoord;
	const bool want_zwalk = want_ds && m_vt.m_primclass == GS_TRIANGLE_CLASS && !m_vt.m_eq.z &&
		!GSConfig.TileFastDepthClassic;
	// Under the fast profile the walk stays only where it is closed-form (the
	// effective-FST integer DDA). What demotes is exactly the unbounded loop: the
	// perspective float walk, whose trip count is the fragment's distance from the
	// span's left edge. Mip STQ TRIANGLES also leave the walk under fast, for the
	// sampler leg's manual-LOD realization (Classic's own mip machinery) — which
	// deletes the walk legs' per-level in-shader filter AND the draw's payload.
	// True-FST mip draws and mip sprites keep the walk: their walk is the cheap
	// closed form, and the manual-LOD shader reads the Q varying, which an FST
	// draw carries as whatever the game last left in RGBAQ (log2 of a zero Q is
	// the NaN the STQ guard exists to floor — STQ draws reach the leg with Q
	// proven finite and positive, FST draws prove nothing). The twalk_mip clause
	// otherwise serves the walk-era unlock (TilePerspectiveNative admits mip STQ
	// triangles onto the walk under the exact profile).
	const bool twalk_mip = PRIM->TME && IsMipMapActive();
	const bool mip_fast =
		fast_tex && twalk_mip && m_vt.m_primclass == GS_TRIANGLE_CLASS && !PRIM->FST;
	const bool twalk_fst_eff = gsTileTexWalkFst(m_vt.m_primclass, PRIM->FST != 0,
		twalk_mip, m_vt.m_eq.q != 0);
	const bool want_twalk = PRIM->TME && !mip_fast &&
		(m_vt.m_primclass == GS_TRIANGLE_CLASS || m_vt.m_primclass == GS_SPRITE_CLASS) &&
		(twalk_fst_eff || twalk_mip || !fast_tex);
	// Colour and fog transcription: a gouraud or fogged triangle takes its colour
	// and fog factor off the SW scanline's blocked walk, replayed per fragment
	// from the primitive's own edge (PS_TILE_CWALK), because the walk's value at a
	// pixel depends on where its span started — which the interpolator cannot
	// know and gs-block measured silicon doing the same. Sprites are flat in
	// both (the second vertex's) and need nothing.
	const bool want_cwalk = m_vt.m_primclass == GS_TRIANGLE_CLASS && (!IsFlatShaded() || PRIM->FGE) &&
		!fast_colour;
	if (!BuildTilePayload(want_zwalk, want_twalk, want_cwalk, reason))
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
	GSVector4i feedback_core = GSVector4i::zero(); ///< admitted feedback draw's sampled core (empty otherwise)
	GSPageBitmap feedback_core_pages;
	bool feedback_subrect = false; ///< the core's sole owner serves as a subrect donor
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
			// The whole-WINDOW test above is the conservative one: the window is the
			// register claim, and a draw can share pages with its own target while
			// SAMPLING none of them — SotC's bloom downsample chain writes into the
			// tail rows of its own 512-page source window and samples everything
			// ABOVE them. The refined question is the sampled CORE: the vertex UV
			// bbox (which bounds every sample coordinate — s/q along an edge is
			// monotone, so vertex extrema bound the triangle) shrunk one texel per
			// side. Core pages disjoint from the write footprint means every tap
			// that could touch written bytes belongs to a sample within one texel
			// of the bbox boundary — the linear filter's second tap, a clamp fold,
			// or a one-texel repeat fold — an area-bounded band whose misread the
			// perceptual gate prices. That is a severity bound, not exactness, so
			// the admission is the fast profile's; exact keeps the floor.
			bool admit = false;
			if (GSConfig.TileFastShading && !GSConfig.TileExactFeedback && mip_levels == 1 &&
				ctx->CLAMP.WMS <= CLAMP_CLAMP && ctx->CLAMP.WMT <= CLAMP_CLAMP)
			{
				const int tw = 1 << std::min<u32>(fixed_tex0.TW, 10);
				const int th = 1 << std::min<u32>(fixed_tex0.TH, 10);
				// Inclusive core [floor(min)+1, floor(max)-1]; exclusive rect form.
				int cx0 = static_cast<int>(std::floor(m_vt.m_min.t.x)) + 1;
				int cy0 = static_cast<int>(std::floor(m_vt.m_min.t.y)) + 1;
				int cx1 = static_cast<int>(std::floor(m_vt.m_max.t.x));
				int cy1 = static_cast<int>(std::floor(m_vt.m_max.t.y));
				// CLAMP folds an overhanging core onto the edge texel (in-window, and
				// already margin territory); REPEAT folds it onto arbitrary interior
				// bytes the core test would not see, so any core overhang floors.
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
					const GSPageBitmap core =
						PagesForTargetRect(tex_l, GSVector4i(cx0, cy0, cx1, cy1));
					admit = !core.intersects(fb_pages) && !core.intersects(z_pages);
					if (admit)
					{
						feedback_core = GSVector4i(cx0, cy0, cx1, cy1);
						feedback_core_pages = core;
					}
				}
			}
			if (!admit)
			{
				reason = GSTileFloorReason::TextureFeedback;
				return false;
			}
			m_native_sync.feedback_admitted++;
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

	// Three unrelated reasons the native route pulls from the GPU, kept apart because
	// they have three different fixes: an upload whose source pages the GPU still owns,
	// a steal of destination pages from another surface (the aliasing tiers, M3e), and a
	// texture source sitting on pages some target owns (render-to-texture). The census
	// attributes each separately or the total says nothing about what to build.
	GSPageBitmap sync_upload = m_vram_model.ReadbackNeeded(up_fb | up_z, kGSTilePlanesAll);
	GSPageBitmap sync_steal;
	if (want_rt)
		sync_steal |= m_vram_model.SpillBeforeNativeDraw(fb_id, fb_pages, plan.fb_claims);
	if (plan.z_write)
		sync_steal |= m_vram_model.SpillBeforeNativeDraw(z_id, z_pages, plan.z_claims);
	// Render-to-texture, served on the device. When ONE surface holds GPU-newest truth
	// for the entire texture window under the window's own layout, that surface's texture
	// already IS the source and the bytes never need to visit CPU memory. The readback
	// census named this the largest single source of native-route stalls -- 142 of GT4's
	// 143, with no other reason present -- and a stall is a full GPU drain whatever it
	// carries, so removing the reason removes the whole cost, not a fraction of it.
	//
	// Every predicate is exact rather than approximate, because a wrong donor is silent
	// wrong output: one owner for every plane of every page at whole-page granularity;
	// the surface's layout equal to the window's, which by the target pool's
	// linear-from-base-page rule makes the two pixel spaces the SAME pixel space; a format
	// whose RGBA8 target bytes already are what the swizzle reader would have produced;
	// and a window that fits inside what the donor actually materializes. Anything else
	// takes the CPU route and pays. Feedback cannot reach here -- it floored above.
	GSPageBitmap sync_tex;
	if (textured)
		sync_tex = m_vram_model.ReadbackNeeded(tex_pages, kGSTilePlanesAll);

	GSTileTextureSource::Donor donor = {};
	const GSTileTextureSource::Donor* donor_p = nullptr;
	GSTexture* direct_tex = nullptr;
	int direct_fmt = -1;
	GSVector4i direct_params = GSVector4i::zero();
	if (!sync_tex.empty())
	{
		NativeSyncReasons& n = m_native_sync;
		const GSTileSurfaceId owner =
			mip_levels == 1 ? m_vram_model.SoleGpuOwner(tex_pages, kGSTilePlanesAll) : kGSTileNoSurface;
		if (mip_levels != 1)
			n.refuse_mip++;
		else if (owner == kGSTileNoSurface)
		{
			// The whole window has no single GPU owner. For an ADMITTED feedback
			// draw that is true by construction — the window spans the surface it
			// samples AND the target it writes — but the sampled CORE usually has
			// exactly one owner (SotC's bloom: the scene RT). Serving THAT as a
			// subrect donor deletes the CPU route's mid-frame pull of a target
			// rendered moments earlier, which the M2 re-price showed was the whole
			// cost of the admission (+16-21% frame, two thirds of it stall). A
			// CT24 window over the CT32 owner copies RGB and bakes TA0 into alpha
			// (AEM off, proven here) — the same bytes the CPU deswizzle produces.
			bool served = false;
			if (!feedback_core.rempty())
			{
				const GSTileSurfaceId co = m_vram_model.SoleGpuOwner(feedback_core_pages, kGSTilePlanesAll);
				if (co != kGSTileNoSurface && co != fb_id && co != z_id)
				{
					const GSVramModel::Surface& cs = m_vram_model.Get(co);
					GSTexture* ctex = cs.pool_handle ? m_target_pool.GetTexture(cs.pool_handle) : nullptr;
					const bool psm_ok =
						(fixed_tex0.PSM == PSMCT32 && cs.layout.psm == PSMCT32) ||
						(fixed_tex0.PSM == PSMCT24 && (cs.layout.psm == PSMCT32 || cs.layout.psm == PSMCT24));
					const bool base_ok = cs.layout.bp == fixed_tex0.TBP0 &&
										 cs.layout.bw == static_cast<u8>(fixed_tex0.TBW) &&
										 cs.layout.kind == GSTileSurfaceKind::Color;
					const bool aem_ok = fixed_tex0.PSM != PSMCT24 || m_draw_env->TEXA.AEM == 0;
					if (ctex && psm_ok && base_ok && aem_ok && cs.residency.contains(feedback_core_pages) &&
						feedback_core.z <= ctex->GetWidth() && feedback_core.w <= ctex->GetHeight())
					{
						donor.tex = ctex;
						donor.width = ctex->GetWidth();
						donor.height = ctex->GetHeight();
						donor.copy_rect = feedback_core;
						donor.fill_alpha =
							(fixed_tex0.PSM == PSMCT24) ? static_cast<int>(m_draw_env->TEXA.TA0) : -1;
						donor_p = &donor;
						feedback_subrect = true;
						n.tex_core_served++;
						sync_tex = GSPageBitmap();
						served = true;
					}
				}
			}
			if (!served)
				n.refuse_no_owner++;
		}
		else if (owner == fb_id || owner == z_id)
		{
			// Only reachable for an ADMITTED feedback draw (core-disjoint window
			// containing its own target): serving the target as its own source
			// while rendering into it is a same-image sample/render hazard. The
			// CPU route below spills and rebuilds instead — correct, just paid.
			n.refuse_self_target++;
		}
		else
		{
			const GSVramModel::Surface& s = m_vram_model.Get(owner);
			const GSTileSurfaceLayout want{fixed_tex0.TBP0, static_cast<u8>(fixed_tex0.TBW),
				static_cast<u8>(fixed_tex0.PSM), KindForPsm(fixed_tex0.PSM)};
			GSTexture* dtex = s.pool_handle ? m_target_pool.GetTexture(s.pool_handle) : nullptr;
			const int dw = dtex ? dtex->GetWidth() : 0;
			const int dh = dtex ? dtex->GetHeight() : 0;
			const int tw = 1 << std::min<u32>(fixed_tex0.TW, 10);
			const int th = 1 << std::min<u32>(fixed_tex0.TH, 10);
			// A palettised window over a colour target: the game is reading the target's
			// BYTES as indices (GT4 tone-maps its frame this way, seventy pages a frame).
			// The bytes are in the owner's texture in the CT32 arrangement; the device
			// re-arranges them into an index texture through the GS swizzle
			// (GSTileSwizzleForms, tile_convert.glsl) — an offset into the owner and a
			// different pitch are just address arithmetic there. Exactness rests on the
			// owner holding EVERY plane of every page of the window (SoleGpuOwner over all
			// planes, so the alpha byte is the owner's too), whole-page truth, and the
			// window's own layout being expressible: pages-per-row as GSOffset computes it.
			const int reinterpret_fmt = GSTileSwizzleForms::IndexFormatFor(fixed_tex0.PSM);
			const bool owner_is_ct32 = s.layout.kind == GSTileSurfaceKind::Color &&
									   (s.layout.psm == PSMCT32 || s.layout.psm == PSMCT24);
			if (fixed_tex0.PSM == PSMCT32 && s.layout != want)
				n.refuse_layout++;
			else if (fixed_tex0.PSM != PSMCT32 && (reinterpret_fmt < 0 || !owner_is_ct32 || !m_reinterpret_serves))
			{
				n.refuse_format++;
				// Does the window at least START where the owner does? A reinterpretation
				// anchored at the owner's own base is a change of arrangement over bytes
				// already in hand; one at an arbitrary offset into it is a different and
				// much harder problem. Measured because the refusal order above means the
				// layout column cannot answer it -- format short-circuits first, so
				// "layout 0" says nothing whatever about the palettised population.
				if (s.layout.bp == fixed_tex0.TBP0)
					n.refuse_format_same_base++;
			}
			else if (!dtex || !s.residency.contains(tex_pages) ||
					 (fixed_tex0.PSM == PSMCT32 && (tw > dw || th > dh)))
				n.refuse_geometry++;
			else if (fixed_tex0.PSM != PSMCT32)
			{
				// Two ways to serve it, both exact. DIRECT: the draw samples the owner
				// texture itself and does the address arithmetic per fetch in its fragment
				// shader — no build, no pass of its own — whenever the owner is not the
				// draw's own colour or depth target (that would be a feedback loop, and
				// the pool texture is what the pass is writing). Otherwise the device
				// re-arranges the window into an index texture in a pass of its own.
				const GSOffset src_off = m_mem.GetOffset(fixed_tex0.TBP0, fixed_tex0.TBW, fixed_tex0.PSM);
				const u32 src_bwpg = static_cast<u32>(src_off.pageRowWidth() >> src_off.pageShiftX());
				// The direct leg is fetch-by-address in the tile texture legs, which
				// only exist on a draw carrying the coordinate walk — a fast-profile
				// draw demoted to the sampler takes the reinterpretation pass instead.
				if (owner != fb_id && owner != z_id && DirectSamplingServes() && m_twalk_at != NoWalk)
				{
					direct_tex = dtex;
					direct_fmt = reinterpret_fmt;
					direct_params = GSVector4i(static_cast<int>(fixed_tex0.TBP0), static_cast<int>(src_bwpg),
						static_cast<int>(s.layout.bp), static_cast<int>(s.layout.bw));
					n.tex_direct++;
				}
				else
				{
					donor.tex = dtex;
					donor.width = dw;
					donor.height = dh;
					donor.reinterpret = true;
					donor.params.src_bp = fixed_tex0.TBP0;
					donor.params.src_bwpg = src_bwpg;
					donor.params.dst_bp = s.layout.bp;
					donor.params.dst_bwpg = s.layout.bw;
					donor.params.fmt = static_cast<u32>(reinterpret_fmt);
					donor_p = &donor;
					n.tex_reinterpreted++;
				}
				n.tex_served++;
				sync_tex = GSPageBitmap();
			}
			else
			{
				donor.tex = dtex;
				donor.width = dw;
				donor.height = dh;
				donor_p = &donor;
				n.tex_served++;
				sync_tex = GSPageBitmap();
			}
		}
	}

	// Attribute each page to exactly one reason so the columns sum to the total instead
	// of triple-counting a page that all three want. The SOLE column is the decisive
	// one: a reason that is never the only one present cannot remove a single stall by
	// being fixed, however many pages it names -- the same logic the floor-reason census
	// uses, and the reason M3c's alpha test measured zero coverage.
	// Ledger-only work: the draw itself consumes the UNION below, which the andnot
	// attribution cannot change, and the per-reason ledger flags further down sit
	// behind the same gate, so they see the attributed bitmaps whenever they look.
	if (GSDrawLog::IsActive()) [[unlikely]]
	{
		sync_steal = sync_steal.andnot(sync_upload);
		sync_tex = sync_tex.andnot(sync_upload | sync_steal);
		NativeSyncReasons& n = m_native_sync;
		n.upload_pages += static_cast<u32>(sync_upload.count());
		n.steal_pages += static_cast<u32>(sync_steal.count());
		n.tex_pages += static_cast<u32>(sync_tex.count());
		const bool u = !sync_upload.empty(), s = !sync_steal.empty(), t = !sync_tex.empty();
		if (u && !s && !t)
			n.upload_sole++;
		if (s && !u && !t)
			n.steal_sole++;
		if (t && !u && !s)
			n.tex_sole++;
	}

	const GSPageBitmap sync_set = sync_upload | sync_steal | sync_tex;

	// Per-draw attribution into the ledger, alongside the teardown census. The census
	// totals the bill; these columns say WHICH draw paid it and for which reason, which
	// is what turns "this title stalls a lot" into a list of draw numbers.
	//
	// Stalls counted off the pool's own drain counter rather than off this call,
	// because a readback whose pages collect to no runs returns without touching the
	// device: a call is not a stall. Pages are noted whether or not anything drained —
	// a quiescent pull still pays its copy, and an all-zero column on a spilling title
	// once read as a dead hook when it was really this gate.
	const u32 drains_before = m_readback[static_cast<u32>(ReadbackSite::NativeDraw)].drains;
	const bool readback_ok = ReadbackModelPages(sync_set, ReadbackSite::NativeDraw);
	if (GSDrawLog::IsActive())
	{
		const u32 drains = m_readback[static_cast<u32>(ReadbackSite::NativeDraw)].drains - drains_before;
		const auto reasons = static_cast<GSDrawLog::TileSyncReason>(
			(sync_upload.empty() ? 0 : GSDrawLog::TileSyncUploadSource) |
			(sync_steal.empty() ? 0 : GSDrawLog::TileSyncStealSurface) |
			(sync_tex.empty() ? 0 : GSDrawLog::TileSyncTextureOnTarget));
		GSDrawLog::NoteTileSync(static_cast<u32>(sync_set.count()), drains, reasons);
	}

	// A device palette still living only in an owner texture must be gathered before
	// anything overwrites its source: the uploads below and this draw's own rendering
	// are the only two writers of a pool texture.
	MaterializePalettesOn(up_fb | up_z | fb_pages | (plan.z_write ? z_pages : GSPageBitmap()));

	if (!readback_ok ||
		(want_rt && !m_target_pool.UploadPages(m_mem, fb_handle, fb_l, up_fb)) ||
		(want_ds && !m_target_pool.UploadPages(m_mem, z_handle, z_l, up_z)))
	{
		// Nothing has been claimed yet, so the model is consistent and the floor
		// path (which spills again from the model's state) remains correct.
		reason = GSTileFloorReason::ResourceFailure;
		return false;
	}

	// The texture source, built from (now-current) CPU local memory: RGBA8 with the
	// TEXA expansion applied by the readers for a direct-colour window, an index
	// texture for a palettised one. The palette then rides beside it as its own
	// device texture, expanded from the CLUT read buffer exactly as the SW
	// rasterizer's would be — Read32 refreshes it, GSTilePaletteCache keys it on the
	// words. The shader expands each fetched index through it before any filtering,
	// the order the gs-idxfilt console capture measured. (Standing M2 gap: the palette
	// itself was loaded from CPU bytes at TEX0-write time, before any draw-side seam
	// ran — a GPU-rendered palette would be read stale; the oracle gate is the
	// detector.)
	GSTexture* tex = nullptr;
	GSTexture* pal = nullptr;
	bool pal_direct = false;
	GSVector4i pal_direct_params = GSVector4i::zero();
	u64 tex_build_id = 0;
	u64 pal_content_id = 0;
	if (textured)
	{
		const u32 pal_entries = GSLocalMemory::m_psm[ctx->TEX0.PSM].pal;
		if (pal_entries > 0)
		{
			// A palette the device loaded off a rendered page is bound as it is — the
			// owner texture itself, sampled through the CLUT's word order, where its
			// source is intact and is not this draw's target; the gathered texture
			// otherwise. The ledger's palette column stays empty for those (the words
			// never visit the CPU). Else the CPU CLUT — synced first where its slots
			// straddled loads.
			const PaletteChoice choice = ResolvePalette(ctx->TEX0, true, want_rt ? fb_id : kGSTileNoSurface,
				want_ds ? z_id : kGSTileNoSurface, m_twalk_at != NoWalk);
			if (choice.gpu)
			{
				pal = choice.gpu;
				pal_direct = choice.direct;
				pal_direct_params = choice.direct_params;
				m_clut_census.gpu_palette_draws++;
				if (pal_direct)
					m_clut_census.direct_palette_draws++;
			}
			else
			{
				m_mem.m_clut.Read32(ctx->TEX0, m_draw_env->TEXA);
				const u32* clut = m_mem.m_clut;
				pal = m_palette_cache.Lookup(clut, pal_entries, m_mem.m_clut.GetReadGeneration(), &pal_content_id);
				if (!pal)
				{
					reason = GSTileFloorReason::ResourceFailure;
					return false;
				}
				// The ledger's palette column, which only the floor's SW arm had been
				// filling — a native palettised draw read as "no palette" until now.
				if (GSDrawLog::IsActive()) [[unlikely]]
					GSDrawLog::NoteClut(clut, pal_entries);
			}
		}
		if (direct_tex)
		{
			tex = direct_tex;
		}
		else
		{
			tex = m_tex_source.Lookup(m_mem, m_vram_model, fixed_tex0, m_draw_env->TEXA,
				feedback_subrect ? feedback_core_pages : tex_pages, level_tex0, mip_levels, donor_p, &tex_build_id,
				feedback_core.rempty() ? nullptr : &feedback_core);
			if (!tex)
			{
				// A device that cannot reinterpret (no pipeline, or not Vulkan) says so once;
				// from here on the window takes the CPU route like any other refused format,
				// and only this draw floors.
				if (donor_p && donor.reinterpret)
					m_reinterpret_serves = false;
				reason = GSTileFloorReason::ResourceFailure;
				return false;
			}
		}

		// The fast profile's palettised draws on the GPU sampler leg swap the
		// (index, palette) pair for the device-expanded RGBA texture
		// (GSTileExpandedCache): value-preserving — an expanded texel is exactly
		// the palette texel the in-shader expansion fetches — so the trade is
		// fetch count (four taps instead of four index taps plus four palette
		// fetches), never values. The walk and DIRECT legs keep the pair (their
		// integer filter expands corners itself, in the console's own order), and
		// GPU/direct palettes stay in-shader everywhere: their words never
		// transit the CPU cache, so they carry no content id to key an expansion
		// on. A mip pyramid expands every level through the same palette (the
		// device op fills levels above zero through its scratch), so the
		// manual-LOD draw samples an ordinary RGBA chain. A failed or refused
		// expansion keeps the pair bound — the in-shader path is always correct.
		if (pal && tex && fast_tex && m_twalk_at == NoWalk && pal_content_id != 0 && tex_build_id != 0 &&
			m_expand_cache.Serves())
		{
			if (GSTexture* expanded = m_expand_cache.Lookup(tex, tex_build_id, pal, pal_content_id, mip_levels))
			{
				tex = expanded;
				pal = nullptr;
			}
		}
	}

	SubmitNativeDraw(plan, r, fixed_tex0,
		want_rt ? m_target_pool.GetTexture(fb_handle) : nullptr,
		want_ds ? m_target_pool.GetTexture(z_handle) : nullptr,
		tex, pal, direct_fmt, direct_params, pal_direct, pal_direct_params);

	if (want_rt)
		m_vram_model.OnNativeDraw(fb_id, fb_pages, plan.fb_claims);
	if (plan.z_write)
		m_vram_model.OnNativeDraw(z_id, z_pages, plan.z_claims);

	// The draw was just recorded into the device's current submission; the pages it
	// wrote wait on that epoch. Read after SubmitNativeDraw, which may have rotated the
	// command buffer (a mid-draw execute), so the epoch is the buffer the draw is IN.
	{
		const u64 epoch = g_gs_device->GetSubmitEpoch();
		if (want_rt)
			m_vram_model.StampGpuWrite(fb_pages, epoch);
		if (plan.z_write)
			m_vram_model.StampGpuWrite(z_pages, epoch);
	}

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
// blocks (PS_TILE_ZWALK), the texture-coordinate walk's blocks (PS_TILE_TWALK) and
// the colour/fog walk's blocks (PS_TILE_CWALK), concatenated into ONE upload — two
// reservations against the streaming buffer can hit a mid-draw flush between them and
// strand the first one's offset.
//
// Every value comes out of the software renderer's own machinery: the vertices are
// converted by GSVertexSW::s_cvb (the converter GSRendererSW::Draw calls, with its
// q_div), the bilinear half-texel comes off the vertices where GetScanlineGlobalData
// takes it off, and every triangle value comes out of GSRasterizer's own compiled
// setup (GSComputeTriangleZPlane), so it carries the software renderer's exact
// roundings: the fp32 barycentric division, the compiler's fused multiply-adds, the
// 2^-10 truncation of the scan gradient, and the fp32 narrowing the scanline's setup
// applies to the lane-offset gradient. Loading the texture numerators, the colour and
// the fog into the same setup takes their gradients off that one division rather than
// a second, independently-rounded one; it cannot disturb the depth values, which are
// computed from other lanes. Sprites go through GSTileTexWalk.h's transcription of
// GSRasterizer::DrawSprite.
//
// Returns false with `reason` set for draws outside an envelope; those floor.
bool GSRendererTile::BuildTilePayload(bool want_zwalk, bool want_twalk, bool want_cwalk, GSTileFloorReason& reason)
{
	m_tile_payload.clear();
	m_zwalk_at = NoWalk;
	m_zwalk_plane = false;
	m_twalk_at = NoWalk;
	m_cwalk_at = NoWalk;
	m_twalk_fst = false;
	if (!want_zwalk && !want_twalk && !want_cwalk)
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
	const u32 primclass = m_vt.m_primclass;
	const bool mip = IsMipMapActive();

	// The draw's vertices exactly as GSRendererSW::Draw converts them — same
	// converter, same q_div — and then the half-texel pre-shift GetScanlineGlobalData
	// applies to an affine bilinear draw. Everything below seeds from these.
	const u32 q_div = gsTileTexWalkQDiv(primclass, mip, m_vt.m_eq.q, m_vt.m_min.t.z);
	GSVertexSW* sw = SwVerts(m_vertex->next);
	if (!envelope(sw != nullptr))
		return false;
	GSVertexSW::s_cvb[primclass][PRIM->TME][PRIM->FST][q_div](ctx, sw, m_vertex->buff, m_vertex->next);
	const bool fst_eff = gsTileTexWalkFst(primclass, PRIM->FST, mip, m_vt.m_eq.q);
	if (want_twalk && gsTileTexWalkPreshift(fst_eff, mip, m_vt.IsLinear()))
	{
		for (u32 i = 0; i < m_vertex->next; i++)
			gsTileTexWalkApplyPreshift(sw[i]);
	}
	m_twalk_fst = fst_eff;

	if (primclass == GS_SPRITE_CLASS)
	{
		// A sprite carries no depth gradient and no colour or fog gradient (both are
		// flat, from its second vertex), so the coordinate walk is the only block
		// here: DrawSprite's seed at the span's first pixel and first row, its per-pixel
		// and per-row steps, and the scissored left/top the walk counts from.
		pxAssert(!want_cwalk);
		if (!want_twalk)
			return true;
		const u32 nprims = m_index->tail / 2;
		m_tile_payload.assign(static_cast<size_t>(nprims) * 8, 0u);
		m_twalk_at = 0;

		u32* out = m_tile_payload.data();
		for (u32 p = 0; p < nprims; p++)
		{
			const GSVertexSW& v0 = sw[m_index->buff[p * 2 + 0]];
			const GSVertexSW& v1 = sw[m_index->buff[p * 2 + 1]];
			GSTileTexWalkSprite spr;
			// A sprite the rasterizer would not draw covers no fragment on the GPU
			// either (the coverage capture certified the conversion), so a zeroed
			// block is never read.
			if (!gsTileTexWalkSprite(v0, v1, ctx->scissor.in, spr))
				continue;
			const float blk[8] = {spr.s0, spr.t0, spr.q, 0.0f, spr.dtx, spr.dty, 0.0f, 0.0f};
			std::memcpy(out + static_cast<size_t>(p) * 8, blk, sizeof(blk));
			// The fast profile takes the fused closed form on every sprite row: the
			// row-replay loop's trip count is the fragment's row distance from the
			// sprite's top (a 448-row scaled blit replays 448 float adds per
			// fragment), and the closed form differs from it only by the measured
			// sixteenth-of-a-weight accumulation class — the perceptual gate's to
			// judge, not byte-identity's.
			const bool rows_closed = spr.rows_exact ||
				(GSConfig.TileFastShading && !GSConfig.TileExactTexCoord);
			const u32 flags = rows_closed ? 1u : 0u;
			out[p * 8 + 3] = flags;
			out[p * 8 + 6] = static_cast<u32>(spr.left);
			out[p * 8 + 7] = static_cast<u32>(spr.top);
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
	// The fast profile's depth contract is the closed plane form: 12 words per
	// primitive against the walk's 20, and ~15 fragment ALU against the
	// soft-float kernel's thousands, within one unit at comparator ties.
	const bool zplane = want_zwalk && GSConfig.TileFastDepthPlane;
	const size_t z_prim_words = zplane ? 12 : 20;
	const size_t zwords = want_zwalk ? (4 + static_cast<size_t>(nprims) * z_prim_words) : 0;
	const size_t twords = want_twalk ? (4 + static_cast<size_t>(nprims) * 24) : 0;
	const size_t cwords = want_cwalk ? (4 + static_cast<size_t>(nprims) * 32) : 0;
	m_tile_payload.assign(zwords + twords + cwords, 0u);
	if (want_zwalk)
	{
		m_zwalk_at = 0;
		m_zwalk_plane = zplane;
	}
	if (want_twalk)
		m_twalk_at = static_cast<u32>(zwords / 4);
	if (want_cwalk)
		m_cwalk_at = static_cast<u32>((zwords + twords) / 4);

	u32* zout = m_tile_payload.data();
	u32* tout = m_tile_payload.data() + zwords;
	u32* cout = m_tile_payload.data() + zwords + twords;
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

	g_perfmon.Put(GSPerfMon::TilePayloadBytes, static_cast<double>(m_tile_payload.size() * 4));

	if (want_zwalk)
	{
		// Header.
		push_f32(fscissor.x);
		push_u32((zmax > fmt_max) ? 1u : 0u); // sel.zclamp, the scanline's own gate
		push_u32(fmt_max);
		push_u32(0u);
	}
	// The texture and colour walks' headers: the scissor's left, which bounds the
	// span's first pixel.
	if (want_twalk)
	{
		std::memcpy(tout, &fscissor.x, 4);
		tout += 4;
	}
	if (want_cwalk)
	{
		std::memcpy(cout, &fscissor.x, 4);
		cout += 4;
	}

	for (u32 p = 0; p < nprims; p++)
	{
		const GSVertexSW tri[3] = {sw[m_index->buff[p * 3 + 0]], sw[m_index->buff[p * 3 + 1]], sw[m_index->buff[p * 3 + 2]]};

		// A primitive the rasterizer would not draw (degenerate y-order or zero
		// cross product) covers no fragments on the GPU either — the coverage
		// capture certified the conversion — so a zeroed block is never read.
		GSTileZPlane zp;
		if (!GSComputeTriangleZPlane(tri, fscissor_y, zp) || zp.nsections == 0)
		{
			zc += want_zwalk ? z_prim_words : 0;
			tout += want_twalk ? 24 : 0;
			cout += want_cwalk ? 32 : 0;
			continue;
		}

		// Per section: the edge (the span's first pixel on a row is a fused
		// multiply-add and a ceiling away, exactly as the depth walk finds it) and
		// the seeds, which are the section's own vertex values; the gradients once,
		// shared by both sections as SetupTriangle hands them out. A single-section
		// primitive repeats section 0 into the second slot and marks the boundary
		// unreachable, as the depth walk's payload does.
		const GSTileZPlaneSection& c0 = zp.sec[0];
		const GSTileZPlaneSection& c1 = zp.sec[(zp.nsections == 2) ? 1 : 0];
		const u32 boundary = (zp.nsections == 2) ? static_cast<u32>(c1.top) & 0xFFFFu : 0xFFFFu;

		if (want_twalk)
		{
			const float blk[24] = {
				c0.edge_x, c0.dedge_x, c0.p0x, c0.p0y,
				c1.edge_x, c1.dedge_x, c1.p0x, c1.p0y,
				c0.tseed.x, c0.tseed.y, c0.tseed.z, 0.0f,
				c1.tseed.x, c1.tseed.y, c1.tseed.z, 0.0f,
				zp.dedge_t.x, zp.dedge_t.y, zp.dedge_t.z, 0.0f,
				zp.dscan_t.x, zp.dscan_t.y, zp.dscan_t.z, 0.0f};
			std::memcpy(tout, blk, sizeof(blk));
			std::memcpy(tout + 11, &boundary, 4);
			tout += 24;
		}

		if (want_cwalk)
		{
			const float blk[32] = {
				c0.edge_x, c0.dedge_x, c0.p0x, c0.p0y,
				c1.edge_x, c1.dedge_x, c1.p0x, c1.p0y,
				c0.cseed.x, c0.cseed.y, c0.cseed.z, c0.cseed.w,
				c1.cseed.x, c1.cseed.y, c1.cseed.z, c1.cseed.w,
				zp.dedge_c.x, zp.dedge_c.y, zp.dedge_c.z, zp.dedge_c.w,
				zp.dscan_c.x, zp.dscan_c.y, zp.dscan_c.z, zp.dscan_c.w,
				c0.fseed, c1.fseed, zp.dedge_f, zp.dscan_f,
				0.0f, 0.0f, 0.0f, 0.0f};
			std::memcpy(cout, blk, sizeof(blk));
			std::memcpy(cout + 28, &boundary, 4);
			cout += 32;
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
		const s32 top_clamped = std::clamp(zp.top_prim, -32768, 32767);
		const u32 rows_word = boundary | (static_cast<u32>(static_cast<u16>(top_clamped)) << 16);

		if (zplane)
		{
			// The walk collapsed to one plane per section (the shader block's
			// derivation): Zc anchored at the section's own top row in double, each
			// quantity split floor-wise into a mod-2^32 integer part and a
			// nonnegative fp32 fraction. The row gradient at constant x is dedge_z
			// ITSELF — the walk re-measures its prestep from the reference vertex
			// every row, so the edge's slope cancels out of z entirely (the mirror
			// suite's walk-comparison test is what pinned this). Anchoring per
			// section keeps the ≤2-unit inter-section residue the truncated
			// gradients leave, exactly as the scanline carries it.
			const double S = zp.dscan_z;
			const double si_d = std::floor(S);
			u32 zci[2], gyi[2], tops[2];
			float zcf[2], gyf[2];
			u32 biasbits = 0;
			const GSTileZPlaneSection* secs[2] = {&s0, &s1};
			for (int n = 0; n < 2; n++)
			{
				const GSTileZPlaneSection& s = *secs[n];
				const double gy = s.dedge_z;
				const int top = std::clamp(s.top, 0, 4095);
				const double zc_d = s.zseed +
					(static_cast<double>(top) - static_cast<double>(s.p0y)) * gy -
					static_cast<double>(s.p0x) * S;
				const double gyi_d = std::floor(gy);
				const double zci_d = std::floor(zc_d);
				gyi[n] = static_cast<u32>(static_cast<s64>(gyi_d));
				zci[n] = static_cast<u32>(static_cast<s64>(zci_d));
				gyf[n] = static_cast<float>(gy - gyi_d);
				zcf[n] = static_cast<float>(zc_d - zci_d);
				tops[n] = static_cast<u32>(top);
				if (s.dedge_z != 0.0)
					biasbits |= (1u | ((s.dedge_z < 0.0) ? 2u : 0u)) << (n * 2);
			}
			push_u32(zci[0]);
			push_f32(zcf[0]);
			push_u32(zci[1]);
			push_f32(zcf[1]);
			push_u32(gyi[0]);
			push_f32(gyf[0]);
			push_u32(gyi[1]);
			push_f32(gyf[1]);
			push_u32(static_cast<u32>(static_cast<s64>(si_d)));
			push_f32(static_cast<float>(S - si_d));
			push_u32(rows_word);
			push_u32(tops[0] | (tops[1] << 12) | (biasbits << 24));
			continue;
		}

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
	GSTexture* rt, GSTexture* ds, GSTexture* tex, GSTexture* pal, int direct_idx, const GSVector4i& direct_idx_params,
	bool pal_direct, const GSVector4i& pal_direct_params)
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
	// The vertex colour is TRUNCATED to the eight bits the GS stores before any
	// observable use — the texture-function multiply included (gs-shade console
	// capture; GSStoredVertexColor in GSDrawScanline.cpp is the SW half). Without
	// this the GPU keeps the interpolator's full precision and the UNORM8 store
	// ROUNDS, which is half a level of systematic bias on every Gouraud gradient —
	// first measured against gs-interp, then sharpened by gs-shade to the stored
	// byte. Flat draws are unaffected: their colour is already an integer, so the
	// truncation is identity.
	conf.ps.tile_vcolor = 1;
	conf.ps.dst_fmt = GSLocalMemory::m_psm[ctx->FRAME.PSM].fmt;
	conf.colormask = GSHWDrawConfig::ColorMaskSelector(plan.pass[0].colormask);
	if (!rt)
		conf.ps.DisableColorOutput();

	// The blend realization (M4b rung 3): the lowering admitted these selectors as
	// pure fixed-function, and the donor's static map supplies the op/factor
	// encoding. C=As rides the second fragment output — the shader's existing
	// alpha_blend, which is the texture function's alpha BEFORE the FBA
	// correction, exactly the operand the console blends with (gs-alpha2) — and
	// C=FIX rides the blend constant, which the backend scales by 128. Alpha is
	// never blended on the GS, so the alpha channel passes the source through.
	// This runs before the second pass copies conf.ps, so a split draw's passes
	// agree about the second colour output.
	if (rt && plan.pass[0].abe)
	{
		const GSTileDrawPass& bp = plan.pass[0];
		const u32 blend_index =
			((static_cast<u32>(bp.blend_a) * 3 + bp.blend_b) * 3 + bp.blend_c) * 3 + bp.blend_d;
		const HWBlend blend = GSDevice::GetBlend(blend_index);
		if (bp.blend_leg == GSTileBlendLeg::Read || bp.blend_leg == GSTileBlendLeg::InShaderNoRec)
		{
			// Rung 9 — the read rung — and its free twin. No blend unit is engaged
			// at all: the fragment shader is handed the register's own selectors and
			// evaluates the console equation in integer arithmetic, then writes the
			// finished pixel. On the read rung the destination comes from the
			// backend (its feedback-loop predicate reads the same blend_a/b/c/d), as
			// a texture barrier where the device has honest ones and a copy of the
			// draw area where it does not — one of each per draw, which is why the
			// lowering admits only draws whose own primitives cannot overlap.
			//
			// InShaderNoRec is the same integer evaluation for equations that
			// reference no Cd anywhere: the shader's destination predicate is
			// selector-derived (PS_FEEDBACK_LOOP_IS_NEEDED_RT), so nothing is read,
			// no barrier or copy is ordered, and overlap is immaterial — each
			// fragment stands alone. Byte-exact on any device, like the read rung.
			//
			// Every carrier rides raw, Ad included: the shader multiplies by the alpha
			// BYTE and shifts by seven, so the /128 scale is exact where the blend
			// unit's DST_ALPHA would divide by 255.
			conf.ps.blend_a = bp.blend_a;
			conf.ps.blend_b = bp.blend_b;
			conf.ps.blend_c = bp.blend_c;
			conf.ps.blend_d = bp.blend_d;
			conf.ps.tile_blend = 1;
			conf.cb_ps.TA_MaxDepth_Af.a = static_cast<float>(bp.afix) / 128.0f;
			conf.require_one_barrier = bp.blend_leg == GSTileBlendLeg::Read;
		}
		else if (bp.blend_leg == GSTileBlendLeg::Accumulation)
		{
			// The accumulation realization, transcribed from the donor (BLEND_ACCU):
			// the shader computes the whole (A−B)·C term — the admitted shapes are
			// Cs·C ± Cd, so after removing Cd the term is always (Cs−0)·C, computed
			// positive and truncated in the shader (ps.blend_mix stays 0, which is
			// the trunc leg of tfx.glsl) — and the ROP adds it to Cd, or reverse-
			// subtracts it from Cd, at factor ONE on both sides. Integer plus
			// integer at the ROP, so the composition is exact up to the ROP's own
			// UNORM rounding (the #131 Adreno class), and no SRC1 output is
			// involved on any device.
			conf.ps.blend_a = 0;
			conf.ps.blend_b = 2;
			conf.ps.blend_c = bp.blend_c;
			conf.ps.blend_d = 2;
			conf.cb_ps.TA_MaxDepth_Af.a = static_cast<float>(bp.afix) / 128.0f;
			conf.blend = {true, GSDevice::CONST_ONE, GSDevice::CONST_ONE, blend.op,
				GSDevice::CONST_ONE, GSDevice::CONST_ZERO, bp.blend_c == 2, bp.afix};
		}
		else if (bp.blend_leg == GSTileBlendLeg::HwRewrite)
		{
			// The donor's BLEND_HW2 shape, Cd·Alpha: the shader splits the 0..2
			// factor across its two outputs — the overflow part max(0, Alpha−1)
			// rides the primary colour (tfx.glsl's PS_BLEND_HW == 2 leg) and the
			// ≤1 part rides SRC1 (or the blend constant for FIX) — and the map's
			// DST_COLOR/SRC1_COLOR factors compose the product at the ROP. The
			// shader's own blend selectors stay zero: this is not a shader blend,
			// just an output rewrite.
			conf.ps.blend_a = 0;
			conf.ps.blend_b = 0;
			conf.ps.blend_c = bp.blend_c;
			conf.ps.blend_d = 0;
			conf.ps.blend_hw = bp.blend_hw;
			conf.cb_ps.TA_MaxDepth_Af.a = static_cast<float>(bp.afix) / 128.0f;
			conf.blend = {true, blend.src, blend.dst, blend.op,
				GSDevice::CONST_ONE, GSDevice::CONST_ZERO, bp.blend_c == 2, bp.afix};
			conf.ps.no_color1 = !GSDevice::IsDualSourceBlendFactor(blend.src) &&
			                    !GSDevice::IsDualSourceBlendFactor(blend.dst);
		}
		else if (bp.blend_mix)
		{
			// The mix realization, transcribed from the donor: the shader computes
			// the Cs-side term — (A−B)·C + D with the selectors rewritten so only
			// the source operand survives, biased by the round-to-floor offset
			// (ps.blend_mix 1 for the add/subtract ops, 2 for reverse subtract,
			// tfx.glsl) — and the ROP adds the Cd term through the map's dst
			// factor at src ONE. MIX3 is the reverse lerp: the shader term is
			// Cs·(1−C).
			//
			// The carrier decides what C is. The exact-mix rung lands a proven
			// constant as FIX: the shader reads it from Af and the ROP from the
			// blend constant, both at full float precision, and with that rung's
			// saturation guard the whole composition is EXACT on a full-precision
			// blend unit. The Classic-parity carrier lands a genuinely variable As
			// raw (blend_c == 0): the shader term uses its own fragment's alpha and
			// the ROP's factor arrives through the second output (SRC1), which
			// transits the /255 grid — the within-one-level Classic-parity class,
			// identical to what Classic ships through the same unit. Either way
			// tile_blend_mix selects the exact-floor 127/256 offset over Classic's
			// reduced-precision-ROP compromise; the gs-blend probe under tile is
			// the per-device instrument.
			const bool mix3 = (blend.flags & BLEND_MIX3) != 0;
			conf.ps.blend_a = mix3 ? 2 : 0;
			conf.ps.blend_b = mix3 ? 0 : 2;
			conf.ps.blend_c = bp.blend_c;
			conf.ps.blend_d = mix3 ? 0 : 2;
			conf.ps.blend_mix = (blend.op == GSDevice::OP_REV_SUBTRACT) ? 2 : 1;
			conf.ps.tile_blend_mix = 1;
			conf.cb_ps.TA_MaxDepth_Af.a = static_cast<float>(bp.afix) / 128.0f;
			u8 dst_factor = blend.dst;
			if (bp.blend_factor_alpha && GSDevice::IsDualSourceBlendFactor(blend.dst))
			{
				// No dual-source unit on this device: the ROP's variable factor
				// arrives through the FIRST output's alpha (Classic's
				// blend_factor_in_alpha fallback, tfx.glsl writes alpha_blend.a to
				// o_col0.a under the bit) instead of the second output. The
				// lowering proved eligibility — the draw writes no alpha anywhere,
				// so the byte the factor rides is discarded on the way to the
				// target. Same value on the same /255 grid as the SRC1 shape, so
				// the realization is byte-identical to it; what it buys is the
				// carrier reaching the dominant variable-As row on the no-dualsrc
				// Mali blobs, where that row otherwise pays a read-rung pass per
				// draw (the measured 8.2× R&C pass explosion).
				conf.ps.blend_factor_in_alpha = 1;
				dst_factor =
					(blend.dst == GSDevice::INV_SRC1_COLOR || blend.dst == GSDevice::INV_SRC1_ALPHA) ?
						static_cast<u8>(GSDevice::INV_SRC_ALPHA) :
						static_cast<u8>(GSDevice::SRC_ALPHA);
			}
			conf.blend = {true, GSDevice::CONST_ONE, dst_factor, blend.op,
				GSDevice::CONST_ONE, GSDevice::CONST_ZERO, bp.blend_c == 2, bp.afix};
			conf.ps.no_color1 = !GSDevice::IsDualSourceBlendFactor(dst_factor);
		}
		else
		{
			conf.blend = {true, blend.src, blend.dst, blend.op,
				GSDevice::CONST_ONE, GSDevice::CONST_ZERO, bp.blend_c == 2, bp.afix};
			conf.ps.no_color1 = !GSDevice::IsDualSourceBlendFactor(blend.src) &&
			                    !GSDevice::IsDualSourceBlendFactor(blend.dst);
		}
		// The donor's global: a reverse-subtract ROP inverts the direction the
		// store's rounding should lean (GSRendererHW does this after every
		// realization; inert on the 32/24-bit native frames today, kept for parity
		// and for any future 16-bit envelope).
		if (conf.blend.op == GSDevice::OP_REV_SUBTRACT)
			conf.ps.round_inv = 1;
	}

	if (tex)
	{
		// A direct-colour source is the backend's "AEM expansion already done by the
		// CPU" leg; a palettised one is its "8-bit index texture + palette" leg
		// (pal_fmt 3 — four-bit indices arrive already widened to a byte, the same
		// convention Classic's CPU-built sources use, so no nibble masking in the
		// shader). Nearest draws sample through the GPU sampler — the exact
		// configuration the gs-texture capture scored 128/128 on nearest cells in
		// every arm; with a palette the sampler fetches the index and the shader
		// expands it. Bilinear draws filter in-shader (PS_TILE_LTF): the same capture
		// measured the console's filter as a 1/16-texel truncating snap with 4-bit
		// nested truncating lerps, which no sampler expresses, and gs-idxfilt measured
		// that a palettised fetch expands every corner through the CLUT BEFORE those
		// lerps — which is what the palette leg of fetch_texel_tile does.
		conf.tex = tex;
		if (pal)
		{
			conf.pal = pal;
			conf.ps.pal_fmt = 3;
			if (pal_direct)
			{
				conf.ps.tile_direct_pal = 1;
				conf.cb_ps.TileDirectPal = pal_direct_params;
			}
		}
		// The direct index leg: `tex` is the owner target and every fetch goes through
		// the swizzle (fetch_texel_tile), so the draw runs the texelFetch legs whatever
		// its filter — the sampler cannot do address arithmetic.
		const bool direct_leg = direct_idx >= 0;
		if (direct_leg)
		{
			conf.ps.tile_direct_idx = static_cast<u32>(direct_idx) + 1;
			conf.cb_ps.TileDirectIdx = direct_idx_params;
		}
		conf.vs.tme = 1;
		conf.vs.fst = PRIM->FST;
		conf.ps.fst = PRIM->FST;
		conf.ps.tfx = ctx->TEX0.TFX;
		conf.ps.tcc = ctx->TEX0.TCC;
		conf.sampler.biln = 0; // nearest by default; only the fast profile's snapped hardware-filter stage below unlocks it

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
		// Every textured native draw takes the in-shader legs: its coordinate comes
		// off the SW scanline's walk (PS_TILE_TWALK — payload built above; points
		// floor before reaching here), and the walk's value is what the scanline
		// hands its own sampler, so the texel indices, the 4-bit filter weight, the
		// per-pixel level selection and the bound shifts all run in the shader on it.
		// The GPU sampler leg below is therefore unreached by a native textured draw
		// today; it stays as the sampler-side reference (gs-texture proved nearest
		// through it exact) in case a class is ever handed back for speed.
		//
		// The lag (gs-shade, SCPH-30001): a non-sprite primitive's coordinate
		// trails the exact plane in the direction the walk is going, one 16.16
		// unit per forward axis, spent before the snap to a sixteenth. Sprites are
		// exact on silicon and take nothing; a point has no gradient to lag.
		const bool mip_draw = IsMipMapActive();
		const bool tclag_draw =
			m_vt.m_primclass != GS_SPRITE_CLASS && m_vt.m_primclass != GS_POINT_CLASS;
		if (m_twalk_at != NoWalk || direct_leg)
		{
			conf.ps.tile_tclag = tclag_draw;
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
				// ti.zw feeds the sampler leg's quotient: SW's numerator space is the
				// register claim × 65536, so the varying carries S·16·reg_tw. The walk
				// legs never read it — their numerator comes off the payload.
				conf.cb_vs.texture_scale = GSVector2((1.0f / 16.0f) / reg_tw, (1.0f / 16.0f) / reg_th);

				// Per-pixel MMAG/MMIN across the level-of-detail crossing, by the
				// scanline's rule and on the scanline's draws (sel.ltfx): lod is
				// -log2(Q)·2^L + K, so lod > 0 is exactly Q below one constant. Only
				// where the scanline divides per pixel — outside mipmapping, a sprite
				// or a constant-Q primitive is affine to it (sel.fst) and takes one
				// filter for the whole primitive.
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
				if (!mip_draw && !m_twalk_fst && m_vt.IsFilterCrossover())
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
			// A linear draw handed to this leg filters as Classic's shader-emulated
			// 4-tap (PS_LTF): four nearest taps through the sampler's own addressing,
			// float weights — the realization Classic runs for every palettised or
			// region-wrapped source, and the fast profile's stage-1 filter for the
			// demoted perspective classes. The sampler itself stays nearest.
			conf.ps.ltf = m_vt.IsLinear();
			if (mip_draw)
			{
				// The fast profile's mip demotion: Classic's own manual-LOD
				// machinery, exactly as its shader-emulated-sampler branch runs it —
				// the 4-tap filter stays in-shader per level, the LEVEL comes from
				// textureLod on the shader's lod (PS_MANUAL_LOD), and the sampler's
				// mipmap mode realizes the round (nearest) or blend (trilinear)
				// between levels. The lod constants encode the scanline's selector:
				// LCM (and FST, whose coordinates have no Q) pin lod = K by zeroing
				// the L factor; a draw already past MXL collapses to the constant
				// max level; hardware clamps the tail of the chain on its own.
				if (m_vt.m_lod.x > 0)
					conf.ps.ltf = (ctx->TEX1.MMIN >> 2) & 1;
				bool tri_mip = (ctx->TEX1.MMIN & 1) != 0;
				u32 lcm = ctx->TEX1.LCM;
				const int mxl = std::min<int>(static_cast<int>(ctx->TEX1.MXL), 6);
				float k = static_cast<float>(ctx->TEX1.K) / 16.0f;
				float l = static_cast<float>(1 << ctx->TEX1.L);
				if (static_cast<int>(m_vt.m_lod.x) >= static_cast<int>(ctx->TEX1.MXL))
				{
					k = static_cast<float>(mxl);
					lcm = 1;
					tri_mip = false;
				}
				// L = 0 pins lod = K − log2(Q)·0 = K. Safe here and only here:
				// every draw on this block is STQ (true-FST mip stays on the
				// walk), so Q is guard-proven finite and positive and the log2
				// term is a real zero, never NaN·0.
				if (lcm)
					l = 0.0f;
				conf.ps.manual_lod = 1;
				conf.cb_ps.LODParams.x = k;
				conf.cb_ps.LODParams.y = l;
				conf.cb_ps.LODParams.z = 0.0f; // the source holds every level from the base
				conf.cb_ps.LODParams.w = static_cast<float>(mxl);
				conf.sampler.triln = static_cast<u8>(tri_mip ? GS_MIN_FILTER::Nearest_Mipmap_Linear :
				                                               GS_MIN_FILTER::Nearest_Mipmap_Nearest);
				conf.sampler.lodclamp = 0;
			}
			// Hardware filter weights (the fast profile's last filter stage): one
			// linear tap at the 1/16-floored coordinate (PS_TILE_SNAP) replaces
			// the 4-tap shader filter — the snap keeps the console's coordinate
			// quantisation, the largest measured error class of Classic's sampler
			// path, and the hardware supplies the weights. Eligible when the
			// source is RGBA (a bound palette still expands indices in-shader;
			// filtering an index texture is meaningless) and both wrap modes are
			// plain (region modes clamp each corner in the shader, the same
			// reason Classic's own sampler path refuses them). Under mip the
			// sampler's min filter follows: the Nearest_ mip modes gain their
			// Linear_ prefix, so the level blend and the intra-level filter both
			// run in hardware off the one manual-LOD textureLod tap.
			const bool fast_filter = GSConfig.TileFastShading && !GSConfig.TileExactTexFilter;
			if (fast_filter && !pal && ((wms | wmt) & 2) == 0 && conf.ps.ltf)
			{
				conf.ps.ltf = 0;
				conf.ps.tile_snap = 1;
				conf.sampler.biln = 1;
				if (mip_draw)
					conf.sampler.triln = conf.sampler.triln + 2; // Nearest_Mipmap_* -> Linear_Mipmap_*
			}
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
	conf.tile_twalk_at = NoWalk;
	conf.tile_cwalk_at = NoWalk;
	if (!m_tile_payload.empty())
	{
		conf.tile_payload = m_tile_payload.data();
		conf.tile_payload_size = static_cast<u32>(m_tile_payload.size() / 4);
		conf.vs.tile_prim_ord = 1;
		if (m_twalk_at != NoWalk)
		{
			// The texture coordinate off the scanline's walk: 1 = triangle blocks, 2 =
			// sprite blocks; the FST bit says whether the walk is the truncating
			// integer DDA (the software renderer's effective sel.fst) or the float one
			// with the per-pixel truncated reciprocal.
			conf.ps.tile_twalk = (m_vt.m_primclass == GS_SPRITE_CLASS) ? 2 : 1;
			conf.ps.tile_twalk_fst = m_twalk_fst;
			conf.tile_twalk_at = m_twalk_at;
		}
		if (m_cwalk_at != NoWalk)
		{
			// The gouraud colour and the fog factor off the scanline's walk (the
			// shader reads whichever of the two this draw's selector enables).
			conf.ps.tile_cwalk = 1;
			conf.tile_cwalk_at = m_cwalk_at;
		}
	}
	conf.depth.ztst = ds ? plan.ztst : static_cast<u8>(ZTST_ALWAYS);
	conf.depth.zwe = plan.pass[0].z_write;
	conf.cb_vs.max_depth = 0xFFFFFFFFu;
	if (zwalk)
	{
		conf.ps.tile_zwalk = m_zwalk_plane ? 2 : 1;
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
		// The factor-alpha repair pass writes the draw's TRUE alpha, so the
		// factor must not ride its output (Classic clears the same bit on its
		// second pass for the same reason). The ROP's alpha factors are ONE/ZERO
		// on every carrier row, so the byte lands unblended.
		conf.alpha_second_pass.ps.blend_factor_in_alpha = 0;
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

	// The submitted-config half of the ledger row. Tile had never filled it, so
	// every column the writer gates on "submitted" — topology, barrier count, the
	// feedback-loop flag, the overlap verdict — was blank on every tile row, and
	// the read rung's arrival looked exactly like the read rung never running.
	// NoteTileDraw runs after this and restores the tile rect over the drawarea,
	// which is the area the tile rows are supposed to carry.
	if (GSDrawLog::IsActive()) [[unlikely]]
		GSDrawLog::EndDraw(conf, static_cast<u8>(m_prim_overlap));

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
	rec.tex_cbp = m_context->TEX0.CBP;
	rec.tex_clut_cfg = static_cast<u16>((m_context->TEX0.CPSM & 0xF) | ((m_context->TEX0.CSM & 1) << 4) |
										((m_context->TEX0.CSA & 0x1F) << 5) | ((m_context->TEX0.CLD & 7) << 10));
	rec.tex_psm = static_cast<u8>(m_context->TEX0.PSM);
	rec.tex_tbw = static_cast<u8>(m_context->TEX0.TBW);
	rec.tex_tw = static_cast<u8>(m_context->TEX0.TW);
	rec.tex_th = static_cast<u8>(m_context->TEX0.TH);

	rec.alpha = static_cast<u16>((ALPHA.A << 6) | (ALPHA.B << 4) | (ALPHA.C << 2) | ALPHA.D);
	rec.alpha_fix = static_cast<u8>(ALPHA.FIX);
	rec.vertex_rgba = static_cast<u32>((m_vt.m_min.c.I32[0] & 0xFF) | ((m_vt.m_min.c.I32[1] & 0xFF) << 8) |
									   ((m_vt.m_min.c.I32[2] & 0xFF) << 16) | ((m_vt.m_min.c.I32[3] & 0xFF) << 24));
	rec.vertex_rgba_eq = (m_vt.m_eq.rgba == 0xFFFF) ? 1 : 0;
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
	// The build half is not repeatable — the CLUT decode, the trace's alpha range and
	// the overlap verdict all warm on first call — so it is timed once and summed,
	// with the timer pair's own cost measured beside it and subtracted at report time.
	const u64 b0 = log ? Common::Timer::GetCurrentValue() : 0;
	const GSTileDrawInput in = BuildLoweringInput();
	if (log) [[unlikely]]
	{
		const u64 b1 = Common::Timer::GetCurrentValue();
		const u64 b2 = Common::Timer::GetCurrentValue();
		m_memo.build_ns += Common::Timer::ConvertValueToNanoseconds(b1 - b0);
		m_memo.timer_ns += Common::Timer::ConvertValueToNanoseconds(b2 - b1);
		m_memo.build_calls++;
	}
	GSTileDrawPlan plan = gsTileLowerDraw(in);
	// The bisect lever: past the session's native-draw budget every draw floors. It
	// counts draws that WOULD go native, so a run with limit N and a run with limit
	// N+1 differ in exactly one draw's route.
	if (plan.native && !r.rempty() && GSConfig.TileNativeDrawLimit > 0)
	{
		if (m_native_budget_used >= static_cast<u32>(GSConfig.TileNativeDrawLimit))
		{
			plan.native = false;
			plan.reason = GSTileFloorReason::DevLimit;
		}
		else
			m_native_budget_used++;
	}
	GSTileFloorReason reason = plan.reason;

	// The lockstep oracle arms only for a draw heading to the native route: a floored
	// draw IS the software arm, so there is nothing to compare it against. Arming syncs
	// the draw's inputs to CPU truth and snapshots its write footprint.
	const bool oracle = GSTileOracle::IsActive() && plan.native && !r.rempty() && OracleBeginDraw(plan, r);
	if (oracle) [[unlikely]]
	{
		// Re-base the ledger's clock: the instrument is not the renderer's cost. (The
		// comparison itself runs after record_ns is taken, so it never lands in it.)
		record_start = Common::Timer::GetCurrentValue();
	}

	bool native = false;
	if (plan.native)
	{
		// An empty rect draws nothing on either route; count it native for free.
		native = r.rempty() || TryNativeDraw(plan, r, reason);
	}

	// Perf-ceiling instrument (EmuCore/GS/TileSkipFloorDraws): drop a floored draw
	// entirely — no input spill, no model bookkeeping, no SW rasterization below.
	// Rendering is WRONG under this lever; its one legitimate output is the frame
	// TIME of a hypothetical 100%-native run (no handoff drains, no re-uploads),
	// which brackets the native path's cost while floor classes remain
	// unimplemented. The 2026-08-16 SD865 A/B motivated it: at 94-98% native the
	// frame is dominated by handoff churn (64-495 readbacks/frame), so a measured
	// frame time says nothing about the native path itself. Never a user setting;
	// never a correctness arm.
	const bool skip_floor = !native && GSConfig.TileSkipFloorDraws;
	if (!native && !skip_floor)
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

	if (GSTileOracle::IsActive() && native && !r.rempty()) [[unlikely]]
		m_oracle.native_draws++;

	if (oracle) [[unlikely]]
	{
		// The native arm ran; now run the software arm over the same inputs and score
		// them against each other and against the state both started from.
		if (native)
		{
			OracleCompareDraw(plan, r);
			m_oracle.compared_draws++;
		}
		else
			OracleAbandonDraw();
	}

	if (!native && !skip_floor)
		GSRendererSW::Draw();

	if (log) [[unlikely]]
	{
		// After record_ns is taken: the census times the lowering on its own clock and
		// must not land in the renderer's per-draw cost.
		const bool memo_hit = MeasureMemo(in);
		// The blending pass's realization, for the per-device leg distribution
		// (the carrier's variable-factor legs need dual-source blend, so which
		// leg a draw takes is a device property as much as a draw property).
		u8 blend_leg = 0;
		u8 blend_src1 = 0;
		if (native)
		{
			for (u32 i = 0; i < plan.pass_count; i++)
			{
				if (plan.pass[i].abe)
				{
					blend_leg = static_cast<u8>(plan.pass[i].blend_leg) + 1;
					blend_src1 = plan.pass[i].blend_src1 ? 1 : 0;
					break;
				}
			}
		}
		GSDrawLog::NoteTileDraw(memo_hit, record_ns, /*pass_id=*/0, MapFallbackReason(reason), r,
			plan.stq_guard, blend_leg, blend_src1, plan.carrier_refusal);
		GSDrawLog::FinishDraw();
	}
}

// -- M3f premise census ---------------------------------------------------------------
//
// What a draw-key memo could save is exactly gsTileLowerDraw: it is a pure function of
// BuildLoweringInput's result, so equal inputs give equal plans, and nothing upstream of
// it is skippable (the alpha range and the overlap verdict are read off THIS draw's
// vertices, and a key cheap enough to skip them cannot distinguish them). So the value
// of the memo is one hit rate times one function's cost, and this measures both.
//
// The cost is timed over kMemoReps repetitions because a single call sits under the
// clock's own resolution — a per-call figure read off one sample would be quantisation,
// not measurement. Timing runs on one draw in kMemoSample so the instrument itself stays
// small; the hit rate is counted on every draw. Nothing here reaches record_ns.
bool GSRendererTile::MeasureMemo(const GSTileDrawInput& in)
{
	static constexpr u32 kMemoReps = 256;
	static constexpr u32 kMemoSample = 32;

	// Stored and compared as bytes throughout, memcpy rather than assignment: a copy
	// that leaves padding behind would make two equal draws compare unequal, and the
	// only symptom would be a hit rate quietly reading low.
	const bool time_this_draw = (m_memo.draws % kMemoSample) == 0;
	m_memo.draws++;
	if (m_memo.have_prev && std::memcmp(&in, &m_memo.prev, sizeof(in)) == 0)
		m_memo.prev_hit++;
	std::memcpy(&m_memo.prev, &in, sizeof(in));
	m_memo.have_prev = true;

	// A move-to-front LRU over recent inputs: what a small direct cache would catch,
	// and the only structure that sees a draw stream alternating between a handful of
	// states (which is what the offline pass found on Ace Combat).
	bool hit = false;
	u32 at = m_memo.lru_count;
	for (u32 i = 0; i < m_memo.lru_count; i++)
	{
		if (std::memcmp(&in, &m_memo.lru[i], sizeof(in)) == 0)
		{
			at = i;
			hit = true;
			m_memo.lru_hit++;
			break;
		}
	}
	if (!hit && m_memo.lru_count < m_memo.lru.size())
		at = m_memo.lru_count++;
	for (u32 i = std::min<u32>(at, static_cast<u32>(m_memo.lru.size()) - 1); i > 0; i--)
		std::memcpy(&m_memo.lru[i], &m_memo.lru[i - 1], sizeof(in));
	std::memcpy(&m_memo.lru[0], &in, sizeof(in));

	if (!time_this_draw)
		return hit;

	// ⚠️ gsTileLowerDraw is pure and the input is loop-invariant, so consuming the
	// result is NOT enough to keep the repetitions: the compiler may legally evaluate
	// it once and reuse it, and the loop would then time 256 additions. Making the
	// input's address opaque at every iteration is what forces a real call. (Measured
	// both ways when this was written: the figure was the same, so nothing had been
	// folded — but the barrier is what makes that a fact rather than a hope.)
	GSTileDrawInput probe = in;
	GSTileDrawInput* p_in = &probe;
	const u64 t0 = Common::Timer::GetCurrentValue();
	for (u32 i = 0; i < kMemoReps; i++)
	{
		asm volatile("" : "+r"(p_in) : : "memory");
		const GSTileDrawPlan p = gsTileLowerDraw(*p_in);
		m_memo.sink += p.colormask + p.pass_count + static_cast<u8>(p.reason);
	}
	m_memo.lower_ns += Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - t0);
	m_memo.lower_calls += kMemoReps;

	// The other half of the trade. A memo does not make the lowering free — it swaps
	// the lowering for a key probe, and the probe is paid on misses too, so the probe's
	// cost is what the saving has to beat.
	//
	// This probes the input that was just moved to the front, so it measures a hit at
	// position zero: the CHEAPEST probe there is. That biases the comparison in the
	// memo's favour, deliberately — a verdict against the memo taken under its best
	// case is a verdict that does not need re-litigating under a fairer one.
	GSTileDrawInput* p_key = &probe;
	const u64 t1 = Common::Timer::GetCurrentValue();
	for (u32 i = 0; i < kMemoReps; i++)
	{
		asm volatile("" : "+r"(p_key) : : "memory");
		for (u32 j = 0; j < m_memo.lru_count; j++)
		{
			if (std::memcmp(p_key, &m_memo.lru[j], sizeof(*p_key)) == 0)
			{
				m_memo.sink += j + 1;
				break;
			}
		}
	}
	m_memo.probe_ns += Common::Timer::ConvertValueToNanoseconds(Common::Timer::GetCurrentValue() - t1);
	m_memo.probe_calls += kMemoReps;

	return hit;
}

void GSRendererTile::ReportMemoCensus() const
{
	if (!GSDrawLog::IsActive() || m_memo.draws == 0)
		return;

	const double draws = static_cast<double>(m_memo.draws);
	Console.WriteLn("Tile memo census over %u draws (LRU depth %zu):", m_memo.draws, m_memo.lru.size());
	Console.WriteLn("  prev-key hit %6.2f%%   LRU hit %6.2f%%", 100.0 * m_memo.prev_hit / draws,
		100.0 * m_memo.lru_hit / draws);
	const double lower = static_cast<double>(m_memo.lower_ns) / static_cast<double>(m_memo.lower_calls);
	const double probe = static_cast<double>(m_memo.probe_ns) / static_cast<double>(m_memo.probe_calls);
	Console.WriteLn("  gsTileLowerDraw %6.2f ns/call   key probe %6.2f ns/call   (%llu samples each)", lower,
		probe, static_cast<unsigned long long>(m_memo.lower_calls));
	// What a memo would net per draw: it skips the lowering on a hit and pays the
	// probe always. Negative means the machinery costs more than the work it removes.
	const double net = (m_memo.lru_hit / draws) * lower - probe;
	Console.WriteLn("  memo net %+7.2f ns/draw  (saves %.2f, pays %.2f)", net,
		(m_memo.lru_hit / draws) * lower, probe);
	if (m_memo.build_calls)
	{
		// The half no memo can skip: it reads THIS draw's vertices and CLUT, so a key
		// cheap enough to avoid it cannot tell two draws apart. Its size is what says
		// whether the memoizable half was ever the place to look.
		const double build = static_cast<double>(m_memo.build_ns - m_memo.timer_ns) /
							 static_cast<double>(m_memo.build_calls);
		Console.WriteLn("  BuildLoweringInput %.1f ns/draw (unmemoizable) -- memo is %.2f%% of the pair",
			build, 100.0 * net / (build + lower));
	}
}
