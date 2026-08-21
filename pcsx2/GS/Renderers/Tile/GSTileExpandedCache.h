// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Tile/GSTileSlotIndex.h"

#include "common/Pcsx2Types.h"

#include <array>

class GSTexture;

/// The build road for a caller that cannot let the expansion happen NOW. GSDevice::TileExpandPalette
/// issues the pass immediately, which is right for a renderer that draws immediately; a renderer
/// that RECORDS draws and executes them later needs the expansion ordered against the index build it
/// reads, in the stream it will eventually execute. The cache keeps everything it already owned —
/// admission, identity, allocation, eviction, the pin — and the builder does exactly one thing:
/// arrange for `dst` to hold `palette[index]` by the time anything samples it.
class GSTileExpandBuilder
{
public:
	virtual ~GSTileExpandBuilder() = default;

	/// Fill `dst` — a Format::Color render target the size of `index` — with the palette lookup of
	/// every index texel. Returning false fails the build: the cache drops the texture and the
	/// caller gets null, exactly as an allocation failure does. Unlike TileExpandPalette's own
	/// refusal this does NOT flip Serves() off, because a deferred builder's failure is a property
	/// of the frame (an op stream that could not take the op), not of the device.
	virtual bool BuildTileExpansion(GSTexture* index, GSTexture* palette, GSTexture* dst) = 0;
};

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

	/// What LookupBuilt did, so a caller can count the road rather than infer it from a pointer.
	enum class BuiltOutcome
	{
		Hit,             ///< the pair already had its expansion: no work at all
		Deferred,        ///< first sight of the pair (or re-seen inside its own frame): nothing built
		Built,           ///< the builder was called and the expansion is this frame's
		RefusedCapacity, ///< every entry is pinned this frame; nothing may be evicted
		Failed,          ///< allocation failed, or the builder refused
	};

	/// The deferred build road: same admission filter, same identity, same cache and same pin as
	/// Lookup, but the expansion pass is arranged by `builder` instead of being issued here. Level 0
	/// only — a caller that defers has no mip chain to fill, and the levels argument would have to
	/// mean "levels the builder will fill later", which is not a thing this class can check.
	///
	/// Returns null on a deferral, a refusal and a failure alike; `outcome` says which, and the
	/// caller counts it. A deferral is the designed shape, not an error: the pair is recorded and
	/// the caller's other road carries the draw for one frame.
	GSTexture* LookupBuilt(GSTexture* index, u64 index_id, GSTexture* palette, u64 palette_id,
		GSTileExpandBuilder& builder, BuiltOutcome* outcome = nullptr);

	/// Frame boundary, for the admission filter's clock. Call once per VSync.
	void NextFrame() { m_frame++; }

	/// The PIN discipline's clock, which is a different clock from the admission filter's above:
	/// this one advances when the caller's recorded plan has EXECUTED, not at the video frame
	/// boundary. Zero (the default, and what the Tile renderer leaves it at) turns the discipline
	/// off. An expansion a recorded-but-unissued draw names is never evicted or recycled while it
	/// is stamped — and note that ProbeAdmit recycles too, since recording a marker can take an
	/// occupied slot. See GSTileTextureSource::SetFrame for the argument.
	void SetFrame(u64 frame) { m_pin_frame = frame; }

	/// Lookups and probes that could not take a slot because every entry is pinned by this frame.
	u64 CapacityRefusals() const { return m_capacity_refusals; }

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
	using SlotIndex = GSTileSlotIndex<kMaxEntries, 1024>;

	struct Entry
	{
		u64 index_id = 0;
		u64 palette_id = 0;
		u64 last_use = 0;
		u64 seen_frame = 0; ///< frame that recorded the marker
		u64 pinned_frame = 0; ///< the plan frame a caller was handed this entry in; 0 = never
		GSTexture* tex = nullptr; ///< null while the entry is a first-sight marker
		bool alive = false;
	};

	bool Pinned(const Entry& e) const { return m_pin_frame != 0 && e.pinned_frame == m_pin_frame; }

	/// The slot holding this exact pair, or SlotIndex::kNil. The index narrows to a bucket; the
	/// comparison is the same exact two-id equality the walk it replaced did.
	u16 FindEntry(u64 hash, u64 index_id, u64 palette_id) const;

	/// Record a first-sight marker for this pair and return its slot, or SlotIndex::kNil when
	/// every entry is pinned by the plan frame (counted, and the caller's leg refuses). All three
	/// admission roads record the same marker the same way, so they record it here.
	u16 AdmitSlot(u64 hash, u64 index_id, u64 palette_id);

	std::array<Entry, kMaxEntries> m_entries;
	/// Which slot holds which pair, which slots are free, and which occupied slot is oldest. The
	/// recency list mirrors `last_use` exactly — every stamp below is paired with a Touch — so an
	/// admission that has to take an occupied slot takes the one the old scan took.
	SlotIndex m_index;
	u64 m_use_counter = 0;
	u64 m_frame = 0;
	u64 m_pin_frame = 0;
	u64 m_capacity_refusals = 0;
	u64 m_hits = 0;
	u64 m_builds = 0;
	u64 m_passes = 0;
	u64 m_deferrals = 0;
	bool m_serves = true;
};
