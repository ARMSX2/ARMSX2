// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Catalog/FileOps.h"

#include "common/Pcsx2Types.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Error;

// Rolling backups of the memory cards, so a card that turns up damaged can be put back as it was.
//
// A file memory card is written straight through to disk as the game plays, so the card on disk is
// the only copy there is. This keeps a short history of snapshots of each card, taken while no card
// is open (see "When snapshots are taken"), and can put one back.
//
// Snapshots live in <DataRoot>/memcard-backups/<card name>/, a sibling of the memory cards folder so
// the card lists never see them. Each is a plain zip, named <milliseconds since epoch>-<reason>.zip
// (e.g. 1760000000000-session.zip), holding a manifest "armsx2-mcbak.json" and the card under
// "card/" (one file for a file card; every file of the folder for a folder card). The format is the
// Android app's (MemoryCardBackup.kt), so either can read the other's snapshots.
//
// When snapshots are taken:
//   Automatically, when the VM boots: FileMcd_Reopen calls OnCardsReopening() after the cards are
//   closed and before they are opened again, so the copy cannot catch a half-finished write. Only
//   slot 1 and 2 cards are covered. A card is snapshotted only if it verifies (Verify() == Good) and
//   its SHA-256 differs from the newest snapshot's, so a boot that changed nothing costs nothing.
//   The setting [MemoryCards] AutoBackup (AutoBackupDefault()) turns this off. A reopen of a running
//   game (the memory card folder or a card changed in the settings, which runs under the settings
//   lock) takes no snapshot: hashing and compressing both cards there would stall the CPU thread.
//   By hand, with Store::BackUp() or BackUpNow(), and before every restore.
//
// Which snapshots are kept (Store::Prune, run after every automatic or manual snapshot): the
// newest 3; plus the newest of each of the next 4 distinct local calendar days; plus the newest
// healthy one however old. The last rule is what makes the history safe: a card that breaks is
// never snapshotted, and the last good copy is never pruned however many times the game is
// launched.
//
// What "verify" can and cannot see: a card passes if its header says it is a formatted PS2 or PS1
// card and it is at least 128 KiB, or, for a folder card, it contains _pcsx2_superblock. It cannot
// see a damaged save inside a valid card. That is why the UIs must offer restoring by hand and not
// only when a boot finds an unreadable card.
//
// Threading: Store functions block on file I/O (a snapshot reads and compresses the whole card).
// The boot hook runs on the CPU thread; a UI calls everything else from a worker thread.
// Functions that change the backup folder or a card are serialised by an internal lock.
namespace MemcardBackup
{
	enum class Health
	{
		Good,
		/// Present but not a usable card (not formatted, too small, folder without a superblock).
		Unreadable,
		Missing,
	};

	enum class Reason
	{
		/// Taken automatically when the VM booted.
		Session,
		/// Declared by the Android app, never taken by it.
		Save,
		/// "Back up now".
		Manual,
		/// The card as it was before a restore replaced it. May be unhealthy: it is taken
		/// precisely because the card may be broken.
		PreRestore,
	};

	/// "SESSION", "SAVE", "MANUAL", "PRE_RESTORE" as written in the manifest.
	const char* ReasonName(Reason reason);

	struct Snapshot
	{
		/// The zip file.
		std::string path;
		/// The card's name in the memory cards folder ("Mcd001.ps2").
		std::string card;
		/// Milliseconds since the Unix epoch.
		s64 taken_at_ms = 0;
		Reason reason = Reason::Manual;
		/// Size of the card contents (the sum of all files for a folder card), not of the zip.
		u64 size_bytes = 0;
		/// SHA-256 of the card contents, lowercase hex. Empty if the manifest could not be read.
		std::string hash;
		/// The card verified when this was taken. False for a zip whose manifest cannot be read.
		bool healthy = false;
		/// Serial of the game that was starting, or empty.
		std::string game;
		bool is_folder = false;
	};

	struct Outcome
	{
		bool ok = false;
		/// A sentence for the user, e.g. "The current card is in use and could not be replaced."
		std::string detail;
	};

	/// What SnapshotIfChanged() did about one card.
	enum class CardStatus
	{
		/// The card does not exist (the core creates it on first use).
		Absent,
		/// The card exists but does not verify, so it was not snapshotted.
		Unreadable,
		/// Same contents as the newest snapshot.
		Unchanged,
		Snapshotted,
		/// Verified, but the snapshot could not be written.
		Failed,
	};

	// ---- cards ----------------------------------------------------------------------------

	/// Card-level health of the file or folder at `card_path`.
	Health Verify(const std::string& card_path);

	/// SHA-256 of a card, as lowercase hex. A file card hashes its bytes; a folder card hashes, for
	/// each file in the order of the relative path (with '/' separators): the path, a zero byte, the
	/// length as 8 bytes big-endian, then the bytes. Same as the Android app. Returns false and sets
	/// `error` if a file cannot be read.
	bool HashCard(const std::string& card_path, std::string* hex, Error* error = nullptr);

	/// A card name must be a plain file name: no directory part, not "." or "..".
	bool IsPlainCardName(std::string_view name);

	// ---- the backup folder ----------------------------------------------------------------

	class Store
	{
	public:
		/// `cards_dir` is the memory cards folder, `backups_dir` the folder snapshots go in. A restore
		/// swaps cards through names in `cards_dir`, so the new card is always on the same file system.
		Store(std::string cards_dir, std::string backups_dir);

		/// EmuFolders::MemoryCards and <EmuFolders::DataRoot>/memcard-backups.
		static Store ForDataRoot();

		const std::string& CardsDir() const { return m_cards_dir; }
		const std::string& BackupsDir() const { return m_backups_dir; }
		std::string CardPath(const std::string& card) const;

		/// For tests: replaces the clock (milliseconds since the epoch) and the file operations used
		/// to swap a restored card into place.
		void SetClock(std::function<s64()> now_ms) { m_clock = std::move(now_ms); }
		void SetFileOps(Catalog::FileOps* ops) { m_ops = ops; }

		/// The names of cards that have backups, sorted. A deleted card's backups stay listed.
		std::vector<std::string> CardsWithBackups() const;

		/// The snapshots of one card, newest first. A zip with an unreadable manifest is listed,
		/// marked unhealthy.
		std::vector<Snapshot> List(const std::string& card) const;

		/// The newest snapshot taken while the card verified, or nothing.
		std::optional<Snapshot> NewestHealthy(const std::string& card) const;

		/// The automatic path: verifies the card, skips it if its hash equals the newest snapshot's,
		/// otherwise snapshots it and prunes.
		CardStatus SnapshotIfChanged(const std::string& card, Reason reason, const std::string& game);

		/// A snapshot whether or not anything changed ("Back up now"). Refuses a card that does not
		/// verify, because there is nothing worth saving in it and a history slot to lose. Prunes.
		std::optional<Snapshot> BackUpNow(const std::string& card, const std::string& game, std::string* error = nullptr);

		/// Applies the retention rule to one card's snapshots.
		void Prune(const std::string& card);

		/// Puts `snapshot` back over the live card. The card must not be open: the VM must not be
		/// running, because the console keeps its own picture of the card's directory in guest memory
		/// and a swap underneath it leaves that stale. At boot, before the cards open, is safe.
		///
		/// First takes a PRE_RESTORE snapshot of the live card, even a broken one, so restoring the
		/// wrong copy never destroys the evidence; if the card is healthy and that snapshot cannot be
		/// written, the restore is refused. The backup is unpacked beside the card as "<card>.restoring",
		/// checked against its recorded hash, and swapped in through "<card>.previous"; if any step
		/// fails the original card is put back. These names do not end in a card extension, so the
		/// card lists never show them.
		Outcome Restore(const Snapshot& snapshot);

	private:
		std::string BackupDirFor(const std::string& card) const;
		std::optional<Snapshot> WriteSnapshot(const std::string& card, Reason reason, const std::string& game,
			bool healthy, const std::string& hash, std::string* error);
		void PruneLocked(const std::string& card);
		s64 Now() const;

		std::string m_cards_dir;
		std::string m_backups_dir;
		std::function<s64()> m_clock;
		Catalog::FileOps* m_ops;
	};

	// ---- the boot prompt ------------------------------------------------------------------

	/// What the user is asked at boot: this card (the one in slot `slot`, 1 or 2) does not verify and
	/// `snapshot`, the newest healthy backup of it, could be put back.
	struct RestorePrompt
	{
		int slot = 0;
		std::string card;
		Snapshot snapshot;
	};

	enum class RestoreAnswer
	{
		/// Put the snapshot back over the card, then boot.
		Restore,
		/// Boot with the card as it is.
		StartAnyway,
	};

	/// Called for each card that fails to verify and has a healthy backup, while the VM is starting
	/// and no card is open yet. The return value is the answer; boot waits for it.
	///
	/// THREADING. The handler is called on the CPU thread and the CPU thread blocks in it. A Qt host
	/// shows its dialog on the UI thread and waits for it on a future; Big Picture posts a message
	/// dialog to the GS thread. Whatever the handler waits for must be answerable WITHOUT the CPU
	/// thread: it must not wait for the CPU thread to run a queued task (Host::RunOnCPUThread with
	/// block = true, a BlockingQueuedConnection into a UI thread that is itself waiting on the CPU
	/// thread) or boot deadlocks. At this point the GS may not be open yet; a handler that needs the
	/// GS thread must check that the Big Picture UI is running. A handler that draws in Big Picture
	/// must also service input on the waiting thread, since input reaches the UI through the CPU
	/// thread (Big Picture's does: see FullscreenUI_Saves.cpp). It must always return.
	using RestorePromptHandler = std::function<RestoreAnswer(const RestorePrompt&)>;

	/// Registers the host's handler (or clears it with an empty function). With none registered the
	/// boot goes ahead with the unreadable card and the player is told on screen where to restore its
	/// backup from (ReportUnansweredRestore()). Set it before the VM starts.
	void SetRestorePromptHandler(RestorePromptHandler handler);

	/// A second place for a handler, for a UI that is not the host's main one: Big Picture sets it
	/// while it is running (and clears it when it shuts down). It is used only while no handler is
	/// set with SetRestorePromptHandler(), whichever of the two was registered first, so the Qt
	/// host's own dialog wins whenever the Qt main window exists and Big Picture asks only when it
	/// is the one UI there is (armsx2-sdl, or Qt before its window exists).
	void SetFallbackRestorePromptHandler(RestorePromptHandler handler);

	/// The handler a boot would call now: the host's, else the fallback, else empty.
	RestorePromptHandler GetRestorePromptHandler();

	/// The fallback handler alone (empty when none). A host whose own window is hidden behind Big
	/// Picture hands the question to it, since Big Picture is what the player is looking at.
	RestorePromptHandler GetFallbackRestorePromptHandler();

	/// What happened to one card in PrepareCardsForBoot().
	struct CardReport
	{
		std::string card;
		/// The handler was asked.
		bool prompted = false;
		/// The user chose to restore and the restore worked.
		bool restored = false;
		/// What SnapshotIfChanged() did; Absent if the card was restored (it equals a snapshot that
		/// already exists, so it is not snapshotted again).
		CardStatus status = CardStatus::Absent;
		/// The card is unreadable and has a healthy backup, the VM is starting, and no handler was
		/// registered to ask: the boot went ahead with the card as it is. PrepareCardsForBoot() has
		/// told the player on screen (ReportUnansweredRestore()).
		bool unanswered = false;
	};

	/// Tells the player, on screen and in the log, that `card` cannot be read and has a backup that was
	/// not offered because nobody could be asked (no handler registered, no window to ask in, a
	/// headless run), and where to restore it from. For a host that answers "Start Anyway" itself.
	void ReportUnansweredRestore(const std::string& card);

	/// The work of OnCardsReopening() on a given list of card names: for each, offers to restore an
	/// unreadable one that has a healthy backup (only if `allow_prompt` and `handler` is set), then
	/// snapshots it if it changed. Duplicate names are done once.
	std::vector<CardReport> PrepareCardsForBoot(Store& store, const std::vector<std::string>& cards,
		const std::string& game, bool allow_prompt, const RestorePromptHandler& handler);

	// ---- entry points ---------------------------------------------------------------------

	/// The default of [MemoryCards] AutoBackup for a build, from the platforms that have a front end
	/// of their own for it: Android's app takes its own snapshots (MemoryCardBackup.kt), and the
	/// libretro core and the iOS app have no screen that lists backups or asks whether to restore
	/// one. Everywhere else the default is on.
	constexpr bool AutoBackupDefaultFor(bool android, bool libretro, bool ios)
	{
		return !(android || libretro || ios);
	}

	/// AutoBackupDefaultFor() for this build. Pcsx2Config sets EmuConfig.McdAutoBackup from it.
	bool AutoBackupDefault();

	/// [MemoryCards] AutoBackup, default AutoBackupDefault(). Read from EmuConfig.McdAutoBackup, so
	/// it follows the per-game and base settings like any other core setting and is safe to call
	/// with the settings lock held.
	bool IsAutoBackupEnabled();

	/// Called by FileMcd_Reopen with the cards closed. While the VM is starting, snapshots the slot 1
	/// and 2 cards (see above) and offers to restore an unreadable one; at any other time it does
	/// nothing (see "When snapshots are taken").
	void OnCardsReopening();

	/// OnCardsReopening() for a caller that already knows whether this is the boot's reopen.
	void OnCardsReopening(bool booting);

	/// For the UIs. Both refuse while a VM exists, with an explanation in the outcome.
	Outcome BackUpCardNow(const std::string& card);
	Outcome RestoreSnapshot(const Snapshot& snapshot);
} // namespace MemcardBackup
