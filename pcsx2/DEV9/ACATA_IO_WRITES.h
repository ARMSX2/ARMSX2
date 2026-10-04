// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Pcsx2Types.h"

#include "fmt/format.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>

// ARMSX2: what an arcade game writes to a hard drive that is a CHD. A CHD cannot be written, and PCSX2x6
// drops those writes, so a game read back what it wrote as it was before, in that session and the next.
// They are kept here instead, in a file of their own beside the game's SRAM, and read back over the CHD.
//
// The file is a header naming the drive it belongs to (its size and the CHD's SHA-1), then a record per
// sector written: the sector's number (8 bytes) and its bytes. A sector written again is rewritten in its
// record. A file made for another image (another dump of the game) is set aside, never read; a record a
// crash cut short is left out. Little-endian, as every host is. Used on the emulator's CPU thread only, as
// the rest of the drive is.
class ChdWrites
{
public:
	// Far past what a game keeps on its drive (settings, rankings, logs): a runaway writer stops here
	// instead of filling the storage.
	static constexpr u64 DEFAULT_MAX_BYTES = 256ull << 20;

	explicit ChdWrites(u64 max_bytes = DEFAULT_MAX_BYTES)
		: m_max_bytes(max_bytes)
	{
	}
	~ChdWrites() { Close(); }

	ChdWrites(const ChdWrites&) = delete;
	ChdWrites& operator=(const ChdWrites&) = delete;

	// The writes to the drive [sha1] / [drive_bytes] names, kept in [path] from the first one on.
	void Open(const std::string& path, const u8 (&sha1)[20], u64 drive_bytes, u32 sector_bytes)
	{
		Close();
		m_path = path;
		m_sector_bytes = sector_bytes;
		m_sectors = sector_bytes ? drive_bytes / sector_bytes : 0;
		m_header = {};
		std::memcpy(m_header.magic, MAGIC, sizeof(m_header.magic));
		m_header.version = VERSION;
		m_header.sector_bytes = sector_bytes;
		m_header.drive_bytes = drive_bytes;
		std::memcpy(m_header.sha1, sha1, sizeof(m_header.sha1));
		if (!m_path.empty() && FileSystem::FileExists(m_path.c_str()))
			Load();
	}

	void Close()
	{
		if (m_file)
			std::fclose(m_file);
		m_file = nullptr;
		m_records.clear();
		m_end = 0;
		m_path.clear();
		m_full = false;
	}

	// Keeps [count] sectors from [lba] on, from [src]. False when one of them could not be kept.
	bool Write(u64 lba, u32 count, const u8* src)
	{
		if (m_path.empty() || m_sector_bytes == 0 || (!m_file && !Create()))
			return false;
		const u64 record = sizeof(u64) + m_sector_bytes;
		bool kept = true;
		for (u32 i = 0; i < count; i++)
		{
			const u64 sector = lba + i;
			if (sector >= m_sectors)
			{
				kept = false;
				continue;
			}
			const auto it = m_records.find(sector);
			const bool fresh = (it == m_records.end());
			if (fresh && m_end + record > m_max_bytes)
			{
				if (!m_full)
					Console.ErrorFmt("ChdWrites: '{}' is full ({} MB); the game's further writes are not kept.", m_path, m_max_bytes >> 20);
				m_full = true;
				kept = false;
				continue;
			}
			const u64 at = fresh ? m_end : it->second;
			if (FileSystem::FSeek64(m_file, static_cast<s64>(at), SEEK_SET) != 0 ||
				std::fwrite(&sector, sizeof(sector), 1, m_file) != 1 ||
				std::fwrite(src + static_cast<size_t>(i) * m_sector_bytes, m_sector_bytes, 1, m_file) != 1)
			{
				Console.ErrorFmt("ChdWrites: writing '{}' failed at sector {}.", m_path, sector);
				kept = false;
				break;
			}
			if (fresh)
			{
				m_records.emplace(sector, at);
				m_end += record;
			}
		}
		if (std::fflush(m_file) != 0)
		{
			Console.ErrorFmt("ChdWrites: writing '{}' failed.", m_path);
			kept = false;
		}
		return kept;
	}

	// Lays the kept sectors among [count] from [lba] over [dst], which holds those sectors as the CHD has
	// them.
	void Apply(u64 lba, u32 count, u8* dst)
	{
		if (m_records.empty() || !m_file)
			return;
		for (u32 i = 0; i < count; i++)
		{
			const auto it = m_records.find(lba + i);
			if (it == m_records.end())
				continue;
			if (FileSystem::FSeek64(m_file, static_cast<s64>(it->second + sizeof(u64)), SEEK_SET) != 0 ||
				std::fread(dst + static_cast<size_t>(i) * m_sector_bytes, m_sector_bytes, 1, m_file) != 1)
				Console.ErrorFmt("ChdWrites: reading '{}' failed at sector {}.", m_path, lba + i);
		}
	}

	// How many sectors are kept.
	size_t Count() const { return m_records.size(); }

private:
	struct Header
	{
		char magic[8];
		u32 version;
		u32 sector_bytes;
		u64 drive_bytes;
		u8 sha1[20];
		u32 reserved;
	};
	static_assert(sizeof(Header) == 48);
	static constexpr char MAGIC[8] = {'A', 'R', 'M', 'S', 'X', '2', 'H', 'W'};
	static constexpr u32 VERSION = 1;

	// An existing file: its records, when it names this drive; set aside when it names another.
	void Load()
	{
		std::FILE* fp = FileSystem::OpenCFile(m_path.c_str(), "r+b");
		if (!fp)
		{
			Console.ErrorFmt("ChdWrites: '{}' cannot be opened; the game's writes are not kept.", m_path);
			m_path.clear();
			return;
		}
		Header got;
		if (std::fread(&got, sizeof(got), 1, fp) != 1 || std::memcmp(&got, &m_header, sizeof(got)) != 0)
		{
			std::fclose(fp);
			// Under a name no other file has: a rename onto one would replace it.
			const long long now = static_cast<long long>(std::time(nullptr));
			std::string aside = fmt::format("{}.{}.old", m_path, now);
			for (int n = 1; FileSystem::FileExists(aside.c_str()); n++)
				aside = fmt::format("{}.{}-{}.old", m_path, now, n);
			if (FileSystem::RenamePath(m_path.c_str(), aside.c_str()))
			{
				Console.WarningFmt("ChdWrites: '{}' was made for another image of this drive; set aside as '{}'.", m_path, aside);
			}
			else
			{
				Console.ErrorFmt("ChdWrites: '{}' was made for another image and cannot be set aside; the game's writes are not kept.", m_path);
				m_path.clear();
			}
			return;
		}
		const s64 size = FileSystem::FSize64(fp);
		const u64 record = sizeof(u64) + m_sector_bytes;
		u64 at = sizeof(Header);
		while (size > 0 && at + record <= static_cast<u64>(size))
		{
			u64 sector;
			if (FileSystem::FSeek64(fp, static_cast<s64>(at), SEEK_SET) != 0 || std::fread(&sector, sizeof(sector), 1, fp) != 1 ||
				sector >= m_sectors)
				break;
			m_records[sector] = at;
			at += record;
		}
		m_file = fp;
		m_end = at;
		Console.WriteLnFmt("ChdWrites: {} sectors the game wrote to its drive, from '{}'.", m_records.size(), m_path);
	}

	// The file, at the drive's first write: the header alone.
	bool Create()
	{
		const std::string dir(Path::GetDirectory(m_path));
		if (!dir.empty())
			FileSystem::EnsureDirectoryExists(dir.c_str(), true);
		std::FILE* fp = FileSystem::OpenCFile(m_path.c_str(), "w+b");
		if (!fp || std::fwrite(&m_header, sizeof(m_header), 1, fp) != 1 || std::fflush(fp) != 0)
		{
			if (fp)
				std::fclose(fp);
			Console.ErrorFmt("ChdWrites: '{}' cannot be made; the game's writes are not kept.", m_path);
			m_path.clear();
			return false;
		}
		m_file = fp;
		m_end = sizeof(Header);
		return true;
	}

	const u64 m_max_bytes;
	std::string m_path;
	std::FILE* m_file = nullptr;
	Header m_header = {};
	u32 m_sector_bytes = 0;
	u64 m_sectors = 0;
	std::unordered_map<u64, u64> m_records; // sector -> where its record is in the file
	u64 m_end = 0; // where the next record goes
	bool m_full = false;
};
