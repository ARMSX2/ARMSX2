// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/TexturePackInstaller.h"

#include "Catalog/FileUtil.h"
#include "Catalog/Json.h"
#include "Catalog/TarZstdExtractor.h"
#include "Catalog/ZipTextureExtractor.h"

#include "Config.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SHA256.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <set>

namespace Catalog
{
	namespace
	{
		// One install at a time, process-wide: two would race for the staging folder and for the
		// disk space the free-space check promised the first.
		std::mutex s_install_mutex;
		std::atomic<bool> s_installing{false};

		constexpr const char* STAGING_NAME = ".texturepacks-tmp";
		constexpr const char* REMOVED_PREFIX = ".deleting-";
		constexpr const char* REPLACEMENTS_NAME = "replacements";

		bool IsPlainFolderName(std::string_view name)
		{
			return !name.empty() && name.front() != '.' && name.find_first_of("/\\") == std::string_view::npos &&
			       name.find('\0') == std::string_view::npos;
		}

		std::string Upper(std::string_view s)
		{
			std::string out(s);
			for (char& c : out)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return out;
		}

		// Digests compare as text without regard to case; the catalog parser gives lower case but a
		// TexturePack built by hand may not.
		bool SameDigest(std::string_view a, std::string_view b)
		{
			return Upper(a) == Upper(b);
		}

		// "SLUS-21410", "slus_21410", "SLUS 21410", "SLUS21410" and nothing else. NormalizeSerial()
		// is looser (it drops everything that is not a letter or digit, for hand-edited catalogs);
		// a request to install is held to the plain spellings, so "../SLUS-21410" is not one.
		bool IsPlainSerialSpelling(std::string_view raw)
		{
			const std::string_view s = StringUtil::StripWhitespace(raw);
			if (s.size() != 9 && s.size() != 10)
				return false;
			for (size_t i = 0; i < 4; i++)
			{
				if (!std::isalpha(static_cast<unsigned char>(s[i])))
					return false;
			}
			size_t digits = 4;
			if (s.size() == 10)
			{
				if (s[4] != '-' && s[4] != '_' && s[4] != ' ')
					return false;
				digits = 5;
			}
			for (size_t i = digits; i < s.size(); i++)
			{
				if (!std::isdigit(static_cast<unsigned char>(s[i])))
					return false;
			}
			return true;
		}

		// A folder, or a link to one (even a dangling one): what a player may have at a replacements path.
		bool PathIsTaken(const std::string& path)
		{
			return FileSystem::DirectoryExists(path.c_str()) || FileSystem::IsSymbolicLink(path.c_str());
		}

		// A symbolic link is removed, never followed: RecursiveDeleteDirectory() lists what the link points at
		// and would empty it, and a player's replacements folder may be a link to a texture library elsewhere.
		void DeleteDirectoryTree(const std::string& path)
		{
			if (FileSystem::IsSymbolicLink(path.c_str()))
			{
				if (!FileSystem::DeleteSymbolicLink(path.c_str()))
					Console.Warning("Texture packs: could not remove the link %s", path.c_str());
				return;
			}
			if (FileSystem::DirectoryExists(path.c_str()) && !FileSystem::RecursiveDeleteDirectory(path.c_str()))
				Console.Warning("Texture packs: could not delete %s completely", path.c_str());
		}

		// "<target>.user-20261008-153012", with -2, -3 ... if that name is taken (two installs in one second).
		std::string KeptFolderPath(const std::string& target)
		{
			const std::time_t now = std::time(nullptr);
			std::tm local = {};
#ifdef _WIN32
			localtime_s(&local, &now);
#else
			localtime_r(&now, &local);
#endif
			char stamp[32] = {};
			std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

			const std::string base = target + ".user-" + stamp;
			std::string candidate = base;
			for (int n = 2; PathIsTaken(candidate); n++)
				candidate = fmt::format("{}-{}", base, n);
			return candidate;
		}
	} // namespace

	std::string FormatPackSize(u64 bytes)
	{
		constexpr u64 GIB = 1024ull * 1024 * 1024;
		if (bytes >= GIB)
			return fmt::format("{:.1f} GB", static_cast<double>(bytes) / static_cast<double>(GIB));
		return fmt::format("{} MB", bytes / 1024 / 1024);
	}

	std::string DescribeProgress(const TexturePackInstaller::Progress& progress)
	{
		using Phase = TexturePackInstaller::Phase;
		switch (progress.phase)
		{
			case Phase::Starting:
				return "Starting...";
			case Phase::Downloading:
				return fmt::format("Downloading {} of {}", FormatPackSize(progress.done), FormatPackSize(progress.total));
			case Phase::Verifying:
				return "Verifying download...";
			case Phase::Extracting:
				return fmt::format("Extracting {} / {} files", progress.done, progress.total);
			case Phase::Installing:
				return "Installing...";
		}
		return {};
	}

	TexturePackInstaller::Config TexturePackInstaller::DefaultConfig()
	{
		Config config;
		config.textures_dir = EmuFolders::Textures;
		config.size_cache_path = Path::Combine(Path::Combine(EmuFolders::Cache, "texture-catalog"), "pack-sizes.json");
		return config;
	}

	TexturePackInstaller::TexturePackInstaller(Fetcher& fetcher, TexturePackState& state, Config config)
		: m_fetcher(fetcher)
		, m_state(state)
		, m_config(std::move(config))
	{
	}

	bool TexturePackInstaller::IsInstalling()
	{
		return s_installing.load(std::memory_order_acquire);
	}

	u64 TexturePackInstaller::RequiredFreeSpace(const TexturePack& pack)
	{
		if (pack.format == TexturePackFormat::TarZstd)
			return pack.size_bytes + pack.decompressed_size_bytes + FREE_SPACE_SLACK;
		return pack.size_bytes * 2 + FREE_SPACE_SLACK;
	}

	bool TexturePackInstaller::CommitDirectory(const std::string& staged, const std::string& target)
	{
		const std::string backup = target + ".old";
		DeleteDirectoryTree(backup);

		const std::string parent(Path::GetDirectory(target));
		if (!parent.empty() && !FileSystem::EnsureDirectoryExists(parent.c_str(), true))
			return false;

		const bool had_previous = PathIsTaken(target);
		if (had_previous && !FileSystem::RenamePath(target.c_str(), backup.c_str()))
		{
			Console.Warning("Texture packs: could not move the existing replacements aside");
			return false;
		}

		if (!FileSystem::RenamePath(staged.c_str(), target.c_str()))
		{
			if (had_previous)
				FileSystem::RenamePath(backup.c_str(), target.c_str());
			Console.Warning("Texture packs: could not move the staged pack into place");
			return false;
		}

		DeleteDirectoryTree(backup);
		return true;
	}

	bool TexturePackInstaller::HasUnrecordedReplacements(std::string_view serial) const
	{
		const std::optional<std::string> folder = IsPlainSerialSpelling(serial) ? NormalizeSerial(serial) : std::nullopt;
		if (!folder.has_value() || m_config.textures_dir.empty())
			return false;
		return PathIsTaken(Path::Combine(Path::Combine(m_config.textures_dir, *folder), REPLACEMENTS_NAME)) &&
		       !m_state.FindForSerial(*folder).has_value();
	}

	TexturePackInstaller::InstallResult TexturePackInstaller::Install(const TexturePack& pack, std::string_view serial,
		const ProgressCallback& progress, const CancelToken& cancel)
	{
		InstallResult result;

		std::unique_lock<std::mutex> lock(s_install_mutex, std::try_to_lock);
		if (!lock.owns_lock())
		{
			result.status = Status::Busy;
			result.error = "Another texture pack is already being installed.";
			return result;
		}
		s_installing.store(true, std::memory_order_release);
		ScopedGuard installing_guard([]() { s_installing.store(false, std::memory_order_release); });

		const auto fail = [&result](std::string why) {
			result.status = Status::Failed;
			result.error = std::move(why);
			return result;
		};
		const auto cancelled = [&result]() {
			result.status = Status::Cancelled;
			return result;
		};
		const auto report = [&progress](Phase phase, u64 done = 0, u64 total = 0) {
			if (progress)
				progress(Progress{phase, done, total});
		};

		const std::optional<std::string> folder = IsPlainSerialSpelling(serial) ? NormalizeSerial(serial) : std::nullopt;
		if (!folder.has_value() || !pack.MatchesSerial(*folder))
			return fail(fmt::format("This pack is not for {}.", serial));
		if (m_config.textures_dir.empty())
			return fail("There is no textures folder.");

		Error error;
		if (!FileSystem::EnsureDirectoryExists(m_config.textures_dir.c_str(), true, &error))
			return fail(fmt::format("Could not create the textures folder: {}", error.GetDescription()));

		report(Phase::Starting);

		// Staging shares a filesystem with the destination, or the commit turns from a rename
		// into a multi-gigabyte copy.
		const std::string staging = Path::Combine(m_config.textures_dir, STAGING_NAME);
		DeleteDirectoryTree(staging);
		if (!FileSystem::EnsureDirectoryExists(staging.c_str(), true, &error))
			return fail(fmt::format("Could not create the staging folder: {}", error.GetDescription()));
		ScopedGuard staging_guard([&staging]() { DeleteDirectoryTree(staging); });

		const u64 required = RequiredFreeSpace(pack);
		const std::optional<u64> available = m_config.query_free_space ? m_config.query_free_space(m_config.textures_dir) :
		                                                                 FreeSpaceOn(m_config.textures_dir);
		// An unknown or zero answer skips the check: a filesystem that cannot say is better
		// tried than refused.
		if (available.has_value() && *available > 0 && *available < required)
			return fail(fmt::format("Needs ~{} MB free, {} MB available", required / 1024 / 1024, *available / 1024 / 1024));

		// Parts stream straight into one file, in order: joining afterwards would hold the pieces
		// and the joined archive at once, doubling the disk use.
		const std::string archive = Path::Combine(staging, "pack.download");
		const std::vector<TexturePackPart> parts = pack.EffectiveParts();
		const bool split = parts.size() > 1;

		SHA256Digest overall;
		u64 done = 0;
		for (size_t index = 0; index < parts.size(); index++)
		{
			if (cancel.IsCancelled())
				return cancelled();

			const TexturePackPart& part = parts[index];
			const std::string label = split ? fmt::format(" (part {} of {})", index + 1, parts.size()) : std::string();

			SHA256Digest part_digest;
			FetchOptions options;
			options.cancel = cancel;
			options.append = index > 0;
			options.max_file_bytes = part.size_bytes;
			// Keep Content-Length honest so the progress bar means something.
			options.headers = {{"Accept-Encoding", "identity"}};
			options.connect_timeout_seconds = 20;
			options.on_data = [&overall, &part_digest](const u8* data, size_t size) {
				overall.Update(data, size);
				part_digest.Update(data, size);
			};
			options.on_progress = [&](u64 part_done, u64) { report(Phase::Downloading, done + part_done, pack.size_bytes); };

			const FetchResponse response = m_fetcher.GetToFile(part.download_url, archive, options);
			if (response.result == FetchResult::Cancelled)
				return cancelled();
			if (response.result == FetchResult::TooLarge)
				return fail(fmt::format("Size mismatch{} (expected {}, got more)", label, part.size_bytes));
			if (response.result != FetchResult::Ok || !response.IsSuccess())
			{
				Console.Warning("Texture packs: download of %s failed: result %d status %d %s", part.download_url.c_str(),
					static_cast<int>(response.result), response.status, response.error.c_str());
				return fail("Download failed" + label);
			}

			// Per-part checks name the bad piece, and stop us paying for the rest of a transfer
			// that can no longer produce the right archive.
			if (response.bytes_received != part.size_bytes)
				return fail(fmt::format("Size mismatch{} (expected {}, got {})", label, part.size_bytes, response.bytes_received));
			if (!SameDigest(part_digest.FinalHex(), part.sha256))
				return fail("Checksum mismatch" + label + ": the download was corrupted");

			done += response.bytes_received;
			report(Phase::Downloading, done, pack.size_bytes);
		}

		// Still checked when every part passed: the parts can each be intact and yet be the wrong
		// parts, or joined in the wrong order.
		report(Phase::Verifying);
		const s64 archive_size = FileSystem::GetPathFileSize(archive.c_str());
		if (archive_size < 0 || static_cast<u64>(archive_size) != pack.size_bytes)
			return fail(fmt::format("Size mismatch (expected {}, got {})", pack.size_bytes, archive_size));
		if (!SameDigest(overall.FinalHex(), pack.sha256))
			return fail("Checksum mismatch: the download was corrupted");
		if (cancel.IsCancelled())
			return cancelled();

		const std::string extracted = Path::Combine(staging, "out");
		const auto extract_progress = [&report](u64 files_done, u64 files_total) { report(Phase::Extracting, files_done, files_total); };
		ExtractResult extraction;
		if (pack.format == TexturePackFormat::TarZstd)
		{
			TarZstdExpectations expected;
			expected.decompressed_bytes = pack.decompressed_size_bytes;
			expected.texture_count = pack.file_count;
			extraction = TarZstdExtractor::ExtractFile(archive, extracted, expected, extract_progress, cancel);
		}
		else
		{
			extraction = ZipTextureExtractor::ExtractFile(archive, extracted, extract_progress, cancel, m_config.query_free_space);
		}
		if (extraction.status == ExtractStatus::Cancelled)
			return cancelled();
		if (extraction.status != ExtractStatus::Ok)
		{
			Console.Warning("Texture packs: extraction failed: %s", extraction.error.c_str());
			return fail(extraction.error.empty() ? "The archive could not be unpacked" : extraction.error);
		}
		if (extraction.textures == 0)
			return fail("Archive contained no textures");
		FileSystem::DeleteFilePath(archive.c_str());

		// From here on cancelling no longer works; the UI hides its Cancel button on this phase.
		report(Phase::Installing);
		const std::string target = Path::Combine(Path::Combine(m_config.textures_dir, *folder), REPLACEMENTS_NAME);

		// A folder with no install record is the player's own. It is set aside under another name, never deleted.
		std::string kept;
		if (PathIsTaken(target) && !m_state.FindForSerial(*folder).has_value())
		{
			kept = KeptFolderPath(target);
			if (!FileSystem::RenamePath(target.c_str(), kept.c_str()))
				return fail("Could not set aside the existing replacements folder");
		}
		if (!CommitDirectory(extracted, target))
		{
			if (!kept.empty())
				FileSystem::RenamePath(kept.c_str(), target.c_str());
			return fail("Could not write to the textures folder");
		}
		if (!kept.empty())
			result.kept_previous = std::string(Path::GetFileName(kept));
		ForgetSize(*folder);

		result.record = TexturePackState::RecordFor(pack, *folder);
		if (!m_state.Record(result.record))
			Console.Warning("Texture packs: installed %s but could not record it", pack.id.c_str());

		result.status = Status::Ok;
		result.textures = extraction.textures;
		return result;
	}

	void TexturePackInstaller::LoadSizeCacheLocked()
	{
		if (m_size_loaded)
			return;
		m_size_loaded = true;
		if (m_config.size_cache_path.empty())
			return;

		const std::optional<std::string> text = FileSystem::ReadFileToString(m_config.size_cache_path.c_str());
		if (!text.has_value())
			return;
		rapidjson::Document document;
		if (!ParseJson(*text, &document) || !document.IsObject())
			return;

		for (const auto& member : document.GetObject())
		{
			if (!member.value.IsObject())
				continue;
			const auto m = member.value.FindMember("m");
			const auto c = member.value.FindMember("c");
			const auto b = member.value.FindMember("b");
			if (m == member.value.MemberEnd() || c == member.value.MemberEnd() || b == member.value.MemberEnd() ||
				!m->value.IsInt64() || !c->value.IsUint64() || !b->value.IsUint64())
				continue;
			m_sizes[std::string(member.name.GetString(), member.name.GetStringLength())] =
				SizeEntry{m->value.GetInt64(), c->value.GetUint64(), b->value.GetUint64()};
		}
	}

	void TexturePackInstaller::SaveSizeCacheLocked()
	{
		if (m_config.size_cache_path.empty())
			return;

		rapidjson::Document document;
		document.SetObject();
		auto& allocator = document.GetAllocator();
		for (const auto& [name, entry] : m_sizes)
		{
			rapidjson::Value object(rapidjson::kObjectType);
			object.AddMember("m", static_cast<int64_t>(entry.mtime), allocator);
			object.AddMember("c", static_cast<uint64_t>(entry.file_count), allocator);
			object.AddMember("b", static_cast<uint64_t>(entry.bytes), allocator);
			document.AddMember(rapidjson::Value(name.c_str(), static_cast<rapidjson::SizeType>(name.size()), allocator), object, allocator);
		}
		Error error;
		if (!WriteJsonFileAtomic(m_config.size_cache_path, document, &error))
			Console.Warning("Texture packs: could not save the size cache: %s", error.GetDescription().c_str());
	}

	void TexturePackInstaller::ForgetSize(std::string_view folder_name)
	{
		std::lock_guard lock(m_size_mutex);
		LoadSizeCacheLocked();
		if (m_sizes.erase(std::string(folder_name)) != 0)
			SaveSizeCacheLocked();
	}

	void TexturePackInstaller::SweepRemoved()
	{
		FileSystem::FindResultsArray folders;
		if (!FileSystem::FindFiles(m_config.textures_dir.c_str(), "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, &folders))
			return;

		for (const FILESYSTEM_FIND_DATA& fd : folders)
		{
			if (Path::GetFileName(fd.FileName).substr(0, std::char_traits<char>::length(REMOVED_PREFIX)) == REMOVED_PREFIX)
				DeleteDirectoryTree(fd.FileName);
		}
	}

	std::vector<TexturePackInstaller::InstalledPack> TexturePackInstaller::ListInstalled(const CancelToken& cancel)
	{
		if (cancel.IsCancelled())
			return {};
		SweepRemoved();

		std::vector<InstalledPack> packs;
		FileSystem::FindResultsArray folders;
		if (FileSystem::FindFiles(m_config.textures_dir.c_str(), "*", FILESYSTEM_FIND_FOLDERS, &folders))
		{
			std::lock_guard lock(m_size_mutex);
			LoadSizeCacheLocked();
			bool cache_changed = false;

			for (const FILESYSTEM_FIND_DATA& fd : folders)
			{
				if (cancel.IsCancelled())
					break;

				const std::string name(Path::GetFileName(fd.FileName));
				if (!IsPlainFolderName(name))
					continue;
				const std::string replacements = Path::Combine(fd.FileName, REPLACEMENTS_NAME);
				if (!FileSystem::DirectoryExists(replacements.c_str()))
					continue;

				InstalledPack pack;
				pack.serial = name;
				pack.directory = replacements;

				// Sizes come from the cache, keyed on the folder's mtime: a changed pack is measured
				// again, an untouched one is not walked.
				const s64 mtime = static_cast<s64>(FileSystem::GetFileTimestamp(replacements.c_str()).value_or(0));
				const auto cached = m_sizes.find(name);
				if (cached != m_sizes.end() && cached->second.mtime == mtime)
				{
					pack.file_count = cached->second.file_count;
					pack.bytes = cached->second.bytes;
				}
				else
				{
					FileSystem::FindResultsArray files;
					FileSystem::FindFiles(replacements.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_HIDDEN_FILES, &files);
					for (const FILESYSTEM_FIND_DATA& file : files)
					{
						pack.file_count++;
						pack.bytes += static_cast<u64>(std::max<s64>(file.Size, 0));
					}
					m_sizes[name] = SizeEntry{mtime, pack.file_count, pack.bytes};
					cache_changed = true;
				}
				packs.push_back(std::move(pack));
			}
			if (cache_changed)
				SaveSizeCacheLocked();
		}

		// A partial list must not reach Reconcile(), which drops the records of packs it does not see.
		if (cancel.IsCancelled())
			return {};

		std::sort(packs.begin(), packs.end(), [](const InstalledPack& a, const InstalledPack& b) { return a.serial < b.serial; });

		// The record and the disk are two stores that can disagree; the disk is the truth.
		std::set<std::string> present;
		for (const InstalledPack& pack : packs)
			present.insert(Upper(pack.serial));
		m_state.Reconcile(present);

		for (InstalledPack& pack : packs)
			pack.record = m_state.FindForSerial(pack.serial);
		return packs;
	}

	bool TexturePackInstaller::Remove(std::string_view folder_name)
	{
		if (!IsPlainFolderName(folder_name))
			return false;

		const std::string name(folder_name);
		const std::string serial_dir = Path::Combine(m_config.textures_dir, name);
		for (const char* part : {REPLACEMENTS_NAME, "replacements.old"})
		{
			const std::string dir = Path::Combine(serial_dir, part);
			if (!PathIsTaken(dir))
				continue;

			// One rename takes the pack out of use however many files it holds; deleting the
			// files comes after, and whatever fails there is finished by SweepRemoved().
			const std::string removed = Path::Combine(m_config.textures_dir,
				fmt::format("{}{}-{}-{}", REMOVED_PREFIX, name, part, std::chrono::steady_clock::now().time_since_epoch().count()));
			const std::string doomed = FileSystem::RenamePath(dir.c_str(), removed.c_str()) ? removed : dir;
			DeleteDirectoryTree(doomed);
		}

		// Texture dumps and anything else in the game's folder stay; the folder goes once empty.
		if (FileSystem::DirectoryExists(serial_dir.c_str()) && FileSystem::DirectoryIsEmpty(serial_dir.c_str()))
			FileSystem::DeleteDirectory(serial_dir.c_str());

		// The record goes whatever happened on disk. A pack that is gone, or half gone, is not
		// installed, and a record left behind blocks the reinstall.
		m_state.ForgetSerial(name);
		ForgetSize(name);
		return !FileSystem::DirectoryExists(Path::Combine(serial_dir, REPLACEMENTS_NAME).c_str());
	}
} // namespace Catalog
