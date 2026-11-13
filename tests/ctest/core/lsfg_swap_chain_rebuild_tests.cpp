// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GSLsfg::SwapChainNeedsRebuild decides when turning frame generation on makes GSDeviceVK rebuild
// the swap chain. A rebuild is a full GPU wait and a blank frame, so the rule is pinned from both
// sides: the chains that must be rebuilt, and the ones that must not be, since an answer that stays
// "rebuild" after a rebuild would repeat it on every frame.

#include "GS/Renderers/Vulkan/GSLsfg.h"

#include <gtest/gtest.h>

namespace
{
	constexpr bool kSized = true;
	constexpr bool kNotSized = false;
} // namespace

// MAILBOX shows only the newest image queued for a refresh, so the real frame presented after each
// generated one replaces it. Vsync off resolves to MAILBOX on Android, spare image or not.
TEST(LsfgSwapChainRebuild, MailboxIsRebuilt)
{
	EXPECT_TRUE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_MAILBOX_KHR, 1, kNotSized));
	EXPECT_TRUE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_MAILBOX_KHR, 0, kNotSized));
	EXPECT_TRUE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_MAILBOX_KHR, 2, kSized));
}

TEST(LsfgSwapChainRebuild, ImmediateIsRebuilt)
{
	EXPECT_TRUE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_IMMEDIATE_KHR, 1, kNotSized));
}

TEST(LsfgSwapChainRebuild, FifoWithASpareImageIsKept)
{
	EXPECT_FALSE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_FIFO_KHR, 1, kSized));
	EXPECT_FALSE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_FIFO_KHR, 1, kNotSized));
}

// SelectPresentMode turns the FIFO that frame generation forces into FIFO_RELAXED where the surface
// has it. It queues like FIFO, so generated frames are displayed, and a rebuild would only pick it
// again: requiring strict FIFO here would rebuild on every frame.
TEST(LsfgSwapChainRebuild, FifoRelaxedWithASpareImageIsKept)
{
	EXPECT_FALSE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_FIFO_RELAXED_KHR, 1, kSized));
}

// Built before frame generation was on, so it never asked for the extra image.
TEST(LsfgSwapChainRebuild, FifoWithNoSpareImageBuiltWithoutFrameGenerationIsRebuilt)
{
	EXPECT_TRUE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_FIFO_KHR, 0, kNotSized));
	EXPECT_TRUE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_FIFO_RELAXED_KHR, 0, kNotSized));
}

// It asked and the driver gave none: a rebuild would get the same answer. Initialize reports
// "no display headroom" instead.
TEST(LsfgSwapChainRebuild, FifoWithNoSpareImageBuiltForFrameGenerationIsKept)
{
	EXPECT_FALSE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_FIFO_KHR, 0, kSized));
	EXPECT_FALSE(GSLsfg::SwapChainNeedsRebuild(VK_PRESENT_MODE_FIFO_RELAXED_KHR, 0, kSized));
}
