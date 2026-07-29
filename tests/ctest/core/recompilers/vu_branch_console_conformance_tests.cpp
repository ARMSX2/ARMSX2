// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU0 branch terminators, integer pipe, MAX/MINI and the clip register against
// a first-party console capture.
//
// These four areas had no oracle at all: every value in them came from the
// manual, from upstream, or from one engine copying the other.  The capture
// behind autocases_vubranch.h settles them.  Each engine is scored against
// hardware, never against the other: on the headline question the two
// legitimately disagree.
//
// What the console established, in the order the tests assert it:
//
//   1. A T-bit (or D-bit) stop on a conditional branch runs the delay slot and
//      then parks at the branch's own outcome: the target when the condition
//      is true, the fall-through when it is false.  The E-bit does the same.
//      One polarity, shared by all three special bits.
//   2. On a non-branch pair the same T-bit stops immediately -- the following
//      pair does not run.  So a branch and its delay slot are atomic with
//      respect to a trace stop.
//   3. VI is 16 bits, arithmetic wraps at 16 bits, CFC2 zero-extends to 32,
//      and the branch comparisons read the register as signed 16-bit.
//   4. A VI write is invisible to a branch in the very next pair and visible
//      from one pair of separation onward.
//   5. MAX/MINI order their operands as sign-magnitude integers with no
//      denormal flush -- a denormal beats +0 -- and leave the MAC flags
//      standing where ADD clears them.
//   6. Every CLIP shifts six bits in, including one in a branch delay slot,
//      and the architecturally-unused destination-mask field changes nothing.
//
// Rows where an engine differs from the console are pinned as DISABLED
// tripwires.

#include <gtest/gtest.h>

#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "autocases_vubranch.h"

#include "VU.h"

#include <cstdio>
#include <string>
#include <vector>

namespace recompiler_tests
{
namespace
{
using console_vubranch::kVuBranchCaseCount;
using console_vubranch::kVuBranchCases;
using console_vubranch::VuBranchCase;

const VuBranchCase& CaseByTag(const char* tag)
{
	for (int i = 0; i < kVuBranchCaseCount; ++i)
	{
		if (std::string(kVuBranchCases[i].tag) == tag)
			return kVuBranchCases[i];
	}
	ADD_FAILURE() << "no console case tagged " << tag;
	return kVuBranchCases[0];
}

// Rebuild the console's pre-state and program, then run both engines.
//
// LoadProgram wants the last pair handed to it to carry the E bit and appends
// the delay-slot NOP itself, so the corpus's trailing NOP is dropped here.
void RunConsoleCase(const VuBranchCase& c, VuTestHarness& h)
{
	vuRegs[0].VI[REG_FBRST].UL = c.fbrst;
	for (u32 i = 0; i < 8; ++i)
		h.SetVi(i + 1, c.vi_seed[i]);
	h.SetVfBits(vu::vf::vf4, c.vf_seed[0], c.vf_seed[1], c.vf_seed[2], c.vf_seed[3]);
	h.SetVfBits(vu::vf::vf5, c.vf_seed[4], c.vf_seed[5], c.vf_seed[6], c.vf_seed[7]);
	h.SetVfBits(vu::vf::vf8, c.vf_seed[8], c.vf_seed[9], c.vf_seed[10], c.vf_seed[11]);
	h.SetVfBits(vu::vf::vf9, c.vf_seed[12], c.vf_seed[13], c.vf_seed[14], c.vf_seed[15]);

	std::vector<vu::VuOp> prog;
	for (u32 k = 0; k + 1 < c.n_words; k += 2)
		prog.push_back(vu::VuOp{c.prog[k], c.prog[k + 1]});
	ASSERT_FALSE(prog.empty());
	prog.pop_back();
	h.LoadProgram(prog);
	h.RunNoDiff();
}

// vi1..vi8 as the console reported them.
u32 ConsoleVi(const VuBranchCase& c, u32 n)
{
	return c.vi[n - 1];
}

} // namespace

// =========================================================================
//  1. The headline: a T-bit stop on a conditional branch
// =========================================================================
//
//  Three implementations disagreed pairwise before this capture:
//    - our arm64 recompiler parks at the branch target on a true condition,
//    - upstream x86 microVU parks at the fall-through on a true condition for
//      the T and D bits (an xInvertCond dating to a 2015 fix whose own message
//      says no T-bit title validated it),
//    - the shared interpreter stops on the branch pair, so its delay slot
//      never runs and its resume PC is the delay slot.
//
//  The console runs the delay slot and parks at the branch's own outcome,
//  which is what the arm64 recompiler does.

TEST(VuBranchConsole, TBitConditionalBranchTakenParksAtTarget)
{
	const VuBranchCase& c = CaseByTag("Q1_TCOND_TAKEN_STOP");
	ASSERT_EQ(c.tpc, 3u) << "capture invariant: the console parked at pair 3";
	ASSERT_EQ(ConsoleVi(c, 2), 0x222u) << "capture invariant: delay slot ran";

	VuTestHarness h(0);
	RunConsoleCase(c, h);

	EXPECT_EQ(h.GetViJit(REG_TPC), c.tpc)
		<< "arm64 recompiler: a taken branch under a T-bit stop must resume at "
		   "the branch TARGET, as the console does";
	EXPECT_EQ(h.GetViJit(vu::vi::vi2), 0x222u)
		<< "arm64 recompiler: the delay slot runs before the stop";
	EXPECT_EQ(h.GetViJit(vu::vi::vi3), 0u) << "fall-through must not run";
	EXPECT_EQ(h.GetViJit(vu::vi::vi4), 0u) << "the target's body must not run";

	// The interpreter agrees on the resume address here but never runs the
	// delay slot -- DISABLED_InterpreterRunsDelaySlotBeforeTraceStop.
	EXPECT_EQ(h.GetViInterp(REG_TPC), c.tpc)
		<< "interpreter: taken-branch resume address matches the console";
}

TEST(VuBranchConsole, TBitConditionalBranchNotTakenParksAtFallthrough)
{
	const VuBranchCase& c = CaseByTag("Q1_TCOND_NOTTAKEN_STOP");
	ASSERT_EQ(c.tpc, 2u);
	ASSERT_EQ(ConsoleVi(c, 2), 0x222u);

	VuTestHarness h(0);
	RunConsoleCase(c, h);

	EXPECT_EQ(h.GetViJit(REG_TPC), c.tpc)
		<< "arm64 recompiler: a not-taken branch under a T-bit stop must "
		   "resume at the fall-through side, as the console does";
	EXPECT_EQ(h.GetViJit(vu::vi::vi2), 0x222u);
	EXPECT_EQ(h.GetViJit(vu::vi::vi3), 0u);
	EXPECT_EQ(h.GetViJit(vu::vi::vi4), 0u);
}

// The console's T-latch is VPU_STAT bit 2 and the D-latch is bit 1; the
// running bit is clear in both cases, so a trace stop really does stop.
TEST(VuBranchConsole, TAndDStopsLatchTheirOwnVpuStatBit)
{
	EXPECT_EQ(CaseByTag("Q1_TCOND_TAKEN_STOP").vpu_stat, 0x4u);
	EXPECT_EQ(CaseByTag("Q1_TCOND_NOTTAKEN_STOP").vpu_stat, 0x4u);
	EXPECT_EQ(CaseByTag("Q1_DCOND_TAKEN_STOP").vpu_stat, 0x2u);
	EXPECT_EQ(CaseByTag("Q1_DCOND_NOTTAKEN_STOP").vpu_stat, 0x2u);
	for (const char* t : {"Q1_TCOND_TAKEN_STOP", "Q1_TCOND_NOTTAKEN_STOP",
	                      "Q1_DCOND_TAKEN_STOP", "Q1_DCOND_NOTTAKEN_STOP"})
	{
		EXPECT_EQ(CaseByTag(t).vpu_stat & 1u, 0u) << t << " must not be running";
	}
}

TEST(VuBranchConsole, DAndEBitShareTheTBitPolarity)
{
	EXPECT_EQ(CaseByTag("Q1_DCOND_TAKEN_STOP").tpc, 3u);
	EXPECT_EQ(CaseByTag("Q1_DCOND_NOTTAKEN_STOP").tpc, 2u);
	EXPECT_EQ(CaseByTag("Q1_ECOND_TAKEN").tpc, 3u);
	EXPECT_EQ(CaseByTag("Q1_ECOND_NOTTAKEN").tpc, 2u);
	for (const char* t : {"Q1_DCOND_TAKEN_STOP", "Q1_DCOND_NOTTAKEN_STOP",
	                      "Q1_ECOND_TAKEN", "Q1_ECOND_NOTTAKEN"})
	{
		EXPECT_EQ(ConsoleVi(CaseByTag(t), 2), 0x222u)
			<< t << ": the delay slot runs under every special bit";
	}
}

TEST(VuBranchConsole, TBitOnPlainPairStopsWithoutRunningTheNextPair)
{
	const VuBranchCase& c = CaseByTag("Q1_TPLAIN_STOP");
	ASSERT_EQ(c.tpc, 1u);
	ASSERT_EQ(ConsoleVi(c, 2), 0u) << "capture invariant: nothing after it ran";

	VuTestHarness h(0);
	RunConsoleCase(c, h);
	EXPECT_EQ(h.GetViJit(vu::vi::vi2), 0u)
		<< "arm64 recompiler: a T-bit stop on a non-branch pair must not run "
		   "the following pair";
	EXPECT_EQ(h.GetViInterp(vu::vi::vi2), 0u)
		<< "interpreter: same";
}

TEST(VuBranchConsole, TBitOnUnconditionalBranchAndJumpParkAtTheTarget)
{
	for (const char* t : {"Q1_TUNCOND_STOP", "Q1_TJR_STOP"})
	{
		const VuBranchCase& c = CaseByTag(t);
		EXPECT_EQ(c.tpc, 3u) << t;
		EXPECT_EQ(ConsoleVi(c, 2), 0x222u) << t << ": delay slot ran";
		EXPECT_EQ(ConsoleVi(c, 4), 0u) << t << ": the target body did not run";

		VuTestHarness h(0);
		RunConsoleCase(c, h);
		EXPECT_EQ(h.GetViJit(REG_TPC), c.tpc) << t << " (arm64 recompiler)";
	}
}

// Liveness. Without these the "nothing ran" rows above would be satisfied just
// as well by a microprogram that never started.
TEST(VuBranchConsole, DisarmedControlsRunToCompletion)
{
	for (const char* t : {"Q1_TCOND_TAKEN_DISARMED", "Q1_TCOND_NOTTAKEN_DISARMED",
	                      "Q1_DCOND_TAKEN_DISARMED", "Q1_DCOND_NOTTAKEN_DISARMED",
	                      "Q1_TUNCOND_DISARMED", "Q1_TJR_DISARMED",
	                      "Q1_TPLAIN_DISARMED"})
	{
		const VuBranchCase& c = CaseByTag(t);
		EXPECT_EQ(c.tpc, 6u) << t << ": must run off the end of the program";
		EXPECT_EQ(c.vpu_stat, 0u) << t << ": no latch, no running bit";
		EXPECT_EQ(ConsoleVi(c, 4), 0x444u) << t << ": must reach the target body";
	}
	// And the not-taken pair must reach the fall-through as well, or the
	// control would not distinguish the two paths.
	EXPECT_EQ(ConsoleVi(CaseByTag("Q1_TCOND_NOTTAKEN_DISARMED"), 3), 0x333u);
	EXPECT_EQ(ConsoleVi(CaseByTag("Q1_TCOND_TAKEN_DISARMED"), 3), 0u);
}

// =========================================================================
//  3. The integer pipe
// =========================================================================

TEST(VuBranchConsole, IntegerRegistersAreSixteenBitsAndCfc2ZeroExtends)
{
	const VuBranchCase& c = CaseByTag("Q3_IWRAP");
	EXPECT_EQ(ConsoleVi(c, 1), 0x0000FFFEu) << "0x7FFF + 0x7FFF";
	EXPECT_EQ(ConsoleVi(c, 2), 0x00000000u) << "0xFFFE + 2 wraps to zero";
	EXPECT_EQ(ConsoleVi(c, 3), 0x00007FFDu) << "0x7FFF three times";
	EXPECT_EQ(ConsoleVi(c, 4), 0x0000FFFFu) << "ISUBIU 0 - 1";
	EXPECT_EQ(ConsoleVi(c, 6), 0x0000FFFCu) << "register-form IADD wraps too";
	// Every readback has a clear upper half: CFC2 zero-extends, it does not
	// sign-extend, even for values whose bit 15 is set.
	for (u32 n : {1u, 2u, 3u, 4u, 6u})
		EXPECT_EQ(ConsoleVi(c, n) >> 16, 0u) << "vi" << n;

	VuTestHarness h(0);
	RunConsoleCase(c, h);
	for (u32 n : {1u, 2u, 3u, 4u, 6u})
	{
		EXPECT_EQ(h.GetViJit(n), ConsoleVi(c, n)) << "arm64 recompiler vi" << n;
		EXPECT_EQ(h.GetViInterp(n), ConsoleVi(c, n)) << "interpreter vi" << n;
	}
}

// The branch comparisons read VI as signed 16-bit: 0x8000 is negative, and
// the four single-operand branches partition consistently at every value.
TEST(VuBranchConsole, BranchComparisonsReadViAsSigned16Bit)
{
	struct Row { const char* tag; bool taken; };
	// vi2 == 0x111 means the not-taken side ran.
	auto Taken = [](const char* tag) {
		return ConsoleVi(CaseByTag(tag), 2) != 0x111u;
	};
	for (u32 v : {0x0000u, 0x0001u, 0x7FFFu, 0x8000u, 0xFFFFu})
	{
		char lt[64], ge[64], gt[64], le[64];
		std::snprintf(lt, sizeof(lt), "Q3_SB_LTZ_%04X", v);
		std::snprintf(ge, sizeof(ge), "Q3_SB_GEZ_%04X", v);
		std::snprintf(gt, sizeof(gt), "Q3_SB_GTZ_%04X", v);
		std::snprintf(le, sizeof(le), "Q3_SB_LEZ_%04X", v);
		const s32 s = static_cast<s16>(static_cast<u16>(v));
		EXPECT_EQ(Taken(lt), s < 0) << "IBLTZ 0x" << std::hex << v;
		EXPECT_EQ(Taken(ge), s >= 0) << "IBGEZ 0x" << std::hex << v;
		EXPECT_EQ(Taken(gt), s > 0) << "IBGTZ 0x" << std::hex << v;
		EXPECT_EQ(Taken(le), s <= 0) << "IBLEZ 0x" << std::hex << v;
	}
	// IBEQ still works either side of the wrap point.
	EXPECT_TRUE(Taken("Q3_SB_EQ_8000_8000"));
	EXPECT_FALSE(Taken("Q3_SB_EQ_8000_0000"));
	EXPECT_TRUE(Taken("Q3_SB_EQ_FFFF_FFFF"));
}

// The corpus's earlier Q3_ISIGN_* rows build their operand in the pair before
// the branch that reads it, so they measure this hazard and not the comparison
// width; they are kept as its witness, superseded by the EE-seeded Q3_SB_*.
TEST(VuBranchConsole, ViWriteIsVisibleToABranchOnePairLater)
{
	auto SawStale = [](const char* tag) {
		return ConsoleVi(CaseByTag(tag), 2) != 0x111u;
	};
	EXPECT_TRUE(SawStale("Q2_IHAZ_D0"))
		<< "a branch in the very next pair still reads the old VI";
	for (const char* t : {"Q2_IHAZ_D1", "Q2_IHAZ_D2", "Q2_IHAZ_D3",
	                      "Q2_IHAZ_D4", "Q2_IHAZ_D5"})
		EXPECT_FALSE(SawStale(t)) << t << ": the write must be visible by now";
}

// =========================================================================
//  6. MAX and MINI
// =========================================================================
//
//  Both engines order the operands as sign-magnitude integers and so let a
//  denormal beat +0.  The FMAC input stage flushes denormal operands to signed
//  zero (the EE overflow capture); the console says MAX/MINI do not share it.

TEST(VuBranchConsole, MaxMiniDoNotFlushDenormalOperands)
{
	const VuBranchCase& c = CaseByTag("Q6_ZERO_DENORM");
	// lane x: fs=+0, ft=denormal.  lane y: the same pair, operands swapped.
	EXPECT_EQ(c.vf6[0], 0x00000001u) << "MAX(+0, denormal) is the DENORMAL";
	EXPECT_EQ(c.vf6[1], 0x00000001u) << "and the same with the operands swapped";
	EXPECT_EQ(c.vf7[0], 0x00000000u) << "MINI(+0, denormal) is +0";
	EXPECT_EQ(c.vf7[1], 0x00000000u);

	VuTestHarness h(0);
	RunConsoleCase(c, h);
	for (int i = 0; i < 4; ++i)
	{
		const char lane = "xyzw"[i];
		EXPECT_EQ(h.GetVfBitsJit(vu::vf::vf6, lane), c.vf6[i])
			<< "arm64 recompiler MAX lane " << lane;
		EXPECT_EQ(h.GetVfBitsJit(vu::vf::vf7, lane), c.vf7[i])
			<< "arm64 recompiler MINI lane " << lane;
		EXPECT_EQ(h.GetVfBitsInterp(vu::vf::vf6, lane), c.vf6[i])
			<< "interpreter MAX lane " << lane;
		EXPECT_EQ(h.GetVfBitsInterp(vu::vf::vf7, lane), c.vf7[i])
			<< "interpreter MINI lane " << lane;
	}
}

TEST(VuBranchConsole, MaxMiniSignedZeroAndOrderIndependence)
{
	const VuBranchCase& c = CaseByTag("Q6_ZERO_DENORM");
	// lane z: fs=+0, ft=-0.  lane w: the same pair, swapped.
	EXPECT_EQ(c.vf6[2], 0x00000000u) << "MAX of the two zeroes is +0";
	EXPECT_EQ(c.vf6[3], 0x00000000u) << "regardless of operand order";
	EXPECT_EQ(c.vf7[2], 0x80000000u) << "MINI of the two zeroes is -0";
	EXPECT_EQ(c.vf7[3], 0x80000000u);
}

TEST(VuBranchConsole, MaxMiniOrderTheWholeDenormalRange)
{
	const VuBranchCase& big = CaseByTag("Q6_BIG_DENORM");
	EXPECT_EQ(big.vf6[0], 0x007FFFFFu) << "MAX(max-denormal, +0)";
	EXPECT_EQ(big.vf6[1], 0x007FFFFFu) << "and swapped";
	EXPECT_EQ(big.vf6[2], 0x00000000u) << "MAX(-denormal, +0) is +0";
	EXPECT_EQ(big.vf6[3], 0x80000000u) << "MAX(-denormal, -0) is -0";

	const VuBranchCase& dd = CaseByTag("Q6_DENORM_DENORM");
	EXPECT_EQ(dd.vf6[0], 0x00400000u) << "ordering holds WITHIN the denormals";
	EXPECT_EQ(dd.vf6[1], 0x00800000u) << "smallest normal beats largest denormal";
	EXPECT_EQ(dd.vf6[2], 0x80000001u) << "and among the negative denormals";
	EXPECT_EQ(dd.vf6[3], 0x00000001u);

	// Liveness: MAX/MINI must actually be selecting, or none of the above
	// distinguishes "correct" from "returns its first operand".
	const VuBranchCase& n = CaseByTag("Q6_NORMAL_CONTROL");
	EXPECT_EQ(n.vf6[0], 0x40000000u);
	EXPECT_EQ(n.vf7[0], 0x3F800000u);
	EXPECT_EQ(n.vf6[2], 0x3F800000u);
	EXPECT_EQ(n.vf7[2], 0xC0000000u);
}

TEST(VuBranchConsole, MaxMiniLeaveMacFlagsStandingButAddClearsThem)
{
	// A multiply that underflows on all four lanes leaves MAC = Z|U per lane.
	const u32 kUnderflowMac = 0x00000F0Fu;
	EXPECT_EQ(CaseByTag("Q6_MACMAX").mac, kUnderflowMac)
		<< "MAX must not overwrite the multiply's MAC flags";
	EXPECT_EQ(CaseByTag("Q6_MACMINI").mac, kUnderflowMac)
		<< "MINI must not either";
	// The negative control. Without it, "MAC still reads 0x0F0F" would be
	// equally consistent with a harness that cannot see MAC change at all.
	EXPECT_EQ(CaseByTag("Q6_MACADD").mac, 0x00000000u)
		<< "ADD is not flag-transparent: it must clear what MAX/MINI keep";

	// The second op ran and produced different values, so transparency is not
	// being read off an op that never ran.
	EXPECT_EQ(CaseByTag("Q6_MACMAX").vf7[0], 0x3F000000u);
	EXPECT_EQ(CaseByTag("Q6_MACMINI").vf7[0], 0x00800000u);
}

// =========================================================================
//  7. The clip register
// =========================================================================

TEST(VuBranchConsole, EveryClipShiftsIncludingInADelaySlot)
{
	// Operands are chosen so the three CLIPs contribute 0x01, 0x04 and 0x10;
	// three shifts therefore read back as 0x001110 and a suppressed middle op
	// would read 0x000050.
	const u32 kThreeShifts = 0x001110u;
	EXPECT_EQ(CaseByTag("Q7_CLIP3").clip, kThreeShifts);
	EXPECT_EQ(CaseByTag("Q7_CLIP3_MASK").clip, kThreeShifts)
		<< "the architecturally-unused destination mask changes nothing";
	EXPECT_EQ(CaseByTag("Q7_CLIP3_DELAY").clip, kThreeShifts)
		<< "a CLIP in a branch delay slot shifts like any other";

	for (const char* t : {"Q7_CLIP3", "Q7_CLIP3_MASK", "Q7_CLIP3_DELAY"})
	{
		const VuBranchCase& c = CaseByTag(t);
		VuTestHarness h(0);
		RunConsoleCase(c, h);
		EXPECT_EQ(h.GetViJit(REG_CLIP_FLAG) & 0xFFFFFFu, c.clip)
			<< t << " (arm64 recompiler)";
		EXPECT_EQ(h.GetViInterp(REG_CLIP_FLAG) & 0xFFFFFFu, c.clip)
			<< t << " (interpreter)";
	}
}

// =========================================================================
//  The score table
// =========================================================================
//
//  Asserts nothing: it prints each engine's per-case score against the
//  console, so a summary can be regenerated rather than retyped.
//
//    ./recompiler_tests --gtest_also_run_disabled_tests \
//        --gtest_filter='VuBranchConsole.DISABLED_ScoreEngines*'

TEST(VuBranchConsole, DISABLED_ScoreEnginesAgainstHardware)
{
	int jit_ok = 0, interp_ok = 0, total = 0;
	std::printf("\n%-28s %-22s %-22s\n", "case", "arm64 JIT", "interpreter");
	for (int i = 0; i < kVuBranchCaseCount; ++i)
	{
		const VuBranchCase& c = kVuBranchCases[i];
		VuTestHarness h(0);
		RunConsoleCase(c, h);

		// Compare on everything the console reported that the harness can see:
		// TPC discriminates the branch cases, the VF and flag registers the
		// arithmetic ones.
		auto Score = [&](bool jit) {
			std::string bad;
			const u32 tpc = jit ? h.GetViJit(REG_TPC) : h.GetViInterp(REG_TPC);
			if (tpc != c.tpc)
				bad += " TPC=" + std::to_string(tpc) + "/" + std::to_string(c.tpc);
			for (u32 n = 1; n <= 8; ++n)
			{
				const u32 v = jit ? h.GetViJit(n) : h.GetViInterp(n);
				if (v != c.vi[n - 1])
					bad += " vi" + std::to_string(n);
			}
			// The probe seeds vf6/vf7 with 0xDEADBEEF; a lane still holding the
			// sentinel is one the program never wrote, and is not scored.
			const bool writes_vf = c.vf6[0] != 0xDEADBEEFu;
			for (int l = 0; writes_vf && l < 4; ++l)
			{
				const char lane = "xyzw"[l];
				const u32 a = jit ? h.GetVfBitsJit(vu::vf::vf6, lane)
				                  : h.GetVfBitsInterp(vu::vf::vf6, lane);
				const u32 b = jit ? h.GetVfBitsJit(vu::vf::vf7, lane)
				                  : h.GetVfBitsInterp(vu::vf::vf7, lane);
				if (a != c.vf6[l])
					bad += std::string(" vf6.") + lane;
				if (b != c.vf7[l])
					bad += std::string(" vf7.") + lane;
			}
			const u32 clip = (jit ? h.GetViJit(REG_CLIP_FLAG)
			                      : h.GetViInterp(REG_CLIP_FLAG)) & 0xFFFFFFu;
			if (clip != c.clip)
				bad += " CLIP";
			const u32 mac = jit ? h.GetViJit(REG_MAC_FLAG) : h.GetViInterp(REG_MAC_FLAG);
			if (mac != c.mac)
			{
				char b[64];
				std::snprintf(b, sizeof(b), " MAC=%08X/%08X", mac, c.mac);
				bad += b;
			}
			return bad;
		};
		const std::string j = Score(true), n = Score(false);
		total++;
		if (j.empty())
			jit_ok++;
		if (n.empty())
			interp_ok++;
		std::printf("%-28s %-22s %-22s\n", c.tag,
		            j.empty() ? "match" : j.c_str(),
		            n.empty() ? "match" : n.c_str());
	}
	std::printf("\narm64 JIT   %d/%d exact against hardware\n", jit_ok, total);
	std::printf("interpreter %d/%d exact against hardware\n", interp_ok, total);
}

// Both engines leave the MAC flags standing across MAX/MINI. Asserted
// engine-internally -- MUL+MAX and MUL+MINI must land on the same MAC, MUL+ADD
// on a different one -- because the console's own MAC word carries the
// underflow-bit divergence pinned below.
TEST(VuBranchConsole, MaxMiniAreFlagTransparentInBothEngines)
{
	auto MacAfter = [](const char* tag, bool jit) {
		const VuBranchCase& c = CaseByTag(tag);
		VuTestHarness h(0);
		RunConsoleCase(c, h);
		return jit ? h.GetViJit(REG_MAC_FLAG) : h.GetViInterp(REG_MAC_FLAG);
	};
	for (bool jit : {true, false})
	{
		const char* who = jit ? "arm64 recompiler" : "interpreter";
		const u32 after_max = MacAfter("Q6_MACMAX", jit);
		const u32 after_mini = MacAfter("Q6_MACMINI", jit);
		const u32 after_add = MacAfter("Q6_MACADD", jit);
		EXPECT_EQ(after_max, after_mini)
			<< who << ": MAX and MINI must both be flag-transparent";
		EXPECT_NE(after_max, after_add)
			<< who << ": ADD must not be flag-transparent, or the test above "
			   "cannot tell transparency from an inert MAC register";
		EXPECT_EQ(after_add, 0u) << who << ": a clean ADD clears the MAC cause";
	}
}

// TRIPWIRE -- known divergence, not a MAX/MINI defect.
//
// Console MAC after `mul.xyzw` of the smallest normal by 0.5 is 0x0F0F: Z and
// U on all four lanes. Both engines report 0x000F -- Z but no U.
//
// The cause is upstream of MAX/MINI entirely. VU_MAC_UPDATE (VUflags.cpp)
// tests `if (f == 0)` first and that branch clears the U bit
// (`& ~(0x1100<<shift)`); only the later `case 0:` denormal branch sets Z|U.
// A product that has already been flushed to zero before the flag logic sees
// it therefore can never raise underflow. Hardware raises U for a result that
// underflowed out of normal operands regardless of the flush.
//
// Same defect class as VuStickyMicroConsoleConformance's
// DISABLED_AllMicroStatusMatchesConsole, which this gives a minimal witness
// for: one multiply, four lanes, all four underflowing from normal operands.
// Enable when the FMAC flag path stops folding flush-to-zero into "the result
// was exactly zero".
TEST(VuBranchConsole, DISABLED_MacUnderflowBitMatchesConsole)
{
	for (const char* tag : {"Q6_MACMAX", "Q6_MACMINI"})
	{
		const VuBranchCase& c = CaseByTag(tag);
		ASSERT_EQ(c.mac, 0x00000F0Fu) << tag << ": capture invariant";
		VuTestHarness h(0);
		RunConsoleCase(c, h);
		EXPECT_EQ(h.GetViJit(REG_MAC_FLAG), c.mac) << tag << " [arm64]";
		EXPECT_EQ(h.GetViInterp(REG_MAC_FLAG), c.mac) << tag << " [interp]";
	}
}

// TRIPWIRE -- the arm64 recompiler does not implement the D bit at all.
//
// microVU hard-codes `doDBitHandling = false` (microVU_Misc-arm64.h), so a
// D-bit stop is compiled away and the microprogram runs to completion: TPC 6
// where the console parks at 3 (taken) or 2 (not taken), with both successors
// executed. The D bit is a debug aid and titles wanting this style of pause
// use the T bit; implementing it would have to produce the T-bit answer.
TEST(VuBranchConsole, DISABLED_DBitStopMatchesConsole)
{
	for (const char* tag : {"Q1_DCOND_TAKEN_STOP", "Q1_DCOND_NOTTAKEN_STOP"})
	{
		const VuBranchCase& c = CaseByTag(tag);
		VuTestHarness h(0);
		RunConsoleCase(c, h);
		EXPECT_EQ(h.GetViJit(REG_TPC), c.tpc) << tag << " [arm64]";
		EXPECT_EQ(h.GetViJit(vu::vi::vi4), c.vi[3]) << tag << " [arm64]";
	}
}

// TRIPWIRE -- the interpreter stops one pair early on a trace stop.
//
// _vu0Exec (VU0microInterp.cpp) latches ebit=1 on the branch pair itself, so
// the delay slot never executes (console: vi2 = 0x222, it does) and on a
// not-taken branch the resume PC rests at the delay slot, pair 1, where the
// console says pair 2. The taken case agrees by coincidence: the epilogue's
// `if (branch) TPC = branchpc` lands on the address the console picks.
TEST(VuBranchConsole, DISABLED_InterpreterRunsDelaySlotBeforeTraceStop)
{
	for (const char* tag : {"Q1_TCOND_TAKEN_STOP", "Q1_TCOND_NOTTAKEN_STOP",
	                        "Q1_TUNCOND_STOP", "Q1_TJR_STOP"})
	{
		const VuBranchCase& c = CaseByTag(tag);
		VuTestHarness h(0);
		RunConsoleCase(c, h);
		EXPECT_EQ(h.GetViInterp(vu::vi::vi2), 0x222u)
			<< tag << ": the delay slot must run before the stop";
		EXPECT_EQ(h.GetViInterp(REG_TPC), c.tpc) << tag;
	}
}

} // namespace recompiler_tests
