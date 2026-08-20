// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The COP2 macro adder's guard mask, recompiler against interpreter.
//
// The interpreter takes every VU FMAC add through EeFpuModel::AddSub, which
// clears the low (|diff| - 1) mantissa bits of the smaller-exponent operand and
// erases it past 24 (fpuGuardMask, FPU.cpp). cop2EmitGuardedAddSub
// (iCOP2-arm64.cpp) is the four-lane branchless form of that rule; before it the
// recompiler emitted a bare Fadd and came back one ULP toward zero on every pair
// the mask could reach.
//
// A uniform random diff cannot see this. The cleared bits sit below half an ULP
// of the sum, so only an add that cancels carries them across an ULP boundary:
// the sweeps here search for the operands where the mask is observable and diff
// only those. The host replica of fpuGuardMask below is a selector -- it decides
// which pairs are worth running, and every expected value comes from the
// interpreter.
//
// Dimensions varied: form (VADD, VSUB, the four broadcast lanes of each, VADDi,
// VSUBi, VADDA, VSUBA, and the multiply-accumulates VMADD, VMSUB, VMADDbc,
// VMADDAbc, VOPMSUB), exponent difference across all four arms of the mask and
// the band it cannot reach, both operand orders, both signs, mantissa shape,
// register aliasing (fd == fs, fd == ft), and the lane the witness is read from.
//
// The multiply-accumulate sweep holds ft at 1.0 so the product is fs exactly:
// the recompiler's plain Fmul does not carry the multiplier's one-ULP deficit
// (EeFpuModel::Mul does), and a product the two engines disagree about would
// score that gap rather than the adder's mask.
//
// The mask is emitted at vuClampMode 4 only, so every sweep here sets the mode
// on the harness and each witness is run a third time at mode 3, one below the
// gate: there the recompiler must come back with the unmasked sum, which is
// what says the two settings are distinguishable and the mode-4 agreement is
// the mask and not a sweep that found nothing.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace recompiler_tests
{
using namespace mips;
using namespace mips::ee;

namespace {

// --- the selector: fpuGuardMask and eeToDouble, on the host -----------------

double eeToDouble(u32 f)
{
	const u32 exp = (f >> 23) & 0xFF;
	u64 bits = static_cast<u64>(f & 0x80000000u) << 32;
	if (exp != 0)
	{
		bits |= (static_cast<u64>(exp) + (1023 - 127)) << 52;
		bits |= static_cast<u64>(f & 0x007FFFFFu) << 29;
	}
	double d;
	std::memcpy(&d, &bits, sizeof(d));
	return d;
}

void guardMask(u32& a, u32& b)
{
	const s32 diff = (s32)((a >> 23) & 0xFF) - (s32)((b >> 23) & 0xFF);
	if (diff >= 25)
		b &= 0x80000000u;
	else if (diff >= 2)
		b &= 0xFFFFFFFFu << (diff - 1);
	else if (diff <= -25)
		a &= 0x80000000u;
	else if (diff <= -2)
		a &= 0xFFFFFFFFu << (-diff - 1);
}

// The chopped single a sum would land on, or kOut when it leaves the EE's
// range in either direction -- saturation and the flush are other files' rows.
constexpr u64 kOut = ~0ull;
u64 chopKey(double x)
{
	u64 b;
	std::memcpy(&b, &x, sizeof(b));
	const u32 dexp = (u32)((b >> 52) & 0x7FF);
	if (dexp < 897 || dexp > 1151)
		return kOut;
	return (b & 0x8000000000000000ull) | ((u64)(dexp - 896) << 23) | ((b >> 29) & 0x7FFFFFull);
}

// Does the mask move this pair's result? Both that it clears bits and that
// clearing them changes the word: on most pairs it does not.
bool maskMatters(u32 a, u32 b, bool issub)
{
	u32 ma = a, mb = b;
	guardMask(ma, mb);
	if (ma == a && mb == b)
		return false;
	const double sb = eeToDouble(issub ? (b ^ 0x80000000u) : b);
	const double msb = eeToDouble(issub ? (mb ^ 0x80000000u) : mb);
	const u64 unmasked = chopKey(eeToDouble(a) + sb);
	const u64 masked = chopKey(eeToDouble(ma) + msb);
	return unmasked != masked && unmasked != kOut && masked != kOut;
}

// Exponent kept clear of 0 and 255 so a pair is a guard-mask witness and not a
// denormal-flush or a saturation one.
u32 randomEeSingle(std::mt19937& rng)
{
	const u32 sign = (rng() & 1u) << 31;
	const u32 exp = 1u + (rng() % 254u);
	u32 man = rng() & 0x7FFFFFu;
	switch (rng() % 5u)
	{
		case 0: man = 0; break;         // exact power of two
		case 1: man = 0x7FFFFFu; break; // all ones
		case 2: man = 1u; break;        // one low bit
		case 3: man = 0x400000u; break; // top mantissa bit only
		default: break;                 // random
	}
	return sign | (exp << 23) | man;
}

// Pull the second operand into the mask's working range most of the time: a
// uniform pair is almost always 25 or more exponents apart, which is only the
// outermost arm. Returns false when the wanted exponent leaves the range.
bool pullIntoRange(std::mt19937& rng, u32 a, u32& b)
{
	if (!(rng() % 4u))
		return true;
	const s32 want = (s32)(rng() % 30u) - 15;
	const s32 eb = (s32)((a >> 23) & 0xFF) - want;
	if (eb < 1 || eb > 254)
		return false;
	b = (b & 0x807FFFFFu) | ((u32)eb << 23);
	return true;
}

int band(u32 a, u32 b)
{
	const s32 d = (s32)((a >> 23) & 0xFF) - (s32)((b >> 23) & 0xFF);
	const s32 m = d < 0 ? -d : d;
	return m <= 1 ? 0 : (m <= 24 ? 1 : 2);
}

// --- encoders the harness does not carry ------------------------------------
// SPECIAL1 funct 0x00-0x03 is VADDbc and 0x04-0x07 VSUBbc; 0x0C-0x0F is
// VMSUBbc. SPECIAL2's dispatch index is (sa << 2) | (funct & 3), so VSUBA is
// index 0x2C -> sa 0x0B, and VMSUBAx is index 0x0C -> sa 0x03.
constexpr u32 kMaskXyzw = 0xF;
u32 VADDbc_C2(u32 fd, u32 fs, u32 ft, u32 bc) { return COP2_FMAC(kMaskXyzw, fd, fs, ft, 0x00 + bc); }
u32 VSUBbc_C2(u32 fd, u32 fs, u32 ft, u32 bc) { return COP2_FMAC(kMaskXyzw, fd, fs, ft, 0x04 + bc); }
u32 VMSUBbc_C2(u32 fd, u32 fs, u32 ft, u32 bc) { return COP2_FMAC(kMaskXyzw, fd, fs, ft, 0x0C + bc); }
u32 VADDi_C2(u32 fd, u32 fs) { return COP2_FMAC(kMaskXyzw, fd, fs, 0, 0x22); }
u32 VSUBi_C2(u32 fd, u32 fs) { return COP2_FMAC(kMaskXyzw, fd, fs, 0, 0x26); }
u32 VSUBA_C2(u32 fs, u32 ft) { return COP2_FMAC(kMaskXyzw, 0x0B, fs, ft, 0x3C); }
u32 VMSUBAx_C2(u32 fs, u32 ft) { return COP2_FMAC(kMaskXyzw, 0x03, fs, ft, 0x3C); }

// --- one run ----------------------------------------------------------------

struct Out
{
	u32 val, mac, stat;
};

// Plain, fd == fs, fd == ft, and a disjoint high-numbered triple.
struct Regs
{
	u32 d, s, t;
};
const Regs kRegs[] = {{4, 1, 2}, {1, 1, 2}, {2, 1, 2}, {12, 9, 10}};

Out Read(EeRecTestHarness& h, bool acc, u32 fd, char lane, bool jit)
{
	Out o{};
	if (jit)
	{
		o.val = acc ? h.GetVu0AccBitsJit(lane) : h.GetVu0VfBitsJit(fd, lane);
		o.mac = h.GetVu0ViJit(REG_MAC_FLAG) & 0xFFFFu;
		o.stat = h.GetVu0ViJit(REG_STATUS_FLAG) & 0xFFFFu;
	}
	else
	{
		o.val = acc ? h.GetVu0AccBitsInterp(lane) : h.GetVu0VfBitsInterp(fd, lane);
		o.mac = h.GetVu0ViInterp(REG_MAC_FLAG) & 0xFFFFu;
		o.stat = h.GetVu0ViInterp(REG_STATUS_FLAG) & 0xFFFFu;
	}
	return o;
}

// Every lane gets the same operand, so the witness lane and the broadcast lane
// are free dimensions and a broadcast form sees what a plain one does.
void SeedPair(EeRecTestHarness& h, const Regs& rg, u32 fs, u32 ft, bool immediate, int clamp_mode)
{
	h.SetVu0ClampMode(clamp_mode);
	h.EnableVu0Capture();
	// Scored per engine rather than through Run()'s auto-diff: the sweeps
	// count the range and underflow classes this file does not own, and the
	// interpreter's product sticky has no recompiler side yet.
	h.ExpectVu0Divergence();
	h.EnableCop1();
	h.SeedVu0VfBits(rg.s, fs, fs, fs, fs);
	if (immediate)
		h.SeedVu0Vi(REG_I, ft);
	else
		h.SeedVu0VfBits(rg.t, ft, ft, ft, ft);
	if (rg.d != rg.s && (immediate || rg.d != rg.t))
		h.SeedVu0VfBits(rg.d, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu);
}

// One run: `clamp_mode` picks whether the recompiler emits the mask at all, so
// the same pair can be scored with it and without it.
Out RunOne(const Regs& rg, u32 word, u32 fs, u32 ft, bool immediate, u32 acc_seed,
	bool acc_out, char lane, int clamp_mode, bool jit)
{
	EeRecTestHarness h;
	SeedPair(h, rg, fs, ft, immediate, clamp_mode);
	h.SeedVu0AccBits(acc_seed, acc_seed, acc_seed, acc_seed);
	h.LoadProgram({word});
	h.Run();
	return Read(h, acc_out, rg.d, lane, jit);
}

// --- the add/sub forms ------------------------------------------------------

struct AddForm
{
	const char* name;
	bool issub, acc, immediate;
	u32 word_bc; // broadcast lane, when the form has one
	u32 (*encode)(const Regs& rg, u32 bc);
};

const AddForm kAddForms[] = {
	{"VADD", false, false, false, 0, [](const Regs& r, u32) { return VADD_C2(kMaskXyzw, r.d, r.s, r.t); }},
	{"VSUB", true, false, false, 0, [](const Regs& r, u32) { return VSUB_C2(kMaskXyzw, r.d, r.s, r.t); }},
	{"VADDx", false, false, false, 0, [](const Regs& r, u32 bc) { return VADDbc_C2(r.d, r.s, r.t, bc); }},
	{"VADDy", false, false, false, 1, [](const Regs& r, u32 bc) { return VADDbc_C2(r.d, r.s, r.t, bc); }},
	{"VADDz", false, false, false, 2, [](const Regs& r, u32 bc) { return VADDbc_C2(r.d, r.s, r.t, bc); }},
	{"VADDw", false, false, false, 3, [](const Regs& r, u32 bc) { return VADDbc_C2(r.d, r.s, r.t, bc); }},
	{"VSUBx", true, false, false, 0, [](const Regs& r, u32 bc) { return VSUBbc_C2(r.d, r.s, r.t, bc); }},
	{"VSUBz", true, false, false, 2, [](const Regs& r, u32 bc) { return VSUBbc_C2(r.d, r.s, r.t, bc); }},
	{"VADDi", false, false, true, 0, [](const Regs& r, u32) { return VADDi_C2(r.d, r.s); }},
	{"VSUBi", true, false, true, 0, [](const Regs& r, u32) { return VSUBi_C2(r.d, r.s); }},
	{"VADDA", false, true, false, 0, [](const Regs& r, u32) { return VADDA_C2(kMaskXyzw, r.s, r.t); }},
	{"VSUBA", true, true, false, 0, [](const Regs& r, u32) { return VSUBA_C2(r.s, r.t); }},
};

// --- the multiply-accumulate forms -----------------------------------------
// ft is 1.0 in every lane, so the product is fs and the accumulate is
// acc +/- fs. VMADDw carries the extra ACC and Ft clamps of mVU_MADDw;
// VOPMSUB reaches the adder with its rotated operands live in the same
// scratch registers the mask uses.

struct MadForm
{
	const char* name;
	bool issub, acc;
	u32 (*encode)(const Regs& rg);
};

const MadForm kMadForms[] = {
	{"VMADD", false, false, [](const Regs& r) { return VMADD_C2(kMaskXyzw, r.d, r.s, r.t); }},
	{"VMSUB", true, false, [](const Regs& r) { return VMSUB_C2(kMaskXyzw, r.d, r.s, r.t); }},
	{"VMADDx", false, false, [](const Regs& r) { return VMADDx_C2(kMaskXyzw, r.d, r.s, r.t); }},
	{"VMADDw", false, false, [](const Regs& r) { return VMADDw_C2(kMaskXyzw, r.d, r.s, r.t); }},
	{"VMSUBz", true, false, [](const Regs& r) { return VMSUBbc_C2(r.d, r.s, r.t, 2); }},
	{"VMADDAx", false, true, [](const Regs& r) { return VMADDAx_C2(kMaskXyzw, r.s, r.t); }},
	{"VMSUBAx", true, true, [](const Regs& r) { return VMSUBAx_C2(r.s, r.t); }},
	{"VOPMSUB", true, false, [](const Regs& r) { return VOPMSUB_C2(kMaskXyzw, r.d, r.s, r.t); }},
};

int envInt(const char* name, int dflt)
{
	const char* v = std::getenv(name);
	return v ? std::atoi(v) : dflt;
}

} // namespace

// Every add and sub form, on the pairs where the mask is observable.
TEST(EeVu0Cop2GuardMask, AddSubFormsMatchTheInterpreter)
{
	const int want = envInt("GUARD_N", 1500);
	std::mt19937 rng((u32)envInt("GUARD_SEED", 1));

	int candidates = 0, tried = 0, compared = 0;
	int value_diffs = 0, flag_diffs = 0, top_binade = 0;
	int unmasked_rows = 0, unmasked_agreed = 0, unmasked_shown = 0;
	int by_band[3] = {}, shown = 0;

	while (candidates < want && tried < want * 400)
	{
		++tried;
		const u32 a = randomEeSingle(rng);
		u32 b = randomEeSingle(rng);
		if (!pullIntoRange(rng, a, b))
			continue;
		const bool issub = (rng() & 1u) != 0;
		if (!maskMatters(a, b, issub))
			continue;

		++candidates;
		++by_band[band(a, b)];

		for (const AddForm& f : kAddForms)
		{
			if (f.issub != issub)
				continue;
			const Regs& rg = kRegs[rng() % std::size(kRegs)];
			const char lane = "xyzw"[rng() % 4];
			const u32 word = f.encode(rg, f.word_bc);

			const Out oi = RunOne(rg, word, a, b, f.immediate, 0x12345678u, f.acc, lane, 4, false);
			const Out oj = RunOne(rg, word, a, b, f.immediate, 0x12345678u, f.acc, lane, 4, true);
			const Out ou = RunOne(rg, word, a, b, f.immediate, 0x12345678u, f.acc, lane, 3, true);
			++compared;
			if (oi.val != oj.val && ((oi.val >> 23) & 0xFF) == 0xFF)
			{
				// Past the EE's largest single the host saturates a binade
				// early. That is the range gap, scored against the console by
				// Vu0MacroFmacRangeConsole, not the mask.
				++top_binade;
				continue;
			}
			// One below the gate. maskMatters picked this pair because the
			// masked and unmasked sums chop to different words, so at mode 3 the
			// recompiler has to be on the unmasked one.
			++unmasked_rows;
			if (ou.val == oi.val)
			{
				if (unmasked_shown++ < 4)
					std::printf("  mode 3 %-6s fs=%08X ft=%08X  interp %08X  jit %08X\n",
						f.name, a, b, oi.val, ou.val);
				++unmasked_agreed;
			}
			if (oi.val != oj.val || oi.mac != oj.mac || oi.stat != oj.stat)
			{
				if (oi.val != oj.val)
					++value_diffs;
				else
					++flag_diffs;
				if (shown++ < 8)
				{
					std::printf("  %-6s fs=%08X ft=%08X fd=%u lane=%c  interp %08X/%04X/%04X"
					            "  jit %08X/%04X/%04X\n",
						f.name, a, b, rg.d, lane, oi.val, oi.mac, oi.stat,
						oj.val, oj.mac, oj.stat);
				}
			}
		}
	}

	std::printf("witnesses %d of %d tried, %d form runs; |exponent difference| 2..24 %d, >=25 %d;"
	            " %d scored at vuClampMode 3\n",
		candidates, tried, compared, by_band[1], by_band[2], unmasked_rows);
	EXPECT_EQ(value_diffs, 0);
	EXPECT_EQ(flag_diffs, 0);
	// Without the mask every one of them has to move, or the mode-4 agreement
	// above is a sweep that found nothing rather than the mask.
	EXPECT_EQ(unmasked_agreed, 0);
	EXPECT_GT(unmasked_rows, want / 2);
	// A sweep that found no witnesses, or only the outermost arm, would assert
	// nothing: both arms of the mask have to be in the run.
	EXPECT_EQ(candidates, want);
	EXPECT_GT(by_band[1], want / 10);
	EXPECT_GT(by_band[2], want / 100);
	EXPECT_EQ(by_band[0], 0);
}

// The accumulate stage of the multiply-accumulate forms takes the same mask.
TEST(EeVu0Cop2GuardMask, MultiplyAccumulateFormsMatchTheInterpreter)
{
	const int want = envInt("GUARD_N", 800);
	std::mt19937 rng((u32)envInt("GUARD_SEED", 3));

	int candidates = 0, tried = 0, compared = 0;
	int value_diffs = 0, mac_diffs = 0, stat_diffs = 0, top_binade = 0;
	int unmasked_rows = 0, unmasked_agreed = 0, unmasked_shown = 0;
	int by_band[3] = {}, shown = 0;

	while (candidates < want && tried < want * 400)
	{
		++tried;
		const u32 acc = randomEeSingle(rng);
		u32 fs = randomEeSingle(rng);
		if (!pullIntoRange(rng, acc, fs))
			continue;
		const bool issub = (rng() & 1u) != 0;
		if (!maskMatters(acc, fs, issub))
			continue;

		++candidates;
		++by_band[band(acc, fs)];

		for (const MadForm& f : kMadForms)
		{
			if (f.issub != issub)
				continue;
			const Regs& rg = kRegs[rng() % std::size(kRegs)];
			// VOPMSUB writes xyz only, whatever the encoded dest field says.
			const char lane = "xyz"[rng() % 3];
			const u32 word = f.encode(rg);

			const Out oi = RunOne(rg, word, fs, 0x3F800000u, false, acc, f.acc, lane, 4, false);
			const Out oj = RunOne(rg, word, fs, 0x3F800000u, false, acc, f.acc, lane, 4, true);
			const Out ou = RunOne(rg, word, fs, 0x3F800000u, false, acc, f.acc, lane, 3, true);
			++compared;
			if (oi.val != oj.val && ((oi.val >> 23) & 0xFF) == 0xFF)
			{
				++top_binade;
				continue;
			}
			++unmasked_rows;
			if (ou.val == oi.val)
			{
				if (unmasked_shown++ < 4)
					std::printf("  mode 3 %-7s acc=%08X fs=%08X  interp %08X  jit %08X\n",
						f.name, acc, fs, oi.val, ou.val);
				++unmasked_agreed;
			}
			if (oi.val != oj.val)
			{
				++value_diffs;
				if (shown++ < 8)
				{
					std::printf("  %-7s acc=%08X fs=%08X fd=%u lane=%c  interp %08X/%04X/%04X"
					            "  jit %08X/%04X/%04X\n",
						f.name, acc, fs, rg.d, lane, oi.val, oi.mac, oi.stat,
						oj.val, oj.mac, oj.stat);
				}
			}
			mac_diffs += (oi.mac != oj.mac);
			stat_diffs += (oi.stat != oj.stat);
		}
	}

	std::printf("witnesses %d of %d tried, %d form runs; |exponent difference| 2..24 %d, >=25 %d;"
	            " status differs %d; %d scored at vuClampMode 3\n",
		candidates, tried, compared, by_band[1], by_band[2], stat_diffs, unmasked_rows);
	EXPECT_EQ(value_diffs, 0);
	EXPECT_EQ(mac_diffs, 0);
	EXPECT_EQ(unmasked_agreed, 0);
	EXPECT_GT(unmasked_rows, want / 2);
	EXPECT_EQ(candidates, want);
	EXPECT_GT(by_band[1], want / 10);
	EXPECT_GT(by_band[2], want / 100);
}

// The opposite polarity, named on the operands rather than through the
// selector: within one exponent the mask clears nothing, the exact sum needs at
// most 25 significant bits, and chopping it lands where a plain host add
// chopped to single lands. These have to agree whether the mask is emitted or
// not, so they catch a mask that fires where it should not.
//
// This is not the complement of the sweeps' predicate. That one misses pairs 25
// or more exponents apart where the mask erases an operand a double add had
// already lost -- the same missing-guard-bit fault, and already counted above.
TEST(EeVu0Cop2GuardMask, PairsTheMaskCannotReachAreUntouched)
{
	const int want = envInt("GUARD_N", 800);
	std::mt19937 rng((u32)envInt("GUARD_SEED", 7));

	int checked = 0, tried = 0, diffs = 0, flag_only = 0, top_binade = 0;
	while (checked < want && tried < want * 400)
	{
		++tried;
		const u32 a = randomEeSingle(rng);
		u32 b = randomEeSingle(rng);
		if (!pullIntoRange(rng, a, b))
			continue;
		if (band(a, b) != 0)
			continue;
		const bool issub = (rng() & 1u) != 0;

		++checked;
		for (const AddForm& f : kAddForms)
		{
			if (f.issub != issub)
				continue;
			const Regs& rg = kRegs[rng() % std::size(kRegs)];
			const char lane = "xyzw"[rng() % 4];
			const u32 word = f.encode(rg, f.word_bc);

			const Out oi = RunOne(rg, word, a, b, f.immediate, 0x12345678u, f.acc, lane, 4, false);
			const Out oj = RunOne(rg, word, a, b, f.immediate, 0x12345678u, f.acc, lane, 4, true);
			if (oi.val != oj.val && ((oi.val >> 23) & 0xFF) == 0xFF)
				++top_binade;
			else if (oi.val != oj.val)
			{
				if (diffs < 8)
					std::printf("  %-6s fs=%08X ft=%08X  interp %08X/%04X/%04X  jit %08X/%04X/%04X\n",
						f.name, a, b, oi.val, oi.mac, oi.stat, oj.val, oj.mac, oj.stat);
				++diffs;
			}
			else if (oi.mac != oj.mac || oi.stat != oj.stat)
			{
				// A sum that cancels into the subnormal range: the interpreter
				// reports the underflow the host FPCR's flush leaves unreported.
				// Counted, not asserted -- the U/O redesign owns it, and the
				// DISABLED rows in vu_sticky_console_conformance_tests.cpp
				// record what it owes.
				++flag_only;
			}
		}
	}
	std::printf("inert pairs %d of %d tried; unexplained value differences %d"
	            " (top binade %d, flags only %d)\n",
		checked, tried, diffs, top_binade, flag_only);
	EXPECT_EQ(diffs, 0);
	EXPECT_EQ(checked, want);
}

} // namespace recompiler_tests
