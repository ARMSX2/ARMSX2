// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The VU1 chunk store's disk pack (microVU_ChunkPack-arm64.inl): a body the
// store gained in one session is a record of the pack, and the next session
// makes the body again from the record instead of compiling.
//
// A "session" here is everything but the process boundary: the pack is closed
// (which writes it), the recompiler is reset (which empties the store), and
// the pack is opened and read again. Each test first runs its routine with no
// pack at all, for the result a cold compile gives, then with a pack, then in
// a new session, where the routine has to run with no compile at every root
// and give the cold result and the interpreter's.

#if defined(_M_ARM64) || defined(__aarch64__)

#include "harness/ProgCachePackMirror.h"
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_ChunkStore-arm64.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/microVU_ProgCache-arm64.h"
#include "arm64/vuaot/VuAotLive.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#if defined(__linux__)
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace recompiler_tests
{

using namespace vu;

namespace
{
constexpr u32 kMicroBytes = 0x4000;
constexpr u32 kLowerNop = 0x8000033Cu;
constexpr u32 kHome = 0x0400;
constexpr u32 kTierRuns = 6;
constexpr u8 kRecordBody = progcache_pack::kBody;
// Set by APackFromAnotherProcessRunsHere for the child it spawns: the cache
// root the child writes its pack under.
constexpr char kChildEnv[] = "VU1_CHUNK_PACK_CHILD_ROOT";

inline VuOp L(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp P(u32 lower, u32 upper) { return VuOp{lower, upper}; }
inline VuOp Nop() { return L(kLowerNop); }
inline VuOp LoadViImm(u32 dst, u32 imm) { return L(VIADDIU_L(dst, vi::vi0, imm)); }

void WritePair(u32 addr, const VuOp& op)
{
	addr &= kMicroBytes - 8;
	std::memcpy(vuRegs[1].Micro + addr + 0, &op.lower, 4);
	std::memcpy(vuRegs[1].Micro + addr + 4, &op.upper, 4);
}

// A whole micro image: the harness's two pairs at address 0, code that
// differs with `seed` in every other pair, then `pairs` at `base`.
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

void SeedRegs(VuTestHarness& h)
{
	h.SetVf(1, 2.0f, 3.0f, 4.0f, 5.0f);
	h.SetVf(2, 0.5f, 1.5f, 2.5f, 3.5f);
	h.SetVf(3, 1.0f, 1.0f, 1.0f, 1.0f);
	h.SetVf(4, 1.0001f, 0.9999f, 1.00001f, 0.5f);
	h.SetVf(5, 0.25f, -0.5f, 8.0f, 1.0f);
	h.TrackMemWindow(0, 0x400);
}

// A subroutine call and return through BAL, JALR and JR, a counted loop of
// FMACs with a store, a forward B and the end: several chunks, a link between
// chunks, a jump table, link values and the end's TPC.
std::vector<VuOp> CallLoopRoutine()
{
	return {
		L(VBAL_L(vi::vi5, +13)),                                                                 // 0: to 14, vi5 = 2
		Nop(),                                                                                   // 1
		LoadViImm(vi::vi1, 3),                                                                   // 2: JR returns here
		P(VIADDIU_L(vi::vi2, vi::vi2, 5), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),        // 3: loop head
		P(VIADDI_L(vi::vi1, vi::vi1, -1), VADD_U(mask::xyz, vf::vf5, vf::vf5, vf::vf3)),         // 4
		P(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0), VSUB_U(mask::xyzw, vf::vf7, vf::vf5, vf::vf3)), // 5
		Nop(),                                                                                   // 6
		L(VIBNE_L(vi::vi1, vi::vi0, -5)),                                                        // 7: back to 3
		Nop(),                                                                                   // 8
		L(VB_L(+2)),                                                                             // 9: to 12
		LoadViImm(vi::vi3, 0x33),                                                                // 10: delay slot
		LoadViImm(vi::vi4, 0x44),                                                                // 11: skipped
		EBit(LoadViImm(vi::vi6, 0x66)),                                                          // 12
		Nop(),                                                                                   // 13
		LoadViImm(vi::vi11, 0xBB),                                                               // 14: subroutine
		L(VIADDIU_L(vi::vi9, vi::vi5, 19)),                                                      // 15: vi9 = pair 21
		Nop(),                                                                                   // 16
		Nop(),                                                                                   // 17
		L(VJALR_L(vi::vi8, vi::vi9)),                                                            // 18: to 21, vi8 = 20
		Nop(),                                                                                   // 19
		LoadViImm(vi::vi10, 0x44),                                                               // 20: skipped
		LoadViImm(vi::vi12, 0xCC),                                                               // 21
		Nop(),                                                                                   // 22
		Nop(),                                                                                   // 23
		L(VJR_L(vi::vi5)),                                                                       // 24: to 2
		Nop(),                                                                                   // 25
	};
}

// A counted loop of FMACs, a flag reader and a store, then the end: what the
// VU AOT tier builds one region from.
std::vector<VuOp> LoopRoutine(u32 passes)
{
	return {
		LoadViImm(vi::vi1, passes),                                                              // 0
		LoadViImm(vi::vi2, 0x10),                                                                // 1
		P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),        // 2: loop head
		P(VFMAND_L(vi::vi3, vi::vi2), VADD_U(mask::xyz, vf::vf5, vf::vf5, vf::vf3)),             // 3
		P(VSQ_L(mask::xyzw, vf::vf5, vi::vi2, 0), VSUB_U(mask::xyzw, vf::vf7, vf::vf5, vf::vf3)), // 4
		L(VIADDI_L(vi::vi1, vi::vi1, -1)),                                                       // 5
		L(VIBNE_L(vi::vi1, vi::vi0, -5)),                                                        // 6: back to 2
		Nop(),                                                                                   // 7
		EBit(LoadViImm(vi::vi6, 0x66)),                                                          // 8
		Nop(),                                                                                   // 9
	};
}

// A loop that calls a subroutine through BAL on every pass; the subroutine
// returns through JALR. The tier takes the JALR's recorded target as a direct
// edge of its region.
std::vector<VuOp> CallRoutine(u32 passes)
{
	return {
		LoadViImm(vi::vi1, passes),                                                              // 0
		LoadViImm(vi::vi2, 0x10),                                                                // 1
		P(VIADDIU_L(vi::vi2, vi::vi2, 1), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),        // 2: loop head
		P(kLowerNop, VADD_U(mask::xyzw, vf::vf5, vf::vf5, vf::vf3)),                             // 3
		L(VBAL_L(vi::vi5, +7)),                                                                  // 4: to 12, vi5 = 6
		Nop(),                                                                                   // 5
		L(VIADDI_L(vi::vi1, vi::vi1, -1)),                                                       // 6: the subroutine returns here
		L(VIBNE_L(vi::vi1, vi::vi0, -6)),                                                        // 7: back to 2
		Nop(),                                                                                   // 8
		EBit(LoadViImm(vi::vi6, 0x66)),                                                          // 9
		Nop(),                                                                                   // 10
		Nop(),                                                                                   // 11
		P(VIADDIU_L(vi::vi11, vi::vi11, 3), VSUB_U(mask::xyzw, vf::vf7, vf::vf5, vf::vf3)),      // 12: subroutine
		L(VJALR_L(vi::vi8, vi::vi5)),                                                            // 13: to 6, vi8 = 15
		Nop(),                                                                                   // 14
	};
}

struct Result
{
	VuSnapshot snap;
	std::vector<u8> path1;
};

Result Take(const VuTestHarness& h) { return Result{h.JitSnapshot(), h.Path1PacketBytesJit()}; }

// What differs between two JIT runs, as text. `status`: compare STATUS too (a
// region may drop its sticky bits while nothing reads them).
std::string Diff(const Result& a, const Result& b, bool status)
{
	struct F
	{
		const char* name;
		size_t off, size;
	};
#define FR(f) F{#f, offsetof(VURegs, f), sizeof(VURegs::f)}
	static const F kFields[] = {FR(VF), FR(ACC), FR(q), FR(p), FR(cycle), FR(flags), FR(ebit),
		FR(pending_q), FR(pending_p), FR(micro_macflags), FR(micro_clipflags),
		F{"VI 0-15", offsetof(VURegs, VI), 16 * sizeof(REG_VI)},
		F{"TPC", offsetof(VURegs, VI) + REG_TPC * sizeof(REG_VI), sizeof(REG_VI)},
		F{"MAC flag", offsetof(VURegs, VI) + REG_MAC_FLAG * sizeof(REG_VI), sizeof(REG_VI)},
		F{"clip flag", offsetof(VURegs, VI) + REG_CLIP_FLAG * sizeof(REG_VI), sizeof(REG_VI)}};
#undef FR
	std::string out;
	const u8* x = reinterpret_cast<const u8*>(&a.snap.regs);
	const u8* y = reinterpret_cast<const u8*>(&b.snap.regs);
	for (const F& f : kFields)
		if (std::memcmp(x + f.off, y + f.off, f.size) != 0)
			out += std::string(f.name) + " differs\n";
	if (status && a.snap.regs.VI[REG_STATUS_FLAG].UL != b.snap.regs.VI[REG_STATUS_FLAG].UL)
		out += "STATUS differs\n";
	for (size_t w = 0; w < a.snap.mem_windows.size() && w < b.snap.mem_windows.size(); w++)
		if (a.snap.mem_windows[w].bytes != b.snap.mem_windows[w].bytes)
			out += "VU memory differs\n";
	if (a.path1 != b.path1)
		out += "GIF path 1 bytes differ\n";
	return out;
}

// The integer registers and TPC of the JIT pass against the interpreter pass
// of the same run.
void ExpectMatchesInterp(const VuTestHarness& h, const char* what)
{
	const VuSnapshot& interp = h.InterpSnapshot();
	for (u32 r = 0; r < 16; r++)
		EXPECT_EQ(h.GetViJit(r) & 0xFFFFu, interp.regs.VI[r].UL & 0xFFFFu) << what << ": vi" << r;
	EXPECT_EQ(h.GetViJit(REG_TPC), interp.regs.VI[REG_TPC].UL) << what << ": TPC";
}

// One run of image (`seed`, `base`, `pairs`) from `start`: the recompiler is
// reset, the JIT runs it once, and the interpreter runs it for comparison.
// `compiles`: blocks the JIT pass compiled.
struct OneRun
{
	VuTestHarness h{1};
	u64 compiles = 0;

	OneRun(u32 seed, u32 base, const std::vector<VuOp>& pairs, u32 start)
	{
		SeedRegs(h);
		h.LoadProgram({EBitNopPair()});
		Upload(seed, base, pairs);
		h.SetStartPc(start);
		const u64 before = mVUPersist::GetBlockCompileCount(1);
		h.RunNoDiff();
		compiles = mVUPersist::GetBlockCompileCount(1) - before;
	}

	// The JIT again, keeping what is compiled.
	void Again() { h.RunJitPreserveBlockCache(); }
};

// The tier on or off for a scope, tiering a routine up after `kTierRuns` runs,
// every job waited for where it was asked for.
struct TierScope
{
	const bool savedOpt = EmuConfig.Cpu.Recompiler.EnableVUAot;
	const bool savedFlagHack = EmuConfig.Speedhacks.vuFlagHack;
	TierScope(bool on, bool flagHack)
	{
		EmuConfig.Cpu.Recompiler.EnableVUAot = on;
		EmuConfig.Speedhacks.vuFlagHack = flagHack;
		vuaot::live::ResetStickyTripwire();
		vuaot::live::SetTripwireMutation(0);
		vuaot::live::SetTierUpRuns(kTierRuns);
		vuaot::live::SetSynchronous(true);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
	}
	~TierScope()
	{
		vuaot::live::HoldWorker(false);
		vuaot::live::SetTripwireMutation(0);
		vuaot::live::ResetStickyTripwire();
		EmuConfig.Cpu.Recompiler.EnableVUAot = savedOpt;
		EmuConfig.Speedhacks.vuFlagHack = savedFlagHack;
		vuaot::live::SetTierUpRuns(vuaot::live::kTierUpRuns);
		vuaot::live::SetSynchronous(false);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
	}
};

using StoreStats = mVUChunkStore::Stats;

class Vu1ChunkPack : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
		mVUPersist::SetRecordingEnabled(true);
		char tmpl[] = "/tmp/vu1_chunk_pack_XXXXXX";
		char* p = ::mkdtemp(tmpl);
		ASSERT_NE(p, nullptr);
		root_ = p;
		cache_root_ = root_;
		prev_cache_ = EmuFolders::Cache;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
	}

	void TearDown() override
	{
		mVUPersist::TestOverrideBuildId(false, 0, 0);
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		mVUProgCache::ResetForTest(1);
		mVUPersist::SetRecordingEnabled(false);
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
		EmuFolders::Cache = prev_cache_;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		std::error_code ec;
		std::filesystem::remove_all(root_, ec);
	}

	// Brings the pack up at the temp root. Before this a test runs with no
	// pack: a cold compile at every reset.
	void OpenPack()
	{
		EmuFolders::Cache = cache_root_;
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
		mVUProgCache::ResetForTest(1);
		mVUProgCache::TestReinitFromLiveSentinel(1);
		ASSERT_TRUE(mVUProgCache::GetStats(1).enabled) << "the cache did not come up at " << cache_root_;
		mVUProgCache::TestWaitForPreload(1);
	}

	// The end of a session: the pack written and closed, the store emptied.
	// Until the next one opens there is no pack (a reset opens none).
	void EndSession()
	{
		mVUProgCache::ResetForTest(1);
		EmuFolders::Cache = prev_cache_;
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
	}

	// The next session's pack: read from disk.
	void BeginSession() { OpenPack(); }

	std::string PackFile() const
	{
		const std::vector<std::string> packs = progcache_pack::FindPacks(Path::Combine(cache_root_, "vu_jit"));
		return packs.size() == 1 ? packs[0] : std::string();
	}

	std::vector<progcache_pack::Record> Bodies() const
	{
		std::vector<progcache_pack::Record> out;
		for (progcache_pack::Record& r : progcache_pack::ReadRecords(PackFile()))
			if (r.kind == kRecordBody)
				out.push_back(std::move(r));
		return out;
	}

	// A temp directory of the test's own, removed at the end, and where the
	// pack is kept (the same, unless a test points it elsewhere).
	std::string root_;
	std::string cache_root_;
	std::string prev_cache_;
};

// The roots a routine is run at in the second session: where it was compiled
// and three other places. (The top of micro memory, where a body may not fit,
// is the chunk store's own tests'.)
std::vector<u32> Roots()
{
	return {kHome, 0x2000, 0x1238, 0x3038};
}
} // namespace

// microVU's own code. One session compiles the routine at one address; the
// next runs it at four with no compile, and every run equals the cold compile
// at that address and the interpreter.
TEST_F(Vu1ChunkPack, PlainBodiesComeBackAtEveryRootWithNoCompile)
{
	const std::vector<VuOp> routine = CallLoopRoutine();
	const std::vector<u32> roots = Roots();
	std::vector<Result> cold;
	for (size_t i = 0; i < roots.size(); i++)
	{
		OneRun run(10 + static_cast<u32>(i), roots[i], routine, roots[i]);
		EXPECT_GT(run.compiles, 0u) << "with no pack every reset compiles";
		ExpectMatchesInterp(run.h, "cold");
		cold.push_back(Take(run.h));
	}

	OpenPack();
	const StoreStats s0 = mVUChunkStore::GetStats();
	{
		OneRun run(1, kHome, routine, kHome);
		EXPECT_GT(run.compiles, 0u);
	}
	const StoreStats s1 = mVUChunkStore::GetStats();
	EXPECT_GT(s1.packSaved - s0.packSaved, 0u) << "the compile wrote no body";
	EXPECT_EQ(s1.packSaved - s0.packSaved, s1.fed - s0.fed) << "every body the store kept goes to the pack";
	EXPECT_EQ(s1.packUnpackable - s0.packUnpackable, 0u);

	EndSession();
	EXPECT_EQ(Bodies().size(), s1.packSaved - s0.packSaved) << "one record per body";
	BeginSession();
	EXPECT_EQ(mVUProgCache::GetStats(1).packsLoaded, 1u);

	for (size_t i = 0; i < roots.size(); i++)
	{
		SCOPED_TRACE(testing::Message() << "root 0x" << std::hex << roots[i]);
		const StoreStats a = mVUChunkStore::GetStats();
		OneRun run(10 + static_cast<u32>(i), roots[i], routine, roots[i]);
		const StoreStats b = mVUChunkStore::GetStats();
		EXPECT_EQ(run.compiles, 0u) << "a body in the pack was compiled again";
		EXPECT_GT(b.packLoaded - a.packLoaded, 0u);
		EXPECT_EQ(b.packRejected - a.packRejected, 0u);
		ExpectMatchesInterp(run.h, "from the pack");
		const std::string d = Diff(cold[i], Take(run.h), true);
		EXPECT_TRUE(d.empty()) << "against the cold compile: " << d;
	}
	EXPECT_EQ(mVUChunkStore::GetStats().packSaved, s1.packSaved) << "a body made from the pack was written again";
}

// A reset in the same session (a savestate load) empties the store. The
// bodies come back from the pack's table in RAM: no compile, and no read of
// the file.
TEST_F(Vu1ChunkPack, AResetBringsBodiesBackFromRam)
{
	const std::vector<VuOp> routine = CallLoopRoutine();
	OpenPack();
	OneRun first(1, kHome, routine, kHome);
	EXPECT_GT(first.compiles, 0u);
	const Result compiled = Take(first.h);
	EXPECT_TRUE(PackFile().empty()) << "nothing is on disk until the writer's pass";

	const StoreStats a = mVUChunkStore::GetStats();
	OneRun again(1, kHome, routine, kHome);
	EXPECT_EQ(again.compiles, 0u);
	EXPECT_GT(mVUChunkStore::GetStats().packLoaded - a.packLoaded, 0u) << "the bodies did not come from the pack's table";
	ExpectMatchesInterp(again.h, "after the reset");
	const std::string d = Diff(compiled, Take(again.h), true);
	EXPECT_TRUE(d.empty()) << d;
	EXPECT_EQ(mVUProgCache::GetStats(1).packsLoaded, 0u);
}

// A routine the VU AOT tier built regions for. In the next session every copy
// of it is placed on the tiered body at its first lookup: nothing is compiled,
// no region is built, and no copy runs microVU's code first.
TEST_F(Vu1ChunkPack, ATieredBodyComesBackAndRunsItsRegionsFromTheFirstDispatch)
{
	struct Case
	{
		const char* name;
		std::vector<VuOp> routine;
	};
	const Case cases[] = {{"loop", LoopRoutine(10)}, {"call", CallRoutine(9)}};
	for (const Case& c : cases)
	{
		for (bool flagHack : {true, false})
		{
			SCOPED_TRACE(testing::Message() << c.name << " flagHack=" << flagHack);
			const std::vector<u32> roots = Roots();
			// microVU's own result at each root, the tier off and no pack.
			std::vector<Result> mvu;
			{
				TierScope off(false, flagHack);
				for (size_t i = 0; i < roots.size(); i++)
				{
					OneRun run(10 + static_cast<u32>(i), roots[i], c.routine, roots[i]);
					mvu.push_back(Take(run.h));
				}
			}

			TierScope on(true, flagHack);
			OpenPack();
			const StoreStats s0 = mVUChunkStore::GetStats();
			const vuaot::live::Stats t0 = vuaot::live::GetStats();
			{
				OneRun run(1, kHome, c.routine, kHome);
				for (u32 i = 0; i < kTierRuns + 2; i++)
					run.Again();
			}
			const StoreStats s1 = mVUChunkStore::GetStats();
			const vuaot::live::Stats t1 = vuaot::live::GetStats();
			ASSERT_EQ((t1.tierUps + t1.retiers) - (t0.tierUps + t0.retiers), 1u) << "the routine was not tiered in the first session";
			EXPECT_EQ(s1.packUnpackable - s0.packUnpackable, 0u) << "the tiered body could not be written";

			EndSession();
			BeginSession();

			for (size_t i = 0; i < roots.size(); i++)
			{
				SCOPED_TRACE(testing::Message() << "root 0x" << std::hex << roots[i]);
				const StoreStats a = mVUChunkStore::GetStats();
				const vuaot::live::Stats ta = vuaot::live::GetStats();
				OneRun run(10 + static_cast<u32>(i), roots[i], c.routine, roots[i]);
				const StoreStats b = mVUChunkStore::GetStats();
				const vuaot::live::Stats tb = vuaot::live::GetStats();
				EXPECT_EQ(run.compiles, 0u) << "microVU compiled";
				EXPECT_EQ((tb.tierUps + tb.retiers) - (ta.tierUps + ta.retiers), 0u) << "a region was built again";
				// (the routine's root is the only head with a tiered body: a
				// tiered hit with no re-aim is the root placed on it at its
				// first lookup)
				EXPECT_GE(b.tieredHits - a.tieredHits, 1u) << "the copy was not placed on the tiered body";
				EXPECT_EQ(b.episodes - a.episodes, b.hits - a.hits) << "a lookup was not served";
				EXPECT_EQ(b.tieredReAimed - a.tieredReAimed, 0u) << "the copy ran microVU's code first";
				EXPECT_EQ(b.packRejected - a.packRejected, 0u);
				ExpectMatchesInterp(run.h, "tiered, from the pack");
				const std::string d = Diff(mvu[i], Take(run.h), false);
				EXPECT_TRUE(d.empty()) << "against microVU: " << d;
				// (and it goes on running there)
				run.Again();
				const std::string d2 = Diff(mvu[i], Take(run.h), false);
				EXPECT_TRUE(d2.empty()) << "second run against microVU: " << d2;
			}
			EndSession();
			std::error_code ec;
			std::filesystem::remove_all(Path::Combine(root_, "vu_jit"), ec);
		}
	}
}

// The same after a reset inside one session (a savestate load): the tiered
// body comes back from the pack's table in RAM, and the first dispatch after
// the reset runs it.
TEST_F(Vu1ChunkPack, AResetBringsATieredBodyBackFromRam)
{
	const std::vector<VuOp> routine = LoopRoutine(10);
	Result mvu;
	{
		TierScope off(false, true);
		OneRun run(1, kHome, routine, kHome);
		mvu = Take(run.h);
	}
	TierScope on(true, true);
	OpenPack();
	const vuaot::live::Stats t0 = vuaot::live::GetStats();
	{
		OneRun run(1, kHome, routine, kHome);
		for (u32 i = 0; i < kTierRuns + 2; i++)
			run.Again();
	}
	const vuaot::live::Stats t1 = vuaot::live::GetStats();
	ASSERT_EQ(t1.tierUps - t0.tierUps, 1u);

	const StoreStats a = mVUChunkStore::GetStats();
	OneRun run(1, kHome, routine, kHome); // (resets first)
	const StoreStats b = mVUChunkStore::GetStats();
	const vuaot::live::Stats t2 = vuaot::live::GetStats();
	EXPECT_EQ(run.compiles, 0u);
	EXPECT_EQ((t2.tierUps + t2.retiers) - (t1.tierUps + t1.retiers), 0u) << "the region was built again after the reset";
	EXPECT_GE(b.tieredHits - a.tieredHits, 1u) << "the copy was not placed on the tiered body";
	EXPECT_EQ(b.tieredReAimed - a.tieredReAimed, 0u) << "the copy ran microVU's code first";
	EXPECT_EQ(mVUProgCache::GetStats(1).packsLoaded, 0u) << "a reset read the file";
	ExpectMatchesInterp(run.h, "tiered, after a reset");
	const std::string d = Diff(mvu, Take(run.h), false);
	EXPECT_TRUE(d.empty()) << d;
}

// A routine tiered in one session, met in the next with other code inside
// it: no body in the pack fits, so microVU compiles it. The pack still says
// the routine was worth regions, so they are asked for at its first run, with
// the tier-up count out of reach.
TEST_F(Vu1ChunkPack, ARoutineTieredBeforeAsksForItsRegionsAtTheFirstRun)
{
	std::vector<VuOp> changed = LoopRoutine(10);
	changed[1] = LoadViImm(vi::vi2, 0x20); // (not the root pair)
	Result mvu;
	{
		TierScope off(false, true);
		OneRun run(2, 0x2000, changed, 0x2000);
		mvu = Take(run.h);
	}
	TierScope on(true, true);
	OpenPack();
	{
		const vuaot::live::Stats t0 = vuaot::live::GetStats();
		OneRun run(1, kHome, LoopRoutine(10), kHome);
		for (u32 i = 0; i < kTierRuns + 2; i++)
			run.Again();
		ASSERT_EQ(vuaot::live::GetStats().tierUps - t0.tierUps, 1u);
	}
	EndSession();
	BeginSession();
	vuaot::live::SetTierUpRuns(1000000);

	const vuaot::live::Stats t1 = vuaot::live::GetStats();
	OneRun run(2, 0x2000, changed, 0x2000);
	EXPECT_GT(run.compiles, 0u) << "a body for other code was used";
	for (u32 i = 0; i < 3; i++)
	{
		run.Again();
		const std::string d = Diff(mvu, Take(run.h), false);
		EXPECT_TRUE(d.empty()) << "run " << i << ": " << d;
	}
	const vuaot::live::Stats t2 = vuaot::live::GetStats();
	EXPECT_EQ(t2.tierUps - t1.tierUps, 1u) << "the routine did not tier up at its first runs";
	EXPECT_GE(mVUChunkStore::GetStats().tieredReAimed, 1u);
	ExpectMatchesInterp(run.h, "tiered at the first run");
}

// A routine the tier builds no region for. The answer is in the pack, and the
// next session does not ask the worker again, however often the routine runs.
TEST_F(Vu1ChunkPack, ADeclinedRoutineIsNotAskedForAgain)
{
	// One straight block to the end: nothing for a region.
	const std::vector<VuOp> routine = {
		LoadViImm(vi::vi1, 7),
		P(VIADDIU_L(vi::vi2, vi::vi2, 5), VMUL_U(mask::xyzw, vf::vf3, vf::vf3, vf::vf4)),
		Nop(),
		EBit(LoadViImm(vi::vi6, 0x66)),
		Nop(),
	};
	TierScope on(true, true);
	OpenPack();
	{
		const vuaot::live::Stats t0 = vuaot::live::GetStats();
		OneRun run(1, kHome, routine, kHome);
		for (u32 i = 0; i < kTierRuns + 2; i++)
			run.Again();
		const vuaot::live::Stats t1 = vuaot::live::GetStats();
		ASSERT_EQ(t1.jobs - t0.jobs, 1u);
		ASSERT_EQ(t1.jobsDeclined - t0.jobsDeclined, 1u) << "the builder was expected to decline this routine";
	}
	EndSession();
	BeginSession();

	for (u32 root : {kHome, 0x2000u})
	{
		const vuaot::live::Stats t1 = vuaot::live::GetStats();
		OneRun run(3, root, routine, root);
		for (u32 i = 0; i < 3 * kTierRuns; i++)
			run.Again();
		const vuaot::live::Stats t2 = vuaot::live::GetStats();
		EXPECT_EQ(t2.jobs - t1.jobs, 0u) << "the declined routine was asked for again at 0x" << std::hex << root;
		EXPECT_EQ(run.compiles, 0u);
		ExpectMatchesInterp(run.h, "declined");
	}
}

// With the tier off, a pack that holds tiered bodies gives none of them out.
TEST_F(Vu1ChunkPack, TieredBodiesAreNotUsedWithTheTierOff)
{
	const std::vector<VuOp> routine = LoopRoutine(10);
	{
		TierScope on(true, true);
		OpenPack();
		const vuaot::live::Stats t0 = vuaot::live::GetStats();
		OneRun run(1, kHome, routine, kHome);
		for (u32 i = 0; i < kTierRuns + 2; i++)
			run.Again();
		const vuaot::live::Stats t1 = vuaot::live::GetStats();
		ASSERT_EQ(t1.tierUps - t0.tierUps, 1u);
		EndSession();
	}
	TierScope off(false, true);
	BeginSession();
	const StoreStats a = mVUChunkStore::GetStats();
	OneRun run(2, 0x2000, routine, 0x2000);
	const StoreStats b = mVUChunkStore::GetStats();
	EXPECT_EQ(b.tieredHits - a.tieredHits, 0u);
	EXPECT_EQ(b.tieredPlaced - a.tieredPlaced, 0u);
	EXPECT_EQ(run.compiles, 0u) << "the plain body is still served";
	ExpectMatchesInterp(run.h, "tier off");
}

// A pack another build wrote, one written under other options, and one of
// another format or compiler ABI: none is used, and the routine compiles.
TEST_F(Vu1ChunkPack, APackOfAnotherBuildOptionsOrVersionIsNotUsed)
{
	const std::vector<VuOp> routine = CallLoopRoutine();
	const auto firstSession = [&]() {
		OpenPack();
		OneRun run(1, kHome, routine, kHome);
		EXPECT_GT(run.compiles, 0u);
		EndSession();
	};
	const auto expectCold = [&](const char* what, bool otherFormat) {
		SCOPED_TRACE(what);
		BeginSession();
		const StoreStats a = mVUChunkStore::GetStats();
		OneRun run(1, kHome, routine, kHome);
		const StoreStats b = mVUChunkStore::GetStats();
		EXPECT_GT(run.compiles, 0u) << "the pack was used";
		EXPECT_EQ(b.packLoaded - a.packLoaded, 0u);
		EXPECT_EQ(mVUProgCache::GetStats(1).otherFormat, otherFormat ? 1u : 0u);
		ExpectMatchesInterp(run.h, what);
		EndSession();
	};
	const auto wipe = [&]() {
		std::error_code ec;
		std::filesystem::remove_all(Path::Combine(root_, "vu_jit"), ec);
	};

	// Another build.
	firstSession();
	mVUPersist::TestOverrideBuildId(true, 0x1111222233334444ull, 0x5555666677778888ull);
	expectCold("another build", true);
	// (and that session replaced the file with its own, which it then reads)
	BeginSession();
	{
		OneRun run(1, kHome, routine, kHome);
		EXPECT_EQ(run.compiles, 0u) << "the pack the other build wrote for itself";
	}
	EndSession();
	mVUPersist::TestOverrideBuildId(false, 0, 0);
	wipe();

	// Other options: another file.
	firstSession();
	{
		const bool saved = EmuConfig.Cpu.Recompiler.vu1Overflow;
		EmuConfig.Cpu.Recompiler.vu1Overflow = !saved;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
		expectCold("other options", false);
		EXPECT_EQ(progcache_pack::FindPacks(Path::Combine(root_, "vu_jit")).size(), 2u) << "each options set has a pack of its own";
		EmuConfig.Cpu.Recompiler.vu1Overflow = saved;
		RecompilerTestEnvironment::ResetVuBlockCache(1);
	}
	wipe();

	// The header's format version, compiler ABI, page size and layout stamp.
	struct Field
	{
		const char* what;
		size_t offset;
	};
	const Field fields[] = {{"format version", 4}, {"compiler ABI", 8}, {"page size", 56}, {"layout stamp", 60}};
	for (const Field& f : fields)
	{
		firstSession();
		const std::string path = PackFile();
		auto bytes = FileSystem::ReadBinaryFile(path.c_str());
		ASSERT_TRUE(bytes.has_value() && bytes->size() > progcache_pack::kHeaderBytes) << f.what;
		(*bytes)[f.offset] ^= 0x01;
		ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), bytes->data(), bytes->size()));
		expectCold(f.what, true);
		wipe();
	}
}

// A pack damaged on disk. A flipped byte fails its record's checksum, and a
// file cut short ends in half a record: the log ends before either, the
// routine compiles, and nothing crashes.
TEST_F(Vu1ChunkPack, ADamagedPackIsRefusedWithoutACrash)
{
	const std::vector<VuOp> routine = CallLoopRoutine();
	for (int mode = 0; mode < 3; mode++)
	{
		SCOPED_TRACE(testing::Message() << "mode " << mode);
		OpenPack();
		{
			OneRun run(1, kHome, routine, kHome);
			EXPECT_GT(run.compiles, 0u);
		}
		EndSession();
		const std::string path = PackFile();
		auto bytes = FileSystem::ReadBinaryFile(path.c_str());
		ASSERT_TRUE(bytes.has_value());
		const size_t first = progcache_pack::kHeaderBytes + sizeof(progcache_pack::RecordHeader);
		ASSERT_GT(bytes->size(), first + 256);
		if (mode == 0)
			(*bytes)[first + 200] ^= 0x40; // in the first record's bytes
		else if (mode == 1)
			bytes->resize(first + 100); // the first record cut short
		else
			for (size_t at = first; at < bytes->size(); at += 97) // every record
				(*bytes)[at] ^= 0x04;
		ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), bytes->data(), bytes->size()));

		BeginSession();
		EXPECT_EQ(mVUProgCache::GetStats(1).tornTails, 1u);
		const StoreStats a = mVUChunkStore::GetStats();
		OneRun run(1, kHome, routine, kHome);
		const StoreStats b = mVUChunkStore::GetStats();
		EXPECT_GT(run.compiles, 0u);
		EXPECT_EQ(b.packLoaded - a.packLoaded, 0u);
		ExpectMatchesInterp(run.h, "damaged pack");
		EndSession();
		std::error_code ec;
		std::filesystem::remove_all(Path::Combine(root_, "vu_jit"), ec);
	}
}

// A record that passes its checksum and is still wrong: every count, offset
// and slot is checked before a body is made of it.
TEST_F(Vu1ChunkPack, ARecordThatDoesNotAddUpIsRefused)
{
	const std::vector<VuOp> routine = CallLoopRoutine();
	OpenPack();
	{
		OneRun run(1, kHome, routine, kHome);
	}
	EndSession();
	const std::vector<progcache_pack::Record> bodies = Bodies();
	ASSERT_FALSE(bodies.empty());

	// The header of a body record (microVU_ChunkPack-arm64.inl), by offset.
	constexpr size_t kMagic = 0, kFlags = 4, kLo = 104, kHi = 108, kSlotCount = 112, kCodeBytes = 116, kReadCount = 124,
					 kBlockCount = 128, kFixupCount = 152, kHeaderEnd = 156;
	const auto put32 = [](std::vector<u8>& v, size_t at, u32 value) { std::memcpy(v.data() + at, &value, 4); };
	const auto get32 = [](const std::vector<u8>& v, size_t at) {
		u32 value;
		std::memcpy(&value, v.data() + at, 4);
		return value;
	};

	for (const progcache_pack::Record& r : bodies)
	{
		const std::vector<u8>& good = r.bytes;
		ASSERT_GT(good.size(), kHeaderEnd);
		ASSERT_TRUE(mVUChunkStore::TestParseRecord(good.data(), good.size())) << "the record the store wrote does not pass";

		struct Edit
		{
			const char* what;
			size_t at;
			u32 value;
		};
		const Edit edits[] = {
			{"magic", kMagic, 0x12345678u},
			{"unknown flag", kFlags, get32(good, kFlags) | 0x80u},
			{"declined with code", kFlags, 3u},
			{"low bound above the root", kLo, 1u},
			{"high bound below the root", kHi, 0u},
			{"bounds outside micro memory", kHi, 0x4000u},
			{"one slot fewer", kSlotCount, get32(good, kSlotCount) - 1},
			{"one slot more", kSlotCount, get32(good, kSlotCount) + 1},
			{"code size not a multiple of 4", kCodeBytes, get32(good, kCodeBytes) + 2},
			{"more code than the record holds", kCodeBytes, get32(good, kCodeBytes) + 4},
			{"huge code size", kCodeBytes, 0x7FFFFFF0u},
			{"one read more", kReadCount, get32(good, kReadCount) + 1},
			{"huge read count", kReadCount, 0xFFFFFFFFu},
			{"no blocks", kBlockCount, 0u},
			{"one block more", kBlockCount, get32(good, kBlockCount) + 1},
			{"one fixup more", kFixupCount, get32(good, kFixupCount) + 1},
			{"huge fixup count", kFixupCount, 0x10000000u},
		};
		for (const Edit& e : edits)
		{
			std::vector<u8> bad = good;
			put32(bad, e.at, e.value);
			EXPECT_FALSE(mVUChunkStore::TestParseRecord(bad.data(), bad.size())) << e.what;
		}
		for (size_t cut : {size_t(1), size_t(4), size_t(100), good.size() - 8})
		{
			std::vector<u8> bad(good.begin(), good.end() - cut);
			EXPECT_FALSE(mVUChunkStore::TestParseRecord(bad.data(), bad.size())) << "cut by " << cut;
		}
		{
			std::vector<u8> bad = good;
			bad.resize(bad.size() + 4);
			EXPECT_FALSE(mVUChunkStore::TestParseRecord(bad.data(), bad.size())) << "four bytes more";
		}
		// Any single byte of the tables (everything before the code) changed:
		// refused or not, it must not read outside the record.
		const size_t tables = good.size() - get32(good, kCodeBytes);
		for (size_t at = 0; at < tables; at++)
		{
			std::vector<u8> bad = good;
			bad[at] ^= 0xFF;
			mVUChunkStore::TestParseRecord(bad.data(), bad.size());
			bad[at] = good[at] ^ 0x01;
			mVUChunkStore::TestParseRecord(bad.data(), bad.size());
		}
	}

	// A record whose fixup names an instruction that is not the form it says:
	// it passes its checksum and the table checks, and is refused when the
	// body is made. The routine compiles, and the record leaves the table.
	{
		const progcache_pack::Record& r = bodies[0];
		std::vector<u8> bad = r.bytes;
		const u32 fixups = get32(bad, kFixupCount), code = get32(bad, kCodeBytes);
		ASSERT_GT(fixups, 0u) << "a body with no host address in it";
		// (the last fixup record ends where the code starts; its form is the byte at +4)
		const size_t lastFixup = bad.size() - code - 24;
		bad[lastFixup + 4] = bad[lastFixup + 4] == 1 ? 2 : 1; // an ADRP as a B, anything else as an ADRP
		ASSERT_TRUE(mVUChunkStore::TestParseRecord(bad.data(), bad.size()));
		const std::string path = PackFile();
		std::error_code ec;
		std::filesystem::remove(path, ec);
		// A pack that holds only the bad record, under the routine's root hash.
		BeginSession();
		ASSERT_TRUE(mVUProgCache::StoreBody(1, r.hash.low64, bad.data(), bad.size(), false));
		EndSession();
		BeginSession();
		const StoreStats a = mVUChunkStore::GetStats();
		OneRun run(1, kHome, routine, kHome);
		const StoreStats b = mVUChunkStore::GetStats();
		EXPECT_EQ(b.packRejected - a.packRejected, 1u);
		EXPECT_EQ(b.packLoaded - a.packLoaded, 0u);
		EXPECT_GT(run.compiles, 0u);
		EXPECT_EQ(mVUProgCache::GetStats(1).bodiesDropped, 1u);
		ExpectMatchesInterp(run.h, "bad fixup");
	}
}

// The child half of the next test: runs only when the parent sets the
// environment variable. One session that compiles a routine, tiers two more
// up, and writes the pack under the directory the parent named.
TEST_F(Vu1ChunkPack, CrossProcessChildWritesAPack)
{
	const char* root = std::getenv(kChildEnv);
	if (!root)
		GTEST_SKIP() << "runs as the child of APackFromAnotherProcessRunsHere";
	cache_root_ = root;
	TierScope on(true, true);
	OpenPack();
	const StoreStats s0 = mVUChunkStore::GetStats();
	const vuaot::live::Stats t0 = vuaot::live::GetStats();
	{
		OneRun run(1, kHome, CallLoopRoutine(), kHome);
	}
	for (const std::vector<VuOp>& routine : {LoopRoutine(10), CallRoutine(9)})
	{
		OneRun run(1, kHome, routine, kHome);
		for (u32 i = 0; i < kTierRuns + 2; i++)
			run.Again();
	}
	const StoreStats s1 = mVUChunkStore::GetStats();
	const vuaot::live::Stats t1 = vuaot::live::GetStats();
	ASSERT_EQ(t1.tierUps - t0.tierUps, 2u);
	ASSERT_EQ(s1.packUnpackable - s0.packUnpackable, 0u);
	EndSession();

	u64 image_base = 0, data_base = 0;
	mVUPersist::TestGetRegionBases(image_base, data_base);
	std::FILE* f = std::fopen(Path::Combine(cache_root_, "bases").c_str(), "wb");
	ASSERT_NE(f, nullptr);
	std::fwrite(&image_base, sizeof(image_base), 1, f);
	std::fwrite(&data_base, sizeof(data_base), 1, f);
	std::fclose(f);
}

// A pack written by another process: this test binary run again, with its
// image, data arena, code cache and heap wherever that run's address-space
// layout put them. Every host address in the bodies has to be patched for
// this process. microVU's code and region code both run with no compile.
TEST_F(Vu1ChunkPack, APackFromAnotherProcessRunsHere)
{
#ifndef __linux__
	GTEST_SKIP() << "spawns this test binary through /proc/self/exe";
#else
	if (std::getenv(kChildEnv))
		GTEST_SKIP() << "already the child";

	const std::vector<VuOp> plain = CallLoopRoutine();
	const std::vector<std::vector<VuOp>> tiered = {LoopRoutine(10), CallRoutine(9)};
	const std::vector<u32> roots = Roots();
	// What a cold compile gives here, with no pack and the tier off.
	std::vector<Result> coldPlain;
	std::vector<std::vector<Result>> mvuTiered(tiered.size());
	{
		TierScope off(false, true);
		for (size_t i = 0; i < roots.size(); i++)
		{
			OneRun run(10 + static_cast<u32>(i), roots[i], plain, roots[i]);
			coldPlain.push_back(Take(run.h));
			for (size_t t = 0; t < tiered.size(); t++)
			{
				OneRun r2(10 + static_cast<u32>(i), roots[i], tiered[t], roots[i]);
				mvuTiered[t].push_back(Take(r2.h));
			}
		}
	}

	char exe[4096];
	const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	ASSERT_GT(n, 0);
	exe[n] = 0;
	std::vector<std::string> env_store;
	for (char** e = environ; *e; e++)
		env_store.emplace_back(*e);
	env_store.push_back(std::string(kChildEnv) + "=" + root_);
	std::vector<char*> envp;
	for (std::string& e : env_store)
		envp.push_back(e.data());
	envp.push_back(nullptr);
	std::string filter = "--gtest_filter=Vu1ChunkPack.CrossProcessChildWritesAPack";
	char* argv[] = {exe, filter.data(), nullptr};
	pid_t pid = 0;
	ASSERT_EQ(posix_spawn(&pid, exe, nullptr, nullptr, argv, envp.data()), 0);
	int status = 0;
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child failed, status " << status;

	u64 child_image = 0, child_data = 0, image_base = 0, data_base = 0;
	{
		std::FILE* f = std::fopen(Path::Combine(root_, "bases").c_str(), "rb");
		ASSERT_NE(f, nullptr) << "the child wrote nothing";
		ASSERT_EQ(std::fread(&child_image, sizeof(child_image), 1, f), 1u);
		ASSERT_EQ(std::fread(&child_data, sizeof(child_data), 1, f), 1u);
		std::fclose(f);
	}
	mVUPersist::TestGetRegionBases(image_base, data_base);
	std::printf("[chunk-pack-xproc] image base here %#llx, in the writing process %#llx; data arena here %#llx, there %#llx\n",
		(unsigned long long)image_base, (unsigned long long)child_image, (unsigned long long)data_base, (unsigned long long)child_data);
	if (image_base == child_image)
		GTEST_SKIP() << "both processes loaded the image at the same base (ASLR off?): the test proves nothing about relocation";
	ASSERT_FALSE(Bodies().empty()) << "the child's pack holds no body";

	TierScope on(true, true);
	BeginSession();
	EXPECT_EQ(mVUProgCache::GetStats(1).otherFormat, 0u) << "the child's pack was not accepted";
	for (size_t i = 0; i < roots.size(); i++)
	{
		SCOPED_TRACE(testing::Message() << "root 0x" << std::hex << roots[i]);
		{
			const StoreStats a = mVUChunkStore::GetStats();
			OneRun run(10 + static_cast<u32>(i), roots[i], plain, roots[i]);
			const StoreStats b = mVUChunkStore::GetStats();
			EXPECT_EQ(run.compiles, 0u) << "microVU compiled a body the pack holds";
			EXPECT_GT(b.packLoaded - a.packLoaded, 0u);
			EXPECT_EQ(b.packRejected - a.packRejected, 0u);
			ExpectMatchesInterp(run.h, "plain, from another process");
			const std::string d = Diff(coldPlain[i], Take(run.h), true);
			EXPECT_TRUE(d.empty()) << "against the cold compile: " << d;
		}
		for (size_t t = 0; t < tiered.size(); t++)
		{
			SCOPED_TRACE(testing::Message() << "tiered routine " << t);
			const StoreStats a = mVUChunkStore::GetStats();
			const vuaot::live::Stats ta = vuaot::live::GetStats();
			OneRun run(10 + static_cast<u32>(i), roots[i], tiered[t], roots[i]);
			run.Again();
			const StoreStats b = mVUChunkStore::GetStats();
			const vuaot::live::Stats tb = vuaot::live::GetStats();
			EXPECT_EQ(run.compiles, 0u) << "microVU compiled";
			EXPECT_EQ((tb.tierUps + tb.retiers) - (ta.tierUps + ta.retiers), 0u) << "a region was built again";
			EXPECT_GE(b.tieredHits - a.tieredHits, 1u) << "the copy was not placed on the tiered body";
			EXPECT_EQ(b.tieredReAimed - a.tieredReAimed, 0u) << "the copy ran microVU's code first";
			EXPECT_EQ(b.packRejected - a.packRejected, 0u);
			ExpectMatchesInterp(run.h, "tiered, from another process");
			const std::string d = Diff(mvuTiered[t][i], Take(run.h), false);
			EXPECT_TRUE(d.empty()) << "against microVU: " << d;
		}
	}
#endif
}

} // namespace recompiler_tests

#endif
