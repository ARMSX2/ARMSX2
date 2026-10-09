// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture: find patches and cheats for the game whose settings are open in community
// repositories, and install the ones the player picks (the Android app's "Browse online").
// Catalog::PatchRepo does the searching and installing; this file is the controller side of it:
//
//   - a "Browse Online" row on the per-game Patches and Cheats pages,
//   - the search, reported as a keyed notification instead of a modal dialog,
//   - a checklist popup of what was found, grouped by source, that needs no typing,
//   - reloading the page's settings after an install.
//
// Everything here runs on the GS thread. The search runs on its own thread and the install on the
// CPU thread, and each hands its answer back through PostToGSThread. The Patches and Cheats pages
// share one search: the community sources cover both, and each page lists its own kind.

#include "Catalog/PatchRepo.h"

#include "Achievements.h"
#include "Config.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/GS.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "ImGui/FullscreenUI_Internal.h"
#include "ImGui/ImGuiFullscreen.h"
#include "MTGS.h"
#include "Patch.h"
#include "VMManager.h"

#include "common/Timer.h"

#include "IconsFontAwesome.h"

#include "fmt/format.h"

#include <algorithm>
#include <memory>

namespace
{
	using namespace FullscreenUI;

	constexpr const char* NOTICE_KEY = "online_patches";
	constexpr float NOTICE_SECONDS = 10.0f;

	// What identifies the game whose settings page is open. It comes from the game list entry the
	// page was opened from: the entry has the disc's CRC, which the running game's name for it
	// (a patch file is <serial>_<CRC>.pnach) has to match.
	struct EditedGame
	{
		std::string serial;
		u32 crc = 0;
		std::string title;
		bool arcade = false;
	};

	bool GetEditedGame(EditedGame* game)
	{
		if (!s_game_settings_entry)
			return false;

		const GameList::Entry& entry = *s_game_settings_entry;
		game->serial = (entry.type != GameList::EntryType::ELF) ? entry.serial : std::string();
		game->crc = entry.crc;
		game->title = entry.GetTitle(s_prefer_english_titles);
		game->arcade = (entry.type == GameList::EntryType::Arcade);
		return true;
	}

	struct Browser
	{
		// What the results below are for. Only meaningful while `bound`.
		bool bound = false;
		std::string serial;
		u32 crc = 0;

		bool have_results = false;
		Catalog::PatchSearchResult result;
		std::vector<u8> selected; ///< Parallel to result.entries.
		bool large_searched = false;

		// A search or an install in flight. Each takes a ticket when it starts, and an answer that
		// carries any other one belongs to something already abandoned.
		bool searching = false;
		bool installing = false;
		u64 search_ticket = 0;
		u64 install_ticket = 0;
		std::unique_ptr<Catalog::PatchSearchJob> job;
		Common::Timer job_finished_for; ///< Running once `job` has finished but its answer has not arrived.
		bool job_finished_seen = false;
		Common::Timer install_age;

		// The popup.
		bool open = false;
		bool open_cheats = false;
		bool focus_pending = false;
	};

	// Declared before s_browser so it is destroyed after it: the search job in s_browser uses it.
	std::shared_ptr<Catalog::PatchRepo> s_repo;
	Browser s_browser;
	u64 s_ticket_counter = 0;

	std::shared_ptr<Catalog::PatchRepo> GetRepo()
	{
		if (!s_repo)
			s_repo = std::make_shared<Catalog::PatchRepo>(Catalog::GetDefaultFetcher());
		return s_repo;
	}

	// The search and the install end on threads that are not the GS thread, and only the CPU thread
	// may push into the GS ring (MTGS::RunOnGSThread), so hop through it.
	void PostToGSThread(std::function<void()> function)
	{
		Host::RunOnCPUThread([function = std::move(function)]() mutable {
			if (MTGS::IsOpen())
				MTGS::RunOnGSThread(std::move(function));
		});
	}

	void ShowNotice(const std::string& title, const std::string& text, float seconds = NOTICE_SECONDS)
	{
		ImGuiFullscreen::AddNotification(NOTICE_KEY, seconds, fmt::format("{} {}", ICON_FA_CLOUD_ARROW_DOWN, title), text, std::string());
	}

	void DismissNotice()
	{
		ImGuiFullscreen::AddNotification(NOTICE_KEY, 0.0f, std::string(), std::string(), std::string());
	}

	// Stops whatever is running and forgets the results. Waits for the search thread, which ends at
	// its next network call.
	void ResetBrowser()
	{
		const u64 search_ticket = ++s_ticket_counter;
		const u64 install_ticket = ++s_ticket_counter;
		s_browser = Browser();
		s_browser.search_ticket = search_ticket;
		s_browser.install_ticket = install_ticket;
	}

	bool IsBoundTo(const EditedGame& game)
	{
		return s_browser.bound && s_browser.serial == game.serial && s_browser.crc == game.crc;
	}

	// The results are for one game. Opening another game's settings starts afresh.
	void ForgetIfOtherGame(const EditedGame& game)
	{
		if (s_browser.bound && !IsBoundTo(game))
		{
			if (s_browser.open)
				QueueResetFocus(FocusResetType::PopupClosed);
			ResetBrowser();
		}
	}

	// An answer that is posted while the GS thread is closed (a renderer change, a device reset) is
	// never delivered, which would leave the row stuck on "Searching...". A finished search whose
	// answer has not shown up after a few seconds, or an install that has taken a minute, is lost.
	void ExpireLostWork()
	{
		if (s_browser.searching && s_browser.job && s_browser.job->IsFinished())
		{
			if (!s_browser.job_finished_seen)
			{
				s_browser.job_finished_seen = true;
				s_browser.job_finished_for.Reset();
			}
			else if (s_browser.job_finished_for.GetTimeSeconds() > 5.0)
			{
				s_browser.searching = false;
				s_browser.job_finished_seen = false;
				DismissNotice();
			}
		}

		if (s_browser.installing && s_browser.install_age.GetTimeSeconds() > 60.0)
			s_browser.installing = false;
	}

	bool IsOnPage(bool cheats)
	{
		return s_current_main_window == MainWindowType::Settings && s_settings_page == (cheats ? SettingsPage::Cheats : SettingsPage::Patches);
	}

	void OpenResults(bool cheats)
	{
		s_browser.open = true;
		s_browser.open_cheats = cheats;
		s_browser.focus_pending = true;
	}

	void CloseResults()
	{
		if (!s_browser.open)
			return;

		s_browser.open = false;
		QueueResetFocus(FocusResetType::PopupClosed);
	}

	//////////////////////////////////////////////////////////////////////////
	// Messages
	//////////////////////////////////////////////////////////////////////////

	void ShowNoSerialMessage()
	{
		OpenInfoMessageDialog(FSUI_STR("Browse Online"), FSUI_STR("No game serial to look up patches for."));
	}

	void ShowArcadeMessage()
	{
		OpenInfoMessageDialog(FSUI_STR("Browse Online"), FSUI_STR("Online patches and cheats are not available for arcade games."));
	}

	// A patch file is named <serial>_<CRC>.pnach, and the emulator only loads it under that name,
	// so with no CRC there is nothing that could be installed.
	void ShowCrcUnknownMessage()
	{
		OpenInfoMessageDialog(FSUI_STR("Patches Cannot Be Installed Yet"),
			FSUI_STR("This game's CRC is not known yet, so patches and cheats cannot be installed for it. The CRC is read when the "
					 "game list scans the game. Scan the game list again, or start the game once, then try again."));
	}

	void ShowUnreachableMessage(bool large_database)
	{
		OpenInfoMessageDialog(FSUI_STR("Browse Online"),
			large_database ?
				FSUI_STR("Could not reach the large cheat database. Check your internet connection and try again in a few minutes. "
						 "If GitHub is limiting requests, it can take up to an hour.") :
				FSUI_STR("Could not reach the online patch sources. Check your internet connection and try again in a few minutes. "
						 "If GitHub is limiting requests, it can take up to an hour."));
	}

	//////////////////////////////////////////////////////////////////////////
	// Search
	//////////////////////////////////////////////////////////////////////////

	void OnSearchProgress(u64 ticket, const Catalog::PatchSearchProgress& progress, bool large_database)
	{
		if (!IsInitialized() || ticket != s_browser.search_ticket || !s_browser.searching || progress.total == 0)
			return;

		const u32 step = std::min(progress.done + 1, progress.total);
		ShowNotice(large_database ? FSUI_STR("Searching the Large Cheat Database") : FSUI_STR("Searching Online Patches"),
			large_database ? FSUI_STR("Downloading the file list. This can take a minute.") :
							 fmt::format(FSUI_FSTR("Checking source {} of {}"), step, progress.total));
	}

	void OnCommunitySearchDone(bool cheats_page, Catalog::PatchSearchResult result)
	{
		s_browser.result = std::move(result);
		s_browser.selected.assign(s_browser.result.entries.size(), 0);
		s_browser.have_results = true;
		s_browser.large_searched = false;

		u32 patches = 0, cheats = 0;
		for (const Catalog::PatchEntry& entry : s_browser.result.entries)
			(entry.kind == Catalog::PatchKind::Cheat ? cheats : patches)++;

		// Only the player on the page that asked gets the list put in front of them. Someone who
		// moved on is told it is there.
		if (!IsOnPage(cheats_page))
		{
			ShowNotice(FSUI_STR("Browse Online"),
				fmt::format(FSUI_FSTR("Found {} patches and {} cheats for this game. Open Browse Online on the Patches or Cheats page."),
					patches, cheats));
			return;
		}

		// A game with no cheats in the community sources can still have some in the large database,
		// and the list is where that is offered.
		if (cheats_page || patches > 0)
		{
			DismissNotice();
			OpenResults(cheats_page);
		}
		else
		{
			ShowNotice(FSUI_STR("Browse Online"), FSUI_STR("No patches for this game in the online sources."));
		}
	}

	void OnLargeSearchDone(Catalog::PatchSearchResult result)
	{
		s_browser.large_searched = true;
		s_browser.result.hardcore_active = result.hardcore_active;

		if (result.status == Catalog::PatchSearchStatus::NoMatches || result.entries.empty())
		{
			ShowNotice(FSUI_STR("Large Cheat Database"), FSUI_STR("The large cheat database has no more cheats for this game."));
			return;
		}

		const size_t added = result.entries.size();
		for (Catalog::PatchEntry& entry : result.entries)
			s_browser.result.entries.push_back(std::move(entry));
		s_browser.selected.resize(s_browser.result.entries.size(), 0);
		if (result.incomplete)
			s_browser.result.incomplete = true;

		ShowNotice(FSUI_STR("Large Cheat Database"), fmt::format(FSUI_FSTR("Found {} more cheats for this game."), added));
	}

	void OnSearchDone(u64 ticket, bool cheats_page, bool large_database, Catalog::PatchSearchResult result)
	{
		if (ticket != s_browser.search_ticket)
			return;

		// Also when Big Picture is being rebuilt (a renderer change): the answer is of no use then,
		// but the row must not stay stuck on "Searching...".
		s_browser.searching = false;
		if (!IsInitialized())
			return;

		EditedGame game;
		if (!GetEditedGame(&game) || !IsBoundTo(game))
		{
			DismissNotice();
			return;
		}

		switch (result.status)
		{
			case Catalog::PatchSearchStatus::Cancelled:
				DismissNotice();
				return;

			case Catalog::PatchSearchStatus::InvalidRequest:
				DismissNotice();
				ShowNoSerialMessage();
				return;

			case Catalog::PatchSearchStatus::Unreachable:
				DismissNotice();
				ShowUnreachableMessage(large_database);
				return;

			case Catalog::PatchSearchStatus::Ok:
			case Catalog::PatchSearchStatus::NoMatches:
				break;
		}

		if (large_database)
			OnLargeSearchDone(std::move(result));
		else
			OnCommunitySearchDone(cheats_page, std::move(result));
	}

	void StartSearch(const EditedGame& game, bool cheats_page, bool large_database)
	{
		const std::shared_ptr<Catalog::PatchRepo> repo = GetRepo();

		Catalog::PatchSearchRequest request;
		request.serial = game.serial;
		request.crc = game.crc;
		if (large_database)
		{
			request.scope = Catalog::PatchSearchScope::LargeCheatDatabase;

			// Names the player already has in front of them, so the large database does not offer them twice.
			for (const Catalog::PatchEntry& entry : s_browser.result.entries)
			{
				if (entry.kind == Catalog::PatchKind::Cheat)
					request.already_listed.push_back(entry.name);
			}
			for (const Patch::PatchInfo& info : s_game_cheats_list)
				request.already_listed.push_back(info.name);
		}

		// The previous job, if any, has finished or is about to; this waits for its thread.
		s_browser.job.reset();

		const u64 ticket = ++s_ticket_counter;
		s_browser.search_ticket = ticket;
		s_browser.searching = true;
		s_browser.job_finished_seen = false;
		s_browser.bound = true;
		s_browser.serial = game.serial;
		s_browser.crc = game.crc;

		ShowNotice(large_database ? FSUI_STR("Searching the Large Cheat Database") : FSUI_STR("Searching Online Patches"),
			large_database ? FSUI_STR("Downloading the file list. This can take a minute.") : FSUI_STR("Contacting the community repositories."));

		s_browser.job = repo->StartSearch(
			std::move(request),
			[ticket, large_database](const Catalog::PatchSearchProgress& progress) {
				PostToGSThread([ticket, large_database, progress]() { OnSearchProgress(ticket, progress, large_database); });
			},
			[ticket, cheats_page, large_database](Catalog::PatchSearchResult result) {
				PostToGSThread([ticket, cheats_page, large_database, result = std::move(result)]() mutable {
					OnSearchDone(ticket, cheats_page, large_database, std::move(result));
				});
			});
	}

	//////////////////////////////////////////////////////////////////////////
	// Install
	//////////////////////////////////////////////////////////////////////////

	// PatchRepo::Install edited the game's settings file on disk. The settings interface this page
	// holds open has its own copy of that file, and the next save would write the copy back over the
	// names the install just added, so read it again and rebuild the lists the page shows.
	void ReloadEditedGameSettings()
	{
		if (!s_game_settings_interface || !s_game_settings_entry)
			return;

		const GameList::Entry& entry = *s_game_settings_entry;
		auto lock = Host::GetSettingsLock();
		s_game_settings_interface->Load();
		PopulatePatchesAndCheatsList((entry.type != GameList::EntryType::ELF) ? std::string_view(entry.serial) : std::string_view(), entry.crc);
	}

	void OnInstallDone(u64 ticket, bool cheats_page, std::vector<size_t> installed_indices, Catalog::PatchInstallResult result)
	{
		if (ticket != s_browser.install_ticket)
			return;

		s_browser.installing = false;
		if (!IsInitialized())
			return;

		switch (result.status)
		{
			case Catalog::PatchInstallStatus::Ok:
				break;

			case Catalog::PatchInstallStatus::NothingSelected:
				ShowToast(ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("Select at least one patch or cheat first."));
				return;

			case Catalog::PatchInstallStatus::CrcUnknown:
				ShowCrcUnknownMessage();
				return;

			case Catalog::PatchInstallStatus::InvalidSerial:
				ShowNoSerialMessage();
				return;

			case Catalog::PatchInstallStatus::WriteFailed:
				OpenInfoMessageDialog(FSUI_STR("Browse Online"),
					fmt::format(FSUI_FSTR("Couldn't install the selected patches.\n\n{}"), result.error));
				return;
		}

		EditedGame game;
		if (GetEditedGame(&game) && IsBoundTo(game))
		{
			for (const size_t index : installed_indices)
			{
				if (index < s_browser.result.entries.size())
				{
					s_browser.result.entries[index].installed = true;
					s_browser.selected[index] = 0;
				}
			}
			s_browser.result.hardcore_active = result.hardcore_active;
			ReloadEditedGameSettings();
		}

		CloseResults();

		const u32 count = result.patches_added + result.cheats_added + result.already_present;
		std::string text = fmt::format(FSUI_FSTR("Turned on for this game: {}."), count);
		if (result.cheats_switched_on)
			text += " " + FSUI_STR("Enable Cheats was turned on for this game.");
		text += " " + (result.reload_requested ? FSUI_STR("The running game has reloaded them.") :
												 FSUI_STR("They apply the next time the game starts."));
		if (result.hardcore_active)
			text += " " + FSUI_STR("Hardcore mode is on, so cheats and most patches stay disabled until it is turned off.");

		ShowNotice(cheats_page ? FSUI_STR("Cheats Installed") : FSUI_STR("Patches Installed"), text);
	}

	void StartInstall(const EditedGame& game, bool cheats_page)
	{
		const Catalog::PatchKind kind = cheats_page ? Catalog::PatchKind::Cheat : Catalog::PatchKind::Patch;

		Catalog::PatchInstallRequest request;
		request.serial = game.serial;
		request.crc = game.crc;
		request.game_title = s_browser.result.game_title;

		std::vector<size_t> indices;
		for (size_t i = 0; i < s_browser.result.entries.size(); i++)
		{
			if (s_browser.selected[i] && s_browser.result.entries[i].kind == kind)
			{
				request.entries.push_back(s_browser.result.entries[i]);
				indices.push_back(i);
			}
		}
		if (request.entries.empty())
			return;

		const u64 ticket = ++s_ticket_counter;
		s_browser.install_ticket = ticket;
		s_browser.installing = true;
		s_browser.install_age.Reset();

		// File work only, but not for the GS thread: a game card's storage can be slow.
		const std::shared_ptr<Catalog::PatchRepo> repo = GetRepo();
		Host::RunOnCPUThread(
			[repo, ticket, cheats_page, request = std::move(request), indices = std::move(indices)]() mutable {
				Catalog::PatchInstallResult result = repo->Install(request);
				PostToGSThread([ticket, cheats_page, indices = std::move(indices), result = std::move(result)]() mutable {
					OnInstallDone(ticket, cheats_page, std::move(indices), std::move(result));
				});
			});
	}

	//////////////////////////////////////////////////////////////////////////
	// The results list
	//////////////////////////////////////////////////////////////////////////

	// One found section: a check box, its name and description, and on the right the source it
	// came from and whether the game already has it. Returns true when the player picked it.
	bool DrawEntryRow(u32 index, const Catalog::PatchEntry& entry, bool checked)
	{
		const std::pair<ImFont*, float> font = g_large_font;
		const std::pair<ImFont*, float> small_font = g_medium_font;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float checkbox_width = LayoutScale(34.0f);
		const float line_height = GetLineHeight(font);
		const float text_width = ImGui::GetContentRegionAvail().x - style.FramePadding.x * 2.0f - checkbox_width;

		// The description wraps to at most two lines; the rest is not worth the room.
		float description_height = 0.0f;
		if (!entry.description.empty() && text_width > 0.0f)
		{
			const float wrapped = small_font.first->CalcTextSizeA(small_font.second, FLT_MAX, text_width, entry.description.c_str()).y;
			description_height = std::min(wrapped, GetLineHeight(small_font) * 2.0f);
		}
		const float content_height = line_height + ((description_height > 0.0f) ? LayoutScale(4.0f) + description_height : 0.0f);
		const float row_height = std::max((description_height > 0.0f) ? LAYOUT_MENU_BUTTON_HEIGHT : LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY,
			ImGuiFullscreen::LayoutUnscale(content_height));

		const std::string id = fmt::format("##online_patch_row_{}", index);
		ImVec2 bb_min, bb_max;
		bool visible, hovered;
		const bool pressed = MenuButtonFrame(id.c_str(), true, row_height, &visible, &hovered, &bb_min, &bb_max);
		if (!visible)
			return false;
		const ImRect bb(bb_min, bb_max);

		ImDrawList* dl = ImGui::GetWindowDrawList();

		// Right side first, because it decides how much room the name has.
		const float chip_padding = LayoutScale(8.0f);
		const float chip_gap = LayoutScale(10.0f);
		float right = bb.Max.x;

		{
			const char* label = Catalog::PatchSourceLabel(entry.source);
			const ImVec2 size = small_font.first->CalcTextSizeA(small_font.second, FLT_MAX, 0.0f, label);
			const float width = size.x + chip_padding * 2.0f;
			const float height = size.y + LayoutScale(4.0f);
			const ImVec2 min(right - width, bb.Min.y + (line_height - height) * 0.5f);
			dl->AddRectFilled(min, ImVec2(right, min.y + height), ImGui::GetColorU32(UISecondaryColor), height * 0.5f);
			dl->AddText(small_font.first, small_font.second, ImVec2(min.x + chip_padding, min.y + (height - size.y) * 0.5f),
				ImGui::GetColorU32(UIPrimaryTextColor), label);
			right = min.x - chip_gap;
		}

		if (entry.installed)
		{
			const std::string label = fmt::format("{} {}", ICON_FA_CHECK, FSUI_STR("Installed"));
			const ImVec2 size = small_font.first->CalcTextSizeA(small_font.second, FLT_MAX, 0.0f, label.c_str());
			const ImVec2 pos(right - size.x, bb.Min.y + (line_height - size.y) * 0.5f);
			dl->AddText(small_font.first, small_font.second, pos, ImGui::GetColorU32(UISecondaryStrongColor), label.c_str());
			right = pos.x - chip_gap;
		}

		ImGui::PushFont(font.first, font.second);
		const ImRect box_bb(bb.Min, ImVec2(bb.Min.x + checkbox_width, bb.Min.y + line_height));
		ImGuiFullscreen::RenderTextClippedWithShadow(box_bb.Min, box_bb.Max, checked ? ICON_FA_SQUARE_CHECK : ICON_FA_SQUARE, nullptr, nullptr,
			ImVec2(0.0f, 0.0f), &box_bb);
		const ImRect name_bb(ImVec2(bb.Min.x + checkbox_width, bb.Min.y), ImVec2(std::max(right, bb.Min.x + checkbox_width), bb.Min.y + line_height));
		ImGuiFullscreen::RenderTextClippedWithShadow(name_bb.Min, name_bb.Max, entry.name.c_str(), nullptr, nullptr, ImVec2(0.0f, 0.0f), &name_bb);
		ImGui::PopFont();

		if (description_height > 0.0f)
		{
			const ImVec2 pos(bb.Min.x + checkbox_width, bb.Min.y + line_height + LayoutScale(4.0f));
			const ImVec4 clip(pos.x, pos.y, bb.Max.x, pos.y + description_height);
			ImGui::PushStyleColor(ImGuiCol_Text, MulAlpha(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.60f));
			ImGuiFullscreen::AddTextWithShadow(dl, small_font, pos, ImGui::GetColorU32(ImGuiCol_Text), entry.description.c_str(), nullptr, text_width, &clip);
			ImGui::PopStyleColor();
		}

		return pressed;
	}

	void DrawNoteRow(const char* icon, const std::string& text)
	{
		ActiveButton(fmt::format("{} {}", icon, text).c_str(), false, false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
	}
} // namespace

void FullscreenUI::DrawOnlinePatchesRow(bool cheats)
{
	EditedGame game;
	if (!GetEditedGame(&game))
		return;

	ForgetIfOtherGame(game);
	ExpireLostWork();

	const bool busy = s_browser.searching || s_browser.installing;
	const char* summary;
	if (busy)
		summary = s_browser.installing ? FSUI_CSTR("Installing...") : FSUI_CSTR("Searching...");
	else if (cheats)
		summary = FSUI_CSTR("Find cheats for this game in the community repositories and install the ones you pick.");
	else
		summary = FSUI_CSTR("Find widescreen, 60 FPS and other patches for this game in the community repositories and install the ones you pick.");

	if (MenuButton(FSUI_ICONSTR(ICON_FA_CLOUD_ARROW_DOWN, "Browse Online"), summary, !busy))
		BrowseOnlinePatches(cheats);
}

void FullscreenUI::BrowseOnlinePatches(bool cheats)
{
	EditedGame game;
	if (!GetEditedGame(&game))
		return;

	ForgetIfOtherGame(game);
	if (s_browser.searching || s_browser.installing)
		return;

	if (game.arcade)
	{
		ShowArcadeMessage();
		return;
	}
	if (game.serial.empty())
	{
		ShowNoSerialMessage();
		return;
	}
	if (game.crc == 0)
	{
		ShowCrcUnknownMessage();
		return;
	}

	if (IsBoundTo(game) && s_browser.have_results)
		OpenResults(cheats);
	else
		StartSearch(game, cheats, false);
}

void FullscreenUI::DrawOnlinePatchesResults()
{
	if (!s_browser.open)
		return;

	EditedGame game;
	if (!IsOnPage(s_browser.open_cheats) || !GetEditedGame(&game) || !IsBoundTo(game) || !s_browser.have_results)
	{
		// The player left the page (or the game) some other way.
		s_browser.open = false;
		return;
	}

	const bool cheats = s_browser.open_cheats;
	const Catalog::PatchKind kind = cheats ? Catalog::PatchKind::Cheat : Catalog::PatchKind::Patch;
	const std::vector<Catalog::PatchEntry>& entries = s_browser.result.entries;
	const bool busy = s_browser.searching || s_browser.installing;

	u32 shown = 0, chosen = 0;
	u32 per_source[8] = {};
	for (size_t i = 0; i < entries.size(); i++)
	{
		if (entries[i].kind != kind)
			continue;
		shown++;
		per_source[static_cast<u8>(entries[i].source) & 7]++;
		if (s_browser.selected[i])
			chosen++;
	}

	const std::string title = fmt::format("{} - {}##online_patches", cheats ? FSUI_STR("Online Cheats") : FSUI_STR("Online Patches"), game.title);

	ImGui::PushFont(g_large_font.first, g_large_font.second);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, LayoutScale(20.0f, 20.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, LayoutScale(ImGuiFullscreen::LAYOUT_WINDOW_ROUNDING));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, LayoutScale(LAYOUT_MENU_BUTTON_X_PADDING, LAYOUT_MENU_BUTTON_Y_PADDING));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_Text, UIPrimaryTextColor);
	ImGui::PushStyleColor(ImGuiCol_TitleBg, UIPrimaryDarkColor);
	ImGui::PushStyleColor(ImGuiCol_TitleBgActive, UIPrimaryColor);
	ImGui::PushStyleColor(ImGuiCol_PopupBg, UIPopupBackgroundColor);

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const ImVec2 footer = LayoutScale(0.0f, LAYOUT_FOOTER_HEIGHT);
	ImGui::SetNextWindowSize(ImVec2(std::min(LayoutScale(1100.0f), display.x - LayoutScale(40.0f)),
		std::min(LayoutScale(640.0f), display.y - footer.y - LayoutScale(40.0f))));
	ImGui::SetNextWindowPos((display - footer) * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::OpenPopup(title.c_str());

	// A message box above this popup (a failed search, a failed install) takes Back for itself.
	bool is_open = ImGuiFullscreen::IsMessageBoxDialogOpen() || !ImGuiFullscreen::WantsToCloseMenu();

	bool install = false, toggle_all = false, search_large = false, search_again = false;
	std::vector<u32> clicked;

	if (ImGui::BeginPopupModal(title.c_str(), &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
	{
		ImGui::PushStyleColor(ImGuiCol_Text, UIBackgroundTextColor);

		BeginMenuButtons();
		ResetFocusHere();

		if (s_browser.result.hardcore_active)
			DrawNoteRow(ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("Hardcore mode is on. Cheats and most patches are disabled while it is on."));
		if (s_browser.result.incomplete)
			DrawNoteRow(ICON_FA_TRIANGLE_EXCLAMATION, FSUI_STR("Some sources could not be reached, so this list may be missing entries."));

		install = ActiveButton(
			(s_browser.installing ? fmt::format("{} {}###online_patch_install", ICON_FA_DOWNLOAD, FSUI_STR("Installing...")) :
									fmt::format("{} {} ({})###online_patch_install", ICON_FA_DOWNLOAD, FSUI_STR("Install Selected"), chosen))
				.c_str(),
			false, chosen > 0 && !busy);

		if (shown > 0)
		{
			toggle_all = ActiveButton(fmt::format("{} {}###online_patch_select_all", ICON_FA_LIST_CHECK,
										  (chosen == shown) ? FSUI_STR("Clear Selection") : FSUI_STR("Select All"))
										  .c_str(),
				false, !busy);
		}

		if (cheats && !s_browser.large_searched)
		{
			search_large = MenuButton(FSUI_ICONSTR(ICON_FA_DATABASE, "Search the large cheat database"),
				FSUI_CSTR("About 11,000 more cheat files. This downloads a file list of several megabytes first."), !busy);
		}

		search_again = ActiveButton(fmt::format("{} {}###online_patch_search_again", ICON_FA_ROTATE, FSUI_STR("Search Again")).c_str(), false, !busy);

		if (shown == 0)
		{
			DrawNoteRow(ICON_FA_CIRCLE_INFO,
				cheats ? FSUI_STR("No cheats for this game were found in the community sources.") :
						 FSUI_STR("No patches for this game were found in the online sources."));
		}

		bool have_source = false;
		Catalog::PatchSourceId last_source = Catalog::PatchSourceId::Pcsx2Patches;
		for (size_t i = 0; i < entries.size(); i++)
		{
			if (entries[i].kind != kind)
				continue;

			if (!have_source || entries[i].source != last_source)
			{
				have_source = true;
				last_source = entries[i].source;
				MenuHeading(fmt::format("{} ({})", Catalog::PatchSourceLabel(last_source), per_source[static_cast<u8>(last_source) & 7]).c_str());
			}

			if (DrawEntryRow(static_cast<u32>(i), entries[i], s_browser.selected[i] != 0) && !busy)
				clicked.push_back(static_cast<u32>(i));
		}

		EndMenuButtons();

		ImGui::PopStyleColor(1);
		ImGui::EndPopup();
	}
	else
	{
		is_open = false;
	}

	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar(4);
	ImGui::PopFont();

	if (s_browser.focus_pending)
	{
		// The page underneath may use the queued reset before this popup exists. Queuing it once the
		// popup is open leaves it for the popup, as the choice dialogs do.
		s_browser.focus_pending = false;
		QueueResetFocus(FocusResetType::PopupOpened);
	}

	for (const u32 index : clicked)
		s_browser.selected[index] ^= 1;

	if (toggle_all)
	{
		const u8 value = (chosen == shown) ? 0 : 1;
		for (size_t i = 0; i < entries.size(); i++)
		{
			if (entries[i].kind == kind)
				s_browser.selected[i] = value;
		}
	}

	if (!is_open)
	{
		CloseResults();
	}
	else if (install)
	{
		StartInstall(game, cheats);
	}
	else if (search_large)
	{
		StartSearch(game, cheats, true);
	}
	else if (search_again)
	{
		CloseResults();
		s_browser.have_results = false;
		s_browser.result = Catalog::PatchSearchResult();
		s_browser.selected.clear();
		StartSearch(game, cheats, false);
	}
	else
	{
		SetStandardSelectionFooterText(false);
	}
}

void FullscreenUI::ShutdownOnlinePatches()
{
	// Resetting stops a running search first (this waits for its thread), so nothing is posted to
	// the GS thread afterwards. The repository goes last: the search thread uses it.
	ResetBrowser();
	s_repo.reset();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// Same scheme as FullscreenUI.cpp and FullscreenUI_Settings.cpp: tools/generate_fullscreen_ui_translation_strings.py
// regenerates the block below from the FSUI_* macros above.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "Browse Online");
TRANSLATE_NOOP("FullscreenUI", "No game serial to look up patches for.");
TRANSLATE_NOOP("FullscreenUI", "Online patches and cheats are not available for arcade games.");
TRANSLATE_NOOP("FullscreenUI", "Patches Cannot Be Installed Yet");
TRANSLATE_NOOP("FullscreenUI", "This game's CRC is not known yet, so patches and cheats cannot be installed for it. The CRC is read when the game list scans the game. Scan the game list again, or start the game once, then try again.");
TRANSLATE_NOOP("FullscreenUI", "Could not reach the large cheat database. Check your internet connection and try again in a few minutes. If GitHub is limiting requests, it can take up to an hour.");
TRANSLATE_NOOP("FullscreenUI", "Could not reach the online patch sources. Check your internet connection and try again in a few minutes. If GitHub is limiting requests, it can take up to an hour.");
TRANSLATE_NOOP("FullscreenUI", "Searching the Large Cheat Database");
TRANSLATE_NOOP("FullscreenUI", "Searching Online Patches");
TRANSLATE_NOOP("FullscreenUI", "Downloading the file list. This can take a minute.");
TRANSLATE_NOOP("FullscreenUI", "No patches for this game in the online sources.");
TRANSLATE_NOOP("FullscreenUI", "Large Cheat Database");
TRANSLATE_NOOP("FullscreenUI", "The large cheat database has no more cheats for this game.");
TRANSLATE_NOOP("FullscreenUI", "Contacting the community repositories.");
TRANSLATE_NOOP("FullscreenUI", "Select at least one patch or cheat first.");
TRANSLATE_NOOP("FullscreenUI", "Enable Cheats was turned on for this game.");
TRANSLATE_NOOP("FullscreenUI", "The running game has reloaded them.");
TRANSLATE_NOOP("FullscreenUI", "They apply the next time the game starts.");
TRANSLATE_NOOP("FullscreenUI", "Hardcore mode is on, so cheats and most patches stay disabled until it is turned off.");
TRANSLATE_NOOP("FullscreenUI", "Cheats Installed");
TRANSLATE_NOOP("FullscreenUI", "Patches Installed");
TRANSLATE_NOOP("FullscreenUI", "Installed");
TRANSLATE_NOOP("FullscreenUI", "Online Cheats");
TRANSLATE_NOOP("FullscreenUI", "Online Patches");
TRANSLATE_NOOP("FullscreenUI", "Hardcore mode is on. Cheats and most patches are disabled while it is on.");
TRANSLATE_NOOP("FullscreenUI", "Some sources could not be reached, so this list may be missing entries.");
TRANSLATE_NOOP("FullscreenUI", "Installing...");
TRANSLATE_NOOP("FullscreenUI", "Install Selected");
TRANSLATE_NOOP("FullscreenUI", "Clear Selection");
TRANSLATE_NOOP("FullscreenUI", "Select All");
TRANSLATE_NOOP("FullscreenUI", "Search Again");
TRANSLATE_NOOP("FullscreenUI", "No cheats for this game were found in the community sources.");
TRANSLATE_NOOP("FullscreenUI", "No patches for this game were found in the online sources.");
TRANSLATE_NOOP("FullscreenUI", "Searching...");
TRANSLATE_NOOP("FullscreenUI", "Find cheats for this game in the community repositories and install the ones you pick.");
TRANSLATE_NOOP("FullscreenUI", "Find widescreen, 60 FPS and other patches for this game in the community repositories and install the ones you pick.");
TRANSLATE_NOOP("FullscreenUI", "About 11,000 more cheat files. This downloads a file list of several megabytes first.");
TRANSLATE_NOOP("FullscreenUI", "Checking source {} of {}");
TRANSLATE_NOOP("FullscreenUI", "Found {} patches and {} cheats for this game. Open Browse Online on the Patches or Cheats page.");
TRANSLATE_NOOP("FullscreenUI", "Found {} more cheats for this game.");
TRANSLATE_NOOP("FullscreenUI", "Couldn't install the selected patches.\n\n{}");
TRANSLATE_NOOP("FullscreenUI", "Turned on for this game: {}.");
TRANSLATE_NOOP("FullscreenUI", "Search the large cheat database");
// TRANSLATION-STRING-AREA-END
#endif
