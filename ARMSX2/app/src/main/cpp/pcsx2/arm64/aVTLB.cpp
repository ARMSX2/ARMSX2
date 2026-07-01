// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0
//
// ARM64 VTLB fastmem backpatch handler.
// When a fastmem LDR/STR faults (unmapped page, MMIO, etc.), the signal
// handler calls vtlb_BackpatchLoadStore → vtlb_DynBackpatchLoadStore.
// We generate a "thunk" that performs the slow vtlb_memRead/Write call
// and jumps back to the instruction after the original fastmem access.
// The faulting instruction is then overwritten with a branch to the thunk.

#include "Common.h"
#include "vtlb.h"
#include "arm64/arm64Emitter.h"
#include "arm64/AsmHelpers.h"

#include "common/Console.h"

using namespace R5900;

// @@MAC_FASTMEM_BACKPATCH@@ Disabled on this Android fork: only the mac EE recompiler runs
// (pcsx2_macrec::recCpu), so a fastmem fault must be serviced by the MAC thunk in
// arm64/mac/RecStubs.cpp — which carves from the mac code buffer and uses the mac register
// model. This stock backpatch would carve from the WRONG (stock) buffer and clobber a live
// mac GPR-cache reg (x20) via its RCYCLE flush/reload. The global vtlb_DynBackpatchLoadStore
// symbol is provided by RecStubs.cpp (forwarding to the mac thunk) instead.
#if 0
void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc,
	u32 guest_addr, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register,
	u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr)
{
	u8* thunk = recBeginThunk();

	if (is_load)
	{
		// Ensure address is in w0 (RWARG1) for vtlb_memRead<T>
		if (address_register != RWARG1.GetCode())
			armAsm->Mov(RWARG1, a64::WRegister(address_register));

		// Flush RCYCLE → cpuRegs.cycle so the slow path sees the correct
		// "now" (Phase B keeps the JIT-current cycle pinned in RCYCLE).
		// Done AFTER the address move so x9 can't clobber address_register.
		armEmitFlushCycleBeforeCall();

		switch (size_in_bits)
		{
			case 8:   armEmitCall((const void*)&vtlb_memRead<mem8_t>);  break;
			case 16:  armEmitCall((const void*)&vtlb_memRead<mem16_t>); break;
			case 32:  armEmitCall((const void*)&vtlb_memRead<mem32_t>); break;
			case 64:  armEmitCall((const void*)&vtlb_memRead<mem64_t>); break;
			case 128: armEmitCall((const void*)&vtlb_memRead128);       break;
			default: pxFailRel("Unexpected fastmem load size"); break;
		}

		if (size_in_bits == 128)
		{
			// vtlb_memRead128 returns r128 in q0 (AAPCS64 vector return).
			// data_register is a Q-reg code (0..31) — Mov if not already q0.
			if (data_register != 0)
				armAsm->Mov(a64::VRegister(data_register, a64::kQRegSize).V16B(), a64::q0.V16B());
		}
		else
		{
			// vtlb_memRead returns unsigned in w0/x0. Sign-extend if needed.
			if (is_signed)
			{
				switch (size_in_bits)
				{
					case 8:  armAsm->Sxtb(RXRET, RWRET); break;
					case 16: armAsm->Sxth(RXRET, RWRET); break;
					case 32: armAsm->Sxtw(RXRET, RWRET); break;
					default: break; // 64-bit: no sign extension needed
				}
			}

			// Move result to the data register if it's not already x0
			if (data_register != RXRET.GetCode())
				armAsm->Mov(a64::XRegister(data_register), RXRET);
		}
	}
	else
	{
		if (size_in_bits == 128)
		{
			// 128-bit store: vtlb_memWrite128(u32 addr in w0, r128 value in q0).
			// data_register is a Q-reg code; address is in a GPR. Different
			// register banks so no swap-conflict possible.
			if (data_register != 0)
				armAsm->Mov(a64::q0.V16B(), a64::VRegister(data_register, a64::kQRegSize).V16B());
			if (address_register != RWARG1.GetCode())
				armAsm->Mov(RWARG1, a64::WRegister(address_register));

			armEmitFlushCycleBeforeCall();
			armEmitCall((const void*)&vtlb_memWrite128);
		}
		else
		{
			// Store: set up (w0 = address, w1/x1 = value) for vtlb_memWrite<T>
			if (address_register == RXARG2.GetCode() && data_register == RXARG1.GetCode())
			{
				// Registers are swapped — use scratch to resolve
				armAsm->Mov(RXVIXLSCRATCH, a64::XRegister(address_register));
				armAsm->Mov(RXARG2, a64::XRegister(data_register));
				armAsm->Mov(RWARG1, a64::WRegister(RXVIXLSCRATCH.GetCode()));
			}
			else
			{
				if (data_register != RXARG2.GetCode())
					armAsm->Mov(RXARG2, a64::XRegister(data_register));
				if (address_register != RWARG1.GetCode())
					armAsm->Mov(RWARG1, a64::WRegister(address_register));
			}

			// Flush RCYCLE → cpuRegs.cycle so the slow path sees the correct
			// "now". Done AFTER the arg moves so x9 can't clobber x0/x1's source.
			armEmitFlushCycleBeforeCall();

			switch (size_in_bits)
			{
				case 8:  armEmitCall((const void*)&vtlb_memWrite<mem8_t>);  break;
				case 16: armEmitCall((const void*)&vtlb_memWrite<mem16_t>); break;
				case 32: armEmitCall((const void*)&vtlb_memWrite<mem32_t>); break;
				case 64: armEmitCall((const void*)&vtlb_memWrite<mem64_t>); break;
				default: pxFailRel("Unexpected fastmem store size"); break;
			}
		}
	}

	// vtlb_mem* may have ended up in an MMIO handler that called CPU_INT,
	// mutating cpuRegs.nextEventCycle. Phase B's pinned RCYCLE (x20) was
	// anchored to the OLD nec — re-derive it from memory before resuming
	// the JIT-compiled block. Uses x9/x10 as scratch so the load result
	// in x0 (already moved to data_register above) stays untouched.
	armEmitReloadCycleAfterCall();

	// Jump back to the instruction after the original fastmem access
	armEmitJmp(reinterpret_cast<const void*>(code_address + code_size));

	recEndThunk();

	// Replace the faulting fastmem instruction with a branch to the thunk
	armEmitJmpPtr(reinterpret_cast<void*>(code_address), thunk, true);
}
#endif // @@MAC_FASTMEM_BACKPATCH@@ stock backpatch disabled — see arm64/mac/RecStubs.cpp
