// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_DriverPacksSettingsWidget.h"

#include "SettingsWidget.h"

#include "Catalog/CancelToken.h"
#include "Catalog/DriverPacks.h"

#include <memory>
#include <string>
#include <vector>

/// The GPU Drivers page (Linux, global settings only): which Vulkan driver ARMSX2 uses, and the
/// driver packs (Turnip builds for Adreno) that can be downloaded, chosen and deleted.
///
/// Everything it does is DriverPacks::Manager and the free functions of Catalog/DriverPacks.h. A
/// choice takes effect only when ARMSX2 starts, because the Vulkan loader reads its environment
/// once, so the page's one action on a choice is "Apply & Restart": it stores the selection, quits
/// the way File > Exit does, and the process starts again in its place.
class DriverPacksSettingsWidget final : public SettingsWidget
{
	Q_OBJECT

public:
	DriverPacksSettingsWidget(SettingsWindow* dialog, QWidget* parent);

	/// For tests and screenshots: `config` says where packs live and which sysfs tree tells whether
	/// there is an Adreno GPU (DriverPacks::ManagerConfig::sysfs_root), and `platform_name` stands in
	/// for QGuiApplication::platformName() ("wayland", "xcb") when judging a Wayland-only pack.
	DriverPacksSettingsWidget(SettingsWindow* dialog, QWidget* parent, DriverPacks::ManagerConfig config, QString platform_name);

	~DriverPacksSettingsWidget() override;

	/// Whether the settings window lists this page: driver packs exist on this device (an Adreno GPU), or a pack
	/// is installed or selected, so the choice can still be undone.
	static bool shouldOffer();

protected:
	void showEvent(QShowEvent* event) override;

private Q_SLOTS:
	void onChoiceChanged();
	void onApplyClicked();
	void onDeleteClicked();
	void onDownloadClicked();
	void onRefreshAvailableClicked();
	void onInstallStripButtonClicked();
	void onAvailableSelectionChanged();
	void onInstalledSelectionChanged();

private:
	void init();
	void reloadInstalled();
	void refreshCurrent();
	void refreshChoice();
	void refreshInstalledList();
	void loadAvailable(bool force_refresh);
	void showAvailable();
	void showStripProgress(const DriverPacks::InstallProgress& progress);
	void showStripMessage(const QString& message);
	void finishInstall(const DriverPacks::InstallResult& result, const DriverPacks::RemoteDriver& driver);

	const DriverPacks::InstalledDriver* findInstalled(const std::string& id) const;
	bool isWaylandOnly(const DriverPacks::InstalledDriver& driver) const;
	bool sessionIsX11() const;
	std::string chosenId() const;
	std::string appliedId() const;
	bool environmentOverrides() const;
	QString driverTitle(const DriverPacks::InstalledDriver& driver) const;

	Ui::DriverPacksSettingsWidget m_ui;

	std::shared_ptr<DriverPacks::Manager> m_manager;
	QString m_platform_name;
	bool m_available = false;
	bool m_manageable = false; ///< available, or a pack is installed or selected (the page then lists it without the downloads)

	std::vector<DriverPacks::InstalledDriver> m_installed;
	DriverPacks::RemoteListing m_listing;
	bool m_listing_loaded = false;
	bool m_listing_busy = false;
	bool m_installing = false;
	bool m_first_show = true;

	Catalog::CancelToken m_list_cancel;
	Catalog::CancelToken m_install_cancel;
};
