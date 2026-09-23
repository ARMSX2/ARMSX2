// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSRegs.h"
#include "common/Pcsx2Defs.h"

#include <string>

/// Per-draw ledger for GS performance triage.
///
/// Records which draws in a scene were expensive and what PS2 state they had, as one
/// table. (GSHWDrawConfig::DumpConfig writes one file per draw, which suits inspecting a
/// single draw, not profiling.)
///
/// Capture only copies a packed POD into a preallocated arena on the GS thread; no
/// formatting or I/O happens there. CSV serialisation happens once, afterwards.
///
/// The arena is bounded; when it fills, later draws are dropped and truncation is reported.
///
/// A row is filled in two steps: the PS2 register view at the top of GSRendererHW::Draw,
/// and the backend view (GSHWDrawConfig) at submit.
///
/// Recording has a cost, so do not compare performance between a run with it enabled and
/// one without.
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
	};

	/// How a draw whose texture aliased the render target or depth buffer was resolved.
	///
	/// The tex_hazard and barrier columns reflect the config after
	/// GSRendererHW::HandleTextureHazards rewrote it, so a self-read resolved by copying looks
	/// like an ordinary textured draw there (tex_hazard NONE, barrier 0). This column records
	/// how the self-read was actually resolved.
	enum SelfRead : u8
	{
		SelfReadNone = 0, ///< the source did not alias the render target or depth buffer
		SelfReadTexIsFb, ///< read its own fragment's framebuffer value in the shader
		SelfReadBarrier, ///< sampled the live attachment under a barrier
		SelfReadDepthDirect, ///< sampled the depth buffer directly, no barrier needed
		SelfReadCopy, ///< copied the target and sampled the copy
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
