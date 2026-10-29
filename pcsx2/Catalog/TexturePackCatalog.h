// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The online texture-pack catalog: what packs exist, which games they are for, who made them,
// and what installing one means (archive URL, size, digest, format). One file carries all of it,
// https://dl.ps2ktxpak.net/catalog.json (mirror: the Backblaze bucket), which the ps2ktxpak
// compiler writes. There is no second file for creators.
//
// File format (schemaVersion 1 or 2, packs under "entries"):
//
//   id, name, version, serials[], authors[], sourceUrl, sizeBytes, sha256, fileCount   required
//   gameTitle, credits, description, license, previewUrls[]                          optional
//   downloadUrl           required unless "parts" is present
//   parts[]               {downloadUrl, sizeBytes, sha256}; they concatenate into the archive
//   format                schema 1: "zip" (or absent). schema 2: "tar+zstd"
//   archiveRevision       schema 2: monotonic per pack, > 0
//   decompressedSizeBytes schema 2: exact length of the tar stream, > 0
//   creators[]            who made the pack, lead first: {name, page?, avatar?, tip?, socials?}
//   type, completeness    optional labels, see TexturePack
//
// A bad entry is dropped on its own; a catalog in which nothing parses is refused as a whole so
// that the next mirror, and finally the cache, get their turn.
//
// All network calls block; call them from a worker thread. Nothing here touches the UI.
//
// Typical use, on a worker thread:
//
//   TexturePackCatalog catalog(GetDefaultFetcher(), TexturePackCatalog::DefaultCache());
//   TexturePackCatalog::LoadResult r = catalog.Load();      // r.IsStale(): say "saved copy"
//   for (const TexturePack* pack : FindPacksForSerial(r.packs, serial)) { ... }

#include "Catalog/CancelToken.h"
#include "Catalog/Fetch.h"
#include "Catalog/ResponseCache.h"

#include "common/Pcsx2Types.h"

#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Catalog
{
	enum class TexturePackFormat
	{
		Zip, ///< schema 1
		TarZstd, ///< schema 2: the converter's tar+zstd of ASTC KTX textures
	};

	/// One piece of a split archive. The pieces concatenate, in order, into the archive that
	/// TexturePack::sha256 describes. Each has its own digest, which names the damaged piece.
	struct TexturePackPart
	{
		std::string download_url;
		u64 size_bytes = 0;
		std::string sha256; ///< 64 lowercase hex characters
	};

	/// A creator credited on a pack, with the links the catalog has for them. A link that is not
	/// https is left empty.
	struct TexturePackCreator
	{
		std::string name;
		std::string page; ///< their own page: a forum profile, say
		std::string avatar;
		std::string tip;
		std::string socials;
	};

	struct TexturePack
	{
		std::string id; ///< Unique in the catalog; keys the installed-pack record.
		std::string name;
		std::string game_title; ///< May be empty.
		std::vector<std::string> serials; ///< Normalised "SLUS-21410"; at least one.
		std::string version;
		std::string credits; ///< A sentence naming the creators, as the catalog writes it.
		std::string description;
		std::string license; ///< A sentence about redistribution permission.
		std::string source_url; ///< https page about the pack
		std::vector<std::string> preview_urls; ///< https only

		/// Names exactly as the entry's "authors" list had them. When nobody named the creator
		/// this holds the placeholder "Unknown creator" that older apps need; it is not a name
		/// and is not shown as one. Use creators / HasNamedCreators() / CreatorText() instead.
		std::vector<std::string> authors;

		/// Who made the pack, lead first.
		///
		/// Empty means nobody has named the creator (the entry carried "creators": []). Say so
		/// ("Creator not identified") instead of showing `authors`.
		///
		/// An entry without a "creators" key comes from a catalog that predates the field; its
		/// `authors` become the creators (name only) and creators_from_authors is true.
		std::vector<TexturePackCreator> creators;
		bool creators_from_authors = false;

		/// "ai_upscale", "handcrafted", "mixed", "port" or "button_replacement"; empty when the
		/// catalog does not say. Values this build does not know are passed through.
		std::string type;

		/// "complete", "in_progress", "incomplete" or "partial"; empty when not known.
		std::string completeness;

		std::string download_url; ///< Empty on a split pack.
		std::vector<TexturePackPart> parts; ///< Empty for a single-file pack.
		TexturePackFormat format = TexturePackFormat::Zip;
		u64 size_bytes = 0; ///< Compressed archive, > 0 and at most 4 GiB.
		std::string sha256; ///< Of the whole archive; 64 lowercase hex characters.
		u32 file_count = 0; ///< Texture files; tar+zstd extraction checks it exactly.
		u64 archive_revision = 0; ///< 0 for zip.
		u64 decompressed_size_bytes = 0; ///< Exact tar stream length; 0 for zip.

		bool HasNamedCreators() const { return !creators.empty(); }

		/// The creators' names on one line, "A, B"; empty when nobody is named.
		std::string CreatorText() const;

		/// Case-insensitive; the argument may be spelled "SLUS-21410", "slus_21410" or "SLUS21410".
		bool MatchesSerial(std::string_view serial) const;

		/// True if any of the pack's serials is in `serials_upper` (upper-case "SLUS-21410").
		bool CoversAnyOf(const std::unordered_set<std::string>& serials_upper) const;

		/// The serial folder this pack installs into for this player. The engine reads
		/// replacements only from the folder named for the running disc's own serial, so a
		/// multi-region pack goes under the serial of the copy the player has: `context_serial`
		/// (the game in context) if the pack covers it, else the first pack serial found in
		/// `owned_serials_upper`, else the first one listed.
		std::string InstallSerialFor(std::string_view context_serial, const std::unordered_set<std::string>& owned_serials_upper) const;

		/// The pieces to fetch, in order; a single-file pack presents as one piece.
		std::vector<TexturePackPart> EffectiveParts() const;

		/// Whether this pack's textures are ASTC and so useless on a GPU without ASTC support
		/// (see TexturePackEngine::GetAstcSupportOnGSThread). Every tar+zstd pack in the catalog
		/// is converted to ASTC KTX; the catalog has no per-pack flag, so the format stands in.
		bool NeedsAstc() const { return format == TexturePackFormat::TarZstd; }
	};

	/// "slus 21287" / "SLUS_21287" / "SLUS-21287" -> "SLUS-21287"; nullopt if it is not a serial
	/// (four letters, five digits).
	std::optional<std::string> NormalizeSerial(std::string_view raw);

	struct CatalogParseResult
	{
		std::vector<TexturePack> packs;
		u32 dropped_entries = 0; ///< Entries that were malformed or duplicate ids.
	};

	/// Parses a catalog file. Returns nullopt, with the reason in `error`, if the JSON is broken,
	/// the schema version is outside 1..2, there is no "entries" array, or no entry survives.
	std::optional<CatalogParseResult> ParseTexturePackCatalog(std::string_view json, std::string* error = nullptr);

	std::vector<const TexturePack*> FindPacksForSerial(const std::vector<TexturePack>& packs, std::string_view serial);

	class TexturePackCatalog
	{
	public:
		static constexpr size_t MAX_CATALOG_BYTES = 8 * 1024 * 1024;
		static constexpr std::chrono::seconds CACHE_MAX_AGE = std::chrono::hours(6);
		static constexpr const char* CACHE_FILE_NAME = "catalog.json";

		/// The mirrors, tried in order.
		static std::vector<std::string> DefaultMirrors();

		struct Config
		{
			std::vector<std::string> urls = DefaultMirrors();
			std::chrono::seconds cache_max_age = CACHE_MAX_AGE;
		};

		/// `cache` holds catalog.json (see DefaultCache()); the fetcher must outlive this object.
		TexturePackCatalog(Fetcher& fetcher, ResponseCache cache);
		TexturePackCatalog(Fetcher& fetcher, ResponseCache cache, Config config);

		/// <Cache>/texture-catalog
		static ResponseCache DefaultCache();

		enum class Source
		{
			None, ///< Nothing usable: see LoadResult::error.
			Network, ///< Fetched just now.
			FreshCache, ///< Saved copy younger than the cache age; the network was not touched.
			StaleCache, ///< Every mirror failed; this is the old saved copy. Say so in the UI.
		};

		struct LoadResult
		{
			Source source = Source::None;
			std::vector<TexturePack> packs;
			u32 dropped_entries = 0;
			std::time_t fetched_at = 0; ///< When this data came from the server.
			bool cancelled = false;
			std::string error; ///< For logs; empty on success.

			bool IsOk() const { return source != Source::None; }
			bool IsStale() const { return source == Source::StaleCache; }
		};

		/// A saved copy younger than six hours is returned without touching the network unless
		/// `force_refresh`. Otherwise each mirror is tried in order (a mirror that answers with a
		/// status other than 200, a body over 8 MiB, or something that does not parse is skipped);
		/// the first good body is saved atomically and returned. If all fail, the saved copy is
		/// returned however old it is, as StaleCache.
		LoadResult Load(bool force_refresh = false, const CancelToken& cancel = {});

	private:
		LoadResult FromCache(Source source);

		Fetcher& m_fetcher;
		ResponseCache m_cache;
		Config m_config;
	};

	/// https://iconstats.ps2ktxpak.net/textures/ : an anonymous counter that counts each pack
	/// once per person per day.
	static constexpr const char* TEXTURE_STATS_BASE_URL = "https://iconstats.ps2ktxpak.net/textures/";

	static constexpr size_t POPULAR_PACK_COUNT = 10;

	/// Pack ids the counter accepts and the app sends: ^[a-z0-9][a-z0-9._-]{1,254}$
	bool IsValidPackId(std::string_view id);

	/// The counter's answer: one line per pack, "<id> <count>". Keeps the id of each line when it
	/// is valid, drops duplicates, and stops at POPULAR_PACK_COUNT.
	std::vector<std::string> ParsePopularList(std::string_view body);

	/// Today's most downloaded pack ids, most first. nullopt if the counter cannot be reached
	/// (the UI says "Can't reach the download counter right now"); an empty list means nothing
	/// has been downloaded yet today.
	std::optional<std::vector<std::string>> FetchPopularPackIds(Fetcher& fetcher, const CancelToken& cancel = {});

	/// Tells the counter that a pack the user chose has installed; only the id is sent. Call it from
	/// a worker thread after a successful user-started install. `allowed` is whether the app may
	/// phone home (the UI ties it to its online-features toggle); when it is false, or the id is not
	/// valid, nothing is sent. Not retried. Returns true if the counter acknowledged it. `cancel`
	/// abandons the request, which otherwise waits up to 30 s for a silent server.
	bool PostInstallHit(Fetcher& fetcher, std::string_view pack_id, bool allowed, const CancelToken& cancel = {});
} // namespace Catalog
