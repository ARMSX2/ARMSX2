// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// MULT, MULTU, MULT1 and MULTU1 with one operand a compile-time constant,
// JIT vs interpreter.

#include "harness/EeRecTestHarness.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace recompiler_tests;
using namespace mips;
namespace e = mips::ee;

extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);

namespace {

constexpr u32 kPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kData = RecompilerTestEnvironment::kScratchAddr;
constexpr u32 kWindowBytes = 128;

enum Op : u32 { kMult, kMultu, kMult1, kMultu1, kOpCount };

const char* const kOpName[] = {"MULT", "MULTU", "MULT1", "MULTU1"};

u32 Encode(u32 op, u32 rd, u32 rs, u32 rt)
{
	switch (op)
	{
		case kMult: return RType(0, rs, rt, rd, 0, 0x18);
		case kMultu: return RType(0, rs, rt, rd, 0, 0x19);
		case kMult1: return e::MULT1(rd, rs, rt);
		default: return e::MULTU1(rd, rs, rt);
	}
}

bool IsSigned(u32 op) { return op == kMult || op == kMult1; }

// The constants the JIT multiplies without a multiply instruction.
bool Folds(u32 op, u32 c)
{
	return c == 0 || (std::has_single_bit(c) && !(IsSigned(op) && c == 0x80000000u));
}

struct Lcg
{
	u64 s;
	u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 33); }
	u32 range(u32 n) { return next() % n; }
	u64 next64() { return (static_cast<u64>(next()) << 32) | next(); }
};

// SMADDL/UMADDL, of which SMULL/UMULL are the Ra = xzr forms.
u32 HostMulCount(u32 guest_pc)
{
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
	{
		ADD_FAILURE() << "no compiled block at 0x" << std::hex << guest_pc;
		return 0;
	}
	const u32* code = reinterpret_cast<const u32*>(fn);
	u32 n = 0;
	for (u32 i = 0; i < size / 4; i++)
		n += (code[i] & 0xFF608000u) == 0x9B200000u;
	return n;
}

// c into g. With `garbage`, g's upper word is garbage ^ sign(c), built in tmp.
void EmitConst(std::vector<u32>& p, u32 g, u32 c, u32 tmp, s16 garbage)
{
	const u32 dst = garbage ? tmp : g;
	if (c < 0x10000)
		p.push_back(ORI(dst, reg::zero, static_cast<u16>(c)));
	else
	{
		p.push_back(LUI(dst, static_cast<u16>(c >> 16)));
		if (c & 0xFFFF)
			p.push_back(ORI(dst, dst, static_cast<u16>(c)));
	}
	if (garbage)
	{
		p.push_back(e::DADDIU(g, reg::zero, garbage));
		p.push_back(e::DSLL32(g, g, 0));
		p.push_back(XOR(g, g, tmp));
	}
}

// Where the variable operand lives when the multiply reads it. All four
// leave garbage in its upper word.
enum Home : u32 { kSeeded, kLoaded, kQuad, kComputed, kHomeCount };

} // namespace

TEST(EeRecMultConstOperand, EveryConstantAliasAndOperandHome)
{
	// consts[0] is $zero itself.
	std::vector<u32> consts = {0, 0, 1};
	for (u32 k = 1; k < 32; k++)
		consts.push_back(1u << k);
	consts.insert(consts.end(), {0xFFFFFFFFu, 3u, 0x7FFFFFFFu});

	u32 folded = 0, generic = 0, runs = 0;
	u32 folded_with_mul = 0, generic_without_mul = 0;
	std::string first_wrong_path;
	for (u32 op = 0; op < kOpCount; op++)
	for (u32 const_in_rs = 0; const_in_rs < 2; const_in_rs++)
	for (u32 rd_kind = 0; rd_kind < 4; rd_kind++)
	for (u32 ci = 0; ci < consts.size(); ci++)
	for (u32 home = 0; home < kHomeCount; home++)
	for (u32 var_pinned = 0; var_pinned < 2; var_pinned++)
	{
		const u32 mix = home * 2 + var_pinned;
		Lcg r{(((op * 2 + const_in_rs) * 4 + rd_kind) * 64 + ci) * 8 + mix + 0x6D756C74ull};
		const u32 c = consts[ci];
		const u32 creg = ci == 0 ? reg::zero : (mix & 1) ? reg::s0 : reg::t0;
		const s16 cgarbage = (mix & 2) ? static_cast<s16>(r.next() | 1) : 0;
		const u32 var = var_pinned ? reg::a0 : reg::t1;
		const u32 rs = const_in_rs ? creg : var;
		const u32 rt = const_in_rs ? var : creg;
		const u32 rd = rd_kind == 0 ? 0 : rd_kind == 1 ? rs : rd_kind == 2 ? rt : (mix & 4) ? reg::v1 : reg::t5;

		std::vector<u32> prog;
		if (creg != reg::zero)
			EmitConst(prog, creg, c, reg::t3, cgarbage);
		switch (home)
		{
			case kLoaded: prog.push_back(e::LD(var, 0, reg::k0)); break;
			case kQuad: prog.push_back(e::LQ(var, 0, reg::k0)); break;
			case kComputed:
				prog.push_back(e::DSLL32(var, reg::a1, 0));
				prog.push_back(OR(var, var, reg::t2));
				break;
			default: break;
		}
		prog.push_back(Encode(op, rd, rs, rt));
		prog.push_back(e::DADDU(reg::t7, rd, reg::a2));
		prog.push_back(e::SQ(rd, 0x20, reg::k0));

		const u32 values[8] = {0, 1, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000001u, r.next(), r.next()};
		for (const u32 vi : {mix, (mix + 3) & 7})
		{
			std::ostringstream desc;
			desc << kOpName[op] << " rd=r" << rd << " rs=r" << rs << " rt=r" << rt << " const=0x" << std::hex << c
				 << " upper=0x" << cgarbage << std::dec << " home=" << home << " var=0x" << std::hex << values[vi];
			SCOPED_TRACE(desc.str());
			const u64 garbage = static_cast<u64>(r.next() | 0x10000u) << 32;
			const u64 v64 = garbage | values[vi];

			EeRecTestHarness h;
			for (u32 g : {reg::t0, reg::s0, reg::a0, reg::t1, reg::v1, reg::t5, reg::t3, reg::t7, reg::a2})
				h.SetGpr128(g, r.next64(), r.next64());
			h.SetGpr64(reg::k0, kData);
			h.SetGpr64(reg::a1, garbage >> 32);
			h.SetGpr128(reg::t2, values[vi], r.next64());
			if (home == kSeeded)
				h.SetGpr128(var, v64, r.next64());
			h.SetLoPair(r.next64(), r.next64());
			h.SetHiPair(r.next64(), r.next64());
			h.WriteU64(kData, v64);
			for (u32 i = 8; i < kWindowBytes; i += 8)
				h.WriteU64(kData + i, r.next64());
			h.TrackMemWindow(kData, kWindowBytes);
			h.LoadProgram(prog);
			h.Run();
			runs++;

			if (vi == mix)
			{
				const u32 muls = HostMulCount(kPc);
				const bool wrong = Folds(op, c) ? muls != 0 : muls != 1;
				(Folds(op, c) ? folded : generic)++;
				(Folds(op, c) ? folded_with_mul : generic_without_mul) += wrong;
				if (wrong && first_wrong_path.empty())
					first_wrong_path = desc.str();
			}
			if (::testing::Test::HasFailure())
				return;
		}
	}
	EXPECT_EQ(folded_with_mul, 0u) << "of " << folded << " folding programs; first: " << first_wrong_path;
	EXPECT_EQ(generic_without_mul, 0u) << "of " << generic << " generic programs; first: " << first_wrong_path;
	EXPECT_EQ(runs, 2u * kOpCount * 2 * 4 * consts.size() * kHomeCount * 2);
	EXPECT_EQ(folded + generic, runs / 2);
	EXPECT_EQ(generic, kOpCount * 2 * 4 * kHomeCount * 2 * 3 + 2 * 2 * 4 * kHomeCount * 2) << "MULT/MULT1 by INT_MIN is generic";
}

namespace {

constexpr u32 kPool[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                         16, 17, 18, 19, 20, 21, 22, 23, 24, 28, 29, 30};
constexpr u32 kPoolN = sizeof(kPool) / sizeof(kPool[0]);

enum Placement : u32 { kPlain, kDelaySlot, kAfterJump, kJumpDelaySlot, kPlacementCount };

struct Gen
{
	Lcg r;
	std::vector<u32> prog;
	u32 hits[kOpCount][kPlacementCount] = {};
	u32 lastConst = reg::zero;

	u32 anyReg() { return kPool[r.range(kPoolN)]; }

	u32 pickConst()
	{
		switch (r.range(8))
		{
			case 0: return 0;
			case 1: return 1;
			case 2: return 0x80000000u;
			case 3: return 0xFFFFFFFFu;
			case 4: return 0x7FFFFFFFu;
			case 5: return r.next();
			default: return 1u << (1 + r.range(31));
		}
	}

	void emitConst(u32 g)
	{
		u32 tmp;
		do
			tmp = anyReg();
		while (tmp == g);
		EmitConst(prog, g, pickConst(), tmp, r.range(2) ? static_cast<s16>(r.next() | 1) : 0);
		lastConst = g;
	}

	// A non-constant value with a garbage upper word.
	void emitVariable(u32 g)
	{
		switch (r.range(4))
		{
			case 0: prog.push_back(e::LD(g, static_cast<s16>(r.range(kWindowBytes / 8) * 8), reg::k0)); break;
			case 1: prog.push_back(e::LQ(g, static_cast<s16>(r.range(kWindowBytes / 16) * 16), reg::k0)); break;
			case 2: prog.push_back(e::DSLL32(g, anyReg(), r.range(32))); prog.push_back(OR(g, g, anyReg())); break;
			default: prog.push_back(e::DADDU(g, anyReg(), anyReg())); break;
		}
	}

	// `alone` places the site with nothing in front of it (a delay slot).
	void emitSite(u32 placement, bool alone)
	{
		const u32 op = r.range(kOpCount);
		hits[op][placement]++;
		u32 c = alone ? lastConst : anyReg(), v = anyReg();
		if (!alone)
		{
			if (r.range(3))
				emitConst(c);
			if (r.range(2))
				emitVariable(v);
		}
		u32 rs = c, rt = v;
		if (r.range(2))
			std::swap(rs, rt);
		u32 rd = 0;
		switch (r.range(5))
		{
			case 0: break;
			case 1: rd = rs; break;
			case 2: rd = rt; break;
			default: rd = anyReg(); break;
		}
		prog.push_back(Encode(op, rd, rs, rt));
	}

	void emitConsumer()
	{
		const u32 x = anyReg(), y = anyReg();
		switch (r.range(7))
		{
			case 0: prog.push_back(MFLO(x)); break;
			case 1: prog.push_back(MFHI(x)); break;
			case 2: prog.push_back(e::MFLO1(x)); break;
			case 3: prog.push_back(e::MFHI1(x)); break;
			case 4: prog.push_back(e::DADDU(x, y, anyReg())); break;
			case 5: prog.push_back(e::SQ(y, static_cast<s16>(r.range(kWindowBytes / 16) * 16), reg::k0)); break;
			default: break;
		}
	}

	void emitUnit()
	{
		switch (r.range(kPlacementCount))
		{
			case kPlain:
				emitSite(kPlain, false);
				break;
			case kDelaySlot:
			{
				emitConst(anyReg());
				const u32 a = anyReg(), b = anyReg(), kind = r.range(4);
				const size_t at = prog.size();
				prog.push_back(NOP);
				emitSite(kDelaySlot, true);
				for (u32 k = 1 + r.range(2); k > 0; k--)
					emitConst(anyReg()); // skipped when taken
				const s16 off = static_cast<s16>(prog.size() - (at + 1));
				switch (kind)
				{
					case 0: prog[at] = BEQ(a, b, off); break;
					case 1: prog[at] = BNE(a, b, off); break;
					case 2: prog[at] = e::BEQL(a, b, off); break;
					default: prog[at] = e::BNEL(a, b, off); break;
				}
				break;
			}
			case kAfterJump:
			{
				emitConst(anyReg());
				prog.push_back(J(kPc + static_cast<u32>(prog.size() + 2) * 4));
				prog.push_back(NOP);
				emitSite(kAfterJump, true);
				break;
			}
			default:
			{
				emitConst(anyReg());
				prog.push_back(J(kPc + static_cast<u32>(prog.size() + 2) * 4));
				emitSite(kJumpDelaySlot, true);
				break;
			}
		}
		emitConsumer();
	}
};

} // namespace

TEST(EeRecMultConstOperand, RandomStreams)
{
	const char* only_env = std::getenv("EEFUZZ_SEED");
	const u32 only = only_env ? static_cast<u32>(std::atoi(only_env)) : ~0u;
	const char* count_env = std::getenv("EEFUZZ_COUNT");
	const u32 count = count_env ? static_cast<u32>(std::strtoul(count_env, nullptr, 10)) : 2000;

	u32 hits[kOpCount][kPlacementCount] = {};
	for (u32 seed = 0; seed < count; ++seed)
	{
		if (only != ~0u && seed != only)
			continue;
		SCOPED_TRACE(::testing::Message() << "seed=" << seed);
		Gen g{{seed * 0x9E3779B97F4A7C15ull + 0x6D756C31ull}};
		EeRecTestHarness h;
		for (u32 i = 1; i < 32; ++i)
		{
			if (i != reg::k0 && i != reg::k1 && i != reg::ra)
				h.SetGpr128(i, g.r.next64(), g.r.next64());
		}
		h.SetGpr64(reg::k0, kData);
		h.SetLoPair(g.r.next64(), g.r.next64());
		h.SetHiPair(g.r.next64(), g.r.next64());
		for (u32 i = 0; i < kWindowBytes; i += 8)
			h.WriteU64(kData + i, g.r.next64());
		h.TrackMemWindow(kData, kWindowBytes);

		const bool loop = g.r.range(4) == 0;
		size_t loop_start = 0;
		if (loop)
		{
			g.prog.push_back(ADDIU(reg::t9, reg::zero, static_cast<s16>(2 + g.r.range(3))));
			loop_start = g.prog.size();
		}
		for (u32 u = 4 + g.r.range(8); u > 0; u--)
			g.emitUnit();
		if (loop)
		{
			g.prog.push_back(ADDIU(reg::t9, reg::t9, -1));
			const s16 off = static_cast<s16>(static_cast<s32>(loop_start) - static_cast<s32>(g.prog.size() + 1));
			g.prog.push_back(BNE(reg::t9, reg::zero, off));
			g.prog.push_back(NOP);
		}
		for (u32 o = 0; o < kOpCount; o++)
			for (u32 p = 0; p < kPlacementCount; p++)
				hits[o][p] += g.hits[o][p];

		h.LoadProgram(g.prog);
		h.Run();
		if (::testing::Test::HasFailure())
		{
			std::fprintf(stderr, "EEFUZZ FAILING seed=%u prog:\n", seed);
			for (size_t i = 0; i < g.prog.size(); ++i)
				std::fprintf(stderr, "  %3zu: %08x\n", i, g.prog[i]);
			return;
		}
	}
	if (only != ~0u)
		return;
	for (u32 o = 0; o < kOpCount; o++)
		for (u32 p = 0; p < kPlacementCount; p++)
			EXPECT_GE(hits[o][p], count / 8) << kOpName[o] << " placement " << p;
}
