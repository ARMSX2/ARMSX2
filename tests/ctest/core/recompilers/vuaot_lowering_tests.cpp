// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// vu-aot: block bodies lowered from vu-aot's IR, run inside
// microVU's framing through the test-only body hook, must leave exactly the
// state microVU's own bodies leave. Every test runs a program twice from the
// same entry state -- microVU alone, then with every block body vu-aot's --
// and compares all of VURegs, VU memory and the GIF output byte for byte,
// with the flag hack on and off.

#if defined(_M_ARM64) || defined(__aarch64__)

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "arm64/vuaot/VuAotMvuTrace.h"
#include "arm64/vuaot/VuAotDecode.h"
#include "arm64/vuaot/VuAotTier.h"
#include "arm64/vuaot/VuAotRegion.h"

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
// Whether framed bodies are list-scheduled (regions always are).
bool g_schedule = false;
// Regions without the sticky STATUS accumulation (debugging a failure).
bool g_noStickyAcc = getenv("VUAOT_NO_ACC") != nullptr;
bool g_merge = true; // RegionTier::merge (variant merging)
bool g_wMem = true;  // RegionTier::wMemHomes
bool g_mergeIgnoreLinks = false; // RegionTier::mergeIgnoreLinksMutation
int g_dropStickyMutation = 0; // relaxation 3 broken on purpose (RegionTier::dropStickyMutation)
// A deliberate break of the hand-off liveness rule, to see the tests notice it
// (VuAotRegion.HandoffMutationsAreCaught); None everywhere else.
vuaot::RegionTier::HandoffMutation g_handoffMutation = getenv("VUAOT_HANDOFF_MUTATION")
	? static_cast<vuaot::RegionTier::HandoffMutation>(std::atoi(getenv("VUAOT_HANDOFF_MUTATION")))
	: vuaot::RegionTier::HandoffMutation::None;

struct TierScope
{
	vuaot::BodyTier tier;
	explicit TierScope(int vu)
		: tier(vu, vuRegs[vu].Micro)
	{
		vuaot::g_vuAotBodyHook = [](const vuaot::MvuBlockTrace& b, const vuaot::MvuEmitEnv& env, void* user) {
			return static_cast<vuaot::BodyTier*>(user)->TryLower(b, env, nullptr);
		};
		vuaot::g_vuAotBodyHookUser = &tier;
	}
	~TierScope()
	{
		vuaot::g_vuAotBodyHook = nullptr;
		vuaot::g_vuAotBodyHookUser = nullptr;
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
	const std::vector<u8>& pa, const std::vector<u8>& pb, bool rings = true, bool stickyRelax = false)
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
		if (!rings && std::strncmp(f.name, "micro_", 6) == 0)
			continue; // the four-deep flag copies, at a budget stop (the contract: only what the continuation reads)
		for (size_t i = 0; i < f.size; i += 4)
		{
			const size_t n = std::min<size_t>(4, f.size - i);
			// ... and the STATUS, MAC and CLIP registers a stop mirrors from them (VI 16-18)
			if (!rings && std::strcmp(f.name, "VI") == 0 && i >= REG_STATUS_FLAG * sizeof(REG_VI) && i < (REG_CLIP_FLAG + 1) * sizeof(REG_VI))
				continue;
			if (std::memcmp(x + f.off + i, y + f.off + i, n) == 0)
				continue;
			u32 va = 0, vb = 0;
			std::memcpy(&va, x + f.off + i, n);
			std::memcpy(&vb, y + f.off + i, n);
			// Relaxation 3: the sticky bits of STATUS (bits 6-7 and 10-11 of the register, the low byte
			// and bits 24-25 of its ring copies); the rest of those words stays pinned.
			if (stickyRelax)
			{
				u32 mask = 0;
				if (std::strcmp(f.name, "VI") == 0 && i == REG_STATUS_FLAG * sizeof(REG_VI))
					mask = 0xCC0;
				else if (std::strcmp(f.name, "micro_statusflags") == 0)
					mask = 0x30000FF;
				if (mask && ((va ^ vb) & ~mask) == 0)
					continue;
			}
			char buf[96];
			std::snprintf(buf, sizeof(buf), "%s+%zu: mvu=%08x vuaot=%08x", f.name, i, va, vb);
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

// Runs `prog` on VU1 through microVU, then with vu-aot bodies, each in a fresh
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

// An FMAC with an empty destination field (xyzw = 0). It writes no VF lane and no ACC lane, and still
// writes the flags: the MAC word is all clear and STATUS's current ZSUO bits are clear, the sticky
// bits kept (the interpreter's VU_MACx_CLEAR and VU_STAT_UPDATE). Fd is vf24..vf27, which no generator
// reads, unless liveFd draws it from vf1..vf8. The ACC forms have no Fd.
u32 EmptyFieldFmac(std::mt19937& rng, bool liveFd = false)
{
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	const u32 fd = liveFd ? 1 + R(8) : 24 + R(4), fs = 1 + R(8), ft = 1 + R(8);
	switch (R(15))
	{
		case 0: return VADD_U(0, fd, fs, ft);
		case 1: return VSUB_U(0, fd, fs, ft);
		case 2: return VMUL_U(0, fd, fs, ft);
		case 3: return VMADD_U(0, fd, fs, ft);
		case 4: return VMSUB_U(0, fd, fs, ft);
		case 5: return VMAX_U(0, fd, fs, ft);
		case 6: return VMINI_U(0, fd, fs, ft);
		case 7: return VMULA_U(0, fs, ft);
		case 8: return VMADDA_U(0, fs, ft);
		case 9: return VMULx_U(0, fd, fs, ft) + R(4);
		case 10: return VMADDx_U(0, fd, fs, ft) + R(4);
		case 11: return VMULAx_U(0, fs, ft) + R(4);
		case 12: return VMADDAx_U(0, fs, ft) + R(4);
		case 13: return VMULq_U(0, fd, fs);
		default: return VOPMSUB_U(0, fd, fs, ft);
	}
}
} // namespace

// A branch reading a VI the previous block's delay slot overwrote reads the old value from
// microVU's VI backup slot, not from the VI's home: the body's write-back must not store the
// new value into the home before the branch reads the old one.
TEST(VuAotLowering, ViBackupSurvivesHomeWriteback)
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
TEST(VuAotLowering, FmacFormsOverSpecials) { FmacFormsOverSpecialsBody(); }
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
TEST(VuAotLowering, ConversionsClipAndClipReaders) { ConversionsClipAndClipReadersBody(); }
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
TEST(VuAotLowering, DivideUnitAndEfu) { DivideUnitAndEfuBody(); }
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
TEST(VuAotLowering, MovesMemoryRandomAndKick) { MovesMemoryRandomAndKickBody(); }
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
// pairs, flag readers and writers, the divide unit and the EFU. Blocks vu-aot
// declines (a branch in a delay slot) run in microVU; everything else is
// vu-aot's, and the result has to be byte for byte microVU's.
static void FuzzBody();
TEST(VuAotLowering, Fuzz) { FuzzBody(); }
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
					vuaot::InstPair ip;
					ip.pc = static_cast<u32>(k * 8);
					ip.upper = vuaot::DecodeUpper(p[k].upper);
					ip.lower = vuaot::DecodeLower(p[k].lower);
					vuaot::FormatPair(ip, buf, sizeof(buf));
					listing += std::string(buf) + "\n";
				}
				ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " declined: " << o.declined << "\n" << Join(o.diffs, 6)
							  << (getenv("VUAOT_LISTING") ? listing : std::string());
				if (getenv("VUAOT_BISECT"))
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
	// The rest are blocks around a branch in a delay slot, which vu-aot declines.
	EXPECT_GT(lowered, blocks * 8 / 10) << lowered << " of " << blocks << " blocks lowered";
	std::printf("[vuaot lowering fuzz] programs=%d blocks=%u lowered=%u\n", programs, blocks, lowered);
}

// A block ending in JR whose target VI is also that VI's final value, under
// enough general-register pressure that the value is evicted before the end.
// It must not take the early write-back to its home: the end of the block
// still reads it as the jump target.
TEST(VuAotLowering, JumpTargetSurvivesEarlyWriteback)
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
TEST(VuAotLowering, PressureFuzz) { PressureFuzzBody(); }
static void PressureFuzzBody()
{
	std::mt19937 rng(20261014);
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	auto vf = [&]() { return 1 + R(31); };
	auto vi = [&]() { return 1 + R(6); };
	auto msk = [&]() { return (R(15) + 1) << 21; };
	const int iters = getenv("VUAOT_PRESSURE_ITERS") ? std::atoi(getenv("VUAOT_PRESSURE_ITERS")) : 60;
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
				if (getenv("VUAOT_SHRINK"))
				{
					// Delete pairs, then NOP out halves, while the program still fails.
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
						vuaot::InstPair ip;
						ip.pc = static_cast<u32>(k * 8);
						ip.upper = vuaot::DecodeUpper(q[k].upper);
						ip.lower = vuaot::DecodeLower(q[k].lower);
						vuaot::FormatPair(ip, buf, sizeof(buf));
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
	std::printf("[vuaot pressure fuzz] programs=%d blocks=%u lowered=%u declined: %s\n", programs, blocks, lowered, lastDeclined.c_str());
}

// Regions: microVU's framing replaced between vu-aot's own blocks, state in
// register homes across links.

namespace
{
struct RegionScope
{
	vuaot::RegionTier region;
	vuaot::BodyTier body;
	explicit RegionScope(int vu)
		: region(vu, vuRegs[vu].Micro)
		, body(vu, vuRegs[vu].Micro)
	{
		vuaot::g_vuAotRegionHook = [](const vuaot::MvuBlockTrace& b, const vuaot::MvuEmitEnv& env, void* user) {
			return static_cast<RegionScope*>(user)->region.OnCompile(b, env, nullptr);
		};
		vuaot::g_vuAotRegionHookUser = this;
		vuaot::g_vuAotBodyHook = [](const vuaot::MvuBlockTrace& b, const vuaot::MvuEmitEnv& env, void* user) {
			return static_cast<RegionScope*>(user)->body.TryLower(b, env, nullptr);
		};
		vuaot::g_vuAotBodyHookUser = this;
	}
	~RegionScope()
	{
		vuaot::g_vuAotRegionHook = nullptr;
		vuaot::g_vuAotRegionHookUser = nullptr;
		vuaot::g_vuAotBodyHook = nullptr;
		vuaot::g_vuAotBodyHookUser = nullptr;
	}
};

struct RegionOutcome
{
	std::vector<std::string> diffs;
	u32 regions = 0, nodes = 0;
	u32 merged = 0;  // nodes that ran another node's body (variant merging)
	u32 wMem = 0;    // vector homes whose w lane stayed in memory
	u64 bytes = 0;   // region code
	u32 accRegions = 0, accNonLinear = 0, accAccumulated = 0; // sticky STATUS accumulation
	u32 handoffEdges = 0, handoffDead = 0; // links into loop heads, and the ring instances their bails leave out
	u32 stickyDropped = 0;                 // regions compiled without the sticky byte of STATUS (relaxation 3)
	u32 statusReaders = 0, stickyReaders = 0, stickyDivReaders = 0; // the census of FSAND/FSEQ/FSOR in the regions
	bool stickyDiffers = false;            // the end states differ in exactly the sticky bits relaxation 3 frees
	bool divStickyDiffers = false;         // ... and among them the sticky I / D bits (STATUS bits 10-11) a divide sets
};

// microVU alone against vu-aot regions (and framed bodies elsewhere), running
// past the cycle budget so budget breaks and resumes happen.
RegionOutcome RunRegion(const std::vector<VuOp>& prog, bool flagHack, const std::function<void(VuTestHarness&)>& setup,
	bool resume = true, s32 vHomes = 14, bool dump = false, const std::map<u32, std::vector<u32>>* jumps = nullptr, u32 budget = 0)
{
	FlagHackScope fh(flagHack);
	// The exactness contract: when a budget cuts a run short, the four-deep flag copies the
	// engines leave need not be the same (only the ones the resumed code reads matter), so the
	// state at the stop is compared without them, and the run is then re-entered, as a game
	// re-dispatches it, to its end, where everything is compared, the copies included.
	const bool observe = !resume; // (a run without a resume can stop at the harness's default budget too)
	struct Arm
	{
		VuSnapshot stop, fin;
		std::vector<u8> stopPath, finPath;
		bool stopped = false;
		bool finStopped = false; // still running after the continuation (a program that does not end)
	};
	const auto runArm = [&](VuTestHarness& h) {
		Arm a;
		h.RunNoDiff();
		if (!observe)
		{
			a.stop = h.JitSnapshot();
			a.stopPath = h.Path1PacketBytesJit();
			a.fin = a.stop;
			a.finPath = a.stopPath;
			return a;
		}
		h.RunJitPreserveBlockCache();
		a.stop = h.JitSnapshot();
		a.stopPath = h.Path1PacketBytesJit();
		a.stopped = !h.HasTerminated();
		h.ContinueJit(100000, 400);
		a.finStopped = !h.HasTerminated();
		a.fin = h.JitSnapshot();
		a.finPath = h.Path1PacketBytesJit();
		return a;
	};
	Arm ref;
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
		ref = runArm(h);
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
	Arm got;
	{
		RegionScope rs(1);
		rs.region.maxVHomes = vHomes;
		rs.region.stickyAcc = !g_noStickyAcc;
		rs.region.accAlways = true; // (the estimate would decline many of these small programs)
		rs.region.handoffMutation = g_handoffMutation;
		rs.region.dropStickyMutation = g_dropStickyMutation;
		rs.region.dump = dump;
		rs.region.merge = g_merge;
		rs.region.wMemHomes = g_wMem;
		rs.region.mergeIgnoreLinksMutation = g_mergeIgnoreLinks;
		if (jumps)
			rs.region.jumpTargets = *jumps;
		got = runArm(h);
		o.regions = rs.region.Regions();
		o.nodes = rs.region.Nodes();
		o.merged = rs.region.Merged();
		o.wMem = rs.region.WMemHomes();
		o.bytes = rs.region.Bytes();
		o.accRegions = rs.region.AccRegions();
		o.accNonLinear = rs.region.AccNonLinear();
		o.accAccumulated = rs.region.AccAccumulated();
		o.handoffEdges = rs.region.HandoffEdges();
		o.handoffDead = rs.region.HandoffRingsDead();
		o.stickyDropped = rs.region.StickyDropped();
		o.statusReaders = rs.region.StatusReaders();
		o.stickyReaders = rs.region.StickyReaders();
		o.stickyDivReaders = rs.region.StickyDivReaders();
	}
	const bool relax = o.stickyDropped > 0;
	if (observe)
	{
		if (ref.stopped != got.stopped)
			o.diffs.emplace_back("one engine stopped at the budget and the other did not");
		for (const std::string& d : Compare(ref.stop, got.stop, ref.stopPath, got.stopPath, !ref.stopped, relax))
			o.diffs.push_back("at the stop: " + d);
		if (ref.finStopped != got.finStopped)
			o.diffs.emplace_back("one engine ended and the other did not");
		for (const std::string& d : Compare(ref.fin, got.fin, ref.finPath, got.finPath, !ref.finStopped, relax))
			o.diffs.push_back("at the end: " + d);
		o.stickyDiffers = relax && o.diffs.empty() && !Compare(ref.fin, got.fin, ref.finPath, got.finPath, !ref.finStopped, false).empty();
		o.divStickyDiffers = o.stickyDiffers &&
			((ref.fin.regs.VI[REG_STATUS_FLAG].UL ^ got.fin.regs.VI[REG_STATUS_FLAG].UL) & 0xC00) != 0;
	}
	else
		o.diffs = Compare(ref.stop, got.stop, ref.stopPath, got.stopPath, true, relax);
	return o;
}
} // namespace

// A counted loop: FMACs, a flag reader, a divide whose Q crosses the link,
// a store; runs long enough to break on the cycle budget several times.
static void RegionCountedLoopBody();
TEST(VuAotRegion, CountedLoop) { RegionCountedLoopBody(); }
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
TEST(VuAotRegion, RecordedJumpTargets)
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
// copies of the matrix rows live in registers for the whole region, filled
// once by the region's entry loader.
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
TEST(VuAotRegion, InvariantMatrix) { RegionInvariantMatrixBody(); }

static void RegionLoopFuzzBody();
TEST(VuAotRegion, LoopFuzz) { RegionLoopFuzzBody(); }
namespace
{
// One random loop program for RegionLoopFuzz: an outer counted loop (vi1) around random FMAC,
// flag, divide, memory and VI work with forward branches inside, and an inner counted loop (vi5)
// on some.
std::vector<VuOp> LoopFuzzProgram(std::mt19937& rng, std::mt19937& erng)
{
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	auto vf = [&]() { return 1 + R(14); };
	auto vi = [&]() { return 2 + R(3); }; // vi2..vi4; vi1 / vi5 are loop counters
	auto msk = [&]() { return (R(15) + 1) << 21; };
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
		if (up == VNOP_U() && erng() % 2 == 0)
			up = EmptyFieldFmac(erng);
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
	return p;
}

std::function<void(VuTestHarness&)> LoopFuzzSetup(int iter)
{
	return [iter](VuTestHarness& h) {
		SeedSpecials(h, 9000 + iter);
		for (int r = 4; r < 16; r++)
			h.SetVf(r, 1.0f + 0.01f * r, 0.99f, -1.0f + 0.001f * r, 0.5f);
		for (int r = 2; r <= 4; r++)
			h.SetVi(r, static_cast<u16>(r * 8));
	};
}

std::string Hex(const std::vector<VuOp>& p)
{
	std::string hex;
	for (const VuOp& op : p)
	{
		char buf[24];
		std::snprintf(buf, sizeof(buf), "%08x:%08x,", op.lower, op.upper);
		hex += buf;
	}
	return hex;
}
} // namespace

static void RegionLoopFuzzBody()
{
	std::mt19937 rng(20261015);
	std::mt19937 erng(20261016); // the empty-field FMACs draw from their own stream, so they do not change the rest of the program
	const int iters = getenv("VUAOT_REGION_ITERS") ? std::atoi(getenv("VUAOT_REGION_ITERS")) : 120;
	int failures = 0;
	u32 regions = 0, nodes = 0, merged = 0;
	for (int iter = 0; iter < iters; iter++)
	{
		const std::vector<VuOp> p = LoopFuzzProgram(rng, erng);
		for (bool fh : {true, false})
		{
			const RegionOutcome o = RunRegion(p, fh, LoopFuzzSetup(iter), false);
			regions += o.regions;
			nodes += o.nodes;
			merged += o.merged;
			if (!o.diffs.empty())
			{
				failures++;
				ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " hex=" << Hex(p) << "\n" << Join(o.diffs, 8);
			}
		}
	}
	EXPECT_EQ(failures, 0);
	EXPECT_GT(regions, static_cast<u32>(iters)); // most programs form a region per flag-hack setting
	std::printf("[vuaot region fuzz] programs=%d regions=%u nodes=%u merged=%u\n", iters * 2, regions, nodes, merged);
}

// Variant merging: a node whose IR, cycles and links equal another node's runs that node's body.
// Over the loop fuzz's programs, merging must leave every run identical to microVU, merge some
// nodes and shrink the code; the programs that merged run again under a range of cycle budgets,
// so runs stop and resume at merged nodes' own entries (each keeps its own bail record).
TEST(VuAotRegion, MergedVariants)
{
	struct Scope
	{
		~Scope() { g_merge = true; }
	} restore;
	std::mt19937 rng(20261015);
	std::mt19937 erng(20261016);
	int failures = 0;
	u32 merged = 0;
	u64 bytesOn = 0, bytesOff = 0;
	std::vector<std::pair<int, std::vector<VuOp>>> withMerges;
	for (int iter = 0; iter < 120; iter++)
	{
		const std::vector<VuOp> p = LoopFuzzProgram(rng, erng);
		for (bool fh : {true, false})
		{
			bool any = false;
			for (bool on : {false, true})
			{
				g_merge = on;
				const RegionOutcome o = RunRegion(p, fh, LoopFuzzSetup(iter), false);
				(on ? bytesOn : bytesOff) += o.bytes;
				if (on)
				{
					merged += o.merged;
					any = o.merged > 0;
				}
				else
					EXPECT_EQ(o.merged, 0u);
				if (!o.diffs.empty() && ++failures <= 4)
					ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " merge=" << on << " hex=" << Hex(p) << "\n" << Join(o.diffs, 8);
			}
			if (any && fh && withMerges.size() < 16)
				withMerges.push_back({iter, p});
		}
	}
	EXPECT_EQ(failures, 0);
	EXPECT_GT(merged, 0u);
	EXPECT_LT(bytesOn, bytesOff);
	ASSERT_FALSE(withMerges.empty());
	g_merge = true;
	for (const auto& [iter, p] : withMerges)
		for (bool fh : {true, false})
			for (u32 budget : {1u, 2u, 3u, 5u, 8u, 13u, 21u, 34u, 55u, 89u, 144u})
			{
				const RegionOutcome o = RunRegion(p, fh, LoopFuzzSetup(iter), false, 14, false, nullptr, budget);
				if (!o.diffs.empty() && ++failures <= 4)
					ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " budget " << budget << " hex=" << Hex(p) << "\n" << Join(o.diffs, 8);
			}
	EXPECT_EQ(failures, 0);
	std::printf("[vuaot merge] merged=%u bytes on=%llu off=%llu, %zu programs swept over budgets\n", merged,
		static_cast<unsigned long long>(bytesOn), static_cast<unsigned long long>(bytesOff), withMerges.size());
}

// Variants of one block with the same code whose successors are not the same must keep their
// own bodies. The program (found by a search over random diamonds) splits at pair 0 into two
// paths with different flag work, which meet at a join block (pair 14) through conditional links
// that are always taken; the join's two variants compile alike, but the pipeline states they hand
// on are not, and their successors are different code. Merging on the IR alone (a mutation) sends
// the run from the shared body on to one variant's successor whichever variant entered it, which
// this must notice.
TEST(VuAotRegion, MergeKeepsVariantsWhoseSuccessorsDiffer)
{
	struct Scope
	{
		~Scope() { g_mergeIgnoreLinks = false; }
	} restore;
	const std::vector<VuOp> p = {
		{0x50060006, 0x000002ff}, // 0
		{0x8000033c, 0x000002ff}, // 1
		{0x8000033c, 0x000002ff}, // 2
		{0x5000000a, 0x000002ff}, // 3
		{0x8000033c, 0x000002ff}, // 4
		{0x8000033c, 0x400002ff}, // 5
		{0x8000033c, 0x000002ff}, // 6
		{0x2c240067, 0x01a568ea}, // 7
		{0x8000033c, 0x000109ff}, // 8
		{0x38040000, 0x000002ff}, // 9
		{0x50000003, 0x000002ff}, // 10
		{0x8000033c, 0x000002ff}, // 11
		{0x8000033c, 0x400002ff}, // 12
		{0x8000033c, 0x000002ff}, // 13
		{0x34042800, 0x00c711ea}, // 14
		{0x8000033c, 0x000002ff}, // 15
		{0x2c04070d, 0x000002ff}, // 16
		{0x50000003, 0x000002ff}, // 17
		{0x8000033c, 0x000002ff}, // 18
		{0x8000033c, 0x400002ff}, // 19
		{0x8000033c, 0x000002ff}, // 20
		{0x8000033c, 0x000002ff}, // 21
		{0x8000033c, 0x000002ff}, // 22
		{0x8000033c, 0x400002ff}, // 23
	};
	u32 merged[2] = {}, caught = 0;
	for (bool mutate : {false, true})
		for (u16 path : {0, 1})
			for (bool fh : {true, false})
			{
				g_mergeIgnoreLinks = mutate;
				const RegionOutcome o = RunRegion(p, fh, [path](VuTestHarness& h) {
					SeedSpecials(h, 31);
					h.SetVi(6, path);
					h.SetVi(5, 0xffff);
				}, false, 14, getenv("VUAOT_TEST_DUMP") != nullptr);
				EXPECT_GE(o.regions, 1u);
				merged[mutate] += o.merged;
				if (!mutate)
					EXPECT_TRUE(o.diffs.empty()) << "path " << path << " flagHack=" << fh << "\n" << Join(o.diffs, 8);
				else
					caught += o.diffs.empty() ? 0 : 1;
			}
	EXPECT_LT(merged[0], merged[1]); // the links kept some variants apart
	EXPECT_GT(caught, 0u) << "a body shared by variants with different successors went unnoticed";
}

// A region tests the cycle budget only at loop heads and at
// entries from microVU, against what the run can spend before the next
// test, and continues a run that is short of it in microVU's own code. A
// single run under every budget from 1 up must stop exactly where microVU
// stops, with microVU's state.
static void RegionBudgetSweepBody();
TEST(VuAotRegion, BudgetSweep) { RegionBudgetSweepBody(); }
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

// Conditional branches and VI values across links, on the 16-bit edges: a
// counter stepping through 0x7fff -> 0x8000 (the sign of IBLTZ and friends)
// and 0xffff -> 0x0000 (a carry out of the low half), every branch kind,
// against VI0 and against another VI, the counter also stored (ISW / ISWR),
// read as a float (MFIR) and summed with IADD. Run to the end and under
// budgets that stop the loop at its head.
static void RegionViBranchEdgesBody();
TEST(VuAotRegion, ViBranchEdges) { RegionViBranchEdgesBody(); }
static void RegionViBranchEdgesBody()
{
	using BranchFn = u32 (*)(u32, s16);
	struct Kind
	{
		const char* name;
		bool two;
		BranchFn one;
	};
	const Kind kinds[] = {
		{"IBEQ", true, nullptr}, {"IBNE", true, nullptr},
		{"IBLTZ", false, [](u32 is, s16 off) { return VIBLTZ_L(is, off); }},
		{"IBGTZ", false, [](u32 is, s16 off) { return VIBGTZ_L(is, off); }},
		{"IBLEZ", false, [](u32 is, s16 off) { return VIBLEZ_L(is, off); }},
		{"IBGEZ", false, [](u32 is, s16 off) { return VIBGEZ_L(is, off); }},
	};
	int failures = 0, runs = 0;
	u32 regions = 0;
	for (const Kind& k : kinds)
		for (u32 start : {0x7ffbu, 0xfffbu, 0x0003u})
			for (u32 other : {0u, 2u})
			{
				if (!k.two && other)
					continue;
				std::vector<VuOp> p;
				// vi1 = start (ISUBIU from VI0 reaches the top half)
				if (start & 0x8000)
					p.push_back(L(VISUBIU_L(vi::vi1, vi::vi0, (0x10000u - start) & 0x7fff))); // 0
				else
					p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, start)));                        // 0
				p.push_back(L(VIADDIU_L(vi::vi3, vi::vi0, 11)));                                // 1
				p.push_back(L(VISUBIU_L(vi::vi2, vi::vi0, 0x7fff)));                            // 2: vi2 = 0x8001
				p.push_back(P(VIADDIU_L(vi::vi1, vi::vi1, 1), VADD_U(mask::xyzw, vf::vf1, vf::vf1, vf::vf2)));  // 3: head
				p.push_back(P(VIADD_L(vi::vi5, vi::vi5, vi::vi1), VMUL_U(mask::xyz, vf::vf3, vf::vf1, vf::vf2))); // 4
				const u32 cmp = other ? vi::vi2 : vi::vi0;
				const s16 fwd = 2; // -> 8
				if (k.two)
					p.push_back(L(std::string(k.name) == "IBEQ" ? VIBEQ_L(cmp, vi::vi1, fwd) : VIBNE_L(cmp, vi::vi1, fwd))); // 5
				else
					p.push_back(L(k.one(vi::vi1, fwd)));                                         // 5
				p.push_back(L(VISW_L(mask::x, vi::vi1, vi::vi3, 4)));                           // 6: delay slot
				p.push_back(P(VMFIR_L(mask::xyzw, vf::vf4, vi::vi1), VMULx_U(mask::w, vf::vf7, vf::vf1, vf::vf2)));  // 7: not taken
				p.push_back(P(VISUBIU_L(vi::vi3, vi::vi3, 1), VSUB_U(mask::xyzw, vf::vf5, vf::vf3, vf::vf1))); // 8
				p.push_back(L(VIBNE_L(vi::vi3, vi::vi0, static_cast<s16>(3 - 9 - 1))));          // 9 -> 3
				p.push_back(L(VISWR_L(mask::y, vi::vi1, vi::vi3)));                             // 10: delay slot
				p.push_back(L(VMFIR_L(mask::xyzw, vf::vf6, vi::vi5)));                          // 11
				p.push_back(EBit(L(VISW_L(mask::z, vi::vi5, vi::vi0, 40))));                    // 12
				for (bool fh : {true, false})
					for (u32 budget : {0u, 3u, 17u, 40u})
					{
						runs++;
						const RegionOutcome o = RunRegion(p, fh, [](VuTestHarness& h) {
							h.SetVf(1, 1.0f, 2.0f, 3.0f, 4.0f);
							h.SetVf(2, 0.5f, -0.5f, 0.25f, 2.0f);
							h.SetVi(5, 0xfff0);
						}, budget == 0, 14, false, nullptr, budget);
						regions += o.regions;
						if (!o.diffs.empty() && ++failures <= 6)
							ADD_FAILURE() << k.name << " start=" << std::hex << start << " other=" << other << std::dec
										  << " flagHack=" << fh << " budget=" << budget << "\n" << Join(o.diffs);
					}
			}
	EXPECT_EQ(failures, 0) << runs << " runs";
	EXPECT_GT(regions, static_cast<u32>(runs / 2));
}

// FMEQ compares the whole register microVU holds a VI in, and microVU holds a
// VI from an earlier block zero-extended: a counter carried across a link past
// 0xffff must compare as its low half (MAC is 0 here, so vi7 counts the trips
// where the counter's low half is 0).
static void RegionFmeqOnCarriedViBody();
TEST(VuAotRegion, FmeqOnCarriedVi) { RegionFmeqOnCarriedViBody(); }
static void RegionFmeqOnCarriedViBody()
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 0x7fff)));                                  // 0
	p.push_back(L(VIADD_L(vi::vi1, vi::vi1, vi::vi1)));                                   // 1: 0xfffe
	p.push_back(L(VIADDIU_L(vi::vi3, vi::vi0, 5)));                                       // 2
	p.push_back(P(VIADDIU_L(vi::vi1, vi::vi1, 1), VADD_U(mask::xyzw, vf::vf1, vf::vf1, vf::vf2))); // 3: head
	p.push_back(L(VISUBIU_L(vi::vi3, vi::vi3, 1)));                                       // 4
	p.push_back(U(VNOP_U()));                                                             // 5
	p.push_back(L(VIBEQ_L(vi::vi3, vi::vi0, 5)));                                         // 6 -> 12
	p.push_back(U(VNOP_U()));                                                             // 7: delay slot
	p.push_back(L(VFMEQ_L(vi::vi6, vi::vi1)));                                            // 8
	p.push_back(L(VIADD_L(vi::vi7, vi::vi7, vi::vi6)));                                   // 9
	p.push_back(L(VB_L(static_cast<s16>(3 - 10 - 1))));                                   // 10 -> 3
	p.push_back(U(VNOP_U()));                                                             // 11: delay slot
	p.push_back(EBit(U(VNOP_U())));                                                       // 12
	for (bool fh : {true, false})
	{
		const RegionOutcome o = RunRegion(p, fh, [](VuTestHarness& h) {
			h.SetVf(1, 1.0f, 2.0f, 3.0f, 4.0f);
			h.SetVf(2, 0.5f, 0.5f, 0.25f, 2.0f);
		});
		EXPECT_TRUE(o.diffs.empty()) << "flagHack=" << fh << "\n" << Join(o.diffs);
		EXPECT_GE(o.regions, 1u);
	}
}

// VI arithmetic across links: loops of random integer operations on vi2..vi6
// seeded at the 16-bit edges (0x7fff, 0x8000, 0xffff, ...), with VI loads and
// stores, MFIR, FMEQ / FMAND / FMOR, forward IBxx of every kind and the VI
// copies microVU folds (IADDIU x, y, 0), so values reach register homes with
// carries out of the low half and results are computed straight into the
// homes of their own operands. To the end and under budgets that stop runs at
// loop heads. FMEQ reads vi8 / vi9, which nothing writes: on a VI computed in
// the same block microVU's answer depends on whether its register allocator
// wrote the VI back and reloaded it (zero-extended) in between, which neither
// the interpreter nor vu-aot reproduces.
static void RegionViFuzzBody();
TEST(VuAotRegion, ViFuzz) { RegionViFuzzBody(); }
static void RegionViFuzzBody()
{
	std::mt19937 rng(20261016);
	const int iters = getenv("VUAOT_REGION_ITERS") ? std::atoi(getenv("VUAOT_REGION_ITERS")) : 150;
	int failures = 0, runs = 0;
	u32 regions = 0;
	for (int iter = 0; iter < iters; iter++)
	{
		auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
		auto vi = [&]() { return 2 + R(5); }; // vi2..vi6; vi1 counts the loop
		auto vf = [&]() { return 1 + R(8); };
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(2 + R(9)))));
		const size_t head = p.size();
		const int len = 4 + R(12);
		int lastBranch = -10;
		for (int i = 0; i < len; i++)
		{
			u32 lo = kNopL, up = VNOP_U();
			if (R(3) == 0)
				up = R(2) ? VADD_U(mask::xyzw, vf(), vf(), vf()) : VMUL_U((R(15) + 1) << 21, vf(), vf(), vf());
			switch (R(17))
			{
				case 0: lo = VIADD_L(vi(), vi(), vi()); break;
				case 1: lo = VISUB_L(vi(), vi(), vi()); break;
				case 2: lo = VIAND_L(vi(), vi(), vi()); break;
				case 3: lo = VIOR_L(vi(), vi(), vi()); break;
				case 4: lo = VIADDI_L(vi(), vi(), static_cast<s32>(R(32)) - 16); break;
				case 5: lo = VIADDIU_L(vi(), vi(), R(2) ? 0u : R(0x8000)); break;
				case 6: lo = VISUBIU_L(vi(), vi(), R(0x8000)); break;
				case 7: lo = VILW_L((R(15) + 1) << 21, vi(), vi(), static_cast<s16>(R(16))); break;
				case 8: lo = VISW_L((R(15) + 1) << 21, vi(), vi(), static_cast<s16>(R(16))); break;
				case 9: lo = VISWR_L((R(15) + 1) << 21, vi(), vi()); break;
				case 10: lo = VILWR_L(1u << (21 + R(4)), vi(), vi()); break;
				case 11: lo = VMFIR_L(mask::xyzw, vf(), vi()); break;
				case 12: lo = VFMEQ_L(vi(), 8 + R(2)); break; // vi8 / vi9: nothing writes them (see the comment above the test)
				case 13: lo = VFMAND_L(vi(), vi()); break;
				case 14: lo = VFMOR_L(vi(), vi()); break;
				case 15:
				case 16:
					if (i + 4 < len && i >= lastBranch + 3)
					{
						const s16 off = static_cast<s16>(1 + R(2));
						switch (R(6))
						{
							case 0: lo = VIBEQ_L(vi(), R(2) ? vi() : 0u, off); break;
							case 1: lo = VIBNE_L(vi(), R(2) ? vi() : 0u, off); break;
							case 2: lo = VIBLTZ_L(vi(), off); break;
							case 3: lo = VIBGTZ_L(vi(), off); break;
							case 4: lo = VIBLEZ_L(vi(), off); break;
							default: lo = VIBGEZ_L(vi(), off); break;
						}
						lastBranch = i;
					}
					break;
			}
			p.push_back(VuOp{lo, up});
		}
		p.push_back(U(VNOP_U()));
		p.push_back(L(VISUBIU_L(vi::vi1, vi::vi1, 1)));
		p.push_back(U(VNOP_U()));
		p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(static_cast<int>(head) - static_cast<int>(p.size()) - 1))));
		p.push_back(L(VIADD_L(vi::vi7, vi::vi2, vi::vi3)));
		p.push_back(EBit(L(VISW_L(mask::xyzw, vi::vi7, vi::vi0, 60))));
		const u16 seeds[] = {0x7fff, 0x8000, 0xffff, 0xfffe, 0x0001, 0x7ffe, 0x8001, 0x0000, 0x3ff, 0x400};
		const auto setup = [&](VuTestHarness& h) {
			for (int r = 2; r <= 6; r++)
				h.SetVi(r, seeds[(iter + r * 3) % 10]);
			h.SetVi(8, 0x0000);
			h.SetVi(9, seeds[iter % 10]);
			for (int r = 1; r <= 8; r++)
				h.SetVf(r, 1.0f + r, -0.5f * r, 0.0f, 0.25f * r);
		};
		for (bool fh : {true, false})
			for (u32 budget : {0u, 7u, 23u})
			{
				runs++;
				const RegionOutcome o = RunRegion(p, fh, setup, budget == 0, 14, false, nullptr, budget);
				regions += o.regions;
				if (!o.diffs.empty() && ++failures <= 4)
					ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " budget=" << budget << " hex=" << Hex(p) << "\n" << Join(o.diffs, 8);
			}
	}
	EXPECT_EQ(failures, 0) << runs << " runs";
	EXPECT_GT(regions, static_cast<u32>(runs / 2));
}

// A vector home whose w lane the region never writes keeps that lane in
// microVU's location (HomeMap::wMem): x, y and z writes go whole into the
// register, a node reading w (a broadcast, a whole-vector store) loads it
// first, and the boundary stores write x, y and z only. Two loops, the second
// without a w reader, so the maps differ on vf5 and the transfers between them
// go both ways; every budget from 1 so bails land everywhere; with the homes
// on and off.
static void RegionWLaneInMemoryBody();
TEST(VuAotRegion, WLaneInMemory) { RegionWLaneInMemoryBody(); }
static void RegionWLaneInMemoryBody()
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 6)));                                       // 0
	p.push_back(U(VMUL_U(mask::xyz, vf::vf5, vf::vf5, vf::vf2)));                          // 1: head 1
	p.push_back(U(VADD_U(mask::xyz, vf::vf6, vf::vf6, vf::vf5)));                          // 2
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMULw_U(mask::x, vf::vf7, vf::vf1, vf::vf5))); // 3: reads vf5.w
	p.push_back(U(VNOP_U()));                                                             // 4
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(1 - 5 - 1))));                // 5 -> 1
	p.push_back(U(VMUL_U(mask::xyz, vf::vf8, vf::vf5, vf::vf2)));                          // 6: delay slot
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 5)));                                       // 7
	p.push_back(U(VADD_U(mask::xyz, vf::vf5, vf::vf5, vf::vf3)));                          // 8: head 2
	p.push_back(U(VMADD_U(mask::xyz, vf::vf6, vf::vf5, vf::vf3)));                         // 9
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMUL_U(mask::x | mask::y, vf::vf8, vf::vf8, vf::vf5))); // 10
	p.push_back(U(VNOP_U()));                                                             // 11
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(8 - 12 - 1))));               // 12 -> 8
	p.push_back(U(VSUB_U(mask::xyz, vf::vf5, vf::vf5, vf::vf2)));                          // 13: delay slot
	p.push_back(L(VSQ_L(mask::xyzw, vf::vf5, vi::vi0, 0)));                               // 14: stores vf5.w
	p.push_back(EBit(U(VADD_U(mask::xyzw, vf::vf9, vf::vf5, vf::vf6))));                   // 15
	const auto setup = [](VuTestHarness& h) {
		h.SetVf(1, 1.0f, 2.0f, 3.0f, 4.0f);
		h.SetVf(2, 1.01f, 0.99f, 1.02f, -3.0f);
		h.SetVf(3, 0.25f, -0.5f, 0.125f, 9.0f);
		h.SetVf(5, 1.5f, 2.0f, 2.5f, 7.0f);
		h.SetVf(6, 0.5f, 0.5f, 0.5f, -2.0f);
		h.SetVf(8, 3.0f, 2.0f, 1.0f, 5.0f);
	};
	struct Scope
	{
		~Scope() { g_wMem = true; }
	} scope;
	int failures = 0;
	u32 wMem = 0;
	for (bool on : {true, false})
	{
		g_wMem = on;
		for (bool fh : {true, false})
			for (u32 budget = 0; budget <= 120; budget++)
			{
				const RegionOutcome o = RunRegion(p, fh, setup, budget == 0, 14, false, nullptr, budget);
				EXPECT_GT(o.regions, 0u);
				if (on)
					wMem += o.wMem;
				else
					EXPECT_EQ(o.wMem, 0u);
				if (!o.diffs.empty() && ++failures <= 4)
					ADD_FAILURE() << "wMem=" << on << " flagHack=" << fh << " budget=" << budget << "\n" << Join(o.diffs);
			}
	}
	EXPECT_EQ(failures, 0);
	EXPECT_GT(wMem, 0u) << "no home kept its w lane in memory";
}

// Two loops working on different vector registers get
// different home maps, so entering and leaving each loop moves variables
// between registers and memory (and fills clamped copies). Every budget
// from 1 up, so bails land inside each loop too.
static void RegionLoopMapsBody();
TEST(VuAotRegion, LoopMaps) { RegionLoopMapsBody(); }
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

// Sticky STATUS accumulation. With the flag hack off an FMAC ORs its Z/S lane
// bits into the sticky half of STATUS, but only the last few STATUS instances
// (and the ones a reader reads) can ever be observed; a region keeps the others'
// contribution in vector accumulators and builds an instance's word only where
// microVU, a reader or a program end can see it. A bail to microVU at a loop head
// hands over every live instance, so a run under every budget must stop with
// microVU's four STATUS instances, MAC and clip, bit for bit.
namespace
{
// Positive, non-zero registers (vf20 is the event vector the loop loads), and
// event vectors in memory from qword 0x40: 1.0 in every lane except, in about
// one vector in four, one lane that is 0.0 or -2.0. vi9 walks them.
void SeedSparse(VuTestHarness& h, u32 seed)
{
	std::mt19937 rng(seed);
	for (u32 r = 1; r < 32; r++)
		h.SetVf(r, 1.0f + 0.25f * (rng() % 7), 0.5f + 0.125f * (rng() % 9), 2.0f + (rng() % 5), 1.5f + 0.5f * (rng() % 3));
	const auto bits = [](float f) {
		u32 b;
		std::memcpy(&b, &f, 4);
		return b;
	};
	for (u32 t = 0; t < 128; t++)
	{
		float v[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		if (rng() % 4 == 0)
			v[rng() % 4] = (rng() & 1) ? 0.0f : -2.0f;
		h.WriteMemU128(0x400 + t * 16, bits(v[0]), bits(v[1]), bits(v[2]), bits(v[3]));
	}
	h.SetVi(9, 0x40);
}

// Values of both signs and zeros in every lane, so each FMAC's MAC word and each
// instance's zero and sign bits differ from its neighbours' (SeedSparse keeps most
// instances equal, which hides a stale one). The event vectors at qword 0x40 are
// drawn the same way.
void SeedMixed(VuTestHarness& h, u32 seed)
{
	static const float kPool[] = {1.0f, -1.0f, 2.5f, -0.5f, 0.0f, -0.0f, 3.0f, 0.25f, -4.0f, 7.5f};
	std::mt19937 rng(seed);
	const auto bits = [](float f) {
		u32 b;
		std::memcpy(&b, &f, 4);
		return b;
	};
	const auto pick = [&]() { return kPool[rng() % std::size(kPool)]; };
	for (u32 r = 1; r < 32; r++)
		h.SetVf(r, pick(), pick(), pick(), pick());
	for (u32 t = 0; t < 128; t++)
		h.WriteMemU128(0x400 + t * 16, bits(pick()), bits(pick()), bits(pick()), bits(pick()));
	h.SetVi(9, 0x40);
}

// One FMAC of the dense loops: k picks the op, the destination field and the
// operands. Fields cover every shape: full, xyz, pairs, single lanes, three
// lanes; results land in vf9..vf14 and feed later operands.
//
// `sparse`: the sources are vf1..vf8, all positive and non-zero, and vf20, the
// event vector the loop loads from memory each trip (SeedSparse): its lanes are
// 1.0 except for a zero or a -2.0 now and then. The results then have a zero or
// a sign bit only when an event passes through, so the sticky bits fill up
// slowly instead of at once and every FMAC's contribution can be told apart
// (random special values saturate all eight within a few FMACs, which hides a
// dropped one).
u32 StickyFmac(u32 k, bool sparse = false)
{
	static const u32 kMask[] = {mask::xyzw, mask::xyz, mask::x | mask::y, mask::z | mask::w, mask::w, mask::xyzw, mask::x | mask::z,
		mask::y | mask::z | mask::w, mask::y, mask::xyzw, mask::x | mask::y | mask::w, mask::z};
	const u32 m = kMask[k % std::size(kMask)];
	const u32 fd = 9 + (k % 6);
	u32 fs = 1 + (k * 3) % 14, ft = 1 + (k * 5 + 2) % 14;
	if (sparse)
	{
		fs = (k % 5 == 2) ? 20 : 1 + (k * 3) % 8;
		ft = (k % 3 == 0) ? 20 : 1 + (k * 5 + 2) % 8;
	}
	switch (k % 9)
	{
		case 0: return VADD_U(m, fd, fs, ft);
		case 1: return VMUL_U(m, fd, fs, ft);
		case 2: return VSUB_U(m, fd, fs, ft);
		case 3: return VMAX_U(m, fd, fs, ft);
		case 4: return VMADD_U(m, fd, fs, ft);
		case 5: return VMULx_U(m, fd, fs, ft);
		case 6: return VMINI_U(m, fd, fs, ft);
		case 7: return VMADDy_U(m, fd, fs, ft);
		default: return VMULA_U(m, fs, ft);
	}
}

// A counted loop of `body` pairs (the counter decrement and the branch ride in
// the last two), shaped by `variant`:
//   0 dense FMACs, nothing reads STATUS inside
//   1 FSAND and FMAND readers inside, so some instances are read
//   2 a divide merging its flags and an FSSET that clears the sticky bits
//   3 a branch over three pairs: the number of status writes per trip varies
//   4 sparse FMACs between integer-only pairs
//   5 an XGKICK (a C call: the accumulators and the chain word ride through it)
//   6 dense FMACs on sparse events (SeedSparse): the sticky bits arrive one at a time
//   7 sparse events, readers, and a branch over three pairs
//   8 sparse events, two blocks: ten FMACs, a branch, then exactly four FMACs ending the loop
std::vector<VuOp> StickyProgram(int variant, u32 trip, u32 body)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(trip))));
	p.push_back(L(VIADDIU_L(vi::vi7, vi::vi0, 1)));
	if (variant == 5)
		p.push_back(L(VIADDIU_L(vi::vi15, vi::vi0, 0x10)));
	if (variant == 8)
	{
		// the loop gets a block of its own: the region's walk enters it at its
		// head (a branch that is always taken, over a pair), so only the head
		// tests the budget
		p.push_back(P(VIBEQ_L(vi::vi0, vi::vi0, 2), VNOP_U()));
		p.push_back(U(VNOP_U())); // delay slot
		p.push_back(U(VNOP_U())); // skipped
	}
	const s32 head = static_cast<s32>(p.size());
	if (variant == 8)
	{
		// A, ten FMACs ending in a conditional branch over one pair (K), then B,
		// exactly four FMACs ending the loop. The last four instances are B's, so
		// A's and K's FMACs only accumulate and B's first instance has to fold
		// them in: the accumulators are owed across the link.
		p.push_back(VuOp{VLQI_L(mask::xyzw, vf::vf20, vi::vi9), StickyFmac(0, true)});
		for (u32 k = 1; k < 9; k++)
			p.push_back(U(StickyFmac(k, true)));
		p.push_back(P(VIAND_L(vi::vi6, vi::vi1, vi::vi7), StickyFmac(9, true)));
		p.push_back(P(VIBEQ_L(vi::vi6, vi::vi0, 2), StickyFmac(10, true)));
		p.push_back(U(StickyFmac(11, true))); // delay slot
		p.push_back(U(StickyFmac(28, true))); // K
		// (ADD, MUL, SUB and ADD: MAX and MINI do not write STATUS)
		p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), StickyFmac(18, true)));
		p.push_back(U(StickyFmac(19, true)));
		const s32 at = static_cast<s32>(p.size());
		p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(head - at - 1)), StickyFmac(20, true)));
		p.push_back(U(StickyFmac(27, true))); // delay slot
		p.push_back(U(StickyFmac(16, true)));
		p.push_back(EBit(P(VFSAND_L(vi::vi4, 0xfff), VNOP_U())));
		return p;
	}
	for (u32 k = 0; k < body; k++)
	{
		const bool sparse = variant >= 6;
		u32 up = StickyFmac(k + static_cast<u32>(variant) * 5, sparse), lo = kNopL;
		if (sparse && k == 0)
			lo = VLQI_L(mask::xyzw, vf::vf20, vi::vi9);
		switch (variant)
		{
			case 1:
				if (k == 5) lo = VFSAND_L(vi::vi3, 0x0c3);
				if (k == 9) lo = VFMAND_L(vi::vi5, vi::vi7);
				if (k == 13) lo = VFSAND_L(vi::vi4, 0xf3c);
				break;
			case 2:
				if (k == 3) lo = VDIV_L(vf::vf1, 0, vf::vf2, 1);
				if (k == 8) lo = VFSSET_L(0x0c3);
				if (k == 11) lo = VFSAND_L(vi::vi3, 0xfff);
				break;
			case 3:
				if (k == 4) lo = VIAND_L(vi::vi6, vi::vi1, vi::vi7);
				if (k == 5) lo = VIBEQ_L(vi::vi6, vi::vi0, 4); // taken: over pairs 7, 8, 9 (the delay slot, pair 6, still runs)
				if (k == 12) lo = VFSAND_L(vi::vi3, 0x0c3);
				break;
			case 4:
				if (k % 3 != 0) up = VNOP_U();
				if (k % 3 == 1) lo = VIADDIU_L(vi::vi8, vi::vi8, 1);
				if (k == 9) lo = VFSAND_L(vi::vi3, 0xfff);
				break;
			case 5:
				if (k == 7) lo = VXGKICK_L(vi::vi15);
				if (k == 13) lo = VFSAND_L(vi::vi3, 0x0c3);
				break;
			case 7:
				if (k == 3) lo = VIAND_L(vi::vi6, vi::vi1, vi::vi7);
				if (k == 4) lo = VIBEQ_L(vi::vi6, vi::vi0, 4);
				if (k == 9) lo = VFSAND_L(vi::vi3, 0xfc3);
				if (k == 12) lo = VFMAND_L(vi::vi5, vi::vi7);
				break;
			default:
				break;
		}
		p.push_back(VuOp{lo, up});
	}
	const bool sp = variant >= 6;
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), StickyFmac(body + 1, sp)));
	const s32 at = static_cast<s32>(p.size());
	p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(head - at - 1)), StickyFmac(body + 2, sp)));
	p.push_back(U(StickyFmac(body + 3, sp))); // delay slot
	p.push_back(U(StickyFmac(body + 4, sp)));
	p.push_back(EBit(P(VFSAND_L(vi::vi4, 0xfff), VNOP_U())));
	return p;
}
} // namespace

static void RegionStickyAccumulationBody();
TEST(VuAotRegion, StickyAccumulation) { RegionStickyAccumulationBody(); }
static void RegionStickyAccumulationBody()
{
	int failures = 0;
	u32 runs = 0, accRegions = 0, accumulated = 0;
	for (int variant = 0; variant <= 8; variant++)
	{
		const std::vector<VuOp> p = StickyProgram(variant, variant == 4 ? 12 : 7, 16);
		for (bool fh : {false, true})
			for (u32 budget = 1; budget <= 150; budget++)
			{
				const RegionOutcome o = RunRegion(p, fh, [variant](VuTestHarness& h) {
					if (variant >= 6)
						SeedSparse(h, 31 + variant);
					else
						SeedSpecials(h, 31 + variant);
					h.SetVi(8, 0);
					// GIF packet at qword 0x10, sent by variant 5's XGKICK
					h.WriteMemU128(0x100, 0x00008001, 0x10000000, 0x0000000e, 0);
					h.WriteMemU128(0x110, 0, 0, 0, 0);
				}, false, 14, false, nullptr, budget);
				runs++;
				if (!fh)
				{
					accRegions += o.accRegions;
					accumulated += o.accAccumulated;
				}
				EXPECT_GT(o.regions, 0u);
				if (!o.diffs.empty() && ++failures <= 4)
					ADD_FAILURE() << "variant " << variant << " flagHack=" << fh << " budget " << budget << "\n" << Join(o.diffs);
			}
	}
	EXPECT_EQ(failures, 0);
	// the point of the test: with the flag hack off the regions accumulate
	EXPECT_GT(accRegions, 0u);
	EXPECT_GT(accumulated, runs);
	std::printf("[vuaot sticky] runs=%u acc regions=%u accumulated updates=%u\n", runs, accRegions, accumulated);
}

// Accumulations made in one block are owed to the first instance built in the
// next: loops where the instance a link later hands over needs the sticky bits
// the block before only accumulated. A region that forgot the debt at a link
// (the dirty state of the dataflow) fails these and nothing smaller.
TEST(VuAotRegion, StickyFoldAcrossLinks)
{
	struct Case
	{
		const char* hex;
		int iter;
		u32 budget;
	};
	static const Case kCases[] = {
		{"10010007:000002ff,10070001:000002ff,81f44b7c:000002ff,50060003:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:01282398,50060002:000002ff,8000033c:0182a398,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:00872aec,8000033c:000002ff,12010801:000002ff,520107f1:000002ff,8000033c:000002ff,8000033c:000002ff,2c2407ff:400002ff,", 2261, 82},
		{"10010008:000002ff,10070001:000002ff,8000033c:000002ff,50060002:000002ff,8000033c:000002ff,34043800:01e44398,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,50060003:000002ff,8000033c:0087a2ec,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,50060001:000002ff,8000033c:000002ff,8000033c:004823ac,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,12010801:000002ff,520107e8:000002ff,8000033c:000002ff,8000033c:000002ff,2c2407ff:400002ff,", 4423, 0},
		{"1001000d:000002ff,10070001:000002ff,81f44b7c:000002ff,50060002:000002ff,8000033c:000002ff,2a0004ab:000002ff,8000033c:000002ff,800709b4:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,50060002:000002ff,8000033c:0185a2be,8000033c:000002ff,8000033c:00682289,8000033c:000002ff,8000033c:000002ff,12010801:000002ff,520107e7:000002ff,8000033c:000002ff,8000033c:000002ff,2c2407ff:400002ff,", 6933, 0},
		{"10010002:000002ff,10070001:000002ff,81f44b7c:000002ff,8000033c:000002ff,8000033c:01f42a6b,50060003:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:01f42328,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:0042a3ac,8000033c:000002ff,8000033c:000002ff,50060003:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:000002ff,8000033c:01e8a2be,8000033c:000002ff,12010801:000002ff,520107e8:000002ff,8000033c:000002ff,8000033c:000002ff,2c2407ff:400002ff,", 15571, 31},
	};
	for (const Case& c : kCases)
	{
		std::vector<VuOp> p;
		for (const char* q = c.hex; *q;)
		{
			VuOp op;
			op.lower = static_cast<u32>(std::strtoul(q, const_cast<char**>(&q), 16));
			q++;
			op.upper = static_cast<u32>(std::strtoul(q, const_cast<char**>(&q), 16));
			if (*q == ',')
				q++;
			p.push_back(op);
		}
		const RegionOutcome o = RunRegion(p, false, [&c](VuTestHarness& h) {
			SeedSparse(h, 4000 + c.iter);
			h.SetVi(2, 8);
		}, c.budget == 0, 14, false, nullptr, c.budget);
		EXPECT_TRUE(o.diffs.empty()) << "iter " << c.iter << " budget " << c.budget << "\n" << Join(o.diffs, 8);
		EXPECT_GT(o.accRegions, 0u);
	}
}

// A bail at a loop head runs microVU's own block for the head, and microVU's code goes on from
// there. Under the exactness contract the flag-ring copies it is handed only have to be the ones
// that code can still read: the rest are overwritten before anything looks at them, so the
// region does not build them for the back edge. Loops whose head block reads old copies (FSAND,
// FMAND, FCAND at the top, a CLIP chain, a divide merge) and overwrites some or all of the ring,
// stopped under every budget: the state the run stops with must be microVU's except for the ring
// copies, and the run re-entered to its end (as a game re-dispatches it) must end in microVU's
// state, ring included. A dropped copy that the continuation reads shows there.
namespace
{
std::vector<VuOp> HandoffProgram(int variant, u32 trip)
{
	struct Shape
	{
		u32 pre, preClip, post, postClip; // FMAC pairs and CLIP pairs before the readers and after them
		u32 readers;                      // 1 FSAND, 2 FMAND, 4 FCAND
		bool twoBlocks;                   // a conditional branch over three pairs after the readers
	};
	static const Shape kShapes[] = {
		{0, 0, 2, 1, 7, false},
		{0, 0, 5, 5, 7, false},
		{3, 0, 3, 0, 7, false},
		{6, 2, 1, 0, 7, false},
		{1, 0, 9, 3, 7, false},
		{2, 2, 6, 2, 1, false}, // STATUS reader only
		{2, 2, 6, 2, 4, false}, // clip reader only
		{2, 1, 6, 2, 2, false}, // MAC reader only
		{3, 1, 4, 2, 7, true},
	};
	const Shape& s = kShapes[variant];
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, static_cast<u16>(trip))));
	p.push_back(L(VIADDIU_L(vi::vi7, vi::vi0, 0x7fff)));
	p.push_back(L(VIADDIU_L(vi::vi10, vi::vi0, 1)));
	const s32 head = static_cast<s32>(p.size());
	u32 k = static_cast<u32>(variant);
	const auto F = [&k]() { return StickyFmac(k++, true); };
	p.push_back(P(VLQI_L(mask::xyzw, vf::vf20, vi::vi9), F()));
	for (u32 i = 0; i < s.pre; i++)
		p.push_back(U(F()));
	for (u32 i = 0; i < s.preClip; i++)
		p.push_back(U(VCLIP_U(1 + i % 4, vf::vf20)));
	// the readers; their results are summed into vi8 two pairs on
	if (s.readers & 1)
		p.push_back(P(VFSAND_L(vi::vi3, 0xfff), F()));
	if (s.readers & 2)
		p.push_back(P(VFMAND_L(vi::vi5, vi::vi7), F()));
	if (s.readers & 4)
		p.push_back(P(VFCAND_L(0xffffff), F())); // (writes vi1)
	p.push_back(U(F()));
	if (s.readers & 1)
		p.push_back(P(VIADD_L(vi::vi8, vi::vi8, vi::vi3), F()));
	if (s.readers & 2)
		p.push_back(P(VIADD_L(vi::vi8, vi::vi8, vi::vi5), F()));
	if (s.readers & 4)
		p.push_back(P(VIADD_L(vi::vi8, vi::vi8, vi::vi1), F()));
	if (s.twoBlocks)
	{
		p.push_back(L(VIAND_L(vi::vi6, vi::vi2, vi::vi10)));
		p.push_back(P(VIBEQ_L(vi::vi6, vi::vi0, 4), F())); // taken: over three pairs (the delay slot still runs)
		p.push_back(U(F()));
		for (int i = 0; i < 3; i++)
			p.push_back(U(F()));
	}
	for (u32 i = 0; i < s.post; i++)
		p.push_back(U(F()));
	for (u32 i = 0; i < s.postClip; i++)
		p.push_back(U(VCLIP_U(5 + i % 4, vf::vf20)));
	p.push_back(P(VISUBIU_L(vi::vi2, vi::vi2, 1), F()));
	const s32 at = static_cast<s32>(p.size());
	p.push_back(P(VIBNE_L(vi::vi2, vi::vi0, static_cast<s16>(head - at - 1)), F()));
	p.push_back(U(F())); // delay slot
	p.push_back(EBit(P(VFSAND_L(vi::vi4, 0xfff), VNOP_U())));
	return p;
}

// Two families of loops, both flag-hack settings, every budget from 1 to 150: the
// sticky-accumulation loops (StickyProgram, the data that tells instances apart) and the
// reader loops above. Returns the runs that differ from microVU (the first few
// reported) and, in *dead and *edges, the ring instances the bails left out and the
// links into loop heads.
u32 HandoffSweep(u32* dead, u32* edges, bool report, bool stopAtFirst)
{
	u32 failures = 0;
	const auto note = [&](const RegionOutcome& o, const char* family, int variant, bool fh, u32 budget) {
		if (dead)
			*dead += o.handoffDead;
		if (edges)
			*edges += o.handoffEdges;
		if (!o.diffs.empty() && (++failures <= 3) && report)
			ADD_FAILURE() << family << " " << variant << " flagHack=" << fh << " budget " << budget << "\n" << Join(o.diffs);
	};
	for (int variant = 0; variant <= 8; variant++)
	{
		const std::vector<VuOp> p = StickyProgram(variant, variant == 4 ? 12 : 7, 16);
		for (bool fh : {false, true})
			for (u32 budget = 1; budget <= 150; budget++)
			{
				const RegionOutcome o = RunRegion(p, fh, [variant](VuTestHarness& h) {
					if (variant >= 6)
						SeedSparse(h, 31 + variant);
					else
						SeedSpecials(h, 31 + variant);
					h.SetVi(8, 0);
					h.WriteMemU128(0x100, 0x00008001, 0x10000000, 0x0000000e, 0);
					h.WriteMemU128(0x110, 0, 0, 0, 0);
				}, false, 14, false, nullptr, budget);
				note(o, "sticky loop", variant, fh, budget);
				if (stopAtFirst && failures)
					return failures;
			}
	}
	for (int variant = 0; variant < 9; variant++)
	{
		const std::vector<VuOp> p = HandoffProgram(variant, 6);
		for (bool fh : {false, true})
			for (u32 budget = 1; budget <= 150; budget++)
			{
				const RegionOutcome o = RunRegion(p, fh, [variant](VuTestHarness& h) {
					SeedMixed(h, 71 + variant);
					h.SetVi(8, 0);
				}, false, 14, false, nullptr, budget);
				note(o, "reader loop", variant, fh, budget);
				if (stopAtFirst && failures)
					return failures;
			}
	}
	return failures;
}

struct HandoffRepro
{
	const char* hex;
	int iter;
	bool flagHack;
	u32 budget;
	int mutation; // the HandoffMutation this program was found with
};

// Programs found by the sticky fuzz with the rule broken. Each must run like microVU with the rule
// as it is, and differ with the rule broken the way it was found (the test checks that they still
// discriminate). Same seeding as StickyFuzz.
const HandoffRepro kHandoffRepros[] = {
	// an instance the head block reads and also writes, counted dead (IgnoreReads)
	{"10010009:000002ff,10070001:000002ff,81f44b7c:01f4a32f,8000033c:0027a36a,03427006:01c44289,8000033c:01c82289,81861bbc:01541a6b,8000033c:01f40a68,12010801:01f44328,520107f8:01541a68,8000033c:01f4a268,8000033c:00272b6a,2c2407ff:400002ff,", 29, false, 57, 1},
	{"10010005:000002ff,10070001:000002ff,81f44b7c:00742a6b,800613bc:000002ff,81812bbc:01a5a369,8000033c:01853ad8,8000033c:01a1a369,8000033c:00e212aa,12010801:0042a398,520107f8:01543a6b,8000033c:01c82289,8000033c:01e8a3ac,2c2407ff:400002ff,", 299, false, 87, 1},
};
} // namespace

static void RegionHandoffRingsBody();
TEST(VuAotRegion, HandoffRings) { RegionHandoffRingsBody(); }
static void RegionHandoffRingsBody()
{
	u32 dead = 0, edges = 0;
	EXPECT_EQ(HandoffSweep(&dead, &edges, true, false), 0u);
	// the point of the test: the bails do leave instances out
	EXPECT_GT(edges, 0u);
	EXPECT_GT(dead, edges);
	std::printf("[vuaot handoff] link edges into loop heads=%u ring instances left out=%u\n", edges, dead);
}

// The same sweep with the rule broken two ways must fail: an instance the continuation reads
// counted as dead where the block also writes it; every instance dead.
TEST(VuAotRegion, HandoffMutationsAreCaught)
{
	using M = vuaot::RegionTier::HandoffMutation;
	struct Guard
	{
		M saved = g_handoffMutation;
		~Guard() { g_handoffMutation = saved; }
	} guard;
	for (M m : {M::IgnoreReads, M::AllDead})
	{
		g_handoffMutation = m;
		const u32 failures = HandoffSweep(nullptr, nullptr, false, true);
		EXPECT_GT(failures, 0u) << "mutation " << static_cast<int>(m) << " went unnoticed";
	}
}

TEST(VuAotRegion, HandoffMutationRepros)
{
	using M = vuaot::RegionTier::HandoffMutation;
	struct Guard
	{
		M saved = g_handoffMutation;
		~Guard() { g_handoffMutation = saved; }
	} guard;
	for (const HandoffRepro& c : kHandoffRepros)
	{
		std::vector<VuOp> p;
		for (const char* q = c.hex; *q;)
		{
			VuOp op;
			op.lower = static_cast<u32>(std::strtoul(q, const_cast<char**>(&q), 16));
			q++;
			op.upper = static_cast<u32>(std::strtoul(q, const_cast<char**>(&q), 16));
			if (*q == ',')
				q++;
			p.push_back(op);
		}
		for (int mutated = 0; mutated < 2; mutated++)
		{
			g_handoffMutation = mutated ? static_cast<M>(c.mutation) : guard.saved;
			const RegionOutcome o = RunRegion(p, c.flagHack, [&c](VuTestHarness& h) {
				if (c.iter & 1)
					SeedSparse(h, 4000 + c.iter);
				else
					SeedSpecials(h, 4000 + c.iter);
				h.SetVi(2, 8);
			}, c.budget == 0, 14, false, nullptr, c.budget);
			if (mutated)
				EXPECT_FALSE(o.diffs.empty()) << "iter " << c.iter << ": mutation " << c.mutation << " went unnoticed";
			else
				EXPECT_TRUE(o.diffs.empty()) << "iter " << c.iter << " budget " << c.budget << "\n" << Join(o.diffs, 8);
		}
	}
}

// Relaxation 3 of the exactness contract: with the flag hack on and no instruction in the micro
// memory reading a sticky STATUS bit, a STATUS update leaves the sticky byte out, so a chain of
// updates costs one pack instead of one per FMAC. The STATUS a program leaves differs from microVU's
// only in bits 6-7 of the register (the sticky zero and sign) and the low byte of its ring copies;
// every other bit, every register, memory and the GIF bytes stay exact, which these tests pin with
// the budget sweep and the run to the end. The loops are the sticky-accumulation and reader loops
// with their status readers' masks cut to the non-sticky bits.
namespace
{
std::vector<VuOp> WithoutStickyReaders(std::vector<VuOp> p)
{
	for (VuOp& op : p)
	{
		if (op.upper & bits::I)
			continue; // the lower word is an immediate
		const vuaot::LowerInst l = vuaot::DecodeLower(op.lower);
		if (l.op == vuaot::LOp::FSAND)
			op.lower = VFSAND_L(l.it, l.imm12 & 0x03f);
		else if (l.op == vuaot::LOp::FSEQ)
			op.lower = VFSEQ_L(l.it, l.imm12 & 0x03f);
		else if (l.op == vuaot::LOp::FSOR)
			op.lower = VFSOR_L(l.it, l.imm12 & 0x03f);
	}
	return p;
}

// A loop that divides by zero (the D flag) and reads the current STATUS bits after the divide
// has landed and FMACs have updated the instance: the D and I bits above the sticky byte must
// survive the updates.
std::vector<VuOp> DivideFlagProgram(u32 trip)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, static_cast<u16>(trip))));
	const s32 head = static_cast<s32>(p.size());
	p.push_back(P(VDIV_L(vf::vf1, 0, vf::vf2, 0), StickyFmac(0, true)));
	for (u32 k = 1; k < 9; k++)
		p.push_back(U(StickyFmac(k, true)));
	p.push_back(P(VFSAND_L(vi::vi3, 0x03f), StickyFmac(9, true)));
	p.push_back(U(StickyFmac(10, true)));
	p.push_back(P(VIADD_L(vi::vi8, vi::vi8, vi::vi3), StickyFmac(11, true)));
	p.push_back(U(StickyFmac(12, true)));
	p.push_back(P(VISUBIU_L(vi::vi2, vi::vi2, 1), StickyFmac(13, true)));
	const s32 at = static_cast<s32>(p.size());
	p.push_back(P(VIBNE_L(vi::vi2, vi::vi0, static_cast<s16>(head - at - 1)), StickyFmac(14, true)));
	p.push_back(U(StickyFmac(15, true)));
	p.push_back(EBit(P(VFSAND_L(vi::vi4, 0x03f), VNOP_U())));
	return p;
}

// The flag hack on and off, every budget from 1 to 150, the loops above. Returns the runs
// that differ, and in the out parameters the regions compiled without the sticky byte, the runs whose
// end states differ in the sticky bits and those that differ in the sticky I / D bits a divide sets.
u32 DropStickySweep(u32* dropped, u32* stickyRuns, u32* divStickyRuns, bool report, bool stopAtFirst)
{
	u32 failures = 0;
	for (bool hack : {true, false})
	for (int family = 0; family < 3; family++)
		for (int variant = 0; variant < (family == 2 ? 2 : 9); variant++)
		{
			const std::vector<VuOp> p = WithoutStickyReaders(family == 0 ? StickyProgram(variant, variant == 4 ? 12 : 7, 16)
														   : family == 1 ? HandoffProgram(variant, 6) : DivideFlagProgram(6));
			for (u32 budget = 1; budget <= 150; budget++)
			{
				const RegionOutcome o = RunRegion(p, hack, [family, variant](VuTestHarness& h) {
					if (family == 0)
					{
						if (variant >= 6)
							SeedSparse(h, 31 + variant);
						else
							SeedSpecials(h, 31 + variant);
						h.SetVi(8, 0);
						h.WriteMemU128(0x100, 0x00008001, 0x10000000, 0x0000000e, 0);
						h.WriteMemU128(0x110, 0, 0, 0, 0);
					}
					else if (family == 1)
					{
						SeedMixed(h, 71 + variant);
						h.SetVi(8, 0);
					}
					else
					{
						// vf2.x is zero: the divide sets the D flag (and 0/0 the I flag)
						SeedSparse(h, 91 + variant);
						h.SetVf(2, 0.0f, 1.0f, 1.0f, 1.0f);
						h.SetVf(1, variant ? 0.0f : 1.0f, 2.0f, 3.0f, 4.0f);
						h.SetVi(8, 0);
					}
				}, false, 14, false, nullptr, budget);
				if (dropped)
					*dropped += o.stickyDropped;
				if (stickyRuns)
					*stickyRuns += o.stickyDiffers ? 1 : 0;
				if (divStickyRuns)
					*divStickyRuns += o.divStickyDiffers ? 1 : 0;
				if (!o.diffs.empty() && (++failures <= 3) && report)
					ADD_FAILURE() << "hack " << hack << " family " << family << " variant " << variant << " budget " << budget << "\n" << Join(o.diffs);
				if (stopAtFirst && failures)
					return failures;
			}
		}
	return failures;
}
} // namespace

TEST(VuAotRegion, DropStickyPinsEverythingElse)
{
	u32 dropped = 0, stickyRuns = 0, divStickyRuns = 0;
	EXPECT_EQ(DropStickySweep(&dropped, &stickyRuns, &divStickyRuns, true, false), 0u);
	// regions do compile without the sticky byte and the difference shows, including the sticky I / D bits of the divides
	EXPECT_GT(dropped, 0u);
	EXPECT_GT(stickyRuns, 0u);
	EXPECT_GT(divStickyRuns, 0u);
	std::printf("[vuaot dropsticky] regions without the sticky byte=%u runs whose sticky bits differ=%u (sticky I/D bits: %u)\n",
		dropped, stickyRuns, divStickyRuns);
}

// The premise is checked per program image: with a sticky reader anywhere in it the sticky byte
// stays, and the run is byte-identical to microVU's. (The loops here read STATUS with 0xfff.)
TEST(VuAotRegion, DropStickyOffWhereAStickyBitIsRead)
{
	int failures = 0;
	for (bool hack : {true, false})
	for (int variant = 0; variant <= 8; variant++)
		for (u32 budget : {0u, 40u, 90u})
		{
			const RegionOutcome o = RunRegion(StickyProgram(variant, variant == 4 ? 12 : 7, 16), hack, [variant](VuTestHarness& h) {
				SeedSparse(h, 31 + variant);
				h.SetVi(8, 0);
				h.WriteMemU128(0x100, 0x00008001, 0x10000000, 0x0000000e, 0);
				h.WriteMemU128(0x110, 0, 0, 0, 0);
			}, budget == 0, 14, false, nullptr, budget);
			EXPECT_EQ(o.stickyDropped, 0u) << "variant " << variant;
			if (!o.diffs.empty() && ++failures <= 3)
				ADD_FAILURE() << "variant " << variant << " budget " << budget << "\n" << Join(o.diffs);
		}
	EXPECT_EQ(failures, 0);
}

// The census of the readers in the regions counts the ones that select a sticky bit, and among
// them the ones that select the sticky I / D bits a divide sets (STATUS bits 10-11): the premise of
// dropping them.
TEST(VuAotRegion, StickyReaderCensusSeparatesTheDivideBits)
{
	struct Case
	{
		u32 imm;
		bool sticky, div;
	};
	for (const Case c : {Case{0x03f, false, false}, Case{0x030, false, false}, Case{0x040, true, false}, Case{0x080, true, false},
			 Case{0x100, true, false}, Case{0x200, true, false}, Case{0x400, true, true}, Case{0x800, true, true},
			 Case{0xc00, true, true}, Case{0xfff, true, true}})
	{
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, 20)));
		p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 0x10)));
		// loop head (pair 2)
		p.push_back(P(VDIV_L(vf::vf1, 0, vf::vf2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)));
		p.push_back(P(VFSAND_L(vi::vi3, c.imm), VADDq_U(mask::xyz, vf::vf5, vf::vf3)));
		p.push_back(P(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0), VMADDy_U(mask::x | mask::w, vf::vf6, vf::vf5, vf::vf4)));
		p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VSUB_U(mask::xyzw, vf::vf7, vf::vf6, vf::vf3)));
		p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(2 - 6 - 1)), VMULAx_U(mask::xyzw, vf::vf7, vf::vf4)));
		p.push_back(P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMADDAw_U(mask::xyzw, vf::vf3, vf::vf4))); // delay slot
		p.push_back(EBit(U(VNOP_U())));
		for (bool fh : {true, false})
		{
			const RegionOutcome o = RunRegion(p, fh, [](VuTestHarness& h) {
				h.SetVf(1, 2.0f, 3.0f, 4.0f, 5.0f);
				h.SetVf(2, 0.5f, 1.5f, 2.5f, 3.5f);
				h.SetVf(3, 1.0f, 1.0f, 1.0f, 1.0f);
				h.SetVf(4, 1.0001f, 0.9999f, 1.00001f, 0.5f);
			});
			EXPECT_TRUE(o.diffs.empty()) << "imm12=" << std::hex << c.imm << "\n" << Join(o.diffs, 6);
			ASSERT_GE(o.regions, 1u);
			EXPECT_GT(o.statusReaders, 0u) << "imm12=" << std::hex << c.imm;
			EXPECT_EQ(o.stickyReaders > 0, c.sticky) << "imm12=" << std::hex << c.imm;
			EXPECT_EQ(o.stickyDivReaders > 0, c.div) << "imm12=" << std::hex << c.imm;
			// a sticky reader anywhere in the image keeps the sticky bits; none drops them
			EXPECT_EQ(o.stickyDropped > 0, !c.sticky) << "imm12=" << std::hex << c.imm << " flagHack=" << fh;
		}
	}
}

// Observable state is still pinned: dropping the bits above the sticky byte too (a divide's D and
// I flags), leaving the update's own pack out (the current Z and S), or leaving a divide's own D / I
// bits out of its flags, must be seen.
TEST(VuAotRegion, DropStickyMutationsAreCaught)
{
	struct Guard
	{
		int saved = g_dropStickyMutation;
		~Guard() { g_dropStickyMutation = saved; }
	} guard;
	for (int m : {1, 2, 3})
	{
		g_dropStickyMutation = m;
		EXPECT_GT(DropStickySweep(nullptr, nullptr, nullptr, false, true), 0u) << "mutation " << m << " went unnoticed";
	}
}

// Loops over every destination field, and flags only the program end reads: the divide's flags
// without branches, the copy-aware liveness at bails, and the MAC and STATUS instances that go
// round a loop untouched.

namespace
{
// A loop whose FMACs cover every destination field (the fifteen masks, the four single lanes twice, in
// several ops), each writing MAC and STATUS, with a MAC and a STATUS reader of the current bits at
// `readers` (0 none, 1 at the top of the loop, reading what the previous trip left, 2 after the
// loop), a divide whose flags land in the loop when `divide`, ending at an E bit.
std::vector<VuOp> FieldLoop(u32 trip, int readers, bool divide)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(trip))));
	p.push_back(L(VIADDIU_L(vi::vi7, vi::vi0, 0x0f)));
	const s32 head = static_cast<s32>(p.size());
	for (u32 m = 1; m <= 15; m++)
	{
		const u32 fd = 9 + (m % 6), fs = 1 + (m * 3) % 8, ft = 1 + (m * 5 + 2) % 8;
		const u32 field = m << 21; // (bit 3 of m is x: mask::x)
		u32 up;
		switch (m % 4)
		{
			case 0: up = VADD_U(field, fd, fs, ft); break;
			case 1: up = VMUL_U(field, fd, fs, ft); break;
			case 2: up = VSUB_U(field, fd, fs, ft); break;
			default: up = VMADD_U(field, fd, fs, ft); break;
		}
		u32 lo = kNopL;
		if (m == 1 && readers == 1)
			lo = VFMAND_L(vi::vi5, vi::vi7);
		if (m == 2 && readers == 1)
			lo = VFSAND_L(vi::vi3, 0x03f);
		if (m == 3 && divide)
			lo = VDIV_L(vf::vf1, 0, vf::vf2, 1);
		if (m == 5)
			lo = VLQI_L(mask::xyzw, vf::vf20, vi::vi9);
		p.push_back(VuOp{lo, up});
	}
	p.push_back(U(VMULA_U(mask::y, vf::vf20, vf::vf3)));
	p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), VMUL_U(mask::w, vf::vf13, vf::vf20, vf::vf4)));
	const s32 at = static_cast<s32>(p.size());
	p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(head - at - 1)), VADD_U(mask::x, vf::vf14, vf::vf20, vf::vf5)));
	p.push_back(U(VSUB_U(mask::z, vf::vf12, vf::vf20, vf::vf6))); // delay slot
	if (readers == 2)
	{
		p.push_back(L(VFMAND_L(vi::vi5, vi::vi7)));
		p.push_back(L(VFSAND_L(vi::vi3, 0x03f)));
	}
	p.push_back(EBit(U(VNOP_U())));
	return p;
}

// A loop whose FMACs run in its first trip only (a branch skips them afterwards), so the MAC and STATUS
// instances they leave go round the loop untouched and the program end reads them. A bail at any
// later trip hands them to microVU, and microVU's continuation enters the region again in the middle
// of the loop: both have to rebuild exactly what the first trip made.
std::vector<VuOp> OnceLoop(int variant)
{
	static const u32 kFields[][4] = {{0xF, 0x8, 0x6, 0x1}, {0x1, 0x2, 0x4, 0x8}, {0xE, 0xF, 0x9, 0x3}, {0x5, 0xA, 0xC, 0x7}};
	const u32* f = kFields[variant % 4];
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(4 + variant))));
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 1)));
	p.push_back(L(VIBEQ_L(vi::vi2, vi::vi0, 6))); // head: over the FMACs once vi2 is 0 (to pair 9)
	p.push_back(U(VNOP_U()));                      // delay slot
	p.push_back(U(VMUL_U(f[0] << 21, vf::vf9, vf::vf1, vf::vf2)));
	p.push_back(U(VADD_U(f[1] << 21, vf::vf10, vf::vf3, vf::vf4)));
	p.push_back(U(variant & 4 ? VMULA_U(f[2] << 21, vf::vf5, vf::vf6) : VSUB_U(f[2] << 21, vf::vf11, vf::vf5, vf::vf6)));
	p.push_back(U(VMADD_U(f[3] << 21, vf::vf12, vf::vf7, vf::vf8)));
	p.push_back(L(VIADDIU_L(vi::vi2, vi::vi0, 0)));
	for (int k = 0; k < 2 + variant % 3; k++) // pair 9: the trips that skip
		p.push_back(L(VIADDIU_L(vi::vi8, vi::vi8, 1)));
	p.push_back(L(VISUBIU_L(vi::vi1, vi::vi1, 1)));
	const s32 at = static_cast<s32>(p.size());
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(2 - at - 1))));
	p.push_back(U(VNOP_U())); // delay slot
	p.push_back(EBit(U(VNOP_U())));
	return p;
}

// FieldLoop and OnceLoop, the flag hack on and off, a run to the end and every budget from 1 to 150, so a bail
// lands at every loop head and microVU's continuation enters the region again in the middle of the loop.
u32 LoopFamilySweep(u32* runs, bool report)
{
	u32 failures = 0;
	for (bool hack : {true, false})
		for (int family = 3; family < 5; family++)
			for (int variant = 0; variant < (family == 3 ? 9 : 8); variant++)
			{
				const std::vector<VuOp> p = family == 3 ? FieldLoop(variant < 3 ? 5 : 1 + variant, variant % 3, variant >= 3) : OnceLoop(variant);
				const auto setup = [family, variant](VuTestHarness& h) {
					SeedMixed(h, 71 + variant + 16 * family);
					h.SetVi(8, 0);
					if (family == 3)
						h.SetVf(2, 0.0f, variant & 1 ? 0.0f : 1.0f, 1.0f, 1.0f); // (the divide: D, and I when vf1.x is 0 too)
				};
				for (u32 budget = 0; budget <= 150; budget++)
				{
					const RegionOutcome o = RunRegion(p, hack, setup, budget == 0, 14, false, nullptr, budget);
					if (runs)
						(*runs)++;
					if (!o.diffs.empty() && (++failures <= 3) && report)
						ADD_FAILURE() << "hack " << hack << " family " << family << " variant " << variant << " budget " << budget << "\n" << Join(o.diffs);
				}
			}
	return failures;
}
} // namespace

TEST(VuAotRegion, FieldAndOnceLoopsMatchMicroVU)
{
	u32 runs = 0;
	EXPECT_EQ(LoopFamilySweep(&runs, true), 0u);
	EXPECT_GT(runs, 5000u);
}

// Random FMAC-dense loops: every field shape, readers and a divide or an FSSET
// now and then, forward branches that change how many status writes a trip
// makes, run under random budgets so bails land anywhere.
static void RegionStickyFuzzBody();
TEST(VuAotRegion, StickyFuzz) { RegionStickyFuzzBody(); }
static void RegionStickyFuzzBody()
{
	std::mt19937 rng(20261013);
	std::mt19937 erng(20261014); // (the empty-field FMACs have their own stream, as in LoopFuzz)
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	const int iters = getenv("VUAOT_STICKY_ITERS") ? std::atoi(getenv("VUAOT_STICKY_ITERS")) : 150;
	int failures = 0;
	u32 accRegions = 0, nonLinear = 0, accumulated = 0;
	for (int iter = 0; iter < iters; iter++)
	{
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(2 + R(14)))));
		p.push_back(L(VIADDIU_L(vi::vi7, vi::vi0, 1)));
		const s32 head = static_cast<s32>(p.size());
		const int len = 6 + static_cast<int>(R(18));
		int lastBranch = -10;
		const bool sparse = iter % 2 == 1;
		for (int i = 0; i < len; i++)
		{
			u32 up = R(10) ? StickyFmac(R(1000), sparse) : VNOP_U(), lo = kNopL;
			if (R(3) == 0)
				up = (up & ~0x1E00000u) | ((R(15) + 1) << 21); // a random field
			if (up == VNOP_U() && erng() % 2 == 0)
				up = EmptyFieldFmac(erng);
			switch (R(22))
			{
				case 0: lo = VFSAND_L(vi::vi3 + R(2), R(0x1000)); break;
				case 1: lo = VFMAND_L(vi::vi3 + R(2), vi::vi7); break;
				case 2: lo = VDIV_L(1 + R(6), R(4), 1 + R(6), R(4)); break;
				case 3:
					if (iter % 3 == 0) // (FSSET kills leave a stale ring slot as the previous instance: those regions keep the old lowering)
						lo = VFSSET_L(R(0x1000));
					break;
				case 4: lo = VIAND_L(vi::vi6, vi::vi1, vi::vi7); break;
				case 5: lo = VSQ_L((R(15) + 1) << 21, 9 + R(6), vi::vi2, static_cast<s16>(R(8))); break;
				case 6:
					if (i + 5 < len && i >= lastBranch + 3)
					{
						lo = VIBEQ_L(vi::vi6, vi::vi0, static_cast<s16>(1 + R(3)));
						lastBranch = i;
					}
					break;
				default: break;
			}
			if (sparse && i == 0)
				lo = VLQI_L(mask::xyzw, vf::vf20, vi::vi9); // the event vector
			p.push_back(VuOp{lo, up});
		}
		p.push_back(P(VISUBIU_L(vi::vi1, vi::vi1, 1), StickyFmac(R(1000), sparse)));
		const s32 at = static_cast<s32>(p.size());
		p.push_back(P(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(head - at - 1)), StickyFmac(R(1000), sparse)));
		p.push_back(U(StickyFmac(R(1000), sparse)));
		p.push_back(U(StickyFmac(R(1000), sparse)));
		p.push_back(EBit(P(VFSAND_L(vi::vi4, 0xfff), VNOP_U())));
		const u32 budget = R(4) == 0 ? 0 : 1 + R(160);
		for (bool fh : {false, true})
		{
			const RegionOutcome o = RunRegion(p, fh, [iter, sparse](VuTestHarness& h) {
				if (sparse)
					SeedSparse(h, 4000 + iter);
				else
					SeedSpecials(h, 4000 + iter);
				h.SetVi(2, 8);
			}, budget == 0, 14, false, nullptr, budget);
			accRegions += o.accRegions;
			nonLinear += o.accNonLinear;
			accumulated += o.accAccumulated;
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
				ADD_FAILURE() << "iter " << iter << " flagHack=" << fh << " budget " << budget << " hex=" << hex << "\n" << Join(o.diffs, 8);
			}
		}
	}
	EXPECT_EQ(failures, 0);
	EXPECT_GT(accRegions, static_cast<u32>(iters) / 2);
	std::printf("[vuaot sticky fuzz] programs=%d acc regions=%u declined (FSSET kills)=%u accumulated updates=%u\n", iters * 2, accRegions, nonLinear, accumulated);
}

// Debugging aid for the sticky fuzz: VUAOT_REGION_HEX="lower:upper,..."
// VUAOT_REGION_SEED=n (the iteration number) VUAOT_REGION_FH=0|1
// VUAOT_REGION_BUDGET=n [VUAOT_REGION_SPARSE=1 for the odd iterations] runs one
// program as the fuzz does, region code dumped.
TEST(VuAotRegion, Repro)
{
	const char* hex = getenv("VUAOT_REGION_HEX");
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
	const int iter = getenv("VUAOT_REGION_SEED") ? std::atoi(getenv("VUAOT_REGION_SEED")) : 0;
	const bool fh = getenv("VUAOT_REGION_FH") && std::atoi(getenv("VUAOT_REGION_FH"));
	const u32 budget = getenv("VUAOT_REGION_BUDGET") ? static_cast<u32>(std::atoi(getenv("VUAOT_REGION_BUDGET"))) : 0;
	const bool sparse = getenv("VUAOT_REGION_SPARSE") != nullptr;
	const auto setup = [iter, sparse](VuTestHarness& h) {
		if (sparse)
			SeedSparse(h, 4000 + iter);
		else
			SeedSpecials(h, 4000 + iter);
		h.SetVi(2, 8);
	};
	if (getenv("VUAOT_REGION_SHRINK"))
	{
		// Drop every operation the failure survives losing (loop control stays).
		const auto control = [](u32 lo) {
			const u32 op = lo >> 25;
			return Classify(lo) == LowerKind::Branch || op == 0x08 || op == 0x09 || lo == (0x8000033c);
		};
		const auto fails = [&](const std::vector<VuOp>& q) { return !RunRegion(q, fh, setup, budget == 0, 14, false, nullptr, budget).diffs.empty(); };
		for (size_t i = 2; i + 1 < p.size(); i++)
		{
			if (p[i].upper != VNOP_U())
			{
				std::vector<VuOp> q = p;
				q[i].upper = VNOP_U();
				if (fails(q))
					p = q;
			}
			if (!control(p[i].lower))
			{
				std::vector<VuOp> q = p;
				q[i].lower = kNopL;
				if (fails(q))
					p = q;
			}
		}
		std::string hex2;
		for (const VuOp& op : p)
		{
			char buf[24];
			std::snprintf(buf, sizeof(buf), "%08x:%08x,", op.lower, op.upper);
			hex2 += buf;
		}
		std::printf("shrunk: %s\n", hex2.c_str());
	}
	const RegionOutcome o = RunRegion(p, fh, setup, budget == 0, 14, true, nullptr, budget);
	EXPECT_TRUE(o.diffs.empty()) << Join(o.diffs, 20);
}

// A run that stops on the cycle budget with an XGKICK in flight (the loop of
// StickyProgram variant 5, stopped 11 to 16 cycles into a trip) must match microVU in
// VPU_STAT bit 12. The interpreter sets bit 12 when a kick starts and clears it when the
// transfer ends, so its reference pass, stopped at the budget, leaves it set in VU0's copy
// of VPU_STAT. microVU and vu-aot never write bit 12, so VuTestHarness::SeedEntryState has to
// clear it: RunRegion's region run follows the reference run in the same process.
TEST(VuAotRegion, StoppedMidKickInheritsNothing)
{
	const std::vector<VuOp> p = StickyProgram(5, 7, 16);
	for (bool fh : {false, true})
		for (u32 budget : {11u, 12u, 16u, 33u, 38u})
		{
			const RegionOutcome o = RunRegion(p, fh, [](VuTestHarness& h) {
				SeedSpecials(h, 31 + 5);
				h.SetVi(8, 0);
				h.WriteMemU128(0x100, 0x00008001, 0x10000000, 0x0000000e, 0);
				h.WriteMemU128(0x110, 0, 0, 0, 0);
			}, false, 14, false, nullptr, budget);
			EXPECT_TRUE(o.diffs.empty()) << "flagHack=" << fh << " budget " << budget << "\n" << Join(o.diffs, 10);
		}
}

// VI values known at compile time fold, memory ops at constant
// addresses address VU memory directly (wrapping like microVU), a VI copy
// reads its source, a second LQ of the same address reuses the first, and an
// LQ after a full-mask SQ to the same address reads the stored register.
static void ViConstantsAndAddressesBody();
TEST(VuAotLowering, ViConstantsAndAddresses) { ViConstantsAndAddressesBody(); }
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

// A result's MAC and status packs share one sign/zero vector,
// the pack weights load once per block, and FSAND computes only the status
// bits its immediate keeps. Every FSAND immediate shape over status words
// with sticky, divide and single-lane bits set.
static void FlagPacksAndFsandBody();
TEST(VuAotLowering, FlagPacksAndFsand) { FlagPacksAndFsandBody(); }
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
TEST(VuAotLowering, MaddClampsCachedAcc)
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
TEST(VuAotLowering, ClampModes)
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
		RegionStickyAccumulationBody();
		RegionStickyFuzzBody();
		g_clampMode = -1;
	}
}

// The lowering suites again with every body list-scheduled
// (operations reordered by latency before the allocator sees them), small
// register pools included.
TEST(VuAotLowering, Scheduled)
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

// Debugging aid: VUAOT_REPRO_HEX="lower:upper,..." VUAOT_REPRO_SEED=n
// VUAOT_REPRO_FH=0|1 runs one program (as the pressure fuzz seeds it) with
// the lowered code dumped.
TEST(VuAotLowering, Repro)
{
	const char* hex = getenv("VUAOT_REPRO_HEX");
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
	const int seed = getenv("VUAOT_REPRO_SEED") ? std::atoi(getenv("VUAOT_REPRO_SEED")) : 5000;
	const bool fh = !getenv("VUAOT_REPRO_FH") || std::atoi(getenv("VUAOT_REPRO_FH"));
	const auto setup = [seed](VuTestHarness& h) {
		SeedSpecials(h, seed);
		for (int r = 12; r < 32; r++)
			h.SetVf(r, 1.0f + r, -0.5f * r, 0.25f + r, 3.0f - r);
	};
	const Outcome o = RunBoth(p, fh, setup, 1, true);
	EXPECT_TRUE(o.diffs.empty()) << Join(o.diffs, 20);
}


// FMACs with an empty destination field. The interpreter's account (VUops.cpp applyBinaryMACOp
// and its siblings): a lane outside the field is not written, so an empty field writes no VF and
// no ACC lane, and its MAC lane bits are cleared, so MAC is 0 and the current ZSUO bits of STATUS
// are 0 with the sticky bits kept. vu-aot and microVU both follow it.

namespace
{
// The VF and ACC lanes of two snapshots that differ.
std::vector<std::string> VfAccDiffs(const VuSnapshot& a, const VuSnapshot& b, const char* an, const char* bn)
{
	std::vector<std::string> out;
	char buf[96];
	for (int r = 0; r < 32; r++)
		for (int l = 0; l < 4; l++)
			if (a.regs.VF[r].UL[l] != b.regs.VF[r].UL[l])
			{
				std::snprintf(buf, sizeof(buf), "VF%d.%d %s=%08x %s=%08x", r, l, an, a.regs.VF[r].UL[l], bn, b.regs.VF[r].UL[l]);
				out.emplace_back(buf);
			}
	for (int l = 0; l < 4; l++)
		if (a.regs.ACC.UL[l] != b.regs.ACC.UL[l])
		{
			std::snprintf(buf, sizeof(buf), "ACC.%d %s=%08x %s=%08x", l, an, a.regs.ACC.UL[l], bn, b.regs.ACC.UL[l]);
			out.emplace_back(buf);
		}
	return out;
}

struct VsInterp
{
	std::vector<std::string> vuaot; // vu-aot region against the interpreter, VF and ACC lanes
	std::vector<std::string> mvu;   // microVU against the interpreter, the same lanes
	u32 regions = 0;
};

// One run of the interpreter and microVU together (the harness runs both), then the region tier.
VsInterp RunVsInterp(const std::vector<VuOp>& prog, bool flagHack, const std::function<void(VuTestHarness&)>& setup)
{
	FlagHackScope fh(flagHack);
	VsInterp r;
	VuSnapshot mvu, interp;
	{
		VuTestHarness h(1);
		setup(h);
		h.LoadProgram(prog);
		h.RunNoDiff();
		mvu = h.JitSnapshot();
		interp = h.InterpSnapshot();
	}
	VuTestHarness h(1);
	setup(h);
	h.LoadProgram(prog);
	{
		RegionScope rs(1);
		rs.region.accAlways = true;
		h.RunNoDiff();
		r.regions = rs.region.Regions();
	}
	r.vuaot = VfAccDiffs(interp, h.JitSnapshot(), "interp", "vuaot");
	r.mvu = VfAccDiffs(interp, mvu, "interp", "mvu");
	return r;
}

// A counted loop: head work, then the branch pair, then the delay slot pair, then the E-bit pair.
// `body` goes after VISUBIU in the branch's block; `delay` is the delay slot's upper.
std::vector<VuOp> DelaySlotLoop(u32 trip, const std::vector<VuOp>& body, u32 delay)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(trip))));
	const s32 head = static_cast<s32>(p.size());
	for (const VuOp& op : body)
		p.push_back(op);
	p.push_back(L(VISUBIU_L(vi::vi1, vi::vi1, 1)));
	const s32 at = static_cast<s32>(p.size());
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(head - at - 1))));
	p.push_back(U(delay));
	p.push_back(EBit(U(VNOP_U())));
	return p;
}
} // namespace

// An empty-field FMAC to vf12, then the loop's delay slot reads vf12 whole, or writes a lane of it. vu-aot
// and microVU both leave vf12 as the interpreter does (no lane written by the empty FMAC).
TEST(VuAotEmptyField, RegionsLeaveTheDestinationAlone)
{
	struct Case
	{
		const char* name;
		u32 empty; // the empty-field FMAC to vf12
	};
	const Case forms[] = {
		{"ADD", VADD_U(0, vf::vf12, vf::vf5, vf::vf6)},
		{"SUB", VSUB_U(0, vf::vf12, vf::vf5, vf::vf6)},
		{"MUL", VMUL_U(0, vf::vf12, vf::vf5, vf::vf6)},
		{"MADD", VMADD_U(0, vf::vf12, vf::vf5, vf::vf6)},
		{"MSUB", VMSUB_U(0, vf::vf12, vf::vf5, vf::vf6)},
		{"MAX", VMAX_U(0, vf::vf12, vf::vf5, vf::vf6)},
		{"MINI", VMINI_U(0, vf::vf12, vf::vf5, vf::vf6)},
		{"MULx", VMULx_U(0, vf::vf12, vf::vf5, vf::vf6)},
		{"MADDy", VMADDx_U(0, vf::vf12, vf::vf5, vf::vf6) + 1},
		{"MULq", VMULq_U(0, vf::vf12, vf::vf5)},
	};
	const u32 delays[] = {
		VSUB_U(mask::z, vf::vf12, vf::vf20, vf::vf6),   // a lane of vf12 written
		VADD_U(mask::xyzw, vf::vf13, vf::vf12, vf::vf0), // vf12 read whole
		VMUL_U(mask::x | mask::y, vf::vf13, vf::vf12, vf::vf6), // two lanes read
	};
	u32 runs = 0;
	for (const Case& c : forms)
		for (u32 d = 0; d < std::size(delays); d++)
			for (u32 trip : {1u, 3u})
				for (bool fh : {false, true})
				{
					const std::vector<VuOp> p = DelaySlotLoop(trip, {U(c.empty)}, delays[d]);
					const VsInterp r = RunVsInterp(p, fh, [](VuTestHarness& h) { SeedMixed(h, 77); });
					EXPECT_GE(r.regions, 1u);
					EXPECT_TRUE(r.vuaot.empty()) << c.name << " delay " << d << " trip " << trip << " flagHack " << fh << "\n" << Join(r.vuaot, 6);
					EXPECT_TRUE(r.mvu.empty()) << "microVU: " << c.name << " delay " << d << " trip " << trip << " flagHack " << fh << "\n" << Join(r.mvu, 6);
					runs++;
				}
	std::printf("[vuaot empty field] %u runs; vu-aot and microVU equal the interpreter in every VF / ACC lane\n", runs);
}

// A counted loop of an empty-field MADD to vf12 and, in the delay slot, a SUB of the z lane of vf12.
// The interpreter leaves x, y and w as they were, and so do vu-aot and microVU.
TEST(VuAotEmptyField, MicroVuAndVuAotLeaveALaterAccessAlone)
{
	const std::vector<VuOp> p = DelaySlotLoop(5, {U(0x00063329)}, 0x0046a32c); // MADD.0 vf12,vf6,vf6 ; SUB.z vf12,vf20,vf6
	const auto setup = [](VuTestHarness& h) { SeedSpecials(h, 4003); };
	for (bool fh : {false, true})
	{
		const VsInterp r = RunVsInterp(p, fh, setup);
		EXPECT_TRUE(r.vuaot.empty()) << "flagHack " << fh << "\n" << Join(r.vuaot);
		EXPECT_TRUE(r.mvu.empty()) << "microVU, flagHack " << fh << "\n" << Join(r.mvu);
	}
}

// What an empty-field FMAC does to the flags, read well after it (the readers wait out the flag
// pipeline): MAC all clear, STATUS's current bits clear, the sticky bits kept. The interpreter, microVU
// and vu-aot agree.
TEST(VuAotEmptyField, FlagsAreClearedStickyKept)
{
	std::vector<VuOp> p;
	p.push_back(L(VIADDIU_L(vi::vi7, vi::vi0, 0x0f)));
	p.push_back(U(VSUB_U(mask::xyzw, vf::vf13, vf::vf1, vf::vf1))); // +0 in every lane: MAC = 0xf, STATUS Z and sticky Z
	for (int i = 0; i < 4; i++)
		p.push_back(U(VNOP_U()));
	p.push_back(L(VFMAND_L(vi::vi5, vi::vi7)));
	p.push_back(L(VFSAND_L(vi::vi6, 0xfff)));
	p.push_back(U(VMADD_U(0, vf::vf24, vf::vf2, vf::vf3))); // the empty field
	for (int i = 0; i < 4; i++)
		p.push_back(U(VNOP_U()));
	p.push_back(L(VFMAND_L(vi::vi8, vi::vi7)));
	p.push_back(L(VFSAND_L(vi::vi9, 0xfff)));
	p.push_back(EBit(U(VNOP_U())));
	for (bool fh : {false, true})
	{
		VuSnapshot interp, mvu;
		{
			FlagHackScope hack(fh);
			VuTestHarness h(1);
			SeedMixed(h, 5);
			h.LoadProgram(p);
			h.RunNoDiff();
			interp = h.InterpSnapshot();
			mvu = h.JitSnapshot();
		}
		const RegionOutcome o = RunRegion(p, fh, [](VuTestHarness& h) { SeedMixed(h, 5); }, false);
		EXPECT_TRUE(o.diffs.empty()) << "flagHack " << fh << "\n" << Join(o.diffs);
		for (const VuSnapshot* e : {&interp, &mvu})
		{
			EXPECT_EQ(e->regs.VI[5].UL, 0xfu) << "MAC after the +0 FMAC";
			EXPECT_EQ(e->regs.VI[6].UL & 0xfff, 0x41u) << "STATUS after it: Z and sticky Z";
			EXPECT_EQ(e->regs.VI[8].UL, 0u) << "MAC after the empty-field FMAC";
			EXPECT_EQ(e->regs.VI[9].UL & 0xfff, 0x40u) << "STATUS after it: current bits clear, sticky kept";
		}
	}
}

// Programs from the loop fuzz's mix with empty-field FMACs: every state vu-aot leaves, flags and ACC
// included, is microVU's. The empty-field FMACs of the even programs write registers nothing reads
// (vf24..vf27) or ACC; those of the odd programs write the registers the program computes with
// (vf1..vf8). 2 x 3,000 programs by default.
TEST(VuAotEmptyField, EmptyFieldFuzzMatchesMicroVU)
{
	const int iters = getenv("VUAOT_EF_ITERS") ? std::atoi(getenv("VUAOT_EF_ITERS")) : 3000;
	std::mt19937 rng(20261015), erng(20261017);
	auto R = [&](u32 n) { return static_cast<u32>(rng() % n); };
	int failures = 0;
	u32 empties = 0, regions = 0;
	for (int iter = 0; iter < iters; iter++)
	{
		std::vector<VuOp> p;
		p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, static_cast<u16>(1 + R(8)))));
		p.push_back(L(VIADDIU_L(vi::vi7, vi::vi0, 0x0f)));
		const s32 head = static_cast<s32>(p.size());
		const int len = 3 + R(8);
		for (int i = 0; i < len; i++)
		{
			u32 lo = kNopL;
			switch (R(8))
			{
				case 0: lo = VFMAND_L(vi::vi3 + R(2), vi::vi7); break;
				case 1: lo = VFSAND_L(vi::vi3 + R(2), R(0x1000)); break;
				default: break;
			}
			u32 up = VNOP_U();
			if (R(5))
			{
				if (R(3) == 0)
				{
					up = EmptyFieldFmac(erng, iter & 1);
					empties++;
				}
				else
				{
					const u32 m = (R(15) + 1) << 21, fd = 1 + R(6), fs = 1 + R(6), ft = 1 + R(6);
					switch (R(6))
					{
						case 0: up = VADD_U(m, fd, fs, ft); break;
						case 1: up = VSUB_U(m, fd, fs, ft); break;
						case 2: up = VMUL_U(m, fd, fs, ft); break;
						case 3: up = VMADD_U(m, fd, fs, ft); break;
						case 4: up = VMULA_U(m, fs, ft); break;
						default: up = VMAX_U(m, fd, fs, ft); break;
					}
				}
			}
			p.push_back(VuOp{lo, up});
		}
		p.push_back(L(VISUBIU_L(vi::vi1, vi::vi1, 1)));
		const s32 at = static_cast<s32>(p.size());
		p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(head - at - 1))));
		p.push_back(U(EmptyFieldFmac(erng, iter & 1))); // the delay slot
		empties++;
		p.push_back(U(VADD_U(mask::xyzw, vf::vf1, vf::vf2, vf::vf3)));
		p.push_back(EBit(L(VFSAND_L(vi::vi4, 0xfff))));
		for (bool fh : {false, true})
		{
			const RegionOutcome o = RunRegion(p, fh, [iter](VuTestHarness& h) { SeedMixed(h, 6000 + iter); }, false);
			regions += o.regions;
			if (!o.diffs.empty() && ++failures <= 3)
			{
				std::string hex;
				for (const VuOp& op : p)
				{
					char b[24];
					std::snprintf(b, sizeof(b), "%s%08x:%08x", hex.empty() ? "" : ",", op.lower, op.upper);
					hex += b;
				}
				ADD_FAILURE() << "iter " << iter << " flagHack " << fh << " hex=" << hex << "\n" << Join(o.diffs, 6);
			}
		}
	}
	EXPECT_EQ(failures, 0);
	EXPECT_GT(empties, static_cast<u32>(iters));
	std::printf("[vuaot empty field fuzz] programs=%d empty-field FMACs=%u regions=%u\n", iters * 2, empties, regions);
}

} // namespace recompiler_tests

#endif
