// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Where a game's cover art comes from: which URL to ask for, given the game's serial, its
// platform (PS2, PS1, arcade), the 2D/3D style the user prefers, and the "cover region" the user
// picked. Port of the Android behaviour (GameInfo.kt coverUrl/discCoverUrl, CoverRegionIndex.kt).
//
// Nothing here touches the network, the settings or the game list; CoverDownloadJob.h runs the
// downloads and CoverAutoDownload.h ties both to the game list and the INI.
//
// The cover repositories name a file after the DISC serial of one specific release, and that
// serial differs in every region (SLUS-21007 / SLES-53411 / SLPS-25329 are one game, Kuon). So
// "show the Japanese cover for my US disc" means finding the other region's serial, which the
// game database makes possible: the index below groups serials by their English title.
//
// Fallback is two separate mechanisms, neither a chain:
//   1. The chosen region swaps the serial in the URL (CoverRegionIndex::CoverSerialFor).
//   2. If that URL 404s, the disc's own serial is tried once (CoverUrls::fallback).

#include "common/Pcsx2Types.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Catalog
{
	enum class CoverPlatform
	{
		PS2,
		PS1,
		Arcade, ///< Namco System 246/256 (serial NM#####). Detected from the serial too, see CoverUrl().
	};

	/// Flat 2D scan (JPEG) or a 3D case render (PNG with transparency). Android's default is 2D.
	enum class CoverStyle
	{
		Flat2D,
		Case3D,
	};

	/// The release region a user wants cover art from. The values are what Android stores in its
	/// preferences and what the INI keeps, so they must not be renumbered.
	enum class CoverRegion : int
	{
		Disc = 0, ///< Whatever region the disc is.
		USA = 1,
		Europe = 2,
		JapanAsia = 3,
	};
	static constexpr int COVER_REGION_COUNT = 4;

	/// "NM" and five digits: a Namco arcade game ID.
	bool IsArcadeGameId(std::string_view serial);

	/// A serial that is safe to put in a URL path and to use as a file name: 1 to 24 characters
	/// from [A-Za-z0-9_-]. Game-list serials normally look like "SLUS-21007".
	bool IsUsableCoverSerial(std::string_view serial);

	/// The URL of the cover for `serial`, or an empty string if the serial is not usable. The
	/// serial is used exactly as given. Rule order: an arcade game ID (NM#####) always goes to the
	/// arcade server whatever `platform` says; then PS2 and PS1 pick their repository.
	///
	///   arcade 2D  https://arcade.ps2ktxpak.net/arcade-covers/<id>.jpg
	///   arcade 3D  https://arcade.ps2ktxpak.net/arcade-covers/3d/<id>.png
	///   PS2        https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/{default/<s>.jpg | 3d/<s>.png}
	///   PS1        https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/{default/<s>.jpg | 3d/<s>.png}
	std::string CoverUrl(CoverPlatform platform, std::string_view serial, CoverStyle style);

	/// The regional-serial groups built from GameIndex.yaml.
	///
	/// The file is read line by line, as Android does: a serial at column 0 ("SLUS-21007:", 8 to 12
	/// characters, '-' fifth), then its indented `name:` and `name-en:` (the English one wins).
	/// Titles are reduced to a comparison key (lower case, anything in [] or () dropped,
	/// non-alphanumerics dropped) and serials sharing a key form a group, in file order. Roughly
	/// 12.8k serials; PS1 serials are not in the database, so they never match.
	class CoverRegionIndex
	{
	public:
		static CoverRegionIndex FromGameIndexYaml(std::string_view yaml);

		/// The comparison key for a title; empty if nothing alphanumeric is left.
		static std::string NormalizeTitle(std::string_view title);

		bool Empty() const { return m_group_of.empty(); }
		size_t SerialCount() const { return m_group_of.size(); }

		/// The serial whose cover should be shown for `serial` when `wanted` is the region: the
		/// first serial of its group (file order) in that region. Null when `wanted` is Disc, when
		/// the serial is unknown, when it already is in the wanted region (no like-for-like
		/// swap), or when the game has no release there. Null means "use the disc's own serial".
		std::optional<std::string> CoverSerialFor(std::string_view serial, CoverRegion wanted) const;

		/// True if the serial's prefix (the part before the '-') belongs to `region`:
		/// USA SLUS SCUS; Europe SLES SCES SLED SCED; Japan/Asia SLPS SLPM SCPS SLKA SCKA SCAJ SLAJ.
		static bool SerialIsInRegion(std::string_view serial, CoverRegion region);

	private:
		std::vector<std::vector<std::string>> m_groups;
		std::unordered_map<std::string, u32> m_group_of; ///< upper-case serial -> index into m_groups
	};

	/// What the user asked for: 2D or 3D, one region for the whole library, and per-game pins that
	/// win over it. A pin to Disc means "this game follows its own disc", which is different from
	/// having no pin ("follow the library").
	struct CoverPreferences
	{
		CoverStyle style = CoverStyle::Flat2D;
		CoverRegion region = CoverRegion::Disc;
		std::map<std::string, CoverRegion> pins; ///< upper-case serial -> region

		std::optional<CoverRegion> PinFor(std::string_view serial) const;

		/// The pin if there is one, else the library-wide region.
		CoverRegion EffectiveRegion(std::string_view serial) const;

		/// Pass nullopt to remove the pin. An empty serial is ignored.
		void SetPin(std::string_view serial, std::optional<CoverRegion> pin);

		/// Whether the region index is worth building: the library region is not Disc or any pin
		/// is not Disc. A user on the defaults never pays for the 2.6 MB parse.
		bool NeedsRegionIndex() const;

		/// The INI text for the pins: "SLUS-20946=3,SLES-51234=1" (sorted by serial). Parsing is
		/// forgiving like Android's: entries without '=' or with a non-number are skipped, values
		/// are clamped to 0..3, serials are upper-cased.
		static std::map<std::string, CoverRegion> ParsePins(std::string_view text);
		std::string FormatPins() const;
	};

	/// Where to ask for one game's cover.
	struct CoverUrls
	{
		std::string primary; ///< The region-swapped cover, or the disc's own when nothing swaps.
		std::string fallback; ///< The disc's own cover when `primary` differs from it, else empty.
		std::string cover_serial; ///< The serial `primary` was built from.

		bool Valid() const { return !primary.empty(); }
	};

	/// Resolves the URLs for `serial`. `index` may be null (not built, or not needed): then the
	/// disc's own serial is used and `fallback` is empty. Arcade IDs never swap.
	CoverUrls ResolveCoverUrls(CoverPlatform platform, std::string_view serial, const CoverPreferences& prefs, const CoverRegionIndex* index);
} // namespace Catalog
