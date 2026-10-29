// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ArcadeSetupDialog.h"
#include "QtShot.h"
#include "QtUtils.h"

#include "pcsx2/Catalog/Fetch.h"
#include "pcsx2/Config.h"
#include "pcsx2/GameList.h"

#include "common/FileSystem.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QLocale>
#include <QtCore/QThread>
#include <QtCore/QUrl>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <memory>

namespace
{
	enum class Tone
	{
		Normal,
		Good,
		Bad,
		Muted,
	};

	// Status text is rich text so a state can have a colour; the words say the same thing, so it is never
	// the colour alone that tells.
	QString Colored(const QString& text, Tone tone)
	{
		const QString escaped = text.toHtmlEscaped();
		switch (tone)
		{
			case Tone::Good:
				return QStringLiteral("<span style=\"color:#2ea043\">%1</span>").arg(escaped);
			case Tone::Bad:
				return QStringLiteral("<span style=\"color:#e5484d\">%1</span>").arg(escaped);
			case Tone::Muted:
				return QStringLiteral("<span style=\"color:#8b949e\">%1</span>").arg(escaped);
			default:
				return escaped;
		}
	}

	QColor ToneColor(Tone tone)
	{
		switch (tone)
		{
			case Tone::Good:
				return QColor(0x2e, 0xa0, 0x43);
			case Tone::Bad:
				return QColor(0xe5, 0x48, 0x4d);
			default:
				return QColor(0x8b, 0x94, 0x9e);
		}
	}

	QString FormatTime(std::time_t time)
	{
		return QLocale().toString(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(time)), QLocale::ShortFormat);
	}

	QString FormatBytes(quint64 bytes)
	{
		return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1);
	}

	QLabel* MakeWrappedLabel(const QString& text, QWidget* parent)
	{
		QLabel* label = new QLabel(text, parent);
		label->setWordWrap(true);
		label->setTextFormat(Qt::RichText);
		return label;
	}
} // namespace

ArcadeSetupDialog::ArcadeSetupDialog(QWidget* parent)
	: QDialog(parent)
{
	buildUi();

	connect(m_download_button, &QPushButton::clicked, this, &ArcadeSetupDialog::onDownloadClicked);
	connect(m_install_button, &QPushButton::clicked, this, &ArcadeSetupDialog::onInstallFromFileClicked);
	connect(m_bios_open_button, &QPushButton::clicked, this, &ArcadeSetupDialog::onOpenBiosFolderClicked);
	connect(m_bios_refresh_button, &QPushButton::clicked, this, [this]() {
		refreshBios();
		refreshGames();
	});
	connect(m_games_refresh_button, &QPushButton::clicked, this, [this]() { refreshGames(); });
	connect(m_games_list, &QTreeWidget::itemSelectionChanged, this, &ArcadeSetupDialog::onGameSelectionChanged);

	refreshBootFiles();
	refreshBios();
	refreshGames();
	if (m_boot.installed)
		startUpdateCheck();
}

ArcadeSetupDialog::~ArcadeSetupDialog()
{
	cancelWork();
}

void ArcadeSetupDialog::buildUi()
{
	setWindowTitle(tr("Arcade Setup"));
	// Tall enough for all three steps on a 1080p screen, and no taller than the screen on a smaller one: the page
	// scrolls.
	const QScreen* screen = QGuiApplication::primaryScreen();
	resize(780, screen ? std::min(800, screen->availableGeometry().height() - 60) : 800);

	QVBoxLayout* outer = new QVBoxLayout(this);

	QScrollArea* scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameStyle(QFrame::NoFrame);
	QWidget* page = new QWidget(scroll);
	scroll->setWidget(page);
	QVBoxLayout* root = new QVBoxLayout(page);
	root->setContentsMargins(0, 0, 8, 0);
	outer->addWidget(scroll, 1);

	m_intro = MakeWrappedLabel(
		tr("Namco System 246 and 256 are arcade boards built from PlayStation 2 hardware. Their games run here from your own "
		   "copies. Every game needs three things: the boot files (downloaded below), the BIOS of the board it ran on, and its "
		   "own disc or hard drive image together with its security dongle."),
		page);
	root->addWidget(m_intro);

	QGroupBox* boot = new QGroupBox(tr("1. Boot files"), page);
	QVBoxLayout* boot_layout = new QVBoxLayout(boot);
	boot_layout->addWidget(MakeWrappedLabel(
		tr("The small program every arcade game starts from (boot.elf). It comes from the Proverb project by PS2Homebrew-arcade "
		   "on GitHub: one download of about 1 MB covers every game. If this computer has no network, download proverb.zip "
		   "elsewhere and choose Install from File."),
		boot));
	m_boot_status = MakeWrappedLabel(QString(), boot);
	m_boot_status->setObjectName(QStringLiteral("arcadeBootStatus"));
	boot_layout->addWidget(m_boot_status);
	m_boot_update = MakeWrappedLabel(QString(), boot);
	m_boot_update->setObjectName(QStringLiteral("arcadeBootUpdate"));
	boot_layout->addWidget(m_boot_update);
	m_boot_progress = new QProgressBar(boot);
	m_boot_progress->setObjectName(QStringLiteral("arcadeBootProgress"));
	m_boot_progress->setTextVisible(true);
	m_boot_progress->setVisible(false);
	boot_layout->addWidget(m_boot_progress);
	m_boot_message = MakeWrappedLabel(QString(), boot);
	m_boot_message->setObjectName(QStringLiteral("arcadeBootMessage"));
	m_boot_message->setVisible(false);
	boot_layout->addWidget(m_boot_message);
	QHBoxLayout* boot_buttons = new QHBoxLayout();
	m_download_button = new QPushButton(tr("Download"), boot);
	m_download_button->setObjectName(QStringLiteral("arcadeDownloadButton"));
	m_install_button = new QPushButton(tr("Install from File..."), boot);
	m_install_button->setObjectName(QStringLiteral("arcadeInstallFileButton"));
	boot_buttons->addWidget(m_download_button);
	boot_buttons->addWidget(m_install_button);
	boot_buttons->addStretch(1);
	boot_layout->addLayout(boot_buttons);
	root->addWidget(boot);

	QGroupBox* bios = new QGroupBox(tr("2. Arcade BIOS"), page);
	QVBoxLayout* bios_layout = new QVBoxLayout(bios);
	m_bios_status = MakeWrappedLabel(QString(), bios);
	m_bios_status->setObjectName(QStringLiteral("arcadeBiosStatus"));
	bios_layout->addWidget(m_bios_status);
	m_bios_help = MakeWrappedLabel(QString(), bios);
	bios_layout->addWidget(m_bios_help);
	m_bios_folder = new QLabel(bios);
	m_bios_folder->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_bios_folder->setWordWrap(true);
	bios_layout->addWidget(m_bios_folder);
	QHBoxLayout* bios_buttons = new QHBoxLayout();
	m_bios_open_button = new QPushButton(tr("Open BIOS Folder"), bios);
	m_bios_open_button->setObjectName(QStringLiteral("arcadeOpenBiosFolderButton"));
	m_bios_refresh_button = new QPushButton(tr("Check Again"), bios);
	m_bios_refresh_button->setObjectName(QStringLiteral("arcadeCheckBiosButton"));
	bios_buttons->addWidget(m_bios_open_button);
	bios_buttons->addWidget(m_bios_refresh_button);
	bios_buttons->addStretch(1);
	bios_layout->addLayout(bios_buttons);
	root->addWidget(bios);

	QGroupBox* games = new QGroupBox(tr("3. Your arcade games"), page);
	QVBoxLayout* games_layout = new QVBoxLayout(games);
	m_games_summary = MakeWrappedLabel(QString(), games);
	m_games_summary->setObjectName(QStringLiteral("arcadeGamesSummary"));
	games_layout->addWidget(m_games_summary);
	m_games_list = new QTreeWidget(games);
	m_games_list->setObjectName(QStringLiteral("arcadeGamesList"));
	m_games_list->setColumnCount(3);
	m_games_list->setHeaderLabels({tr("Game"), tr("ID"), tr("Status")});
	m_games_list->setRootIsDecorated(false);
	m_games_list->setUniformRowHeights(true);
	m_games_list->setAlternatingRowColors(true);
	m_games_list->setSelectionMode(QAbstractItemView::SingleSelection);
	m_games_list->setMinimumHeight(130);
	m_games_list->header()->setStretchLastSection(true);
	m_games_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_games_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_games_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	games_layout->addWidget(m_games_list);
	m_game_detail = MakeWrappedLabel(QString(), games);
	m_game_detail->setObjectName(QStringLiteral("arcadeGameDetail"));
	m_game_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
	games_layout->addWidget(m_game_detail);
	QHBoxLayout* games_buttons = new QHBoxLayout();
	m_games_refresh_button = new QPushButton(tr("Check Again"), games);
	m_games_refresh_button->setObjectName(QStringLiteral("arcadeRefreshGamesButton"));
	games_buttons->addWidget(m_games_refresh_button);
	games_buttons->addStretch(1);
	games_layout->addLayout(games_buttons);
	root->addWidget(games);
	root->addStretch(1);

	QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);
}

void ArcadeSetupDialog::runTask(std::function<void()> work, std::function<void()> done)
{
	QThread* thread = QThread::create(std::move(work));
	thread->setParent(this);
	connect(thread, &QThread::finished, this, [this, thread, done = std::move(done)]() {
		m_tasks.removeOne(thread);
		thread->deleteLater();
		done();
	});
	m_tasks.push_back(thread);
	thread->start();
}

void ArcadeSetupDialog::cancelWork()
{
	m_download_cancel.Cancel();
	m_background_cancel.Cancel();
	// The workers use this dialog's service and report back to it, so none may outlive it.
	for (QThread* thread : m_tasks)
		thread->wait();
}

void ArcadeSetupDialog::done(int result)
{
	cancelWork();
	QDialog::done(result);
}

void ArcadeSetupDialog::changeEvent(QEvent* event)
{
	QDialog::changeEvent(event);

	// The player comes back from the file manager after copying a BIOS or a dongle: look again.
	if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_games_list && !m_downloading && !m_installing &&
		!m_checking_games)
	{
		refreshBios();
		refreshGames();
	}
}

void ArcadeSetupDialog::refreshBootFiles()
{
	m_boot = m_service.GetBootFilesStatus();
	updateBootControls();
}

void ArcadeSetupDialog::startUpdateCheck()
{
	m_checking_update = true;
	m_update.reset();
	updateBootControls();

	const auto check = std::make_shared<ArcadeSetup::UpdateCheck>();
	runTask(
		[this, check]() { *check = m_service.CheckForBootFilesUpdate(Catalog::GetDefaultFetcher(), m_background_cancel); },
		[this, check]() {
			m_checking_update = false;
			// A check cut short by closing the dialog is not an answer.
			if (!m_background_cancel.IsCancelled())
				m_update = *check;
			updateBootControls();
		});
}

void ArcadeSetupDialog::updateBootControls()
{
	const bool busy = m_downloading || m_installing;

	if (!m_boot.installed)
	{
		m_boot_status->setText(Colored(tr("Not downloaded yet."), Tone::Bad));
	}
	else
	{
		QString text = Colored(tr("Installed: boot programs for %n game(s).", nullptr, static_cast<int>(m_boot.game_ids.size())), Tone::Good);
		if (!m_boot.release_name.empty())
		{
			QString build = QString::fromStdString(m_boot.release_name).toHtmlEscaped();
			if (m_boot.release_time != 0)
				build += QStringLiteral(" (%1)").arg(tr("built %1").arg(FormatTime(m_boot.release_time)).toHtmlEscaped());
			text += QStringLiteral("<br>") + tr("Build: %1").arg(build);
		}
		else
		{
			text += QStringLiteral("<br>") + tr("Installed from a file on %1.").arg(FormatTime(m_boot.installed_at)).toHtmlEscaped();
		}
		m_boot_status->setText(text);
	}

	QString update;
	if (m_boot.installed)
	{
		if (m_checking_update)
		{
			update = Colored(tr("Checking GitHub for a newer build..."), Tone::Muted);
		}
		else if (m_update.has_value())
		{
			switch (m_update->state)
			{
				case ArcadeSetup::UpdateCheck::State::UpToDate:
					update = Colored(tr("This is the newest build."), Tone::Muted);
					break;
				case ArcadeSetup::UpdateCheck::State::UpdateAvailable:
				{
					QString what = QString::fromStdString(m_update->release_name);
					if (m_update->release_time != 0)
						what += (what.isEmpty() ? QString() : QStringLiteral(", ")) + tr("built %1").arg(FormatTime(m_update->release_time));
					update = Colored(what.isEmpty() ? tr("A newer build is available.") : tr("A newer build is available: %1.").arg(what), Tone::Good);
					break;
				}
				case ArcadeSetup::UpdateCheck::State::Failed:
					update = Colored(tr("Could not check for a newer build: %1").arg(QString::fromStdString(m_update->error)), Tone::Muted);
					break;
				default:
					break;
			}
		}
	}
	m_boot_update->setText(update);
	m_boot_update->setVisible(!update.isEmpty());

	if (m_downloading)
		m_download_button->setText(tr("Cancel"));
	else if (!m_boot.installed)
		m_download_button->setText(tr("Download"));
	else if (m_update.has_value() && m_update->state == ArcadeSetup::UpdateCheck::State::UpdateAvailable)
		m_download_button->setText(tr("Update"));
	else
		m_download_button->setText(tr("Download Again"));
	m_download_button->setEnabled(!m_installing && !(m_downloading && m_download_cancel.IsCancelled()));
	m_install_button->setEnabled(!busy);
	m_boot_progress->setVisible(busy);
}

void ArcadeSetupDialog::setBootMessage(const QString& html)
{
	m_boot_message->setText(html);
	m_boot_message->setVisible(!html.isEmpty());
}

void ArcadeSetupDialog::onDownloadClicked()
{
	if (m_downloading)
	{
		m_download_cancel.Cancel();
		updateBootControls();
		return;
	}

	m_download_cancel = Catalog::CancelToken();
	m_downloading = true;
	m_boot_progress->setRange(0, 0); // Busy until the size is known.
	m_boot_progress->setFormat(tr("Downloading..."));
	setBootMessage(QString());
	updateBootControls();

	ArcadeSetup::DownloadOptions options;
	options.cancel = m_download_cancel;
	// Called on the worker thread; the bar belongs to this one.
	options.on_progress = [this](u64 done, u64 total) {
		QMetaObject::invokeMethod(this, [this, done, total]() { onDownloadProgress(done, total); }, Qt::QueuedConnection);
	};

	const auto result = std::make_shared<ArcadeSetup::InstallResult>();
	runTask(
		[this, result, options]() { *result = m_service.DownloadBootFiles(Catalog::GetDefaultFetcher(), options); },
		[this, result]() { onInstallFinished(*result, true); });
}

void ArcadeSetupDialog::onDownloadProgress(quint64 done, quint64 total)
{
	if (!m_downloading)
		return;
	if (total == 0)
	{
		m_boot_progress->setRange(0, 0);
		m_boot_progress->setFormat(tr("Downloading... %1").arg(FormatBytes(done)));
		return;
	}
	m_boot_progress->setRange(0, static_cast<int>(std::min<quint64>(total, 0x7fffffff)));
	m_boot_progress->setValue(static_cast<int>(std::min<quint64>(done, 0x7fffffff)));
	m_boot_progress->setFormat(tr("Downloading... %1 of %2").arg(FormatBytes(done), FormatBytes(total)));
}

void ArcadeSetupDialog::onInstallFromFileClicked()
{
	const QString path = QFileDialog::getOpenFileName(
		this, tr("Install Arcade Boot Files"), QString(), tr("Proverb Boot Files (*.zip);;All Files (*)"));
	if (path.isEmpty())
		return;

	m_installing = true;
	m_boot_progress->setRange(0, 0);
	m_boot_progress->setFormat(tr("Installing..."));
	setBootMessage(QString());
	updateBootControls();

	const auto result = std::make_shared<ArcadeSetup::InstallResult>();
	runTask(
		[this, result, zip = path.toStdString()]() { *result = m_service.InstallBootFilesFromFile(zip); },
		[this, result]() { onInstallFinished(*result, false); });
}

void ArcadeSetupDialog::onInstallFinished(const ArcadeSetup::InstallResult& result, bool was_download)
{
	m_downloading = false;
	m_installing = false;
	m_boot = result.status; // The boot files as they are now; unchanged when it failed.

	if (result.ok)
	{
		setBootMessage(Colored(was_download ? tr("Downloaded.") : tr("Installed."), Tone::Good));
		m_update.reset();
		if (was_download)
		{
			// What was just downloaded is the release GitHub has.
			ArcadeSetup::UpdateCheck fresh;
			fresh.state = ArcadeSetup::UpdateCheck::State::UpToDate;
			m_update = fresh;
		}
	}
	else if (result.cancelled)
	{
		setBootMessage(Colored(tr("Cancelled."), Tone::Muted));
	}
	else
	{
		setBootMessage(Colored(QString::fromStdString(result.error), Tone::Bad));
	}

	updateBootControls();
	// A game's boot program may have been the missing part.
	refreshGames();
}

void ArcadeSetupDialog::refreshBios()
{
	const ArcadeSetup::BiosStatus bios = m_service.GetBiosStatus();
	if (bios.IsPresent())
	{
		QStringList lines;
		for (const ArcadeSetup::BiosDump& dump : bios.dumps)
		{
			const QString file = QString::fromStdString(dump.file_name).toHtmlEscaped();
			lines.push_back(QStringLiteral("• ") +
							(dump.board.empty() ? file : tr("%1 (%2)").arg(file, QString::fromStdString(dump.board).toHtmlEscaped())));
		}
		m_bios_status->setText(Colored(tr("Arcade BIOS found:"), Tone::Good) + QStringLiteral("<br>") + lines.join(QStringLiteral("<br>")));
		m_bios_help->setText(
			tr("Each game picks the arcade BIOS it runs on by itself. Keep a System 246 dump next to a System 256 one if you have "
			   "both: some games, such as Battle Gear 3, only run on System 246."));
	}
	else
	{
		m_bios_status->setText(Colored(tr("No arcade BIOS found."), Tone::Bad));
		m_bios_help->setText(
			tr("Arcade games need the BIOS of a Namco System 246 or 256 board: a 2 MB file dumped from a board you own. Copy it "
			   "into the BIOS folder below and press Check Again. It is used for arcade games only; your PlayStation 2 BIOS is "
			   "not touched."));
	}
	m_bios_folder->setText(tr("BIOS folder: %1").arg(QDir::toNativeSeparators(QString::fromStdString(EmuFolders::Bios))));
}

void ArcadeSetupDialog::onOpenBiosFolderClicked()
{
	// The folder may not exist yet on a fresh data directory.
	FileSystem::EnsureDirectoryExists(EmuFolders::Bios.c_str(), true);
	QtUtils::OpenURL(this, QUrl::fromLocalFile(QString::fromStdString(EmuFolders::Bios)));
}

void ArcadeSetupDialog::refreshGames()
{
	const u32 generation = ++m_games_generation;

	m_games.clear();
	{
		const auto lock = GameList::GetLock();
		for (u32 i = 0, count = GameList::GetEntryCount(); i < count; i++)
		{
			const GameList::Entry* entry = GameList::GetEntryByIndex(i);
			if (entry->IsArcade())
				m_games.push_back({entry->path, entry->GetTitle(), entry->serial, std::nullopt});
		}
	}
	std::sort(m_games.begin(), m_games.end(), [](const GameRow& a, const GameRow& b) {
		return QString::fromStdString(a.title).localeAwareCompare(QString::fromStdString(b.title)) < 0;
	});

	m_games_list->clear();
	for (size_t i = 0; i < m_games.size(); i++)
	{
		QTreeWidgetItem* item = new QTreeWidgetItem(m_games_list);
		item->setToolTip(0, QDir::toNativeSeparators(QString::fromStdString(m_games[i].path)));
		fillGameRow(static_cast<int>(i));
	}
	updateGamesSummary();
	onGameSelectionChanged();

	if (m_games.empty())
	{
		m_checking_games = false;
		return;
	}

	// Missing() looks at the BIOS folder and the boot files for every game; not on the UI thread.
	m_checking_games = true;
	const auto paths = std::make_shared<std::vector<std::string>>();
	for (const GameRow& game : m_games)
		paths->push_back(game.path);
	const auto reports = std::make_shared<std::vector<ArcadeSetup::MissingReport>>();
	runTask(
		[this, paths, reports]() {
			for (const std::string& path : *paths)
				reports->push_back(m_service.Missing(path));
		},
		[this, reports, generation]() {
			if (generation != m_games_generation)
				return; // A newer check has been started since.
			m_checking_games = false;
			for (size_t i = 0; i < m_games.size() && i < reports->size(); i++)
			{
				m_games[i].report = std::move((*reports)[i]);
				fillGameRow(static_cast<int>(i));
			}
			updateGamesSummary();

			// With nothing chosen yet, show what the first game that cannot start lacks.
			if (m_games_list->selectedItems().isEmpty())
			{
				const auto it = std::find_if(m_games.begin(), m_games.end(),
					[](const GameRow& game) { return game.report.has_value() && !game.report->IsReady(); });
				if (it != m_games.end())
					m_games_list->setCurrentItem(m_games_list->topLevelItem(static_cast<int>(it - m_games.begin())));
			}
			onGameSelectionChanged();
		});
}

void ArcadeSetupDialog::fillGameRow(int index)
{
	QTreeWidgetItem* item = m_games_list->topLevelItem(index);
	if (!item)
		return;

	const GameRow& game = m_games[static_cast<size_t>(index)];
	item->setText(0, QString::fromStdString(game.title));
	item->setText(1, QString::fromStdString(game.serial));

	QString status;
	Tone tone = Tone::Normal;
	if (!game.report.has_value())
	{
		status = tr("Checking...");
		tone = Tone::Muted;
	}
	else if (!game.report->IsReady())
	{
		status = tr("Missing: %1").arg(QString::fromStdString(game.report->Summary()));
		tone = Tone::Bad;
	}
	else if (game.report->attract_only)
	{
		status = tr("Attract mode only");
		tone = Tone::Muted;
	}
	else
	{
		status = tr("Ready to play");
		tone = Tone::Good;
	}
	item->setText(2, status);
	if (tone != Tone::Normal)
		item->setForeground(2, QBrush(ToneColor(tone)));
}

void ArcadeSetupDialog::updateGamesSummary()
{
	if (m_games.empty())
	{
		m_games_summary->setText(Colored(tr("No arcade games in the game list yet."), Tone::Muted));
		return;
	}

	if (m_checking_games)
	{
		m_games_summary->setText(tr("%n arcade game(s) in the game list. Checking what each one needs...", nullptr, static_cast<int>(m_games.size())));
		return;
	}

	int ready = 0;
	for (const GameRow& game : m_games)
		ready += (game.report.has_value() && game.report->IsReady()) ? 1 : 0;
	m_games_summary->setText(tr("%1 of %n arcade game(s) ready to play.", nullptr, static_cast<int>(m_games.size())).arg(ready));
}

void ArcadeSetupDialog::onGameSelectionChanged()
{
	if (m_games.empty())
	{
		m_game_detail->setText(
			tr("An arcade game is an .acgame file: a small text file that names the game's ID and where its image and boot "
			   "program are. Put one in a game folder (Settings > Game List), keep the game's files where it says, and choose "
			   "Settings > Scan For New Games. The game then shows here and in the game list."));
		return;
	}

	const QList<QTreeWidgetItem*> selected = m_games_list->selectedItems();
	if (selected.isEmpty())
	{
		m_game_detail->setText(tr("Select a game to see what it still needs."));
		return;
	}

	const GameRow& game = m_games[static_cast<size_t>(m_games_list->indexOfTopLevelItem(selected.front()))];
	QString text;
	if (!game.report.has_value())
	{
		text = tr("Checking...");
	}
	else if (!game.report->IsReady())
	{
		QStringList lines;
		for (const ArcadeSetup::MissingPart& part : game.report->parts)
			lines.push_back(QStringLiteral("• ") + QString::fromStdString(part.Explain()).toHtmlEscaped());
		text = lines.join(QStringLiteral("<br>"));
	}
	else if (game.report->attract_only)
	{
		text = tr("Everything is in place. This game starts, but so far it only reaches its attract demo.").toHtmlEscaped();
	}
	else
	{
		text = tr("Everything is in place. Start it from the game list.").toHtmlEscaped();
	}
	m_game_detail->setText(text + QStringLiteral("<br><small>%1</small>").arg(QDir::toNativeSeparators(QString::fromStdString(game.path)).toHtmlEscaped()));
}

QTSHOT_DIALOG("arcade-setup", "Tools > Arcade Setup", [](const QtShot::Context&) { return new ArcadeSetupDialog(nullptr); });

#include "moc_ArcadeSetupDialog.cpp"
