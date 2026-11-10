// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSDrawingContext.h"
#include "GS/GSDrawingEnvironment.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSRegs.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSVertex.h"

#include <memory>
#include <unordered_map>
#include <vector>

// The burst memo.
//
// Some games build textures every frame with bursts of hundreds of small sprite copies that the hardware
// renderer runs on the CPU (the copy road: GSRendererHW::CanUseSwPrimRender, then SwPrimRender). Most of
// those bursts repeat one from the last few frames with the same inputs: Spider-Man 3 and Call of Duty 2
// and 3 are the titles that pay for it. A copy-road draw that runs is recorded here: what it was asked to
// do (its packet), the bytes it read, the bytes of its box before it wrote them, and the bytes of its box
// after. A later draw that asks for the same thing and finds the same bytes is not run: its box is written
// from the record and its effects on the rest of the renderer are replayed (GSRendererHWBurstMemo.cpp).
//
// The draws of a burst are recorded together, as one entry, because a draw's record says nothing about the
// blocks that earlier draws of its burst wrote: those blocks hold what the earlier draws left, which the
// earlier draws' records already determine. So a burst is matched from its first draw, in order, and a
// match stops at the first draw that differs.
//
// This file holds the parts that need no renderer: the packet and its masked compare, the records, and
// the store with its history and memory budget.
namespace GSBurstMemo
{
	/// A draw's packet as the draw record delivers it to the back end: the registers of its drawing context
	/// and environment, one word of the record's own flags, its vertices and its indices. The words the
	/// draw's own state says it does not read are masked: a texture's palette fields when it has no palette,
	/// ALPHA without blending, ZBUF and vertex Z without the depth test, and so on (see Take()). Every bit
	/// that decides a mask lies in a word that is always compared, so the mask taken from the recorded draw
	/// is the mask of any draw that matches it.
	///
	/// A word is either compared whole, not compared, or (TEX0 without a palette, and PRIM) compared under a
	/// fixed partial mask, so a recorded packet keeps only the compared words, a bit set of which they are,
	/// and one flag for TEX0 (Compare()).
	struct Packet
	{
		static constexpr u32 CTX_WORDS = 12; ///< GSDrawingContext, XYOFFSET to ZBUF
		static constexpr u32 ENV_WORDS = 11; ///< GSDrawingEnvironment, PRIM to PABE
		static constexpr u32 WORDS = CTX_WORDS + ENV_WORDS + 1;
		static constexpr u32 TEX0_WORD = 1;
		static constexpr u32 PRIM_WORD = CTX_WORDS;

		/// The bit of PRIM that picks the drawing context. The context's registers are compared, not
		/// which of the two holds them.
		static constexpr u64 PRIM_CTXT_BIT = u64(1) << 9;
		/// TEX0 without its palette fields (CBP, CPSM, CSM, CSA, CLD).
		static constexpr u64 TEX0_NO_PALETTE = (u64(1) << 37) - 1;
		/// TEX0's TBP0, TBW, PSM, TW and TH: compared whenever the draw is textured.
		static constexpr u64 TEX0_WINDOW = (u64(1) << 34) - 1;

		u32 live = 0; ///< bit i: word i is compared
		bool tex0_no_palette = false; ///< TEX0 is compared without its palette fields
		std::vector<u64> words; ///< the compared words, masked, in word order
		GSVector4i vmask[2] = {GSVector4i::zero(), GSVector4i::zero()};
		std::vector<GSVertex> verts; ///< masked
		std::vector<u16> index;
		u64 signature = 0;

		/// Reads the words in Packet order.
		static void Gather(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, u64 flags, u64* out);

		/// Records a draw: its words, the masks its state implies, and its vertices and indices.
		void Take(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, u64 flags, const GSVertex* v, u32 nverts,
			const u16* idx, u32 nindex);

		/// Whether a draw asks for what the recorded one asked for, under the recorded masks.
		bool Matches(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, u64 flags, const GSVertex* v, u32 nverts,
			const u16* idx, u32 nindex) const;

		/// The compare on a packet in any storage: `words` the compared words in order, `verts` nverts masked
		/// vertices of 32 bytes each (16-byte aligned).
		static bool Compare(u32 live, bool tex0_no_palette, const u64* words, const GSVector4i* vmask, const u8* verts,
			u32 nverts, const u16* index, u32 nindex, const GSDrawingContext& ctx, const GSDrawingEnvironment& env, u64 flags,
			const GSVertex* v, u32 nv, const u16* idx, u32 ni);

		/// A hash of fields every mask keeps: two draws that match have the same signature.
		static u64 Signature(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, const GSVertex* v, u32 nverts, u32 nindex);
		u64 Signature() const { return signature; }
	};

	/// A 256-byte block of local memory and the hash its bytes had (GSTextureCache::BlockHash).
	struct BlockKey
	{
		u32 block;
		u64 hash;
	};

	/// What a recorded draw left in the renderer's per-draw members that something outside the draw can
	/// read before the next draw sets them again: the front thread's coverage check reads the cached TEST and
	/// FRAME and the last primitive class (GSState's coverage-alpha answer). The others are restored so that
	/// a skipped draw leaves the members as a run one would.
	struct LeftState
	{
		u64 test = 0; ///< HWCachedCtx::TEST after the draw's alpha and depth simplifications
		u32 fbmsk = 0; ///< HWCachedCtx::FRAME.FBMSK after them
		bool zmsk = false; ///< HWCachedCtx::ZBUF.ZMSK after them
		bool process_texture = false;
		bool fast_stencil_shadow = false;
		u8 covers = 0; ///< m_primitive_covers_without_gaps
		u8 primclass = 0; ///< m_vt.m_primclass
		GSVector4i r = GSVector4i::zero();
		GSVector4i r_no_scissor = GSVector4i::zero();
	};

	/// One copy-road draw as it ran, while it is being recorded. Entry::Append() lays it out as a Record.
	struct Draw
	{
		Packet packet;
		/// The texture blocks it read that no earlier draw of its burst wrote.
		std::vector<BlockKey> src;
		/// The blocks of its box that no earlier draw of its burst wrote, before it wrote them.
		std::vector<BlockKey> pre;
		/// Entries of the expanded palette it read (0 for none) and their hash.
		u32 palette_entries = 0;
		u64 palette_hash = 0;
		/// The verdict looked for a target at the frame (and found none): a hit has to find none too.
		bool frame_target_checked = false;
		/// One of the copy kernels drew it, rather than the rasterizer; the rasterizer also marks depth.
		bool copied = false;
		/// The box the copy road computed (GSSwPrimRenderBBox).
		GSVector4i bbox = GSVector4i::zero();
		/// The blocks of the box, and their bytes after the draw, 256 per block in the same order.
		std::vector<u32> out_blocks;
		std::vector<u8> out_bytes;
		LeftState left;
	};

	/// The fixed part of a recorded draw, at the start of its record. The rest of the record follows in this
	/// order, each part on a 16-byte boundary: the compared packet words, the masked vertices, the indices,
	/// the source blocks and their hashes, the blocks of the box first written in the burst and their hashes,
	/// the blocks of the box, and the bytes the draw left in them.
	struct alignas(16) RecordHeader
	{
		u32 bytes; ///< the whole record
		u32 live; ///< Packet::live
		/// Where each part starts, from the start of the record.
		u32 at_words, at_verts, at_index, at_src_blocks, at_src_hashes, at_pre_blocks, at_pre_hashes, at_out_blocks, at_out_bytes;
		u16 nverts, nindex, nsrc, npre, nout;
		u8 flags; ///< REC_*
		u8 nwords;
		u32 palette_entries;
		u64 palette_hash;
		GSVector4i bbox;
		GSVector4i vmask[2];
		LeftState left;
	};
	enum : u8
	{
		REC_FRAME_TARGET_CHECKED = 1,
		REC_COPIED = 2,
		REC_TEX0_NO_PALETTE = 4,
	};

	/// A record in its entry's buffer.
	struct Record
	{
		const RecordHeader* h = nullptr;
		const u64* words = nullptr;
		const u8* verts = nullptr;
		const u16* index = nullptr;
		const u32* src_blocks = nullptr;
		const u64* src_hashes = nullptr;
		const u32* pre_blocks = nullptr;
		const u64* pre_hashes = nullptr;
		const u32* out_blocks = nullptr;
		const u8* out_bytes = nullptr;

		bool Matches(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, u64 flags, const GSVertex* v, u32 nv,
			const u16* idx, u32 ni) const
		{
			return Packet::Compare(h->live, (h->flags & REC_TEX0_NO_PALETTE) != 0, words, h->vmask, verts, h->nverts, index,
				h->nindex, ctx, env, flags, v, nv, idx, ni);
		}
	};

	/// 16 bytes of an entry's buffer. Not initialised on construction, so growing the buffer does not first fill
	/// it with zeros.
	struct alignas(16) Chunk
	{
		u8 b[16];
		Chunk() {}
	};
	using Buffer = std::vector<Chunk>;

	/// The draws of one burst, laid out one record after another in one buffer, so a burst being skipped is
	/// read front to back.
	struct Entry
	{
		Buffer buf;
		std::vector<u32> offsets; ///< each record's start in `buf`
		u64 first_signature = 0; ///< the first draw's Packet::Signature()
		size_t bytes = 0;
		u64 last_used = 0; ///< Store frame of the install or the last hit
		u64 hits = 0; ///< draws skipped from this entry
		bool stored = false; ///< in the store; false once evicted

		u32 Count() const { return static_cast<u32>(offsets.size()); }
		Record At(u32 k) const;
		/// Where record k starts, for a prefetch.
		const void* Address(u32 k) const { return &buf[offsets[k]]; }
		void Append(const Draw& d);
		void Recount();
	};

	using EntryPtr = std::shared_ptr<Entry>;

	/// The entries of the last History drawn frames that had copy-road draws, indexed by the signature of
	/// their first draw, within a byte budget.
	class Store
	{
	public:
		/// Frames an entry stays without a hit or an install. A burst three frames back is common on moving
		/// scenes (probe 3: Spider-Man 3 street got 24 of its 56 points of hit rate there).
		static constexpr u64 HISTORY = 4;
		/// The default byte budget. Probe 3 kept every burst of four frames: Call of Duty 3 is the largest
		/// title, about 66 KB per burst.
		static constexpr size_t DEFAULT_BUDGET = 32u << 20;

		Store();

		void Clear();
		void SetBudget(size_t bytes);

		/// Whether an entry may begin with this signature: false is certain, true needs Find().
		bool MayHave(u64 sig) const
		{
			const u32 bit = static_cast<u32>(sig >> 52); // 12 bits
			return (m_bloom[bit >> 6] >> (bit & 63)) & 1;
		}
		const std::vector<EntryPtr>* Find(u64 sig) const;

		/// Adds a burst. May evict others (the least recently used first) to stay in budget.
		void Insert(EntryPtr e);
		/// An empty buffer for a new entry, one an evicted entry left behind if there is one: a burst being
		/// recorded then grows into memory that is already the process's, not fresh pages from the allocator.
		Buffer TakeBuffer();
		/// Records a hit: the entry stays HISTORY more frames.
		void Touch(Entry& e) { e.last_used = m_frame; }
		/// Ends a drawn frame that had copy-road draws: evicts the entries no frame of the history used.
		void EndFrame();

		u64 Frame() const { return m_frame; }
		size_t Bytes() const { return m_bytes; }
		size_t PeakBytes() const { return m_peak_bytes; }
		size_t Budget() const { return m_budget; }
		size_t Entries() const { return m_entries; }
		u64 Evictions() const { return m_evictions; }
		u64 BudgetEvictions() const { return m_budget_evictions; }

	private:
		void Remove(const EntryPtr& e);
		void RebuildBloom();
		/// Keeps a leaving entry's buffer for TakeBuffer(), unless a burst being skipped still reads it.
		void Recycle(const EntryPtr& e);

		static constexpr size_t MAX_SPARE_BUFFERS = 64;
		std::vector<Buffer> m_spare;

		std::unordered_map<u64, std::vector<EntryPtr>> m_index;
		u64 m_bloom[64] = {};
		u64 m_frame = 1;
		size_t m_bytes = 0;
		size_t m_peak_bytes = 0;
		size_t m_budget = DEFAULT_BUDGET;
		size_t m_entries = 0;
		u64 m_evictions = 0;
		u64 m_budget_evictions = 0;
	};

	/// Counts for gsrunner's statistics.
	struct Stats
	{
		u64 copy_draws = 0; ///< copy-road draws that ran
		u64 recorded = 0; ///< of those, recorded into an entry
		u64 hit_draws = 0; ///< draws skipped
		u64 sequences = 0; ///< bursts matched from their first draw
		u64 stop_packet = 0; ///< a sequence stopped: the next draw asked for something else
		u64 stop_key = 0; ///< ... it found other bytes or another palette
		u64 stop_route = 0; ///< ... a target now sits at its texture or frame
		u64 stop_write = 0; ///< ... something else wrote a block the burst had written
		u64 stop_state = 0; ///< ... the renderer was in a state the copy road would leave early
		u64 not_recorded_target = 0; ///< copy-road draws the memo does not take: their verdict weighed a target
		u64 installs = 0;
		u64 packet_checks = 0; ///< recorded draws compared with a draw in hand
		u64 key_checks = 0; ///< recorded draws whose block and palette keys were checked
		u64 key_blocks = 0; ///< blocks whose hash those checks compared
		u64 peak_bytes = 0; ///< the store's largest size
		u64 evictions = 0; ///< entries that left the history
		u64 budget_evictions = 0; ///< entries the budget pushed out
	};

	/// The counts over every renderer since the last ResetStats().
	inline Stats g_stats;
	inline Stats& GlobalStats()
	{
		return g_stats;
	}
	void ResetStats();

	/// The negative control for the identity check: every skipped draw writes one wrong byte into each block
	/// it writes (each, because a later draw of the burst rewrites the whole block). Frames that come out
	/// identical with this on would mean the check cannot see a wrong skip. Off by default; gsrunner's
	/// -burst-memo-control turns it on.
	inline bool g_control_corruption = false;
	inline void SetControlCorruption(bool on)
	{
		g_control_corruption = on;
	}
	inline bool ControlCorruption()
	{
		return g_control_corruption;
	}

	/// The memo inside a renderer: the store, the burst being recorded, and the burst being skipped.
	struct Memo
	{
		Store store;

		/// The burst being recorded: the copy-road draws that ran since the last other draw.
		EntryPtr recording;
		/// The blocks and pages its draws wrote; a set bit is a block in `recording_blocks`.
		std::vector<u32> recording_blocks;
		std::vector<u32> recording_pages;
		u64 recording_written[GS_MAX_BLOCKS / 64] = {};
		/// GSLocalMemory::WriteSeq() after its last draw.
		u64 recording_seq = 0;

		/// The burst being skipped: the entries whose first `position` draws it matched, the pages it
		/// wrote, and WriteSeq() after its last draw.
		std::vector<EntryPtr> candidates;
		u32 position = 0;
		std::vector<u32> sequence_pages;
		u64 sequence_seq = 0;

		/// The draw before this one went down the copy road, run or skipped.
		bool last_was_copy = false;
		/// This frame had copy-road draws: it counts towards the history.
		bool frame_had_copies = false;

		/// The draw in hand passed the copy road's register test, and its vertices before texel rounding.
		bool candidate = false;
		std::vector<GSVertex> raw_verts;

		/// The draw in hand, between BurstMemoBeforeCopy and BurstMemoAfterCopy.
		Draw pending;
		/// Scratch for the texture blocks of the draw in hand.
		std::vector<u32> scratch_blocks;
		/// The candidates' records at `position`, beside `candidates`.
		std::vector<Record> records;

		/// The last palette hashed: GSClut::ReadGeneration(), entries, hash.
		u64 palette_generation = ~u64(0);
		u32 palette_entries = 0;
		u64 palette_hash = 0;
	};
} // namespace GSBurstMemo
