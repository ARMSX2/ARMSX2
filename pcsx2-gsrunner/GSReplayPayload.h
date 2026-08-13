// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

/// Turns a recorded GS dump into a payload a PlayStation 2 can replay.
///
/// The point is to score recorded *game* content against silicon. Every hardware
/// capture we have is a grid we designed ourselves, which makes them excellent at
/// attribution and structurally blind to interaction -- textures uploaded once and
/// never aliased against a render target, palettes that never move under a draw, no
/// dependence between one case and the next. Real content does all of those things,
/// and when our renderers agree with each other on it we learn nothing, because they
/// share the code that would be wrong.
///
/// A dump is already a replay script rather than a recording that needs interpreting:
/// a freeze of all four megabytes of local memory plus every register, then a tagged
/// stream of transfers, vsyncs and FIFO reads. So the emitter is close to a file
/// transform -- put the freeze onto real GS memory, push the stream at the real GIF,
/// read the framebuffer back.
///
/// Deliberately NOT a per-draw extractor. Extracting one draw means deciding what
/// state that draw depends on; replaying the stream means deciding nothing, and for an
/// oracle a subtly wrong instrument is worse than no instrument. Attribution comes from
/// truncating the stream at a checkpoint instead, which needs no analysis at all.
namespace GSReplayPayload
{
	struct Options
	{
		/// Where to write the payload.
		std::string output_path;

		/// Stop after this many vsyncs. 0 emits the whole dump.
		u32 frame_limit = 0;

		/// Readback rectangle for every checkpoint. Zeroed fields are filled from the
		/// freeze's context-0 FRAME, and whatever is used is printed.
		u32 rb_bp = 0;
		u32 rb_bw = 0;
		u32 rb_psm = 0;
		u32 rb_x = 0;
		u32 rb_y = 0;
		u32 rb_w = 0;
		u32 rb_h = 0;
		bool rb_explicit = false;

		/// Also read after every Nth dump packet, not only at vsync. This is the
		/// console half of the checkpoint ladder: gsrunner's `-ladder-every` names its
		/// rungs by the same packet index, so the two arms line up without either side
		/// interpreting the other's numbering.
		///
		/// ⚠️ Wants a *small* window. The console holds its whole capture in memory
		/// before writing it, so a full frame buys ten rungs and a 64x64 window buys
		/// hundreds -- and a ladder is only worth running when the rungs are dense
		/// enough to name a draw rather than a frame.
		u32 ladder_every = 0;
	};

	/// Emits `opts.output_path` from the dump at `dump_path`. Returns false and explains
	/// itself on the console if the dump cannot be replayed faithfully -- an unsupported
	/// freeze version, or a transfer still in flight at the snapshot, which would make
	/// the restored state one nobody can interpret.
	bool Emit(const std::string& dump_path, const Options& opts);
} // namespace GSReplayPayload
