// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// DIV, DIV1, DIVU and DIVU1 by a constant divisor, rs not constant, JIT vs
// interpreter. Of the divisors the shift forms leave, zero stores the
// divide-by-zero result with no divide, and any other is divided with no zero
// test in front. A divisor built in the block before is not constant there
// and keeps the test.

#include "harness/EeRecTestHarness.h"

#include "arm64/iR5900-arm64.h"

#include "R5900.h"
#include "vtlb.h"

#include "vixl/aarch64/disasm-aarch64.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
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

// The divisors recDivConstPow2 and recDivuConstPow2 take.
bool ShiftForm(u32 op, u32 d)
{
	return std::has_single_bit(d) || (IsSigned(op) && std::has_single_bit(0u - d));
}

struct Lcg
{
	u64 s;
	u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 33); }
	u64 next64() { return (static_cast<u64>(next()) << 32) | next(); }
};

u64 Sext(u32 v) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(v))); }

// The interpreter's results, written out independently of it: LO and HI,
// each sign-extended from 32 bits.
void Expected(u32 op, u32 n, u32 d, u64* lo, u64* hi)
{
	if (IsSigned(op))
	{
		const s32 sn = static_cast<s32>(n), sd = static_cast<s32>(d);
		if (sd == 0)
		{
			*lo = Sext(sn < 0 ? 1u : 0xFFFFFFFFu);
			*hi = Sext(n);
		}
		else if (sn == INT32_MIN && sd == -1)
		{
			*lo = Sext(0x80000000u);
			*hi = 0;
		}
		else
		{
			*lo = Sext(static_cast<u32>(sn / sd));
			*hi = Sext(static_cast<u32>(sn % sd));
		}
	}
	else if (d == 0)
	{
		*lo = ~0ull;
		*hi = Sext(n);
	}
	else
	{
		*lo = Sext(n / d);
		*hi = Sext(n % d);
	}
}

// The divide sites in one compiled block: 32-bit SDIV/UDIV, how many have a
// 32-bit CBZ right in front, how many are followed by an MSUB and then an
// unconditional B, and the MSUB count over the whole block.
struct DivShape
{
	u32 divs = 0;
	u32 guarded = 0;
	u32 branch_after = 0;
	u32 msubs = 0;
	u32 words = 0;
	bool found = false;
};

bool IsDiv32(u32 w) { return (w & 0xFFE0F800u) == 0x1AC00800u; }
bool IsCbz32(u32 w) { return (w & 0xFF000000u) == 0x34000000u; }
bool IsMsub(u32 w) { return (w & 0x7FE08000u) == 0x1B008000u; }
bool IsB(u32 w) { return (w & 0xFC000000u) == 0x14000000u; }

DivShape ShapeAt(u32 guest_pc)
{
	DivShape s;
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
		return s;
	s.found = true;
	const u32* code = reinterpret_cast<const u32*>(fn);
	s.words = size / 4;
	for (u32 i = 0; i < s.words; i++)
	{
		s.msubs += IsMsub(code[i]);
		if (!IsDiv32(code[i]))
			continue;
		s.divs++;
		s.guarded += i > 0 && IsCbz32(code[i - 1]);
		s.branch_after += i + 2 < s.words && IsMsub(code[i + 1]) && IsB(code[i + 2]);
	}
	return s;
}

// The program page goes to manual protection after enough write-and-clear
// cycles in one process, and a manual block entered later can reset the page
// and take the earlier blocks with it. Each test starts from a reset page.
void ResetRecAndPageProtection()
{
	recCpu.Reset();
	mmap_ResetBlockTracking();
}

void DumpBlock(u32 guest_pc)
{
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
		return;
	std::fprintf(stderr, "block 0x%x, %u host bytes:\n", guest_pc, size);
	vixl::aarch64::PrintDisassembler dis(stderr);
	dis.DisassembleBuffer(reinterpret_cast<const vixl::aarch64::Instruction*>(fn), size);
}

// How rt gets its value. kZeroReg is rt = r0. kPrevBlock builds the constant
// in the block before the divide, so the divide's block compiles with rt not
// constant. kWide leaves an upper word that is not the sign extension of d.
enum Form : u32 { kOri, kAddiu, kLui, kLuiOri, kWide, kZeroReg, kPrevBlock, kFormCount };

const char* const kFormName[] = {"ORI", "ADDIU", "LUI", "LUI/ORI", "wide", "r0", "prev-block"};

bool FormApplies(u32 form, u32 d)
{
	switch (form)
	{
		case kOri: return d <= 0xFFFFu;
		case kAddiu: return static_cast<s32>(d) >= -32768 && static_cast<s32>(d) <= 32767;
		case kLui: return (d & 0xFFFFu) == 0;
		case kZeroReg: return d == 0;
		default: return true;
	}
}

void EmitConst(std::vector<u32>& p, u32 form, u32 g, u32 d, u32 tmp, s16 garbage)
{
	switch (form)
	{
		case kOri: p.push_back(ORI(g, reg::zero, static_cast<u16>(d))); break;
		case kAddiu: p.push_back(ADDIU(g, reg::zero, static_cast<s16>(d))); break;
		case kLui: p.push_back(LUI(g, static_cast<u16>(d >> 16))); break;
		case kWide:
			p.push_back(LUI(tmp, static_cast<u16>(d >> 16)));
			p.push_back(ORI(tmp, tmp, static_cast<u16>(d)));
			p.push_back(e::DADDIU(g, reg::zero, garbage));
			p.push_back(e::DSLL32(g, g, 0));
			p.push_back(XOR(g, g, tmp));
			break;
		case kZeroReg: break;
		default:
			p.push_back(LUI(g, static_cast<u16>(d >> 16)));
			p.push_back(ORI(g, g, static_cast<u16>(d)));
			break;
	}
}

// Where rs lives when the divide reads it. kLoaded and kComputed write it
// earlier in the divide's block, so it is dirty in an allocator register.
enum Home : u32 { kSeeded, kLoaded, kComputed, kHomeCount };

// Where the divide sits in its block: straight-line, or in the delay slot of
// a taken branch whose condition folds (BEQ r0, r0) or does not (BNE k0, r0).
enum Placement : u32 { kInline, kDelayConst, kDelayRuntime, kPlacementCount };

// Where HI and LO are read back: not at all (the state diff reads them), in
// the divide's block, or in the block after it.
enum Readback : u32 { kNoRead, kSameBlock, kNextBlock, kReadbackCount };

std::vector<u32> Dividends(u32 d, Lcg& r)
{
	std::vector<u32> v = {0, 1, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000001u, 0x7FFFFFFEu,
		d, d + 1, d - 1, 0u - d, d * 3 + 1};
	for (int i = 0; i < 4; i++)
		v.push_back(r.next());
	return v;
}

struct Tally
{
	u32 runs = 0;
	u32 rows[kFormCount] = {};
	u32 homes[kHomeCount] = {};
	u32 placements[kPlacementCount] = {};
	u32 readbacks[kReadbackCount] = {};
	u32 wide_dividends = 0;
	u32 pinned_rs = 0;
	u32 alias_rows = 0;
	u32 const_programs = 0, const_wrong = 0;
	u32 zero_programs = 0;
	u32 runtime_programs = 0, runtime_wrong = 0;
	u32 unobserved = 0;
	std::string first_wrong;
};

// One divide program. `alias` divides creg by itself.
void RunOne(u32 op, u32 d, u32 form, u32 home, u32 placement, u32 readback, bool pinned, bool alias, u32 n,
	bool wide, Lcg& r, Tally& t)
{
	const u32 creg = form == kZeroReg ? reg::zero : (pinned ? reg::s0 : reg::t0);
	const u32 rs = alias ? creg : (pinned ? reg::a0 : reg::t1);
	if (alias)
		n = d;
	const u64 sext = Sext(n);
	const u64 v64 = wide ? (sext ^ (static_cast<u64>(r.next() | 1) << 32)) : sext;

	std::vector<u32> prog;
	EmitConst(prog, form, creg, d, reg::t3, static_cast<s16>(r.next() | 1));
	u32 div_pc = kPc;
	if (form == kPrevBlock)
	{
		prog.push_back(J(kPc + static_cast<u32>(prog.size() + 2) * 4));
		prog.push_back(NOP);
		div_pc = kPc + static_cast<u32>(prog.size()) * 4;
	}
	if (!alias)
	{
		switch (home)
		{
			case kLoaded: prog.push_back(e::LD(rs, 0, reg::k0)); break;
			case kComputed:
				prog.push_back(e::DSLL32(rs, reg::a1, 0));
				prog.push_back(OR(rs, rs, reg::t2));
				break;
			default: break;
		}
	}
	if (placement == kInline)
		prog.push_back(Encode(op, rs, creg));
	else
	{
		prog.push_back(placement == kDelayConst ? BEQ(reg::zero, reg::zero, 1) : BNE(reg::k0, reg::zero, 1));
		prog.push_back(Encode(op, rs, creg));
		prog.push_back(ADDIU(reg::t7, reg::zero, 1)); // skipped
	}
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
		prog.push_back(e::SQ(reg::t5, 0x30, reg::k0));
	}

	std::ostringstream desc;
	desc << kOpName[op] << " rs=r" << rs << " rt=r" << creg << " d=0x" << std::hex << d << std::dec
		 << " form=" << kFormName[form] << " home=" << home << " placement=" << placement
		 << " readback=" << readback << (alias ? " alias" : "") << " rs=0x" << std::hex << v64;
	SCOPED_TRACE(desc.str());

	EeRecTestHarness h;
	for (u32 g : {reg::t0, reg::s0, reg::a0, reg::t1, reg::v1, reg::t5, reg::t3, reg::t7})
		h.SetGpr128(g, r.next64(), r.next64());
	h.SetGpr64(reg::k0, kData);
	// kComputed builds rs as (a1 << 32) | t2.
	h.SetGpr64(reg::a1, v64 >> 32);
	h.SetGpr128(reg::t2, static_cast<u32>(v64), r.next64());
	if (home == kSeeded && !alias)
		h.SetGpr128(rs, v64, r.next64());
	const u64 lo_pre[2] = {r.next64(), r.next64()};
	const u64 hi_pre[2] = {r.next64(), r.next64()};
	h.SetLoPair(lo_pre[0], lo_pre[1]);
	h.SetHiPair(hi_pre[0], hi_pre[1]);
	h.WriteU64(kData, v64);
	for (u32 i = 8; i < kWindowBytes; i += 8)
		h.WriteU64(kData + i, r.next64());
	h.TrackMemWindow(kData, kWindowBytes);
	h.LoadProgram(prog);
	h.Run();

	t.runs++;
	t.rows[form]++;
	t.homes[alias ? kSeeded : home]++;
	t.placements[placement]++;
	t.readbacks[readback]++;
	t.wide_dividends += wide && !alias;
	t.pinned_rs += !alias && armEEPinForGPR(static_cast<int>(rs)) != nullptr;
	t.alias_rows += alias;

	u64 lo, hi;
	Expected(op, n, d, &lo, &hi);
	const bool up = IsUpper(op);
	EXPECT_EQ(up ? h.GetLoUpper64Interp() : h.GetLo64Interp(), lo);
	EXPECT_EQ(up ? h.GetHiUpper64Interp() : h.GetHi64Interp(), hi);
	EXPECT_EQ(up ? h.GetLoUpper64Jit() : h.GetLo64Jit(), lo);
	EXPECT_EQ(up ? h.GetHiUpper64Jit() : h.GetHi64Jit(), hi);
	// The other pipeline's half is left alone.
	EXPECT_EQ(up ? h.GetLo64Jit() : h.GetLoUpper64Jit(), lo_pre[up ? 0 : 1]);
	EXPECT_EQ(up ? h.GetHi64Jit() : h.GetHiUpper64Jit(), hi_pre[up ? 0 : 1]);
	if (readback != kNoRead)
	{
		EXPECT_EQ(h.GetGpr64Jit(reg::v1), lo);
		EXPECT_EQ(h.GetGpr64Jit(reg::t5), hi);
	}

	// Which emitter path the divide took.
	if (alias || ShiftForm(op, d))
		return;
	const DivShape s = ShapeAt(div_pc);
	if (!s.found)
	{
		t.unobserved++;
		return;
	}
	bool wrong;
	if (form == kPrevBlock)
	{
		t.runtime_programs++;
		wrong = s.divs != 1 || s.guarded != 1;
		t.runtime_wrong += wrong;
	}
	else
	{
		t.const_programs++;
		t.zero_programs += d == 0;
		wrong = s.guarded != 0 || s.branch_after != 0 || s.divs != (d == 0 ? 0u : 1u) ||
				(d == 0 && s.msubs != 0);
		t.const_wrong += wrong;
	}
	if (wrong && t.first_wrong.empty())
	{
		std::ostringstream m;
		m << desc.str() << " (divs " << s.divs << ", guarded " << s.guarded << ", followed by b " << s.branch_after
		  << ", msubs " << s.msubs << ")";
		t.first_wrong = m.str();
		DumpBlock(div_pc);
	}
}

const u32 kDivisors[] = {0, 1, 0xFFFFFFFFu, 2, 3, 7, 10, 0xFFFFFFF9u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu,
	0x0001ABCDu, 0xDEADBEEFu, 0x12345679u, 0xFFFF0001u};

void RunGrid(u32 first_op, Tally& t)
{
	for (u32 op = first_op; op < first_op + 2; op++)
	for (u32 di = 0; di < std::size(kDivisors); di++)
	for (u32 form = 0; form < kFormCount; form++)
	for (u32 pinned = 0; pinned < 2; pinned++)
	{
		const u32 d = kDivisors[di];
		if (!FormApplies(form, d) || (form == kZeroReg && pinned))
			continue;
		Lcg r{(((op * 32 + di) * 8 + form) * 2 + pinned) * 0x9E3779B97F4A7C15ull + 0x64697663ull};
		const std::vector<u32> dividends = Dividends(d, r);
		for (u32 vi = 0; vi < dividends.size(); vi++)
		{
			const u32 home = (vi + form) % kHomeCount;
			const u32 placement = (vi / kHomeCount + pinned) % kPlacementCount;
			const u32 readback = (vi + di + pinned) % kReadbackCount;
			// Odd rows give the dividend an upper word that is not its sign extension.
			const bool wide = ((vi + form + di) & 1) != 0;
			RunOne(op, d, form, home, placement, readback, pinned != 0, false, dividends[vi], wide, r, t);
			if (::testing::Test::HasFailure())
				return;
		}
		if (form != kZeroReg)
			RunOne(op, d, form, kSeeded, kInline, pinned ? kSameBlock : kNoRead, pinned != 0, true, 0, false, r, t);
		if (::testing::Test::HasFailure())
			return;
	}
}

void CheckTally(const Tally& t)
{
	::testing::Test::RecordProperty("runs", static_cast<int>(t.runs));
	::testing::Test::RecordProperty("const_programs", static_cast<int>(t.const_programs));
	::testing::Test::RecordProperty("zero_programs", static_cast<int>(t.zero_programs));
	::testing::Test::RecordProperty("runtime_programs", static_cast<int>(t.runtime_programs));
	::testing::Test::RecordProperty("unobserved", static_cast<int>(t.unobserved));
	EXPECT_EQ(t.const_wrong, 0u) << "of " << t.const_programs << " constant-divisor programs; first: " << t.first_wrong;
	EXPECT_EQ(t.runtime_wrong, 0u) << "of " << t.runtime_programs << " prev-block programs; first: " << t.first_wrong;
	EXPECT_LE(t.unobserved * 50, t.const_programs + t.runtime_programs) << t.unobserved << " blocks gone by the time they were read";
	EXPECT_GE(t.const_programs, 500u);
	EXPECT_GE(t.zero_programs, 100u);
	EXPECT_GE(t.runtime_programs, 100u);
	for (u32 f = 0; f < kFormCount; f++)
		EXPECT_GE(t.rows[f], 30u) << kFormName[f];
	for (u32 x = 0; x < kHomeCount; x++)
		EXPECT_GE(t.homes[x], t.runs / 5) << "home " << x;
	for (u32 x = 0; x < kPlacementCount; x++)
		EXPECT_GE(t.placements[x], t.runs / 5) << "placement " << x;
	for (u32 x = 0; x < kReadbackCount; x++)
		EXPECT_GE(t.readbacks[x], t.runs / 5) << "readback " << x;
	EXPECT_GE(t.wide_dividends, t.runs / 3);
	EXPECT_GE(t.pinned_rs, t.runs / 3);
	EXPECT_GE(t.alias_rows, 40u);
}

} // namespace

TEST(EeRecDivConstDivisor, SignedEveryDivisorFormAndDividend)
{
	ResetRecAndPageProtection();
	Tally t;
	RunGrid(kDiv, t);
	CheckTally(t);
}

TEST(EeRecDivConstDivisor, UnsignedEveryDivisorFormAndDividend)
{
	ResetRecAndPageProtection();
	Tally t;
	RunGrid(kDivu, t);
	CheckTally(t);
}

// INT_MIN / -1 gives LO = INT_MIN, HI = 0. A constant -1 takes the shift
// form; a divisor built in the block before, or seeded, takes SDIV and MSUB.
TEST(EeRecDivConstDivisor, IntMinByMinusOne)
{
	ResetRecAndPageProtection();
	for (const u32 op : {kDiv, kDiv1})
	for (const u32 form : {kAddiu, kLuiOri, kPrevBlock, kFormCount})
	for (const u64 rs_val : {0xFFFFFFFF80000000ull, 0x0000000080000000ull, 0x1234567880000000ull})
	{
		SCOPED_TRACE(::testing::Message() << kOpName[op] << " form=" << (form == kFormCount ? "seeded" : kFormName[form])
										  << " rs=0x" << std::hex << rs_val);
		std::vector<u32> prog;
		u32 div_pc = kPc;
		if (form != kFormCount)
			EmitConst(prog, form, reg::t0, 0xFFFFFFFFu, reg::t3, 1);
		if (form == kPrevBlock)
		{
			prog.push_back(J(kPc + static_cast<u32>(prog.size() + 2) * 4));
			prog.push_back(NOP);
			div_pc = kPc + static_cast<u32>(prog.size()) * 4;
		}
		prog.push_back(Encode(op, reg::a0, reg::t0));
		prog.push_back(Mflo(op, reg::v1));
		prog.push_back(Mfhi(op, reg::t5));

		EeRecTestHarness h;
		h.SetGpr64(reg::a0, rs_val);
		h.SetGpr64(reg::t0, 0x00000001FFFFFFFFull);
		h.SetLoPair(0x1111, 0x2222);
		h.SetHiPair(0x3333, 0x4444);
		h.LoadProgram(prog);
		h.Run();
		const bool up = IsUpper(op);
		EXPECT_EQ(up ? h.GetLoUpper64Jit() : h.GetLo64Jit(), 0xFFFFFFFF80000000ull);
		EXPECT_EQ(up ? h.GetHiUpper64Jit() : h.GetHi64Jit(), 0u);
		EXPECT_EQ(up ? h.GetLoUpper64Interp() : h.GetLo64Interp(), 0xFFFFFFFF80000000ull);
		EXPECT_EQ(up ? h.GetHiUpper64Interp() : h.GetHi64Interp(), 0u);
		EXPECT_EQ(h.GetGpr64Jit(reg::v1), 0xFFFFFFFF80000000ull);
		EXPECT_EQ(h.GetGpr64Jit(reg::t5), 0u);

		const DivShape s = ShapeAt(div_pc);
		ASSERT_TRUE(s.found);
		const bool constant = form == kAddiu || form == kLuiOri;
		EXPECT_EQ(s.divs, constant ? 0u : 1u);
		if (!constant)
			EXPECT_EQ(s.guarded, 1u);
	}
}

// The constant-divisor arm emits no zero test and no divide-by-zero tail: a
// nonzero divisor is SDIV/UDIV, MSUB and the stores, a zero one no divide at
// all. The divisor built in the block before keeps the CBZ, which is what
// shows the scan can see one.
TEST(EeRecDivConstDivisor, ConstantDivisorHasNoZeroTest)
{
	ResetRecAndPageProtection();
	const bool dump = std::getenv("EEDIV_DUMP") != nullptr;
	u32 cases = 0;
	for (u32 op = 0; op < kOpCount; op++)
	for (const u32 d : {7u, 3u, 10u, 0xFFFFFFF9u, 0x7FFFFFFFu, 0u})
	for (const u32 form : {kOri, kLuiOri, kWide, kZeroReg, kPrevBlock})
	for (const u32 rs : {reg::a0, reg::t1})
	{
		if (!FormApplies(form, d))
			continue;
		const u32 creg = form == kZeroReg ? reg::zero : reg::t0;
		SCOPED_TRACE(::testing::Message() << kOpName[op] << " d=0x" << std::hex << d << std::dec << " form="
										  << kFormName[form] << " rs=r" << rs);
		std::vector<u32> prog;
		EmitConst(prog, form, creg, d, reg::t3, 0x55);
		u32 div_pc = kPc;
		if (form == kPrevBlock)
		{
			prog.push_back(J(kPc + static_cast<u32>(prog.size() + 2) * 4));
			prog.push_back(NOP);
			div_pc = kPc + static_cast<u32>(prog.size()) * 4;
		}
		prog.push_back(Encode(op, rs, creg));
		prog.push_back(Mflo(op, reg::v0));
		prog.push_back(Mfhi(op, reg::v1));

		EeRecTestHarness h;
		h.SetGpr64(reg::a0, 0x12345678);
		h.SetGpr64(reg::t1, 0xFFFFFFFF87654321ull);
		h.LoadProgram(prog);
		h.Run();

		const DivShape s = ShapeAt(div_pc);
		ASSERT_TRUE(s.found);
		cases++;
		bool ok;
		if (form == kPrevBlock)
			ok = s.divs == 1 && s.guarded == 1 && s.branch_after == 1;
		else
			ok = s.guarded == 0 && s.branch_after == 0 && s.divs == (d == 0 ? 0u : 1u) && s.msubs == (d == 0 ? 0u : 1u);
		EXPECT_TRUE(ok) << "divs " << s.divs << ", guarded " << s.guarded << ", followed by b " << s.branch_after
						<< ", msubs " << s.msubs;
		if (dump || !ok)
		{
			std::fprintf(stderr, "%s d=0x%x form=%s rs=r%u: %u words\n", kOpName[op], d, kFormName[form], rs, s.words);
			DumpBlock(div_pc);
		}
	}
	// Per op and rs: 7, 3 and 10 by ORI, LUI/ORI, wide and prev-block; the
	// two wide divisors by the last three; 0 by all five.
	EXPECT_EQ(cases, 4u * 2 * (3 * 4 + 2 * 3 + 5));
}
