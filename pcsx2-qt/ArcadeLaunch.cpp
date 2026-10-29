// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ArcadeLaunch.h"
#include "ArcadeSetupDialog.h"
#include "QtShot.h"

#include "pcsx2/GameList.h"
#include "pcsx2/VMManager.h"

#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>

#include <cstdio>
#include <memory>

namespace
{
	// An .acgame, or a file the game list knows as an arcade game (an image kept without an .acgame is listed that way).
	bool IsArcadeGame(const std::string& filename)
	{
		if (filename.empty())
			return false;
		if (VMManager::IsArcadeGameFileName(filename))
			return true;

		const auto lock = GameList::GetLock();
		const GameList::Entry* entry = GameList::GetEntryForPath(filename.c_str());
		return entry && entry->IsArcade();
	}
} // namespace

bool ArcadeLaunch::PrepareForBoot(QWidget* parent, VMBootParameters* params)
{
	// A host that already found the board's files says so in arcade_elf; anything that is no arcade game is not ours.
	if (!params->arcade_elf.empty() || !IsArcadeGame(params->filename))
		return true;

	const std::string path = params->filename;
	const auto result = std::make_shared<ArcadeSetup::PrepareResult>();

	// Prepare() copies files (a dongle is up to 64 MB) and reads a zip, so it runs on a thread and this waits in an
	// event loop, with a busy dialog if it takes long enough to notice.
	QThread* thread = QThread::create([result, path]() {
		ArcadeSetup::Service service;
		*result = service.Prepare(path);
	});

	QProgressDialog progress(tr("Getting the arcade game ready..."), QString(), 0, 0, parent);
	progress.setWindowTitle(tr("Arcade"));
	progress.setWindowModality(Qt::WindowModal);
	progress.setCancelButton(nullptr);
	progress.setMinimumDuration(300);

	QEventLoop loop;
	// finished() comes from the thread, so this is queued: it is delivered once the loop runs, never lost.
	QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
	thread->start();
	loop.exec();
	thread->wait();
	delete thread;
	progress.reset();

	if (result->ok)
	{
		result->ApplyTo(params);
		return true;
	}

	ShowMissingReport(parent, *result);
	return false;
}

QMessageBox* ArcadeLaunch::CreateMissingReport(QWidget* parent, const ArcadeSetup::PrepareResult& result)
{
	QMessageBox* box = new QMessageBox(QMessageBox::Warning, tr("Arcade Game Not Ready"), QString(), QMessageBox::NoButton, parent);
	box->setObjectName(QStringLiteral("arcadeMissingReport"));

	const QString title = QString::fromStdString(result.title);
	if (result.missing.parts.empty())
	{
		// Nothing was missing, but getting the files ready failed (a folder that cannot be written, say).
		box->setText(title.isEmpty() ? tr("This arcade game could not be made ready.") : tr("%1 could not be made ready.").arg(title));
		box->setInformativeText(QString::fromStdString(result.error));
	}
	else
	{
		const QString summary = QString::fromStdString(result.missing.Summary());
		box->setText(title.isEmpty() ? tr("This arcade game cannot start yet. It is missing: %1.").arg(summary) :
		                               tr("%1 cannot start yet. It is missing: %2.").arg(title, summary));

		QStringList lines;
		for (const ArcadeSetup::MissingPart& part : result.missing.parts)
			lines.push_back(QStringLiteral("• ") + QString::fromStdString(part.Explain()));
		box->setInformativeText(lines.join(QLatin1Char('\n')));
	}

	QPushButton* setup = box->addButton(tr("Open Arcade Setup"), QMessageBox::ActionRole);
	setup->setObjectName(QStringLiteral("openArcadeSetupButton"));
	box->addButton(QMessageBox::Close);
	box->setDefaultButton(setup);
	return box;
}

void ArcadeLaunch::ShowMissingReport(QWidget* parent, const ArcadeSetup::PrepareResult& result)
{
	QMessageBox* box = CreateMissingReport(parent, result);
	box->exec();
	const bool open_setup = (box->clickedButton() == box->findChild<QPushButton*>(QStringLiteral("openArcadeSetupButton")));
	delete box;

	if (open_setup)
		ShowSetup(parent);
}

void ArcadeLaunch::ShowSetup(QWidget* parent)
{
	ArcadeSetupDialog dialog(parent);
	dialog.exec();
}

// -uishot dialog:arcade-missing with --game <an .acgame>: what the player sees when that game is started now.
QTSHOT_DIALOG("arcade-missing", "Report for the --game .acgame when it is started", [](const QtShot::Context& ctx) -> QWidget* {
	if (ctx.game_path.isEmpty())
	{
		fprintf(stderr, "arcade-missing needs --game <an .acgame>\n");
		return nullptr;
	}

	// The harness links the game into its own folder, where the files the .acgame names are not: look through it.
	ArcadeSetup::Service service;
	const ArcadeSetup::PrepareResult result = service.Prepare(QFileInfo(ctx.game_path).canonicalFilePath().toStdString());
	if (result.ok)
	{
		fprintf(stderr, "arcade-missing: the game is ready, so there is nothing to report\n");
		return nullptr;
	}
	return ArcadeLaunch::CreateMissingReport(nullptr, result);
});

