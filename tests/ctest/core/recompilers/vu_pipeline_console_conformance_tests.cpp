// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU pipeline visibility against a first-party console capture: how far
// downstream a Q (divide unit), a P (elementary function unit) and a flag
// result is actually readable, and whether an in-flight result still lands
// when the microprogram ends inside the latency window.
//
// Nothing in this area had ever been checked against silicon.  Every number
// came from documentation or from upstream, and the two engines commit results
// at structurally different moments -- ours when the latency expires, the
// interpreter's when the corresponding pipe flushes.
//
// What the console established:
//
//   1. Divide unit: DIV 7, SQRT 7, RSQRT 13.  Elementary function unit:
//      ESADD 11, ESUM/ERCPR/ESQRT 12, ERSADD/ELENG/ERSQRT 18, ERLENG 24,
//      ESIN 29, EEXP 44, EATAN/EATANxy/EATANxz 54.  Every one of the sixteen
//      is exactly the number we already modelled.
//   2. FMAC result latency is 4.  Not value-observable -- VF reads interlock
//      -- so it was measured by timing 2000 iterations of eight multiplies at
//      dependency distances 1..8; the extra-cycles row (21, 6, 2, 0, 0, 0, 0)
//      is reproduced by exactly one latency under in-order issue, which also
//      pins the COP0-tick-to-VU-cycle ratio at 1:1.  It is here as the reason
//      distances 4 and up behave alike; no value diff can assert it.
//   3. The divide unit's cause bits commit at distance 7, the same instant as
//      the quotient, both when set (a divide by zero) and when cleared (a
//      clean divide after one).  There is no side-latch-then-status-word
//      split: FSAND at 6 sees nothing and at 7 sees the cause bit and its
//      sticky twin together.
//   4. The FMAC's flags commit at distance 4, the same instant as its result.
//   5. An in-flight result always lands.  Q, the status word and P all hold
//      the new value even when the E bit is on the producer's own pair.
//   6. VU0 has no elementary function unit.  ESADD/ELENG/ESQRT on VU0 leave P
//      untouched, and the MFP encoding does something else entirely.

#include <gtest/gtest.h>

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "autocases_vulat.h"
#include "vulat_console.h"

#include "VU.h"

#include <cstdio>
#include <string>
#include <vector>

namespace recompiler_tests
{
namespace
{
using namespace vulat_common;
using vulat::Case;
using vulat::kCases;
using vulat::kNumCases;
using vu::VuOp;

constexpr u32 kVu1ResQw = vulat::kVu1SampQw;

// ---------------------------------------------------------------------------
//  Program builders -- mirrors of gen_vl.py, pinned by the FNV check in
//  vulat_console.h's PrepareConsoleCase
// ---------------------------------------------------------------------------

// Sampler j reads Q at distance first_dist + j - 1 into vf(j). DIV is a lower
// op and ADDq an upper one, so distance 0 is expressible: the reader shares
// the producer's pair.
std::vector<VuOp> QLatencyProgram(u32 old_lower, u32 new_lower, u32 first_dist)
{
	std::vector<VuOp> p;
	p.push_back(VuOp{old_lower, vu::VNOP_U()});
	PushNops(p, kSettle);

	u32 first_sampler = 1;
	if (first_dist == 0)
	{
		p.push_back(VuOp{new_lower, vu::VADDq_U(vu::mask::x, 1, 0)});
		first_sampler = 2;
	}
	else
	{
		p.push_back(VuOp{new_lower, vu::VNOP_U()});
		PushNops(p, first_dist - 1);
	}
	for (u32 j = first_sampler; j <= kNSamp; ++j)
		p.push_back(VuOp{vu::VIADDIU_L(0, 0, 0), vu::VADDq_U(vu::mask::x, j, 0)});
	PushETail(p);
	return p;
}

// MFP and the EFU ops are both lower, so the minimum distance is 1. The
// samplers are stored to VU1 memory at the end because VU1's register file is
// not EE-readable and the console had to see them somehow.
std::vector<VuOp> PLatencyProgram(u32 old_lower, u32 new_lower, u32 first_dist)
{
	std::vector<VuOp> p = Vu1Preamble();
	p.push_back(VuOp{old_lower, vu::VNOP_U()});
	PushNops(p, kSettle);
	p.push_back(VuOp{new_lower, vu::VNOP_U()});
	PushNops(p, first_dist - 1);
	for (u32 j = 1; j <= kNSamp; ++j)
		p.push_back(VuOp{vu::VMFP_L(vu::mask::x, j), vu::VNOP_U()});
	for (u32 n = 1; n < 32; ++n)
		p.push_back(VuOp{vu::VSQ_L(vu::mask::xyzw, n, 0, static_cast<s16>(vulat::kVu1SampQw + n)), vu::VNOP_U()});
	PushETail(p);
	return p;
}

std::vector<VuOp> FlagLatencyProgram(VuOp old_pair, VuOp new_pair, u32 first_dist,
                                     bool fmand, u32 nsamp)
{
	std::vector<VuOp> p;
	p.push_back(VuOp{vu::VFSSET_L(0), vu::VNOP_U()});
	PushNops(p, 8);
	p.push_back(old_pair);
	PushNops(p, kSettle);
	p.push_back(new_pair);
	PushNops(p, first_dist - 1);
	for (u32 j = 1; j <= nsamp; ++j)
		p.push_back(VuOp{fmand ? vu::VFMAND_L(j, 15) : vu::VFSAND_L(j, 0xFFF), vu::VNOP_U()});
	PushETail(p);
	return p;
}

// The old producer and the settle are here so "it did not land" is a value
// this program put there, not whatever the previous case left in Q or P.
std::vector<VuOp> TruncProgram(u32 old_lower, u32 new_lower, u32 dist, bool vu1)
{
	std::vector<VuOp> p = vu1 ? Vu1Preamble() : std::vector<VuOp>{};
	p.push_back(VuOp{old_lower, vu::VNOP_U()});
	PushNops(p, kSettle);
	p.push_back(VuOp{new_lower, dist == 0 ? (vu::VNOP_U() | vu::bits::E) : vu::VNOP_U()});
	if (dist > 0)
	{
		PushNops(p, dist - 1);
		p.push_back(vu::EBit(LNop()));
	}
	p.push_back(LNop());
	return p;
}

std::vector<VuOp> PReadProgram()
{
	std::vector<VuOp> p;
	p.push_back(VuOp{vu::VMFP_L(vu::mask::xyzw, 1), vu::VNOP_U()});
	PushNops(p, 6);
	p.push_back(VuOp{vu::VSQ_L(vu::mask::xyzw, 1, 0, static_cast<s16>(kVu1ResQw)), vu::VNOP_U()});
	PushNops(p, 4);
	PushETail(p);
	return p;
}

std::vector<VuOp> Vu0EfuProgram(u32 efu_lower)
{
	std::vector<VuOp> p;
	p.push_back(VuOp{efu_lower, vu::VNOP_U()});
	PushNops(p, kSettle);
	p.push_back(VuOp{vu::VMFP_L(vu::mask::x, 1), vu::VNOP_U()});
	PushNops(p, 8);
	PushETail(p);
	return p;
}

// ---------------------------------------------------------------------------
//  Producer encodings
// ---------------------------------------------------------------------------

// vf4 = (2.0, 8.0, 16.0, 1.0), vf5 = (1.0, 1.0, 1.0, 0.0) -- so vf5.w is the
// zero divisor and vf0.x the zero dividend.
constexpr u32 kDivOk = vu::VDIV_L(vu::vf::vf4, 0, vu::vf::vf5, 0);   // 2.0 / 1.0
constexpr u32 kDivNew = vu::VDIV_L(vu::vf::vf4, 1, vu::vf::vf5, 1);  // 8.0 / 1.0
constexpr u32 kDivZero = vu::VDIV_L(vu::vf::vf4, 0, vu::vf::vf5, 3); // 2.0 / 0.0 -> D
constexpr u32 kDivZeroZero = vu::VDIV_L(vu::vf::vf0, 0, vu::vf::vf5, 3); // 0/0 -> I

// (4, 64, 256, 1): no lane zero, so MAC stays clear.
constexpr u32 kMulNonzero = vu::VMUL_U(vu::mask::xyzw, 20, vu::vf::vf4, vu::vf::vf4);
// (1, 1, 1, 0): the w lane is zero, so MAC picks up one Z bit.
constexpr u32 kMulZero = vu::VMUL_U(vu::mask::xyzw, 21, vu::vf::vf5, vu::vf::vf5);

u32 EfuFsf(const std::string& op)
{
	if (op == "ESQRT" || op == "ERSQRT")
		return 3; // w
	if (op == "ESIN" || op == "EATAN" || op == "EEXP")
		return 2; // z
	return 0;     // x, or unused
}

u32 EfuEncode(const std::string& op, u32 fs)
{
	const u32 fsf = EfuFsf(op);
	if (op == "ESADD")   return vu::VESADD_L(fs);
	if (op == "EATANxy") return vu::VEATANXY_L(fs);
	if (op == "ESQRT")   return vu::VESQRT_L(fs, fsf);
	if (op == "ESIN")    return vu::VESIN_L(fs, fsf);
	if (op == "ERSADD")  return vu::VERSADD_L(fs);
	if (op == "EATANxz") return vu::VEATANXZ_L(fs);
	if (op == "ERSQRT")  return vu::VERSQRT_L(fs, fsf);
	if (op == "EATAN")   return vu::VEATAN_L(fs, fsf);
	if (op == "ELENG")   return vu::VELENG_L(fs);
	if (op == "ESUM")    return vu::VESUM_L(fs);
	if (op == "ERCPR")   return vu::VERCPR_L(fs, fsf);
	if (op == "EEXP")    return vu::VEEXP_L(fs, fsf);
	if (op == "ERLENG")  return vu::VERLENG_L(fs);
	ADD_FAILURE() << "unknown EFU op " << op;
	return 0;
}

// "Q2_P_ESADD_W1" -> "ESADD"; "Q2_Q_DIV" -> "DIV"; "Q2_VU0EFU_ELENG" -> "ELENG".
std::string OpFromTag(const std::string& tag)
{
	const std::size_t last = tag.rfind('_');
	if (tag.compare(0, 5, "Q2_P_") == 0)
		return tag.substr(5, last - 5);
	return tag.substr(last + 1);
}

std::vector<VuOp> BuildProgram(const Case& c)
{
	const std::string tag(c.tag);
	switch (c.kind)
	{
		case vulat::kQLat:
		{
			const std::string op = OpFromTag(tag);
			if (op == "DIV")
				return QLatencyProgram(kDivOk, kDivNew, c.first_dist);
			if (op == "SQRT")
				return QLatencyProgram(vu::VSQRT_L(vu::vf::vf4, 3),
				                       vu::VSQRT_L(vu::vf::vf4, 2), c.first_dist);
			if (op == "RSQRT")
				return QLatencyProgram(vu::VRSQRT_L(vu::vf::vf4, 1, vu::vf::vf4, 3),
				                       vu::VRSQRT_L(vu::vf::vf4, 0, vu::vf::vf4, 2),
				                       c.first_dist);
			ADD_FAILURE() << "unknown Q op in " << tag;
			return {};
		}
		case vulat::kPLat:
		{
			const std::string op = OpFromTag(tag);
			return PLatencyProgram(EfuEncode(op, vu::vf::vf8), EfuEncode(op, vu::vf::vf9),
			                       c.first_dist);
		}
		case vulat::kVu0Efu:
			return Vu0EfuProgram(EfuEncode(OpFromTag(tag), vu::vf::vf9));
		case vulat::kFlag:
		{
			const VuOp nop{vu::VIADDIU_L(0, 0, 0), vu::VNOP_U()};
			if (tag.compare(0, 20, "Q2_F_DIVZERO_SET_W") == 0 ||
			    tag.rfind("Q2_F_DIVZERO_SET_W", 0) == 0)
				return FlagLatencyProgram(VuOp{kDivOk, vu::VNOP_U()},
				                          VuOp{kDivZero, vu::VNOP_U()},
				                          c.first_dist, false, 15);
			if (tag.rfind("Q2_F_DIVZERO_CLR_W", 0) == 0)
				return FlagLatencyProgram(VuOp{kDivZero, vu::VNOP_U()},
				                          VuOp{kDivOk, vu::VNOP_U()},
				                          c.first_dist, false, 15);
			if (tag.rfind("Q2_F_INVALID_SET_W", 0) == 0)
				return FlagLatencyProgram(VuOp{kDivOk, vu::VNOP_U()},
				                          VuOp{kDivZeroZero, vu::VNOP_U()},
				                          c.first_dist, false, 15);
			if (tag.rfind("Q2_F_MAC_W", 0) == 0)
				return FlagLatencyProgram(VuOp{nop.lower, kMulNonzero},
				                          VuOp{nop.lower, kMulZero},
				                          c.first_dist, true, 14);
			if (tag.rfind("Q2_F_FMACSTATUS_W", 0) == 0)
				return FlagLatencyProgram(VuOp{nop.lower, kMulNonzero},
				                          VuOp{nop.lower, kMulZero},
				                          c.first_dist, false, 15);
			ADD_FAILURE() << "unknown flag case " << tag;
			return {};
		}
		case vulat::kTrunc:
		{
			const u32 d = static_cast<u32>(std::atoi(tag.substr(tag.rfind("_D") + 2).c_str()));
			if (tag.rfind("Q2_TRUNC_Q_D", 0) == 0)
				return TruncProgram(kDivOk, kDivNew, d, false);
			if (tag.rfind("Q2_TRUNC_STATUS_D", 0) == 0)
				return TruncProgram(kDivOk, kDivZero, d, false);
			if (tag.rfind("Q2_TRUNC_P_D", 0) == 0)
				return TruncProgram(vu::VESADD_L(vu::vf::vf8), vu::VESADD_L(vu::vf::vf9),
				                    d, true);
			ADD_FAILURE() << "unknown truncation case " << tag;
			return {};
		}
		case vulat::kPRead:
			return PReadProgram();
		case vulat::kSeed:
		{
			std::vector<VuOp> p;
			PushNops(p, 4);
			PushETail(p);
			return p;
		}
		default:
			return {};
	}
}

// ---------------------------------------------------------------------------
//  Case execution
// ---------------------------------------------------------------------------

bool RunConsoleCase(const Case& c, VuTestHarness& h)
{
	return vulat_common::RunConsoleCase(c, h, BuildProgram(c));
}

// Sampler k's value, per engine. VU0 reads it out of VF[k].x; VU1 reads it out
// of the data-memory slot the program stored it to.
u32 SampleJit(const Case& c, const VuTestHarness& h, u32 k)
{
	return c.vu ? h.GetMemU32Jit((vulat::kVu1SampQw + k) * 16)
	            : h.GetVfBitsJit(k, 'x');
}

u32 SampleInterp(const Case& c, const VuTestHarness& h, u32 k)
{
	return c.vu ? h.GetMemU32Interp((vulat::kVu1SampQw + k) * 16)
	            : h.GetVfBitsInterp(k, 'x');
}

// The distance at which a sampler run first shows something other than its
// first entry -- i.e. the latency, read the same way off hardware and off an
// engine.
int FirstChange(const u32* v, u32 n, u32 first_dist)
{
	for (u32 k = 1; k < n; ++k)
	{
		if (v[k] != v[0])
			return static_cast<int>(first_dist + k);
	}
	return -1;
}

int FirstChangeConsole(const Case& c)
{
	// sample[j] holds the reader at distance first_dist + j - 1, j = 1..31.
	return FirstChange(&c.sample[1], kNSamp, c.first_dist);
}

int FirstChangeEngine(const Case& c, const VuTestHarness& h, bool jit)
{
	u32 v[kNSamp];
	for (u32 k = 0; k < kNSamp; ++k)
		v[k] = jit ? SampleJit(c, h, k + 1) : SampleInterp(c, h, k + 1);
	return FirstChange(v, kNSamp, c.first_dist);
}

} // namespace

// ---------------------------------------------------------------------------
//  1. The latency numbers themselves
// ---------------------------------------------------------------------------

// Sanity floor: without this, a corpus that somehow lost its sampler values
// would let every latency test below pass by comparing nothing to nothing.
TEST(VuPipelineConsole, ConsoleCurvesActuallyMove)
{
	int moved = 0;
	for (const Case* c : CasesOfKind(vulat::kQLat))
	{
		EXPECT_GT(FirstChangeConsole(*c), 0) << c->tag;
		++moved;
	}
	for (const Case* c : CasesOfKind(vulat::kPLat))
	{
		if (FirstChangeConsole(*c) > 0)
			++moved;
	}
	EXPECT_GE(moved, 16) << "the capture has fewer moving latency curves than "
	                        "there are ops with a measured latency";
}

TEST(VuPipelineConsole, DivideUnitLatencyMatchesConsole)
{
	// DIV 7, SQRT 7, RSQRT 13 on silicon.
	for (const Case* c : CasesOfKind(vulat::kQLat))
	{
		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << c->tag;
		EXPECT_EQ(FirstChangeEngine(*c, h, true), FirstChangeConsole(*c))
			<< c->tag << " (arm64 recompiler)";
		EXPECT_EQ(FirstChangeEngine(*c, h, false), FirstChangeConsole(*c))
			<< c->tag << " (interpreter)";
	}
}

TEST(VuPipelineConsole, DivideUnitStaleAndSettledValuesMatchConsole)
{
	// Not just the transition distance: the value read before it must be the
	// previous quotient and the value after it the new one, on every sampler.
	for (const Case* c : CasesOfKind(vulat::kQLat))
	{
		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << c->tag;
		for (u32 k = 1; k <= kNSamp; ++k)
		{
			EXPECT_EQ(SampleJit(*c, h, k), c->sample[k])
				<< c->tag << " sampler at distance " << (c->first_dist + k - 1)
				<< " (arm64 recompiler)";
			EXPECT_EQ(SampleInterp(*c, h, k), c->sample[k])
				<< c->tag << " sampler at distance " << (c->first_dist + k - 1)
				<< " (interpreter)";
		}
	}
}

TEST(VuPipelineConsole, ElementaryFunctionUnitLatencyMatchesConsole)
{
	// All thirteen EFU latencies, on VU1 where the unit actually exists.
	for (const Case* c : CasesOfKind(vulat::kPLat))
	{
		VuTestHarness h(1);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << c->tag;
		EXPECT_EQ(FirstChangeEngine(*c, h, true), FirstChangeConsole(*c))
			<< c->tag << " (arm64 recompiler)";
		EXPECT_EQ(FirstChangeEngine(*c, h, false), FirstChangeConsole(*c))
			<< c->tag << " (interpreter)";
	}
}

// ---------------------------------------------------------------------------
//  2. Flag commit
// ---------------------------------------------------------------------------

TEST(VuPipelineConsole, DivideUnitFlagsCommitWithTheQuotient)
{
	// The cause bit and its sticky twin appear together, at distance 7 -- the
	// same instant the quotient becomes readable. No side-latch-first split.
	for (const char* tag : {"Q2_F_DIVZERO_SET_W1", "Q2_F_DIVZERO_CLR_W1",
	                        "Q2_F_INVALID_SET_W1"})
	{
		const Case& c = CaseByTag(tag);
		EXPECT_EQ(FirstChange(c.vi, 15, c.first_dist), 7)
			<< tag << ": the console capture itself";

		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(c, h)) << tag;
		u32 jit[15];
		for (u32 k = 0; k < 15; ++k)
			jit[k] = h.GetViJit(k + 1);
		EXPECT_EQ(FirstChange(jit, 15, c.first_dist), 7) << tag << " (arm64 recompiler)";
		// The interpreter is two pairs early on the sticky half — see
		// DISABLED_InterpreterPublishesStickyDivideFlagsEarly.
	}
}

TEST(VuPipelineConsole, FmacFlagsCommitWithTheResult)
{
	// MAC and the status word's Z field both appear at distance 4, which is
	// the FMAC result latency measured by timing.
	for (const char* tag : {"Q2_F_MAC_W1", "Q2_F_FMACSTATUS_W1"})
	{
		const Case& c = CaseByTag(tag);
		const u32 nsamp = (std::string(tag) == "Q2_F_MAC_W1") ? 14u : 15u;
		EXPECT_EQ(FirstChange(c.vi, nsamp, c.first_dist), 4)
			<< tag << ": the console capture itself";

		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(c, h)) << tag;
		u32 jit[15], interp[15];
		for (u32 k = 0; k < nsamp; ++k)
		{
			jit[k] = h.GetViJit(k + 1);
			interp[k] = h.GetViInterp(k + 1);
		}
		EXPECT_EQ(FirstChange(jit, nsamp, c.first_dist), 4) << tag << " (arm64 recompiler)";
		EXPECT_EQ(FirstChange(interp, nsamp, c.first_dist), 4) << tag << " (interpreter)";
	}
}

TEST(VuPipelineConsole, FlagSamplerValuesMatchConsole)
{
	// arm64 only; the interpreter's two divergent rows are pinned below.
	for (const Case* c : CasesOfKind(vulat::kFlag))
	{
		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << c->tag;
		const u32 nsamp = (std::string(c->tag).rfind("Q2_F_MAC_W", 0) == 0) ? 14u : 15u;
		for (u32 k = 0; k < nsamp; ++k)
		{
			EXPECT_EQ(h.GetViJit(k + 1), c->vi[k])
				<< c->tag << " sampler at distance " << (c->first_dist + k)
				<< " (arm64 recompiler)";
		}
	}
}

TEST(VuPipelineConsole, InterpreterFlagSamplersThatDoMatchConsole)
{
	// The interpreter's complement of the tripwire below: everything except
	// the two divide-unit set cases is right, and this is what stops a "fix"
	// for that one from quietly breaking the rest.
	for (const Case* c : CasesOfKind(vulat::kFlag))
	{
		const std::string tag(c->tag);
		if (tag == "Q2_F_DIVZERO_SET_W1" || tag == "Q2_F_INVALID_SET_W1")
			continue;
		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << tag;
		const u32 nsamp = (tag.rfind("Q2_F_MAC_W", 0) == 0) ? 14u : 15u;
		for (u32 k = 0; k < nsamp; ++k)
		{
			EXPECT_EQ(h.GetViInterp(k + 1), c->vi[k])
				<< tag << " sampler at distance " << (c->first_dist + k)
				<< " (interpreter)";
		}
	}
}

TEST(VuPipelineConsole, DISABLED_InterpreterPublishesStickyDivideFlagsEarly)
{
	// TRIPWIRE. Console and the arm64 recompiler publish a divide-unit cause
	// bit and its sticky twin together, at distance 7. The interpreter
	// publishes the sticky half at distance 5 and the current half at 7:
	//
	//   distance   1   2   3   4   5   6   7 ...
	//   console    000 000 000 000 000 000 820
	//   arm64      000 000 000 000 000 000 820
	//   interp     000 000 000 000 800 800 820
	//
	// Cause: `_vuRegsFSAND` (VUops.cpp) puts FSAND in the FMAC pipe, and
	// `_vuFMACflush` republishes the whole sticky field with
	// `(VU->fmac[i].statusflag & 0xFC0)` -- which includes the sticky copies
	// of D and I. `_vuDIV` sets those in `VU->statusflag` at issue (via
	// VU_STICKY_DI), so the sampler issued one pair after the divide snapshots
	// them and its own FMAC flush, four cycles later, publishes them at
	// distance 5. The comment directly above that line states the invariant
	// this breaks: "D/I are modified only by FDIV instructions". The FDIV pipe
	// then publishes the current bit at 7, correctly.
	//
	// Only the two set cases diverge; clearing a cause bit does not, because
	// the sticky field is never cleared by the clear.
	//
	// Not fixed here: the recompiler is already right and the interpreter's
	// flag pipeline is shared code.
	for (const char* tag : {"Q2_F_DIVZERO_SET_W1", "Q2_F_INVALID_SET_W1"})
	{
		const Case& c = CaseByTag(tag);
		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(c, h)) << tag;
		u32 interp[15];
		for (u32 k = 0; k < 15; ++k)
			interp[k] = h.GetViInterp(k + 1);
		EXPECT_EQ(FirstChange(interp, 15, c.first_dist), 7) << tag << " (interpreter)";
		for (u32 k = 0; k < 15; ++k)
		{
			EXPECT_EQ(interp[k], c.vi[k])
				<< tag << " sampler at distance " << (c.first_dist + k)
				<< " (interpreter)";
		}
	}
}

// ---------------------------------------------------------------------------
//  3. Ending the microprogram inside the latency window
// ---------------------------------------------------------------------------

TEST(VuPipelineConsole, InFlightDivideResultLandsWhenTheProgramEnds)
{
	// Console: Q holds the new quotient at every distance from 0 up, including
	// the E bit sitting on the DIV's own pair. The negative control is the
	// value the OLD divide left -- 2.0 -- which never appears.
	for (const Case* c : CasesOfKind(vulat::kTrunc))
	{
		const std::string tag(c->tag);
		if (tag.rfind("Q2_TRUNC_Q_D", 0) != 0)
			continue;
		EXPECT_EQ(c->q, 0x41000000u) << tag << ": the console capture itself";

		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << tag;
		EXPECT_EQ(h.GetViJit(REG_Q), c->q) << tag << " (arm64 recompiler)";
		EXPECT_EQ(h.GetViInterp(REG_Q), c->q) << tag << " (interpreter)";
	}
}

TEST(VuPipelineConsole, InFlightDivideFlagsLandWhenTheProgramEnds)
{
	for (const Case* c : CasesOfKind(vulat::kTrunc))
	{
		const std::string tag(c->tag);
		if (tag.rfind("Q2_TRUNC_STATUS_D", 0) != 0)
			continue;
		EXPECT_EQ(c->status, 0x820u) << tag << ": the console capture itself";

		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << tag;
		EXPECT_EQ(h.GetViJit(REG_STATUS_FLAG), c->status) << tag << " (arm64 recompiler)";
		EXPECT_EQ(h.GetViInterp(REG_STATUS_FLAG), c->status) << tag << " (interpreter)";
	}
}

TEST(VuPipelineConsole, InFlightEfuResultLandsWhenTheProgramEnds)
{
	// The console needed a second microprogram to see this, because VU1's P is
	// not EE-readable; the ground truth is that follower's captured value. In
	// the harness P is directly inspectable, so the follower is not re-run --
	// its console value is what the truncation case is scored against.
	for (const Case* c : CasesOfKind(vulat::kTrunc))
	{
		const std::string tag(c->tag);
		if (tag.rfind("Q2_TRUNC_P_D", 0) != 0)
			continue;
		const std::string dist = tag.substr(tag.rfind("_D") + 2);
		const Case& reader = CaseByTag(("Q2_TRUNC_PREAD_D" + dist).c_str());
		const u32 expected = reader.dump[0];
		EXPECT_EQ(expected, 0x41D00000u) << tag << ": the console capture itself";

		VuTestHarness h(1);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << tag;
		EXPECT_EQ(h.GetViJit(REG_P), expected) << tag << " (arm64 recompiler)";
		EXPECT_EQ(h.GetViInterp(REG_P), expected) << tag << " (interpreter)";
	}
}

// ---------------------------------------------------------------------------
//  4. VU0 has no elementary function unit
// ---------------------------------------------------------------------------

TEST(VuPipelineConsole, DISABLED_Vu0HasNoElementaryFunctionUnit)
{
	// TRIPWIRE. On hardware VU0 answers an EFU op with nothing: P stays put
	// and the MFP encoding does something else entirely (it delivered VU0 data
	// memory quadword 255, i.e. a decrementing load from the hardwired zero
	// index). Our interpreter dispatches the whole EFU family on VU0
	// (VUops.cpp:3250 VU0regsMI_ESADD onwards) and models a P register for it.
	//
	// Low impact: no VU0 microprogram in a shipped game can use an instruction
	// the assembler will not emit for VU0.
	for (const Case* c : CasesOfKind(vulat::kVu0Efu))
	{
		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << c->tag;
		EXPECT_EQ(h.GetViJit(REG_P), c->p) << c->tag << " (arm64 recompiler)";
		EXPECT_EQ(h.GetViInterp(REG_P), c->p) << c->tag << " (interpreter)";
		EXPECT_EQ(h.GetVfBitsJit(1, 'x'), c->sample[1]) << c->tag << " (arm64 recompiler)";
		EXPECT_EQ(h.GetVfBitsInterp(1, 'x'), c->sample[1]) << c->tag << " (interpreter)";
	}
}

// ---------------------------------------------------------------------------
//  5. Scoring
// ---------------------------------------------------------------------------

TEST(VuPipelineConsole, DISABLED_ScoreEnginesAgainstHardware)
{
	// Reporting tool, not an assertion. Prints how each engine scores against
	// the console row by row, the way the sticky/EFU/overflow captures did.
	//
	// Latency and value are scored separately: the question this capture was
	// built for is "at what distance does the new value appear", and both
	// engines get every one of those right. The EFU ops that then disagree on
	// the value itself are the pre-existing EFU alignment job, which already
	// has its own console column.
	u32 jit_lat = 0, interp_lat = 0, lat_scored = 0;
	u32 jit_ok = 0, interp_ok = 0, scored = 0;
	for (u32 i = 0; i < kNumCases; ++i)
	{
		const Case& c = kCases[i];
		if (c.kind != vulat::kQLat && c.kind != vulat::kPLat && c.kind != vulat::kFlag)
			continue;

		VuTestHarness h(c.vu);
		if (!RunConsoleCase(c, h))
			continue;

		bool jit_match = true, interp_match = true;
		if (c.kind == vulat::kFlag)
		{
			const u32 n = (std::string(c.tag).rfind("Q2_F_MAC_W", 0) == 0) ? 14u : 15u;
			for (u32 k = 0; k < n; ++k)
			{
				jit_match &= (h.GetViJit(k + 1) == c.vi[k]);
				interp_match &= (h.GetViInterp(k + 1) == c.vi[k]);
			}
		}
		else
		{
			for (u32 k = 1; k <= kNSamp; ++k)
			{
				jit_match &= (SampleJit(c, h, k) == c.sample[k]);
				interp_match &= (SampleInterp(c, h, k) == c.sample[k]);
			}
			const int want = FirstChangeConsole(c);
			++lat_scored;
			jit_lat += (FirstChangeEngine(c, h, true) == want);
			interp_lat += (FirstChangeEngine(c, h, false) == want);
		}
		++scored;
		jit_ok += jit_match;
		interp_ok += interp_match;
		std::printf("%-28s arm64 %s   interp %s\n", c.tag, jit_match ? "ok  " : "DIFF",
		            interp_match ? "ok  " : "DIFF");
		if (c.kind == vulat::kFlag && !(jit_match && interp_match))
		{
			const u32 n = (std::string(c.tag).rfind("Q2_F_MAC_W", 0) == 0) ? 14u : 15u;
			std::printf("    console:");
			for (u32 k = 0; k < n; ++k)
				std::printf(" %03X", c.vi[k]);
			std::printf("\n    arm64  :");
			for (u32 k = 0; k < n; ++k)
				std::printf(" %03X", h.GetViJit(k + 1));
			std::printf("\n    interp :");
			for (u32 k = 0; k < n; ++k)
				std::printf(" %03X", h.GetViInterp(k + 1));
			std::printf("\n");
		}
	}
	std::printf("\nlatency (the distance the new value appears at)\n"
	            "  arm64 recompiler %u/%u   interpreter %u/%u\n"
	            "value (every sampler bit-exact, flag rows included)\n"
	            "  arm64 recompiler %u/%u   interpreter %u/%u\n",
	            jit_lat, lat_scored, interp_lat, lat_scored,
	            jit_ok, scored, interp_ok, scored);
}

} // namespace recompiler_tests
