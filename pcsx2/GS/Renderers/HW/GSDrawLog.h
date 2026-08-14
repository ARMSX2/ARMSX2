// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSRegs.h"
#include "common/Pcsx2Defs.h"

#include <string>

/// Per-draw ledger for GS performance triage.
///
/// Answers "which draws were expensive, and what PS2 state made them expensive" over a
/// whole scene. The existing per-draw facility (GSHWDrawConfig::DumpConfig, driven by
/// SaveHWConfig) writes one text file per draw, which is the right shape for inspecting
/// a single suspicious draw and the wrong shape for profiling: a 900-draw frame produces
/// 900 files. This produces one append-only table instead.
///
/// I/O discipline is the whole design. At ~900 draws/frame and 60 fps this sees roughly
/// 54k rows/sec; formatting a row costs microseconds and writing it costs bandwidth, and
/// both would land on the GS thread -- the thread under investigation -- turning the
/// ledger into a measurement of itself. So capture records a packed POD into a
/// preallocated arena (a memcpy-class store, no formatting, no I/O) and serialisation to
/// CSV happens once, afterwards.
///
/// The arena is bounded, so a long session yields the last N frames rather than an
/// unusable multi-gigabyte file. Truncation is reported rather than silent.
///
/// A row is assembled from two points in the draw, because the field sets do not
/// overlap: the PS2 view (registers, live at the top of GSRendererHW::Draw) and the
/// backend view (GSHWDrawConfig, only finalised at submit).
///
/// This is an attribution tool, never a comparison tool. It is not free -- roughly 0.3%
/// of a core plus the cache pressure of streaming writes -- so an A/B with the ledger
/// enabled on one arm is invalid.
namespace GSDrawLog
{
	/// Packed to keep the arena small and the per-draw store cheap. Enum-ish fields are
	/// stored raw and named at serialisation time.
	struct Record
	{
		u32 frame;
		u32 draw; // GS draw serial (s_n)

		/// Index of the GS dump packet this draw came out of, or PacketNone when the run
		/// is not a dump replay (or nobody asked the replayer to publish marks).
		///
		/// This is the join key between the ledger and a checkpoint ladder. A ladder rung
		/// is named by a dump packet because that is the only identity a local run and a
		/// console replay can both compute without reading each other's file; a ledger row
		/// is named by a draw serial. Without this column, "the divergence appears after
		/// packet 49" and "these draws are the filtered ones" are two facts about the same
		/// stream that cannot be put in one table -- which is how an attribution ends up
		/// resting on the assumption that two arms enumerate draws identically.
		u32 packet;

		u32 frame_block;
		u32 frame_fbmsk;
		u32 z_block;
		u32 tex_tbp0;

		/// Identity of the expanded palette this draw sampled through, or 0 when it
		/// sampled no palette. Filled by NoteClut.
		///
		/// The palette is the one part of a draw's input that the register state does not
		/// name: TEX0's CBP says where the load came from, not what arrived, and a game
		/// that repaints a screen through hundreds of palettes at one CBP produces
		/// hundreds of identical-looking rows. Hashing what the sampler actually holds
		/// separates them, and the contents go out beside the CSV so a row can be resolved
		/// back to real colours.
		u64 clut_hash;

		/// TEX0's palette fields: CBP in block units, then CPSM/CSM/CSA/CLD packed as
		/// CPSM bits 0-3, CSM bit 4, CSA bits 5-9, CLD bits 10-12. Valid on any row whose
		/// texture format is palettised; zero elsewhere.
		u32 tex_cbp;
		u16 tex_clut_cfg;

		/// The vertex colour every vertex of this draw carried, when they all carried
		/// one; otherwise the minimum, with vertex_rgba_eq clear.
		///
		/// Without it a texture-function column says MODULATE and stops there, when
		/// whether MODULATE is an identity or a truncating scale is decided entirely by
		/// this value -- 0x80 is exact and anything else loses a level per draw. A pass
		/// that accumulates several modulated draws turns that into several levels, which
		/// is not recoverable from any other column.
		u32 vertex_rgba;

		u16 prim_count;
		u16 alpha; // ALPHA A/B/C/D, 2 bits each

		// ALPHA.FIX and TEST.AREF, raw bytes. FIX is what separates a C=FIX blend that
		// maps onto a hardware blend factor from one that needs shader arithmetic, and
		// AREF is what decides whether a vertex-trace alpha range proves an alpha test —
		// the M4 blend census cannot be sized without either.
		u8 alpha_fix;
		u8 aref;
		u8 vertex_rgba_eq; ///< every vertex carried vertex_rgba

		u8 prim_type;
		u8 frame_psm;
		u8 frame_fbw;
		u8 z_psm;
		u8 z_ztst;
		u8 tex_psm;
		u8 tex_tbw;
		u8 tex_tw;
		u8 tex_th;
		u8 atst;
		u8 afail;
		u8 datm;
		u8 flags;

		// Sampler + environment census bits. tex_filter is only meaningful on textured
		// draws: bit 0 MMAG, bits 1-3 MMIN, bits 4-6 MXL. env is always valid: bit 0
		// FGE, bit 1 FST, bit 2 TCC, bits 3-4 TFX, bit 5 COLCLAMP.CLAMP, bit 6 PABE,
		// bit 7 AA1. These order the Tile envelope work (nearest vs filtered vs mip
		// splits of the textured floor) and carry the wrap-mode frequency census the
		// blend capture said it could not weight itself.
		u8 tex_filter;
		u8 env;
		// Continuation of env, which is full. bit 0 DTHE, bit 1 IIP, bit 2 FBA (the
		// context register that ORs bit 7 into stored alpha — the gs-alpha2 capture
		// measured it applying at storage; its frequency decides how much the M4b
		// in-shader FBA realization is worth). The dither capture
		// measured two gaps whose cost is a frequency question this answers: how
		// often a game enables dither at all decides whether the SW rasterizer's
		// bulk rectangle fill (which cannot dither, so dithered draws now take the
		// slower scanline) and the Tile floor for dithered draws cost anything.
		// IIP separates gouraud from flat shading, which no other column reveals:
		// every shading-sensitive defect is identity on a flat-shaded draw, so a
		// corpus whose native draws are all flat is blind to a whole class of
		// error and cannot say so. This column is what lets a candidate dump be
		// scored for that coverage before it is adopted.
		u8 env2;

		// Backend view, filled at submit. Zeroed if the draw returned early.
		u8 topology;
		u8 tex_hazard;
		u8 destination_alpha;
		u8 colormask;
		u8 barrier; // 0 none, 1 one-barrier, 2 full-barrier
		u8 self_read; // SelfRead enum, filled during hazard handling
		u8 prim_overlap; // GSState::PRIM_OVERLAP -- whether the draw's own primitives overlap

		s16 area_x;
		s16 area_y;
		s16 area_z;
		s16 area_w;

		// Tile-renderer view (NoteTileDraw). The columns exist ahead of the machinery
		// they will measure, deliberately: the memoization hit rate and per-draw record
		// cost are load-bearing bets of the Tile design, and the instrument has to
		// predate the bet so the numbers exist the day the machinery lands. Rows from
		// the Classic renderer leave all of this zero and serialise it as empty cells.
		// Tile rows never carry the backend view above (their draws do not go through
		// GSHWDrawConfig), so they serialise as submitted=0 with the tile cells filled.
		u32 pass_id; // pass-graph instance the draw landed in; 0 until the graph exists
		u32 record_ns; // Tile bookkeeping cost for this draw (key hash, probes, page sets) -- never draw execution
		u8 tile; // row came from the Tile renderer
		// Row came from the software rasterizer. Like Tile rows these carry no backend
		// view, so they serialise as submitted=0 with the draw rect filled. The column
		// exists because the software arm is an oracle in its own right: reconciling it
		// against console captures needs the pixels and the per-draw state to come out
		// of the same run, not out of a second arm assumed to enumerate draws the same way.
		u8 sw;
		u8 memo_hit; // draw-key memoization hit
		u8 fallback_reason; // TileFallback enum
		// GSTileStqGuard mask, on non-FST textured Tile rows only. Recorded whatever
		// reason actually floored the draw, because TEX_PERSPECTIVE outranks the STQ
		// guard: without it the guard's population and its per-clause split are both
		// invisible until the perspective floor lifts, which is exactly when the
		// numbers are needed.
		u8 stq_guard;
	};

	/// How a draw whose texture aliased the render target or depth buffer was resolved.
	///
	/// The tex_hazard and barrier columns are the draw config AFTER
	/// GSRendererHW::HandleTextureHazards rewrote it, so a draw that arrived aliasing the
	/// target and was resolved by copying is indistinguishable in them from an ordinary
	/// textured draw -- it reads as tex_hazard NONE, barrier 0, no copy anywhere in sight.
	/// Reading that resolved state as the original state produced a confidently wrong
	/// diagnosis once. This column records which road the draw actually took.
	enum SelfRead : u8
	{
		SelfReadNone = 0, ///< the source did not alias the render target or depth buffer
		SelfReadTexIsFb, ///< read its own fragment's framebuffer value in the shader
		SelfReadBarrier, ///< sampled the live attachment under a barrier
		SelfReadDepthDirect, ///< sampled the depth buffer directly, no barrier needed
		SelfReadCopy, ///< copied the target and sampled the copy
	};

	/// Why a Tile-renderer draw took the SW-fallback floor instead of a native
	/// realization. The per-title distribution of this column is the primary health
	/// metric of the Tile renderer: promotion work is aimed at whatever reason
	/// dominates. Reasons are appended as the native path grows; Floor means the
	/// native path does not exist yet and every draw spills by construction.
	enum TileFallback : u8
	{
		TileFallbackNone = 0, ///< realized natively (no fallback)
		TileFallbackFloor, ///< unconditional floor (kept for pre-native-path ledgers)
		// Register-predicate reasons (GSTileDrawLowering):
		TileFallbackPrimClass,
		TileFallbackSpriteExpand,
		TileFallbackTextured,
		TileFallbackBlend,
		TileFallbackCoverageAA1,
		TileFallbackFog,
		TileFallbackFba,
		TileFallbackScanMask,
		TileFallbackAlphaTest,
		TileFallbackDateTest,
		TileFallbackZTestNever,
		TileFallbackFrameMaskPartial,
		TileFallbackNothingWritten,
		TileFallbackFramePsm,
		TileFallbackZbufPsm,
		TileFallbackFrameZPairing,
		TileFallbackStrideZero,
		// Geometry/resource reasons (decided by the draw route):
		TileFallbackFootprintExtent,
		TileFallbackFrameZOverlap,
		TileFallbackResourceFailure,
		// M3b texture gates (TileFallbackTextured retired with them — kept so old
		// ledgers still decode):
		TileFallbackTextureFiltered,
		TileFallbackTextureMip,
		TileFallbackTexturePsm,
		TileFallbackTextureFeedback,
		TileFallbackTexturePerspective,
		TileFallbackTextureStqOverflow,
		// gs-dither gate: the native path writes no dither matrix.
		TileFallbackDither,
		// Depth transcription gate: a Z-gradient triangle draw outside the soft-float
		// walk's envelope (z hull, gradient magnitude, or no VS expand support).
		TileFallbackDepthWalkEnvelope,
		// M4a blend guardrail: COLCLAMP=0 on a blended draw — wrap semantics need the
		// in-shader blend over a read (M4c); fixed-function cannot express them.
		TileFallbackColClip,
	};

	enum Flags : u8
	{
		FlagTextured = 1 << 0,
		FlagBlend = 1 << 1,
		FlagAlphaTest = 1 << 2,
		FlagDate = 1 << 3,
		FlagZTest = 1 << 4,
		FlagZMask = 1 << 5,
		FlagSubmitted = 1 << 6, ///< reached the backend; absent means the draw was skipped
		FlagFeedbackLoopRT = 1 << 7, ///< the pixel shader reads the render target (any reason)
	};

	/// Record::packet when the draw did not come from a dump packet.
	static constexpr u32 PacketNone = 0xFFFFFFFFu;

	/// Whether recording is currently active. Cheap enough to test per draw.
	bool IsActive();

	/// Whether a row is open. The ledger holds one at a time, so a rasterization that can
	/// run nested inside another draw -- the Tile renderer's SW floor -- asks this before
	/// opening one of its own, and rides the caller's row when the answer is yes.
	bool HasOpenDraw();

	/// Names the dump packet whose work is about to reach the GS thread. Every row opened
	/// after this call carries that index until the next one.
	///
	/// ⚠️ Must be called ON THE GS THREAD, ordered against the packet's own work -- which
	/// under MTGS means queued through the same channel the packet's registers travel
	/// down, not read from the CPU thread's counter. The CPU thread runs ahead, so a
	/// direct read names a packet several ahead of the draw it is stamping, and the error
	/// is invisible: the numbers stay plausible and monotonic.
	void MarkPacket(u32 packet_index);

	/// Allocates the arena and begins recording. Safe to call when already active.
	void Start();

	/// Stops recording. Retains the arena so it can still be written out.
	void Stop();

	/// Begins a row from the PS2 register state. Returns without effect if inactive or
	/// the arena is full.
	void BeginDraw(const Record& ps2_state);

	/// Records how a target-aliasing source was resolved, on the open row. Called from
	/// hazard handling rather than at submit because the resolution is not recoverable
	/// from the finished draw config -- see SelfRead.
	void NoteSelfRead(SelfRead resolution);

	/// Records the expanded palette the draw is about to sample through, on the open row.
	/// `clut` is the post-TEXA 32-bit palette the rasterizer will read, `entries` its
	/// length (16 or 256).
	///
	/// Content is interned by hash: the row carries only the hash, and each distinct
	/// palette is written once beside the CSV. A hash alone says which rows shared a
	/// palette, which is the cheap half; comparing a palettised draw against a console
	/// capture needs the colours themselves, and reconstructing them afterwards from a
	/// CBP means re-deriving the load, the CSA mapping and the TEXA expansion -- three
	/// places our own model is known to be under measurement.
	void NoteClut(const u32* clut, u32 entries);

	/// Completes the row opened by BeginDraw with the backend view. No-op if BeginDraw
	/// did not record one (inactive, or arena full). prim_overlap is GSState::PRIM_OVERLAP,
	/// passed in because it lives on the renderer rather than the draw config.
	void EndDraw(const GSHWDrawConfig& config, u8 prim_overlap);

	/// Completes the row opened by BeginDraw with the Tile-renderer view instead of the
	/// backend view. record_ns is the draw's Tile bookkeeping cost only; pass_id is 0
	/// until the pass graph exists. No-op if BeginDraw did not record a row.
	void NoteTileDraw(bool memo_hit, u32 record_ns, u32 pass_id, TileFallback fallback, const GSVector4i& rect,
		u8 stq_guard);

	/// Completes the row opened by BeginDraw with the software rasterizer's view: the
	/// draw rect it will rasterize into (bbox ∩ scissor). No backend view exists for
	/// these draws. No-op if BeginDraw did not record a row.
	void NoteSWDraw(const GSVector4i& rect);

	/// Closes any row left open by a draw that returned before submit, so skipped draws
	/// still appear. Called on every exit from GSRendererHW::Draw.
	void FinishDraw();

	/// Serialises the arena to CSV. Returns false on I/O failure.
	bool WriteCSV(const std::string& path);

	/// Number of rows recorded, and whether the arena filled up and dropped rows.
	size_t GetRecordCount();
	bool WasTruncated();

	/// Drops all recorded rows and frees the arena.
	void Reset();
} // namespace GSDrawLog
