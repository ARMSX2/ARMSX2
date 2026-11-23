// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU AOT tiered bodies: a region is built once per routine and kept on the
// routine's chunk store entry, so every copy of the routine runs it, at any
// address.
//
// Each test runs a routine at one address until the tier builds its region,
// then uploads the micro image again with the routine somewhere else and
// other code around it. The copy has to run the region with no second build,
// and every run has to equal the interpreter's and microVU's own.

#if defined(_M_ARM64) || defined(__aarch64__)

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_ChunkStore-arm64.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/vuaot/VuAotLive.h"

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
constexpr u32 kMicroBytes = 0x4000;
constexpr u32 kLowerNop = 0x8000033Cu;
constexpr u32 kHome = 0x0400;
constexpr u32 kCopy = 0x2000;
constexpr u32 kTierRuns = 6;

inline VuOp L(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp P(u32 lower, u32 upper) { return VuOp{lower, upper}; }
inline VuOp Nop() { return L(kLowerNop); }
inline VuOp LoadViImm(u32 dst, u32 imm) { return L(VIADDIU_L(dst, vi::vi0, imm)); }
constexpr u32 PairIndex(u32 base, u32 n) { return ((base / 8) + n) & 0x7FFu; }

void WritePair(u32 addr, const VuOp& op)
{
	addr &= kMicroBytes - 8;
	std::memcpy(vuRegs[1].Micro + addr + 0, &op.lower, 4);
	std::memcpy(vuRegs[1].Micro + addr + 4, &op.upper, 4);
}

// A whole micro image: the harness's two pairs at address 0, code that
// differs with `seed` in every other pair, then `pairs` at `base`.
void Upload(u32 seed, u32 base, const std::vector<VuOp>& pairs)
{
	WritePair(0, EBitNopPair());
	WritePair(8, Nop());
	for (u32 n = 2; n < kMicroBytes / 8; n++)
		WritePair(n * 8, LoadViImm(vi::vi13, (seed * 37 + n * 5) & 0x3FF));
	for (size_t n = 0; n < pairs.size(); n++)
		WritePair(base + static_cast<u32>(n) * 8, pairs[n]);
	CpuMicroVU1.Clear(0, kMicroBytes);
}

void SeedRegs(VuTestHarness& h)
{
	h.SetVf(1, 2.0f, 3.0f, 4.0f, 5.0f);
	h.SetVf(2, 0.5f, 1.5f, 2.5f, 3.5f);
	h.SetVf(3, 1.0f, 1.0f, 1.0f, 1.0f);
	h.SetVf(4, 1.0001f, 0.9999f, 1.00001f, 0.5f);
	h.SetVf(5, 0.25f, -0.5f, 8.0f, 1.0f);
	h.TrackMemWindow(0, 0x400);
}

// A counted loop of FMACs, a flag reader and a store, then the end.
// The start is pair 0.
std::vector<VuOp> LoopRoutine(u32 passes)
{
	return {
		LoadViImm(vi::vi1, passes),                                                              // 0
		LoadViImm(vi::vi2, 0x10),                                                                // 1
		P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),        // 2: loop head
		P(VFMAND_L(vi::vi3, vi::vi2), VADD_U(mask::xyz, vf::vf5, vf::vf5, vf::vf3)),             // 3
		P(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0), VSUB_U(mask::xyzw, vf::vf7, vf::vf5, vf::vf3)), // 4
		L(VIADDI_L(vi::vi1, vi::vi1, -1)),                                                       // 5
		L(VIBNE_L(vi::vi1, vi::vi0, -5)),                                                        // 6: back to 2
		Nop(),                                                                                   // 7
		EBit(LoadViImm(vi::vi6, 0x66)),                                                          // 8
		Nop(),                                                                                   // 9
	};
}

// A loop that calls a subroutine through BAL on every pass; the subroutine
// returns through JALR. vi5 and vi8 hold link values.
std::vector<VuOp> CallRoutine(u32 passes)
{
	return {
		LoadViImm(vi::vi1, passes),                                                              // 0
		LoadViImm(vi::vi2, 0x10),                                                                // 1
		P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),        // 2: loop head
		P(kLowerNop, VADD_U(mask::xyzw, vf::vf5, vf::vf5, vf::vf3)),                             // 3
		L(VBAL_L(vi::vi5, +7)),                                                                  // 4: to 12, vi5 = 6
		Nop(),                                                                                   // 5
		L(VIADDI_L(vi::vi1, vi::vi1, -1)),                                                       // 6: the subroutine returns here
		L(VIBNE_L(vi::vi1, vi::vi0, -6)),                                                        // 7: back to 2
		Nop(),                                                                                   // 8
		EBit(LoadViImm(vi::vi6, 0x66)),                                                          // 9
		Nop(),                                                                                   // 10
		Nop(),                                                                                   // 11
		P(VIADDIU_L(vi::vi11, vi::vi11, 3), VSUB_U(mask::xyzw, vf::vf7, vf::vf5, vf::vf3)),      // 12: subroutine
		L(VJALR_L(vi::vi8, vi::vi5)),                                                            // 13: to 6, vi8 = 15
		Nop(),                                                                                   // 14
	};
}

// Ten pairs whose last two are a BAL and its delay slot. The start is pair 3;
// the BAL goes back to pair 0, which ends the program. Placed at the top of
// micro memory the BAL sits in pair 0x7FE and links 0x800.
constexpr u32 kTopStart = 3;
constexpr u32 kTopPairs = 10;
std::vector<VuOp> TopRoutine(u32 passes)
{
	return {
		LoadViImm(vi::vi11, 0xBB),                                                               // 0: the BAL's target
		EBit(LoadViImm(vi::vi6, 0x66)),                                                          // 1
		Nop(),                                                                                   // 2
		LoadViImm(vi::vi1, passes),                                                              // 3: start
		P(VIADDIU_L(vi::vi2, vi::vi2, 5), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),        // 4: loop head
		L(VIADDI_L(vi::vi1, vi::vi1, -1)),                                                       // 5
		L(VIBNE_L(vi::vi1, vi::vi0, -3)),                                                        // 6: back to 4
		Nop(),                                                                                   // 7
		L(VBAL_L(vi::vi5, -9)),                                                                  // 8: to 0, vi5 = 10
		Nop(),                                                                                   // 9
	};
}

// A loop, then the end on the last two pairs of the routine: placed at the
// top of micro memory the E bit is on pair 0x7FE, its delay slot on 0x7FF,
// and the program stops with TPC wrapped to 0.
constexpr u32 kEndPairs = 7;
std::vector<VuOp> EndAtTopRoutine(u32 passes)
{
	return {
		LoadViImm(vi::vi1, passes),                                                              // 0
		P(VIADDIU_L(vi::vi2, vi::vi2, 5), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),        // 1: loop head
		L(VIADDI_L(vi::vi1, vi::vi1, -1)),                                                       // 2
		L(VIBNE_L(vi::vi1, vi::vi0, -3)),                                                        // 3: back to 1
		Nop(),                                                                                   // 4
		EBit(LoadViImm(vi::vi6, 0x66)),                                                          // 5
		Nop(),                                                                                   // 6
	};
}

struct Result
{
	VuSnapshot snap;
	std::vector<u8> path1;
};

Result Take(const VuTestHarness& h) { return Result{h.JitSnapshot(), h.Path1PacketBytesJit()}; }

// What differs between two JIT runs, as text. Left out: STATUS, whose sticky
// bits a region may drop while nothing reads them.
std::string Diff(const Result& a, const Result& b)
{
	struct F
	{
		const char* name;
		size_t off, size;
	};
#define FR(f) F{#f, offsetof(VURegs, f), sizeof(VURegs::f)}
	static const F kFields[] = {FR(VF), FR(ACC), FR(q), FR(p), FR(cycle), FR(flags), FR(ebit),
		FR(pending_q), FR(pending_p), FR(micro_macflags), FR(micro_clipflags),
		F{"VI 0-15", offsetof(VURegs, VI), 16 * sizeof(REG_VI)},
		F{"TPC", offsetof(VURegs, VI) + REG_TPC * sizeof(REG_VI), sizeof(REG_VI)},
		F{"MAC flag", offsetof(VURegs, VI) + REG_MAC_FLAG * sizeof(REG_VI), sizeof(REG_VI)},
		F{"clip flag", offsetof(VURegs, VI) + REG_CLIP_FLAG * sizeof(REG_VI), sizeof(REG_VI)}};
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

// The integer registers and TPC of the last JIT run against the interpreter's,
// whole: both leave a link value unwrapped and TPC wrapped, as the console
// does. (The float registers are compared against microVU's.)
void ExpectMatchesInterp(const VuTestHarness& h, const VuSnapshot& interp, const char* what)
{
	for (u32 r = 0; r < 16; r++)
		EXPECT_EQ(h.GetViJit(r) & 0xFFFFu, interp.regs.VI[r].UL & 0xFFFFu) << what << ": vi" << r;
	EXPECT_EQ(h.GetViJit(REG_TPC), interp.regs.VI[REG_TPC].UL) << what << ": TPC";
}

VuSnapshot InterpOf(u32 seed, u32 base, const std::vector<VuOp>& pairs, u32 start)
{
	VuTestHarness h(1);
	SeedRegs(h);
	h.LoadProgram({EBitNopPair()});
	Upload(seed, base, pairs);
	h.SetStartPc(start);
	h.RunInterpOnly();
	return h.InterpSnapshot();
}

// The tier on or off for a scope, tiering a routine up after `kTierRuns` runs,
// every job waited for where it was asked for.
struct TierScope
{
	const bool savedOpt = EmuConfig.Cpu.Recompiler.EnableVUAot;
	const bool savedFlagHack = EmuConfig.Speedhacks.vuFlagHack;
	TierScope(bool on, bool flagHack, bool sync = true)
	{
		EmuConfig.Cpu.Recompiler.EnableVUAot = on;
		EmuConfig.Speedhacks.vuFlagHack = flagHack;
		vuaot::live::ResetStickyTripwire();
		vuaot::live::SetTripwireMutation(0);
		vuaot::live::SetTierUpRuns(kTierRuns);
		vuaot::live::SetSynchronous(sync);
	}
	~TierScope()
	{
		vuaot::live::HoldWorker(false);
		vuaot::live::SetJumpTierUpRuns(vuaot::live::kJumpTierUpRuns);
		vuaot::live::SetTripwireMutation(0);
		vuaot::live::ResetStickyTripwire();
		EmuConfig.Cpu.Recompiler.EnableVUAot = savedOpt;
		EmuConfig.Speedhacks.vuFlagHack = savedFlagHack;
		vuaot::live::SetTierUpRuns(vuaot::live::kTierUpRuns);
		vuaot::live::SetSynchronous(false);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
	}
};

// A JIT harness that uploads images and runs them, keeping the code cache.
struct Jit
{
	VuTestHarness h{1};

	Jit(u32 seed, u32 base, const std::vector<VuOp>& pairs, u32 start, u32 budget = 0)
	{
		SeedRegs(h);
		h.LoadProgram({EBitNopPair()});
		h.ResumeUntilTerminated();
		Upload(seed, base, pairs);
		h.SetStartPc(start);
		h.RunNoDiff();
		if (budget)
			h.SetCycleBudget(budget);
		h.RunJitPreserveBlockCache();
	}

	void RunAt(u32 start)
	{
		h.SetStartPc(start);
		h.RunJitPreserveBlockCache();
	}
};

// microVU's own result (the tier off) for the image run from `start`.
Result MvuOf(u32 seed, u32 base, const std::vector<VuOp>& pairs, u32 start, bool flagHack, u32 budget = 0)
{
	TierScope off(false, flagHack);
	Jit jit(seed, base, pairs, start, budget);
	return Take(jit.h);
}

// The tier's counters: region builds (tier-ups and re-tiers), regions, bail
// compiles, jump targets handed to regions, tiered bodies held, placements of
// one, and copies moved onto one from microVU's code.
struct Counts
{
	u64 builds, regions, bails, targets, bodies, placed, reaimed;
};
Counts Now()
{
	const vuaot::live::Stats s = vuaot::live::GetStats();
	return {s.tierUps + s.retiers, s.regions, s.bailCompiles, s.targetsRecorded, s.tieredBodies, s.tieredPlaced, s.tieredReAimed};
}
Counts operator-(const Counts& a, const Counts& b)
{
	return {a.builds - b.builds, a.regions - b.regions, a.bails - b.bails, a.targets - b.targets, a.bodies - b.bodies,
		a.placed - b.placed, a.reaimed - b.reaimed};
}

// Runs the routine at `home` until the tier has built its region, then
// uploads it at `copy` and runs it there `copyRuns` times. Every run at the
// copy has to equal the interpreter and microVU, and the copy must cost no
// region build.
void TierThenMove(const std::vector<VuOp>& routine, u32 startPair, u32 home, u32 copy, bool flagHack, u32 budget, u32 copyRuns,
	Counts* atHome = nullptr, Counts* atCopy = nullptr)
{
	SCOPED_TRACE(testing::Message() << "flagHack=" << flagHack << " budget=" << budget << " copy=0x" << std::hex << copy);
	const u32 homeStart = home + startPair * 8, copyStart = copy + startPair * 8;
	const VuSnapshot interpHome = InterpOf(1, home, routine, homeStart);
	const VuSnapshot interpCopy = InterpOf(2, copy, routine, copyStart);
	const Result mvuCopy = MvuOf(2, copy, routine, copyStart, flagHack, budget);

	TierScope on(true, flagHack);
	const Counts c0 = Now();
	Jit jit(1, home, routine, homeStart, budget);
	for (u32 i = 0; i < kTierRuns + 2; i++)
	{
		jit.RunAt(homeStart);
		ExpectMatchesInterp(jit.h, interpHome, "home");
	}
	const Counts c1 = Now();
	ASSERT_EQ(c1.builds - c0.builds, 1u) << "the routine was not tiered at its first address";
	ASSERT_GE(c1.regions - c0.regions, 1u) << "the tier built no region";
	EXPECT_EQ(c1.bodies, 1u) << "one build is one tiered body";
	EXPECT_EQ(c1.reaimed - c0.reaimed, 1u) << "the copy that asked for the build was not moved onto the body";

	Upload(2, copy, routine);
	for (u32 i = 0; i < copyRuns; i++)
	{
		jit.RunAt(copyStart);
		ExpectMatchesInterp(jit.h, interpCopy, "copy");
		const std::string d = Diff(mvuCopy, Take(jit.h));
		EXPECT_TRUE(d.empty()) << "copy run " << i << " against microVU: " << d;
	}
	const Counts c2 = Now();
	EXPECT_EQ(c2.builds - c1.builds, 0u) << "the copy was built again";
	EXPECT_EQ(c2.bodies, 1u);
	EXPECT_EQ(c2.placed - c1.placed, 1u) << "the copy does not run the tiered body";
	EXPECT_EQ(c2.reaimed - c1.reaimed, 0u) << "the copy ran microVU's code first";
	if (atHome)
		*atHome = c1 - c0;
	if (atCopy)
		*atCopy = c2 - c1;
}
} // namespace

TEST(VuAotBodies, RoutineTieredAtOneAddressRunsItsRegionAtAnother)
{
	for (bool fh : {true, false})
		TierThenMove(LoopRoutine(10), 0, kHome, kCopy, fh, 0, kTierRuns + 4);
}

// A JALR whose target the jump caches recorded is a direct edge in the
// region. The copy's target is the same distance from its own root.
TEST(VuAotBodies, RecordedJumpTargetsInARelocatedCopy)
{
	for (bool fh : {true, false})
	{
		Counts home{};
		TierThenMove(CallRoutine(9), 0, kHome, kCopy, fh, 0, kTierRuns + 4, &home);
		EXPECT_GE(home.targets, 1u) << "the JALR's return target was not handed to the region";
	}
}

// A cycle budget short of the region's own test: the copy leaves the region
// for microVU's code of the copy's block and resumes.
TEST(VuAotBodies, BailInARelocatedCopy)
{
	for (bool fh : {true, false})
	{
		Counts copy{};
		TierThenMove(LoopRoutine(10), 0, kHome, kCopy, fh, 23, kTierRuns + 4, nullptr, &copy);
		EXPECT_GT(copy.bails, 0u) << "a budget this small must take the region's bails in the copy";
	}
}

// BAL and JALR link values and the TPC of a program end, in region code at
// a new root.
TEST(VuAotBodies, LinkValuesAndProgramEndAtANewRoot)
{
	TierThenMove(CallRoutine(9), 0, kHome, kCopy + 0x1238, true, 0, kTierRuns + 2);
	TierThenMove(TopRoutine(9), kTopStart, kHome, kCopy, true, 0, kTierRuns + 2);
}

// The same with the copy ending on the last pair of micro memory: a BAL in
// pair 0x7FE links 0x800, not wrapped, and a program end whose delay slot is
// pair 0x7FF leaves TPC 0.
TEST(VuAotBodies, LinkValueAndProgramEndAtTheTopOfMicroMemory)
{
	const u32 top = kMicroBytes - kTopPairs * 8;
	TierThenMove(TopRoutine(9), kTopStart, kHome, top, true, 0, kTierRuns + 2);
	{
		// The link is the BAL's own pair + 2 with no wrap.
		const VuSnapshot interp = InterpOf(2, top, TopRoutine(9), top + kTopStart * 8);
		EXPECT_EQ(interp.regs.VI[vi::vi5].UL & 0xFFFFu, 0x800u);
	}
	TierThenMove(EndAtTopRoutine(9), 0, kHome, kMicroBytes - kEndPairs * 8, true, 0, kTierRuns + 2);
}

// A body built while nothing read the sticky bits of STATUS leaves them out.
// Once a reader is known (the tripwire is set) it is not placed for a new
// copy: the copy runs microVU's code, and its STATUS is exact. The trip is
// forced here with the VU1 reset that normally follows it left out, so the
// body is still in the store when the copy arrives. (Mutation: the placing
// rule off, and the copy's STATUS loses its sticky bits.)
TEST(VuAotBodies, BodyThatDroppedTheStickyBitsIsNotPlacedOnceAReaderIsKnown)
{
	const std::vector<VuOp> routine = LoopRoutine(10);
	const auto statusOf = [](const VuTestHarness& h) { return h.JitSnapshot().regs.VI[REG_STATUS_FLAG].UL; };
	u32 mvuStatus = 0;
	{
		TierScope off(false, false);
		Jit jit(2, kCopy, routine, kCopy);
		mvuStatus = statusOf(jit.h);
	}
	ASSERT_NE(mvuStatus & 0xFC0u, 0u) << "the routine sets no sticky bit: the test would show nothing";

	for (bool mutated : {false, true})
	{
		SCOPED_TRACE(mutated ? "placing rule off" : "as shipped");
		TierScope on(true, false);
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		Jit jit(1, kHome, routine, kHome);
		for (u32 i = 0; i < kTierRuns + 2; i++)
			jit.RunAt(kHome);
		const vuaot::live::Stats s1 = vuaot::live::GetStats();
		ASSERT_EQ(s1.tierUps - s0.tierUps, 1u);
		ASSERT_GT(s1.stickyDropped, s0.stickyDropped) << "the body was built with the sticky byte";

		int mask = vuaot::live::TripMask(vuaot::live::kTripMutNoRebuild);
		if (mutated)
			mask |= vuaot::live::TripMask(vuaot::live::kTripMutPlace);
		vuaot::live::SetTripwireMutation(mask);
		vuaot::live::TripStickyForTest();
		ASSERT_TRUE(vuaot::live::StickyTripped());

		const Counts c1 = Now();
		Upload(2, kCopy, routine);
		jit.RunAt(kCopy);
		const Counts c2 = Now();
		if (!mutated)
		{
			EXPECT_EQ(c2.placed - c1.placed, 0u) << "a body that dropped the sticky bits was placed with the tripwire set";
			EXPECT_EQ(statusOf(jit.h), mvuStatus);
		}
		else
		{
			EXPECT_EQ(c2.placed - c1.placed, 1u);
			EXPECT_NE(statusOf(jit.h) & 0xFC0u, mvuStatus & 0xFC0u) << "the mutation went unnoticed";
		}
	}
}

// The worker finishes a routine's regions after the game has moved the
// routine: the job was made from the copy at one address, and the copy that
// is there when it comes back is at another. The regions are built into a
// body at that copy, with no second job.
TEST(VuAotBodies, JobFinishedAfterTheRoutineMovedIsBuiltAtTheNewAddress)
{
	for (u32 copy : {kCopy, kHome - 0x200})
	{
		SCOPED_TRACE(testing::Message() << "copy=0x" << std::hex << copy);
		const std::vector<VuOp> routine = CallRoutine(9);
		const VuSnapshot interpCopy = InterpOf(2, copy, routine, copy);
		const Result mvuCopy = MvuOf(2, copy, routine, copy, true);

		TierScope on(true, true, /*sync=*/false);
		const vuaot::live::Stats s0 = vuaot::live::GetStats();
		Jit jit(1, kHome, routine, kHome);
		vuaot::live::HoldWorker(true);
		for (u32 i = 0; i < kTierRuns + 2; i++)
			jit.RunAt(kHome);
		ASSERT_EQ(vuaot::live::GetStats().jobs - s0.jobs, 1u) << "the routine did not ask for its regions";
		ASSERT_EQ(vuaot::live::GetStats().built, s0.built);

		Upload(2, copy, routine);
		vuaot::live::HoldWorker(false);
		vuaot::live::WaitWorkerIdle();
		for (u32 i = 0; i < 3; i++)
		{
			jit.RunAt(copy + 0);
			ExpectMatchesInterp(jit.h, interpCopy, "copy");
			const std::string d = Diff(mvuCopy, Take(jit.h));
			EXPECT_TRUE(d.empty()) << "copy run " << i << " against microVU: " << d;
		}
		const vuaot::live::Stats s1 = vuaot::live::GetStats();
		EXPECT_EQ(s1.built - s0.built, 1u) << "the job was not built at the copy";
		EXPECT_EQ(s1.jobs - s0.jobs, 1u);
		EXPECT_EQ(s1.discardMemory, s0.discardMemory);
		EXPECT_GE(s1.targetsRecorded - s0.targetsRecorded, 1u) << "the test wants a region with recorded jump targets";
		EXPECT_EQ(s1.tieredReAimed - s0.tieredReAimed, 1u) << "the copy was not moved onto the body";
	}
}

// A JR/JALR target the jump cache keeps missing is a routine of its own and
// asks for its regions. When the routine that jumps to it is built first, its
// body holds the target as a node and takes over the target's block: the
// target's own job is not built on top of it.
TEST(VuAotBodies, RootTakenInByAnotherRoutinesBodyIsNotBuiltAgain)
{
	const std::vector<VuOp> routine = CallRoutine(9);
	const VuSnapshot interp = InterpOf(1, kHome, routine, kHome);

	TierScope on(true, true, /*sync=*/false);
	vuaot::live::SetJumpTierUpRuns(4);
	const vuaot::live::Stats s0 = vuaot::live::GetStats();
	Jit jit(1, kHome, routine, kHome);
	vuaot::live::HoldWorker(true);
	// A write over the routine empties its quick slots, so the JALR's first
	// return of each run misses the jump cache.
	const auto run = [&]() {
		CpuMicroVU1.Clear(kHome, 0x100);
		jit.RunAt(kHome);
		ExpectMatchesInterp(jit.h, interp, "home");
	};
	for (u32 i = 0; i < kTierRuns + 4; i++)
		run();
	ASSERT_EQ(vuaot::live::GetStats().jobs - s0.jobs, 2u) << "the routine and the return point each ask for regions";

	vuaot::live::HoldWorker(false);
	vuaot::live::WaitWorkerIdle();
	for (u32 i = 0; i < 4; i++)
		run();
	const vuaot::live::Stats s1 = vuaot::live::GetStats();
	EXPECT_EQ(s1.built - s0.built, 1u);
	EXPECT_EQ(s1.discardCovered - s0.discardCovered, 1u) << "the return point's job was built over the body that holds it";
	EXPECT_EQ(s1.tieredBodies, 1u);
}

// Two routines that begin with the same pair share a head. One has a loop and
// is tiered; the tier declines the other (a single block). Runs of the
// declined one are not the tiered one's: however many there are, they stay on
// the dispatcher's quick path.
TEST(VuAotBodies, DeclinedRoutineSharingAHeadStaysOnTheQuickPath)
{
	const std::vector<VuOp> looping = LoopRoutine(10);
	// The same first pair, then straight to the end.
	const std::vector<VuOp> straight = {
		looping[0],
		P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),
		EBit(LoadViImm(vi::vi6, 0x66)),
		Nop(),
	};
	const VuSnapshot interp = InterpOf(2, kHome, straight, kHome);

	TierScope on(true, true);
	const vuaot::live::Stats s0 = vuaot::live::GetStats();
	Jit jit(1, kHome, looping, kHome);
	for (u32 i = 0; i < kTierRuns + 2; i++)
		jit.RunAt(kHome);
	ASSERT_EQ(vuaot::live::GetStats().tierUps - s0.tierUps, 1u);

	Upload(2, kHome, straight);
	for (u32 i = 0; i < kTierRuns + 2; i++)
		jit.RunAt(kHome);
	const vuaot::live::Stats s1 = vuaot::live::GetStats();
	ASSERT_EQ(s1.jobsDeclined - s0.jobsDeclined, 1u) << "the straight routine was not declined";
	ASSERT_EQ(s1.tierUps - s0.tierUps, 1u);

	// Past the tiered routine's next look for new jump targets (every 1024 runs).
	for (u32 i = 0; i < vuaot::live::kRetierCheckMask + 64; i++)
		jit.RunAt(kHome);
	const vuaot::live::Stats s2 = vuaot::live::GetStats();
	for (u32 i = 0; i < 50; i++)
		jit.RunAt(kHome);
	ExpectMatchesInterp(jit.h, interp, "straight");
	const vuaot::live::Stats s3 = vuaot::live::GetStats();
	EXPECT_EQ(s3.slowDispatches - s2.slowDispatches, 0u) << "a declined routine's dispatches take the full search";
	EXPECT_EQ(s3.jobs, s1.jobs);
}

// Heat is counted on the routine, not on the copy: runs at two addresses add
// up to one tier-up. The copy that was there before the region arrived gets
// it at its next slow path.
TEST(VuAotBodies, CopiesShareHeatAndAnExistingCopyIsReAimed)
{
	const std::vector<VuOp> routine = LoopRoutine(10);
	const VuSnapshot interpHome = InterpOf(1, kHome, routine, kHome);
	const VuSnapshot interpCopy = InterpOf(2, kCopy, routine, kCopy);

	TierScope on(true, true);
	const Counts c0 = Now();
	// Four or five runs at the home address (the harness's first run may be
	// thrown away with a reset): under the threshold.
	Jit jit(1, kHome, routine, kHome);
	for (u32 i = 0; i < 3; i++)
		jit.RunAt(kHome);
	EXPECT_EQ(Now().builds, c0.builds);
	// Three at the copy, also under the threshold alone, take the routine over it.
	Upload(2, kCopy, routine);
	for (u32 i = 0; i < 3; i++)
	{
		jit.RunAt(kCopy);
		ExpectMatchesInterp(jit.h, interpCopy, "copy");
	}
	const Counts c1 = Now();
	EXPECT_EQ(c1.builds - c0.builds, 1u) << "the runs of the two copies did not add up to one tier-up";
	ASSERT_GE(c1.regions - c0.regions, 1u);
	EXPECT_EQ(c1.reaimed - c0.reaimed, 1u);

	// The first image again: its program still holds microVU's blocks.
	Upload(1, kHome, routine);
	for (u32 i = 0; i < 3; i++)
	{
		jit.RunAt(kHome);
		ExpectMatchesInterp(jit.h, interpHome, "home again");
	}
	const Counts c2 = Now();
	EXPECT_EQ(c2.builds - c1.builds, 0u) << "the first copy was built again";
	EXPECT_EQ(c2.reaimed - c1.reaimed, 1u) << "the copy that was there before the body was not moved onto it";
	EXPECT_EQ(c2.bodies, 1u);
}

} // namespace recompiler_tests

#endif
