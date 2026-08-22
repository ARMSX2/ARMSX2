// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU FMAC's range and its underflow, scored on microVU.
//
// autocases_vusat.h and autocases_vuflow.h are console measurements
// (captures/vusat, captures/vuflow), and until this file existed only the COP2
// macro emitter was scored on them -- microVU's FMAC was checked against the
// interpreter alone, by vu_micro_fmac_uo_tests.cpp. That leaves an emitter with
// its own operand clamps, its own result clamp and its own register allocator
// standing on a differential whose reference is not the console.
//
// Both captures were taken in VU0 MACRO mode. Whether a VU0 microprogram
// answers the same has not been measured, so a row where the two emitters
// differ is a question for the next probe rather than a defect to chase: what
// is scored here is microVU against the console, and the macro column is beside
// it for comparison, not as an oracle.
//
// The interpreter is the same code either way -- one VUops.cpp for macro and
// micro -- so its column has to come out at the macro file's number, which is
// zero for both tables. That is the plumbing check: if the program below did
// not reach the FMAC, or the flag read-back came out stale, the interpreter
// would miss rows too.
//
// What the two emitters come to: 44 column-misses on vusat and 168 on vuflow,
// the same totals as the COP2 macro path, and on vuflow row for row. On vusat
// four rows land differently, all four of them the operand clamp microVU
// applies and the macro path deliberately does not (recCOP2_VSUB says why):
//
//     4  max + -2^128    micro clamps both addends to +/-FLT_MAX and returns 0
//     5  max - 2^128     with MAC Z; the console returns 7F7FFFFE
//     6  2^128 + -2^128  the same clamp, and here 0 IS the console's answer
//    27  1.0 + 0*2^128   the clamped product is a plain zero, so the value
//                        survives where the macro path's NaN does not
//
// Which is one trade, taken twice each way. Both emitters need the operands
// held at their real magnitude, and that is where all four rows go.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <utility>

#include "autocases_vusat.h"
#include "autocases_vuflow.h"

namespace recompiler_tests
{
using namespace vu;

namespace {

// The console probe's register assignment, kept so the pair scored is the one
// that was measured.
constexpr u32 kFs = 1, kFt = 2, kAcc = 3, kFd = 4, kZero = 7;

constexpr int kScoredMode = 4;

// Columns, shared by the two tables (VSB_* and VFB_* are the same three bits).
enum { kValue = 1, kMac = 2, kStat = 4 };

const char* kColName[] = {"value", "mac", "stat"};
constexpr u8 kColBit[] = {kValue, kMac, kStat};

int PopCount(u8 v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1); }

// One row of either table, flattened: the two headers differ only in their op
// enum, and both seed the same three registers.
struct Case
{
	u32 word;
	u32 fs, ft, acc;
	u32 out;
	u16 mac, stat;
	const char* what;
};

u32 EncodeSat(u8 op)
{
	using namespace console_vusat;
	switch (op)
	{
		case VS_ADD:  return VADD_U (mask::xyzw, kFd, kFs, kFt);
		case VS_SUB:  return VSUB_U (mask::xyzw, kFd, kFs, kFt);
		case VS_MUL:  return VMUL_U (mask::xyzw, kFd, kFs, kFt);
		case VS_MADD: return VMADD_U(mask::xyzw, kFd, kFs, kFt);
		case VS_MSUB: return VMSUB_U(mask::xyzw, kFd, kFs, kFt);
		case VS_MAX:  return VMAX_U (mask::xyzw, kFd, kFs, kFt);
		case VS_MINI: return VMINI_U(mask::xyzw, kFd, kFs, kFt);
		default:      return 0;
	}
}

u32 EncodeFlow(u8 op)
{
	using namespace console_vuflow;
	switch (op)
	{
		case VF_ADD:  return VADD_U (mask::xyzw, kFd, kFs, kFt);
		case VF_SUB:  return VSUB_U (mask::xyzw, kFd, kFs, kFt);
		case VF_MUL:  return VMUL_U (mask::xyzw, kFd, kFs, kFt);
		case VF_MADD: return VMADD_U(mask::xyzw, kFd, kFs, kFt);
		case VF_MSUB: return VMSUB_U(mask::xyzw, kFd, kFs, kFt);
		default:      return 0;
	}
}

std::vector<Case> SatCases()
{
	std::vector<Case> v;
	for (const console_vusat::VuSatCase& c : console_vusat::kVuSatCases)
		v.push_back({EncodeSat(c.op), c.fs, c.ft, c.acc, c.out, c.mac, c.stat, c.what});
	return v;
}

std::vector<Case> FlowCases()
{
	std::vector<Case> v;
	for (const console_vuflow::VuFlowCase& c : console_vuflow::kVuFlowCases)
		v.push_back({EncodeFlow(c.op), c.fs, c.ft, c.acc, c.out, c.mac, c.stat, c.what});
	return v;
}

struct Observed
{
	u32 out[4];
	u32 mac;
	u32 stat;
};

struct Pair
{
	Observed jit, interp;
};

// The capture's pair, as a microprogram: the ACC seed is a VADDA against a zero
// register rather than a direct write, because the seed's own flags are part of
// the status column. Four NOP pairs stand between the op and the E-bit so the
// flag pipeline has drained by the time the read-back runs.
Pair RunCase(int vu, const Case& c, int clamp_mode)
{
	VuTestHarness h(vu);
	h.SetVuClampMode(clamp_mode);
	h.SetVfBits(kFs, c.fs, c.fs, c.fs, c.fs);
	h.SetVfBits(kFt, c.ft, c.ft, c.ft, c.ft);
	h.SetVfBits(kAcc, c.acc, c.acc, c.acc, c.acc);
	h.SetVfBits(kZero, 0, 0, 0, 0);
	h.SetVfBits(kFd, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu);
	h.LoadProgram({
		VuOp{0u, VADDA_U(mask::xyzw, kAcc, kZero)},
		VuOp{0u, c.word},
		VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()},
		EBitNopPair(),
	});
	// Each engine is scored against the console on its own; the two
	// legitimately disagree on the top binade, which is what this table is for.
	h.RunNoDiff();

	Pair p{};
	for (int lane = 0; lane < 4; ++lane)
	{
		p.jit.out[lane] = h.JitSnapshot().regs.VF[kFd].UL[lane];
		p.interp.out[lane] = h.InterpSnapshot().regs.VF[kFd].UL[lane];
	}
	p.jit.mac = h.GetViJit(REG_MAC_FLAG) & 0xFFFFu;
	p.interp.mac = h.GetViInterp(REG_MAC_FLAG) & 0xFFFFu;
	p.jit.stat = h.GetViJit(REG_STATUS_FLAG) & 0xFFFFu;
	p.interp.stat = h.GetViInterp(REG_STATUS_FLAG) & 0xFFFFu;
	return p;
}

// The probe seeded fs and ft into all four lanes and read one word back, so a
// lane that does not carry the console's word is a miss whichever lane it is.
u8 Misses(const Case& c, const Observed& o)
{
	u8 m = 0;
	for (int lane = 0; lane < 4; ++lane)
	{
		if (o.out[lane] != c.out)
			m |= kValue;
	}
	if (o.mac != c.mac) m |= kMac;
	if (o.stat != c.stat) m |= kStat;
	return m;
}

// What microVU currently fails to reproduce, per row and per column, at
// kScoredMode. Not console data: regenerate from DISABLED_DumpConsoleComparison
// below, which prints these two lines whole. Clearing a bit is how a fix lands.
constexpr u8 kMicroBadSat[] = {
	0, 0, 1, 1, 7, 7, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 4, 7, 7, 7, 7,
	4, 4, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0,
	0, 0, 0, 7, 7, 7, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1,
	0, 0, 0, 0,
};
constexpr u8 kMicroBadFlow[] = {
	0, 0, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 0, 0, 0,
	0, 7, 7, 0, 0, 0, 0, 0, 0, 0,
};

// The totals the two tables come to, so a change that moved rows in both
// directions cannot pass by keeping the shape of the arrays -- and what they
// come to at the two modes below, where none of the models is emitted. vusat
// gets worse from 1 to 2 rather than better: mode 2 is where mVUclamp3 starts
// clamping the addends, which costs the two rows the macro emitter keeps by
// not clamping at all.
constexpr int kMicroBadSatTotal = 44;
constexpr int kMicroBadFlowTotal = 168;
constexpr int kMicroBadSatBelow[] = {74, 76, 76};   // vuClampMode 1, 2, 3
constexpr int kMicroBadFlowBelow[] = {180, 180, 180};

void Score(const std::vector<Case>& cases, const u8* bad, size_t bad_count, int total,
	const char* table)
{
	ASSERT_EQ(bad_count, cases.size()) << table << ": known-bad table is the wrong length";
	int bad_jit = 0, bad_interp = 0;
	for (size_t i = 0; i < cases.size(); ++i)
	{
		const Case& c = cases[i];
		ASSERT_NE(c.word, 0u) << table << " case " << i << ": no encoder";
		const Pair p = RunCase(0, c, kScoredMode);
		const u8 got[2] = {Misses(c, p.interp), Misses(c, p.jit)};
		const u8 known[2] = {0, bad[i]};
		bad_interp += PopCount(got[0]);
		bad_jit += PopCount(known[1]);
		for (int jit = 0; jit < 2; ++jit)
		{
			for (int col = 0; col < 3; ++col)
			{
				SCOPED_TRACE(::testing::Message()
					<< table << " case " << i << " [" << (jit ? "jit" : "interp") << "] "
					<< kColName[col] << ": " << c.what);
				if (known[jit] & kColBit[col])
				{
					EXPECT_TRUE(got[jit] & kColBit[col])
						<< "now MATCHES the console. Regenerate the table from "
						   "DISABLED_DumpConsoleComparison.";
				}
				else
				{
					EXPECT_FALSE(got[jit] & kColBit[col]) << "new divergence from silicon";
				}
			}
		}
	}
	EXPECT_EQ(bad_interp, 0) << table << ": the interpreter is scored at 0 by the macro file";
	EXPECT_EQ(bad_jit, total);
}

} // namespace

TEST(VuMicroFmacConsole, FmacRangeMatchesConsole)
{
	Score(SatCases(), kMicroBadSat, std::size(kMicroBadSat), kMicroBadSatTotal, "vusat");
}

TEST(VuMicroFmacConsole, UnderflowMatchesConsole)
{
	Score(FlowCases(), kMicroBadFlow, std::size(kMicroBadFlow), kMicroBadFlowTotal, "vuflow");
}

// The other side of the gates. Every exact model microVU has for this FMAC is
// on one of two modes -- the multiply's U at 3, and MAC O for all three
// families, the adder's guard mask and the saturation ceiling at 4 -- so both
// tables have to get worse below the top one, and the vusat table has to get
// worse again below 3. It is also the check that the interpreter column above
// is the interpreter's: it does not move with the mode, and the recompiler's
// does.
TEST(VuMicroFmacConsole, ModesBelowFourMissMore)
{
	for (int mode = 1; mode <= 3; ++mode)
	{
		const std::pair<std::vector<Case>, const int*> tables[] = {
			{SatCases(), kMicroBadSatBelow},
			{FlowCases(), kMicroBadFlowBelow},
		};
		const int at_top[] = {kMicroBadSatTotal, kMicroBadFlowTotal};
		for (int t = 0; t < 2; ++t)
		{
			SCOPED_TRACE(::testing::Message() << "vuClampMode " << mode << " table " << t);
			int bad_jit = 0, bad_interp = 0;
			for (const Case& c : tables[t].first)
			{
				const Pair p = RunCase(0, c, mode);
				bad_jit += PopCount(Misses(c, p.jit));
				bad_interp += PopCount(Misses(c, p.interp));
			}
			std::printf("vuClampMode %d table %d: %d jit column misses\n", mode, t, bad_jit);
			EXPECT_EQ(bad_interp, 0);
			EXPECT_EQ(bad_jit, tables[t].second[mode - 1]);
			// Strictly worse at every mode below the gate: both tables see the
			// multiply's U, and the models are all on the one rung.
			EXPECT_GT(bad_jit, at_top[t]) << "the gate has stopped separating the modes";
		}
	}
}

// One FMAC for the two VUs: the models read mVU.index, so a VU1 microprogram at
// the scored mode has to land on VU0's word and flags. Nothing has measured VU1
// against silicon, so this is an equality between the two emitted programs and
// not a console claim.
TEST(VuMicroFmacConsole, BothVusAnswerAlike)
{
	for (const std::vector<Case>& table : {SatCases(), FlowCases()})
	{
		for (size_t i = 0; i < table.size(); ++i)
		{
			const Case& c = table[i];
			const Pair vu0 = RunCase(0, c, kScoredMode);
			const Pair vu1 = RunCase(1, c, kScoredMode);
			SCOPED_TRACE(::testing::Message() << "case " << i << ": " << c.what);
			for (int lane = 0; lane < 4; ++lane)
				EXPECT_EQ(vu1.jit.out[lane], vu0.jit.out[lane]) << "lane " << lane;
			EXPECT_EQ(vu1.jit.mac, vu0.jit.mac);
			EXPECT_EQ(vu1.jit.stat, vu0.jit.stat);
		}
	}
}

// Regenerates the two known-bad tables, and prints every row microVU and the
// COP2 macro emitter answer differently -- the rows the next hardware probe
// would have to settle.
TEST(VuMicroFmacConsole, DISABLED_DumpConsoleComparison)
{
	struct Table { const char* name; std::vector<Case> cases; const u8* macro; };
	std::vector<u8> macro_sat, macro_flow;
	for (const console_vusat::VuSatCase& c : console_vusat::kVuSatCases)
		macro_sat.push_back(c.bad_jit);
	for (const console_vuflow::VuFlowCase& c : console_vuflow::kVuFlowCases)
		macro_flow.push_back(c.bad_jit);

	const Table tables[] = {
		{"kMicroBadSat", SatCases(), macro_sat.data()},
		{"kMicroBadFlow", FlowCases(), macro_flow.data()},
	};
	for (const Table& t : tables)
	{
		std::vector<u8> mj(t.cases.size());
		std::vector<Pair> got;
		int bad_jit = 0, bad_interp = 0, differs = 0;
		for (size_t i = 0; i < t.cases.size(); ++i)
		{
			got.push_back(RunCase(0, t.cases[i], kScoredMode));
			mj[i] = Misses(t.cases[i], got[i].jit);
			bad_jit += PopCount(mj[i]);
			bad_interp += PopCount(Misses(t.cases[i], got[i].interp));
			differs += mj[i] != t.macro[i];
		}

		std::printf("constexpr u8 %s[] = {\n\t", t.name);
		for (size_t i = 0; i < mj.size(); ++i)
			std::printf("%u,%s", mj[i], (i % 16 == 15) ? "\n\t" : " ");
		std::printf("\n};\n");
		std::printf("// %s: jit %d column-misses, interp %d, %d of %zu rows unlike "
					"the macro path\n", t.name, bad_jit, bad_interp, differs, t.cases.size());
		for (size_t i = 0; i < mj.size(); ++i)
		{
			if (mj[i] != t.macro[i])
				std::printf("//   %2zu micro %u macro %u | micro out %08X mac %04X stat %04X"
							" | console %08X %04X %04X | %s\n",
					i, mj[i], t.macro[i], got[i].jit.out[0], got[i].jit.mac, got[i].jit.stat,
					t.cases[i].out, t.cases[i].mac, t.cases[i].stat, t.cases[i].what);
		}
	}
}

} // namespace recompiler_tests
