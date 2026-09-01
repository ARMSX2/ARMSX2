// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSClut.h"
#include "GS/GSRegs.h"
#include "common/Pcsx2Types.h"

#include <cstddef>
#include <cstring>
#include <unordered_map>

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
//  2. THE CONTENT KEY, which costs one hash of the words. ⚠️ This is the one that carries the
//     population, and the reason is measured rather than assumed: the register key alone removed
//     23.9% of rcuya-effects' stream, exactly its read-generation-held population. That title
//     RELOADS its CLUT from local memory 1,354 times a frame, and every load moves the RAM
//     generation -- so the RAM generation cannot hold still across a palette alternation there,
//     however few distinct palettes the alternation cycles through. Repetition on that road is
//     repetition of BYTES across different RAM states, and only the bytes can see it.
//     (Not the READ generation either: it is monotonic, counting re-expansions rather than
//     contents, so A,B,A,B gets a fresh generation on every switch.)
//
// A content hit is verified with a memcmp against the words already in the stream, so a 64-bit
// hash collision costs one compare and can never hand a draw the wrong palette.
//
// LIFETIME IS THE PLAN, NOT THE FRAME. What a hit hands back is an OFFSET INTO the plan's palette
// stream, and that vector is cleared every time a plan is submitted -- at the frame end and also
// mid-frame, whenever something forces a sync. An entry that outlived its stream would name words
// that are no longer there.
class GSTileClutStreamDedup
{
public:
	/// Everything GSClut::Read32's output depends on, packed. Compared for equality, never
	/// ordered: two keys are the same palette or they are not.
	struct Key
	{
		u64 texa = 0; ///< TEXA's TA1/AEM/TA0, the bits Expand16 reads
		u32 gen = 0;  ///< GSClut::GetWriteGeneration(), the CLUT RAM's identity
		u32 regs = 0; ///< CBP | CSA | CPSM | CSM | the load's CSM | the entry width

		bool operator==(const Key& o) const { return texa == o.texa && gen == o.gen && regs == o.regs; }
		bool operator!=(const Key& o) const { return !(*this == o); }
	};

	struct KeyHash
	{
		size_t operator()(const Key& k) const
		{
			// One 64-bit mix over the three fields. The map verifies the key on a bucket hit, so a
			// collision costs a compare and can never hand back the wrong palette.
			u64 h = k.texa;
			h = (h ^ (static_cast<u64>(k.gen) << 1)) * 0x9E3779B97F4A7C15ull;
			h = (h ^ (static_cast<u64>(k.regs) << 32)) * 0xBF58476D1CE4E5B9ull;
			return static_cast<size_t>(h ^ (h >> 31));
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

	/// Level 1: the expansion this plan holds for that register state, or null. Costs no hash.
	const Stream* FindByRegisters(const Key& k) const
	{
		const auto it = m_by_key.find(k);
		return (it == m_by_key.end()) ? nullptr : &it->second;
	}

	/// Level 2: the expansion this plan holds for those WORDS, or null. `stream` is the plan's
	/// palette stream (the entry's offset indexes it) and `words` the freshly expanded CLUT; the
	/// two are compared, so the answer is the words' and not the hash's.
	const Stream* FindByContent(u64 content_id, const u32* stream, const u32* words, u32 entries) const
	{
		const auto it = m_by_content.find(content_id);
		if (it == m_by_content.end())
			return nullptr;
		const Stream& s = it->second;
		if (s.entries != entries || std::memcmp(stream + s.offset, words, entries * sizeof(u32)) != 0)
			return nullptr;
		return &s;
	}

	/// A palette the plan's stream now holds, under both keys.
	void Insert(const Key& k, u64 content_id, u32 offset, u32 entries)
	{
		const Stream s{offset, entries, content_id};
		m_by_key[k] = s;
		m_by_content[content_id] = s;
	}

	/// ...and the register key alone, for a draw whose words were already in the stream under a
	/// different RAM generation. Costs the next draw at these registers its hash.
	void InsertRegisters(const Key& k, const Stream& s) { m_by_key[k] = s; }

	/// Called wherever the plan's palette stream is cleared, and nowhere else -- the entries name
	/// offsets into it.
	void Clear()
	{
		m_by_key.clear();
		m_by_content.clear();
	}

	size_t RegisterEntries() const { return m_by_key.size(); }
	size_t ContentEntries() const { return m_by_content.size(); }

private:
	std::unordered_map<Key, Stream, KeyHash> m_by_key;
	std::unordered_map<u64, Stream> m_by_content;
};
