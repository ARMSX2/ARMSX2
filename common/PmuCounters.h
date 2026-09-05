// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>

// Thin wrapper around perf_event_open(2) for measuring hardware perf counters
// on the calling thread, user-mode only. Designed for codegen-iteration loops
// that need cycles/instructions/branch-misses/L1D-refills per iteration with
// sub-microsecond overhead.
//
// Linux-only — on other platforms Open() returns false and Read() returns
// zeros, so callers should treat "open failed" as "skip the bench" rather
// than as an error.
//
// Counter access can be restricted by /proc/sys/kernel/perf_event_paranoid.
// Level 2, the common default, still permits an unprivileged process to count
// its OWN user-mode execution, which is all this does — so it normally needs no
// configuration. Levels 1 and 0 additionally allow kernel and raw tracepoint
// measurement, which this does not ask for.
//
// ⚠️ EVERY ARM TARGET WE SHIP TO IS big.LITTLE, AND THAT HAS TEETH HERE.
// A heterogeneous machine exposes one PMU PER CLUSTER, not one per machine —
// Apple Silicon under Asahi has `apple_avalanche_pmu` (P) and
// `apple_blizzard_pmu` (E); Snapdragon and Dimensity are the same shape. An
// event is opened against ONE PMU and counts NOTHING while the thread runs on a
// core belonging to another. Asking for a bare PERF_TYPE_HARDWARE event on such
// a host therefore yields a counter that silently reads zero, or worse, reads a
// fraction of the truth if the scheduler moves the thread mid-measurement.
//
// Open() handles this by installing the whole group ONCE PER CORE PMU and
// summing at Read(). Where no per-cluster PMUs are advertised (plain x86, or a
// kernel too old to expose them) it falls back to a single anonymous group,
// which is the correct behaviour there.
//
// ⚠️ A SUM ACROSS CLUSTERS IS NOT A MEANINGFUL CYCLE COUNT. Measured here, the
// same loop costs 180M cycles on an E-core and 30M on a P-core; a thread that
// migrated mid-measurement produces 210M, a number describing no processor that
// exists. Instruction counts add up fine, cycles do not. Use ActivePmuCount()
// to reject a migrated measurement, and pin the thread for any A/B.
//
// Per-counter availability also varies by host. Apple Silicon under Asahi
// exposes cycles / instructions / branch-misses / branch-instructions but
// not the generic L1D cache events. Open() tolerates per-counter failures:
// the leader (CPU_CYCLES) is required, followers that fail are simply not
// installed and will read as 0. Use IsAvailable(c) to distinguish "0 events"
// from "this PMU doesn't have that counter." A follower that opens on some
// clusters and not others counts as unavailable — a partial sum is worse than
// no number, because it looks like a number.

namespace PmuCounters
{
	// Counters we read in a single perf_event_open group. Order matches the
	// order of values returned by Read() and Measure().
	enum Counter : int
	{
		CpuCycles = 0,         // PERF_COUNT_HW_CPU_CYCLES — required (group leader)
		InstructionsRetired,   // PERF_COUNT_HW_INSTRUCTIONS
		BranchMisses,          // PERF_COUNT_HW_BRANCH_MISSES
		BranchInstructions,    // PERF_COUNT_HW_BRANCH_INSTRUCTIONS
		L1dCacheRefills,       // L1D read miss — unavailable on Apple PMU
		Count
	};

	using Values = std::array<u64, Counter::Count>;

	class Group
	{
	public:
		Group();
		~Group();

		Group(const Group&) = delete;
		Group& operator=(const Group&) = delete;

		// Opens the hardware perf counters as one group on the calling thread,
		// user-mode-only, initially disabled. Returns false on syscall
		// failure (most commonly EACCES from perf_event_paranoid).
		bool Open();

		// Reset accumulated counts to zero.
		void Reset();

		// Toggle counting. Group leader uses PERF_FORMAT_GROUP; a single
		// PERF_EVENT_IOC_ENABLE/DISABLE ioctl with PERF_IOC_FLAG_GROUP flips
		// every counter in the group atomically.
		void Enable();
		void Disable();

		// Read current counter values. Safe to call any time after Open();
		// returns zeros if the group isn't open. Read is allowed while
		// counters are running — the values are an instantaneous snapshot.
		Values Read() const;

		// Convenience: enable, run callable, disable, read. The Reset()
		// before the run means the returned values are deltas, not absolute.
		template <typename F>
		Values Measure(F&& fn)
		{
			Reset();
			Enable();
			fn();
			Disable();
			return Read();
		}

		// True iff Open() succeeded and the group hasn't been closed.
		bool IsOpen() const { return m_pmu_count > 0; }

		// How many core PMUs the group is installed on. 1 on a homogeneous
		// machine; one per cluster on big.LITTLE.
		int PmuCount() const { return m_pmu_count; }

		// How many of those PMUs actually counted anything since the last
		// Reset() — i.e. how many clusters the measured thread ran on.
		//
		// ⚠️ Anything but 1 means the thread migrated and the summed cycle count
		// mixes clusters that do not retire work at the same rate. Callers doing
		// codegen A/Bs should treat >1 as "discard and re-run pinned", not as a
		// number to report. Reads the counters, so it is not free.
		int ActivePmuCount() const;

		// True iff this counter was actually installed by Open(). Followers
		// that returned ENOENT (PMU doesn't support the event) read as 0
		// from Read(); IsAvailable lets callers distinguish "really 0" from
		// "not measured."
		bool IsAvailable(Counter c) const;

		// True iff the machine advertises per-cluster core PMUs but the kernel
		// refused to open against them, so the group fell back to one anonymous
		// counter set (PmuCount() == 1). On a big.LITTLE host that means the
		// "sum across clusters" warning above applies with no way to see the
		// migration: a thread that moves counts nothing for the time it spends
		// off the PMU the kernel picked. Kernels below 6.6 land here.
		bool MultiPmuUnsupported() const { return m_multi_pmu_unsupported; }

		// Upper bound on core PMUs we will install on. Two clusters is the
		// common shape; Snapdragon's prime/gold/silver split makes three. Eight
		// is slack, not a prediction.
		static constexpr int MaxPmus = 8;

	private:
		void Close();

		// One complete counter group, installed against one core PMU. On a
		// homogeneous host there is exactly one of these and it is opened
		// without naming a PMU at all.
		struct PmuGroup
		{
			// fds[0] is the group leader; fds[1..] are followers attached via
			// the leader's group_fd. -1 marks slots that failed to open.
			int leader_fd = -1;
			int follower_fds[Counter::Count - 1] = {};

			// Slot i in the PERF_FORMAT_GROUP read buffer that corresponds to
			// Counter i. -1 means the counter wasn't installed.
			int read_slot[Counter::Count] = {};
			int installed_count = 0;
		};

		// Reads one PMU's counters, indexed by Counter. False means the group
		// could not be read at all, which is NOT the same as reading zeros: a
		// cluster the thread never ran on legitimately counts nothing.
		static bool ReadOneGroup(const PmuGroup& g, Values& out);

		PmuGroup m_pmu[MaxPmus];
		int m_pmu_count = 0;
		bool m_multi_pmu_unsupported = false;

		// Availability is the AND across every installed PMU: a counter present
		// on one cluster and missing on another would otherwise report a sum
		// that silently omits whichever cluster lacked it.
		bool m_available[Counter::Count] = {};
	};

	// Static text label for a counter — useful for printing.
	const char* Name(Counter c);
} // namespace PmuCounters
