// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The burst memo where it meets the renderer. GSBurstMemo.h says what the memo is.
//
// A draw passes three places:
//
// 1. SkipMemoizedDraw(), from the draw executor before the vertex trace. A draw that continues a burst being
//    skipped, or begins one the store holds, that finds the bytes and the palette the recorded draw found,
//    and finds no target where the copy road would look for one, is skipped: BurstMemoApply() writes its box
//    from the record and does what the copy road does besides.
// 2. BurstMemoBeforeCopy(), in Draw() when the copy road takes a draw that was not skipped: the record's keys
//    are taken while memory still holds what the draw will read.
// 3. BurstMemoAfterCopy(): the bytes the draw left in its box and in the renderer's members.
//
// Why a skipped draw is exact. A copy-road draw's output is decided by its packet, the bytes it reads (its
// texels, its palette, and the bytes of its box that it does not overwrite) and the copy road's verdict,
// which reads the texture cache only to look for a target at the texture and at the frame. A record keys all
// of them except the bytes of blocks an earlier draw of the same burst wrote. Those hold what the earlier
// draws left, and the earlier draws matched, unless something else wrote them in between, which the page
// stamps tell. A skipped draw writes the bytes the recorded draw left in its whole box, then marks the pages,
// invalidates the texture cache over its box and queues its draw transfer, in the copy road's order. So local
// memory, its stamps and the texture cache end where a run copy leaves them, and nothing that reads local
// memory needs to know the memo exists.
//
// What is not taken: draws whose verdict found a target, and everything while the copy road's own early exits
// could fire differently (a channel shuffle in progress, a split texture shuffle, a pending colour clip
// resolve, a game's skip-draw hook). A mipmapped draw is keyed on every level the copy road can read.

#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/HW/GSDrawLog.h"
#include "GS/GSPerfMon.h"
#include "GS/GSXXH.h"

#include <algorithm>
#include <cstring>

using namespace GSBurstMemo;

static __fi bool TestBit(const u64* bits, u32 block)
{
	return (bits[block >> 6] >> (block & 63)) & 1;
}

void GSRendererHW::RefreshBurstMemo()
{
	Memo& m = m_burst_memo;
	m.store.Clear();
	m.recording.reset();
	for (const u32 block : m.recording_blocks)
		m.recording_written[block >> 6] = 0;
	m.recording_blocks.clear();
	m.recording_pages.clear();
	m.candidates.clear();
	m.position = 0;
	m.sequence_pages.clear();
	m.last_was_copy = false;
	m.frame_had_copies = false;
	m.candidate = false;
	m.palette_generation = ~u64(0);

	// The copy road has to be on, and nothing may be watching or changing draws the memo would skip.
	m_burst_memo_enabled = GSConfig.BurstMemo && GSConfig.UserHacks_CPUSpriteRenderBW != 0 && !GSConfig.DumpGSData &&
	                       !GSConfig.UserHacks_RewriteLargeSTCoords &&
	                       GSConfig.UserHacks_GPUTargetCLUTMode == GSGPUTargetCLUTMode::Disabled && !m_gsc &&
	                       GSConfig.SkipDrawEnd == 0;
}

bool GSRendererHW::SkipMemoizedDraw()
{
	if (!m_burst_memo_enabled)
		return false;

	Memo& m = m_burst_memo;
	m.candidate = false;

	// A burst does not run on across a draw that was not a copy.
	const bool after_copy = m.last_was_copy;
	m.last_was_copy = false;
	if (!after_copy)
	{
		BurstMemoCloseRecording();
		BurstMemoEndSequence();
	}

	// The head of CanUseSwPrimRender, from the registers alone: a frame too wide never takes the copy road.
	const u32 fbw = m_context->FRAME.FBW;
	if ((fbw > GSConfig.UserHacks_CPUSpriteRenderBW && fbw != 32) || GSDrawLog::IsActive() || !m_sw_prim.palette_block_copy)
	{
		BurstMemoCloseRecording();
		BurstMemoEndSequence();
		return false;
	}

	m.candidate = true;
	if (BurstMemoTryHit())
	{
		m.last_was_copy = true;
		m.frame_had_copies = true;
		return true;
	}

	// The draw runs. Texel rounding may change its vertices before the copy road records them.
	m.raw_verts.assign(m_vertex->buff, m_vertex->buff + m_vertex->next);
	return false;
}

bool GSRendererHW::BurstMemoTryHit()
{
	Memo& m = m_burst_memo;
	Stats& st = GlobalStats();

	// Where Draw() would leave before the copy road, or the verdict rest on more than the record holds.
	if (m_channel_shuffle || m_split_texture_shuffle_pages || g_gs_device->GetColorClipTexture())
	{
		if (!m.candidates.empty())
		{
			st.stop_state++;
			BurstMemoEndSequence();
		}
		return false;
	}

	const GSDrawingContext& ctx = *m_context;
	const GSDrawingEnvironment& env = *m_draw_env;
	const u64 flags = m_isPackedUV_HackFlag;
	const GSVertex* v = m_vertex->buff;
	const u32 nv = m_vertex->next;
	const u16* idx = m_index->buff;
	const u32 ni = m_index->tail;

	// Continue the burst being skipped: keep the entries whose next draw this one matches. Filtered in place,
	// since moving a shared pointer leaves its count alone.
	if (!m.candidates.empty())
	{
		size_t keep = 0;
		m.records.resize(m.candidates.size());
		for (size_t i = 0; i < m.candidates.size(); i++)
		{
			const Entry& e = *m.candidates[i];
			st.packet_checks++;
			if (e.Count() <= m.position)
				continue;
			const GSBurstMemo::Record r = e.At(m.position);
			if (r.Matches(ctx, env, flags, v, nv, idx, ni))
			{
				if (keep != i)
					m.candidates[keep] = std::move(m.candidates[i]);
				m.records[keep] = r;
				keep++;
			}
		}
		m.candidates.resize(keep);
		m.records.resize(keep);
		if (!keep)
		{
			st.stop_packet++;
			BurstMemoEndSequence();
		}
		else if (BurstMemoHit())
		{
			return true;
		}
	}

	// Begin one: the entries whose first draw this one matches.
	const u64 sig = GSBurstMemo::Packet::Signature(ctx, env, v, nv, ni);
	if (!m.store.MayHave(sig))
		return false;
	const std::vector<EntryPtr>* list = m.store.Find(sig);
	if (!list)
		return false;
	m.candidates.clear();
	m.records.clear();
	for (const EntryPtr& e : *list)
	{
		st.packet_checks++;
		const GSBurstMemo::Record r = e->At(0);
		if (r.Matches(ctx, env, flags, v, nv, idx, ni))
		{
			m.candidates.push_back(e);
			m.records.push_back(r);
		}
	}
	if (m.candidates.empty())
		return false;

	m.position = 0;
	m.sequence_pages.clear();
	if (!BurstMemoHit())
		return false;
	st.sequences++;
	return true;
}

bool GSRendererHW::BurstMemoHit()
{
	Memo& m = m_burst_memo;
	Stats& st = GlobalStats();
	const u32 k = m.position;
	const GSDrawingContext& ctx = *m_context;

	// Something wrote a page the burst wrote since its last skipped draw: the bytes the earlier draws left
	// there may not be what the recorded draws found.
	if (k > 0 && BurstMemoPagesWrittenSince(m.sequence_pages, m.sequence_seq))
	{
		st.stop_write++;
		BurstMemoEndSequence();
		return false;
	}

	// The palette the copy reads, loaded the way Draw() loads it before the copy road. The candidates matched
	// this draw's packet, so they agree on whether there is one.
	const u32 entries = m.records[0].h->palette_entries;
	u64 palette = 0;
	if (entries)
	{
		m_mem.m_clut.Read32(ctx.TEX0, m_draw_env->TEXA);
		palette = BurstMemoPaletteHash(entries);
	}

	size_t keep = 0;
	for (size_t i = 0; i < m.candidates.size(); i++)
	{
		const GSBurstMemo::Record d = m.records[i];
		st.key_checks++;
		st.key_blocks += d.h->nsrc + d.h->npre;
		if (entries && d.h->palette_hash != palette)
			continue;
		if (!g_texture_cache->BlockHashesMatch(m_mem, d.src_blocks, d.src_hashes, d.h->nsrc) ||
			!g_texture_cache->BlockHashesMatch(m_mem, d.pre_blocks, d.pre_hashes, d.h->npre))
			continue;
		if (keep != i)
		{
			m.candidates[keep] = std::move(m.candidates[i]);
			m.records[keep] = d;
		}
		keep++;
	}
	m.candidates.resize(keep);
	m.records.resize(keep);
	if (!keep)
	{
		st.stop_key++;
		BurstMemoEndSequence();
		return false;
	}

	// The verdict looked for a target at the texture, and maybe at the frame, and found none.
	const GSBurstMemo::Record d = m.records[0];
	if ((PRIM->TME && BurstMemoTargetAt(ctx.TEX0.TBP0, ctx.TEX0.PSM)) ||
		((d.h->flags & GSBurstMemo::REC_FRAME_TARGET_CHECKED) && BurstMemoTargetAt(ctx.FRAME.Block(), ctx.FRAME.PSM)))
	{
		st.stop_route++;
		BurstMemoEndSequence();
		return false;
	}

	// The copy-road draws before this one are a burst of their own.
	BurstMemoCloseRecording();

	BurstMemoApply(d);

	for (const EntryPtr& e : m.candidates)
	{
		m.store.Touch(*e);
		e->hits++;
	}
	for (u32 i = 0; i < d.h->nout; i++)
	{
		const u32 page = d.out_blocks[i] >> 5;
		if (std::find(m.sequence_pages.begin(), m.sequence_pages.end(), page) == m.sequence_pages.end())
			m.sequence_pages.push_back(page);
	}
	m.sequence_seq = m_mem.WriteSeq();
	m.position = k + 1;
	st.hit_draws++;

	// The burst's next draw is the next record: start loading its header and words while the next draw is
	// decoded.
	const Entry& next = *m.candidates[0];
	if (next.Count() > k + 1)
	{
		const char* p = static_cast<const char*>(next.Address(k + 1));
		__builtin_prefetch(p);
		__builtin_prefetch(p + 64);
		__builtin_prefetch(p + 128);
		__builtin_prefetch(p + 192);
	}
	return true;
}

void GSRendererHW::BurstMemoApply(const GSBurstMemo::Record& d)
{
	const GSDrawingContext* context = m_context;
	const bool copied = (d.h->flags & GSBurstMemo::REC_COPIED) != 0;

	// What the kernel wrote: the whole blocks of the box, as the recorded draw left them.
	u8* const vm = m_mem.m_vm8;
	const u32 blocks = d.h->nout;
	for (u32 i = 0; i < blocks; i++)
		std::memcpy(vm + static_cast<size_t>(d.out_blocks[i]) * GS_BLOCK_SIZE, d.out_bytes + static_cast<size_t>(i) * GS_BLOCK_SIZE, GS_BLOCK_SIZE);
	if (ControlCorruption()) [[unlikely]]
	{
		for (u32 i = 0; i < blocks; i++)
			vm[static_cast<size_t>(d.out_blocks[i]) * GS_BLOCK_SIZE + 17] ^= 0x5A;
	}

	// What GSSwPrimRenderFunctions::Run() did after the kernel: the marks, frame and, for the rasterizer, depth.
	const GSVector4i bbox = d.h->bbox;
	const GSVector4i grown(std::max(bbox.x - 1, 0), std::max(bbox.y - 1, 0), bbox.z + 1, bbox.w + 1);
	if (copied)
		g_perfmon.Put(GSPerfMon::SwPaletteBlockCopies, 1);
	m_mem.MarkPagesWritten(context->offset.fb, grown);
	if (!copied)
		m_mem.MarkPagesWritten(context->offset.zb, grown);

	// What SwPrimRender() did after Run(): the texture cache invalidation and the draw transfer.
	GSOffset frame_offs = context->offset.fb;
	if (GSLocalMemory::m_psm[context->FRAME.PSM].trbpp == 32 && context->FRAME.FBMSK)
	{
		if (context->FRAME.FBMSK == 0xFF000000)
			frame_offs = m_mem.GetOffset(context->FRAME.Block(), context->FRAME.FBW, PSMCT24);
		else if (context->FRAME.FBMSK == 0x00FFFFFF)
			frame_offs = m_mem.GetOffset(context->FRAME.Block(), context->FRAME.FBW, PSMT8H);
	}
	g_texture_cache->InvalidateVideoMem(frame_offs, bbox);

	GSUploadQueue uq;
	uq.transfer_type = EEGS_TransferType::EE_to_GS;
	uq.blit.U64 = 0;
	uq.blit.DBP = context->FRAME.Block();
	uq.blit.DBW = context->FRAME.FBW;
	uq.blit.DPSM = context->FRAME.PSM;
	uq.draw = s_n;
	uq.rect = bbox;
	m_draw_transfers.push_back(uq);

	// What Draw() left in its members on the way to the copy road.
	const LeftState& left = d.h->left;
	m_draw_color_dispatch_accepted = false;
	m_cached_ctx.TEX0 = context->TEX0;
	m_cached_ctx.TEXA = m_draw_env->TEXA;
	m_cached_ctx.CLAMP = context->CLAMP;
	m_cached_ctx.TEST.U64 = left.test;
	m_cached_ctx.FRAME = context->FRAME;
	m_cached_ctx.FRAME.FBMSK = left.fbmsk;
	m_cached_ctx.ZBUF = context->ZBUF;
	m_cached_ctx.ZBUF.ZMSK = left.zmsk;
	m_last_channel_shuffle_fbp = 0xffff;
	m_last_channel_shuffle_tbp = 0xffff;
	m_last_channel_shuffle_end_block = 0xffff;
	m_last_rt = nullptr;
	m_channel_shuffle_width = 0;
	m_full_screen_shuffle = false;
	m_channel_shuffle_finish = false;
	m_channel_shuffle_src_valid = GSVector4i::zero();
	m_r = left.r;
	m_r_no_scissor = left.r_no_scissor;
	m_process_texture = left.process_texture;
	m_fast_stencil_shadow = left.fast_stencil_shadow;
	m_primitive_covers_without_gaps = static_cast<NoGapsType>(left.covers);
	m_vt.m_primclass = static_cast<GS_PRIM_CLASS>(left.primclass);
}

bool GSRendererHW::BurstMemoBeforeCopy()
{
	Memo& m = m_burst_memo;
	if (!m_burst_memo_enabled || !m.candidate)
		return false;
	m.candidate = false;

	if (!m_copy_road_no_targets)
	{
		GlobalStats().not_recorded_target++;
		BurstMemoCloseRecording();
		return false;
	}

	// Something wrote a page the burst wrote since its last draw: this draw begins a new burst, whose
	// keys see those bytes.
	if (m.recording && BurstMemoPagesWrittenSince(m.recording_pages, m.recording_seq))
		BurstMemoCloseRecording();
	if (!m.recording)
	{
		m.recording = std::make_shared<Entry>();
		m.recording->buf = m.store.TakeBuffer();
	}

	const GSDrawingContext& ctx = *m_context;
	// The pending draw is reused, so its arrays keep their capacity from one draw to the next.
	GSBurstMemo::Draw& d = m.pending;
	d.src.clear();
	d.pre.clear();
	d.out_blocks.clear();
	d.out_bytes.clear();
	d.palette_entries = 0;
	d.palette_hash = 0;
	d.packet.Take(ctx, *m_draw_env, m_isPackedUV_HackFlag, m.raw_verts.data(), static_cast<u32>(m.raw_verts.size()),
		m_index->buff, m_index->tail);
	d.frame_target_checked = m_copy_road_frame_checked;
	d.bbox = GSSwPrimRenderBBox(m_vt, ctx.scissor.in);

	// The blocks of the box; those no earlier draw of the burst wrote are keyed as they are now.
	ctx.offset.fb.loopBlocks(d.bbox, [&d](u32 block) { d.out_blocks.push_back(block % GS_MAX_BLOCKS); });
	std::sort(d.out_blocks.begin(), d.out_blocks.end());
	d.out_blocks.erase(std::unique(d.out_blocks.begin(), d.out_blocks.end()), d.out_blocks.end());
	for (const u32 block : d.out_blocks)
	{
		if (!TestBit(m.recording_written, block))
			d.pre.push_back({block, g_texture_cache->BlockHash(m_mem, block)});
	}

	if (PRIM->TME)
	{
		// The texels the draw reads: its coverage, or the whole window when the coverage is not inside it.
		const GSVector4i whole(0, 0, 1 << std::min<u32>(ctx.TEX0.TW, 10), 1 << std::min<u32>(ctx.TEX0.TH, 10));
		GSVector4i cov(GetTextureMinMax(m_cached_ctx.TEX0, m_cached_ctx.CLAMP, m_vt.IsLinear(), false).coverage);
		if (cov.rempty() || cov.left < 0 || cov.top < 0 || cov.right > whole.right || cov.bottom > whole.bottom)
			cov = whole;
		std::vector<u32>& src = m.scratch_blocks;
		src.clear();
		m_mem.GetOffset(ctx.TEX0.TBP0, ctx.TEX0.TBW, ctx.TEX0.PSM).loopBlocks(cov, [&src](u32 block) { src.push_back(block % GS_MAX_BLOCKS); });

		// A mipmapped draw reads the other levels too, every one up to MXL (GSSwPrimRenderFunctions::Run). The
		// copy road's coverage of level i lies inside this level's coverage scaled down by 2^i, rounded outwards
		// and widened by two texels (its coordinates and its clamp region are this level's halved i times, and
		// its filter reaches one texel further), so that rectangle is keyed: a superset of what it reads.
		if (IsMipMapActive())
		{
			for (u32 i = 1, last = std::min<u32>(ctx.TEX1.MXL, 6); i <= last; i++)
			{
				const GIFRegTEX0 level = GetTex0Layer(i);
				const GSVector4i level_whole(0, 0, 1 << std::min<u32>(level.TW, 10), 1 << std::min<u32>(level.TH, 10));
				const int round = (1 << i) - 1;
				const GSVector4i r = GSVector4i((cov.left >> i) - 2, (cov.top >> i) - 2, ((cov.right + round) >> i) + 2,
					((cov.bottom + round) >> i) + 2)
				                         .rintersect(level_whole);
				m_mem.GetOffset(level.TBP0, level.TBW, level.PSM).loopBlocks(r.rempty() ? level_whole : r, [&src](u32 block) { src.push_back(block % GS_MAX_BLOCKS); });
			}
		}
		std::sort(src.begin(), src.end());
		src.erase(std::unique(src.begin(), src.end()), src.end());
		for (const u32 block : src)
		{
			if (!TestBit(m.recording_written, block))
				d.src.push_back({block, g_texture_cache->BlockHash(m_mem, block)});
		}

		// Draw() has loaded the palette for this draw before the copy road.
		const u32 entries = GSLocalMemory::m_psm[ctx.TEX0.PSM].pal;
		if (entries)
		{
			d.palette_entries = entries;
			d.palette_hash = BurstMemoPaletteHash(entries);
		}
	}
	return true;
}

void GSRendererHW::BurstMemoAfterCopy(bool recorded, bool copied)
{
	if (!m_burst_memo_enabled || !copied)
		return;

	Memo& m = m_burst_memo;
	Stats& st = GlobalStats();
	st.copy_draws++;
	m.frame_had_copies = true;
	if (!recorded)
		return;

	GSBurstMemo::Draw& d = m.pending;
	d.copied = m_sw_prim.last_draw_copied;
	d.out_bytes.resize(d.out_blocks.size() * GS_BLOCK_SIZE);
	const u8* const vm = m_mem.m_vm8;
	for (size_t i = 0; i < d.out_blocks.size(); i++)
		std::memcpy(&d.out_bytes[i * GS_BLOCK_SIZE], vm + static_cast<size_t>(d.out_blocks[i]) * GS_BLOCK_SIZE, GS_BLOCK_SIZE);

	LeftState& left = d.left;
	left.test = m_cached_ctx.TEST.U64;
	left.fbmsk = m_cached_ctx.FRAME.FBMSK;
	left.zmsk = m_cached_ctx.ZBUF.ZMSK;
	left.process_texture = m_process_texture;
	left.fast_stencil_shadow = m_fast_stencil_shadow;
	left.covers = static_cast<u8>(m_primitive_covers_without_gaps);
	left.primclass = static_cast<u8>(m_vt.m_primclass);
	left.r = m_r;
	left.r_no_scissor = m_r_no_scissor;

	for (const u32 block : d.out_blocks)
	{
		if (TestBit(m.recording_written, block))
			continue;
		m.recording_written[block >> 6] |= u64(1) << (block & 63);
		m.recording_blocks.push_back(block);
		const u32 page = block >> 5;
		if (std::find(m.recording_pages.begin(), m.recording_pages.end(), page) == m.recording_pages.end())
			m.recording_pages.push_back(page);
	}
	m.recording->Append(d);
	m.recording_seq = m_mem.WriteSeq();
	m.last_was_copy = true;
	st.recorded++;
}

void GSRendererHW::BurstMemoCloseRecording()
{
	Memo& m = m_burst_memo;
	if (!m.recording)
		return;
	if (m.recording->Count())
	{
		m.recording->Recount();
		m.store.Insert(std::move(m.recording));
		Stats& st = GlobalStats();
		st.installs++;
		st.peak_bytes = std::max<u64>(st.peak_bytes, m.store.PeakBytes());
		st.budget_evictions = m.store.BudgetEvictions();
	}
	m.recording.reset();
	for (const u32 block : m.recording_blocks)
		m.recording_written[block >> 6] = 0;
	m.recording_blocks.clear();
	m.recording_pages.clear();
}

void GSRendererHW::BurstMemoEndSequence()
{
	Memo& m = m_burst_memo;
	m.candidates.clear();
	m.records.clear();
	m.position = 0;
	m.sequence_pages.clear();
}

void GSRendererHW::BurstMemoEndFrame(bool idle_frame)
{
	if (!m_burst_memo_enabled)
		return;
	Memo& m = m_burst_memo;
	BurstMemoCloseRecording();
	BurstMemoEndSequence();
	m.last_was_copy = false;
	if (m.frame_had_copies && !idle_frame)
	{
		m.store.EndFrame();
		GlobalStats().evictions = m.store.Evictions();
	}
	m.frame_had_copies = false;
}

bool GSRendererHW::BurstMemoPagesWrittenSince(const std::vector<u32>& pages, u64 seq) const
{
	if (m_mem.WriteSeq() == seq)
		return false;
	for (const u32 page : pages)
	{
		if (m_mem.PageStamp(page) > seq)
			return true;
	}
	return false;
}

u64 GSRendererHW::BurstMemoPaletteHash(u32 entries)
{
	Memo& m = m_burst_memo;
	const u64 generation = m_mem.m_clut.ReadGeneration();
	if (generation != m.palette_generation || entries != m.palette_entries)
	{
		m.palette_hash = GSXXH3_64bits(static_cast<const u32*>(m_mem.m_clut), entries * sizeof(u32));
		m.palette_generation = generation;
		m.palette_entries = entries;
	}
	return m.palette_hash;
}

bool GSRendererHW::BurstMemoTargetAt(u32 bp, u32 psm) const
{
	// Every target is indexed over its pages, so none in reach of the page means none at it.
	if (!GSTextureCache::TargetsMayReach(bp, bp))
		return false;
	return g_texture_cache->GetTargetWithSharedBits(bp, psm) != nullptr;
}
