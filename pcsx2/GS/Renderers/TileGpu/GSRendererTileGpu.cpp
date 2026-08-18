// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GSRendererTileGpu.h"

#include "GS/Renderers/Tile/GSTileTypes.h"
#include "GS/Renderers/Tile/GSVramModel.h"
#include "GS/GSLocalMemory.h"

#include "common/Console.h"
#include "common/Assertions.h"

#include <algorithm>
#include <cmath>
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
} // namespace

// Stage-1 pin: the back-record/back-thread machinery stays off for this variant whatever
// GSBackThreadMode says — its own front end is the threading story (revisited only if a
// measurement asks).
GSRendererTileGpu::GSRendererTileGpu()
	: GSRenderer(/*allow_back_records=*/false)
{
	// The planner is intrinsic to this variant, not a toggle: with nothing else drawing yet,
	// the pass model IS the renderer's output, so it runs unconditionally.
	m_pass_sim.SetActive(true);
}

GSRendererTileGpu::~GSRendererTileGpu()
{
	ReportPassStructure();
}

void GSRendererTileGpu::VSync(u32 field, bool registers_written, bool idle_frame)
{
	// Close the frame's open pass and push its stats before the base presents (the order the
	// Tile renderer's observer uses — the model is independent of presentation, but the frame
	// boundary is here).
	m_pass_sim.OnVSync();

	// Structure the frame's accumulated draws into a pass plan and render it through the
	// device executor, before the base presents — so the targets the executor drew into are
	// ready when the base's present path calls GetOutput.
	BuildAndExecutePlan();

	GSRenderer::VSync(field, registers_written, idle_frame);

	// Nothing consumes the transfer log yet; clear it so it cannot grow without bound.
	m_draw_transfers.clear();
}

void GSRendererTileGpu::Draw()
{
	ObserveDraw();
	AccumulateDraw();
}

GSTexture* GSRendererTileGpu::GetOutput(int i, float& scale, int& y_offset)
{
	// The display buffer, matched to a colour target the executor rendered this frame. The
	// resolution mirrors the HW renderer's: the PCRTC display register names a FRAME base,
	// and the executor's targets are keyed by FRAME base. Wrong-fast: scale 1, no y offset
	// (no upscale, no wrapped display buffer yet). Absent target -> nothing to present.
	const int index = i >= 0 ? i : 1;
	const auto& fb = PCRTCDisplays.PCRTCDisplays[index];
	const GSVector2i size = PCRTCDisplays.GetFramebufferSize(i);
	if (fb.framebufferRect.rempty() || fb.FBW == 0 || size.x < 0 || size.y < 0)
		return nullptr;

	const u32 disp_bp = static_cast<u32>(fb.Block());
	for (const DisplayTarget& dt : m_display_targets)
	{
		if (dt.bp == disp_bp)
		{
			scale = 1.0f;
			y_offset = 0;
			return dt.texture;
		}
	}

	// Wrong-fast fallback: the PS2 display buffer's content is composited from work buffers by
	// the sprites and moves this stage does not render yet, so it has no target of its own.
	// Present the largest colour target instead — the frame's main render buffer — so the 3D
	// geometry is visible rather than a blank display. Removed once compositing is handled.
	GSTexture* best = nullptr;
	int best_area = 0;
	for (const DisplayTarget& dt : m_display_targets)
	{
		const int area = dt.texture->GetSize().x * dt.texture->GetSize().y;
		if (area > best_area)
		{
			best_area = area;
			best = dt.texture;
		}
	}
	if (best)
	{
		scale = 1.0f;
		y_offset = 0;
		return best;
	}
	return nullptr;
}

bool GSRendererTileGpu::IsCoverageAlphaSupported()
{
	return false;
}

// A host->local transfer's destination: an upload into the pass model. The base hook is
// empty (the transfer's bytes already landed in GSState's transfer path), so this only
// observes — an upload never has to break the pass in the model.
void GSRendererTileGpu::InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
{
	if (m_pass_sim_in_move)
		return; // the move's destination write is already modelled by OnMove
	const GSTileSurfaceLayout layout{BITBLTBUF.DBP, static_cast<u8>(BITBLTBUF.DBW),
		static_cast<u8>(BITBLTBUF.DPSM), KindForPsm(BITBLTBUF.DPSM)};
	m_pass_sim.OnUpload(PagesForTargetRect(layout, r));
}

// A local->host readback's source: a CPU consumer of GPU-written pages. The clut flag
// splits the on-GPU palette-gather road from a genuine readback in the model.
void GSRendererTileGpu::InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut)
{
	if (m_pass_sim_in_move)
		return; // the move's source read is already modelled by OnMove
	const GSTileSurfaceLayout layout{BITBLTBUF.SBP, static_cast<u8>(BITBLTBUF.SBW),
		static_cast<u8>(BITBLTBUF.SPSM), KindForPsm(BITBLTBUF.SPSM)};
	m_pass_sim.OnCpuRead(PagesForTargetRect(layout, r), clut);
}

// A local->local move. Observed as OnMove first — matching the Tile renderer's order, where
// OnMove sees the pass state before the move's own read/write halves touch it — then the
// base performs the real guest-memory copy. That base Move calls InvalidateLocalMem(src) and
// InvalidateVideoMem(dst) in turn; OnMove has already accounted both halves, so m_pass_sim_in_move
// suppresses their sim forwards for the copy's duration (otherwise the destination write
// double-counts as a draw-realized upload — the latent defect the Tile `moves` counter prices).
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

	m_pass_sim.OnMove(PagesForTargetRect(src_l, src_r), PagesForTargetRect(dst_l, dst_r));

	m_pass_sim_in_move = true;
	GSRenderer::Move();
	m_pass_sim_in_move = false;
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

// The accumulated per-frame pass structure, mean / p50 at teardown. This is the offline
// -tilepasssim arm's aggregate (minus the Tile-only GIF-stream tail): the structural counters
// the stage-1 gate reads against the design values, now over draws AND the stream's memory
// events, so every break reason is measured rather than zero by construction.
void GSRendererTileGpu::ReportPassStructure()
{
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

// The pass plan's geometry, one flushed batch at a time. Copies this draw's vertices and
// indices into the frame streams and records an indexed indirect draw plus the deferred
// PendingDraw (its target identity and transform inputs, resolved once the frame's targets
// are sized). Skips empty-rect draws exactly as ObserveDraw does, so the plan and the pass
// model count the same draws. Indices are 0-based within the batch; the draw's vertex_offset
// rebases them into the frame vertex stream.
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
		// the GS draws it flat, taking colour, Q, Z and fog from the second vertex. Build four
		// corners off that second vertex — only their XY differs — and emit two triangles. Indices
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

	PendingDraw pd = {};
	pd.fb_bp = ctx->FRAME.Block();
	pd.fb_psm = ctx->FRAME.PSM;
	pd.fb_fbw = ctx->FRAME.FBW;
	pd.z_bp = ctx->ZBUF.Block();
	pd.z_psm = ctx->ZBUF.PSM;
	pd.z_write = ctx->TEST.ZTE && !ctx->ZBUF.ZMSK;
	pd.z_test = ctx->TEST.ZTE && ctx->TEST.ZTST > ZTST_ALWAYS;
	pd.z_used = pd.z_write || pd.z_test;
	pd.ofx = static_cast<s32>(ctx->XYOFFSET.OFX);
	pd.ofy = static_cast<s32>(ctx->XYOFFSET.OFY);
	pd.rect = r;
	pd.draw_index = draw.state_index;

	// Texture inputs. Only the direct 32-bit families (PSMCT32, PSMCT24) are sampled at this stage:
	// they share one page/block/column geometry and need no CLUT, so one address path serves both.
	// Every other textured draw (16-bit, paletted, the alpha-byte views) keeps the vertex-colour
	// path until its format is added. The fixed TEX0 folds TW/TH to the used ST range, exactly as
	// ObserveDraw derives its footprint, so the dimensions the shader scales by match the draw.
	pd.tex_enable = false;
	if (PRIM->TME)
	{
		const bool mip = IsMipMapActive();
		const GIFRegTEX0 tex0 = ctx->GetSizeFixedTEX0(m_vt.m_min.t.xyxy(m_vt.m_max.t), m_vt.IsLinear(), mip);
		if (tex0.PSM == PSMCT32 || tex0.PSM == PSMCT24)
		{
			pd.tex_enable = true;
			pd.fst = PRIM->FST;
			pd.tbp0 = tex0.TBP0;
			pd.tbw = std::max<u32>(tex0.TBW, 1);
			pd.tw = 1u << std::min<u32>(tex0.TW, 10);
			pd.th = 1u << std::min<u32>(tex0.TH, 10);
			pd.tfx = tex0.TFX;
			pd.tcc = tex0.TCC;
			pd.wms = ctx->CLAMP.WMS;
			pd.wmt = ctx->CLAMP.WMT;
		}
	}

	m_plan_pending.push_back(pd);
}

// Recycle last frame's targets back to the device pool. Called at the head of a plan build,
// once last frame's present (which read them through GetOutput) has completed.
void GSRendererTileGpu::RecycleTargets()
{
	for (GSTexture* t : m_plan_targets)
	{
		if (t)
			g_gs_device->Recycle(t);
	}
	m_plan_targets.clear();
	m_display_targets.clear();
}

// Size the frame's targets, resolve each draw's screen->NDC transform against the target it
// lands in, group draws into GS-semantic passes, and submit the assembled plan to the device
// executor. Wrong-fast: one FRAME/ZBUF pair per pass (break on target change only), scale 1.
void GSRendererTileGpu::BuildAndExecutePlan()
{
	// An idle frame accumulated nothing; keep last frame's targets so the display still shows
	// the last rendered buffer rather than blanking.
	if (m_plan_pending.empty())
		return;

	RecycleTargets();

	if (g_gs_device->TileGpuExecutorAvailable())
	{
		// 1. Resolve the distinct colour (FRAME base) and depth (ZBUF base) targets this
		//    frame draws into, each grown to hold every draw that lands in it.
		std::vector<TargetSlot> color_slots;
		std::vector<TargetSlot> depth_slots;
		const auto slot_for = [](std::vector<TargetSlot>& slots, u32 bp, u32 fbw, int bottom) -> u32 {
			for (u32 k = 0; k < slots.size(); k++)
			{
				if (slots[k].bp == bp)
				{
					slots[k].height = std::max(slots[k].height, bottom);
					slots[k].fbw = std::max(slots[k].fbw, fbw);
					return k;
				}
			}
			slots.push_back(TargetSlot{bp, fbw, bottom, 0});
			return static_cast<u32>(slots.size() - 1);
		};
		for (PendingDraw& pd : m_plan_pending)
		{
			pd.color_slot = slot_for(color_slots, pd.fb_bp, pd.fb_fbw, pd.rect.w);
			pd.z_slot = pd.z_used ? slot_for(depth_slots, pd.z_bp, pd.fb_fbw, pd.rect.w) : 0;
		}

		// 2. Allocate a GSTexture per slot; the plan's target list is colours then depths.
		const auto alloc = [this](std::vector<TargetSlot>& slots, bool depth) {
			for (TargetSlot& s : slots)
			{
				const int w = std::max<int>(static_cast<int>(s.fbw), 1) * 64;
				const int h = std::clamp(s.height, 64, 2048);
				s.target_index = static_cast<u32>(m_plan_targets.size());
				m_plan_targets.push_back(depth ? g_gs_device->CreateDepthStencil(w, h, true)
											   : g_gs_device->CreateRenderTarget(w, h, GSTexture::Format::Color, true));
			}
		};
		alloc(color_slots, false);
		alloc(depth_slots, true);

		// 3. The per-draw state row: the HW tfx VertexScale/VertexOffset transform against the
		//    resolved colour target's size (scale 1), plus the z enables. Row i serves draw i.
		m_plan_states.resize(m_plan_pending.size());
		for (u32 i = 0; i < m_plan_pending.size(); i++)
		{
			const PendingDraw& pd = m_plan_pending[i];
			const GSVector2i dim = m_plan_targets[color_slots[pd.color_slot].target_index]->GetSize();
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
		}

		// 4. Group contiguous draws sharing a colour+depth target into one pass, one target
		//    pair each. Coarser than the pass model (which also breaks on feedback and the
		//    8-pair budget); rendering-correct, which is what the submission needs. The model
		//    stays the authoritative accounting — this is the actual GPU pass structure.
		u32 i = 0;
		while (i < m_plan_draws.size())
		{
			const u32 cs = m_plan_pending[i].color_slot;
			const bool zu = m_plan_pending[i].z_used;
			const u32 zs = m_plan_pending[i].z_slot;
			// The depth pipeline is per-pass fixed function, so z-write and z-test must be uniform
			// across the pass. A ZTST=ALWAYS write sprite and a ZTST=GEQUAL 3D draw share this depth
			// buffer in SotC; grouping them into one pass would force one test on both, so the sprite
			// either wrongly depth-rejects (GEQUAL) or the 3D wrongly always-passes (ALWAYS). Break on
			// z_write and z_test alongside the target pair so each pass gets its own depth pipeline.
			const bool zw = m_plan_pending[i].z_write;
			const bool zt = m_plan_pending[i].z_test;
			u32 j = i + 1;
			while (j < m_plan_draws.size() && m_plan_pending[j].color_slot == cs &&
				   m_plan_pending[j].z_used == zu && m_plan_pending[j].z_slot == zs &&
				   m_plan_pending[j].z_write == zw && m_plan_pending[j].z_test == zt)
				j++;

			GSDevice::GSTileGpuTargetPair tp = {};
			tp.frame_target = color_slots[cs].target_index;
			tp.zbuf_target = zu ? depth_slots[zs].target_index : GSDevice::GSTileGpuPassPlan::kNoTarget;

			GSDevice::GSTileGpuPass pass = {};
			pass.first_draw = i;
			pass.draw_count = j - i;
			pass.first_target_pair = static_cast<u32>(m_plan_target_pairs.size());
			pass.target_pair_count = 1;
			pass.declares_self_read = false;
			// GS depth grows towards the viewer: a real test is GEQUAL, a write-only draw is ALWAYS,
			// and the write follows ZMSK independently of the test. z_used == (z_write || z_test).
			pass.depth_mode = !zu ? GSDevice::GSTileGpuDepthMode::None
				: (zt ? (zw ? GSDevice::GSTileGpuDepthMode::TestWrite
							: GSDevice::GSTileGpuDepthMode::TestNoWrite)
					  : GSDevice::GSTileGpuDepthMode::WriteAlways);

			m_plan_target_pairs.push_back(tp);
			m_plan_passes.push_back(pass);
			i = j;
		}

		// 4b. Backstop for the framebuffer-fits-its-attachments clamp. The executor renders each
		//     pass into min(colour, depth) of the pair (a framebuffer may not be bigger than the
		//     attachment it carries), so a draw whose bottom falls below that minimum height would
		//     be silently clipped. It cannot: every z-using draw in the pass grew BOTH slots'
		//     height from its own rect.w in step 1, so min(colour,depth) height >= every draw's
		//     bottom by construction, and this loop never fires. It is a latent guard against a
		//     future grouping change breaking that invariant -- verified silent on the SotC dump,
		//     but the invariant is what is asserted, not the dump. (Width is fbw-derived and shared
		//     across the pair, so the clamp's only lossy axis is height.)
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

		// 5. Assemble the plan over the frame's streams and submit. The spans are CPU-side
		//    views alive until the clear below; the executor stages what it needs during the
		//    synchronous call.
		GSDevice::GSTileGpuPassPlan plan;
		plan.passes = m_plan_passes;
		plan.draws = m_plan_draws;
		plan.topologies = m_plan_topologies;
		plan.target_pairs = m_plan_target_pairs;
		plan.state_table = m_plan_states.data();
		plan.state_stride = sizeof(StateRow);
		plan.state_count = static_cast<u32>(m_plan_states.size());
		plan.vertices = std::span<const u8>(reinterpret_cast<const u8*>(m_plan_vertices.data()),
			m_plan_vertices.size() * sizeof(GSVertex));
		plan.vertex_stride = sizeof(GSVertex);
		plan.indices = m_plan_indices;
		plan.targets = m_plan_targets;

		// The frame's texture source: the current guest VRAM. Uploads have already landed in it
		// through the base transfer path, so it is the CPU-current truth this stage samples (a draw
		// sampling a target another draw rendered this frame is the feedback road, not handled here).
		plan.vram = m_mem.vm8();
		plan.vram_size = static_cast<u32>(GSLocalMemory::m_vmsize);

		g_gs_device->ExecuteTileGpuPassPlan(plan);

		// 6. Keep the colour targets addressable by FRAME base for GetOutput.
		for (const TargetSlot& s : color_slots)
			m_display_targets.push_back(DisplayTarget{s.bp, m_plan_targets[s.target_index]});
	}

	// The plan is submitted (or the device does not serve it); reset the per-frame streams.
	// The targets stay live for GetOutput and are recycled at the next build.
	m_plan_vertices.clear();
	m_plan_indices.clear();
	m_plan_states.clear();
	m_plan_draws.clear();
	m_plan_topologies.clear();
	m_plan_pending.clear();
	m_plan_passes.clear();
	m_plan_target_pairs.clear();
}
