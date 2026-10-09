// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "AutoCoversController.h"
#include "GameList/GameListModel.h"
#include "GameList/GameListWidget.h"
#include "QtHost.h"

#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/MTGS.h"

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QStatusBar>

namespace AutoCovers = Catalog::AutoCovers;

static AutoCoversController* s_instance = nullptr;

AutoCoversController::AutoCoversController(GameListWidget* game_list, QMainWindow* window)
	: QObject(window)
	, m_game_list(game_list)
	, m_window(window)
{
	s_instance = this;

	// Covers saved in quick succession ask Big Picture to forget its cover lookups once.
	m_big_picture_timer.setSingleShot(true);
	m_big_picture_timer.setInterval(1000);
	connect(&m_big_picture_timer, &QTimer::timeout, this, &AutoCoversController::flushBigPicture);

	// Queued: the refresh thread is still being torn down when the signal fires.
	connect(game_list, &GameListWidget::refreshComplete, this, &AutoCoversController::startAfterRefresh, Qt::QueuedConnection);
}

AutoCoversController::~AutoCoversController()
{
	// Joins the worker, so no callback is running or queued against this object after this line.
	AutoCovers::Shutdown();
	if (s_instance == this)
		s_instance = nullptr;
}

AutoCoversController* AutoCoversController::instance()
{
	return s_instance;
}

void AutoCoversController::startAfterRefresh()
{
	if (AutoCovers::IsRunning())
		return;

	m_run_requested.store(false);
	AutoCovers::StartAfterRefresh(makeCallbacks());
}

void AutoCoversController::startRedownload(std::vector<std::string> paths)
{
	if (AutoCovers::IsRunning())
	{
		m_window->statusBar()->showMessage(tr("A cover download is already running."), 5000);
		return;
	}

	if (paths.empty())
	{
		const auto lock = GameList::GetLock();
		const u32 count = GameList::GetEntryCount();
		for (u32 i = 0; i < count; i++)
		{
			if (const GameList::Entry* entry = GameList::GetEntryByIndex(i))
				paths.push_back(entry->path);
		}
	}

	m_run_requested.store(true);
	if (!AutoCovers::StartRedownload(paths, makeCallbacks()))
		m_window->statusBar()->showMessage(tr("There are no games to download covers for."), 5000);
}

AutoCovers::Callbacks AutoCoversController::makeCallbacks()
{
	AutoCovers::Callbacks callbacks;
	callbacks.on_progress = [this](const Catalog::CoverProgress& progress) {
		{
			std::lock_guard<std::mutex> lock(m_handoff.mutex);
			m_handoff.progress = progress;
		}
		queueDrain();
	};
	callbacks.on_cover_saved = [this](const Catalog::CoverSaved& saved) {
		{
			std::lock_guard<std::mutex> lock(m_handoff.mutex);
			m_handoff.saved.emplace_back(saved.id, saved.path);
		}
		queueDrain();
	};
	callbacks.on_finished = [this](const Catalog::CoverJobSummary& summary) {
		{
			std::lock_guard<std::mutex> lock(m_handoff.mutex);
			m_handoff.finished = summary;
		}
		queueDrain();
	};
	return callbacks;
}

void AutoCoversController::queueDrain()
{
	{
		std::lock_guard<std::mutex> lock(m_handoff.mutex);
		if (m_handoff.drain_queued)
			return;
		m_handoff.drain_queued = true;
	}
	QMetaObject::invokeMethod(this, &AutoCoversController::drainHandoff, Qt::QueuedConnection);
}

void AutoCoversController::drainHandoff()
{
	std::vector<std::pair<std::string, std::string>> saved;
	std::optional<Catalog::CoverProgress> progress;
	std::optional<Catalog::CoverJobSummary> finished;
	{
		std::lock_guard<std::mutex> lock(m_handoff.mutex);
		m_handoff.drain_queued = false;
		saved.swap(m_handoff.saved);
		progress.swap(m_handoff.progress);
		finished.swap(m_handoff.finished);
	}

	// A cover that arrived replaces the placeholder (or the old cover) of that game's tile.
	if (!saved.empty())
	{
		GameListModel* model = m_game_list->getModel();
		for (const auto& [game_path, cover_path] : saved)
			model->refreshCoverForPath(game_path);
		m_big_picture_timer.start();
	}

	if (finished.has_value())
	{
		hideProgress();
		showFinished(*finished);
		m_big_picture_timer.stop();
		flushBigPicture();
	}
	else if (progress.has_value() && progress->total > 0)
	{
		showProgress(*progress);
	}
}

void AutoCoversController::showProgress(const Catalog::CoverProgress& progress)
{
	QStatusBar* bar = m_window->statusBar();
	if (!m_progress_bar)
	{
		m_progress_bar = new QProgressBar(bar);
		m_progress_bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		m_progress_bar->setFixedSize(140, 16);
		m_progress_bar->setMinimum(0);
	}
	if (!m_progress_shown)
	{
		bar->addPermanentWidget(m_progress_bar);
		m_progress_bar->show();
		m_progress_shown = true;
	}
	m_progress_bar->setMaximum(static_cast<int>(progress.total));
	m_progress_bar->setValue(static_cast<int>(progress.done));

	bar->showMessage(tr("Downloading covers: %1 of %2 (%3)")
			.arg(progress.done)
			.arg(progress.total)
			.arg(QString::fromStdString(progress.current_title)));
}

void AutoCoversController::hideProgress()
{
	if (!m_progress_shown)
		return;

	m_progress_shown = false;
	m_progress_bar->hide();
	m_window->statusBar()->removeWidget(m_progress_bar);
	m_window->statusBar()->clearMessage();
}

void AutoCoversController::showFinished(const Catalog::CoverJobSummary& summary)
{
	// A run a refresh started stays quiet unless it brought something; one the user asked for answers.
	const bool requested = m_run_requested.load();
	QStatusBar* bar = m_window->statusBar();

	switch (summary.end)
	{
		case Catalog::CoverJobEnd::Completed:
			if (summary.downloaded > 0)
				bar->showMessage(tr("Downloaded %n cover(s).", nullptr, static_cast<int>(summary.downloaded)), 5000);
			else if (requested)
				bar->showMessage(tr("No covers were available to download."), 5000);
			break;

		case Catalog::CoverJobEnd::RateLimited:
			bar->showMessage(tr("The cover server asked us to slow down. Downloads resume later."), 8000);
			break;

		case Catalog::CoverJobEnd::BackedOff:
			if (requested)
				bar->showMessage(tr("The cover server is limiting requests. Try again later."), 8000);
			break;

		case Catalog::CoverJobEnd::NetworkDown:
			if (requested)
				bar->showMessage(tr("Could not reach the cover server."), 8000);
			break;

		case Catalog::CoverJobEnd::Cancelled:
			break;
	}
}

void AutoCoversController::flushBigPicture()
{
	// Big Picture caches where each game's cover is, including "none": it has to look again. Its
	// cache belongs to the GS thread, which the CPU thread can reach.
	Host::RunOnCPUThread([]() {
		if (MTGS::IsOpen())
			FullscreenUI::InvalidateCoverCache();
	});
}

#include "moc_AutoCoversController.cpp"
