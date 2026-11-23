// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU1 chunk store (microVU_ChunkStore-arm64.h).
//
// Each test compiles a routine at one address, then uploads the micro image
// again with the routine somewhere else and other code around it, as a game
// that moves its routines does. The second image is a new program. The store
// has to serve it without compiling anything, and the run has to equal the
// interpreter's run of the second image. The tests of what must not be served
// (a compile-time word that differs, a missing branch target, a place where
// the code does not fit) check that the JIT compiled instead and that the
// result is still the interpreter's.

#include "harness/VuTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "VU.h"
#include "VUmicro.h"
#include "Config.h"
#include "arm64/microVU_ChunkStore-arm64.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/vuaot/VuAotLive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

constexpr u32 kMicroBytes = 0x4000;
constexpr u32 kLowerNop = 0x8000033Cu;

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp Nop() { return LowerOnly(kLowerNop); }
inline VuOp LoadViImm(u32 dst, u32 imm) { return LowerOnly(VIADDIU_L(dst, vi::vi0, imm)); }

constexpr u32 PairIndex(u32 base, u32 n) { return ((base / 8) + n) & 0x7FFu; }

void WritePair(u32 addr, const VuOp& op)
{
	addr &= kMicroBytes - 8;
	std::memcpy(vuRegs[1].Micro + addr + 0, &op.lower, 4);
	std::memcpy(vuRegs[1].Micro + addr + 4, &op.upper, 4);
}

// A whole micro image: the harness's own two pairs at address 0, then code
// that differs with `seed` in every pair, then `pairs` at `base`. Writing
// micro memory behind the recompiler's back needs the Clear a real upload
// performs.
void Upload(u32 seed, u32 base, const std::vector<VuOp>& pairs)
{
	WritePair(0, EBitNopPair());
	WritePair(8, Nop());
	for (u32 n = 2; n < kMicroBytes / 8; n++)
		WritePair(n * 8, LoadViImm(vi::vi13, (seed * 37 + n * 5) & 0x3FF));
	for (size_t n = 0; n < pairs.size(); n++)
		WritePair(base + static_cast<u32>(n) * 8, pairs[n]);
	CpuMicroVU1.Clear(0, kMicroBytes);
}

struct Regs
{
	u32 vi[16];
	u32 tpc;
};

// The integer registers and TPC of the last JIT run against the interpreter's,
// whole: both leave a link value unwrapped and TPC wrapped, as the console does.
void ExpectJitMatches(const VuTestHarness& h, const VuSnapshot& interp)
{
	for (u32 r = 0; r < 16; r++)
		EXPECT_EQ(h.GetViJit(r) & 0xFFFFu, interp.regs.VI[r].UL & 0xFFFFu) << "vi" << r;
	EXPECT_EQ(h.GetViJit(REG_TPC), interp.regs.VI[REG_TPC].UL) << "TPC";
}

using Seed = std::function<void(VuTestHarness&)>;

// The interpreter's result for image (`seed`, `base`, `pairs`) started at
// `start`, from a harness of its own.
VuSnapshot InterpOf(u32 seed, u32 base, const std::vector<VuOp>& pairs, u32 start, const Seed& regs = nullptr)
{
	VuTestHarness h(1);
	if (regs)
		regs(h);
	h.LoadProgram({EBitNopPair()});
	Upload(seed, base, pairs);
	h.SetStartPc(start);
	h.RunInterpOnly();
	return h.InterpSnapshot();
}

// A JIT harness with the image (`seed`, `base`, `pairs`) compiled from `start`
// and left in the code cache. (The harness's first run resets the recompiler
// between its two engines, so the compile that stays is the second run's.)
struct Jit
{
	VuTestHarness h{1};

	Jit(u32 seed, u32 base, const std::vector<VuOp>& pairs, u32 start, const Seed& regs = nullptr)
	{
		if (regs)
			regs(h);
		h.LoadProgram({EBitNopPair()});
		Upload(seed, base, pairs);
		h.SetStartPc(start);
		h.RunNoDiff();
		h.RunJitPreserveBlockCache();
	}

	// Uploads another image and runs it from `start`. Returns how many
	// blocks the run compiled.
	u64 RunUpload(u32 seed, u32 base, const std::vector<VuOp>& pairs, u32 start)
	{
		Upload(seed, base, pairs);
		return RunAt(start);
	}

	u64 RunAt(u32 start)
	{
		const u64 before = mVUPersist::GetBlockCompileCount(1);
		h.SetStartPc(start);
		h.RunJitPreserveBlockCache();
		return mVUPersist::GetBlockCompileCount(1) - before;
	}
};

struct ScopedIbitHack
{
	bool prev;
	explicit ScopedIbitHack(bool on) : prev(EmuConfig.Gamefixes.IbitHack) { EmuConfig.Gamefixes.IbitHack = on; }
	~ScopedIbitHack() { EmuConfig.Gamefixes.IbitHack = prev; }
};

struct ScopedVu1TStop
{
	u32 prev;
	ScopedVu1TStop() : prev(vuRegs[0].VI[REG_FBRST].UL) { vuRegs[0].VI[REG_FBRST].UL = prev | 0x800u; }
	~ScopedVu1TStop() { vuRegs[0].VI[REG_FBRST].UL = prev; }
};

// A subroutine call and return through BAL, JALR and JR, a counted loop, a
// forward B and the end. Pairs 11 and 20 are never reached and never read by
// the compiler; `unread` goes in them.
std::vector<VuOp> CallLoopProgram(u32 unread = 0x44)
{
	return {
		LowerOnly(VBAL_L(vi::vi5, +13)),                 // 0: to 14, vi5 = 2
		Nop(),                                           // 1
		LoadViImm(vi::vi1, 3),                           // 2: JR returns here
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi2, 5)),       // 3: loop head
		LowerOnly(VIADDI_L(vi::vi1, vi::vi1, -1)),       // 4
		Nop(),                                           // 5
		Nop(),                                           // 6
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, -5)),        // 7: back to 3
		Nop(),                                           // 8
		LowerOnly(VB_L(+2)),                             // 9: to 12
		LoadViImm(vi::vi3, 0x33),                        // 10: delay slot
		LoadViImm(vi::vi4, unread),                      // 11: skipped
		EBit(LoadViImm(vi::vi6, 0x66)),                  // 12
		Nop(),                                           // 13
		LoadViImm(vi::vi11, 0xBB),                       // 14: subroutine
		LowerOnly(VIADDIU_L(vi::vi9, vi::vi5, 19)),      // 15: vi9 = pair 21
		Nop(),                                           // 16
		Nop(),                                           // 17
		LowerOnly(VJALR_L(vi::vi8, vi::vi9)),            // 18: to 21, vi8 = 20
		Nop(),                                           // 19
		LoadViImm(vi::vi10, unread),                     // 20: skipped
		LoadViImm(vi::vi12, 0xCC),                       // 21
		Nop(),                                           // 22
		Nop(),                                           // 23
		LowerOnly(VJR_L(vi::vi5)),                       // 24: to 2
		Nop(),                                           // 25
	};
}

// A counted loop, a forward B over one pair, then the end.
std::vector<VuOp> LoopProgram(u32 passes)
{
	return {
		LoadViImm(vi::vi1, passes),                      // 0
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi2, 5)),       // 1: loop head
		LowerOnly(VIADDI_L(vi::vi1, vi::vi1, -1)),       // 2
		Nop(),                                           // 3
		Nop(),                                           // 4
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, -5)),        // 5: back to 1
		Nop(),                                           // 6
		LowerOnly(VB_L(+2)),                             // 7: to 10
		LoadViImm(vi::vi3, 0x33),                        // 8: delay slot
		LoadViImm(vi::vi4, 0x44),                        // 9: skipped
		LoadViImm(vi::vi5, 0x55),                        // 10
		EBitNopPair(),                                   // 11
		Nop(),                                           // 12
	};
}

// A loop that calls a subroutine on every pass. The JR that returns starts a
// second compile at pair 4, and that compile's pass back through pair 1 calls
// the subroutine the first compile emitted: a branch from one chunk into
// another. The subroutine returns through JALR, so vi8 holds an address that
// is only right if the branch reached the subroutine of the same copy.
std::vector<VuOp> CallInLoopProgram(u32 passes)
{
	return {
		LoadViImm(vi::vi1, passes),                      // 0
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi2, 5)),       // 1: loop head
		LowerOnly(VBAL_L(vi::vi5, +7)),                  // 2: to 10, vi5 = 4
		Nop(),                                           // 3
		LowerOnly(VIADDI_L(vi::vi1, vi::vi1, -1)),       // 4: the subroutine returns here
		LowerOnly(VIBNE_L(vi::vi1, vi::vi0, -5)),        // 5: back to 1
		Nop(),                                           // 6
		EBit(LoadViImm(vi::vi6, 0x66)),                  // 7
		Nop(),                                           // 8
		Nop(),                                           // 9
		LowerOnly(VIADDIU_L(vi::vi11, vi::vi11, 3)),     // 10: subroutine
		LowerOnly(VJALR_L(vi::vi8, vi::vi5)),            // 11: to 4, vi8 = 13
		Nop(),                                           // 12
	};
}

// Two entry points: pair 0 is a routine, pair 12 runs a few pairs and
// branches to it.
std::vector<VuOp> TwoEntryProgram()
{
	return {
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi2, 7)),       // 0: first entry
		Nop(),                                           // 1
		LoadViImm(vi::vi3, 0x33),                        // 2
		EBitNopPair(),                                   // 3
		Nop(),                                           // 4
		Nop(),                                           // 5
		Nop(),                                           // 6
		Nop(),                                           // 7
		Nop(),                                           // 8
		Nop(),                                           // 9
		Nop(),                                           // 10
		Nop(),                                           // 11
		LoadViImm(vi::vi4, 0x44),                        // 12: second entry
		Nop(),                                           // 13
		Nop(),                                           // 14
		Nop(),                                           // 15
		LowerOnly(VB_L(-17)),                            // 16: to 0
		Nop(),                                           // 17
	};
}

// Two branches in a row: the second sits in the first one's delay slot.
struct Shape
{
	const char* name;
	u32 first;
	u32 second;
};
const Shape kDelaySlotShapes[] = {
	{"B then B", VB_L(+4), VB_L(+7)},
	{"B then BAL", VB_L(+4), VBAL_L(vi::vi5, +7)},
	{"B then IBNE", VB_L(+4), VIBNE_L(vi::vi1, vi::vi0, +7)},
	{"IBNE then B", VIBNE_L(vi::vi1, vi::vi0, +4), VB_L(+7)},
	{"IBNE then IBNE", VIBNE_L(vi::vi1, vi::vi0, +4), VIBNE_L(vi::vi2, vi::vi0, +7)},
	{"IBNE then BAL", VIBNE_L(vi::vi1, vi::vi0, +4), VBAL_L(vi::vi5, +7)},
	{"BAL then BAL", VBAL_L(vi::vi6, +4), VBAL_L(vi::vi5, +7)},
};

std::vector<VuOp> DelaySlotProgram(const Shape& s)
{
	std::vector<VuOp> p = {Nop(), LowerOnly(s.first), LowerOnly(s.second)};
	for (u32 n = 3; n < 14; n++)
		p.push_back(LowerOnly(VIADDIU_L(vi::vi10, vi::vi10, 1u << (n - 3))));
	for (u32 n = 0; n < 16; n++)
		p.push_back((n & 1) ? LoadViImm(vi::vi15, 0x700 + n) : EBit(LoadViImm(vi::vi14, 0x700 + n)));
	return p;
}

std::vector<VuOp> IbitProgram(u32 f, u32 a, u32 b, s32 c, s32 d, u32 e, s16 lw, s16 sw, u32 dst = vi::vi1)
{
	return {
		IBit(VuOp{VLitI(f), VNOP_U()}),                                  // 0: I = f
		IBit(VuOp{VLitZero(), VMULi_U(mask::xyzw, vf::vf2, vf::vf1)}),   // 1: vf2 = vf1 * I
		LowerOnly(VIADDIU_L(dst, vi::vi0, a)),                           // 2
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi1, b)),                       // 3
		LowerOnly(VIADDI_L(vi::vi3, vi::vi0, c)),                        // 4
		LowerOnly(VIADDI_L(vi::vi4, vi::vi2, d)),                        // 5
		LowerOnly(VISUBIU_L(vi::vi5, vi::vi2, e)),                       // 6
		LowerOnly(VILW_L(mask::x, vi::vi6, vi::vi0, lw)),                // 7
		LowerOnly(VISW_L(mask::x, vi::vi2, vi::vi0, sw)),                // 8
		EBitNopPair(),                                                   // 9
		Nop(),                                                           // 10
	};
}

void SeedIbit(VuTestHarness& h)
{
	h.SetVfBits(vf::vf1, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u);
	h.WriteMemU128(0x30, 0x1234, 0, 0, 0);
	h.WriteMemU128(0x50, 0x4321, 0, 0, 0);
	h.TrackMemWindow(0x40, 16);
	h.TrackMemWindow(0x60, 16);
}

constexpr u32 kHome = 0x0400;
constexpr u32 kCopy = 0x2000;

// Where a copy of `pairs` ends on the last pair of micro memory.
inline u32 TopOf(const std::vector<VuOp>& pairs) { return kMicroBytes - static_cast<u32>(pairs.size()) * 8; }

} // namespace

// The call/loop routine moved and uploaded next to other code: branch
// targets, BAL and JALR link values, the JR and JALR exits and the TPC the E
// bit leaves. The pairs the compiler never read differ between the two images
// as well. In the middle of micro memory and ending on its last pair.
TEST(Vu1ChunkStore, RoutineUploadedAtAnotherAddressIsServedWithoutACompile)
{
	for (const u32 copy : {kCopy, TopOf(CallLoopProgram())})
	{
		SCOPED_TRACE(::testing::Message() << "copy at 0x" << std::hex << copy);
		const VuSnapshot interp = InterpOf(2, copy, CallLoopProgram(0x45), copy);
		ASSERT_EQ(interp.regs.VI[vi::vi2].UL & 0xFFFFu, 15u);
		ASSERT_EQ(interp.regs.VI[vi::vi12].UL & 0xFFFFu, 0xCCu);

		Jit jit(1, kHome, CallLoopProgram(), kHome);
		const mVUChunkStore::Stats before = mVUChunkStore::GetStats();
		EXPECT_EQ(jit.RunUpload(2, copy, CallLoopProgram(0x45), copy), 0u) << "the copy was compiled";
		const mVUChunkStore::Stats after = mVUChunkStore::GetStats();
		EXPECT_GT(after.hits, before.hits);
		EXPECT_EQ(after.episodes - before.episodes, after.hits - before.hits) << "every compile asked for was served";
		EXPECT_EQ(after.entries, before.entries) << "serving a chunk adds no entry";
		EXPECT_EQ(after.fed, before.fed);
		EXPECT_EQ(jit.h.GetViJit(vi::vi5), PairIndex(copy, 2));
		EXPECT_EQ(jit.h.GetViJit(vi::vi8), PairIndex(copy, 20));
		EXPECT_EQ(jit.h.GetViJit(REG_TPC), PairIndex(copy, 14));
		ExpectJitMatches(jit.h, interp);

		// And again from the jump caches the first run filled.
		EXPECT_EQ(jit.RunAt(copy), 0u);
		ExpectJitMatches(jit.h, interp);
	}
}

// A served copy's JR and JALR blocks record the targets they reach in jump
// caches of their own, where the VU AOT tier looks for them, not in those of
// the blocks the code was compiled as.
TEST(Vu1ChunkStore, ServedCopyRecordsItsJumpTargetsInItsOwnBlocks)
{
	const VuSnapshot interp = InterpOf(2, kCopy, CallLoopProgram(0x45), kCopy);
	Jit jit(1, kHome, CallLoopProgram(), kHome);
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kHome + 14 * 8), std::vector<u32>{kHome + 21 * 8});

	EXPECT_EQ(jit.RunUpload(2, kCopy, CallLoopProgram(0x45), kCopy), 0u) << "the copy was compiled";
	ExpectJitMatches(jit.h, interp);
	// The JALR in the block at pair 14 goes to pair 21; the JR in the block
	// at pair 21 returns to pair 2.
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kCopy + 14 * 8), std::vector<u32>{kCopy + 21 * 8});
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kCopy + 21 * 8), std::vector<u32>{kCopy + 2 * 8});
	EXPECT_EQ(jit.RunAt(kCopy), 0u);
	ExpectJitMatches(jit.h, interp);
}

// The same image holding the routine twice is one program: the second copy is
// installed next to the first.
TEST(Vu1ChunkStore, SecondCopyInTheSameProgramIsServed)
{
	std::vector<VuOp> both = CallLoopProgram();
	both.resize((kCopy - kHome) / 8, Nop());
	const std::vector<VuOp> copy = CallLoopProgram();
	both.insert(both.end(), copy.begin(), copy.end());
	const VuSnapshot interp = InterpOf(1, kHome, both, kCopy);

	Jit jit(1, kHome, both, kHome);
	EXPECT_EQ(jit.RunAt(kCopy), 0u) << "the copy was compiled";
	EXPECT_EQ(jit.h.GetViJit(vi::vi5), PairIndex(kCopy, 2));
	ExpectJitMatches(jit.h, interp);
}

// Two copies of a routine in one program run the same code. The JR and JALR
// blocks of each copy record the targets that copy reached, and not those of
// the other copy.
TEST(Vu1ChunkStore, CopiesInOneProgramRecordTheirOwnJumpTargets)
{
	std::vector<VuOp> both = CallLoopProgram();
	both.resize((kCopy - kHome) / 8, Nop());
	const std::vector<VuOp> copy = CallLoopProgram();
	both.insert(both.end(), copy.begin(), copy.end());
	const VuSnapshot interp = InterpOf(1, kHome, both, kCopy);

	Jit jit(1, kHome, both, kHome);
	EXPECT_EQ(jit.RunAt(kCopy), 0u) << "the copy was compiled";
	ExpectJitMatches(jit.h, interp);
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kHome + 14 * 8), std::vector<u32>{kHome + 21 * 8});
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kHome + 21 * 8), std::vector<u32>{kHome + 2 * 8});
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kCopy + 14 * 8), std::vector<u32>{kCopy + 21 * 8});
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kCopy + 21 * 8), std::vector<u32>{kCopy + 2 * 8});

	// Again, from the jump caches the first runs filled.
	EXPECT_EQ(jit.RunAt(kHome), 0u);
	EXPECT_EQ(jit.RunAt(kCopy), 0u);
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kHome + 14 * 8), std::vector<u32>{kHome + 21 * 8});
	EXPECT_EQ(mVUChunkStore::TestJumpTargets(kCopy + 14 * 8), std::vector<u32>{kCopy + 21 * 8});
}

// Copies of a routine run the code compiled once and use its jump tables: a
// placed copy adds no table, in another program or at another address.
TEST(Vu1ChunkStore, PlacedCopiesAddNoJumpTable)
{
	const u32 bases[] = {kCopy, 0x1000, 0x3000};
	std::vector<VuSnapshot> interp;
	for (u32 n = 0; n < 3; n++)
		interp.push_back(InterpOf(2 + n, bases[n], CallLoopProgram(0x45), bases[n]));

	Jit jit(1, kHome, CallLoopProgram(), kHome);
	const u64 compiled = mvu_test_hooks::GetJumpTableUsage().liveTables;
	ASSERT_GE(compiled, 2u) << "the JALR block and the JR block each have a table";
	for (u32 n = 0; n < 3; n++)
	{
		SCOPED_TRACE(::testing::Message() << "copy at 0x" << std::hex << bases[n]);
		EXPECT_EQ(jit.RunUpload(2 + n, bases[n], CallLoopProgram(0x45), bases[n]), 0u) << "the copy was compiled";
		ExpectJitMatches(jit.h, interp[n]);
		EXPECT_EQ(mVUChunkStore::TestJumpTargets(bases[n] + 14 * 8), std::vector<u32>{bases[n] + 21 * 8});
		EXPECT_EQ(mVUChunkStore::TestJumpTargets(bases[n] + 21 * 8), std::vector<u32>{bases[n] + 2 * 8});
	}
	EXPECT_EQ(mvu_test_hooks::GetJumpTableUsage().liveTables, compiled) << "a placed copy has jump tables of its own";
}

// The E bit and the T bit on branches and on a plain pair: the TPC an exit
// stores is the copy's.
TEST(Vu1ChunkStore, ExitTpcOfAServedCopy)
{
	for (const bool tbit : {false, true})
	{
		for (const u32 seed : {0u, 1u})
		{
			SCOPED_TRACE(::testing::Message() << (tbit ? "T bit" : "E bit") << " vi1 " << seed);
			ScopedVu1TStop tstop;
			const auto flag = [tbit](VuOp op) { return tbit ? TBit(op) : EBit(op); };
			const std::vector<VuOp> prog = {
				Nop(),                                           // 0
				flag(LowerOnly(VIBNE_L(vi::vi1, vi::vi0, +5))),  // 1: to 7
				LoadViImm(vi::vi2, 0x22),                        // 2
				flag(LoadViImm(vi::vi3, 0x33)),                  // 3
				EBitNopPair(),                                   // 4
				Nop(),                                           // 5
				Nop(),                                           // 6
				flag(LowerOnly(VB_L(+3))),                       // 7: to 11
				LoadViImm(vi::vi4, 0x44),                        // 8
				Nop(),                                           // 9
				Nop(),                                           // 10
				EBitNopPair(),                                   // 11
				Nop(),                                           // 12
			};
			const Seed regs = [seed](VuTestHarness& h) { h.SetVi(vi::vi1, seed); };
			const VuSnapshot interp = InterpOf(2, kCopy, prog, kCopy, regs);

			Jit jit(1, kHome, prog, kHome, regs);
			EXPECT_EQ(jit.RunUpload(2, kCopy, prog, kCopy), 0u) << "the copy was compiled";
			EXPECT_EQ(jit.h.GetViJit(REG_TPC), interp.regs.VI[REG_TPC].UL);
			ExpectJitMatches(jit.h, interp);
		}
	}
}

// A branch in another branch's delay slot: the addresses the block stores for
// the continuation, in the middle of micro memory and at its end.
TEST(Vu1ChunkStore, BranchInDelaySlotOfAServedCopy)
{
	for (const Shape& s : kDelaySlotShapes)
	{
		const std::vector<VuOp> prog = DelaySlotProgram(s);
		for (const u32 copy : {kCopy, TopOf(prog)})
		{
			for (const u32 seed : {0u, 1u, 2u, 3u})
			{
				SCOPED_TRACE(::testing::Message() << s.name << " seed " << seed << " copy at 0x" << std::hex << copy);
				const Seed regs = [seed](VuTestHarness& h) {
					h.SetVi(vi::vi1, seed & 1);
					h.SetVi(vi::vi2, seed >> 1);
				};
				const VuSnapshot interp = InterpOf(2, copy, prog, copy, regs);

				Jit jit(1, kHome, prog, kHome, regs);
				EXPECT_EQ(jit.RunUpload(2, copy, prog, copy), 0u) << "the copy was compiled";
				ExpectJitMatches(jit.h, interp);
			}
		}
	}
}

// Under the IbitHack gamefix a block reads its immediates when it runs. A copy
// whose immediates differ is the same code: it is served and reads its own. A
// copy with another destination register is other code: it is compiled.
TEST(Vu1ChunkStore, IbitHackImmediateIsMaskedAndARegisterFieldIsNot)
{
	ScopedIbitHack hack(true);
	const std::vector<VuOp> home = IbitProgram(0x40400000u, 0x123, 0x456, -7, 5, 0x21, 3, 4);
	const std::vector<VuOp> imm = IbitProgram(0x40A00000u, 0x321, 0x111, 9, -3, 0x12, 5, 6);
	const std::vector<VuOp> reg = IbitProgram(0x40A00000u, 0x321, 0x111, 9, -3, 0x12, 5, 6, vi::vi7);
	const VuSnapshot interpImm = InterpOf(2, kCopy, imm, kCopy, SeedIbit);
	const VuSnapshot interpReg = InterpOf(3, kCopy + 0x800, reg, kCopy + 0x800, SeedIbit);
	ASSERT_EQ(interpImm.regs.VF[vf::vf2].i.x, 0x40A00000u);
	ASSERT_EQ(interpImm.regs.VI[vi::vi2].UL & 0xFFFFu, 0x432u);
	ASSERT_EQ(interpReg.regs.VI[vi::vi7].UL & 0xFFFFu, 0x321u);

	Jit jit(1, kHome, home, kHome, SeedIbit);
	ASSERT_EQ(jit.h.GetVfBitsJit(vf::vf2, 'x'), 0x40400000u);

	EXPECT_EQ(jit.RunUpload(2, kCopy, imm, kCopy), 0u) << "a copy that differs in immediates was compiled";
	EXPECT_EQ(jit.h.GetVfBitsJit(vf::vf2, 'x'), 0x40A00000u);
	EXPECT_EQ(jit.h.GetMemU32Jit(0x60), 0x432u) << "ISW of vi2 at the copy's offset";
	ExpectJitMatches(jit.h, interpImm);

	const mVUChunkStore::Stats before = mVUChunkStore::GetStats();
	EXPECT_GT(jit.RunUpload(3, kCopy + 0x800, reg, kCopy + 0x800), 0u) << "code with another register was served";
	const mVUChunkStore::Stats after = mVUChunkStore::GetStats();
	EXPECT_EQ(after.hits, before.hits);
	EXPECT_GT(after.missReadSet, before.missReadSet);
	EXPECT_EQ(jit.h.GetViJit(vi::vi7), 0x321u);
	ExpectJitMatches(jit.h, interpReg);
}

// A served copy compares the words whose immediates it reads at run time
// through their masks, as a compiled program does (microProgram::keptBits): a
// rewrite of one of those immediates in place keeps the program, and the copy
// reads the new immediate.
TEST(Vu1ChunkStore, ServedCopyKeepsItsKeptBitWords)
{
	ScopedIbitHack hack(true);
	const std::vector<VuOp> home = IbitProgram(0x40400000u, 0x123, 0x456, -7, 5, 0x21, 3, 4);
	const std::vector<VuOp> copy = IbitProgram(0x40A00000u, 0x321, 0x111, 9, -3, 0x12, 5, 6);
	std::vector<VuOp> rewritten = copy;
	rewritten[3] = LowerOnly(VIADDIU_L(vi::vi2, vi::vi1, 0x222));
	const VuSnapshot interp = InterpOf(2, kCopy, rewritten, kCopy, SeedIbit);
	ASSERT_EQ(interp.regs.VI[vi::vi2].UL & 0xFFFFu, 0x543u);

	Jit jit(1, kHome, home, kHome, SeedIbit);
	ASSERT_EQ(jit.RunUpload(2, kCopy, copy, kCopy), 0u) << "the copy was compiled";

	WritePair(kCopy + 3 * 8, rewritten[3]);
	CpuMicroVU1.Clear(kCopy + 3 * 8, 8);

	const mVUChunkStore::Stats before = mVUChunkStore::GetStats();
	EXPECT_EQ(jit.RunAt(kCopy), 0u);
	const mVUChunkStore::Stats after = mVUChunkStore::GetStats();
	EXPECT_EQ(after.episodes, before.episodes) << "the rewrite of an immediate dropped the served program";
	EXPECT_EQ(jit.h.GetViJit(vi::vi2), 0x543u);
	ExpectJitMatches(jit.h, interp);
}

// Under the gamefix, a copy whose last pair is the last pair of micro memory is
// served and reads its own immediates. A copy whose reads run across the end
// is not served: it is compiled where it is, and its reads past the end come
// from the bottom of micro memory.
TEST(Vu1ChunkStore, IbitHackCopyAcrossTheTopReadsItsOwnImmediates)
{
	ScopedIbitHack hack(true);
	const std::vector<VuOp> home = IbitProgram(0x40400000u, 0x123, 0x456, -7, 5, 0x21, 3, 4);
	const std::vector<VuOp> copy = IbitProgram(0x40A00000u, 0x321, 0x111, 9, -3, 0x12, 5, 6);
	const u32 top = kMicroBytes - static_cast<u32>(copy.size()) * 8;
	// Pairs 6 to 10 of this one (the ISUBIU, ILW and ISW among them) sit at
	// the bottom of micro memory.
	const u32 across = kMicroBytes - 6 * 8;
	const VuSnapshot interpTop = InterpOf(2, top, copy, top, SeedIbit);
	const VuSnapshot interpAcross = InterpOf(3, across, copy, across, SeedIbit);
	ASSERT_EQ(interpAcross.regs.VI[vi::vi5].UL & 0xFFFFu, 0x432u - 0x12u);
	ASSERT_EQ(interpAcross.regs.VI[vi::vi6].UL & 0xFFFFu, 0x4321u);

	Jit jit(1, kHome, home, kHome, SeedIbit);
	EXPECT_EQ(jit.RunUpload(2, top, copy, top), 0u) << "the copy at the top was compiled";
	EXPECT_EQ(jit.h.GetVfBitsJit(vf::vf2, 'x'), 0x40A00000u);
	EXPECT_EQ(jit.h.GetMemU32Jit(0x60), 0x432u) << "ISW of vi2 at the copy's offset";
	ExpectJitMatches(jit.h, interpTop);

	const mVUChunkStore::Stats before = mVUChunkStore::GetStats();
	EXPECT_GT(jit.RunUpload(3, across, copy, across), 0u) << "a copy whose reads run across the end was served";
	const mVUChunkStore::Stats after = mVUChunkStore::GetStats();
	EXPECT_EQ(after.hits, before.hits);
	EXPECT_GT(after.missBounds, before.missBounds);
	EXPECT_EQ(jit.h.GetVfBitsJit(vf::vf2, 'x'), 0x40A00000u);
	EXPECT_EQ(jit.h.GetMemU32Jit(0x60), 0x432u);
	ExpectJitMatches(jit.h, interpAcross);
}

// Entered at pair 4, which branches back to pair 0: the compile that starts
// at pair 4 also emits the block at pair 0, whose reads sit below the chunk's
// root.
std::vector<VuOp> ReadBelowTheRootProgram(u32 a, s16 lw)
{
	return {
		LowerOnly(VIADDIU_L(vi::vi1, vi::vi0, a)),           // 0
		LowerOnly(VILW_L(mask::x, vi::vi6, vi::vi0, lw)),    // 1
		EBitNopPair(),                                       // 2
		Nop(),                                               // 3
		LowerOnly(VIADDIU_L(vi::vi2, vi::vi0, 0x55)),        // 4: entry
		LowerOnly(VB_L(-6)),                                 // 5: to 0
		Nop(),                                               // 6
	};
}

TEST(Vu1ChunkStore, IbitHackReadBelowTheRootOfAServedCopy)
{
	ScopedIbitHack hack(true);
	const std::vector<VuOp> home = ReadBelowTheRootProgram(0x123, 3);
	const std::vector<VuOp> copy = ReadBelowTheRootProgram(0x321, 5);
	const u32 entry = 4 * 8;
	const VuSnapshot interp = InterpOf(2, kCopy, copy, kCopy + entry, SeedIbit);
	ASSERT_EQ(interp.regs.VI[vi::vi1].UL & 0xFFFFu, 0x321u);
	ASSERT_EQ(interp.regs.VI[vi::vi6].UL & 0xFFFFu, 0x4321u);

	Jit jit(1, kHome, home, kHome + entry, SeedIbit);
	ASSERT_EQ(jit.h.GetViJit(vi::vi1), 0x123u);
	ASSERT_EQ(jit.h.GetViJit(vi::vi6), 0x1234u);
	EXPECT_EQ(jit.RunUpload(2, kCopy, copy, kCopy + entry), 0u) << "the copy was compiled";
	EXPECT_EQ(jit.h.GetViJit(vi::vi1), 0x321u);
	EXPECT_EQ(jit.h.GetViJit(vi::vi6), 0x4321u);
	ExpectJitMatches(jit.h, interp);
}

// Without the gamefix an immediate is compiled in: a copy with another one is
// other code.
TEST(Vu1ChunkStore, ImmediateCompiledInIsPartOfTheKey)
{
	std::vector<VuOp> other = LoopProgram(3);
	other[10] = LoadViImm(vi::vi5, 0x56);
	const VuSnapshot interpSame = InterpOf(2, kCopy, LoopProgram(3), kCopy);
	const VuSnapshot interpOther = InterpOf(3, kCopy + 0x800, other, kCopy + 0x800);

	Jit jit(1, kHome, LoopProgram(3), kHome);
	EXPECT_EQ(jit.RunUpload(2, kCopy, LoopProgram(3), kCopy), 0u);
	ExpectJitMatches(jit.h, interpSame);
	EXPECT_GT(jit.RunUpload(3, kCopy + 0x800, other, kCopy + 0x800), 0u) << "code with another immediate was served";
	EXPECT_EQ(jit.h.GetViJit(vi::vi5), 0x56u);
	ExpectJitMatches(jit.h, interpOther);
}

// A branch from one chunk into another goes to the block of the copy it runs
// as, in a new program and in the program that holds the original.
TEST(Vu1ChunkStore, CrossChunkBranchResolvedPerPlacement)
{
	const std::vector<VuOp> prog = CallInLoopProgram(6);
	{
		SCOPED_TRACE("new program");
		const VuSnapshot interp = InterpOf(2, kCopy, prog, kCopy);
		ASSERT_EQ(interp.regs.VI[vi::vi11].UL & 0xFFFFu, 21u);

		Jit jit(1, kHome, prog, kHome);
		EXPECT_EQ(jit.h.GetViJit(vi::vi8), PairIndex(kHome, 13));
		EXPECT_EQ(jit.RunUpload(2, kCopy, prog, kCopy), 0u) << "the copy was compiled";
		EXPECT_EQ(jit.h.GetViJit(vi::vi8), PairIndex(kCopy, 13)) << "the branch out of the chunk reached another copy's subroutine";
		ExpectJitMatches(jit.h, interp);
	}
	{
		SCOPED_TRACE("same program");
		std::vector<VuOp> both = prog;
		both.resize((kCopy - kHome) / 8, Nop());
		both.insert(both.end(), prog.begin(), prog.end());
		const VuSnapshot interp = InterpOf(1, kHome, both, kCopy);

		Jit jit(1, kHome, both, kHome);
		EXPECT_EQ(jit.RunAt(kCopy), 0u) << "the copy was compiled";
		EXPECT_EQ(jit.h.GetViJit(vi::vi8), PairIndex(kCopy, 13));
		ExpectJitMatches(jit.h, interp);
		// The original still runs as itself.
		EXPECT_EQ(jit.RunAt(kHome), 0u);
		EXPECT_EQ(jit.h.GetViJit(vi::vi8), PairIndex(kHome, 13));
	}
}

// A chunk that branches to a block outside it cannot be installed in a program
// that does not have that block: the JIT compiles. What it compiled is kept as
// well, and the next program in the same situation is served.
TEST(Vu1ChunkStore, MissingBranchTargetIsCompiledInstead)
{
	const std::vector<VuOp> prog = TwoEntryProgram();
	const u32 second = 12 * 8;
	const VuSnapshot interpB = InterpOf(2, kCopy, prog, kCopy + second);
	const VuSnapshot interpC = InterpOf(3, kCopy + 0x800, prog, kCopy + 0x800 + second);
	const VuSnapshot interpD = InterpOf(4, kCopy + 0x1000, prog, kCopy + 0x1000 + second);
	ASSERT_EQ(interpB.regs.VI[vi::vi4].UL & 0xFFFFu, 0x44u);
	ASSERT_EQ(interpB.regs.VI[vi::vi2].UL & 0xFFFFu, 7u);

	// First entry, then the second: the second's chunk branches to the first's block.
	Jit jit(1, kHome, prog, kHome);
	ASSERT_GT(jit.RunAt(kHome + second), 0u);

	// A new program entered at the second entry only: the block is not there.
	const mVUChunkStore::Stats before = mVUChunkStore::GetStats();
	EXPECT_GT(jit.RunUpload(2, kCopy, prog, kCopy + second), 0u) << "served without its branch target";
	const mVUChunkStore::Stats after = mVUChunkStore::GetStats();
	EXPECT_GT(after.missLinkTarget, before.missLinkTarget);
	EXPECT_EQ(after.hits, before.hits);
	EXPECT_GT(after.fed, before.fed);
	ExpectJitMatches(jit.h, interpB);

	// The same again: served by what that compile produced.
	EXPECT_EQ(jit.RunUpload(3, kCopy + 0x800, prog, kCopy + 0x800 + second), 0u);
	ExpectJitMatches(jit.h, interpC);

	// First entry, then the second, in a new program: both served, the
	// second by the chunk that branches.
	Upload(4, kCopy + 0x1000, prog);
	EXPECT_EQ(jit.RunAt(kCopy + 0x1000), 0u);
	EXPECT_EQ(jit.RunAt(kCopy + 0x1000 + second), 0u);
	ExpectJitMatches(jit.h, interpD);
}

// A cycle budget that runs out inside a served copy: the break parks the
// copy's own loop block, and re-entering until the end gives the interpreter's
// result, in the middle of micro memory and at its end.
TEST(Vu1ChunkStore, BudgetBreakAndResumeInAServedCopy)
{
	const std::vector<VuOp> prog = LoopProgram(200);
	for (const u32 copy : {kCopy, TopOf(prog)})
	{
		SCOPED_TRACE(::testing::Message() << "copy at 0x" << std::hex << copy);
		const VuSnapshot interp = InterpOf(2, copy, prog, copy);
		ASSERT_EQ(interp.regs.VI[vi::vi2].UL & 0xFFFFu, 1000u);

		Jit jit(1, kHome, prog, kHome);
		jit.h.SetCycleBudget(40);
		EXPECT_EQ(jit.RunUpload(2, copy, prog, copy), 0u) << "the copy was compiled";
		ASSERT_FALSE(jit.h.HasTerminated());
		EXPECT_EQ(jit.h.GetViJit(REG_TPC), PairIndex(copy, 1)) << "the break parks the loop block of the copy";
		const u64 compiled = mVUPersist::GetBlockCompileCount(1);
		jit.h.ContinueJit(40, 400);
		ASSERT_TRUE(jit.h.HasTerminated());
		EXPECT_EQ(mVUPersist::GetBlockCompileCount(1), compiled);
		ExpectJitMatches(jit.h, interp);
	}
}

// A copy whose last pair is the last pair of micro memory is served. One pair
// higher the code would run across the end: there the JIT compiles.
TEST(Vu1ChunkStore, CopyNearTheTopOfMicroMemory)
{
	const std::vector<VuOp> prog = LoopProgram(3);
	const u32 top = kMicroBytes - static_cast<u32>(prog.size()) * 8;
	const VuSnapshot interpTop = InterpOf(2, top, prog, top);
	const VuSnapshot interpOver = InterpOf(3, top + 8, prog, top + 8);

	Jit jit(1, kHome, prog, kHome);
	EXPECT_EQ(jit.RunUpload(2, top, prog, top), 0u) << "the copy at the top was compiled";
	EXPECT_EQ(jit.h.GetViJit(REG_TPC), 0u) << "TPC after the last pair of micro memory";
	ExpectJitMatches(jit.h, interpTop);

	const mVUChunkStore::Stats before = mVUChunkStore::GetStats();
	EXPECT_GT(jit.RunUpload(3, top + 8, prog, top + 8), 0u) << "a copy that runs across the end was served";
	const mVUChunkStore::Stats after = mVUChunkStore::GetStats();
	EXPECT_EQ(after.hits, before.hits);
	EXPECT_GT(after.missBounds, before.missBounds);
	EXPECT_GT(after.feedWrapped, before.feedWrapped) << "a chunk that ran across the end was kept";
	ExpectJitMatches(jit.h, interpOver);
}

// Variants of a routine that differ in an immediate compiled in share a root
// and are told apart by their read sets, one plain body each. A root keeps
// its 16 used last: a new variant past that replaces the one used least
// recently, and a variant that was served stays.
TEST(Vu1ChunkStore, PlainBodiesOfARootAreBoundedAndTheLeastRecentlyUsedGoes)
{
	const auto variant = [](u32 k) {
		std::vector<VuOp> v = LoopProgram(3);
		v[10] = LoadViImm(vi::vi5, 0x100 + k);
		return v;
	};
	u32 seed = 2;
	Jit jit(1, kHome, variant(0), kHome);
	const mVUChunkStore::Stats s0 = mVUChunkStore::GetStats();
	for (u32 k = 1; k < 16; k++)
		ASSERT_GT(jit.RunUpload(seed++, kHome, variant(k), kHome), 0u) << "variant " << k << " was served";
	const mVUChunkStore::Stats s1 = mVUChunkStore::GetStats();
	ASSERT_EQ(s1.entries - s0.entries, 15u);
	EXPECT_EQ(s1.feedEvicted, s0.feedEvicted);

	// Variant 0 served at another address is used last now; variant 1 is the
	// least recently used, and the 17th variant replaces it.
	EXPECT_EQ(jit.RunUpload(seed++, kCopy, variant(0), kCopy), 0u);
	EXPECT_GT(jit.RunUpload(seed++, kHome, variant(16), kHome), 0u);
	const mVUChunkStore::Stats s2 = mVUChunkStore::GetStats();
	EXPECT_EQ(s2.entries, s1.entries) << "the root holds more than 16 plain bodies";
	EXPECT_EQ(s2.feedEvicted - s1.feedEvicted, 1u);

	EXPECT_GT(jit.RunUpload(seed++, kCopy + 0x400, variant(1), kCopy + 0x400), 0u) << "the least recently used variant was kept";
	EXPECT_EQ(jit.RunUpload(seed++, kCopy + 0x800, variant(0), kCopy + 0x800), 0u) << "the variant used last was dropped";
	EXPECT_EQ(jit.RunUpload(seed++, kCopy + 0xC00, variant(16), kCopy + 0xC00), 0u);
	EXPECT_EQ(mVUChunkStore::GetStats().entries, s1.entries);
	ExpectJitMatches(jit.h, InterpOf(seed - 1, kCopy + 0xC00, variant(16), kCopy + 0xC00));
}

// A program the store serves costs a few KiB of code, so the code cache does
// not bound how many there are. Their memory does: once the programs and
// blocks reach the bound, the dispatcher exit resets the recompiler, and the
// reset frees every one of them.
TEST(Vu1ChunkStore, ProgramMemoryOfServedProgramsIsBounded)
{
	// Every program and block counted is uncounted when it is freed: a reset
	// leaves nothing, whatever the tests before this one did.
	RecompilerTestEnvironment::ResetVuBlockCache(1);
	ASSERT_EQ(mvu_test_hooks::GetProgramMemory(1).bytes, 0u);

	const std::vector<VuOp> routines[2] = {LoopProgram(3), CallLoopProgram()};
	Jit jit(1, kHome, routines[0], kHome);
	jit.RunUpload(2, kHome + 0x800, routines[1], kHome + 0x800);

	const mvu_test_hooks::ProgramMemory m0 = mvu_test_hooks::GetProgramMemory(1);
	ASSERT_GT(m0.programs, 0u);
	ASSERT_GE(m0.bytes, m0.programs * m0.programBytes);
	u64 peak = m0.bytes, served = 0, compiled = 0;
	u32 seed = 3;
	bool reset = false;
	mvu_test_hooks::ProgramMemory last = m0;
	for (u32 base = 0x100; !reset && base + 26 * 8 <= kMicroBytes; base += 8)
	{
		for (const std::vector<VuOp>& routine : routines)
		{
			const u64 compiles = jit.RunUpload(seed++, base, routine, base);
			const mvu_test_hooks::ProgramMemory m = mvu_test_hooks::GetProgramMemory(1);
			ASSERT_LT(m.bytes, m.resetAt) << "a dispatch ended with the programs over the bound";
			if (m.bytes < last.bytes)
			{
				reset = true;
				// Freed with the reset: what is left is the program the run
				// after it made.
				EXPECT_LE(m.programs, 1u);
				EXPECT_LT(m.bytes, 2 * m.programBytes);
				break;
			}
			(compiles ? compiled : served)++;
			peak = std::max(peak, m.bytes);
			last = m;
		}
	}
	ASSERT_TRUE(reset) << "no reset after " << served + compiled << " programs, " << peak << " bytes";
	EXPECT_GT(served, 10 * compiled) << "the programs were not made from placements";
	EXPECT_GE(peak + m0.programBytes + 64 * m0.blockBytes, m0.resetAt) << "the reset came before the bound";
}

// The VU AOT tier builds a routine's regions in one compile of its own, which
// the store is not asked about; what that compile emits is kept as the
// routine's tiered body.
TEST(Vu1ChunkStore, TieredBodyBuildIsNotLookedUp)
{
	ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
	const bool savedOpt = EmuConfig.Cpu.Recompiler.EnableVUAot;
	const bool savedFlagHack = EmuConfig.Speedhacks.vuFlagHack;
	EmuConfig.Cpu.Recompiler.EnableVUAot = true;
	EmuConfig.Speedhacks.vuFlagHack = true;
	vuaot::live::ResetStickyTripwire();
	vuaot::live::SetTierUpRuns(6);
	vuaot::live::SetSynchronous(true);
	{
		const std::vector<VuOp> prog = LoopProgram(10);
		const VuSnapshot interp = InterpOf(2, kCopy, prog, kCopy);

		Jit jit(1, kHome, prog, kHome);
		ASSERT_TRUE(vuaot::live::IsEnabled());
		const mVUChunkStore::Stats s0 = mVUChunkStore::GetStats();
		const vuaot::live::Stats t0 = vuaot::live::GetStats();

		// Not tiered yet: the copy is served the plain body.
		EXPECT_EQ(jit.RunUpload(2, kCopy, prog, kCopy), 0u);
		const mVUChunkStore::Stats s1 = mVUChunkStore::GetStats();
		EXPECT_GT(s1.hits, s0.hits);
		EXPECT_EQ(s1.skipped, s0.skipped);
		ExpectJitMatches(jit.h, interp);

		// Run it until the tier builds the routine's regions.
		for (int i = 0; i < 12; i++)
		{
			jit.RunAt(kCopy);
			ExpectJitMatches(jit.h, interp);
		}
		const mVUChunkStore::Stats s2 = mVUChunkStore::GetStats();
		const vuaot::live::Stats t2 = vuaot::live::GetStats();
		EXPECT_EQ(t2.tierUps, t0.tierUps + 1) << "the routine was not tiered up";
		EXPECT_EQ(s2.tieredBuilds, s1.tieredBuilds + 1);
		EXPECT_EQ(s2.skipped, s1.skipped + 1) << "only the build goes round the store";
		EXPECT_EQ(s2.tieredBodies, 1u);
		EXPECT_EQ(s2.entries, s1.entries) << "the build added a plain body";
	}
	EmuConfig.Cpu.Recompiler.EnableVUAot = savedOpt;
	EmuConfig.Speedhacks.vuFlagHack = savedFlagHack;
	vuaot::live::ResetStickyTripwire();
	vuaot::live::SetTierUpRuns(vuaot::live::kTierUpRuns);
	vuaot::live::SetSynchronous(false);
	RecompilerTestEnvironment::ResetVuBlockCache(1);
}

} // namespace recompiler_tests
