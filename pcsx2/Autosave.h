// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Catalog/FileOps.h"

#include "common/Pcsx2Types.h"

#include <ctime>
#include <functional>
#include <string>
#include <vector>

// Autosaves: save states the emulator writes by itself, kept as a short rotating history.
//
// Files, in the save state folder (EmuFolders::Savestates), for the running game:
//
//   {serial} ({CRC}).autosave.p2s      the newest. This is VMManager::SAVESTATE_SLOT_AUTOSAVE (-2).
//   {serial} ({CRC}).autosave.2.p2s    the one before it, then .3 .4 .5 (KEEP_MAX in all).
//   {serial} ({CRC}).autosave.N.p2s.drop   transient: the oldest, set aside while a new one is
//                                       being written, so a failed save loses nothing. One a crash
//                                       left behind is put back at its slot if that is empty (the
//                                       only autosave, with keep 1), else deleted by the next save.
//
// Every new autosave moves the existing ones one place older and drops the oldest past the
// "keep" count. These files are separate from the numbered slots 1-10 and from the resume state
// ({serial} ({CRC}).resume.p2s, written by EmuCore/SaveStateOnShutdown); see below.
//
// Settings (all in [EmuCore], read from EmuConfig):
//   AutosaveIntervalMinutes   0 = off (default), else save every N minutes of running time
//   AutosaveKeep              how many to keep, 1-5, default 3
//   AutosaveOnShutdown        also save when the game is closed, default off
//
// Threading: everything that writes (SaveNow, OnPollInput, OnShutdown) runs on the CPU thread and
// blocks it until the file is written (a tenth to a third of a second for a game on an SSD; longer
// on a slow SD card). A host on another thread marshals with Host::RunOnCPUThread(..., /*block=*/
// true). The List functions only read the folder and can be called from any thread; during a save
// they may see the files mid-rotation.
//
// When: the interval timer runs from VMManager::Internal::PollInputOnCPUThread, once per frame right
// after the hotkeys are polled, which is the point a hotkey save state is taken at.
//
// Loading an autosave is the ordinary VMManager::LoadState(entry.path); its thumbnail is
// SaveState_ReadScreenshot(entry.path, ...). The UIs offer autosaves in their Load menus only;
// nothing saves into a chosen autosave file. VMManager::DeleteSaveStates() does not touch them.
//
// How this relates to the resume state: "Save State On Shutdown" (EmuCore/SaveStateOnShutdown)
// writes ONE file, the resume state, that the next launch offers to continue from; it is made by
// VMManager::Shutdown(save_resume_state) and is untouched by this code. AutosaveOnShutdown writes
// into the rotating autosave history instead, so it never replaces the resume state and the resume
// state never counts against the keep limit. Both can be on at once: a shutdown then writes both.
namespace Autosave
{
	constexpr int KEEP_MIN = 1;
	constexpr int KEEP_MAX = 5;
	constexpr int KEEP_DEFAULT = 3;

	/// After a save was refused because the memory card was written to recently, try again after
	/// this long rather than waiting for a whole interval.
	constexpr int RETRY_WHEN_BUSY_SECONDS = 30;

	/// The file name of the n-th newest autosave beside `newest`, n = 1 being `newest` itself and
	/// 2..KEEP_MAX the older ones ("... (CRC).autosave.3.p2s"). Returns `newest` for n <= 1.
	std::string FileNameAt(const std::string& newest, int n);

	// ---- the rotation ---------------------------------------------------------------------

	/// Makes room for one new autosave, and puts everything back if it does not get written.
	///
	///   Rotation rotation(newest, keep);
	///   if (!rotation.Begin()) { ... nothing was changed ... }
	///   write the new file to `newest`
	///   if (written) rotation.Commit(); else rotation.Rollback();
	///
	/// Begin() moves every existing autosave one place older. The one that would pass `keep` is not
	/// deleted but renamed to "<its name>.drop" until Commit(), so a save that fails loses nothing.
	/// Rollback() deletes whatever was written to `newest` (it may be half a file) and moves
	/// everything back; the destructor does the same if neither was called. A step of Begin() that
	/// fails undoes the steps before it and returns false.
	///
	/// With keep == 1 there is no history: Begin() sets the current autosave aside as .drop in the
	/// same way, so a failed save still leaves the previous autosave in place.
	class Rotation
	{
	public:
		Rotation(std::string newest, int keep, Catalog::FileOps& ops = Catalog::GetRealFileOps());
		~Rotation();

		Rotation(const Rotation&) = delete;
		Rotation& operator=(const Rotation&) = delete;

		bool Begin();
		/// The new autosave was written: delete the one set aside, and any autosave past `keep`
		/// (left over from a larger setting).
		void Commit();
		/// The new autosave was not written: put everything back.
		void Rollback();

		bool IsOpen() const { return m_open; }

	private:
		struct Move
		{
			std::string from;
			std::string to;
		};

		bool MoveFile(const std::string& from, const std::string& to);
		void Undo(bool remove_new_newest);

		std::string m_newest;
		int m_keep;
		Catalog::FileOps& m_ops;
		std::vector<Move> m_moves;
		bool m_open = false;
	};

	enum class SaveResult
	{
		Saved,
		/// No game with a name to save under (BIOS menu), or no VM.
		NoGame,
		/// The game wrote to the memory card in the last few seconds. Nothing was changed; try again.
		CardBusy,
		/// A file could not be moved to make room. Nothing was changed and no save was attempted.
		RotationFailed,
		/// The state could not be written. The previous autosaves are back where they were.
		SaveFailed,
	};

	/// Writes one autosave with the rotation around it. `save` writes the state to the path it is
	/// given and returns false, with a reason, if it could not. The result is Saved only if `save`
	/// returned true and the file exists afterwards. This is the whole of SaveNow() apart from reading
	/// the VM, kept separate so the failure paths can be tested without one.
	SaveResult RotatedSave(const std::string& newest, int keep,
		const std::function<bool(const std::string& path, std::string* error)>& save, std::string* error = nullptr,
		Catalog::FileOps& ops = Catalog::GetRealFileOps());

	/// The keep count from the settings, clamped to KEEP_MIN..KEEP_MAX.
	int ConfiguredKeep();

	// ---- saving ---------------------------------------------------------------------------

	/// Saves an autosave of the running game now, keeping `keep` (clamped to 1-5). CPU thread only;
	/// blocks until written. Refuses with CardBusy while the game is writing its memory card, the
	/// same rule as every other save state. `error`, if given, gets a reason on failure.
	SaveResult SaveNow(int keep, std::string* error = nullptr);

	/// Called by the VM: once per frame on the CPU thread; runs the interval timer.
	void OnPollInput();
	/// Called by VMManager::Shutdown: saves if AutosaveOnShutdown is set, and resets the timer.
	void OnShutdown();

	// ---- the timer ------------------------------------------------------------------------

	/// Counts running time and says when an interval has passed. Separate from the clock so it can
	/// be tested. Time the emulator did not run (paused, a long stall) does not count: a gap larger
	/// than MAX_COUNTED_GAP_SECONDS between two calls is ignored.
	class IntervalTimer
	{
	public:
		static constexpr double MAX_COUNTED_GAP_SECONDS = 2.0;

		/// `seconds` is the time since the previous call. Returns true when a save is due. With an
		/// interval of 0 the timer is off and never due.
		bool Advance(double seconds, int interval_minutes);

		/// Tells the timer how the save it asked for went. Saved or failed: wait a whole interval.
		/// CardBusy: due again after RETRY_WHEN_BUSY_SECONDS.
		void OnResult(SaveResult result, int interval_minutes);

		void Reset() { m_elapsed = 0.0; }
		double Elapsed() const { return m_elapsed; }

	private:
		double m_elapsed = 0.0;
	};

	// ---- listing --------------------------------------------------------------------------

	struct Entry
	{
		/// 1 = the newest, 2..5 older. The Nth entry of an autosave is "Autosave N" in the UIs.
		int index = 0;
		/// Full path; pass to VMManager::LoadState().
		std::string path;
		/// When the file was last written.
		std::time_t modified = 0;
		s64 size = 0;
	};

	/// The autosaves that exist beside `newest` (see FileNameAt), newest first. This is rotation
	/// order, not sorted by time stamp: the file at index 1 is the newest autosave. A ".drop" file
	/// whose slot is empty (left by a crash) is first renamed back to the slot, unless a save is
	/// running in this process; it deletes nothing.
	std::vector<Entry> List(const std::string& newest);

	/// The autosaves of any game, for the game list.
	std::vector<Entry> List(const char* game_serial, u32 game_crc);

	/// The autosaves of the running game. Empty when there is no VM or the game has no name.
	std::vector<Entry> ListForRunningGame();
} // namespace Autosave
