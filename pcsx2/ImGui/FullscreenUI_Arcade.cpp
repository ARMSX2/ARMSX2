// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture screens for Namco System 246/256 arcade games, over the shared services in pcsx2/Arcade:
//
//   - the global "Arcade Setup" settings page: boot files (download, update), the arcade BIOS, and the
//     button layout that applies to every arcade game;
//   - an "Arcade" section on a game's own Summary page: what the game still lacks, and its own layout;
//   - the pause menu's Arcade submenu: coin, Service and Test while an arcade game runs;
//   - starting an arcade game: ArcadeSetup::Service::Prepare() first, and the missing-files report instead
//     of a boot when something is not there.
//
// The other FullscreenUI files call in here with a line each (declarations at the end of
// FullscreenUI_Internal.h). FullscreenUI state belongs to the GS thread; the downloads and Prepare() run on
// a worker thread of their own and report back by posting to the GS thread, as FullscreenUI_Packs.cpp does.

#include "Arcade/ArcadeFiles.h"
#include "Arcade/ArcadeInput.h"
#include "Arcade/ArcadeSetup.h"
#include "Catalog/CancelToken.h"
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
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"
#include "imgui.h"
#include "imgui_internal.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <thread>

namespace Setup = ArcadeSetup;

namespace
{
	using namespace FullscreenUI;
	using GIB = GenericInputBinding;

	constexpr const char* NOTICE_KEY = "arcade_setup";
	constexpr const char* LAUNCH_NOTICE_KEY = "arcade_launch";

	// How long the cabinet's Service button is held for a press from the pause menu (Android: 200 ms).
	constexpr double SERVICE_HOLD_SECONDS = 0.2;

	// The GitHub release is asked about at most this often while the page stays open or is opened again.
	constexpr double UPDATE_CHECK_MAX_AGE_SECONDS = 600.0;

	// ------------------------------------------------------------------------------------------------
	// Worker threads
	// ------------------------------------------------------------------------------------------------

	// One background job at a time. Start() and Join() are called from the GS thread only.
	class Worker
	{
	public:
		// A finished thread that was never joined would end the program at exit (std::thread's destructor).
		~Worker() { Join(); }

		bool IsBusy() const { return m_busy.load(); }

		bool Start(std::function<void()> job)
		{
			if (m_busy.exchange(true))
				return false;
			Join();
			m_thread = std::thread([this, job = std::move(job)]() {
				job();
				m_busy.store(false);
			});
			return true;
		}

		void Join()
		{
			if (m_thread.joinable())
				m_thread.join();
		}

	private:
		std::thread m_thread;
		std::atomic_bool m_busy{false};
	};

	// Downloads, installs and Prepare() share this one. The update check has its own, so the Download button
	// does not wait for a slow GitHub.
	Worker s_work;
	Worker s_check_worker;
	Catalog::CancelToken s_work_cancel;
	Catalog::CancelToken s_check_cancel;

	// A push into the GS ring is only allowed from the CPU thread, so a worker hops through it.
	void PostToGSThread(std::function<void()> function)
	{
		Host::RunOnCPUThread([function = std::move(function)]() mutable {
			if (MTGS::IsOpen())
				MTGS::RunOnGSThread(std::move(function));
		});
	}

	// ------------------------------------------------------------------------------------------------
	// State (GS thread)
	// ------------------------------------------------------------------------------------------------

	// What the setup page shows, read from disk when the page is opened and after a job.
	Setup::BootFilesStatus s_boot;
	Setup::BiosStatus s_bios;
	int s_setup_page_frame = -100;
	PageDwell s_setup_dwell; ///< the update check waits until the page has stayed open

	// The GitHub answer about the boot files, for the version the page shows. A change of the stored boot files
	// makes an earlier answer (still on its way) stale: it carries the generation it was asked in.
	u32 s_check_generation = 0;
	bool s_check_pending = false;
	std::optional<Setup::UpdateCheck> s_check;
	Common::Timer s_check_age;

	bool s_boot_job_active = false; // A download or an install from a file runs.
	bool s_boot_job_is_download = false; // ... and it is a download, which can be cancelled.
	std::atomic<u64> s_progress_done{0};
	std::atomic<u64> s_progress_total{0};

	// A game's report, read when its settings are opened and after the boot files change.
	std::map<std::string, Setup::MissingReport> s_missing;
	int s_game_page_frame = -100;
	std::string s_layout_game_id;
	std::optional<ArcadeInput::Layout> s_layout;

	// The Service button is held for a moment after it is pressed from the pause menu.
	bool s_service_down = false;
	Common::Timer s_service_timer;

	// ------------------------------------------------------------------------------------------------
	// Small helpers
	// ------------------------------------------------------------------------------------------------

	// True the first time a page is drawn after it was not (it was closed, or another page was showing).
	bool IsFreshOpen(int* last_frame)
	{
		const int frame = ImGui::GetFrameCount();
		const bool fresh = (frame - *last_frame) > 1;
		*last_frame = frame;
		return fresh;
	}

	std::string FormatDate(std::time_t time)
	{
		if (time == 0)
			return std::string();
		char buf[64];
		struct tm local;
#ifdef _MSC_VER
		localtime_s(&local, &time);
#else
		localtime_r(&time, &local);
#endif
		std::strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
		return buf;
	}

	std::string FormatMegabytes(u64 bytes)
	{
		return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	}

	const char* ButtonName(const ArcadeInput::ButtonInfo& button)
	{
		return Host::TranslateToCString("Arcade", button.display_name);
	}

	const ArcadeInput::ButtonInfo* FindButtonInfo(GIB binding)
	{
		for (const ArcadeInput::ButtonInfo& button : ArcadeInput::GetRemappableButtons())
		{
			if (button.binding == binding)
				return &button;
		}
		return nullptr;
	}

	void ShowNotice(const std::string& title, const std::string& text, float seconds)
	{
		ImGuiFullscreen::AddNotification(NOTICE_KEY, seconds, fmt::format("{} {}", ICON_FA_COINS, title), text, std::string());
	}

	void RefreshSetupStatus()
	{
		const Setup::Service service;
		s_boot = service.GetBootFilesStatus();
		s_bios = service.GetBiosStatus();
	}

	// ------------------------------------------------------------------------------------------------
	// Boot files: update check, download, install from a file
	// ------------------------------------------------------------------------------------------------

	// Forgets what GitHub said and what was asked: the stored boot files changed, or are about to.
	void DropUpdateCheck()
	{
		s_check_cancel.Cancel();
		s_check_generation++;
		s_check_pending = false;
		s_check.reset();
	}

	void StartUpdateCheck()
	{
		if (s_check.has_value() || s_check_pending || s_check_worker.IsBusy() || s_work.IsBusy() || !s_boot.installed)
			return;

		s_check_cancel = Catalog::CancelToken();
		const Catalog::CancelToken cancel = s_check_cancel;
		const u32 generation = s_check_generation;
		if (!s_check_worker.Start([cancel, generation]() {
				const Setup::Service service;
				Setup::UpdateCheck result = service.CheckForBootFilesUpdate(Catalog::GetDefaultFetcher(), cancel);
				PostToGSThread([generation, result = std::move(result)]() mutable {
					if (generation != s_check_generation)
						return;
					s_check_pending = false;
					s_check = std::move(result);
					s_check_age.Reset();
				});
			}))
		{
			return;
		}
		s_check_pending = true;
	}

	// GS thread: a download or install ended.
	void FinishBootFilesJob(const Setup::InstallResult& result)
	{
		s_boot_job_active = false;
		s_boot = result.status;
		s_missing.clear();
		DropUpdateCheck();

		if (result.ok)
		{
			ShowNotice(FSUI_STR("Arcade Boot Files"),
				fmt::format(FSUI_FSTR("Installed. They have boot programs for {} games."), result.status.game_ids.size()), 6.0f);
		}
		else if (result.cancelled)
		{
			ShowNotice(FSUI_STR("Arcade Boot Files"), FSUI_STR("Cancelled."), 4.0f);
		}
		else
		{
			ShowNotice(FSUI_STR("Arcade Boot Files"), result.error, 10.0f);
		}
	}

	void StartBootFilesDownload()
	{
		if (s_work.IsBusy())
		{
			ShowToast(ICON_FA_COINS, FSUI_STR("Another arcade job is still running."));
			return;
		}

		DropUpdateCheck();
		s_boot_job_active = true;
		s_boot_job_is_download = true;
		s_progress_done = 0;
		s_progress_total = 0;
		s_work_cancel = Catalog::CancelToken();
		const Catalog::CancelToken cancel = s_work_cancel;
		ShowNotice(FSUI_STR("Arcade Boot Files"), FSUI_STR("Downloading..."), 8.0f);

		s_work.Start([cancel]() {
			Setup::DownloadOptions options;
			options.cancel = cancel;
			options.on_progress = [](u64 done, u64 total) {
				s_progress_done = done;
				s_progress_total = total;
			};
			Setup::InstallResult result = Setup::Service().DownloadBootFiles(Catalog::GetDefaultFetcher(), options);
			PostToGSThread([result = std::move(result)]() { FinishBootFilesJob(result); });
		});
	}

	void StartBootFilesInstall(std::string zip_path)
	{
		if (s_work.IsBusy())
		{
			ShowToast(ICON_FA_COINS, FSUI_STR("Another arcade job is still running."));
			return;
		}

		DropUpdateCheck();
		s_boot_job_active = true;
		s_boot_job_is_download = false;
		s_progress_done = 0;
		s_progress_total = 0;
		s_work_cancel = Catalog::CancelToken();

		s_work.Start([zip_path = std::move(zip_path)]() {
			Setup::InstallResult result = Setup::Service().InstallBootFilesFromFile(zip_path);
			PostToGSThread([result = std::move(result)]() { FinishBootFilesJob(result); });
		});
	}

	// ------------------------------------------------------------------------------------------------
	// What a game lacks
	// ------------------------------------------------------------------------------------------------

	bool NeedsBootFiles(const Setup::MissingReport& report)
	{
		return std::any_of(report.parts.begin(), report.parts.end(),
			[](const Setup::MissingPart& part) { return part.kind == Setup::PartKind::BootProgram; });
	}

	// The whole report: every missing part, in a sentence each. The one part that Big Picture can fix, the boot
	// files, comes with a button for it.
	void ShowMissingReport(const std::string& game_title, const Setup::MissingReport& report, const std::string& error)
	{
		std::string text;
		if (report.parts.empty())
		{
			text = error;
		}
		else
		{
			text = fmt::format(FSUI_FSTR("{} cannot be started yet."), game_title.empty() ? FSUI_STR("This game") : game_title);
			for (const Setup::MissingPart& part : report.parts)
			{
				text += "\n\n- ";
				text += part.Explain();
			}
		}

		const std::string title = FSUI_ICONSTR(ICON_FA_TRIANGLE_EXCLAMATION, "Arcade Game Not Ready");
		if (!NeedsBootFiles(report))
		{
			OpenInfoMessageDialog(title, std::move(text));
			return;
		}

		const std::string download = s_boot.installed ? std::string(FSUI_ICONSTR(ICON_FA_CLOUD_ARROW_DOWN, "Update Boot Files")) :
														std::string(FSUI_ICONSTR(ICON_FA_CLOUD_ARROW_DOWN, "Download Boot Files"));
		OpenConfirmMessageDialog(title, std::move(text),
			[](bool yes) {
				if (yes)
					StartBootFilesDownload();
			},
			true, download, FSUI_ICONSTR(ICON_FA_SQUARE_XMARK, "Close"));
	}

	// ------------------------------------------------------------------------------------------------
	// Starting a game
	// ------------------------------------------------------------------------------------------------

	// A game the board runs: an .acgame, or a loose image (named after its game, or in a folder that is).
	bool IsArcadeStartPath(const std::string& path)
	{
		return VMManager::IsArcadeGameFileName(path) || !Arcade::LooseImageGameId(path).empty();
	}

	void DismissLaunchNotice()
	{
		ImGuiFullscreen::AddNotification(LAUNCH_NOTICE_KEY, 0.0f, std::string(), std::string(), std::string());
	}

	void StartPrepare(std::string path, std::optional<s32> state_index, std::optional<bool> fast_boot, std::string save_state)
	{
		if (s_work.IsBusy())
		{
			ShowToast(ICON_FA_COINS, s_boot_job_active ? FSUI_STR("Wait for the boot files to finish downloading.") :
														  FSUI_STR("Another arcade game is being prepared."));
			return;
		}

		ImGuiFullscreen::AddNotification(LAUNCH_NOTICE_KEY, 10.0f, fmt::format("{} {}", ICON_FA_COINS, FSUI_STR("Arcade")),
			FSUI_STR("Getting the game ready..."), std::string());

		// Copying a dongle or unpacking a card takes a moment: not on the GS thread.
		s_work.Start([path = std::move(path), state_index, fast_boot, save_state = std::move(save_state)]() {
			Setup::PrepareResult result = Setup::Service().Prepare(path);
			if (!result.ok)
			{
				PostToGSThread([result = std::move(result)]() {
					DismissLaunchNotice();
					s_boot = Setup::Service().GetBootFilesStatus();
					ShowMissingReport(result.title, result.missing, result.error);
				});
				return;
			}

			VMBootParameters params;
			result.ApplyTo(&params);
			params.state_index = state_index;
			params.fast_boot = fast_boot;
			params.save_state = save_state;
			PostToGSThread([]() { DismissLaunchNotice(); });
			Host::RunOnCPUThread([params = std::move(params)]() { DoVMInitialize(params, false); });
		});
	}

	// ------------------------------------------------------------------------------------------------
	// The button layout
	// ------------------------------------------------------------------------------------------------

	const ArcadeInput::Layout& LayoutFor(const std::string& game_id)
	{
		if (!s_layout.has_value() || s_layout_game_id != game_id)
		{
			s_layout = ArcadeInput::GetLayout(game_id);
			s_layout_game_id = game_id;
		}
		return *s_layout;
	}

	// The analog pedals of a driving cabinet stay on their triggers.
	const ArcadeInput::Job* FixedJobOf(const ArcadeInput::Layout& layout, GIB button)
	{
		for (const ArcadeInput::Job& job : layout.jobs)
		{
			if (job.fixed && std::find(job.buttons.begin(), job.buttons.end(), button) != job.buttons.end())
				return &job;
		}
		return nullptr;
	}

	std::string RowValue(const std::string& value, bool changed)
	{
		return changed ? fmt::format(FSUI_FSTR("{} (changed)"), value) : value;
	}

	// All arcade games: each pad button acts as another pad button. Picking is two steps for a controller: the
	// button here, then what it acts as in a list. [index] is the list's: the remappable buttons, then Nothing.
	void ApplyGlobalTarget(GIB button, s32 index)
	{
		const std::span<const ArcadeInput::ButtonInfo> buttons = ArcadeInput::GetRemappableButtons();
		const GIB target = (index >= 0 && static_cast<size_t>(index) < buttons.size()) ? buttons[index].binding : GIB::Unknown;

		auto lock = Host::GetSettingsLock();
		SettingsInterface* base = Host::Internal::GetBaseSettingsLayer();
		ArcadeInput::Remap remap = ArcadeInput::LoadRemap(*base);
		ArcadeInput::SetGlobalTarget(remap, button, target);
		ArcadeInput::SaveRemap(*base, remap);
		SetSettingsChanged(base);
	}

	void PickGlobalTarget(GIB button)
	{
		const std::span<const ArcadeInput::ButtonInfo> buttons = ArcadeInput::GetRemappableButtons();
		const ArcadeInput::Remap global = ArcadeInput::LoadRemap(*Host::Internal::GetBaseSettingsLayer());
		const auto it = global.find(button);
		const GIB current = (it != global.end()) ? it->second : button;

		ImGuiFullscreen::ChoiceDialogOptions options;
		for (const ArcadeInput::ButtonInfo& candidate : buttons)
		{
			std::string name = ButtonName(candidate);
			if (candidate.binding == button)
				name = fmt::format(FSUI_FSTR("{} (Default)"), name);
			options.emplace_back(std::move(name), candidate.binding == current);
		}
		options.emplace_back(FSUI_STR("Nothing"), current == GIB::Unknown);

		const ArcadeInput::ButtonInfo* info = FindButtonInfo(button);
		OpenChoiceDialog(fmt::format(FSUI_FSTR("What {} Does"), info ? ButtonName(*info) : ""), false, std::move(options),
			[button](s32 index, const std::string& title, bool checked) {
				if (index < 0)
					return;
				ApplyGlobalTarget(button, index);
				CloseChoiceDialog();
			});
	}

	void DrawGlobalLayoutRows()
	{
		const ArcadeInput::Remap global = ArcadeInput::LoadRemap(*Host::Internal::GetBaseSettingsLayer());
		for (const ArcadeInput::ButtonInfo& button : ArcadeInput::GetRemappableButtons())
		{
			const auto it = global.find(button.binding);
			const GIB target = (it != global.end()) ? it->second : button.binding;
			const ArcadeInput::ButtonInfo* target_info = FindButtonInfo(target);
			std::string value;
			if (target == button.binding)
				value = FSUI_STR("Default");
			else if (target_info)
				value = fmt::format(FSUI_FSTR("Acts As {}"), ButtonName(*target_info));
			else
				value = FSUI_STR("Nothing");

			const std::string title = fmt::format("{}##arcade_all_{}", ButtonName(button), button.name);
			if (MenuButtonWithValue(title.c_str(), nullptr, value.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
				PickGlobalTarget(button.binding);
		}
	}

	// One game: each pad button does one of the cabinet's jobs. [index] is the list's: the jobs that are not an
	// analog pedal's, then Nothing.
	void ApplyGameJob(const std::string& game_id, GIB button, s32 index)
	{
		auto lock = Host::GetSettingsLock();
		// The page was left for another game's while the list was open.
		if (!s_game_settings_interface || s_game_settings_serial != game_id)
			return;

		const ArcadeInput::Layout& layout = LayoutFor(game_id);
		std::vector<const ArcadeInput::Job*> choices;
		for (const ArcadeInput::Job& job : layout.jobs)
		{
			if (!job.fixed)
				choices.push_back(&job);
		}
		const ArcadeInput::Job* job = (index >= 0 && static_cast<size_t>(index) < choices.size()) ? choices[index] : nullptr;

		const ArcadeInput::Remap global = ArcadeInput::LoadRemap(*Host::Internal::GetBaseSettingsLayer());
		ArcadeInput::Remap game = ArcadeInput::LoadRemap(*s_game_settings_interface);
		ArcadeInput::SetJob(game, global, layout, button, job);
		ArcadeInput::SaveRemap(*s_game_settings_interface, game);
		SetSettingsChanged(s_game_settings_interface.get());
	}

	void PickGameJob(const std::string& game_id, GIB button)
	{
		const ArcadeInput::Layout& layout = LayoutFor(game_id);
		const ArcadeInput::Remap global = ArcadeInput::LoadRemap(*Host::Internal::GetBaseSettingsLayer());
		const ArcadeInput::Remap game = ArcadeInput::LoadRemap(*s_game_settings_interface);
		const ArcadeInput::Job* current = ArcadeInput::GetJobOf(layout, global, game, button);

		ImGuiFullscreen::ChoiceDialogOptions options;
		for (const ArcadeInput::Job& job : layout.jobs)
		{
			if (!job.fixed)
				options.emplace_back(job.label, &job == current);
		}
		options.emplace_back(FSUI_STR("Nothing"), current == nullptr);

		const ArcadeInput::ButtonInfo* info = FindButtonInfo(button);
		OpenChoiceDialog(fmt::format(FSUI_FSTR("What {} Does"), info ? ButtonName(*info) : ""), false, std::move(options),
			[game_id, button](s32 index, const std::string& title, bool checked) {
				if (index < 0)
					return;
				ApplyGameJob(game_id, button, index);
				CloseChoiceDialog();
			});
	}

	void DrawGameLayoutRows(const std::string& game_id)
	{
		const ArcadeInput::Layout& layout = LayoutFor(game_id);
		const ArcadeInput::Remap global = ArcadeInput::LoadRemap(*Host::Internal::GetBaseSettingsLayer());
		const ArcadeInput::Remap game = ArcadeInput::LoadRemap(*s_game_settings_interface);

		for (const ArcadeInput::ButtonInfo& button : ArcadeInput::GetRemappableButtons())
		{
			const std::string title = fmt::format("{}##arcade_game_{}", ButtonName(button), button.name);

			if (const ArcadeInput::Job* fixed = FixedJobOf(layout, button.binding))
			{
				const std::string value = fmt::format(FSUI_FSTR("{} (analog)"), fixed->label);
				MenuButtonWithValue(title.c_str(), nullptr, value.c_str(), false, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY);
				continue;
			}

			const ArcadeInput::Job* job = ArcadeInput::GetJobOf(layout, global, game, button.binding);
			const bool changed = (job != ArcadeInput::GetJobOf(layout, {}, {}, button.binding));
			const std::string value = RowValue(job ? job->label : FSUI_STR("Nothing"), changed);
			if (MenuButtonWithValue(title.c_str(), nullptr, value.c_str(), true, LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY))
				PickGameJob(game_id, button.binding);
		}
	}

	// How the pad works: the layout in general, then what each kind of cabinet does with the sticks and triggers.
	void ShowControlsHelp()
	{
		std::string text = FSUI_STR("A controller button does the cabinet job it is given in the game: the D-pad is the lever, the face "
									"buttons are the cabinet's buttons, and Select inserts a coin. Below you can give a button another "
									"button's job, for every arcade game. A game can have a layout of its own: open the game's settings and "
									"look under Arcade Controls. The analog pedals of a driving cabinet stay on their triggers.\n\n"
									"Each arcade game is one of these kinds of cabinet:");
		for (const ArcadeInput::ModeInfo& mode : ArcadeInput::GetModes())
		{
			text += "\n\n";
			text += Host::TranslateToString("Arcade", mode.display_name);
			text += ": ";
			text += Host::TranslateToString("Arcade", mode.hint);
		}
		OpenInfoMessageDialog(FSUI_ICONSTR(ICON_FA_GAMEPAD, "How The Controller Works"), std::move(text));
	}

	// ------------------------------------------------------------------------------------------------
	// The setup page
	// ------------------------------------------------------------------------------------------------

	std::string UpdateLine()
	{
		if (!s_boot.installed || s_boot_job_active)
			return std::string();
		if (s_check_pending && !s_check.has_value())
			return FSUI_STR("Checking for a newer version...");
		if (!s_check.has_value())
			return std::string();

		switch (s_check->state)
		{
			case Setup::UpdateCheck::State::UpdateAvailable:
			{
				if (s_check->release_name.empty())
					return FSUI_STR("A newer version is available.");
				return fmt::format(FSUI_FSTR("A newer version is available: {}."), s_check->release_name);
			}
			case Setup::UpdateCheck::State::UpToDate:
				return FSUI_STR("This is the latest version.");
			case Setup::UpdateCheck::State::Failed:
			case Setup::UpdateCheck::State::NotInstalled:
			default:
				return FSUI_STR("Could not check for a newer version.");
		}
	}

	std::string BootFilesSummary()
	{
		if (!s_boot.installed)
		{
			return FSUI_STR("Every arcade game starts from a small boot program. They come from the Proverb project "
							"(PS2Homebrew-arcade) on GitHub, about 1 MB for all games at once.");
		}

		std::string text;
		if (!s_boot.release_name.empty())
		{
			const std::string built = FormatDate(s_boot.release_time);
			text = built.empty() ? s_boot.release_name :
								   fmt::format(FSUI_FSTR("{}, built {}"), s_boot.release_name, built);
		}
		else
		{
			text = fmt::format(FSUI_FSTR("Downloaded {}"), FormatDate(s_boot.installed_at));
		}
		text += fmt::format(FSUI_FSTR(". Boot programs for {} games."), s_boot.game_ids.size());

		const std::string update = UpdateLine();
		if (!update.empty())
		{
			text += ' ';
			text += update;
		}
		return text;
	}

	void ShowBootFilesDetails()
	{
		std::string text = FSUI_STR("Every arcade game starts from a small boot program (boot.elf). The PS2Homebrew-arcade project "
									"\"Proverb\" builds one for each game, and its nightly release holds all of them in one zip of about "
									"1 MB. It is stored once, and the game you start takes its boot program out of it. Games that come "
									"with their own boot.elf use that one.");
		text += "\n\n";
		text += fmt::format(FSUI_FSTR("Source: {}"), "github.com/PS2Homebrew-arcade/proverb");
		if (s_boot.installed)
		{
			text += "\n";
			text += fmt::format(FSUI_FSTR("Stored as: {}"), s_boot.path);
			text += "\n";
			text += fmt::format(FSUI_FSTR("Size: {}"), FormatMegabytes(s_boot.size_bytes));
			if (!s_boot.game_ids.empty())
			{
				text += "\n";
				text += fmt::format(FSUI_FSTR("Games: {}"), StringUtil::JoinString(s_boot.game_ids.begin(), s_boot.game_ids.end(), ", "));
			}
		}
		OpenInfoMessageDialog(FSUI_ICONSTR(ICON_FA_FILE_ZIPPER, "Arcade Boot Files"), std::move(text));
	}

	void DrawBootFilesRows()
	{
		MenuHeading(FSUI_CSTR("Boot Files"));

		if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_FILE_ZIPPER, "Arcade Boot Files"), BootFilesSummary().c_str(),
				s_boot.installed ? FSUI_CSTR("Installed") : FSUI_CSTR("Not Downloaded")))
		{
			ShowBootFilesDetails();
		}

		if (s_boot_job_active && !s_boot_job_is_download)
		{
			MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_FILE_ZIPPER, "Installing"), FSUI_CSTR("Reading the boot files you picked."), "...", false);
			return;
		}

		if (s_boot_job_active)
		{
			const u64 done = s_progress_done.load();
			const u64 total = s_progress_total.load();
			std::string progress = (total != 0) ? fmt::format("{}%", std::min<u64>(100, done * 100 / total)) :
												  (done != 0 ? FormatMegabytes(done) : std::string("..."));
			if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_XMARK, "Cancel"), FSUI_CSTR("Stops the download. Boot files you already have stay as they are."),
					progress.c_str()))
			{
				s_work_cancel.Cancel();
			}
			return;
		}

		const bool update_available = s_check.has_value() && s_check->state == Setup::UpdateCheck::State::UpdateAvailable;
		if (!s_boot.installed)
		{
			if (MenuButton(FSUI_ICONSTR(ICON_FA_CLOUD_ARROW_DOWN, "Download Boot Files"),
					FSUI_CSTR("Downloads the boot programs from GitHub. Needs an internet connection.")))
			{
				StartBootFilesDownload();
			}
		}
		else if (update_available)
		{
			if (MenuButton(FSUI_ICONSTR(ICON_FA_CLOUD_ARROW_DOWN, "Update Boot Files"),
					FSUI_CSTR("Replaces the boot files with the newer version. Needs an internet connection.")))
			{
				StartBootFilesDownload();
			}
		}
		else
		{
			if (MenuButton(FSUI_ICONSTR(ICON_FA_ARROWS_ROTATE, "Download Again"),
					FSUI_CSTR("Downloads the boot files again, replacing what is stored.")))
			{
				StartBootFilesDownload();
			}
		}

		if (MenuButton(FSUI_ICONSTR(ICON_FA_FILE_ARROW_DOWN, "Install From A File"),
				FSUI_CSTR("For a device with no internet connection: pick the proverb.zip you downloaded on another device.")))
		{
			OpenFileSelector(FSUI_ICONSTR(ICON_FA_FILE_ZIPPER, "Select Boot Files"), false,
				[](const std::string& path) {
					if (!path.empty())
						StartBootFilesInstall(path);
					CloseFileSelector();
				},
				{"*.zip"});
		}
	}

	void DrawBiosRows()
	{
		MenuHeading(FSUI_CSTR("Arcade BIOS"));

		std::string summary;
		if (s_bios.IsPresent())
		{
			summary = fmt::format(FSUI_FSTR("Found: {}. Each game starts on a BIOS it runs on."), s_bios.Summary());
		}
		else
		{
			summary = Setup::MissingPart{Setup::PartKind::ArcadeBios, std::string()}.Explain();
		}
		summary += ' ';
		summary += fmt::format(FSUI_FSTR("BIOS folder: {}"), EmuFolders::Bios);

		if (MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_MICROCHIP, "Arcade BIOS"), summary.c_str(),
				s_bios.IsPresent() ? FSUI_CSTR("Found") : FSUI_CSTR("Missing")))
		{
			CopyTextToClipboard(FSUI_STR("BIOS folder copied to clipboard."), EmuFolders::Bios);
		}
	}

	void DrawGamesRows()
	{
		MenuHeading(FSUI_CSTR("Games"));

		if (MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "Adding Games"),
				FSUI_CSTR("Where a game's image and dongle go. Select to read.")))
		{
			OpenInfoMessageDialog(FSUI_STR("Adding Games"),
				FSUI_STR("Put each game's image and its security dongle in one of your game folders (see the game list settings), "
						 "named after the game's ID: NM00010.chd and NM00010.ps2, for example, or both in a folder named NM00010. "
						 "The image is a .chd or an unpacked .iso. The dongle is the game's memory card file, a .ps2 or .bin; a "
						 "packed .bin.gz also works.\n\n"
						 "Nothing is copied except the dongle, into the memory cards folder, the first time the game starts. "
						 "A game made from a PCSX2x6 template folder, with its own .acgame, works as it is.\n\n"
						 "The game list shows a game with what it still lacks, and starting one that is not ready says what to add."));
		}
	}

	void ResetGlobalLayout()
	{
		OpenConfirmMessageDialog(FSUI_ICONSTR(ICON_FA_ROTATE_LEFT, "Reset Button Layout"),
			FSUI_STR("Put every button back to the cabinet's own layout in all arcade games? A game's own layout is not changed."),
			[](bool yes) {
				if (!yes)
					return;
				auto lock = Host::GetSettingsLock();
				SettingsInterface* base = Host::Internal::GetBaseSettingsLayer();
				ArcadeInput::ResetRemap(*base);
				SetSettingsChanged(base);
			},
			false);
	}
} // namespace

//////////////////////////////////////////////////////////////////////////
// Hooks
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::DrawArcadeSettingsPage()
{
	if (IsFreshOpen(&s_setup_page_frame))
	{
		RefreshSetupStatus();
		// A check made a while ago may be out of date; one in progress or just made is not asked again.
		if (s_check.has_value() && s_check_age.GetTimeSeconds() > UPDATE_CHECK_MAX_AGE_SECONDS)
			DropUpdateCheck();
	}
	// GitHub is asked once the page has stayed open, not while the D-pad is only passing it.
	if (s_setup_dwell.Settled())
		StartUpdateCheck();

	BeginMenuButtons();

	DrawBootFilesRows();
	DrawBiosRows();
	DrawGamesRows();

	MenuHeading(FSUI_CSTR("Button Layout For All Arcade Games"));
	if (MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "How The Controller Works"),
			FSUI_CSTR("What each button does on the cabinet, and the kinds of cabinet. A game's own layout comes first. Select to read.")))
	{
		ShowControlsHelp();
	}
	DrawGlobalLayoutRows();
	if (MenuButton(FSUI_ICONSTR(ICON_FA_ROTATE_LEFT, "Reset Button Layout"), FSUI_CSTR("Puts every button back to the cabinet's own layout.")))
		ResetGlobalLayout();

	EndMenuButtons();
}

void FullscreenUI::DrawArcadeGameSettings(const GameList::Entry& entry)
{
	if (entry.type != GameList::EntryType::Arcade || !s_game_settings_interface)
		return;

	if (IsFreshOpen(&s_game_page_frame))
	{
		s_boot = Setup::Service().GetBootFilesStatus();
		s_missing.clear();
		s_layout.reset();
	}

	auto report_it = s_missing.find(entry.path);
	if (report_it == s_missing.end())
		report_it = s_missing.emplace(entry.path, Setup::Service().Missing(entry.path)).first;
	const Setup::MissingReport& report = report_it->second;

	MenuHeading(FSUI_CSTR("Arcade"));

	if (!report.IsReady())
	{
		const std::string summary = fmt::format(FSUI_FSTR("Missing: {}. Select for what to add."), report.Summary());
		if (MenuButton(FSUI_ICONSTR(ICON_FA_TRIANGLE_EXCLAMATION, "Not Ready To Play"), summary.c_str()))
			ShowMissingReport(entry.title, report, std::string());
	}
	else if (report.attract_only)
	{
		MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "Attract Mode Only"),
			FSUI_CSTR("Everything is in place. This game starts but only reaches its demo screen so far."), false);
	}
	else
	{
		MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_CHECK, "Ready To Play"), FSUI_CSTR("Everything this game needs is in place."), false);
	}

	const ArcadeInput::Layout& layout = LayoutFor(entry.serial);
	const ArcadeInput::ModeInfo& mode = ArcadeInput::GetModeInfo(layout.mode);

	MenuHeading(FSUI_CSTR("Arcade Controls"));
	MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_GAMEPAD, "Cabinet Type"), Host::TranslateToCString("Arcade", mode.hint),
		Host::TranslateToCString("Arcade", mode.display_name), false);
	DrawGameLayoutRows(entry.serial);

	if (MenuButton(FSUI_ICONSTR(ICON_FA_ROTATE_LEFT, "Reset Arcade Controls"),
			FSUI_CSTR("Puts every button back to the layout for all arcade games, or the cabinet's own.")))
	{
		OpenConfirmMessageDialog(FSUI_ICONSTR(ICON_FA_ROTATE_LEFT, "Reset Arcade Controls"),
			FSUI_STR("Put every button of this game back to the layout for all arcade games?"),
			[serial = entry.serial](bool yes) {
				if (!yes)
					return;
				auto lock = Host::GetSettingsLock();
				if (!s_game_settings_interface || s_game_settings_serial != serial)
					return;
				ArcadeInput::ResetRemap(*s_game_settings_interface);
				SetSettingsChanged(s_game_settings_interface.get());
			},
			false);
	}
}

bool FullscreenUI::StartArcadePath(const std::string& path, std::optional<s32> state_index, std::optional<bool> fast_boot, std::string save_state)
{
	if (!IsArcadeStartPath(path))
		return false;

	StartPrepare(path, state_index, fast_boot, std::move(save_state));
	return true;
}

bool FullscreenUI::IsArcadeSessionActive()
{
	return ArcadeInput::IsActive();
}

void FullscreenUI::DrawArcadePauseMenuEntry()
{
	if (!ArcadeInput::IsActive())
		return;

	if (ActiveButton(FSUI_ICONSTR(ICON_FA_COINS, "Arcade"), false))
		OpenPauseSubMenu(PauseSubMenu::Arcade);
}

u32 FullscreenUI::GetArcadePauseMenuItemCount(PauseSubMenu submenu)
{
	switch (submenu)
	{
		case PauseSubMenu::None:
			return ArcadeInput::IsActive() ? 1 : 0;
		default:
			return 0;
	}
}

void FullscreenUI::GetArcadePauseWrapIds(ImGuiID* first, ImGuiID* last)
{
	*first = ImGui::GetID(FSUI_ICONSTR(ICON_PF_BACKWARD, "Back To Pause Menu"));
	*last = ImGui::GetID(FSUI_ICONSTR(ICON_FA_SCREWDRIVER_WRENCH, "Test Switch"));
}

void FullscreenUI::DrawArcadePauseSubMenu(bool just_focused)
{
	if (just_focused)
		ImGui::SetFocusID(ImGui::GetID(FSUI_ICONSTR(ICON_PF_BACKWARD, "Back To Pause Menu")), ImGui::GetCurrentWindow());

	if (ActiveButton(FSUI_ICONSTR(ICON_PF_BACKWARD, "Back To Pause Menu"), false) || WantsToCloseMenu())
		OpenPauseSubMenu(PauseSubMenu::None);

	// A coin goes in while the menu stays open, so a second player's credit is one more press; the game takes
	// the credits when it runs again.
	if (ActiveButton(FSUI_ICONSTR(ICON_FA_COINS, "Insert Coin (Player 1)"), false))
	{
		ArcadeInput::InsertCoin(0);
		ShowToast(ICON_FA_COINS, FSUI_STR("Coin inserted for player 1."), 3.0f);
	}

	if (ActiveButton(FSUI_ICONSTR(ICON_FA_COINS, "Insert Coin (Player 2)"), false))
	{
		ArcadeInput::InsertCoin(1);
		ShowToast(ICON_FA_COINS, FSUI_STR("Coin inserted for player 2."), 3.0f);
	}

	// The game has to run for the press to count, so the menu closes first, then the button is held for a moment.
	if (ActiveButton(FSUI_ICONSTR(ICON_FA_HAND_POINTER, "Service Button"), false))
	{
		ClosePauseMenu();
		ArcadeInput::SetService(true);
		s_service_down = true;
		s_service_timer.Reset();
	}

	if (ImGuiFullscreen::ActiveButtonWithRightText(FSUI_ICONSTR(ICON_FA_SCREWDRIVER_WRENCH, "Test Switch"),
			ArcadeInput::IsTestOn() ? FSUI_CSTR("On") : FSUI_CSTR("Off"), false))
	{
		ArcadeInput::ToggleTest();
	}
}

void FullscreenUI::PollArcade()
{
	if (s_service_down && s_service_timer.GetTimeSeconds() >= SERVICE_HOLD_SECONDS)
	{
		s_service_down = false;
		ArcadeInput::SetService(false);
	}
}

void FullscreenUI::ShutdownArcade()
{
	s_check_cancel.Cancel();
	s_work_cancel.Cancel();
	s_check_worker.Join();
	s_work.Join();

	if (s_service_down)
	{
		s_service_down = false;
		ArcadeInput::SetService(false);
	}

	s_boot = {};
	s_bios = {};
	s_check.reset();
	s_check_pending = false;
	s_check_generation++;
	s_boot_job_active = false;
	s_missing.clear();
	s_layout.reset();
	s_layout_game_id.clear();
	s_setup_page_frame = -100;
	s_setup_dwell.Reset();
	s_game_page_frame = -100;
}

//////////////////////////////////////////////////////////////////////////
// Screenshot harness entry points (FullscreenUIShot.cpp)
//////////////////////////////////////////////////////////////////////////

std::string FullscreenUI::ShotArcadeNames()
{
	return "arcadelaunch, arcadedownload, arcadepick, arcadepickall, arcadeset, arcadesetall, arcadetest, arcadeprops, arcadepause, arcadepausemain";
}

bool FullscreenUI::ShotArcade(std::string_view what, std::string* error)
{
	if (what == "download")
	{
		SwitchToSettings();
		s_settings_page = SettingsPage::Arcade;
		StartBootFilesDownload();
		return true;
	}

	if (what == "pause" || what == "pausemain")
	{
		// Without a VM the menu cannot close itself: this only draws it, with a title in place of a game's.
		if (!VMManager::HasValidVM())
		{
			s_current_game_title = "Tekken 4";
			s_current_game_subtitle = "NM00004 / 00000000";
		}
		s_current_main_window = MainWindowType::PauseMenu;
		s_current_pause_submenu = (what == "pause") ? PauseSubMenu::Arcade : PauseSubMenu::None;
		QueueResetFocus(FocusResetType::WindowChanged);
		return true;
	}

	if (what == "test")
	{
		ArcadeInput::ToggleTest();
		return true;
	}

	// The pause menu's Game Properties, for the running game.
	if (what == "props")
	{
		SwitchToGameSettings();
		return true;
	}

	if (what == "setall" || what == "pickall")
	{
		SwitchToSettings();
		s_settings_page = SettingsPage::Arcade;
		if (what == "setall")
			ApplyGlobalTarget(GIB::Cross, 5); // Cross acts as Circle
		else
			PickGlobalTarget(GIB::Cross);
		return true;
	}

	// The rest start from the first arcade game in the list.
	const GameList::Entry* entry = nullptr;
	GameList::Entry copy;
	{
		auto lock = GameList::GetLock();
		for (u32 i = 0; i < GameList::GetEntryCount() && !entry; i++)
		{
			const GameList::Entry* candidate = GameList::GetEntryByIndex(i);
			if (candidate && candidate->type == GameList::EntryType::Arcade)
			{
				copy = *candidate;
				entry = &copy;
			}
		}
	}
	if (!entry)
	{
		*error = "the game list has no arcade game (open 'rescan' first and wait a few frames)";
		return false;
	}

	if (what == "launch")
	{
		s_current_main_window = MainWindowType::GameList;
		DoStartPath(entry->path);
		return true;
	}
	if (what == "pick" || what == "set")
	{
		SwitchToGameSettings(entry);
		if (what == "pick")
			PickGameJob(entry->serial, GIB::Cross);
		else
			ApplyGameJob(entry->serial, GIB::Square, 5); // Square does the job at index 5 of the list
		return true;
	}

	*error = fmt::format("unknown arcade page '{}'", what);
	return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// Same scheme as FullscreenUI.cpp and FullscreenUI_Settings.cpp: tools/generate_fullscreen_ui_translation_strings.py
// regenerates the block below from the FSUI_* macros above.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "Arcade Boot Files");
TRANSLATE_NOOP("FullscreenUI", "Cancelled.");
TRANSLATE_NOOP("FullscreenUI", "Another arcade job is still running.");
TRANSLATE_NOOP("FullscreenUI", "Downloading...");
TRANSLATE_NOOP("FullscreenUI", "This game");
TRANSLATE_NOOP("FullscreenUI", "Wait for the boot files to finish downloading.");
TRANSLATE_NOOP("FullscreenUI", "Another arcade game is being prepared.");
TRANSLATE_NOOP("FullscreenUI", "Arcade");
TRANSLATE_NOOP("FullscreenUI", "Getting the game ready...");
TRANSLATE_NOOP("FullscreenUI", "Nothing");
TRANSLATE_NOOP("FullscreenUI", "Each arcade game is one of these kinds of cabinet, and the controller works as follows. Select (the Select button) inserts a coin.\n\n");
TRANSLATE_NOOP("FullscreenUI", "Checking for a newer version...");
TRANSLATE_NOOP("FullscreenUI", "A newer version is available.");
TRANSLATE_NOOP("FullscreenUI", "This is the latest version.");
TRANSLATE_NOOP("FullscreenUI", "Could not check for a newer version.");
TRANSLATE_NOOP("FullscreenUI", "Every arcade game starts from a small boot program. They come from the Proverb project (PS2Homebrew-arcade) on GitHub, about 1 MB for all games at once.");
TRANSLATE_NOOP("FullscreenUI", "Every arcade game starts from a small boot program (boot.elf). The PS2Homebrew-arcade project \"Proverb\" builds one for each game, and its nightly release holds all of them in one zip of about 1 MB. It is stored once, and the game you start takes its boot program out of it. Games that come with their own boot.elf use that one.");
TRANSLATE_NOOP("FullscreenUI", "BIOS folder copied to clipboard.");
TRANSLATE_NOOP("FullscreenUI", "Adding Games");
TRANSLATE_NOOP("FullscreenUI", "Put each game's image and its security dongle in one of your game folders (see the game list settings), named after the game's ID: NM00010.chd and NM00010.ps2, for example, or both in a folder named NM00010. The image is a .chd or an unpacked .iso. The dongle is the game's memory card file, a .ps2 or .bin; a packed .bin.gz also works.\n\nNothing is copied except the dongle, into the memory cards folder, the first time the game starts. A game made from a PCSX2x6 template folder, with its own .acgame, works as it is.\n\nThe game list shows a game with what it still lacks, and starting one that is not ready says what to add.");
TRANSLATE_NOOP("FullscreenUI", "Put every button back to the cabinet's own layout in all arcade games? A game's own layout is not changed.");
TRANSLATE_NOOP("FullscreenUI", "Button Layouts");
TRANSLATE_NOOP("FullscreenUI", "A controller button normally does the cabinet job it is given in the game: the D-pad is the lever, the face buttons are the cabinet's buttons, Select inserts a coin. Here you choose, for every arcade game, another button's job for a button. The analog pedals of a driving cabinet stay on their triggers.\n\nA game can have a layout of its own: open the game's settings and look under Arcade.");
TRANSLATE_NOOP("FullscreenUI", "Put every button of this game back to the layout for all arcade games?");
TRANSLATE_NOOP("FullscreenUI", "Coin inserted for player 1.");
TRANSLATE_NOOP("FullscreenUI", "Coin inserted for player 2.");
TRANSLATE_NOOP("FullscreenUI", "How the sticks, triggers and buttons work on each kind of cabinet. Select to read.");
TRANSLATE_NOOP("FullscreenUI", "Boot Files");
TRANSLATE_NOOP("FullscreenUI", "Installed");
TRANSLATE_NOOP("FullscreenUI", "Not Downloaded");
TRANSLATE_NOOP("FullscreenUI", "Reading the boot files you picked.");
TRANSLATE_NOOP("FullscreenUI", "Stops the download. Boot files you already have stay as they are.");
TRANSLATE_NOOP("FullscreenUI", "Downloads the boot programs from GitHub. Needs an internet connection.");
TRANSLATE_NOOP("FullscreenUI", "Replaces the boot files with the newer version. Needs an internet connection.");
TRANSLATE_NOOP("FullscreenUI", "Downloads the boot files again, replacing what is stored.");
TRANSLATE_NOOP("FullscreenUI", "For a device with no internet connection: pick the proverb.zip you downloaded on another device.");
TRANSLATE_NOOP("FullscreenUI", "Arcade BIOS");
TRANSLATE_NOOP("FullscreenUI", "Found");
TRANSLATE_NOOP("FullscreenUI", "Missing");
TRANSLATE_NOOP("FullscreenUI", "Games");
TRANSLATE_NOOP("FullscreenUI", "Where a game's image and dongle go. Select to read.");
TRANSLATE_NOOP("FullscreenUI", "Controls");
TRANSLATE_NOOP("FullscreenUI", "Button Layout For All Arcade Games");
TRANSLATE_NOOP("FullscreenUI", "What each button does on the cabinet. A game's own layout, in its settings, comes first. Select a button to change it.");
TRANSLATE_NOOP("FullscreenUI", "Puts every button back to the cabinet's own layout.");
TRANSLATE_NOOP("FullscreenUI", "Everything is in place. This game starts but only reaches its demo screen so far.");
TRANSLATE_NOOP("FullscreenUI", "Everything this game needs is in place.");
TRANSLATE_NOOP("FullscreenUI", "Arcade Controls");
TRANSLATE_NOOP("FullscreenUI", "Puts every button back to the layout for all arcade games, or the cabinet's own.");
TRANSLATE_NOOP("FullscreenUI", "On");
TRANSLATE_NOOP("FullscreenUI", "Off");
TRANSLATE_NOOP("FullscreenUI", "Installed. They have boot programs for {} games.");
TRANSLATE_NOOP("FullscreenUI", "{} cannot be started yet.");
TRANSLATE_NOOP("FullscreenUI", "{} (changed)");
TRANSLATE_NOOP("FullscreenUI", "{} (Default)");
TRANSLATE_NOOP("FullscreenUI", "What {} Does");
TRANSLATE_NOOP("FullscreenUI", "{} (analog)");
TRANSLATE_NOOP("FullscreenUI", "A newer version is available: {}.");
TRANSLATE_NOOP("FullscreenUI", "{}, built {}");
TRANSLATE_NOOP("FullscreenUI", "Downloaded {}");
TRANSLATE_NOOP("FullscreenUI", ". Boot programs for {} games.");
TRANSLATE_NOOP("FullscreenUI", "Source: {}");
TRANSLATE_NOOP("FullscreenUI", "Stored as: {}");
TRANSLATE_NOOP("FullscreenUI", "Size: {}");
TRANSLATE_NOOP("FullscreenUI", "Games: {}");
TRANSLATE_NOOP("FullscreenUI", "Found: {}. Each game starts on a BIOS it runs on.");
TRANSLATE_NOOP("FullscreenUI", "BIOS folder: {}");
TRANSLATE_NOOP("FullscreenUI", "Missing: {}. Select for what to add.");
TRANSLATE_NOOP("FullscreenUI", "Arcade Game Not Ready");
TRANSLATE_NOOP("FullscreenUI", "Update Boot Files");
TRANSLATE_NOOP("FullscreenUI", "Download Boot Files");
TRANSLATE_NOOP("FullscreenUI", "Close");
TRANSLATE_NOOP("FullscreenUI", "Cabinet Types");
TRANSLATE_NOOP("FullscreenUI", "Installing");
TRANSLATE_NOOP("FullscreenUI", "Cancel");
TRANSLATE_NOOP("FullscreenUI", "Download Again");
TRANSLATE_NOOP("FullscreenUI", "Install From A File");
TRANSLATE_NOOP("FullscreenUI", "Select Boot Files");
TRANSLATE_NOOP("FullscreenUI", "Reset Button Layout");
TRANSLATE_NOOP("FullscreenUI", "About Button Layouts");
TRANSLATE_NOOP("FullscreenUI", "Not Ready To Play");
TRANSLATE_NOOP("FullscreenUI", "Attract Mode Only");
TRANSLATE_NOOP("FullscreenUI", "Ready To Play");
TRANSLATE_NOOP("FullscreenUI", "Cabinet Type");
TRANSLATE_NOOP("FullscreenUI", "Reset Arcade Controls");
TRANSLATE_NOOP("FullscreenUI", "Back To Pause Menu");
TRANSLATE_NOOP("FullscreenUI", "Test Switch");
TRANSLATE_NOOP("FullscreenUI", "Insert Coin (Player 1)");
TRANSLATE_NOOP("FullscreenUI", "Insert Coin (Player 2)");
TRANSLATE_NOOP("FullscreenUI", "Service Button");
// TRANSLATION-STRING-AREA-END
#endif
