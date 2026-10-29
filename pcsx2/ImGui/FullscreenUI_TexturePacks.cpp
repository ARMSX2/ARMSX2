// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture's Texture Packs page (SettingsPage::TexturePacks). The other FullscreenUI files call in
// here with one line each (declarations at the end of FullscreenUI_Internal.h).
//
// Threads. All state below belongs to the GS thread, which draws the page. The catalog download, the
// install and the disk work (listing and deleting installed packs) run on worker threads and hand
// their results over under s_handoff.mutex:
//
//   * The catalog and installed-pack results are picked up by the page's own draw (PollHandoff), so
//     they only matter while the page is open.
//   * An install keeps reporting while the page is closed, because its progress is a keyed
//     notification. Its reports hop worker -> CPU thread -> GS thread, as the cover downloader's do
//     (see PostToGSThread).
//
// The page runs under the settings lock (DrawSettingsWindow holds it around every page body), which is
// a plain mutex: nothing drawn here may call a helper that takes Host::GetSettingsLock(). Work that
// reads settings or the game list (the online-features check, the library serials) runs on workers.

#include "Catalog/CoverAutoDownload.h"
#include "Catalog/TexturePackCatalog.h"
#include "Catalog/TexturePackEngine.h"
#include "Catalog/TexturePackInstaller.h"
#include "Catalog/TexturePackState.h"

#include "Achievements.h"
#include "Config.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "ImGui/FullscreenUI_Internal.h"
#include "ImGui/FullscreenUI_PageDwell.h"
#include "ImGui/ImGuiFullscreen.h"
#include "MTGS.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/SHA256.h"
#include "common/SettingsInterface.h"

#include "IconsFontAwesome.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Cat = Catalog;
namespace TPEngine = Catalog::TexturePackEngine;
namespace AutoCovers = Catalog::AutoCovers;

namespace
{
	using namespace FullscreenUI;

	constexpr const char* INSTALL_NOTIFICATION_KEY = "texture_pack_install";
	constexpr const char* DELETE_NOTIFICATION_KEY = "texture_pack_delete";

	// Height of a pack row in layout units: the pack's name and two lines of facts under it.
	constexpr float PACK_ROW_HEIGHT = 66.0f;

	// How long a popularity list is trusted when the page is opened again.
	constexpr std::chrono::seconds POPULAR_MAX_AGE(60);

	enum class ListFilter : int
	{
		Popular,
		All,
		MyGames,
		Installed,
	};

	struct PackEntry
	{
		Cat::TexturePack pack;
		std::string search_key; ///< lower case: name, game, serials, creators
		std::string sort_key; ///< lower case: game, then name
	};

	std::vector<PackEntry> s_packs; ///< sorted by game, then name
	std::unordered_map<std::string, size_t> s_pack_index; ///< pack id -> s_packs index
	bool s_catalog_loaded = false;
	Cat::TexturePackCatalog::Source s_catalog_source = Cat::TexturePackCatalog::Source::None;
	std::time_t s_catalog_fetched_at = 0;

	bool s_popular_known = false; ///< the counter answered (the list may be empty)
	std::vector<std::string> s_popular;
	std::chrono::steady_clock::time_point s_popular_asked_at;

	std::vector<Cat::TexturePackInstaller::InstalledPack> s_installed;
	bool s_installed_loaded = false;
	std::unordered_set<std::string> s_library_serials; ///< upper-case "SLUS-21410", from the game list
	std::unordered_map<std::string, std::string> s_library_titles; ///< serial -> title
	std::optional<u64> s_free_bytes; ///< free space on the textures drive
	bool s_local_again = false; ///< the installed packs changed while a listing was running
	std::string s_pending_delete_folder; ///< a delete waiting for the listing that is running
	std::string s_pending_delete_name;

	std::map<std::string, Cat::TexturePackRecord> s_records;
	u32 s_records_revision = ~0u;

	TPEngine::AstcSupport s_astc = TPEngine::AstcSupport::Unknown;

	ListFilter s_filter = ListFilter::Popular;
	std::string s_search;
	std::string s_filter_serial; ///< when set, only packs for this game are listed
	std::string s_filter_game_settings_path; ///< that game's own settings file, when the page was opened from its settings
	std::string s_filter_serial_title;
	bool s_filter_serial_requested = false; ///< the page was opened for a game, not through the page bar
	bool s_opened_before = false;
	int s_last_draw_frame = -100;
	PageDwell s_dwell; ///< the network waits until the page has stayed open (see OnPageSettled)
	bool s_fetch_deferred = false; ///< the page wants the network and is waiting for s_dwell
	bool s_install_notice_up = false; ///< a progress notification may be on screen

	std::vector<u32> s_view; ///< indices into s_packs, or into s_installed for the Installed filter
	bool s_view_dirty = true;

	// What the detail window shows: a catalog pack, an installed folder, or both.
	struct DetailState
	{
		bool open = false;
		std::string pack_id; ///< empty for a hand-copied folder
		std::string folder; ///< the installed folder's name; empty when nothing is installed
	};
	DetailState s_detail;

	struct InstallUi
	{
		bool active = false;
		bool cancelling = false;
		bool cancellable = true;
		std::string pack_id;
		std::string pack_name;
		Cat::TexturePackInstaller::Progress progress;
		Cat::CancelToken cancel;
		std::string last_message; ///< why the last install did not finish, shown on the page
	};
	InstallUi s_install;

	// Harness only: something to do once the catalog and the installed packs are in (see OpenTexturePacksForShot).
	enum class ShotAction
	{
		None,
		Detail, ///< open the pack window
		ConfirmDelete, ///< ask whether to delete an installed pack
		Delete, ///< delete it, as if the answer was yes
	};
	ShotAction s_shot_action = ShotAction::None;
	std::string s_shot_id;

	struct LocalResult
	{
		std::vector<Cat::TexturePackInstaller::InstalledPack> installed;
		std::unordered_set<std::string> library_serials;
		std::unordered_map<std::string, std::string> library_titles;
		std::optional<u64> free_bytes;
		bool was_delete = false;
		bool delete_ok = false;
		std::string delete_name;
	};

	struct InstallOutcome
	{
		Cat::TexturePackInstaller::Status status = Cat::TexturePackInstaller::Status::Failed;
		std::string error;
		std::string pack_name;
		u64 textures = 0;
		std::string game_settings_path; ///< the game's own settings file, when the install was made from its settings
	};

	struct Handoff
	{
		std::mutex mutex;

		std::optional<Cat::TexturePackCatalog::LoadResult> catalog;
		bool popular_ready = false;
		std::optional<std::vector<std::string>> popular; ///< nullopt: the counter could not be reached
		std::optional<LocalResult> local;

		bool install_drain_queued = false;
		std::optional<Cat::TexturePackInstaller::Progress> install_progress;
		std::optional<InstallOutcome> install_finished;
	};
	Handoff s_handoff;

	// A worker thread that does one job at a time. `running` is cleared by the job itself, last, so a
	// worker that is not running has posted everything it was going to.
	struct Worker
	{
		std::thread thread;
		std::atomic_bool running{false};
		Cat::CancelToken cancel;

		~Worker()
		{
			cancel.Cancel();
			Join();
		}

		void Join()
		{
			if (thread.joinable())
				thread.join();
		}

		void JoinIfIdle()
		{
			if (!running.load() && thread.joinable())
				thread.join();
		}

		// False if the previous job is still going.
		bool Start(Cat::CancelToken token, std::function<void()> job)
		{
			JoinIfIdle();
			if (running.load())
				return false;
			cancel = std::move(token);
			running.store(true);
			thread = std::thread([this, job = std::move(job)]() {
				job();
				running.store(false);
			});
			return true;
		}
	};

	Worker s_catalog_worker;
	Worker s_local_worker;
	Worker s_install_worker;

	// A worker's thread is not the CPU thread, and a push into the GS ring is only allowed from the CPU
	// thread (MTGS::RunOnGSThread), so hop through it.
	void PostToGSThread(std::function<void()> function)
	{
		Host::RunOnCPUThread([function = std::move(function)]() mutable {
			if (MTGS::IsOpen())
				MTGS::RunOnGSThread(std::move(function));
		});
	}

	std::string Lower(std::string_view text)
	{
		std::string out(text);
		for (char& c : out)
		{
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		}
		return out;
	}

	std::string Trimmed(std::string_view text)
	{
		const size_t first = text.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos)
			return {};
		const size_t last = text.find_last_not_of(" \t\r\n");
		return std::string(text.substr(first, last - first + 1));
	}

	// Joins the non-empty parts with a middle dot.
	std::string JoinFacts(std::initializer_list<std::string> parts)
	{
		std::string out;
		for (const std::string& part : parts)
		{
			if (part.empty())
				continue;
			if (!out.empty())
				out += " \xC2\xB7 ";
			out += part;
		}
		return out;
	}

	std::string TypeLabel(const std::string& raw)
	{
		if (raw == "ai_upscale")
			return FSUI_STR("AI Upscale");
		if (raw == "handcrafted")
			return FSUI_STR("Handcrafted");
		if (raw == "mixed")
			return FSUI_STR("Mixed");
		if (raw == "port")
			return FSUI_STR("Port");
		if (raw == "button_replacement")
			return FSUI_STR("Button Replacement");
		return raw;
	}

	std::string CompletenessLabel(const std::string& raw)
	{
		if (raw == "complete")
			return FSUI_STR("Complete");
		if (raw == "in_progress")
			return FSUI_STR("In Progress");
		if (raw == "incomplete")
			return FSUI_STR("Incomplete");
		if (raw == "partial")
			return FSUI_STR("Partial");
		return raw;
	}

	// Says nobody has named the creator when the creators list is empty, and never shows the "Unknown
	// creator" placeholder that older catalog files keep in `authors`.
	std::string CreatorsText(const Cat::TexturePack& pack)
	{
		if (pack.HasNamedCreators())
			return pack.CreatorText();
		return FSUI_STR("No one has named the creator");
	}

	std::string SerialsText(const Cat::TexturePack& pack)
	{
		std::string out;
		const size_t shown = std::min<size_t>(pack.serials.size(), 3);
		for (size_t i = 0; i < shown; i++)
		{
			if (i)
				out += ", ";
			out += pack.serials[i];
		}
		if (pack.serials.size() > shown)
			out += fmt::format(" +{}", pack.serials.size() - shown);
		return out;
	}

	const char* FilterName(ListFilter filter)
	{
		switch (filter)
		{
			case ListFilter::Popular:
				return FSUI_CSTR("Popular Today");
			case ListFilter::All:
				return FSUI_CSTR("All");
			case ListFilter::MyGames:
				return FSUI_CSTR("My Games");
			case ListFilter::Installed:
				break;
		}
		return FSUI_CSTR("Installed");
	}

	const char* ActionLabel(Cat::InstallAction action)
	{
		switch (action)
		{
			case Cat::InstallAction::Install:
				return FSUI_CSTR("Download");
			case Cat::InstallAction::Update:
				return FSUI_CSTR("Update");
			case Cat::InstallAction::Installed:
				return FSUI_CSTR("Installed");
			case Cat::InstallAction::Conflict:
				break;
		}
		return FSUI_CSTR("Conflict");
	}

	std::string AgeText(std::time_t then)
	{
		const std::time_t now = std::time(nullptr);
		const s64 seconds = now > then ? static_cast<s64>(now - then) : 0;
		if (seconds < 90)
			return FSUI_STR("just now");
		if (seconds < 90 * 60)
			return fmt::format(FSUI_FSTR("{} minutes ago"), (seconds + 30) / 60);
		if (seconds < 36 * 3600)
			return fmt::format(FSUI_FSTR("{} hours ago"), (seconds + 1800) / 3600);
		return fmt::format(FSUI_FSTR("{} days ago"), (seconds + 43200) / 86400);
	}

	// "Downloading 241 MB of 1.2 GB (19%)", "Extracting 32 / 4277 files", ...
	std::string ProgressText(const Cat::TexturePackInstaller::Progress& progress)
	{
		using Phase = Cat::TexturePackInstaller::Phase;
		switch (progress.phase)
		{
			case Phase::Starting:
				return FSUI_STR("Starting...");
			case Phase::Downloading:
			{
				std::string text = fmt::format(
					FSUI_FSTR("Downloading {} of {}"), Cat::FormatPackSize(progress.done), Cat::FormatPackSize(progress.total));
				if (progress.total > 0)
					text += fmt::format(" ({}%)", std::min<u64>(100, progress.done * 100 / progress.total));
				return text;
			}
			case Phase::Verifying:
				return FSUI_STR("Verifying download...");
			case Phase::Extracting:
				return fmt::format(FSUI_FSTR("Extracting {} / {} files"), progress.done, progress.total);
			case Phase::Installing:
				break;
		}
		return FSUI_STR("Installing...");
	}

	// 0..1 for the phases that have a length, a negative number otherwise.
	float ProgressFraction(const Cat::TexturePackInstaller::Progress& progress)
	{
		using Phase = Cat::TexturePackInstaller::Phase;
		if ((progress.phase == Phase::Downloading || progress.phase == Phase::Extracting) && progress.total > 0)
			return std::min(1.0f, static_cast<float>(static_cast<double>(progress.done) / static_cast<double>(progress.total)));
		if (progress.phase == Phase::Verifying || progress.phase == Phase::Installing)
			return 1.0f;
		return -1.0f;
	}

	// Shortens `text` so it fits in `width` pixels, ending in an ellipsis when it had to cut.
	std::string FitText(const std::pair<ImFont*, float>& font, const std::string& text, float width)
	{
		const char* begin = text.c_str();
		const char* end = begin + text.size();
		if (font.first->CalcTextSizeA(font.second, FLT_MAX, 0.0f, begin, end).x <= width)
			return text;

		const float ellipsis_width = font.first->CalcTextSizeA(font.second, FLT_MAX, 0.0f, g_ellipsis).x;
		const char* cut = end;
		font.first->CalcTextSizeA(font.second, std::max(0.0f, width - ellipsis_width), 0.0f, begin, end, &cut);
		return std::string(begin, cut) + g_ellipsis;
	}

	void StartLocalRefresh(std::string delete_folder = {}, std::string delete_name = {});

	void RefreshRecordsIfChanged(bool force)
	{
		const u32 revision = Cat::TexturePackState::Revision();
		if (!force && revision == s_records_revision)
			return;
		s_records_revision = revision;
		s_records = Cat::TexturePackState::InDataRoot().All();
		s_view_dirty = true;
	}

	// TexturePackState::ActionForPack() reads the record file on every call; with the records held here
	// the same rule (ActionFor) answers for a whole page of rows.
	Cat::InstallAction ActionOf(const Cat::TexturePack& pack)
	{
		std::optional<Cat::TexturePackRecord> record;
		const auto it = s_records.find(pack.id);
		if (it != s_records.end())
			record = it->second;
		return Cat::TexturePackState::ActionFor(record, pack);
	}

	const PackEntry* FindPack(const std::string& id)
	{
		if (id.empty())
			return nullptr;
		const auto it = s_pack_index.find(id);
		return it == s_pack_index.end() ? nullptr : &s_packs[it->second];
	}

	const Cat::TexturePackInstaller::InstalledPack* FindInstalledByFolder(const std::string& folder)
	{
		for (const auto& installed : s_installed)
		{
			if (installed.serial == folder)
				return &installed;
		}
		return nullptr;
	}

	const Cat::TexturePackInstaller::InstalledPack* FindInstalledForPack(const std::string& pack_id)
	{
		for (const auto& installed : s_installed)
		{
			if (installed.record.has_value() && installed.record->pack_id == pack_id)
				return &installed;
		}
		return nullptr;
	}

	// The game an installed folder belongs to, as well as we can tell.
	std::string InstalledGameTitle(const Cat::TexturePackInstaller::InstalledPack& installed)
	{
		if (installed.record.has_value())
		{
			if (const PackEntry* entry = FindPack(installed.record->pack_id); entry && !entry->pack.game_title.empty())
				return entry->pack.game_title;
		}
		const auto it = s_library_titles.find(installed.serial);
		return it == s_library_titles.end() ? std::string() : it->second;
	}

	// What the pack is called: the catalog's name for it, or for a folder somebody copied in, its game.
	std::string InstalledName(const Cat::TexturePackInstaller::InstalledPack& installed)
	{
		if (installed.record.has_value() && !installed.record->name.empty())
			return installed.record->name;
		const std::string game = InstalledGameTitle(installed);
		if (!game.empty())
			return game;
		return fmt::format(FSUI_FSTR("Folder {}"), installed.serial);
	}

	// The serial a download goes under when the player is looking at a particular game: the game being
	// edited or played, if the pack covers it.
	std::string ContextSerial()
	{
		if (!s_filter_serial.empty())
			return s_filter_serial;
		if (VMManager::HasValidVM())
		{
			if (const std::optional<std::string> serial = Cat::NormalizeSerial(s_current_disc_serial))
				return *serial;
		}
		return {};
	}

	void RebuildView()
	{
		s_view_dirty = false;
		s_view.clear();

		const std::string needle = Lower(Trimmed(s_search));
		// "slus21410" and "SLUS_21410" find SLUS-21410.
		const std::optional<std::string> needle_serial = Cat::NormalizeSerial(Trimmed(s_search));
		const std::string context = ContextSerial();

		if (s_filter == ListFilter::Installed)
		{
			for (size_t i = 0; i < s_installed.size(); i++)
			{
				const auto& installed = s_installed[i];
				if (!s_filter_serial.empty() && installed.serial != s_filter_serial)
					continue;
				if (!needle.empty())
				{
					const std::string haystack = Lower(JoinFacts({InstalledName(installed), installed.serial, InstalledGameTitle(installed)}));
					if (haystack.find(needle) == std::string::npos)
						continue;
				}
				s_view.push_back(static_cast<u32>(i));
			}
			return;
		}

		const auto wanted = [&](const PackEntry& entry) {
			if (!s_filter_serial.empty() && !entry.pack.MatchesSerial(s_filter_serial))
				return false;
			return needle.empty() || entry.search_key.find(needle) != std::string::npos ||
				   (needle_serial.has_value() && entry.pack.MatchesSerial(*needle_serial));
		};

		switch (s_filter)
		{
			case ListFilter::Popular:
				// In the counter's order, most downloaded first.
				for (const std::string& id : s_popular)
				{
					const auto it = s_pack_index.find(id);
					if (it != s_pack_index.end() && wanted(s_packs[it->second]))
						s_view.push_back(static_cast<u32>(it->second));
				}
				break;

			case ListFilter::All:
				for (size_t i = 0; i < s_packs.size(); i++)
				{
					if (wanted(s_packs[i]))
						s_view.push_back(static_cast<u32>(i));
				}
				break;

			case ListFilter::MyGames:
				for (size_t i = 0; i < s_packs.size(); i++)
				{
					const Cat::TexturePack& pack = s_packs[i].pack;
					if (wanted(s_packs[i]) && (pack.CoversAnyOf(s_library_serials) || (!context.empty() && pack.MatchesSerial(context))))
						s_view.push_back(static_cast<u32>(i));
				}
				break;

			case ListFilter::Installed:
				break;
		}
	}

	void SetFilter(ListFilter filter)
	{
		s_filter = filter;
		s_view_dirty = true;
	}

	void ApplyCatalog(Cat::TexturePackCatalog::LoadResult result)
	{
		if (result.cancelled)
			return;
		if (!result.IsOk())
		{
			Console.Warning("Texture packs: the catalog could not be loaded: %s", result.error.c_str());
			return; // keep what we had, and let the catalog row say so
		}

		std::vector<PackEntry> packs;
		packs.reserve(result.packs.size());
		for (Cat::TexturePack& pack : result.packs)
		{
			PackEntry entry;
			std::string key = JoinFacts({pack.name, pack.game_title, SerialsText(pack), pack.CreatorText()});
			for (size_t i = 3; i < pack.serials.size(); i++)
				key += " " + pack.serials[i];
			entry.search_key = Lower(key);
			entry.sort_key = Lower(JoinFacts({pack.game_title.empty() ? pack.name : pack.game_title, pack.name}));
			entry.pack = std::move(pack);
			packs.push_back(std::move(entry));
		}

		// By game, then by pack name: the order the player thinks in, and the same on every start.
		std::stable_sort(packs.begin(), packs.end(), [](const PackEntry& a, const PackEntry& b) { return a.sort_key < b.sort_key; });

		s_packs = std::move(packs);
		s_pack_index.clear();
		for (size_t i = 0; i < s_packs.size(); i++)
			s_pack_index[s_packs[i].pack.id] = i;

		s_catalog_loaded = true;
		s_catalog_source = result.source;
		s_catalog_fetched_at = result.fetched_at;
		s_view_dirty = true;
	}

	void ApplyLocal(LocalResult local)
	{
		s_installed = std::move(local.installed);
		std::sort(s_installed.begin(), s_installed.end(), [](const auto& a, const auto& b) { return a.serial < b.serial; });
		s_installed_loaded = true;
		s_library_serials = std::move(local.library_serials);
		s_library_titles = std::move(local.library_titles);
		s_free_bytes = local.free_bytes;
		s_view_dirty = true;
		// The listing reconciles the install records with the disk.
		RefreshRecordsIfChanged(true);

		if (local.was_delete)
		{
			ImGuiFullscreen::AddNotification(DELETE_NOTIFICATION_KEY, 6.0f,
				fmt::format("{} {}", ICON_FA_IMAGES, local.delete_ok ? FSUI_STR("Texture Pack Deleted") : FSUI_STR("Texture Pack Not Deleted")),
				local.delete_ok ? local.delete_name :
								  fmt::format(FSUI_FSTR("{} could not be removed completely."), local.delete_name),
				std::string());
		}
	}

	void PollHandoff()
	{
		std::optional<Cat::TexturePackCatalog::LoadResult> catalog;
		std::optional<std::vector<std::string>> popular;
		bool popular_ready = false;
		std::optional<LocalResult> local;
		{
			std::lock_guard<std::mutex> lock(s_handoff.mutex);
			catalog.swap(s_handoff.catalog);
			if (s_handoff.popular_ready)
			{
				popular_ready = true;
				popular.swap(s_handoff.popular);
				s_handoff.popular_ready = false;
			}
			local.swap(s_handoff.local);
		}

		if (catalog.has_value())
			ApplyCatalog(std::move(*catalog));
		if (popular_ready)
		{
			s_popular_known = popular.has_value();
			s_popular = popular.has_value() ? std::move(*popular) : std::vector<std::string>();
			s_view_dirty = true;
		}
		if (local.has_value())
			ApplyLocal(std::move(*local));

		s_catalog_worker.JoinIfIdle();
		s_local_worker.JoinIfIdle();
		s_install_worker.JoinIfIdle();

		if (!s_local_worker.running.load())
		{
			if (!s_pending_delete_folder.empty())
			{
				std::string folder = std::move(s_pending_delete_folder);
				std::string name = std::move(s_pending_delete_name);
				s_pending_delete_folder.clear();
				s_pending_delete_name.clear();
				s_local_again = false;
				StartLocalRefresh(std::move(folder), std::move(name));
			}
			else if (s_local_again)
			{
				s_local_again = false;
				StartLocalRefresh();
			}
		}
	}

	std::optional<u64> FreeSpaceAt(const std::string& directory)
	{
		// The textures folder may not exist yet; the drive it will be on does.
		std::filesystem::path path(directory);
		for (int depth = 0; depth < 12 && !path.empty(); depth++)
		{
			std::error_code error;
			const std::filesystem::space_info info = std::filesystem::space(path, error);
			if (!error)
				return static_cast<u64>(info.available);
			path = path.parent_path();
		}
		return std::nullopt;
	}

	// Loads the catalog (and asks the counter for today's popular packs). `load_catalog` false asks
	// only the counter, for a page that is opened again with the catalog already in hand.
	void StartCatalogLoad(bool force, bool load_catalog)
	{
		if (s_catalog_worker.running.load())
			return;

		s_popular_asked_at = std::chrono::steady_clock::now();
		Cat::CancelToken cancel;
		s_catalog_worker.Start(cancel, [force, load_catalog, cancel]() {
			if (load_catalog)
			{
				Cat::TexturePackCatalog catalog(Cat::GetDefaultFetcher(), Cat::TexturePackCatalog::DefaultCache());
				Cat::TexturePackCatalog::LoadResult result = catalog.Load(force, cancel);
				const bool usable = result.IsOk();
				{
					std::lock_guard<std::mutex> lock(s_handoff.mutex);
					s_handoff.catalog = std::move(result);
				}
				if (!usable)
				{
					std::lock_guard<std::mutex> lock(s_handoff.mutex);
					s_handoff.popular.reset();
					s_handoff.popular_ready = true;
					return;
				}
			}
			if (cancel.IsCancelled())
				return;

			std::optional<std::vector<std::string>> popular = Cat::FetchPopularPackIds(Cat::GetDefaultFetcher(), cancel);
			std::lock_guard<std::mutex> lock(s_handoff.mutex);
			s_handoff.popular = std::move(popular);
			s_handoff.popular_ready = true;
		});
	}

	// Lists the installed packs, the games in the library and the free space on the textures drive;
	// optionally deletes one pack first. All of it is disk or game list work, so it is a worker's.
	void StartLocalRefresh(std::string delete_folder, std::string delete_name)
	{
		if (!delete_folder.empty() && s_install.active)
		{
			// The install is about to write into the folder that would be deleted from.
			ShowToast(ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("Not while a texture pack is being installed."));
			return;
		}

		if (s_local_worker.running.load())
		{
			// What is running is a listing (nothing else is started while it runs). A plain listing is asked
			// again once it has been picked up; a delete waits for it the same way, so it is never lost and
			// the GS thread never waits for a disk walk.
			if (delete_folder.empty())
			{
				s_local_again = true;
			}
			else
			{
				s_pending_delete_folder = std::move(delete_folder);
				s_pending_delete_name = std::move(delete_name);
			}
			return;
		}

		const Cat::TexturePackInstaller::Config config = Cat::TexturePackInstaller::DefaultConfig();
		const bool prefer_english = s_prefer_english_titles;
		Cat::CancelToken cancel;
		s_local_worker.Start(cancel, [config, prefer_english, delete_folder, delete_name, cancel]() {
			Cat::TexturePackState state = Cat::TexturePackState::InDataRoot();
			Cat::TexturePackInstaller installer(Cat::GetDefaultFetcher(), state, config);

			LocalResult out;
			if (!delete_folder.empty())
			{
				out.was_delete = true;
				out.delete_name = delete_name;
				out.delete_ok = installer.Remove(delete_folder);
				TPEngine::ReloadRunningGame();
			}
			// Shutting Big Picture down cancels the walk; nothing is handed over then.
			out.installed = installer.ListInstalled(cancel);
			if (cancel.IsCancelled())
				return;
			out.free_bytes = FreeSpaceAt(config.textures_dir);

			{
				auto lock = GameList::GetLock();
				const u32 count = GameList::GetEntryCount();
				for (u32 i = 0; i < count; i++)
				{
					const GameList::Entry* entry = GameList::GetEntryByIndex(i);
					if (!entry || entry->serial.empty())
						continue;
					if (const std::optional<std::string> serial = Cat::NormalizeSerial(entry->serial))
					{
						out.library_serials.insert(*serial);
						out.library_titles.emplace(*serial, entry->GetTitle(prefer_english));
					}
				}
			}

			std::lock_guard<std::mutex> lock(s_handoff.mutex);
			s_handoff.local = std::move(out);
		});
	}

	void ShowInstallNotice(const std::string& title, const std::string& text, float seconds)
	{
		if (!IsInitialized())
			return;
		ImGuiFullscreen::AddNotification(INSTALL_NOTIFICATION_KEY, seconds, fmt::format("{} {}", ICON_FA_IMAGES, title), text, std::string());
	}

	// The page draws the install's progress itself, with the Cancel button; a notification over it
	// would cover the top of the page, so the progress is only a notification while the page is closed.
	bool PageIsShowing()
	{
		return ImGui::GetCurrentContext() && (ImGui::GetFrameCount() - s_last_draw_frame) <= 2;
	}

	void DismissInstallProgressNotice()
	{
		if (!s_install_notice_up)
			return;
		s_install_notice_up = false;
		ImGuiFullscreen::AddNotification(INSTALL_NOTIFICATION_KEY, 0.0f, std::string(), std::string(), std::string());
	}

	void ShowInstallProgressNotice()
	{
		if (PageIsShowing())
		{
			DismissInstallProgressNotice();
			return;
		}
		s_install_notice_up = true;
		// Long enough to outlast a slow stretch; the next report, or the end of the install, replaces it.
		ShowInstallNotice(s_install.pack_name, ProgressText(s_install.progress), 15.0f);
	}

	void DrainInstallHandoff()
	{
		std::optional<Cat::TexturePackInstaller::Progress> progress;
		std::optional<InstallOutcome> finished;
		{
			std::lock_guard<std::mutex> lock(s_handoff.mutex);
			s_handoff.install_drain_queued = false;
			progress.swap(s_handoff.install_progress);
			finished.swap(s_handoff.install_finished);
		}

		// With the UI shut down there is nobody to tell, but the install did end: the state below must
		// still follow it, or the page would show an install that is over when it comes back.
		if (finished.has_value())
		{
			s_install.active = false;
			s_install.cancelling = false;
			s_install.cancellable = true;
			s_install_notice_up = false;

			switch (finished->status)
			{
				case Cat::TexturePackInstaller::Status::Ok:
					s_install.last_message.clear();
					ShowInstallNotice(FSUI_STR("Texture Pack Installed"),
						fmt::format(FSUI_FSTR("{}. The textures are used the next time the game starts."), finished->pack_name), 8.0f);
					// The install may have switched on this game's own setting in its file: the copy the game's
					// settings page holds open must not save the old value back.
					if (!finished->game_settings_path.empty() && s_game_settings_interface &&
						s_game_settings_interface->GetFileName() == finished->game_settings_path)
					{
						s_game_settings_interface->Load();
					}
					break;

				case Cat::TexturePackInstaller::Status::Cancelled:
					s_install.last_message.clear();
					ShowInstallNotice(finished->pack_name, FSUI_STR("Cancelled."), 4.0f);
					break;

				case Cat::TexturePackInstaller::Status::Busy:
				case Cat::TexturePackInstaller::Status::Failed:
					s_install.last_message = finished->error;
					ShowInstallNotice(FSUI_STR("Texture Pack Install Failed"), JoinFacts({finished->pack_name, finished->error}), 12.0f);
					break;
			}

			// The new pack, or the failed attempt's leftovers, change what is installed.
			StartLocalRefresh();
			s_view_dirty = true;
			return;
		}

		if (progress.has_value() && s_install.active)
		{
			s_install.progress = *progress;
			s_install.cancellable = (progress->phase != Cat::TexturePackInstaller::Phase::Installing);
			ShowInstallProgressNotice();
		}
	}

	// Does nothing once the pack is being put in place.
	void CancelInstall()
	{
		if (!s_install.active || !s_install.cancellable || s_install.cancelling)
			return;
		s_install.cancelling = true;
		s_install.cancel.Cancel();
	}

	void QueueInstallDrain()
	{
		{
			std::lock_guard<std::mutex> lock(s_handoff.mutex);
			if (s_handoff.install_drain_queued)
				return;
			s_handoff.install_drain_queued = true;
		}
		PostToGSThread(&DrainInstallHandoff);
	}

	// Installs `pack` for the player. `fetcher` null means the real network; only the screenshot harness
	// passes another. One install at a time, and only when the GS thread asks (so the UI state is safe).
	void StartInstall(const Cat::TexturePack& pack, Cat::Fetcher* fetcher = nullptr)
	{
		if (s_install.active || Cat::TexturePackInstaller::IsInstalling())
		{
			ShowToast(ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("A texture pack is already being installed."));
			return;
		}
		s_install_worker.JoinIfIdle();
		if (s_install_worker.running.load())
		{
			ShowToast(ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("Finishing the last install. Try again in a moment."));
			return;
		}

		const std::string serial = pack.InstallSerialFor(ContextSerial(), s_library_serials);
		// An install made from a game's settings must not be switched off by that game's own settings.
		const std::string game_settings_path = s_filter_serial.empty() ? std::string() : s_filter_game_settings_path;

		s_install = InstallUi();
		s_install.active = true;
		s_install.pack_id = pack.id;
		s_install.pack_name = pack.name;
		s_install.cancel = Cat::CancelToken();
		ShowInstallProgressNotice();

		const Cat::TexturePackInstaller::Config config = Cat::TexturePackInstaller::DefaultConfig();
		const Cat::CancelToken cancel = s_install.cancel;
		s_install_worker.Start(cancel, [pack, serial, game_settings_path, config, cancel, fetcher]() {
			Cat::Fetcher& net = fetcher ? *fetcher : Cat::GetDefaultFetcher();
			Cat::TexturePackState state = Cat::TexturePackState::InDataRoot();
			Cat::TexturePackInstaller installer(net, state, config);

			const Cat::TexturePackInstaller::InstallResult result = installer.Install(
				pack, serial,
				[](const Cat::TexturePackInstaller::Progress& progress) {
					{
						std::lock_guard<std::mutex> lock(s_handoff.mutex);
						s_handoff.install_progress = progress;
					}
					QueueInstallDrain();
				},
				cancel);

			if (result.IsOk())
			{
				TPEngine::ActivateAfterInstall();
				if (!game_settings_path.empty())
					TPEngine::EnableForGame(game_settings_path);
			}

			InstallOutcome outcome;
			outcome.status = result.status;
			outcome.error = result.error;
			outcome.pack_name = pack.name;
			outcome.textures = result.textures;
			outcome.game_settings_path = game_settings_path;
			{
				std::lock_guard<std::mutex> lock(s_handoff.mutex);
				s_handoff.install_progress.reset();
				s_handoff.install_finished = std::move(outcome);
			}
			QueueInstallDrain();

			// The popularity count, after the player has been told. It is only sent when the player allows
			// online features at all (the automatic cover download switch stands for that), and a slow
			// answer must not hold up the install's end.
			if (result.IsOk())
				Cat::PostInstallHit(net, pack.id, AutoCovers::IsEnabled(), cancel);
		});
	}

	struct RowText
	{
		std::string title;
		std::string line1;
		std::string line2;
		std::string value; ///< the state, large, top right
		std::string value2; ///< a fact under the state, small
		bool dim_value = false;
		bool warn = false;
	};

	// One pack or installed-folder row: the name, two lines of facts under it, and the state on the
	// right with a fact beneath it. make_text runs only for a row that is on screen: the list is
	// hundreds long.
	template <typename MakeText>
	bool DrawPackRow(const char* id, MakeText&& make_text)
	{
		ImVec2 bb_min, bb_max;
		bool visible, hovered;
		const bool pressed = ImGuiFullscreen::MenuButtonFrame(id, true, PACK_ROW_HEIGHT, &visible, &hovered, &bb_min, &bb_max);
		if (!visible)
			return false;
		const ImRect bb(bb_min, bb_max);

		const RowText text = make_text();
		const float gap = LayoutScale(12.0f);
		const std::pair<ImFont*, float> large = g_large_font;
		const std::pair<ImFont*, float> medium = g_medium_font;

		// The right edge: the state and the fact under it share a right-aligned column, and a warning
		// mark (amber) sits to the left of the state.
		const float value_width = text.value.empty() ? 0.0f : large.first->CalcTextSizeA(large.second, FLT_MAX, 0.0f, text.value.c_str()).x;
		const float value2_width = text.value2.empty() ? 0.0f : medium.first->CalcTextSizeA(medium.second, FLT_MAX, 0.0f, text.value2.c_str()).x;
		const float column_width = std::max(value_width, value2_width);
		const float warn_width = text.warn ? large.first->CalcTextSizeA(large.second, FLT_MAX, 0.0f, ICON_FA_TRIANGLE_EXCLAMATION).x + gap : 0.0f;
		const float right_zone = column_width + warn_width + (column_width > 0.0f || warn_width > 0.0f ? gap : 0.0f);
		const float text_right = bb.Max.x - right_zone;

		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float title_height = GetLineHeight(large) + LayoutScale(4.0f);
		const ImRect title_bb(bb.Min, ImVec2(text_right, bb.Min.y + title_height));

		ImGui::PushFont(large.first, large.second);
		ImGuiFullscreen::RenderTextClippedWithShadow(title_bb.Min, title_bb.Max, text.title.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f), &title_bb);

		if (text.warn)
		{
			const ImVec2 warn_pos(bb.Max.x - column_width - warn_width, bb.Min.y);
			ImGuiFullscreen::AddTextWithShadow(dl, large, warn_pos, ImGui::GetColorU32(ImVec4(1.0f, 0.78f, 0.34f, 1.0f)), ICON_FA_TRIANGLE_EXCLAMATION);
		}
		if (!text.value.empty())
		{
			if (text.dim_value)
				ImGui::PushStyleColor(ImGuiCol_Text, MulAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.60f));
			const ImRect value_bb(ImVec2(bb.Max.x - value_width, bb.Min.y), ImVec2(bb.Max.x, bb.Min.y + title_height));
			ImGuiFullscreen::RenderTextClippedWithShadow(value_bb.Min, value_bb.Max, text.value.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f), &value_bb);
			if (text.dim_value)
				ImGui::PopStyleColor();
		}
		ImGui::PopFont();

		// The facts, dimmed, one line each and cut at the state's edge.
		ImGui::PushFont(medium.first, medium.second);
		ImGui::PushStyleColor(ImGuiCol_Text, MulAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.60f));
		const float line_height = GetLineHeight(medium);
		const float available = std::max(0.0f, text_right - bb.Min.x);
		float y = bb.Min.y + title_height;
		for (const std::string* line : {&text.line1, &text.line2})
		{
			if (!line->empty())
			{
				const std::string fitted = FitText(medium, *line, available);
				const ImVec4 clip(bb.Min.x, y, text_right, y + line_height);
				ImGuiFullscreen::AddTextWithShadow(dl, medium, ImVec2(bb.Min.x, y), ImGui::GetColorU32(ImGuiCol_Text), fitted.c_str(),
					nullptr, 0.0f, &clip);
			}
			y += line_height;
		}
		if (!text.value2.empty())
		{
			const ImVec4 clip(bb.Max.x - value2_width, bb.Min.y + title_height, bb.Max.x, bb.Min.y + title_height + line_height);
			ImGuiFullscreen::AddTextWithShadow(dl, medium, ImVec2(bb.Max.x - value2_width, bb.Min.y + title_height),
				ImGui::GetColorU32(ImGuiCol_Text), text.value2.c_str(), nullptr, 0.0f, &clip);
		}
		ImGui::PopStyleColor();
		ImGui::PopFont();

		return pressed;
	}

	// A line of text in the list that is not a pack: why it is empty, or that it is still loading.
	void NoticeRow(const char* icon, const std::string& title, const std::string& summary)
	{
		const std::string text = fmt::format("{} {}", icon, title);
		MenuButton(text.c_str(), summary.empty() ? nullptr : summary.c_str(), false);
	}

	// Closes the pack window from outside it (the thing it describes has gone, or a screenshot moves on).
	// It is the only popup this page opens, so closing every popup closes just that.
	void ForceCloseDetail()
	{
		if (!s_detail.open)
			return;
		s_detail = DetailState();
		if (ImGui::GetCurrentContext())
			ImGui::ClosePopupToLevel(0, true);
		QueueResetFocus(FocusResetType::PopupClosed);
	}

	void OpenDetail(std::string pack_id, std::string folder)
	{
		s_detail.open = true;
		s_detail.pack_id = std::move(pack_id);
		s_detail.folder = std::move(folder);
		QueueResetFocus(FocusResetType::PopupOpened);
	}

	void DrawCatalogList()
	{
		const bool loading = s_catalog_worker.running.load() || s_fetch_deferred;

		std::string heading = FilterName(s_filter);
		if (s_catalog_loaded)
			heading = fmt::format("{} ({})", heading, s_view.size());
		MenuHeading(heading.c_str());

		if (s_view.empty())
		{
			if (!s_catalog_loaded)
			{
				if (loading)
					NoticeRow(ICON_FA_SPINNER, FSUI_STR("Loading Catalog..."), FSUI_STR("Fetching the list of texture packs."));
				else
					NoticeRow(ICON_FA_CIRCLE_XMARK, FSUI_STR("No Catalog"), FSUI_STR("The list of texture packs is not available. Use Retry above."));
			}
			else if (s_filter == ListFilter::Popular && !s_popular_known)
			{
				if (s_catalog_worker.running.load() || s_fetch_deferred)
					NoticeRow(ICON_FA_SPINNER, FSUI_STR("Loading..."), FSUI_STR("Asking which packs were downloaded today."));
				else
					NoticeRow(ICON_FA_WIFI, FSUI_STR("Can't Reach the Download Counter"), FSUI_STR("Can't reach the download counter right now. Choose All to see every pack."));
			}
			else if (s_filter == ListFilter::Popular && s_popular.empty())
			{
				NoticeRow(ICON_FA_CIRCLE_INFO, FSUI_STR("Nothing Yet"), FSUI_STR("Nothing has been downloaded yet today."));
			}
			else if (s_filter == ListFilter::MyGames && s_library_serials.empty() && s_filter_serial.empty())
			{
				NoticeRow(ICON_FA_CIRCLE_INFO, FSUI_STR("No Games"), FSUI_STR("There are no games in your library yet, so no pack can be matched to them."));
			}
			else
			{
				NoticeRow(ICON_FA_CIRCLE_INFO, FSUI_STR("No Packs Match"), FSUI_STR("No packs match."));
			}
			return;
		}

		const bool warn_astc = (s_astc == TPEngine::AstcSupport::Unsupported);
		for (const u32 index : s_view)
		{
			if (index >= s_packs.size())
				continue;
			const PackEntry& entry = s_packs[index];
			const Cat::TexturePack& pack = entry.pack;

			const bool pressed = DrawPackRow(pack.id.c_str(), [&]() {
				RowText text;
				text.title = pack.name;
				text.line1 = JoinFacts({pack.game_title, SerialsText(pack)});
				text.line2 = JoinFacts({CreatorsText(pack), TypeLabel(pack.type), CompletenessLabel(pack.completeness)});
				text.value2 = Cat::FormatPackSize(pack.size_bytes);
				const Cat::InstallAction action = ActionOf(pack);
				text.value = ActionLabel(action);
				text.dim_value = (action == Cat::InstallAction::Installed);
				text.warn = warn_astc && pack.NeedsAstc();
				return text;
			});
			if (pressed)
			{
				const Cat::TexturePackInstaller::InstalledPack* installed = FindInstalledForPack(pack.id);
				OpenDetail(pack.id, installed ? installed->serial : std::string());
			}
		}
	}

	void DrawInstalledList()
	{
		std::string heading = FilterName(s_filter);
		if (s_installed_loaded)
			heading = fmt::format("{} ({})", heading, s_view.size());
		MenuHeading(heading.c_str());

		if (!s_installed_loaded)
		{
			NoticeRow(ICON_FA_SPINNER, FSUI_STR("Loading..."), FSUI_STR("Looking at the packs in the textures folder."));
			return;
		}
		if (s_view.empty())
		{
			NoticeRow(ICON_FA_CIRCLE_INFO, FSUI_STR("No Packs Installed"),
				s_installed.empty() ? FSUI_STR("No texture packs are installed. Choose another list to download one.") : FSUI_STR("No packs match."));
			return;
		}

		for (const u32 index : s_view)
		{
			if (index >= s_installed.size())
				continue;
			const Cat::TexturePackInstaller::InstalledPack& installed = s_installed[index];

			const bool pressed = DrawPackRow(installed.serial.c_str(), [&]() {
				RowText text;
				text.title = InstalledName(installed);
				const std::string game = InstalledGameTitle(installed);
				text.line1 = (game == text.title) ? installed.serial : JoinFacts({game, installed.serial});
				std::string version;
				if (installed.record.has_value())
					version = installed.record->version;
				else
					version = FSUI_STR("Not from the catalog");
				text.line2 = JoinFacts({fmt::format(FSUI_FSTR("{} files"), installed.file_count), version});
				text.value2 = Cat::FormatPackSize(installed.bytes);

				// Whether the catalog has something newer for it.
				text.value = FSUI_STR("Installed");
				text.dim_value = true;
				if (installed.record.has_value())
				{
					if (const PackEntry* entry = FindPack(installed.record->pack_id))
					{
						const Cat::InstallAction action = ActionOf(entry->pack);
						if (action == Cat::InstallAction::Update || action == Cat::InstallAction::Conflict)
						{
							text.value = ActionLabel(action);
							text.dim_value = false;
						}
					}
				}
				return text;
			});
			if (pressed)
			{
				OpenDetail(installed.record.has_value() ? installed.record->pack_id : std::string(), installed.serial);
			}
		}
	}

	// A fact on one line. Selecting copies it, which also lets a controller reach every row.
	void InfoRow(const char* icon, const char* label, const std::string& value)
	{
		const std::string title = fmt::format("{} {}", icon, Host::TranslateToStringView(TR_CONTEXT, label));
		if (MenuButtonWithValue(title.c_str(), nullptr, value.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
			CopyTextToClipboard(FSUI_STR("Copied to clipboard."), value);
	}

	// A fact that is a sentence, a link or a path: the text goes under the label.
	void TextRow(const char* icon, const char* label, const std::string& value)
	{
		const std::string title = fmt::format("{} {}", icon, Host::TranslateToStringView(TR_CONTEXT, label));
		if (MenuButton(title.c_str(), value.c_str()))
			CopyTextToClipboard(FSUI_STR("Copied to clipboard."), value);
	}

	// Delete asks first: a pack is hundreds of megabytes the player may have waited for.
	void ConfirmDelete(const Cat::TexturePackInstaller::InstalledPack& installed)
	{
		const std::string name = InstalledName(installed);
		const std::string game = InstalledGameTitle(installed);
		const std::string game_text = game.empty() || game == name ? installed.serial : fmt::format("{} ({})", game, installed.serial);
		std::string message = fmt::format(
			FSUI_FSTR("Delete {} for {}? This removes {} files ({}) from the textures folder. You can download the pack again later."), name,
			game_text, installed.file_count, Cat::FormatPackSize(installed.bytes));
		const DetailState reopen = s_detail;

		OpenConfirmMessageDialog(FSUI_ICONSTR(ICON_FA_TRASH, "Delete Texture Pack"), std::move(message),
			[folder = installed.serial, name, reopen](bool yes) {
				if (yes)
					StartLocalRefresh(folder, name);
				else if (!reopen.pack_id.empty() || !reopen.folder.empty())
					OpenDetail(reopen.pack_id, reopen.folder);
			},
			false, FSUI_ICONSTR(ICON_FA_TRASH, "Delete"), FSUI_ICONSTR(ICON_FA_XMARK, "Cancel"));
	}

	// An install over a replacements folder the installer did not make (textures the player made or copied)
	// keeps that folder under another name; the player is told before it starts.
	void RequestInstall(const Cat::TexturePack& pack)
	{
		const std::string serial = pack.InstallSerialFor(ContextSerial(), s_library_serials);
		Cat::TexturePackState state = Cat::TexturePackState::InDataRoot();
		const Cat::TexturePackInstaller installer(Cat::GetDefaultFetcher(), state, Cat::TexturePackInstaller::DefaultConfig());
		if (!installer.HasUnrecordedReplacements(serial))
		{
			StartInstall(pack);
			return;
		}

		std::string message = fmt::format(
			FSUI_FSTR("{} already has texture replacements that were not installed from the catalog. The existing folder is kept as {}<date and time> and no longer used. Install the pack?"),
			serial, Cat::TexturePackInstaller::KEPT_FOLDER_PREFIX);
		OpenConfirmMessageDialog(FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Install Texture Pack"), std::move(message),
			[pack](bool yes) {
				if (yes)
					StartInstall(pack);
			},
			false, FSUI_ICONSTR(ICON_FA_DOWNLOAD, "Install"), FSUI_ICONSTR(ICON_FA_XMARK, "Cancel"));
	}

	void DrawDetailBody(const PackEntry* entry, const Cat::TexturePackInstaller::InstalledPack* installed, bool* close)
	{
		const Cat::TexturePack* pack = entry ? &entry->pack : nullptr;
		const bool installing_this = s_install.active && pack && s_install.pack_id == pack->id;

		if (pack)
		{
			const Cat::InstallAction action = ActionOf(*pack);
			const std::string serial = pack->InstallSerialFor(ContextSerial(), s_library_serials);

			const char* label = FSUI_CSTR("Download");
			std::string summary = fmt::format(FSUI_FSTR("Downloads {} and installs it for {}. Turns on texture replacement."),
				Cat::FormatPackSize(pack->size_bytes), serial);
			if (action == Cat::InstallAction::Update)
			{
				label = FSUI_CSTR("Update");
				summary = fmt::format(FSUI_FSTR("A newer version of this pack is available. Replaces the installed pack for {}."), serial);
			}
			else if (action == Cat::InstallAction::Conflict)
			{
				label = FSUI_CSTR("Update");
				summary = FSUI_STR("The installed pack differs from the catalog's copy of the same version. Updating replaces it.");
			}
			else if (action == Cat::InstallAction::Installed)
			{
				label = FSUI_CSTR("Installed");
				summary = FSUI_STR("This pack is installed and up to date.");
			}

			// Another pack for the same game is replaced, not added to.
			if (action == Cat::InstallAction::Install || action == Cat::InstallAction::Update)
			{
				for (const auto& [id, record] : s_records)
				{
					if (id != pack->id && record.serial == serial)
					{
						summary += " " + fmt::format(FSUI_FSTR("It replaces {}, which is installed for this game."), record.name);
						break;
					}
				}
			}

			const bool busy = s_install.active;
			if (installing_this)
				summary = ProgressText(s_install.progress);
			else if (busy)
				summary = FSUI_STR("Another texture pack is being installed. Wait for it to finish.");

			const bool can_install = !busy && action != Cat::InstallAction::Installed;
			const std::string title = fmt::format("{} {}", ICON_FA_DOWNLOAD, installing_this ? FSUI_STR("Installing...") : std::string(label));
			if (MenuButton(title.c_str(), summary.c_str(), can_install))
			{
				RequestInstall(*pack);
				*close = true;
			}
			if (can_install)
				ImGui::SetItemDefaultFocus();
		}

		if (installed)
		{
			const bool can_delete = !s_install.active;
			if (MenuButton(FSUI_ICONSTR(ICON_FA_TRASH, "Delete"),
					can_delete ? FSUI_CSTR("Removes this pack's textures from the textures folder. Asks first.") :
								 FSUI_CSTR("Not while a texture pack is being installed."),
					can_delete))
			{
				ConfirmDelete(*installed);
				*close = true;
			}
			if (!pack)
				ImGui::SetItemDefaultFocus();
		}

		if (MenuButton(FSUI_ICONSTR(ICON_FA_XMARK, "Close"), nullptr, true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
			*close = true;
		if (!pack && !installed)
			ImGui::SetItemDefaultFocus();

		if (pack && pack->NeedsAstc() && s_astc == TPEngine::AstcSupport::Unsupported)
		{
			const std::string title = fmt::format("{} {}", ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("This GPU Cannot Show These Textures"));
			MenuButton(title.c_str(),
				FSUI_CSTR("This pack is made of ASTC textures, which this GPU does not support, so the game would look unchanged. You can still install it."));
		}

		if (pack && s_free_bytes.has_value() && !(s_install.active && installing_this))
		{
			const u64 needed = Cat::TexturePackInstaller::RequiredFreeSpace(*pack);
			if (*s_free_bytes < needed && ActionOf(*pack) != Cat::InstallAction::Installed)
			{
				const std::string title = fmt::format("{} {}", ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("Not Enough Free Space"));
				const std::string summary = fmt::format(FSUI_FSTR("Installing needs {} free on the drive that holds the textures folder, and {} is free."),
					Cat::FormatPackSize(needed), Cat::FormatPackSize(*s_free_bytes));
				MenuButton(title.c_str(), summary.c_str());
			}
		}

		if (pack)
		{
			MenuHeading(FSUI_CSTR("Pack"));
			if (!pack->game_title.empty())
				InfoRow(ICON_FA_GAMEPAD, FSUI_NSTR("Game"), pack->game_title);
			InfoRow(ICON_FA_PAGER, FSUI_NSTR("Serials"), SerialsText(*pack));
			TextRow(ICON_FA_USERS, FSUI_NSTR("Creators"), CreatorsText(*pack));
			if (pack->HasNamedCreators())
			{
				const Cat::TexturePackCreator& lead = pack->creators.front();
				if (!lead.page.empty())
					TextRow(ICON_FA_LINK, FSUI_NSTR("Creator Page"), lead.page);
				if (!lead.tip.empty())
					TextRow(ICON_FA_HEART, FSUI_NSTR("Tip the Creator"), lead.tip);
				if (!lead.socials.empty())
					TextRow(ICON_FA_LINK, FSUI_NSTR("Socials"), lead.socials);
			}
			if (!pack->type.empty())
				InfoRow(ICON_FA_PALETTE, FSUI_NSTR("Texture Type"), TypeLabel(pack->type));
			if (!pack->completeness.empty())
				InfoRow(ICON_FA_CIRCLE_CHECK, FSUI_NSTR("Status"), CompletenessLabel(pack->completeness));
			if (!pack->version.empty())
				InfoRow(ICON_FA_TAG, FSUI_NSTR("Version"), pack->version);

			MenuHeading(FSUI_CSTR("Size"));
			InfoRow(ICON_FA_CLOUD_ARROW_DOWN, FSUI_NSTR("Download Size"),
				fmt::format(FSUI_FSTR("{}, {} files"), Cat::FormatPackSize(pack->size_bytes), pack->file_count));
			if (pack->decompressed_size_bytes > 0)
				InfoRow(ICON_FA_HARD_DRIVE, FSUI_NSTR("Installed Size"), Cat::FormatPackSize(pack->decompressed_size_bytes));
			{
				const u64 needed = Cat::TexturePackInstaller::RequiredFreeSpace(*pack);
				const std::string free_text =
					s_free_bytes.has_value() ? fmt::format(FSUI_FSTR("{} ({} free)"), Cat::FormatPackSize(needed), Cat::FormatPackSize(*s_free_bytes)) :
											   Cat::FormatPackSize(needed);
				InfoRow(ICON_FA_DATABASE, FSUI_NSTR("Free Space Needed"), free_text);
			}
			InfoRow(ICON_FA_FOLDER_OPEN, FSUI_NSTR("Installs Into"), pack->InstallSerialFor(ContextSerial(), s_library_serials));
			InfoRow(ICON_FA_IMAGES, FSUI_NSTR("Previews"),
				pack->preview_urls.empty() ? FSUI_STR("None") : fmt::format(FSUI_FSTR("{} preview images"), pack->preview_urls.size()));

			MenuHeading(FSUI_CSTR("About"));
			if (!pack->source_url.empty())
				TextRow(ICON_FA_LINK, FSUI_NSTR("Source"), pack->source_url);
			if (!pack->license.empty())
				TextRow(ICON_FA_SCALE_BALANCED, FSUI_NSTR("License"), pack->license);
			if (!pack->credits.empty())
				TextRow(ICON_FA_HEART, FSUI_NSTR("Credits"), pack->credits);
			if (!pack->description.empty())
				TextRow(ICON_FA_CIRCLE_INFO, FSUI_NSTR("Description"), pack->description);
		}

		if (installed)
		{
			MenuHeading(FSUI_CSTR("Installed"));
			if (!pack)
			{
				InfoRow(ICON_FA_PAGER, FSUI_NSTR("Serial"), installed->serial);
				const std::string game = InstalledGameTitle(*installed);
				if (!game.empty())
					InfoRow(ICON_FA_GAMEPAD, FSUI_NSTR("Game"), game);
				if (installed->record.has_value() && !installed->record->version.empty())
					InfoRow(ICON_FA_TAG, FSUI_NSTR("Version"), installed->record->version);
				else
					TextRow(ICON_FA_CIRCLE_INFO, FSUI_NSTR("Source"),
						FSUI_STR("These textures were not installed from the catalog, or the catalog does not list them any more."));
			}
			InfoRow(ICON_FA_IMAGES, FSUI_NSTR("Files"), fmt::format("{}", installed->file_count));
			InfoRow(ICON_FA_HARD_DRIVE, FSUI_NSTR("Size on Disk"), Cat::FormatPackSize(installed->bytes));
			TextRow(ICON_FA_FOLDER_OPEN, FSUI_NSTR("Folder"), installed->directory);
		}
	}

	void DrawDetailWindow()
	{
		if (!s_detail.open)
			return;

		const PackEntry* entry = FindPack(s_detail.pack_id);
		const Cat::TexturePackInstaller::InstalledPack* installed = s_detail.folder.empty() ? nullptr : FindInstalledByFolder(s_detail.folder);
		if (!entry && !installed)
		{
			// What it described has gone: the pack left the catalog while it was not installed, or its folder was removed.
			ForceCloseDetail();
			return;
		}

		const std::string name = entry ? entry->pack.name : InstalledName(*installed);
		const std::string window_id = fmt::format("{}##texture_pack_detail", name);

		const ImVec2 display = ImGui::GetIO().DisplaySize;
		const float width = std::min(LayoutScale(900.0f), display.x - LayoutScale(40.0f));
		const float height = std::min(LayoutScale(620.0f), display.y - LayoutScale(LAYOUT_FOOTER_HEIGHT) - LayoutScale(40.0f));
		ImGui::SetNextWindowSize(ImVec2(width, height));
		ImGui::SetNextWindowPos((display - LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT)) * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::OpenPopup(window_id.c_str());

		ImGui::PushFont(g_large_font.first, g_large_font.second);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(ImGuiFullscreen::LAYOUT_WINDOW_ROUNDING));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, UIPrimaryTextColor);
		ImGui::PushStyleColor(ImGuiCol_TitleBg, UIPrimaryDarkColor);
		ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIPrimaryColor);
		ImGui::PushStyleColor(ImGuiCol_PopupBg, UIPopupBackgroundColor);

		// The search box opens above this window and takes Back for itself.
		bool is_open = ImGuiFullscreen::IsInputDialogOpen() || !ImGuiFullscreen::WantsToCloseMenu();
		bool close = false;
		if (ImGui::BeginPopupModal(window_id.c_str(), &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::PushStyleColor(ImGuiCol_Text, UIBackgroundTextColor);
			BeginMenuButtons();
			ResetFocusHere();

			DrawDetailBody(entry, installed, &close);

			EndMenuButtons();
			ImGui::PopStyleColor();
			if (close)
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		else
		{
			is_open = false;
		}

		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar(4);
		ImGui::PopFont();

		if (!is_open || close)
		{
			s_detail = DetailState();
			QueueResetFocus(FocusResetType::PopupClosed);
		}
	}

	bool CatalogStale()
	{
		return !s_catalog_loaded || (std::time(nullptr) - s_catalog_fetched_at) > std::chrono::duration_cast<std::chrono::seconds>(Cat::TexturePackCatalog::CACHE_MAX_AGE).count();
	}

	bool PopularStale()
	{
		return !s_popular_known || (std::chrono::steady_clock::now() - s_popular_asked_at) > POPULAR_MAX_AGE;
	}

	// Runs when the page is drawn after not being drawn the frame before: it has just been opened.
	void OnPageOpened()
	{
		if (!s_filter_serial_requested)
		{
			s_filter_serial.clear();
			s_filter_serial_title.clear();
		}
		s_filter_serial_requested = false;
		s_install.last_message.clear();

		if (!s_opened_before)
		{
			// Where Android opens: the player's own games when a game is in context, else what is popular.
			s_filter = !s_filter_serial.empty() ? ListFilter::All : (VMManager::HasValidVM() ? ListFilter::MyGames : ListFilter::Popular);
			s_opened_before = true;
		}
		else if (!s_filter_serial.empty())
		{
			s_filter = ListFilter::All;
		}
		s_view_dirty = true;

		RefreshRecordsIfChanged(true);
		StartLocalRefresh();

		// The network waits for OnPageSettled: D-pad left/right across the page list opens every page it passes.
		s_fetch_deferred = CatalogStale() || PopularStale();
	}

	// The page has stayed open for PageDwell::SECONDS: ask the network for what is out of date.
	void OnPageSettled()
	{
		s_fetch_deferred = false;
		if (CatalogStale())
			StartCatalogLoad(false, true);
		else if (PopularStale())
			StartCatalogLoad(false, false);
	}

	void DrawInstallStatus()
	{
		if (s_install.active)
		{
			DismissInstallProgressNotice();

			const std::string title = fmt::format("{} {}", ICON_FA_DOWNLOAD, fmt::format(FSUI_FSTR("Installing {}"), s_install.pack_name));
			const std::string summary = ProgressText(s_install.progress);
			const float fraction = ProgressFraction(s_install.progress);
			const std::string value = fraction >= 0.0f ? fmt::format("{}%", static_cast<int>(fraction * 100.0f)) : std::string();
			MenuButtonWithValue(title.c_str(), summary.c_str(), value.c_str(), false);

			if (fraction >= 0.0f)
			{
				const ImVec2 pos = ImGui::GetCursorScreenPos();
				const float inset = LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING);
				const float bar_height = LayoutScale(6.0f);
				const float full = ImGui::GetContentRegionAvail().x - inset * 2.0f;
				ImDrawList* dl = ImGui::GetWindowDrawList();
				const ImVec2 min(pos.x + inset, pos.y);
				dl->AddRectFilled(min, ImVec2(min.x + full, min.y + bar_height), ImGui::GetColorU32(UIPrimaryColor), bar_height * 0.5f);
				dl->AddRectFilled(min, ImVec2(min.x + full * fraction, min.y + bar_height), ImGui::GetColorU32(UISecondaryStrongColor), bar_height * 0.5f);
				ImGui::Dummy(ImVec2(0.0f, bar_height + LayoutScale(8.0f)));
			}

			const char* cancel_summary = s_install.cancelling ?
											 FSUI_CSTR("Cancelling...") :
											 (s_install.cancellable ? FSUI_CSTR("Stops the install and throws away what was downloaded.") :
																	  FSUI_CSTR("Too late to cancel: the pack is being put in place."));
			if (MenuButton(FSUI_ICONSTR(ICON_FA_XMARK, "Cancel Install"), cancel_summary, s_install.cancellable && !s_install.cancelling))
				CancelInstall();
		}
		else if (!s_install.last_message.empty())
		{
			NoticeRow(ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("The Last Install Did Not Finish"), s_install.last_message);
		}
	}

	void DrawBrowseRows()
	{
		if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_FILTER, "Show"),
				FSUI_CSTR("Today's most downloaded packs, every pack, the packs for games in your library, or the ones you installed."),
				FilterName(s_filter)))
		{
			ImGuiFullscreen::ChoiceDialogOptions options;
			for (const ListFilter filter : {ListFilter::Popular, ListFilter::All, ListFilter::MyGames, ListFilter::Installed})
				options.emplace_back(FilterName(filter), filter == s_filter);

			OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_FILTER, "Show"), false, std::move(options), [](s32 index, const std::string& title, bool checked) {
				if (index >= 0 && index <= static_cast<s32>(ListFilter::Installed))
					SetFilter(static_cast<ListFilter>(index));
				CloseChoiceDialog();
			});
		}
		// Where the controller starts, even while an install's Cancel button is above it.
		ImGui::SetItemDefaultFocus();

		if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_MAGNIFYING_GLASS, "Search"),
				FSUI_CSTR("Find a pack by its name, game, serial or creator."), s_search.empty() ? FSUI_CSTR("Not set") : s_search.c_str()))
		{
			OpenInputStringDialog(FSUI_STR("Search Texture Packs"),
				FSUI_STR("Enter part of a pack name, game, serial or creator. Leave it empty to show every pack."), std::string(),
				FSUI_ICONSTR(ICON_FA_CHECK, "OK"),
				[](std::string text) {
					s_search = Trimmed(text);
					s_view_dirty = true;
				},
				s_search);
		}

		if (!s_filter_serial.empty())
		{
			const std::string game = s_filter_serial_title.empty() ? s_filter_serial : fmt::format("{} ({})", s_filter_serial_title, s_filter_serial);
			const std::string summary = fmt::format(FSUI_FSTR("Showing the packs for {}. Select to show the packs for every game."), game);
			if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_GAMEPAD, "Game"), summary.c_str(), FSUI_CSTR("Show All")))
			{
				s_filter_serial.clear();
				s_filter_serial_title.clear();
				s_view_dirty = true;
			}
		}

		// The catalog: how current it is, and the way to ask again.
		const bool loading = s_catalog_worker.running.load() || s_fetch_deferred;
		// The ### keeps the row's identity when its icon changes, so a controller keeps its place on it.
		const std::string catalog_title = fmt::format("{} {}###texture_pack_catalog", loading ? ICON_FA_SPINNER : ICON_FA_CLOUD, FSUI_STR("Catalog"));
		if (loading)
		{
			MenuButtonWithValue(catalog_title.c_str(), s_catalog_loaded ? FSUI_CSTR("Checking for new packs...") : FSUI_CSTR("Fetching the list of texture packs..."),
				FSUI_CSTR("Loading..."), false);
		}
		else if (!s_catalog_loaded)
		{
			if (MenuButtonWithValue(catalog_title.c_str(), FSUI_CSTR("Could not reach the texture catalog. Check your connection and try again."), FSUI_CSTR("Retry")))
				StartCatalogLoad(true, true);
		}
		else
		{
			const bool stale = (s_catalog_source == Cat::TexturePackCatalog::Source::StaleCache);
			const std::string summary = stale ?
											fmt::format(FSUI_FSTR("Showing a saved copy of the catalog from {}. Could not refresh."), AgeText(s_catalog_fetched_at)) :
											fmt::format(FSUI_FSTR("{} packs. Updated {}."), s_packs.size(), AgeText(s_catalog_fetched_at));
			if (MenuButtonWithValue(catalog_title.c_str(), summary.c_str(), stale ? FSUI_CSTR("Retry") : FSUI_CSTR("Refresh")))
				StartCatalogLoad(true, true);
		}

		// On a GPU without ASTC the packs would install and change nothing.
		if (s_astc == TPEngine::AstcSupport::Unsupported)
		{
			const std::string title = fmt::format("{} {}", ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("ASTC Textures Are Not Supported"));
			if (MenuButtonWithValue(title.c_str(),
					FSUI_CSTR("Packs marked with a warning use ASTC textures that this GPU cannot show, so they would not change the game. You can still install them."),
					FSUI_CSTR("Details")))
			{
				OpenInfoMessageDialog(FSUI_STR("ASTC Textures"),
					FSUI_STR("The packs in the catalog are made of ASTC textures, a compressed format that phones, handhelds, Apple Silicon and Asahi GPUs read directly. "
							 "Most desktop NVIDIA, AMD and Intel GPUs cannot, and the emulator skips ASTC replacement textures on a GPU that lacks it.\n\n"
							 "A pack marked with a warning still downloads and installs, but it will not change how the game looks on this GPU."));
			}
		}
	}

	// Does what a screenshot asked for, once the catalog and the installed packs it needs have arrived.
	void ApplyShotAction()
	{
		if (s_shot_action == ShotAction::None || !s_catalog_loaded || !s_installed_loaded || s_view_dirty)
			return;
		const ShotAction action = s_shot_action;
		s_shot_action = ShotAction::None;

		if (action == ShotAction::ConfirmDelete || action == ShotAction::Delete)
		{
			const Cat::TexturePackInstaller::InstalledPack* installed = FindInstalledByFolder(s_shot_id);
			if (!installed)
				return;
			if (action == ShotAction::ConfirmDelete)
				ConfirmDelete(*installed);
			else
				StartLocalRefresh(installed->serial, InstalledName(*installed));
			return;
		}

		const PackEntry* entry = FindPack(s_shot_id);
		if (!entry && s_filter != ListFilter::Installed && !s_view.empty())
			entry = &s_packs[s_view.front()];
		if (!entry && s_filter == ListFilter::Installed && !s_view.empty())
		{
			const auto& installed = s_installed[s_view.front()];
			OpenDetail(installed.record.has_value() ? installed.record->pack_id : std::string(), installed.serial);
			return;
		}
		if (entry)
		{
			const Cat::TexturePackInstaller::InstalledPack* installed = FindInstalledForPack(entry->pack.id);
			OpenDetail(entry->pack.id, installed ? installed->serial : std::string());
		}
	}

	// Screenshot harness: a pack and a network, so an install can be drawn and run without downloading
	// anything. Reached only through OpenTexturePacksForShot().

	// Serves one local file slowly, as every download, or with no file stalls part way through a
	// download until it is cancelled.
	class ShotFetcher final : public Cat::Fetcher
	{
	public:
		std::string file;

		Cat::FetchResponse Get(const std::string&, size_t, const Cat::FetchOptions&) override
		{
			Cat::FetchResponse response;
			response.result = Cat::FetchResult::Transport;
			response.error = "not available";
			return response;
		}

		Cat::FetchResponse GetToFile(const std::string&, const std::string& path, const Cat::FetchOptions& options) override
		{
			Cat::FetchResponse response;
			if (file.empty())
			{
				while (!options.cancel.IsCancelled())
				{
					if (options.on_progress)
						options.on_progress(482ull * 1024 * 1024, 0);
					std::this_thread::sleep_for(std::chrono::milliseconds(250));
				}
				response.result = Cat::FetchResult::Cancelled;
				return response;
			}

			std::ifstream in(file, std::ios::binary | std::ios::ate);
			std::ofstream out(path, std::ios::binary | (options.append ? std::ios::app : std::ios::trunc));
			if (!in || !out)
			{
				response.result = Cat::FetchResult::FileError;
				return response;
			}
			const u64 total = static_cast<u64>(in.tellg());
			in.seekg(0);

			std::vector<char> buffer(64 * 1024);
			u64 done = 0;
			while (in)
			{
				if (options.cancel.IsCancelled())
				{
					response.result = Cat::FetchResult::Cancelled;
					return response;
				}
				in.read(buffer.data(), buffer.size());
				const size_t got = static_cast<size_t>(in.gcount());
				if (got == 0)
					break;
				out.write(buffer.data(), got);
				if (options.on_data)
					options.on_data(reinterpret_cast<const u8*>(buffer.data()), got);
				done += got;
				if (options.on_progress)
					options.on_progress(done, total);
				std::this_thread::sleep_for(std::chrono::milliseconds(15));
			}
			response.status = 200;
			response.bytes_received = done;
			return response;
		}

		Cat::FetchResponse Post(const std::string&, std::string_view, std::string_view, const Cat::FetchOptions&) override
		{
			Cat::FetchResponse response;
			response.result = Cat::FetchResult::Transport;
			return response;
		}
	};
	ShotFetcher s_shot_fetcher;

	Cat::TexturePack MakeExamplePack()
	{
		Cat::TexturePack pack;
		pack.id = "example-hd-texture-pack";
		pack.name = "Example HD Texture Pack";
		pack.game_title = "Example Game";
		pack.serials = {"SLUS-00000"};
		pack.version = "v1";
		pack.creators = {Cat::TexturePackCreator{"Example Creator", {}, {}, {}, {}}};
		pack.download_url = "https://example.invalid/example.tar.zst";
		pack.format = Cat::TexturePackFormat::TarZstd;
		pack.size_bytes = 1229ull * 1024 * 1024;
		pack.sha256 = std::string(64, '0');
		pack.file_count = 4277;
		pack.archive_revision = 1;
		pack.decompressed_size_bytes = 1536ull * 1024 * 1024;
		return pack;
	}

	void OpenPage()
	{
		s_about_window_open = false;
		SwitchToSettings();
		s_settings_page = SettingsPage::TexturePacks;
		QueueResetFocus(FocusResetType::WindowChanged);
	}

	// Opens the page for one game's packs: what the Browse Texture Packs row of a game's own settings does.
	void OpenPageForGame(std::string serial, std::string title, std::string game_settings_path = std::string())
	{
		if (!IsInitialized())
			return;
		OpenPage();
		if (const std::optional<std::string> normalized = Cat::NormalizeSerial(serial))
		{
			s_filter_serial = *normalized;
			s_filter_serial_title = std::move(title);
			s_filter_game_settings_path = std::move(game_settings_path);
			s_filter_serial_requested = true;
			s_filter = ListFilter::All;
			s_view_dirty = true;
		}
	}
} // namespace

void FullscreenUI::DrawTexturePacksSettingsPage()
{
	const int frame = ImGui::GetFrameCount();
	const bool opened = (frame - s_last_draw_frame) > 1;
	s_last_draw_frame = frame;
	if (opened)
		OnPageOpened();
	if (s_dwell.Settled() && s_fetch_deferred)
		OnPageSettled();

	PollHandoff();
	RefreshRecordsIfChanged(false);
	s_astc = TPEngine::GetAstcSupportOnGSThread();
	if (s_view_dirty)
		RebuildView();
	ApplyShotAction();

	BeginMenuButtons();

	DrawInstallStatus();
	DrawBrowseRows();

	if (s_filter == ListFilter::Installed)
		DrawInstalledList();
	else
		DrawCatalogList();

	EndMenuButtons();

	DrawDetailWindow();
}

void FullscreenUI::DrawTexturePacksBrowseSetting(SettingsInterface* bsi)
{
	const bool editing_game = IsEditingGameSettings(bsi);
	const bool for_game = editing_game && !s_game_settings_serial.empty();

	if (!MenuButton(FSUI_ICONSTR(ICON_FA_CLOUD_ARROW_DOWN, "Browse Texture Packs"),
			for_game ? FSUI_CSTR("Download texture packs for this game, or manage the ones you installed. Opens the Texture Packs page.") :
					   FSUI_CSTR("Download community texture packs, or manage the ones you installed. Opens the Texture Packs page.")))
	{
		return;
	}

	if (!editing_game)
	{
		s_filter_serial_requested = false;
		s_settings_page = SettingsPage::TexturePacks;
		QueueResetFocus(FocusResetType::WindowChanged);
		return;
	}

	// The game's own settings are torn down to get to the global pages, and this page is still being
	// drawn from them: leave when the GS thread is between frames.
	std::string serial = s_game_settings_serial;
	std::string title = s_game_settings_entry ? s_game_settings_entry->GetTitle(s_prefer_english_titles) : std::string();
	std::string settings_path = s_game_settings_interface ? s_game_settings_interface->GetFileName() : std::string();
	PostToGSThread([serial = std::move(serial), title = std::move(title), settings_path = std::move(settings_path)]() mutable {
		OpenPageForGame(std::move(serial), std::move(title), std::move(settings_path));
	});
}

void FullscreenUI::ShutdownTexturePacks()
{
	// Stops the workers first, so nothing is added to the hand-over after it is cleared.
	s_catalog_worker.cancel.Cancel();
	s_local_worker.cancel.Cancel();
	s_install_worker.cancel.Cancel();
	s_install.cancel.Cancel();
	s_catalog_worker.Join();
	s_local_worker.Join();
	s_install_worker.Join();

	{
		std::lock_guard<std::mutex> lock(s_handoff.mutex);
		s_handoff.catalog.reset();
		s_handoff.popular.reset();
		s_handoff.popular_ready = false;
		s_handoff.local.reset();
		s_handoff.install_drain_queued = false;
		s_handoff.install_progress.reset();
		s_handoff.install_finished.reset();
	}

	s_packs.clear();
	s_pack_index.clear();
	s_catalog_loaded = false;
	s_popular_known = false;
	s_popular.clear();
	s_installed.clear();
	s_installed_loaded = false;
	s_library_serials.clear();
	s_library_titles.clear();
	s_free_bytes.reset();
	s_records.clear();
	s_records_revision = ~0u;
	s_local_again = false;
	s_pending_delete_folder.clear();
	s_pending_delete_name.clear();
	s_view.clear();
	s_view_dirty = true;
	s_detail = DetailState();
	s_install = InstallUi();
	s_search.clear();
	s_filter_serial.clear();
	s_filter_serial_title.clear();
	s_filter_game_settings_path.clear();
	s_filter_serial_requested = false;
	s_opened_before = false;
	s_fetch_deferred = false;
	s_last_draw_frame = -100;
	s_dwell.Reset();
	s_install_notice_up = false;
	s_shot_action = ShotAction::None;
}

std::string FullscreenUI::TexturePacksShotVariants()
{
	return "popular|all|mine|installed, detail[=<pack id>], game=<serial>, search=<text>, install, "
		   "install-local=<archive>:<tar bytes>:<textures>, cancel, delete=<folder>, deleted=<folder>; join with + (installed+detail)";
}

bool FullscreenUI::OpenTexturePacksForShot(std::string_view variant, std::string* error)
{
	ListFilter filter = ListFilter::Popular;
	ShotAction action = ShotAction::None;
	std::string action_id;
	bool install = false;
	bool cancel = false;
	std::string local_archive;
	u64 local_tar_bytes = 0;
	u32 local_textures = 0;
	std::string serial;
	std::string search;

	while (!variant.empty())
	{
		const size_t plus = variant.find('+');
		std::string_view token = variant.substr(0, plus);
		variant = (plus == std::string_view::npos) ? std::string_view() : variant.substr(plus + 1);

		std::string_view value;
		if (const size_t equals = token.find('='); equals != std::string_view::npos)
		{
			value = token.substr(equals + 1);
			token = token.substr(0, equals);
		}

		if (token == "popular")
			filter = ListFilter::Popular;
		else if (token == "all")
			filter = ListFilter::All;
		else if (token == "mine")
			filter = ListFilter::MyGames;
		else if (token == "installed")
			filter = ListFilter::Installed;
		else if (token == "detail")
		{
			action = ShotAction::Detail;
			action_id = std::string(value);
		}
		else if (token == "delete" || token == "deleted")
		{
			action = (token == "delete") ? ShotAction::ConfirmDelete : ShotAction::Delete;
			action_id = std::string(value);
		}
		else if (token == "game")
		{
			const std::optional<std::string> normalized = Cat::NormalizeSerial(value);
			if (!normalized.has_value())
			{
				*error = "texturepacks:game wants a serial such as SLUS-21410";
				return false;
			}
			serial = *normalized;
		}
		else if (token == "search")
			search = std::string(value);
		else if (token == "install")
			install = true;
		else if (token == "install-local")
		{
			// <archive path>:<length of the tar inside>:<number of textures in it>
			const size_t last = value.rfind(':');
			const size_t middle = (last == std::string_view::npos || last == 0) ? std::string_view::npos : value.rfind(':', last - 1);
			if (last == std::string_view::npos || middle == std::string_view::npos)
			{
				*error = "texturepacks:install-local wants <archive>:<tar bytes>:<textures>";
				return false;
			}
			local_archive = std::string(value.substr(0, middle));
			local_tar_bytes = std::strtoull(std::string(value.substr(middle + 1, last - middle - 1)).c_str(), nullptr, 10);
			local_textures = static_cast<u32>(std::strtoul(std::string(value.substr(last + 1)).c_str(), nullptr, 10));
			install = true;
		}
		else if (token == "cancel")
			cancel = true;
		else
		{
			*error = fmt::format("unknown texture packs state '{}' (known: {})", token, TexturePacksShotVariants());
			return false;
		}
	}

	ForceCloseDetail();
	ImGuiFullscreen::CloseMessageDialog(); // a confirmation an earlier state left open
	s_opened_before = true;
	if (!serial.empty())
	{
		// The same way the Browse Texture Packs row of a game's settings gets here, hop included.
		std::string title;
		{
			auto lock = GameList::GetLock();
			const u32 count = GameList::GetEntryCount();
			for (u32 i = 0; i < count && title.empty(); i++)
			{
				const GameList::Entry* entry = GameList::GetEntryByIndex(i);
				if (entry && Cat::NormalizeSerial(entry->serial) == serial)
					title = entry->GetTitle(s_prefer_english_titles);
			}
		}
		PostToGSThread([serial, title]() mutable { OpenPageForGame(std::move(serial), std::move(title)); });
		return true;
	}

	OpenPage();
	s_filter = filter;
	s_search = search;
	s_filter_serial.clear();
	s_filter_serial_title.clear();
	s_filter_serial_requested = false;
	s_view_dirty = true;
	s_shot_action = action;
	s_shot_id = action_id;

	if (install)
	{
		Cat::TexturePack pack = MakeExamplePack();
		s_shot_fetcher.file = local_archive;
		if (!local_archive.empty())
		{
			std::string sha;
			Error hash_error;
			const s64 size = FileSystem::GetPathFileSize(local_archive.c_str());
			if (size <= 0 || SHA256Digest::HashFile(local_archive.c_str(), &sha, &hash_error) != SHA256Digest::FileResult::Ok)
			{
				*error = fmt::format("cannot read {}", local_archive);
				return false;
			}
			pack.size_bytes = static_cast<u64>(size);
			pack.sha256 = sha;
			pack.decompressed_size_bytes = local_tar_bytes;
			pack.file_count = local_textures;
		}
		s_install = InstallUi();
		StartInstall(pack, &s_shot_fetcher);
	}
	if (cancel)
		CancelInstall();
	return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// Same scheme as FullscreenUI.cpp and FullscreenUI_Settings.cpp: tools/generate_fullscreen_ui_translation_strings.py
// regenerates the block below from the FSUI_* macros above.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "AI Upscale");
TRANSLATE_NOOP("FullscreenUI", "Handcrafted");
TRANSLATE_NOOP("FullscreenUI", "Mixed");
TRANSLATE_NOOP("FullscreenUI", "Port");
TRANSLATE_NOOP("FullscreenUI", "Button Replacement");
TRANSLATE_NOOP("FullscreenUI", "Complete");
TRANSLATE_NOOP("FullscreenUI", "In Progress");
TRANSLATE_NOOP("FullscreenUI", "Incomplete");
TRANSLATE_NOOP("FullscreenUI", "Partial");
TRANSLATE_NOOP("FullscreenUI", "No one has named the creator");
TRANSLATE_NOOP("FullscreenUI", "just now");
TRANSLATE_NOOP("FullscreenUI", "Starting...");
TRANSLATE_NOOP("FullscreenUI", "Verifying download...");
TRANSLATE_NOOP("FullscreenUI", "Installing...");
TRANSLATE_NOOP("FullscreenUI", "Texture Pack Deleted");
TRANSLATE_NOOP("FullscreenUI", "Texture Pack Not Deleted");
TRANSLATE_NOOP("FullscreenUI", "Not while a texture pack is being installed.");
TRANSLATE_NOOP("FullscreenUI", "Texture Pack Installed");
TRANSLATE_NOOP("FullscreenUI", "Cancelled.");
TRANSLATE_NOOP("FullscreenUI", "Texture Pack Install Failed");
TRANSLATE_NOOP("FullscreenUI", "A texture pack is already being installed.");
TRANSLATE_NOOP("FullscreenUI", "Finishing the last install. Try again in a moment.");
TRANSLATE_NOOP("FullscreenUI", "Loading Catalog...");
TRANSLATE_NOOP("FullscreenUI", "Fetching the list of texture packs.");
TRANSLATE_NOOP("FullscreenUI", "No Catalog");
TRANSLATE_NOOP("FullscreenUI", "The list of texture packs is not available. Use Retry above.");
TRANSLATE_NOOP("FullscreenUI", "Loading...");
TRANSLATE_NOOP("FullscreenUI", "Asking which packs were downloaded today.");
TRANSLATE_NOOP("FullscreenUI", "Can't Reach the Download Counter");
TRANSLATE_NOOP("FullscreenUI", "Can't reach the download counter right now. Choose All to see every pack.");
TRANSLATE_NOOP("FullscreenUI", "Nothing Yet");
TRANSLATE_NOOP("FullscreenUI", "Nothing has been downloaded yet today.");
TRANSLATE_NOOP("FullscreenUI", "No Games");
TRANSLATE_NOOP("FullscreenUI", "There are no games in your library yet, so no pack can be matched to them.");
TRANSLATE_NOOP("FullscreenUI", "No Packs Match");
TRANSLATE_NOOP("FullscreenUI", "No packs match.");
TRANSLATE_NOOP("FullscreenUI", "Looking at the packs in the textures folder.");
TRANSLATE_NOOP("FullscreenUI", "No Packs Installed");
TRANSLATE_NOOP("FullscreenUI", "No texture packs are installed. Choose another list to download one.");
TRANSLATE_NOOP("FullscreenUI", "Not from the catalog");
TRANSLATE_NOOP("FullscreenUI", "Installed");
TRANSLATE_NOOP("FullscreenUI", "Copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "The installed pack differs from the catalog's copy of the same version. Updating replaces it.");
TRANSLATE_NOOP("FullscreenUI", "This pack is installed and up to date.");
TRANSLATE_NOOP("FullscreenUI", "Another texture pack is being installed. Wait for it to finish.");
TRANSLATE_NOOP("FullscreenUI", "This GPU Cannot Show These Textures");
TRANSLATE_NOOP("FullscreenUI", "Not Enough Free Space");
TRANSLATE_NOOP("FullscreenUI", "None");
TRANSLATE_NOOP("FullscreenUI", "These textures were not installed from the catalog, or the catalog does not list them any more.");
TRANSLATE_NOOP("FullscreenUI", "The Last Install Did Not Finish");
TRANSLATE_NOOP("FullscreenUI", "Search Texture Packs");
TRANSLATE_NOOP("FullscreenUI", "Enter part of a pack name, game, serial or creator. Leave it empty to show every pack.");
TRANSLATE_NOOP("FullscreenUI", "Catalog");
TRANSLATE_NOOP("FullscreenUI", "ASTC Textures Are Not Supported");
TRANSLATE_NOOP("FullscreenUI", "ASTC Textures");
TRANSLATE_NOOP("FullscreenUI", "The packs in the catalog are made of ASTC textures, a compressed format that phones, handhelds, Apple Silicon and Asahi GPUs read directly. Most desktop NVIDIA, AMD and Intel GPUs cannot, and the emulator skips ASTC replacement textures on a GPU that lacks it.\n\nA pack marked with a warning still downloads and installs, but it will not change how the game looks on this GPU.");
TRANSLATE_NOOP("FullscreenUI", "Popular Today");
TRANSLATE_NOOP("FullscreenUI", "All");
TRANSLATE_NOOP("FullscreenUI", "My Games");
TRANSLATE_NOOP("FullscreenUI", "Download");
TRANSLATE_NOOP("FullscreenUI", "Update");
TRANSLATE_NOOP("FullscreenUI", "Conflict");
TRANSLATE_NOOP("FullscreenUI", "Removes this pack's textures from the textures folder. Asks first.");
TRANSLATE_NOOP("FullscreenUI", "This pack is made of ASTC textures, which this GPU does not support, so the game would look unchanged. You can still install it.");
TRANSLATE_NOOP("FullscreenUI", "Pack");
TRANSLATE_NOOP("FullscreenUI", "Size");
TRANSLATE_NOOP("FullscreenUI", "About");
TRANSLATE_NOOP("FullscreenUI", "Cancelling...");
TRANSLATE_NOOP("FullscreenUI", "Stops the install and throws away what was downloaded.");
TRANSLATE_NOOP("FullscreenUI", "Too late to cancel: the pack is being put in place.");
TRANSLATE_NOOP("FullscreenUI", "Today's most downloaded packs, every pack, the packs for games in your library, or the ones you installed.");
TRANSLATE_NOOP("FullscreenUI", "Find a pack by its name, game, serial or creator.");
TRANSLATE_NOOP("FullscreenUI", "Not set");
TRANSLATE_NOOP("FullscreenUI", "Show All");
TRANSLATE_NOOP("FullscreenUI", "Checking for new packs...");
TRANSLATE_NOOP("FullscreenUI", "Fetching the list of texture packs...");
TRANSLATE_NOOP("FullscreenUI", "Could not reach the texture catalog. Check your connection and try again.");
TRANSLATE_NOOP("FullscreenUI", "Retry");
TRANSLATE_NOOP("FullscreenUI", "Refresh");
TRANSLATE_NOOP("FullscreenUI", "Packs marked with a warning use ASTC textures that this GPU cannot show, so they would not change the game. You can still install them.");
TRANSLATE_NOOP("FullscreenUI", "Details");
TRANSLATE_NOOP("FullscreenUI", "Download texture packs for this game, or manage the ones you installed. Opens the Texture Packs page.");
TRANSLATE_NOOP("FullscreenUI", "Download community texture packs, or manage the ones you installed. Opens the Texture Packs page.");
TRANSLATE_NOOP("FullscreenUI", "{} minutes ago");
TRANSLATE_NOOP("FullscreenUI", "{} hours ago");
TRANSLATE_NOOP("FullscreenUI", "{} days ago");
TRANSLATE_NOOP("FullscreenUI", "Downloading {} of {}");
TRANSLATE_NOOP("FullscreenUI", "Extracting {} / {} files");
TRANSLATE_NOOP("FullscreenUI", "Folder {}");
TRANSLATE_NOOP("FullscreenUI", "{} could not be removed completely.");
TRANSLATE_NOOP("FullscreenUI", "{}. The textures are used the next time the game starts.");
TRANSLATE_NOOP("FullscreenUI", "{} files");
TRANSLATE_NOOP("FullscreenUI", "Delete {} for {}? This removes {} files ({}) from the textures folder. You can download the pack again later.");
TRANSLATE_NOOP("FullscreenUI", "Downloads {} and installs it for {}. Turns on texture replacement.");
TRANSLATE_NOOP("FullscreenUI", "A newer version of this pack is available. Replaces the installed pack for {}.");
TRANSLATE_NOOP("FullscreenUI", "It replaces {}, which is installed for this game.");
TRANSLATE_NOOP("FullscreenUI", "Installing needs {} free on the drive that holds the textures folder, and {} is free.");
TRANSLATE_NOOP("FullscreenUI", "{}, {} files");
TRANSLATE_NOOP("FullscreenUI", "{} ({} free)");
TRANSLATE_NOOP("FullscreenUI", "{} preview images");
TRANSLATE_NOOP("FullscreenUI", "Installing {}");
TRANSLATE_NOOP("FullscreenUI", "Showing the packs for {}. Select to show the packs for every game.");
TRANSLATE_NOOP("FullscreenUI", "Showing a saved copy of the catalog from {}. Could not refresh.");
TRANSLATE_NOOP("FullscreenUI", "{} packs. Updated {}.");
TRANSLATE_NOOP("FullscreenUI", "Game");
TRANSLATE_NOOP("FullscreenUI", "Serials");
TRANSLATE_NOOP("FullscreenUI", "Creators");
TRANSLATE_NOOP("FullscreenUI", "Creator Page");
TRANSLATE_NOOP("FullscreenUI", "Tip the Creator");
TRANSLATE_NOOP("FullscreenUI", "Socials");
TRANSLATE_NOOP("FullscreenUI", "Texture Type");
TRANSLATE_NOOP("FullscreenUI", "Status");
TRANSLATE_NOOP("FullscreenUI", "Version");
TRANSLATE_NOOP("FullscreenUI", "Download Size");
TRANSLATE_NOOP("FullscreenUI", "Installed Size");
TRANSLATE_NOOP("FullscreenUI", "Free Space Needed");
TRANSLATE_NOOP("FullscreenUI", "Installs Into");
TRANSLATE_NOOP("FullscreenUI", "Previews");
TRANSLATE_NOOP("FullscreenUI", "Source");
TRANSLATE_NOOP("FullscreenUI", "License");
TRANSLATE_NOOP("FullscreenUI", "Credits");
TRANSLATE_NOOP("FullscreenUI", "Description");
TRANSLATE_NOOP("FullscreenUI", "Serial");
TRANSLATE_NOOP("FullscreenUI", "Files");
TRANSLATE_NOOP("FullscreenUI", "Size on Disk");
TRANSLATE_NOOP("FullscreenUI", "Folder");
TRANSLATE_NOOP("FullscreenUI", "Delete Texture Pack");
TRANSLATE_NOOP("FullscreenUI", "{} already has texture replacements that were not installed from the catalog. The existing folder is kept as {}<date and time> and no longer used. Install the pack?");
TRANSLATE_NOOP("FullscreenUI", "Install Texture Pack");
TRANSLATE_NOOP("FullscreenUI", "Install");
TRANSLATE_NOOP("FullscreenUI", "Delete");
TRANSLATE_NOOP("FullscreenUI", "Cancel");
TRANSLATE_NOOP("FullscreenUI", "Close");
TRANSLATE_NOOP("FullscreenUI", "Cancel Install");
TRANSLATE_NOOP("FullscreenUI", "Show");
TRANSLATE_NOOP("FullscreenUI", "Search");
TRANSLATE_NOOP("FullscreenUI", "OK");
TRANSLATE_NOOP("FullscreenUI", "Browse Texture Packs");
// TRANSLATION-STRING-AREA-END
#endif
