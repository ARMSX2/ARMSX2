// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A single-lane FMAC computes one lane. Below clamp mode 2 microVU computes a
// y, z or w lane in its own place, full width; from mode 2 up it rotates the
// lane (and ACC) to lane 0 and back and uses scalar arithmetic. On operands
// no clamp touches -- small finite numbers -- the two forms have to leave
// everything a later dispatch can see word for word the same: the written
// lane, the registers the op only read, all four MAC and STATUS instances in
// their internal (denormalized) layout, which keeps a per-lane bit position,
// and ACC.
//
// So each program here runs at mode 1 and at mode 2 and the JIT's post-state
// is compared between the two, with the flag hack off (every FMAC writes
// STATUS) and on, on both VUs. The programs cover the four FMAC bodies, each
// broadcast/I/Q/lane-wise operand shape, MAX/MINI, results that are negative
// and zero in the written lane (so the S and Z bits are live), status-only
// writers (an FMAC with no MAC reader followed by another flag writer) and
// two I-bit immediates in a row, the second reaching a single-lane op that
// must not see the first. Run() also diffs each against the interpreter.

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"

#include <gtest/gtest.h>

#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }
inline VuOp WithI(u32 imm, u32 upper) { return IBit(VuOp{VLitI(imm), upper}); }

struct Post
{
	u32 vf[32][4];
	u32 acc[4];
	u32 mac[4], status[4], clip[4];
	u32 vi[32];
};

Post RunAt(int vu, int mode, bool flagHack, const std::vector<VuOp>& prog)
{
	const bool savedHack = EmuConfig.Speedhacks.vuFlagHack;
	EmuConfig.Speedhacks.vuFlagHack = flagHack;
	VuTestHarness h(vu);
	h.SetVuClampMode(mode);
	h.SetVf(1, 1.5f, -2.0f, 3.25f, -4.5f);
	h.SetVf(2, 1.5f, 2.0f, -3.25f, -4.5f);
	h.SetVf(3, -0.75f, 0.5f, 0.25f, 2.0f);
	h.SetVf(4, 10.0f, 20.0f, 30.0f, 40.0f);
	h.SetVf(5, 1.0f, 1.0f, 1.0f, 1.0f);
	h.LoadProgram(prog);
	if (flagHack)
		h.RunNoDiff(); // the interpreter computes every flag; the hack drops some
	else
		h.Run();
	const VURegs& j = h.JitSnapshot().regs;
	Post p{};
	for (int r = 0; r < 32; r++)
		for (int l = 0; l < 4; l++)
			p.vf[r][l] = j.VF[r].UL[l];
	for (int l = 0; l < 4; l++)
	{
		p.acc[l] = j.ACC.UL[l];
		p.mac[l] = j.micro_macflags[l];
		p.status[l] = j.micro_statusflags[l];
		p.clip[l] = j.micro_clipflags[l];
	}
	for (int r = 0; r < 32; r++)
		p.vi[r] = j.VI[r].UL;
	EmuConfig.Speedhacks.vuFlagHack = savedHack;
	return p;
}

void ExpectSame(const Post& a, const Post& b)
{
	for (int r = 0; r < 32; r++)
		for (int l = 0; l < 4; l++)
			EXPECT_EQ(a.vf[r][l], b.vf[r][l]) << "vf" << r << " lane " << l;
	for (int l = 0; l < 4; l++)
	{
		EXPECT_EQ(a.acc[l], b.acc[l]) << "ACC lane " << l;
		EXPECT_EQ(a.mac[l], b.mac[l]) << "MAC instance " << l;
		EXPECT_EQ(a.status[l], b.status[l]) << "STATUS instance " << l;
		EXPECT_EQ(a.clip[l], b.clip[l]) << "CLIP instance " << l;
	}
	for (int r = 0; r < 32; r++)
		EXPECT_EQ(a.vi[r], b.vi[r]) << "vi" << r;
}

std::vector<VuOp> Program(u32 m, int variant)
{
	switch (variant)
	{
		case 0: // FMACa: lane-wise, broadcast, I, then a MAX and a MINI
			return {
				UpperOnly(VADD_U(m, 6, 1, 2)),
				UpperOnly(VSUBz_U(m, 7, 1, 2)),
				UpperOnly(VMULw_U(m, 8, 1, 3)),
				WithI(0x3fc00000u, VADDi_U(m, 9, 2)),
				UpperOnly(VMAX_U(m, 10, 1, 2)),
				UpperOnly(VMINIy_U(m, 11, 1, 3)),
				UpperOnly(VADD_U(mask::xyzw, 12, 1, 3)),
				EBitNopPair(),
			};
		case 1: // FMACc / FMACd against ACC, lane-wise and broadcast
			return {
				UpperOnly(VMULA_U(mask::xyzw, 4, 5)),
				UpperOnly(VMADD_U(m, 6, 1, 2)),
				UpperOnly(VMADDw_U(m, 7, 2, 3)),
				UpperOnly(VMSUB_U(m, 8, 1, 3)),
				UpperOnly(VMSUBx_U(m, 9, 2, 1)),
				UpperOnly(VADD_U(mask::xyzw, 12, 1, 3)),
				EBitNopPair(),
			};
		case 2: // FMACb and the ACC-target FMACa forms
			return {
				UpperOnly(VMULA_U(mask::xyzw, 4, 5)),
				UpperOnly(VMADDAx_U(m, 1, 2)),
				UpperOnly(VMSUBAw_U(m, 2, 3)),
				UpperOnly(VADDAy_U(m, 1, 3)),
				UpperOnly(VMULAz_U(m, 2, 1)),
				UpperOnly(VMADD_U(mask::xyzw, 12, 1, 5)),
				EBitNopPair(),
			};
		default: // two I immediates in a row, each read by a single-lane op
			return {
				WithI(0x40000000u, VMAXi_U(m, 6, 1)),
				WithI(0xc0400000u, VMINIi_U(m, 7, 1)),
				WithI(0x3f000000u, VMULi_U(m, 8, 2)),
				WithI(0xbf800000u, VADDi_U(m, 9, 3)),
				UpperOnly(VADD_U(mask::xyzw, 12, 1, 3)),
				EBitNopPair(),
			};
	}
}

} // namespace

TEST(VuSingleLaneInPlace, MatchesTheRotatedFormWordForWord)
{
	for (int vu = 0; vu < 2; vu++)
	{
		for (bool hack : {false, true})
		{
			for (u32 m : {mask::y, mask::z, mask::w})
			{
				for (int variant = 0; variant < 4; variant++)
				{
					SCOPED_TRACE(::testing::Message() << "vu" << vu << " hack " << hack << " dest "
					                                  << (m >> 21) << " program " << variant);
					const std::vector<VuOp> prog = Program(m, variant);
					ExpectSame(RunAt(vu, 1, hack, prog), RunAt(vu, 2, hack, prog));
				}
			}
		}
	}
}

} // namespace recompiler_tests
