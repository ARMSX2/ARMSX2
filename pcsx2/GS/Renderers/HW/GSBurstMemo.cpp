// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSBurstMemo.h"
#include "GS/GSLocalMemory.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>

namespace GSBurstMemo
{
	void ResetStats()
	{
		g_stats = {};
	}

	// Word positions in Packet order.
	enum : u32
	{
		W_XYOFFSET = 0,
		W_TEX0,
		W_TEX1,
		W_CLAMP,
		W_MIPTBP1,
		W_MIPTBP2,
		W_SCISSOR,
		W_ALPHA,
		W_TEST,
		W_FBA,
		W_FRAME,
		W_ZBUF,
		W_PRIM,
		W_PRMODE,
		W_PRMODECONT,
		W_TEXCLUT,
		W_SCANMSK,
		W_TEXA,
		W_FOGCOL,
		W_DIMX,
		W_DTHE,
		W_COLCLAMP,
		W_PABE,
		W_FLAGS,
		W_COUNT
	};
	static_assert(W_COUNT == Packet::WORDS);
	static_assert(W_PRIM == Packet::CTX_WORDS);

	void Packet::Gather(const GSDrawingContext& c, const GSDrawingEnvironment& e, u64 flags, u64* w)
	{
		w[W_XYOFFSET] = c.XYOFFSET.U64;
		w[W_TEX0] = c.TEX0.U64;
		w[W_TEX1] = c.TEX1.U64;
		w[W_CLAMP] = c.CLAMP.U64;
		w[W_MIPTBP1] = c.MIPTBP1.U64;
		w[W_MIPTBP2] = c.MIPTBP2.U64;
		w[W_SCISSOR] = c.SCISSOR.U64;
		w[W_ALPHA] = c.ALPHA.U64;
		w[W_TEST] = c.TEST.U64;
		w[W_FBA] = c.FBA.U64;
		w[W_FRAME] = c.FRAME.U64;
		w[W_ZBUF] = c.ZBUF.U64;
		w[W_PRIM] = e.PRIM.U64;
		w[W_PRMODE] = e.PRMODE.U64;
		w[W_PRMODECONT] = e.PRMODECONT.U64;
		w[W_TEXCLUT] = e.TEXCLUT.U64;
		w[W_SCANMSK] = e.SCANMSK.U64;
		w[W_TEXA] = e.TEXA.U64;
		w[W_FOGCOL] = e.FOGCOL.U64;
		w[W_DIMX] = e.DIMX.U64;
		w[W_DTHE] = e.DTHE.U64;
		w[W_COLCLAMP] = e.COLCLAMP.U64;
		w[W_PABE] = e.PABE.U64;
		w[W_FLAGS] = flags;
	}

	// Byte masks of a GSVertex's two 16-byte halves: ST 0-7, RGBA 8-11, Q 12-15, then XY 16-19, Z 20-23,
	// UV 24-27, FOG 28-31.
	static GSVector4i VertexHalf(bool lo, bool hi_lo, bool lo_hi, bool hi)
	{
		const int a = lo ? -1 : 0, b = hi_lo ? -1 : 0, c = lo_hi ? -1 : 0, d = hi ? -1 : 0;
		return GSVector4i(a, b, c, d);
	}

	void Packet::Take(const GSDrawingContext& c, const GSDrawingEnvironment& e, u64 flags, const GSVertex* v, u32 nverts,
		const u16* idx, u32 nindex)
	{

		const bool tme = e.PRIM.TME;
		const bool fst = e.PRIM.FST;
		const bool abe = e.PRIM.ABE;
		const bool fge = e.PRIM.FGE;
		const bool zte = c.TEST.ZTE;
		const bool dthe = e.DTHE.DTHE;
		const GSLocalMemory::psm_t& tpsm = GSLocalMemory::m_psm[c.TEX0.PSM];
		const bool pal = tme && tpsm.pal > 0;
		const bool tex16 = tme && (tpsm.bpp == 16 || tpsm.trbpp == 24 || (pal && GSLocalMemory::m_psm[c.TEX0.CPSM].bpp == 16));
		constexpr u64 all = ~u64(0);

		u64 w[WORDS];
		Gather(c, e, flags, w);
		u64 mask[WORDS];
		for (u64& m : mask)
			m = all;
		mask[W_TEX0] = tme ? (pal ? all : TEX0_NO_PALETTE) : 0;
		mask[W_TEX1] = tme ? all : 0;
		mask[W_CLAMP] = tme ? all : 0;
		mask[W_MIPTBP1] = tme ? all : 0;
		mask[W_MIPTBP2] = tme ? all : 0;
		mask[W_ALPHA] = abe ? all : 0;
		mask[W_ZBUF] = zte ? all : 0;
		mask[W_PRIM] = all & ~PRIM_CTXT_BIT;
		mask[W_TEXCLUT] = (pal && c.TEX0.CSM) ? all : 0;
		mask[W_TEXA] = tex16 ? all : 0;
		mask[W_FOGCOL] = fge ? all : 0;
		mask[W_DIMX] = dthe ? all : 0;
		mask[W_PABE] = abe ? all : 0;
		tex0_no_palette = tme && !pal;
		live = 0;
		words.clear();
		for (u32 i = 0; i < WORDS; i++)
		{
			if (!mask[i])
				continue;
			live |= 1u << i;
			words.push_back(w[i] & mask[i]);
		}

		const bool stq = tme && !fst;
		vmask[0] = VertexHalf(stq, stq, true, stq);
		vmask[1] = VertexHalf(true, zte, tme && fst, fge);

		verts.resize(nverts);
		for (u32 i = 0; i < nverts; i++)
		{
			GSVector4i::store<true>(&verts[i].m[0], GSVector4i(v[i].m[0]) & vmask[0]);
			GSVector4i::store<true>(&verts[i].m[1], GSVector4i(v[i].m[1]) & vmask[1]);
		}
		index.assign(idx, idx + nindex);
		signature = Signature(c, e, v, nverts, nindex);
	}

	bool Packet::Compare(u32 live, bool tex0_no_palette, const u64* words, const GSVector4i* vmask, const u8* verts, u32 nverts,
		const u16* index, u32 nindex, const GSDrawingContext& c, const GSDrawingEnvironment& e, u64 flags, const GSVertex* v, u32 nv,
		const u16* idx, u32 ni)
	{
		if (nv != nverts || ni != nindex)
			return false;

		u64 w[WORDS];
		Gather(c, e, flags, w);
		w[W_PRIM] &= ~PRIM_CTXT_BIT;
		if (tex0_no_palette)
			w[W_TEX0] &= TEX0_NO_PALETTE;
		u64 diff = 0;
		for (u32 bits = live; bits; bits &= bits - 1)
			diff |= w[std::countr_zero(bits)] ^ *words++;
		if (diff)
			return false;

		GSVector4i vdiff = GSVector4i::zero();
		for (u32 i = 0; i < nv; i++, verts += sizeof(GSVertex))
		{
			vdiff |= (GSVector4i(v[i].m[0]) & vmask[0]) ^ GSVector4i::load<false>(verts);
			vdiff |= (GSVector4i(v[i].m[1]) & vmask[1]) ^ GSVector4i::load<false>(verts + 16);
		}
		if (!vdiff.eq(GSVector4i::zero()))
			return false;

		return ni == 0 || std::memcmp(idx, index, ni * sizeof(u16)) == 0;
	}

	bool Packet::Matches(const GSDrawingContext& c, const GSDrawingEnvironment& e, u64 flags, const GSVertex* v, u32 nv,
		const u16* idx, u32 ni) const
	{
		return Compare(live, tex0_no_palette, words.data(), vmask, reinterpret_cast<const u8*>(verts.data()),
			static_cast<u32>(verts.size()), index.data(), static_cast<u32>(index.size()), c, e, flags, v, nv, idx, ni);
	}

	// The signature mixes fields no mask removes: FRAME, XYOFFSET, PRIM without the context bit, the first
	// vertex's position and the index count, and the texture's window when the draw is textured (PRIM.TME
	// is in the PRIM word, so a textured draw never matches an untextured one).
	static u64 SignatureOf(u64 frame, u64 xyoffset, u64 prim, u64 tex0, u32 xy0, u32 nindex)
	{
		u64 h = frame * 0x9E3779B97F4A7C15ull;
		h ^= (xyoffset + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
		h ^= ((prim & ~Packet::PRIM_CTXT_BIT) + (static_cast<u64>(nindex) << 32)) * 0x165667B19E3779F9ull;
		h ^= (static_cast<u64>(xy0) + 0x27D4EB2F165667C5ull) * 0x94D049BB133111EBull;
		if (prim & (u64(1) << 4)) // TME
			h ^= ((tex0 & Packet::TEX0_WINDOW) + 0x85EBCA77C2B2AE63ull) * 0xBF58476D1CE4E5B9ull;
		return h ^ (h >> 29);
	}

	u64 Packet::Signature(const GSDrawingContext& c, const GSDrawingEnvironment& e, const GSVertex* v, u32 nverts, u32 nindex)
	{
		const u32 xy0 = nverts ? (static_cast<u32>(v[0].XYZ.X) | (static_cast<u32>(v[0].XYZ.Y) << 16)) : 0;
		return SignatureOf(c.FRAME.U64, c.XYOFFSET.U64, e.PRIM.U64, c.TEX0.U64, xy0, nindex);
	}

	static constexpr size_t Align16(size_t n)
	{
		return (n + 15) & ~size_t(15);
	}

	// Byte offsets of a record's parts, from its counts (RecordHeader).
	struct Layout
	{
		size_t words, verts, index, src_blocks, src_hashes, pre_blocks, pre_hashes, out_blocks, out_bytes, end;

		Layout(u32 nwords, u32 nverts, u32 nindex, u32 nsrc, u32 npre, u32 nout)
		{
			size_t o = Align16(sizeof(RecordHeader));
			words = o;
			o = Align16(o + nwords * sizeof(u64));
			verts = o;
			o = Align16(o + nverts * sizeof(GSVertex));
			index = o;
			o = Align16(o + nindex * sizeof(u16));
			src_blocks = o;
			o = Align16(o + nsrc * sizeof(u32));
			src_hashes = o;
			o = Align16(o + nsrc * sizeof(u64));
			pre_blocks = o;
			o = Align16(o + npre * sizeof(u32));
			pre_hashes = o;
			o = Align16(o + npre * sizeof(u64));
			out_blocks = o;
			o = Align16(o + nout * sizeof(u32));
			out_bytes = o;
			end = Align16(o + nout * GS_BLOCK_SIZE);
		}
	};

	SlabPool::~SlabPool()
	{
		if (m_cur && --m_cur->refs == 0)
			m_free.push_back(m_cur);
		for (Slab* s : m_free)
		{
			::operator delete[](s->data, std::align_val_t(16));
			delete s;
		}
	}

	u8* SlabPool::Place(size_t bytes, Slab*& slab)
	{
		if (!m_cur || m_used + bytes > m_cur->size)
		{
			if (m_cur)
				Release(m_cur);
			if (bytes <= SLAB_BYTES && !m_free.empty())
			{
				m_cur = m_free.back();
				m_free.pop_back();
			}
			else
			{
				const size_t size = std::max(bytes, SLAB_BYTES);
				m_cur = new Slab{static_cast<u8*>(::operator new[](size, std::align_val_t(16))), size, 0};
				m_allocations++;
				m_held += size;
			}
			m_cur->refs = 1;
			m_used = 0;
		}
		u8* const p = m_cur->data + m_used;
		m_used += bytes;
		slab = m_cur;
		return p;
	}

	void SlabPool::Release(Slab* slab)
	{
		if (--slab->refs)
			return;
		if (slab->size == SLAB_BYTES && m_free.size() < MAX_FREE_SLABS)
		{
			m_free.push_back(slab);
			return;
		}
		m_held -= slab->size;
		::operator delete[](slab->data, std::align_val_t(16));
		delete slab;
	}

	Entry::Entry(std::shared_ptr<SlabPool> p)
		: pool(p ? std::move(p) : std::make_shared<SlabPool>())
	{
	}

	Entry::~Entry()
	{
		for (SlabPool::Slab* s : slabs)
			pool->Release(s);
	}

	Record Entry::At(u32 k) const
	{
		const u8* base = records[k];
		Record r;
		r.h = reinterpret_cast<const RecordHeader*>(base);
		r.words = reinterpret_cast<const u64*>(base + r.h->at_words);
		r.verts = base + r.h->at_verts;
		r.index = reinterpret_cast<const u16*>(base + r.h->at_index);
		r.src_blocks = reinterpret_cast<const u32*>(base + r.h->at_src_blocks);
		r.src_hashes = reinterpret_cast<const u64*>(base + r.h->at_src_hashes);
		r.pre_blocks = reinterpret_cast<const u32*>(base + r.h->at_pre_blocks);
		r.pre_hashes = reinterpret_cast<const u64*>(base + r.h->at_pre_hashes);
		r.out_blocks = reinterpret_cast<const u32*>(base + r.h->at_out_blocks);
		r.out_bytes = base + r.h->at_out_bytes;
		return r;
	}

	void Entry::Append(const Draw& d)
	{
		const Packet& p = d.packet;
		const u32 nwords = static_cast<u32>(p.words.size());
		const u32 nverts = static_cast<u32>(p.verts.size());
		const u32 nindex = static_cast<u32>(p.index.size());
		const u32 nsrc = static_cast<u32>(d.src.size());
		const u32 npre = static_cast<u32>(d.pre.size());
		const u32 nout = static_cast<u32>(d.out_blocks.size());
		const Layout l(nwords, nverts, nindex, nsrc, npre, nout);

		if (records.empty())
			first_signature = p.Signature();
		SlabPool::Slab* slab;
		u8* const base = pool->Place(l.end, slab);
		if (slabs.empty() || slabs.back() != slab)
		{
			slab->refs++;
			slabs.push_back(slab);
		}
		records.push_back(base);
		record_bytes += l.end;
		RecordHeader h;
		h.bytes = static_cast<u32>(l.end);
		h.live = p.live;
		h.at_words = static_cast<u32>(l.words);
		h.at_verts = static_cast<u32>(l.verts);
		h.at_index = static_cast<u32>(l.index);
		h.at_src_blocks = static_cast<u32>(l.src_blocks);
		h.at_src_hashes = static_cast<u32>(l.src_hashes);
		h.at_pre_blocks = static_cast<u32>(l.pre_blocks);
		h.at_pre_hashes = static_cast<u32>(l.pre_hashes);
		h.at_out_blocks = static_cast<u32>(l.out_blocks);
		h.at_out_bytes = static_cast<u32>(l.out_bytes);
		h.nverts = static_cast<u16>(nverts);
		h.nindex = static_cast<u16>(nindex);
		h.nsrc = static_cast<u16>(nsrc);
		h.npre = static_cast<u16>(npre);
		h.nout = static_cast<u16>(nout);
		h.flags = (d.frame_target_checked ? REC_FRAME_TARGET_CHECKED : 0) | (d.copied ? REC_COPIED : 0) |
		          (p.tex0_no_palette ? REC_TEX0_NO_PALETTE : 0);
		h.nwords = static_cast<u8>(nwords);
		h.palette_entries = d.palette_entries;
		h.palette_hash = d.palette_hash;
		h.bbox = d.bbox;
		h.vmask[0] = p.vmask[0];
		h.vmask[1] = p.vmask[1];
		h.left = d.left;
		std::memcpy(base, &h, sizeof(h));
		std::memcpy(base + l.words, p.words.data(), nwords * sizeof(u64));
		std::memcpy(base + l.verts, p.verts.data(), nverts * sizeof(GSVertex));
		std::memcpy(base + l.index, p.index.data(), nindex * sizeof(u16));
		for (u32 i = 0; i < nsrc; i++)
		{
			std::memcpy(base + l.src_blocks + i * sizeof(u32), &d.src[i].block, sizeof(u32));
			std::memcpy(base + l.src_hashes + i * sizeof(u64), &d.src[i].hash, sizeof(u64));
		}
		for (u32 i = 0; i < npre; i++)
		{
			std::memcpy(base + l.pre_blocks + i * sizeof(u32), &d.pre[i].block, sizeof(u32));
			std::memcpy(base + l.pre_hashes + i * sizeof(u64), &d.pre[i].hash, sizeof(u64));
		}
		std::memcpy(base + l.out_blocks, d.out_blocks.data(), nout * sizeof(u32));
		std::memcpy(base + l.out_bytes, d.out_bytes.data(), d.out_bytes.size());
	}

	void Entry::Recount()
	{
		bytes = sizeof(Entry) + record_bytes + records.size() * sizeof(const u8*) + slabs.size() * sizeof(SlabPool::Slab*);
	}

	Store::Store() = default;

	void Store::Clear()
	{
		for (auto& [sig, list] : m_index)
			for (const EntryPtr& e : list)
				e->stored = false;
		m_index.clear();
		std::memset(m_bloom, 0, sizeof(m_bloom));
		m_bytes = 0;
		m_entries = 0;
	}

	void Store::SetBudget(size_t bytes)
	{
		m_budget = bytes;
	}

	const std::vector<EntryPtr>* Store::Find(u64 sig) const
	{
		const auto it = m_index.find(sig);
		return it == m_index.end() ? nullptr : &it->second;
	}

	void Store::Insert(EntryPtr e)
	{
		if (!e->Count() || e->bytes > m_budget)
			return;
		const u64 sig = e->first_signature;
		e->last_used = m_frame;
		e->stored = true;
		m_bytes += e->bytes;
		m_entries++;
		const u32 bit = static_cast<u32>(sig >> 52);
		m_bloom[bit >> 6] |= u64(1) << (bit & 63);
		m_index[sig].push_back(std::move(e));

		if (m_bytes > m_budget)
		{
			// Least recently used first; among equals, the oldest install (earlier in its list).
			std::vector<Entry*> all;
			all.reserve(m_entries);
			for (auto& [s, list] : m_index)
				for (const EntryPtr& x : list)
					all.push_back(x.get());
			std::stable_sort(all.begin(), all.end(), [](const Entry* a, const Entry* b) { return a->last_used < b->last_used; });
			for (Entry* x : all)
			{
				if (m_bytes <= m_budget)
					break;
				Remove(x);
				m_budget_evictions++;
			}
			RebuildBloom();
		}
		m_peak_bytes = std::max(m_peak_bytes, m_bytes);
	}

	void Store::EndFrame()
	{
		m_frame++;
		bool removed = false;
		for (auto it = m_index.begin(); it != m_index.end();)
		{
			std::vector<EntryPtr>& list = it->second;
			for (size_t i = 0; i < list.size();)
			{
				Entry& e = *list[i];
				if (m_frame - e.last_used > HISTORY)
				{
					e.stored = false;
					m_bytes -= e.bytes;
					m_entries--;
					m_evictions++;
					list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
					removed = true;
				}
				else
				{
					i++;
				}
			}
			it = list.empty() ? m_index.erase(it) : std::next(it);
		}
		if (removed)
			RebuildBloom();
	}

	void Store::Remove(Entry* e)
	{
		const auto it = m_index.find(e->first_signature);
		if (it == m_index.end())
			return;
		std::vector<EntryPtr>& list = it->second;
		const auto pos = std::find_if(list.begin(), list.end(), [e](const EntryPtr& x) { return x.get() == e; });
		if (pos == list.end())
			return;
		e->stored = false;
		m_bytes -= e->bytes;
		m_entries--;
		list.erase(pos);
		if (list.empty())
			m_index.erase(it);
	}

	void Store::RebuildBloom()
	{
		std::memset(m_bloom, 0, sizeof(m_bloom));
		for (const auto& [sig, list] : m_index)
		{
			const u32 bit = static_cast<u32>(sig >> 52);
			m_bloom[bit >> 6] |= u64(1) << (bit & 63);
		}
	}
} // namespace GSBurstMemo
