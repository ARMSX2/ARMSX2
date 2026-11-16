// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A SIF1 transfer that the EE stops part way through and starts again, as the BIOS's sceSifSetDma does when it queues a
// packet while the IOP has not yet taken all of the one before. Used by the pump test (Off, with the calls made by hand in
// the order the IOP thread makes them) and by the Lockstep test (the same order, through the real register writes).
//
// The tags sit in a ring of 16-byte slots, as the BIOS keeps them: a packet is a REF tag for its IOP header and a REF or
// REFE tag for its payload. Slot 4, after the last packet's REFE, holds a tag from an earlier lap of the ring. The channel
// must never read it.

#pragma once

#include "Common.h"
#include "Dmac.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "Memory.h"
#include "Sif.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>

namespace sif1restart
{
	constexpr u32 kRing = 0x211c0;
	constexpr u32 kHeaders = 0x213c0; // one quadword of IOP header per slot
	constexpr u32 kPayloadA = 0x100000, kPayloadB = 0x110000, kStale = 0x120000;
	constexpr u32 kQwcA = 40; // more than the FIFO holds, so a pump with the IOP not armed stops part way through it
	constexpr u32 kQwcB = 3;
	constexpr u32 kQwcStale = 8;
	constexpr u32 kIopDstA = 0x30000, kIopDstB = 0x38000;
	constexpr u32 kMarkA = 0xA0000000, kMarkB = 0xB0000000, kMarkStale = 0x5EA1E000;
	constexpr u32 kIopEnd = 0x40000000;

	inline u32 Slot(u32 i) { return kRing + 16 * i; }

	inline void Put(u32 addr, u32 w0, u32 w1)
	{
		const u32 w[4] = {w0, w1, 0, 0};
		std::memcpy(eeMem->Main + addr, w, sizeof(w));
	}

	inline void Fill(u32 addr, u32 qwc, u32 mark)
	{
		for (u32 i = 0; i < qwc * 4; i++)
		{
			const u32 v = mark | i;
			std::memcpy(eeMem->Main + addr + 4 * i, &v, 4);
		}
	}

	inline u32 IopWord(u32 addr)
	{
		u32 v;
		std::memcpy(&v, iopMem->Main + addr, 4);
		return v;
	}

	// A packet in slots `slot` and `slot + 1`.
	inline void PutPacket(u32 slot, u32 payload, u32 qwc, u32 iop_dst, u32 mark, bool iop_end)
	{
		const u32 header = kHeaders + 16 * slot;
		Put(header, iop_dst | (iop_end ? kIopEnd : 0), qwc * 4);
		Put(Slot(slot), 1 | (TAG_REF << 28), header);
		Put(Slot(slot + 1), qwc | (TAG_REFE << 28), payload);
		Fill(payload, qwc, mark);
	}

	// Packet A in slots 0 and 1, the old tag in slot 4 (a REFE of a buffer with no IOP header in front), and the channel
	// pointed at slot 0. The caller starts it.
	inline void BuildPacketA()
	{
		PutPacket(0, kPayloadA, kQwcA, kIopDstA, kMarkA, false);
		Put(Slot(4), kQwcStale | (TAG_REFE << 28), kStale);
		Fill(kStale, kQwcStale, kMarkStale);
		sif1ch.tadr = Slot(0);
		sif1ch.qwc = 0;
		hw_dma10.madr = 0;
		hw_dma10.bcr = 0;
		hw_dma10.chcr = 0x01000200;
	}

	// What sceSifSetDma does after the stop when the slot that was running is the only one in use: that slot becomes a REF
	// of what is left of it, the channel is pointed back at it, and packet B goes into the next two slots. The caller
	// starts the channel again.
	inline void FixUpAndQueuePacketB()
	{
		Put(Slot(1), sif1ch.qwc | (TAG_REF << 28), sif1ch.madr);
		sif1ch.qwc = 0;
		sif1ch.tadr = Slot(1);
		PutPacket(2, kPayloadB, kQwcB, kIopDstB, kMarkB, true);
	}

	// After the EE's own pump of the restart: both packets arrived whole, the channel read no tag past packet B's REFE, and
	// nothing of the old buffer is in the FIFO or in IOP RAM.
	inline void ExpectBothPacketsAndNothingElse()
	{
		for (u32 i = 0; i < kQwcA * 4; i++)
			ASSERT_EQ(kMarkA | i, IopWord(kIopDstA + 4 * i)) << "packet A, word " << i;
		for (u32 i = 0; i < kQwcB * 4; i++)
			ASSERT_EQ(kMarkB | i, IopWord(kIopDstB + 4 * i)) << "packet B, word " << i;
		EXPECT_EQ(Slot(4), sif1ch.tadr) << "the channel read the tag after the chain's end";
		EXPECT_EQ(0, sif1.fifo.size) << "words in the FIFO after both packets went through";
		u32 stale_words = 0;
		for (u32 a = 0; a < Ps2MemSize::ExposedIopRam; a += 4)
			stale_words += (IopWord(a) & 0xFFFFF000u) == kMarkStale;
		EXPECT_EQ(0u, stale_words) << "words of the old buffer in IOP RAM";
		EXPECT_FALSE(sif1.ee.busy);
		EXPECT_NE(0u, cpuRegs.interrupt & (1u << DMAC_SIF1)) << "the EE's completion is scheduled";
	}

	// EE and IOP RAM as heap blocks for the pumps, and the SIF structures, all put back when the test is done.
	class MemoryGuard
	{
	public:
		MemoryGuard()
			: m_ee(eeMem)
			, m_iop(iopMem)
			, m_s0(sif0)
			, m_s1(sif1)
		{
			// Lazily committed: the test pays for the pages it writes.
			eeMem = m_ee_block = static_cast<EEVM_MemoryAllocMess*>(std::calloc(1, sizeof(EEVM_MemoryAllocMess)));
			iopMem = m_iop_block = static_cast<IopVM_MemoryAllocMess*>(std::calloc(1, sizeof(IopVM_MemoryAllocMess)));
			sifReset();
		}
		~MemoryGuard()
		{
			eeMem = m_ee;
			iopMem = m_iop;
			sif0 = m_s0;
			sif1 = m_s1;
			std::free(m_ee_block);
			std::free(m_iop_block);
		}
		bool Ok() const { return m_ee_block && m_iop_block; }

	private:
		EEVM_MemoryAllocMess* m_ee;
		IopVM_MemoryAllocMess* m_iop;
		EEVM_MemoryAllocMess* m_ee_block;
		IopVM_MemoryAllocMess* m_iop_block;
		_sif m_s0, m_s1;
	};
} // namespace sif1restart
