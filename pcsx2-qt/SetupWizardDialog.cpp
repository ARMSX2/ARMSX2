// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/Achievements.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "QtHost.h"
#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "Settings/AchievementLoginDialog.h"
#include "Settings/BIOSSettingsWidget.h"
#include "Settings/ControllerSettingWidgetBinder.h"
#include "Settings/InterfaceSettingsWidget.h"
#include "SetupWizardDialog.h"

#include "Catalog/Fetch.h"

#include "common/StringUtil.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QThreadPool>
#include <QtGui/QGuiApplication>

namespace
{
	QString driverSizeText(u64 bytes)
	{
		const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
		return QCoreApplication::translate("SetupWizardDialog", "%1 MB").arg(mib, 0, 'f', mib < 10.0 ? 1 : 0);
	}
} // namespace

SetupWizardDialog::SetupWizardDialog()
{
	setupUi();
	updatePageLabels(-1);
	updatePageButtons();
}

SetupWizardDialog::~SetupWizardDialog()
{
	// A download still running when the wizard closes stops at its next check.
	m_driver_cancel.Cancel();
}

void SetupWizardDialog::resizeEvent(QResizeEvent* event)
{
	QDialog::resizeEvent(event);
	resizeDirectoryListColumns();
}

bool SetupWizardDialog::canShowNextPage()
{
	const int current_page = m_ui.pages->currentIndex();

	switch (current_page)
	{
		case Page_BIOS:
		{
			if (!m_ui.biosList->currentItem())
			{
				if (QMessageBox::question(this, tr("Warning"),
						tr("A BIOS image has not been selected. ARMSX2 <strong>will not</strong> be able to run games "
						   "without a BIOS image.<br><br>Are you sure you wish to continue without selecting a BIOS "
						   "image?")) != QMessageBox::Yes)
				{
					return false;
				}
			}
		}
		break;

		case Page_GameList:
		{
			if (m_ui.searchDirectoryList->rowCount() == 0)
			{
				if (QMessageBox::question(this, tr("Warning"),
						tr("No game directories have been selected. You will have to manually open any game dumps you "
						   "want to play, PCSX2's list will be empty.\n\nAre you sure you want to continue?")) !=
					QMessageBox::Yes)
				{
					return false;
				}
			}
		}
		break;

		default:
			break;
	}

	return true;
}

int SetupWizardDialog::pageAfter(int page) const
{
	const int next = page + 1;
	return (next == Page_Driver && !m_driver_page_enabled) ? next + 1 : next;
}

int SetupWizardDialog::pageBefore(int page) const
{
	const int previous = page - 1;
	return (previous == Page_Driver && !m_driver_page_enabled) ? previous - 1 : previous;
}

void SetupWizardDialog::previousPage()
{
	const int current_page = m_ui.pages->currentIndex();
	if (current_page == 0 || m_driver_installing)
		return;

	m_ui.pages->setCurrentIndex(pageBefore(current_page));
	updatePageLabels(current_page);
	updatePageButtons();
}

void SetupWizardDialog::nextPage()
{
	const int current_page = m_ui.pages->currentIndex();
	if (m_driver_installing)
		return;

	if (current_page == Page_Complete)
	{
		if (m_ui.createDesktopShortcut->isChecked())
			QtUtils::CreateShortcut(this, "ARMSX2", std::string(), {}, std::string(), std::string(), true, false);
		if (m_ui.addToApplicationMenu->isChecked())
			QtUtils::CreateShortcut(this, "ARMSX2", std::string(), {}, std::string(), std::string(), false, false);

		accept();
		return;
	}

	if (!canShowNextPage())
		return;

	if (current_page == Page_AutoTune)
	{
		Host::SetBaseBoolSettingValue("EmuCore", "AutoTune", m_ui.autoTuneEnabled->isChecked());
		Host::CommitBaseSettingChanges();
		g_emu_thread->applySettings();
	}

	const int new_page = pageAfter(current_page);
	m_ui.pages->setCurrentIndex(new_page);
	updatePageLabels(current_page);
	updatePageButtons();
	pageChangedTo(new_page);
}

void SetupWizardDialog::pageChangedTo(int page)
{
	switch (page)
	{
		case Page_GameList:
			resizeDirectoryListColumns();
			break;

		case Page_RetroAchievements:
			refreshRetroAchievementsLoginState();
			break;

		case Page_Driver:
			loadDriverOffer();
			break;

		default:
			break;
	}
}

void SetupWizardDialog::updatePageLabels(int prev_page)
{
	if (prev_page >= 0)
	{
		QFont prev_font = m_page_labels[prev_page]->font();
		prev_font.setBold(false);
		m_page_labels[prev_page]->setFont(prev_font);
	}

	const int page = m_ui.pages->currentIndex();
	QFont font = m_page_labels[page]->font();
	font.setBold(true);
	m_page_labels[page]->setFont(font);
}

void SetupWizardDialog::updatePageButtons()
{
	const int page = m_ui.pages->currentIndex();
	m_ui.next->setText((page == Page_Complete) ? tr("&Finish") : tr("&Next"));
	// The download is not left half done: Back and Next wait until it finishes or is cancelled.
	m_ui.back->setEnabled(page > 0 && !m_driver_installing);
	m_ui.next->setEnabled(!m_driver_installing);
}

void SetupWizardDialog::confirmCancel()
{
	if (QMessageBox::question(this, tr("Cancel Setup"),
			tr("Are you sure you want to cancel ARMSX2 setup?\n\nAny changes have been saved, and the wizard will run "
			   "again next time you start ARMSX2.")) != QMessageBox::Yes)
	{
		return;
	}

	reject();
}

void SetupWizardDialog::setupUi()
{
	m_ui.setupUi(this);

	m_ui.logo->setPixmap(QPixmap(QStringLiteral("%1/icons/AppIconLarge.png").arg(QtHost::GetResourcesBasePath())));

	m_ui.pages->setCurrentIndex(0);

	m_page_labels[Page_Language] = m_ui.labelLanguage;
	m_page_labels[Page_BIOS] = m_ui.labelBIOS;
	m_page_labels[Page_GameList] = m_ui.labelGameList;
	m_page_labels[Page_Controller] = m_ui.labelController;
	m_page_labels[Page_RetroAchievements] = m_ui.labelRetroAchievements;
	m_page_labels[Page_AutoTune] = m_ui.labelAutoTune;
	m_page_labels[Page_Driver] = m_ui.labelDriver;
	m_page_labels[Page_Complete] = m_ui.labelComplete;

	connect(m_ui.back, &QPushButton::clicked, this, &SetupWizardDialog::previousPage);
	connect(m_ui.next, &QPushButton::clicked, this, &SetupWizardDialog::nextPage);
	connect(m_ui.cancel, &QPushButton::clicked, this, &SetupWizardDialog::confirmCancel);

	setupLanguagePage();
	setupBIOSPage();
	setupGameListPage();
	setupControllerPage();
	setupRetroAchievementsPage();
	setupAutoTunePage();
	setupDriverPage();
	setupCompletePage();
}

void SetupWizardDialog::setupLanguagePage()
{
	SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.theme, "UI", "Theme",
		InterfaceSettingsWidget::THEME_NAMES, InterfaceSettingsWidget::THEME_VALUES, QtHost::GetDefaultThemeName(), "InterfaceSettingsWidget");
	connect(m_ui.theme, &QComboBox::currentIndexChanged, this, &SetupWizardDialog::themeChanged);

	for (const std::pair<QString, QString>& it : QtHost::GetAvailableLanguageList())
	{
		QIcon flag_icon = QtUtils::GetFlagIconForLanguage(it.second);
		if (!flag_icon.isNull())
			m_ui.language->addItem(flag_icon, it.first, it.second);
		else
			m_ui.language->addItem(it.first, it.second);
	}
	SettingWidgetBinder::BindWidgetToStringSetting(
		nullptr, m_ui.language, "UI", "Language", QtHost::GetDefaultLanguage());
	connect(
		m_ui.language, &QComboBox::currentIndexChanged, this, &SetupWizardDialog::languageChanged);

	// ARMSX2: auto-updater temporarily disabled — force the checkbox off and
	// non-interactive instead of binding it to the AutoUpdater setting.
	m_ui.autoUpdateEnabled->setChecked(false);
	m_ui.autoUpdateEnabled->setEnabled(false);
}

void SetupWizardDialog::themeChanged()
{
	// Main window gets recreated at the end here anyway, so it's fine to just yolo it.
	QtHost::UpdateApplicationTheme();
}

void SetupWizardDialog::languageChanged()
{
	// Skip the recreation, since we don't have many dynamic UI elements.
	QtHost::InstallTranslator(this);
	m_ui.retranslateUi(this);
}

void SetupWizardDialog::setupBIOSPage()
{
	SettingWidgetBinder::BindWidgetToFolderSetting(nullptr, m_ui.biosSearchDirectory, m_ui.browseBiosSearchDirectory,
		m_ui.openBiosSearchDirectory, m_ui.resetBiosSearchDirectory, "Folders", "Bios",
		Path::Combine(EmuFolders::DataRoot, "bios"));

	refreshBiosList();

	connect(m_ui.biosSearchDirectory, &QLineEdit::textChanged, this, &SetupWizardDialog::refreshBiosList);
	connect(m_ui.refreshBiosList, &QPushButton::clicked, this, &SetupWizardDialog::refreshBiosList);
	connect(m_ui.biosList, &QTreeWidget::currentItemChanged, this, &SetupWizardDialog::biosListItemChanged);
}

void SetupWizardDialog::refreshBiosList()
{
	BIOSSettingsWidget::populateList(m_ui.biosList, m_ui.biosSearchDirectory->text().toStdString());
}

void SetupWizardDialog::biosListItemChanged(const QTreeWidgetItem* current, const QTreeWidgetItem* previous)
{
	Host::SetBaseStringSettingValue("Filenames", "BIOS", current->text(0).toUtf8().constData());
	Host::CommitBaseSettingChanges();
	g_emu_thread->applySettings();
}

void SetupWizardDialog::setupGameListPage()
{
	m_ui.searchDirectoryList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_ui.searchDirectoryList->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_ui.searchDirectoryList->setAlternatingRowColors(true);
	m_ui.searchDirectoryList->setShowGrid(false);
	m_ui.searchDirectoryList->horizontalHeader()->setHighlightSections(false);
	m_ui.searchDirectoryList->verticalHeader()->hide();
	m_ui.searchDirectoryList->setCurrentIndex({});
	m_ui.searchDirectoryList->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);

	connect(m_ui.searchDirectoryList, &QTableWidget::customContextMenuRequested, this, &SetupWizardDialog::onDirectoryListContextMenuRequested);
	connect(m_ui.searchDirectoryList, &QTableWidget::itemSelectionChanged, this, &SetupWizardDialog::onDirectoryListSelectionChanged);
	connect(m_ui.addSearchDirectoryButton, &QPushButton::clicked, this, &SetupWizardDialog::onAddSearchDirectoryButtonClicked);
	connect(m_ui.removeSearchDirectoryButton, &QPushButton::clicked, this, &SetupWizardDialog::onRemoveSearchDirectoryButtonClicked);

	refreshDirectoryList();
}

void SetupWizardDialog::onDirectoryListContextMenuRequested(const QPoint& point)
{
	QModelIndexList selection = m_ui.searchDirectoryList->selectionModel()->selectedIndexes();
	if (selection.size() < 1)
		return;

	const int row = selection[0].row();

	QMenu menu;
	//: Part of the right-click menu for game directory entries
	menu.addAction(tr("Remove"), [this]() { onRemoveSearchDirectoryButtonClicked(); });
	menu.addSeparator();
	//: Part of the right-click menu for game directory entries
	menu.addAction(tr("Open Directory..."),
		[this, row]() { QtUtils::OpenURL(this, QUrl::fromLocalFile(m_ui.searchDirectoryList->item(row, 0)->text())); });
	menu.exec(m_ui.searchDirectoryList->mapToGlobal(point));
}

void SetupWizardDialog::onDirectoryListSelectionChanged()
{
	m_ui.removeSearchDirectoryButton->setEnabled(!m_ui.searchDirectoryList->selectedItems().isEmpty());
}

void SetupWizardDialog::onAddSearchDirectoryButtonClicked()
{
	QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select Search Directory")));

	if (dir.isEmpty())
		return;

	QMessageBox::StandardButton selection = QMessageBox::question(this, tr("Scan Recursively?"),
		tr("Would you like to scan the directory \"%1\" recursively?\n\nScanning recursively takes "
		   "more time, but will identify files in subdirectories.")
			.arg(dir),
		QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
	if (selection == QMessageBox::Cancel)
		return;

	const bool recursive = (selection == QMessageBox::Yes);
	const std::string spath = dir.toStdString();
	Host::RemoveBaseValueFromStringList("GameList", recursive ? "Paths" : "RecursivePaths", spath.c_str());
	Host::AddBaseValueToStringList("GameList", recursive ? "RecursivePaths" : "Paths", spath.c_str());
	Host::CommitBaseSettingChanges();
	refreshDirectoryList();
}

void SetupWizardDialog::onRemoveSearchDirectoryButtonClicked()
{
	const int row = m_ui.searchDirectoryList->currentRow();
	std::unique_ptr<QTableWidgetItem> item((row >= 0) ? m_ui.searchDirectoryList->takeItem(row, 0) : nullptr);
	if (!item)
		return;

	const std::string spath = item->text().toStdString();
	if (!Host::RemoveBaseValueFromStringList("GameList", "Paths", spath.c_str()) &&
		!Host::RemoveBaseValueFromStringList("GameList", "RecursivePaths", spath.c_str()))
	{
		return;
	}

	Host::CommitBaseSettingChanges();
	refreshDirectoryList();
}

void SetupWizardDialog::addPathToTable(const std::string& path, bool recursive)
{
	const int row = m_ui.searchDirectoryList->rowCount();
	m_ui.searchDirectoryList->insertRow(row);

	QTableWidgetItem* item = new QTableWidgetItem();
	item->setText(QString::fromStdString(path));
	item->setFlags(item->flags() & ~(Qt::ItemIsEditable));
	m_ui.searchDirectoryList->setItem(row, 0, item);

	QCheckBox* cb = new QCheckBox(m_ui.searchDirectoryList);
	m_ui.searchDirectoryList->setCellWidget(row, 1, cb);
	cb->setChecked(recursive);

	connect(cb, &QCheckBox::checkStateChanged, this, [item](Qt::CheckState state) {
		const std::string path(item->text().toStdString());
		if (state == Qt::Checked)
		{
			Host::RemoveBaseValueFromStringList("GameList", "Paths", path.c_str());
			Host::AddBaseValueToStringList("GameList", "RecursivePaths", path.c_str());
		}
		else
		{
			Host::RemoveBaseValueFromStringList("GameList", "RecursivePaths", path.c_str());
			Host::AddBaseValueToStringList("GameList", "Paths", path.c_str());
		}
		Host::CommitBaseSettingChanges();
	});
}

void SetupWizardDialog::refreshDirectoryList()
{
	QSignalBlocker sb(m_ui.searchDirectoryList);
	while (m_ui.searchDirectoryList->rowCount() > 0)
		m_ui.searchDirectoryList->removeRow(0);

	std::vector<std::string> path_list = Host::GetBaseStringListSetting("GameList", "Paths");
	for (const std::string& entry : path_list)
		addPathToTable(entry, false);

	path_list = Host::GetBaseStringListSetting("GameList", "RecursivePaths");
	for (const std::string& entry : path_list)
		addPathToTable(entry, true);

	m_ui.searchDirectoryList->sortByColumn(0, Qt::AscendingOrder);
	m_ui.removeSearchDirectoryButton->setEnabled(false);
}

void SetupWizardDialog::resizeDirectoryListColumns()
{
	QtUtils::ResizeColumnsForTableView(m_ui.searchDirectoryList, {-1, 100});
}

void SetupWizardDialog::setupControllerPage()
{
	static constexpr u32 NUM_PADS = 2;

	struct PadWidgets
	{
		QComboBox* type_combo;
		QLabel* mapping_result;
		QToolButton* mapping_button;
	};
	const PadWidgets pad_widgets[NUM_PADS] = {
		{m_ui.controller1Type, m_ui.controller1Mapping, m_ui.controller1AutomaticMapping},
		{m_ui.controller2Type, m_ui.controller2Mapping, m_ui.controller2AutomaticMapping},
	};

	for (u32 port = 0; port < NUM_PADS; port++)
	{
		const std::string section = fmt::format("Pad{}", port + 1);
		const PadWidgets& w = pad_widgets[port];

		for (const auto& [name, display_name] : Pad::GetControllerTypeNames())
			w.type_combo->addItem(QString::fromUtf8(display_name), QString::fromUtf8(name));
		ControllerSettingWidgetBinder::BindWidgetToInputProfileString(
			nullptr, w.type_combo, section, "Type", Pad::GetControllerInfo(Pad::GetDefaultPadType(port))->name);

		w.mapping_result->setText((port == 0) ? tr("Default (Keyboard)") : tr("Default (None)"));

		connect(w.mapping_button, &QAbstractButton::clicked, this,
			[this, port, label = w.mapping_result]() { openAutomaticMappingMenu(port, label); });
	}

	// Trigger enumeration to populate the device list.
	connect(g_emu_thread, &EmuThread::onInputDevicesEnumerated, this, &SetupWizardDialog::onInputDevicesEnumerated);
	connect(g_emu_thread, &EmuThread::onInputDeviceConnected, this, &SetupWizardDialog::onInputDeviceConnected);
	connect(g_emu_thread, &EmuThread::onInputDeviceDisconnected, this, &SetupWizardDialog::onInputDeviceDisconnected);
	g_emu_thread->enumerateInputDevices();
}

void SetupWizardDialog::setupRetroAchievementsPage()
{
	const QString base_path(QtHost::GetResourcesBasePath());
	QtUtils::SetScalableIcon(m_ui.raLogo, QIcon(QStringLiteral("%1/icons/ra-icon.svg").arg(base_path)), QSize(56, 56));

	if (Achievements::IsUsingRAIntegration())
	{
		m_ui.raIntegrationLabel->setVisible(true);
		m_ui.raContentWidget->setVisible(false);
		return;
	}

	SettingWidgetBinder::BindWidgetToBoolSetting(
		nullptr, m_ui.raEnableAchievements, "Achievements", "Enabled", false);
	SettingWidgetBinder::BindWidgetToBoolSetting(
		nullptr, m_ui.raHardcoreMode, "Achievements", "ChallengeMode", false);
	connect(m_ui.raLoginButton, &QPushButton::clicked, this, &SetupWizardDialog::onRetroAchievementsLoginLogoutPressed);
	connect(m_ui.raViewProfileButton, &QPushButton::clicked, this, &SetupWizardDialog::onRetroAchievementsViewProfilePressed);
	refreshRetroAchievementsLoginState();
}

void SetupWizardDialog::setupAutoTunePage()
{
	// Checked unless the player already turned it off. A change is saved at once; leaving the page saves the answer
	// even when it was left as it was (see nextPage), so the settings file shows the player was asked.
	SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.autoTuneEnabled, "EmuCore", "AutoTune", true);
}

void SetupWizardDialog::setupDriverPage()
{
	// The page exists only where a pack can help: an Adreno on Linux whose player has not chosen or installed one
	// and whose launcher does not set the driver itself. The page is skipped otherwise, and its step is hidden.
	m_driver_manager = std::make_shared<DriverPacks::Manager>(Catalog::GetDefaultFetcher(), DriverPacks::ManagerConfig::ForUser());
	const QString platform = QGuiApplication::platformName();
	const bool session_is_x11 = platform == QLatin1String("xcb") || platform == QLatin1String("x11");
	m_driver_page_enabled = DriverPacks::ShouldOfferInSetup(DriverPacks::GetSetupContext(*m_driver_manager, session_is_x11));
	m_ui.labelDriver->setVisible(m_driver_page_enabled);

	connect(m_ui.driverInstall, &QPushButton::clicked, this, &SetupWizardDialog::onDriverInstallClicked);
}

void SetupWizardDialog::loadDriverOffer()
{
	if (m_driver_listing_started)
		return;
	m_driver_listing_started = true;
	m_driver_offer.reset();
	m_ui.driverInstall->setEnabled(false);
	m_ui.driverInstall->setText(tr("Download and Install"));
	m_ui.driverStatus->clear();
	m_ui.driverOffer->setText(tr("Looking up the driver..."));

	QPointer<SetupWizardDialog> self(this);
	const std::shared_ptr<DriverPacks::Manager> manager = m_driver_manager;
	const Catalog::CancelToken cancel = m_driver_cancel;
	QThreadPool::globalInstance()->start([self, manager, cancel]() {
		DriverPacks::RemoteListing listing = manager->ListAvailable(cancel);
		if (cancel.IsCancelled())
			return;

		QMetaObject::invokeMethod(QCoreApplication::instance(), [self, listing = std::move(listing)]() {
			if (self)
				self->showDriverOffer(listing); }, Qt::QueuedConnection);
	});
}

void SetupWizardDialog::showDriverOffer(const DriverPacks::RemoteListing& listing)
{
	if (const DriverPacks::RemoteDriver* pick = DriverPacks::PickRecommended(listing.drivers))
	{
		m_driver_offer = *pick;
		QStringList parts;
		parts << QString::fromStdString(pick->tag);
		if (pick->size)
			parts << driverSizeText(pick->size);
		m_ui.driverOffer->setText(QStringLiteral("<b>%1</b><br>%2")
									  .arg(QString::fromStdString(pick->release_name.empty() ? pick->tag : pick->release_name).toHtmlEscaped(),
										  parts.join(QStringLiteral(" · ")).toHtmlEscaped()));
		m_ui.driverInstall->setText(tr("Download and Install"));
	}
	else
	{
		const QString reason = listing.errors.empty() ? tr("No GPU driver is available for this device right now.") :
		                                                tr("Could not read the driver list: %1").arg(QString::fromStdString(listing.errors.front().message));
		m_ui.driverOffer->setText(reason);
		m_ui.driverInstall->setText(tr("Try Again"));
	}
	m_ui.driverInstall->setEnabled(true);
}

void SetupWizardDialog::onDriverInstallClicked()
{
	if (m_driver_installing)
	{
		m_driver_cancel.Cancel();
		m_ui.driverInstall->setEnabled(false);
		m_ui.driverStatus->setText(tr("Cancelling..."));
		return;
	}

	if (!m_driver_offer)
	{
		// The list could not be read: ask again.
		m_driver_listing_started = false;
		loadDriverOffer();
		return;
	}

	m_driver_installing = true;
	m_driver_cancel = Catalog::CancelToken();
	m_ui.driverInstall->setText(tr("Cancel"));
	m_ui.driverProgress->setRange(0, 0);
	m_ui.driverProgress->setVisible(true);
	m_ui.driverStatus->setText(tr("Preparing..."));
	updatePageButtons();

	QPointer<SetupWizardDialog> self(this);
	const std::shared_ptr<DriverPacks::Manager> manager = m_driver_manager;
	const Catalog::CancelToken cancel = m_driver_cancel;
	QThreadPool::globalInstance()->start([self, manager, cancel, driver = *m_driver_offer]() {
		const DriverPacks::InstallResult result = manager->Install(driver, [self](const DriverPacks::InstallProgress& progress) { QMetaObject::invokeMethod(QCoreApplication::instance(), [self, progress]() {
				if (self)
					self->showDriverProgress(progress); }, Qt::QueuedConnection); }, cancel);

		QMetaObject::invokeMethod(QCoreApplication::instance(), [self, result]() {
			if (self)
				self->finishDriverInstall(result); }, Qt::QueuedConnection);
	});
}

void SetupWizardDialog::showDriverProgress(const DriverPacks::InstallProgress& progress)
{
	if (!m_driver_installing)
		return;

	QString text;
	bool determinate = false;
	switch (progress.stage)
	{
		case DriverPacks::InstallStage::Preparing:
			text = tr("Preparing...");
			break;
		case DriverPacks::InstallStage::Downloading:
			text = progress.total ? tr("Downloading %1 of %2").arg(driverSizeText(progress.done), driverSizeText(progress.total)) : tr("Downloading %1").arg(driverSizeText(progress.done));
			determinate = progress.total > 0;
			break;
		case DriverPacks::InstallStage::Verifying:
			text = tr("Verifying download...");
			break;
		case DriverPacks::InstallStage::Extracting:
			text = tr("Extracting...");
			break;
		case DriverPacks::InstallStage::Checking:
			text = tr("Checking that the driver loads...");
			break;
		case DriverPacks::InstallStage::Finishing:
			text = tr("Finishing...");
			break;
	}
	m_ui.driverStatus->setText(text);
	if (determinate)
	{
		m_ui.driverProgress->setRange(0, 1000);
		m_ui.driverProgress->setValue(static_cast<int>(progress.done * 1000 / progress.total));
	}
	else
	{
		m_ui.driverProgress->setRange(0, 0);
	}

	// The last steps cannot be cancelled usefully.
	m_ui.driverInstall->setEnabled(progress.stage != DriverPacks::InstallStage::Finishing && !m_driver_cancel.IsCancelled());
}

void SetupWizardDialog::finishDriverInstall(const DriverPacks::InstallResult& result)
{
	m_driver_installing = false;
	m_ui.driverProgress->setVisible(false);

	if (result.Ok())
	{
		// Selected now; it loads when ARMSX2 starts again, which the caller does after the wizard.
		DriverPacks::SetSelectedPack(result.id);
		m_driver_installed = true;
		m_ui.driverStatus->setText(tr("Installed. It loads when ARMSX2 restarts, which happens when you finish setup."));
		m_ui.driverInstall->setText(tr("Installed"));
		m_ui.driverInstall->setEnabled(false);
		m_ui.driverRestartNote->setVisible(true);
	}
	else
	{
		m_ui.driverStatus->setText(result.Cancelled() ? tr("Cancelled.") : tr("Could not install the driver: %1").arg(QString::fromStdString(result.message)));
		m_ui.driverInstall->setText(tr("Download and Install"));
		m_ui.driverInstall->setEnabled(true);
	}
	updatePageButtons();
}

void SetupWizardDialog::setupCompletePage()
{
#if defined(_WIN32)
	const bool can_create_shortcuts = true;
	m_ui.addToApplicationMenu->setText(tr("Add ARMSX2 to the Start Menu"));
#elif defined(__linux__)
	// Only offer shortcuts for the AppImage since Flatpak creates its own launcher, and
	// third-party builds (AUR, COPR, etc.) ship their own .desktop file.
	const bool can_create_shortcuts = QtUtils::IsRunningInAppImage();
#else
	const bool can_create_shortcuts = false;
#endif
	m_ui.createDesktopShortcut->setVisible(can_create_shortcuts);
	m_ui.addToApplicationMenu->setVisible(can_create_shortcuts);
}

void SetupWizardDialog::refreshRetroAchievementsLoginState()
{
	const std::string username(Host::GetBaseStringSettingValue("Achievements", "Username"));
	const bool logged_in = !username.empty();

	if (logged_in)
	{
		const u64 login_unix_timestamp =
			StringUtil::FromChars<u64>(Host::GetBaseStringSettingValue("Achievements", "LoginTimestamp", "0")).value_or(0);
		const QDateTime login_timestamp(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(login_unix_timestamp)));
		m_ui.raLoginStatus->setText(tr("Username: %1\nLogin token generated on %2.")
				.arg(QString::fromStdString(username))
				.arg(login_timestamp.toString(Qt::TextDate)));
		m_ui.raLoginButton->setText(tr("Logout"));
	}
	else
	{
		m_ui.raLoginStatus->setText(tr("Not Logged In."));
		m_ui.raLoginButton->setText(tr("Login..."));
	}

	m_ui.raViewProfileButton->setEnabled(logged_in);
}

void SetupWizardDialog::onRetroAchievementsLoginLogoutPressed()
{
	if (!Host::GetBaseStringSettingValue("Achievements", "Username").empty())
	{
		Host::RunOnCPUThread([]() { Achievements::Logout(); }, true);
		refreshRetroAchievementsLoginState();
		return;
	}

	AchievementLoginDialog login(this, Achievements::LoginRequestReason::UserInitiated);
	if (login.exec() != 0)
		return;

	refreshRetroAchievementsLoginState();

	// Login can enable achievements, keep the checkbox state in sync.
	if (!m_ui.raEnableAchievements->isChecked() &&
		Host::GetBaseBoolSettingValue("Achievements", "Enabled", false))
	{
		QSignalBlocker sb(m_ui.raEnableAchievements);
		m_ui.raEnableAchievements->setChecked(true);
	}
	if (!m_ui.raHardcoreMode->isChecked() &&
		Host::GetBaseBoolSettingValue("Achievements", "ChallengeMode", false))
	{
		QSignalBlocker sb(m_ui.raHardcoreMode);
		m_ui.raHardcoreMode->setChecked(true);
	}
}

void SetupWizardDialog::onRetroAchievementsViewProfilePressed()
{
	const std::string username(Host::GetBaseStringSettingValue("Achievements", "Username"));
	if (username.empty())
		return;

	const QByteArray encoded_username(QUrl::toPercentEncoding(QString::fromStdString(username)));
	QtUtils::OpenURL(this, QUrl(QStringLiteral("https://retroachievements.org/user/%1").arg(QString::fromUtf8(encoded_username))));
}

void SetupWizardDialog::openAutomaticMappingMenu(u32 port, QLabel* update_label)
{
	QMenu menu(this);
	bool added = false;

	for (const QPair<QString, QString>& dev : m_device_list)
	{
		// we set it as data, because the device list could get invalidated while the menu is up
		QAction* action = menu.addAction(QStringLiteral("%1 (%2)").arg(dev.first).arg(dev.second));
		action->setData(dev.first);
		connect(action, &QAction::triggered, this, [this, port, update_label, action]() {
			doDeviceAutomaticBinding(port, update_label, action->data().toString());
		});
		added = true;
	}

	if (!added)
	{
		QAction* action = menu.addAction(tr("No devices available"));
		action->setEnabled(false);
	}

	menu.exec(QCursor::pos());
}

void SetupWizardDialog::doDeviceAutomaticBinding(u32 port, QLabel* update_label, const QString& device)
{
	std::vector<std::pair<GenericInputBinding, std::string>> mapping =
		InputManager::GetGenericBindingMapping(device.toStdString());
	if (mapping.empty())
	{
		QMessageBox::critical(this, tr("Automatic Binding"),
			tr("No generic bindings were generated for device '%1'. The controller/source may not support automatic "
			   "mapping.")
				.arg(device));
		return;
	}

	bool result;
	{
		auto lock = Host::GetSettingsLock();
		result = Pad::MapController(*Host::Internal::GetBaseSettingsLayer(), port, mapping);
	}
	if (!result)
		return;

	Host::CommitBaseSettingChanges();

	update_label->setText(device);
}

void SetupWizardDialog::onInputDevicesEnumerated(const QList<QPair<QString, QString>>& devices)
{
	m_device_list = devices;
}

void SetupWizardDialog::onInputDeviceConnected(const QString& identifier, const QString& device_name)
{
	m_device_list.emplace_back(identifier, device_name);
}

void SetupWizardDialog::onInputDeviceDisconnected(const QString& identifier)
{
	for (auto iter = m_device_list.begin(); iter != m_device_list.end(); ++iter)
	{
		if (iter->first == identifier)
		{
			m_device_list.erase(iter);
			break;
		}
	}
}

#include "moc_SetupWizardDialog.cpp"
