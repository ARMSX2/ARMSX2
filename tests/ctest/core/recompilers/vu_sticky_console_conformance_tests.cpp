// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU sticky flags against a first-party console capture.
//
// Earlier VU captures issued `ctc2 $0, $vi16` between every pair of ops, which
// clears the sticky field.  That isolates each op's flags -- but it means
// nothing in them says how the six sticky bits (STATUS 0xFC0) accumulate.
// Every case here deliberately never clears.
//
// PCSX2 contradicts itself on the central question.  For the div unit:
//
//   micro  _vuFDIVflush:  STATUS = (STATUS & 0xFCF) | (statusflag & 0xC30)
//   macro  SYNCFDIV:      STATUS = (STATUS & 0x3CF) | (statusflag & 0x30)
//                                                   | ((statusflag & 0x30) << 6)
//
// The macro line clears sticky D and I (bits 10-11) on every div-unit op and
// rewrites them from the new event.  The micro line keeps them -- but
// `statusflag` never carries sticky bits outside an FSSET, so in practice the
// micro path never sets sticky D or I at all.  Both cannot be right, and the
// console says neither is: the stickies accumulate, in both modes.
//
// What the capture establishes, in the order the cases prove it:
//
//   1. All six sticky bits are monotone.  Only an explicit write clears them
//      (CTC2 in macro mode, FSSET in micro mode).  Proven for Z/S/U/O from the
//      FMAC pipe, for D/I from the div unit, and across the two -- a divide
//      leaves the FMAC's stickies alone and an FMAC leaves the divide's alone.
//   2. A clean divide clears the D/I *cause* pair and keeps the D/I stickies.
//   3. The cause nibble is not a stored bit of the register.  `ctc2 $0` clears
//      the stickies and leaves the cause standing, and the ZSUO cause always
//      equals the OR of the MAC register's four lane nibbles.
//   4. FSSET assigns the sticky field; it does not OR into it.
//
// Reads go through CFC2, the way the console observed them, rather than
// through the VU0 snapshot -- CFC2 is the path under test.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "VU.h"

#include <string>
#include <vector>

#include "autocases_vusticky.h"

using namespace console_vusticky;

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee;

// One VF pair per op, so the block never has to reload operands mid-stream the
// way the console probe did with LQC2.
constexpr u32 kFs[3] = {4, 7, 11};
constexpr u32 kFt[3] = {5, 8, 12};
constexpr u32 kFd = 6;
constexpr u32 kVfOne = 10; // 1.0 in every lane, for the prologue

// GPRs holding the four (STATUS, MAC, Q) triples plus the final CLIP.
constexpr u32 kRStatus[4] = {8, 11, 14, 17};
constexpr u32 kRMac[4] = {9, 12, 15, 18};
constexpr u32 kRQ[4] = {10, 13, 16, 19};
constexpr u32 kRClip = 20;
constexpr u32 kRTmp = 21;

constexpr u32 kStickyMask = 0xFC0u;
constexpr u32 kCauseZsuo = 0x00Fu;
constexpr u32 kCauseDi = 0x030u;

void AppendOp(std::vector<u32>& prog, const VuStickyOp& op, int slot)
{
	const u32 fs = kFs[slot], ft = kFt[slot];
	switch (op.kind)
	{
		case VS_NOP: prog.push_back(NOP); break;
		case VS_MUL: prog.push_back(VMUL_C2(op.mask, kFd, fs, ft)); break;
		case VS_ADD: prog.push_back(VADD_C2(op.mask, kFd, fs, ft)); break;
		case VS_MUL_MASK0: prog.push_back(VMUL_C2(op.mask, kFd, fs, ft)); break;
		case VS_DIV: prog.push_back(VDIV_C2(0, 0, fs, ft)); break;
		case VS_SQRT: prog.push_back(VSQRT_C2(0, ft)); break;
		case VS_RSQRT: prog.push_back(VRSQRT_C2(0, 0, fs, ft)); break;
		case VS_CLIP: prog.push_back(VCLIP_C2(ft, fs)); break;
		case VS_IADD: prog.push_back(VIADD_C2(1, 2, 3)); break;
		case VS_CTC2_ZERO: prog.push_back(CTC2(0, REG_STATUS_FLAG)); break;
		case VS_CTC2_FFF:
			prog.push_back(ORI(kRTmp, 0, 0xFFF));
			prog.push_back(CTC2(kRTmp, REG_STATUS_FLAG));
			break;
	}
}

void AppendRead(std::vector<u32>& prog, int k)
{
	prog.push_back(CFC2(kRStatus[k], REG_STATUS_FLAG));
	prog.push_back(CFC2(kRMac[k], REG_MAC_FLAG));
	prog.push_back(CFC2(kRQ[k], REG_Q));
}

// Builds and runs one case, leaving the harness available for read-back.
// The prologue mirrors the probe's: a clean FMAC and a clean divide settle the
// cause nibble, then CTC2 clears the stickies and CLIP.  Whether an FMAC
// clears the divide's D/I is one of the things under test, so the setup must
// not assume it -- hence both.
void BuildProgram(EeRecTestHarness& h, const VuStickyCase& c)
{
	h.EnableVu0Capture();
	for (int s = 0; s < 3; ++s)
	{
		h.SeedVu0VfBits(kFs[s], c.op[s].fs[0], c.op[s].fs[1], c.op[s].fs[2], c.op[s].fs[3]);
		h.SeedVu0VfBits(kFt[s], c.op[s].ft[0], c.op[s].ft[1], c.op[s].ft[2], c.op[s].ft[3]);
	}
	h.SeedVu0VfBits(kVfOne, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);
	h.SeedVu0VfBits(kFd, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);

	std::vector<u32> prog;
	prog.push_back(VADD_C2(0x8, kFd, kVfOne, kVfOne));
	prog.push_back(VDIV_C2(0, 0, kVfOne, kVfOne));
	prog.push_back(CTC2(0, REG_STATUS_FLAG));
	prog.push_back(CTC2(0, REG_CLIP_FLAG));
	AppendRead(prog, 0);
	for (int s = 0; s < 3; ++s)
	{
		AppendOp(prog, c.op[s], s);
		AppendRead(prog, s + 1);
	}
	prog.push_back(CFC2(kRClip, REG_CLIP_FLAG));
	h.LoadProgram(prog);
}

// A recorded per-engine divergence: this case's read `slot` does not match the
// console on the named engine, and that is the state as of the last capture.
struct Divergence
{
	const char* tag;
	int slot; // 0 = post-prologue, 1..3 after each op
	bool interp;
	bool jit;
	const char* cause;
};

// Recorded from an actual run of both engines, never derived from a rule. Each
// row names its own cause.
//
// The largest group is one defect, seen thirty times: `cop2EmitFlagUpdate`
// (pcsx2/arm64/iCOP2-arm64.cpp) extracts a sign bit (CMLT) and a zero bit
// (FCMEQ) per lane and nothing else, so the arm64 COP2 macro path can never
// raise MAC U or MAC O -- and, because the underflowing product is left as a
// denormal rather than flushed, it does not even raise Z where hardware does.
// `Arm64Cop2MacroFlagExtractionIsSignAndZeroOnly` below states that once, with
// a minimal witness; these rows are its fallout across the sticky cases.
//
// What remains on the interpreter side is a single case, and both engines fail
// it: VRSQRT of -0 raises only D where hardware raises D and I, and slots 2-3
// are that one missing bit carried forward. The interpreter's macro flag-merge
// masks (SYNCMSFLAGS, SYNCFDIV) used to account for eight more rows here and no
// longer do.
constexpr Divergence kMacroStatusDivergences[] = {
	{"VUSTICKY_FMAC_ZSUO_ACCUMULATE", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_FMAC_ZSUO_ACCUMULATE", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_FMAC_ZSUO_ACCUMULATE", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_SURVIVES_SILENT_FMAC", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_SURVIVES_SILENT_FMAC", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_SURVIVES_SILENT_FMAC", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_ONE_OP_ALL_FOUR", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_ONE_OP_ALL_FOUR", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_ONE_OP_ALL_FOUR", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_DIV_KEEPS_FMAC_FLAGS", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_DIV_KEEPS_FMAC_FLAGS", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_DIV_KEEPS_FMAC_FLAGS", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_CTC2_CLEARS_STICKY_NOT_CAUSE", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_CTC2_CLEARS_STICKY_NOT_CAUSE", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_CTC2_CLEARS_STICKY_NOT_CAUSE", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_CTC2_WRITTEN_AND_OP_SET_ALIKE", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_EMPTY_DEST_MASK_SILENT", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_EMPTY_DEST_MASK_SILENT", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_EMPTY_DEST_MASK_SILENT", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_VCLIP_TOUCHES_ONLY_CLIP", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_VCLIP_TOUCHES_ONLY_CLIP", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_VCLIP_TOUCHES_ONLY_CLIP", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_INTEGER_OP_SILENT", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_INTEGER_OP_SILENT", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_INTEGER_OP_SILENT", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_THREE_EVENTS_ONE_ACCUMULATION", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_THREE_EVENTS_ONE_ACCUMULATION", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_THREE_EVENTS_ONE_ACCUMULATION", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_CLEAN_DIV_KEEPS_STICKY_DI", 1, true, true,
	 "vrsqrt of -0 raises only D; hardware raises D and I"},
	{"VUSTICKY_CLEAN_DIV_KEEPS_STICKY_DI", 2, true, true,
	 "fallout of slot 1: the I the interpreter never raised cannot be sticky here"},
	{"VUSTICKY_CLEAN_DIV_KEEPS_STICKY_DI", 3, true, true,
	 "fallout of slot 1: the I the interpreter never raised cannot be sticky here"},
};

// MAC is scored on its own table for the same reason STATUS is: a mask defect
// in the flag merge and a missing lane-flag extraction are different bugs, and
// collapsing them would hide which engine is wrong about what.
constexpr Divergence kMacroMacDivergences[] = {
	{"VUSTICKY_FMAC_ZSUO_ACCUMULATE", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_FMAC_ZSUO_ACCUMULATE", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_FMAC_ZSUO_ACCUMULATE", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_SURVIVES_SILENT_FMAC", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_ONE_OP_ALL_FOUR", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_ONE_OP_ALL_FOUR", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_ONE_OP_ALL_FOUR", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_DIV_KEEPS_FMAC_FLAGS", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_DIV_KEEPS_FMAC_FLAGS", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_DIV_KEEPS_FMAC_FLAGS", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_CTC2_CLEARS_STICKY_NOT_CAUSE", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_CTC2_CLEARS_STICKY_NOT_CAUSE", 2, false, true,
	 "CTC2 to STATUS overwrites the live cause nibble"},
	{"VUSTICKY_CTC2_CLEARS_STICKY_NOT_CAUSE", 3, false, true,
	 "CTC2 to STATUS overwrites the live cause nibble"},
	{"VUSTICKY_CTC2_WRITTEN_AND_OP_SET_ALIKE", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_EMPTY_DEST_MASK_SILENT", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_VCLIP_TOUCHES_ONLY_CLIP", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_VCLIP_TOUCHES_ONLY_CLIP", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_VCLIP_TOUCHES_ONLY_CLIP", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_INTEGER_OP_SILENT", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_INTEGER_OP_SILENT", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_INTEGER_OP_SILENT", 3, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_THREE_EVENTS_ONE_ACCUMULATION", 1, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
	{"VUSTICKY_THREE_EVENTS_ONE_ACCUMULATION", 2, false, true,
	 "arm64 cop2EmitFlagUpdate extracts sign and zero only -- no U/O, no underflow flush"},
};

// Micro mode.  What is left here fails on BOTH engines; the interp-only FSSET
// rows are gone (VU_STAT_UPDATE now ORs the sticky bits in where the flags are
// produced, instead of leaving them to be re-derived from a stale cause nibble
// at flush time -- which was what resurrected a bit FSSET had just cleared).
constexpr Divergence kMicroDivergences[] = {
	{"VUSTICKY_MICRO_FMAC_ZSUO_ACCUMULATE", 3, true, true,
	 "micro FMAC loses U (VU_MAC_UPDATE's ~0x1100 clears U on a flush-to-zero) "
	 "and O (the configured clamp mode saturates below exp 255)"},
	{"VUSTICKY_MICRO_CLEAN_DIV_KEEPS_STICKY_DI", 3, true, true,
	 "vrsqrt of -0 raises only D where hardware raises D and I, so sticky I is "
	 "never set -- both engines read back C00 as 800. Not the sticky-"
	 "accumulation gap (VU_STICKY_DI fixed that); this is the cause bit itself"},
	{"VUSTICKY_MICRO_SURVIVES_SILENT_FMAC", 3, true, true,
	 "micro FMAC loses U on the flush-to-zero underflow, as above"},
};

const Divergence* FindDivergence(const Divergence* table, size_t n, const char* tag, int slot)
{
	for (size_t i = 0; i < n; ++i)
		if (table[i].slot == slot && std::string(tag) == table[i].tag)
			return &table[i];
	return nullptr;
}

#define MACRO_STATUS_DIVERGENCE(tag, slot) \
	FindDivergence(kMacroStatusDivergences, std::size(kMacroStatusDivergences), tag, slot)
#define MACRO_MAC_DIVERGENCE(tag, slot) \
	FindDivergence(kMacroMacDivergences, std::size(kMacroMacDivergences), tag, slot)
#define MICRO_DIVERGENCE(tag, slot) \
	FindDivergence(kMicroDivergences, std::size(kMicroDivergences), tag, slot)

std::vector<vu::VuOp> ProgramPairs(const VuStickyProgram& p)
{
	std::vector<vu::VuOp> pairs;
	for (u32 i = 0; i < p.n_pairs; ++i)
		pairs.push_back(vu::VuOp{p.lower[i], p.upper[i]});
	return pairs;
}

void SeedMicro(VuTestHarness& h, const VuStickyProgram& p)
{
	h.SetVfBits(4, p.seed_fs1[0], p.seed_fs1[1], p.seed_fs1[2], p.seed_fs1[3]);
	h.SetVfBits(5, p.seed_ft1[0], p.seed_ft1[1], p.seed_ft1[2], p.seed_ft1[3]);
	h.SetVfBits(7, p.seed_fs2[0], p.seed_fs2[1], p.seed_fs2[2], p.seed_fs2[3]);
	h.SetVfBits(8, p.seed_ft2[0], p.seed_ft2[1], p.seed_ft2[2], p.seed_ft2[3]);
}

// The ZSUO cause nibble is the OR of the MAC register's four lane nibbles
// (VUflags.cpp VU_MAC_UPDATE: Z = 0x0001<<shift, S = 0x0010<<shift,
// U = 0x0100<<shift, O = 0x1000<<shift, shift 3/2/1/0 for x/y/z/w).
u32 CauseFromMac(u32 mac)
{
	u32 c = 0;
	if (mac & 0x000Fu) c |= 0x1u;
	if (mac & 0x00F0u) c |= 0x2u;
	if (mac & 0x0F00u) c |= 0x4u;
	if (mac & 0xF000u) c |= 0x8u;
	return c;
}
} // namespace

// ---------------------------------------------------------------------------
// Group A -- VU0 macro mode
// ---------------------------------------------------------------------------

TEST(VuStickyConsoleConformance, MacroStatusMatchesConsole)
{
	int checked = 0, diverged = 0;
	for (const VuStickyCase& c : kVuStickyCases)
	{
		EeRecTestHarness h;
		BuildProgram(h, c);
		h.RunJitNoDiff();
		EeRecTestHarness hi;
		BuildProgram(hi, c);
		hi.RunInterpOnly();

		for (int k = 0; k < 4; ++k)
		{
			SCOPED_TRACE(::testing::Message() << c.tag << " read " << k << " -- " << c.rule);
			const Divergence* d = MACRO_STATUS_DIVERGENCE(c.tag, k);
			const u32 want = c.read[k].status;
			const u32 got_i = hi.GetGprInterp(kRStatus[k]);
			const u32 got_j = h.GetGprJit(kRStatus[k]);
			if (d && d->interp)
				EXPECT_NE(got_i, want) << "[interp] recorded divergence has been fixed";
			else
				EXPECT_EQ(got_i, want) << "[interp] STATUS";
			if (d && d->jit)
				EXPECT_NE(got_j, want) << "[jit] recorded divergence has been fixed";
			else
				EXPECT_EQ(got_j, want) << "[jit] STATUS";
			if (d)
				++diverged;
			++checked;
		}
	}
	EXPECT_EQ(checked, static_cast<int>(std::size(kVuStickyCases)) * 4);
	EXPECT_EQ(diverged, static_cast<int>(std::size(kMacroStatusDivergences)));
}

// MAC and CLIP are scored separately from STATUS: a STATUS divergence is about
// the flag-merge masks, and pinning the register values alongside it is what
// says the case executed the arithmetic the console executed.
//
// Q is deliberately NOT scored.  The div-unit saturation value is a function of
// the configured VU clamp mode -- PCSX2 returns "max allowed" 0x7F7FFFFF where
// the console gives 0x7FFFFFFF -- which is a clamp-mode question, not a flag
// one.  The console's Q is carried in the header for whoever wants it.
TEST(VuStickyConsoleConformance, MacroMacClipMatchConsole)
{
	int checked = 0, diverged = 0;
	for (const VuStickyCase& c : kVuStickyCases)
	{
		EeRecTestHarness h;
		BuildProgram(h, c);
		h.RunJitNoDiff();
		EeRecTestHarness hi;
		BuildProgram(hi, c);
		hi.RunInterpOnly();

		for (int k = 0; k < 4; ++k)
		{
			SCOPED_TRACE(::testing::Message() << c.tag << " read " << k);
			const Divergence* d = MACRO_MAC_DIVERGENCE(c.tag, k);
			const u32 want = c.read[k].mac;
			if (d && d->interp)
				EXPECT_NE(hi.GetGprInterp(kRMac[k]), want) << "[interp] MAC divergence fixed";
			else
				EXPECT_EQ(hi.GetGprInterp(kRMac[k]), want) << "[interp] MAC";
			if (d && d->jit)
				EXPECT_NE(h.GetGprJit(kRMac[k]), want) << "[jit] MAC divergence fixed";
			else
				EXPECT_EQ(h.GetGprJit(kRMac[k]), want) << "[jit] MAC";
			if (d)
				++diverged;
			++checked;
		}
		SCOPED_TRACE(::testing::Message() << c.tag << " clip");
		EXPECT_EQ(hi.GetGprInterp(kRClip), c.clip) << "[interp] CLIP";
		EXPECT_EQ(h.GetGprJit(kRClip), c.clip) << "[jit] CLIP";
	}
	EXPECT_EQ(checked, static_cast<int>(std::size(kVuStickyCases)) * 4);
	EXPECT_EQ(diverged, static_cast<int>(std::size(kMacroMacDivergences)));
}

// The class behind thirty of the STATUS rows and every one of the MAC rows,
// stated once with the smallest witness that shows it.
//
// `cop2EmitFlagUpdate` (pcsx2/arm64/iCOP2-arm64.cpp) builds the MAC flag from
// exactly two per-lane predicates -- CMLT for the sign bit and FCMEQ for the
// zero bit -- and then clears the U/O positions outright. So on the arm64 COP2
// macro path:
//
//   * a product that underflows raises neither U nor Z, because the result is
//     left as a denormal instead of being flushed to zero;
//   * a product that overflows raises no O.
//
// The shared interpreter gets both right, and both match the console, so the
// gap is one emitter's, not the model's. Recorded rather than fixed: the fix
// belongs with the emitter and wants its own before/after.
TEST(VuStickyConsoleConformance, Arm64Cop2MacroFlagExtractionIsSignAndZeroOnly)
{
	struct Witness
	{
		const char* what;
		u32 fs, ft;
		u32 console_mac;
	};
	// Straight off the console: read 1 of the two cases that use these operands.
	constexpr Witness kWitnesses[] = {
		{"underflow (2^-126 * 0.5)", 0x00800000u, 0x3F000000u, 0x0808u},
		{"overflow (2^127 * 2^127)", 0x7F000000u, 0x7F000000u, 0x8000u},
	};
	for (const Witness& w : kWitnesses)
	{
		SCOPED_TRACE(w.what);
		const auto build = [&](EeRecTestHarness& h) {
			h.EnableVu0Capture();
			h.SeedVu0VfBits(4, w.fs, w.fs, w.fs, w.fs);
			h.SeedVu0VfBits(5, w.ft, w.ft, w.ft, w.ft);
			h.LoadProgram({
				CTC2(0, REG_STATUS_FLAG),
				VMUL_C2(0x8, 6, 4, 5),
				CFC2(kRMac[0], REG_MAC_FLAG),
			});
		};
		EeRecTestHarness hi;
		build(hi);
		hi.RunInterpOnly();
		EeRecTestHarness hj;
		build(hj);
		hj.RunJitNoDiff();
		EXPECT_EQ(hi.GetGprInterp(kRMac[0]), w.console_mac)
			<< "the interpreter is the control here and must match the console";
		EXPECT_EQ(hj.GetGprJit(kRMac[0]), 0u)
			<< "the arm64 COP2 macro path is recorded as raising no flag at all "
			   "for this operand pair; if it now raises one, retire this test";
	}
}

TEST(VuStickyConsoleConformance, DISABLED_Arm64Cop2MacroExtractsUnderflowAndOverflow)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.SeedVu0VfBits(4, 0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u);
	h.SeedVu0VfBits(5, 0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u);
	h.LoadProgram({
		CTC2(0, REG_STATUS_FLAG),
		VMUL_C2(0x8, 6, 4, 5),
		CFC2(kRMac[0], REG_MAC_FLAG),
	});
	h.RunJitNoDiff();
	EXPECT_EQ(h.GetGprJit(kRMac[0]), 0x0808u) << "MAC Z+U on an underflowing product";
}

// The structural law behind case 8: the ZSUO cause nibble is not stored in
// STATUS, it tracks MAC.  Asserted on the CONSOLE data, so it stands whatever
// the emulator does, and separately on each engine, where it must also hold --
// PCSX2 derives both from the same `macflag`, so an engine that broke it would
// have broken the MAC register too.
TEST(VuStickyConsoleConformance, CauseNibbleTracksMac)
{
	int console_checked = 0, engine_checked = 0;
	for (const VuStickyCase& c : kVuStickyCases)
	{
		for (int k = 0; k < 4; ++k)
		{
			SCOPED_TRACE(::testing::Message() << c.tag << " read " << k);
			EXPECT_EQ(c.read[k].status & kCauseZsuo, CauseFromMac(c.read[k].mac))
				<< "console STATUS cause does not match its MAC";
			++console_checked;
		}
	}
	for (const VuStickyCase& c : kVuStickyCases)
	{
		EeRecTestHarness h;
		BuildProgram(h, c);
		h.RunJitNoDiff();
		for (int k = 0; k < 4; ++k)
		{
			SCOPED_TRACE(::testing::Message() << c.tag << " read " << k << " [jit]");
			EXPECT_EQ(h.GetGprJit(kRStatus[k]) & kCauseZsuo,
			          CauseFromMac(h.GetGprJit(kRMac[k])));
			++engine_checked;
		}
	}
	EXPECT_EQ(console_checked, static_cast<int>(std::size(kVuStickyCases)) * 4);
	EXPECT_EQ(engine_checked, console_checked);
}

// Monotonicity, stated as a property of the capture rather than of any one
// case: outside the two cases that write STATUS explicitly, no op ever clears
// a sticky bit.  This is the assertion that would catch a future capture, or a
// future edit of the case table, that quietly contradicts the finding.
TEST(VuStickyConsoleConformance, StickyFieldIsMonotoneWithoutAWrite)
{
	int transitions = 0, writes = 0;
	for (const VuStickyCase& c : kVuStickyCases)
	{
		for (int k = 0; k < 3; ++k)
		{
			const bool writer = c.op[k].kind == VS_CTC2_ZERO || c.op[k].kind == VS_CTC2_FFF;
			const u32 prev = c.read[k].status & kStickyMask;
			const u32 cur = c.read[k + 1].status & kStickyMask;
			SCOPED_TRACE(::testing::Message() << c.tag << " op" << (k + 1));
			if (writer)
			{
				EXPECT_NE(prev, cur) << "the explicit STATUS write changed nothing";
				++writes;
			}
			else
			{
				EXPECT_EQ(prev & ~cur, 0u) << "a sticky bit was cleared with no write";
				++transitions;
			}
		}
	}
	EXPECT_EQ(transitions + writes, static_cast<int>(std::size(kVuStickyCases)) * 3);
	EXPECT_EQ(writes, 2);
}

// ---------------------------------------------------------------------------
// Group B -- VU0 micro mode
// ---------------------------------------------------------------------------
//
// Scored on the sticky field and the D/I cause only.  The console ran the
// eight programs back to back, so a program with no FMAC of its own inherited
// the previous one's MAC -- and the ZSUO cause tracks MAC, so those four bits
// carry an artifact a harness starting from a clean VU cannot reproduce.  The
// rule itself is pinned by CauseNibbleTracksMac above.

TEST(VuStickyMicroConsoleConformance, MicroStatusMatchesConsole)
{
	int checked = 0, diverged = 0;
	for (const VuStickyProgram& p : kVuStickyPrograms)
	{
		VuTestHarness h(0);
		SeedMicro(h, p);
		h.LoadProgram(ProgramPairs(p));
		h.RunNoDiff();
		ASSERT_TRUE(h.HasTerminated()) << p.tag << " did not reach its E bit";

		const u32 mask = kStickyMask | kCauseDi;
		const u32 want = p.final_status & mask;
		SCOPED_TRACE(::testing::Message() << p.tag << " -- " << p.rule);
		const Divergence* d = MICRO_DIVERGENCE(p.tag, 3);
		const u32 got_i = h.GetViInterp(REG_STATUS_FLAG) & mask;
		const u32 got_j = h.GetViJit(REG_STATUS_FLAG) & mask;
		if (d && d->interp)
			EXPECT_NE(got_i, want) << "[interp] recorded divergence has been fixed";
		else
			EXPECT_EQ(got_i, want) << "[interp] STATUS sticky+DI";
		if (d && d->jit)
			EXPECT_NE(got_j, want) << "[jit] recorded divergence has been fixed";
		else
			EXPECT_EQ(got_j, want) << "[jit] STATUS sticky+DI";
		if (d)
			++diverged;
		++checked;
	}
	EXPECT_EQ(checked, static_cast<int>(std::size(kVuStickyPrograms)));
	EXPECT_EQ(diverged, static_cast<int>(std::size(kMicroDivergences)));
}

// The path control.  Program 0 does no flag work at all -- it exists so that a
// zero anywhere else in group B means "the emulator got the flags wrong", not
// "the microprogram never ran".
TEST(VuStickyMicroConsoleConformance, MicroPathControlRuns)
{
	const VuStickyProgram& p = kVuStickyPrograms[0];
	ASSERT_STREQ(p.tag, "VUSTICKY_MICRO_PATH_CONTROL");
	VuTestHarness h(0);
	SeedMicro(h, p);
	h.LoadProgram(ProgramPairs(p));
	h.RunNoDiff();
	ASSERT_TRUE(h.HasTerminated());
	EXPECT_EQ(p.final_vi01, 0x123u) << "the console's own control did not run";
	EXPECT_EQ(h.GetViInterp(1), 0x123u) << "[interp]";
	EXPECT_EQ(h.GetViJit(1), 0x123u) << "[jit]";
}

// ---------------------------------------------------------------------------
// Tripwires
// ---------------------------------------------------------------------------

TEST(VuStickyConsoleConformance, DISABLED_AllMacroStatusMatchesConsole)
{
	for (const VuStickyCase& c : kVuStickyCases)
	{
		EeRecTestHarness h;
		BuildProgram(h, c);
		h.RunJitNoDiff();
		EeRecTestHarness hi;
		BuildProgram(hi, c);
		hi.RunInterpOnly();
		for (int k = 0; k < 4; ++k)
		{
			SCOPED_TRACE(::testing::Message() << c.tag << " read " << k << " -- " << c.rule);
			EXPECT_EQ(hi.GetGprInterp(kRStatus[k]), c.read[k].status) << "[interp]";
			EXPECT_EQ(h.GetGprJit(kRStatus[k]), c.read[k].status) << "[jit]";
		}
	}
}

TEST(VuStickyMicroConsoleConformance, DISABLED_AllMicroStatusMatchesConsole)
{
	for (const VuStickyProgram& p : kVuStickyPrograms)
	{
		VuTestHarness h(0);
		SeedMicro(h, p);
		h.LoadProgram(ProgramPairs(p));
		h.RunNoDiff();
		const u32 mask = kStickyMask | kCauseDi;
		SCOPED_TRACE(::testing::Message() << p.tag << " -- " << p.rule);
		EXPECT_EQ(h.GetViInterp(REG_STATUS_FLAG) & mask, p.final_status & mask) << "[interp]";
		EXPECT_EQ(h.GetViJit(REG_STATUS_FLAG) & mask, p.final_status & mask) << "[jit]";
	}
}

} // namespace recompiler_tests
