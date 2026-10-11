// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture rows for features brought over from the Android app. The other FullscreenUI files
// call in here with one line each (declarations at the end of FullscreenUI_Internal.h); anything
// longer than a row lives here, so an upstream rework of those pages touches these features in a
// handful of places.

#include "Catalog/CoverAutoDownload.h"
#include "Catalog/LocalLinkSettings.h"

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
#include "VMManager.h"

#include "common/SettingsInterface.h"
#include "common/Timer.h"

#include "IconsFontAwesome.h"

#include "fmt/format.h"

#include <mutex>
#include <optional>

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace LocalLink = Catalog::LocalLink;
namespace AutoCovers = Catalog::AutoCovers;

//////////////////////////////////////////////////////////////////////////
// Texture upscaler
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::DrawTextureUpscaleSetting(SettingsInterface* bsi)
{
	// Labels as in the Qt widget (Graphics > Texture Replacement). Its help text is five sentences,
	// which does not fit here: MenuButtonWithValue reserves too little width for the value, so a
	// summary longer than one line is drawn under it. This is a one-line version of the help.
	// The values are GSTextureUpscaleMode: 0 Off, 1 RAISR 2x, 2 RAISR 4x.
	static constexpr const char* s_options[] = {
		FSUI_NSTR("Off (Default)"),
		FSUI_NSTR("RAISR 2x"),
		FSUI_NSTR("RAISR 4x"),
	};

	DrawIntListSetting(bsi, FSUI_ICONSTR(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Upscale Textures"),
		FSUI_CSTR("Upscales game textures on the CPU in the background using RAISR. Uses more memory and CPU. Hardware renderers only."),
		"EmuCore/GS", "TextureUpscale", static_cast<int>(GSTextureUpscaleMode::Off), s_options, std::size(s_options), true);
}

//////////////////////////////////////////////////////////////////////////
// Local Link
//////////////////////////////////////////////////////////////////////////

namespace
{
	using namespace FullscreenUI;

	// The reason an entry was refused, in the user's language.
	std::string LocalLinkProblemText(LocalLink::Problem problem)
	{
		switch (problem)
		{
			case LocalLink::Problem::AddressTooLong:
				return FSUI_STR("That address is too long.");
			case LocalLink::Problem::PortNotANumber:
				return FSUI_STR("The port must be a number.");
			case LocalLink::Problem::PortOutOfRange:
				return FSUI_STR("The port must be between 1024 and 65535.");
			case LocalLink::Problem::PeerIdNotANumber:
				return FSUI_STR("The peer ID must be a number.");
			case LocalLink::Problem::PeerIdOutOfRange:
				return FSUI_STR("The peer ID must be between 2 and 65533. 1 is the host.");
			case LocalLink::Problem::RoomCodeLength:
				return FSUI_STR("The room code must be 4 to 12 letters or digits.");
			case LocalLink::Problem::None:
				break;
		}
		return std::string();
	}

	// Asks for one Local Link value and stores it if it passes the rules in LocalLinkSettings.h.
	// A refused answer comes back as a toast with the box open again on what was typed, since the
	// usual mistake is one character off.
	void EditLocalLinkField(SettingsInterface* bsi, LocalLink::Field field, std::string title, std::string message,
		std::string text, InputFilterType filter)
	{
		OpenInputStringDialog(
			title, message, std::string(), FSUI_ICONSTR(ICON_FA_CHECK, "OK"),
			[bsi, field, title, message, filter](std::string answer) {
				LocalLink::Problem problem;
				{
					auto lock = Host::GetSettingsLock();
					problem = LocalLink::Apply(*bsi, field, answer);
					if (problem == LocalLink::Problem::None)
						SetSettingsChanged(bsi);
				}

				if (problem != LocalLink::Problem::None)
				{
					ShowToast(ICON_FA_TRIANGLE_EXCLAMATION, LocalLinkProblemText(problem));
					EditLocalLinkField(bsi, field, title, message, std::move(answer), filter);
				}
			},
			std::move(text), filter);
	}

#ifndef _WIN32
	// This device's own IPv4 addresses, for the host to read out to the guests. Asked at most every
	// few seconds: the page redraws every frame.
	const std::string& LocalIPv4Addresses()
	{
		static std::string s_text;
		static Common::Timer s_age;
		static bool s_valid = false;
		if (s_valid && s_age.GetTimeSeconds() < 3.0)
			return s_text;

		s_valid = true;
		s_age.Reset();
		s_text.clear();

		ifaddrs* list = nullptr;
		if (getifaddrs(&list) != 0)
			return s_text;

		for (const ifaddrs* it = list; it; it = it->ifa_next)
		{
			if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
				continue;
			if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK))
				continue;

			char buffer[INET_ADDRSTRLEN];
			const auto* address = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
			if (!inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)))
				continue;

			if (!s_text.empty())
				s_text += ", ";
			s_text += buffer;
		}
		freeifaddrs(list);
		return s_text;
	}
#endif
} // namespace

bool FullscreenUI::DrawLocalLinkSettings(SettingsInterface* bsi, const std::string& api, bool network_enabled)
{
	if (api != LocalLink::API_NAME)
		return false;

	MenuHeading(FSUI_CSTR("Local Link"));

	if (MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "Which Games Work?"),
			FSUI_CSTR("Local Link needs a game with its own LAN or System Link mode. Select to read how to connect.")))
	{
		OpenInfoMessageDialog(FSUI_STR("Local Link"),
			FSUI_STR("Local Link connects consoles over a network, so it only works with games that have their own LAN or System Link mode, "
					 "for example Gran Turismo 4, TimeSplitters 2 and Midnight Club 3. If a game asks you to sign in to a server instead, it was online-only.\n\n"
					 "One device hosts and starts the game; every other device enters the host's address with the same port and room code. "
					 "Each player needs their own copy of the game. A shared Wi-Fi network or a phone hotspot is easiest; over the internet, "
					 "a VPN such as Tailscale on every device is the simplest way."));
	}

	if (DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_SERVER, "Host Local Game"),
			FSUI_CSTR("This device hosts the session. Turn off to join a host instead."), LocalLink::SECTION, LocalLink::KEY_HOST, false,
			network_enabled))
	{
		// A guest needs a peer ID in 2..65533 and the default is the host's 1.
		LocalLink::SeedDefaults(*bsi);
	}

	const bool host = bsi->GetBoolValue(LocalLink::SECTION, LocalLink::KEY_HOST, false);

	if (host)
	{
#ifndef _WIN32
		const std::string& addresses = LocalIPv4Addresses();
		if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_LOCATION_DOT, "This Device's Address"),
				FSUI_CSTR("Give one of these to the other players to enter on their device. Select to copy."),
				addresses.empty() ? FSUI_CSTR("Not connected") : addresses.c_str(), network_enabled) &&
			!addresses.empty())
		{
			CopyTextToClipboard(FSUI_STR("Address copied to clipboard."), addresses);
		}
#endif
	}
	else
	{
		const std::string address = bsi->GetStringValue(LocalLink::SECTION, LocalLink::KEY_ADDRESS, "");
		if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_LOCATION_DOT, "Host Address"),
				FSUI_CSTR("The address shown on the host device, for example 192.168.1.42, or a VPN address such as 100.101.102.103. "
						  "A host name also works."),
				address.empty() ? FSUI_CSTR("Not set") : address.c_str(), network_enabled))
		{
			EditLocalLinkField(bsi, LocalLink::Field::Address, FSUI_STR("Host Address"),
				FSUI_STR("Enter the address of the host: an IPv4 address such as 192.168.1.42, or a host name."), address,
				InputFilterType::None);
		}
	}

	const u32 port = bsi->GetUIntValue(LocalLink::SECTION, LocalLink::KEY_PORT, LocalLink::DEFAULT_PORT);
	if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_PLUG, "Local Link Port"),
			FSUI_CSTR("Use the same port on every device (1024-65535). There is no automatic negotiation."),
			std::to_string(port).c_str(), network_enabled))
	{
		EditLocalLinkField(bsi, LocalLink::Field::Port, FSUI_STR("Local Link Port"),
			FSUI_STR("Enter the port, 1024 to 65535. It must be the same on every device."), std::to_string(port), InputFilterType::Numeric);
	}

	const std::string room_code = bsi->GetStringValue(LocalLink::SECTION, LocalLink::KEY_ROOM_CODE, "");
	if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_KEY, "Room Code"),
			FSUI_CSTR("Use the same 4-12 letter or digit code on every device. It keeps two nearby sessions apart; it is not security, "
					  "so use it on a network you trust."),
			room_code.empty() ? FSUI_CSTR("Not set") : room_code.c_str(), network_enabled))
	{
		EditLocalLinkField(bsi, LocalLink::Field::RoomCode, FSUI_STR("Room Code"),
			FSUI_STR("Enter the room code, 4 to 12 letters or digits. Lower case is changed to upper case."), room_code,
			InputFilterType::None);
	}

	if (MenuButton(FSUI_ICONSTR(ICON_FA_DICE, "Generate Room Code"),
			FSUI_CSTR("Replaces the room code with a new random one. Give it to the other players."), network_enabled))
	{
		LocalLink::Apply(*bsi, LocalLink::Field::RoomCode, LocalLink::GenerateRoomCode());
		SetSettingsChanged(bsi);
	}

	if (host)
	{
		MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_ID_BADGE, "Peer ID"), FSUI_CSTR("The host is always 1."),
			std::to_string(LocalLink::HOST_PEER_ID).c_str(), false);
	}
	else
	{
		const u32 peer_id = bsi->GetUIntValue(LocalLink::SECTION, LocalLink::KEY_PEER_ID, LocalLink::HOST_PEER_ID);
		if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_ID_BADGE, "Peer ID"),
				FSUI_CSTR("Different on every device, because it decides the console's network address (2-65533). Set automatically; "
						  "change it only if two devices share one."),
				std::to_string(peer_id).c_str(), network_enabled))
		{
			EditLocalLinkField(bsi, LocalLink::Field::PeerId, FSUI_STR("Peer ID"),
				FSUI_STR("Enter this device's peer ID, 2 to 65533. Every device in a session needs a different one."),
				std::to_string(peer_id), InputFilterType::Numeric);
		}
	}

	return true;
}

void FullscreenUI::LocalLinkApiSelected(SettingsInterface* bsi, const std::string& api)
{
	// An empty room code makes the adapter refuse to start, and the game then says "network
	// adapter not connected" with nothing pointing at the code.
	if (api == LocalLink::API_NAME)
		LocalLink::SeedDefaults(*bsi);
}

//////////////////////////////////////////////////////////////////////////
// Automatic covers
//
// FullscreenUI state belongs to the GS thread. The cover job runs on its own worker thread, so its
// callbacks only fill s_cover_handoff and ask the GS thread to look at it.
//////////////////////////////////////////////////////////////////////////

namespace
{
	using namespace FullscreenUI;

	constexpr const char* COVER_NOTIFICATION_KEY = "cover_download";
	constexpr double COVER_LIST_QUIET_SECONDS = 3.0;
	constexpr double COVER_POLL_SECONDS = 0.25;

	// What the cover worker thread hands to the GS thread. Callbacks add to it and queue a drain;
	// the drain empties it. At most one drain is queued at a time, so a run that skips thousands of
	// games a second costs the GS thread one look per CPU-thread tick.
	struct CoverHandoff
	{
		std::mutex mutex;
		bool drain_queued = false;
		std::optional<Catalog::CoverProgress> progress;
		std::vector<std::pair<std::string, std::string>> saved; ///< game path, cover file
		std::optional<Catalog::CoverJobSummary> finished;
	};
	CoverHandoff s_cover_handoff;

	// True while the running job is one the user asked for (a re-download), which gets a message
	// when it ends even if nothing came of it. A run started by a refresh stays quiet unless it
	// downloaded something.
	std::atomic_bool s_cover_run_is_requested{false};

	// AutoCovers is process-wide: under Qt the Qt controller may have started the run that is going, and
	// leaving Big Picture must not cancel that. This is set while a run Big Picture started is going.
	std::atomic_bool s_cover_run_started_here{false};

	// When the game list last changed, as the poll sees it (GS thread).
	u32 s_poll_count = 0;
	const GameList::Entry* s_poll_first = nullptr;
	bool s_poll_pending = false;
	Common::Timer s_poll_timer;
	Common::Timer s_poll_changed;

	// The cover job's thread is not the CPU thread, and a push into the GS ring is only allowed from
	// the CPU thread (MTGS::RunOnGSThread), so hop through it.
	void PostToGSThread(std::function<void()> function)
	{
		Host::RunOnCPUThread([function = std::move(function)]() mutable {
			if (MTGS::IsOpen())
				MTGS::RunOnGSThread(std::move(function));
		});
	}

	void ShowCoverNotice(const std::string& title, const std::string& text, float seconds)
	{
		ImGuiFullscreen::AddNotification(COVER_NOTIFICATION_KEY, seconds, fmt::format("{} {}", ICON_FA_IMAGE, title), text, std::string());
	}

	// Takes the progress message down (a notification with no time left goes at the next draw).
	void DismissCoverNotice()
	{
		ImGuiFullscreen::AddNotification(COVER_NOTIFICATION_KEY, 0.0f, std::string(), std::string(), std::string());
	}

	// GS thread: applies what the worker reported since the last time.
	void DrainCoverHandoff()
	{
		std::vector<std::pair<std::string, std::string>> saved;
		std::optional<Catalog::CoverProgress> progress;
		std::optional<Catalog::CoverJobSummary> finished;
		{
			std::lock_guard<std::mutex> lock(s_cover_handoff.mutex);
			s_cover_handoff.drain_queued = false;
			saved.swap(s_cover_handoff.saved);
			progress.swap(s_cover_handoff.progress);
			finished.swap(s_cover_handoff.finished);
		}

		if (finished.has_value())
			s_cover_run_started_here.store(false);

		if (!IsInitialized())
			return;

		// A cover that arrived is picked up the next time its game is drawn: forget where the cover
		// was looked for (an empty answer is remembered too) and drop the picture if the file was
		// replaced.
		for (const auto& [game_path, cover_path] : saved)
		{
			s_cover_image_map.erase(game_path);
			ImGuiFullscreen::InvalidateCachedTexture(cover_path);
		}

		if (finished.has_value())
		{
			const bool requested = s_cover_run_is_requested.load();
			const u32 downloaded = finished->downloaded;

			switch (finished->end)
			{
				case Catalog::CoverJobEnd::Completed:
					if (downloaded > 0)
						ShowCoverNotice(FSUI_STR("Covers Downloaded"), fmt::format(FSUI_FSTR("New covers: {}"), downloaded), 5.0f);
					else if (requested)
						ShowCoverNotice(FSUI_STR("Covers"), FSUI_STR("No covers were available to download."), 5.0f);
					else
						DismissCoverNotice();
					break;

				case Catalog::CoverJobEnd::RateLimited:
					ShowCoverNotice(FSUI_STR("Covers Paused"),
						FSUI_STR("The cover server asked us to slow down. Downloads resume later."), 8.0f);
					break;

				case Catalog::CoverJobEnd::BackedOff:
					if (requested)
						ShowCoverNotice(FSUI_STR("Covers Paused"), FSUI_STR("The cover server is limiting requests. Try again later."), 8.0f);
					else
						DismissCoverNotice();
					break;

				case Catalog::CoverJobEnd::NetworkDown:
					if (requested)
						ShowCoverNotice(FSUI_STR("Covers"), FSUI_STR("Could not reach the cover server."), 8.0f);
					else
						DismissCoverNotice();
					break;

				case Catalog::CoverJobEnd::Cancelled:
					DismissCoverNotice();
					break;
			}
		}
		else if (progress.has_value() && progress->total > 0)
		{
			// Long enough to outlast a slow download; the message at the end of the run replaces it.
			ShowCoverNotice(FSUI_STR("Downloading Covers"),
				fmt::format(FSUI_FSTR("{} of {}: {}"), progress->done, progress->total, progress->current_title), 10.0f);
		}
	}

	void QueueCoverDrain()
	{
		{
			std::lock_guard<std::mutex> lock(s_cover_handoff.mutex);
			if (s_cover_handoff.drain_queued)
				return;
			s_cover_handoff.drain_queued = true;
		}
		PostToGSThread(&DrainCoverHandoff);
	}

	AutoCovers::Callbacks MakeCoverCallbacks()
	{
		AutoCovers::Callbacks callbacks;
		callbacks.on_progress = [](const Catalog::CoverProgress& progress) {
			{
				std::lock_guard<std::mutex> lock(s_cover_handoff.mutex);
				s_cover_handoff.progress = progress;
			}
			QueueCoverDrain();
		};
		callbacks.on_cover_saved = [](const Catalog::CoverSaved& saved) {
			{
				std::lock_guard<std::mutex> lock(s_cover_handoff.mutex);
				s_cover_handoff.saved.emplace_back(saved.id, saved.path);
			}
			QueueCoverDrain();
		};
		callbacks.on_finished = [](const Catalog::CoverJobSummary& summary) {
			{
				std::lock_guard<std::mutex> lock(s_cover_handoff.mutex);
				s_cover_handoff.finished = summary;
			}
			QueueCoverDrain();
		};
		return callbacks;
	}

	// Starts the cover job on the CPU thread (it reads the settings and the game list, and must not
	// run on the GS thread, which holds the settings lock while a settings page draws).
	//   missing_only: the covers games have none of, like after a refresh.
	//   otherwise: every game in `paths` (every game in the list when `paths` is empty), replacing
	//   what they have, because the user changed what a cover should look like.
	void StartCoverJob(bool missing_only, std::vector<std::string> paths = {})
	{
		Host::RunOnCPUThread([missing_only, paths = std::move(paths)]() mutable {
			if (!IsInitialized())
				return;

			if (AutoCovers::IsRunning())
			{
				if (!missing_only)
				{
					PostToGSThread([]() {
						ShowCoverNotice(FSUI_STR("Covers"), FSUI_STR("A cover download is already running."), 5.0f);
					});
				}
				return;
			}

			if (!missing_only && paths.empty())
			{
				auto lock = GameList::GetLock();
				const u32 count = GameList::GetEntryCount();
				for (u32 i = 0; i < count; i++)
				{
					if (const GameList::Entry* entry = GameList::GetEntryByIndex(i))
						paths.push_back(entry->path);
				}
			}

			s_cover_run_is_requested.store(!missing_only);
			const bool started = missing_only ? AutoCovers::StartAfterRefresh(MakeCoverCallbacks()) :
												AutoCovers::StartRedownload(paths, MakeCoverCallbacks());
			if (started)
				s_cover_run_started_here.store(true);
			if (!started && !missing_only)
			{
				PostToGSThread([]() {
					ShowCoverNotice(FSUI_STR("Covers"), FSUI_STR("There are no games to download covers for."), 5.0f);
				});
			}
		});
	}

	// After the style or region changed: the covers on disk still look the old way until they are
	// downloaded again, which is the user's call (it is a lot of downloading).
	void OfferCoverRedownload(std::vector<std::string> paths, std::string message)
	{
		OpenConfirmMessageDialog(FSUI_ICONSTR(ICON_FA_IMAGE, "Download Covers Again?"), std::move(message),
			[paths = std::move(paths)](bool yes) {
				if (yes)
					StartCoverJob(false, paths);
			},
			false);
	}

	const char* CoverStyleName(Catalog::CoverStyle style)
	{
		return style == Catalog::CoverStyle::Case3D ? FSUI_CSTR("3D (Case Render)") : FSUI_CSTR("2D (Flat Scan)");
	}

	const char* CoverRegionName(Catalog::CoverRegion region)
	{
		switch (region)
		{
			case Catalog::CoverRegion::USA:
				return FSUI_CSTR("USA");
			case Catalog::CoverRegion::Europe:
				return FSUI_CSTR("Europe");
			case Catalog::CoverRegion::JapanAsia:
				return FSUI_CSTR("Japan / Asia");
			case Catalog::CoverRegion::Disc:
				break;
		}
		return FSUI_CSTR("Disc's Own Region");
	}
} // namespace

void FullscreenUI::DrawAutoCoverSettings(SettingsInterface* bsi)
{
	MenuHeading(FSUI_CSTR("Covers"));

	if (DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_CLOUD_ARROW_DOWN, "Download Covers Automatically"),
			FSUI_CSTR("Downloads a cover for each game that has none after the game list is scanned. Needs an internet connection."),
			AutoCovers::SETTINGS_SECTION, AutoCovers::KEY_AUTO_DOWNLOAD, AutoCovers::DEFAULT_AUTO_DOWNLOAD))
	{
		// Turned on with a list that is not about to change: look at it now, not at the next scan.
		if (AutoCovers::IsEnabled(*bsi))
		{
			s_poll_pending = true;
			s_poll_changed.Reset();
		}
	}

	const Catalog::CoverPreferences prefs = AutoCovers::LoadPreferences(*bsi);

	if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_CUBE, "Cover Style"),
			FSUI_CSTR("Flat scans of the box front, or 3D renders of the case. Applies to covers downloaded from now on."),
			CoverStyleName(prefs.style)))
	{
		ImGuiFullscreen::ChoiceDialogOptions options;
		options.emplace_back(FSUI_STR("2D (Flat Scan)"), prefs.style == Catalog::CoverStyle::Flat2D);
		options.emplace_back(FSUI_STR("3D (Case Render)"), prefs.style == Catalog::CoverStyle::Case3D);

		OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_CUBE, "Cover Style"), false, std::move(options),
			[bsi, current = prefs.style](s32 index, const std::string& title, bool checked) {
				if (index < 0)
					return;

				const Catalog::CoverStyle style = (index == 1) ? Catalog::CoverStyle::Case3D : Catalog::CoverStyle::Flat2D;
				{
					auto lock = Host::GetSettingsLock();
					AutoCovers::SetStyle(*bsi, style);
					SetSettingsChanged(bsi);
				}
				// CloseChoiceDialog() destroys this lambda and its captures: read them first.
				const bool changed = (style != current);
				CloseChoiceDialog();

				if (changed)
				{
					OfferCoverRedownload({}, FSUI_STR("Covers you already have keep their old style until they are downloaded again. Download them again now?"));
				}
			});
	}

	if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_GLOBE, "Cover Region"),
			FSUI_CSTR("Which release's cover to download: the disc's own, or the one from another region. Applies to covers downloaded from now on."),
			CoverRegionName(prefs.region)))
	{
		ImGuiFullscreen::ChoiceDialogOptions options;
		for (int region = 0; region < Catalog::COVER_REGION_COUNT; region++)
			options.emplace_back(CoverRegionName(static_cast<Catalog::CoverRegion>(region)), static_cast<int>(prefs.region) == region);

		OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_GLOBE, "Cover Region"), false, std::move(options),
			[bsi, current = prefs.region](s32 index, const std::string& title, bool checked) {
				if (index < 0 || index >= Catalog::COVER_REGION_COUNT)
					return;

				const Catalog::CoverRegion region = static_cast<Catalog::CoverRegion>(index);
				{
					auto lock = Host::GetSettingsLock();
					AutoCovers::SetLibraryRegion(*bsi, region);
					SetSettingsChanged(bsi);
				}
				// CloseChoiceDialog() destroys this lambda and its captures: read them first.
				const bool changed = (region != current);
				CloseChoiceDialog();

				if (changed)
				{
					OfferCoverRedownload({}, FSUI_STR("Covers you already have keep their old region until they are downloaded again. Download them again now?"));
				}
			});
	}
}

void FullscreenUI::DrawCoverRegionPinSetting(const GameList::Entry& entry)
{
	// The region swap works from the PS2 game database, which has no PS1 or arcade serials.
	if (entry.type != GameList::EntryType::PS2Disc || !Catalog::IsUsableCoverSerial(entry.serial))
		return;

	// The pins are one table in the global settings, not part of the game's own settings.
	SettingsInterface* base = Host::Internal::GetBaseSettingsLayer();
	const Catalog::CoverPreferences prefs = AutoCovers::LoadPreferences(*base);
	const std::optional<Catalog::CoverRegion> pin = prefs.PinFor(entry.serial);

	if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_GLOBE, "Cover Region"),
			FSUI_CSTR("Use the cover of another region's release of this game. Without a choice here, the library setting applies."),
			pin.has_value() ? CoverRegionName(*pin) : FSUI_CSTR("Same as Library")))
	{
		// Index 0 follows the library (no pin); 1 is the disc's own region and 2-4 the others.
		ImGuiFullscreen::ChoiceDialogOptions options;
		options.emplace_back(FSUI_STR("Same as Library"), !pin.has_value());
		for (int region = 0; region < Catalog::COVER_REGION_COUNT; region++)
			options.emplace_back(CoverRegionName(static_cast<Catalog::CoverRegion>(region)), pin.has_value() && static_cast<int>(*pin) == region);

		OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_GLOBE, "Cover Region"), false, std::move(options),
			[base, serial = entry.serial, path = entry.path, current = pin](s32 index, const std::string& title, bool checked) {
				if (index < 0 || index > Catalog::COVER_REGION_COUNT)
					return;

				std::optional<Catalog::CoverRegion> chosen;
				if (index > 0)
					chosen = static_cast<Catalog::CoverRegion>(index - 1);
				{
					auto lock = Host::GetSettingsLock();
					AutoCovers::SetGameRegionPin(*base, serial, chosen);
					SetSettingsChanged(base);
				}
				// CloseChoiceDialog() destroys this lambda and its captures: read them first.
				const bool changed = (chosen != current);
				std::string game_path = path;
				CloseChoiceDialog();

				if (changed)
					OfferCoverRedownload({std::move(game_path)}, FSUI_STR("The cover you have keeps its old region until it is downloaded again. Download it again now?"));
			});
	}
}

void FullscreenUI::PollAutoCovers()
{
	// Nothing tells Big Picture that a game list refresh has finished: the hosts run the scan on
	// their own thread and report to nobody. The list does show it, though. Each refresh builds a new
	// list and fills it entry by entry, so a list that stopped changing a few seconds ago is the end
	// of one. A scan that pauses longer than that gets another look when it adds more.
	if (VMManager::HasValidVM() || s_poll_timer.GetTimeSeconds() < COVER_POLL_SECONDS)
		return;
	s_poll_timer.Reset();

	u32 count;
	const GameList::Entry* first;
	{
		auto lock = GameList::GetLock();
		count = GameList::GetEntryCount();
		first = count ? GameList::GetEntryByIndex(0) : nullptr;
	}

	if (count != s_poll_count || first != s_poll_first)
	{
		s_poll_count = count;
		s_poll_first = first;
		s_poll_pending = (count > 0);
		s_poll_changed.Reset();
		return;
	}

	if (s_poll_pending && s_poll_changed.GetTimeSeconds() >= COVER_LIST_QUIET_SECONDS && !AutoCovers::IsRunning())
	{
		s_poll_pending = false;
		StartCoverJob(true);
	}
}

void FullscreenUI::ShutdownAutoCovers()
{
	// Stops the worker first, so nothing is added to the hand-off after it is cleared. Only a run this file
	// started: another UI's run (Qt's) has its own callbacks and carries on.
	if (s_cover_run_started_here.exchange(false))
		AutoCovers::Shutdown();

	{
		std::lock_guard<std::mutex> lock(s_cover_handoff.mutex);
		s_cover_handoff.drain_queued = false;
		s_cover_handoff.progress.reset();
		s_cover_handoff.saved.clear();
		s_cover_handoff.finished.reset();
	}
	s_cover_run_is_requested.store(false);
	s_poll_count = 0;
	s_poll_first = nullptr;
	s_poll_pending = false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// Same scheme as FullscreenUI.cpp and FullscreenUI_Settings.cpp: tools/generate_fullscreen_ui_translation_strings.py
// regenerates the block below from the FSUI_* macros above.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "That address is too long.");
TRANSLATE_NOOP("FullscreenUI", "The port must be a number.");
TRANSLATE_NOOP("FullscreenUI", "The port must be between 1024 and 65535.");
TRANSLATE_NOOP("FullscreenUI", "The peer ID must be a number.");
TRANSLATE_NOOP("FullscreenUI", "The peer ID must be between 2 and 65533. 1 is the host.");
TRANSLATE_NOOP("FullscreenUI", "The room code must be 4 to 12 letters or digits.");
TRANSLATE_NOOP("FullscreenUI", "Local Link");
TRANSLATE_NOOP("FullscreenUI", "Local Link connects consoles over a network, so it only works with games that have their own LAN or System Link mode, for example Gran Turismo 4, TimeSplitters 2 and Midnight Club 3. If a game asks you to sign in to a server instead, it was online-only.\n\nOne device hosts and starts the game; every other device enters the host's address with the same port and room code. Each player needs their own copy of the game. A shared Wi-Fi network or a phone hotspot is easiest; over the internet, a VPN such as Tailscale on every device is the simplest way.");
TRANSLATE_NOOP("FullscreenUI", "Address copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Host Address");
TRANSLATE_NOOP("FullscreenUI", "Enter the address of the host: an IPv4 address such as 192.168.1.42, or a host name.");
TRANSLATE_NOOP("FullscreenUI", "Local Link Port");
TRANSLATE_NOOP("FullscreenUI", "Enter the port, 1024 to 65535. It must be the same on every device.");
TRANSLATE_NOOP("FullscreenUI", "Room Code");
TRANSLATE_NOOP("FullscreenUI", "Enter the room code, 4 to 12 letters or digits. Lower case is changed to upper case.");
TRANSLATE_NOOP("FullscreenUI", "Peer ID");
TRANSLATE_NOOP("FullscreenUI", "Enter this device's peer ID, 2 to 65533. Every device in a session needs a different one.");
TRANSLATE_NOOP("FullscreenUI", "Covers Downloaded");
TRANSLATE_NOOP("FullscreenUI", "Covers");
TRANSLATE_NOOP("FullscreenUI", "No covers were available to download.");
TRANSLATE_NOOP("FullscreenUI", "Covers Paused");
TRANSLATE_NOOP("FullscreenUI", "The cover server asked us to slow down. Downloads resume later.");
TRANSLATE_NOOP("FullscreenUI", "The cover server is limiting requests. Try again later.");
TRANSLATE_NOOP("FullscreenUI", "Could not reach the cover server.");
TRANSLATE_NOOP("FullscreenUI", "Downloading Covers");
TRANSLATE_NOOP("FullscreenUI", "A cover download is already running.");
TRANSLATE_NOOP("FullscreenUI", "There are no games to download covers for.");
TRANSLATE_NOOP("FullscreenUI", "2D (Flat Scan)");
TRANSLATE_NOOP("FullscreenUI", "3D (Case Render)");
TRANSLATE_NOOP("FullscreenUI", "Covers you already have keep their old style until they are downloaded again. Download them again now?");
TRANSLATE_NOOP("FullscreenUI", "Covers you already have keep their old region until they are downloaded again. Download them again now?");
TRANSLATE_NOOP("FullscreenUI", "Same as Library");
TRANSLATE_NOOP("FullscreenUI", "The cover you have keeps its old region until it is downloaded again. Download it again now?");
TRANSLATE_NOOP("FullscreenUI", "Upscales game textures on the CPU in the background using RAISR. Uses more memory and CPU. Hardware renderers only.");
TRANSLATE_NOOP("FullscreenUI", "Local Link needs a game with its own LAN or System Link mode. Select to read how to connect.");
TRANSLATE_NOOP("FullscreenUI", "This device hosts the session. Turn off to join a host instead.");
TRANSLATE_NOOP("FullscreenUI", "Give one of these to the other players to enter on their device. Select to copy.");
TRANSLATE_NOOP("FullscreenUI", "Not connected");
TRANSLATE_NOOP("FullscreenUI", "The address shown on the host device, for example 192.168.1.42, or a VPN address such as 100.101.102.103. A host name also works.");
TRANSLATE_NOOP("FullscreenUI", "Not set");
TRANSLATE_NOOP("FullscreenUI", "Use the same port on every device (1024-65535). There is no automatic negotiation.");
TRANSLATE_NOOP("FullscreenUI", "Use the same 4-12 letter or digit code on every device. It keeps two nearby sessions apart; it is not security, so use it on a network you trust.");
TRANSLATE_NOOP("FullscreenUI", "Replaces the room code with a new random one. Give it to the other players.");
TRANSLATE_NOOP("FullscreenUI", "The host is always 1.");
TRANSLATE_NOOP("FullscreenUI", "Different on every device, because it decides the console's network address (2-65533). Set automatically; change it only if two devices share one.");
TRANSLATE_NOOP("FullscreenUI", "USA");
TRANSLATE_NOOP("FullscreenUI", "Europe");
TRANSLATE_NOOP("FullscreenUI", "Japan / Asia");
TRANSLATE_NOOP("FullscreenUI", "Disc's Own Region");
TRANSLATE_NOOP("FullscreenUI", "Downloads a cover for each game that has none after the game list is scanned. Needs an internet connection.");
TRANSLATE_NOOP("FullscreenUI", "Flat scans of the box front, or 3D renders of the case. Applies to covers downloaded from now on.");
TRANSLATE_NOOP("FullscreenUI", "Which release's cover to download: the disc's own, or the one from another region. Applies to covers downloaded from now on.");
TRANSLATE_NOOP("FullscreenUI", "Use the cover of another region's release of this game. Without a choice here, the library setting applies.");
TRANSLATE_NOOP("FullscreenUI", "New covers: {}");
TRANSLATE_NOOP("FullscreenUI", "{} of {}: {}");
TRANSLATE_NOOP("FullscreenUI", "Off (Default)");
TRANSLATE_NOOP("FullscreenUI", "RAISR 2x");
TRANSLATE_NOOP("FullscreenUI", "RAISR 4x");
TRANSLATE_NOOP("FullscreenUI", "Upscale Textures");
TRANSLATE_NOOP("FullscreenUI", "OK");
TRANSLATE_NOOP("FullscreenUI", "Which Games Work?");
TRANSLATE_NOOP("FullscreenUI", "Host Local Game");
TRANSLATE_NOOP("FullscreenUI", "This Device's Address");
TRANSLATE_NOOP("FullscreenUI", "Generate Room Code");
TRANSLATE_NOOP("FullscreenUI", "Download Covers Again?");
TRANSLATE_NOOP("FullscreenUI", "Download Covers Automatically");
TRANSLATE_NOOP("FullscreenUI", "Cover Style");
TRANSLATE_NOOP("FullscreenUI", "Cover Region");
// TRANSLATION-STRING-AREA-END
#endif
