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
		u8 flags2; // Flags2 bits

		/// The two alpha ranges an FBMSK-drop rule has to compare: what this draw can write
		/// (GetAlphaMinMax with FBA folded in, after the alpha-test correction) and what the
		/// target is already known to hold, as it stood before this draw. Only filled for
		/// draws that reach the render-target alpha-range calculation; Flags2AlphaRanges says
		/// which those are.
		s16 src_alpha_min;
		s16 src_alpha_max;
		s16 rt_alpha_min;
		s16 rt_alpha_max;

		/// Census scaffolding: the DestinationAlphaMode this draw would take on the Adreno
		/// feature set, on the same set with a stencil buffer, and on this device (the last
		/// is the self-check -- it must equal destination_alpha). Filled only for draws that
		/// reach the DATE method selection; Flags2DATEModes says which those are.
		u8 date_mode_adreno;
		u8 date_mode_stencil;
		u8 date_mode_selfcheck;

		/// Which render target the draw wrote, and how it touched that target's alpha.
		///
		/// fb_addr names the address the draw asked for; it does not name the target
		/// object, which can outlive an address or be recreated at one. Any question of the
		/// form "what had already been written to this target when that draw ran" needs the
		/// object, so Target carries a serial and the ledger records it here. rt_id is zero
		/// on a row that reached no render-target alpha calculation.
		u32 rt_id;
		u32 rt_tbp0; ///< that target's own TEX0.TBP0, which need not equal fb_addr
		u8 rt_alpha_flags; ///< RTAlpha bits
		u8 rt_fbmask_a; ///< the alpha mask CalculateAlphaRange worked from (FbMask.a, or 0)
		u8 rt_alpha_fmt_mask; ///< the target format's alpha mask (fmsk >> 24)

		/// TargetEvent for a row that records something other than a draw; zero on draws.
		u8 evt_kind;

		/// ExactAlphaDrop: what the exact alpha-mask-drop rule decided for this draw, whether or
		/// not the key let it act. Recorded rather than re-derived because the decision reads the
		/// target's known alpha bits, which only exist while the draw is running.
		u8 exact_alpha_drop;

		s16 area_x;
		s16 area_y;
		s16 area_z;
		s16 area_w;

		// GSHWDrawConfig::samplearea -- the region of the SOURCE the draw reads, in the same
		// scaled coordinates as the draw rect above. Without it a self-reading draw's row says
		// only where it wrote, so "read the pixel it is writing" and "read a different part of
		// the target it is writing" are the same row -- and those two take different roads and
		// have different hazards. Zero on a draw with no texture, and on software rows.
		s16 sample_x;
		s16 sample_y;
		s16 sample_z;
		s16 sample_w;

		// Row came from the software rasterizer. Such a row carries no backend view, so
		// it serialises as submitted=0 with the draw rect filled. The column exists
		// because the software arm is an oracle in its own right: reconciling it against
		// console captures needs the pixels and the per-draw state to come out of the
		// same run, not out of a second arm assumed to enumerate draws the same way.
		u8 sw;
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
		/// Channel shuffle whose source page differs from its destination: sampled the live
		/// attachment at gl_FragCoord + ChannelShuffleOffset, a page away from the pixel the
		/// fragment writes. Split out of SelfReadBarrier because it is a different road with a
		/// different remedy, and the two were indistinguishable in the column -- which is what
		/// the offset-read census could not resolve. Rows recorded as BARRIER by a build older
		/// than this one include these.
		SelfReadShuffleOffset,
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

	/// Flags ran out of bits at FlagFeedbackLoopRT, so the second byte starts here.
	enum Flags2 : u8
	{
		Flags2AlphaRanges = 1 << 0, ///< src_alpha_* / rt_alpha_* were filled
		Flags2DATEModes = 1 << 1, ///< date_mode_* were filled
		Flags2RTAlpha = 1 << 2, ///< rt_id / rt_alpha_flags / rt_fbmask_a were filled
		Flags2Event = 1 << 3, ///< the row is a target event, not a draw; see evt_kind
	};

	/// What the exact alpha-mask-drop rule decided about a draw.
	///
	/// The three refusals are separated because they say different things about the title: an
	/// unknown target is a tracker problem, a non-constant source is a geometry fact, and a
	/// load-bearing mask is a draw the rule must never touch.
	enum ExactAlphaDrop : u8
	{
		ExactAlphaDropNotConsidered = 0, ///< not an alpha-only partial mask on a 32-bit target
		ExactAlphaDropTaken, ///< the mask was the identity; it was cleared
		ExactAlphaDropIneligible, ///< an alpha-only partial mask, refused on a precondition
		ExactAlphaDropTargetUnknown, ///< the target does not know the bits the mask holds back
		ExactAlphaDropSourceNotConstant, ///< the fragment alpha straddles one of those bits
		ExactAlphaDropLoadBearing, ///< known and constant, and different: the mask is doing work
	};

	/// How a draw touched its render target's alpha, as CalculateAlphaRange saw it.
	///
	/// The distinction that matters is whether the alpha write reached the bits it wrote
	/// unconditionally: a write whose mask covers some alpha bits leaves those bits holding
	/// whatever was there, which is why a mask-aware tracker can carry a known bit across
	/// such a draw and a min/max tracker cannot.
	enum RTAlpha : u8
	{
		RTAlphaWritten = 1 << 0, ///< took the ordinary alpha-range branch
		RTAlphaShuffle = 1 << 1, ///< took the texture-shuffle branch instead
		RTAlphaFullCover = 1 << 2, ///< full_cover was true: the draw covers the whole valid rect
		RTAlphaCommitted = 1 << 3, ///< rt->m_alpha_min/max were actually assigned afterwards
		RTAlphaRangeWasSet = 1 << 4, ///< rt->m_alpha_range as it stood before the draw

		// full_cover is a conjunction of three unrelated things, and "the draw did not cover
		// the target" is a different fact from "the draw might have covered it but a depth or
		// alpha test could reject pixels". Anything that wants to establish what a target
		// holds needs to know which one refused, so each conjunct is recorded on its own.
		RTAlphaCoversValid = 1 << 5, ///< the draw rect contains the whole valid rect
		RTAlphaNoGaps = 1 << 6, ///< m_primitive_covers_without_gaps == FullCover
		RTAlphaTestsPass = 1 << 7, ///< no DATE, alpha test always writes alpha, depth always passes
	};

	/// Everything other than a draw that moves what a render target's alpha holds.
	///
	/// A draw ledger alone cannot answer "was this bit known when that draw ran", because
	/// the answer is set by things that are not draws: the target being created, cleared,
	/// refilled from local memory, or handed another target's contents. Each of those gets
	/// a row of its own, in stream order with the draws.
	enum TargetEvent : u8
	{
		TargetEventNone = 0,
		TargetEventCreate, ///< target constructed; payload is the alpha range it starts with
		TargetEventDestroy,
		TargetEventClear, ///< TryTargetClear wrote a constant colour; payload is its alpha
		TargetEventUploadFull, ///< local memory refilled the whole valid rect; payload is its alpha range
		TargetEventUploadPartial, ///< local memory refilled part of it; payload likewise
		TargetEventUploadNoAlpha, ///< local memory refilled colour only; alpha untouched
		TargetEventInherit, ///< took another target's alpha range wholesale; tbp0 names the source
		TargetEventClobber, ///< alpha range forced, contents no longer described by it
		TargetEventValidGrow, ///< the valid rect grew, so it now covers pixels nothing has written
	};

	/// Record::packet when the draw did not come from a dump packet.
	static constexpr u32 PacketNone = 0xFFFFFFFFu;

	/// Whether recording is currently active. Cheap enough to test per draw.
	bool IsActive();

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

	/// Records the draw's source alpha range and the target's tracked alpha range, on the
	/// open row. Both are read where the renderer already computes them; nothing here
	/// evaluates them a second time.
	void NoteAlphaRanges(int src_min, int src_max, int rt_min, int rt_max);

	/// Records the DATE road this draw would take on the two hypothetical feature sets and on
	/// this device, on the open row. Census scaffolding; see Record::date_mode_adreno.
	void NoteDATEModes(u8 adreno, u8 stencil, u8 selfcheck);

	/// Records which target the draw is about to write and how it touched its alpha, on the
	/// open row. Read where CalculateAlphaRange already has all of it in hand.
	void NoteRTAlpha(u32 target_id, u32 tbp0, u8 alpha_flags, u8 fbmask_a, u8 alpha_fmt_mask);

	/// Records what the exact alpha-mask-drop rule decided, on the open row. See ExactAlphaDrop.
	void NoteExactAlphaDrop(u8 decision);

	/// Marks the open row as having actually assigned the target's new alpha range. A draw
	/// can compute the range and then return before the assignment, and a reconstruction
	/// that misses that reads one draw ahead of the renderer forever after.
	void NoteRTAlphaCommitted();

	/// Records something other than a draw happening to a target: see TargetEvent. Opens
	/// and closes its own row, so it is safe to call from outside a draw. payload_min and
	/// payload_max describe what the event brought in (a clear colour's alpha, an upload's
	/// alpha range); rt_min and rt_max are the target's tracked range after it.
	void NoteTargetEvent(u8 kind, u32 target_id, u32 tbp0, int payload_min, int payload_max,
		int rt_min, int rt_max, const GSVector4i& valid);

	/// Records how a target-aliasing source was resolved, on the open row. Called from
	/// hazard handling rather than at submit because the resolution is not recoverable
	/// from the finished draw config -- see SelfRead.
	void NoteSelfRead(SelfRead resolution);

	/// Completes the row opened by BeginDraw with the backend view. No-op if BeginDraw
	/// did not record one (inactive, or arena full). prim_overlap is GSState::PRIM_OVERLAP,
	/// passed in because it lives on the renderer rather than the draw config.
	void EndDraw(const GSHWDrawConfig& config, u8 prim_overlap);

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
