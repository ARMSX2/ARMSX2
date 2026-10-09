// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "SettingsWidget.h"

#include "pcsx2/Arcade/ArcadeInput.h"

#include <vector>

class QComboBox;
class QLabel;
class QPushButton;

// What each button of a controller does on a Namco System 246/256 cabinet. The cabinet has levers, numbered buttons,
// a coin slot and so on; ArcadeInput lays the controller out on it for each kind of cabinet, and this page lets the
// player move a button to another job. The page is the same widget in two windows, as every settings page is:
//   - in a game's own settings (a game whose ID is an arcade game's): that game's layout. Each button shows the job it
//     does on this cabinet and offers the cabinet's other jobs. Stored in the game's settings file, [Arcade] Controls.
//   - in the global settings: the layout every arcade game starts from. Each button shows which controller button it
//     acts as. Stored in the global settings, [Arcade] Controls.
// A game's own entry for a button wins over the global one.
class ArcadeControlsSettingsWidget final : public SettingsWidget
{
	Q_OBJECT

public:
	ArcadeControlsSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent);
	~ArcadeControlsSettingsWidget() override;

protected:
	void showEvent(QShowEvent* event) override;

private:
	struct Row
	{
		const ArcadeInput::ButtonInfo* button;
		QLabel* name;
		QComboBox* combo;
		QLabel* source;
		bool fixed = false; // An analog pedal on this cabinet: stays where it is.
	};

	void buildUi();
	void loadRemaps();
	void refreshRows();
	void onChoice(size_t row, int combo_index);
	void onResetClicked();
	void saveRemap(const ArcadeInput::Remap& remap);

	bool m_per_game;
	std::string m_gameid;
	ArcadeInput::Layout m_layout;
	ArcadeInput::Remap m_global;
	ArcadeInput::Remap m_game;

	QLabel* m_mode_label = nullptr;
	QPushButton* m_reset_button = nullptr;
	std::vector<Row> m_rows;
};
