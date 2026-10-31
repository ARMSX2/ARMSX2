// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Shift pairs whose second instruction reads and overwrites the first one's
// destination compile to one bitfield instruction. JIT vs interpreter over
// every shift amount and ANDI mask, with the pairs that must not fuse.

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
#include <utility>
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

struct Lcg
{
	u64 s;
	u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 33); }
	u64 next64() { return (static_cast<u64>(next()) << 32) | next(); }
};

u64 Sext(u32 v) { return static_cast<u64>(static_cast<s64>(static_cast<s32>(v))); }

enum Family : u32 { kLeftRight32, kLeftRight64, kRightMask, kMaskLeft, kFamilyCount };

const char* const kFamilyName[] = {"SLL+SRx", "DSLLx+DSRx", "SRx+ANDI", "ANDI+SLLx"};

// Instructions with their register fields left zero.
constexpr u32 kSll = SLL(0, 0, 0), kSrl = SRL(0, 0, 0), kSra = SRA(0, 0, 0);
constexpr u32 kDsll = e::DSLL(0, 0, 0), kDsrl = e::DSRL(0, 0, 0), kDsra = e::DSRA(0, 0, 0);
constexpr u32 kDsll32 = e::DSLL32(0, 0, 0), kDsrl32 = e::DSRL32(0, 0, 0), kDsra32 = e::DSRA32(0, 0, 0);
constexpr u32 kAndi = ANDI(0, 0, 0);

const u16 kMasks[] = {0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F, 0x00FF, 0x01FF, 0x03FF, 0x07FF,
	0x0FFF, 0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF, 0x0000, 0x0002, 0x00F0, 0x8000, 0x5555, 0xFFFE, 0x7FFE, 0x0101};

bool IsAndi(u32 code) { return (code >> 26) == 0x0C; }

u32 WithAmount(u32 shift, u32 sa) { return shift | (sa << 6); }
u32 WithMask(u16 m) { return kAndi | m; }

// Register fields: a shift reads rt and writes rd, ANDI reads rs and writes rt.
u32 WithRegs(u32 code, u32 src, u32 dest)
{
	if (IsAndi(code))
		return code | (src << 21) | (dest << 16);
	return code | (src << 16) | (dest << 11);
}

// The interpreter's result, written out independently of it.
u64 Apply(u32 code, u64 v)
{
	if (IsAndi(code))
		return v & (code & 0xFFFF);
	const u32 sa = (code >> 6) & 31;
	switch (code & 0x3F)
	{
		case 0x00: return Sext(static_cast<u32>(v) << sa);
		case 0x02: return Sext(static_cast<u32>(v) >> sa);
		case 0x03: return Sext(static_cast<u32>(static_cast<s32>(v) >> sa));
		case 0x38: return v << sa;
		case 0x3A: return v >> sa;
		case 0x3B: return static_cast<u64>(static_cast<s64>(v) >> sa);
		case 0x3C: return v << (sa + 32);
		case 0x3E: return v >> (sa + 32);
		default: return static_cast<u64>(static_cast<s64>(v) >> (sa + 32));
	}
}

u32 Amount(u32 code)
{
	const u32 f = code & 0x3F;
	return ((code >> 6) & 31) + (f >= 0x3C ? 32 : 0);
}

struct Pair
{
	Family family;
	u32 first, second; // register fields zero
	bool fuses;        // when the second reads and writes the first's destination
};

std::vector<Pair> AllPairs()
{
	std::vector<Pair> v;
	for (u32 a = 0; a < 32; a++)
	for (u32 b = 0; b < 32; b++)
	for (const u32 r : {kSra, kSrl})
		v.push_back({kLeftRight32, WithAmount(kSll, a), WithAmount(r, b), true});
	for (const u32 l : {kDsll, kDsll32})
	for (const u32 r : {kDsra, kDsrl, kDsra32, kDsrl32})
	for (u32 a = 0; a < 32; a++)
	for (u32 b = 0; b < 32; b++)
	{
		const u32 first = WithAmount(l, a), second = WithAmount(r, b);
		v.push_back({kLeftRight64, first, second, Amount(first) != 0 || Amount(second) != 0});
	}
	for (const u32 r : {kSrl, kSra, kDsrl, kDsra, kDsrl32, kDsra32})
	for (u32 a = 0; a < 32; a++)
	for (const u16 m : kMasks)
	{
		const u32 first = WithAmount(r, a);
		const u32 n = std::has_single_bit(static_cast<u32>(m) + 1) ? std::popcount(m) : 0;
		const bool arith = r == kSra || r == kDsra || r == kDsra32;
		const u32 bits = (r == kSrl || r == kSra) ? 32 : 64;
		v.push_back({kRightMask, first, WithMask(m), n != 0 && !(arith && Amount(first) + n > bits)});
	}
	for (const u16 m : kMasks)
	for (const u32 l : {kSll, kDsll, kDsll32})
	for (u32 b = 0; b < 32; b++)
		v.push_back({kMaskLeft, WithMask(m), WithAmount(l, b), std::has_single_bit(static_cast<u32>(m) + 1) && m != 0});
	return v;
}

// SBFM, UBFM (every immediate shift, sxtw, ubfx/ubfiz/sbfx/sbfiz) and AND,
// immediate or register, 32- or 64-bit.
bool IsShiftClass(u32 w)
{
	const u32 op = w & 0x7F800000u;
	return op == 0x13000000u || op == 0x53000000u || op == 0x12000000u || (w & 0x7F200000u) == 0x0A000000u;
}

// mov between registers (ORR with the zero register, no shift).
bool IsMov(u32 w) { return (w & 0x7FE0FFE0u) == 0x2A0003E0u; }

bool IsCall(u32 w) { return (w & 0xFC000000u) == 0x94000000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u; }

struct Shape
{
	bool found = false;
	u32 ops = 0; // shift-class and mov
	u32 calls = 0;
	u32 first_shift_class = 0;
};

Shape ShapeAt(u32 guest_pc)
{
	Shape s;
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
		return s;
	s.found = true;
	const u32* code = reinterpret_cast<const u32*>(fn);
	for (u32 i = 0; i < size / 4; i++)
	{
		if (IsShiftClass(code[i]) && s.first_shift_class == 0)
			s.first_shift_class = code[i];
		s.ops += IsShiftClass(code[i]) || IsMov(code[i]);
		s.calls += IsCall(code[i]);
	}
	return s;
}

bool Is64BitShiftByZero(u32 code) { return !IsAndi(code) && (code & 0x3F) >= 0x38 && Amount(code) == 0; }

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

void ResetRecAndPageProtection()
{
	recCpu.Reset();
	mmap_ResetBlockTracking();
}

// Source and destination of the first instruction; aliased or not, pinned or not.
struct Regs
{
	u32 src, dest;
};
const Regs kRegs[] = {{reg::t0, reg::t1}, {reg::t1, reg::t1}, {reg::a0, reg::s0}, {reg::s0, reg::s0},
	{reg::a1, reg::t3}, {reg::t2, reg::v1}};

// Where the source's value comes from. kComputed and kQuad write it earlier
// in the block (an allocator register, a 128-bit home); kConst builds it with
// LUI/ORI so the pair folds; kZero reads r0.
enum Home : u32 { kSeeded, kComputed, kQuad, kConst, kZero, kHomeCount };

// Where the pair sits. kDelayTaken puts the first instruction in the delay
// slot of a taken BEQ r0,r0 whose target is the second; kDelaySwap the same
// under BNE k0,r0, which can hoist the slot; kDelayCont under a BEQ k0,r0 that
// falls through to the second in the same superblock; kDelaySkip under a taken
// BNE that jumps past the second. kSplit makes the second a branch target, so
// a block starts there (it runs twice). kBeforeBranch puts the pair right
// before a taken branch whose delay slot reads the destination.
enum Placement : u32
{
	kInline,
	kDelayTaken,
	kDelaySwap,
	kDelayCont,
	kDelaySkip,
	kSplit,
	kBeforeBranch,
	kPlacementCount
};

const char* const kPlacementName[] = {"inline", "delay-taken", "delay-swap", "delay-cont", "delay-skip", "split",
	"before-branch"};

// Whether the second instruction completes the pair. kDestOther writes another
// register, so the intermediate stays live and is read later; kSrcOther reads
// another register.
enum Variant : u32 { kFuse, kDestOther, kSrcOther, kVariantCount };

// Where the destination is read back: not at all, in the pair's block, or in
// the block after it.
enum Readback : u32 { kNoRead, kSameBlock, kNextBlock, kReadbackCount };

const u64 kEdgeValues[] = {0, 0x7FFFFFFFull, 0xFFFFFFFF80000000ull, 0xFFFFFFFFFFFFFFFFull, 0x00000000FFFFFFFFull,
	0x0000000080000000ull, 0x123456787FFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFF00000000ull, 0x0000000100000000ull};

struct Tally
{
	u32 runs = 0;
	u32 families[kFamilyCount] = {};
	u32 homes[kHomeCount] = {};
	u32 placements[kPlacementCount] = {};
	u32 variants[kVariantCount] = {};
	u32 readbacks[kReadbackCount] = {};
	u32 regs[std::size(kRegs)] = {};
	u32 wide = 0;
	u32 split_blocks = 0;
};

struct Case
{
	Pair pair;
	u32 regs, home, placement, variant, readback;
	u64 value;
};

void RunOne(const Case& c, Lcg& r, Tally& t)
{
	const u32 src = c.home == kZero ? reg::zero : kRegs[c.regs].src;
	const u32 dest = kRegs[c.regs].dest;
	const u32 other = (c.regs & 1) ? reg::a2 : reg::t5;
	const u32 other2 = reg::t4;
	const u64 v = c.home == kZero ? 0 : c.home == kConst ? Sext(static_cast<u32>(c.value)) : c.value;

	const u32 first = WithRegs(c.pair.first, src, dest);
	u32 second;
	switch (c.variant)
	{
		case kDestOther: second = WithRegs(c.pair.second, dest, other); break;
		case kSrcOther: second = WithRegs(c.pair.second, other2, dest); break;
		default: second = WithRegs(c.pair.second, dest, dest); break;
	}

	std::vector<u32> p;
	switch (c.home)
	{
		case kComputed: p.push_back(e::DADDU(src, reg::k1, reg::t8)); break;
		case kQuad: p.push_back(e::LQ(src, 0, reg::k0)); break;
		case kConst:
			p.push_back(LUI(src, static_cast<u16>(v >> 16)));
			p.push_back(ORI(src, src, static_cast<u16>(v)));
			break;
		default: break;
	}
	const auto here = [&] { return kPc + static_cast<u32>(p.size()) * 4; };
	u32 split_pc = 0;
	switch (c.placement)
	{
		case kDelayTaken: p.push_back(BEQ(reg::zero, reg::zero, 1)); break;
		case kDelaySwap: p.push_back(BNE(reg::k0, reg::zero, 1)); break;
		case kDelayCont: p.push_back(BEQ(reg::k0, reg::zero, 2)); break;
		case kDelaySkip: p.push_back(BNE(reg::k0, reg::zero, 2)); break;
		default: break;
	}
	p.push_back(first);
	if (c.placement == kSplit)
		split_pc = here();
	p.push_back(second);
	if (c.placement == kSplit)
	{
		p.push_back(ADDIU(reg::t9, reg::t9, -1));
		p.push_back(BNE(reg::t9, reg::zero, -3));
		p.push_back(NOP);
	}
	else if (c.placement == kDelayCont || c.placement == kDelaySkip)
		p.push_back(ADDIU(reg::t7, reg::zero, 1));
	else if (c.placement == kBeforeBranch)
	{
		p.push_back(BEQ(reg::zero, reg::zero, 2));
		p.push_back(e::DADDU(reg::t6, dest, reg::zero));
		p.push_back(ADDIU(reg::t7, reg::zero, 1)); // skipped
	}
	if (c.readback == kNextBlock)
	{
		p.push_back(J(here() + 8));
		p.push_back(NOP);
	}
	if (c.readback != kNoRead)
	{
		p.push_back(e::DADDU(reg::t7, dest, reg::t6));
		p.push_back(e::SQ(dest, 0x20, reg::k0));
		if (c.variant == kDestOther)
			p.push_back(e::SQ(other, 0x30, reg::k0));
	}

	std::ostringstream desc;
	desc << kFamilyName[c.pair.family] << " first=0x" << std::hex << first << " second=0x" << second << std::dec
		 << " src=r" << src << " dest=r" << dest << " home=" << c.home << " " << kPlacementName[c.placement]
		 << " variant=" << c.variant << " readback=" << c.readback << " v=0x" << std::hex << v;
	SCOPED_TRACE(desc.str());

	EeRecTestHarness h;
	for (u32 g : {reg::t0, reg::t1, reg::t2, reg::t3, reg::t4, reg::t5, reg::t6, reg::t7, reg::a0, reg::a1, reg::a2,
			 reg::s0, reg::v1})
		h.SetGpr128(g, r.next64(), r.next64());
	h.SetGpr64(reg::k0, kData);
	h.SetGpr64(reg::t9, 2);
	const u64 addend = r.next64();
	h.SetGpr64(reg::k1, v - addend);
	h.SetGpr64(reg::t8, addend);
	if (c.home == kSeeded)
		h.SetGpr128(src, v, r.next64());
	h.WriteU64(kData, v);
	for (u32 i = 8; i < 0x40; i += 8)
		h.WriteU64(kData + i, r.next64());
	h.TrackMemWindow(kData, 0x40);
	const u64 other2_v = r.next64();
	h.SetGpr64(other2, other2_v);
	h.LoadProgram(p);
	h.Run();

	t.runs++;
	t.families[c.pair.family]++;
	t.homes[c.home]++;
	t.placements[c.placement]++;
	t.variants[c.variant]++;
	t.readbacks[c.readback]++;
	t.regs[c.regs]++;
	t.wide += Sext(static_cast<u32>(v)) != v;
	if (c.placement == kSplit)
	{
		uptr fn = 0, lut = 0;
		u32 size = 0;
		t.split_blocks += recEeBlockHostInfo(split_pc, &fn, &size, &lut);
	}

	// How many times the second instruction runs.
	const u32 runs = c.placement == kDelaySkip ? 0 : c.placement == kSplit ? 2 : 1;
	const u64 mid = Apply(first, v);
	u64 want = mid;
	if (c.variant == kFuse)
		for (u32 k = 0; k < runs; k++)
			want = Apply(second, want);
	else if (c.variant == kSrcOther && runs != 0)
		want = Apply(second, other2_v);
	EXPECT_EQ(h.GetGpr64Jit(dest), want);
	EXPECT_EQ(h.GetGpr64Interp(dest), want);
	if (c.variant == kDestOther && runs != 0)
		EXPECT_EQ(h.GetGpr64Jit(other), Apply(second, mid));
}

void CheckTally(const Tally& t)
{
	::testing::Test::RecordProperty("runs", static_cast<int>(t.runs));
	for (u32 x = 0; x < kHomeCount; x++)
		EXPECT_GE(t.homes[x], t.runs / 7) << "home " << x;
	for (u32 x = 0; x < kPlacementCount; x++)
		EXPECT_GE(t.placements[x], t.runs / 10) << kPlacementName[x];
	for (u32 x = 0; x < kVariantCount; x++)
		EXPECT_GE(t.variants[x], t.runs / 5) << "variant " << x;
	for (u32 x = 0; x < kReadbackCount; x++)
		EXPECT_GE(t.readbacks[x], t.runs / 5) << "readback " << x;
	for (u32 x = 0; x < std::size(kRegs); x++)
		EXPECT_GE(t.regs[x], t.runs / 9) << "regs " << x;
	EXPECT_GE(t.wide, t.runs / 4);
	EXPECT_EQ(t.split_blocks, t.placements[kSplit]) << "a block starts at the second instruction";
}

// Every pair of the family, fused inline on a value and its complement, then
// once more with the other dimensions rotated.
void RunFamily(Family f, u64 seed)
{
	ResetRecAndPageProtection();
	Tally t, inline_runs;
	Lcg r{seed};
	u32 i = 0;
	for (const Pair& pr : AllPairs())
	{
		if (pr.family != f)
			continue;
		const u64 v = r.next64();
		for (const u64 value : {v, ~v})
		{
			const Case c{pr, i % static_cast<u32>(std::size(kRegs)), i % 3, kInline, kFuse, i % kReadbackCount, value};
			RunOne(c, r, inline_runs);
		}
		Case c;
		c.pair = pr;
		c.regs = i % std::size(kRegs);
		// kFuse is half the runs; the rest split between the two non-pairs.
		c.variant = (i / 7) % 4 < 2 ? kFuse : ((i / 7) % 4 == 2 ? kDestOther : kSrcOther);
		c.home = (i / 3) % kHomeCount;
		c.placement = (i / 5 + i / 35) % kPlacementCount;
		c.readback = (i / 2 + i / 11) % kReadbackCount;
		c.value = (i % 3 == 0) ? kEdgeValues[(i / 3) % std::size(kEdgeValues)] : r.next64();
		RunOne(c, r, t);
		i++;
		if (::testing::Test::HasFailure())
			return;
	}
	EXPECT_EQ(inline_runs.runs, 2 * t.runs);
	CheckTally(t);
}

} // namespace

TEST(EeRecShiftPair, LeftThenRight32)
{
	RunFamily(kLeftRight32, 0x736c6c737261ull);
}

TEST(EeRecShiftPair, LeftThenRight64)
{
	RunFamily(kLeftRight64, 0x64736c6c64737261ull);
}

TEST(EeRecShiftPair, RightThenMask)
{
	RunFamily(kRightMask, 0x7372616e6469ull);
}

TEST(EeRecShiftPair, MaskThenLeft)
{
	RunFamily(kMaskLeft, 0x616e64697366ull);
}

// Every pair, seeded source, alone in its block. A pair that fuses is one
// shift-class instruction. One that does not is the first instruction's and
// the second's, which is nothing for a 64-bit shift by 0 of a register onto
// itself. The block's tail adds its own, counted from a block of NOPs.
TEST(EeRecShiftPair, FusedPairIsOneInstruction)
{
	ResetRecAndPageProtection();
	const bool dump = std::getenv("EESHIFTPAIR_DUMP") != nullptr;
	u32 fused = 0, unfused = 0, wrong = 0;
	u32 floor;
	{
		EeRecTestHarness h;
		h.LoadProgram({NOP, NOP});
		h.Run();
		floor = ShapeAt(kPc).ops;
	}
	for (const Pair& pr : AllPairs())
	{
		const u32 first = WithRegs(pr.first, reg::t0, reg::t1);
		const u32 second = WithRegs(pr.second, reg::t1, reg::t1);
		EeRecTestHarness h;
		h.SetGpr64(reg::t0, 0x0123456789ABCDEFull);
		h.LoadProgram({first, second});
		h.Run();
		const Shape s = ShapeAt(kPc);
		ASSERT_TRUE(s.found);
		const u32 want = floor + (pr.fuses ? 1 : 1 + !Is64BitShiftByZero(pr.second));
		fused += pr.fuses;
		unfused += !pr.fuses;
		const bool ok = s.ops == want;
		if (!ok && wrong++ == 0)
			ADD_FAILURE() << kFamilyName[pr.family] << std::hex << " first=0x" << first << " second=0x" << second
						  << std::dec << ": " << s.ops << " shift-class and mov instructions, want " << want;
		if (dump || (!ok && wrong == 1))
			DumpBlock(kPc);
	}
	EXPECT_EQ(wrong, 0u);
	EXPECT_EQ(floor, 1u);
	EXPECT_EQ(fused, 2048u + 8190u + 2832u + 1536u);
	EXPECT_EQ(unfused, AllPairs().size() - fused);
}

// The second instruction is still charged, and the instruction after the
// pair still runs: a fused pair costs the cycles of the same pair writing
// another register.
TEST(EeRecShiftPair, FusedPairChargesBothInstructions)
{
	ResetRecAndPageProtection();
	const std::pair<u32, u32> pairs[] = {
		{WithAmount(kSll, 16), WithAmount(kSra, 16)},
		{WithAmount(kSrl, 8), WithMask(0xFF)},
		{WithMask(0xFF), WithAmount(kSll, 4)},
		{WithAmount(kDsll32, 0), WithAmount(kDsra32, 0)},
	};
	constexpr u64 kV = 0xFEDCBA9876543210ull;
	for (const auto& [first, second] : pairs)
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "first=0x" << first << " second=0x" << second);
		u64 cycles[2];
		for (u32 twin = 0; twin < 2; twin++)
		{
			const u32 dest = twin ? reg::t2 : reg::t1;
			EeRecTestHarness h;
			h.SetGpr64(reg::t0, kV);
			h.LoadProgram({WithRegs(first, reg::t0, reg::t1), WithRegs(second, reg::t1, dest),
				e::DADDU(reg::t3, dest, dest)});
			h.Run();
			cycles[twin] = h.GetCycleDeltaJit();
			EXPECT_EQ(h.GetGpr64Jit(reg::t3), 2 * Apply(second, Apply(first, kV)));
		}
		EXPECT_EQ(cycles[0], cycles[1]);
		EXPECT_GT(cycles[0], 0u);
	}
}

TEST(EeRecShiftPair, PairOntoR0WritesNothing)
{
	ResetRecAndPageProtection();
	for (const auto& [first, second] : {std::pair{WithAmount(kSll, 16), WithAmount(kSra, 16)},
			 std::pair{WithMask(0xFF), WithAmount(kSll, 4)}, std::pair{WithAmount(kDsll32, 0), WithAmount(kDsra32, 0)}})
	{
		SCOPED_TRACE(::testing::Message() << std::hex << "first=0x" << first << " second=0x" << second);
		EeRecTestHarness h;
		h.SetGpr64(reg::t0, 0xFEDCBA9876543210ull);
		h.LoadProgram({WithRegs(first, reg::t0, reg::zero), WithRegs(second, reg::zero, reg::zero),
			e::DADDU(reg::t1, reg::zero, reg::t0)});
		h.Run();
		h.ExpectGpr64(reg::zero, 0);
		h.ExpectGpr64(reg::t1, 0xFEDCBA9876543210ull);
	}
}

// A second instruction forced to the interpreter runs there on the first
// one's result, so the first emits what it emits alone.
TEST(EeRecShiftPair, FallbackSecondHalfDoesNotFuse)
{
	struct Row
	{
		u32 group;
		u32 first, second;
	};
	const Row rows[] = {
		{EERecFallback::Shift, WithMask(0x0FFF), WithAmount(kSll, 20)},
		{EERecFallback::Shift, WithMask(0x00FF), WithAmount(kDsll32, 3)},
		{EERecFallback::Arith, WithAmount(kSrl, 7), WithMask(0x001F)},
		{EERecFallback::Arith, WithAmount(kDsra32, 2), WithMask(0x00FF)},
	};
	constexpr u64 kV = 0x89ABCDEF12345678ull;
	const u32 saved = EERecFallback::g_groups;
	for (const Row& row : rows)
	{
		const u32 first = WithRegs(row.first, reg::t0, reg::t1);
		const u32 second = WithRegs(row.second, reg::t1, reg::t1);
		SCOPED_TRACE(::testing::Message() << std::hex << "first=0x" << first << " second=0x" << second);
		Shape alone, native, forced;
		for (u32 leg = 0; leg < 3; leg++)
		{
			EERecFallback::g_groups = leg == 2 ? row.group : 0;
			ResetRecAndPageProtection();
			EeRecTestHarness h;
			h.SetGpr64(reg::t0, kV);
			if (leg == 0)
				h.LoadProgram({first});
			else
				h.LoadProgram({first, second});
			h.Run();
			(leg == 0 ? alone : leg == 1 ? native : forced) = ShapeAt(kPc);
			EERecFallback::g_groups = saved;
			if (leg != 0)
				EXPECT_EQ(h.GetGpr64Jit(reg::t1), Apply(second, Apply(first, kV)));
		}
		EXPECT_GT(forced.calls, native.calls) << "the second instruction went to the interpreter";
		EXPECT_NE(native.first_shift_class, alone.first_shift_class) << "the pair fuses natively";
		EXPECT_EQ(forced.first_shift_class, alone.first_shift_class);
	}
	EERecFallback::g_groups = saved;
	ResetRecAndPageProtection();
}
