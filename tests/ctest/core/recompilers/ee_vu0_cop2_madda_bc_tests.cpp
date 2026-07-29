// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// COP2 macro-mode broadcast MADDA/MSUBA: ACC = ACC +/- VF[fs] * VF[ft].bc
//
// These are SPECIAL2 indices 0x08-0x0F, emitted by COP2_MADDA_BC in
// iCOP2-arm64.cpp. They are the backbone of an EE-side 4x4 transform — the
// MULAx/MADDAy/MADDAz/MADDAw chain that accumulates a matrix-vector product —
// so a wrong answer here shows up as displaced geometry rather than anything
// that trips an assert.
//
// The whole family shares one emitter macro parameterized only by the
// broadcast lane, which makes it easy to assume the four lanes are
// interchangeable. They are not: the dest-mask write-back and the ACC operand
// fetch interact with the lane, and a sweep over (lane x dest mask) is the
// only thing that pins every combination the macro can generate.
//
// The oracle is the VU0 interpreter via EeRecTestHarness's JIT-vs-interpreter
// diff, which CLAUDE.md records as a zero-known-bug baseline.

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

// COP2 SPECIAL2 is reached with funct 0x3C-0x3F; the recCOP2SPECIAL2t index is
// (code & 3) | ((code >> 4) & 0x7C), i.e. the low two bits of funct plus the
// FD/SA field shifted up by two. So an index maps back to
// funct = 0x3C | (idx & 3) and sa = idx >> 2.
constexpr u32 COP2_SPEC2(u32 mask_xyzw, u32 idx, u32 fs, u32 ft)
{
	return COP2_FMAC(mask_xyzw, /*fd/sa*/ idx >> 2, fs, ft, 0x3Cu | (idx & 3u));
}

// SPECIAL2 row 1: 0x08-0x0B = MADDAx/y/z/w, 0x0C-0x0F = MSUBAx/y/z/w.
constexpr u32 VMADDAx_C2(u32 m, u32 fs, u32 ft) { return COP2_SPEC2(m, 0x08, fs, ft); }
constexpr u32 VMADDAy_C2(u32 m, u32 fs, u32 ft) { return COP2_SPEC2(m, 0x09, fs, ft); }
constexpr u32 VMADDAz_C2(u32 m, u32 fs, u32 ft) { return COP2_SPEC2(m, 0x0A, fs, ft); }
constexpr u32 VMADDAw_C2(u32 m, u32 fs, u32 ft) { return COP2_SPEC2(m, 0x0B, fs, ft); }
constexpr u32 VMSUBAx_C2(u32 m, u32 fs, u32 ft) { return COP2_SPEC2(m, 0x0C, fs, ft); }
constexpr u32 VMSUBAy_C2(u32 m, u32 fs, u32 ft) { return COP2_SPEC2(m, 0x0D, fs, ft); }
constexpr u32 VMSUBAz_C2(u32 m, u32 fs, u32 ft) { return COP2_SPEC2(m, 0x0E, fs, ft); }
constexpr u32 VMSUBAw_C2(u32 m, u32 fs, u32 ft) { return COP2_SPEC2(m, 0x0F, fs, ft); }

struct BcOp
{
	const char* name;
	u32 (*encode)(u32 m, u32 fs, u32 ft);
	char lane;
};

const BcOp kMaddaOps[] = {
	{"VMADDAx", VMADDAx_C2, 'x'}, {"VMADDAy", VMADDAy_C2, 'y'},
	{"VMADDAz", VMADDAz_C2, 'z'}, {"VMADDAw", VMADDAw_C2, 'w'},
};

const BcOp kMsubaOps[] = {
	{"VMSUBAx", VMSUBAx_C2, 'x'}, {"VMSUBAy", VMSUBAy_C2, 'y'},
	{"VMSUBAz", VMSUBAz_C2, 'z'}, {"VMSUBAw", VMSUBAw_C2, 'w'},
};

// Run one op at one dest mask and report the JIT-vs-interpreter ACC diff.
void CheckOneCase(const BcOp& op, u32 mask,
	const float (&fs)[4], const float (&ft)[4], const float (&acc)[4])
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vf(1, fs[0], fs[1], fs[2], fs[3]);
	h.SeedVu0Vf(2, ft[0], ft[1], ft[2], ft[3]);
	h.SeedVu0Acc(acc[0], acc[1], acc[2], acc[3]);
	h.LoadProgram({op.encode(mask, /*fs*/1, /*ft*/2)});
	h.Run();

	for (char l : {'x', 'y', 'z', 'w'})
	{
		EXPECT_EQ(h.GetVu0AccBitsJit(l), h.GetVu0AccBitsInterp(l))
			<< op.name << " dest mask 0x" << std::hex << mask << std::dec
			<< " ACC." << l << ": JIT and interpreter disagree";
	}
}

const float kFs[4] = {1.5f, -2.25f, 3.75f, -4.5f};
const float kFt[4] = {5.5f, 6.25f, -7.75f, 8.5f};
const float kAcc[4] = {100.0f, -200.0f, 300.0f, -400.0f};

} // namespace

// Every broadcast lane against every dest mask. The x/y/z lanes and the full
// 0xF mask are the well-trodden paths; the partial masks are where the
// write-back picks a different shape (single-lane insert vs BSL merge vs
// in-place full overwrite).
TEST(EeVu0Cop2MaddaBc, EveryLaneEveryDestMaskMatchesInterp)
{
	for (const BcOp* ops : {kMaddaOps, kMsubaOps})
	for (int i = 0; i < 4; i++)
	{
		const BcOp& op = ops[i];
		for (u32 mask = 0; mask <= 0xF; mask++)
		{
			SCOPED_TRACE(::testing::Message()
				<< op.name << " mask=0x" << std::hex << mask);
			CheckOneCase(op, mask, kFs, kFt, kAcc);
		}
	}
}

// The transform shape the EE actually runs: seed ACC via MULAx, accumulate
// y/z, then finish with the w lane. This is the sequence NASCAR Thunder 2002
// uses to place car geometry, and it exercises MADDAw with an ACC that a
// previous macro op just produced rather than one seeded from memory.
TEST(EeVu0Cop2MaddaBc, MatrixVectorChainFullMaskMatchesInterp)
{
	EeRecTestHarness h;
	h.EnableVu0Capture();
	h.EnableCop1();
	// Four matrix rows in vf1..vf4, the vector in vf5.
	h.SeedVu0Vf(1, 1.0f, 2.0f, 3.0f, 4.0f);
	h.SeedVu0Vf(2, 5.0f, 6.0f, 7.0f, 8.0f);
	h.SeedVu0Vf(3, 9.0f, 10.0f, 11.0f, 12.0f);
	h.SeedVu0Vf(4, 13.0f, 14.0f, 15.0f, 16.0f);
	h.SeedVu0Vf(5, 0.5f, -1.5f, 2.5f, 1.0f);
	h.SeedVu0Acc(0.0f, 0.0f, 0.0f, 0.0f);

	h.LoadProgram({
		COP2_SPEC2(0xF, 0x18, /*fs*/1, /*ft*/5), // VMULAx  ACC = vf1 * vf5.x
		VMADDAy_C2(0xF, /*fs*/2, /*ft*/5),       // ACC += vf2 * vf5.y
		VMADDAz_C2(0xF, /*fs*/3, /*ft*/5),       // ACC += vf3 * vf5.z
		VMADDAw_C2(0xF, /*fs*/4, /*ft*/5),       // ACC += vf4 * vf5.w
	});
	h.Run();

	for (char l : {'x', 'y', 'z', 'w'})
	{
		EXPECT_EQ(h.GetVu0AccBitsJit(l), h.GetVu0AccBitsInterp(l))
			<< "matrix-vector chain ACC." << l;
	}
}

// The PS2 VU has no infinities or NaNs: an exponent-0xFF word is an ordinary,
// very large number. x86 mVU therefore specifies cFs for the whole MADDA
// broadcast row (mVU_MADDAx/y/z/w in microVU_Upper.inl), clamping Fs to
// +/-FLT_MAX *before* the multiply. Without that pre-clamp an exp-FF Fs against
// a zero broadcast lane multiplies as host Inf * 0 = NaN, and the post-op
// result clamp then folds the NaN to +/-FLT_MAX instead of the architectural 0.
//
// A zero in the broadcast lane is exactly what a homogeneous transform feeds
// the w-lane variant, which is why this shows up in one lane of a family whose
// four members share an emitter.
//
// MSUBAx/y/z/w are deliberately excluded: x86 gives them clampType 0, so their
// unclamped Fs is a shared, by-design divergence rather than an arm64 defect.
TEST(EeVu0Cop2MaddaBc, ExpFfFsAgainstZeroBroadcastMatchesInterp)
{
	constexpr u32 kExpFfPos = 0x7FFFFFFFu; // largest positive exp-FF word
	constexpr u32 kExpFfNeg = 0xFFFFFFFFu;
	constexpr u32 kPosInf   = 0x7F800000u;

	for (const BcOp& op : kMaddaOps)
	{
		for (u32 fsBits : {kExpFfPos, kExpFfNeg, kPosInf})
		{
			SCOPED_TRACE(::testing::Message()
				<< op.name << " fs=0x" << std::hex << fsBits);

			EeRecTestHarness h;
			h.EnableVu0Capture();
			h.EnableCop1();
			// Every Fs lane is exp-FF, so whichever lanes the dest mask keeps
			// exercise the missing pre-clamp.
			h.SeedVu0VfBits(1, fsBits, fsBits, fsBits, fsBits);
			// Ft is zero in every lane, so any broadcast lane multiplies by 0.
			h.SeedVu0Vf(2, 0.0f, 0.0f, 0.0f, 0.0f);
			h.SeedVu0Acc(1.0f, 2.0f, 3.0f, 4.0f);

			h.LoadProgram({op.encode(/*mask*/0xF, /*fs*/1, /*ft*/2)});
			h.Run();

			for (char l : {'x', 'y', 'z', 'w'})
			{
				EXPECT_EQ(h.GetVu0AccBitsJit(l), h.GetVu0AccBitsInterp(l))
					<< op.name << " ACC." << l
					<< ": exp-FF Fs against a zero broadcast";
			}
		}
	}
}

} // namespace recompiler_tests
