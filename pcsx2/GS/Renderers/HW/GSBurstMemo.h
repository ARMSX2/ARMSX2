// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSDrawingContext.h"
#include "GS/GSDrawingEnvironment.h"
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
	struct Packet
	{
		static constexpr u32 CTX_WORDS = 12; ///< GSDrawingContext, XYOFFSET to ZBUF
		static constexpr u32 ENV_WORDS = 11; ///< GSDrawingEnvironment, PRIM to PABE
		static constexpr u32 WORDS = CTX_WORDS + ENV_WORDS + 1;

		/// The bit of PRIM that picks the drawing context. The context's registers are compared, not
		/// which of the two holds them.
		static constexpr u64 PRIM_CTXT_BIT = u64(1) << 9;
		/// TEX0 without its palette fields (CBP, CPSM, CSM, CSA, CLD).
		static constexpr u64 TEX0_NO_PALETTE = (u64(1) << 37) - 1;
		/// TEX0's TBP0, TBW, PSM, TW and TH: compared whenever the draw is textured.
		static constexpr u64 TEX0_WINDOW = (u64(1) << 34) - 1;

		u64 words[WORDS] = {};
		u64 mask[WORDS] = {};
		GSVector4i vmask[2] = {GSVector4i::zero(), GSVector4i::zero()};
		std::vector<GSVertex> verts;
		std::vector<u16> index;

		/// Reads the words in Packet order.
		static void Gather(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, u64 flags, u64* out);

		/// Records a draw: its words, the masks its state implies, and its vertices and indices.
		void Take(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, u64 flags, const GSVertex* v, u32 nverts,
			const u16* idx, u32 nindex);

		/// Whether a draw asks for what the recorded one asked for, under the recorded masks.
		bool Matches(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, u64 flags, const GSVertex* v, u32 nverts,
			const u16* idx, u32 nindex) const;

		/// A hash of fields every mask keeps: two draws that match have the same signature.
		static u64 Signature(const GSDrawingContext& ctx, const GSDrawingEnvironment& env, const GSVertex* v, u32 nverts, u32 nindex);
		u64 Signature() const;

		size_t Bytes() const { return verts.size() * sizeof(GSVertex) + index.size() * sizeof(u16); }
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

	/// One copy-road draw as it ran.
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

		size_t Bytes() const;
	};

	/// The draws of one burst, in order.
	struct Entry
	{
		std::vector<Draw> draws;
		size_t bytes = 0;
		u64 last_used = 0; ///< Store frame of the install or the last hit
		u64 hits = 0; ///< draws skipped from this entry
		bool stored = false; ///< in the store; false once evicted

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
		u64 not_recorded = 0; ///< copy-road draws the memo does not take (mipmapped, or routed past a target)
		u64 installs = 0;
	};
} // namespace GSBurstMemo
