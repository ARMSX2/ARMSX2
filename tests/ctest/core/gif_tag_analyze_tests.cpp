// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Gif_Tag::analyzeTag against its own reference loop: regs[] unpacked from the NREG nibbles, and hasAD
// set only for an A+D nibble below NREG.

#include "Gif_Unit.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>

namespace
{
	struct Expected
	{
		u8 regs[16];
		bool hasAD;
	};

	Expected Reference(u64 regs64, u32 nreg)
	{
		Expected e = {};
		for (u32 i = 0; i < nreg; i++)
		{
			e.regs[i] = static_cast<u8>((regs64 >> (i * 4)) & 0xF);
			e.hasAD |= (e.regs[i] == GIF_REG_A_D);
		}
		return e;
	}

	// A PACKED tag with NLOOP 1, the given NREG field (0 is 16) and REGS.
	void Check(u64 regs64, u32 nreg_field)
	{
		alignas(16) u8 mem[16];
		const u64 lo = 1ULL | (static_cast<u64>(GIF_FLG_PACKED) << 58) | (static_cast<u64>(nreg_field & 0xF) << 60);
		std::memcpy(mem, &lo, 8);
		std::memcpy(mem + 8, &regs64, 8);

		Gif_Tag t;
		t.setTag(mem, true);
		const u32 nreg = nreg_field ? nreg_field : 16;
		ASSERT_EQ(t.nRegs, nreg);
		const Expected e = Reference(regs64, nreg);
		EXPECT_EQ(t.hasAD, e.hasAD) << std::hex << "REGS 0x" << regs64 << std::dec << " NREG " << nreg;
		for (u32 i = 0; i < nreg; i++)
			ASSERT_EQ(t.regs[i], e.regs[i]) << "register " << i << " REGS 0x" << std::hex << regs64;
	}

	TEST(GifTagAnalyze, MatchesTheReferenceForEveryNregOnRandomRegs)
	{
		std::mt19937_64 rng(2026);
		for (int round = 0; round < 4000; round++)
		{
			u64 regs = rng();
			// Plenty of A+D nibbles, not just the 1-in-16 of random data.
			if (round & 1)
				regs = (regs & 0xF0F0F0F0F0F0F0F0ULL) | 0x0E0E0E0E0E0E0E0EULL;
			for (u32 nreg = 0; nreg < 16; nreg++)
			{
				Check(regs, nreg);
				if (::testing::Test::HasFailure())
					return;
			}
		}
	}

	TEST(GifTagAnalyze, AnAdPastNregDoesNotCount)
	{
		for (u32 nreg = 1; nreg < 16; nreg++)
		{
			// Every register below NREG is 1, every one from NREG on is A+D.
			u64 regs = 0;
			for (u32 i = 0; i < 16; i++)
				regs |= static_cast<u64>(i < nreg ? 0x1 : GIF_REG_A_D) << (i * 4);
			Check(regs, nreg);
		}
		Check(0xEEEEEEEEEEEEEEEEULL, 0);
		Check(0x1111111111111111ULL, 0);
		Check(0xE000000000000000ULL, 0); // the 16th register only
	}
} // namespace
