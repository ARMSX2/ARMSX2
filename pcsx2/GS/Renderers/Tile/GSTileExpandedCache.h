// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>

class GSTexture;

// The fast profile's palette-expanded sources: an index texture pushed through its
// palette ONCE on the device (GSDevice::TileExpandPalette) into an RGBA8 texture, so
// a draw on the GPU sampler leg fetches colours directly instead of expanding four
// filter corners through the palette per fragment. The expanded texel is
// bit-identical to what the in-shader expansion produces (the pass reuses the draw
// shader's own index decode and an unfiltered palette fetch), so the substitution is
// value-preserving; what it buys is fetch count and, later, the hardware filter.
//
// Keyed on the CONTENT IDENTITY of both parents: the index texture's build id
// (GSTileTextureSource — stamped per (re)build, so a rebuild in place or a recycled
// slot changes it) times the palette texture's build id (GSTilePaletteCache — one id
// per distinct palette words). A pair of equal ids is the same bytes through the
// same palette, whatever either cache has since evicted; an entry never holds a
// parent pointer, so parent eviction dangles nothing — the orphaned pair just stops
// matching and ages out by LRU. One entry per pair means a game cycling N palettes
// over one source holds N expansions and rebuilds none of them (the CLUT-thrash
// risk is the Passes() counter's job to expose).
class GSTileExpandedCache
{
public:
	GSTileExpandedCache();
	~GSTileExpandedCache();

	/// The expanded RGBA texture for this (index, palette) pair, or null when the
	/// pair does not (yet) merit one. Expansion is admitted on second sight ACROSS
	/// A FRAME BOUNDARY: the first frame that sees a pair records a marker and
	/// returns null (the draw keeps the in-shader path), a later frame seeing the
	/// same pair builds, later lookups hit. Both halves of the filter are
	/// load-bearing, measured on GT4-fast: the index cache rebuilds ~200 windows
	/// per frame (fresh ids that never repeat — expanding those would add hundreds
	/// of passes per frame for draws that run once), and a per-frame-volatile
	/// window sampled by two or more draws reaches its second sight WITHIN the
	/// frame (61.5 builds/frame sustained under a frame-blind second-sight
	/// filter, every one of them dead by the next frame). A pair that survives a
	/// frame boundary is stable by demonstration and pays one pass for its
	/// lifetime; a volatile one never pays anything, which is exactly the
	/// pre-expansion status quo for its draws.
	///
	/// Null is also returned when the device does not serve the expansion or
	/// allocation fails. A refused expansion DRAW flips Serves() off for the
	/// session — the dominant cause is a device that will never serve it (not
	/// Vulkan, pipeline failed to build), and the rare transient cause degrades to
	/// the same correct fallback, the in-shader palette path. A failed texture
	/// allocation here leaves Serves() on. `levels` must match the index texture's
	/// mip count (each level expands separately through the same palette).
	GSTexture* Lookup(GSTexture* index, u64 index_id, GSTexture* palette, u64 palette_id, u32 levels);

	/// The admission filter's verdict for a pair, without building anything: would a
	/// Lookup right now hand back an expanded texture, or would it defer?
	///
	/// Not const, and it cannot be: first sight IS the marker, so a probe that recorded
	/// nothing would report first sight forever. What it does not do is allocate, expand
	/// or touch the device — the entry it leaves behind is the same marker Lookup would
	/// have left, and a later Lookup on the pair picks up from it unchanged. Serves()
	/// is not consulted (nothing is asked of the device) and no counter moves; a caller
	/// probing rather than building keeps its own.
	enum class Admission
	{
		Deferred, ///< first sight of the pair, in this frame or a marker recorded this frame
		Served,   ///< the pair survived a frame boundary: an expansion is earned
	};
	Admission ProbeAdmit(u64 index_id, u64 palette_id);

	/// Frame boundary, for the admission filter's clock. Call once per VSync.
	void NextFrame() { m_frame++; }

	/// Recycles every cached texture (reset / teardown / hot-switch).
	void Clear();

	/// False once an expansion draw has been refused (not Vulkan, or the pipeline
	/// failed to build; a transient device failure trips it too) — permanent for
	/// the session, and every draw keeps the in-shader palette path.
	bool Serves() const { return m_serves; }

	u64 Hits() const { return m_hits; }
	u64 Builds() const { return m_builds; }
	/// Per-level expansion draws issued — the CLUT-thrash column: a title cycling
	/// palettes faster than the cache holds shows up here, not in Builds() alone.
	u64 Passes() const { return m_passes; }
	/// First-sight lookups that recorded a marker instead of building — the
	/// admission filter's reject column. High and steady = parent churn being
	/// absorbed as intended; high HITS beside high deferrals is the healthy shape.
	u64 Deferrals() const { return m_deferrals; }

private:
	static constexpr u32 kMaxEntries = 512;

	struct Entry
	{
		u64 index_id = 0;
		u64 palette_id = 0;
		u64 last_use = 0;
		u64 seen_frame = 0; ///< frame that recorded the marker
		GSTexture* tex = nullptr; ///< null while the entry is a first-sight marker
		bool alive = false;
	};

	std::array<Entry, kMaxEntries> m_entries;
	u64 m_use_counter = 0;
	u64 m_frame = 0;
	u64 m_hits = 0;
	u64 m_builds = 0;
	u64 m_passes = 0;
	u64 m_deferrals = 0;
	bool m_serves = true;
};
