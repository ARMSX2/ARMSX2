// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRunsPolicy.h"

#include <cstring>
#include <vector>

/// Twins held back until something could observe them, so that later bases on the same targets are drawn ahead of
/// them. GSDevice::DrawRow draws every base at once and remembers its twin key; a row that is the twin of the remembered
/// base, with nothing drawn between them, is held. A later base on the same colour target and depth buffer that reads
/// neither is drawn ahead of the held twins. Every device entry point that could see a target or a texture a held twin
/// uses emits the twins first, in arrival order (GSDevice::FlushDeferredDraws, the framework the render-pass scheduler
/// introduced), and forgets the base.
///
/// Exact where the bases test GREATER: a moved twin meets a later base's depth only
/// where that base passed, strictly nearer than the twin's own depth, so the twin fails there, as the base would have
/// covered it in the game's order. A twin needs the depth at its pixels to be its base's at its turn, so it must follow
/// its base with nothing between them: anything that could touch the base's targets flushes, and the flush forgets the
/// base.
class GSRowQueue
{
public:
	static constexpr u32 MAX_ROWS = 8184;
	static constexpr u32 MAX_VERTICES = 256 * 1024;
	static constexpr u32 MAX_INDICES = 768 * 1024;

	__fi u32 Count() const { return static_cast<u32>(m_rows.size()); }

	/// The held twins, plus one for a remembered base: what the device counts as deferred, so that every entry point
	/// that could touch the base's targets flushes and forgets it.
	__fi u32 Pending() const { return Count() + m_base; }

	/// Whether a held twin draws into, or samples, this texture, or it is the remembered base's target.
	bool References(const GSTexture* tex) const
	{
		if (!tex || (m_rows.empty() && !m_base))
			return false;
		return tex == m_rt || tex == m_ds || tex->m_row_queue_mark == m_generation;
	}

	/// Whether the row is the twin of the remembered base: an allowed twin state, the base's targets, and the same
	/// primitives, positions, scissor and vertex transform. The depth floor (Adreno) may differ between them: the base
	/// floors the depth it tests and writes and the twin tests its own unfloored depth, and both orders still agree,
	/// since a later base that passed holds a floored depth above the earlier base's floor, so above the twin's depth too.
	bool IsTwin(const GSHWDrawConfig& c) const
	{
		if (!m_base || !CanBeTwin(c) || c.rt != m_rt || c.ds != m_ds)
			return false;
		const Key& b = m_key;
		if (c.nverts != b.nverts || c.nindices != b.nindices || c.topology != b.topology || c.vs.key != b.vs_key ||
			!c.scissor.eq(b.scissor) || std::memcmp(&c.cb_vs.vertex_scale, &b.vertex_scale, sizeof(GSVector2)) != 0 ||
			std::memcmp(&c.cb_vs.vertex_offset, &b.vertex_offset, sizeof(GSVector2)) != 0 ||
			c.cb_vs.max_depth != b.max_depth ||
			std::memcmp(c.indices, m_base_indices.data(), c.nindices * sizeof(u16)) != 0)
			return false;
		for (u32 v = 0; v < c.nverts; v++)
		{
			if (c.verts[v].XYZ.U64 != m_base_xyz[v])
				return false;
		}
		return true;
	}

	/// Whether a base may be drawn ahead of the held twins: it draws into their targets and reads neither.
	bool MayGoAhead(const GSHWDrawConfig& c) const
	{
		return !m_rows.empty() && c.rt == m_rt && c.ds == m_ds && !ReadsTargets(c);
	}

	/// A base drawn ahead of the held twins: every one of them is now moved.
	void NoteAhead() { m_moved = Count(); }

	/// Remembers a base just drawn, for its twin. Copies its positions and indices: they live in GSState's per-draw
	/// buffers, which the next draw overwrites.
	void RememberBase(const GSHWDrawConfig& c)
	{
		m_base = true;
		m_rt = c.rt;
		m_ds = c.ds;
		m_key.nverts = c.nverts;
		m_key.nindices = c.nindices;
		m_key.topology = c.topology;
		m_key.vs_key = c.vs.key;
		m_key.scissor = c.scissor;
		m_key.vertex_scale = c.cb_vs.vertex_scale;
		m_key.vertex_offset = c.cb_vs.vertex_offset;
		m_key.max_depth = c.cb_vs.max_depth;
		m_base_xyz.resize(c.nverts);
		for (u32 v = 0; v < c.nverts; v++)
			m_base_xyz[v] = c.verts[v].XYZ.U64;
		m_base_indices.assign(c.indices, c.indices + c.nindices);
	}

	/// Holds the twin of the remembered base (its configuration, vertices and indices are copied) and forgets the base.
	/// False when the queue is full.
	bool Hold(const GSHWDrawConfig& config, GSRowDeviceCache& cache)
	{
		if (m_rows.size() == MAX_ROWS || m_vertices.size() + config.nverts > MAX_VERTICES ||
			m_indices.size() + config.nindices > MAX_INDICES)
			return false;
		// Built from the configuration in place: a default-constructed record would zero both constant buffers first.
		m_rows.emplace_back(config, &cache, static_cast<u32>(m_vertices.size()), static_cast<u32>(m_indices.size()));
		m_vertices.insert(m_vertices.end(), config.verts, config.verts + config.nverts);
		m_indices.insert(m_indices.end(), config.indices, config.indices + config.nindices);
		Note(config.tex);
		Note(config.pal);
		m_base = false;
		return true;
	}

	/// Draws every held twin through `draw(config, cache)` in arrival order, empties the queue and forgets the base.
	template <typename Draw>
	void Emit(Draw&& draw)
	{
		const u32 n = Count();
		for (u32 i = 0; i < n; i++)
		{
			Record& r = m_rows[i];
			r.config.verts = m_vertices.data() + r.vfirst;
			r.config.indices = m_indices.data() + r.ifirst;
			draw(r.config, *r.cache);
		}
		s_emitted += n;
		s_moved += m_moved;
		Clear();
	}

	void Clear()
	{
		if (!m_rows.empty())
		{
			m_rows.clear();
			m_vertices.clear();
			m_indices.clear();
			m_generation++;
		}
		m_moved = 0;
		m_base = false;
		m_rt = m_ds = nullptr;
	}

	/// Twins emitted, and of those drawn after at least one later base, by every queue since the process started
	/// (gsrunner reads them after the device is gone).
	static u64 Emitted() { return s_emitted; }
	static u64 Moved() { return s_moved; }

	/// The base of a twin pair: plain full-colour writes (blending elided), depth written, the GREATER test, no alpha test.
	static bool CanBeTwinBase(const GSHWDrawConfig& c)
	{
		return c.depth.zwe && c.depth.ztst == ZTST_GREATER && GSRunsPolicy::Way(c) == 2 &&
		       c.alpha_test == GSHWDrawConfig::AlphaTestMode::NONE && !c.alpha_second_pass.enable;
	}

	/// The twin drawn over its base: no depth written, a depth test its base's depth passes (GEQUAL), no alpha test.
	static bool CanBeTwin(const GSHWDrawConfig& c)
	{
		return !c.depth.zwe && c.depth.ztst == ZTST_GEQUAL && c.alpha_test == GSHWDrawConfig::AlphaTestMode::NONE &&
		       !c.alpha_second_pass.enable;
	}

	/// Whether the draw reads its colour target or depth buffer other than through the depth test: a base that does
	/// would see the targets without the held twins.
	static bool ReadsTargets(const GSHWDrawConfig& c)
	{
		return c.require_one_barrier || c.require_full_barrier || c.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE ||
		       c.ps.IsFeedbackLoopRT() || c.ps.IsFeedbackLoopDepth() || c.ps.tex_is_fb || c.ps.HasColorROV() ||
		       c.ps.HasDepthROV() || (c.tex && (c.tex == c.rt || c.tex == c.ds)) ||
		       c.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off ||
		       c.date_copy != GSAlphaBitLogicOp::NoDateCopy || c.road.clone_rt || c.road.rt_loop || c.road.depth_loop ||
		       c.road.depth_read;
	}

private:
	struct Record
	{
		Record(const GSHWDrawConfig& c, GSRowDeviceCache* k, u32 v, u32 i) : config(c), cache(k), vfirst(v), ifirst(i) {}
		GSHWDrawConfig config; ///< verts and indices point into the queue's arrays from Emit on
		GSRowDeviceCache* cache;
		u32 vfirst, ifirst;
	};

	/// The remembered base's fields a twin must equal (positions and indices are kept beside it).
	struct Key
	{
		u32 nverts = 0, nindices = 0;
		GSHWDrawConfig::Topology topology{};
		u16 vs_key = 0;
		GSVector4i scissor;
		GSVector2 vertex_scale, vertex_offset;
		u32 max_depth = 0;
	};

	void Note(GSTexture* t)
	{
		if (t)
			t->m_row_queue_mark = m_generation;
	}

	std::vector<Record> m_rows;
	std::vector<GSVertex> m_vertices;
	std::vector<u16> m_indices;
	Key m_key;
	std::vector<u64> m_base_xyz;
	std::vector<u16> m_base_indices;
	u32 m_moved = 0;    ///< held twins drawn after a later base: the first m_moved
	bool m_base = false; ///< a base was drawn last and its twin may follow
	u64 m_generation = 1; ///< raised whenever held twins are emitted: a texture marked with an older one is not sampled
	GSTexture* m_rt = nullptr;
	GSTexture* m_ds = nullptr;
	static inline u64 s_emitted = 0;
	static inline u64 s_moved = 0;
};
