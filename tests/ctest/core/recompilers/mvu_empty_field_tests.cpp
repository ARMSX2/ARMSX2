// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// A write with an empty destination field (xyzw = 0) writes no VF lane and no
// ACC lane. The interpreter clears the MAC word and the current ZSUO bits of
// STATUS, keeps the sticky bits, and leaves everything else alone.
//
// microVU's allocReg records a slot whose xyzw is 0 as an unmodified cached
// copy of its register, and an empty-field write passes the same value. The
// slot holding the arithmetic result was therefore recorded as a clean copy of
// the register it was told to write: Fd, or VF0 for the bodies that allocate
// Fs or the I register as a write to VF0 ("a scratch copy, drop it"). A later
// read of that register took the result for its value, and a later partial
// write to it merged its lane into that slot and wrote the whole slot back.
//
// Each program here seeds ACC, runs one empty-field instruction, then one
// access that can find the stale slot (a read of Fd, a partial write to Fd, a
// read of ACC, a read of VF0), in straight-line code and in a counted loop's
// branch delay slot. microVU's post-state is diffed against the interpreter's
// on both VUs, with the flag hack off (the whole architectural state) and on
// (VF and ACC; the hack drops flags the interpreter computes).
//
// No title in the VU capture corpus has an empty-field FMAC.

#include "harness/VuTestHarness.h"
#include "harness/VuEncode.h"

#include "Config.h"
#include "VU.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp U(u32 upper) { return VuOp{0x8000033c, upper}; }
inline VuOp L(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp WithI(u32 imm, u32 upper) { return IBit(VuOp{VLitI(imm), upper}); }

constexpr u32 kFd = vf::vf12;
constexpr u32 kFs = vf::vf5;
constexpr u32 kFt = vf::vf6;

struct Form
{
	const char* name;
	VuOp op; // the pair holding the empty-field instruction
	Form(const char* n, u32 upper) : name(n), op(U(upper)) {}
	Form(const char* n, VuOp pair) : name(n), op(pair) {}
};

// Every FMAC body the emitters have: FMACa (ADD/SUB/MUL/MAX/MINI and their
// broadcast, I and Q operand shapes), FMACc (MADD), FMACd (MSUB).
std::vector<Form> FdForms()
{
	const u32 z = mask::none;
	return {
		{"ADD", VADD_U(z, kFd, kFs, kFt)},
		{"SUB", VSUB_U(z, kFd, kFs, kFt)},
		{"MUL", VMUL_U(z, kFd, kFs, kFt)},
		{"MAX", VMAX_U(z, kFd, kFs, kFt)},
		{"MINI", VMINI_U(z, kFd, kFs, kFt)},
		{"ADDx", VADDx_U(z, kFd, kFs, kFt)},
		{"SUBz", VSUBz_U(z, kFd, kFs, kFt)},
		{"MULw", VMULw_U(z, kFd, kFs, kFt)},
		{"ADDi", VADDi_U(z, kFd, kFs)},
		{"MULi", VMULi_U(z, kFd, kFs)},
		{"ADDq", VADDq_U(z, kFd, kFs)},
		{"MULq", VMULq_U(z, kFd, kFs)},
		{"MADD", VMADD_U(z, kFd, kFs, kFt)},
		{"MADDy", VMADDy_U(z, kFd, kFs, kFt)},
		{"MADDi", VMADDi_U(z, kFd, kFs)},
		{"MADDq", VMADDq_U(z, kFd, kFs)},
		{"MSUB", VMSUB_U(z, kFd, kFs, kFt)},
		{"MSUBx", VMSUBx_U(z, kFd, kFs, kFt)},
		{"MSUBi", VMSUBi_U(z, kFd, kFs)},
		{"MSUBq", VMSUBq_U(z, kFd, kFs)},
	};
}

// The forms that write ACC (FMACa with isACC, FMACb).
std::vector<Form> AccForms()
{
	const u32 z = mask::none;
	return {
		{"ADDA", VADDA_U(z, kFs, kFt)},
		{"SUBA", VSUBA_U(z, kFs, kFt)},
		{"MULA", VMULA_U(z, kFs, kFt)},
		{"ADDAx", VADDAx_U(z, kFs, kFt)},
		{"MULAw", VMULAw_U(z, kFs, kFt)},
		{"MULAq", VMULAq_U(z, kFs)},
		{"MULAi", VMULAi_U(z, kFs)},
		{"MADDA", VMADDA_U(z, kFs, kFt)},
		{"MSUBA", VMSUBA_U(z, kFs, kFt)},
		{"MADDAx", VMADDAx_U(z, kFs, kFt)},
		{"MSUBAz", VMSUBAz_U(z, kFs, kFt)},
	};
}

// The other emitters that allocate their destination with the instruction's
// field: ABS and the conversions in the upper pipe, and the moves and loads of
// the lower pipe. An empty field writes nothing in them either.
std::vector<Form> OtherForms()
{
	const u32 z = mask::none;
	return {
		{"ABS", VABS_U(z, kFd, kFs)},
		{"ITOF0", VITOF0_U(z, kFd, kFs)},
		{"FTOI0", VFTOI0_U(z, kFd, kFs)},
		{"MOVE", VuOp{VMOVE_L(z, kFd, kFs), VNOP_U()}},
		{"MR32", VuOp{VMR32_L(z, kFd, kFs), VNOP_U()}},
		{"MFIR", VuOp{VMFIR_L(z, kFd, vi::vi2), VNOP_U()}},
		{"LQ", VuOp{VLQ_L(z, kFd, vi::vi0, 0x10), VNOP_U()}},
		{"LQI", VuOp{VLQI_L(z, kFd, vi::vi3), VNOP_U()}},
		{"LQD", VuOp{VLQD_L(z, kFd, vi::vi3), VNOP_U()}},
	};
}

// Accesses that find a stale slot. Each is one pair, run right after the
// empty-field instruction.
struct Access
{
	const char* name;
	u32 upper;
};

const Access kAccesses[] = {
	{"read Fd", VADD_U(mask::xyzw, vf::vf13, kFd, vf::vf0)},
	{"partial write Fd", VSUB_U(mask::z, kFd, vf::vf20, kFt)},
	{"read ACC", VMADD_U(mask::xyzw, vf::vf14, vf::vf1, vf::vf2)},
	{"read VF0", VADD_U(mask::xyzw, vf::vf15, vf::vf0, vf::vf1)},
};

void Seed(VuTestHarness& h)
{
	// Distinct finite values per register, so a lane that took the wrong
	// register or the arithmetic result cannot match by accident.
	for (u32 r = 1; r < 32; r++)
	{
		const float b = static_cast<float>(r);
		h.SetVf(r, b * 1.25f + 0.5f, -(b * 0.75f) - 1.0f, b * 0.5f + 2.25f, b * 0.125f - 3.5f);
	}
	h.SetQ(0x3fc00000u); // 1.5
}

// Prelude: I = 2.5 (for the I forms), ACC = vf3 * vf4 (so a write to ACC shows).
void Prelude(std::vector<VuOp>& p)
{
	p.push_back(WithI(0x40200000u, VNOP_U()));
	p.push_back(U(VMULA_U(mask::xyzw, vf::vf3, vf::vf4)));
}

struct Outcome
{
	std::vector<std::string> diffs;
	VuSnapshot jit, interp;
};

class FlagHackScope
{
public:
	explicit FlagHackScope(bool on) : saved_(EmuConfig.Speedhacks.vuFlagHack) { EmuConfig.Speedhacks.vuFlagHack = on; }
	~FlagHackScope() { EmuConfig.Speedhacks.vuFlagHack = saved_; }

private:
	bool saved_;
};

// VF and ACC lanes where two snapshots differ.
void VfAccDiffs(const VuSnapshot& a, const VuSnapshot& b, std::vector<std::string>& out)
{
	char buf[96];
	for (int r = 0; r < 32; r++)
		for (int l = 0; l < 4; l++)
			if (a.regs.VF[r].UL[l] != b.regs.VF[r].UL[l])
			{
				std::snprintf(buf, sizeof(buf), "vf%d lane %d: interp %08x, microVU %08x", r, l, a.regs.VF[r].UL[l], b.regs.VF[r].UL[l]);
				out.emplace_back(buf);
			}
	for (int l = 0; l < 4; l++)
		if (a.regs.ACC.UL[l] != b.regs.ACC.UL[l])
		{
			std::snprintf(buf, sizeof(buf), "ACC lane %d: interp %08x, microVU %08x", l, a.regs.ACC.UL[l], b.regs.ACC.UL[l]);
			out.emplace_back(buf);
		}
}

// The flag hack drops flags the interpreter computes, so with it on only VF
// and ACC are compared; with it off the whole architectural state is.
Outcome RunBoth(int vu, bool flagHack, const std::vector<VuOp>& prog)
{
	FlagHackScope fh(flagHack);
	VuTestHarness h(vu);
	Seed(h);
	h.LoadProgram(prog);
	h.RunNoDiff();
	Outcome o;
	o.jit = h.JitSnapshot();
	o.interp = h.InterpSnapshot();
	if (flagHack)
		VfAccDiffs(o.interp, o.jit, o.diffs);
	else
		o.diffs = DiffVu(o.jit, o.interp); // labelled JIT=, INTERP=
	return o;
}

std::string Join(const std::vector<std::string>& v, size_t max = 8)
{
	std::string s;
	for (size_t i = 0; i < v.size() && i < max; i++)
		s += "  " + v[i] + "\n";
	if (v.size() > max)
		s += "  ... " + std::to_string(v.size() - max) + " more\n";
	return s;
}

std::vector<VuOp> Straight(VuOp empty, u32 access)
{
	std::vector<VuOp> p;
	Prelude(p);
	p.push_back(empty);
	p.push_back(U(access));
	p.push_back(EBit(U(VNOP_U())));
	return p;
}

// A counted loop: `empty` is the body, `delay` the branch's delay slot.
std::vector<VuOp> DelaySlotLoop(u32 trip, VuOp empty, u32 delay)
{
	std::vector<VuOp> p;
	Prelude(p);
	p.push_back(L(VIADDIU_L(vi::vi1, vi::vi0, trip)));
	const s32 head = static_cast<s32>(p.size());
	p.push_back(empty);
	p.push_back(L(VISUBIU_L(vi::vi1, vi::vi1, 1)));
	p.push_back(U(VNOP_U())); // the branch reads vi1 a pair after the decrement
	const s32 at = static_cast<s32>(p.size());
	p.push_back(L(VIBNE_L(vi::vi1, vi::vi0, static_cast<s16>(head - at - 1))));
	p.push_back(U(delay));
	p.push_back(EBit(U(VNOP_U())));
	return p;
}

// One empty-field instruction, one access, in the two places an access can
// sit: the next pair of straight-line code, and a counted loop's branch delay
// slot. The block's preload (mvuPreloadRegisters) loads every register a
// later pair reads into its own slot before the first one runs, and the
// allocator's search takes the lowest-numbered slot, so in straight line the
// preloaded copy of Fd hides a stale one. The preload stops at a branch, so a
// delay slot's reads are not preloaded and meet the stale slot.
void RunForms(const std::vector<Form>& forms, const char* what)
{
	u32 runs = 0, bad = 0;
	std::string report;
	const auto note = [&](const Outcome& o, int vu, bool hack, const char* shape, const Form& f, const Access& a) {
		runs++;
		if (o.diffs.empty())
			return;
		bad++;
		if (bad <= 6)
			report += std::string("vu") + std::to_string(vu) + " hack " + std::to_string(hack) + " " + shape + ": " + f.name + ".0, then " + a.name + ":\n" + Join(o.diffs, 4);
	};
	for (int vu = 0; vu < 2; vu++)
		for (bool hack : {false, true})
			for (const Form& f : forms)
				for (const Access& a : kAccesses)
				{
					note(RunBoth(vu, hack, Straight(f.op, a.upper)), vu, hack, "straight", f, a);
					for (u32 trip : {1u, 3u})
						note(RunBoth(vu, hack, DelaySlotLoop(trip, f.op, a.upper)), vu, hack, trip == 1 ? "loop x1" : "loop x3", f, a);
				}
	EXPECT_EQ(bad, 0u) << what << ": " << bad << " of " << runs << " runs differ from the interpreter\n" << report;
}

} // namespace

// The Fd forms: an empty-field FMAC to vf12, then each access.
TEST(MvuEmptyField, FdFormsWriteNoVfLane)
{
	RunForms(FdForms(), "Fd forms");
}

// The ACC forms: an empty-field ADDA/MULA/MADDA/... leaves ACC as the prelude
// set it, and the register it computed in does not pass for VF0.
TEST(MvuEmptyField, AccFormsWriteNoAccLane)
{
	RunForms(AccForms(), "ACC forms");
}

// The same for the emitters that are not FMACs.
TEST(MvuEmptyField, OtherWritersWriteNoVfLane)
{
	RunForms(OtherForms(), "other writers");
}

// The program the fault was first seen in, byte for byte: a counted loop of
// MADD.0 vf12,vf6,vf6 (upper 00063329) with SUB.z vf12,vf20,vf6 (0046a32c) in
// the branch delay slot. vf12 x, y and w came out holding the MADD's result.
TEST(MvuEmptyField, AsFoundInTheShrunkProgram)
{
	for (int vu = 0; vu < 2; vu++)
		for (bool hack : {false, true})
		{
			const Outcome o = RunBoth(vu, hack, DelaySlotLoop(5, U(0x00063329), 0x0046a32c));
			EXPECT_TRUE(o.diffs.empty()) << "vu" << vu << " hack " << hack << "\n" << Join(o.diffs);
		}
}

// The destination keeps its words and ACC keeps its value, checked against
// the seeds and not only against the interpreter, and the flags are what an
// empty field leaves: MAC clear, STATUS's current bits clear, its sticky bits
// kept.
TEST(MvuEmptyField, LeavesRegistersAndClearsFlags)
{
	std::vector<Form> forms = {
		{"ADD", VADD_U(mask::none, kFd, kFs, kFt)},
		{"MUL", VMUL_U(mask::none, kFd, kFs, kFt)},
		{"MADD", VMADD_U(mask::none, kFd, kFs, kFt)},
		{"ADDA", VADDA_U(mask::none, kFs, kFt)},
		{"MADDA", VMADDA_U(mask::none, kFs, kFt)},
	};
	for (int vu = 0; vu < 2; vu++)
		for (bool hack : {false, true})
			for (const Form& f : forms)
			{
				SCOPED_TRACE(::testing::Message() << "vu" << vu << " hack " << hack << " " << f.name);
				std::vector<VuOp> p;
				Prelude(p);
				p.push_back(L(VIADDIU_L(vi::vi7, vi::vi0, 0x0f)));
				p.push_back(U(VSUB_U(mask::xyzw, vf::vf13, vf::vf1, vf::vf1))); // +0 in every lane: MAC 0xf, STATUS Z and sticky Z
				for (int i = 0; i < 4; i++)
					p.push_back(U(VNOP_U()));
				p.push_back(L(VFMAND_L(vi::vi5, vi::vi7)));
				p.push_back(L(VFSAND_L(vi::vi6, 0xfff)));
				p.push_back(f.op); // the empty field
				for (int i = 0; i < 4; i++)
					p.push_back(U(VNOP_U()));
				p.push_back(L(VFMAND_L(vi::vi8, vi::vi7)));
				p.push_back(L(VFSAND_L(vi::vi9, 0xfff)));
				p.push_back(EBit(U(VNOP_U())));

				const Outcome o = RunBoth(vu, hack, p);
				EXPECT_TRUE(o.diffs.empty()) << Join(o.diffs);

				// ACC = vf3 * vf4 from the prelude, computed the way the
				// harness seeds them (float multiply, no clamp involved).
				for (const VuSnapshot* s : {&o.jit, &o.interp})
				{
					const bool jit = (s == &o.jit);
					SCOPED_TRACE(jit ? "microVU" : "interpreter");
					for (int l = 0; l < 4; l++)
					{
						const float b3 = static_cast<float>(3), b4 = static_cast<float>(4);
						const float v3[4] = {b3 * 1.25f + 0.5f, -(b3 * 0.75f) - 1.0f, b3 * 0.5f + 2.25f, b3 * 0.125f - 3.5f};
						const float v4[4] = {b4 * 1.25f + 0.5f, -(b4 * 0.75f) - 1.0f, b4 * 0.5f + 2.25f, b4 * 0.125f - 3.5f};
						const float acc = v3[l] * v4[l];
						u32 want;
						std::memcpy(&want, &acc, 4);
						EXPECT_EQ(s->regs.ACC.UL[l], want) << "ACC lane " << l;
					}
					EXPECT_EQ(s->regs.VI[5].UL, 0xfu) << "MAC after the +0 FMAC";
					EXPECT_EQ(s->regs.VI[6].UL & 0xfff, 0x41u) << "STATUS after it: Z and sticky Z";
					EXPECT_EQ(s->regs.VI[8].UL, 0u) << "MAC after the empty-field FMAC";
					EXPECT_EQ(s->regs.VI[9].UL & 0xfff, 0x40u) << "STATUS after it: current bits clear, sticky kept";
				}
			}
}

} // namespace recompiler_tests
