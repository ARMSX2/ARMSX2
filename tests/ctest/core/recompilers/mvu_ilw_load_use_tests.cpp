// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ILW and ILWR load their VI over four cycles. Any instruction that reads
// that VI waits until the load has finished: a reader N pairs after the load
// issues no earlier than the fourth cycle after it, so it stalls 3 - N cycles
// for N = 0..2 and not at all from N = 3. microVU takes the stall from the
// VI's load latency for every reader (analyzeVIreg1).
//
// autocases_vubranch_ilwuse.h holds console rows for IADDIU and LQI behind a
// branch. This file sweeps every instruction that reads a VI, on both VUs
// and for both loads, and compares the cycle count of each engine with the
// rule. The program is the load, N NOP pairs, the reader, a NOP pair for the
// reader's delay slot, eight NOP pairs so that nothing is in flight at the
// end, and the E-bit pair with its delay slot, so the cycle count is the
// pair count plus the stall.
//
// ILW into vi0 loads nothing (vi0 is wired to zero), so a reader of vi0 never
// waits; microVU treats it that way and the interpreter has to as well.

#include <gtest/gtest.h>

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuReplay.h"

#include "VU.h"
#include "VUmicro.h"
#include "vu_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace recompiler_tests
{
using namespace vu;

namespace
{
constexpr u32 kNopLower = 0x8000033cu;

VuOp NopP() { return VuOp{kNopLower, VNOP_U()}; }

struct Reader
{
	const char* name;
	u32 lower;
};

// Every lower instruction that reads the VI `v`, in each operand slot it can
// sit in. The branches are laid out so that they fall through or land on the
// pair after their delay slot; either way the pair count is the same.
std::vector<Reader> ReadersOf(u32 v)
{
	using namespace vu::mask;
	return {
		{"IADD Is", VIADD_L(2, v, 0)},
		{"IADD It", VIADD_L(2, 0, v)},
		{"ISUB Is", VISUB_L(2, v, 0)},
		{"IAND It", VIAND_L(2, 0, v)},
		{"IOR Is", VIOR_L(2, v, 0)},
		{"IADDI", VIADDI_L(2, v, 3)},
		{"IADDIU", VIADDIU_L(2, v, 5)},
		{"IADDIU vi,vi,1", VIADDIU_L(v ? v : 2, v, 1)},
		{"ISUBIU", VISUBIU_L(2, v, 5)},
		{"ILW", VILW_L(x, 2, v, 0)},
		{"ILWR", VILWR_L(x, 2, v)},
		{"ISW It", VISW_L(x, v, 0, 4)},
		{"ISW Is", VISW_L(x, 0, v, 4)},
		{"ISWR It", VISWR_L(x, v, 0)},
		{"ISWR Is", VISWR_L(x, 0, v)},
		{"LQ", VLQ_L(xyzw, 4, v, 0)},
		{"LQI", VLQI_L(xyzw, 4, v)},
		{"LQD", VLQD_L(xyzw, 4, v)},
		{"SQ", VSQ_L(xyzw, 4, v, 0)},
		{"SQI", VSQI_L(xyzw, 4, v)},
		{"SQD", VSQD_L(xyzw, 4, v)},
		{"MFIR", VMFIR_L(xyzw, 4, v)},
		{"FMEQ", VFMEQ_L(2, v)},
		{"FMAND", VFMAND_L(2, v)},
		{"FMOR", VFMOR_L(2, v)},
		{"IBEQ", VIBEQ_L(2, v, 1)},
		{"IBNE", VIBNE_L(0, v, 1)},
		{"IBLTZ", VIBLTZ_L(v, 1)},
		{"IBGTZ", VIBGTZ_L(v, 1)},
		{"IBLEZ", VIBLEZ_L(v, 1)},
		{"IBGEZ", VIBGEZ_L(v, 1)},
	};
}

struct Cycles
{
	u64 jit = 0;
	u64 interp = 0;
	bool ok = false;
};

// The load, `gap` NOP pairs, the reader, and the padding described above.
Cycles Run(u32 vuIndex, u32 load, u32 reader, u32 gap)
{
	std::vector<VuOp> prog;
	prog.push_back(VuOp{load, VNOP_U()});
	for (u32 i = 0; i < gap; ++i)
		prog.push_back(NopP());
	prog.push_back(VuOp{reader, VNOP_U()});
	prog.push_back(NopP());
	for (int i = 0; i < 8; ++i)
		prog.push_back(NopP());
	prog.push_back(EBitNopPair());
	prog.push_back(NopP());

	vu_capture::CaptureRecord rec;
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
	rec.vumem.assign(vuIndex ? VU1_MEMSIZE : VU0_MEMSIZE, 0);

	const VuReplayResult r = ReplayCapture(rec, VuDiffMode::PipelinePermissive, 0);
	Cycles c;
	c.ok = r.ok && r.jit_ebit && r.interp_ebit;
	c.jit = r.jit_cycles;
	c.interp = r.interp_cycles;
	return c;
}

// The sweep for one engine: the lines for every (VU, load, reader, gap) whose
// cycle count is not what the rule says. `loadsVi0` sets the VI the load
// targets and the reader reads to vi0.
std::string Misses(bool jit, bool loadsVi0)
{
	using namespace vu::mask;
	const u32 v = loadsVi0 ? 0 : 1;
	std::string out;
	for (u32 vuIndex = 0; vuIndex < 2; ++vuIndex)
	{
		for (int ilwr = 0; ilwr < 2; ++ilwr)
		{
			const u32 load = ilwr ? VILWR_L(x, v, 0) : VILW_L(x, v, 0, 0);
			for (const Reader& r : ReadersOf(v))
			{
				for (u32 gap = 0; gap <= 4; ++gap)
				{
					// 13 pairs without a gap or a stall: load, reader, delay
					// slot, eight fillers, E-bit pair and its delay slot.
					const u64 stall = loadsVi0 ? 0 : (gap < 3 ? 3 - gap : 0);
					const u64 want = 13 + gap + stall;
					const Cycles c = Run(vuIndex, load, r.lower, gap);
					const u64 got = jit ? c.jit : c.interp;
					if (!c.ok || got != want)
					{
						char buf[160];
						std::snprintf(buf, sizeof(buf), "\n  vu%u %s %s, %u pair(s) between: %llu cycles, want %llu%s", vuIndex,
							ilwr ? "ILWR" : "ILW", r.name, gap, static_cast<unsigned long long>(got),
							static_cast<unsigned long long>(want), c.ok ? "" : " (did not reach the E-bit)");
						out += buf;
					}
				}
			}
		}
	}
	return out;
}
} // namespace

TEST(MvuIlwLoadUse, MicroVuReaderWaitsForTheLoad)
{
	const std::string bad = Misses(true, false);
	EXPECT_TRUE(bad.empty()) << "microVU cycle counts that differ from the rule:" << bad;
}

TEST(MvuIlwLoadUse, MicroVuReaderOfVi0NeverWaits)
{
	const std::string bad = Misses(true, true);
	EXPECT_TRUE(bad.empty()) << "microVU cycle counts that differ from the rule:" << bad;
}

// The interpreter used to stall only a branch on the loading VI, so every
// other reader issued too early.
TEST(MvuIlwLoadUse, InterpreterReaderWaitsForTheLoad)
{
	const std::string bad = Misses(false, false);
	EXPECT_TRUE(bad.empty()) << "interpreter cycle counts that differ from the rule:" << bad;
}

TEST(MvuIlwLoadUse, InterpreterReaderOfVi0NeverWaits)
{
	const std::string bad = Misses(false, true);
	EXPECT_TRUE(bad.empty()) << "interpreter cycle counts that differ from the rule:" << bad;
}

} // namespace recompiler_tests
