// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Whichever dither path this host selects must be bit-identical to the scalar one.
//
// ipu_dither() has three implementations -- a scalar reference, an SSE2 path and a
// NEON path -- and the emulator picks one at compile time from the architecture.
// Nothing ever compared them. That is a bad shape for a function like this: it is
// the last step of MPEG colour conversion, so an error does not crash, it tints an
// FMV slightly, and nobody files that as a bug.
//
// The transform is per-pixel and depends on nothing but the pixel's four bytes and
// its position modulo four in each axis, so the whole input domain is small enough
// to cover directly rather than sampled. The sweeps below walk every byte value
// through every one of the sixteen dither cells; the randomised case exists on top
// of that only to catch a path that crosses channels, which a sweep holding r, g
// and b equal would not see.
//
// Both dither states matter. dte=0 is not a trivial passthrough -- it still packs
// 8888 down to 1555 and still derives alpha from a compare against 0x40 -- so a
// path that only got the dithered arm right would be half broken in exactly the
// mode most games use.

#include "IPU/IPU_MultiISA.h"
#include "GS/MultiISA.h"

#include "gtest/gtest.h"

#include <array>
#include <cstring>
#include <random>

namespace
{
	constexpr int kDim = 16;

	// A value no real conversion can produce: every output pixel has its top bit
	// set only when alpha matched, so an all-ones block would require every pixel
	// to be white and opaque at once. Seeding with it turns "this path skipped a
	// pixel" into a mismatch instead of a silent pass on stale memory.
	void PoisonOutput(macroblock_rgb16& rgb16)
	{
		std::memset(&rgb16, 0xFF, sizeof(rgb16));
	}

	// Returns the index of the first differing pixel, or -1 when the two agree.
	int FirstMismatch(const macroblock_rgb16& a, const macroblock_rgb16& b)
	{
		u16 a_words[kDim * kDim];
		u16 b_words[kDim * kDim];
		std::memcpy(a_words, &a, sizeof(a_words));
		std::memcpy(b_words, &b, sizeof(b_words));

		for (int i = 0; i < kDim * kDim; i++)
		{
			if (a_words[i] != b_words[i])
				return i;
		}
		return -1;
	}

	// Runs the host's selected path and the reference over the same input and
	// reports the first pixel they disagree on, with enough context to place it in
	// the dither matrix.
	void ExpectMatchesReference(const macroblock_rgb32& rgb32, int dte, const char* what)
	{
		macroblock_rgb16 got;
		macroblock_rgb16 want;
		PoisonOutput(got);
		PoisonOutput(want);

		MULTI_ISA_SELECT(ipu_dither)(rgb32, got, dte);
		MULTI_ISA_SELECT(ipu_dither_reference)(rgb32, want, dte);

		const int bad = FirstMismatch(got, want);
		if (bad < 0)
			return;

		const int row = bad / kDim;
		const int col = bad % kDim;
		const auto& src = rgb32.c[row][col];

		u16 got_words[kDim * kDim];
		u16 want_words[kDim * kDim];
		std::memcpy(got_words, &got, sizeof(got_words));
		std::memcpy(want_words, &want, sizeof(want_words));

		ADD_FAILURE() << what << ": dte=" << dte << " first mismatch at row " << row
					  << " col " << col << " (dither cell [" << (row & 3) << "][" << (col & 3) << "])"
					  << "\n  source rgba = " << int(src.r) << ", " << int(src.g) << ", "
					  << int(src.b) << ", " << int(src.a)
					  << "\n  got  = 0x" << std::hex << got_words[bad]
					  << "\n  want = 0x" << want_words[bad] << std::dec;
	}
} // namespace

// Every byte value, through every dither cell, on the colour channels. Holding the
// three channels equal is what makes this a clean sweep of the dither arithmetic;
// channel independence is the randomised test's job.
TEST(IPUDither, ColourSweepMatchesReference)
{
	for (int v = 0; v <= 255; v++)
	{
		macroblock_rgb32 rgb32;
		for (int i = 0; i < kDim; i++)
		{
			for (int j = 0; j < kDim; j++)
			{
				rgb32.c[i][j].r = static_cast<u8>(v);
				rgb32.c[i][j].g = static_cast<u8>(v);
				rgb32.c[i][j].b = static_cast<u8>(v);
				// Alternate the two alpha outcomes so neither is ever untested.
				rgb32.c[i][j].a = ((i + j) & 1) ? 0x40 : 0x00;
			}
		}

		ExpectMatchesReference(rgb32, 1, "colour sweep");
		ExpectMatchesReference(rgb32, 0, "colour sweep");
	}
}

// Alpha is a compare against 0x40, not a range, so the interesting inputs are the
// neighbours of that value as much as the extremes. Sweeping the whole byte covers
// both without having to guess.
TEST(IPUDither, AlphaSweepMatchesReference)
{
	for (int v = 0; v <= 255; v++)
	{
		macroblock_rgb32 rgb32;
		for (int i = 0; i < kDim; i++)
		{
			for (int j = 0; j < kDim; j++)
			{
				// Distinct per channel, so an alpha bug cannot hide behind a
				// colour that happens to match.
				rgb32.c[i][j].r = static_cast<u8>(j * 16);
				rgb32.c[i][j].g = static_cast<u8>(i * 16);
				rgb32.c[i][j].b = static_cast<u8>((i + j) * 8);
				rgb32.c[i][j].a = static_cast<u8>(v);
			}
		}

		ExpectMatchesReference(rgb32, 1, "alpha sweep");
		ExpectMatchesReference(rgb32, 0, "alpha sweep");
	}
}

// The saturating arithmetic only shows its edges where a cell pushes a value past
// a limit, and the cells reach +3 and -4. Pinning the exact boundary values means a
// path that clamps with the wrong operation fails here rather than on one unlucky
// random block.
TEST(IPUDither, SaturationEdgesMatchReference)
{
	static constexpr std::array<u8, 10> kEdges = {0, 1, 2, 3, 4, 251, 252, 253, 254, 255};

	for (const u8 lo : kEdges)
	{
		for (const u8 hi : kEdges)
		{
			macroblock_rgb32 rgb32;
			for (int i = 0; i < kDim; i++)
			{
				for (int j = 0; j < kDim; j++)
				{
					rgb32.c[i][j].r = lo;
					rgb32.c[i][j].g = hi;
					rgb32.c[i][j].b = static_cast<u8>((j & 1) ? lo : hi);
					rgb32.c[i][j].a = ((i + j) & 1) ? 0x40 : 0x3F;
				}
			}

			ExpectMatchesReference(rgb32, 1, "saturation edges");
			ExpectMatchesReference(rgb32, 0, "saturation edges");
		}
	}
}

// The sweeps all hold something constant across the block. This one holds nothing
// constant, which is what catches a path that reads the right bytes into the wrong
// channel -- a deinterleave that transposes r and b survives every test above.
TEST(IPUDither, RandomMacroblocksMatchReference)
{
	std::mt19937 rng(20260814u); // fixed seed: a failing case must be reproducible
	std::uniform_int_distribution<int> byte(0, 255);

	for (int iter = 0; iter < 256; iter++)
	{
		macroblock_rgb32 rgb32;
		for (int i = 0; i < kDim; i++)
		{
			for (int j = 0; j < kDim; j++)
			{
				rgb32.c[i][j].r = static_cast<u8>(byte(rng));
				rgb32.c[i][j].g = static_cast<u8>(byte(rng));
				rgb32.c[i][j].b = static_cast<u8>(byte(rng));
				// Bias alpha towards the one value the compare cares about,
				// otherwise it is almost never hit at random.
				rgb32.c[i][j].a = (byte(rng) < 128) ? 0x40 : static_cast<u8>(byte(rng));
			}
		}

		ExpectMatchesReference(rgb32, iter & 1, "random macroblock");
	}
}
