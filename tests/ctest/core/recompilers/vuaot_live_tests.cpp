// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// vu-aot live tier (EmuCore/CPU/Recompiler/EnableVUAot): the tier that runs
// inside the emulator's own dispatcher. Each test runs one program on VU1
// through the real dispatcher many times -- the harness's JIT-only re-run keeps
// the compiled blocks, so the run counter climbs the way a game's does -- and
// every run must leave exactly what microVU alone leaves, before, at and after
// the tier-up.

#if defined(_M_ARM64) || defined(__aarch64__)

#include "harness/ProgCachePackMirror.h"
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/microVU_ProgCache-arm64.h"
#include "arm64/vuaot/VuAotBackground.h"
#include "arm64/vuaot/VuAotLive.h"

#include "common/FileSystem.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sched.h>
#include <unistd.h>
#ifdef __linux__
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

// Test hook exported by pcsx2/arm64/microVU-arm64.cpp under PCSX2_RECOMPILER_TESTS (see
// mvu_clear_occupied_tests): prog.quickOcc, the bitmap mVUclear walks, agrees with quick[].
namespace mvu_test_hooks
{
	bool QuickOccupancyMatchesScan(int vu_index);
} // namespace mvu_test_hooks

namespace recompiler_tests
{

using namespace vu;

namespace
{
inline VuOp L(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp U(u32 upper) { return VuOp{0x8000033c, upper}; }
inline VuOp P(u32 lower, u32 upper) { return VuOp{lower, upper}; }

// The live tier on or off, tiering up after `threshold` dispatches. Leaving the
// scope puts the config back and resets VU1, so the next test starts clean.
struct LiveScope
{
	const bool savedOpt = EmuConfig.Cpu.Recompiler.EnableVUAot;
	const bool savedFlagHack = EmuConfig.Speedhacks.vuFlagHack;
	// `sync`: the deterministic mode: every background job is waited for and published where it was asked for, so a test's
	// sequence of compiles is the same from run to run.
	LiveScope(bool on, u32 threshold, bool flagHack, u32 jumpThreshold = vuaot::live::kJumpTierUpRuns, bool sync = true)
	{
		EmuConfig.Cpu.Recompiler.EnableVUAot = on;
		EmuConfig.Speedhacks.vuFlagHack = flagHack;
		// (a test starts with the tripwire armed, as a VM reset leaves it, and nothing broken)
		vuaot::live::ResetStickyTripwire();
		vuaot::live::SetTripwireMutation(0);
		vuaot::live::SetTierUpRuns(threshold);
		vuaot::live::SetJumpTierUpRuns(jumpThreshold);
		vuaot::live::SetSynchronous(sync);
	}
	~LiveScope()
	{
		// Whatever the tier did to VU1's quick slots (a root publication empties a program's, a slow path
		// puts one back), prog.quickOcc, the bitmap mVUclear walks instead of the array, still agrees.
		EXPECT_TRUE(mvu_test_hooks::QuickOccupancyMatchesScan(1)) << "VU1's quick slots and prog.quickOcc disagree";
		vuaot::live::SetWorkerGate(nullptr);
		vuaot::live::HoldWorker(false);
		vuaot::live::SetTripwireMutation(0);
		vuaot::live::ResetStickyTripwire();
		EmuConfig.Cpu.Recompiler.EnableVUAot = savedOpt;
		EmuConfig.Speedhacks.vuFlagHack = savedFlagHack;
		vuaot::live::SetTierUpRuns(vuaot::live::kTierUpRuns);
		vuaot::live::SetJumpTierUpRuns(vuaot::live::kJumpTierUpRuns);
		vuaot::live::SetSynchronous(false);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
	}
};

struct Result
{
	VuSnapshot snap;
	std::vector<u8> path1;
};

Result Take(const VuTestHarness& h) { return Result{h.JitSnapshot(), h.Path1PacketBytesJit()}; }

std::string Diff(const Result& a, const Result& b)
{
	struct F
	{
		const char* name;
		size_t off, size;
	};
#define FR(f) F{#f, offsetof(VURegs, f), sizeof(VURegs::f)}
	static const F kFields[] = {FR(VF), FR(VI), FR(ACC), FR(q), FR(p), FR(cycle), FR(flags), FR(start_pc), FR(ebit),
		FR(pending_q), FR(pending_p), FR(micro_macflags), FR(micro_clipflags), FR(micro_statusflags),
		FR(nextBlockCycles), FR(xgkickaddr), FR(xgkickdiff), FR(xgkicksizeremaining), FR(xgkicklastcycle),
		FR(xgkickcyclecount), FR(xgkickenable), FR(xgkickendpacket)};
#undef FR
	std::string out;
	const u8* x = reinterpret_cast<const u8*>(&a.snap.regs);
	const u8* y = reinterpret_cast<const u8*>(&b.snap.regs);
	for (const F& f : kFields)
		if (std::memcmp(x + f.off, y + f.off, f.size) != 0)
			out += std::string(f.name) + " differs\n";
	for (size_t w = 0; w < a.snap.mem_windows.size() && w < b.snap.mem_windows.size(); w++)
		if (a.snap.mem_windows[w].bytes != b.snap.mem_windows[w].bytes)
			out += "VU memory differs\n";
	if (a.path1 != b.path1)
		out += "GIF path 1 bytes differ\n";
	return out;
}

using Setup = std::function<void(VuTestHarness&)>;

void Prepare(VuTestHarness& h, const std::vector<VuOp>& prog, const Setup& setup, u32 budget)
{
	if (setup)
		setup(h);
	h.TrackMemWindow(0, 0x4000);
	h.LoadProgram(prog);
	h.ResumeUntilTerminated();
	if (budget)
		h.SetCycleBudget(budget);
}

// A counted loop: FMACs, a flag reader, a divide whose Q crosses the link, a
// store; long enough to take several cycle-budget breaks under a small budget.
std::vector<VuOp> CountedLoop(u32 n)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(n))));
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 0x10)));
	p.push_back(P(VDIV_L(vf::vf1, 0, vf::vf2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)));
	p.push_back(P(VFMAND_L(vi::vi3, vi::vi2), VADDq_U(mask::xyz, vf::vf5, vf::vf3)));
	p.push_back(P(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0), VMADDy_U(mask::x | mask::w, vf::vf6, vf::vf5, vf::vf4)));
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VSUB_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf3)));
	p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(2 - 6 - 1)), VMULAx_U(mask::xyzw, vf::vf7, vf::vf4)));
	p.push_back(P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMADDAw_U(mask::xyzw, vf::vf3, vf::vf4))); // delay slot
	p.push_back(EBit(P(VFSAND_L(vi::vi4, 0xfff), VNOP_U())));
	return p;
}

void SetupLoop(VuTestHarness& h)
{
	h.SetVf(1, 2.0f, 3.0f, 4.0f, 5.0f);
	h.SetVf(2, 0.5f, 1.5f, 2.5f, 3.5f);
	h.SetVf(3, 1.0f, 1.0f, 1.0f, 1.0f);
	h.SetVf(4, 1.0001f, 0.9999f, 1.00001f, 0.5f);
}

// A loop that calls one of two subroutines through a table-loaded JALR; each
// returns with JR. microVU's jump caches learn the targets while the program
// runs, and the tier-up hands them to the regions.
constexpr u32 kT1 = 12, kT2 = 16;
std::vector<VuOp> JumpLoop()
{
	constexpr u32 kHead = 2;
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 37)));
	p.push_back(L(VIADDIU_L(vi::vi5, vi::vi0, 1)));
	p.push_back(P(VIAND_L(vi::vi4, vi::vi1, vi::vi5), VMUL_U(mask::xyzw, vf::vf4, vf::vf4, vf::vf2))); // 2: head
	p.push_back(L(VILW_L(mask::x, vi::vi3, vi::vi4, 0)));
	p.push_back(L(VJALR_L(vi::vi15, vi::vi3)));
	p.push_back(U(VADD_U(mask::xyzw, vf::vf5, vf::vf5, vf::vf1)));                                  // delay slot
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMADD_U(mask::xyz, vf::vf6, vf::vf4, vf::vf5)));      // 6: return here
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(static_cast<int>(kHead) - 7 - 1))));
	p.push_back(U(VSUB_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf3)));                                  // delay slot
	p.push_back(EBit(L(VFSAND_L(vi::vi6, 0xfff))));
	p.push_back(U(VNOP_U()));
	p.push_back(U(VNOP_U()));
	p.push_back(U(VADD_U(mask::xyzw, vf::vf1, vf::vf1, vf::vf2))); // 12: T1
	p.push_back(L(VJR_L(vi::vi15)));
	p.push_back(U(VMUL_U(mask::x | mask::y, vf::vf3, vf::vf1, vf::vf1)));
	p.push_back(U(VNOP_U()));
	p.push_back(P(VFMAND_L(vi::vi7, vi::vi5), VSUB_U(mask::xyzw, vf::vf2, vf::vf2, vf::vf1))); // 16: T2
	p.push_back(L(VJR_L(vi::vi15)));
	p.push_back(EBit(U(VMULq_U(mask::z | mask::w, vf::vf3, vf::vf2))));
	p.back().upper &= ~bits::E;
	p.push_back(EBit(U(VNOP_U())));
	return p;
}

void SetupJump(VuTestHarness& h)
{
	h.SetVf(1, 1.0f, 2.0f, 3.0f, 4.0f);
	h.SetVf(2, 0.5f, 0.25f, 0.125f, 1.0f);
	h.SetVf(3, -1.0f, 1.5f, 2.5f, -3.5f);
	h.SetVf(4, 1.001f, 0.999f, 1.0f, 0.5f);
	h.WriteMemU32(0x00, kT2);
	h.WriteMemU32(0x10, kT1);
}

// The program once under microVU alone (the tier off), then `runs` times under
// the tier with each run compared to it. Returns the tier's stats delta.
vuaot::live::Stats RunLive(const std::vector<VuOp>& prog, const Setup& setup, bool flagHack, u32 threshold, u32 runs, u32 budget,
	std::string& problems, u32 jumpThreshold = vuaot::live::kJumpTierUpRuns)
{
	Result ref;
	{
		LiveScope off(false, threshold, flagHack, jumpThreshold);
		VuTestHarness h(1);
		Prepare(h, prog, setup, budget);
		h.RunNoDiff();
		ref = Take(h);
	}
	LiveScope on(true, threshold, flagHack, jumpThreshold);
	const vuaot::live::Stats before = vuaot::live::GetStats();
	VuTestHarness h(1);
	Prepare(h, prog, setup, budget);
	h.RunNoDiff();
	if (const std::string d = Diff(ref, Take(h)); !d.empty())
		problems += "first run: " + d;
	for (u32 i = 0; i < runs; i++)
	{
		h.RunJitPreserveBlockCache();
		if (const std::string d = Diff(ref, Take(h)); !d.empty())
			problems += "run " + std::to_string(i + 2) + ": " + d;
	}
	vuaot::live::Stats after = vuaot::live::GetStats();
	after.tierUps -= before.tierUps;
	after.jumpTierUps -= before.jumpTierUps;
	after.jumpCounted -= before.jumpCounted;
	after.retiers -= before.retiers;
	after.regions -= before.regions;
	after.regionNodes -= before.regionNodes;
	after.targetsRecorded -= before.targetsRecorded;
	after.bailCompiles -= before.bailCompiles;
	return after;
}
} // namespace

// With the tier off nothing counts, tiers up or compiles: the dispatcher is microVU's. (The setting
// is on by default; the test environment starts with it off, see RecompilerTestEnvironment::Initialize.)
TEST(VuAotLive, OffIsInert)
{
	EXPECT_FALSE(EmuConfig.Cpu.Recompiler.EnableVUAot);
	LiveScope off(false, 2, true);
	const vuaot::live::Stats before = vuaot::live::GetStats();
	VuTestHarness h(1);
	Prepare(h, CountedLoop(20), SetupLoop, 0);
	h.RunNoDiff();
	for (int i = 0; i < 20; i++)
		h.RunJitPreserveBlockCache();
	EXPECT_FALSE(vuaot::live::IsEnabled());
	EXPECT_FALSE(vuaot::live::WorkerRunning()) << "the tier off starts no worker thread";
	const vuaot::live::Stats after = vuaot::live::GetStats();
	EXPECT_EQ(after.tierUps, before.tierUps);
	EXPECT_EQ(after.rootsCompiled, before.rootsCompiled);
}

// A counted loop under a small cycle budget, so the tier-up falls in the
// middle of runs that break and resume: every run before, at and after it must
// equal microVU's.
TEST(VuAotLive, TierUpKeepsEveryRunIdentical)
{
	for (bool fh : {true, false})
		for (u32 budget : {0u, 60u, 23u})
		{
			std::string problems;
			const vuaot::live::Stats d = RunLive(CountedLoop(10), SetupLoop, fh, 4, 12, budget, problems);
			EXPECT_TRUE(problems.empty()) << "flagHack=" << fh << " budget=" << budget << "\n" << problems;
			EXPECT_EQ(d.tierUps, 1u) << "flagHack=" << fh << " budget=" << budget;
			EXPECT_GE(d.regions, 1u) << "flagHack=" << fh << " budget=" << budget;
			if (budget == 23)
				EXPECT_GT(d.bailCompiles, 0u) << "a budget this small must take regions' bails (flagHack=" << fh << ")";
		}
}

// JR/JALR: the targets microVU's jump caches learned before the tier-up are
// handed to the regions as direct edges.
TEST(VuAotLive, JumpTargetsComeFromTheJumpCaches)
{
	for (bool fh : {true, false})
		for (u32 budget : {0u, 40u})
		{
			std::string problems;
			const vuaot::live::Stats d = RunLive(JumpLoop(), SetupJump, fh, 4, 12, budget, problems);
			EXPECT_TRUE(problems.empty()) << "flagHack=" << fh << " budget=" << budget << "\n" << problems;
			EXPECT_EQ(d.tierUps, 1u);
			EXPECT_GE(d.targetsRecorded, 3u) << "T1, T2 and the return";
			EXPECT_GE(d.regions, 1u);
		}
}

// The JR/JALR slow path counts, but it does not tier up a program the dispatcher enters: that
// keeps its dispatch threshold, because tiering it from a jump into its middle would root its
// first compile there. With the dispatch threshold out of reach nothing tiers up, and every
// run equals microVU's.
TEST(VuAotLive, JumpPathLeavesADispatchedProgramToItsDispatchCount)
{
	for (bool fh : {true, false})
	{
		std::string problems;
		const vuaot::live::Stats d = RunLive(JumpLoop(), SetupJump, fh, 1000000, 12, 0, problems, 6);
		EXPECT_TRUE(problems.empty()) << "flagHack=" << fh << "\n" << problems;
		EXPECT_EQ(d.tierUps, 0u) << "flagHack=" << fh;
		EXPECT_GT(d.jumpCounted, 6u) << "the JR path counts every entry it resolves";
	}
}

// A stub the dispatcher enters, whose JR goes into an overlay (JumpLoop's code) the dispatcher never enters, so the tier-up
// comes from inside the overlay's running code; this is God of War II's shape. Micro memory changes after the stub's program
// was made (the first run stops before the JR executes, then a word outside its ranges is rewritten), so the search files the
// JR's target as a program of its own. Each run is preceded by a micro-memory write that nulls the quick slots, so every
// JR takes the slow path.
namespace
{
constexpr u32 kOverlayPair = 32;
std::vector<VuOp> StubAndOverlay()
{
	std::vector<VuOp> p;
	p.push_back(U(VADD_U(mask::xyzw, vf::vf5, vf::vf5, vf::vf1)));                       // 0
	p.push_back(L(VB_L(2)));                                                              // 1: to 4
	p.push_back(U(VMUL_U(mask::xyz, vf::vf6, vf::vf6, vf::vf2)));                         // 2: delay slot
	p.push_back(U(VNOP_U()));                                                             // 3
	p.push_back(L(VIADDIU_L(vi::vi5, vi::vi0, static_cast<u16>(kOverlayPair))));          // 4
	p.push_back(L(VJR_L(vi::vi5)));                                                       // 5
	p.push_back(U(VSUB_U(mask::xyzw, vf::vf7, vf::vf7, vf::vf3)));                        // 6: delay slot
	while (p.size() < kOverlayPair)
		p.push_back(U(VNOP_U()));
	// the overlay: JumpLoop's code, whose own JALR/JR meet the jump cache over and over
	for (const VuOp& op : JumpLoop())
		p.push_back(op);
	return p;
}

void SetupOverlay(VuTestHarness& h)
{
	SetupJump(h);
	h.WriteMemU32(0x00, kOverlayPair + kT2);
	h.WriteMemU32(0x10, kOverlayPair + kT1);
}

std::vector<Result> OverlaySequence(bool on, bool flagHack, u32 runs, vuaot::live::Stats& delta)
{
	std::vector<Result> out;
	LiveScope scope(on, 1000000, flagHack, 5);
	const vuaot::live::Stats before = vuaot::live::GetStats();
	VuTestHarness h(1);
	SetupOverlay(h);
	h.TrackMemWindow(0, 0x4000);
	h.LoadProgram(StubAndOverlay());
	h.RunNoDiff(); // (leaves VU1 with no programs: the harness resets the cache after its second pass)
	out.push_back(Take(h));
	// Run 0: the stub's first block runs and the break comes at the second, so the stub's
	// program is made and the JR is never reached: its ranges end at the stub.
	h.SetCycleBudget(1);
	h.RunJitPreserveBlockCache();
	out.push_back(Take(h));
	h.SetCycleBudget(100000);
	// A word nobody executes changes the image: the next search no longer finds the stub's
	// program by content, so the JR's target becomes a program of its own.
	const u32 marker = 0x12345678;
	std::memcpy(vuRegs[1].Micro + 0x3000, &marker, 4);
	for (u32 i = 0; i < runs; i++)
	{
		// (a write over the overlay's range: micro-memory writes null only the quick slots of
		// programs whose compiled ranges they overlap)
		CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
		h.RunJitPreserveBlockCache();
		out.push_back(Take(h));
	}
	vuaot::live::Stats after = vuaot::live::GetStats();
	delta = after;
	delta.tierUps -= before.tierUps;
	delta.jumpTierUps -= before.jumpTierUps;
	delta.jumpCounted -= before.jumpCounted;
	delta.regions -= before.regions;
	return out;
}
} // namespace

TEST(VuAotLive, JumpPathTiersUpAnOverlayTheDispatcherNeverEnters)
{
	for (bool fh : {true, false})
	{
		vuaot::live::Stats off{}, on{};
		const std::vector<Result> ref = OverlaySequence(false, fh, 12, off);
		const std::vector<Result> got = OverlaySequence(true, fh, 12, on);
		ASSERT_EQ(ref.size(), got.size());
		for (size_t i = 0; i < ref.size(); i++)
			EXPECT_EQ(Diff(ref[i], got[i]), "") << "flagHack=" << fh << " run " << i;
		// The overlay's entry and the routines its own jumps land on are each a routine the
		// jump path counts; the stub, which only the dispatcher enters, is not tiered.
		EXPECT_GE(on.tierUps, 1u) << "flagHack=" << fh;
		EXPECT_EQ(on.jumpTierUps, on.tierUps) << "the stub was tiered up (flagHack=" << fh << ")";
		EXPECT_GE(on.jumpCounted, 5u);
		EXPECT_GE(on.regions, 1u);
	}
}

// The background compile: microVU keeps running a program while the worker builds its regions, and what the worker made
// is published at a slow path. None of it may change what a run leaves.

namespace
{
// Holds the worker inside a job until released: a compile in flight.
struct JobGate
{
	std::mutex mu;
	std::condition_variable cv;
	bool entered = false, released = false;

	void Install()
	{
		vuaot::live::SetWorkerGate([this] {
			std::unique_lock lk(mu);
			entered = true;
			cv.notify_all();
			cv.wait(lk, [this] { return released; });
		});
	}
	void WaitEntered()
	{
		std::unique_lock lk(mu);
		ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(30), [this] { return entered; })) << "no job started";
	}
	void Release()
	{
		std::lock_guard lk(mu);
		released = true;
		cv.notify_all();
	}
};

vuaot::live::Stats Delta(const vuaot::live::Stats& a, const vuaot::live::Stats& b)
{
	vuaot::live::Stats d = b;
	d.tierUps -= a.tierUps;
	d.regions -= a.regions;
	d.jobs -= a.jobs;
	d.jobsDropped -= a.jobsDropped;
	d.jobsDone -= a.jobsDone;
	d.built -= a.built;
	d.discardStale -= a.discardStale;
	d.discardMemory -= a.discardMemory;
	return d;
}

void WriteMicro(const std::vector<VuOp>& prog)
{
	std::memset(vuRegs[1].Micro, 0, 0x4000);
	for (size_t i = 0; i < prog.size(); i++)
	{
		std::memcpy(vuRegs[1].Micro + i * 8 + 0, &prog[i].lower, 4);
		std::memcpy(vuRegs[1].Micro + i * 8 + 4, &prog[i].upper, 4);
	}
	CpuMicroVU1.Clear(0, 0x4000);
}
} // namespace

// The tier-up asked for while the worker is held: microVU runs the program on,
// unchanged, and nothing is published. Released, the region is published at the
// next dispatch, and the runs after it are vu-aot's, still identical.
TEST(VuAotLive, BackgroundTierUpPublishesAtADispatch)
{
	for (bool fh : {true, false})
		for (u32 budget : {0u, 23u})
		{
			Result ref;
			{
				LiveScope off(false, 4, fh);
				VuTestHarness h(1);
				Prepare(h, CountedLoop(10), SetupLoop, budget);
				h.RunNoDiff();
				ref = Take(h);
			}
			LiveScope on(true, 4, fh, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
			VuTestHarness h(1);
			Prepare(h, CountedLoop(10), SetupLoop, budget);
			h.RunNoDiff();
			EXPECT_EQ(Diff(ref, Take(h)), "");
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			vuaot::live::HoldWorker(true);
			for (int i = 0; i < 8; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << "held, run " << i << " fh=" << fh << " budget=" << budget;
			}
			vuaot::live::Stats d = Delta(s0, vuaot::live::GetStats());
			EXPECT_EQ(d.jobs, 1u) << "one tier-up asked for, not one per dispatch";
			EXPECT_EQ(d.built, 0u);
			EXPECT_EQ(d.regions, 0u);
			vuaot::live::HoldWorker(false);
			vuaot::live::WaitWorkerIdle();
			for (int i = 0; i < 6; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << "released, run " << i << " fh=" << fh << " budget=" << budget;
			}
			d = Delta(s0, vuaot::live::GetStats());
			EXPECT_EQ(d.built, 1u) << "fh=" << fh << " budget=" << budget;
			EXPECT_EQ(d.tierUps, 1u);
			EXPECT_GE(d.regions, 1u);
		}
}

// Micro memory changes under a finished job before it is published, through a write the program's quick slot did not see
// (so microVU keeps running its old code): the job compiled words that are gone, and is discarded, never published.
TEST(VuAotLive, PublishAfterMicroMemoryChangedIsDiscarded)
{
	const auto sequence = [](bool on, vuaot::live::Stats& d) {
		std::vector<Result> out;
		LiveScope scope(on, 4, true, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, CountedLoop(10), SetupLoop, 0);
		h.RunNoDiff();
		out.push_back(Take(h));
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		vuaot::live::HoldWorker(true);
		for (int i = 0; i < 6; i++)
		{
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
		}
		vuaot::live::HoldWorker(false);
		vuaot::live::WaitWorkerIdle();
		// The loop's subtract (pair 5) becomes a different immediate.
		const u32 lower = VISUBIU_L(vi::vi1, vi::vi1, 2);
		std::memcpy(vuRegs[1].Micro + 5 * 8, &lower, 4);
		h.RunJitPreserveBlockCache();
		out.push_back(Take(h));
		// Keep the re-asked tier-up from running: a job made now would compile the new word for a program whose quick slot never
		// saw the change, a state a real write (through mVUclear) cannot leave behind.
		vuaot::live::HoldWorker(true);
		for (int i = 0; i < 3; i++)
		{
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
		}
		d = Delta(s0, vuaot::live::GetStats());
		return out;
	};
	vuaot::live::Stats off{}, on{};
	const std::vector<Result> ref = sequence(false, off);
	const std::vector<Result> got = sequence(true, on);
	ASSERT_EQ(ref.size(), got.size());
	for (size_t i = 0; i < ref.size(); i++)
		EXPECT_EQ(Diff(ref[i], got[i]), "") << "run " << i;
	EXPECT_EQ(on.jobs, 2u) << "the discarded tier-up is asked for again (and held)";
	EXPECT_EQ(on.discardMemory, 1u) << "the job read the word that changed";
	EXPECT_EQ(on.built, 0u);
	EXPECT_EQ(on.regions, 0u);
}

// A reset while a job is compiling (cache exhaustion; a savestate load resets the
// same way): the job finishes into its own memory, is dropped unread when it is
// collected, and the program tiers up again afterwards.
TEST(VuAotLive, ResetDuringCompileDropsTheJob)
{
	for (int how = 0; how < 2; how++)
	{
		Result ref;
		{
			LiveScope off(false, 4, true);
			VuTestHarness h(1);
			Prepare(h, CountedLoop(10), SetupLoop, 0);
			h.RunNoDiff();
			ref = Take(h);
		}
		LiveScope on(true, 4, true, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, CountedLoop(10), SetupLoop, 0);
		h.RunNoDiff();
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		JobGate gate;
		gate.Install();
		for (int i = 0; i < 6; i++)
		{
			h.RunJitPreserveBlockCache();
			EXPECT_EQ(Diff(ref, Take(h)), "") << "how=" << how << " run " << i;
		}
		gate.WaitEntered();
		// how 0: the recompiler reset a cache-full or a settings change makes; how 1:
		// the one a savestate load makes (VMManager's ClearCPUExecutionCaches resets VU1).
		if (how == 0)
			RecompilerTestEnvironment::ResetVuBlockCache(1);
		else
			CpuMicroVU1.Reset();
		vuaot::live::SetWorkerGate(nullptr);
		gate.Release();
		vuaot::live::WaitWorkerIdle();
		for (int i = 0; i < 12; i++)
		{
			h.RunJitPreserveBlockCache();
			EXPECT_EQ(Diff(ref, Take(h)), "") << "how=" << how << " after reset, run " << i;
			if (i == 6)
				vuaot::live::WaitWorkerIdle();
		}
		const vuaot::live::Stats d = Delta(s0, vuaot::live::GetStats());
		EXPECT_EQ(d.discardStale, 1u) << "how=" << how;
		EXPECT_EQ(d.jobs, 2u) << "the tier-up asked for again after the reset (how=" << how << ")";
		EXPECT_EQ(d.built, 1u) << "how=" << how;
		EXPECT_GE(d.regions, 1u);
	}
}

#if defined(__linux__)
// The worker runs wherever the process's first thread may, not where the thread that started it was
// pinned: the tier can be switched on from the CPU thread of a running game, which the emulator has
// pinned to one core. (Mutation: leave the new thread's mask as it begins, and it is that one core.)
TEST(VuAotLive, WorkerStartedFromAPinnedThreadIsNotPinnedWithIt)
{
	cpu_set_t processMask;
	ASSERT_EQ(sched_getaffinity(getpid(), sizeof(processMask), &processMask), 0);
	if (CPU_COUNT(&processMask) < 2)
		GTEST_SKIP() << "the process may use one processor only";
	int firstCpu = 0;
	while (!CPU_ISSET(firstCpu, &processMask))
		firstCpu++;

	struct ReadMask final : vuaot::bg::Job
	{
		int cpus = -1;
		void Run() override
		{
			cpu_set_t m;
			if (sched_getaffinity(0, sizeof(m), &m) == 0)
				cpus = CPU_COUNT(&m);
		}
	};
	auto job = std::make_shared<ReadMask>();

	std::thread starter([&] {
		cpu_set_t one;
		CPU_ZERO(&one);
		CPU_SET(firstCpu, &one);
		ASSERT_EQ(sched_setaffinity(0, sizeof(one), &one), 0);
		vuaot::bg::Worker worker;
		worker.Start();
		ASSERT_TRUE(worker.Submit(job));
		worker.Wait(job);
		worker.Stop();
	});
	starter.join();
	EXPECT_EQ(job->cpus, CPU_COUNT(&processMask)) << "the worker kept the starting thread's single core";
}
#endif

// The tier switched off while a job is compiling: the reset that applies the
// setting stops the worker (waiting for the job), nothing it made is published,
// no worker thread is left, and microVU alone runs on.
TEST(VuAotLive, SettingOffDuringCompileStopsTheWorker)
{
	Result ref;
	{
		LiveScope off(false, 4, true);
		VuTestHarness h(1);
		Prepare(h, CountedLoop(10), SetupLoop, 0);
		h.RunNoDiff();
		ref = Take(h);
		EXPECT_FALSE(vuaot::live::WorkerRunning()) << "the tier off has no worker thread";
	}
	LiveScope on(true, 4, true, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
	VuTestHarness h(1);
	Prepare(h, CountedLoop(10), SetupLoop, 0);
	h.RunNoDiff();
	EXPECT_TRUE(vuaot::live::WorkerRunning());
	const vuaot::live::Stats s0 = vuaot::live::GetStats();
	JobGate gate;
	gate.Install();
	for (int i = 0; i < 6; i++)
		h.RunJitPreserveBlockCache();
	gate.WaitEntered();
	// The reset joins the worker, which is inside the job: let it go a moment later.
	std::thread releaser([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		gate.Release();
	});
	EmuConfig.Cpu.Recompiler.EnableVUAot = false;
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	releaser.join();
	EXPECT_FALSE(vuaot::live::IsEnabled());
	EXPECT_FALSE(vuaot::live::WorkerRunning());
	for (int i = 0; i < 6; i++)
	{
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(Diff(ref, Take(h)), "") << "run " << i;
	}
	const vuaot::live::Stats d = Delta(s0, vuaot::live::GetStats());
	EXPECT_EQ(d.built, 0u);
	EXPECT_EQ(d.regions, 0u);
}

// Many programs reach the threshold at once, more of them than the worker gets
// through before they are dispatched again: one job each (none asked for twice
// while one is pending), every run of every program identical to microVU's
// whether its region is published yet or not, and all published in the end.
TEST(VuAotLive, ManyConcurrentRequests)
{
	constexpr u32 kPrograms = 24;
	std::vector<std::vector<VuOp>> progs;
	for (u32 k = 0; k < kPrograms; k++)
		progs.push_back(CountedLoop(5 + k));
	const auto sequence = [&](bool on, vuaot::live::Stats& d) {
		std::vector<Result> out;
		LiveScope scope(on, 2, true, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, progs[0], SetupLoop, 0);
		h.RunNoDiff();
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		for (u32 round = 0; round < 8; round++)
		{
			for (u32 k = 0; k < kPrograms; k++)
			{
				WriteMicro(progs[k]);
				h.RunJitPreserveBlockCache();
				out.push_back(Take(h));
			}
			if (round == 5)
				vuaot::live::WaitWorkerIdle();
		}
		d = Delta(s0, vuaot::live::GetStats());
		return out;
	};
	vuaot::live::Stats off{}, on{};
	const std::vector<Result> ref = sequence(false, off);
	const std::vector<Result> got = sequence(true, on);
	ASSERT_EQ(ref.size(), got.size());
	for (size_t i = 0; i < ref.size(); i++)
		EXPECT_EQ(Diff(ref[i], got[i]), "") << "run " << i << " (program " << (i % kPrograms) << ")";
	EXPECT_EQ(on.jobsDropped, 0u);
	EXPECT_EQ(on.jobs, kPrograms) << "one tier-up per program";
	EXPECT_EQ(on.built, kPrograms);
	EXPECT_GE(on.regions, kPrograms);
}

// A tiered program survives a settings round trip: switching the tier off and
// on again resets VU1 (every program, with the tier's state for it, is freed)
// and the next runs tier it up again.
TEST(VuAotLive, SettingsRoundTrip)
{
	LiveScope on(true, 3, true);
	for (int round = 0; round < 3; round++)
	{
		const vuaot::live::Stats before = vuaot::live::GetStats();
		VuTestHarness h(1);
		Prepare(h, CountedLoop(50), SetupLoop, 0);
		h.RunNoDiff();
		const Result ref = Take(h);
		for (int i = 0; i < 8; i++)
		{
			h.RunJitPreserveBlockCache();
			EXPECT_EQ(Diff(ref, Take(h)), "") << "round " << round << " run " << i;
		}
		EXPECT_EQ(vuaot::live::GetStats().tierUps - before.tierUps, 1u) << "round " << round;
		EmuConfig.Cpu.Recompiler.EnableVUAot = false;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		EXPECT_FALSE(vuaot::live::IsEnabled());
		EmuConfig.Cpu.Recompiler.EnableVUAot = true;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		EXPECT_TRUE(vuaot::live::IsEnabled());
	}
}

// The gamefixes vu-aot does not model keep the tier from running at all.
TEST(VuAotLive, UnsupportedGamefixKeepsItOff)
{
	LiveScope on(true, 2, true);
	const bool savedIbit = EmuConfig.Gamefixes.IbitHack;
	EmuConfig.Gamefixes.IbitHack = true;
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	EXPECT_FALSE(vuaot::live::IsEnabled());
	EmuConfig.Gamefixes.IbitHack = savedIbit;
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	EXPECT_TRUE(vuaot::live::IsEnabled());
}

// Sticky-STATUS tripwire: regions drop the sticky bits of STATUS (6-7 and 10-11) in either flag-hack mode while no reader of
// them is known. A reader that shows up (a micro image holding one, a compiled one, VU0 touching VU1's STATUS) sets the
// tripwire off, and VU1's regions are rebuilt and kept exact.

namespace
{
using vuaot::live::TripCause;

// A loop that makes every sticky bit: negative and zero FMAC results (the sticky sign and zero
// bits) and a divide by zero (the sticky divide-by-zero bit). Its only STATUS reader looks at
// the current bits.
std::vector<VuOp> StickyLoop(u32 n)
{
	constexpr u32 kHead = 2, kBranch = 8;
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(n))));
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 0x10)));
	p.push_back(P(VDIV_L(vf::vf1, 0, vf::vf2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)));
	p.push_back(P(VFMAND_L(vi::vi3, vi::vi2), VADDq_U(mask::xyz, vf::vf5, vf::vf3)));
	p.push_back(P(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0), VMADDy_U(mask::x | mask::w, vf::vf6, vf::vf5, vf::vf4)));
	p.push_back(U(VSUB_U(mask::xyz, vf::vf8, vf::vf0, vf::vf4)));  // negative
	p.push_back(U(VSUB_U(mask::xyzw, vf::vf9, vf::vf3, vf::vf3))); // zero
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VSUB_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf3)));
	p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(static_cast<int>(kHead) - static_cast<int>(kBranch) - 1)),
		VMULAx_U(mask::xyzw, vf::vf7, vf::vf4)));
	p.push_back(P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMADDAw_U(mask::xyzw, vf::vf3, vf::vf4))); // delay slot
	p.push_back(EBit(P(VFSAND_L(vi::vi4, 0x03f), VNOP_U()))); // (no sticky bit read)
	return p;
}

void SetupSticky(VuTestHarness& h)
{
	SetupLoop(h);
	h.SetVf(2, 0.5f, 0.0f, 2.5f, 3.5f); // the loop's DIV is vf1.x / vf2.y
}

// Everything but the sticky bits of STATUS: the register's bits 6-7 and 10-11, the low byte and
// bits 24-25 of its ring copies. What the contract frees while the tripwire is armed.
std::string DiffExceptSticky(const Result& a, const Result& b)
{
	Result x = a, y = b;
	for (Result* r : {&x, &y})
	{
		r->snap.regs.VI[REG_STATUS_FLAG].UL &= ~0xCC0u;
		for (u32& w : r->snap.regs.micro_statusflags)
			w &= ~0x30000FFu;
	}
	return Diff(x, y);
}

bool StickyBitsDiffer(const Result& a, const Result& b, u32 archMask)
{
	return ((a.snap.regs.VI[REG_STATUS_FLAG].UL ^ b.snap.regs.VI[REG_STATUS_FLAG].UL) & archMask) != 0;
}

// An image holding one reader: FSAND of `imm12`, an E bit, and the delay-slot pair.
void WriteReaderImage(u32 at, u32 imm12)
{
	const VuOp ops[2] = {EBit(P(VFSAND_L(vi::vi5, imm12), VNOP_U())), NopPair()};
	for (int i = 0; i < 2; i++)
	{
		std::memcpy(vuRegs[1].Micro + at + i * 8 + 0, &ops[i].lower, 4);
		std::memcpy(vuRegs[1].Micro + at + i * 8 + 4, &ops[i].upper, 4);
	}
	CpuMicroVU1.Clear(at, 16);
}

enum class Trigger
{
	Upload,     // an image with a sticky reader is run (a program is created over it)
	Compile,    // ... with the scan of new programs out of the way, so it is the compile that notices
	OutsideJit, // microVU0's code reads VU1's STATUS through the window
	OutsideInt, // the VU0 interpreter does
	Forced,
};

constexpr u32 kReaderAt = 0x3000;

struct TripOutcome
{
	std::string problems;
	vuaot::live::Stats d; // counters over the trip and what followed
};

// The program under the tier with the drop in effect, then the tripwire set off by `trig`; every
// run before it equals microVU's but for the sticky bits (and some do differ in them), every run
// from the next one on equals microVU's exactly. `mutation`: one of TripMutation.
TripOutcome RunTripScenario(Trigger trig, bool flagHack, int mutation, u32 readerImm = 0xfc0)
{
	TripOutcome out;
	Result ref;
	{
		LiveScope off(false, 4, flagHack);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		ref = Take(h);
	}
	LiveScope on(true, 4, flagHack);
	VuTestHarness h(1);
	Prepare(h, StickyLoop(10), SetupSticky, 0);
	h.RunNoDiff();
	const vuaot::live::Stats s0 = vuaot::live::GetStats();
	bool sawSticky = false, sawDivSticky = false;
	const auto expectDropped = [&](int runs, const char* when) {
		for (int i = 0; i < runs; i++)
		{
			h.RunJitPreserveBlockCache();
			const Result got = Take(h);
			if (const std::string d = DiffExceptSticky(ref, got); !d.empty())
				out.problems += std::string(when) + " run " + std::to_string(i) + ": " + d;
			sawSticky |= StickyBitsDiffer(ref, got, 0xC0);
			sawDivSticky |= StickyBitsDiffer(ref, got, 0xC00);
		}
	};
	expectDropped(10, "armed");
	if (vuaot::live::GetStats().stickyDropped == s0.stickyDropped)
		out.problems += "no region dropped the sticky byte before the trip\n";
	if (!sawSticky)
		out.problems += "the drop was not visible in the sticky zero/sign bits\n";
	if (!sawDivSticky)
		out.problems += "the drop was not visible in the sticky divide bits\n";
	if (vuaot::live::StickyTripped())
		out.problems += "tripped before any reader\n";

	vuaot::live::SetTripwireMutation(mutation);
	TripCause want = TripCause::None;
	switch (trig)
	{
		case Trigger::Upload:
		case Trigger::Compile:
		{
			if (trig == Trigger::Compile)
				vuaot::live::SetTripwireMutation(mutation | vuaot::live::TripMask(vuaot::live::kTripMutProgramImage));
			WriteReaderImage(kReaderAt, readerImm);
			h.SetStartPc(kReaderAt);
			h.RunJitPreserveBlockCache();
			h.SetStartPc(0);
			want = trig == Trigger::Upload ? TripCause::ProgramImage : TripCause::Compile;
			break;
		}
		case Trigger::OutsideJit:
		case Trigger::OutsideInt:
		{
			VuTestHarness h0(0);
			h0.SetVi(vi::vi1, 0x400 + 48); // VU1's VI[16], STATUS
			h0.LoadProgram({VuOp{VILW_L(mask::x, vi::vi5, vi::vi1, 0), VNOP_U()}, EBitNopPair()});
			if (trig == Trigger::OutsideInt)
				h0.RunInterpOnly();
			else
			{
				// (compile and run once with the hook out of the way: the interpreter pass of
				// RunNoDiff would be the one to notice; the second, JIT-only run is the test)
				vuaot::live::SetTripwireMutation(vuaot::live::TripMask(vuaot::live::kTripMutOutside));
				h0.RunNoDiff();
				vuaot::live::SetTripwireMutation(mutation);
				h0.RunJitPreserveBlockCache();
			}
			want = TripCause::OutsideRead;
			break;
		}
		case Trigger::Forced:
			vuaot::live::TripStickyForTest();
			want = TripCause::Forced;
			break;
	}
	if (!vuaot::live::StickyTripped())
		out.problems += "the tripwire did not go off\n";
	else if (vuaot::live::StickyTripCause() != want)
		out.problems += std::string("tripped by the wrong thing: ") + vuaot::live::TripCauseName(vuaot::live::StickyTripCause()) + "\n";

	// From the next run on: exactly microVU's, the sticky bits included, and the tier tiers up again
	// (with the drop off).
	const vuaot::live::Stats s1 = vuaot::live::GetStats();
	for (int i = 0; i < 12; i++)
	{
		h.RunJitPreserveBlockCache();
		if (const std::string d = Diff(ref, Take(h)); !d.empty())
			out.problems += "after the trip, run " + std::to_string(i) + ": " + d;
	}
	out.d = vuaot::live::GetStats();
	out.d.stickyTrips -= s0.stickyTrips;
	out.d.stickyTripResets -= s0.stickyTripResets;
	out.d.tierUps -= s1.tierUps;
	out.d.stickyDropped -= s1.stickyDropped;
	out.d.regions -= s1.regions;
	return out;
}

std::string TripReport(const TripOutcome& o, const char* what)
{
	std::string out = o.problems;
	if (o.d.stickyTrips != 1)
		out += std::string(what) + ": trips " + std::to_string(o.d.stickyTrips) + ", want 1\n";
	if (o.d.stickyTripResets != 1)
		out += std::string(what) + ": resets " + std::to_string(o.d.stickyTripResets) + ", want 1 (the regions that dropped the byte are rebuilt)\n";
	if (o.d.tierUps != 1)
		out += std::string(what) + ": tier-ups after the trip " + std::to_string(o.d.tierUps) + ", want 1\n";
	if (o.d.stickyDropped != 0)
		out += std::string(what) + ": regions built after the trip still dropped the byte\n";
	return out;
}
} // namespace

// A micro image holding a sticky reader is run: the programs tiered up under the drop are rebuilt,
// the STATUS at a program's end is microVU's again, byte for byte, and the new regions keep it so.
TEST(VuAotLive, TripwireImageUploadRebuildsExact)
{
	for (bool fh : {true, false})
	{
		const TripOutcome o = RunTripScenario(Trigger::Upload, fh, 0);
		EXPECT_EQ(TripReport(o, "upload"), "") << "flagHack=" << fh;
	}
}

// ... and the compile alone notices it too (the scan of a new program's image out of the way).
TEST(VuAotLive, TripwireCompiledReaderRebuildsExact)
{
	for (bool fh : {true, false})
	{
		const TripOutcome o = RunTripScenario(Trigger::Compile, fh, 0);
		EXPECT_EQ(TripReport(o, "compile"), "") << "flagHack=" << fh;
	}
}

// Anything that selects one of the sticky bits trips it, the divide's sticky bits (10 and 11) alone
// included; a reader of the current bits only does not.
TEST(VuAotLive, TripwireEveryStickyBitTripsAndTheCurrentBitsDoNot)
{
	for (u32 imm : {0x040u, 0x080u, 0x100u, 0x200u, 0x400u, 0x800u, 0xc00u, 0xfffu})
	{
		LiveScope on(true, 4, false);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		EXPECT_FALSE(vuaot::live::StickyTripped());
		vuaot::live::SetTripwireMutation(vuaot::live::TripMask(vuaot::live::kTripMutProgramImage)); // (the compile hook alone)
		WriteReaderImage(kReaderAt, imm);
		h.SetStartPc(kReaderAt);
		h.RunJitPreserveBlockCache();
		EXPECT_TRUE(vuaot::live::StickyTripped()) << "imm12=" << std::hex << imm;
		EXPECT_EQ(vuaot::live::StickyTripCause(), TripCause::Compile) << "imm12=" << std::hex << imm;
	}
	for (u32 imm : {0x000u, 0x001u, 0x03fu, 0x030u, 0x03cu})
	{
		LiveScope on(true, 4, false);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		WriteReaderImage(kReaderAt, imm);
		h.SetStartPc(kReaderAt);
		h.RunJitPreserveBlockCache();
		EXPECT_FALSE(vuaot::live::StickyTripped()) << "imm12=" << std::hex << imm;
	}
}

// VU0 reads VU1's STATUS register through its window onto VU1's registers: from microVU0's code
// and from the interpreter.
TEST(VuAotLive, TripwireOutsideReadRebuildsExact)
{
	for (bool fh : {true, false})
	{
		TripOutcome o = RunTripScenario(Trigger::OutsideJit, fh, 0);
		EXPECT_EQ(TripReport(o, "outside, jit"), "") << "flagHack=" << fh;
		o = RunTripScenario(Trigger::OutsideInt, fh, 0);
		EXPECT_EQ(TripReport(o, "outside, interpreter"), "") << "flagHack=" << fh;
	}
}

// The other quadwords of the window, VU1's own accesses and VU0's own memory are not readers of
// VU1's STATUS.
TEST(VuAotLive, TripwireOrdinaryAccessesDoNotTrip)
{
	LiveScope on(true, 4, false);
	const vuaot::live::Stats base = vuaot::live::GetStats();
	VuTestHarness h1(1);
	Prepare(h1, StickyLoop(10), SetupSticky, 0);
	h1.RunNoDiff();
	for (int i = 0; i < 8; i++)
		h1.RunJitPreserveBlockCache();
	EXPECT_GE(vuaot::live::GetStats().stickyDropped, 1u);
	EXPECT_FALSE(vuaot::live::StickyTripped()) << "a program with no reader";

	// every quadword of the window but VU1's STATUS (VF0-31 are 0-31, VI0-15 are 32-47, MAC is 49, CLIP 50 ...)
	for (u32 q = 0; q < 64; q++)
	{
		if (q == VU0_WINDOW_VU1_STATUS_QWORD)
			continue;
		VuTestHarness h0(0);
		h0.SetVi(vi::vi1, 0x400 + q);
		h0.LoadProgram({VuOp{VILW_L(mask::x, vi::vi5, vi::vi1, 0), VNOP_U()},
			VuOp{VLQ_L(mask::xyzw, vf::vf3, vi::vi1, 0), VNOP_U()}, EBitNopPair()});
		h0.RunNoDiff();
		h0.RunJitPreserveBlockCache();
		h0.RunInterpOnly();
		ASSERT_FALSE(vuaot::live::StickyTripped()) << "quadword " << q << " of the window";
	}
	// VU0's own memory, and the same indices without the window bit
	for (u32 q : {0u, 16u, 48u, 0x1f0u, 0x3f0u})
	{
		VuTestHarness h0(0);
		h0.SetVi(vi::vi1, q);
		h0.LoadProgram({VuOp{VILW_L(mask::x, vi::vi5, vi::vi1, 0), VNOP_U()}, EBitNopPair()});
		h0.RunNoDiff();
		h0.RunJitPreserveBlockCache();
		ASSERT_FALSE(vuaot::live::StickyTripped()) << "VU0 memory " << q;
	}
	// the interpreter's address function, VU1's own and a window access that is not VI[16]
	GET_VU_MEM(&vuRegs[1], 0x4300);
	GET_VU_MEM(&vuRegs[0], 0x2f0);
	GET_VU_MEM(&vuRegs[0], 0x42f0);
	GET_VU_MEM(&vuRegs[0], 0x4310);
	EXPECT_FALSE(vuaot::live::StickyTripped());
	GET_VU_MEM(&vuRegs[0], 0x4300);
	EXPECT_TRUE(vuaot::live::StickyTripped());
	EXPECT_EQ(vuaot::live::StickyTripCause(), TripCause::OutsideRead);
	const vuaot::live::Stats now = vuaot::live::GetStats();
	EXPECT_EQ(now.stickyTrips - base.stickyTrips, 1u) << "the one trip of the test";
	EXPECT_EQ(now.stickyTripsBy[static_cast<u32>(TripCause::OutsideRead)] - base.stickyTripsBy[static_cast<u32>(TripCause::OutsideRead)], 1u);
}

// A job made while the tripwire was armed finishes after it went off: the region it built does not
// drop the byte of a program that is now known to have readers, so it is not published, and the
// runs equal microVU's throughout.
TEST(VuAotLive, TripwireJobMadeBeforeItIsNotPublished)
{
	for (bool fh : {true, false})
	{
		const auto sequence = [&](int mutation, bool on, std::string& problems, vuaot::live::Stats& d) {
			Result ref;
			{
				LiveScope off(false, 4, fh);
				VuTestHarness h(1);
				Prepare(h, StickyLoop(10), SetupSticky, 0);
				h.RunNoDiff();
				ref = Take(h);
			}
			LiveScope scope(on, 4, fh, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
			VuTestHarness h(1);
			Prepare(h, StickyLoop(10), SetupSticky, 0);
			h.RunNoDiff();
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			vuaot::live::HoldWorker(true);
			for (int i = 0; i < 6; i++)
			{
				h.RunJitPreserveBlockCache();
				if (const std::string x = Diff(ref, Take(h)); !x.empty())
					problems += "queued, run " + std::to_string(i) + ": " + x;
			}
			vuaot::live::HoldWorker(false);
			vuaot::live::WaitWorkerIdle();
			vuaot::live::TripStickyForTest(); // the job is done and not yet published
			vuaot::live::SetTripwireMutation(mutation);
			for (int i = 0; i < 4; i++)
			{
				h.RunJitPreserveBlockCache();
				if (const std::string x = Diff(ref, Take(h)); !x.empty())
					problems += "after the trip, run " + std::to_string(i) + ": " + x;
			}
			d = vuaot::live::GetStats();
			d.discardSticky -= s0.discardSticky;
			d.built -= s0.built;
			d.stickyDropped -= s0.stickyDropped;
			d.stickyTripResets -= s0.stickyTripResets;
		};
		std::string problems;
		vuaot::live::Stats d{};
		sequence(0, true, problems, d);
		EXPECT_EQ(problems, "") << "flagHack=" << fh;
		EXPECT_EQ(d.stickyDropped, 1u) << "the job was made with the drop";
		EXPECT_EQ(d.discardSticky, 1u) << "flagHack=" << fh;
		// (a tier-up asked for after the trip is built without the drop and may be published in the
		// runs that follow: at most that one)
		EXPECT_LE(d.built, 1u) << "flagHack=" << fh;
		EXPECT_EQ(d.stickyTripResets, 0u) << "nothing was built with the drop, so nothing is rebuilt";
	}
}

// A sticky reader uploaded outside the ranges of the program that runs makes no new program, so
// the scan of a new program's image does not see it. The job made for the program's tier-up
// scans the micro image it is made from: the tripwire goes off there, the job keeps the sticky
// byte, and every run equals microVU's, the sticky bits included.
TEST(VuAotLive, TripwireJobImageHoldingAReaderKeepsTheStickyByte)
{
	for (bool fh : {true, false})
	{
		Result ref;
		{
			LiveScope off(false, 4, fh);
			VuTestHarness h(1);
			Prepare(h, StickyLoop(10), SetupSticky, 0);
			h.RunNoDiff();
			ref = Take(h);
		}
		LiveScope on(true, 4, fh);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		std::string problems;
		for (int i = 0; i < 2; i++)
		{
			h.RunJitPreserveBlockCache();
			if (const std::string d = Diff(ref, Take(h)); !d.empty())
				problems += "before the upload, run " + std::to_string(i) + ": " + d;
		}
		// (not run: no program is made over it)
		WriteReaderImage(kReaderAt, 0xfc0);
		for (int i = 0; i < 8; i++)
		{
			h.RunJitPreserveBlockCache();
			if (const std::string d = Diff(ref, Take(h)); !d.empty())
				problems += "after the upload, run " + std::to_string(i) + ": " + d;
		}
		const vuaot::live::Stats s1 = vuaot::live::GetStats();
		EXPECT_EQ(problems, "") << "flagHack=" << fh;
		EXPECT_EQ(s1.tierUps - s0.tierUps, 1u) << "flagHack=" << fh;
		EXPECT_TRUE(vuaot::live::StickyTripped()) << "flagHack=" << fh;
		EXPECT_EQ(vuaot::live::StickyTripCause(), TripCause::ProgramImage) << "flagHack=" << fh;
		EXPECT_EQ(s1.stickyDropped - s0.stickyDropped, 0u) << "a region dropped the sticky byte, flagHack=" << fh;
	}
}

// A trip from another thread (VU0 reading VU1's STATUS) can land after the check that allows a
// body that dropped the sticky byte and before the note that such a body is live, so the trip
// sees nothing to rebuild. The placement looks at the tripwire again after the note: VU1 resets
// at its next run, and every run after the placement equals microVU's, the sticky bits included.
TEST(VuAotLive, TripwireTripDuringAPlacementRebuildsExact)
{
	for (bool fh : {true, false})
	{
		Result ref;
		{
			LiveScope off(false, 4, fh);
			VuTestHarness h(1);
			Prepare(h, StickyLoop(10), SetupSticky, 0);
			h.RunNoDiff();
			ref = Take(h);
		}
		LiveScope on(true, 4, fh);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		vuaot::live::TripAtNextDroppedBodyPlacement();
		std::string problems;
		bool placed = false;
		for (int i = 0; i < 12; i++)
		{
			h.RunJitPreserveBlockCache();
			if (placed)
				if (const std::string d = Diff(ref, Take(h)); !d.empty())
					problems += "after the placement, run " + std::to_string(i) + ": " + d;
			placed = placed || vuaot::live::GetStats().tieredPlaced != s0.tieredPlaced;
		}
		const vuaot::live::Stats s1 = vuaot::live::GetStats();
		ASSERT_TRUE(placed) << "flagHack=" << fh;
		ASSERT_GE(s1.stickyDropped - s0.stickyDropped, 1u) << "the body did not drop the sticky byte, flagHack=" << fh;
		EXPECT_TRUE(vuaot::live::StickyTripped()) << "flagHack=" << fh;
		EXPECT_EQ(s1.stickyTripResets - s0.stickyTripResets, 1u) << "flagHack=" << fh;
		EXPECT_EQ(problems, "") << "flagHack=" << fh;
	}
}

// A savestate load or a settings change (VU1's reset) does not re-arm the tripwire; a VM reset
// or boot does, and the regions drop the byte again.
TEST(VuAotLive, TripwireStaysSetThroughResetsUntilTheVmIsReset)
{
	LiveScope on(true, 4, false);
	VuTestHarness h(1);
	Prepare(h, StickyLoop(10), SetupSticky, 0);
	h.RunNoDiff();
	vuaot::live::TripStickyForTest();
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	CpuMicroVU1.Reset();
	EXPECT_TRUE(vuaot::live::StickyTripped()) << "a reset of VU1 (savestate load, settings change, cache full) keeps it";
	const u64 dropped0 = vuaot::live::GetStats().stickyDropped;
	for (int i = 0; i < 12; i++)
		h.RunJitPreserveBlockCache();
	EXPECT_EQ(vuaot::live::GetStats().stickyDropped, dropped0) << "no region drops the byte while it is set";
	EXPECT_GE(vuaot::live::GetStats().tierUps, 1u);

	vuAotResetStickyTripwire(); // VMManager::Reset / Initialize
	EXPECT_FALSE(vuaot::live::StickyTripped());
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	for (int i = 0; i < 12; i++)
		h.RunJitPreserveBlockCache();
	EXPECT_GT(vuaot::live::GetStats().stickyDropped, dropped0) << "armed again: regions drop the byte";
}

// The counters and the log: the live stats line carries the trip.
TEST(VuAotLive, TripwireIsCountedInTheStatsLine)
{
	LiveScope on(true, 4, false);
	RecompilerTestEnvironment::ResetVuBlockCache(1); // (the setting reaches the tier at a reset)
	const u64 trips0 = vuaot::live::GetStats().stickyTrips;
	std::string line = vuaot::live::FormatStats();
	EXPECT_NE(line.find(" sticky_trips="), std::string::npos) << line;
	EXPECT_NE(line.find(" sticky_trip_resets="), std::string::npos) << line;
	EXPECT_NE(line.find("sticky_tripped=0"), std::string::npos) << line;
	vuaot::live::TripStickyForTest();
	EXPECT_EQ(vuaot::live::GetStats().stickyTrips, trips0 + 1);
	line = vuaot::live::FormatStats();
	EXPECT_NE(line.find(" sticky_trips=" + std::to_string(trips0 + 1) + " "), std::string::npos) << line;
	EXPECT_NE(line.find("sticky_tripped=1"), std::string::npos) << line;
	EXPECT_NE(line.find("sticky tripwire: trips="), std::string::npos) << line;
}

// Each part of the tripwire, broken on purpose, is seen by a test above.
TEST(VuAotLive, TripwireMutationsAreCaught)
{
	struct Case
	{
		int mutation;
		Trigger trig;
		const char* what;
	};
	const Case cases[] = {
		{vuaot::live::TripMask(vuaot::live::kTripMutProgramImage), Trigger::Upload, "no scan of a new program's image"},
		{vuaot::live::TripMask(vuaot::live::kTripMutCompile), Trigger::Compile, "no hook on a compiled sticky reader"},
		{vuaot::live::TripMask(vuaot::live::kTripMutOutside), Trigger::OutsideJit, "microVU0's window access is not noticed"},
		{vuaot::live::TripMask(vuaot::live::kTripMutOutside), Trigger::OutsideInt, "the interpreter's window access is not noticed"},
		{vuaot::live::TripMask(vuaot::live::kTripMutNoRebuild), Trigger::Forced, "the trip does not rebuild the regions"},
		{vuaot::live::TripMask(vuaot::live::kTripMutJobOption), Trigger::Forced, "jobs made after the trip still drop the byte"},
	};
	for (const Case& c : cases)
	{
		const TripOutcome o = RunTripScenario(c.trig, false, c.mutation);
		EXPECT_NE(TripReport(o, c.what), "") << "mutation went unnoticed: " << c.what;
	}
	// a job published after the trip
	const auto published = [](int mutation) {
		Result ref;
		{
			LiveScope off(false, 4, false);
			VuTestHarness h(1);
			Prepare(h, StickyLoop(10), SetupSticky, 0);
			h.RunNoDiff();
			ref = Take(h);
		}
		LiveScope scope(true, 4, false, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		vuaot::live::HoldWorker(true);
		for (int i = 0; i < 6; i++)
			h.RunJitPreserveBlockCache();
		vuaot::live::HoldWorker(false);
		vuaot::live::WaitWorkerIdle();
		vuaot::live::TripStickyForTest();
		vuaot::live::SetTripwireMutation(mutation);
		std::string problems;
		for (int i = 0; i < 4; i++)
		{
			h.RunJitPreserveBlockCache();
			problems += Diff(ref, Take(h));
		}
		return problems;
	};
	EXPECT_EQ(published(0), "");
	// Two rules stand between such a job and a run: it is not built into a body, and a body made with
	// the drop is not placed. Either alone keeps every run exact.
	EXPECT_EQ(published(vuaot::live::TripMask(vuaot::live::kTripMutPublish)), "");
	EXPECT_NE(published(vuaot::live::TripMask(vuaot::live::kTripMutPublish) | vuaot::live::TripMask(vuaot::live::kTripMutPlace)), "")
		<< "a job made with the drop was built and placed after the trip and nobody noticed";
}


// EnableVUAot turns the on-disk VU program cache on. The cache is filled from the recorder's log of each program
// (mVUPersist), which stops growing once a program is tiered (RegisterBlock fails every episode of a program with tier state),
// so the image saved for a program is the microVU code it had before its tier-up. These tests run the whole path: a program is
// compiled, tiered up and saved; a later session loads it from the disk cache, tiers it up again, and every run equals microVU's.

namespace
{
void RecursiveRm(const std::string& dir)
{
	if (!FileSystem::DirectoryExists(dir.c_str()))
	{
		::unlink(dir.c_str());
		return;
	}
	FileSystem::FindResultsArray ents;
	FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &ents);
	for (const auto& e : ents)
	{
		if (e.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
			RecursiveRm(e.FileName);
		else
			::unlink(e.FileName.c_str());
	}
	::rmdir(dir.c_str());
}

// A program with a second way in, after the first one's code, for runs that compile after its tier-up.
constexpr u32 kSecondEntry = 10 * 8;
std::vector<VuOp> TwoEntryLoop(u32 n)
{
	std::vector<VuOp> p = CountedLoop(n); // pairs 0-8, the E bit on the last
	p.push_back(NopPair());               // 9: its delay slot
	p.push_back(U(VADD_U(mask::xyzw, vf::vf10, vf::vf10, vf::vf4))); // 10: the second entry
	p.push_back(P(VFMAND_L(vi::vi6, vi::vi2), VMUL_U(mask::xyz, vf::vf11, vf::vf10, vf::vf10)));
	p.push_back(EBit(U(VSUB_U(mask::xyzw, vf::vf12, vf::vf11, vf::vf3))));
	return p;
}

// A saved image without its lists of compiled ranges and kept-bit words: what the recorder's log made
// (the block records and the chunks of code). The ranges and kept-bit words are the program's own, read
// when the image is written, and grow with every block compiled, a tiered program's included; they say
// which parts of micro memory the program answers for and hold no code. (Layout: mVUPersist::ImageHeader,
// 88 bytes with the kept-bit count at 12 and the range count at 60, then 8 bytes per range, then 8 bytes
// per kept-bit word.)
std::vector<u8> LoggedPart(const std::vector<u8>& image)
{
	constexpr size_t kHeader = 88, kKeptCountAt = 12, kRangeCountAt = 60;
	if (image.size() < kHeader)
		return {};
	u32 ranges = 0, kept = 0;
	std::memcpy(&ranges, image.data() + kRangeCountAt, sizeof(ranges));
	std::memcpy(&kept, image.data() + kKeptCountAt, sizeof(kept));
	const size_t skip = kHeader + (size_t(ranges) + kept) * 8;
	if (image.size() < skip)
		return {};
	return std::vector<u8>(image.begin() + skip, image.end());
}

// What the process boundary does, in one process: the cache's in-memory state is dropped and the disk read again.
void NewProcess()
{
	mVUProgCache::ResetForTest(1);
}

class VuAotCache : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
		char tmpl[] = "/tmp/vuaot_cache_XXXXXX";
		char* d = ::mkdtemp(tmpl);
		ASSERT_NE(d, nullptr);
		root_ = d;
		prevCache_ = EmuFolders::Cache;
		prevProgCache_ = EmuConfig.Cpu.Recompiler.EnableVUProgramCache;
		EmuFolders::Cache = root_;
		// Recording is under the tests' control (RecompilerTestEnvironment): the reset-time sync
		// from the setting does not touch it. The program cache itself follows the setting.
		mVUPersist::SetRecordingEnabled(true);
		NewProcess();
	}

	void TearDown() override
	{
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = prevProgCache_;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		NewProcess();
		mVUPersist::SetRecordingEnabled(false);
		EmuFolders::Cache = prevCache_;
		RecursiveRm(root_);
	}

	// The tier off, the program run once: the answer every later run must give.
	static Result Reference(const std::vector<VuOp>& prog, const std::function<void(VuTestHarness&)>& setup, bool flagHack, u32 startPc = 0)
	{
		const bool savedCache = EmuConfig.Cpu.Recompiler.EnableVUProgramCache;
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
		Result ref;
		{
			LiveScope off(false, 4, flagHack);
			VuTestHarness h(1);
			Prepare(h, prog, setup, 0);
			h.SetStartPc(startPc);
			h.RunNoDiff();
			ref = Take(h);
		}
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = savedCache;
		return ref;
	}

	std::string root_;
	std::string prevCache_;
	bool prevProgCache_ = false;
};
} // namespace

// The tier does not change what a program has saved: the image after the tier-up equals the one before it. A tiered body's
// build runs in block tables that are thrown away, and the recorder keeps none of it. (Mutation: let the recorder keep the
// build's episode, and the regions end up in the image.)
TEST_F(VuAotCache, TierUpDoesNotChangeWhatIsSaved)
{
	for (bool sync : {true, false})
		for (bool fh : {true, false})
		{
			NewProcess();
			LiveScope on(true, 4, fh, vuaot::live::kJumpTierUpRuns, sync);
			const std::vector<VuOp> prog = TwoEntryLoop(20);
			VuTestHarness h(1);
			Prepare(h, prog, SetupLoop, 0);
			h.RunNoDiff();
			ASSERT_TRUE(mVUProgCache::GetStats(1).initialized && mVUProgCache::GetStats(1).enabled)
				<< "EnableVUAot on, EnableVUProgramCache off: the cache is up";
			h.RunJitPreserveBlockCache(); // the compile, untiered
			std::vector<u8> before, afterTier;
			ASSERT_TRUE(mVUPersist::TestSerializeNewestProgram(1, before)) << "the recorder did not log the program";
			ASSERT_GT(before.size(), 88u);
			const u64 tierUps0 = vuaot::live::GetStats().tierUps;
			for (int i = 0; i < 14; i++)
				h.RunJitPreserveBlockCache();
			if (!sync)
			{
				vuaot::live::WaitWorkerIdle();
				for (int i = 0; i < 4; i++)
					h.RunJitPreserveBlockCache();
			}
			EXPECT_GT(vuaot::live::GetStats().tierUps, tierUps0) << "sync " << sync << ": the program tiered up";
			ASSERT_TRUE(mVUPersist::TestSerializeNewestProgram(1, afterTier));
			ASSERT_FALSE(LoggedPart(before).empty());
			EXPECT_EQ(before, afterTier) << "sync " << sync << " flagHack " << fh << ": the tier-up changed the saved image";
		}
}

// A cache filled in one session is loaded in the next: the program is hydrated (no microVU compile), tiers
// up as any program does, and every run equals microVU's. `fill`: who filled it. The tier on (the cache is
// on because of it), or the tier off with the cache's own setting on: the images are the same either way.
TEST_F(VuAotCache, LoadedProgramsTierUpAndStayExact)
{
	struct Prog
	{
		const char* name;
		std::vector<VuOp> ops;
		std::function<void(VuTestHarness&)> setup;
		u32 second; // a second entry to run after the tier-up (0: none): its blocks compile under the tier
	};
	const Prog progs[] = {{"two entries", TwoEntryLoop(20), SetupLoop, kSecondEntry}, {"jumps", JumpLoop(), SetupJump, 0}};
	for (const Prog& pr : progs)
		for (bool fillWithTier : {true, false})
			for (bool fh : {true, false})
			{
				const Result ref = Reference(pr.ops, pr.setup, fh);
				const Result ref2 = pr.second ? Reference(pr.ops, pr.setup, fh, pr.second) : Result{};
				RecompilerTestEnvironment::ResetVuBlockCache(1);
				NewProcess();
				RecursiveRm(root_ + "/vu_jit");
				std::string where = std::string(pr.name) + (fillWithTier ? ", filled by the tier" : ", filled by the cache setting") + (fh ? ", flag hack" : "");

				// session one: compile, tier up (when on), save at the reset
				{
					EmuConfig.Cpu.Recompiler.EnableVUProgramCache = !fillWithTier;
					LiveScope s1(fillWithTier, 4, fh);
					VuTestHarness h(1);
					Prepare(h, pr.ops, pr.setup, 0);
					h.RunNoDiff();
					ASSERT_TRUE(mVUProgCache::GetStats(1).initialized && mVUProgCache::GetStats(1).enabled) << where;
					for (int i = 0; i < 12; i++)
					{
						h.RunJitPreserveBlockCache();
						EXPECT_EQ(Diff(ref, Take(h)), "") << where << ": session one, run " << i;
					}
					if (pr.second)
					{
						h.SetStartPc(pr.second);
						h.RunJitPreserveBlockCache();
						EXPECT_EQ(Diff(ref2, Take(h)), "") << where << ": session one, the second entry";
						h.SetStartPc(0);
					}
					RecompilerTestEnvironment::ResetVuBlockCache(1); // (the save)
					EXPECT_GE(mVUProgCache::GetStats(1).payloadWrites, 1u) << where << ": no payload was written";
				}
				EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;

				// session two: the tier on, the cache read from disk
				NewProcess();
				LiveScope s2(true, 4, fh);
				VuTestHarness h(1);
				Prepare(h, pr.ops, pr.setup, 0);
				const u64 compiles0 = mVUPersist::GetBlockCompileCount(1);
				h.RunNoDiff(); // (the JIT pass loads the program)
				ASSERT_TRUE(mVUProgCache::GetStats(1).enabled) << where;
				mVUProgCache::TestWaitForPreload(1);
				EXPECT_EQ(Diff(ref, Take(h)), "") << where << ": the first run of the loaded program";
				EXPECT_GE(mVUProgCache::GetStats(1).payloadHits, 1u) << where << ": the program was not loaded from the cache";
				EXPECT_EQ(mVUProgCache::GetStats(1).payloadRejects, 0u) << where;
				EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), compiles0) << where << ": the load compiled blocks";
				const vuaot::live::Stats before = vuaot::live::GetStats();
				for (int i = 0; i < 14; i++)
				{
					h.RunJitPreserveBlockCache();
					EXPECT_EQ(Diff(ref, Take(h)), "") << where << ": session two, run " << i;
				}
				if (pr.second)
					for (int i = 0; i < 3; i++)
					{
						h.SetStartPc(pr.second);
						h.RunJitPreserveBlockCache();
						EXPECT_EQ(Diff(ref2, Take(h)), "") << where << ": session two, the second entry, run " << i;
						h.SetStartPc(0);
						h.RunJitPreserveBlockCache();
						EXPECT_EQ(Diff(ref, Take(h)), "") << where << ": session two, the first entry again, run " << i;
					}
				const vuaot::live::Stats after = vuaot::live::GetStats();
				EXPECT_GT(after.tierUps, before.tierUps) << where << ": the loaded program did not tier up";
				EXPECT_GT(after.regions, before.regions) << where << ": no region for the loaded program";
				EXPECT_GE(mVUPersist::GetStats(1).programsHydrated, 1u) << where;
			}
}

namespace
{
// StickyLoop with a reader of the sticky bits at its end.
std::vector<VuOp> StickyReaderLoop(u32 n)
{
	std::vector<VuOp> p = StickyLoop(n);
	p.back() = EBit(P(VFSAND_L(vi::vi4, 0xfc0), VNOP_U()));
	return p;
}
} // namespace

// A program whose image reads the sticky bits of STATUS is loaded from the cache: no microVU compile
// notices the reader, so the scan of the image at program creation does (it runs before the cache is
// asked). The tier then keeps the sticky bits, and every run equals microVU's, byte for byte.
// (Mutation: the scan out of the way, and the tripwire stays armed.)
TEST_F(VuAotCache, LoadedStickyReaderTripsTheWire)
{
	for (int mutation : {0, 1})
		for (bool fh : {true, false})
		{
			const Result ref = Reference(StickyReaderLoop(10), SetupSticky, fh);
			RecompilerTestEnvironment::ResetVuBlockCache(1);
			NewProcess();
			RecursiveRm(root_ + "/vu_jit");
			{
				EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
				LiveScope s1(false, 4, fh);
				VuTestHarness h(1);
				Prepare(h, StickyReaderLoop(10), SetupSticky, 0);
				h.RunNoDiff();
				ASSERT_TRUE(mVUProgCache::GetStats(1).enabled);
				h.RunJitPreserveBlockCache();
				RecompilerTestEnvironment::ResetVuBlockCache(1);
				ASSERT_GE(mVUProgCache::GetStats(1).payloadWrites, 1u);
			}
			EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
			NewProcess();
			LiveScope s2(true, 4, fh);
			if (mutation)
				vuaot::live::SetTripwireMutation(vuaot::live::TripMask(vuaot::live::kTripMutProgramImage));
			VuTestHarness h(1);
			Prepare(h, StickyReaderLoop(10), SetupSticky, 0);
			const u64 compiles0 = mVUPersist::GetBlockCompileCount(1);
			h.RunNoDiff();
			EXPECT_GE(mVUProgCache::GetStats(1).payloadHits, 1u) << "the program was not loaded from the cache";
			EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), compiles0) << "the load compiled blocks";
			if (mutation)
			{
				EXPECT_FALSE(vuaot::live::StickyTripped()) << "with the scan out of the way, nothing else sees a loaded reader (the compile hook has no compile to see)";
				continue;
			}
			ASSERT_TRUE(vuaot::live::StickyTripped()) << "flagHack " << fh << ": the load of a sticky reader did not set the tripwire off";
			EXPECT_EQ(vuaot::live::StickyTripCause(), vuaot::live::TripCause::ProgramImage);
			EXPECT_EQ(Diff(ref, Take(h)), "") << "the first run of the loaded program";
			const vuaot::live::Stats before = vuaot::live::GetStats();
			for (int i = 0; i < 14; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << "flagHack " << fh << " run " << i;
			}
			const vuaot::live::Stats after = vuaot::live::GetStats();
			EXPECT_GT(after.tierUps, before.tierUps);
			EXPECT_EQ(after.stickyDropped, before.stickyDropped) << "no region built after the trip drops the sticky byte";
		}
}


// The tripwire across sessions: a title that set it off starts every later session (a VM boot or reset) with it set, so the
// regions keep the sticky bits from the start and every run equals microVU byte for byte; another title, or the same with the
// program cache off, starts armed. (Mutation: no memory, and session two's regions drop the sticky bits again.)
TEST_F(VuAotCache, TrippedTitleStartsTheNextSessionTripped)
{
	for (bool fh : {true, false})
	{
		const Result ref = Reference(StickyLoop(10), SetupSticky, fh);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		NewProcess();
		RecursiveRm(root_ + "/vu_jit");
		vuAotSetTitle("SLUS-99999", 0x1234ABCD);
		// session one: the tier drops the sticky bits until the tripwire goes off
		{
			LiveScope s1(true, 4, fh);
			EXPECT_FALSE(vuaot::live::StickyTripped()) << "a title with no trip on record starts armed";
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			Prepare(h, StickyLoop(10), SetupSticky, 0);
			h.RunNoDiff();
			for (int i = 0; i < 8; i++)
				h.RunJitPreserveBlockCache();
			EXPECT_GT(vuaot::live::GetStats().stickyDropped, s0.stickyDropped) << "fh " << fh << ": session one dropped the sticky bits";
			vuaot::live::TripStickyForTest();
			ASSERT_TRUE(vuaot::live::StickyTripped());
			RecompilerTestEnvironment::ResetVuBlockCache(1);
		}
		const std::optional<std::string> file = FileSystem::ReadFileToString((root_ + "/vu_jit/vuaot_tripped.txt").c_str());
		ASSERT_TRUE(file.has_value()) << "the trip was not remembered";
		EXPECT_NE(file->find("SLUS-99999 1234ABCD"), std::string::npos) << *file;

		// session two, same title: a new process, then the VM boot (LiveScope re-arms as the boot does)
		NewProcess();
		{
			LiveScope s2(true, 4, fh);
			ASSERT_TRUE(vuaot::live::StickyTripped()) << "fh " << fh << ": the remembered trip did not carry over";
			EXPECT_EQ(vuaot::live::StickyTripCause(), vuaot::live::TripCause::Remembered);
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			Prepare(h, StickyLoop(10), SetupSticky, 0);
			h.RunNoDiff();
			EXPECT_EQ(Diff(ref, Take(h)), "") << "fh " << fh;
			for (int i = 0; i < 10; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << "fh " << fh << " run " << i << ": the regions must keep the sticky bits";
			}
			const vuaot::live::Stats s1 = vuaot::live::GetStats();
			EXPECT_GT(s1.tierUps, s0.tierUps) << "fh " << fh;
			EXPECT_EQ(s1.stickyDropped, s0.stickyDropped) << "fh " << fh << ": session two started with the drop off";
			EXPECT_EQ(s1.stickyTrips, s0.stickyTrips) << "a remembered trip is not a new trip";
			EXPECT_GE(s1.stickyRemembered, 1u);
		}

		// another title starts armed; so does this one with the program cache off (nowhere to remember)
		vuAotSetTitle("SLUS-99998", 0x1234ABCD);
		{
			LiveScope s3(true, 4, fh);
			EXPECT_FALSE(vuaot::live::StickyTripped()) << "fh " << fh << ": another title";
		}
		vuAotSetTitle("SLUS-99999", 0x1234ABCD);
		{
			LiveScope s4(false, 4, fh);
			EXPECT_FALSE(vuaot::live::StickyTripped()) << "fh " << fh << ": with vu-aot (and so the program cache) off";
		}
		vuAotSetTitle("", 0);
	}
}

// A resident 48 KiB table for every jumping block is a cost microVU pays even with vu-aot off: a block meets a handful of
// targets, so most of its table stays zeros. Only the pages its targets fall in may be resident.
TEST(VuAotLive, AJumpTableCostsOnlyThePagesItsTargetsFallIn)
{
	LiveScope off(false, 1000000, false);
	VuTestHarness h(1);
	Prepare(h, JumpLoop(), SetupJump, 0);
	h.RunNoDiff();
	for (int i = 0; i < 4; i++)
		h.RunJitPreserveBlockCache();
	const mvu_test_hooks::JumpTableUsage u = mvu_test_hooks::GetJumpTableUsage();
	ASSERT_GE(u.liveTables, 1u) << "the program has a JR, so a block has a jump table";
#if defined(__linux__)
	constexpr u64 kJumpTableBytes = 24 * 2048;
	EXPECT_LT(u.liveResident, u.liveTables * kJumpTableBytes) << "every jump table is resident in full";
#endif
}

} // namespace recompiler_tests

#endif // arm64
