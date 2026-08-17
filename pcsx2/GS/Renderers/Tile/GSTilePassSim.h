// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Tile/GSPageBitmap.h"

#include <algorithm>
#include <vector>

// Offline pass-structure simulator (EmuCore/GS/TilePassSim, gsrunner -tilepasssim).
//
// Answers one design question per corpus dump: if a renderer kept every draw on the
// GPU timeline and broke a render pass ONLY where GS semantics force it to, how many
// passes, snapshots and syncs would a frame carry? It renders nothing and changes
// nothing — it is an accounting observer fed the same footprints the model uses —
// and its output is the per-title arithmetic for the tiler-native design brief
// (pass count x measured pass-break cost, snapshot count x measured copy cost).
//
// The pass model it scores, deliberately idealized:
//  - A pass holds up to kMaxTargetPairs FRAME/ZBUF surface pairs (paraLLEl-GS ships
//    8); switching targets inside the budget does not break the pass.
//  - Blending and destination reads are in-pass (the raster-order-access design), so
//    neither blending nor same-coordinate feedback breaks anything. A textured draw
//    whose sampled CORE (the vertex UV bbox shrunk one texel, the in-tree feedback
//    admission's own proof) avoids every page the open pass wrote is in-pass too.
//  - A draw that samples pages the open pass wrote OUTSIDE that proof takes a
//    SNAPSHOT: end the pass, copy the hazard pages, continue. Counted as both a
//    break and a snapshot.
//  - An upload into pages the open pass read or wrote breaks it (the stream's order
//    must stay visible); an upload into untouched pages is free (pre-pass upload).
//  - A CPU read of GPU-written pages breaks and counts as a genuine readback — that
//    consumer exists in every design.
//
// Everything here is a MINIMUM under this model: a real renderer only adds breaks.
class GSTilePassSim
{
public:
	static constexpr u32 kMaxTargetPairs = 8;

	enum class BreakReason : u32
	{
		Feedback,     // sampled pages the open pass wrote, core proof failed -> snapshot
		TargetCap,    // draw needed a target pair past the per-pass budget
		Upload,       // transfer into pages the open pass touched
		MoveHazard,   // local->local copy touching pass pages
		CpuRead,      // CPU consumer of pass-written pages (genuine readback)
		FrameEnd,     // vsync closes the open pass
		Count
	};

	struct FrameStats
	{
		u32 draws = 0;
		u32 verts = 0;
		u32 passes = 0;
		u32 snapshots = 0;
		u32 snapshot_pages = 0;
		u32 breaks[static_cast<u32>(BreakReason::Count)] = {};
		u32 feedback_inpass = 0;  // sampled its own pass, served by the core proof
		u32 feedback_identity = 0; // sampled its own pass pixel-to-pixel: the raster-order
		                           // in-pass read serves it (the fbfetch shape) — no break
		u32 uploads_free = 0;        // transfers landing outside the open pass
		u32 uploads_versionable = 0; // transfers into pages the pass only READ: a fresh
		                             // texture version serves them without a break
		u32 uploads_drawable = 0;    // transfers over pages the pass WROTE: realized as
		                             // an in-pass draw from fresh staging — no break
		u32 palette_gathers = 0; // CLUT loads of pass-written pages: on-GPU gathers in the
		                         // design (a break, never a CPU sync) — split from CpuRead
		u32 cpu_read_pages = 0;  // non-CLUT CPU consumers: genuine readbacks in any design
		u32 feedback_runs = 0; // consecutive-hazard runs: the break count under the
		                       // go-for-broke stale-snapshot policy (offset reads within a
		                       // run served from the run's first snapshot — Classic-grade
		                       // staleness, one break per run instead of one per draw)
		u32 feedback_fst = 0;  // offset-feedback draws with FST coordinates (the residue's
		u32 feedback_stq = 0;  // shape: fixed-coordinate vs perspective sampling)
		u32 draws_in_biggest_pass = 0;
	};

	bool IsActive() const { return m_active; }
	void SetActive(bool active) { m_active = active; }

	// --- per-draw ------------------------------------------------------------
	// fb/z: the draw's write footprints (empty when not written). tex: the sampled
	// window, all mip levels. core: the proven sampled core (empty when no proof
	// holds). Returns true if the draw broke the open pass.
	bool OnDraw(const GSPageBitmap& fb_written, const GSPageBitmap& z_written, const GSPageBitmap& read_deps,
		const GSPageBitmap& tex, const GSPageBitmap& core, bool has_core, bool identity_feedback, bool fst,
		u32 fb_bp, u32 fb_psm, u32 z_bp, u32 z_psm, bool z_used, u32 verts)
	{
		bool broke = false;
		bool hazard_draw = false;
		m_frame.draws++;
		m_frame.verts += verts;

		// Texture-feedback hazard: only pages DRAWS wrote count. Pages an in-pass
		// upload wrote are always servable from the upload's own staging (a fresh
		// texture version — the bytes never existed only in tile memory).
		if (!tex.empty() && tex.intersects(m_pass_draw_written))
		{
			if (has_core && !core.intersects(m_pass_draw_written))
			{
				m_frame.feedback_inpass++;
			}
			else if (identity_feedback)
			{
				m_frame.feedback_identity++;
			}
			else
			{
				// Snapshot the hazard: the pages this window needs that draws wrote.
				GSPageBitmap hazard = tex;
				hazard &= m_pass_draw_written;
				m_frame.snapshots++;
				m_frame.snapshot_pages += hazard.count();
				if (fst)
					m_frame.feedback_fst++;
				else
					m_frame.feedback_stq++;
				if (!m_last_draw_was_hazard)
					m_frame.feedback_runs++;
				hazard_draw = true;
				EndPass(BreakReason::Feedback);
				broke = true;
			}
		}
		m_last_draw_was_hazard = hazard_draw;

		// Target-pair budget.
		if (!AddTarget(fb_bp, fb_psm, false) || (z_used && !AddTarget(z_bp, z_psm, true)))
		{
			EndPass(BreakReason::TargetCap);
			broke = true;
			AddTarget(fb_bp, fb_psm, false);
			if (z_used)
				AddTarget(z_bp, z_psm, true);
		}

		if (m_draws_in_pass == 0)
			m_frame.passes++;
		m_draws_in_pass++;
		m_frame.draws_in_biggest_pass = std::max(m_frame.draws_in_biggest_pass, m_draws_in_pass);

		m_pass_draw_written |= fb_written;
		m_pass_draw_written |= z_written;
		m_pass_read |= read_deps;
		m_pass_read |= tex;
		return broke;
	}

	// --- non-draw stream events ----------------------------------------------
	void OnUpload(const GSPageBitmap& pages)
	{
		// An upload never has to break the pass: into untouched pages it hoists ahead
		// of the pass outright; into read-only pages a fresh texture version serves
		// later samplers; over pass-WRITTEN pages it is realized as an in-pass draw
		// from fresh staging (the bytes never existed on the GPU, so nothing needs
		// flushing first). The three counters price the three mechanisms.
		if (pages.intersects(m_pass_draw_written))
		{
			m_frame.uploads_drawable++;
			m_pass_upload_written |= pages; // bytes stay CPU/staging-visible by construction
		}
		else if (pages.intersects(m_pass_read))
			m_frame.uploads_versionable++;
		else
		{
			m_frame.uploads_free++;
			m_pass_upload_written |= pages;
		}
	}

	void OnMove(const GSPageBitmap& src, const GSPageBitmap& dst)
	{
		// The design realizes a move as a textured draw targeting dst. Its source read
		// is an OFFSET read by construction (a move that copies nowhere is a no-op), so
		// a source the pass wrote is the snapshot shape; the destination side follows
		// the upload rules above.
		if (src.intersects(m_pass_draw_written))
		{
			GSPageBitmap hazard = src;
			hazard &= m_pass_draw_written;
			m_frame.snapshots++;
			m_frame.snapshot_pages += hazard.count();
			EndPass(BreakReason::MoveHazard);
			m_pass_draw_written |= dst; // the move-draw's output exists only on the GPU
			return;
		}
		if (dst.intersects(m_pass_draw_written))
			m_frame.uploads_drawable++;
		else if (dst.intersects(m_pass_read))
			m_frame.uploads_versionable++;
		else
			m_frame.uploads_free++;
		m_pass_draw_written |= dst;
	}

	void OnCpuRead(const GSPageBitmap& pages, bool clut)
	{
		// Only draw-written pages force anything: an upload's bytes are CPU truth
		// already, so a CPU read of upload-written-only pages is free.
		if (pages.intersects(m_pass_draw_written))
		{
			if (clut)
			{
				// The design gathers a rendered palette on-GPU: a pass break at most,
				// never a CPU sync. Split out so GT4's seventy-per-frame class reads
				// as what it is.
				m_frame.palette_gathers++;
				EndPass(BreakReason::CpuRead);
			}
			else
			{
				m_frame.cpu_read_pages += pages.count();
				EndPass(BreakReason::CpuRead);
			}
		}
	}

	void OnVSync()
	{
		if (m_draws_in_pass != 0)
			EndPass(BreakReason::FrameEnd);
		m_frames.push_back(m_frame);
		m_frame = {};
	}

	const std::vector<FrameStats>& Frames() const { return m_frames; }

private:
	struct TargetPair
	{
		u32 bp;
		u32 psm;
		bool depth;
	};

	void EndPass(BreakReason reason)
	{
		if (m_draws_in_pass == 0 && reason != BreakReason::FrameEnd)
			return; // an empty pass cannot break
		m_frame.breaks[static_cast<u32>(reason)]++;
		m_pass_draw_written = {};
		m_pass_upload_written = {};
		m_pass_read = {};
		m_target_count = 0;
		m_draws_in_pass = 0;
	}

	bool AddTarget(u32 bp, u32 psm, bool depth)
	{
		for (u32 i = 0; i < m_target_count; i++)
		{
			const TargetPair& t = m_targets[i];
			if (t.bp == bp && t.psm == psm && t.depth == depth)
				return true;
		}
		if (m_target_count == kMaxTargetPairs)
			return false;
		m_targets[m_target_count++] = {bp, psm, depth};
		return true;
	}

	bool m_active = false;
	GSPageBitmap m_pass_draw_written;   // written by draws: exists only in tile memory/GPU
	GSPageBitmap m_pass_upload_written; // written by in-pass uploads: staging holds the bytes
	GSPageBitmap m_pass_read;
	bool m_last_draw_was_hazard = false;
	TargetPair m_targets[kMaxTargetPairs] = {};
	u32 m_target_count = 0;
	u32 m_draws_in_pass = 0;
	FrameStats m_frame;
	std::vector<FrameStats> m_frames;
};
