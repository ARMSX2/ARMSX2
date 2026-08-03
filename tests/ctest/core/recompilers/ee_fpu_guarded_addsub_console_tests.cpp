// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// The EE FPU adder's missing guard bits, against the console, on both engines.
//
// The EE adder keeps no guard bits, so a cancelling subtraction (or mixed-sign
// add) renormalises left over the lost bits and lands one ULP toward zero from
// the IEEE answer. The mechanism, the mask and the emitters it was ported from
// are at fpuAddSubGuarded in FPU.cpp. This file is the console pin for the
// class; EeRecFpuGuardBit (ee_rec_fpu_guardbit_tests.cpp) covers the emitted
// code and the fpuGuardedAddSub opt-out.
//
// Every expected value below comes from the 1147-case EE FPU capture taken on an
// SCPH-90000 (the [fpm] store, corpus v3), with the case ordinal on each row;
// none of it is derived from an engine. Over the whole corpus the port moved 13
// cases onto the console value and 0 off it, and changed 36 values in total, all
// 36 strictly closer to the console in representable steps.
//
// The capture used fd/fs/ft = $f22/$f20/$f21; the rows below pin operand values,
// not register numbers, and aliasing is covered separately.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include "common/FPControl.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFd = 4, kFs = 5, kFt = 6;

enum Form
{
	FORM_ADD,    // fd = fs + ft
	FORM_SUB,    // fd = fs - ft
	FORM_ADDA,   // ACC = fs + ft
	FORM_SUBA,   // ACC = fs - ft
	FORM_MADD,   // fd = ACC + fs * ft
	FORM_MSUB,   // fd = ACC - fs * ft
	FORM_MADDA,  // ACC = ACC + fs * ft
	FORM_MSUBA,  // ACC = ACC - fs * ft
};

bool WritesAcc(Form f)
{
	return f == FORM_ADDA || f == FORM_SUBA || f == FORM_MADDA || f == FORM_MSUBA;
}

u32 Encode(Form f)
{
	switch (f)
	{
		case FORM_ADD:   return ADD_S(kFd, kFs, kFt);
		case FORM_SUB:   return SUB_S(kFd, kFs, kFt);
		case FORM_ADDA:  return ADDA_S(kFs, kFt);
		case FORM_SUBA:  return SUBA_S(kFs, kFt);
		case FORM_MADD:  return MADD_S(kFd, kFs, kFt);
		case FORM_MSUB:  return MSUB_S(kFd, kFs, kFt);
		case FORM_MADDA: return MADDA_S(kFs, kFt);
		default:         return MSUBA_S(kFs, kFt);
	}
}

const char* FormName(Form f)
{
	switch (f)
	{
		case FORM_ADD:   return "add.s";
		case FORM_SUB:   return "sub.s";
		case FORM_ADDA:  return "adda.s";
		case FORM_SUBA:  return "suba.s";
		case FORM_MADD:  return "madd.s";
		case FORM_MSUB:  return "msub.s";
		case FORM_MADDA: return "madda.s";
		default:         return "msuba.s";
	}
}

// One instruction, one engine. `acc` seeds the accumulator for the mad forms;
// the result is read from wherever the form writes.
u32 RunOne(Form f, u32 fs, u32 ft, u32 acc, bool jit)
{
	EeRecTestHarness h;
	h.EnableCop1();
	h.SetAccBits(acc);
	h.SetFprBits(kFs, fs);
	h.SetFprBits(kFt, ft);
	h.LoadProgram({Encode(f)});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();

	if (WritesAcc(f))
		return jit ? h.GetAccBitsJit() : h.GetAccBitsInterp();
	return jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd);
}

// The thirteen console rows the port moved. Each is exactly one ULP toward zero
// without the guard.
struct ConsoleCase
{
	int ordinal;      // case id in the [fpm] corpus
	Form form;
	u32 fs, ft, acc;
	u32 want;         // console
	u32 unguarded;    // what plain IEEE arithmetic produces -- the old interp value
};

constexpr ConsoleCase kConsole[] = {
	{ 101, FORM_ADD,   0xDEADBEEFu, 0x3F800000u, 0u,          0xDEADBEEFu, 0xDEADBEEEu},
	{ 116, FORM_SUB,   0x3EAAAAABu, 0x40400000u, 0u,          0xC02AAAABu, 0xC02AAAAAu},
	{ 124, FORM_SUB,   0x00800000u, 0x3F000000u, 0u,          0xBF000000u, 0xBEFFFFFFu},
	{ 125, FORM_SUB,   0x00800001u, 0x3F000000u, 0u,          0xBF000000u, 0xBEFFFFFFu},
	{ 134, FORM_SUB,   0x7F000000u, 0x40800000u, 0u,          0x7F000000u, 0x7EFFFFFFu},
	{ 141, FORM_SUB,   0x4F000000u, 0x3F800000u, 0u,          0x4F000000u, 0x4EFFFFFFu},
	{ 490, FORM_ADDA,  0xDEADBEEFu, 0x3F800000u, 0u,          0xDEADBEEFu, 0xDEADBEEEu},
	{ 496, FORM_SUBA,  0x00800000u, 0x3F000000u, 0u,          0xBF000000u, 0xBEFFFFFFu},
	{ 501, FORM_SUBA,  0x3EAAAAABu, 0x40400000u, 0u,          0xC02AAAABu, 0xC02AAAAAu},
	{ 539, FORM_MSUB,  0x3EAAAAABu, 0x40400000u, 0x3EAAAAABu, 0xBF2AAAABu, 0xBF2AAAAAu},
	{ 567, FORM_MSUBA, 0x3EAAAAABu, 0x40400000u, 0x3EAAAAABu, 0xBF2AAAABu, 0xBF2AAAAAu},
	{1078, FORM_MSUB,  0x40400000u, 0x3EAAAAABu, 0x3EAAAAABu, 0xBF2AAAABu, 0xBF2AAAAAu},
	{1130, FORM_MSUBA, 0x40400000u, 0x3EAAAAABu, 0x3EAAAAABu, 0xBF2AAAABu, 0xBF2AAAAAu},
};
constexpr int kConsoleCount = static_cast<int>(sizeof(kConsole) / sizeof(kConsole[0]));

// The FP environment the two engines run in, applied to this thread so a model
// value computed here is computed the same way.
//
// The harness pins its own thread to EmuConfig.Cpu.FPUFPCR (production ChopZero
// + FZ) for each run, but gtest calls a test body at the host default --
// round-to-nearest, FZ clear -- so an expectation computed outside this guard
// rounds differently from the thing under test. Without it the randomised test
// below failed on its first inexact pair (a=c8cc3adf b=c847db91) by one ULP at
// diff=1, where no masking happens at all.
struct ScopedEeFpEnv
{
	FPControlRegister saved;
	ScopedEeFpEnv()
		: saved(FPControlRegister::GetCurrent())
	{
		FPControlRegister::SetCurrent(EmuConfig.Cpu.FPUFPCR);
	}
	~ScopedEeFpEnv() { FPControlRegister::SetCurrent(saved); }
	ScopedEeFpEnv(const ScopedEeFpEnv&) = delete;
	ScopedEeFpEnv& operator=(const ScopedEeFpEnv&) = delete;
};

// The reference model, transcribed from x86 FPU_ADD_SUB (iFPU.cpp); the rule it
// implements is stated at fpuAddSubGuarded in FPU.cpp.
//
// __noinline: several callers below pass compile-time constants, and an inlined
// body would let the compiler fold the arithmetic at build time, under the
// compiler's rounding mode rather than the EE's that ScopedEeFpEnv sets.
__noinline u32 ModelGuardedAddSub(u32 a, u32 b, bool issub)
{
	const int d = static_cast<int>((a >> 23) & 0xff) - static_cast<int>((b >> 23) & 0xff);
	if (d >= 25)
		b &= 0x80000000u;
	else if (d >= 2)
		b &= (0xffffffffu << (d - 1));
	else if (d <= -25)
		a &= 0x80000000u;
	else if (d <= -2)
		a &= (0xffffffffu << (-d - 1));

	float fa, fb;
	std::memcpy(&fa, &a, 4);
	std::memcpy(&fb, &b, 4);
	const float r = issub ? fa - fb : fa + fb;
	u32 rb;
	std::memcpy(&rb, &r, 4);
	return rb;
}

__noinline u32 ModelPlainAddSub(u32 a, u32 b, bool issub)
{
	float fa, fb;
	std::memcpy(&fa, &a, 4);
	std::memcpy(&fb, &b, 4);
	const float r = issub ? fa - fb : fa + fb;
	u32 rb;
	std::memcpy(&rb, &r, 4);
	return rb;
}

} // namespace

// ---------------------------------------------------------------------------
// The interpreter failed all thirteen of these before fpuAddSubGuarded().
// ---------------------------------------------------------------------------
TEST(EeFpuGuardedAddSubConsole, InterpMatchesConsoleOnEveryGuardBitRow)
{
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(testing::Message() << "[fpm] case " << c.ordinal << " "
			<< FormName(c.form));
		EXPECT_EQ(RunOne(c.form, c.fs, c.ft, c.acc, false), c.want);
	}
	EXPECT_EQ(kConsoleCount, 13) << "anti-vacuity: the console row table emptied";
}

// ---------------------------------------------------------------------------
// The recompiler already matched the console on all thirteen. Pinned so the
// port cannot be "simplified" later by moving the JIT to the interpreter.
// ---------------------------------------------------------------------------
TEST(EeFpuGuardedAddSubConsole, JitMatchesConsoleOnEveryGuardBitRow)
{
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(testing::Message() << "[fpm] case " << c.ordinal << " "
			<< FormName(c.form));
		EXPECT_EQ(RunOne(c.form, c.fs, c.ft, c.acc, true), c.want);
	}
}

// ---------------------------------------------------------------------------
// On every row the console value differs from what plain IEEE arithmetic gives,
// by one representable step toward zero. Without this the two tests above would
// still pass if the masking were deleted and the unguarded values copied into
// `want`.
// ---------------------------------------------------------------------------
TEST(EeFpuGuardedAddSubConsole, EveryRowIsOneStepFromThePlainIeeeAnswer)
{
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(testing::Message() << "[fpm] case " << c.ordinal);
		ASSERT_NE(c.want, c.unguarded)
			<< "a row whose guarded and unguarded values agree pins nothing";
		// Same sign, adjacent magnitudes, and the unguarded one is the smaller:
		// "one ULP toward zero" as a checkable statement about the encoding.
		EXPECT_EQ(c.want & 0x80000000u, c.unguarded & 0x80000000u);
		EXPECT_EQ((c.want & 0x7FFFFFFFu) - (c.unguarded & 0x7FFFFFFFu), 1u)
			<< "the unguarded value must be exactly one step toward zero";
	}
}

// ---------------------------------------------------------------------------
// The class rather than the thirteen rows: randomised operands over the
// dimensions that matter for guard bits, with both engines asserted against the
// x86 model and therefore against each other.
//
// Dimensions varied:
//   - exponent difference across the whole meaningful range, including the
//     |diff| <= 1 no-mask boundary and the |diff| >= 25 sign-only cliff;
//   - both signs on both operands, so cancelling adds and non-cancelling subs
//     are both reached;
//   - dirty low mantissa bits, without which the mask has nothing to clear;
//   - both add and sub.
//
// Exponents 1..254 only: exponent 0 and exponent 255 are the operand model's
// business, folded away by fpuOperandBits and the JIT's fpuClampInput before the
// adder sees them. Console rows 124/125 sit at the exponent-1 boundary, and
// EeFpuOverflowConsole covers the top binade.
// ---------------------------------------------------------------------------
TEST(EeFpuGuardedAddSubConsole, RandomisedAddSubMatchesTheModelOnBothEngines)
{
	const ScopedEeFpEnv fp_env;
	std::mt19937 rng(0x6A5D5EED);
	std::uniform_int_distribution<u32> mant_dist(0, 0x7FFFFF);
	std::uniform_int_distribution<u32> sign_dist(0, 1);
	std::uniform_int_distribution<int> exp_dist(1, 254);
	// Deliberately oversample small differences: |diff| 0..3 is where the mask
	// switches on, and a uniform exponent pair almost never lands there.
	std::uniform_int_distribution<int> near_dist(-4, 4);
	std::uniform_int_distribution<u32> pick(0, 1);

	int checked = 0, masked_rows = 0, cliff_rows = 0, boundary_rows = 0;
	int underflow_rows = 0;
	for (int i = 0; i < 3000; ++i)
	{
		const int ea = exp_dist(rng);
		int eb = pick(rng) ? ea + near_dist(rng) : exp_dist(rng);
		if (eb < 1 || eb > 254)
			continue;

		const u32 a = (sign_dist(rng) << 31) | (static_cast<u32>(ea) << 23) | mant_dist(rng);
		const u32 b = (sign_dist(rng) << 31) | (static_cast<u32>(eb) << 23) | mant_dist(rng);

		const u32 want_add = ModelGuardedAddSub(a, b, false);
		const u32 want_sub = ModelGuardedAddSub(a, b, true);
		// Skip pairs whose result leaves the EE's range: the result clamp then
		// dominates and the guard mask is unobservable.
		if ((want_add & 0x7F800000u) >= 0x7F800000u || (want_sub & 0x7F800000u) >= 0x7F800000u)
			continue;
		// Same at the bottom: below 2^-126 the console keeps an add/sub result's
		// mantissa bits instead of flushing (EeFpuUnderflowConsole), and the
		// model above computes in host floats, which FZ has already flushed.
		// Counted, see the bound below.
		if ((want_add & 0x7F800000u) == 0 || (want_sub & 0x7F800000u) == 0)
		{
			++underflow_rows;
			continue;
		}

		SCOPED_TRACE(testing::Message() << std::hex << "a=" << a << " b=" << b
			<< std::dec << " diff=" << (ea - eb));
		ASSERT_EQ(RunOne(FORM_ADD, a, b, 0, false), want_add) << "interp add.s";
		ASSERT_EQ(RunOne(FORM_ADD, a, b, 0, true),  want_add) << "jit add.s";
		ASSERT_EQ(RunOne(FORM_SUB, a, b, 0, false), want_sub) << "interp sub.s";
		ASSERT_EQ(RunOne(FORM_SUB, a, b, 0, true),  want_sub) << "jit sub.s";

		const int d = ea - eb;
		if (d >= 2 || d <= -2)
			++masked_rows;
		if (d >= 25 || d <= -25)
			++cliff_rows;
		if (d >= -1 && d <= 1)
			++boundary_rows;
		// The mask is only observable when it changes something. Count the rows
		// where it did, so the anti-vacuity floor below means what it says.
		if (want_add != ModelPlainAddSub(a, b, false) || want_sub != ModelPlainAddSub(a, b, true))
			++checked;
	}

	EXPECT_GT(masked_rows, 500) << "anti-vacuity: too few |diff| >= 2 pairs, the "
								   "mask is barely being exercised";
	EXPECT_GT(cliff_rows, 100) << "anti-vacuity: the |diff| >= 25 sign-only cliff "
								  "must be reached";
	EXPECT_GT(boundary_rows, 100) << "anti-vacuity: the |diff| <= 1 no-mask "
									 "boundary must be reached";
	EXPECT_GT(checked, 100) << "anti-vacuity: on no pair did the guard change the "
							   "answer, so this test would pass with the masking "
							   "deleted";
	// If the skip swallows more than a handful of the 3000 pairs, the
	// generator's exponent distribution moved.
	EXPECT_LT(underflow_rows, 100) << "the underflow skip swallowed " << underflow_rows
								   << " pairs; it is meant to be a handful";
}

// ---------------------------------------------------------------------------
// All eight members of the family have to be wired to the guard: the
// accumulator forms route their ACC +/- product through the same adder. The
// console rows above cover add/sub/adda/suba/msub/msuba; this covers the two
// they miss (madd/madda) and asserts the family as a whole against the model, on
// both engines.
//
// ACC and the product are chosen so the accumulate is the guard-sensitive step:
// ACC = 4.0 against a magnitude of 1.0 + 3ulp, so |diff| = 2 and the smaller
// operand loses one bit.
//
// The add legs take a negative right operand: guard bits are only observable
// when the operation cancels and renormalises left, and 4.0 + (1.0+3ulp) rounds
// to the same word masked or not, which would leave four of the eight legs
// vacuous.
// ---------------------------------------------------------------------------
TEST(EeFpuGuardedAddSubConsole, EveryFormInTheFamilyGuardsItsAccumulate)
{
	const ScopedEeFpEnv fp_env;
	constexpr u32 kAcc = 0x40800000u; // 4.0
	constexpr u32 kPos = 0x3F800003u; // +(1.0 + 3ulp)
	constexpr u32 kNeg = 0xBF800003u; // -(1.0 + 3ulp)
	constexpr u32 kOne = 0x3F800000u; // 1.0 -- multiplying by it is exact, so
	                                  // the mad forms' product is exactly fs

	struct Leg { Form form; u32 fs, ft, acc; bool issub; u32 lhs, rhs; };
	const Leg legs[] = {
		// The plain and A forms take fs/ft straight into the adder.
		{FORM_ADD,   kAcc, kNeg, 0u,   false, kAcc, kNeg},
		{FORM_SUB,   kAcc, kPos, 0u,   true,  kAcc, kPos},
		{FORM_ADDA,  kAcc, kNeg, 0u,   false, kAcc, kNeg},
		{FORM_SUBA,  kAcc, kPos, 0u,   true,  kAcc, kPos},
		// The mad forms' adder sees ACC and the product (== fs by construction).
		{FORM_MADD,  kNeg, kOne, kAcc, false, kAcc, kNeg},
		{FORM_MSUB,  kPos, kOne, kAcc, true,  kAcc, kPos},
		{FORM_MADDA, kNeg, kOne, kAcc, false, kAcc, kNeg},
		{FORM_MSUBA, kPos, kOne, kAcc, true,  kAcc, kPos},
	};

	int divergent = 0;
	for (const Leg& l : legs)
	{
		SCOPED_TRACE(FormName(l.form));
		const u32 want = ModelGuardedAddSub(l.lhs, l.rhs, l.issub);
		EXPECT_EQ(RunOne(l.form, l.fs, l.ft, l.acc, false), want) << "interp";
		EXPECT_EQ(RunOne(l.form, l.fs, l.ft, l.acc, true), want) << "jit";
		if (want != ModelPlainAddSub(l.lhs, l.rhs, l.issub))
			++divergent;
	}
	EXPECT_EQ(divergent, 8) << "anti-vacuity: every leg must be a case where the "
							   "guard actually changes the answer, or a form that "
							   "forgot to guard would still pass";
}

// ---------------------------------------------------------------------------
// Register aliasing. fd == fs, fd == ft and fs == ft all collapse operands onto
// each other; the guard reads both operands before writing, so none of them may
// change the answer. The corpus carries aliased variants (cases 699-701) and
// they are in the moved set.
// ---------------------------------------------------------------------------
TEST(EeFpuGuardedAddSubConsole, AliasedOperandsGuardIdentically)
{
	const ScopedEeFpEnv fp_env;
	constexpr u32 kBig = 0x40800000u; // 4.0
	constexpr u32 kSmall = 0x3F800003u; // 1.0 + 3ulp

	// fd == fs: the destination is also the left operand.
	for (int jit = 0; jit < 2; ++jit)
	{
		SCOPED_TRACE(jit ? "jit" : "interp");
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(kFs, kBig);
		h.SetFprBits(kFt, kSmall);
		h.LoadProgram({SUB_S(kFs, kFs, kFt)});
		if (jit) h.RunJitNoDiff(); else h.RunInterpOnly();
		EXPECT_EQ(jit ? h.GetFprBitsJit(kFs) : h.GetFprBitsInterp(kFs),
			ModelGuardedAddSub(kBig, kSmall, true)) << "fd == fs";
	}

	// fd == ft: the destination is also the right operand.
	for (int jit = 0; jit < 2; ++jit)
	{
		SCOPED_TRACE(jit ? "jit" : "interp");
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFprBits(kFs, kBig);
		h.SetFprBits(kFt, kSmall);
		h.LoadProgram({SUB_S(kFt, kFs, kFt)});
		if (jit) h.RunJitNoDiff(); else h.RunInterpOnly();
		EXPECT_EQ(jit ? h.GetFprBitsJit(kFt) : h.GetFprBitsInterp(kFt),
			ModelGuardedAddSub(kBig, kSmall, true)) << "fd == ft";
	}

	// fs == ft: |diff| is 0, so nothing is masked and x - x is exactly zero.
	for (int jit = 0; jit < 2; ++jit)
	{
		SCOPED_TRACE(jit ? "jit" : "interp");
		EXPECT_EQ(RunOne(FORM_SUB, kSmall, kSmall, 0, jit != 0),
			ModelGuardedAddSub(kSmall, kSmall, true)) << "fs == ft";
	}
}

// ---------------------------------------------------------------------------
// The FCR31 axis must not move. The guard changes the value, but the O/U causes
// and stickies are decided from the exact result (raiseOrClearOU, FPU.cpp), and
// the port changed the flag word on no corpus case. So a guard-sensitive op with
// the flags pre-seeded must clear the causes and keep the stickies exactly as an
// unguarded one did.
// ---------------------------------------------------------------------------
TEST(EeFpuGuardedAddSubConsole, GuardingDoesNotDisturbFcr31)
{
	constexpr u32 kO = 0x00008000u, kU = 0x00004000u;
	constexpr u32 kSticky = 0x00000010u | 0x00000008u; // SO|SU
	constexpr u32 kSeed = 0x01000001u | kO | kU | kSticky;

	for (int jit = 0; jit < 2; ++jit)
	{
		SCOPED_TRACE(jit ? "jit" : "interp");
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFcr31(kSeed);
		h.SetFprBits(kFs, 0x40800000u); // 4.0
		h.SetFprBits(kFt, 0x3F800003u); // 1.0 + 3ulp -- guard-sensitive
		h.LoadProgram({SUB_S(kFd, kFs, kFt)});
		if (jit) h.RunJitNoDiff(); else h.RunInterpOnly();

		const u32 got = (jit ? h.JitSnapshot() : h.InterpSnapshot()).fprs.fprc[31];
		EXPECT_EQ(got & (kO | kU), 0u)
			<< "an in-range sub.s must clear the O and U causes";
		EXPECT_EQ(got & kSticky, kSticky) << "the sticky flags must survive";
		// And the value is the guarded one, so this cannot pass on a path where
		// the guard did nothing.
		EXPECT_EQ(jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd), 0x403FFFFFu);
	}
}
