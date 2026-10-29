// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// EE cycle-rate selector: what the seven settings charge, and which of the two
// selectors (configured vs effective) the emitted code is built from.
//
// The charge formula is written out twice by hand, in the EE interpreter
// (intUpdateCPUCycles) and in the AArch64 EE recompiler
// (scaleblockcycles_calculation / _clear), and the recompiler bakes its result
// into the block tail as an immediate. These tests pin both copies.
//
// The charge table characterizes what both engines do, including the parts that
// look arbitrary (the tuned 80/168 window at rate -1, the 40-cycle floor below
// which nothing is scaled, the wider remainder mask at +2/+3). Changing a number
// in it changes guest timing for every game.
//
// A block compiled while the governor holds the effective selector at -1 must be
// byte-identical to one compiled with -1 configured. If it is not, some site
// still reads EmuConfig instead of EECycleRate::GetEffective().

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "EECycleRate.h"
#include "R5900.h"
#include "VU.h"
#include "VUmicro.h"
#include "vtlb.h"

#include "arm64/iR5900-arm64.h"

#include <gtest/gtest.h>

#include <functional>
#include <vector>

// Test-only scaler drivers (iR5900-arm64.cpp / Interpreter.cpp), declared here
// rather than in a header. Each sets the engine's own raw block-cycle
// accumulator, runs the production scaler, and returns the charge plus the
// remainder carried to the next block.
extern u32 recEeScaleBlockCyclesForTest(u32 raw_block_cycles, u32* out_remainder);
extern u32 intScaleBlockCyclesForTest(u32 raw_block_cycles, u32* out_remainder);
extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);


using namespace recompiler_tests;
using namespace mips;

namespace {

constexpr u32 kProgramPc = RecompilerTestEnvironment::kProgramPc;

// Sets the configured selector, and the effective one with it, for one test.
struct ScopedCycleRate
{
	s8 prev_configured;

	explicit ScopedCycleRate(int configured)
		: prev_configured(EmuConfig.Speedhacks.EECycleRate)
	{
		EmuConfig.Speedhacks.EECycleRate = static_cast<s8>(configured);
		EECycleRate::SyncToConfigured();
	}

	// The re-sync also undoes any ApplyEffective() the test made.
	~ScopedCycleRate()
	{
		EmuConfig.Speedhacks.EECycleRate = prev_configured;
		EECycleRate::SyncToConfigured();
	}
};

struct Expect
{
	u32 charge;
	u32 remainder;
};

struct CycleRateCase
{
	u32    raw;
	Expect by_selector[7]; // index 0 == selector -3 ... index 6 == selector +3
};

// charge / retained-remainder for each raw 3-bit-fixed-point block cost.
//
//   raw <= 40      no scaling at any selector: charge = raw >> 3.
//   selector -1    5/32 of raw, except in the manually tuned (80, 168] window
//                  where it is 7/32 — the 80->81 and 168->169 rows are that
//                  boundary.
//   selector -2/-3 7/32 and 9/32 of raw.
//   selector +1    (raw >> 3) / 1.3, truncated.
//   selector +2/+3 raw >> 4 and raw >> 5.
//   every selector charges at least 1.
//   the remainder is raw & 7, except at +2 (raw & 15) and +3 (raw & 31) for a
//   block that was eligible for scaling at all (raw > 40).
const CycleRateCase kCases[] = {
	{    0, {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}}},
	{    1, {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}}},
	{    7, {{1, 7}, {1, 7}, {1, 7}, {1, 7}, {1, 7}, {1, 7}, {1, 7}}},
	{    8, {{1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}}},
	{   40, {{5, 0}, {5, 0}, {5, 0}, {5, 0}, {5, 0}, {5, 0}, {5, 0}}},
	{   41, {{11, 1}, {8, 1}, {6, 1}, {5, 1}, {3, 1}, {2, 9}, {1, 9}}},
	{   79, {{22, 7}, {17, 7}, {12, 7}, {9, 7}, {6, 7}, {4, 15}, {2, 15}}},
	{   80, {{22, 0}, {17, 0}, {12, 0}, {10, 0}, {7, 0}, {5, 0}, {2, 16}}},
	{   81, {{22, 1}, {17, 1}, {17, 1}, {10, 1}, {7, 1}, {5, 1}, {2, 17}}},
	{  168, {{47, 0}, {36, 0}, {36, 0}, {21, 0}, {16, 0}, {10, 8}, {5, 8}}},
	{  169, {{47, 1}, {36, 1}, {26, 1}, {21, 1}, {16, 1}, {10, 9}, {5, 9}}},
	{  320, {{90, 0}, {70, 0}, {50, 0}, {40, 0}, {30, 0}, {20, 0}, {10, 0}}},
	{ 1023, {{287, 7}, {223, 7}, {159, 7}, {127, 7}, {97, 7}, {63, 15}, {31, 31}}},
	{ 4096, {{1152, 0}, {896, 0}, {640, 0}, {512, 0}, {393, 0}, {256, 0}, {128, 0}}},
	{65535, {{18431, 7}, {14335, 7}, {10239, 7}, {8191, 7}, {6300, 7}, {4095, 15}, {2047, 31}}},
};

// A block long enough to clear the 40-cycle no-scaling floor, so its charge
// really does depend on the selector.
std::vector<u32> LongBlock()
{
	std::vector<u32> prog;
	for (int i = 0; i < 32; i++)
		prog.push_back(ADDIU(reg::a1, reg::a1, 1));
	return prog;
}

// A block whose charge outgrows ADD's 12-bit immediate two steps below the
// configured rate. At configured 0 the governor's window bottoms out at -2,
// which charges 7*raw/32; that first stops being encodable at raw 18730. PDIVW
// costs 176 raw units (Cycles::MMI_Div), doubled when CP0 Config bit 18 is
// clear, so 128 of them are past that either way.
std::vector<u32> DeepDivideBlock()
{
	std::vector<u32> prog;
	for (int i = 0; i < 128; i++)
		prog.push_back(ee::PDIVW(reg::a1, reg::a2));
	return prog;
}

// Compile LongBlock() from an empty code cache and return the host bytes of the
// block at kProgramPc. The reset rewinds the emit pointer, so the same program
// lands at the same host address and the bytes are comparable across calls.
std::vector<u8> CaptureBlockBytes(int configured, int effective)
{
	EmuConfig.Speedhacks.EECycleRate = static_cast<s8>(configured);
	EECycleRate::SyncToConfigured();
	if (effective != configured)
		EXPECT_TRUE(EECycleRate::ApplyEffective(static_cast<s8>(effective), "ee_rec_cycle_rate_tests"));

	recCpu.Reset();

	EeRecTestHarness h;
	h.SetGpr64(reg::a1, 0);
	h.LoadProgram(LongBlock());
	h.Run();

	uptr fnptr = 0, lut = 0;
	u32  host_size = 0;
	EXPECT_TRUE(recEeBlockHostInfo(kProgramPc, &fnptr, &host_size, &lut));
	if (!fnptr || !host_size)
		return {};

	const u8* p = reinterpret_cast<const u8*>(fnptr);
	return std::vector<u8>(p, p + host_size);
}

// The patch pass only runs when the EE recompiler is the active provider, and
// EeRecTestHarness::Run() leaves the global pointer back on the interpreter.
bool RepatchAsRecCpu(int selector)
{
	R5900cpu* const saved = Cpu;
	Cpu = &recCpu;
	const bool ok = recEeRepatchCycleCharges(static_cast<s8>(selector));
	Cpu = saved;
	return ok;
}

// A deterministic starting state for a byte comparison. The recompiler reset
// rewinds the emit pointer so the same program lands at the same host address;
// the vtlb block-tracking reset puts the program page back to ProtMode_None, so
// the manual-protection prologue is present or absent the same way in every run.
void ResetRecAndPageProtection()
{
	recCpu.Reset();
	mmap_ResetBlockTracking();
}

std::vector<u8> BlockBytesAt(u32 pc)
{
	uptr fnptr = 0, lut = 0;
	u32 host_size = 0;
	if (!recEeBlockHostInfo(pc, &fnptr, &host_size, &lut) || !fnptr || host_size == 0)
		return {};
	const u8* p = reinterpret_cast<const u8*>(fnptr);
	return std::vector<u8>(p, p + host_size);
}

// The cold side-exit arena, in full. Superblock taken arms are outlined there
// and carry the MOVZ charge site, which no block's own extent covers.
std::vector<u8> ArenaBytes(bool cold)
{
	uptr base = 0;
	u32 used = 0;
	if (!recEeArenaExtent(cold, &base, &used) || used == 0)
		return {};
	const u8* p = reinterpret_cast<const u8*>(base);
	return std::vector<u8>(p, p + used);
}

std::vector<u8> ColdArenaBytes() { return ArenaBytes(true); }

// Both arenas, since a program can put charge sites in either.
struct CodeImage
{
	std::vector<u8> block;
	std::vector<u8> cold;
	bool operator==(const CodeImage& o) const { return block == o.block && cold == o.cold; }
};

CodeImage CurrentImage()
{
	return CodeImage{BlockBytesAt(kProgramPc), ColdArenaBytes()};
}

using ProgramRunner = std::function<void(EeRecTestHarness&)>;

CodeImage CompileAt(int rate, const ProgramRunner& run)
{
	EmuConfig.Speedhacks.EECycleRate = static_cast<s8>(rate);
	EECycleRate::SyncToConfigured();
	ResetRecAndPageProtection();
	EeRecTestHarness h;
	run(h);
	return CurrentImage();
}

// Every charge immediate in a block, decoded out of the ADD/ADDS form. Address
// independent, unlike the byte image, so it can compare two compiles of the
// same program that landed at different host addresses.
std::vector<u32> ChargeImmediatesIn(const std::vector<u8>& bytes)
{
	std::vector<u32> out;
	for (size_t i = 0; i + 4 <= bytes.size(); i += 4)
	{
		u32 word = 0;
		std::memcpy(&word, bytes.data() + i, sizeof(word));
		if (!recEeChargeWordHasFormForTest(word, EeChargeForm::AddImm12))
			continue;
		const u32 imm12 = (word >> 10) & 0xfffu;
		out.push_back((word & 0x00400000u) ? (imm12 << 12) : imm12);
	}
	return out;
}

// Patching an already-compiled block to a new selector must produce exactly the
// bytes a fresh compile at that selector emits. A same-rate control runs first:
// if the program's emission is not byte-stable, the comparison means nothing.
void ExpectPatchMatchesFreshCompileAt(u32 pc, int from, int to, const ProgramRunner& run, const char* what)
{
	const auto image = [pc] { return CodeImage{BlockBytesAt(pc), ColdArenaBytes()}; };

	CompileAt(from, run);
	const CodeImage control_a = image();
	CompileAt(from, run);
	const CodeImage control_b = image();
	ASSERT_FALSE(control_a.block.empty()) << what << ": nothing was compiled at " << from;
	ASSERT_TRUE(control_a == control_b) << what << ": emission is not deterministic, so the rest proves nothing";

	CompileAt(to, run);
	const CodeImage fresh = image();
	ASSERT_FALSE(fresh.block.empty()) << what << ": nothing was compiled at " << to;

	CompileAt(from, run);
	ASSERT_TRUE(RepatchAsRecCpu(to)) << what << ": the patch pass refused (" << from << " -> " << to
	                                 << "): " << EECycleRate::GetLastTransitionPath().reason;
	const CodeImage patched = image();

	EXPECT_EQ(patched.block, fresh.block) << what << ": block bytes after patching " << from << " -> " << to;
	EXPECT_EQ(patched.cold, fresh.cold) << what << ": cold arena bytes after patching " << from << " -> " << to;
}

void ExpectPatchMatchesFreshCompile(int from, int to, const ProgramRunner& run, const char* what)
{
	ExpectPatchMatchesFreshCompileAt(kProgramPc, from, to, run, what);
}

void ExpectEmitterCovered(const char* what, EeChargeSite site, const ProgramRunner& run)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;
	CompileAt(0, run);
	EXPECT_GT(recEeGetChargeSiteCountFor(site), 0u)
		<< what << ": the program never reached this emitter, so the byte comparison is not coverage of it";
	ExpectPatchMatchesFreshCompile(0, -1, run, what);
	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}

// The common block tail (emitCycleUpdateAndEventCheck).
void RunLongBlock(EeRecTestHarness& h)
{
	h.SetGpr64(reg::a1, 0);
	h.LoadProgram(LongBlock());
	h.Run();
}

// SL-1 resident self-loop: the back-edge tail carries its own charge.
void RunResidentLoop(EeRecTestHarness& h)
{
	h.SetGpr64(reg::t0, 0);
	h.SetGpr64(reg::t1, 7);
	h.SetGpr64(reg::t2, 3);
	h.LoadProgramNoTerm({
		ADDU(reg::t0, reg::t0, reg::t2),
		ADDIU(reg::t1, reg::t1, -1),
		BNE(reg::t1, reg::zero, -3),
		NOP,
		ADDIU(reg::v0, reg::zero, 0x55),
		J(RecompilerTestEnvironment::kParkingPc), NOP,
	});
	h.Run();
}

// SL-03 superblock: the forward branch's taken arm is outlined into the cold
// arena and charges through the shared exit stub — the one MOVZ site.
void RunSuperblockTakenExit(EeRecTestHarness& h)
{
	h.SetGpr64(reg::t0, 1); // taken
	h.LoadProgramNoTerm({
		BNE(reg::t0, reg::zero, 2),
		NOP,
		ADDIU(reg::t1, reg::zero, 5),
		ADDIU(reg::t2, reg::zero, 7),
		J(RecompilerTestEnvironment::kParkingPc), NOP,
	});
	h.Run();
}

// A two-instruction block that ends without a branch, which takes the <= 6
// instruction short-tail path. The jump at the top compiles 0x10 first, so when
// the back-edge dispatches to 0x08 the scan stops at that compiled block.
void RunShortNonBranchTail(EeRecTestHarness& h)
{
	h.SetGpr64(reg::t0, 0);
	h.SetGpr64(reg::t1, 0);
	h.SetGpr64(reg::t2, 2);
	h.LoadProgramNoTerm({
		J(RecompilerTestEnvironment::kProgramPc + 0x10), NOP, // 0x00: over the landing pad
		ADDIU(reg::t1, reg::t1, 1), NOP,                      // 0x08: landing pad, entered from below
		ADDIU(reg::t2, reg::t2, -1),                          // 0x10:
		BGTZ(reg::t2, -4),                                    // 0x14: back to 0x08
		NOP,                                                  // 0x18: delay slot
		J(RecompilerTestEnvironment::kParkingPc), NOP,
	});
	h.Run();
}

// MFC0 $9 (Count), the absolute-cycle COP0 flush. The leading ADDIU reads a
// register on purpose: `addiu t0, zero, 1` emits nothing (t0 becomes a
// compile-time constant), which would put the charge Add on the block's entry
// point. Cop0EntryChargeTail() covers that shape; this is the mid-block case.
void RunCop0CountRead(EeRecTestHarness& h)
{
	h.EnableCop0();
	h.SetGpr64(reg::t0, 1);
	h.LoadProgram({
		ADDIU(reg::t0, reg::t0, 1),
		MFC0(reg::v0, 9),
		ADDIU(reg::t1, reg::t1, 2),
	});
	// JIT only: COP0 Count derives from cpuRegs.cycle, which the recompiler
	// advances per block and the interpreter per instruction, so a Count read
	// always differs between the engines.
	h.RunJitNoDiff();
}

// MFC0 $25 (performance counters) — the delta-only COP0 flush, which goes
// through the interpreter.
void RunCop0PerfCounterRead(EeRecTestHarness& h)
{
	h.EnableCop0();
	h.SetGpr64(reg::t0, 1);
	h.LoadProgram({
		ADDIU(reg::t0, reg::t0, 1),
		MFPS(reg::v0),
		ADDIU(reg::t1, reg::t1, 2),
	});
	h.Run();
}

// EI: an interpreter call that reads and writes no guest GPR, so it takes the pin-sparing
// seam (recBranchCallNoGprWrite, shared with ERET and a constant-call SYSCALL) and charges
// the block's cycles there. The ADDIU in front gives the block something to charge.
void RunEi(EeRecTestHarness& h)
{
	h.SetGpr64(reg::t0, 1);
	h.LoadProgram({
		ADDIU(reg::t0, reg::t0, 1),
		ee::EI,
		ADDIU(reg::t1, reg::t1, 2),
	});
	h.Run();
}

// A VU0 microprogram that stops. The E bit is what ends it, and an interlocked
// COP2 op after the kick waits for exactly that — without the E bit the wait
// never returns. The leading NOP pairs keep VPU_STAT busy past the kick so the
// sync seam takes the call path rather than the "already idle" skip.
void SeedTerminatingVu0Program(EeRecTestHarness& h)
{
	h.EnableVu0Capture();
	h.EnableCop1();
	h.SeedVu0Vi(REG_VPU_STAT, 0);

	u32 off = 0;
	for (int i = 0; i < 32; i++, off += 8)
		h.SeedVu0Microprogram(off, {vu::NopPair()});
	h.SeedVu0Microprogram(off, {vu::EBitNopPair(), vu::NopPair()});
}

void RunVcallms(EeRecTestHarness& h)
{
	SeedTerminatingVu0Program(h);
	h.LoadProgram({
		ADDIU(reg::t0, reg::zero, 1),
		ee::VCALLMS(0),
		ADDIU(reg::t1, reg::zero, 2),
	});
	h.Run();
}

// A VU0 kick followed by an interlocked COP2 read, so the analysis marks the
// read as needing a sync and the sync seam charges.
void RunCop2InterlockedSync(EeRecTestHarness& h)
{
	SeedTerminatingVu0Program(h);
	h.LoadProgram({
		ee::VCALLMS(0),
		ee::CFC2_I(reg::v0, 1),
		ADDIU(reg::t0, reg::zero, 1),
	});
	h.Run();
}

// The same, non-interlocked.
void RunCop2PlainSync(EeRecTestHarness& h)
{
	SeedTerminatingVu0Program(h);
	h.LoadProgram({
		ee::VCALLMS(0),
		ee::CFC2(reg::v0, 1),
		ADDIU(reg::t0, reg::zero, 1),
	});
	h.Run();
}

// Sixteen small blocks, each ending in a jump to the next, followed by a tail
// block. The pass refuses outright when the unpatchable share of the live blocks
// is over its cap, so testing "one block is invalidated and the rest are
// patched" needs a body of patchable blocks. Each chain block clears the
// 40-cycle no-scaling floor, so its charge moves with the selector.
constexpr int kChainBlocks = 16;
constexpr u32 kChainBlockWords = 8; // 6 x ADDIU, J, delay slot
constexpr u32 kTailBlockPc = RecompilerTestEnvironment::kProgramPc + kChainBlocks * kChainBlockWords * 4;

std::vector<u32> ChainThen(const std::vector<u32>& tail)
{
	std::vector<u32> prog;
	for (int k = 0; k < kChainBlocks; k++)
	{
		for (u32 i = 0; i < kChainBlockWords - 2; i++)
			prog.push_back(ADDIU(reg::t0, reg::t0, 1));
		prog.push_back(J(RecompilerTestEnvironment::kProgramPc + (k + 1) * kChainBlockWords * 4));
		prog.push_back(NOP);
	}
	prog.insert(prog.end(), tail.begin(), tail.end());
	return prog;
}

// Tail: parallel divides, deep enough that the charge outgrows ADD's imm12 two
// steps below the configured rate but NOT at the configured rate itself — which
// is what makes the emitted shape widen across the transition and gives the
// test something to see. PDIVW costs 296 raw units (Cycles::MMI_Div), doubled
// because CP0 Config bit 18 is clear under the harness, so 40 of them are
// 23680 raw: 2960 at rate 0, inside imm12, and 5180 at rate -2, outside it.
// (DeepDivideBlock above uses 128 and is over the line at BOTH ends, which is
// all its own test needs.)
std::vector<u32> DeepDivideTail()
{
	std::vector<u32> tail;
	for (int i = 0; i < 40; i++)
		tail.push_back(ee::PDIVW(reg::a1, reg::a2));
	tail.push_back(J(RecompilerTestEnvironment::kParkingPc));
	tail.push_back(NOP);
	return tail;
}

// Tail: a COP0 Count read behind a constant materialisation that emits no code,
// so the Count read's charge lands on the block's entry point, the one word
// Remove() overwrites. The charge fits imm12 at every selector, so a live block
// is patched in place; once the block is removed its records are orphaned.
std::vector<u32> Cop0EntryChargeTail()
{
	return {
		ADDIU(reg::t1, reg::zero, 1), // const-propagated: emits nothing
		MFC0(reg::v0, 9),             // so this charge is the block's first word
		ADDIU(reg::t2, reg::t2, 2),
		J(RecompilerTestEnvironment::kParkingPc),
		NOP,
	};
}

void RunChainThenDeepDivide(EeRecTestHarness& h)
{
	h.SetGpr64(reg::a1, 0x0001000200030004ull);
	h.SetGpr64(reg::a2, 0x0000000500000007ull);
	h.LoadProgramNoTerm(ChainThen(DeepDivideTail()));
	// JIT only: the test is about emitted code, not PDIVW's result.
	h.RunJitNoDiff();
}

void RunChainThenCop0EntryCharge(EeRecTestHarness& h)
{
	h.EnableCop0();
	h.LoadProgramNoTerm(ChainThen(Cop0EntryChargeTail()));
	// JIT only: a Count read differs between the engines, as in RunCop0CountRead.
	h.RunJitNoDiff();
}

void RunDeepDivideAlone(EeRecTestHarness& h)
{
	h.SetGpr64(reg::a1, 0x0001000200030004ull);
	h.SetGpr64(reg::a2, 0x0000000500000007ull);
	h.LoadProgram(DeepDivideBlock());
	h.RunJitNoDiff();
}

// Turns the WaitLoop speedhack on so a spin loop's tail takes the fast-forward
// shape instead of the ordinary event check.
struct ScopedWaitLoop
{
	bool prev;
	explicit ScopedWaitLoop(bool on) : prev(EmuConfig.Speedhacks.WaitLoop) { EmuConfig.Speedhacks.WaitLoop = on; }
	~ScopedWaitLoop() { EmuConfig.Speedhacks.WaitLoop = prev; }
};

} // namespace

// The interpreter and the recompiler each hold their own copy of the formula.
// The charge itself always agrees, or a game would run at one speed under the
// JIT and another under the interpreter.
TEST(EeRecCycleRate, InterpreterAndRecompilerChargeTheSame)
{
	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		for (const CycleRateCase& c : kCases)
		{
			u32 rec_rem = 0, int_rem = 0;
			const u32 rec_charge = recEeScaleBlockCyclesForTest(c.raw, &rec_rem);
			const u32 int_charge = intScaleBlockCyclesForTest(c.raw, &int_rem);
			EXPECT_EQ(rec_charge, int_charge)
				<< "selector " << selector << ", raw " << c.raw << ": charge";
			(void)rec_rem;
			(void)int_rem;
		}
	}
}

// The retained remainder does NOT always agree; this pins where it doesn't.
//
// Both recompilers (arm64 scaleblockcycles_clear, and x86's in
// ix86-32/iR5900.cpp) use the wide +2/+3 mask only for a block that was
// eligible for scaling:
//
//     if (!lowcycles && cyclerate > 1)   s_nBlockCycles &= (1 << (cyclerate+2)) - 1;
//     else                               s_nBlockCycles &= 0x7;
//
// The EE interpreter (intUpdateCPUCycles, shared upstream code) has no
// `!lowcycles` term, so for a block of 40 raw cycles or fewer at selector +2 or
// +3 it carries 4 or 5 bits where the recompilers carry 3, and the engines
// drift apart by a fraction of a cycle per short block.
//
// This is an upstream divergence, confined to the two overclock selectors. It is
// recorded rather than fixed because either change alters guest timing. If it
// is fixed, this test will fail.
TEST(EeRecCycleRate, InterpreterCarriesAWiderRemainderOnShortOverclockedBlocks)
{
	const u32 raw = 25; // <= 40, so no scaling; low bits set past bit 2

	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		u32 rec_rem = 0, int_rem = 0;
		recEeScaleBlockCyclesForTest(raw, &rec_rem);
		intScaleBlockCyclesForTest(raw, &int_rem);

		EXPECT_EQ(rec_rem, raw & 0x7u) << "selector " << selector;
		if (selector > 1)
		{
			EXPECT_EQ(int_rem, raw & ((1u << (selector + 2)) - 1u))
				<< "selector " << selector << ": interpreter no longer carries the wide remainder";
			EXPECT_NE(int_rem, rec_rem) << "selector " << selector;
		}
		else
		{
			EXPECT_EQ(int_rem, rec_rem) << "selector " << selector;
		}
	}
}

TEST(EeRecCycleRate, CharacterizesTheSevenSelectors)
{
	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		const int idx = selector + 3;
		for (const CycleRateCase& c : kCases)
		{
			u32 rem = 0;
			const u32 charge = recEeScaleBlockCyclesForTest(c.raw, &rem);
			EXPECT_EQ(charge, c.by_selector[idx].charge)
				<< "selector " << selector << ", raw " << c.raw << ": charge";
			EXPECT_EQ(rem, c.by_selector[idx].remainder)
				<< "selector " << selector << ", raw " << c.raw << ": remainder";
			EXPECT_GE(charge, 1u) << "selector " << selector << ", raw " << c.raw;
		}
	}
}

// The remainder is what carries the fractional part of a block's cost into the
// next block, so its width is part of the timing model, not an implementation
// detail: 3 bits everywhere except +2 (4 bits) and +3 (5 bits), and 3 bits for
// any block short enough to skip scaling entirely.
TEST(EeRecCycleRate, RemainderMaskWidthPerSelector)
{
	const u32 all_ones = 0x3F; // 6 bits set, > 40 so scaling is eligible
	const u32 expected_mask[7] = {0x7, 0x7, 0x7, 0x7, 0x7, 0xF, 0x1F};

	for (int selector = -3; selector <= 3; selector++)
	{
		ScopedCycleRate rate(selector);
		u32 rem = 0;
		recEeScaleBlockCyclesForTest(all_ones, &rem);
		EXPECT_EQ(rem, all_ones & expected_mask[selector + 3]) << "selector " << selector;

		// A block at or under the 40-cycle floor keeps the narrow mask whatever
		// the selector says.
		u32 low_rem = 0;
		recEeScaleBlockCyclesForTest(39, &low_rem);
		EXPECT_EQ(low_rem, 39u & 0x7u) << "selector " << selector << " (low-cycle block)";
	}
}

// The emitted charge follows the effective selector, not the configured one.
TEST(EeRecCycleRate, EmittedCodeFollowsTheEffectiveSelector)
{
	const s8 saved_configured = EmuConfig.Speedhacks.EECycleRate;

	// Discard the first compile: the program's page is still write-protected, so
	// that block has no manual-protection prologue. Loading the program a second
	// time flips the page to ProtMode_Manual, and every later compile carries the
	// page-compare prologue, so only warmed compiles are comparable.
	CaptureBlockBytes(0, 0);
	const std::vector<u8> rate0_a = CaptureBlockBytes(0, 0);
	const std::vector<u8> rate0_b = CaptureBlockBytes(0, 0);
	const std::vector<u8> ratem1  = CaptureBlockBytes(-1, -1);
	const std::vector<u8> ratem1b = CaptureBlockBytes(-1, -1);
	const std::vector<u8> effm1   = CaptureBlockBytes(0, -1);
	const std::vector<u8> rate0_c = CaptureBlockBytes(0, 0);
	EXPECT_EQ(ratem1, ratem1b);
	EXPECT_EQ(rate0_a, rate0_c);

	EmuConfig.Speedhacks.EECycleRate = saved_configured;
	EECycleRate::SyncToConfigured();

	ASSERT_FALSE(rate0_a.empty());

	ASSERT_EQ(rate0_a, rate0_b) << "block emission is not deterministic; the rest of this test proves nothing";

	ASSERT_NE(rate0_a, ratem1);

	EXPECT_EQ(effm1, ratem1);
}

// Bounds. The governor may go two steps down from the user's baseline and no
// further, and may never go up past it.
TEST(EeRecCycleRate, ApplyEffectiveHonoursTheBounds)
{
	ScopedCycleRate rate(0);

	R5900cpu* saved_cpu = Cpu;
	BaseVUmicroCPU* saved_vu0 = CpuVU0;
	Cpu = &intCpu;
	CpuVU0 = &CpuIntVU0;

	EXPECT_EQ(EECycleRate::GetConfigured(), 0);
	EXPECT_EQ(EECycleRate::GetFloor(), -2);

	const u32 before = EECycleRate::GetTransitionCount();

	EXPECT_FALSE(EECycleRate::ApplyEffective(1, "test"));
	EXPECT_EQ(EECycleRate::GetEffective(), 0);

	EXPECT_FALSE(EECycleRate::ApplyEffective(-3, "test"));
	EXPECT_EQ(EECycleRate::GetEffective(), 0);

	EXPECT_EQ(EECycleRate::GetTransitionCount(), before) << "a rejected request is not a transition";

	EXPECT_TRUE(EECycleRate::ApplyEffective(-2, "test"));
	EXPECT_EQ(EECycleRate::GetEffective(), -2);
	EXPECT_EQ(EECycleRate::GetTransitionCount(), before + 1);

	// Asking for what is already set is a no-op, not a transition.
	EXPECT_TRUE(EECycleRate::ApplyEffective(-2, "test"));
	EXPECT_EQ(EECycleRate::GetTransitionCount(), before + 1);

	EXPECT_TRUE(EECycleRate::ApplyEffective(0, "test"));
	EXPECT_EQ(EECycleRate::GetEffective(), 0);
	EXPECT_EQ(EECycleRate::GetTransitionCount(), before + 2);

	Cpu = saved_cpu;
	CpuVU0 = saved_vu0;
}

// A transition patches the EE in place and rebuilds microVU0 only. An EE reset
// here would mean the patch pass refused and the transition fell back to a full
// LUT rewrite and re-JIT.
//
// microVU0 is reset because mVUtestCycles scales its compile-time cycle count by
// the selector and gates the emit on the scaled value being 1, so the emitted
// shape changes with the selector and no immediate rewrite can add an
// instruction that was never emitted.
//
// The IOP recompiler, microVU1 and the VIF unpack dynarec bake in no EE
// selector; under MTVU, resetting VU1 would also reach across a thread.
TEST(EeRecCycleRate, TransitionPatchesTheEeAndResetsOnlyVu0)
{
	ScopedCycleRate rate(0);

	R5900cpu* saved_cpu = Cpu;
	BaseVUmicroCPU* saved_vu0 = CpuVU0;
	Cpu = &recCpu;
	CpuVU0 = &CpuMicroVU0;

	// Something has to be compiled, or the pass has no sites to repair and the
	// test would pass on an empty cache.
	ResetRecAndPageProtection();
	{
		EeRecTestHarness h;
		RunLongBlock(h);
	}
	Cpu = &recCpu; // Run() leaves it on the interpreter

	const EECycleRate::ResetGenerations before = EECycleRate::GetResetGenerations();
	ASSERT_TRUE(EECycleRate::ApplyEffective(-1, "test"));
	const EECycleRate::ResetGenerations after = EECycleRate::GetResetGenerations();

	const EECycleRate::TransitionPath path = EECycleRate::GetLastTransitionPath();
	EXPECT_TRUE(path.patched) << "the pass refused: " << path.reason;
	EXPECT_GT(path.sites, 0u) << "nothing was patched, so nothing was proved";

	EXPECT_EQ(after.ee, before.ee) << "the EE recompiler was reset; the patch pass did not take";
	EXPECT_GT(after.vu0, before.vu0);
	EXPECT_EQ(after.iop, before.iop);
	EXPECT_EQ(after.vu1, before.vu1);
	EXPECT_EQ(after.vif, before.vif);

	EXPECT_TRUE(EECycleRate::ApplyEffective(0, "test"));

	Cpu = saved_cpu;
	CpuVU0 = saved_vu0;
}

// Counts the charge sites that would stop fitting the one instruction they were
// emitted in if the governor moved the selector, and the blocks holding one.
//
// An ordinary block is nowhere near the boundary: 32 ADDIUs cost 9 raw units
// each, so the charge is double digits at every selector in the window and the
// same ADD encoding covers all of them.
TEST(EeRecCycleRate, AnOrdinaryBlockKeepsItsChargeEncodingAcrossTheWindow)
{
	ScopedCycleRate rate(0);
	ASSERT_EQ(EECycleRate::GetFloor(), -2);

	recCpu.Reset();

	EeRecTestHarness h;
	h.SetGpr64(reg::a1, 0);
	h.LoadProgram(LongBlock());
	h.Run();

	EXPECT_GT(recEeGetChargeSiteCount(), 0u) << "nothing was compiled; the rest proves nothing";
	EXPECT_EQ(recEeGetFormChangeSiteCount(), 0u);
	EXPECT_EQ(recEeGetFormChangeBlockCount(), 0u);
}

// A block of 128 parallel divides outgrows the single-instruction form two steps
// down, so it cannot be repaired by rewriting the immediate in place; a fresh
// compile would emit a longer sequence there.
TEST(EeRecCycleRate, ADeepDivideBlockOutgrowsItsChargeEncodingTwoStepsDown)
{
	ScopedCycleRate rate(0);
	ASSERT_EQ(EECycleRate::GetFloor(), -2);

	// The boundary itself, taken from the production scaler. IsImmAddSub accepts
	// a 12-bit immediate, or a 12-bit one shifted left by 12 with the low bits
	// clear, so 4096 is still one instruction and 4097 is not. At rate -2 the
	// charge is 7*raw/32, which reaches 4097 at raw 18730.
	const auto fits_add_imm12 = [](u32 c) {
		return (c <= 0xfffu) || (((c >> 12) <= 0xfffu) && ((c & 0xfffu) == 0));
	};
	EXPECT_TRUE(fits_add_imm12(recEeScaleBlockCyclesForTest(18730, nullptr)))
		<< "at the configured rate this block is nowhere near the boundary";
	{
		ScopedCycleRate floor_rate(-2);
		EXPECT_EQ(recEeScaleBlockCyclesForTest(18729, nullptr), 4096u);
		EXPECT_TRUE(fits_add_imm12(recEeScaleBlockCyclesForTest(18729, nullptr)));
		EXPECT_EQ(recEeScaleBlockCyclesForTest(18730, nullptr), 4097u);
		EXPECT_FALSE(fits_add_imm12(recEeScaleBlockCyclesForTest(18730, nullptr)));
	}

	recCpu.Reset();

	EeRecTestHarness h;
	h.SetGpr64(reg::a1, 0x0001000200030004ull);
	h.SetGpr64(reg::a2, 0x0000000500000007ull);
	h.LoadProgram(DeepDivideBlock());
	// JIT only: the test is about emitted code, not PDIVW's result.
	h.RunJitNoDiff();

	EXPECT_GE(recEeGetFormChangeSiteCount(), 1u);
	EXPECT_GE(recEeGetFormChangeBlockCount(), 1u);
}

// The side table the immediate-patching transition walks. Every charge site
// must emit exactly one record: a site with no record would keep charging the
// old rate after a transition claimed to have repaired everything. The
// recompiler asserts the pairing in both directions at emit time; this pins the
// totals from outside, where a missed emitter shows up as a count mismatch.
TEST(EeRecCycleRate, EveryChargeSiteLeavesExactlyOneRecord)
{
	ScopedCycleRate rate(0);

	recCpu.Reset();
	EXPECT_EQ(recEeGetChargeSiteCount(), 0u) << "the reset did not clear the site count";
	EXPECT_EQ(recEeGetChargeRecordCount(), 0u) << "the reset did not clear the side table";

	// A handful of different block shapes, so the totals cover more than one
	// emitter: a long straight-line block, a short one (the <= 6 instruction
	// tail is a different site), and a divide-heavy one.
	{
		EeRecTestHarness h;
		h.SetGpr64(reg::a1, 0);
		h.LoadProgram(LongBlock());
		h.Run();
	}
	EXPECT_GT(recEeGetChargeSiteCount(), 0u) << "nothing was compiled; the rest proves nothing";
	EXPECT_EQ(recEeGetChargeRecordCount(), recEeGetChargeSiteCount());

	{
		std::vector<u32> shortprog;
		for (int i = 0; i < 4; i++)
			shortprog.push_back(ADDIU(reg::a1, reg::a1, 1));
		EeRecTestHarness h;
		h.SetGpr64(reg::a1, 0);
		h.LoadProgram(shortprog);
		h.Run();
	}
	EXPECT_EQ(recEeGetChargeRecordCount(), recEeGetChargeSiteCount());

	{
		EeRecTestHarness h;
		h.SetGpr64(reg::a1, 0x0001000200030004ull);
		h.SetGpr64(reg::a2, 0x0000000500000007ull);
		h.LoadProgram(DeepDivideBlock());
		h.RunJitNoDiff();
	}
	EXPECT_EQ(recEeGetChargeRecordCount(), recEeGetChargeSiteCount());

	// The reset zeroes both: the records point into an arena that is about to
	// rewind.
	recCpu.Reset();
	EXPECT_EQ(recEeGetChargeSiteCount(), 0u);
	EXPECT_EQ(recEeGetChargeRecordCount(), 0u);
}

// A block compiled at one selector and then patched to another must be
// byte-for-byte what a fresh compile at the second selector emits, not merely
// charge the same: anything else is a second code shape nobody tests.
//
// This holds because the charge is a pure function of the raw block-cycle count
// and the selector, and the remainder the scaler retains between sites in one
// block is & 0x7 for every selector from -3 through +1. Only the immediate
// moves.
TEST(EeRecCycleRate, EmittedCodeAfterPatchingMatchesAFreshCompile)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;

	ExpectPatchMatchesFreshCompile(0, -1, RunLongBlock, "0 -> -1");
	ExpectPatchMatchesFreshCompile(0, -2, RunLongBlock, "0 -> -2");

	// The restore direction. Configured -1 so the classifier sees a window
	// containing both ends; the emitted bytes do not depend on the configured
	// selector (see EmittedCodeFollowsTheEffectiveSelector).
	ExpectPatchMatchesFreshCompile(-1, 0, RunLongBlock, "-1 -> 0");
	ExpectPatchMatchesFreshCompile(-1, -3, RunLongBlock, "-1 -> -3");

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}

// Two steps in a row land where one step would have. Each pass scales the
// recorded raw and never rewrites it, so nothing accumulates; a design that
// stored the last charge instead would drift here and nowhere else.
TEST(EeRecCycleRate, PatchingTwiceLandsWherePatchingOnceWould)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;

	const CodeImage fresh_m1 = CompileAt(-1, RunLongBlock);
	ASSERT_FALSE(fresh_m1.block.empty());

	CompileAt(0, RunLongBlock);
	ASSERT_TRUE(RepatchAsRecCpu(-2));
	ASSERT_TRUE(RepatchAsRecCpu(-1));

	EXPECT_EQ(CurrentImage().block, fresh_m1.block);
	EXPECT_EQ(CurrentImage().cold, fresh_m1.cold);

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}

// Site coverage. LongBlock exercises the common block tail; these are the other
// emitters, each with a program that reaches it and a per-emitter counter that
// proves it did, so an emitter that becomes unreachable fails loudly. One test
// per emitter, so a hang or a failure names the site.
TEST(EeRecCycleRate, ChargeSiteBlockTailSurvivesAPatch)
{
	ExpectEmitterCovered("block tail", EeChargeSite::BlockTail, RunLongBlock);
}

TEST(EeRecCycleRate, ChargeSiteBranchCallNoGprWriteSurvivesAPatch)
{
	ExpectEmitterCovered("EI via recBranchCallNoGprWrite", EeChargeSite::BranchCallNoGprWrite, RunEi);
}

TEST(EeRecCycleRate, ChargeSiteLoopBackedgeSurvivesAPatch)
{
	ExpectEmitterCovered("resident self-loop back-edge", EeChargeSite::LoopBackedge, RunResidentLoop);
}

TEST(EeRecCycleRate, ChargeSiteSuperblockExitSurvivesAPatch)
{
	ExpectEmitterCovered("superblock taken side exit (MOVZ)", EeChargeSite::SuperblockExit,
		RunSuperblockTakenExit);
}

TEST(EeRecCycleRate, ChargeSiteShortNonBranchTailSurvivesAPatch)
{
	ExpectEmitterCovered("short non-branch block tail", EeChargeSite::ShortBlockTail, RunShortNonBranchTail);
}

TEST(EeRecCycleRate, ChargeSiteCop0CountReadSurvivesAPatch)
{
	ExpectEmitterCovered("COP0 Count read", EeChargeSite::Cop0FlushCyclesAbs, RunCop0CountRead);
}

TEST(EeRecCycleRate, ChargeSiteCop0PerfCounterReadSurvivesAPatch)
{
	ExpectEmitterCovered("COP0 performance-counter read", EeChargeSite::Cop0FlushCycles,
		RunCop0PerfCounterRead);
}

TEST(EeRecCycleRate, ChargeSiteVcallmsSurvivesAPatch)
{
	ExpectEmitterCovered("VCALLMS", EeChargeSite::Vcallms, RunVcallms);
}

TEST(EeRecCycleRate, ChargeSiteCop2InterlockedSyncSurvivesAPatch)
{
	ExpectEmitterCovered("COP2 interlocked sync", EeChargeSite::Cop2SyncInterlock, RunCop2InterlockedSync);
}

TEST(EeRecCycleRate, ChargeSiteCop2PlainSyncSurvivesAPatch)
{
	ExpectEmitterCovered("COP2 non-interlocked sync", EeChargeSite::Cop2SyncPlain, RunCop2PlainSync);
}

// The WaitLoop fast-forward tail, which only exists with the speedhack on.
TEST(EeRecCycleRate, WaitLoopFastForwardTailSurvivesAPatch)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;
	ScopedWaitLoop waitloop(true);

	// A spin branching to its own startpc, so s_branchTo == startpc and the
	// block qualifies as a wait loop. The branch is not taken at run time (t0 is
	// nonzero): a taken wait loop never leaves, since the fast-forward tail jumps
	// to DispatcherEvent and straight back to the loop top. The taken arm is
	// emitted whether or not it runs.
	const ProgramRunner run = [](EeRecTestHarness& h) {
		h.SetGpr64(reg::t0, 1);
		h.LoadProgramNoTerm({
			BEQ(reg::t0, reg::zero, -1),
			NOP,                         // delay slot
			ADDIU(reg::v0, reg::zero, 0x42),
			J(RecompilerTestEnvironment::kParkingPc), NOP,
		});
		h.Run();
	};

	CompileAt(0, run);
	EXPECT_GT(recEeGetChargeSiteCountFor(EeChargeSite::WaitLoopFastFwd), 0u)
		<< "the WaitLoop fast-forward tail was not emitted; the comparison below does not cover it";
	ExpectPatchMatchesFreshCompile(0, -1, run, "WaitLoop fast-forward tail");

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}

// The hand encoder against vixl, at the boundaries where the encoding changes
// shape. The byte-identity tests above are the real proof; this one localises a
// failure to the bit layout.
TEST(EeRecCycleRate, HandEncodedChargeImmediatesMatchVixl)
{
	alignas(4) static u8 buffer[64];

	const auto vixl_word = [&](int form, u32 charge) -> u32 {
		vixl::aarch64::MacroAssembler masm(buffer, sizeof(buffer));
		switch (form)
		{
			case 0: masm.Add(RECCYCLE, RECCYCLE, charge); break;
			case 1: masm.Adds(RECCYCLE, RECCYCLE, charge); break;
			default: masm.Mov(RWARG2, charge); break;
		}
		masm.FinalizeCode();
		EXPECT_EQ(masm.GetSizeOfCodeGenerated(), 4u)
			<< "vixl needed more than one instruction for charge " << charge
			<< "; the test case is outside the single-instruction forms";
		u32 word = 0;
		std::memcpy(&word, buffer, sizeof(word));
		return word;
	};

	// IsImmAddSub: a 12-bit immediate, or a 12-bit one shifted left by 12 with
	// the low bits clear. 0xfff000 is the largest charge that encodes at all.
	for (const u32 charge : {1u, 2u, 0xfffu, 0x1000u, 0x2000u, 0xfff000u})
	{
		for (int form = 0; form < 2; form++)
		{
			const u32 base = vixl_word(form, 1); // a valid word of this shape
			const u32 want = vixl_word(form, charge);
			EXPECT_TRUE(recEeChargeWordHasFormForTest(base, EeChargeForm::AddImm12))
				<< "form " << form;
			EXPECT_EQ(recEeEncodeChargeWordForTest(base, EeChargeForm::AddImm12, charge), want)
				<< "form " << form << ", charge " << charge;
		}
	}

	for (const u32 charge : {1u, 2u, 0xfffu, 0x1000u, 0x1234u, 0xffffu})
	{
		const u32 base = vixl_word(2, 1);
		const u32 want = vixl_word(2, charge);
		EXPECT_TRUE(recEeChargeWordHasFormForTest(base, EeChargeForm::MovzImm16));
		EXPECT_EQ(recEeEncodeChargeWordForTest(base, EeChargeForm::MovzImm16, charge), want)
			<< "MOVZ charge " << charge;
	}

	// The decoder must reject the shapes a patch must never land on: the
	// register-form ADD that a materialise-then-add ends with, and a MOVK.
	{
		vixl::aarch64::MacroAssembler masm(buffer, sizeof(buffer));
		masm.add(RECCYCLE, RECCYCLE, vixl::aarch64::x9);
		masm.FinalizeCode();
		u32 word = 0;
		std::memcpy(&word, buffer, sizeof(word));
		EXPECT_FALSE(recEeChargeWordHasFormForTest(word, EeChargeForm::AddImm12))
			<< "the register form of ADD passed the immediate-form decoder";
	}
	{
		vixl::aarch64::MacroAssembler masm(buffer, sizeof(buffer));
		masm.movk(RWARG2, 0x1234);
		masm.FinalizeCode();
		u32 word = 0;
		std::memcpy(&word, buffer, sizeof(word));
		EXPECT_FALSE(recEeChargeWordHasFormForTest(word, EeChargeForm::MovzImm16))
			<< "MOVK passed the MOVZ decoder";
	}
}

// A charge on a block's entry point is patched in place like any other.
// Arm64BaseBlocks::Remove stamps its redirect stub over that word, but only once
// the block is dead; while it is alive the word is the ADD the record describes.
// The pass reads every word before it writes, so it can never land on a stub.
//
// The block is the constant-propagation shape: `addiu t1, zero, 1` emits no
// code, so the COP0 Count read behind it puts the charge Add at the block's
// first word.
TEST(EeRecCycleRate, AChargeAtABlockEntryIsPatchedInPlace)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;

	CompileAt(0, RunChainThenCop0EntryCharge);
	ASSERT_GT(recEeGetChargeSiteCountFor(EeChargeSite::Cop0FlushCyclesAbs), 0u)
		<< "the Count read was not compiled, so this program does not cover the entry case";
	// Not a form change — the charge is tiny at every selector in the window.
	ASSERT_EQ(recEeGetFormChangeBlockCount(), 0u);

	ExpectPatchMatchesFreshCompileAt(kTailBlockPc, 0, -1, RunChainThenCop0EntryCharge,
		"a charge on the block entry point");

	const EECycleRate::TransitionPath path = EECycleRate::GetLastTransitionPath();
	EXPECT_TRUE(path.patched);
	EXPECT_EQ(path.blocks_invalidated, 0u) << "an entry charge is repairable; nothing should have been thrown away";
	EXPECT_EQ(path.blocks_unpatchable, 0u);
	EXPECT_EQ(path.sites_orphaned, 0u) << "every block is live, so no record can be orphaned";

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}

// Once that block is removed, its entry word is the redirect stub and its
// records point at code nothing executes. The pass recognises this (that word is
// the only one anything but the pass writes), steps over the record, and leaves
// the stub alone. Treating the failed decode as the recompiler contradicting
// itself would refuse every transition after the first SMC clear of a block
// shaped like this.
TEST(EeRecCycleRate, AnOrphanedEntryChargeIsSteppedOverAndItsStubLeftAlone)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;

	CompileAt(0, RunChainThenCop0EntryCharge);

	uptr entry = 0, lut = 0;
	u32 size = 0;
	ASSERT_TRUE(recEeBlockHostInfo(kTailBlockPc, &entry, &size, &lut));

	const auto word_at = [](uptr addr) {
		u32 w = 0;
		std::memcpy(&w, reinterpret_cast<const void*>(addr), sizeof(w));
		return w;
	};

	const u32 charge_word = word_at(entry);
	ASSERT_TRUE(recEeChargeWordHasFormForTest(charge_word, EeChargeForm::AddImm12))
		<< "the block's first word is not the charge, so this test is not about what it says it is";

	// Remove it as the SMC path does; Remove() stamps the redirect stub over the
	// entry.
	recCpu.Clear(kTailBlockPc, 8);
	const u32 stub_word = word_at(entry);
	ASSERT_NE(stub_word, charge_word) << "Clear() did not stamp a redirect stub over the entry";
	ASSERT_FALSE(recEeChargeWordHasFormForTest(stub_word, EeChargeForm::AddImm12));

	ASSERT_TRUE(RepatchAsRecCpu(-1)) << "the pass refused: " << EECycleRate::GetLastTransitionPath().reason;

	const EECycleRate::TransitionPath path = EECycleRate::GetLastTransitionPath();
	EXPECT_EQ(path.sites_orphaned, 1u) << "exactly the removed block's one entry record is orphaned";
	EXPECT_EQ(path.blocks_invalidated, 0u);
	EXPECT_GT(path.sites, 0u) << "the surviving chain blocks should still have been patched";
	EXPECT_EQ(word_at(entry), stub_word) << "the pass wrote over the redirect stub";

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}

// The one reason a block cannot be repaired: its charge outgrows ADD's 12-bit
// immediate two steps down, so a fresh compile would emit a
// materialise-then-add of two to five instructions where one sits now. There is
// no room on the page for that, so the block is thrown away and comes back in
// the wider shape on its next dispatch.
TEST(EeRecCycleRate, ABlockWhoseChargeOutgrowsItsFormIsInvalidated)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;
	R5900cpu* saved_cpu = Cpu;
	BaseVUmicroCPU* saved_vu0 = CpuVU0;

	CompileAt(-2, RunChainThenDeepDivide);
	const std::vector<u32> want = ChargeImmediatesIn(BlockBytesAt(kTailBlockPc));

	CompileAt(0, RunChainThenDeepDivide);
	ASSERT_GE(recEeGetFormChangeBlockCount(), 1u) << "the deep block was not classified as unpatchable";
	const std::vector<u32> at_rate_0 = ChargeImmediatesIn(BlockBytesAt(kTailBlockPc));
	// The charge is a single ADD immediate at the configured rate; two steps down
	// vixl materialises it into a register instead, so the immediate found at
	// rate 0 is absent from the -2 compile.
	ASSERT_NE(at_rate_0, want) << "the two selectors emit the same shape, so nothing below can fail";

	uptr before_fnptr = 0, before_lut = 0;
	u32 before_size = 0;
	ASSERT_TRUE(recEeBlockHostInfo(kTailBlockPc, &before_fnptr, &before_size, &before_lut));

	Cpu = &recCpu;
	CpuVU0 = &CpuMicroVU0;
	const EECycleRate::ResetGenerations gen_before = EECycleRate::GetResetGenerations();
	ASSERT_TRUE(EECycleRate::ApplyEffective(-2, "test"));
	const EECycleRate::ResetGenerations gen_after = EECycleRate::GetResetGenerations();
	const EECycleRate::TransitionPath path = EECycleRate::GetLastTransitionPath();

	EXPECT_TRUE(path.patched) << "the pass refused: " << path.reason;
	EXPECT_EQ(path.blocks_invalidated, 1u);
	EXPECT_GT(path.sites, 0u) << "the chain blocks should still have been patched";
	EXPECT_EQ(gen_after.ee, gen_before.ee) << "the EE recompiler was reset";

	{
		EeRecTestHarness h;
		h.SetGpr64(reg::a1, 0x0001000200030004ull);
		h.SetGpr64(reg::a2, 0x0000000500000007ull);
		h.LoadProgramNoTerm(ChainThen(DeepDivideTail()));
		h.RunJitNoDiff(EeRecTestHarness::RunMode::PreserveCache);
	}

	uptr after_fnptr = 0, after_lut = 0;
	u32 after_size = 0;
	ASSERT_TRUE(recEeBlockHostInfo(kTailBlockPc, &after_fnptr, &after_size, &after_lut));
	EXPECT_NE(after_fnptr, before_fnptr) << "the block did not recompile";
	// Host SIZE is not usable as the comparison here: a block recompiled at a
	// different arena address can need a far-call veneer where the reference
	// compile needed none, which moves the size by more than the charge does.
	EXPECT_EQ(ChargeImmediatesIn(BlockBytesAt(kTailBlockPc)), want)
		<< "the block did not come back in the shape the new rate compiles to";

	Cpu = saved_cpu;
	CpuVU0 = saved_vu0;
	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}

// Past the cap the pass refuses outright and touches nothing. A deep-divide
// program on its own has almost no other blocks, so its single unpatchable block
// is well over the 10% share and the transition falls back to the reset.
TEST(EeRecCycleRate, TooManyUnpatchableBlocksRefusesAndWritesNothing)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;

	CompileAt(0, RunDeepDivideAlone);
	ASSERT_GE(recEeGetFormChangeBlockCount(), 1u) << "the deep block was not classified as unpatchable";

	const std::vector<u8> hot_before = ArenaBytes(false);
	const std::vector<u8> cold_before = ArenaBytes(true);
	ASSERT_FALSE(hot_before.empty());

	EXPECT_FALSE(RepatchAsRecCpu(-2)) << "the pass took a transition it should have refused";

	EXPECT_EQ(ArenaBytes(false), hot_before) << "a refused pass wrote into the hot arena";
	EXPECT_EQ(ArenaBytes(true), cold_before) << "a refused pass wrote into the cold arena";

	const EECycleRate::TransitionPath path = EECycleRate::GetLastTransitionPath();
	EXPECT_FALSE(path.patched);
	EXPECT_EQ(path.sites, 0u);
	EXPECT_EQ(path.blocks_invalidated, 0u);
	EXPECT_GE(path.blocks_unpatchable, 1u);
	EXPECT_STRNE(path.reason, "");

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}

// Configured +2 and +3 widen the remainder the scaler retains between sites in
// one block, so the raw a later site saw differs between selectors in the window
// and the recorded raws are only right for the selector they were taken at. The
// pass refuses rather than patch them wrongly.
TEST(EeRecCycleRate, PatchingIsRefusedAboveConfiguredPlusOne)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;

	CompileAt(2, RunLongBlock);
	EXPECT_FALSE(RepatchAsRecCpu(1)) << "the pass ran with the wide remainder mask in play";

	CompileAt(1, RunLongBlock);
	EXPECT_TRUE(RepatchAsRecCpu(0)) << "configured +1 is inside the narrow-remainder range and must patch";

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();
}
