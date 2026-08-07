// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ARM64 EE Jump Instruction Codegen

#include "arm64/iR5900-arm64.h"
#include "Config.h"
#include "vtlb.h"
#include "common/Console.h"

namespace a64 = vixl::aarch64;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {

namespace Interp = R5900::Interpreter::OpcodeImpl;


/*********************************************************
 * Jump to target                                        *
 * Format: OP target                                     *
 *********************************************************/

//// J
void recJ()
{
	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	recompileNextInstruction(true, false);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

//// JAL — jump and link (r31 = return address)
void recJAL()
{
	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	const u32 retpc = pc + 4; // == the $ra link value; capture before the delay slot advances pc
	_deleteEEreg(31, 0);
	if (EE_CONST_PROP)
	{
		GPR_SET_CONST(31);
		g_cpuConstRegs[31].UL[0] = retpc;
		g_cpuConstRegs[31].UL[1] = 0;
	}
	else
	{
		armAsm->Mov(RXSCRATCH, (u64)retpc);
		_eeStoreGPRDestReg(31, RXSCRATCH);
	}

	recompileNextInstruction(true, false);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImmCall(newpc, retpc);
}

/*********************************************************
 * Register jump                                         *
 * Format: OP rs, rd                                     *
 *********************************************************/

//// JR — jump to address in rs
void recJR()
{
	const u32 rs = _Rs_;

	// Capture the jump target before the delay slot, which MIPS lets write rs,
	// into an ARM64TYPE_PCWRITEBACK slot -- see SetBranchReg for how it's read
	// back.
	const int wbreg = _allocArm64GPR(ARM64TYPE_PCWRITEBACK, 0, MODE_WRITE);
	_eeMoveGPRtoR(armWRegister(wbreg), rs);

	recompileNextInstruction(true, false);

	// JR $ra is the ABI return idiom — pop the call-ret ring and RET so the
	// hardware RAS (pushed by the paired call-site BL) predicts the target.
	// Emit-gated off under GoemonTlbHack: SetBranchReg compares V2P-translated
	// targets there, which can never match the virtual frame RAs.
	if (rs == 31 && !EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchReg(EEBranchRegMode::Return, 0, wbreg);
	else
		SetBranchReg(EEBranchRegMode::Jump, 0, wbreg);
}

//// JALR — jump to rs, link in rd
void recJALR()
{
	const u32 rs = _Rs_;
	const u32 rd = _Rd_;
	const u32 newpc = pc + 4;

	// Capture the jump target before the delay slot; see recJR. Ordered ahead
	// of the rd write below because rd may be rs, and the capture has to see
	// the pre-link value.
	const int wbreg = _allocArm64GPR(ARM64TYPE_PCWRITEBACK, 0, MODE_WRITE);
	_eeMoveGPRtoR(armWRegister(wbreg), rs);

	// Write link address to rd
	if (rd)
	{
		_deleteEEreg(rd, 0);
		if (EE_CONST_PROP)
		{
			GPR_SET_CONST(rd);
			g_cpuConstRegs[rd].UL[0] = newpc;
			g_cpuConstRegs[rd].UL[1] = 0;
		}
		else
		{
			armAsm->Mov(RXSCRATCH, (u64)newpc);
			_eeStoreGPRDestReg(rd, RXSCRATCH);
		}
	}

	recompileNextInstruction(true, false);

	// JALR linking into $ra is the ABI indirect-call idiom — push a call-ret
	// frame and transfer via BL so the callee's return RETs to our landing.
	// Other link registers don't pair with the JR-$ra pop, so they take the
	// plain jump (their returns just compare-miss if the callee uses jr $ra).
	if (rd == 31 && !EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchReg(EEBranchRegMode::Call, newpc, wbreg);
	else
		SetBranchReg(EEBranchRegMode::Jump, 0, wbreg);
}


} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
