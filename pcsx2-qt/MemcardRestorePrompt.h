// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The Qt answer to MemcardBackup's boot-time question: "this memory card cannot be read and has a
// backup, restore it before the game starts?"
//
// MemcardBackup calls the handler on the CPU thread, from VMManager::Initialize (through
// FileMcd_Reopen) before any card is opened, and the boot waits for the answer. The question is
// shown on the UI thread through a blocking queued call, and the CPU thread sleeps in that call
// until the player has answered. That is safe because nothing on the UI thread ever waits for the
// CPU thread during a boot:
//   - starting a game only queues EmuThread::startVM; the UI thread does not wait for it;
//   - Initialize itself blocks on the UI thread in several places already (Host::CancelGameListRefresh,
//     Host::AcquireRenderWindow, Host::OnVMStarting's signal), so a UI thread that stays free is already
//     what boot depends on;
//   - no lock is held here (not the settings lock, not the VM info mutex), so a UI thread running its
//     nested event loop for the dialog can read settings and VM state freely;
//   - the dialog is application modal, so the user cannot trigger one of the few UI actions that do
//     block on the CPU thread (Achievements logout, MainWindow::recreate) while it is open.
//
// Cases where a dialog would not be seen, or could not be answered, are answered "Start Anyway"
// without asking, and the player is told on screen and in the log where to restore from:
//   - Big Picture is running (fullscreen UI with the main window hidden; a controller-only player has
//     no way to press a Qt button, and a stuck boot is worse than booting with the unreadable card);
//   - the application was started with -nogui, so there is no window at all.
// "Start Anyway" never changes the card, and the newest good snapshot is never pruned, so the backup
// is still there to restore from Settings > Memory Cards > Backups.

namespace MemcardRestorePrompt
{
	/// Registers the handler with MemcardBackup. `owner` is the main window; calling this again from a
	/// new main window (MainWindow::recreate) simply takes over.
	void Register(const void* owner);

	/// Clears the handler if `owner` is still the one that registered it. A window being replaced
	/// destroys itself after its replacement registered, and must not clear the replacement's handler.
	void Unregister(const void* owner);
} // namespace MemcardRestorePrompt
