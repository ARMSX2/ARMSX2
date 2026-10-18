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

// ---------------------------------------------------------------------------------------------- placement
//
// Where the back thread may run once the front thread (MTGS) has its processor: the rest of the front's cluster in
// the performance tier, then the rest of the tier, each first without the processors pinned to other busy emu
// threads, then with them, then everything else.

namespace
{
	constexpr u64 Core(int n) { return static_cast<u64>(1) << n; }
}

// The MQ65: four A73 cores (4-7) and four A53 (0-3). EE on 4, MTVU on 5, the front on 6.
TEST(GSBackThreadPlacement, TakesAFreeBigCoreWhenThereIsOne)
{
	const u64 front = Core(6);
	const u64 occupied = Core(4) | Core(5) | front;
	const u64 tier = 0xf0;
	const GSBackThreadPlacement p = GSPlaceBackThread(tier & ~front, tier & ~front, 0xff & ~front, occupied);
	EXPECT_EQ(p.mask, Core(7));
	EXPECT_FALSE(p.shares_busy_core);
}

// With the IOP thread on core 7 the four big cores are all taken: "GS back thread may run on 0xb0" in the log,
// which are cores 4, 5 and 7, each already running a busy thread. Measured on the device: -19% speed.
TEST(GSBackThreadPlacement, SaysSoWhenEveryBigCoreIsTaken)
{
	const u64 front = Core(6);
	const u64 occupied = Core(4) | Core(5) | Core(7) | front;
	const u64 tier = 0xf0;
	const GSBackThreadPlacement p = GSPlaceBackThread(tier & ~front, tier & ~front, 0xff & ~front, occupied);
	EXPECT_EQ(p.mask, 0xb0u);
	EXPECT_TRUE(p.shares_busy_core);
}

// The rest of the tier comes before the rest of the cluster only when the cluster is full.
TEST(GSBackThreadPlacement, PrefersTheFrontsClusterThenTheRestOfTheTier)
{
	const u64 front = Core(4);
	const u64 occupied = Core(4) | Core(5);
	// Two clusters in the tier: 4-5 (the front's) and 6-7.
	const GSBackThreadPlacement p = GSPlaceBackThread(Core(5), Core(5) | Core(6) | Core(7), 0xff & ~front, occupied);
	EXPECT_EQ(p.mask, Core(6) | Core(7)) << "the cluster's only other core is busy, the other cluster is free";
	EXPECT_FALSE(p.shares_busy_core);
}

TEST(GSBackThreadPlacement, UnpinnedMeansNothingIsShared)
{
	const GSBackThreadPlacement p = GSPlaceBackThread(0, 0, 0, 0);
	EXPECT_EQ(p.mask, 0u);
	EXPECT_FALSE(p.shares_busy_core);
}

// A free slow core does not make a busy big core free: the order is the placement's, and it takes the big one.
TEST(GSBackThreadPlacement, ABusyBigCoreIsStillSharedWhileASlowCoreIsFree)
{
	const u64 front = Core(6);
	const u64 occupied = Core(4) | Core(5) | Core(7) | front;
	const GSBackThreadPlacement p = GSPlaceBackThread(0xb0, 0xb0, 0x0f | 0xb0, occupied);
	EXPECT_EQ(p.mask, 0xb0u);
	EXPECT_TRUE(p.shares_busy_core);
}
