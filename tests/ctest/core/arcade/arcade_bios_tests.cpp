// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Arcade BIOS dumps (COH-H) are for arcade games only. The BIOS lists a player picks a console BIOS from must
// not show them, and a console game must not pick one by itself. The images here are made up: a ROMDIR that
// LoadBiosVersion reads, with the ROMVER and EXTINFO of a console and of two arcade boards, padded to the size
// the real dumps have.

#include <gtest/gtest.h>

#include "Config.h"
#include "ps2/BiosTools.h"

#include "common/ARCADE.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	constexpr size_t MB = 1024 * 1024;

	// ROMDIR records of the made-up image: RESET, ROMDIR, EXTINFO, ROMVER and the empty one that ends the list.
	// The loader reads the EXTINFO serial 0x10 into its file and the ROMVER at the start of its own.
	std::vector<char> MakeBios(const char* romver, const char* extinfo, size_t size)
	{
		std::vector<char> image(size, 0);
		const auto record = [&image](size_t index, const char* name, uint32_t file_size) {
			char* at = image.data() + index * 16;
			std::strncpy(at, name, 10);
			std::memcpy(at + 12, &file_size, 4);
		};
		record(0, "RESET", 0x10);
		record(1, "ROMDIR", 0x50);
		record(2, "EXTINFO", 0x20);
		record(3, "ROMVER", 0x10);
		// RESET at 0, ROMDIR at 0x10, EXTINFO at 0x60 (its serial 0x10 in), ROMVER at 0x80.
		std::memcpy(image.data() + 0x70, extinfo, 15);
		std::memcpy(image.data() + 0x80, romver, 14);
		return image;
	}

	void WriteImage(const std::string& path, const std::vector<char>& image)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(image.data(), static_cast<std::streamsize>(image.size()));
	}

	// The three kinds of dump, as the real ones are sized (the boards' flash chips are 2 MB).
	std::vector<char> ConsoleBios() { return MakeBios("0200AC20040614", "20040614-000000", 4 * MB); }
	std::vector<char> System256Bios() { return MakeBios("0100TZ20000603", "20040519-145634", 2 * MB); }
	std::vector<char> System246Bios() { return MakeBios("0100TZ20000603", "20021119-163841", 2 * MB); }

	class ArcadeBiosTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_dir = Path::Combine(testing::TempDir(), "arcade_bios");
			std::error_code ec;
			std::filesystem::remove_all(m_dir, ec);
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(m_dir.c_str(), true));
			m_old_bios_folder = EmuFolders::Bios;
			EmuFolders::Bios = m_dir;
		}

		void TearDown() override
		{
			EmuFolders::Bios = m_old_bios_folder;
			Arcade::s_session = false;
			std::error_code ec;
			std::filesystem::remove_all(m_dir, ec);
		}

		std::string Add(const char* name, const std::vector<char>& image)
		{
			const std::string path = Path::Combine(m_dir, name);
			WriteImage(path, image);
			return path;
		}

		std::string m_dir, m_old_bios_folder;
	};
} // namespace

TEST_F(ArcadeBiosTest, TheMadeUpImagesAreTheKindsOfBiosTheyStandFor)
{
	u32 version, region;
	std::string description, zone;
	ASSERT_TRUE(IsBIOS(Add("console.bin", ConsoleBios()).c_str(), version, description, region, zone));
	EXPECT_EQ(zone, "USA");

	// IsBIOS accepts arcade dumps too; IsConsoleBIOS is the filter.
	ASSERT_TRUE(IsBIOS(Add("arcade.bin", System256Bios()).c_str(), version, description, region, zone));
	EXPECT_EQ(zone, "COH-H");
	EXPECT_NE(description.find("System 256"), std::string::npos) << description;
}

TEST_F(ArcadeBiosTest, TheConsoleBiosListsLeaveOutArcadeDumps)
{
	u32 version, region;
	std::string description, zone;
	EXPECT_TRUE(IsConsoleBIOS(Add("console.bin", ConsoleBios()).c_str(), version, description, region, zone));
	EXPECT_EQ(zone, "USA");
	EXPECT_FALSE(IsConsoleBIOS(Add("system256.bin", System256Bios()).c_str(), version, description, region, zone));
	EXPECT_FALSE(IsConsoleBIOS(Add("system246.bin", System246Bios()).c_str(), version, description, region, zone));
	EXPECT_FALSE(IsConsoleBIOS(Add("junk.bin", std::vector<char>(4 * MB, 'x')).c_str(), version, description, region, zone));
	EXPECT_FALSE(IsConsoleBIOS(Path::Combine(m_dir, "missing.bin").c_str(), version, description, region, zone));
}

TEST_F(ArcadeBiosTest, AnArcadeDumpIsRecognisedByItsBoard)
{
	std::string serial;
	EXPECT_TRUE(IsArcadeBIOS(Add("system256.bin", System256Bios()).c_str(), &serial));
	EXPECT_EQ(serial, "20040519-145634");
	EXPECT_TRUE(IsArcadeBIOS(Add("system246.bin", System246Bios()).c_str(), &serial));
	EXPECT_EQ(serial, "20021119-163841");
	EXPECT_TRUE(IsArcadeBIOS(Add("system256.bin", System256Bios()).c_str())); // The serial is optional.
	EXPECT_FALSE(IsArcadeBIOS(Add("console.bin", ConsoleBios()).c_str(), &serial));
	EXPECT_FALSE(IsArcadeBIOS(Path::Combine(m_dir, "missing.bin").c_str()));
}

TEST_F(ArcadeBiosTest, AConsoleGameNeverStartsFromAnArcadeDump)
{
	// Only an arcade dump in the folder: there is no console BIOS to find, though the file is big enough to be one.
	Add("system256.bin", System256Bios());
	Add("big-arcade.bin", MakeBios("0100TZ20000603", "20040519-145634", 4 * MB));
	EXPECT_FALSE(IsBIOSAvailable(""));

	Add("console.bin", ConsoleBios());
	EXPECT_TRUE(IsBIOSAvailable(""));
}

TEST_F(ArcadeBiosTest, AnArcadeGameStartsFromTheBoardsDumpAndNotAConsoleOne)
{
	Add("console.bin", ConsoleBios());
	std::string needs;
	EXPECT_EQ(FindArcadeBiosFor("NM00004", "", &needs), "");
	EXPECT_EQ(needs, ""); // No arcade BIOS at all, so nothing to name.

	Add("system246.bin", System246Bios());
	EXPECT_EQ(FindArcadeBiosFor("NM00004", "", &needs), "system246.bin");
	Add("system256.bin", System256Bios());
	EXPECT_EQ(FindArcadeBiosFor("NM00004", "", &needs), "system256.bin"); // The System 256 one first.
	EXPECT_EQ(FindArcadeBiosFor("NM00010", "", &needs), "system246.bin"); // Battle Gear 3 refuses it.

	EXPECT_TRUE(std::filesystem::remove(Path::Combine(m_dir, "system246.bin")));
	EXPECT_EQ(FindArcadeBiosFor("NM00010", "", &needs), "");
	EXPECT_EQ(needs, "System 246");
}
