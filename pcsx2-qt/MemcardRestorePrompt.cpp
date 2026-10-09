// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "MemcardRestorePrompt.h"

#include "MainWindow.h"
#include "QtHost.h"

#include "pcsx2/Host.h"
#include "pcsx2/SIO/Memcard/MemcardBackup.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtCore/QMetaObject>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

#include <mutex>

namespace
{
	std::mutex s_mutex;
	const void* s_owner = nullptr;

	QString describe(const MemcardBackup::RestorePrompt& prompt)
	{
		const QString when = QLocale::system().toString(QDateTime::fromMSecsSinceEpoch(prompt.snapshot.taken_at_ms), QLocale::ShortFormat);
		return qApp->translate("MemcardRestorePrompt",
			"The memory card in slot %1 (%2) cannot be read, but it has a backup from %3.")
			.arg(prompt.slot)
			.arg(QString::fromStdString(prompt.card), when);
	}

	// UI thread.
	MemcardBackup::RestoreAnswer ask(const MemcardBackup::RestorePrompt& prompt)
	{
		QWidget* parent = (g_main_window && g_main_window->isVisible()) ? static_cast<QWidget*>(g_main_window) : nullptr;

		QMessageBox box(parent);
		box.setIcon(QMessageBox::Warning);
		box.setWindowIcon(QtHost::GetAppIcon());
		box.setWindowTitle(qApp->translate("MemcardRestorePrompt", "Memory Card Problem"));
		box.setWindowModality(Qt::ApplicationModal);
		box.setText(describe(prompt));
		box.setInformativeText(qApp->translate("MemcardRestorePrompt",
			"Restore the backup before the game starts? The card as it is now is kept as a \"Before a restore\" backup first. "
			"Starting anyway leaves the card as it is."));

		QPushButton* restore = box.addButton(qApp->translate("MemcardRestorePrompt", "Restore Backup"), QMessageBox::AcceptRole);
		QPushButton* start = box.addButton(qApp->translate("MemcardRestorePrompt", "Start Anyway"), QMessageBox::RejectRole);
		box.setDefaultButton(restore);
		box.setEscapeButton(start);
		box.raise();
		box.exec();

		return (box.clickedButton() == restore) ? MemcardBackup::RestoreAnswer::Restore : MemcardBackup::RestoreAnswer::StartAnyway;
	}

	// CPU thread.
	MemcardBackup::RestoreAnswer answer(const MemcardBackup::RestorePrompt& prompt)
	{
		// In Big Picture mode the main window is hidden and a message box would go unseen: Big Picture
		// asks instead, through the handler it registers while it runs.
		if (g_emu_thread && g_emu_thread->isRunningFullscreenUI())
		{
			if (const MemcardBackup::RestorePromptHandler big_picture = MemcardBackup::GetFallbackRestorePromptHandler())
				return big_picture(prompt);
		}

		if (Host::InNoGUIMode() || (g_emu_thread && g_emu_thread->isRunningFullscreenUI()))
		{
			MemcardBackup::ReportUnansweredRestore(prompt.card); // no window to ask in
			return MemcardBackup::RestoreAnswer::StartAnyway;
		}

		MemcardBackup::RestoreAnswer result = MemcardBackup::RestoreAnswer::StartAnyway;
		if (QtHost::IsOnUIThread())
			result = ask(prompt);
		else
			QMetaObject::invokeMethod(qApp, [&]() { result = ask(prompt); }, Qt::BlockingQueuedConnection);
		return result;
	}
} // namespace

void MemcardRestorePrompt::Register(const void* owner)
{
	std::lock_guard lock(s_mutex);
	s_owner = owner;
	MemcardBackup::SetRestorePromptHandler(&answer);
}

void MemcardRestorePrompt::Unregister(const void* owner)
{
	std::lock_guard lock(s_mutex);
	if (s_owner != owner)
		return;

	s_owner = nullptr;
	MemcardBackup::SetRestorePromptHandler({});
}
