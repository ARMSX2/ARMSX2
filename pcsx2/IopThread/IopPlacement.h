// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Where the IOP thread runs, as pure functions of the host's processors and the settings, kept
// apart from the OS calls so a test can describe a device. VMManager::SetEmuThreadAffinities pins
// the EE, GS and VU1 threads; with the IOP thread running concurrently it is one more busy thread
// and takes a place in the same plans.
//
// The same file holds the one rule shared with ResolveMode: which processors are the performance
// tier. ResolveMode needs the tier to be readable, the Android tier mode confines the emulation
// threads to it, and a concurrent IOP thread is kept out of a tier that is too narrow for the
// threads, so all of them read the same mask.

#pragma once

#include "IopThread/IopThreadMode.h"

#include "common/Pcsx2Types.h"

#include <cstddef>
#include <vector>

namespace IopThread
{
	struct CpuCapacity
	{
		u32 processor_id = 0; // the OS's number for it (Linux: linux_id); the mask bit is this number
		u32 capacity = 0; // cpu_capacity, else the maximum frequency, else cpuinfo's frequency: only ratios matter
		bool first_thread = true; // false for the second hardware thread of a core, which is never pinned to
		u64 frequency_hz = 0; // what cpuinfo reports, 0 when it cannot read it (the ROCKNIX and Android kernels leave it at 0)
	};

	/// A processor is in the tier when its capacity is at least this fraction of the fastest one's.
	/// 0.75 keeps a 4x A715 at 855 beside an X3 at 1024 (0.83), and drops an A510 at 280 (0.27).
	/// Mid tiers on 1+2+2+3 parts sit around 0.80 to 0.90 and are kept on purpose.
	inline constexpr double kPerformanceClusterRatio = 0.75;

	/// Every processor within kPerformanceClusterRatio of the fastest one. Deliberately not "the
	/// cluster owning the fastest core": on any 1+3+4 flagship the prime core is a cluster of one,
	/// and confining the emulation threads to a single core is worse than not confining them. The
	/// prime and the big cores form one mask, which is what is meant by "performance cores".
	///
	/// 0 when no processor has a reading (the cores cannot be told apart): the caller must not
	/// confine anything then, and ResolveMode takes it as "no spare big core".
	u64 PerformanceMask(const std::vector<CpuCapacity>& cpus);

	/// What ResolveMode needs to know about a host, from the topology: the performance-core count is
	/// the number of processors in the mask.
	HostCpuInfo MakeHostInfo(bool is_arm64, u32 total_cores, u64 performance_mask);

	/// The processor numbers in the order pinned threads take them: fastest first, one entry per core (the second hardware
	/// thread of a core is left out). The key is the capacity, then the frequency cpuinfo reports, then the processor number,
	/// so equal cores keep their number order.
	///
	/// The capacity comes first because it is the one reading every target has. On the SD865 and the MQ65 under ROCKNIX cpuinfo
	/// sees one cluster of eight cores and no frequency, so a list ordered by frequency alone comes out 0, 1, 2 ... and the
	/// EE thread is pinned to cpu 0, a little core, with the GS and the VU1 thread on the other little ones.
	std::vector<u32> OrderProcessors(const std::vector<CpuCapacity>& cpus);

	/// Android's Performance Cores mode: the EE, GS and, with MTVU, VU1 threads are confined to the performance
	/// tier and the scheduler places them within it. The IOP thread is not counted: it runs outside the tier
	/// (SlowCoresMask). A host with no slower core has every processor in the tier, and ResolveMode only gives it
	/// a concurrent IOP thread when there are two more cores than threads, so the thread still has room there.
	struct TierPlan
	{
		u32 threads_to_place = 0; // EE and GS, plus the VU1 thread with MTVU
		bool tier_fits = false; // the tier has a processor for each of them; otherwise nothing is confined
		bool back_thread_fits = false; // and one more for the GS back thread, which otherwise stays unpinned
	};
	TierPlan PlanTier(u64 performance_mask, bool mtvu);

	/// Whether the Linux ordered thread pin (EnableThreadPinning) may be applied: false only when the performance tier can
	/// be read and cannot hold the EE, GS and, with MTVU, VU1 threads. The pin hands out processors fastest first, so on
	/// a tier of two it puts the GS thread on a slow core, and measured on the MQ65 with two fast cores it lost 30 to 45%
	/// speed in every game. The IOP thread is not counted: it goes outside the tier (SlowCoresMask). True when the
	/// tier cannot be read (mask 0), which leaves a host that has no capacity source as it was.
	bool OrderedPinAllowed(u64 performance_mask, bool mtvu);

	/// Whether the game database's GS back-thread split fits on a host: the EE, VU1, GS front and GS back threads each
	/// want a performance core, and a concurrent IOP thread wants a fifth. It is busy 40 to 99% of a core, and with it
	/// running and only four big cores the split measured slower than a single GS thread in most games tried, even with
	/// the prime core rule. That measurement had the IOP thread on the fast cores; it runs on the slow ones now and this
	/// rule has not been measured again. There also has to be a slower core left to keep the rest off the tier: a host whose
	/// every processor is in the tier (`performance_cores >= processors`) has none.
	///
	/// `iop_thread_threaded` is whether the IOP thread will actually run concurrently on this host, which is what
	/// IopThread::ResolveMode says (Threaded), not whether the setting is on: on a host that can never give Threaded the
	/// setting costs the split nothing.
	bool GsSplitFits(u32 performance_cores, u32 processors, bool iop_thread_threaded);

	/// The processors a concurrent IOP thread is confined to: every processor outside the performance tier, in every
	/// placement mode, whether or not the tier could have held it. 0, meaning the caller's placement stands, when the
	/// IOP thread takes turns with the EE (Lockstep), when the tier cannot be read, or when there is no processor
	/// outside it.
	///
	/// The thread spins while it waits for the EE, so the scheduler counts it as busy. Floating, it spent 41% of its
	/// cycles on the big cores of the RG 55G1 (two Cortex-A78, six Cortex-A55) and pushed the EE, GS and VU1 threads onto
	/// the little ones. Confined with those three threads to the four big cores of a Dimensity 7300, it left none spare,
	/// and emulation lost 25% while the app's UI threads drew the on-screen controls.
	u64 SlowCoresMask(u64 performance_mask, u64 all_mask, bool concurrent);

	/// Android's ranked modes (1 to 6): each thread gets one processor from the list ordered fastest
	/// first. ee, vu and gs are ranks; the caller clamps them to the list. The IOP thread
	/// takes the first rank none of the others uses. -1 when there is no IOP thread to place or the
	/// list has no rank left for it: it is then left alone, never put on another thread's processor.
	struct RankPlan
	{
		int ee = 0;
		int vu = 0;
		int gs = 0;
		int iop = -1;
	};
	RankPlan PlanRanks(int affinity_mode, bool mtvu, bool iop_thread, size_t list_size);

	/// The desktop pin: indices into the processor list ordered fastest first. EE, then the VU1
	/// thread, then GS; the IOP thread takes the next slot, and the software renderer's threads start
	/// after it. -1 when the list has no slot left for it. The caller has already checked that the
	/// list holds the slots for EE, GS and, with MTVU, the VU1 thread.
	struct OrderedPlan
	{
		u32 ee = 0;
		u32 vu = 1;
		u32 gs = 1;
		int iop = -1;
		u32 first_software_thread = 2;
	};
	OrderedPlan PlanOrdered(size_t list_size, bool mtvu, bool iop_thread);

	/// A processor is "the prime core" when it is the only one at the top capacity and the rest of the tier is at
	/// least this far below it. The SD865 (1024 against 871) and the T820 (1024 against 869) qualify; a Dimensity
	/// 8300 (an X-class core a few percent over three more of its kind) and a part whose top cores are equal do not.
	inline constexpr double kPrimeCoreRatio = 1.10;

	/// The prime core and the rest of the performance tier. Both 0 when the tier has no single core that clearly
	/// outruns the others, or when it holds fewer than three processors (a prime core plus one other leaves nothing to
	/// share).
	struct PrimeSplit
	{
		u64 prime = 0; // exactly one bit
		u64 rest = 0; // the performance tier without the prime core
	};
	PrimeSplit PrimeCoreSplit(const std::vector<CpuCapacity>& cpus);

	/// The threads that can be the one the frame waits on, and so the one that wants the prime core.
	enum PrimeContender : int
	{
		kPrimeEE = 0,
		kPrimeVU,
		kPrimeGSFront,
		kPrimeGSBack,
		kPrimeContenderCount
	};

	/// What the last window measured: how much of a processor each thread used, in percent (100 = one whole core).
	struct PrimeLoad
	{
		bool present[kPrimeContenderCount] = {};
		double busy[kPrimeContenderCount] = {};
	};

	/// A challenger takes the prime core after being this far above the owner (percentage points) for this many
	/// windows in a row. The margin and the streak keep two threads that are about equally busy from trading the core
	/// back and forth; a move costs the thread its warm caches.
	inline constexpr double kPrimeMarginPercent = 10.0;
	inline constexpr u32 kPrimeWindows = 3;

	struct PrimeArbiter
	{
		int owner = -1; // the thread that has the prime core to itself; -1 until the first window names one
		int challenger = -1;
		u32 streak = 0;
	};

	/// Feeds one window into the arbiter and returns the owner. The first window names the busiest thread. A thread
	/// that is not running cannot own the core: the busiest thread that is running takes it at once.
	int PrimeArbiterUpdate(PrimeArbiter& arbiter, const PrimeLoad& load);
} // namespace IopThread
