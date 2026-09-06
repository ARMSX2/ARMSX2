// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// What the console answers for the corner of the VU load/store address path
// both engines had been guessing at: a stepping form whose base register is
// vi00.
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

} // namespace

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
