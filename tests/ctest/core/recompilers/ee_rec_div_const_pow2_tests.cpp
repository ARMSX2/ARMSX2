// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// DIV, DIV1, DIVU and DIVU1 by a compile-time constant divisor, JIT vs
// interpreter. A signed divisor of ±2^k or ±1 and an unsigned divisor of 2^k
// are divided with shifts and no sdiv or udiv; any other constant takes the
// generic path.

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

enum Op : u32 { kDiv, kDiv1, kDivu, kDivu1, kOpCount };

const char* const kOpName[] = {"DIV", "DIV1", "DIVU", "DIVU1"};

bool IsSigned(u32 op) { return op == kDiv || op == kDiv1; }
bool IsUpper(u32 op) { return op == kDiv1 || op == kDivu1; }

u32 Encode(u32 op, u32 rs, u32 rt)
{
	switch (op)
	{
		case kDiv: return DIV(rs, rt);
		case kDiv1: return e::DIV1(rs, rt);
		case kDivu: return DIVU(rs, rt);
		default: return e::DIVU1(rs, rt);
	}
}

u32 Mflo(u32 op, u32 rd) { return IsUpper(op) ? e::MFLO1(rd) : MFLO(rd); }
u32 Mfhi(u32 op, u32 rd) { return IsUpper(op) ? e::MFHI1(rd) : MFHI(rd); }

// The divisors the JIT divides without a divide instruction.
bool Folds(u32 op, u32 d)
{
	return std::has_single_bit(d) || (IsSigned(op) && std::has_single_bit(0u - d));
}

struct Lcg
{
	u64 s;
	u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 33); }
	u32 range(u32 n) { return next() % n; }
	u64 next64() { return (static_cast<u64>(next()) << 32) | next(); }
};

// SDIV for the signed ops and UDIV for the unsigned ones, 32- or 64-bit.
u32 HostDivCount(u32 guest_pc, u32 op)
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
		n += (code[i] & 0x7FE0FC00u) == (IsSigned(op) ? 0x1AC00C00u : 0x1AC00800u);
	return n;
}

// How the divisor becomes a constant.
enum Form : u32 { kOri, kAddiu, kLui, kLuiOri, kWide, kFormCount };

const char* const kFormName[] = {"ORI", "ADDIU", "LUI", "LUI/ORI", "wide"};

bool FormApplies(u32 form, u32 d)
{
	switch (form)
	{
		case kOri: return d <= 0xFFFFu;
		case kAddiu: return static_cast<s32>(d) >= -32768 && static_cast<s32>(d) <= 32767;
		case kLui: return (d & 0xFFFFu) == 0;
		default: return true;
	}
}

// d into g. kWide leaves an upper word that is not the sign extension of d,
// built with tmp.
void EmitConst(std::vector<u32>& p, u32 form, u32 g, u32 d, u32 tmp, s16 garbage)
{
	switch (form)
	{
		case kOri: p.push_back(ORI(g, reg::zero, static_cast<u16>(d))); break;
		case kAddiu: p.push_back(ADDIU(g, reg::zero, static_cast<s16>(d))); break;
		case kLui: p.push_back(LUI(g, static_cast<u16>(d >> 16))); break;
		case kLuiOri:
			p.push_back(LUI(g, static_cast<u16>(d >> 16)));
			p.push_back(ORI(g, g, static_cast<u16>(d)));
			break;
		default:
			p.push_back(LUI(tmp, static_cast<u16>(d >> 16)));
			p.push_back(ORI(tmp, tmp, static_cast<u16>(d)));
			p.push_back(e::DADDIU(g, reg::zero, garbage));
			p.push_back(e::DSLL32(g, g, 0));
			p.push_back(XOR(g, g, tmp));
			break;
	}
}

// Where the dividend lives when the divide reads it. kComputed writes it
// earlier in the block.
enum Home : u32 { kSeeded, kLoaded, kComputed, kHomeCount };

// Where HI and LO are read back: not at all (the state diff reads them), in
// the divide's block, or in the block after it.
enum Readback : u32 { kNoRead, kSameBlock, kNextBlock, kReadbackCount };

std::vector<u32> Dividends(u32 d, Lcg& r, bool high_randoms)
{
	const u32 mag = static_cast<s32>(d) < 0 ? 0u - d : d;
	const int k = std::has_single_bit(mag) ? std::countr_zero(mag) : 2;
	std::vector<u32> v = {0, 1, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000001u, 0x7FFFFFFEu};
	for (int j = k - 1; j <= k + 1; j++)
	{
		if (j < 0 || j > 31)
			continue;
		const u32 p = 1u << j;
		for (const u32 x : {p, p + 1, p - 1, 0u - p, 0u - p + 1, 0u - p - 1})
			v.push_back(x);
	}
	for (int i = 0; i < 3; i++)
		v.push_back(r.next());
	if (high_randoms)
	{
		for (int i = 0; i < 3; i++)
			v.push_back(r.next() | 0x80000000u);
	}
	return v;
}

// Every divisor in `divisors` through every form that can build it, every
// dividend home and readback, for the two ops starting at `first_op`.
// `folding_forms` and `generic_forms` are the (divisor, form) pairs per op
// expected to fold and not to.
void RunGrid(u32 first_op, const std::vector<u32>& divisors, u32 folding_forms, u32 generic_forms)
{
	u32 folded = 0, generic = 0, runs = 0;
	u32 folded_with_div = 0, generic_without_div = 0;
	u32 readbacks[kReadbackCount] = {};
	u32 wide_dividends = 0;
	std::string first_wrong_path;
	for (u32 op = first_op; op < first_op + 2; op++)
	for (u32 di = 0; di < divisors.size(); di++)
	for (u32 form = 0; form < kFormCount; form++)
	for (u32 home = 0; home < kHomeCount; home++)
	for (u32 pinned = 0; pinned < 2; pinned++)
	{
		const u32 d = divisors[di];
		if (!FormApplies(form, d))
			continue;
		Lcg r{((((op * 128 + di) * 8 + form) * 4 + home) * 2 + pinned) * 0x9E3779B97F4A7C15ull + 0x64697632ull};
		const u32 creg = pinned ? reg::s0 : reg::t0;
		const u32 rs = pinned ? reg::a0 : reg::t1;
		const s16 cgarbage = static_cast<s16>(r.next() | 1);
		const std::vector<u32> dividends = Dividends(d, r, !IsSigned(op));

		for (u32 vi = 0; vi < dividends.size(); vi++)
		{
			const u32 readback = (vi + home + pinned) % kReadbackCount;
			// Odd rows give the dividend an upper word that is not its sign extension.
			const bool wide = ((vi + form + home) & 1) != 0;
			const u32 lo32 = dividends[vi];
			const u64 sext = static_cast<u64>(static_cast<s64>(static_cast<s32>(lo32)));
			const u64 v64 = wide ? (sext ^ (static_cast<u64>(r.next() | 1) << 32)) : sext;

			std::vector<u32> prog;
			EmitConst(prog, form, creg, d, reg::t3, cgarbage);
			switch (home)
			{
				case kLoaded: prog.push_back(e::LD(rs, 0, reg::k0)); break;
				case kComputed:
					prog.push_back(e::DSLL32(rs, reg::a1, 0));
					prog.push_back(OR(rs, rs, reg::t2));
					break;
				default: break;
			}
			prog.push_back(Encode(op, rs, creg));
			if (readback == kNextBlock)
			{
				prog.push_back(J(kPc + static_cast<u32>(prog.size() + 2) * 4));
				prog.push_back(NOP);
			}
			if (readback != kNoRead)
			{
				prog.push_back(Mflo(op, reg::v1));
				prog.push_back(Mfhi(op, reg::t5));
				prog.push_back(e::DADDU(reg::t7, reg::v1, reg::t5));
				prog.push_back(e::SQ(reg::v1, 0x20, reg::k0));
			}

			std::ostringstream desc;
			desc << kOpName[op] << " rs=r" << rs << " rt=r" << creg << " d=0x" << std::hex << d << std::dec
				 << " form=" << kFormName[form] << " home=" << home << " readback=" << readback << " rs=0x"
				 << std::hex << v64;
			SCOPED_TRACE(desc.str());

			EeRecTestHarness h;
			for (u32 g : {reg::t0, reg::s0, reg::a0, reg::t1, reg::v1, reg::t5, reg::t3, reg::t7})
				h.SetGpr128(g, r.next64(), r.next64());
			h.SetGpr64(reg::k0, kData);
			// kComputed builds rs as (a1 << 32) | t2.
			h.SetGpr64(reg::a1, v64 >> 32);
			h.SetGpr128(reg::t2, static_cast<u32>(v64), r.next64());
			if (home == kSeeded)
				h.SetGpr128(rs, v64, r.next64());
			h.SetLoPair(r.next64(), r.next64());
			h.SetHiPair(r.next64(), r.next64());
			h.WriteU64(kData, v64);
			for (u32 i = 8; i < kWindowBytes; i += 8)
				h.WriteU64(kData + i, r.next64());
			h.TrackMemWindow(kData, kWindowBytes);
			h.LoadProgram(prog);
			h.Run();
			runs++;
			readbacks[readback]++;
			wide_dividends += wide;

			// Pin the interpreter to a host truncating divide, each half
			// sign-extended from 32 bits.
			s32 lo, hi;
			if (IsSigned(op))
			{
				const s32 n = static_cast<s32>(lo32), q = static_cast<s32>(d);
				lo = (n == INT32_MIN && q == -1) ? INT32_MIN : n / q;
				hi = (n == INT32_MIN && q == -1) ? 0 : n % q;
			}
			else
			{
				lo = static_cast<s32>(lo32 / d);
				hi = static_cast<s32>(lo32 % d);
			}
			EXPECT_EQ(IsUpper(op) ? h.GetLoUpper64Interp() : h.GetLo64Interp(), static_cast<u64>(static_cast<s64>(lo)));
			EXPECT_EQ(IsUpper(op) ? h.GetHiUpper64Interp() : h.GetHi64Interp(), static_cast<u64>(static_cast<s64>(hi)));

			if (vi == 0)
			{
				const bool folds = Folds(op, d);
				const u32 divs = HostDivCount(kPc, op);
				const bool wrong = folds ? divs != 0 : divs != 1;
				(folds ? folded : generic)++;
				(folds ? folded_with_div : generic_without_div) += wrong;
				if (wrong && first_wrong_path.empty())
					first_wrong_path = desc.str();
			}
			if (::testing::Test::HasFailure())
				return;
		}
	}
	EXPECT_EQ(folded_with_div, 0u) << "of " << folded << " folding programs; first: " << first_wrong_path;
	EXPECT_EQ(generic_without_div, 0u) << "of " << generic << " generic programs; first: " << first_wrong_path;
	EXPECT_EQ(folded, 2 * folding_forms * kHomeCount * 2);
	EXPECT_EQ(generic, 2 * generic_forms * kHomeCount * 2);
	for (u32 rb = 0; rb < kReadbackCount; rb++)
		EXPECT_GE(readbacks[rb], runs / 4) << "readback " << rb;
	EXPECT_GE(wide_dividends, runs / 3);
}

} // namespace

TEST(EeRecDivConstPow2, EveryDivisorFormHomeAndReadback)
{
	std::vector<u32> divisors = {1, 0xFFFFFFFFu, 0x80000000u};
	for (u32 k = 1; k <= 30; k++)
	{
		divisors.push_back(1u << k);
		divisors.push_back(0u - (1u << k));
	}
	const u32 kGeneric[] = {3, 0xFFFFFFFDu, 6, 0x7FFFFFFFu, 0x80000001u};
	divisors.insert(divisors.end(), std::begin(kGeneric), std::end(kGeneric));
	// 63 folding divisors and 5 generic ones, each reached by LUI/ORI and
	// wide; ORI reaches 1 and 2^1..2^15, ADDIU ±1, ±2^1..2^14 and -2^15, LUI
	// INT_MIN and ±2^16..2^30. Of the generic ones ORI reaches 3 and 6, and
	// ADDIU 3, -3 and 6.
	RunGrid(kDiv, divisors, 63 * 2 + 16 + 31 + 31, 5 * 2 + 2 + 3);
}

TEST(EeRecDivConstPow2, UnsignedEveryDivisorFormHomeAndReadback)
{
	std::vector<u32> divisors;
	for (u32 k = 0; k <= 31; k++)
		divisors.push_back(1u << k);
	// -8 and -2^16 fold for DIV but not for DIVU.
	const u32 kGeneric[] = {3, 7, 6, 0xFFFFFFFFu, 0x80000001u, 0x7FFFFFFFu, 0xFFFFFFF8u, 0xFFFF0000u};
	divisors.insert(divisors.end(), std::begin(kGeneric), std::end(kGeneric));
	// 32 folding divisors and 8 generic ones, each reached by LUI/ORI and
	// wide; ORI reaches 2^0..2^15, ADDIU 2^0..2^14, LUI 2^16..2^31. Of the
	// generic ones ORI reaches 3, 7 and 6, ADDIU those and -1 and -8, and LUI
	// 0xFFFF0000.
	RunGrid(kDivu, divisors, 32 * 2 + 16 + 15 + 16, 8 * 2 + 3 + 5 + 1);
}

namespace {

// k0 = window base, t9 = loop counter, ra = terminator; never written.
constexpr u32 kPool[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                         16, 17, 18, 19, 20, 21, 22, 23, 24, 28, 29, 30};
constexpr u32 kPoolN = sizeof(kPool) / sizeof(kPool[0]);

enum Placement : u32 { kPlain, kDelaySlot, kAfterJump, kJumpDelaySlot, kPlacementCount };

// Streams of the two ops starting at `first_op`.
struct Gen
{
	Lcg r;
	u32 first_op;
	std::vector<u32> prog;
	u32 hits[kOpCount][kPlacementCount] = {};
	u32 folding_sites = 0;
	u32 lastConst = reg::zero;
	u32 lastValue = 0;

	u32 anyReg() { return kPool[r.range(kPoolN)]; }

	u32 pickDivisor()
	{
		switch (r.range(10))
		{
			case 0: return 1;
			case 1: return 0xFFFFFFFFu;
			case 2: return 0x80000000u;
			case 3: return 3 + 2 * r.range(1000);
			case 4: return r.next();
			case 5: case 6: return 0u - (1u << (1 + r.range(30)));
			default: return 1u << (1 + r.range(30));
		}
	}

	void emitConst(u32 g)
	{
		u32 tmp;
		do
			tmp = anyReg();
		while (tmp == g);
		const u32 d = pickDivisor();
		u32 form;
		do
			form = r.range(kFormCount);
		while (!FormApplies(form, d));
		EmitConst(prog, form, g, d, tmp, static_cast<s16>(r.next() | 1));
		lastConst = g;
		lastValue = d;
	}

	// A non-constant value with a garbage upper word, written in the block.
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

	// `alone` places the site with nothing in front of it (a delay slot), on
	// the constant the unit built last.
	void emitSite(u32 placement, bool alone)
	{
		const u32 op = first_op + r.range(2);
		hits[op][placement]++;
		const u32 c = alone ? lastConst : anyReg();
		u32 v;
		do
			v = anyReg();
		while (v == c);
		bool fresh = alone;
		if (!alone)
		{
			if (r.range(4))
			{
				emitConst(c);
				fresh = true;
			}
			if (r.range(2))
				emitVariable(v);
		}
		folding_sites += fresh && Folds(op, lastValue);
		prog.push_back(Encode(op, v, c));
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

void RunStreams(u32 first_op, u64 salt)
{
	const char* only_env = std::getenv("EEFUZZ_SEED");
	const u32 only = only_env ? static_cast<u32>(std::atoi(only_env)) : ~0u;
	const char* count_env = std::getenv("EEFUZZ_COUNT");
	const u32 count = count_env ? static_cast<u32>(std::strtoul(count_env, nullptr, 10)) : 2000;

	u32 hits[kOpCount][kPlacementCount] = {};
	u32 folding_sites = 0;
	for (u32 seed = 0; seed < count; ++seed)
	{
		if (only != ~0u && seed != only)
			continue;
		SCOPED_TRACE(::testing::Message() << "seed=" << seed);
		Gen g{{seed * 0x9E3779B97F4A7C15ull + salt}, first_op};
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
		folding_sites += g.folding_sites;

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
	for (u32 o = first_op; o < first_op + 2; o++)
		for (u32 p = 0; p < kPlacementCount; p++)
			EXPECT_GE(hits[o][p], count / 4) << kOpName[o] << " placement " << p;
	EXPECT_GE(folding_sites, count * 2);
}

} // namespace

TEST(EeRecDivConstPow2, RandomStreams)
{
	RunStreams(kDiv, 0x64697631ull);
}

TEST(EeRecDivConstPow2, UnsignedRandomStreams)
{
	RunStreams(kDivu, 0x64697675ull);
}
