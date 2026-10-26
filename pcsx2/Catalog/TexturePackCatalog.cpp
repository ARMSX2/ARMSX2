// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/TexturePackCatalog.h"

#include "Catalog/Json.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace Catalog
{
	namespace
	{
		constexpr u64 MAX_ARCHIVE_BYTES = 4ull * 1024 * 1024 * 1024;
		constexpr u64 MAX_TAR_STREAM_BYTES = 16ull * 1024 * 1024 * 1024;
		constexpr int MIN_SCHEMA_VERSION = 1;
		constexpr int MAX_SCHEMA_VERSION = 2;
		constexpr size_t MAX_POPULAR_BYTES = 64 * 1024;

		std::string Upper(std::string_view s)
		{
			std::string out(s);
			for (char& c : out)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return out;
		}

		std::string Lower(std::string_view s)
		{
			std::string out(s);
			for (char& c : out)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		std::string Trimmed(std::string_view s)
		{
			return std::string(StringUtil::StripWhitespace(s));
		}

		// A string member, trimmed; empty when the member is missing or not a string.
		std::string GetString(const rapidjson::Value& object, const char* key)
		{
			const auto it = object.FindMember(key);
			if (it == object.MemberEnd() || !it->value.IsString())
				return {};
			return Trimmed(std::string_view(it->value.GetString(), it->value.GetStringLength()));
		}

		// A https link, or empty. These go to the network stack and the browser on someone else's say-so.
		std::string GetLink(const rapidjson::Value& object, const char* key)
		{
			std::string value = GetString(object, key);
			return IsHttpsUrl(value) ? value : std::string();
		}

		// The non-empty strings of an array member, trimmed. `present` tells "[]" from "missing".
		std::vector<std::string> GetStringList(const rapidjson::Value& object, const char* key, bool* present = nullptr)
		{
			std::vector<std::string> out;
			const auto it = object.FindMember(key);
			const bool is_array = it != object.MemberEnd() && it->value.IsArray();
			if (present)
				*present = is_array;
			if (!is_array)
				return out;
			for (const rapidjson::Value& element : it->value.GetArray())
			{
				if (!element.IsString())
					continue;
				std::string value = Trimmed(std::string_view(element.GetString(), element.GetStringLength()));
				if (!value.empty())
					out.push_back(std::move(value));
			}
			return out;
		}

		u64 GetU64(const rapidjson::Value& object, const char* key)
		{
			const auto it = object.FindMember(key);
			if (it == object.MemberEnd() || !it->value.IsUint64())
				return 0;
			return it->value.GetUint64();
		}

		// 64 hex characters in either case; returns them lowercase, or empty.
		std::string NormalizeSha256(std::string_view raw)
		{
			const std::string value = Trimmed(raw);
			if (value.size() != 64)
				return {};
			for (const char c : value)
			{
				if (!std::isxdigit(static_cast<unsigned char>(c)))
					return {};
			}
			return Lower(value);
		}

		// The entry's parts: nullopt when a "parts" array is present but unusable (the entry is
		// dropped rather than falling back to fetching one fragment as if it were the pack),
		// an empty vector when there are none.
		std::optional<std::vector<TexturePackPart>> ParseParts(const rapidjson::Value& entry)
		{
			std::vector<TexturePackPart> parts;
			const auto it = entry.FindMember("parts");
			if (it == entry.MemberEnd() || !it->value.IsArray())
				return parts;
			if (it->value.Empty())
				return std::nullopt;

			u64 total = 0;
			for (const rapidjson::Value& element : it->value.GetArray())
			{
				if (!element.IsObject())
					return std::nullopt;
				TexturePackPart part;
				part.download_url = GetLink(element, "downloadUrl");
				part.size_bytes = GetU64(element, "sizeBytes");
				part.sha256 = NormalizeSha256(GetString(element, "sha256"));
				if (part.download_url.empty() || part.size_bytes == 0 || part.sha256.empty())
					return std::nullopt;

				// The running total is capped, not just each piece: enough valid pieces would
				// otherwise get around the single-file cap.
				total += part.size_bytes;
				if (total > MAX_ARCHIVE_BYTES)
					return std::nullopt;
				parts.push_back(std::move(part));
			}
			return parts;
		}

		std::vector<TexturePackCreator> ParseCreators(const rapidjson::Value& entry, const std::vector<std::string>& authors, bool* from_authors)
		{
			std::vector<TexturePackCreator> creators;
			const auto it = entry.FindMember("creators");
			if (it == entry.MemberEnd() || !it->value.IsArray())
			{
				// A catalog from before the field existed: authors are all it says.
				*from_authors = true;
				for (const std::string& author : authors)
					creators.push_back(TexturePackCreator{author, {}, {}, {}, {}});
				return creators;
			}

			*from_authors = false;
			for (const rapidjson::Value& element : it->value.GetArray())
			{
				if (!element.IsObject())
					continue;
				TexturePackCreator creator;
				creator.name = GetString(element, "name");
				if (creator.name.empty())
					continue;
				creator.page = GetLink(element, "page");
				creator.avatar = GetLink(element, "avatar");
				creator.tip = GetLink(element, "tip");
				creator.socials = GetLink(element, "socials");
				creators.push_back(std::move(creator));
			}
			return creators;
		}

		std::optional<TexturePack> ParsePack(const rapidjson::Value& entry, int schema)
		{
			if (!entry.IsObject())
				return std::nullopt;

			TexturePack pack;
			pack.id = GetString(entry, "id");
			pack.name = GetString(entry, "name");
			if (pack.id.empty() || pack.name.empty())
				return std::nullopt;

			// Format rules per schema. Schema 1 is zip (absent or explicit); anything else,
			// including tar+zstd, which a schema-1 reader cannot extract, drops the entry.
			// Schema 2 requires tar+zstd with the numbers that keep updates monotonic and the
			// extraction bounded.
			const std::string format = Lower(GetString(entry, "format"));
			if (schema == 1)
			{
				if (!format.empty() && format != "zip")
					return std::nullopt;
				pack.format = TexturePackFormat::Zip;
			}
			else
			{
				if (format != "tar+zstd")
					return std::nullopt;
				pack.format = TexturePackFormat::TarZstd;
				pack.archive_revision = GetU64(entry, "archiveRevision");
				pack.decompressed_size_bytes = GetU64(entry, "decompressedSizeBytes");
				if (pack.archive_revision == 0)
					return std::nullopt;
				if (pack.decompressed_size_bytes == 0 || pack.decompressed_size_bytes > MAX_TAR_STREAM_BYTES)
					return std::nullopt;
			}

			for (const std::string& raw : GetStringList(entry, "serials"))
			{
				std::optional<std::string> serial = NormalizeSerial(raw);
				if (serial.has_value() && std::find(pack.serials.begin(), pack.serials.end(), *serial) == pack.serials.end())
					pack.serials.push_back(std::move(*serial));
			}
			if (pack.serials.empty())
				return std::nullopt;

			pack.authors = GetStringList(entry, "authors");
			if (pack.authors.empty())
				return std::nullopt;

			std::optional<std::vector<TexturePackPart>> parts = ParseParts(entry);
			if (!parts.has_value())
				return std::nullopt;
			pack.parts = std::move(*parts);

			// A split pack has no single archive URL. Older apps drop an entry without one, which
			// is what they should do with a pack they cannot reassemble.
			pack.download_url = GetLink(entry, "downloadUrl");
			if (pack.parts.empty() && pack.download_url.empty())
				return std::nullopt;

			pack.source_url = GetLink(entry, "sourceUrl");
			if (pack.source_url.empty())
				return std::nullopt;

			pack.size_bytes = GetU64(entry, "sizeBytes");
			if (pack.size_bytes == 0 || pack.size_bytes > MAX_ARCHIVE_BYTES)
				return std::nullopt;

			// The parts must add up to the archive they claim to be. Catching it here beats
			// finding out after several gigabytes have been transferred.
			if (!pack.parts.empty())
			{
				u64 sum = 0;
				for (const TexturePackPart& part : pack.parts)
					sum += part.size_bytes;
				if (sum != pack.size_bytes)
					return std::nullopt;
			}

			// The digest is the only thing between a damaged or substituted download and the
			// player's texture folder.
			pack.sha256 = NormalizeSha256(GetString(entry, "sha256"));
			if (pack.sha256.empty())
				return std::nullopt;

			const u64 file_count = GetU64(entry, "fileCount");
			if (file_count == 0 || file_count > 0x7FFFFFFFull)
				return std::nullopt;
			pack.file_count = static_cast<u32>(file_count);

			pack.version = GetString(entry, "version");
			if (pack.version.empty())
				return std::nullopt;

			pack.game_title = GetString(entry, "gameTitle");
			pack.credits = GetString(entry, "credits");
			pack.description = GetString(entry, "description");
			pack.license = GetString(entry, "license");
			pack.type = GetString(entry, "type");
			pack.completeness = GetString(entry, "completeness");
			for (std::string& url : GetStringList(entry, "previewUrls"))
			{
				if (IsHttpsUrl(url))
					pack.preview_urls.push_back(std::move(url));
			}
			pack.creators = ParseCreators(entry, pack.authors, &pack.creators_from_authors);
			return pack;
		}

		bool IsIdChar(char c, bool first)
		{
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
				return true;
			return !first && (c == '.' || c == '_' || c == '-');
		}
	} // namespace

	// -------------------------------------------------------------------------------------
	// TexturePack
	// -------------------------------------------------------------------------------------

	std::string TexturePack::CreatorText() const
	{
		std::string out;
		for (const TexturePackCreator& creator : creators)
		{
			if (!out.empty())
				out += ", ";
			out += creator.name;
		}
		return out;
	}

	bool TexturePack::MatchesSerial(std::string_view serial) const
	{
		const std::optional<std::string> wanted = NormalizeSerial(serial);
		if (!wanted.has_value())
			return false;
		return std::find(serials.begin(), serials.end(), *wanted) != serials.end();
	}

	bool TexturePack::CoversAnyOf(const std::unordered_set<std::string>& serials_upper) const
	{
		return std::any_of(serials.begin(), serials.end(), [&](const std::string& s) { return serials_upper.count(Upper(s)) != 0; });
	}

	std::string TexturePack::InstallSerialFor(std::string_view context_serial, const std::unordered_set<std::string>& owned_serials_upper) const
	{
		if (!context_serial.empty() && MatchesSerial(context_serial))
			return *NormalizeSerial(context_serial);
		for (const std::string& serial : serials)
		{
			if (owned_serials_upper.count(Upper(serial)))
				return serial;
		}
		return serials.front();
	}

	std::vector<TexturePackPart> TexturePack::EffectiveParts() const
	{
		if (!parts.empty())
			return parts;
		return {TexturePackPart{download_url, size_bytes, sha256}};
	}

	std::optional<std::string> NormalizeSerial(std::string_view raw)
	{
		std::string compact;
		for (const char c : raw)
		{
			if (std::isalnum(static_cast<unsigned char>(c)))
				compact += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		if (compact.size() != 9)
			return std::nullopt;
		for (size_t i = 0; i < 9; i++)
		{
			const bool letter = compact[i] >= 'A' && compact[i] <= 'Z';
			const bool digit = compact[i] >= '0' && compact[i] <= '9';
			if (i < 4 ? !letter : !digit)
				return std::nullopt;
		}
		return compact.substr(0, 4) + "-" + compact.substr(4);
	}

	std::optional<CatalogParseResult> ParseTexturePackCatalog(std::string_view json, std::string* error)
	{
		const auto fail = [error](std::string why) -> std::optional<CatalogParseResult> {
			if (error)
				*error = std::move(why);
			return std::nullopt;
		};

		if (StringUtil::StripWhitespace(json).empty())
			return fail("empty catalog");

		rapidjson::Document document;
		std::string parse_error;
		if (!ParseJson(json, &document, &parse_error))
			return fail("catalog is not valid JSON: " + parse_error);
		if (!document.IsObject())
			return fail("catalog is not a JSON object");

		// A schema bump means fields this build does not understand: refuse at the source, so
		// the next mirror and the cache still get their turn.
		const auto schema_it = document.FindMember("schemaVersion");
		if (schema_it == document.MemberEnd() || !schema_it->value.IsInt() ||
			schema_it->value.GetInt() < MIN_SCHEMA_VERSION || schema_it->value.GetInt() > MAX_SCHEMA_VERSION)
		{
			return fail(fmt::format("catalog schemaVersion is not in {}..{}", MIN_SCHEMA_VERSION, MAX_SCHEMA_VERSION));
		}
		const int schema = schema_it->value.GetInt();

		const auto entries_it = document.FindMember("entries");
		if (entries_it == document.MemberEnd() || !entries_it->value.IsArray())
			return fail("catalog has no entries array");

		CatalogParseResult result;
		std::unordered_set<std::string> seen;
		for (const rapidjson::Value& entry : entries_it->value.GetArray())
		{
			// One malformed entry must not cost the player the whole catalog.
			std::optional<TexturePack> pack = ParsePack(entry, schema);
			if (!pack.has_value() || !seen.insert(pack->id).second)
			{
				result.dropped_entries++;
				continue;
			}
			result.packs.push_back(std::move(*pack));
		}

		// A catalog in which nothing parsed is not an empty-but-valid catalog. Treating it as
		// one would save the emptiness and hide every mirror behind it.
		if (result.packs.empty())
			return fail("catalog has no usable entries");
		return result;
	}

	std::vector<const TexturePack*> FindPacksForSerial(const std::vector<TexturePack>& packs, std::string_view serial)
	{
		std::vector<const TexturePack*> found;
		for (const TexturePack& pack : packs)
		{
			if (pack.MatchesSerial(serial))
				found.push_back(&pack);
		}
		return found;
	}

	// -------------------------------------------------------------------------------------
	// TexturePackCatalog
	// -------------------------------------------------------------------------------------

	std::vector<std::string> TexturePackCatalog::DefaultMirrors()
	{
		return {
			"https://dl.ps2ktxpak.net/catalog.json",
			"https://f005.backblazeb2.com/file/armsx2-textures/catalog.json",
		};
	}

	ResponseCache TexturePackCatalog::DefaultCache()
	{
		return ResponseCache::InCacheFolder("texture-catalog");
	}

	TexturePackCatalog::TexturePackCatalog(Fetcher& fetcher, ResponseCache cache)
		: TexturePackCatalog(fetcher, std::move(cache), Config())
	{
	}

	TexturePackCatalog::TexturePackCatalog(Fetcher& fetcher, ResponseCache cache, Config config)
		: m_fetcher(fetcher)
		, m_cache(std::move(cache))
		, m_config(std::move(config))
	{
	}

	TexturePackCatalog::LoadResult TexturePackCatalog::FromCache(Source source)
	{
		LoadResult result;
		const std::optional<ResponseCache::Entry> entry = m_cache.Load(CACHE_FILE_NAME);
		if (!entry.has_value())
		{
			result.error = "no saved copy of the catalog";
			return result;
		}

		std::string error;
		std::optional<CatalogParseResult> parsed = ParseTexturePackCatalog(entry->body, &error);
		if (!parsed.has_value())
		{
			result.error = "the saved copy of the catalog is unusable: " + error;
			return result;
		}

		result.source = source;
		result.packs = std::move(parsed->packs);
		result.dropped_entries = parsed->dropped_entries;
		result.fetched_at = entry->fetched_at;
		return result;
	}

	TexturePackCatalog::LoadResult TexturePackCatalog::Load(bool force_refresh, const CancelToken& cancel)
	{
		if (!force_refresh && m_cache.IsFresh(CACHE_FILE_NAME, m_config.cache_max_age))
		{
			LoadResult cached = FromCache(Source::FreshCache);
			if (cached.IsOk())
				return cached;
		}

		std::string last_error;
		for (const std::string& url : m_config.urls)
		{
			if (cancel.IsCancelled())
			{
				LoadResult result;
				result.cancelled = true;
				result.error = "cancelled";
				return result;
			}

			FetchOptions options;
			options.cancel = cancel;
			options.headers = {{"Accept", "application/json"}};
			options.idle_timeout_seconds = 20;

			const FetchResponse response = m_fetcher.Get(url, MAX_CATALOG_BYTES, options);
			if (response.result == FetchResult::Cancelled)
			{
				LoadResult result;
				result.cancelled = true;
				result.error = "cancelled";
				return result;
			}
			if (response.result != FetchResult::Ok)
			{
				last_error = fmt::format("{}: {}", url, response.error);
				continue;
			}
			if (response.status != 200)
			{
				last_error = fmt::format("{}: HTTP {}", url, response.status);
				continue;
			}

			std::string parse_error;
			std::optional<CatalogParseResult> parsed = ParseTexturePackCatalog(response.body, &parse_error);
			if (!parsed.has_value())
			{
				last_error = fmt::format("{}: {}", url, parse_error);
				continue;
			}

			// Saved only now, after a successful parse, so a bad body never replaces a good copy.
			Error store_error;
			if (!m_cache.Store(CACHE_FILE_NAME, response.body, &store_error))
				Console.Warning("Texture catalog: could not save the cache: %s", store_error.GetDescription().c_str());

			LoadResult result;
			result.source = Source::Network;
			result.packs = std::move(parsed->packs);
			result.dropped_entries = parsed->dropped_entries;
			result.fetched_at = std::time(nullptr);
			return result;
		}

		// Every mirror failed. An old catalog still lets someone install, so it beats an error.
		LoadResult stale = FromCache(Source::StaleCache);
		if (!stale.IsOk())
			stale.error = last_error.empty() ? stale.error : last_error + "; " + stale.error;
		return stale;
	}

	// -------------------------------------------------------------------------------------
	// Popularity
	// -------------------------------------------------------------------------------------

	bool IsValidPackId(std::string_view id)
	{
		if (id.size() < 2 || id.size() > 255)
			return false;
		for (size_t i = 0; i < id.size(); i++)
		{
			if (!IsIdChar(id[i], i == 0))
				return false;
		}
		return true;
	}

	std::vector<std::string> ParsePopularList(std::string_view body)
	{
		std::vector<std::string> ids;
		size_t pos = 0;
		while (pos <= body.size() && ids.size() < POPULAR_PACK_COUNT)
		{
			size_t end = body.find('\n', pos);
			if (end == std::string_view::npos)
				end = body.size();

			const std::string line = Trimmed(body.substr(pos, end - pos));
			const std::string id = line.substr(0, line.find(' '));
			if (IsValidPackId(id) && std::find(ids.begin(), ids.end(), id) == ids.end())
				ids.push_back(id);

			pos = end + 1;
		}
		return ids;
	}

	std::optional<std::vector<std::string>> FetchPopularPackIds(Fetcher& fetcher, const CancelToken& cancel)
	{
		FetchOptions options;
		options.cancel = cancel;
		options.idle_timeout_seconds = 30;

		const FetchResponse response = fetcher.Get(std::string(TEXTURE_STATS_BASE_URL) + "popular", MAX_POPULAR_BYTES, options);
		if (response.result != FetchResult::Ok || response.status != 200)
			return std::nullopt;
		return ParsePopularList(response.body);
	}

	bool PostInstallHit(Fetcher& fetcher, std::string_view pack_id, bool allowed, const CancelToken& cancel)
	{
		if (!allowed || !IsValidPackId(pack_id))
			return false;

		FetchOptions options;
		options.cancel = cancel;
		options.idle_timeout_seconds = 30;
		const FetchResponse response = fetcher.Post(std::string(TEXTURE_STATS_BASE_URL) + "hit", pack_id, "text/plain; charset=utf-8", options);
		return response.IsSuccess();
	}
} // namespace Catalog
