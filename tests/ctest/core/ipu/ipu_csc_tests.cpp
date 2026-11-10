// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// IPU colour conversion after yuv2rgb(): alpha thresholds, then the sign flip, which used to skip the
// block and write past it.

#include "IPU/IPU_MultiISA.h"
#include "GS/MultiISA.h"

#include "gtest/gtest.h"

#include <cstring>

extern u16 g_ipu_thresh[2];

namespace
{
	// The block with a guard after it, the way the decoder lays rgb32 out before rgb16.
	struct GuardedBlock
	{
		macroblock_rgb32 rgb32;
		u8 guard[sizeof(macroblock_rgb32)];
	};

	constexpr u8 kGuard = 0xA5;

	class IpuCscAlphaAndSign : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_thresh[0] = g_ipu_thresh[0];
			m_thresh[1] = g_ipu_thresh[1];
			for (int i = 0; i < 256; i++)
			{
				auto& c = m_block.rgb32.c[i / 16][i % 16];
				// Dark pixels (all channels under 0x20), mid pixels (under 0x60) and bright ones.
				const u8 v = (i % 3 == 0) ? 0x10 : (i % 3 == 1) ? 0x50 : 0xC0;
				c.r = v;
				c.g = static_cast<u8>(v + 1);
				c.b = static_cast<u8>(v + 2);
				c.a = 0x80;
			}
			std::memset(m_block.guard, kGuard, sizeof(m_block.guard));
			std::memcpy(&m_before, &m_block.rgb32, sizeof(m_before));
		}

		void TearDown() override
		{
			g_ipu_thresh[0] = m_thresh[0];
			g_ipu_thresh[1] = m_thresh[1];
		}

		bool GuardIntact() const
		{
			for (u8 b : m_block.guard)
			{
				if (b != kGuard)
					return false;
			}
			return true;
		}

		u32 Before(int i) const
		{
			u32 v;
			std::memcpy(&v, &m_before.c[i / 16][i % 16], sizeof(v));
			return v;
		}

		u32 After(int i) const
		{
			u32 v;
			std::memcpy(&v, &m_block.rgb32.c[i / 16][i % 16], sizeof(v));
			return v;
		}

		GuardedBlock m_block;
		macroblock_rgb32 m_before;
		u16 m_thresh[2];
	};

	TEST_F(IpuCscAlphaAndSign, AThresholdAndSignedOutputStayInsideTheBlockAndFlipEveryPixel)
	{
		g_ipu_thresh[0] = 0x20;
		g_ipu_thresh[1] = 0x60;
		MULTI_ISA_SELECT(ipu_csc_alpha_and_sign)(m_block.rgb32, 1);

		EXPECT_TRUE(GuardIntact());
		for (int i = 0; i < 256; i++)
		{
			// Dark pixels go to 0 first, mid ones get alpha 0x40; then every pixel's RGB is flipped.
			u32 want = Before(i);
			if (i % 3 == 0)
				want = 0;
			else if (i % 3 == 1)
				want = (want & 0x00FFFFFFu) | 0x40000000u;
			want ^= 0x808080u;
			ASSERT_EQ(After(i), want) << "pixel " << i;
		}
	}

	TEST_F(IpuCscAlphaAndSign, TheUpperThresholdAloneWithSignedOutputFlipsEveryPixel)
	{
		g_ipu_thresh[0] = 0;
		g_ipu_thresh[1] = 0x60;
		MULTI_ISA_SELECT(ipu_csc_alpha_and_sign)(m_block.rgb32, 1);

		EXPECT_TRUE(GuardIntact());
		for (int i = 0; i < 256; i++)
		{
			u32 want = Before(i);
			if (i % 3 != 2)
				want = (want & 0x00FFFFFFu) | 0x40000000u;
			want ^= 0x808080u;
			ASSERT_EQ(After(i), want) << "pixel " << i;
		}
	}

	TEST_F(IpuCscAlphaAndSign, SignedOutputWithoutThresholdsOnlyFlips)
	{
		g_ipu_thresh[0] = 0;
		g_ipu_thresh[1] = 0;
		MULTI_ISA_SELECT(ipu_csc_alpha_and_sign)(m_block.rgb32, 1);

		EXPECT_TRUE(GuardIntact());
		for (int i = 0; i < 256; i++)
			ASSERT_EQ(After(i), Before(i) ^ 0x808080u) << "pixel " << i;
	}

	TEST_F(IpuCscAlphaAndSign, AThresholdWithUnsignedOutputLeavesRgbAlone)
	{
		g_ipu_thresh[0] = 0x20;
		g_ipu_thresh[1] = 0x60;
		MULTI_ISA_SELECT(ipu_csc_alpha_and_sign)(m_block.rgb32, 0);

		EXPECT_TRUE(GuardIntact());
		for (int i = 0; i < 256; i++)
		{
			u32 want = Before(i);
			if (i % 3 == 0)
				want = 0;
			else if (i % 3 == 1)
				want = (want & 0x00FFFFFFu) | 0x40000000u;
			ASSERT_EQ(After(i), want) << "pixel " << i;
		}
	}
} // namespace
