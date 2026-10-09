// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Tools > Arcade Setup: everything a Namco System 246/256 game needs besides the game's own files, and a
// check of which games in the game list are ready to start. A port of the Android app's Arcade screen over
// ArcadeSetup::Service, which does the work (this only shows it):
//   1. the boot files: download or update them from the Proverb project, or install a copy from a file;
//   2. the arcade BIOS: which board dumps are in the BIOS folder, and what to do when there are none;
//   3. the games: each arcade game in the game list, ready or what it lacks and where that goes.
// Downloading, the update check and the readiness check run on worker threads; the dialog waits for them
// before it goes away.

#include "pcsx2/Arcade/ArcadeSetup.h"

#include <QtCore/QList>
#include <QtWidgets/QDialog>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QLabel;
class QProgressBar;
class QPushButton;
class QThread;
class QTreeWidget;

class ArcadeSetupDialog final : public QDialog
{
	Q_OBJECT

public:
	explicit ArcadeSetupDialog(QWidget* parent = nullptr);
	~ArcadeSetupDialog() override;

protected:
	void changeEvent(QEvent* event) override;
	void done(int result) override;

private Q_SLOTS:
	void onDownloadClicked();
	void onInstallFromFileClicked();
	void onOpenBiosFolderClicked();
	void onGameSelectionChanged();

private:
	struct GameRow
	{
		std::string path; // The .acgame.
		std::string title;
		std::string serial;
		std::optional<ArcadeSetup::MissingReport> report; // Empty while it is being checked.
	};

	void buildUi();

	void refreshBootFiles();
	void refreshBios();
	void refreshGames();
	void startUpdateCheck();
	void updateBootControls();
	void setBootMessage(const QString& html);
	void onDownloadProgress(quint64 done, quint64 total);
	void onInstallFinished(const ArcadeSetup::InstallResult& result, bool was_download);
	void fillGameRow(int index);
	void updateGamesSummary();
	void cancelWork();

	/// Runs [work] on a thread; [done] runs on the UI thread afterwards, unless the dialog is gone by then.
	void runTask(std::function<void()> work, std::function<void()> done);

	ArcadeSetup::Service m_service;
	Catalog::CancelToken m_download_cancel;
	Catalog::CancelToken m_background_cancel; // The update check; fires when the dialog closes.
	QList<QThread*> m_tasks;

	// The boot files.
	ArcadeSetup::BootFilesStatus m_boot;
	std::optional<ArcadeSetup::UpdateCheck> m_update;
	bool m_checking_update = false;
	bool m_downloading = false;
	bool m_installing = false;

	// The games. A refresh made while an older one is still being checked makes the older answer stale.
	std::vector<GameRow> m_games;
	u32 m_games_generation = 0;
	bool m_checking_games = false;

	QLabel* m_intro = nullptr;
	QLabel* m_boot_status = nullptr;
	QLabel* m_boot_update = nullptr;
	QLabel* m_boot_message = nullptr;
	QProgressBar* m_boot_progress = nullptr;
	QPushButton* m_download_button = nullptr;
	QPushButton* m_install_button = nullptr;
	QLabel* m_bios_status = nullptr;
	QLabel* m_bios_help = nullptr;
	QLabel* m_bios_folder = nullptr;
	QPushButton* m_bios_open_button = nullptr;
	QPushButton* m_bios_refresh_button = nullptr;
	QLabel* m_games_summary = nullptr;
	QTreeWidget* m_games_list = nullptr;
	QLabel* m_game_detail = nullptr;
	QPushButton* m_games_refresh_button = nullptr;
};
