// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSPassScheduler.h"

#include "common/Assertions.h"

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

	// Multi-pass draws re-read the target between their own passes.
	if (config.alpha_second_pass.enable || config.blend_multi_pass.enable)
		return false;

	// A drawlist means the backend splits the draw per primitive group for barriers; it is
	// only ever set alongside require_full_barrier, but reject it explicitly since the
	// pointer aims at GSRendererHW's per-draw vectors.
	if (config.drawlist || config.drawlist_bbox)
		return false;

	return true;
}

bool GSPassScheduler::MatchesOpenRun(const GSHWDrawConfig& config) const
{
	if (m_records.empty())
		return false;

	// A render pass is defined by its attachments, so that is the whole key.
	const GSHWDrawConfig& open = m_records.back().config;
	return open.rt == config.rt && open.ds == config.ds;
}

bool GSPassScheduler::Enqueue(const GSHWDrawConfig& config)
{
	pxAssert(IsDeferrable(config));

	const size_t vertex_bytes = sizeof(GSVertex) * config.nverts;
	const size_t index_bytes = sizeof(u16) * config.nindices;

	if (m_records.size() >= MAX_DRAWS)
		return false;
	if ((m_vertices.size() * sizeof(GSVertex)) + vertex_bytes > MAX_VERTEX_BYTES)
		return false;
	if ((m_indices.size() * sizeof(u16)) + index_bytes > MAX_INDEX_BYTES)
		return false;

	Record& rec = m_records.emplace_back();
	rec.config = config;
	rec.vertex_offset = static_cast<u32>(m_vertices.size());
	rec.index_offset = static_cast<u32>(m_indices.size());

	// GSState reuses its vertex and index buffers for the very next draw, so the geometry
	// has to be taken by value here, not by pointer.
	m_vertices.resize(m_vertices.size() + config.nverts);
	std::memcpy(m_vertices.data() + rec.vertex_offset, config.verts, vertex_bytes);

	m_indices.resize(m_indices.size() + config.nindices);
	std::memcpy(m_indices.data() + rec.index_offset, config.indices, index_bytes);

	return true;
}

void GSPassScheduler::Emit(GSDevice* dev)
{
	// Resolve the geometry pointers only now: the vectors have finished growing, so
	// data() is finally stable.
	for (Record& rec : m_records)
	{
		rec.config.verts = m_vertices.data() + rec.vertex_offset;
		rec.config.indices = m_indices.data() + rec.index_offset;
		dev->DoRenderHW(rec.config);
	}

	Clear();
}

void GSPassScheduler::Clear()
{
	// Keep the capacity - see the note on the members.
	m_records.clear();
	m_vertices.clear();
	m_indices.clear();
}
