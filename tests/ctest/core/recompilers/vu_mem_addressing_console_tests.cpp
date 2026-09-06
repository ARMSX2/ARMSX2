// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// What the console answers for the two corners of the VU load/store address
// path that both engines had been guessing at: the dest field of a load into
// VI, and a stepping form whose base register is vi00.
//
// Captured on a real PS2 over ps2link, one microprogram per case: the VU0
// cases read their answer back through CFC2 / SQC2 and the VU1 cases store it
// into VU1 data memory, since nothing in VU1 is EE-readable. Every case here
// came back byte-identical on two runs from separate ELFs.
//
// Data memory carried a value naming where it came from -- quadword k lane l
// held ((l + 1) << 28) | (k << 4) | l -- so a load that reads the wrong lane
// of the right quadword is a divergence and not a coincidence. The same seed
// is used below.

#include "harness/VuTestHarness.h"

#include "VU.h"
#include "VUmicro.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

u32 Seed(u32 quad, u32 lane)
{
	return ((lane + 1u) << 28) | ((quad & 0xfffu) << 4) | lane;
}

void SeedMem(VuTestHarness& h, bool vu1)
{
	const u32 quads = vu1 ? 1024u : 256u;
	for (u32 k = 0; k < quads; k++)
		h.WriteMemU128(k * 16, Seed(k, 0), Seed(k, 1), Seed(k, 2), Seed(k, 3));
	h.TrackMemWindow(0, quads * 16);
}

u32 MemWord(const VuSnapshot& s, u32 byte)
{
	u32 v = 0;
	std::memcpy(&v, s.mem_windows[0].bytes.data() + byte, 4);
	return v;
}

// Quadwords of data memory the run left holding something other than the seed.
std::vector<u32> ChangedQuads(const VuSnapshot& s, bool vu1)
{
	std::vector<u32> out;
	const u32 quads = vu1 ? 1024u : 256u;
	for (u32 k = 0; k < quads; k++)
		for (u32 l = 0; l < 4; l++)
			if (MemWord(s, k * 16 + l * 4) != Seed(k, l))
			{
				out.push_back(k);
				break;
			}
	return out;
}

// Every dest field, low bit first, so the index is the field itself: bit 3 x,
// bit 2 y, bit 1 z, bit 0 w.
u32 FieldBits(u32 field)
{
	return ((field & 8) ? mask::x : 0) | ((field & 4) ? mask::y : 0)
	     | ((field & 2) ? mask::z : 0) | ((field & 1) ? mask::w : 0);
}

// Byte offset into the quadword that ILW and ILWR read, per dest field. y
// drives bit 0 of a two-bit lane code and z drives bit 1; x drives neither and
// w is not wired to it, so w and an empty field both read lane w, and yz reads
// lane w as well.
constexpr u32 kIlwLaneByte[16] = {
	/* ---- */ 12, /* ---w */ 12, /* --z- */ 8, /* --zw */ 8,
	/* -y-- */  4, /* -y-w */  4, /* -yz- */ 12, /* -yzw */ 12,
	/* x--- */  0, /* x--w */  0, /* x-z- */ 8, /* x-zw */ 8,
	/* xy-- */  4, /* xy-w */  4, /* xyz- */ 12, /* xyzw */ 12,
};

} // namespace

// vi1 starts at a marker no lane of quadword 1 can produce, so a field that
// wrote nothing would be distinguishable from one that read lane x.
TEST(VuMemAddressingConsole, IlwDestFieldNamesOneLane)
{
	for (int vu = 0; vu <= 1; vu++)
	{
		for (u32 field = 0; field < 16; field++)
		{
			VuTestHarness h(vu);
			SeedMem(h, vu != 0);
			h.SetVi(vi::vi1, 0x7777);
			h.SetVi(vi::vi2, 1);
			h.LoadProgram({
				VuOp{VILW_L(FieldBits(field), vi::vi1, vi::vi0, 1), VNOP_U()},
				VuOp{VILWR_L(FieldBits(field), vi::vi3, vi::vi2), VNOP_U()},
				EBitNopPair(),
			});
			h.Run();

			const u32 want = Seed(1, kIlwLaneByte[field] / 4) & 0xffffu;
			EXPECT_EQ(h.JitSnapshot().regs.VI[vi::vi1].UL & 0xffffu, want)
				<< "vu" << vu << " jit ILW field " << field;
			EXPECT_EQ(h.InterpSnapshot().regs.VI[vi::vi1].UL & 0xffffu, want)
				<< "vu" << vu << " interp ILW field " << field;
			EXPECT_EQ(h.JitSnapshot().regs.VI[vi::vi3].UL & 0xffffu, want)
				<< "vu" << vu << " jit ILWR field " << field;
			EXPECT_EQ(h.InterpSnapshot().regs.VI[vi::vi3].UL & 0xffffu, want)
				<< "vu" << vu << " interp ILWR field " << field;
			if (::testing::Test::HasFailure())
				return;
		}
	}
}

// A store's dest field is a set of lanes and not a code: every set lane takes
// the register's low halfword and the upper halfword of that word is cleared.
TEST(VuMemAddressingConsole, IswDestFieldWritesEveryLane)
{
	VuTestHarness h(0);
	SeedMem(h, false);
	h.SetVi(vi::vi1, 0x1234);
	std::vector<VuOp> prog;
	for (u32 field = 1; field < 16; field++)
		prog.push_back(VuOp{VISW_L(FieldBits(field), vi::vi1, vi::vi0, static_cast<s16>(field)), VNOP_U()});
	prog.push_back(EBitNopPair());
	h.LoadProgram(prog);
	h.Run();

	for (u32 field = 1; field < 16; field++)
	{
		for (u32 lane = 0; lane < 4; lane++)
		{
			// Lane 0 is the x bit, which is bit 3 of the field.
			const bool set = (field & (8u >> lane)) != 0;
			const u32 want = set ? 0x1234u : Seed(field, lane);
			EXPECT_EQ(MemWord(h.JitSnapshot(), field * 16 + lane * 4), want)
				<< "jit ISW field " << field << " lane " << lane;
			EXPECT_EQ(MemWord(h.InterpSnapshot(), field * 16 + lane * 4), want)
				<< "interp ISW field " << field << " lane " << lane;
		}
	}
}

// The step reaches the address whatever the base register is. VI0 being
// hardwired suppresses the write-back, not the decrement, so LQD and SQD off
// vi00 address the quadword below zero -- 1023 on VU1, and on VU0 the index
// 0xffff, whose bit 0x400 leaves VU0's data memory for VU1's register file.
// LQI and SQI read the register before stepping it, so they address quadword
// 0 and always did.
TEST(VuMemAddressingConsole, Vu1SteppingFormsOffVi0)
{
	{
		VuTestHarness h(1);
		SeedMem(h, true);
		h.LoadProgram({
			VuOp{VLQD_L(mask::xyzw, vf::vf1, vi::vi0), VNOP_U()},
			VuOp{VLQI_L(mask::xyzw, vf::vf2, vi::vi0), VNOP_U()},
			EBitNopPair(),
		});
		h.Run();
		for (u32 l = 0; l < 4; l++)
		{
			EXPECT_EQ(h.JitSnapshot().regs.VF[vf::vf1].UL[l], Seed(1023, l)) << "jit LQD lane " << l;
			EXPECT_EQ(h.InterpSnapshot().regs.VF[vf::vf1].UL[l], Seed(1023, l)) << "interp LQD lane " << l;
			EXPECT_EQ(h.JitSnapshot().regs.VF[vf::vf2].UL[l], Seed(0, l)) << "jit LQI lane " << l;
			EXPECT_EQ(h.InterpSnapshot().regs.VF[vf::vf2].UL[l], Seed(0, l)) << "interp LQI lane " << l;
		}
		EXPECT_EQ(h.JitSnapshot().regs.VI[vi::vi0].UL & 0xffffu, 0u);
		EXPECT_EQ(h.InterpSnapshot().regs.VI[vi::vi0].UL & 0xffffu, 0u);
	}

	for (int store = 0; store < 2; store++)
	{
		const u32 want = store ? 0u : 1023u; // SQI slot 0, SQD slot 1023
		VuTestHarness h(1);
		SeedMem(h, true);
		h.SetVfBits(vf::vf1, 0x5EED0001, 0x5EED0002, 0x5EED0003, 0x5EED0004);
		h.LoadProgram({
			VuOp{store ? VSQI_L(mask::xyzw, vf::vf1, vi::vi0) : VSQD_L(mask::xyzw, vf::vf1, vi::vi0), VNOP_U()},
			EBitNopPair(),
		});
		h.Run();
		EXPECT_EQ(ChangedQuads(h.JitSnapshot(), true), std::vector<u32>{want}) << "jit store " << store;
		EXPECT_EQ(ChangedQuads(h.InterpSnapshot(), true), std::vector<u32>{want}) << "interp store " << store;
	}
}

// The same step on VU0. Index 0xffff is not an address in VU0's data memory,
// so the console's SQD leaves all 256 quadwords of it holding the seed; the
// window it goes to instead is what Vu0WindowIntoVu1Registers covers.
TEST(VuMemAddressingConsole, Vu0SteppingFormsOffVi0)
{
	VuTestHarness h(0);
	SeedMem(h, false);
	h.SetVfBits(vf::vf1, 0x5EED0001, 0x5EED0002, 0x5EED0003, 0x5EED0004);
	h.LoadProgram({
		VuOp{VSQD_L(mask::xyzw, vf::vf1, vi::vi0), VNOP_U()},
		VuOp{VLQI_L(mask::xyzw, vf::vf2, vi::vi0), VNOP_U()},
		EBitNopPair(),
	});
	h.Run();
	EXPECT_TRUE(ChangedQuads(h.JitSnapshot(), false).empty()) << "jit SQD reached VU0 data memory";
	EXPECT_TRUE(ChangedQuads(h.InterpSnapshot(), false).empty()) << "interp SQD reached VU0 data memory";
	for (u32 l = 0; l < 4; l++)
	{
		EXPECT_EQ(h.JitSnapshot().regs.VF[vf::vf2].UL[l], Seed(0, l)) << "jit LQI lane " << l;
		EXPECT_EQ(h.InterpSnapshot().regs.VF[vf::vf2].UL[l], Seed(0, l)) << "interp LQI lane " << l;
	}
}

} // namespace recompiler_tests
