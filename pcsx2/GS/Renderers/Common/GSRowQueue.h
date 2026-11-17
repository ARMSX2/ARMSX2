// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRunCompiler.h"

#include <vector>

/// The rows of the open run, held back until something could observe them. GSDevice::DrawRow enqueues; every device
/// entry point that could see a target or a texture a row uses emits the queue first (GSDevice::FlushDeferredDraws,
/// the framework the render-pass scheduler introduced), so holding rows back is invisible. At emit the pass compiler
/// orders them (the twin sort) and the backend draws them in that order.
///
/// One run: every row has the same colour target and depth buffer. A row for another pair emits the queue first.
class GSRowQueue
{
public:
	static constexpr u32 MAX_ROWS = 8184;
	static constexpr u32 MAX_VERTICES = 256 * 1024;
	static constexpr u32 MAX_INDICES = 768 * 1024;

	__fi u32 Count() const { return static_cast<u32>(m_rows.size()); }
	__fi bool Empty() const { return m_rows.empty(); }

	/// Whether a queued row draws into, or samples, this texture.
	bool References(const GSTexture* tex) const
	{
		if (m_rows.empty() || !tex)
			return false;
		return tex == m_rt || tex == m_ds || tex->m_row_queue_mark == m_generation;
	}

	/// Whether holding this row back can change anything: only a window of twin pairs is reordered, so the queue takes
	/// a row that can be a twin base, or a row after a base that can be its twin. Anything else is drawn at once, after
	/// the queue: holding it would cost a copy of its configuration and geometry and buy nothing.
	bool Wants(const GSHWDrawConfig& config) const
	{
		if (CanBeTwinBase(config))
			return true;
		return !m_rows.empty() && m_compiler_rows.back().role == GSRunCompiler::ROLE_BASE && CanBeTwin(config);
	}

	/// Copies the row (its vertices and indices too: they live in GSState's per-draw buffers). False when the queue must
	/// be emitted first: another target pair, or full.
	bool Enqueue(const GSHWDrawConfig& config, GSRowDeviceCache& cache, bool sort_twins)
	{
		if (!m_rows.empty() && (config.rt != m_rt || config.ds != m_ds))
			return false;
		if (m_rows.size() == MAX_ROWS || m_vertices.size() + config.nverts > MAX_VERTICES ||
			m_indices.size() + config.nindices > MAX_INDICES)
			return false;
		if (m_rows.empty())
		{
			m_rt = config.rt;
			m_ds = config.ds;
		}
		// Built from the configuration in place: a default-constructed record would zero both constant buffers first
		// (the RG 476H's back thread spent half of Enqueue on that).
		m_rows.emplace_back(config, &cache, static_cast<u32>(m_vertices.size()), static_cast<u32>(m_indices.size()));
		m_vertices.insert(m_vertices.end(), config.verts, config.verts + config.nverts);
		m_indices.insert(m_indices.end(), config.indices, config.indices + config.nindices);
		Note(config.tex);
		Note(config.pal);

		GSRunCompiler::Row& c = m_compiler_rows.emplace_back();
		c.pinned = config.alpha_second_pass.enable;
		c.role = sort_twins ? Role(m_rows.size() - 1) : GSRunCompiler::ROLE_NONE;
		return true;
	}

	/// Draws every row through `draw(config, cache)` in the compiled order and empties the queue.
	template <typename Draw>
	void Emit(Draw&& draw, bool sort_twins)
	{
		const u32 n = Count();
		if (n == 0)
			return;
		m_order.resize(n);
		GSRunCompiler::SortTwins(m_compiler_rows.data(), n, m_order.data(), sort_twins);
		for (u32 k = 0; k < n; k++)
		{
			const u32 i = m_order[k];
			s_moved += (i != k);
			Record& r = m_rows[i];
			r.config.verts = m_vertices.data() + r.vfirst;
			r.config.indices = m_indices.data() + r.ifirst;
			draw(r.config, *r.cache);
		}
		s_emitted += n;
		Clear();
	}

	void Clear()
	{
		m_rows.clear();
		m_compiler_rows.clear();
		m_vertices.clear();
		m_indices.clear();
		m_generation++;
		m_rt = m_ds = nullptr;
	}

	/// Rows emitted, and moved by the twin sort, by every queue since the process started (gsrunner reads them after
	/// the device is gone).
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

private:
	struct Record
	{
		Record(const GSHWDrawConfig& c, GSRowDeviceCache* k, u32 v, u32 i) : config(c), cache(k), vfirst(v), ifirst(i) {}
		GSHWDrawConfig config; ///< verts and indices point into the queue's arrays from Emit on
		GSRowDeviceCache* cache;
		u32 vfirst, ifirst;
	};

	void Note(GSTexture* t)
	{
		if (t)
			t->m_row_queue_mark = m_generation;
	}

	/// The row at `i` (just appended) as the twin sort sees it.
	u8 Role(size_t i) const
	{
		const GSHWDrawConfig& c = m_rows[i].config;
		if (i > 0 && m_compiler_rows[i - 1].role == GSRunCompiler::ROLE_BASE && CanBeTwin(c))
		{
			const Record& p = m_rows[i - 1];
			const GSHWDrawConfig& b = p.config;
			// The twin covers exactly its base's pixels at its base's depth: the same primitives, positions, scissor and
			// vertex transform. The depth floor (Adreno) may differ between them: the base floors the depth it tests
			// and writes and the twin tests its own unfloored depth, and both orders still agree, since a later base
			// that passed holds a floored depth above the earlier base's floor, so above the twin's depth too.
			if (c.nverts == b.nverts && c.nindices == b.nindices && c.topology == b.topology && c.vs.key == b.vs.key &&
				c.scissor.eq(b.scissor) &&
				std::memcmp(&c.cb_vs.vertex_scale, &b.cb_vs.vertex_scale, sizeof(GSVector2)) == 0 &&
				std::memcmp(&c.cb_vs.vertex_offset, &b.cb_vs.vertex_offset, sizeof(GSVector2)) == 0 &&
				c.cb_vs.max_depth == b.cb_vs.max_depth &&
				std::memcmp(m_indices.data() + p.ifirst, m_indices.data() + m_rows[i].ifirst, c.nindices * sizeof(u16)) == 0)
			{
				const GSVertex* bv = m_vertices.data() + p.vfirst;
				const GSVertex* cv = m_vertices.data() + m_rows[i].vfirst;
				u32 v = 0;
				while (v < c.nverts && bv[v].XYZ.U64 == cv[v].XYZ.U64)
					v++;
				if (v == c.nverts)
					return GSRunCompiler::ROLE_TWIN;
			}
		}
		return CanBeTwinBase(c) ? GSRunCompiler::ROLE_BASE : GSRunCompiler::ROLE_NONE;
	}

	std::vector<Record> m_rows;
	std::vector<GSRunCompiler::Row> m_compiler_rows;
	std::vector<u32> m_order;
	std::vector<GSVertex> m_vertices;
	std::vector<u16> m_indices;
	u64 m_generation = 1; ///< raised whenever the queue empties: a texture marked with an older one is not sampled
	GSTexture* m_rt = nullptr;
	GSTexture* m_ds = nullptr;
	static inline u64 s_emitted = 0;
	static inline u64 s_moved = 0;
};
