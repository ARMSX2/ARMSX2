// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The COP2 macro multiply's MAC U bit, differentially against the interpreter.
//
// vu0_macro_fmac_range_console_tests.cpp holds the console rows the model was
// built from; those are six operand pairs on one opcode. This is the other
// half of the coverage: every emitter in the tree that computes a bare product
// -- VMUL and its broadcast/Q/I forms, the accumulator variants, VOPMULA --
// against random operands, every dest mask and every register aliasing, with
// the interpreter's exact model (EeFpuModel::Mul, VUops.cpp) as the reference.
//
// The operand pool is bounded above so nothing here overflows: the recompiler's
// value model still stops a binade short of the EE's maximum and its multiplier
// does not truncate the way silicon's array does, and either would show up as a
// flag divergence for reasons that have nothing to do with U. Exponents are
// drawn from the bottom half of the range instead, where a product underflows
// often and saturation is unreachable. Denormal operands and signed zeros are
// in the pool because they are the cases a result-only test cannot tell from an
// underflow.
//
// Both polarities: the same stream below vuClampMode 4 has to lose U on the
// rows mode 4 gets right, or the mode-4 pass says nothing about the gate.

#include "harness/EeRecTestHarness.h"
#include "harness/MipsEncode.h"

#include "VU.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace recompiler_tests
{
using namespace mips;
using namespace mips::ee;

namespace {

constexpr u32 kFs = 1, kFt = 2, kFd = 4;
constexpr u32 kRMac = 8, kRStatus = 9;

// One product-computing emitter, and how to encode it.
struct MulForm
{
	const char* name;
	u32 (*encode)(u32 mask, u32 fd, u32 fs, u32 ft);
};

u32 EncMul  (u32 m, u32 fd, u32 fs, u32 ft) { return VMUL_C2  (m, fd, fs, ft); }
u32 EncMulX (u32 m, u32 fd, u32 fs, u32 ft) { return VMULx_C2 (m, fd, fs, ft); }
u32 EncMulY (u32 m, u32 fd, u32 fs, u32 ft) { return VMULy_C2 (m, fd, fs, ft); }
u32 EncMulZ (u32 m, u32 fd, u32 fs, u32 ft) { return VMULz_C2 (m, fd, fs, ft); }
u32 EncMulW (u32 m, u32 fd, u32 fs, u32 ft) { return VMULw_C2 (m, fd, fs, ft); }
u32 EncMulQ (u32 m, u32 fd, u32 fs, u32   ) { return VMULq_C2 (m, fd, fs); }
u32 EncMulI (u32 m, u32 fd, u32 fs, u32   ) { return VMULi_C2 (m, fd, fs); }
u32 EncMulA (u32 m, u32   , u32 fs, u32 ft) { return VMULA_C2 (m, fs, ft); }
u32 EncMulAX(u32 m, u32   , u32 fs, u32 ft) { return VMULAx_C2(m, fs, ft); }
u32 EncMulAY(u32 m, u32   , u32 fs, u32 ft) { return VMULAy_C2(m, fs, ft); }
u32 EncMulAZ(u32 m, u32   , u32 fs, u32 ft) { return VMULAz_C2(m, fs, ft); }
u32 EncMulAW(u32 m, u32   , u32 fs, u32 ft) { return VMULAw_C2(m, fs, ft); }
u32 EncMulAQ(u32 m, u32   , u32 fs, u32   ) { return VMULAq_C2(m, fs); }
u32 EncMulAI(u32 m, u32   , u32 fs, u32   ) { return VMULAi_C2(m, fs); }
u32 EncOpMulA(u32 m, u32  , u32 fs, u32 ft) { return VOPMULA_C2(m, fs, ft); }

constexpr MulForm kForms[] = {
	{"VMUL", EncMul},     {"VMULx", EncMulX},   {"VMULy", EncMulY},
	{"VMULz", EncMulZ},   {"VMULw", EncMulW},   {"VMULq", EncMulQ},
	{"VMULi", EncMulI},   {"VMULA", EncMulA},   {"VMULAx", EncMulAX},
	{"VMULAy", EncMulAY}, {"VMULAz", EncMulAZ}, {"VMULAw", EncMulAW},
	{"VMULAq", EncMulAQ}, {"VMULAi", EncMulAI}, {"VOPMULA", EncOpMulA},
};

// A word whose magnitude is at most 2^-1, so no pair of them can leave the
// EE's range on the way up. Signed zeros, denormals, the first normal and the
// bottom of the exponent window are drawn far more often than chance would.
u32 RandomOperand(std::mt19937& rng)
{
	std::uniform_int_distribution<u32> pick(0, 15);
	const u32 sign = (rng() & 1u) << 31;
	switch (pick(rng))
	{
		case 0: return sign;                       // +/-0
		case 1: return sign | 0x00000001u;         // smallest denormal
		case 2: return sign | 0x00400000u;         // mid denormal
		case 3: return sign | 0x007FFFFFu;         // largest denormal
		case 4: return sign | 0x00800000u;         // 2^-126
		case 5: return sign | 0x00800001u;         // 2^-126 + 1ulp
		case 6: return sign | 0x3F000000u;         // 0.5
		case 7: return sign | 0x3F7FFFFFu;         // just under 1.0
		default:
		{
			// exponent 1..126 (magnitude <= 2^-1), full random mantissa
			std::uniform_int_distribution<u32> e(1, 126), m(0, 0x7FFFFFu);
			return sign | (e(rng) << 23) | m(rng);
		}
	}
}

struct Obs
{
	u32 mac, status;
	u32 vf[4], acc[4]; // whichever the op wrote; the other is its seed
};

Obs RunOne(const MulForm& f, u32 mask, u32 fd, u32 fs, u32 ft, const u32 (&vfs)[3][4],
	u32 q, u32 i, bool jit, int mode, bool diff = true)
{
	EeRecTestHarness h;
	h.SetVu0ClampMode(mode);
	h.EnableVu0Capture();
	// Each engine is read on its own: the recompiler's multiply does not
	// truncate the way silicon's array does, so the VU auto-diff would fail on
	// mantissas that have nothing to do with U. `diff` off drops the EE-level
	// one too, for the runs below the gate where the CFC2'd MAC is meant to
	// differ.
	h.ExpectVu0Divergence();
	h.SeedVu0VfBits(kFs, vfs[0][0], vfs[0][1], vfs[0][2], vfs[0][3]);
	h.SeedVu0VfBits(kFt, vfs[1][0], vfs[1][1], vfs[1][2], vfs[1][3]);
	h.SeedVu0AccBits(vfs[2][0], vfs[2][1], vfs[2][2], vfs[2][3]);
	h.SeedVu0VfBits(kFd, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu, 0x5EED5EEDu);
	h.SeedVu0Vi(REG_Q, q);
	h.SeedVu0Vi(REG_I, i);
	// ACC is seeded directly rather than through a VADDA so that MAC starts at
	// zero: VOPMULA writes XYZ and the two engines disagree about whether that
	// clears the MAC W bit or leaves the previous op's standing (the DISABLED
	// tripwire in ee_vu0_cop2_macro_tests.cpp). Nothing here is about that.
	h.LoadProgram({
		CTC2(0, REG_STATUS_FLAG),
		f.encode(mask, fd, fs, ft),
		CFC2(kRMac, REG_MAC_FLAG),
		CFC2(kRStatus, REG_STATUS_FLAG),
	});
	if (diff)
		h.Run();
	else if (jit)
		h.RunJitNoDiff();
	else
		h.RunInterpOnly();

	Obs o{};
	o.mac = jit ? h.GetGprJit(kRMac) : h.GetGprInterp(kRMac);
	o.status = jit ? h.GetGprJit(kRStatus) : h.GetGprInterp(kRStatus);
	static const char kLane[4] = {'x', 'y', 'z', 'w'};
	for (int l = 0; l < 4; ++l)
	{
		o.vf[l] = jit ? h.GetVu0VfBitsJit(fd, kLane[l]) : h.GetVu0VfBitsInterp(fd, kLane[l]);
		o.acc[l] = jit ? h.GetVu0AccBitsJit(kLane[l]) : h.GetVu0AccBitsInterp(kLane[l]);
	}
	return o;
}

constexpr u32 kMacU = 0x0F00u;   // MAC underflow nibble
constexpr u32 kStatU = 0x0104u;  // STATUS current U (bit 2) and sticky US (bit 8)

} // namespace

// At vuClampMode 4 the recompiler's flag words must equal the interpreter's on
// every one of these, and enough of them must actually raise U for that to
// mean something.
TEST(EeVu0Cop2MulUnderflow, FlagsMatchTheInterpreterAtModeFour)
{
	std::mt19937 rng(0xC0DE2141u);
	int raised = 0, cases = 0;
	for (int iter = 0; iter < 900; ++iter)
	{
		const MulForm& f = kForms[iter % std::size(kForms)];
		u32 vfs[3][4];
		for (auto& reg : vfs)
			for (u32& lane : reg)
				lane = RandomOperand(rng);
		const u32 q = RandomOperand(rng), i = RandomOperand(rng);
		const u32 mask = rng() & 0xFu;
		// Aliasing: fd == fs, fd == ft and fs == ft all reachable.
		const u32 fs = (rng() & 1u) ? kFs : kFt;
		const u32 ft = (rng() & 1u) ? kFt : kFs;
		const u32 fd = (rng() % 3u == 0) ? fs : ((rng() % 2u) ? ft : kFd);

		const Obs gi = RunOne(f, mask, fd, fs, ft, vfs, q, i, false, 4);
		const Obs gj = RunOne(f, mask, fd, fs, ft, vfs, q, i, true, 4);
		SCOPED_TRACE(::testing::Message()
			<< f.name << " mask " << mask << " fd " << fd << " fs " << fs << " ft " << ft
			<< " iter " << iter);
		EXPECT_EQ(gj.mac, gi.mac) << "MAC";
		EXPECT_EQ(gj.status, gi.status) << "STATUS";
		raised += (gi.mac & kMacU) != 0;
		++cases;
	}
	EXPECT_EQ(cases, 900);
	EXPECT_GT(raised, 200) << "the stream stopped underflowing, so it stopped testing U";
}

// The gate, from the other side. Replaying the same stream below vuClampMode 4
// has to lose the U bits and nothing else: MAC and STATUS agree with the
// interpreter except in the U positions, and they disagree there on every row
// the model moved. All three low modes, because the ladder's other rungs run
// through the same FMAC bodies.
TEST(EeVu0Cop2MulUnderflow, ModesBelowFourLoseTheUBitsAndOnlyThose)
{
	std::mt19937 rng(0xC0DE2141u);
	int lost = 0, nonzero = 0;
	for (int iter = 0; iter < 900; ++iter)
	{
		const MulForm& f = kForms[iter % std::size(kForms)];
		u32 vfs[3][4];
		for (auto& reg : vfs)
			for (u32& lane : reg)
				lane = RandomOperand(rng);
		const u32 q = RandomOperand(rng), i = RandomOperand(rng);
		const u32 mask = rng() & 0xFu;
		const u32 fs = (rng() & 1u) ? kFs : kFt;
		const u32 ft = (rng() & 1u) ? kFt : kFs;
		const u32 fd = (rng() % 3u == 0) ? fs : ((rng() % 2u) ? ft : kFd);

		const Obs gi = RunOne(f, mask, fd, fs, ft, vfs, q, i, false, 4);
		const Obs g4 = RunOne(f, mask, fd, fs, ft, vfs, q, i, true, 4);
		for (int mode = 1; mode <= 3; ++mode)
		{
			const Obs g = RunOne(f, mask, fd, fs, ft, vfs, q, i, true, mode, /*diff=*/false);
			SCOPED_TRACE(::testing::Message()
				<< f.name << " mask " << mask << " fd " << fd << " fs " << fs << " ft " << ft
				<< " iter " << iter << " vuClampMode " << mode);
			EXPECT_EQ(g.mac & ~kMacU, gi.mac & ~kMacU) << "MAC outside the U nibble";
			EXPECT_EQ(g.status & ~kStatU, gi.status & ~kStatU) << "STATUS outside U/US";
			EXPECT_EQ(g.mac & kMacU, 0u) << "raised MAC U";
			// The model is a flag, so it must not have moved a word. This is
			// where the predicate's scratch registers are checked: q27 and
			// RQSCRATCH3 are live across the multiply, and a full-mask op
			// computes into a VF cache slot that may be Fs's or Ft's.
			for (int l = 0; l < 4; ++l)
			{
				EXPECT_EQ(g4.vf[l], g.vf[l]) << "VF[fd] lane " << l << " moved with the mode";
				EXPECT_EQ(g4.acc[l], g.acc[l]) << "ACC lane " << l << " moved with the mode";
			}
		}
		for (int l = 0; l < 4; ++l)
			nonzero += (g4.vf[l] | g4.acc[l]) != 0;
		lost += (gi.mac & kMacU) != 0;
	}
	EXPECT_GT(lost, 200) << "nothing was lost, so the gate was not exercised";
	EXPECT_GT(nonzero, 900) << "the below-gate run read back nothing, so the value "
	                           "comparison above compared zero with zero";
}
} // namespace recompiler_tests
