// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The SBUS registers: what each side of the SIF link can do to them.
//
// The SBUS registers are six words at 0x1000F200-F260 in the EE's hardware page. The EE reads and
// writes them through its hardware handlers (hwRead32/hwWrite32, page 0x0f), the IOP through its
// 0x1D000000 window (iopMemRead16/32, iopMemWrite16/32), and both land on the same words:
//
//   F200  MSCOM   the EE writes, the IOP reads (an IOP write is dropped)
//   F210  SMCOM   the IOP writes, the EE reads
//   F220  MSFLG   the EE sets bits, the IOP clears bits
//   F230  SMFLG   the IOP sets bits, the EE clears bits
//   F240          the control register: the EE sets or clears bit 8; the IOP toggles bits 4-7 and, on a
//                 write with bit 5 or 7, resets 12-15 and sets bit 13; the SIF pumps clear their own bits
//   F260          the EE writes it, an IOP write sets it to 0
//
// The first half of this file records what those accesses do today, value by value, so that making
// them atomic can be checked not to move any of it. The second half runs the EE side and the IOP side
// of one register on two threads at once. A plain read-modify-write loses an update when the other
// side's lands between its load and its store; an atomic one cannot, so the counts come out exact.
//
// Memory. The IOP window reads and writes through psxMemRLUT/psxMemWLUT and iopMem->Sif, and the
// handlers read psxRegs; this file points them at a heap block and a small table instead of bringing up
// SysMemory. Everything it touches is saved before the first test and put back after the last.

#include "Common.h"
#include "Hw.h"
#include "IopHw.h"
#include "IopMem.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "R3000A.h"
#include "ps2/HwInternal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
	constexpr u32 kIopSbus = 0x1D000000; // the IOP's view of F200

	struct Saved
	{
		uptr* wlut = nullptr;
		const uptr* rlut = nullptr;
		IopVM_MemoryAllocMess* iop_mem = nullptr;
		u8 ee_hw[Ps2MemSize::Hardware];
		u32 status = 0;
	};

	Saved* s_saved = nullptr;
	uptr* s_lut = nullptr;
	IopVM_MemoryAllocMess* s_block = nullptr;

	bool BringUpMachine()
	{
		s_saved = new Saved;
		s_saved->wlut = psxMemWLUT;
		s_saved->rlut = psxMemRLUT;
		s_saved->iop_mem = iopMem;
		std::memcpy(s_saved->ee_hw, eeHw, sizeof(eeHw));
		s_saved->status = psxRegs.CP0.n.Status;

		s_lut = static_cast<uptr*>(std::calloc(0x2000 * 2, sizeof(uptr)));
		s_block = static_cast<IopVM_MemoryAllocMess*>(std::calloc(1, sizeof(IopVM_MemoryAllocMess)));
		if (!s_lut || !s_block)
			return false;
		psxMemWLUT = s_lut;
		psxMemRLUT = s_lut + 0x2000;
		iopMem = s_block;
		// The only mapping the SBUS window needs: reads of page 0x1d00 find a table, so the handler goes on to
		// pick the register. Writes find none and take the register branch.
		s_lut[0x2000 + 0x1d00] = reinterpret_cast<uptr>(iopMem->Sif);
		psxRegs.CP0.n.Status = 0;
		return true;
	}

	void TearDownMachine()
	{
		if (!s_saved)
			return;
		psxMemWLUT = s_saved->wlut;
		psxMemRLUT = s_saved->rlut;
		iopMem = s_saved->iop_mem;
		std::memcpy(eeHw, s_saved->ee_hw, sizeof(eeHw));
		psxRegs.CP0.n.Status = s_saved->status;
		std::free(s_lut);
		std::free(s_block);
		s_lut = nullptr;
		s_block = nullptr;
		delete s_saved;
		s_saved = nullptr;
	}

	// The EE's side.
	u32 EeRead(u32 reg) { return hwRead32<0x0f>(reg); }
	void EeWrite(u32 reg, u32 value) { hwWrite32<0x0f>(reg, value); }

	// The IOP's side, by the register's EE address.
	u32 IopRead32(u32 reg) { return iopMemRead32(kIopSbus + (reg & 0xff)); }
	u16 IopRead16(u32 reg) { return iopMemRead16(kIopSbus + (reg & 0xff)); }
	void IopWrite32(u32 reg, u32 value) { iopMemWrite32(kIopSbus + (reg & 0xff), value); }
	void IopWrite16(u32 reg, u16 value) { iopMemWrite16(kIopSbus + (reg & 0xff), value); }

	u32& Word(u32 reg) { return psHu32(reg); }

	class Sbus : public ::testing::Test
	{
	protected:
		static void SetUpTestSuite() { s_ready = BringUpMachine(); }
		static void TearDownTestSuite()
		{
			TearDownMachine();
			s_ready = false;
		}
		void SetUp() override
		{
			ASSERT_TRUE(s_ready);
			for (const u32 reg : {SBUS_F200, SBUS_F210, SBUS_F220, SBUS_F230, SBUS_F240, SBUS_F250, SBUS_F260})
				Word(reg) = 0;
			std::memset(iopMem->Sif, 0, sizeof(iopMem->Sif));
		}
		static bool s_ready;
	};
	bool Sbus::s_ready = false;
} // namespace

// ---- What each register does, from either side ------------------------------------------------

TEST_F(Sbus, F200TheEeWritesAndTheIopReads)
{
	EeWrite(SBUS_F200, 0x12345678);
	EXPECT_EQ(0x12345678u, Word(SBUS_F200));
	EXPECT_EQ(0x12345678u, EeRead(SBUS_F200));
	EXPECT_EQ(0x12345678u, IopRead32(SBUS_F200));
	EXPECT_EQ(0x5678u, IopRead16(SBUS_F200));

	// An IOP write is dropped.
	IopWrite32(SBUS_F200, 0xDEADBEEF);
	IopWrite16(SBUS_F200, 0xBEEF);
	EXPECT_EQ(0x12345678u, Word(SBUS_F200));

	// The EE's second write replaces the first; it does not merge.
	EeWrite(SBUS_F200, 0x000000FF);
	EXPECT_EQ(0x000000FFu, IopRead32(SBUS_F200));
}

TEST_F(Sbus, F210TheIopWritesAndTheEeReads)
{
	IopWrite32(SBUS_F210, 0xAABBCCDD);
	EXPECT_EQ(0xAABBCCDDu, Word(SBUS_F210));
	EXPECT_EQ(0xAABBCCDDu, EeRead(SBUS_F210));
	EXPECT_EQ(0xAABBCCDDu, IopRead32(SBUS_F210));
	EXPECT_EQ(0xCCDDu, IopRead16(SBUS_F210));

	// A halfword write replaces the low half and leaves the high half.
	IopWrite16(SBUS_F210, 0x1234);
	EXPECT_EQ(0xAABB1234u, Word(SBUS_F210));
	EXPECT_EQ(0xAABB1234u, EeRead(SBUS_F210));

	// An EE write is stored as it is.
	EeWrite(SBUS_F210, 0x01020304);
	EXPECT_EQ(0x01020304u, IopRead32(SBUS_F210));
}

TEST_F(Sbus, F220TheEeSetsBitsAndTheIopClearsThem)
{
	EeWrite(SBUS_F220, 0x00000005);
	EeWrite(SBUS_F220, 0x00000100);
	EXPECT_EQ(0x00000105u, Word(SBUS_F220));
	EXPECT_EQ(0x00000105u, EeRead(SBUS_F220));
	EXPECT_EQ(0x00000105u, IopRead32(SBUS_F220));

	IopWrite32(SBUS_F220, 0x00000004); // clears bit 2
	EXPECT_EQ(0x00000101u, Word(SBUS_F220));
	IopWrite32(SBUS_F220, 0xFFFFFFFF);
	EXPECT_EQ(0u, Word(SBUS_F220));

	// Bits the EE sets again after a clear are set.
	EeWrite(SBUS_F220, 0x80000000);
	EXPECT_EQ(0x80000000u, IopRead32(SBUS_F220));
}

TEST_F(Sbus, F230TheIopSetsBitsAndTheEeClearsThem)
{
	IopWrite32(SBUS_F230, 0x00F0);
	IopWrite32(SBUS_F230, 0x0001);
	EXPECT_EQ(0x00F1u, Word(SBUS_F230));
	EXPECT_EQ(0x00F1u, EeRead(SBUS_F230));
	EXPECT_EQ(0x00F1u, IopRead32(SBUS_F230));

	EeWrite(SBUS_F230, 0x0030); // clears bits 4 and 5
	EXPECT_EQ(0x00C1u, Word(SBUS_F230));
	EeWrite(SBUS_F230, 0xFFFFFFFF);
	EXPECT_EQ(0u, Word(SBUS_F230));
}

TEST_F(Sbus, F240ReadsHaveTheirConstantBitsOrIn)
{
	Word(SBUS_F240) = 0x00006060;
	EXPECT_EQ(0x00006060u | 0xF0000102u, EeRead(SBUS_F240));
	EXPECT_EQ(0x00006060u | 0xF0000002u, IopRead32(SBUS_F240));
	EXPECT_EQ((0x6060u | 0x0002u), static_cast<u32>(IopRead16(SBUS_F240)));
	EXPECT_EQ(0x00006060u, Word(SBUS_F240)); // reading changes nothing
}

TEST_F(Sbus, F240TheEeSetsAndClearsBit8AndNothingElse)
{
	Word(SBUS_F240) = 0x00006061;
	EeWrite(SBUS_F240, 0x100);
	EXPECT_EQ(0x00006161u, Word(SBUS_F240));
	EeWrite(SBUS_F240, 0x100 | 0x20 | 0x4000); // other bits of the value are not stored
	EXPECT_EQ(0x00006161u, Word(SBUS_F240));
	EeWrite(SBUS_F240, 0);
	EXPECT_EQ(0x00006061u, Word(SBUS_F240));
	EeWrite(SBUS_F240, 0xFF); // bit 8 clear in the value, so it is cleared
	EXPECT_EQ(0x00006061u, Word(SBUS_F240));
}

// The IOP's write to the control register, for the 32-bit and the 16-bit access alike.
struct F240Case
{
	u32 before;
	u32 value;
	u32 after;
	const char* why;
};

static const F240Case kF240Cases[] = {
	{0x00000000, 0x10, 0x00000010, "a bit that was clear is set"},
	{0x00000010, 0x10, 0x00000000, "a bit that was set is cleared"},
	{0x00000040, 0x50, 0x00000000, "any of the bits set clears all of them"},
	{0x00000000, 0x50, 0x00000050, "none of the bits set sets all of them"},
	{0x00000000, 0x80, 0x00002080, "bit 7 resets 12-15 and sets 13 first"},
	{0x0000F0F0, 0x20, 0x000020D0, "bit 5 resets 12-15, sets 13, then clears bit 5 that was set"},
	{0x0000F000, 0x00, 0x0000F000, "a value without bits 4-7 changes nothing"},
	{0x80000040, 0x10, 0x80000050, "the high half is left alone"},
	{0x8000F0F0, 0x80, 0x80002070, "the high half is left alone by a reset; bit 7 was set, so it is cleared after"},
	{0x00000000, 0xFFA0, 0x000020A0, "only the low byte of the value is used"},
	{0x00000000, 0xFF0F, 0x00000000, "a value with none of bits 4-7 changes nothing, whatever its other bits"},
};

TEST_F(Sbus, F240TheIopToggleBits4To7AndResetOn5Or7By32BitWrite)
{
	for (const F240Case& c : kF240Cases)
	{
		Word(SBUS_F240) = c.before;
		IopWrite32(SBUS_F240, c.value);
		EXPECT_EQ(c.after, Word(SBUS_F240)) << c.why << ": before " << std::hex << c.before << " write " << c.value;
	}
}

TEST_F(Sbus, F240TheIopToggleBits4To7AndResetOn5Or7By16BitWrite)
{
	for (const F240Case& c : kF240Cases)
	{
		Word(SBUS_F240) = c.before;
		IopWrite16(SBUS_F240, static_cast<u16>(c.value));
		EXPECT_EQ(c.after, Word(SBUS_F240)) << c.why << ": before " << std::hex << c.before << " write " << c.value;
	}
}

TEST_F(Sbus, F260TheEeWritesAndAnIopWriteClearsIt)
{
	EeWrite(SBUS_F260, 0x1D000060);
	EXPECT_EQ(0x1D000060u, Word(SBUS_F260));
	EXPECT_EQ(0x1D000060u, EeRead(SBUS_F260));
	EXPECT_EQ(0u, IopRead32(SBUS_F260)); // the IOP reads it as 0 whatever it holds
	EXPECT_EQ(0u, IopRead16(SBUS_F260));

	IopWrite32(SBUS_F260, 0x55);
	EXPECT_EQ(0u, Word(SBUS_F260));
	EeWrite(SBUS_F260, 0x1D000060);
	IopWrite16(SBUS_F260, 0x55);
	EXPECT_EQ(0u, Word(SBUS_F260));
}

TEST_F(Sbus, TheRegistersDoNotTouchEachOther)
{
	const u32 start[] = {0x11111111, 0x22222222, 0x33333333, 0x44444444, 0x55555555, 0x66666666, 0x77777777};
	const u32 regs[] = {SBUS_F200, SBUS_F210, SBUS_F220, SBUS_F230, SBUS_F240, SBUS_F250, SBUS_F260};
	for (int i = 0; i < 7; i++)
		Word(regs[i]) = start[i];

	EeWrite(SBUS_F220, 0x00000F00);
	for (int i = 0; i < 7; i++)
		EXPECT_EQ(regs[i] == SBUS_F220 ? 0x33333F33u : start[i], Word(regs[i])) << std::hex << regs[i];
}

// ---- Both sides at once ------------------------------------------------------------------------
//
// The EE side sets bits of a register one after another; the IOP side finds each set bit and clears it,
// and counts what it cleared. If no update is lost the IOP clears exactly as many bits as the EE set,
// because the EE only reuses a bit after the IOP cleared it. An update lost on either side shows as a
// count that is not the rounds: a set that a clear overwrote is never seen (fewer), a clear that a set
// overwrote is seen again (more).

namespace
{
	struct Handshake
	{
		u32 set_reg;
		u32 clear_reg;
		u32 rounds;
		std::atomic<bool> setter_done{false};
		std::atomic<bool> clearer_done{false};
		u64 cleared = 0;
	};

	// F220: the EE sets, the IOP clears.
	void RunEeSetsIopClears(Handshake& h)
	{
		std::thread ee([&h] {
			for (u32 r = 0; r < h.rounds; r++)
			{
				const u32 bit = 1u << (r & 15);
				while ((EeRead(h.set_reg) & bit) && !h.clearer_done.load(std::memory_order_relaxed))
				{
				}
				EeWrite(h.set_reg, bit);
			}
			h.setter_done.store(true, std::memory_order_release);
		});
		std::thread iop([&h] {
			u64 cleared = 0;
			for (;;)
			{
				const bool last_pass = h.setter_done.load(std::memory_order_acquire);
				const u32 seen = IopRead32(h.clear_reg) & 0xFFFF;
				for (u32 b = 0; b < 16; b++)
				{
					if (seen & (1u << b))
					{
						IopWrite32(h.clear_reg, 1u << b);
						cleared++;
					}
				}
				if (last_pass)
					break;
			}
			h.cleared = cleared;
			h.clearer_done.store(true, std::memory_order_release);
		});
		ee.join();
		iop.join();
	}

	// F230: the IOP sets, the EE clears.
	void RunIopSetsEeClears(Handshake& h)
	{
		std::thread iop([&h] {
			for (u32 r = 0; r < h.rounds; r++)
			{
				const u32 bit = 1u << (r & 15);
				while ((IopRead32(h.set_reg) & bit) && !h.clearer_done.load(std::memory_order_relaxed))
				{
				}
				IopWrite32(h.set_reg, bit);
			}
			h.setter_done.store(true, std::memory_order_release);
		});
		std::thread ee([&h] {
			u64 cleared = 0;
			for (;;)
			{
				const bool last_pass = h.setter_done.load(std::memory_order_acquire);
				const u32 seen = EeRead(h.clear_reg) & 0xFFFF;
				for (u32 b = 0; b < 16; b++)
				{
					if (seen & (1u << b))
					{
						EeWrite(h.clear_reg, 1u << b);
						cleared++;
					}
				}
				if (last_pass)
					break;
			}
			h.cleared = cleared;
			h.clearer_done.store(true, std::memory_order_release);
		});
		iop.join();
		ee.join();
	}
} // namespace

TEST_F(Sbus, F220NoSetIsLostWhenTheEeSetsWhileTheIopClears)
{
	Handshake h;
	h.set_reg = SBUS_F220;
	h.clear_reg = SBUS_F220;
	h.rounds = 1000000;
	RunEeSetsIopClears(h);
	EXPECT_EQ(static_cast<u64>(h.rounds), h.cleared) << "an update to F220 was lost";
	EXPECT_EQ(0u, Word(SBUS_F220) & 0xFFFF);
}

TEST_F(Sbus, F230NoSetIsLostWhenTheIopSetsWhileTheEeClears)
{
	Handshake h;
	h.set_reg = SBUS_F230;
	h.clear_reg = SBUS_F230;
	h.rounds = 1000000;
	RunIopSetsEeClears(h);
	EXPECT_EQ(static_cast<u64>(h.rounds), h.cleared) << "an update to F230 was lost";
	EXPECT_EQ(0u, Word(SBUS_F230) & 0xFFFF);
}

// The control register has the EE changing bit 8 while the IOP toggles bit 4. Each side's bit ends where
// its own operations leave it, whatever the other did meanwhile: the IOP toggled an even number of times,
// and the EE's last write set bit 8.
TEST_F(Sbus, F240EachSidesBitEndsWhereItsOwnWritesLeaveIt)
{
	constexpr u32 kToggles = 200000; // even
	for (int trial = 0; trial < 16; trial++)
	{
		Word(SBUS_F240) = 0;
		std::atomic<bool> go{false};
		std::thread ee([&] {
			while (!go.load(std::memory_order_acquire))
			{
			}
			for (u32 i = 0; i < kToggles; i++)
				EeWrite(SBUS_F240, (i & 1) ? 0 : 0x100); // set, clear, set, clear ... ends cleared
			EeWrite(SBUS_F240, 0x100); // and one more: set
		});
		std::thread iop([&] {
			while (!go.load(std::memory_order_acquire))
			{
			}
			for (u32 i = 0; i < kToggles; i++)
				IopWrite32(SBUS_F240, 0x10); // toggle bit 4
		});
		go.store(true, std::memory_order_release);
		ee.join();
		iop.join();
		EXPECT_EQ(0x100u, Word(SBUS_F240)) << "trial " << trial << ": an update to F240 was lost";
		if (Word(SBUS_F240) != 0x100u)
			break;
	}
}
