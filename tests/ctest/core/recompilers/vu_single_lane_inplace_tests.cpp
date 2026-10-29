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
// Each program runs at mode 1 and at mode 2 and the JIT's post-state is compared between the two,
// with the flag hack off (every FMAC writes STATUS) and on, on both VUs. Besides the four FMAC
// bodies in each operand shape, the programs make the S and Z bits live in the written lane
// (negative and zero results), include status-only writers (an FMAC with no MAC reader followed
// by another flag writer), and put two I-bit immediates in a row so the second reaches a
// single-lane op that must not see the first. With the hack off, Run() also diffs each against
// the interpreter.

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
		case 0:
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
		case 1:
			return {
				UpperOnly(VMULA_U(mask::xyzw, 4, 5)),
				UpperOnly(VMADD_U(m, 6, 1, 2)),
				UpperOnly(VMADDw_U(m, 7, 2, 3)),
				UpperOnly(VMSUB_U(m, 8, 1, 3)),
				UpperOnly(VMSUBx_U(m, 9, 2, 1)),
				UpperOnly(VADD_U(mask::xyzw, 12, 1, 3)),
				EBitNopPair(),
			};
		case 2:
			return {
				UpperOnly(VMULA_U(mask::xyzw, 4, 5)),
				UpperOnly(VMADDAx_U(m, 1, 2)),
				UpperOnly(VMSUBAw_U(m, 2, 3)),
				UpperOnly(VADDAy_U(m, 1, 3)),
				UpperOnly(VMULAz_U(m, 2, 1)),
				UpperOnly(VMADD_U(mask::xyzw, 12, 1, 5)),
				EBitNopPair(),
			};
		default:
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

// MOVE and MR32 with a one-lane field, into a destination that is cached (read by the op before),
// not cached, and the source itself. The written lane takes Fs's same lane (MOVE) or the next one
// up (MR32); the other three keep the destination's words.
TEST(VuSingleLaneInPlace, MoveAndMr32IntoEveryLane)
{
	static const u32 kSrc[4] = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	static const u32 kDst[4] = {0xc1200000u, 0xc1a00000u, 0xc1f00000u, 0xc2200000u};
	for (int vu = 0; vu < 2; vu++)
	{
		for (int lane = 0; lane < 4; lane++)
		{
			const u32 m = mask::x >> lane;
			for (int shape = 0; shape < 3; shape++)
			{
				for (bool mr32 : {false, true})
				{
					SCOPED_TRACE(::testing::Message() << "vu" << vu << " lane " << lane << " shape " << shape
					                                  << (mr32 ? " MR32" : " MOVE"));
					VuTestHarness h(vu);
					h.SetVfBits(1, kSrc[0], kSrc[1], kSrc[2], kSrc[3]);
					h.SetVfBits(2, kDst[0], kDst[1], kDst[2], kDst[3]);
					const u32 fs = 1, ft = (shape == 2) ? 1 : 2;
					std::vector<VuOp> prog;
					if (shape == 0) // cache the destination first
						prog.push_back(UpperOnly(VADD_U(mask::xyzw, 3, 2, 0)));
					prog.push_back(VuOp{mr32 ? VMR32_L(m, ft, fs) : VMOVE_L(m, ft, fs), VNOP_U()});
					prog.push_back(EBitNopPair());
					h.LoadProgram(prog);
					h.Run();
					const u32* before = (ft == 1) ? kSrc : kDst;
					for (int l = 0; l < 4; l++)
					{
						const u32 want = (l == lane) ? (mr32 ? kSrc[(l + 1) & 3] : kSrc[l]) : before[l];
						EXPECT_EQ(h.JitSnapshot().regs.VF[ft].UL[l], want) << "lane " << l;
					}
				}
			}
		}
	}
}

} // namespace recompiler_tests
