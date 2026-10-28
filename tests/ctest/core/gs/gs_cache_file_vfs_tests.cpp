// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The GS cache packs under a host file system (the libretro core's frontend VFS), where every file is
// a stream with no descriptor: no lock and no truncate. A pack must still write, cut a torn tail and
// start over, without taking the missing lock for another process's. Its own binary, because a
// host file system is installed once for the whole process.

#include "GS/GSCacheFile.h"

#include "common/FileSystem.h"
#include "common/HostVFS.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using namespace GSCacheFile;

namespace
{
	// The host side: plain stdio behind the VFS table, with each handle a FILE*.
	void* VfsOpen(const char* path, unsigned mode, unsigned)
	{
		const bool update = (mode & 4) != 0;
		const char* m = ((mode & 3) == 1) ? "rb" : ((mode & 3) == 2) ? (update ? "r+b" : "wb") : (update ? "r+b" : "w+b");
		return std::fopen(path, m);
	}
	int VfsClose(void* h) { return std::fclose(static_cast<std::FILE*>(h)); }
	s64 VfsSize(void* h)
	{
		std::FILE* fp = static_cast<std::FILE*>(h);
		const long pos = std::ftell(fp);
		std::fseek(fp, 0, SEEK_END);
		const long size = std::ftell(fp);
		std::fseek(fp, pos, SEEK_SET);
		return size;
	}
	s64 VfsTell(void* h) { return std::ftell(static_cast<std::FILE*>(h)); }
	s64 VfsSeek(void* h, s64 off, int whence)
	{
		const int w = (whence == 0) ? SEEK_SET : (whence == 1) ? SEEK_CUR : SEEK_END;
		return std::fseek(static_cast<std::FILE*>(h), static_cast<long>(off), w) == 0 ? VfsTell(h) : -1;
	}
	s64 VfsRead(void* h, void* buf, u64 len) { return static_cast<s64>(std::fread(buf, 1, len, static_cast<std::FILE*>(h))); }
	s64 VfsWrite(void* h, const void* buf, u64 len)
	{
		const size_t n = std::fwrite(buf, 1, len, static_cast<std::FILE*>(h));
		std::fflush(static_cast<std::FILE*>(h));
		return static_cast<s64>(n);
	}
	int VfsFlush(void* h) { return std::fflush(static_cast<std::FILE*>(h)); }
	int VfsRemove(const char* p) { return std::remove(p); }
	int VfsRename(const char* a, const char* b) { return std::rename(a, b); }
	int VfsStat(const char* p, s32* size)
	{
		struct stat st;
		if (stat(p, &st) != 0)
			return 0;
		if (size)
			*size = static_cast<s32>(st.st_size);
		return 1 | (S_ISDIR(st.st_mode) ? 2 : 0);
	}
	int VfsMkdir(const char* d) { return mkdir(d, 0777) == 0 ? 0 : -1; }

	class GSCacheFileVfsTest : public ::testing::Test
	{
	protected:
		static void SetUpTestSuite()
		{
			static HostVFS::Ops ops = {};
			ops.open = VfsOpen;
			ops.close = VfsClose;
			ops.size = VfsSize;
			ops.tell = VfsTell;
			ops.seek = VfsSeek;
			ops.read = VfsRead;
			ops.write = VfsWrite;
			ops.flush = VfsFlush;
			ops.remove = VfsRemove;
			ops.rename = VfsRename;
			ops.stat = VfsStat;
			ops.mkdir = VfsMkdir;
			HostVFS::Install(ops);
		}

		void SetUp() override
		{
			m_dir = (std::filesystem::temp_directory_path() / ("gs_cache_file_vfs_tests_" + std::to_string(getpid()))).string();
			std::filesystem::remove_all(m_dir);
			std::filesystem::create_directories(m_dir);
		}
		void TearDown() override { std::filesystem::remove_all(m_dir); }

		static Stamp MakeStamp()
		{
			Stamp s;
			s.Add("build", "vfs");
			return s;
		}
		static std::vector<u8> Key(u32 n)
		{
			std::vector<u8> k(24, 0);
			std::memcpy(k.data(), &n, sizeof(n));
			return k;
		}

		std::string m_dir;
	};
} // namespace

TEST_F(GSCacheFileVfsTest, StreamsAreCookies)
{
	std::FILE* fp = FileSystem::OpenCFile(Path::Combine(m_dir, "probe").c_str(), "w+b");
	ASSERT_NE(fp, nullptr);
	EXPECT_EQ(fileno(fp), -1);
	std::fclose(fp);
}

TEST_F(GSCacheFileVfsTest, PackWritesReadsAndRecoversWithoutLocksOrTruncate)
{
	const std::string path = Path::Combine(m_dir, "test.pack");
	const std::vector<u8> data(300, 0x5a);
	{
		Pack pack;
		pack.Open(path, KIND_TEST, MakeStamp(), 1u << 30);
		for (u32 i = 0; i < 5; i++)
			ASSERT_TRUE(pack.Put(1, Key(i).data(), 24, data.data(), data.size(), i));
		const Pack::WriteResult res = pack.Write();
		ASSERT_FALSE(res.failed);
		EXPECT_FALSE(res.contended);
		// Appended through the host file system, which has no append mode on a new file.
		ASSERT_TRUE(pack.Put(1, Key(5).data(), 24, data.data(), data.size(), 5));
		ASSERT_EQ(pack.Write().records, 1u);
	}
	{
		Pack pack;
		pack.Open(path, KIND_TEST, MakeStamp(), 1u << 30);
		EXPECT_EQ(pack.GetEntryCount(), 6u);
		std::vector<u8> out;
		ASSERT_TRUE(pack.Get(1, Key(3).data(), 24, &out));
		EXPECT_EQ(out, data);
	}

	// A torn tail is cut by writing the pack whole, since the stream cannot be truncated.
	{
		std::FILE* fp = std::fopen(path.c_str(), "ab");
		std::fwrite("torn", 1, 4, fp);
		std::fclose(fp);
	}
	{
		Pack pack;
		pack.Open(path, KIND_TEST, MakeStamp(), 1u << 30);
		EXPECT_EQ(pack.GetLoadInfo().torn_bytes, 4u);
		EXPECT_EQ(pack.GetEntryCount(), 6u);
		EXPECT_TRUE(pack.Write().rewrote);
		// And emptied the same way.
		pack.Clear();
		EXPECT_TRUE(pack.Write().rewrote);
		ASSERT_TRUE(pack.Put(1, Key(9).data(), 24, data.data(), data.size(), 9));
		ASSERT_FALSE(pack.Write().failed);
	}
	Pack pack;
	pack.Open(path, KIND_TEST, MakeStamp(), 1u << 30);
	EXPECT_EQ(pack.GetLoadInfo().torn_bytes, 0u);
	EXPECT_EQ(pack.GetEntryCount(), 1u);
}
