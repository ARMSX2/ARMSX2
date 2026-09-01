// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "arm64/AsmHelpers.h"

#include "EeFpuModel.h"

#include "common/Assertions.h"

namespace a64 = vixl::aarch64;

/*	Reaching the EE FPU model from a recompiler.

	EEFPU_MODEL_CALL (EeFpuModel.h) spares q8-q31 and x9-x30, which is where
	both VU engines keep their state -- microVU's VFs, Q/P pipeline and VI
	cache, the EE's pinned GPRs and COP2 VF cache -- so no allocator is flushed
	and no pin mirror is written back. The stub saves the caller's own half of
	the convention: x2-x8, the x30 that the bl to it wrote, and the low vector
	registers.

	That half is the same wherever the model is reached from, so one stub per
	target serves every site and the site emits a bl and nothing else. Deciding
	the set by walking an allocator instead would have to happen at the site,
	and cannot: each op answers the zero divisor first and only the other arm
	reaches the model, and a flush marks registers clean at compile time for a
	path that runs conditionally.

	The vector half is per target. kVec* is one past the last q register the
	target's closure reaches, so a stub carries only what its own callee can
	dirty; the GPR half is not worth splitting, since 17 of the 18 targets
	reach x8. Nothing on the callee's side holds it to the declaration, so
	model_call_contract_tests walks each shipped closure and fails on a q
	register at or above the extent it was generated with.

	The GPRs are stored below the vectors so that both halves stay inside their
	addressing modes: STP's offset is a signed 7-bit multiple of the operand
	size, and the fallback frame is wide enough that vectors first would push
	the GPR pairs past it.  */
namespace EeFpuModelFrame
{
	// x17 and x18 are excluded from the GPR half either way: one is vixl's own
	// scratch, dead across a call by its rules, and the other is the
	// platform's.
	constexpr int kGprEnd = EEFPU_MODEL_CALL_SPARES_MOST ? 8 : 16;

	constexpr int kNumGpr = (kGprEnd - 2 + 1) + 1; // x2..kGprEnd, then x30
	constexpr int kGprBytes = kNumGpr * 8;
	static_assert(kNumGpr % 2 == 0, "the gpr spill pairs up");
	static_assert(kGprBytes % 16 == 0, "sp stays 16-byte aligned");

	// Divide, the mul-band entries and the EFU's reciprocal are integer
	// throughout. The square roots take a host sqrt in q0. The nine EFU
	// polynomials evaluate in q0-q4.
	constexpr int kVecNone = 0;
	constexpr int kVecSqrt = 2;
	constexpr int kVecPoly = 6;

	// The pair after the last saved GPR is x30, which has no run to sit in.
	__fi static a64::XRegister Gpr(int i) { return a64::XRegister(i <= kGprEnd ? i : 30); }
} // namespace EeFpuModelFrame

struct EeFpuModelCallee
{
	const void* fn;
	int vecEnd; // one of EeFpuModelFrame::kVec*
};

// Reached by a bl, so the x30 in the frame is the site's return address.
__fi static const u8* armDynGenEeFpuModelStub(EeFpuModelCallee callee)
{
	using namespace EeFpuModelFrame;
	// Without the attribute the convention is plain AAPCS and every
	// caller-saved vector has to go, whatever this target reaches.
	const int neonEnd = EEFPU_MODEL_CALL_SPARES_MOST ? callee.vecEnd : 32;
	const int frame = kGprBytes + neonEnd * 16;
	pxAssert(neonEnd >= 0 && neonEnd % 2 == 0);

	const u8* start = armGetCurrentCodePointer();

	armAsm->Sub(a64::sp, a64::sp, frame);
	for (int i = 2, off = 0; i < 2 + kNumGpr; i += 2, off += 16)
		armAsm->Stp(Gpr(i), Gpr(i + 1), a64::MemOperand(a64::sp, off));
	for (int i = 0, off = kGprBytes; i < neonEnd; i += 2, off += 32)
		armAsm->Stp(a64::QRegister(i), a64::QRegister(i + 1), a64::MemOperand(a64::sp, off));

	armEmitCall(callee.fn);

	for (int i = 2, off = 0; i < 2 + kNumGpr; i += 2, off += 16)
		armAsm->Ldp(Gpr(i), Gpr(i + 1), a64::MemOperand(a64::sp, off));
	for (int i = 0, off = kGprBytes; i < neonEnd; i += 2, off += 32)
		armAsm->Ldp(a64::QRegister(i), a64::QRegister(i + 1), a64::MemOperand(a64::sp, off));
	armAsm->Add(a64::sp, a64::sp, frame);

	armAsm->Ret();
	return start;
}
