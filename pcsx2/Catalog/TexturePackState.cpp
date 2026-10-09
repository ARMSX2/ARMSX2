// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/TexturePackState.h"

#include "Catalog/Json.h"

#include "Config.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>

namespace Catalog
{
	namespace
	{
		// All reads and writes of every state file in the process go through this: a read that
		// lands between the .bak copy and the rename of a write must not see a half-way state.
		std::mutex s_file_mutex;
		std::atomic<u32> s_revision{0};

		std::string Upper(std::string_view s)
		{
			std::string out(s);
			for (char& c : out)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return out;
		}

		bool EqualsIgnoreCase(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && Upper(a) == Upper(b);
		}

		std::string StringMember(const rapidjson::Value& object, const char* key, const char* fallback = "")
		{
			const auto it = object.FindMember(key);
			if (it == object.MemberEnd() || !it->value.IsString())
				return fallback;
			return std::string(it->value.GetString(), it->value.GetStringLength());
		}

		// Reads one file into records. False if it is missing or not a JSON object.
		bool ReadFile(const std::string& path, std::map<std::string, TexturePackRecord>* out)
		{
			const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
			if (!text.has_value())
				return false;

			rapidjson::Document document;
			if (!ParseJson(*text, &document) || !document.IsObject())
				return false;

			out->clear();
			for (const auto& member : document.GetObject())
			{
				if (!member.value.IsObject())
					continue;

				TexturePackRecord record;
				record.pack_id = std::string(member.name.GetString(), member.name.GetStringLength());
				record.serial = Upper(StringMember(member.value, "serial"));
				record.version = StringMember(member.value, "version");
				record.name = StringMember(member.value, "name");
				// Records from before the tar+zstd catalog lack these; the defaults describe
				// exactly that legacy zip install.
				record.format = StringMember(member.value, "format", "zip");
				record.sha256 = StringMember(member.value, "sha256");
				const auto schema = member.value.FindMember("schema");
				if (schema != member.value.MemberEnd() && schema->value.IsInt())
					record.schema = schema->value.GetInt();
				const auto revision = member.value.FindMember("archiveRevision");
				if (revision != member.value.MemberEnd() && revision->value.IsUint64())
					record.archive_revision = revision->value.GetUint64();
				(*out)[record.pack_id] = std::move(record);
			}
			return true;
		}

		// The main file, else the previous good copy, else nothing. Without the fallback one
		// unreadable file reads as "nothing installed", and the next write then persists a file
		// holding only that one pack.
		std::map<std::string, TexturePackRecord> ReadAllLocked(const std::string& path)
		{
			std::map<std::string, TexturePackRecord> records;
			if (ReadFile(path, &records))
				return records;
			if (ReadFile(path + ".bak", &records))
				return records;
			return {};
		}

		bool WriteAllLocked(const std::string& path, const std::map<std::string, TexturePackRecord>& records)
		{
			rapidjson::Document document;
			document.SetObject();
			auto& allocator = document.GetAllocator();
			const auto str = [&allocator](const std::string& s) { return rapidjson::Value(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), allocator); };

			for (const auto& [id, record] : records)
			{
				rapidjson::Value object(rapidjson::kObjectType);
				object.AddMember("serial", str(record.serial), allocator);
				object.AddMember("version", str(record.version), allocator);
				object.AddMember("name", str(record.name), allocator);
				object.AddMember("format", str(record.format), allocator);
				object.AddMember("schema", record.schema, allocator);
				object.AddMember("archiveRevision", static_cast<uint64_t>(record.archive_revision), allocator);
				object.AddMember("sha256", str(record.sha256), allocator);
				document.AddMember(str(id), object, allocator);
			}

			// Keep the last good copy before replacing, so that a failure after this point is
			// recoverable. A main file that no longer parses must not overwrite a good .bak.
			std::map<std::string, TexturePackRecord> current;
			if (ReadFile(path, &current))
				FileSystem::CopyFilePath(path.c_str(), (path + ".bak").c_str(), true);

			Error error;
			if (!WriteJsonFileAtomic(path, document, &error))
			{
				// Losing this file costs the player a re-download of every pack: say so.
				Console.Error("Texture packs: could not write %s: %s", path.c_str(), error.GetDescription().c_str());
				return false;
			}

			s_revision.fetch_add(1, std::memory_order_release);
			return true;
		}
	} // namespace

	TexturePackState::TexturePackState(std::string file_path)
		: m_path(std::move(file_path))
	{
	}

	TexturePackState TexturePackState::InDataRoot()
	{
		return TexturePackState(Path::Combine(EmuFolders::DataRoot, "texture-packs.json"));
	}

	u32 TexturePackState::Revision()
	{
		return s_revision.load(std::memory_order_acquire);
	}

	std::map<std::string, TexturePackRecord> TexturePackState::All() const
	{
		std::lock_guard lock(s_file_mutex);
		return ReadAllLocked(m_path);
	}

	std::optional<TexturePackRecord> TexturePackState::Find(std::string_view pack_id) const
	{
		const auto records = All();
		const auto it = records.find(std::string(pack_id));
		if (it == records.end())
			return std::nullopt;
		return it->second;
	}

	std::optional<TexturePackRecord> TexturePackState::FindForSerial(std::string_view serial) const
	{
		const auto records = All();
		for (const auto& [id, record] : records)
		{
			if (EqualsIgnoreCase(record.serial, serial))
				return record;
		}
		return std::nullopt;
	}

	bool TexturePackState::Record(const TexturePackRecord& record)
	{
		std::lock_guard lock(s_file_mutex);
		auto records = ReadAllLocked(m_path);
		for (auto it = records.begin(); it != records.end();)
		{
			if (it->first != record.pack_id && EqualsIgnoreCase(it->second.serial, record.serial))
				it = records.erase(it);
			else
				++it;
		}
		TexturePackRecord stored = record;
		stored.serial = Upper(stored.serial);
		records[stored.pack_id] = std::move(stored);
		return WriteAllLocked(m_path, records);
	}

	bool TexturePackState::ForgetSerial(std::string_view serial)
	{
		std::lock_guard lock(s_file_mutex);
		auto records = ReadAllLocked(m_path);
		bool changed = false;
		for (auto it = records.begin(); it != records.end();)
		{
			if (EqualsIgnoreCase(it->second.serial, serial))
			{
				it = records.erase(it);
				changed = true;
			}
			else
			{
				++it;
			}
		}
		return !changed || WriteAllLocked(m_path, records);
	}

	void TexturePackState::Reconcile(const std::set<std::string>& present_serials_upper)
	{
		std::lock_guard lock(s_file_mutex);
		auto records = ReadAllLocked(m_path);
		if (present_serials_upper.empty() && !records.empty())
		{
			Console.Warning("Texture packs: reconcile skipped, the scan found 0 packs but %zu are recorded", records.size());
			return;
		}

		bool changed = false;
		for (auto it = records.begin(); it != records.end();)
		{
			if (present_serials_upper.count(Upper(it->second.serial)) == 0)
			{
				it = records.erase(it);
				changed = true;
			}
			else
			{
				++it;
			}
		}
		if (changed)
			WriteAllLocked(m_path, records);
	}

	InstallAction TexturePackState::ActionFor(const std::optional<TexturePackRecord>& installed, const TexturePack& pack)
	{
		if (!installed.has_value())
			return InstallAction::Install;

		if (pack.format == TexturePackFormat::TarZstd)
		{
			if (!installed->IsTarZstd())
				return InstallAction::Update;
			if (pack.archive_revision > installed->archive_revision)
				return InstallAction::Update;
			if (pack.archive_revision < installed->archive_revision)
				return InstallAction::Installed;
			return EqualsIgnoreCase(pack.sha256, installed->sha256) ? InstallAction::Installed : InstallAction::Conflict;
		}

		// A zip fallback can never downgrade a tar+zstd install, whatever its version string says.
		if (installed->IsTarZstd())
			return InstallAction::Installed;
		return installed->version == pack.version ? InstallAction::Installed : InstallAction::Update;
	}

	InstallAction TexturePackState::ActionForPack(const TexturePack& pack) const
	{
		return ActionFor(Find(pack.id), pack);
	}

	TexturePackRecord TexturePackState::RecordFor(const TexturePack& pack, std::string_view serial)
	{
		TexturePackRecord record;
		record.pack_id = pack.id;
		record.serial = Upper(serial);
		record.version = pack.version;
		record.name = pack.name;
		record.format = pack.format == TexturePackFormat::TarZstd ? "tar+zstd" : "zip";
		record.schema = pack.format == TexturePackFormat::TarZstd ? 2 : 1;
		record.archive_revision = pack.archive_revision;
		record.sha256 = pack.sha256;
		return record;
	}
} // namespace Catalog
