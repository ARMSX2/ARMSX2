// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// vu-aot front end: decoder, the port of microVU's first pass, the program
// graph and SSA construction.
//
// The port is checked against microVU itself rather than against expected
// values written by hand: each program runs through microVU with the
// first-pass hook armed, and every block microVU compiles is re-analysed by
// vu-aot from the same entry key, in the same order, field by field. The
// hand-written programs aim at the timing rules (flag instances, Q/P lanes,
// stalls, the branch VI backup); the fuzzed ones at everything else.
//
// The SSA checks then ask semantic questions of the IR: which producer a flag
// reader binds to, which divide a Q reader sees, which lanes a partial write
// leaves alone.

#if defined(_M_ARM64) || defined(__aarch64__)

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "arm64/vuaot/VuAotDecode.h"
#include "arm64/vuaot/VuAotGraph.h"
#include "arm64/vuaot/VuAotIR.h"
#include "arm64/vuaot/VuAotMvuTrace.h"
#include "arm64/vuaot/VuAotPipe.h"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <link.h>
#endif

namespace recompiler_tests
{

using namespace vu;

namespace
{
inline VuOp L(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp U(u32 upper) { return VuOp{0x8000033c, upper}; }
inline VuOp P(u32 lower, u32 upper) { return VuOp{lower, upper}; }
constexpr u32 kNopL = 0x8000033c;

struct HookScope
{
	std::vector<vuaot::MvuBlockTrace> blocks;
	int vu;
	explicit HookScope(int vuIndex)
		: vu(vuIndex)
	{
		vuaot::g_mvuBlockHook = [](const vuaot::MvuBlockTrace& b, void* user) {
			HookScope* s = static_cast<HookScope*>(user);
			if (static_cast<int>(b.vuIndex) == s->vu)
				s->blocks.push_back(b);
		};
		vuaot::g_mvuBlockHookUser = this;
	}
	~HookScope()
	{
		vuaot::g_mvuBlockHook = nullptr;
		vuaot::g_mvuBlockHookUser = nullptr;
	}
};

struct FlagHackScope
{
	bool saved;
	explicit FlagHackScope(bool on)
		: saved(EmuConfig.Speedhacks.vuFlagHack)
	{
		EmuConfig.Speedhacks.vuFlagHack = on;
	}
	~FlagHackScope() { EmuConfig.Speedhacks.vuFlagHack = saved; }
};

// Runs `prog` through microVU and replays every compiled block through
// vu-aot's port. Returns the mismatching fields (empty = identical).
std::vector<std::string> CompareWithMicroVU(int vu, const std::vector<VuOp>& prog, bool flagHack,
	std::vector<vuaot::MvuBlockTrace>* blocksOut = nullptr)
{
	FlagHackScope fh(flagHack);
	VuTestHarness h(vu);
	h.LoadProgram(prog);
	std::vector<std::string> out;
	std::vector<vuaot::MvuBlockTrace> blocks;
	{
		HookScope hook(vu);
		h.RunNoDiff();
		blocks = hook.blocks;
	}
	vuaot::PipeConfig cfg;
	cfg.vuIndex = vu;
	cfg.flagHack = flagHack;
	cfg.xgkickHack = EmuConfig.Gamefixes.XgKickHack;
	vuaot::PipeAnalyzer an(vuRegs[vu].Micro, cfg);
	for (const auto& mb : blocks)
	{
		vuaot::PipeState entry;
		std::memcpy(entry.bytes, mb.entryState, 96);
		vuaot::MvuBlockTrace ours;
		an.Analyze(mb.startPC, entry).ToTrace(ours, vu);
		for (const auto& d : vuaot::CompareBlockTraces(mb, ours))
			out.push_back("block@" + std::to_string(mb.startPC) + ": " + d);
	}
	if (blocks.empty())
		out.push_back("microVU compiled no block");
	if (blocksOut)
		*blocksOut = std::move(blocks);
	return out;
}

std::string Join(const std::vector<std::string>& v, size_t max = 12)
{
	std::string s;
	for (size_t i = 0; i < v.size() && i < max; i++)
		s += v[i] + "\n";
	if (v.size() > max)
		s += "... " + std::to_string(v.size() - max) + " more\n";
	return s;
}

void ExpectMatch(int vu, const std::vector<VuOp>& prog)
{
	for (bool fh : {true, false})
	{
		const auto d = CompareWithMicroVU(vu, prog, fh);
		EXPECT_TRUE(d.empty()) << "vu" << vu << " flagHack=" << fh << "\n" << Join(d);
	}
}

// A micro image for the analyses that do not need microVU: pairs at pc 0, an
// E-bit on the last one and its delay slot appended.
std::vector<u8> Image(std::vector<VuOp> prog, u32 size = 0x4000)
{
	prog.back().upper |= bits::E;
	prog.push_back(U(VNOP_U()));
	std::vector<u8> img(size, 0);
	for (size_t i = 0; i < prog.size(); i++)
	{
		std::memcpy(&img[i * 8], &prog[i].lower, 4);
		std::memcpy(&img[i * 8 + 4], &prog[i].upper, 4);
	}
	return img;
}

struct Built
{
	vuaot::ProgramGraph graph;
	vuaot::ir::Function fn;
};

Built Build(const std::vector<u8>& img, bool flagHack = true)
{
	vuaot::GraphOptions go;
	go.cfg.vuIndex = 1;
	go.cfg.flagHack = flagHack;
	go.entries.push_back({0, vuaot::PipeState()});
	Built b;
	b.graph = vuaot::BuildProgramGraph(img.data(), go);
	vuaot::ir::BuildOptions bo;
	b.fn = vuaot::ir::BuildSSA(b.graph, bo);
	return b;
}

// The IR instructions of `op` emitted for the pair at `pc`, in order.
std::vector<vuaot::ir::ValueId> InstsAt(const vuaot::ir::Function& f, u32 pc, vuaot::ir::Op op)
{
	std::vector<vuaot::ir::ValueId> r;
	for (const auto& bl : f.blocks)
		for (auto v : bl.insts)
			if (f.insts[v].pc == pc && f.insts[v].op == op)
				r.push_back(v);
	return r;
}
} // namespace

TEST(VuAotDecode, UpperForms)
{
	using namespace vuaot;
	const UpperInst a = DecodeUpper(VMULx_U(mask::xyz, vf::vf3, vf::vf1, vf::vf2));
	EXPECT_EQ(a.op, UOp::MULbc);
	EXPECT_EQ(a.form, FtForm::Bc);
	EXPECT_EQ(a.bc, 0);
	EXPECT_EQ(a.fd, 3);
	EXPECT_EQ(a.fs, 1);
	EXPECT_EQ(a.ft, 2);
	EXPECT_EQ(a.mask, 0xE);
	EXPECT_TRUE(a.flagWriter);

	const UpperInst b = DecodeUpper(VMADDAw_U(mask::xyzw, vf::vf4, vf::vf5));
	EXPECT_EQ(b.op, UOp::MADDAbc);
	EXPECT_EQ(b.cls, UClass::FmacB);
	EXPECT_TRUE(b.isAcc);
	EXPECT_EQ(b.bc, 3);

	const UpperInst c = DecodeUpper(VMAX_U(mask::xyzw, vf::vf1, vf::vf2, vf::vf3));
	EXPECT_EQ(c.op, UOp::MAX);
	EXPECT_FALSE(c.flagWriter);
	EXPECT_FALSE(c.foundPass4);

	EXPECT_EQ(DecodeUpper(VNOP_U()).op, UOp::NOP);
	EXPECT_EQ(DecodeUpper(VCLIP_U(vf::vf1, vf::vf2)).op, UOp::CLIP);
	EXPECT_EQ(DecodeUpper(VOPMULA_U(mask::xyz, vf::vf1, vf::vf2)).op, UOp::OPMULA);
	EXPECT_EQ(DecodeUpper(VMULAq_U(mask::xyzw, vf::vf1)).form, FtForm::Q);
	EXPECT_EQ(DecodeUpper(VMULAi_U(mask::xyzw, vf::vf1)).form, FtForm::I);
	EXPECT_TRUE(DecodeUpper(EBit(U(VNOP_U())).upper).eBit);
}

TEST(VuAotDecode, LowerForms)
{
	using namespace vuaot;
	const LowerInst a = DecodeLower(VIBNE_L(vi::vi1, vi::vi2, -3));
	EXPECT_EQ(a.op, LOp::IBNE);
	EXPECT_EQ(a.it, 1);
	EXPECT_EQ(a.is, 2);
	EXPECT_EQ(a.imm11, -3);
	EXPECT_TRUE(a.isCondBranch());

	const LowerInst b = DecodeLower(VLQI_L(mask::xyz, vf::vf7, vi::vi3));
	EXPECT_EQ(b.op, LOp::LQI);
	EXPECT_EQ(b.ft, 7);
	EXPECT_EQ(b.is, 3);
	EXPECT_EQ(b.mask, 0xE);

	EXPECT_EQ(DecodeLower(VDIV_L(vf::vf1, 3, vf::vf2, 1)).op, LOp::DIV);
	EXPECT_EQ(DecodeLower(VDIV_L(vf::vf1, 3, vf::vf2, 1)).fsf, 3);
	EXPECT_EQ(DecodeLower(VDIV_L(vf::vf1, 3, vf::vf2, 1)).ftf, 1);
	EXPECT_EQ(DecodeLower(VESIN_L(vf::vf1, 2)).op, LOp::ESIN);
	EXPECT_EQ(DecodeLower(VXGKICK_L(vi::vi4)).op, LOp::XGKICK);
	EXPECT_EQ(DecodeLower(VFMAND_L(vi::vi1, vi::vi2)).op, LOp::FMAND);
	EXPECT_EQ(DecodeLower(VJALR_L(vi::vi15, vi::vi4)).op, LOp::JALR);
	EXPECT_EQ(DecodeLower(VIADDIU_L(vi::vi3, vi::vi0, 0x1234)).imm15, 0x1234u);
	// The assembler's lower NOP is MOVE vf00, vf00.
	EXPECT_EQ(DecodeLower(kNopL).op, LOp::MOVE);
	EXPECT_EQ(DecodeLower(kNopL).ft, 0);
}

// The decode memo is a per-thread heap table behind a thread_local pointer. Every thread, the
// compile worker included, must get identical decodes, including for words that collide in a slot.
TEST(VuAotDecode, MemoIsPerThreadAndConsistent)
{
	std::mt19937 rng(0x7e1f);
	std::vector<u32> words;
	for (int i = 0; i < 20000; i++)
		words.push_back(static_cast<u32>(rng()));
	// words that share a slot in the 4096-entry memo (code ^ code >> 12 ^ code >> 24, low 12 bits)
	for (u32 k = 1; k < 64; k++)
		words.push_back(0x00001000u * k + 0x2a5);
	const auto digest = [&](std::vector<u64>& out) {
		out.clear();
		for (int pass = 0; pass < 2; pass++) // (the second pass hits what the first left, where it survives)
			for (u32 w : words)
			{
				const vuaot::UpperInst& u = vuaot::DecodeUpperRef(w);
				const vuaot::LowerInst& l = vuaot::DecodeLowerRef(w);
				const vuaot::UpperInst uc = vuaot::DecodeUpper(w);
				const vuaot::LowerInst lc = vuaot::DecodeLower(w);
				EXPECT_EQ(u.word, w);
				EXPECT_EQ(l.word, w);
				EXPECT_EQ(std::memcmp(&u, &uc, sizeof(u)), 0);
				EXPECT_EQ(l.op, lc.op);
				if (pass == 0)
					out.push_back((static_cast<u64>(u.op) << 40) ^ (static_cast<u64>(l.op) << 24) ^ (static_cast<u64>(u.mask) << 16) ^
								  (static_cast<u64>(l.imm11) << 1) ^ l.imm12 ^ (static_cast<u64>(l.ft) << 48) ^ (static_cast<u64>(u.fd) << 56));
			}
	};
	std::vector<u64> here;
	digest(here);
	for (int t = 0; t < 3; t++)
	{
		std::vector<u64> there;
		std::thread th([&] { digest(there); });
		th.join();
		EXPECT_EQ(here, there) << "thread " << t;
	}
}

#if defined(__linux__)
// glibc carves a program's static TLS out of the stack of every thread it creates, and cubeb's ALSA
// thread asks for 256 KiB: a program with 265 KB of static TLS could not start that thread on the
// handhelds. The program's PT_TLS segment has to stay far below that.
TEST(VuAotDecode, StaticTlsStaysSmall)
{
	u64 tls = 0;
	dl_iterate_phdr(
		[](struct dl_phdr_info* info, size_t, void* data) -> int {
			u64* total = static_cast<u64*>(data);
			for (int i = 0; i < info->dlpi_phnum; i++)
				if (info->dlpi_phdr[i].p_type == PT_TLS)
					*total += info->dlpi_phdr[i].p_memsz;
			return 1; // (the first object is the program itself)
		},
		&tls);
	constexpr u64 kMaxStaticTlsBytes = 64 * 1024;
	std::printf("[vuaot tls] static TLS of the program: %llu bytes\n", static_cast<unsigned long long>(tls));
	EXPECT_LE(tls, kMaxStaticTlsBytes) << "a thread_local array or a large thread_local object grew the static TLS block; "
										  "allocate it on the heap behind a thread_local pointer";
}
#endif

// MAC/status readers at every distance from a run of FMACs, in a loop whose
// head reads flags (so the back edge is an exact-match link).
TEST(VuAotPipe, FlagReadersMatchMicroVU)
{
	for (int vu : {0, 1})
	{
		for (int gap = 0; gap < 6; gap++)
		{
			std::vector<VuOp> p;
			p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 3)));
			p.push_back(L(VIADDIU_L(vi::vi3, vi::vi0, 0xFF)));
			p.push_back(P(VFMAND_L(vi::vi1, vi::vi3), VMUL_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2))); // loop head
			p.push_back(U(VADD_U(mask::x | mask::y, vf::vf5, vf::vf4, vf::vf3)));
			p.push_back(U(VSUB_U(mask::z, vf::vf6, vf::vf5, vf::vf1)));
			for (int g = 0; g < gap; g++)
				p.push_back(U(VNOP_U()));
			p.push_back(L(VFSAND_L(vi::vi4, 0x2)));
			p.push_back(L(VFMAND_L(vi::vi5, vi::vi3)));
			p.push_back(L(VISUBIU_L(vi::vi2, vi::vi2, 1)));
			p.push_back(P(VIBNE_L(vi::vi2, vi::vi0, static_cast<s16>(2 - static_cast<int>(p.size()) - 1)), VMUL_U(mask::xyzw, vf::vf7, vf::vf4, vf::vf4)));
			p.push_back(U(VADD_U(mask::xyzw, vf::vf8, vf::vf7, vf::vf7)));
			p.push_back(EBit(L(VFMAND_L(vi::vi6, vi::vi3))));
			ExpectMatch(vu, p);
		}
	}
}

TEST(VuAotPipe, QAndPLatencyMatchMicroVU)
{
	for (int dist = 1; dist <= 14; dist++)
	{
		std::vector<VuOp> p;
		p.push_back(L(VDIV_L(vf::vf1, 0, vf::vf2, 1)));
		for (int k = 1; k < dist; k++)
			p.push_back(U(VNOP_U()));
		p.push_back(U(VMULq_U(mask::xyzw, vf::vf3, vf::vf1)));
		p.push_back(L(VRSQRT_L(vf::vf1, 0, vf::vf2, 1)));
		p.push_back(L(VESIN_L(vf::vf1, 0)));
		for (int k = 1; k < dist; k++)
			p.push_back(U(VNOP_U()));
		p.push_back(P(VMFP_L(mask::xyzw, vf::vf4), VMULq_U(mask::xyzw, vf::vf5, vf::vf1)));
		p.push_back(L(VWAITQ_L()));
		p.push_back(L(VELENG_L(vf::vf1)));
		p.push_back(L(VWAITP_L()));
		p.push_back(EBit(L(VMFP_L(mask::x, vf::vf6))));
		ExpectMatch(1, p);
	}
}

// A branch reading a VI the previous pair (or the previous block's delay slot)
// wrote: microVU's branch VI backup.
TEST(VuAotPipe, BranchViBackupMatchesMicroVU)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 2)));
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi1, 1)));
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, 2)));   // reads vi1 written just before
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 5)));  // delay slot writes vi2
	p.push_back(U(VNOP_U()));
	p.push_back(U(VNOP_U()));
	p.push_back(L(VIBEQ_L(vi::vi2, vi::vi0, 2)));   // first pair of the target block reads vi2
	p.push_back(U(VNOP_U()));
	p.push_back(U(VNOP_U()));
	p.push_back(EBit(U(VNOP_U())));
	ExpectMatch(0, p);
	ExpectMatch(1, p);
}

// Random programs: FMAC/ALU/flag/Q/P/memory mixes with forward branches, an
// occasional branch in a delay slot, and E-bit ends. No loops, so every
// program terminates; no imm=1 branches, the one shape microVU decides from a
// neighbouring slot's record left by an earlier compile.
TEST(VuAotPipe, FuzzMatchesMicroVU)
{
	std::mt19937 rng(20261013);
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	auto vf = [&]() { return R(8); };
	auto vi = [&]() { return R(6); };
	auto msk = [&]() { return (R(15) + 1) << 21; };
	int programs = 0, failures = 0;
	size_t blocksCompared = 0, opsCompared = 0, evilBlocks = 0, flagDecisions = 0;
	for (int vu : {1, 0})
	{
		for (int iter = 0; iter < 300; iter++)
		{
			const int len = 8 + R(40);
			std::vector<VuOp> p;
			p.push_back(L(VIADDIU_L(1, 0, R(4))));
			p.push_back(L(VIADDIU_L(2, 0, R(4))));
			for (int i = 0; i < len; i++)
			{
				u32 up, lo = kNopL;
				switch (R(12))
				{
					case 0: up = VADD_U(msk(), vf(), vf(), vf()); break;
					case 1: up = VMUL_U(msk(), vf(), vf(), vf()); break;
					case 2: up = VMADDx_U(msk(), vf(), vf(), vf()) + R(4); break;
					case 3: up = VMAX_U(msk(), vf(), vf(), vf()); break;
					case 4: up = VMULq_U(msk(), vf(), vf()); break;
					case 5: up = VMADDA_U(msk(), vf(), vf()); break;
					case 6: up = VCLIP_U(vf(), vf()); break;
					case 7: up = VOPMULA_U(mask::xyz, vf(), vf()); break;
					case 8: up = VSUBi_U(msk(), vf(), vf()); break;
					default: up = VNOP_U(); break;
				}
				switch (R(22))
				{
					case 0: lo = VFMAND_L(vi(), vi()); break;
					case 1: lo = VFSAND_L(vi(), 1 + R(2)); break;
					case 2: lo = VFCAND_L(R(0x1000)); break;
					case 3: lo = VDIV_L(vf(), R(4), vf(), R(4)); break;
					case 4: lo = VSQRT_L(vf(), R(4)); break;
					case 5: lo = (vu == 1) ? VESADD_L(vf()) : kNopL; break;
					case 6: lo = (vu == 1) ? VMFP_L(msk(), vf()) : kNopL; break;
					case 7: lo = VIADD_L(vi(), vi(), vi()); break;
					case 8: lo = VLQ_L(msk(), vf(), vi(), static_cast<s16>(R(8))); break;
					case 9: lo = VSQ_L(msk(), vf(), vi(), static_cast<s16>(R(8))); break;
					case 10: lo = VLQI_L(msk(), vf(), vi()); break;
					case 11: lo = VMOVE_L(msk(), vf(), vf()); break;
					case 12: lo = VWAITQ_L(); break;
					case 13: lo = VFSSET_L(R(0x1000) & 0xFC0); break;
					case 14: lo = VFCSET_L(R(0x1000000)); break;
					case 15:
					case 16:
					{
						if (i + 6 >= len)
							break;
						const s16 off = static_cast<s16>(2 + R(4));
						lo = R(2) ? VIBNE_L(vi(), vi(), off) : VIBGEZ_L(vi(), off);
						break;
					}
					case 17:
						if (i + 6 < len)
							lo = VB_L(static_cast<s16>(2 + R(3)));
						break;
					case 18: lo = VMTIR_L(vi(), vf(), R(4)); break;
					case 19: lo = VMR32_L(msk(), vf(), vf()); break;
					default: break;
				}
				if (R(30) == 0)
					up |= bits::E;
				if (R(25) == 0 && !(up & bits::E))
				{
					// I-bit pair: the lower word is an immediate
					up |= bits::I;
					lo = R(2) ? 0x3f800000u : 0x7f800000u;
				}
				p.push_back(VuOp{lo, up});
			}
			p.back().upper |= bits::E;
			if (p.back().upper & bits::I)
			{
				p.back().upper &= ~bits::I;
				p.back().lower = kNopL;
			}
			if (vuaot::DecodeLower(p.back().lower).isBranch())
				p.back().lower = kNopL;
			for (bool fh : {true, false})
			{
				programs++;
				std::vector<vuaot::MvuBlockTrace> blocks;
				const auto d = CompareWithMicroVU(vu, p, fh, &blocks);
				blocksCompared += blocks.size();
				for (const auto& b : blocks)
				{
					opsCompared += b.ops.size();
					evilBlocks += (b.entryState[6] == 2); // blockType
					for (const auto& o : b.ops)
						flagDecisions += o.mFlag.doFlag + o.sFlag.doNonSticky;
				}
				if (!d.empty())
				{
					failures++;
					ADD_FAILURE() << "vu" << vu << " iter " << iter << " flagHack=" << fh << "\n" << Join(d, 6);
				}
			}
		}
	}
	EXPECT_EQ(failures, 0) << programs << " programs";
	// The generator has to reach the shapes it is meant to: several blocks per
	// program, branches in delay slots, flag readers reviving producers.
	EXPECT_GT(blocksCompared, static_cast<size_t>(programs) * 2);
	EXPECT_GT(evilBlocks, 0u);
	EXPECT_GT(flagDecisions, 100u);
	std::printf("[vuaot fuzz] programs=%d blocks=%zu ops=%zu evil=%zu flagdecisions=%zu\n",
		programs, blocksCompared, opsCompared, evilBlocks, flagDecisions);
}

// Every block microVU compiles is a graph node with the same key.
TEST(VuAotGraph, CoversMicroVUBlocks)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 4)));
	p.push_back(L(VIADDIU_L(vi::vi3, vi::vi0, 0xF0)));
	p.push_back(P(VFMAND_L(vi::vi1, vi::vi3), VMUL_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2)));
	p.push_back(L(VIBEQ_L(vi::vi1, vi::vi0, 3)));
	p.push_back(U(VADD_U(mask::xyzw, vf::vf5, vf::vf4, vf::vf4)));
	p.push_back(U(VSUB_U(mask::xyzw, vf::vf6, vf::vf4, vf::vf4)));
	p.push_back(L(VISUBIU_L(vi::vi2, vi::vi2, 1)));
	p.push_back(P(VIBNE_L(vi::vi2, vi::vi0, -6), VMUL_U(mask::xyzw, vf::vf4, vf::vf4, vf::vf4)));
	p.push_back(U(VNOP_U()));
	p.push_back(EBit(U(VNOP_U())));
	for (bool fh : {true, false})
	{
		std::vector<vuaot::MvuBlockTrace> blocks;
		ASSERT_TRUE(CompareWithMicroVU(1, p, fh, &blocks).empty());
		vuaot::GraphOptions go;
		go.cfg.vuIndex = 1;
		go.cfg.flagHack = fh;
		vuaot::PipeState entry;
		std::memcpy(entry.bytes, blocks.front().entryState, 96);
		go.entries.push_back({blocks.front().startPC, entry});
		const vuaot::ProgramGraph g = vuaot::BuildProgramGraph(vuRegs[1].Micro, go);
		for (const auto& mb : blocks)
		{
			vuaot::PipeState st;
			std::memcpy(st.bytes, mb.entryState, 96);
			EXPECT_GE(g.Find(mb.startPC, st, fh), 0) << "block at " << mb.startPC << " flagHack=" << fh;
		}
		EXPECT_GE(g.stats.backEdges + g.stats.irreducibleEdges, 1u);
	}
}

// A return through a BAL link register resolves statically.
TEST(VuAotGraph, ReturnToLinkResolvesStatically)
{
	std::vector<VuOp> p;
	p.push_back(L(VBAL_L(vi::vi15, 4)));            // 0x00: call 0x28
	p.push_back(U(VNOP_U()));                         // 0x08
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 1)));  // 0x10: return lands here
	p.push_back(EBit(U(VNOP_U())));                   // 0x18
	p.push_back(U(VNOP_U()));                         // 0x20 delay slot
	p.push_back(L(VJR_L(vi::vi15)));                  // 0x28: subroutine
	p.push_back(U(VADD_U(mask::xyzw, vf::vf1, vf::vf1, vf::vf1)));  // 0x30 delay slot
	p.push_back(U(VNOP_U()));                         // 0x38 (unreached; carries the image's E-bit)
	const auto img = Image(p);
	const Built b = Build(img);
	EXPECT_EQ(b.graph.stats.jumpSites, 1u);
	EXPECT_EQ(b.graph.stats.jumpSitesStatic, 1u);
	EXPECT_EQ(b.graph.stats.jumpSitesUnresolved, 0u);
	bool found = false;
	for (const auto& n : b.graph.nodes)
		if (n.block.endKind == vuaot::BlockEnd::Jump)
			for (int t : n.jumpTargets)
				found |= (b.graph.nodes[t].pc == 0x10);
	EXPECT_TRUE(found);
	EXPECT_TRUE(vuaot::ir::Verify(b.fn).empty());
}

// A MAC reader binds to the producer four cycles back, not to the newest one.
TEST(VuAotSSA, MacReaderBindsToProducerFourCyclesBack)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 0xFF)));             // 0x00
	p.push_back(U(VMUL_U(mask::xyzw, vf::vf1, vf::vf2, vf::vf3)));  // 0x08: producer A
	p.push_back(U(VADD_U(mask::xyzw, vf::vf4, vf::vf2, vf::vf3)));  // 0x10: producer B
	p.push_back(U(VNOP_U()));                                       // 0x18
	p.push_back(U(VNOP_U()));                                       // 0x20
	p.push_back(L(VFMAND_L(vi::vi1, vi::vi2)));                     // 0x28: sees A (B lands next cycle)
	p.push_back(L(VFMAND_L(vi::vi3, vi::vi2)));                     // 0x30: sees B
	const auto img = Image(p);
	const Built b = Build(img);
	ASSERT_TRUE(vuaot::ir::Verify(b.fn).empty());
	using vuaot::ir::Op;
	const auto r1 = InstsAt(b.fn, 0x28, Op::FlagRead);
	const auto r2 = InstsAt(b.fn, 0x30, Op::FlagRead);
	ASSERT_EQ(r1.size(), 1u);
	ASSERT_EQ(r2.size(), 1u);
	const auto& m1 = b.fn.insts[b.fn.insts[r1[0]].args[0]];
	const auto& m2 = b.fn.insts[b.fn.insts[r2[0]].args[0]];
	EXPECT_EQ(m1.op, Op::MacFlags);
	EXPECT_EQ(m1.pc, 0x08u);
	EXPECT_EQ(m2.op, Op::MacFlags);
	EXPECT_EQ(m2.pc, 0x10u);
}

// A Q reader before the divide lands sees the old Q; after, the new one.
TEST(VuAotSSA, QReaderBindsByLatency)
{
	std::vector<VuOp> p;
	p.push_back(L(VDIV_L(vf::vf1, 0, vf::vf2, 0)));  // 0x00
	p.push_back(U(VMULq_U(mask::xyzw, vf::vf3, vf::vf1)));  // 0x08: old Q
	for (int i = 0; i < 8; i++)
		p.push_back(U(VNOP_U()));
	p.push_back(U(VMULq_U(mask::xyzw, vf::vf4, vf::vf1)));  // 0x50: new Q
	const auto img = Image(p);
	const Built b = Build(img);
	ASSERT_TRUE(vuaot::ir::Verify(b.fn).empty());
	using vuaot::ir::Op;
	const auto early = InstsAt(b.fn, 0x08, Op::Fmac);
	const auto late = InstsAt(b.fn, 0x50, Op::Fmac);
	ASSERT_EQ(early.size(), 1u);
	ASSERT_EQ(late.size(), 1u);
	const auto& qe = b.fn.insts[b.fn.insts[early[0]].args[4]];
	const auto& ql = b.fn.insts[b.fn.insts[late[0]].args[4]];
	EXPECT_EQ(qe.op, Op::LiveIn);
	EXPECT_EQ(ql.op, Op::Div);
	EXPECT_EQ(ql.pc, 0x00u);
}

// A .xyz write leaves .w's definition alone.
TEST(VuAotSSA, PartialWriteKeepsUntouchedLane)
{
	std::vector<VuOp> p;
	p.push_back(U(VADD_U(mask::xyz, vf::vf1, vf::vf2, vf::vf3)));     // 0x00
	p.push_back(L(VMOVE_L(mask::xyzw, vf::vf4, vf::vf1)));             // 0x08
	const auto img = Image(p);
	const Built b = Build(img);
	ASSERT_TRUE(vuaot::ir::Verify(b.fn).empty());
	using vuaot::ir::Op;
	const auto add = InstsAt(b.fn, 0x00, Op::Fmac);
	const auto mv = InstsAt(b.fn, 0x08, Op::Move);
	ASSERT_EQ(add.size(), 1u);
	ASSERT_EQ(mv.size(), 1u);
	const auto& a = b.fn.insts[mv[0]].args;
	EXPECT_EQ(a[0], add[0]);
	EXPECT_EQ(a[1], add[0]);
	EXPECT_EQ(a[2], add[0]);
	EXPECT_EQ(b.fn.insts[a[3]].op, Op::LiveIn);
	EXPECT_EQ(b.fn.insts[a[3]].var, vuaot::ir::var::VF(1, 3));
}

// A loop of two blocks, each entered from outside: irreducible in the graph
// the SSA is built on. SSA stays valid and minimal.
TEST(VuAotSSA, IrreducibleLoopIsValid)
{
	std::vector<VuOp> p;
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, 5)));                       // 0x00: -> 0x30 (Y)
	p.push_back(U(VNOP_U()));                                            // 0x08
	p.push_back(U(VADD_U(mask::xyzw, vf::vf1, vf::vf1, vf::vf2)));       // 0x10: X
	p.push_back(L(VB_L(2)));                                             // 0x18: -> 0x30
	p.push_back(U(VNOP_U()));                                            // 0x20
	p.push_back(U(VNOP_U()));                                            // 0x28
	p.push_back(P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf1))); // 0x30: Y
	p.push_back(L(VIBNE_L(vi::vi2, vi::vi3, -6)));                      // 0x38: -> 0x10 (X)
	p.push_back(U(VNOP_U()));                                            // 0x40
	p.push_back(U(VNOP_U()));                                            // 0x48
	const auto img = Image(p);
	const Built b = Build(img);
	EXPECT_TRUE(vuaot::ir::Verify(b.fn).empty()) << Join(vuaot::ir::Verify(b.fn));
	EXPECT_GE(b.graph.stats.irreducibleEdges, 1u);
	// No phi may merge one value with only itself.
	for (const auto& bl : b.fn.blocks)
	{
		for (auto ph : bl.phis)
		{
			const auto& in = b.fn.insts[ph];
			vuaot::ir::ValueId other = 0;
			bool trivial = true;
			for (auto a : in.args)
			{
				if (a == ph || a == other)
					continue;
				if (other)
				{
					trivial = false;
					break;
				}
				other = a;
			}
			EXPECT_FALSE(trivial) << "trivial phi %" << ph;
		}
	}
}

} // namespace recompiler_tests

#endif
