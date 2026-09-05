// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/PmuCounters.h"

#ifdef __linux__

#include <asm/unistd.h>
#include <dirent.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Lets a legacy PERF_TYPE_HARDWARE event name a specific PMU on a
// heterogeneous machine, by putting the PMU's sysfs type in the high 32 bits of
// config. Defined here as well so the build does not depend on the age of the
// installed uapi headers -- but note that having the macro is not the same as
// the kernel honouring it, and an older kernel does NOT ignore the high bits:
//
//   >= 6.6        arm_pmu advertises PERF_PMU_CAP_EXTENDED_HW_TYPE; the
//                 selector works and each cluster gets its own group.
//   5.13 - 6.5    the uapi macro exists but arm_pmu does not advertise the cap,
//                 so perf_init_event finds no PMU for the extended type and
//                 perf_event_open returns ENOENT.
//   < 5.13        the kernel knows nothing of the selector, so the high bits
//                 leave config >= PERF_COUNT_HW_MAX and armpmu_map_hw_event
//                 returns EINVAL.
//
// So Open() opens the per-PMU plan first and falls back to a single anonymous
// group on either of those two errnos. Without the fallback every Android
// vendor kernel (5.10/5.15/6.1) and every ROCKNIX image below 6.6 would report
// no counters at all.
#ifndef PERF_PMU_TYPE_SHIFT
#define PERF_PMU_TYPE_SHIFT 32
#endif

namespace PmuCounters
{
	namespace
	{
		long PerfEventOpen(struct perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags)
		{
			return ::syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
		}

		void FillAttrFor(Counter c, struct perf_event_attr& attr)
		{
			std::memset(&attr, 0, sizeof(attr));
			attr.size = sizeof(attr);
			attr.disabled = 1;
			attr.exclude_kernel = 1;
			attr.exclude_hv = 1;
			// PERF_FORMAT_GROUP returns all counters in the group from a
			// single read on the leader fd. PERF_FORMAT_ID is not requested
			// because counters are read in fixed order — the index is the
			// identity.
			attr.read_format = PERF_FORMAT_GROUP;

			switch (c)
			{
				case CpuCycles:
					attr.type = PERF_TYPE_HARDWARE;
					attr.config = PERF_COUNT_HW_CPU_CYCLES;
					break;
				case InstructionsRetired:
					attr.type = PERF_TYPE_HARDWARE;
					attr.config = PERF_COUNT_HW_INSTRUCTIONS;
					break;
				case BranchMisses:
					attr.type = PERF_TYPE_HARDWARE;
					attr.config = PERF_COUNT_HW_BRANCH_MISSES;
					break;
				case BranchInstructions:
					attr.type = PERF_TYPE_HARDWARE;
					attr.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
					break;
				case L1dCacheRefills:
					attr.type = PERF_TYPE_HW_CACHE;
					attr.config = (PERF_COUNT_HW_CACHE_L1D)
						| (PERF_COUNT_HW_CACHE_OP_READ << 8)
						| (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
					break;
				default:
					break;
			}
		}

		// The core PMUs advertised by the kernel, as sysfs `type` numbers.
		//
		// A core PMU is identified by having a `cpus` file naming the processors
		// it covers -- that is exactly what distinguishes a per-cluster CPU PMU
		// from the software/tracepoint/uncore pseudo-devices sitting beside it.
		// A homogeneous host advertises none (x86's "cpu" has no `cpus` file),
		// and the caller then opens one anonymous group, which is right there.
		//
		// Sorted, so the group order is stable run to run and a per-PMU number
		// means the same thing across runs.
		int CollectCorePmuTypes(u32* out, int max_out)
		{
			DIR* dir = ::opendir("/sys/bus/event_source/devices");
			if (!dir)
				return 0;

			int count = 0;
			while (const struct dirent* ent = ::readdir(dir))
			{
				if (count >= max_out)
					break;
				if (ent->d_name[0] == '.')
					continue;

				char path[512];
				std::snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/cpus", ent->d_name);
				if (::access(path, R_OK) != 0)
					continue;

				std::snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type", ent->d_name);
				std::FILE* fp = std::fopen(path, "re");
				if (!fp)
					continue;
				unsigned long type = 0;
				const bool ok = (std::fscanf(fp, "%lu", &type) == 1);
				std::fclose(fp);
				if (ok)
					out[count++] = static_cast<u32>(type);
			}
			::closedir(dir);
			std::sort(out, out + count);
			return count;
		}
	} // namespace

	// Reads one PMU's counters. False means the group could not be read at all,
	// which is NOT the same as reading zeros -- a cluster the thread never ran on
	// legitimately counts nothing, and that is the ordinary case on big.LITTLE.
	bool Group::ReadOneGroup(const PmuGroup& g, Values& out)
	{
		out = Values{};
		if (g.leader_fd < 0)
			return false;

		{
			// PERF_FORMAT_GROUP read layout. Assumes read_format is GROUP-ONLY — no
			// PERF_FORMAT_TOTAL_TIME_ENABLED/RUNNING/ID/LOST. Those flags insert extra
			// u64s around values[] and would shift every counter; if attr.read_format
			// ever gains one, this struct and the offset math below must change too.
			//   u64 nr;            // number of counters actually installed
			//   u64 values[nr];    // each installed counter's accumulated count
			struct ReadBuf
			{
				u64 nr;
				u64 values[Counter::Count];
			} buf{};
			const size_t expected_bytes = sizeof(u64) * (1 + g.installed_count);
			const ssize_t n = ::read(g.leader_fd, &buf, expected_bytes);
			if (n < static_cast<ssize_t>(expected_bytes))
				return false;
			if (static_cast<int>(buf.nr) != g.installed_count)
				return false;
			for (int i = 0; i < Counter::Count; ++i)
			{
				if (g.read_slot[i] >= 0)
					out[i] = buf.values[g.read_slot[i]];
			}
			return true;
		}
	}

	Group::Group()
	{
		// Initialize every slot to the "not installed" sentinel (-1), matching
		// the documented invariant. Without this the arrays zero-initialize, so
		// a Group destroyed without a successful Open() would (a) report
		// IsAvailable()==true for slot 0 (0 >= 0), and (b) have Close() call
		// ::close(0) on the still-zero follower fds — closing stdin. Open()
		// repeats this reset before installing counters.
		for (PmuGroup& g : m_pmu)
		{
			for (int& fd : g.follower_fds)
				fd = -1;
			for (int& slot : g.read_slot)
				slot = -1;
		}
	}

	Group::~Group()
	{
		Close();
	}

	bool Group::Open()
	{
		Close();

		for (PmuGroup& g : m_pmu)
		{
			g = PmuGroup{};
			for (int& fd : g.follower_fds)
				fd = -1;
			for (int& slot : g.read_slot)
				slot = -1;
		}
		m_pmu_count = 0;
		m_multi_pmu_unsupported = false;
		for (bool& a : m_available)
			a = false;

		// One group per core PMU. On big.LITTLE an event opened against one
		// cluster's PMU counts nothing while the thread runs on another, so a
		// single group is not a counter that is merely imprecise -- it is a
		// counter that reads zero for the entire measurement whenever the thread
		// lands on the wrong side, which looks exactly like "this machine has no
		// counters".
		u32 pmu_types[MaxPmus];
		int pmu_count = CollectCorePmuTypes(pmu_types, MaxPmus);

		bool available[Counter::Count];

		// Two attempts at most: the per-PMU plan, then the anonymous one. A
		// kernel that does not implement the PMU selector rejects the leader
		// outright rather than ignoring the high bits (see the header comment on
		// PERF_PMU_TYPE_SHIFT), and every arm64 host advertises a core PMU, so
		// without this the whole rig loses its counters on any kernel below 6.6.
		for (;;)
		{
			// A homogeneous host advertises no per-cluster PMUs; open a single
			// group naming no PMU, which is what every non-hybrid machine wants.
			const int groups_to_open = (pmu_count > 0) ? pmu_count : 1;

			for (int i = 0; i < Counter::Count; ++i)
				available[i] = true;

			int leader_errno = 0;

			for (int p = 0; p < groups_to_open; ++p)
			{
				// The high 32 bits of a legacy PERF_TYPE_HARDWARE config select the
				// PMU; zero means "let the kernel choose", the homogeneous case.
				const u64 pmu_bits = (pmu_count > 0) ? (static_cast<u64>(pmu_types[p]) << PERF_PMU_TYPE_SHIFT) : 0;
				PmuGroup& g = m_pmu[m_pmu_count];

				struct perf_event_attr leader_attr;
				FillAttrFor(CpuCycles, leader_attr);
				leader_attr.config |= pmu_bits;
				// Leader: pid=0 (calling thread), cpu=-1 (any), group_fd=-1.
				g.leader_fd = static_cast<int>(PerfEventOpen(&leader_attr, 0, -1, -1, 0));
				if (g.leader_fd < 0)
				{
					if (leader_errno == 0)
						leader_errno = errno;
					g.leader_fd = -1;
					// One cluster refusing the leader is not fatal while another
					// accepts it -- but every counter then has a hole, so the thread
					// running there would report zero. Treat it as a failed open only
					// if NO group came up; otherwise carry on and let ActivePmuCount()
					// expose the gap.
					continue;
				}
				g.read_slot[CpuCycles] = g.installed_count++;

				// Followers — failures are tolerated (e.g. Apple PMU under Asahi
				// returns ENOENT for the L1D cache event). Read() returns 0 for
				// any counter whose read_slot is -1.
				for (int i = 1; i < Counter::Count; ++i)
				{
					struct perf_event_attr attr;
					FillAttrFor(static_cast<Counter>(i), attr);
					if (attr.type == PERF_TYPE_HARDWARE)
						attr.config |= pmu_bits;
					const int fd = static_cast<int>(PerfEventOpen(&attr, 0, -1, g.leader_fd, 0));
					if (fd < 0)
					{
						available[i] = false;
						continue;
					}
					g.follower_fds[i - 1] = fd;
					g.read_slot[i] = g.installed_count++;
				}

				m_pmu_count++;
			}

			if (m_pmu_count > 0)
				break;

			// Nothing came up. If the plan named PMUs and the kernel answered
			// with the two errnos that mean "I do not know that selector"
			// (ENOENT from perf_init_event on 5.13-6.5, EINVAL from
			// armpmu_map_hw_event before that, where the high bits push config
			// past PERF_COUNT_HW_MAX), retry with no selector at all. Any other
			// errno -- EACCES, EPERM, ENOSYS -- would fail the same way twice.
			if (pmu_count > 0 && (leader_errno == ENOENT || leader_errno == EINVAL))
			{
				pmu_count = 0;
				m_multi_pmu_unsupported = true;
				continue;
			}

			return false;
		}

		// PERF_TYPE_HW_CACHE carries no PMU selector, so on a heterogeneous host
		// it lands on whichever PMU the kernel picks and cannot be summed
		// honestly. Where it opened on only some clusters `available` already
		// says no; where it opened on all of them it is still one cluster's
		// event repeated, so leave that judgement to the AND above rather than
		// inventing a special case here.
		for (int i = 0; i < Counter::Count; ++i)
			m_available[i] = available[i];

		Reset();
		return true;
	}

	bool Group::IsAvailable(Counter c) const
	{
		if (c < 0 || c >= Counter::Count)
			return false;
		return m_pmu_count > 0 && m_available[c];
	}

	void Group::Close()
	{
		for (int p = 0; p < m_pmu_count; ++p)
		{
			PmuGroup& g = m_pmu[p];
			for (int& fd : g.follower_fds)
			{
				if (fd >= 0)
				{
					::close(fd);
					fd = -1;
				}
			}
			if (g.leader_fd >= 0)
			{
				::close(g.leader_fd);
				g.leader_fd = -1;
			}
		}
		m_pmu_count = 0;
	}

	void Group::Reset()
	{
		for (int p = 0; p < m_pmu_count; ++p)
			::ioctl(m_pmu[p].leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
	}

	void Group::Enable()
	{
		for (int p = 0; p < m_pmu_count; ++p)
			::ioctl(m_pmu[p].leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
	}

	void Group::Disable()
	{
		for (int p = 0; p < m_pmu_count; ++p)
			::ioctl(m_pmu[p].leader_fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
	}

	Values Group::Read() const
	{
		Values out{};
		for (int p = 0; p < m_pmu_count; ++p)
		{
			Values one;
			if (!ReadOneGroup(m_pmu[p], one))
				continue;
			for (int i = 0; i < Counter::Count; ++i)
				out[i] += one[i];
		}
		// A counter missing from any cluster would otherwise hand back a partial
		// sum wearing the costume of a whole one.
		for (int i = 0; i < Counter::Count; ++i)
		{
			if (!m_available[i])
				out[i] = 0;
		}
		return out;
	}

	int Group::ActivePmuCount() const
	{
		int active = 0;
		for (int p = 0; p < m_pmu_count; ++p)
		{
			Values one;
			if (ReadOneGroup(m_pmu[p], one) && one[CpuCycles] != 0)
				active++;
		}
		return active;
	}

	const char* Name(Counter c)
	{
		switch (c)
		{
			case CpuCycles:            return "cycles";
			case InstructionsRetired:  return "instructions";
			case BranchMisses:         return "branch-misses";
			case BranchInstructions:   return "branches";
			case L1dCacheRefills:      return "L1-dcache-load-misses";
			default:                   return "?";
		}
	}
} // namespace PmuCounters

#else  // !__linux__

namespace PmuCounters
{
	Group::Group() = default;
	Group::~Group() = default;
	bool Group::Open() { return false; }
	bool Group::IsAvailable(Counter) const { return false; }
	int Group::ActivePmuCount() const { return 0; }
	bool Group::ReadOneGroup(const PmuGroup&, Values&) { return false; }
	void Group::Close() {}
	void Group::Reset() {}
	void Group::Enable() {}
	void Group::Disable() {}
	Values Group::Read() const { return {}; }
	const char* Name(Counter c)
	{
		switch (c)
		{
			case CpuCycles:            return "cycles";
			case InstructionsRetired:  return "instructions";
			case BranchMisses:         return "branch-misses";
			case BranchInstructions:   return "branches";
			case L1dCacheRefills:      return "L1-dcache-load-misses";
			default:                   return "?";
		}
	}
} // namespace PmuCounters

#endif
