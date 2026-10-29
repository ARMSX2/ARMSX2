// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Does the end of a VU program wait for work still in flight?  When a program ends (E bit, then the
// delay-slot pair) the EE sees VPU_STAT busy clear a fixed time after the delay slot, or later if a
// divide or EFU result is still pending.  On the console (SCPH-30001, the 76 programs of
// autocases_vuenddrain.h) an FMAC and an ILW/ILWR never move that time, wherever they sit in the tail:
// their result lands at most four cycles after the delay slot, inside the fixed part.  microVU and
// the interpreter both stop their cycle count at the delay slot and match.
//
// A divide or EFU op is different: the console waits for it, but only past a fixed point (the end moves
// by max(0, L - p - 5) for a divide and max(0, L - p - 6) for an EFU op, L = latency).  The interpreter
// waits for all of L - p, microVU for none of it; neither is right.  That is a known divergence;
// InterpreterQAndPWaitIsUnchanged pins the interpreter's side so a change to the FMAC/ILW wait
// cannot move it by accident.
//
// The check is a difference: each engine's cycle count for a program minus its count for the empty
// tail (A_none) must equal the console's end for the program minus the console's end for A_none.  The
// one-cycle control is the same table: A_none with n extra NOP pairs moves the console's end by exactly
// n.  Skipping the wait changes only the cycle count: the registers and flags the work in flight
// would have written are still written at the end (CommittedStateIsTheSame...).

#include <gtest/gtest.h>

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuReplay.h"

#include "autocases_vuenddrain.h"

#include "VU.h"
#include "VUmicro.h"
#include "vu_capture.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace recompiler_tests
{
using namespace vu;
using console_vuenddrain::kVuEndDrainRowCount;
using console_vuenddrain::kVuEndDrainRows;
using console_vuenddrain::Op;
using console_vuenddrain::VuEndDrainRow;

namespace
{
struct Replay
{
	bool ok = false;
	u64 jit = 0;
	u64 interp = 0;
	bool diverged = false;
	std::string diff;
	u32 jitMac = 0;
	u32 interpMac = 0;
};

std::vector<VuOp> ProgramOf(const VuEndDrainRow& row)
{
	std::vector<VuOp> prog;
	for (u32 i = 0; i < row.pairs; ++i)
		prog.push_back(VuOp{row.words[i][0], row.words[i][1]});
	return prog;
}

Replay RunProgram(u32 vuIndex, const std::vector<VuOp>& prog)
{
	vu_capture::CaptureRecord rec;
	std::memset(&rec.state, 0, sizeof(rec.state));
	rec.state.VF[0][3] = 0x3F800000u; // vf0.w = 1.0
	rec.vu_index = static_cast<u8>(vuIndex);
	rec.start_pc = 0;
	rec.cycle_budget = 4096;
	rec.microcode.assign(vuIndex ? VU1_PROGSIZE : VU0_PROGSIZE, 0);
	size_t off = 0;
	for (const VuOp& op : prog)
	{
		std::memcpy(rec.microcode.data() + off, &op.lower, 4);
		std::memcpy(rec.microcode.data() + off + 4, &op.upper, 4);
		off += 8;
	}
	// Data memory as the probe left it: qw0 = (0.5, 2, 4, 8), qw1 = (-2, 0, 2, 2).
	const u32 data[8] = {0x3F000000u, 0x40000000u, 0x40800000u, 0x41000000u, 0xC0000000u, 0x00000000u, 0x40000000u, 0x40000000u};
	rec.vumem.assign(vuIndex ? VU1_MEMSIZE : VU0_MEMSIZE, 0);
	std::memcpy(rec.vumem.data(), data, sizeof(data));

	const VuReplayResult r = ReplayCapture(rec, VuDiffMode::PipelinePermissive, 0);
	Replay out;
	out.ok = r.ok && r.jit_ebit && r.interp_ebit;
	out.jit = r.jit_cycles;
	out.interp = r.interp_cycles;
	out.diverged = r.diverged;
	for (const std::string& l : r.diff_lines)
		out.diff += "\n    " + l;
	out.jitMac = r.jit_snapshot.regs.VI[REG_MAC_FLAG].UL;
	out.interpMac = r.interp_snapshot.regs.VI[REG_MAC_FLAG].UL;
	return out;
}

const VuEndDrainRow* BaselineOf(u32 vu)
{
	for (int i = 0; i < kVuEndDrainRowCount; ++i)
		if (kVuEndDrainRows[i].vu == vu && kVuEndDrainRows[i].op == Op::None && kVuEndDrainRows[i].extra == 0)
			return &kVuEndDrainRows[i];
	return nullptr;
}

bool IsFmacOrIlw(Op op)
{
	return op == Op::Fmac || op == Op::Ilw || op == Op::FmacX4;
}

bool IsQOrP(Op op)
{
	return op == Op::Div || op == Op::Rsqrt || op == Op::Esqrt || op == Op::Ersqrt || op == Op::Eatan;
}

u32 LatencyOf(Op op)
{
	switch (op)
	{
		case Op::Div: return 7;
		case Op::Rsqrt: return 13;
		case Op::Esqrt: return 12;
		case Op::Ersqrt: return 18;
		case Op::Eatan: return 54;
		default: return 0;
	}
}

const char* OpName(Op op)
{
	switch (op)
	{
		case Op::None: return "none";
		case Op::Fmac: return "FMAC";
		case Op::Ilw: return "ILW";
		case Op::Div: return "DIV";
		case Op::Rsqrt: return "RSQRT";
		case Op::Esqrt: return "ESQRT";
		case Op::Ersqrt: return "ERSQRT";
		case Op::Eatan: return "EATAN";
		case Op::FmacX4: return "FMAC x4";
	}
	return "?";
}

// How far one engine's end moved relative to its own count for the empty tail, against how far the
// console's did, for every row whose operation is `select`ed.  One line per row that differs.
template <typename Select>
std::string EndMisses(bool jit, Select select)
{
	std::string out;
	for (u32 vu = 0; vu < 2; ++vu)
	{
		const VuEndDrainRow* base = BaselineOf(vu);
		const Replay b = RunProgram(vu, ProgramOf(*base));
		if (!b.ok)
			return "\n  the empty-tail baseline did not reach the E bit";
		const u64 baseCount = jit ? b.jit : b.interp;
		for (int i = 0; i < kVuEndDrainRowCount; ++i)
		{
			const VuEndDrainRow& row = kVuEndDrainRows[i];
			if (row.vu != vu || !select(row))
				continue;
			const Replay r = RunProgram(vu, ProgramOf(row));
			const s64 got = static_cast<s64>(jit ? r.jit : r.interp) - static_cast<s64>(baseCount);
			const s64 want = static_cast<s64>(row.consoleEnd) - static_cast<s64>(base->consoleEnd);
			if (!r.ok || got != want)
			{
				char buf[200];
				std::snprintf(buf, sizeof(buf), "\n  vu%u %s: the end moved by %lld, the console's by %lld%s", vu, row.name,
					static_cast<long long>(got), static_cast<long long>(want), r.ok ? "" : " (did not reach the E bit)");
				out += buf;
			}
		}
	}
	return out;
}

bool FmacIlwOrControl(const VuEndDrainRow& row)
{
	return row.op == Op::None || IsFmacOrIlw(row.op);
}
} // namespace

// The table itself: 47 VU1 programs and 29 VU0 programs, and what the console says about them.
TEST(VuEndDrainConsole, TheTableSaysFmacAndIlwNeverMoveTheEnd)
{
	EXPECT_EQ(kVuEndDrainRowCount, 76);
	for (u32 vu = 0; vu < 2; ++vu)
	{
		const VuEndDrainRow* base = BaselineOf(vu);
		ASSERT_NE(base, nullptr);
		for (int i = 0; i < kVuEndDrainRowCount; ++i)
		{
			const VuEndDrainRow& row = kVuEndDrainRows[i];
			if (row.vu != vu)
				continue;
			if (row.op == Op::None)
			{
				EXPECT_EQ(row.consoleEnd, base->consoleEnd + row.extra) << row.name;
			}
			else if (IsFmacOrIlw(row.op))
			{
				EXPECT_EQ(row.consoleEnd, base->consoleEnd) << row.name;
			}
			else
			{
				ASSERT_TRUE(IsQOrP(row.op)) << row.name;
				const u32 fixed = (row.op == Op::Div || row.op == Op::Rsqrt) ? 5u : 6u;
				const s32 over = static_cast<s32>(LatencyOf(row.op)) - row.p - static_cast<s32>(fixed);
				EXPECT_EQ(row.consoleEnd, base->consoleEnd + static_cast<u32>(over > 0 ? over : 0)) << row.name;
			}
		}
	}
}

TEST(VuEndDrainConsole, MicroVuEndDoesNotWaitForFmacOrIlw)
{
	const std::string bad = EndMisses(true, FmacIlwOrControl);
	EXPECT_TRUE(bad.empty()) << "microVU ends that differ from the console:" << bad;
}

// An FMAC or ILW still in flight when the program ends (p = 0..3 pairs before the delay slot)
// must not add to the interpreter's cycle count.
TEST(VuEndDrainConsole, InterpreterEndDoesNotWaitForFmacOrIlw)
{
	const std::string bad = EndMisses(false, FmacIlwOrControl);
	EXPECT_TRUE(bad.empty()) << "interpreter ends that differ from the console:" << bad;
}

// Skipping the wait must not drop the results.  The flags an FMAC in flight at the end would have
// written are still written (MUL vf31,vf3,vf1 on (-1, 0, 8, 16): MAC S.x and Z.y = 0x84), and the
// two engines finish in the same architectural state, for every program with an FMAC or ILW near
// the end.
TEST(VuEndDrainConsole, CommittedStateIsTheSameInBothEngines)
{
	for (int i = 0; i < kVuEndDrainRowCount; ++i)
	{
		const VuEndDrainRow& row = kVuEndDrainRows[i];
		if (!FmacIlwOrControl(row))
			continue;
		const Replay r = RunProgram(row.vu, ProgramOf(row));
		ASSERT_TRUE(r.ok) << row.name;
		EXPECT_FALSE(r.diverged) << "vu" << row.vu << " " << row.name << " ends in a different state:" << r.diff;
		if (row.op == Op::Fmac || row.op == Op::FmacX4)
		{
			EXPECT_EQ(r.interpMac & 0xFFFFu, 0x84u) << "vu" << row.vu << " " << row.name << " interpreter MAC flag";
			EXPECT_EQ(r.jitMac & 0xFFFFu, 0x84u) << "vu" << row.vu << " " << row.name << " microVU MAC flag";
		}
	}
}

// KNOWN DIVERGENCE, left unchanged.  The console's end moves by max(0, L - p - 5) for a divide and
// max(0, L - p - 6) for an EFU op; microVU adds nothing and the interpreter waits for the whole
// L - p.  This pins the interpreter's side.
TEST(VuEndDrainConsole, InterpreterQAndPWaitIsUnchanged)
{
	for (u32 vu = 0; vu < 2; ++vu)
	{
		const VuEndDrainRow* base = BaselineOf(vu);
		const Replay b = RunProgram(vu, ProgramOf(*base));
		ASSERT_TRUE(b.ok);
		int seen = 0;
		for (int i = 0; i < kVuEndDrainRowCount; ++i)
		{
			const VuEndDrainRow& row = kVuEndDrainRows[i];
			if (row.vu != vu || !IsQOrP(row.op))
				continue;
			const Replay r = RunProgram(vu, ProgramOf(row));
			ASSERT_TRUE(r.ok) << row.name;
			EXPECT_EQ(static_cast<s64>(r.interp) - static_cast<s64>(b.interp), static_cast<s64>(LatencyOf(row.op)) - row.p)
				<< "vu" << vu << " " << row.name << ": the interpreter's wait for " << OpName(row.op);
			++seen;
		}
		EXPECT_EQ(seen, vu ? 30 : 12);
	}
}

// An FMAC beside a pending divide or EFU op changes nothing: the end is the divide's or EFU op's alone.
// FMACs sit in the upper slot of the delay-slot pair and of the E-bit pair (p = 0 and 1).
namespace
{
std::vector<VuOp> WithFmacsAtTheEnd(const VuEndDrainRow& row)
{
	std::vector<VuOp> prog = ProgramOf(row);
	const u32 fmac = VMUL_U(mask::xyzw, 30, 3, 1);
	for (size_t from_end = 1; from_end <= 2; ++from_end)
	{
		VuOp& op = prog[prog.size() - from_end];
		op.upper = fmac | (op.upper & bits::E);
	}
	return prog;
}

std::string FmacBesideQOrPMisses(bool jit)
{
	std::string out;
	for (int i = 0; i < kVuEndDrainRowCount; ++i)
	{
		const VuEndDrainRow& row = kVuEndDrainRows[i];
		if (!IsQOrP(row.op))
			continue;
		const Replay alone = RunProgram(row.vu, ProgramOf(row));
		const Replay both = RunProgram(row.vu, WithFmacsAtTheEnd(row));
		const u64 a = jit ? alone.jit : alone.interp;
		const u64 b = jit ? both.jit : both.interp;
		if (!alone.ok || !both.ok || a != b)
		{
			char buf[200];
			std::snprintf(buf, sizeof(buf), "\n  vu%u %s: %llu cycles alone, %llu with two FMACs at the end", row.vu, row.name,
				static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
			out += buf;
		}
	}
	return out;
}
} // namespace

TEST(VuEndDrainConsole, MicroVuFmacBesideQOrPAddsNothing)
{
	const std::string bad = FmacBesideQOrPMisses(true);
	EXPECT_TRUE(bad.empty()) << "microVU cycle counts that changed when FMACs were added:" << bad;
}

// With a divide or EFU op that finishes before the FMACs do (DIV at p = 4 and 5), the interpreter's
// end must not be pushed out to the FMACs' completion.
TEST(VuEndDrainConsole, InterpreterFmacBesideQOrPAddsNothing)
{
	const std::string bad = FmacBesideQOrPMisses(false);
	EXPECT_TRUE(bad.empty()) << "interpreter cycle counts that changed when FMACs were added:" << bad;
}

} // namespace recompiler_tests
