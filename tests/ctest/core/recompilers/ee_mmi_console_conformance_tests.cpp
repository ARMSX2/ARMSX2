// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// MMI conformance against real PS2 hardware.
//
// autocases.h is generated from the `.expected` captures shipped by
// unknownbrackets/ps2autotests, recorded on a real console. Both engines are
// scored against those captures rather than against each other, so a mistake
// the interpreter and the recompiler share is still visible here — which a
// JIT-vs-interp differential cannot see.
//
// 1743 rd-writing cases over the saturating arithmetic, compares, logic,
// pack/unpack/exchange shuffles, PADSBH, PLZCW and both shift forms, plus 422
// HI/LO cases over the multiply/divide family.
//
// Operands are reconstructed from each capture's printed label, which the test
// program emits from the actual register contents. A mis-reconstruction shows
// up as mass divergence rather than a subtle one, so a run in which nearly
// everything matches is itself evidence the mapping is right.

#include <gtest/gtest.h>

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/RecompilerTestEnvironment.h"

#include <string>

#include "autocases.h"

using namespace ps2auto;
using recompiler_tests::EeRecTestHarness;

namespace
{
constexpr u32 kRd = 8, kRs = 9, kRt = 10;

u64 Lo64(const u32* w) { return (u64)w[0] | ((u64)w[1] << 32); }
u64 Hi64(const u32* w) { return (u64)w[2] | ((u64)w[3] << 32); }

u32 EncodeRd(const AutoCase& c)
{
	const std::string op = c.op;
	using namespace mips;
	using namespace mips::ee;
#define R3(NAME, FN) if (op == NAME) return FN(kRd, kRs, kRt);
#define R2(NAME, FN) if (op == NAME) return FN(kRd, kRt);
#define SI(NAME, FN) if (op == NAME) return FN(kRd, kRt, c.sa & 0x1F);
#define SV(NAME, FN) if (op == NAME) return FN(kRd, kRt, kRs);
	R3("paddb", PADDB) R3("paddh", PADDH) R3("paddw", PADDW)
	R3("paddsb", PADDSB) R3("paddsh", PADDSH) R3("paddsw", PADDSW)
	R3("paddub", PADDUB) R3("padduh", PADDUH) R3("padduw", PADDUW)
	R3("psubb", PSUBB) R3("psubh", PSUBH) R3("psubw", PSUBW)
	R3("psubsb", PSUBSB) R3("psubsh", PSUBSH) R3("psubsw", PSUBSW)
	R3("psubub", PSUBUB) R3("psubuh", PSUBUH) R3("psubuw", PSUBUW)
	R3("padsbh", PADSBH)
	R3("pmaxh", PMAXH) R3("pmaxw", PMAXW) R3("pminh", PMINH) R3("pminw", PMINW)
	R2("pabsh", PABSH) R2("pabsw", PABSW)
	R3("pceqb", PCEQB) R3("pceqh", PCEQH) R3("pceqw", PCEQW)
	R3("pcgtb", PCGTB) R3("pcgth", PCGTH) R3("pcgtw", PCGTW)
	R3("pand", PAND) R3("por", POR) R3("pxor", PXOR) R3("pnor", PNOR)
	R2("plzcw", PLZCW)
	R3("pextlb", PEXTLB) R3("pextlh", PEXTLH) R3("pextlw", PEXTLW)
	R3("pextub", PEXTUB) R3("pextuh", PEXTUH) R3("pextuw", PEXTUW)
	R3("ppacb", PPACB) R3("ppach", PPACH) R3("ppacw", PPACW)
	R3("pinteh", PINTEH) R3("pinth", PINTH)
	R3("pcpyld", PCPYLD) R3("pcpyud", PCPYUD)
	R2("pcpyh", PCPYH) R2("prevh", PREVH) R2("prot3w", PROT3W)
	R2("pexch", PEXCH) R2("pexcw", PEXCW) R2("pexeh", PEXEH) R2("pexew", PEXEW)
	R2("pext5", PEXT5) R2("ppac5", PPAC5)
	SI("psllh", PSLLH) SI("psllw", PSLLW) SI("psrah", PSRAH)
	SI("psraw", PSRAW) SI("psrlh", PSRLH) SI("psrlw", PSRLW)
	SV("psllvw", PSLLVW) SV("psravw", PSRAVW) SV("psrlvw", PSRLVW)
#undef R3
#undef R2
#undef SI
#undef SV
	return 0;
}

u32 EncodeHl(const AutoHlCase& c)
{
	const std::string op = c.op;
	using namespace mips;
	using namespace mips::ee;
#define R3(NAME, FN) if (op == NAME) return FN(kRd, kRs, kRt);
#define R2(NAME, FN) if (op == NAME) return FN(kRs, kRt);
	R3("pmaddw", PMADDW) R3("pmsubw", PMSUBW) R3("pmadduw", PMADDUW)
	R3("pmaddh", PMADDH) R3("pmsubh", PMSUBH)
	R3("phmadh", PHMADH) R3("phmsbh", PHMSBH)
	R3("pmultw", PMULTW) R3("pmultuw", PMULTUW) R3("pmulth", PMULTH)
	R2("pdivw", PDIVW) R2("pdivuw", PDIVUW) R2("pdivbw", PDIVBW)
#undef R3
#undef R2
	return 0;
}
} // namespace

TEST(EeMmiConsoleConformance, RdWritingOpsMatchConsole)
{
	int checked = 0;
	for (int i = 0; i < kAutoCaseCount; ++i)
	{
		const AutoCase& c = kAutoCases[i];
		const u32 word = EncodeRd(c);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;

		for (int jit = 0; jit < 2; ++jit)
		{
			SCOPED_TRACE(::testing::Message()
			             << c.op << " " << c.label << (jit ? " [jit]" : " [interp]"));
			EeRecTestHarness h;
			h.SetGpr128(kRs, Lo64(c.rs), Hi64(c.rs));
			h.SetGpr128(kRt, Lo64(c.rt), Hi64(c.rt));
			h.SetGpr128(kRd, Lo64(c.rd_pre), Hi64(c.rd_pre));
			h.LoadProgram({word});
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();
			const u64 lo = jit ? h.GetGpr64Jit(kRd) : h.GetGpr64Interp(kRd);
			const u64 hi = jit ? h.GetGprUpper64Jit(kRd) : h.GetGprUpper64Interp(kRd);
			EXPECT_EQ(lo, Lo64(c.rd));
			EXPECT_EQ(hi, Hi64(c.rd));
		}
		++checked;
	}
	// Guards against the table silently emptying out.
	EXPECT_EQ(checked, kAutoCaseCount);
	EXPECT_GT(checked, 1700);
}

// The multiply/divide family. HI/LO are preset before every case, which is what
// makes the width of the accumulate observable at all — and it is what caught
// PMADDW/PMSUBW: both engines used to accumulate into two independent 32-bit
// halves (plus a truncating divide and a lane-0 addend standing in for the PS2
// multiply errata), which loses the carry between the halves. With HI/LO preset
// to zero none of that is observable; preset to the captures' values, 10 of the
// 64 cases separate. Nothing here is held back any more.
TEST(EeMmiConsoleConformance, HiLoWritingOpsMatchConsole)
{
	int checked = 0;
	for (int i = 0; i < kAutoHlCaseCount; ++i)
	{
		const AutoHlCase& c = kAutoHlCases[i];
		const u32 word = EncodeHl(c);
		ASSERT_NE(word, 0u) << "no encoder for " << c.op;

		for (int jit = 0; jit < 2; ++jit)
		{
			EeRecTestHarness h;
			h.SetGpr128(kRs, Lo64(c.rs), Hi64(c.rs));
			h.SetGpr128(kRt, Lo64(c.rt), Hi64(c.rt));
			h.SetGpr128(kRd, Lo64(c.rd_pre), Hi64(c.rd_pre));
			h.SetHiPair(kHiPre, kHi1Pre);
			h.SetLoPair(kLoPre, kLo1Pre);
			h.LoadProgram({word});
			if (jit)
				h.RunJitNoDiff();
			else
				h.RunInterpOnly();

			const u64 hi  = jit ? h.GetHi64Jit()      : h.GetHi64Interp();
			const u64 hi1 = jit ? h.GetHiUpper64Jit() : h.GetHiUpper64Interp();
			const u64 lo  = jit ? h.GetLo64Jit()      : h.GetLo64Interp();
			const u64 lo1 = jit ? h.GetLoUpper64Jit() : h.GetLoUpper64Interp();
			const u64 rl  = jit ? h.GetGpr64Jit(kRd)  : h.GetGpr64Interp(kRd);
			const u64 rh  = jit ? h.GetGprUpper64Jit(kRd) : h.GetGprUpper64Interp(kRd);

			SCOPED_TRACE(::testing::Message()
			             << c.op << " " << c.label << (jit ? " [jit]" : " [interp]"));
			EXPECT_EQ(hi, c.hi);
			EXPECT_EQ(hi1, c.hi1);
			EXPECT_EQ(lo, c.lo);
			EXPECT_EQ(lo1, c.lo1);
			if (c.form == F_RRRHL)
			{
				EXPECT_EQ(rl, Lo64(c.rd));
				EXPECT_EQ(rh, Hi64(c.rd));
			}
		}
		++checked;
	}

	EXPECT_EQ(checked, kAutoHlCaseCount);
}
