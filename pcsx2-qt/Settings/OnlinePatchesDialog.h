// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// "Browse Online..." on the per-game Patches and Cheats pages: searches the community patch and cheat
// repositories for the game (Catalog::PatchRepo), lists what it found grouped by source, and installs
// the sections the player ticks. The Patches page shows patches, the Cheats page shows cheats, and the
// Cheats page also offers the separate "large cheat database" search.
//
// The search runs on PatchRepo's own worker thread and reports back through queued calls; installing
// runs on a short-lived worker (file I/O only). Nothing here blocks the UI thread except closing the
// dialog while a request is in flight, which waits for the request to stop with the dialog still
// responding.
//
// Install edits the game's settings file on disk, so after an install the settings window's copy of
// that file is reloaded before anything else can save it; see reloadSettingsFromDisk().

#include "pcsx2/Catalog/PatchRepo.h"

#include <QtWidgets/QDialog>

#include <memory>
#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class SettingsWindow;

class OnlinePatchesDialog final : public QDialog
{
	Q_OBJECT

public:
	/// Opens the dialog over `parent` and returns when it is closed. `kind` picks the page it serves.
	/// Returns true if anything was installed, in which case the settings window's settings have
	/// already been reloaded from disk and the page should rebuild its list.
	///
	/// `enable_cheats` is the Cheats page's "Enable Cheats" checkbox. Installing a cheat switches
	/// EmuCore/EnableCheats on in the game's settings file; the checkbox is brought in line with it.
	static bool browse(QWidget* parent, SettingsWindow* settings, Catalog::PatchKind kind, QCheckBox* enable_cheats = nullptr);

	OnlinePatchesDialog(QWidget* parent, SettingsWindow* settings, Catalog::PatchKind kind);
	~OnlinePatchesDialog() override;

	bool installedAnything() const { return m_installed_anything; }

	/// Makes the settings window's copy of the per-game settings file match the file on disk. Needed
	/// after PatchRepo::Install, which writes the file itself: a window that kept its old copy would
	/// drop the new names from [Patches]/[Cheats] Enable the next time anything on it saves. Returns false
	/// when the file could not be read; the window then keeps the settings it had.
	static bool reloadSettingsFromDisk(SettingsWindow* settings);

public Q_SLOTS:
	void reject() override;

private:
	struct InstallOutcome;

	void startSearch(Catalog::PatchSearchScope scope);
	void cancelSearch();
	void onSearchProgress(const Catalog::PatchSearchProgress& progress);
	void onSearchDone(Catalog::PatchSearchScope scope, Catalog::PatchSearchResult result);
	void installSelected();
	void onInstallDone(InstallOutcome outcome);

	void rebuildTree();
	void updateWidgets();
	int checkedCount() const;
	void showMessage(const QString& text);
	void updateTitle();
	QString notice() const;

	SettingsWindow* m_settings;
	Catalog::PatchKind m_kind;
	std::string m_serial;
	u32 m_crc;

	std::unique_ptr<Catalog::PatchSearchJob> m_job;
	Catalog::PatchSearchScope m_job_scope = Catalog::PatchSearchScope::Community;
	u32 m_search_id = 0;
	bool m_searching = false;
	bool m_installing = false;
	bool m_closing = false;
	bool m_large_searched = false;
	bool m_hardcore_active = false;
	bool m_incomplete = false;
	bool m_installed_anything = false;

	std::string m_game_title;
	std::vector<Catalog::PatchEntry> m_entries; ///< Everything found, both kinds. Only m_kind is shown.
	size_t m_other_kind_count = 0;

	QLabel* m_title_label;
	QLabel* m_notice_label;
	QWidget* m_progress_row;
	QLabel* m_progress_label;
	QProgressBar* m_progress_bar;
	QPushButton* m_cancel_button;
	QLabel* m_message_label;
	QTreeWidget* m_tree;
	QLabel* m_status_label;
	QPushButton* m_retry_button;
	QPushButton* m_large_button;
	QPushButton* m_install_button;
	QPushButton* m_close_button;
};
