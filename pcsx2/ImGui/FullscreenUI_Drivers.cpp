// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture's "GPU Drivers" settings page: choose the Vulkan driver (the system's or an installed
// driver pack), download and delete packs, and restart so the choice takes effect. The work is done
// by Catalog/DriverPacks.h; this file is the page and the threads that call it.
//
// How a choice takes effect: DriverPacks::ApplyEnvironment() reads the selected pack once, early in
// main(), so a changed selection is used the next time ARMSX2 starts. The page therefore shows two
// things: what this run is using (DriverPacks::GetLastApplyResult) and what is selected now, and
// offers "Restart Now" when they differ.
//
// Threading: everything here that touches FullscreenUI state runs on the GS thread. The feed listing
// and the install each run on a thread of their own and only fill s_handoff, then ask the GS thread
// (through the CPU thread, which owns the ring) to read it.
//
// The settings lock is held while a settings page draws, so the page body reads and writes the
// selection through the SettingsInterface it is given and never through DriverPacks::GetSelectedPack
// or SetSelectedPack, which take the lock themselves. Dialog callbacks run with the lock released.

#include "Catalog/CancelToken.h"
#include "Catalog/DriverPacks.h"
#include "Catalog/Fetch.h"

#include "Achievements.h"
#include "Config.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/GS.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "ImGui/FullscreenUI_Internal.h"
#include "ImGui/FullscreenUI_PageDwell.h"
#include "ImGui/ImGuiFullscreen.h"
#include "MTGS.h"
#include "VMManager.h"

#include "common/SettingsInterface.h"
#include "common/WindowInfo.h"

#include "IconsFontAwesome.h"

#include "fmt/format.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace DP = DriverPacks;

namespace
{
	using namespace FullscreenUI;

	constexpr const char* INSTALL_NOTIFICATION_KEY = "driver_install";

	// What the worker threads hand to the GS thread. A callback adds to it and queues one drain; the
	// drain empties it. At most one drain is queued at a time, so a download reporting progress for
	// every chunk costs the GS thread one look per CPU-thread tick.
	struct Handoff
	{
		std::mutex mutex;
		bool drain_queued = false;
		std::optional<DP::RemoteListing> listing;
		std::optional<DP::InstallProgress> progress;
		std::optional<DP::InstallResult> finished;
	};
	Handoff s_handoff;

	enum class FeedState
	{
		NotLoaded,
		Loading,
		Loaded,
	};

	// Page state, GS thread only.
	struct PageState
	{
		~PageState()
		{
			// Normally ShutdownDriversPage() has joined already; this only keeps a joinable thread from
			// reaching std::terminate if the process ends some other way.
			feed_cancel.Cancel();
			install_cancel.Cancel();
			if (feed_thread.joinable())
				feed_thread.join();
			if (install_thread.joinable())
				install_thread.join();
		}

		std::unique_ptr<DP::Manager> manager;
		int last_frame = -100; ///< ImGui frame the page was last drawn in, to notice it being opened
		PageDwell dwell; ///< the feed is asked for once the page has stayed open
		DP::Availability availability;
		std::vector<DP::InstalledDriver> installed;
		bool installed_known = false; ///< `installed` has been read at least once (ShowDriversSettingsPage needs it before the page opens)

		FeedState feed = FeedState::NotLoaded;
		DP::RemoteListing listing;
		Catalog::CancelToken feed_cancel;
		std::thread feed_thread;

		bool installing = false;
		std::string install_id;
		std::string install_tag;
		DP::InstallProgress progress;
		Catalog::CancelToken install_cancel;
		std::thread install_thread;

		// The last install that did not finish, kept on the page because a notification goes away.
		std::string failure_name;
		std::string failure_message;

		// First-run setup: the install in flight was started by its GPU Driver step, and the pack that step
		// installed. Setup selects it when it finishes, since a pack loads at the next start.
		bool install_from_setup = false;
		std::string setup_installed_id;
		std::string setup_installed_tag;
		int setup_last_frame = -100;
	};
	PageState s_state;

	DP::Manager& GetManager()
	{
		if (!s_state.manager)
			s_state.manager = std::make_unique<DP::Manager>(Catalog::GetDefaultFetcher(), DP::ManagerConfig::ForUser());
		return *s_state.manager;
	}

	// ---- small helpers ----------------------------------------------------------------------------

	std::string Trimmed(std::string value)
	{
		const size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos)
			return {};
		const size_t last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	// Same value DriverPacks::GetSelectedPack() returns, read from the settings the page was given.
	std::string SelectedPack(const SettingsInterface& bsi)
	{
		return Trimmed(bsi.GetStringValue(DP::SETTING_SECTION, DP::SETTING_KEY, ""));
	}

	void WriteSelection(SettingsInterface& bsi, const std::string& id)
	{
		if (id.empty())
			bsi.DeleteValue(DP::SETTING_SECTION, DP::SETTING_KEY);
		else
			bsi.SetStringValue(DP::SETTING_SECTION, DP::SETTING_KEY, id.c_str());
	}

	// Two '#' in a row start a hidden ImGui ID in a label; a release title could contain them.
	std::string NoHash(std::string text)
	{
		for (size_t pos = text.find("##"); pos != std::string::npos; pos = text.find("##", pos + 2))
			text.insert(pos + 1, " ");
		return text;
	}

	std::string FormatSize(u64 bytes)
	{
		if (bytes >= (1ull << 30))
			return fmt::format("{:.1f} GB", static_cast<double>(bytes) / static_cast<double>(1ull << 30));
		return fmt::format("{:.1f} MB", static_cast<double>(bytes) / static_cast<double>(1ull << 20));
	}

	// "2026-10-04T23:54:03Z" -> "2026-10-04"; anything else is shown as sent.
	std::string FormatDate(const std::string& iso)
	{
		if (iso.size() >= 10 && iso[4] == '-' && iso[7] == '-')
			return iso.substr(0, 10);
		return iso;
	}

	// DP::InstalledDriver::platforms is the window systems the build supports, as the pack names
	// them ("wayland"). A pack that lists Wayland and no X11 cannot present on an X11 session.
	bool PackIsWaylandOnly(const std::string& platforms)
	{
		bool wayland = false, x11 = false;
		std::string token;
		const auto flush = [&]() {
			if (token == "wayland")
				wayland = true;
			else if (token == "x11" || token == "xcb" || token == "xlib")
				x11 = true;
			token.clear();
		};
		for (const char c : platforms)
		{
			if (std::isalnum(static_cast<unsigned char>(c)))
				token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			else
				flush();
		}
		flush();
		return wayland && !x11;
	}

	// The window system the GS was given, which is how the GS and window code tell sessions apart
	// (WindowInfo::Type). Under the Vulkan display path or Wayland the answer is no.
	bool SessionIsX11()
	{
		return g_gs_device && g_gs_device->GetWindowInfo().type == WindowInfo::Type::X11;
	}

	bool PackIsUnusableHere(const DP::InstalledDriver& pack)
	{
		return PackIsWaylandOnly(pack.platforms) && SessionIsX11();
	}

	const DP::InstalledDriver* FindInstalled(const std::string& id)
	{
		for (const DP::InstalledDriver& pack : s_state.installed)
		{
			if (pack.id == id)
				return &pack;
		}
		return nullptr;
	}

	void RefreshInstalled()
	{
		s_state.installed = GetManager().ListInstalled();
		s_state.installed_known = true;
	}

	// ---- what this run uses versus what is selected -------------------------------------------------

	struct Status
	{
		enum class Kind
		{
			System, ///< the system driver
			Pack, ///< a pack is in use
			Launcher, ///< the launcher set VK_DRIVER_FILES / VK_ICD_FILENAMES; the selection is ignored
			Missing, ///< a pack was selected at start but is not installed
			NoGpu, ///< a pack was selected at start but this machine has no Adreno
		};

		Kind kind = Kind::System;
		std::string in_use_id; ///< the pack this run uses, empty for the system driver
		bool restart_needed = false; ///< what is selected now would be used after a restart
	};

	bool EnvironmentSetsDriver()
	{
		const auto is_set = [](const char* name) {
			const char* value = std::getenv(name);
			return value && *value;
		};
		return is_set("VK_DRIVER_FILES") || is_set("VK_ICD_FILENAMES");
	}

	Status ComputeStatus(const DP::ApplyResult& apply, const std::string& selected)
	{
		Status status;
		switch (apply.outcome)
		{
			case DP::ApplyOutcome::Applied:
				status.kind = Status::Kind::Pack;
				status.in_use_id = apply.pack_id;
				break;
			case DP::ApplyOutcome::EnvironmentAlreadySet:
				status.kind = Status::Kind::Launcher;
				break;
			case DP::ApplyOutcome::PackMissing:
				status.kind = Status::Kind::Missing;
				break;
			case DP::ApplyOutcome::GpuNotSupported:
				status.kind = Status::Kind::NoGpu;
				break;
			case DP::ApplyOutcome::SystemDriver:
			case DP::ApplyOutcome::Unsupported:
				// With nothing selected, ApplyEnvironment() does not look at the environment, but a launcher
				// that names a driver (ROCKNIX's does) is in charge all the same. A pack this process applied
				// has the Applied outcome, so the variables are not ours here.
				if (EnvironmentSetsDriver())
					status.kind = Status::Kind::Launcher;
				break;
		}

		// A launcher's choice wins whatever is selected here, and without an Adreno there is nothing
		// to switch to. Otherwise a selection that differs from this run's driver needs a restart,
		// unless it names a pack that is not installed (a restart would not change anything).
		if (status.kind != Status::Kind::Launcher && status.kind != Status::Kind::NoGpu && selected != status.in_use_id)
			status.restart_needed = selected.empty() || FindInstalled(selected) != nullptr;
		return status;
	}

	std::string PackLabel(const std::string& id)
	{
		const DP::InstalledDriver* pack = FindInstalled(id);
		return pack ? pack->name : id;
	}

	std::string StatusSummary(const Status& status, const DP::ApplyResult& apply)
	{
		switch (status.kind)
		{
			case Status::Kind::Pack:
				return fmt::format(FSUI_FSTR("Using the driver pack \"{}\"."), PackLabel(status.in_use_id));
			case Status::Kind::Launcher:
				return FSUI_STR("Your launcher sets the Vulkan driver itself, so the choice below has no effect.");
			case Status::Kind::Missing:
				return fmt::format(FSUI_FSTR("The selected driver pack \"{}\" is not installed. The system driver is in use."), apply.pack_id);
			case Status::Kind::NoGpu:
				return FSUI_STR("A driver pack is selected, but this device has no Adreno GPU. The system driver is in use.");
			case Status::Kind::System:
				break;
		}
		return FSUI_STR("Using the system driver.");
	}

	std::string StatusValue(const Status& status)
	{
		switch (status.kind)
		{
			case Status::Kind::Pack:
			{
				const DP::InstalledDriver* pack = FindInstalled(status.in_use_id);
				return (pack && !pack->build_tag.empty()) ? pack->build_tag : status.in_use_id;
			}
			case Status::Kind::Launcher:
				return FSUI_STR("Set by Launcher");
			default:
				break;
		}
		return FSUI_STR("System Driver");
	}

	// ---- worker threads -> GS thread -----------------------------------------------------------------

	// The workers are not the CPU thread, and a push into the GS ring is only allowed from the CPU
	// thread (MTGS::RunOnGSThread), so hop through it.
	void PostToGSThread(std::function<void()> function)
	{
		Host::RunOnCPUThread([function = std::move(function)]() mutable {
			if (MTGS::IsOpen())
				MTGS::RunOnGSThread(std::move(function));
		});
	}

	void ShowInstallNotice(const std::string& title, const std::string& text, float seconds)
	{
		ImGuiFullscreen::AddNotification(INSTALL_NOTIFICATION_KEY, seconds, fmt::format("{} {}", ICON_FA_DOWNLOAD, title), text, std::string());
	}

	std::string ProgressText(const DP::InstallProgress& progress)
	{
		switch (progress.stage)
		{
			case DP::InstallStage::Downloading:
				if (progress.total > 0)
				{
					return fmt::format(FSUI_FSTR("Downloading: {} of {} ({}%)"), FormatSize(progress.done), FormatSize(progress.total),
						std::min<u64>(100, progress.done * 100 / progress.total));
				}
				return fmt::format(FSUI_FSTR("Downloading: {}"), FormatSize(progress.done));
			case DP::InstallStage::Verifying:
				return FSUI_STR("Checking the download...");
			case DP::InstallStage::Extracting:
				return FSUI_STR("Unpacking...");
			case DP::InstallStage::Checking:
				return FSUI_STR("Checking that the driver loads...");
			case DP::InstallStage::Finishing:
				return FSUI_STR("Finishing...");
			case DP::InstallStage::Preparing:
				break;
		}
		return FSUI_STR("Starting...");
	}

	void ShowInstallProgress()
	{
		// Long enough to outlast a slow chunk; the next update or the end-of-install message replaces it.
		ShowInstallNotice(fmt::format(FSUI_FSTR("Installing {}"), s_state.install_tag), ProgressText(s_state.progress), 10.0f);
	}

	// GS thread: applies what the workers reported since the last time. The page also calls this when
	// it draws, so a result that arrived while Big Picture was closed is not lost; the notices need the
	// UI, the rest does not.
	void DrainHandoff()
	{
		std::optional<DP::RemoteListing> listing;
		std::optional<DP::InstallProgress> progress;
		std::optional<DP::InstallResult> finished;
		{
			std::lock_guard<std::mutex> lock(s_handoff.mutex);
			s_handoff.drain_queued = false;
			listing.swap(s_handoff.listing);
			progress.swap(s_handoff.progress);
			finished.swap(s_handoff.finished);
		}

		const bool ui_up = IsInitialized();

		if (listing.has_value())
		{
			s_state.listing = std::move(*listing);
			s_state.feed = FeedState::Loaded;
		}

		if (finished.has_value())
		{
			const std::string tag = s_state.install_tag;
			s_state.installing = false;
			RefreshInstalled();

			if (finished->Ok())
			{
				s_state.failure_message.clear();
				if (s_state.install_from_setup)
				{
					s_state.setup_installed_id = finished->id;
					s_state.setup_installed_tag = tag;
				}
				if (ui_up)
				{
					ShowInstallNotice(fmt::format(FSUI_FSTR("Installed {}"), tag),
						s_state.install_from_setup ? FSUI_STR("ARMSX2 uses it after it restarts, when you finish setup.") :
													 FSUI_STR("Choose it on the GPU Drivers page to use it."),
						6.0f);
				}
			}
			else if (finished->Cancelled())
			{
				s_state.failure_message.clear();
				if (ui_up)
					ShowInstallNotice(fmt::format(FSUI_FSTR("Cancelled {}"), tag), FSUI_STR("The install was cancelled. Nothing was changed."), 4.0f);
			}
			else
			{
				// The service writes its message for display ("Checksum mismatch: ...", "libwayland-client.so.0:
				// cannot open shared object file"); show it as it is.
				s_state.failure_name = tag;
				s_state.failure_message = finished->message.empty() ? std::string(FSUI_STR("The install failed.")) : finished->message;
				if (ui_up)
					ShowInstallNotice(fmt::format(FSUI_FSTR("Could not install {}"), tag), s_state.failure_message, 12.0f);
			}
		}
		else if (progress.has_value() && s_state.installing)
		{
			s_state.progress = *progress;
			if (ui_up)
				ShowInstallProgress();
		}
	}

	void QueueDrain()
	{
		{
			std::lock_guard<std::mutex> lock(s_handoff.mutex);
			if (s_handoff.drain_queued)
				return;
			s_handoff.drain_queued = true;
		}
		PostToGSThread(&DrainHandoff);
	}

	void StartFeedFetch(bool force_refresh)
	{
		if (s_state.feed == FeedState::Loading)
			return;
		if (s_state.feed_thread.joinable())
			s_state.feed_thread.join();

		s_state.feed = FeedState::Loading;
		s_state.feed_cancel = Catalog::CancelToken();
		s_state.feed_thread = std::thread([manager = &GetManager(), cancel = s_state.feed_cancel, force_refresh]() {
			DP::RemoteListing listing = manager->ListAvailable(cancel, force_refresh);
			if (cancel.IsCancelled())
				return;
			{
				std::lock_guard<std::mutex> lock(s_handoff.mutex);
				s_handoff.listing = std::move(listing);
			}
			QueueDrain();
		});
	}

	void StartInstall(const DP::RemoteDriver& driver, bool from_setup = false)
	{
		if (s_state.installing)
		{
			ShowToast(ICON_FA_DOWNLOAD, FSUI_STR("Another driver is being installed."), 5.0f);
			return;
		}
		if (s_state.install_thread.joinable())
			s_state.install_thread.join();

		s_state.install_from_setup = from_setup;
		s_state.installing = true;
		s_state.install_id = driver.id;
		s_state.install_tag = driver.tag.empty() ? driver.id : driver.tag;
		s_state.progress = DP::InstallProgress();
		s_state.failure_message.clear();
		s_state.install_cancel = Catalog::CancelToken();
		ShowInstallProgress();

		s_state.install_thread = std::thread([manager = &GetManager(), driver, cancel = s_state.install_cancel]() {
			const DP::InstallResult result = manager->Install(
				driver,
				[](const DP::InstallProgress& progress) {
					{
						std::lock_guard<std::mutex> lock(s_handoff.mutex);
						s_handoff.progress = progress;
					}
					QueueDrain();
				},
				cancel);
			{
				std::lock_guard<std::mutex> lock(s_handoff.mutex);
				s_handoff.finished = result;
			}
			QueueDrain();
		});
	}

	// ---- actions ---------------------------------------------------------------------------------------

	void OnPageEntered()
	{
		s_state.availability = DP::GetAvailability();
		RefreshInstalled();
	}

	void DoRestartNow()
	{
		// RelaunchAtExit arms a restart that runs when the process ends and cannot be taken back, so
		// the exit is requested without a prompt the user could cancel: the confirmation, where one
		// is needed, has been asked already.
		std::string error;
		if (!DP::RelaunchAtExit(&error))
		{
			OpenInfoMessageDialog(FSUI_STR("Cannot Restart"), error.empty() ? std::string(FSUI_STR("ARMSX2 could not prepare to restart.")) : error);
			return;
		}
		Host::RunOnCPUThread([]() { Host::RequestExitApplication(false); });
	}

	void RequestRestart()
	{
		const bool game = VMManager::HasValidVM();
		if (!game && !s_state.installing)
		{
			DoRestartNow();
			return;
		}

		std::string message;
		if (game)
			message += FSUI_STR("A game is running. Restarting closes it, and anything you have not saved is lost.");
		if (s_state.installing)
		{
			if (!message.empty())
				message += "\n\n";
			message += FSUI_STR("A driver is still being installed. Restarting cancels it.");
		}
		message += "\n\n";
		message += FSUI_STR("Restart now?");

		OpenConfirmMessageDialog(FSUI_STR("Restart ARMSX2?"), std::move(message), [](bool yes) {
			if (yes)
				DoRestartNow();
		}, false);
	}

	// A selection that goes through a dialog (so the settings lock is free to take).
	void CommitSelection(SettingsInterface* bsi, const std::string& id)
	{
		auto lock = Host::GetSettingsLock();
		WriteSelection(*bsi, id);
		SetSettingsChanged(bsi);
	}

	void ChoosePack(SettingsInterface* bsi, const DP::InstalledDriver& pack)
	{
		if (PackIsUnusableHere(pack))
		{
			OpenConfirmMessageDialog(FSUI_STR("Wayland Only"),
				FSUI_STR("This driver pack was built for Wayland sessions only, and ARMSX2 is running in an X11 session. "
						 "It will probably not be able to show a picture.\n\nUse it anyway?"),
				[bsi, id = pack.id](bool yes) {
					if (yes)
						CommitSelection(bsi, id);
				},
				false);
			return;
		}

		// Called from the page body, where the settings lock is already held.
		WriteSelection(*bsi, pack.id);
		SetSettingsChanged(bsi);
	}

	void ConfirmDelete(SettingsInterface* bsi, const DP::InstalledDriver& pack)
	{
		OpenConfirmMessageDialog(FSUI_STR("Delete Driver Pack?"),
			fmt::format(FSUI_FSTR("Delete \"{}\" from this device? You can download it again later."), pack.name),
			[bsi, id = pack.id, name = pack.name](bool yes) {
				if (!yes)
					return;

				std::string error;
				if (!GetManager().Delete(id, &error))
				{
					RefreshInstalled();
					OpenInfoMessageDialog(FSUI_STR("Cannot Delete"), error.empty() ? std::string(FSUI_STR("The driver pack could not be deleted.")) : error);
					return;
				}

				// Delete leaves the setting alone; a selection that names a pack that is gone would only
				// fall back to the system driver at the next start, with a log line nobody reads.
				{
					auto lock = Host::GetSettingsLock();
					if (SelectedPack(*bsi) == id)
					{
						WriteSelection(*bsi, std::string());
						SetSettingsChanged(bsi);
					}
				}
				RefreshInstalled();
				ShowToast(ICON_FA_TRASH, fmt::format(FSUI_FSTR("Deleted {}."), name));
			},
			false);
	}

	// ---- the page --------------------------------------------------------------------------------------

	std::string DescribePack(const DP::InstalledDriver& pack)
	{
		std::string text;
		const auto add = [&](const std::string& part) {
			if (part.empty())
				return;
			if (!text.empty())
				text += "  |  ";
			text += part;
		};
		add(pack.version);
		if (!pack.mesa_version.empty())
			add(fmt::format("Mesa {}", pack.mesa_version));
		add(pack.feed_label);
		if (PackIsWaylandOnly(pack.platforms))
			add(PackIsUnusableHere(pack) ? FSUI_STR("Wayland only; this is an X11 session") : FSUI_STR("Wayland only"));
		return text.empty() ? std::string(FSUI_STR("Installed")) : text;
	}

	void DrawNotAvailable(const DP::Availability& availability)
	{
		MenuHeading(FSUI_CSTR("GPU Drivers"));

		// "No driver packs for this GPU." is the service's own message for a machine without an Adreno.
		if (availability.reason == DP::Availability::Reason::NoMatchingGpu)
		{
			if (MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "No driver packs for this GPU."),
					FSUI_CSTR("Driver packs are Turnip builds for Qualcomm Adreno GPUs. This device uses its system driver.")))
			{
				OpenInfoMessageDialog(FSUI_STR("GPU Drivers"),
					FSUI_STR("Driver packs are Turnip builds for Qualcomm Adreno GPUs. This device has no Adreno GPU, so it keeps using its system driver."));
			}
			return;
		}

		if (MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "Driver packs are not available on this platform."),
				FSUI_CSTR("Driver packs are only offered on Linux.")))
		{
			OpenInfoMessageDialog(FSUI_STR("GPU Drivers"), FSUI_STR("Driver packs are only offered on Linux."));
		}
	}

	void DrawStatusSection(const Status& status, const DP::ApplyResult& apply, const std::string& selected)
	{
		MenuHeading(FSUI_CSTR("Current Driver"));

		const std::string summary = StatusSummary(status, apply);
		const std::string value = StatusValue(status);
		if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_MICROCHIP, "Driver In Use"), summary.c_str(), value.c_str()))
		{
			std::string details = summary;
			if (status.kind == Status::Kind::Pack)
			{
				if (const DP::InstalledDriver* pack = FindInstalled(status.in_use_id))
				{
					if (!pack->version.empty())
						details += fmt::format(FSUI_FSTR("\n\nVersion: {}"), pack->version);
					if (!pack->mesa_version.empty())
						details += fmt::format(FSUI_FSTR("\nMesa: {}"), pack->mesa_version);
					if (!pack->driver_info_token.empty())
					{
						details += fmt::format(
							FSUI_FSTR("\n\nWhen this pack is loaded, the GS log's \"VK: GPU\" line ends with {}."), pack->driver_info_token);
					}
				}
			}
			OpenInfoMessageDialog(FSUI_STR("Driver In Use"), std::move(details));
		}

		if (status.restart_needed)
		{
			const bool game = VMManager::HasValidVM();
			if (MenuButton(FSUI_ICONSTR(ICON_FA_ARROWS_ROTATE, "Restart Now"),
					game ? FSUI_CSTR("The driver you chose applies the next time ARMSX2 starts. Restarting closes ARMSX2 and the running game, then opens it again.") :
						   FSUI_CSTR("The driver you chose applies the next time ARMSX2 starts. Restart now to use it; ARMSX2 opens again with the same options.")))
			{
				RequestRestart();
			}
		}

		// The pack that will be used is built for a window system this session is not.
		if (const DP::InstalledDriver* chosen = FindInstalled(selected); chosen && PackIsUnusableHere(*chosen))
		{
			if (MenuButton(FSUI_ICONSTR(ICON_FA_TRIANGLE_EXCLAMATION, "Wayland Only"),
					FSUI_CSTR("The selected pack was built for Wayland sessions only, and this is an X11 session. Choose another driver.")))
			{
				OpenInfoMessageDialog(FSUI_STR("Wayland Only"),
					FSUI_STR("The selected driver pack was built for Wayland sessions only, and ARMSX2 is running in an X11 session. "
							 "It will probably not be able to show a picture. Choose the system driver or another pack, or start ARMSX2 in a Wayland session."));
			}
		}

		if (s_state.installing)
		{
			const std::string progress = fmt::format(FSUI_FSTR("Installing {}: {}"), s_state.install_tag, ProgressText(s_state.progress));
			if (MenuButton(FSUI_ICONSTR(ICON_FA_XMARK, "Cancel Install"), progress.c_str()))
				s_state.install_cancel.Cancel();
		}
		else if (!s_state.failure_message.empty())
		{
			const std::string summary = fmt::format("{}: {}", s_state.failure_name, s_state.failure_message);
			if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_TRIANGLE_EXCLAMATION, "Install Failed"), summary.c_str(), FSUI_CSTR("Dismiss")))
				s_state.failure_message.clear();
		}
	}

	void DrawChoiceSection(SettingsInterface* bsi, const std::string& selected)
	{
		MenuHeading(FSUI_CSTR("Choose Driver"));

		const bool system_chosen = selected.empty();
		{
			const std::string title = fmt::format("{} {}###drv_system", system_chosen ? ICON_FA_CIRCLE_DOT : ICON_FA_CIRCLE, FSUI_STR("System Driver"));
			if (MenuButtonWithValue(title.c_str(), FSUI_CSTR("The Vulkan driver that came with this device. A change applies the next time ARMSX2 starts."),
					system_chosen ? FSUI_CSTR("Selected") : "") &&
				!system_chosen)
			{
				WriteSelection(*bsi, std::string());
				SetSettingsChanged(bsi);
			}
		}

		for (const DP::InstalledDriver& pack : s_state.installed)
		{
			const bool chosen = (selected == pack.id);
			const std::string title = fmt::format("{} {}###drv_use_{}", chosen ? ICON_FA_CIRCLE_DOT : ICON_FA_CIRCLE, NoHash(pack.name), pack.id);
			const std::string summary = DescribePack(pack);
			if (MenuButtonWithValue(title.c_str(), summary.c_str(), chosen ? FSUI_CSTR("Selected") : "") && !chosen)
				ChoosePack(bsi, pack);
		}

		// A selection that names no installed pack (an edited settings file, or a pack removed by hand).
		if (!selected.empty() && !FindInstalled(selected))
		{
			const std::string title = fmt::format("{} {}###drv_missing", ICON_FA_CIRCLE_DOT, NoHash(selected));
			if (MenuButtonWithValue(title.c_str(), FSUI_CSTR("This driver pack is not installed. Select the system driver or install the pack."),
					FSUI_CSTR("Not Installed")))
			{
				OpenInfoMessageDialog(FSUI_STR("Not Installed"),
					fmt::format(FSUI_FSTR("The selected driver pack \"{}\" is not installed, so ARMSX2 uses the system driver."), selected));
			}
		}
	}

	void DrawAvailableSection()
	{
		MenuHeading(FSUI_CSTR("Available Driver Packs"));

		if (s_state.feed == FeedState::NotLoaded || s_state.feed == FeedState::Loading)
		{
			MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_NOTCH, "Loading driver list..."), FSUI_CSTR("Asking GitHub which driver packs are available."));
			return;
		}

		for (const DP::FeedError& error : s_state.listing.errors)
		{
			const std::string title = fmt::format("{} {}###drv_feed_error_{}", ICON_FA_TRIANGLE_EXCLAMATION,
				fmt::format(FSUI_FSTR("Could not load the {} list"), error.feed_label), error.feed_label);
			// The service words the reason for display ("GitHub is limiting requests from this network; ...").
			if (MenuButtonWithValue(title.c_str(), error.message.c_str(), FSUI_CSTR("Try Again")))
				StartFeedFetch(true);
		}

		if (s_state.listing.used_stale_cache)
		{
			MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "Showing the last saved list"),
				FSUI_CSTR("GitHub could not be reached, so this list may be out of date."));
		}

		if (s_state.listing.drivers.empty() && s_state.listing.errors.empty())
			MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "No driver packs found"), FSUI_CSTR("The feed has no driver packs for this device right now."));

		for (const DP::RemoteDriver& driver : s_state.listing.drivers)
		{
			const bool installed = FindInstalled(driver.id) != nullptr;
			const bool this_one = s_state.installing && s_state.install_id == driver.id;

			std::string summary = driver.tag;
			const auto add = [&](const std::string& part) {
				if (part.empty())
					return;
				if (!summary.empty())
					summary += "  |  ";
				summary += part;
			};
			add(FormatDate(driver.published_at));
			if (driver.size > 0)
				add(FormatSize(driver.size));
			if (driver.prerelease)
				add(FSUI_STR("Pre-release"));

			std::string value;
			if (this_one)
			{
				value = (s_state.progress.stage == DP::InstallStage::Downloading && s_state.progress.total > 0) ?
				            fmt::format("{}%", std::min<u64>(100, s_state.progress.done * 100 / s_state.progress.total)) :
				            std::string(FSUI_STR("Installing"));
			}
			else
				value = installed ? FSUI_STR("Installed") : FSUI_STR("Download");

			const std::string title = fmt::format("{} {}###drv_get_{}", installed ? ICON_FA_CIRCLE_CHECK : ICON_FA_CLOUD_ARROW_DOWN,
				NoHash(driver.release_name.empty() ? driver.tag : driver.release_name), driver.id);
			if (MenuButtonWithValue(title.c_str(), summary.c_str(), value.c_str()))
			{
				if (installed)
					ShowToast(ICON_FA_CIRCLE_CHECK, fmt::format(FSUI_FSTR("{} is already installed."), driver.tag), 4.0f);
				else if (!this_one)
					StartInstall(driver);
			}
		}

		// A feed that failed already has "Try Again".
		if (s_state.listing.errors.empty() &&
			MenuButton(FSUI_ICONSTR(ICON_FA_ARROWS_ROTATE, "Refresh List"), FSUI_CSTR("Asks GitHub for the current list of driver packs.")))
		{
			StartFeedFetch(true);
		}
	}

	void DrawInstalledSection(SettingsInterface* bsi)
	{
		if (s_state.installed.empty())
			return;

		MenuHeading(FSUI_CSTR("Installed Driver Packs"));

		for (const DP::InstalledDriver& pack : s_state.installed)
		{
			// The tag keeps the title short; the pack's full name goes in the summary.
			const std::string title = fmt::format("{} {}###drv_delete_{}", ICON_FA_TRASH,
				fmt::format(FSUI_FSTR("Delete {}"), NoHash(pack.version.empty() ? pack.id : pack.version)), pack.id);
			const std::string summary = fmt::format(FSUI_FSTR("Removes \"{}\" from this device. If it is selected, the system driver is used instead."), pack.name);
			if (MenuButton(title.c_str(), summary.c_str()))
				ConfirmDelete(bsi, pack);
		}
	}
} // namespace

bool FullscreenUI::ShowDriversSettingsPage()
{
	// Driver packs exist only on desktop Linux, which is where CurrentPlatform() is not empty.
	static const bool s_supported = !DP::CurrentPlatform().empty();
	if (!s_supported)
		return false;

	// The page is for a machine with an Adreno GPU. Elsewhere it stays out of the list unless a pack is
	// installed or selected, which the user may want to undo. The GPU does not change while we run.
	static const bool s_available = DP::GetAvailability().IsAvailable();
	if (s_available)
		return true;

	if (!s_state.installed_known)
		RefreshInstalled();
	return !s_state.installed.empty() || !DP::GetSelectedPack().empty();
}

void FullscreenUI::DrawDriversSettingsPage(SettingsInterface* bsi)
{
	// The page is drawn every frame it is open, so a gap in the frame count means it was opened again.
	const int frame = ImGui::GetFrameCount();
	if (frame - s_state.last_frame > 1)
		OnPageEntered();
	s_state.last_frame = frame;
	// The list comes from GitHub: not while the D-pad is only passing the page (its API allows 60 requests an hour).
	if (s_state.dwell.Settled() && s_state.availability.IsAvailable() && s_state.feed == FeedState::NotLoaded)
		StartFeedFetch(false);
	DrainHandoff();

	BeginMenuButtons();

	const bool available = s_state.availability.IsAvailable();
	const std::string selected = SelectedPack(*bsi);
	if (!available)
	{
		DrawNotAvailable(s_state.availability);
		// Listed only because a pack is installed or selected: keep it possible to undo the choice and delete it.
		if (selected.empty() && s_state.installed.empty())
		{
			EndMenuButtons();
			return;
		}
	}

	const DP::ApplyResult apply = DP::GetLastApplyResult();
	const Status status = ComputeStatus(apply, selected);

	DrawStatusSection(status, apply, selected);
	DrawChoiceSection(bsi, selected);
	if (available)
		DrawAvailableSection();
	DrawInstalledSection(bsi);

	EndMenuButtons();
}

void FullscreenUI::ShutdownDriversPage()
{
	// Stop the workers first, so nothing is added to the hand-off after it is cleared.
	s_state.feed_cancel.Cancel();
	s_state.install_cancel.Cancel();
	if (s_state.feed_thread.joinable())
		s_state.feed_thread.join();
	if (s_state.install_thread.joinable())
		s_state.install_thread.join();

	{
		std::lock_guard<std::mutex> lock(s_handoff.mutex);
		s_handoff.drain_queued = false;
		s_handoff.listing.reset();
		s_handoff.progress.reset();
		s_handoff.finished.reset();
	}

	s_state.manager.reset();
	s_state.last_frame = -100;
	s_state.dwell.Reset();
	s_state.availability = DP::Availability();
	s_state.installed.clear();
	s_state.installed_known = false;
	s_state.feed = FeedState::NotLoaded;
	s_state.listing = DP::RemoteListing();
	s_state.installing = false;
	s_state.install_id.clear();
	s_state.install_tag.clear();
	s_state.failure_name.clear();
	s_state.failure_message.clear();
	s_state.install_from_setup = false;
	s_state.setup_installed_id.clear();
	s_state.setup_installed_tag.clear();
	s_state.setup_last_frame = -100;
}

// ---- first-run setup ---------------------------------------------------------------------------------

bool FullscreenUI::ShouldOfferDriverInSetup()
{
	return DP::ShouldOfferInSetup(DP::GetSetupContext(GetManager(), SessionIsX11()));
}

bool FullscreenUI::IsDriverSetupInstalling()
{
	return s_state.installing && s_state.install_from_setup;
}

bool FullscreenUI::IsDriverSetupInstalled()
{
	return !s_state.setup_installed_id.empty();
}

bool FullscreenUI::FinishDriverSetup()
{
	if (s_state.setup_installed_id.empty())
		return false;
	DP::SetSelectedPack(s_state.setup_installed_id);
	return true;
}

void FullscreenUI::RestartForDriverSetup()
{
	DoRestartNow();
}

void FullscreenUI::DrawDriverSetupStep()
{
	// Drawn every frame the step is open, so a gap in the frame count means it was opened again.
	const int frame = ImGui::GetFrameCount();
	if (frame - s_state.setup_last_frame > 1)
		OnPageEntered();
	s_state.setup_last_frame = frame;
	DrainHandoff();
	if (s_state.feed == FeedState::NotLoaded)
		StartFeedFetch(false);

	BeginMenuButtons();
	MenuHeading(FSUI_CSTR("GPU Driver"));
	ActiveButton(FSUI_CSTR("ARMSX2 has its own Vulkan driver for your GPU. It is the driver ARMSX2 is tuned for."), false, false,
		LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);

	if (!s_state.setup_installed_id.empty())
	{
		const std::string title = fmt::format("{} {}###drv_setup_done", ICON_FA_CIRCLE_CHECK, fmt::format(FSUI_FSTR("Installed {}"), s_state.setup_installed_tag));
		MenuButton(title.c_str(), FSUI_CSTR("ARMSX2 restarts when you finish setup so the driver can load."));
	}
	else if (s_state.installing && s_state.install_from_setup)
	{
		const std::string progress = fmt::format(FSUI_FSTR("Installing {}: {}"), s_state.install_tag, ProgressText(s_state.progress));
		if (MenuButton(FSUI_ICONSTR(ICON_FA_XMARK, "Cancel Install"), progress.c_str()))
			s_state.install_cancel.Cancel();
	}
	else
	{
		if (!s_state.failure_message.empty())
		{
			const std::string summary = fmt::format("{}: {}", s_state.failure_name, s_state.failure_message);
			if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_TRIANGLE_EXCLAMATION, "Install Failed"), summary.c_str(), FSUI_CSTR("Dismiss")))
				s_state.failure_message.clear();
		}

		if (s_state.feed == FeedState::NotLoaded || s_state.feed == FeedState::Loading)
		{
			MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_NOTCH, "Loading driver list..."), FSUI_CSTR("Asking GitHub which driver packs are available."));
		}
		else if (const DP::RemoteDriver* pick = DP::PickRecommended(s_state.listing.drivers))
		{
			std::string summary = pick->tag;
			if (pick->size > 0)
				summary += "  |  " + FormatSize(pick->size);
			const std::string title = fmt::format("{} {}###drv_setup_get", ICON_FA_CLOUD_ARROW_DOWN, NoHash(pick->release_name.empty() ? pick->tag : pick->release_name));
			if (MenuButtonWithValue(title.c_str(), summary.c_str(), FSUI_CSTR("Download")))
				StartInstall(*pick, true);
		}
		else
		{
			// The service words the reason for display ("GitHub is limiting requests from this network; ...").
			const std::string reason = s_state.listing.errors.empty() ? std::string(FSUI_STR("The feed has no driver packs for this device right now.")) :
																		 s_state.listing.errors.front().message;
			if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_TRIANGLE_EXCLAMATION, "Could not load the driver list"), reason.c_str(), FSUI_CSTR("Try Again")))
				StartFeedFetch(true);
		}

		MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "You can skip this"), FSUI_CSTR("Choose a driver later under Settings, GPU Drivers."));
	}

	EndMenuButtons();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// Same scheme as FullscreenUI.cpp and FullscreenUI_Settings.cpp: tools/generate_fullscreen_ui_translation_strings.py
// regenerates the block below from the FSUI_* macros above.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "Your launcher sets the Vulkan driver itself, so the choice below has no effect.");
TRANSLATE_NOOP("FullscreenUI", "A driver pack is selected, but this device has no Adreno GPU. The system driver is in use.");
TRANSLATE_NOOP("FullscreenUI", "Using the system driver.");
TRANSLATE_NOOP("FullscreenUI", "Set by Launcher");
TRANSLATE_NOOP("FullscreenUI", "System Driver");
TRANSLATE_NOOP("FullscreenUI", "Checking the download...");
TRANSLATE_NOOP("FullscreenUI", "Unpacking...");
TRANSLATE_NOOP("FullscreenUI", "Checking that the driver loads...");
TRANSLATE_NOOP("FullscreenUI", "Finishing...");
TRANSLATE_NOOP("FullscreenUI", "Starting...");
TRANSLATE_NOOP("FullscreenUI", "ARMSX2 uses it after it restarts, when you finish setup.");
TRANSLATE_NOOP("FullscreenUI", "Choose it on the GPU Drivers page to use it.");
TRANSLATE_NOOP("FullscreenUI", "The install was cancelled. Nothing was changed.");
TRANSLATE_NOOP("FullscreenUI", "The install failed.");
TRANSLATE_NOOP("FullscreenUI", "Another driver is being installed.");
TRANSLATE_NOOP("FullscreenUI", "Cannot Restart");
TRANSLATE_NOOP("FullscreenUI", "ARMSX2 could not prepare to restart.");
TRANSLATE_NOOP("FullscreenUI", "A game is running. Restarting closes it, and anything you have not saved is lost.");
TRANSLATE_NOOP("FullscreenUI", "A driver is still being installed. Restarting cancels it.");
TRANSLATE_NOOP("FullscreenUI", "Restart now?");
TRANSLATE_NOOP("FullscreenUI", "Restart ARMSX2?");
TRANSLATE_NOOP("FullscreenUI", "Wayland Only");
TRANSLATE_NOOP("FullscreenUI", "This driver pack was built for Wayland sessions only, and ARMSX2 is running in an X11 session. It will probably not be able to show a picture.\n\nUse it anyway?");
TRANSLATE_NOOP("FullscreenUI", "Delete Driver Pack?");
TRANSLATE_NOOP("FullscreenUI", "Cannot Delete");
TRANSLATE_NOOP("FullscreenUI", "The driver pack could not be deleted.");
TRANSLATE_NOOP("FullscreenUI", "Wayland only; this is an X11 session");
TRANSLATE_NOOP("FullscreenUI", "Wayland only");
TRANSLATE_NOOP("FullscreenUI", "Installed");
TRANSLATE_NOOP("FullscreenUI", "GPU Drivers");
TRANSLATE_NOOP("FullscreenUI", "Driver packs are Turnip builds for Qualcomm Adreno GPUs. This device has no Adreno GPU, so it keeps using its system driver.");
TRANSLATE_NOOP("FullscreenUI", "Driver packs are only offered on Linux.");
TRANSLATE_NOOP("FullscreenUI", "Driver In Use");
TRANSLATE_NOOP("FullscreenUI", "The selected driver pack was built for Wayland sessions only, and ARMSX2 is running in an X11 session. It will probably not be able to show a picture. Choose the system driver or another pack, or start ARMSX2 in a Wayland session.");
TRANSLATE_NOOP("FullscreenUI", "Not Installed");
TRANSLATE_NOOP("FullscreenUI", "Pre-release");
TRANSLATE_NOOP("FullscreenUI", "Installing");
TRANSLATE_NOOP("FullscreenUI", "Download");
TRANSLATE_NOOP("FullscreenUI", "The feed has no driver packs for this device right now.");
TRANSLATE_NOOP("FullscreenUI", "Driver packs are Turnip builds for Qualcomm Adreno GPUs. This device uses its system driver.");
TRANSLATE_NOOP("FullscreenUI", "Current Driver");
TRANSLATE_NOOP("FullscreenUI", "The driver you chose applies the next time ARMSX2 starts. Restarting closes ARMSX2 and the running game, then opens it again.");
TRANSLATE_NOOP("FullscreenUI", "The driver you chose applies the next time ARMSX2 starts. Restart now to use it; ARMSX2 opens again with the same options.");
TRANSLATE_NOOP("FullscreenUI", "The selected pack was built for Wayland sessions only, and this is an X11 session. Choose another driver.");
TRANSLATE_NOOP("FullscreenUI", "Dismiss");
TRANSLATE_NOOP("FullscreenUI", "Choose Driver");
TRANSLATE_NOOP("FullscreenUI", "The Vulkan driver that came with this device. A change applies the next time ARMSX2 starts.");
TRANSLATE_NOOP("FullscreenUI", "Selected");
TRANSLATE_NOOP("FullscreenUI", "This driver pack is not installed. Select the system driver or install the pack.");
TRANSLATE_NOOP("FullscreenUI", "Available Driver Packs");
TRANSLATE_NOOP("FullscreenUI", "Asking GitHub which driver packs are available.");
TRANSLATE_NOOP("FullscreenUI", "Try Again");
TRANSLATE_NOOP("FullscreenUI", "GitHub could not be reached, so this list may be out of date.");
TRANSLATE_NOOP("FullscreenUI", "Asks GitHub for the current list of driver packs.");
TRANSLATE_NOOP("FullscreenUI", "Installed Driver Packs");
TRANSLATE_NOOP("FullscreenUI", "GPU Driver");
TRANSLATE_NOOP("FullscreenUI", "ARMSX2 has its own Vulkan driver for your GPU. It is the driver ARMSX2 is tuned for.");
TRANSLATE_NOOP("FullscreenUI", "ARMSX2 restarts when you finish setup so the driver can load.");
TRANSLATE_NOOP("FullscreenUI", "Choose a driver later under Settings, GPU Drivers.");
TRANSLATE_NOOP("FullscreenUI", "Using the driver pack \"{}\".");
TRANSLATE_NOOP("FullscreenUI", "The selected driver pack \"{}\" is not installed. The system driver is in use.");
TRANSLATE_NOOP("FullscreenUI", "Downloading: {} of {} ({}%)");
TRANSLATE_NOOP("FullscreenUI", "Downloading: {}");
TRANSLATE_NOOP("FullscreenUI", "Installing {}");
TRANSLATE_NOOP("FullscreenUI", "Installed {}");
TRANSLATE_NOOP("FullscreenUI", "Cancelled {}");
TRANSLATE_NOOP("FullscreenUI", "Could not install {}");
TRANSLATE_NOOP("FullscreenUI", "Delete \"{}\" from this device? You can download it again later.");
TRANSLATE_NOOP("FullscreenUI", "Deleted {}.");
TRANSLATE_NOOP("FullscreenUI", "\n\nVersion: {}");
TRANSLATE_NOOP("FullscreenUI", "\nMesa: {}");
TRANSLATE_NOOP("FullscreenUI", "\n\nWhen this pack is loaded, the GS log's \"VK: GPU\" line ends with {}.");
TRANSLATE_NOOP("FullscreenUI", "Installing {}: {}");
TRANSLATE_NOOP("FullscreenUI", "The selected driver pack \"{}\" is not installed, so ARMSX2 uses the system driver.");
TRANSLATE_NOOP("FullscreenUI", "Could not load the {} list");
TRANSLATE_NOOP("FullscreenUI", "{} is already installed.");
TRANSLATE_NOOP("FullscreenUI", "Delete {}");
TRANSLATE_NOOP("FullscreenUI", "Removes \"{}\" from this device. If it is selected, the system driver is used instead.");
TRANSLATE_NOOP("FullscreenUI", "No driver packs for this GPU.");
TRANSLATE_NOOP("FullscreenUI", "Driver packs are not available on this platform.");
TRANSLATE_NOOP("FullscreenUI", "Restart Now");
TRANSLATE_NOOP("FullscreenUI", "Cancel Install");
TRANSLATE_NOOP("FullscreenUI", "Install Failed");
TRANSLATE_NOOP("FullscreenUI", "Loading driver list...");
TRANSLATE_NOOP("FullscreenUI", "Showing the last saved list");
TRANSLATE_NOOP("FullscreenUI", "No driver packs found");
TRANSLATE_NOOP("FullscreenUI", "Refresh List");
TRANSLATE_NOOP("FullscreenUI", "Could not load the driver list");
TRANSLATE_NOOP("FullscreenUI", "You can skip this");
// TRANSLATION-STRING-AREA-END
#endif
