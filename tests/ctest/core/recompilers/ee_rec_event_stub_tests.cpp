// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The shared event-test stub (DispatcherEvent) with lazy-dirty pins.
//
// Every block tail whose cycle delta has gone non-negative enters the stub,
// which calls recEventTest. Before the call it must store all nine pins:
// the event test can reach the host message pump at vsync, where a savestate
// save reads GPR memory, and it can leave the JIT through recExitExecution,
// after which only memory survives. After a normal return it reloads the
// cycle delta and the pins in caller-saved host registers (x11-x13), which
// the call destroyed. The callee-saved pins are still in their registers, and
// nothing recEventTest returns normally from writes guest GPR memory (a
// savestate load or a recompiler reset requested from the pump also requests
// an exit, so it leaves through recExitExecution), so they need no reload.
//
// The harness runs with no VM, where the shared event test returns early, so
// the tests put their work inside recEventTest through a test hook: one reads
// GPR memory there, as a savestate save from the message pump does, and one
// raises an interrupt there, as the shared event test does, so the guest
// handler, running as JIT code, reads every pin. All nine pins are dirty when
// the stub runs. The shape tests count the stub's pin stores and reloads and
// check that a recompiler reset regenerates the stub byte for byte, since
// recEventTest can reset the recompiler in place and return into the new copy.

#include "harness/EeRecTestHarness.h"

#include "arm64/iR5900-arm64.h"

#include "R5900.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

extern void recEeDispatcherAddrs(const void** dispatcher_event, const void** dispatcher_reg);
extern void (*g_eeHarnessEventTestHook)();

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kIntVector = 0x80000200;
constexpr u32 kCp0Status = 12;
constexpr u32 kCp0Cause = 13;
constexpr u32 kCp0Epc = 14;

// Installs a guest interrupt handler at the BEV=0 interrupt vector, and puts
// back the environment's `jr ra; nop` stub afterwards.
class InterruptHandler
{
public:
	InterruptHandler(EeRecTestHarness& h, const std::vector<u32>& words)
		: h_(h)
		, size_(words.size())
	{
		for (size_t i = 0; i < words.size(); i++)
			h_.WriteU32(kIntVector + static_cast<u32>(i * 4), words[i]);
	}
	~InterruptHandler()
	{
		h_.WriteU32(kIntVector + 0, JR(reg::ra));
		h_.WriteU32(kIntVector + 4, NOP);
		for (size_t i = 2; i < size_; i++)
			h_.WriteU32(kIntVector + static_cast<u32>(i * 4), NOP);
	}

private:
	EeRecTestHarness& h_;
	size_t size_;
};

// Sets the event-test hook for the lifetime of a test.
class EventTestHook
{
public:
	explicit EventTestHook(void (*fn)()) { g_eeHarnessEventTestHook = fn; }
	~EventTestHook() { g_eeHarnessEventTestHook = nullptr; }
};

constexpr u64 kSeed = 0x2222333344445550ull;

void SeedSources(EeRecTestHarness& h)
{
	// Run-time sources, one distinct wide value per pinned register.
	h.SetGpr64(reg::s2, kSeed + 1); // -> $sp
	h.SetGpr64(reg::s3, kSeed + 2); // -> $ra
	h.SetGpr64(reg::s4, kSeed + 3); // -> $v0
	h.SetGpr64(reg::s5, kSeed + 4); // -> $v1
	h.SetGpr64(reg::s6, kSeed + 5); // -> $a0
	h.SetGpr64(reg::s7, kSeed + 6); // -> $a1
	h.SetGpr64(reg::t8, kSeed + 7); // -> $a2
	h.SetGpr64(reg::t7, kSeed + 8); // -> $s0
	h.SetGpr64(reg::t6, kSeed + 9); // -> $s1
}

// Block 1: dirty all nine pins from run-time values (saving the parking $ra
// in $t9), then end the block with a branch to the next instruction. The
// harness makes every block tail an event test, so the stub runs here with
// every pin dirty, and the interrupt is taken with EPC at block 2.
std::vector<u32> DirtyEveryPinThenEndBlock()
{
	return {
		OR(reg::t9, reg::ra, reg::zero),
		DADDU(reg::sp, reg::s2, reg::zero),
		DADDU(reg::ra, reg::s3, reg::zero),
		DADDU(reg::v0, reg::s4, reg::zero),
		DADDU(reg::v1, reg::s5, reg::zero),
		DADDU(reg::a0, reg::s6, reg::zero),
		DADDU(reg::a1, reg::s7, reg::zero),
		DADDU(reg::a2, reg::t8, reg::zero),
		DADDU(reg::s0, reg::t7, reg::zero),
		DADDU(reg::s1, reg::t6, reg::zero),
		BEQ(reg::zero, reg::zero, 1),
		NOP,
	};
}
constexpr u32 kBlock2Pc = kProgPc + 4 * 12;

// The interrupt handler: copy all nine pins into unpinned registers, so a pin
// that arrived wrong is consumed inside JIT code, and ERET to block 2.
std::vector<u32> CopyEveryPinHandler()
{
	return {
		DADDU(reg::t0, reg::sp, reg::zero),
		DADDU(reg::t1, reg::ra, reg::zero),
		DADDU(reg::t2, reg::v0, reg::zero),
		DADDU(reg::t3, reg::v1, reg::zero),
		DADDU(reg::t4, reg::a0, reg::zero),
		DADDU(reg::t5, reg::a1, reg::zero),
		DADDU(reg::a3, reg::a2, reg::zero),
		DADDU(reg::gp, reg::s0, reg::zero),
		DADDU(reg::s8, reg::s1, reg::zero),
		ERET,
	};
}

// Block 2: copy every pin again after the return, restore the parking $ra.
std::vector<u32> ReadEveryPinThenPark()
{
	return {
		DADDU(reg::s2, reg::sp, reg::zero),
		DADDU(reg::s3, reg::ra, reg::zero),
		DADDU(reg::s4, reg::v0, reg::zero),
		DADDU(reg::s5, reg::v1, reg::zero),
		DADDU(reg::s6, reg::a0, reg::zero),
		DADDU(reg::s7, reg::a1, reg::zero),
		DADDU(reg::t8, reg::a2, reg::zero),
		DADDU(reg::t7, reg::s0, reg::zero),
		DADDU(reg::t6, reg::s1, reg::zero),
		OR(reg::ra, reg::t9, reg::zero),
	};
}

std::vector<u32> Concat(std::initializer_list<std::vector<u32>> parts)
{
	std::vector<u32> out;
	for (const auto& p : parts)
		out.insert(out.end(), p.begin(), p.end());
	return out;
}

struct PinTraffic
{
	u32 stores = 0;
	u32 loads = 0;
};

// Pin stores (STR Xpin, [RSTATE, #GPR[n].UD[0]]) and reloads (LDR) in
// [begin, end): the 64-bit unsigned-offset forms armFlushEEGPRPins,
// armReloadEEGPRPins and their subsets emit.
PinTraffic CountPinTraffic(const u8* begin, const u8* end)
{
	PinTraffic t;
	const u32 rstate = RSTATE.GetCode();
	for (const u8* p = begin; p + 4 <= end; p += 4)
	{
		u32 w;
		std::memcpy(&w, p, 4);
		const bool str = (w & 0xFFC00000u) == 0xF9000000u;
		const bool ldr = (w & 0xFFC00000u) == 0xF9400000u;
		if ((!str && !ldr) || ((w >> 5) & 0x1F) != rstate)
			continue;
		const u32 rt = w & 0x1F;
		const u32 off = ((w >> 10) & 0xFFF) * 8;
		for (const EEPinnedGPR& pin : kEEPinTable)
		{
			const u32 pin_off = static_cast<u32>(reinterpret_cast<const u8*>(&cpuRegs.GPR.r[pin.gpr].UD[0]) -
				reinterpret_cast<const u8*>(&cpuRegs));
			if (pin.host.GetCode() == rt && pin_off == off)
				(str ? t.stores : t.loads)++;
		}
	}
	return t;
}

// The DispatcherEvent stub: it starts at DispatcherEvent and falls through
// into DispatcherReg, which it directly precedes.
void EventStubRange(const u8** begin, const u8** end)
{
	const void* event = nullptr;
	const void* reg = nullptr;
	recEeDispatcherAddrs(&event, &reg);
	*begin = static_cast<const u8*>(event);
	*end = static_cast<const u8*>(reg);
}

u32 CallerSavedPins()
{
	u32 n = 0;
	for (const EEPinnedGPR& pin : kEEPinTable)
		n += !armIsCalleeSavedRegister(static_cast<int>(pin.host.GetCode()));
	return n;
}

// What the hooks see. The event test at the block 1 tail is the one whose
// stored pc is block 2; the hooks ignore any other.
u64 s_seen[9];
bool s_seenSet;

constexpr u8 kPinnedOrder[9] = {29, 31, 2, 3, 4, 5, 6, 16, 17}; // sp ra v0 v1 a0 a1 a2 s0 s1

// Real host code in the event test uses the caller-saved registers; the
// harness's early-returning event test may not. Put junk in every AAPCS64
// caller-saved general register (the pins among them are x11-x13) so a stub
// that forgets to reload them fails here rather than by luck.
void ClobberCallerSavedRegisters()
{
	asm volatile(
		"mov x9, #0xdead\n\tmov x10, #0xdead\n\tmov x11, #0xdead\n\tmov x12, #0xdead\n\t"
		"mov x13, #0xdead\n\tmov x14, #0xdead\n\tmov x15, #0xdead\n\t"
		::: "x9", "x10", "x11", "x12", "x13", "x14", "x15");
}

void ReadGprMemoryAtBlock1Tail()
{
	ClobberCallerSavedRegisters();
	if (cpuRegs.pc != kBlock2Pc || s_seenSet)
		return;
	for (int i = 0; i < 9; i++)
		s_seen[i] = cpuRegs.GPR.r[kPinnedOrder[i]].UD[0];
	s_seenSet = true;
}

void RaiseInterruptAtBlock1Tail()
{
	ClobberCallerSavedRegisters();
	if (cpuRegs.pc != kBlock2Pc || s_seenSet)
		return;
	s_seenSet = true;
	cpuException(0x400, cpuRegs.branch); // INTC, as _cpuEventTest_Shared raises it
}

} // namespace

// A C reader inside the event test, the way a savestate save reached from
// the message pump at vsync is, must find every dirty pin's value in GPR
// memory.
TEST(EeRecEventStub, HostCodeInTheEventTestReadsEveryDirtyPinFromMemory)
{
	EeRecTestHarness h;
	SeedSources(h);
	s_seenSet = false;
	EventTestHook hook(ReadGprMemoryAtBlock1Tail);
	h.LoadProgram(Concat({DirtyEveryPinThenEndBlock(), ReadEveryPinThenPark()}));
	h.Run();
	ASSERT_TRUE(s_seenSet) << "no event test ran at the block 1 tail";
	for (int i = 0; i < 9; i++)
		EXPECT_EQ(s_seen[i], kSeed + 1 + i) << "GPR " << int(kPinnedOrder[i]);
}

// An interrupt delivered by the event test while every pin is dirty: the
// handler, as JIT code, must see the values the interrupted block computed,
// and so must the interrupted code after ERET.
TEST(EeRecEventStub, InterruptWithEveryPinDirtyReachesHandlerAndReturns)
{
	EeRecTestHarness h;
	h.EnableCop0();
	SeedSources(h);
	s_seenSet = false;
	InterruptHandler handler(h, CopyEveryPinHandler());
	EventTestHook hook(RaiseInterruptAtBlock1Tail);
	h.LoadProgram(Concat({DirtyEveryPinThenEndBlock(), ReadEveryPinThenPark()}));
	// The interpreter does not run recEventTest, so it never takes the
	// interrupt: check the JIT against the expected values instead.
	h.RunJitNoDiff();
	ASSERT_TRUE(s_seenSet) << "no event test ran at the block 1 tail";

	// The interrupt was taken at the block 1 tail, and ERET came back.
	EXPECT_EQ(h.GetCp0Jit(kCp0Epc), kBlock2Pc);
	EXPECT_EQ(h.GetCp0Jit(kCp0Cause) & 0x7Cu, 0u); // exception code 0: interrupt
	EXPECT_EQ(h.GetCp0Jit(kCp0Status) & 0x2u, 0u); // EXL cleared by ERET

	// The handler saw every dirty pin.
	EXPECT_EQ(h.GetGpr64Jit(reg::t0), kSeed + 1);
	EXPECT_EQ(h.GetGpr64Jit(reg::t1), kSeed + 2);
	EXPECT_EQ(h.GetGpr64Jit(reg::t2), kSeed + 3);
	EXPECT_EQ(h.GetGpr64Jit(reg::t3), kSeed + 4);
	EXPECT_EQ(h.GetGpr64Jit(reg::t4), kSeed + 5);
	EXPECT_EQ(h.GetGpr64Jit(reg::t5), kSeed + 6);
	EXPECT_EQ(h.GetGpr64Jit(reg::a3), kSeed + 7);
	EXPECT_EQ(h.GetGpr64Jit(reg::gp), kSeed + 8);
	EXPECT_EQ(h.GetGpr64Jit(reg::s8), kSeed + 9);

	// So did the interrupted code after the return.
	EXPECT_EQ(h.GetGpr64Jit(reg::s2), kSeed + 1);
	EXPECT_EQ(h.GetGpr64Jit(reg::s3), kSeed + 2);
	EXPECT_EQ(h.GetGpr64Jit(reg::s4), kSeed + 3);
	EXPECT_EQ(h.GetGpr64Jit(reg::s5), kSeed + 4);
	EXPECT_EQ(h.GetGpr64Jit(reg::s6), kSeed + 5);
	EXPECT_EQ(h.GetGpr64Jit(reg::s7), kSeed + 6);
	EXPECT_EQ(h.GetGpr64Jit(reg::t8), kSeed + 7);
	EXPECT_EQ(h.GetGpr64Jit(reg::t7), kSeed + 8);
	EXPECT_EQ(h.GetGpr64Jit(reg::t6), kSeed + 9);
}

// The same program with nothing for the event test to do: the stub still
// runs at the block 1 tail, returns normally, and block 2 reads every pin.
TEST(EeRecEventStub, QuietEventTestWithEveryPinDirty)
{
	EeRecTestHarness h;
	SeedSources(h);
	EventTestHook hook(ClobberCallerSavedRegisters);
	h.LoadProgram(Concat({DirtyEveryPinThenEndBlock(), ReadEveryPinThenPark()}));
	h.Run();
	h.ExpectGpr64(reg::s2, kSeed + 1);
	h.ExpectGpr64(reg::s3, kSeed + 2);
	h.ExpectGpr64(reg::s4, kSeed + 3);
	h.ExpectGpr64(reg::s5, kSeed + 4);
	h.ExpectGpr64(reg::s6, kSeed + 5);
	h.ExpectGpr64(reg::s7, kSeed + 6);
	h.ExpectGpr64(reg::t8, kSeed + 7);
	h.ExpectGpr64(reg::t7, kSeed + 8);
	h.ExpectGpr64(reg::t6, kSeed + 9);
}

// ---- Shape ----

// All nine pins are stored before recEventTest (savestate save at vsync and
// the exit path read GPR memory); only the caller-saved ones are reloaded.
TEST(EeRecEventStub, StoresEveryPinAndReloadsOnlyCallerSavedPins)
{
	EeRecTestHarness h;
	h.LoadProgram({NOP});
	h.Run(); // the dispatchers exist from here on
	const u8* begin;
	const u8* end;
	EventStubRange(&begin, &end);
	ASSERT_LT(begin, end);
	const PinTraffic t = CountPinTraffic(begin, end);
	EXPECT_EQ(t.stores, static_cast<u32>(std::size(kEEPinTable)));
	EXPECT_EQ(t.loads, CallerSavedPins());
}

// recEventTest can reset the recompiler in place and return into the
// regenerated stub at the same offset, so a regeneration must emit the stub
// byte for byte (feedback_ee_rec_reset_in_place_needs_identical_stubs).
TEST(EeRecEventStub, ResetRegeneratesTheStubByteForByte)
{
	EeRecTestHarness h;
	h.LoadProgram({NOP});
	h.Run();
	const u8* begin;
	const u8* end;
	EventStubRange(&begin, &end);
	ASSERT_LT(begin, end);
	const std::vector<u8> before(begin, end);

	recCpu.Reset(); // not executing, so this regenerates the dispatchers now
	h.Run();

	const u8* begin2;
	const u8* end2;
	EventStubRange(&begin2, &end2);
	EXPECT_EQ(begin2, begin);
	ASSERT_EQ(end2, end);
	EXPECT_EQ(std::memcmp(begin2, before.data(), before.size()), 0);
}

// The tests above name the nine pinned guest registers by number
// (kPinnedOrder, the Dirty/Copy/Read programs) and use other registers as
// run-time sources and sinks, which must not be pins. Fails first when
// kEEPinTable moves.
TEST(EeRecEventStub, TableMatchesTheRegistersThisFileHardCodes)
{
	EXPECT_EQ(std::size(kEEPinTable), std::size(kPinnedOrder));
	for (u8 gpr : kPinnedOrder)
		EXPECT_NE(armEEPinForGPR(gpr), nullptr) << "guest r" << int(gpr) << " is not pinned";
	for (u32 gpr : {reg::s2, reg::s3, reg::s4, reg::s5, reg::s6, reg::s7, reg::t8, reg::t7, reg::t6, reg::t9,
			reg::t0, reg::t1, reg::t2, reg::t3, reg::t4, reg::t5, reg::a3, reg::gp, reg::s8})
		EXPECT_EQ(armEEPinForGPR(static_cast<int>(gpr)), nullptr) << "guest r" << gpr << " is pinned";
}
