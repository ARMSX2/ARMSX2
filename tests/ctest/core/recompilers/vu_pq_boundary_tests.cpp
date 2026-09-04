// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Q/P pipeline state across a BLOCK BOUNDARY — branches and end-of-program.
//
// The behavioural model this suite exists for:
//
// The VU's Q and P scalars are double-buffered. While a DIV/SQRT/RSQRT (Q) or
// an EFU op (P) is in flight, the architectural value a reader sees and the
// slot the producer will land in are two different registers. microVU tracks
// which of the two is "current" per compiled instruction as mVU.q / mVU.p, and
// keeps both live in one host vector — qmmPQ lane 0 = Q, lane 1 = pending_q,
// lane 2 = P, lane 3 = pending_p.
//
// Every compiled block, however, is entered assuming instance #0. So when a
// producer's latency expires mid-block, the instance flips, and any branch out
// of that block must physically swap the two lanes before the jump — otherwise
// the target block reads the stale buffer and the whole program computes with
// the PREVIOUS quotient. That swap (mVUsetupBranch, microVU_Branch-arm64.inl)
// had no test coverage at all: nothing in the suite had ever branched with a
// Q or P value in flight.
//
// The failure mode is silent and plausible: a stale Q is a real float that
// propagates through the rest of the microprogram. So each test seeds the
// pre-branch Q/P with a distinctive sentinel and asserts the ABSOLUTE
// post-branch value, not just JIT-vs-interp agreement — a diff alone would
// also pass if the swap were dropped on both sides, and a sentinel-free test
// would pass if the two buffers happened to hold the same number.
//
// The end-of-program half covers the other boundary: mVUendProgram's
// division-flag transfer, which only runs when the program ends while the
// DIV's flag latency is still outstanding. That is the case the older
// Vu0Qpipe tests deliberately avoid (they drain with VWAITQ first), so it too
// was unreached.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <bit>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }
// I-bit set so the zero lower word is the VI[REG_I] immediate rather than
// decoding as `LQ vf0` — the canonical NOP-pair idiom.
inline VuOp Nop() { return IBit(VuOp{VLitZero(), VNOP_U()}); }

// DIV/SQRT latency is 7 cycles (microVU_Lower-arm64.inl, mVUanalyzeFDIV);
// ESADD's EFU latency is 11. Padding by exactly the latency puts the
// instance flip immediately before the branch, which is the state under test.
// A zero divisor saturates to the console's 0x7FFFFFFF on the interpreter and,
// below vuClampMode 3, to FLT_MAX in the recompiler; VuDivUnitConsole scores it.
constexpr const char* kWhyQCeiling =
	"Q: the recompiler's saturation ceiling is FLT_MAX, the interpreter's is 0x7FFFFFFF";

constexpr int kDivLatency = 7;
constexpr int kEsaddLatency = 11;

// Sentinels: what a dropped lane swap would leave behind. Chosen far from
// every computed result so a stale read is unmistakable in the failure text.
constexpr float kStaleQ = 1000.0f;
constexpr float kStaleP = 500.0f;

void PushNops(std::vector<VuOp>& prog, int count)
{
	for (int i = 0; i < count; i++)
		prog.push_back(Nop());
}

} // namespace

// =========================================================================
//  Q instance across a branch — mVUsetupBranch's lane-0/1 swap
// =========================================================================

TEST(VuPqBoundary, QInstanceSurvivesUnconditionalBranch)
{
	// vf1.x / vf2.x = 6/2 = 3. The quotient lands in the pending buffer, the
	// instance flips as its latency expires, and the branch must carry it into
	// lane 0 for the target block to read.
	VuTestHarness h(0);
	h.SetQ(std::bit_cast<u32>(kStaleQ));
	h.SetVf(vf::vf1, 6.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf2, 2.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf6, 10.0f, 10.0f, 10.0f, 10.0f); // addend on the taken side
	h.SetVf(vf::vf7, -50.0f, -50.0f, -50.0f, -50.0f); // addend on the skipped pair

	std::vector<VuOp> prog;
	prog.push_back(LowerOnly(VDIV_L(vf::vf1, /*fsf=*/0, vf::vf2, /*ftf=*/0)));
	PushNops(prog, kDivLatency);
	prog.push_back(LowerOnly(VB_L(+2))); // branch over the poison pair
	prog.push_back(Nop());               // delay slot
	prog.push_back(UpperOnly(VADDq_U(mask::xyzw, vf::vf5, vf::vf7))); // skipped
	prog.push_back(UpperOnly(VADDq_U(mask::xyzw, vf::vf5, vf::vf6))); // target
	prog.push_back(EBitNopPair());
	h.LoadProgram(std::move(prog));

	h.Run();

	// 10 + 3 == 13. A dropped lane swap reads the seeded Q instead: 10 + 1000.
	// Landing on the skipped pair instead would give -50 + 3.
	EXPECT_FLOAT_EQ(h.GetVfJit(vf::vf5, 'x'), 13.0f)
		<< "target block read the wrong Q buffer across the branch";
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 3.0f);
}

TEST(VuPqBoundary, QInstanceSurvivesConditionalBranchTaken)
{
	VuTestHarness h(0);
	h.SetQ(std::bit_cast<u32>(kStaleQ));
	h.SetVf(vf::vf1, 9.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf2, 4.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf6, 1.0f, 1.0f, 1.0f, 1.0f);
	h.SetVf(vf::vf7, -50.0f, -50.0f, -50.0f, -50.0f);
	h.SetVi(vi::vi1, 0); // IBEQ vi1, vi0 => taken

	std::vector<VuOp> prog;
	prog.push_back(LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 0)));
	PushNops(prog, kDivLatency);
	prog.push_back(LowerOnly(VIBEQ_L(vi::vi1, vi::vi0, +2)));
	prog.push_back(Nop());
	prog.push_back(UpperOnly(VADDq_U(mask::xyzw, vf::vf5, vf::vf7))); // not-taken side
	prog.push_back(UpperOnly(VADDq_U(mask::xyzw, vf::vf5, vf::vf6))); // taken target
	prog.push_back(EBitNopPair());
	h.LoadProgram(std::move(prog));

	h.Run();

	EXPECT_FLOAT_EQ(h.GetVfJit(vf::vf5, 'x'), 3.25f); // 1 + 9/4
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 2.25f);
}

TEST(VuPqBoundary, QInstanceSurvivesConditionalBranchNotTaken)
{
	// The not-taken side of a conditional branch is a separate emit path
	// (condBranch compiles the fall-through inline and patches the taken
	// target), so it needs its own case: the swap must have happened before
	// EITHER successor runs.
	VuTestHarness h(0);
	h.SetQ(std::bit_cast<u32>(kStaleQ));
	h.SetVf(vf::vf1, 9.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf2, 4.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf6, 1.0f, 1.0f, 1.0f, 1.0f);
	h.SetVf(vf::vf7, -50.0f, -50.0f, -50.0f, -50.0f);
	h.SetVi(vi::vi1, 1); // IBEQ vi1, vi0 => not taken

	std::vector<VuOp> prog;
	prog.push_back(LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 0)));
	PushNops(prog, kDivLatency);
	prog.push_back(LowerOnly(VIBEQ_L(vi::vi1, vi::vi0, +4)));
	prog.push_back(Nop());
	prog.push_back(UpperOnly(VADDq_U(mask::xyzw, vf::vf5, vf::vf6))); // not-taken side
	prog.push_back(EBitNopPair());                                    // ends the not-taken path
	prog.push_back(Nop());                                            // its E-bit delay slot
	prog.push_back(UpperOnly(VADDq_U(mask::xyzw, vf::vf5, vf::vf7))); // taken target
	prog.push_back(EBitNopPair());
	h.LoadProgram(std::move(prog));

	h.Run();

	EXPECT_FLOAT_EQ(h.GetVfJit(vf::vf5, 'x'), 3.25f);
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 2.25f);
}

// =========================================================================
//  P instance across a branch — mVUsetupBranch's lane-2/3 swap. VU1 only:
//  VU0 has no EFU, so P never goes in flight there.
// =========================================================================

TEST(VuPqBoundary, PInstanceSurvivesBranchOnVu1)
{
	// ESADD sums the squares of xyz into P: 3² + 4² + 0² = 25.
	VuTestHarness h(1);
	h.SetP(std::bit_cast<u32>(kStaleP));
	h.SetVf(vf::vf1, 3.0f, 4.0f, 0.0f, 99.0f);
	h.SetVf(vf::vf7, -1.0f, -1.0f, -1.0f, -1.0f);

	std::vector<VuOp> prog;
	prog.push_back(LowerOnly(VESADD_L(vf::vf1)));
	PushNops(prog, kEsaddLatency);
	prog.push_back(LowerOnly(VB_L(+2)));
	prog.push_back(Nop());
	prog.push_back(LowerOnly(VMOVE_L(mask::xyzw, vf::vf5, vf::vf7))); // skipped
	prog.push_back(LowerOnly(VMFP_L(mask::xyzw, vf::vf5)));           // target: vf5 = P
	prog.push_back(EBitNopPair());
	h.LoadProgram(std::move(prog));

	h.Run();

	EXPECT_FLOAT_EQ(h.GetVfJit(vf::vf5, 'x'), 25.0f)
		<< "target block read the wrong P buffer across the branch";
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_P)), 25.0f);
}

// =========================================================================
//  End-of-program division-flag transfer
//
//  A DIV raises its invalid/divide-by-zero bits into a side latch, and they
//  reach the architectural STATUS flag only when the instruction 7 cycles
//  downstream commits them. If the program ENDS inside that window there is
//  no such instruction, so mVUendProgram runs the transfer itself. Programs
//  that drain the pipe with VWAITQ first (every other Q test in the suite)
//  never reach it.
//
//  STATUS is opted out of the cross-engine diff here, not because the answer
//  is unknown but because it is already settled the other way round: the
//  console captures show the sticky D/I bits accumulate, which microVU does
//  and the shared interpreter does not (see
//  vu_sticky_console_conformance_tests.cpp, kMicroDivergences). So the JIT is
//  the side worth asserting, and its cause bit is the observable that proves
//  the end-of-program transfer ran at all.
// =========================================================================

TEST(VuPqBoundary, DivByZeroFlagReachesStatusWhenProgramEndsInsideLatency)
{
	// 1.0 / 0.0 — divide-by-zero, STATUS bit 0x20 (D).
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(vf::vf1, 1.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf2, 0.0f, 0.0f, 0.0f, 0.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 0)),
		EBitNopPair(),
	});

	h.RunRequiringDivergence(kWhyQCeiling);

	EXPECT_NE(h.GetViJit(REG_STATUS_FLAG) & 0x20u, 0u)
		<< "mVUendProgram must fold the pending divide-by-zero flag into STATUS "
		   "when the program ends before the FDIV flag latency elapses";
	// The quotient itself still has to be committed on the way out.
	EXPECT_EQ(h.GetViJit(REG_Q), 0x7F7FFFFFu);
}

TEST(VuPqBoundary, DivInvalidFlagReachesStatusWhenProgramEndsInsideLatency)
{
	// 0.0 / 0.0 — invalid operation, STATUS bit 0x10 (I). Distinct latch bit
	// from the case above, so it catches a transfer that hard-codes one of them.
	VuTestHarness h(0);
	h.IgnoreViInDiff(REG_STATUS_FLAG);
	h.SetVf(vf::vf1, 0.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf2, 0.0f, 0.0f, 0.0f, 0.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 0)),
		EBitNopPair(),
	});

	h.RunRequiringDivergence(kWhyQCeiling);

	EXPECT_NE(h.GetViJit(REG_STATUS_FLAG) & 0x10u, 0u)
		<< "mVUendProgram must fold the pending invalid-operation flag into "
		   "STATUS when the program ends before the FDIV flag latency elapses";
	EXPECT_EQ(h.GetViJit(REG_Q), 0x7F7FFFFFu);
}

// =========================================================================
//  Q/P commit on the T-bit end-program path
//
//  A T-bit stop on a branch does not go through the normal end-of-program
//  routine — it has its own variant, with its own copy of the Q/P commit. The
//  copy matters because committing a double-buffered scalar from a host
//  vector means rotating lanes, and the rotate is not an involution: undoing
//  a 4-byte rotate takes a 12-byte one, not another 4. A copy that no test
//  ever runs is exactly where that kind of slip survives.
//
//  Reaching it needs both scalars still in flight AT the branch, so that the
//  end-of-program cycle advance is what retires them and flips the instance —
//  and VU1, since P only exists there.
//
//  Scored one-sided for the reason the other T-bit branch tests are (see
//  vu0_e_d_t_m_bit_tests.cpp): the stop makes the two engines legitimately
//  disagree on delay-slot execution.
// =========================================================================

TEST(VuPqBoundary, TBitStopCommitsBothInFlightScalarsOnVu1)
{
	VuTestHarness h(1);
	vuRegs[1].VI[REG_FBRST].UL = 0x800u; // T-stop for VU1 (FBRST bit 11)
	vuRegs[0].VI[REG_FBRST].UL = 0x800u;
	h.SetQ(std::bit_cast<u32>(kStaleQ));
	h.SetP(std::bit_cast<u32>(kStaleP));
	h.SetVf(vf::vf1, 8.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf2, 2.0f, 0.0f, 0.0f, 0.0f);
	h.SetVf(vf::vf3, 3.0f, 4.0f, 0.0f, 99.0f);
	h.LoadProgram({
		LowerOnly(VDIV_L(vf::vf1, 0, vf::vf2, 0)), // pair 0: Q = 8/2 = 4
		LowerOnly(VESADD_L(vf::vf3)),              // pair 1: P = 9+16+0 = 25
		TBit(LowerOnly(VB_L(+2))),                 // pair 2: T-bit branch, both in flight
		Nop(),                                     // pair 3: delay slot
		Nop(),                                     // pair 4: skipped
		Nop(),                                     // pair 5: branch target
		EBitNopPair(),                             // pair 6
	});

	h.RunInterpOnly();
	h.RunJitPreserveBlockCache();

	EXPECT_EQ((vuRegs[0].VI[REG_VPU_STAT].UL & 0x400u), 0x400u) << "VU1 T-finished bit";
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_Q)), 4.0f)
		<< "T-bit stop must commit the in-flight quotient, not the buffer it "
		   "displaced";
	EXPECT_FLOAT_EQ(std::bit_cast<float>(h.GetViJit(REG_P)), 25.0f)
		<< "T-bit stop must commit the in-flight EFU result, not the buffer it "
		   "displaced";
}

} // namespace recompiler_tests
