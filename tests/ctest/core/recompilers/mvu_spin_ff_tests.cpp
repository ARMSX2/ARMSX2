// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU0 spin-wait fast-forward (mVUdetectSpinLoop / mVUemitSpinFF,
// microVU_Compile-arm64.inl).
//
// EE-handshake busy-wait loops — a conditional VI branch whose whole loop
// body is exact-NOP encodings — dominate VU0 execution in macro-handshake
// titles (UYA gameplay: 89% of VU0 micro cycles are three such loops). The
// FF consumes the entire remaining cycle grant in one step whenever the
// spin condition still holds: nothing can release the spin mid-grant (the
// EE thread is stalled inside Execute; only the EE writes the handshake
// VI), so N spin iterations are architecturally identical to none plus a
// cycle skip.
//
// Contracts pinned here:
//   1. A held spin consumes the whole grant (VU0.cycle advances by the
//      full budget) with ZERO architectural side effects, stays running,
//      and parks TPC at the loop head (the VE-07 resume slot).
//   2. Releasing the handshake VI exits the loop and the program completes
//      identically to a never-spun run.
//   3. Both recognized shapes FF: the 4-pair {cond; NOP; B ->head; NOP}
//      and the 2-pair self-loop {cond ->self; NOP}.
//   4. NEGATIVE: a loop whose body has any real op (non-NOP encoding) is
//      NOT fast-forwarded — it must execute its iterations for real.
//   5. The bounce a held spin makes — break, re-dispatch, break again —
//      is taken in C++ (mVUspinBounce) without entering the recompiler,
//      and a run that takes it is bit-identical in VURegs to the same run
//      forced through the emitted head.

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "R5900.h"
#include "VU.h"
#include "VUmicro.h"

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

// Test hooks exported by pcsx2/arm64/microVU-arm64.cpp under
// PCSX2_RECOMPILER_TESTS (same cross-TU pattern as
// mvu_resume_dispatch_tests.cpp).
namespace mvu_test_hooks
{
	void SetSpinBounceEnabled(bool enabled);
	u64 GetSpinBounceCount(int vu_index);
	void PoisonIrBlockSpinMemo(int vu_index);
} // namespace mvu_test_hooks

// The C++ bounce, like the resume fast path it rides, is gated off while
// ProgCache recording is on. Force it off so these tests do not depend on
// suite ordering.
namespace mVUPersist
{
	void SetRecordingEnabled(bool enabled);
	bool IsRecordingEnabled();
} // namespace mVUPersist

namespace recompiler_tests {

using namespace vu;

namespace {

// The detector requires the EXACT architectural NOP encodings the games use
// (upper 0x000002FF, lower 0x8000033C) — the harness's generic NopPair()
// uses an I-bit literal-zero pair, which is deliberately NOT recognized.
constexpr u32 kUpperNop = 0x000002FFu;
constexpr u32 kLowerNop = 0x8000033Cu;
inline VuOp ExactNopPair() { return VuOp{kLowerNop, kUpperNop}; }
inline VuOp SpinOp(u32 lower) { return VuOp{lower, kUpperNop}; }
inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }

void SeedVu0Dispatch(u32 start_pc_bytes)
{
	vuRegs[0].VI[REG_TPC].UL = start_pc_bytes / 8u;
	CpuMicroVU0.SetStartPC(start_pc_bytes);
	vuRegs[0].VI[REG_VPU_STAT].UL |= 0x1u;
}

bool Vu0Busy()
{
	return (vuRegs[0].VI[REG_VPU_STAT].UL & 0x1u) != 0;
}

// The UYA shape: spin at head while vi1 != 0, then release code sets vi3
// and ends. Loop exit is the IBEQ taken arm (+3 pairs → pair 4).
// Loaded with vi1 = 0 (released) so Run()'s JIT-vs-interp diff exercises
// the deterministic exit path; the spin assertions re-dispatch directly.
void LoadUyaShapeProgram(VuTestHarness& h)
{
	h.SetVi(vi::vi1, 0); // released for the Run() equivalence pass
	h.LoadProgram({
		SpinOp(VIBEQ_L(vi::vi1, vi::vi0, 3)),  // pair 0: exit when vi1 == 0
		ExactNopPair(),                        // pair 1: branch delay slot
		SpinOp(VB_L(-3)),                      // pair 2: back to pair 0
		ExactNopPair(),                        // pair 3: branch delay slot
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 42)), // pair 4: loop exit
		EBitNopPair(),
	});
}

class RecordingOffScope
{
public:
	RecordingOffScope() : m_prev(mVUPersist::IsRecordingEnabled())
	{
		mVUPersist::SetRecordingEnabled(false);
	}
	~RecordingOffScope() { mVUPersist::SetRecordingEnabled(m_prev); }

private:
	bool m_prev;
};

class SpinBounceScope
{
public:
	explicit SpinBounceScope(bool enabled) { mvu_test_hooks::SetSpinBounceEnabled(enabled); }
	~SpinBounceScope() { mvu_test_hooks::SetSpinBounceEnabled(true); }
};

// Byte offset of the first difference, or -1.
int FirstDiff(const std::vector<u8>& a, const std::vector<u8>& b)
{
	for (size_t i = 0; i < a.size(); i++)
	{
		if (a[i] != b[i])
			return static_cast<int>(i);
	}
	return -1;
}

// The whole VU0 register file, including everything the break writes back:
// the flag shadows, Q/pending_q, TPC, cycle and nextBlockCycles.
std::vector<u8> SnapshotVu0()
{
	std::vector<u8> out(sizeof(VURegs));
	std::memcpy(out.data(), &vuRegs[0], sizeof(VURegs));
	return out;
}

// Where the differential's copy of the loop lives. A program at the start of
// micro memory parks TPC at zero, which reads the same whether or not the
// bounce scales it back into a byte address — so the differential must not
// run from there.
constexpr u32 kSpinBase = 0x200;

void WriteUyaShapeAt(u32 base)
{
	const VuOp pairs[] = {
		SpinOp(VIBEQ_L(vi::vi1, vi::vi0, 3)),       // pair 0: exit when vi1 == 0
		ExactNopPair(),                             // pair 1: branch delay slot
		SpinOp(VB_L(-3)),                           // pair 2: back to pair 0
		ExactNopPair(),                             // pair 3: branch delay slot
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 42)), // pair 4: loop exit
		EBitNopPair(),
	};
	for (u32 i = 0; i < std::size(pairs); i++)
	{
		std::memcpy(vuRegs[0].Micro + base + i * 8 + 0, &pairs[i].lower, 4);
		std::memcpy(vuRegs[0].Micro + base + i * 8 + 4, &pairs[i].upper, 4);
	}
	CpuMicroVU0.Clear(base, static_cast<u32>(sizeof(pairs)));
}

} // namespace

TEST(MvuSpinFF, HeldSpinConsumesWholeGrantWithoutSideEffects)
{
	VuTestHarness h(0);
	LoadUyaShapeProgram(h);
	h.Run(); // seeds micro mem + verifies base JIT-vs-interp equivalence of
	         // the released path (Run() flips nothing: vi1 == 1 spins until
	         // the harness budget starves both sides identically)

	// Direct production dispatch: a held spin must consume the entire grant.
	vuRegs[0].VI[vi::vi1].UL = 1;
	vuRegs[0].VI[vi::vi3].UL = 0;
	const u64 cycle_before = vuRegs[0].cycle;
	SeedVu0Dispatch(0);
	CpuMicroVU0.Execute(1000);

	EXPECT_TRUE(Vu0Busy()) << "a held spin must stay running";
	EXPECT_EQ(vuRegs[0].cycle - cycle_before, 1000u)
		<< "the FF must bank the whole grant into VU0.cycle";
	EXPECT_EQ(vuRegs[0].VI[vi::vi1].UL & 0xFFFFu, 1u) << "spin must not write VI";
	EXPECT_EQ(vuRegs[0].VI[vi::vi3].UL & 0xFFFFu, 0u) << "exit code must not run";
	EXPECT_EQ(vuRegs[0].VI[REG_TPC].UL, 0u)
		<< "TPC must park at the spin head for the resume re-entry";

	// Release: the next grant exits the loop and completes the program.
	vuRegs[0].VI[vi::vi1].UL = 0;
	CpuMicroVU0.Execute(1000);
	EXPECT_FALSE(Vu0Busy()) << "released spin must run to the E-bit end";
	EXPECT_EQ(vuRegs[0].VI[vi::vi3].UL & 0xFFFFu, 42u);
}

TEST(MvuSpinFF, SelfLoopShapeFastForwards)
{
	VuTestHarness h(0);
	h.SetVi(vi::vi1, 1);
	h.LoadProgram({
		SpinOp(VIBNE_L(vi::vi1, vi::vi0, -1)),      // pair 0: spin while vi1 != 0
		ExactNopPair(),                             // pair 1: delay slot
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 7)),  // pair 2: fallthrough exit
		EBitNopPair(),
	});
	h.Run();

	vuRegs[0].VI[vi::vi1].UL = 1;
	vuRegs[0].VI[vi::vi3].UL = 0;
	const u64 cycle_before = vuRegs[0].cycle;
	SeedVu0Dispatch(0);
	CpuMicroVU0.Execute(500);

	EXPECT_TRUE(Vu0Busy());
	EXPECT_EQ(vuRegs[0].cycle - cycle_before, 500u)
		<< "self-loop shape must FF the whole grant";
	EXPECT_EQ(vuRegs[0].VI[REG_TPC].UL, 0u);

	vuRegs[0].VI[vi::vi1].UL = 0;
	CpuMicroVU0.Execute(500);
	EXPECT_FALSE(Vu0Busy());
	EXPECT_EQ(vuRegs[0].VI[vi::vi3].UL & 0xFFFFu, 7u);
}

TEST(MvuSpinFF, SideEffectfulLoopIsNotFastForwarded)
{
	// Same 4-pair shape but the B delay slot increments vi4 — a real op, so
	// the detector must reject it and the loop must execute for real.
	//
	// The branch compares two held-equal registers on purpose. The detector
	// records viA/viB before it decides, so a rejected block still carries a
	// pair, and one that compares equal is what the emitted head would call a
	// held spin: only the rejection itself keeps the loop running.
	RecordingOffScope recording_off;
	VuTestHarness h(0);
	h.SetVi(vi::vi1, 1);
	h.SetVi(vi::vi2, 1);
	h.SetVi(vi::vi4, 0);
	h.LoadProgram({
		SpinOp(VIBNE_L(vi::vi1, vi::vi2, 3)),       // pair 0: exit when vi1 != vi2
		ExactNopPair(),                             // pair 1
		SpinOp(VB_L(-3)),                           // pair 2
		SpinOp(VIADDI_L(vi::vi4, vi::vi4, 1)),      // pair 3: REAL op in ds
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 42)), // pair 4: exit
		EBitNopPair(),
	});
	h.Run();

	vuRegs[0].VI[vi::vi1].UL = 1;
	vuRegs[0].VI[vi::vi2].UL = 1;
	vuRegs[0].VI[vi::vi4].UL = 0;
	SeedVu0Dispatch(0);
	CpuMicroVU0.Execute(400);

	EXPECT_TRUE(Vu0Busy());
	EXPECT_GT(vuRegs[0].VI[vi::vi4].UL & 0xFFFFu, 10u)
		<< "a side-effectful loop must iterate for real (no FF)";

	// It breaks on budget like any other block, so the next dispatch arrives
	// with a resume parked — and must still run, not bounce.
	const u64 bounces_before = mvu_test_hooks::GetSpinBounceCount(0);
	const u32 iterations_before = vuRegs[0].VI[vi::vi4].UL & 0xFFFFu;
	CpuMicroVU0.Execute(400);
	EXPECT_EQ(mvu_test_hooks::GetSpinBounceCount(0), bounces_before)
		<< "a block the detector rejected must never bounce";
	EXPECT_GT(vuRegs[0].VI[vi::vi4].UL & 0xFFFFu, iterations_before);
}

// M13's target: the memo belongs to the block, and a compile must not carry
// one in from the IR block it is copied out of.
TEST(MvuSpinFF, FreshBlockDoesNotInheritAPoisonedMemo)
{
	RecordingOffScope recording_off;
	VuTestHarness h(0);
	h.SetVi(vi::vi1, 1);
	h.SetVi(vi::vi2, 0);
	h.LoadProgram({
		SpinOp(VIBEQ_L(vi::vi1, vi::vi0, 3)),       // pair 0
		ExactNopPair(),                             // pair 1
		SpinOp(VB_L(-3)),                           // pair 2
		SpinOp(VIADDI_L(vi::vi2, vi::vi2, 1)),      // pair 3: real op in ds
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 42)), // pair 4: exit
		EBitNopPair(),
	});
	CpuMicroVU0.Clear(0, 48); // force a compile of the block below

	vuRegs[0].VI[vi::vi1].UL = 1;
	vuRegs[0].VI[vi::vi2].UL = 0;
	SeedVu0Dispatch(0);
	mvu_test_hooks::PoisonIrBlockSpinMemo(0);
	CpuMicroVU0.Execute(400);

	const u64 bounces_before = mvu_test_hooks::GetSpinBounceCount(0);
	const u32 iterations_before = vuRegs[0].VI[vi::vi2].UL & 0xFFFFu;
	CpuMicroVU0.Execute(400);
	EXPECT_EQ(mvu_test_hooks::GetSpinBounceCount(0), bounces_before)
		<< "the installed block must start with no memo";
	EXPECT_GT(vuRegs[0].VI[vi::vi2].UL & 0xFFFFu, iterations_before);
}

// Contract 5, first half: after the emitted head has parked the resume once,
// every further held-spin dispatch is answered in C++. The count is what
// keeps the state assertions from passing vacuously through the recompiler.
TEST(MvuSpinFF, HeldSpinBouncesInCppAfterTheFirstBreak)
{
	RecordingOffScope recording_off;
	VuTestHarness h(0);
	LoadUyaShapeProgram(h);
	h.Run();

	vuRegs[0].VI[vi::vi1].UL = 1;
	vuRegs[0].VI[vi::vi3].UL = 0;
	SeedVu0Dispatch(0);

	// The kick disarmed the resume slot, so this one goes through the JIT.
	const u64 bounces_before = mvu_test_hooks::GetSpinBounceCount(0);
	CpuMicroVU0.Execute(64);
	EXPECT_EQ(mvu_test_hooks::GetSpinBounceCount(0) - bounces_before, 0u)
		<< "the first dispatch after a kick has no parked resume to test";

	const u64 cycle_before = vuRegs[0].cycle;
	for (int i = 0; i < 5; i++)
		CpuMicroVU0.Execute(64);

	EXPECT_EQ(mvu_test_hooks::GetSpinBounceCount(0) - bounces_before, 5u)
		<< "every further held-spin dispatch must be answered without the JIT";
	EXPECT_EQ(vuRegs[0].cycle - cycle_before, 5u * 64u)
		<< "each bounce banks its whole grant";
	EXPECT_TRUE(Vu0Busy());
	EXPECT_EQ(vuRegs[0].VI[REG_TPC].UL, 0u) << "TPC must stay parked at the loop head";
	EXPECT_EQ(vuRegs[0].VI[vi::vi1].UL & 0xFFFFu, 1u);
	EXPECT_EQ(vuRegs[0].VI[vi::vi3].UL & 0xFFFFu, 0u);

	// Release: the exit condition holds, so the bounce declines and the
	// program runs to its E-bit end through the recompiler.
	vuRegs[0].VI[vi::vi1].UL = 0;
	const u64 bounces_at_release = mvu_test_hooks::GetSpinBounceCount(0);
	CpuMicroVU0.Execute(1000);
	EXPECT_EQ(mvu_test_hooks::GetSpinBounceCount(0), bounces_at_release)
		<< "a released spin must not be bounced";
	EXPECT_FALSE(Vu0Busy());
	EXPECT_EQ(vuRegs[0].VI[vi::vi3].UL & 0xFFFFu, 42u);
}

// Contract 5, second half: the same sliced run, once bounced in C++ and once
// forced through the emitted head, ends bit-identical in VURegs. This is the
// assertion that fails if the banking the exit stub does inline (VU0.cycle,
// and under EECycleSkip the EE clock the consumed count is scaled into) is
// replicated wrong.
TEST(MvuSpinFF, BounceIsBitIdenticalToTheEmittedHead)
{
	RecordingOffScope recording_off;

	// Uneven grants: the banked count differs per dispatch, so a replication
	// that only works for one grant size cannot pass. 4096 straddles the
	// EE skip's 3000-cycle clamp.
	static constexpr u32 kGrants[] = {17, 64, 3, 250, 1, 4096, 33, 7};

	struct Result
	{
		std::vector<u8> regs;
		u64 ee_cycle;
		u64 bounces;
	};

	auto sliced_run = [](bool bounce_in_cpp) {
		SpinBounceScope scope(bounce_in_cpp);
		VuTestHarness h(0);
		WriteUyaShapeAt(kSpinBase);

		vuRegs[0].VI[vi::vi1].UL = 1;
		vuRegs[0].VI[vi::vi3].UL = 0;
		cpuRegs.cycle = 0;
		SeedVu0Dispatch(kSpinBase);
		const u64 bounces_before = mvu_test_hooks::GetSpinBounceCount(0);
		for (const u32 grant : kGrants)
			CpuMicroVU0.Execute(grant);

		// Release and finish, so the comparison covers the state the spin
		// left behind being picked up by a real run.
		vuRegs[0].VI[vi::vi1].UL = 0;
		CpuMicroVU0.Execute(1000);

		return Result{SnapshotVu0(), cpuRegs.cycle,
			mvu_test_hooks::GetSpinBounceCount(0) - bounces_before};
	};

	const u8 prev_skip = EmuConfig.Speedhacks.EECycleSkip;
	for (const u8 ee_skip : {u8(0), u8(3)})
	{
		SCOPED_TRACE(testing::Message() << "EECycleSkip=" << static_cast<int>(ee_skip));
		EmuConfig.Speedhacks.EECycleSkip = ee_skip;

		const Result emitted = sliced_run(false);
		const Result bounced = sliced_run(true);

		EXPECT_EQ(emitted.bounces, 0u) << "the forced arm must never bounce in C++";
		EXPECT_EQ(bounced.bounces, std::size(kGrants) - 1)
			<< "every dispatch after the kick must bounce in C++";
		EXPECT_FALSE(Vu0Busy());
		EXPECT_EQ(emitted.ee_cycle, bounced.ee_cycle) << "EE clock skip must match";

		const int diff = FirstDiff(emitted.regs, bounced.regs);
		EXPECT_EQ(diff, -1) << "VURegs differ at byte " << diff
							<< " (VI starts at " << offsetof(VURegs, VI) << ", cycle at "
							<< offsetof(VURegs, cycle) << ")";
	}
	EmuConfig.Speedhacks.EECycleSkip = prev_skip;
}

// Contract 5, third: a spin on two live VI registers. Every loop the games
// run compares against vi0, so nothing else here would notice a bounce that
// never reads the second operand.
TEST(MvuSpinFF, TwoRegisterSpinReadsBothOperands)
{
	RecordingOffScope recording_off;
	VuTestHarness h(0);
	h.SetVi(vi::vi1, 5);
	h.SetVi(vi::vi2, 5); // released for Run()'s equivalence pass
	h.LoadProgram({
		SpinOp(VIBNE_L(vi::vi1, vi::vi2, -1)),     // pair 0: spin while vi1 != vi2
		ExactNopPair(),                            // pair 1: delay slot
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 9)), // pair 2: fallthrough exit
		EBitNopPair(),
	});
	h.Run();

	vuRegs[0].VI[vi::vi1].UL = 1;
	vuRegs[0].VI[vi::vi2].UL = 5;
	vuRegs[0].VI[vi::vi3].UL = 0;
	SeedVu0Dispatch(0);
	CpuMicroVU0.Execute(64); // through the JIT: arms the resume

	const u64 bounces_before = mvu_test_hooks::GetSpinBounceCount(0);
	CpuMicroVU0.Execute(64);
	CpuMicroVU0.Execute(64);
	EXPECT_EQ(mvu_test_hooks::GetSpinBounceCount(0) - bounces_before, 2u);
	EXPECT_TRUE(Vu0Busy());

	// Release by moving the second operand onto the first, leaving vi1 alone.
	// CTC2 can leave a VI register's upper half dirty and the compare the
	// guest makes is 16-bit, so the release rides bits the read must drop.
	vuRegs[0].VI[vi::vi2].UL = 0x00080001u;
	CpuMicroVU0.Execute(64);
	EXPECT_FALSE(Vu0Busy()) << "the release must be seen through the second operand";
	EXPECT_EQ(vuRegs[0].VI[vi::vi3].UL & 0xFFFFu, 9u);
}

// The bounce stands aside under the VU sync gamefixes: their break site also
// stores the block's own cycle count into nextBlockCycles, which the block's
// encoding does not carry.
TEST(MvuSpinFF, SyncHackKeepsTheSpinInTheRecompiler)
{
	RecordingOffScope recording_off;
	const bool prev_sync = EmuConfig.Gamefixes.VUSyncHack;
	const bool prev_full = EmuConfig.Gamefixes.FullVU0SyncHack;

	for (int which = 0; which < 2; which++)
	{
		SCOPED_TRACE(which == 0 ? "VUSyncHack" : "FullVU0SyncHack");
		EmuConfig.Gamefixes.VUSyncHack = (which == 0);
		EmuConfig.Gamefixes.FullVU0SyncHack = (which == 1);
		CpuMicroVU0.Reset();

		VuTestHarness h(0);
		LoadUyaShapeProgram(h);
		h.Run();

		vuRegs[0].VI[vi::vi1].UL = 1;
		vuRegs[0].VI[vi::vi3].UL = 0;
		SeedVu0Dispatch(0);
		const u64 bounces_before = mvu_test_hooks::GetSpinBounceCount(0);
		for (int i = 0; i < 4; i++)
			CpuMicroVU0.Execute(64);

		EXPECT_EQ(mvu_test_hooks::GetSpinBounceCount(0), bounces_before)
			<< "the sync hacks must keep every dispatch on the emitted path";
		EXPECT_TRUE(Vu0Busy());

		vuRegs[0].VI[vi::vi1].UL = 0;
		CpuMicroVU0.Execute(1000);
		EXPECT_FALSE(Vu0Busy());
		EXPECT_EQ(vuRegs[0].VI[vi::vi3].UL & 0xFFFFu, 42u);
	}

	EmuConfig.Gamefixes.VUSyncHack = prev_sync;
	EmuConfig.Gamefixes.FullVU0SyncHack = prev_full;
	CpuMicroVU0.Reset();
}

// The memo is a property of the block, not of the slot it was allocated in:
// a recompile at the same PC and entry state can land on a freed block's
// allocation, and its answer must not be inherited.
TEST(MvuSpinFF, RecompiledBlockDoesNotInheritTheMemo)
{
	RecordingOffScope recording_off;
	VuTestHarness h(0);

	// First program: a self-loop on vi1, bounced until vi1 matches vi0.
	h.SetVi(vi::vi1, 0);
	h.LoadProgram({
		SpinOp(VIBNE_L(vi::vi1, vi::vi0, -1)),
		ExactNopPair(),
		LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 7)),
		EBitNopPair(),
	});
	h.Run();

	vuRegs[0].VI[vi::vi1].UL = 1;
	SeedVu0Dispatch(0);
	CpuMicroVU0.Execute(64);
	const u64 bounces_before = mvu_test_hooks::GetSpinBounceCount(0);
	CpuMicroVU0.Execute(64);
	ASSERT_EQ(mvu_test_hooks::GetSpinBounceCount(0) - bounces_before, 1u)
		<< "the first program must reach the memo";

	// Second program at the same PC and entry state, spinning on vi2 instead.
	// vi1 stays held at 1, so a memo carried over from the first block would
	// keep bouncing on a register this loop never reads.
	vuRegs[0].VI[vi::vi2].UL = 0;
	vuRegs[0].VI[vi::vi3].UL = 0;
	{
		const VuOp pairs[] = {
			SpinOp(VIBNE_L(vi::vi2, vi::vi0, -1)),
			ExactNopPair(),
			LowerOnly(VIADDIU_L(vi::vi3, vi::vi0, 11)),
			EBitNopPair(),
		};
		for (u32 i = 0; i < std::size(pairs); i++)
		{
			std::memcpy(vuRegs[0].Micro + i * 8 + 0, &pairs[i].lower, 4);
			std::memcpy(vuRegs[0].Micro + i * 8 + 4, &pairs[i].upper, 4);
		}
		CpuMicroVU0.Clear(0, static_cast<u32>(sizeof(pairs)));
	}

	SeedVu0Dispatch(0);
	CpuMicroVU0.Execute(64);
	EXPECT_FALSE(Vu0Busy()) << "the recompiled loop is released and must run";
	EXPECT_EQ(vuRegs[0].VI[vi::vi3].UL & 0xFFFFu, 11u);
}

} // namespace recompiler_tests
