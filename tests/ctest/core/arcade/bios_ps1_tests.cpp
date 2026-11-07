// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// PS1 BIOS files do not run PS2 games. The PS1's own BIOS is a 512 KB image without a PS2 ROM directory; the
// PS3's ps1_rom.bin carries one and reads as a v1.90 PS2 BIOS, stamped 2011-10-12, so it used to be listed and
// auto-picked as a BIOS. The images here are made up, as in arcade_bios_tests.cpp: a ROMDIR that LoadBiosVersion
// reads, with a ROMVER and an EXTINFO stamp, padded to the size the real dumps have.

#include <gtest/gtest.h>

#include "Config.h"
#include "ps2/BiosTools.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
	constexpr size_t KB = 1024;
	constexpr size_t MB = 1024 * KB;

	// ROMDIR records: RESET, ROMDIR, EXTINFO, ROMVER and the empty one that ends the list. The loader reads the
	// EXTINFO serial 0x10 into its file and the ROMVER at the start of its own.
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

	// A v1.90 USA console BIOS as Sony built it, and the PS3 ROM that reads as the same version.
	std::vector<char> RealConsoleBios() { return MakeBios("0190AC20030623", "20030623-120000", 4 * MB); }
	std::vector<char> Ps3Ps1Rom() { return MakeBios("0190AC20030623", "20111012-094243", 4 * MB); }
	// The PS1's: 512 KB of whatever, and no "RESET" record in it.
	std::vector<char> Ps1Bios() { return std::vector<char>(512 * KB, 0x5a); }

	class Ps1BiosTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_dir = Path::Combine(testing::TempDir(), "bios_ps1");
			std::error_code ec;
			std::filesystem::remove_all(m_dir, ec);
			ASSERT_TRUE(FileSystem::CreateDirectoryPath(m_dir.c_str(), true));
			m_old_bios_folder = EmuFolders::Bios;
			EmuFolders::Bios = m_dir;
		}

		void TearDown() override
		{
			EmuFolders::Bios = m_old_bios_folder;
			std::error_code ec;
			std::filesystem::remove_all(m_dir, ec);
		}

		std::string Add(const char* name, const std::vector<char>& image)
		{
			const std::string path = Path::Combine(m_dir, name);
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out.write(image.data(), static_cast<std::streamsize>(image.size()));
			return path;
		}

		std::string m_dir, m_old_bios_folder;
	};
} // namespace

TEST_F(Ps1BiosTest, The512KbPs1ImageIsPs1AndNotAPs2Bios)
{
	const std::string path = Add("scph1001.bin", Ps1Bios());
	u32 version, region;
	std::string description, zone;
	EXPECT_TRUE(IsPs1BIOS(path.c_str()));
	EXPECT_FALSE(IsBIOS(path.c_str(), version, description, region, zone));
}

TEST_F(Ps1BiosTest, The2011StampedPs3RomIsPs1AndNoLongerPassesForAConsoleBios)
{
	const std::string path = Add("ps1_rom.bin", Ps3Ps1Rom());
	u32 version, region;
	std::string description, zone;
	EXPECT_TRUE(IsPs1BIOS(path.c_str()));
	EXPECT_FALSE(IsBIOS(path.c_str(), version, description, region, zone));
	EXPECT_FALSE(IsConsoleBIOS(path.c_str(), version, description, region, zone));
}

TEST_F(Ps1BiosTest, ARealConsoleBiosOfTheSameVersionIsNotPs1)
{
	const std::string path = Add("scph50001.bin", RealConsoleBios());
	u32 version, region;
	std::string description, zone;
	EXPECT_FALSE(IsPs1BIOS(path.c_str()));
	ASSERT_TRUE(IsBIOS(path.c_str(), version, description, region, zone));
	EXPECT_EQ(zone, "USA");
	EXPECT_NE(description.find("v01.90"), std::string::npos) << description;
}

TEST_F(Ps1BiosTest, FourMegabytesOfNothingIsNeitherAPs1NorAPs2Bios)
{
	const std::string path = Add("blank.bin", std::vector<char>(4 * MB, 0));
	u32 version, region;
	std::string description, zone;
	EXPECT_FALSE(IsPs1BIOS(path.c_str()));
	EXPECT_FALSE(IsBIOS(path.c_str(), version, description, region, zone));
}

TEST_F(Ps1BiosTest, AMissingFileIsNotPs1)
{
	EXPECT_FALSE(IsPs1BIOS(Path::Combine(m_dir, "nothing.bin").c_str()));
}

#ifndef _WIN32
TEST_F(Ps1BiosTest, TheFileDescriptorFormAgreesWithThePathForm)
{
	const std::string ps1 = Add("scph5501.bin", Ps1Bios());
	const std::string ps3 = Add("ps3_ps1_bios.bin", Ps3Ps1Rom());
	const std::string real = Add("scph50001.bin", RealConsoleBios());
	EXPECT_TRUE(IsPs1BIOSFromFd(::open(ps1.c_str(), O_RDONLY)));
	EXPECT_TRUE(IsPs1BIOSFromFd(::open(ps3.c_str(), O_RDONLY)));
	EXPECT_FALSE(IsPs1BIOSFromFd(::open(real.c_str(), O_RDONLY)));
	EXPECT_FALSE(IsPs1BIOSFromFd(-1));
}
#endif

TEST_F(Ps1BiosTest, TheSearchSkipsPs1FilesAndStillFindsARealBios)
{
	// Nothing but PS1 files in the folder: nothing to boot with.
	Add("scph1001.bin", Ps1Bios());
	Add("ps1_rom.bin", Ps3Ps1Rom());
	EXPECT_FALSE(IsBIOSAvailable(std::string()));

	// Add a real one beside them: found, whichever order the folder lists them in.
	Add("scph50001.bin", RealConsoleBios());
	EXPECT_TRUE(IsBIOSAvailable(std::string()));
}
