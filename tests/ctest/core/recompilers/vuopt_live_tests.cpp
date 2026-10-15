// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// vuopt live tier (EmuCore/CPU/Recompiler/EnableVUOpt): the tier that runs
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
#include "arm64/vuopt/VuOptLive.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
	const bool savedOpt = EmuConfig.Cpu.Recompiler.EnableVUOpt;
	const bool savedFlagHack = EmuConfig.Speedhacks.vuFlagHack;
	// `sync`: the deterministic mode (every background job waited for and
	// published where it was asked for), which keeps a test's sequence of
	// compiles the same from run to run.
	LiveScope(bool on, u32 threshold, bool flagHack, u32 jumpThreshold = vuopt::live::kJumpTierUpRuns, bool sync = true)
	{
		EmuConfig.Cpu.Recompiler.EnableVUOpt = on;
		EmuConfig.Speedhacks.vuFlagHack = flagHack;
		// (a test starts with the tripwire armed, as a VM reset leaves it, and nothing broken)
		vuopt::live::ResetStickyTripwire();
		vuopt::live::SetTripwireMutation(0);
		vuopt::live::SetTierUpRuns(threshold);
		vuopt::live::SetJumpTierUpRuns(jumpThreshold);
		vuopt::live::SetSynchronous(sync);
	}
	~LiveScope()
	{
		vuopt::live::SetWorkerGate(nullptr);
		vuopt::live::HoldWorker(false);
		vuopt::live::SetTripwireMutation(0);
		vuopt::live::ResetStickyTripwire();
		EmuConfig.Cpu.Recompiler.EnableVUOpt = savedOpt;
		EmuConfig.Speedhacks.vuFlagHack = savedFlagHack;
		vuopt::live::SetTierUpRuns(vuopt::live::kTierUpRuns);
		vuopt::live::SetJumpTierUpRuns(vuopt::live::kJumpTierUpRuns);
		vuopt::live::SetSynchronous(false);
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
vuopt::live::Stats RunLive(const std::vector<VuOp>& prog, const Setup& setup, bool flagHack, u32 threshold, u32 runs, u32 budget,
	std::string& problems, u32 jumpThreshold = vuopt::live::kJumpTierUpRuns)
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
	const vuopt::live::Stats before = vuopt::live::GetStats();
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
	vuopt::live::Stats after = vuopt::live::GetStats();
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

// The tier is off by default and with it off nothing counts, tiers up or
// compiles: the dispatcher is microVU's.
TEST(VuoptLive, OffByDefaultIsInert)
{
	EXPECT_FALSE(EmuConfig.Cpu.Recompiler.EnableVUOpt);
	LiveScope off(false, 2, true);
	const vuopt::live::Stats before = vuopt::live::GetStats();
	VuTestHarness h(1);
	Prepare(h, CountedLoop(20), SetupLoop, 0);
	h.RunNoDiff();
	for (int i = 0; i < 20; i++)
		h.RunJitPreserveBlockCache();
	EXPECT_FALSE(vuopt::live::IsEnabled());
	EXPECT_FALSE(vuopt::live::WorkerRunning()) << "the tier off starts no worker thread";
	const vuopt::live::Stats after = vuopt::live::GetStats();
	EXPECT_EQ(after.tierUps, before.tierUps);
	EXPECT_EQ(after.rootsCompiled, before.rootsCompiled);
	EXPECT_EQ(after.bodyBlocks, before.bodyBlocks);
}

// A counted loop under a small cycle budget, so the tier-up falls in the
// middle of runs that break and resume: every run before, at and after it must
// equal microVU's.
TEST(VuoptLive, TierUpKeepsEveryRunIdentical)
{
	for (bool fh : {true, false})
		for (u32 budget : {0u, 60u, 23u})
		{
			std::string problems;
			const vuopt::live::Stats d = RunLive(CountedLoop(10), SetupLoop, fh, 4, 12, budget, problems);
			EXPECT_TRUE(problems.empty()) << "flagHack=" << fh << " budget=" << budget << "\n" << problems;
			EXPECT_EQ(d.tierUps, 1u) << "flagHack=" << fh << " budget=" << budget;
			EXPECT_GE(d.regions, 1u) << "flagHack=" << fh << " budget=" << budget;
			if (budget == 23)
				EXPECT_GT(d.bailCompiles, 0u) << "a budget this small must take regions' bails (flagHack=" << fh << ")";
		}
}

// JR/JALR: the targets microVU's jump caches learned before the tier-up are
// handed to the regions as direct edges.
TEST(VuoptLive, JumpTargetsComeFromTheJumpCaches)
{
	for (bool fh : {true, false})
		for (u32 budget : {0u, 40u})
		{
			std::string problems;
			const vuopt::live::Stats d = RunLive(JumpLoop(), SetupJump, fh, 4, 12, budget, problems);
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
TEST(VuoptLive, JumpPathLeavesADispatchedProgramToItsDispatchCount)
{
	for (bool fh : {true, false})
	{
		std::string problems;
		const vuopt::live::Stats d = RunLive(JumpLoop(), SetupJump, fh, 1000000, 12, 0, problems, 6);
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

std::vector<Result> OverlaySequence(bool on, bool flagHack, u32 runs, vuopt::live::Stats& delta)
{
	std::vector<Result> out;
	LiveScope scope(on, 1000000, flagHack, 5);
	const vuopt::live::Stats before = vuopt::live::GetStats();
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
	vuopt::live::Stats after = vuopt::live::GetStats();
	delta = after;
	delta.tierUps -= before.tierUps;
	delta.jumpTierUps -= before.jumpTierUps;
	delta.jumpCounted -= before.jumpCounted;
	delta.regions -= before.regions;
	delta.blocksSetAside -= before.blocksSetAside;
	return out;
}
} // namespace

TEST(VuoptLive, JumpPathTiersUpAnOverlayTheDispatcherNeverEnters)
{
	for (bool fh : {true, false})
	{
		vuopt::live::Stats off{}, on{};
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
		vuopt::live::SetWorkerGate([this] {
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

vuopt::live::Stats Delta(const vuopt::live::Stats& a, const vuopt::live::Stats& b)
{
	vuopt::live::Stats d = b;
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
// next dispatch, and the runs after it are vuopt's, still identical.
TEST(VuoptLive, BackgroundTierUpPublishesAtADispatch)
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
			LiveScope on(true, 4, fh, vuopt::live::kJumpTierUpRuns, /*sync=*/false);
			VuTestHarness h(1);
			Prepare(h, CountedLoop(10), SetupLoop, budget);
			h.RunNoDiff();
			EXPECT_EQ(Diff(ref, Take(h)), "");
			const vuopt::live::Stats s0 = vuopt::live::GetStats();
			vuopt::live::HoldWorker(true);
			for (int i = 0; i < 8; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << "held, run " << i << " fh=" << fh << " budget=" << budget;
			}
			vuopt::live::Stats d = Delta(s0, vuopt::live::GetStats());
			EXPECT_EQ(d.jobsGen, 1u) << "one tier-up asked for, not one per dispatch";
			EXPECT_EQ(d.publishedGen, 0u);
			EXPECT_EQ(d.regions, 0u);
			vuopt::live::HoldWorker(false);
			vuopt::live::WaitWorkerIdle();
			for (int i = 0; i < 6; i++)
			{
				h.RunJitPreserveBlockCache();
				EXPECT_EQ(Diff(ref, Take(h)), "") << "released, run " << i << " fh=" << fh << " budget=" << budget;
			}
			d = Delta(s0, vuopt::live::GetStats());
			EXPECT_EQ(d.publishedGen, 1u) << "fh=" << fh << " budget=" << budget;
			EXPECT_EQ(d.tierUps, 1u);
			EXPECT_GE(d.regions, 1u);
		}
}

// Micro memory changes under a finished job before it is published (a write the
// program's quick slot did not see, so microVU, here and in the reference, goes on
// running its old code): the job compiled words that are no longer there, and is
// thrown away, never published.
TEST(VuoptLive, PublishAfterMicroMemoryChangedIsDiscarded)
{
	const auto sequence = [](bool on, vuopt::live::Stats& d) {
		std::vector<Result> out;
		LiveScope scope(on, 4, true, vuopt::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, CountedLoop(10), SetupLoop, 0);
		h.RunNoDiff();
		out.push_back(Take(h));
		const vuopt::live::Stats s0 = vuopt::live::GetStats();
		vuopt::live::HoldWorker(true);
		for (int i = 0; i < 6; i++)
		{
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
		}
		vuopt::live::HoldWorker(false);
		vuopt::live::WaitWorkerIdle();
		// The loop's subtract (pair 5) becomes a different immediate.
		const u32 lower = VISUBIU_L(vi::vi1, vi::vi1, 2);
		std::memcpy(vuRegs[1].Micro + 5 * 8, &lower, 4);
		h.RunJitPreserveBlockCache();
		out.push_back(Take(h));
		// (the tier-up is asked for again; a job made now would compile the new word
		// for a program whose quick slot never saw it change, which a real write,
		// through mVUclear, cannot leave behind: keep it from running)
		vuopt::live::HoldWorker(true);
		for (int i = 0; i < 3; i++)
		{
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
		}
		d = Delta(s0, vuopt::live::GetStats());
		return out;
	};
	vuopt::live::Stats off{}, on{};
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
TEST(VuoptLive, PublishAfterAStickyReaderAppearsIsDiscarded)
{
	std::vector<VuOp> prog = CountedLoop(10);
	prog.back() = EBit(P(VFSAND_L(vi::vi4, 0x03f), VNOP_U())); // (no sticky bit read)
	const auto sequence = [&](bool on, vuopt::live::Stats& d) {
		std::vector<Result> out;
		LiveScope scope(on, 4, true, vuopt::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, prog, SetupLoop, 0);
		h.RunNoDiff();
		out.push_back(Take(h));
		const vuopt::live::Stats s0 = vuopt::live::GetStats();
		vuopt::live::HoldWorker(true);
		for (int i = 0; i < 6; i++)
		{
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
		}
		vuopt::live::HoldWorker(false);
		vuopt::live::WaitWorkerIdle();
		// An FSAND of the sticky bits, far from the program.
		const u32 lower = VFSAND_L(vi::vi5, 0xfc0), upper = VNOP_U();
		std::memcpy(vuRegs[1].Micro + 0x3000, &lower, 4);
		std::memcpy(vuRegs[1].Micro + 0x3004, &upper, 4);
		CpuMicroVU1.Clear(0x3000, 8);
		h.RunJitPreserveBlockCache();
		out.push_back(Take(h));
		vuopt::live::HoldWorker(true); // (the tier-up asked for again stays queued)
		for (int i = 0; i < 3; i++)
		{
			h.RunJitPreserveBlockCache();
			out.push_back(Take(h));
		}
		d = Delta(s0, vuopt::live::GetStats());
		d.discardSticky = vuopt::live::GetStats().discardSticky - s0.discardSticky;
		d.stickyDropped = vuopt::live::GetStats().stickyDropped - s0.stickyDropped;
		return out;
	};
	vuopt::live::Stats off{}, on{};
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
TEST(VuoptLive, ResetDuringCompileDropsTheJob)
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
		LiveScope on(true, 4, true, vuopt::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, CountedLoop(10), SetupLoop, 0);
		h.RunNoDiff();
		const vuopt::live::Stats s0 = vuopt::live::GetStats();
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
		vuopt::live::SetWorkerGate(nullptr);
		gate.Release();
		vuopt::live::WaitWorkerIdle();
		for (int i = 0; i < 12; i++)
		{
			h.RunJitPreserveBlockCache();
			EXPECT_EQ(Diff(ref, Take(h)), "") << "how=" << how << " after reset, run " << i;
			if (i == 6)
				vuopt::live::WaitWorkerIdle();
		}
		const vuopt::live::Stats d = Delta(s0, vuopt::live::GetStats());
		EXPECT_EQ(d.discardStale, 1u) << "how=" << how;
		EXPECT_EQ(d.jobsGen, 2u) << "the tier-up asked for again after the reset (how=" << how << ")";
		EXPECT_EQ(d.publishedGen, 1u) << "how=" << how;
		EXPECT_GE(d.regions, 1u);
	}
}

// The tier switched off while a job is compiling: the reset that applies the
// setting stops the worker (waiting for the job), nothing it made is published,
// no worker thread is left, and microVU alone runs on.
TEST(VuoptLive, SettingOffDuringCompileStopsTheWorker)
{
	Result ref;
	{
		LiveScope off(false, 4, true);
		VuTestHarness h(1);
		Prepare(h, CountedLoop(10), SetupLoop, 0);
		h.RunNoDiff();
		ref = Take(h);
		EXPECT_FALSE(vuopt::live::WorkerRunning()) << "the tier off has no worker thread";
	}
	LiveScope on(true, 4, true, vuopt::live::kJumpTierUpRuns, /*sync=*/false);
	VuTestHarness h(1);
	Prepare(h, CountedLoop(10), SetupLoop, 0);
	h.RunNoDiff();
	EXPECT_TRUE(vuopt::live::WorkerRunning());
	const vuopt::live::Stats s0 = vuopt::live::GetStats();
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
	EmuConfig.Cpu.Recompiler.EnableVUOpt = false;
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	releaser.join();
	EXPECT_FALSE(vuopt::live::IsEnabled());
	EXPECT_FALSE(vuopt::live::WorkerRunning());
	for (int i = 0; i < 6; i++)
	{
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(Diff(ref, Take(h)), "") << "run " << i;
	}
	const vuopt::live::Stats d = Delta(s0, vuopt::live::GetStats());
	EXPECT_EQ(d.publishedGen, 0u);
	EXPECT_EQ(d.regions, 0u);
}

// Many programs reach the threshold at once, more of them than the worker gets
// through before they are dispatched again: one job each (none asked for twice
// while one is pending), every run of every program identical to microVU's
// whether its region is published yet or not, and all published in the end.
TEST(VuoptLive, ManyConcurrentRequests)
{
	constexpr u32 kPrograms = 24;
	std::vector<std::vector<VuOp>> progs;
	for (u32 k = 0; k < kPrograms; k++)
		progs.push_back(CountedLoop(5 + k));
	const auto sequence = [&](bool on, vuopt::live::Stats& d) {
		std::vector<Result> out;
		LiveScope scope(on, 2, true, vuopt::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, progs[0], SetupLoop, 0);
		h.RunNoDiff();
		const vuopt::live::Stats s0 = vuopt::live::GetStats();
		for (u32 round = 0; round < 8; round++)
		{
			for (u32 k = 0; k < kPrograms; k++)
			{
				WriteMicro(progs[k]);
				h.RunJitPreserveBlockCache();
				out.push_back(Take(h));
			}
			if (round == 5)
				vuopt::live::WaitWorkerIdle();
		}
		d = Delta(s0, vuopt::live::GetStats());
		return out;
	};
	vuopt::live::Stats off{}, on{};
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

// A tiered program survives a settings round trip: switching the tier off and
// on again resets VU1 (every program, with the tier's state for it, is freed)
// and the next runs tier it up again.
TEST(VuoptLive, SettingsRoundTrip)
{
	LiveScope on(true, 3, true);
	for (int round = 0; round < 3; round++)
	{
		const vuopt::live::Stats before = vuopt::live::GetStats();
		VuTestHarness h(1);
		Prepare(h, CountedLoop(50), SetupLoop, 0);
		h.RunNoDiff();
		const Result ref = Take(h);
		for (int i = 0; i < 8; i++)
		{
			h.RunJitPreserveBlockCache();
			EXPECT_EQ(Diff(ref, Take(h)), "") << "round " << round << " run " << i;
		}
		EXPECT_EQ(vuopt::live::GetStats().tierUps - before.tierUps, 1u) << "round " << round;
		// Off and back on: a reset in between.
		EmuConfig.Cpu.Recompiler.EnableVUOpt = false;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		EXPECT_FALSE(vuopt::live::IsEnabled());
		EmuConfig.Cpu.Recompiler.EnableVUOpt = true;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		EXPECT_TRUE(vuopt::live::IsEnabled());
	}
}

// The gamefixes vuopt does not model keep the tier from running at all.
TEST(VuoptLive, UnsupportedGamefixKeepsItOff)
{
	LiveScope on(true, 2, true);
	const bool savedIbit = EmuConfig.Gamefixes.IbitHack;
	EmuConfig.Gamefixes.IbitHack = true;
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	EXPECT_FALSE(vuopt::live::IsEnabled());
	EmuConfig.Gamefixes.IbitHack = savedIbit;
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	EXPECT_TRUE(vuopt::live::IsEnabled());
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
using vuopt::live::TripCause;

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

// Everything but the sticky bits of STATUS: the register's bits 6-7, the low byte of its ring
// copies. What the contract frees while the tripwire is armed.
std::string DiffExceptSticky(const Result& a, const Result& b)
{
	Result x = a, y = b;
	for (Result* r : {&x, &y})
	{
		r->snap.regs.VI[REG_STATUS_FLAG].UL &= ~0xC0u;
		for (u32& w : r->snap.regs.micro_statusflags)
			w &= ~0xFFu;
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
	vuopt::live::Stats d; // counters over the trip and what followed
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
	const vuopt::live::Stats s0 = vuopt::live::GetStats();
	bool sawSticky = false;
	const auto expectDropped = [&](int runs, const char* when) {
		for (int i = 0; i < runs; i++)
		{
			h.RunJitPreserveBlockCache();
			const Result got = Take(h);
			if (const std::string d = DiffExceptSticky(ref, got); !d.empty())
				out.problems += std::string(when) + " run " + std::to_string(i) + ": " + d;
			sawSticky |= StickyBitsDiffer(ref, got, 0xC0);
		}
	};
	expectDropped(10, "armed");
	if (vuopt::live::GetStats().stickyDropped == s0.stickyDropped)
		out.problems += "no region dropped the sticky byte before the trip\n";
	if (!sawSticky)
		out.problems += "the drop was not visible in the sticky zero/sign bits\n";
	if (vuopt::live::StickyTripped())
		out.problems += "tripped before any reader\n";

	vuopt::live::SetTripwireMutation(mutation);
	TripCause want = TripCause::None;
	switch (trig)
	{
		case Trigger::Upload:
		case Trigger::Compile:
		{
			if (trig == Trigger::Compile)
				vuopt::live::SetTripwireMutation(mutation | vuopt::live::TripMask(vuopt::live::kTripMutProgramImage));
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
				vuopt::live::SetTripwireMutation(vuopt::live::TripMask(vuopt::live::kTripMutOutside));
				h0.RunNoDiff();
				vuopt::live::SetTripwireMutation(mutation);
				h0.RunJitPreserveBlockCache();
			}
			want = TripCause::OutsideRead;
			break;
		}
		case Trigger::Forced:
			vuopt::live::TripStickyForTest();
			want = TripCause::Forced;
			break;
	}
	if (!vuopt::live::StickyTripped())
		out.problems += "the tripwire did not go off\n";
	else if (vuopt::live::StickyTripCause() != want)
		out.problems += std::string("tripped by the wrong thing: ") + vuopt::live::TripCauseName(vuopt::live::StickyTripCause()) + "\n";

	// From the next run on: exactly microVU's, the sticky bits included, and the tier tiers up again
	// (with the drop off).
	const vuopt::live::Stats s1 = vuopt::live::GetStats();
	for (int i = 0; i < 12; i++)
	{
		h.RunJitPreserveBlockCache();
		if (const std::string d = Diff(ref, Take(h)); !d.empty())
			out.problems += "after the trip, run " + std::to_string(i) + ": " + d;
	}
	out.d = vuopt::live::GetStats();
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
TEST(VuoptLive, TripwireImageUploadRebuildsExact)
{
	for (bool fh : {true})
	{
		const TripOutcome o = RunTripScenario(Trigger::Upload, fh, 0);
		EXPECT_EQ(TripReport(o, "upload"), "") << "flagHack=" << fh;
	}
}

// ... and the compile alone notices it too (the scan of a new program's image out of the way).
TEST(VuoptLive, TripwireCompiledReaderRebuildsExact)
{
	for (bool fh : {true})
	{
		const TripOutcome o = RunTripScenario(Trigger::Compile, fh, 0);
		EXPECT_EQ(TripReport(o, "compile"), "") << "flagHack=" << fh;
	}
}

// Anything that selects one of the sticky bits trips it, the divide's sticky bits (10 and 11) alone
// included; a reader of the current bits only does not.
TEST(VuoptLive, TripwireEveryStickyBitTripsAndTheCurrentBitsDoNot)
{
	for (u32 imm : {0x040u, 0x080u, 0x100u, 0x200u, 0x400u, 0x800u, 0xc00u, 0xfffu})
	{
		LiveScope on(true, 4, true);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		EXPECT_FALSE(vuopt::live::StickyTripped());
		vuopt::live::SetTripwireMutation(vuopt::live::TripMask(vuopt::live::kTripMutProgramImage)); // (the compile hook alone)
		WriteReaderImage(kReaderAt, imm);
		h.SetStartPc(kReaderAt);
		h.RunJitPreserveBlockCache();
		EXPECT_TRUE(vuopt::live::StickyTripped()) << "imm12=" << std::hex << imm;
		EXPECT_EQ(vuopt::live::StickyTripCause(), TripCause::Compile) << "imm12=" << std::hex << imm;
	}
	for (u32 imm : {0x000u, 0x001u, 0x03fu, 0x030u, 0x03cu})
	{
		LiveScope on(true, 4, true);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		WriteReaderImage(kReaderAt, imm);
		h.SetStartPc(kReaderAt);
		h.RunJitPreserveBlockCache();
		EXPECT_FALSE(vuopt::live::StickyTripped()) << "imm12=" << std::hex << imm;
	}
}

// VU0 reads VU1's STATUS register through its window onto VU1's registers: from microVU0's code
// and from the interpreter.
TEST(VuoptLive, TripwireOutsideReadRebuildsExact)
{
	for (bool fh : {true})
	{
		TripOutcome o = RunTripScenario(Trigger::OutsideJit, fh, 0);
		EXPECT_EQ(TripReport(o, "outside, jit"), "") << "flagHack=" << fh;
		o = RunTripScenario(Trigger::OutsideInt, fh, 0);
		EXPECT_EQ(TripReport(o, "outside, interpreter"), "") << "flagHack=" << fh;
	}
}

// The other quadwords of the window, VU1's own accesses and VU0's own memory are not readers of
// VU1's STATUS.
TEST(VuoptLive, TripwireOrdinaryAccessesDoNotTrip)
{
	LiveScope on(true, 4, true);
	const vuopt::live::Stats base = vuopt::live::GetStats();
	VuTestHarness h1(1);
	Prepare(h1, StickyLoop(10), SetupSticky, 0);
	h1.RunNoDiff();
	for (int i = 0; i < 8; i++)
		h1.RunJitPreserveBlockCache();
	EXPECT_GE(vuopt::live::GetStats().stickyDropped, 1u);
	EXPECT_FALSE(vuopt::live::StickyTripped()) << "a program with no reader";

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
		ASSERT_FALSE(vuopt::live::StickyTripped()) << "quadword " << q << " of the window";
	}
	// VU0's own memory, and the same indices without the window bit
	for (u32 q : {0u, 16u, 48u, 0x1f0u, 0x3f0u})
	{
		VuTestHarness h0(0);
		h0.SetVi(vi::vi1, q);
		h0.LoadProgram({VuOp{VILW_L(mask::x, vi::vi5, vi::vi1, 0), VNOP_U()}, EBitNopPair()});
		h0.RunNoDiff();
		h0.RunJitPreserveBlockCache();
		ASSERT_FALSE(vuopt::live::StickyTripped()) << "VU0 memory " << q;
	}
	// the interpreter's address function, VU1's own and a window access that is not VI[16]
	GET_VU_MEM(&vuRegs[1], 0x4300);
	GET_VU_MEM(&vuRegs[0], 0x2f0);
	GET_VU_MEM(&vuRegs[0], 0x42f0);
	GET_VU_MEM(&vuRegs[0], 0x4310);
	EXPECT_FALSE(vuopt::live::StickyTripped());
	GET_VU_MEM(&vuRegs[0], 0x4300);
	EXPECT_TRUE(vuopt::live::StickyTripped());
	EXPECT_EQ(vuopt::live::StickyTripCause(), TripCause::OutsideRead);
	const vuopt::live::Stats now = vuopt::live::GetStats();
	EXPECT_EQ(now.stickyTrips - base.stickyTrips, 1u) << "the one trip of the test";
	EXPECT_EQ(now.stickyTripsBy[static_cast<u32>(TripCause::OutsideRead)] - base.stickyTripsBy[static_cast<u32>(TripCause::OutsideRead)], 1u);
}

// A job made while the tripwire was armed finishes after it went off: the region it built does not
// drop the byte of a program that is now known to have readers, so it is not published, and the
// runs equal microVU's throughout.
TEST(VuoptLive, TripwireJobMadeBeforeItIsNotPublished)
{
	for (bool fh : {true})
	{
		const auto sequence = [&](int mutation, bool on, std::string& problems, vuopt::live::Stats& d) {
			Result ref;
			{
				LiveScope off(false, 4, fh);
				VuTestHarness h(1);
				Prepare(h, StickyLoop(10), SetupSticky, 0);
				h.RunNoDiff();
				ref = Take(h);
			}
			LiveScope scope(on, 4, fh, vuopt::live::kJumpTierUpRuns, /*sync=*/false);
			VuTestHarness h(1);
			Prepare(h, StickyLoop(10), SetupSticky, 0);
			h.RunNoDiff();
			const vuopt::live::Stats s0 = vuopt::live::GetStats();
			vuopt::live::HoldWorker(true);
			for (int i = 0; i < 6; i++)
			{
				h.RunJitPreserveBlockCache();
				if (const std::string x = Diff(ref, Take(h)); !x.empty())
					problems += "queued, run " + std::to_string(i) + ": " + x;
			}
			vuopt::live::HoldWorker(false);
			vuopt::live::WaitWorkerIdle();
			vuopt::live::TripStickyForTest(); // the job is done and not yet published
			vuopt::live::SetTripwireMutation(mutation);
			for (int i = 0; i < 4; i++)
			{
				h.RunJitPreserveBlockCache();
				if (const std::string x = Diff(ref, Take(h)); !x.empty())
					problems += "after the trip, run " + std::to_string(i) + ": " + x;
			}
			d = vuopt::live::GetStats();
			d.discardSticky -= s0.discardSticky;
			d.publishedGen -= s0.publishedGen;
			d.stickyDropped -= s0.stickyDropped;
			d.stickyTripResets -= s0.stickyTripResets;
		};
		std::string problems;
		vuopt::live::Stats d{};
		sequence(0, true, problems, d);
		EXPECT_EQ(problems, "") << "flagHack=" << fh;
		EXPECT_EQ(d.stickyDropped, 1u) << "the job was made with the drop";
		EXPECT_EQ(d.discardSticky, 1u) << "flagHack=" << fh;
		EXPECT_EQ(d.publishedGen, 0u) << "flagHack=" << fh;
		EXPECT_EQ(d.stickyTripResets, 0u) << "nothing was built with the drop, so nothing is rebuilt";
	}
}

// A savestate load or a settings change (VU1's reset) does not re-arm the tripwire; a VM reset
// or boot does, and the regions drop the byte again.
TEST(VuoptLive, TripwireStaysSetThroughResetsUntilTheVmIsReset)
{
	LiveScope on(true, 4, true);
	VuTestHarness h(1);
	Prepare(h, StickyLoop(10), SetupSticky, 0);
	h.RunNoDiff();
	vuopt::live::TripStickyForTest();
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	CpuMicroVU1.Reset();
	EXPECT_TRUE(vuopt::live::StickyTripped()) << "a reset of VU1 (savestate load, settings change, cache full) keeps it";
	const u64 dropped0 = vuopt::live::GetStats().stickyDropped;
	for (int i = 0; i < 12; i++)
		h.RunJitPreserveBlockCache();
	EXPECT_EQ(vuopt::live::GetStats().stickyDropped, dropped0) << "no region drops the byte while it is set";
	EXPECT_GE(vuopt::live::GetStats().tierUps, 1u);

	vuoptResetStickyTripwire(); // VMManager::Reset / Initialize
	EXPECT_FALSE(vuopt::live::StickyTripped());
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	for (int i = 0; i < 12; i++)
		h.RunJitPreserveBlockCache();
	EXPECT_GT(vuopt::live::GetStats().stickyDropped, dropped0) << "armed again: regions drop the byte";
}

// The counters and the log: the live stats line carries the trip.
TEST(VuoptLive, TripwireIsCountedInTheStatsLine)
{
	LiveScope on(true, 4, true);
	RecompilerTestEnvironment::ResetVuBlockCache(1); // (the setting reaches the tier at a reset)
	const u64 trips0 = vuopt::live::GetStats().stickyTrips;
	std::string line = vuopt::live::FormatStats();
	EXPECT_NE(line.find(" sticky_trips="), std::string::npos) << line;
	EXPECT_NE(line.find(" sticky_trip_resets="), std::string::npos) << line;
	EXPECT_NE(line.find("sticky_tripped=0"), std::string::npos) << line;
	vuopt::live::TripStickyForTest();
	EXPECT_EQ(vuopt::live::GetStats().stickyTrips, trips0 + 1);
	line = vuopt::live::FormatStats();
	EXPECT_NE(line.find(" sticky_trips=" + std::to_string(trips0 + 1) + " "), std::string::npos) << line;
	EXPECT_NE(line.find("sticky_tripped=1"), std::string::npos) << line;
	EXPECT_NE(line.find("sticky tripwire: trips="), std::string::npos) << line;
}

// Each part of the tripwire, broken on purpose, is seen by a test above.
TEST(VuoptLive, TripwireMutationsAreCaught)
{
	struct Case
	{
		int mutation;
		Trigger trig;
		const char* what;
	};
	const Case cases[] = {
		{vuopt::live::TripMask(vuopt::live::kTripMutProgramImage), Trigger::Upload, "no scan of a new program's image"},
		{vuopt::live::TripMask(vuopt::live::kTripMutCompile), Trigger::Compile, "no hook on a compiled sticky reader"},
		{vuopt::live::TripMask(vuopt::live::kTripMutOutside), Trigger::OutsideJit, "microVU0's window access is not noticed"},
		{vuopt::live::TripMask(vuopt::live::kTripMutOutside), Trigger::OutsideInt, "the interpreter's window access is not noticed"},
		{vuopt::live::TripMask(vuopt::live::kTripMutNoRebuild), Trigger::Forced, "the trip does not rebuild the regions"},
		{vuopt::live::TripMask(vuopt::live::kTripMutJobOption), Trigger::Forced, "jobs made after the trip still drop the byte"},
	};
	for (const Case& c : cases)
	{
		const TripOutcome o = RunTripScenario(c.trig, true, c.mutation);
		EXPECT_NE(TripReport(o, c.what), "") << "mutation went unnoticed: " << c.what;
	}
	// a job published after the trip
	const auto published = [](int mutation) {
		Result ref;
		{
			LiveScope off(false, 4, true);
			VuTestHarness h(1);
			Prepare(h, StickyLoop(10), SetupSticky, 0);
			h.RunNoDiff();
			ref = Take(h);
		}
		LiveScope scope(true, 4, true, vuopt::live::kJumpTierUpRuns, /*sync=*/false);
		VuTestHarness h(1);
		Prepare(h, StickyLoop(10), SetupSticky, 0);
		h.RunNoDiff();
		vuopt::live::HoldWorker(true);
		for (int i = 0; i < 6; i++)
			h.RunJitPreserveBlockCache();
		vuopt::live::HoldWorker(false);
		vuopt::live::WaitWorkerIdle();
		vuopt::live::TripStickyForTest();
		vuopt::live::SetTripwireMutation(mutation);
		std::string problems;
		for (int i = 0; i < 4; i++)
		{
			h.RunJitPreserveBlockCache();
			problems += Diff(ref, Take(h));
		}
		return problems;
	};
	EXPECT_EQ(published(0), "");
	EXPECT_NE(published(vuopt::live::TripMask(vuopt::live::kTripMutPublish)), "") << "a job made with the drop was published after the trip and nobody noticed";
}

} // namespace recompiler_tests

#endif // arm64
