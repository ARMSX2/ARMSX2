// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "FolderSettingsWidget.h"
#include "SettingWidgetBinder.h"
#include "SettingsWindow.h"

FolderSettingsWidget::FolderSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent)
	: SettingsWidget(settings_dialog, parent)
{
	SettingsInterface* sif = dialog()->getSettingsInterface();

	setupTab(m_ui);

	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.cache, m_ui.cacheBrowse, m_ui.cacheOpen, m_ui.cacheReset, "Folders", "Cache", Path::Combine(EmuFolders::DataRoot, "cache"));
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.cheats, m_ui.cheatsBrowse, m_ui.cheatsOpen, m_ui.cheatsReset, "Folders", "Cheats", Path::Combine(EmuFolders::DataRoot, "cheats"));
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.covers, m_ui.coversBrowse, m_ui.coversOpen, m_ui.coversReset, "Folders", "Covers", Path::Combine(EmuFolders::DataRoot, "covers"));
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.snapshots, m_ui.snapshotsBrowse, m_ui.snapshotsOpen, m_ui.snapshotsReset, "Folders", "Snapshots", Path::Combine(EmuFolders::DataRoot, "snaps"));
	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.organizeSnapshotsByGame, "EmuCore/GS", "OrganizeScreenshotsByGame", false);
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.saveStates, m_ui.saveStatesBrowse, m_ui.saveStatesOpen, m_ui.saveStatesReset,
		"Folders", "Savestates", Path::Combine(EmuFolders::DataRoot, "sstates"));
	dialog()->registerWidgetHelp(m_ui.cache, tr("Cache Directory"), tr("Default"),
		tr("Location used for caching compiled shaders, game covers, and temporary files."));
	dialog()->registerWidgetHelp(m_ui.cheats, tr("Cheats Directory"), tr("Default"),
		tr("Location where user pnach cheat files are stored."));
	dialog()->registerWidgetHelp(m_ui.covers, tr("Covers Directory"), tr("Default"),
		tr("Location where game box art and cover images are saved."));
	dialog()->registerWidgetHelp(m_ui.snapshots, tr("Snapshots Directory"), tr("Default"),
		tr("Location where screenshots taken during gameplay are saved."));
	dialog()->registerWidgetHelp(m_ui.organizeSnapshotsByGame, tr("Save Snapshots in Game-Specific Folders"), tr("Unchecked"),
		tr("Saves snapshots to per-game subfolders instead of a shared folder."));
	dialog()->registerWidgetHelp(m_ui.saveStates, tr("Save States Directory"), tr("Default"),
		tr("Location where save state files are saved and loaded from."));
}

FolderSettingsWidget::~FolderSettingsWidget() = default;

#include "moc_FolderSettingsWidget.cpp"
