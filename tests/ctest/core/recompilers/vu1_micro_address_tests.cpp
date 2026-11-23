// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Every place VU1 code produces a micro-memory address.
//
// A VU address reaches the guest through branch targets, the link value BAL
// and JALR write, the TPC a program leaves on exit (E bit, T bit, a cycle
// budget break), the continuation address of a branch that sits in another
// branch's delay slot, and, under the IbitHack gamefix, the run-time load of
// an instruction's own immediate. Each test runs its program at several
// places in micro memory, including the last pairs before the 16 KiB wrap, so
// a target, a link value or a TPC that is not wrapped the way the VU wraps it
// shows up against the interpreter.

#include "harness/ChunkPackScope.h"
#include "harness/VuTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "VU.h"
#include "VUmicro.h"
#include "Config.h"
#include "arm64/microVU_Persist-arm64.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

constexpr u32 kMicroBytes = 0x4000;
constexpr u32 kLowerNop = 0x8000033Cu;

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp Nop() { return LowerOnly(kLowerNop); }
inline VuOp LoadViImm(u32 dst, u32 imm) { return LowerOnly(VIADDIU_L(dst, vi::vi0, imm)); }

// Where each program is placed: the middle of micro memory, and three places
// near the top where the program runs across the wrap to address 0.
constexpr u32 kBases[] = {0x0200, 0x3FE0, 0x3FF0, 0x3FF8};

// Pair index (the unit link values and TPC use) of pair `n` of a program at `base`.
constexpr u32 PairIndex(u32 base, u32 n) { return ((base / 8) + n) & 0x7FFu; }

// The link value a BAL or JALR in pair `n` of a program at `base` writes: its
// own pair index plus 2, not wrapped at the top of micro memory (0x800 or
// 0x801 from the last two pairs), as a console capture reads it.
constexpr u32 LinkValue(u32 base, u32 n) { return PairIndex(base, n) + 2; }

void WritePair(u32 addr, const VuOp& op)
{
	addr &= kMicroBytes - 8;
	std::memcpy(vuRegs[1].Micro + addr + 0, &op.lower, 4);
	std::memcpy(vuRegs[1].Micro + addr + 4, &op.upper, 4);
}

// Pairs that end the program wherever control lands on them: an E-bit pair
// that marks vi14, then a plain pair that marks vi15 (its delay slot).
void WriteLandingField(u32 addr, u32 pairs)
{
	for (u32 n = 0; n < pairs; n++)
	{
		const VuOp op = (n & 1) ? LoadViImm(vi::vi15, 0x700 + n) : EBit(LoadViImm(vi::vi14, 0x700 + n));
		WritePair(addr + n * 8, op);
	}
}

// Loads the harness (which wants a program of its own at address 0), then
// writes `pairs` at `base` followed by a landing field, and starts there.
void Place(VuTestHarness& h, u32 base, const std::vector<VuOp>& pairs)
{
	h.LoadProgram({EBitNopPair()});
	for (size_t n = 0; n < pairs.size(); n++)
		WritePair(base + static_cast<u32>(n) * 8, pairs[n]);
	WriteLandingField(base + static_cast<u32>(pairs.size()) * 8, 16);
	h.SetStartPc(base);
}

// The integer registers and TPC of the last JIT run against the interpreter's,
// whole: both leave a link unwrapped and TPC wrapped, as the console does.
void ExpectJitMatchesInterp(const VuTestHarness& h)
{
	for (u32 r = 0; r < 16; r++)
		EXPECT_EQ(h.GetViJit(r), h.GetViInterp(r)) << "vi" << r;
	EXPECT_EQ(h.GetViJit(REG_TPC), h.GetViInterp(REG_TPC)) << "TPC";
}

struct ScopedIbitHack
{
	bool prev;
	explicit ScopedIbitHack(bool on) : prev(EmuConfig.Gamefixes.IbitHack) { EmuConfig.Gamefixes.IbitHack = on; }
	~ScopedIbitHack() { EmuConfig.Gamefixes.IbitHack = prev; }
};

struct ScopedVu1TStop
{
	u32 prev;
	ScopedVu1TStop() : prev(vuRegs[0].VI[REG_FBRST].UL) { vuRegs[0].VI[REG_FBRST].UL = prev | 0x800u; }
	~ScopedVu1TStop() { vuRegs[0].VI[REG_FBRST].UL = prev; }
};

// A counted loop, a forward B over one pair, then the end.
std::vector<VuOp> LoopAndBranchProgram()
{
	return {
		LoadViImm(vi::vi1, 3),                           // 0
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi2, 5)),       // 1: loop head
		LowerOnly(VIADDI_L(vi::vi1, vi::vi1, -1)),       // 2
		Nop(),                                           // 3
		Nop(),                                           // 4
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, -5)),        // 5: back to 1
		Nop(),                                           // 6
		LowerOnly(VB_L(+2)),                             // 7: to 10
		LoadViImm(vi::vi3, 0x33),                        // 8: delay slot
		LoadViImm(vi::vi4, 0x44),                        // 9: skipped
		LoadViImm(vi::vi5, 0x55),                        // 10
		EBitNopPair(),                                   // 11
		Nop(),                                           // 12
	};
}

// Two branches in a row: the second sits in the first one's delay slot.
struct Shape
{
	const char* name;
	u32 first;  // pair 1
	u32 second; // pair 2, the delay slot
	bool targetInVi9 = false; // jumps to the pair index a test seeds in vi9
};
const Shape kDelaySlotShapes[] = {
	{"B then B", VB_L(+4), VB_L(+7)},
	{"B then BAL", VB_L(+4), VBAL_L(vi::vi5, +7)},
	{"B then IBNE", VB_L(+4), VIBNE_L(vi::vi1, vi::vi0, +7)},
	{"IBNE then B", VIBNE_L(vi::vi1, vi::vi0, +4), VB_L(+7)},
	{"IBNE then IBNE", VIBNE_L(vi::vi1, vi::vi0, +4), VIBNE_L(vi::vi2, vi::vi0, +7)},
	{"IBNE then BAL", VIBNE_L(vi::vi1, vi::vi0, +4), VBAL_L(vi::vi5, +7)},
	{"BAL then BAL", VBAL_L(vi::vi6, +4), VBAL_L(vi::vi5, +7)},
	{"B then JALR", VB_L(+4), VJALR_L(vi::vi5, vi::vi9), true},
};

// The delay-slot program for a shape: every pair control can reach after the
// two branches marks vi10, and a landing field ends the program.
void WriteDelaySlotProgram(u32 base, const Shape& s)
{
	WritePair(base + 0, Nop());
	WritePair(base + 8, LowerOnly(s.first));
	WritePair(base + 16, LowerOnly(s.second));
	for (u32 n = 3; n < 14; n++)
		WritePair(base + n * 8, LowerOnly(VIADDIU_L(vi::vi10, vi::vi10, 1u << (n - 3))));
	WriteLandingField(base + 14 * 8, 16);
}

// A loop that calls a subroutine on every pass. The JR that returns starts a
// second compile at pair 4, and that compile's pass back through pair 1 calls
// the subroutine the first compile emitted: a branch from one chunk into
// another, taken `passes` times.
std::vector<VuOp> CallInLoopProgram(u32 passes)
{
	return {
		LoadViImm(vi::vi1, passes),                      // 0
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi2, 5)),       // 1: loop head
		LowerOnly(VBAL_L(vi::vi5, +7)),                  // 2: to 10, vi5 = 4
		Nop(),                                           // 3
		LowerOnly(VIADDI_L(vi::vi1, vi::vi1, -1)),       // 4: JR returns here
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, -5)),        // 5: back to 1
		Nop(),                                           // 6
		EBit(LoadViImm(vi::vi6, 0x66)),                  // 7
		Nop(),                                           // 8
		Nop(),                                           // 9
		LowerOnly(VIADDIU_L(vi::vi11, vi::vi11, 3)),     // 10: subroutine
		LowerOnly(VJR_L(vi::vi5)),                       // 11: to 4
		Nop(),                                           // 12
	};
}

void WriteProgram(u32 base, const std::vector<VuOp>& pairs)
{
	for (size_t n = 0; n < pairs.size(); n++)
		WritePair(base + static_cast<u32>(n) * 8, pairs[n]);
	WriteLandingField(base + static_cast<u32>(pairs.size()) * 8, 16);
}

} // namespace

TEST(Vu1MicroAddress, BranchTargetsAndEndTpc)
{
	for (const u32 base : kBases)
	{
		SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base);
		VuTestHarness h(1);
		Place(h, base, LoopAndBranchProgram());
		h.Run();
		EXPECT_EQ(h.GetViJit(vi::vi2), 15u);
		EXPECT_EQ(h.GetViJit(vi::vi3), 0x33u);
		EXPECT_EQ(h.GetViJit(vi::vi4), 0u);
		EXPECT_EQ(h.GetViJit(vi::vi5), 0x55u);
		EXPECT_EQ(h.GetViJit(REG_TPC), PairIndex(base, 13));
		ExpectJitMatchesInterp(h);
	}
}

// BAL and JALR write the pair index after their delay slot. JR and JALR leave
// through the run-time jump path; the second run takes its cache.
TEST(Vu1MicroAddress, BalAndJalrLinkValuesAndRegisterJumps)
{
	for (const u32 base : kBases)
	{
		SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base);
		VuTestHarness h(1);
		Place(h, base, {
			LowerOnly(VBAL_L(vi::vi5, +4)),              // 0: to 5, vi5 = 2
			Nop(),                                       // 1
			LoadViImm(vi::vi7, 0x77),                    // 2: JALR returns here
			EBitNopPair(),                               // 3
			Nop(),                                       // 4
			LoadViImm(vi::vi6, 0x66),                    // 5
			LowerOnly(VJALR_L(vi::vi8, vi::vi9)),        // 6: to 9, vi8 = 8
			Nop(),                                       // 7
			LoadViImm(vi::vi10, 0xAA),                   // 8: skipped
			LoadViImm(vi::vi11, 0xBB),                   // 9
			LowerOnly(VJR_L(vi::vi5)),                   // 10: back to 2
			Nop(),                                       // 11
		});
		h.SetVi(vi::vi9, PairIndex(base, 9));
		h.RunNoDiff();
		EXPECT_EQ(h.GetViJit(vi::vi5), LinkValue(base, 0));
		EXPECT_EQ(h.GetViJit(vi::vi8), LinkValue(base, 6));
		EXPECT_EQ(h.GetViJit(vi::vi6), 0x66u);
		EXPECT_EQ(h.GetViJit(vi::vi7), 0x77u);
		EXPECT_EQ(h.GetViJit(vi::vi10), 0u);
		EXPECT_EQ(h.GetViJit(vi::vi11), 0xBBu);
		EXPECT_EQ(h.GetViJit(REG_TPC), PairIndex(base, 5));
		ExpectJitMatchesInterp(h);

		h.RunJitPreserveBlockCache();
		ExpectJitMatchesInterp(h);
		h.RunJitPreserveBlockCache();
		ExpectJitMatchesInterp(h);
	}
}

// The E bit on a branch: the program ends after the delay slot with TPC at
// the taken target or at the pair after the delay slot.
TEST(Vu1MicroAddress, EbitOnBranchLeavesTargetOrFallthroughInTpc)
{
	for (const u32 base : kBases)
	{
		for (const u32 seed : {0u, 1u})
		{
			SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base << " vi1 " << seed);
			VuTestHarness h(1);
			Place(h, base, {
				Nop(),                                           // 0
				EBit(LowerOnly(VIBNE_L(vi::vi1, vi::vi0, +5))),  // 1: to 7
				LoadViImm(vi::vi2, 0x22),                        // 2: delay slot
			});
			h.SetVi(vi::vi1, seed);
			h.Run();
			EXPECT_EQ(h.GetViJit(REG_TPC), seed ? PairIndex(base, 7) : PairIndex(base, 3));
			ExpectJitMatchesInterp(h);
		}

		SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base << " B");
		VuTestHarness h(1);
		Place(h, base, {
			Nop(),                                // 0
			EBit(LowerOnly(VB_L(+9))),            // 1: to 11
			LoadViImm(vi::vi2, 0x22),             // 2
		});
		h.Run();
		EXPECT_EQ(h.GetViJit(REG_TPC), PairIndex(base, 11));
		ExpectJitMatchesInterp(h);
	}
}

// A cycle budget that runs out inside the loop: the break leaves the loop
// head in TPC, and re-entering until the end gives the interpreter's result.
TEST(Vu1MicroAddress, BudgetBreakLeavesBlockStartInTpcAndResumes)
{
	for (const u32 base : kBases)
	{
		SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base);
		std::vector<VuOp> prog = LoopAndBranchProgram();
		prog[0] = LoadViImm(vi::vi1, 200);

		VuTestHarness h(1);
		Place(h, base, prog);
		h.Run();
		ASSERT_EQ(h.GetViJit(vi::vi2), 1000u);
		ExpectJitMatchesInterp(h);

		h.SetCycleBudget(40);
		h.RunJitPreserveBlockCache();
		ASSERT_FALSE(h.HasTerminated());
		EXPECT_EQ(h.GetViJit(REG_TPC), PairIndex(base, 1)) << "the break parks the loop block";
		h.ContinueJit(40, 400);
		ASSERT_TRUE(h.HasTerminated());
		ExpectJitMatchesInterp(h);
	}
}

// With the T-stop bit of FBRST set, a T bit stops the program: after a plain
// pair at the next pair, on a branch at its target or its fall-through.
TEST(Vu1MicroAddress, TbitStopLeavesNextAddressInTpc)
{
	for (const u32 base : kBases)
	{
		{
			SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base << " plain pair");
			ScopedVu1TStop tstop;
			VuTestHarness h(1);
			Place(h, base, {
				LoadViImm(vi::vi1, 0x11),             // 0
				TBit(LoadViImm(vi::vi2, 0x22)),       // 1: stops after this pair
				LoadViImm(vi::vi3, 0x33),             // 2
			});
			h.RunNoDiff();
			EXPECT_EQ(h.GetViJit(vi::vi2), 0x22u);
			EXPECT_EQ(h.GetViJit(vi::vi3), 0u);
			EXPECT_EQ(h.GetViJit(REG_TPC), PairIndex(base, 2));
			ExpectJitMatchesInterp(h);
		}
		for (const u32 seed : {0u, 1u})
		{
			SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base << " IBNE vi1 " << seed);
			ScopedVu1TStop tstop;
			VuTestHarness h(1);
			Place(h, base, {
				Nop(),                                           // 0
				TBit(LowerOnly(VIBNE_L(vi::vi1, vi::vi0, +5))),  // 1: to 7
				LoadViImm(vi::vi2, 0x22),                        // 2
			});
			h.SetVi(vi::vi1, seed);
			h.RunNoDiff();
			EXPECT_EQ(h.GetViJit(REG_TPC), seed ? PairIndex(base, 7) : PairIndex(base, 3));
			ExpectJitMatchesInterp(h);
		}
		{
			SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base << " B");
			ScopedVu1TStop tstop;
			VuTestHarness h(1);
			Place(h, base, {
				Nop(),                                // 0
				TBit(LowerOnly(VB_L(+9))),            // 1: to 11
				LoadViImm(vi::vi2, 0x22),             // 2
			});
			h.RunNoDiff();
			EXPECT_EQ(h.GetViJit(REG_TPC), PairIndex(base, 11));
			ExpectJitMatchesInterp(h);
		}
	}
}

// A branch in another branch's delay slot. The second branch's target, or the
// address after the first branch's target, is computed at run time from
// addresses the block stores.
TEST(Vu1MicroAddress, BranchInDelaySlotContinuationAddresses)
{
	// The second base puts both branch targets across the wrap; the third makes
	// the first branch target the last pair of micro memory, so a continuation
	// computed at run time as "address + 8" wraps to pair 0.
	for (const u32 base : {0x0200u, 0x3FB0u, 0x3FC8u})
	{
		for (const Shape& s : kDelaySlotShapes)
		{
			for (const u32 seed : {0u, 1u, 2u, 3u})
			{
				SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base << " " << s.name << " seed " << seed);
				VuTestHarness h(1);
				h.LoadProgram({EBitNopPair()});
				WriteDelaySlotProgram(base, s);
				h.SetStartPc(base);
				h.SetVi(vi::vi1, seed & 1);
				h.SetVi(vi::vi2, seed >> 1);
				h.SetVi(vi::vi9, PairIndex(base, 12));
				h.RunNoDiff();
				ExpectJitMatchesInterp(h);
			}
		}
	}
}

// Under the IbitHack gamefix the block reads its immediates from micro memory
// when it runs: the I-bit float, IADDI, IADDIU, ISUBIU and the offset of a
// memory op.
TEST(Vu1MicroAddress, IbitHackImmediatesReadAtTheirOwnAddress)
{
	ScopedIbitHack hack(true);
	for (const u32 base : kBases)
	{
		SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base);
		VuTestHarness h(1);
		h.SetVfBits(vf::vf1, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u);
		h.WriteMemU128(0x30, 0x1234, 0, 0, 0);
		h.TrackMemWindow(0x40, 16);
		Place(h, base, {
			IBit(VuOp{VLitI(0x40400000u), VNOP_U()}),                        // 0: I = 3.0
			IBit(VuOp{VLitZero(), VMULi_U(mask::xyzw, vf::vf2, vf::vf1)}),   // 1: vf2 = vf1 * I
			LowerOnly(VIADDIU_L(vi::vi1, vi::vi0, 0x123)),                   // 2
			LowerOnly(VIADDIU_L(vi::vi2, vi::vi1, 0x456)),                   // 3
			LowerOnly(VIADDI_L(vi::vi3, vi::vi0, -7)),                       // 4
			LowerOnly(VIADDI_L(vi::vi4, vi::vi2, 5)),                        // 5
			LowerOnly(VISUBIU_L(vi::vi5, vi::vi2, 0x21)),                    // 6
			LowerOnly(VILW_L(mask::x, vi::vi6, vi::vi0, 3)),                 // 7: vi6 = mem[0x30].x
			LowerOnly(VISW_L(mask::x, vi::vi2, vi::vi0, 4)),                 // 8: mem[0x40].x = vi2
			LowerOnly(VILW_L(mask::x, vi::vi7, vi::vi1, -0x120)),            // 9: vi7 = mem[0x30].x
			EBitNopPair(),                                                   // 10
			Nop(),                                                           // 11
		});
		h.Run();
		EXPECT_EQ(h.GetVfBitsJit(vf::vf2, 'x'), 0x40400000u);
		EXPECT_EQ(h.GetVfBitsJit(vf::vf2, 'x'), h.GetVfBitsInterp(vf::vf2, 'x'));
		EXPECT_EQ(h.GetViJit(vi::vi1), 0x123u);
		EXPECT_EQ(h.GetViJit(vi::vi2), 0x579u);
		EXPECT_EQ(h.GetViJit(vi::vi3), 0xFFF9u);
		EXPECT_EQ(h.GetViJit(vi::vi4), 0x57Eu);
		EXPECT_EQ(h.GetViJit(vi::vi5), 0x558u);
		EXPECT_EQ(h.GetViJit(vi::vi6), 0x1234u);
		EXPECT_EQ(h.GetViJit(vi::vi7), 0x1234u) << "a negative offset";
		EXPECT_EQ(h.GetMemU32Jit(0x40), 0x579u);
		ExpectJitMatchesInterp(h);
	}
}

// A branch into another chunk and a budget break both go through the
// placement's slots. Here they run as compiled, then, after a reset, from the
// chunk store's bodies made again from the pack at another place in the code
// cache, where the slots and the stubs that name them are new.
TEST(Vu1MicroAddress, CrossChunkBranchAndBudgetBreakSurviveReload)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	const u32 base = 0x0200;
	mVUPersist::SetRecordingEnabled(true);
	{
		ChunkPackScope pack;
		ASSERT_TRUE(pack.Up());
		VuTestHarness h(1);
		Place(h, base, CallInLoopProgram(40));
		h.Run();
		// (The branch reads vi1 as it was before the pair in front of it.)
		ASSERT_EQ(h.GetViJit(vi::vi2), 205u);
		ASSERT_EQ(h.GetViJit(vi::vi11), 123u);
		ExpectJitMatchesInterp(h);

		// Fresh compile that stays in the cache, then a run cut by its budget.
		h.RunJitPreserveBlockCache();
		h.SetCycleBudget(40);
		h.RunJitPreserveBlockCache();
		ASSERT_FALSE(h.HasTerminated());
		h.ContinueJit(40, 400);
		ASSERT_TRUE(h.HasTerminated());
		ExpectJitMatchesInterp(h);

		// Something else at the front of the code cache, so the bodies land
		// somewhere new and their placements get slots of their own.
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		WritePair(0x1000, LoadViImm(vi::vi3, 1));
		WritePair(0x1008, EBitNopPair());
		WritePair(0x1010, Nop());
		h.SetStartPc(0x1000);
		h.RunJitPreserveBlockCache();
		h.SetStartPc(base);
		// Micro memory as it was.
		for (u32 n = 0; n < 3; n++)
			WritePair(0x1000 + n * 8, VuOp{0, 0});
		CpuMicroVU1.Clear(0, kMicroBytes);

		const u64 compiled = mVUPersist::GetBlockCompileCount(1);
		h.RunJitPreserveBlockCache();
		ASSERT_FALSE(h.HasTerminated());
		h.ContinueJit(40, 400);
		ASSERT_TRUE(h.HasTerminated());
		EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), compiled) << "the reloaded program compiled something";
		ExpectJitMatchesInterp(h);
	}
	mVUPersist::SetRecordingEnabled(false);
	RecompilerTestEnvironment::ResetVuBlockCache(1);
}

// Under the IbitHack gamefix the entry stubs also store the placement's micro
// memory, which the reads are taken from. Code made again from the pack after
// a reset reads its immediates from micro memory as it is, a word rewritten
// after the load included.
TEST(Vu1MicroAddress, IbitHackImmediatesSurviveReload)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	ScopedIbitHack hack(true);
	const u32 base = 0x0200;
	mVUPersist::SetRecordingEnabled(true);
	{
		ChunkPackScope pack;
		ASSERT_TRUE(pack.Up());
		VuTestHarness h(1);
		h.SetVfBits(vf::vf1, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u);
		h.WriteMemU128(0x30, 0x1234, 0, 0, 0);
		h.WriteMemU128(0x50, 0x4321, 0, 0, 0);
		h.TrackMemWindow(0x40, 16);
		Place(h, base, {
			IBit(VuOp{VLitI(0x40400000u), VNOP_U()}),                        // 0: I = 3.0
			IBit(VuOp{VLitZero(), VMULi_U(mask::xyzw, vf::vf2, vf::vf1)}),   // 1: vf2 = vf1 * I
			LowerOnly(VIADDIU_L(vi::vi1, vi::vi0, 0x123)),                   // 2
			LowerOnly(VILW_L(mask::x, vi::vi6, vi::vi0, 3)),                 // 3: vi6 = mem[0x30].x
			LowerOnly(VISW_L(mask::x, vi::vi1, vi::vi0, 4)),                 // 4: mem[0x40].x = vi1
			EBitNopPair(),                                                   // 5
			Nop(),                                                           // 6
		});
		h.Run();
		ASSERT_EQ(h.GetViJit(vi::vi1), 0x123u);
		h.RunJitPreserveBlockCache();
		// Something else at the front of the code cache, so the bodies land
		// somewhere new.
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		std::vector<u8> saved(vuRegs[1].Micro + 0x1000, vuRegs[1].Micro + 0x1018);
		WritePair(0x1000, LoadViImm(vi::vi3, 1));
		WritePair(0x1008, EBitNopPair());
		WritePair(0x1010, Nop());
		h.SetStartPc(0x1000);
		h.RunJitPreserveBlockCache();
		h.SetStartPc(base);
		std::memcpy(vuRegs[1].Micro + 0x1000, saved.data(), saved.size());
		CpuMicroVU1.Clear(0, kMicroBytes);

		const u64 compiled = mVUPersist::GetBlockCompileCount(1);
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), compiled) << "the reloaded program compiled something";
		EXPECT_EQ(h.GetVfBitsJit(vf::vf2, 'x'), 0x40400000u);
		EXPECT_EQ(h.GetViJit(vi::vi1), 0x123u);
		EXPECT_EQ(h.GetViJit(vi::vi6), 0x1234u);

		// The ILW's offset, rewritten in place: the reloaded block reads it.
		const u32 ilw = VILW_L(mask::x, vi::vi6, vi::vi0, 5);
		std::memcpy(vuRegs[1].Micro + base + 3 * 8, &ilw, 4);
		CpuMicroVU1.Clear(base + 3 * 8, 4);
		h.RunJitPreserveBlockCache();
		EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), compiled);
		EXPECT_EQ(h.GetViJit(vi::vi6), 0x4321u) << "ILW of mem[0x50]";
	}
	mVUPersist::SetRecordingEnabled(false);
	RecompilerTestEnvironment::ResetVuBlockCache(1);
}

} // namespace recompiler_tests
