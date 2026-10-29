// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Settings/OnlinePatchesDialog.h"

#include "QtAsyncCall.h"
#include "QtHost.h"
#include "QtUtils.h"
#include "Settings/SettingsWindow.h"

#include "pcsx2/INISettingsInterface.h"

#include "common/FileSystem.h"

#include <QtCore/QSet>
#include <QtGui/QColor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace
{
	// One repository client for the process: it remembers the file lists it fetched and which ones are
	// in a back-off, which is the point of keeping it between dialogs.
	Catalog::PatchRepo& sharedRepo()
	{
		static Catalog::PatchRepo repo(Catalog::GetDefaultFetcher());
		return repo;
	}

	constexpr int ENTRY_INDEX_ROLE = Qt::UserRole;

	QColor sourceColor(Catalog::PatchSourceId source)
	{
		switch (source)
		{
			case Catalog::PatchSourceId::Pcsx2Patches:
				return QColor(0x2b, 0x6c, 0xb0);
			case Catalog::PatchSourceId::Gabominated:
				return QColor(0x6b, 0x46, 0xc1);
			case Catalog::PatchSourceId::Shadowninja826:
				return QColor(0x2f, 0x85, 0x5a);
			case Catalog::PatchSourceId::Xs1l3n7x:
				return QColor(0xb7, 0x79, 0x1f);
			case Catalog::PatchSourceId::XiGuanChi:
			default:
				return QColor(0xc0, 0x56, 0x21);
		}
	}

	QString entryKey(const Catalog::PatchEntry& entry)
	{
		return QString::fromStdString(entry.file_path) + QLatin1Char('\n') + QString::fromStdString(entry.name);
	}

	QWidget* makeGroupHeader(QWidget* parent, Catalog::PatchSourceId source, size_t count)
	{
		QWidget* header = new QWidget(parent);
		QHBoxLayout* layout = new QHBoxLayout(header);
		layout->setContentsMargins(2, 2, 2, 2);

		QLabel* chip = new QLabel(QString::fromUtf8(Catalog::PatchSourceLabel(source)), header);
		chip->setStyleSheet(QStringLiteral("QLabel { background-color: %1; color: white; border-radius: 8px; padding: 1px 9px; font-weight: 600; }")
								.arg(sourceColor(source).name()));
		layout->addWidget(chip);

		QLabel* detail = new QLabel(OnlinePatchesDialog::tr("%1  -  %n item(s)", "", static_cast<int>(count))
										.arg(QString::fromUtf8(Catalog::PatchSourceRepository(source))),
			header);
		detail->setEnabled(false);
		layout->addWidget(detail);
		layout->addStretch(1);
		return header;
	}
} // namespace

struct OnlinePatchesDialog::InstallOutcome
{
	Catalog::PatchInstallResult result;
	std::vector<Catalog::PatchEntry> entries;
};

bool OnlinePatchesDialog::browse(QWidget* parent, SettingsWindow* settings, Catalog::PatchKind kind, QCheckBox* enable_cheats)
{
	OnlinePatchesDialog dialog(QtUtils::GetRootWidget(parent), settings, kind);
	dialog.exec();
	if (!dialog.installedAnything())
		return false;

	if (enable_cheats)
	{
		// Installing a cheat switches EnableCheats on in the game's own settings file.
		const std::optional<bool> enabled = settings->getBoolValue("EmuCore", "EnableCheats", std::nullopt);
		enable_cheats->setCheckState(enabled.has_value() ? (*enabled ? Qt::Checked : Qt::Unchecked) : Qt::PartiallyChecked);
	}
	return true;
}

bool OnlinePatchesDialog::reloadSettingsFromDisk(SettingsWindow* settings)
{
	if (!settings->isPerGameSettings())
		return true;

	INISettingsInterface* ini = static_cast<INISettingsInterface*>(settings->getSettingsInterface());
	const std::string path = ini->GetFileName();
	if (!FileSystem::FileExists(path.c_str()))
		return true; // nothing was written, so there is nothing to catch up with

	// Read the file into a scratch copy first. A file that cannot be read must leave the window's settings as
	// they are: clearing them and then failing to load would leave the window empty, and its next save would
	// write that empty copy over the game's settings.
	INISettingsInterface scratch(path);
	if (!scratch.Load())
		return false;

	ini->Clear();
	return ini->Load();
}

OnlinePatchesDialog::OnlinePatchesDialog(QWidget* parent, SettingsWindow* settings, Catalog::PatchKind kind)
	: QDialog(parent)
	, m_settings(settings)
	, m_kind(kind)
	, m_serial(settings->getSerial())
	, m_crc(settings->getDiscCRC())
{
	const bool cheats = (kind == Catalog::PatchKind::Cheat);
	setWindowTitle(cheats ? tr("Browse Online Cheats") : tr("Browse Online Patches"));
	setWindowIcon(QtHost::GetAppIcon());
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	resize(780, 540);

	QVBoxLayout* layout = new QVBoxLayout(this);

	m_title_label = new QLabel(this);
	m_title_label->setObjectName(QStringLiteral("onlinePatchesTitle"));
	m_title_label->setTextFormat(Qt::RichText);
	layout->addWidget(m_title_label);

	m_notice_label = new QLabel(this);
	m_notice_label->setObjectName(QStringLiteral("onlinePatchesNotice"));
	m_notice_label->setWordWrap(true);
	m_notice_label->setTextFormat(Qt::RichText);
	layout->addWidget(m_notice_label);

	m_progress_row = new QWidget(this);
	QHBoxLayout* progress_layout = new QHBoxLayout(m_progress_row);
	progress_layout->setContentsMargins(0, 0, 0, 0);
	m_progress_label = new QLabel(m_progress_row);
	m_progress_bar = new QProgressBar(m_progress_row);
	m_progress_bar->setObjectName(QStringLiteral("onlinePatchesProgress"));
	m_progress_bar->setRange(0, 0);
	m_cancel_button = new QPushButton(tr("Cancel"), m_progress_row);
	m_cancel_button->setObjectName(QStringLiteral("onlinePatchesCancel"));
	progress_layout->addWidget(m_progress_label, 1);
	progress_layout->addWidget(m_progress_bar, 2);
	progress_layout->addWidget(m_cancel_button);
	layout->addWidget(m_progress_row);

	m_message_label = new QLabel(this);
	m_message_label->setObjectName(QStringLiteral("onlinePatchesMessage"));
	m_message_label->setWordWrap(true);
	m_message_label->setAlignment(Qt::AlignCenter);
	layout->addWidget(m_message_label, 1);

	m_tree = new QTreeWidget(this);
	m_tree->setObjectName(QStringLiteral("onlinePatchesTree"));
	m_tree->setColumnCount(3);
	m_tree->setHeaderLabels({tr("Name"), tr("Description"), tr("Status")});
	m_tree->setRootIsDecorated(true);
	m_tree->setUniformRowHeights(false);
	m_tree->setSelectionMode(QAbstractItemView::NoSelection);
	m_tree->header()->setStretchLastSection(false);
	m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
	m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_tree->setColumnWidth(0, 300);
	layout->addWidget(m_tree, 1);

	m_status_label = new QLabel(this);
	m_status_label->setObjectName(QStringLiteral("onlinePatchesStatus"));
	m_status_label->setWordWrap(true);
	layout->addWidget(m_status_label);

	QHBoxLayout* buttons = new QHBoxLayout();
	m_large_button = new QPushButton(tr("Search the large cheat database"), this);
	m_large_button->setObjectName(QStringLiteral("searchLargeCheatDatabase"));
	m_large_button->setToolTip(tr("Searches a much bigger collection of cheat files (about 11,000). Its file list is several MB, so it is only fetched when asked for."));
	m_large_button->setVisible(cheats);
	m_retry_button = new QPushButton(tr("Search Again"), this);
	m_retry_button->setObjectName(QStringLiteral("onlinePatchesRetry"));
	m_install_button = new QPushButton(tr("Install Selected"), this);
	m_install_button->setObjectName(QStringLiteral("installSelectedButton"));
	m_install_button->setDefault(true);
	m_close_button = new QPushButton(tr("Close"), this);
	m_close_button->setObjectName(QStringLiteral("onlinePatchesClose"));
	buttons->addWidget(m_large_button);
	buttons->addWidget(m_retry_button);
	buttons->addStretch(1);
	buttons->addWidget(m_install_button);
	buttons->addWidget(m_close_button);
	layout->addLayout(buttons);

	connect(m_cancel_button, &QPushButton::clicked, this, &OnlinePatchesDialog::cancelSearch);
	connect(m_close_button, &QPushButton::clicked, this, &OnlinePatchesDialog::reject);
	connect(m_install_button, &QPushButton::clicked, this, &OnlinePatchesDialog::installSelected);
	connect(m_retry_button, &QPushButton::clicked, this, [this]() { startSearch(Catalog::PatchSearchScope::Community); });
	connect(m_large_button, &QPushButton::clicked, this, [this]() { startSearch(Catalog::PatchSearchScope::LargeCheatDatabase); });
	connect(m_tree, &QTreeWidget::itemChanged, this, [this]() { updateWidgets(); });

	updateTitle();

	if (m_serial.empty() && m_crc == 0)
	{
		showMessage(tr("This game has no serial number or CRC to look up patches by."));
		updateWidgets();
		return;
	}

	startSearch(Catalog::PatchSearchScope::Community);
}

OnlinePatchesDialog::~OnlinePatchesDialog()
{
	// The job's thread calls back into this object until it has ended.
	m_job.reset();
	QtAsyncCall::WaitForAll(this);
}

void OnlinePatchesDialog::reject()
{
	if (m_installing)
		return;

	if (m_searching && m_job)
	{
		// One request may be in flight for a few seconds. Stay open and responsive until it stops.
		m_closing = true;
		m_job->Cancel();
		m_status_label->setText(tr("Stopping the search..."));
		updateWidgets();
		return;
	}

	QDialog::reject();
}

void OnlinePatchesDialog::startSearch(Catalog::PatchSearchScope scope)
{
	Catalog::PatchSearchRequest request;
	request.serial = m_serial;
	request.crc = m_crc;
	request.scope = scope;
	if (scope == Catalog::PatchSearchScope::LargeCheatDatabase)
	{
		for (const Catalog::PatchEntry& entry : m_entries)
		{
			if (entry.kind == Catalog::PatchKind::Cheat)
				request.already_listed.push_back(entry.name);
		}
	}

	const u32 id = ++m_search_id;
	m_job_scope = scope;
	m_searching = true;
	m_status_label->clear();
	m_message_label->clear();
	m_progress_bar->setRange(0, 0);
	m_progress_label->setText(scope == Catalog::PatchSearchScope::Community ? tr("Searching the community repositories...") :
																				 tr("Searching the large cheat database..."));
	updateWidgets();

	m_job = sharedRepo().StartSearch(
		std::move(request),
		[this, id](const Catalog::PatchSearchProgress& progress) {
			QMetaObject::invokeMethod(
				this, [this, id, progress]() {
					if (id == m_search_id)
						onSearchProgress(progress);
				},
				Qt::QueuedConnection);
		},
		[this, id, scope](Catalog::PatchSearchResult result) {
			QMetaObject::invokeMethod(
				this, [this, id, scope, result = std::move(result)]() mutable {
					if (id == m_search_id)
						onSearchDone(scope, std::move(result));
				},
				Qt::QueuedConnection);
		});
}

void OnlinePatchesDialog::cancelSearch()
{
	if (m_searching && m_job)
	{
		m_job->Cancel();
		m_cancel_button->setEnabled(false);
		m_progress_label->setText(tr("Stopping..."));
	}
}

void OnlinePatchesDialog::onSearchProgress(const Catalog::PatchSearchProgress& progress)
{
	if (!m_searching || m_closing)
		return;

	m_progress_label->setText(QString::fromStdString(progress.message));
	if (progress.total > 0)
	{
		m_progress_bar->setRange(0, static_cast<int>(progress.total));
		m_progress_bar->setValue(static_cast<int>(progress.done));
	}
}

void OnlinePatchesDialog::onSearchDone(Catalog::PatchSearchScope scope, Catalog::PatchSearchResult result)
{
	m_searching = false;
	m_status_label->clear();

	if (m_closing)
	{
		QDialog::reject();
		return;
	}

	const bool community = (scope == Catalog::PatchSearchScope::Community);
	if (!result.game_title.empty() && m_game_title.empty())
	{
		m_game_title = result.game_title;
		updateTitle();
	}
	const QString game = QString::fromStdString(m_game_title.empty() ? m_serial : m_game_title);
	m_hardcore_active = m_hardcore_active || result.hardcore_active;

	switch (result.status)
	{
		case Catalog::PatchSearchStatus::Ok:
			if (community)
			{
				m_entries = std::move(result.entries);
				m_incomplete = result.incomplete;
			}
			else
			{
				m_large_searched = true;
				m_incomplete = m_incomplete || result.incomplete;
				if (result.entries.empty())
					m_status_label->setText(tr("The large cheat database has no more cheats for this game."));
				for (Catalog::PatchEntry& entry : result.entries)
					m_entries.push_back(std::move(entry));
			}
			break;

		case Catalog::PatchSearchStatus::NoMatches:
			if (community)
			{
				m_entries.clear();
				showMessage(tr("No patches or cheats in the database for %1.").arg(game));
			}
			else
			{
				m_large_searched = true;
				m_status_label->setText(tr("The large cheat database has no more cheats for this game."));
			}
			break;

		case Catalog::PatchSearchStatus::Unreachable:
			if (community)
			{
				showMessage(tr("Could not reach the online patch sources. They may be rate limiting this address; try again in a few minutes."));
			}
			else
			{
				m_status_label->setText(tr("Could not reach the large cheat database. Try again in a few minutes."));
			}
			break;

		case Catalog::PatchSearchStatus::Cancelled:
			if (community)
				showMessage(tr("The search was cancelled."));
			else
				m_status_label->setText(tr("The search was cancelled."));
			break;

		case Catalog::PatchSearchStatus::InvalidRequest:
			showMessage(tr("This game has no serial number or CRC to look up patches by."));
			break;
	}

	m_other_kind_count = static_cast<size_t>(std::count_if(m_entries.begin(), m_entries.end(), [this](const Catalog::PatchEntry& e) { return e.kind != m_kind; }));
	rebuildTree();
	updateWidgets();
}

void OnlinePatchesDialog::rebuildTree()
{
	QSet<QString> checked;
	for (int g = 0; g < m_tree->topLevelItemCount(); g++)
	{
		const QTreeWidgetItem* group = m_tree->topLevelItem(g);
		for (int c = 0; c < group->childCount(); c++)
		{
			const QTreeWidgetItem* item = group->child(c);
			if (item->checkState(0) == Qt::Checked)
				checked.insert(entryKey(m_entries[item->data(0, ENTRY_INDEX_ROLE).toInt()]));
		}
	}

	m_tree->blockSignals(true);
	m_tree->clear();

	const std::vector<Catalog::PatchSourceId> order = {
		Catalog::PatchSourceId::Pcsx2Patches,
		Catalog::PatchSourceId::Gabominated,
		Catalog::PatchSourceId::Shadowninja826,
		Catalog::PatchSourceId::Xs1l3n7x,
		Catalog::PatchSourceId::XiGuanChi,
	};
	for (const Catalog::PatchSourceId source : order)
	{
		size_t count = 0;
		for (const Catalog::PatchEntry& entry : m_entries)
			count += (entry.source == source && entry.kind == m_kind) ? 1 : 0;
		if (count == 0)
			continue;

		QTreeWidgetItem* group = new QTreeWidgetItem(m_tree);
		group->setFirstColumnSpanned(true);
		group->setFlags(Qt::ItemIsEnabled);
		QWidget* header = makeGroupHeader(m_tree, source, count);
		group->setSizeHint(0, header->sizeHint());
		m_tree->setItemWidget(group, 0, header);

		for (size_t i = 0; i < m_entries.size(); i++)
		{
			const Catalog::PatchEntry& entry = m_entries[i];
			if (entry.source != source || entry.kind != m_kind)
				continue;

			QTreeWidgetItem* item = new QTreeWidgetItem(group);
			item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
			item->setCheckState(0, checked.contains(entryKey(entry)) ? Qt::Checked : Qt::Unchecked);
			item->setData(0, ENTRY_INDEX_ROLE, static_cast<int>(i));
			item->setText(0, QString::fromStdString(entry.name));
			item->setText(1, QString::fromStdString(entry.description).simplified());
			const QString tip = QString::fromStdString(entry.description) + QLatin1Char('\n') + QString::fromStdString(entry.file_path);
			item->setToolTip(0, tip.trimmed());
			item->setToolTip(1, tip.trimmed());
			if (entry.installed)
			{
				item->setText(2, tr("Installed"));
				item->setForeground(2, QBrush(QColor(0x38, 0xa1, 0x69)));
			}
		}
		group->setExpanded(true);
	}
	m_tree->blockSignals(false);
}

int OnlinePatchesDialog::checkedCount() const
{
	int count = 0;
	for (int g = 0; g < m_tree->topLevelItemCount(); g++)
	{
		const QTreeWidgetItem* group = m_tree->topLevelItem(g);
		for (int c = 0; c < group->childCount(); c++)
			count += (group->child(c)->checkState(0) == Qt::Checked) ? 1 : 0;
	}
	return count;
}

void OnlinePatchesDialog::updateTitle()
{
	const QString crc = (m_crc != 0) ? QStringLiteral("%1").arg(m_crc, 8, 16, QLatin1Char('0')).toUpper() : tr("unknown");
	const QString serial = QString::fromStdString(m_serial.empty() ? std::string("?") : m_serial).toHtmlEscaped();
	if (m_game_title.empty())
		m_title_label->setText(tr("<b>%1</b> &nbsp; CRC %2").arg(serial, crc));
	else if (m_game_title.find(m_serial) != std::string::npos)
		m_title_label->setText(tr("<b>%1</b> &nbsp; CRC %2").arg(QString::fromStdString(m_game_title).toHtmlEscaped(), crc)); // the repositories often put the serial in the title
	else
		m_title_label->setText(tr("<b>%1</b> &nbsp; %2 &nbsp; CRC %3").arg(QString::fromStdString(m_game_title).toHtmlEscaped(), serial, crc));
}

void OnlinePatchesDialog::showMessage(const QString& text)
{
	m_message_label->setText(text);
}

QString OnlinePatchesDialog::notice() const
{
	QStringList lines;
	if (m_crc == 0 && !m_serial.empty())
	{
		lines << tr("This game's disc CRC is not known, so nothing can be installed: patch files are named after it. You can still see what is available.");
	}
	if (m_hardcore_active)
	{
		lines << tr("Achievements hardcore mode is on. The emulator ignores cheats (and patches other than display tweaks) while it is, so anything installed now will not apply until hardcore mode is turned off.");
	}
	if (m_incomplete)
		lines << tr("Some sources could not be reached, so this list may be missing entries.");
	return lines.join(QStringLiteral("<br>"));
}

void OnlinePatchesDialog::updateWidgets()
{
	const bool busy = m_searching || m_installing;
	const bool has_rows = (m_tree->topLevelItemCount() > 0);

	const QString notice_text = notice();
	m_notice_label->setText(notice_text);
	m_notice_label->setVisible(!notice_text.isEmpty());

	m_progress_row->setVisible(m_searching);
	m_cancel_button->setEnabled(m_searching && !m_closing);

	// With nothing of this kind to list, say why instead of showing an empty tree.
	if (!has_rows && !m_searching && m_message_label->text().isEmpty() && !m_entries.empty())
	{
		if (m_kind == Catalog::PatchKind::Patch)
		{
			m_message_label->setText(m_other_kind_count > 0 ?
										 tr("No patches in the database for this game. There are %n cheat(s) for it on the Cheats page.", "", static_cast<int>(m_other_kind_count)) :
										 tr("No patches in the database for this game."));
		}
		else
		{
			m_message_label->setText(tr("No cheats in the community repositories for this game. The large cheat database may have some."));
		}
	}
	m_message_label->setVisible(!has_rows); // takes the room the list would, so the rows above and below keep their places
	m_tree->setVisible(has_rows);
	m_status_label->setVisible(!m_status_label->text().isEmpty());

	const int checked = checkedCount();
	m_install_button->setText(checked > 0 ? tr("Install Selected (%1)").arg(checked) : tr("Install Selected"));
	m_install_button->setEnabled(!busy && !m_closing && m_crc != 0 && !m_serial.empty() && checked > 0);
	if (m_crc == 0)
		m_install_button->setToolTip(tr("The game's disc CRC is not known, so nothing can be installed for it."));
	else
		m_install_button->setToolTip(QString());

	m_large_button->setEnabled(!busy && !m_closing && !m_large_searched && (!m_serial.empty() || m_crc != 0));
	m_retry_button->setVisible(!m_searching && m_entries.empty() && (!m_serial.empty() || m_crc != 0));
	m_retry_button->setEnabled(!busy && !m_closing);
	m_close_button->setEnabled(!m_installing);
}

void OnlinePatchesDialog::installSelected()
{
	Catalog::PatchInstallRequest request;
	request.serial = m_serial;
	request.crc = m_crc;
	request.game_title = m_game_title;
	for (int g = 0; g < m_tree->topLevelItemCount(); g++)
	{
		const QTreeWidgetItem* group = m_tree->topLevelItem(g);
		for (int c = 0; c < group->childCount(); c++)
		{
			const QTreeWidgetItem* item = group->child(c);
			if (item->checkState(0) == Qt::Checked)
				request.entries.push_back(m_entries[item->data(0, ENTRY_INDEX_ROLE).toInt()]);
		}
	}
	if (request.entries.empty())
		return;

	m_installing = true;
	m_status_label->setText(tr("Installing..."));
	updateWidgets();

	// Install and the follow-up scan of the new files are file I/O only, but they are still off the UI thread.
	std::vector<Catalog::PatchEntry> entries = m_entries;
	QtAsyncCall::Run(
		this,
		[request = std::move(request), entries = std::move(entries)]() mutable {
			InstallOutcome outcome;
			outcome.result = sharedRepo().Install(request);
			sharedRepo().UpdateInstalledFlags(entries, request.serial, request.crc);
			outcome.entries = std::move(entries);
			return outcome;
		},
		[this](InstallOutcome outcome) { onInstallDone(std::move(outcome)); });
}

void OnlinePatchesDialog::onInstallDone(InstallOutcome outcome)
{
	m_installing = false;
	m_status_label->clear();
	const Catalog::PatchInstallResult& result = outcome.result;

	switch (result.status)
	{
		case Catalog::PatchInstallStatus::Ok:
		{
			m_installed_anything = true;
			m_entries = std::move(outcome.entries);

			// The install wrote the game's settings file behind the settings window's back.
			if (!reloadSettingsFromDisk(m_settings))
			{
				QMessageBox::warning(this, tr("Install"),
					tr("The game's settings file could not be read back, so its settings window may not know about the new items. "
					   "Close the game's properties and open them again before changing anything else there."));
			}

			const u32 added = result.patches_added + result.cheats_added;
			QStringList sentences;
			if (added > 0)
				sentences << tr("Installed %n item(s).", "", static_cast<int>(added));
			if (result.already_present > 0)
				sentences << (added > 0 ? tr("%n of the selected items were already installed.", "", static_cast<int>(result.already_present)) :
										  tr("The selected items were already installed. They are switched on."));
			sentences << (result.reload_requested ? tr("The running game was told to reload its patches.") :
													tr("They take effect the next time the game starts."));
			if (result.cheats_switched_on)
				sentences << tr("Cheats were switched on for this game.");
			if (result.hardcore_active)
			{
				m_hardcore_active = true;
				sentences << tr("Hardcore mode is on, so the emulator ignores cheats until it is turned off.");
			}
			m_status_label->setText(sentences.join(QLatin1Char(' ')));

			rebuildTree();
			break;
		}

		case Catalog::PatchInstallStatus::CrcUnknown:
			QMessageBox::warning(this, tr("Install"), tr("The game's disc CRC is not known, so nothing was installed."));
			break;

		case Catalog::PatchInstallStatus::NothingSelected:
			break;

		case Catalog::PatchInstallStatus::InvalidSerial:
			QMessageBox::warning(this, tr("Install"), tr("The game's serial number is not usable as a file name, so nothing was installed."));
			break;

		case Catalog::PatchInstallStatus::WriteFailed:
			QMessageBox::critical(this, tr("Install"), tr("Couldn't install the selected items.\n\n%1").arg(QString::fromStdString(result.error)));
			break;
	}

	updateWidgets();
}

#include "moc_OnlinePatchesDialog.cpp"
