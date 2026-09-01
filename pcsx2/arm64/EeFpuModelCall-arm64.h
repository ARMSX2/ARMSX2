// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "arm64/AsmHelpers.h"

#include "EeFpuModel.h"

namespace a64 = vixl::aarch64;

/*	Reaching the EE FPU model from a recompiler.

	EEFPU_MODEL_CALL (EeFpuModel.h) spares q8-q31 and x9-x30, which is where
	both VU engines keep their state -- microVU's VFs, Q/P pipeline and VI
	cache, the EE's pinned GPRs and COP2 VF cache -- so no allocator is flushed
	and no pin mirror is written back. The stub saves the caller's own half of
	the convention, q0-q7 and x2-x8, plus the x30 that the bl to it wrote.

	That half is the same wherever the model is reached from, so one stub per
	target serves every site and the site emits a bl and nothing else. Deciding
	the set by walking an allocator instead would have to happen at the site,
	and cannot: each op answers the zero divisor first and only the other arm
	reaches the model, and a flush marks registers clean at compile time for a
	path that runs conditionally.

	The GPRs are stored below the vectors so that both halves stay inside their
	addressing modes: STP's offset is a signed 7-bit multiple of the operand
	size, and the fallback frame is wide enough that vectors first would push
	the GPR pairs past it.  */
namespace EeFpuModelFrame
{
	// Without the attribute the convention is plain AAPCS and everything
	// caller-saved has to go. x17 and x18 are excluded either way: one is
	// vixl's own scratch, dead across a call by its rules, and the other is
	// the platform's.
	constexpr int kNeonEnd = EEFPU_MODEL_CALL_SPARES_MOST ? 8 : 32;
	constexpr int kGprEnd = EEFPU_MODEL_CALL_SPARES_MOST ? 8 : 16;

	constexpr int kNumGpr = (kGprEnd - 2 + 1) + 1; // x2..kGprEnd, then x30
	constexpr int kGprBytes = kNumGpr * 8;
	constexpr int kFrame = kGprBytes + kNeonEnd * 16;
	static_assert(kNumGpr % 2 == 0, "the gpr spill pairs up");
	static_assert(kFrame % 16 == 0, "sp stays 16-byte aligned");

	// The pair after the last saved GPR is x30, which has no run to sit in.
	__fi static a64::XRegister Gpr(int i) { return a64::XRegister(i <= kGprEnd ? i : 30); }
} // namespace EeFpuModelFrame

// Reached by a bl, so the x30 in the frame is the site's return address.
__fi static const u8* armDynGenEeFpuModelStub(const void* fn)
{
	using namespace EeFpuModelFrame;
	const u8* start = armGetCurrentCodePointer();

	armAsm->Sub(a64::sp, a64::sp, kFrame);
	for (int i = 2, off = 0; i < 2 + kNumGpr; i += 2, off += 16)
		armAsm->Stp(Gpr(i), Gpr(i + 1), a64::MemOperand(a64::sp, off));
	for (int i = 0, off = kGprBytes; i < kNeonEnd; i += 2, off += 32)
		armAsm->Stp(a64::QRegister(i), a64::QRegister(i + 1), a64::MemOperand(a64::sp, off));

	armEmitCall(fn);

	for (int i = 2, off = 0; i < 2 + kNumGpr; i += 2, off += 16)
		armAsm->Ldp(Gpr(i), Gpr(i + 1), a64::MemOperand(a64::sp, off));
	for (int i = 0, off = kGprBytes; i < kNeonEnd; i += 2, off += 32)
		armAsm->Ldp(a64::QRegister(i), a64::QRegister(i + 1), a64::MemOperand(a64::sp, off));
	armAsm->Add(a64::sp, a64::sp, kFrame);

	armAsm->Ret();
	return start;
}
