// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The on-disk format every GS shader and pipeline cache uses (GS/GSCacheFile.h): a stamp that does
// not match discards a file whole, a damaged entry is dropped and never returned, and a torn end is
// cut off with everything before it kept. Packs are held in RAM and written by a background writer.

#include "GS/GSCacheFile.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <csignal>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace GSCacheFile;

namespace
{
	class GSCacheFileTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
			m_dir = (std::filesystem::temp_directory_path() /
					 (std::string("gs_cache_file_tests_") + info->name() + "_" + std::to_string(getpid())))
						.string();
			std::filesystem::remove_all(m_dir);
			std::filesystem::create_directories(m_dir);
		}

		void TearDown() override { std::filesystem::remove_all(m_dir); }

		std::string PathOf(const char* name) const { return Path::Combine(m_dir, name); }

		static Stamp MakeStamp(const char* build)
		{
			Stamp s;
			s.Add("build", build);
			s.Add("driver", "test driver 1.0");
			return s;
		}

		static std::vector<u8> Bytes(u32 seed, size_t size)
		{
			std::vector<u8> v(size);
			for (size_t i = 0; i < size; i++)
				v[i] = static_cast<u8>(seed * 31 + i * 7);
			return v;
		}

		static std::vector<u8> Key(u32 n)
		{
			std::vector<u8> k(24, 0);
			std::memcpy(k.data(), &n, sizeof(n));
			return k;
		}

		static std::vector<u8> ReadAll(const std::string& path)
		{
			return FileSystem::ReadBinaryFile(path.c_str()).value_or(std::vector<u8>());
		}

		static void WriteAll(const std::string& path, const std::vector<u8>& data)
		{
			ASSERT_TRUE(FileSystem::WriteBinaryFile(path.c_str(), data.data(), data.size()));
		}

		std::string PackPath() const { return PathOf("test.pack"); }

		static constexpr u16 TYPE_A = 1;
		static constexpr u16 TYPE_B = 2;
		static constexpr u64 BIG = 1ull << 30;
		/// The pack layout, mirrored: a 56-byte file header, then records of a 24-byte header, the key
		/// and the payload.
		static constexpr size_t HEADER = 56;
		static constexpr size_t RECORD_HEADER = 24;
		static size_t RecordBytes(u32 i) { return RECORD_HEADER + 24 + 100 + i * 10; }

		static void Put(Pack& pack, u32 i)
		{
			const std::vector<u8> data = Bytes(i, 100 + i * 10);
			ASSERT_TRUE(pack.Put(TYPE_A, Key(i).data(), 24, data.data(), data.size(), i));
		}

		static void ExpectEntry(const Pack& pack, u32 i)
		{
			std::vector<u8> data;
			u32 aux = ~0u;
			ASSERT_TRUE(pack.Get(TYPE_A, Key(i).data(), 24, &data, &aux)) << "entry " << i;
			EXPECT_EQ(data, Bytes(i, 100 + i * 10));
			EXPECT_EQ(aux, i);
		}

		/// The keys of TYPE_A, in ForEach order.
		static std::vector<u32> Order(const Pack& pack)
		{
			std::vector<u32> order;
			pack.ForEach(TYPE_A, [&order](const u8* key, u32 key_size, const std::vector<u8>&, u32) {
				u32 n;
				std::memcpy(&n, key, sizeof(n));
				EXPECT_EQ(key_size, 24u);
				order.push_back(n);
			});
			return order;
		}

		std::string m_dir;
	};
} // namespace

TEST_F(GSCacheFileTest, StampTextSeparatesValues)
{
	// Length-prefixed: "ab"+"c" and "a"+"bc" must not make the same stamp.
	Stamp a, b;
	a.Add("x", "ab").Add("y", "c");
	b.Add("x", "a").Add("y", "bc");
	EXPECT_NE(a.GetDigest(), b.GetDigest());
	EXPECT_EQ(MakeStamp("A").GetDigest(), MakeStamp("A").GetDigest());
	EXPECT_NE(MakeStamp("A").GetDigest(), MakeStamp("B").GetDigest());
}

TEST_F(GSCacheFileTest, BuildIdIsSet)
{
	EXPECT_FALSE(GetBuildId().empty());
	EXPECT_EQ(GetBuildVersionWord(), GetBuildVersionWord());
}

TEST_F(GSCacheFileTest, FramedRoundTrip)
{
	const std::string path = PathOf("framed.bin");
	const std::vector<u8> payload = Bytes(1, 5000);
	ASSERT_TRUE(WriteFramedFile(path, KIND_TEST, MakeStamp("A"), payload.data(), payload.size()));

	std::vector<u8> out;
	ASSERT_EQ(ReadFramedFile(path, KIND_TEST, MakeStamp("A"), &out), ReadResult::Ok);
	EXPECT_EQ(out, payload);
}

TEST_F(GSCacheFileTest, FramedStampMismatchDiscards)
{
	const std::string path = PathOf("framed.bin");
	const std::vector<u8> payload = Bytes(1, 5000);
	ASSERT_TRUE(WriteFramedFile(path, KIND_TEST, MakeStamp("A"), payload.data(), payload.size()));

	std::vector<u8> out;
	EXPECT_EQ(ReadFramedFile(path, KIND_TEST, MakeStamp("B"), &out), ReadResult::StampMismatch);
	EXPECT_TRUE(out.empty());
	// Another kind of cache written under the same stamp is not this one.
	EXPECT_EQ(ReadFramedFile(path, KIND_VK_PIPELINES, MakeStamp("A"), &out), ReadResult::BadHeader);
	EXPECT_EQ(ReadFramedFile(PathOf("absent.bin"), KIND_TEST, MakeStamp("A"), &out), ReadResult::Missing);
}

TEST_F(GSCacheFileTest, FramedDamageIsNeverReturned)
{
	const std::string path = PathOf("framed.bin");
	const std::vector<u8> payload = Bytes(1, 5000);
	ASSERT_TRUE(WriteFramedFile(path, KIND_TEST, MakeStamp("A"), payload.data(), payload.size()));
	const std::vector<u8> good = ReadAll(path);
	std::vector<u8> out;

	// One flipped bit in the payload.
	std::vector<u8> bad = good;
	bad[good.size() - 100] ^= 0x10;
	WriteAll(path, bad);
	EXPECT_EQ(ReadFramedFile(path, KIND_TEST, MakeStamp("A"), &out), ReadResult::ChecksumMismatch);

	// A write cut short, at every length from nothing up to one byte short.
	for (size_t len : {size_t(0), size_t(10), size_t(55), size_t(56), good.size() / 2, good.size() - 1})
	{
		WriteAll(path, std::vector<u8>(good.begin(), good.begin() + len));
		const ReadResult res = ReadFramedFile(path, KIND_TEST, MakeStamp("A"), &out);
		EXPECT_TRUE(res == ReadResult::Truncated || res == ReadResult::Missing) << len;
	}

	// A flipped bit in the header.
	bad = good;
	bad[20] ^= 0x01;
	WriteAll(path, bad);
	EXPECT_EQ(ReadFramedFile(path, KIND_TEST, MakeStamp("A"), &out), ReadResult::BadHeader);
}

TEST_F(GSCacheFileTest, FramedRejectsOldPipelineCacheLayout)
{
	// What a build from before this format left behind: a raw Vulkan pipeline cache blob.
	std::vector<u8> old(64, 0);
	const u32 words[] = {32, 1, 0x5143, 0x1234};
	std::memcpy(old.data(), words, sizeof(words));
	WriteAll(PathOf("vulkan_pipelines.bin"), old);
	std::vector<u8> out;
	EXPECT_EQ(ReadFramedFile(PathOf("vulkan_pipelines.bin"), KIND_VK_PIPELINES, MakeStamp("A"), &out),
		ReadResult::BadHeader);
}

TEST_F(GSCacheFileTest, FramedWriteAbandonedAtTheDeadlineLeavesTheOldFile)
{
	const std::string path = PathOf("framed.bin");
	const std::vector<u8> payload = Bytes(1, 5000);
	ASSERT_TRUE(WriteFramedFile(path, KIND_TEST, MakeStamp("A"), payload.data(), payload.size()));
	const std::vector<u8> other = Bytes(2, 5000);
	EXPECT_FALSE(WriteFramedFile(path, KIND_TEST, MakeStamp("A"), other.data(), other.size(), Clock::now() - std::chrono::seconds(1)));

	std::vector<u8> out;
	ASSERT_EQ(ReadFramedFile(path, KIND_TEST, MakeStamp("A"), &out), ReadResult::Ok);
	EXPECT_EQ(out, payload);
	EXPECT_EQ(CleanStaleTempFiles(m_dir), 0u) << "the temporary file was left behind";
	for (const auto& entry : std::filesystem::directory_iterator(m_dir))
		EXPECT_EQ(entry.path().filename().string(), "framed.bin");
}

TEST_F(GSCacheFileTest, PackRoundTripInFirstStoredOrder)
{
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		EXPECT_EQ(pack.GetLoadInfo().result, ReadResult::Missing);
		for (const u32 i : {3u, 1u, 4u, 0u, 2u})
			Put(pack, i);
		const u32 other = 7;
		ASSERT_TRUE(pack.Put(TYPE_B, nullptr, 0, &other, sizeof(other)));
		const Pack::WriteResult res = pack.Write();
		EXPECT_FALSE(res.failed);
		EXPECT_EQ(res.records, 6u);
		EXPECT_FALSE(pack.HasPendingWrites());
		// A second write has nothing to add.
		EXPECT_EQ(pack.Write().records, 0u);
	}

	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(pack.GetLoadInfo().result, ReadResult::Ok);
	EXPECT_EQ(pack.GetLoadInfo().records, 6u);
	EXPECT_EQ(pack.GetEntryCount(), 6u);
	EXPECT_FALSE(pack.HasPendingWrites());
	for (u32 i = 0; i < 5; i++)
		ExpectEntry(pack, i);
	std::vector<u8> value;
	ASSERT_TRUE(pack.Get(TYPE_B, nullptr, 0, &value));
	EXPECT_EQ(value.size(), sizeof(u32));
	EXPECT_EQ(Order(pack), (std::vector<u32>{3, 1, 4, 0, 2}));
}

TEST_F(GSCacheFileTest, PackNewestRecordWinsAndErasesStick)
{
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		for (u32 i = 0; i < 3; i++)
			Put(pack, i);
		ASSERT_FALSE(pack.Write().failed);

		// The same payload and aux change nothing, and without replace nothing is replaced.
		const std::vector<u8> same = Bytes(1, 110);
		EXPECT_FALSE(pack.Put(TYPE_A, Key(1).data(), 24, same.data(), same.size(), 1));
		const std::vector<u8> other = Bytes(50, 77);
		EXPECT_FALSE(pack.Put(TYPE_A, Key(1).data(), 24, other.data(), other.size(), 9, false));
		EXPECT_FALSE(pack.HasPendingWrites());

		EXPECT_TRUE(pack.Put(TYPE_A, Key(1).data(), 24, other.data(), other.size(), 9));
		pack.Erase(TYPE_A, Key(2).data(), 24);
		EXPECT_EQ(pack.GetEntryCount(), 2u);
		ASSERT_FALSE(pack.Write().failed);
	}

	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(pack.GetEntryCount(), 2u);
	ExpectEntry(pack, 0);
	std::vector<u8> data;
	u32 aux = 0;
	ASSERT_TRUE(pack.Get(TYPE_A, Key(1).data(), 24, &data, &aux));
	EXPECT_EQ(data, Bytes(50, 77));
	EXPECT_EQ(aux, 9u);
	EXPECT_FALSE(pack.Get(TYPE_A, Key(2).data(), 24, &data));
	// A replaced key keeps its place.
	EXPECT_EQ(Order(pack), (std::vector<u32>{0, 1}));
	// The same key under another type is another entry.
	EXPECT_FALSE(pack.Get(TYPE_B, Key(0).data(), 24, &data));
}

TEST_F(GSCacheFileTest, PackOfAnotherStampOrKindStartsEmptyAndIsReplaced)
{
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		Put(pack, 0);
		ASSERT_FALSE(pack.Write().failed);
	}
	{
		Pack pack;
		pack.Open(PackPath(), KIND_VK_SPIRV, MakeStamp("A"), BIG);
		EXPECT_EQ(pack.GetLoadInfo().result, ReadResult::BadHeader);
		EXPECT_EQ(pack.GetEntryCount(), 0u);
	}
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("B"), BIG);
		EXPECT_EQ(pack.GetLoadInfo().result, ReadResult::StampMismatch);
		EXPECT_EQ(pack.GetEntryCount(), 0u);
		Put(pack, 1);
		const Pack::WriteResult res = pack.Write();
		EXPECT_TRUE(res.rewrote);
	}
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("B"), BIG);
	EXPECT_EQ(pack.GetLoadInfo().result, ReadResult::Ok);
	EXPECT_EQ(pack.GetEntryCount(), 1u);
	ExpectEntry(pack, 1);
}

TEST_F(GSCacheFileTest, PackBrokenRecordEndsTheLogAndIsCutOff)
{
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		for (u32 i = 0; i < 4; i++)
			Put(pack, i);
		ASSERT_FALSE(pack.Write().failed);
	}
	// One flipped bit in the third record's payload: it and everything after it go.
	std::vector<u8> file = ReadAll(PackPath());
	const size_t third = HEADER + RecordBytes(0) + RecordBytes(1);
	file[third + RECORD_HEADER + 30] ^= 0x04;
	WriteAll(PackPath(), file);
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		EXPECT_EQ(pack.GetEntryCount(), 2u);
		EXPECT_EQ(pack.GetLoadInfo().torn_bytes, RecordBytes(2) + RecordBytes(3));
		ExpectEntry(pack, 0);
		ExpectEntry(pack, 1);
		EXPECT_TRUE(pack.HasPendingWrites());
		Put(pack, 5);
		ASSERT_FALSE(pack.Write().failed);
	}
	EXPECT_EQ(ReadAll(PackPath()).size(), HEADER + RecordBytes(0) + RecordBytes(1) + RecordBytes(5));
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(pack.GetLoadInfo().torn_bytes, 0u);
	EXPECT_EQ(pack.GetEntryCount(), 3u);
	ExpectEntry(pack, 5);
}

TEST_F(GSCacheFileTest, PackTornTailIsCutOffAndTheRestKept)
{
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		for (u32 i = 0; i < 4; i++)
			Put(pack, i);
		ASSERT_FALSE(pack.Write().failed);
	}
	// A record cut short: its header and half its payload.
	std::vector<u8> file = ReadAll(PackPath());
	const std::vector<u8> extra(file.end() - RecordBytes(3), file.end() - RecordBytes(3) / 2);
	file.insert(file.end(), extra.begin(), extra.end());
	WriteAll(PackPath(), file);
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		EXPECT_EQ(pack.GetLoadInfo().torn_bytes, extra.size());
		EXPECT_EQ(pack.GetEntryCount(), 4u);
		// Cut even when nothing else changed.
		ASSERT_FALSE(pack.Write().failed);
	}
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(pack.GetLoadInfo().torn_bytes, 0u);
	for (u32 i = 0; i < 4; i++)
		ExpectEntry(pack, i);
}

TEST_F(GSCacheFileTest, PackIsWrittenWholeWhenMostlyGarbage)
{
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	const std::vector<u8> key = Key(0);
	bool rewrote = false;
	u32 writes = 0;
	for (u32 i = 0; i < 10 && !rewrote; i++)
	{
		const std::vector<u8> data = Bytes(i, 300 * 1024);
		ASSERT_TRUE(pack.Put(TYPE_A, key.data(), 24, data.data(), data.size()));
		// Not in a pass with a deadline (a close): there only the appends must land.
		EXPECT_FALSE(pack.Write(Clock::now() + std::chrono::hours(1)).rewrote);
		pack.Put(TYPE_A, key.data(), 24, Bytes(i + 100, 300 * 1024).data(), 300 * 1024);
		rewrote = pack.Write().rewrote;
		writes++;
	}
	EXPECT_TRUE(rewrote);
	EXPECT_LE(writes, 3u);
	EXPECT_EQ(ReadAll(PackPath()).size(), HEADER + RECORD_HEADER + 24 + 300 * 1024);
	EXPECT_EQ(pack.GetFileBytes(), ReadAll(PackPath()).size());

	Pack again;
	again.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	std::vector<u8> data;
	ASSERT_TRUE(again.Get(TYPE_A, key.data(), 24, &data));
	EXPECT_EQ(data, Bytes(writes - 1 + 100, 300 * 1024));
}

TEST_F(GSCacheFileTest, PackPastTheRamCapKeepsAPrefixAndIsOnlyAppendedTo)
{
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		for (u32 i = 0; i < 10; i++)
			Put(pack, i);
		ASSERT_FALSE(pack.Write().failed);
		// A newer record of the first key, at the end of the log.
		const std::vector<u8> newer = Bytes(99, 100);
		pack.Put(TYPE_A, Key(0).data(), 24, newer.data(), newer.size(), 0);
		ASSERT_FALSE(pack.Write().failed);
	}
	const size_t full_size = ReadAll(PackPath()).size();
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), 500);
		EXPECT_TRUE(pack.GetLoadInfo().ram_cap_hit);
		EXPECT_LT(pack.GetEntryCount(), 10u);
		EXPECT_GT(pack.GetEntryCount(), 0u);
		EXPECT_LE(pack.GetRamBytes(), 500u);
		ExpectEntry(pack, 1);
		// The newest record of key 0 is past the cap, so the older one must not be served.
		std::vector<u8> data;
		EXPECT_FALSE(pack.Get(TYPE_A, Key(0).data(), 24, &data));
		Put(pack, 20);
		const Pack::WriteResult res = pack.Write();
		EXPECT_FALSE(res.rewrote);
		EXPECT_EQ(res.records, 1u);
	}
	EXPECT_EQ(ReadAll(PackPath()).size(), full_size + RecordBytes(20));
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(pack.GetEntryCount(), 11u);
	ExpectEntry(pack, 20);
	std::vector<u8> data;
	ASSERT_TRUE(pack.Get(TYPE_A, Key(0).data(), 24, &data));
	EXPECT_EQ(data, Bytes(99, 100));
}

TEST_F(GSCacheFileTest, PackWriteStopsAtTheDeadlineAndKeepsTheRestPending)
{
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	for (u32 i = 0; i < 3; i++)
		Put(pack, i);
	const Pack::WriteResult late = pack.Write(Clock::now() - std::chrono::seconds(1));
	EXPECT_TRUE(late.deadline_hit);
	EXPECT_FALSE(late.failed);
	EXPECT_EQ(late.records, 0u);
	EXPECT_TRUE(pack.HasPendingWrites());
	EXPECT_EQ(pack.Write().records, 3u);
	EXPECT_FALSE(pack.HasPendingWrites());
}

TEST_F(GSCacheFileTest, PackClearLeavesAnEmptyPack)
{
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	for (u32 i = 0; i < 3; i++)
		Put(pack, i);
	ASSERT_FALSE(pack.Write().failed);
	pack.Clear();
	EXPECT_EQ(pack.GetEntryCount(), 0u);
	EXPECT_TRUE(pack.HasPendingWrites());
	EXPECT_TRUE(pack.Write().rewrote);
	EXPECT_FALSE(pack.HasPendingWrites());
	EXPECT_EQ(ReadAll(PackPath()).size(), HEADER);
	Put(pack, 4);
	ASSERT_FALSE(pack.Write().failed);

	Pack again;
	again.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(again.GetEntryCount(), 1u);
	ExpectEntry(again, 4);
}

TEST_F(GSCacheFileTest, PackDeletedFromOutsideIsWrittenWhole)
{
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	for (u32 i = 0; i < 3; i++)
		Put(pack, i);
	ASSERT_FALSE(pack.Write().failed);
	ASSERT_TRUE(FileSystem::DeleteFilePath(PackPath().c_str()));
	Put(pack, 3);
	EXPECT_TRUE(pack.Write().rewrote);

	Pack again;
	again.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(again.GetEntryCount(), 4u);
}

#ifndef _WIN32
TEST_F(GSCacheFileTest, PackWritePassIsSkippedWhileAnotherProcessWrites)
{
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	Put(pack, 0);
	ASSERT_FALSE(pack.Write().failed);

	// A second open of the file stands in for a second process: the lock is per open file.
	std::FILE* other = std::fopen(PackPath().c_str(), "r+b");
	ASSERT_NE(other, nullptr);
	ASSERT_EQ(flock(fileno(other), LOCK_EX | LOCK_NB), 0);
	Put(pack, 1);
	const Pack::WriteResult res = pack.Write();
	EXPECT_TRUE(res.contended);
	EXPECT_FALSE(res.failed);
	EXPECT_TRUE(pack.HasPendingWrites());
	std::fclose(other);
	EXPECT_EQ(pack.Write().records, 1u);
}
#endif

TEST_F(GSCacheFileTest, WriterWritesWhenAskedAndOnStop)
{
	auto pack = std::make_shared<Pack>();
	pack->Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	u32 extra_passes = 0;
	bool extra_asked = false;
	PackWriter writer;
	writer.Start(m_dir, [&](Clock::time_point, bool asked) {
		extra_passes++;
		extra_asked = asked;
	}, std::chrono::hours(1));
	EXPECT_TRUE(writer.IsRunning());
	writer.Add(pack);

	Put(*pack, 0);
	writer.Flush(true);
	EXPECT_FALSE(pack->HasPendingWrites());
	EXPECT_EQ(extra_passes, 1u);
	EXPECT_TRUE(extra_asked);
	{
		Pack reader;
		reader.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		EXPECT_EQ(reader.GetEntryCount(), 1u);
	}

	// A released pack is written once more, then let go.
	Put(*pack, 1);
	writer.Release(pack);
	std::weak_ptr<Pack> weak = pack;
	pack.reset();
	writer.Flush(true);
	EXPECT_TRUE(weak.expired());
	{
		Pack reader;
		reader.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		EXPECT_EQ(reader.GetEntryCount(), 2u);
	}

	auto second = std::make_shared<Pack>();
	second->Open(PathOf("second.pack"), KIND_TEST, MakeStamp("A"), BIG);
	writer.Add(second);
	Put(*second, 2);
	writer.Stop(Clock::now() + PackWriter::CLOSE_DEADLINE);
	EXPECT_FALSE(writer.IsRunning());
	EXPECT_FALSE(second->HasPendingWrites());
	// Not running: a flush returns at once.
	writer.Flush(true);
}

TEST_F(GSCacheFileTest, PackDirectoriesAreNamedForTheGame)
{
	EXPECT_EQ(GetPackDirectory("/c", "", 0), Path::Combine(Path::Combine("/c", "gs_packs"), "_"));
	EXPECT_EQ(GetPackDirectory("/c", "SLUS-20946", 0x1A2B3C4D), Path::Combine(Path::Combine("/c", "gs_packs"), "SLUS-20946_1A2B3C4D"));
	EXPECT_GE(GetDefaultPackRamCap(), 32ull * 1024 * 1024);
}

TEST_F(GSCacheFileTest, DeleteAllTakesEveryCacheAndNothingElse)
{
	for (const char* name : {"vulkan_shaders.idx", "vulkan_shaders.bin", "vulkan_shaders_debug.idx",
			 "vulkan_pipelines.bin", "vulkan_pipelines.bin.tmp1234", "gl_programs.idx", "gl_programs.bin",
			 "d3d_shaders_sm50.idx", "d3d12_pipelines_sm60.bin", "lsfg_spirv.cache", "gamelist.cache", "keep.txt"})
	{
		WriteAll(PathOf(name), Bytes(1, 8));
	}
	std::filesystem::create_directories(PathOf("vulkan_pipeline_keys"));
	WriteAll(PathOf("vulkan_pipeline_keys/SLUS-12345_ABCDEF01_00112233.bin"), Bytes(1, 8));
	std::filesystem::create_directories(PathOf("achievement_images"));
	WriteAll(PathOf("achievement_images/1.png"), Bytes(1, 8));
	std::filesystem::create_directories(PathOf("gs_packs/SLUS-12345_ABCDEF01"));
	WriteAll(PathOf("gs_packs/SLUS-12345_ABCDEF01/vk_spirv_11111111.pack"), Bytes(1, 8));
	WriteAll(PathOf("gs_packs/SLUS-12345_ABCDEF01/gl_22222222.pack.tmp99"), Bytes(1, 8));

	// A pack held open, as by a running game, is emptied in place, and what it adds afterwards lands
	// in the file a new start reads.
	std::filesystem::create_directories(PathOf("gs_packs/_"));
	Pack live;
	live.Open(PathOf("gs_packs/_/vk_spirv_33333333.pack"), KIND_TEST, MakeStamp("A"), BIG);
	for (u32 i = 0; i < 3; i++)
		Put(live, i);
	ASSERT_FALSE(live.Write().failed);

	EXPECT_EQ(DeleteAll(m_dir), 14u);
	EXPECT_TRUE(FileSystem::FileExists(PathOf("gamelist.cache").c_str()));
	EXPECT_TRUE(FileSystem::FileExists(PathOf("keep.txt").c_str()));
	EXPECT_TRUE(FileSystem::FileExists(PathOf("achievement_images/1.png").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(PathOf("vulkan_pipeline_keys").c_str()));
	EXPECT_FALSE(FileSystem::FileExists(PathOf("vulkan_pipelines.bin.tmp1234").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(PathOf("gs_packs/SLUS-12345_ABCDEF01").c_str()));
	EXPECT_EQ(live.GetEntryCount(), 0u);
	EXPECT_EQ(ReadAll(live.GetPath()).size(), HEADER);

	Put(live, 4);
	ASSERT_FALSE(live.Write().failed);
	live.Close();
	Pack next;
	next.Open(PathOf("gs_packs/_/vk_spirv_33333333.pack"), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(next.GetEntryCount(), 1u);
	ExpectEntry(next, 4);
}

TEST_F(GSCacheFileTest, LeftoversOfOlderCachesAndUnusedPacksAreRemoved)
{
	const auto age = [](const std::string& path, std::chrono::hours hours) {
		std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - hours);
	};
	const std::chrono::hours day(24);

	WriteAll(PathOf("vulkan_shaders_11111111.idx"), Bytes(1, 8));
	age(PathOf("vulkan_shaders_11111111.idx"), 8 * day);
	// A week has not passed: another build sharing the data root may still use it.
	WriteAll(PathOf("gl_programs_22222222.bin"), Bytes(1, 8));
	age(PathOf("gl_programs_22222222.bin"), 1 * day);
	WriteAll(PathOf("vulkan_pipelines_33333333.bin"), Bytes(1, 8));
	age(PathOf("vulkan_pipelines_33333333.bin"), 90 * day);
	std::filesystem::create_directories(PathOf("vulkan_pipeline_keys"));
	WriteAll(PathOf("vulkan_pipeline_keys/a.bin"), Bytes(1, 8));
	age(PathOf("vulkan_pipeline_keys/a.bin"), 8 * day);

	std::filesystem::create_directories(PathOf("gs_packs/T"));
	std::filesystem::create_directories(PathOf("gs_packs/U"));
	WriteAll(PathOf("gs_packs/T/old.pack"), Bytes(1, 8));
	age(PathOf("gs_packs/T/old.pack"), 61 * day);
	WriteAll(PathOf("gs_packs/T/new.pack"), Bytes(1, 8));
	age(PathOf("gs_packs/T/new.pack"), 59 * day);
	Pack open;
	open.Open(PathOf("gs_packs/U/open.pack"), KIND_TEST, MakeStamp("A"), BIG);
	Put(open, 0);
	ASSERT_FALSE(open.Write().failed);
	age(PathOf("gs_packs/U/open.pack"), 61 * day);

	EXPECT_EQ(CleanLeftovers(m_dir), 3u);
	for (const char* gone : {"vulkan_shaders_11111111.idx", "vulkan_pipeline_keys/a.bin", "gs_packs/T/old.pack"})
		EXPECT_FALSE(FileSystem::FileExists(PathOf(gone).c_str())) << gone;
	for (const char* kept : {"gl_programs_22222222.bin", "vulkan_pipelines_33333333.bin", "gs_packs/T/new.pack", "gs_packs/U/open.pack"})
		EXPECT_TRUE(FileSystem::FileExists(PathOf(kept).c_str())) << kept;
	EXPECT_FALSE(FileSystem::DirectoryExists(PathOf("vulkan_pipeline_keys").c_str()));

	// Opening a pack marks it used.
	open.Close();
	open.Open(PathOf("gs_packs/U/open.pack"), KIND_TEST, MakeStamp("A"), BIG);
	open.Close();
	EXPECT_EQ(CleanLeftovers(m_dir), 0u);
	EXPECT_TRUE(FileSystem::FileExists(PathOf("gs_packs/U/open.pack").c_str()));
}

#ifndef _WIN32
// A full disk, simulated with a file size limit: the append fails part way, and the pack must stop
// writing and leave a file the next start reads up to the failure. The limit is set in this process,
// soft limit only, and put back on every return. Not in a forked child: fork() returns -1 on the CI
// runners.
namespace
{
	class FileSizeLimit
	{
	public:
		explicit FileSizeLimit(rlim_t limit)
		{
			m_old_handler = std::signal(SIGXFSZ, SIG_IGN);
			if (getrlimit(RLIMIT_FSIZE, &m_old) != 0)
				return;
			const struct rlimit rl = {limit, m_old.rlim_max};
			m_set = (setrlimit(RLIMIT_FSIZE, &rl) == 0);
		}
		~FileSizeLimit()
		{
			if (m_set)
				setrlimit(RLIMIT_FSIZE, &m_old);
			std::signal(SIGXFSZ, m_old_handler);
		}
		bool IsSet() const { return m_set; }

	private:
		struct rlimit m_old = {};
		void (*m_old_handler)(int) = SIG_DFL;
		bool m_set = false;
	};
} // namespace

TEST_F(GSCacheFileTest, FailedWriteStopsWritingAndLeavesAReadablePack)
{
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		for (u32 i = 0; i < 4; i++)
			Put(pack, i);
		ASSERT_FALSE(pack.Write().failed);
	}
	{
		Pack pack;
		pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
		const std::vector<u8> big = Bytes(9, 4 * 1024 * 1024);
		pack.Put(TYPE_A, Key(9).data(), 24, big.data(), big.size());
		Pack::WriteResult res;
		{
			FileSizeLimit fsize(static_cast<rlim_t>(ReadAll(PackPath()).size() + 100));
			ASSERT_TRUE(fsize.IsSet());
			res = pack.Write();
		}
		EXPECT_TRUE(res.failed);
		// Nothing more this session.
		EXPECT_FALSE(pack.HasPendingWrites());
		Put(pack, 5);
		EXPECT_EQ(pack.Write().records, 0u);
		ExpectEntry(pack, 5);
	}
	Pack pack;
	pack.Open(PackPath(), KIND_TEST, MakeStamp("A"), BIG);
	EXPECT_EQ(pack.GetEntryCount(), 4u);
	for (u32 i = 0; i < 4; i++)
		ExpectEntry(pack, i);
}

TEST_F(GSCacheFileTest, StaleTempFilesAreRemoved)

{
	// A pid far above any this machine hands out is not running.
	WriteAll(PathOf("vulkan_pipelines_aa.bin.tmp2147480000"), Bytes(1, 8));
	const std::string mine = PathOf(("gl_programs.bin.tmp" + std::to_string(getpid())).c_str());
	WriteAll(mine, Bytes(1, 8));
	WriteAll(PathOf("notes.tmpfile"), Bytes(1, 8));
	EXPECT_EQ(CleanStaleTempFiles(m_dir), 1u);
	EXPECT_TRUE(FileSystem::FileExists(mine.c_str()));
	EXPECT_TRUE(FileSystem::FileExists(PathOf("notes.tmpfile").c_str()));
}
#endif

TEST_F(GSCacheFileTest, OtherIdentitiesArePrunedOldestFirst)
{
	// Five builds' SPIR-V stores and the unversioned name of the old format, oldest first.
	const char* stems[] = {"vulkan_shaders", "vulkan_shaders_11111111", "vulkan_shaders_22222222",
		"vulkan_shaders_33333333", "vulkan_shaders_44444444", "vulkan_shaders_55555555"};
	s64 t = 1000000000;
	for (const char* stem : stems)
	{
		for (const char* ext : {".idx", ".bin"})
		{
			const std::string p = PathOf((std::string(stem) + ext).c_str());
			WriteAll(p, Bytes(1, 8));
			std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now() -
													 std::chrono::seconds(2000000000 - t));
		}
		t += 1000;
	}
	WriteAll(PathOf("vulkan_pipelines_11111111.bin"), Bytes(1, 8));

	// The current identity is the oldest one; it stays, with the two most recent others.
	EXPECT_EQ(PruneOtherIdentities(m_dir, "vulkan_shaders", "vulkan_shaders_11111111", 3), 6u);
	for (const char* keep : {"vulkan_shaders_11111111.idx", "vulkan_shaders_44444444.bin", "vulkan_shaders_55555555.idx",
			 "vulkan_pipelines_11111111.bin"})
		EXPECT_TRUE(FileSystem::FileExists(PathOf(keep).c_str())) << keep;
	for (const char* gone : {"vulkan_shaders.idx", "vulkan_shaders_22222222.bin", "vulkan_shaders_33333333.idx"})
		EXPECT_FALSE(FileSystem::FileExists(PathOf(gone).c_str())) << gone;
}
