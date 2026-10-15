// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Where the IOP thread runs, and whether it is offered at all.
//
// Three groups:
//   1. The performance tier: which processors count as the fast ones. The tables are the cpu_capacity
//      values read off the devices on 2026-10-15 (SD865, MQ65, Dimensity 8300, M2 Max), so the masks
//      in these tests are the masks those devices produce. The RK3562 and SD662 tables are described
//      from their deploy notes (a cluster of four equal A53s, and four A73 beside four A53), since
//      neither device could be read.
//   2. ResolveMode fed from those masks, so the core-count rule is checked against what a device's
//      topology yields and not against a count typed into the test.
//   3. The placement plans for the three ways the emulator pins its threads (Android's performance
//      tier, Android's ranked modes, and the frequency-ordered list the desktop path uses), and the
//      IOP thread's own handle.

#include <gtest/gtest.h>

#include "Config.h"
#include "IopThread/IopPlacement.h"
#include "IopThread/IopThreadMode.h"
#include "IopThreadTestSupport.h"
#include "VMManager.h"

#include <bit>
#include <vector>

namespace
{
	using IopThread::CpuCapacity;
	using IopThread::HostCpuInfo;
	using IopThread::MakeHostInfo;
	using IopThread::PerformanceMask;

	// n processors numbered from `first`, all with the same capacity.
	void Add(std::vector<CpuCapacity>& cpus, u32 first, u32 count, u32 capacity)
	{
		for (u32 i = 0; i < count; i++)
			cpus.push_back({first + i, capacity, true});
	}

	std::vector<CpuCapacity> Sd865()
	{
		std::vector<CpuCapacity> cpus;
		Add(cpus, 0, 4, 284); // A55
		Add(cpus, 4, 3, 871); // A77
		Add(cpus, 7, 1, 1024); // A77 prime
		return cpus;
	}

	std::vector<CpuCapacity> Mq65()
	{
		std::vector<CpuCapacity> cpus;
		Add(cpus, 0, 4, 611); // A53
		Add(cpus, 4, 4, 1024); // A73
		return cpus;
	}

	std::vector<CpuCapacity> Dimensity8300()
	{
		std::vector<CpuCapacity> cpus;
		Add(cpus, 0, 4, 221); // A510
		Add(cpus, 4, 3, 996); // A715
		Add(cpus, 7, 1, 1024); // A715 prime
		return cpus;
	}

	std::vector<CpuCapacity> M2Max()
	{
		std::vector<CpuCapacity> cpus;
		Add(cpus, 0, 4, 561); // efficiency
		Add(cpus, 4, 8, 1024); // performance
		return cpus;
	}

	std::vector<CpuCapacity> Rk3562()
	{
		std::vector<CpuCapacity> cpus;
		Add(cpus, 0, 4, 1024); // 4 x A53 in one cluster
		return cpus;
	}

	std::vector<CpuCapacity> Sd662()
	{
		std::vector<CpuCapacity> cpus;
		Add(cpus, 0, 4, 400); // A53
		Add(cpus, 4, 4, 1024); // A73
		return cpus;
	}

	Pcsx2Config Requested(bool lockstep, bool mtvu)
	{
		Pcsx2Config config;
		config.Speedhacks.iopThread = true;
		config.Speedhacks.iopThreadLockstep = lockstep;
		config.Speedhacks.vuThread = mtvu;
		return config;
	}

	IopThreadMode ResolveFor(const std::vector<CpuCapacity>& cpus, bool lockstep, bool mtvu)
	{
		const HostCpuInfo host = MakeHostInfo(true, static_cast<u32>(cpus.size()), PerformanceMask(cpus));
		return IopThread::ResolveMode(Requested(lockstep, mtvu), host, {});
	}
} // namespace

// ---------------------------------------------------------------- the performance tier

TEST(IopThreadPerformanceMask, TheSd865TierIsTheFourBigCores)
{
	// The three A77 are at 0.85 of the prime core, the A55 at 0.28.
	EXPECT_EQ(PerformanceMask(Sd865()), 0xF0u);
}

TEST(IopThreadPerformanceMask, TheMq65TierIsTheFourGoldCores)
{
	EXPECT_EQ(PerformanceMask(Mq65()), 0xF0u);
}

TEST(IopThreadPerformanceMask, TheDimensity8300TierIsTheFourBigCores)
{
	EXPECT_EQ(PerformanceMask(Dimensity8300()), 0xF0u);
}

TEST(IopThreadPerformanceMask, TheM2MaxTierIsTheEightPerformanceCores)
{
	EXPECT_EQ(PerformanceMask(M2Max()), 0xFF0u);
}

TEST(IopThreadPerformanceMask, AClusterOfIdenticalCoresIsAllTier)
{
	// The RK3562 has nothing slower to keep away from, so every core is in the tier.
	EXPECT_EQ(PerformanceMask(Rk3562()), 0xFu);
}

TEST(IopThreadPerformanceMask, TheCutIsAtThreeQuartersOfTheFastestCore)
{
	std::vector<CpuCapacity> cpus;
	Add(cpus, 0, 1, 1024);
	Add(cpus, 1, 1, 768); // exactly 0.75 x 1024: in
	Add(cpus, 2, 1, 767); // one below: out
	EXPECT_EQ(PerformanceMask(cpus), 0x3u);
}

TEST(IopThreadPerformanceMask, AMidTierBelowThePrimeCoreStaysInUntilItFallsUnderTheCut)
{
	// A715 at 855 beside an X3 at 1024 is 0.83; an A510 at 280 is 0.27 (the comment on the ratio).
	std::vector<CpuCapacity> cpus;
	Add(cpus, 0, 3, 280);
	Add(cpus, 3, 4, 855);
	Add(cpus, 7, 1, 1024);
	EXPECT_EQ(PerformanceMask(cpus), 0xF8u);
}

TEST(IopThreadPerformanceMask, TheBitIsTheProcessorNumberAndNotThePositionInTheList)
{
	// Linux ids need not be contiguous or in order: a core can be offline, or numbered by cluster.
	std::vector<CpuCapacity> cpus = {{9, 1024, true}, {2, 300, true}, {5, 1000, true}, {0, 290, true}};
	EXPECT_EQ(PerformanceMask(cpus), (1ull << 9) | (1ull << 5));
}

TEST(IopThreadPerformanceMask, TheSecondThreadOfACoreIsNeverCounted)
{
	std::vector<CpuCapacity> cpus = {{0, 1024, true}, {1, 1024, false}, {2, 1024, true}, {3, 1024, false}};
	EXPECT_EQ(PerformanceMask(cpus), 0x5u);
}

TEST(IopThreadPerformanceMask, CoresThatCannotBeReadGiveNoTier)
{
	// All-zero capacities mean "cannot tell the clusters apart": the caller must not confine anything.
	std::vector<CpuCapacity> cpus;
	Add(cpus, 0, 8, 0);
	EXPECT_EQ(PerformanceMask(cpus), 0u);
	EXPECT_EQ(PerformanceMask({}), 0u);
}

TEST(IopThreadPerformanceMask, AProcessorWithNoReadingIsNotInTheTier)
{
	std::vector<CpuCapacity> cpus;
	Add(cpus, 0, 3, 1024);
	Add(cpus, 3, 1, 0);
	EXPECT_EQ(PerformanceMask(cpus), 0x7u);
}

// ---------------------------------------------------------------- ResolveMode from a mask

TEST(IopThreadHostInfo, TheCountIsTheNumberOfBitsInTheMask)
{
	const HostCpuInfo host = MakeHostInfo(true, 12, 0xFF0);
	EXPECT_EQ(host.performance_cores, 8u);
	EXPECT_EQ(host.total_cores, 12u);
	EXPECT_TRUE(host.is_arm64);
	EXPECT_EQ(MakeHostInfo(true, 8, 0).performance_cores, 0u);
	EXPECT_FALSE(MakeHostInfo(false, 8, 0xF0).is_arm64);
}

TEST(IopThreadResolveFromMask, TheBigCoreDevicesAreOn)
{
	for (const bool mtvu : {true, false})
	{
		EXPECT_EQ(ResolveFor(Sd865(), false, mtvu), IopThreadMode::Threaded);
		EXPECT_EQ(ResolveFor(Mq65(), false, mtvu), IopThreadMode::Threaded);
		EXPECT_EQ(ResolveFor(Dimensity8300(), false, mtvu), IopThreadMode::Threaded);
		EXPECT_EQ(ResolveFor(M2Max(), false, mtvu), IopThreadMode::Threaded);
		EXPECT_EQ(ResolveFor(Sd662(), false, mtvu), IopThreadMode::Threaded);
	}
}

TEST(IopThreadResolveFromMask, FourEqualCoresLeaveNoSpareCoreAndResolveOff)
{
	// The RK3562: the tier is all four cores, which is enough for the threads and leaves none for the
	// OS, the GS back thread and the audio callback.
	EXPECT_EQ(PerformanceMask(Rk3562()), 0xFu);
	EXPECT_EQ(ResolveFor(Rk3562(), false, true), IopThreadMode::Off);
	EXPECT_EQ(ResolveFor(Rk3562(), false, false), IopThreadMode::Off);
}

TEST(IopThreadResolveFromMask, TheRk3562StillRunsLockstep)
{
	EXPECT_EQ(ResolveFor(Rk3562(), true, true), IopThreadMode::Lockstep);
}

TEST(IopThreadResolveFromMask, ADeviceWithTooFewFastCoresForTheThreadsIsOff)
{
	// Three fast cores and five slow ones: eight cores in total, but a tier of three cannot hold the EE,
	// the GS, the VU1 thread and the IOP.
	std::vector<CpuCapacity> cpus;
	Add(cpus, 0, 5, 300);
	Add(cpus, 5, 3, 1024);
	EXPECT_EQ(PerformanceMask(cpus), 0xE0u);
	EXPECT_EQ(ResolveFor(cpus, false, true), IopThreadMode::Off);
	// Without MTVU it needs three, and has three.
	EXPECT_EQ(ResolveFor(cpus, false, false), IopThreadMode::Threaded);
}

TEST(IopThreadResolveFromMask, ADeviceWhoseCoresCannotBeReadIsOffForThreadedAndOnForLockstep)
{
	std::vector<CpuCapacity> cpus;
	Add(cpus, 0, 8, 0);
	EXPECT_EQ(ResolveFor(cpus, false, true), IopThreadMode::Off);
	EXPECT_EQ(ResolveFor(cpus, true, true), IopThreadMode::Lockstep);
}

TEST(IopThreadResolveFromMask, TheRealHostIsDescribedByItsOwnMask)
{
	const HostCpuInfo host = IopThread::CurrentHostInfo();
	EXPECT_EQ(host.performance_cores, static_cast<u32>(std::popcount(VMManager::Internal::GetPerformanceCoreMask())));
	EXPECT_EQ(host.performance_cores, VMManager::Internal::GetPerformanceCoreCount());
}

// ---------------------------------------------------------------- the order pinned threads take processors in

namespace
{
	using IopThread::OrderProcessors;
	using ProcessorList = std::vector<u32>;

	// The capacity table of a device with the cpuinfo frequency of each core, which the ROCKNIX and Android kernels leave at 0.
	std::vector<CpuCapacity> WithFrequencies(std::vector<CpuCapacity> cpus, u64 hz)
	{
		for (CpuCapacity& cpu : cpus)
			cpu.frequency_hz = hz;
		return cpus;
	}
} // namespace

TEST(IopThreadProcessorOrder, TheSd865PutsThePrimeCoreFirstThenTheOtherThreeBigCores)
{
	// cpuinfo sees "8 cores, 8 processors, 1 clusters" and no frequencies on this device, so the order has to come from the
	// capacity table. Before it did, the EE thread was pinned to cpu 0, a Cortex-A55.
	EXPECT_EQ(OrderProcessors(Sd865()), (ProcessorList{7, 4, 5, 6, 0, 1, 2, 3}));
}

TEST(IopThreadProcessorOrder, TheMq65PutsTheGoldCoresFirst)
{
	EXPECT_EQ(OrderProcessors(Mq65()), (ProcessorList{4, 5, 6, 7, 0, 1, 2, 3}));
}

TEST(IopThreadProcessorOrder, TheDimensity8300PutsThePrimeCoreFirst)
{
	EXPECT_EQ(OrderProcessors(Dimensity8300()), (ProcessorList{7, 4, 5, 6, 0, 1, 2, 3}));
}

TEST(IopThreadProcessorOrder, TheM2MaxPutsThePerformanceCoresFirst)
{
	EXPECT_EQ(OrderProcessors(M2Max()), (ProcessorList{4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3}));
}

TEST(IopThreadProcessorOrder, WithNoCapacityTheFrequencyOrdersThemAsItAlwaysDid)
{
	// A desktop where cpuinfo knows the frequency and the kernel has no capacity table.
	std::vector<CpuCapacity> cpus = {{0, 0, true, 3000000000ull}, {1, 0, true, 4800000000ull}, {2, 0, true, 3000000000ull}, {3, 0, true, 4800000000ull}};
	EXPECT_EQ(OrderProcessors(cpus), (ProcessorList{1, 3, 0, 2}));
}

TEST(IopThreadProcessorOrder, ACapacityOutranksAFrequency)
{
	std::vector<CpuCapacity> cpus = {{0, 1024, true, 1000000000ull}, {1, 500, true, 3000000000ull}};
	EXPECT_EQ(OrderProcessors(cpus), (ProcessorList{0, 1}));
}

TEST(IopThreadProcessorOrder, EqualCoresKeepTheirNumberOrder)
{
	EXPECT_EQ(OrderProcessors(Rk3562()), (ProcessorList{0, 1, 2, 3}));
	EXPECT_EQ(OrderProcessors(WithFrequencies(Mq65(), 0)), (ProcessorList{4, 5, 6, 7, 0, 1, 2, 3}));
	// Nothing is known about any core: the numbers, in order, whatever order the list came in.
	std::vector<CpuCapacity> cpus = {{3, 0, true, 0}, {1, 0, true, 0}, {2, 0, true, 0}, {0, 0, true, 0}};
	EXPECT_EQ(OrderProcessors(cpus), (ProcessorList{0, 1, 2, 3}));
}

TEST(IopThreadProcessorOrder, TheSecondThreadOfACoreIsLeftOut)
{
	std::vector<CpuCapacity> cpus = {{0, 1024, true, 0}, {1, 1024, false, 0}, {2, 1024, true, 0}, {3, 1024, false, 0}};
	EXPECT_EQ(OrderProcessors(cpus), (ProcessorList{0, 2}));
}

TEST(IopThreadProcessorOrder, EveryPinnedThreadLandsInThePerformanceTierOnTheTargetDevices)
{
	// The point of the order: with the emulator's thread pinning on, the EE, GS, VU1 and IOP threads each get one
	// processor, and on a big.LITTLE handheld all of them are performance cores.
	const std::pair<const char*, std::vector<CpuCapacity>> devices[] = {
		{"SD865", Sd865()}, {"MQ65", Mq65()}, {"Dimensity 8300", Dimensity8300()}, {"M2 Max", M2Max()}};
	for (const auto& [name, cpus] : devices)
	{
		const u64 tier = PerformanceMask(cpus);
		const ProcessorList order = OrderProcessors(cpus);
		for (const bool mtvu : {true, false})
		{
			const auto plan = IopThread::PlanOrdered(order.size(), mtvu, true);
			ASSERT_GE(plan.iop, 0) << name;
			std::vector<u32> slots = {plan.ee, plan.gs, static_cast<u32>(plan.iop)};
			if (mtvu)
				slots.push_back(plan.vu);
			u64 used = 0;
			for (const u32 slot : slots)
			{
				const u64 bit = 1ull << order[slot];
				EXPECT_NE(tier & bit, 0u) << name << " mtvu " << mtvu << ": a pinned thread is on cpu " << order[slot] << ", not in the tier";
				EXPECT_EQ(used & bit, 0u) << name << ": two threads on cpu " << order[slot];
				used |= bit;
			}
		}
	}
}

// ---------------------------------------------------------------- Android: the performance tier

TEST(IopThreadPlaceTier, WithoutTheIopThreadTheCountsAreWhatTheyWere)
{
	// EE + GS, plus the VU1 thread. This is the arithmetic the mode used before the IOP thread existed.
	const auto with_vu = IopThread::PlanTier(0xF0, true, false);
	EXPECT_EQ(with_vu.threads_to_place, 3u);
	EXPECT_TRUE(with_vu.tier_fits);
	EXPECT_TRUE(with_vu.back_thread_fits);

	const auto without_vu = IopThread::PlanTier(0xF0, false, false);
	EXPECT_EQ(without_vu.threads_to_place, 2u);
	EXPECT_TRUE(without_vu.tier_fits);
	EXPECT_TRUE(without_vu.back_thread_fits);
}

TEST(IopThreadPlaceTier, TheIopThreadIsOneMoreThreadToPlace)
{
	EXPECT_EQ(IopThread::PlanTier(0xF0, true, true).threads_to_place, 4u);
	EXPECT_EQ(IopThread::PlanTier(0xF0, false, true).threads_to_place, 3u);
}

TEST(IopThreadPlaceTier, OnFourBigCoresWithMtvuTheGsBackThreadNoLongerFits)
{
	// The design accepts this (10.3): it was already so with MTVU on a three-big-core device.
	const auto plan = IopThread::PlanTier(0xF0, true, true);
	EXPECT_TRUE(plan.tier_fits);
	EXPECT_FALSE(plan.back_thread_fits);
}

TEST(IopThreadPlaceTier, OnFourBigCoresWithoutMtvuTheBackThreadStillFits)
{
	const auto plan = IopThread::PlanTier(0xF0, false, true);
	EXPECT_TRUE(plan.tier_fits);
	EXPECT_TRUE(plan.back_thread_fits);
}

TEST(IopThreadPlaceTier, ATierTooNarrowForTheThreadsIsNotUsed)
{
	// Confining four busy threads to three processors is worse than leaving them alone.
	EXPECT_FALSE(IopThread::PlanTier(0xE0, true, true).tier_fits);
	EXPECT_TRUE(IopThread::PlanTier(0xE0, true, false).tier_fits);
	EXPECT_FALSE(IopThread::PlanTier(0, false, false).tier_fits);
}

TEST(IopThreadPlaceTier, TheEightCoreM2KeepsEveryThreadAndTheBackThread)
{
	const auto plan = IopThread::PlanTier(0xFF0, true, true);
	EXPECT_TRUE(plan.tier_fits);
	EXPECT_TRUE(plan.back_thread_fits);
}

// ---------------------------------------------------------------- Android: the ranked modes

namespace
{
	// The old table, row by row: {ee, vu, gs} for modes 1 to 6.
	constexpr int kOldRanks[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {2, 0, 1}, {1, 2, 0}, {2, 1, 0}};
} // namespace

TEST(IopThreadPlaceRanks, WithoutTheIopThreadEveryModeKeepsItsOldRanks)
{
	for (int mode = 1; mode <= 6; mode++)
	{
		const auto plan = IopThread::PlanRanks(mode, true, false, 8);
		EXPECT_EQ(plan.ee, kOldRanks[mode - 1][0]) << "mode " << mode;
		EXPECT_EQ(plan.vu, kOldRanks[mode - 1][1]) << "mode " << mode;
		EXPECT_EQ(plan.gs, kOldRanks[mode - 1][2]) << "mode " << mode;
		EXPECT_EQ(plan.iop, -1) << "mode " << mode;
	}
}

TEST(IopThreadPlaceRanks, WithMtvuOffGsTakesTheVuRankWhenVuRankedHigher)
{
	// The existing rule: no VU thread to place, so GS inherits its slot when it ranked above GS.
	EXPECT_EQ(IopThread::PlanRanks(1, false, false, 8).gs, 1); // EE>VU>GS: VU is rank 1, GS was 2
	EXPECT_EQ(IopThread::PlanRanks(3, false, false, 8).gs, 0); // VU>EE>GS: VU is rank 0
	EXPECT_EQ(IopThread::PlanRanks(5, false, false, 8).gs, 0); // GS>EE>VU: GS already first
}

TEST(IopThreadPlaceRanks, TheIopThreadTakesTheFirstRankNoOtherThreadUses)
{
	for (int mode = 1; mode <= 6; mode++)
	{
		for (const bool mtvu : {true, false})
		{
			const auto plan = IopThread::PlanRanks(mode, mtvu, true, 8);
			ASSERT_GE(plan.iop, 0) << "mode " << mode;
			EXPECT_NE(plan.iop, plan.ee) << "mode " << mode << " mtvu " << mtvu;
			EXPECT_NE(plan.iop, plan.gs) << "mode " << mode << " mtvu " << mtvu;
			if (mtvu)
				EXPECT_NE(plan.iop, plan.vu) << "mode " << mode;
			// The first free rank: nothing below it is free.
			for (int r = 0; r < plan.iop; r++)
				EXPECT_TRUE(r == plan.ee || r == plan.gs || (mtvu && r == plan.vu)) << "mode " << mode << " rank " << r;
		}
	}
}

TEST(IopThreadPlaceRanks, TheIopThreadDoesNotMoveTheOtherThreads)
{
	for (int mode = 1; mode <= 6; mode++)
	{
		for (const bool mtvu : {true, false})
		{
			const auto off = IopThread::PlanRanks(mode, mtvu, false, 8);
			const auto on = IopThread::PlanRanks(mode, mtvu, true, 8);
			EXPECT_EQ(on.ee, off.ee);
			EXPECT_EQ(on.vu, off.vu);
			EXPECT_EQ(on.gs, off.gs);
		}
	}
}

TEST(IopThreadPlaceRanks, TheExpectedRankInTheModeThatPutsEeFirst)
{
	const auto with_vu = IopThread::PlanRanks(1, true, true, 8); // EE>VU>GS
	EXPECT_EQ(with_vu.iop, 3);
	const auto without_vu = IopThread::PlanRanks(1, false, true, 8);
	EXPECT_EQ(without_vu.iop, 2);
}

TEST(IopThreadPlaceRanks, AListTooShortToHoldTheIopThreadLeavesItUnplaced)
{
	EXPECT_EQ(IopThread::PlanRanks(1, true, true, 4).iop, 3); // exactly fits
	EXPECT_EQ(IopThread::PlanRanks(1, true, true, 3).iop, -1);
	EXPECT_EQ(IopThread::PlanRanks(1, false, true, 2).iop, -1);
}

// ---------------------------------------------------------------- desktop: the ordered list

TEST(IopThreadPlaceOrdered, WithoutTheIopThreadTheSlotsAreWhatTheyWere)
{
	const auto with_vu = IopThread::PlanOrdered(8, true, false);
	EXPECT_EQ(with_vu.ee, 0u);
	EXPECT_EQ(with_vu.vu, 1u);
	EXPECT_EQ(with_vu.gs, 2u);
	EXPECT_EQ(with_vu.iop, -1);
	EXPECT_EQ(with_vu.first_software_thread, 3u);

	const auto without_vu = IopThread::PlanOrdered(8, false, false);
	EXPECT_EQ(without_vu.ee, 0u);
	EXPECT_EQ(without_vu.gs, 1u);
	EXPECT_EQ(without_vu.iop, -1);
	EXPECT_EQ(without_vu.first_software_thread, 2u);
}

TEST(IopThreadPlaceOrdered, TheIopThreadTakesTheSlotAfterTheGsAndTheSoftwareThreadsStartAfterIt)
{
	const auto with_vu = IopThread::PlanOrdered(8, true, true);
	EXPECT_EQ(with_vu.ee, 0u);
	EXPECT_EQ(with_vu.vu, 1u);
	EXPECT_EQ(with_vu.gs, 2u);
	EXPECT_EQ(with_vu.iop, 3);
	EXPECT_EQ(with_vu.first_software_thread, 4u);

	const auto without_vu = IopThread::PlanOrdered(8, false, true);
	EXPECT_EQ(without_vu.ee, 0u);
	EXPECT_EQ(without_vu.gs, 1u);
	EXPECT_EQ(without_vu.iop, 2);
	EXPECT_EQ(without_vu.first_software_thread, 3u);
}

TEST(IopThreadPlaceOrdered, TheIopThreadNeverTakesAnotherThreadsProcessor)
{
	for (size_t size = 2; size <= 12; size++)
	{
		for (const bool mtvu : {true, false})
		{
			if (size < (mtvu ? 3u : 2u))
				continue;
			const auto plan = IopThread::PlanOrdered(size, mtvu, true);
			if (plan.iop < 0)
				continue;
			EXPECT_NE(static_cast<u32>(plan.iop), plan.ee);
			EXPECT_NE(static_cast<u32>(plan.iop), plan.gs);
			if (mtvu)
				EXPECT_NE(static_cast<u32>(plan.iop), plan.vu);
			EXPECT_LT(static_cast<size_t>(plan.iop), size);
		}
	}
}

TEST(IopThreadPlaceOrdered, AListWithNoSlotLeftForTheIopThreadLeavesItUnplaced)
{
	EXPECT_EQ(IopThread::PlanOrdered(4, true, true).iop, 3);
	EXPECT_EQ(IopThread::PlanOrdered(3, true, true).iop, -1);
	EXPECT_EQ(IopThread::PlanOrdered(3, false, true).iop, 2);
	EXPECT_EQ(IopThread::PlanOrdered(2, false, true).iop, -1);
	// An unplaced thread does not push the software renderer's threads down.
	EXPECT_EQ(IopThread::PlanOrdered(3, true, true).first_software_thread, 3u);
}

// ---------------------------------------------------------------- the thread's own handle

namespace
{
	// The CPU the test may pin to: one the process is allowed to run on.
	int AllowedCpu()
	{
		const u64 mask = Threading::ThreadHandle::GetForCallingThread().GetAffinity();
		return mask ? std::countr_zero(mask) : -1;
	}
} // namespace

TEST(IopThreadHandle, ARunningThreadHasAHandleThatCanBePlaced)
{
	iopt::SettingsGuard guard;
	EXPECT_FALSE(static_cast<bool>(IopThread::g_thread.GetThreadHandle()));

	iopt::StartLockstep();
	ASSERT_TRUE(IopThread::g_thread.IsRunning());
	const Threading::ThreadHandle& handle = IopThread::g_thread.GetThreadHandle();
	ASSERT_TRUE(static_cast<bool>(handle));

	const int cpu = AllowedCpu();
	if (cpu < 0)
		GTEST_SKIP() << "this platform cannot read thread affinities";
	// The thread was created by this one, so it began with this thread's mask.
	const u64 before = handle.GetAffinity();
	EXPECT_NE(before, 0u);

	ASSERT_TRUE(handle.SetAffinity(1ull << cpu));
	EXPECT_EQ(handle.GetAffinity(), 1ull << cpu);
	// Putting it back to "any" must not leave it on the one core.
	ASSERT_TRUE(handle.SetAffinity(before));
	EXPECT_EQ(handle.GetAffinity(), before);

	IopThread::StopThread();
	EXPECT_FALSE(static_cast<bool>(IopThread::g_thread.GetThreadHandle()));
}

TEST(IopThreadHandle, TheStartCountChangesWithEveryStart)
{
	iopt::SettingsGuard guard;
	const u32 never = IopThread::g_thread.StartCount();

	iopt::StartLockstep();
	const u32 first = IopThread::g_thread.StartCount();
	EXPECT_NE(first, never);

	IopThread::StopThread();
	iopt::StartLockstep();
	const u32 second = IopThread::g_thread.StartCount();
	EXPECT_NE(second, first);
	IopThread::StopThread();
}

TEST(IopThreadHandle, OnlyAThreadedThreadCountsWhenThreadsAreCounted)
{
	// Lockstep runs the two threads in strict turns, so it does not take a processor from anyone.
	iopt::SettingsGuard guard;
	EXPECT_FALSE(IopThread::PlacesThread());
	iopt::StartLockstep();
	EXPECT_FALSE(IopThread::PlacesThread());
	EXPECT_NE(IopThread::PlacementToken(), 0u); // but it exists, and was created on the caller's mask
	IopThread::StopThread();
	EXPECT_FALSE(IopThread::PlacesThread());
	EXPECT_EQ(IopThread::PlacementToken(), 0u);
}
