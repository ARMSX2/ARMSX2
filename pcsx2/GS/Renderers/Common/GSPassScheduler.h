// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <array>
#include <vector>

/// Holds hardware draws back so that draws to the same render target can be emitted
/// together, as one render pass instead of one pass each.
///
/// This exists for tiling GPUs. On a desktop GPU a render pass boundary is close to free;
/// on Adreno or Mali each one is a full tile load followed by a full tile store, so a game
/// that alternates between two targets pays for the whole framebuffer twice per switch.
/// Dirge of Cerberus is the motivating case: 1234 draws per frame ping-ponging 1:1 between
/// a colour target and a mask target, 822 switches, ~887 render passes, and only 11 draws
/// that actually read across the pair. Keeping a run open per target turns that alternation
/// into two passes, removing both the tile traffic and the per-pass CPU cost the driver
/// charges on the GS thread.
///
/// Correctness rests on two things. Draws within a run keep their order, so same-target
/// results are never affected. Draws in *different* runs are reordered, which is only legal
/// while the runs are independent - so every way one run could observe another is a flush:
/// overlapping attachments, one run sampling what another writes, or one run writing what
/// another sampled.
///
/// The scheduler deliberately knows nothing about GS memory; it works on GSTexture identity
/// and relies on GSDevice::FlushDeferredDraws() being unavoidable, since anything that could
/// observe a target goes through an entry point that flushes first. Layers that know more -
/// the texture cache, which can see two GSTextures aliasing the same GS block range - flush
/// explicitly.
class GSPassScheduler
{
public:
	GSPassScheduler();
	~GSPassScheduler();

	enum class Disposition
	{
		Queued,     ///< Taken; the caller is done with this draw.
		NeedsFlush, ///< Caller must emit the backlog, then retry once.
	};

	__fi u32 GetCount() const { return m_queued; }
	__fi bool IsEmpty() const { return m_queued == 0; }

	/// True when a draw is a plain write to its target: no barrier, no feedback loop, no
	/// destination alpha test, no colclip, no second pass. Those all either read the target
	/// they write or carry state across draws, and holding them back would change what they
	/// see. Anything rejected here renders immediately, so a game that never ping-pongs
	/// simply behaves as it did before.
	static bool IsDeferrable(const GSHWDrawConfig& config);

	/// Copies the draw into whichever run it belongs to, opening one if there is room.
	/// Returns NeedsFlush when the draw would be reordered against something it can see, or
	/// when a cap is reached. The geometry is copied because config.verts and config.indices
	/// point into GSState's per-draw buffers, which the next draw overwrites.
	Disposition TryEnqueue(const GSHWDrawConfig& config);

	/// Renders every queued draw and empties the queue. Runs are emitted one after another
	/// so each becomes a single render pass; order within a run is preserved.
	void Emit(GSDevice* dev);

	/// Drops the queue without rendering. Only for device teardown, where the targets are
	/// going away anyway.
	void Clear();

private:
	// Two runs is what the ping-pong pattern needs, and every extra run widens the window in
	// which an aliasing draw forces everything to flush.
	static constexpr u32 MAX_RUNS = 2;

	// Caps exist so a pathological frame cannot grow the queue without bound. Hitting one is
	// not an error: it forces an earlier flush, which costs a render pass, not correctness.
	// Sized well above Dirge's ~600-draw runs.
	static constexpr u32 MAX_DRAWS = 2048;
	static constexpr u32 MAX_VERTEX_BYTES = 4 * 1024 * 1024;
	static constexpr u32 MAX_INDEX_BYTES = 1 * 1024 * 1024;
	static constexpr u32 MAX_SAMPLED = 64;

	struct Record
	{
		GSHWDrawConfig config; ///< verts/indices are dangling here; fixed up in Emit()
		u32 vertex_offset;
		u32 index_offset;
	};

	struct Run
	{
		GSTexture* rt = nullptr;
		GSTexture* ds = nullptr;
		std::vector<Record> records;
	};

	/// True when this draw could see, or be seen by, a draw already queued in another run.
	bool ConflictsWithQueued(const GSHWDrawConfig& config) const;

	bool IsAttachmentOfAnyRun(const GSTexture* tex) const;
	bool WasSampled(const GSTexture* tex) const;
	bool NoteSampled(GSTexture* tex);

	std::array<Run, MAX_RUNS> m_runs;
	u32 m_run_count = 0;
	u32 m_queued = 0;

	/// Every texture sampled by a queued draw, deduped. Bounded: overflow just forces a
	/// flush, which is always safe.
	std::vector<GSTexture*> m_sampled;

	// Offsets rather than pointers: these vectors reallocate as a run grows, which would
	// dangle any pointer taken earlier. They are never shrunk, so once a scene reaches its
	// high-water mark the steady state does no allocation at all.
	std::vector<GSVertex> m_vertices;
	std::vector<u16> m_indices;
};
