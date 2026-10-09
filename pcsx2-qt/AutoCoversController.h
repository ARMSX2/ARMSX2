// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Catalog/CoverAutoDownload.h"

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class GameListWidget;
class QMainWindow;
class QProgressBar;

/// Runs the automatic cover download (Catalog::AutoCovers) for the main window: starts it when a game
/// list refresh has finished, shows its progress in the status bar, and when a cover file lands makes
/// the grid (and Big Picture, if it is open) pick it up.
///
/// The cover job runs on its own thread and calls back from there; everything the callbacks report
/// is queued to the UI thread. The destructor stops the job and waits for it, so nothing runs after
/// the main window is gone.
class AutoCoversController final : public QObject
{
	Q_OBJECT

public:
	/// `window` owns the status bar and the controller (it is the parent).
	AutoCoversController(GameListWidget* game_list, QMainWindow* window);
	~AutoCoversController() override;

	/// The main window's controller, or null before it exists.
	static AutoCoversController* instance();

	/// Downloads covers for the games that have none, if [UI] AutoDownloadCovers is on. Does nothing
	/// when a run is going.
	void startAfterRefresh();

	/// Downloads covers again for the games at `paths` (game list paths; empty means every game),
	/// replacing what they have. The user asked for this, so it reports even when nothing came of it.
	void startRedownload(std::vector<std::string> paths);

private Q_SLOTS:
	void drainHandoff();
	void flushBigPicture();

private:
	Catalog::AutoCovers::Callbacks makeCallbacks();
	void queueDrain();
	void showFinished(const Catalog::CoverJobSummary& summary);
	void showProgress(const Catalog::CoverProgress& progress);
	void hideProgress();

	// What the worker thread hands over. At most one drain is queued at a time, so a run that skips
	// thousands of games a second costs the UI thread one look per event-loop turn.
	struct Handoff
	{
		std::mutex mutex;
		bool drain_queued = false;
		std::optional<Catalog::CoverProgress> progress;
		std::vector<std::pair<std::string, std::string>> saved; ///< game path, cover file
		std::optional<Catalog::CoverJobSummary> finished;
	};
	Handoff m_handoff;

	GameListWidget* m_game_list;
	QMainWindow* m_window;
	QProgressBar* m_progress_bar = nullptr;
	bool m_progress_shown = false;
	QTimer m_big_picture_timer;
	std::atomic_bool m_run_requested{false};
};
