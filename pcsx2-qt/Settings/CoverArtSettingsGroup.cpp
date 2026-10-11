// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "CoverArtSettingsGroup.h"
#include "AutoCoversController.h"
#include "SettingWidgetBinder.h"
#include "SettingsWindow.h"

#include "Catalog/CoverAutoDownload.h"

#include "pcsx2/GameList.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>

namespace AutoCovers = Catalog::AutoCovers;

namespace
{
	QString regionName(int region)
	{
		switch (static_cast<Catalog::CoverRegion>(region))
		{
			case Catalog::CoverRegion::USA:
				return CoverArtSettingsGroup::tr("USA");
			case Catalog::CoverRegion::Europe:
				return CoverArtSettingsGroup::tr("Europe");
			case Catalog::CoverRegion::JapanAsia:
				return CoverArtSettingsGroup::tr("Japan / Asia");
			case Catalog::CoverRegion::Disc:
				break;
		}
		return CoverArtSettingsGroup::tr("Disc's own region");
	}

	void fillRegionNames(QComboBox* combo)
	{
		for (int region = 0; region < Catalog::COVER_REGION_COUNT; region++)
			combo->addItem(regionName(region));
	}
} // namespace

CoverArtSettingsGroup::CoverArtSettingsGroup(SettingsWindow* dialog, QWidget* parent)
	: QGroupBox(parent)
{
	m_ui.setupUi(this);

	m_ui.coverStyle->addItem(tr("2D (flat scan)"));
	m_ui.coverStyle->addItem(tr("3D (case render)"));
	fillRegionNames(m_ui.coverRegion);

	const Catalog::CoverPreferences prefs = AutoCovers::LoadPreferences();
	m_ui.coverStyle->setCurrentIndex(prefs.style == Catalog::CoverStyle::Case3D ? 1 : 0);
	m_ui.coverRegion->setCurrentIndex(static_cast<int>(prefs.region));

	// This page is global only, so the binder edits the base settings. Its handler stores the value
	// first (it is connected first), then ours starts the download.
	SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.autoDownloadCovers, AutoCovers::SETTINGS_SECTION, AutoCovers::KEY_AUTO_DOWNLOAD,
		AutoCovers::DEFAULT_AUTO_DOWNLOAD);
	connect(m_ui.autoDownloadCovers, &QCheckBox::toggled, this, [](bool enabled) {
		if (enabled && AutoCoversController::instance())
			AutoCoversController::instance()->startAfterRefresh();
	});

	connect(m_ui.coverStyle, &QComboBox::currentIndexChanged, this, &CoverArtSettingsGroup::onStyleChanged);
	connect(m_ui.coverRegion, &QComboBox::currentIndexChanged, this, &CoverArtSettingsGroup::onRegionChanged);

	dialog->registerWidgetHelp(m_ui.autoDownloadCovers, tr("Download Covers Automatically"), tr("Checked"),
		tr("Downloads a cover for each game that has none after the game list is scanned. Needs an internet connection."));
	dialog->registerWidgetHelp(m_ui.coverStyle, tr("Cover Style"), tr("2D (flat scan)"),
		tr("Flat scans of the box front, or 3D renders of the case. Applies to covers downloaded from now on."));
	dialog->registerWidgetHelp(m_ui.coverRegion, tr("Cover Region"), tr("Disc's own region"),
		tr("Which release's cover to download: the disc's own, or the one from another region. Applies to covers downloaded from now on. "
		   "A game can choose its own region on its Summary page."));
}

CoverArtSettingsGroup::~CoverArtSettingsGroup() = default;

void CoverArtSettingsGroup::onStyleChanged(int index)
{
	AutoCovers::SetStyle(index == 1 ? Catalog::CoverStyle::Case3D : Catalog::CoverStyle::Flat2D);
	offerRedownload(tr("style"));
}

void CoverArtSettingsGroup::onRegionChanged(int index)
{
	AutoCovers::SetLibraryRegion(static_cast<Catalog::CoverRegion>(index));
	offerRedownload(tr("region"));
}

void CoverArtSettingsGroup::offerRedownload(const QString& what)
{
	// The covers on disk still look the old way until they are downloaded again, which is the user's
	// call: it is a lot of downloading.
	if (QMessageBox::question(this, tr("Download Covers Again?"),
			tr("Covers you already have keep their old %1 until they are downloaded again. Download them again now?").arg(what)) != QMessageBox::Yes)
	{
		return;
	}

	if (AutoCoversController* controller = AutoCoversController::instance())
		controller->startRedownload({});
}

void addCoverRegionPinRow(QFormLayout* form, QWidget* after_label, const GameList::Entry* entry, SettingsWindow* dialog)
{
	// The region swap works from the PS2 game database, which has no PS1 or arcade serials.
	if (!entry || entry->type != GameList::EntryType::PS2Disc || !Catalog::IsUsableCoverSerial(entry->serial))
		return;

	int row = 0;
	QFormLayout::ItemRole role;
	form->getWidgetPosition(after_label, &row, &role);
	if (row < 0)
		return;

	QLabel* label = new QLabel(QCoreApplication::translate("CoverArtSettingsGroup", "Cover region:"), form->parentWidget());
	QComboBox* combo = new QComboBox(form->parentWidget());
	combo->setObjectName(QStringLiteral("coverRegionPin"));
	combo->addItem(QCoreApplication::translate("CoverArtSettingsGroup", "Same as library"));
	fillRegionNames(combo);
	label->setBuddy(combo);
	form->insertRow(row + 1, label, combo);

	// The pins are one table in the global settings, not part of the game's own settings.
	const std::optional<Catalog::CoverRegion> pin = AutoCovers::LoadPreferences().PinFor(entry->serial);
	combo->setCurrentIndex(pin.has_value() ? static_cast<int>(*pin) + 1 : 0);

	QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo, serial = entry->serial, path = entry->path](int index) {
		AutoCovers::SetGameRegionPin(serial, index > 0 ? std::optional<Catalog::CoverRegion>(static_cast<Catalog::CoverRegion>(index - 1)) : std::nullopt);

		if (QMessageBox::question(combo, QCoreApplication::translate("CoverArtSettingsGroup", "Download Cover Again?"),
				QCoreApplication::translate("CoverArtSettingsGroup",
					"The cover you have keeps its old region until it is downloaded again. Download it again now?")) == QMessageBox::Yes)
		{
			if (AutoCoversController* controller = AutoCoversController::instance())
				controller->startRedownload({path});
		}
	});

	dialog->registerWidgetHelp(combo, QCoreApplication::translate("CoverArtSettingsGroup", "Cover Region"),
		QCoreApplication::translate("CoverArtSettingsGroup", "Same as library"),
		QCoreApplication::translate("CoverArtSettingsGroup",
			"Use the cover of another region's release of this game. Without a choice here, the library setting on the Game List page applies."));
}

#include "moc_CoverArtSettingsGroup.cpp"
