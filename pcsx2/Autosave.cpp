// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Autosave.h"

#include "Config.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "SIO/Sio.h"
#include "VMManager.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"

#include "IconsFontAwesome.h"
#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string_view>
#include <utility>

namespace Autosave
{
	std::string FileNameAt(const std::string& newest, int n)
	{
		if (n <= 1 || !newest.ends_with(".p2s"))
			return newest;
		return fmt::format("{}.{}.p2s", std::string_view(newest).substr(0, newest.size() - 4), n);
	}

	namespace
	{
		// While a rotation is open its files are mid-move, and a .drop with an empty slot is how a save in
		// progress looks, not a crash. Begin() holds the mutex while it moves files, and List() recovers
		// only when no rotation is open, so neither sees the other half way.
		std::mutex s_rotation_mutex;
		int s_open_rotations = 0; // guarded by s_rotation_mutex

		// A run cut short leaves "<slot>.drop": the autosave set aside to make room for one that was not
		// written. If nothing has taken its slot since, it is an autosave again (with keep == 1 it can be the
		// only one there is). If its slot is filled, a later step of that run got further and what the .drop
		// holds was due to go: [remove_stale] deletes it.
		void RecoverSetAside(const std::string& newest, Catalog::FileOps& ops, bool remove_stale)
		{
			for (int n = KEEP_MIN; n <= KEEP_MAX; n++)
			{
				const std::string slot = FileNameAt(newest, n);
				if (n > 1 && slot == newest)
					break; // not a .p2s name, so there are no numbered neighbours
				const std::string drop = slot + ".drop";
				if (!ops.Exists(drop))
					continue;

				if (!ops.Exists(slot))
				{
					if (!ops.Rename(drop, slot))
						Console.Error("Autosave: could not put '%s' back as '%s'", drop.c_str(), slot.c_str());
				}
				else if (remove_stale)
				{
					ops.Remove(drop);
				}
			}
		}
	} // namespace

	// ---- Rotation -------------------------------------------------------------------------

	Rotation::Rotation(std::string newest, int keep, Catalog::FileOps& ops)
		: m_newest(std::move(newest))
		, m_keep(std::clamp(keep, KEEP_MIN, KEEP_MAX))
		, m_ops(ops)
	{
	}

	Rotation::~Rotation()
	{
		if (m_open)
			Rollback();
	}

	bool Rotation::MoveFile(const std::string& from, const std::string& to)
	{
		if (!m_ops.Rename(from, to))
		{
			Console.Error("Autosave: could not rename '%s' to '%s'", from.c_str(), to.c_str());
			return false;
		}
		m_moves.push_back({from, to});
		return true;
	}

	bool Rotation::Begin()
	{
		if (m_open)
			return true;
		// Only a name ending in .p2s has older neighbours to move to; anything else would be
		// renamed onto itself.
		if (m_newest.empty() || FileNameAt(m_newest, 2) == m_newest)
			return false;

		std::lock_guard lock(s_rotation_mutex);

		// Files set aside by a run that was cut short: put back where nothing took their place, else
		// deleted, since they would be in the way of this run's own.
		RecoverSetAside(m_newest, m_ops, true);

		m_moves.clear();
		m_open = true;
		s_open_rotations++;

		// Nothing to move when there is no autosave yet; the new one simply becomes the newest.
		if (!m_ops.Exists(m_newest))
			return true;

		const std::string oldest = FileNameAt(m_newest, m_keep);
		const std::string drop = oldest + ".drop";

		bool ok = true;
		if (m_keep == 1)
		{
			ok = MoveFile(m_newest, drop);
		}
		else
		{
			if (m_ops.Exists(oldest))
				ok = MoveFile(oldest, drop);
			for (int n = m_keep - 1; ok && n >= 2; n--)
			{
				const std::string from = FileNameAt(m_newest, n);
				if (m_ops.Exists(from))
					ok = MoveFile(from, FileNameAt(m_newest, n + 1));
			}
			if (ok)
				ok = MoveFile(m_newest, FileNameAt(m_newest, 2));
		}

		if (!ok)
		{
			// Whatever sits at the newest name is still the original unless it was moved.
			Undo(false);
			m_open = false;
			s_open_rotations--;
			return false;
		}
		return true;
	}

	void Rotation::Commit()
	{
		if (!m_open)
			return;

		std::lock_guard lock(s_rotation_mutex);
		const std::string drop = FileNameAt(m_newest, m_keep) + ".drop";
		if (m_ops.Exists(drop))
			m_ops.Remove(drop);

		// Autosaves past the keep count are left from a larger setting.
		for (int n = m_keep + 1; n <= KEEP_MAX; n++)
		{
			const std::string extra = FileNameAt(m_newest, n);
			if (m_ops.Exists(extra))
				m_ops.Remove(extra);
		}

		m_moves.clear();
		m_open = false;
		s_open_rotations--;
	}

	void Rotation::Rollback()
	{
		if (!m_open)
			return;
		std::lock_guard lock(s_rotation_mutex);
		Undo(true);
		m_open = false;
		s_open_rotations--;
	}

	void Rotation::Undo(bool remove_new_newest)
	{
		// The file at the newest name after a failed save is the new one, possibly half written,
		// and must go before the old newest can come back. After a failed Begin() it is still the
		// original, unless it had already been moved.
		bool newest_was_moved = false;
		for (const Move& move : m_moves)
			newest_was_moved = newest_was_moved || (move.from == m_newest);
		if ((remove_new_newest || newest_was_moved) && m_ops.Exists(m_newest))
			m_ops.Remove(m_newest);

		for (auto it = m_moves.rbegin(); it != m_moves.rend(); ++it)
		{
			if (!m_ops.Rename(it->to, it->from))
				Console.Error("Autosave: could not put '%s' back as '%s'", it->to.c_str(), it->from.c_str());
		}
		m_moves.clear();
	}

	// ---- saving ---------------------------------------------------------------------------

	SaveResult RotatedSave(const std::string& newest, int keep,
		const std::function<bool(const std::string& path, std::string* error)>& save, std::string* error,
		Catalog::FileOps& ops)
	{
		if (newest.empty())
			return SaveResult::NoGame;

		Rotation rotation(newest, keep, ops);
		if (!rotation.Begin())
		{
			if (error)
				*error = "The older autosaves could not be moved to make room.";
			return SaveResult::RotationFailed;
		}

		std::string save_error;
		const bool wrote = save(newest, &save_error) && ops.Exists(newest);
		if (!wrote)
		{
			rotation.Rollback();
			if (error)
				*error = save_error.empty() ? std::string("The save state was not written.") : std::move(save_error);
			return SaveResult::SaveFailed;
		}

		rotation.Commit();
		return SaveResult::Saved;
	}

	int ConfiguredKeep()
	{
		return std::clamp<int>(EmuConfig.AutosaveKeep, KEEP_MIN, KEEP_MAX);
	}

	static SaveResult SaveRunningGame(int keep, std::string* error)
	{
		if (GSDumpReplayer::IsReplayingDump())
			return SaveResult::NoGame;

		const std::string newest = VMManager::GetSaveStateFileName(
			VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC(), VMManager::SAVESTATE_SLOT_AUTOSAVE);
		if (newest.empty())
			return SaveResult::NoGame;

		// VMManager::SaveState() refuses too, but by then the older autosaves would have moved.
		if (MemcardBusy::IsBusy())
		{
			if (error)
				*error = "The memory card is busy.";
			return SaveResult::CardBusy;
		}

		return RotatedSave(
			newest, keep,
			[](const std::string& path, std::string* save_error) {
				std::string reason;
				// Not SaveStateToSlot(): that shows "Saving state to slot -2..." and keeps a .backup
				// of the file it replaces, and the rotation has already set the old one aside.
				VMManager::SaveState(path.c_str(), false, false, [&reason](const std::string& e) { reason = e; });
				if (reason.empty())
					return true;
				*save_error = std::move(reason);
				return false;
			},
			error);
	}

	SaveResult SaveNow(int keep, std::string* error)
	{
		pxAssertMsg(VMManager::Internal::IsOnCPUThread(), "Autosave::SaveNow() off the CPU thread; use Host::RunOnCPUThread()");
		if (!VMManager::HasValidVM())
			return SaveResult::NoGame;
		return SaveRunningGame(keep, error);
	}

	// ---- the timer ------------------------------------------------------------------------

	bool IntervalTimer::Advance(double seconds, int interval_minutes)
	{
		if (interval_minutes <= 0)
		{
			m_elapsed = 0.0;
			return false;
		}
		// A gap this long was a pause or a stall, not play time.
		if (seconds > 0.0 && seconds <= MAX_COUNTED_GAP_SECONDS)
			m_elapsed += seconds;
		return m_elapsed >= static_cast<double>(interval_minutes) * 60.0;
	}

	void IntervalTimer::OnResult(SaveResult result, int interval_minutes)
	{
		if (result == SaveResult::CardBusy)
		{
			const double interval_seconds = static_cast<double>(std::max(interval_minutes, 0)) * 60.0;
			m_elapsed = std::max(0.0, interval_seconds - RETRY_WHEN_BUSY_SECONDS);
		}
		else
		{
			m_elapsed = 0.0;
		}
	}

	// ---- hooks the VM calls ---------------------------------------------------------------

	namespace
	{
		IntervalTimer s_timer;
		std::chrono::steady_clock::time_point s_last_tick;
		bool s_have_tick = false;
		// Set once the VM has run a frame, so a shutdown after a failed boot does not try to freeze
		// a machine that never started.
		bool s_vm_ran = false;

		void ReportFailure(const std::string& error)
		{
			Console.Warning("Autosave failed: %s", error.c_str());
			Host::AddIconOSDMessage("AutosaveFailed", ICON_FA_TRIANGLE_EXCLAMATION,
				fmt::format(TRANSLATE_FS("Autosave", "Autosave failed: {}"), error), Host::OSD_WARNING_DURATION);
		}
	} // namespace

	void OnPollInput()
	{
		s_vm_ran = true;

		const auto now = std::chrono::steady_clock::now();
		const double seconds = s_have_tick ? std::chrono::duration<double>(now - s_last_tick).count() : 0.0;
		s_last_tick = now;
		s_have_tick = true;

		const int interval = EmuConfig.AutosaveIntervalMinutes;
		if (!s_timer.Advance(seconds, interval))
			return;

		std::string error;
		const SaveResult result = SaveRunningGame(ConfiguredKeep(), &error);
		s_timer.OnResult(result, interval);
		// The time spent writing is not play time.
		s_last_tick = std::chrono::steady_clock::now();

		switch (result)
		{
			case SaveResult::Saved:
				Host::AddIconOSDMessage("AutosaveSaved", ICON_FA_FLOPPY_DISK, TRANSLATE_STR("Autosave", "Autosaved."),
					Host::OSD_QUICK_DURATION);
				break;
			case SaveResult::RotationFailed:
			case SaveResult::SaveFailed:
				ReportFailure(error);
				break;
			case SaveResult::CardBusy:
				DevCon.WriteLn("Autosave: the memory card is busy, trying again in %d seconds", RETRY_WHEN_BUSY_SECONDS);
				break;
			case SaveResult::NoGame:
				break;
		}
	}

	void OnShutdown()
	{
		const bool ran = std::exchange(s_vm_ran, false);
		s_timer.Reset();
		s_have_tick = false;

		if (!ran || !EmuConfig.AutosaveOnShutdown)
			return;

		std::string error;
		const SaveResult result = SaveRunningGame(ConfiguredKeep(), &error);
		if (result == SaveResult::Saved)
			Console.WriteLn("Autosave: saved on shutdown");
		else if (result == SaveResult::CardBusy)
			Console.Warning("Autosave: not saved on shutdown, the memory card is busy");
		else if (result != SaveResult::NoGame)
			ReportFailure(error);
	}

	// ---- listing --------------------------------------------------------------------------

	std::vector<Entry> List(const std::string& newest)
	{
		std::vector<Entry> entries;
		if (newest.empty())
			return entries;

		{
			std::lock_guard lock(s_rotation_mutex);
			if (s_open_rotations == 0)
				RecoverSetAside(newest, Catalog::GetRealFileOps(), false);
		}

		for (int n = 1; n <= KEEP_MAX; n++)
		{
			const std::string path = FileNameAt(newest, n);
			if (n > 1 && path == newest)
				break; // not a .p2s name, so there are no numbered neighbours

			FILESYSTEM_STAT_DATA stat;
			if (!FileSystem::StatFile(path.c_str(), &stat) || (stat.Attributes & FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY))
				continue;

			Entry entry;
			entry.index = n;
			entry.path = path;
			entry.modified = stat.ModificationTime;
			entry.size = stat.Size;
			entries.push_back(std::move(entry));
		}
		return entries;
	}

	std::vector<Entry> List(const char* game_serial, u32 game_crc)
	{
		return List(VMManager::GetSaveStateFileName(game_serial, game_crc, VMManager::SAVESTATE_SLOT_AUTOSAVE));
	}

	std::vector<Entry> ListForRunningGame()
	{
		if (!VMManager::HasValidVM())
			return {};
		return List(VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC());
	}
} // namespace Autosave
