// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// See ArcadeSetup.h. The rules here are the Android app's (Arcade.kt, ArcadeLibrary.kt); a comment says where
// the two differ.

#include "Arcade/ArcadeSetup.h"

#include "Arcade/AcGameFile.h"
#include "Arcade/ArcadeFiles.h"

#include "Catalog/FileUtil.h"
#include "Catalog/Json.h"

#include "Config.h"
#include "GameDatabase.h"
#include "Host.h"
#include "VMManager.h"
#include "ps2/BiosTools.h"

#include "common/ARCADE.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SHA256.h"
#include "common/StringUtil.h"
#include "common/ZipHelpers.h"

#include "fmt/format.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>

namespace
{
	using namespace ArcadeSetup;

	constexpr const char* BOOT_ZIP_NAME = "proverb.zip";
	constexpr const char* BOOT_INFO_NAME = "proverb.json";
	// The GitHub reply for a release is a few kilobytes of JSON.
	constexpr size_t MAX_RELEASE_JSON_BYTES = 1 << 20;
	// A boot program is about 25 KB.
	constexpr u64 MAX_BOOT_PROGRAM_BYTES = 4ull << 20;
	// BiosTools.cpp's MIN_ARCADE_BIOS_SIZE and MAX_BIOS_SIZE: the boards' BIOS is a 2 MB flash chip.
	constexpr s64 MIN_ARCADE_BIOS_BYTES = 2ll << 20;
	constexpr s64 MAX_ARCADE_BIOS_BYTES = 8ll << 20;
	// How long a game's dongle may get when unpacked from a .gz (the biggest card the core makes is 66 MB).
	constexpr u64 MAX_DONGLE_BYTES = Arcade::MAX_CARD_BYTES + (8ull << 20);

	std::mutex s_install_mutex; // One download or install of the boot files at a time.
	std::mutex s_prepare_mutex; // Prepare() copies files and keeps notes; two at once would trip over each other.

	std::string BaseName(std::string_view path)
	{
		return std::string(Path::GetFileName(path));
	}

	bool EqualNoCase(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && StringUtil::Strncasecmp(a.data(), b.data(), a.size()) == 0;
	}

	// ----------------------------------------------------------------------------------------------
	// Files
	// ----------------------------------------------------------------------------------------------

	// [path] if there is a file there, else the file in its folder that differs from it only in case (names in an
	// .acgame are often typed by hand, and Linux cares), else empty.
	std::string ResolveExisting(const std::string& path)
	{
		if (path.empty())
			return {};
		if (FileSystem::FileExists(path.c_str()))
			return path;

		const std::string name = BaseName(path);
		FileSystem::FindResultsArray results;
		if (!FileSystem::FindFiles(std::string(Path::GetDirectory(path)).c_str(), "*",
				FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &results))
		{
			return {};
		}
		for (const FILESYSTEM_FIND_DATA& file : results)
		{
			if (EqualNoCase(BaseName(file.FileName), name))
				return file.FileName;
		}
		return {};
	}

	bool HasContent(const std::string& path)
	{
		return FileSystem::GetPathFileSize(path.c_str()) > 0;
	}

	bool IsGzName(std::string_view name)
	{
		return StringUtil::EndsWithNoCase(name, ".gz");
	}

	// Feeds the bytes of the file to [sink], gunzipped first when [unpack], until the file ends or the sink returns
	// false. False when the file cannot be read or is not valid gzip data.
	bool StreamFile(const std::string& path, bool unpack, const std::function<bool(const u8*, size_t)>& sink)
	{
		const auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
		if (!fp)
			return false;

		std::vector<u8> in(1 << 16);
		std::vector<u8> out(unpack ? (1 << 16) : 0);
		z_stream zs = {};
		if (unpack && inflateInit2(&zs, 15 + 16) != Z_OK) // 16: gzip, not zlib
			return false;

		bool ok = true;
		bool done = false;
		while (ok && !done)
		{
			const size_t got = std::fread(in.data(), 1, in.size(), fp.get());
			if (got == 0)
			{
				ok = std::feof(fp.get()) && !std::ferror(fp.get()) && !unpack; // gzip data cut short is an error
				break;
			}
			if (!unpack)
			{
				done = !sink(in.data(), got);
				continue;
			}

			zs.next_in = in.data();
			zs.avail_in = static_cast<uInt>(got);
			while (zs.avail_in > 0 && !done)
			{
				zs.next_out = out.data();
				zs.avail_out = static_cast<uInt>(out.size());
				const int status = inflate(&zs, Z_NO_FLUSH);
				if (status != Z_OK && status != Z_STREAM_END)
				{
					ok = false;
					break;
				}
				const size_t produced = out.size() - zs.avail_out;
				if (produced > 0 && !sink(out.data(), produced))
				{
					done = true;
					break;
				}
				if (status == Z_STREAM_END)
				{
					done = true;
					break;
				}
			}
		}

		if (unpack)
			inflateEnd(&zs);
		return ok;
	}

	std::vector<u8> HeadOf(const std::string& path, bool unpack, size_t count)
	{
		std::vector<u8> head;
		StreamFile(path, unpack, [&head, count](const u8* data, size_t size) {
			head.insert(head.end(), data, data + std::min(size, count - head.size()));
			return head.size() < count;
		});
		return head;
	}

	// The SHA-256 of the file (unpacked first when [unpack]), lower-case hex; nothing if it cannot be read.
	std::optional<std::string> Sha256Of(const std::string& path, bool unpack)
	{
		SHA256Digest sha;
		const bool ok = StreamFile(path, unpack, [&sha](const u8* data, size_t size) {
			sha.Update(data, size);
			return true;
		});
		if (!ok)
			return std::nullopt;
		return sha.FinalHex();
	}

	// Copies a file (unpacking a .gz when [unpack]) through a temporary file, so a failed copy leaves [target] as it
	// was. [expected_bytes], when not 0, is the size the result must have; [max_bytes] stops a packed file that
	// unpacks into something huge.
	bool CopyInto(const std::string& source, const std::string& target, bool unpack, u64 max_bytes = ~0ull, u64 expected_bytes = 0)
	{
		const std::string directory(Path::GetDirectory(target));
		if (!directory.empty() && !FileSystem::EnsureDirectoryExists(directory.c_str(), true))
			return false;

		const std::string temp = Path::Combine(directory, "." + BaseName(target) + ".copying");
		std::FILE* const raw = FileSystem::OpenCFile(temp.c_str(), "wb");
		if (!raw)
			return false;

		u64 written = 0;
		bool ok = StreamFile(source, unpack, [raw, max_bytes, &written](const u8* data, size_t size) {
			written += size;
			return written <= max_bytes && std::fwrite(data, 1, size, raw) == size;
		});
		ok = ok && written > 0 && written <= max_bytes && (expected_bytes == 0 || written == expected_bytes);
		ok = (std::fclose(raw) == 0) && ok;
		if (!ok || !FileSystem::RenamePath(temp.c_str(), target.c_str()))
		{
			FileSystem::DeleteFilePath(temp.c_str());
			return false;
		}
		return true;
	}

	// The regular files of a folder, not the hidden ones (a Mac's ._NM00010.ps2 is no dongle).
	struct Sibling
	{
		std::string name;
		std::string path;
		u64 size;
	};

	std::vector<Sibling> ListFiles(const std::string& directory)
	{
		std::vector<Sibling> files;
		FileSystem::FindResultsArray results;
		if (FileSystem::FindFiles(directory.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
		{
			for (const FILESYSTEM_FIND_DATA& file : results)
			{
				std::string name = BaseName(file.FileName);
				if (!name.empty() && name[0] != '.')
					files.push_back({std::move(name), file.FileName, static_cast<u64>(std::max<s64>(file.Size, 0))});
			}
		}
		std::sort(files.begin(), files.end(), [](const Sibling& a, const Sibling& b) { return a.name < b.name; });
		return files;
	}

	std::vector<std::string> ReadLines(const std::string& path)
	{
		std::vector<std::string> lines;
		const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
		if (!text.has_value())
			return lines;
		size_t start = 0;
		while (start < text->size())
		{
			size_t end = text->find('\n', start);
			if (end == std::string::npos)
				end = text->size();
			std::string line = text->substr(start, end - start);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			lines.push_back(std::move(line));
			start = end + 1;
		}
		return lines;
	}

	std::vector<u8> HexToBytes(std::string_view hex)
	{
		std::vector<u8> bytes;
		for (size_t i = 0; i + 1 < hex.size(); i += 2)
		{
			const auto value = StringUtil::FromChars<u8>(hex.substr(i, 2), 16);
			if (!value.has_value())
				return {};
			bytes.push_back(*value);
		}
		return bytes;
	}

	// ----------------------------------------------------------------------------------------------
	// Time
	// ----------------------------------------------------------------------------------------------

	s64 DaysFromCivil(s64 y, unsigned m, unsigned d)
	{
		y -= m <= 2;
		const s64 era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = static_cast<unsigned>(y - era * 400);
		const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return era * 146097 + static_cast<s64>(doe) - 719468;
	}

	// "2026-07-25T21:32:56Z" as GitHub writes it; 0 for anything else.
	std::time_t ParseUtcTime(const char* text)
	{
		int year, month, day, hour, minute, second;
		if (!text || std::sscanf(text, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &minute, &second) != 6 ||
			month < 1 || month > 12 || day < 1 || day > 31)
		{
			return 0;
		}
		return static_cast<std::time_t>(DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
										hour * 3600 + minute * 60 + second);
	}

	// ----------------------------------------------------------------------------------------------
	// The boot files
	// ----------------------------------------------------------------------------------------------

	std::string BootZipPath(const Folders& folders)
	{
		return Path::Combine(folders.arcade, BOOT_ZIP_NAME);
	}

	std::string BootInfoPath(const Folders& folders)
	{
		return Path::Combine(folders.arcade, BOOT_INFO_NAME);
	}

	// The game a zip entry is the boot program of: bin/NM#####/boot.elf under any folder, in any case
	// (the Android app's rule, (?:^|/)bin/(NM\d{5})/boot\.elf$). Empty for any other entry.
	std::string BootEntryGameId(std::string_view name)
	{
		const std::string lower = StringUtil::toLower(name);
		constexpr std::string_view tail = "/boot.elf";
		std::string_view rest = lower;
		if (!rest.ends_with(tail))
			return {};
		rest.remove_suffix(tail.size());
		if (rest.size() < 7)
			return {};
		const std::string id = StringUtil::toUpper(rest.substr(rest.size() - 7));
		rest.remove_suffix(7);
		if (!Arcade::IsGameId(id) || !rest.ends_with("bin/"))
			return {};
		rest.remove_suffix(4);
		if (!rest.empty() && !rest.ends_with('/'))
			return {};
		return id;
	}

	struct BootEntry
	{
		std::string game_id;
		zip_uint64_t index;
	};

	std::vector<BootEntry> ListBootEntries(zip_t* zip)
	{
		std::vector<BootEntry> entries;
		const zip_int64_t count = zip_get_num_entries(zip, 0);
		for (zip_int64_t i = 0; i < count; i++)
		{
			const char* name = zip_get_name(zip, static_cast<zip_uint64_t>(i), 0);
			if (!name)
				continue;
			std::string id = BootEntryGameId(name);
			if (id.empty() ||
				std::any_of(entries.begin(), entries.end(), [&id](const BootEntry& e) { return e.game_id == id; }))
			{
				continue;
			}
			entries.push_back({std::move(id), static_cast<zip_uint64_t>(i)});
		}
		return entries;
	}

	// The game IDs the zip at [path] has a boot program for; empty when it is no zip or has none.
	std::vector<std::string> BootGamesIn(const std::string& path)
	{
		std::vector<std::string> ids;
		zip_error_t ze = {};
		const auto zip = zip_open_managed(path.c_str(), ZIP_RDONLY, &ze);
		if (!zip)
			return ids;
		for (const BootEntry& entry : ListBootEntries(zip.get()))
			ids.push_back(entry.game_id);
		std::sort(ids.begin(), ids.end());
		return ids;
	}

	// What is known about the stored zip besides the zip itself (proverb.json).
	struct StoredInfo
	{
		std::string sha256;
		u64 size = 0;
		std::string release_name;
		std::time_t release_time = 0;
	};

	std::optional<StoredInfo> ReadStoredInfo(const Folders& folders)
	{
		const std::optional<std::string> text = FileSystem::ReadFileToString(BootInfoPath(folders).c_str());
		rapidjson::Document doc;
		if (!text.has_value() || !Catalog::ParseJson(*text, &doc) || !doc.IsObject())
			return std::nullopt;

		StoredInfo info;
		if (doc.HasMember("sha256") && doc["sha256"].IsString())
			info.sha256 = doc["sha256"].GetString();
		if (doc.HasMember("size") && doc["size"].IsUint64())
			info.size = doc["size"].GetUint64();
		if (doc.HasMember("releaseName") && doc["releaseName"].IsString())
			info.release_name = doc["releaseName"].GetString();
		if (doc.HasMember("releaseTime") && doc["releaseTime"].IsInt64())
			info.release_time = static_cast<std::time_t>(doc["releaseTime"].GetInt64());
		return info;
	}

	void WriteStoredInfo(const Folders& folders, const StoredInfo& info)
	{
		rapidjson::Document doc(rapidjson::kObjectType);
		auto& alloc = doc.GetAllocator();
		doc.AddMember("sha256", rapidjson::Value(info.sha256.c_str(), alloc), alloc);
		doc.AddMember("size", rapidjson::Value(static_cast<uint64_t>(info.size)), alloc);
		doc.AddMember("releaseName", rapidjson::Value(info.release_name.c_str(), alloc), alloc);
		doc.AddMember("releaseTime", rapidjson::Value(static_cast<int64_t>(info.release_time)), alloc);
		Catalog::WriteJsonFileAtomic(BootInfoPath(folders), doc);
	}

	BootFilesStatus ReadBootFilesStatus(const Folders& folders)
	{
		BootFilesStatus status;
		status.path = BootZipPath(folders);

		FILESYSTEM_STAT_DATA sd;
		if (!FileSystem::StatFile(status.path.c_str(), &sd))
			return status;
		status.game_ids = BootGamesIn(status.path);
		if (status.game_ids.empty())
			return status;

		status.installed = true;
		status.size_bytes = static_cast<u64>(sd.Size);
		status.installed_at = sd.ModificationTime;

		// What was noted at the download counts while it still describes this file.
		const std::optional<StoredInfo> stored = ReadStoredInfo(folders);
		if (stored.has_value() && stored->size == status.size_bytes && !stored->sha256.empty())
		{
			status.sha256 = stored->sha256;
			status.release_name = stored->release_name;
			status.release_time = stored->release_time;
		}
		else
		{
			SHA256Digest::HashFile(status.path.c_str(), &status.sha256);
		}
		return status;
	}

	// What GitHub says about the release's zip.
	struct ReleaseInfo
	{
		std::string name;
		std::time_t time = 0; // When the zip was built.
		u64 size = 0;
		std::string sha256; // Lower-case hex; empty when GitHub gave no digest.
	};

	bool ParseReleaseJson(std::string_view body, ReleaseInfo* release, std::string* error)
	{
		rapidjson::Document doc;
		std::string parse_error;
		if (!Catalog::ParseJson(body, &doc, &parse_error) || !doc.IsObject())
		{
			*error = "GitHub's answer about the boot files was not understood.";
			return false;
		}

		if (doc.HasMember("name") && doc["name"].IsString())
			release->name = doc["name"].GetString();
		std::time_t published = 0;
		if (doc.HasMember("published_at") && doc["published_at"].IsString())
			published = ParseUtcTime(doc["published_at"].GetString());

		if (doc.HasMember("assets") && doc["assets"].IsArray())
		{
			for (const rapidjson::Value& asset : doc["assets"].GetArray())
			{
				if (!asset.IsObject() || !asset.HasMember("name") || !asset["name"].IsString() ||
					std::string_view(asset["name"].GetString()) != BOOT_ZIP_NAME)
				{
					continue;
				}
				if (asset.HasMember("size") && asset["size"].IsUint64())
					release->size = asset["size"].GetUint64();
				if (asset.HasMember("updated_at") && asset["updated_at"].IsString())
					release->time = ParseUtcTime(asset["updated_at"].GetString());
				if (asset.HasMember("digest") && asset["digest"].IsString())
				{
					const std::string_view digest = asset["digest"].GetString();
					if (digest.starts_with("sha256:"))
						release->sha256 = StringUtil::toLower(digest.substr(7));
				}
				if (release->time == 0)
					release->time = published;
				return true;
			}
		}
		*error = "GitHub's release has no boot files in it.";
		return false;
	}

	// Asks GitHub about the release. False with [error] set when it cannot say.
	bool FetchReleaseInfo(Catalog::Fetcher& fetcher, const Catalog::CancelToken& cancel, ReleaseInfo* release, std::string* error, bool* cancelled)
	{
		Catalog::FetchOptions options;
		options.cancel = cancel;
		options.headers.emplace_back("Accept", "application/vnd.github+json");
		const Catalog::FetchResponse response = fetcher.Get(BOOT_RELEASE_URL, MAX_RELEASE_JSON_BYTES, options);
		if (response.result == Catalog::FetchResult::Cancelled)
		{
			*cancelled = true;
			*error = "Cancelled.";
			return false;
		}
		if (response.result != Catalog::FetchResult::Ok)
		{
			*error = fmt::format("GitHub could not be reached: {}", response.error);
			return false;
		}
		if (!response.IsSuccess())
		{
			*error = fmt::format("GitHub answered HTTP {}.", response.status);
			return false;
		}
		return ParseReleaseJson(response.body, release, error);
	}

	// Moves a finished zip into place, as the stored boot files, if it holds boot programs.
	InstallResult CommitBootFiles(const Folders& folders, const std::string& zip_file, const std::string& sha256,
		const std::string& release_name, std::time_t release_time, bool delete_source)
	{
		InstallResult result;
		result.status = ReadBootFilesStatus(folders);

		if (BootGamesIn(zip_file).empty())
		{
			if (delete_source)
				FileSystem::DeleteFilePath(zip_file.c_str());
			result.error = TRANSLATE_STR("Arcade", "That is not the arcade boot files: it holds no boot programs.");
			return result;
		}

		const std::string target = BootZipPath(folders);
		Error error;
		if (!FileSystem::EnsureDirectoryExists(folders.arcade.c_str(), true, &error))
		{
			result.error = error.GetDescription();
			return result;
		}
		if (!FileSystem::RenamePath(zip_file.c_str(), target.c_str(), &error))
		{
			if (delete_source)
				FileSystem::DeleteFilePath(zip_file.c_str());
			result.error = fmt::format(TRANSLATE_FS("Arcade", "The boot files could not be stored: {}"), error.GetDescription());
			return result;
		}

		StoredInfo info;
		info.sha256 = sha256;
		info.size = static_cast<u64>(std::max<s64>(FileSystem::GetPathFileSize(target.c_str()), 0));
		info.release_name = release_name;
		info.release_time = release_time;
		WriteStoredInfo(folders, info);

		result.ok = true;
		result.status = ReadBootFilesStatus(folders);
		return result;
	}

	// ----------------------------------------------------------------------------------------------
	// The Conquest card and the dongle
	// ----------------------------------------------------------------------------------------------

	bool IsConquestCardFile(const std::string& path, bool unpack)
	{
		return Arcade::IsConquestCardHeader(HeadOf(path, unpack, Arcade::ConquestHeaderBytes()));
	}

	// Whether [path] in the memory cards folder is Soul Calibur II's Conquest card and not the dongle of the game
	// [id]: only that game has one.
	bool IsConquestCardIn(const std::string& id, const std::string& path)
	{
		return id == Arcade::CONQUEST_GAME && FileSystem::FileExists(path.c_str()) && IsConquestCardFile(path, false);
	}

	// Whether [path] is still exactly the uncleared Conquest card the 2.8.2 Android app shipped. The probe tells
	// almost every other card apart without hashing all 8 MB.
	bool IsUnclearedConquestCard(const Folders::ConquestCard& spec, const std::string& path)
	{
		if (static_cast<u64>(std::max<s64>(FileSystem::GetPathFileSize(path.c_str()), 0)) != spec.bytes)
			return false;

		const std::vector<u8> probe = HexToBytes(spec.probe_hex);
		if (!probe.empty())
		{
			const auto fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
			std::vector<u8> found(probe.size());
			if (!fp || FileSystem::FSeek64(fp.get(), static_cast<s64>(spec.probe_offset), SEEK_SET) != 0 ||
				std::fread(found.data(), 1, found.size(), fp.get()) != found.size() || found != probe)
			{
				return false;
			}
		}
		return Sha256Of(path, false) == spec.uncleared_sha256;
	}

	bool InstallClearedConquestCard(const Folders& folders, const std::string& target)
	{
		return CopyInto(folders.conquest_card_gz, target, true, folders.conquest.bytes, folders.conquest.bytes);
	}

	// Soul Calibur II's Conquest card, which the game reads in slot 2: NM00007.conquestcard in the memory cards
	// folder. It is put there once and kept from then on, as the game writes its Conquest mode to it. It is the
	// first of: the card already there; a Conquest card named as the dongle (NM00007.ps2), moved; the player's own
	// beside the game's image ([siblings], loose games only); else the cleared card that comes with the app. The
	// one exception to keeping it: a card still exactly the uncleared one the Android app once shipped, which
	// the game never wrote, is swapped for the cleared one. Its name there, or empty when none could be put there:
	// the game then starts without one.
	std::string EnsureConquestCard(const Folders& folders, const std::vector<Sibling>* siblings, const std::string& folder_id)
	{
		const std::string id = Arcade::CONQUEST_GAME;
		const std::string target = Path::Combine(folders.memory_cards, Arcade::CONQUEST_CARD_NAME);
		FileSystem::EnsureDirectoryExists(folders.memory_cards.c_str(), true);

		if (HasContent(target))
		{
			if (IsUnclearedConquestCard(folders.conquest, target) && InstallClearedConquestCard(folders, target))
				Console.WriteLn("(Arcade) %s was the uncleared Conquest card: now the cleared one", Arcade::CONQUEST_CARD_NAME);
			return Arcade::CONQUEST_CARD_NAME;
		}

		// A Conquest card somebody put in as the dongle would be written over by the real dongle's copy.
		const std::string stale = Path::Combine(folders.memory_cards, id + ".ps2");
		if (static_cast<u64>(std::max<s64>(FileSystem::GetPathFileSize(stale.c_str()), 0)) == folders.conquest.bytes &&
			IsConquestCardFile(stale, false) && FileSystem::RenamePath(stale.c_str(), target.c_str()))
		{
			Console.WriteLn("(Arcade) %s.ps2 was the Conquest card, not the dongle: now %s", id.c_str(), Arcade::CONQUEST_CARD_NAME);
			return Arcade::CONQUEST_CARD_NAME;
		}

		if (siblings)
		{
			for (const Sibling& sibling : *siblings)
			{
				std::string found = Arcade::FindGameIdInName(sibling.name);
				if (found.empty())
					found = folder_id;
				if (found != id || Arcade::ClassifyFile(sibling.name, sibling.size) != Arcade::FileKind::Card)
					continue;
				const bool packed = IsGzName(sibling.name);
				if (!IsConquestCardFile(sibling.path, packed))
					continue;
				if (CopyInto(sibling.path, target, packed, folders.conquest.bytes, folders.conquest.bytes))
				{
					Console.WriteLn("(Arcade) copied %s into the memory cards folder as %s", sibling.name.c_str(), Arcade::CONQUEST_CARD_NAME);
					return Arcade::CONQUEST_CARD_NAME;
				}
				// Unreadable, or dumped without its spare bytes, which hold the game's checksums: not one it reads.
				Console.WriteLn("(Arcade) %s could not be used as the Conquest card", sibling.name.c_str());
			}
		}

		if (InstallClearedConquestCard(folders, target))
		{
			Console.WriteLn("(Arcade) put the cleared Conquest card in the memory cards folder as %s", Arcade::CONQUEST_CARD_NAME);
			return Arcade::CONQUEST_CARD_NAME;
		}
		FileSystem::DeleteFilePath(target.c_str());
		Console.Warning("(Arcade) no Conquest card could be put in place; the game starts without one");
		return {};
	}

	// The dongle candidates of a loose game among the files beside its image: a .ps2 first, then a .bin, then a
	// packed one. The likelier a file is the dongle, the earlier. Never Soul Calibur II's Conquest card, a card
	// file as well.
	std::vector<Sibling> DongleCandidates(const std::string& id, const std::string& folder_id, const std::vector<Sibling>& siblings)
	{
		std::vector<Sibling> candidates;
		for (const Sibling& sibling : siblings)
		{
			std::string found = Arcade::FindGameIdInName(sibling.name);
			if (found.empty())
				found = folder_id;
			if (found != id || Arcade::ClassifyFile(sibling.name, sibling.size) != Arcade::FileKind::Card)
				continue;
			if (id == Arcade::CONQUEST_GAME && IsConquestCardFile(sibling.path, IsGzName(sibling.name)))
				continue;
			candidates.push_back(sibling);
		}
		const auto rank = [](const Sibling& s) {
			return StringUtil::EndsWithNoCase(s.name, ".ps2") ? 0 : (IsGzName(s.name) ? 2 : 1);
		};
		std::stable_sort(candidates.begin(), candidates.end(), [&rank](const Sibling& a, const Sibling& b) { return rank(a) < rank(b); });
		return candidates;
	}

	// The dongle of the loose game [id] that is in the memory cards folder already (from an earlier launch, or put
	// there by hand): <id>.ps2 or <id>.bin. Empty for none.
	std::string DongleInCards(const Folders& folders, const std::string& id)
	{
		for (const std::string& name : {id + ".ps2", id + ".bin"})
		{
			const std::string path = Path::Combine(folders.memory_cards, name);
			if (HasContent(path) && !IsConquestCardIn(id, path))
				return name;
		}
		return {};
	}

	// The dongle of the loose game [id], in the memory cards folder: already there, else copied there from beside
	// its image (unpacked, if it is a .gz). It is named after the game and what it holds (DongleName). Empty when
	// there is none.
	//
	// The copy there is kept, as the game writes to it, until the file it was copied from (dongle.from) is gone
	// from beside the image and a different dongle is there: a key swapped for another (Soul Calibur II's
	// Japanese one for its export one) then replaces it. The same key renamed keeps the copy.
	std::string PrepareLooseDongle(const Folders& folders, const std::string& id, const std::string& folder_id, const std::vector<Sibling>& siblings)
	{
		FileSystem::EnsureDirectoryExists(folders.memory_cards.c_str(), true);
		const std::vector<Sibling> candidates = DongleCandidates(id, folder_id, siblings);
		const std::string from_path = Path::Combine(Path::Combine(folders.arcade, id), "dongle.from");
		const std::string have = DongleInCards(folders, id);

		const auto content_of = [](const Sibling& s) { return Sha256Of(s.path, IsGzName(s.name)).value_or(std::string()); };
		const auto write_from = [&from_path](const Sibling& s, const std::string& content) {
			Catalog::WriteFileAtomic(from_path, fmt::format("{}\n{}\n{}\n", s.name, s.size, content));
		};

		if (!have.empty())
		{
			if (candidates.empty())
				return have;
			const Sibling& first = candidates.front();
			std::vector<std::string> made = ReadLines(from_path);
			// Copied before anything was noted, or put there by hand: taken to come from the first one there.
			if (made.size() < 3)
			{
				write_from(first, content_of(first));
				return have;
			}
			const bool still_there = std::any_of(candidates.begin(), candidates.end(),
				[&made](const Sibling& s) { return s.name == made[0] && std::to_string(s.size) == made[1]; });
			if (still_there)
				return have;
			const std::string content = content_of(first);
			if (!content.empty() && content == made[2])
			{
				write_from(first, content);
				return have;
			}
			Console.WriteLn("(Arcade) %s is no longer beside the image: its dongle is now %s", made[0].c_str(), first.name.c_str());
		}

		for (const Sibling& candidate : candidates)
		{
			const std::string temp = Path::Combine(folders.memory_cards, "." + id + ".dongle");
			const bool packed = IsGzName(candidate.name);
			if (!CopyInto(candidate.path, temp, packed, MAX_DONGLE_BYTES))
				continue;

			const u64 bytes = static_cast<u64>(std::max<s64>(FileSystem::GetPathFileSize(temp.c_str()), 0));
			const std::string content = Sha256Of(temp, false).value_or(std::string());
			const std::string name = Arcade::DongleName(id, bytes);
			if (bytes >= Arcade::MIN_CARD_BYTES && bytes <= Arcade::MAX_CARD_BYTES &&
				FileSystem::RenamePath(temp.c_str(), Path::Combine(folders.memory_cards, name).c_str()))
			{
				if (!have.empty() && have != name)
					FileSystem::DeleteFilePath(Path::Combine(folders.memory_cards, have).c_str());
				Catalog::WriteFileAtomic(from_path, fmt::format("{}\n{}\n{}\n", candidate.name, candidate.size, content));
				Console.WriteLn("(Arcade) copied %s into the memory cards folder as %s", candidate.name.c_str(), name.c_str());
				return name;
			}
			FileSystem::DeleteFilePath(temp.c_str());
		}
		return have;
	}

	// ----------------------------------------------------------------------------------------------
	// A game, looked at
	// ----------------------------------------------------------------------------------------------

	std::string TitleOf(const std::string& id)
	{
		const GameDatabaseSchema::GameEntry* const entry = GameDatabase::findGame(id);
		return (entry && !entry->name.empty()) ? entry->name : id;
	}

	void CheckBios(const std::string& id, MissingReport* report)
	{
		std::string needs;
		if (!FindBiosFor(id, &needs).empty())
			return;
		if (needs.empty())
			report->parts.push_back({PartKind::ArcadeBios, {}});
		else
			report->parts.push_back({PartKind::BoardBios, needs});
	}

	// What Missing() and Prepare() both work from: the game's files, found, and what is not there.
	struct Inspection
	{
		bool acgame = false;
		std::optional<Arcade::AcGameInfo> info; // .acgame only
		std::string id;
		std::string folder_id; // Loose only: the game ID of the folder the image is in, else empty.
		std::string image; // Found, empty when not.
		std::string elf; // .acgame only: the boot program beside the game, empty when not.
		std::vector<Sibling> siblings; // Loose only: the files beside the image.
		MissingReport report;
	};

	Inspection Inspect(const Folders& folders, const Service& service, const std::string& path)
	{
		Inspection in;
		in.acgame = StringUtil::EndsWithNoCase(path, ".acgame");
		MissingReport& report = in.report;

		if (in.acgame)
		{
			std::string why;
			in.info = Arcade::ReadAcGameInfo(path, &why);
			if (!in.info.has_value())
			{
				report.parts.push_back({PartKind::Unreadable, std::move(why)});
				return in;
			}
			const Arcade::AcGameInfo& game = *in.info;
			in.id = report.game_id = game.gameid;
			report.title = game.name.empty() ? TitleOf(in.id) : game.name;
			report.attract_only = IsAttractOnly(in.id);

			CheckBios(in.id, &report);
			in.elf = ResolveExisting(game.elf_path);
			if (in.elf.empty() && !service.HasBootProgram(in.id))
				report.parts.push_back({PartKind::BootProgram, BaseName(game.elf_path)});
			in.image = ResolveExisting(game.media_path);
			if (in.image.empty())
				report.parts.push_back({PartKind::Image, BaseName(game.media_path)});

			// The dongle and the card are in the memory cards folder, or beside the .acgame to be copied there.
			const auto beside = [&game](const std::string& name) {
				return !ResolveExisting(Path::Combine(game.base_dir, BaseName(name))).empty() ||
				       !ResolveExisting(Path::Combine(game.dir, BaseName(name))).empty();
			};
			if (!HasContent(Path::Combine(folders.memory_cards, game.dongle)) && !beside(game.dongle))
				report.parts.push_back({PartKind::Dongle, game.dongle});
			if (!game.card.empty() && !HasContent(Path::Combine(folders.memory_cards, game.card)) && !beside(game.card))
				report.parts.push_back({PartKind::Card, game.card});
			return in;
		}

		// A loose image.
		const std::string name = BaseName(path);
		const std::string folder(Path::GetDirectory(path));
		in.folder_id = Arcade::FindGameIdInName(BaseName(folder));
		in.id = Arcade::FindGameIdInName(name);
		if (in.id.empty())
			in.id = in.folder_id;
		const s64 size = FileSystem::GetPathFileSize(path.c_str());
		const Arcade::FileKind kind = Arcade::ClassifyFile(name, static_cast<u64>(std::max<s64>(size, 0)));
		if (in.id.empty() || size < 0 || (kind != Arcade::FileKind::Image && kind != Arcade::FileKind::PackedImage))
		{
			report.parts.push_back({PartKind::Unreadable, {}});
			return in;
		}

		report.game_id = in.id;
		report.title = TitleOf(in.id);
		report.attract_only = IsAttractOnly(in.id);
		in.image = path;
		in.siblings = ListFiles(folder);

		CheckBios(in.id, &report);
		if (!service.HasBootProgram(in.id))
			report.parts.push_back({PartKind::BootProgram, "boot.elf"});
		if (kind == Arcade::FileKind::PackedImage)
			report.parts.push_back({PartKind::PackedImage, name});
		if (DongleInCards(folders, in.id).empty() && DongleCandidates(in.id, in.folder_id, in.siblings).empty())
			report.parts.push_back({PartKind::Dongle, in.id + ".ps2"});
		return in;
	}

	std::string JoinExplanations(const MissingReport& report)
	{
		std::string text;
		for (const MissingPart& part : report.parts)
		{
			if (!text.empty())
				text += ' ';
			text += part.Explain();
		}
		return text;
	}
} // namespace

// --------------------------------------------------------------------------------------------------
// Public
// --------------------------------------------------------------------------------------------------

ArcadeSetup::Folders ArcadeSetup::Folders::FromEmuFolders()
{
	Folders folders;
	folders.arcade = Path::Combine(EmuFolders::DataRoot, "arcade");
	folders.memory_cards = EmuFolders::MemoryCards;
	folders.conquest_card_gz = EmuFolders::GetOverridableResourcePath("arcade/NM00007.conquestcard.gzip");
	return folders;
}

std::string ArcadeSetup::BiosStatus::Summary() const
{
	std::vector<std::string> names;
	for (const BiosDump& dump : dumps)
	{
		const std::string& name = dump.board.empty() ? dump.file_name : dump.board;
		if (std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(name);
	}
	std::sort(names.begin(), names.end());
	return StringUtil::JoinString(names.begin(), names.end(), ", ");
}

std::string ArcadeSetup::FindBiosFor(const std::string& game_id, std::string* needs)
{
	// The old [Filenames] ArcadeBIOS pick only chooses between two dumps of one board; neither matters here.
	return FindArcadeBiosFor(game_id, std::string(), needs);
}

bool ArcadeSetup::IsAttractOnly(std::string_view game_id)
{
	return game_id == "NM00014" || game_id == "NM00020" || game_id == "NM00022";
}

std::string ArcadeSetup::MissingPart::Short() const
{
	switch (kind)
	{
		case PartKind::ArcadeBios:
			return TRANSLATE_STR("Arcade", "arcade BIOS");
		case PartKind::BoardBios:
			return fmt::format(TRANSLATE_FS("Arcade", "{} BIOS"), detail);
		case PartKind::BootProgram:
			return TRANSLATE_STR("Arcade", "boot file");
		case PartKind::Image:
			return TRANSLATE_STR("Arcade", "game image");
		case PartKind::PackedImage:
			return TRANSLATE_STR("Arcade", "an unpacked image");
		case PartKind::Dongle:
			return TRANSLATE_STR("Arcade", "dongle");
		case PartKind::Card:
			return TRANSLATE_STR("Arcade", "memory card");
		case PartKind::Unreadable:
		default:
			return TRANSLATE_STR("Arcade", "readable game file");
	}
}

std::string ArcadeSetup::MissingPart::Explain() const
{
	switch (kind)
	{
		case PartKind::ArcadeBios:
			return TRANSLATE_STR("Arcade", "Arcade games need the BIOS of a Namco System 246 or 256 board. Put it in the BIOS folder: it is used for arcade games only, and your console BIOS stays as it is.");
		case PartKind::BoardBios:
			return fmt::format(TRANSLATE_FS("Arcade", "This game does not run on the arcade BIOS you have. Put the {} BIOS in the BIOS folder too: each arcade game starts with a BIOS it runs on."), detail);
		case PartKind::BootProgram:
			return fmt::format(TRANSLATE_FS("Arcade", "The boot program {} was not found. Download the arcade boot files on the Arcade Setup page, or put it in the game's folder."), detail);
		case PartKind::Image:
			return fmt::format(TRANSLATE_FS("Arcade", "The game image {} was not found. It goes in the arcade game's folder, the one named after its game ID unless the .acgame says otherwise."), detail);
		case PartKind::PackedImage:
			return fmt::format(TRANSLATE_FS("Arcade", "{} is packed. Unpack it to an .iso, or convert it to a .chd, and keep it where it is."), detail);
		case PartKind::Dongle:
			return fmt::format(TRANSLATE_FS("Arcade", "The security dongle {} was not found. Put it in the memory cards folder, or next to the game's image or .acgame."), detail);
		case PartKind::Card:
			return fmt::format(TRANSLATE_FS("Arcade", "The memory card {} was not found. Put it in the memory cards folder, or next to the .acgame."), detail);
		case PartKind::Unreadable:
		default:
			if (!detail.empty())
				return fmt::format(TRANSLATE_FS("Arcade", "This arcade game file cannot be used. {}"), detail);
			return TRANSLATE_STR("Arcade", "This arcade game file could not be read, or it has no game ID. An .acgame needs a gameid line in its [game] section: NM followed by five digits; an image needs the ID in its name.");
	}
}

std::string ArcadeSetup::MissingReport::Summary() const
{
	std::string text;
	for (const MissingPart& part : parts)
	{
		if (!text.empty())
			text += ", ";
		text += part.Short();
	}
	return text;
}

void ArcadeSetup::PrepareResult::ApplyTo(VMBootParameters* params) const
{
	params->filename = acgame;
	// A disc source would take the boot away from the arcade branch of VMManager::Initialize.
	params->source_type.reset();
	params->elf_override.clear();
	params->arcade_elf = elf;
	params->arcade_media = media;
	params->arcade_sram = sram;
}

ArcadeSetup::Service::Service(Folders folders)
	: m_folders(std::move(folders))
{
}

// ---- The boot files ----

ArcadeSetup::BootFilesStatus ArcadeSetup::Service::GetBootFilesStatus() const
{
	return ReadBootFilesStatus(m_folders);
}

bool ArcadeSetup::Service::HasBootProgram(std::string_view game_id) const
{
	const std::vector<std::string> ids = BootGamesIn(BootZipPath(m_folders));
	return std::find(ids.begin(), ids.end(), StringUtil::toUpper(game_id)) != ids.end();
}

bool ArcadeSetup::Service::ExtractBootProgram(std::string_view game_id, const std::string& dest_path, std::string* error) const
{
	const auto fail = [error](std::string message) {
		if (error)
			*error = std::move(message);
		return false;
	};

	zip_error_t ze = {};
	const auto zip = zip_open_managed(BootZipPath(m_folders).c_str(), ZIP_RDONLY, &ze);
	if (!zip)
		return fail(TRANSLATE_STR("Arcade", "The arcade boot files are not downloaded."));

	const std::string wanted = StringUtil::toUpper(game_id);
	for (const BootEntry& entry : ListBootEntries(zip.get()))
	{
		if (entry.game_id != wanted)
			continue;

		zip_stat_t st;
		zip_stat_init(&st);
		if (zip_stat_index(zip.get(), entry.index, 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE) || st.size > MAX_BOOT_PROGRAM_BYTES)
			return fail(TRANSLATE_STR("Arcade", "The boot program in the boot files is not usable."));

		const auto file = zip_fopen_index(zip.get(), entry.index, 0);
		if (!file)
			return fail(TRANSLATE_STR("Arcade", "The boot program in the boot files is not usable."));
		std::vector<u8> data(st.size);
		if (!data.empty() && zip_fread(file, data.data(), data.size()) != static_cast<zip_int64_t>(data.size()))
		{
			zip_fclose(file);
			return fail(TRANSLATE_STR("Arcade", "The boot program in the boot files is not usable."));
		}
		zip_fclose(file);

		Error write_error;
		if (!Catalog::WriteFileAtomic(dest_path, data.data(), data.size(), &write_error))
			return fail(write_error.GetDescription());
		return true;
	}
	return fail(fmt::format(TRANSLATE_FS("Arcade", "The boot files have no boot program for {}."), wanted));
}

ArcadeSetup::InstallResult ArcadeSetup::Service::DownloadBootFiles(Catalog::Fetcher& fetcher, const DownloadOptions& options)
{
	InstallResult result;
	result.status = ReadBootFilesStatus(m_folders);

	std::unique_lock lock(s_install_mutex, std::try_to_lock);
	if (!lock.owns_lock())
	{
		result.error = TRANSLATE_STR("Arcade", "The boot files are already being downloaded.");
		return result;
	}

	// What GitHub calls the release now, for the version shown and to check the download against. When it cannot
	// say (offline, rate limited) the download goes ahead without.
	ReleaseInfo release;
	std::string release_error;
	bool cancelled = false;
	const bool have_release = FetchReleaseInfo(fetcher, options.cancel, &release, &release_error, &cancelled);
	if (cancelled || options.cancel.IsCancelled())
	{
		result.cancelled = true;
		result.error = TRANSLATE_STR("Arcade", "Cancelled.");
		return result;
	}

	Error error;
	if (!FileSystem::EnsureDirectoryExists(m_folders.arcade.c_str(), true, &error))
	{
		result.error = error.GetDescription();
		return result;
	}
	const std::string part = BootZipPath(m_folders) + ".part";
	FileSystem::DeleteFilePath(part.c_str());

	SHA256Digest sha;
	Catalog::FetchOptions fetch;
	fetch.cancel = options.cancel;
	fetch.on_progress = options.on_progress;
	fetch.max_file_bytes = MAX_BOOT_FILES_BYTES;
	fetch.on_data = [&sha](const u8* data, size_t size) { sha.Update(data, size); };
	const Catalog::FetchResponse response = fetcher.GetToFile(BOOT_FILES_URL, part, fetch);
	if (response.result != Catalog::FetchResult::Ok || !response.IsSuccess())
	{
		FileSystem::DeleteFilePath(part.c_str());
		if (response.result == Catalog::FetchResult::Cancelled)
		{
			result.cancelled = true;
			result.error = TRANSLATE_STR("Arcade", "Cancelled.");
		}
		else if (response.result == Catalog::FetchResult::Ok)
		{
			result.error = fmt::format(TRANSLATE_FS("Arcade", "The boot files could not be downloaded: GitHub answered HTTP {}."), response.status);
		}
		else
		{
			result.error = fmt::format(TRANSLATE_FS("Arcade", "The boot files could not be downloaded: {}"), response.error);
		}
		return result;
	}

	const std::string hash = sha.FinalHex();
	// When GitHub published a digest for the zip, the file that arrived must be that one: it was damaged or
	// swapped on the way, or the release was republished between the two requests (try again then).
	if (have_release && !release.sha256.empty() && release.sha256 != hash)
	{
		FileSystem::DeleteFilePath(part.c_str());
		result.error = TRANSLATE_STR("Arcade", "The boot files that arrived do not match the checksum GitHub published for them, so they were not installed. Try again.");
		return result;
	}
	return CommitBootFiles(m_folders, part, hash, have_release ? release.name : std::string(), have_release ? release.time : 0, true);
}

ArcadeSetup::InstallResult ArcadeSetup::Service::InstallBootFilesFromFile(const std::string& zip_path)
{
	InstallResult result;
	result.status = ReadBootFilesStatus(m_folders);

	std::unique_lock lock(s_install_mutex, std::try_to_lock);
	if (!lock.owns_lock())
	{
		result.error = TRANSLATE_STR("Arcade", "The boot files are already being installed.");
		return result;
	}

	if (BootGamesIn(zip_path).empty())
	{
		result.error = TRANSLATE_STR("Arcade", "That is not the arcade boot files: it holds no boot programs.");
		return result;
	}

	const std::string part = BootZipPath(m_folders) + ".part";
	if (!CopyInto(zip_path, part, false, MAX_BOOT_FILES_BYTES))
	{
		result.error = TRANSLATE_STR("Arcade", "The boot files could not be read.");
		return result;
	}
	std::string hash;
	SHA256Digest::HashFile(part.c_str(), &hash);
	return CommitBootFiles(m_folders, part, hash, std::string(), 0, true);
}

ArcadeSetup::UpdateCheck ArcadeSetup::Service::CheckForBootFilesUpdate(Catalog::Fetcher& fetcher, const Catalog::CancelToken& cancel) const
{
	UpdateCheck check;
	ReleaseInfo release;
	bool cancelled = false;
	if (!FetchReleaseInfo(fetcher, cancel, &release, &check.error, &cancelled))
		return check; // Failed

	check.release_name = release.name;
	check.release_time = release.time;
	check.size_bytes = release.size;

	const BootFilesStatus status = ReadBootFilesStatus(m_folders);
	if (!status.installed)
		check.state = UpdateCheck::State::NotInstalled;
	else if (!release.sha256.empty())
		check.state = (release.sha256 == status.sha256) ? UpdateCheck::State::UpToDate : UpdateCheck::State::UpdateAvailable;
	else if (release.size != 0 && release.size != status.size_bytes)
		check.state = UpdateCheck::State::UpdateAvailable;
	else if (release.time != 0 && status.release_time != 0 && release.time > status.release_time)
		check.state = UpdateCheck::State::UpdateAvailable;
	else
		check.state = UpdateCheck::State::UpToDate;
	return check;
}

// ---- The BIOS ----

ArcadeSetup::BiosStatus ArcadeSetup::Service::GetBiosStatus() const
{
	BiosStatus status;
	FileSystem::FindResultsArray results;
	if (!FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
		return status;

	for (const FILESYSTEM_FIND_DATA& file : results)
	{
		if (file.Size < MIN_ARCADE_BIOS_BYTES || file.Size > MAX_ARCADE_BIOS_BYTES || !IsArcadeBIOS(file.FileName.c_str()))
			continue;

		// The core's description is the zone, the board and the EXTINFO serial: "COH-H   System 256 20040519-145634".
		BiosDump dump;
		dump.file_name = BaseName(file.FileName);
		u32 version, region;
		std::string description, zone;
		if (IsBIOS(file.FileName.c_str(), version, description, region, zone))
		{
			std::string_view text = StringUtil::StripWhitespace(description);
			if (text.starts_with(zone))
				text = StringUtil::StripWhitespace(text.substr(zone.size()));
			const size_t last_space = text.rfind(' ');
			dump.board = std::string(last_space == std::string_view::npos ? std::string_view() : StringUtil::StripWhitespace(text.substr(0, last_space)));
		}
		status.dumps.push_back(std::move(dump));
	}
	std::sort(status.dumps.begin(), status.dumps.end(), [](const BiosDump& a, const BiosDump& b) { return a.file_name < b.file_name; });
	return status;
}

// ---- Games ----

ArcadeSetup::MissingReport ArcadeSetup::Service::Missing(const std::string& path) const
{
	return Inspect(m_folders, *this, path).report;
}

ArcadeSetup::PrepareResult ArcadeSetup::Service::Prepare(const std::string& path)
{
	std::lock_guard lock(s_prepare_mutex);

	PrepareResult result;
	Inspection in = Inspect(m_folders, *this, path);
	result.game_id = in.report.game_id;
	result.title = in.report.title;
	if (!in.report.IsReady())
	{
		result.missing = in.report;
		result.error = JoinExplanations(in.report);
		return result;
	}

	const auto fail = [&](PartKind kind, std::string detail) {
		result.missing = in.report;
		result.missing.parts.push_back({kind, std::move(detail)});
		result.error = JoinExplanations(result.missing);
		return result;
	};

	const std::string game_dir = Path::Combine(m_folders.arcade, in.id);
	if (!FileSystem::EnsureDirectoryExists(game_dir.c_str(), true))
	{
		result.error = fmt::format(TRANSLATE_FS("Arcade", "The folder {} could not be created."), game_dir);
		return result;
	}

	if (in.acgame)
	{
		const Arcade::AcGameInfo& game = *in.info;

		// The boot program: the game's own, else Proverb's. The BIOS loads it through host:, which reads from the
		// boot program's own folder.
		result.elf = in.elf;
		if (result.elf.empty())
		{
			const std::string elf = Path::Combine(game_dir, "boot.elf");
			if (!ExtractBootProgram(in.id, elf))
				return fail(PartKind::BootProgram, BaseName(game.elf_path));
			result.elf = elf;
		}
		result.media = in.image;

		// Soul Calibur II's Conquest card, when its .acgame names none: the core takes it from the memory cards
		// folder by its name.
		if (game.card.empty() && in.id == Arcade::CONQUEST_GAME)
			EnsureConquestCard(m_folders, nullptr, {});

		// The dongle, and the second card if the game has one: in the memory cards folder, or copied there from
		// beside the .acgame.
		for (const auto& [name, kind] : {std::pair{game.dongle, PartKind::Dongle}, std::pair{game.card, PartKind::Card}})
		{
			if (name.empty())
				continue;
			const std::string target = Path::Combine(m_folders.memory_cards, name);
			if (HasContent(target))
				continue;
			std::string source = ResolveExisting(Path::Combine(game.base_dir, BaseName(name)));
			if (source.empty())
				source = ResolveExisting(Path::Combine(game.dir, BaseName(name)));
			if (source.empty() || !CopyInto(source, target, false))
				return fail(kind, name);
			Console.WriteLn("(Arcade) copied %s into the memory cards folder", name.c_str());
		}

		// The SRAM is the core's to write; a file shipped beside the game (from a real board) seeds it once. It is
		// kept in the data folder, since a library folder is often one that can be read and not written.
		result.sram = Path::Combine(game_dir, BaseName(game.sram_path));
		if (!FileSystem::FileExists(result.sram.c_str()))
		{
			const std::string seed = ResolveExisting(game.sram_path);
			if (!seed.empty())
				CopyInto(seed, result.sram, false);
		}

		result.acgame = path;
		result.ok = true;
		return result;
	}

	// A loose image: its .acgame is written into the data folder, with the boot program from Proverb. Nothing
	// of the game itself is copied but its dongle.
	result.media = in.image;
	result.elf = Path::Combine(game_dir, "boot.elf");
	if (!ExtractBootProgram(in.id, result.elf))
		return fail(PartKind::BootProgram, "boot.elf");

	// Soul Calibur II's Conquest card before its dongle: it is a card file beside the game too.
	std::string card;
	if (in.id == Arcade::CONQUEST_GAME)
		card = EnsureConquestCard(m_folders, &in.siblings, in.folder_id);
	const std::string dongle = PrepareLooseDongle(m_folders, in.id, in.folder_id, in.siblings);
	if (dongle.empty())
		return fail(PartKind::Dongle, in.id + ".ps2");

	std::string title = in.report.title;
	title.erase(std::remove_if(title.begin(), title.end(), [](char c) { return c == '\r' || c == '\n'; }), title.end());
	std::string text = fmt::format("[game]\nname={}\ngameid={}\n\n[data]\nsubdir=\ndongle={}\n", title, in.id, dongle);
	if (!card.empty())
		text += fmt::format("card={}\n", card);
	result.acgame = Path::Combine(game_dir, in.id + ".acgame");
	Error write_error;
	if (!Catalog::WriteFileAtomic(result.acgame, text, &write_error))
	{
		result.error = fmt::format(TRANSLATE_FS("Arcade", "The game's .acgame could not be written: {}"), write_error.GetDescription());
		return result;
	}

	result.sram = Path::Combine(game_dir, "sram.bin");
	result.ok = true;
	return result;
}
