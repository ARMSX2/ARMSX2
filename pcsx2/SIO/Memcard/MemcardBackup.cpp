// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Memcard/MemcardBackup.h"

#include "BuildVersion.h"
#include "Catalog/FileUtil.h"
#include "Catalog/Json.h"
#include "Config.h"
#include "Host.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SHA256.h"
#include "common/StringUtil.h"
#include "common/ZipHelpers.h"

#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <set>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace MemcardBackup
{
	namespace
	{
		constexpr const char* MANIFEST_NAME = "armsx2-mcbak.json";
		constexpr std::string_view PAYLOAD_PREFIX = "card/";
		constexpr const char* SUPERBLOCK_NAME = "_pcsx2_superblock";
		constexpr int MANIFEST_SCHEMA = 1;

		// A PS1 card is 128 KiB; nothing smaller is a card. A floor, not a table of sizes: strict
		// about the header and permissive about the size refuses damaged cards without refusing
		// unusual ones that work.
		constexpr s64 MIN_CARD_BYTES = 128 * 1024;

		constexpr size_t KEEP_RECENT = 3;
		constexpr size_t KEEP_DAYS = 4;

		constexpr int DEFLATE_LEVEL = 5;

		// Everything that changes the backup folder or a card runs under this.
		std::mutex s_store_mutex;

		std::mutex s_handler_mutex;
		RestorePromptHandler s_prompt_handler;
		RestorePromptHandler s_fallback_prompt_handler;

		Outcome Fail(std::string detail)
		{
			return Outcome{false, std::move(detail)};
		}

		bool IsDirectory(const std::string& path)
		{
			return FileSystem::DirectoryExists(path.c_str());
		}

		// The files of a folder card, as '/' separated paths relative to the folder, in byte order.
		std::vector<std::pair<std::string, s64>> ListFolderFiles(const std::string& dir)
		{
			FileSystem::FindResultsArray found;
			FileSystem::FindFiles(dir.c_str(), "*",
				FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_HIDDEN_FILES,
				&found);

			std::vector<std::pair<std::string, s64>> files;
			files.reserve(found.size());
			for (const FILESYSTEM_FIND_DATA& fd : found)
			{
				std::string relative = fd.FileName;
				std::replace(relative.begin(), relative.end(), '\\', '/');
				files.emplace_back(std::move(relative), fd.Size);
			}
			std::sort(files.begin(), files.end());
			return files;
		}

		u64 CardSizeBytes(const std::string& path)
		{
			if (!IsDirectory(path))
			{
				const s64 size = FileSystem::GetPathFileSize(path.c_str());
				return size > 0 ? static_cast<u64>(size) : 0;
			}

			u64 total = 0;
			for (const auto& file : ListFolderFiles(path))
				total += static_cast<u64>(std::max<s64>(file.second, 0));
			return total;
		}

		bool FeedFile(SHA256Digest& sha, const std::string& path, Error* error)
		{
			FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(path.c_str(), "rb", error);
			if (!fp)
				return false;

			std::vector<u8> buffer(256 * 1024);
			for (;;)
			{
				const size_t n = std::fread(buffer.data(), 1, buffer.size(), fp.get());
				if (n > 0)
					sha.Update(buffer.data(), n);
				if (n < buffer.size())
					break;
			}
			if (std::ferror(fp.get()))
			{
				Error::SetStringFmt(error, "Reading '{}' failed.", path);
				return false;
			}
			return true;
		}

		// A card's folder under the backup root: its name with the characters Windows forbids replaced.
		std::string SanitizeCardName(std::string_view name)
		{
			std::string out(name);
			for (char& c : out)
			{
				if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos)
					c = '_';
			}
			const size_t first = out.find_first_not_of(" \t");
			const size_t last = out.find_last_not_of(" \t");
			out = (first == std::string::npos) ? std::string() : out.substr(first, last - first + 1);
			return out.empty() ? std::string("card") : out;
		}

		// A path inside a backup that is safe to join onto the staging folder: components only, no "..".
		bool IsSafeRelativePath(std::string_view rel)
		{
			if (rel.empty() || rel.front() == '/' || rel.find('\\') != std::string_view::npos ||
				rel.find('\0') != std::string_view::npos)
				return false;
			size_t start = 0;
			while (start <= rel.size())
			{
				size_t end = rel.find('/', start);
				if (end == std::string_view::npos)
					end = rel.size();
				const std::string_view part = rel.substr(start, end - start);
				if (part.empty() || part == "." || part == "..")
					return false;
#ifdef _WIN32
				if (part.find(':') != std::string_view::npos)
					return false;
#endif
				start = end + 1;
			}
			return true;
		}

		int DayKey(s64 ms)
		{
			const std::time_t t = static_cast<std::time_t>(ms / 1000);
			std::tm local = {};
#ifdef _WIN32
			localtime_s(&local, &t);
#else
			localtime_r(&t, &local);
#endif
			return (local.tm_year + 1900) * 1000 + local.tm_yday;
		}

		// ---- manifest ----

		std::string BuildManifest(const Snapshot& snap)
		{
			rapidjson::Document doc;
			doc.SetObject();
			auto& alloc = doc.GetAllocator();
			const auto str = [&alloc](const std::string& s) { return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), alloc); };

			doc.AddMember("schemaVersion", MANIFEST_SCHEMA, alloc);
			doc.AddMember("card", str(snap.card), alloc);
			doc.AddMember("takenAt", static_cast<int64_t>(snap.taken_at_ms), alloc);
			doc.AddMember("reason", str(ReasonName(snap.reason)), alloc);
			doc.AddMember("sizeBytes", static_cast<int64_t>(snap.size_bytes), alloc);
			doc.AddMember("hash", str(snap.hash), alloc);
			doc.AddMember("healthy", snap.healthy, alloc);
			doc.AddMember("folder", snap.is_folder, alloc);
			doc.AddMember("game", str(snap.game), alloc);
			doc.AddMember("appVersion", str(BuildVersion::GitTag ? BuildVersion::GitTag : ""), alloc);
			return Catalog::JsonToString(doc);
		}

		std::string JsonString(const rapidjson::Value& obj, const char* key, std::string fallback = {})
		{
			if (obj.HasMember(key) && obj[key].IsString())
				return std::string(obj[key].GetString(), obj[key].GetStringLength());
			return fallback;
		}

		s64 JsonInt(const rapidjson::Value& obj, const char* key, s64 fallback)
		{
			if (obj.HasMember(key) && obj[key].IsInt64())
				return obj[key].GetInt64();
			return fallback;
		}

		bool JsonBool(const rapidjson::Value& obj, const char* key, bool fallback)
		{
			if (obj.HasMember(key) && obj[key].IsBool())
				return obj[key].GetBool();
			return fallback;
		}

		Reason ReasonFromName(std::string_view name)
		{
			for (const Reason r : {Reason::Session, Reason::Save, Reason::Manual, Reason::PreRestore})
			{
				if (name == ReasonName(r))
					return r;
			}
			return Reason::Manual;
		}

		// Reads a snapshot's manifest. A zip that cannot be read is still returned, unhealthy, so the
		// user sees it and can judge; hiding it would be worse.
		Snapshot ReadSnapshot(const std::string& zip_path, const std::string& card_fallback)
		{
			const std::string file_name(Path::GetFileName(zip_path));

			Snapshot snap;
			snap.path = zip_path;
			snap.card = card_fallback;
			snap.reason = Reason::Manual;
			snap.healthy = false;

			// <millis>-<reason>.zip, else the file's own time.
			const size_t dash = file_name.find('-');
			std::optional<s64> from_name;
			if (dash != std::string::npos && dash > 0)
				from_name = StringUtil::FromChars<s64>(std::string_view(file_name).substr(0, dash));
			if (from_name.has_value())
			{
				snap.taken_at_ms = *from_name;
			}
			else
			{
				FILESYSTEM_STAT_DATA stat = {};
				if (FileSystem::StatFile(zip_path.c_str(), &stat))
					snap.taken_at_ms = static_cast<s64>(stat.ModificationTime) * 1000;
			}
			snap.size_bytes = static_cast<u64>(std::max<s64>(FileSystem::GetPathFileSize(zip_path.c_str()), 0));

			zip_error_t ze = {};
			auto za = zip_open_managed(zip_path.c_str(), ZIP_RDONLY, &ze);
			if (!za)
				return snap;
			const std::optional<std::string> text = ReadFileInZipToString(za.get(), MANIFEST_NAME);
			if (!text.has_value())
				return snap;

			rapidjson::Document doc;
			if (!Catalog::ParseJson(*text, &doc) || !doc.IsObject())
				return snap;

			snap.card = JsonString(doc, "card", card_fallback);
			snap.taken_at_ms = JsonInt(doc, "takenAt", snap.taken_at_ms);
			snap.reason = ReasonFromName(JsonString(doc, "reason", "MANUAL"));
			snap.size_bytes = static_cast<u64>(std::max<s64>(JsonInt(doc, "sizeBytes", 0), 0));
			snap.hash = JsonString(doc, "hash");
			snap.healthy = JsonBool(doc, "healthy", false);
			snap.game = JsonString(doc, "game");
			snap.is_folder = JsonBool(doc, "folder", false);
			return snap;
		}

		// ---- zip ----

		struct ZipWriter
		{
			zip_t* za = nullptr;
			~ZipWriter()
			{
				if (za)
					zip_discard(za);
			}
		};

		bool AddFileEntry(zip_t* za, const std::string& entry_name, const std::string& source_path, std::string* error)
		{
			zip_source_t* source = zip_source_file(za, source_path.c_str(), 0, 0);
			if (!source)
			{
				*error = fmt::format("Could not read '{}': {}", source_path, zip_strerror(za));
				return false;
			}
			const zip_int64_t index = zip_file_add(za, entry_name.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
			if (index < 0)
			{
				zip_source_free(source);
				*error = fmt::format("Could not add '{}' to the backup: {}", entry_name, zip_strerror(za));
				return false;
			}
			zip_set_file_compression(za, static_cast<zip_uint64_t>(index), ZIP_CM_DEFLATE, DEFLATE_LEVEL);
			return true;
		}

		bool WriteZip(const std::string& zip_path, const std::string& card_path, const std::string& card_name,
			bool is_folder, const std::string& manifest, std::string* error)
		{
			zip_error_t ze = {};
			zip_source_t* zs = zip_source_file_create(zip_path.c_str(), 0, 0, &ze);
			if (!zs)
			{
				*error = fmt::format("Could not create the backup file: {}", zip_error_strerror(&ze));
				return false;
			}
			ZipWriter writer;
			writer.za = zip_open_from_source(zs, ZIP_CREATE | ZIP_TRUNCATE, &ze);
			if (!writer.za)
			{
				zip_source_free(zs);
				*error = fmt::format("Could not create the backup file: {}", zip_error_strerror(&ze));
				return false;
			}

			// The buffer must outlive zip_close(), which is where the data is read.
			zip_source_t* manifest_source = zip_source_buffer(writer.za, manifest.data(), manifest.size(), 0);
			if (!manifest_source ||
				zip_file_add(writer.za, MANIFEST_NAME, manifest_source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0)
			{
				if (manifest_source)
					zip_source_free(manifest_source);
				*error = "Could not add the manifest to the backup.";
				return false;
			}

			if (is_folder)
			{
				for (const auto& file : ListFolderFiles(card_path))
				{
					if (!AddFileEntry(writer.za, std::string(PAYLOAD_PREFIX) + file.first, Path::Combine(card_path, file.first), error))
						return false;
				}
			}
			else if (!AddFileEntry(writer.za, std::string(PAYLOAD_PREFIX) + card_name, card_path, error))
			{
				return false;
			}

			// This is where the card is read and compressed.
			if (zip_close(writer.za) != 0)
			{
				*error = fmt::format("Writing the backup failed: {}", zip_strerror(writer.za));
				return false;
			}
			writer.za = nullptr;
			return true;
		}

		bool ExtractEntry(zip_t* za, zip_uint64_t index, const std::string& dest_path, std::string* error)
		{
			zip_stat_t stat;
			zip_stat_init(&stat);
			if (zip_stat_index(za, index, 0, &stat) != 0)
			{
				*error = "A file in the backup could not be read.";
				return false;
			}
			auto zf = zip_fopen_index_managed(za, index, 0);
			if (!zf)
			{
				*error = "A file in the backup could not be read.";
				return false;
			}

			const std::string parent(Path::GetDirectory(dest_path));
			if (!parent.empty() && !FileSystem::EnsureDirectoryExists(parent.c_str(), true))
			{
				*error = "A folder for the restored card could not be created.";
				return false;
			}

			FileSystem::ManagedCFilePtr out = FileSystem::OpenManagedCFile(dest_path.c_str(), "wb");
			if (!out)
			{
				*error = "The restored card could not be written.";
				return false;
			}

			std::vector<u8> buffer(256 * 1024);
			u64 total = 0;
			for (;;)
			{
				const zip_int64_t n = zip_fread(zf.get(), buffer.data(), buffer.size());
				if (n < 0)
				{
					*error = "The backup is damaged.";
					return false;
				}
				if (n == 0)
					break;
				if (std::fwrite(buffer.data(), 1, static_cast<size_t>(n), out.get()) != static_cast<size_t>(n))
				{
					*error = "The restored card could not be written (disk full?).";
					return false;
				}
				total += static_cast<u64>(n);
			}
			if (std::fflush(out.get()) != 0)
			{
				*error = "The restored card could not be written (disk full?).";
				return false;
			}
			if ((stat.valid & ZIP_STAT_SIZE) && total != stat.size)
			{
				*error = "The backup is damaged.";
				return false;
			}
			return true;
		}

		// Unpacks the card in `snap` to `staging`. Fails on an unsafe path rather than skipping it: an
		// entry that escapes the folder means the zip is not one of ours.
		bool UnpackSnapshot(const Snapshot& snap, const std::string& staging, std::string* error)
		{
			zip_error_t ze = {};
			auto za = zip_open_managed(snap.path.c_str(), ZIP_RDONLY, &ze);
			if (!za)
			{
				*error = "That backup could not be read.";
				return false;
			}

			if (snap.is_folder && !FileSystem::EnsureDirectoryExists(staging.c_str(), true))
			{
				*error = "A folder for the restored card could not be created.";
				return false;
			}

			int extracted = 0;
			const zip_int64_t count = zip_get_num_entries(za.get(), 0);
			for (zip_int64_t i = 0; i < count; i++)
			{
				const char* raw_name = zip_get_name(za.get(), static_cast<zip_uint64_t>(i), 0);
				if (!raw_name)
					continue;
				const std::string_view name(raw_name);
				if (!name.starts_with(PAYLOAD_PREFIX) || name.ends_with('/'))
					continue;
				const std::string_view relative = name.substr(PAYLOAD_PREFIX.size());

				std::string dest;
				if (snap.is_folder)
				{
					if (!IsSafeRelativePath(relative))
					{
						*error = "That backup contains a file name that is not allowed.";
						return false;
					}
					dest = Path::Combine(staging, relative);
				}
				else
				{
					if (extracted > 0)
					{
						*error = "That backup holds more than one file but is not a folder card.";
						return false;
					}
					dest = staging;
				}

				if (!ExtractEntry(za.get(), static_cast<zip_uint64_t>(i), dest, error))
					return false;
				extracted++;
			}

			if (extracted == 0)
			{
				*error = "That backup could not be read.";
				return false;
			}
			return true;
		}
	} // namespace

	const char* ReasonName(Reason reason)
	{
		switch (reason)
		{
			case Reason::Session:
				return "SESSION";
			case Reason::Save:
				return "SAVE";
			case Reason::Manual:
				return "MANUAL";
			case Reason::PreRestore:
				return "PRE_RESTORE";
		}
		return "MANUAL";
	}

	// ---- cards ----------------------------------------------------------------------------

	Health Verify(const std::string& card_path)
	{
		FILESYSTEM_STAT_DATA stat = {};
		if (!FileSystem::StatFile(card_path.c_str(), &stat))
			return Health::Missing;

		if (stat.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY)
			return FileSystem::FileExists(Path::Combine(card_path, SUPERBLOCK_NAME).c_str()) ? Health::Good : Health::Unreadable;

		if (stat.Size < MIN_CARD_BYTES)
			return Health::Unreadable;

		return FileMcd_IsMemoryCardFormatted(card_path) ? Health::Good : Health::Unreadable;
	}

	bool HashCard(const std::string& card_path, std::string* hex, Error* error)
	{
		if (!IsDirectory(card_path))
			return SHA256Digest::HashFile(card_path.c_str(), hex, error) == SHA256Digest::FileResult::Ok;

		SHA256Digest sha;
		for (const auto& file : ListFolderFiles(card_path))
		{
			sha.Update(file.first);
			const u8 zero = 0;
			sha.Update(&zero, 1);

			// The size on disk now, not what the directory listing said a moment ago.
			const s64 size = FileSystem::GetPathFileSize(Path::Combine(card_path, file.first).c_str());
			u8 length[8];
			for (int i = 0; i < 8; i++)
				length[i] = static_cast<u8>(static_cast<u64>(std::max<s64>(size, 0)) >> (56 - 8 * i));
			sha.Update(length, sizeof(length));

			if (!FeedFile(sha, Path::Combine(card_path, file.first), error))
				return false;
		}
		*hex = sha.FinalHex();
		return true;
	}

	bool IsPlainCardName(std::string_view name)
	{
		return !name.empty() && name != "." && name != ".." && name.find('/') == std::string_view::npos &&
		       name.find('\\') == std::string_view::npos && name.find('\0') == std::string_view::npos;
	}

	// ---- Store ----------------------------------------------------------------------------

	Store::Store(std::string cards_dir, std::string backups_dir)
		: m_cards_dir(std::move(cards_dir))
		, m_backups_dir(std::move(backups_dir))
		, m_ops(&Catalog::GetRealFileOps())
	{
	}

	Store Store::ForDataRoot()
	{
		return Store(EmuFolders::MemoryCards, Path::Combine(EmuFolders::DataRoot, "memcard-backups"));
	}

	std::string Store::CardPath(const std::string& card) const
	{
		return Path::Combine(m_cards_dir, card);
	}

	std::string Store::BackupDirFor(const std::string& card) const
	{
		return Path::Combine(m_backups_dir, SanitizeCardName(card));
	}

	s64 Store::Now() const
	{
		if (m_clock)
			return m_clock();
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	}

	std::vector<std::string> Store::CardsWithBackups() const
	{
		std::vector<std::string> names;
		FileSystem::FindResultsArray found;
		FileSystem::FindFiles(m_backups_dir.c_str(), "*", FILESYSTEM_FIND_FOLDERS, &found);
		for (const FILESYSTEM_FIND_DATA& fd : found)
		{
			const std::string name(Path::GetFileName(fd.FileName));
			if (!name.empty() && name.front() != '.')
				names.push_back(name);
		}
		std::sort(names.begin(), names.end());
		return names;
	}

	std::vector<Snapshot> Store::List(const std::string& card) const
	{
		std::vector<Snapshot> snapshots;
		FileSystem::FindResultsArray found;
		FileSystem::FindFiles(BackupDirFor(card).c_str(), "*", FILESYSTEM_FIND_FILES, &found);
		for (const FILESYSTEM_FIND_DATA& fd : found)
		{
			if (!fd.FileName.ends_with(".zip") && !fd.FileName.ends_with(".mcbak"))
				continue;
			Snapshot snap = ReadSnapshot(fd.FileName, card);
			// Two card names can share a folder after sanitising; list only this card's.
			if (snap.card == card)
				snapshots.push_back(std::move(snap));
		}

		std::sort(snapshots.begin(), snapshots.end(), [](const Snapshot& a, const Snapshot& b) {
			if (a.taken_at_ms != b.taken_at_ms)
				return a.taken_at_ms > b.taken_at_ms;
			return a.path > b.path;
		});
		return snapshots;
	}

	std::optional<Snapshot> Store::NewestHealthy(const std::string& card) const
	{
		for (Snapshot& snap : List(card))
		{
			if (snap.healthy)
				return std::move(snap);
		}
		return std::nullopt;
	}

	std::optional<Snapshot> Store::WriteSnapshot(const std::string& card, Reason reason, const std::string& game,
		bool healthy, const std::string& hash, std::string* error)
	{
		std::string scratch;
		std::string& why = error ? *error : scratch;

		if (!IsPlainCardName(card))
		{
			why = "The card has no usable name.";
			return std::nullopt;
		}
		const std::string card_path = CardPath(card);
		if (!m_ops->Exists(card_path))
		{
			why = "The card does not exist.";
			return std::nullopt;
		}

		const std::string dir = BackupDirFor(card);
		if (!FileSystem::EnsureDirectoryExists(dir.c_str(), true))
		{
			why = "The backup folder could not be created.";
			return std::nullopt;
		}

		// A .part left by a snapshot that was cut short is never listed; sweep it.
		FileSystem::FindResultsArray stale;
		FileSystem::FindFiles(dir.c_str(), "*.part", FILESYSTEM_FIND_FILES, &stale);
		for (const FILESYSTEM_FIND_DATA& fd : stale)
			FileSystem::DeleteFilePath(fd.FileName.c_str());

		Snapshot snap;
		snap.card = card;
		snap.reason = reason;
		snap.is_folder = IsDirectory(card_path);
		snap.healthy = healthy;
		snap.game = game;
		snap.size_bytes = CardSizeBytes(card_path);
		snap.taken_at_ms = Now();
		snap.hash = hash;
		if (snap.hash.empty() && !HashCard(card_path, &snap.hash))
			snap.hash.clear();

		// Two snapshots in one millisecond must not share a name.
		std::string target;
		for (;; snap.taken_at_ms++)
		{
			target = Path::Combine(dir, fmt::format("{}-{}.zip", snap.taken_at_ms, StringUtil::toLower(ReasonName(reason))));
			if (!FileSystem::FileExists(target.c_str()))
				break;
		}

		// Written beside its final name and renamed into place, so a process killed mid-write leaves
		// no truncated archive that would list as a valid snapshot.
		const std::string part = target + ".part";
		if (!WriteZip(part, card_path, card, snap.is_folder, BuildManifest(snap), &why))
		{
			FileSystem::DeleteFilePath(part.c_str());
			return std::nullopt;
		}
		if (!FileSystem::RenamePath(part.c_str(), target.c_str()))
		{
			FileSystem::DeleteFilePath(part.c_str());
			why = "The backup could not be moved into place.";
			return std::nullopt;
		}

		snap.path = target;
		return ReadSnapshot(target, card);
	}

	CardStatus Store::SnapshotIfChanged(const std::string& card, Reason reason, const std::string& game)
	{
		std::lock_guard lock(s_store_mutex);

		const std::string card_path = CardPath(card);
		switch (Verify(card_path))
		{
			case Health::Missing:
				return CardStatus::Absent;
			case Health::Unreadable:
				// There is nothing worth saving in it, and a history slot to lose.
				return CardStatus::Unreadable;
			case Health::Good:
				break;
		}

		std::string hash;
		Error error;
		if (!HashCard(card_path, &hash, &error))
		{
			Console.Warning("MemcardBackup: could not read %s to compare it: %s", card.c_str(), error.GetDescription().c_str());
			return CardStatus::Failed;
		}

		const std::vector<Snapshot> existing = List(card);
		if (!existing.empty() && existing.front().hash == hash)
			return CardStatus::Unchanged;

		std::string why;
		if (!WriteSnapshot(card, reason, game, true, hash, &why).has_value())
		{
			Console.Warning("MemcardBackup: could not back up %s: %s", card.c_str(), why.c_str());
			return CardStatus::Failed;
		}

		PruneLocked(card);
		return CardStatus::Snapshotted;
	}

	std::optional<Snapshot> Store::BackUpNow(const std::string& card, const std::string& game, std::string* error)
	{
		std::lock_guard lock(s_store_mutex);

		std::string scratch;
		std::string& why = error ? *error : scratch;

		switch (Verify(CardPath(card)))
		{
			case Health::Missing:
				why = "The card does not exist.";
				return std::nullopt;
			case Health::Unreadable:
				why = "The card is not readable (not formatted), so there is nothing worth backing up.";
				return std::nullopt;
			case Health::Good:
				break;
		}

		std::optional<Snapshot> snap = WriteSnapshot(card, Reason::Manual, game, true, std::string(), &why);
		if (snap.has_value())
			PruneLocked(card);
		return snap;
	}

	void Store::Prune(const std::string& card)
	{
		std::lock_guard lock(s_store_mutex);
		PruneLocked(card);
	}

	void Store::PruneLocked(const std::string& card)
	{
		const std::vector<Snapshot> all = List(card);
		if (all.empty())
			return;

		std::set<std::string> keep;

		// The newest few, whatever their age.
		std::set<int> recent_days;
		for (size_t i = 0; i < all.size() && i < KEEP_RECENT; i++)
		{
			keep.insert(all[i].path);
			recent_days.insert(DayKey(all[i].taken_at_ms));
		}

		// The newest of each of the next few days. Three copies from one afternoon protect against one
		// afternoon; corruption that goes unnoticed for days needs a history that spans days.
		std::set<int> seen_days;
		for (const Snapshot& snap : all)
		{
			const int day = DayKey(snap.taken_at_ms);
			if (recent_days.count(day) || seen_days.count(day))
				continue;
			seen_days.insert(day);
			keep.insert(snap.path);
			if (seen_days.size() >= KEEP_DAYS)
				break;
		}

		// The newest healthy one, however old. A card whose header is intact and whose contents are
		// subtly wrong does not stop the history filling with copies of it; this is what survives.
		for (const Snapshot& snap : all)
		{
			if (snap.healthy)
			{
				keep.insert(snap.path);
				break;
			}
		}

		for (const Snapshot& snap : all)
		{
			if (!keep.count(snap.path))
				FileSystem::DeleteFilePath(snap.path.c_str());
		}
	}

	Outcome Store::Restore(const Snapshot& snap)
	{
		std::lock_guard lock(s_store_mutex);

		if (!IsPlainCardName(snap.card))
			return Fail("That backup does not name a valid memory card.");
		if (!FileSystem::FileExists(snap.path.c_str()))
			return Fail("That backup no longer exists.");
		if (!FileSystem::EnsureDirectoryExists(m_cards_dir.c_str(), true))
			return Fail("The memory cards folder could not be created.");

		const std::string live = CardPath(snap.card);
		const std::string staging = live + ".restoring";
		const std::string previous = live + ".previous";

		// Unpack and check before touching the live card, so a backup that cannot be read changes
		// nothing.
		m_ops->Remove(staging);
		std::string why;
		if (!UnpackSnapshot(snap, staging, &why))
		{
			m_ops->Remove(staging);
			return Fail(why);
		}
		if (snap.healthy && Verify(staging) != Health::Good)
		{
			m_ops->Remove(staging);
			return Fail("That backup is damaged.");
		}
		if (!snap.hash.empty())
		{
			std::string hash;
			if (!HashCard(staging, &hash) || hash != snap.hash)
			{
				m_ops->Remove(staging);
				return Fail("That backup is damaged.");
			}
		}

		// The card as it is now, broken or not, so restoring the wrong copy cannot destroy it.
		if (m_ops->Exists(live))
		{
			const bool live_is_good = (Verify(live) == Health::Good);
			std::string pre_error;
			if (!WriteSnapshot(snap.card, Reason::PreRestore, std::string(), live_is_good, std::string(), &pre_error) &&
				live_is_good)
			{
				m_ops->Remove(staging);
				return Fail("The current card could not be backed up first, so it was left as it is.");
			}
		}

		// Swap through a sidelined copy rather than deleting first, so a failure part-way leaves the
		// original recoverable instead of leaving no card at all.
		m_ops->Remove(previous);
		if (m_ops->Exists(live) && !m_ops->Rename(live, previous))
		{
			m_ops->Remove(staging);
			return Fail("The current card is in use and could not be replaced.");
		}
		if (!m_ops->Rename(staging, live))
		{
			std::string detail = "The restored card could not be put in place.";
			if (m_ops->Exists(previous) && !m_ops->Rename(previous, live))
				detail += fmt::format(" Your previous card is saved as {}.", Path::GetFileName(previous));
			m_ops->Remove(staging);
			return Fail(std::move(detail));
		}
		m_ops->Remove(previous);

		return Outcome{true, fmt::format("Restored {}.", snap.card)};
	}

	// ---- boot hook ------------------------------------------------------------------------

	void SetRestorePromptHandler(RestorePromptHandler handler)
	{
		std::lock_guard lock(s_handler_mutex);
		s_prompt_handler = std::move(handler);
	}

	void SetFallbackRestorePromptHandler(RestorePromptHandler handler)
	{
		std::lock_guard lock(s_handler_mutex);
		s_fallback_prompt_handler = std::move(handler);
	}

	RestorePromptHandler GetRestorePromptHandler()
	{
		std::lock_guard lock(s_handler_mutex);
		return s_prompt_handler ? s_prompt_handler : s_fallback_prompt_handler;
	}

	RestorePromptHandler GetFallbackRestorePromptHandler()
	{
		std::lock_guard lock(s_handler_mutex);
		return s_fallback_prompt_handler;
	}

	bool AutoBackupDefault()
	{
#ifdef __ANDROID__
		constexpr bool android = true;
#else
		constexpr bool android = false;
#endif
#ifdef ENABLE_LIBRETRO
		constexpr bool libretro = true;
#else
		constexpr bool libretro = false;
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
		constexpr bool ios = true;
#else
		constexpr bool ios = false;
#endif
		return AutoBackupDefaultFor(android, libretro, ios);
	}

	bool IsAutoBackupEnabled()
	{
		// From EmuConfig, not Host::GetBoolSettingValue(): FileMcd_Reopen can be called with the
		// settings lock held (VMManager::UpdateEmuFolders), and that lock is not recursive.
		return EmuConfig.McdAutoBackup;
	}

	void ReportUnansweredRestore(const std::string& card)
	{
		Console.Warning("MemcardBackup: %s cannot be read and has a backup, but nobody could be asked; starting with the card as it is. "
						"Its backup can be restored from Settings > Memory Cards > Backups.",
			card.c_str());
		Host::AddKeyedOSDMessage("MemcardRestorePrompt",
			fmt::format(TRANSLATE_FS("MemoryCard", "Memory card {} cannot be read. A backup exists: restore it from Settings > Memory Cards > Backups."), card),
			Host::OSD_WARNING_DURATION);
	}

	std::vector<CardReport> PrepareCardsForBoot(Store& store, const std::vector<std::string>& cards,
		const std::string& game, bool allow_prompt, const RestorePromptHandler& handler)
	{
		std::vector<CardReport> reports;
		std::set<std::string> done;
		int slot = 0;
		for (const std::string& card : cards)
		{
			slot++;
			if (!IsPlainCardName(card) || !done.insert(card).second)
				continue;

			CardReport report;
			report.card = card;

			if (allow_prompt && Verify(store.CardPath(card)) == Health::Unreadable)
			{
				const std::optional<Snapshot> healthy = store.NewestHealthy(card);
				if (healthy.has_value())
				{
					Console.Warning("MemcardBackup: %s is not readable and has a backup from %lld", card.c_str(),
						static_cast<long long>(healthy->taken_at_ms));
					if (handler)
					{
						RestorePrompt prompt;
						prompt.slot = slot;
						prompt.card = card;
						prompt.snapshot = *healthy;
						report.prompted = true;
						if (handler(prompt) == RestoreAnswer::Restore)
						{
							const Outcome outcome = store.Restore(*healthy);
							if (outcome.ok)
							{
								Console.WriteLn("MemcardBackup: %s", outcome.detail.c_str());
								report.restored = true;
							}
							else
							{
								Host::ReportErrorAsync(TRANSLATE_SV("MemoryCard", "Memory Card Restore Failed"), outcome.detail);
							}
						}
					}
					else
					{
						report.unanswered = true;
						ReportUnansweredRestore(card);
					}
				}
			}

			if (!report.restored)
			{
				report.status = store.SnapshotIfChanged(card, Reason::Session, game);
				switch (report.status)
				{
					case CardStatus::Absent:
					case CardStatus::Failed:
						break;
					case CardStatus::Unreadable:
						Console.Warning("MemcardBackup: %s is not readable, not backed up", card.c_str());
						break;
					case CardStatus::Unchanged:
						DevCon.WriteLn("MemcardBackup: %s unchanged", card.c_str());
						break;
					case CardStatus::Snapshotted:
						Console.WriteLn("MemcardBackup: backed up %s", card.c_str());
						break;
				}
			}
			reports.push_back(std::move(report));
		}
		return reports;
	}

	void OnCardsReopening()
	{
		OnCardsReopening(VMManager::GetState() == VMState::Initializing);
	}

	void OnCardsReopening(bool booting)
	{
		if (!booting || !IsAutoBackupEnabled())
			return;

		// Slots 1 and 2 only: the multitap cards are not covered.
		constexpr size_t BACKED_UP_SLOTS = 2;

		std::vector<std::string> cards;
		for (size_t slot = 0; slot < BACKED_UP_SLOTS; slot++)
		{
			const Pcsx2Config::McdOptions& mcd = EmuConfig.Mcd[slot];
			// An empty name keeps the slot number of the card after it.
			cards.push_back((mcd.Enabled && mcd.Type != MemoryCardType::Empty) ? mcd.Filename : std::string());
		}

		const RestorePromptHandler handler = GetRestorePromptHandler();

		Store store = Store::ForDataRoot();
		PrepareCardsForBoot(store, cards, VMManager::GetDiscSerial(), true, handler);
	}

	Outcome BackUpCardNow(const std::string& card)
	{
		if (VMManager::GetState() != VMState::Shutdown)
			return Fail("Memory cards can only be backed up while no game is running.");
		Store store = Store::ForDataRoot();
		std::string error;
		const std::optional<Snapshot> snap = store.BackUpNow(card, std::string(), &error);
		if (!snap.has_value())
			return Fail(std::move(error));
		return Outcome{true, fmt::format("Backed up {}.", card)};
	}

	Outcome RestoreSnapshot(const Snapshot& snapshot)
	{
		if (VMManager::GetState() != VMState::Shutdown)
			return Fail("A backup can only be restored while no game is running.");
		return Store::ForDataRoot().Restore(snapshot);
	}
} // namespace MemcardBackup
