// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <vector>

/// Holds hardware draws back so that consecutive draws to the same render target can be
/// emitted as one render pass instead of one pass each.
///
/// This exists for tiling GPUs. On a desktop GPU a render pass boundary is close to free;
/// on Adreno or Mali each one is a full tile load followed by a full tile store, so a game
/// that alternates between two targets pays for the whole framebuffer twice per switch.
/// Dirge of Cerberus is the motivating case: 1234 draws per frame ping-ponging 1:1 between
/// a colour target and a mask target, 822 switches, ~887 render passes, and only 11 draws
/// that actually read across the pair. Collapsing that alternation into a couple of runs
/// removes both the tile traffic and the per-pass CPU cost the driver charges on the GS
/// thread.
///
/// The scheduler deliberately knows nothing about GS memory. It keys runs on GSTexture
/// identity and relies on GSDevice::FlushDeferredDraws() being unavoidable: anything that
/// could observe a target - a copy, a sample, a readback, a present - goes through a
/// GSDevice entry point that flushes first. Higher layers that know more (the texture
/// cache, which can see two GSTextures aliasing the same GS block range) flush explicitly.
class GSPassScheduler
{
public:
	GSPassScheduler();
	~GSPassScheduler();

	__fi u32 GetCount() const { return static_cast<u32>(m_records.size()); }
	__fi bool IsEmpty() const { return m_records.empty(); }

	/// True when a draw is a plain write to its target: no barrier, no feedback loop, no
	/// destination alpha test, no colclip, no second pass. Those all either read the target
	/// they write or carry state across draws, and holding them back would change what they
	/// see. Anything rejected here renders immediately, so a game that never ping-pongs
	/// simply behaves as it did before.
	static bool IsDeferrable(const GSHWDrawConfig& config);

	/// True when this draw belongs to the run that is already open, i.e. it targets the same
	/// colour and depth attachments and so can share the render pass.
	bool MatchesOpenRun(const GSHWDrawConfig& config) const;

	/// Copies the draw into the queue. Returns false when a cap is hit and the caller should
	/// emit what is queued first. The geometry is copied because config.verts and
	/// config.indices point into GSState's per-draw buffers, which the next draw overwrites.
	bool Enqueue(const GSHWDrawConfig& config);

	/// Renders every queued draw, in submission order, and empties the queue.
	void Emit(GSDevice* dev);

	/// Drops the queue without rendering. Only for device teardown, where the targets are
	/// going away anyway.
	void Clear();

private:
	// Caps exist so a pathological frame cannot grow the queue without bound. Hitting one is
	// not an error: it just forces an earlier flush, which costs a render pass, not
	// correctness. Sized well above Dirge's ~600-draw runs.
	static constexpr u32 MAX_DRAWS = 2048;
	static constexpr u32 MAX_VERTEX_BYTES = 4 * 1024 * 1024;
	static constexpr u32 MAX_INDEX_BYTES = 1 * 1024 * 1024;

	struct Record
	{
		GSHWDrawConfig config; ///< verts/indices are dangling here; fixed up in Emit()
		u32 vertex_offset;
		u32 index_offset;
	};

	// Offsets rather than pointers: these vectors reallocate as a run grows, which would
	// dangle any pointer taken earlier. They are never shrunk, so once a scene reaches its
	// high-water mark the steady state does no allocation at all.
	std::vector<Record> m_records;
	std::vector<GSVertex> m_vertices;
	std::vector<u16> m_indices;
};
