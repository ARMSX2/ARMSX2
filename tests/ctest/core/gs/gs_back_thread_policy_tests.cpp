// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the GS multi-threading setting resolves to. See GSBackThreadPolicy.h.

#include "GS/Renderers/Common/GSBackThreadPolicy.h"

#include <gtest/gtest.h>

namespace
{
	GSBackThreadInputs Requesting(bool requested)
	{
		GSBackThreadInputs in;
		in.requested = requested;
		return in;
	}
} // namespace

TEST(GSBackThreadPolicy, OnIsGivenWhereItCanPipeline)
{
	const GSBackThreadDecision d = GSDecideBackThread(Requesting(true));
	EXPECT_TRUE(d.on);
	EXPECT_EQ(d.reason, GSBackThreadReason::Requested);
}

// The titles whose database entry forces Unsynchronized downloads (Jak 3, Black, Mortal Kombat
// Shaolin Monks) get off.
TEST(GSBackThreadPolicy, OffUnderUnsynchronizedDownloads)
{
	GSBackThreadInputs in = Requesting(true);
	in.download_mode = GSHardwareDownloadMode::Unsynchronized;
	const GSBackThreadDecision d = GSDecideBackThread(in);
	EXPECT_FALSE(d.on);
	EXPECT_EQ(d.reason, GSBackThreadReason::LiveMemoryDownloads);
}

TEST(GSBackThreadPolicy, OffOnANonVulkanHardwareRenderer)
{
	GSBackThreadInputs in = Requesting(true);
	in.vulkan = false;
	const GSBackThreadDecision d = GSDecideBackThread(in);
	EXPECT_FALSE(d.on);
	EXPECT_EQ(d.reason, GSBackThreadReason::BackendCannotQueue);
}

// The software renderer queues on any API and never reads live memory from the EE thread.
TEST(GSBackThreadPolicy, TheSoftwareRendererKeepsTheSplit)
{
	GSBackThreadInputs in = Requesting(true);
	in.hardware_renderer = false;
	in.vulkan = false;
	in.download_mode = GSHardwareDownloadMode::Unsynchronized;
	EXPECT_TRUE(GSDecideBackThread(in).on);
}

// Asynchronous downloads read a shadow under a mutex, so queue depth cannot change what the EE
// sees and the split stays on. Every other download mode reads on the GS thread.
TEST(GSBackThreadPolicy, OtherDownloadModesKeepTheSplit)
{
	for (const GSHardwareDownloadMode mode :
		{GSHardwareDownloadMode::Enabled, GSHardwareDownloadMode::EnabledForceFull, GSHardwareDownloadMode::NoReadbacks,
			GSHardwareDownloadMode::Disabled, GSHardwareDownloadMode::Asynchronous})
	{
		GSBackThreadInputs in = Requesting(true);
		in.download_mode = mode;
		EXPECT_TRUE(GSDecideBackThread(in).on) << static_cast<int>(mode);
	}
}

// Off is given as asked, whatever else holds.
TEST(GSBackThreadPolicy, OffIsGivenAsAsked)
{
	for (const bool hw : {false, true})
	for (const bool vk : {false, true})
	for (const GSHardwareDownloadMode dl : {GSHardwareDownloadMode::Enabled, GSHardwareDownloadMode::Unsynchronized,
			 GSHardwareDownloadMode::Asynchronous})
	{
		GSBackThreadInputs in;
		in.requested = false;
		in.hardware_renderer = hw;
		in.vulkan = vk;
		in.download_mode = dl;
		const GSBackThreadDecision d = GSDecideBackThread(in);
		EXPECT_FALSE(d.on);
		EXPECT_EQ(d.reason, GSBackThreadReason::Requested);
	}
}
