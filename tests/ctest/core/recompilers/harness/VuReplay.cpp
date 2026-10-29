// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuReplay.h"

#include "RecompilerTestEnvironment.h"

#include "Config.h"
#include "Gif_Unit.h"
#include "VU.h"
#include "VUmicro.h"
#include "common/FPControl.h"

#if defined(_M_ARM64) || defined(__aarch64__)
#include "vixl/aarch64/decoder-aarch64.h"
#include "vixl/aarch64/disasm-aarch64.h"

// Avoid including microVU-arm64.h directly: it carries __fi function defs
// without `inline`, which create ODR collisions when pulled into more than
// one TU. Just declare the needed accessor.
namespace vu_capture_internal {
	void GetCompiledRange(int vu_index, const u8** out_start, const u8** out_end);
}
namespace mvu_test_hooks {
	void PoisonLpStateBlockType(int vu_index, u32 block_type);
}
#endif

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace recompiler_tests {

namespace
{

VURegs& Regs(int idx) { return vuRegs[idx]; }
u32 MemSize(int idx) { return idx == 0 ? VU0_MEMSIZE : VU1_MEMSIZE; }
u32 RunningBit(int idx) { return idx == 0 ? 0x1u : 0x100u; }

BaseVUmicroCPU* InterpCpu(int idx)
{
	return idx == 0 ? static_cast<BaseVUmicroCPU*>(&CpuIntVU0)
	                : static_cast<BaseVUmicroCPU*>(&CpuIntVU1);
}

BaseVUmicroCPU* JitCpu(int idx)
{
	return idx == 0 ? static_cast<BaseVUmicroCPU*>(&CpuMicroVU0)
	                : static_cast<BaseVUmicroCPU*>(&CpuMicroVU1);
}

// Whole-VuMem window so the diff covers any byte the program touches.
std::vector<VuMemWindow> WholeMemWindow(int vu_index)
{
	return {VuMemWindow{0u, std::vector<u8>(MemSize(vu_index))}};
}

// VPU_STAT lives in VU0's VI for both VUs. Set the running bit for the
// replayed VU, and bit 12 (VU1 XGKICK still transferring) from the
// capture's own XGKICK state, so neither leaks in from an earlier replay.
// Bits 0-11 are cleared; the VU's own VI copy mirrors the result.
void SeedVpuStat(const vu_capture::CaptureRecord& rec)
{
	const int idx = static_cast<int>(rec.vu_index);
	constexpr u32 kXgkickBusy = 1u << 12;
	u32 stat = vuRegs[0].VI[REG_VPU_STAT].UL & ~(0xFFFu | kXgkickBusy);
	stat |= RunningBit(idx);
	if (idx == 1 && rec.state.xgkickenable)
		stat |= kXgkickBusy;
	vuRegs[0].VI[REG_VPU_STAT].UL = stat;
	Regs(idx).VI[REG_VPU_STAT].UL = stat;
}

// Restore microcode + data memory + registers from a CaptureRecord. Sets up
// the VPU_STAT running bit + start_pc the same way VuTestHarness does, and
// drops the JIT block cache so the capture's start_pc compiles fresh.
void PrimeFromCapture(const vu_capture::CaptureRecord& rec,
	bool reset_block_cache = true)
{
	const int idx = static_cast<int>(rec.vu_index);
	auto& vu = Regs(idx);

	std::memcpy(vu.Micro, rec.microcode.data(), rec.microcode.size());
	std::memcpy(vu.Mem, rec.vumem.data(), rec.vumem.size());

	vu_capture::RestoreState(rec.state, vu);
	vu_capture::RestoreExternal(idx, rec.external);

	vu.start_pc = rec.start_pc;
	vu.VI[REG_TPC].UL = rec.start_pc / 8u;
	SeedVpuStat(rec);

	vu.cycle = 0;
	vu.ebit = 0;
	vu.branch = 0;
	vu.branchpc = 0;
	vu.delaybranchpc = 0;
	vu.takedelaybranch = false;
	vu.flags = 0;
	vu.fmacreadpos = vu.fmacwritepos = vu.fmaccount = 0;
	vu.ialureadpos = vu.ialuwritepos = vu.ialucount = 0;
	std::memset(&vu.fdiv, 0, sizeof(vu.fdiv));
	std::memset(&vu.efu, 0, sizeof(vu.efu));
	std::memset(&vu.fmac, 0, sizeof(vu.fmac));
	std::memset(&vu.ialu, 0, sizeof(vu.ialu));

	InterpCpu(idx)->SetStartPC(rec.start_pc);
	JitCpu(idx)->SetStartPC(rec.start_pc);

	// Reset drops every compiled VU block so the capture's start_pc compiles
	// fresh. Callers that re-seed the architectural + pipeline state between
	// timed bench iterations pass reset_block_cache=false: they want the
	// already-compiled block kept so the measured Execute is steady-state
	// (no recompile) rather than paying the one-time compile every iter.
	if (reset_block_cache)
		RecompilerTestEnvironment::ResetVuBlockCache(idx);
}

void RunInterpFromSeeded(int idx, u32 cycles)
{
	// Same REC_VU1 / EnableVU1 dance VuTestHarness does — see VuTestHarness.cpp
	// `RunInterpFromSeeded` for the rationale (Gif_Unit GetGSPacketSize gates
	// its bit-31 EOP signal on `(CHECK_XGKICKHACK || !REC_VU1)`).
	const bool saved = EmuConfig.Cpu.Recompiler.EnableVU1;
	EmuConfig.Cpu.Recompiler.EnableVU1 = false;
	InterpCpu(idx)->Execute(cycles);
	EmuConfig.Cpu.Recompiler.EnableVU1 = saved;
}

void RunJitFromSeeded(int idx, u32 cycles)
{
#if defined(_M_ARM64) || defined(__aarch64__)
	// Offline repro knob for live-carried block-search keys: poison lpState
	// AFTER PrimeFromCapture's cache reset (which zeroes it) and right before
	// dispatch, the way a live mid-program resume key or a savestate thaw
	// would present it. Testing-only (vurunner / recompiler_tests).
	if (const char* poison = std::getenv("PCSX2_VU_POISON_LPSTATE_BLOCKTYPE"))
		mvu_test_hooks::PoisonLpStateBlockType(idx, static_cast<u32>(std::atoi(poison)));
#endif
	JitCpu(idx)->Execute(cycles);
}

// Holds the host FPCR at the replayed VU's FPCR (FZ + ChopZero by default) for its lifetime. Every driver
// that executes VU code needs it: the interpreter computes with host scalar FP (`fs / ft` in `_vuDIV`, host
// `sqrt` in `_vuSQRT`, ...), which uses the thread's FPCR, and microVU writes FPCR at dispatch only when the
// EE and VU FPCRs differ, so with the default config the JIT also inherits the caller's FPCR. Under
// round-to-nearest either computes different values than the game did: 1-ULP false divergences in a diff,
// and in a bench, a program whose loop condition depends on the rounding can run to the cycle budget.
class ScopedVuFpcr
{
public:
	explicit ScopedVuFpcr(int idx)
		: m_saved(FPControlRegister::GetCurrent())
	{
		FPControlRegister::SetCurrent(idx == 0 ? EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR);
	}
	~ScopedVuFpcr() { FPControlRegister::SetCurrent(m_saved); }
	ScopedVuFpcr(const ScopedVuFpcr&) = delete;
	ScopedVuFpcr& operator=(const ScopedVuFpcr&) = delete;

private:
	FPControlRegister m_saved;
};

// The PATH1 sink covers Gif_Unit::TransferGSPacketData, but microVU's XGKICK
// wrap path sends the pre-wrap head through Gif_Path::CopyGSPacketData
// directly (the harness blind spot pstef found landing the console XGKICK
// cases) — those bytes land in the REAL gifPath[1] ring, which nothing drains
// in a runner with no GS thread. Across a few hundred wrapped-kick captures
// in one process the ring fills, CopyGSPacketData calls mtgsReadWait, and
// MTGS::WaitGS trips its devel closed-thread assert (abort mid-sweep; release
// would early-return and lose the wait entirely). Reset the path per replay
// so escaped bytes can never accumulate across captures — still correct once
// the sink covers both entry points, just belt-and-suspenders.
static void ResetGifPath1()
{
	gifUnit.gifPath[GIF_PATH_1].Reset();
}

} // namespace

void ReseedFromCapture(const vu_capture::CaptureRecord& rec)
{
	// Re-seed the architectural + pipeline state from the capture WITHOUT
	// dropping the compiled VU block cache. Used between timed bench iters so
	// each iteration re-runs the whole program from its captured entry state
	// (no E-bit drift-to-halt) while still measuring already-compiled code
	// (no per-iter recompile).
	PrimeFromCapture(rec, /*reset_block_cache=*/false);
}

std::vector<PmuCounters::Values> BenchJitSteady(const vu_capture::CaptureRecord& rec,
	u32 iters)
{
	std::vector<PmuCounters::Values> samples;
	if (rec.vu_index > 1 || iters == 0)
		return samples;
	if (!RecompilerTestEnvironment::IsReady())
		return samples;

	const int idx = static_cast<int>(rec.vu_index);
	const u32 cycles = rec.cycle_budget;

	PmuCounters::Group g;
	if (!g.Open())
		return samples;

	std::vector<u8> path1_buf;
	gif_test_hooks::g_path1_sink = &path1_buf;
	ResetGifPath1();
	const ScopedVuFpcr fpcr(idx);

	// Prime once WITH a cache reset so iter 0 compiles the block; subsequent
	// iters re-seed without a reset (block stays compiled) so the measured
	// Execute is honest steady-state program execution. iter 0 (which pays
	// the compile) is dropped as warmup by the caller.
	PrimeFromCapture(rec);
	samples.reserve(iters);
	for (u32 i = 0; i < iters; ++i)
	{
		if (i > 0)
			ReseedFromCapture(rec);
		path1_buf.clear();
		samples.push_back(g.Measure([&]() {
			RunJitFromSeeded(idx, cycles);
		}));
	}

	gif_test_hooks::g_path1_sink = nullptr;
	return samples;
}

VuReplayResult ReplayCapture(const vu_capture::CaptureRecord& rec,
	VuDiffMode diff_mode,
	u32 cycle_budget_override)
{
	VuReplayResult out;

	if (rec.vu_index > 1)
		return out;
	if (!RecompilerTestEnvironment::IsReady())
		return out;

	const int idx = static_cast<int>(rec.vu_index);
	const u32 cycles = cycle_budget_override ? cycle_budget_override : rec.cycle_budget;

	const auto windows = WholeMemWindow(idx);

	// Both passes run under the VU FPCR, so any divergence is a real
	// codegen difference rather than a rounding-mode one.
	const ScopedVuFpcr fpcr(idx);

	std::vector<u8> path1_buf;
	gif_test_hooks::g_path1_sink = &path1_buf;
	ResetGifPath1();
	PrimeFromCapture(rec);
	const VuSnapshot pre = VuSnapshot::Capture(idx, windows);
	path1_buf.clear();
	const u64 jit_cycle_before = Regs(idx).cycle;
	RunJitFromSeeded(idx, cycles);
	out.jit_cycles = Regs(idx).cycle - jit_cycle_before;
	out.jit_snapshot = VuSnapshot::Capture(idx, windows);
	out.path1_packets_jit = path1_buf;
	// Authoritative running bit lives in vuRegs[0] for both VUs (see header).
	out.jit_ebit = (vuRegs[0].VI[REG_VPU_STAT].UL & RunningBit(idx)) == 0;

	// Interp pass — restore the JIT-pass pre-state so the engines see
	// identical inputs.
	pre.Restore();
	// The VIF registers and (for VU0) VU1's registers are outside the
	// snapshot; a VU0 program can store into the latter during the JIT pass.
	vu_capture::RestoreExternal(idx, rec.external);
	std::memcpy(Regs(idx).Micro, rec.microcode.data(), rec.microcode.size());
	std::memcpy(Regs(idx).Mem, rec.vumem.data(), rec.vumem.size());
	Regs(idx).start_pc = rec.start_pc;
	Regs(idx).VI[REG_TPC].UL = rec.start_pc / 8u;
	SeedVpuStat(rec);
	InterpCpu(idx)->SetStartPC(rec.start_pc);
	JitCpu(idx)->SetStartPC(rec.start_pc);

	path1_buf.clear();
	const u64 interp_cycle_before = Regs(idx).cycle;
	RunInterpFromSeeded(idx, cycles);
	out.interp_cycles = Regs(idx).cycle - interp_cycle_before;
	out.interp_snapshot = VuSnapshot::Capture(idx, windows);
	out.path1_packets_interp = path1_buf;
	out.interp_ebit = (vuRegs[0].VI[REG_VPU_STAT].UL & RunningBit(idx)) == 0;

	gif_test_hooks::g_path1_sink = nullptr;

	out.diff_lines = DiffVu(out.jit_snapshot, out.interp_snapshot, diff_mode);
	out.diverged = !out.diff_lines.empty();
	out.ok = true;
	return out;
}

VuReplayResult ReplayJitOnly(const vu_capture::CaptureRecord& rec, u32 cycle_budget_override)
{
	VuReplayResult out;
	if (rec.vu_index > 1)
		return out;
	if (!RecompilerTestEnvironment::IsReady())
		return out;

	const int idx = static_cast<int>(rec.vu_index);
	const u32 cycles = cycle_budget_override ? cycle_budget_override : rec.cycle_budget;
	const auto windows = WholeMemWindow(idx);

	// Same FPCR handling as ReplayCapture's JIT pass.
	const FPControlRegister saved_fpcr = FPControlRegister::GetCurrent();
	FPControlRegister::SetCurrent((idx == 0) ? EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR);

	std::vector<u8> path1_buf;
	gif_test_hooks::g_path1_sink = &path1_buf;
	ResetGifPath1();
	PrimeFromCapture(rec);
	path1_buf.clear();
	const u64 jit_cycle_before = Regs(idx).cycle;
	RunJitFromSeeded(idx, cycles);
	out.jit_cycles = Regs(idx).cycle - jit_cycle_before;
	out.jit_snapshot = VuSnapshot::Capture(idx, windows);
	out.path1_packets_jit = path1_buf;
	out.jit_ebit = (vuRegs[0].VI[REG_VPU_STAT].UL & RunningBit(idx)) == 0;

	gif_test_hooks::g_path1_sink = nullptr;
	FPControlRegister::SetCurrent(saved_fpcr);
	out.ok = true;
	return out;
}

VuChainResult ReplayJitChain(const vu_capture::CaptureRecord& rec, u32 cycle_budget_override, u32 max_resumes)
{
	VuChainResult out;
	if (rec.vu_index > 1)
		return out;
	if (!RecompilerTestEnvironment::IsReady())
		return out;

	const int idx = static_cast<int>(rec.vu_index);
	const u32 cycles = cycle_budget_override ? cycle_budget_override : rec.cycle_budget;
	const auto windows = WholeMemWindow(idx);

	const FPControlRegister saved_fpcr = FPControlRegister::GetCurrent();
	FPControlRegister::SetCurrent((idx == 0) ? EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR);

	std::vector<u8> path1_buf;
	gif_test_hooks::g_path1_sink = &path1_buf;
	ResetGifPath1();
	PrimeFromCapture(rec);
	path1_buf.clear();
	const u64 jit_cycle_before = Regs(idx).cycle;
	RunJitFromSeeded(idx, cycles);
	out.first.jit_cycles = Regs(idx).cycle - jit_cycle_before;
	out.first.jit_snapshot = VuSnapshot::Capture(idx, windows);
	out.first.path1_packets_jit = path1_buf;
	out.first.jit_ebit = (vuRegs[0].VI[REG_VPU_STAT].UL & RunningBit(idx)) == 0;
	out.final_ebit = out.first.jit_ebit;
	while (!out.final_ebit && out.resumes < max_resumes)
	{
		JitCpu(idx)->Execute(cycles);
		out.resumes++;
		out.final_ebit = (vuRegs[0].VI[REG_VPU_STAT].UL & RunningBit(idx)) == 0;
	}
	out.final_cycles = Regs(idx).cycle - jit_cycle_before;
	out.final_snapshot = VuSnapshot::Capture(idx, windows);
	out.final_path1 = path1_buf;
	out.first.ok = true;
	out.ok = true;

	gif_test_hooks::g_path1_sink = nullptr;
	FPControlRegister::SetCurrent(saved_fpcr);
	return out;
}

VuReplayResult LoadAndReplay(const std::string& path,
	VuDiffMode diff_mode,
	u32 cycle_budget_override)
{
	vu_capture::CaptureRecord rec;
	if (!vu_capture::ReadFromFile(path, rec))
		return {};
	return ReplayCapture(rec, diff_mode, cycle_budget_override);
}

bool DumpJitAsm(const vu_capture::CaptureRecord& rec, const std::string& out_path)
{
#if defined(_M_ARM64) || defined(__aarch64__)
	if (rec.vu_index > 1)
		return false;
	if (!RecompilerTestEnvironment::IsReady())
		return false;

	const int idx = static_cast<int>(rec.vu_index);
	const u32 cycles = rec.cycle_budget;

	// Compile once via the standard prime+execute path.
	std::vector<u8> path1_buf;
	gif_test_hooks::g_path1_sink = &path1_buf;
	{
		const ScopedVuFpcr fpcr(idx);
		PrimeFromCapture(rec);
		RunJitFromSeeded(idx, cycles);
	}
	gif_test_hooks::g_path1_sink = nullptr;

	const u8* code_start = nullptr;
	const u8* code_end = nullptr;
	vu_capture_internal::GetCompiledRange(idx, &code_start, &code_end);
	if (!code_start || code_end <= code_start)
		return false;

	std::FILE* f = std::fopen(out_path.c_str(), "w");
	if (!f)
		return false;

	std::fprintf(f, "// vu_capture codegen dump\n");
	std::fprintf(f, "//   vu_index    = %d\n", idx);
	std::fprintf(f, "//   start_pc    = 0x%08X\n", rec.start_pc);
	std::fprintf(f, "//   host bytes  = %zu\n", static_cast<size_t>(code_end - code_start));
	std::fprintf(f, "//   host range  = [%p, %p)\n", code_start, code_end);
	std::fprintf(f, "\n");

	vixl::aarch64::Decoder decoder;
	vixl::aarch64::Disassembler disasm;
	decoder.AppendVisitor(&disasm);

	for (const u8* p = code_start; p + 4 <= code_end; p += 4)
	{
		u32 word;
		std::memcpy(&word, p, 4);
		const auto* instr = reinterpret_cast<const vixl::aarch64::Instruction*>(p);
		decoder.Decode(instr);
		std::fprintf(f, "  %p  %08x  %s\n",
			static_cast<const void*>(p), word, disasm.GetOutput());
	}

	std::fclose(f);
	return true;
#else
	(void)rec;
	std::FILE* f = std::fopen(out_path.c_str(), "w");
	if (!f)
		return false;
	std::fprintf(f, "// DumpJitAsm: not wired on non-ARM64 hosts.\n");
	std::fclose(f);
	return false;
#endif
}

std::vector<PmuCounters::Values> BenchJit(const vu_capture::CaptureRecord& rec,
	u32 iters,
	u32 cycle_budget_override,
	bool reprime_per_iter)
{
	std::vector<PmuCounters::Values> samples;
	if (rec.vu_index > 1 || iters == 0)
		return samples;
	if (!RecompilerTestEnvironment::IsReady())
		return samples;

	const int idx = static_cast<int>(rec.vu_index);
	const u32 cycles = cycle_budget_override ? cycle_budget_override : rec.cycle_budget;

	PmuCounters::Group g;
	if (!g.Open())
		return samples;

	// Path1 sink installed but unused — the bench cares about cycles, not
	// XGKICK packets. The sink is still installed because some captures may emit
	// path-1 packets and asserting when no sink is present must be avoided.
	std::vector<u8> path1_buf;
	gif_test_hooks::g_path1_sink = &path1_buf;
	const ScopedVuFpcr fpcr(idx);

	samples.reserve(iters);
	if (!reprime_per_iter)
		PrimeFromCapture(rec);
	for (u32 i = 0; i < iters; ++i)
	{
		if (reprime_per_iter)
			PrimeFromCapture(rec);
		path1_buf.clear();
		samples.push_back(g.Measure([&]() {
			RunJitFromSeeded(idx, cycles);
		}));
	}

	gif_test_hooks::g_path1_sink = nullptr;
	return samples;
}

} // namespace recompiler_tests
