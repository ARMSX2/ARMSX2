// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"

#include <atomic>
#include <string>
#include <type_traits>

// Front-end decode recorder: a byte-diff instrument over everything the GIF
// decode hands to a renderer.
//
// It answers one question: did anything the GIF front end produces move?
// `-fedump <path>` writes a canonical serialisation of that output surface over
// a whole replay; `-fediff <path>` runs the same replay again and compares
// against the recording, and a divergence names the frame, the event, the
// record, the field and the byte. So any change to GIF decode or to the vertex
// kick can be gated on "byte-identical", and when it is not, the first byte
// that moved is located rather than searched for.
//
// WHERE THIS TAPS, AND WHY.
//
// The GS is already split into a front (GIF parse / vertex kick / draw
// buffering) and a back (local memory, texture cache, draw, present), with
// self-contained records crossing the seam -- GSBackQueue.h. Those records are
// the output surface, and they were designed under exactly the constraint this
// format needs: "every record carries its register snapshot, so the consumer
// needs no live register state machine". So the tap sits on the CONSUMPTION side
// of that seam, at the one funnel every mode and every renderer reaches:
//
//   Draw        GSState::DrawRecordTail (entry) -- both the record path
//               (ExecDrawRecord) and the records-off path call it, and it is the
//               last common point before Draw().
//   Transfer    GSState::ExecTransferRecord (entry) -- both TransferRecord build
//               sites (FlushWrite and the Write whole-packet fast path) funnel here.
//   Move        GSState::ExecMoveRecord (entry).
//   ClutLoad    GSState::ExecClutLoadRecord (entry).
//   LocalToHost GSState::Read, after the transfer-buffer clamp.
//   Vsync       GSRenderer::SubmitVsync -- frame boundaries, so the locator can
//               say "frame 3, draw 812" instead of only "event 41022".
//
// WHAT IS DELIBERATELY NOT IN THE FORMAT.
//
//  - Derived state. The vertex trace (GSState::m_vt), the per-context cached
//    scissor and GSOffset block (GSDrawingContext::scissor / ::offset -- one of
//    which is a raw pointer) are computed FROM the recorded words. A decoder is
//    not obliged to reproduce a cache, and folding one into the contract makes
//    the gate fail on things that are not divergences.
//  - Everything DrawRecordTail itself does after the tap: the texel-coordinate
//    rounding pass (it reads m_vt, so it is consumer-side by construction) and
//    the ZTST_NEVER skip. Both are consumer work, not decode output.
//  - PCRTC display geometry (GSBackQueue::PcrtcSyncRecord). GSvsync computes it
//    from the privileged registers, not from the GIF stream, so no GIF decoder
//    produces it.
//  - The payload bytes of a local->host read. Those come out of local memory,
//    which the BACK end wrote; a renderer-dependent value has no place in a
//    front-end contract. The request registers and the length are recorded.
//
// A stream is only comparable against another stream from the SAME renderer arm:
// GSState::m_channel_shuffle_finish is written on both sides of the seam (the
// front sets it as a one-shot message, the draw path as persistent state), so its
// value at the tap can legitimately differ per renderer.
//
// COST WHEN OFF: one load of GSFeDecode::g_active and an untaken branch. Every
// call site is `if (GSFeDecode::IsActive()) [[unlikely]]`, and every gather runs
// out of line.

namespace GSFeDecode
{
	// ------------------------------------------------------------------
	// Format
	// ------------------------------------------------------------------

	// Bump kFormatVersion on ANY layout change below. A reader that sees a
	// version it does not know refuses the file rather than mis-parsing it --
	// a silently reinterpreted stream is a green gate that proves nothing.
	inline constexpr u32 kFormatVersion = 1;

	inline constexpr u32 kEnvRegWords = 15; // GSDrawingEnvironment PRIM..TRXREG
	inline constexpr u32 kCtxRegWords = 12; // GSDrawingContext XYOFFSET..ZBUF
	inline constexpr u32 kEnvSnapshotWords = kEnvRegWords + 2 * kCtxRegWords;

	inline constexpr u32 kPayloadHeadBytes = 32;

	// File header. Fixed 64 bytes, written once at offset 0.
	struct StreamHeader
	{
		char magic[8]; // "ARMSFED\0"
		u32 format_version;
		u32 header_bytes; // sizeof(StreamHeader)
		u32 record_header_bytes; // sizeof(RecordHeader)
		u32 vertex_stride; // sizeof(GSVertex)
		u32 env_reg_words;
		u32 ctx_reg_words;
		char build[32]; // build identity string, NUL-padded (informational)
	};
	static_assert(sizeof(StreamHeader) == 64);
	static_assert(std::is_trivially_copyable_v<StreamHeader>);

	enum class RecordType : u32
	{
		Draw = 1,
		Transfer = 2,
		Move = 3,
		ClutLoad = 4,
		LocalToHost = 5,
		Vsync = 6,
	};

	// Per-record header. `record_digest` and `stream_digest` are DERIVED and are
	// excluded from the bytes they cover: the canonical bytes of a record are the
	// first 16 bytes of this header (type, body_bytes, event_index) followed by
	// the body. That keeps the digest computable by a reader that only has the
	// record, and keeps the comparison self-consistent.
	struct RecordHeader
	{
		u32 type;
		u32 body_bytes; // body length, always a multiple of 8
		u64 event_index; // monotonic from 0 over the whole stream
		u64 record_digest; // XXH3-64 of this record's canonical bytes
		u64 stream_digest; // XXH3-64 chain: hash(prev_stream_digest, record_digest)
	};
	static_assert(sizeof(RecordHeader) == 32);
	static_assert(std::is_trivially_copyable_v<RecordHeader>);

	inline constexpr u32 kCanonicalPrefixBytes = 16; // type + body_bytes + event_index

	// Raw register snapshot of one GSDrawingEnvironment: the 15 environment
	// registers in declaration order, then CTXT[0]'s 12, then CTXT[1]'s 12.
	// Register WORDS only -- see the header comment on derived state.
	struct EnvSnapshot
	{
		u64 w[kEnvSnapshotWords];
	};
	static_assert(sizeof(EnvSnapshot) == 312);

	// One flushed draw. Mirrors GSBackQueue::DrawRecord field for field, with the
	// pointers replaced by the arrays they point at and GSDrawingEnvironment
	// reduced to its register words.
	struct DrawBody
	{
		u64 draw_serial;
		EnvSnapshot draw_env; // the environment the draw executes under (*m_draw_env)
		EnvSnapshot next_env; // the look-ahead peek the HW heuristics read (m_env)
		u32 next_v[8]; // GSVertex m_v, raw bytes (not the type: it is alignas(32))
		s32 draw_rect[4]; // temp_draw_rect
		s32 backed_up_ctx;
		u32 dirty_gs_regs;
		s32 flush_reason; // GSState::GSFlushReason, widened
		u8 channel_shuffle_finish;
		u8 packed_uv_hack_flag;
		u8 pad0[2];
		u64 prim; // *PRIM, recorded explicitly: the pointer need not aim into draw_env
		u32 ctxt; // PRIM->CTXT
		u32 vertex_head;
		u32 vertex_tail;
		u32 vertex_next;
		u32 vertex_count; // vertices serialized below = max(tail, next)
		u32 index_count; // indices serialized below = GSIndexBuff::tail
		// Tail payload, in this order:
		//   GSVertex vertices[vertex_count]   (32 bytes each, raw)
		//   u16      indices[index_count]
		//   zero padding to an 8-byte multiple
	};
	static_assert(sizeof(DrawBody) == 728);
	static_assert(std::is_trivially_copyable_v<DrawBody>);

	// One HOST->LOCAL upload slice. Mirrors GSBackQueue::TransferRecord; the
	// payload is summarized (a full copy would dwarf the rest of the stream) by
	// a digest over all `len` bytes plus a fixed head window, which is what makes
	// a payload divergence localizable at all.
	struct TransferBody
	{
		u64 blit; // m_tr.m_blit
		u64 env_blit; // m_env.BITBLTBUF / m_tr.m_blit per build site
		u64 pos; // TRXPOS
		u64 reg; // TRXREG
		u64 draw_serial;
		u64 payload_digest; // XXH3-64 over payload[0, len)
		s32 rect[4];
		s32 len;
		s32 stat_len;
		s32 end;
		s32 total;
		s32 init_x;
		s32 init_y;
		u8 first_slice;
		u8 pad0[3];
		u8 payload_head[kPayloadHeadBytes]; // first min(len, 32) bytes, zero-padded
	};
	static_assert(sizeof(TransferBody) == 128);
	static_assert(std::is_trivially_copyable_v<TransferBody>);

	// LOCAL->LOCAL blit. Mirrors GSBackQueue::MoveRecord.
	struct MoveBody
	{
		u64 blit;
		u64 pos;
		u64 reg;
		u64 draw_serial;
	};
	static_assert(sizeof(MoveBody) == 32);

	// CLUT palette load. Mirrors GSBackQueue::ClutLoadRecord. The palette BYTES
	// are not recorded: they are read out of local memory back-side, so they
	// belong to the transfers that put them there, not to this record.
	struct ClutLoadBody
	{
		u64 tex0; // post-CPSM-mask TEX0, as installed
		u64 texclut;
	};
	static_assert(sizeof(ClutLoadBody) == 16);

	// LOCAL->HOST read. Request only -- see the header comment.
	struct LocalToHostBody
	{
		u64 blit; // m_env.BITBLTBUF
		u64 pos; // m_env.TRXPOS
		u64 reg; // m_env.TRXREG
		u64 draw_serial;
		s32 len; // bytes handed to the host, after the transfer-buffer clamp
		s32 total; // m_tr.total
		s32 end; // m_tr.end before this read advances it
		u32 pad0;
	};
	static_assert(sizeof(LocalToHostBody) == 48);

	// Frame boundary. Mirrors GSBackQueue::VsyncRecord, plus the front-assigned
	// draw serial the locator quotes.
	//
	// g_perfmon.GetFrame() is deliberately NOT here, and it is worth saying why:
	// it was, and the corpus self-diff caught it. That counter is bumped inside
	// GSRenderer::VSync's non-skipped branch, so it counts PRESENTED frames --
	// GSDevice::ShouldSkipPresentingFrame() is a present-throttle decision made
	// on timing, and two runs of one dump legitimately disagree about it (seen
	// both directions: flatout2 3-vs-2, xenosaga 1-vs-2). The locator's "frame"
	// is the instrument's own count of Vsync records instead, which is a
	// property of the stream and not of the clock.
	struct VsyncBody
	{
		u64 draw_serial;
		u32 field;
		u8 registers_written;
		u8 idle_frame;
		u8 pad0[2];
	};
	static_assert(sizeof(VsyncBody) == 16);

	// ------------------------------------------------------------------
	// Arming
	// ------------------------------------------------------------------

	/// Set only by BeginRecord/BeginDiff/End (and by Disarm on a write failure).
	/// Read on the GS hot path, which is why it is a plain extern rather than an
	/// accessor behind a config lookup. Atomic because the arming calls come from
	/// whichever thread drives the session while the taps run on the GS thread;
	/// relaxed is enough, since the arming order already establishes ordering for
	/// everything the instrument touches.
	extern std::atomic<bool> g_active;

	__forceinline_odr bool IsActive()
	{
		return g_active.load(std::memory_order_relaxed);
	}

	/// Start recording the front-end output surface to `path`. Returns false (and
	/// stays disarmed) if the file cannot be created.
	bool BeginRecord(const std::string& path, const std::string& build_id);

	/// Start comparing the live surface against the recording at `path`. Returns
	/// false (and stays disarmed) if the file is missing, truncated, or carries a
	/// header this build cannot read.
	bool BeginDiff(const std::string& path);

	/// Flush/close. In diff mode this also reports a recording that outlived the
	/// live stream (records the replay never produced). Idempotent.
	void End();

	/// True once a divergence has been seen. Never clears.
	bool Diverged();

	/// The first-divergence report, ready to print. Empty until Diverged().
	const std::string& DivergenceReport();

	/// Records emitted so far (recording) or compared so far (diff).
	u64 EventCount();

	// ------------------------------------------------------------------
	// Submission (called by the GSState/GSRenderer taps)
	// ------------------------------------------------------------------

	/// Append (or compare) one record. `fixed` is the body struct for `type`;
	/// `tail` is the variable-length remainder (Draw only, else nullptr/0).
	void Submit(RecordType type, const void* fixed, u32 fixed_bytes, const void* tail, u32 tail_bytes);

	/// Convenience for fixed-size records.
	template <typename T>
	__fi void SubmitFixed(RecordType type, const T& body)
	{
		Submit(type, &body, sizeof(T), nullptr, 0);
	}

	// ------------------------------------------------------------------
	// Locator (exposed for the unit tests)
	// ------------------------------------------------------------------

	/// Human-readable name of the field covering `body_offset` in a `type` body.
	/// `body` must point at the record's body bytes (the Draw case reads
	/// vertex_count/index_count from it to name a payload element).
	std::string DescribeOffset(RecordType type, u32 body_offset, const void* body, u32 body_bytes);

	const char* TypeName(RecordType type);
} // namespace GSFeDecode
