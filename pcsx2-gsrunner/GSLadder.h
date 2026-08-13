// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

/// Reads a rectangle of GS memory back at chosen points *inside* a dump's stream, and
/// writes it in the same file format the console replayer produces.
///
/// This is the local half of the hardware oracle's checkpoint ladder. The console runs
/// a payload and reads the render target at each rung; this runs the same dump under
/// any renderer and reads the same rectangle at the same rungs. A rung is named by the
/// **dump packet index** it follows, which both halves index identically, so the arms
/// line up without either side having to interpret the other's numbering.
///
/// It exists because a whole-frame score says only *that* two renderers disagree. The
/// frame is the end of several thousand draws, and every one of them could be the
/// cause. Reading the same window every N packets turns "they disagree" into "they
/// first disagree here", which is a question about one draw rather than about a game.
///
/// ⚠️ **What each arm's readback actually means differs, and it is not a detail.** Under
/// the software renderer local memory *is* the result. Under a hardware renderer the
/// result lives on the GPU and only reaches local memory through a download, so this
/// asks for that download explicitly (the same one a game's readback triggers) and then
/// reads. Where the texture cache holds no target for part of the rectangle, what comes
/// back is whatever was in memory -- correct in the regions it owns, stale outside them.
/// A hardware arm's rung is therefore evidence about the renderer only where the two
/// arms are both covered; treat a wholesale disagreement as the instrument first.
namespace GSLadder
{
	struct Options
	{
		/// Where to write the capture. Empty disables the ladder entirely.
		std::string output_path;

		/// The rectangle read at every rung. `bp` is in blocks, `bw` in pages.
		u32 bp = 0;
		u32 bw = 0;
		u32 psm = 0;
		u32 x = 0;
		u32 y = 0;
		u32 w = 0;
		u32 h = 0;

		/// Read after every Nth packet. Zero reads only at vsync.
		u32 every = 0;
	};

	/// Arms the ladder. Must be called before the dump starts running.
	bool Begin(const Options& opts);

	/// Writes the capture. Safe to call when the ladder was never armed.
	void Finish();
} // namespace GSLadder
