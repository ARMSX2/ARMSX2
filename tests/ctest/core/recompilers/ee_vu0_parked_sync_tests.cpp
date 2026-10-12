// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The run-ahead COP2 sync stub's parked-spin bounce (iCOP2-arm64.cpp,
// cop2EmitParkedSpinTest; armed by mVU0ArmParkedSpin in microVU-arm64.cpp).
//
// A VU0 program parked in a NOP-only wait loop (mvu_spin_ff_tests.cpp) is
// answered by recMicroVU0::Execute's mVUspinBounce without entering the
// recompiler: VU0.cycle advances by the floored grant and nothing else moves.
// The stub now gives that same answer itself, before its pin flush and the C
// call, whenever Execute has armed it. It may only skip host work:
//
//   1. With VU0 parked, an EE block that syncs many times (bounces, a
//      negative delta inside the 16-cycle floor, a release by CTC2 and the
//      real run it causes, re-parking) ends with VURegs, the EE registers
//      read through CFC2 (including VPU_STAT and TPC) and both clocks
//      byte-identical across three arms: the emitted spin head only, the C++
//      bounce only (master), and the stub's bounce. The stub arm must have
//      taken it: fewer Execute calls than the C++ arm.
//   2. A loop the detector rejects never arms the stub, and every sync of it
//      still reaches Execute.
//   3. A spin on two live VI registers, released by a CTC2 that leaves the
//      second operand's upper half dirty, is seen through 16-bit reads.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "R5900.h"
#include "VU.h"
#include "VUmicro.h"

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace mvu_test_hooks
{
	void SetSpinBounceEnabled(bool enabled);
	void SetParkedSyncEnabled(bool enabled);
	u64 GetSpinBounceCount(int vu_index);
	u64 GetVu0ExecuteCount();
	u32 GetParkedSpinWord();
} // namespace mvu_test_hooks

namespace mVUPersist
{
	void SetRecordingEnabled(bool enabled);
	bool IsRecordingEnabled();
} // namespace mVUPersist

namespace recompiler_tests {

using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 r_t0 = 8, r_t1 = 9, r_t2 = 10, r_t3 = 11, r_t4 = 12, r_t5 = 13;

// The detector wants the exact NOP encodings the games use.
constexpr u32 kUpperNop = 0x000002FFu;
constexpr u32 kLowerNop = 0x8000033Cu;
inline vu::VuOp ExactNopPair() { return vu::VuOp{kLowerNop, kUpperNop}; }
inline vu::VuOp SpinOp(u32 lower) { return vu::VuOp{lower, kUpperNop}; }
inline vu::VuOp LowerOnly(u32 lower) { return vu::VuOp{lower, vu::VNOP_U()}; }

// Not at micro address 0: a TPC of zero reads the same in both units.
constexpr u32 kVuBase = 0x200;

// UYA's shape: a job server that never ends. It waits while vi1 != 0, then
// counts a job in vi3, sets vi1 back to 1 and returns to waiting.
const std::vector<vu::VuOp> kServer = {
	SpinOp(vu::VIBEQ_L(vu::vi::vi1, vu::vi::vi0, 3)),        // 0: leave the wait when vi1 == 0
	ExactNopPair(),                                           // 1
	SpinOp(vu::VB_L(-3)),                                     // 2: back to 0
	ExactNopPair(),                                           // 3
	LowerOnly(vu::VIADDIU_L(vu::vi::vi3, vu::vi::vi3, 1)),   // 4: the job
	LowerOnly(vu::VIADDIU_L(vu::vi::vi1, vu::vi::vi0, 1)),   // 5: ready for the next one
	SpinOp(vu::VB_L(-7)),                                     // 6: back to 0
	ExactNopPair(),                                           // 7
};

// The same loop with a real op in the back-branch's delay slot, which the
// detector rejects: VU0 runs every iteration.
const std::vector<vu::VuOp> kBusyLoop = {
	SpinOp(vu::VIBEQ_L(vu::vi::vi1, vu::vi::vi0, 3)),
	ExactNopPair(),
	SpinOp(vu::VB_L(-3)),
	SpinOp(vu::VIADDI_L(vu::vi::vi4, vu::vi::vi4, 1)),
	LowerOnly(vu::VIADDIU_L(vu::vi::vi3, vu::vi::vi3, 1)),
	vu::EBitNopPair(),
	vu::NopPair(),
};

// A self-loop on two live registers: spins while vi1 != vi2, then ends.
const std::vector<vu::VuOp> kTwoRegister = {
	SpinOp(vu::VIBNE_L(vu::vi::vi1, vu::vi::vi2, -1)),
	ExactNopPair(),
	LowerOnly(vu::VIADDIU_L(vu::vi::vi3, vu::vi::vi0, 9)),
	vu::EBitNopPair(),
	vu::NopPair(),
};

std::vector<u32> Pad(std::vector<u32> prog, int nops)
{
	for (int i = 0; i < nops; i++)
		prog.push_back(NOP);
	return prog;
}

// The EE side of the handshake, the way UYA's feeder loop does it: CFC2
// reads and `CTC2 $zero, viN` writes, each a non-interlocked run-ahead sync
// (the zero write re-arms the analysis' sync for the next transfer). The
// NOP runs put more than the 16-cycle floor between most syncs.
std::vector<u32> ServerFeeder()
{
	std::vector<u32> p;
	p = Pad({CFC2(r_t0, vu::vi::vi3)}, 24);                 // bounce
	p.push_back(CTC2(0, vu::vi::vi2));                       // bounce, writes a register nobody polls
	p = Pad(p, 24);
	p.push_back(CFC2(r_t1, REG_VPU_STAT));                   // bounce; VU0 reads as running
	p = Pad(p, 24);
	p.push_back(CTC2(0, vu::vi::vi2));                       // bounce
	p.push_back(CTC2(0, vu::vi::vi2));                       // delta under the floor: VU0 runs ahead
	p.push_back(CTC2(0, vu::vi::vi2));                       // negative delta: nothing to do
	p = Pad(p, 24);
	p.push_back(CTC2(0, vu::vi::vi1));                       // bounce, then post the job
	p = Pad(p, 24);
	p.push_back(CFC2(r_t2, vu::vi::vi3));                    // the loop exits: VU0 runs the job
	p = Pad(p, 24);
	p.push_back(CTC2(0, vu::vi::vi2));                       // parked again
	p = Pad(p, 24);
	p.push_back(CTC2(0, vu::vi::vi2));
	p = Pad(p, 24);
	p.push_back(CFC2(r_t3, REG_TPC));
	p.push_back(CFC2(r_t4, vu::vi::vi1));
	return p;
}

std::vector<u32> TwoRegisterFeeder()
{
	std::vector<u32> p = {LUI(r_t5, 0x0008), ORI(r_t5, r_t5, 0x0001)};
	p.push_back(CFC2(r_t0, vu::vi::vi3));                    // bounce
	p = Pad(p, 24);
	p.push_back(CTC2(0, vu::vi::vi4));                       // bounce
	p = Pad(p, 24);
	p.push_back(CTC2(r_t5, vu::vi::vi2));                    // bounce, then vi2's low half = vi1
	p = Pad(p, 24);
	p.push_back(CTC2(0, vu::vi::vi4));                       // released: VU0 runs to its end
	p = Pad(p, 24);
	p.push_back(CFC2(r_t1, REG_VPU_STAT));
	p.push_back(CFC2(r_t2, vu::vi::vi3));
	return p;
}

enum class Arm
{
	EmittedHead, // neither bounce: every sync enters the recompiler
	CppBounce,   // master: Execute bounces
	StubBounce,  // the sync stub bounces when armed
};

const char* ArmName(Arm a)
{
	switch (a)
	{
		case Arm::EmittedHead: return "emitted head";
		case Arm::CppBounce: return "C++ bounce";
		default: return "stub bounce";
	}
}

struct ArmResult
{
	std::vector<u8> vu; // VURegs with the clock zeroed (compared separately)
	s64 vu_cycle;       // VU0.cycle - EE start cycle
	s64 ee_cycle;       // cpuRegs.cycle - EE start cycle
	u64 gpr[5];
	u64 executes;
	u64 cpp_bounces;
	u32 parked_word;
};

// VU0 on microVU for the arm, and back to the environment's provider after
// it, with the parked spin dropped (mVUreset disarms it).
class MicroVu0Scope
{
public:
	MicroVu0Scope(Arm arm) : m_prev(CpuVU0), m_recording(mVUPersist::IsRecordingEnabled())
	{
		mVUPersist::SetRecordingEnabled(false);
		mvu_test_hooks::SetSpinBounceEnabled(arm != Arm::EmittedHead);
		mvu_test_hooks::SetParkedSyncEnabled(arm == Arm::StubBounce);
		CpuVU0 = &CpuMicroVU0;
		CpuMicroVU0.Reset(); // every arm compiles its own blocks, memos unknown
	}
	~MicroVu0Scope()
	{
		CpuMicroVU0.Reset();
		CpuVU0 = m_prev;
		mvu_test_hooks::SetSpinBounceEnabled(true);
		mvu_test_hooks::SetParkedSyncEnabled(true);
		mVUPersist::SetRecordingEnabled(m_recording);
	}

private:
	BaseVUmicroCPU* m_prev;
	bool m_recording;
};

void WriteVuProgram(u32 base, const std::vector<vu::VuOp>& pairs)
{
	for (size_t i = 0; i < pairs.size(); i++)
	{
		std::memcpy(vuRegs[0].Micro + base + i * 8 + 0, &pairs[i].lower, 4);
		std::memcpy(vuRegs[0].Micro + base + i * 8 + 4, &pairs[i].upper, 4);
	}
	CpuMicroVU0.Clear(base, static_cast<u32>(pairs.size() * 8));
}

// Kicks the VU program, lets it reach its loop and dispatches it once more,
// so a parked spin has been through Execute's bounce (memoized and, in the
// stub arm, armed) before the EE block runs. Leaves VU0's clock even with
// the EE's.
ArmResult RunArm(Arm arm, const std::vector<vu::VuOp>& vu_prog,
	std::initializer_list<std::pair<u32, u32>> vi_seed, const std::vector<u32>& ee_prog)
{
	SCOPED_TRACE(ArmName(arm));
	MicroVu0Scope scope(arm);

	EeRecTestHarness h;
	h.EnableVu0Capture();
	WriteVuProgram(kVuBase, vu_prog);
	for (const auto& [reg, value] : vi_seed)
		vuRegs[0].VI[reg].UL = value;

	const u64 start = cpuRegs.cycle;
	vuRegs[0].cycle = start - 128;
	vuRegs[0].VI[REG_TPC].UL = kVuBase / 8;
	CpuMicroVU0.SetStartPC(kVuBase);
	vuRegs[0].VI[REG_VPU_STAT].UL |= 1;
	CpuMicroVU0.Execute(64);
	CpuMicroVU0.Execute(64);

	ArmResult r = {};
	const u64 executes_before = mvu_test_hooks::GetVu0ExecuteCount();
	const u64 bounces_before = mvu_test_hooks::GetSpinBounceCount(0);

	h.LoadProgram(ee_prog);
	h.RunJitNoDiff();

	r.executes = mvu_test_hooks::GetVu0ExecuteCount() - executes_before;
	r.cpp_bounces = mvu_test_hooks::GetSpinBounceCount(0) - bounces_before;
	r.parked_word = mvu_test_hooks::GetParkedSpinWord();
	r.vu_cycle = static_cast<s64>(vuRegs[0].cycle - start);
	r.ee_cycle = static_cast<s64>(cpuRegs.cycle - start);
	const u32 regs[] = {r_t0, r_t1, r_t2, r_t3, r_t4};
	for (int i = 0; i < 5; i++)
		r.gpr[i] = h.GetGpr64Jit(regs[i]);

	VURegs copy;
	std::memcpy(&copy, &vuRegs[0], sizeof(VURegs));
	copy.cycle = 0;
	r.vu.resize(sizeof(VURegs));
	std::memcpy(r.vu.data(), &copy, sizeof(VURegs));
	return r;
}

int FirstDiff(const std::vector<u8>& a, const std::vector<u8>& b)
{
	for (size_t i = 0; i < a.size(); i++)
	{
		if (a[i] != b[i])
			return static_cast<int>(i);
	}
	return -1;
}

void ExpectSameEmulatedState(const ArmResult& ref, const ArmResult& got, const char* what)
{
	SCOPED_TRACE(what);
	const int diff = FirstDiff(ref.vu, got.vu);
	EXPECT_EQ(diff, -1) << "VURegs differ at byte " << diff << " (VI starts at "
						<< offsetof(VURegs, VI) << ", flags at " << offsetof(VURegs, flags) << ")";
	EXPECT_EQ(ref.vu_cycle, got.vu_cycle) << "VU0 clock";
	EXPECT_EQ(ref.ee_cycle, got.ee_cycle) << "EE clock";
	for (int i = 0; i < 5; i++)
		EXPECT_EQ(ref.gpr[i], got.gpr[i]) << "t" << i;
}

} // namespace

TEST(EeVu0ParkedSync, StubBounceLeavesTheSameStateAsExecute)
{
	const std::vector<u32> ee = ServerFeeder();
	const std::initializer_list<std::pair<u32, u32>> seed = {{1, 1}, {3, 0}};
	const ArmResult head = RunArm(Arm::EmittedHead, kServer, seed, ee);
	const ArmResult cpp = RunArm(Arm::CppBounce, kServer, seed, ee);
	const ArmResult stub = RunArm(Arm::StubBounce, kServer, seed, ee);

	// The scenario did what it says, in the reference arm.
	EXPECT_EQ(head.gpr[1] & 1, 1u) << "VPU_STAT must read VU0 as running while it waits";
	EXPECT_EQ(head.gpr[2] & 0xFFFF, 1u) << "the posted job must have run exactly once";
	EXPECT_EQ(head.gpr[3], kVuBase / 8) << "TPC parks at the loop head";
	EXPECT_EQ(head.gpr[4] & 0xFFFF, 1u) << "the server re-arms its wait";
	EXPECT_EQ(head.cpp_bounces, 0u);
	EXPECT_GT(cpp.cpp_bounces, 0u) << "master's arm must have bounced in C++";

	ExpectSameEmulatedState(head, cpp, "C++ bounce vs emitted head");
	ExpectSameEmulatedState(head, stub, "stub bounce vs emitted head");

	// Not vacuous: the stub answered syncs that master sent to Execute. One
	// sync reaches Execute for the released loop's real run, and one more for
	// the first bounce on the block the server loops back into.
	EXPECT_LT(stub.executes + 4, cpp.executes)
		<< "the stub must have bounced (Execute calls: stub " << stub.executes << ", C++ " << cpp.executes << ")";
	EXPECT_NE(stub.parked_word, 0u) << "a re-parked server must be armed again";
}

TEST(EeVu0ParkedSync, RejectedLoopNeverArms)
{
	const std::vector<u32> ee = ServerFeeder();
	const std::initializer_list<std::pair<u32, u32>> seed = {{1, 1}, {3, 0}, {4, 0}};
	const ArmResult cpp = RunArm(Arm::CppBounce, kBusyLoop, seed, ee);
	const ArmResult stub = RunArm(Arm::StubBounce, kBusyLoop, seed, ee);

	u16 iterations;
	std::memcpy(&iterations, &stub.vu[offsetof(VURegs, VI) + vu::vi::vi4 * sizeof(REG_VI)], sizeof(iterations));
	EXPECT_GT(iterations, 0u) << "the loop must have iterated for real";
	ExpectSameEmulatedState(cpp, stub, "stub arm vs C++ arm");
	EXPECT_EQ(stub.executes, cpp.executes) << "every sync of a running loop must reach Execute";
	EXPECT_EQ(stub.cpp_bounces, 0u);
	EXPECT_EQ(stub.parked_word, 0u);
}

TEST(EeVu0ParkedSync, TwoRegisterSpinReleasedThroughTheLowHalf)
{
	const std::vector<u32> ee = TwoRegisterFeeder();
	const std::initializer_list<std::pair<u32, u32>> seed = {{1, 1}, {2, 5}, {3, 0}};
	const ArmResult head = RunArm(Arm::EmittedHead, kTwoRegister, seed, ee);
	const ArmResult cpp = RunArm(Arm::CppBounce, kTwoRegister, seed, ee);
	const ArmResult stub = RunArm(Arm::StubBounce, kTwoRegister, seed, ee);

	EXPECT_EQ(head.gpr[1] & 1, 0u) << "the released program must have ended";
	EXPECT_EQ(head.gpr[2] & 0xFFFF, 9u);
	ExpectSameEmulatedState(head, cpp, "C++ bounce vs emitted head");
	ExpectSameEmulatedState(head, stub, "stub bounce vs emitted head");
	EXPECT_LT(stub.executes, cpp.executes) << "the stub must have bounced";
}

} // namespace recompiler_tests
