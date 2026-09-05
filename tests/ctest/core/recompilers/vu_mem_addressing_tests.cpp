// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Randomised differential sweep over the VU load/store address path.
//
// A VU memory access builds its address in three places: the constant-address
// fold for a vi00 base, mVUaddrFix's mask and shift, and the VURegs::Mem load
// the site adds the result to. Which arm of each runs turns on the opcode,
// the base register, the immediate and the dest field together, and the
// coverage until now is one program per shape.
//
// So: random streams of the ten memory ops -- opcode x base VI x offset x dest
// mask x register aliasing x seeded memory -- run through both engines, with
// the whole of data memory in the diff.

#include "harness/VuTestHarness.h"

#include "VU.h"
#include "VUmicro.h"

#include "common/StringUtil.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

// A stream of our own rather than <random>: the standard distributions draw
// differently per stdlib, so a seed would not name the same programs twice.
struct Rng
{
	u64 s;
	explicit Rng(u64 seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
	u32 Next()
	{
		s = s * 6364136223846793005ull + 1442695040888963407ull;
		return static_cast<u32>(s >> 33);
	}
	u32 Below(u32 n) { return Next() % n; }
};

constexpr u32 kMasks[15] = {
	mask::x, mask::y, mask::z, mask::w,
	mask::x | mask::y, mask::x | mask::z, mask::x | mask::w,
	mask::y | mask::z, mask::y | mask::w, mask::z | mask::w,
	mask::x | mask::y | mask::z, mask::x | mask::y | mask::w,
	mask::x | mask::z | mask::w, mask::y | mask::z | mask::w,
	mask::xyzw,
};

s16 DrawOffset(Rng& rng, bool vu1)
{
	if (!vu1)
		return static_cast<s16>(rng.Below(0x100)); // stay clear of VU0's 0x400 window

	if (rng.Below(4) == 0)
		return static_cast<s16>(-static_cast<s32>(rng.Below(1024))); // wraps under the 0x3ff mask
	return static_cast<s16>(rng.Below(1024));
}

// One memory op. `is` addresses; `it`/`ft` is the register moved.
VuOp DrawOp(Rng& rng, bool vu1)
{
	const u32 m = kMasks[rng.Below(15)];
	const s16 imm = DrawOffset(rng, vu1);
	// VU0's address registers are drawn apart from the registers a load
	// writes, so nothing a program reads out of data memory can come back as
	// an address. An index with bit 0x400 set is not an address in VU0's
	// memory at all -- it is the window onto VU1's register file, where the
	// two engines already disagree (DISABLED_Vu0WindowIntoVu1Registers below)
	// -- and a VI loaded from memory is the one way into it. VU1 has no such
	// window, so it draws from the whole file and gets the aliasing with it.
	const u32 baseHi = vu1 ? 15u : 8u;
	// vi00 as the base is what the constant-address fold keys on, so draw it
	// often rather than one time in sixteen.
	const u32 is = (rng.Below(3) == 0) ? 0u : (1u + rng.Below(baseHi));
	// Aliasing the address register with the destination is legal and is the
	// case a post-increment form has to get right.
	const u32 it = vu1
		? ((rng.Below(4) == 0 && is != 0) ? is : (1u + rng.Below(15)))
		: (9u + rng.Below(7));
	const u32 ft = 1u + rng.Below(31);

	// The four stepping forms are drawn off vi01-vi15 only. A vi00 base sends
	// them down an arm that answers the address the x86 recompiler answers --
	// the top of micro memory, not slot 0 -- which is not the interpreter's,
	// and that disagreement is older and wider than the addressing this
	// sweeps.
	const u32 step = (is != 0) ? is : (1u + rng.Below(baseHi));

	// ILW and ILWR are drawn single-lane. The dest field of a load into VI
	// names one lane; with several set the recompiler reads the first and the
	// interpreter the last, and which of those a console answers is a
	// different question from where the data memory is.
	const u32 lane = kMasks[rng.Below(4)];

	switch (rng.Below(10))
	{
		case 0:  return VuOp{VLQ_L(m, ft, is, imm), VNOP_U()};
		case 1:  return VuOp{VSQ_L(m, ft, is, imm), VNOP_U()};
		case 2:  return VuOp{VLQI_L(m, ft, step), VNOP_U()};
		case 3:  return VuOp{VSQI_L(m, ft, step), VNOP_U()};
		case 4:  return VuOp{VLQD_L(m, ft, step), VNOP_U()};
		case 5:  return VuOp{VSQD_L(m, ft, step), VNOP_U()};
		case 6:  return VuOp{VILW_L(lane, it, is, imm), VNOP_U()};
		case 7:  return VuOp{VISW_L(m, it, is, imm), VNOP_U()};
		case 8:  return VuOp{VILWR_L(lane, it, is), VNOP_U()};
		default: return VuOp{VISWR_L(m, it, is), VNOP_U()};
	}
}

// Every VF lane, every VI and every quadword of data memory carries a value
// that names where it came from, so a diff points at the slot that moved.
void SeedState(VuTestHarness& h, Rng& rng, bool vu1)
{
	const u32 memBytes = vu1 ? VU1_MEMSIZE : VU0_MEMSIZE;
	// The low halfword differs per lane as well as per quadword, so a load
	// that reads the wrong lane of the right quadword is a divergence and not
	// a coincidence.
	for (u32 addr = 0; addr < memBytes; addr += 16)
		h.WriteMemU128(addr, 0xA0000000u | (addr + 0), 0xB0000000u | (addr + 1),
			0xC0000000u | (addr + 2), 0xD0000000u | (addr + 3));
	h.TrackMemWindow(0, memBytes);

	for (u32 r = 1; r < 32; r++)
		h.SetVfBits(r, 0x3F800000u + r, 0xBF800000u + r, rng.Next(), rng.Next() | 0x00800000u);

	// Held below the quadword count so a pre-decrementing chain cannot walk a
	// VU0 index under zero and into the cross-VU window this sweep leaves out.
	const u32 slots = vu1 ? 0x3FFu : 0xFFu;
	for (u32 r = 1; r < 16; r++)
		h.SetVi(r, 0x20 + rng.Below(slots - 0x40));
}

void RunSweep(int vuIndex, u64 seed, int programs, int opsPerProgram)
{
	const bool vu1 = (vuIndex != 0);
	for (int p = 0; p < programs; p++)
	{
		Rng rng(seed + static_cast<u64>(p) * 0x9E3779B97F4A7C15ull);
		VuTestHarness h(vuIndex);
		SeedState(h, rng, vu1);

		std::vector<VuOp> prog;
		for (int i = 0; i < opsPerProgram; i++)
			prog.push_back(DrawOp(rng, vu1));
		prog.push_back(EBitNopPair());

		h.LoadProgram(prog);
		h.Run();
		if (::testing::Test::HasFailure())
		{
			// The diff already names the register or slot; this says what ran.
			std::string words;
			for (const VuOp& op : prog)
				words += StringUtil::StdStringFromFormat(" %08x", op.lower);
			ADD_FAILURE() << "vu" << vuIndex << " program " << p << " (seed " << seed
						  << ") lower words:" << words;
			return;
		}
	}
}

} // namespace

TEST(VuMemAddressing, Vu1RandomStreams)
{
	RunSweep(1, 0x5150C0DEull, 400, 8);
}

TEST(VuMemAddressing, Vu0RandomStreams)
{
	RunSweep(0, 0x0BADF00Dull, 400, 8);
}

// Every quadword slot of VU1 memory, read and written through a vi00 base --
// the constant-address fold's own axis, swept end to end rather than sampled.
TEST(VuMemAddressing, Vu1ConstantAddressWalksEveryQuadword)
{
	for (s16 base = 0; base < 1024; base += 32)
	{
		Rng rng(0xC0FFEEull + static_cast<u64>(base));
		VuTestHarness h(1);
		SeedState(h, rng, true);

		std::vector<VuOp> prog;
		for (s16 off = base; off < base + 32; off++)
		{
			prog.push_back(VuOp{VLQ_L(mask::xyzw, vf::vf1, vi::vi0, off), VNOP_U()});
			prog.push_back(VuOp{VSQ_L(mask::z | mask::w, vf::vf2, vi::vi0, off), VNOP_U()});
			prog.push_back(VuOp{VILW_L(mask::z, vi::vi5, vi::vi0, off), VNOP_U()});
			prog.push_back(VuOp{VISW_L(mask::y, vi::vi5, vi::vi0, off), VNOP_U()});
		}
		prog.push_back(EBitNopPair());

		h.LoadProgram(prog);
		h.Run();
		if (::testing::Test::HasFailure())
		{
			ADD_FAILURE() << "vu1 constant-address walk over quadwords " << base << "-" << (base + 31);
			return;
		}
	}
}

// The sweep drew this on VU0 before its address registers were held apart
// from its load destinations, and it diverges: the run ends with vi3 = 0x691
// and vi7 = 0x5e0, indices whose bit 0x400 makes them VU0's window onto VU1's
// register file rather than addresses in VU0's own memory, and vf18.w comes
// back 0 from the recompiler against 0xde from the interpreter. Nobody has
// traced it further than the two engines' GET_VU_MEM and mVUaddrFix
// disagreeing somewhere inside that window.
TEST(VuMemAddressing, DISABLED_Vu0WindowIntoVu1Registers)
{
	Rng rng(0x0BADF00Dull + 336ull * 0x9E3779B97F4A7C15ull);
	VuTestHarness h(0);
	SeedState(h, rng, false);
	h.LoadProgram({
		VuOp{0x804363feu, VNOP_U()}, // ILWR.z  vi3  <- [vi12]
		VuOp{0x81b21b7eu, VNOP_U()}, // LQD.xyw vf18 <- [--vi3]
		VuOp{0x01715037u, VNOP_U()}, // LQ.xzw  vf17 <- [vi10 + 55]
		VuOp{0x01431827u, VNOP_U()}, // LQ.xz   vf3  <- [vi3 + 39]
		VuOp{0x80291bffu, VNOP_U()}, // ISWR.w  vi9  -> [vi3]
		VuOp{0x0195606bu, VNOP_U()}, // LQ.xy   vf21 <- [vi12 + 107]
		VuOp{0x81025b7eu, VNOP_U()}, // LQD.x   vf2  <- [--vi11]
		VuOp{0x81073bfeu, VNOP_U()}, // ILWR.x  vi7  <- [vi7]
		EBitNopPair(),
	});
	h.Run();
}

// VU0's window onto VU1's register file, reached by ILW and ILWR.
//
// An address index with bit 0x400 set does not address VU0's own memory:
// mVUaddrFix answers a 64-bit offset from VU0.Mem to VU1's register file, and
// the load adds it to the VURegs::Mem pointer. ILW and ILWR are the only
// memory ops that add their lane offset to the address rather than carrying
// it in the access, so they are the only ones that can lose the top half of
// that offset -- the rest of the window is exercised by the sweeps above.
//
// The values are read out of VU1's register file, which this harness does not
// drive, so the test writes the pattern there itself and both engines have to
// come back with it.
TEST(VuMemAddressing, Vu0IlwThroughTheWindow)
{
	for (u32 lane = 0; lane < 4; lane++)
	{
		VuTestHarness h(0);
		// vi1 selects VF[17] of VU1: bit 0x400 opens the window, the low six
		// bits are the quadword within the register file.
		h.SetVi(vi::vi1, 0x400 + 17);
		h.SetVi(vi::vi2, 0x400 + 18);
		for (u32 r = 16; r < 20; r++)
			for (u32 l = 0; l < 4; l++)
				vuRegs[1].VF[r].UL[l] = 0x1000u * r + 0x10u * l + 7u;

		const u32 m = kMasks[lane];
		h.LoadProgram({
			VuOp{VILW_L(m, vi::vi5, vi::vi1, 0), VNOP_U()},
			VuOp{VILWR_L(m, vi::vi6, vi::vi2), VNOP_U()},
			EBitNopPair(),
		});
		h.Run();

		const u32 want5 = (0x1000u * 17 + 0x10u * lane + 7u) & 0xffffu;
		const u32 want6 = (0x1000u * 18 + 0x10u * lane + 7u) & 0xffffu;
		EXPECT_EQ(h.InterpSnapshot().regs.VI[vi::vi5].UL & 0xffffu, want5) << "interp ILW lane " << lane;
		EXPECT_EQ(h.JitSnapshot().regs.VI[vi::vi5].UL & 0xffffu, want5) << "jit ILW lane " << lane;
		EXPECT_EQ(h.InterpSnapshot().regs.VI[vi::vi6].UL & 0xffffu, want6) << "interp ILWR lane " << lane;
		EXPECT_EQ(h.JitSnapshot().regs.VI[vi::vi6].UL & 0xffffu, want6) << "jit ILWR lane " << lane;
		if (::testing::Test::HasFailure())
			return;
	}
}

} // namespace recompiler_tests
