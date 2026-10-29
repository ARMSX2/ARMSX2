// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// End-to-end coverage for the VU capture-replay path: synthesize a tiny
// CaptureRecord (VADD pre-state in VF1/VF2, E-bit terminator) and replay
// it through ReplayCapture. Asserts the JIT produces the architecturally
// expected result *and* converges with the interpreter — i.e. the replay
// driver primes both engines from the captured state correctly.

#include "harness/VuReplay.h"
#include "harness/VuEncode.h"

#include "vu_capture.h"
#include "Config.h"
#include "Vif.h"
#include "VU.h"
#include "VUmicro.h"

#include "common/FPControl.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <gtest/gtest.h>

namespace recompiler_tests {

using namespace vu;

namespace
{

// Pad a one-pair-plus-NOP-delay-slot program into a full ProgSize buffer so
// the CaptureRecord's microcode field passes the format size check.
std::vector<u8> BuildProgramBytes(int vu_index, std::initializer_list<VuOp> pairs)
{
	const u32 prog_size = vu_index ? VU1_PROGSIZE : VU0_PROGSIZE;
	std::vector<u8> bytes(prog_size, 0);
	size_t off = 0;
	for (const auto& p : pairs)
	{
		std::memcpy(bytes.data() + off + 0, &p.lower, 4);
		std::memcpy(bytes.data() + off + 4, &p.upper, 4);
		off += 8;
	}
	return bytes;
}

vu_capture::CaptureRecord MakeVaddProgram(int vu_index)
{
	vu_capture::CaptureRecord rec;
	rec.vu_index = static_cast<u8>(vu_index);
	rec.start_pc = 0;
	rec.cycle_budget = 4096;

	// Pair 0: VADD.xyzw vf3, vf1, vf2 (upper) + I-bit-skipped lower.
	const VuOp p0 = IBit(VuOp{VLitZero(), VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)});
	// Pair 1: E-bit NOP.
	const VuOp p1 = EBitNopPair();
	// Pair 2: NOP (architectural delay slot after E-bit).
	const VuOp p2 = NopPair();
	rec.microcode = BuildProgramBytes(vu_index, {p0, p1, p2});
	rec.vumem.assign(vu_index ? VU1_MEMSIZE : VU0_MEMSIZE, 0);

	// Seed pre-state: VF1 = (1, 2, 3, 4), VF2 = (10, 20, 30, 40); expect
	// VF3 = (11, 22, 33, 44) post-execution.
	const float vf1[4] = {1.0f, 2.0f, 3.0f, 4.0f};
	const float vf2[4] = {10.0f, 20.0f, 30.0f, 40.0f};
	for (int i = 0; i < 4; ++i)
	{
		std::memcpy(&rec.state.VF[1][i], &vf1[i], 4);
		std::memcpy(&rec.state.VF[2][i], &vf2[i], 4);
	}
	return rec;
}

float AsFloat(u32 bits) { float f; std::memcpy(&f, &bits, 4); return f; }

// Portable, collision-free temp path (mirrors vu_capture_format_tests.cpp).
// std::filesystem::temp_directory_path() honors TMPDIR on macOS — /tmp is not
// the conventional temp dir there — and the getpid() suffix keeps parallel
// ctest -j workers from racing on a shared filename.
std::string MakeTempPath(const char* tag)
{
	std::filesystem::path p = std::filesystem::temp_directory_path() /
		(std::string("pcsx2-vureplay-test-") + tag + "-" + std::to_string(::getpid()) + ".vucap");
	return p.string();
}

std::string DiffText(const VuReplayResult& r)
{
	std::string s;
	for (const auto& l : r.diff_lines) { s += "  "; s += l; s += "\n"; }
	return s;
}

// VU1: XTOP vi1, XITOP vi2, then E-bit. The program's whole result is the
// two VIF registers it read.
vu_capture::CaptureRecord MakeVifReadProgram()
{
	vu_capture::CaptureRecord rec;
	rec.vu_index = 1;
	rec.start_pc = 0;
	rec.cycle_budget = 4096;
	rec.microcode = BuildProgramBytes(1, {
		VuOp{VXTOP_L(vi::vi1), VNOP_U()},
		VuOp{VXITOP_L(vi::vi2), VNOP_U()},
		EBitNopPair(),
		NopPair(),
	});
	rec.vumem.assign(VU1_MEMSIZE, 0);
	return rec;
}

} // namespace

TEST(VuReplay, ReplayVu1VaddProducesExpectedAndDoesNotDiverge)
{
	const auto rec = MakeVaddProgram(1);
	const auto result = ReplayCapture(rec);

	ASSERT_TRUE(result.ok);
	EXPECT_FALSE(result.diverged) << [&] {
		std::string s;
		for (const auto& l : result.diff_lines) { s += "  "; s += l; s += "\n"; }
		return s;
	}();

	const auto& jit_vf3 = result.jit_snapshot.regs.VF[3];
	EXPECT_FLOAT_EQ(AsFloat(jit_vf3.i.x), 11.0f);
	EXPECT_FLOAT_EQ(AsFloat(jit_vf3.i.y), 22.0f);
	EXPECT_FLOAT_EQ(AsFloat(jit_vf3.i.z), 33.0f);
	EXPECT_FLOAT_EQ(AsFloat(jit_vf3.i.w), 44.0f);

	const auto& interp_vf3 = result.interp_snapshot.regs.VF[3];
	EXPECT_FLOAT_EQ(AsFloat(interp_vf3.i.x), 11.0f);
	EXPECT_FLOAT_EQ(AsFloat(interp_vf3.i.y), 22.0f);
	EXPECT_FLOAT_EQ(AsFloat(interp_vf3.i.z), 33.0f);
	EXPECT_FLOAT_EQ(AsFloat(interp_vf3.i.w), 44.0f);
}

TEST(VuReplay, ReplayVu0VaddProducesExpectedAndDoesNotDiverge)
{
	const auto rec = MakeVaddProgram(0);
	const auto result = ReplayCapture(rec);

	ASSERT_TRUE(result.ok);
	// Print the diff, same as the VU1 twin above -- a bare EXPECT_FALSE on
	// `diverged` says nothing about WHICH register moved, which made an
	// order-dependent failure here far more expensive to triage than it
	// needed to be.
	EXPECT_FALSE(result.diverged) << [&] {
		std::string s;
		for (const auto& l : result.diff_lines) { s += "  "; s += l; s += "\n"; }
		return s;
	}();

	const auto& jit_vf3 = result.jit_snapshot.regs.VF[3];
	EXPECT_FLOAT_EQ(AsFloat(jit_vf3.i.x), 11.0f);
	EXPECT_FLOAT_EQ(AsFloat(jit_vf3.i.y), 22.0f);
	EXPECT_FLOAT_EQ(AsFloat(jit_vf3.i.z), 33.0f);
	EXPECT_FLOAT_EQ(AsFloat(jit_vf3.i.w), 44.0f);
}

// A replay must not inherit the interpreter's live flag accumulators from
// whatever ran before it.
//
// vu_capture::CapturedState carries microVU's four-deep micro_statusflags[]
// shadows but not the interpreter's scalar VU->statusflag/macflag/clipflag, so
// RestoreState has to derive those from VI[] -- see the comment there. The one
// that bites is STATUS: every op recomputes the 0xF cause nibble, but the
// 0xFC0 sticky field is only ever cleared by FSSET, so a stale sticky bit
// rides _vuFMACAdd into fmac[i].statusflag and _vuFMACflush ORs it straight
// into VI[REG_STATUS_FLAG]. The JIT derives its status entirely from the
// restored micro_statusflags[] and so never grows the phantom bit, and the
// replay reports a divergence that belongs to neither engine.
//
// This surfaced as an order-dependent failure of
// ReplayVu0VaddProducesExpectedAndDoesNotDiverge under --gtest_shuffle, where
// a preceding microVU test left VU0.statusflag = 0x82 and the replay of a VADD
// of all-positive operands came out `vi16: JIT=0x0 INTERP=0x80`. Seeding the
// accumulator directly is the same fault with none of the ordering, so it
// belongs in the suite rather than in a shuffle seed.
TEST(VuReplay, ReplayDoesNotInheritStaleInterpreterStickyFlags)
{
	const auto rec = MakeVaddProgram(0);

	// 0x82 = sticky Sign (bit 7) + cause Sign (bit 1), exactly what the
	// triggering predecessor left behind. Only the sticky half survives an op,
	// which is why a cause-only value would not reproduce this.
	vuRegs[0].statusflag = 0x82;
	vuRegs[0].macflag = 0xC0;
	vuRegs[0].clipflag = 0;

	const auto result = ReplayCapture(rec);
	ASSERT_TRUE(result.ok);
	EXPECT_FALSE(result.diverged) << [&] {
		std::string s;
		for (const auto& l : result.diff_lines) { s += "  "; s += l; s += "\n"; }
		return s;
	}();
	// The captured state has STATUS = 0, so a clean replay must end there too:
	// no phantom sticky bit on either side.
	EXPECT_EQ(result.interp_snapshot.regs.VI[REG_STATUS_FLAG].UL & 0xFC0u, 0u)
		<< "[interp] carried a sticky STATUS bit the program never set";
	EXPECT_EQ(result.jit_snapshot.regs.VI[REG_STATUS_FLAG].UL & 0xFC0u, 0u)
		<< "[jit] carried a sticky STATUS bit the program never set";
}

TEST(VuReplay, LoadAndReplayRoundTripsThroughDisk)
{
	const auto rec = MakeVaddProgram(1);
	const std::string path = MakeTempPath("roundtrip");
	ASSERT_TRUE(vu_capture::WriteToFile(path, rec));

	const auto result = LoadAndReplay(path);
	std::remove(path.c_str());

	ASSERT_TRUE(result.ok);
	EXPECT_FALSE(result.diverged);
	const auto& vf3 = result.jit_snapshot.regs.VF[3];
	EXPECT_FLOAT_EQ(AsFloat(vf3.i.x), 11.0f);
}

// XTOP/XITOP read VIF1's TOP/ITOP, which live outside the VU. A replay has
// to restore the values the game had when the capture was taken, not
// whatever the VIF registers hold in the replay process. Without that, a
// double-buffered program reads its packet header from the wrong half of
// VU memory (from address 0 when TOP is 0), and a loop count taken from it
// can wrap at 16 bits.
TEST(VuReplay, ReplayRestoresRecordedVifTopAndItop)
{
	auto rec = MakeVifReadProgram();
	rec.external.flags = vu_capture::kExternalValid;
	rec.external.vif_top = 0x1A0;
	rec.external.vif_itop = 0x55;

	// What an earlier replay or test might have left in the VIF.
	vif1Regs.top = 0x3FF;
	vif1Regs.itop = 0;

	const auto result = ReplayCapture(rec);
	ASSERT_TRUE(result.ok);
	EXPECT_FALSE(result.diverged) << DiffText(result);
	EXPECT_EQ(result.jit_snapshot.regs.VI[1].US[0], 0x1A0u) << "[jit] XTOP";
	EXPECT_EQ(result.jit_snapshot.regs.VI[2].US[0], 0x55u) << "[jit] XITOP";
	EXPECT_EQ(result.interp_snapshot.regs.VI[1].US[0], 0x1A0u) << "[interp] XTOP";
	EXPECT_EQ(result.interp_snapshot.regs.VI[2].US[0], 0x55u) << "[interp] XITOP";
}

// The capture probe's side of the same contract: what the live VIF holds
// when the program is dispatched survives a write to disk and comes back on
// replay.
TEST(VuReplay, LiveVifTopRoundTripsThroughCaptureFile)
{
	auto rec = MakeVifReadProgram();
	vif1Regs.top = 0x123;
	vif1Regs.itop = 0x2A;
	vu_capture::SnapshotExternal(1, rec.external);

	const std::string path = MakeTempPath("viftop");
	ASSERT_TRUE(vu_capture::WriteToFile(path, rec));
	vif1Regs.top = 0;
	vif1Regs.itop = 0;
	const auto result = LoadAndReplay(path);
	std::remove(path.c_str());

	ASSERT_TRUE(result.ok);
	EXPECT_FALSE(result.diverged) << DiffText(result);
	EXPECT_EQ(result.jit_snapshot.regs.VI[1].US[0], 0x123u);
	EXPECT_EQ(result.jit_snapshot.regs.VI[2].US[0], 0x2Au);
	EXPECT_EQ(result.interp_snapshot.regs.VI[1].US[0], 0x123u);
	EXPECT_EQ(result.interp_snapshot.regs.VI[2].US[0], 0x2Au);
}

// A pre-v3 capture has no VIF state. Its replay must not depend on what the
// replay process happens to hold: TOP and ITOP read as 0.
TEST(VuReplay, LegacyCaptureReplaysVifTopAsZero)
{
	const auto rec = MakeVifReadProgram();
	ASSERT_EQ(rec.external.flags & vu_capture::kExternalValid, 0u);
	vif1Regs.top = 0x77;
	vif1Regs.itop = 0x66;

	const auto result = ReplayCapture(rec);
	ASSERT_TRUE(result.ok);
	EXPECT_FALSE(result.diverged) << DiffText(result);
	EXPECT_EQ(result.jit_snapshot.regs.VI[1].US[0], 0u);
	EXPECT_EQ(result.interp_snapshot.regs.VI[1].US[0], 0u);
	EXPECT_TRUE(vu_capture::NeedsUnrecordedVifState(rec));
}

// VU0 sees VU1's VF/VI registers as data memory at 0x4000-0x43FF. A VU0
// capture records them; the replay restores them, so a VU0 program that
// reads VU1's registers is not fed whatever an earlier VU1 replay left.
TEST(VuReplay, Vu0ReplayRestoresRecordedVu1RegisterWindow)
{
	vu_capture::CaptureRecord rec;
	rec.vu_index = 0;
	rec.start_pc = 0;
	rec.cycle_budget = 4096;
	// vi1 = 0x401 (qword address of VU1 VF1), then LQ vf3, 0(vi1).
	rec.microcode = BuildProgramBytes(0, {
		VuOp{VIADDIU_L(vi::vi1, vi::vi0, 0x401), VNOP_U()},
		NopPair(),
		VuOp{VLQ_L(mask::xyzw, vf::vf3, vi::vi1, 0), VNOP_U()},
		VuOp{VXITOP_L(vi::vi2), VNOP_U()},
		EBitNopPair(),
		NopPair(),
	});
	rec.vumem.assign(VU0_MEMSIZE, 0);
	rec.external.flags = vu_capture::kExternalValid;
	rec.external.vif_itop = 0x42;
	const u32 vu1_vf1[4] = {0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	std::memcpy(rec.external.vu1_regs + 16, vu1_vf1, sizeof(vu1_vf1));

	// What a preceding VU1 replay might have left behind.
	for (int i = 0; i < 4; ++i)
		vuRegs[1].VF[1].UL[i] = 0xDEAD0000u + i;
	vif0Regs.itop = 0x11;

	const auto result = ReplayCapture(rec);
	ASSERT_TRUE(result.ok);
	EXPECT_FALSE(result.diverged) << DiffText(result);
	for (int i = 0; i < 4; ++i)
	{
		EXPECT_EQ(result.jit_snapshot.regs.VF[3].UL[i], vu1_vf1[i]) << "[jit] lane " << i;
		EXPECT_EQ(result.interp_snapshot.regs.VF[3].UL[i], vu1_vf1[i]) << "[interp] lane " << i;
	}
	EXPECT_EQ(result.jit_snapshot.regs.VI[2].US[0], 0x42u);
	EXPECT_EQ(result.interp_snapshot.regs.VI[2].US[0], 0x42u);
}

// VPU_STAT bit 12 says VU1's XGKICK is still transferring. A replay takes the bit from the
// capture's own XGKICK state; a bit left set by a previous replay would make VU1's
// VI[REG_VPU_STAT] differ between two replays of the same capture.
TEST(VuReplay, ReplayDoesNotInheritXgkickBusyBitFromPreviousReplay)
{
	const auto rec = MakeVaddProgram(1);
	ASSERT_EQ(rec.state.xgkickenable, 0u);
	vuRegs[0].VI[REG_VPU_STAT].UL |= 1u << 12;

	const auto result = ReplayCapture(rec);
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.jit_snapshot.regs.VI[REG_VPU_STAT].UL & (1u << 12), 0u);
	EXPECT_EQ(result.interp_snapshot.regs.VI[REG_VPU_STAT].UL & (1u << 12), 0u);
	EXPECT_EQ(vuRegs[0].VI[REG_VPU_STAT].UL & (1u << 12), 0u);
}

TEST(VuReplay, ScanFindsXtopAndXitopOnlyOnReachablePaths)
{
	// XTOP behind a conditional branch is reachable; the XITOP after the
	// E-bit's delay slot is not; an I-bit pair whose immediate happens to
	// encode XITOP is data, not an instruction.
	vu_capture::CaptureRecord rec;
	rec.vu_index = 1;
	rec.start_pc = 0;
	rec.microcode = BuildProgramBytes(1, {
		VuOp{VIBNE_L(vi::vi1, vi::vi0, 2), VNOP_U()}, // 0x00 -> 0x18
		NopPair(),                                    // 0x08 delay slot
		IBit(VuOp{VXITOP_L(vi::vi3), VNOP_U()}),      // 0x10 immediate
		VuOp{VXTOP_L(vi::vi2), VNOP_U()},             // 0x18
		EBitNopPair(),                                // 0x20
		NopPair(),                                    // 0x28 delay slot
		VuOp{VXITOP_L(vi::vi4), VNOP_U()},            // 0x30 unreachable
	});
	rec.vumem.assign(VU1_MEMSIZE, 0);
	EXPECT_EQ(vu_capture::ScanExternalReads(rec), vu_capture::kScanReadsXtop);

	// The same program entered past the branch reaches nothing that reads
	// the VIF.
	rec.start_pc = 0x20;
	EXPECT_EQ(vu_capture::ScanExternalReads(rec), 0u);

	// A JR target can't be followed, so the scan says it may be incomplete.
	rec.start_pc = 0;
	rec.microcode = BuildProgramBytes(1, {
		VuOp{VJR_L(vi::vi5), VNOP_U()},
		NopPair(),
	});
	EXPECT_EQ(vu_capture::ScanExternalReads(rec), vu_capture::kScanIndirectJump);
	EXPECT_FALSE(vu_capture::NeedsUnrecordedVifState(rec));

	// An XTOP the walk never reached, in a program with a JR, may be where
	// the JR lands: flagged, so excluding flagged captures is conservative.
	rec.microcode = BuildProgramBytes(1, {
		VuOp{VJR_L(vi::vi5), VNOP_U()},
		NopPair(),
		VuOp{VXTOP_L(vi::vi2), VNOP_U()},
	});
	EXPECT_EQ(vu_capture::ScanExternalReads(rec),
		vu_capture::kScanIndirectJump | vu_capture::kScanUnreachedRead);
	EXPECT_TRUE(vu_capture::NeedsUnrecordedVifState(rec));
}

TEST(VuReplay, RecordedVifStateClearsTheUnrecordedFlag)
{
	auto rec = MakeVifReadProgram();
	EXPECT_EQ(vu_capture::ScanExternalReads(rec),
		vu_capture::kScanReadsXtop | vu_capture::kScanReadsXitop);
	EXPECT_TRUE(vu_capture::NeedsUnrecordedVifState(rec));
	rec.external.flags = vu_capture::kExternalValid;
	EXPECT_FALSE(vu_capture::NeedsUnrecordedVifState(rec));
}

// The bench drivers run the JIT only, and microVU writes the host FPCR at
// dispatch only when the EE and VU FPCRs differ, so a bench inherits its
// caller's FPCR. They must set the VU FPCR themselves, as ReplayCapture
// does, or they time arithmetic rounded differently from the game's.
// (1 + 2^-23) * (1.5 + 2^-23) = 1.5 + 2.5 ulp + 2^-46: chop gives 1.5 + 2 ulp,
// round-to-nearest gives 1.5 + 3 ulp.
TEST(VuReplay, BenchDriversRunUnderTheVuFpcr)
{
	vu_capture::CaptureRecord rec;
	rec.vu_index = 1;
	rec.start_pc = 0;
	rec.cycle_budget = 4096;
	rec.microcode = BuildProgramBytes(1, {
		IBit(VuOp{VLitZero(), VMUL_U(mask::x, vf::vf3, vf::vf1, vf::vf2)}),
		EBitNopPair(),
		NopPair(),
	});
	rec.vumem.assign(VU1_MEMSIZE, 0);
	rec.state.VF[1][0] = 0x3F800001u;
	rec.state.VF[2][0] = 0x3FC00001u;
	constexpr u32 kChop = 0x3FC00002u;
	constexpr u32 kNearest = 0x3FC00003u;

	ASSERT_EQ(EmuConfig.Cpu.VU1FPCR.GetRoundMode(), FPRoundMode::ChopZero);
	const auto replay = ReplayCapture(rec);
	ASSERT_TRUE(replay.ok);
	ASSERT_EQ(replay.jit_snapshot.regs.VF[3].UL[0], kChop);

	const FPControlRegister saved = FPControlRegister::GetCurrent();
	FPControlRegister nearest = saved;
	nearest.SetRoundMode(FPRoundMode::Nearest);

	FPControlRegister::SetCurrent(nearest);
	const auto steady = BenchJitSteady(rec, 2);
	const FPControlRegister after_steady = FPControlRegister::GetCurrent();
	const u32 steady_result = vuRegs[1].VF[3].UL[0];

	FPControlRegister::SetCurrent(nearest);
	const auto reprime = BenchJit(rec, 2);
	const FPControlRegister after_reprime = FPControlRegister::GetCurrent();
	const u32 reprime_result = vuRegs[1].VF[3].UL[0];
	FPControlRegister::SetCurrent(saved);

	if (steady.empty() || reprime.empty())
		GTEST_SKIP() << "PMU counters unavailable; the bench drivers did not run";
	EXPECT_EQ(steady_result, kChop) << "BenchJitSteady ran under the caller's FPCR"
									<< (steady_result == kNearest ? " (round-to-nearest)" : "");
	EXPECT_EQ(reprime_result, kChop) << "BenchJit ran under the caller's FPCR"
									 << (reprime_result == kNearest ? " (round-to-nearest)" : "");
	EXPECT_EQ(after_steady.bitmask, nearest.bitmask) << "BenchJitSteady did not restore the FPCR";
	EXPECT_EQ(after_reprime.bitmask, nearest.bitmask) << "BenchJit did not restore the FPCR";
}

TEST(VuReplay, LoadAndReplayReturnsNotOkOnMissingFile)
{
	const auto result = LoadAndReplay(MakeTempPath("missing-never-created"));
	EXPECT_FALSE(result.ok);
	EXPECT_FALSE(result.diverged);
}

} // namespace recompiler_tests
