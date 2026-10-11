// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Settings/MemoryCardBackupsDialog.h"

#include "QtAsyncCall.h"
#include "QtHost.h"
#include "QtShot.h"
#include "QtUtils.h"

#include "pcsx2/SIO/Memcard/MemoryCardFile.h"
#include "pcsx2/VMManager.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtGui/QBrush>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

namespace
{
	constexpr int SNAPSHOT_INDEX_ROLE = Qt::UserRole;

	QString reasonText(MemcardBackup::Reason reason)
	{
		switch (reason)
		{
			case MemcardBackup::Reason::Session:
				return MemoryCardBackupsDialog::tr("Game start");
			case MemcardBackup::Reason::Manual:
				return MemoryCardBackupsDialog::tr("Back Up Now");
			case MemcardBackup::Reason::PreRestore:
				return MemoryCardBackupsDialog::tr("Before a restore");
			case MemcardBackup::Reason::Save:
			default:
				return MemoryCardBackupsDialog::tr("Save");
		}
	}
} // namespace

struct MemoryCardBackupsDialog::ActionOutcome
{
	bool restore = false;
	MemcardBackup::Outcome outcome;
};

struct MemoryCardBackupsDialog::LoadOutcome
{
	std::string card; ///< The card the snapshots are of.
	std::vector<std::string> cards_with_backups;
	std::vector<MemcardBackup::Snapshot> snapshots;
};

bool MemoryCardBackupsDialog::showForCard(QWidget* parent, const QString& card)
{
	MemoryCardBackupsDialog dialog(QtUtils::GetRootWidget(parent), card);
	dialog.exec();
	return dialog.restoredAnything();
}

MemoryCardBackupsDialog::MemoryCardBackupsDialog(QWidget* parent, const QString& card)
	: QDialog(parent)
	, m_card(card.toStdString())
{
	setWindowTitle(tr("Memory Card Backups"));
	setWindowIcon(QtHost::GetAppIcon());
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(720, 420);

	QVBoxLayout* layout = new QVBoxLayout(this);

	QHBoxLayout* card_row = new QHBoxLayout();
	QLabel* card_label = new QLabel(tr("Card:"), this);
	m_card_combo = new QComboBox(this);
	m_card_combo->setObjectName(QStringLiteral("backupsCardCombo"));
	card_label->setBuddy(m_card_combo);
	card_row->addWidget(card_label);
	card_row->addWidget(m_card_combo, 1);
	layout->addLayout(card_row);

	// The cards in the folder, plus (once the list is read) any card that only has backups left.
	QStringList names;
	for (const AvailableMcdInfo& mcd : FileMcd_GetAvailableCards(true))
		names << QString::fromStdString(mcd.name);
	names.sort();
	m_card_combo->addItems(names);
	if (!card.isEmpty() && m_card_combo->findText(card) < 0)
		m_card_combo->addItem(card);
	if (!card.isEmpty())
		m_card_combo->setCurrentText(card);
	m_card = m_card_combo->currentText().toStdString();

	QLabel* explain = new QLabel(
		tr("A snapshot of the card is taken when a game starts, if Automatic backups is on and the card has changed since the last one. "
		   "Restoring puts a snapshot back over the card; the card as it is now is kept as a \"Before a restore\" snapshot first."),
		this);
	explain->setWordWrap(true);
	layout->addWidget(explain);

	m_message_label = new QLabel(this);
	m_message_label->setObjectName(QStringLiteral("backupsMessage"));
	m_message_label->setWordWrap(true);
	m_message_label->setAlignment(Qt::AlignCenter);
	layout->addWidget(m_message_label, 1);

	m_tree = new QTreeWidget(this);
	m_tree->setObjectName(QStringLiteral("backupsList"));
	m_tree->setColumnCount(5);
	m_tree->setHeaderLabels({tr("Date"), tr("Reason"), tr("Size"), tr("Status"), tr("Game")});
	m_tree->setRootIsDecorated(false);
	m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
	m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_tree->header()->setStretchLastSection(true);
	m_tree->setColumnWidth(0, 180);
	m_tree->setColumnWidth(1, 120);
	m_tree->setColumnWidth(2, 80);
	m_tree->setColumnWidth(3, 150);
	layout->addWidget(m_tree, 1);

	m_status_label = new QLabel(this);
	m_status_label->setObjectName(QStringLiteral("backupsStatus"));
	m_status_label->setWordWrap(true);
	layout->addWidget(m_status_label);

	QHBoxLayout* buttons = new QHBoxLayout();
	m_restore_button = new QPushButton(tr("Restore..."), this);
	m_restore_button->setObjectName(QStringLiteral("restoreBackupButton"));
	m_backup_button = new QPushButton(tr("Back Up Now"), this);
	m_backup_button->setObjectName(QStringLiteral("backUpNowButton"));
	m_close_button = new QPushButton(tr("Close"), this);
	m_close_button->setObjectName(QStringLiteral("backupsCloseButton"));
	m_close_button->setDefault(true);
	buttons->addWidget(m_restore_button);
	buttons->addWidget(m_backup_button);
	buttons->addStretch(1);
	buttons->addWidget(m_close_button);
	layout->addLayout(buttons);

	connect(m_restore_button, &QPushButton::clicked, this, &MemoryCardBackupsDialog::restoreSelected);
	connect(m_backup_button, &QPushButton::clicked, this, &MemoryCardBackupsDialog::backUpNow);
	connect(m_close_button, &QPushButton::clicked, this, &QDialog::accept);
	connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &MemoryCardBackupsDialog::updateWidgets);
	connect(m_card_combo, &QComboBox::activated, this, [this]() {
		m_card = m_card_combo->currentText().toStdString();
		m_status_label->clear();
		reloadList();
	});

	reloadList();
}

MemoryCardBackupsDialog::~MemoryCardBackupsDialog()
{
	QtAsyncCall::WaitForAll(this);
}

bool MemoryCardBackupsDialog::gameIsRunning() const
{
	// The same test the service applies, so the buttons are not enabled for a call it would refuse.
	return VMManager::GetState() != VMState::Shutdown;
}

void MemoryCardBackupsDialog::reloadList()
{
	if (m_card.empty())
	{
		m_message_label->setText(tr("There are no memory cards."));
		updateWidgets();
		return;
	}

	m_loading = true;
	m_message_label->setText(tr("Reading backups..."));
	updateWidgets();

	m_tree->clear();
	m_snapshots.clear();
	QtAsyncCall::Run(
		this,
		[card = m_card]() {
			LoadOutcome loaded;
			const MemcardBackup::Store store = MemcardBackup::Store::ForDataRoot();
			loaded.card = card;
			loaded.cards_with_backups = store.CardsWithBackups();
			loaded.snapshots = store.List(card);
			return loaded;
		},
		[this](LoadOutcome loaded) { onListLoaded(std::move(loaded)); });
}

void MemoryCardBackupsDialog::onListLoaded(LoadOutcome loaded)
{
	m_loading = false;
	if (loaded.card != m_card)
		return; // the choice changed while this was being read; its own read is on the way

	for (const std::string& name : loaded.cards_with_backups)
	{
		const QString qname = QString::fromStdString(name);
		if (m_card_combo->findText(qname) < 0)
			m_card_combo->addItem(qname);
	}
	m_snapshots = std::move(loaded.snapshots);
	m_message_label->setText(m_snapshots.empty() ? tr("There are no backups of this card yet.") : QString());

	m_tree->clear();
	const QLocale locale = QLocale::system();
	for (size_t i = 0; i < m_snapshots.size(); i++)
	{
		const MemcardBackup::Snapshot& snapshot = m_snapshots[i];
		QTreeWidgetItem* item = new QTreeWidgetItem(m_tree);
		item->setData(0, SNAPSHOT_INDEX_ROLE, static_cast<int>(i));
		item->setText(0, locale.toString(QDateTime::fromMSecsSinceEpoch(snapshot.taken_at_ms), QLocale::ShortFormat));
		item->setText(1, reasonText(snapshot.reason));
		item->setText(2, locale.formattedDataSize(static_cast<qint64>(snapshot.size_bytes)));
		if (snapshot.healthy)
		{
			item->setText(3, tr("Verified"));
		}
		else
		{
			item->setText(3, tr("Unreadable when saved"));
			item->setForeground(3, QBrush(QColor(0xd9, 0x53, 0x4f)));
		}
		item->setText(4, QString::fromStdString(snapshot.game));
		item->setToolTip(0, QString::fromStdString(snapshot.path));
	}
	if (m_tree->topLevelItemCount() > 0)
		m_tree->setCurrentItem(m_tree->topLevelItem(0));

	updateWidgets();
}

void MemoryCardBackupsDialog::updateWidgets()
{
	const bool running = gameIsRunning();
	const bool has_selection = !m_tree->selectedItems().isEmpty();

	m_tree->setVisible(!m_snapshots.empty());
	m_message_label->setVisible(!m_message_label->text().isEmpty());
	m_status_label->setVisible(!m_status_label->text().isEmpty());

	const QString running_tip = tr("A game is running. Stop it first: the memory card cannot be replaced or backed up while the console is using it.");
	m_restore_button->setEnabled(!running && !m_busy && !m_loading && has_selection);
	m_restore_button->setToolTip(running ? running_tip : QString());
	m_backup_button->setEnabled(!running && !m_busy && !m_loading);
	m_backup_button->setToolTip(running ? running_tip : QString());
	m_close_button->setEnabled(!m_busy);
	m_card_combo->setEnabled(!m_busy && !m_loading);
}

void MemoryCardBackupsDialog::restoreSelected()
{
	const QList<QTreeWidgetItem*> selection = m_tree->selectedItems();
	if (selection.isEmpty())
		return;

	const MemcardBackup::Snapshot snapshot = m_snapshots[selection[0]->data(0, SNAPSHOT_INDEX_ROLE).toInt()];
	const QString when = QLocale::system().toString(QDateTime::fromMSecsSinceEpoch(snapshot.taken_at_ms), QLocale::ShortFormat);
	if (QMessageBox::question(this, tr("Restore Backup"),
			tr("Replace the memory card '%1' with the backup from %2?\n\nThe card as it is now is kept as a \"Before a restore\" backup, so this can be undone.")
				.arg(QString::fromStdString(m_card), when)) != QMessageBox::Yes)
	{
		return;
	}

	m_busy = true;
	m_status_label->setText(tr("Restoring..."));
	updateWidgets();
	QtAsyncCall::Run(
		this,
		[snapshot]() {
			ActionOutcome result;
			result.restore = true;
			result.outcome = MemcardBackup::RestoreSnapshot(snapshot);
			return result;
		},
		[this](ActionOutcome outcome) { onActionDone(std::move(outcome)); });
}

void MemoryCardBackupsDialog::backUpNow()
{
	m_busy = true;
	m_status_label->setText(tr("Backing up..."));
	updateWidgets();
	QtAsyncCall::Run(
		this,
		[card = m_card]() {
			ActionOutcome result;
			result.restore = false;
			result.outcome = MemcardBackup::BackUpCardNow(card);
			return result;
		},
		[this](ActionOutcome outcome) { onActionDone(std::move(outcome)); });
}

void MemoryCardBackupsDialog::onActionDone(ActionOutcome outcome)
{
	m_busy = false;
	const QString detail = QString::fromStdString(outcome.outcome.detail);
	if (outcome.outcome.ok)
	{
		m_restored = m_restored || outcome.restore;
		m_status_label->setText(detail);
	}
	else
	{
		m_status_label->clear();
		QMessageBox::critical(this, outcome.restore ? tr("Restore Backup") : tr("Back Up Now"), detail);
	}
	reloadList();
}

QTSHOT_DIALOG("memcard-backups", "Memory Cards > Backups... for Mcd001.ps2",
	[](const QtShot::Context&) { return new MemoryCardBackupsDialog(nullptr, QStringLiteral("Mcd001.ps2")); });

#include "moc_MemoryCardBackupsDialog.cpp"
