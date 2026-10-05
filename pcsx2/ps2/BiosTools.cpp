// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <cstdio>
#include <cstring>

#include "common/ARCADE.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "CDVD/CDVD.h"

#include "Common.h"
#include "BiosTools.h"
#include "Config.h"

static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;
static constexpr u32 DIRENTRY_SIZE = 16;

// --------------------------------------------------------------------------------------
// romdir structure (packing required!)
// --------------------------------------------------------------------------------------
//
#pragma pack(push, 1)

struct romdir
{
	char fileName[10];
	u16 extInfoSize;
	u32 fileSize;
};

#pragma pack(pop)

static_assert(sizeof(romdir) == DIRENTRY_SIZE, "romdir struct not packed to 16 bytes");

u32 BiosVersion;
u32 BiosChecksum;
u32 BiosRegion;
ConfigParam configParams1;
Config2Param configParams2;
bool ParamsRead;
bool NoOSD;
bool AllowParams1;
bool AllowParams2;
std::string BiosDescription;
std::string BiosZone;
std::string BiosSerial;
std::string BiosPath;
BiosDebugInformation CurrentBiosInformation;
std::vector<u8> BiosRom;

void ReadOSDConfigParames()
{
	if (ParamsRead)
		return;

	ParamsRead = true;

	u8 params[16];
	cdvdReadLanguageParams(params);

	configParams1.UC[0] = params[1] & 0x1F; // SPDIF, Screen mode, RGB/Comp, Jap/Eng Switch (Early bios).
	configParams1.ps1drvConfig = params[0]; // PS1 Mode Settings.
	configParams1.version = (params[2] & 0xE0) >> 5; // OSD Ver (Not sure but best guess).
	configParams1.language = params[2] & 0x1F; // Language.
	configParams1.timezoneOffset = params[4] | ((u32)(params[3] & 0x7) << 8);  // Timezone offset in minutes.
	configParams1.timeZoneID = params[6]; // ID for time zone selection
	
	// Region settings for time/date and extended language
	configParams2.UC[1] = ((u32)params[3] & 0x78) << 1; // Daylight Savings, 24hr clock, Date format
	configParams2.daylightSavings = configParams2.UC[1] & 0x10 ? 1 : 0;
	configParams2.timeFormat = configParams2.UC[1] & 0x20 ? 1 : 0;
	configParams2.dateFormat = configParams2.UC[1] & 0x80 ? 2 : (configParams2.UC[1] & 0x40 ? 1 : 0);
	// FIXME: format, version and language are set manually by the bios. Not sure if any game needs them, but it seems to set version to 2 and duplicate the language value.
	configParams2.version = 2;
	configParams2.language = configParams1.language;
}

static bool LoadBiosVersion(std::FILE* fp, u32& version, std::string& description, u32& region, std::string& zone, std::string& serial)
{
	romdir rd;
	for (u32 i = 0; i < 512 * 1024; i++)
	{
		if (std::fread(&rd, sizeof(rd), 1, fp) != 1)
			return false;

		if (std::strncmp(rd.fileName, "RESET", sizeof(rd.fileName)) == 0)
			break; /* found romdir */
	}

	s64 fileOffset = 0;
	s64 fileSize = FileSystem::FSize64(fp);
	bool foundRomVer = false;
	char romver[14 + 1] = {}; // ascii version loaded from disk.
	char extinfo[15 + 1] = {}; // ascii version loaded from disk.

	// ensure it's a null-terminated and not zero-length string
	while (rd.fileName[0] != '\0' && strnlen(rd.fileName, sizeof(rd.fileName)) != sizeof(rd.fileName))
	{
		if (std::strncmp(rd.fileName, "EXTINFO", sizeof(rd.fileName)) == 0)
		{
			s64 pos = FileSystem::FTell64(fp);
			if (FileSystem::FSeek64(fp, fileOffset + 0x10, SEEK_SET) != 0 ||
				std::fread(extinfo, 15, 1, fp) != 1 || FileSystem::FSeek64(fp, pos, SEEK_SET) != 0)
			{
				break;
			}
			serial = extinfo;
		}

		if (std::strncmp(rd.fileName, "ROMVER", sizeof(rd.fileName)) == 0)
		{

			s64 pos = FileSystem::FTell64(fp);
			if (FileSystem::FSeek64(fp, fileOffset, SEEK_SET) != 0 ||
				std::fread(romver, 14, 1, fp) != 1 || FileSystem::FSeek64(fp, pos, SEEK_SET) != 0)
			{
				break;
			}

			foundRomVer = true;
		}

		if ((rd.fileSize % 0x10) == 0)
			fileOffset += rd.fileSize;
		else
			fileOffset += (rd.fileSize + 0x10) & 0xfffffff0;

		if (std::fread(&rd, sizeof(rd), 1, fp) != 1)
			break;
	}

	fileOffset -= ((rd.fileSize + 0x10) & 0xfffffff0) - rd.fileSize;

	if (foundRomVer)
	{
		switch (romver[4])
		{
			// clang-format off
			case 'J': zone = "Japan";  region = 0;  break;
			case 'A': zone = "USA";    region = 1;  break;
			case 'E': zone = "Europe"; region = 2;  break;
			// case 'E': zone = "Oceania";region = 3;  break; // Not implemented
			case 'H': zone = "Asia";   region = 4;  break;
			// case 'E': zone = "Russia"; region = 3;  break; // Not implemented
			case 'C': zone = "China";  region = 6;  break;
			// case 'A': zone = "Mexico"; region = 7;  break; // Not implemented
			case 'T': zone = (romver[5]=='Z') ? "COH-H" : "T10K";   region = 8;  break;
			case 'X': zone = "Test";   region = 9;  break;
			case 'P': zone = "Free";   region = 10; break;
			// clang-format on
			default:
				zone.clear();
				zone += romver[4];
				region = 0;
				break;
		}
		// TODO: some regions can be detected only from rom1
		/* switch (rom1:DVDID[4])
		{
			// clang-format off
			case 'O': zone = "Oceania";region = 3;  break;
			case 'R': zone = "Russia"; region = 5;  break;
			case 'M': zone = "Mexico"; region = 7;  break;
			// clang-format on
		} */

		char vermaj[3] = {romver[0], romver[1], 0};
		char vermin[3] = {romver[2], romver[3], 0};
		// An arcade COH-H BIOS: every one reads v1.0(06/03/2000), the date its image for the arcade TOOL
		// was built, so name the board by its EXTINFO serial instead (PCSX2x6).
		if (zone == "COH-H")
		{
			const char* board = (serial == "20040519-145634") ? "System 256" : // also Super System 256
			                    (serial == "20021119-163841") ? "System 246 Rack C" :
			                    (serial == "20000901-114731") ? "COH-H Board (A-000-010)" :
			                                                    "Arcade board";
			description = StringUtil::StdStringFromFormat("%-7s %s %s", zone.c_str(), board, serial.c_str());
		}
		else
		{
			description = StringUtil::StdStringFromFormat("%-7s v%s.%s(%c%c/%c%c/%c%c%c%c)  %s %s",
				zone.c_str(),
				vermaj, vermin,
				romver[12], romver[13], // day
				romver[10], romver[11], // month
				romver[6], romver[7], romver[8], romver[9], // year!
				(romver[5] == 'C') ? "Console" : (romver[5] == 'D') ? "Devel" :
																	  "",
				serial.c_str());
		}

		version = static_cast<u32>(strtol(vermaj, (char**)NULL, 0) << 8);
		version |= strtol(vermin, (char**)NULL, 0);

		Console.WriteLn("BIOS Found: %s", description.c_str());
	}
	else
		return false;

	if (fileSize < (int)fileOffset)
	{
		description += StringUtil::StdStringFromFormat(" %d%%", (((int)fileSize * 100) / (int)fileOffset));
		// we force users to have correct bioses,
		// not that lame scph10000 of 513KB ;-)
	}

	return true;
}

static void ChecksumIt(u32& result, u32 offset, u32 size)
{
	const u8* srcdata = &BiosRom[offset];
	pxAssume((size & 3) == 0);
	for (size_t i = 0; i < size / 4; ++i)
		result ^= reinterpret_cast<const u32*>(srcdata)[i];
}

// Attempts to load a BIOS rom sub-component, by trying multiple combinations of base
// filename and extension.  The bios specified in the user's configuration is used as
// the base.
//
// Parameters:
//   ext - extension of the sub-component to load. Valid options are rom1 and rom2.
//
static void LoadExtraRom(const char* ext, u32 offset, u32 size)
{
	// Try first a basic extension concatenation (normally results in something like name.bin.rom1)
	std::string Bios1(StringUtil::StdStringFromFormat("%s.%s", BiosPath.c_str(), ext));

	s64 filesize;
	if ((filesize = FileSystem::GetPathFileSize(Bios1.c_str())) <= 0)
	{
		// Try the name properly extensioned next (name.rom1)
		Bios1 = Path::ReplaceExtension(BiosPath, ext);
		if ((filesize = FileSystem::GetPathFileSize(Bios1.c_str())) <= 0)
		{
			Console.WriteLn(Color_Gray, "BIOS %s module not found, skipping...", ext);
			return;
		}
	}

	BiosRom.resize(offset + size);

	auto fp = FileSystem::OpenManagedCFileTryIgnoreCase(Bios1.c_str(), "rb");
	if (!fp || std::fread(&BiosRom[offset], static_cast<size_t>(std::min<s64>(size, filesize)), 1, fp.get()) != 1)
	{
		Console.Warning("BIOS Warning: %s could not be read (permission denied?)", ext);
		return;
	}
	// Checksum for ROM1, ROM2?  Rama says no, Gigaherz says yes.  I'm not sure either way.  --air
	//ChecksumIt( BiosChecksum, dest );
}

static void LoadIrx(const std::string& filename, u8* dest, size_t maxSize)
{
	auto fp = FileSystem::OpenManagedCFile(filename.c_str(), "rb");
	if (fp)
	{
		const s64 filesize = FileSystem::FSize64(fp.get());
		const s64 readSize = std::min(filesize, static_cast<s64>(maxSize));
		if (std::fread(dest, readSize, 1, fp.get()) == 1)
			return;
	}

	Console.Warning("IRX Warning: %s could not be read", filename.c_str());
	return;
}

// EXTINFO serial of the System 256 board BIOS (also on Super System 256 boards).
static constexpr const char* ARCADE_S256_BIOS_SERIAL = "20040519-145634";

// The boards' BIOS is a 2MB flash chip, and that is how it is dumped (MAME's sys246/sys256 sets,
// r27v1602f.7d / .8g). LoadBIOS reads any size into the 4MB ROM, as PCSX2x6 does; only the search
// has to know a 2MB file can be one.
static constexpr u32 MIN_ARCADE_BIOS_SIZE = 2 * _1mb;

// Whether the file is a Namco arcade board's BIOS (a COH-H dump), and its EXTINFO serial, which names the
// board it is from.
static bool IsArcadeBIOS(const char* filename, std::string* board_serial = nullptr)
{
	const auto fp = FileSystem::OpenManagedCFile(filename, "rb");
	if (!fp)
		return false;

	u32 version, region;
	std::string description, zone, serial;
	if (!LoadBiosVersion(fp.get(), version, description, region, zone, serial) || zone != "COH-H")
		return false;

	if (board_serial)
		*board_serial = std::move(serial);
	return true;
}

// An arcade game takes the board's own BIOS and nothing else: a console BIOS lacks the FILEIO, MCMAN and
// SIO2MAN behaviour the arcade games are written against. Any COH-H dump will do, the System 256 one
// first, because it runs System 246 games as well (PCSX2x6's advice).
static std::string FindArcadeBiosImage()
{
	Console.WriteLn("Searching for an arcade (COH-H) BIOS image in '%s'...", EmuFolders::Bios.c_str());

	FileSystem::FindResultsArray results;
	if (!FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
		return std::string();

	std::string found;
	for (const FILESYSTEM_FIND_DATA& fd : results)
	{
		if (fd.Size < MIN_ARCADE_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
			continue;

		std::string serial;
		if (!IsArcadeBIOS(fd.FileName.c_str(), &serial))
			continue;

		if (serial == ARCADE_S256_BIOS_SERIAL)
			return std::move(fd.FileName);
		if (found.empty())
			found = std::move(fd.FileName);
	}

	if (found.empty())
		Console.Error("Unable to find an arcade (COH-H) BIOS image");
	return found;
}

// The board an arcade BIOS dump is from, by its EXTINFO serial (PCSX2x6's names, as in LoadBiosVersion).
enum class ArcadeBoard
{
	S256, // System 256, also on Super System 256 boards
	S246C, // System 246 Rack C
	CohA000010, // Sony's COH-H board (A-000-010)
	Other,
};

static ArcadeBoard ArcadeBoardOf(const std::string& serial)
{
	if (serial == ARCADE_S256_BIOS_SERIAL)
		return ArcadeBoard::S256;
	if (serial == "20021119-163841")
		return ArcadeBoard::S246C;
	if (serial == "20000901-114731")
		return ArcadeBoard::CohA000010;
	return ArcadeBoard::Other;
}

// The arcade games that do not start on one board's BIOS, from PCSX2x6's compatibility list
// (https://github.com/PS2Homebrew-arcade/pcsx2x6/issues/9), and the board whose BIOS they need instead.
// Battle Gear 3 and Battle Gear 3 Tuned reject the System 256 BIOS (the game stops at "RACK ERROR!!");
// Bloody Roar 3 crashes on Sony's COH-H board BIOS, and runs on the System 246 Rack C and System 256 ones.
struct ArcadeBiosRule
{
	const char* gameid;
	ArcadeBoard refuses;
	const char* needs;
};
static constexpr ArcadeBiosRule s_arcade_bios_rules[] = {
	{"NM00002", ArcadeBoard::CohA000010, "System 256"},
	{"NM00010", ArcadeBoard::S256, "System 246"},
	{"NM00015", ArcadeBoard::S256, "System 246"},
};

std::string FindArcadeBiosFor(const std::string& gameid, const std::string& picked, std::string* needs)
{
	struct Dump
	{
		std::string name;
		ArcadeBoard board;
	};
	std::vector<Dump> dumps;
	FileSystem::FindResultsArray results;
	if (FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
	{
		for (const FILESYSTEM_FIND_DATA& fd : results)
		{
			std::string serial;
			if (fd.Size >= MIN_ARCADE_BIOS_SIZE && fd.Size <= MAX_BIOS_SIZE && IsArcadeBIOS(fd.FileName.c_str(), &serial))
				dumps.push_back({std::string(Path::GetFileName(fd.FileName)), ArcadeBoardOf(serial)});
		}
	}

	const ArcadeBiosRule* rule = nullptr;
	for (const ArcadeBiosRule& r : s_arcade_bios_rules)
	{
		if (gameid == r.gameid)
			rule = &r;
	}
	const auto runs = [rule](const Dump& d) { return !rule || d.board != rule->refuses; };
	// One of [board]'s dumps the game runs on, the picked one when it is one of them.
	const auto of_board = [&dumps, &runs, &picked](ArcadeBoard board) -> const Dump* {
		const Dump* found = nullptr;
		for (const Dump& d : dumps)
		{
			if (d.board != board || !runs(d))
				continue;
			if (d.name == picked)
				return &d;
			if (!found)
				found = &d;
		}
		return found;
	};

	// The System 256 BIOS first, for every game: it runs System 246 games as well (PCSX2x6's default), and
	// they sound right on it; Soul Calibur III's sound buzzed on the System 246 Rack C one. Then the System 246
	// Rack C BIOS (Battle Gear 3, which refuses the System 256 one, starts here), Sony's COH-H board's, any other.
	for (const ArcadeBoard board : {ArcadeBoard::S256, ArcadeBoard::S246C, ArcadeBoard::CohA000010, ArcadeBoard::Other})
	{
		if (const Dump* d = of_board(board))
			return d->name;
	}

	if (needs && rule && !dumps.empty())
		*needs = rule->needs;
	return {};
}

static std::string FindBiosImage()
{
	if (Arcade::IsActive())
		return FindArcadeBiosImage();

	Console.WriteLn("Searching for a BIOS image in '%s'...", EmuFolders::Bios.c_str());

	FileSystem::FindResultsArray results;
	if (!FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
		return std::string();

	u32 version, region;
	std::string description, zone;
	for (const FILESYSTEM_FIND_DATA& fd : results)
	{
		if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
			continue;

		// An arcade board's BIOS (COH-H) cannot run a console game: one dumped at 4 MB or more would
		// otherwise pass for a console BIOS here.
		if (IsBIOS(fd.FileName.c_str(), version, description, region, zone) && zone != "COH-H")
		{
			Console.WriteLn("Using BIOS '%s' (%s %s)", fd.FileName.c_str(), description.c_str(), zone.c_str());
			return std::move(fd.FileName);
		}
	}

	Console.Error("Unable to auto locate a BIOS image");
	return std::string();
}

bool IsBIOS(const char* filename, u32& version, std::string& description, u32& region, std::string& zone)
{
	std::string serial;
	const auto fp = FileSystem::OpenManagedCFile(filename, "rb");
	if (!fp)
		return false;

	// FPS2BIOS is smaller and of variable size
	//if (inway.Length() < 512*1024) return false;
	return LoadBiosVersion(fp.get(), version, description, region, zone, serial);
}

bool IsBIOSFromFd(int fd, u32& version, std::string& description, u32& region, std::string& zone)
{
	if (fd < 0)
		return false;

	std::FILE* fp = ::fdopen(fd, "rb");
	if (!fp)
		return false;

	std::string serial;
	const bool ok = LoadBiosVersion(fp, version, description, region, zone, serial);
	std::fclose(fp); // closes underlying fd as well
	return ok;
}

bool IsBIOSAvailable(const std::string& full_path)
{
	// We can't use EmuConfig here since it may not be loaded yet.
	if (!full_path.empty() && FileSystem::FileExists(full_path.c_str()))
		return true;

	// No bios configured or the configured name is missing, check for one in the BIOS directory.
	const std::string auto_path(FindBiosImage());
	return !auto_path.empty() && FileSystem::FileExists(auto_path.c_str());
}

// Loads the configured bios rom file into PS2 memory.  PS2 memory must be allocated prior to
// this method being called.
//
// Remarks:
//   This function does not fail if rom1 or rom2 files are missing, since none are
//   explicitly required for most emulation tasks.
//
// Exceptions:
//   BadStream - Thrown if the primary bios file (usually .bin) is not found, corrupted, etc.
//
bool LoadBIOS()
{
	pxAssertMsg(eeMem->ROM, "PS2 system memory has not been initialized yet.");

	std::string path = EmuConfig.FullpathToBios();

	// The arcade BIOS a player picked has to be one; otherwise, look for one (FindBiosImage).
	if (Arcade::IsActive() && !path.empty() && FileSystem::FileExists(path.c_str()) && !IsArcadeBIOS(path.c_str()))
	{
		Console.Warning("Arcade BIOS '%s' is not a COH-H BIOS, looking for one.", EmuConfig.BaseFilenames.Bios.c_str());
		path.clear();
	}

	if (path.empty() || !FileSystem::FileExists(path.c_str()))
	{
		if (!path.empty())
		{
			Console.Warning("Configured BIOS '%s' does not exist, trying to find an alternative.",
				EmuConfig.BaseFilenames.Bios.c_str());
		}

		path = FindBiosImage();
		if (path.empty())
			return false;
	}

	auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
	if (!fp)
		return false;

	const s64 filesize = FileSystem::FSize64(fp.get());
	if (filesize <= 0)
		return false;

	LoadBiosVersion(fp.get(), BiosVersion, BiosDescription, BiosRegion, BiosZone, BiosSerial);

	BiosRom.resize(Ps2MemSize::Rom);

	if (FileSystem::FSeek64(fp.get(), 0, SEEK_SET) ||
		std::fread(BiosRom.data(), static_cast<size_t>(std::min<s64>(Ps2MemSize::Rom, filesize)), 1, fp.get()) != 1)
	{
		return false;
	}

	// If file is less than 2mb it doesn't have an OSD (Devel consoles)
	// So skip HLEing OSDSys Param stuff
	if (filesize < 2465792)
		NoOSD = true;
	else
		NoOSD = false;

	BiosChecksum = 0;
	ChecksumIt(BiosChecksum, 0, Ps2MemSize::Rom);
	BiosPath = std::move(path);

	//injectIRX("host.irx");	//not fully tested; still buggy

	LoadExtraRom("rom1", Ps2MemSize::Rom, Ps2MemSize::Rom1);
	LoadExtraRom("rom2", Ps2MemSize::Rom + Ps2MemSize::Rom1, Ps2MemSize::Rom2);
	return true;
}

void CopyBIOSToMemory()
{
	if (BiosRom.size() >= Ps2MemSize::Rom)
	{
		std::memcpy(eeMem->ROM, BiosRom.data(), sizeof(eeMem->ROM));
		if (BiosRom.size() >= (Ps2MemSize::Rom + Ps2MemSize::Rom1))
		{
			std::memcpy(eeMem->ROM1, BiosRom.data() + Ps2MemSize::Rom, sizeof(eeMem->ROM1));
			if (BiosRom.size() >= (Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2))
				std::memcpy(eeMem->ROM2, BiosRom.data() + Ps2MemSize::Rom + Ps2MemSize::Rom1, sizeof(eeMem->ROM2));
		}
	}

	if (EmuConfig.CurrentIRX.length() > 3)
		LoadIrx(EmuConfig.CurrentIRX, &eeMem->ROM[0x3C0000], sizeof(eeMem->ROM) - 0x3C0000);

	CurrentBiosInformation.eeThreadListAddr = 0;
}
