// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Volumetric (GSVolumetric.h, ENABLE_VOLUMETRIC): the hardware renderer's depth capture and stereo.
// GSRendererHW.cpp only calls into these functions (one-line hooks), which are empty in builds
// without ENABLE_VOLUMETRIC (GSRendererHW.h).

#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/GSVolumetric.h"

#include "VMManager.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

void GSRendererHW::RecordDepthPairing(GSTextureCache::Target* rt, GSTextureCache::Target* ds)
{
	GSVolumetricDepthPairing* slot = &m_vr.depth_pairings[0];
	for (GSVolumetricDepthPairing& p : m_vr.depth_pairings)
	{
		if (p.rt_bp == rt->m_TEX0.TBP0 && p.last_draw)
		{
			slot = &p;
			break;
		}
		if (p.last_draw < slot->last_draw)
			slot = &p;
	}
	if (slot->rt_bp != rt->m_TEX0.TBP0)
		slot->dirty = false; // an evicted pairing's snapshot no longer belongs to this target
	slot->rt_bp = rt->m_TEX0.TBP0;
	slot->rt_bw = std::max(rt->m_TEX0.TBW, 1U);
	slot->z_bp = ds->m_TEX0.TBP0;
	slot->z_bw = std::max(ds->m_TEX0.TBW, 1U);
	slot->z_psm = m_cached_ctx.ZBUF.PSM;
	slot->last_draw = s_n + 1;
	slot->dirty = true;
}

void GSRendererHW::SnapshotDepthBeforeDraw()
{
	const u32 fbp = m_cached_ctx.FRAME.Block();
	const u32 zbp = m_cached_ctx.ZBUF.Block();
	// A clear: constant depth written unconditionally over (most of) the frame. HUD stamps are flat
	// too but small; snapshotting before them would split a frame's depth from its colour (late 3D
	// draws, often characters, follow the HUD), so only a clear marks the end of a frame.
	// Measured against the scissor: field-rendered games draw 640x224 buffers.
	const GIFRegSCISSOR& scissor = m_context->SCISSOR;
	const float scissor_w = static_cast<float>(scissor.SCAX1 - scissor.SCAX0 + 1);
	const float scissor_h = static_cast<float>(scissor.SCAY1 - scissor.SCAY0 + 1);
	const bool covers_frame = (m_vt.m_max.p.x - m_vt.m_min.p.x) >= 0.9f * scissor_w &&
	                          (m_vt.m_max.p.y - m_vt.m_min.p.y) >= 0.9f * scissor_h;
	// A depth test that the flat Z passes everywhere is no condition either: Ninja Assault starts each
	// frame with a full-screen sprite at 0xFFFF00 (24-bit Z), tested GEQUAL, over the frame just drawn.
	const float z_max = static_cast<float>((1ull << GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].trbpp) - 1);
	const bool passes_everywhere = !m_cached_ctx.TEST.ZTE || m_cached_ctx.TEST.ZTST == ZTST_ALWAYS ||
	                               ((m_cached_ctx.TEST.ZTST == ZTST_GEQUAL || m_cached_ctx.TEST.ZTST == ZTST_GREATER) &&
	                                   m_vt.m_min.p.z >= 0.99f * z_max);
	const bool flat_overwrite = !m_cached_ctx.ZBUF.ZMSK && m_vt.m_eq.z && covers_frame && passes_everywhere;
	if (m_vr.depth_trace_frame)
	{
		Console.WriteLnFmt("DepthTrace:   draw {} rt {:05x} z {:05x} {} {} ztst {} prim {} fbmsk {:08x} tex {} "
						   "bounds {:.0f},{:.0f}-{:.0f},{:.0f} z {:.0f}..{:.0f}{}",
			s_n, fbp, zbp, m_cached_ctx.ZBUF.ZMSK ? "zmask " : "zwrite", m_vt.m_eq.z ? "flat" : "3d  ",
			m_cached_ctx.TEST.ZTE ? static_cast<int>(m_cached_ctx.TEST.ZTST) : 9, static_cast<int>(m_vt.m_primclass),
			m_cached_ctx.FRAME.FBMSK, PRIM->TME ? "yes" : "no", m_vt.m_min.p.x, m_vt.m_min.p.y, m_vt.m_max.p.x,
			m_vt.m_max.p.y, m_vt.m_min.p.z, m_vt.m_max.p.z, flat_overwrite ? " CLEAR" : "");
	}
	for (GSVolumetricDepthPairing& p : m_vr.depth_pairings)
	{
		// A frame's depth is done once drawing moves to another colour target, or when it is about
		// to be cleared.
		if (p.dirty && (p.rt_bp != fbp || (flat_overwrite && p.z_bp == zbp)))
			SnapshotDepth(p);
	}
}

// Copies a whole target into `copy` (recreated when the size changes) with the given format, shrunk
// by `shrink` (nearest samples).
static bool CopyTarget(GSTexture* src, GSTexture*& copy, GSTexture::Format format, float shrink = 1.0f)
{
	const GSVector2i full = src->GetSize();
	const GSVector2i size(std::max(1, static_cast<int>(std::ceil(full.x / shrink))),
		std::max(1, static_cast<int>(std::ceil(full.y / shrink))));
	if (copy && (copy->GetWidth() != size.x || copy->GetHeight() != size.y))
	{
		g_gs_device->Recycle(copy);
		copy = nullptr;
	}
	if (!copy)
		copy = g_gs_device->CreateRenderTarget(size.x, size.y, format, false);
	if (!copy)
		return false;
	g_gs_device->StretchRect(src, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), copy,
		GSVector4(0.0f, 0.0f, static_cast<float>(size.x), static_cast<float>(size.y)),
		GetConvertShader(src->GetFormat(), format), Filter::Nearest);
	return true;
}

void GSRendererHW::SnapshotDepth(GSVolumetricDepthPairing& pairing)
{
	pairing.dirty = false;
	if (m_vr.depth_trace_frame)
		Console.WriteLnFmt("DepthTrace:   SNAPSHOT rt {:05x} z {:05x}", pairing.rt_bp, pairing.z_bp);
	GSTextureCache::Target* ds = g_texture_cache->FindExactTarget(pairing.z_bp, pairing.z_bw, GSTextureCache::DepthStencil);
	// Without the extras, the snapshot only feeds the guns' depth: taken at the PS2's resolution.
	const bool extras = GSVolumetric::DepthExtras();
	const float shrink = (!extras && ds && ds->m_scale > 1.0f) ? ds->m_scale : 1.0f;
	if (!ds || !ds->m_texture || !CopyTarget(ds->m_texture, pairing.snapshot, GSTexture::Format::DepthColor, shrink))
		return;
	pairing.snapshot_scale = ds->m_scale * static_cast<float>(pairing.snapshot->GetWidth()) /
	                         static_cast<float>(ds->m_texture->GetWidth());
	pairing.snapshot_vsync = m_vr.depth_vsync;

	GSTextureCache::Target* rt = g_texture_cache->FindExactTarget(pairing.rt_bp, pairing.rt_bw, GSTextureCache::RenderTarget);
	if (extras && rt && rt->m_texture && rt->m_texture->GetFormat() == GSTexture::Format::Color &&
		CopyTarget(rt->m_texture, pairing.colour_snapshot, GSTexture::Format::Color))
		pairing.colour_snapshot_scale = rt->m_scale;
	else if (pairing.colour_snapshot)
	{
		g_gs_device->Recycle(pairing.colour_snapshot);
		pairing.colour_snapshot = nullptr;
	}
}

int GSRendererHW::DisplayedStereoEye()
{
	// The merge draws circuit 1 over circuit 0.
	GSTextureCache::Target* rt = m_vr.output_rt[1] ? m_vr.output_rt[1] : m_vr.output_rt[0];
	if (!rt)
		return -1;
	for (const auto& [bp, eye] : m_vr.rt_stereo_eye)
	{
		if (bp == rt->m_TEX0.TBP0)
			return eye;
	}
	return -1;
}

void GSRendererHW::ReleaseDepthCanvases()
{
	for (GSTexture*& canvas : m_vr.depth_canvas)
	{
		if (canvas)
			g_gs_device->Recycle(canvas);
		canvas = nullptr;
	}
	for (auto* ring : {&m_vr.prehud_canvas, &m_vr.live_depth_canvas})
	{
		for (GSTexture*& canvas : *ring)
		{
			if (canvas)
				g_gs_device->Recycle(canvas);
			canvas = nullptr;
		}
	}
	for (GSVolumetricDepthPairing& p : m_vr.depth_pairings)
	{
		if (p.snapshot)
			g_gs_device->Recycle(p.snapshot);
		if (p.colour_snapshot)
			g_gs_device->Recycle(p.colour_snapshot);
		p = {};
	}
	GSVolumetric::CurrentFrame().prehud = nullptr;
	GSVolumetric::CurrentFrame().depth = nullptr;
}

void GSRendererHW::MergeDepth(GSTexture* const* tex, const GSVector4* src_gs_read, const GSVector4* dst, const GSVector2i& fs)
{
	GSVolumetricFrame& frame = GSVolumetric::CurrentFrame();
	frame.depth = nullptr;
	frame.prehud = nullptr;
	frame.live_depth = nullptr;
	const u32 vsync = m_vr.depth_vsync++;

	// The merge draws circuit 1 over circuit 0, and drops circuit 0 when both show the same target.
	const int c = tex[1] ? 1 : 0;
	GSTextureCache::Target* rt = m_vr.output_rt[c];
	// One frame in every 300 gets each draw logged, from this vsync to the next.
	m_vr.depth_trace_frame = GSVolumetric::IsTraceEnabled() && m_vr.depth_trace_vsyncs == 150;
	if (m_vr.depth_trace_frame)
		Console.WriteLnFmt("DepthTrace: frame after vsync, displayed rt {:05x}:", rt ? rt->m_TEX0.TBP0 : 0xFFFFFu);
	if (GSVolumetric::IsTraceEnabled() && ++m_vr.depth_trace_vsyncs >= 300)
	{
		Console.WriteLnFmt("DepthTrace: displayed rt {:05x} over {} vsyncs, 3D textured Q {:.6f}..{:.6f}",
			rt ? rt->m_TEX0.TBP0 : 0xFFFFFu, m_vr.depth_trace_vsyncs, m_vr.trace_q_min, m_vr.trace_q_max);
		m_vr.trace_q_min = m_vr.trace_q_max = 0.0f;
		for (const auto& [key, count] : m_vr.depth_trace)
			Console.WriteLnFmt("DepthTrace:   rt {:05x} z {:05x} {} ztst {} : {} draws", std::get<0>(key), std::get<1>(key),
				std::get<2>(key) ? "flat" : "3d  ", std::get<3>(key), count);
		m_vr.depth_trace.clear();
		m_vr.depth_trace_vsyncs = 0;
	}
	if (!tex[c] || !rt || rt->m_texture != tex[c] || fs.x <= 0 || fs.y <= 0)
		return;

	const GSVolumetricDepthPairing* pairing = nullptr;
	for (const GSVolumetricDepthPairing& p : m_vr.depth_pairings)
	{
		if (p.last_draw && p.rt_bp == rt->m_TEX0.TBP0 && (!pairing || p.last_draw > pairing->last_draw))
			pairing = &p;
	}
	if (!pairing)
		return;

	// The latest complete snapshot wins, if it is recent: games clear Z mid-frame before the HUD and
	// draw a few things after it, and when a vsync lands before that clear the live depth is a frame
	// half drawn. Only without a recent snapshot does a target still being drawn give its live depth.
	const bool fresh_snapshot = pairing->snapshot && vsync - pairing->snapshot_vsync <= 1;
	const bool recent_snapshot = pairing->snapshot && vsync - pairing->snapshot_vsync <= 3;
	GSTexture* depth_src = nullptr;
	float depth_scale = 1.0f;
	GSTexture::Format depth_format = GSTexture::Format::DepthStencil;
	GSTexture* prehud_src = nullptr; // the live frame has no HUD on top of it yet: no mask
	float prehud_scale = 1.0f;
	if (pairing->dirty && !recent_snapshot)
	{
		GSTextureCache::Target* ds = g_texture_cache->FindExactTarget(pairing->z_bp, pairing->z_bw, GSTextureCache::DepthStencil);
		if (ds && ds->m_texture)
		{
			depth_src = ds->m_texture;
			depth_scale = ds->m_scale;
		}
	}
	else if (recent_snapshot)
	{
		// A snapshot more than a few vsyncs old belongs to an earlier scene: a movie or 2D screen
		// drawn into the same target since has no depth of its own.
		depth_src = pairing->snapshot;
		depth_scale = pairing->snapshot_scale;
		depth_format = GSTexture::Format::DepthColor;
		prehud_src = pairing->colour_snapshot;
		prehud_scale = pairing->colour_snapshot_scale;
	}
	if (!depth_src)
		return;

	const u32 ring = m_vr.depth_canvas_index;
	m_vr.depth_canvas_index = (m_vr.depth_canvas_index + 1) % static_cast<u32>(m_vr.depth_canvas.size());
	// Without the extras, the depth canvas is shrunk to the PS2's resolution; the host samples it by
	// UV, so its size does not matter there.
	const bool extras = GSVolumetric::DepthExtras();
	const float shrink = (!extras && rt->m_scale > 1.0f) ? rt->m_scale : 1.0f;
	const GSVector2i canvas_size(std::max(1, static_cast<int>(std::ceil(fs.x / shrink))),
		std::max(1, static_cast<int>(std::ceil(fs.y / shrink))));
	const GSVector4 depth_dst = dst[c] * GSVector4(static_cast<float>(canvas_size.x) / static_cast<float>(fs.x),
		static_cast<float>(canvas_size.y) / static_cast<float>(fs.y)).xyxy();
	// A canvas also goes when the device's layer count changed (a switch between Flat and Volumetric):
	// the host reads it as layered exactly when the frame has a right eye.
	const auto canvas_for = [&](GSTexture*& canvas, GSTexture::Format format, const GSVector2i& size) {
		if (canvas && (canvas->GetWidth() != size.x || canvas->GetHeight() != size.y ||
						  canvas->GetArrayLayers() != g_gs_device->GetStereoLayers()))
		{
			g_gs_device->Recycle(canvas);
			canvas = nullptr;
		}
		if (!canvas)
			canvas = g_gs_device->CreateRenderTarget(size.x, size.y, format, false);
		if (canvas)
			g_gs_device->ClearRenderTarget(canvas, 0);
		return canvas;
	};

	// src_gs_read is normalised to the colour target; the depth target and the snapshots hold the
	// same GS pixels but can differ in size and scale.
	const GSVector4 src_px = src_gs_read[c] * GSVector4(tex[c]->GetSize()).xyxy();
	const auto src_rect = [&](GSTexture* src, float scale) {
		return src_px * GSVector4(scale / rt->m_scale) / GSVector4(src->GetSize()).xyxy();
	};

	GSTexture* canvas = canvas_for(m_vr.depth_canvas[ring], GSTexture::Format::DepthColor, canvas_size);
	if (!canvas)
		return;
	g_gs_device->StretchRect(depth_src, src_rect(depth_src, depth_scale), canvas, depth_dst,
		GetConvertShader(depth_format, GSTexture::Format::DepthColor), Filter::Nearest);

	if (prehud_src && extras)
	{
		if (GSTexture* prehud = canvas_for(m_vr.prehud_canvas[ring], GSTexture::Format::Color, fs))
		{
			g_gs_device->StretchRect(prehud_src, src_rect(prehud_src, prehud_scale), prehud, dst[c],
				GetConvertShader(GSTexture::Format::Color, GSTexture::Format::Color), Filter::Nearest);
			frame.prehud = prehud;
		}
	}

	if (fresh_snapshot && extras)
	{
		GSTextureCache::Target* ds = g_texture_cache->FindExactTarget(pairing->z_bp, pairing->z_bw, GSTextureCache::DepthStencil);
		GSTexture* live = ds && ds->m_texture ? canvas_for(m_vr.live_depth_canvas[ring], GSTexture::Format::DepthColor, fs) : nullptr;
		if (live)
		{
			g_gs_device->StretchRect(ds->m_texture, src_rect(ds->m_texture, ds->m_scale), live, dst[c],
				GetConvertShader(GSTexture::Format::DepthStencil, GSTexture::Format::DepthColor), Filter::Nearest);
			frame.live_depth = live;
		}
	}

	frame.depth = canvas;
	frame.depth_scale = 4294967296.0f / static_cast<float>((1ull << GSLocalMemory::m_psm[pairing->z_psm].trbpp) - 1);
}

void GSRendererHW::VolumetricTraceDraw(GSTextureCache::Target* rt, GSTextureCache::Target* ds,
	GSTextureCache::Source* src, bool skip_draw)
{
	if (GSVolumetric::DrawTraceActive())
	{
		Console.WriteLn("Draw: %s rt %04x/%d %dx%d L%u ds %04x tex %s %04x/%d tw %d th %d tbw %d src %s target %d direct %d from %04x %dx%d L%u stereo %d prim %d verts %u fst %d bbox %.0f,%.0f-%.0f,%.0f",
			skip_draw ? "SKIP" : "draw", rt ? static_cast<u32>(rt->m_TEX0.TBP0) : 0xFFFFu, rt ? static_cast<int>(rt->m_TEX0.PSM) : -1,
			(rt && rt->m_texture) ? rt->m_texture->GetWidth() : 0, (rt && rt->m_texture) ? rt->m_texture->GetHeight() : 0,
			(rt && rt->m_texture) ? rt->m_texture->GetArrayLayers() : 0u, ds ? static_cast<u32>(ds->m_TEX0.TBP0) : 0xFFFFu,
			PRIM->TME ? "on" : "off", static_cast<u32>(m_cached_ctx.TEX0.TBP0), static_cast<int>(m_cached_ctx.TEX0.PSM),
			1 << m_cached_ctx.TEX0.TW, 1 << m_cached_ctx.TEX0.TH, static_cast<int>(m_cached_ctx.TEX0.TBW),
			src ? "yes" : "no", src ? static_cast<int>(src->m_target) : -1, src ? static_cast<int>(src->m_target_direct) : -1,
			(src && src->m_from_target) ? static_cast<u32>(src->m_from_target->m_TEX0.TBP0) : 0xFFFFu,
			(src && src->m_texture) ? src->m_texture->GetWidth() : 0, (src && src->m_texture) ? src->m_texture->GetHeight() : 0,
			(src && src->m_texture) ? src->m_texture->GetArrayLayers() : 0u,
			(src && src->m_texture) ? static_cast<int>(src->m_texture->HasStereoContent()) : -1,
			static_cast<int>(m_vt.m_primclass), m_vertex->next, PRIM->FST ? 1 : 0,
			m_vt.m_min.p.x, m_vt.m_min.p.y, m_vt.m_max.p.x, m_vt.m_max.p.y);
	}
}

void GSRendererHW::VolumetricSaveTraceDraw(GSTextureCache::Target* rt, GSTextureCache::Target* ds, bool skip_draw)
{
	// Draw trace with VOLUMETRIC_TRACE_SAVE set: the render target after every draw of the traced frame.
	if (GSVolumetric::DrawTraceActive() && !skip_draw && rt && rt->m_texture)
	{
		static const char* s_save_dir = std::getenv("VOLUMETRIC_TRACE_SAVE");
		static const bool s_save_ds = std::getenv("VOLUMETRIC_TRACE_SAVE_DS") != nullptr;
		static const char* s_every_env = std::getenv("VOLUMETRIC_TRACE_SAVE_EVERY");
		static const u32 s_every = s_every_env ? static_cast<u32>(std::atoi(s_every_env)) : 1;
		static u32 s_trace = 0, s_index = 0;
		if (s_trace != GSVolumetric::DrawTraceNumber())
		{
			s_trace = GSVolumetric::DrawTraceNumber();
			s_index = 0;
		}
		const u32 index = s_index++;
		if (s_save_dir && (s_every <= 1 || (index % s_every) == s_every - 1))
		{
			g_gs_device->FlushBeforeReadback();
			GSTexture* target = (s_save_ds && ds && ds->m_texture) ? ds->m_texture : rt->m_texture;
			target->Save(fmt::format("{}/t{}-draw{:03d}-{:04x}.png", s_save_dir, s_trace, index, static_cast<u32>(rt->m_TEX0.TBP0)));
		}
	}
}

void GSRendererHW::VolumetricRecordDraw(GSTextureCache::Target* rt, GSTextureCache::Target* ds)
{
	if (rt && GSVolumetric::GetStereo().enabled)
	{
		auto it = std::find_if(m_vr.rt_stereo_eye.begin(), m_vr.rt_stereo_eye.end(),
			[bp = rt->m_TEX0.TBP0](const auto& e) { return e.first == bp; });
		if (it == m_vr.rt_stereo_eye.end())
		{
			it = m_vr.rt_stereo_eye.begin() + m_vr.rt_stereo_eye_next;
			m_vr.rt_stereo_eye_next = (m_vr.rt_stereo_eye_next + 1) % m_vr.rt_stereo_eye.size();
		}
		*it = {static_cast<u32>(rt->m_TEX0.TBP0), GSVolumetric::CurrentEye()};
	}

	if (rt && ds && !m_using_temp_z && !m_cached_ctx.ZBUF.ZMSK && GSVolumetric::IsEnabled())
	{
		if (GSVolumetric::IsTraceEnabled())
		{
			m_vr.depth_trace[{static_cast<u32>(rt->m_TEX0.TBP0), static_cast<u32>(ds->m_TEX0.TBP0), m_vt.m_eq.z != 0,
				m_cached_ctx.TEST.ZTE ? static_cast<u32>(m_cached_ctx.TEST.ZTST) : 9u}]++;
			if (PRIM->TME && !PRIM->FST && !m_vt.m_eq.z)
			{
				m_vr.trace_q_min = m_vr.trace_q_max == 0.0f ? m_vt.m_min.t.z : std::min(m_vr.trace_q_min, m_vt.m_min.t.z);
				m_vr.trace_q_max = std::max(m_vr.trace_q_max, m_vt.m_max.t.z);
			}
		}
		if (!m_vt.m_eq.z)
			RecordDepthPairing(rt, ds);
	}
}

void GSRendererHW::VolumetricClassifyDraw(GSTextureCache::Target* rt, GSTextureCache::Source* tex)
{
	// GSVolumetric stereo: a draw that textures from a picture already drawn with each eye's
	// parallax (a copy, blend or post effect over it) is not shifted again, or the parallax would be
	// applied twice and smear the picture over itself. Nor is a flat textured draw with Q exactly 1,
	// a 2D quad given 3D coordinates. A texture holding the same picture in both layers is an
	// ordinary texture, wherever the texture cache keeps it: Time Crisis II textures its scene from
	// an area the cache holds as a target, and draws its transparent scenery with Q = 1.
	// In Z mode the shift comes from Z alone, so a Q = 1 draw tested against the scene's depth at a
	// real Z sits where its Z puts it and is shifted like the rest: Time Crisis II draws the halos
	// of its wall lamps as additive Q = 1 billboards at the lamp's depth, which pinned to the glass
	// floated in front of the wall.
	// A per-eye depth buffer read as a texture is a screen-space effect over the scene too: Time
	// Crisis 3 turns each pixel's depth into fog alpha with sprites at the far plane, which shifted
	// put the far scene's fog over the near rocks of the other eye's picture.
	const bool stereo_depth_source = tex && tex->m_texture && tex->m_texture->GetArrayLayers() > 1 &&
									 tex->m_from_target && tex->m_from_target->m_type == GSTextureCache::DepthStencil;
	const bool stereo_source = tex && tex->m_texture && (tex->m_texture->HasStereoContent() || stereo_depth_source);
	const bool depth_placed = GSVolumetric::GetStereo().z_mode && m_cached_ctx.TEST.ZTE &&
							  (m_cached_ctx.TEST.ZTST == ZTST_GEQUAL || m_cached_ctx.TEST.ZTST == ZTST_GREATER) &&
							  m_vt.m_min.p.z > 0.0f;
	m_vr.stereo_source = stereo_source;
	m_vr.stereo_screen_space = stereo_source ||
		((m_vt.m_primclass == GS_SPRITE_CLASS || m_vt.m_eq.z) && m_process_texture && !PRIM->FST && m_vt.m_eq.q &&
			m_vt.m_min.t.z == 1.0f && !depth_placed);
	// An untextured fill across the whole width (a background colour or clear at the far plane)
	// moved by the parallax would leave a strip of stale picture along one edge.
	if (!m_process_texture && (m_vt.m_primclass == GS_SPRITE_CLASS || m_vt.m_eq.z) && rt &&
		(m_vt.m_max.p.x - m_vt.m_min.p.x) >= 0.95f * static_cast<float>(rt->GetUnscaledSize().x))
	{
		m_vr.stereo_screen_space = true;
	}

	// Only the pictures that get displayed are drawn from the eyes' positions. Offscreen renders
	// (shadow textures, reflections, Time Crisis II's character shadows into 0x1a40) are not seen
	// through the window; they stay the same for both eyes and are used like any texture.
	if (GSVolumetric::GetStereo().enabled)
	{
		for (int i = 0; i < 2; i++)
		{
			const u32 bp = m_regs->DISP[i].DISPFB.Block();
			if (std::find(m_vr.stereo_display_bp.begin(), m_vr.stereo_display_bp.end(), bp) == m_vr.stereo_display_bp.end())
			{
				std::rotate(m_vr.stereo_display_bp.rbegin(), m_vr.stereo_display_bp.rbegin() + 1, m_vr.stereo_display_bp.rend());
				m_vr.stereo_display_bp[0] = bp;
			}
		}
	}
	m_vr.stereo_display_target = rt && std::find(m_vr.stereo_display_bp.begin(), m_vr.stereo_display_bp.end(),
										 static_cast<u32>(rt->m_TEX0.TBP0)) != m_vr.stereo_display_bp.end();

	// A 2D overlay centred on where a shot landed is an effect of that shot (Crisis Zone's hit
	// bursts: FST sprites at its HUD's Z, drawn without a depth test among the HUD's pieces): it goes
	// to the scene's depth there. HUD pieces are told apart by place and size: away from the shot,
	// or spanning most of the picture (Crisis Zone's HUD frame covers all of it). Textured by TME, not
	// m_process_texture: the renderer skips the texture of some of the additive bursts.
	// A HUD piece is drawn in the same place every frame (Crisis Zone's ammo bar, next to where the
	// player shoots), or at the Z format's maximum like the HUDs VolumetricStereoConstants pins.
	// With the game's fx_overlays, an additive overlay that is not a HUD piece goes to the nearest
	// scene depth under its middle wherever it is (Crisis Zone's bursts on a dying enemy).
	// So does, with fx_overlays, a flat Q = 1 quad tested against the scene, without a Z write, at a
	// depth far nearer than the scene's there: an effect given a fake depth so that it always shows
	// (Ninja Assault's hit sparks, at a tenth of the depth range). One at the depth of what it is on
	// (Time Crisis II's lamp halos) keeps its own.
	m_vr.overlay_znorm = -1.0f;
	m_vr.overlay_fx = false;
	const GSVolumetric::StereoSettings& stereo = GSVolumetric::GetStereo();
	const bool fake_depth = stereo.fx_overlays && !PRIM->FST && depth_placed && m_vt.m_eq.z && m_vt.m_eq.q &&
							m_vt.m_min.t.z == 1.0f && m_cached_ctx.ZBUF.ZMSK;
	if (!m_vr.stereo_display_target || !stereo.enabled || !stereo.z_mode || !PRIM->TME || stereo_source ||
		!(PRIM->FST || m_vr.stereo_screen_space || fake_depth))
		return;
	// By corner alone: Crisis Zone uploads its HUD's textures to a different address every frame.
	const u64 key = static_cast<u64>(static_cast<u32>(static_cast<int>(m_vt.m_min.p.x) >> 1)) << 32 |
					static_cast<u32>(static_cast<int>(m_vt.m_min.p.y) >> 1);
	auto [seen, added] = m_vr.overlay_seen.try_emplace(key, m_vr.overlay_vsync, m_vr.overlay_vsync);
	if (!added && m_vr.overlay_vsync - seen->second.second > 3) // a gap: shown again
		seen->second.first = m_vr.overlay_vsync;
	seen->second.second = m_vr.overlay_vsync;
	constexpr u32 HUD_VSYNCS = 60; // about a second
	const bool steady = m_vr.overlay_vsync - seen->second.first >= HUD_VSYNCS;
	const u64 z_format_max = (1ull << GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].trbpp) - 1;
	const bool at_max_depth = m_vt.m_eq.z && m_vt.m_min.p.z >= static_cast<float>(z_format_max);
	// Far nearer: in front of the scene by more than the scene's own depth again.
	const float own_znorm = m_vt.m_min.p.z / static_cast<float>(z_format_max);
	const auto in_front_of = [&](float znorm) { return !fake_depth || own_znorm > 2.0f * znorm; };
	// Placed by scaling its Z, which needs some.
	if (steady || at_max_depth || m_vt.m_max.p.z < 1.0f)
		return;
	const int display = PCRTCDisplays.PCRTCDisplays[0].enabled ? 0 : 1;
	const GSVector4i fb = PCRTCDisplays.GetFramebufferRect(display);
	const float fw = static_cast<float>(fb.width()), fh = static_cast<float>(fb.height());
	const float w = m_vt.m_max.p.x - m_vt.m_min.p.x, h = m_vt.m_max.p.y - m_vt.m_min.p.y;
	if (fw <= 0.0f || fh <= 0.0f || w >= 0.6f * fw || h >= 0.75f * fh)
		return;

	const float cx = 0.5f * (m_vt.m_min.p.x + m_vt.m_max.p.x), cy = 0.5f * (m_vt.m_min.p.y + m_vt.m_max.p.y);
	const float reach = 0.25f * std::max(w, h) + 0.03f * fw;
	float nearest = reach * reach * 2.0f;
	for (const GSVolumetric::ShotAnchor& a : GSVolumetric::GetShotAnchors())
	{
		const float dx = cx - (static_cast<float>(fb.x) + a.u * fw), dy = cy - (static_cast<float>(fb.y) + a.v * fh);
		if (std::abs(dx) <= reach && std::abs(dy) <= reach && dx * dx + dy * dy < nearest && in_front_of(a.znorm))
		{
			nearest = dx * dx + dy * dy;
			m_vr.overlay_znorm = a.znorm;
		}
	}

	// Additive: Cs * (As or FIX) + Cd. A HUD is blended over the picture instead.
	const bool additive = PRIM->ABE && m_context->ALPHA.A == 0 && m_context->ALPHA.B == 2 && m_context->ALPHA.D == 1;
	if (m_vr.overlay_znorm < 0.0f && stereo.fx_overlays && (additive || fake_depth))
	{
		// A small window at its middle, the same for each layer of an effect (a burst is a few sprites
		// of different sizes around one point); its glow spreads past what it is on.
		const float u = (cx - static_cast<float>(fb.x)) / fw, v = (cy - static_cast<float>(fb.y)) / fh;
		const float znorm = GSVolumetric::SceneDepth(u - 0.03f, v - 0.04f, u + 0.03f, v + 0.04f);
		if (znorm >= 0.0f && in_front_of(znorm))
		{
			m_vr.overlay_znorm = znorm;
			m_vr.overlay_fx = true;
		}
	}
}

void GSRendererHW::VolumetricStereoConstants(GSTextureCache::Target* rt)
{
	// GSVolumetric stereo prototype: this frame's eye.
	const GSVolumetric::StereoSettings& stereo = GSVolumetric::GetStereo();
	// A draw at constant depth at the Z format's maximum is a HUD stamped over the scene (Time Crisis
	// II draws its whole HUD that way, with ordinary texture coordinates): no shift, on the screen.
	const u64 z_format_max = (1ull << GSLocalMemory::m_psm[m_cached_ctx.ZBUF.PSM].trbpp) - 1;
	const bool hud_at_max_depth = m_vt.m_eq.z && m_vt.m_min.p.z >= static_cast<float>(z_format_max);
	if (stereo.enabled && rt && rt->m_texture)
	{
		// What this draw leaves in the target: its own parallax, or the stereo picture it re-reads.
		const bool marks =
			(!hud_at_max_depth && !m_vr.stereo_screen_space && !PRIM->FST && m_vr.stereo_display_target) || m_vr.stereo_source;
		if (marks)
			rt->m_texture->SetStereoContent(true);
	}
	// FST draws are 2D (HUDs, menus, overlays): only an overlay placed in the scene is shifted (see
	// VolumetricClassifyDraw), its Z scaled so that it reads as the depth it goes to.
	const bool placed_overlay = m_vr.overlay_znorm >= 0.0f;
	if (stereo.enabled && m_vr.stereo_display_target &&
		(placed_overlay || (!hud_at_max_depth && !m_vr.stereo_screen_space && !PRIM->FST)))
	{
		// Multiview draws both eyes at once: x/y of vr_stereo for the left (view 0), y/z of
		// vr_stereo2 for the right. Otherwise vr_stereo carries this frame's eye.
		const bool multiview = g_gs_device->GetStereoLayers() > 1;
		const GSVector4 shift = GSVolumetric::EyeShift(multiview ? 0 : GSVolumetric::CurrentEye());
		const GSVector4 shift_right = multiview ? GSVolumetric::EyeShift(1) : GSVector4::zero();
		// Portal: w carries the eyes' distance ratios, the left one negated (w < 0 selects the portal).
		const bool portal = stereo.portal && stereo.z_mode;
		const float mode = portal ? -std::max(shift.z, 0.01f) : stereo.z_mode ? 1.0f : 0.0f;
		m_conf.cb_vs.vr_stereo = GSVector4(shift.x, shift.y, stereo.convergence, mode);
		const float z_scale = placed_overlay ? m_vr.overlay_znorm * 4294967296.0f / m_vt.m_max.p.z :
											 4294967296.0f / static_cast<float>(z_format_max);
		m_conf.cb_vs.vr_stereo2 = GSVector4(z_scale, shift_right.x,
			shift_right.y, portal ? std::max(shift_right.z, 0.01f) : 0.0f);
	}
	else
	{
		m_conf.cb_vs.vr_stereo = GSVector4::zero();
		m_conf.cb_vs.vr_stereo2 = GSVector4::zero();
	}
	if (stereo.enabled && GSVolumetric::DrawTraceActive())
	{
		const char* why = !m_vr.stereo_display_target ? "offscreen" :
						  placed_overlay           ? (m_vr.overlay_fx ? "fx-overlay" : "shot-overlay") :
						  hud_at_max_depth         ? "pin:hud" :
						  m_vr.stereo_source          ? "pin:stereo-source" :
						  m_vr.stereo_screen_space    ? (m_process_texture ? "pin:q1" : "pin:fill") :
						  PRIM->FST                   ? "pin:fst" : // 2D, not placed in the scene
													 "shifted";
		if (placed_overlay)
			Console.WriteLn("Draw stereo: overlay placed at Znorm %.4f", m_vr.overlay_znorm);
		Console.WriteLn("Draw stereo: %s z %.0f..%.0f q %.3f..%.3f eqz %d eqq %d fst %d abe %d abcd %d%d%d%d zte %d ztst %d zmsk %d "
						"prim %d tme %d verts %u rect %.0f,%.0f..%.0f,%.0f rt %04x tex %04x",
			why, m_vt.m_min.p.z, m_vt.m_max.p.z, m_vt.m_min.t.z, m_vt.m_max.t.z, m_vt.m_eq.z ? 1 : 0, m_vt.m_eq.q ? 1 : 0,
			PRIM->FST ? 1 : 0, PRIM->ABE ? 1 : 0, static_cast<int>(m_context->ALPHA.A), static_cast<int>(m_context->ALPHA.B),
			static_cast<int>(m_context->ALPHA.C), static_cast<int>(m_context->ALPHA.D), static_cast<int>(m_cached_ctx.TEST.ZTE),
			static_cast<int>(m_cached_ctx.TEST.ZTST), m_cached_ctx.ZBUF.ZMSK ? 1 : 0, static_cast<int>(m_vt.m_primclass),
			PRIM->TME ? 1 : 0, m_vertex->next, m_vt.m_min.p.x, m_vt.m_min.p.y, m_vt.m_max.p.x, m_vt.m_max.p.y,
			rt ? static_cast<u32>(rt->m_TEX0.TBP0) : 0xffffu, PRIM->TME ? static_cast<u32>(m_cached_ctx.TEX0.TBP0) : 0xffffu);
	}
}

// Between frames: the host's stereo settings take effect (GSVolumetric::ApplyStereo), and a switch
// between Volumetric (2 layers) and Flat (1 layer, drawn as upstream draws) rebuilds every target
// with the new layer count. The texture cache is written back to GS memory and dropped, and the
// next frame recreates its targets from that memory, as after a change of the upscale multiplier.
void GSRendererHW::VolumetricSetOutput(int index, GSTextureCache::Target* rt)
{
	m_vr.output_rt[index] = rt;
}

void GSRendererHW::VolumetricEndVSync()
{
	// Overlays not drawn for a few vsyncs are gone (VolumetricClassifyDraw).
	m_vr.overlay_vsync++;
	std::erase_if(m_vr.overlay_seen, [v = m_vr.overlay_vsync](const auto& e) { return v - e.second.second > 8; });

	GSVolumetric::ApplyStereo();
	const GSVolumetric::StereoSettings& stereo = GSVolumetric::GetStereo();
	const u32 layers = (stereo.enabled && stereo.multiview) ? 2 : 1;
	if (layers == g_gs_device->GetStereoLayers() || !g_gs_device->SupportsStereoLayers(layers))
		return;

	Common::Timer timer;
	ReadbackTextureCache();
	PurgeTextureCache(true, true, true);
	m_vr.output_rt = {};
	m_vr.rt_stereo_eye = {};
	m_vr.rt_stereo_eye_next = 0;
	g_gs_device->ClearCurrent();
	g_gs_device->PurgePool();
	if (!g_gs_device->SetStereoLayers(layers))
	{
		Console.Error("3D: the GS could not switch to %u layer(s).", layers);
		return;
	}
	// The pipeline key list and its background builds are per layer count (the shader header differs).
	g_gs_device->SetGameIdentity(VMManager::GetDiscSerial(), VMManager::GetDiscCRC());
	Console.WriteLn("3D: the GS now draws %s (%u layer(s)), switched in %.0f ms.",
		(layers > 1) ? "both eyes" : "flat", layers, timer.GetTimeMilliseconds());
}
