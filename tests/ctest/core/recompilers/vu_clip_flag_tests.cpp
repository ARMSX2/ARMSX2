// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// CLIP's six result bits and the 24-bit history they shift into.
//
// One CLIP tests fs.x, fs.y and fs.z against +ft.w and -ft.w and contributes
// six bits to VI[REG_CLIP_FLAG], which holds the last four results: the flag
// shifts left by six and the new bits land at the bottom. The bit ORDER is
// the whole content of the low half -- x before y before z, and the + test
// before the - test within each -- and nothing about it is derivable from the
// values, so a gather that permutes two bits produces a well-formed flag that
// no arithmetic assertion catches.
//
// The two engines encode the same rule differently, which is why the model
// here is a third form rather than a transcription of either. The interpreter
// (VUops.cpp _vuCLIP) raises the threshold to the largest denormal pattern
// when ft.w has a zero exponent field and compares fs untouched; the
// recompiler (microVU_Upper-arm64.inl mVU_CLIP) zeroes fs lanes with a zero
// exponent field and leaves the threshold at |ft.w|. Below the smallest
// normal both reduce to "no comparison is true either way", so the two agree
// everywhere; the model states the rule as flushing both sides.

#include "harness/VuTestHarness.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper) { return IBit(VuOp{VLitZero(), upper}); }
inline VuOp Clip(u32 fs, u32 ft) { return UpperOnly(VCLIP_U(fs, ft)); }

// A CLIP operand whose exponent field is zero is a zero to this unit: the
// denormal's mantissa never reaches a comparison.
constexpr u32 FlushToZero(u32 bits)
{
	return (bits & 0x7f800000u) ? bits : 0u;
}

// The six bits one CLIP contributes: bit 0 = x > +|w|, bit 1 = x < -|w|,
// bits 2/3 the same for y, bits 4/5 for z.
u32 ClipBits(u32 x, u32 y, u32 z, u32 w)
{
	// Once neither side is denormal, the float comparison is the signed
	// integer comparison of the encodings: a non-negative float encodes as a
	// non-negative integer that grows with the value, a negative one as a
	// negative integer.
	const s32 threshold = static_cast<s32>(FlushToZero(w) & 0x7fffffffu);
	const u32 lane[3] = {FlushToZero(x), FlushToZero(y), FlushToZero(z)};

	u32 bits = 0;
	for (int i = 0; i < 3; i++)
	{
		if (static_cast<s32>(lane[i]) > threshold)
			bits |= 1u << (2 * i);
		if (static_cast<s32>(lane[i] ^ 0x80000000u) > threshold)
			bits |= 2u << (2 * i);
	}
	return bits;
}

// What the flag holds after one CLIP folds into it.
constexpr u32 FoldClip(u32 flag, u32 bits)
{
	return ((flag << 6) | bits) & 0xFFFFFFu;
}

// The operand classes a CLIP has to separate. Both signs of each, because the
// threshold is |ft.w| and the two per-lane tests are the two signs of fs.
constexpr u32 kValues[] = {
	0x00000000u, 0x80000000u, // zero
	0x00000001u, 0x80000001u, // smallest denormal
	0x007fffffu, 0x807fffffu, // largest denormal
	0x00800000u, 0x80800000u, // smallest normal
	0x3f800000u, 0xbf800000u, // 1.0
	0x40000000u, 0xc0000000u, // 2.0 -- equal to the threshold the sweep uses,
	0x3fffffffu, 0xbfffffffu, // one ULP below it,
	0x40000001u, 0xc0000001u, // and one ULP above: the comparison is strict
	0x40600000u, 0xc0600000u, // 3.5
	0x7f7fffffu, 0xff7fffffu, // FLT_MAX
	0x7f800000u, 0xff800000u, // the infinity encoding
	0x7fc00000u, 0xffc00000u, // a quiet NaN encoding
	0x7fffffffu, 0xffffffffu, // the VU FMAC's ceiling
};
constexpr int kValueCount = static_cast<int>(std::size(kValues));

struct ClipCase
{
	u32 x, y, z, w;
	u32 fs, ft;
};

// Four CLIPs per program, which is exactly what the 24-bit flag retains, so
// one run scores four cases and the history shift on each of them. The flag
// each case shifts into is the previous case's result: VI[REG_CLIP_FLAG] is
// not the running copy either engine reads (the interpreter keeps VURegs
// clipflag, the recompiler a four-slot ring), so a program's incoming history
// can only be built by its own earlier CLIPs.
void RunQuad(int vuIndex, const ClipCase (&cases)[4])
{
	VuTestHarness h(vuIndex);

	std::vector<VuOp> program;
	for (const ClipCase& c : cases)
	{
		h.SetVfBits(c.fs, c.x, c.y, c.z, c.w);
		h.SetVfBits(c.ft, c.x, c.y, c.z, c.w);
		program.push_back(Clip(c.fs, c.ft));
	}
	program.push_back(EBitNopPair());
	h.LoadProgram(std::move(program));
	h.Run();

	u32 want = 0;
	for (const ClipCase& c : cases)
		want = FoldClip(want, ClipBits(c.x, c.y, c.z, c.w));

	std::string where;
	for (const ClipCase& c : cases)
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf), " [vf%u=%08x,%08x,%08x vs vf%u.w=%08x]",
			c.fs, c.x, c.y, c.z, c.ft, c.w);
		where += buf;
	}

	EXPECT_EQ(h.GetViJit(REG_CLIP_FLAG), want) << "jit" << where;
	EXPECT_EQ(h.GetViInterp(REG_CLIP_FLAG), want) << "interp" << where;
}

// fs and ft come from the same quad, so the w each case tests against is its
// own register's w: pairing register indices this way keeps every case's four
// operands in one VF pair and lets a case set fs == ft.
ClipCase MakeCase(u32 x, u32 y, u32 z, u32 w, int slot, bool alias)
{
	const u32 fs = static_cast<u32>(vf::vf1) + static_cast<u32>(slot);
	return ClipCase{x, y, z, w, fs, alias ? fs : fs + 4u};
}

// A 32-bit xorshift, so the case stream is the same on every standard library.
struct Rng
{
	u32 s;
	u32 Next()
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
};

} // namespace

// Every operand class against every threshold class, once per lane: the swept
// class takes x, then y, then z, with the other two lanes walking the table
// alongside so all three see all of it. The threshold classes include 2.0 and
// the operand classes include 2.0 and both its neighbours, so the strict
// comparison is decided in both directions.
TEST(VuClipFlag, LaneAndThresholdClassesAgreeWithTheRule)
{
	ClipCase quad[4];
	int filled = 0;

	for (int lead = 0; lead < 3; lead++)
	{
		for (int wi = 0; wi < kValueCount; wi++)
		{
			for (int vi = 0; vi < kValueCount; vi++)
			{
				u32 lane[3];
				lane[lead] = kValues[vi];
				lane[(lead + 1) % 3] = kValues[(vi + 7) % kValueCount];
				lane[(lead + 2) % 3] = kValues[(vi + 13) % kValueCount];
				quad[filled] = MakeCase(lane[0], lane[1], lane[2], kValues[wi], filled, false);
				if (++filled == 4)
				{
					RunQuad(0, quad);
					filled = 0;
				}
			}
		}
	}
	ASSERT_EQ(filled, 0) << "the class sweep must divide into whole quads";
}

// The same rule with the four lanes drawn independently, on both VUs, and
// with fs == ft on a share of the cases -- the aliased form is the one where
// the recompiler allocates two registers from one VF and the flush it applies
// to fs must not reach the copy the threshold comes from.
TEST(VuClipFlag, RandomOperandCombinationsAgreeWithTheRule)
{
	Rng rng{0xC11Fu};
	for (int vuIndex = 0; vuIndex <= 1; vuIndex++)
	{
		for (int program = 0; program < 400; program++)
		{
			ClipCase quad[4];
			for (int slot = 0; slot < 4; slot++)
			{
				const u32 x = kValues[rng.Next() % kValueCount];
				const u32 y = kValues[rng.Next() % kValueCount];
				const u32 z = kValues[rng.Next() % kValueCount];
				const u32 w = kValues[rng.Next() % kValueCount];
				quad[slot] = MakeCase(x, y, z, w, slot, (rng.Next() & 3u) == 0u);
			}
			RunQuad(vuIndex, quad);
		}
	}
}

// vf0 is the constant (0, 0, 0, 1) and the register allocator gives it its own
// path, so a CLIP that reads it reaches code no other case here does. Both
// operand positions, since fs is flushed and ft is not.
TEST(VuClipFlag, Vf0OperandsTakeTheConstantRegisterPath)
{
	for (int vuIndex = 0; vuIndex <= 1; vuIndex++)
	{
		VuTestHarness h(vuIndex);
		h.SetVfBits(vf::vf1, 0x40000000u, 0xc0000000u, 0x00000001u, 0x3f000000u);
		h.LoadProgram({
			Clip(vf::vf0, vf::vf1),
			Clip(vf::vf1, vf::vf0),
			Clip(vf::vf0, vf::vf0),
			EBitNopPair(),
		});
		h.Run();

		// vf0 is (0, 0, 0, 1) in every VU.
		u32 want = 0;
		want = FoldClip(want, ClipBits(0, 0, 0, 0x3f000000u));
		want = FoldClip(want, ClipBits(0x40000000u, 0xc0000000u, 0x00000001u, 0x3f800000u));
		want = FoldClip(want, ClipBits(0, 0, 0, 0x3f800000u));

		EXPECT_EQ(h.GetViJit(REG_CLIP_FLAG), want) << "jit, vu" << vuIndex;
		EXPECT_EQ(h.GetViInterp(REG_CLIP_FLAG), want) << "interp, vu" << vuIndex;
	}
}

// The history is 24 bits and a CLIP shifts it by six, so the fifth result back
// is gone. A program of six CLIPs whose results are all distinct pins both the
// shift and the discard: read the flag as four 6-bit fields and the first two
// results must be absent from all of them.
TEST(VuClipFlag, HistoryKeepsTheLastFourResultsAndDropsTheRest)
{
	VuTestHarness h(0);
	// Six operand sets chosen so the six results are six different values.
	const u32 x[6] = {0x40000000u, 0xc0000000u, 0x00000000u, 0x40000000u, 0x00000000u, 0xc0000000u};
	const u32 y[6] = {0x00000000u, 0x40000000u, 0xc0000000u, 0xc0000000u, 0x40000000u, 0x00000000u};
	const u32 z[6] = {0xc0000000u, 0x00000000u, 0x40000000u, 0x00000000u, 0xc0000000u, 0x40000000u};
	const u32 w = 0x3f800000u; // 1.0, below every operand magnitude above

	std::vector<VuOp> program;
	for (int i = 0; i < 6; i++)
	{
		h.SetVfBits(static_cast<u32>(vf::vf1) + static_cast<u32>(i), x[i], y[i], z[i], w);
		program.push_back(Clip(static_cast<u32>(vf::vf1) + static_cast<u32>(i),
			static_cast<u32>(vf::vf1) + static_cast<u32>(i)));
	}
	program.push_back(EBitNopPair());
	h.LoadProgram(std::move(program));
	h.Run();

	u32 want = 0;
	for (int i = 0; i < 6; i++)
		want = FoldClip(want, ClipBits(x[i], y[i], z[i], w));

	// The four results the flag should hold, newest in the low field.
	for (int back = 0; back < 4; back++)
	{
		const u32 field = (want >> (6 * back)) & 0x3Fu;
		ASSERT_EQ(field, ClipBits(x[5 - back], y[5 - back], z[5 - back], w))
			<< "the model's own history field " << back;
	}
	// Six distinct results, so a flag that kept the wrong four is a different
	// number rather than an accidental match.
	for (int a = 0; a < 6; a++)
	{
		for (int b = a + 1; b < 6; b++)
		{
			ASSERT_NE(ClipBits(x[a], y[a], z[a], w), ClipBits(x[b], y[b], z[b], w))
				<< "results " << a << " and " << b << " collide";
		}
	}

	EXPECT_EQ(h.GetViJit(REG_CLIP_FLAG), want) << "jit";
	EXPECT_EQ(h.GetViInterp(REG_CLIP_FLAG), want) << "interp";
}

} // namespace recompiler_tests
