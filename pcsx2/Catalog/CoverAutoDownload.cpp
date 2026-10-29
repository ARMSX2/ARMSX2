// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/CoverAutoDownload.h"

#include "Catalog/Fetch.h"

#include "Config.h"
#include "EmbeddedResources.h"
#include "Host.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>

namespace Catalog
{
	namespace AutoCovers
	{
		static CoverJobRunner s_runner;

		static constexpr char GAMEINDEX_FILE_NAME[] = "GameIndex.yaml";

		bool IsEnabled(const SettingsInterface& si)
		{
			return si.GetBoolValue(SETTINGS_SECTION, KEY_AUTO_DOWNLOAD, DEFAULT_AUTO_DOWNLOAD);
		}

		CoverPreferences LoadPreferences(const SettingsInterface& si)
		{
			CoverPreferences prefs;
			prefs.style = si.GetBoolValue(SETTINGS_SECTION, KEY_STYLE_3D, false) ? CoverStyle::Case3D : CoverStyle::Flat2D;

			const int region = si.GetIntValue(SETTINGS_SECTION, KEY_REGION, 0);
			prefs.region = static_cast<CoverRegion>(std::clamp(region, 0, COVER_REGION_COUNT - 1));

			prefs.pins = CoverPreferences::ParsePins(si.GetStringValue(SETTINGS_SECTION, KEY_REGION_PER_GAME, ""));
			return prefs;
		}

		void SetStyle(SettingsInterface& si, CoverStyle style)
		{
			si.SetBoolValue(SETTINGS_SECTION, KEY_STYLE_3D, style == CoverStyle::Case3D);
		}

		void SetLibraryRegion(SettingsInterface& si, CoverRegion region)
		{
			si.SetIntValue(SETTINGS_SECTION, KEY_REGION, static_cast<int>(region));
		}

		void SetGameRegionPin(SettingsInterface& si, const std::string& serial, std::optional<CoverRegion> pin)
		{
			CoverPreferences prefs = LoadPreferences(si);
			prefs.SetPin(serial, pin);
			if (prefs.pins.empty())
				si.DeleteValue(SETTINGS_SECTION, KEY_REGION_PER_GAME);
			else
				si.SetStringValue(SETTINGS_SECTION, KEY_REGION_PER_GAME, prefs.FormatPins().c_str());
		}

		// The host forms: the base layer under the settings lock. With no base layer (a tool
		// that never set one) they read the defaults and write nothing.
		template <typename Fn>
		static void ModifyBaseSettings(Fn&& fn)
		{
			{
				auto lock = Host::GetSettingsLock();
				if (SettingsInterface* si = Host::Internal::GetBaseSettingsLayer())
					fn(*si);
			}
			Host::CommitBaseSettingChanges();
		}

		bool IsEnabled()
		{
			auto lock = Host::GetSettingsLock();
			const SettingsInterface* si = Host::Internal::GetBaseSettingsLayer();
			return si ? IsEnabled(*si) : DEFAULT_AUTO_DOWNLOAD;
		}

		CoverPreferences LoadPreferences()
		{
			auto lock = Host::GetSettingsLock();
			const SettingsInterface* si = Host::Internal::GetBaseSettingsLayer();
			return si ? LoadPreferences(*si) : CoverPreferences();
		}

		void SetStyle(CoverStyle style)
		{
			ModifyBaseSettings([&](SettingsInterface& si) { SetStyle(si, style); });
		}

		void SetLibraryRegion(CoverRegion region)
		{
			ModifyBaseSettings([&](SettingsInterface& si) { SetLibraryRegion(si, region); });
		}

		void SetGameRegionPin(const std::string& serial, std::optional<CoverRegion> pin)
		{
			ModifyBaseSettings([&](SettingsInterface& si) { SetGameRegionPin(si, serial, pin); });
		}

		std::optional<CoverWorkItem> WorkItemForEntry(const GameList::Entry& entry)
		{
			CoverWorkItem item;
			switch (entry.type)
			{
				case GameList::EntryType::PS2Disc:
					item.platform = CoverPlatform::PS2;
					break;
				case GameList::EntryType::PS1Disc:
					item.platform = CoverPlatform::PS1;
					break;
				case GameList::EntryType::Arcade:
					item.platform = CoverPlatform::Arcade;
					if (!IsArcadeGameId(entry.serial))
						return std::nullopt;
					break;
				default:
					return std::nullopt;
			}

			if (!IsUsableCoverSerial(entry.serial))
				return std::nullopt;

			item.id = entry.path;
			item.serial = entry.serial;
			item.title = entry.title;
			return item;
		}

		std::vector<CoverWorkItem> CollectMissingCovers()
		{
			std::vector<CoverWorkItem> items;
			auto lock = GameList::GetLock();
			const u32 count = GameList::GetEntryCount();
			for (u32 i = 0; i < count; i++)
			{
				const GameList::Entry* entry = GameList::GetEntryByIndex(i);
				if (!entry)
					continue;

				std::optional<CoverWorkItem> item = WorkItemForEntry(*entry);
				if (!item.has_value() || !GameList::GetCoverImagePathForEntry(entry).empty())
					continue;
				items.push_back(std::move(*item));
			}
			return items;
		}

		// The game database text, from the resources folder or, in builds that carry one, from the
		// copy compiled into the binary (the same order GameDatabase uses).
		static std::shared_ptr<const CoverRegionIndex> LoadRegionIndex(const std::string& resources_dir)
		{
			std::optional<std::string> yaml = FileSystem::ReadFileToString(Path::Combine(resources_dir, GAMEINDEX_FILE_NAME).c_str());
#ifdef ARMSX2_EMBEDDED_RESOURCES
			if (!yaml.has_value())
				yaml = EmbeddedResources::Read(GAMEINDEX_FILE_NAME);
#endif
			if (!yaml.has_value())
			{
				Console.WarningFmt("Covers: {} not found, covers keep the disc's own region", GAMEINDEX_FILE_NAME);
				return nullptr;
			}
			return std::make_shared<const CoverRegionIndex>(CoverRegionIndex::FromGameIndexYaml(*yaml));
		}

		// Everything a run reads from the host is captured here, on the calling thread, so the
		// worker never touches the settings or the folder globals.
		static CoverJobOptions MakeOptions(Callbacks& callbacks)
		{
			CoverJobOptions options;
			options.covers_dir = EmuFolders::Covers;
			options.preferences = LoadPreferences();
			options.miss_cache = std::make_shared<CoverMissCache>(Path::Combine(Path::Combine(EmuFolders::Cache, "covers"), "misses.json"));
			options.load_region_index = [resources_dir = EmuFolders::Resources]() { return LoadRegionIndex(resources_dir); };
			options.has_cover = [](const CoverWorkItem& item) {
				// Same re-check GameList::DownloadCovers makes before each request. A game that
				// left the list counts as done.
				auto lock = GameList::GetLock();
				const GameList::Entry* entry = GameList::GetEntryForPath(item.id.c_str());
				return !entry || !GameList::GetCoverImagePathForEntry(entry).empty();
			};
			options.on_progress = std::move(callbacks.on_progress);
			options.on_cover_saved = std::move(callbacks.on_cover_saved);
			return options;
		}

		bool StartAfterRefresh(Callbacks callbacks, Fetcher* fetcher)
		{
			if (!IsEnabled() || s_runner.IsRunning())
				return false;

			std::vector<CoverWorkItem> items = CollectMissingCovers();
			if (items.empty())
				return false;

			CoverJobOptions options = MakeOptions(callbacks);
			return s_runner.Start(fetcher ? *fetcher : GetDefaultFetcher(), std::move(items), std::move(options), std::move(callbacks.on_finished));
		}

		bool StartRedownload(const std::vector<std::string>& game_paths, Callbacks callbacks, Fetcher* fetcher)
		{
			if (s_runner.IsRunning())
				return false;

			std::vector<CoverWorkItem> items;
			{
				auto lock = GameList::GetLock();
				for (const std::string& path : game_paths)
				{
					const GameList::Entry* entry = GameList::GetEntryForPath(path.c_str());
					if (!entry)
						continue;

					std::optional<CoverWorkItem> item = WorkItemForEntry(*entry);
					if (!item.has_value())
						continue;
					item->force = true;
					item->replace_path = GameList::GetCoverImagePathForEntry(entry);
					items.push_back(std::move(*item));
				}
			}
			if (items.empty())
				return false;

			CoverJobOptions options = MakeOptions(callbacks);
			return s_runner.Start(fetcher ? *fetcher : GetDefaultFetcher(), std::move(items), std::move(options), std::move(callbacks.on_finished));
		}

		bool IsRunning()
		{
			return s_runner.IsRunning();
		}

		void Cancel()
		{
			s_runner.Cancel();
		}

		void Shutdown()
		{
			s_runner.Shutdown();
		}
	} // namespace AutoCovers
} // namespace Catalog
