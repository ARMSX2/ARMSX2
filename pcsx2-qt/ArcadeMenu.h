// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// System > Arcade: the cabinet's coin slots, Service button and Test switch, for an arcade game that is running.
// The same four things the hotkeys do (Hotkeys.cpp), for a player with a mouse and no spare buttons. The menu is
// greyed out unless an arcade game is running; every press is posted to the CPU thread, where the board lives.

#include <QtCore/QObject>

class EmuThread;
class QAction;
class QMenu;

class ArcadeMenu final : public QObject
{
	Q_OBJECT

public:
	/// [menu] and the actions belong to the main window's .ui; the menu follows [thread]'s VM from here on.
	ArcadeMenu(QObject* parent, EmuThread* thread, QMenu* menu, QAction* coin_p1, QAction* coin_p2, QAction* service, QAction* test);

private:
	void updateEnabled();
	void syncTestState();
	void insertCoin(unsigned player);
	void pressService();
	void toggleTest();

	EmuThread* m_thread;
	QMenu* m_menu;
	QAction* m_coin_p1;
	QAction* m_coin_p2;
	QAction* m_service;
	QAction* m_test;

	bool m_vm_running = false;
	bool m_vm_paused = false;
};
