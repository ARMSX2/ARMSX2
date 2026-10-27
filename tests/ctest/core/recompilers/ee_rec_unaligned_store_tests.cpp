// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// SDL and SDR (unaligned doubleword stores) on the fastmem path.
//
// The fastmem form is one branch-free read-merge-write for all eight
// alignments, the shift a register operand:
//   SDR: mem = (mem & ~(~0 << s)) | (Rt << s),  s = (addr & 7) * 8
//   SDL: mem = (mem & ~(~0 >> s)) | (Rt >> s),  s = (7 - (addr & 7)) * 8
// The softmem form (a PC that has faulted) keeps the two-path form whose
// degenerate alignment skips the read.
//
// EeRecLoadStore.SdlAllAlignments / SdrAllAlignments cover every alignment
// with a pinned Rt. These cover the other homes Rt can have when the store
// compiles (memory, an allocator slot, a 128-bit NEON home, a constant, $zero),
// a base that is not pinned, back-to-back stores off one base (the shape of
// Ratchet & Clank: Up Your Arsenal's VU0 feeder loop), and the emitted shape.

#include "harness/EeRecTestHarness.h"

#include "Config.h"

#include <gtest/gtest.h>

#include <vector>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;

constexpr u64 kRtValue = 0x99AABBCCDDEEFF01ull;

class SoftmemScope
{
public:
	SoftmemScope()
		: saved_(EmuConfig.Cpu.Recompiler.EnableFastmem)
	{
		EmuConfig.Cpu.Recompiler.EnableFastmem = false;
	}
	~SoftmemScope() { EmuConfig.Cpu.Recompiler.EnableFastmem = saved_; }

private:
	bool saved_;
};

void SeedWindow(EeRecTestHarness& h)
{
	h.WriteU64(kScratch + 0, 0x1122334455667788ull);
	h.WriteU64(kScratch + 8, 0x0123456789ABCDEFull);
	h.WriteU64(kScratch + 16, 0xF0E1D2C3B4A59687ull);
	h.WriteU64(kScratch + 24, 0x8877665544332211ull);
	h.TrackMemWindow(kScratch, 32);
}

// B.cond with condition EQ: the old form's degenerate-alignment branch. The
// block tail's event check is B.GE, so it does not count.
u32 BranchEqCount(u32 guest_pc)
{
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
	{
		ADD_FAILURE() << "no compiled block at 0x" << std::hex << guest_pc;
		return 0;
	}
	const u32* code = reinterpret_cast<const u32*>(fn);
	u32 n = 0;
	for (u32 i = 0; i < size / 4; i++)
		n += (code[i] & 0xFF00001Fu) == 0x54000000u;
	return n;
}

} // namespace

// Rt in cpuRegs memory (t1 is not pinned and nothing in the block loads it).
TEST(EeRecUnalignedStore, RtFromMemoryEveryAlignment)
{
	for (s16 off = 0; off <= 7; off++)
	{
		for (bool sdl : {false, true})
		{
			SCOPED_TRACE(testing::Message() << (sdl ? "SDL" : "SDR") << " offset=" << off);
			EeRecTestHarness h;
			SeedWindow(h);
			h.SetGpr64(reg::a0, kScratch);
			h.SetGpr64(reg::t1, kRtValue);
			h.LoadProgram({sdl ? SDL(reg::t1, off, reg::a0) : SDR(reg::t1, off, reg::a0)});
			h.Run();
		}
	}
}

// Rt written by an ALU op earlier in the block, so it is allocator- or
// pin-resident and dirty when the store compiles; the base is not pinned.
TEST(EeRecUnalignedStore, RtResidentAndUnpinnedBase)
{
	for (s16 off = 0; off <= 7; off++)
	{
		for (bool sdl : {false, true})
		{
			SCOPED_TRACE(testing::Message() << (sdl ? "SDL" : "SDR") << " offset=" << off);
			EeRecTestHarness h;
			SeedWindow(h);
			h.SetGpr64(reg::t2, kScratch + 8);
			h.SetGpr64(reg::t1, kRtValue);
			h.SetGpr64(reg::t3, 0x0102030405060708ull);
			h.LoadProgram({
				DADDU(reg::t4, reg::t1, reg::t3),
				sdl ? SDL(reg::t4, off, reg::t2) : SDR(reg::t4, off, reg::t2),
				DADDU(reg::v0, reg::t4, reg::zero), // t4 still readable after the store
			});
			h.Run();
		}
	}
}

// Rt whose newest value lives in a 128-bit NEON home (an MMI result).
TEST(EeRecUnalignedStore, RtInA128BitHome)
{
	for (s16 off : {0, 3, 7})
	{
		for (bool sdl : {false, true})
		{
			SCOPED_TRACE(testing::Message() << (sdl ? "SDL" : "SDR") << " offset=" << off);
			EeRecTestHarness h;
			SeedWindow(h);
			h.SetGpr64(reg::a0, kScratch);
			h.SetGpr64(reg::t1, kRtValue);
			h.SetGpr64(reg::t3, 0x1111111111111111ull);
			h.LoadProgram({
				PADDW(reg::t5, reg::t1, reg::t3),
				sdl ? SDL(reg::t5, off, reg::a0) : SDR(reg::t5, off, reg::a0),
			});
			h.Run();
		}
	}
}

// Rt a known constant, and Rt = $zero.
TEST(EeRecUnalignedStore, RtConstantAndZero)
{
	for (s16 off : {0, 1, 6, 7})
	{
		for (bool sdl : {false, true})
		{
			SCOPED_TRACE(testing::Message() << (sdl ? "SDL" : "SDR") << " offset=" << off);
			EeRecTestHarness h;
			SeedWindow(h);
			h.SetGpr64(reg::a0, kScratch);
			h.LoadProgram({
				LUI(reg::t6, 0x8765),
				ORI(reg::t6, reg::t6, 0x4321),
				sdl ? SDL(reg::t6, off, reg::a0) : SDR(reg::t6, off, reg::a0),
				sdl ? SDL(reg::zero, off + 8, reg::a0) : SDR(reg::zero, off + 8, reg::a0),
			});
			h.Run();
		}
	}
}

// Ratchet & Clank: Up Your Arsenal's feeder loop: six SDL/SDR off one pinned
// base ($v1) at offsets 0, 6, 13, 12, 19, 18, interleaved with other work, so
// no pair is adjacent and none fuses. Run at every base alignment.
TEST(EeRecUnalignedStore, UyaFeederRunOffOneBase)
{
	for (u32 align = 0; align < 8; align++)
	{
		SCOPED_TRACE(testing::Message() << "base alignment " << align);
		EeRecTestHarness h;
		SeedWindow(h);
		h.SetGpr64(reg::v1, kScratch + align);
		h.SetGpr64(reg::s4, 0x0102030405060708ull);
		h.SetGpr64(reg::s5, 0x1112131415161718ull);
		h.SetGpr64(reg::s2, 0x2122232425262728ull);
		h.SetGpr64(reg::s3, 0x3132333435363738ull);
		h.SetGpr64(reg::t0, 0x7777);
		h.LoadProgram({
			SDR(reg::s4, 0x0, reg::v1),
			SRL(reg::t6, reg::t0, 3),
			SDR(reg::s5, 0x6, reg::v1),
			DADDU(reg::s6, reg::s4, reg::s5),
			SDL(reg::s5, 0xD, reg::v1),
			ANDI(reg::t0, reg::t0, 0x1FF),
			SDR(reg::s2, 0xC, reg::v1),
			DADDU(reg::s7, reg::s6, reg::s2),
			SDL(reg::s2, 0x13, reg::v1),
			ADDU(reg::t7, reg::t0, reg::t6),
			SDR(reg::s3, 0x12, reg::v1),
		});
		h.Run();
	}
}

// Shape: the fastmem form has no alignment branch.
TEST(EeRecUnalignedStore, FastmemFormHasNoAlignmentBranch)
{
	ASSERT_TRUE(CHECK_FASTMEM);
	for (bool sdl : {false, true})
	{
		SCOPED_TRACE(sdl ? "SDL" : "SDR");
		EeRecTestHarness h;
		SeedWindow(h);
		h.SetGpr64(reg::a0, kScratch);
		h.SetGpr64(reg::t1, kRtValue);
		h.LoadProgram({sdl ? SDL(reg::t1, 3, reg::a0) : SDR(reg::t1, 3, reg::a0)});
		h.Run();
		EXPECT_EQ(BranchEqCount(kProgPc), 0u);
	}
}

// The control: softmem keeps the two-path form, so the counter is not blind.
TEST(EeRecUnalignedStore, SoftmemFormKeepsTheAlignmentBranch)
{
	SoftmemScope softmem;
	for (bool sdl : {false, true})
	{
		SCOPED_TRACE(sdl ? "SDL" : "SDR");
		EeRecTestHarness h;
		SeedWindow(h);
		h.SetGpr64(reg::a0, kScratch);
		h.SetGpr64(reg::t1, kRtValue);
		h.LoadProgram({sdl ? SDL(reg::t1, 3, reg::a0) : SDR(reg::t1, 3, reg::a0)});
		h.Run();
		EXPECT_EQ(BranchEqCount(kProgPc), 1u);
	}
}
