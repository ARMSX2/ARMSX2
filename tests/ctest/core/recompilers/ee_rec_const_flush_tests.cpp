// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Pending constant GPRs across the MULT/DIV emitters and the fastmem
// load/store emitters, JIT vs interpreter, including fastmem accesses that
// fault into the backpatch thunk.

#include "harness/EeRecTestHarness.h"

#include "arm64/iR5900-arm64.h"

#include "Memory.h"
#include "vtlb.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace recompiler_tests;
using namespace mips;
namespace e = mips::ee;

extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);

namespace {

constexpr u32 kPc = RecompilerTestEnvironment::kProgramPc;
constexpr u32 kData = RecompilerTestEnvironment::kScratchAddr;
constexpr u32 kWindowBytes = 256;
// Faulting stores go to INTC_STAT, which is write-1-to-clear, so word and
// doubleword stores of anything leave a zeroed register zero. Faulting loads
// read INTC_MASK: an INTC_STAT read fast-forwards cpuRegs.cycle.
constexpr u32 kMmio = 0x1000F000u;
constexpr s16 kMmioLoadOff = 0x10;

// k0 = window base, k1 = MMIO base, t9 = loop counter, ra = terminator. The
// generator never writes any of them.
constexpr u32 kPool[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                         16, 17, 18, 19, 20, 21, 22, 23, 24, 28, 29, 30};
constexpr u32 kPoolN = sizeof(kPool) / sizeof(kPool[0]);

struct Lcg
{
	u64 s;
	u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(s >> 33); }
	u32 range(u32 n) { return next() % n; }
	u64 next64() { return (static_cast<u64>(next()) << 32) | next(); }
};

enum Site : u32
{
	kMult, kMultu, kDiv, kDivu, kMult3, kMultu3, kMult1, kMultu1, kDiv1, kDivu1,
	kMadd, kMaddu, kMadd1, kMaddu1,
	kLb, kLbu, kLh, kLhu, kLw, kLwu, kLd, kLq,
	kSb, kSh, kSw, kSd, kSq,
	kLqc2, kSqc2,
	kSiteCount
};

bool IsMulDiv(u32 s) { return s <= kMaddu1; }

// Access size in bytes for the L/S sites.
u32 AccessBytes(u32 s)
{
	switch (s)
	{
		case kLb: case kLbu: case kSb: return 1;
		case kLh: case kLhu: case kSh: return 2;
		case kLw: case kLwu: case kSw: return 4;
		case kLd: case kSd: return 8;
		default: return 16;
	}
}

// The MMIO accesses that can stand in for a faulting fastmem access.
bool CanFault(u32 s)
{
	switch (s)
	{
		case kLb: case kLbu: case kLh: case kLhu: case kLw: case kLwu: case kLd: case kLq: case kLqc2:
		case kSw: case kSd:
			return true;
		default:
			return false;
	}
}

struct Gen
{
	Lcg r;
	std::vector<u32> prog;
	u32 pinned[kPoolN], plain[kPoolN];
	u32 pinnedN = 0, plainN = 0;
	u32 live[4] = {}; // recently written constant regs
	u32 liveN = 0;
	u32 siteHits[kSiteCount] = {};
	u32 faultSites = 0;

	explicit Gen(u64 seed) : r{seed}
	{
		for (u32 g : kPool)
		{
			if (armEEPinForGPR(static_cast<int>(g)))
				pinned[pinnedN++] = g;
			else
				plain[plainN++] = g;
		}
	}

	u32 anyReg() { return kPool[r.range(kPoolN)]; }
	u32 constTarget() { return r.range(2) ? pinned[r.range(pinnedN)] : plain[r.range(plainN)]; }
	u32 liveConst() { return liveN ? live[r.range(liveN)] : anyReg(); }

	void noteConst(u32 g)
	{
		if (liveN < 4)
			live[liveN++] = g;
		else
			live[r.range(4)] = g;
	}

	// One constant into g, over the edge values.
	void emitConst(u32 g)
	{
		switch (r.range(11))
		{
			case 0: prog.push_back(ORI(g, reg::zero, 0)); break;
			case 1: prog.push_back(ADDIU(g, reg::zero, 1)); break;
			case 2: prog.push_back(ADDIU(g, reg::zero, -1)); break;
			case 3: prog.push_back(LUI(g, 0x8000)); break; // INT_MIN
			case 4: prog.push_back(LUI(g, 0x7FFF)); prog.push_back(ORI(g, g, 0xFFFF)); break;
			case 5: prog.push_back(ORI(g, reg::zero, static_cast<u16>(r.next()))); break;
			case 6: prog.push_back(LUI(g, static_cast<u16>(r.next()))); prog.push_back(ORI(g, g, static_cast<u16>(r.next()))); break;
			case 7: prog.push_back(e::DADDIU(g, reg::zero, static_cast<s16>(r.next()))); break;
			case 8: prog.push_back(ADDIU(g, reg::zero, static_cast<s16>(r.next()))); prog.push_back(e::DSLL32(g, g, r.range(32))); break;
			case 9: prog.push_back(LUI(g, 0x0002)); prog.push_back(ORI(g, g, static_cast<u16>(r.range(kWindowBytes) & ~15u))); break; // window address
			default: prog.push_back(ADDIU(g, reg::zero, static_cast<s16>(1 + r.range(16)))); break;
		}
		noteConst(g);
	}

	void emitConsts(u32 n)
	{
		for (u32 i = 0; i < n; i++)
			emitConst(constTarget());
	}

	// rs/rt/rd for a mul/div site. Roles: a fresh constant as the operand, a
	// pending constant as the destination, or neither, with the three aliases.
	// `alone` emits the op with no producers in front (a delay slot).
	void emitMulDiv(u32 s, bool alone)
	{
		u32 rs = anyReg(), rt = anyReg(), rd = anyReg();
		switch (r.range(5))
		{
			case 0: rs = alone ? liveConst() : constTarget(); if (!alone) emitConst(rs); break;
			case 1: rt = alone ? liveConst() : constTarget(); if (!alone) emitConst(rt); break;
			case 2:
				rs = alone ? liveConst() : constTarget();
				rt = alone ? liveConst() : constTarget();
				if (!alone) { emitConst(rs); emitConst(rt); }
				break;
			case 3: rd = liveConst(); break;
			default: break;
		}
		switch (r.range(5))
		{
			case 0: rd = rs; break;
			case 1: rd = rt; break;
			case 2: rt = rs; break;
			default: break;
		}
		switch (s)
		{
			case kMult: prog.push_back(MULT(rs, rt)); break;
			case kMultu: prog.push_back(MULTU(rs, rt)); break;
			case kDiv: prog.push_back(DIV(rs, rt)); break;
			case kDivu: prog.push_back(DIVU(rs, rt)); break;
			case kMult3: prog.push_back(RType(0, rs, rt, rd, 0, 0x18)); break;
			case kMultu3: prog.push_back(RType(0, rs, rt, rd, 0, 0x19)); break;
			case kMult1: prog.push_back(e::MULT1(rd, rs, rt)); break;
			case kMultu1: prog.push_back(e::MULTU1(rd, rs, rt)); break;
			case kDiv1: prog.push_back(e::DIV1(rs, rt)); break;
			case kDivu1: prog.push_back(e::DIVU1(rs, rt)); break;
			case kMadd: prog.push_back(e::MADD(rd, rs, rt)); break;
			case kMaddu: prog.push_back(e::MADDU(rd, rs, rt)); break;
			case kMadd1: prog.push_back(e::MMI(rs, rt, rd, 0, 0x20)); break;
			default: prog.push_back(e::MMI(rs, rt, rd, 0, 0x21)); break;
		}
	}

	// A load/store site. The base is the window (k0), a constant window
	// address, or the MMIO page (k1, a runtime base, so the access takes the
	// fastmem path and faults). The data register is a fresh constant, a
	// pending constant, the base itself, or anything.
	void emitLoadStore(u32 s, bool fault, bool alone)
	{
		const u32 bytes = AccessBytes(s);
		u32 base = reg::k0;
		s16 off = static_cast<s16>(r.range(kWindowBytes - bytes + 1) & ~(bytes - 1));
		if (fault)
		{
			base = reg::k1;
			off = (s == kSw || s == kSd) ? 0 : kMmioLoadOff;
			faultSites++;
		}
		else if (!alone && r.range(3) == 0)
		{
			base = constTarget();
			prog.push_back(LUI(base, 0x0002));
			noteConst(base);
		}

		u32 rt = anyReg();
		switch (r.range(4))
		{
			case 0:
				if (alone)
				{
					rt = liveConst();
					break;
				}
				do
					rt = constTarget();
				while (rt == base);
				emitConst(rt);
				break;
			case 1: rt = liveConst(); break;
			case 2: if (base != reg::k0 && base != reg::k1) rt = base; break;
			default: break;
		}
		const u32 vf = 1 + r.range(31);
		switch (s)
		{
			case kLb: prog.push_back(LB(rt, off, base)); break;
			case kLbu: prog.push_back(LBU(rt, off, base)); break;
			case kLh: prog.push_back(LH(rt, off, base)); break;
			case kLhu: prog.push_back(LHU(rt, off, base)); break;
			case kLw: prog.push_back(LW(rt, off, base)); break;
			case kLwu: prog.push_back(e::LWU(rt, off, base)); break;
			case kLd: prog.push_back(e::LD(rt, off, base)); break;
			case kLq: prog.push_back(e::LQ(rt, off, base)); break;
			case kSb: prog.push_back(SB(rt, off, base)); break;
			case kSh: prog.push_back(SH(rt, off, base)); break;
			case kSw: prog.push_back(SW(rt, off, base)); break;
			case kSd: prog.push_back(e::SD(rt, off, base)); break;
			case kSq: prog.push_back(e::SQ(rt, off, base)); break;
			case kLqc2: prog.push_back(e::LQC2(vf, base, off)); break;
			default: prog.push_back(e::SQC2(vf, base, off)); break;
		}
	}

	void emitSite(bool alone = false)
	{
		const u32 s = r.range(kSiteCount);
		siteHits[s]++;
		if (IsMulDiv(s))
			emitMulDiv(s, alone);
		else
			emitLoadStore(s, CanFault(s) && r.range(3) == 0, alone);
	}

	// What reads the pending constants after the site.
	void emitConsumer()
	{
		const u32 c = liveConst(), x = anyReg(), y = anyReg();
		switch (r.range(9))
		{
			case 0: break;                                              // the block tail
			case 1: emitConst(c); break;                                // the old value goes dead
			case 2: prog.push_back(ADDU(x, c, y)); break;
			case 3: prog.push_back(e::SD(c, static_cast<s16>(r.range(kWindowBytes / 8) * 8), reg::k0)); break;
			case 4: prog.push_back(e::SQ(c, static_cast<s16>(r.range(kWindowBytes / 16) * 16), reg::k0)); break;
			case 5: prog.push_back(e::PCPYLD(x, c, y)); break;
			case 6: prog.push_back(e::PDIVW(c, y)); break;              // interpreter call
			case 7: prog.push_back(r.range(2) ? MFLO(x) : MFHI(x)); break;
			default: emitSite(); break;
		}
	}

	void emitUnit()
	{
		emitConsts(r.range(3));
		switch (r.range(6))
		{
			case 0:
			case 1:
				emitSite();
				break;
			case 2:
			{
				// The site in a delay slot, the branch skipping a few constants.
				const u32 a = r.range(2) ? liveConst() : anyReg(), b = anyReg();
				const u32 kind = r.range(4);
				const size_t at = prog.size();
				prog.push_back(NOP);
				emitSite(true);
				for (u32 k = 1 + r.range(3); k > 0; k--)
					emitConst(constTarget()); // skipped when taken
				const s16 off = static_cast<s16>(prog.size() - (at + 1));
				switch (kind)
				{
					case 0: prog[at] = BEQ(a, b, off); break;
					case 1: prog[at] = BNE(a, b, off); break;
					case 2: prog[at] = e::BEQL(a, b, off); break;
					default: prog[at] = e::BNEL(a, b, off); break;
				}
				break;
			}
			case 3:
			{
				// A block boundary between the constants and the site.
				const u32 target = kPc + static_cast<u32>(prog.size() + 2) * 4;
				prog.push_back(J(target));
				prog.push_back(NOP);
				emitSite();
				break;
			}
			case 4:
			{
				// The site in a J's delay slot, then a new block.
				const u32 target = kPc + static_cast<u32>(prog.size() + 2) * 4;
				prog.push_back(J(target));
				emitSite(true);
				break;
			}
			default:
				emitSite();
				emitSite();
				break;
		}
		emitConsumer();
	}
};

void SeedState(EeRecTestHarness& h, Lcg& r)
{
	for (u32 i = 1; i < 32; ++i)
	{
		if (i == reg::k0 || i == reg::k1 || i == reg::ra)
			continue;
		h.SetGpr128(i, r.next64(), r.next64());
	}
	h.SetGpr64(reg::k0, kData);
	h.SetGpr64(reg::k1, kMmio);
	h.SetLoPair(r.next64(), r.next64());
	h.SetHiPair(r.next64(), r.next64());
	for (u32 i = 0; i < kWindowBytes / 4; ++i)
		h.WriteU32(kData + i * 4, r.next());
	h.TrackMemWindow(kData, kWindowBytes);
}

u32 SeedCount(u32 def)
{
	if (const char* c = std::getenv("EEFUZZ_COUNT"))
		return static_cast<u32>(std::strtoul(c, nullptr, 10));
	return def;
}

// MOVK Wd/Xd, #imm16, lsl #16 anywhere in the block at guest_pc. The upper
// half, because a block that checks its own guest words for SMC materializes
// those words too, and their lower halves are the instructions' immediates.
u32 MovkHighCount(u32 guest_pc, u32 imm16)
{
	uptr fn = 0, lut = 0;
	u32 size = 0;
	if (!recEeBlockHostInfo(guest_pc, &fn, &size, &lut))
	{
		ADD_FAILURE() << "no compiled block at 0x" << std::hex << guest_pc;
		return 0;
	}
	const u32* code = reinterpret_cast<const u32*>(fn);
	u32 n = 0;
	for (u32 i = 0; i < size / 4; i++)
		n += (code[i] & 0x7FE00000u) == 0x72A00000u && ((code[i] >> 5) & 0xFFFF) == imm16;
	return n;
}

} // namespace

TEST(EeRecConstFlush, DirtyConstantsAcrossMulDivAndFastmemSites)
{
	const char* only_env = std::getenv("EEFUZZ_SEED");
	const u32 only = only_env ? static_cast<u32>(std::atoi(only_env)) : ~0u;
	u32 sites[kSiteCount] = {};
	u32 fault_seeds = 0, faulted_seeds = 0;

	const u32 count = SeedCount(3000);
	for (u32 seed = 0; seed < count; ++seed)
	{
		if (only != ~0u && seed != only)
			continue;
		SCOPED_TRACE(::testing::Message() << "seed=" << seed);
		Gen g{seed * 0x9E3779B97F4A7C15ull + 0xC0257ull};
		EeRecTestHarness h;
		h.EnableVu0Capture();
		SeedState(h, g.r);
		psHu32(kMmio) = 0;

		const bool loop = g.r.range(4) == 0;
		size_t loop_start = 0;
		if (loop)
		{
			g.prog.push_back(ADDIU(reg::t9, reg::zero, static_cast<s16>(2 + g.r.range(3))));
			loop_start = g.prog.size();
		}
		const u32 units = 6 + g.r.range(10);
		for (u32 u = 0; u < units; u++)
			g.emitUnit();
		if (loop)
		{
			g.prog.push_back(ADDIU(reg::t9, reg::t9, -1));
			const s16 off = static_cast<s16>(static_cast<s32>(loop_start) - static_cast<s32>(g.prog.size() + 1));
			g.prog.push_back(BNE(reg::t9, reg::zero, off));
			g.prog.push_back(NOP);
		}
		for (u32 s = 0; s < kSiteCount; s++)
			sites[s] += g.siteHits[s];

		vtlb_ClearLoadStoreInfo(); // the faulting-PC set is process-global
		h.LoadProgram(g.prog);
		h.Run();

		if (g.faultSites)
		{
			fault_seeds++;
			bool faulted = false;
			for (size_t i = 0; i < g.prog.size(); i++)
				faulted |= vtlb_IsFaultingPC(kPc + static_cast<u32>(i) * 4);
			faulted_seeds += faulted;
		}
		if (::testing::Test::HasFailure())
		{
			std::fprintf(stderr, "EEFUZZ FAILING seed=%u prog:\n", seed);
			for (size_t i = 0; i < g.prog.size(); ++i)
				std::fprintf(stderr, "  %3zu: %08x\n", i, g.prog[i]);
			return;
		}
	}
	vtlb_ClearLoadStoreInfo();

	if (only != ~0u)
		return;
	for (u32 s = 0; s < kSiteCount; s++)
		EXPECT_GE(sites[s], count / 4) << "site family " << s << " generated " << sites[s] << " times";
	EXPECT_GE(faulted_seeds, fault_seeds * 9 / 10)
		<< faulted_seeds << " of " << fault_seeds << " programs with an MMIO access took the fastmem fault";
	EXPECT_GE(fault_seeds, count / 2);
}

TEST(EeRecConstFlush, DeadConstantIsNotWrittenBackAtMulDivOrFastmemSite)
{
	const struct
	{
		const char* name;
		u32 site;
	} cases[] = {
		{"DIVU", DIVU(reg::a0, reg::a1)},
		{"MULT1", e::MULT1(reg::v1, reg::a0, reg::a1)},
		{"MADD", e::MADD(reg::v1, reg::a0, reg::a1)},
		{"LW", LW(reg::v1, 8, reg::s2)},
		{"SD", e::SD(reg::a0, 16, reg::s2)},
		{"LQ", e::LQ(reg::v1, 16, reg::s2)},
		{"SQ", e::SQ(reg::a0, 32, reg::s2)},
		{"LQC2", e::LQC2(3, reg::s2, 16)},
		{"SQC2", e::SQC2(3, reg::s2, 32)},
	};
	for (const auto& c : cases)
	{
		SCOPED_TRACE(c.name);
		for (const u32 dead : {reg::t1, reg::s0}) // unpinned and pinned
		{
			SCOPED_TRACE(::testing::Message() << "dead const in r" << dead);
			EeRecTestHarness h;
			h.EnableVu0Capture();
			h.SetGpr64(reg::a0, 0x12345678);
			h.SetGpr64(reg::a1, 0xFFFFFFFF87654321ull);
			h.SetGpr64(reg::s2, kData);
			for (u32 i = 0; i < 16; i++)
				h.WriteU32(kData + i * 4, 0xCAFE0000u + i);
			h.TrackMemWindow(kData, 64);
			h.LoadProgram({LUI(dead, 0x1234), ORI(dead, dead, 0x5678), c.site, ORI(dead, reg::zero, 5)});
			h.Run();
			h.ExpectGpr64(dead, 5);
			EXPECT_EQ(MovkHighCount(kPc, 0x1234), 0u) << "0x12345678 was materialized for a write-back at the site";
		}
	}
}
