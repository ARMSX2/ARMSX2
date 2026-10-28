// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// mVUProgCache packs: where they live, the header handshake, the log's
// crash safety, and what is deleted as leftovers.
//
// A pack is one file per title, VU and options sentinel,
// vu_jit/packs/<title>/vu{0,1}-<sentinel hex>.pack: a 64-byte header
// (format, compiler ABI, VU, sentinel, arch), then an append-only log of
// checksummed records. Nothing is written until there is something to
// write, and nothing a session writes touches another pack.
//
// What this proves:
//
//   1. Init writes nothing; the first flush writes a header matching the
//      running build.
//   2. Another sentinel, or another title, opens a pack of its own and
//      leaves the first one intact (two games with different GameDB VU
//      settings, played in turn, keep a cache each).
//   3. A pack whose header differs (magic, format, compiler ABI, VU, arch)
//      loads nothing and is started again at the next write.
//   4. A log that ends in a broken record keeps every record before it,
//      and the next write cuts the broken one off first.
//   5. The older layouts, writes cut short, and packs unused for 60 days
//      are deleted; recent packs and the tripwire's memory stay.
//
// Records here are vu-aot sidecars under made-up hashes: they need no
// microVU behind them. mvu_progcache_disk_tests.cpp covers programs.
//
// References:
//   pcsx2/arm64/microVU_ProgCache-arm64.inl  PackHeader, RecordHeader, LoadPack, WritePack, DeleteLeftovers
//   pcsx2/arm64/microVU-arm64.h             kMvuCompilerAbiVersion
//   pcsx2/arm64/microVU_ProgCache-arm64.h    kProgCacheFormatVersion, ResetForTest

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

#include "Config.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "arm64/microVU_ProgCache-arm64.h"
#include "EECycleRate.h"

// kMvuCompilerAbiVersion lives in microVU-arm64.h, but that header
// also #includes the .inl files at the bottom — including it from
// a test TU collides with libpcsx2.a's emitted bodies. Mirror the
// constant here; a future bump must keep the two in sync. Drift is
// caught directly by `mirror_matches_production_abi_version` below
// (which compares this against the linkable accessor
// mVUProgCache::GetCompilerAbiVersion()), with a clear message.
namespace pcsx2_test
{
	static constexpr u32 kMvuCompilerAbiVersionMirror = 63;
}

namespace
{
	// Mirrors of the layouts in microVU_ProgCache-arm64.inl, so the tests can
	// read and poison packs without a backdoor in production. If the
	// production layout drifts, the round trips below fail.
#pragma pack(push, 1)
	struct PackHeaderMirror
	{
		u32           magic;
		u32           formatVersion;
		u32           compilerAbiVersion;
		u32           vuIndex;
		XXH128_hash_t optionsSentinel;
		char          archTag[16];
		u8            reserved[16];
	};
	struct RecordHeaderMirror
	{
		u32           magic;
		u8            kind;
		u8            reserved[3];
		u32           length;
		u32           reserved2;
		XXH128_hash_t hash;
		u64           checksum;
	};
#pragma pack(pop)
	static_assert(sizeof(PackHeaderMirror) == 64, "PackHeader mirror drifted");
	static_assert(sizeof(RecordHeaderMirror) == 40, "RecordHeader mirror drifted");

	constexpr u32 kPackMagicMirror = 0x4B415056u; // 'VPAK'
	constexpr u8 kRecordSidecar = 2;

	XXH128_hash_t Hash(u64 lo, u64 hi)
	{
		XXH128_hash_t h;
		h.low64 = lo;
		h.high64 = hi;
		return h;
	}

	// A fresh temporary cache folder for the test, removed with everything under it.
	class TempCacheRoot
	{
	public:
		TempCacheRoot()
		{
			char tmpl[] = "/tmp/progcache_ver_XXXXXX";
			char* p = ::mkdtemp(tmpl);
			EXPECT_NE(p, nullptr) << "mkdtemp failed: " << std::strerror(errno);
			m_root = p ? p : std::string();
			m_prev_cache = EmuFolders::Cache;
			EmuFolders::Cache = m_root;
		}
		~TempCacheRoot()
		{
			mVUProgCache::ResetForTest(0);
			mVUProgCache::ResetForTest(1);
			mVUProgCache::SetTitle("", 0);
			EmuFolders::Cache = m_prev_cache;
			// (an empty root would make the removal below start at "/")
			if (m_root.empty())
				return;
			std::error_code ec;
			std::filesystem::remove_all(m_root, ec);
		}
		const std::string& root() const { return m_root; }
		std::string jit() const { return Path::Combine(m_root, "vu_jit"); }
		std::string packs() const { return Path::Combine(jit(), "packs"); }
		std::string pack(const char* title, u32 vu, const XXH128_hash_t& sentinel) const
		{
			char name[64];
			std::snprintf(name, sizeof(name), "vu%u-%016llx%016llx.pack", vu,
				static_cast<unsigned long long>(sentinel.high64), static_cast<unsigned long long>(sentinel.low64));
			return Path::Combine(Path::Combine(packs(), title), name);
		}
	private:
		std::string m_root;
		std::string m_prev_cache;
	};

	// Stores a sidecar of `size` bytes, all `fill`, under `hash`.
	void PutSidecar(u32 vu, const XXH128_hash_t& hash, u8 fill, size_t size = 64)
	{
		mVUProgCache::QueueSidecar(vu, hash, [fill, size]() { return std::vector<u8>(size, fill); });
	}

	bool ReadBack(u32 vu, const XXH128_hash_t& hash, u8 fill)
	{
		std::vector<u8> out;
		if (!mVUProgCache::ReadSidecar(vu, hash, out) || out.empty())
			return false;
		return out[0] == fill;
	}

	// The records of a pack file, in order (kind, hash, length); stops at the first one that does not fit.
	struct Rec
	{
		u8 kind;
		XXH128_hash_t hash;
		u32 length;
	};
	std::vector<Rec> Records(const std::string& path, PackHeaderMirror* header = nullptr)
	{
		std::vector<Rec> out;
		auto bytes = FileSystem::ReadBinaryFile(path.c_str());
		if (!bytes.has_value() || bytes->size() < sizeof(PackHeaderMirror))
			return out;
		if (header)
			std::memcpy(header, bytes->data(), sizeof(*header));
		size_t off = sizeof(PackHeaderMirror);
		while (bytes->size() - off >= sizeof(RecordHeaderMirror))
		{
			RecordHeaderMirror h;
			std::memcpy(&h, bytes->data() + off, sizeof(h));
			if (h.length > bytes->size() - off - sizeof(h))
				break;
			out.push_back({h.kind, h.hash, h.length});
			off += sizeof(h) + h.length;
		}
		return out;
	}

	void SetAgeDays(const std::string& path, int days)
	{
		std::error_code ec;
		std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * days), ec);
		ASSERT_FALSE(ec) << path;
	}
}

// 1. Init writes nothing; the first flush writes the pack, its header the running build's.
TEST(ProgCachePack, nothing_is_written_until_there_is_something_to_write)
{
	TempCacheRoot tmp;
	const XXH128_hash_t sentinel = Hash(0xAAAAAAAAAAAAAAAAull, 0xBBBBBBBBBBBBBBBBull);
	mVUProgCache::InitWithSentinel(1, sentinel);
	auto stats = mVUProgCache::GetStats(1);
	EXPECT_TRUE(stats.initialized);
	EXPECT_TRUE(stats.enabled);
	EXPECT_EQ(stats.entries, 0u);
	mVUProgCache::TestWaitForPreload(1);
	const std::string path = tmp.pack("_", 1, sentinel);
	EXPECT_EQ(mVUProgCache::TestPackPath(1), path);
	EXPECT_FALSE(FileSystem::FileExists(path.c_str())) << "an empty cache wrote a file";

	PutSidecar(1, Hash(1, 2), 0x11);
	EXPECT_FALSE(FileSystem::FileExists(path.c_str())) << "storing is RAM only";
	mVUProgCache::FlushSidecars(1);
	PackHeaderMirror hdr{};
	const std::vector<Rec> recs = Records(path, &hdr);
	ASSERT_EQ(recs.size(), 1u);
	EXPECT_EQ(recs[0].kind, kRecordSidecar);
	EXPECT_EQ(hdr.magic, kPackMagicMirror);
	EXPECT_EQ(hdr.formatVersion, mVUProgCache::kProgCacheFormatVersion);
	EXPECT_EQ(hdr.compilerAbiVersion, pcsx2_test::kMvuCompilerAbiVersionMirror);
	EXPECT_EQ(hdr.vuIndex, 1u);
	EXPECT_EQ(hdr.optionsSentinel.low64, sentinel.low64);
	EXPECT_EQ(hdr.optionsSentinel.high64, sentinel.high64);
	EXPECT_STREQ(hdr.archTag, "arm64-jit");

	// A new process loads it.
	mVUProgCache::ResetForTest(1);
	mVUProgCache::InitWithSentinel(1, sentinel);
	mVUProgCache::TestWaitForPreload(1);
	EXPECT_EQ(mVUProgCache::GetStats(1).loadedRecords, 1u);
	EXPECT_TRUE(ReadBack(1, Hash(1, 2), 0x11));
}

TEST(ProgCachePack, mirror_matches_production_abi_version)
{
	EXPECT_EQ(pcsx2_test::kMvuCompilerAbiVersionMirror, mVUProgCache::GetCompilerAbiVersion())
		<< "kMvuCompilerAbiVersionMirror in this test is out of date: update it to the value in microVU-arm64.h";
}

// 2. A different sentinel gets a pack of its own, and the first one is still there when its sentinel comes
//    back. The GameDB sets the VU clamp modes and gamefixes per game, so two games played in turn can have
//    two sentinels; neither may throw the other's cache away.
TEST(ProgCachePack, optionsSentinel_change_keeps_each_cache)
{
	TempCacheRoot tmp;
	const XXH128_hash_t s_a = Hash(0xDEAD0000ull, 0xBEEF0000ull), s_b = Hash(0x1234ull, 0x5678ull);
	const XXH128_hash_t prog = Hash(0x0A0A0A0Aull, 0x0B0B0B0Bull);

	mVUProgCache::InitWithSentinel(1, s_a);
	PutSidecar(1, prog, 0x2A);
	mVUProgCache::ResetForTest(1); // (writes it)

	mVUProgCache::InitWithSentinel(1, s_b);
	mVUProgCache::TestWaitForPreload(1);
	EXPECT_EQ(mVUProgCache::GetStats(1).entries, 0u) << "s_b's cache starts empty";
	PutSidecar(1, Hash(7, 7), 0x07);
	mVUProgCache::ResetForTest(1);

	mVUProgCache::InitWithSentinel(1, s_a);
	mVUProgCache::TestWaitForPreload(1);
	EXPECT_TRUE(ReadBack(1, prog, 0x2A)) << "s_a's cache must survive a session under s_b";
	EXPECT_FALSE(mVUProgCache::TestHasEntry(1, Hash(7, 7), true));
	EXPECT_EQ(Records(tmp.pack("_", 1, s_a)).size(), 1u);
	EXPECT_EQ(Records(tmp.pack("_", 1, s_b)).size(), 1u);
}

// 2b. The same for titles: a pack per game, under a directory named by its serial and CRC.
TEST(ProgCachePack, each_title_has_its_own_pack)
{
	TempCacheRoot tmp;
	const XXH128_hash_t sentinel = Hash(5, 6), prog = Hash(0x77, 0x88);

	mVUProgCache::SetTitle("SLUS-21065", 0xA1B2C3D4u);
	mVUProgCache::InitWithSentinel(1, sentinel);
	EXPECT_EQ(mVUProgCache::TestPackPath(1), tmp.pack("SLUS-21065_A1B2C3D4", 1, sentinel));
	PutSidecar(1, prog, 0x31);
	mVUProgCache::ResetForTest(1);

	mVUProgCache::SetTitle("SCES 50/01", 0x1u); // (characters a file name cannot hold become '_')
	mVUProgCache::InitWithSentinel(1, sentinel);
	EXPECT_EQ(mVUProgCache::TestPackPath(1), tmp.pack("SCES_50_01_00000001", 1, sentinel));
	mVUProgCache::TestWaitForPreload(1);
	EXPECT_FALSE(mVUProgCache::TestHasEntry(1, prog, true)) << "another game's records";
	mVUProgCache::ResetForTest(1);

	mVUProgCache::SetTitle("SLUS-21065", 0xA1B2C3D4u);
	mVUProgCache::InitWithSentinel(1, sentinel);
	mVUProgCache::TestWaitForPreload(1);
	EXPECT_TRUE(ReadBack(1, prog, 0x31));
}

// 3. A header that differs in any field loads nothing, and the next write starts the file again.
TEST(ProgCachePack, a_pack_of_another_format_is_replaced)
{
	struct Poison
	{
		const char* what;
		void (*apply)(PackHeaderMirror&);
	};
	const Poison poisons[] = {
		{"magic", [](PackHeaderMirror& h) { h.magic = 0xDEADBEEFu; }},
		{"formatVersion", [](PackHeaderMirror& h) { h.formatVersion = mVUProgCache::kProgCacheFormatVersion - 1; }},
		{"compilerAbiVersion", [](PackHeaderMirror& h) { h.compilerAbiVersion ^= 0x100u; }},
		{"vuIndex", [](PackHeaderMirror& h) { h.vuIndex = 0; }},
		{"archTag", [](PackHeaderMirror& h) { std::strncpy(h.archTag, "x86-64", sizeof(h.archTag)); }},
	};
	for (const Poison& poison : poisons)
	{
		TempCacheRoot tmp;
		const XXH128_hash_t sentinel = Hash(0x7777ull, 0x8888ull);
		mVUProgCache::InitWithSentinel(1, sentinel);
		PutSidecar(1, Hash(1, 1), 0x01);
		mVUProgCache::ResetForTest(1);

		const std::string path = tmp.pack("_", 1, sentinel);
		auto bytes = FileSystem::ReadBinaryFile(path.c_str());
		ASSERT_TRUE(bytes.has_value()) << poison.what;
		PackHeaderMirror hdr;
		std::memcpy(&hdr, bytes->data(), sizeof(hdr));
		poison.apply(hdr);
		std::memcpy(bytes->data(), &hdr, sizeof(hdr));
		ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), bytes->data(), bytes->size()));

		mVUProgCache::InitWithSentinel(1, sentinel);
		mVUProgCache::TestWaitForPreload(1);
		auto stats = mVUProgCache::GetStats(1);
		EXPECT_EQ(stats.otherFormat, 1u) << poison.what;
		EXPECT_EQ(stats.entries, 0u) << poison.what;
		EXPECT_FALSE(mVUProgCache::TestHasEntry(1, Hash(1, 1), true)) << poison.what;

		PutSidecar(1, Hash(2, 2), 0x02);
		mVUProgCache::FlushSidecars(1);
		PackHeaderMirror now{};
		const std::vector<Rec> recs = Records(path, &now);
		EXPECT_EQ(recs.size(), 1u) << poison.what << ": the old records went with the old format";
		EXPECT_EQ(now.magic, kPackMagicMirror) << poison.what;
		EXPECT_EQ(now.formatVersion, mVUProgCache::kProgCacheFormatVersion) << poison.what;
		EXPECT_EQ(now.vuIndex, 1u) << poison.what;
	}
}

// 4. A log that ends in a broken record (a write cut short, a flipped byte) keeps what comes before it,
//    and the next write cuts the broken record off, so what it appends can be read.
TEST(ProgCachePack, a_broken_record_ends_the_log_and_is_cut_off)
{
	TempCacheRoot tmp;
	const XXH128_hash_t sentinel = Hash(0xC0FFEEull, 0xBBull);
	mVUProgCache::InitWithSentinel(1, sentinel);
	PutSidecar(1, Hash(1, 1), 0x01);
	PutSidecar(1, Hash(2, 2), 0x02);
	mVUProgCache::FlushSidecars(1);
	PutSidecar(1, Hash(3, 3), 0x03, 200);
	mVUProgCache::ResetForTest(1);
	const std::string path = tmp.pack("_", 1, sentinel);
	ASSERT_EQ(Records(path).size(), 3u);

	// The last record's last byte flips: its checksum fails.
	auto bytes = FileSystem::ReadBinaryFile(path.c_str());
	ASSERT_TRUE(bytes.has_value());
	bytes->back() ^= 0xFF;
	ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), bytes->data(), bytes->size()));

	mVUProgCache::InitWithSentinel(1, sentinel);
	mVUProgCache::TestWaitForPreload(1);
	auto stats = mVUProgCache::GetStats(1);
	EXPECT_EQ(stats.tornTails, 1u);
	EXPECT_EQ(stats.loadedRecords, 2u);
	EXPECT_TRUE(ReadBack(1, Hash(1, 1), 0x01));
	EXPECT_TRUE(ReadBack(1, Hash(2, 2), 0x02));
	EXPECT_FALSE(mVUProgCache::TestHasEntry(1, Hash(3, 3), true));

	PutSidecar(1, Hash(4, 4), 0x04);
	mVUProgCache::ResetForTest(1);
	mVUProgCache::InitWithSentinel(1, sentinel);
	mVUProgCache::TestWaitForPreload(1);
	stats = mVUProgCache::GetStats(1);
	EXPECT_EQ(stats.tornTails, 0u) << "the broken record was not cut off";
	EXPECT_EQ(stats.loadedRecords, 3u);
	EXPECT_TRUE(ReadBack(1, Hash(4, 4), 0x04));
}

// 4b. A record written again supersedes the earlier one, in RAM and in the next session; once most of the
//     file is superseded records, the writer writes it again whole.
TEST(ProgCachePack, the_newest_record_wins_and_a_mostly_dead_pack_is_rewritten)
{
	TempCacheRoot tmp;
	const XXH128_hash_t sentinel = Hash(0x5Eull, 0x5Full), prog = Hash(9, 9);
	mVUProgCache::InitWithSentinel(1, sentinel);
	const size_t big = 300u << 10;
	for (u8 round = 1; round <= 6; round++)
	{
		PutSidecar(1, prog, round, big);
		mVUProgCache::FlushSidecars(1);
	}
	EXPECT_GE(mVUProgCache::GetStats(1).rewrites, 1u) << "1.5 MiB of file for 300 KiB of records";
	const s64 size = FileSystem::GetPathFileSize(tmp.pack("_", 1, sentinel).c_str());
	EXPECT_LT(size, static_cast<s64>(3 * big)) << "the rewrite left superseded records behind";
	mVUProgCache::ResetForTest(1);

	mVUProgCache::InitWithSentinel(1, sentinel);
	mVUProgCache::TestWaitForPreload(1);
	EXPECT_TRUE(ReadBack(1, prog, 6));
}

// 4c. Past the RAM cap, a read keeps what fits and leaves the rest on disk: the pack is never rewritten
//     from a table that does not hold all of it.
TEST(ProgCachePack, past_the_ram_cap_the_pack_is_kept_whole_on_disk)
{
	TempCacheRoot tmp;
	const XXH128_hash_t sentinel = Hash(0xCAull, 0xFEull);
	mVUProgCache::InitWithSentinel(1, sentinel);
	for (u64 n = 1; n <= 8; n++)
		PutSidecar(1, Hash(n, n), static_cast<u8>(n), 64u << 10);
	mVUProgCache::ResetForTest(1);
	const std::string path = tmp.pack("_", 1, sentinel);
	ASSERT_EQ(Records(path).size(), 8u);

	mVUProgCache::TestSetRamCap(1, 200u << 10);
	mVUProgCache::InitWithSentinel(1, sentinel);
	mVUProgCache::TestWaitForPreload(1);
	auto stats = mVUProgCache::GetStats(1);
	EXPECT_EQ(stats.ramCapHit, 1u);
	EXPECT_LE(stats.ramBytes, 200u << 10);
	EXPECT_LT(stats.entries, 8u);
	PutSidecar(1, Hash(20, 20), 0x20);
	mVUProgCache::ResetForTest(1);
	EXPECT_EQ(Records(path).size(), 9u) << "records the table did not hold were lost";
}

// 5. Leftovers: the layouts before packs (vu0/, vu1/ and the vu{0,1}.stale.* they set aside), a write cut
//    short, and packs unused for 60 days. A pack used 10 days ago stays, and so does the tripwire's memory.
TEST(ProgCachePack, leftovers_and_unused_packs_are_deleted)
{
	TempCacheRoot tmp;
	const auto make_file = [](const std::string& dir, const char* name) {
		ASSERT_TRUE(FileSystem::EnsureDirectoryExists(dir.c_str(), true));
		ASSERT_TRUE(FileSystem::WriteStringToFile(Path::Combine(dir, name).c_str(), "x"));
	};
	const std::string jit = tmp.jit();
	make_file(Path::Combine(jit, "vu1.stale.123"), "INDEX");
	make_file(Path::Combine(jit, "vu0"), "VERSION");
	make_file(Path::Combine(Path::Combine(jit, "vu1"), "0123456789abcdef0123456789abcdef"), "INDEX");
	make_file(jit, "vuaot_tripped.txt");
	const std::string game = Path::Combine(tmp.packs(), "SLUS-00001_00000000");
	const std::string other = Path::Combine(tmp.packs(), "SLUS-00002_00000000");
	make_file(game, "vu1-00000000000000000000000000000001.pack");
	make_file(game, "vu1-00000000000000000000000000000002.pack");
	make_file(game, "vu1-00000000000000000000000000000002.pack.tmp");
	make_file(other, "vu0-00000000000000000000000000000003.pack");
	SetAgeDays(Path::Combine(game, "vu1-00000000000000000000000000000001.pack"), 10);
	SetAgeDays(Path::Combine(game, "vu1-00000000000000000000000000000002.pack"), 61);
	SetAgeDays(Path::Combine(game, "vu1-00000000000000000000000000000002.pack.tmp"), 1);
	SetAgeDays(Path::Combine(other, "vu0-00000000000000000000000000000003.pack"), 90);

	mVUProgCache::InitWithSentinel(1, Hash(1, 0));
	mVUProgCache::TestWaitForPreload(1);

	EXPECT_FALSE(FileSystem::DirectoryExists(Path::Combine(jit, "vu1.stale.123").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(Path::Combine(jit, "vu0").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(Path::Combine(jit, "vu1").c_str()));
	EXPECT_TRUE(FileSystem::FileExists(Path::Combine(game, "vu1-00000000000000000000000000000001.pack").c_str())) << "used 10 days ago";
	EXPECT_FALSE(FileSystem::FileExists(Path::Combine(game, "vu1-00000000000000000000000000000002.pack").c_str())) << "unused for 61 days";
	EXPECT_FALSE(FileSystem::FileExists(Path::Combine(game, "vu1-00000000000000000000000000000002.pack.tmp").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(other.c_str())) << "a title with no pack left";
	EXPECT_TRUE(FileSystem::FileExists(Path::Combine(jit, "vuaot_tripped.txt").c_str())) << "the tripwire's memory stays";
	EXPECT_GE(mVUProgCache::GetStats(1).leftoverFiles, 6u);
}

// Opening a pack marks its use, so a pack played now and then is not taken for an unused one.
TEST(ProgCachePack, opening_a_pack_marks_its_use)
{
	TempCacheRoot tmp;
	const XXH128_hash_t sentinel = Hash(3, 4);
	mVUProgCache::InitWithSentinel(1, sentinel);
	PutSidecar(1, Hash(1, 1), 1);
	mVUProgCache::ResetForTest(1);
	const std::string path = tmp.pack("_", 1, sentinel);
	SetAgeDays(path, 59);
	mVUProgCache::InitWithSentinel(1, sentinel);
	mVUProgCache::TestWaitForPreload(1);
	std::error_code ec;
	const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(path, ec);
	ASSERT_FALSE(ec);
	// In minutes, as a plain integer: libc++'s file clock counts in __int128, which gtest cannot print.
	EXPECT_LT(std::chrono::duration_cast<std::chrono::minutes>(age).count(), 60);
}

// The pack being opened is never taken for an unused one, however old its time says it is: the leftover
// pass and the read run side by side, and the read marks the use only once it is done.
TEST(ProgCachePack, the_pack_being_opened_is_not_deleted)
{
	TempCacheRoot tmp;
	const XXH128_hash_t sentinel = Hash(8, 9);
	mVUProgCache::InitWithSentinel(1, sentinel);
	PutSidecar(1, Hash(1, 1), 0x5A);
	mVUProgCache::ResetForTest(1);
	const std::string path = tmp.pack("_", 1, sentinel);
	SetAgeDays(path, 90);

	mVUProgCache::TestHoldLoad(1, true); // (the read waits, with the file read, until the leftover pass is over)
	mVUProgCache::InitWithSentinel(1, sentinel);
	for (int i = 0; i < 2000 && !mVUProgCache::TestLoadIsHeld(1); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	PutSidecar(1, Hash(2, 2), 0x5B);
	std::this_thread::sleep_for(std::chrono::milliseconds(50)); // (the writer's leftover pass runs first)
	mVUProgCache::TestHoldLoad(1, false);
	mVUProgCache::TestWaitForPreload(1);
	EXPECT_TRUE(FileSystem::FileExists(path.c_str()));
	EXPECT_TRUE(ReadBack(1, Hash(1, 1), 0x5A));
	mVUProgCache::ResetForTest(1);
	EXPECT_EQ(Records(path).size(), 2u);
}

// 6. Empty EmuFolders::Cache (e.g. test harness with no VMManager init) disables the cache silently. Init
//    must NOT touch /tmp or anywhere unsolicited.
TEST(ProgCachePack, empty_emu_folders_disables_cache)
{
	const std::string prev = EmuFolders::Cache;
	EmuFolders::Cache.clear();
	mVUProgCache::ResetForTest(1);

	mVUProgCache::InitWithSentinel(1, XXH128_hash_t{});
	const auto stats = mVUProgCache::GetStats(1);
	EXPECT_TRUE(stats.initialized);
	EXPECT_FALSE(stats.enabled) << "empty EmuFolders::Cache must result in enabled=false";
	EXPECT_EQ(mVUProgCache::TestPackPath(1), "");

	mVUProgCache::ResetForTest(1);
	EmuFolders::Cache = prev;
}

// ---------------------------------------------------------------------------
// EE cycle rate in the options sentinel: a VU0-only code-generation input.
//
// mVUtestCycles applies the EE nominal-rate adjustment when compiling VU0 and
// only VU0 — VU1's generated code has never depended on the EE cycle-rate
// selector. The sentinel used to bake the selector for both indices anyway, so
// changing the EE speedhack changed every VU1 program's identity and threw the
// whole VU1 disk cache away for nothing. With a dynamic governor moving the
// effective rate at runtime that would be a cache eviction per transition.
//
// These pin the property in both directions: VU0's identity must move with the
// effective rate (its emitted cycle counts really do change), and VU1's must
// not move at all.
// ---------------------------------------------------------------------------

namespace
{
	XXH128_hash_t SentinelAtEffectiveRate(u32 vu_index, int rate)
	{
		EmuConfig.Speedhacks.EECycleRate = static_cast<s8>(rate);
		EECycleRate::SyncToConfigured();
		XXH128_hash_t h{};
		EXPECT_TRUE(mVUProgCache::TestBuildSentinelForIndex(vu_index, &h));
		return h;
	}

	bool SameHash(const XXH128_hash_t& a, const XXH128_hash_t& b)
	{
		return a.low64 == b.low64 && a.high64 == b.high64;
	}
} // namespace

TEST(MvuOptionsSentinelEECycleRate, Vu0IdentityFollowsTheEffectiveRate)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;

	const XXH128_hash_t at0  = SentinelAtEffectiveRate(0, 0);
	const XXH128_hash_t atm1 = SentinelAtEffectiveRate(0, -1);
	const XXH128_hash_t atm2 = SentinelAtEffectiveRate(0, -2);
	const XXH128_hash_t at0_again = SentinelAtEffectiveRate(0, 0);

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();

	EXPECT_FALSE(SameHash(at0, atm1)) << "VU0 emits different cycle counts at -1; its identity must say so";
	EXPECT_FALSE(SameHash(atm1, atm2));
	EXPECT_TRUE(SameHash(at0, at0_again)) << "the sentinel is not a pure function of the config";
}

TEST(MvuOptionsSentinelEECycleRate, Vu1IdentityIsInvariant)
{
	const s8 saved = EmuConfig.Speedhacks.EECycleRate;

	const XXH128_hash_t at0  = SentinelAtEffectiveRate(1, 0);
	const XXH128_hash_t atm1 = SentinelAtEffectiveRate(1, -1);
	const XXH128_hash_t atm2 = SentinelAtEffectiveRate(1, -2);
	const XXH128_hash_t atp3 = SentinelAtEffectiveRate(1, 3);

	EmuConfig.Speedhacks.EECycleRate = saved;
	EECycleRate::SyncToConfigured();

	EXPECT_TRUE(SameHash(at0, atm1)) << "VU1 program identity moved with the EE cycle rate";
	EXPECT_TRUE(SameHash(at0, atm2));
	EXPECT_TRUE(SameHash(at0, atp3));
}
