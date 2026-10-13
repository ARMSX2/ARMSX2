// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// vuopt milestone 2: block bodies lowered from vuopt's IR, run inside
// microVU's framing through the test-only body hook, must leave exactly the
// state microVU's own bodies leave. Every test runs a program twice from the
// same entry state -- microVU alone, then with every block body vuopt's --
// and compares all of VURegs, VU memory and the GIF output byte for byte,
// with the flag hack on and off.

#if defined(_M_ARM64) || defined(__aarch64__)

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "arm64/vuopt/VuOptMvuTrace.h"
#include "arm64/vuopt/VuOptDecode.h"
#include "arm64/vuopt/VuOptTier.h"
#include "arm64/vuopt/VuOptRegion.h"

#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace recompiler_tests
{

using namespace vu;

namespace
{
inline VuOp L(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp U(u32 upper) { return VuOp{0x8000033c, upper}; }
inline VuOp P(u32 lower, u32 upper) { return VuOp{lower, upper}; }
constexpr u32 kNopL = 0x8000033c;

enum class LowerKind { Other, Branch };
LowerKind Classify(u32 lower)
{
	const u32 op = lower >> 25;
	return (op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25 || (op >= 0x28 && op <= 0x2F)) ? LowerKind::Branch : LowerKind::Other;
}

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

// Clamp mode every harness below runs under (-1: the harness default, mode 1).
int g_clampMode = -1;
// Whether framed bodies are list-scheduled (milestone 9; regions always are).
bool g_schedule = false;

struct TierScope
{
	vuopt::BodyTier tier;
	explicit TierScope(int vu)
		: tier(vu, vuRegs[vu].Micro)
	{
		vuopt::g_vuoptBodyHook = [](const vuopt::MvuBlockTrace& b, const vuopt::MvuEmitEnv& env, void* user) {
			return static_cast<vuopt::BodyTier*>(user)->TryLower(b, env, nullptr);
		};
		vuopt::g_vuoptBodyHookUser = &tier;
	}
	~TierScope()
	{
		vuopt::g_vuoptBodyHook = nullptr;
		vuopt::g_vuoptBodyHookUser = nullptr;
	}
};

struct Outcome
{
	std::vector<std::string> diffs;
	u32 blocks = 0, lowered = 0;
	std::string declined;
};

// Byte comparison of everything the recompiler leaves in VURegs (the
// interpreter-only scratch fields are left out: RunNoDiff runs the
// interpreter too, and it leaves them behind for the next run), plus VU
// memory and the GIF output.
std::vector<std::string> Compare(const VuSnapshot& ra, const VuSnapshot& rb,
	const std::vector<u8>& pa, const std::vector<u8>& pb)
{
	struct F
	{
		const char* name;
		size_t off, size;
	};
#define FR(f) F{#f, offsetof(VURegs, f), sizeof(VURegs::f)}
	static const F kFields[] = {FR(VF), FR(VI), FR(ACC), FR(q), FR(p), FR(cycle), FR(flags), FR(start_pc), FR(ebit),
		FR(pending_q), FR(pending_p), FR(micro_macflags), FR(micro_clipflags), FR(micro_statusflags),
		FR(nextBlockCycles), FR(xgkickaddr), FR(xgkickdiff), FR(xgkicksizeremaining), FR(xgkicklastcycle),
		FR(xgkickcyclecount), FR(xgkickenable), FR(xgkickendpacket)};
#undef FR
	std::vector<std::string> out;
	const u8* x = reinterpret_cast<const u8*>(&ra.regs);
	const u8* y = reinterpret_cast<const u8*>(&rb.regs);
	for (const F& f : kFields)
	{
		for (size_t i = 0; i < f.size; i += 4)
		{
			const size_t n = std::min<size_t>(4, f.size - i);
			if (std::memcmp(x + f.off + i, y + f.off + i, n) == 0)
				continue;
			u32 va = 0, vb = 0;
			std::memcpy(&va, x + f.off + i, n);
			std::memcpy(&vb, y + f.off + i, n);
			char buf[96];
			std::snprintf(buf, sizeof(buf), "%s+%zu: mvu=%08x vuopt=%08x", f.name, i, va, vb);
			out.emplace_back(buf);
		}
	}
	for (size_t w = 0; w < ra.mem_windows.size() && w < rb.mem_windows.size(); w++)
		if (ra.mem_windows[w].bytes != rb.mem_windows[w].bytes)
			out.emplace_back("VU memory differs");
	if (pa != pb)
		out.emplace_back("GIF path 1 bytes differ");
	return out;
}

// Runs `prog` on VU1 through microVU, then with vuopt bodies, each in a fresh
// harness seeded the same way (a harness takes its pre-state from whatever
// its previous run left).
Outcome RunBoth(const std::vector<VuOp>& prog, bool flagHack,
	const std::function<void(VuTestHarness&)>& setup = {}, s32 limit = -1, bool dumpLast = false, u32 poolV = 0, u32 poolG = 0)
{
	FlagHackScope fh(flagHack);
	VuSnapshot ref;
	std::vector<u8> refPath;
	{
		VuTestHarness h(1);
		if (setup)
			setup(h);
		if (g_clampMode >= 0)
			h.SetVuClampMode(g_clampMode);
		h.TrackMemWindow(0, 0x4000);
		h.LoadProgram(prog);
		h.RunNoDiff();
		ref = h.JitSnapshot();
		refPath = h.Path1PacketBytesJit();
	}
	Outcome o;
	VuTestHarness h(1);
	if (setup)
		setup(h);
	if (g_clampMode >= 0)
		h.SetVuClampMode(g_clampMode);
	h.TrackMemWindow(0, 0x4000);
	h.LoadProgram(prog);
	{
		TierScope t(1);
		t.tier.limit = limit;
		if (dumpLast && limit > 0)
			t.tier.only = limit - 1;
		t.tier.dump = dumpLast;
		t.tier.poolLimitV = poolV;
		t.tier.poolLimitG = poolG;
		t.tier.schedule = g_schedule;
		h.RunNoDiff();
		o.blocks = t.tier.Blocks();
		o.lowered = t.tier.Lowered();
		for (const auto& d : t.tier.Declined())
			o.declined += d.first + " ";
	}
	o.diffs = Compare(ref, h.JitSnapshot(), refPath, h.Path1PacketBytesJit());
	return o;
}

std::string Join(const std::vector<std::string>& v, size_t max = 10)
{
	std::string s;
	for (size_t i = 0; i < v.size() && i < max; i++)
		s += v[i] + "\n";
	return s;
}

void ExpectIdentical(const std::vector<VuOp>& prog, const std::function<void(VuTestHarness&)>& setup = {})
{
	for (bool fh : {true, false})
	{
		const Outcome o = RunBoth(prog, fh, setup);
		EXPECT_GT(o.blocks, 0u);
		EXPECT_EQ(o.lowered, o.blocks) << "declined: " << o.declined;
		EXPECT_TRUE(o.diffs.empty()) << "flagHack=" << fh << "\n" << Join(o.diffs);
	}
}

// Register contents that reach the corner cases: signed zeros, denormals,
// infinities, NaNs, the clamp bounds, ordinary values.
void SeedSpecials(VuTestHarness& h, u32 seed)
{
	static const u32 kWords[] = {0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x7f800000, 0xff800000,
		0x7fc00000, 0xffc00000, 0x7f7fffff, 0xff7fffff, 0x3f800000, 0xbf800000, 0x40490fdb, 0x3a83126f,
		0x4b000000, 0xc2c80000, 0x00800000, 0x7fffffff, 0x41200000, 0xc0000000};
	std::mt19937 rng(seed);
	for (u32 r = 1; r < 32; r++)
		h.SetVfBits(r, kWords[rng() % 20], kWords[rng() % 20], kWords[rng() % 20], kWords[rng() % 20]);
	for (u32 r = 1; r < 16; r++)
		h.SetVi(r, rng() & 0x3ff);
	for (u32 a = 0; a < 0x4000; a += 4)
		h.WriteMemU32(a, (rng() & 1) ? kWords[rng() % 20] : static_cast<u32>(rng()));
}
} // namespace

// A branch reading a VI the previous block's delay slot overwrote reads the
// old value from microVU's VI backup slot. The body's write-back once stored
// the new VI into its home first and then read "the old value" from that home.
TEST(VuoptLowering, ViBackupSurvivesHomeWriteback)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi7, vi::vi0, 8)));      // 0x00
	p.push_back(L(VIADDIU_L(vi::vi15, vi::vi0, 0)));     // 0x08
	p.push_back(L(VB_L(2)));                             // 0x10 -> 0x28
	p.push_back(U(VNOP_U()));                            // 0x18
	p.push_back(U(VNOP_U()));                            // 0x20
	p.push_back(L(VB_L(2)));                             // 0x28 -> 0x40: vi7 enters this block from memory
	p.push_back(L(VIAND_L(vi::vi7, vi::vi7, vi::vi15))); // 0x30 delay slot: vi7 = 0, backs up 8
	p.push_back(U(VNOP_U()));                            // 0x38
	p.push_back(L(VIBEQ_L(vi::vi0, vi::vi7, 3)));        // 0x40 reads the backup (8): not taken
	p.push_back(U(VNOP_U()));                            // 0x48
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 1)));      // 0x50
	p.push_back(EBit(U(VNOP_U())));                      // 0x58
	p.push_back(U(VNOP_U()));                            // 0x60 delay slot / taken target
	p.push_back(EBit(L(VIADDIU_L(vi::vi2, vi::vi0, 2)))); // 0x68
	ExpectIdentical(p);
}

// Every FMAC form and dest-field shape over special values, with MAC and
// status readers so microVU writes the flag instances too.
static void FmacFormsOverSpecialsBody();
TEST(VuoptLowering, FmacFormsOverSpecials) { FmacFormsOverSpecialsBody(); }
static void FmacFormsOverSpecialsBody()
{
	const u32 masks[] = {mask::xyzw, mask::x, mask::y, mask::z, mask::w, mask::xyz, mask::x | mask::w, mask::y | mask::z};
	for (u32 seed = 1; seed <= 4; seed++)
	{
		for (u32 m : masks)
		{
			std::vector<VuOp> p;
			p.push_back(L(VIADDIU_L(vi::vi9, vi::vi0, 0xFF)));
			p.push_back(U(VADD_U(m, vf::vf1, vf::vf2, vf::vf3)));
			p.push_back(U(VSUB_U(m, vf::vf4, vf::vf5, vf::vf6)));
			p.push_back(U(VSUB_U(m, vf::vf7, vf::vf8, vf::vf8))); // Ft == Fs: +0
			p.push_back(P(VFMAND_L(vi::vi1, vi::vi9), VMUL_U(m, vf::vf9, vf::vf10, vf::vf11)));
			p.push_back(U(VMADD_U(m, vf::vf12, vf::vf13, vf::vf14)));
			p.push_back(U(VMSUB_U(m, vf::vf15, vf::vf16, vf::vf17)));
			p.push_back(P(VFSAND_L(vi::vi2, 0x3), VMAX_U(m, vf::vf18, vf::vf19, vf::vf20)));
			p.push_back(U(VMINI_U(m, vf::vf21, vf::vf22, vf::vf23)));
			p.push_back(U(VMULx_U(m, vf::vf24, vf::vf25, vf::vf26) + (seed & 3)));
			p.push_back(U(VMADDx_U(m, vf::vf27, vf::vf28, vf::vf29) + ((seed + 1) & 3)));
			p.push_back(P(VFMAND_L(vi::vi3, vi::vi9), VMSUBx_U(m, vf::vf30, vf::vf31, vf::vf1) + ((seed + 2) & 3)));
			p.push_back(U(VMULq_U(m, vf::vf2, vf::vf3)));
			p.push_back(U(VADDi_U(m, vf::vf4, vf::vf5)));
			p.push_back(U(VMULi_U(m, vf::vf6, vf::vf7)));
			p.push_back(U(VMAXi_U(m, vf::vf8, vf::vf9)));
			p.push_back(U(VMADDA_U(m, vf::vf10, vf::vf11)));
			p.push_back(U(VMSUBA_U(m, vf::vf12, vf::vf13)));
			p.push_back(U(VMULA_U(m, vf::vf14, vf::vf15)));
			p.push_back(U(VMADDAx_U(m, vf::vf16, vf::vf17)));
			p.push_back(U(VOPMULA_U(m & mask::xyz, vf::vf18, vf::vf19)));
			p.push_back(U(VOPMSUB_U(m, vf::vf20, vf::vf21, vf::vf22)));
			p.push_back(U(VABS_U(m, vf::vf23, vf::vf24)));
			p.push_back(P(VFMAND_L(vi::vi4, vi::vi9), VADD_U(m, vf::vf25, vf::vf26, vf::vf27)));
			p.push_back(P(VFSAND_L(vi::vi5, 0x1), VNOP_U()));
			p.push_back(EBit(L(VFMAND_L(vi::vi6, vi::vi9))));
			ExpectIdentical(p, [seed](VuTestHarness& h) {
				SeedSpecials(h, seed);
				h.SetQ(0x3f000000 + seed);
				h.SetVi(REG_I, (seed & 1) ? 0x7f800000 : 0x40400000);
			});
		}
	}
}

static void ConversionsClipAndClipReadersBody();
TEST(VuoptLowering, ConversionsClipAndClipReaders) { ConversionsClipAndClipReadersBody(); }
static void ConversionsClipAndClipReadersBody()
{
	for (u32 seed = 1; seed <= 6; seed++)
	{
		std::vector<VuOp> p;
		const u32 ftoi[4] = {UpperFD(mask::xyzw, vf::vf1, vf::vf2, 0x05, 0), UpperFD(mask::xyzw, vf::vf3, vf::vf4, 0x05, 1),
			UpperFD(mask::xyz, vf::vf5, vf::vf6, 0x05, 2), UpperFD(mask::w, vf::vf7, vf::vf8, 0x05, 3)};
		const u32 itof[4] = {UpperFD(mask::xyzw, vf::vf9, vf::vf10, 0x04, 0), UpperFD(mask::x | mask::y, vf::vf11, vf::vf12, 0x04, 1),
			UpperFD(mask::xyzw, vf::vf13, vf::vf14, 0x04, 2), UpperFD(mask::z, vf::vf15, vf::vf16, 0x04, 3)};
		for (int i = 0; i < 4; i++)
		{
			p.push_back(U(ftoi[i]));
			p.push_back(U(itof[i]));
		}
		p.push_back(U(VCLIP_U(vf::vf17, vf::vf18)));
		p.push_back(U(VCLIP_U(vf::vf19, vf::vf20)));
		p.push_back(U(VCLIP_U(vf::vf21, vf::vf22)));
		p.push_back(U(VNOP_U()));
		p.push_back(U(VNOP_U()));
		p.push_back(L(VFCAND_L(0x3f)));
		p.push_back(L(VFCOR_L(0xfdf7df)));
		p.push_back(L(VFCEQ_L(0x000041)));
		p.push_back(L(VFCGET_L(vi::vi4)));
		p.push_back(L(VFCSET_L(0x123456)));
		p.push_back(U(VCLIP_U(vf::vf23, vf::vf24)));
		p.push_back(U(VNOP_U()));
		p.push_back(U(VNOP_U()));
		p.push_back(U(VNOP_U()));
		p.push_back(EBit(L(VFCGET_L(vi::vi5))));
		ExpectIdentical(p, [seed](VuTestHarness& h) { SeedSpecials(h, seed + 10); });
	}
}

static void DivideUnitAndEfuBody();
TEST(VuoptLowering, DivideUnitAndEfu) { DivideUnitAndEfuBody(); }
static void DivideUnitAndEfuBody()
{
	for (u32 seed = 1; seed <= 6; seed++)
	{
		std::vector<VuOp> p;
		p.push_back(L(VDIV_L(vf::vf1, seed & 3, vf::vf2, (seed + 1) & 3)));
		p.push_back(L(VWAITQ_L()));
		p.push_back(U(VMULq_U(mask::xyzw, vf::vf3, vf::vf4)));
		p.push_back(L(VDIV_L(vf::vf0, 3, vf::vf5, seed & 3))); // 1/x off VF0: microVU leaves the quotient unclamped
		p.push_back(L(VWAITQ_L()));
		p.push_back(U(VMULq_U(mask::xyzw, vf::vf6, vf::vf7)));
		p.push_back(L(VSQRT_L(vf::vf8, seed & 3)));
		p.push_back(L(VWAITQ_L()));
		p.push_back(U(VADDq_U(mask::xyzw, vf::vf9, vf::vf10)));
		p.push_back(L(VRSQRT_L(vf::vf11, 0, vf::vf12, seed & 3)));
		p.push_back(L(VWAITQ_L()));
		p.push_back(U(VMULq_U(mask::x, vf::vf13, vf::vf14)));
		const u32 efu[] = {VESADD_L(vf::vf15), VERSADD_L(vf::vf16), VELENG_L(vf::vf17), VERLENG_L(vf::vf18),
			VESUM_L(vf::vf19), VEATANXY_L(vf::vf20), VEATANXZ_L(vf::vf21), VESQRT_L(vf::vf22, seed & 3),
			VERSQRT_L(vf::vf23, seed & 3), VERCPR_L(vf::vf24, seed & 3), VESIN_L(vf::vf25, seed & 3),
			VEATAN_L(vf::vf26, seed & 3), VEEXP_L(vf::vf27, seed & 3)};
		u32 dst = 1;
		for (u32 e : efu)
		{
			// the EFU2 forms take their dest field from bits 21-24: give them xyz or xyzw
			const u32 word = (e & ~(0xFu << 21)) | ((e >> 21) & 0xF ? 0 : (mask::xyz | ((seed & 1) ? mask::w : 0)));
			p.push_back(L(((e >> 21) & 0xF) ? e : word));
			p.push_back(L(VWAITP_L()));
			p.push_back(L(VMFP_L(mask::xyzw, dst)));
			dst++;
		}
		p.push_back(EBit(L(VFSAND_L(vi::vi1, 0x30))));
		ExpectIdentical(p, [seed](VuTestHarness& h) { SeedSpecials(h, seed + 20); });
	}
}

static void MovesMemoryRandomAndKickBody();
TEST(VuoptLowering, MovesMemoryRandomAndKick) { MovesMemoryRandomAndKickBody(); }
static void MovesMemoryRandomAndKickBody()
{
	for (u32 seed = 1; seed <= 4; seed++)
	{
		std::vector<VuOp> p;
		p.push_back(L(VMOVE_L((mask::x | mask::z), vf::vf1, vf::vf2)));
		p.push_back(L(VMR32_L(mask::xyzw, vf::vf3, vf::vf4)));
		p.push_back(L(VMR32_L(mask::y, vf::vf5, vf::vf6)));
		p.push_back(L(VMFIR_L((mask::x | mask::y | mask::w), vf::vf7, vi::vi3)));
		p.push_back(L(VMTIR_L(vi::vi4, vf::vf8, seed & 3)));
		p.push_back(L(VLQ_L(mask::xyzw, vf::vf9, vi::vi5, 3)));
		p.push_back(L(VLQ_L(mask::y, vf::vf10, vi::vi6, -2)));
		p.push_back(L(VLQI_L(mask::xyz, vf::vf11, vi::vi7)));
		p.push_back(L(VLQD_L(mask::w, vf::vf12, vi::vi8)));
		p.push_back(L(VSQ_L(mask::xyzw, vf::vf13, vi::vi9, 5)));
		p.push_back(L(VSQ_L(mask::x | mask::z, vf::vf14, vi::vi10, 7)));
		p.push_back(L(VSQI_L(mask::xyzw, vf::vf15, vi::vi11)));
		p.push_back(L(VSQD_L(mask::y, vf::vf16, vi::vi12)));
		p.push_back(L(VILW_L(mask::y, vi::vi13, vi::vi5, 1)));
		p.push_back(L(VILWR_L(mask::w, vi::vi14, vi::vi6)));
		p.push_back(L(VISW_L(mask::xyzw, vi::vi4, vi::vi9, 9)));
		p.push_back(L(VISWR_L(mask::z, vi::vi13, vi::vi10)));
		p.push_back(L(BuildLowerT3(0x3E, 0x10, 0, vf::vf17, 0, seed & 3, 0))); // RINIT R, vf17
		p.push_back(L(BuildLowerT3(0x3C, 0x10, vf::vf18, 0, mask::xyzw)));      // RNEXT vf18
		p.push_back(L(BuildLowerT3(0x3F, 0x10, 0, vf::vf19, 0, (seed + 1) & 3, 0))); // RXOR R, vf19
		p.push_back(L(BuildLowerT3(0x3D, 0x10, vf::vf20, 0, (mask::x | mask::z))));        // RGET vf20
		p.push_back(L(VIADDIU_L(vi::vi15, vi::vi0, 0x10)));
		p.push_back(L(VXGKICK_L(vi::vi15)));
		p.push_back(IBit(VuOp{(seed & 1) ? 0x7f800001u : 0x40490fdbu, VMULi_U(mask::xyzw, vf::vf21, vf::vf22)}));
		p.push_back(U(VADDi_U(mask::xyzw, vf::vf23, vf::vf24)));
		p.push_back(EBit(U(VNOP_U())));
		ExpectIdentical(p, [seed](VuTestHarness& h) {
			SeedSpecials(h, seed + 30);
			// a one-quadword GIF packet at qword 0x10: NLOOP 1, EOP, PACKED, NREG 1 (A+D)
			h.WriteMemU128(0x100, 0x00008001, 0x10000000, 0x0000000e, 0);
			h.WriteMemU128(0x110, 0, 0, 0, 0);
		});
	}
}

// Random VU1 programs over special values: forward branches, E-bits, I-bit
// pairs, flag readers and writers, the divide unit and the EFU. Blocks vuopt
// declines (a branch in a delay slot) run in microVU; everything else is
// vuopt's, and the result has to be byte for byte microVU's.
static void FuzzBody();
TEST(VuoptLowering, Fuzz) { FuzzBody(); }
static void FuzzBody()
{
	std::mt19937 rng(20261013);
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	auto vf = [&]() { return R(12); };
	auto vi = [&]() { return R(7); };
	auto msk = [&]() { return (R(15) + 1) << 21; };
	int programs = 0, failures = 0;
	u32 lowered = 0, blocks = 0;
	for (int iter = 0; iter < 160; iter++)
	{
		const int len = 8 + R(40);
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(1, 0, R(4))));
		p.push_back(L(VIADDIU_L(2, 0, R(4))));
		for (int i = 0; i < len; i++)
		{
			u32 up, lo = kNopL;
			switch (R(16))
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
				case 9: up = VMSUBx_U(msk(), vf(), vf(), vf()) + R(4); break;
				case 10: up = UpperFD(msk(), vf(), vf(), 0x05, R(4)); break; // FTOI
				case 11: up = VMINI_U(msk(), vf(), vf(), vf()); break;
				default: up = VNOP_U(); break;
			}
			switch (R(26))
			{
				case 0: lo = VFMAND_L(vi(), vi()); break;
				case 1: lo = VFSAND_L(vi(), 1 + R(2)); break;
				case 2: lo = VFCAND_L(R(0x1000)); break;
				case 3: lo = VDIV_L(vf(), R(4), vf(), R(4)); break;
				case 4: lo = VSQRT_L(vf(), R(4)); break;
				case 5: lo = VESADD_L(vf()) | mask::xyz; break;
				case 6: lo = VMFP_L(msk(), vf()); break;
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
					if (i + 6 < len)
						lo = R(2) ? VIBNE_L(vi(), vi(), static_cast<s16>(2 + R(4))) : VIBGEZ_L(vi(), static_cast<s16>(2 + R(4)));
					break;
				case 17:
					if (i + 6 < len)
						lo = VB_L(static_cast<s16>(2 + R(3)));
					break;
				case 18: lo = VMTIR_L(vi(), vf(), R(4)); break;
				case 19: lo = VMR32_L(msk(), vf(), vf()); break;
				case 20: lo = VRSQRT_L(vf(), R(4), vf(), R(4)); break;
				case 21: lo = VESIN_L(vf(), R(4)); break;
				case 22: lo = VISW_L(msk(), vi(), vi(), static_cast<s16>(R(16))); break;
				case 23: lo = VILW_L(msk(), vi(), vi(), static_cast<s16>(R(16))); break;
				default: break;
			}
			if (R(30) == 0)
				up |= bits::E;
			if (R(25) == 0 && !(up & bits::E))
			{
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
		const LowerKind last = Classify(p.back().lower);
		if (last == LowerKind::Branch)
			p.back().lower = kNopL;
		for (bool fh : {true, false})
		{
			programs++;
			const Outcome o = RunBoth(p, fh, [iter](VuTestHarness& h) { SeedSpecials(h, 1000 + iter); });
			blocks += o.blocks;
			lowered += o.lowered;
			if (!o.diffs.empty())
			{
				failures++;
				std::string listing;
				for (size_t k = 0; k < p.size(); k++)
				{
					char buf[160];
					vuopt::InstPair ip;
					ip.pc = static_cast<u32>(k * 8);
					ip.upper = vuopt::DecodeUpper(p[k].upper);
					ip.lower = vuopt::DecodeLower(p[k].lower);
					vuopt::FormatPair(ip, buf, sizeof(buf));
					listing += std::string(buf) + "\n";
				}
				ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " declined: " << o.declined << "\n" << Join(o.diffs, 6)
							  << (getenv("VUOPT_LISTING") ? listing : std::string());
				if (getenv("VUOPT_BISECT"))
				{
					const auto seed = [iter](VuTestHarness& h) { SeedSpecials(h, 1000 + iter); };
					for (s32 lim = 1; lim <= static_cast<s32>(o.blocks); lim++)
					{
						if (!RunBoth(p, fh, seed, lim).diffs.empty())
						{
							std::printf("first breaking block: index %d\n", lim - 1);
							RunBoth(p, fh, seed, lim, true);
							break;
						}
					}
				}
			}
		}
	}
	EXPECT_EQ(failures, 0) << programs << " programs";
	// The rest are blocks around a branch in a delay slot, which vuopt declines.
	EXPECT_GT(lowered, blocks * 8 / 10) << lowered << " of " << blocks << " blocks lowered";
	std::printf("[vuopt lowering fuzz] programs=%d blocks=%u lowered=%u\n", programs, blocks, lowered);
}

// A block ending in JR whose target VI is also that VI's final value, under
// enough general-register pressure that the value is evicted before the end.
// It must not take the early write-back to its home: the end of the block
// still reads it as the jump target.
TEST(VuoptLowering, JumpTargetSurvivesEarlyWriteback)
{
	for (int filler = 10; filler <= 22; filler += 3)
	{
		std::vector<VuOp> p;
		const u32 target = 0x200 / 8;
		p.push_back(L(VIADDIU_L(vi::vi6, vi::vi0, static_cast<u16>(target))));
		// many live general values: flag reads into every other VI, read again
		// at the end through their homes, plus MAC instances
		for (int k = 0; k < filler; k++)
		{
			const u32 v = 1 + (k % 15);
			if (v == 6)
				continue;
			p.push_back(P(VMTIR_L(v, 1 + (k % 8), k & 3), VMUL_U(mask::xyzw, 9 + (k % 6), 1 + (k % 8), 2 + (k % 7))));
		}
		p.push_back(L(VJR_L(vi::vi6)));
		p.push_back(U(VNOP_U()));
		while (p.size() < target)
			p.push_back(U(VNOP_U()));
		p.push_back(EBit(L(VIADDIU_L(vi::vi7, vi::vi6, 1))));
		for (bool fh : {true, false})
		{
			const Outcome o = RunBoth(
				p, fh,
				[](VuTestHarness& h) {
					for (int r = 1; r < 16; r++)
						h.SetVf(r, 1.0f + r, 2.5f * r, -r, 0.5f);
				},
				-1, false, 0, 8);
			EXPECT_TRUE(o.diffs.empty()) << "filler=" << filler << " flagHack=" << fh << "\n" << Join(o.diffs);
			EXPECT_EQ(o.lowered, o.blocks) << o.declined;
		}
	}
}

// Long straight blocks over every VF register: enough live values that the
// allocator spills and reloads, with every broadcast form, partial writes,
// moves and flag readers in between.
static void PressureFuzzBody();
TEST(VuoptLowering, PressureFuzz) { PressureFuzzBody(); }
static void PressureFuzzBody()
{
	std::mt19937 rng(20261014);
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	auto vf = [&]() { return 1 + R(31); };
	auto vi = [&]() { return 1 + R(6); };
	auto msk = [&]() { return (R(15) + 1) << 21; };
	const int iters = getenv("VUOPT_PRESSURE_ITERS") ? std::atoi(getenv("VUOPT_PRESSURE_ITERS")) : 60;
	int programs = 0, failures = 0;
	u32 lowered = 0, blocks = 0;
	std::string lastDeclined;
	for (int iter = 0; iter < iters; iter++)
	{
		const int len = 60 + R(140);
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(1, 0, R(4))));
		p.push_back(L(VIADDIU_L(2, 0, R(4))));
		for (int i = 0; i < len; i++)
		{
			u32 up = VNOP_U(), lo = kNopL;
			switch (R(14))
			{
				case 0: up = VADD_U(msk(), vf(), vf(), vf()); break;
				case 1: up = VMUL_U(msk(), vf(), vf(), vf()); break;
				case 2: up = VMULx_U(msk(), vf(), vf(), vf()) + R(4); break;
				case 3: up = VMADDx_U(msk(), vf(), vf(), vf()) + R(4); break;
				case 4: up = VMSUBx_U(msk(), vf(), vf(), vf()) + R(4); break;
				case 5: up = VMULAx_U(msk(), vf(), vf()) + R(4); break;
				case 6: up = VMADDAx_U(msk(), vf(), vf()) + R(4); break;
				case 7: up = VADDx_U(msk(), vf(), vf(), vf()) + R(4); break;
				case 8: up = VMULq_U(msk(), vf(), vf()); break;
				case 9: up = VMAX_U(msk(), vf(), vf(), vf()); break;
				case 10: up = VSUBx_U(msk(), vf(), vf(), vf()) + R(4); break;
				case 11: up = VMADD_U(msk(), vf(), vf(), vf()); break;
				default: break;
			}
			switch (R(12))
			{
				case 0: lo = VMOVE_L(msk(), vf(), vf()); break;
				case 1: lo = VMR32_L(msk(), vf(), vf()); break;
				case 2: lo = VSQRT_L(vf(), R(4)); break;
				case 3: lo = VDIV_L(vf(), R(4), vf(), R(4)); break;
				case 4: lo = VLQ_L(msk(), vf(), vi(), static_cast<s16>(R(8))); break;
				case 5: lo = VSQ_L(msk(), vf(), vi(), static_cast<s16>(R(8))); break;
				case 6: lo = VFMAND_L(vi(), vi()); break;
				case 7: lo = VMTIR_L(vi(), vf(), R(4)); break;
				case 8: lo = VMFIR_L(msk(), vf(), vi()); break;
				default: break;
			}
			p.push_back(VuOp{lo, up});
		}
		p.back().upper |= bits::E;
		for (bool fh : {true, false})
		{
			programs++;
			// odd programs also run with small register pools
			const Outcome o = RunBoth(
				p, fh,
				[iter](VuTestHarness& h) {
					SeedSpecials(h, 5000 + iter);
					for (int r = 12; r < 32; r++)
						h.SetVf(r, 1.0f + r, -0.5f * r, 0.25f + r, 3.0f - r);
				},
				-1, false, (iter & 1) ? 12 : 0, (iter & 1) ? 8 : 0);
			blocks += o.blocks;
			lowered += o.lowered;
			if (!o.declined.empty())
				lastDeclined = o.declined;
			if (!o.diffs.empty())
			{
				failures++;
				ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " declined: " << o.declined << "\n" << Join(o.diffs, 6);
				if (getenv("VUOPT_SHRINK"))
				{
					// Replace pairs with NOPs while the program still fails.
					const auto seed = [iter](VuTestHarness& h) {
						SeedSpecials(h, 5000 + iter);
						for (int r = 12; r < 32; r++)
							h.SetVf(r, 1.0f + r, -0.5f * r, 0.25f + r, 3.0f - r);
					};
					std::vector<VuOp> q = p;
					for (bool changed = true; changed;)
					{
						changed = false;
						for (size_t k = 2; k + 1 < q.size(); k++)
						{
							{
								std::vector<VuOp> r = q;
								r.erase(r.begin() + static_cast<long>(k));
								if (!RunBoth(r, fh, seed).diffs.empty())
								{
									q = r;
									changed = true;
									k--;
									continue;
								}
							}
							for (int part = 0; part < 2; part++)
							{
								VuOp t = q[k];
								if (part == 0 && t.upper != VNOP_U())
									t.upper = VNOP_U();
								else if (part == 1 && t.lower != kNopL)
									t.lower = kNopL;
								else
									continue;
								std::vector<VuOp> r = q;
								r[k] = t;
								if (!RunBoth(r, fh, seed).diffs.empty())
								{
									q = r;
									changed = true;
								}
							}
						}
					}
					std::string listing;
					for (size_t k = 0; k < q.size(); k++)
					{
						if (q[k].upper == VNOP_U() && q[k].lower == kNopL)
							continue;
						char buf[160];
						vuopt::InstPair ip;
						ip.pc = static_cast<u32>(k * 8);
						ip.upper = vuopt::DecodeUpper(q[k].upper);
						ip.lower = vuopt::DecodeLower(q[k].lower);
						vuopt::FormatPair(ip, buf, sizeof(buf));
						listing += std::string(buf) + "\n";
					}
					std::string hex;
					for (const VuOp& op : q)
					{
						char buf[24];
						std::snprintf(buf, sizeof(buf), "%08x:%08x,", op.lower, op.upper);
						hex += buf;
					}
					std::printf("shrunk iter %d fh=%d seed=%d hex=%s\n%s%s\n", iter, fh, 5000 + iter, hex.c_str(), listing.c_str(),
						Join(RunBoth(q, fh, seed).diffs, 6).c_str());
				}
			}
		}
	}
	EXPECT_EQ(failures, 0) << programs << " programs";
	EXPECT_GT(lowered, blocks * 9 / 10) << lowered << " of " << blocks << " blocks lowered";
	std::printf("[vuopt pressure fuzz] programs=%d blocks=%u lowered=%u declined: %s\n", programs, blocks, lowered, lastDeclined.c_str());
}

//------------------------------------------------------------------
// Regions (milestone 4): microVU's framing replaced between vuopt's own
// blocks, state in register homes across links.
//------------------------------------------------------------------

namespace
{
struct RegionScope
{
	vuopt::RegionTier region;
	vuopt::BodyTier body;
	explicit RegionScope(int vu)
		: region(vu, vuRegs[vu].Micro)
		, body(vu, vuRegs[vu].Micro)
	{
		vuopt::g_vuoptRegionHook = [](const vuopt::MvuBlockTrace& b, const vuopt::MvuEmitEnv& env, void* user) {
			return static_cast<RegionScope*>(user)->region.OnCompile(b, env, nullptr);
		};
		vuopt::g_vuoptRegionHookUser = this;
		vuopt::g_vuoptBodyHook = [](const vuopt::MvuBlockTrace& b, const vuopt::MvuEmitEnv& env, void* user) {
			return static_cast<RegionScope*>(user)->body.TryLower(b, env, nullptr);
		};
		vuopt::g_vuoptBodyHookUser = this;
	}
	~RegionScope()
	{
		vuopt::g_vuoptRegionHook = nullptr;
		vuopt::g_vuoptRegionHookUser = nullptr;
		vuopt::g_vuoptBodyHook = nullptr;
		vuopt::g_vuoptBodyHookUser = nullptr;
	}
};

struct RegionOutcome
{
	std::vector<std::string> diffs;
	u32 regions = 0, nodes = 0;
};

// microVU alone against vuopt regions (and framed bodies elsewhere), running
// past the cycle budget so budget breaks and resumes happen.
RegionOutcome RunRegion(const std::vector<VuOp>& prog, bool flagHack, const std::function<void(VuTestHarness&)>& setup,
	bool resume = true, s32 vHomes = 14, bool dump = false, const std::map<u32, std::vector<u32>>* jumps = nullptr, u32 budget = 0)
{
	FlagHackScope fh(flagHack);
	VuSnapshot ref;
	std::vector<u8> refPath;
	{
		VuTestHarness h(1);
		if (setup)
			setup(h);
		if (g_clampMode >= 0)
			h.SetVuClampMode(g_clampMode);
		h.TrackMemWindow(0, 0x4000);
		h.LoadProgram(prog);
		if (resume)
			h.ResumeUntilTerminated();
		if (budget)
			h.SetCycleBudget(budget);
		h.RunNoDiff();
		ref = h.JitSnapshot();
		refPath = h.Path1PacketBytesJit();
	}
	RegionOutcome o;
	VuTestHarness h(1);
	if (setup)
		setup(h);
	if (g_clampMode >= 0)
		h.SetVuClampMode(g_clampMode);
	h.TrackMemWindow(0, 0x4000);
	h.LoadProgram(prog);
	if (resume)
		h.ResumeUntilTerminated();
	if (budget)
		h.SetCycleBudget(budget);
	{
		RegionScope rs(1);
		rs.region.maxVHomes = vHomes;
		rs.region.dump = dump;
		if (jumps)
			rs.region.jumpTargets = *jumps;
		h.RunNoDiff();
		o.regions = rs.region.Regions();
		o.nodes = rs.region.Nodes();
	}
	o.diffs = Compare(ref, h.JitSnapshot(), refPath, h.Path1PacketBytesJit());
	return o;
}
} // namespace

// A counted loop: FMACs, a flag reader, a divide whose Q crosses the link,
// a store; runs long enough to break on the cycle budget several times.
static void RegionCountedLoopBody();
TEST(VuoptRegion, CountedLoop) { RegionCountedLoopBody(); }
static void RegionCountedLoopBody()
{
	for (u32 n : {1u, 2u, 7u, 300u, 900u})
	{
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(n))));
		p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 0x10)));
		// loop head (pair 2)
		p.push_back(P(VDIV_L(vf::vf1, 0, vf::vf2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)));
		p.push_back(P(VFMAND_L(vi::vi3, vi::vi2), VADDq_U(mask::xyz, vf::vf5, vf::vf3)));
		p.push_back(P(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0), VMADDy_U(mask::x | mask::w, vf::vf6, vf::vf5, vf::vf4)));
		p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VSUB_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf3)));
		p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(2 - 6 - 1)), VMULAx_U(mask::xyzw, vf::vf7, vf::vf4)));
		p.push_back(P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMADDAw_U(mask::xyzw, vf::vf3, vf::vf4))); // delay slot
		p.push_back(EBit(P(VFSAND_L(vi::vi4, 0xfff), VNOP_U())));
		for (bool fh : {true, false})
		{
			const RegionOutcome o = RunRegion(p, fh, [](VuTestHarness& h) {
				h.SetVf(1, 2.0f, 3.0f, 4.0f, 5.0f);
				h.SetVf(2, 0.5f, 1.5f, 2.5f, 3.5f);
				h.SetVf(3, 1.0f, 1.0f, 1.0f, 1.0f);
				h.SetVf(4, 1.0001f, 0.9999f, 1.00001f, 0.5f);
			});
			EXPECT_TRUE(o.diffs.empty()) << "n=" << n << " flagHack=" << fh << "\n" << Join(o.diffs, 10);
			EXPECT_GE(o.regions, 1u) << "n=" << n;
		}
	}
}

// JR/JALR inside a region: a loop calls one of two subroutines through a
// table-loaded JALR, and each returns with JR. The region dispatches the
// targets it was given and sends every other one through microVU's jump
// cache; both must leave microVU's state.
TEST(VuoptRegion, RecordedJumpTargets)
{
	constexpr u32 kT1 = 12, kT2 = 16, kRet = 6, kHead = 2;
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 37)));                         // 0
	p.push_back(L(VIADDIU_L(vi::vi5, vi::vi0, 1)));                          // 1
	p.push_back(P(VIAND_L(vi::vi4, vi::vi1, vi::vi5), VMUL_U(mask::xyzw, vf::vf4, vf::vf4, vf::vf2)));   // 2: head
	p.push_back(L(VILW_L(mask::x, vi::vi3, vi::vi4, 0)));                    // 3
	p.push_back(L(VJALR_L(vi::vi15, vi::vi3)));                              // 4
	p.push_back(U(VADD_U(mask::xyzw, vf::vf5, vf::vf5, vf::vf1)));            // 5: delay slot
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMADD_U(mask::xyz, vf::vf6, vf::vf4, vf::vf5)));        // 6: return here
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(static_cast<int>(kHead) - 7 - 1))));      // 7
	p.push_back(U(VSUB_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf3)));            // 8: delay slot
	p.push_back(EBit(L(VFSAND_L(vi::vi6, 0xfff))));                           // 9
	p.push_back(U(VNOP_U()));                                                 // 10
	p.push_back(U(VNOP_U()));                                                 // 11
	p.push_back(U(VADD_U(mask::xyzw, vf::vf1, vf::vf1, vf::vf2)));            // 12: T1
	p.push_back(L(VJR_L(vi::vi15)));                                          // 13
	p.push_back(U(VMUL_U(mask::x | mask::y, vf::vf3, vf::vf1, vf::vf1)));              // 14: delay slot
	p.push_back(U(VNOP_U()));                                                 // 15
	p.push_back(P(VFMAND_L(vi::vi7, vi::vi5), VSUB_U(mask::xyzw, vf::vf2, vf::vf2, vf::vf1)));           // 16: T2
	p.push_back(L(VJR_L(vi::vi15)));                                          // 17
	p.push_back(EBit(U(VMULq_U(mask::z | mask::w, vf::vf3, vf::vf2))));               // 18: delay slot (never reached as E)
	// the final pair carries an E bit only to satisfy the harness; T2's JR
	// delay slot is not an E bit in effect because the jump leaves first.
	p.back().upper &= ~bits::E;
	p.push_back(EBit(U(VNOP_U())));                                           // 19
	const auto setup = [](VuTestHarness& h) {
		h.SetVf(1, 1.0f, 2.0f, 3.0f, 4.0f);
		h.SetVf(2, 0.5f, 0.25f, 0.125f, 1.0f);
		h.SetVf(3, -1.0f, 1.5f, 2.5f, -3.5f);
		h.SetVf(4, 1.001f, 0.999f, 1.0f, 0.5f);
		// the jump table: VI word x of quadwords 0 and 1
		h.WriteMemU32(0x00, kT2);
		h.WriteMemU32(0x10, kT1);
	};
	const std::map<u32, std::vector<u32>> all = {{kHead * 8, {kT1 * 8, kT2 * 8}}, {kT1 * 8, {kRet * 8}}, {kT2 * 8, {kRet * 8}}};
	const std::map<u32, std::vector<u32>> some = {{kHead * 8, {kT1 * 8}}, {kT2 * 8, {kRet * 8}}};
	const std::map<u32, std::vector<u32>> none;
	for (const auto* jt : {&none, &all, &some})
		for (bool fh : {true, false})
		{
			const RegionOutcome o = RunRegion(p, fh, setup, true, 14, false, jt);
			EXPECT_TRUE(o.diffs.empty()) << "targets " << jt->size() << " flagHack=" << fh << "\n" << Join(o.diffs, 10);
			EXPECT_GE(o.regions, 1u);
		}
}

// A vertex-transform loop over a matrix the loop never writes: the clamped
// copies of the matrix rows live in registers for the whole region
// (milestone 5), filled once by the region's entry loader.
static void RegionInvariantMatrixBody()
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 40)));   // 0: count
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 0x40))); // 1: pointer
	// 2: loop head
	p.push_back(P(VLQI_L(mask::xyzw, vf::vf10, vi::vi2), VMULAx_U(mask::xyzw, vf::vf1, vf::vf10)));
	p.push_back(U(VMADDAy_U(mask::xyzw, vf::vf2, vf::vf10)));
	p.push_back(U(VMADDAz_U(mask::xyzw, vf::vf3, vf::vf10)));
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMADDw_U(mask::xyzw, vf::vf11, vf::vf4, vf::vf10)));
	p.push_back(P(VSQ_L(mask::xyzw, vf::vf11, vi::vi2, 7), VMUL_U(mask::xyz, vf::vf12, vf::vf1, vf::vf11)));
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(2 - 7 - 1))));
	p.push_back(U(VSUB_U(mask::xyzw, vf::vf13, vf::vf2, vf::vf12)));
	p.push_back(EBit(L(VFSAND_L(vi::vi4, 0xfff))));
	for (bool fh : {true, false})
	{
		const RegionOutcome o = RunRegion(p, fh, [](VuTestHarness& h) {
			h.SetVfBits(1, 0x3f800000, 0x7f800000, 0x00000001, 0xbf800000);
			h.SetVfBits(2, 0x7fffffff, 0x3f000000, 0xff800000, 0x40000000);
			h.SetVfBits(3, 0x40400000, 0x80000000, 0x7f7fffff, 0xffc00000);
			h.SetVfBits(4, 0x3f800000, 0x3f800000, 0x7f900000, 0x3f800000);
			for (u32 a = 0x400; a < 0x1000; a += 4)
				h.WriteMemU32(a, (a & 0x30) == 0x30 ? 0x7f800000 : 0x3f800000 + a * 37);
		});
		EXPECT_TRUE(o.diffs.empty()) << "flagHack=" << fh << "\n" << Join(o.diffs, 10);
		EXPECT_GE(o.regions, 1u);
	}
}
TEST(VuoptRegion, InvariantMatrix) { RegionInvariantMatrixBody(); }

// Random loop programs: an outer counted loop (vi1) around random FMAC,
// flag, divide, memory and VI work with forward branches inside, and an
// inner counted loop (vi5) on some.
static void RegionLoopFuzzBody();
TEST(VuoptRegion, LoopFuzz) { RegionLoopFuzzBody(); }
static void RegionLoopFuzzBody()
{
	std::mt19937 rng(20261015);
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	auto vf = [&]() { return 1 + R(14); };
	auto vi = [&]() { return 2 + R(3); }; // vi2..vi4; vi1 / vi5 are loop counters
	auto msk = [&]() { return (R(15) + 1) << 21; };
	const int iters = getenv("VUOPT_REGION_ITERS") ? std::atoi(getenv("VUOPT_REGION_ITERS")) : 120;
	int failures = 0;
	u32 regions = 0, nodes = 0;
	for (int iter = 0; iter < iters; iter++)
	{
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(1 + R(60)))));
		const size_t head = p.size();
		const bool inner = R(2);
		const int len = 4 + R(14);
		int lastBranch = -10;
		for (int i = 0; i < len; i++)
		{
			u32 up = VNOP_U(), lo = kNopL;
			switch (R(10))
			{
				case 0: up = VADD_U(msk(), vf(), vf(), vf()); break;
				case 1: up = VMUL_U(msk(), vf(), vf(), vf()); break;
				case 2: up = VMADDx_U(msk(), vf(), vf(), vf()) + R(4); break;
				case 3: up = VMULAx_U(msk(), vf(), vf()) + R(4); break;
				case 4: up = VMULq_U(msk(), vf(), vf()); break;
				case 5: up = VMAX_U(msk(), vf(), vf(), vf()); break;
				case 6: up = VCLIP_U(vf(), vf()); break;
				case 7: up = VSUBx_U(msk(), vf(), vf(), vf()) + R(4); break;
				default: break;
			}
			switch (R(14))
			{
				case 0: lo = VFMAND_L(vi(), vi()); break;
				case 1: lo = VFSAND_L(vi(), R(0x1000)); break;
				case 2: lo = VFCAND_L(R(0x1000000)); break;
				case 3: lo = VDIV_L(vf(), R(4), vf(), R(4)); break;
				case 4: lo = VSQ_L(msk(), vf(), vi(), static_cast<s16>(R(8))); break;
				case 5: lo = VLQ_L(msk(), vf(), vi(), static_cast<s16>(R(8))); break;
				case 6: lo = VIADDIU_L(vi(), vi(), static_cast<u16>(R(8))); break;
				case 7: lo = VMTIR_L(vi(), vf(), R(4)); break;
				case 8: lo = VMOVE_L(msk(), vf(), vf()); break;
				case 9:
					// forward, inside the body, never in another branch's delay
					// slot or with a branch target in one
					if (i + 4 < len && i >= lastBranch + 3)
					{
						lo = VIBNE_L(vi(), vi(), static_cast<s16>(1 + R(2)));
						lastBranch = i;
					}
					break;
				case 10: lo = VSQRT_L(vf(), R(4)); break;
				default: break;
			}
			p.push_back(VuOp{lo, up});
		}
		if (inner)
		{
			p.push_back(L(VIADDIU_L(vi::vi5, vi::vi0, static_cast<u16>(1 + R(5)))));
			const size_t ih = p.size();
			p.push_back(U(VMADDAx_U(mask::xyzw, vf(), vf()) + R(4)));
			p.push_back(L(VISUBIU_L(vi::vi5, vi::vi5, 1)));
			p.push_back(L(VIBNE_L(vi::vi5, vi::vi0, static_cast<s16>(static_cast<int>(ih) - static_cast<int>(p.size()) - 1))));
			p.push_back(U(VMADD_U(mask::xyzw, vf(), vf(), vf())));
		}
		p.push_back(L(VISUBIU_L(vi::vi1, vi::vi1, 1)));
		p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(static_cast<int>(head) - static_cast<int>(p.size()) - 1))));
		p.push_back(U(VMUL_U(mask::xyzw, vf(), vf(), vf())));
		p.push_back(EBit(L(VFSAND_L(vi::vi4, 0xfff))));
		for (bool fh : {true, false})
		{
			const auto setup = [iter](VuTestHarness& h) {
				SeedSpecials(h, 9000 + iter);
				for (int r = 4; r < 16; r++)
					h.SetVf(r, 1.0f + 0.01f * r, 0.99f, -1.0f + 0.001f * r, 0.5f);
				for (int r = 2; r <= 4; r++)
					h.SetVi(r, static_cast<u16>(r * 8));
			};
			const RegionOutcome o = RunRegion(p, fh, setup, false);
			regions += o.regions;
			nodes += o.nodes;
			if (!o.diffs.empty())
			{
				failures++;
				std::string hex;
				for (const VuOp& op : p)
				{
					char buf[24];
					std::snprintf(buf, sizeof(buf), "%08x:%08x,", op.lower, op.upper);
					hex += buf;
				}
				ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " hex=" << hex << "\n" << Join(o.diffs, 8);
			}
		}
	}
	EXPECT_EQ(failures, 0);
	EXPECT_GT(regions, static_cast<u32>(iters)); // most programs form a region per flag-hack setting
	std::printf("[vuopt region fuzz] programs=%d regions=%u nodes=%u\n", iters * 2, regions, nodes);
}

// Milestone 8: a region tests the cycle budget only at loop heads and at
// entries from microVU, against what the run can spend before the next
// test, and continues a run that is short of it in microVU's own code. A
// single run under every budget from 1 up must stop exactly where microVU
// stops, with microVU's state.
static void RegionBudgetSweepBody();
TEST(VuoptRegion, BudgetSweep) { RegionBudgetSweepBody(); }
static void RegionBudgetSweepBody()
{
	std::vector<std::vector<VuOp>> progs;
	{
		// counted loop with a divide and a flag reader
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 9)));
		p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 0x10)));
		p.push_back(P(VDIV_L(vf::vf1, 0, vf::vf2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)));
		p.push_back(P(VFMAND_L(vi::vi3, vi::vi2), VADDq_U(mask::xyz, vf::vf5, vf::vf3)));
		p.push_back(P(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0), VMADDy_U(mask::x | mask::w, vf::vf6, vf::vf5, vf::vf4)));
		p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VSUB_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf3)));
		p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(2 - 6 - 1)), VMULAx_U(mask::xyzw, vf::vf7, vf::vf4)));
		p.push_back(P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMADDAw_U(mask::xyzw, vf::vf3, vf::vf4)));
		p.push_back(EBit(P(VFSAND_L(vi::vi4, 0xfff), VNOP_U())));
		progs.push_back(p);
	}
	{
		// a loop whose body branches both ways (a stretch of several blocks),
		// then a straight tail
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 6)));                          // 0
		p.push_back(U(VADD_U(mask::xyzw, vf::vf2, vf::vf2, vf::vf3)));           // 1 loop head
		p.push_back(L(VIAND_L(vi::vi5, vi::vi1, vi::vi6)));                      // 2
		p.push_back(L(VIBEQ_L(vi::vi5, vi::vi0, 3)));                            // 3 -> 7
		p.push_back(U(VMUL_U(mask::xyz, vf::vf4, vf::vf2, vf::vf3)));            // 4 delay
		p.push_back(P(VIADDIU_L(vi::vi7, vi::vi7, 3), VSUB_U(mask::xyzw, vf::vf5, vf::vf4, vf::vf2))); // 5
		p.push_back(U(VMADD_U(mask::xyzw, vf::vf6, vf::vf5, vf::vf3)));          // 6
		p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMAX_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf2))); // 7
		p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(1 - 8 - 1))));  // 8 -> 1
		p.push_back(U(VMULi_U(mask::xyzw, vf::vf8, vf::vf7)));                   // 9 delay
		p.push_back(P(VSQ_L(mask::xyzw, vf::vf8, vi::vi0, 3), VADD_U(mask::xyzw, vf::vf9, vf::vf8, vf::vf8))); // 10
		p.push_back(U(VMUL_U(mask::xyzw, vf::vf10, vf::vf9, vf::vf9)));          // 11
		p.push_back(EBit(U(VNOP_U())));                                          // 12
		progs.push_back(p);
	}
	int failures = 0;
	for (size_t k = 0; k < progs.size(); k++)
		for (bool fh : {true, false})
			for (u32 budget = 1; budget <= 140; budget++)
			{
				const RegionOutcome o = RunRegion(progs[k], fh, [](VuTestHarness& h) {
					SeedSpecials(h, 11);
					h.SetVi(6, 2);
				}, false, 14, false, nullptr, budget);
				EXPECT_GT(o.regions, 0u);
				if (!o.diffs.empty() && ++failures <= 4)
					ADD_FAILURE() << "program " << k << " flagHack=" << fh << " budget " << budget << "\n" << Join(o.diffs);
			}
	EXPECT_EQ(failures, 0);
}

// Milestone 10: two loops working on different vector registers get
// different home maps, so entering and leaving each loop moves variables
// between registers and memory (and fills clamped copies). Every budget
// from 1 up, so bails land inside each loop too.
static void RegionLoopMapsBody();
TEST(VuoptRegion, LoopMaps) { RegionLoopMapsBody(); }
static void RegionLoopMapsBody()
{
	std::vector<VuOp> p;
	const auto loop = [&](u32 counter, u32 first, u32 iters) {
		p.push_back(L(VIADDIU_L(counter, vi::vi0, iters)));
		const s32 head = static_cast<s32>(p.size());
		// a body of two blocks (a branch over one pair), heavy on vf first..first+11
		for (u32 k = 0; k < 6; k++)
			p.push_back(U(VMADD_U(mask::xyzw, first + k, first + k + 6, first + ((k + 1) % 6))));
		p.push_back(L(VIBEQ_L(vi::vi7, vi::vi0, 1)));
		p.push_back(U(VMUL_U(mask::xyz, first + 2, first + 8, first + 9)));
		p.push_back(U(VADD_U(mask::xyzw, first + 3, first + 3, first + 10)));
		p.push_back(P(VISUBIU_L(counter, counter, 1), VADDw_U(mask::xyzw, first + 4, first + 4, first + 11)));
		const s32 at = static_cast<s32>(p.size());
		p.push_back(L(VIBNE_L(counter, vi::vi0, static_cast<s16>(head - at - 1))));
		p.push_back(U(VMUL_U(mask::xyzw, first + 5, first + 5, first + 7)));
	};
	p.push_back(U(VADD_U(mask::xyzw, vf::vf1, vf::vf2, vf::vf3)));
	loop(vi::vi1, 1, 5);
	p.push_back(U(VMUL_U(mask::xyzw, vf::vf25, vf::vf1, vf::vf13)));
	loop(vi::vi2, 13, 5);
	p.push_back(U(VADD_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf25)));
	p.push_back(EBit(U(VMUL_U(mask::xyzw, vf::vf26, vf::vf2, vf::vf14))));
	int failures = 0;
	for (bool fh : {true, false})
		for (u32 budget : {0u, 1u, 2u, 5u, 9u, 13u, 20u, 31u, 47u, 60u, 75u, 90u, 120u, 160u})
		{
			const RegionOutcome o = RunRegion(p, fh, [](VuTestHarness& h) {
				SeedSpecials(h, 23);
				h.SetVi(7, 1);
			}, budget == 0, 14, false, nullptr, budget);
			EXPECT_GT(o.regions, 0u);
			if (!o.diffs.empty() && ++failures <= 4)
				ADD_FAILURE() << "flagHack=" << fh << " budget " << budget << "\n" << Join(o.diffs);
		}
	EXPECT_EQ(failures, 0);
}

// Milestone 6: VI values known at compile time fold, memory ops at constant
// addresses address VU memory directly (wrapping like microVU), a VI copy
// reads its source, a second LQ of the same address reuses the first, and an
// LQ after a full-mask SQ to the same address reads the stored register.
static void ViConstantsAndAddressesBody();
TEST(VuoptLowering, ViConstantsAndAddresses) { ViConstantsAndAddressesBody(); }
static void ViConstantsAndAddressesBody()
{
	std::vector<std::vector<VuOp>> progs;
	// Folded arithmetic feeding constant addresses, including ones that wrap.
	progs.push_back({L(VIADDIU_L(vi::vi1, vi::vi0, 0x3fe)), L(VIADDI_L(vi::vi2, vi::vi1, 5)),
		L(VISUBIU_L(vi::vi3, vi::vi0, 3)), L(VIADD_L(vi::vi4, vi::vi1, vi::vi2)),
		L(VLQ_L(mask::xyzw, vf::vf1, vi::vi1, 4)), L(VLQ_L(mask::y | mask::w, vf::vf2, vi::vi3, 0)),
		L(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0)), L(VSQ_L(mask::x, vf::vf6, vi::vi0, -1)),
		L(VILW_L(mask::z, vi::vi5, vi::vi4, 2)), L(VISW_L(mask::y, vi::vi4, vi::vi3, 1)),
		L(VIAND_L(vi::vi6, vi::vi1, vi::vi3)), L(VIOR_L(vi::vi7, vi::vi6, vi::vi2)),
		L(VISUB_L(vi::vi8, vi::vi7, vi::vi1)), L(VMFIR_L(mask::xyzw, vf::vf7, vi::vi8)),
		EBit(U(VNOP_U()))});
	// Copies: IADDIU 0 and IADD with vi0, then the source and the copy written.
	progs.push_back({L(VIADDIU_L(vi::vi1, vi::vi5, 0)), L(VIADD_L(vi::vi2, vi::vi0, vi::vi6)),
		L(VIADDI_L(vi::vi5, vi::vi5, 1)), L(VLQ_L(mask::xyzw, vf::vf1, vi::vi1, 0)),
		L(VIADD_L(vi::vi3, vi::vi2, vi::vi1)), L(VIADDI_L(vi::vi2, vi::vi2, -1)),
		L(VSQ_L(mask::xyzw, vf::vf1, vi::vi2, 0)), L(VMFIR_L(mask::x, vf::vf2, vi::vi3)),
		EBit(U(VNOP_U()))});
	// LQ reuse and SQ forwarding, with partial masks and an intervening store.
	progs.push_back({L(VLQ_L(mask::xyzw, vf::vf1, vi::vi5, 2)), L(VLQ_L(mask::xyzw, vf::vf2, vi::vi5, 2)),
		L(VLQ_L(mask::x | mask::z, vf::vf3, vi::vi5, 2)), U(VADD_U(mask::xyzw, vf::vf4, vf::vf1, vf::vf2)),
		L(VSQ_L(mask::xyzw, vf::vf4, vi::vi6, 0)), L(VLQ_L(mask::xyzw, vf::vf8, vi::vi6, 0)),
		L(VSQ_L(mask::y, vf::vf9, vi::vi6, 0)), L(VLQ_L(mask::xyzw, vf::vf10, vi::vi6, 0)),
		L(VSQ_L(mask::xyzw, vf::vf11, vi::vi7, 0)), L(VLQ_L(mask::xyzw, vf::vf12, vi::vi5, 2)),
		L(VLQ_L(mask::xyzw, vf::vf13, vi::vi7, 0)), EBit(U(VNOP_U()))});
	// Constant addresses across blocks and a branch on folded values.
	progs.push_back({L(VIADDIU_L(vi::vi1, vi::vi0, 7)), L(VIADDIU_L(vi::vi2, vi::vi0, 7)),
		L(VIBEQ_L(vi::vi1, vi::vi2, 2)), U(VNOP_U()), L(VSQ_L(mask::xyzw, vf::vf3, vi::vi0, 9)),
		L(VLQ_L(mask::xyzw, vf::vf4, vi::vi1, 0)), L(VSQ_L(mask::xyzw, vf::vf4, vi::vi2, 3)),
		EBit(U(VNOP_U()))});
	for (size_t k = 0; k < progs.size(); k++)
		for (u32 seed : {3u, 4u})
			for (bool fh : {true, false})
			{
				const Outcome o = RunBoth(progs[k], fh, [seed](VuTestHarness& h) { SeedSpecials(h, seed + 70); });
				EXPECT_TRUE(o.diffs.empty()) << "program " << k << " seed " << seed << " flagHack=" << fh << "\n" << Join(o.diffs);
				EXPECT_EQ(o.lowered, o.blocks) << o.declined;
			}
}

// Milestone 7: a result's MAC and status packs share one sign/zero vector,
// the pack weights load once per block, and FSAND computes only the status
// bits its immediate keeps. Every FSAND immediate shape over status words
// with sticky, divide and single-lane bits set.
static void FlagPacksAndFsandBody();
TEST(VuoptLowering, FlagPacksAndFsand) { FlagPacksAndFsandBody(); }
static void FlagPacksAndFsandBody()
{
	static const u32 kImm[] = {0x000, 0xfff, 0x001, 0x002, 0x004, 0x008, 0x010, 0x020, 0x040, 0x080, 0x100, 0x200,
		0x400, 0x800, 0x0c3, 0xf3c, 0x041, 0x082, 0x030, 0x830, 0x3c3, 0x0ff, 0xf00, 0x5a5};
	int k = 0;
	for (u32 imm : kImm)
	{
		std::vector<VuOp> p;
		p.push_back(U(VSUB_U(mask::xyzw, vf::vf5, vf::vf1, vf::vf1)));  // zeros
		p.push_back(U(VMULy_U(mask::y, vf::vf6, vf::vf2, vf::vf3)));    // single lane
		p.push_back(P(VDIV_L(vf::vf4, 0, vf::vf0, 0), VADDx_U(mask::x | mask::w, vf::vf7, vf::vf3, vf::vf2)));
		p.push_back(L(VFSAND_L(vi::vi1, imm)));
		p.push_back(P(VFMAND_L(vi::vi2, vi::vi15), VADD_U(mask::xyzw, vf::vf8, vf::vf8, vf::vf3)));
		p.push_back(U(VNOP_U()));
		p.push_back(L(VFSAND_L(vi::vi3, imm)));
		p.push_back(L(VFSAND_L(vi::vi4, imm ^ 0xfff)));
		p.push_back(U(VNOP_U()));
		p.push_back(L(VFSAND_L(vi::vi5, imm)));
		p.push_back(L(VFMOR_L(vi::vi6, vi::vi0)));
		p.push_back(EBit(L(VFSAND_L(vi::vi7, imm))));
		for (u32 seed : {5u, 6u})
			for (bool fh : {true, false})
			{
				const Outcome o = RunBoth(p, fh, [seed](VuTestHarness& h) {
					SeedSpecials(h, seed + 90);
					h.SetVi(15, 0xffff);
				});
				EXPECT_TRUE(o.diffs.empty()) << "imm " << imm << " seed " << seed << " flagHack=" << fh << "\n" << Join(o.diffs);
				EXPECT_EQ(o.lowered, o.blocks) << o.declined;
			}
		k++;
	}
	EXPECT_EQ(k, static_cast<int>(std::size(kImm)));
}

// Clamp modes 2/3: MADD (to Fd) reads ACC through microVU's read-only
// mapping and clamps that cached register in place. The clamped copy is what
// the rest of the block reads, and it reaches ACC's home when the block writes
// ACC before or after; a clean cached copy is dropped.
TEST(VuoptLowering, MaddClampsCachedAcc)
{
	const auto seed = [](VuTestHarness& h) {
		h.SetVfBits(1, 0x7f800000, 0xff800000, 0x7fffffff, 0x3f800000);
		h.SetVfBits(2, 0x3f800000, 0x40000000, 0x7f7fffff, 0xff7fffff);
		h.SetVfBits(3, 0x7fc00000, 0x40400000, 0xbf800000, 0x7f800001);
		h.SetVfBits(4, 0x7f900000, 0x3f000000, 0xffffffff, 0x41200000);
	};
	std::vector<std::vector<VuOp>> progs;
	// ACC written first (dirty), then MADD: the clamp reaches ACC.
	progs.push_back({U(VMULA_U(mask::xyzw, vf::vf1, vf::vf2)), U(VMADD_U(mask::xyz, vf::vf5, vf::vf3, vf::vf2)),
		EBit(U(VNOP_U()))});
	// ACC never written: the clamp is dropped.
	progs.push_back({U(VMADD_U(mask::xyzw, vf::vf5, vf::vf3, vf::vf2)), EBit(U(VMADDx_U(mask::y, vf::vf6, vf::vf1, vf::vf2)))});
	// A partial ACC write after the MADD merges into the clamped copy.
	progs.push_back({U(VMADD_U(mask::xyzw, vf::vf5, vf::vf3, vf::vf2)), U(VMULA_U(mask::x | mask::z, vf::vf2, vf::vf2)),
		EBit(U(VNOP_U()))});
	// Single-lane MADD clamps its lane only; a later full read sees it.
	progs.push_back({U(VMADDy_U(mask::z, vf::vf5, vf::vf3, vf::vf4)), U(VMADDA_U(mask::xyzw, vf::vf2, vf::vf2)),
		EBit(U(VNOP_U()))});
	// OPMULA partial after the MADD, and an MSUB reading the clamped copy.
	progs.push_back({U(VMULA_U(mask::w, vf::vf4, vf::vf2)), U(VMADD_U(mask::x, vf::vf5, vf::vf3, vf::vf1)),
		U(VOPMULA_U(mask::x | mask::y, vf::vf3, vf::vf4)), EBit(U(VMSUB_U(mask::xyzw, vf::vf6, vf::vf2, vf::vf3)))});
	for (int mode : {2, 3})
	{
		g_clampMode = mode;
		for (size_t k = 0; k < progs.size(); k++)
			for (bool fh : {true, false})
			{
				const Outcome o = RunBoth(progs[k], fh, [&](VuTestHarness& h) {
					seed(h);
					h.SetAccBits(0x7f800000, 0xff800000, 0x7fffffff, 0xffc00001);
				});
				EXPECT_TRUE(o.diffs.empty()) << "mode " << mode << " program " << k << " flagHack=" << fh << "\n" << Join(o.diffs);
				EXPECT_EQ(o.lowered, o.blocks) << o.declined;
			}
		g_clampMode = -1;
	}
}

// Every lowering and region test again under clamp modes 0, 2 and 3: mode 0
// clamps nothing, modes 2 and 3 clamp every FMAC step's operands (mode 3
// sign-preservingly) and mode 2 its results, and at both MADD clamps
// microVU's cached ACC in place.
TEST(VuoptLowering, ClampModes)
{
	for (int mode : {0, 2, 3})
	{
		SCOPED_TRACE("clamp mode " + std::to_string(mode));
		g_clampMode = mode;
		FmacFormsOverSpecialsBody();
		ConversionsClipAndClipReadersBody();
		DivideUnitAndEfuBody();
		MovesMemoryRandomAndKickBody();
		FuzzBody();
		PressureFuzzBody();
		RegionCountedLoopBody();
		RegionLoopFuzzBody();
		RegionInvariantMatrixBody();
		ViConstantsAndAddressesBody();
		FlagPacksAndFsandBody();
		RegionBudgetSweepBody();
		RegionLoopMapsBody();
		g_clampMode = -1;
	}
}

// Milestone 9: the lowering suites again with every body list-scheduled
// (operations reordered by latency before the allocator sees them), small
// register pools included.
TEST(VuoptLowering, Scheduled)
{
	g_schedule = true;
	FmacFormsOverSpecialsBody();
	ConversionsClipAndClipReadersBody();
	DivideUnitAndEfuBody();
	MovesMemoryRandomAndKickBody();
	FuzzBody();
	PressureFuzzBody();
	ViConstantsAndAddressesBody();
	FlagPacksAndFsandBody();
	g_schedule = false;
}

// Debugging aid: VUOPT_REPRO_HEX="lower:upper,..." VUOPT_REPRO_SEED=n
// VUOPT_REPRO_FH=0|1 runs one program (as the pressure fuzz seeds it) with
// the lowered code dumped.
TEST(VuoptLowering, Repro)
{
	const char* hex = getenv("VUOPT_REPRO_HEX");
	if (!hex)
		GTEST_SKIP();
	std::vector<VuOp> p;
	for (const char* c = hex; *c;)
	{
		VuOp op;
		op.lower = static_cast<u32>(std::strtoul(c, const_cast<char**>(&c), 16));
		c++;
		op.upper = static_cast<u32>(std::strtoul(c, const_cast<char**>(&c), 16));
		if (*c == ',')
			c++;
		p.push_back(op);
	}
	const int seed = getenv("VUOPT_REPRO_SEED") ? std::atoi(getenv("VUOPT_REPRO_SEED")) : 5000;
	const bool fh = !getenv("VUOPT_REPRO_FH") || std::atoi(getenv("VUOPT_REPRO_FH"));
	const auto setup = [seed](VuTestHarness& h) {
		SeedSpecials(h, seed);
		for (int r = 12; r < 32; r++)
			h.SetVf(r, 1.0f + r, -0.5f * r, 0.25f + r, 3.0f - r);
	};
	const Outcome o = RunBoth(p, fh, setup, 1, true);
	EXPECT_TRUE(o.diffs.empty()) << Join(o.diffs, 20);
}

} // namespace recompiler_tests

#endif
