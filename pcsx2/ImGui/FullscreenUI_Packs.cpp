// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture rows for features brought over from the Android app. The other FullscreenUI files
// call in here with one line each (declarations at the end of FullscreenUI_Internal.h); anything
// longer than a row lives here, so an upstream rework of those pages touches these features in a
// handful of places.

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

#include "common/SettingsInterface.h"
#include "common/Timer.h"

#include "IconsFontAwesome.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace LocalLink = Catalog::LocalLink;

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
TRANSLATE_NOOP("FullscreenUI", "Off (Default)");
TRANSLATE_NOOP("FullscreenUI", "RAISR 2x");
TRANSLATE_NOOP("FullscreenUI", "RAISR 4x");
TRANSLATE_NOOP("FullscreenUI", "Upscale Textures");
TRANSLATE_NOOP("FullscreenUI", "OK");
TRANSLATE_NOOP("FullscreenUI", "Which Games Work?");
TRANSLATE_NOOP("FullscreenUI", "Host Local Game");
TRANSLATE_NOOP("FullscreenUI", "This Device's Address");
TRANSLATE_NOOP("FullscreenUI", "Generate Room Code");
// TRANSLATION-STRING-AREA-END
#endif
