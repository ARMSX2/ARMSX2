// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSPassScheduler.h"

#include "common/Assertions.h"

#include <algorithm>
#include <cstring>

GSPassScheduler::GSPassScheduler() = default;

GSPassScheduler::~GSPassScheduler() = default;

bool GSPassScheduler::IsDeferrable(const GSHWDrawConfig& config)
{
	// No target to group by.
	if (!config.rt)
		return false;

	// A barrier means the draw reads what it (or the previous prim) just wrote, so its
	// position in the stream is load-bearing.
	if (config.require_one_barrier || config.require_full_barrier)
		return false;

	// Same reason, expressed through the sampler instead: the draw samples its own colour
	// or depth attachment.
	if (config.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE)
		return false;
	if (config.ps.IsFeedbackLoopRT() || config.ps.IsFeedbackLoopDepth() || config.ps.tex_is_fb)
		return false;
	if (config.tex && (config.tex == config.rt || config.tex == config.ds))
		return false;

	// Destination alpha testing carries stencil or primitive-ID state between passes.
	if (config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off)
		return false;

	// Colour clipping is a cross-draw state machine (colclip_mode and colclip_update_area
	// survive ResetStates deliberately), so its draws must stay where they are.
	if (config.colclip_mode != GSHWDrawConfig::ColClipMode::NoModify)
		return false;

	// The second passes are not render passes: they are extra draws the backend issues
	// inside this one RenderHW call, into the same attachments, with different pipeline
	// state and the same geometry. Nothing new is sampled and the render pass stays open,
	// so they travel with the record. Only their own barrier requests are disqualifying,
	// for the same reason the primary one is above.
	if (config.alpha_second_pass.enable &&
		(config.alpha_second_pass.require_one_barrier || config.alpha_second_pass.require_full_barrier))
		return false;

	// A drawlist means the backend splits the draw per primitive group for barriers; it is
	// only ever set alongside require_full_barrier, but reject it explicitly since the
	// pointer aims at GSRendererHW's per-draw vectors.
	if (config.drawlist || config.drawlist_bbox)
		return false;

	return true;
}

bool GSPassScheduler::IsAttachmentOfAnyRun(const GSTexture* tex) const
{
	if (!tex)
		return false;

	for (u32 i = 0; i < m_run_count; i++)
	{
		if (m_runs[i].rt == tex || m_runs[i].ds == tex)
			return true;
	}
	return false;
}

bool GSPassScheduler::References(const GSTexture* tex) const
{
	return IsAttachmentOfAnyRun(tex) || WasSampled(tex);
}

bool GSPassScheduler::WasSampled(const GSTexture* tex) const
{
	return tex && std::find(m_sampled.begin(), m_sampled.end(), tex) != m_sampled.end();
}

bool GSPassScheduler::NoteSampled(GSTexture* tex)
{
	if (!tex || WasSampled(tex))
		return true;
	if (m_sampled.size() >= MAX_SAMPLED)
		return false;

	m_sampled.push_back(tex);
	return true;
}

bool GSPassScheduler::ConflictsWithQueued(const GSHWDrawConfig& config) const
{
	// Read-after-write: this draw samples something an already-queued draw writes.
	// Reordering could move that write in front of this read.
	if (IsAttachmentOfAnyRun(config.tex) || IsAttachmentOfAnyRun(config.pal))
		return true;

	// Write-after-read: this draw writes something an already-queued draw samples.
	// Reordering could move this write in front of that read.
	//
	// This deliberately fires even when the draw is joining the run that already owns the
	// attachment: the queued reader may live in the *other* run, and reordering this write
	// ahead of it would show it the wrong contents.
	if (WasSampled(config.rt) || WasSampled(config.ds))
		return true;

	return false;
}

GSPassScheduler::Disposition GSPassScheduler::TryEnqueue(const GSHWDrawConfig& config)
{
	pxAssert(IsDeferrable(config));

	if (ConflictsWithQueued(config))
		return Disposition::NeedsFlush;

	// Find the run this draw belongs to. A render pass is defined by its attachments, so
	// that pair is the whole key.
	Run* run = nullptr;
	for (u32 i = 0; i < m_run_count; i++)
	{
		if (m_runs[i].rt == config.rt && m_runs[i].ds == config.ds)
		{
			run = &m_runs[i];
			break;
		}
	}

	if (!run)
	{
		if (m_run_count >= MAX_RUNS)
			return Disposition::NeedsFlush;

		// Runs get reordered against each other, so they must not share an attachment. A
		// partial match - same depth buffer, different colour target, say - is exactly the
		// aliasing write this cannot allow.
		if (IsAttachmentOfAnyRun(config.rt) || IsAttachmentOfAnyRun(config.ds))
			return Disposition::NeedsFlush;

		run = &m_runs[m_run_count++];
		run->rt = config.rt;
		run->ds = config.ds;
		pxAssert(run->records.empty());

		// GSTexture::State is not private to the backend: the texture cache reads it to decide
		// whether a target has been written yet, and an undeferred draw would have flipped it
		// to Dirty on the spot, when the backend picked the attachment's load op. Do that here
		// so the deferral window is invisible, and stash the original to hand back at emit -
		// the backend still needs to see Cleared or Invalidated to get the load op right.
		run->rt_state = run->rt->GetState();
		run->rt->SetStateForDeferral(GSTexture::State::Dirty);
		if (run->ds)
		{
			run->ds_state = run->ds->GetState();
			run->ds->SetStateForDeferral(GSTexture::State::Dirty);
		}
	}

	const size_t vertex_bytes = sizeof(GSVertex) * config.nverts;
	const size_t index_bytes = sizeof(u16) * config.nindices;

	if (m_queued >= MAX_DRAWS)
		return Disposition::NeedsFlush;
	if ((m_vertices.size() * sizeof(GSVertex)) + vertex_bytes > MAX_VERTEX_BYTES)
		return Disposition::NeedsFlush;
	if ((m_indices.size() * sizeof(u16)) + index_bytes > MAX_INDEX_BYTES)
		return Disposition::NeedsFlush;

	// Record what this draw reads before committing it, so a full sampled set forces a flush
	// rather than silently dropping a hazard.
	if (!NoteSampled(config.tex) || !NoteSampled(config.pal))
		return Disposition::NeedsFlush;

	Record& rec = run->records.emplace_back();
	rec.config = config;
	rec.vertex_offset = static_cast<u32>(m_vertices.size());
	rec.index_offset = static_cast<u32>(m_indices.size());

	// GSState reuses its vertex and index buffers for the very next draw, so the geometry
	// has to be taken by value here, not by pointer.
	m_vertices.resize(m_vertices.size() + config.nverts);
	std::memcpy(m_vertices.data() + rec.vertex_offset, config.verts, vertex_bytes);

	m_indices.resize(m_indices.size() + config.nindices);
	std::memcpy(m_indices.data() + rec.index_offset, config.indices, index_bytes);

	m_queued++;
	return Disposition::Queued;
}

void GSPassScheduler::Emit(GSDevice* dev)
{
	// The runs are independent by construction - anything that could have made them
	// dependent flushed on the way in - so emitting them back to back is free to pick an
	// order. Each run's draws land contiguously, which is what collapses them into one
	// render pass.
	for (u32 i = 0; i < m_run_count; i++)
	{
		// Rewind the attachment state to what the first draw would have found, so the backend
		// picks the same load op it would have picked undeferred. It sets Dirty again itself.
		m_runs[i].rt->SetStateForDeferral(m_runs[i].rt_state);
		if (m_runs[i].ds)
			m_runs[i].ds->SetStateForDeferral(m_runs[i].ds_state);

		// Resolve the geometry pointers only now: the vectors have finished growing, so
		// data() is finally stable.
		for (Record& rec : m_runs[i].records)
		{
			rec.config.verts = m_vertices.data() + rec.vertex_offset;
			rec.config.indices = m_indices.data() + rec.index_offset;
			dev->DoRenderHW(rec.config);
		}
	}

	Clear();
}

void GSPassScheduler::Clear()
{
	// Keep the capacity - see the note on the members.
	for (u32 i = 0; i < m_run_count; i++)
	{
		m_runs[i].records.clear();
		m_runs[i].rt = nullptr;
		m_runs[i].ds = nullptr;
	}

	m_run_count = 0;
	m_queued = 0;
	m_sampled.clear();
	m_vertices.clear();
	m_indices.clear();
}
