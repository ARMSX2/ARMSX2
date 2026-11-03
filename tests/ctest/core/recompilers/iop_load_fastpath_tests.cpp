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

#include <cstddef>
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

// How many guest registers are live (computed from a runtime value, so each sits
// dirty in a host register) decides which host register the load's destination
// gets and which ones the slow path has to give back. Sweep it from none to more
// than the allocator has, for a RAM load and for loads that leave RAM.
TEST(IopLoadFastPath, LiveRegisterCountSweep)
{
	// 24 guest registers a program can use freely (not the ones it needs for operands).
	static const u32 pool[] = {reg::t0, reg::t1, reg::t2, reg::t3, reg::t4, reg::t5, reg::t6, reg::t7,
	                           reg::s0, reg::s1, reg::s2, reg::s3, reg::s4, reg::s5, reg::s6, reg::s7,
	                           reg::t8, reg::t9, reg::gp, reg::s8, reg::at, reg::k0, reg::k1, reg::a3};
	static const u32 addrs[] = {kBase, 0xA0420000u, 0x1F801070u, 0x1F801074u, 0x1FC00000u, 0x00800000u};
	for (const u32 addr : addrs)
	{
		for (size_t live = 0; live <= std::size(pool); live++)
		{
			SCOPED_TRACE("addr " + std::to_string(addr) + ", live " + std::to_string(live));
			JitTestHarness h;
			SeedRam(h);
			h.SetGpr(reg::a0, 0x1000u);
			h.SetGpr(reg::a1, addr);
			std::vector<u32> prog;
			for (size_t i = 0; i < live; i++)
				prog.push_back(ADDIU(pool[i], reg::a0, static_cast<s16>(i + 1)));
			prog.push_back(LW(reg::v0, 0, reg::a1));
			prog.push_back(LBU(reg::v1, 3, reg::a1));
			// use every live value after the loads
			for (size_t i = 0; i < live; i++)
				prog.push_back(XOR(reg::a2, reg::a2, pool[i]));
			h.LoadProgramAt(RecompilerTestEnvironment::kProgramPc, prog.data(), prog.size(), true);
			h.Run();
		}
	}
}

// A load in a branch delay slot is compiled once when the delay slot can be hoisted above the
// branch and twice (taken and not-taken) when it cannot; either way its slow path sits in
// the block with the allocator in whatever state that path had.
TEST(IopLoadFastPath, LoadInABranchDelaySlot)
{
	static const u32 addrs[] = {kBase, 0x1F801070u, 0x1FC00000u};
	for (const u32 addr : addrs)
	{
		for (const bool taken : {false, true})
		{
			for (const int form : {0, 1, 2, 3})
			{
				SCOPED_TRACE("addr " + std::to_string(addr) + (taken ? ", taken" : ", not taken") + ", form " + std::to_string(form));
				JitTestHarness h;
				SeedRam(h);
				h.SetGpr(reg::a0, 0x2000u);
				h.SetGpr(reg::a1, addr);
				h.SetGpr(reg::a2, 5);
				h.SetGpr(reg::a3, taken ? 5u : 6u);
				u32 branch, delay;
				switch (form)
				{
					case 0: branch = BEQ(reg::a2, reg::a3, 7); delay = LW(reg::v0, 0, reg::a1); break;   // hoistable
					case 1: branch = BEQ(reg::a2, reg::a3, 7); delay = LW(reg::a2, 0, reg::a1); break;   // writes a branch source
					case 2: branch = BNE(reg::a2, reg::a3, 7); delay = LBU(reg::a3, 1, reg::a1); break;  // writes the other source
					default: branch = BGTZ(reg::a2, 7); delay = LH(reg::v0, 2, reg::a1); break;
				}
				if (form == 2)
					h.SetGpr(reg::a3, taken ? 6u : 5u); // BNE: taken when they differ
				if (form == 3)
					h.SetGpr(reg::a2, taken ? 5u : 0u); // BGTZ: taken when positive
				h.LoadProgramNoTerm({
					ADDIU(reg::t0, reg::a0, 1), // live across the delay-slot load
					ADDIU(reg::t1, reg::a0, 2),
					branch,
					delay,
					ADDIU(reg::t2, reg::t0, 100), ADDIU(reg::v1, reg::zero, 1), J(RecompilerTestEnvironment::kParkingPc), NOP,
					NOP,
					ADDIU(reg::t2, reg::t1, 200), ADDIU(reg::v1, reg::zero, 2), J(RecompilerTestEnvironment::kParkingPc), NOP,
				});
				h.Run();
			}
		}
	}
}

// JR and JALR take their target from a register while the delay slot loads.
TEST(IopLoadFastPath, LoadInAJumpRegisterDelaySlot)
{
	static const u32 addrs[] = {kBase, 0x1F801070u};
	for (const u32 addr : addrs)
	{
		SCOPED_TRACE(addr);
		JitTestHarness h;
		SeedRam(h);
		h.SetGpr(reg::a0, 0x3000u);
		h.SetGpr(reg::a1, addr);
		h.SetGpr(reg::t5, RecompilerTestEnvironment::kParkingPc);
		h.LoadProgramNoTerm({
			ADDIU(reg::t0, reg::a0, 1),
			ADDIU(reg::t1, reg::a0, 2),
			JALR(reg::ra, reg::t5),
			LW(reg::v0, 0, reg::a1),
			NOP,
		});
		h.Run();
	}
}

// LWL/LWR/SWL/SWR still flush and call C; they have to find the registers a load before
// them left dirty.
TEST(IopLoadFastPath, UnalignedOpsAfterLoads)
{
	JitTestHarness h;
	SeedRam(h);
	h.TrackMemWindow(kBase + 0x40, 16);
	h.SetGpr(reg::a0, kBase);
	h.SetGpr(reg::a1, 0x1F801070u);
	h.LoadProgram({
		LW(reg::t0, 0, reg::a0),
		LBU(reg::t1, 5, reg::a0),
		LW(reg::t2, 0, reg::a1),
		ADDU(reg::t3, reg::t0, reg::t1),
		LWL(reg::t3, 6, reg::a0),
		LWR(reg::t3, 3, reg::a0),
		LH(reg::t4, 0, reg::a0),
		SWL(reg::t3, 0x42, reg::a0),
		SWR(reg::t3, 0x45, reg::a0),
		LW(reg::t5, 0x40, reg::a0),
		ADDU(reg::t6, reg::t5, reg::t4),
	});
	h.Run();
}

// ---------------------------------------------------------------------------
// The cold stub's contract, called directly: every allocator register comes
// back, and the value is extended. Whether the C function it calls happens to
// use a given register is up to the compiler, so check a spread of handlers.
// ---------------------------------------------------------------------------

#include "IopMem.h"
#include "arm64/iR3000A-arm64.h"

namespace {

// In: x0-x7, x11-x15 from `in`, w8 = address, x21 = &psxRegs, x19 = rc, w20 = re (the pinned
// clocks, see RIOPCYC). Out: the same registers, and x9.
//
// The asm takes one input, a pointer to this block, and keeps it in x22 for the whole call. Eight
// separate register operands do not fit: the asm clobbers x0-x17, x19-x21 and x30, which leaves
// x22-x28 and, where they are not reserved, x18 and x29. On Darwin x18 and x29 are reserved, so
// seven registers are left for eight operands and the compiler gives up. Elsewhere it would put an
// operand in x18, which a C call in the stub is free to overwrite, and the asm reads that operand
// again after the call. x22 is callee-saved, so the stub's C calls give it back.
struct ColdStubCall
{
	const void* stub;  // 0
	const u64* in;     // 8
	u64* out;          // 16
	u64* x9;           // 24
	u64* rc;           // 32
	u32* re;           // 40
	void* state;       // 48
	u64 addr;          // 56
};
static_assert(offsetof(ColdStubCall, stub) == 0 && offsetof(ColdStubCall, in) == 8 && offsetof(ColdStubCall, out) == 16 &&
              offsetof(ColdStubCall, x9) == 24 && offsetof(ColdStubCall, rc) == 32 && offsetof(ColdStubCall, re) == 40 &&
              offsetof(ColdStubCall, state) == 48 && offsetof(ColdStubCall, addr) == 56,
	"the asm in CallColdStub addresses these fields by offset");

void CallColdStub(const void* stub, u32 addr, const u64 (&in)[13], u64 (&out)[13], u64& x9, u64& rc, u32& re)
{
	ColdStubCall call = {stub, in, out, &x9, &rc, &re, &psxRegs, addr};
	asm volatile(
		"mov x22, %[call]\n\t"
		"ldr x16, [x22, #8]\n\t" // in
		"ldp x0, x1, [x16, #0]\n\t"
		"ldp x2, x3, [x16, #16]\n\t"
		"ldp x4, x5, [x16, #32]\n\t"
		"ldp x6, x7, [x16, #48]\n\t"
		"ldp x11, x12, [x16, #64]\n\t"
		"ldp x13, x14, [x16, #80]\n\t"
		"ldr x15, [x16, #96]\n\t"
		"ldr w8, [x22, #56]\n\t"  // addr
		"ldr x21, [x22, #48]\n\t" // state
		"ldr x16, [x22, #32]\n\t" // rc
		"ldr x19, [x16]\n\t"
		"ldr x16, [x22, #40]\n\t" // re
		"ldr w20, [x16]\n\t"
		"ldr x17, [x22, #0]\n\t" // stub
		"blr x17\n\t"
		"ldr x16, [x22, #16]\n\t" // out
		"stp x0, x1, [x16, #0]\n\t"
		"stp x2, x3, [x16, #16]\n\t"
		"stp x4, x5, [x16, #32]\n\t"
		"stp x6, x7, [x16, #48]\n\t"
		"stp x11, x12, [x16, #64]\n\t"
		"stp x13, x14, [x16, #80]\n\t"
		"str x15, [x16, #96]\n\t"
		"ldr x16, [x22, #24]\n\t" // x9
		"str x9, [x16]\n\t"
		"ldr x16, [x22, #32]\n\t" // rc
		"str x19, [x16]\n\t"
		"ldr x16, [x22, #40]\n\t" // re
		"str w20, [x16]\n\t"
		:
		: [call] "r"(&call)
		: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		  "x19", "x20", "x21", "x22", "x30", "memory", "cc");
}

} // namespace

TEST(IopLoadFastPath, ColdStubGivesEveryAllocatorRegisterBack)
{
	JitTestHarness h; // brings the IOP recompiler (and its stubs) up
	static const u32 addrs[] = {0x1F801070u, 0x1F801074u, 0x1F801078u, 0x1F8010F4u, 0x1F801450u, 0x1F801000u, 0x1F803000u,
	                            0x1F808260u, 0x1F802000u, 0x1F9005C8u, 0x1F900188u, 0x1D000000u, 0x1D000040u, 0x10000000u,
	                            0x1FC00000u, 0x1E000000u, 0x1F000000u, 0x1F400000u, 0x00800000u, 0x1F801100u, 0x1F801800u};
	for (const Kind kind : kAllKinds)
	{
		const void* stub = g_iopLoadColdStub[static_cast<int>(kind)];
		ASSERT_NE(stub, nullptr);
		for (const u32 addr : addrs)
		{
			SCOPED_TRACE(std::string(KindName(kind)) + " " + std::to_string(addr));
			u64 in[13], out[13], x9 = 0;
			for (int i = 0; i < 13; i++)
				in[i] = 0xA5A5000000000000ull | (static_cast<u64>(i + 1) << 32) | (0x0F0F0000u + i);
			// The pinned clocks: the stub must make memory say what they say (and give them back unchanged;
			// the device read here does not move them).
			// (The SPU2 registers advance the SPU2 to psxRegs.cycle, which the test environment has no SPU2 for:
			// the clocks there stay at zero.)
			const bool spu2 = (addr & 0x1FFF0000u) == 0x1F900000u;
			const u64 next = spu2 ? 0 : 20000;
			psxRegs.cycle = 12345;
			psxRegs.iopNextEventCycle = next;
			psxRegs.iopCycleEE = 99;
			u64 rc = spu2 ? 0 : 777; // cycle - iopNextEventCycle
			u32 re = 4321;
			if (spu2)
				psxRegs.cycle = 0;
			CallColdStub(stub, addr, in, out, x9, rc, re);
			for (int i = 0; i < 13; i++)
				EXPECT_EQ(out[i], in[i]) << "register slot " << i;
			EXPECT_EQ(psxRegs.cycle, next + (spu2 ? 0 : 777u)) << "the stub spills RIOPCYC to psxRegs.cycle";
			EXPECT_EQ(psxRegs.iopCycleEE, 4321) << "the stub spills RIOPEE to psxRegs.iopCycleEE";
			EXPECT_EQ(rc, spu2 ? 0u : 777u) << "RIOPCYC comes back as it was";
			EXPECT_EQ(re, 4321u) << "RIOPEE comes back as it was";

			// the value is what iopMemRead* gives, extended; read it with the registers the same
			u32 want;
			switch (kind)
			{
				case Kind::LB:  want = static_cast<u32>(static_cast<s32>(static_cast<s8>(iopMemRead8(addr)))); break;
				case Kind::LBU: want = iopMemRead8(addr); break;
				case Kind::LH:  want = static_cast<u32>(static_cast<s32>(static_cast<s16>(iopMemRead16(addr)))); break;
				case Kind::LHU: want = iopMemRead16(addr); break;
				default:        want = iopMemRead32(addr); break;
			}
			EXPECT_EQ(static_cast<u32>(x9), want);
			EXPECT_EQ(x9 >> 32, 0u) << "w9 write must leave the upper half clear";
		}
	}
}
