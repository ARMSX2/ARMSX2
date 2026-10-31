// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// MADD, MADDU, MADD1 and MADDU1, JIT vs interpreter.

#include "harness/EeRecTestHarness.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace recompiler_tests;
using namespace mips;
namespace e = mips::ee;

namespace {

enum Op : u32 { kMadd, kMaddu, kMadd1, kMaddu1, kOpCount };

const char* const kOpName[] = {"MADD", "MADDU", "MADD1", "MADDU1"};

u32 Encode(u32 op, u32 rd, u32 rs, u32 rt)
{
	switch (op)
	{
		case kMadd: return e::MADD(rd, rs, rt);
		case kMaddu: return e::MADDU(rd, rs, rt);
		case kMadd1: return e::MMI(rs, rt, rd, 0, 0x20);
		default: return e::MMI(rs, rt, rd, 0, 0x21);
	}
}

struct Lcg
{
	u64 s;
	u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 33); }
	u64 next64() { return (static_cast<u64>(next()) << 32) | next(); }
};

// The accumulator is HI.UL[0]:LO.UL[0] (UL[2] for the 1 forms); the other
// words of LO and HI are seeded with garbage and must come out unchanged.
struct Acc
{
	u32 lo, hi;
};

const Acc kAccs[] = {
	{0, 0},
	{0xFFFFFFFFu, 0x7FFFFFFFu}, // carry out of LO into HI, HI wraps
	{0x80000000u, 0x80000000u},
	{0x12345678u, 0xFFFFFFFFu},
};

const u32 kVals[] = {0, 1, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000001u, 0x9E3779B9u};

// Operand register pairs: pinned and unpinned homes, and rs == rt.
struct Regs
{
	u32 rs, rt;
};

const Regs kRegs[] = {
	{reg::a0, reg::a1},
	{reg::t0, reg::t1},
	{reg::a0, reg::t1},
	{reg::t0, reg::t0},
	{reg::a0, reg::a0},
};

} // namespace

TEST(EeRecMadd, EveryFormAliasAndAccumulator)
{
	u32 runs = 0;
	for (u32 op = 0; op < kOpCount; op++)
	for (const Regs& rr : kRegs)
	for (u32 rd_kind = 0; rd_kind < 4; rd_kind++)
	for (u32 vs : kVals)
	for (u32 vt : kVals)
	for (const Acc& acc : kAccs)
	{
		if (rr.rs == rr.rt && vs != vt)
			continue;
		Lcg r{(static_cast<u64>(op) << 40) ^ (static_cast<u64>(vs) << 8) ^ vt ^ (static_cast<u64>(acc.lo) << 20) ^ rd_kind};
		const u32 rd = rd_kind == 0 ? 0 : rd_kind == 1 ? rr.rs : rd_kind == 2 ? rr.rt : (vs & 1) ? reg::v1 : reg::t5;
		const bool upper = op == kMadd1 || op == kMaddu1;

		EeRecTestHarness h;
		h.SetGpr128(rr.rs, (r.next64() & ~0xFFFFFFFFull) | vs, r.next64());
		if (rr.rt != rr.rs)
			h.SetGpr128(rr.rt, (r.next64() & ~0xFFFFFFFFull) | vt, r.next64());
		if (rd != rr.rs && rd != rr.rt && rd)
			h.SetGpr128(rd, r.next64(), r.next64());
		const u64 lo_word = (r.next64() & ~0xFFFFFFFFull) | acc.lo;
		const u64 hi_word = (r.next64() & ~0xFFFFFFFFull) | acc.hi;
		const u64 lo_other = r.next64(), hi_other = r.next64();
		h.SetLoPair(upper ? lo_other : lo_word, upper ? lo_word : lo_other);
		h.SetHiPair(upper ? hi_other : hi_word, upper ? hi_word : hi_other);

		// A second MADD accumulates onto the first's result.
		std::vector<u32> prog = {Encode(op, rd, rr.rs, rr.rt), Encode(op, rd, rr.rs, rr.rt)};
		if (rd)
			prog.push_back(e::DADDU(reg::t7, rd, reg::zero));
		h.LoadProgram(prog);
		h.Run();
		runs++;
		if (::testing::Test::HasFailure())
		{
			ADD_FAILURE() << kOpName[op] << " rd=" << rd << " rs=" << rr.rs << " rt=" << rr.rt << std::hex
						  << " vs=0x" << vs << " vt=0x" << vt << " lo=0x" << acc.lo << " hi=0x" << acc.hi;
			return;
		}
	}
	EXPECT_EQ(runs, 4u * 4u * (3u * 7u * 7u + 2u * 7u) * 4u);
}

TEST(EeRecMadd, OneConstantOperand)
{
	for (u32 op = 0; op < kOpCount; op++)
	for (u32 const_in_rs = 0; const_in_rs < 2; const_in_rs++)
	for (u32 c : {0u, 1u, 8u, 0x80000000u, 0xFFFFFFFFu, 0x12345u})
	for (u32 v : kVals)
	{
		EeRecTestHarness h;
		h.SetGpr64(reg::a0, 0xA5A5A5A500000000ull | v);
		h.SetLoPair(0x11111111FFFFFFF0ull, 0x22222222FFFFFFF0ull);
		h.SetHiPair(0x333333337FFFFFFFull, 0x444444447FFFFFFFull);
		std::vector<u32> prog;
		if (c >> 16)
		{
			prog.push_back(LUI(reg::t0, static_cast<u16>(c >> 16)));
			prog.push_back(ORI(reg::t0, reg::t0, static_cast<u16>(c)));
		}
		else
			prog.push_back(ORI(reg::t0, reg::zero, static_cast<u16>(c)));
		const u32 rs = const_in_rs ? reg::t0 : reg::a0;
		const u32 rt = const_in_rs ? reg::a0 : reg::t0;
		prog.push_back(Encode(op, reg::v1, rs, rt));
		h.LoadProgram(prog);
		h.Run();
		if (::testing::Test::HasFailure())
		{
			ADD_FAILURE() << kOpName[op] << std::hex << " c=0x" << c << " v=0x" << v << " const_in_rs=" << const_in_rs;
			return;
		}
	}
}
