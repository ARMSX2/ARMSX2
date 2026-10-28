// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Persisted-JIT VU program cache — the production path end to end.
//
// The in-process serialize → hydrate path is covered via test hooks elsewhere.
// This file proves the PRODUCTION path, through the real cache and the real
// dispatcher seam:
//
//   compile (recording on) → mVUreset → SaveAllPrograms stores the image in
//   the open pack's table → next dispatch misses contentMap + per-PC deque →
//   mVUsearchProg calls mVUProgCache::TryLoadProgram → HydrateProgram from
//   RAM (episode closed/reopened around it) → program runs with ZERO block
//   compiles and a bit-identical post-state.
//
// And the disk side: the writer puts the table in the pack, a new session
// (ResetForTest + Init: everything but the process boundary) reads it back
// whole, and from then on the disk is not read again. A reset, as a savestate
// load does, brings programs back from RAM and writes nothing. Close writes
// what is left within its deadline.
//
// Fail-safe gates: a refused image (another build) and a program recorded
// with recording off degrade to a clean recompile.
//
// All tests drive VU0 — same thread-affinity reasoning as
// mvu_persist_roundtrip_tests.cpp.

#include "harness/ProgCachePackMirror.h"
#include "harness/RecompilerTestEnvironment.h"
#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/microVU_ProgCache-arm64.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp UpperOnly(u32 upper)
{
	return IBit(VuOp{VLitZero(), upper});
}

using progcache_pack::CountRecords;

XXH128_hash_t HashOf(u64 n)
{
	XXH128_hash_t h;
	h.low64 = n * 0x9E3779B97F4A7C15ull + 1;
	h.high64 = ~n;
	return h;
}

class MvuProgCacheDisk : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_TRUE(RecompilerTestEnvironment::IsReady());
		// Recording is under manual test control (RecompilerTestEnvironment
		// engaged SetTestManualRecording), so mVUreset's SyncRecordingFromConfig
		// won't fight these tests — drive it directly.
		mVUPersist::SetRecordingEnabled(true);

		char tmpl[] = "/tmp/progcache_disk_XXXXXX";
		char* p = ::mkdtemp(tmpl);
		ASSERT_NE(p, nullptr);
		root_ = p;
		prev_cache_ = EmuFolders::Cache;
		EmuFolders::Cache = root_;
	}

	void TearDown() override
	{
		// Free live programs while the cache is still up, then disable the cache for everything that
		// runs after us.
		RecompilerTestEnvironment::ResetVuBlockCache(0);
		mVUProgCache::ResetForTest(0);
		mVUProgCache::ResetForTest(1);
		mVUPersist::SetRecordingEnabled(false);
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
		EmuFolders::Cache = prev_cache_;
		std::error_code ec;
		std::filesystem::remove_all(root_, ec);
	}

	// Bring the cache up at the temp root. Deferred out of SetUp so tests can compile a genuine no-cache
	// baseline first.
	void InitDiskCache()
	{
		// The setting says the cache is wanted from here on: a reset closes a cache that nothing asks for
		// (mVUreset). TearDown puts it back.
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
		mVUProgCache::ResetForTest(0);
		mVUProgCache::TestReinitFromLiveSentinel(0);
		ASSERT_TRUE(mVUProgCache::GetStats(0).enabled) << "cache failed to init at " << root_;
	}

	// The next process: everything written, the table dropped, the pack read again.
	void NewSession()
	{
		mVUProgCache::ResetForTest(0);
		mVUProgCache::TestReinitFromLiveSentinel(0);
		mVUProgCache::TestWaitForPreload(0);
	}

	std::string jit() const { return Path::Combine(root_, "vu_jit"); }
	size_t Programs() const { return CountRecords(jit(), progcache_pack::kProgram); }
	size_t Sidecars() const { return CountRecords(jit(), progcache_pack::kSidecar); }

	std::string root_;
	std::string prev_cache_;
};

// The shared three-op straight-line program. Kept identical across tests so
// each test's expectations stay easy to eyeball.
const std::initializer_list<vu::VuOp> kProgram = {
	UpperOnly(VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
	UpperOnly(VMUL_U(mask::xyzw, vf::vf4, vf::vf3, vf::vf2)),
	UpperOnly(bits::E | VSUB_U(mask::xyzw, vf::vf5, vf::vf4, vf::vf1)),
};

void SeedAndLoad(VuTestHarness& h)
{
	h.SetVf(1, 1.5f, -2.25f, 3.0f, 0.0625f);
	h.SetVf(2, 4.0f, 0.5f, -1.0f, 8.0f);
	h.LoadProgram(kProgram);
}

// Programs that differ only in the destination of the last op: one content hash each.
std::vector<vu::VuOp> Variant(u32 k)
{
	return {
		UpperOnly(VADD_U(mask::xyzw, vf::vf3, vf::vf1, vf::vf2)),
		UpperOnly(VMUL_U(mask::xyzw, vf::vf4, vf::vf3, vf::vf2)),
		UpperOnly(bits::E | VSUB_U(mask::xyzw, vf::vf5 + k, vf::vf4, vf::vf1)),
	};
}

void SeedVariant(VuTestHarness& h, u32 k)
{
	h.SetVf(1, 1.5f, -2.25f, 3.0f, 0.0625f);
	h.SetVf(2, 4.0f, 0.5f, -1.0f, 8.0f);
	h.LoadProgram(Variant(k));
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
}

// Runs the program in micro memory again, as after a reset threw the compiled code away: how many blocks
// it compiled.
u64 RunAgain(VuTestHarness& h)
{
	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	const u64 before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();
	return mVUPersist::GetBlockCompileCount(0) - before;
}

} // namespace

// A reset (what a savestate load does) stores the program in RAM, writes nothing, and the next dispatch
// hydrates it from RAM: zero compiles, the same post-state.
TEST_F(MvuProgCacheDisk, AResetBringsTheProgramBackFromRamAndWritesNothing)
{
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();
	const VuSnapshot fresh_jit = h.JitSnapshot();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	auto cstats = mVUProgCache::GetStats(0);
	ASSERT_GE(cstats.payloadWrites, 1u) << "reset-side SaveAllPrograms stored no image";
	EXPECT_FALSE(FileSystem::FileExists(mVUProgCache::TestPackPath(0).c_str())) << "the reset wrote to disk";

	EXPECT_EQ(RunAgain(h), 0u) << "dispatch recompiled instead of hydrating";
	cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.payloadHits, 1u);
	EXPECT_EQ(cstats.payloadRejects, 0u);
	EXPECT_GE(mVUPersist::GetStats(0).programsHydrated, 1u);
	EXPECT_EQ(cstats.recordsWritten, 0u);

	const auto diffs = DiffVu(fresh_jit, h.JitSnapshot(), VuDiffMode::Strict);
	EXPECT_TRUE(diffs.empty()) << [&] {
		std::string s = "hydrated JIT diverged from fresh JIT:\n";
		for (const auto& d : diffs)
			s += "  " + d + "\n";
		return s;
	}();
	EXPECT_FLOAT_EQ(h.GetVfJit(3, 'x'), 5.5f);
}

// The writer puts the image in the pack; the next session reads the pack once, and every later load is
// RAM: with the file deleted, resets (savestate loads) keep bringing the program back.
TEST_F(MvuProgCacheDisk, ANewSessionReadsThePackOnceAndKeepsItInRam)
{
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();
	const VuSnapshot fresh_jit = h.JitSnapshot();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	mVUProgCache::FlushSidecars(0);
	ASSERT_EQ(Programs(), 1u);

	NewSession();
	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.packsLoaded, 1u);
	EXPECT_EQ(cstats.loadedRecords, 1u);
	ASSERT_EQ(::unlink(mVUProgCache::TestPackPath(0).c_str()), 0);

	for (int reset = 0; reset < 3; reset++)
	{
		EXPECT_EQ(RunAgain(h), 0u) << "reset " << reset << ": the program was compiled, not served from RAM";
		EXPECT_TRUE(DiffVu(fresh_jit, h.JitSnapshot(), VuDiffMode::Strict).empty()) << "reset " << reset;
		RecompilerTestEnvironment::ResetVuBlockCache(0);
	}
	cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.payloadHits, 3u);
	EXPECT_EQ(cstats.payloadWrites, 0u) << "an unchanged hydrated program was stored again";
	EXPECT_EQ(cstats.packsLoaded, 1u);
}

TEST_F(MvuProgCacheDisk, RecordingDisabledStoresNothing)
{
	// With recording off a program has no persist log: nothing is stored, nothing is written, and the
	// next dispatch compiles.
	mVUPersist::SetRecordingEnabled(false);

	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.savesAttempted, 1u);
	EXPECT_EQ(cstats.payloadWrites, 0u);
	mVUProgCache::FlushSidecars(0);
	EXPECT_EQ(Programs(), 0u);

	EXPECT_GT(RunAgain(h), 0u);
	cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.misses, 1u);
	EXPECT_EQ(cstats.payloadHits, 0u);
}

TEST_F(MvuProgCacheDisk, RecordingStateJoinsOptionsSentinel)
{
	// Safety property: recording changes emitted code forms
	// (fixed-shape MOVZ/MOVK runs for recorded addresses), so the recording state
	// MUST be part of program identity. A cache built with recording on
	// must never be served to a recording-off run (or vice versa) — the
	// sentinel divergence below is what puts them in different packs and
	// what keys contentHash lookups apart.
	XXH128_hash_t on{}, off{}, on_again{};
	ASSERT_TRUE(mVUProgCache::TestGetLiveSentinel(0, &on)); // SetUp: recording on

	mVUPersist::SetRecordingEnabled(false);
	ASSERT_TRUE(mVUProgCache::TestGetLiveSentinel(0, &off));

	mVUPersist::SetRecordingEnabled(true);
	ASSERT_TRUE(mVUProgCache::TestGetLiveSentinel(0, &on_again));

	EXPECT_TRUE(on.low64 != off.low64 || on.high64 != off.high64)
		<< "recording on/off produced the SAME options sentinel — the "
		   "recording bit is not mixed into program identity";
	EXPECT_EQ(on.low64, on_again.low64);
	EXPECT_EQ(on.high64, on_again.high64);
}

TEST_F(MvuProgCacheDisk, ConfigBoolGatesProductionInit)
{
	// The production seam: mVUreset must bring the cache up when (and
	// only when) EmuCore/CPU/Recompiler EnableVUProgramCache is set — even
	// though EmuFolders::Cache points at a perfectly writable directory.
	mVUProgCache::ResetForTest(0);

	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_FALSE(mVUProgCache::GetStats(0).initialized)
		<< "cache initialized with the config bool OFF";

	// As VMManager would after a settings toggle: flip the bool; the next
	// recompiler reset activates the cache.
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	const auto stats = mVUProgCache::GetStats(0);
	EXPECT_TRUE(stats.initialized)
		<< "reset with the config bool ON did not initialize the cache";
	EXPECT_TRUE(stats.enabled);
}

TEST_F(MvuProgCacheDisk, VuAotTurnsTheCacheOn)
{
	// Owner decision 2026-10-15: EnableVUAot is bound to the program cache. With it set and
	// EnableVUProgramCache clear, the next recompiler reset (what a settings change causes) brings the
	// cache up and starts recording, exactly as EnableVUProgramCache does; clearing it takes
	// recording away again (the cache stays up for the session, as for the cache's own setting).
	struct Restore
	{
		~Restore()
		{
			EmuConfig.Cpu.Recompiler.EnableVUAot = false;
			RecompilerTestEnvironment::ResetVuBlockCache(0);
			mVUPersist::SetTestManualRecording(true);
		}
	} restore;

	mVUProgCache::ResetForTest(0);
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
	ASSERT_FALSE(EmuConfig.Cpu.Recompiler.EnableVUAot);
	EXPECT_FALSE(EmuConfig.Cpu.Recompiler.UseVUProgramCache());
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_FALSE(mVUProgCache::GetStats(0).initialized) << "cache initialized with both settings OFF";

	EmuConfig.Cpu.Recompiler.EnableVUAot = true;
	EXPECT_TRUE(EmuConfig.Cpu.Recompiler.UseVUProgramCache());
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	const auto stats = mVUProgCache::GetStats(0);
	EXPECT_TRUE(stats.initialized) << "reset with EnableVUAot ON did not initialize the cache";
	EXPECT_TRUE(stats.enabled);

	// The recording sync, with the test harness's manual control of it out of the way.
	mVUPersist::SetTestManualRecording(false);
	mVUPersist::SetRecordingEnabled(false);
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_TRUE(mVUPersist::IsRecordingEnabled()) << "EnableVUAot on: programs are recorded for the cache";
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
	EmuConfig.Cpu.Recompiler.EnableVUAot = false;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_TRUE(mVUPersist::IsRecordingEnabled()) << "EnableVUProgramCache alone still records";
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_FALSE(mVUPersist::IsRecordingEnabled()) << "both off: recording is off";
}

TEST_F(MvuProgCacheDisk, AResetThatNoLongerWantsTheCacheClosesItAndSavesNothing)
{
	// The cache is up and a program is live. Both settings go off, and the reset that follows (a settings
	// change) closes the cache instead of checkpointing into it: nothing is stored or written, and the
	// threads are gone before anything deletes the files.
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();
	InitDiskCache();
	ASSERT_TRUE(mVUProgCache::GetStats(0).initialized);

	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
	ASSERT_FALSE(EmuConfig.Cpu.Recompiler.EnableVUAot);
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_FALSE(mVUProgCache::GetStats(0).initialized) << "the unwanted cache stayed up";
	EXPECT_EQ(Programs(), 0u) << "the reset saved into a cache that nothing asks for";

	// And it comes back, empty-handed but working, when something asks for it again.
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_TRUE(mVUProgCache::GetStats(0).initialized);
	EXPECT_TRUE(mVUProgCache::GetStats(0).enabled);
}

TEST_F(MvuProgCacheDisk, AProgramThatGrewIsStoredAgainAndOneThatDidNotIsNot)
{
	// The reset-side save serializes only programs whose block graph grew since they were stored
	// (mVUPersist::LogShape): an unchanged hydrated program costs nothing, a grown one is stored again.
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	ASSERT_EQ(mVUProgCache::GetStats(0).payloadWrites, 1u);

	EXPECT_EQ(RunAgain(h), 0u); // hydrates
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_EQ(mVUProgCache::GetStats(0).payloadWrites, 1u) << "unchanged hydrated program was stored again";

	// The same program entered at another PC compiles a new block into it.
	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	h.RunJitPreserveBlockCache(); // hydrates
	const u64 before = mVUPersist::GetBlockCompileCount(0);
	h.SetStartPc(8);
	h.RunJitPreserveBlockCache();
	h.SetStartPc(VuTestHarness::kProgramPc);
	ASSERT_GT(mVUPersist::GetBlockCompileCount(0), before) << "the second entry did not compile";
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_EQ(mVUProgCache::GetStats(0).payloadWrites, 2u) << "the grown program was not stored again";
}

TEST_F(MvuProgCacheDisk, AnImageOfAnotherBuildIsRefusedAndReplaced)
{
	// An image's fixups are offsets into the module of the build that recorded it. Another build refuses
	// it, compiles the program as new, and its next save replaces the image. The build after that loads
	// the replacement.
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();
	const VuSnapshot fresh_jit = h.JitSnapshot();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	mVUProgCache::FlushSidecars(0);
	ASSERT_EQ(mVUProgCache::GetStats(0).payloadWrites, 1u);
	NewSession();

	mVUPersist::TestOverrideBuildId(true, 0x1234, 0x5678);
	EXPECT_GT(RunAgain(h), 0u) << "the other build's image was used";
	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.payloadRejects, 1u);
	EXPECT_EQ(cstats.payloadHits, 0u);
	EXPECT_TRUE(DiffVu(fresh_jit, h.JitSnapshot(), VuDiffMode::Strict).empty());

	RecompilerTestEnvironment::ResetVuBlockCache(0); // saves
	EXPECT_EQ(mVUProgCache::GetStats(0).payloadWrites, 1u) << "the refused image was not replaced";
	NewSession();
	EXPECT_EQ(RunAgain(h), 0u) << "the build that saved the replacement could not load it";
	EXPECT_GE(mVUProgCache::GetStats(0).payloadHits, 1u);
	mVUPersist::TestOverrideBuildId(false, 0, 0);
}

// A reset after the title changed puts the live programs in the old title's pack and opens the new one; a
// pack switched back to before its writes reached the disk is taken back from RAM, whole.
TEST_F(MvuProgCacheDisk, ATitleChangeMovesToThatTitlesPack)
{
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();

	mVUProgCache::SetTitle("SLUS-00001", 1);
	InitDiskCache();
	const std::string first = mVUProgCache::TestPackPath(0);
	mVUProgCache::SetTitle("SLUS-00002", 2);
	RecompilerTestEnvironment::ResetVuBlockCache(0); // saves into the first pack, then moves
	const std::string second = mVUProgCache::TestPackPath(0);
	EXPECT_NE(first, second);
	mVUProgCache::TestWaitForPreload(0);
	EXPECT_GT(RunAgain(h), 0u) << "the second title's pack holds no programs";

	mVUProgCache::SetTitle("SLUS-00001", 1);
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_EQ(mVUProgCache::TestPackPath(0), first);
	mVUProgCache::TestWaitForPreload(0);
	EXPECT_EQ(RunAgain(h), 0u) << "the first title's program did not come back";

	mVUProgCache::FlushSidecars(0);
	EXPECT_EQ(CountRecords(std::string(Path::GetDirectory(first)), progcache_pack::kProgram), 1u);
	EXPECT_EQ(CountRecords(std::string(Path::GetDirectory(second)), progcache_pack::kProgram), 1u);
	mVUProgCache::SetTitle("", 0);
}

//------------------------------------------------------------------
// Sidecars and the writer.
//------------------------------------------------------------------

class MvuProgCacheWriter : public MvuProgCacheDisk
{
protected:
	static constexpr u32 kPrograms = 4;

	// The sidecars are read for the vu-aot tier only. Turning it on changes the options sentinel (the reset
	// rebuilds it), so it goes on before the cache is first written.
	void SetUp() override
	{
		MvuProgCacheDisk::SetUp();
		saved_aot_ = EmuConfig.Cpu.Recompiler.EnableVUAot;
		EmuConfig.Cpu.Recompiler.EnableVUAot = true;
		RecompilerTestEnvironment::ResetVuBlockCache(0);
	}

	void TearDown() override
	{
		EmuConfig.Cpu.Recompiler.EnableVUAot = saved_aot_;
		MvuProgCacheDisk::TearDown();
	}

	// Four programs, each run and saved on its own.
	void SaveVariants()
	{
		h_ = std::make_unique<VuTestHarness>(0);
		InitDiskCache();
		for (u32 k = 0; k < kPrograms; k++)
		{
			SeedVariant(*h_, k);
			h_->Run();
			h_->RunJitPreserveBlockCache();
			RecompilerTestEnvironment::ResetVuBlockCache(0); // stores variant k
		}
		ASSERT_EQ(mVUProgCache::GetStats(0).payloadWrites, kPrograms);
	}

	bool saved_aot_ = false;
	std::unique_ptr<VuTestHarness> h_;
};

TEST_F(MvuProgCacheWriter, AQueuedSidecarIsWhatTheNextReadGetsBeforeAnyWrite)
{
	// A reset queues each program's plan, and the program can come back before the writer runs: the read
	// builds the queued request itself, so it never sees an older copy.
	InitDiskCache();
	mVUProgCache::QueueSidecar(0, HashOf(1), [] { return std::vector<u8>(128, 0x11); });
	mVUProgCache::FlushSidecars(0);
	NewSession();

	mVUProgCache::QueueSidecar(0, HashOf(1), [] { return std::vector<u8>(128, 0x22); });
	std::vector<u8> out;
	ASSERT_TRUE(mVUProgCache::ReadSidecar(0, HashOf(1), out));
	EXPECT_EQ(out, std::vector<u8>(128, 0x22)) << "the read answered the older copy";

	// A build that returns nothing keeps what is stored.
	mVUProgCache::QueueSidecar(0, HashOf(1), [] { return std::vector<u8>(); });
	ASSERT_TRUE(mVUProgCache::ReadSidecar(0, HashOf(1), out));
	EXPECT_EQ(out, std::vector<u8>(128, 0x22));
	EXPECT_EQ(mVUProgCache::GetStats(0).sidecarSkips, 1u);

	mVUProgCache::FlushSidecars(0);
	NewSession();
	ASSERT_TRUE(mVUProgCache::ReadSidecar(0, HashOf(1), out));
	EXPECT_EQ(out, std::vector<u8>(128, 0x22));
}

TEST_F(MvuProgCacheWriter, WhatTheSessionStoredWinsOverTheFilesOlderCopy)
{
	// The read puts the file in the table a moment after it starts; a sidecar queued in between is newer
	// and stays.
	InitDiskCache();
	mVUProgCache::QueueSidecar(0, HashOf(2), [] { return std::vector<u8>(64, 0x01); });
	mVUProgCache::FlushSidecars(0);

	mVUProgCache::ResetForTest(0);
	mVUProgCache::TestHoldLoad(0, true);
	mVUProgCache::TestReinitFromLiveSentinel(0);
	for (int i = 0; i < 2000 && !mVUProgCache::TestLoadIsHeld(0); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	ASSERT_TRUE(mVUProgCache::TestLoadIsHeld(0));
	mVUProgCache::QueueSidecar(0, HashOf(2), [] { return std::vector<u8>(64, 0x02); });
	mVUProgCache::TestHoldLoad(0, false);
	mVUProgCache::TestWaitForPreload(0);

	std::vector<u8> out;
	ASSERT_TRUE(mVUProgCache::ReadSidecar(0, HashOf(2), out));
	EXPECT_EQ(out, std::vector<u8>(64, 0x02)) << "the file's older copy replaced the session's";
}

TEST_F(MvuProgCacheWriter, CloseWritesWithinItsDeadlineAndTheNextSessionReadsWhatGotThere)
{
	// A slow card: every record takes 150 ms. Close gives the writer 400 ms, writes the records that fit,
	// leaves the rest, and returns. The next session reads an intact log.
	SaveVariants();
	for (u64 n = 1; n <= 6; n++)
		mVUProgCache::QueueSidecar(0, HashOf(n), [n] { return std::vector<u8>(256, static_cast<u8>(n)); });
	mVUProgCache::TestSetWriteDelayMs(0, 150);
	mVUProgCache::TestSetCloseDeadlineMs(0, 400);
	const auto t0 = std::chrono::steady_clock::now();
	mVUProgCache::TestClose(0);
	const auto took = std::chrono::steady_clock::now() - t0;
	EXPECT_LT(took, std::chrono::milliseconds(1000)) << "Close waited for every record";
	const auto cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.closeDeadlineHit, 1u);
	EXPECT_GT(cstats.recordsDropped, 0u);
	EXPECT_GT(cstats.recordsWritten, 0u);
	EXPECT_EQ(cstats.recordsWritten + cstats.recordsDropped, kPrograms + 6u);

	mVUProgCache::TestSetWriteDelayMs(0, 0);
	NewSession();
	const auto next = mVUProgCache::GetStats(0);
	EXPECT_EQ(next.tornTails, 0u);
	EXPECT_EQ(next.loadedRecords, cstats.recordsWritten);
}

TEST_F(MvuProgCacheWriter, NothingReachesTheDiskBetweenFlushes)
{
	// Saves and sidecars stay in RAM until the writer's pass (every 30 s in production): a run of resets
	// and queued plans touches no file.
	SaveVariants();
	for (u64 n = 1; n <= 4; n++)
		mVUProgCache::QueueSidecar(0, HashOf(n), [n] { return std::vector<u8>(64, static_cast<u8>(n)); });
	EXPECT_FALSE(FileSystem::FileExists(mVUProgCache::TestPackPath(0).c_str()));
	EXPECT_EQ(mVUProgCache::GetStats(0).recordsWritten, 0u);
	mVUProgCache::FlushSidecars(0);
	EXPECT_EQ(Programs(), kPrograms);
	EXPECT_EQ(Sidecars(), 4u);
	// A flush with nothing new writes nothing.
	const u64 written = mVUProgCache::GetStats(0).recordsWritten;
	mVUProgCache::FlushSidecars(0);
	EXPECT_EQ(mVUProgCache::GetStats(0).recordsWritten, written);
}

//------------------------------------------------------------------
// Clearing the vu-aot cache when the setting goes from on to off.
//------------------------------------------------------------------

class MvuAotCacheClear : public MvuProgCacheWriter
{
protected:
	void TearDown() override
	{
		mVUProgCache::ResetForTest(1);
		MvuProgCacheWriter::TearDown();
	}

	// VU0: four programs and four sidecars. VU1: a pack of its own with two sidecars. The tripwire's memory.
	void Populate()
	{
		SaveVariants();
		for (u64 n = 1; n <= kPrograms; n++)
			mVUProgCache::QueueSidecar(0, HashOf(n), [n]() { return std::vector<u8>(512, static_cast<u8>(n)); });
		mVUProgCache::FlushSidecars(0);

		mVUProgCache::ResetForTest(1);
		mVUProgCache::TestReinitFromLiveSentinel(1);
		ASSERT_TRUE(mVUProgCache::GetStats(1).enabled);
		for (u64 n : {11, 12})
			mVUProgCache::QueueSidecar(1, HashOf(n), [n]() { return std::vector<u8>(256, static_cast<u8>(n)); });
		mVUProgCache::FlushSidecars(1);

		tripped_ = Path::Combine(jit(), "vuaot_tripped.txt");
		ASSERT_TRUE(FileSystem::WriteStringToFile(tripped_.c_str(), "SCUS-97481 2F123FD8\n"));

		NewSession(); // the next session: the sidecars sit in RAM
		ASSERT_TRUE(mVUProgCache::TestHasEntry(0, HashOf(1), true));
		ASSERT_EQ(Sidecars(), kPrograms + 2u);
		ASSERT_EQ(Programs(), kPrograms);
	}

	std::string tripped_;
};

TEST_F(MvuAotCacheClear, OnToOffDeletesTheSidecarsAndKeepsWhatAnotherSettingStillWants)
{
	Populate();

	// EnableVUProgramCache stays on: the programs stay, in RAM and on disk.
	const auto r = mVUProgCache::OnVuAotSettingChanged(true, false, true);
	EXPECT_EQ(r.files, kPrograms + 2u) << "sidecar records dropped";
	EXPECT_GT(r.bytes, (kPrograms + 2u) * 256u);
	EXPECT_EQ(Sidecars(), 0u) << "VU0 and VU1";
	EXPECT_EQ(Programs(), kPrograms);
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str())) << "the tripwire's memory stays";
	EXPECT_FALSE(mVUPersist::IsProcessDisabled());

	// What the table held is gone too, and the cache still works: a new sidecar is written.
	EXPECT_FALSE(mVUProgCache::TestHasEntry(0, HashOf(1), true));
	EXPECT_TRUE(mVUProgCache::GetStats(0).initialized);
	mVUProgCache::QueueSidecar(0, HashOf(1), []() { return std::vector<u8>(64, 1); });
	mVUProgCache::FlushSidecars(0);
	EXPECT_EQ(Sidecars(), 1u);
	NewSession();
	EXPECT_EQ(mVUProgCache::GetStats(0).loadedRecords, kPrograms + 1u);
	EXPECT_EQ(mVUProgCache::GetStats(0).tornTails, 0u);
}

TEST_F(MvuAotCacheClear, OnToOffWithNothingElseUsingTheCacheDeletesTheWholeCache)
{
	Populate();
	const auto r = mVUProgCache::OnVuAotSettingChanged(true, false, false);
	EXPECT_EQ(r.files, 2u) << "both packs";
	EXPECT_FALSE(FileSystem::DirectoryExists(Path::Combine(jit(), "packs").c_str()));
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str())) << "the tripwire's memory stays";
	EXPECT_FALSE(mVUProgCache::GetStats(0).initialized) << "the cache in memory is closed";

	// Switched on again, the cache starts fresh.
	mVUProgCache::TestReinitFromLiveSentinel(0);
	mVUProgCache::TestWaitForPreload(0);
	EXPECT_TRUE(mVUProgCache::GetStats(0).enabled);
	EXPECT_EQ(mVUProgCache::GetStats(0).entries, 0u);
}

TEST_F(MvuAotCacheClear, EveryOtherChangeClearsNothing)
{
	Populate();
	for (const bool programCache : {false, true})
	{
		const auto before_off_on = mVUProgCache::OnVuAotSettingChanged(false, true, programCache);
		const auto on_on = mVUProgCache::OnVuAotSettingChanged(true, true, programCache);
		const auto off_off = mVUProgCache::OnVuAotSettingChanged(false, false, programCache);
		EXPECT_EQ(before_off_on.files + on_on.files + off_off.files, 0u);
	}
	EXPECT_EQ(Sidecars(), kPrograms + 2u);
	EXPECT_EQ(Programs(), kPrograms);
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str()));
	EXPECT_TRUE(mVUProgCache::TestHasEntry(0, HashOf(1), true)) << "the table is untouched";
}

// Settings applied with no VM running (VMManager::CheckForConfigChanges in the Shutdown state) compare the
// global setting with the global value last seen with no VM, which VMManager records at startup and as a VM
// ends. The config a game ends with, its own settings over the global ones, never counts: a boot that
// fails applies settings in the Shutdown state with exactly that config as the old one.
TEST_F(MvuAotCacheClear, LeavingAGameWithItsOwnSettingOnAndTheGlobalOneOffClearsNothing)
{
	Populate();
	vuAotNoteGlobalSetting(false); // startup: the global setting is off (the game's own settings turn it on)
	// ...the game runs and ends; a failed boot, or any later apply, sees the global setting off again
	vuAotNoVmSettingsApplied(false, false);
	vuAotNoVmSettingsApplied(false, false);
	EXPECT_EQ(Sidecars(), kPrograms + 2u);
	EXPECT_EQ(Programs(), kPrograms);
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str()));
}

TEST_F(MvuAotCacheClear, TheGlobalSettingTurnedOffWithNoVmClearsOnceAndOnlyThen)
{
	Populate();
	vuAotNoteGlobalSetting(true);
	vuAotNoVmSettingsApplied(true, true); // an unrelated setting applied
	EXPECT_EQ(Sidecars(), kPrograms + 2u);

	vuAotNoVmSettingsApplied(false, true); // on to off, the program cache setting on: sidecars only
	EXPECT_EQ(Sidecars(), 0u);
	EXPECT_EQ(Programs(), kPrograms);

	// Off to off clears nothing (new records stay), and so does off to on.
	mVUProgCache::QueueSidecar(0, HashOf(1), []() { return std::vector<u8>(64, 1); });
	mVUProgCache::FlushSidecars(0);
	vuAotNoVmSettingsApplied(false, true);
	vuAotNoVmSettingsApplied(true, true);
	EXPECT_EQ(Sidecars(), 1u);
	vuAotNoteGlobalSetting(false);
}

TEST_F(MvuAotCacheClear, TheGlobalValueNotedAsAGameEndsIsNotAChangeMadeWithNoVm)
{
	Populate();
	vuAotNoteGlobalSetting(true); // startup: on
	// the global setting is turned off while a game runs (the game's own settings keep it on), and the VM ends
	vuAotNoteGlobalSetting(false);
	vuAotNoVmSettingsApplied(false, false);
	EXPECT_EQ(Sidecars(), kPrograms + 2u);
}

TEST_F(MvuAotCacheClear, ANonexistentCacheDirectoryIsFine)
{
	// (the fixture's reset brought VU0's cache up under the temporary root: take it away again)
	mVUProgCache::ResetForTest(0);
	std::error_code ec;
	std::filesystem::remove_all(jit(), ec);
	EXPECT_FALSE(FileSystem::DirectoryExists(jit().c_str()));
	EXPECT_EQ(mVUProgCache::OnVuAotSettingChanged(true, false, false).files, 0u);
	EXPECT_EQ(mVUProgCache::OnVuAotSettingChanged(true, false, true).files, 0u);
	const std::string saved = EmuFolders::Cache;
	EmuFolders::Cache.clear();
	EXPECT_EQ(mVUProgCache::ClearVuAot(true).files, 0u) << "no cache folder at all";
	EmuFolders::Cache = saved;
}

TEST_F(MvuAotCacheClear, ASidecarStillQueuedIsNotWrittenAfterTheClear)
{
	InitDiskCache();
	for (u64 n = 1; n <= 12; n++)
		mVUProgCache::QueueSidecar(0, HashOf(n), [n]() { return std::vector<u8>(64, static_cast<u8>(n)); });
	mVUProgCache::ClearVuAot(false);
	EXPECT_EQ(mVUProgCache::GetStats(0).sidecarWrites, 0u) << "the queue was built";
	mVUProgCache::FlushSidecars(0);
	EXPECT_EQ(Sidecars(), 0u) << "the writer put a record back after the clear";
	std::vector<u8> out;
	EXPECT_FALSE(mVUProgCache::ReadSidecar(0, HashOf(1), out));
}

} // namespace recompiler_tests
