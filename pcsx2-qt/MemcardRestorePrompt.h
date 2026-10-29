// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The Qt answer to MemcardBackup's boot-time question: "this memory card cannot be read and has a
// backup, restore it before the game starts?"
//
// MemcardBackup calls the handler on the CPU thread, from VMManager::Initialize (through
// FileMcd_Reopen) before any card is opened, and the boot waits for the answer. The question is
// shown on the UI thread through a blocking queued call, and the CPU thread sleeps in that call
// until the player has answered. That is safe because nothing on the UI thread waits for the CPU
// thread during a boot: starting a game only queues EmuThread::startVM, Initialize already blocks on
// the UI thread in several places, no lock is held across the call, and the dialog is application
// modal, so the player cannot trigger the few UI actions that do block on the CPU thread
// (Achievements logout, MainWindow::recreate) while it is open.
//
// No dialog is shown with -nogui (there is no window) or in Big Picture (the main window is hidden,
// and a controller-only player cannot press a Qt button). Big Picture asks through its own handler
// when it has registered one (MemcardBackup::GetFallbackRestorePromptHandler); otherwise the answer
// is "Start Anyway" without asking, and the player is told on screen and in the log where to restore
// from. A stuck boot is worse than booting with the unreadable card.
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
