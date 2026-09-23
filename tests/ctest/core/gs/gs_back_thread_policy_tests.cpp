// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What the GS back-thread setting resolves to. See GSBackThreadPolicy.h.

#include "GS/Renderers/Common/GSBackThreadPolicy.h"

#include <gtest/gtest.h>

namespace
{
	GSBackThreadInputs Requesting(GSBackThreadMode requested)
	{
		GSBackThreadInputs in;
		in.requested = requested;
		return in;
	}
} // namespace

TEST(GSBackThreadPolicy, PipelinedIsGivenWhereItCanPipeline)
{
	const GSBackThreadDecision d = GSDecideBackThreadMode(Requesting(GSBackThreadMode::Pipelined));
	EXPECT_EQ(d.mode, GSBackThreadMode::Pipelined);
	EXPECT_EQ(d.reason, GSBackThreadReason::Requested);
}

// The titles whose database entry forces Unsynchronized downloads (Jak 3, Black, Mortal Kombat
// Shaolin Monks) used to get lockstep from a pipelined request. They get Off.
TEST(GSBackThreadPolicy, PipelinedIsOffUnderUnsynchronizedDownloads)
{
	GSBackThreadInputs in = Requesting(GSBackThreadMode::Pipelined);
	in.download_mode = GSHardwareDownloadMode::Unsynchronized;
	const GSBackThreadDecision d = GSDecideBackThreadMode(in);
	EXPECT_EQ(d.mode, GSBackThreadMode::Off);
	EXPECT_EQ(d.reason, GSBackThreadReason::LiveMemoryDownloads);
}

TEST(GSBackThreadPolicy, PipelinedIsOffOnANonVulkanHardwareRenderer)
{
	GSBackThreadInputs in = Requesting(GSBackThreadMode::Pipelined);
	in.vulkan = false;
	const GSBackThreadDecision d = GSDecideBackThreadMode(in);
	EXPECT_EQ(d.mode, GSBackThreadMode::Off);
	EXPECT_EQ(d.reason, GSBackThreadReason::BackendCannotQueue);
}

// The software renderer queues on any API and never reads live memory from the EE thread.
TEST(GSBackThreadPolicy, TheSoftwareRendererKeepsTheSplit)
{
	GSBackThreadInputs in = Requesting(GSBackThreadMode::Pipelined);
	in.hardware_renderer = false;
	in.vulkan = false;
	in.download_mode = GSHardwareDownloadMode::Unsynchronized;
	EXPECT_EQ(GSDecideBackThreadMode(in).mode, GSBackThreadMode::Pipelined);
}

// Asynchronous downloads read a shadow under a mutex, so queue depth cannot change what the EE
// sees and the split stays on. Every other download mode reads on the GS thread.
TEST(GSBackThreadPolicy, OtherDownloadModesKeepTheSplit)
{
	for (const GSHardwareDownloadMode mode :
		{GSHardwareDownloadMode::Enabled, GSHardwareDownloadMode::EnabledForceFull, GSHardwareDownloadMode::NoReadbacks,
			GSHardwareDownloadMode::Disabled, GSHardwareDownloadMode::Asynchronous})
	{
		GSBackThreadInputs in = Requesting(GSBackThreadMode::Pipelined);
		in.download_mode = mode;
		EXPECT_EQ(GSDecideBackThreadMode(in).mode, GSBackThreadMode::Pipelined) << static_cast<int>(mode);
	}
}

// Off and the debugging rungs are given exactly as asked, whatever the download mode.
TEST(GSBackThreadPolicy, OtherModesAreGivenAsAsked)
{
	for (const GSBackThreadMode mode : {GSBackThreadMode::Off, GSBackThreadMode::InlineRecords, GSBackThreadMode::Lockstep})
	{
		GSBackThreadInputs in = Requesting(mode);
		in.download_mode = GSHardwareDownloadMode::Unsynchronized;
		in.vulkan = false;
		const GSBackThreadDecision d = GSDecideBackThreadMode(in);
		EXPECT_EQ(d.mode, mode);
		EXPECT_EQ(d.reason, GSBackThreadReason::Requested);
	}
}

// No input produces Lockstep unless Lockstep was the request.
TEST(GSBackThreadPolicy, NothingButALockstepRequestResolvesToLockstep)
{
	for (const GSBackThreadMode requested : {GSBackThreadMode::Off, GSBackThreadMode::InlineRecords, GSBackThreadMode::Pipelined})
	for (const bool hw : {false, true})
	for (const bool vk : {false, true})
	for (const GSHardwareDownloadMode dl : {GSHardwareDownloadMode::Enabled, GSHardwareDownloadMode::Unsynchronized,
			 GSHardwareDownloadMode::Asynchronous})
	{
		GSBackThreadInputs in;
		in.requested = requested;
		in.hardware_renderer = hw;
		in.vulkan = vk;
		in.download_mode = dl;
		EXPECT_NE(GSDecideBackThreadMode(in).mode, GSBackThreadMode::Lockstep);
	}
}
