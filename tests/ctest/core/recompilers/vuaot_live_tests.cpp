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

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/microVU_ProgCache-arm64.h"
#include "arm64/vuaot/VuAotBackground.h"
#include "arm64/vuaot/VuAotLive.h"
#include "arm64/vuaot/VuAotPersist.h"

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
	// `sync`: the deterministic mode (every background job waited for and
	// published where it was asked for), which keeps a test's sequence of
	// compiles the same from run to run.
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
	after.blocksSetAside -= before.blocksSetAside;
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
	EXPECT_EQ(after.bodyBlocks, before.bodyBlocks);
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
			EXPECT_GT(d.blocksSetAside, 0u);
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

// A stub program the dispatcher enters, whose JR goes into code the dispatcher never enters (JumpLoop's
// code, so the overlay's own jumps meet the jump cache and the tier-up comes from inside its running code):
// micro memory changed after the stub's program was made (the first run stops before the JR
// executes, then a word outside its ranges is rewritten), so the search files the jump's
// target as a program of its own. This is God of War II's shape (a stub, and overlays that
// only exist because a JR reached them). Each run is preceded by the micro-memory write that
// nulls the quick slots, so every run's JR takes the slow path.
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
	delta.blocksSetAside -= before.blocksSetAside;
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
		EXPECT_EQ(on.tierUps, 1u) << "the overlay, not the stub (flagHack=" << fh << ")";
		EXPECT_EQ(on.jumpTierUps, 1u);
		EXPECT_GE(on.jumpCounted, 5u);
		EXPECT_GE(on.regions, 1u);
		EXPECT_GT(on.blocksSetAside, 0u);
	}
}

//------------------------------------------------------------------
// The background compile (compiler-design.md 9aa). microVU keeps running a
// program while the worker builds its regions; what the worker made is
// published at a slow path. None of it may change what a run leaves.
//------------------------------------------------------------------

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
	d.jobsGen -= a.jobsGen;
	d.jobsRoot -= a.jobsRoot;
	d.jobsDropped -= a.jobsDropped;
	d.jobsDone -= a.jobsDone;
	d.publishedGen -= a.publishedGen;
	d.publishedRoot -= a.publishedRoot;
	d.discardStale -= a.discardStale;
	d.discardGen -= a.discardGen;
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
			EXPECT_EQ(d.jobsGen, 1u) << "one tier-up asked for, not one per dispatch";
			EXPECT_EQ(d.publishedGen, 0u);
			EXPECT_EQ(d.regions, 0u);
			vuaot::live::HoldWorker(false);
			vuaot::live::WaitWorkerIdle();
			for (int i = 0; i < 6; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << "released, run " << i << " fh=" << fh << " budget=" << budget;
			}
			d = Delta(s0, vuaot::live::GetStats());
			EXPECT_EQ(d.publishedGen, 1u) << "fh=" << fh << " budget=" << budget;
			EXPECT_EQ(d.tierUps, 1u);
			EXPECT_GE(d.regions, 1u);
		}
}

// Micro memory changes under a finished job before it is published (a write the
// program's quick slot did not see, so microVU, here and in the reference, goes on
// running its old code): the job compiled words that are no longer there, and is
// thrown away, never published.
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
		// (the tier-up is asked for again; a job made now would compile the new word
		// for a program whose quick slot never saw it change, which a real write,
		// through mVUclear, cannot leave behind: keep it from running)
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
	EXPECT_EQ(on.jobsGen, 2u) << "the discarded tier-up is asked for again (and held)";
	EXPECT_EQ(on.discardMemory, 1u) << "the job read the word that changed";
	EXPECT_EQ(on.publishedGen, 0u);
	EXPECT_EQ(on.regions, 0u);
}

// Relaxation 3 (9ab) on the worker: with the flag hack on and no sticky STATUS reader in the
// job's copy of micro memory, the region drops the sticky byte. A reader uploaded before the job
// is published (outside the program's ranges and the words its compile read, so the program and
// those words are unchanged) means the region would run under an image that can see the byte: the
// job is thrown away.
TEST(VuAotLive, PublishAfterAStickyReaderAppearsIsDiscarded)
{
	std::vector<VuOp> prog = CountedLoop(10);
	prog.back() = EBit(P(VFSAND_L(vi::vi4, 0x03f), VNOP_U())); // (no sticky bit read)
	const auto sequence = [&](bool on, vuaot::live::Stats& d) {
		std::vector<Result> out;
		LiveScope scope(on, 4, true, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, prog, SetupLoop, 0);
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
		// An FSAND of the sticky bits, far from the program.
		const u32 lower = VFSAND_L(vi::vi5, 0xfc0), upper = VNOP_U();
		std::memcpy(vuRegs[1].Micro + 0x3000, &lower, 4);
		std::memcpy(vuRegs[1].Micro + 0x3004, &upper, 4);
		CpuMicroVU1.Clear(0x3000, 8);
		h.RunJitPreserveBlockCache();
		out.push_back(Take(h));
		vuaot::live::HoldWorker(true); // (the tier-up asked for again stays queued)
		for (int i = 0; i < 3; i++)
		{
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
		}
		d = Delta(s0, vuaot::live::GetStats());
		d.discardSticky = vuaot::live::GetStats().discardSticky - s0.discardSticky;
		d.stickyDropped = vuaot::live::GetStats().stickyDropped - s0.stickyDropped;
		return out;
	};
	vuaot::live::Stats off{}, on{};
	const std::vector<Result> ref = sequence(false, off);
	const std::vector<Result> got = sequence(true, on);
	ASSERT_EQ(ref.size(), got.size());
	for (size_t i = 0; i < ref.size(); i++)
		EXPECT_EQ(Diff(ref[i], got[i]), "") << "run " << i;
	EXPECT_EQ(on.stickyDropped, 1u) << "the job's image had no sticky reader";
	EXPECT_EQ(on.discardSticky, 1u);
	EXPECT_EQ(on.discardMemory, 0u) << "no word the compile read changed";
	EXPECT_EQ(on.publishedGen, 0u);
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
		EXPECT_EQ(d.jobsGen, 2u) << "the tier-up asked for again after the reset (how=" << how << ")";
		EXPECT_EQ(d.publishedGen, 1u) << "how=" << how;
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
	EXPECT_EQ(d.publishedGen, 0u);
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
	EXPECT_EQ(on.jobsGen, kPrograms) << "one tier-up per program";
	EXPECT_EQ(on.publishedGen, kPrograms);
	EXPECT_GE(on.regions, kPrograms);
}

// A region rooted in a program reached by a JR (an overlay, God of War II's shape) is published at the
// JR slow path that has just resolved that program. Publishing a root drops the program's quick slots,
// so the jump cache entry the slow path then fills must not be left as "no program, this entry": the
// inline probe in front of every JR (normJumpCompile) takes an entry whose program equals the quick
// slot's, and does not ask whether that program is null. Such an entry would be taken by the next JR
// to that address whenever the slot is empty, which it is after any micro-memory write over the code
// there: the stub then jumps into the overlay's old code when a new overlay is in place.
//
// The stub at 0 jumps to the overlay at kOverlayPair (as in StubAndOverlay), which tiers up through
// the jump path. A dispatch at a second entry of the overlay (kOverlayEntry2, a counted loop) asks for
// a root job, held in the worker. The stub runs to its JR's block and stops (cycle budget), the job is
// let go, and the stub resumes: its JR misses the jump cache (the overlay's slots were emptied by a
// write) and the slow path publishes the root. Then a different routine is written over the overlay's
// entry and the stub runs again: microVU alone makes a program of the new routine and runs it.
// (Mutation: vuAotRefetch leaves the quick slot empty after a root publication; the last run then
// executes the old overlay.)
namespace
{
constexpr u32 kOverlayEntry2 = 64;
std::vector<VuOp> StubOverlayAndSecondEntry()
{
	std::vector<VuOp> p = StubAndOverlay();
	while (p.size() < kOverlayEntry2)
		p.push_back(U(VNOP_U()));
	std::vector<VuOp> loop = CountedLoop(10);
	loop.back() = EBit(P(VFSAND_L(vi::vi4, 0x03f), VNOP_U())); // (no sticky bit read)
	for (const VuOp& op : loop)
		p.push_back(op);
	return p;
}

// What replaces the overlay's entry: a routine of its own.
void WriteNewOverlay()
{
	const std::vector<VuOp> c = {U(VADD_U(mask::xyzw, vf::vf8, vf::vf1, vf::vf2)), U(VMUL_U(mask::xyzw, vf::vf9, vf::vf8, vf::vf8)),
		EBit(U(VNOP_U())), U(VNOP_U())};
	for (size_t i = 0; i < c.size(); i++)
	{
		std::memcpy(vuRegs[1].Micro + (kOverlayPair + i) * 8 + 0, &c[i].lower, 4);
		std::memcpy(vuRegs[1].Micro + (kOverlayPair + i) * 8 + 4, &c[i].upper, 4);
	}
	CpuMicroVU1.Clear(kOverlayPair * 8, static_cast<u32>(c.size() * 8));
}
} // namespace

TEST(VuAotLive, RootPublishedAtAJumpLeavesNoUncheckedJumpCacheEntry)
{
	for (bool fh : {true, false})
	{
		const auto sequence = [&](bool on, vuaot::live::Stats& d) {
			std::vector<Result> out;
			LiveScope scope(on, 1000000, fh, 5, /*sync=*/false);
			VuTestHarness h(1);
			SetupOverlay(h);
			h.TrackMemWindow(0, 0x4000);
			h.LoadProgram(StubOverlayAndSecondEntry());
			h.RunNoDiff();
			out.push_back(Take(h));
			// The stub's program ends before its JR, and a word nobody executes changes, so the JR's
			// target becomes a program of its own (OverlaySequence).
			h.SetCycleBudget(1);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			h.SetCycleBudget(100000);
			const u32 marker = 0x12345678;
			std::memcpy(vuRegs[1].Micro + 0x3000, &marker, 4);
			// The overlay tiers up through the jump path; each publication waited for.
			for (u32 i = 0; i < 10; i++)
			{
				CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
				h.RunJitPreserveBlockCache();
				out.push_back(Take(h));
				if (on)
					vuaot::live::WaitWorkerIdle();
			}
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			// A dispatch at the overlay's second entry: microVU compiles it, a root job is held.
			JobGate gate;
			if (on)
				gate.Install();
			h.SetStartPc(kOverlayEntry2 * 8);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			if (on)
				gate.WaitEntered();
			// The stub up to its JR's block; the job finishes; the stub resumes into the JR slow path.
			h.SetStartPc(0);
			CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
			h.SetCycleBudget(1);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			if (on)
			{
				vuaot::live::SetWorkerGate(nullptr);
				gate.Release();
				vuaot::live::WaitWorkerIdle();
			}
			h.ContinueJit(100000, 4);
			out.push_back(Take(h));
			d = Delta(s0, vuaot::live::GetStats());
			// A new routine where the overlay was; the stub jumps to it.
			h.SetCycleBudget(100000);
			WriteNewOverlay();
			for (u32 i = 0; i < 3; i++)
			{
				h.RunJitPreserveBlockCache();
				out.push_back(Take(h));
			}
			return out;
		};
		vuaot::live::Stats off{}, on{};
		const std::vector<Result> ref = sequence(false, off);
		const std::vector<Result> got = sequence(true, on);
		ASSERT_EQ(ref.size(), got.size());
		for (size_t i = 0; i < ref.size(); i++)
			EXPECT_EQ(Diff(ref[i], got[i]), "") << "flagHack=" << fh << " run " << i;
		EXPECT_EQ(on.jobsRoot, 1u) << "flagHack=" << fh << ": the second entry asks for one root job";
		EXPECT_EQ(on.publishedRoot, 1u) << "flagHack=" << fh << ": published at the stub's JR";
	}
}

// A root publication empties every quick slot of its program, and the slow path that publishes it puts
// back only the one it is in. mVUclear walks the slots that prog.quickOcc marks occupied instead of the
// array (microVU ABI 62), so the bits of the slots the publication emptied have to go with them. (Mutation:
// vuAotPublishRoot leaves them set. A stale bit is harmless to mVUclear, which drops a slot with no program
// as one that overlaps; what it breaks is the bitmap being exactly the set of occupied slots.)
//
// One program is dispatched at two entries, so two slots hold it. A root job asked for at the second
// entry is held in the worker, released, and published at the next dispatch.
TEST(VuAotLive, RootPublicationKeepsTheQuickOccupancyBitmapInStep)
{
	constexpr u32 kEntry2 = 64;
	std::vector<VuOp> prog = CountedLoop(10);
	while (prog.size() < kEntry2)
		prog.push_back(U(VNOP_U()));
	std::vector<VuOp> loop2 = CountedLoop(6);
	loop2.back() = EBit(P(VFSAND_L(vi::vi4, 0x03f), VNOP_U())); // (no sticky bit read)
	for (const VuOp& op : loop2)
		prog.push_back(op);
	for (bool fh : {true, false})
	{
		LiveScope scope(true, 3, fh, vuaot::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, prog, SetupLoop, 0);
		h.RunNoDiff();
		for (int i = 0; i < 8; i++)
			h.RunJitPreserveBlockCache();
		vuaot::live::WaitWorkerIdle();
		for (int i = 0; i < 4; i++)
			h.RunJitPreserveBlockCache(); // the tier-up is published at one of these
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		JobGate gate;
		gate.Install();
		h.SetStartPc(kEntry2 * 8); // microVU compiles this entry's block as its own; a root job is held
		h.RunJitPreserveBlockCache();
		gate.WaitEntered();
		h.SetStartPc(0);
		h.RunJitPreserveBlockCache();
		EXPECT_TRUE(mvu_test_hooks::QuickOccupancyMatchesScan(1)) << "flagHack=" << fh << ": before the publication";
		vuaot::live::SetWorkerGate(nullptr);
		gate.Release();
		vuaot::live::WaitWorkerIdle();
		for (int i = 0; i < 3; i++)
			h.RunJitPreserveBlockCache();
		const vuaot::live::Stats d = Delta(s0, vuaot::live::GetStats());
		EXPECT_EQ(d.jobsRoot, 1u) << "flagHack=" << fh;
		EXPECT_EQ(d.publishedRoot, 1u) << "flagHack=" << fh;
		EXPECT_TRUE(mvu_test_hooks::QuickOccupancyMatchesScan(1)) << "flagHack=" << fh << ": after the publication";
	}
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
		// Off and back on: a reset in between.
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

//------------------------------------------------------------------
// The sticky-STATUS tripwire (owner decision j, section 9ad). Regions drop
// the sticky bits of STATUS (6-7 and 10-11) in either flag-hack mode while no
// reader of them is known; a reader that shows up -- a micro image holding
// one, a compiled one, VU0 touching VU1's STATUS -- sets the tripwire off, and
// VU1's regions are rebuilt and kept exact.
//------------------------------------------------------------------

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
			d.publishedGen -= s0.publishedGen;
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
		EXPECT_LE(d.publishedGen, 1u) << "flagHack=" << fh;
		EXPECT_EQ(d.stickyTripResets, 0u) << "nothing was built with the drop, so nothing is rebuilt";
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
	EXPECT_NE(published(vuaot::live::TripMask(vuaot::live::kTripMutPublish)), "") << "a job made with the drop was published after the trip and nobody noticed";
}


//------------------------------------------------------------------
// The VU program cache and the tier (compiler-design.md 9ag)
//------------------------------------------------------------------
//
// EnableVUAot turns the on-disk program cache on. The cache stores what microVU compiled, from the
// recorder's log of each program (mVUPersist), and the tier changes none of that: the log stops growing
// when a program is tiered (RegisterBlock fails every episode of a program with tier state), so the image
// saved for a program is the microVU code it had before its tier-up. These tests run the whole path:
// a program is compiled, tiered up and saved; a later session loads it from the disk cache, tiers it up
// again and every run equals microVU's.

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

// A saved image without its list of compiled ranges: what the recorder's log made (the block records and
// the chunks of code). The ranges are the program's own, read when the image is written, and grow with
// every block compiled, a tiered program's included; they say which parts of micro memory the program
// answers for and hold no code. (Layout: mVUPersist::ImageHeader, 88 bytes with the range count at 60,
// then 8 bytes per range.)
std::vector<u8> LoggedPart(const std::vector<u8>& image)
{
	constexpr size_t kHeader = 88, kRangeCountAt = 60;
	if (image.size() < kHeader)
		return {};
	u32 ranges = 0;
	std::memcpy(&ranges, image.data() + kRangeCountAt, sizeof(ranges));
	if (image.size() < kHeader + size_t(ranges) * 8)
		return {};
	return std::vector<u8>(image.begin() + kHeader + size_t(ranges) * 8, image.end());
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

// The tier does not change what a program saves: its image after the tier-up is the image it had before,
// byte for byte, and after more of it compiled under the tier (a second entry, whose root the tier takes
// into a region) the code in the image is the same (the ranges the program answers for have grown).
// (Mutation: let the recorder keep episodes of a tiered program, and the region placed in one is in the
// image.)
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
			std::vector<u8> before, afterTier, afterSecond;
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

			// code compiled now belongs to a tiered program
			const u64 compiles0 = mVUPersist::GetBlockCompileCount(1);
			h.SetStartPc(kSecondEntry);
			h.RunJitPreserveBlockCache();
			if (!sync)
			{
				vuaot::live::WaitWorkerIdle();
				h.RunJitPreserveBlockCache();
			}
			h.SetStartPc(0);
			ASSERT_TRUE(mVUPersist::TestSerializeNewestProgram(1, afterSecond));
			EXPECT_EQ(LoggedPart(before), LoggedPart(afterSecond)) << "sync " << sync << " flagHack " << fh << ": a compile under the tier changed the saved code";
			EXPECT_GT(mVUPersist::GetBlockCompileCount(1), compiles0) << "the second entry compiled blocks under the tier";
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
				// (filled by the tier, the program's saved plan sends its generation as it loads, and the
				// publication compiles the region's root and exits: VuAotCache.SavedPlanTiersUpAtTheFirstRun)
				if (!fillWithTier)
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
				// (filled by the tier, the program installs its saved regions instead: VuAotCache.Installed*)
				EXPECT_GT(after.tierUps + after.aotPrograms, before.tierUps + before.aotPrograms) << where << ": the loaded program did not tier up";
				EXPECT_GT(after.regions + after.aotRegions, before.regions + before.aotRegions) << where << ": no region for the loaded program";
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


//------------------------------------------------------------------
// The saved plan (persistence-design.md, the plan half): with the program cache on, a tiered program's
// regions are saved as their plan (roots, jump targets, the micro-memory words each compile read) in a
// sidecar next to its payload, and a later session sends the jobs to the worker as the program loads.
//------------------------------------------------------------------

namespace
{
// Every sidecar under `dir`.
std::vector<std::string> FindSidecars(const std::string& dir)
{
	std::vector<std::string> out;
	FileSystem::FindResultsArray ents;
	FileSystem::FindFiles(dir.c_str(), "*.vuaot", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE, &ents);
	for (const auto& e : ents)
		out.push_back(e.FileName);
	return out;
}

// The identity a sidecar claims (FileHeader: mvuAbi at 12, content hash at 32, sentinel at 48, build at 64).
vuaot::persist::Identity ClaimedIdentity(const std::vector<u8>& b)
{
	vuaot::persist::Identity id;
	if (b.size() < 80)
		return id;
	std::memcpy(&id.mvuAbi, b.data() + 12, 4);
	std::memcpy(&id.contentHashLo, b.data() + 32, 8);
	std::memcpy(&id.contentHashHi, b.data() + 40, 8);
	std::memcpy(&id.sentinelLo, b.data() + 48, 8);
	std::memcpy(&id.sentinelHi, b.data() + 56, 8);
	std::memcpy(&id.buildLo, b.data() + 64, 8);
	std::memcpy(&id.buildHi, b.data() + 72, 8);
	std::memcpy(&id.layoutHash, b.data() + 28, 4);
	return id;
}

vuaot::live::Stats PlanDelta(const vuaot::live::Stats& a, const vuaot::live::Stats& b)
{
	vuaot::live::Stats d = Delta(a, b);
	d.planLookups -= a.planLookups;
	d.planLoaded -= a.planLoaded;
	d.planRejected -= a.planRejected;
	d.planStale -= a.planStale;
	for (int i = 0; i < 16; i++)
		d.planRejectedBy[i] -= a.planRejectedBy[i];
	d.planGenJobs -= a.planGenJobs;
	d.planRootJobs -= a.planRootJobs;
	d.planRootsStale -= a.planRootsStale;
	d.planRootsCovered -= a.planRootsCovered;
	d.planRootsTraced -= a.planRootsTraced;
	d.planRootsUnplaced -= a.planRootsUnplaced;
	d.planPublishedGen -= a.planPublishedGen;
	d.planPublishedRoot -= a.planPublishedRoot;
	d.planFirstRegions -= a.planFirstRegions;
	d.planFirstRegionRuns -= a.planFirstRegionRuns;
	d.sidecarsQueued -= a.sidecarsQueued;
	return d;
}

// A loaded sidecar is used as a plan only (pcsx2-eerunner --vuaot-no-install): the plan tests below test the
// path a sidecar whose code cannot be installed takes.
struct PlanOnly
{
	PlanOnly() { vuaot::live::SetInstallEnabled(false); }
	~PlanOnly() { vuaot::live::SetInstallEnabled(true); }
};

// Two counted loops, the second entered at its own PC: after the tier-up, a dispatch there is a root
// whose region the builder accepts (it has a loop). The first loop's end reads the sticky bits, so the
// image sets the tripwire off and the regions keep them (runs compare byte for byte with microVU's).
constexpr u32 kLoopEntry2 = 64 * 8;
std::vector<VuOp> TwoLoopProg()
{
	std::vector<VuOp> p = CountedLoop(10);
	while (p.size() < kLoopEntry2 / 8)
		p.push_back(U(VNOP_U()));
	std::vector<VuOp> loop2 = CountedLoop(6);
	loop2.back() = EBit(P(VFSAND_L(vi::vi4, 0x03f), VNOP_U()));
	for (const VuOp& op : loop2)
		p.push_back(op);
	return p;
}

// The sidecars the cache's writer had written when FillPlan's session ended (the session's scope closes
// the cache, and its counters with it, once the tier is back off).
u64 g_planFillSidecarWrites = 0;

// Session one of a plan test: the tier on, TwoLoopProg tiered up, its second entry run under the tier
// (a root job), and the reset that saves. Returns the number of sidecars on disk afterwards.
size_t FillPlan(const std::string& root, bool sync, bool fh, const Result& ref, const Result& ref2)
{
	LiveScope s1(true, 4, fh, vuaot::live::kJumpTierUpRuns, sync);
	VuTestHarness h(1);
	Prepare(h, TwoLoopProg(), SetupLoop, 0);
	h.RunNoDiff();
	for (int i = 0; i < 12; i++)
	{
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(Diff(ref, Take(h)), "") << "session one, run " << i;
		if (!sync)
			vuaot::live::WaitWorkerIdle();
	}
	for (int i = 0; i < 2; i++)
	{
		h.SetStartPc(kLoopEntry2);
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(Diff(ref2, Take(h)), "") << "session one, the second entry";
		if (!sync)
			vuaot::live::WaitWorkerIdle();
	}
	h.SetStartPc(0);
	h.RunJitPreserveBlockCache();
	RecompilerTestEnvironment::ResetVuBlockCache(1); // (the save)
	mVUProgCache::FlushSidecars(1);
	g_planFillSidecarWrites = mVUProgCache::GetStats(1).sidecarWrites;
	return FindSidecars(root + "/vu_jit").size();
}
} // namespace

// A tiered program's plan is saved, and the next session tiers the program up from it as it loads: the
// generation is published at the first runs (the tier-up threshold is out of reach in session two), the
// root plan (the second entry) is published before that entry has ever run, and every run equals microVU's.
// Nothing about the saved plan is code, so nothing it holds can run.
TEST_F(VuAotCache, SavedPlanTiersUpAtTheFirstRun)
{
	const PlanOnly planOnly;
	for (bool sync : {true, false})
		for (bool fh : {true, false})
		{
			const std::string where = std::string(sync ? "sync" : "background") + (fh ? ", flag hack" : "");
			const Result ref = Reference(TwoLoopProg(), SetupLoop, fh);
			const Result ref2 = Reference(TwoLoopProg(), SetupLoop, fh, kLoopEntry2);
			RecompilerTestEnvironment::ResetVuBlockCache(1);
			NewProcess();
			RecursiveRm(root_ + "/vu_jit");
			ASSERT_EQ(FillPlan(root_, sync, fh, ref, ref2), 1u) << where << ": one program, one sidecar";
			EXPECT_GE(g_planFillSidecarWrites, 1u) << where;

			NewProcess();
			LiveScope s2(true, 1000000, fh, 1000000, sync);
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			Prepare(h, TwoLoopProg(), SetupLoop, 0);
			h.RunNoDiff();
			EXPECT_EQ(Diff(ref, Take(h)), "") << where << ": the first run";
			for (int i = 0; i < 4; i++)
			{
				if (!sync)
					vuaot::live::WaitWorkerIdle();
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << where << ": run " << i;
			}
			const vuaot::live::Stats d = PlanDelta(s0, vuaot::live::GetStats());
			EXPECT_GE(d.planLoaded, 1u) << where;
			EXPECT_EQ(d.planRejected, 0u) << where;
			EXPECT_GE(d.planGenJobs, 1u) << where;
			EXPECT_GE(d.planPublishedGen, 1u) << where << ": the generation from the plan was published";
			EXPECT_GE(d.publishedGen, 1u) << where;
			EXPECT_GE(d.planRootJobs, 1u) << where << ": the second entry's root plan was sent";
			EXPECT_GE(d.planPublishedRoot, 1u) << where << ": and published before the entry ran";
			EXPECT_GE(d.planFirstRegions, 1u) << where;
			EXPECT_LE(d.planFirstRegionRuns, 4u * d.planFirstRegions) << where << ": regions within the first runs";
			EXPECT_GE(mVUProgCache::GetStats(1).payloadHits, 1u) << where << ": the payload loaded too";
			for (int i = 0; i < 3; i++)
			{
				h.SetStartPc(kLoopEntry2);
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref2, Take(h)), "") << where << ": the second entry, run " << i;
				h.SetStartPc(0);
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << where << ": the first entry again, run " << i;
			}
			const vuaot::live::Stats d2 = PlanDelta(s0, vuaot::live::GetStats());
			EXPECT_EQ(d2.jobsRoot - d.jobsRoot, 0u) << where << ": the second entry asked for no job of its own";
			EXPECT_EQ(d2.tierUps, d2.planPublishedGen) << where << ": every tier-up came from the plan";
		}
}

// A session that loads a plan and is reset (a savestate load, a settings change) while the root plans'
// jobs are still out saves the plan again with those roots in it: a reset must not write a smaller plan
// over the one the session loaded. (Mutation: save only what was published, and the second session's file
// has the generation alone.)
TEST_F(VuAotCache, ResetBeforeTheRootsArriveKeepsThePlan)
{
	const PlanOnly planOnly;
	const bool fh = true;
	const Result ref = Reference(TwoLoopProg(), SetupLoop, fh);
	const Result ref2 = Reference(TwoLoopProg(), SetupLoop, fh, kLoopEntry2);
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	NewProcess();
	RecursiveRm(root_ + "/vu_jit");
	ASSERT_EQ(FillPlan(root_, true, fh, ref, ref2), 1u);
	const std::string path = FindSidecars(root_ + "/vu_jit").front();
	const auto records = [&]() -> size_t {
		const std::optional<std::vector<u8>> b = FileSystem::ReadBinaryFile(path.c_str());
		vuaot::persist::File f;
		if (!b.has_value() || vuaot::persist::Parse(b->data(), b->size(), ClaimedIdentity(*b), f) != vuaot::persist::ParseResult::Ok)
			return 0;
		return f.records.size();
	};
	const size_t saved = records();
	ASSERT_GE(saved, 2u) << "the plan has its generation and a root";

	NewProcess();
	{
		LiveScope s2(true, 1000000, fh, 1000000, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, TwoLoopProg(), SetupLoop, 0);
		h.RunNoDiff();
		h.RunJitPreserveBlockCache(); // (the load: the generation goes to the worker)
		vuaot::live::WaitWorkerIdle();
		JobGate gate;
		gate.Install();
		h.RunJitPreserveBlockCache(); // the generation is published, its root plan sent and held
		EXPECT_EQ(Diff(ref, Take(h)), "");
		gate.WaitEntered();
		const vuaot::live::Stats st = vuaot::live::GetStats();
		EXPECT_GE(st.planPublishedGen, 1u);
		// The reset saves (the root job is still held), then the job is let go.
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		mVUProgCache::FlushSidecars(1);
		vuaot::live::SetWorkerGate(nullptr);
		gate.Release();
		vuaot::live::WaitWorkerIdle();
	}
	EXPECT_EQ(records(), saved) << "the reset wrote a plan without the root that was still out";
}

// The tripwire across sessions (the owner's default on decision 3): a title that set it off starts every
// later session (a VM boot or reset) with it set, so the regions keep the sticky bits from the start and every
// run equals microVU byte for byte; another title, or the same with the program cache off, starts armed.
// (Mutation: no memory, and session two's regions drop the sticky bits again.)
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

// A generation published at a slow path that has just compiled blocks for another program, still untiered,
// leaves that program's blocks to the persisted-program recorder: the publication runs in a code-cache episode
// of its own. (Before, the recorder dropped the slow path's whole episode once the tiered program's root
// registered in it, and the untiered program's blocks never reached its payload: 0 to 9 episodes, up to 1,161
// blocks, per God of War II scene on a cold launch.)
//
// The overlay tiers up through the jump path and its job is finished but not yet published; then the stub's
// first word changes, so the next dispatch makes a new program for the stub and compiles its blocks, and the
// same slow path publishes the overlay's generation (its code is still in micro memory).
TEST_F(VuAotCache, GenerationPublishedAtAnotherProgramsSlowPathLeavesItsBlocksRecorded)
{
	for (bool fh : {true, false})
	{
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		NewProcess();
		LiveScope scope(true, 1000000, fh, 5, /*sync=*/false);
		VuTestHarness h(1);
		SetupOverlay(h);
		h.TrackMemWindow(0, 0x4000);
		h.LoadProgram(StubAndOverlay());
		h.RunNoDiff();
		h.SetCycleBudget(1);
		h.RunJitPreserveBlockCache();
		h.SetCycleBudget(100000);
		const u32 marker = 0x12345678;
		std::memcpy(vuRegs[1].Micro + 0x3000, &marker, 4);
		JobGate gate;
		gate.Install();
		for (u32 i = 0; i < 8; i++)
		{
			CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
			h.RunJitPreserveBlockCache();
		}
		gate.WaitEntered();
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		vuaot::live::SetWorkerGate(nullptr);
		gate.Release();
		vuaot::live::WaitWorkerIdle();
		// A new stub: same code but its first upper instruction (vf5 += vf2 instead of vf1).
		const u32 upper = VADD_U(mask::xyzw, vf::vf5, vf::vf5, vf::vf2);
		std::memcpy(vuRegs[1].Micro + 4, &upper, 4);
		CpuMicroVU1.Clear(0, 8);
		const mVUPersist::Stats p0 = mVUPersist::GetStats(1);
		h.RunJitPreserveBlockCache();
		const vuaot::live::Stats d = Delta(s0, vuaot::live::GetStats());
		const mVUPersist::Stats p1 = mVUPersist::GetStats(1);
		ASSERT_EQ(d.publishedGen, 1u) << "fh " << fh << ": the overlay's generation was published at the new stub's dispatch";
		std::vector<u8> image;
		EXPECT_TRUE(mVUPersist::TestSerializeNewestProgram(1, image)) << "fh " << fh << ": the new stub's blocks were not recorded";
		EXPECT_GT(p1.blocksRecorded, p0.blocksRecorded) << "fh " << fh;
	}
}

// Deterministic mode (pcsx2-eerunner --vuaot-sync, the unit tests): a root job microVU asks for while a loaded
// plan's root jobs are finished but not yet published takes only its own job off the worker's finished list;
// the plan's go to their program and are published at its next slow path. (Before, the synchronous root path
// threw every other finished job away, so those roots never got their regions.)
//
// Session one saves a plan with the generation at entry A and a root at entry B. Session two first dispatches a
// third entry C: the load sends the generation and, once it is published in that same slow path, the root plan
// for B; then C, which no plan covers, compiles as a synchronous root.
TEST_F(VuAotCache, SynchronousRootJobLeavesPlanRootsToTheirPublication)
{
	const PlanOnly planOnly;
	constexpr u32 kEntryC = 128 * 8;
	std::vector<VuOp> prog = TwoLoopProg();
	while (prog.size() < kEntryC / 8)
		prog.push_back(U(VNOP_U()));
	std::vector<VuOp> loop3 = CountedLoop(5);
	loop3.back() = EBit(P(VFSAND_L(vi::vi4, 0x03f), VNOP_U()));
	for (const VuOp& op : loop3)
		prog.push_back(op);
	for (bool fh : {true, false})
	{
		const Result refA = Reference(prog, SetupLoop, fh), refB = Reference(prog, SetupLoop, fh, kLoopEntry2),
					 refC = Reference(prog, SetupLoop, fh, kEntryC);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		NewProcess();
		RecursiveRm(root_ + "/vu_jit");
		{
			LiveScope s1(true, 4, fh);
			VuTestHarness h(1);
			Prepare(h, prog, SetupLoop, 0);
			h.RunNoDiff();
			for (int i = 0; i < 8; i++)
				h.RunJitPreserveBlockCache();
			h.SetStartPc(kLoopEntry2);
			for (int i = 0; i < 2; i++)
				h.RunJitPreserveBlockCache();
			RecompilerTestEnvironment::ResetVuBlockCache(1);
			mVUProgCache::FlushSidecars(1);
		}
		ASSERT_EQ(FindSidecars(root_ + "/vu_jit").size(), 1u);
		NewProcess();
		LiveScope s2(true, 1000000, fh, 1000000, /*sync=*/true);
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		VuTestHarness h(1);
		Prepare(h, prog, SetupLoop, 0);
		h.SetStartPc(kEntryC);
		h.RunNoDiff();
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(Diff(refC, Take(h)), "") << "fh " << fh << ": entry C";
		h.SetStartPc(0);
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(Diff(refA, Take(h)), "") << "fh " << fh << ": entry A";
		h.SetStartPc(kLoopEntry2);
		for (int i = 0; i < 2; i++)
		{
			h.RunJitPreserveBlockCache();
			EXPECT_EQ(Diff(refB, Take(h)), "") << "fh " << fh << ": entry B, run " << i;
		}
		const vuaot::live::Stats d = PlanDelta(s0, vuaot::live::GetStats());
		EXPECT_GE(d.planRootJobs, 1u) << "fh " << fh;
		// (the program loads twice: RunNoDiff's JIT pass, then its cache reset, which drops what that load sent)
		EXPECT_GE(d.planPublishedRoot, 1u) << "fh " << fh << ": the root plan sent by the load that runs was published";
	}
}

// A sidecar that is damaged, truncated, from another build, or whose read set no longer matches micro memory
// is refused cleanly: no job from it, the payload still loads, the program tiers up by its run count as any
// program does, and every run equals microVU's.
TEST_F(VuAotCache, DamagedOrStaleSidecarIsRefused)
{
	const PlanOnly planOnly;
	const bool fh = true;
	const Result ref = Reference(TwoLoopProg(), SetupLoop, fh);
	const Result ref2 = Reference(TwoLoopProg(), SetupLoop, fh, kLoopEntry2);
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	NewProcess();
	RecursiveRm(root_ + "/vu_jit");
	ASSERT_EQ(FillPlan(root_, true, fh, ref, ref2), 1u);
	const std::string path = FindSidecars(root_ + "/vu_jit").front();
	const std::optional<std::vector<u8>> good = FileSystem::ReadBinaryFile(path.c_str());
	ASSERT_TRUE(good.has_value() && good->size() > 100);

	enum Case
	{
		FlipByte,
		Truncate,
		Empty,
		OtherBuild,
		WrongWord,
		WrongRootWord,
		Garbage,
	};
	for (Case c : {FlipByte, Truncate, Empty, OtherBuild, WrongWord, WrongRootWord, Garbage})
	{
		std::vector<u8> bytes = *good;
		bool overrideBuild = false;
		switch (c)
		{
			case FlipByte: bytes[bytes.size() / 2] ^= 0x40; break;
			case Truncate: bytes.resize(bytes.size() - 13); break;
			case Empty: bytes.resize(3); break;
			case OtherBuild: overrideBuild = true; break;
			case Garbage:
				for (size_t i = 0; i < bytes.size(); i++)
					bytes[i] = static_cast<u8>(i * 131 + 7);
				break;
			case WrongWord:
			case WrongRootWord:
			{
				vuaot::persist::File f;
				ASSERT_EQ(vuaot::persist::Parse(bytes.data(), bytes.size(), ClaimedIdentity(bytes), f), vuaot::persist::ParseResult::Ok);
				const size_t which = (c == WrongWord) ? 0 : f.records.size() - 1;
				ASSERT_TRUE(c == WrongWord || f.records.size() >= 2) << "the plan has a root record";
				ASSERT_FALSE(f.records[which].readValues.empty());
				f.records[which].readValues[0] ^= 0x100; // as if the image there had changed
				bytes = vuaot::persist::Serialize(f);
				break;
			}
		}
		const std::string where = std::string("case ") + std::to_string(int(c));
		ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), bytes.data(), bytes.size()));
		NewProcess();
		if (overrideBuild)
			mVUPersist::TestOverrideBuildId(true, 0x1234, 0x5678);
		{
			LiveScope s2(true, 4, fh, vuaot::live::kJumpTierUpRuns, true);
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			Prepare(h, TwoLoopProg(), SetupLoop, 0);
			h.RunNoDiff();
			EXPECT_EQ(Diff(ref, Take(h)), "") << where;
			for (int i = 0; i < 12; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << where << " run " << i;
			}
			h.SetStartPc(kLoopEntry2);
			h.RunJitPreserveBlockCache();
			EXPECT_EQ(Diff(ref2, Take(h)), "") << where << " second entry";
			h.SetStartPc(0);
			const vuaot::live::Stats d = PlanDelta(s0, vuaot::live::GetStats());
			// (another build's payload is refused too and replaced by the save at the first reset, which the
			// program's next lookup in this session then loads)
			if (c == OtherBuild)
				EXPECT_GE(mVUProgCache::GetStats(1).payloadRejects, 1u) << where << ": another build's payload is refused too";
			else
				EXPECT_GE(mVUProgCache::GetStats(1).payloadHits, 1u) << where << ": the payload still loads";
			EXPECT_GE(d.tierUps, 1u) << where << ": the program tiers up by its run count";
			switch (c)
			{
				case WrongRootWord:
					EXPECT_GE(d.planLoaded, 1u) << where;
					EXPECT_GE(d.planRootsStale, 1u) << where << ": the root plan whose word changed is dropped";
					EXPECT_EQ(d.planRootJobs, 0u) << where;
					break;
				case WrongWord:
					EXPECT_EQ(d.planLoaded, 0u) << where;
					EXPECT_GE(d.planStale, 1u) << where << ": the generation's read set no longer matches";
					EXPECT_EQ(d.planGenJobs, 0u) << where;
					break;
				default:
					EXPECT_EQ(d.planLoaded, 0u) << where;
					EXPECT_GE(d.planRejected, 1u) << where;
					EXPECT_EQ(d.planGenJobs, 0u) << where;
					break;
			}
			if (c == OtherBuild)
				EXPECT_GE(d.planRejectedBy[static_cast<int>(vuaot::persist::ParseResult::OtherBuild)], 1u);
			if (c == FlipByte || c == Truncate)
				EXPECT_GE(d.planRejectedBy[static_cast<int>(vuaot::persist::ParseResult::BadChecksum)], 1u) << where;
		}
		mVUPersist::TestOverrideBuildId(false, 0, 0);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		mVUProgCache::FlushSidecars(1);
	}
}

// Files are good for the build that wrote them only: a payload's image fixups are offsets into that build's
// module, so another build lays the same program's helpers out elsewhere. A launch over files of another build
// refuses the payload and the sidecar, compiles the program as new, and its save replaces both; the next launch of
// that build loads them. (MvuProgCacheDisk.PayloadOfAnotherBuildIsRefusedAndReplaced pins the payload's replacement
// when the new image is the size of the old.)
TEST_F(VuAotCache, FilesOfAnotherBuildAreRefusedAndReplaced)
{
	const bool fh = true;
	const Result ref = Reference(TwoLoopProg(), SetupLoop, fh);
	const Result ref2 = Reference(TwoLoopProg(), SetupLoop, fh, kLoopEntry2);
	NewProcess();
	RecursiveRm(root_ + "/vu_jit");
	struct Launch
	{
		vuaot::live::Stats d;
		u64 installed = 0; // programs whose saved regions went in
		mVUProgCache::Stats cache;
	};
	const auto launch = [&](bool otherBuild) {
		Launch out;
		NewProcess();
		if (otherBuild)
			mVUPersist::TestOverrideBuildId(true, 0x1234, 0x5678);
		{
			LiveScope s(true, 4, fh, vuaot::live::kJumpTierUpRuns, true);
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			Prepare(h, TwoLoopProg(), SetupLoop, 0);
			h.RunNoDiff();
			EXPECT_EQ(Diff(ref, Take(h)), "");
			for (int i = 0; i < 12; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << "run " << i;
			}
			h.SetStartPc(kLoopEntry2);
			h.RunJitPreserveBlockCache();
			EXPECT_EQ(Diff(ref2, Take(h)), "") << "second entry";
			h.SetStartPc(0);
			const vuaot::live::Stats s1 = vuaot::live::GetStats();
			out.d = PlanDelta(s0, s1);
			out.installed = s1.aotPrograms - s0.aotPrograms;
			out.cache = mVUProgCache::GetStats(1);
			RecompilerTestEnvironment::ResetVuBlockCache(1); // (the save)
			mVUProgCache::FlushSidecars(1);
		}
		mVUPersist::TestOverrideBuildId(false, 0, 0);
		return out;
	};
	const Launch fill = launch(false); // this build's files, from an empty cache
	ASSERT_EQ(FindSidecars(root_ + "/vu_jit").size(), 1u);
	EXPECT_EQ(fill.cache.payloadRejects, 0u);
	// (The first dispatch of a launch refuses the files. The reset after it saves the program, replacing them, and
	// the dispatch after that loads what was saved: that is the one payload hit of such a launch.)
	const Launch other = launch(true);
	EXPECT_GE(other.cache.payloadRejects, 1u);
	EXPECT_GE(other.d.planRejectedBy[static_cast<int>(vuaot::persist::ParseResult::OtherBuild)], 1u);
	EXPECT_EQ(other.installed, 0u);
	const Launch again = launch(true);
	EXPECT_EQ(again.cache.payloadRejects, 0u) << "the payload the first launch saved was refused by its own build";
	EXPECT_GE(again.cache.payloadHits, 1u);
	EXPECT_GE(again.installed, 1u) << "the sidecar the first launch saved was not loaded by its own build";
	const Launch back = launch(false);
	EXPECT_GE(back.cache.payloadRejects, 1u) << "the files of the other build were not refused";
	EXPECT_EQ(back.installed, 0u);
}

//------------------------------------------------------------------
// Saved region code (persistence-design.md, the install half): a published job's code with every
// address in it a fixup another process can resolve, and the install of it at load.
//------------------------------------------------------------------

namespace
{
// A loop with a second entry whose own small loop then branches into the first program's root: a root
// region that links to a block of the graph it was built against.
constexpr u32 kLinkEntry = 64 * 8;
std::vector<VuOp> LoopIntoTheFirst()
{
	std::vector<VuOp> p = CountedLoop(10);
	while (p.size() < kLinkEntry / 8)
		p.push_back(U(VNOP_U()));
	p.push_back(L(VIADDIU_L(vi::vi8, vi::vi0, 3)));                                          // 64
	p.push_back(P(VISUBIU_L(vi::vi8, vi::vi8, 1), VADD_U(mask::xyzw, vf::vf13, vf::vf13, vf::vf4))); // 65
	p.push_back(L(VIBNE_L(vi::vi8, vi::vi0, static_cast<s16>(65 - 66 - 1))));               // 66: to 65
	p.push_back(U(VNOP_U()));                                                                // 67: delay slot
	p.push_back(L(VB_L(static_cast<s16>(0 - 68 - 1))));                                      // 68: to 0
	p.push_back(U(VNOP_U()));                                                                // 69: delay slot
	p.push_back(EBit(U(VNOP_U())));                                                          // (never reached; the harness wants an end)
	return p;
}

// A loop that reads VIF's TOP, kicks a GIF packet and ends a program: a C helper, VIF state and VU0's
// VPU_STAT word in a region.
constexpr u32 kTagQword = 0x40;
std::vector<VuOp> KickLoop()
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 4)));
	p.push_back(L(VIADDIU_L(vi::vi5, vi::vi0, kTagQword)));
	p.push_back(P(VXTOP_L(vi::vi6), VADD_U(mask::xyzw, vf::vf8, vf::vf8, vf::vf4)));          // 2: head
	p.push_back(P(VXGKICK_L(vi::vi5), VMUL_U(mask::xyz, vf::vf9, vf::vf8, vf::vf8)));
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VSUB_U(mask::xyzw, vf::vf10, vf::vf9, vf::vf3)));
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(2 - 5 - 1))));
	p.push_back(U(VNOP_U()));                                                                 // delay slot
	p.push_back(EBit(P(VFSAND_L(vi::vi7, 0xfff), VNOP_U())));
	return p;
}

void SetupKick(VuTestHarness& h)
{
	SetupLoop(h);
	// an EOP-only GIF tag
	h.WriteMemU32(kTagQword * 16 + 0, 0x00008000u);
	h.WriteMemU32(kTagQword * 16 + 4, 0);
	h.WriteMemU32(kTagQword * 16 + 8, 0);
	h.WriteMemU32(kTagQword * 16 + 12, 0);
}

// Two loops over vf5, the first reading its w lane and the second never touching it, so the region's home
// maps differ on vf5 and its w lane stays in microVU's location in the second (HomeMap::wMem, whose loads,
// partial stores and transfers address VU state off the pinned base). One region, the whole program.
std::vector<VuOp> WLaneLoops()
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 6)));                                                // 0
	p.push_back(U(VMUL_U(mask::xyz, vf::vf5, vf::vf5, vf::vf2)));                                   // 1: head 1
	p.push_back(U(VADD_U(mask::xyz, vf::vf6, vf::vf6, vf::vf5)));                                   // 2
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMULw_U(mask::x, vf::vf7, vf::vf1, vf::vf5)));       // 3: reads vf5.w
	p.push_back(U(VNOP_U()));                                                                      // 4
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(1 - 5 - 1))));                         // 5 -> 1
	p.push_back(U(VMUL_U(mask::xyz, vf::vf8, vf::vf5, vf::vf2)));                                   // 6: delay slot
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 5)));                                                // 7
	p.push_back(U(VADD_U(mask::xyz, vf::vf5, vf::vf5, vf::vf3)));                                   // 8: head 2
	p.push_back(U(VMADD_U(mask::xyz, vf::vf6, vf::vf5, vf::vf3)));                                  // 9
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMUL_U(mask::x | mask::y, vf::vf8, vf::vf8, vf::vf5))); // 10
	p.push_back(U(VNOP_U()));                                                                      // 11
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(8 - 12 - 1))));                        // 12 -> 8
	p.push_back(U(VSUB_U(mask::xyz, vf::vf5, vf::vf5, vf::vf2)));                                   // 13: delay slot
	p.push_back(P(VSQ_L(mask::xyzw, vf::vf5, vi::vi0, 0), VADD_U(mask::xyzw, vf::vf9, vf::vf5, vf::vf6))); // 14: stores vf5.w
	p.push_back(EBit(P(VFSAND_L(vi::vi4, 0xfff), VNOP_U())));                                      // 15 (a STATUS reader: the sticky bits stay)
	return p;
}

void SetupWLane(VuTestHarness& h)
{
	h.SetVf(1, 1.0f, 2.0f, 3.0f, 4.0f);
	h.SetVf(2, 1.01f, 0.99f, 1.02f, -3.0f);
	h.SetVf(3, 0.25f, -0.5f, 0.125f, 9.0f);
	h.SetVf(5, 1.5f, 2.0f, 2.5f, 7.0f);
	h.SetVf(6, 0.5f, 0.5f, 0.5f, -2.0f);
	h.SetVf(8, 3.0f, 2.0f, 1.0f, 5.0f);
}

// The address-shaped operands of a record's code that no fixup covers: a 64-bit MOVZ/MOVK run whose
// value lands in a mapped page of this process, a B or BL that leaves the code, and any ADRP. It reads
// the instruction words, not the emitters, so a new emitter that bakes in an address shows here.
struct MappedRanges
{
	std::vector<std::pair<u64, u64>> r;
	MappedRanges()
	{
		std::FILE* f = std::fopen("/proc/self/maps", "r");
		if (!f)
			return;
		char line[512];
		while (std::fgets(line, sizeof(line), f))
		{
			unsigned long long a = 0, b = 0;
			if (std::sscanf(line, "%llx-%llx", &a, &b) == 2)
				r.push_back({a, b});
		}
		std::fclose(f);
	}
	bool Has(u64 v) const
	{
		for (const auto& x : r)
			if (v >= x.first && v < x.second)
				return true;
		return false;
	}
};

std::string AuditRecordCode(const vuaot::persist::RegionCode& c, const MappedRanges& maps)
{
	const size_t n = c.code.size() / 4;
	std::vector<u8> covered(n, 0);
	for (const vuaot::persist::Fixup& f : c.fixups)
		for (u32 i = 0; i < vuaot::persist::RegionCode::Span(f); i++)
			if (f.offset / 4 + i < n)
				covered[f.offset / 4 + i] = 1;
	const auto word = [&](size_t i) {
		u32 w;
		std::memcpy(&w, c.code.data() + 4 * i, 4);
		return w;
	};
	std::string out;
	for (size_t i = 0; i < n; i++)
	{
		const u32 w = word(i);
		if (covered[i])
			continue;
		if ((w & 0x9F000000u) == 0x90000000u)
			out += "ADRP at " + std::to_string(4 * i) + "\n";
		if ((w & 0x7C000000u) == 0x14000000u)
		{
			const s64 disp = static_cast<s64>(static_cast<s32>(w << 6) >> 6) * 4;
			const s64 to = static_cast<s64>(4 * i) + disp;
			if (to < 0 || to >= static_cast<s64>(c.code.size()))
				out += "B/BL out of the code at " + std::to_string(4 * i) + "\n";
		}
		// movz xN, #imm, lsl 0, then movk xN, #imm, lsl 16*k
		if ((w & 0xFFE00000u) == 0xD2800000u)
		{
			const u32 rd = w & 31;
			u64 v = (w >> 5) & 0xFFFF;
			size_t k = 1;
			while (i + k < n && k < 4 && (word(i + k) & 0xFFE0001Fu) == (0xF2800000u | (static_cast<u32>(k) << 21) | rd))
			{
				v |= static_cast<u64>((word(i + k) >> 5) & 0xFFFF) << (16 * k);
				k++;
			}
			if (k >= 2 && v >= 0x10000 && maps.Has(v))
				out += "address run at " + std::to_string(4 * i) + "\n";
		}
	}
	return out;
}
} // namespace

// Every job the worker publishes while the program cache keeps sidecars describes its code with a fixup for
// every address in it, of a kind another process can resolve (persistence-design.md section 3): no address
// run, outside branch or ADRP is left uncovered, and C helpers, VU state, the program, the nodes' handles and
// placeholders for microVU's code all show up. (Mutation: drop the recorder's outside runs, and the audit
// finds the C helpers.)
TEST_F(VuAotCache, WorkerRecordsEveryAddressItsRegionsHold)
{
#if !defined(__linux__)
	GTEST_SKIP() << "the audit reads the mapped ranges from /proc/self/maps";
#endif
	const MappedRanges maps;
	ASSERT_FALSE(maps.r.empty());
	struct Case
	{
		const char* name;
		std::vector<VuOp> prog;
		::recompiler_tests::Setup setup;
		std::vector<u32> entries;
	};
	const Case cases[] = {
		{"two loops", TwoLoopProg(), SetupLoop, {0, kLoopEntry2}},
		{"jump loop", JumpLoop(), SetupJump, {0}},
		{"loop into the first", LoopIntoTheFirst(), SetupLoop, {0, kLinkEntry}},
		{"kick loop", KickLoop(), SetupKick, {0}},
		{"w lane loops", WLaneLoops(), SetupWLane, {0}},
	};
	u32 counts[vuaot::persist::kTargetCount] = {};
	u32 wMemJobs = 0;
	for (bool sync : {true, false})
		for (bool fh : {true, false})
			for (const Case& c : cases)
			{
				const std::string where = std::string(c.name) + (sync ? " sync" : " background") + (fh ? " fh" : "");
				RecompilerTestEnvironment::ResetVuBlockCache(1);
				NewProcess();
				(void)vuaot::live::TakeRecordCodeSamples();
				{
					LiveScope s(true, 4, fh, vuaot::live::kJumpTierUpRuns, sync);
					VuTestHarness h(1);
					Prepare(h, c.prog, c.setup, 0);
					h.RunNoDiff();
					for (u32 e : c.entries)
						for (int i = 0; i < 10; i++)
						{
							h.SetStartPc(e);
							h.RunJitPreserveBlockCache();
							if (!sync)
								vuaot::live::WaitWorkerIdle();
						}
				}
				const std::vector<vuaot::live::RecordCodeSample> samples = vuaot::live::TakeRecordCodeSamples();
				EXPECT_GE(samples.size(), 1u) << where << ": no job was published";
				std::printf("[case] %s: %zu jobs\n", where.c_str(), samples.size());
				for (const auto& smp : samples)
				{
					EXPECT_EQ(smp.notPersistable, "") << where;
					EXPECT_EQ(AuditRecordCode(smp.code, maps), "") << where;
					for (const auto& f : smp.code.fixups)
						counts[f.target]++;
					if (smp.wMemHomes)
						wMemJobs++;
				}
			}
	EXPECT_GT(counts[vuaot::persist::kTargetPlaceholder], 0u);
	EXPECT_GT(counts[vuaot::persist::kTargetImage], 0u) << "no C helper or global was recorded";
	EXPECT_GT(counts[vuaot::persist::kTargetProgram], 0u);
	EXPECT_GT(counts[vuaot::persist::kTargetNodeHandle], 0u);
	EXPECT_GT(counts[vuaot::persist::kTargetBlockRef], 0u) << "no root region linked to a block of its program's graph";
	EXPECT_GT(wMemJobs, 0u) << "no audited region kept a w lane in microVU's location";
	std::printf("[record targets] intra %u placeholder %u image %u data %u blockref %u program %u handle %u\n",
		counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
}

namespace
{
struct InstallCase
{
	const char* name;
	std::vector<VuOp> prog;
	::recompiler_tests::Setup setup;
	std::vector<u32> entries;
	std::vector<u32> budgets; // cycle budgets that stop a run inside its regions and still finish in the harness's resumes
};

std::vector<InstallCase> InstallCases()
{
	return {
		{"two loops", TwoLoopProg(), SetupLoop, {0, kLoopEntry2}, {61, 97}},
		{"jump loop", JumpLoop(), SetupJump, {0}, {61, 97, 193}},
		{"loop into the first", LoopIntoTheFirst(), SetupLoop, {0, kLinkEntry}, {61, 97}},
		{"kick loop", KickLoop(), SetupKick, {0}, {3, 5, 9}},
		{"w lane loops", WLaneLoops(), SetupWLane, {0}, {7, 13, 29}},
	};
}

// The tier off: each entry's answer.
std::vector<Result> References(const InstallCase& c, bool fh, u32 budget)
{
	const bool savedCache = EmuConfig.Cpu.Recompiler.EnableVUProgramCache;
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
	std::vector<Result> out;
	for (u32 e : c.entries)
	{
		LiveScope off(false, 4, fh);
		VuTestHarness h(1);
		Prepare(h, c.prog, c.setup, budget);
		h.SetStartPc(e);
		h.RunNoDiff();
		out.push_back(Take(h));
	}
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = savedCache;
	return out;
}

// Session one of an install test: the tier on, each entry run ten times (so the program tiers up and every
// entry's root is built), then the reset that saves. Returns the offset of the last region placed.
size_t FillInstall(const InstallCase& c, bool sync, bool fh, const std::vector<Result>& ref, std::string& problems)
{
	LiveScope s1(true, 4, fh, vuaot::live::kJumpTierUpRuns, sync);
	VuTestHarness h(1);
	Prepare(h, c.prog, c.setup, 0);
	h.RunNoDiff();
	for (size_t k = 0; k < c.entries.size(); k++)
		for (int i = 0; i < 10; i++)
		{
			h.SetStartPc(c.entries[k]);
			h.RunJitPreserveBlockCache();
			if (const std::string d = Diff(ref[k], Take(h)); !d.empty())
				problems += std::string("session one, entry ") + std::to_string(k) + ": " + d;
			if (!sync)
				vuaot::live::WaitWorkerIdle();
		}
	const size_t placed = vuaot::live::LastPlacedOffset();
	RecompilerTestEnvironment::ResetVuBlockCache(1); // (the save)
	mVUProgCache::FlushSidecars(1);
	return placed;
}

vuaot::live::Stats AotDelta(const vuaot::live::Stats& a, const vuaot::live::Stats& b)
{
	vuaot::live::Stats d = PlanDelta(a, b);
	d.aotPrograms -= a.aotPrograms;
	d.aotRecords -= a.aotRecords;
	d.aotRegions -= a.aotRegions;
	d.aotBytes -= a.aotBytes;
	d.aotRootsToPlan -= a.aotRootsToPlan;
	d.aotNoCode -= a.aotNoCode;
	d.declinedLoaded -= a.declinedLoaded;
	d.declinedSkipped -= a.declinedSkipped;
	for (int i = 0; i < 8; i++)
		d.aotRejectedBy[i] -= a.aotRejectedBy[i];
	d.firstRegions -= a.firstRegions;
	d.firstRegionRuns -= a.firstRegionRuns;
	d.bailsEntry -= a.bailsEntry;
	d.bailsLoopHead -= a.bailsLoopHead;
	d.bailCompiles -= a.bailCompiles;
	return d;
}

// Session two: the program loads with its sidecar (after `pad` bytes of other code, so it lands elsewhere),
// each entry runs `runs` times under `budget` and every run must equal the tier off's.
struct InstallRun
{
	vuaot::live::Stats d;  // over the runs after the first (the program loaded again)
	vuaot::live::Stats d0; // over both loads
	size_t placed = 0;
	std::string problems;
};
InstallRun LoadInstall(const InstallCase& c, bool sync, bool fh, const std::vector<Result>& ref, u32 budget, u32 pad, int runs = 4)
{
	InstallRun out;
	NewProcess();
	LiveScope s2(true, 1000000, fh, 1000000, sync);
	const vuaot::live::Stats s0 = vuaot::live::GetStats();
	VuTestHarness h(1);
	Prepare(h, c.prog, c.setup, budget);
	h.SetStartPc(c.entries[0]);
	h.RunNoDiff(); // (loads the program in its JIT pass, then the reset drops it and saves it)
	if (const std::string d = Diff(ref[0], Take(h)); !d.empty())
		out.problems += "the first run: " + d;
	mVUProgCache::FlushSidecars(1);
	const vuaot::live::Stats s1 = vuaot::live::GetStats();
	if (pad)
		vuaot::live::TestPadCodeCache(pad);
	for (size_t k = 0; k < c.entries.size(); k++)
		for (int i = 0; i < runs; i++)
		{
			h.SetStartPc(c.entries[k]);
			h.RunJitPreserveBlockCache();
			if (const std::string d = Diff(ref[k], Take(h)); !d.empty())
				out.problems += std::string("entry ") + std::to_string(k) + " run " + std::to_string(i) + ": " + d;
			if (k == 0 && i == 0)
				out.placed = vuaot::live::LastPlacedOffset();
			if (!sync)
				vuaot::live::WaitWorkerIdle();
		}
	out.d = AotDelta(s1, vuaot::live::GetStats());
	out.d0 = AotDelta(s0, vuaot::live::GetStats());
	return out;
}
} // namespace

// A tiered program's regions are saved with their code, and the next session puts them in place as the
// program loads: no generation job, no root job for a saved root, regions from the very first dispatch, and
// every run equals the tier off's. The code lands at another offset of the code cache than where it was made,
// so every relocation is exercised. (Mutations, run by hand: no program-word patch, no handle patch, block refs
// left at the saved address, one record linked at a time; see compiler-design.md 9ap.)
TEST_F(VuAotCache, InstalledRegionsRunFromTheFirstDispatch)
{
	for (bool sync : {true, false})
		for (bool fh : {true, false})
			for (const InstallCase& c : InstallCases())
			{
				const std::string where = std::string(c.name) + (sync ? " sync" : " background") + (fh ? " fh" : "");
				const std::vector<Result> ref = References(c, fh, 0);
				RecompilerTestEnvironment::ResetVuBlockCache(1);
				NewProcess();
				RecursiveRm(root_ + "/vu_jit");
				std::string problems;
				const size_t madeAt = FillInstall(c, sync, fh, ref, problems);
				EXPECT_EQ(problems, "") << where;
				ASSERT_EQ(FindSidecars(root_ + "/vu_jit").size(), 1u) << where;

				const InstallRun r = LoadInstall(c, sync, fh, ref, 0, 4096 + 64);
				EXPECT_EQ(r.problems, "") << where;
				EXPECT_EQ(r.d.aotPrograms, 1u) << where << ": the program installed its saved regions";
				EXPECT_GE(r.d.aotRecords, c.entries.size()) << where << ": a record per entry's region";
				EXPECT_EQ(r.d.planGenJobs, 0u) << where;
				EXPECT_EQ(r.d.jobsGen, 0u) << where << ": no generation was compiled";
				EXPECT_EQ(r.d.tierUps, 0u) << where;
				EXPECT_EQ(r.d.firstRegionRuns, 0u) << where << ": the regions were there before the first run";
				for (int i = 1; i < vuaot::live::kAotRejects; i++)
					EXPECT_EQ(r.d.aotRejectedBy[i], 0u) << where << ": refused " << vuaot::live::AotRejectName(static_cast<vuaot::live::AotReject>(i));
				EXPECT_NE(r.placed, madeAt) << where << ": the code went in where it was made, which proves nothing about its fixups";
				EXPECT_GE(mVUProgCache::GetStats(1).payloadHits, 1u) << where;
			}
}

// An installed program is found at every start PC it holds a block for, as one compiled by microVU is. A dispatch
// at a start PC walks that PC's list of programs and compares ranges; only a search puts a program in a list, and
// the regions of an installed program take their jumps inside their code, so none of its PCs has been searched. After
// a write to micro memory that misses the program's ranges, the first dispatch at its other entry then found an empty
// list, hashed the changed image and made a new program, which compiled the same code again (God of War II scene 3:
// 145 programs in a second launch against 125 in the first, 16.5 G VU1 instructions against 14.1 G).
// (Mutation: leave the lists alone at install, and the second entry makes a program.)
TEST_F(VuAotCache, InstalledProgramIsFoundAtItsOtherEntryAfterAWriteOutsideItsRanges)
{
	for (bool fh : {true, false})
	{
		const InstallCase c = InstallCases()[0]; // two loops: entries 0 and kLoopEntry2
		ASSERT_EQ(c.entries.size(), 2u);
		const std::vector<Result> ref = References(c, fh, 0);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		NewProcess();
		RecursiveRm(root_ + "/vu_jit");
		std::string problems;
		FillInstall(c, true, fh, ref, problems);
		EXPECT_EQ(problems, "");
		ASSERT_EQ(FindSidecars(root_ + "/vu_jit").size(), 1u);

		NewProcess();
		LiveScope s2(true, 1000000, fh, 1000000, true);
		VuTestHarness h(1);
		Prepare(h, c.prog, c.setup, 0);
		h.SetStartPc(c.entries[0]);
		h.RunNoDiff(); // (loads the program in its JIT pass, then the reset drops it and saves it)
		mVUProgCache::FlushSidecars(1);
		h.SetStartPc(c.entries[0]);
		h.RunJitPreserveBlockCache(); // the program installs from its sidecar
		EXPECT_EQ(Diff(ref[0], Take(h)), "") << "first entry";

		// A write far from the program, then its other entry: the program still answers for the image.
		const vuaot::live::Stats before = vuaot::live::GetStats();
		const u32 marker = 0x12345678;
		std::memcpy(vuRegs[1].Micro + 0x3000, &marker, 4);
		CpuMicroVU1.Clear(0x3000, 8);
		h.SetStartPc(c.entries[1]);
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(Diff(ref[1], Take(h)), "") << "second entry";
		const vuaot::live::Stats d = PlanDelta(before, vuaot::live::GetStats());
		EXPECT_EQ(d.planLookups, 0u) << (fh ? "flag hack: " : "") << "the second entry made a program for the changed image";
	}
}

// The same under cycle budgets that stop the runs inside the regions: their bails read the program word, the
// node handles and the bail slots of installed code, and compile microVU's code for the block they stop in.
TEST_F(VuAotCache, InstalledRegionsBailUnderSmallBudgets)
{
	const bool sync = true;
	for (bool fh : {true, false})
		for (const InstallCase& c : InstallCases())
			for (u32 budget : c.budgets)
			{
				const std::string where = std::string(c.name) + (fh ? " fh" : "") + " budget " + std::to_string(budget);
				const std::vector<Result> ref = References(c, fh, budget);
				RecompilerTestEnvironment::ResetVuBlockCache(1);
				NewProcess();
				RecursiveRm(root_ + "/vu_jit");
				std::string problems;
				FillInstall(c, sync, fh, References(c, fh, 0), problems);
				EXPECT_EQ(problems, "") << where;
				const InstallRun r = LoadInstall(c, sync, fh, ref, budget, 1024, 6);
				EXPECT_EQ(r.problems, "") << where;
				EXPECT_EQ(r.d.aotPrograms, 1u) << where;
				EXPECT_GT(r.d.bailsEntry + r.d.bailsLoopHead, 0u) << where << ": no installed region bailed";
			}
}

// A saved record the session cannot use is refused without anything going in: the program falls back to its
// plan (the generation compiled on the worker at once) or, for a root, that root's plan, and every run equals
// the tier off's. Cases: a word of the generation's read set changed (the whole file is stale), a root's word
// changed (that root alone), a C helper's offset past the module, a block reference's run too short for this
// process's code cache, a file from a build whose struct layout differs.
TEST_F(VuAotCache, UnusableSavedCodeFallsBackToThePlan)
{
	const bool fh = true;
	const InstallCase c = InstallCases()[2]; // loop into the first: a generation, and a root with a block reference
	const std::vector<Result> ref = References(c, fh, 0);
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	NewProcess();
	RecursiveRm(root_ + "/vu_jit");
	std::string problems;
	FillInstall(c, true, fh, ref, problems);
	ASSERT_EQ(problems, "");
	const std::string path = FindSidecars(root_ + "/vu_jit").front();
	const std::optional<std::vector<u8>> good = FileSystem::ReadBinaryFile(path.c_str());
	ASSERT_TRUE(good.has_value());
	{
		vuaot::persist::File f;
		ASSERT_EQ(vuaot::persist::Parse(good->data(), good->size(), ClaimedIdentity(*good), f), vuaot::persist::ParseResult::Ok);
		ASSERT_GE(f.records.size(), 2u);
		ASSERT_TRUE(f.records[0].HasCode() && f.records[1].HasCode());
		bool ref = false;
		for (const auto& fx : f.records[1].code.fixups)
			ref |= fx.target == vuaot::persist::kTargetBlockRef;
		ASSERT_TRUE(ref) << "the root links to a block of the graph";
	}
	enum Case
	{
		GenWord,
		RootWord,
		ImagePastModule,
		NarrowBlockRef,
		OtherLayout,
	};
	for (Case k : {GenWord, RootWord, ImagePastModule, NarrowBlockRef, OtherLayout})
	{
		std::vector<u8> bytes = *good;
		vuaot::persist::File f;
		ASSERT_EQ(vuaot::persist::Parse(bytes.data(), bytes.size(), ClaimedIdentity(bytes), f), vuaot::persist::ParseResult::Ok);
		switch (k)
		{
			case GenWord: f.records[0].readValues[0] ^= 0x100; break;
			case RootWord: f.records[1].readValues[0] ^= 0x100; break;
			case ImagePastModule:
				for (auto& fx : f.records[0].code.fixups)
					if (fx.target == vuaot::persist::kTargetImage)
						fx.value += s64(1) << 40;
				break;
			case NarrowBlockRef:
				for (auto& fx : f.records[1].code.fixups)
					if (fx.target == vuaot::persist::kTargetBlockRef)
						fx.width = 1;
				break;
			case OtherLayout: f.id.layoutHash ^= 1; break;
		}
		bytes = vuaot::persist::Serialize(f);
		ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), bytes.data(), bytes.size()));
		const std::string where = std::string("case ") + std::to_string(int(k));
		const InstallRun r = LoadInstall(c, true, fh, ref, 0, 512);
		EXPECT_EQ(r.problems, "") << where;
		// (the first load meets the edited file; the reset after it saves a good one, which the second load installs)
		const vuaot::live::Stats& d = r.d0;
		switch (k)
		{
			case GenWord:
				EXPECT_GE(d.planStale, 1u) << where << ": the generation's read set no longer matches";
				EXPECT_EQ(d.planGenJobs, 0u) << where;
				break;
			case RootWord:
				EXPECT_GE(d.planRootsStale, 1u) << where << ": the root whose word changed is dropped";
				EXPECT_GE(d.aotPrograms, 1u) << where << ": the generation went in";
				break;
			case ImagePastModule:
				EXPECT_GE(d.aotRejectedBy[vuaot::live::kAotAddress], 1u) << where;
				EXPECT_GE(d.planGenJobs, 1u) << where << ": the plan took over";
				EXPECT_GE(d.planPublishedGen, 1u) << where;
				break;
			case NarrowBlockRef:
				EXPECT_GE(d.aotRejectedBy[vuaot::live::kAotCacheRun], 1u) << where;
				EXPECT_GE(d.aotRootsToPlan, 1u) << where;
				EXPECT_GE(d.planRootJobs, 1u) << where << ": the root's plan was sent";
				break;
			case OtherLayout:
				EXPECT_GE(d.planRejectedBy[static_cast<int>(vuaot::persist::ParseResult::OtherLayout)], 1u) << where;
				EXPECT_EQ(d.planGenJobs, 0u) << where;
				break;
		}
		if (k != GenWord && k != OtherLayout)
			EXPECT_EQ(r.d.aotPrograms, 1u) << where << ": the file the first load's session saved installs";
		ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), good->data(), good->size()));
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		mVUProgCache::FlushSidecars(1);
		ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), good->data(), good->size()));
	}
}

// Regions saved while the sticky drop was allowed are not installed into a session whose tripwire is set: the
// generation's plan compiles exact regions instead, and every run equals microVU's, the sticky bits of STATUS
// included. With the tripwire armed they install, and a trip after the load rebuilds them exact.
TEST_F(VuAotCache, DroppedStickyRecordsStayOutOfATrippedSession)
{
	for (bool fh : {true, false})
	{
		InstallCase c{"sticky loop", StickyLoop(10), SetupSticky, {0}};
		const std::vector<Result> ref = References(c, fh, 0);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		NewProcess();
		RecursiveRm(root_ + "/vu_jit");
		{
			LiveScope s1(true, 4, fh);
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			Prepare(h, c.prog, c.setup, 0);
			h.RunNoDiff();
			for (int i = 0; i < 10; i++)
				h.RunJitPreserveBlockCache();
			ASSERT_GT(vuaot::live::GetStats().stickyDropped, s0.stickyDropped) << "fh " << fh << ": session one dropped the sticky bits";
			RecompilerTestEnvironment::ResetVuBlockCache(1);
			mVUProgCache::FlushSidecars(1);
		}
		// Tripped before the load: refused, exact.
		{
			NewProcess();
			LiveScope s2(true, 1000000, fh, 1000000, true);
			RecompilerTestEnvironment::ResetVuBlockCache(1); // (the tier takes the setting at a reset)
			vuaot::live::TripStickyForTest();
			ASSERT_TRUE(vuaot::live::StickyTripped());
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			Prepare(h, c.prog, c.setup, 0);
			h.RunNoDiff();
			EXPECT_EQ(Diff(ref[0], Take(h)), "") << "fh " << fh;
			const vuaot::live::Stats d = AotDelta(s0, vuaot::live::GetStats());
			EXPECT_EQ(d.aotPrograms, 0u) << "fh " << fh;
			EXPECT_GE(d.aotRejectedBy[vuaot::live::kAotSticky], 1u) << "fh " << fh;
			EXPECT_GE(d.planPublishedGen, 1u) << "fh " << fh << ": the plan compiled the exact regions";
			// (the reset saved the exact regions over the dropped ones; the next load installs those)
			mVUProgCache::FlushSidecars(1);
			for (int i = 0; i < 6; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref[0], Take(h)), "") << "fh " << fh << " run " << i << ": a dropped region ran in a tripped session";
			}
		}
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		mVUProgCache::FlushSidecars(1);
		NewProcess();
		RecursiveRm(root_ + "/vu_jit");
		NewProcess();
		{
			// refill with the drop (the tripped session saved exact regions over the dropped ones)
			LiveScope s1(true, 4, fh);
			VuTestHarness h(1);
			Prepare(h, c.prog, c.setup, 0);
			h.RunNoDiff();
			for (int i = 0; i < 10; i++)
				h.RunJitPreserveBlockCache();
			RecompilerTestEnvironment::ResetVuBlockCache(1);
			mVUProgCache::FlushSidecars(1);
		}
		// Armed: installed; then a trip, and from the next run on exactly microVU's.
		{
			NewProcess();
			LiveScope s2(true, 1000000, fh, 1000000, true);
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			Prepare(h, c.prog, c.setup, 0);
			h.RunNoDiff();
			for (int i = 0; i < 4; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(DiffExceptSticky(ref[0], Take(h)), "") << "fh " << fh << " run " << i;
			}
			const vuaot::live::Stats d = AotDelta(s0, vuaot::live::GetStats());
			EXPECT_GE(d.aotPrograms, 1u) << "fh " << fh << ": armed, the dropped regions install";
			vuaot::live::TripStickyForTest();
			for (int i = 0; i < 6; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref[0], Take(h)), "") << "fh " << fh << " after the trip, run " << i;
			}
			EXPECT_GE(vuaot::live::GetStats().stickyTripResets, s0.stickyTripResets + 1) << "fh " << fh;
		}
	}
}

// The jump cache fault of compiler-design.md 9ah, over a program loaded with its plan: the overlay a stub
// JRs into is loaded from the cache, its generation comes from the plan, and its root plan (a second entry
// the session never runs) is held in the worker, then published at the stub's JR slow path. Then a new
// routine is written over the overlay and the stub jumps there: every run equals microVU alone, so the jump
// cache entry the slow path filled names a program (vuAotRefetch), and nothing the load did wrote a quick
// slot.
TEST_F(VuAotCache, LoadedOverlayRootPublishedAtAJumpLeavesNoUncheckedJumpCacheEntry)
{
	const PlanOnly planOnly;
	for (bool fh : {true, false})
	{
		// session one: the overlay tiers up through the jump path; the second entry becomes a root
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		NewProcess();
		RecursiveRm(root_ + "/vu_jit");
		{
			LiveScope s1(true, 1000000, fh, 5, /*sync=*/false);
			VuTestHarness h(1);
			SetupOverlay(h);
			h.TrackMemWindow(0, 0x4000);
			h.LoadProgram(StubOverlayAndSecondEntry());
			h.RunNoDiff();
			h.SetCycleBudget(1);
			h.RunJitPreserveBlockCache();
			h.SetCycleBudget(100000);
			const u32 marker = 0x12345678;
			std::memcpy(vuRegs[1].Micro + 0x3000, &marker, 4);
			for (u32 i = 0; i < 10; i++)
			{
				CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
				h.RunJitPreserveBlockCache();
				vuaot::live::WaitWorkerIdle();
			}
			h.SetStartPc(kOverlayEntry2 * 8);
			h.RunJitPreserveBlockCache();
			vuaot::live::WaitWorkerIdle();
			h.RunJitPreserveBlockCache();
			h.SetStartPc(0);
			CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
			h.RunJitPreserveBlockCache();
			RecompilerTestEnvironment::ResetVuBlockCache(1);
			mVUProgCache::FlushSidecars(1);
		}
		ASSERT_GE(FindSidecars(root_ + "/vu_jit").size(), 1u) << "flagHack=" << fh;

		const auto sequence = [&](bool on, vuaot::live::Stats& d) {
			std::vector<Result> out;
			NewProcess();
			LiveScope scope(on, 1000000, fh, 1000000, /*sync=*/false);
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			SetupOverlay(h);
			h.TrackMemWindow(0, 0x4000);
			h.LoadProgram(StubOverlayAndSecondEntry());
			h.RunNoDiff();
			out.push_back(Take(h));
			h.SetCycleBudget(1);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			h.SetCycleBudget(100000);
			const u32 marker = 0x12345678;
			std::memcpy(vuRegs[1].Micro + 0x3000, &marker, 4);
			// The stub JRs into the overlay: it loads with its plan, and the generation goes to the worker.
			CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			if (on)
				vuaot::live::WaitWorkerIdle();
			// The generation is published at this run's slow path; the root plan it sends is held.
			JobGate gate;
			if (on)
				gate.Install();
			CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			if (on)
				gate.WaitEntered();
			// The stub up to its JR's block; the job finishes; the stub resumes into the JR slow path.
			CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
			h.SetCycleBudget(1);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			if (on)
			{
				vuaot::live::SetWorkerGate(nullptr);
				gate.Release();
				vuaot::live::WaitWorkerIdle();
			}
			h.ContinueJit(100000, 4);
			out.push_back(Take(h));
			d = PlanDelta(s0, vuaot::live::GetStats());
			// A new routine where the overlay was; the stub jumps to it.
			h.SetCycleBudget(100000);
			WriteNewOverlay();
			for (u32 i = 0; i < 3; i++)
			{
				h.RunJitPreserveBlockCache();
				out.push_back(Take(h));
			}
			return out;
		};
		vuaot::live::Stats off{}, on{};
		const std::vector<Result> ref = sequence(false, off);
		const std::vector<Result> got = sequence(true, on);
		ASSERT_EQ(ref.size(), got.size());
		for (size_t i = 0; i < ref.size(); i++)
			EXPECT_EQ(Diff(ref[i], got[i]), "") << "flagHack=" << fh << " run " << i;
		EXPECT_GE(on.planLoaded, 1u) << "flagHack=" << fh;
		EXPECT_EQ(on.planPublishedGen, 1u) << "flagHack=" << fh << ": the overlay's generation came from its plan";
		EXPECT_GE(on.planRootJobs, 1u) << "flagHack=" << fh << ": the second entry's root plan was sent";
		EXPECT_EQ(on.planPublishedRoot, on.planRootJobs) << "flagHack=" << fh << ": and published (one at the stub's JR)";
	}
}

// A root the builder declines (the second entry of TwoEntryLoop: one straight block to an E bit) is saved with
// the program's sidecar, and a later session that loads it asks for no job when microVU compiles that root:
// the answer was no region, and microVU's own code is what runs either way. Every run equals the tier off's.
TEST_F(VuAotCache, DeclinedRootsAreNotAskedForAgain)
{
	for (bool sync : {true, false})
		for (bool fh : {true, false})
		{
			const std::string where = std::string(sync ? "sync" : "background") + (fh ? " fh" : "");
			const InstallCase c{"two entries", TwoEntryLoop(20), SetupLoop, {0, kSecondEntry}, {}};
			const std::vector<Result> ref = References(c, fh, 0);
			RecompilerTestEnvironment::ResetVuBlockCache(1);
			NewProcess();
			RecursiveRm(root_ + "/vu_jit");
			std::string problems;
			const vuaot::live::Stats f0 = vuaot::live::GetStats();
			FillInstall(c, sync, fh, ref, problems);
			EXPECT_EQ(problems, "") << where;
			EXPECT_GT(vuaot::live::GetStats().jobsDeclined, f0.jobsDeclined) << where << ": session one declined the second entry's root";
			{
				const std::string path = FindSidecars(root_ + "/vu_jit").front();
				const std::optional<std::vector<u8>> b = FileSystem::ReadBinaryFile(path.c_str());
				vuaot::persist::File f;
				ASSERT_TRUE(b.has_value());
				ASSERT_EQ(vuaot::persist::Parse(b->data(), b->size(), ClaimedIdentity(*b), f), vuaot::persist::ParseResult::Ok) << where;
				bool found = false;
				for (const auto& d : f.declinedRoots)
					found |= d.pc == kSecondEntry;
				EXPECT_TRUE(found) << where << ": the declined root is in the sidecar";
			}
			const InstallRun r = LoadInstall(c, sync, fh, ref, 0, 0);
			EXPECT_EQ(r.problems, "") << where;
			EXPECT_EQ(r.d.aotPrograms, 1u) << where;
			EXPECT_GE(r.d.declinedSkipped, 1u) << where;
			EXPECT_EQ(r.d.jobsRoot, 0u) << where << ": the declined root was asked for again";
		}
}

// The jump cache fault of compiler-design.md 9ah, over an overlay whose saved regions are installed: the stub's
// JR loads the overlay from the cache and its generation and root go in at that slow path, before the jump cache
// entry is filled (from the quick slot the search installs afterwards). Then a new routine is written over the
// overlay and the stub jumps there: every run equals microVU alone.
TEST_F(VuAotCache, InstalledOverlayLeavesNoUncheckedJumpCacheEntry)
{
	for (bool fh : {true, false})
	{
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		NewProcess();
		RecursiveRm(root_ + "/vu_jit");
		{
			LiveScope s1(true, 1000000, fh, 5, /*sync=*/false);
			VuTestHarness h(1);
			SetupOverlay(h);
			h.TrackMemWindow(0, 0x4000);
			h.LoadProgram(StubOverlayAndSecondEntry());
			h.RunNoDiff();
			h.SetCycleBudget(1);
			h.RunJitPreserveBlockCache();
			h.SetCycleBudget(100000);
			const u32 marker = 0x12345678;
			std::memcpy(vuRegs[1].Micro + 0x3000, &marker, 4);
			for (u32 i = 0; i < 10; i++)
			{
				CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
				h.RunJitPreserveBlockCache();
				vuaot::live::WaitWorkerIdle();
			}
			h.SetStartPc(kOverlayEntry2 * 8);
			h.RunJitPreserveBlockCache();
			vuaot::live::WaitWorkerIdle();
			h.RunJitPreserveBlockCache();
			h.SetStartPc(0);
			CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
			h.RunJitPreserveBlockCache();
			RecompilerTestEnvironment::ResetVuBlockCache(1);
			mVUProgCache::FlushSidecars(1);
		}
		ASSERT_GE(FindSidecars(root_ + "/vu_jit").size(), 1u) << "flagHack=" << fh;

		const auto sequence = [&](bool on, vuaot::live::Stats& d) {
			std::vector<Result> out;
			NewProcess();
			LiveScope scope(on, 1000000, fh, 1000000, /*sync=*/false);
			const vuaot::live::Stats s0 = vuaot::live::GetStats();
			VuTestHarness h(1);
			SetupOverlay(h);
			h.TrackMemWindow(0, 0x4000);
			h.LoadProgram(StubOverlayAndSecondEntry());
			h.RunNoDiff();
			out.push_back(Take(h));
			h.SetCycleBudget(1);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			h.SetCycleBudget(100000);
			const u32 marker = 0x12345678;
			std::memcpy(vuRegs[1].Micro + 0x3000, &marker, 4);
			// The stub JRs into the overlay: it loads, and its saved regions go in at that slow path.
			for (u32 i = 0; i < 3; i++)
			{
				CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
				h.RunJitPreserveBlockCache();
				out.push_back(Take(h));
			}
			// The stub up to its JR's block, then on into the JR slow path.
			CpuMicroVU1.Clear(kOverlayPair * 8, 0x100);
			h.SetCycleBudget(1);
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
			h.ContinueJit(100000, 4);
			out.push_back(Take(h));
			d = AotDelta(s0, vuaot::live::GetStats());
			// A new routine where the overlay was; the stub jumps to it.
			h.SetCycleBudget(100000);
			WriteNewOverlay();
			for (u32 i = 0; i < 3; i++)
			{
				h.RunJitPreserveBlockCache();
				out.push_back(Take(h));
			}
			return out;
		};
		vuaot::live::Stats off{}, on{};
		const std::vector<Result> ref = sequence(false, off);
		const std::vector<Result> got = sequence(true, on);
		ASSERT_EQ(ref.size(), got.size());
		for (size_t i = 0; i < ref.size(); i++)
			EXPECT_EQ(Diff(ref[i], got[i]), "") << "flagHack=" << fh << " run " << i;
		EXPECT_GE(on.aotPrograms, 1u) << "flagHack=" << fh << ": the overlay installed its saved regions";
		EXPECT_GE(on.aotRecords, 2u) << "flagHack=" << fh << ": its generation and the second entry's root";
		EXPECT_EQ(on.jobsGen, 0u) << "flagHack=" << fh;
	}
}

namespace
{
constexpr const char* kXprocDirEnv = "PCSX2_TEST_VUAOT_XPROC_DIR";
} // namespace

// The child half of the cross-process test: runs only when the parent names a cache directory, fills it with
// every install case's program (flag hack on) and writes its module base next to it.
TEST_F(VuAotCache, CrossProcessChildFill)
{
	const char* dir = std::getenv(kXprocDirEnv);
	if (!dir)
		GTEST_SKIP() << "runs as the child of InstallsRegionsFromAnotherProcess";
	EmuFolders::Cache = dir;
	NewProcess();
	for (const InstallCase& c : InstallCases())
	{
		std::string problems;
		FillInstall(c, true, true, References(c, true, 0), problems);
		EXPECT_EQ(problems, "") << c.name;
		NewProcess();
	}
	u64 imageBase = 0, dataBase = 0;
	mVUPersist::TestGetRegionBases(imageBase, dataBase);
	std::FILE* f = std::fopen((std::string(dir) + "/child-image-base").c_str(), "wb");
	ASSERT_NE(f, nullptr);
	std::fwrite(&imageBase, sizeof(imageBase), 1, f);
	std::fclose(f);
}

// Saved regions made by another process (this binary re-executed: a fresh ASLR draw for the module, the heap
// and the code cache) install here: no rejects, no generation compiled, every run equals the tier off's. The
// image, data-arena, program, handle and block-reference operands all had to move.
TEST_F(VuAotCache, InstallsRegionsFromAnotherProcess)
{
#ifndef __linux__
	GTEST_SKIP() << "spawns this test binary through /proc/self/exe";
#else
	if (std::getenv(kXprocDirEnv))
		GTEST_SKIP() << "already the child";
	char exe[4096];
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	ASSERT_GT(n, 0);
	exe[n] = 0;
	std::vector<std::string> envStore;
	for (char** e = environ; *e; e++)
		envStore.emplace_back(*e);
	envStore.push_back(std::string(kXprocDirEnv) + "=" + root_);
	std::vector<char*> envp;
	for (std::string& e : envStore)
		envp.push_back(e.data());
	envp.push_back(nullptr);
	std::string filter = "--gtest_filter=VuAotCache.CrossProcessChildFill";
	char* argv[] = {exe, filter.data(), nullptr};
	pid_t pid = 0;
	ASSERT_EQ(posix_spawn(&pid, exe, nullptr, nullptr, argv, envp.data()), 0);
	int status = 0;
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child failed, status " << status;

	u64 childBase = 0;
	{
		std::FILE* f = std::fopen((root_ + "/child-image-base").c_str(), "rb");
		ASSERT_NE(f, nullptr);
		ASSERT_EQ(std::fread(&childBase, sizeof(childBase), 1, f), 1u);
		std::fclose(f);
	}
	u64 imageBase = 0, dataBase = 0;
	mVUPersist::TestGetRegionBases(imageBase, dataBase);
	std::printf("[vuaot-xproc] module base here %#llx, in the filling process %#llx\n", (unsigned long long)imageBase,
		(unsigned long long)childBase);
	if (imageBase == childBase)
		GTEST_SKIP() << "both processes loaded the module at the same base (ASLR off?): nothing about relocation is shown";
	EXPECT_EQ(FindSidecars(root_ + "/vu_jit").size(), InstallCases().size());
	for (bool sync : {true, false})
		for (const InstallCase& c : InstallCases())
		{
			const std::string where = std::string(c.name) + (sync ? " sync" : " background");
			const InstallRun r = LoadInstall(c, sync, true, References(c, true, 0), 0, 0);
			EXPECT_EQ(r.problems, "") << where;
			EXPECT_EQ(r.d0.aotPrograms, 2u) << where << ": both loads installed";
			EXPECT_EQ(r.d0.jobsGen, 0u) << where;
			for (int i = 1; i < vuaot::live::kAotRejects; i++)
				EXPECT_EQ(r.d0.aotRejectedBy[i], 0u) << where << ": refused " << vuaot::live::AotRejectName(static_cast<vuaot::live::AotReject>(i));
			EXPECT_EQ(r.d0.planRejected, 0u) << where;
		}
#endif
}

} // namespace recompiler_tests

#endif // arm64
