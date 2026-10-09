// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_DEV9LocalLinkWidget.h"

#include "Catalog/LocalLinkSettings.h"

#include "common/Pcsx2Types.h"

#include <QtWidgets/QWidget>

#include <string>

class MemorySettingsInterface;
class SettingsWindow;

/// The Local Link rows of the Network & HDD page: host or join, the host's address, port, room code
/// and peer ID. Every rule (what the room code may contain, the port range, the guest peer ID,
/// seeding) is Catalog::LocalLink in pcsx2/Catalog/LocalLinkSettings.h; this widget only shows the
/// values and hands what the user typed to it.
///
/// It works on the layer its settings window edits: the global INI, or a game's own INI. The values it
/// shows are the effective ones (the game's, else the global ones), and a change writes only the keys
/// that changed, so editing one field in a game's window does not copy the others into the game.
class DEV9LocalLinkWidget final : public QWidget
{
	Q_OBJECT

public:
	DEV9LocalLinkWidget(SettingsWindow* dialog, QWidget* parent);
	~DEV9LocalLinkWidget() override;

	/// Local Link has just become the selected device type: makes sure a room code and, for a guest, a
	/// peer ID exist (an empty room code makes the adapter refuse to start), and shows the values.
	void onSelected();

	/// Shows the stored values again.
	void reload();

protected:
	void showEvent(QShowEvent* event) override;

private Q_SLOTS:
	void onHostToggled(bool host);
	void onAddressEdited();
	void onPortEdited();
	void onRoomCodeEdited();
	void onGenerateClicked();
	void onPeerIdEdited();

private:
	struct Values
	{
		bool host = false;
		std::string address;
		u32 port = 0;
		u32 peer_id = 0;
		std::string room_code;
	};

	Values readValues() const;
	static void writeScratch(MemorySettingsInterface& scratch, const Values& values);
	static Values readScratch(const MemorySettingsInterface& scratch);
	void store(const Values& before, const Values& after);

	/// Runs `rule` on a scratch copy of the effective values, stores what it changed, shows the
	/// outcome. Returns what the rule refused, if anything; nothing is stored then.
	template <typename Rule>
	Catalog::LocalLink::Problem change(Rule&& rule);

	void showValues(const Values& values);
	void showProblem(Catalog::LocalLink::Problem problem);

	Ui::DEV9LocalLinkWidget m_ui;
	SettingsWindow* m_dialog;
};
