// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// SA register / QFSRV / link-branch / COP0-perf conformance against a real
// PS2. The oracle is first-party: a probe was run on a console and streamed
// its results back over ps2link's `host:` channel, and its records were
// reshaped into autocases_sa.h. Nothing in unknownbrackets/ps2autotests
// answers any of the three questions below.
//
// A. SA. Picked because PCSX2 holds THREE different opinions about what
//    happens when sa >= 16:
//      interpreter  MMI.cpp   sa used in full, `{rs:rt} >> (sa*8)`, which is
//                             C++ UB the moment sa*8 reaches 128
//      x86 rec      iMMI.cpp  sa used in full as a byte index into a 32-byte
//                             temp -- a guest-controlled out-of-bounds host
//                             read
//      arm64 rec    iMMI-arm64.cpp  sa masked to 0..15 at consumption
//    and upstream recMTSA masks with &0xf on its constant-propagation path
//    only, so even x86 disagrees with itself depending on whether the operand
//    folded. The console settles it: SA IS FOUR BITS. `mtsa 0x10` leaves SA
//    at 0, `mtsa 0xFFFFFFFF` leaves it at 0xF, `mtsa 0x13` leaves it at 3.
//    Every one of those opinions is downstream of the same root cause -- MTSA
//    storing a value SA cannot hold -- so masking at the write makes all of
//    them unreachable at once, which is what MTSA/recMTSA now do.
//
//    MTSAB and MTSAH need no change: `(rs^imm)&0xF` and `((rs^imm)&7)<<1`
//    reproduce all 118 of their captured rows exactly.
//
// B. Link branches with rs == $31. The assembler REFUSES to encode these
//    ("the source register must not be $31"), which is precisely why no
//    capture in the wild contains them and why both PCSX2 engines have been
//    free to agree with each other. They do: recBranchLink stores the link to
//    GPR 31 and only then evaluates the condition, and R5900 BGEZAL / BLTZAL /
//    BGEZALL / BLTZALL plus IOP psxBGEZAL / psxBLTZAL all call _SetLink(31)
//    first -- six functions, one shape. The console says the OLD $ra wins:
//    `bltzal $ra` with $ra = -1 is TAKEN, and its no-link twin `bltz $ra`
//    agrees on all six probe values. `jalr $31,$31` rides along as a positive
//    control -- the console jumps to the old $ra, which is what the same
//    ordering rule predicts.
//
// C. COP0 perf registers. PCCR's writable mask is 0x800FFBFE -- exactly the
//    layout R5900.h already documents, with pad0 (bit 0), pad1 (bit 10) and
//    the eleven Reserved bits (20-30) reading back as zero. PCR0/PCR1 are
//    fully 32-bit writable, which kills the "31-bit counter" hypothesis.
//    PCSX2 stores all three verbatim.
//
// Divergences are recorded per engine from a real run, never derived from a
// rule, and the always-on tests assert that the clean cases pass AND that the
// recorded ones still diverge. DISABLED_AllSaPerfMatchesConsole is the
// graduation tripwire.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include <cstdio>
#include <string>
#include <vector>

#include "autocases_sa.h"

using namespace console_sa;
using recompiler_tests::EeRecTestHarness;
using recompiler_tests::RecompilerTestEnvironment;
using mips::reg::ra;
using mips::reg::zero;

namespace
{
constexpr u32 kProgramPc = RecompilerTestEnvironment::kProgramPc;

// SA probes.
constexpr u32 kRs = mips::reg::t0;   // MTSA/MTSAB/MTSAH source
constexpr u32 kRd = mips::reg::t1;   // MFSA destination

// QFSRV probes. Two source pairings, because the emulator has two code paths:
// Rs == Rt+1 takes an "adjacent registers" fast path in both recompilers that
// reads the GPR file directly instead of staging through a temp. Hardware has
// no such distinction, so the same console row scores both.
constexpr u32 kQt = mips::reg::t4;         // rt -- low half of the window
constexpr u32 kQsFar = mips::reg::t6;      // rs, NOT adjacent
constexpr u32 kQsAdj = mips::reg::t5;      // rs == rt + 1, the fast path
constexpr u32 kQd = mips::reg::t7;

// Link-branch probes.
constexpr u32 kVal = mips::reg::s0;    // holds the value to force into $ra
constexpr u32 kSave = mips::reg::s1;   // parks the harness's real $ra
constexpr u32 kTaken = mips::reg::s2;
constexpr u32 kDelay = mips::reg::s3;
constexpr u32 kLink = mips::reg::s4;

// Perf probes.
constexpr u32 kPv = mips::reg::t0;
constexpr u32 kPr = mips::reg::t1;

inline u32 Move(u32 rd, u32 rs) { return mips::OR(rd, rs, zero); }

u32 EncodeSaWrite(const SaCase& c)
{
	switch (c.kind)
	{
	case SA_MTSA:  return mips::ee::MTSA(kRs);
	case SA_MTSAB: return mips::ee::MTSAB(kRs, static_cast<s16>(c.imm));
	case SA_MTSAH: return mips::ee::MTSAH(kRs, static_cast<s16>(c.imm));
	}
	return 0;
}

u32 EncodeBranch(int op, s16 off)
{
	switch (op)
	{
	case 0: return mips::BLTZAL(ra, off);
	case 1: return mips::BGEZAL(ra, off);
	case 2: return mips::BLTZALL(ra, off);
	case 3: return mips::BGEZALL(ra, off);
	case 4: return mips::BLTZ(ra, off);
	case 5: return mips::BGEZ(ra, off);
	}
	return 0;
}

// Recorded from a real run, per engine -- never derived from a rule.
struct Divergence { const char* label; bool bad_interp, bad_jit; };

// Both engines link before they compare, so both miss every case where the
// pre-link $ra was negative and the link address (positive) is not. The three
// positive probe values agree either way and are clean, as are all twelve
// no-link bltz/bgez rows -- which is what makes this an ordering defect rather
// than a comparison one. Six functions share the shape: R5900 BLTZAL/BGEZAL/
// BLTZALL/BGEZALL (Interpreter.cpp) and IOP psxBLTZAL/psxBGEZAL
// (R3000AInterpreter.cpp); the JITs inherit it via recBranchLink /
// recBranchLinkLikely. Left as recorded rather than fixed: unlike the MTSA
// mask, the two engines AGREE here, so nothing forces the issue and changing
// branch semantics is a call for a human to make.
constexpr Divergence kSaDivergences[] = {
	{"bltzal $ra=ffffffffffffffff", true, true},
	{"bltzal $ra=8000000000000000", true, true},
	{"bltzal $ra=ffffffff80000000", true, true},
	{"bgezal $ra=ffffffffffffffff", true, true},
	{"bgezal $ra=8000000000000000", true, true},
	{"bgezal $ra=ffffffff80000000", true, true},
	{"bltzall $ra=ffffffffffffffff", true, true},
	{"bltzall $ra=8000000000000000", true, true},
	{"bltzall $ra=ffffffff80000000", true, true},
	{"bgezall $ra=ffffffffffffffff", true, true},
	{"bgezall $ra=8000000000000000", true, true},
	{"bgezall $ra=ffffffff80000000", true, true},

	// COP0.cpp stores PCCR verbatim; hardware drops pad0, pad1 and the eleven
	// Reserved bits. Shared by both engines, so likewise recorded.
	{"pccr write 00000001", true, true},
	{"pccr write 00000400", true, true},
	{"pccr write 00100000", true, true},
	{"pccr write 00200000", true, true},
	{"pccr write 00400000", true, true},
	{"pccr write 00800000", true, true},
	{"pccr write 01000000", true, true},
	{"pccr write 02000000", true, true},
	{"pccr write 04000000", true, true},
	{"pccr write 08000000", true, true},
	{"pccr write 10000000", true, true},
	{"pccr write 20000000", true, true},
	{"pccr write 40000000", true, true},
	{"pccr write ffffffff", true, true},
};

bool IsKnownBad(const Divergence* table, int n, const char* label, bool jit)
{
	for (int i = 0; i < n; ++i)
	{
		if (std::string(label) == table[i].label)
			return jit ? table[i].bad_jit : table[i].bad_interp;
	}
	return false;
}

// ---- runners -------------------------------------------------------------

u64 RunMfsa(const SaCase& c, bool jit)
{
	EeRecTestHarness h;
	h.SetGpr64(kRs, c.rs);
	h.SetGpr64(kRd, kMfsaPreset);
	h.LoadProgram({EncodeSaWrite(c), mips::ee::MFSA(kRd)});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	return jit ? h.GetGpr64Jit(kRd) : h.GetGpr64Interp(kRd);
}

void RunQfsrv(const SaCase& c, bool jit, bool adjacent, u64& lo, u64& hi)
{
	const u32 rs = adjacent ? kQsAdj : kQsFar;
	EeRecTestHarness h;
	h.SetGpr64(kRs, c.rs);
	h.SetGpr128(kQt, kQfsrvRtLo, kQfsrvRtHi);
	h.SetGpr128(rs, kQfsrvRsLo, kQfsrvRsHi);
	h.SetGpr128(kQd, 0, 0);
	h.LoadProgram({EncodeSaWrite(c), mips::ee::QFSRV(kQd, rs, kQt)});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	lo = jit ? h.GetGpr64Jit(kQd) : h.GetGpr64Interp(kQd);
	hi = jit ? h.GetGprUpper64Jit(kQd) : h.GetGprUpper64Interp(kQd);
}

// Reproduces the probe's kernel exactly, including the +3 branch offset that
// skips the delay slot, the `b` and its own delay slot:
//
//   +0  move  kSave, $ra      park the harness's return address
//   +4  move  $ra, kVal       $ra = the probe value
//   +8  ori   kTaken, $0, 0
//   +12 ori   kDelay, $0, 0
//   +16 <branch> $ra, +3      <-- link address is +24
//   +20 addiu kDelay, kDelay, 1    delay slot; nullified by the likely forms
//   +24 beq   $0, $0, +2
//   +28 nop
//   +32 ori   kTaken, $0, 1   <-- taken lands here
//   +36 move  kLink, $ra      whatever $ra ended up holding
//   +40 move  $ra, kSave      restore, so the appended `jr $ra` still parks
constexpr int kBranchIndex = 4;
constexpr u32 kExpectedLink = kProgramPc + 4 * (kBranchIndex + 2);

void RunBranch(const BranchCase& c, bool jit, u32& taken, u32& delay, u64& link)
{
	EeRecTestHarness h;
	h.SetGpr64(kVal, c.ra_in);
	h.SetGpr64(kTaken, 0xDEAD);
	h.SetGpr64(kDelay, 0xDEAD);
	h.SetGpr64(kLink, 0xDEAD);
	h.LoadProgram({
		Move(kSave, ra),
		Move(ra, kVal),
		mips::ORI(kTaken, zero, 0),
		mips::ORI(kDelay, zero, 0),
		EncodeBranch(c.op, 3),
		mips::ADDIU(kDelay, kDelay, 1),
		mips::BEQ(zero, zero, 2),
		mips::NOP,
		mips::ORI(kTaken, zero, 1),
		Move(kLink, ra),
		Move(ra, kSave),
	});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	taken = jit ? h.GetGprJit(kTaken) : h.GetGprInterp(kTaken);
	delay = jit ? h.GetGprJit(kDelay) : h.GetGprInterp(kDelay);
	link = jit ? h.GetGpr64Jit(kLink) : h.GetGpr64Interp(kLink);
}

u32 RunPerf(u32 written, int which, bool jit)
{
	EeRecTestHarness h;
	h.SetGpr64(kPv, written);
	h.SetGpr64(kPr, 0xDEAD);
	std::vector<u32> prog;
	if (which == 0)
	{
		prog.push_back(mips::MTPC(zero, 0));
		prog.push_back(mips::MTPC(zero, 1));
		prog.push_back(mips::MTPS(kPv));
		prog.push_back(mips::MFPS(kPr));
	}
	else
	{
		prog.push_back(mips::MTPS(zero)); // CTE=0 so nothing counts
		prog.push_back(mips::MTPC(kPv, which - 1));
		prog.push_back(mips::MFPC(kPr, which - 1));
	}
	h.LoadProgram(prog);
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	return jit ? h.GetGprJit(kPr) : h.GetGprInterp(kPr);
}
} // namespace

// ---------------------------------------------------------------------------
// A. SA
// ---------------------------------------------------------------------------

// MFSA after every capture row. The engines may legitimately disagree, so
// each is scored against the console independently.
// No known-bads here: before the MTSA mask this failed on 18 of the 30 `mtsa`
// rows on BOTH engines (every rs with a bit set above bit 3).
TEST(EeSaPerfConsoleConformance, MfsaMatchesConsole)
{
	for (int i = 0; i < kSaCaseCount; ++i)
	{
		const SaCase& c = kSaCases[i];
		SCOPED_TRACE(::testing::Message() << c.label << " -- console capture");
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			EXPECT_EQ(RunMfsa(c, jit != 0), c.mfsa);
		}
	}
}

// The QFSRV sources are byte-identity patterns, so each result names its own
// byte shift -- a wrong shift cannot coincidentally produce the right bytes.
TEST(EeSaPerfConsoleConformance, QfsrvMatchesConsole)
{
	for (int i = 0; i < kSaCaseCount; ++i)
	{
		const SaCase& c = kSaCases[i];
		for (int adj = 0; adj < 2; ++adj)
		{
			SCOPED_TRACE(::testing::Message()
			             << c.label << (adj ? " [rs==rt+1 fast path]" : " [general]")
			             << " -- console capture");
			for (int jit = 0; jit < 2; ++jit)
			{
				SCOPED_TRACE(jit ? "[jit]" : "[interp]");
				u64 lo = 0, hi = 0;
				RunQfsrv(c, jit != 0, adj != 0, lo, hi);
				EXPECT_EQ(lo, c.q_lo);
				EXPECT_EQ(hi, c.q_hi);
			}
		}
	}
}

// MTSAB/MTSAH are the rows that needed no change: the console reproduces
// `(rs^imm)&0xF` and `((rs^imm)&7)<<1` exactly. Pinned separately so a future
// "simplification" of the SA path cannot quietly take them with it.
TEST(EeSaPerfConsoleConformance, MtsabMtsahFormulasMatchConsole)
{
	int checked = 0;
	for (int i = 0; i < kSaCaseCount; ++i)
	{
		const SaCase& c = kSaCases[i];
		if (c.kind == SA_MTSA)
			continue;
		const u64 want = (c.kind == SA_MTSAB)
		                     ? (((c.rs & 0xF) ^ (c.imm & 0xF)))
		                     : ((((c.rs & 0x7) ^ (c.imm & 0x7))) << 1);
		SCOPED_TRACE(::testing::Message() << c.label);
		EXPECT_EQ(c.mfsa, want) << "console disagrees with the closed form";
		++checked;
	}
	EXPECT_GT(checked, 100);
}

// SA is four bits wide on hardware, full stop -- no captured row ever left a
// value above 15 in it, including `mtsa 0xFFFFFFFF`.
TEST(EeSaPerfConsoleConformance, SaIsFourBitsOnConsole)
{
	for (int i = 0; i < kSaCaseCount; ++i)
	{
		SCOPED_TRACE(::testing::Message() << kSaCases[i].label);
		EXPECT_LE(kSaCases[i].mfsa, 15u);
	}
}

// ---------------------------------------------------------------------------
// B. link branches with rs == $31
// ---------------------------------------------------------------------------

TEST(EeSaPerfConsoleConformance, LinkBranchesWithRaAsSourceMatchConsole)
{
	int diverged = 0;
	for (int i = 0; i < kBranchCaseCount; ++i)
	{
		const BranchCase& c = kBranchCases[i];
		SCOPED_TRACE(::testing::Message() << c.label << " -- console capture");
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			u32 taken = 0, delay = 0;
			u64 link = 0;
			RunBranch(c, jit != 0, taken, delay, link);

			const u64 want_link = c.links ? kExpectedLink : c.ra_in;
			const bool known = IsKnownBad(
			    kSaDivergences,
			    (int)(sizeof(kSaDivergences) / sizeof(kSaDivergences[0])),
			    c.label, jit != 0);
			if (!known)
			{
				EXPECT_EQ(taken, c.taken ? 1u : 0u);
				EXPECT_EQ(delay, c.delay_ran ? 1u : 0u);
				EXPECT_EQ(link, want_link);
			}
			else if (taken != (c.taken ? 1u : 0u))
				++diverged;

			// The link itself is correct in every case, recorded or not --
			// only the comparison reads it too early.
			EXPECT_EQ(link, want_link);
		}
	}
	// 12 rows x 2 engines. If this drops, the ordering got fixed and the
	// recorded divergences above should go with it.
	EXPECT_EQ(diverged, 24);
}

// The no-link twins are the control: on the console `bltz $ra` and
// `bltzal $ra` agree on every probe value, which is what makes "the link write
// does not feed the comparison" a conclusion rather than an inference.
TEST(EeSaPerfConsoleConformance, LinkBranchesAgreeWithTheirNoLinkTwins)
{
	int pairs = 0;
	for (int i = 0; i < kBranchCaseCount; ++i)
	{
		const BranchCase& li = kBranchCases[i];
		if (!li.links)
			continue;
		const int twin_op = (li.op == 0 || li.op == 2) ? 4 : 5; // bltz / bgez
		for (int j = 0; j < kBranchCaseCount; ++j)
		{
			const BranchCase& tw = kBranchCases[j];
			if (tw.op != twin_op || tw.ra_in != li.ra_in)
				continue;
			SCOPED_TRACE(::testing::Message() << li.label << " vs " << tw.label);
			EXPECT_EQ(li.taken, tw.taken);
			++pairs;
		}
	}
	EXPECT_EQ(pairs, 24);
}

// Positive control on a second CPU for the same ordering rule the IOP's
// psxJALR follows: the EE agrees that `jalr` reads rs before it writes the
// link.
TEST(EeSaPerfConsoleConformance, JalrSelfJumpsToOldRsOnConsole)
{
	EXPECT_EQ(kJalrSelfWentToOldRs, 1);
}

// ---------------------------------------------------------------------------
// C. COP0 perf registers
// ---------------------------------------------------------------------------

TEST(EeSaPerfConsoleConformance, PerfRegisterWriteMasksMatchConsole)
{
	int diverged = 0;
	for (int i = 0; i < kPerfCaseCount; ++i)
	{
		const PerfCase& c = kPerfCases[i];
		char label[64];
		std::snprintf(label, sizeof(label), "pccr write %08x", c.written);
		SCOPED_TRACE(::testing::Message() << label << " -- console capture");
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			const u32 pccr = RunPerf(c.written, 0, jit != 0);
			if (!IsKnownBad(kSaDivergences,
			                (int)(sizeof(kSaDivergences) / sizeof(kSaDivergences[0])),
			                label, jit != 0))
				EXPECT_EQ(pccr, c.pccr);
			else if (pccr != c.pccr)
				++diverged;

			// PCR0/PCR1 take all 32 bits on hardware, and PCSX2 agrees.
			EXPECT_EQ(RunPerf(c.written, 1, jit != 0), c.pcr0);
			EXPECT_EQ(RunPerf(c.written, 2, jit != 0), c.pcr1);
		}
	}
	// 14 rows x 2 engines: bit 0, bit 10, the eleven Reserved bits, and the
	// all-ones write that shows all thirteen at once.
	EXPECT_EQ(diverged, 28);
}

// The mask the capture actually implies, stated once so a regenerated header
// that changed it would fail here rather than silently rewriting the claim.
TEST(EeSaPerfConsoleConformance, PccrMaskIsTheDocumentedLayout)
{
	EXPECT_EQ(kPccrWriteMask, 0x800FFBFEu);
	EXPECT_EQ(kPcr0WriteMask, 0xFFFFFFFFu);
	EXPECT_EQ(kPcr1WriteMask, 0xFFFFFFFFu);
	// bit 0 (pad0), bit 10 (pad1) and bits 20-30 (Reserved) per R5900.h.
	EXPECT_EQ(kPccrWriteMask & 0x00000001u, 0u);
	EXPECT_EQ(kPccrWriteMask & 0x00000400u, 0u);
	EXPECT_EQ(kPccrWriteMask & 0x7FF00000u, 0u);
	EXPECT_EQ(kPccrWriteMask & 0x80000000u, 0x80000000u); // CTE
}

// Cross-engine agreement over all three families. No recorded divergences.
TEST(EeSaPerfConsoleConformance, EnginesAgreeOnEveryCase)
{
	for (int i = 0; i < kSaCaseCount; ++i)
	{
		const SaCase& c = kSaCases[i];
		SCOPED_TRACE(::testing::Message() << "mfsa " << c.label);
		EXPECT_EQ(RunMfsa(c, true), RunMfsa(c, false));
	}
	for (int i = 0; i < kBranchCaseCount; ++i)
	{
		const BranchCase& c = kBranchCases[i];
		SCOPED_TRACE(::testing::Message() << "branch " << c.label);
		u32 jt = 0, jd = 0, nt = 0, nd = 0;
		u64 jl = 0, nl = 0;
		RunBranch(c, true, jt, jd, jl);
		RunBranch(c, false, nt, nd, nl);
		EXPECT_EQ(jt, nt) << "engines disagree on whether the branch was taken";
		EXPECT_EQ(jd, nd) << "engines disagree on whether the delay slot ran";
		EXPECT_EQ(jl, nl) << "engines disagree on the link value";
	}
	for (int i = 0; i < kPerfCaseCount; ++i)
	{
		const PerfCase& c = kPerfCases[i];
		for (int which = 0; which < 3; ++which)
		{
			SCOPED_TRACE(::testing::Message()
			             << "pccr " << c.written << " reg " << which);
			EXPECT_EQ(RunPerf(c.written, which, true),
			          RunPerf(c.written, which, false));
		}
	}
}

// Graduation tripwire: flip this on once the recorded divergences are fixed.
TEST(EeSaPerfConsoleConformance, DISABLED_AllSaPerfMatchesConsole)
{
	for (int i = 0; i < kSaCaseCount; ++i)
	{
		const SaCase& c = kSaCases[i];
		SCOPED_TRACE(::testing::Message() << c.label);
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			EXPECT_EQ(RunMfsa(c, jit != 0), c.mfsa);
		}
	}
	for (int i = 0; i < kBranchCaseCount; ++i)
	{
		const BranchCase& c = kBranchCases[i];
		SCOPED_TRACE(::testing::Message() << c.label);
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			u32 taken = 0, delay = 0;
			u64 link = 0;
			RunBranch(c, jit != 0, taken, delay, link);
			EXPECT_EQ(taken, c.taken ? 1u : 0u);
			EXPECT_EQ(delay, c.delay_ran ? 1u : 0u);
			EXPECT_EQ(link, c.links ? kExpectedLink : c.ra_in);
		}
	}
	for (int i = 0; i < kPerfCaseCount; ++i)
	{
		const PerfCase& c = kPerfCases[i];
		SCOPED_TRACE(::testing::Message() << "pccr " << c.written);
		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			EXPECT_EQ(RunPerf(c.written, 0, jit != 0), c.pccr);
		}
	}
}
