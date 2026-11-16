// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Folder memory cards (pcsx2/SIO/Memcard/MemoryCardFolder.h) on a small card built in a temp
// folder. The tests act as the console's card driver: they write raw pages through Save(), in a
// chosen order, flush at chosen points, and check what lands in the host files.

#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Memcard/MemoryCardFolder.h"

#include "Config.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	namespace fs = std::filesystem;

	class ScopedTempDir
	{
	public:
		ScopedTempDir()
		{
			static std::atomic<unsigned> counter{0};
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			m_path = fs::temp_directory_path() / ("armsx2-mcdfolder-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
			fs::create_directories(m_path);
		}
		~ScopedTempDir()
		{
			std::error_code ec;
			fs::permissions(m_path, fs::perms::owner_all, fs::perm_options::add, ec);
			for (auto it = fs::recursive_directory_iterator(m_path, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
				fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
			fs::remove_all(m_path, ec);
		}
		const fs::path& Path() const { return m_path; }

	private:
		fs::path m_path;
	};

	void WriteHostFile(const fs::path& path, const std::string& content)
	{
		fs::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
	}

	std::string ReadHostFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	// An 8 MB card's superblock, as the BIOS formats it.
	std::string FormattedSuperblock()
	{
		std::string raw(FolderMemoryCard::BlockSize, '\xff');
		superblock sb;
		std::memset(&sb, 0xFF, sizeof(sb));
		std::memcpy(sb.magic, "Sony PS2 Memory Card Format ", sizeof(sb.magic));
		std::memset(sb.version, 0, sizeof(sb.version));
		std::memcpy(sb.version, "1.2.0.0", 7);
		sb.page_len = 512;
		sb.pages_per_cluster = 2;
		sb.pages_per_block = 16;
		sb.unused = 0xFF00;
		sb.clusters_per_card = 8192;
		sb.alloc_offset = 41;
		sb.alloc_end = 8135;
		sb.rootdir_cluster = 0;
		sb.backup_block1 = 1023;
		sb.backup_block2 = 1022;
		sb.padding0x48 = 0;
		std::memset(sb.ifc_list, 0, sizeof(sb.ifc_list));
		sb.ifc_list[0] = 8;
		sb.card_type = 2;
		sb.card_flags = 0x52;
		std::memcpy(raw.data(), &sb, sizeof(sb));
		return raw;
	}

	std::string PageString(const u8* page) { return std::string(reinterpret_cast<const char*>(page), FolderMemoryCard::PageSize); }

	constexpr const char* SAVE_DIR = "BESLES-00001";
	constexpr u32 FILE_SIZE = FolderMemoryCard::ClusterSize; // one cluster

	// Exposes the card's internals so a test can play the console's card driver.
	class TestCard : public FolderMemoryCard
	{
	public:
		void OpenAt(const fs::path& folder)
		{
			Pcsx2Config::McdOptions options;
			options.Enabled = true;
			options.Type = MemoryCardType::Folder;
			Open(folder.string(), options, 0, false, "");
			ASSERT_TRUE(IsPresent());
			ASSERT_TRUE(IsFormatted());
		}

		// The flush the card makes by itself once it has been idle.
		void RegularFlush() { Flush(); }

		u32 DataPage(u32 cluster, u32 page) const { return (cluster + m_superBlock.data.alloc_offset) * 2 + page; }
		u32 BlockOfDataCluster(u32 cluster) const { return (cluster + m_superBlock.data.alloc_offset) / 8; }
		bool IsPageCached(u32 absPage) const { return m_cache.find(absPage) != m_cache.end(); }

		void ReadPage(u32 absPage, u8* out) { Read(out, absPage * PageSizeRaw, PageSize); }
		void WritePage(u32 absPage, const u8* in) { Save(in, absPage * PageSizeRaw, PageSize); }
		void FillPage(u32 absPage, u8 value)
		{
			u8 page[PageSize];
			std::memset(page, value, sizeof(page));
			WritePage(absPage, page);
		}
		void FillCluster(u32 cluster, u8 value)
		{
			FillPage(DataPage(cluster, 0), value);
			FillPage(DataPage(cluster, 1), value);
		}

		void SetFat(u32 cluster, u32 value)
		{
			const u32 fatCluster = m_indirectFat.data[0][cluster / (ClusterSize / 4)];
			const u32 absPage = fatCluster * 2 + (cluster % (ClusterSize / 4)) / (PageSize / 4);
			u8 page[PageSize];
			ReadPage(absPage, page);
			std::memcpy(page + (cluster % (PageSize / 4)) * 4, &value, sizeof(value));
			WritePage(absPage, page);
		}

		MemoryCardFileEntry ReadEntry(u32 cluster, u32 index)
		{
			MemoryCardFileEntry entry;
			ReadPage(DataPage(cluster, index), entry.entry.raw);
			return entry;
		}

		void WriteEntry(u32 cluster, u32 index, const MemoryCardFileEntry& entry) { WritePage(DataPage(cluster, index), entry.entry.raw); }

		u32 FreeDataCluster() const { return GetFreeDataCluster(); }
		u32 NextCluster(u32 cluster) const { return m_fat.data[0][0][cluster] & NextDataClusterMask; }
	};

	// A card with one save directory holding one file, a.bin, of 0x11 bytes.
	// Layout after indexing: root entries in data clusters 0 and 1, the save directory's
	// "." and ".." in cluster 2, a.bin's entry in cluster 3 slot 0, a.bin's data from cluster 4.
	class FolderCardTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_card_dir = m_temp.Path() / "Mcd001.ps2";
			WriteHostFile(m_card_dir / "_pcsx2_superblock", FormattedSuperblock());
			WriteHostFile(m_card_dir / SAVE_DIR / "a.bin", std::string(m_file_size, '\x11'));
			Reopen();
		}

		void TearDown() override { m_card.Close(); }

		void Reopen()
		{
			m_card.Close();
			m_card.OpenAt(m_card_dir);
			// The layout the comment above describes.
			ASSERT_EQ(m_card.NextCluster(0), 1u);
			ASSERT_EQ(m_card.ReadEntry(1, 0).entry.data.cluster, kDirCluster);
			ASSERT_EQ(m_card.NextCluster(kDirCluster), kDirEntryCluster);
			ASSERT_EQ(m_card.ReadEntry(kDirEntryCluster, 0).entry.data.cluster, kFileCluster);
		}

		fs::path SaveDir() const { return m_card_dir / SAVE_DIR; }

		// Sets the save directory's file count, in the root's entry for it and in its own "." entry.
		void SetDirLength(u32 length)
		{
			MemoryCardFileEntry dir = m_card.ReadEntry(1, 0);
			dir.entry.data.length = length;
			m_card.WriteEntry(1, 0, dir);
			MemoryCardFileEntry dot = m_card.ReadEntry(kDirCluster, 0);
			dot.entry.data.length = length;
			m_card.WriteEntry(kDirCluster, 0, dot);
		}

		MemoryCardFileEntry FileEntry(const char* name, u32 cluster, u32 length)
		{
			MemoryCardFileEntry entry = m_card.ReadEntry(kDirEntryCluster, 0); // a.bin's, for its times
			entry.entry.data.mode = MemoryCardFileEntry::DefaultFileMode;
			std::memset(entry.entry.data.name, 0, sizeof(entry.entry.data.name));
			std::strncpy(reinterpret_cast<char*>(entry.entry.data.name), name, sizeof(entry.entry.data.name) - 1);
			entry.entry.data.cluster = cluster;
			entry.entry.data.length = length;
			return entry;
		}

		static constexpr u32 kDirCluster = 2;
		static constexpr u32 kDirEntryCluster = 3;
		static constexpr u32 kFileCluster = 4;
		static constexpr u32 kUsedLast = FolderMemoryCard::LastDataCluster | FolderMemoryCard::DataClusterInUseMask;

		u32 m_file_size = FILE_SIZE;
		ScopedTempDir m_temp;
		fs::path m_card_dir;
		TestCard m_card;
	};
} // namespace

// A new file's data written before the FAT and directory entry that place it, with a flush in
// between, reaches the host file once the entry is written.
TEST_F(FolderCardTest, DataWrittenBeforeItsFileEntrySurvivesAFlush)
{
	const u32 cluster = m_card.FreeDataCluster();
	ASSERT_EQ(cluster, 5u);
	m_card.FillCluster(cluster, 0x5A);

	m_card.FlushNow();

	m_card.SetFat(cluster, kUsedLast);
	m_card.WriteEntry(kDirEntryCluster, 1, FileEntry("b.bin", cluster, FILE_SIZE));
	SetDirLength(4);
	m_card.FlushNow();
	m_card.Close();

	EXPECT_EQ(ReadHostFile(SaveDir() / "b.bin"), std::string(FILE_SIZE, '\x5A'));
}

// A file entry that already names its cluster but not yet its length: the data flushed before the
// length arrives is kept and written once it does.
TEST_F(FolderCardTest, DataPastTheFileLengthSurvivesAFlush)
{
	const u32 cluster = m_card.FreeDataCluster();
	m_card.SetFat(cluster, kUsedLast);
	m_card.WriteEntry(kDirEntryCluster, 1, FileEntry("b.bin", cluster, 0));
	SetDirLength(4);
	m_card.FillCluster(cluster, 0x5A);

	m_card.FlushNow();

	m_card.WriteEntry(kDirEntryCluster, 1, FileEntry("b.bin", cluster, FILE_SIZE));
	m_card.FlushNow();
	m_card.Close();

	EXPECT_EQ(ReadHostFile(SaveDir() / "b.bin"), std::string(FILE_SIZE, '\x5A'));
}

// An unplaced page that holds what the card reads without it, such as an erased page of a free
// cluster, is dropped by the flush; one that holds data is kept.
TEST_F(FolderCardTest, UnplacedPageIsDroppedOnlyWhenTheCardReadsTheSameBytes)
{
	const u32 cluster = m_card.FreeDataCluster();
	m_card.FillPage(m_card.DataPage(cluster, 0), 0xFF);
	m_card.FillPage(m_card.DataPage(cluster, 1), 0x5A);

	m_card.FlushNow();

	EXPECT_FALSE(m_card.IsPageCached(m_card.DataPage(cluster, 0)));
	EXPECT_TRUE(m_card.IsPageCached(m_card.DataPage(cluster, 1)));
}

// A flush with nothing written since the last one does nothing, even with a page still waiting
// for a place: the directory index it would rewrite is left alone.
TEST_F(FolderCardTest, FlushWithNothingWrittenSinceTheLastOneDoesNothing)
{
	m_card.FillCluster(m_card.FreeDataCluster(), 0x5A);
	m_card.FlushNow();
	ASSERT_TRUE(fs::exists(SaveDir() / "_pcsx2_index"));
	fs::remove(SaveDir() / "_pcsx2_index");

	m_card.FlushNow();

	EXPECT_FALSE(fs::exists(SaveDir() / "_pcsx2_index"));
}

// A deleted file's cluster reused by a new file whose entry is not written yet: the data must not
// go to the deleted file's name, and must reach the new file.
TEST_F(FolderCardTest, ReusedClusterOfADeletedFileGoesToTheNewFile)
{
	MemoryCardFileEntry a = m_card.ReadEntry(kDirEntryCluster, 0);
	a.entry.data.mode &= ~MemoryCardFileEntry::Mode_Used;
	m_card.WriteEntry(kDirEntryCluster, 0, a);
	m_card.SetFat(kFileCluster, FolderMemoryCard::LastDataCluster);
	m_card.RegularFlush();
	ASSERT_FALSE(fs::exists(SaveDir() / "a.bin"));

	ASSERT_EQ(m_card.FreeDataCluster(), kFileCluster);
	m_card.SetFat(kFileCluster, kUsedLast);
	m_card.FillCluster(kFileCluster, 0xC3);
	m_card.RegularFlush();

	m_card.WriteEntry(kDirEntryCluster, 1, FileEntry("c.bin", kFileCluster, FILE_SIZE));
	SetDirLength(4);
	m_card.RegularFlush();
	m_card.Close();

	EXPECT_FALSE(fs::exists(SaveDir() / "a.bin"));
	EXPECT_EQ(ReadHostFile(SaveDir() / "c.bin"), std::string(FILE_SIZE, '\xC3'));
}

// A FAT chain that loops back on itself: a flush walks no further than the file's length, so it
// finishes, and the file's data lands where its length puts it.
TEST_F(FolderCardTest, LoopingFatChainEndsAtTheFileLength)
{
	const u32 first = m_card.FreeDataCluster();
	const u32 second = first + 1;
	m_card.SetFat(first, second | FolderMemoryCard::DataClusterInUseMask);
	m_card.SetFat(second, first | FolderMemoryCard::DataClusterInUseMask);
	m_card.WriteEntry(kDirEntryCluster, 1, FileEntry("b.bin", first, 2 * FILE_SIZE));
	SetDirLength(4);
	m_card.FillCluster(first, 0x5A);
	m_card.FillCluster(second, 0xA5);

	m_card.RegularFlush();
	m_card.Close();

	EXPECT_EQ(ReadHostFile(SaveDir() / "b.bin"), std::string(FILE_SIZE, '\x5A') + std::string(FILE_SIZE, '\xA5'));
}

// A save of two clusters.
class TwoClusterFolderCardTest : public FolderCardTest
{
protected:
	TwoClusterFolderCardTest() { m_file_size = 2 * FILE_SIZE; }
};

// While a flush cannot place a live file's cluster (here the FAT link to it briefly ends the
// chain), a write of part of a page there still starts from the file's bytes, not from 0xFF.
TEST_F(TwoClusterFolderCardTest, PartialWriteToAClusterTheFlushCouldNotPlaceKeepsTheFilesBytes)
{
	const u32 second = m_card.NextCluster(kFileCluster);
	const u32 last = FolderMemoryCard::LastDataCluster;
	ASSERT_NE(second, last);
	m_card.SetFat(kFileCluster, kUsedLast);
	m_card.RegularFlush();

	const u8 patch[16] = {0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99};
	m_card.Save(patch, m_card.DataPage(second, 0) * FolderMemoryCard::PageSizeRaw, sizeof(patch));
	m_card.SetFat(kFileCluster, second | FolderMemoryCard::DataClusterInUseMask);
	m_card.RegularFlush();
	m_card.Close();

	std::string expected(2 * FILE_SIZE, '\x11');
	expected.replace(FILE_SIZE, sizeof(patch), sizeof(patch), '\x99');
	EXPECT_EQ(ReadHostFile(SaveDir() / "a.bin"), expected);
}
