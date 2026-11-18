// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRunsPolicy.h"

#include <cstring>
#include <vector>

/// The order rows are recorded in. Recording order is draw order, and only twins are held: GSDevice::DrawRow records
/// every row as it arrives (a backend may leave it in its open group of the bases' side, recorded before anything else),
/// and remembers a twin base's key; a row that is the twin of the remembered base, with nothing drawn between them, is
/// held by the backend. A later base on the same colour target and depth buffer that reads neither, and that the backend
/// can draw without changing the pass, is recorded ahead of the held twins. Every device entry point that could see a
/// target or a texture a held twin uses records what is pending first: the bases' open group, then the held twins in
/// arrival order (GSDevice::FlushDeferredDraws), and forgets the base.
///
/// Exact where the bases test GREATER: a moved twin meets a later base's depth only
/// where that base passed, strictly nearer than the twin's own depth, so the twin fails there, as the base would have
/// covered it in the game's order. A twin needs the depth at its pixels to be its base's at its turn, so it must follow
/// its base with nothing between them: anything that could touch the base's targets flushes, and the flush forgets the
/// base.
///
/// This class holds the rules and the bookkeeping the device entry points need (what is pending, which textures it
/// uses); the backend holds the rows' data and records them.
class GSRowEmitter
{
public:
	/// Rows held at once at most; the backend refuses to hold more.
	static constexpr u32 MAX_HELD = 8184;

	/// Held twins.
	__fi u32 Held() const { return m_held; }

	/// The held twins, plus one for a remembered base: what the device counts as deferred, so that every entry point
	/// that could touch the base's targets flushes and forgets it.
	__fi u32 Pending() const { return m_held + m_base; }

	/// Whether a pending row draws into, or samples, this texture, or it is the remembered base's target.
	bool References(const GSTexture* tex) const
	{
		if (!tex)
			return false;
		if ((m_held != 0 || m_base) && (tex == m_rt || tex == m_ds))
			return true;
		if (m_group_rt && (tex == m_group_rt || tex == m_group_ds))
			return true;
		return m_marked && tex->m_row_mark == m_generation;
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

	/// Whether a base may be recorded ahead of the held twins: it draws into their targets and reads neither. The
	/// backend must also be able to draw it without changing the pass (GSDevice::RowsMayGoAhead).
	bool MayGoAhead(const GSHWDrawConfig& c) const
	{
		return m_held != 0 && c.rt == m_rt && c.ds == m_ds && !ReadsTargets(c);
	}

	/// A base recorded ahead of the held twins: every one of them is now moved.
	void NoteAhead() { m_moved = m_held; }

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

	/// The row drawn was not a base: a twin may no longer follow.
	void ForgetBase()
	{
		m_base = false;
		if (m_held == 0)
			m_rt = m_ds = nullptr;
	}

	/// The backend holds the twin of the remembered base: its textures are marked and the base is forgotten. The held
	/// twins keep the base's targets.
	void NoteHeld(const GSHWDrawConfig& c)
	{
		m_held++;
		Mark(c.tex);
		Mark(c.pal);
		m_base = false;
	}

	/// A texture a pending row samples that is not a held twin's: a row of a group the backend has not recorded yet.
	__fi void Mark(GSTexture* t)
	{
		if (t)
		{
			t->m_row_mark = m_generation;
			m_marked = true;
		}
	}

	/// The colour target and depth buffer of the backend's open group on the bases' side (nullptr: none open).
	void SetGroupTargets(const GSTexture* rt, const GSTexture* ds)
	{
		m_group_rt = rt;
		m_group_ds = ds;
	}

	/// Everything pending was recorded or dropped: the held twins are counted, the base and the marks forgotten.
	void Clear()
	{
		s_emitted += m_held;
		s_moved += m_moved;
		m_held = 0;
		m_moved = 0;
		m_base = false;
		if (m_marked)
		{
			m_generation++;
			m_marked = false;
		}
		m_rt = m_ds = nullptr;
		m_group_rt = m_group_ds = nullptr;
	}

	/// Twins held and later recorded, and of those recorded after at least one later base, by every emitter since the
	/// process started.
	static u64 Emitted() { return s_emitted; }
	static u64 Moved() { return s_moved; }

	/// The base of a twin pair: plain full-colour writes (blending elided), depth written, the GREATER test, and nothing
	/// that discards a pixel the depth test passed (alpha test, destination alpha test, scan mask, AA1 edges): the twin
	/// may move only where its base's depth is at every pixel it passed.
	static bool CanBeTwinBase(const GSHWDrawConfig& c)
	{
		return c.depth.zwe && c.depth.ztst == ZTST_GREATER && GSRunsPolicy::Way(c) == 2 &&
		       c.alpha_test == GSHWDrawConfig::AlphaTestMode::NONE && !c.alpha_second_pass.enable &&
		       c.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Off && !c.ps.date &&
		       c.ps.atst == GSShader::PS_ATST::NONE && !c.ps.scanmsk && c.ps.aa1 != GSShader::PS_AA1::TRIANGLE_SW_Z;
	}

	/// The twin drawn over its base: no depth written, a depth test its base's depth passes (GEQUAL), no alpha test.
	static bool CanBeTwin(const GSHWDrawConfig& c)
	{
		return !c.depth.zwe && c.depth.ztst == ZTST_GEQUAL && c.alpha_test == GSHWDrawConfig::AlphaTestMode::NONE &&
		       !c.alpha_second_pass.enable;
	}

	/// Whether the draw reads its colour target or depth buffer other than through the depth test: a base that does
	/// would see the targets without the held twins. A blend in a second pass and a logic op read the colour at the
	/// draw's own pixels, where an earlier twin drew before the base in the game's order.
	static bool ReadsTargets(const GSHWDrawConfig& c)
	{
		return c.blend_multi_pass.enable || c.colormask.logic_op != GSAlphaBitLogicOp::Off ||
		       c.require_one_barrier || c.require_full_barrier || c.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE ||
		       c.ps.IsFeedbackLoopRT() || c.ps.IsFeedbackLoopDepth() || c.ps.tex_is_fb || c.ps.HasColorROV() ||
		       c.ps.HasDepthROV() || (c.tex && (c.tex == c.rt || c.tex == c.ds)) ||
		       c.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off ||
		       c.date_copy != GSAlphaBitLogicOp::NoDateCopy || c.road.clone_rt || c.road.rt_loop || c.road.depth_loop ||
		       c.road.depth_read;
	}

private:
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

	Key m_key;
	std::vector<u64> m_base_xyz;
	std::vector<u16> m_base_indices;
	u32 m_held = 0;
	u32 m_moved = 0;     ///< held twins recorded after a later base: the first m_moved
	bool m_base = false; ///< a base was drawn last and its twin may follow
	bool m_marked = false; ///< a texture carries the current generation
	u64 m_generation = 1; ///< raised whenever marked rows are recorded: a texture marked with an older one is not sampled
	const GSTexture* m_rt = nullptr;
	const GSTexture* m_ds = nullptr;
	const GSTexture* m_group_rt = nullptr;
	const GSTexture* m_group_ds = nullptr;
	static inline u64 s_emitted = 0;
	static inline u64 s_moved = 0;
};
