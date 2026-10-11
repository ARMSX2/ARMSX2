// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_CoverArtSettingsGroup.h"

#include <QtWidgets/QGroupBox>

class QFormLayout;
class QWidget;
class SettingsWindow;

namespace GameList
{
	struct Entry;
}

/// The "Cover Art" box on the Game List settings page: the automatic download switch, the 2D/3D
/// style and the library-wide cover region (Catalog::AutoCovers). Changing the style or region only
/// affects covers downloaded from then on, so it offers to download the existing ones again.
class CoverArtSettingsGroup final : public QGroupBox
{
	Q_OBJECT

public:
	CoverArtSettingsGroup(SettingsWindow* dialog, QWidget* parent);
	~CoverArtSettingsGroup() override;

private Q_SLOTS:
	void onStyleChanged(int index);
	void onRegionChanged(int index);

private:
	void offerRedownload(const QString& what);

	Ui::CoverArtSettingsGroup m_ui;
};

/// Adds the "Cover region" row to a game's summary page: which release's cover this game uses
/// (the library setting, the disc's own region, or another region). It goes right after the row of
/// `after_label`. Nothing is added for a game the cover region cannot apply to (not a PS2 disc, or
/// no usable serial).
void addCoverRegionPinRow(QFormLayout* form, QWidget* after_label, const GameList::Entry* entry, SettingsWindow* dialog);
