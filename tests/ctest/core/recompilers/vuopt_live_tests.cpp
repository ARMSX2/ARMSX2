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

#include <cstring>
#include <functional>
#include <string>
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
	LiveScope(bool on, u32 threshold, bool flagHack, u32 jumpThreshold = vuopt::live::kJumpTierUpRuns)
	{
		EmuConfig.Cpu.Recompiler.EnableVUOpt = on;
		EmuConfig.Speedhacks.vuFlagHack = flagHack;
		vuopt::live::SetTierUpRuns(threshold);
		vuopt::live::SetJumpTierUpRuns(jumpThreshold);
	}
	~LiveScope()
	{
		EmuConfig.Cpu.Recompiler.EnableVUOpt = savedOpt;
		EmuConfig.Speedhacks.vuFlagHack = savedFlagHack;
		vuopt::live::SetTierUpRuns(vuopt::live::kTierUpRuns);
		vuopt::live::SetJumpTierUpRuns(vuopt::live::kJumpTierUpRuns);
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

// The JR/JALR slow path counts too: with the dispatch threshold out of reach, a program
// that keeps missing the jump cache is tiered up from inside mVUcompileJIT, while its own
// code is running (the old graph is set aside under the caller, which carries on into the
// new entry). Every run, the one that tiers up included, must equal microVU's.
TEST(VuoptLive, JumpPathTiersUpAProgramMidRun)
{
	for (bool fh : {true, false})
		for (u32 budget : {0u, 40u})
		{
			std::string problems;
			const vuopt::live::Stats d = RunLive(JumpLoop(), SetupJump, fh, 1000000, 12, budget, problems, 6);
			EXPECT_TRUE(problems.empty()) << "flagHack=" << fh << " budget=" << budget << "\n" << problems;
			// (the harness's first run does the JIT pass more than once, each from a reset VU1,
			// so more than one program can be tiered up)
			EXPECT_GE(d.tierUps, 1u) << "flagHack=" << fh << " budget=" << budget;
			EXPECT_EQ(d.jumpTierUps, d.tierUps) << "the dispatcher's count is out of reach: every tier-up is the JR path's";
			EXPECT_GE(d.jumpCounted, 6u);
			EXPECT_GE(d.regions, 1u);
		}
	// ... and with the jump threshold out of reach too, nothing tiers up
	std::string problems;
	const vuopt::live::Stats d = RunLive(JumpLoop(), SetupJump, true, 1000000, 12, 0, problems, 1000000);
	EXPECT_TRUE(problems.empty()) << problems;
	EXPECT_EQ(d.tierUps, 0u);
	EXPECT_GT(d.jumpCounted, 0u) << "the JR path counts every entry it resolves";
}

// A stub program the dispatcher enters, whose JR goes into code the dispatcher never enters:
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
	for (const VuOp& op : CountedLoop(9))
		p.push_back(op);
	return p;
}

std::vector<Result> OverlaySequence(bool on, bool flagHack, u32 runs, vuopt::live::Stats& delta)
{
	std::vector<Result> out;
	LiveScope scope(on, 1000000, flagHack, 5);
	const vuopt::live::Stats before = vuopt::live::GetStats();
	VuTestHarness h(1);
	SetupLoop(h);
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
		CpuMicroVU1.Clear(kOverlayPair * 8, 0x200);
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

} // namespace recompiler_tests

#endif // arm64
