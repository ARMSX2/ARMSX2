// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture for memory card backups and autosaves (pcsx2/SIO/Memcard/MemcardBackup.h and
// pcsx2/Autosave.h hold the logic). The other FullscreenUI files call in here with one line each
// (declarations at the end of FullscreenUI_Internal.h); everything longer lives here, so an
// upstream rework of those pages touches these features in a handful of places.
//
//   - Memory card page: the Automatic Backups toggle and the row that opens the backups window.
//   - The backups window (MainWindowType::MemcardBackups): cards, their snapshots, Restore, Back Up Now.
//   - The boot-time question "this card is unreadable, restore the backup?".
//   - Settings: autosave interval, how many to keep, autosave on shutdown.
//   - The autosave tiles in the Load State selector.

#include "Achievements.h"
#include "Autosave.h"
#include "Catalog/BlockingAnswer.h"
#include "Config.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSHeadlessPresent.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "ImGui/FullscreenUI_Internal.h"
#include "ImGui/ImGuiFullscreen.h"
#include "Input/InputManager.h"
#include "MTGS.h"
#include "Patch.h"
#include "SIO/Memcard/MemcardBackup.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SaveState.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/SettingsInterface.h"

#include "IconsFontAwesome.h"
#include "IconsPromptFont.h"

#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iterator>
#include <thread>

namespace Backup = MemcardBackup;

namespace
{
	using namespace FullscreenUI;

	constexpr const char* MEMCARD_NOTIFICATION_KEY = "memcard_backup";

	/// How long the boot-time restore question waits for an answer before it starts the game anyway.
	constexpr std::chrono::seconds RESTORE_PROMPT_TIMEOUT{60};

	// A worker thread's only way to reach the GS thread: only the CPU thread may push to the GS ring
	// (MTGS::RunOnGSThread asserts otherwise), so hop through it. Neither step waits for anything.
	void PostToGSThread(std::function<void()> function)
	{
		Host::RunOnCPUThread([function = std::move(function)]() mutable {
			if (MTGS::IsOpen())
				MTGS::RunOnGSThread(std::move(function));
		});
	}

	std::string FormatTime(s64 milliseconds_since_epoch)
	{
		return std::string(TimeToPrintableString(static_cast<time_t>(milliseconds_since_epoch / 1000)).c_str());
	}

	std::string FormatSize(u64 bytes)
	{
		if (bytes >= 1024 * 1024)
			return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
		return fmt::format("{} KB", (bytes + 1023) / 1024);
	}

	void ShowNotice(const std::string& title, const std::string& text, float seconds)
	{
		ImGuiFullscreen::AddNotification(MEMCARD_NOTIFICATION_KEY, seconds, fmt::format("{} {}", ICON_PF_MEMORY_CARD, title), text, std::string());
	}

	////////////////////////////////////////////////////////////////////////
	// The backups window
	//
	// All of this state belongs to the GS thread. Reading the snapshots opens a zip each and a backup
	// compresses a whole card, so both run on a worker thread that hands its result back with
	// PostToGSThread. One job at a time.
	////////////////////////////////////////////////////////////////////////

	struct BackupCard
	{
		std::string name;
		int slot = 0; ///< 1 or 2 when this is the card in that slot, else 0
		Backup::Health health = Backup::Health::Missing;
		std::vector<Backup::Snapshot> snapshots; ///< newest first
	};

	struct BackupsView
	{
		bool loaded = false; ///< `cards` holds a listing
		bool busy = false; ///< a worker job has not reported back yet
		std::vector<BackupCard> cards;
	};

	BackupsView s_view;
	std::thread s_worker;

	void StartWorker(std::function<void()> body)
	{
		// The previous job has handed in its result (busy was false), so this returns at once.
		if (s_worker.joinable())
			s_worker.join();
		s_worker = std::thread(std::move(body));
	}

	// Worker thread. The cards in slots 1 and 2 (if they exist) come first, then every other card
	// that has backups, including ones since deleted: their backups are still restorable.
	std::vector<BackupCard> BuildCardList()
	{
		const Backup::Store store = Backup::Store::ForDataRoot();
		std::vector<BackupCard> cards;

		const auto known = [&cards](const std::string& name) {
			return std::any_of(cards.begin(), cards.end(), [&name](const BackupCard& c) { return c.name == name; });
		};

		for (int slot = 1; slot <= 2; slot++)
		{
			const bool enabled = Host::GetBaseBoolSettingValue("MemoryCards", fmt::format("Slot{}_Enable", slot).c_str(), true);
			const std::string name = Host::GetBaseStringSettingValue(
				"MemoryCards", fmt::format("Slot{}_Filename", slot).c_str(), FileMcd_GetDefaultName(slot - 1).c_str());
			if (!enabled || !Backup::IsPlainCardName(name) || known(name) ||
				Backup::Verify(store.CardPath(name)) == Backup::Health::Missing)
			{
				continue;
			}

			BackupCard card;
			card.name = name;
			card.slot = slot;
			cards.push_back(std::move(card));
		}

		for (const std::string& name : store.CardsWithBackups())
		{
			if (known(name))
				continue;
			BackupCard card;
			card.name = name;
			cards.push_back(std::move(card));
		}

		for (BackupCard& card : cards)
		{
			card.health = Backup::Verify(store.CardPath(card.name));
			card.snapshots = store.List(card.name);
		}
		return cards;
	}

	void ReloadCards()
	{
		if (s_view.busy)
			return;
		s_view.busy = true;

		StartWorker([]() {
			std::vector<BackupCard> cards = BuildCardList();
			PostToGSThread([cards = std::move(cards)]() mutable {
				s_view.cards = std::move(cards);
				s_view.loaded = true;
				s_view.busy = false;
			});
		});
	}

	// Runs a service call that changes cards (a backup or a restore) off the GS thread, tells the user
	// how it went, and reads the listing again.
	void RunCardJob(const std::string& working_title, const std::string& working_text, std::function<Backup::Outcome()> job)
	{
		if (s_view.busy)
			return;
		s_view.busy = true;
		ShowNotice(working_title, working_text, 120.0f);

		StartWorker([job = std::move(job)]() {
			const Backup::Outcome outcome = job();
			std::vector<BackupCard> cards = BuildCardList();
			PostToGSThread([outcome, cards = std::move(cards)]() mutable {
				if (IsInitialized())
				{
					ShowNotice(outcome.ok ? FSUI_STR("Memory Card Backups") : FSUI_STR("Memory Card Backup Failed"), outcome.detail, 8.0f);
				}
				s_view.cards = std::move(cards);
				s_view.loaded = true;
				s_view.busy = false;
			});
		});
	}

	void ConfirmRestore(const Backup::Snapshot& snapshot)
	{
		std::string message = fmt::format(FSUI_FSTR("Replace {0} with the backup from {1}?\n\nThe card as it is now is backed up first, so this can be undone."),
			snapshot.card, FormatTime(snapshot.taken_at_ms));
		if (!snapshot.healthy)
		{
			message += "\n\n";
			message += FSUI_STR("This backup was taken when the card could not be read, so restoring it may not help.");
		}

		OpenConfirmMessageDialog(
			FSUI_ICONSTR(ICON_FA_CLOCK_ROTATE_LEFT, "Restore Backup"), std::move(message),
			[snapshot](bool yes) {
				if (!yes)
					return;
				RunCardJob(FSUI_STR("Restoring Memory Card"), fmt::format(FSUI_FSTR("Restoring {}..."), snapshot.card),
					[snapshot]() { return Backup::RestoreSnapshot(snapshot); });
			},
			false, FSUI_ICONSTR(ICON_FA_CHECK, "Restore"), FSUI_ICONSTR(ICON_FA_XMARK, "Cancel"));
	}

	const char* ReasonLabel(Backup::Reason reason)
	{
		switch (reason)
		{
			case Backup::Reason::Session:
				return FSUI_CSTR("Taken when a game started");
			case Backup::Reason::Save:
				return FSUI_CSTR("Taken on save");
			case Backup::Reason::Manual:
				return FSUI_CSTR("Backed up by hand");
			case Backup::Reason::PreRestore:
				return FSUI_CSTR("Kept before a restore");
		}
		return "";
	}

	void CloseBackupsWindow()
	{
		s_current_main_window = MainWindowType::Settings;
		QueueResetFocus(FocusResetType::WindowChanged);
	}

	////////////////////////////////////////////////////////////////////////
	// The boot-time question
	////////////////////////////////////////////////////////////////////////

	void OpenRestorePromptDialog(const Backup::RestorePrompt& prompt, ImGuiFullscreen::ConfirmMessageDialogCallback callback)
	{
		std::string message = fmt::format(
			FSUI_FSTR("The memory card in slot {0}, {1}, cannot be read: it is damaged or no longer a memory card.\n\n"
					  "A backup of it from {2}, taken when it was in good order, is available. Restore it before the game starts?\n\n"
					  "Restoring saves the card as it is now as a backup first. Starting anyway leaves the card as it is."),
			prompt.slot, prompt.card, FormatTime(prompt.snapshot.taken_at_ms));

		OpenConfirmMessageDialog(FSUI_ICONSTR(ICON_PF_MEMORY_CARD, "Memory Card Cannot Be Read"), std::move(message), std::move(callback), true,
			FSUI_ICONSTR(ICON_FA_CLOCK_ROTATE_LEFT, "Restore Backup"), FSUI_ICONSTR(ICON_FA_PLAY, "Start Anyway"));
	}

	// Called by VMManager::Initialize on the CPU thread, with the cards closed. The comment above
	// RegisterMemcardRestorePrompt() below explains why this cannot deadlock.
	Backup::RestoreAnswer AskToRestoreCard(const Backup::RestorePrompt& prompt)
	{
		using Answer = Backup::RestoreAnswer;
		using Question = Catalog::BlockingAnswer<Answer>;

		// A Qt session with Big Picture closed never gets here (the handler is registered while Big Picture
		// is running); this covers the window in which it is being torn down.
		if (!MTGS::IsOpen() || !IsInitialized())
			return Answer::StartAnyway;

		// A headless run (armsx2-sdl --headless, the screenshot tools) has no one to press a button, and a
		// boot waiting for one would never finish.
		if (GSHeadless::IsEnabled())
		{
			Backup::ReportUnansweredRestore(prompt.card);
			return Answer::StartAnyway;
		}

		const auto question = std::make_shared<Question>();
		MTGS::RunOnGSThread([responder = Question::Responder(question, Answer::StartAnyway), prompt]() {
			// Big Picture went away before this ran: the responder dies with this lambda and answers.
			if (!IsInitialized())
				return;

			OpenRestorePromptDialog(prompt, [responder](bool restore) { responder(restore ? Answer::Restore : Answer::StartAnyway); });
		});

		// The GS thread keeps drawing while the VM is starting (MTGS's idle loop, because Big Picture set
		// run-idle), but input reaches Big Picture through the CPU thread, which is the thread waiting.
		// Do here what its loop would between frames: run what other threads queued for it (the Qt host
		// delivers keyboard and mouse presses that way) and poll the controllers.
		// A boot started by a script or a launcher may have nobody at the controller: it waits a minute, then
		// starts with the card as it is, as it does for any host that cannot ask.
		bool timed_out = false;
		const Answer answer = question->WaitFor(RESTORE_PROMPT_TIMEOUT, Answer::StartAnyway, std::chrono::milliseconds(16), []() {
			Host::PumpMessagesOnCPUThread();
			InputManager::PollSources();
			return MTGS::IsOpen() && IsInitialized();
		}, &timed_out);

		if (timed_out)
		{
			Backup::ReportUnansweredRestore(prompt.card);
			// The dialog is still up; it must not wait on for a question that has been answered.
			MTGS::RunOnGSThread([]() {
				if (IsInitialized())
					ImGuiFullscreen::CloseMessageDialog();
			});
		}

		if (answer == Answer::Restore)
		{
			Host::AddIconOSDMessage("MemcardRestore", ICON_PF_MEMORY_CARD,
				fmt::format(FSUI_FSTR("Restoring {} from a backup..."), prompt.card), Host::OSD_INFO_DURATION);
		}
		return answer;
	}

	////////////////////////////////////////////////////////////////////////
	// Autosave settings
	////////////////////////////////////////////////////////////////////////

	constexpr int AUTOSAVE_INTERVAL_CHOICES[] = {0, 1, 2, 5, 10, 15, 30};

	std::string AutosaveIntervalLabel(int minutes)
	{
		if (minutes <= 0)
			return FSUI_STR("Off");
		if (minutes == 1)
			return FSUI_STR("Every Minute");
		return fmt::format(FSUI_FSTR("Every {} Minutes"), minutes);
	}

	void DrawAutosaveIntervalSetting(SettingsInterface* bsi)
	{
		const int current = std::max(bsi->GetIntValue("EmuCore", "AutosaveIntervalMinutes", 0), 0);
		const std::string current_label = AutosaveIntervalLabel(current);

		if (!MenuButtonWithValue(FSUI_ICONSTR(ICON_FA_STOPWATCH, "Autosave Interval"),
				FSUI_CSTR("Saves the game by itself while you play, apart from your save slots. Load an autosave from the Load State menu."),
				current_label.c_str()))
		{
			return;
		}

		// A value set elsewhere (the Android app's slider allows any number of minutes) is kept and listed.
		std::vector<int> values(std::begin(AUTOSAVE_INTERVAL_CHOICES), std::end(AUTOSAVE_INTERVAL_CHOICES));
		if (std::find(values.begin(), values.end(), current) == values.end())
		{
			values.push_back(current);
			std::sort(values.begin(), values.end());
		}

		ImGuiFullscreen::ChoiceDialogOptions options;
		for (const int minutes : values)
			options.emplace_back(AutosaveIntervalLabel(minutes), minutes == current);

		OpenChoiceDialog(FSUI_ICONSTR(ICON_FA_STOPWATCH, "Autosave Interval"), false, std::move(options),
			[values = std::move(values)](s32 index, const std::string& title, bool checked) {
				if (index >= 0 && static_cast<size_t>(index) < values.size())
				{
					auto lock = Host::GetSettingsLock();
					SettingsInterface* bsi = GetEditingSettingsInterface(false);
					bsi->SetIntValue("EmuCore", "AutosaveIntervalMinutes", values[index]);
					SetSettingsChanged(bsi);
				}
				CloseChoiceDialog();
			});
	}

	////////////////////////////////////////////////////////////////////////
	// Autosave tiles in the Load State selector
	////////////////////////////////////////////////////////////////////////

	// The slot number an autosave tile carries. The selector hands entry.slot to DoLoadState and to the
	// "failed to load" message, so these are the real slot for the newest (VMManager::SAVESTATE_SLOT_AUTOSAVE,
	// -2) and -3 .. -6 for the older ones; IsAutosaveSlot() recognises all of them.
	s32 SlotForAutosave(int index)
	{
		return index <= 1 ? VMManager::SAVESTATE_SLOT_AUTOSAVE : static_cast<s32>(-1 - index);
	}

	// InitializeSaveStateListEntry() in FullscreenUI.cpp builds the tile for a numbered slot from the
	// game's serial and CRC; an autosave is named differently and the older ones have no slot number at
	// all, so this fills the same tile from a path. The thumbnail part is the same: the device texture
	// pool is shared with the GS back thread, so drain it first.
	bool InitializeAutosaveListEntry(SaveStateListEntry* li, const Autosave::Entry& autosave)
	{
		li->title = fmt::format("{}##game_slot_{}",
			autosave.index <= 1 ? FSUI_STR("Autosave") : fmt::format(FSUI_FSTR("Autosave {}"), autosave.index), SlotForAutosave(autosave.index));
		li->summary = fmt::format(FSUI_FSTR("Saved {}"), TimeToPrintableString(autosave.modified));
		li->slot = SlotForAutosave(autosave.index);
		li->timestamp = autosave.modified;
		li->path = autosave.path;

		GSDrainBackQueue();
		li->preview_texture.reset();

		u32 screenshot_width, screenshot_height;
		std::vector<u32> screenshot_pixels;
		if (SaveState_ReadScreenshot(li->path, &screenshot_width, &screenshot_height, &screenshot_pixels))
		{
			li->preview_texture =
				std::unique_ptr<GSTexture>(g_gs_device->CreateTexture(screenshot_width, screenshot_height, 1, GSTexture::Format::Color));
			if (!li->preview_texture || !li->preview_texture->Update(GSVector4i(0, 0, screenshot_width, screenshot_height),
											screenshot_pixels.data(), sizeof(u32) * screenshot_width))
			{
				Console.Error("Failed to upload autosave image to GPU");
				if (li->preview_texture)
					g_gs_device->Recycle(li->preview_texture.release());
			}
		}
		return true;
	}
} // namespace

//////////////////////////////////////////////////////////////////////////
// Memory card page
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::DrawMemcardBackupSettings(SettingsInterface* bsi)
{
	DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_CLOCK_ROTATE_LEFT, "Automatic Backups"),
		FSUI_CSTR("Keeps copies of your memory cards, taken when a game starts, so a damaged card can be put back."), "MemoryCards",
		"AutoBackup", MemcardBackup::AutoBackupDefault());

	if (MenuButton(FSUI_ICONSTR(ICON_FA_BOX_ARCHIVE, "Memory Card Backups"),
			FSUI_CSTR("Restore a memory card from one of its backups, or back one up now.")))
	{
		OpenMemcardBackupsWindow();
	}
}

//////////////////////////////////////////////////////////////////////////
// The backups window
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::OpenMemcardBackupsWindow()
{
	s_current_main_window = MainWindowType::MemcardBackups;
	QueueResetFocus(FocusResetType::WindowChanged);
	ReloadCards();
}

void FullscreenUI::DrawMemcardBackupsWindow()
{
	ImGuiIO& io = ImGui::GetIO();
	const ImVec2 heading_size =
		ImVec2(io.DisplaySize.x, LayoutScale(LAYOUT_MENU_BUTTON_HEIGHT_NO_SUMMARY) + (LayoutScale(LAYOUT_MENU_BUTTON_Y_PADDING) * 2.0f) + LayoutScale(2.0f));
	const float bg_alpha = VMManager::HasValidVM() ? 0.90f : 1.0f;

	if (BeginFullscreenWindow(ImVec2(0.0f, 0.0f), heading_size, "memcard_backups_nav", MulAlpha(UIPrimaryColor, bg_alpha)))
	{
		BeginNavBar();
		if (NavButton(ICON_PF_BACKWARD, true, true))
			CloseBackupsWindow();
		NavTitle(FSUI_CSTR("Memory Card Backups"));
		EndNavBar();
	}
	EndFullscreenWindow();

	// One window id per view: the listing replaces the loading message, and a stale remembered
	// selection would leave the controller with nothing focused.
	if (!BeginFullscreenWindow(ImVec2(0.0f, heading_size.y),
			ImVec2(io.DisplaySize.x, io.DisplaySize.y - heading_size.y - LayoutScale(LAYOUT_FOOTER_HEIGHT)),
			s_view.loaded ? "memcard_backups_list" : "memcard_backups_loading", MulAlpha(UIBackgroundColor, bg_alpha), 0.0f,
			ImVec2(ImGuiFullscreen::LAYOUT_MENU_WINDOW_X_PADDING, 0.0f)))
	{
		EndFullscreenWindow();
		return;
	}

	if (ImGui::IsWindowFocused() && WantsToCloseMenu())
		CloseBackupsWindow();

	// The service refuses both while any VM exists: the console keeps its own picture of the card.
	const bool vm_active = (VMManager::GetState() != VMState::Shutdown);
	const bool can_act = !vm_active && !s_view.busy;

	BeginMenuButtons();

	if (!s_view.loaded)
	{
		MenuButton(FSUI_ICONSTR(ICON_FA_HOURGLASS_HALF, "Loading Backups"), FSUI_CSTR("Reading the backup folder..."), false);
	}
	else
	{
		if (vm_active)
		{
			MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "A Game Is Running"),
				FSUI_CSTR("Close the game to restore or back up a memory card."), false);
		}

		if (s_view.cards.empty())
		{
			MenuButton(FSUI_ICONSTR(ICON_FA_CIRCLE_INFO, "No Backups Yet"),
				FSUI_CSTR("A backup is made the first time a game starts with Automatic Backups turned on."), false);
		}

		for (const BackupCard& card : s_view.cards)
		{
			if (card.slot != 0)
				MenuHeading(fmt::format(FSUI_FSTR("{0} (Slot {1})"), card.name, card.slot).c_str());
			else
				MenuHeading(card.name.c_str());

			const bool backed_up_ok = card.health == Backup::Health::Good;
			// The text before ## is shown; the part after it keeps the id of each card's row apart.
			if (MenuButton(fmt::format("{} {}##backup_{}", ICON_FA_BOX_ARCHIVE, FSUI_VSTR("Back Up Now"), card.name).c_str(),
					backed_up_ok ? FSUI_CSTR("Saves a copy of this card as it is now.") :
					card.health == Backup::Health::Missing ?
								   FSUI_CSTR("This card no longer exists. Restore one of its backups below to put it back.") :
								   FSUI_CSTR("This card cannot be read, so there is nothing worth backing up."),
					can_act && backed_up_ok))
			{
				RunCardJob(FSUI_STR("Backing Up Memory Card"), fmt::format(FSUI_FSTR("Backing up {}..."), card.name),
					[name = card.name]() { return Backup::BackUpCardNow(name); });
			}

			if (card.snapshots.empty())
			{
				MenuButton(fmt::format("{} {}##nobackups_{}", ICON_FA_CIRCLE_INFO, FSUI_VSTR("No Backups Of This Card"), card.name).c_str(),
					FSUI_CSTR("One is made when a game next starts with this card in a slot."), false);
			}

			for (const Backup::Snapshot& snapshot : card.snapshots)
			{
				const std::string title = fmt::format("{} {}##snap_{}_{}", snapshot.healthy ? ICON_FA_CLOCK_ROTATE_LEFT : ICON_FA_TRIANGLE_EXCLAMATION,
					FormatTime(snapshot.taken_at_ms), card.name, snapshot.taken_at_ms);
				const std::string summary = snapshot.game.empty() ?
				                                fmt::format("{} - {}", ReasonLabel(snapshot.reason), FormatSize(snapshot.size_bytes)) :
				                                fmt::format("{} - {} - {}", ReasonLabel(snapshot.reason), FormatSize(snapshot.size_bytes), snapshot.game);

				if (MenuButtonWithValue(title.c_str(), summary.c_str(),
						snapshot.healthy ? FSUI_CSTR("Verified") : FSUI_CSTR("Unreadable when saved"), can_act))
				{
					ConfirmRestore(snapshot);
				}
			}
		}
	}

	EndMenuButtons();
	EndFullscreenWindow();

	SetStandardSelectionFooterText(true);
}

//////////////////////////////////////////////////////////////////////////
// The boot-time question
//
// THREADING. Backup::OnCardsReopening() calls the handler on the CPU thread, inside
// VMManager::Initialize, and boot waits for the return value. The question is drawn and answered on
// the GS thread. Nothing in between may need the CPU thread to run something, because it is the thread
// that is waiting:
//
//   CPU thread (in the handler)               GS thread
//   ---------------------------               ---------
//   MTGS::RunOnGSThread(open the dialog) ---> draws the dialog every frame: MTGS's idle loop keeps
//   BlockingAnswer::Wait(16 ms steps):          presenting while the VM is not Running, because Big
//     InputManager::PollSources()               Picture set run-idle. The window and the GS are already
//     still open? else give up                  open: the question is only asked when Big Picture is up.
//                                          <--- the button callback (or the dialog being replaced, or
//   returns Restore / StartAnyway                Big Picture shutting down) sets the answer.
//
// Pushing onto the GS ring from the CPU thread is the one allowed direction. Nothing here calls
// Host::RunOnCPUThread with block = true, and the dialog callback only sets the answer. The pieces that
// would otherwise hang the CPU thread forever are each covered: the dialog replaced by another
// (OpenConfirmMessageDialog drops the old callback unanswered) or cleared at shutdown destroys the
// callback, and BlockingAnswer::Responder answers "start anyway" when its last copy goes; the GS thread
// closing, or Big Picture shutting down, is seen by the keep-waiting check every 16 ms.
//
// Input is the one thing that does need this thread. Gamepads are polled by whoever runs the CPU thread's
// loop, and the Qt host hands keyboard and mouse presses to the CPU thread as queued tasks; the loop is
// blocked here, so the wait runs both itself every 16 ms (Host::PumpMessagesOnCPUThread and
// InputManager::PollSources), as the Qt host's progress dialog does while a VM starts. Nothing waits for
// that to happen on some other thread.
//
// Precedence. The handler is registered as the *fallback* (MemcardBackup::SetFallbackRestorePromptHandler),
// while Big Picture is initialised. A handler the host registers with SetRestorePromptHandler (the
// Qt main window's dialog) always wins, whichever was registered first, so Qt asks with its own dialog
// whenever its main window exists and Big Picture asks only when it is the one UI (armsx2-sdl). With Big
// Picture not initialised there is no fallback and boot goes ahead with the card as it is: for example
// a game started from the command line, where Big Picture does not exist until the GS opens, after the
// cards were already looked at.
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::RegisterMemcardRestorePrompt()
{
	Backup::SetFallbackRestorePromptHandler(&AskToRestoreCard);
}

void FullscreenUI::ShutdownSavesUI()
{
	// Before anything else goes: a boot waiting on the dialog gets its answer from the dialog
	// callbacks being destroyed (ImGuiFullscreen::Shutdown) or from its own check on IsInitialized().
	Backup::SetFallbackRestorePromptHandler(Backup::RestorePromptHandler());

	// A card job in flight finishes first: stopping it halfway through a card swap is worse than
	// waiting. It ends in PostToGSThread, which does not wait for this thread.
	if (s_worker.joinable())
		s_worker.join();
	s_view = BackupsView();
}

void FullscreenUI::OpenMemcardPromptPreview()
{
	Backup::RestorePrompt prompt;
	prompt.slot = 1;
	prompt.card = "Mcd001.ps2";
	prompt.snapshot.card = prompt.card;
	// A fixed time (18:30 UTC on 2 October 2026; shown in the local time zone), so a screenshot of this
	// dialog does not change from day to day.
	prompt.snapshot.taken_at_ms = 1790965800000;
	prompt.snapshot.healthy = true;
	OpenRestorePromptDialog(prompt, [](bool) {});
}

//////////////////////////////////////////////////////////////////////////
// Autosaves
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::DrawAutosaveSettings(SettingsInterface* bsi)
{
	DrawAutosaveIntervalSetting(bsi);

	static constexpr const char* s_keep_options[] = {"1", "2", "3", "4", "5"};
	static_assert(std::size(s_keep_options) == Autosave::KEEP_MAX - Autosave::KEEP_MIN + 1);
	DrawIntListSetting(bsi, FSUI_ICONSTR(ICON_FA_LAYER_GROUP, "Autosaves to Keep"),
		FSUI_CSTR("How many autosaves to keep for each game. The oldest is deleted when a new one is saved."), "EmuCore", "AutosaveKeep",
		Autosave::KEEP_DEFAULT, s_keep_options, std::size(s_keep_options), false, Autosave::KEEP_MIN);

	DrawToggleSetting(bsi, FSUI_ICONSTR(ICON_FA_POWER_OFF, "Autosave On Shutdown"),
		FSUI_CSTR("Also saves an autosave when the game is closed. Separate from Save State On Shutdown, which keeps one state to resume from."),
		"EmuCore", "AutosaveOnShutdown", false);
}

bool FullscreenUI::IsAutosaveSlot(s32 slot)
{
	return slot <= VMManager::SAVESTATE_SLOT_AUTOSAVE;
}

void FullscreenUI::AppendAutosaveListEntries(const std::string& serial, u32 crc)
{
	// Load menus only: nothing may be saved into an autosave, so the Save State selector has none.
	if (!s_save_state_selector_loading)
		return;

	for (const Autosave::Entry& autosave : Autosave::List(serial.c_str(), crc))
	{
		SaveStateListEntry li;
		if (InitializeAutosaveListEntry(&li, autosave))
			s_save_state_selector_slots.push_back(std::move(li));
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// Same scheme as FullscreenUI.cpp and FullscreenUI_Settings.cpp: tools/generate_fullscreen_ui_translation_strings.py
// regenerates the block below from the FSUI_* macros above.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "Memory Card Backups");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Backup Failed");
TRANSLATE_NOOP("FullscreenUI", "This backup was taken when the card could not be read, so restoring it may not help.");
TRANSLATE_NOOP("FullscreenUI", "Restoring Memory Card");
TRANSLATE_NOOP("FullscreenUI", "Off");
TRANSLATE_NOOP("FullscreenUI", "Every Minute");
TRANSLATE_NOOP("FullscreenUI", "Autosave");
TRANSLATE_NOOP("FullscreenUI", "Backing Up Memory Card");
TRANSLATE_NOOP("FullscreenUI", "Taken when a game started");
TRANSLATE_NOOP("FullscreenUI", "Taken on save");
TRANSLATE_NOOP("FullscreenUI", "Backed up by hand");
TRANSLATE_NOOP("FullscreenUI", "Kept before a restore");
TRANSLATE_NOOP("FullscreenUI", "Saves the game by itself while you play, apart from your save slots. Load an autosave from the Load State menu.");
TRANSLATE_NOOP("FullscreenUI", "Keeps copies of your memory cards, taken when a game starts, so a damaged card can be put back.");
TRANSLATE_NOOP("FullscreenUI", "Restore a memory card from one of its backups, or back one up now.");
TRANSLATE_NOOP("FullscreenUI", "Reading the backup folder...");
TRANSLATE_NOOP("FullscreenUI", "Close the game to restore or back up a memory card.");
TRANSLATE_NOOP("FullscreenUI", "A backup is made the first time a game starts with Automatic Backups turned on.");
TRANSLATE_NOOP("FullscreenUI", "Saves a copy of this card as it is now.");
TRANSLATE_NOOP("FullscreenUI", "This card no longer exists. Restore one of its backups below to put it back.");
TRANSLATE_NOOP("FullscreenUI", "This card cannot be read, so there is nothing worth backing up.");
TRANSLATE_NOOP("FullscreenUI", "One is made when a game next starts with this card in a slot.");
TRANSLATE_NOOP("FullscreenUI", "Verified");
TRANSLATE_NOOP("FullscreenUI", "Unreadable when saved");
TRANSLATE_NOOP("FullscreenUI", "How many autosaves to keep for each game. The oldest is deleted when a new one is saved.");
TRANSLATE_NOOP("FullscreenUI", "Also saves an autosave when the game is closed. Separate from Save State On Shutdown, which keeps one state to resume from.");
TRANSLATE_NOOP("FullscreenUI", "Replace {0} with the backup from {1}?\n\nThe card as it is now is backed up first, so this can be undone.");
TRANSLATE_NOOP("FullscreenUI", "Restoring {}...");
TRANSLATE_NOOP("FullscreenUI", "The memory card in slot {0}, {1}, cannot be read: it is damaged or no longer a memory card.\n\nA backup of it from {2}, taken when it was in good order, is available. Restore it before the game starts?\n\nRestoring saves the card as it is now as a backup first. Starting anyway leaves the card as it is.");
TRANSLATE_NOOP("FullscreenUI", "Restoring {} from a backup...");
TRANSLATE_NOOP("FullscreenUI", "Every {} Minutes");
TRANSLATE_NOOP("FullscreenUI", "Autosave {}");
TRANSLATE_NOOP("FullscreenUI", "Saved {}");
TRANSLATE_NOOP("FullscreenUI", "{0} (Slot {1})");
TRANSLATE_NOOP("FullscreenUI", "Backing up {}...");
TRANSLATE_NOOP("FullscreenUI", "Back Up Now");
TRANSLATE_NOOP("FullscreenUI", "No Backups Of This Card");
TRANSLATE_NOOP("FullscreenUI", "Restore Backup");
TRANSLATE_NOOP("FullscreenUI", "Restore");
TRANSLATE_NOOP("FullscreenUI", "Cancel");
TRANSLATE_NOOP("FullscreenUI", "Memory Card Cannot Be Read");
TRANSLATE_NOOP("FullscreenUI", "Start Anyway");
TRANSLATE_NOOP("FullscreenUI", "Autosave Interval");
TRANSLATE_NOOP("FullscreenUI", "Automatic Backups");
TRANSLATE_NOOP("FullscreenUI", "Loading Backups");
TRANSLATE_NOOP("FullscreenUI", "A Game Is Running");
TRANSLATE_NOOP("FullscreenUI", "No Backups Yet");
TRANSLATE_NOOP("FullscreenUI", "Autosaves to Keep");
TRANSLATE_NOOP("FullscreenUI", "Autosave On Shutdown");
// TRANSLATION-STRING-AREA-END
#endif
