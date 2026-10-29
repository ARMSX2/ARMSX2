// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ArcadeMenu.h"
#include "QtHost.h"

#include "pcsx2/Arcade/ArcadeInput.h"
#include "pcsx2/Host.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtWidgets/QMenu>

namespace
{
	// Hold time for one Service press from the menu. The game reads the board once per frame, so a shorter
	// tap can be missed.
	constexpr int SERVICE_PRESS_MS = 200;
} // namespace

ArcadeMenu::ArcadeMenu(QObject* parent, EmuThread* thread, QMenu* menu, QAction* coin_p1, QAction* coin_p2, QAction* service, QAction* test)
	: QObject(parent)
	, m_thread(thread)
	, m_menu(menu)
	, m_coin_p1(coin_p1)
	, m_coin_p2(coin_p2)
	, m_service(service)
	, m_test(test)
{
	connect(m_coin_p1, &QAction::triggered, this, [this]() { insertCoin(0); });
	connect(m_coin_p2, &QAction::triggered, this, [this]() { insertCoin(1); });
	connect(m_service, &QAction::triggered, this, [this]() { pressService(); });
	connect(m_test, &QAction::triggered, this, [this]() { toggleTest(); });
	// The Test switch can also be flipped with its hotkey, so read it back when the menu is opened.
	connect(m_menu, &QMenu::aboutToShow, this, [this]() { syncTestState(); });

	connect(m_thread, &EmuThread::onVMStarted, this, [this]() {
		m_vm_running = true;
		m_vm_paused = false;
		updateEnabled();
	});
	connect(m_thread, &EmuThread::onVMStopped, this, [this]() {
		m_vm_running = false;
		m_vm_paused = false;
		updateEnabled();
	});
	connect(m_thread, &EmuThread::onVMPaused, this, [this]() { m_vm_paused = true; });
	connect(m_thread, &EmuThread::onVMResumed, this, [this]() { m_vm_paused = false; });

	updateEnabled();
}

void ArcadeMenu::updateEnabled()
{
	// IsActive() is true from the moment an arcade game starts booting until its VM is gone.
	const bool enabled = m_vm_running && ArcadeInput::IsActive();
	m_menu->setEnabled(enabled);
	for (QAction* action : {m_coin_p1, m_coin_p2, m_service, m_test})
		action->setEnabled(enabled);

	if (enabled)
		syncTestState();
	else
		m_test->setChecked(false);
}

void ArcadeMenu::syncTestState()
{
	// Read directly, not through the CPU thread; a stale value only shows the old tick briefly.
	const bool on = ArcadeInput::IsTestOn();
	const QSignalBlocker blocker(m_test);
	m_test->setChecked(on);
}

void ArcadeMenu::insertCoin(unsigned player)
{
	Host::RunOnCPUThread([player]() { ArcadeInput::InsertCoin(player); });
}

void ArcadeMenu::pressService()
{
	// The game only reads the Service button while it runs, so resume a paused game first (as the Android
	// pause menu does).
	if (m_vm_paused)
		m_thread->setVMPaused(false);

	Host::RunOnCPUThread([]() { ArcadeInput::SetService(true); });
	QTimer::singleShot(SERVICE_PRESS_MS, this, []() { Host::RunOnCPUThread([]() { ArcadeInput::SetService(false); }); });
}

void ArcadeMenu::toggleTest()
{
	// The click already toggled the tick; read it back after the CPU thread has applied the toggle, in case
	// the hotkey moved the switch in between.
	Host::RunOnCPUThread([]() { ArcadeInput::ToggleTest(); });
	QTimer::singleShot(250, this, [this]() { syncTestState(); });
}

#include "moc_ArcadeMenu.cpp"
