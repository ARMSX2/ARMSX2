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
		u32 feedback_inpass = 0; // sampled its own pass, served by the core proof
		u32 uploads_free = 0;    // transfers landing outside the open pass
		u32 cpu_read_pages = 0;
		u32 draws_in_biggest_pass = 0;
	};

	bool IsActive() const { return m_active; }
	void SetActive(bool active) { m_active = active; }

	// --- per-draw ------------------------------------------------------------
	// fb/z: the draw's write footprints (empty when not written). tex: the sampled
	// window, all mip levels. core: the proven sampled core (empty when no proof
	// holds). Returns true if the draw broke the open pass.
	bool OnDraw(const GSPageBitmap& fb_written, const GSPageBitmap& z_written, const GSPageBitmap& read_deps,
		const GSPageBitmap& tex, const GSPageBitmap& core, bool has_core, u32 fb_bp, u32 fb_psm, u32 z_bp,
		u32 z_psm, bool z_used, u32 verts)
	{
		bool broke = false;
		m_frame.draws++;
		m_frame.verts += verts;

		// Texture-feedback hazard against everything the open pass wrote so far.
		if (!tex.empty() && tex.intersects(m_pass_written))
		{
			if (has_core && !core.intersects(m_pass_written))
			{
				m_frame.feedback_inpass++;
			}
			else
			{
				// Snapshot the hazard: the pages this window needs that the pass wrote.
				GSPageBitmap hazard = tex;
				hazard &= m_pass_written;
				m_frame.snapshots++;
				m_frame.snapshot_pages += hazard.count();
				EndPass(BreakReason::Feedback);
				broke = true;
			}
		}

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

		m_pass_written |= fb_written;
		m_pass_written |= z_written;
		m_pass_read |= read_deps;
		m_pass_read |= tex;
		return broke;
	}

	// --- non-draw stream events ----------------------------------------------
	void OnUpload(const GSPageBitmap& pages)
	{
		if (pages.intersects(m_pass_written) || pages.intersects(m_pass_read))
			EndPass(BreakReason::Upload);
		else
			m_frame.uploads_free++;
	}

	void OnMove(const GSPageBitmap& src, const GSPageBitmap& dst)
	{
		if (src.intersects(m_pass_written) || dst.intersects(m_pass_written) || dst.intersects(m_pass_read))
			EndPass(BreakReason::MoveHazard);
		else
			m_frame.uploads_free++;
	}

	void OnCpuRead(const GSPageBitmap& pages)
	{
		if (pages.intersects(m_pass_written))
		{
			m_frame.cpu_read_pages += pages.count();
			EndPass(BreakReason::CpuRead);
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
		m_pass_written = {};
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
	GSPageBitmap m_pass_written;
	GSPageBitmap m_pass_read;
	TargetPair m_targets[kMaxTargetPairs] = {};
	u32 m_target_count = 0;
	u32 m_draws_in_pass = 0;
	FrameStats m_frame;
	std::vector<FrameStats> m_frames;
};
