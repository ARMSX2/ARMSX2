// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The system-call round trip with lazy-dirty pins: SYSCALL in user code, a
// guest kernel handler at the common exception vector that runs as JIT code,
// MTC0 Status writes inside it, and ERET / EI back out.
//
// Each of those instructions is an interpreter call. The pins are newest and
// memory is stale while a pin is dirty, so before a C callee reads a guest
// GPR from memory its pin must be stored, and a pin in a caller-saved host
// register (x11-x13) must be stored before and reloaded after any C call,
// because the call destroys the register. A callee that writes no guest GPR
// needs nothing more: callee-saved pins ride through the call in their
// registers.
//
// The correctness tests seed every pinned register from a run-time value
// (not a constant), so all nine pins are dirty at the SYSCALL, and the
// kernel handler reads pinned registers other than $v1 and $a0-$a2.
// The pins are $sp $ra $v0 $v1 $a0 $a1 $a2 $s0 $s1, with $a2/$s0/$s1 in the
// caller-saved host registers x12/x13/x11; the guard test at the end of the
// file fails if the table stops matching what this file hard-codes. The
// shape tests count the pin stores and reloads emitted at each seam, so a
// seam that still flushes and reloads the whole table shows up even though
// it computes the right answer.

#include "harness/EeRecTestHarness.h"

#include "arm64/iR5900-arm64.h"

#include "R5900.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

using namespace recompiler_tests;
using namespace mips;
using namespace mips::ee;

extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);

namespace {

constexpr u32 kProgPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kVector = 0x80000180;
constexpr u32 kCp0Status = 12;
constexpr u32 kCp0Epc = 14;

// GetThreadId: a call number the interpreter's SYSCALL handles with no case
// of its own (reads $v1 only, writes no GPR), as in Shadow of the Colossus's
// hot stubs.
constexpr s16 kGetThreadId = 0x2F;

// Installs a guest kernel handler at the common exception vector and puts
// the environment's `jr ra; nop` stub back afterwards. WriteU32 invalidates
// any block compiled over the words it writes.
class KernelHandler
{
public:
	KernelHandler(EeRecTestHarness& h, const std::vector<u32>& words)
		: h_(h)
		, size_(words.size())
	{
		for (size_t i = 0; i < words.size(); i++)
			h_.WriteU32(kVector + static_cast<u32>(i * 4), words[i]);
	}
	~KernelHandler()
	{
		h_.WriteU32(kVector + 0, JR(reg::ra));
		h_.WriteU32(kVector + 4, NOP);
		for (size_t i = 2; i < size_; i++)
			h_.WriteU32(kVector + static_cast<u32>(i * 4), NOP);
	}

private:
	EeRecTestHarness& h_;
	size_t size_;
};

// The handler the round-trip tests install. It reads five pinned registers
// that are neither $v1 nor $a0-$a2 into unpinned ones, makes two MTC0 Status
// writes (one whose source is a callee-saved pin, $v0, and one whose source
// is a caller-saved pin, $s1), steps EPC past the SYSCALL through $k0 as the
// BIOS does, and returns with ERET. $k0 is plain scratch here (it is no longer
// a pin).
std::vector<u32> RoundTripHandler()
{
	return {
		DADDU(reg::t0, reg::sp, reg::zero),
		DADDU(reg::t1, reg::ra, reg::zero),
		DADDU(reg::t2, reg::v0, reg::zero),
		DADDU(reg::t3, reg::s0, reg::zero),
		DADDU(reg::t4, reg::s1, reg::zero),
		// Status <- $v0: Interp::MTC0 reads $v0 from memory, so its dirty pin
		// must be stored first.
		MFC0(reg::v0, kCp0Status),
		MTC0(reg::v0, kCp0Status),
		DADDU(reg::v0, reg::t2, reg::zero), // give the caller its $v0 back
		// Status <- $s1 (caller-saved pin).
		MFC0(reg::s1, kCp0Status),
		MTC0(reg::s1, kCp0Status),
		DADDU(reg::s1, reg::t4, reg::zero), // give the caller its $s1 back
		// EPC += 4, through $k0, as the BIOS does.
		MFC0(reg::k0, kCp0Epc),
		ADDIU(reg::k0, reg::k0, 4),
		MTC0(reg::k0, kCp0Epc),
		ERET,
	};
}

void SeedSources(EeRecTestHarness& h)
{
	// Run-time sources for the nine pins. A distinct, wide value per register
	// so a value landing in the wrong place or going stale cannot match.
	h.SetGpr64(reg::s2, 0x1111222233334441ull); // -> $sp
	h.SetGpr64(reg::s3, 0x1111222233334442ull); // -> $ra
	h.SetGpr64(reg::s4, 0x1111222233334443ull); // -> $v0
	h.SetGpr64(reg::s5, 0x1111222233334444ull); // -> $a0
	h.SetGpr64(reg::s6, 0x1111222233334445ull); // -> $a1
	h.SetGpr64(reg::s7, 0x1111222233334446ull); // -> $a2
	h.SetGpr64(reg::t8, 0x1111222233334447ull); // -> $s0
	h.SetGpr64(reg::t6, 0x1111222233334448ull); // -> $s1
	h.SetGpr64(reg::t7, 0x1111222233334449ull); // -> $v1 (dynamic call number tests)
	// What memory holds for the pinned registers before the program dirties
	// them. If a seam lets a C callee read a stale $v0, Status receives this
	// value (bit 24 set, which is reserved) instead of the one MFC0 read.
	h.SetGpr64(reg::v0, 0x0000000011000002ull);
}

// Dirties all nine pins from run-time values, saving the parking $ra in $t9.
std::vector<u32> DirtyEveryPin()
{
	return {
		OR(reg::t9, reg::ra, reg::zero),
		DADDU(reg::sp, reg::s2, reg::zero),
		DADDU(reg::ra, reg::s3, reg::zero),
		DADDU(reg::v0, reg::s4, reg::zero),
		DADDU(reg::a0, reg::s5, reg::zero),
		DADDU(reg::a1, reg::s6, reg::zero),
		DADDU(reg::a2, reg::s7, reg::zero),
		DADDU(reg::s0, reg::t8, reg::zero),
		DADDU(reg::s1, reg::t6, reg::zero),
	};
}

// After the kernel returns: copy every pin into an unpinned register, so a
// pin that came back wrong is consumed inside JIT code, then park.
std::vector<u32> ReadEveryPinThenPark()
{
	return {
		DADDU(reg::a3, reg::sp, reg::zero),
		DADDU(reg::gp, reg::ra, reg::zero),
		DADDU(reg::s8, reg::v0, reg::zero),
		DADDU(reg::at, reg::a0, reg::zero),
		DADDU(reg::k1, reg::a1, reg::zero),
		DADDU(reg::t5, reg::a2, reg::zero),
		DADDU(reg::s2, reg::s0, reg::zero),
		DADDU(reg::s3, reg::s1, reg::zero),
		DADDU(reg::s4, reg::v1, reg::zero),
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

// Pin stores (STR Xpin, [RSTATE, #GPR[n].UD[0]]) and reloads (LDR) in a
// compiled block's host code. Only the 64-bit unsigned-offset forms, which is
// what armFlushEEGPRPins / armReloadEEGPRPins and their subsets emit.
struct PinTraffic
{
	u32 stores = 0;
	u32 loads = 0;
};

PinTraffic CountPinTraffic(u32 guest_pc)
{
	PinTraffic t;
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
	{
		ADD_FAILURE() << "no compiled block at 0x" << std::hex << guest_pc;
		return t;
	}
	const u32* code = reinterpret_cast<const u32*>(fn);
	const u32 rstate = RSTATE.GetCode();
	for (u32 i = 0; i < size / 4; i++)
	{
		const u32 w = code[i];
		const bool str = (w & 0xFFC00000u) == 0xF9000000u;
		const bool ldr = (w & 0xFFC00000u) == 0xF9400000u;
		if (!str && !ldr)
			continue;
		const u32 rt = w & 0x1F;
		const u32 rn = (w >> 5) & 0x1F;
		const u32 off = ((w >> 10) & 0xFFF) * 8;
		if (rn != rstate)
			continue;
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

u32 CallerSavedPins()
{
	u32 n = 0;
	for (const EEPinnedGPR& pin : kEEPinTable)
		n += !armIsCalleeSavedRegister(static_cast<int>(pin.host.GetCode()));
	return n;
}

} // namespace

// The whole round trip with a constant call number, the shape Shadow of the
// Colossus runs 360,000 times a second: every pin dirty at the SYSCALL, the
// kernel handler reading and writing pins as JIT code across two MTC0 Status
// seams and an ERET, user code reading every pin after the return.
TEST(EeRecSyscallSeam, ConstantCallRoundTripWithEveryPinDirty)
{
	EeRecTestHarness h;
	h.EnableCop0();
	SeedSources(h);
	KernelHandler kernel(h, RoundTripHandler());
	h.LoadProgram(Concat({
		DirtyEveryPin(),
		{ADDIU(reg::v1, reg::zero, kGetThreadId), SYSCALL_()},
		ReadEveryPinThenPark(),
	}));
	h.Run();
	// The kernel saw the caller's dirty values.
	h.ExpectGpr64(reg::t0, 0x1111222233334441ull);
	h.ExpectGpr64(reg::t1, 0x1111222233334442ull);
	h.ExpectGpr64(reg::t2, 0x1111222233334443ull);
	h.ExpectGpr64(reg::t3, 0x1111222233334447ull);
	h.ExpectGpr64(reg::t4, 0x1111222233334448ull);
	// The caller saw its own values back, and the kernel's $k0.
	h.ExpectGpr64(reg::a3, 0x1111222233334441ull);
	h.ExpectGpr64(reg::gp, 0x1111222233334442ull);
	h.ExpectGpr64(reg::s8, 0x1111222233334443ull);
	h.ExpectGpr64(reg::at, 0x1111222233334444ull);
	h.ExpectGpr64(reg::k1, 0x1111222233334445ull);
	h.ExpectGpr64(reg::t5, 0x1111222233334446ull);
	h.ExpectGpr64(reg::s2, 0x1111222233334447ull);
	h.ExpectGpr64(reg::s3, 0x1111222233334448ull);
	h.ExpectGpr64(reg::s4, static_cast<u64>(kGetThreadId));
	h.ExpectGpr64(reg::k0, static_cast<u64>(kProgPc + 4 * 11));
	// EXL cleared by ERET; Status otherwise what MFC0 read (no stale $v0).
	EXPECT_EQ(h.GetCp0Jit(kCp0Status) & (1u << 24), 0u);
	EXPECT_EQ(h.GetCp0Interp(kCp0Status) & 0x2u, 0u);
}

// Same round trip with the call number held in a register (not known at
// compile time). The seam cannot know which interpreter case runs, so it
// keeps the full flush and reload; this pins that the fallback stays correct.
TEST(EeRecSyscallSeam, DynamicCallRoundTripWithEveryPinDirty)
{
	EeRecTestHarness h;
	h.EnableCop0();
	SeedSources(h);
	h.SetGpr64(reg::t7, kGetThreadId);
	KernelHandler kernel(h, RoundTripHandler());
	h.LoadProgram(Concat({
		DirtyEveryPin(),
		{DADDU(reg::v1, reg::t7, reg::zero), SYSCALL_()},
		ReadEveryPinThenPark(),
	}));
	h.Run();
	h.ExpectGpr64(reg::t0, 0x1111222233334441ull);
	h.ExpectGpr64(reg::t4, 0x1111222233334448ull);
	h.ExpectGpr64(reg::a3, 0x1111222233334441ull);
	h.ExpectGpr64(reg::s3, 0x1111222233334448ull);
	h.ExpectGpr64(reg::s4, static_cast<u64>(kGetThreadId));
}

// A negative constant in $v1 selects the same call (the interpreter takes
// -$v1). The seam must derive the call number the same way.
TEST(EeRecSyscallSeam, NegativeConstantCallNumber)
{
	EeRecTestHarness h;
	h.EnableCop0();
	SeedSources(h);
	KernelHandler kernel(h, RoundTripHandler());
	h.LoadProgram(Concat({
		DirtyEveryPin(),
		{ADDIU(reg::v1, reg::zero, static_cast<s16>(-kGetThreadId)), SYSCALL_()},
		ReadEveryPinThenPark(),
	}));
	h.Run();
	h.ExpectGpr64(reg::t4, 0x1111222233334448ull);
	h.ExpectGpr64(reg::s4, static_cast<u64>(static_cast<s64>(-kGetThreadId)));
}

// EI with every pin dirty, then every pin read back. EI reads and writes no
// guest GPR.
TEST(EeRecSyscallSeam, EiWithEveryPinDirty)
{
	EeRecTestHarness h;
	SeedSources(h);
	h.SetGpr64(reg::v1, 0x5555666677778888ull);
	h.LoadProgram(Concat({
		DirtyEveryPin(),
		{ee::EI},
		ReadEveryPinThenPark(),
	}));
	h.Run();
	h.ExpectGpr64(reg::a3, 0x1111222233334441ull);
	h.ExpectGpr64(reg::s8, 0x1111222233334443ull);
	h.ExpectGpr64(reg::s2, 0x1111222233334447ull);
	h.ExpectGpr64(reg::s3, 0x1111222233334448ull);
	EXPECT_NE(h.GetCp0Interp(kCp0Status) & (1u << 16), 0u);
}

// ---- Shape: how much pin traffic each seam emits ----
//
// A seam whose callee reads at most a known set of GPRs and writes none
// should store the pins it reads plus the caller-saved pins, and reload only
// the caller-saved pins. The program blocks below contain no other C seam,
// so every pin store or reload in them belongs to the seam under test.

TEST(EeRecSyscallSeam, ConstantCallSyscallFlushesOnlyV1AndCallerSavedPins)
{
	EeRecTestHarness h;
	h.EnableCop0();
	SeedSources(h);
	KernelHandler kernel(h, RoundTripHandler());
	h.LoadProgram(Concat({
		DirtyEveryPin(),
		{ADDIU(reg::v1, reg::zero, kGetThreadId), SYSCALL_()},
		ReadEveryPinThenPark(),
	}));
	h.Run();
	const PinTraffic t = CountPinTraffic(kProgPc);
	// $v1 (the call number the interpreter reads) plus the caller-saved pins.
	EXPECT_EQ(t.stores, 1 + CallerSavedPins());
	EXPECT_EQ(t.loads, CallerSavedPins());
}

TEST(EeRecSyscallSeam, KernelMtc0StatusAndEretFlushOnlyWhatTheyRead)
{
	EeRecTestHarness h;
	h.EnableCop0();
	SeedSources(h);
	KernelHandler kernel(h, RoundTripHandler());
	h.LoadProgram(Concat({
		DirtyEveryPin(),
		{ADDIU(reg::v1, reg::zero, kGetThreadId), SYSCALL_()},
		ReadEveryPinThenPark(),
	}));
	h.Run();
	const PinTraffic t = CountPinTraffic(kVector);
	const u32 cs = CallerSavedPins();
	// MTC0 Status <- $v0: $v0 (callee-saved) + caller-saved, reload caller-saved.
	// MTC0 Status <- $s1: caller-saved only ($s1 is one of them).
	// ERET: caller-saved only.
	EXPECT_EQ(t.stores, (1 + cs) + cs + cs);
	EXPECT_EQ(t.loads, cs + cs + cs);
}

TEST(EeRecSyscallSeam, EiFlushesOnlyCallerSavedPins)
{
	EeRecTestHarness h;
	SeedSources(h);
	h.LoadProgram(Concat({
		DirtyEveryPin(),
		{ee::EI},
		ReadEveryPinThenPark(),
	}));
	h.Run();
	const PinTraffic t = CountPinTraffic(kProgPc);
	EXPECT_EQ(t.stores, CallerSavedPins());
	EXPECT_EQ(t.loads, CallerSavedPins());
}

// The dynamic call number keeps the full table flush and reload.
TEST(EeRecSyscallSeam, DynamicCallSyscallKeepsFullFlushAndReload)
{
	EeRecTestHarness h;
	h.EnableCop0();
	SeedSources(h);
	h.SetGpr64(reg::t7, kGetThreadId);
	KernelHandler kernel(h, RoundTripHandler());
	h.LoadProgram(Concat({
		DirtyEveryPin(),
		{DADDU(reg::v1, reg::t7, reg::zero), SYSCALL_()},
		ReadEveryPinThenPark(),
	}));
	h.Run();
	const PinTraffic t = CountPinTraffic(kProgPc);
	EXPECT_EQ(t.stores, static_cast<u32>(std::size(kEEPinTable)));
	EXPECT_EQ(t.loads, static_cast<u32>(std::size(kEEPinTable)));
}

// The registers above are hard-coded: nine pins by guest number, and a set of
// run-time sources, sinks and kernel scratch registers that must NOT be pins,
// or a "pin" test would be reading an allocator-resident register. Fails first
// when kEEPinTable moves.
TEST(EeRecSyscallSeam, TableMatchesTheRegistersThisFileHardCodes)
{
	for (u32 gpr : {reg::sp, reg::ra, reg::v0, reg::v1, reg::a0, reg::a1, reg::a2, reg::s0, reg::s1})
		EXPECT_NE(armEEPinForGPR(static_cast<int>(gpr)), nullptr) << "guest r" << gpr << " is not pinned";
	EXPECT_EQ(std::size(kEEPinTable), 9u);

	// The Status writes: $v0 is the callee-saved case, $s1 the caller-saved one.
	ASSERT_NE(armEEPinForGPR(reg::v0), nullptr);
	ASSERT_NE(armEEPinForGPR(reg::s1), nullptr);
	EXPECT_TRUE(armIsCalleeSavedRegister(static_cast<int>(armEEPinForGPR(reg::v0)->GetCode())));
	EXPECT_FALSE(armIsCalleeSavedRegister(static_cast<int>(armEEPinForGPR(reg::s1)->GetCode())));

	for (u32 gpr : {reg::s2, reg::s3, reg::s4, reg::s5, reg::s6, reg::s7, reg::t8, reg::t6, reg::t7, reg::t9,
			reg::t0, reg::t1, reg::t2, reg::t3, reg::t4, reg::t5, reg::a3, reg::gp, reg::s8, reg::at, reg::k0,
			reg::k1})
		EXPECT_EQ(armEEPinForGPR(static_cast<int>(gpr)), nullptr) << "guest r" << gpr << " is pinned";
}
