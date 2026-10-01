// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GS.h"
#include "GS/GSRegs.h"
#include "GS/GSVector.h"
#include "GS/GSDrawingContext.h"
#include "GS/GSDrawingEnvironment.h"
#include "GS/GSVertexKick.h"
#include "GS/Renderers/Common/GSVertex.h"

#include "common/HostSys.h"
#include "common/Threading.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

// GV-7: self-contained records crossing the GS front (GIF parse / vertex kick /
// draw buffering) → back (local memory, texture cache, draw, present) boundary.
// Every record carries its register snapshot, so the consumer needs no live
// register state machine. Records are built by the front-side seam functions
// (FlushWrite / Move / ...) and consumed by GSState::Exec*Record on the back
// thread. With GS multi-threading off the seam functions call the Exec*Record
// bodies directly and no record is built.
// Seam classification: scratchpad/gv7-2026-07/SEAM-AUDIT.md.

namespace GSBackQueue
{
	// One slice of a HOST->LOCAL transfer (today's FlushWrite body, or the
	// whole-packet fast path in GSState::Write). A logical transfer is one
	// first_slice record followed by zero or more continuation slices; the
	// executor owns the write cursor across slices.
	struct TransferRecord
	{
		GIFRegBITBLTBUF blit; // m_tr.m_blit: wi() blit argument + partial-end fixup
		GIFRegBITBLTBUF env_blit; // invalidate rect + wi selection: m_env.BITBLTBUF in
		                          // FlushWrite, m_tr.m_blit on the Write fast path —
		                          // they diverge if BITBLTBUF is rewritten mid-transfer
		GIFRegTRXPOS pos;
		GIFRegTRXREG reg;
		GSVector4i rect; // m_tr.rect
		const u8* payload;
		int len; // bytes handed to wi()
		int stat_len; // bytes counted for the Swizzle perfmon stat (the Write
		              // fast path counts the raw packet length, which can
		              // exceed the transfer total — preserved exactly)
		int end; // m_tr.end as of this slice (partial-end fixup input)
		int total; // m_tr.total
		int init_x, init_y; // write-cursor init, consumed when first_slice
		u64 draw_serial; // s_n at build time (upload-queue entry stamping)
		bool first_slice; // initialises the cursor + pushes the upload-queue entry
	};

	// LOCAL->LOCAL blit. The executor installs these registers and runs the
	// virtual Move chain (HW hack -> TC move -> software blit) unchanged.
	struct MoveRecord
	{
		GIFRegBITBLTBUF blit;
		GIFRegTRXPOS pos;
		GIFRegTRXREG reg;
		// Move() skips when XDIR is 3 and sets it to 3 when done. The back object's
		// m_env is otherwise only refreshed by draw records, so without this every
		// move after the first in a run with no draw between them is dropped.
		GIFRegTRXDIR dir;
		u64 draw_serial; // consumed once GV7-0d makes serials record-carried
	};

	// CLUT palette load. The decision chain (WriteTest / CanLoadCLUT /
	// InvalidateRange dirty tracking) is register/address-only and stays
	// front-side; this record triggers the back-side palette-byte read from
	// local memory into the CLUT buffer.
	struct ClutLoadRecord
	{
		GIFRegTEX0 TEX0; // post-CPSM-mask, as installed in m_env.CTXT[i]
		GIFRegTEXCLUT TEXCLUT;
	};

	// Vertex/index buffer sets — the DRAW record payload. Hoisted from GSState
	// (front fills them at kick time, the draw executor consumes and mutates
	// them in place); the GV7-1 pool hands ownership across the boundary.
	struct VertexBuff
	{
		GSVertex* buff;
		GSVertex* buff_copy; // same size buffer to copy/modify the original buffer
		u32 head, tail, next, maxcount; // head: first vertex, tail: last vertex + 1, next: last indexed + 1
		u32 xy_tail;
		GSVector4i xy[4];
		GSVector4i xyhead;
		// Scalar mirror of xy[] for the outcode cull fast path: written wherever
		// xy[] is written, outcodes re-derived on scissor change (RefreshKickMirror).
		GSVertexKernels::CullMirrorEntry kick_ring[4];
		// Fused vertex-trace bounds (aarch64 only): FindMinMax min/max accumulated
		// at index emission over this buffer's referenced vertices. fmm_watermark is
		// the first vertex position not yet folded in (clamped on rewinds/compaction
		// so re-referenced positions re-accumulate); fmm_valid means the accumulator
		// covers every emitted index of the pending draw. Reset lazily at the first
		// emission of a draw (itail == n).
		GSVertexKernels::FmmAcc fmm_acc;
		u32 fmm_watermark;
		bool fmm_valid;
	};

	struct IndexBuff
	{
		u16* buff;
		u32 tail;
	};

	// GV7-1c: one pooled vertex+index buffer set. On the record path, FlushPrim
	// hands the live heap arrays to a node (struct copy + array exchange: the
	// parse slot takes the node's recycled arrays as its fresh buffers), so the
	// DRAW record's payload stays valid until consumed while the front keeps
	// parsing into the same GSState buffer slots it always did. The consumer
	// releases the node after the draw executes.
	struct DrawNode
	{
		VertexBuff vb;
		IndexBuff ib;
	};

	// GV7-1c: pooled transfer staging buffer (4MB, the GSTransferBuffer size).
	// On the front, m_tr.buff aliases the current node's buffer: the front
	// stages into it and TRANSFER records reference slices of it (disjoint
	// ranges, so front-appends and back-reads never overlap). At the next
	// transfer Init after any slice referenced the buffer, the front rotates to
	// a fresh node and emits a RELEASE_PAYLOAD record behind the slices — FIFO
	// guarantees they were consumed by the time the release executes.
	struct PayloadNode
	{
		u8* buff;
	};

	struct ReleasePayloadRecord
	{
		PayloadNode* node;
	};

	// PCRTC digest state — hoisted from GSState (GSvsync writes it once per
	// frame from the privileged registers; the Draw() heuristics and the Merge
	// circuit read it back-side, so it ships whole in PCRTC_SYNC records).
	struct GSPCRTCRegs
	{
		struct PCRTCDisplay
		{
			bool enabled;
			int FBP;
			int FBW;
			int PSM;
			int DBY;
			int DBX;
			GSRegDISPFB prevFramebufferReg;
			GSVector2i prevDisplayOffset;
			GSVector2i displayOffset;
			GSVector4i displayRect;
			GSVector2i magnification;
			GSVector2i prevFramebufferOffsets;
			GSVector2i framebufferOffsets;
			GSVector4i framebufferRect;

			__fi int Block() const { return FBP << 5; }
		};

		int videomode = 0;
		int interlaced = 0;
		int FFMD = 0;
		bool PCRTCSameSrc = false;
		bool toggling_field = false;
		PCRTCDisplay PCRTCDisplays[2] = {};

		bool IsAnalogue();

		// Calculates which display is closest to matching zero offsets in either direction.
		GSVector2i NearestToZeroOffset();

		void SetVideoMode(GSVideoMode videoModeIn);

		// Enable each of the displays.
		void EnableDisplays(GSRegPMODE pmode, GSRegSMODE2 smode2, bool smodetoggle);

		void CheckSameSource();

		bool FrameWrap();

		// If the start point of both frames match, we can do a single read
		bool FrameRectMatch();

		GSVector2i GetResolution();

		GSVector4i GetFramebufferRect(int display);

		int GetFramebufferBitDepth();

		GSVector2i GetFramebufferSize(int display);

		// Sets up the rectangles for both the framebuffer read and the displays for the merge circuit.
		void SetRects(int display, GSRegDISPLAY displayReg, GSRegDISPFB framebufferReg);

		// Calculate framebuffer read offsets, should be considered if only one circuit is enabled, or difference is more than 1 line.
		// Only considered if "Anti-blur" is enabled.
		void CalculateFramebufferOffset(bool scanmask, GSRegDISPFB framebuffer0Reg, GSRegDISPFB framebuffer1Reg);

		// Used in software mode to align the buffer when reading. Offset is accounted for (block aligned) by GetOutput.
		void RemoveFramebufferOffset(int display);

		// If the two displays are offset from each other, move them to the correct offsets.
		// If using screen offsets, calculate the positions here.
		void CalculateDisplayOffset(bool scanmask);
	};

	// Once-per-frame PCRTC digest, shipped BEFORE the vsync-flushed draw
	// records so those draws see the fresh display state, exactly like today
	// (GSvsync digests, then flushes). Mid-frame draws keep seeing the previous
	// frame's digest, also like today.
	struct PcrtcSyncRecord
	{
		GSPCRTCRegs displays;
		u8 scanmask_used; // pre-decrement value; Merge's decrement stays back-side
	};

	// End of frame: the whole VSync() body (Merge -> present -> capture ->
	// perfmon frame tick). Never queued: SubmitVsync drains the back thread and
	// runs it on the MTGS thread.
	struct VsyncRecord
	{
		u32 field;
		bool registers_written;
		bool idle_frame;
	};

	// The privileged-register fields a draw reads, as they stood when the draw was flushed. The
	// registers themselves are one block shared with MTGS, which rewrites it at the vsync packet
	// (and the EE thread in WaitGS) while queued draws have not run yet.
	struct DrawPrivRegs
	{
		u32 dispfb_fbp[2]; // DISP[n].DISPFB.FBP
		bool display_enabled[2]; // PMODE.EN1 / EN2
		bool field_render; // SMODE2.FFMD && isReallyInterlaced()
	};

	// One flushed draw (today's FlushPrim tail: vertex trace -> texel rounding ->
	// Draw() -> perfmon). Self-contained: the executor installs the env snapshots
	// and scalars, then runs the tail against the referenced buffers, which it
	// owns and may mutate in place (texel rounding, HW draw rewrites). The
	// carry-over window is captured front-side before the record is built.
	struct DrawRecord
	{
		// Installed into the consumer's m_prev_env: the draw's own environment,
		// exactly as FlushBuffers staged it before the flush.
		GSDrawingEnvironment draw_env;
		// Next-draw peek for the HW look-ahead heuristics: the live m_env/m_v at
		// build time — bit-exact with today because FlushBuffers installs buffer
		// i+1's env into m_env before flushing buffer i.
		GSDrawingEnvironment next_env;
		GSVertex next_v;
		GSVector4i draw_rect; // temp_draw_rect at flush
		// temp_native_draw_rect at flush. Carried beside draw_rect, and installed
		// beside it, so the shipped rect and its native-grid twin cannot drift
		// apart on the pipelined path.
		GSVector4i native_draw_rect;
		VertexBuff* vertex; // = &node->vb/&node->ib on the record path
		IndexBuff* index;
		DrawNode* node; // released by the consumer after the tail runs (null in tests)
		u64 draw_serial; // front-assigned s_n
		int backed_up_ctx;
		u32 dirty_gs_regs;
		int flush_reason; // GSState::GSFlushReason (class-scoped enum, stored widened)
		bool channel_shuffle_finish;
		bool packed_uv_hack_flag;
		DrawPrivRegs priv;
	};

	// ------------------------------------------------------------------
	// GV7-1: the front->back SPSC ring.
	// ------------------------------------------------------------------

	// Discriminator for ring slots.
	enum class RecordType : u8
	{
		Transfer,
		Move,
		ClutLoad,
		PcrtcSync,
		Vsync,
		Draw,
		ReleasePayload,
	};

	// Single-producer/single-consumer ring. Producer = the MTGS ("front") thread,
	// consumer = the GS back thread. Indices are free-running u32s over a
	// power-of-two slot count; acquire/release only — no RMW, so this is
	// armv8.0-safe (compiles to plain LDAR/STLR).
	template <typename SlotT, u32 kCount>
	class SpscRing
	{
		static_assert(kCount != 0 && (kCount & (kCount - 1)) == 0, "slot count must be a power of two");

	public:
		SpscRing()
			: m_slots(std::make_unique<SlotT[]>(kCount))
		{
		}

		static constexpr u32 Capacity() { return kCount; }

		u32 Size() const { return m_tail.load(std::memory_order_acquire) - m_head.load(std::memory_order_acquire); }
		bool IsEmpty() const { return Size() == 0; }

		// Producer side. BeginPush returns the slot to fill in place (records are
		// built directly in the ring — no intermediate copy), or nullptr when the
		// ring is full (caller applies backpressure). CommitPush publishes the
		// slot to the consumer.
		SlotT* BeginPush()
		{
			const u32 tail = m_tail.load(std::memory_order_relaxed);
			// m_cached_head is a shadow of the consumer's cursor that only ever
			// lags, so a full reading off it is worth re-testing against the
			// real thing while a not-full reading is always true. That keeps the
			// consumer's line out of the push path: it is re-read once per
			// kCount pushes instead of once per push.
			if (tail - m_cached_head == kCount)
			{
				m_cached_head = m_head.load(std::memory_order_acquire);
				if (tail - m_cached_head == kCount)
					return nullptr;
			}
			return &m_slots[tail & (kCount - 1)];
		}

		void CommitPush()
		{
			m_tail.store(m_tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		}

		// Consumer side. Peek returns the oldest unconsumed slot (nullptr when
		// empty); Pop retires it, releasing the slot back to the producer.
		SlotT* Peek()
		{
			const u32 head = m_head.load(std::memory_order_relaxed);
			// Mirror of BeginPush: an empty reading off the shadow is re-tested,
			// a non-empty one is always true. The consumer runs behind whenever
			// there is work, so the shadow answers nearly every call and the
			// producer's line is touched about once per drained batch.
			if (head == m_cached_tail)
			{
				m_cached_tail = m_tail.load(std::memory_order_acquire);
				if (head == m_cached_tail)
					return nullptr;
			}
			return &m_slots[head & (kCount - 1)];
		}

		void Pop()
		{
			m_head.store(m_head.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		}

	private:
		std::unique_ptr<SlotT[]> m_slots;
		// One line per side: each holds that side's own cursor and its shadow of
		// the other's, so neither side writes a line the other reads on its fast
		// path. The shadows are plain u32 — they are private to their owner.
		alignas(64) std::atomic<u32> m_head{0}; // consumer cursor
		u32 m_cached_tail = 0; // consumer's shadow of m_tail
		alignas(64) std::atomic<u32> m_tail{0}; // producer cursor
		u32 m_cached_head = 0; // producer's shadow of m_head
	};

	// Where the producer sleeps when the ring is full or a pool is empty, and
	// how the consumer wakes it.
	//
	// The producer spins briefly on its condition, then arms the wait, re-checks,
	// and sleeps with a timeout. The timeout is what normally ends the sleep: the
	// producer wakes on its own and re-checks, so the consumer pays nothing. A
	// futex post costs the consumer ~5us on a Dimensity 8300, and the consumer is
	// usually the bottleneck when the producer waits, so posting per batch made
	// frames slower there. The consumer posts only as a starvation guard: when it
	// is down to kLowWater queued records while the producer is still asleep, or
	// when it runs out of records. The producer halves its timeout after each
	// such post and lengthens it slowly after each wake that came too early
	// (still no space), so it settles just short of where the guard fires.
	//
	// Lost wake-ups: the producer's "arm; fence; read cursors" and the
	// consumer's "free; fence; read armed word" in NotifyIdle are a Dekker pair,
	// so at least one side sees the other. The per-record check in
	// NotifyRetired has no fence and may see the arming late, which only delays
	// the wake: the consumer passes NotifyIdle before it sleeps, and by then the
	// ring is empty and every pool node is back, so any condition the producer
	// can wait on holds. Independently of all that, a missed post costs at most
	// one timeout, never a hang.
	//
	// Exactly one Post per armed wait that the consumer claims. A producer that
	// disarms itself (condition met on re-check, or timed out) and finds the
	// wait already claimed takes that post, so the semaphore count returns to
	// zero after every wait.
	class alignas(64) SpaceWait
	{
	public:
		// Timeouts in microseconds. The tests pin all three to one long value so
		// that a lost wake-up shows up as a hang instead of a timeout.
		explicit SpaceWait(u32 initial_timeout_us = 50, u32 min_timeout_us = 20, u32 max_timeout_us = 2000)
			: m_timeout_us(initial_timeout_us)
			, m_min_timeout_us(min_timeout_us)
			, m_max_timeout_us(max_timeout_us)
		{
		}

		// Producer. Returns once ready() is true. Pass a ready() that needs a
		// batch of space, not a single slot, or the spin below succeeds after
		// every record and the producer never sleeps.
		template <typename Ready>
		void Wait(Ready&& ready)
		{
			for (u32 spun = 0; spun < kSpinNs; spun += ShortSpin())
			{
				if (ready())
					return;
			}

			for (;;)
			{
				m_armed.store(true, std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				if (ready())
				{
					Disarm();
					return;
				}

				bool posted = m_sema.TimedWait(m_timeout_us);
				if (!posted)
					posted = Disarm();

				const bool done = ready();
				if (posted)
					m_timeout_us = std::max(m_min_timeout_us, m_timeout_us / 2);
				else if (!done)
					m_timeout_us = std::min(m_max_timeout_us, m_timeout_us + m_timeout_us / 8 + 1);
				if (done)
					return;
			}
		}

		// Consumer, after each retired record (and after returning its pool
		// node). queued() is the number of records still in the ring; it is only
		// called while the producer is waiting.
		template <typename Queued>
		void NotifyRetired(Queued&& queued)
		{
			if (!m_armed.load(std::memory_order_relaxed)) [[likely]]
				return;
			if (queued() <= kLowWater)
				Wake();
		}

		// Consumer, when the ring is empty and before it waits for work.
		void NotifyIdle()
		{
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if (m_armed.load(std::memory_order_relaxed))
				Wake();
		}

	private:
		static constexpr u32 kSpinNs = 4000;
		// Queued records left when the guard posts. At ~3us a draw on the
		// slowest target this is ~50us of work, about one futex wake-up.
		static constexpr u32 kLowWater = 16;

		void Wake()
		{
			if (m_armed.exchange(false, std::memory_order_relaxed))
				m_sema.Post();
		}

		// Producer. Returns true if the consumer had already claimed the wait,
		// after taking the post it sends.
		bool Disarm()
		{
			if (m_armed.exchange(false, std::memory_order_relaxed))
				return false;
			m_sema.Wait();
			return true;
		}

		std::atomic<bool> m_armed{false};
		// Producer-only.
		u32 m_timeout_us;
		u32 m_min_timeout_us;
		u32 m_max_timeout_us;
		Threading::KernelSemaphore m_sema;
	};

	// Tagged slot sized for the largest record (DRAW). All records are trivially
	// copyable (asserted below), so slots are reused with no destructor
	// bookkeeping.
	struct RecordSlot
	{
		RecordType type;
		alignas(alignof(DrawRecord)) u8 data[sizeof(DrawRecord)];

		template <typename T>
		T* As()
		{
			static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(data));
			return reinterpret_cast<T*>(data);
		}

		template <typename T>
		const T* As() const
		{
			static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(data));
			return reinterpret_cast<const T*>(data);
		}
	};

	static_assert(std::is_trivially_copyable_v<TransferRecord>);
	static_assert(std::is_trivially_copyable_v<MoveRecord>);
	static_assert(std::is_trivially_copyable_v<ClutLoadRecord>);
	static_assert(std::is_trivially_copyable_v<PcrtcSyncRecord>);
	static_assert(std::is_trivially_copyable_v<VsyncRecord>);
	static_assert(std::is_trivially_copyable_v<DrawRecord>);
	static_assert(std::is_trivially_copyable_v<ReleasePayloadRecord>);

	using RecordRing = SpscRing<RecordSlot, 512>;

	// GV7-1d-ii: everything shared between the producing (front) and consuming
	// (back) sides of the split. The front parser object points at the back
	// object's channel, so records, pool nodes, and drain waits all target one
	// shared instance. The channel's storage owner (the
	// back object) frees the pooled arrays in its destructor; the producer must
	// be destroyed or drained first.
	struct Channel
	{
		RecordRing ring;
		Threading::WorkSema sema;

		// The producer's wait for ring or pool space (separate from `sema`, whose
		// one empty-waiter slot belongs to the drain).
		SpaceWait space;

		// Set while the back thread is running. Read/written only on the MTGS
		// thread (start/stop/drain all happen there), so a plain bool is enough.
		bool consumer_running = false;

		// The one thread allowed to wait on `sema` for empty. WorkSema supports a
		// SINGLE empty-waiter — a second one blocks on m_empty_sema forever,
		// because the worker posts exactly once per transition to idle and then
		// sleeps with the flag already cleared. That is a silent hang in Release,
		// where the pxAssertMsg guarding it inside WaitForEmpty is compiled out,
		// so record the owner at start and check it on every drain instead.
		// Written before the consumer starts, read-only afterwards.
		std::thread::id drain_thread;

		// Draw-node pool: the producer acquires (free ring first, then arena
		// growth up to the cap, then backpressure), the consumer releases after
		// the draw executes. Free-ring capacity == arena cap, so Release can
		// never fail.
		static constexpr u32 kMaxDrawNodes = 64;
		std::vector<DrawNode*> draw_arena;
		SpscRing<DrawNode*, kMaxDrawNodes> draw_free;

		// Transfer payload pool: the producer stages into the current node, the
		// consumer releases rotated-out nodes via RELEASE_PAYLOAD records.
		static constexpr u32 kMaxPayloadNodes = 8;
		std::vector<PayloadNode*> payload_arena;
		SpscRing<PayloadNode*, kMaxPayloadNodes> payload_free;

		// How much the producer waits for once it has to wait at all: an eighth
		// of the ring, a quarter of the draw pool, one payload node (they are
		// 4 MB and rotate once per transfer). Waiting for a batch keeps the
		// producer's spin from succeeding after every retired record.
		static constexpr u32 kRingRefill = RecordRing::Capacity() / 8;
		static constexpr u32 kDrawRefill = kMaxDrawNodes / 4;
		static constexpr u32 kPayloadRefill = 1;
	};
} // namespace GSBackQueue
