// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// EE loads and stores against real PS2 hardware.
//
// autocases_eelsu.h is generated from unknownbrackets/ps2autotests
// tests/cpu/ee/lsu.expected: every EE load and store — 21 opcodes — against a
// fixed byte pattern at three offsets, plus a deliberately misaligned +11 for
// the quad ops. 134 cases.
//
// ee_rec_loadstore_tests.cpp already covers this ground as a JIT-vs-interp
// differential, and that is its limitation: the two engines share an idea of
// what LWL at offset 1 means, and a differential cannot see a shared mistake.
// The eight unaligned ops (LDL/LDR/LWL/LWR, SDL/SDR/SWL/SWR) are the classic
// place for one — their byte/shift tables are fiddly enough that the same
// off-by-one gets written twice.
//
// Two things this capture pins that a 64-bit test cannot:
//
//   * It prints the FULL 128-bit destination on every load, so "loads leave
//     bits 127:64 alone" is asserted on all twelve of them — and LQ is the
//     one that must not.
//   * The `-> $0` cases load into $0 and read $0 back through `ori rt, $0, 0`,
//     so a load that wrongly wrote the zero register shows up immediately.
//
// The register presets are executed rather than injected: where the capture
// builds rt with `lui/ori`, so does this, which re-pins that neither touches
// bits 127:64.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include <cstring>
#include <string>
#include <vector>

#include "autocases_eelsu.h"

using namespace ps2auto_eelsu;

namespace recompiler_tests
{
namespace
{
using namespace mips;
using namespace mips::ee; // LD/LDL/LDR/SD/SDL/SDR/LWU/LQ/SQ live here

constexpr u32 kScratch = RecompilerTestEnvironment::kScratchAddr;
constexpr u32 kBase = 8; // $t0 — points at C_PATTERN[1], i.e. pattern + 16
constexpr u32 kRt = 9;   // $t1 — the loaded/stored register

// C_GARBAGE1 as a 128-bit register value. Both presets start from this; the
// [imm] cases then overwrite the low 64 with lui/ori, which is why every
// expected `hi` in the table is this same word.
constexpr u64 kGarbageLo =
	(static_cast<u64>(kGarbage1[1]) << 32) | kGarbage1[0];
constexpr u64 kGarbageHi =
	(static_cast<u64>(kGarbage1[3]) << 32) | kGarbage1[2];

u32 EncodeLoad(const std::string& op, u32 rt, s16 off, u32 base)
{
	if (op == "lb") return LB(rt, off, base);
	if (op == "lbu") return LBU(rt, off, base);
	if (op == "lh") return LH(rt, off, base);
	if (op == "lhu") return LHU(rt, off, base);
	if (op == "lw") return LW(rt, off, base);
	if (op == "lwu") return LWU(rt, off, base);
	if (op == "lwl") return LWL(rt, off, base);
	if (op == "lwr") return LWR(rt, off, base);
	if (op == "ld") return LD(rt, off, base);
	if (op == "ldl") return LDL(rt, off, base);
	if (op == "ldr") return LDR(rt, off, base);
	if (op == "lq") return LQ(rt, off, base);
	return 0;
}

u32 EncodeStore(const std::string& op, u32 rt, s16 off, u32 base)
{
	if (op == "sb") return SB(rt, off, base);
	if (op == "sh") return SH(rt, off, base);
	if (op == "sw") return SW(rt, off, base);
	if (op == "swl") return SWL(rt, off, base);
	if (op == "swr") return SWR(rt, off, base);
	if (op == "sd") return SD(rt, off, base);
	if (op == "sdl") return SDL(rt, off, base);
	if (op == "sdr") return SDR(rt, off, base);
	if (op == "sq") return SQ(rt, off, base);
	return 0;
}

// The capture's `SET_U32<0xABCD4321>`: lui then ori, executed, so the low 64
// bits get there by sign extension and 127:64 keep C_GARBAGE1's half.
void AppendImmPreset(std::vector<u32>& prog)
{
	prog.push_back(LUI(kRt, static_cast<u16>(kImmPreset >> 16)));
	prog.push_back(ORI(kRt, kRt, static_cast<u16>(kImmPreset & 0xFFFF)));
}

void Prepare(EeRecTestHarness& h)
{
	h.WriteBytes(kScratch, kPattern, sizeof(kPattern));
	h.SetGpr64(kBase, kScratch + 16);
	h.SetGpr128(kRt, kGarbageLo, kGarbageHi);
}

// No recorded divergences: every load case matches silicon on both engines.
// The interpreter's LD used to write cpuRegs.GPR.r[_Rt_].UD[0] with no `!_Rt_`
// guard, so a following `ori rt, $0, 0` read back the loaded quad instead of
// zero; it now guards like its eleven neighbours (LB/LBU/LH/LHU/LW/LWU/LWL/
// LWR/LDL/LDR early-out on !_Rt_, LQ routes through gpr_GetWritePtr).

// Builds and runs one load case on one engine. Returns false if an opcode has
// no encoder, so the caller can fail with a useful name.
bool RunLoadCase(const LoadCase& c, bool jit, u64& lo, u64& hi)
{
	const u32 w1 = EncodeLoad(c.op1, c.into_zero ? 0u : kRt,
	                          static_cast<s16>(c.off1), kBase);
	if (w1 == 0u)
		return false;

	std::vector<u32> prog;
	if (c.imm_preset)
		AppendImmPreset(prog);
	prog.push_back(w1);
	if (c.op2)
	{
		const u32 w2 = EncodeLoad(c.op2, kRt, static_cast<s16>(c.off2), kBase);
		if (w2 == 0u)
			return false;
		prog.push_back(w2);
	}
	if (c.into_zero)
		prog.push_back(ORI(kRt, 0, 0)); // read $0 back, as the capture does

	EeRecTestHarness h;
	Prepare(h);
	h.LoadProgram(prog);
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();

	lo = jit ? h.GetGpr64Jit(kRt) : h.GetGpr64Interp(kRt);
	hi = jit ? h.GetGprUpper64Jit(kRt) : h.GetGprUpper64Interp(kRt);
	return true;
}
} // namespace

// Cross-engine agreement. The comment on LoadsMatchConsole below reserves the
// right for the engines to disagree; no load case does.
TEST(EeLsuConsoleConformance, EnginesAgreeOnEveryLoadCase)
{
	int checked = 0;
	for (int i = 0; i < kLoadCaseCount; ++i)
	{
		const LoadCase& c = kLoadCases[i];
		u64 jl = 0, jh = 0, il = 0, ih = 0;
		if (!RunLoadCase(c, true, jl, jh))
			continue;
		ASSERT_TRUE(RunLoadCase(c, false, il, ih)) << c.label;
		++checked;
		SCOPED_TRACE(::testing::Message() << c.label);
		EXPECT_EQ(jl, il) << "engines disagree on the low 64 bits";
		EXPECT_EQ(jh, ih) << "engines disagree on the high 64 bits";
	}
	EXPECT_GT(checked, 70) << "the load-case table shrank or stopped encoding";
}

// The engines legitimately might disagree here, so each is scored against the
// console independently rather than against the other.
TEST(EeLsuConsoleConformance, LoadsMatchConsole)
{
	for (int i = 0; i < kLoadCaseCount; ++i)
	{
		const LoadCase& c = kLoadCases[i];
		SCOPED_TRACE(::testing::Message() << c.label);

		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			u64 lo = 0, hi = 0;
			ASSERT_TRUE(RunLoadCase(c, jit != 0, lo, hi));
			EXPECT_EQ(lo, c.lo) << "bits 63:0";
			EXPECT_EQ(hi, c.hi) << "bits 127:64";
		}
	}
}

TEST(EeLsuConsoleConformance, StoresMatchConsole)
{
	for (int i = 0; i < kStoreCaseCount; ++i)
	{
		const StoreCase& c = kStoreCases[i];
		SCOPED_TRACE(::testing::Message() << c.label);

		const u32 w1 = EncodeStore(c.op1, kRt, static_cast<s16>(c.off1),
		                           kBase);
		ASSERT_NE(w1, 0u) << "no encoder for " << c.op1;

		std::vector<u32> prog;
		if (c.imm_preset)
			AppendImmPreset(prog);
		prog.push_back(w1);
		if (c.op2)
		{
			const u32 w2 = EncodeStore(c.op2, kRt, static_cast<s16>(c.off2),
			                           kBase);
			ASSERT_NE(w2, 0u) << "no encoder for " << c.op2;
			prog.push_back(w2);
		}

		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(jit ? "[jit]" : "[interp]");
			EeRecTestHarness h;
			Prepare(h);
			h.TrackMemWindow(kScratch, sizeof(kPattern));
			h.LoadProgram(prog);
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();

			const u32 blk = kScratch + 16 + static_cast<u32>(c.block);
			for (int w = 0; w < 4; ++w)
			{
				EXPECT_EQ(h.ReadU32(blk + 4u * static_cast<u32>(w)), c.mem[w])
					<< "word " << w;
			}
		}
	}
}

// Two invariants the capture states outright, pulled out so a failure names
// the rule rather than a row of the table.

// LQ/SQ mask the address down to 16 bytes rather than faulting or reading
// unaligned: the console's `lq +11` row equals its own `lq +0` row.
TEST(EeLsuConsoleConformance, QuadOpsMaskTheAddressTo16Bytes)
{
	int checked = 0;
	for (int i = 0; i < kLoadCaseCount; ++i)
	{
		const LoadCase& c = kLoadCases[i];
		if (std::string(c.op1) != "lq" || c.off1 != 11)
			continue;
		for (int j = 0; j < kLoadCaseCount; ++j)
		{
			const LoadCase& z = kLoadCases[j];
			if (std::string(z.op1) != "lq" || z.off1 != 0 || z.into_zero ||
				z.imm_preset != c.imm_preset)
				continue;
			EXPECT_EQ(c.lo, z.lo) << c.label << " vs " << z.label;
			EXPECT_EQ(c.hi, z.hi) << c.label << " vs " << z.label;
			++checked;
		}
	}
	EXPECT_EQ(checked, 2) << "expected the +11 row of both presets";
}

TEST(EeLsuConsoleConformance, LoadsPreserveTheUpper64BitsExceptLq)
{
	int preserved = 0, overwritten = 0;
	for (int i = 0; i < kLoadCaseCount; ++i)
	{
		const LoadCase& c = kLoadCases[i];
		// LQ writes all 128 bits, so only its `-> $0` row (which never lands)
		// keeps the preset half.
		if (std::string(c.op1) == "lq" && !c.into_zero)
		{
			EXPECT_NE(c.hi, kGarbageHi) << c.label;
			++overwritten;
			continue;
		}
		EXPECT_EQ(c.hi, kGarbageHi) << c.label;
		++preserved;
	}
	EXPECT_EQ(overwritten, 8);
	EXPECT_EQ(preserved, kLoadCaseCount - 8);
}

} // namespace recompiler_tests
