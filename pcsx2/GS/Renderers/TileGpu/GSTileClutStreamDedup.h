// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSClut.h"
#include "GS/GSRegs.h"
#include "common/Pcsx2Types.h"

#include <algorithm>
#include <cstring>
#include <vector>

// The plan's palette stream, keyed on the identity of the expansion instead of appended blind.
//
// A paletted draw the CLUT gather cannot serve expands its CLUT, copies the expansion into the
// plan's palette stream, and hashes it for a content id. Nothing asked whether the plan already
// held that expansion. On the R&C UYA effects scene the answer was that 1,535 draws a frame
// appended 380,480 words carrying 85 distinct palettes: the stream is 94.5% duplicate.
//
// TWO KEYS, IN THAT ORDER, because they cost different amounts and catch different populations.
//
//  1. THE REGISTER KEY, which costs no hash. GSClut::Read32 writes the expanded buffer as a pure
//     function of the CLUT RAM and the registers that select and widen out of it, so two draws
//     agreeing on (RAM generation x those registers) read a byte-identical buffer:
//       - the RAM generation is GSClut::GetWriteGeneration(), which moves on every change to the
//         CLUT RAM -- a load from local memory, a device-loaded palette synced back through
//         SetEntries32, and a reset;
//       - the registers are CBP, CSA, CPSM and CSM off the draw's TEX0, TEXA's AEM/TA0/TA1 (which
//         the 16-bit expansion widens through), the entry count, and the CSM of the LOAD --
//         Read32's four-bit 32-bit path un-swizzles exactly when a CSM1 read follows a CSM0 load,
//         so the load's CSM is an input even though no read-side register carries it.
//
//  2. THE CONTENT KEY, which costs one hash of the words -- the hash the road already paid. ⚠️
//     This is the one that carries the population, and the reason is measured rather than assumed:
//     the register key alone removed 23.9% of rcuya-effects' stream, exactly its
//     read-generation-held population. That title RELOADS its CLUT from local memory 1,354 times a
//     frame, and every load moves the RAM generation -- so the RAM generation cannot hold still
//     across a palette alternation there, however few distinct palettes the alternation cycles
//     through. Repetition on that road is repetition of BYTES across different RAM states, and
//     only the bytes can see it. (Not the READ generation either: it is monotonic, counting
//     re-expansions rather than contents, so A,B,A,B gets a fresh generation on every switch.)
//
// A content hit is verified with a memcmp against the words already in the stream, so a 64-bit
// hash collision costs one compare and can never hand a draw the wrong palette.
//
// ⚠️ WHY THIS IS NOT std::unordered_map. It was, and the map cost more than the lever saved on the
// two titles with nothing much to dedup: flatout2 +0.70% and gt4 +0.85% instructions per drawn
// frame, against rcuya-effects' -0.57%. The lookups were not the problem -- the per-node
// allocation, the rehash as a plan filled, and clear() walking the nodes to free them were, and
// all three scale with draws rather than with duplicates, so a title that gains nothing still pays
// them. Open addressing over a flat array with a generation stamp has no allocation after the
// first plan, clears in one increment, and probes without chasing a pointer.
//
// LIFETIME IS THE PLAN, NOT THE FRAME. What a hit hands back is an OFFSET INTO the plan's palette
// stream, and that vector is cleared every time a plan is submitted -- at the frame end and also
// mid-frame, whenever something forces a sync. An entry that outlived its stream would name words
// that are no longer there.
class GSTileClutStreamDedup
{
public:
	/// No entry. Returned by both finds, and the one value an index can never take.
	static constexpr u32 kNone = ~0u;

	/// Everything GSClut::Read32's output depends on, packed. Compared for equality, never
	/// ordered: two keys are the same palette or they are not.
	struct Key
	{
		u64 texa = 0; ///< TEXA's TA1/AEM/TA0, the bits Expand16 reads
		u32 gen = 0;  ///< GSClut::GetWriteGeneration(), the CLUT RAM's identity
		u32 regs = 0; ///< CBP | CSA | CPSM | CSM | the load's CSM | the entry width

		bool operator==(const Key& o) const { return texa == o.texa && gen == o.gen && regs == o.regs; }
		bool operator!=(const Key& o) const { return !(*this == o); }

		u64 Hash() const
		{
			// One 64-bit mix over the three fields. Every probe verifies the key it lands on, so a
			// collision costs a compare and can never hand back the wrong palette.
			u64 h = texa;
			h = (h ^ (static_cast<u64>(gen) << 1)) * 0x9E3779B97F4A7C15ull;
			h = (h ^ (static_cast<u64>(regs) << 32)) * 0xBF58476D1CE4E5B9ull;
			return h ^ (h >> 31);
		}
	};

	/// Where an earlier draw of this plan put the same expansion, and the id it hashed for it.
	struct Stream
	{
		u32 offset = 0;     ///< word index of entry 0 in the plan's palette stream
		u32 entries = 0;    ///< 16 or 256, so a content id cannot match across widths
		u64 content_id = 0; ///< GSTilePaletteCache::ContentId over those words
	};

	/// TEXA's expansion-relevant bits: TA1, AEM, TA0. The same mask GSClut::ReadState::IsDirty
	/// tests, so the key moves exactly where Read32 would re-expand.
	static constexpr u64 kTexaMask = 0xFF000080FFull;

	static Key MakeKey(u32 clut_generation, u32 load_csm, const GIFRegTEX0& tex0, const GIFRegTEXA& texa,
		u32 entries)
	{
		Key k;
		k.texa = texa.U64 & kTexaMask;
		k.gen = clut_generation;
		k.regs = static_cast<u32>(tex0.CBP) | (static_cast<u32>(tex0.CSA) << 14) |
				 (static_cast<u32>(tex0.CPSM) << 19) | (static_cast<u32>(tex0.CSM) << 23) |
				 ((load_csm & 1u) << 24) | ((entries == 256 ? 1u : 0u) << 25);
		return k;
	}

	static Key MakeKey(GSClut& clut, const GIFRegTEX0& tex0, const GIFRegTEXA& texa, u32 entries)
	{
		return MakeKey(clut.GetWriteGeneration(), clut.GetCLUTCSM(), tex0, texa, entries);
	}

	GSTileClutStreamDedup() { Reserve(kInitialSlots); }

	/// Level 1: the entry this plan holds for that register state, or kNone. Costs no hash of the
	/// palette words -- which is the entire reason it is asked first.
	u32 FindByRegisters(const Key& k) const
	{
		if (m_reg_slots.empty())
			return kNone;
		const u32 mask = static_cast<u32>(m_reg_slots.size()) - 1;
		for (u32 i = static_cast<u32>(k.Hash()) & mask;; i = (i + 1) & mask)
		{
			const RegSlot& s = m_reg_slots[i];
			if (s.stamp != m_stamp)
				return kNone;
			if (s.key == k)
				return s.entry;
		}
	}

	/// Level 2: the entry this plan holds for those WORDS, or kNone. `stream` is the plan's palette
	/// stream (an entry's offset indexes it) and `words` the freshly expanded CLUT; the two are
	/// compared, so the answer is the words' and not the hash's.
	u32 FindByContent(u64 content_id, const u32* stream, const u32* words, u32 entries) const
	{
		if (m_con_slots.empty())
			return kNone;
		const u32 mask = static_cast<u32>(m_con_slots.size()) - 1;
		for (u32 i = static_cast<u32>(MixId(content_id)) & mask;; i = (i + 1) & mask)
		{
			const ConSlot& s = m_con_slots[i];
			if (s.stamp != m_stamp)
				return kNone;
			if (s.id == content_id)
			{
				const Stream& e = m_entries[s.entry];
				if (e.entries == entries && std::memcmp(stream + e.offset, words, entries * sizeof(u32)) == 0)
					return s.entry;
				return kNone;
			}
		}
	}

	const Stream& At(u32 index) const { return m_entries[index]; }

	/// A palette the plan's stream now holds, under both keys. Returns its index.
	u32 Insert(const Key& k, u64 content_id, u32 offset, u32 entries)
	{
		const u32 index = static_cast<u32>(m_entries.size());
		m_entries.push_back(Stream{offset, entries, content_id});
		GrowIfFull();
		PlaceRegister(k, index);
		PlaceContent(content_id, index);
		return index;
	}

	/// ...and the register key alone, pointed at an entry the content key already found. This is
	/// the draw whose words were in the stream under a different RAM generation; the alias is what
	/// spares the NEXT draw at these registers its hash.
	void AliasRegisters(const Key& k, u32 index)
	{
		GrowIfFull();
		PlaceRegister(k, index);
	}

	/// Called wherever the plan's palette stream is cleared, and nowhere else -- the entries name
	/// offsets into it. One increment: the slots stay allocated and are simply out of date.
	void Clear()
	{
		m_entries.clear();
		m_reg_live = 0;
		m_con_live = 0;
		if (++m_stamp == 0) [[unlikely]]
		{
			// Two billion plans later. Wipe rather than reason about it.
			std::memset(m_reg_slots.data(), 0, m_reg_slots.size() * sizeof(RegSlot));
			std::memset(m_con_slots.data(), 0, m_con_slots.size() * sizeof(ConSlot));
			m_stamp = 1;
		}
	}

	u32 Size() const { return static_cast<u32>(m_entries.size()); }

private:
	// Enough for every corpus title's per-plan palette count at load factor 1/2 (gt4 is the
	// heaviest at ~300 entries a frame across several plans), so a warm renderer never grows.
	static constexpr u32 kInitialSlots = 1024;

	struct RegSlot
	{
		u32 stamp = 0;
		u32 entry = 0;
		Key key;
	};
	struct ConSlot
	{
		u32 stamp = 0;
		u32 entry = 0;
		u64 id = 0;
	};

	static u64 MixId(u64 id)
	{
		// The content id is already an FNV hash, but its low bits are the ones a mask keeps and FNV
		// mixes those least. One multiply-shift so the table sees the whole word.
		u64 h = id * 0xBF58476D1CE4E5B9ull;
		return h ^ (h >> 29);
	}

	void PlaceRegister(const Key& k, u32 index)
	{
		const u32 mask = static_cast<u32>(m_reg_slots.size()) - 1;
		for (u32 i = static_cast<u32>(k.Hash()) & mask;; i = (i + 1) & mask)
		{
			RegSlot& s = m_reg_slots[i];
			if (s.stamp != m_stamp)
			{
				s.stamp = m_stamp;
				s.entry = index;
				s.key = k;
				m_reg_live++;
				return;
			}
			if (s.key == k)
			{
				s.entry = index; // a re-insert under a key already present: newest wins
				return;
			}
		}
	}

	void PlaceContent(u64 id, u32 index)
	{
		const u32 mask = static_cast<u32>(m_con_slots.size()) - 1;
		for (u32 i = static_cast<u32>(MixId(id)) & mask;; i = (i + 1) & mask)
		{
			ConSlot& s = m_con_slots[i];
			if (s.stamp != m_stamp)
			{
				s.stamp = m_stamp;
				s.entry = index;
				s.id = id;
				m_con_live++;
				return;
			}
			if (s.id == id)
			{
				s.entry = index;
				return;
			}
		}
	}

	/// Half full is where linear probing stops being cheap. Both tables are sized together because
	/// the register table is the larger of the two by exactly the alias count, and one bound is
	/// simpler to reason about than two.
	void GrowIfFull()
	{
		const size_t want = static_cast<size_t>(std::max(m_reg_live, m_con_live) + 2) * 2;
		if (want <= m_reg_slots.size()) [[likely]]
			return;
		size_t slots = m_reg_slots.empty() ? kInitialSlots : m_reg_slots.size();
		while (slots < want)
			slots *= 2;
		Rehash(slots);
	}

	void Reserve(size_t slots)
	{
		m_reg_slots.assign(slots, RegSlot{});
		m_con_slots.assign(slots, ConSlot{});
		m_reg_live = 0;
		m_con_live = 0;
	}

	void Rehash(size_t slots)
	{
		std::vector<RegSlot> old_reg;
		std::vector<ConSlot> old_con;
		old_reg.swap(m_reg_slots);
		old_con.swap(m_con_slots);
		Reserve(slots);
		for (const RegSlot& s : old_reg)
		{
			if (s.stamp == m_stamp)
				PlaceRegister(s.key, s.entry);
		}
		for (const ConSlot& s : old_con)
		{
			if (s.stamp == m_stamp)
				PlaceContent(s.id, s.entry);
		}
	}

	std::vector<Stream> m_entries;
	std::vector<RegSlot> m_reg_slots;
	std::vector<ConSlot> m_con_slots;
	u32 m_reg_live = 0;
	u32 m_con_live = 0;
	/// Which plan the slots belong to. A slot whose stamp is not this one is empty, which is what
	/// makes Clear() an increment rather than a walk.
	u32 m_stamp = 1;
};
