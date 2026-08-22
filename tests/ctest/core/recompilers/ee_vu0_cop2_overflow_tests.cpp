// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The COP2 macro FMAC's MAC O bit, differentially against the interpreter.
//
// vu0_macro_fmac_range_console_tests.cpp holds the console rows the model was
// built from -- eight operand pairs across three opcodes, including both sides
// of the threshold to one ULP. This is the other half of the coverage: every
// emitter in the tree that overflows, over random operands drawn from the top
// of the range, every dest mask and every register aliasing, with the
// interpreter's exact model as the reference.
//
// What the VU calls an overflow is a result at or past 2^129, a binade above
// anything single precision holds, and the operands reach that far too. The O
// positions carry the saturated word with them at vuClampMode 3, but the rows
// that do not saturate can still be a binade short -- an exponent-255 operand
// is a NaN to every host op the arithmetic runs through. The flags are
// compared in two steps because of that: the O positions on every row, and the
// whole of MAC and STATUS on the rows where the two engines wrote the same
// words, which is what says the O nibble did not arrive by pushing some other
// bit out of place.
//
// Both polarities: the same stream at vuClampMode 1 has to lose O on the rows
// the model gets right, or the pass says nothing about the gate.
//
// MAC O and the ceiling are at vuClampMode 4, so the sweep runs there; the
// multiply's MAC U is one mode down, where
// ee_vu0_cop2_mul_underflow_tests.cpp scores it.
//
// MADD and MSUB are deliberately absent. Their accumulate reads a product the
// host has already saturated, so no test of the addends can reproduce what the
// console does with the unsaturated one; ExcludedFormsRaiseNoOverflow below
// pins that they raise nothing rather than something wrong.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <random>
#include <vector>

namespace recompiler_tests
{
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFs = 1, kFt = 2, kFd = 4;

// MAC O nibble, and STATUS current O (bit 3) with its sticky OS (bit 9).
constexpr u32 kMacO = 0xF000u;
constexpr u32 kStatO = 0x0208u;
// The multiply's U rides the same gate, so a mode-1 comparison has to allow it.
constexpr u32 kMacU = 0x0F00u;
constexpr u32 kStatU = 0x0104u;

// One overflowing emitter, and how to encode it. `adder` marks the forms that
// take their O from the sum rather than from a product: the two differ in what
// vuClampMode 3 does to the VALUE as well, since the adder also gains its guard
// mask there.
struct Form
{
	const char* name;
	bool adder;
	u32 (*encode)(u32 mask, u32 fd, u32 fs, u32 ft);
};

u32 EncMul   (u32 m, u32 fd, u32 fs, u32 ft) { return VMUL_C2   (m, fd, fs, ft); }
u32 EncMulX  (u32 m, u32 fd, u32 fs, u32 ft) { return VMULx_C2  (m, fd, fs, ft); }
u32 EncMulW  (u32 m, u32 fd, u32 fs, u32 ft) { return VMULw_C2  (m, fd, fs, ft); }
u32 EncMulQ  (u32 m, u32 fd, u32 fs, u32   ) { return VMULq_C2  (m, fd, fs); }
u32 EncMulI  (u32 m, u32 fd, u32 fs, u32   ) { return VMULi_C2  (m, fd, fs); }
u32 EncMulA  (u32 m, u32   , u32 fs, u32 ft) { return VMULA_C2  (m, fs, ft); }
u32 EncMulAY (u32 m, u32   , u32 fs, u32 ft) { return VMULAy_C2 (m, fs, ft); }
u32 EncMulAQ (u32 m, u32   , u32 fs, u32   ) { return VMULAq_C2 (m, fs); }
u32 EncOpMulA(u32 m, u32   , u32 fs, u32 ft) { return VOPMULA_C2(m, fs, ft); }

u32 EncAdd   (u32 m, u32 fd, u32 fs, u32 ft) { return VADD_C2   (m, fd, fs, ft); }
u32 EncAddX  (u32 m, u32 fd, u32 fs, u32 ft) { return VADDx_C2  (m, fd, fs, ft); }
u32 EncAddZ  (u32 m, u32 fd, u32 fs, u32 ft) { return VADDz_C2  (m, fd, fs, ft); }
u32 EncAddQ  (u32 m, u32 fd, u32 fs, u32   ) { return VADDq_C2  (m, fd, fs); }
u32 EncAddI  (u32 m, u32 fd, u32 fs, u32   ) { return VADDi_C2  (m, fd, fs); }
u32 EncAddA  (u32 m, u32   , u32 fs, u32 ft) { return VADDA_C2  (m, fs, ft); }
u32 EncAddAW (u32 m, u32   , u32 fs, u32 ft) { return VADDAw_C2 (m, fs, ft); }
u32 EncAddAI (u32 m, u32   , u32 fs, u32   ) { return VADDAi_C2 (m, fs); }

u32 EncSub   (u32 m, u32 fd, u32 fs, u32 ft) { return VSUB_C2   (m, fd, fs, ft); }
u32 EncSubY  (u32 m, u32 fd, u32 fs, u32 ft) { return VSUBy_C2  (m, fd, fs, ft); }
u32 EncSubW  (u32 m, u32 fd, u32 fs, u32 ft) { return VSUBw_C2  (m, fd, fs, ft); }
u32 EncSubQ  (u32 m, u32 fd, u32 fs, u32   ) { return VSUBq_C2  (m, fd, fs); }
u32 EncSubI  (u32 m, u32 fd, u32 fs, u32   ) { return VSUBi_C2  (m, fd, fs); }
u32 EncSubA  (u32 m, u32   , u32 fs, u32 ft) { return VSUBA_C2  (m, fs, ft); }
u32 EncSubAX (u32 m, u32   , u32 fs, u32 ft) { return VSUBAx_C2 (m, fs, ft); }
u32 EncSubAQ (u32 m, u32   , u32 fs, u32   ) { return VSUBAq_C2 (m, fs); }

constexpr Form kForms[] = {
	{"VMUL", false, EncMul},      {"VMULx", false, EncMulX},
	{"VMULw", false, EncMulW},    {"VMULq", false, EncMulQ},
	{"VMULi", false, EncMulI},    {"VMULA", false, EncMulA},
	{"VMULAy", false, EncMulAY},  {"VMULAq", false, EncMulAQ},
	{"VOPMULA", false, EncOpMulA},
	{"VADD", true, EncAdd},       {"VADDx", true, EncAddX},
	{"VADDz", true, EncAddZ},     {"VADDq", true, EncAddQ},
	{"VADDi", true, EncAddI},     {"VADDA", true, EncAddA},
	{"VADDAw", true, EncAddAW},   {"VADDAi", true, EncAddAI},
	{"VSUB", true, EncSub},       {"VSUBy", true, EncSubY},
	{"VSUBw", true, EncSubW},     {"VSUBq", true, EncSubQ},
	{"VSUBi", true, EncSubI},     {"VSUBA", true, EncSubA},
	{"VSUBAx", true, EncSubAX},   {"VSUBAq", true, EncSubAQ},
};

// Operands from the top of the VU's range, where a sum or a product leaves it.
// An add can only overflow against an exponent-255 operand -- below that the
// two addends are each under 2^128 and their sum cannot reach 2^129 -- so the
// whole top binade is drawn far more often than chance would, at both signs.
// The small values are here for the other half of the answer: a row that must
// NOT raise O.
u32 RandomOperand(std::mt19937& rng)
{
	std::uniform_int_distribution<u32> pick(0, 15);
	const u32 sign = (rng() & 1u) << 31;
	switch (pick(rng))
	{
		case 0: return sign | 0x7F800000u;         // 2^128, the first of the top binade
		case 1: return sign | 0x7F800001u;         // and one ULP along
		case 2: return sign | 0x7FC00000u;         // 1.5 * 2^128
		case 3: return sign | 0x7FFFFFFEu;         // one ULP below the maximum
		case 4: return sign | 0x7FFFFFFFu;         // the VU's largest value
		case 5: return sign | 0x7F7FFFFFu;         // FLT_MAX, the host's
		case 6: return sign | 0x7F000000u;         // 2^127
		case 7: return sign;                       // +/-0
		case 8: return sign | 0x3F800000u;         // 1.0
		case 9: return sign | 0x00400000u;         // a denormal
		default:
		{
			// The upper half of the exponent range, where two operands can
			// still multiply their way out.
			std::uniform_int_distribution<u32> e(120, 255), m(0, 0x7FFFFFu);
			return sign | (e(rng) << 23) | m(rng);
		}
	}
}

struct Obs
{
	u32 mac, status;
	u32 word[8]; // VF[fd] then ACC; the op wrote one of them
};

// One case through both engines. The flags are read out of the VU0 snapshots
// rather than through a CFC2 pair, because at mode 3 the two engines really do
// disagree about MAC's Z and S on an overflowing row -- their result words
// differ by a binade -- and a CFC2 would put that in an EE register where the
// harness's own diff would call it a failure.
void RunBoth(const Form& f, u32 mask, u32 fd, u32 fs, u32 ft, const u32 (&vfs)[3][4],
	u32 q, u32 i, int mode, Obs& interp, Obs& jit)
{
	EeRecTestHarness h;
	h.SetVu0ClampMode(mode);
	h.EnableVu0Capture();
	// Each engine is read on its own: an overflowing row is a row where the
	// recompiler's value is a binade short of the console's, so the VU
	// auto-diff would fail on every case this file exists to test.
	h.ExpectVu0Divergence();
	h.SeedVu0VfBits(kFs, vfs[0][0], vfs[0][1], vfs[0][2], vfs[0][3]);
	h.SeedVu0VfBits(kFt, vfs[1][0], vfs[1][1], vfs[1][2], vfs[1][3]);
	h.SeedVu0AccBits(vfs[2][0], vfs[2][1], vfs[2][2], vfs[2][3]);
	h.SeedVu0VfBits(kFd, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu);
	h.SeedVu0Vi(REG_Q, q);
	h.SeedVu0Vi(REG_I, i);
	h.LoadProgram({CTC2(0, REG_STATUS_FLAG), f.encode(mask, fd, fs, ft)});
	h.Run();

	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	interp.mac = h.GetVu0ViInterp(REG_MAC_FLAG) & 0xFFFFu;
	interp.status = h.GetVu0ViInterp(REG_STATUS_FLAG) & 0xFFFFu;
	jit.mac = h.GetVu0ViJit(REG_MAC_FLAG) & 0xFFFFu;
	jit.status = h.GetVu0ViJit(REG_STATUS_FLAG) & 0xFFFFu;
	for (int l = 0; l < 4; ++l)
	{
		interp.word[l] = h.GetVu0VfBitsInterp(fd, kLane[l]);
		jit.word[l] = h.GetVu0VfBitsJit(fd, kLane[l]);
		interp.word[4 + l] = h.GetVu0AccBitsInterp(kLane[l]);
		jit.word[4 + l] = h.GetVu0AccBitsJit(kLane[l]);
	}
}

bool SameWords(const Obs& a, const Obs& b)
{
	for (int l = 0; l < 8; ++l)
		if (a.word[l] != b.word[l])
			return false;
	return true;
}

// MAC's four bits for one NEON lane. PS2 MAC order is bit 0 = W, bit 3 = X, so
// lane l sits at bit 3 - l of each nibble.
constexpr u32 LaneMacBits(int lane)
{
	const u32 b = 1u << (3 - lane);
	return b | (b << 4) | (b << 8) | (b << 12);
}

// One random case, so the two tests below replay the same stream.
struct Case
{
	const Form* f;
	u32 mask, fd, fs, ft, q, i;
	u32 vfs[3][4];
};

Case NextCase(std::mt19937& rng, int iter)
{
	Case c{};
	c.f = &kForms[iter % std::size(kForms)];
	for (auto& reg : c.vfs)
		for (u32& lane : reg)
			lane = RandomOperand(rng);
	c.q = RandomOperand(rng);
	c.i = RandomOperand(rng);
	c.mask = rng() & 0xFu;
	// Aliasing: fd == fs, fd == ft and fs == ft all reachable.
	c.fs = (rng() & 1u) ? kFs : kFt;
	c.ft = (rng() & 1u) ? kFt : kFs;
	c.fd = (rng() % 3u == 0) ? c.fs : ((rng() % 2u) ? c.ft : kFd);
	return c;
}

constexpr int kIters = 1200;

} // namespace

// At vuClampMode 4 the recompiler must raise O exactly where the interpreter
// does, and enough of these must raise it -- and not raise it -- for that to
// mean anything.
TEST(EeVu0Cop2Overflow, OverflowMatchesTheInterpreterAtModeFour)
{
	std::mt19937 rng(0x0FA5EEDu);
	int raised = 0, quiet = 0, comparable = 0;
	for (int iter = 0; iter < kIters; ++iter)
	{
		const Case c = NextCase(rng, iter);
		Obs gi{}, gj{};
		RunBoth(*c.f, c.mask, c.fd, c.fs, c.ft, c.vfs, c.q, c.i, 4, gi, gj);
		SCOPED_TRACE(::testing::Message()
			<< c.f->name << " mask " << c.mask << " fd " << c.fd << " fs " << c.fs
			<< " ft " << c.ft << " iter " << iter);
		EXPECT_EQ(gj.mac & kMacO, gi.mac & kMacO) << "MAC O";
		EXPECT_EQ(gj.status & kStatO, gi.status & kStatO) << "STATUS O/OS";
		// Where a lane's written word is the same on both engines, its whole
		// ZSUO group has to be as well: that is what says the O nibble arrived
		// without pushing some other bit out of place. A lane whose words
		// differ is the value gap, which this file is not about.
		for (int l = 0; l < 4; ++l)
		{
			if (gi.word[l] != gj.word[l] || gi.word[4 + l] != gj.word[4 + l])
				continue;
			EXPECT_EQ(gj.mac & LaneMacBits(l), gi.mac & LaneMacBits(l))
				<< "MAC lane " << l;
			++comparable;
		}
		if (SameWords(gi, gj))
			EXPECT_EQ(gj.status, gi.status) << "STATUS";
		raised += (gi.mac & kMacO) != 0;
		quiet += (gi.mac & kMacO) == 0;
	}
	EXPECT_GT(raised, 200) << "the stream stopped overflowing, so it stopped testing O";
	EXPECT_GT(quiet, 200) << "everything overflowed, so a constant would pass";
	EXPECT_GT(comparable, 400) << "no lane wrote the same word on both engines, so "
	                              "the per-lane flag comparison never ran";
}

// The gate, from the other side. MAC O and the ceiling are the only two things
// on vuClampMode 4, so replaying the same stream at 1, 2 and 3 has to lose the O
// bits at each, and for the multiplies it must move a word only where it lost
// one: the ceiling is the whole of what mode 4 does to a multiply's value, so a
// moved word has to be the lower mode's own answer with its magnitude promoted
// from FLT_MAX to 0x7FFFFFFF, in a lane whose O it raised. The adds are excluded
// from that half deliberately: mode 4 also turns on the guard mask there, which
// is a value model and is scored by the guard-mask files. The MAC U comparison
// skips the gated nibbles because U is a mode lower and mode 1 has none of it;
// ee_vu0_cop2_mul_underflow_tests.cpp scores that gate.
TEST(EeVu0Cop2Overflow, ModesBelowFourLoseTheOverflowBits)
{
	for (int low = 1; low <= 3; ++low)
	{
	SCOPED_TRACE(::testing::Message() << "vuClampMode " << low);
	std::mt19937 rng(0x0FA5EEDu);
	int lost = 0, nonzero = 0, promoted = 0;
	for (int iter = 0; iter < kIters; ++iter)
	{
		const Case c = NextCase(rng, iter);
		Obs gi{}, g4{}, gd{}, g1{};
		RunBoth(*c.f, c.mask, c.fd, c.fs, c.ft, c.vfs, c.q, c.i, 4, gi, g4);
		RunBoth(*c.f, c.mask, c.fd, c.fs, c.ft, c.vfs, c.q, c.i, low, gd, g1);
		SCOPED_TRACE(::testing::Message()
			<< c.f->name << " mask " << c.mask << " fd " << c.fd << " fs " << c.fs
			<< " ft " << c.ft << " iter " << iter);
		EXPECT_EQ(g1.mac & kMacO, 0u) << "raised MAC O below the gate";
		EXPECT_EQ(g1.status & kStatO, 0u) << "raised STATUS O/OS below the gate";
		if (!c.f->adder)
		{
			EXPECT_EQ(g1.mac & ~(kMacO | kMacU), g4.mac & ~(kMacO | kMacU))
				<< "MAC outside the gated nibbles";
			EXPECT_EQ(g1.status & ~(kStatO | kStatU), g4.status & ~(kStatO | kStatU))
				<< "STATUS outside the gated bits";
			// Word 0-3 is Fd's lane, 4-7 is the ACC's; either way the lane
			// index is the low two bits, and only the register the op wrote
			// can move. This is also where the predicate's scratch registers
			// are checked: it runs before the operand clamp, with both
			// operands and the destination live, and a full-mask op computes
			// into a VF cache slot that may be Fs's or Ft's.
			for (int l = 0; l < 8; ++l)
			{
				if (g4.word[l] != g1.word[l])
				{
					EXPECT_EQ(g1.word[l] & 0x7FFFFFFFu, 0x7F7FFFFFu)
						<< "word " << l << " moved from something other than FLT_MAX";
					EXPECT_EQ(g4.word[l], g1.word[l] | 0x7FFFFFFFu)
						<< "word " << l << " moved to something other than the ceiling";
					EXPECT_NE(g4.mac & LaneMacBits(l & 3) & kMacO, 0u)
						<< "word " << l << " moved in a lane that raised no O";
					++promoted;
				}
				nonzero += g1.word[l] != 0;
			}
		}
		lost += (gi.mac & kMacO) != 0;
	}
	EXPECT_GT(lost, 200) << "nothing was lost, so the gate was not exercised";
	EXPECT_GT(nonzero, 1000) << "the below-gate run read back nothing, so the value "
	                            "comparison above compared zero with zero";
	EXPECT_GT(promoted, 100) << "no multiply's word took the ceiling, so the shape of "
	                            "the move was never checked";
	}
}

// The forms the model does not reach, pinned so that adding one is a decision
// rather than an accident. A MADD's accumulate is handed a product the host has
// already saturated at FLT_MAX -- and a NaN product, from zero against an
// exponent-255 operand, arrives as +FLT_MAX too -- so a magnitude test on the
// addends both misses overflows the console raises and invents ones it does
// not. The recompiler raises nothing here instead, which is wrong on the rows
// the console overflows and right on the rest.
TEST(EeVu0Cop2Overflow, ExcludedFormsRaiseNoOverflow)
{
	struct Row { const char* what; bool interp_o; u32 fs, ft, acc, word; };
	// Both directions, from vu0_macro_fmac_range_console_tests.cpp: rows the
	// console overflows on a product the host has already saturated, and a row
	// it does not where the saturation would invent one. The addends are the
	// same magnitude in all four.
	const Row kRows[] = {
		{"MADD -max + 2^130", true, 0x7F800000u, 0x40800000u, 0xFFFFFFFFu,
		 VMADD_C2(0xF, kFd, kFs, kFt)},
		{"MSUB max - 2^130", true, 0x7F800000u, 0x40800000u, 0x7FFFFFFFu,
		 VMSUB_C2(0xF, kFd, kFs, kFt)},
		{"OPMSUB -max - 2^130", true, 0x7F800000u, 0x40800000u, 0xFFFFFFFFu,
		 VOPMSUB_C2(0xF, kFd, kFs, kFt)},
		// The product is zero times 2^128, which the console reads as a plain
		// zero and the host as a NaN the clamp folds to +FLT_MAX.
		{"MADD max + 0*2^128", false, 0x00000000u, 0x7F800000u, 0x7FFFFFFFu,
		 VMADD_C2(0xF, kFd, kFs, kFt)},
	};
	for (const Row& r : kRows)
	{
		SCOPED_TRACE(r.what);
		EeRecTestHarness h;
		h.SetVu0ClampMode(3);
		h.EnableVu0Capture();
		h.ExpectVu0Divergence();
		h.SeedVu0VfBits(kFs, r.fs, r.fs, r.fs, r.fs);
		h.SeedVu0VfBits(kFt, r.ft, r.ft, r.ft, r.ft);
		h.SeedVu0AccBits(r.acc, r.acc, r.acc, r.acc);
		h.SeedVu0VfBits(kFd, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu);
		h.LoadProgram({CTC2(0, REG_STATUS_FLAG), r.word});
		h.Run();
		EXPECT_EQ(h.GetVu0ViJit(REG_MAC_FLAG) & kMacO, 0u) << "[jit] MAC O";
		EXPECT_EQ((h.GetVu0ViInterp(REG_MAC_FLAG) & kMacO) != 0u, r.interp_o)
			<< "[interp] MAC O, which is what the console says here";
	}
}

} // namespace recompiler_tests
