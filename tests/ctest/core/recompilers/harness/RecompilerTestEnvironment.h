// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/FPControl.h"
#include "common/Pcsx2Types.h"

namespace recompiler_tests {

// Stand-up logic for a headless PCSX2 core suitable for in-process
// recompiler work — no Qt, no VMManager, no BIOS. Used by both the gtest
// recompiler suite (via RecompilerTestGtestEnvironment) and the
// pcsx2-vurunner binary, which is gtest-free.
//
// Lifecycle (Initialize):
//   1. FPU default rounding mode.
//   2. cpuinfo_initialize().
//   3. SysMemory::Allocate()    — host VM memory (EE/IOP/VU RAM + rec buffers).
//   4. psxRec.Reserve()          — BASEBLOCK tables + dispatcher.
//   5. psxInt.Reserve()          — no-op, for API symmetry.
//   6. SysMemory::Reset()        — zero + remap mirrors.
//   7. psxRec.Reset()            — compile dispatcher, clear LUT.
//   8. psxInt.Reset()            — no-op.
//   9. Install a parking-lot (infinite NOP loop) at kParkingPc so any test
//      program's `jr ra` with ra=kParkingPc settles in predictable code.
//
// Shutdown reverses 4-8 and releases SysMemory.
class RecompilerTestEnvironment
{
public:
	// Guest addresses reserved for the harness.
	//   [kProgramPc, kProgramPc + 4KB)   test program emit region
	//   [kScratchPc, kScratchPc + 4KB)   scratch data memory for load/store tests
	//   [kParkingPc, kParkingPc + 8)     `j self; nop` — safe `ra` target
	static constexpr u32 kProgramPc = 0x00010000;
	static constexpr u32 kScratchAddr = 0x00020000;
	static constexpr u32 kParkingPc   = 0x001F0000;

	// Idempotent: returns true if already initialized. Returns false if
	// SysMemory::Allocate or other one-shot setup failed; the caller must
	// not invoke any harness/replay primitives in that case.
	static bool Initialize();
	static void Shutdown();

	// Returns true once Initialize() has completed successfully.
	static bool IsReady();

	// Invalidate microVU's per-VU block cache via mVUreset so a test's JIT
	// compile cannot inherit a cached block from a prior test that happened
	// to land at the same start_pc with a matching microRegInfo. Call from
	// the VU harness's SeedEntryState() (i.e. once per Run()).
	static void ResetVuBlockCache(int vu_index);
};

// Opt a test into the IEEE-ish FP environment: round-to-nearest, denormals
// live. Scoped, restores on exit.
//
// The harnesses run the EE under the environment a real game runs it under --
// EmuConfig.Cpu.FPUFPCR, which defaults to DAZ+FTZ+ChopZero (0x1c00000 on
// aarch64, measured live in recExecute). That is the right default for a
// differential suite, and it is what EeRecTestHarness now establishes.
//
// Some behaviour is only REACHABLE without it, and that is not a defect of the
// test:
//
//   - `DenormalsAreZero` is a per-unit user setting (EmuCore/CPU:
//     FPU/VU0/VU1.DenormalsAreZero), so FZ-off is a supported configuration,
//     and it is the one in which PCSX2 reproduces the console's VU MAC U bit.
//     With FZ on, the mantissa U is defined over is erased before either engine
//     can look at it, and both engines lose the bit together.
//   - Round-toward-zero turns every overflow into a saturation: an overflowing
//     product rounds to +/-FLT_MAX (exp 254), never to Inf (exp 255). Any code
//     -- or test -- that detects overflow by looking for Inf is therefore inert
//     under the production rounding mode. That single fact is behind the VU O
//     flag, the EE FPU MADD/MSUB "unclamped intermediate product" cases, and
//     the FCR31 overflow bit.
//
// A test that declares this guard is stating "my subject needs denormals and/or
// Inf to exist", which is a claim about the test, not a workaround. Tests that
// do NOT declare it run as a game would.
// It works by rewriting EmuConfig.Cpu's four FPCRs for the scope, not by poking
// the host register behind the harness's back. That is deliberate: the state it
// selects is a state a USER can select (EmuCore/CPU Roundmode and
// DenormalsAreZero), and rewriting the config is how the whole stack finds out
// -- the harness's ambient set, the FPUDivFPCR swap the EE DIV/SQRT emitters
// bake as an immediate, and mVU's mvuNeedsFPCRUpdate gate all read from there.
// Poking only the host register would leave those three disagreeing with it.
//
// Consequence to respect: EE DIV/SQRT bake FPUFPCR into the block as an
// immediate, and mVU hashes all four into its options sentinel, so a block
// compiled under one environment must not be reused under another. The default
// RunMode::FreshCache and mVU's sentinel check both handle this; a test that
// deliberately preserves the block cache across a change of environment would
// not be covered.
struct ScopedFpEnv
{
	enum Kind
	{
		// Round-to-nearest, denormals live, no flush. The environment in which
		// the VU MAC U bit exists at all.
		IeeeNearest,
		// Round-to-nearest, denormals flushed -- i.e. production with the
		// rounding mode changed, and bit-for-bit the default FPUDivFPCR, the
		// environment the EE FPU's own DIV/SQRT/RSQRT already run under. What
		// the EE FPU tests want: they need overflow to reach Inf, but they are
		// built around FZ and diverge between engines without it (the FPU
		// interpreter flushes denormals in software via fpuDouble; the JIT
		// leaves it to the hardware).
		FlushNearest,
	};

	explicit ScopedFpEnv(Kind kind = IeeeNearest);
	~ScopedFpEnv();

	ScopedFpEnv(const ScopedFpEnv&) = delete;
	ScopedFpEnv& operator=(const ScopedFpEnv&) = delete;

private:
	FPControlRegister saved_host_;
	FPControlRegister saved_fpu_, saved_fpu_div_, saved_vu0_, saved_vu1_;
};

} // namespace recompiler_tests
