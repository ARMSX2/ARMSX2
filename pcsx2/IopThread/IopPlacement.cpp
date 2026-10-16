// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "IopThread/IopPlacement.h"

#include <algorithm>
#include <bit>

u64 IopThread::PerformanceMask(const std::vector<CpuCapacity>& cpus)
{
	u32 peak = 0;
	for (const CpuCapacity& cpu : cpus)
		peak = std::max(peak, cpu.capacity);
	if (peak == 0)
		return 0;

	const u32 threshold = static_cast<u32>(static_cast<double>(peak) * kPerformanceClusterRatio);

	u64 mask = 0;
	for (const CpuCapacity& cpu : cpus)
	{
		if (!cpu.first_thread || cpu.capacity < threshold || cpu.processor_id >= 64)
			continue;
		mask |= static_cast<u64>(1) << cpu.processor_id;
	}
	return mask;
}

std::vector<u32> IopThread::OrderProcessors(const std::vector<CpuCapacity>& cpus)
{
	std::vector<CpuCapacity> cores;
	for (const CpuCapacity& cpu : cpus)
	{
		if (cpu.first_thread)
			cores.push_back(cpu);
	}
	std::stable_sort(cores.begin(), cores.end(), [](const CpuCapacity& lhs, const CpuCapacity& rhs) {
		if (lhs.capacity != rhs.capacity)
			return lhs.capacity > rhs.capacity;
		if (lhs.frequency_hz != rhs.frequency_hz)
			return lhs.frequency_hz > rhs.frequency_hz;
		return lhs.processor_id < rhs.processor_id;
	});

	std::vector<u32> order;
	order.reserve(cores.size());
	for (const CpuCapacity& core : cores)
		order.push_back(core.processor_id);
	return order;
}

IopThread::HostCpuInfo IopThread::MakeHostInfo(bool is_arm64, u32 total_cores, u64 performance_mask)
{
	HostCpuInfo host;
	host.is_arm64 = is_arm64;
	host.total_cores = total_cores;
	host.performance_cores = static_cast<u32>(std::popcount(performance_mask));
	return host;
}

IopThread::TierPlan IopThread::PlanTier(u64 performance_mask, bool mtvu, bool iop_thread)
{
	TierPlan plan;
	plan.threads_to_place = 2 + (mtvu ? 1 : 0) + (iop_thread ? 1 : 0);
	const u32 tier = static_cast<u32>(std::popcount(performance_mask));
	plan.tier_fits = performance_mask != 0 && tier >= plan.threads_to_place;
	plan.back_thread_fits = tier >= plan.threads_to_place + 1;
	return plan;
}

IopThread::RankPlan IopThread::PlanRanks(int affinity_mode, bool mtvu, bool iop_thread, size_t list_size)
{
	// Rows are {ee_rank, vu_rank, gs_rank}; the row order matches the UI order.
	//   1 EE>VU>GS   2 EE>GS>VU   3 VU>EE>GS   4 VU>GS>EE   5 GS>EE>VU   6 GS>VU>EE
	static constexpr int kRanks[6][3] = {
		{0, 1, 2},
		{0, 2, 1},
		{1, 0, 2},
		{2, 0, 1},
		{1, 2, 0},
		{2, 1, 0},
	};
	const int row = std::clamp(affinity_mode, 1, 6) - 1;

	RankPlan plan;
	plan.ee = kRanks[row][0];
	plan.vu = kRanks[row][1];
	plan.gs = kRanks[row][2];

	// With MTVU off there is no VU thread to place, so GS inherits VU's slot when VU ranked higher;
	// otherwise the faster core sits idle.
	if (!mtvu && plan.vu < plan.gs)
		plan.gs = plan.vu;

	if (iop_thread)
	{
		int rank = 0;
		while (rank == plan.ee || rank == plan.gs || (mtvu && rank == plan.vu))
			rank++;
		if (static_cast<size_t>(rank) < list_size)
			plan.iop = rank;
	}
	return plan;
}

IopThread::OrderedPlan IopThread::PlanOrdered(size_t list_size, bool mtvu, bool iop_thread)
{
	OrderedPlan plan;
	plan.ee = 0;
	plan.vu = 1;
	plan.gs = mtvu ? 2 : 1; // without MTVU the GS takes the VU thread's slot

	const u32 used = mtvu ? 3 : 2;
	plan.iop = (iop_thread && list_size > used) ? static_cast<int>(used) : -1;
	plan.first_software_thread = used + (plan.iop >= 0 ? 1 : 0);
	return plan;
}
