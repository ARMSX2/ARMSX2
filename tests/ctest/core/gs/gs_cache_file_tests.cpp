// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The on-disk format every GS shader and pipeline cache uses (GS/GSCacheFile.h): a stamp that does
// not match discards a file whole, a damaged entry is dropped and never returned, and a torn end is
// cut off with everything before it kept.

#include "GS/GSCacheFile.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
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

		static void FillStore(BlobStore& store, u32 count)
		{
			for (u32 i = 0; i < count; i++)
			{
				const std::vector<u8> data = Bytes(i, 100 + i * 10);
				ASSERT_TRUE(store.Insert(Key(i).data(), data.data(), data.size(), i));
			}
		}

		static void ExpectEntry(BlobStore& store, u32 i)
		{
			std::vector<u8> data;
			u32 aux = ~0u;
			ASSERT_TRUE(store.Lookup(Key(i).data(), &data, &aux)) << "entry " << i;
			EXPECT_EQ(data, Bytes(i, 100 + i * 10));
			EXPECT_EQ(aux, i);
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

TEST_F(GSCacheFileTest, BlobStoreRoundTrip)
{
	const std::string base = PathOf("store");
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		EXPECT_EQ(store.GetEntryCount(), 0u);
		FillStore(store, 20);
		for (u32 i = 0; i < 20; i++)
			ExpectEntry(store, i);
	}
	BlobStore store;
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_EQ(store.GetOpenInfo().discarded, ReadResult::Ok);
	EXPECT_EQ(store.GetEntryCount(), 20u);
	for (u32 i = 0; i < 20; i++)
		ExpectEntry(store, i);
	std::vector<u8> data;
	EXPECT_FALSE(store.Lookup(Key(99).data(), &data));
}

TEST_F(GSCacheFileTest, BlobStoreStampMismatchDiscardsWholeStore)
{
	const std::string base = PathOf("store");
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		FillStore(store, 10);
	}
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("B"), 24));
		EXPECT_EQ(store.GetOpenInfo().discarded, ReadResult::StampMismatch);
		EXPECT_EQ(store.GetEntryCount(), 0u);
		std::vector<u8> data;
		EXPECT_FALSE(store.Lookup(Key(3).data(), &data));
		FillStore(store, 2);
	}
	// And the files now belong to B: going back to A discards again.
	BlobStore store;
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_EQ(store.GetOpenInfo().discarded, ReadResult::StampMismatch);
	EXPECT_EQ(store.GetEntryCount(), 0u);
	// A different key layout is a different store.
	store.Close();
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 28));
	EXPECT_EQ(store.GetOpenInfo().discarded, ReadResult::BadHeader);
}

TEST_F(GSCacheFileTest, BlobStoreDamagedDataIsDroppedNotReturned)
{
	const std::string base = PathOf("store");
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		FillStore(store, 5);
	}
	// Entry 2's data starts after 100+110 bytes; flip a byte inside it.
	std::vector<u8> bin = ReadAll(base + ".bin");
	bin[100 + 110 + 5] ^= 0xFF;
	WriteAll(base + ".bin", bin);

	BlobStore store;
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_EQ(store.GetEntryCount(), 5u);
	std::vector<u8> data = {1, 2, 3};
	EXPECT_FALSE(store.Lookup(Key(2).data(), &data));
	EXPECT_TRUE(data.empty());
	EXPECT_EQ(store.GetBadDataCount(), 1u);
	EXPECT_EQ(store.GetEntryCount(), 4u);
	for (u32 i : {0u, 1u, 3u, 4u})
		ExpectEntry(store, i);

	// Rebuilt and added again, the new copy wins on the next open.
	const std::vector<u8> fresh = Bytes(2, 100 + 2 * 10);
	ASSERT_TRUE(store.Insert(Key(2).data(), fresh.data(), fresh.size(), 2));
	store.Close();
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
	for (u32 i = 0; i < 5; i++)
		ExpectEntry(store, i);
}

TEST_F(GSCacheFileTest, BlobStoreTornIndexTailIsCutAndTheRestKept)
{
	const std::string base = PathOf("store");
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		FillStore(store, 6);
	}
	const std::vector<u8> idx = ReadAll(base + ".idx");
	const size_t entry_size = 24 + 32;

	// Half an entry appended: the app died mid-write.
	std::vector<u8> torn = idx;
	torn.insert(torn.end(), idx.end() - entry_size, idx.end() - entry_size / 2);
	WriteAll(base + ".idx", torn);
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		EXPECT_EQ(store.GetOpenInfo().truncated_bytes, entry_size / 2);
		EXPECT_EQ(store.GetEntryCount(), 6u);
		for (u32 i = 0; i < 6; i++)
			ExpectEntry(store, i);
		// Cut on disk, so appending continues from a clean end.
		EXPECT_EQ(ReadAll(base + ".idx").size(), idx.size());
		const std::vector<u8> more = Bytes(50, 77);
		ASSERT_TRUE(store.Insert(Key(50).data(), more.data(), more.size(), 50));
	}
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		EXPECT_EQ(store.GetOpenInfo().truncated_bytes, 0u);
		EXPECT_EQ(store.GetEntryCount(), 7u);
	}

	// A whole entry of garbage (a torn write the file system filled with zeros) in the middle ends
	// the index there; the entries before it survive.
	std::vector<u8> zeroed = ReadAll(base + ".idx");
	const size_t header = zeroed.size() - 7 * entry_size;
	std::fill(zeroed.begin() + header + 3 * entry_size, zeroed.begin() + header + 4 * entry_size, 0);
	WriteAll(base + ".idx", zeroed);
	BlobStore store;
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_EQ(store.GetEntryCount(), 3u);
	EXPECT_EQ(store.GetOpenInfo().truncated_bytes, 4 * entry_size);
	for (u32 i = 0; i < 3; i++)
		ExpectEntry(store, i);
}

TEST_F(GSCacheFileTest, BlobStoreEntryPastTheDataEndsTheIndex)
{
	const std::string base = PathOf("store");
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		FillStore(store, 4);
	}
	// The index reached the disk and the last data did not.
	std::vector<u8> bin = ReadAll(base + ".bin");
	bin.resize(bin.size() - 20);
	WriteAll(base + ".bin", bin);

	BlobStore store;
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_EQ(store.GetEntryCount(), 3u);
	for (u32 i = 0; i < 3; i++)
		ExpectEntry(store, i);
}

TEST_F(GSCacheFileTest, BlobStoreReplacesOldFormatAndGarbage)
{
	const std::string base = PathOf("store");
	// The old layout: a small version number, then entries.
	std::vector<u8> old(4 + 40 + 5 * 32, 0);
	const u32 version = 125;
	std::memcpy(old.data(), &version, sizeof(version));
	WriteAll(base + ".idx", old);
	WriteAll(base + ".bin", Bytes(9, 4000));

	BlobStore store;
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_EQ(store.GetOpenInfo().discarded, ReadResult::BadHeader);
	EXPECT_EQ(store.GetEntryCount(), 0u);
	EXPECT_EQ(ReadAll(base + ".bin").size(), 0u);
	FillStore(store, 3);
	for (u32 i = 0; i < 3; i++)
		ExpectEntry(store, i);
}

TEST_F(GSCacheFileTest, BlobStoreOverSizeLimitStartsOver)
{
	const std::string base = PathOf("store");
	{
		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		FillStore(store, 10);
	}
	BlobStore store;
	ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24, 500));
	EXPECT_EQ(store.GetEntryCount(), 0u);
}

#ifndef _WIN32
TEST_F(GSCacheFileTest, BlobStoreSecondProcessReadsButNeverWrites)
{
	// A second open of the same files stands in for a second process: the lock is per open file.
	const std::string base = PathOf("store");
	BlobStore owner;
	ASSERT_TRUE(owner.Open(base, KIND_TEST, MakeStamp("A"), 24));
	FillStore(owner, 4);
	EXPECT_TRUE(owner.IsWritable());

	BlobStore other;
	ASSERT_TRUE(other.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_TRUE(other.GetOpenInfo().read_only);
	EXPECT_FALSE(other.IsWritable());
	for (u32 i = 0; i < 4; i++)
		ExpectEntry(other, i);
	const std::vector<u8> data = Bytes(7, 10);
	EXPECT_FALSE(other.Insert(Key(7).data(), data.data(), data.size()));

	// A reader with another stamp does not wipe the owner's files; it runs without a cache.
	BlobStore stranger;
	EXPECT_FALSE(stranger.Open(base, KIND_TEST, MakeStamp("B"), 24));
	EXPECT_FALSE(stranger.IsOpen());
	for (u32 i = 0; i < 4; i++)
		ExpectEntry(owner, i);
}
#endif

TEST_F(GSCacheFileTest, RecordFileRoundTripAndDamage)
{
	static constexpr u32 RECORD_SIZE = 40;
	const Stamp stamp = MakeStamp("A");
	std::vector<u8> file = MakeRecordFileHeader(KIND_TEST, stamp, RECORD_SIZE, 17);
	for (u32 i = 0; i < 5; i++)
	{
		std::vector<u8> rec = Bytes(i, RECORD_SIZE);
		SealRecord(rec.data(), RECORD_SIZE);
		file.insert(file.end(), rec.begin(), rec.end());
	}

	u64 user = 0;
	u32 dropped = 0;
	std::vector<std::vector<u8>> records;
	ASSERT_EQ(ParseRecordFile(file, KIND_TEST, stamp, RECORD_SIZE, &user, &records, &dropped), ReadResult::Ok);
	EXPECT_EQ(user, 17u);
	EXPECT_EQ(records.size(), 5u);
	EXPECT_EQ(dropped, 0u);

	// A record damaged in place is dropped; the others are kept.
	std::vector<u8> bad = file;
	bad[GetRecordFileHeaderSize() + 2 * RECORD_SIZE + 3] ^= 0x40;
	ASSERT_EQ(ParseRecordFile(bad, KIND_TEST, stamp, RECORD_SIZE, &user, &records, &dropped), ReadResult::Ok);
	EXPECT_EQ(records.size(), 4u);
	EXPECT_EQ(dropped, 1u);

	// A torn record at the end is ignored.
	std::vector<u8> torn = file;
	torn.resize(torn.size() - RECORD_SIZE / 2);
	ASSERT_EQ(ParseRecordFile(torn, KIND_TEST, stamp, RECORD_SIZE, &user, &records, &dropped), ReadResult::Ok);
	EXPECT_EQ(records.size(), 4u);
}

TEST_F(GSCacheFileTest, RecordFileLayoutOrStampChangeDiscards)
{
	const Stamp stamp = MakeStamp("A");
	std::vector<u8> file = MakeRecordFileHeader(KIND_VK_PIPELINE_KEYS, stamp, 40, 3);
	std::vector<u8> rec = Bytes(1, 40);
	SealRecord(rec.data(), 40);
	file.insert(file.end(), rec.begin(), rec.end());

	u64 user = 0;
	std::vector<std::vector<u8>> records;
	// The record grew: an older list cannot be read as the new layout.
	EXPECT_EQ(ParseRecordFile(file, KIND_VK_PIPELINE_KEYS, stamp, 44, &user, &records), ReadResult::BadHeader);
	EXPECT_TRUE(records.empty());
	// Another build: same layout, and still discarded.
	EXPECT_EQ(ParseRecordFile(file, KIND_VK_PIPELINE_KEYS, MakeStamp("B"), 40, &user, &records),
		ReadResult::StampMismatch);
	EXPECT_TRUE(records.empty());
	// The format before this one: a different magic in the first word.
	std::vector<u8> old = file;
	const u32 old_magic = 0x59454B50;
	std::memcpy(old.data(), &old_magic, sizeof(old_magic));
	EXPECT_EQ(ParseRecordFile(old, KIND_VK_PIPELINE_KEYS, stamp, 40, &user, &records), ReadResult::BadHeader);
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

	EXPECT_EQ(DeleteAll(m_dir), 11u);
	EXPECT_TRUE(FileSystem::FileExists(PathOf("gamelist.cache").c_str()));
	EXPECT_TRUE(FileSystem::FileExists(PathOf("keep.txt").c_str()));
	EXPECT_TRUE(FileSystem::FileExists(PathOf("achievement_images/1.png").c_str()));
	EXPECT_FALSE(FileSystem::DirectoryExists(PathOf("vulkan_pipeline_keys").c_str()));
	EXPECT_FALSE(FileSystem::FileExists(PathOf("vulkan_pipelines.bin.tmp1234").c_str()));
}

TEST_F(GSCacheFileTest, RecreateKeepsTheLock)
{
	const std::string base = PathOf("store");
	BlobStore owner;
	ASSERT_TRUE(owner.Open(base, KIND_TEST, MakeStamp("A"), 24));
	FillStore(owner, 3);
	ASSERT_TRUE(owner.Recreate());
	EXPECT_EQ(owner.GetEntryCount(), 0u);
	EXPECT_TRUE(owner.IsWritable());
#ifndef _WIN32
	BlobStore other;
	ASSERT_TRUE(other.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_TRUE(other.GetOpenInfo().read_only);
#endif
	FillStore(owner, 2);
	owner.Close();
	BlobStore again;
	ASSERT_TRUE(again.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_EQ(again.GetEntryCount(), 2u);
}

TEST_F(GSCacheFileTest, ClearEmptiesLiveStoresInPlaceAndDeletesTheRest)
{
	// A renderer's store stays open through the clear, as it does with a running game or with the
	// fullscreen UI's device alive; the clear must not leave it appending to a deleted file.
	const std::string base = PathOf("vulkan_shaders_0123abcd");
	BlobStore live;
	ASSERT_TRUE(live.Open(base, KIND_TEST, MakeStamp("A"), 24));
	FillStore(live, 5);
	WriteAll(PathOf("vulkan_pipelines_0123abcd.bin"), Bytes(1, 64));
	WriteAll(PathOf("gl_programs_99999999.idx"), Bytes(1, 64));

	EXPECT_EQ(DeleteAll(m_dir), 4u);
	EXPECT_FALSE(FileSystem::FileExists(PathOf("vulkan_pipelines_0123abcd.bin").c_str()));
	EXPECT_FALSE(FileSystem::FileExists(PathOf("gl_programs_99999999.idx").c_str()));
	EXPECT_TRUE(FileSystem::FileExists((base + ".idx").c_str()));
	EXPECT_EQ(live.GetEntryCount(), 0u);
	EXPECT_TRUE(live.IsWritable());
	EXPECT_EQ(ReadAll(base + ".bin").size(), 0u);

	// What it adds afterwards lands in the files a new start reads.
	FillStore(live, 2);
	live.Close();
	BlobStore next;
	ASSERT_TRUE(next.Open(base, KIND_TEST, MakeStamp("A"), 24));
	EXPECT_EQ(next.GetEntryCount(), 2u);
	ExpectEntry(next, 0);
	ExpectEntry(next, 1);
}

#ifndef _WIN32
// A full disk, simulated with a file size limit in a child process: the append fails part way, and
// the store must cut both files back and stop writing, leaving nothing for the next start to trip on.
static int AppendUntilFull(const std::string& base, const Stamp& stamp, bool fail_in_index)
{
	BlobStore store;
	if (!store.Open(base, KIND_TEST, stamp, 24))
		return 10;
	const s64 idx_before = FileSystem::GetPathFileSize((base + ".idx").c_str());
	const s64 bin_before = FileSystem::GetPathFileSize((base + ".bin").c_str());

	// Room for a 1-byte blob but not a whole index entry, or not even the blob.
	const rlim_t limit = fail_in_index ? static_cast<rlim_t>(idx_before + 20) : static_cast<rlim_t>(bin_before + 10);
	std::signal(SIGXFSZ, SIG_IGN);
	const struct rlimit rl = {limit, limit};
	if (setrlimit(RLIMIT_FSIZE, &rl) != 0)
		return 11;

	std::vector<u8> key(24, 0x77);
	const std::vector<u8> data(fail_in_index ? 1 : 100, 0x42);
	if (store.Insert(key.data(), data.data(), data.size()))
		return 12;
	if (store.IsWritable())
		return 13;
	key[0] = 0x78;
	if (store.Insert(key.data(), data.data(), 1))
		return 14;
	if (FileSystem::GetPathFileSize((base + ".idx").c_str()) != idx_before)
		return 15;
	if (FileSystem::GetPathFileSize((base + ".bin").c_str()) != bin_before)
		return 16;
	// Reading still works.
	std::vector<u8> out;
	std::vector<u8> first(24, 0);
	if (!store.Lookup(first.data(), &out))
		return 17;
	return 0;
}

TEST_F(GSCacheFileTest, FailedAppendIsCutBackAndStopsWriting)
{
	for (const bool fail_in_index : {true, false})
	{
		const std::string base = PathOf(fail_in_index ? "full_idx" : "full_bin");
		{
			BlobStore store;
			ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
			FillStore(store, 4);
		}
		const pid_t pid = fork();
		ASSERT_GE(pid, 0);
		if (pid == 0)
			_exit(AppendUntilFull(base, MakeStamp("A"), fail_in_index));
		int status = 0;
		ASSERT_EQ(waitpid(pid, &status, 0), pid);
		ASSERT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), 0) << (fail_in_index ? "index" : "data");

		BlobStore store;
		ASSERT_TRUE(store.Open(base, KIND_TEST, MakeStamp("A"), 24));
		EXPECT_EQ(store.GetOpenInfo().truncated_bytes, 0u);
		EXPECT_EQ(store.GetEntryCount(), 4u);
		for (u32 i = 0; i < 4; i++)
			ExpectEntry(store, i);
	}
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
