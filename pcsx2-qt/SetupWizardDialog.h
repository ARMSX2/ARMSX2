// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_SetupWizardDialog.h"

#include "Catalog/CancelToken.h"
#include "Catalog/DriverPacks.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtWidgets/QDialog>

#include "common/Pcsx2Defs.h"

#include <memory>
#include <optional>

class SetupWizardDialog final : public QDialog
{
	Q_OBJECT

public:
	SetupWizardDialog();
	~SetupWizardDialog();

	/// True if the wizard installed a GPU driver pack and selected it. A pack loads when ARMSX2 starts,
	/// so the caller restarts after the wizard.
	bool driverPackInstalled() const { return m_driver_installed; }

private Q_SLOTS:
	bool canShowNextPage();
	void previousPage();
	void nextPage();
	void confirmCancel();

	void themeChanged();
	void languageChanged();

	void refreshBiosList();
	void biosListItemChanged(const QTreeWidgetItem* current, const QTreeWidgetItem* previous);

	void onDirectoryListContextMenuRequested(const QPoint& point);
	void onDirectoryListSelectionChanged();
	void onAddSearchDirectoryButtonClicked();
	void onRemoveSearchDirectoryButtonClicked();
	void refreshDirectoryList();
	void resizeDirectoryListColumns();

	void onInputDevicesEnumerated(const QList<QPair<QString, QString>>& devices);
	void onInputDeviceConnected(const QString& identifier, const QString& device_name);
	void onInputDeviceDisconnected(const QString& identifier);
	void onRetroAchievementsLoginLogoutPressed();
	void onRetroAchievementsViewProfilePressed();

protected:
	void resizeEvent(QResizeEvent* event);

private:
	enum Page : u32
	{
		Page_Language,
		Page_BIOS,
		Page_GameList,
		Page_Controller,
		Page_RetroAchievements,
		Page_AutoTune,
		Page_Driver,
		Page_Complete,
		Page_Count,
	};

	void setupUi();
	void setupLanguagePage();
	void setupBIOSPage();
	void setupGameListPage();
	void setupControllerPage();
	void setupRetroAchievementsPage();
	void setupAutoTunePage();
	void setupDriverPage();
	void setupCompletePage();
	void refreshRetroAchievementsLoginState();

	void pageChangedTo(int page);
	void updatePageLabels(int prev_page);
	void updatePageButtons();

	/// The page to show after / before `page`, stepping over the GPU driver page when it is not offered.
	int pageAfter(int page) const;
	int pageBefore(int page) const;
	void loadDriverOffer();
	void showDriverOffer(const DriverPacks::RemoteListing& listing);
	void onDriverInstallClicked();
	void showDriverProgress(const DriverPacks::InstallProgress& progress);
	void finishDriverInstall(const DriverPacks::InstallResult& result);

	void addPathToTable(const std::string& path, bool recursive);

	void openAutomaticMappingMenu(u32 port, QLabel* update_label);
	void doDeviceAutomaticBinding(u32 port, QLabel* update_label, const QString& device);

	Ui::SetupWizardDialog m_ui;

	std::array<QLabel*, Page_Count> m_page_labels;

	QList<QPair<QString, QString>> m_device_list;

	std::shared_ptr<DriverPacks::Manager> m_driver_manager;
	std::optional<DriverPacks::RemoteDriver> m_driver_offer;
	Catalog::CancelToken m_driver_cancel;
	bool m_driver_page_enabled = false;
	bool m_driver_listing_started = false;
	bool m_driver_installing = false;
	bool m_driver_installed = false;
};
