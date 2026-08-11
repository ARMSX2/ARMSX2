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

		u32 frame_block;
		u32 frame_fbmsk;
		u32 z_block;
		u32 tex_tbp0;

		u16 prim_count;
		u16 alpha; // ALPHA A/B/C/D, 2 bits each

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
		u8 memo_hit; // draw-key memoization hit
		u8 fallback_reason; // TileFallback enum
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

	/// Whether recording is currently active. Cheap enough to test per draw.
	bool IsActive();

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

	/// Completes the row opened by BeginDraw with the backend view. No-op if BeginDraw
	/// did not record one (inactive, or arena full). prim_overlap is GSState::PRIM_OVERLAP,
	/// passed in because it lives on the renderer rather than the draw config.
	void EndDraw(const GSHWDrawConfig& config, u8 prim_overlap);

	/// Completes the row opened by BeginDraw with the Tile-renderer view instead of the
	/// backend view. record_ns is the draw's Tile bookkeeping cost only; pass_id is 0
	/// until the pass graph exists. No-op if BeginDraw did not record a row.
	void NoteTileDraw(bool memo_hit, u32 record_ns, u32 pass_id, TileFallback fallback, const GSVector4i& rect);

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
