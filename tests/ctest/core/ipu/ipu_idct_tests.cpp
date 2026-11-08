// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The NEON IDCT against the scalar one, bit for bit, on decoder-like blocks and the full 16-bit range.

#include "IPU/IPU_MultiISA.h"
#include "GS/MultiISA.h"

#include "gtest/gtest.h"

#include <cstring>
#include <random>

namespace
{
	int FirstMismatch(const s16* a, const s16* b)
	{
		for (int i = 0; i < 64; i++)
			if (a[i] != b[i])
				return i;
		return -1;
	}

	void Expect(const s16 (&in)[64], const char* what, int round)
	{
		alignas(16) s16 got[64];
		alignas(16) s16 want[64];
		std::memcpy(got, in, sizeof(got));
		std::memcpy(want, in, sizeof(want));
		MULTI_ISA_SELECT(ipu_idct_block)(got);
		MULTI_ISA_SELECT(ipu_idct_block_reference)(want);
		const int bad = FirstMismatch(got, want);
		ASSERT_LT(bad, 0) << what << " round " << round << ": first mismatch at " << bad << " (row " << bad / 8 << ")"
						  << " got " << got[bad] << " want " << want[bad];
	}

	TEST(IpuIdct, MatchesTheScalarTransformOnDecoderSizedBlocks)
	{
		std::mt19937 rng(1);
		std::uniform_int_distribution<int> coef(-2048, 2047);
		for (int round = 0; round < 20000; round++)
		{
			s16 in[64] = {};
			// Mostly sparse, as dequantized blocks are; some dense.
			const int nonzero = (round % 4 == 0) ? 64 : 1 + static_cast<int>(rng() % 12);
			for (int k = 0; k < nonzero; k++)
				in[(round % 4 == 0) ? k : rng() % 64] = static_cast<s16>(coef(rng));
			Expect(in, "decoder-sized", round);
		}
	}

	TEST(IpuIdct, MatchesTheScalarTransformOverTheWhole16BitRange)
	{
		std::mt19937 rng(2);
		for (int round = 0; round < 20000; round++)
		{
			s16 in[64];
			for (s16& c : in)
				c = static_cast<s16>(rng());
			Expect(in, "full-range", round);
		}
	}

	TEST(IpuIdct, MatchesTheScalarTransformOnDcOnlyRowsAndBlocks)
	{
		std::mt19937 rng(3);
		for (int round = 0; round < 4000; round++)
		{
			s16 in[64] = {};
			for (int row = 0; row < 8; row++)
				if (rng() & 1)
					in[row * 8] = static_cast<s16>(rng());
			Expect(in, "dc-only rows", round);
		}
		const s16 extremes[] = {0, 1, -1, 4, -4, 255, -256, 2047, -2048, 32767, -32768};
		for (const s16 dc : extremes)
		{
			s16 in[64] = {};
			in[0] = dc;
			Expect(in, "dc-only block", dc);
		}
	}
} // namespace
