// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// microVU's MAC U and MAC O, scored against the interpreter.
//
// The interpreter reads both bits off the exact result. microVU read neither:
// its only overflow was the VUOverflowHack gamefix's |result| >= FLT_MAX, and
// it had no underflow at all. It now emits the same three models the COP2 macro
// path does (VuFmacFlags-arm64.h) at vuClampMode 4, from the operands, before
// the clamp that used to put an exponent-255 operand out of reach.
//
// Dimensions crossed below: opcode family (MUL / ADD / SUB, their broadcast,
// I and Q forms, and their ACC-writing A-forms, plus OPMULA); destination field
// (full, each single lane -- the shape whose weight vector folds a rotate --
// and two partials); register aliasing (fd == fs, fd == ft, fs == ft); operand
// exponents drawn both uniformly and from a band straddling the O and U
// thresholds, so a row lands on either side of each; mantissas at 0, at their
// maximum and random; and both signs, which is the half of the adder's rule a
// magnitude test alone gets wrong. Both VUs, because the gate reads
// CHECK_VU_EXACT(vunum) and VU1 is the one microVU exists for.
//
// The two engines still disagree on the VALUE in the top binade -- microVU
// clamps operands to FLT_MAX where the console's range runs to 0x7FFFFFFF --
// so the flags are scored on their own, and the whole per-lane ZSUO group only
// where the two wrote the same word. A lane that raised O is the exception:
// there the word IS the top of the range, and O is what picks it, so the two
// have to agree.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <random>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return VuOp{0u, upper}; }
inline VuOp BareNopPair() { return VuOp{0u, VNOP_U()}; }

// MAC: Z [3:0], S [7:4], U [11:8], O [15:12], bit 0 = lane W.
constexpr u32 kMacU = 0x0F00u;
constexpr u32 kMacO = 0xF000u;

// STATUS, normalized: U bit 2, O bit 3, US bit 8, OS bit 9.
constexpr u32 kStatU = (1u << 2) | (1u << 8);
constexpr u32 kStatO = (1u << 3) | (1u << 9);

// The lane's four MAC bits, one per nibble. Lane 0 (x) is bit 3.
constexpr u32 LaneMacBits(int lane)
{
	const u32 b = 1u << (3 - lane);
	return b | (b << 4) | (b << 8) | (b << 12);
}

enum Dest
{
	kFd,
	kAcc
};

struct Form
{
	const char* tag;
	u32 (*enc)(u32 mask, u32 fd, u32 fs, u32 ft);
	Dest dest;
};

// Uniform (mask, fd, fs, ft) wrappers over the three encoder shapes.
#define FD3(name) [](u32 m, u32 fd, u32 fs, u32 ft) { return name(m, fd, fs, ft); }
#define FD2(name) [](u32 m, u32 fd, u32 fs, u32) { return name(m, fd, fs); }
#define ACC2(name) [](u32 m, u32, u32 fs, u32 ft) { return name(m, fs, ft); }

// Every family that reaches mVUemitFmacUO: the multiply's U and O, and the
// adder's O.
constexpr Form kModelled[] = {
	{"VMUL", FD3(VMUL_U), kFd},
	{"VMULx", FD3(VMULx_U), kFd},
	{"VMULw", FD3(VMULw_U), kFd},
	{"VMULq", FD2(VMULq_U), kFd},
	{"VMULi", FD2(VMULi_U), kFd},
	{"VMULA", ACC2(VMULA_U), kAcc},
	{"VMULAy", ACC2(VMULAy_U), kAcc},
	{"VOPMULA", ACC2(VOPMULA_U), kAcc},
	{"VADD", FD3(VADD_U), kFd},
	{"VADDz", FD3(VADDz_U), kFd},
	{"VADDq", FD2(VADDq_U), kFd},
	{"VADDi", FD2(VADDi_U), kFd},
	{"VADDA", ACC2(VADDA_U), kAcc},
	{"VADDAw", ACC2(VADDAw_U), kAcc},
	{"VSUB", FD3(VSUB_U), kFd},
	{"VSUBy", FD3(VSUBy_U), kFd},
	{"VSUBq", FD2(VSUBq_U), kFd},
	{"VSUBi", FD2(VSUBi_U), kFd},
	{"VSUBA", ACC2(VSUBA_U), kAcc},
	{"VSUBAx", ACC2(VSUBAx_U), kAcc},
};

// The families that model nothing: their accumulate is handed a product the
// host has already saturated at FLT_MAX, so no test of the addends reproduces
// the console.
constexpr Form kExcluded[] = {
	{"VMADD", FD3(VMADD_U), kFd},
	{"VMADDw", FD3(VMADDw_U), kFd},
	{"VMSUB", FD3(VMSUB_U), kFd},
	{"VMSUBz", FD3(VMSUBz_U), kFd},
	{"VMADDA", ACC2(VMADDA_U), kAcc},
	{"VMSUBAy", ACC2(VMSUBAy_U), kAcc},
	{"VOPMSUB", FD3(VOPMSUB_U), kFd},
};

constexpr u32 kDestFields[] = {
	mask::xyzw, mask::x, mask::y, mask::z, mask::w,
	mask::x | mask::y, mask::z | mask::w, mask::x | mask::z | mask::w,
};

// fd, fs, ft -- including every aliasing pair. vf0 is hardwired and stays out.
struct RegTriple
{
	u32 fd, fs, ft;
};
constexpr RegTriple kTriples[] = {
	{3, 1, 2}, {1, 1, 2}, {2, 1, 2}, {3, 1, 1}, {1, 1, 1},
};

// One operand word: sign x exponent x mantissa, with the three mantissas that
// bracket a binade.
u32 MakeWord(std::mt19937& rng, int exp_field)
{
	const u32 sign = (rng() & 1u) << 31;
	u32 mant;
	switch (rng() % 4u)
	{
		case 0: mant = 0; break;                  // the binade's floor
		case 1: mant = 0x7FFFFFu; break;          // one ulp below the next
		case 2: mant = 0x400000u; break;          // midpoint
		default: mant = rng() & 0x7FFFFFu; break;
	}
	return sign | (static_cast<u32>(exp_field & 0xFF) << 23) | mant;
}

// A pair of exponent fields whose sum sits within `spread` of `target`, which
// is what puts a multiply on either side of its threshold: |a * b| crosses
// 2^129 at exp(a) + exp(b) == 383, and flushes to zero below 128.
void PickExponentPair(std::mt19937& rng, int target, int spread, int& ea, int& eb)
{
	const int sum = target + (static_cast<int>(rng() % (2u * spread + 1u)) - spread);
	int lo = sum - 255;
	if (lo < 0)
		lo = 0;
	int hi = sum;
	if (hi > 255)
		hi = 255;
	ea = lo + static_cast<int>(rng() % static_cast<u32>(hi - lo + 1));
	eb = sum - ea;
	if (eb < 0) eb = 0;
	if (eb > 255) eb = 255;
}

struct Row
{
	u32 fs[4], ft[4], fd[4];
	u32 q, i;
};

// Four regimes, so no single one can carry the sweep: the multiply's overflow
// band, its underflow band, the adder's (both addends must be within a binade
// or two of 2^128 for a sum to reach 2^129), and a uniform draw.
Row MakeRow(std::mt19937& rng, int regime)
{
	Row r{};
	for (int lane = 0; lane < 4; ++lane)
	{
		int ea = 0, eb = 0;
		switch (regime)
		{
			case 0: PickExponentPair(rng, 383, 2, ea, eb); break;
			case 1: PickExponentPair(rng, 128, 2, ea, eb); break;
			case 2:
				ea = 250 + static_cast<int>(rng() % 6u);
				eb = 250 + static_cast<int>(rng() % 6u);
				break;
			default:
				ea = static_cast<int>(rng() % 256u);
				eb = static_cast<int>(rng() % 256u);
				break;
		}
		r.fs[lane] = MakeWord(rng, ea);
		r.ft[lane] = MakeWord(rng, eb);
		r.fd[lane] = MakeWord(rng, static_cast<int>(rng() % 256u));
	}
	r.q = r.ft[0];
	r.i = r.ft[1];
	return r;
}

// Runs one op through both engines with no diff -- the two legitimately
// disagree on the top binade's VALUE, which is a separate item -- and hands
// back the flags and the destination words from each.
struct Outcome
{
	u32 mac_jit, mac_interp;
	u32 status_jit, status_interp;
	u32 dst_jit[4], dst_interp[4];
};

Outcome RunOne(int vu, int clamp_mode, const Form& f, u32 dest, const RegTriple& t, const Row& r)
{
	VuTestHarness h(vu);
	h.SetVuClampMode(clamp_mode);
	h.SetVfBits(t.fs, r.fs[0], r.fs[1], r.fs[2], r.fs[3]);
	h.SetVfBits(t.ft, r.ft[0], r.ft[1], r.ft[2], r.ft[3]);
	h.SetVfBits(t.fd, r.fd[0], r.fd[1], r.fd[2], r.fd[3]);
	h.SetQ(r.q);
	h.LoadProgram({
		IBit(VuOp{VLitI(r.i), VNOP_U()}),
		UpperOnly(f.enc(dest, t.fd, t.fs, t.ft)),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		EBitNopPair(),
	});
	h.RunNoDiff();

	Outcome o{};
	o.mac_jit = h.GetViJit(REG_MAC_FLAG) & 0xFFFFu;
	o.mac_interp = h.GetViInterp(REG_MAC_FLAG) & 0xFFFFu;
	o.status_jit = h.GetViJit(REG_STATUS_FLAG);
	o.status_interp = h.GetViInterp(REG_STATUS_FLAG);
	for (int lane = 0; lane < 4; ++lane)
	{
		if (f.dest == kAcc)
		{
			o.dst_jit[lane] = h.JitSnapshot().regs.ACC.UL[lane];
			o.dst_interp[lane] = h.InterpSnapshot().regs.ACC.UL[lane];
		}
		else
		{
			o.dst_jit[lane] = h.JitSnapshot().regs.VF[t.fd].UL[lane];
			o.dst_interp[lane] = h.InterpSnapshot().regs.VF[t.fd].UL[lane];
		}
	}
	return o;
}

constexpr int kIters = 1500;
constexpr u32 kSeed = 0x5EC0'0DE1u;

} // namespace

// The whole point: at vuClampMode 4 microVU's U and O positions are the
// interpreter's on every row, and where the two engines also agree on a lane's
// result word, so is that lane's entire ZSUO group.
TEST(VuMicroFmacUO, MatchTheInterpreterAtModeFour)
{
	std::mt19937 rng(kSeed);
	int raised_o = 0, raised_u = 0, quiet = 0, comparable = 0;
	int o_lanes = 0;

	for (int it = 0; it < kIters; ++it)
	{
		const int vu = static_cast<int>(rng() % 2u);
		const Form& f = kModelled[rng() % std::size(kModelled)];
		const u32 dest = kDestFields[rng() % std::size(kDestFields)];
		const RegTriple& t = kTriples[rng() % std::size(kTriples)];
		const Row r = MakeRow(rng, static_cast<int>(rng() % 4u));

		const Outcome o = RunOne(vu, 4, f, dest, t, r);
		SCOPED_TRACE(::testing::Message()
			<< "VU" << vu << ' ' << f.tag << " dest=" << (dest >> 21) << " it=" << it);

		EXPECT_EQ(o.mac_jit & kMacO, o.mac_interp & kMacO) << "MAC O";
		EXPECT_EQ(o.mac_jit & kMacU, o.mac_interp & kMacU) << "MAC U";
		EXPECT_EQ(o.status_jit & kStatO, o.status_interp & kStatO) << "STATUS O/OS";
		EXPECT_EQ(o.status_jit & kStatU, o.status_interp & kStatU) << "STATUS U/US";

		bool all_lanes_agree = true;
		for (int lane = 0; lane < 4; ++lane)
		{
			if (o.dst_jit[lane] != o.dst_interp[lane])
			{
				all_lanes_agree = false;
				continue;
			}
			++comparable;
			const u32 bits = LaneMacBits(lane);
			EXPECT_EQ(o.mac_jit & bits, o.mac_interp & bits) << "lane " << lane << " ZSUO";
		}
		if (all_lanes_agree)
			EXPECT_EQ(o.status_jit, o.status_interp) << "STATUS";

		// A lane that raised O saturated, and the word it saturated to is a
		// binade above anything the result clamp reaches -- so this is the one
		// part of the value the two engines have to agree on even while the
		// rest of the top binade divides them.
		for (int lane = 0; lane < 4; ++lane)
		{
			if (!(o.mac_interp & LaneMacBits(lane) & kMacO))
				continue;
			++o_lanes;
			EXPECT_EQ(o.dst_jit[lane], o.dst_interp[lane]) << "lane " << lane << " ceiling";
		}
		if (o.mac_interp & kMacO) ++raised_o;
		if (o.mac_interp & kMacU) ++raised_u;
		if (!(o.mac_interp & (kMacO | kMacU))) ++quiet;
	}

	// Liveness: the sweep has to reach both sides of both thresholds, or the
	// equalities above are free.
	EXPECT_GT(raised_o, 100) << "the O window was never reached";
	EXPECT_GT(raised_u, 100) << "the U window was never reached";
	EXPECT_GT(quiet, 100) << "every row raised something";
	EXPECT_GT(comparable, 400) << "the two engines never agreed on a result word";
	EXPECT_GT(o_lanes, 200) << "no lane saturated, so the ceiling was never compared";
}

// The gate. Below vuClampMode 4 neither model is emitted, so every bit they
// carry reads zero -- and nothing else about the op moves. All three modes
// under it, not just the bottom one: 2 and 3 add clamps of their own through
// the same FMAC bodies.
TEST(VuMicroFmacUO, ModesBelowFourCarryNeitherBit)
{
	for (int mode = 1; mode <= 3; ++mode)
	{
		std::mt19937 rng(kSeed);
		int lost = 0, unchanged_nonzero = 0;

		for (int it = 0; it < kIters; ++it)
		{
			const int vu = static_cast<int>(rng() % 2u);
			const Form& f = kModelled[rng() % std::size(kModelled)];
			const u32 dest = kDestFields[rng() % std::size(kDestFields)];
			const RegTriple& t = kTriples[rng() % std::size(kTriples)];
			const Row r = MakeRow(rng, static_cast<int>(rng() % 4u));

			const Outcome four = RunOne(vu, 4, f, dest, t, r);
			const Outcome low = RunOne(vu, mode, f, dest, t, r);
			SCOPED_TRACE(::testing::Message()
				<< "VU" << vu << ' ' << f.tag << " dest=" << (dest >> 21)
				<< " it=" << it << " vuClampMode " << mode);

			EXPECT_EQ(low.mac_jit & (kMacO | kMacU), 0u) << "packed a U or O nibble";
			EXPECT_EQ(low.status_jit & (kStatO | kStatU), 0u) << "reduced a U or O to STATUS";

			if (four.mac_jit & (kMacO | kMacU))
				++lost;
			if (low.mac_jit != 0)
				++unchanged_nonzero;
		}

		EXPECT_GT(lost, 100) << "mode 4 raised nothing, so losing it proves nothing";
		EXPECT_GT(unchanged_nonzero, 300) << "no MAC was written at all";
	}
}

// MADD, MSUB, their A-forms and OPMSUB take none of this. The recompiler
// therefore raises neither bit on them, and the count of rows where the
// interpreter did is the size of what is left -- an operand-side item, not a
// flag one, since the addend arrives already saturated.
TEST(VuMicroFmacUO, AccumulateFormsCarryNeitherBit)
{
	std::mt19937 rng(kSeed ^ 0x9E3779B9u);
	int interp_raised = 0;

	for (int it = 0; it < kIters / 2; ++it)
	{
		const int vu = static_cast<int>(rng() % 2u);
		const Form& f = kExcluded[rng() % std::size(kExcluded)];
		const u32 dest = kDestFields[rng() % std::size(kDestFields)];
		const RegTriple& t = kTriples[rng() % std::size(kTriples)];
		const Row r = MakeRow(rng, static_cast<int>(rng() % 4u));

		const Outcome o = RunOne(vu, 4, f, dest, t, r);
		SCOPED_TRACE(::testing::Message()
			<< "VU" << vu << ' ' << f.tag << " dest=" << (dest >> 21) << " it=" << it);

		EXPECT_EQ(o.mac_jit & (kMacO | kMacU), 0u) << "an excluded form packed a U or O nibble";
		if (o.mac_interp & (kMacO | kMacU))
			++interp_raised;
	}

	EXPECT_GT(interp_raised, 20) << "the interpreter raised nothing here either";
}

// The I immediate reaches the models whole. doIbit used to bake the operand
// clamp into the constant it stores, which no FMAC needs -- every one of them
// re-clamps I at the use site -- and which put an exponent-255 immediate
// permanently below the O threshold. VMAXi and VMINIi compare the raw bits and
// emit no clamp of their own, so the same change is what lets them see the
// immediate the interpreter sees.
TEST(VuMicroFmacUO, TheIImmediateReachesTheModelsWhole)
{
	constexpr u32 kTwo128 = 0x7F800000u; // exponent 255: a VU number, not an Inf
	constexpr u32 kTwo127 = 0x7F000000u;

	struct ICase
	{
		const char* tag;
		u32 upper;
		u32 fs;
		u32 imm;
		u32 want_mac_o;
		u32 want_dst;
	};
	const ICase cases[] = {
		// 2^128 + 2^128 = 2^129: the threshold exactly, and only reachable
		// through an exponent-255 operand.
		{"VADDi 2^128 + 2^128", VADDi_U(mask::xyzw, 2, 1), kTwo128, kTwo128, 0xF000u, 0u},
		// 2^127 + 2^127 = 2^128, one binade short.
		{"VADDi 2^127 + 2^127", VADDi_U(mask::xyzw, 2, 1), kTwo127, kTwo127, 0x0000u, 0u},
		{"VSUBi 2^128 - -2^128", VSUBi_U(mask::xyzw, 2, 1), kTwo128, kTwo128 | 0x80000000u, 0xF000u, 0u},
		// MAX/MINI are integer compares with no clamp of their own, so the
		// immediate they see is the value.
		{"VMAXi 2^127 vs 2^128", VMAXi_U(mask::xyzw, 2, 1), kTwo127, kTwo128, 0x0000u, kTwo128},
		{"VMINIi 2^127 vs 2^128", VMINIi_U(mask::xyzw, 2, 1), kTwo127, kTwo128, 0x0000u, kTwo127},
	};

	for (const ICase& c : cases)
	{
		VuTestHarness h(0);
		h.SetVuClampMode(4);
		h.SetVfBits(1, c.fs, c.fs, c.fs, c.fs);
		h.SetVfBits(2, 0x11111111u, 0x11111111u, 0x11111111u, 0x11111111u);
		h.LoadProgram({
			IBit(VuOp{VLitI(c.imm), VNOP_U()}),
			UpperOnly(c.upper),
			BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
			EBitNopPair(),
		});
		h.RunNoDiff();
		SCOPED_TRACE(c.tag);
		EXPECT_EQ(h.GetViJit(REG_MAC_FLAG) & kMacO, c.want_mac_o) << "[jit] MAC O";
		EXPECT_EQ(h.GetViInterp(REG_MAC_FLAG) & kMacO, c.want_mac_o) << "[interp] MAC O";
		if (c.want_dst != 0)
		{
			EXPECT_EQ(h.JitSnapshot().regs.VF[2].UL[0], c.want_dst) << "[jit] result";
			EXPECT_EQ(h.InterpSnapshot().regs.VF[2].UL[0], c.want_dst) << "[interp] result";
		}
	}
}

} // namespace recompiler_tests
