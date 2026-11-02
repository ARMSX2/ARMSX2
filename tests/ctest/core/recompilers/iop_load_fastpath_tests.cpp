// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The IOP recompiler's RAM-load fast path against the interpreter. A load goes
// to iopMemRead*, which serves RAM from a lookup table and everything else
// (hardware registers, ROM, SIF, DEV9, the parallel-port page, unmapped space)
// by address; the recompiler decides RAM-versus-other from the address bits
// without that table, so the thing to prove is that its decision and its
// extension agree with the interpreter's for every kind of address:
//
//   * every access width and signedness, against RAM in each segment (KUSEG,
//     KSEG0, KSEG1) and each mirror of the 2MB image across the 8MB window,
//   * the edges: last byte/word of the image, accesses across a 64KB page
//     boundary, the first address past the RAM window, unaligned addresses,
//   * every non-RAM class the C function distinguishes,
//   * the base register in the forms the code generator treats differently:
//     runtime, with a positive or negative offset, equal to the destination,
//     a compile-time constant, and a $zero destination,
//   * guest registers that are live (dirty, not yet stored) across a load, so
//     whatever the load does about the register allocator is visible.

#include "harness/JitTestHarness.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kBase = RecompilerTestEnvironment::kScratchAddr; // 0x00020000

enum class Kind
{
	LB,
	LBU,
	LH,
	LHU,
	LW,
};

constexpr Kind kAllKinds[] = {Kind::LB, Kind::LBU, Kind::LH, Kind::LHU, Kind::LW};

const char* KindName(Kind k)
{
	switch (k)
	{
		case Kind::LB:  return "LB";
		case Kind::LBU: return "LBU";
		case Kind::LH:  return "LH";
		case Kind::LHU: return "LHU";
		case Kind::LW:  return "LW";
	}
	return "?";
}

u32 Load(Kind k, u32 rt, s16 off, u32 base)
{
	switch (k)
	{
		case Kind::LB:  return LB(rt, off, base);
		case Kind::LBU: return LBU(rt, off, base);
		case Kind::LH:  return LH(rt, off, base);
		case Kind::LHU: return LHU(rt, off, base);
		case Kind::LW:  return LW(rt, off, base);
	}
	return 0;
}

// Bytes with the top bit set in every position, so sign extension of a byte or
// a halfword is visible wherever the access lands in a word.
u32 Pattern(u32 addr)
{
	const u32 a = addr & ~3u;
	return 0x81C3A5E7u ^ (a * 0x01010101u) ^ (a << 9);
}

// Fill the RAM spots the address lists below read, through the guest memory
// path (which the harness also tracks for the post-run diff).
void SeedRam(JitTestHarness& h)
{
	static const u32 spots[] = {
		0x00000000u, 0x00020000u, 0x0002fff8u, 0x00030000u, 0x00030008u, 0x0004fffcu, 0x00050000u,
		0x001ffff0u, 0x001ffff8u, 0x00200000u, // the word at 0x200000 is a mirror of 0
	};
	for (const u32 s : spots)
	{
		if (s < 0x200000u)
		{
			for (u32 a = s; a < s + 12 && a < 0x200000u; a += 4)
				h.WriteU32(a, Pattern(a));
		}
	}
}

// Addresses grouped by what iopMemRead* does with them.
struct AddrCase
{
	const char* what;
	u32 addr;
};

const AddrCase kRamAddrs[] = {
	{"phys", 0x00020000u},
	{"phys+4", 0x00020004u},
	{"kseg0", 0x80020000u},
	{"kseg1", 0xA0020000u},
	{"mirror +2MB", 0x00220000u},
	{"mirror +4MB kseg0", 0x80420000u},
	{"mirror +6MB kseg1", 0xA0620000u},
	{"unaligned +1", 0x00020001u},
	{"unaligned +2", 0x00020002u},
	{"unaligned +3", 0x00020003u},
	{"unaligned kseg1 +3", 0xA0020003u},
	{"page edge -2", 0x0002fffeu},
	{"page edge -1", 0x0002ffffu},
	{"page edge 0", 0x00030000u},
	{"page edge mirror", 0x0043fffeu},
	{"page edge kseg0 mirror", 0x8083fffeu},
	{"last word of the image", 0x001ffffcu},
	{"last byte of the image", 0x001fffffu},
	{"last halfword of the image", 0x001ffffeu},
	{"image +0 (wraps to 0)", 0x00200000u},
	{"first word", 0x00000000u},
	{"last word of the 8MB window", 0x007ffffcu},
	{"last byte of the 8MB window", 0x007fffffu},
};

const AddrCase kOtherAddrs[] = {
	{"first address past the RAM window", 0x00800000u},
	{"past the RAM window kseg1", 0xA0800000u},
	{"I_STAT", 0x1F801070u},
	{"I_MASK", 0x1F801074u},
	{"I_CTRL", 0x1F801078u},
	{"DMA control", 0x1F8010F4u},
	{"SBUS", 0x1F801450u},
	{"hw page 0x1000 region", 0x1F801000u},
	{"hw page 0x3000 region", 0x1F803000u},
	{"hw page 0x8000 region", 0x1F808260u},
	{"hw page default", 0x1F802000u},
	{"I_STAT kseg1", 0xBF801070u},
	// Not the counters (0x1f801100 and friends): their value depends on psxRegs.cycle, which the recompiler
	// advances once per block and the interpreter once per instruction, so JIT and interpreter differ by design.
	{"SPU2 status", 0x1F9005C8u},
	{"SPU2 register", 0x1F900188u},
	{"SIF", 0x1D000000u},
	{"SIF 0x40", 0x1D000040u},
	{"DEV9", 0x10000000u},
	{"BIOS ROM", 0x1FC00000u},
	{"BIOS ROM kseg1", 0xBFC00100u},
	{"ROM1", 0x1E000000u},
	{"ROM2", 0x1E400000u},
	{"parallel-port page", 0x1F000000u},
	{"hw4 page", 0x1F400000u},
	{"unmapped 0x1f600000", 0x1F600000u},
	{"unmapped 0x1e800000", 0x1E800000u},
	{"top of the physical space", 0x1FFFFFFCu},
	{"kseg2 alias", 0xFFFFFFFCu},
};

// Shapes of the base register.
enum class Shape
{
	RuntimeBase,      // lw v0, 0(a0)
	PositiveOffset,   // a0 = addr - 0x44; lw v0, 0x44(a0)
	NegativeOffset,   // a0 = addr + 0x20; lw v0, -0x20(a0)
	RsIsRt,           // lw a0, 0(a0)
	ZeroDestination,  // lw zero, 0(a0)
	ConstBase,        // lui t0, hi; lw v0, lo(t0)  (address known at compile time)
	ConstBaseRsIsRt,  // lui t0, hi; lw t0, lo(t0)
};

const char* ShapeName(Shape s)
{
	switch (s)
	{
		case Shape::RuntimeBase:     return "runtime";
		case Shape::PositiveOffset:  return "+offset";
		case Shape::NegativeOffset:  return "-offset";
		case Shape::RsIsRt:          return "rs==rt";
		case Shape::ZeroDestination: return "rt=$zero";
		case Shape::ConstBase:       return "const";
		case Shape::ConstBaseRsIsRt: return "const rs==rt";
	}
	return "?";
}

constexpr Shape kAllShapes[] = {Shape::RuntimeBase,    Shape::PositiveOffset,  Shape::NegativeOffset, Shape::RsIsRt,
                                Shape::ZeroDestination, Shape::ConstBase,      Shape::ConstBaseRsIsRt};

void RunCase(Kind kind, Shape shape, const AddrCase& c)
{
	SCOPED_TRACE(std::string(KindName(kind)) + " / " + ShapeName(shape) + " / " + c.what);
	JitTestHarness h;
	SeedRam(h);
	h.SetGpr(reg::v0, 0x0BADF00Du); // a destination that must be overwritten, not merged with
	h.SetGpr(reg::t0, 0x13572468u);
	switch (shape)
	{
		case Shape::RuntimeBase:
			h.SetGpr(reg::a0, c.addr);
			h.LoadProgram({Load(kind, reg::v0, 0, reg::a0)});
			break;
		case Shape::PositiveOffset:
			h.SetGpr(reg::a0, c.addr - 0x44u);
			h.LoadProgram({Load(kind, reg::v0, 0x44, reg::a0)});
			break;
		case Shape::NegativeOffset:
			h.SetGpr(reg::a0, c.addr + 0x20u);
			h.LoadProgram({Load(kind, reg::v0, -0x20, reg::a0)});
			break;
		case Shape::RsIsRt:
			h.SetGpr(reg::a0, c.addr);
			h.LoadProgram({Load(kind, reg::a0, 0, reg::a0)});
			break;
		case Shape::ZeroDestination:
			h.SetGpr(reg::a0, c.addr);
			h.LoadProgram({Load(kind, reg::zero, 0, reg::a0)});
			break;
		case Shape::ConstBase:
		case Shape::ConstBaseRsIsRt:
		{
			const s16 lo = static_cast<s16>(c.addr & 0xFFFFu);
			const u16 hi = static_cast<u16>((c.addr - static_cast<u32>(static_cast<s32>(lo))) >> 16);
			h.LoadProgram({LUI(reg::t0, hi),
			               Load(kind, shape == Shape::ConstBase ? reg::v0 : reg::t0, lo, reg::t0)});
			break;
		}
	}
	h.Run();
	if (shape != Shape::ZeroDestination)
		EXPECT_EQ(h.GetGprJit(reg::zero), 0u);
}

} // namespace

TEST(IopLoadFastPath, RamAddressesAllKindsAllShapes)
{
	for (const Kind kind : kAllKinds)
		for (const Shape shape : kAllShapes)
			for (const AddrCase& c : kRamAddrs)
				RunCase(kind, shape, c);
}

TEST(IopLoadFastPath, NonRamAddressesAllKindsAllShapes)
{
	for (const Kind kind : kAllKinds)
		for (const Shape shape : kAllShapes)
			for (const AddrCase& c : kOtherAddrs)
				RunCase(kind, shape, c);
}

// A value read from RAM must be what the interpreter read, not just agree with
// it: the diff alone would pass if both were wrong the same way.
TEST(IopLoadFastPath, ExtensionOfAKnownWord)
{
	JitTestHarness h;
	h.WriteU32(kBase, 0x8081FF7Fu); // bytes 7F FF 81 80
	h.SetGpr(reg::a0, kBase);
	h.LoadProgram({
		LB(reg::v0, 0, reg::a0),  // 0x7f
		LB(reg::v1, 1, reg::a0),  // 0xff -> -1
		LBU(reg::t0, 1, reg::a0), // 0xff
		LH(reg::t1, 2, reg::a0),  // 0x8081 -> sign extended
		LHU(reg::t2, 2, reg::a0), // 0x8081
		LW(reg::t3, 0, reg::a0),
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0x7Fu);
	EXPECT_EQ(h.GetGprInterp(reg::v1), 0xFFFFFFFFu);
	EXPECT_EQ(h.GetGprInterp(reg::t0), 0xFFu);
	EXPECT_EQ(h.GetGprInterp(reg::t1), 0xFFFF8081u);
	EXPECT_EQ(h.GetGprInterp(reg::t2), 0x8081u);
	EXPECT_EQ(h.GetGprInterp(reg::t3), 0x8081FF7Fu);
	EXPECT_EQ(h.GetGprJit(reg::v1), 0xFFFFFFFFu);
	EXPECT_EQ(h.GetGprJit(reg::t1), 0xFFFF8081u);
}

// Registers that hold a value the guest has computed but the recompiler has not
// stored yet, live across a load: before, between and after loads that hit RAM,
// a mirror, and a hardware register.
TEST(IopLoadFastPath, LiveRegistersSurviveEveryKindOfLoad)
{
	static const u32 addrs[] = {kBase, 0x80220000u, 0x1F801070u, 0x1FC00000u, 0x00800000u, 0x1F801074u};
	for (const u32 addr : addrs)
	{
		SCOPED_TRACE(addr);
		JitTestHarness h;
		SeedRam(h);
		h.SetGpr(reg::a0, addr);
		h.SetGpr(reg::s0, 0x1111u);
		h.LoadProgram({
			ADDIU(reg::t1, reg::s0, 0x123),       // t1, t2, t3 live and dirty
			ADDIU(reg::t2, reg::t1, -7),
			XOR(reg::t3, reg::t1, reg::t2),
			LW(reg::v0, 0, reg::a0),              // the load
			ADDU(reg::t4, reg::t1, reg::t2),      // uses them after it
			ADDU(reg::t5, reg::t3, reg::v0),
			LBU(reg::v1, 1, reg::a0),             // a second load while t4/t5 are live
			ADDU(reg::t6, reg::t4, reg::v1),
			SUBU(reg::t7, reg::t5, reg::t6),
			LHU(reg::t1, 2, reg::a0),             // destination is a live source of the next op
			ADDU(reg::s1, reg::t1, reg::t4),
		});
		h.Run();
	}
}

// A load whose destination is a register the load itself just made dirty, and
// loads in a row: the register-allocator state after one load must be right for
// the next.
TEST(IopLoadFastPath, BackToBackLoadsAndDependentLoads)
{
	JitTestHarness h;
	SeedRam(h);
	// A chain through RAM: each word holds the address of the next.
	h.WriteU32(kBase + 0, kBase + 0x10);
	h.WriteU32(kBase + 0x10, 0x80000000u | (kBase + 0x20));
	h.WriteU32(kBase + 0x20, 0xA0000000u | (kBase + 0x30));
	h.WriteU32(kBase + 0x30, 0x1F801070u);
	h.SetGpr(reg::a0, kBase);
	h.LoadProgram({
		LW(reg::a0, 0, reg::a0),
		LW(reg::a0, 0, reg::a0),
		LW(reg::a0, 0, reg::a0),
		LW(reg::a0, 0, reg::a0),
		LW(reg::v0, 0, reg::a0), // I_STAT
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::a0), 0x1F801070u);
	EXPECT_EQ(h.GetGprJit(reg::a0), 0x1F801070u);
}

// A store and a load of the same bytes through different mirrors, in one block.
TEST(IopLoadFastPath, StoreThenLoadAcrossMirrors)
{
	JitTestHarness h;
	h.TrackMemWindow(kBase, 16);
	h.SetGpr(reg::a0, kBase);
	h.SetGpr(reg::a1, 0xA0000000u | (kBase + 0x00200000u));
	h.SetGpr(reg::a2, 0x80000000u | kBase);
	h.SetGpr(reg::a3, 0xCAFEBABEu);
	h.LoadProgram({
		SW(reg::a3, 0, reg::a0),
		LW(reg::v0, 0, reg::a1),
		LHU(reg::v1, 2, reg::a2),
		SB(reg::v1, 8, reg::a0),
		LB(reg::t0, 8, reg::a1),
	});
	h.Run();
	EXPECT_EQ(h.GetGprInterp(reg::v0), 0xCAFEBABEu);
	EXPECT_EQ(h.GetGprJit(reg::v0), 0xCAFEBABEu);
	EXPECT_EQ(h.GetGprInterp(reg::v1), 0xCAFEu);
}
