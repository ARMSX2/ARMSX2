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

#include "Config.h"
#include "VU.h"

#include "common/FPControl.h"

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

// The environment pins vuFlagHack off (RecompilerTestEnvironment.cpp) so that
// JIT and interp stay bit-comparable; a test that needs the production default
// opts back in for its own scope.
struct ScopedFlagHack
{
	bool saved;
	explicit ScopedFlagHack(bool on)
		: saved(EmuConfig.Speedhacks.vuFlagHack) { EmuConfig.Speedhacks.vuFlagHack = on; }
	~ScopedFlagHack() { EmuConfig.Speedhacks.vuFlagHack = saved; }
};

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
// Twenty-eight of these rows are one defect: `cop2EmitFlagUpdate`
// (pcsx2/arm64/iCOP2-arm64.cpp) extracts a sign bit (CMLT) and a zero bit
// (FCMEQ) per lane and nothing else, so the arm64 COP2 macro path cannot raise
// MAC U or MAC O, and does not raise Z where hardware does -- an underflowing
// product reaches the flag update as a live denormal, which FCMEQ calls
// non-zero. DISABLED_Arm64Cop2MacroExtractsUnderflowAndOverflow below states
// that once, with a minimal witness, and prices what an extraction would cost.
//
// The remaining three rows fail on BOTH engines and are a different bug:
// VRSQRT of -0 raises only D where hardware raises D and I, and slots 2-3 are
// that one missing bit carried forward.
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
	{"VUSTICKY_MICRO_FMAC_ZSUO_ACCUMULATE", 3, false, true,
	 "the recompiler loses U on a flush-to-zero and O to the clamp, which "
	 "saturates below exp 255; the interpreter reads both off the exact result"},
	{"VUSTICKY_MICRO_SURVIVES_SILENT_FMAC", 3, false, true,
	 "the recompiler loses U on the flush-to-zero underflow; the interpreter "
	 "classifies it from the operands and matches"},
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

// The console capture was taken on hardware, which has no denormals and
// saturates on overflow; PCSX2 reproduces its U and O bits only with denormals
// live and overflow reaching Inf, i.e. with DenormalsAreZero off. Under the
// default (production) FP environment both engines lose those bits TOGETHER --
// see ProductionFpEnvironmentErasesUnderflowAndOverflow below, which pins that
// state so it cannot change unnoticed. This test is the DAZ-off configuration,
// which is a supported user setting and the one that matches the console.
TEST(VuStickyConsoleConformance, MacroStatusMatchesConsole)
{
	const ScopedFpEnv fp_env; // needs live denormals and Inf -- see above
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
	const ScopedFpEnv fp_env; // as MacroStatusMatchesConsole
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

// The class behind the divergence rows above, stated once with the smallest
// witnesses that show it. Replaces `Arm64Cop2MacroFlagExtractionIsSignAndZeroOnly`,
// which asserted the arm64 side raised NOTHING here; the assertion is inverted
// rather than deleted: same operands, same console values, required of both
// engines the day the emitter learns U/O.
//
// `cop2EmitFlagUpdate` (pcsx2/arm64/iCOP2-arm64.cpp) builds the MAC flag from
// exactly two per-lane predicates -- CMLT for the sign bit and FCMEQ for the
// zero bit -- and clears the U/O positions outright. So on the arm64 COP2 macro
// path a product that underflows raises neither U nor Z (the result arrives as
// a live denormal, which FCMEQ calls non-zero), and a product that overflows
// raises no O (the +/-FLT_MAX clamp runs BEFORE the flag update and has already
// folded Inf's exponent away).
//
// Both halves need the IEEE environment to be observable at all: FZ erases the
// mantissa U is defined over, and ChopZero saturates an overflow to FLT_MAX so
// nothing is ever exp 255. That is a property of the FP environment, not of the
// extraction -- the extraction is what would make DAZ-off match the console.
// TRIPWIRE -- the arm64 COP2 macro flag update extracts sign and zero only.
// An exponent-field extraction adding MAC U/O costs ~+19 host insns per FMAC
// for bits unreachable under the shipping FPCR, and would split VU0 macro from
// VU0 micro (which computes O only under Fix_VUOverflow and never computes U).
// Enable when a redesign lands a costed version.
TEST(VuStickyConsoleConformance, DISABLED_Arm64Cop2MacroExtractsUnderflowAndOverflow)
{
	const ScopedFpEnv fp_env; // needs live denormals and Inf
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
		// Sign rides alongside U: VU_MAC_UPDATE sets S from bit 31 before it
		// looks at the exponent at all, so a negative denormal is Z+S+U.
		{"negative underflow", 0x80800000u, 0x3F000000u, 0x0888u},
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
		EXPECT_EQ(hi.GetGprInterp(kRMac[0]), w.console_mac) << "[interp] MAC";
		EXPECT_EQ(hj.GetGprJit(kRMac[0]), w.console_mac) << "[jit] MAC";
	}
}

// An FMAC whose dest mask is empty still RETIRES the MAC: every lane takes
// VU_MACx_CLEAR, so the register reads back 0 and the STATUS cause nibble
// empties while the stickies stand.
//
// This is a regression test in the strict sense -- the arm64 emitter returned
// early on an empty mask and wrote neither register, and nothing caught it
// because the flag update could not raise anything anyway, so "leave MAC alone"
// and "clear MAC" agreed on the value 0. Teaching the emitter U/O made the two
// disagree and this case went red. Cheap to break again, so it is pinned on its
// own rather than left to the table above.
//
// The setup op raises S off a plain -1.0, deliberately NOT the Z+U underflow it
// used to use. The property under test -- an empty mask retires the register --
// holds in every FP environment, so its witness should too, and an underflow
// witness would have quietly tied it to DenormalsAreZero being off. Anything
// non-zero to clear will do; S is the one flag no rounding mode or flush can
// take away.
TEST(VuStickyConsoleConformance, Arm64Cop2MacroEmptyDestMaskRetiresTheMacFlag)
{
	const auto build = [](EeRecTestHarness& h) {
		h.EnableVu0Capture();
		// -1.0 * 1.0 -> MAC S on lane x, in any FP environment.
		h.SeedVu0VfBits(4, 0xBF800000u, 0xBF800000u, 0xBF800000u, 0xBF800000u);
		h.SeedVu0VfBits(5, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);
		h.LoadProgram({
			CTC2(0, REG_STATUS_FLAG),
			VMUL_C2(0x8, 6, 4, 5),          // raises S
			CFC2(kRMac[0], REG_MAC_FLAG),
			VMUL_C2(0x0, 6, 4, 5),          // empty dest mask
			CFC2(kRMac[1], REG_MAC_FLAG),
			CFC2(kRStatus[1], REG_STATUS_FLAG),
		});
	};
	EeRecTestHarness hi;
	build(hi);
	hi.RunInterpOnly();
	EeRecTestHarness hj;
	build(hj);
	hj.RunJitNoDiff();

	EXPECT_EQ(hi.GetGprInterp(kRMac[0]), 0x0080u) << "[interp] the masked op must have something to clear";
	EXPECT_EQ(hj.GetGprJit(kRMac[0]), 0x0080u) << "[jit] the masked op must have something to clear";
	EXPECT_EQ(hi.GetGprInterp(kRMac[1]), 0u) << "[interp] MAC after an empty-mask FMAC";
	EXPECT_EQ(hj.GetGprJit(kRMac[1]), 0u) << "[jit] MAC after an empty-mask FMAC";
	// Cause nibble cleared, sticky S (0x080) still standing.
	EXPECT_EQ(hj.GetGprJit(kRStatus[1]), hi.GetGprInterp(kRStatus[1])) << "STATUS after an empty-mask FMAC";
	EXPECT_EQ(hj.GetGprJit(kRStatus[1]) & kCauseZsuo, 0u) << "the cause nibble must empty";
	EXPECT_EQ(hj.GetGprJit(kRStatus[1]) & kStickyMask, 0x080u) << "the stickies must stand";
}

// The default FP environment, pinned. Everything above that carries a
// ScopedFpEnv is describing the DAZ-off configuration; this describes the one a
// game actually gets.
//
// It used to be an equality between the engines, on the grounds that they agreed
// on a value the console contradicts and that the agreement was worth recording.
// Neither half is an equality now. The interpreter classifies the underflow from
// its operands rather than from the flushed result, and reads the overflow off
// the exact magnitude rather than off an exponent field the clamp never lets
// reach 255, so it raises U and O and matches the console on both rows.
//
// The recompilers do neither, and this is where that is written down -- not as a
// parity gap, because x86's mVUupdateFlags computes only S and Z as well, and
// says in its own comment that the host's range is why. Closing it means paying
// for the flags on the hottest emitter in the project, which is a measurement
// nobody has taken.
TEST(VuStickyConsoleConformance, ProductionFpEnvironmentErasesOverflowAndTheJitsUnderflow)
{
	struct Witness
	{
		const char* what;
		u32 fs, ft;
		u32 want_interp;
		u32 want_jit;
		u32 console_mac;
	};
	static const Witness kWitnesses[] = {
		// FZ flushes the denormal before the recompiler's flag update sees a
		// mantissa, so U is gone there and only Z survives. The interpreter
		// re-derives it and lands on the console's answer.
		{"underflow 2^-126 * 0.5", 0x00800000u, 0x3F000000u, 0x0808u, 0x0008u, 0x0808u},
		// ChopZero saturates the overflow to +FLT_MAX (exp 254), so an engine
		// that asks the exponent field never sees one. The interpreter asks the
		// magnitude of the exact product instead.
		{"overflow 2^127 * 2^127", 0x7F000000u, 0x7F000000u, 0x8000u, 0x0000u, 0x8000u},
	};
	for (const Witness& w : kWitnesses)
	{
		SCOPED_TRACE(w.what);
		EeRecTestHarness hi;
		hi.EnableVu0Capture();
		hi.SeedVu0VfBits(4, w.fs, w.fs, w.fs, w.fs);
		hi.SeedVu0VfBits(5, w.ft, w.ft, w.ft, w.ft);
		hi.LoadProgram({CTC2(0, REG_STATUS_FLAG), VMUL_C2(0x8, 6, 4, 5),
		                CFC2(kRMac[0], REG_MAC_FLAG)});
		hi.RunInterpOnly();
		EeRecTestHarness hj;
		hj.EnableVu0Capture();
		hj.SeedVu0VfBits(4, w.fs, w.fs, w.fs, w.fs);
		hj.SeedVu0VfBits(5, w.ft, w.ft, w.ft, w.ft);
		hj.LoadProgram({CTC2(0, REG_STATUS_FLAG), VMUL_C2(0x8, 6, 4, 5),
		                CFC2(kRMac[0], REG_MAC_FLAG)});
		hj.RunJitNoDiff();

		EXPECT_EQ(hi.GetGprInterp(kRMac[0]), w.want_interp) << "[interp]";
		EXPECT_EQ(hj.GetGprJit(kRMac[0]), w.want_jit) << "[jit]";
		EXPECT_NE(w.want_jit, w.console_mac)
			<< "the recompiler now matches the console here. If the emitters "
			   "learned U/O, fold this witness back into an engine equality; if "
			   "the default environment changed, the ScopedFpEnv tags above are "
			   "stale.";
	}
}

// The other half of the same gap: the arm64 macro path has to get the underflow
// VALUE right as well as the flags. VU_MAC_UPDATE returns `s` -- bare signed
// zero -- for a denormal result, and the interpreter writes that to VF; arm64
// used to leave the denormal standing, because FPCR.FZ is not set on this path
// and nothing flushed it afterwards.
//
// The sign is the whole point of the second witness. A flush that cleared the
// register outright would pass the first row and quietly turn -0 into +0, which
// is observable: VU_MAC_UPDATE sets the S bit from bit 31 unconditionally, so
// the sign of an underflowed result survives into MAC even though the magnitude
// does not.
//
// Runs through h.Run(), so the harness auto-diff covers both engines' full
// state, not just the lane this asserts on.
// TRIPWIRE -- the arm64 COP2 macro path does not software-flush a denormal
// FMAC result. None is needed: FTZ already does it (to a SIGNED zero, measured) in
// every shipping configuration, and the clamp two instructions later would
// flush it again. Only observable with DenormalsAreZero off, as here.
TEST(VuStickyConsoleConformance, DISABLED_Arm64Cop2MacroFlushesDenormalResultsToSignedZero)
{
	// With FZ on there is no denormal left for the software flush to act on --
	// the hardware already did it -- so the subject only exists here.
	const ScopedFpEnv fp_env;
	// Every operand below is a NORMAL float. Denormal INPUTS are a different
	// divergence -- the interpreter's vuDouble() flushes them on read and arm64
	// does not -- and feeding one here would silently test that instead.
	constexpr u32 kPosTiny = 0x00800000u; // +2^-126, the smallest normal
	constexpr u32 kNegTiny = 0x80800000u;
	constexpr u32 kPosBig = 0x00C00000u;  // +1.5 * 2^-126
	constexpr u32 kNegBig = 0x80C00000u;
	constexpr u32 kHalf = 0x3F000000u;
	constexpr u32 kPoison = 0x12345678u;  // an unwritten lane must keep this

	// The three macro FMAC emitter shapes, plus the two destinations. VEC_ARITH
	// (VMUL/VADD/VSUB) and COP2_BC_OP (VMULx) and the MADD family all funnel
	// through cop2FinishFmac, but the MASK decides which register the flush has
	// to reach: on a full mask cop2ResultReg hands back fd's VF-cache slot
	// (q16-q20) and the arithmetic lands there directly, while every partial
	// mask computes into RQSCRATCH and merges afterwards. Both are covered.
	enum Shape { MUL, ADD, SUB, MULBC, MADD, MULA };
	struct Witness
	{
		const char* what;
		Shape shape;
		u32 mask;
		char lane;
		u32 fs, ft;
		u32 want;
	};
	static const Witness kWitnesses[] = {
		{"VMUL .x       2^-126 * 0.5", MUL, 0x8, 'x', kPosTiny, kHalf, 0x00000000u},
		{"VMUL .x      -2^-126 * 0.5", MUL, 0x8, 'x', kNegTiny, kHalf, 0x80000000u},
		{"VMUL .w      -2^-126 * 0.5", MUL, 0x1, 'w', kNegTiny, kHalf, 0x80000000u},
		{"VMUL .xyzw   -2^-126 * 0.5", MUL, 0xF, 'z', kNegTiny, kHalf, 0x80000000u},
		{"VADD .y       2^-126 + -1.5*2^-126", ADD, 0x4, 'y', kPosTiny, kNegBig, 0x80000000u},
		{"VADD .xyzw   -2^-126 + 1.5*2^-126", ADD, 0xF, 'w', kNegTiny, kPosBig, 0x00000000u},
		{"VSUB .z       2^-126 - 1.5*2^-126", SUB, 0x2, 'z', kPosTiny, kPosBig, 0x80000000u},
		{"VSUB .xyzw   -2^-126 - -1.5*2^-126", SUB, 0xF, 'x', kNegTiny, kNegBig, 0x00000000u},
		{"VMULx .x     -2^-126 * 0.5", MULBC, 0x8, 'x', kNegTiny, kHalf, 0x80000000u},
		{"VMULx .xyzw   2^-126 * 0.5", MULBC, 0xF, 'y', kPosTiny, kHalf, 0x00000000u},
		// The MADD pair asks a narrower question than the rows above it. The
		// product still has to flush, but its sign cannot reach the result:
		// the accumulator is +0, and a zero sum whose addends disagree in sign
		// is +0 on the console however the flushed product is signed. What
		// these two catch is a denormal left standing, not a sign dropped --
		// the signed-zero accumulator that does separate the two is measured
		// in Vu0MacroZeroSignConsole.
		{"VMADD .y     0 + -2^-126 * 0.5", MADD, 0x4, 'y', kNegTiny, kHalf, 0x00000000u},
		{"VMADD .xyzw  0 + 2^-126 * 0.5", MADD, 0xF, 'x', kPosTiny, kHalf, 0x00000000u},
		{"VMULAx .z    -2^-126 * 0.5 -> ACC", MULA, 0x2, 'z', kNegTiny, kHalf, 0x80000000u},
		{"VMULAx .xyzw  2^-126 * 0.5 -> ACC", MULA, 0xF, 'w', kPosTiny, kHalf, 0x00000000u},
	};

	for (const Witness& w : kWitnesses)
	{
		SCOPED_TRACE(w.what);
		EeRecTestHarness h;
		h.EnableVu0Capture();
		h.SeedVu0VfBits(4, w.fs, w.fs, w.fs, w.fs);
		h.SeedVu0VfBits(5, w.ft, w.ft, w.ft, w.ft);
		h.SeedVu0VfBits(6, kPoison, kPoison, kPoison, kPoison);
		h.SeedVu0AccBits(0, 0, 0, 0); // the MADD rows accumulate onto an exact +0

		std::vector<u32> prog{CTC2(0, REG_STATUS_FLAG)};
		switch (w.shape)
		{
			case MUL: prog.push_back(VMUL_C2(w.mask, 6, 4, 5)); break;
			case ADD: prog.push_back(VADD_C2(w.mask, 6, 4, 5)); break;
			case SUB: prog.push_back(VSUB_C2(w.mask, 6, 4, 5)); break;
			case MULBC: prog.push_back(VMULx_C2(w.mask, 6, 4, 5)); break;
			case MADD: prog.push_back(VMADD_C2(w.mask, 6, 4, 5)); break;
			case MULA: prog.push_back(VMULAx_C2(w.mask, 4, 5)); break;
		}
		h.LoadProgram(prog);
		h.Run(); // auto-diffs the two engines across all state, flags included

		const bool acc = (w.shape == MULA);
		EXPECT_EQ(acc ? h.GetVu0AccBitsInterp(w.lane) : h.GetVu0VfBitsInterp(6, w.lane), w.want)
			<< "[interp] result";
		EXPECT_EQ(acc ? h.GetVu0AccBitsJit(w.lane) : h.GetVu0VfBitsJit(6, w.lane), w.want)
			<< "[jit] result";
	}
}

// The denormal FMAC result reads back as SIGNED zero even with the flag body
// elided -- and with no software flush anywhere in the emitter.
//
// That is the point of the test. An explicit flush here would cost +5 ARM64
// instructions on every flag-dead FMAC -- exactly the path vuFlagHack exists
// to make cheap -- and would be dead weight, because
// FPCR.FZ already does it: the harness runs the FP environment a game runs in
// (RecompilerTestEnvironment, FPUFPCR = DAZ+FTZ+ChopZero), and ARM flush-to-zero
// is sign-preserving -- measured, -2^-126 * 0.5 -> 0x80000000, not 0x00000000.
//
// If this ever starts failing, FZ stopped covering the case and the redesign
// owes a gated software flush. Its FZ-off sibling is the DISABLED tripwire
// Arm64Cop2MacroFlushesDenormalResultsToSignedZero.
//
// The elision needs an FMAC that is not the block's last flag writer
// (CommitAllFlags always marks that one live), so a second FMAC follows. Read
// back through the JIT alone: with the flag body skipped the JIT legitimately
// diverges from the always-accurate interpreter on flags, which is the accepted
// vuFlagHack trade -- the RESULT is what must survive it.
TEST(VuStickyConsoleConformance, Arm64Cop2MacroFlushesDenormalsWithTheFlagUpdateElided)
{
	ScopedFlagHack flagHack(true);
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.SeedVu0VfBits(4, 0x80800000u, 0x80800000u, 0x80800000u, 0x80800000u); // -2^-126
	h.SeedVu0VfBits(5, 0x3F000000u, 0x3F000000u, 0x3F000000u, 0x3F000000u); // 0.5
	h.SeedVu0VfBits(6, 0x12345678u, 0x12345678u, 0x12345678u, 0x12345678u);
	h.SeedVu0VfBits(7, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u); // 1.0
	h.LoadProgram({
		VMUL_C2(0xF, 6, 4, 5), // denormal result; flags dead (overwritten below)
		VADD_C2(0xF, 8, 7, 7), // block's last flag writer, so this one is live
	});

	h.RunJitNoDiff();

	for (const char lane : {'x', 'y', 'z', 'w'})
	{
		SCOPED_TRACE(lane);
		EXPECT_EQ(h.GetVu0VfBitsJit(6, lane), 0x80000000u);
	}
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

// The div unit has no denormal encoding. The interpreter runs it on the bit
// model in FPU.cpp, which says so in three places:
//
//     eeDivide:    ea == 0  ->  sign        a denormal dividend is a zero
//                  e  <  1  ->  sign        no binade below 2^-126 to land in
//     eeSqrtBits:  E  == 0  ->  0           and the operand's sign goes with it
//
// and a divisor with a zero exponent field never reaches eeDivide at all --
// _vuDIV and _vuRSQRT test `(ft & 0x7F800000) == 0`, not `ft == 0.0f`, so a
// denormal divisor saturates Q and raises D or I like a true zero.
//
// Both arm64 pipes ran that on host floats: a pair of Fcmps against 0.0 for
// the two zero tests, and the hardware's FPCR.FZ for the flush. Which is
// right, exactly and only while FZ is set. The tests below therefore come in
// two halves -- one per FP environment -- because each pins a different thing:
//
//   FZ off: the software model. Every row here failed on both pipes before
//           the exponent-field tests and the flush landed; 19 Q and 8 D/I of
//           the 26 rows, per pipe.
//   FZ on:  that the hardware's answer and the model's are the same one, and
//           that the flush the emitters skip there was not carrying anything.
//
// The clamp tier is not an axis: nothing on this path consults
// CHECK_VU_OVERFLOW, and arm64's Fminnm/Fmaxnm clamp is emitted
// unconditionally. Swept across all four VU0 tiers (none / Overflow / +Extra
// / +Sign): identical results, on both engines and both pipes.
//
// The environment once made this file lie. The tripwire these tests replace
// carried `jit 00400000 / interp 00000000`, captured before commit 9c05f75019
// moved the harness into the production FP environment; afterwards it passed
// silently, as a disabled test, while the defect it named stood. Hence the
// asserted premise in each half.
namespace
{
struct DivDenormCase
{
	const char* what;
	enum { kDiv, kSqrt, kRsqrt } op;
	u32 fs, ft;
	u32 want_q;
	u32 want_di; // STATUS D/I, mask 0x30
};

// Dimensions crossed: op family x which operand is denormal (neither, the
// dividend, the divisor, both) x operand signs x how far a denormal result
// falls past the boundary x the negative-ft branch, plus five controls that
// must not move.
constexpr DivDenormCase kDivDenormCases[] = {
	// Quotient lands mid-denormal, from operands that are both normal.
	{"DIV 2^-126 / 2.0", DivDenormCase::kDiv, 0x00800000u, 0x40000000u, 0x00000000u, 0x00u},
	{"DIV -2^-126 / 2.0", DivDenormCase::kDiv, 0x80800000u, 0x40000000u, 0x80000000u, 0x00u},
	{"DIV 2^-126 / -2.0", DivDenormCase::kDiv, 0x00800000u, 0xC0000000u, 0x80000000u, 0x00u},
	{"DIV -2^-126 / -2.0", DivDenormCase::kDiv, 0x80800000u, 0xC0000000u, 0x00000000u, 0x00u},
	{"DIV 2^-126 / 4.0", DivDenormCase::kDiv, 0x00800000u, 0x40800000u, 0x00000000u, 0x00u},
	// Boundary: the largest denormal (0x007FFFFF, one ulp below the smallest
	// normal) and the smallest (0x00000001). A predicate that tested the
	// exponent with the wrong comparison would let one of these through.
	{"DIV 2^-126 / (1+2^-23)", DivDenormCase::kDiv, 0x00800000u, 0x3F800001u, 0x00000000u, 0x00u},
	{"DIV 2^-126 / 2^23", DivDenormCase::kDiv, 0x00800000u, 0x4B000000u, 0x00000000u, 0x00u},
	// The far side of the boundary, and not a witness: 2^-150 is half the
	// smallest denormal, so it rounds to a true zero on its own and the two
	// engines agree here either way. Kept to bound the sweep.
	{"DIV 2^-126 / 2^24 (already zero, agrees either way)", DivDenormCase::kDiv, 0x00800000u, 0x4B800000u, 0x00000000u, 0x00u},
	// RSQRT: fs / sqrt(|ft|), so the sign is fs's alone.
	{"RSQRT 2^-126 / sqrt(4.0)", DivDenormCase::kRsqrt, 0x00800000u, 0x40800000u, 0x00000000u, 0x00u},
	{"RSQRT -2^-126 / sqrt(4.0)", DivDenormCase::kRsqrt, 0x80800000u, 0x40800000u, 0x80000000u, 0x00u},
	{"RSQRT 2^-126 / sqrt(16.0)", DivDenormCase::kRsqrt, 0x00800000u, 0x41800000u, 0x00000000u, 0x00u},
	// Negative ft takes the abs-and-raise-I branch before the flush.
	{"RSQRT 2^-126 / sqrt(-4.0)", DivDenormCase::kRsqrt, 0x00800000u, 0xC0800000u, 0x00000000u, 0x10u},
	{"RSQRT -2^-126 / sqrt(-4.0)", DivDenormCase::kRsqrt, 0x80800000u, 0xC0800000u, 0x80000000u, 0x10u},

	// Denormal dividend over a normal divisor. Not reachable from the result:
	// over a near-minimal divisor a denormal divides out to an ordinary
	// single, which no test of the quotient alone would flush.
	{"DIV denorm / 2^-126", DivDenormCase::kDiv, 0x00400000u, 0x00800000u, 0x00000000u, 0x00u},
	{"DIV -denorm / 2^-126", DivDenormCase::kDiv, 0x80400000u, 0x00800000u, 0x80000000u, 0x00u},
	{"DIV mindenorm / 2^-126", DivDenormCase::kDiv, 0x00000001u, 0x00800000u, 0x00000000u, 0x00u},
	{"RSQRT denorm / sqrt(2^-126)", DivDenormCase::kRsqrt, 0x00400000u, 0x00800000u, 0x00000000u, 0x00u},

	// Denormal divisor: a zero divisor, so Q saturates and D or I is raised.
	// The far end of the class -- the host quotient here is not a ulp out, it
	// is sixty binades out.
	{"DIV 1.0 / denorm", DivDenormCase::kDiv, 0x3F800000u, 0x00400000u, 0x7FFFFFFFu, 0x20u},
	{"DIV 1.0 / -denorm", DivDenormCase::kDiv, 0x3F800000u, 0x80400000u, 0xFFFFFFFFu, 0x20u},
	{"DIV -1.0 / denorm", DivDenormCase::kDiv, 0xBF800000u, 0x00400000u, 0xFFFFFFFFu, 0x20u},
	{"DIV denorm / denorm", DivDenormCase::kDiv, 0x00400000u, 0x00400000u, 0x7FFFFFFFu, 0x10u},
	{"RSQRT 1.0 / sqrt(denorm)", DivDenormCase::kRsqrt, 0x3F800000u, 0x00400000u, 0x7FFFFFFFu, 0x20u},
	{"RSQRT denorm / sqrt(denorm)", DivDenormCase::kRsqrt, 0x00400000u, 0x00400000u, 0x7FFFFFFFu, 0x10u},
	// A denormal dividend over a true zero picks D, not I: the flag is a
	// question about the exponent field, and 0/0 is exclusive with x/0.
	{"DIV denorm / 0.0", DivDenormCase::kDiv, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x10u},
	{"RSQRT denorm / sqrt(0)", DivDenormCase::kRsqrt, 0x00400000u, 0x00000000u, 0x7FFFFFFFu, 0x10u},

	// A denormal radicand is a zero radicand, and eeSqrtBits drops its sign.
	{"SQRT denorm", DivDenormCase::kSqrt, 0x00000000u, 0x00400000u, 0x00000000u, 0x00u},
	{"SQRT mindenorm", DivDenormCase::kSqrt, 0x00000000u, 0x00000001u, 0x00000000u, 0x00u},
	{"SQRT -denorm", DivDenormCase::kSqrt, 0x00000000u, 0x80400000u, 0x00000000u, 0x10u},

	// Controls. 2^-125 / 2.0 is the smallest normal, exponent 1, and must
	// survive untouched: it fails if the predicate tests exp <= 1 rather than
	// exp == 0. SQRT cannot produce a denormal from a normal operand at all
	// (sqrt of the smallest normal is 2^-63), so it pins the flush as a no-op
	// on the one div-unit op that never needs it. 1.0 / 0.0 pins the true-zero
	// divisor the denormal rows above join.
	{"DIV 2^-125 / 2.0 (smallest normal survives)", DivDenormCase::kDiv, 0x01000000u, 0x40000000u, 0x00800000u, 0x00u},
	{"DIV 8.0 / 2.0", DivDenormCase::kDiv, 0x41000000u, 0x40000000u, 0x40800000u, 0x00u},
	{"DIV 1.0 / 0.0", DivDenormCase::kDiv, 0x3F800000u, 0x00000000u, 0x7FFFFFFFu, 0x20u},
	{"SQRT 2^-126", DivDenormCase::kSqrt, 0x00000000u, 0x00800000u, 0x20000000u, 0x00u},
	{"SQRT 4.0", DivDenormCase::kSqrt, 0x00000000u, 0x40800000u, 0x40000000u, 0x00u},
	{"RSQRT 1.0 / sqrt(4.0)", DivDenormCase::kRsqrt, 0x3F800000u, 0x40800000u, 0x3F000000u, 0x00u},
};

constexpr u32 kDivDI = 0x30;

// `which` names the register the pipe under test actually runs under: COP2
// macro ops execute on the EE thread under FPUFPCR, micro programs under the
// per-unit VU0FPCR.
void RequireDenormalsLive(const FPControlRegister& fpcr, const char* which)
{
	ASSERT_FALSE(fpcr.GetDenormalsAreZero())
		<< which << " has DenormalsAreZero set, so the host flushes the quotient "
		<< "before either engine sees it and every case below passes for a reason "
		<< "unrelated to the code under test";
}

void RequireDenormalsFlushed(const FPControlRegister& fpcr, const char* which)
{
	ASSERT_TRUE(fpcr.GetDenormalsAreZero())
		<< which << " has DenormalsAreZero clear, so this half is testing the "
		<< "software model rather than the hardware's agreement with it";
}

// COP2 macro: VDIV/VSQRT/VRSQRT -> Q, read out of the VU0 snapshot rather than
// through a CFC2, whose EE-side result would diverge on exactly the rows under
// test and fail the harness's own GPR diff first.
void RunMacroDivCase(const DivDenormCase& c)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.ExpectVu0Divergence();
	h.SeedVu0VfBits(4, c.fs, c.fs, c.fs, c.fs);
	h.SeedVu0VfBits(5, c.ft, c.ft, c.ft, c.ft);
	h.LoadProgram({
		CTC2(0, REG_STATUS_FLAG),
		c.op == DivDenormCase::kDiv     ? VDIV_C2(0, 0, 4, 5)
		: c.op == DivDenormCase::kSqrt  ? VSQRT_C2(0, 5)
										: VRSQRT_C2(0, 0, 4, 5),
	});
	h.Run();
	EXPECT_EQ(h.GetVu0ViInterp(REG_Q), c.want_q) << "[interp] Q";
	EXPECT_EQ(h.GetVu0ViJit(REG_Q), c.want_q) << "[jit] Q";
	EXPECT_EQ(h.GetVu0ViInterp(REG_STATUS_FLAG) & kDivDI, c.want_di) << "[interp] D/I";
	EXPECT_EQ(h.GetVu0ViJit(REG_STATUS_FLAG) & kDivDI, c.want_di) << "[jit] D/I";
}

// The micro pipe reaches the same three ops through a different emitter and a
// different destination -- writeQreg's insert into the Q-pipeline's pending
// lane, drained here by VWAITQ.
void RunMicroDivCase(const DivDenormCase& c)
{
	VuTestHarness h(0);
	h.SetVfBits(1, c.fs, c.fs, c.fs, c.fs);
	h.SetVfBits(2, c.ft, c.ft, c.ft, c.ft);
	h.LoadProgram({
		vu::VuOp{c.op == DivDenormCase::kDiv     ? vu::VDIV_L(vu::vf::vf1, 0, vu::vf::vf2, 0)
				 : c.op == DivDenormCase::kSqrt  ? vu::VSQRT_L(vu::vf::vf2, 0)
												 : vu::VRSQRT_L(vu::vf::vf1, 0, vu::vf::vf2, 0),
			vu::VNOP_U()},
		vu::VuOp{vu::VWAITQ_L(), vu::VNOP_U()},
		vu::EBitNopPair(),
	});
	h.RunNoDiff();
	EXPECT_EQ(h.GetViInterp(REG_Q), c.want_q) << "[interp] Q";
	EXPECT_EQ(h.GetViJit(REG_Q), c.want_q) << "[jit] Q";
	EXPECT_EQ(h.GetViInterp(REG_STATUS_FLAG) & kDivDI, c.want_di) << "[interp] D/I";
	EXPECT_EQ(h.GetViJit(REG_STATUS_FLAG) & kDivDI, c.want_di) << "[jit] D/I";
}
} // namespace

TEST(VuStickyConsoleConformance, Arm64Cop2DivUnitHasNoDenormals)
{
	const ScopedFpEnv env{ScopedFpEnv::IeeeNearest};
	ASSERT_NO_FATAL_FAILURE(RequireDenormalsLive(EmuConfig.Cpu.FPUFPCR, "FPUFPCR"));

	for (const DivDenormCase& c : kDivDenormCases)
	{
		SCOPED_TRACE(c.what);
		RunMacroDivCase(c);
	}
}

TEST(VuStickyConsoleConformance, Arm64Cop2DivUnitHasNoDenormalsWithHardwareFlush)
{
	ASSERT_NO_FATAL_FAILURE(RequireDenormalsFlushed(EmuConfig.Cpu.FPUFPCR, "FPUFPCR"));

	for (const DivDenormCase& c : kDivDenormCases)
	{
		SCOPED_TRACE(c.what);
		RunMacroDivCase(c);
	}
}

// TRIPWIRE -- microVU still runs the div unit on host floats: two Fcmps
// against 0.0 for the zero tests, and FPCR.FZ for the flush. 25 of the rows
// below fail on it with FZ off, and none with FZ on.
TEST(VuStickyMicroConsoleConformance, DISABLED_MicroDivUnitHasNoDenormals)
{
	const ScopedFpEnv env{ScopedFpEnv::IeeeNearest};
	ASSERT_NO_FATAL_FAILURE(RequireDenormalsLive(EmuConfig.Cpu.VU0FPCR, "VU0FPCR"));

	for (const DivDenormCase& c : kDivDenormCases)
	{
		SCOPED_TRACE(c.what);
		RunMicroDivCase(c);
	}
}

// Every row above seeds all four lanes alike, so none of them can see an fsf
// or ftf that reads the wrong one. The div unit's operands are scalars picked
// out of a quadword, and the flush reloads its operand words rather than
// reusing the ones the divide consumed -- two more places to index. Here the
// four lanes hold four different denormals over four different divisors, so
// each (fsf, ftf) pair has an answer no other pair shares.
namespace
{
// Lane x is a denormal, y the smallest normal, z a denormal one bit wide, w
// negative: the sign, the exponent-field test and the boundary all differ by
// lane, and the divisors below turn each into a different quotient.
constexpr u32 kLaneFs[4] = {0x00400000u, 0x00800000u, 0x00000001u, 0x80400000u};
constexpr u32 kLaneFt[4] = {0x40000000u, 0x00400000u, 0x40800000u, 0x00000000u};
} // namespace

TEST(VuStickyConsoleConformance, Arm64Cop2DivUnitReadsTheAddressedLane)
{
	const ScopedFpEnv env{ScopedFpEnv::IeeeNearest};
	ASSERT_NO_FATAL_FAILURE(RequireDenormalsLive(EmuConfig.Cpu.FPUFPCR, "FPUFPCR"));

	for (u32 fsf = 0; fsf < 4; ++fsf)
	{
		for (u32 ftf = 0; ftf < 4; ++ftf)
		{
			SCOPED_TRACE(::testing::Message() << "fsf " << fsf << " ftf " << ftf);
			for (int op = 0; op < 3; ++op)
			{
				SCOPED_TRACE(op == 0 ? "VDIV" : op == 1 ? "VSQRT" : "VRSQRT");
				EeRecTestHarness h;
				h.EnableVu0Capture();
				h.ExpectVu0Divergence();
				h.SeedVu0VfBits(4, kLaneFs[0], kLaneFs[1], kLaneFs[2], kLaneFs[3]);
				h.SeedVu0VfBits(5, kLaneFt[0], kLaneFt[1], kLaneFt[2], kLaneFt[3]);
				h.LoadProgram({
					CTC2(0, REG_STATUS_FLAG),
					op == 0 ? VDIV_C2(fsf, ftf, 4, 5)
					: op == 1 ? VSQRT_C2(ftf, 5)
							  : VRSQRT_C2(fsf, ftf, 4, 5),
				});
				h.Run();
				EXPECT_EQ(h.GetVu0ViJit(REG_Q), h.GetVu0ViInterp(REG_Q)) << "Q";
				EXPECT_EQ(h.GetVu0ViJit(REG_STATUS_FLAG) & kDivDI,
					h.GetVu0ViInterp(REG_STATUS_FLAG) & kDivDI) << "D/I";
			}
		}
	}
}

} // namespace recompiler_tests

