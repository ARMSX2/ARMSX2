// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU data-memory addressing and XGKICK packet fetch against a first-party
// console capture -- batch 2, questions 4 and 5 of the adjudication request.
//
// Neither area had ever been checked against silicon. Both engines implement
// the same addressing model, which means agreeing with each other proves
// nothing; the model itself was inherited and had no oracle behind it.
//
// What the console established:
//
//   Q4. VU0's own data memory wraps at 256 quadwords (index & 0xFF). Index
//       bit 10 selects the VU0->VU1 register window instead, and inside the
//       window index & 0x3F picks one of 64 quadwords: VF0..VF31 then
//       VI0..VI31, repeating every 64. VU1 has no window at all -- its index
//       is simply & 0x3FF, so index 1088 is quadword 64, not a register.
//       Writes through the window work in the same direction as reads: a VU0
//       SQ to index 1027 lands in VU1's VF3 and nowhere in VU0's memory.
//
//   Q5. An XGKICK packet read does wrap at the top of VU1 memory. A GIFtag in
//       quadword 1023 whose payload runs past the end reads quadword 0 next,
//       and a packet straddling the seam reads 1022, 1023, 0, 1 in that order.
//
// The console read Q5 back through the GS: an A+D write to the GS LABEL
// register lands in SIGLBLID at 0x12001080, which the EE can read without a
// GS download. The harness has no GS, so the equivalent here is to replay the
// captured Path 1 packet stream through `Gif_HandlerAD` -- the same in-tree
// handler production uses -- and compare the resulting LBLID against the
// console's.

#include <gtest/gtest.h>

#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuEncode.h"
#include "harness/VuTestHarness.h"

#include "autocases_vulat.h"
#include "vulat_console.h"

#include "GS.h"
#include "Gif_Unit.h"
#include "VU.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace recompiler_tests
{
namespace
{
using namespace vulat_common;
using vulat::Case;
using vu::VuOp;

constexpr u32 kVu1ResQw = vulat::kVu1SampQw;  // 512 -- LQ cases store here
constexpr u32 kVu1Res2Qw = vulat::kVu1SampQw + 1; // 513 -- SQ cases read back here
constexpr u32 kVu1PaintQw = 900;              // 900 + n holds VU1's vf(n) paint
constexpr u32 kXgPresetQw = 200;
constexpr u32 kXgPresetVal = 0x5A5A5A5Au;

// ---------------------------------------------------------------------------
//  Program builders -- mirrors of gen_vl.py, pinned by PrepareConsoleCase's
//  FNV-1a check against the words the console actually ran
// ---------------------------------------------------------------------------

// vi1 carries the index, seeded from the EE through CTC2 so no VU integer
// hazard sits between the write and the load.
std::vector<VuOp> Vu0LoadProgram()
{
	std::vector<VuOp> p;
	p.push_back(VuOp{vu::VLQ_L(vu::mask::xyzw, 7, 1, 0), vu::VNOP_U()});
	PushNops(p, 6);
	PushETail(p);
	return p;
}

// SQ vf6 through vi1, then LQ back through vi2 -- and the EE reads VU0 data
// memory directly as an independent second route to the same fact.
std::vector<VuOp> Vu0StoreProgram()
{
	std::vector<VuOp> p;
	p.push_back(VuOp{vu::VSQ_L(vu::mask::xyzw, 6, 1, 0), vu::VNOP_U()});
	PushNops(p, 6);
	p.push_back(VuOp{vu::VLQ_L(vu::mask::xyzw, 7, 2, 0), vu::VNOP_U()});
	PushNops(p, 6);
	PushETail(p);
	return p;
}

std::vector<VuOp> Vu1LoadProgram(u32 index)
{
	std::vector<VuOp> p = Vu1Preamble();
	p.push_back(SetIndex(1, index));
	PushNops(p, 4);
	p.push_back(VuOp{vu::VLQ_L(vu::mask::xyzw, 7, 1, 0), vu::VNOP_U()});
	PushNops(p, 6);
	p.push_back(VuOp{vu::VSQ_L(vu::mask::xyzw, 7, 0, static_cast<s16>(kVu1ResQw)), vu::VNOP_U()});
	PushNops(p, 4);
	PushETail(p);
	return p;
}

std::vector<VuOp> Vu1StoreProgram(u32 index, u32 readback)
{
	std::vector<VuOp> p = Vu1Preamble();
	p.push_back(SetIndex(1, index));
	PushNops(p, 4);
	p.push_back(VuOp{vu::VSQ_L(vu::mask::xyzw, 6, 1, 0), vu::VNOP_U()});
	PushNops(p, 6);
	p.push_back(VuOp{vu::VLQ_L(vu::mask::xyzw, 7, 0, static_cast<s16>(readback)), vu::VNOP_U()});
	PushNops(p, 6);
	p.push_back(VuOp{vu::VSQ_L(vu::mask::xyzw, 7, 0, static_cast<s16>(kVu1Res2Qw)), vu::VNOP_U()});
	PushNops(p, 4);
	PushETail(p);
	return p;
}

// No preamble -- the point is to read a VF register the previous case wrote
// through the VU0 window, so seeding it would erase the evidence.
std::vector<VuOp> Vu1DumpVf3Program()
{
	std::vector<VuOp> p;
	p.push_back(VuOp{vu::VSQ_L(vu::mask::xyzw, 3, 0, static_cast<s16>(kVu1ResQw)), vu::VNOP_U()});
	PushNops(p, 4);
	PushETail(p);
	return p;
}

// Paints VU1's whole register file with values nothing else in the corpus
// produces, so the VU0 window reads say which register they hit rather than
// just "not my sentinel".
std::vector<VuOp> Vu1PaintProgram()
{
	std::vector<VuOp> p;
	for (u32 n = 1; n < 32; ++n)
		p.push_back(VuOp{vu::VLQ_L(vu::mask::xyzw, n, 0, static_cast<s16>(kVu1PaintQw + n)), vu::VNOP_U()});
	for (u32 n = 1; n < 16; ++n)
		p.push_back(VuOp{vu::VIADDIU_L(n, 0, 0x100 * n + n), vu::VNOP_U()});
	PushNops(p, 4);
	PushETail(p);
	return p;
}

std::vector<VuOp> XgkickProgram(u32 preset_qw, int test_qw)
{
	std::vector<VuOp> p = Vu1Preamble();
	p.push_back(VuOp{vu::VIADDIU_L(1, 0, preset_qw), vu::VNOP_U()});
	PushNops(p, 6);
	p.push_back(VuOp{vu::VXGKICK_L(1), vu::VNOP_U()});
	PushNops(p, 80);
	if (test_qw >= 0)
	{
		p.push_back(VuOp{vu::VIADDIU_L(2, 0, static_cast<u32>(test_qw)), vu::VNOP_U()});
		PushNops(p, 6);
		p.push_back(VuOp{vu::VXGKICK_L(2), vu::VNOP_U()});
		PushNops(p, 80);
	}
	PushETail(p);
	return p;
}

// "Q4_VU0_LQ_1025" -> 1025. "Q4_VU1_SQ_1029" -> 1029.
u32 IndexFromTag(const std::string& tag)
{
	return static_cast<u32>(std::strtoul(tag.substr(tag.rfind('_') + 1).c_str(), nullptr, 10));
}

std::vector<VuOp> BuildProgram(const Case& c)
{
	const std::string tag(c.tag);
	if (tag == "Q4_VU1_PAINT")
		return Vu1PaintProgram();
	if (tag == "Q4_VU1_READBACK_VF3")
		return Vu1DumpVf3Program();
	if (tag == "Q4_VU0_SQ_WINDOW_VF3")
		return Vu0StoreProgram();
	if (tag.rfind("Q4_VU0_LQ_", 0) == 0)
		return Vu0LoadProgram();
	if (tag.rfind("Q4_VU0_SQ_", 0) == 0)
		return Vu0StoreProgram();
	if (tag.rfind("Q4_VU1_LQ_", 0) == 0)
		return Vu1LoadProgram(IndexFromTag(tag));
	if (tag.rfind("Q4_VU1_SQ_", 0) == 0)
	{
		// vi_seed is a VU0-only channel; the VU1 readback quadword is the
		// wrapped index, which is what the store landed on.
		const u32 index = IndexFromTag(tag);
		return Vu1StoreProgram(index, index & 0x3FFu);
	}
	// Q5 programs are built by the XGKICK tests directly, not from the kind.
	ADD_FAILURE() << "unhandled case " << tag;
	return {};
}

bool RunConsoleCase(const Case& c, VuTestHarness& h)
{
	return vulat_common::RunConsoleCase(c, h, BuildProgram(c));
}

// ---------------------------------------------------------------------------
//  The VU1 register-file paint, reproduced as state rather than replayed
// ---------------------------------------------------------------------------

// On the console the paint is its own microprogram, and the window reads that
// follow are separate cases: FBRST resets the VU's execution state but not its
// register file, so VU1 still holds the paint when the next VU0 program runs.
// The harness gives one VU instance per fixture and zeroes it on construction,
// so the paint has to be established here as pre-state. The values are what
// Q4_VU1_PAINT's program writes, and the test below checks that they are.
void PaintVu1RegisterFile()
{
	VURegs& v = vuRegs[1];
	v.VF[0].i.x = 0u;
	v.VF[0].i.y = 0u;
	v.VF[0].i.z = 0u;
	v.VF[0].f.w = 1.0f;
	for (u32 n = 1; n < 32; ++n)
	{
		v.VF[n].i.x = 0xF1000000u | n;
		v.VF[n].i.y = 0xF2000000u | n;
		v.VF[n].i.z = 0xF3000000u | n;
		v.VF[n].i.w = 0xF4000000u | n;
	}
	// The window maps VI as 128-bit quadwords, so the padding matters as much
	// as the value: zero the whole array, then write the low word.
	std::memset(&v.VI[0], 0, sizeof(v.VI));
	for (u32 n = 1; n < 16; ++n)
		v.VI[n].UL = 0x100u * n + n;
}

// What the paint left in window quadword `w` (0..63), lane x.
u32 PaintedWindowWordX(u32 w)
{
	if (w < 32)
		return (w == 0) ? 0u : (0xF1000000u | w);
	const u32 vi = w - 32;
	return (vi >= 1 && vi < 16) ? (0x100u * vi + vi) : 0u;
}

// ---------------------------------------------------------------------------
//  Q5 oracle bridge: replay a captured Path 1 stream through the real handler
// ---------------------------------------------------------------------------

// Every packet in this corpus is PACKED / NREG=1 / REGS[0]=A+D, so the stream
// is a sequence of {GIFtag, NLOOP A+D quadwords}. Feeding each A+D to
// Gif_HandlerAD is what production does; the only thing this function adds is
// the tag walk. Returns LBLID after the replay, restoring the global.
u32 ReplayPath1IntoLabel(const std::vector<u8>& bytes, u32 lblid_pre, const char* tag)
{
	const u32 saved_sig = GSSIGLBLID.SIGID;
	const u32 saved_lbl = GSSIGLBLID.LBLID;
	GSSIGLBLID.LBLID = lblid_pre;

	std::size_t off = 0;
	while (off + 16 <= bytes.size())
	{
		u32 gt[4];
		std::memcpy(gt, bytes.data() + off, 16);
		off += 16;
		const u32 nloop = gt[0] & 0x7FFFu;
		const u32 flg = (gt[1] >> 26) & 0x3u;
		const u32 nreg = (gt[1] >> 28) & 0xFu;
		EXPECT_EQ(flg, 0u) << tag << ": expected a PACKED GIFtag";
		EXPECT_EQ(nreg, 1u) << tag << ": expected NREG = 1";
		EXPECT_EQ(gt[2] & 0xFu, 0xEu) << tag << ": expected REGS[0] = A+D";
		for (u32 i = 0; i < nloop && off + 16 <= bytes.size(); ++i, off += 16)
			Gif_HandlerAD(const_cast<u8*>(bytes.data()) + off);
	}
	EXPECT_EQ(off, bytes.size()) << tag << ": the Path 1 stream did not parse "
	                                       "cleanly as whole GIF packets";

	const u32 out = GSSIGLBLID.LBLID;
	GSSIGLBLID.SIGID = saved_sig;
	GSSIGLBLID.LBLID = saved_lbl;
	return out;
}

// Q5 programs are keyed by the quadword the packet under test starts at, which
// the corpus records only in the rule string; rebuild it from the tag instead.
int XgTestQuadword(const std::string& tag)
{
	if (tag == "Q5_XG_NOKICK")
		return -1;
	if (tag == "Q5_XG_CTRL")
		return 100;
	if (tag == "Q5_XG_WRAPTAG")
		return 1021;
	return 1023;
}

} // namespace

// ---------------------------------------------------------------------------
//  Q4.1 -- VU0's own data memory
// ---------------------------------------------------------------------------

TEST(VuMemoryConsole, Vu0MemoryWrapsAtItsOwnSizeMatchesConsole)
{
	// Every index with bit 10 clear addresses VU0's own 4 KB, wrapping at 256
	// quadwords -- including 2048 and 2560, which have higher bits set.
	int scored = 0;
	for (const Case* c : CasesOfKind(vulat::kVu0Mem))
	{
		const std::string tag(c->tag);
		if (tag.rfind("Q4_VU0_LQ_", 0) != 0)
			continue;
		const u32 index = IndexFromTag(tag);
		if (index & 0x400u)
			continue;

		// Independent route to the same fact: the console's own reading,
		// re-derived from the rule rather than from either engine.
		EXPECT_EQ(c->sample[7], vulat::kVu0Pattern | (index & 0xFFu))
			<< tag << ": the console capture itself";

		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << tag;
		EXPECT_EQ(h.GetVfBitsJit(7, 'x'), c->sample[7]) << tag << " (arm64 recompiler)";
		EXPECT_EQ(h.GetVfBitsInterp(7, 'x'), c->sample[7]) << tag << " (interpreter)";
		++scored;
	}
	EXPECT_GE(scored, 7) << "the capture lost its non-window VU0 load rows";
}

TEST(VuMemoryConsole, Vu0StoreIndexWrapMatchesConsole)
{
	// The store side of the same rule: SQ to 256, 259 and 2048 land on
	// quadwords 0, 3 and 0, which the program reads back and the probe also
	// dumped straight out of VU0 memory.
	for (const char* tag : {"Q4_VU0_SQ_256", "Q4_VU0_SQ_259", "Q4_VU0_SQ_2048"})
	{
		const Case& c = CaseByTag(tag);
		VuTestHarness h(0);
		ASSERT_TRUE(RunConsoleCase(c, h)) << tag;
		EXPECT_EQ(h.GetVfBitsJit(7, 'x'), c.sample[7]) << tag << " (arm64 recompiler)";
		EXPECT_EQ(h.GetVfBitsInterp(7, 'x'), c.sample[7]) << tag << " (interpreter)";
		for (u32 d = 0; d < 4; ++d)
		{
			for (u32 lane = 0; lane < 4; ++lane)
			{
				const u32 addr = c.dumpqw[d] * 16 + lane * 4;
				EXPECT_EQ(h.GetMemU32Jit(addr), c.dump[d * 4 + lane])
					<< tag << " VU0 quadword " << c.dumpqw[d] << " lane " << lane
					<< " (arm64 recompiler)";
				EXPECT_EQ(h.GetMemU32Interp(addr), c.dump[d * 4 + lane])
					<< tag << " VU0 quadword " << c.dumpqw[d] << " lane " << lane
					<< " (interpreter)";
			}
		}
	}
}

// ---------------------------------------------------------------------------
//  Q4.2 -- the VU0 -> VU1 register window
// ---------------------------------------------------------------------------

TEST(VuMemoryConsole, TheHandSeededPaintIsWhatTheConsolePaintProgramProduces)
{
	// PaintVu1RegisterFile() is pre-state this file writes by hand, which
	// would be an invented oracle if nothing checked it. Run the console's
	// actual paint microprogram through both engines and require the register
	// file it leaves to be exactly that pre-state.
	const Case& c = CaseByTag("Q4_VU1_PAINT");

	VuTestHarness h(1);
	ASSERT_TRUE(PrepareConsoleCase(c, h, BuildProgram(c)));

	u32 interp_vf[32], jit_vf[32], interp_vi[16], jit_vi[16];
	h.RunInterpOnly();
	for (u32 n = 0; n < 32; ++n)
		interp_vf[n] = vuRegs[1].VF[n].i.x;
	for (u32 n = 0; n < 16; ++n)
		interp_vi[n] = vuRegs[1].VI[n].UL;

	h.RunJitPreserveBlockCache();
	for (u32 n = 0; n < 32; ++n)
		jit_vf[n] = vuRegs[1].VF[n].i.x;
	for (u32 n = 0; n < 16; ++n)
		jit_vi[n] = vuRegs[1].VI[n].UL;

	PaintVu1RegisterFile();
	for (u32 n = 1; n < 32; ++n)
	{
		EXPECT_EQ(jit_vf[n], vuRegs[1].VF[n].i.x) << "vf" << n << " (arm64 recompiler)";
		EXPECT_EQ(interp_vf[n], vuRegs[1].VF[n].i.x) << "vf" << n << " (interpreter)";
	}
	for (u32 n = 1; n < 16; ++n)
	{
		EXPECT_EQ(jit_vi[n], vuRegs[1].VI[n].UL) << "vi" << n << " (arm64 recompiler)";
		EXPECT_EQ(interp_vi[n], vuRegs[1].VI[n].UL) << "vi" << n << " (interpreter)";
	}
	// And the console agrees about the two register slots it dumped through
	// memory (quadwords 901 and 931 are vf1's and vf31's paint sources).
	EXPECT_EQ(c.dump[0], 0xF1000001u) << "the console capture itself";
	EXPECT_EQ(c.dump[4], 0xF100001Fu) << "the console capture itself";
}

TEST(VuMemoryConsole, Vu0IndexBit10SelectsTheVu1RegisterWindowMatchesConsole)
{
	// Index bit 10 leaves VU0's memory entirely: index & 0x3F picks one of 64
	// quadwords, VF0..VF31 then VI0..VI31, and the window repeats every 64.
	int scored = 0;
	for (const Case* c : CasesOfKind(vulat::kVu0Mem))
	{
		const std::string tag(c->tag);
		if (tag.rfind("Q4_VU0_LQ_", 0) != 0)
			continue;
		const u32 index = IndexFromTag(tag);
		if (!(index & 0x400u))
			continue;

		// Independent route: what the paint put in that window slot, derived
		// from the paint program rather than read off an engine.
		EXPECT_EQ(c->sample[7], PaintedWindowWordX(index & 0x3Fu))
			<< tag << ": the console capture itself";

		VuTestHarness h(0);
		PaintVu1RegisterFile();
		ASSERT_TRUE(RunConsoleCase(*c, h)) << tag;
		EXPECT_EQ(h.GetVfBitsJit(7, 'x'), c->sample[7]) << tag << " (arm64 recompiler)";
		EXPECT_EQ(h.GetVfBitsInterp(7, 'x'), c->sample[7]) << tag << " (interpreter)";
		++scored;
	}
	EXPECT_GE(scored, 9) << "the capture lost its window rows";
}

TEST(VuMemoryConsole, TheWindowIsNotTheSentinelAndNotVu0Memory)
{
	// Liveness for the test above. If the paint never reached VU1, or the
	// window silently resolved back into VU0 memory, every window row would
	// still "match" some constant -- so assert the readings are actually
	// distinct from both alternatives, and that the window rows differ from
	// each other.
	const u32 vf1 = CaseByTag("Q4_VU0_LQ_1025").sample[7];
	const u32 vf31 = CaseByTag("Q4_VU0_LQ_1055").sample[7];
	const u32 vi1 = CaseByTag("Q4_VU0_LQ_1057").sample[7];
	EXPECT_EQ(vf1, 0xF1000001u);
	EXPECT_EQ(vf31, 0xF100001Fu);
	EXPECT_EQ(vi1, 0x00000101u);
	EXPECT_NE(vf1, vf31);
	EXPECT_NE(vf1, vi1);
	for (u32 v : {vf1, vf31, vi1})
	{
		EXPECT_NE(v & 0xFFFFFF00u, vulat::kVu0Pattern)
			<< "a window read returned VU0 memory";
		EXPECT_NE(v, vulat::kSentinel) << "a window read returned the sentinel";
	}
}

TEST(VuMemoryConsole, Vu0StoreThroughTheWindowLandsInVu1AndNotInVu0Memory)
{
	// The console needed two microprograms for this: a VU0 store to index
	// 1027, then a VU1 program that dumps its own VF3. The second one's
	// reading is the oracle for the first.
	const Case& c = CaseByTag("Q4_VU0_SQ_WINDOW_VF3");
	const Case& readback = CaseByTag("Q4_VU1_READBACK_VF3");
	EXPECT_EQ(readback.dump[0], 0xC0DE0006u) << "the console capture itself";

	// Both engines write VU1's register file, and the harness restores only
	// the VU under test between passes -- so the two passes are run one-sided
	// and re-painted in between, or the second engine would be scored against
	// the first engine's result.
	VuTestHarness h(0);
	PaintVu1RegisterFile();
	ASSERT_TRUE(PrepareConsoleCase(c, h, BuildProgram(c)));
	// The pre-state has to differ from what is being written, or "it landed"
	// is unfalsifiable.
	ASSERT_NE(vuRegs[1].VF[3].i.x, readback.dump[0]);

	h.RunInterpOnly();
	u32 interp_vf3[4] = {vuRegs[1].VF[3].i.x, vuRegs[1].VF[3].i.y,
	                     vuRegs[1].VF[3].i.z, vuRegs[1].VF[3].i.w};

	PaintVu1RegisterFile();
	h.RunJitPreserveBlockCache();
	u32 jit_vf3[4] = {vuRegs[1].VF[3].i.x, vuRegs[1].VF[3].i.y,
	                  vuRegs[1].VF[3].i.z, vuRegs[1].VF[3].i.w};

	for (u32 lane = 0; lane < 4; ++lane)
	{
		EXPECT_EQ(jit_vf3[lane], readback.dump[lane])
			<< "VU1 VF3 lane " << lane << " (arm64 recompiler)";
		EXPECT_EQ(interp_vf3[lane], readback.dump[lane])
			<< "VU1 VF3 lane " << lane << " (interpreter)";
	}

	// The other half of the claim: it landed in VU1 instead of VU0 memory.
	// Quadword 3 is what index 1027 would have hit had bit 10 been ignored.
	for (u32 d = 0; d < 4; ++d)
	{
		for (u32 lane = 0; lane < 4; ++lane)
		{
			const u32 addr = c.dumpqw[d] * 16 + lane * 4;
			EXPECT_EQ(h.GetMemU32Jit(addr), c.dump[d * 4 + lane])
				<< "VU0 quadword " << c.dumpqw[d] << " lane " << lane
				<< " (arm64 recompiler)";
			EXPECT_EQ(h.GetMemU32Interp(addr), c.dump[d * 4 + lane])
				<< "VU0 quadword " << c.dumpqw[d] << " lane " << lane
				<< " (interpreter)";
		}
	}
	EXPECT_EQ(c.dump[4], vulat::kVu0Pattern | 3u)
		<< "the console capture itself: VU0 quadword 3 was never written";
}

// ---------------------------------------------------------------------------
//  Q4.3 -- VU1's own data memory, which has no window
// ---------------------------------------------------------------------------

TEST(VuMemoryConsole, Vu1MemoryWrapsAtItsOwnSizeMatchesConsole)
{
	int scored = 0;
	for (const Case* c : CasesOfKind(vulat::kVu1Mem))
	{
		const std::string tag(c->tag);
		if (tag.rfind("Q4_VU1_LQ_", 0) != 0)
			continue;
		const u32 index = IndexFromTag(tag);

		// Independent route: VU1 masks the index and nothing else. Index 1088
		// is the load-bearing row -- bit 10 is set, and on VU0 that would be
		// the register window.
		EXPECT_EQ(c->dump[0], vulat::kVu1Pattern | (index & 0x3FFu))
			<< tag << ": the console capture itself";

		VuTestHarness h(1);
		ASSERT_TRUE(RunConsoleCase(*c, h)) << tag;
		for (u32 lane = 0; lane < 4; ++lane)
		{
			const u32 addr = kVu1ResQw * 16 + lane * 4;
			EXPECT_EQ(h.GetMemU32Jit(addr), c->dump[lane])
				<< tag << " lane " << lane << " (arm64 recompiler)";
			EXPECT_EQ(h.GetMemU32Interp(addr), c->dump[lane])
				<< tag << " lane " << lane << " (interpreter)";
		}
		++scored;
	}
	EXPECT_GE(scored, 8) << "the capture lost its VU1 load rows";
}

TEST(VuMemoryConsole, Vu1HasNoRegisterWindow)
{
	// Bit 10 of the index is the window selector on VU0 and nothing at all on
	// VU1. Stated as its own row because it is the one place the two VUs'
	// addressing genuinely differs, and because "1088 -> quadword 64" is only
	// interesting next to "1088 -> VF0" on VU0.
	EXPECT_EQ(CaseByTag("Q4_VU1_LQ_1088").dump[0], vulat::kVu1Pattern | 64u);
	EXPECT_EQ(CaseByTag("Q4_VU0_LQ_1088").sample[7], PaintedWindowWordX(0));
}

TEST(VuMemoryConsole, Vu1StoreIndexWrapMatchesConsole)
{
	for (const char* tag : {"Q4_VU1_SQ_1029", "Q4_VU1_SQ_2048"})
	{
		const Case& c = CaseByTag(tag);
		VuTestHarness h(1);
		ASSERT_TRUE(RunConsoleCase(c, h)) << tag;
		// dumpqw = {513 readback, wrapped index, wrapped index, 1023}: the
		// value the program read back and the quadword it landed on.
		for (u32 d = 0; d < 4; ++d)
		{
			for (u32 lane = 0; lane < 4; ++lane)
			{
				const u32 addr = c.dumpqw[d] * 16 + lane * 4;
				EXPECT_EQ(h.GetMemU32Jit(addr), c.dump[d * 4 + lane])
					<< tag << " VU1 quadword " << c.dumpqw[d] << " lane " << lane
					<< " (arm64 recompiler)";
				EXPECT_EQ(h.GetMemU32Interp(addr), c.dump[d * 4 + lane])
					<< tag << " VU1 quadword " << c.dumpqw[d] << " lane " << lane
					<< " (interpreter)";
			}
		}
		// Liveness: quadword 1023 is the far end of memory and must not have
		// been touched, or "the store wrapped" would be unfalsifiable.
		EXPECT_EQ(c.dump[12], vulat::kVu1Pattern | 1023u)
			<< tag << ": the console capture itself";
	}
}

// ---------------------------------------------------------------------------
//  Q5 -- does an XGKICK packet read wrap at the top of VU1 memory?
// ---------------------------------------------------------------------------

TEST(VuXgkickConsole, WrappedPacketReadMatchesConsoleLabel)
{
	// Replay each engine's captured Path 1 stream through the production A+D
	// handler and compare LBLID with what the console's GS actually held.
	int scored = 0;
	for (const Case* c : CasesOfKind(vulat::kXgkick))
	{
		const std::string tag(c->tag);
		VuTestHarness h(1);
		ASSERT_TRUE(vulat_common::RunConsoleCase(
			*c, h, XgkickProgram(kXgPresetQw, XgTestQuadword(tag))))
			<< tag;

		EXPECT_EQ(ReplayPath1IntoLabel(h.Path1PacketBytesJit(), c->lblid_pre, c->tag),
		          c->lblid_post)
			<< tag << " (arm64 recompiler)";
		EXPECT_EQ(ReplayPath1IntoLabel(h.Path1PacketBytesInterp(), c->lblid_pre, c->tag),
		          c->lblid_post)
			<< tag << " (interpreter)";
		++scored;
	}
	EXPECT_EQ(scored, 7) << "the capture lost XGKICK rows";
}

TEST(VuXgkickConsole, WrappedPacketIsNotAControlArtifact)
{
	// The controls, stated as assertions rather than left implicit. Without
	// these, "the wrapped packet produced the expected LBLID" is equally
	// consistent with LBLID never moving at all.
	//
	//   CTRL     a packet wholly inside memory moves LBLID           -> 11112222
	//   NOKICK   only the preset kick runs                           -> 5A5A5A5A
	//   TAGONLY  an NLOOP=0 tag in the last quadword changes nothing -> 5A5A5A5A
	//   WRAPNEG  a wrapped payload the GS ignores changes nothing    -> 5A5A5A5A
	//
	// so the three rows that do move LBLID moved it because of what was read
	// past the seam, not because a kick happened.
	EXPECT_EQ(CaseByTag("Q5_XG_CTRL").lblid_post, 0x11112222u);
	for (const char* tag : {"Q5_XG_NOKICK", "Q5_XG_TAGONLY", "Q5_XG_WRAPNEG"})
		EXPECT_EQ(CaseByTag(tag).lblid_post, kXgPresetVal) << tag;
	for (const char* tag : {"Q5_XG_WRAP1", "Q5_XG_WRAP2", "Q5_XG_WRAPTAG"})
		EXPECT_NE(CaseByTag(tag).lblid_post, kXgPresetVal) << tag;

	// One payload quadword past the end is quadword 0.
	EXPECT_EQ(CaseByTag("Q5_XG_WRAP1").lblid_post, 0xC0FFEE01u);
	// Two past the end are quadwords 0 and 1, each writing a disjoint half --
	// so both landing is distinguishable from either one landing.
	EXPECT_EQ(CaseByTag("Q5_XG_WRAP2").lblid_post, 0xBBBBAAAAu);
	// A packet straddling the seam reads 1022, 1023, 0, 1 -- one byte of
	// LBLID each, so the byte pattern reports the order.
	EXPECT_EQ(CaseByTag("Q5_XG_WRAPTAG").lblid_post, 0xDDCCBBAAu);
}

TEST(VuXgkickConsole, EnginesEmitTheSamePath1Stream)
{
	// Not a hardware claim -- a guard on the row above. The LBLID replay
	// collapses a packet to 32 bits, so two engines could agree on LBLID
	// while emitting different packets. This says they do not.
	for (const Case* c : CasesOfKind(vulat::kXgkick))
	{
		const std::string tag(c->tag);
		VuTestHarness h(1);
		h.SetDiffMode(VuDiffMode::XgkickPacketEquivalent);
		ASSERT_TRUE(vulat_common::RunConsoleCase(
			*c, h, XgkickProgram(kXgPresetQw, XgTestQuadword(tag))))
			<< tag;
		EXPECT_EQ(h.Path1PacketBytesJit(), h.Path1PacketBytesInterp()) << tag;
		EXPECT_FALSE(h.Path1PacketBytesJit().empty())
			<< tag << ": no Path 1 packet was emitted at all";
	}
}

TEST(VuXgkickConsole, DISABLED_DumpPath1Streams)
{
	// Reporting tool, not an assertion. Prints each engine's captured Path 1
	// byte stream quadword by quadword next to the console's LBLID, which is
	// what the two rows above are read against.
	for (const Case* c : CasesOfKind(vulat::kXgkick))
	{
		const std::string tag(c->tag);
		VuTestHarness h(1);
		ASSERT_TRUE(vulat_common::RunConsoleCase(
			*c, h, XgkickProgram(kXgPresetQw, XgTestQuadword(tag))))
			<< tag;
		std::printf("%-16s console LBLID %08X -> %08X   jit %zu bytes  interp %zu bytes\n",
		            c->tag, c->lblid_pre, c->lblid_post,
		            h.Path1PacketBytesJit().size(), h.Path1PacketBytesInterp().size());
		for (int pass = 0; pass < 2; ++pass)
		{
			const std::vector<u8>& b = pass ? h.Path1PacketBytesInterp()
			                                : h.Path1PacketBytesJit();
			std::printf("  %s:", pass ? "interp" : "jit   ");
			for (std::size_t off = 0; off + 16 <= b.size(); off += 16)
			{
				u32 q[4];
				std::memcpy(q, b.data() + off, 16);
				std::printf("\n    +%03zu  %08X %08X %08X %08X", off, q[0], q[1], q[2], q[3]);
			}
			std::printf("\n");
		}
	}
}

} // namespace recompiler_tests
