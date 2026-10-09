// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "DriverPacksSettingsWidget.h"
#include "MainWindow.h"
#include "QtHost.h"
#include "SettingsWindow.h"

#include "Catalog/Fetch.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QThreadPool>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTreeWidget>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace
{
	enum InstalledColumn
	{
		InstalledName,
		InstalledVersion,
		InstalledMesa,
		InstalledWindowSystem,
	};

	enum AvailableColumn
	{
		AvailableRelease,
		AvailableTag,
		AvailableSize,
		AvailablePublished,
		AvailableStatus,
	};

	constexpr int ID_ROLE = Qt::UserRole;

	/// "3.9 MB", "16 MB": a driver pack is a few megabytes, where the catalog's whole-number MB would
	/// say 3 for 3.9.
	QString sizeText(u64 bytes)
	{
		const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
		if (mib < 1.0)
			return QCoreApplication::translate("DriverPacksSettingsWidget", "%1 KB").arg(bytes / 1024);
		return QCoreApplication::translate("DriverPacksSettingsWidget", "%1 MB").arg(mib, 0, 'f', mib < 10.0 ? 1 : 0);
	}

	/// "2026-10-04" from GitHub's "2026-10-04T23:50:10Z".
	QString dateText(const std::string& iso)
	{
		return QString::fromStdString(iso.substr(0, 10));
	}
} // namespace

DriverPacksSettingsWidget::DriverPacksSettingsWidget(SettingsWindow* dialog, QWidget* parent)
	: DriverPacksSettingsWidget(dialog, parent, DriverPacks::ManagerConfig::ForUser(), QGuiApplication::platformName())
{
}

DriverPacksSettingsWidget::DriverPacksSettingsWidget(SettingsWindow* dialog, QWidget* parent, DriverPacks::ManagerConfig config, QString platform_name)
	: SettingsWidget(dialog, parent)
	, m_manager(std::make_shared<DriverPacks::Manager>(Catalog::GetDefaultFetcher(), std::move(config)))
	, m_platform_name(std::move(platform_name))
{
	init();
}

bool DriverPacksSettingsWidget::shouldOffer()
{
	if (DriverPacks::GetAvailability().IsAvailable() || !DriverPacks::GetSelectedPack().empty())
		return true;
	return !DriverPacks::Manager(Catalog::GetDefaultFetcher(), DriverPacks::ManagerConfig::ForUser()).ListInstalled().empty();
}

DriverPacksSettingsWidget::~DriverPacksSettingsWidget()
{
	// A download or a listing still running finds its way back through a QPointer, which is null by
	// then; telling it to stop saves the bytes.
	m_list_cancel.Cancel();
	m_install_cancel.Cancel();
}

void DriverPacksSettingsWidget::init()
{
	setupTab(m_ui);

	for (QLabel* label : {m_ui.availabilityLabel, m_ui.currentDriverLabel, m_ui.currentDriverNote, m_ui.x11Warning, m_ui.chooseNote, m_ui.availableStatus, m_ui.installText})
		label->setTextFormat(Qt::PlainText);

	m_ui.installedList->setColumnWidth(InstalledName, 260);
	m_ui.installedList->setColumnWidth(InstalledVersion, 110);
	m_ui.installedList->setColumnWidth(InstalledMesa, 130);
	m_ui.availableList->setColumnWidth(AvailableRelease, 260);
	m_ui.availableList->setColumnWidth(AvailableTag, 110);
	m_ui.availableList->setColumnWidth(AvailableSize, 70);
	m_ui.availableList->setColumnWidth(AvailablePublished, 90);
	m_ui.x11Warning->setStyleSheet(QStringLiteral("QLabel { background: #fff3cd; color: #664d03; border: 1px solid #ffda6a; border-radius: 4px; padding: 6px; }"));
	m_ui.chooseNote->setText(tr("A driver takes effect the next time ARMSX2 starts. Apply & Restart saves your choice, closes ARMSX2 and starts it again. "
								"Driver packs are used for ARMSX2 only; nothing is installed into the system."));

	const DriverPacks::Availability availability = m_manager->GetAvailability();
	m_available = availability.IsAvailable();
	// Without an Adreno the page is listed only for a pack that is installed or selected, to undo the choice or delete the pack.
	m_manageable = m_available || !DriverPacks::GetSelectedPack().empty() || !m_manager->ListInstalled().empty();
	if (!m_available)
	{
		m_ui.contentWidget->setVisible(m_manageable);
		m_ui.availabilityLabel->setVisible(true);
		m_ui.availabilityLabel->setText(
			availability.reason == DriverPacks::Availability::Reason::UnsupportedPlatform ?
				tr("Driver packs are not available on this platform.") :
				tr("No driver packs for this GPU. Driver packs are Turnip builds of the Vulkan driver for Qualcomm Adreno GPUs, and this device has none."));
		m_ui.availableGroup->setVisible(false);
		if (!m_manageable)
			return;
	}

	connect(m_ui.driverCombo, &QComboBox::currentIndexChanged, this, &DriverPacksSettingsWidget::onChoiceChanged);
	connect(m_ui.applyButton, &QPushButton::clicked, this, &DriverPacksSettingsWidget::onApplyClicked);
	connect(m_ui.deleteButton, &QPushButton::clicked, this, &DriverPacksSettingsWidget::onDeleteClicked);
	connect(m_ui.downloadButton, &QPushButton::clicked, this, &DriverPacksSettingsWidget::onDownloadClicked);
	connect(m_ui.refreshAvailableButton, &QPushButton::clicked, this, &DriverPacksSettingsWidget::onRefreshAvailableClicked);
	connect(m_ui.installCancel, &QPushButton::clicked, this, &DriverPacksSettingsWidget::onInstallStripButtonClicked);
	connect(m_ui.availableList, &QTreeWidget::itemSelectionChanged, this, &DriverPacksSettingsWidget::onAvailableSelectionChanged);
	connect(m_ui.installedList, &QTreeWidget::itemSelectionChanged, this, &DriverPacksSettingsWidget::onInstalledSelectionChanged);

	dialog()->registerWidgetHelp(m_ui.driverCombo, tr("Driver"), tr("System driver"),
		tr("The Vulkan driver ARMSX2 uses. A driver pack replaces the system's driver for ARMSX2 only and takes effect when ARMSX2 starts."));
	dialog()->registerWidgetHelp(m_ui.applyButton, tr("Apply &amp; Restart"), tr("N/A"),
		tr("Saves the choice, closes ARMSX2 (stopping a running game) and starts it again with the chosen driver."));
	dialog()->registerWidgetHelp(m_ui.availableList, tr("Available Drivers"), tr("N/A"),
		tr("Driver packs published for this device. Select one and press Download. The download is checked against its published checksum, and the driver is test-loaded before it is kept."));

	reloadInstalled();
	refreshCurrent();
	showAvailable();
}

void DriverPacksSettingsWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);
	if (!m_manageable)
		return;

	reloadInstalled();
	if (m_first_show)
	{
		m_first_show = false;
		if (m_available)
			loadAvailable(false);
	}
}

// ---- What is in use, and what is installed ----------------------------------------------------------------

bool DriverPacksSettingsWidget::sessionIsX11() const
{
	return m_platform_name == QLatin1String("xcb") || m_platform_name == QLatin1String("x11");
}

bool DriverPacksSettingsWidget::isWaylandOnly(const DriverPacks::InstalledDriver& driver) const
{
	const QString platforms = QString::fromStdString(driver.platforms).toLower();
	return platforms.contains(QLatin1String("wayland")) && !platforms.contains(QLatin1String("x11")) && !platforms.contains(QLatin1String("xcb"));
}

const DriverPacks::InstalledDriver* DriverPacksSettingsWidget::findInstalled(const std::string& id) const
{
	for (const DriverPacks::InstalledDriver& driver : m_installed)
	{
		if (driver.id == id)
			return &driver;
	}
	return nullptr;
}

QString DriverPacksSettingsWidget::driverTitle(const DriverPacks::InstalledDriver& driver) const
{
	return QString::fromStdString(driver.name.empty() ? driver.id : driver.name);
}

std::string DriverPacksSettingsWidget::appliedId() const
{
	const DriverPacks::ApplyResult applied = DriverPacks::GetLastApplyResult();
	return applied.outcome == DriverPacks::ApplyOutcome::Applied ? applied.pack_id : std::string();
}

bool DriverPacksSettingsWidget::environmentOverrides() const
{
	return DriverPacks::GetLastApplyResult().outcome == DriverPacks::ApplyOutcome::EnvironmentAlreadySet;
}

std::string DriverPacksSettingsWidget::chosenId() const
{
	return m_ui.driverCombo->currentData().toString().toStdString();
}

void DriverPacksSettingsWidget::reloadInstalled()
{
	m_installed = m_manager->ListInstalled();
	refreshInstalledList();
	refreshChoice();
	refreshCurrent();
	showAvailable();
}

void DriverPacksSettingsWidget::refreshCurrent()
{
	const DriverPacks::ApplyResult applied = DriverPacks::GetLastApplyResult();

	QString text;
	QString note;
	switch (applied.outcome)
	{
		case DriverPacks::ApplyOutcome::Applied:
		{
			const DriverPacks::InstalledDriver* driver = findInstalled(applied.pack_id);
			text = tr("Using the driver pack %1.").arg(driver ? driverTitle(*driver) : QString::fromStdString(applied.pack_id));
			break;
		}

		case DriverPacks::ApplyOutcome::EnvironmentAlreadySet:
			text = tr("Using the Vulkan driver your launcher or environment chose.");
			note = tr("VK_DRIVER_FILES or VK_ICD_FILENAMES was already set when ARMSX2 started (the ROCKNIX launcher does this), so a pack chosen here "
					  "is not used. Change the driver in the launcher, or start ARMSX2 without those variables.");
			break;

		case DriverPacks::ApplyOutcome::PackMissing:
			text = tr("Using the system Vulkan driver.");
			note = tr("The driver pack %1 is selected but it is not installed, so the system driver is used. Download it again, or pick another driver below.")
			           .arg(QString::fromStdString(applied.pack_id));
			break;

		case DriverPacks::ApplyOutcome::GpuNotSupported:
			text = tr("Using the system Vulkan driver.");
			note = tr("The driver pack %1 is selected, but this device has no Adreno GPU, so it is not used.").arg(QString::fromStdString(applied.pack_id));
			break;

		case DriverPacks::ApplyOutcome::SystemDriver:
		case DriverPacks::ApplyOutcome::Unsupported:
			text = tr("Using the system Vulkan driver.");
			break;
	}

	// A choice made and stored, but not in use yet.
	const std::string stored = DriverPacks::GetSelectedPack();
	if (note.isEmpty() && !environmentOverrides() && stored != appliedId())
	{
		const DriverPacks::InstalledDriver* driver = findInstalled(stored);
		note = stored.empty() ? tr("The system driver is selected and will be used after ARMSX2 restarts.") :
		                        tr("%1 is selected and will be used after ARMSX2 restarts.").arg(driver ? driverTitle(*driver) : QString::fromStdString(stored));
	}

	m_ui.currentDriverLabel->setText(text);
	m_ui.currentDriverNote->setText(note);
	m_ui.currentDriverNote->setVisible(!note.isEmpty());
}

void DriverPacksSettingsWidget::refreshChoice()
{
	// The choice on screen, else the stored one.
	std::string wanted = m_ui.driverCombo->count() ? chosenId() : DriverPacks::GetSelectedPack();
	if (!wanted.empty() && !findInstalled(wanted))
		wanted.clear();

	const QSignalBlocker block(m_ui.driverCombo);
	m_ui.driverCombo->clear();
	m_ui.driverCombo->addItem(tr("System driver"), QString());
	for (const DriverPacks::InstalledDriver& driver : m_installed)
	{
		QString title = driverTitle(driver);
		if (!driver.version.empty() && !title.contains(QString::fromStdString(driver.version)))
			title += QStringLiteral(" (%1)").arg(QString::fromStdString(driver.version));
		m_ui.driverCombo->addItem(title, QString::fromStdString(driver.id));
	}
	const int index = m_ui.driverCombo->findData(QString::fromStdString(wanted));
	m_ui.driverCombo->setCurrentIndex(index >= 0 ? index : 0);

	// A launcher that sets the Vulkan driver decides, whatever is chosen here.
	m_ui.driverCombo->setEnabled(!environmentOverrides());
	onChoiceChanged();
}

void DriverPacksSettingsWidget::onChoiceChanged()
{
	const std::string id = chosenId();
	// Also when the stored choice differs from what is shown (a pack that is selected but not installed shows as
	// the system driver), so that choice can be cleared.
	m_ui.applyButton->setEnabled(!environmentOverrides() && (id != appliedId() || id != DriverPacks::GetSelectedPack()));

	// A pack built for Wayland only cannot show a picture on an X11 session.
	const DriverPacks::InstalledDriver* driver = id.empty() ? nullptr : findInstalled(id);
	const bool warn = driver && isWaylandOnly(*driver) && sessionIsX11();
	m_ui.x11Warning->setVisible(warn);
	if (warn)
	{
		m_ui.x11Warning->setText(tr("This driver pack is built for Wayland only, and this session uses X11, so it cannot show a picture here. "
									"Choose another driver, or log in to a Wayland session."));
	}
}

void DriverPacksSettingsWidget::refreshInstalledList()
{
	const std::string applied = appliedId();

	const QSignalBlocker block(m_ui.installedList);
	m_ui.installedList->clear();
	for (const DriverPacks::InstalledDriver& driver : m_installed)
	{
		QTreeWidgetItem* item = new QTreeWidgetItem(m_ui.installedList);
		item->setData(InstalledName, ID_ROLE, QString::fromStdString(driver.id));

		QString name = driverTitle(driver);
		if (driver.id == applied)
			name += tr("  (in use)");
		item->setText(InstalledName, name);
		item->setText(InstalledVersion, QString::fromStdString(driver.version.empty() ? driver.build_tag : driver.version));
		item->setText(InstalledMesa, QString::fromStdString(driver.mesa_version));
		item->setText(InstalledWindowSystem, QString::fromStdString(driver.platforms));

		if (isWaylandOnly(driver) && sessionIsX11())
		{
			item->setIcon(InstalledWindowSystem, QIcon::fromTheme(QStringLiteral("warning-line")));
			item->setToolTip(InstalledWindowSystem, tr("Built for Wayland only; this session uses X11."));
		}
	}
	m_ui.installedList->setVisible(!m_installed.empty());
	m_ui.installedEmpty->setVisible(m_installed.empty());
	if (m_ui.installedList->topLevelItemCount() > 0)
		m_ui.installedList->setCurrentItem(m_ui.installedList->topLevelItem(0));
	onInstalledSelectionChanged();
}

void DriverPacksSettingsWidget::onInstalledSelectionChanged()
{
	m_ui.deleteButton->setEnabled(!m_ui.installedList->selectedItems().isEmpty() && !m_installing);
}

// ---- Apply & Restart, delete --------------------------------------------------------------------------------

void DriverPacksSettingsWidget::onApplyClicked()
{
	const std::string id = chosenId();
	const DriverPacks::InstalledDriver* driver = id.empty() ? nullptr : findInstalled(id);
	const QString name = driver ? driverTitle(*driver) : tr("the system driver");

	QString message = tr("ARMSX2 will close and start again to use %1.").arg(name);
	if (QtHost::IsVMValid())
		message += tr("\n\nThe running game will be stopped first.");
	if (m_installing)
		message += tr("\n\nA driver is still being installed. Restarting cancels it.");
	message += tr("\n\nContinue?");
	if (QMessageBox::question(this, tr("Apply and Restart"), message) != QMessageBox::Yes)
		return;

	// Stop the download now, instead of leaving it running while ARMSX2 shuts down.
	if (m_installing)
		m_install_cancel.Cancel();

	DriverPacks::SetSelectedPack(id);

	// Work out how to start again now, while the window is still open to report a failure.
	DriverPacks::RelaunchPlan plan;
	std::string error;
	if (!DriverPacks::BuildRelaunchPlan(&plan, &error))
	{
		QMessageBox::warning(this, tr("Apply and Restart"),
			tr("The choice is saved, but ARMSX2 cannot restart itself (%1). Close it and start it again.").arg(QString::fromStdString(error)));
		refreshCurrent();
		onChoiceChanged();
		return;
	}

	// The restart is armed at the moment Qt really leaves its event loop: a game that refuses to stop
	// (the memory card is still saving) cancels the quit, and then nothing may start a second copy
	// whenever ARMSX2 is closed later.
	auto connection = std::make_shared<QMetaObject::Connection>();
	*connection = connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, QCoreApplication::instance(), [plan, connection]() {
		std::string relaunch_error;
		if (!DriverPacks::RelaunchAtExit(plan, &relaunch_error))
			fprintf(stderr, "DriverPacks: could not arm the restart: %s\n", relaunch_error.c_str());
		QObject::disconnect(*connection);
	});

	// Quit the usual way: stops the VM, saves the settings and joins the threads.
	const bool had_vm = QtHost::IsVMValid();
	g_main_window->requestExit(false);
	if (had_vm && QtHost::IsVMValid())
	{
		// The shutdown was declined.
		QObject::disconnect(*connection);
		refreshCurrent();
	}
}

void DriverPacksSettingsWidget::onDeleteClicked()
{
	const QList<QTreeWidgetItem*> selected = m_ui.installedList->selectedItems();
	if (selected.isEmpty())
		return;

	const std::string id = selected.front()->data(InstalledName, ID_ROLE).toString().toStdString();
	const DriverPacks::InstalledDriver* driver = findInstalled(id);
	if (!driver)
		return;

	QString message = tr("Delete the driver pack \"%1\"?\n\nIts files are removed from the drivers folder.").arg(driverTitle(*driver));
	if (id == appliedId())
		message += tr("\n\nARMSX2 is using it now and keeps working until it is closed; after that it uses the system driver.");
	if (QMessageBox::question(this, tr("Delete Driver Pack"), message) != QMessageBox::Yes)
		return;

	std::string error;
	if (!m_manager->Delete(id, &error))
	{
		QMessageBox::warning(this, tr("Delete Driver Pack"), tr("Could not delete the driver pack: %1").arg(QString::fromStdString(error)));
		reloadInstalled();
		return;
	}

	// Deleting a pack does not clear the selection that names it.
	if (DriverPacks::GetSelectedPack() == id)
		DriverPacks::SetSelectedPack(std::string());

	reloadInstalled();
}

// ---- Available packs and downloads -----------------------------------------------------------------------------

void DriverPacksSettingsWidget::loadAvailable(bool force_refresh)
{
	if (m_listing_busy)
		return;
	m_listing_busy = true;
	showAvailable();

	m_list_cancel = Catalog::CancelToken();
	QPointer<DriverPacksSettingsWidget> self(this);
	const std::shared_ptr<DriverPacks::Manager> manager = m_manager;
	const Catalog::CancelToken cancel = m_list_cancel;

	QThreadPool::globalInstance()->start([self, manager, cancel, force_refresh]() {
		DriverPacks::RemoteListing listing = manager->ListAvailable(cancel, force_refresh);
		if (cancel.IsCancelled())
			return;

		QMetaObject::invokeMethod(QCoreApplication::instance(), [self, listing = std::move(listing)]() mutable {
			if (!self)
				return;
			self->m_listing = std::move(listing);
			self->m_listing_loaded = true;
			self->m_listing_busy = false;
			self->showAvailable(); }, Qt::QueuedConnection);
	});
}

void DriverPacksSettingsWidget::showAvailable()
{
	// What is selected now stays selected.
	std::string keep;
	if (const QList<QTreeWidgetItem*> selected = m_ui.availableList->selectedItems(); !selected.isEmpty())
		keep = selected.front()->data(AvailableRelease, ID_ROLE).toString().toStdString();

	const QSignalBlocker block(m_ui.availableList);
	m_ui.availableList->clear();

	QTreeWidgetItem* to_select = nullptr;
	for (const DriverPacks::RemoteDriver& driver : m_listing.drivers)
	{
		QTreeWidgetItem* item = new QTreeWidgetItem(m_ui.availableList);
		item->setData(AvailableRelease, ID_ROLE, QString::fromStdString(driver.id));
		item->setText(AvailableRelease, QString::fromStdString(driver.release_name.empty() ? driver.tag : driver.release_name));
		item->setText(AvailableTag, QString::fromStdString(driver.tag));
		item->setText(AvailableSize, driver.size ? sizeText(driver.size) : QString());
		item->setText(AvailablePublished, dateText(driver.published_at));
		if (findInstalled(driver.id))
			item->setText(AvailableStatus, tr("Installed"));
		else if (driver.prerelease)
			item->setText(AvailableStatus, tr("Pre-release"));

		if (driver.id == keep)
			to_select = item;
	}
	if (!to_select && m_ui.availableList->topLevelItemCount() > 0)
		to_select = m_ui.availableList->topLevelItem(0);
	if (to_select)
		m_ui.availableList->setCurrentItem(to_select);

	// What the listing said about itself.
	QString status;
	if (m_listing_busy)
		status = tr("Looking for driver packs...");
	else if (m_listing_loaded && !m_listing.errors.empty())
	{
		QStringList problems;
		for (const DriverPacks::FeedError& error : m_listing.errors)
			problems << tr("Could not read %1: %2").arg(QString::fromStdString(error.feed_label), QString::fromStdString(error.message));
		status = problems.join(QLatin1Char('\n'));
	}
	else if (m_listing_loaded && m_listing.drivers.empty())
		status = tr("No driver packs are published for this device.");
	else if (m_listing_loaded && m_listing.used_stale_cache)
		status = tr("Showing a saved list; could not reach GitHub just now.");
	m_ui.availableStatus->setText(status);
	m_ui.availableStatus->setVisible(!status.isEmpty());

	m_ui.refreshAvailableButton->setEnabled(!m_listing_busy);
	onAvailableSelectionChanged();
}

void DriverPacksSettingsWidget::onAvailableSelectionChanged()
{
	bool can_download = false;
	if (const QList<QTreeWidgetItem*> selected = m_ui.availableList->selectedItems(); !selected.isEmpty() && !m_installing)
		can_download = !findInstalled(selected.front()->data(AvailableRelease, ID_ROLE).toString().toStdString());
	m_ui.downloadButton->setEnabled(can_download);
}

void DriverPacksSettingsWidget::onRefreshAvailableClicked()
{
	loadAvailable(true);
}

void DriverPacksSettingsWidget::onDownloadClicked()
{
	const QList<QTreeWidgetItem*> selected = m_ui.availableList->selectedItems();
	if (selected.isEmpty() || m_installing)
		return;

	const std::string id = selected.front()->data(AvailableRelease, ID_ROLE).toString().toStdString();
	const auto it = std::find_if(m_listing.drivers.begin(), m_listing.drivers.end(), [&id](const DriverPacks::RemoteDriver& driver) { return driver.id == id; });
	if (it == m_listing.drivers.end())
		return;

	m_installing = true;
	m_install_cancel = Catalog::CancelToken();
	m_ui.installCancel->setText(tr("Cancel"));
	m_ui.installCancel->setEnabled(true);
	m_ui.installProgress->setVisible(true);
	m_ui.installProgress->setRange(0, 0);
	m_ui.installText->setText(tr("Preparing..."));
	m_ui.installStrip->setVisible(true);
	onAvailableSelectionChanged();
	onInstalledSelectionChanged();

	QPointer<DriverPacksSettingsWidget> self(this);
	const std::shared_ptr<DriverPacks::Manager> manager = m_manager;
	const Catalog::CancelToken cancel = m_install_cancel;
	QThreadPool::globalInstance()->start([self, manager, cancel, driver = *it]() {
		const DriverPacks::InstallResult result = manager->Install(driver, [self](const DriverPacks::InstallProgress& progress) { QMetaObject::invokeMethod(QCoreApplication::instance(), [self, progress]() {
				if (self)
					self->showStripProgress(progress); }, Qt::QueuedConnection); }, cancel);

		QMetaObject::invokeMethod(QCoreApplication::instance(), [self, result, driver]() {
			if (self)
				self->finishInstall(result, driver); }, Qt::QueuedConnection);
	});
}

void DriverPacksSettingsWidget::showStripProgress(const DriverPacks::InstallProgress& progress)
{
	if (!m_installing)
		return;

	QString text;
	bool determinate = false;
	switch (progress.stage)
	{
		case DriverPacks::InstallStage::Preparing:
			text = tr("Preparing...");
			break;
		case DriverPacks::InstallStage::Downloading:
			text = progress.total ? tr("Downloading %1 of %2").arg(sizeText(progress.done), sizeText(progress.total)) : tr("Downloading %1").arg(sizeText(progress.done));
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
	m_ui.installText->setText(text);
	if (determinate)
	{
		m_ui.installProgress->setRange(0, 1000);
		m_ui.installProgress->setValue(static_cast<int>(progress.done * 1000 / progress.total));
	}
	else
	{
		m_ui.installProgress->setRange(0, 0);
	}

	// The last steps cannot be cancelled usefully.
	m_ui.installCancel->setEnabled(progress.stage != DriverPacks::InstallStage::Finishing && !m_install_cancel.IsCancelled());
}

void DriverPacksSettingsWidget::showStripMessage(const QString& message)
{
	m_ui.installProgress->setVisible(false);
	m_ui.installText->setText(message);
	m_ui.installCancel->setText(tr("Dismiss"));
	m_ui.installCancel->setEnabled(true);
	m_ui.installStrip->setVisible(true);
}

void DriverPacksSettingsWidget::finishInstall(const DriverPacks::InstallResult& result, const DriverPacks::RemoteDriver& driver)
{
	m_installing = false;
	const QString name = QString::fromStdString(driver.release_name.empty() ? driver.tag : driver.release_name);

	if (result.Ok())
	{
		// Offer the new pack as the choice: one more click and the restart uses it.
		m_installed = m_manager->ListInstalled();
		refreshInstalledList();
		refreshChoice();
		const int index = m_ui.driverCombo->findData(QString::fromStdString(result.id));
		if (index >= 0)
			m_ui.driverCombo->setCurrentIndex(index);
		refreshCurrent();
		showAvailable();
		showStripMessage(tr("Installed %1. Choose it under Driver To Use and press Apply & Restart.").arg(name));
	}
	else if (result.Cancelled())
	{
		showStripMessage(tr("Cancelled."));
		onAvailableSelectionChanged();
	}
	else
	{
		showStripMessage(tr("Could not install %1: %2").arg(name, QString::fromStdString(result.message)));
		onAvailableSelectionChanged();
	}
	onInstalledSelectionChanged();
}

void DriverPacksSettingsWidget::onInstallStripButtonClicked()
{
	if (m_installing)
	{
		m_install_cancel.Cancel();
		m_ui.installCancel->setEnabled(false);
		m_ui.installText->setText(tr("Cancelling..."));
	}
	else
	{
		m_ui.installStrip->setVisible(false);
	}
}

#include "moc_DriverPacksSettingsWidget.cpp"

// ---- Screenshot entries (see QtShot.h) ---------------------------------------------------------------------------
//
// The page only has content on a device with an Adreno GPU. These entries stand one up on any machine
// by giving the page a sysfs tree that has an msm DRM device, which is the seam DriverPacks itself
// offers for its tests (ManagerConfig::sysfs_root). Packs live in a throwaway folder, so what the page
// shows is what a first-time user would see.

#include "QtShot.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>

namespace
{
	QWidget* makeShotPage(const QString& platform_name, bool adreno)
	{
		// A settings window of its own, never shown: the page only needs it for its help text and layer.
		SettingsWindow* window = new SettingsWindow();

		// A sysfs tree with (or without) /class/drm/card0/device/driver -> .../msm. The folder lives as
		// long as the process; the app is about to exit anyway.
		static QTemporaryDir* s_sysfs = new QTemporaryDir();
		QDir root(s_sysfs->path());
		if (adreno)
		{
			root.mkpath(QStringLiteral("class/drm/card0/device"));
			root.mkpath(QStringLiteral("bus/platform/drivers/msm"));
			QFile::link(root.filePath(QStringLiteral("bus/platform/drivers/msm")), root.filePath(QStringLiteral("class/drm/card0/device/driver")));
		}
		else
		{
			root.mkpath(QStringLiteral("class/drm"));
		}

		DriverPacks::ManagerConfig config = DriverPacks::ManagerConfig::ForUser();
		config.sysfs_root = s_sysfs->path().toStdString();
		DriverPacksSettingsWidget* page = new DriverPacksSettingsWidget(window, nullptr, std::move(config), platform_name);
		page->resize(760, 820);
		return page;
	}
} // namespace

QTSHOT_DIALOG("gpu-drivers-adreno", "GPU Drivers page on a device with an Adreno GPU (a fake sysfs tree), Wayland session",
	[](const QtShot::Context&) { return makeShotPage(QStringLiteral("wayland"), true); });
QTSHOT_DIALOG("gpu-drivers-adreno-x11", "GPU Drivers page on a device with an Adreno GPU, X11 session",
	[](const QtShot::Context&) { return makeShotPage(QStringLiteral("xcb"), true); });
QTSHOT_DIALOG("gpu-drivers-no-adreno", "GPU Drivers page on a device without an Adreno GPU",
	[](const QtShot::Context&) { return makeShotPage(QStringLiteral("wayland"), false); });
