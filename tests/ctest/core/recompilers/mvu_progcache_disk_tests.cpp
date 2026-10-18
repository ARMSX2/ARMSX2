// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Persisted-JIT VU program cache — disk round-trip gate.
//
// The in-process serialize → hydrate path is covered via test hooks elsewhere.
// This file proves the PRODUCTION path end to end, through the real disk
// cache and the real dispatcher seam:
//
//   compile (recording on) → mVUreset → SaveAllPrograms writes INDEX +
//   .vuprog payload → next dispatch misses contentMap + per-PC deque →
//   mVUsearchProg calls mVUProgCache::TryLoadProgram → payload read +
//   HydrateProgram (episode closed/reopened around it) → program runs
//   with ZERO block compiles and a bit-identical post-state.
//
// Only process death separates this from the cross-process flow — the
// in-memory INDEX is rebuilt from disk by Init, and the payload bytes
// travel exclusively through the filesystem (the vurunner --cache-dir
// gate covers the true two-process case on corpus captures).
//
// Fail-safe gates: a corrupt payload (checksum), a missing payload file,
// and a telemetry-only INDEX entry (recording off when saved) must all
// degrade to a clean recompile.
//
// All tests drive VU0 — same thread-affinity reasoning as
// mvu_persist_roundtrip_tests.cpp.

#include "harness/VuTestHarness.h"
#include "harness/RecompilerTestEnvironment.h"

#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "arm64/microVU_Persist-arm64.h"
#include "arm64/microVU_ProgCache-arm64.h"

#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
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

void RecursiveRm(const std::string& dir)
{
	if (!FileSystem::DirectoryExists(dir.c_str()))
	{
		::unlink(dir.c_str());
		return;
	}
	FileSystem::FindResultsArray ents;
	FileSystem::FindFiles(dir.c_str(), "*",
		FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES,
		&ents);
	for (const auto& e : ents)
	{
		if (e.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
			RecursiveRm(e.FileName);
		else
			::unlink(e.FileName.c_str());
	}
	::rmdir(dir.c_str());
}

// Collect every *.vuprog under `dir` (payloads live one shard level down).
void FindPayloads(const std::string& dir, std::vector<std::string>& out)
{
	if (!FileSystem::DirectoryExists(dir.c_str()))
		return;
	FileSystem::FindResultsArray ents;
	FileSystem::FindFiles(dir.c_str(), "*",
		FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES,
		&ents);
	for (const auto& e : ents)
	{
		if (e.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
			FindPayloads(e.FileName, out);
		else if (e.FileName.size() > 7 &&
			e.FileName.compare(e.FileName.size() - 7, 7, ".vuprog") == 0)
			out.push_back(e.FileName);
	}
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
		// Free live programs while the cache is still up (the reset-side
		// SaveAllPrograms writing into the doomed temp dir is harmless),
		// then disable the disk cache for everything that runs after us.
		RecompilerTestEnvironment::ResetVuBlockCache(0);
		mVUProgCache::ResetForTest(0);
		mVUPersist::SetRecordingEnabled(false);
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
		EmuFolders::Cache = prev_cache_;
		RecursiveRm(root_);
	}

	// Bring the on-disk cache up at the temp root. Deferred out of SetUp so
	// tests can compile a genuine no-cache baseline first.
	void InitDiskCache()
	{
		// The setting says the cache is wanted from here on: a reset closes a cache that nothing asks for
		// (mVUreset). TearDown puts it back.
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
		mVUProgCache::ResetForTest(0);
		mVUProgCache::TestReinitFromLiveSentinel(0);
		ASSERT_TRUE(mVUProgCache::GetStats(0).enabled)
			<< "disk cache failed to init at " << root_;
	}

	std::string vu0_root() const
	{
		return Path::Combine(Path::Combine(root_, "vu_jit"), "vu0");
	}

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

} // namespace

TEST_F(MvuProgCacheDisk, SaveThenHydrateAcrossReset)
{
	// 1. Genuine fresh compile, no disk cache in the loop yet.
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();
	const VuSnapshot fresh_jit = h.JitSnapshot();

	// 2. Cache up; reset writes INDEX + payload for the live program.
	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);

	auto cstats = mVUProgCache::GetStats(0);
	ASSERT_GE(cstats.payloadWrites, 1u) << "reset-side SaveAllPrograms wrote no payload";
	std::vector<std::string> payloads;
	FindPayloads(vu0_root(), payloads);
	ASSERT_EQ(payloads.size(), 1u);

	// 3. Next dispatch must hydrate from disk, not compile.
	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUPersist::GetBlockCompileCount(0), compiles_before)
		<< "dispatch recompiled instead of hydrating from disk";

	cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.payloadHits, 1u);
	EXPECT_EQ(cstats.payloadRejects, 0u);
	EXPECT_GE(mVUPersist::GetStats(0).programsHydrated, 1u);

	const auto diffs = DiffVu(fresh_jit, h.JitSnapshot(), VuDiffMode::Strict);
	EXPECT_TRUE(diffs.empty()) << [&] {
		std::string s = "disk-hydrated JIT diverged from fresh JIT:\n";
		for (const auto& d : diffs)
			s += "  " + d + "\n";
		return s;
	}();
	EXPECT_FLOAT_EQ(h.GetVfJit(3, 'x'), 5.5f);
}

TEST_F(MvuProgCacheDisk, CorruptPayloadFallsBackToRecompile)
{
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();
	const VuSnapshot fresh_jit = h.JitSnapshot();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);

	std::vector<std::string> payloads;
	FindPayloads(vu0_root(), payloads);
	ASSERT_EQ(payloads.size(), 1u);

	// Flip one byte past the image header — valid structure, wrong code.
	// Only the payload checksum can catch this.
	auto bytes = FileSystem::ReadBinaryFile(payloads[0].c_str());
	ASSERT_TRUE(bytes.has_value());
	ASSERT_GT(bytes->size(), 96u);
	(*bytes)[bytes->size() - 4] ^= 0xFF;
	ASSERT_TRUE(FileSystem::WriteBinaryFile(payloads[0].c_str(), bytes->data(), bytes->size()));

	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();

	EXPECT_GT(mVUPersist::GetBlockCompileCount(0), compiles_before)
		<< "corrupt payload should force a recompile";
	const auto cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.payloadRejects, 1u);
	EXPECT_EQ(cstats.payloadHits, 0u);

	// Degrade, never corrupt: the recompiled run is still correct.
	const auto diffs = DiffVu(fresh_jit, h.JitSnapshot(), VuDiffMode::Strict);
	EXPECT_TRUE(diffs.empty());
}

TEST_F(MvuProgCacheDisk, MissingPayloadFallsBackToRecompile)
{
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);

	std::vector<std::string> payloads;
	FindPayloads(vu0_root(), payloads);
	ASSERT_EQ(payloads.size(), 1u);
	ASSERT_EQ(::unlink(payloads[0].c_str()), 0);

	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();

	EXPECT_GT(mVUPersist::GetBlockCompileCount(0), compiles_before);
	const auto cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.payloadMissing, 1u);
	EXPECT_EQ(cstats.payloadHits, 0u);
}

TEST_F(MvuProgCacheDisk, RecordingDisabledSavesIndexOnly)
{
	// With recording off the INDEX keeps its telemetry role and no payload
	// appears on disk.
	mVUPersist::SetRecordingEnabled(false);

	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);

	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.entries, 1u);
	EXPECT_EQ(cstats.payloadWrites, 0u);
	std::vector<std::string> payloads;
	FindPayloads(vu0_root(), payloads);
	EXPECT_TRUE(payloads.empty());

	// Dispatch hits the INDEX, finds no payload, recompiles.
	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();
	EXPECT_GT(mVUPersist::GetBlockCompileCount(0), compiles_before);
	cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.payloadMissing, 1u);
}

TEST_F(MvuProgCacheDisk, PreloadServesHydrationWithoutDisk)
{
	// Init's background preload reads payload-bearing INDEX entries into RAM
	// so first-dispatch hydration never does file I/O on the EE/MTVU
	// threads. Proven by force: preload, then DELETE the .vuprog from disk —
	// hydration must still succeed, served from the preload map.
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();
	const VuSnapshot fresh_jit = h.JitSnapshot();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	ASSERT_GE(mVUProgCache::GetStats(0).payloadWrites, 1u);

	// Re-init from disk: the INDEX now carries a payload-bearing entry, so
	// this Init spawns the preload.
	mVUProgCache::ResetForTest(0);
	mVUProgCache::TestReinitFromLiveSentinel(0);
	mVUProgCache::TestWaitForPreload(0);
	auto cstats = mVUProgCache::GetStats(0);
	ASSERT_GE(cstats.preloadedPayloads, 1u)
		<< "Init did not preload the payload-bearing INDEX entry";
	ASSERT_GT(cstats.preloadedBytes, 0u);

	std::vector<std::string> payloads;
	FindPayloads(vu0_root(), payloads);
	ASSERT_EQ(payloads.size(), 1u);
	ASSERT_EQ(::unlink(payloads[0].c_str()), 0);

	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUPersist::GetBlockCompileCount(0), compiles_before)
		<< "dispatch recompiled — preloaded payload was not served";

	cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.preloadHits, 1u);
	EXPECT_GE(cstats.payloadHits, 1u);
	EXPECT_EQ(cstats.payloadMissing, 0u);
	EXPECT_EQ(cstats.preloadedBytes, 0u) << "consumed buffer was not released";

	const auto diffs = DiffVu(fresh_jit, h.JitSnapshot(), VuDiffMode::Strict);
	EXPECT_TRUE(diffs.empty());
}

TEST_F(MvuProgCacheDisk, RecordingStateJoinsOptionsSentinel)
{
	// Safety property: recording changes emitted code forms
	// (fixed-shape MOVZ/MOVK runs for recorded addresses), so the recording state
	// MUST be part of program identity. A cache built with recording on
	// must never be served to a recording-off run (or vice versa) — the
	// sentinel divergence below is what makes the VERSION handshake evict
	// it and what keys contentHash lookups apart.
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
	// The production seam: mVUreset must bring the disk cache up when (and
	// only when) EmuCore/CPU/Recompiler EnableVUProgramCache is set — even
	// though EmuFolders::Cache points at a perfectly writable directory.
	mVUProgCache::ResetForTest(0);

	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = false;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_FALSE(mVUProgCache::GetStats(0).initialized)
		<< "disk cache initialized with the config bool OFF";

	// As VMManager would after a settings toggle: flip the bool; the next
	// recompiler reset activates the cache.
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	const auto stats = mVUProgCache::GetStats(0);
	EXPECT_TRUE(stats.initialized)
		<< "reset with the config bool ON did not initialize the disk cache";
	EXPECT_TRUE(stats.enabled);
}

TEST_F(MvuProgCacheDisk, VuAotTurnsTheCacheOn)
{
	// Owner decision 2026-10-15: EnableVUAot is bound to the program cache. With it set and
	// EnableVUProgramCache clear, the next recompiler reset (what a settings change causes) brings the
	// disk cache up and starts recording, exactly as EnableVUProgramCache does; clearing it takes
	// recording away again (the disk state stays up for the session, as for the cache's own setting).
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
	EXPECT_FALSE(mVUProgCache::GetStats(0).initialized) << "disk cache initialized with both settings OFF";

	EmuConfig.Cpu.Recompiler.EnableVUAot = true;
	EXPECT_TRUE(EmuConfig.Cpu.Recompiler.UseVUProgramCache());
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	const auto stats = mVUProgCache::GetStats(0);
	EXPECT_TRUE(stats.initialized) << "reset with EnableVUAot ON did not initialize the disk cache";
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
	// change) closes the cache instead of checkpointing into it: nothing is written, and the writer and
	// preload threads are gone before anything deletes the files.
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
	std::vector<std::string> payloads;
	FindPayloads(vu0_root(), payloads);
	EXPECT_TRUE(payloads.empty()) << "the reset saved into a cache that nothing asks for";

	// And it comes back, empty-handed but working, when something asks for it again.
	EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_TRUE(mVUProgCache::GetStats(0).initialized);
	EXPECT_TRUE(mVUProgCache::GetStats(0).enabled);
}

TEST_F(MvuProgCacheDisk, HydratedProgramResavesWithoutGrowth)
{
	// A hydrated program that runs and resets again must NOT rewrite its
	// payload (the rebuilt persist log serializes to the same bytes) —
	// pins the size-based growth signal that keeps reset-side saves cheap.
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	auto cstats = mVUProgCache::GetStats(0);
	ASSERT_EQ(cstats.payloadWrites, 1u);

	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	h.RunJitPreserveBlockCache(); // hydrates
	ASSERT_GE(mVUProgCache::GetStats(0).payloadHits, 1u);

	RecompilerTestEnvironment::ResetVuBlockCache(0); // saves again
	cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.payloadWrites, 1u)
		<< "unchanged hydrated program rewrote its payload on re-save";
}

TEST_F(MvuProgCacheDisk, PayloadOfAnotherBuildIsRefusedAndReplaced)
{
	// A payload's image fixups are offsets into the module of the build that
	// recorded it. Another build refuses it, compiles the program as new, and
	// its next save replaces the file -- also though the new image is the size
	// the INDEX records for the old one (SaveProgram skips a payload of the
	// recorded size). The build after that loads the replacement.
	VuTestHarness h(0);
	SeedAndLoad(h);
	h.Run();
	h.RunJitPreserveBlockCache();
	const VuSnapshot fresh_jit = h.JitSnapshot();

	InitDiskCache();
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	ASSERT_EQ(mVUProgCache::GetStats(0).payloadWrites, 1u);
	std::vector<std::string> payloads;
	FindPayloads(vu0_root(), payloads);
	ASSERT_EQ(payloads.size(), 1u);
	const auto recorded = FileSystem::ReadBinaryFile(payloads[0].c_str());
	ASSERT_TRUE(recorded.has_value());

	mVUPersist::TestOverrideBuildId(true, 0x1234, 0x5678);
	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();
	EXPECT_GT(mVUPersist::GetBlockCompileCount(0), compiles_before) << "the other build's payload was used";
	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.payloadRejects, 1u);
	EXPECT_EQ(cstats.payloadHits, 0u);
	EXPECT_TRUE(DiffVu(fresh_jit, h.JitSnapshot(), VuDiffMode::Strict).empty());

	RecompilerTestEnvironment::ResetVuBlockCache(0); // saves
	cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.payloadWrites, 2u) << "the refused payload was not replaced";
	const auto replaced = FileSystem::ReadBinaryFile(payloads[0].c_str());
	ASSERT_TRUE(replaced.has_value());
	EXPECT_EQ(replaced->size(), recorded->size()) << "the program's image should come out the same size";
	EXPECT_NE(*replaced, *recorded);

	h.LoadProgram(kProgram);
	CpuMicroVU0.Clear(0, VU0_PROGSIZE);
	h.RunJitPreserveBlockCache();
	cstats = mVUProgCache::GetStats(0);
	EXPECT_GE(cstats.payloadHits, 1u) << "the build that saved the replacement could not load it";
	mVUPersist::TestOverrideBuildId(false, 0, 0);
}

//------------------------------------------------------------------
// The preload budget: the most recently used programs come first, the rest
// load when their program appears, and programs unused for long are deleted.
//------------------------------------------------------------------

namespace
{
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

XXH128_hash_t HashOfPayload(const std::string& path)
{
	const std::string name(Path::GetFileName(path));
	XXH128_hash_t h;
	h.high64 = std::strtoull(name.substr(0, 16).c_str(), nullptr, 16);
	h.low64 = std::strtoull(name.substr(16, 16).c_str(), nullptr, 16);
	return h;
}

// The file's last-use time, `days` ago (the preload's ranking and age-out read it).
void SetAge(const std::string& path, double days)
{
	std::error_code ec;
	std::filesystem::last_write_time(path,
		std::filesystem::file_time_type::clock::now() - std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
			std::chrono::duration<double, std::ratio<86400>>(days)), ec);
	ASSERT_FALSE(ec) << path;
}

double AgeDays(const std::string& path)
{
	std::error_code ec;
	const auto t = std::filesystem::last_write_time(path, ec);
	if (ec)
		return -1.0;
	return std::chrono::duration<double, std::ratio<86400>>(std::filesystem::file_time_type::clock::now() - t).count();
}
} // namespace

// K programs, each saved on its own: variant k's payload path.
class MvuProgCachePreload : public MvuProgCacheDisk
{
protected:
	static constexpr u32 kPrograms = 4;

	// The sidecars are read for the vu-aot tier only. Turning it on changes the options sentinel (the reset
	// rebuilds it), so it goes on before the cache directory is first written.
	struct AotOn
	{
		AotOn()
		{
			saved = EmuConfig.Cpu.Recompiler.EnableVUAot;
			EmuConfig.Cpu.Recompiler.EnableVUAot = true;
			RecompilerTestEnvironment::ResetVuBlockCache(0);
		}
		~AotOn() { EmuConfig.Cpu.Recompiler.EnableVUAot = saved; }
		bool saved;
	};

	void SaveVariants()
	{
		h_ = std::make_unique<VuTestHarness>(0);
		VuTestHarness& h = *h_;
		InitDiskCache();
		for (u32 k = 0; k < kPrograms; k++)
		{
			SeedVariant(h, k);
			h.Run();
			h.RunJitPreserveBlockCache();
			RecompilerTestEnvironment::ResetVuBlockCache(0); // saves variant k
			std::vector<std::string> now;
			FindPayloads(vu0_root(), now);
			ASSERT_EQ(now.size(), k + 1u);
			for (const std::string& p : now)
				if (std::find(paths_.begin(), paths_.end(), p) == paths_.end())
					paths_.push_back(p);
			ASSERT_EQ(paths_.size(), k + 1u);
		}
		ASSERT_EQ(mVUProgCache::GetStats(0).payloadWrites, kPrograms);
	}

	// A new session over the same directory, the preload budget `bytes` (0: the default).
	void Reopen(size_t bytes)
	{
		mVUProgCache::ResetForTest(0);
		mVUProgCache::TestSetPreloadBudget(0, bytes);
		mVUProgCache::TestReinitFromLiveSentinel(0);
		mVUProgCache::TestWaitForPreload(0);
	}

	size_t SizeOf(const std::string& path) { return static_cast<size_t>(FileSystem::GetPathFileSize(path.c_str())); }

	std::vector<std::string> paths_; // variant k's payload
	std::unique_ptr<VuTestHarness> h_;
};

TEST_F(MvuProgCachePreload, ThePreloadTakesTheMostRecentlyUsedFirst)
{
	SaveVariants();
	// Last used: variant 2 (a day ago), 0 (two), 3 (three), 1 (four). Not the order they were saved in.
	const u32 rank[kPrograms] = {2, 0, 3, 1}; // newest first
	SetAge(paths_[2], 1);
	SetAge(paths_[0], 2);
	SetAge(paths_[3], 3);
	SetAge(paths_[1], 4);
	const size_t two = SizeOf(paths_[rank[0]]) + SizeOf(paths_[rank[1]]);

	Reopen(two);
	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.preloadedPayloads, 2u);
	EXPECT_EQ(cstats.preloadBudgetHit, 1u);
	EXPECT_EQ(cstats.preloadReadBytes, two);
	for (u32 i = 0; i < kPrograms; i++)
		EXPECT_EQ(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[rank[i]]), false), i < 2) << "rank " << i;

	// A budget of one byte less than the two takes one; of everything, all four.
	Reopen(two - 1);
	EXPECT_EQ(mVUProgCache::GetStats(0).preloadedPayloads, 1u);
	EXPECT_TRUE(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[rank[0]]), false));
	size_t all = 0;
	for (const std::string& p : paths_)
		all += SizeOf(p);
	Reopen(all);
	cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.preloadedPayloads, 4u);
	EXPECT_EQ(cstats.preloadBudgetHit, 0u);
}

TEST_F(MvuProgCachePreload, AProgramNotPreloadedLoadsWhenItAppearsAndRanksFirstAfterwards)
{
	SaveVariants();
	SetAge(paths_[0], 1);
	SetAge(paths_[1], 2);
	SetAge(paths_[2], 3);
	SetAge(paths_[3], 4);
	const size_t two = SizeOf(paths_[0]) + SizeOf(paths_[1]);
	Reopen(two);
	ASSERT_EQ(mVUProgCache::GetStats(0).preloadedPayloads, 2u);
	ASSERT_FALSE(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[3]), false));

	// Variant 3 was not preloaded: it is read from disk and hydrated, no compile.
	VuTestHarness& h = *h_;
	SeedVariant(h, 3);
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUPersist::GetBlockCompileCount(0), compiles_before) << "the program compiled instead of loading from disk";
	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.payloadHits, 1u);
	EXPECT_EQ(cstats.preloadHits, 0u) << "served from the preload";
	EXPECT_EQ(cstats.payloadMissing, 0u);
	EXPECT_FLOAT_EQ(h.GetVfJit(3, 'x'), 5.5f);
	EXPECT_FLOAT_EQ(h.GetVfJit(8, 'x'), 20.5f);

	// The load counts as a use: variant 3 is the newest file now, and the next session's one-program
	// budget takes it.
	EXPECT_LT(AgeDays(paths_[3]), 0.01);
	RecompilerTestEnvironment::ResetVuBlockCache(0); // (the session ends: its programs go)
	Reopen(SizeOf(paths_[3]));
	EXPECT_EQ(mVUProgCache::GetStats(0).preloadedPayloads, 1u);
	EXPECT_TRUE(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[3]), false));

	// A preloaded one is served from RAM.
	SeedVariant(h, 3);
	h.RunJitPreserveBlockCache();
	EXPECT_EQ(mVUProgCache::GetStats(0).preloadHits, 1u);
}

TEST_F(MvuProgCachePreload, ASidecarFollowsItsProgramsRankAndLoadsOnDemandPastTheBudget)
{
	const AotOn aot;
	SaveVariants();
	// 1 KiB sidecars of recognisable bytes
	std::vector<std::vector<u8>> side(kPrograms);
	for (u32 k = 0; k < kPrograms; k++)
	{
		side[k].assign(1024, static_cast<u8>(0x40 + k));
		mVUProgCache::QueueSidecar(0, HashOfPayload(paths_[k]), [b = side[k]]() { return b; });
	}
	mVUProgCache::FlushSidecars(0);
	SetAge(paths_[0], 1);
	SetAge(paths_[1], 2);
	SetAge(paths_[2], 3);
	SetAge(paths_[3], 4);
	// the sidecars count against the budget: two programs' payloads and sidecars
	Reopen(SizeOf(paths_[0]) + SizeOf(paths_[1]) + 2 * 1024);
	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.preloadedPayloads, 2u);
	EXPECT_EQ(cstats.sidecarsPreloaded, 2u);
	EXPECT_EQ(cstats.preloadBudgetHit, 1u);
	for (u32 k = 0; k < kPrograms; k++)
	{
		EXPECT_EQ(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[k]), true), k < 2) << "variant " << k;
		std::vector<u8> got;
		ASSERT_TRUE(mVUProgCache::ReadSidecar(0, HashOfPayload(paths_[k]), got)) << "variant " << k;
		EXPECT_EQ(got, side[k]) << "variant " << k;
	}
	cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.sidecarReads, 4u);
	EXPECT_EQ(cstats.sidecarPreloadHits, 2u) << "the other two were read from disk";
}

// The preload reads a session's files when the cache comes up and puts them in memory a little later, and a
// program asks for its sidecar when it first appears, which may be before that: it reads the file. A sidecar
// the session then writes for the program replaces the file, and the copy the preload puts in memory after
// all of this is the file as it was at startup. The next ask for the sidecar (the program is made again
// after a reset) must not be answered by that copy, older than the file the session wrote: the session
// remembers what its writer has written, and the preload does not put those hashes in memory.
//
// (Found as the flake of VuAotCache.UnusableSavedCodeFallsBackToThePlan, which failed about one run in
// twenty on a loaded machine: its second load got the edited file back instead of the one the first load's
// session had written.)
TEST_F(MvuProgCachePreload, ASidecarWrittenInTheSessionIsNotAnsweredByTheOlderCopyThePreloadHeld)
{
	const AotOn aot;
	SaveVariants();
	const XXH128_hash_t hash = HashOfPayload(paths_[0]);
	const std::vector<u8> before(512, 0x11), after(256, 0x22);
	mVUProgCache::QueueSidecar(0, hash, [before]() { return before; });
	mVUProgCache::FlushSidecars(0);
	// (the program is the most recently used, so the preload reads it first, and holds with it in hand)
	SetAge(paths_[0], 0);
	SetAge(paths_[1], 1);
	SetAge(paths_[2], 2);
	SetAge(paths_[3], 3);

	// A new session whose preload is slower than the first ask.
	mVUProgCache::ResetForTest(0);
	mVUProgCache::TestHoldPreload(0, true);
	mVUProgCache::TestReinitFromLiveSentinel(0);
	for (int i = 0; i < 5000 && !mVUProgCache::TestPreloadIsHeld(0); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	ASSERT_TRUE(mVUProgCache::TestPreloadIsHeld(0)) << "the preload has read its first file";
	std::vector<u8> got;
	ASSERT_TRUE(mVUProgCache::ReadSidecar(0, hash, got));
	EXPECT_EQ(got, before) << "the first ask finds nothing preloaded yet and reads the file";

	// The session learns more about the program and writes a new sidecar.
	mVUProgCache::QueueSidecar(0, hash, [after]() { return after; });
	mVUProgCache::FlushSidecars(0);

	// The preload catches up, with the file it read before that.
	mVUProgCache::TestHoldPreload(0, false);
	mVUProgCache::TestWaitForPreload(0);

	ASSERT_TRUE(mVUProgCache::ReadSidecar(0, hash, got));
	EXPECT_EQ(got, after) << "answered by the copy from before the session wrote its own";
}

// The same for a payload (SaveProgram rewrites it when the program's graph has grown).
TEST_F(MvuProgCachePreload, APayloadWrittenInTheSessionIsNotAnsweredByTheOlderCopyThePreloadHeld)
{
	SaveVariants();
	const XXH128_hash_t hash = HashOfPayload(paths_[0]);
	SetAge(paths_[0], 0);
	SetAge(paths_[1], 1);
	SetAge(paths_[2], 2);
	SetAge(paths_[3], 3);

	mVUProgCache::ResetForTest(0);
	mVUProgCache::TestHoldPreload(0, true);
	mVUProgCache::TestReinitFromLiveSentinel(0);
	for (int i = 0; i < 5000 && !mVUProgCache::TestPreloadIsHeld(0); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	ASSERT_TRUE(mVUProgCache::TestPreloadIsHeld(0)) << "the preload has read its first file";

	ASSERT_TRUE(mVUProgCache::TestWritePayload(0, hash, std::vector<u8>(777, 0x33)));
	mVUProgCache::TestHoldPreload(0, false);
	mVUProgCache::TestWaitForPreload(0);

	EXPECT_FALSE(mVUProgCache::TestIsPreloaded(0, hash, false)) << "the copy from before the session wrote its own";
	for (u32 k = 1; k < kPrograms; k++)
		EXPECT_TRUE(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[k]), false)) << "variant " << k << " was not written";
	EXPECT_EQ(mVUProgCache::GetStats(0).preloadedPayloads, kPrograms - 1u);
}

// A copy already in memory when the session writes goes at once.
TEST_F(MvuProgCachePreload, AWriteDropsTheCopyAlreadyInMemory)
{
	const AotOn aot;
	SaveVariants();
	const XXH128_hash_t hash = HashOfPayload(paths_[0]);
	mVUProgCache::QueueSidecar(0, hash, []() { return std::vector<u8>(512, 0x11); });
	mVUProgCache::FlushSidecars(0);
	Reopen(0);
	ASSERT_TRUE(mVUProgCache::TestIsPreloaded(0, hash, true));
	ASSERT_TRUE(mVUProgCache::TestIsPreloaded(0, hash, false));

	const std::vector<u8> after(256, 0x22);
	mVUProgCache::QueueSidecar(0, hash, [after]() { return after; });
	mVUProgCache::FlushSidecars(0);
	ASSERT_TRUE(mVUProgCache::TestWritePayload(0, hash, std::vector<u8>(300, 0x44)));
	EXPECT_FALSE(mVUProgCache::TestIsPreloaded(0, hash, true));
	EXPECT_FALSE(mVUProgCache::TestIsPreloaded(0, hash, false));
	std::vector<u8> got;
	ASSERT_TRUE(mVUProgCache::ReadSidecar(0, hash, got));
	EXPECT_EQ(got, after);

	// And the next session preloads the new files.
	Reopen(0);
	EXPECT_TRUE(mVUProgCache::TestIsPreloaded(0, hash, true));
	EXPECT_TRUE(mVUProgCache::TestIsPreloaded(0, hash, false));
}

TEST_F(MvuProgCachePreload, ProgramsUnusedForLongAreDeletedAndCompileAgainIfTheyReturn)
{
	const AotOn aot;
	SaveVariants();
	for (u32 k = 0; k < kPrograms; k++)
		mVUProgCache::QueueSidecar(0, HashOfPayload(paths_[k]), [k]() { return std::vector<u8>(512, static_cast<u8>(k + 1)); });
	mVUProgCache::FlushSidecars(0);
	const std::string sideOld = mVUProgCache::TestSidecarPath(0, HashOfPayload(paths_[1]));
	ASSERT_TRUE(FileSystem::FileExists(sideOld.c_str()));
	SetAge(paths_[1], 200); // far past the limit
	SetAge(paths_[2], 30);  // inside it

	Reopen(0);
	auto cstats = mVUProgCache::GetStats(0);
	EXPECT_EQ(cstats.ageOutFiles, 2u) << "the payload and the sidecar";
	EXPECT_GT(cstats.ageOutBytes, 512u);
	EXPECT_FALSE(FileSystem::FileExists(paths_[1].c_str()));
	EXPECT_FALSE(FileSystem::FileExists(sideOld.c_str()));
	for (u32 k : {0u, 2u, 3u})
	{
		EXPECT_TRUE(FileSystem::FileExists(paths_[k].c_str())) << "variant " << k;
		EXPECT_TRUE(FileSystem::FileExists(mVUProgCache::TestSidecarPath(0, HashOfPayload(paths_[k])).c_str())) << "variant " << k;
	}
	EXPECT_EQ(cstats.preloadedPayloads, 3u) << "the deleted program is not preloaded";
	EXPECT_EQ(cstats.sidecarsPreloaded, 3u);

	// The program comes back: its INDEX entry has no file, so it compiles, and the next save writes it again.
	VuTestHarness& h = *h_;
	SeedVariant(h, 1);
	const u64 compiles_before = mVUPersist::GetBlockCompileCount(0);
	h.RunJitPreserveBlockCache();
	EXPECT_GT(mVUPersist::GetBlockCompileCount(0), compiles_before);
	EXPECT_EQ(mVUProgCache::GetStats(0).payloadMissing, 1u);
	EXPECT_FLOAT_EQ(h.GetVfJit(6, 'x'), 20.5f);
	RecompilerTestEnvironment::ResetVuBlockCache(0);
	EXPECT_TRUE(FileSystem::FileExists(paths_[1].c_str())) << "the returned program was not saved again";
	EXPECT_LT(AgeDays(paths_[1]), 0.01);
}

//------------------------------------------------------------------
// Turning vu-aot off clears what it saved (mVUProgCache::OnVuAotSettingChanged, called by
// VMManager::CheckForCPUConfigChanges after the reset).
//------------------------------------------------------------------

namespace
{
// Every file under `dir` whose name ends in `ext`.
void FindWithExtension(const std::string& dir, const char* ext, std::vector<std::string>& out)
{
	if (!FileSystem::DirectoryExists(dir.c_str()))
		return;
	const size_t n = std::strlen(ext);
	FileSystem::FindResultsArray ents;
	FileSystem::FindFiles(dir.c_str(), "*",
		FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &ents);
	for (const auto& e : ents)
	{
		if (e.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
			FindWithExtension(e.FileName, ext, out);
		else if (e.FileName.size() > n && e.FileName.compare(e.FileName.size() - n, n, ext) == 0)
			out.push_back(e.FileName);
	}
}

XXH128_hash_t HashOf(u64 n)
{
	XXH128_hash_t h;
	h.low64 = n * 0x9E3779B97F4A7C15ull + 1;
	h.high64 = ~n;
	return h;
}
} // namespace

class MvuAotCacheClear : public MvuProgCachePreload
{
protected:
	void SetUp() override
	{
		MvuProgCachePreload::SetUp();
		saved_aot_ = EmuConfig.Cpu.Recompiler.EnableVUAot;
		EmuConfig.Cpu.Recompiler.EnableVUAot = true;
		RecompilerTestEnvironment::ResetVuBlockCache(0);
	}

	void TearDown() override
	{
		mVUProgCache::ResetForTest(1);
		EmuConfig.Cpu.Recompiler.EnableVUAot = saved_aot_;
		MvuProgCachePreload::TearDown();
	}

	std::string vu_jit() const { return Path::Combine(root_, "vu_jit"); }

	// The shard directory a file for `hash` lives in is the first byte of the name; a file written by hand
	// goes under the same shape.
	std::string HandFile(const std::string& dir, u64 n, const char* ext)
	{
		const std::string name = StringUtil::StdStringFromFormat("%016llx%016llx", (unsigned long long)HashOf(n).high64,
			(unsigned long long)HashOf(n).low64);
		const std::string shard = Path::Combine(dir, name.substr(0, 2));
		FileSystem::EnsureDirectoryExists(shard.c_str(), true);
		const std::string path = Path::Combine(shard, name + ext);
		const std::vector<u8> bytes(100 + n, static_cast<u8>(n));
		EXPECT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), bytes.data(), bytes.size()));
		return path;
	}

	// VU0: four real programs with sidecars (the first two preloaded). VU1: a cache of its own with two
	// sidecars and a payload. A set-aside cache directory with one of each. And the tripwire's memory.
	void Populate()
	{
		SaveVariants();
		for (u32 k = 0; k < kPrograms; k++)
			mVUProgCache::QueueSidecar(0, HashOfPayload(paths_[k]), [k]() { return std::vector<u8>(512, static_cast<u8>(k + 1)); });
		mVUProgCache::FlushSidecars(0);

		mVUProgCache::ResetForTest(1);
		mVUProgCache::TestReinitFromLiveSentinel(1);
		ASSERT_TRUE(mVUProgCache::GetStats(1).enabled);
		for (u64 n : {1, 2})
			mVUProgCache::QueueSidecar(1, HashOf(n), [n]() { return std::vector<u8>(256, static_cast<u8>(n)); });
		mVUProgCache::FlushSidecars(1);
		vu1_payload_ = HandFile(Path::Combine(vu_jit(), "vu1"), 3, ".vuprog");

		stale_ = Path::Combine(vu_jit(), "vu0.stale.123");
		stale_side_ = HandFile(stale_, 4, ".vuaot");
		stale_payload_ = HandFile(stale_, 5, ".vuprog");

		tripped_ = Path::Combine(vu_jit(), "vuaot_tripped.txt");
		ASSERT_TRUE(FileSystem::WriteStringToFile(tripped_.c_str(), "SCUS-97481 2F123FD8\n"));

		Reopen(0); // the next session: the sidecars of the first programs sit in RAM
		ASSERT_GE(mVUProgCache::GetStats(0).sidecarsPreloaded, 1u);
	}

	size_t CountSidecars() const
	{
		std::vector<std::string> v;
		FindWithExtension(vu_jit(), ".vuaot", v);
		return v.size();
	}

	size_t CountPayloads() const
	{
		std::vector<std::string> v;
		FindWithExtension(vu_jit(), ".vuprog", v);
		return v.size();
	}

	bool saved_aot_ = false;
	std::string vu1_payload_, stale_, stale_side_, stale_payload_, tripped_;
};

TEST_F(MvuAotCacheClear, OnToOffDeletesTheSidecarsAndKeepsWhatAnotherSettingStillWants)
{
	Populate();
	ASSERT_EQ(CountSidecars(), 4u + 2u + 1u);
	ASSERT_EQ(CountPayloads(), 4u + 1u + 1u);
	ASSERT_TRUE(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[0]), true));

	// EnableVUProgramCache stays on: the payloads and the INDEX are not touched.
	const auto r = mVUProgCache::OnVuAotSettingChanged(true, false, true);
	EXPECT_EQ(r.files, 7u);
	EXPECT_GT(r.bytes, 7u * 100u);
	EXPECT_EQ(CountSidecars(), 0u) << "VU0, VU1 and the set-aside directory";
	EXPECT_EQ(CountPayloads(), 6u);
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str())) << "the tripwire's memory stays";
	EXPECT_TRUE(FileSystem::FileExists(Path::Combine(Path::Combine(vu_jit(), "vu0"), "INDEX").c_str()));
	EXPECT_TRUE(FileSystem::FileExists(Path::Combine(Path::Combine(vu_jit(), "vu0"), "VERSION").c_str()));
	EXPECT_FALSE(mVUPersist::IsProcessDisabled());

	// What the preload held is gone too, and the cache still works: a new sidecar is written.
	EXPECT_FALSE(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[0]), true));
	EXPECT_TRUE(mVUProgCache::GetStats(0).initialized);
	mVUProgCache::QueueSidecar(0, HashOfPayload(paths_[0]), []() { return std::vector<u8>(64, 1); });
	mVUProgCache::FlushSidecars(0);
	EXPECT_EQ(CountSidecars(), 1u);
}

TEST_F(MvuAotCacheClear, OnToOffWithNothingElseUsingTheCacheDeletesTheWholeCache)
{
	Populate();
	const auto r = mVUProgCache::OnVuAotSettingChanged(true, false, false);
	EXPECT_GE(r.files, 7u + 6u + 2u) << "sidecars, payloads, and the INDEX and VERSION of both caches";
	EXPECT_FALSE(FileSystem::DirectoryExists(Path::Combine(vu_jit(), "vu0").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(Path::Combine(vu_jit(), "vu1").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(stale_.c_str())) << "the set-aside cache";
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str())) << "the tripwire's memory stays";
	EXPECT_FALSE(mVUProgCache::GetStats(0).initialized) << "the cache in memory is closed";
	EXPECT_FALSE(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[0]), false));

	// Switched on again, the cache starts fresh.
	mVUProgCache::TestReinitFromLiveSentinel(0);
	EXPECT_TRUE(mVUProgCache::GetStats(0).enabled);
	EXPECT_TRUE(FileSystem::FileExists(Path::Combine(Path::Combine(vu_jit(), "vu0"), "VERSION").c_str()));
	EXPECT_EQ(mVUProgCache::GetStats(0).entries, 0u);
}

TEST_F(MvuAotCacheClear, EveryOtherChangeClearsNothing)
{
	Populate();
	const size_t sidecars = CountSidecars(), payloads = CountPayloads();
	for (const bool programCache : {false, true})
	{
		const auto before_off_on = mVUProgCache::OnVuAotSettingChanged(false, true, programCache);
		const auto on_on = mVUProgCache::OnVuAotSettingChanged(true, true, programCache);
		const auto off_off = mVUProgCache::OnVuAotSettingChanged(false, false, programCache);
		EXPECT_EQ(before_off_on.files + on_on.files + off_off.files, 0u);
	}
	EXPECT_EQ(CountSidecars(), sidecars);
	EXPECT_EQ(CountPayloads(), payloads);
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str()));
	EXPECT_TRUE(mVUProgCache::TestIsPreloaded(0, HashOfPayload(paths_[0]), true)) << "the preload is untouched";
}

// Settings applied with no VM running (VMManager::CheckForConfigChanges in the Shutdown state) compare the
// global setting with the global value last seen with no VM, which VMManager records at startup and as a VM
// ends. The config a game ends with, its own settings over the global ones, never counts: a boot that
// fails applies settings in the Shutdown state with exactly that config as the old one.
TEST_F(MvuAotCacheClear, LeavingAGameWithItsOwnSettingOnAndTheGlobalOneOffClearsNothing)
{
	Populate();
	const size_t sidecars = CountSidecars(), payloads = CountPayloads();
	vuAotNoteGlobalSetting(false); // startup: the global setting is off (the game's own settings turn it on)
	// ...the game runs and ends; a failed boot, or any later apply, sees the global setting off again
	vuAotNoVmSettingsApplied(false, false);
	vuAotNoVmSettingsApplied(false, false);
	EXPECT_EQ(CountSidecars(), sidecars);
	EXPECT_EQ(CountPayloads(), payloads);
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str()));
}

TEST_F(MvuAotCacheClear, TheGlobalSettingTurnedOffWithNoVmClearsOnceAndOnlyThen)
{
	Populate();
	const size_t payloads = CountPayloads();
	vuAotNoteGlobalSetting(true);
	vuAotNoVmSettingsApplied(true, true); // an unrelated setting applied
	EXPECT_EQ(CountSidecars(), 7u);

	vuAotNoVmSettingsApplied(false, true); // on to off, the program cache setting on: sidecars only
	EXPECT_EQ(CountSidecars(), 0u);
	EXPECT_EQ(CountPayloads(), payloads);

	// Off to off clears nothing (new files stay), and so does off to on.
	mVUProgCache::QueueSidecar(0, HashOfPayload(paths_[0]), []() { return std::vector<u8>(64, 1); });
	mVUProgCache::FlushSidecars(0);
	vuAotNoVmSettingsApplied(false, true);
	vuAotNoVmSettingsApplied(true, true);
	EXPECT_EQ(CountSidecars(), 1u);

	// On again, and off again: a second change, a second clear. With nothing keeping the program cache it goes whole.
	vuAotNoVmSettingsApplied(false, false);
	EXPECT_EQ(CountSidecars(), 0u);
	EXPECT_EQ(CountPayloads(), 0u);
	EXPECT_TRUE(FileSystem::FileExists(tripped_.c_str()));
}

TEST_F(MvuAotCacheClear, TheGlobalValueNotedAsAGameEndsIsNotAChangeMadeWithNoVm)
{
	Populate();
	const size_t sidecars = CountSidecars();
	vuAotNoteGlobalSetting(true); // startup: on
	// the global setting is turned off while a game runs (the game's own settings keep it on), and the VM ends
	vuAotNoteGlobalSetting(false);
	vuAotNoVmSettingsApplied(false, false);
	EXPECT_EQ(CountSidecars(), sidecars);
}

TEST_F(MvuAotCacheClear, ANonexistentCacheDirectoryIsFine)
{
	// (the fixture's reset brought VU0's cache up under the temporary root: take it away again)
	mVUProgCache::ResetForTest(0);
	RecursiveRm(vu_jit());
	EXPECT_FALSE(FileSystem::DirectoryExists(vu_jit().c_str()));
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
	// Slow builds, so most of the queue is still waiting when the clear starts.
	for (u64 n = 1; n <= 12; n++)
	{
		mVUProgCache::QueueSidecar(0, HashOf(n), [n]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			return std::vector<u8>(64, static_cast<u8>(n));
		});
	}
	mVUProgCache::ClearVuAot(false);
	// (the one being built when the clear started is finished and deleted; the rest are dropped, not written
	// and deleted)
	EXPECT_LE(mVUProgCache::GetStats(0).sidecarWrites, 2u) << "the queue was written out";
	EXPECT_EQ(CountSidecars(), 0u) << "the writer put a file back after the clear";
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_EQ(CountSidecars(), 0u) << "the writer was still running";
}

} // namespace recompiler_tests
