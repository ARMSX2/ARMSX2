// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ImGui/FullscreenUIShot.h"

#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSHeadlessPresent.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "Achievements.h"
#include "GameList.h"
#include "Host.h"
#include "Host/AudioStream.h"
#include "INISettingsInterface.h"
#include "ImGui/FullscreenUI_Internal.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"
#include "MTGS.h"
#include "Patch.h"
#include "USB/USB.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "IconsFontAwesome.h"

#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace FullscreenUI
{
	namespace
	{
		//////////////////////////////////////////////////////////////////////////
		// Page names. The one place an agent's page name meets the Big Picture state.
		//////////////////////////////////////////////////////////////////////////

		void OpenLanding()
		{
			s_about_window_open = false;
			SwitchToLanding();
		}

		void OpenSetup()
		{
			s_about_window_open = false;
			SwitchToSetup();
		}

		void OpenStartGame()
		{
			s_about_window_open = false;
			s_current_main_window = MainWindowType::StartGame;
			QueueResetFocus(FocusResetType::WindowChanged);
		}

		void OpenExit()
		{
			s_about_window_open = false;
			s_current_main_window = MainWindowType::Exit;
			QueueResetFocus(FocusResetType::WindowChanged);
		}

		void OpenGameList()
		{
			s_about_window_open = false;
			SwitchToGameList();
		}

		void OpenGameListSettings()
		{
			s_about_window_open = false;
			s_current_main_window = MainWindowType::GameListSettings;
			QueueResetFocus(FocusResetType::WindowChanged);
		}

		// A window on top of whatever is showing, as the landing page's About button opens it.
		void OpenAbout()
		{
			OpenAboutWindow();
		}

		void OpenSettings()
		{
			s_about_window_open = false;
			SwitchToSettings();
			QueueResetFocus(FocusResetType::WindowChanged);
		}

		// The memory card backups window, reached from the Memory Card page in normal use. It lists
		// what is in <data root>/memcard-backups, so seed that folder (or boot once) before opening it.
		void OpenMemcardBackups()
		{
			s_about_window_open = false;
			OpenMemcardBackupsWindow();
		}

		// The question a boot asks when a memory card cannot be read, with a sample card and no effect,
		// over whatever is showing.
		void OpenMemcardPrompt()
		{
			OpenMemcardPromptPreview();
		}

		// Starts a game list scan and shows the list, for pages that need games in it. The scan runs
		// on the host's own thread, so give it a few frames (UIWaitFrames) before the next page.
		void OpenRescan()
		{
			Host::RefreshGameListAsync(false);
			OpenGameList();
		}

		// The first game in the list: its own settings, starting on the Summary page. The settings
		// pages of one game only exist for a game, which a page name alone cannot supply.
		bool OpenFirstGameSettings(std::string* error)
		{
			auto lock = GameList::GetLock();
			const GameList::Entry* entry = GameList::GetEntryCount() ? GameList::GetEntryByIndex(0) : nullptr;
			if (!entry)
			{
				*error = "the game list is empty (open 'rescan' first and wait a few frames)";
				return false;
			}

			s_about_window_open = false;
			SwitchToGameSettings(entry);
			return true;
		}

		// The Load State selector of the first game in the list: its numbered slots and autosaves.
		bool OpenFirstGameLoadState(std::string* error)
		{
			std::string path;
			{
				auto lock = GameList::GetLock();
				const GameList::Entry* entry = GameList::GetEntryCount() ? GameList::GetEntryByIndex(0) : nullptr;
				if (entry)
					path = entry->path;
			}
			if (path.empty())
			{
				*error = "the game list is empty (open 'rescan' first and wait a few frames)";
				return false;
			}

			s_about_window_open = false;
			s_current_main_window = MainWindowType::GameList;
			if (!OpenLoadStateSelectorForGame(path))
			{
				*error = "the first game has no save states or autosaves";
				return false;
			}
			return true;
		}

		struct PageEntry
		{
			const char* name;
			void (*open)();
		};

		constexpr PageEntry kPages[] = {
			{"landing", &OpenLanding},
			{"setup", &OpenSetup},
			{"startgame", &OpenStartGame},
			{"exit", &OpenExit},
			{"gamelist", &OpenGameList},
			{"gamelistsettings", &OpenGameListSettings},
			{"about", &OpenAbout},
			{"settings", &OpenSettings},
			{"rescan", &OpenRescan},
			{"memcardbackups", &OpenMemcardBackups},
			{"memcardprompt", &OpenMemcardPrompt},
		};

		// `global` pages are the ones the settings window lists when it is not editing one game's
		// settings; the others need a game, which this does not set up.
		struct SettingsPageEntry
		{
			const char* name;
			SettingsPage page;
			bool global;
		};

		constexpr SettingsPageEntry kSettingsPages[] = {
			{"summary", SettingsPage::Summary, false},
			{"interface", SettingsPage::Interface, true},
			{"bios", SettingsPage::BIOS, true},
			{"emulation", SettingsPage::Emulation, true},
			{"graphics", SettingsPage::Graphics, true},
			{"osd", SettingsPage::OSD, true},
			{"audio", SettingsPage::Audio, true},
			{"memorycard", SettingsPage::MemoryCard, true},
			{"networkhdd", SettingsPage::NetworkHDD, true},
			{"folders", SettingsPage::Folders, true},
			{"achievements", SettingsPage::Achievements, true},
			{"controller", SettingsPage::Controller, true},
			{"hotkey", SettingsPage::Hotkey, true},
			{"advanced", SettingsPage::Advanced, true},
			{"patches", SettingsPage::Patches, false},
			{"cheats", SettingsPage::Cheats, false},
			{"gamefixes", SettingsPage::GameFixes, false},
			{"drivers", SettingsPage::Drivers, true},
			{"arcade", SettingsPage::Arcade, true},
			{"texturepacks", SettingsPage::TexturePacks, true},
		};

		constexpr bool SettingsPageTableIsInEnumOrder()
		{
			for (size_t i = 0; i < std::size(kSettingsPages); i++)
			{
				if (static_cast<size_t>(kSettingsPages[i].page) != i)
					return false;
			}
			return true;
		}

		// A new SettingsPage needs a name above so UIOpenPage can reach it.
		static_assert(std::size(kSettingsPages) == static_cast<size_t>(SettingsPage::Count),
			"add the new SettingsPage to kSettingsPages in FullscreenUIShot.cpp");
		static_assert(SettingsPageTableIsInEnumOrder(), "kSettingsPages must list SettingsPage in enum order");

		// gamesettings:<page>[:browse]. The first game's settings opened on one of the pages, for the
		// pages that only exist for a game; with :browse on patches or cheats, the same search a press
		// of that page's Browse Online row starts (it needs the network, so wait for it to finish).
		bool OpenFirstGameSettingsPage(std::string_view spec, std::string* error)
		{
			std::string_view page_name = spec, action;
			if (const size_t colon = spec.find(':'); colon != std::string_view::npos)
			{
				page_name = spec.substr(0, colon);
				action = spec.substr(colon + 1);
			}

			const SettingsPageEntry* page = nullptr;
			for (const SettingsPageEntry& entry : kSettingsPages)
			{
				if (StringUtil::compareNoCase(page_name, entry.name))
					page = &entry;
			}
			if (!page)
			{
				*error = fmt::format("unknown settings page '{}'", page_name);
				return false;
			}

			const bool browsable = (page->page == SettingsPage::Patches || page->page == SettingsPage::Cheats);
			if (!action.empty() && !(browsable && StringUtil::compareNoCase(action, "browse")))
			{
				*error = fmt::format("'{}' has no action '{}' (only patches and cheats have :browse)", page_name, action);
				return false;
			}

			if (!OpenFirstGameSettings(error))
				return false;
			s_settings_page = page->page;
			if (!action.empty())
				BrowseOnlinePatches(page->page == SettingsPage::Cheats);
			return true;
		}

		//////////////////////////////////////////////////////////////////////////
		// textdialog: Big Picture's text entry dialog (the on-screen keyboard), open on a sample question
		//////////////////////////////////////////////////////////////////////////

		struct ShotPadKey
		{
			const char* name;
			ImGuiKey key;
		};

		// The controller's buttons as ImGui sees them. "confirm" and "cancel" follow the user's OK-button
		// setting; the compass names are the buttons themselves.
		constexpr ShotPadKey kShotPadKeys[] = {
			{"up", ImGuiKey_GamepadDpadUp},
			{"down", ImGuiKey_GamepadDpadDown},
			{"left", ImGuiKey_GamepadDpadLeft},
			{"right", ImGuiKey_GamepadDpadRight},
			{"south", ImGuiKey_GamepadFaceDown},
			{"east", ImGuiKey_GamepadFaceRight},
			{"west", ImGuiKey_GamepadFaceLeft},
			{"north", ImGuiKey_GamepadFaceUp},
			{"l1", ImGuiKey_GamepadL1},
			{"r1", ImGuiKey_GamepadR1},
			{"l2", ImGuiKey_GamepadL2},
			{"select", ImGuiKey_GamepadBack},
			{"start", ImGuiKey_GamepadStart},
		};

		// A physical keyboard's editing keys.
		constexpr ShotPadKey kShotKeyboardKeys[] = {
			{"backspace", ImGuiKey_Backspace},
			{"delete", ImGuiKey_Delete},
			{"left", ImGuiKey_LeftArrow},
			{"right", ImGuiKey_RightArrow},
			{"home", ImGuiKey_Home},
			{"end", ImGuiKey_End},
			{"enter", ImGuiKey_Enter},
			{"escape", ImGuiKey_Escape},
		};

		void ShotRunFrames(u32 count)
		{
			for (u32 i = 0; i < count; i++)
				GSPresentCurrentFrame();
		}

		// One press and release of `key`, with frames in between so the dialog sees both.
		void ShotPress(ImGuiKey key)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.AddKeyAnalogEvent(key, true, 1.0f);
			ShotRunFrames(2);
			io.AddKeyAnalogEvent(key, false, 0.0f);
			ShotRunFrames(2);
		}

		// A comma separated list of key names, each optionally followed by *N to press it N times.
		bool ShotPressList(std::string_view list, const ShotPadKey* table, size_t table_size, bool gamepad, std::string* error)
		{
			for (const std::string_view item : StringUtil::SplitString(list, ',', true))
			{
				std::string_view name = item;
				u32 times = 1;
				if (const size_t star = item.find('*'); star != std::string_view::npos)
				{
					name = item.substr(0, star);
					times = StringUtil::FromChars<u32>(item.substr(star + 1)).value_or(1);
				}

				std::optional<ImGuiKey> key;
				if (gamepad && StringUtil::compareNoCase(name, "confirm"))
					key = ImGui::GetIO().ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
				else if (gamepad && StringUtil::compareNoCase(name, "cancel"))
					key = ImGui::GetIO().ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;
				for (size_t i = 0; i < table_size && !key; i++)
				{
					if (StringUtil::compareNoCase(name, table[i].name))
						key = table[i].key;
				}
				if (!key)
				{
					*error = fmt::format("unknown key '{}' in textdialog", name);
					return false;
				}

				for (u32 i = 0; i < times; i++)
					ShotPress(*key);
			}
			return true;
		}

		// A left click at each "x,y" (screen pixels) of a "+" separated list, with the mouse moved there first.
		bool ShotClickList(std::string_view list, std::string* error)
		{
			ImGuiIO& io = ImGui::GetIO();
			for (const std::string_view item : StringUtil::SplitString(list, '+', true))
			{
				const size_t comma = item.find(',');
				const std::optional<float> x = (comma == std::string_view::npos) ? std::nullopt : StringUtil::FromChars<float>(item.substr(0, comma));
				const std::optional<float> y = (comma == std::string_view::npos) ? std::nullopt : StringUtil::FromChars<float>(item.substr(comma + 1));
				if (!x || !y)
				{
					*error = fmt::format("click wants x,y pixels, got '{}'", item);
					return false;
				}

				io.AddMousePosEvent(*x, *y);
				ShotRunFrames(2);
				io.AddMouseButtonEvent(0, true);
				ShotRunFrames(2);
				io.AddMouseButtonEvent(0, false);
				ShotRunFrames(2);
			}
			return true;
		}

		// padkeys:<keys>: plays pad buttons (as in textdialog's keys=) on whatever Big Picture is showing, so a
		// shot can start from a real row (settings:memorycard, then padkeys:down,confirm opens Create Memory Card).
		bool PlayPadKeys(std::string_view keys, std::string* error)
		{
			if (!GSHeadless::IsEnabled())
			{
				*error = "padkeys needs --headless (it supplies the frames the presses are spread over)";
				return false;
			}
			ShotRunFrames(2);
			if (!ShotPressList(keys, kShotPadKeys, std::size(kShotPadKeys), true, error))
				return false;
			ShotRunFrames(2);
			return true;
		}

		// textdialog[:numeric|:ip][:text=<initial text>][:keys=<pad keys>][:type=<characters>][:pkeys=<keyboard keys>][:click=<x,y+x,y..>]
		// Opens the text entry dialog over whatever is showing, as Local Link's Host Address row would (a number
		// pad with :numeric or :ip), then plays what follows as a user would: pad buttons (up down left right
		// south east west north l1 r1 l2 select start confirm cancel, e.g. keys=down*2,confirm), characters typed on
		// a physical keyboard, keyboard keys (backspace delete left right home end enter escape), and mouse clicks.
		// Needs --headless, which supplies the frames the presses are spread over. Values cannot contain ':'.
		bool OpenTextDialog(std::string_view spec, std::string* error)
		{
			InputFilterType filter = InputFilterType::None;
			std::string initial, pad_keys, typed, keyboard_keys, clicks;
			bool long_message = false;
			for (const std::string_view token : StringUtil::SplitString(spec, ':', true))
			{
				const size_t equals = token.find('=');
				const std::string_view key = token.substr(0, equals);
				const std::string_view value = (equals == std::string_view::npos) ? std::string_view() : token.substr(equals + 1);
				if (StringUtil::compareNoCase(key, "long"))
					long_message = true;
				else if (StringUtil::compareNoCase(key, "numeric"))
					filter = InputFilterType::Numeric;
				else if (StringUtil::compareNoCase(key, "ip"))
					filter = InputFilterType::IPAddress;
				else if (StringUtil::compareNoCase(key, "text"))
					initial = value;
				else if (StringUtil::compareNoCase(key, "keys"))
					pad_keys = value;
				else if (StringUtil::compareNoCase(key, "type"))
					typed = value;
				else if (StringUtil::compareNoCase(key, "pkeys"))
					keyboard_keys = value;
				else if (StringUtil::compareNoCase(key, "click"))
					clicks = value;
				else
				{
					*error = fmt::format("unknown textdialog option '{}' (long, numeric, ip, text=, keys=, type=, pkeys=, click=)", token);
					return false;
				}
			}

			const bool scripted = !pad_keys.empty() || !typed.empty() || !keyboard_keys.empty() || !clicks.empty();
			if (scripted && !GSHeadless::IsEnabled())
			{
				*error = "textdialog keys need --headless (it supplies the frames the presses are spread over)";
				return false;
			}

			s_about_window_open = false;
			ImGuiFullscreen::CloseInputDialog();
			const char* const title = (filter == InputFilterType::None) ? "Host Address" : (filter == InputFilterType::Numeric) ? "Local Link Port" : "PS2 IP Address";
			const char* const message = (filter == InputFilterType::None) ?
			                                "Enter the address of the host: an IPv4 address such as 192.168.1.42, or a host name." :
			                                (filter == InputFilterType::Numeric) ? "Enter the port, 1024 to 65535. It must be the same on every device." :
			                                                                       "Enter the PS2 IP address";
			// The longest message any caller gives the dialog, from Save Profile on the Controller page.
			const std::string long_text = "Custom input profiles are used to override the Shared input profile for specific games.\n\n"
			                              "To apply a custom input profile to a game, go to its Game Properties, then change the 'Input Profile' on "
			                              "the Summary tab.\n\nEnter the name for the new input profile:";
			// What OK submits shows as a toast, for the shot to read.
			OpenInputStringDialog(
				title, long_message ? long_text : std::string(message), std::string(), ICON_FA_CHECK " OK",
				[](std::string text) { ShowToast(ICON_FA_CHECK, fmt::format("OK with '{}'", text), 60.0f); }, std::move(initial), filter);

			if (scripted)
			{
				// The dialog ignores input on the frame it opens, and its window needs a frame to get its size.
				ShotRunFrames(4);
				if (!ShotPressList(pad_keys, kShotPadKeys, std::size(kShotPadKeys), true, error))
					return false;
				if (!typed.empty())
				{
					ImGui::GetIO().AddInputCharactersUTF8(typed.c_str());
					ShotRunFrames(3);
				}
				if (!ShotPressList(keyboard_keys, kShotKeyboardKeys, std::size(kShotKeyboardKeys), false, error))
					return false;
				if (!ShotClickList(clicks, error))
					return false;
				ShotRunFrames(2);
			}
			return true;
		}
	} // namespace

	std::string PageNames()
	{
		std::string out;
		for (const PageEntry& page : kPages)
		{
			if (!out.empty())
				out += ", ";
			out += page.name;
		}
		out += ", gamesettings, gamesettings:<page>, gamesettings:patches:browse, gamesettings:cheats:browse, loadstate, " + ShotArcadeNames();
		out += ", texturepacks:<" + TexturePacksShotVariants() + ">";
		out += ", textdialog[:long][:numeric|:ip][:text=..][:keys=..][:type=..][:pkeys=..][:click=..], padkeys:<keys>";
		for (const SettingsPageEntry& page : kSettingsPages)
		{
			if (page.global)
			{
				out += ", settings:";
				out += page.name;
			}
		}
		return out;
	}

	bool OpenPageByName(std::string_view name, std::string* error)
	{
		if (!ImGuiManager::InitializeFullscreenUI())
		{
			*error = "Big Picture could not start";
			return false;
		}

		std::string_view settings_page;
		std::string_view top = name;
		if (const size_t colon = name.find(':'); colon != std::string_view::npos)
		{
			top = name.substr(0, colon);
			settings_page = name.substr(colon + 1);
			if (StringUtil::compareNoCase(top, "texturepacks"))
				return OpenTexturePacksForShot(settings_page, error);
			if (StringUtil::compareNoCase(top, "gamesettings"))
				return OpenFirstGameSettingsPage(settings_page, error);
			if (StringUtil::compareNoCase(top, "textdialog"))
				return OpenTextDialog(settings_page, error);
			if (StringUtil::compareNoCase(top, "padkeys"))
				return PlayPadKeys(settings_page, error);
			if (!StringUtil::compareNoCase(top, "settings"))
			{
				*error = fmt::format("'{}' has no sub-pages (only settings:<page> does)", top);
				return false;
			}
		}

		if (!settings_page.empty())
		{
			for (const SettingsPageEntry& entry : kSettingsPages)
			{
				if (!StringUtil::compareNoCase(settings_page, entry.name))
					continue;

				if (!entry.global)
				{
					*error = fmt::format("settings page '{}' only exists while editing one game's settings", entry.name);
					return false;
				}
				OpenSettings();
				s_settings_page = entry.page;
				return true;
			}
			*error = fmt::format("unknown settings page '{}'", settings_page);
			return false;
		}

		if (StringUtil::compareNoCase(top, "gamesettings"))
			return OpenFirstGameSettings(error);

		if (StringUtil::compareNoCase(top, "loadstate"))
			return OpenFirstGameLoadState(error);
		if (StringUtil::compareNoCase(top, "textdialog"))
			return OpenTextDialog({}, error);
		// arcadelaunch, arcadepick, ...: the arcade screens' states (FullscreenUI_Arcade.cpp).
		if (StringUtil::StartsWithNoCase(top, "arcade"))
			return ShotArcade(StringUtil::toLower(top.substr(6)), error);

		for (const PageEntry& page : kPages)
		{
			if (StringUtil::compareNoCase(top, page.name))
			{
				page.open();
				return true;
			}
		}
		*error = fmt::format("unknown page '{}'", name);
		return false;
	}
} // namespace FullscreenUI

namespace FullscreenUIShot
{
	namespace
	{
		bool ReadU32(const u8*& cursor, u32& avail, u32* out)
		{
			if (avail < sizeof(u32))
				return false;
			std::memcpy(out, cursor, sizeof(u32));
			cursor += sizeof(u32);
			avail -= sizeof(u32);
			return true;
		}

		// [u32 len][len bytes], no terminator. Same shape PINE's other string arguments use.
		bool ReadString(const u8*& cursor, u32& avail, std::string* out)
		{
			u32 len;
			if (!ReadU32(cursor, avail, &len) || len > avail)
				return false;
			out->assign(reinterpret_cast<const char*>(cursor), len);
			cursor += len;
			avail -= len;
			return true;
		}

		std::string JsonEscape(std::string_view s)
		{
			std::string out;
			out.reserve(s.size());
			for (const char c : s)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if (static_cast<unsigned char>(c) < 0x20)
							out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
						else
							out += c;
						break;
				}
			}
			return out;
		}

		std::string Failure(std::string_view error)
		{
			return fmt::format("{{\"ok\":false,\"error\":\"{}\"}}", JsonEscape(error));
		}

		// Runs `fn` on the GS thread and returns once it has finished.
		//
		// The MTGS ring has one producer, the CPU thread, so a PINE thread pushing a packet itself
		// races the EE thread's writes. Hop to the CPU thread first and push from there, the way
		// PINE.cpp's other GS-touching commands do.
		bool RunOnGSThreadAndWait(std::function<void()> fn)
		{
			if (!MTGS::IsOpen())
				return false;

			Host::RunOnCPUThread(
				[&fn]() {
					MTGS::RunOnGSThread(fn);
					MTGS::WaitGS(false);
				},
				true);
			return true;
		}

		std::string OpenPage(const std::string& name)
		{
			std::string error;
			bool opened = false;
			if (!RunOnGSThreadAndWait([&]() { opened = FullscreenUI::OpenPageByName(name, &error); }))
				return Failure("the GS is not running");

			if (!opened)
			{
				return fmt::format("{{\"ok\":false,\"error\":\"{}\",\"pages\":\"{}\"}}", JsonEscape(error),
					JsonEscape(FullscreenUI::PageNames()));
			}
			return fmt::format("{{\"ok\":true,\"page\":\"{}\"}}", JsonEscape(name));
		}

		// ImGui's clock when a screenshot starts drawing. The selected item's border pulses with
		// ImGui::GetTime(), so two shots of the same page differ unless they start from the same
		// time. Frames carry a fixed time step in headless mode (ImGuiManager::NewFrame), so the
		// only thing left to pin is where the clock starts, which is here, every time. That can
		// move ImGui's clock backwards by the few seconds since the last shot; the absolute
		// timestamps it affects (OSD message expiry, mouse double-click) just see a slightly
		// longer or shorter interval once, which a screenshot tool can live with.
		constexpr double kUiClockEpoch = 10000.0;
		constexpr u32 kMaxSettleFrames = 3600;

		// GS thread. Draws `settle_frames` frames at the idle pace, so animations finish and
		// asynchronous loads (covers, game list) get wall time to land, then writes one more.
		bool SettleAndCapture(const std::string& path, u32 settle_frames, u32* width, u32* height, Error* error)
		{
			if (!GSHeadless::IsEnabled() || !g_gs_device)
			{
				Error::SetStringView(error, "headless present is not enabled (start the host with --headless WxH)");
				return false;
			}
			if (settle_frames > kMaxSettleFrames)
			{
				Error::SetStringFmt(error, "settle_frames is capped at {}", kMaxSettleFrames);
				return false;
			}

			if (ImGuiContext* const ctx = ImGui::GetCurrentContext())
				ctx->Time = kUiClockEpoch;

			for (u32 i = 0; i < settle_frames; i++)
			{
				GSPresentCurrentFrame();
				g_gs_device->ThrottlePresentation();
			}
			return GSHeadless::CaptureFreshFrameToPNG(path, width, height, error);
		}

		std::string Screenshot(const std::string& path, u32 settle_frames)
		{
			Error error;
			u32 width = 0, height = 0;
			bool saved = false;
			if (!RunOnGSThreadAndWait([&]() { saved = SettleAndCapture(path, settle_frames, &width, &height, &error); }))
				return Failure("the GS is not running");

			if (!saved)
				return Failure(error.GetDescription());
			return fmt::format("{{\"ok\":true,\"path\":\"{}\",\"width\":{},\"height\":{}}}", JsonEscape(path), width, height);
		}

		std::string WaitFrames(u32 frames, u32 timeout_ms)
		{
			if (!GSHeadless::IsEnabled())
				return Failure("headless present is not enabled (start the host with --headless WxH)");

			if (timeout_ms == 0)
				timeout_ms = 30000;

			const u64 target = GSHeadless::PresentCount() + frames;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
			bool timed_out = false;
			while (GSHeadless::PresentCount() < target)
			{
				if (std::chrono::steady_clock::now() >= deadline)
				{
					timed_out = true;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}

			return fmt::format("{{\"ok\":{},\"timed_out\":{},\"present_count\":{}}}", timed_out ? "false" : "true",
				timed_out ? "true" : "false", GSHeadless::PresentCount());
		}
	} // namespace

	bool HandlePineCommand(u8 opcode, const u8* args, u32 avail, u32* consumed, std::string* reply)
	{
		const u8* cursor = args;
		u32 remaining = avail;

		switch (opcode)
		{
			case OpScreenshot:
			{
				std::string path;
				u32 settle_frames;
				if (!ReadString(cursor, remaining, &path) || !ReadU32(cursor, remaining, &settle_frames))
					return false;
				*reply = Screenshot(path, settle_frames);
				break;
			}
			case OpOpenPage:
			{
				std::string name;
				if (!ReadString(cursor, remaining, &name))
					return false;
				*reply = OpenPage(name);
				break;
			}
			case OpWaitFrames:
			{
				u32 frames, timeout_ms;
				if (!ReadU32(cursor, remaining, &frames) || !ReadU32(cursor, remaining, &timeout_ms))
					return false;
				*reply = WaitFrames(frames, timeout_ms);
				break;
			}
			default:
				return false;
		}

		*consumed = static_cast<u32>(cursor - args);
		return true;
	}
} // namespace FullscreenUIShot
