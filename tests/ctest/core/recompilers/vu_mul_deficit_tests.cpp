// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The EE multiplier's one-ULP deficit, on the VU's two emitters.
//
// The interpreter routes every VU product through EeFpuModel::Mul, so a
// JIT-vs-interpreter differential over multiply operands scores the emitters
// against the console's multiply array (FPU.cpp eeMulArray, bit-exact on the
// 100663296 rows of captures/fpmul).
//
// Random operand pairs do not reach it. The deficit is decided by ft alone
// wherever the exact product is representable in single, and that is 7 in
// 4000000 random pairs against 21-31% of a real game's stream. The generator
// below builds the regime instead: one operand with `s` trailing zero mantissa
// bits against one with 25 - s, walked over the whole split, plus the fs = 1.0
// row the console sweep measured densest.
//
// What the emitters carry, and where the rungs sit:
//
//   vuClampMode <= 3   nothing
//   vuClampMode 4      the mask on ft's mantissa bits 1,3,5,7,9, and over it
//                      the boundary term on bits 11..15, which closes the
//                      zero-tail regime exactly
//
// Four tables cover operands the predicate over ft cannot decide on its own,
// named by what the exact 48-bit mantissa product looks like:
//
//   long-tail        bits below the result's last bit, 0x8000 or more.
//                    eeMulRound does not decrement these either.
//   short-tail       those bits non-zero but below 0x8000. The array's
//                    truncation still crosses, so only eeMulOneUlpLow decides.
//   low-exp          product below 2^-79. The emitted exactness test does not
//                    work there, because the rounding error it measures has
//                    itself flushed to zero.
//   low-short-tail   both at once. Both emitters call eeMulOneUlpLow for the
//                    middle two; this one keeps the lanes they still miss.
//
// Only the COP2 macro path calls eeMulOneUlpLow, so the last three are where
// the two emitters differ.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"
#include "harness/VuTestHarness.h"
#include "harness/VuEncode.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFs = 1, kFt = 2, kAcc = 3, kFd = 4, kZero = 7;

struct Row
{
	u32 fs, ft;
};

// Mantissas whose product is exact in single: `s` trailing zeros against
// 25 - s. `e` is the exponent field both operands take.
void AppendSplit(std::vector<Row>& out, std::mt19937& rng, u32 e, int per_split)
{
	const u32 exp = e << 23;
	for (u32 sa = 1; sa <= 22; sa++)
	{
		const u32 sb = 25 - sa;
		if (sb > 23)
			continue;
		for (int i = 0; i < per_split; i++)
		{
			const u32 ma = static_cast<u32>(((0x800000u | (rng() & 0x7FFFFFu)) >> sa) << sa);
			const u32 mb = static_cast<u32>(((0x800000u | (rng() & 0x7FFFFFu)) >> sb) << sb);
			const u32 sgn = rng();
			out.push_back({exp | (ma & 0x7FFFFFu) | ((sgn & 1) << 31),
				exp | (mb & 0x7FFFFFu) | ((sgn & 2) << 30)});
		}
	}
}

// The regime the emitters model: exponents well inside the range, so nothing
// here saturates, underflows, or loses the FMLS residue.
const std::vector<Row>& ZeroTail()
{
	static const std::vector<Row> v = [] {
		std::mt19937 rng(0x5eed1234u);
		std::vector<Row> out;
		for (int i = 0; i < 50; i++)
		{
			const u32 ft = static_cast<u32>(0x3f800000u | (rng() & 0x7FFFFFu));
			out.push_back({0x3f800000u, ft});
			out.push_back({0xbf800000u, ft});
		}
		for (u32 e : {100u, 127u, 160u})
			AppendSplit(out, rng, e, 3);
		return out;
	}();
	return v;
}

// Exact products too small for the residue to survive FZ, and the binade above
// them where it does. The product's exponent is 2*e - 127, so e = 87 puts it at
// 47 and e = 96 at 65.
const std::vector<Row>& LowExponent()
{
	static const std::vector<Row> v = [] {
		std::mt19937 rng(0x10a0e2f3u);
		std::vector<Row> out;
		for (u32 e : {80u, 87u, 88u, 96u})
			AppendSplit(out, rng, e, 2);
		return out;
	}();
	return v;
}

// Products that need more than 24 bits: no predicate over ft decides these.
const std::vector<Row>& LongTail()
{
	static const std::vector<Row> v = [] {
		std::mt19937 rng(0xabcd0001u);
		std::vector<Row> out;
		for (int i = 0; i < 200; i++)
			out.push_back({static_cast<u32>(0x3f800000u | (rng() & 0x7FFFFFu)),
				static_cast<u32>(0x3f800000u | (rng() & 0x7FFFFFu))});
		return out;
	}();
	return v;
}

// Operand pairs whose exact mantissa product has bits below the result's last
// bit, but fewer than 0x8000 of them. eeMulOneUlpLow only stops decrementing
// once those bits reach 0x8000, so the emitted model, which decrements nothing
// when they are non-zero at all, disagrees with it here.
const std::vector<Row>& ShortTail()
{
	static const std::vector<Row> v = [] {
		std::mt19937 rng(0x51012a11u);
		std::vector<Row> out;
		while (out.size() < 200)
		{
			const u32 fs = 0x3f800000u | (rng() & 0x7FFFFFu);
			const u32 ft = 0x3f800000u | (rng() & 0x7FFFFFu);
			const u64 a = 0x800000u | (fs & 0x7FFFFFu);
			const u64 b = 0x800000u | (ft & 0x7FFFFFu);
			const u64 prod = a * b;
			const int k = (prod >> 47) ? 24 : 23;
			const u64 tail = prod & ((1ull << k) - 1u);
			if (tail == 0 || tail >= 0x8000u)
				continue;
			out.push_back({fs, ft});
		}
		return out;
	}();
	return v;
}

// Both of the above at once: a product below 2^-79 whose exact mantissa product
// also has non-zero bits below 0x8000. This is where the emitters still differ
// from the interpreter, because the lanes they send to eeMulOneUlpLow are
// selected using the predicate over ft, and a lane that predicate rejects is
// never sent even when the array would have decremented it.
//
// Neither table above samples this: ShortTail() uses exponent 127 and
// LowExponent() builds products with no bits below the last one.
const std::vector<Row>& LowShortTail()
{
	static const std::vector<Row> v = [] {
		std::mt19937 rng(0x10517a11u);
		std::vector<Row> out;
		// Exponents 64 to 79 put the product's field at 1 to 31, where a tail
		// under 0x8000 makes a residue under 2^-126 whatever the tail is, so
		// every row here has a residue the arithmetic has flushed.
		while (out.size() < 400)
		{
			const u32 e = (64u + (out.size() & 15u)) << 23;
			const u32 fs = e | (rng() & 0x7FFFFFu);
			const u32 ft = e | (rng() & 0x7FFFFFu);
			const u64 a = 0x800000u | (fs & 0x7FFFFFu);
			const u64 b = 0x800000u | (ft & 0x7FFFFFu);
			const u64 prod = a * b;
			const int k = (prod >> 47) ? 24 : 23;
			const u64 tail = prod & ((1ull << k) - 1u);
			if (tail == 0 || tail >= 0x8000u)
				continue;
			out.push_back({fs, ft});
		}
		return out;
	}();
	return v;
}

// Zeros, denormals, the smallest normal, both ends of the range and the two
// exponent-255 words the VU reads as ordinary numbers -- crossed with a normal
// operand and with each other. Nothing here may take a decrement.
//
// Split at exponent 255, because the two halves test different things. Below
// it both emitters multiply the architectural words, so the deficit model
// decides the result. At exponent 255 the word is an Inf or NaN to the host and
// both emitters bound it before multiplying -- differently -- so those rows
// score the operand clamp rather than this model.
constexpr bool TopBinade(u32 w) { return ((w >> 23) & 0xFFu) == 0xFFu; }

const u32 kEdgeWords[] = {
	0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00800000u,
	0x80800000u, 0x3f800000u, 0xbf800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
	0x7F800000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x49998921u, 0x3fAAAAABu,
};

std::vector<Row> CrossEdgeWords(bool topBinade)
{
	std::vector<Row> out;
	for (u32 a : kEdgeWords)
		for (u32 b : kEdgeWords)
			if ((TopBinade(a) || TopBinade(b)) == topBinade)
				out.push_back({a, b});
	return out;
}

const std::vector<Row>& Edges()
{
	static const std::vector<Row> v = CrossEdgeWords(false);
	return v;
}

const std::vector<Row>& TopBinadeEdges()
{
	static const std::vector<Row> v = CrossEdgeWords(true);
	return v;
}

// ---- the ops ----------------------------------------------------------

struct Shape
{
	const char* name;
	u32 macro;   // COP2 macro word
	u32 micro;   // microVU upper word
	bool q;      // seeds VI[REG_Q] with ft
	bool i;      // seeds VI[REG_I] with ft
	bool acc;    // the result lands in ACC rather than VF[fd]
	u32 dest;
};

std::vector<Shape> Shapes(u32 dest)
{
	using namespace vu;
	const u32 d = dest;
	const u32 m = dest << 21; // the micro encoders take the field in place
	return {
		{"MUL",    VMUL_C2(d, kFd, kFs, kFt),  VMUL_U(m, kFd, kFs, kFt),  false, false, false, d},
		{"MULx",   VMULx_C2(d, kFd, kFs, kFt), VMULx_U(m, kFd, kFs, kFt), false, false, false, d},
		{"MULw",   VMULw_C2(d, kFd, kFs, kFt), VMULw_U(m, kFd, kFs, kFt), false, false, false, d},
		{"MULq",   VMULq_C2(d, kFd, kFs),      VMULq_U(m, kFd, kFs),      true,  false, false, d},
		{"MULi",   VMULi_C2(d, kFd, kFs),      VMULi_U(m, kFd, kFs),      false, true,  false, d},
		{"MADD",   VMADD_C2(d, kFd, kFs, kFt), VMADD_U(m, kFd, kFs, kFt), false, false, false, d},
		{"MSUB",   VMSUB_C2(d, kFd, kFs, kFt), VMSUB_U(m, kFd, kFs, kFt), false, false, false, d},
		{"MADDx",  VMADDx_C2(d, kFd, kFs, kFt), VMADDx_U(m, kFd, kFs, kFt), false, false, false, d},
		{"MSUBw",  VMSUBw_C2(d, kFd, kFs, kFt), VMSUBw_U(m, kFd, kFs, kFt), false, false, false, d},
		{"MADDq",  VMADDq_C2(d, kFd, kFs),     VMADDq_U(m, kFd, kFs),     true,  false, false, d},
		{"MSUBi",  VMSUBi_C2(d, kFd, kFs),     VMSUBi_U(m, kFd, kFs),     false, true,  false, d},
		{"OPMSUB", VOPMSUB_C2(0xF, kFd, kFs, kFt), VOPMSUB_U(mask::xyzw, kFd, kFs, kFt), false, false, false, 0xF},
		{"MULA",   VMULA_C2(d, kFs, kFt),      VMULA_U(m, kFs, kFt),      false, false, true,  d},
		{"MULAx",  VMULAx_C2(d, kFs, kFt),     VMULAx_U(m, kFs, kFt),     false, false, true,  d},
		{"MULAq",  VMULAq_C2(d, kFs),          VMULAq_U(m, kFs),          true,  false, true,  d},
		{"MULAi",  VMULAi_C2(d, kFs),          VMULAi_U(m, kFs),          false, true,  true,  d},
		{"MADDA",  VMADDA_C2(d, kFs, kFt),     VMADDA_U(m, kFs, kFt),     false, false, true,  d},
		{"MSUBA",  VMSUBA_C2(d, kFs, kFt),     VMSUBA_U(m, kFs, kFt),     false, false, true,  d},
		{"OPMULA", VOPMULA_C2(0xF, kFs, kFt),  VOPMULA_U(mask::xyzw, kFs, kFt),  false, false, true,  0xF},
	};
}

// A lane the op never wrote still carries its seed, and two engines that both
// did nothing agree. `stale` counts those lanes; an op that wrote none of them
// is a mis-encoded word reading as agreement, which is what happened to the
// microVU column here before the dest mask was moved into place. An accumulate
// can leave the seed standing honestly -- 0.5 plus a small enough product is
// 0.5 -- so the clause is that SOME lane moved, not that none stayed.
constexpr u32 kSeed = 0xDEADBEEFu;
constexpr u32 kAccSeed = 0x3f000000u;

struct Score
{
	int bad = 0;
	int stale = 0;
	int lanes = 0;
};

// One COP2 macro op, JIT against interpreter, value column only.
Score ScoreMacro(const std::vector<Row>& rows, const Shape& s, int mode)
{
	Score sc;
	for (const Row& r : rows)
	{
		EeRecTestHarness h;
		h.SetVu0ClampMode(mode);
		h.EnableVu0Capture();
		h.ExpectVu0Divergence();
		h.EnableCop1();
		h.SeedVu0VfBits(kFs, r.fs, r.fs, r.fs, r.fs);
		h.SeedVu0VfBits(kFt, r.ft, r.ft, r.ft, r.ft);
		h.SeedVu0VfBits(kZero, 0, 0, 0, 0);
		h.SeedVu0VfBits(kFd, kSeed, kSeed, kSeed, kSeed);
		h.SeedVu0AccBits(kAccSeed, kAccSeed, kAccSeed, kAccSeed);
		if (s.q)
			h.SeedVu0Vi(REG_Q, r.ft);
		if (s.i)
			h.SeedVu0Vi(REG_I, r.ft);
		h.LoadProgram({s.macro});
		h.Run();

		for (int lane = 0; lane < 4; lane++)
		{
			if (!((s.dest >> (3 - lane)) & 1))
				continue;
			const char ln = "xyzw"[lane];
			const u32 j = s.acc ? h.GetVu0AccBitsJit(ln) : h.GetVu0VfBitsJit(kFd, ln);
			const u32 i = s.acc ? h.GetVu0AccBitsInterp(ln) : h.GetVu0VfBitsInterp(kFd, ln);
			sc.lanes++;
			if (i == (s.acc ? kAccSeed : kSeed))
				sc.stale++;
			if (j != i)
			{
				sc.bad++;
				break;
			}
		}
	}
	return sc;
}

// The same op as a VU0 microprogram.
Score ScoreMicro(const std::vector<Row>& rows, const Shape& s, int mode)
{
	using namespace vu;
	Score sc;
	for (const Row& r : rows)
	{
		VuTestHarness h(0);
		h.SetVuClampMode(mode);
		h.SetVfBits(kFs, r.fs, r.fs, r.fs, r.fs);
		h.SetVfBits(kFt, r.ft, r.ft, r.ft, r.ft);
		h.SetVfBits(kAcc, kAccSeed, kAccSeed, kAccSeed, kAccSeed);
		h.SetVfBits(kZero, 0, 0, 0, 0);
		h.SetVfBits(kFd, kSeed, kSeed, kSeed, kSeed);
		if (s.q)
			h.SetQ(r.ft);
		if (s.i)
			h.SetVi(REG_I, r.ft);
		h.LoadProgram({
			VuOp{0u, VADDA_U(mask::xyzw, kAcc, kZero)},
			VuOp{0u, s.micro},
			VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()}, VuOp{0u, VNOP_U()},
			EBitNopPair(),
		});
		h.RunNoDiff();

		for (int lane = 0; lane < 4; lane++)
		{
			if (!((s.dest >> (3 - lane)) & 1))
				continue;
			const u32 j = s.acc ? h.JitSnapshot().regs.ACC.UL[lane]
			                    : h.JitSnapshot().regs.VF[kFd].UL[lane];
			const u32 i = s.acc ? h.InterpSnapshot().regs.ACC.UL[lane]
			                    : h.InterpSnapshot().regs.VF[kFd].UL[lane];
			sc.lanes++;
			if (i == (s.acc ? kAccSeed : kSeed))
				sc.stale++;
			if (j != i)
			{
				sc.bad++;
				break;
			}
		}
	}
	return sc;
}

// ---- the grid ---------------------------------------------------------

enum { kZeroTail, kLowExp, kLongTail, kShortTail, kLowShortTail, kEdges, kTopBinade,
	kTableCount };

const char* kTableName[] = {"zero-tail", "low-exp", "long-tail", "short-tail",
	"low-short-tail", "edges", "top-binade"};

const std::vector<Row>& Table(int t)
{
	switch (t)
	{
		case kZeroTail: return ZeroTail();
		case kLowExp:   return LowExponent();
		case kLongTail: return LongTail();
		case kShortTail: return ShortTail();
		case kLowShortTail: return LowShortTail();
		case kEdges:    return Edges();
		default:        return TopBinadeEdges();
	}
}

struct Grid
{
	int bad[kTableCount][5][2]{};   // [table][mode][macro=0, micro=1]
	int lanes[kTableCount][5][2]{};
	int stale[kTableCount][5][2]{};
};

// Every shape at both a full and a partial dest field, summed. The two dest
// fields are not decoration: a partial one is where the destination register
// and the clamped Fs are the same, which is the shape the deficit has to
// compute its product twice for.
const Grid& Measured()
{
	static const Grid g = [] {
		Grid out;
		for (u32 dest : {0xFu, 0xEu})
		{
			for (const Shape& sh : Shapes(dest))
			{
				for (int t = 0; t < kTableCount; t++)
				{
					for (int mode = 1; mode <= 4; mode++)
					{
						const Score ma = ScoreMacro(Table(t), sh, mode);
						const Score mi = ScoreMicro(Table(t), sh, mode);
						const Score* pair[2] = {&ma, &mi};
						for (int e = 0; e < 2; e++)
						{
							out.bad[t][mode][e] += pair[e]->bad;
							out.lanes[t][mode][e] += pair[e]->lanes;
							out.stale[t][mode][e] += pair[e]->stale;
						}
					}
				}
			}
		}
		return out;
	}();
	return g;
}

// What the grid comes to, per table and mode, summed over every shape and both
// dest fields, as {macro, micro}. Regenerate from DISABLED_Measure; do not
// hand-edit.
//
// The two emitters produce the same count on every table but the last, which
// holds the exponent-255 operands and scores the operand clamp rather than this
// model.
//
// The macro path bounds what the mVU clampType row it mirrors names, at every
// vuClampMode. Where the row names nothing -- mVU_MADD, mVU_MULA, mVU_MADDA,
// mVU_MSUBA, mVU_MSUBw, and the OP ops, which reach NEON_MULPS with no row --
// the word arrives at Fmul as a host NaN and the result clamp folds the product
// to +/-FLT_MAX where the interpreter multiplies two numbers. microVU bounds by
// mode instead: mVUclamp3 takes every operand from vuClampMode 2 up whatever
// the row says, and below vuClampMode 3 the bound is mVUclamp1's Fminnm/Fmaxnm
// pair, which loses an exponent-255 word's sign. So the macro column is the
// larger of the two at every mode, and the shapes where the micro column is
// larger are confined to modes 1 and 2, where the sign goes.
//
// vu_micro_fmac_console_tests.cpp scores that clamp against the console; this
// file only has to leave it alone.
//
// The mismatches the in-range edge table still has at mode 4 come from the
// result, not from the multiply array: the product, or the MADD sum it feeds,
// lands above FLT_MAX and below 0x7FFFFFFF, which single precision cannot
// represent. It used to have 80 more, from products below 2^-79, which
// the COP2 macro path now passes to eeMulOneUlpLow and microVU does not; that
// is the whole difference between the two columns.
constexpr int kBad[kTableCount][5][2] = {
	//  -        mode1          mode2          mode3        mode4
	{{0,0}, {6920,6920}, {6920,6920}, {6920,6920}, {0,0}},       // zero-tail
	{{0,0}, {2904,2904}, {2904,2904}, {2904,2904}, {0,500}},     // low-exp
	{{0,0}, {0,0}, {0,0}, {0,0}, {0,0}},                         // long-tail
	{{0,0}, {2744,2744}, {2744,2744}, {2744,2744}, {0,2744}},    // short-tail
	{{0,0}, {6920,6920}, {6920,6920}, {6920,6920}, {20,2920}},   // low-short-tail
	{{0,0}, {1110,1110}, {1110,1110}, {1110,1110}, {296,376}},   // edges
	{{0,0}, {2742,2402}, {2742,2326}, {2742,2166}, {2242,1546}}, // top-binade
};

} // namespace

// What each encoded word actually is. A wrong funct field decodes as a
// different op, and a JIT-versus-interpreter differential cannot see that --
// both engines decode the same word. Every shape but the two OP ops multiplies
// 2.0 by 3.0 and, where it accumulates, against an ACC of 0.5; the OP ops
// rotate their operands, so all that is asked of them is that they wrote.
TEST(VuMulDeficit, EveryShapeIsTheOpItNames)
{
	const std::vector<Row> row = {{0x40000000u, 0x40400000u}}; // 2.0, 3.0
	for (const Shape& sh : Shapes(0xF))
	{
		SCOPED_TRACE(sh.name);
		EeRecTestHarness h;
		h.SetVu0ClampMode(4);
		h.EnableVu0Capture();
		h.ExpectVu0Divergence();
		h.EnableCop1();
		h.SeedVu0VfBits(kFs, row[0].fs, row[0].fs, row[0].fs, row[0].fs);
		h.SeedVu0VfBits(kFt, row[0].ft, row[0].ft, row[0].ft, row[0].ft);
		h.SeedVu0VfBits(kFd, kSeed, kSeed, kSeed, kSeed);
		h.SeedVu0AccBits(kAccSeed, kAccSeed, kAccSeed, kAccSeed);
		if (sh.q)
			h.SeedVu0Vi(REG_Q, row[0].ft);
		if (sh.i)
			h.SeedVu0Vi(REG_I, row[0].ft);
		h.LoadProgram({sh.macro});
		h.Run();

		const u32 got = sh.acc ? h.GetVu0AccBitsInterp('x') : h.GetVu0VfBitsInterp(kFd, 'x');
		const std::string name = sh.name;
		if (name.rfind("OP", 0) == 0)
		{
			EXPECT_NE(got, sh.acc ? kAccSeed : kSeed) << "wrote nothing";
			continue;
		}
		u32 want = 0x40C00000u; // 6.0
		if (name.rfind("MADD", 0) == 0)
			want = 0x40D00000u; // 0.5 + 6.0
		else if (name.rfind("MSUB", 0) == 0)
			want = 0xC0B00000u; // 0.5 - 6.0
		EXPECT_EQ(got, want);
	}
}

// The regime the model is a model of. Both emitters have to answer what the
// interpreter answers on every row of it at vuClampMode 4, and the interpreter
// is EeFpuModel::Mul, so this is the console's array.
TEST(VuMulDeficit, ExactProductsMatchAtModeFour)
{
	const Grid& g = Measured();
	for (int e = 0; e < 2; e++)
	{
		SCOPED_TRACE(e ? "micro" : "macro");
		EXPECT_EQ(g.bad[kZeroTail][4][e], 0);
		EXPECT_GT(g.lanes[kZeroTail][4][e] - g.stale[kZeroTail][4][e], 0)
			<< "the table wrote nothing, so it compared a seed with a seed";
	}
}

// The gate. Modes 1 to 3 emit none of it; 4 emits the whole ft-only law and
// closes the regime. The zero-tail table is where that shows: every row of it
// is a product the model moves, so a mode that emits nothing has to come back
// with mode 1's count and mode 4 with none.
TEST(VuMulDeficit, OnlyModeFourEmitsIt)
{
	const Grid& g = Measured();
	for (int t = 0; t < kTableCount; t++)
	{
		SCOPED_TRACE(kTableName[t]);
		for (int mode = 1; mode <= 4; mode++)
		{
			SCOPED_TRACE(::testing::Message() << "vuClampMode " << mode);
			EXPECT_EQ(g.bad[t][mode][0], kBad[t][mode][0]) << "macro";
			EXPECT_EQ(g.bad[t][mode][1], kBad[t][mode][1]) << "micro";
		}
	}
	for (int e = 0; e < 2; e++)
	{
		SCOPED_TRACE(e ? "micro" : "macro");
		for (int mode = 2; mode <= 3; mode++)
			EXPECT_EQ(g.bad[kZeroTail][mode][e], g.bad[kZeroTail][1][e])
				<< "vuClampMode " << mode << " emitted some of it";
		EXPECT_GT(g.bad[kZeroTail][3][e], g.bad[kZeroTail][4][e]) << "mode 4 emitted none of it";
	}
}

// The same model on both emitters. The top-binade table is excluded for the
// reason given above kBad; on every other table the COP2 macro path and microVU
// must produce the same count at every mode. A difference would mean the model
// was wired into one emitter and not the other.
TEST(VuMulDeficit, BothEmittersAnswerAlike)
{
	const Grid& g = Measured();
	for (int t = 0; t < kTopBinade; t++)
	{
		if (t == kShortTail || t == kLowExp || t == kLowShortTail || t == kEdges)
			continue; // only the COP2 macro path calls eeMulOneUlpLow
		for (int mode = 1; mode <= 4; mode++)
		{
			SCOPED_TRACE(::testing::Message() << kTableName[t] << " vuClampMode " << mode);
			EXPECT_EQ(g.bad[t][mode][0], g.bad[t][mode][1]);
		}
	}
}

// eeMulOneUlpLow does not decrement when the bits below the result's last bit
// reach 0x8000, and neither does the emitted model, so both emitters agree with
// the interpreter on this table at every mode.
TEST(VuMulDeficit, NothingFiresOnALongTail)
{
	const Grid& g = Measured();
	for (int e = 0; e < 2; e++)
	{
		SCOPED_TRACE(e ? "micro" : "macro");
		for (int mode = 1; mode <= 4; mode++)
			EXPECT_EQ(g.bad[kLongTail][mode][e], 0) << "vuClampMode " << mode;
	}
}

// Below 2^-79 the rounding error the exactness test measures has itself flushed
// to zero, so the emitted model cannot distinguish an exact product from a
// rounded one and its exponent test rejects the lane. eeMulOneUlpLow recomputes
// the mantissa product instead. The COP2 macro path passes it these
// lanes and matches the interpreter; microVU still rejects them.
TEST(VuMulDeficit, TheLowExponentRegime)
{
	const Grid& g = Measured();
	EXPECT_EQ(g.bad[kLowExp][4][0], 0) << "macro";
	EXPECT_GT(g.lanes[kLowExp][4][0] - g.stale[kLowExp][4][0], 0) << "wrote nothing";
	for (int mode = 1; mode <= 3; mode++)
		EXPECT_EQ(g.bad[kLowExp][mode][0], g.bad[kLowExp][1][0])
			<< "macro vuClampMode " << mode;
	EXPECT_GT(g.bad[kLowExp][4][1], 0) << "micro stops at the floor";
	EXPECT_LT(g.bad[kLowExp][4][1], g.bad[kLowExp][1][1]) << "the floor swallowed the model";
}

// The lanes that selection still misses. Which lanes are passed to
// eeMulOneUlpLow is decided using the emitted model's own condition, so a lane
// the predicate over ft rejects is never passed, even below 2^-79 where the
// array would have decremented it. Selecting on the product's exponent alone
// would need that exponent live alongside the condition and the constant 48,
// which is one more scratch register than the COP2 macro path has.
//
// 20 lanes of 6920. The table has to be built for this case specifically: a
// first version at a single exponent found none of them in 200 rows.
TEST(VuMulDeficit, TheRoutingIsMasked)
{
	const Grid& g = Measured();
	EXPECT_GT(g.bad[kLowShortTail][4][0], 0) << "macro closed the corner";
	EXPECT_LT(g.bad[kLowShortTail][4][0], g.bad[kLowShortTail][4][1] / 10)
		<< "macro no longer routes most of it";
	for (int mode = 1; mode <= 3; mode++)
		EXPECT_EQ(g.bad[kLowShortTail][mode][0], g.bad[kLowShortTail][1][0])
			<< "macro vuClampMode " << mode;
}

// Tails below 0x8000, and the clamp mode that gates the whole model.
TEST(VuMulDeficit, TheShortTailBand)
{
	const Grid& g = Measured();
	EXPECT_EQ(g.bad[kShortTail][4][0], 0) << "macro";
	EXPECT_GT(g.lanes[kShortTail][4][0] - g.stale[kShortTail][4][0], 0) << "wrote nothing";
	for (int mode = 1; mode <= 3; mode++)
	{
		EXPECT_EQ(g.bad[kShortTail][mode][0], g.bad[kShortTail][1][0])
			<< "macro vuClampMode " << mode;
		EXPECT_GT(g.bad[kShortTail][mode][0], 0) << "macro vuClampMode " << mode;
	}
	for (int mode = 1; mode <= 4; mode++)
		EXPECT_EQ(g.bad[kShortTail][mode][1], g.bad[kShortTail][1][1])
			<< "micro vuClampMode " << mode;
	EXPECT_GT(g.bad[kShortTail][4][1], 0) << "micro carries no guard";
}

// Regenerates kBad, then prints the same grid split by op and dest field --
// which is where a single emitter that missed the model shows up.
TEST(VuMulDeficit, DISABLED_Measure)
{
	const Grid& g = Measured();
	std::printf("\nconstexpr int kBad[kTableCount][5] = {\n\t//  -   mode1   mode2   mode3   mode4\n");
	for (int t = 0; t < kTableCount; t++)
	{
		std::printf("\t{{0,0}, {%d,%d}, {%d,%d}, {%d,%d}, {%d,%d}}, // %s\n",
			g.bad[t][1][0], g.bad[t][1][1], g.bad[t][2][0], g.bad[t][2][1],
			g.bad[t][3][0], g.bad[t][3][1], g.bad[t][4][0], g.bad[t][4][1], kTableName[t]);
	}
	std::printf("};\n\n");
{
	struct Table { const char* name; const std::vector<Row>& rows; };
	const Table tables[] = {
		{"zero-tail", ZeroTail()},
		{"low-exp", LowExponent()},
		{"long-tail", LongTail()},
		{"short-tail", ShortTail()},
		{"low-short-tail", LowShortTail()},
		{"edges", Edges()},
		{"top-binade", TopBinadeEdges()},
	};
	for (u32 dest : {0xFu, 0xEu})
	{
		for (const Shape& s : Shapes(dest))
		{
			for (const Table& t : tables)
			{
				std::printf("dest %x %-7s %-10s rows %4zu |", dest, s.name, t.name, t.rows.size());
				for (int mode = 1; mode <= 4; mode++)
				{
					const Score ma = ScoreMacro(t.rows, s, mode);
					const Score mi = ScoreMicro(t.rows, s, mode);
					std::printf(" m%d macro %4d micro %4d |", mode, ma.bad, mi.bad);
					EXPECT_LT(ma.stale, ma.lanes) << "macro " << s.name << " wrote nothing";
					EXPECT_LT(mi.stale, mi.lanes) << "micro " << s.name << " wrote nothing";
				}
				std::printf("\n");
			}
		}
	}
	}
}

} // namespace recompiler_tests
