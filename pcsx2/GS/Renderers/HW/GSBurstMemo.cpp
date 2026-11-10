// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/HW/GSBurstMemo.h"
#include "GS/GSLocalMemory.h"

#include <algorithm>
#include <cstring>

namespace GSBurstMemo
{
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
		Gather(c, e, flags, words);

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
		for (u32 i = 0; i < WORDS; i++)
			words[i] &= mask[i];

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
	}

	bool Packet::Matches(const GSDrawingContext& c, const GSDrawingEnvironment& e, u64 flags, const GSVertex* v, u32 nverts,
		const u16* idx, u32 nindex) const
	{
		if (nverts != verts.size() || nindex != index.size())
			return false;

		u64 live[WORDS];
		Gather(c, e, flags, live);
		u64 diff = 0;
		for (u32 i = 0; i < WORDS; i++)
			diff |= (live[i] & mask[i]) ^ words[i];
		if (diff)
			return false;

		GSVector4i vdiff = GSVector4i::zero();
		for (u32 i = 0; i < nverts; i++)
		{
			vdiff |= (GSVector4i(v[i].m[0]) & vmask[0]) ^ GSVector4i(verts[i].m[0]);
			vdiff |= (GSVector4i(v[i].m[1]) & vmask[1]) ^ GSVector4i(verts[i].m[1]);
		}
		if (!vdiff.eq(GSVector4i::zero()))
			return false;

		return nindex == 0 || std::memcmp(idx, index.data(), nindex * sizeof(u16)) == 0;
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

	u64 Packet::Signature() const
	{
		const u32 xy0 = verts.empty() ? 0 : (static_cast<u32>(verts[0].XYZ.X) | (static_cast<u32>(verts[0].XYZ.Y) << 16));
		return SignatureOf(words[W_FRAME], words[W_XYOFFSET], words[W_PRIM], words[W_TEX0], xy0, static_cast<u32>(index.size()));
	}

	size_t Draw::Bytes() const
	{
		return sizeof(Draw) + packet.Bytes() + (src.size() + pre.size()) * sizeof(BlockKey) + out_blocks.size() * sizeof(u32) +
		       out_bytes.size();
	}

	void Entry::Recount()
	{
		bytes = sizeof(Entry);
		for (const Draw& d : draws)
			bytes += d.Bytes();
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
		if (e->draws.empty() || e->bytes > m_budget)
			return;
		const u64 sig = e->draws[0].packet.Signature();
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
			std::vector<EntryPtr> all;
			all.reserve(m_entries);
			for (auto& [s, list] : m_index)
				for (const EntryPtr& x : list)
					all.push_back(x);
			std::stable_sort(all.begin(), all.end(), [](const EntryPtr& a, const EntryPtr& b) { return a->last_used < b->last_used; });
			for (const EntryPtr& x : all)
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

	void Store::Remove(const EntryPtr& e)
	{
		const auto it = m_index.find(e->draws[0].packet.Signature());
		if (it == m_index.end())
			return;
		std::vector<EntryPtr>& list = it->second;
		const auto pos = std::find(list.begin(), list.end(), e);
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
