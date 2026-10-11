// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The "Backups..." window of the Memory Cards page: the snapshots MemcardBackup has taken of one card,
// newest first, with Restore and Back Up Now. Reading the snapshot list, restoring and backing up all
// run on a worker thread (they hash and unpack the whole card); the window stays responsive meanwhile.
//
// Restoring and backing up are refused by the service while a VM exists (the console keeps its own
// picture of the card in guest memory), so both buttons are disabled with an explanation while a game
// runs. The state is read when the window opens and again after each action.

#include "pcsx2/SIO/Memcard/MemcardBackup.h"

#include <QtWidgets/QDialog>

#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

class MemoryCardBackupsDialog final : public QDialog
{
	Q_OBJECT

public:
	/// Opens the window over `parent`, starting on the card called `card` ("Mcd001.ps2"; empty = the
	/// first card), and returns when it is closed. The window has a card chooser, because the cards
	/// in the console's slots cannot be selected in the page's list. Returns true if a card on disk
	/// was replaced by a restore.
	static bool showForCard(QWidget* parent, const QString& card);

	MemoryCardBackupsDialog(QWidget* parent, const QString& card);
	~MemoryCardBackupsDialog() override;

	bool restoredAnything() const { return m_restored; }

private:
	struct ActionOutcome;
	struct LoadOutcome;

	void reloadList();
	void onListLoaded(LoadOutcome loaded);
	void restoreSelected();
	void backUpNow();
	void onActionDone(ActionOutcome outcome);
	void updateWidgets();
	bool gameIsRunning() const;

	std::string m_card;
	std::vector<MemcardBackup::Snapshot> m_snapshots;
	bool m_loading = false;
	bool m_busy = false;
	bool m_restored = false;

	QComboBox* m_card_combo;
	QLabel* m_message_label;
	QTreeWidget* m_tree;
	QLabel* m_status_label;
	QPushButton* m_restore_button;
	QPushButton* m_backup_button;
	QPushButton* m_close_button;
};
