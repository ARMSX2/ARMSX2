// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// COP2 macro-mode broadcast MAX/MINI and the fixed-point conversion family.
//
// ee_vu0_cop2_macro_tests.cpp covers the non-broadcast arithmetic ops and a few
// of the broadcast MADD/MUL variants. The whole broadcast MAX/MINI row
// (recVMAXx/y/z/w, recVMINIx/y/z/w — COP2 SPECIAL1 funct 0x10-0x17) and most of
// the conversion row (recVITOF0/4/12/15, recVFTOI12/15 — SPECIAL2 indices 16-23)
// had no coverage at all. Both are places where an arm64 transcription is more
// likely to be wrong than the arithmetic ops that already have tests:
//
// MAX/MINI — the PS2 VU has no inf or NaN encodings, so an exponent-0xFF word
// is just a very large number that has to be ordered like one. iCOP2-arm64.cpp
// therefore does not use a float compare at all: cop2EmitIntegerMax/Min order
// the operands as sign-magnitude integers, via CMGT corrected by a both-negative
// mask, because signed integer comparison alone gets two negatives backwards.
// Two things there are worth pinning. The correction itself, which only shows up
// when both operands are negative and is silently absent otherwise; and the
// choice of an integer compare in the first place, which only differs from
// Fmaxnm/Fminnm on exp-FF operands — exactly the bit patterns a QMTC2 leaves in
// a register, the same shape as the True Crime black-world bug.
//
// ITOF/FTOI — the scale is baked into the opcode (0, 4, 12 or 15 fractional
// bits), so a wrong shift is a silently wrong magnitude rather than a crash.
// FTOI additionally has to saturate: out-of-range float-to-int is UB in C and
// the hosts disagree, with arm64 clamping to INT_MAX/INT_MIN where x86 returns
// INT_MIN for everything. The VU's own answer is the saturating one, so the
// out-of-range cases below are the ones that would catch a port that let the
// host instruction decide.
//
// The oracle throughout is the VU0 interpreter via EeRecTestHarness's
// JIT-vs-interpreter diff, which CLAUDE.md records as a zero-known-bug baseline.
// Absolute expectations are asserted as well wherever the architectural answer
// is unambiguous, so a test failure says which side moved.

#include "harness/EeRecTestHarness.h"

#include "VU.h"
#include "VUmicro.h"
#include "Config.h"

#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;
using namespace vu;

namespace {

constexpr u32 mask_xyzw = 0xF;

// COP2 SPECIAL1 funct values, read off recCOP2SPECIAL1t in iR5900Misc-arm64.cpp:
// row 2 is MAXx/y/z/w then MINIx/y/z/w.
constexpr u32 VMAXx_C2 (u32 m, u32 fd, u32 fs, u32 ft) { return COP2_FMAC(m, fd, fs, ft, 0x10); }
constexpr u32 VMAXy_C2 (u32 m, u32 fd, u32 fs, u32 ft) { return COP2_FMAC(m, fd, fs, ft, 0x11); }
constexpr u32 VMAXz_C2 (u32 m, u32 fd, u32 fs, u32 ft) { return COP2_FMAC(m, fd, fs, ft, 0x12); }
constexpr u32 VMAXw_C2 (u32 m, u32 fd, u32 fs, u32 ft) { return COP2_FMAC(m, fd, fs, ft, 0x13); }
constexpr u32 VMINIx_C2(u32 m, u32 fd, u32 fs, u32 ft) { return COP2_FMAC(m, fd, fs, ft, 0x14); }
constexpr u32 VMINIy_C2(u32 m, u32 fd, u32 fs, u32 ft) { return COP2_FMAC(m, fd, fs, ft, 0x15); }
constexpr u32 VMINIz_C2(u32 m, u32 fd, u32 fs, u32 ft) { return COP2_FMAC(m, fd, fs, ft, 0x16); }
constexpr u32 VMINIw_C2(u32 m, u32 fd, u32 fs, u32 ft) { return COP2_FMAC(m, fd, fs, ft, 0x17); }

// SPECIAL2 conversion row: sub-op 0x04 = ITOF, 0x05 = FTOI, with the funct low
// two bits selecting the fixed-point fraction (0x3C/3D/3E/3F = 0/4/12/15).
// VITOF0 / VFTOI0 / VFTOI4 already have encoders in MipsEncode.h.
constexpr u32 VITOF4_C2 (u32 m, u32 ft, u32 fs) { return COP2_FMAC(m, 0x04, fs, ft, 0x3D); }
constexpr u32 VITOF12_C2(u32 m, u32 ft, u32 fs) { return COP2_FMAC(m, 0x04, fs, ft, 0x3E); }
constexpr u32 VITOF15_C2(u32 m, u32 ft, u32 fs) { return COP2_FMAC(m, 0x04, fs, ft, 0x3F); }

// Bit patterns that are ordinary VU numbers but host NaNs / infinities.
constexpr u32 kExpFfQuiet = 0x7FFFFFFFu; // largest positive exp-FF word
constexpr u32 kExpFfNegQ  = 0xFFFFFFFFu; // its negative counterpart
constexpr u32 kPosInfBits = 0x7F800000u;
constexpr u32 kNegInfBits = 0xFF800000u;

void ExpectAllLanesAgree(const EeRecTestHarness& h, u32 reg)
{
	for (char l : {'x', 'y', 'z', 'w'})
		EXPECT_EQ(h.GetVu0VfBitsJit(reg, l), h.GetVu0VfBitsInterp(reg, l))
			<< "vf" << reg << " lane " << l;
}

} // namespace

// =========================================================================
//  Broadcast MAX — fd.lane = max(fs.lane, ft.<bc>)
// =========================================================================

TEST(EeVu0Cop2MaxMini, VmaxBroadcastsEachLaneOfFt)
{
	// One test per broadcast lane, sharing operands so the only thing that
	// changes is which lane of ft is splatted. fs straddles the broadcast
	// values so each lane exercises both the "keep fs" and "take ft" side.
	struct Variant { const char* name; u32 (*enc)(u32, u32, u32, u32); float bc; };
	const Variant variants[] = {
		{"x", VMAXx_C2, 5.0f},
		{"y", VMAXy_C2, 15.0f},
		{"z", VMAXz_C2, 25.0f},
		{"w", VMAXw_C2, 35.0f},
	};

	for (const Variant& v : variants)
	{
		SCOPED_TRACE(v.name);
		EeRecTestHarness h;
		h.EnableVu0Capture();
		h.EnableCop1();
		h.SeedVu0Vf(1, 0.0f, 10.0f, 20.0f, 30.0f);
		h.SeedVu0Vf(2, 5.0f, 15.0f, 25.0f, 35.0f);
		h.LoadProgram({v.enc(mask_xyzw, /*fd*/3, /*fs*/1, /*ft*/2)});
		h.Run();

		EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'x'), std::max(0.0f, v.bc));
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'y'), std::max(10.0f, v.bc));
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'z'), std::max(20.0f, v.bc));
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'w'), std::max(30.0f, v.bc));
		ExpectAllLanesAgree(h, 3);
	}
}

TEST(EeVu0Cop2MaxMini, VminiBroadcastsEachLaneOfFt)
{
	struct Variant { const char* name; u32 (*enc)(u32, u32, u32, u32); float bc; };
	const Variant variants[] = {
		{"x", VMINIx_C2, 5.0f},
		{"y", VMINIy_C2, 15.0f},
		{"z", VMINIz_C2, 25.0f},
		{"w", VMINIw_C2, 35.0f},
	};

	for (const Variant& v : variants)
	{
		SCOPED_TRACE(v.name);
		EeRecTestHarness h;
		h.EnableVu0Capture();
		h.EnableCop1();
		h.SeedVu0Vf(1, 0.0f, 10.0f, 20.0f, 30.0f);
		h.SeedVu0Vf(2, 5.0f, 15.0f, 25.0f, 35.0f);
		h.LoadProgram({v.enc(mask_xyzw, 3, 1, 2)});
		h.Run();

		EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'x'), std::min(0.0f, v.bc));
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'y'), std::min(10.0f, v.bc));
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'z'), std::min(20.0f, v.bc));
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'w'), std::min(30.0f, v.bc));
		ExpectAllLanesAgree(h, 3);
	}
}

TEST(EeVu0Cop2MaxMini, VmaxNegativeOperandsOrderCorrectly)
{
	// Sign handling is the other half of MAX/MINI. A comparison done on the raw
	// integer bits rather than as floats gets negatives backwards.
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, -1.0f, -100.0f, 1.0f, -0.0f);
	h.SeedVu0Vf(2, -50.0f, -50.0f, -50.0f, -50.0f);
	h.LoadProgram({VMAXx_C2(mask_xyzw, 3, 1, 2)});
	h.Run();

	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'x'), -1.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'y'), -50.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'z'), 1.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'w'), -0.0f);
	ExpectAllLanesAgree(h, 3);
}

TEST(EeVu0Cop2MaxMini, VminiNegativeOperandsOrderCorrectly)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, -1.0f, -100.0f, 1.0f, 0.0f);
	h.SeedVu0Vf(2, -50.0f, -50.0f, -50.0f, -50.0f);
	h.LoadProgram({VMINIx_C2(mask_xyzw, 3, 1, 2)});
	h.Run();

	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'x'), -50.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'y'), -100.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'z'), -50.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'w'), -50.0f);
	ExpectAllLanesAgree(h, 3);
}

// The reason this file exists. Fmax vs Fmaxnm only diverge on host-NaN
// operands, which on the VU are ordinary large numbers, so no amount of
// well-behaved test data would separate them.
TEST(EeVu0Cop2MaxMini, VmaxExpFfOperandsAreOrderedNotPropagated)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(1, kExpFfQuiet, kExpFfNegQ, kPosInfBits, kNegInfBits);
	h.SeedVu0VfBits(2, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u); // 1.0
	h.LoadProgram({VMAXx_C2(mask_xyzw, 3, 1, 2)});
	h.Run();
	ExpectAllLanesAgree(h, 3);
}

TEST(EeVu0Cop2MaxMini, VminiExpFfOperandsAreOrderedNotPropagated)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(1, kExpFfQuiet, kExpFfNegQ, kPosInfBits, kNegInfBits);
	h.SeedVu0VfBits(2, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);
	h.LoadProgram({VMINIx_C2(mask_xyzw, 3, 1, 2)});
	h.Run();
	ExpectAllLanesAgree(h, 3);
}

// Same operands, but the exp-FF word arrives as the broadcast source rather
// than the per-lane one — the two sides go into different NEON operand slots.
TEST(EeVu0Cop2MaxMini, VmaxExpFfAsBroadcastSource)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, -1.0f, 0.0f, 1.0f, 100.0f);
	h.SeedVu0VfBits(2, kExpFfQuiet, 0u, 0u, 0u);
	h.LoadProgram({VMAXx_C2(mask_xyzw, 3, 1, 2)});
	h.Run();
	ExpectAllLanesAgree(h, 3);
}

TEST(EeVu0Cop2MaxMini, VminiExpFfAsBroadcastSource)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, -1.0f, 0.0f, 1.0f, 100.0f);
	h.SeedVu0VfBits(2, kExpFfNegQ, 0u, 0u, 0u);
	h.LoadProgram({VMINIx_C2(mask_xyzw, 3, 1, 2)});
	h.Run();
	ExpectAllLanesAgree(h, 3);
}

// A partial destination mask must leave the other lanes byte-identical.
TEST(EeVu0Cop2MaxMini, VmaxRespectsDestinationMask)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(3, 0xDEADBEEFu, 0xCAFEBABEu, 0x12345678u, 0x87654321u);
	h.SeedVu0Vf(1, 1.0f, 2.0f, 3.0f, 4.0f);
	h.SeedVu0Vf(2, 9.0f, 9.0f, 9.0f, 9.0f);
	h.LoadProgram({VMAXx_C2(/*y only*/ 0x4, 3, 1, 2)});
	h.Run();

	EXPECT_EQ(h.GetVu0VfBitsJit(3, 'x'), 0xDEADBEEFu);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(3, 'y'), 9.0f);
	EXPECT_EQ(h.GetVu0VfBitsJit(3, 'z'), 0x12345678u);
	EXPECT_EQ(h.GetVu0VfBitsJit(3, 'w'), 0x87654321u);
	ExpectAllLanesAgree(h, 3);
}

// fd aliasing fs and ft is the case where a codegen that materialises into the
// destination before reading its sources loses an operand.
TEST(EeVu0Cop2MaxMini, VmaxWithDestinationAliasingSources)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, 3.0f, -3.0f, 7.0f, -7.0f);
	h.LoadProgram({VMAXw_C2(mask_xyzw, /*fd*/1, /*fs*/1, /*ft*/1)});
	h.Run();

	// Broadcast is fs.w = -7.0, so every lane becomes max(lane, -7).
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(1, 'x'), 3.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(1, 'y'), -3.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(1, 'z'), 7.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(1, 'w'), -7.0f);
	ExpectAllLanesAgree(h, 1);
}

// =========================================================================
//  ITOF / FTOI — the scale lives in the opcode
// =========================================================================

TEST(EeVu0Cop2Conv, ItofAppliesTheOpcodeScale)
{
	// The same integer through all four scales. 4096 with 12 fractional bits is
	// 1.0; a port that used the wrong shift gives a power-of-two multiple, which
	// these distinct expectations pin apart.
	struct Variant { const char* name; u32 (*enc)(u32, u32, u32); float scale; };
	const Variant variants[] = {
		{"itof0",  VITOF0_C2,  1.0f},
		{"itof4",  VITOF4_C2,  1.0f / 16.0f},
		{"itof12", VITOF12_C2, 1.0f / 4096.0f},
		{"itof15", VITOF15_C2, 1.0f / 32768.0f},
	};

	for (const Variant& v : variants)
	{
		SCOPED_TRACE(v.name);
		EeRecTestHarness h;
		h.EnableVu0Capture();
		h.EnableCop1();
		h.SeedVu0VfBits(1, 4096u, static_cast<u32>(-4096), 1u, 0u);
		h.LoadProgram({v.enc(mask_xyzw, /*ft*/2, /*fs*/1)});
		h.Run();

		EXPECT_FLOAT_EQ(h.GetVu0VfJit(2, 'x'), 4096.0f * v.scale);
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(2, 'y'), -4096.0f * v.scale);
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(2, 'z'), 1.0f * v.scale);
		EXPECT_FLOAT_EQ(h.GetVu0VfJit(2, 'w'), 0.0f);
		ExpectAllLanesAgree(h, 2);
	}
}

TEST(EeVu0Cop2Conv, FtoiAppliesTheOpcodeScale)
{
	struct Variant { const char* name; u32 (*enc)(u32, u32, u32); float scale; };
	const Variant variants[] = {
		{"ftoi0",  VFTOI0_C2,  1.0f},
		{"ftoi4",  VFTOI4_C2,  16.0f},
		{"ftoi12", VFTOI12_C2, 4096.0f},
		{"ftoi15", VFTOI15_C2, 32768.0f},
	};

	for (const Variant& v : variants)
	{
		SCOPED_TRACE(v.name);
		EeRecTestHarness h;
		h.EnableVu0Capture();
		h.EnableCop1();
		h.SeedVu0Vf(1, 1.0f, -1.0f, 0.5f, 0.0f);
		h.LoadProgram({v.enc(mask_xyzw, /*ft*/2, /*fs*/1)});
		h.Run();

		EXPECT_EQ(h.GetVu0VfBitsJit(2, 'x'), static_cast<u32>(static_cast<s32>(1.0f * v.scale)));
		EXPECT_EQ(h.GetVu0VfBitsJit(2, 'y'), static_cast<u32>(static_cast<s32>(-1.0f * v.scale)));
		EXPECT_EQ(h.GetVu0VfBitsJit(2, 'z'), static_cast<u32>(static_cast<s32>(0.5f * v.scale)));
		EXPECT_EQ(h.GetVu0VfBitsJit(2, 'w'), 0u);
		ExpectAllLanesAgree(h, 2);
	}
}

// Out-of-range float-to-int is where the hosts disagree: arm64's FCVTZS
// saturates, x86's CVTTPS2DQ yields INT_MIN for everything out of range. The
// interpreter defines which one the VU wants, so this only needs to pin the two
// against each other — but it needs the operands that actually separate them.
TEST(EeVu0Cop2Conv, FtoiSaturatesOutOfRangeOperands)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, 1.0e10f, -1.0e10f, 2.5e9f, -2.5e9f);
	h.LoadProgram({VFTOI0_C2(mask_xyzw, 2, 1)});
	h.Run();
	ExpectAllLanesAgree(h, 2);
}

TEST(EeVu0Cop2Conv, FtoiScaledOverflowSaturates)
{
	// In range before the scale, out of range after it — catches a port that
	// saturates first and scales second.
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, 1.0e6f, -1.0e6f, 1.0e9f, -1.0e9f);
	h.LoadProgram({VFTOI15_C2(mask_xyzw, 2, 1)});
	h.Run();
	ExpectAllLanesAgree(h, 2);
}

TEST(EeVu0Cop2Conv, FtoiExpFfOperands)
{
	// exp-FF words are ordinary huge VU numbers; converting them must give the
	// interpreter's answer rather than whatever the host does with a NaN.
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(1, kExpFfQuiet, kExpFfNegQ, kPosInfBits, kNegInfBits);
	h.LoadProgram({VFTOI0_C2(mask_xyzw, 2, 1)});
	h.Run();
	ExpectAllLanesAgree(h, 2);
}

TEST(EeVu0Cop2Conv, ItofFullIntegerRange)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0x00000001u);
	h.LoadProgram({VITOF0_C2(mask_xyzw, 2, 1)});
	h.Run();
	ExpectAllLanesAgree(h, 2);
}

TEST(EeVu0Cop2Conv, ConversionRespectsDestinationMask)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(2, 0xDEADBEEFu, 0xCAFEBABEu, 0x12345678u, 0x87654321u);
	h.SeedVu0VfBits(1, 4096u, 4096u, 4096u, 4096u);
	h.LoadProgram({VITOF12_C2(/*z only*/ 0x2, 2, 1)});
	h.Run();

	EXPECT_EQ(h.GetVu0VfBitsJit(2, 'x'), 0xDEADBEEFu);
	EXPECT_EQ(h.GetVu0VfBitsJit(2, 'y'), 0xCAFEBABEu);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(2, 'z'), 1.0f);
	EXPECT_EQ(h.GetVu0VfBitsJit(2, 'w'), 0x87654321u);
	ExpectAllLanesAgree(h, 2);
}

// ft aliasing fs: the conversion writes its own source.
TEST(EeVu0Cop2Conv, ConversionInPlace)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0VfBits(1, 4096u, 8192u, 0u, static_cast<u32>(-4096));
	h.LoadProgram({VITOF12_C2(mask_xyzw, /*ft*/1, /*fs*/1)});
	h.Run();

	EXPECT_FLOAT_EQ(h.GetVu0VfJit(1, 'x'), 1.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(1, 'y'), 2.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(1, 'z'), 0.0f);
	EXPECT_FLOAT_EQ(h.GetVu0VfJit(1, 'w'), -1.0f);
	ExpectAllLanesAgree(h, 1);
}

} // namespace recompiler_tests
