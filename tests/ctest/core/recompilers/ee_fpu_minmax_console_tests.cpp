// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// MAX.S / MIN.S against the console, across the EE clamp modes.
//
// The console does not compute a maximum, it SELECTS one: the two raw words are
// ordered by (sign, magnitude) and the winner's 32 bits are written through
// untouched. Denormal encodings survive, and an exponent-255 word is just a
// very large number — the EE has no Inf and no NaN. That is `fp_max`/`fp_min`
// in FPU.cpp (a pure integer compare), which is what the interpreter has always
// done and what the DOUBLE tier has always done (DOUBLE::recMINMAX).
//
// The arm64 fast path did not: it clamped both operands to ±fMax and then used
// Fmaxnm/Fminnm. Two operand classes came back wrong, and both are visible in
// the SCPH-90000 capture (1147-case EE FPU corpus, sections C-minmax/G3-minmax
// /D-alias) — 38 of 66 MAX cases and 16 of 66 MIN cases at the shipping default:
//
//   denormals    Fmaxnm/Fminnm are arithmetic ops, so FPCR.FZ flushed the
//                operand first: max(0x00000001, 0x00000000) read back
//                0x00000000 where the console says 0x00000001.
//   exponent 255 the ±fMax clamp folded the whole top binade onto 0x7F7FFFFF:
//                max(0x7F7FFFFF, 0x7FFFFFFF) read back 0x7F7FFFFF where the
//                console says 0x7FFFFFFF.
//
// The same two classes were what ABS.S/NEG.S carried until cbf04acba1, for the
// same two reasons — see ee_fpu_absneg_clamp_tests.cpp.
//
// Every expected value below is a console reading. Nothing here is derived from
// either engine.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "Config.h"

#include <gtest/gtest.h>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFd = 22, kFs = 20, kFt = 21;

// The console's ordering, transcribed from FPU.cpp's fp_max/fp_min. It is the
// model, not an engine: the 54 rows below were checked against it and it
// reproduces all 54, so a row that disagrees with the model is a capture
// transcription error rather than a code defect.
u32 ModelMinMax(u32 a, u32 b, bool ismin)
{
	const s32 sa = static_cast<s32>(a), sb = static_cast<s32>(b);
	if (ismin)
		return static_cast<u32>((sa < 0 && sb < 0) ? std::max(sa, sb) : std::min(sa, sb));
	return static_cast<u32>((sa < 0 && sb < 0) ? std::min(sa, sb) : std::max(sa, sb));
}

struct ConsoleCase
{
	int ismin;
	u32 fs;
	u32 ft;
	u32 want;
	const char* what;
};

// [fpm] the 1147-case EE FPU capture (SCPH-90000), corpus v3, every distinct
// (op, fs, ft) triple its MAX/MIN population contains — 54 of them, from 132
// cases (the rest are register-aliasing repeats, covered separately below).
constexpr ConsoleCase kConsole[] = {
	{0, 0x00000000u, 0x80000000u, 0x00000000u, "max P0, N0"},
	{0, 0x80000000u, 0x00000000u, 0x00000000u, "max N0, P0"},
	{0, 0x00000000u, 0x00000000u, 0x00000000u, "max P0, P0"},
	{0, 0x80000000u, 0x80000000u, 0x80000000u, "max N0, N0"},
	{0, 0x3F800000u, 0x40000000u, 0x40000000u, "max ONE, TWO"},
	{0, 0x40000000u, 0x3F800000u, 0x40000000u, "max TWO, ONE"},
	{0, 0xBF800000u, 0x3F800000u, 0x3F800000u, "max NONE, ONE"},
	{0, 0x3F800000u, 0xBF800000u, 0x3F800000u, "max ONE, NONE"},
	{0, 0x00000001u, 0x00000000u, 0x00000001u, "max MIN_DEN, P0"},
	{0, 0x00000000u, 0x00000001u, 0x00000001u, "max P0, MIN_DEN"},
	{0, 0x80000001u, 0x00000001u, 0x00000001u, "max NMIN_DEN, MIN_DEN"},
	{0, 0x00000001u, 0x00800000u, 0x00800000u, "max MIN_DEN, MIN_NORM"},
	{0, 0x007FFFFFu, 0x00800000u, 0x00800000u, "max MAX_DEN, MIN_NORM"},
	{0, 0x7F7FFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, "max FMAX, EEMAX"},
	{0, 0x7FFFFFFFu, 0x7F800000u, 0x7FFFFFFFu, "max EEMAX, E128"},
	{0, 0xFFFFFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu, "max NEEMAX, NFMAX"},
	{0, 0x7FC00000u, 0x3F800000u, 0x7FC00000u, "max QNAN, ONE"},
	{0, 0x3F800000u, 0x7FC00000u, 0x7FC00000u, "max ONE, QNAN"},
	{0, 0xDEADBEEFu, 0x3F800000u, 0x3F800000u, "max GARB2, ONE"},
	{0, 0x3E800000u, 0x40490FDBu, 0x40490FDBu, "max QTR, PIO2"},
	{0, 0x80000000u, 0x00000001u, 0x00000001u, "max N0, MIN_DEN"},
	{0, 0x00000001u, 0x80000000u, 0x00000001u, "max MIN_DEN, N0"},
	{0, 0x00000001u, 0x80000001u, 0x00000001u, "max MIN_DEN, NMIN_DEN"},
	{0, 0x007FFFFFu, 0x807FFFFFu, 0x007FFFFFu, "max MAX_DEN, NMAX_DEN"},
	{0, 0x807FFFFFu, 0x007FFFFFu, 0x007FFFFFu, "max NMAX_DEN, MAX_DEN"},
	{0, 0x00400000u, 0x00000000u, 0x00400000u, "max MID_DEN, P0"},
	{0, 0x00000000u, 0x00400000u, 0x00400000u, "max P0, MID_DEN"},
	{1, 0x00000000u, 0x80000000u, 0x80000000u, "min P0, N0"},
	{1, 0x80000000u, 0x00000000u, 0x80000000u, "min N0, P0"},
	{1, 0x00000000u, 0x00000000u, 0x00000000u, "min P0, P0"},
	{1, 0x80000000u, 0x80000000u, 0x80000000u, "min N0, N0"},
	{1, 0x3F800000u, 0x40000000u, 0x3F800000u, "min ONE, TWO"},
	{1, 0x40000000u, 0x3F800000u, 0x3F800000u, "min TWO, ONE"},
	{1, 0xBF800000u, 0x3F800000u, 0xBF800000u, "min NONE, ONE"},
	{1, 0x3F800000u, 0xBF800000u, 0xBF800000u, "min ONE, NONE"},
	{1, 0x00000001u, 0x00000000u, 0x00000000u, "min MIN_DEN, P0"},
	{1, 0x00000000u, 0x00000001u, 0x00000000u, "min P0, MIN_DEN"},
	{1, 0x80000001u, 0x00000001u, 0x80000001u, "min NMIN_DEN, MIN_DEN"},
	{1, 0x00000001u, 0x00800000u, 0x00000001u, "min MIN_DEN, MIN_NORM"},
	{1, 0x007FFFFFu, 0x00800000u, 0x007FFFFFu, "min MAX_DEN, MIN_NORM"},
	{1, 0x7F7FFFFFu, 0x7FFFFFFFu, 0x7F7FFFFFu, "min FMAX, EEMAX"},
	{1, 0x7FFFFFFFu, 0x7F800000u, 0x7F800000u, "min EEMAX, E128"},
	{1, 0xFFFFFFFFu, 0xFF7FFFFFu, 0xFFFFFFFFu, "min NEEMAX, NFMAX"},
	{1, 0x7FC00000u, 0x3F800000u, 0x3F800000u, "min QNAN, ONE"},
	{1, 0x3F800000u, 0x7FC00000u, 0x3F800000u, "min ONE, QNAN"},
	{1, 0xDEADBEEFu, 0x3F800000u, 0xDEADBEEFu, "min GARB2, ONE"},
	{1, 0x3E800000u, 0x40490FDBu, 0x3E800000u, "min QTR, PIO2"},
	{1, 0x80000000u, 0x00000001u, 0x80000000u, "min N0, MIN_DEN"},
	{1, 0x00000001u, 0x80000000u, 0x80000000u, "min MIN_DEN, N0"},
	{1, 0x00000001u, 0x80000001u, 0x80000001u, "min MIN_DEN, NMIN_DEN"},
	{1, 0x007FFFFFu, 0x807FFFFFu, 0x807FFFFFu, "min MAX_DEN, NMAX_DEN"},
	{1, 0x807FFFFFu, 0x007FFFFFu, 0x807FFFFFu, "min NMAX_DEN, MAX_DEN"},
	{1, 0x00400000u, 0x00000000u, 0x00000000u, "min MID_DEN, P0"},
	{1, 0x00000000u, 0x00400000u, 0x00000000u, "min P0, MID_DEN"},
};
constexpr int kConsoleCount = static_cast<int>(sizeof(kConsole) / sizeof(kConsole[0]));

enum Mode
{
	MODE_CLAMP0,   // eeClampMode 0 — fpuOverflow off
	MODE_DEFAULT,  // eeClampMode 1 — fpuOverflow on (shipping default)
	MODE_EXTRA,    // eeClampMode 2 — + fpuExtraOverflow
	MODE_FULL,     // eeClampMode 3 — the DOUBLE tier (iFPUd-arm64.cpp)
	MODE_COUNT
};

const char* ModeName(Mode m)
{
	switch (m)
	{
		case MODE_CLAMP0:  return "clamp0 ";
		case MODE_DEFAULT: return "default";
		case MODE_EXTRA:   return "extra  ";
		case MODE_FULL:    return "full   ";
		default:           return "?";
	}
}

// The interpreter has no clamp-mode switches at all, so it is only meaningful
// to run it once; jit==false ignores `mode` beyond the harness bringup.
u32 RunOne(u32 insn, u32 fd, u32 fs_bits, u32 ft_bits, bool jit, Mode mode)
{
	EeRecTestHarness h;
	h.EnableCop1();
	if (jit)
	{
		if (mode == MODE_CLAMP0)
			h.DisableFpuOverflow();
		if (mode == MODE_EXTRA)
			h.EnableFpuExtraOverflow();
		if (mode == MODE_FULL)
			h.EnableFpuFullMode();
	}
	h.SetFprBits(kFs, fs_bits);
	h.SetFprBits(kFt, ft_bits);
	h.LoadProgram({insn});
	if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();
	// RunJitNoDiff never runs the interpreter, and GetFprBitsInterp would then
	// hand back the JIT's own register file — read the side that ran.
	return jit ? h.GetFprBitsJit(fd) : h.GetFprBitsInterp(fd);
}

u32 Encode(bool ismin, u32 fd, u32 fs, u32 ft)
{
	return ismin ? MIN_S(fd, fs, ft) : MAX_S(fd, fs, ft);
}

} // namespace

// ---------------------------------------------------------------------------
// The 54 console triples, on the interpreter. This is the side that has always
// matched silicon and it is a must-not-regress control.
// ---------------------------------------------------------------------------
TEST(EeFpuMinMaxConsole, InterpMatchesConsole)
{
	int checked = 0;
	for (int i = 0; i < kConsoleCount; ++i)
	{
		const ConsoleCase& c = kConsole[i];
		SCOPED_TRACE(c.what);
		EXPECT_EQ(RunOne(Encode(c.ismin != 0, kFd, kFs, kFt), kFd, c.fs, c.ft, false, MODE_DEFAULT),
			c.want);
		++checked;
	}
	EXPECT_EQ(checked, kConsoleCount) << "anti-vacuity";
}

// ---------------------------------------------------------------------------
// The same 54 triples on the JIT, in all four clamp modes. eeClampMode has
// nothing to say about a bit-selection op, so all four owe the same answer.
//
// Before the fix this failed on 20 of the 54 rows in modes 0/1/2 (every
// denormal and every exponent-255 row) and passed in mode 3, which already ran
// the integer path.
// ---------------------------------------------------------------------------
TEST(EeFpuMinMaxConsole, JitMatchesConsoleInEveryClampMode)
{
	int checked = 0;
	for (int m = 0; m < MODE_COUNT; ++m)
	{
		const Mode mode = static_cast<Mode>(m);
		for (int i = 0; i < kConsoleCount; ++i)
		{
			const ConsoleCase& c = kConsole[i];
			SCOPED_TRACE(testing::Message() << c.what << " [" << ModeName(mode) << "]");
			EXPECT_EQ(RunOne(Encode(c.ismin != 0, kFd, kFs, kFt), kFd, c.fs, c.ft, true, mode),
				c.want);
			++checked;
		}
	}
	EXPECT_EQ(checked, kConsoleCount * MODE_COUNT) << "anti-vacuity";
}

// ---------------------------------------------------------------------------
// The console rows agree with fp_max/fp_min on all 54, so the property can be
// asserted directly rather than only through the transcribed table: the result
// must be one of the two input words, bit for bit, chosen by (sign, magnitude).
//
// Kept separate from the table because it is a property, not a transcription —
// and because the operand pool below deliberately contains pairs the capture
// does not, which is where a future emitter regression would land first.
// ---------------------------------------------------------------------------
TEST(EeFpuMinMaxConsole, ResultIsAlwaysOneOfTheTwoInputWords)
{
	static constexpr u32 kPool[] = {
		0x00000000u, 0x80000000u,              // ±0
		0x00000001u, 0x80000001u,              // ±MIN_DENORM
		0x007FFFFFu, 0x807FFFFFu,              // ±MAX_DENORM
		0x00400000u, 0x00001337u,              // mid denormals
		0x00800000u, 0x80800000u,              // ±MIN_NORMAL
		0x3F800000u, 0xBF800000u,              // ±1.0
		0x7F7FFFFFu, 0xFF7FFFFFu,              // ±FLT_MAX
		0x7F800000u, 0xFF800000u,              // ±2^128 (host inf)
		0x7FC00000u, 0xFFC00000u,              // exp255 mant 0x400000 (host qNaN)
		0x7FA00000u,                           // exp255 mant 0x200000 (host sNaN)
		0x7FFFFFFFu, 0xFFFFFFFFu,              // ±EEMAX
		0xDEADBEEFu,                           // garbage
	};
	constexpr int kPoolCount = static_cast<int>(sizeof(kPool) / sizeof(kPool[0]));

	int denormPairs = 0, exp255Pairs = 0, checked = 0;
	for (int m = 0; m < MODE_COUNT; ++m)
	{
		const Mode mode = static_cast<Mode>(m);
		for (int i = 0; i < kPoolCount; ++i)
		{
			for (int j = 0; j < kPoolCount; ++j)
			{
				const u32 a = kPool[i], b = kPool[j];
				SCOPED_TRACE(testing::Message()
					<< "fs=" << std::hex << a << " ft=" << b << " [" << ModeName(mode) << "]");
				for (int ismin = 0; ismin < 2; ++ismin)
				{
					const u32 want = ModelMinMax(a, b, ismin != 0);
					const u32 got = RunOne(Encode(ismin != 0, kFd, kFs, kFt), kFd, a, b, true, mode);
					EXPECT_EQ(got, want);
					EXPECT_TRUE(got == a || got == b) << "must select a whole input word";
					++checked;
				}
				if (m != 0)
					continue;
				const bool aden = (a & 0x7F800000u) == 0 && (a & 0x7FFFFFu) != 0;
				const bool bden = (b & 0x7F800000u) == 0 && (b & 0x7FFFFFu) != 0;
				if (aden || bden)
					++denormPairs;
				if ((a & 0x7F800000u) == 0x7F800000u || (b & 0x7F800000u) == 0x7F800000u)
					++exp255Pairs;
			}
		}
	}
	EXPECT_EQ(checked, kPoolCount * kPoolCount * 2 * MODE_COUNT) << "anti-vacuity";
	EXPECT_GT(denormPairs, 100) << "anti-vacuity: the pool must keep denormal pairs — "
								   "the class FPCR.FZ flushed under Fmaxnm/Fminnm";
	EXPECT_GT(exp255Pairs, 100) << "anti-vacuity: the pool must keep exponent-255 pairs — "
								   "the class the ±fMax operand clamp folded together";
}

// ---------------------------------------------------------------------------
// Register aliasing. fd==fs and fd==ft are what the capture's D-alias section
// exercises, and they are the shapes where writing the destination early would
// destroy an operand the emitter still needs. fs==ft is the degenerate case.
// ---------------------------------------------------------------------------
TEST(EeFpuMinMaxConsole, AliasedRegistersSelectTheSameWord)
{
	static constexpr u32 kPairs[][2] = {
		{0x00000001u, 0x80000001u},
		{0x7FFFFFFFu, 0x7F800000u},
		{0x007FFFFFu, 0x00800000u},
		{0xDEADBEEFu, 0x3F800000u},
		{0x80000000u, 0x00000000u},
	};
	constexpr int kPairCount = static_cast<int>(sizeof(kPairs) / sizeof(kPairs[0]));

	int checked = 0;
	for (int m = 0; m < MODE_COUNT; ++m)
	{
		const Mode mode = static_cast<Mode>(m);
		for (int i = 0; i < kPairCount; ++i)
		{
			const u32 a = kPairs[i][0], b = kPairs[i][1];
			for (int ismin = 0; ismin < 2; ++ismin)
			{
				SCOPED_TRACE(testing::Message()
					<< (ismin ? "min " : "max ") << std::hex << a << "," << b
					<< " [" << ModeName(mode) << "]");
				// fd == fs
				EXPECT_EQ(RunOne(Encode(ismin != 0, kFs, kFs, kFt), kFs, a, b, true, mode),
					ModelMinMax(a, b, ismin != 0));
				// fd == ft
				EXPECT_EQ(RunOne(Encode(ismin != 0, kFt, kFs, kFt), kFt, a, b, true, mode),
					ModelMinMax(a, b, ismin != 0));
				// fs == ft: the answer is that word, whichever op
				EXPECT_EQ(RunOne(Encode(ismin != 0, kFd, kFs, kFs), kFd, a, b, true, mode), a);
				checked += 3;
			}
		}
	}
	EXPECT_EQ(checked, kPairCount * 2 * 3 * MODE_COUNT) << "anti-vacuity";
}

// ---------------------------------------------------------------------------
// MAX.S/MIN.S clear O and U and touch nothing else in FCR31. The two expected
// words are console readings: capture cases 734 (max ONE, TWO) and 735
// (min P0, N0), the only two MAX/MIN rows seeded with a dirty FCR31, both
// 0x0083C078 in and 0x01830079 back out through CFC1.
//
// Pinned here as well as in ee_fpu_fcr_console_conformance_tests.cpp because
// the emitter now reaches for raw GPR scratch around fpuClearOUFlags(), and the
// order of the two matters: the flag RMW must happen before the scratch goes
// live, or an allocator eviction lands in the middle of the compare.
// ---------------------------------------------------------------------------
TEST(EeFpuMinMaxConsole, ClearsOverflowAndUnderflowOnly)
{
	constexpr u32 kSeed = 0x0083C078u;    // every writable flag set, O and U among them
	constexpr u32 kConsole = 0x01830079u; // O|U gone, sticky bits and fixed ones kept

	int checked = 0;
	for (int m = 0; m < MODE_COUNT; ++m)
	{
		const Mode mode = static_cast<Mode>(m);
		for (int ismin = 0; ismin < 2; ++ismin)
		{
			for (int jit = 0; jit < 2; ++jit)
			{
				SCOPED_TRACE(testing::Message()
					<< (ismin ? "min.s" : "max.s") << (jit ? " [jit " : " [interp ")
					<< ModeName(mode) << "]");
				EeRecTestHarness h;
				h.EnableCop1();
				if (jit)
				{
					if (mode == MODE_CLAMP0)
						h.DisableFpuOverflow();
					if (mode == MODE_EXTRA)
						h.EnableFpuExtraOverflow();
					if (mode == MODE_FULL)
						h.EnableFpuFullMode();
				}
				h.SetFcr31(kSeed);
				h.SetFprBits(kFs, ismin ? 0x00000000u : 0x3F800000u);
				h.SetFprBits(kFt, ismin ? 0x80000000u : 0x40000000u);
				h.SetGpr128(reg::v0, 0, 0);
				h.LoadProgram({Encode(ismin != 0, kFd, kFs, kFt), ee::CFC1(reg::v0, 31)});
				if (jit)
					h.RunJitNoDiff();
				else
					h.RunInterpOnly();
				EXPECT_EQ(jit ? h.GetGprJit(reg::v0) : h.GetGprInterp(reg::v0), kConsole);
				EXPECT_EQ(jit ? h.GetFprBitsJit(kFd) : h.GetFprBitsInterp(kFd),
					ismin ? 0x80000000u : 0x40000000u);
				++checked;
			}
		}
	}
	EXPECT_EQ(checked, MODE_COUNT * 2 * 2) << "anti-vacuity";
}

// ---------------------------------------------------------------------------
// The FCR31-residency hazard, same class as
// EeRecFpu.CompareSurvivesInterposedGuardedAddCfc1 (the SotC glitch).
//
// A resident FCR31 (GE-12) lives in the ARM64TYPE_FPRC pool {x2-x7, x14, x15}.
// recMINMAX raw-clobbers w0/w1/w8/w9, all outside that pool — this asserts it,
// by parking FCR31 in a pool register with a C.LT and reading it back across an
// interposed MAX.S/MIN.S whose operands would set bit 23 in the clobbered regs.
//
// 0x00800000 is FPUflagC's bit position, so an operand of that shape landing in
// the FCR31 host register is exactly what flips a false compare to true.
// ---------------------------------------------------------------------------
TEST(EeFpuMinMaxConsole, CompareSurvivesInterposedMinMax)
{
	for (int ismin = 0; ismin < 2; ++ismin)
	{
		SCOPED_TRACE(ismin ? "min.s" : "max.s");
		EeRecTestHarness h;
		h.EnableCop1();
		h.SetFpr(1, 2.0f);
		h.SetFpr(2, 1.0f);
		h.SetFprBits(kFs, 0x00800000u); // bit 23 set — FPUflagC's position
		h.SetFprBits(kFt, 0x00FFFFFFu); // bit 23 set as well
		h.LoadProgram({
			ee::C_LT_S(1, 2),                        // 2 < 1 → false → C = 0, FCR31 resident
			Encode(ismin != 0, kFd, kFs, kFt),       // must not touch the FPRC pool
			ee::CFC1(reg::v0, 31),
		});
		h.RunJitNoDiff();
		EXPECT_EQ(h.GetGpr64Jit(reg::v0), 0x01000001ull); // C clear + always-one bits
	}
}
