// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the EE charges for the instructions whose charge was set from the
// SCPH-30001 timing round, other than the divides (ee_rec_divide_charge_tests.cpp
// holds those).
//
// Every guest instruction is charged a fixed amount from the opcode table
// (R5900OpcodeTables.cpp, in eighths of a cycle): the interpreter adds it per
// instruction, the recompiler sums it per block. A charge can only stand for
// one number per instruction, so the families pinned here are the ones whose
// console cost is the same whether or not the next instruction needs the
// result: their latency equals their issue interval. The comments in the table
// say what was measured. A change to one of these numbers is a timing change
// for every game that uses the instruction, not a test update.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EECycleRate.h"
#include "R5900.h"
#include "R5900OpcodeTables.h"

#include <gtest/gtest.h>

#include <vector>

using namespace recompiler_tests;
using namespace mips;

namespace {

// CP0 Config bit 18 (DIE, dual issue enable); the BIOS sets it.
constexpr u32 kConfigDie = 1u << 18;

u32 NopEighths() { return R5900::GetInstruction(NOP).cycles; }
u32 TailEighths() { return R5900::GetInstruction(JR(reg::ra)).cycles + NopEighths(); }

struct MeasuredOp
{
	const char* name;
	u32 code;
	u32 eighths; // the table charge
};

// Every MMI op the table charges as MMI_Default: the parallel ALU, shift, pack,
// compare and move ops. rs = a1, rt = a2, rd = t0; immediate shifts by 1.
const MeasuredOp kMmiAlu[] = {
	{"PLZCW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x00, 0x04), 1 * 8},
	{"PMFHL", ee::MMI(reg::a1, reg::a2, reg::t0, 0x00, 0x30), 1 * 8},
	{"PMTHL", ee::MMI(reg::a1, reg::a2, reg::t0, 0x00, 0x31), 1 * 8},
	{"PSLLH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x01, 0x34), 1 * 8},
	{"PSRLH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x01, 0x36), 1 * 8},
	{"PSRAH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x01, 0x37), 1 * 8},
	{"PSLLW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x01, 0x3c), 1 * 8},
	{"PSRLW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x01, 0x3e), 1 * 8},
	{"PSRAW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x01, 0x3f), 1 * 8},
	{"PADDW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x00, 0x08), 1 * 8},
	{"PADDH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x04, 0x08), 1 * 8},
	{"PADDB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x08, 0x08), 1 * 8},
	{"PADDSW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x10, 0x08), 1 * 8},
	{"PADDSH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x14, 0x08), 1 * 8},
	{"PADDSB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x18, 0x08), 1 * 8},
	{"PADDUW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x10, 0x28), 1 * 8},
	{"PADDUH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x14, 0x28), 1 * 8},
	{"PADDUB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x18, 0x28), 1 * 8},
	{"PSUBW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x01, 0x08), 1 * 8},
	{"PSUBH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x05, 0x08), 1 * 8},
	{"PSUBB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x09, 0x08), 1 * 8},
	{"PSUBSW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x11, 0x08), 1 * 8},
	{"PSUBSH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x15, 0x08), 1 * 8},
	{"PSUBSB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x19, 0x08), 1 * 8},
	{"PSUBUW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x11, 0x28), 1 * 8},
	{"PSUBUH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x15, 0x28), 1 * 8},
	{"PSUBUB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x19, 0x28), 1 * 8},
	{"PCGTW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x02, 0x08), 1 * 8},
	{"PMAXW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x03, 0x08), 1 * 8},
	{"PMAXH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x07, 0x08), 1 * 8},
	{"PCGTH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x06, 0x08), 1 * 8},
	{"PCGTB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x0a, 0x08), 1 * 8},
	{"PEXTLW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x12, 0x08), 1 * 8},
	{"PEXTLH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x16, 0x08), 1 * 8},
	{"PEXTLB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1a, 0x08), 1 * 8},
	{"PEXT5", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1e, 0x08), 1 * 8},
	{"PPACW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x13, 0x08), 1 * 8},
	{"PPACH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x17, 0x08), 1 * 8},
	{"PPACB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1b, 0x08), 1 * 8},
	{"PPAC5", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1f, 0x08), 1 * 8},
	{"PABSW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x01, 0x28), 1 * 8},
	{"PABSH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x05, 0x28), 1 * 8},
	{"PCEQW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x02, 0x28), 1 * 8},
	{"PMINW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x03, 0x28), 1 * 8},
	{"PMINH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x07, 0x28), 1 * 8},
	{"PADSBH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x04, 0x28), 1 * 8},
	{"PCEQH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x06, 0x28), 1 * 8},
	{"PCEQB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x0a, 0x28), 1 * 8},
	{"PEXTUW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x12, 0x28), 1 * 8},
	{"PEXTUH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x16, 0x28), 1 * 8},
	{"PEXTUB", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1a, 0x28), 1 * 8},
	{"PSLLVW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x02, 0x09), 1 * 8},
	{"PSRLVW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x03, 0x09), 1 * 8},
	{"QFSRV", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1b, 0x28), 1 * 8},
	{"PINTH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x0a, 0x09), 1 * 8},
	{"PCPYLD", ee::MMI(reg::a1, reg::a2, reg::t0, 0x0e, 0x09), 1 * 8},
	{"PAND", ee::MMI(reg::a1, reg::a2, reg::t0, 0x12, 0x09), 1 * 8},
	{"PXOR", ee::MMI(reg::a1, reg::a2, reg::t0, 0x13, 0x09), 1 * 8},
	{"PEXEH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1a, 0x09), 1 * 8},
	{"PREVH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1b, 0x09), 1 * 8},
	{"PEXEW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1e, 0x09), 1 * 8},
	{"PROT3W", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1f, 0x09), 1 * 8},
	{"PSRAVW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x03, 0x29), 1 * 8},
	{"PMTHI", ee::MMI(reg::a1, reg::a2, reg::t0, 0x08, 0x29), 1 * 8},
	{"PMTLO", ee::MMI(reg::a1, reg::a2, reg::t0, 0x09, 0x29), 1 * 8},
	{"PINTEH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x0a, 0x29), 1 * 8},
	{"PCPYUD", ee::MMI(reg::a1, reg::a2, reg::t0, 0x0e, 0x29), 1 * 8},
	{"POR", ee::MMI(reg::a1, reg::a2, reg::t0, 0x12, 0x29), 1 * 8},
	{"PNOR", ee::MMI(reg::a1, reg::a2, reg::t0, 0x13, 0x29), 1 * 8},
	{"PEXCH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1a, 0x29), 1 * 8},
	{"PCPYH", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1b, 0x29), 1 * 8},
	{"PEXCW", ee::MMI(reg::a1, reg::a2, reg::t0, 0x1e, 0x29), 1 * 8},
};

// The 128-bit HI/LO reads.
const MeasuredOp kHiLoReads[] = {
	{"PMFHI", ee::PMFHI(reg::t0), 1 * 8},
	{"PMFLO", ee::PMFLO(reg::t0), 1 * 8},
};

// The FPU ops whose result goes to the accumulator (fs = f1, ft = f2).
const MeasuredOp kFpuAccumulator[] = {
	{"ADDA_S",  ee::COP1(0x10, 2, 1, 0, 0x18), 1 * 8},
	{"SUBA_S",  ee::COP1(0x10, 2, 1, 0, 0x19), 1 * 8},
	{"MULA_S",  ee::COP1(0x10, 2, 1, 0, 0x1A), 1 * 8},
	{"MADDA_S", ee::COP1(0x10, 2, 1, 0, 0x1E), 1 * 8},
	{"MSUBA_S", ee::COP1(0x10, 2, 1, 0, 0x1F), 1 * 8},
};

// COP0 reads (two registers whose value both engines agree on), DI and TLBP.
const MeasuredOp kCop0[] = {
	{"MFC0", MFC0(reg::t0, 15), 1 * 8}, // PRId
	{"MFC0", MFC0(reg::t0, 12), 1 * 8}, // Status
	{"DI",   ee::DI,            1 * 8},
	{"TLBP", (0x10u << 26) | (0x10u << 21) | 0x08u, 3 * 8},
};

struct ScopedCycleRate
{
	s8 prev;
	explicit ScopedCycleRate(int selector)
		: prev(EmuConfig.Speedhacks.EECycleRate)
	{
		EmuConfig.Speedhacks.EECycleRate = static_cast<s8>(selector);
		EECycleRate::SyncToConfigured();
	}
	~ScopedCycleRate()
	{
		EmuConfig.Speedhacks.EECycleRate = prev;
		EECycleRate::SyncToConfigured();
	}
};

// The interpreter's wait-loop speedhack charges a repeated branch target twice
// (see ee_rec_divide_charge_tests.cpp); it is off here.
struct ScopedWaitLoopOff
{
	bool prev;
	ScopedWaitLoopOff() : prev(EmuConfig.Speedhacks.WaitLoop) { EmuConfig.Speedhacks.WaitLoop = false; }
	~ScopedWaitLoopOff() { EmuConfig.Speedhacks.WaitLoop = prev; }
};

// `copies` of the op, then NOPs until the raw charge (with the harness's JR +
// NOP tail) is a multiple of 32, so the interpreter's carried sub-cycle
// remainder cannot move its charge and both engines can be held to one number.
struct Block
{
	std::vector<u32> words;
	u32 raw;
};

Block MakeBlock(const MeasuredOp& op, int copies)
{
	Block b;
	u32 raw = copies * op.eighths + TailEighths();
	for (int i = 0; i < copies; i++)
		b.words.push_back(op.code);
	while (raw % 32 != 0)
	{
		b.words.push_back(NOP);
		raw += NopEighths();
	}
	b.raw = raw;
	return b;
}

// Runs the block twice and returns the second run's charges in both engines.
void RunBlock(const Block& b, u64* jit, u64* interp)
{
	const ScopedWaitLoopOff no_wait_loop;
	for (int pass = 0; pass < 2; pass++)
	{
		EeRecTestHarness h;
		h.EnableCop0();
		h.EnableCop1();
		h.SetCp0(16, kConfigDie);
		h.SetGpr64(reg::a1, 0x0001000200030004ull);
		h.SetGpr64(reg::a2, 0x0000000500000007ull);
		h.SetFprBits(1, 0x40800000u); // 4.0
		h.SetFprBits(2, 0x40800000u); // 4.0
		h.LoadProgram(b.words);
		h.Run();
		*jit = h.GetCycleDeltaJit();
		*interp = h.GetCycleDeltaInterp();
	}
}

// The table holds each op's measured charge, and both engines charge exactly
// that for one and for two copies, at the default selector with dual issue on.
template <size_t N>
void ExpectMeasuredCharges(const MeasuredOp (&ops)[N])
{
	ScopedCycleRate rate(0);
	for (const MeasuredOp& op : ops)
	{
		const R5900::OPCODE& entry = R5900::GetInstruction(op.code);
		EXPECT_STREQ(entry.Name, op.name);
		EXPECT_EQ(static_cast<u32>(entry.cycles), op.eighths) << op.name;
		for (int copies = 1; copies <= 2; copies++)
		{
			const Block b = MakeBlock(op, copies);
			u64 jit = 0, interp = 0;
			RunBlock(b, &jit, &interp);
			EXPECT_EQ(jit, b.raw / 8) << op.name << " x" << copies << " (jit)";
			EXPECT_EQ(interp, b.raw / 8) << op.name << " x" << copies << " (interp)";
		}
	}
}

} // namespace

// The console runs every one of these at one per cycle, dependent or not, and
// never two at once (nor beside an integer ALU op).
TEST(EeRecMeasuredCharge, MmiAluOpsChargeOneCycle)
{
	ExpectMeasuredCharges(kMmiAlu);
}

// The console reads HI or LO in 1 cycle; after a parallel multiply the read
// waits, and a PMULTH then PMFLO costs 4 per pair, which the multiply's own 3
// plus this 1 already makes.
TEST(EeRecMeasuredCharge, ParallelHiLoReadsChargeOneCycle)
{
	ExpectMeasuredCharges(kHiLoReads);
}

// The console accumulates into ACC at one per cycle: a MADDA.S chain costs 1
// per op, and MULA.S then a MADD.S that reads ACC costs 2 per pair.
TEST(EeRecMeasuredCharge, FpuAccumulatorWritesChargeOneCycle)
{
	ExpectMeasuredCharges(kFpuAccumulator);
}

// The console reads any COP0 register in 1 cycle (eight tried), and DI costs
// 1 and TLBP 3, independent or not.
TEST(EeRecMeasuredCharge, Cop0ReadsDiAndTlbpChargeAsMeasured)
{
	ExpectMeasuredCharges(kCop0);
}
