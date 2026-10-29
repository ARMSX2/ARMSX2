// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "TexturePacksSettingsWidget.h"
#include "QtHost.h"
#include "SettingsWindow.h"
#include "TexturePackService.h"

#include "Catalog/Fetch.h"

#include "pcsx2/Config.h"
#include "pcsx2/GameList.h"

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QUrl>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QMessageBox>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace Tex = Catalog;
using Engine = Catalog::TexturePackEngine::AstcSupport;

namespace
{
	enum Column
	{
		ColName,
		ColGame,
		ColSize,
		ColStatus,
	};

	constexpr int SIZE_ROLE = Qt::UserRole + 1;

	class PackItem final : public QTreeWidgetItem
	{
	public:
		using QTreeWidgetItem::QTreeWidgetItem;

		bool operator<(const QTreeWidgetItem& other) const override
		{
			const int column = treeWidget() ? treeWidget()->sortColumn() : ColName;
			if (column == ColSize)
				return data(ColSize, SIZE_ROLE).toULongLong() < other.data(ColSize, SIZE_ROLE).toULongLong();
			return QtHost::LocaleSensitiveCompare(text(column), other.text(column)) < 0;
		}
	};

	QString typeName(const std::string& type)
	{
		if (type == "ai_upscale")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "AI upscale");
		if (type == "handcrafted")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "Handcrafted");
		if (type == "mixed")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "Mixed");
		if (type == "port")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "Port");
		if (type == "button_replacement")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "Button replacement");
		return QString::fromStdString(type); // a value a newer catalog added
	}

	QString completenessName(const std::string& completeness)
	{
		if (completeness == "complete")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "Complete");
		if (completeness == "in_progress")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "In progress");
		if (completeness == "incomplete")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "Incomplete");
		if (completeness == "partial")
			return QCoreApplication::translate("TexturePacksSettingsWidget", "Partial");
		return QString::fromStdString(completeness);
	}

	/// A link to an https page, or just the text when the address is not one. Catalog text is
	/// untrusted, so everything that goes into a rich text label is escaped.
	QString htmlLink(const std::string& url, const QString& text)
	{
		if (!Tex::IsHttpsUrl(url))
			return text.toHtmlEscaped();
		return QStringLiteral("<a href=\"%1\">%2</a>").arg(QString::fromStdString(url).toHtmlEscaped(), text.toHtmlEscaped());
	}

	/// "3 hours", "2 days": how long ago, for the saved-copy notice.
	QString ageText(std::time_t then)
	{
		const qint64 seconds = std::max<qint64>(0, QDateTime::currentSecsSinceEpoch() - static_cast<qint64>(then));
		if (seconds < 90)
			return QCoreApplication::translate("TexturePacksSettingsWidget", "a moment");
		if (seconds < 90 * 60)
			return QCoreApplication::translate("TexturePacksSettingsWidget", "%n minute(s)", nullptr, static_cast<int>(seconds / 60));
		if (seconds < 36 * 3600)
			return QCoreApplication::translate("TexturePacksSettingsWidget", "%n hour(s)", nullptr, static_cast<int>(seconds / 3600));
		return QCoreApplication::translate("TexturePacksSettingsWidget", "%n day(s)", nullptr, static_cast<int>(seconds / 86400));
	}

	std::optional<u64> freeSpaceAt(const std::string& directory)
	{
		std::error_code ec;
		std::filesystem::path path(directory);
		while (!path.empty() && !std::filesystem::exists(path, ec))
			path = path.parent_path();
		if (path.empty())
			return std::nullopt;
		const std::filesystem::space_info info = std::filesystem::space(path, ec);
		if (ec)
			return std::nullopt;
		return static_cast<u64>(info.available);
	}

	/// A byte count in a progress line: whole MB hides a small download's progress ("0 MB of 3 MB").
	QString progressSizeText(u64 bytes)
	{
		if (bytes < 10ull * 1024 * 1024)
			return QCoreApplication::translate("TexturePacksSettingsWidget", "%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
		return QString::fromStdString(Tex::FormatPackSize(bytes));
	}

	/// A size for an installed folder, which can be small: the catalog's MB rounds a few hundred KB to 0.
	QString installedSizeText(u64 bytes)
	{
		if (bytes < 1024ull * 1024)
			return QCoreApplication::translate("TexturePacksSettingsWidget", "%1 KB").arg(bytes / 1024);
		return QString::fromStdString(Tex::FormatPackSize(bytes));
	}

	QString foldCase(const QString& text)
	{
		return text.toLower();
	}
} // namespace

std::string TexturePacksSettingsWidget::Row::key() const
{
	if (pack)
		return "pack:" + pack->id;
	return "folder:" + (installed ? installed->serial : std::string());
}

TexturePacksSettingsWidget::TexturePacksSettingsWidget(SettingsWindow* dialog, QWidget* parent)
	: SettingsWidget(dialog, parent)
	, m_service(&TexturePackService::instance())
	, m_per_game(dialog->isPerGameSettings())
{
	setupTab(m_ui);

	// What the catalog says is untrusted text: show it as plain text, and build the few rich text
	// labels from escaped pieces.
	for (QLabel* label : {m_ui.scopeLabel, m_ui.statusLabel, m_ui.emptyLabel, m_ui.detailTitle, m_ui.detailGame, m_ui.detailType, m_ui.detailCompleteness,
			 m_ui.detailSize, m_ui.detailSpace, m_ui.detailLicense, m_ui.detailInstalled, m_ui.detailDescription, m_ui.detailCredits, m_ui.astcBadge,
			 m_ui.installText, m_ui.detailHint})
	{
		label->setTextFormat(Qt::PlainText);
	}
	m_ui.detailCreators->setTextFormat(Qt::RichText);
	m_ui.detailSource->setTextFormat(Qt::RichText);

	QFont title_font = m_ui.detailTitle->font();
	title_font.setBold(true);
	title_font.setPointSizeF(title_font.pointSizeF() * 1.25);
	m_ui.detailTitle->setFont(title_font);
	m_ui.astcBadge->setStyleSheet(QStringLiteral("QLabel { background: #fff3cd; color: #664d03; border: 1px solid #ffda6a; border-radius: 4px; padding: 6px; }"));

	m_ui.packList->setColumnWidth(ColName, 270);
	m_ui.packList->setColumnWidth(ColGame, 190);
	m_ui.packList->setColumnWidth(ColSize, 70);
	m_ui.packList->header()->setStretchLastSection(true);
	m_ui.packList->header()->setSortIndicator(-1, Qt::AscendingOrder);
	m_ui.splitter->setSizes({250, 300});

	// The filters, in the order of the Filter enum. A game's window has no "My Games": the list is
	// already that game's.
	m_ui.filterTabs->setDrawBase(false);
	m_ui.filterTabs->setExpanding(false);
	m_ui.filterTabs->addTab(tr("Popular"));
	m_ui.filterTabs->addTab(tr("All"));
	m_ui.filterTabs->addTab(tr("My Games"));
	m_ui.filterTabs->addTab(tr("Installed"));
	m_ui.filterTabs->setTabVisible(static_cast<int>(Filter::MyGames), !m_per_game);
	m_ui.filterTabs->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	m_ui.filterTabs->updateGeometry();
	connect(m_ui.filterTabs, &QTabBar::currentChanged, this, &TexturePacksSettingsWidget::onFilterChanged);
	connect(m_ui.searchEdit, &QLineEdit::textChanged, this, [this]() { refreshList(); });
	connect(m_ui.refreshButton, &QToolButton::clicked, this, [this]() { m_service->loadCatalog(true); });
	connect(m_ui.retryButton, &QPushButton::clicked, this, &TexturePacksSettingsWidget::onRetryClicked);
	connect(m_ui.packList, &QTreeWidget::itemSelectionChanged, this, &TexturePacksSettingsWidget::onSelectionChanged);
	connect(m_ui.installButton, &QPushButton::clicked, this, &TexturePacksSettingsWidget::onInstallClicked);
	connect(m_ui.deleteButton, &QPushButton::clicked, this, &TexturePacksSettingsWidget::onDeleteClicked);
	connect(m_ui.installCancel, &QPushButton::clicked, this, &TexturePacksSettingsWidget::onInstallStripButtonClicked);

	connect(m_service, &TexturePackService::catalogChanged, this, &TexturePacksSettingsWidget::onCatalogChanged);
	connect(m_service, &TexturePackService::installedChanged, this, &TexturePacksSettingsWidget::onInstalledChanged);
	connect(m_service, &TexturePackService::installStateChanged, this, &TexturePacksSettingsWidget::onInstallStateChanged);
	connect(m_service, &TexturePackService::installFinished, this, &TexturePacksSettingsWidget::onInstallFinished);
	connect(m_service, &TexturePackService::removeFinished, this, [this](bool ok, const QString& folder) {
		if (!ok)
		{
			QMessageBox::warning(this, tr("Texture Packs"),
				tr("Could not delete the texture pack in the folder \"%1\". Some of its files may still be in use.").arg(folder));
		}
	});

	dialog->registerWidgetHelp(m_ui.packList, tr("Texture Packs"), tr("N/A"),
		tr("Texture packs replace a game's textures with higher resolution ones. Pick a pack to see who made it and what it needs, then download it. "
		   "Texture replacement is switched on for you when a pack is installed."));
	dialog->registerWidgetHelp(m_ui.searchEdit, tr("Search"), tr("N/A"), tr("Finds packs by name, game, serial or creator."));

	refreshContext();
	refreshStatus();
	showInstallStrip();
}

TexturePacksSettingsWidget::~TexturePacksSettingsWidget() = default;

void TexturePacksSettingsWidget::showEvent(QShowEvent* event)
{
	QWidget::showEvent(event);

	refreshContext();
	queryAstcSupport();

	if (m_first_show)
	{
		m_first_show = false;

		// Android opens on the game the player is in, else on what is popular today.
		if (m_per_game)
			m_ui.filterTabs->setCurrentIndex(static_cast<int>(Filter::All));
		else if (!m_context_serial.empty())
			m_ui.filterTabs->setCurrentIndex(static_cast<int>(Filter::MyGames));
	}

	if (m_service->catalog().status == TexturePackService::CatalogStatus::NotLoaded)
		m_service->loadCatalog(false);
	m_service->refreshInstalled();

	onCatalogChanged();
}

void TexturePacksSettingsWidget::refreshContext()
{
	m_context_serial.clear();
	if (m_per_game)
	{
		if (const std::optional<std::string> serial = Tex::NormalizeSerial(dialog()->getSerial()))
			m_context_serial = *serial;
	}
	else if (QtHost::IsVMValid())
	{
		if (const std::optional<std::string> serial = Tex::NormalizeSerial(QtHost::GetCurrentGameSerial().toStdString()))
			m_context_serial = *serial;
	}

	// The games in the library, which "My Games" and the choice of install folder use.
	m_owned_serials.clear();
	{
		const auto lock = GameList::GetLock();
		const u32 count = GameList::GetEntryCount();
		for (u32 i = 0; i < count; i++)
		{
			const GameList::Entry* entry = GameList::GetEntryByIndex(i);
			if (!entry)
				continue;
			if (const std::optional<std::string> serial = Tex::NormalizeSerial(entry->serial))
				m_owned_serials.insert(*serial);
		}
	}

	if (m_per_game)
	{
		m_ui.scopeLabel->setVisible(true);
		m_ui.scopeLabel->setText(m_context_serial.empty() ?
									 tr("This game has no serial, so no texture packs can be matched to it.") :
									 tr("Showing the texture packs for this game (%1).").arg(QString::fromStdString(m_context_serial)));
	}
}

void TexturePacksSettingsWidget::queryAstcSupport()
{
	// The answer comes from the graphics device, which only exists while a game runs, and arrives on
	// the GS thread.
	QPointer<TexturePacksSettingsWidget> self(this);
	Catalog::TexturePackEngine::QueryAstcSupport([self](Engine support) {
		QtHost::RunOnUIThread([self, support]() {
			if (self)
				self->setAstcSupport(support);
		});
	});
}

void TexturePacksSettingsWidget::setAstcSupport(Engine support)
{
	if (m_astc == support || (support == Engine::Unknown && m_astc != Engine::Unknown))
		return;
	m_astc = support;
	refreshList();
}

void TexturePacksSettingsWidget::onCatalogChanged()
{
	// The rows point into the packs of the catalog that is being replaced.
	m_rows.clear();
	m_packs = m_service->catalog().packs;
	refreshStatus();
	refreshList();
}

void TexturePacksSettingsWidget::onInstalledChanged()
{
	refreshList();
}

void TexturePacksSettingsWidget::onInstallStateChanged()
{
	showInstallStrip();

	// The row of the pack being installed says so while it runs; the list only needs rebuilding when
	// an install starts or ends, not at every progress report.
	if (m_service->install().active != m_install_was_active)
	{
		m_install_was_active = m_service->install().active;
		refreshList();
		return;
	}
	if (const Row* row = currentRow())
		showDetail(row);
}

void TexturePacksSettingsWidget::onInstallFinished()
{
	// The install switched Load Textures on in the settings; show it on the Graphics page, whose
	// checkbox does not watch the setting.
	const TexturePackService::InstallOutcome& outcome = m_service->lastOutcome();
	if (outcome.ok && !m_per_game)
	{
		if (QCheckBox* box = dialog()->findChild<QCheckBox*>(QStringLiteral("loadTextureReplacements")); box && !box->isChecked())
			box->setChecked(true);
	}

	// An install made from a game's own settings: if that game turns replacement off, the pack would do
	// nothing for it. The window holds the game's settings open, so it changes them itself.
	const bool started_here = std::exchange(m_install_started_here, false);
	if (outcome.ok && started_here && m_per_game && Catalog::TexturePackEngine::EnableInGameLayer(*dialog()->getSettingsInterface()))
	{
		dialog()->saveAndReloadGameSettings();
		if (QCheckBox* box = dialog()->findChild<QCheckBox*>(QStringLiteral("loadTextureReplacements")))
		{
			const QSignalBlocker blocker(box);
			box->setCheckState(Qt::Checked);
		}
	}
}

void TexturePacksSettingsWidget::onRetryClicked()
{
	m_service->loadCatalog(true);
}

void TexturePacksSettingsWidget::refreshStatus()
{
	using Status = TexturePackService::CatalogStatus;
	const TexturePackService::CatalogData& catalog = m_service->catalog();

	QString text;
	bool retry = false;
	switch (catalog.status)
	{
		case Status::NotLoaded:
		case Status::Loading:
			text = tr("Loading catalog...");
			break;

		case Status::Ready:
			if (catalog.source == Tex::TexturePackCatalog::Source::StaleCache)
			{
				text = tr("Showing a saved copy of the catalog from %1 ago. Could not refresh.").arg(ageText(catalog.fetched_at));
				retry = true;
			}
			break;

		case Status::Unavailable:
			text = tr("Could not reach the texture catalog. Check your connection and try again.");
			retry = true;
			break;
	}

	m_ui.statusLabel->setText(text);
	m_ui.statusLabel->setVisible(!text.isEmpty());
	m_ui.retryButton->setVisible(retry);
	m_ui.refreshButton->setEnabled(catalog.status != Status::Loading);
}

const Tex::TexturePack* TexturePacksSettingsWidget::packForId(const std::string& id) const
{
	if (!m_packs)
		return nullptr;
	for (const Tex::TexturePack& pack : *m_packs)
	{
		if (pack.id == id)
			return &pack;
	}
	return nullptr;
}

std::string TexturePacksSettingsWidget::installSerialFor(const Tex::TexturePack& pack) const
{
	return pack.InstallSerialFor(m_context_serial, m_owned_serials);
}

bool TexturePacksSettingsWidget::rowMatchesSearch(const Row& row, const QString& needle) const
{
	if (needle.isEmpty())
		return true;

	QStringList haystack;
	if (row.pack)
	{
		const Tex::TexturePack& pack = *row.pack;
		haystack << QString::fromStdString(pack.name) << QString::fromStdString(pack.game_title);
		for (const std::string& serial : pack.serials)
			haystack << QString::fromStdString(serial);
		for (const Tex::TexturePackCreator& creator : pack.creators)
			haystack << QString::fromStdString(creator.name);
	}
	if (row.installed)
		haystack << QString::fromStdString(row.installed->serial);

	return std::any_of(haystack.begin(), haystack.end(), [&needle](const QString& text) { return foldCase(text).contains(needle); });
}

QString TexturePacksSettingsWidget::statusTextFor(const Row& row) const
{
	const TexturePackService::InstallState& install = m_service->install();
	if (row.pack && install.active && install.pack_id == row.pack->id)
		return tr("Installing...");

	if (!row.pack)
		return tr("Installed");

	switch (m_service->actionFor(*row.pack))
	{
		case Tex::InstallAction::Installed:
			return tr("Installed");
		case Tex::InstallAction::Update:
		case Tex::InstallAction::Conflict:
			return tr("Update available");
		case Tex::InstallAction::Install:
			break;
	}
	return QString();
}

void TexturePacksSettingsWidget::refreshList()
{
	const TexturePackService::CatalogData& catalog = m_service->catalog();
	const TexturePackService::InstalledData& installed = m_service->installed();
	const QString needle = foldCase(m_ui.searchEdit->text().trimmed());

	// The row to keep selected across the rebuild is m_selected_key, which onSelectionChanged() keeps.
	// A game's list is that game's packs only.
	const auto in_scope = [this](const Tex::TexturePack& pack) { return !m_per_game || (!m_context_serial.empty() && pack.MatchesSerial(m_context_serial)); };

	std::vector<Row> rows;
	if (m_filter == Filter::Installed)
	{
		for (const Tex::TexturePackInstaller::InstalledPack& folder : installed.packs)
		{
			Row row;
			row.installed = folder;
			if (folder.record)
				row.pack = packForId(folder.record->pack_id);
			if (m_per_game && (m_context_serial.empty() || folder.serial != m_context_serial))
				continue;
			rows.push_back(std::move(row));
		}
	}
	else if (m_packs)
	{
		const auto installed_folder_for = [&installed](const Tex::TexturePack& pack) -> std::optional<Tex::TexturePackInstaller::InstalledPack> {
			for (const Tex::TexturePackInstaller::InstalledPack& folder : installed.packs)
			{
				if (folder.record && folder.record->pack_id == pack.id)
					return folder;
			}
			return std::nullopt;
		};

		const auto add = [&](const Tex::TexturePack& pack) {
			if (!in_scope(pack))
				return;
			Row row;
			row.pack = &pack;
			row.installed = installed_folder_for(pack);
			rows.push_back(std::move(row));
		};

		switch (m_filter)
		{
			case Filter::Popular:
				if (catalog.popular)
				{
					for (const std::string& id : *catalog.popular)
					{
						if (const Tex::TexturePack* pack = packForId(id))
							add(*pack);
					}
				}
				break;

			case Filter::MyGames:
				for (const Tex::TexturePack& pack : *m_packs)
				{
					if (pack.CoversAnyOf(m_owned_serials) || (!m_context_serial.empty() && pack.MatchesSerial(m_context_serial)))
						add(pack);
				}
				break;

			case Filter::All:
				for (const Tex::TexturePack& pack : *m_packs)
					add(pack);
				std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
					return QtHost::LocaleSensitiveCompare(QString::fromStdString(a.pack->game_title.empty() ? a.pack->name : a.pack->game_title),
							   QString::fromStdString(b.pack->game_title.empty() ? b.pack->name : b.pack->game_title)) < 0;
				});
				break;

			case Filter::Installed:
				break;
		}
	}

	rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& row) { return !rowMatchesSearch(row, needle); }), rows.end());
	m_rows = std::move(rows);

	// The rows keep the order they were built in until a header is clicked.
	QSignalBlocker block(m_ui.packList);
	m_ui.packList->setSortingEnabled(false);
	m_ui.packList->clear();

	QTreeWidgetItem* to_select = nullptr;
	for (size_t i = 0; i < m_rows.size(); i++)
	{
		const Row& row = m_rows[i];
		PackItem* item = new PackItem(m_ui.packList);
		item->setData(ColName, Qt::UserRole, static_cast<int>(i));

		if (row.pack)
		{
			const Tex::TexturePack& pack = *row.pack;
			item->setText(ColName, QString::fromStdString(pack.name));
			item->setText(ColGame, QString::fromStdString(pack.game_title.empty() ? (pack.serials.empty() ? std::string() : pack.serials.front()) : pack.game_title));
			// The Installed list shows what the pack takes on disk; the others, what it costs to download.
			const u64 shown_bytes = (m_filter == Filter::Installed && row.installed) ? row.installed->bytes : pack.size_bytes;
			item->setText(ColSize, (m_filter == Filter::Installed && row.installed) ? installedSizeText(shown_bytes) : QString::fromStdString(Tex::FormatPackSize(shown_bytes)));
			item->setData(ColSize, SIZE_ROLE, static_cast<qulonglong>(shown_bytes));
			if (pack.NeedsAstc() && m_astc == Engine::Unsupported)
			{
				item->setIcon(ColName, QIcon::fromTheme(QStringLiteral("warning-line")));
				item->setToolTip(ColName, tr("This GPU has no ASTC texture support, so this pack would have no effect."));
			}
		}
		else
		{
			item->setText(ColName, row.installed->record ? QString::fromStdString(row.installed->record->name) : tr("Texture folder %1").arg(QString::fromStdString(row.installed->serial)));
			item->setText(ColGame, QString::fromStdString(row.installed->serial));
			item->setText(ColSize, installedSizeText(row.installed->bytes));
			item->setData(ColSize, SIZE_ROLE, static_cast<qulonglong>(row.installed->bytes));
		}
		item->setText(ColStatus, statusTextFor(row));

		if (row.key() == m_selected_key)
			to_select = item;
	}

	m_ui.packList->setSortingEnabled(true);
	if (!to_select && m_ui.packList->topLevelItemCount() > 0)
		to_select = m_ui.packList->topLevelItem(0);
	if (to_select)
		m_ui.packList->setCurrentItem(to_select);
	block.unblock();

	QString empty;
	if (m_rows.empty())
	{
		using Status = TexturePackService::CatalogStatus;
		if (m_per_game && m_context_serial.empty())
			empty = tr("This game has no serial, so no texture packs can be matched to it.");
		else if (m_filter == Filter::Installed)
			empty = installed.loaded ? tr("No texture packs are installed.") : tr("Loading...");
		else if (catalog.status == Status::NotLoaded || catalog.status == Status::Loading)
			empty = tr("Loading catalog...");
		else if (catalog.status == Status::Unavailable)
			empty = tr("The catalog is not available.");
		else if (m_filter == Filter::Popular && !catalog.popular_loaded)
			empty = tr("Loading...");
		else if (m_filter == Filter::Popular && !catalog.popular)
			empty = tr("Can't reach the download counter right now.");
		else if (m_filter == Filter::Popular && catalog.popular->empty())
			empty = tr("Nothing has been downloaded yet today.");
		else if (m_per_game && needle.isEmpty())
			empty = tr("No texture packs are listed for this game yet.");
		else if (m_filter == Filter::MyGames && m_owned_serials.empty() && m_context_serial.empty())
			empty = tr("There are no games in your library yet.");
		else
			empty = tr("No packs match.");
	}
	m_ui.emptyLabel->setText(empty);
	m_ui.listStack->setCurrentIndex(m_rows.empty() ? 1 : 0);

	onSelectionChanged();
}

const TexturePacksSettingsWidget::Row* TexturePacksSettingsWidget::currentRow() const
{
	const QList<QTreeWidgetItem*> selected = m_ui.packList->selectedItems();
	if (selected.isEmpty())
		return nullptr;
	const int index = selected.front()->data(ColName, Qt::UserRole).toInt();
	return (index >= 0 && index < static_cast<int>(m_rows.size())) ? &m_rows[index] : nullptr;
}

void TexturePacksSettingsWidget::onFilterChanged()
{
	m_filter = static_cast<Filter>(std::clamp(m_ui.filterTabs->currentIndex(), 0, 3));

	// "My Games" reads the library as it is now.
	refreshContext();
	refreshList();
}

void TexturePacksSettingsWidget::onSelectionChanged()
{
	const Row* row = currentRow();
	if (row)
		m_selected_key = row->key();
	showDetail(row);
}

void TexturePacksSettingsWidget::showDetail(const Row* row)
{
	if (!row)
	{
		m_ui.detailHint->setText(m_rows.empty() ? QString() : tr("Select a pack to see its details."));
		m_ui.detailStack->setCurrentIndex(0);
		return;
	}
	m_ui.detailStack->setCurrentIndex(1);

	const bool busy = m_service->install().active;
	const auto set_row = [](QLabel* label, QLabel* value, const QString& text) {
		value->setText(text);
		const bool visible = !text.isEmpty();
		label->setVisible(visible);
		value->setVisible(visible);
	};

	if (!row->pack)
	{
		// A folder the catalog does not know: all we can say is what is there.
		const auto& folder = *row->installed;
		m_ui.detailTitle->setText(folder.record ? QString::fromStdString(folder.record->name) : tr("Texture folder %1").arg(QString::fromStdString(folder.serial)));
		m_ui.detailGame->setText(QString::fromStdString(folder.serial));
		m_ui.astcBadge->setVisible(false);
		set_row(m_ui.detailCreatorsLabel, m_ui.detailCreators, QString());
		set_row(m_ui.detailTypeLabel, m_ui.detailType, QString());
		set_row(m_ui.detailCompletenessLabel, m_ui.detailCompleteness, QString());
		set_row(m_ui.detailSizeLabel, m_ui.detailSize,
			tr("%1 files, %2").arg(folder.file_count).arg(installedSizeText(folder.bytes)));
		set_row(m_ui.detailSpaceLabel, m_ui.detailSpace, QString());
		set_row(m_ui.detailLicenseLabel, m_ui.detailLicense, QString());
		set_row(m_ui.detailSourceLabel, m_ui.detailSource, QString());
		set_row(m_ui.detailInstalledLabel, m_ui.detailInstalled, QString::fromStdString(folder.directory));
		m_ui.detailDescription->setText(tr("This folder was not installed from the catalog, or the catalog no longer lists the pack."));
		m_ui.detailDescription->setVisible(true);
		m_ui.detailCredits->setVisible(false);
		m_ui.installButton->setVisible(false);
		m_ui.deleteButton->setVisible(true);
		m_ui.deleteButton->setEnabled(!busy);
		return;
	}

	const Tex::TexturePack& pack = *row->pack;
	m_ui.detailTitle->setText(QString::fromStdString(pack.name));

	QStringList game;
	if (!pack.game_title.empty())
		game << QString::fromStdString(pack.game_title);
	QStringList serials;
	for (const std::string& serial : pack.serials)
		serials << QString::fromStdString(serial);
	game << serials.join(QStringLiteral(", "));
	m_ui.detailGame->setText(game.join(QStringLiteral("  -  ")));

	// ASTC: a warning when the GPU is known not to have it, else a plain note that it is needed.
	if (pack.NeedsAstc() && m_astc == Engine::Unsupported)
	{
		m_ui.astcBadge->setText(tr("This GPU cannot use these textures: it has no ASTC texture support. The pack would install, but the game would look the same."));
		m_ui.astcBadge->setVisible(true);
	}
	else
	{
		m_ui.astcBadge->setVisible(false);
	}

	// Who made it. An entry that says nobody has named the creator is not shown as an unknown name.
	QString creators;
	if (!pack.HasNamedCreators())
	{
		creators = QStringLiteral("<i>%1</i>").arg(tr("Creator not identified").toHtmlEscaped());
	}
	else
	{
		QStringList parts;
		for (const Tex::TexturePackCreator& creator : pack.creators)
			parts << htmlLink(creator.page, QString::fromStdString(creator.name));
		creators = parts.join(QStringLiteral(", "));
	}
	set_row(m_ui.detailCreatorsLabel, m_ui.detailCreators, creators);

	set_row(m_ui.detailTypeLabel, m_ui.detailType, typeName(pack.type));
	set_row(m_ui.detailCompletenessLabel, m_ui.detailCompleteness, completenessName(pack.completeness));

	QString size = tr("%1 download").arg(QString::fromStdString(Tex::FormatPackSize(pack.size_bytes)));
	if (pack.decompressed_size_bytes)
		size += tr(", %1 unpacked").arg(QString::fromStdString(Tex::FormatPackSize(pack.decompressed_size_bytes)));
	size += tr(", %n file(s)", nullptr, static_cast<int>(pack.file_count));
	set_row(m_ui.detailSizeLabel, m_ui.detailSize, size);

	const u64 needed = Tex::TexturePackInstaller::RequiredFreeSpace(pack);
	const std::optional<u64> available = freeSpaceAt(EmuFolders::Textures);
	QString space = QString::fromStdString(Tex::FormatPackSize(needed));
	if (available)
	{
		space += tr(" (%1 available)").arg(QString::fromStdString(Tex::FormatPackSize(*available)));
		if (*available < needed)
			space += tr(". Not enough free space.");
	}
	set_row(m_ui.detailSpaceLabel, m_ui.detailSpace, space);

	set_row(m_ui.detailLicenseLabel, m_ui.detailLicense, QString::fromStdString(pack.license));
	QString source;
	if (!pack.source_url.empty())
		source = htmlLink(pack.source_url, QUrl(QString::fromStdString(pack.source_url)).host());
	set_row(m_ui.detailSourceLabel, m_ui.detailSource, source);

	const Tex::InstallAction action = m_service->actionFor(pack);
	QString installed_text;
	if (row->installed && row->installed->record)
	{
		const Tex::TexturePackRecord& record = *row->installed->record;
		installed_text = (action == Tex::InstallAction::Update || action == Tex::InstallAction::Conflict) ?
		                     tr("Update available (installed version: %1)").arg(QString::fromStdString(record.version)) :
		                     tr("Version %1, in %2").arg(QString::fromStdString(record.version), QString::fromStdString(row->installed->serial));
	}
	set_row(m_ui.detailInstalledLabel, m_ui.detailInstalled, installed_text);

	m_ui.detailDescription->setText(QString::fromStdString(pack.description));
	m_ui.detailDescription->setVisible(!pack.description.empty());
	m_ui.detailCredits->setText(QString::fromStdString(pack.credits));
	m_ui.detailCredits->setVisible(!pack.credits.empty());

	// The note about ASTC for a pack that needs it, when the GPU has not been asked.
	if (pack.NeedsAstc() && m_astc == Engine::Unknown)
	{
		QString note = m_ui.detailDescription->text();
		if (!note.isEmpty())
			note += QStringLiteral("\n\n");
		note += tr("These textures are ASTC compressed, so they need a GPU with ASTC support. Apple, Qualcomm, Arm and Asahi GPUs have it; many PC GPUs do not. "
				   "ARMSX2 checks this while a game is running.");
		m_ui.detailDescription->setText(note);
		m_ui.detailDescription->setVisible(true);
	}

	m_ui.installButton->setVisible(true);
	switch (action)
	{
		case Tex::InstallAction::Install:
			m_ui.installButton->setText(tr("Download"));
			m_ui.installButton->setEnabled(!busy);
			break;
		case Tex::InstallAction::Update:
		case Tex::InstallAction::Conflict:
			m_ui.installButton->setText(tr("Update"));
			m_ui.installButton->setEnabled(!busy);
			break;
		case Tex::InstallAction::Installed:
			m_ui.installButton->setText(tr("Installed"));
			m_ui.installButton->setEnabled(false);
			break;
	}
	m_ui.deleteButton->setVisible(row->installed.has_value());
	m_ui.deleteButton->setEnabled(!busy);
}

void TexturePacksSettingsWidget::onInstallClicked()
{
	const Row* row = currentRow();
	if (!row || !row->pack)
		return;

	const std::string serial = installSerialFor(*row->pack);

	// An install over a replacements folder the installer did not make (textures the player made or copied)
	// keeps that folder under another name; ask first.
	Tex::TexturePackState state = Tex::TexturePackState::InDataRoot();
	const Tex::TexturePackInstaller installer(Tex::GetDefaultFetcher(), state, Tex::TexturePackInstaller::DefaultConfig());
	if (installer.HasUnrecordedReplacements(serial))
	{
		const QString message = tr("%1 already has texture replacements that were not installed from the catalog.\n\nThe existing folder is kept as %2<date and time> and no longer used. Install the pack?")
		                            .arg(QString::fromStdString(serial), Tex::TexturePackInstaller::KEPT_FOLDER_PREFIX);
		if (QMessageBox::question(this, tr("Install Texture Pack"), message) != QMessageBox::Yes)
			return;
	}

	if (!m_service->startInstall(*row->pack, serial))
		QMessageBox::information(this, tr("Texture Packs"), tr("Another texture pack is being installed. Wait for it to finish, or cancel it."));
	else
		m_install_started_here = true;
}

void TexturePacksSettingsWidget::onDeleteClicked()
{
	const Row* row = currentRow();
	if (!row || !row->installed)
		return;

	const QString name = row->pack ? QString::fromStdString(row->pack->name) :
	                                 (row->installed->record ? QString::fromStdString(row->installed->record->name) : QString::fromStdString(row->installed->serial));
	const QString message = tr("Delete the installed texture pack \"%1\"?\n\nThis removes %2 files (%3) from the textures folder. Texture dumps in the same folder are kept.")
	                            .arg(name)
	                            .arg(row->installed->file_count)
	                            .arg(installedSizeText(row->installed->bytes));
	if (QMessageBox::question(this, tr("Delete Texture Pack"), message) != QMessageBox::Yes)
		return;

	m_service->removeInstalled(row->installed->serial);
}

void TexturePacksSettingsWidget::showInstallStrip()
{
	const TexturePackService::InstallState& install = m_service->install();
	const TexturePackService::InstallOutcome& outcome = m_service->lastOutcome();

	if (!install.active && !outcome.valid)
	{
		m_ui.installStrip->setVisible(false);
		return;
	}
	m_ui.installStrip->setVisible(true);

	if (install.active)
	{
		using Phase = Tex::TexturePackInstaller::Phase;
		const Tex::TexturePackInstaller::Progress& progress = install.progress;

		QString text;
		bool determinate = false;
		switch (progress.phase)
		{
			case Phase::Starting:
				text = tr("Starting...");
				break;
			case Phase::Downloading:
				text = tr("Downloading %1 of %2").arg(progressSizeText(progress.done), progressSizeText(progress.total));
				determinate = progress.total > 0;
				break;
			case Phase::Verifying:
				text = tr("Verifying download...");
				break;
			case Phase::Extracting:
				text = tr("Extracting %1 / %2 files").arg(progress.done).arg(progress.total);
				determinate = progress.total > 0;
				break;
			case Phase::Installing:
				text = tr("Installing...");
				break;
		}
		if (install.cancelling)
			text = tr("Cancelling...");

		m_ui.installText->setText(QStringLiteral("%1: %2").arg(install.pack_name, text));
		m_ui.installProgress->setVisible(true);
		if (determinate)
		{
			m_ui.installProgress->setRange(0, 1000);
			m_ui.installProgress->setValue(static_cast<int>(progress.done * 1000 / progress.total));
		}
		else
		{
			m_ui.installProgress->setRange(0, 0);
		}

		// Once the commit has begun it runs to the end, so there is nothing left to cancel.
		m_ui.installCancel->setText(tr("Cancel"));
		m_ui.installCancel->setVisible(progress.phase != Phase::Installing);
		m_ui.installCancel->setEnabled(!install.cancelling);
		return;
	}

	m_ui.installProgress->setVisible(false);
	m_ui.installCancel->setText(tr("Dismiss"));
	m_ui.installCancel->setVisible(true);
	m_ui.installCancel->setEnabled(true);
	if (outcome.ok)
		m_ui.installText->setText(tr("Installed %1. Texture replacement is switched on; a running game reloads its textures now.").arg(outcome.pack_name));
	else if (outcome.cancelled)
		m_ui.installText->setText(tr("Cancelled."));
	else
		m_ui.installText->setText(tr("Could not install %1: %2").arg(outcome.pack_name, outcome.message));
}

void TexturePacksSettingsWidget::onInstallStripButtonClicked()
{
	if (m_service->install().active)
		m_service->cancelInstall();
	else
		m_service->clearOutcome();
}

#include "moc_TexturePacksSettingsWidget.cpp"

// Screenshot entries (see QtShot.h): the page in states a plain page open cannot reach.
#include "MainWindow.h"
#include "QtShot.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTabBar>

namespace
{
	QWidget* makeShotPage(int filter, const QString& search, Engine astc)
	{
		// A settings window of its own, never shown: the page only needs it for its help text and layer.
		SettingsWindow* window = new SettingsWindow();

		TexturePacksSettingsWidget* page = new TexturePacksSettingsWidget(window, nullptr);
		page->resize(760, 700);
		page->setAstcSupport(astc);
		page->findChild<QTabBar*>(QStringLiteral("filterTabs"))->setCurrentIndex(filter);
		page->findChild<QLineEdit*>(QStringLiteral("searchEdit"))->setText(search);
		return page;
	}
} // namespace

QTSHOT_DIALOG("texture-packs-no-astc", "Texture Packs page on a GPU without ASTC support, All filter, searching for a pack",
	[](const QtShot::Context&) { return makeShotPage(1, QStringLiteral("armageddon"), Engine::Unsupported); });
QTSHOT_DIALOG("texture-packs-search", "Texture Packs page, All filter, searching for a game",
	[](const QtShot::Context&) { return makeShotPage(1, QStringLiteral("burnout"), Engine::Unknown); });
QTSHOT_DIALOG("texture-packs-installed", "Texture Packs page, Installed filter, searching for a pack",
	[](const QtShot::Context&) { return makeShotPage(3, QStringLiteral("black"), Engine::Unknown); });
QTSHOT_DIALOG("texture-packs-demo", "Texture Packs page with the demo entry of a screenshot catalog selected",
	[](const QtShot::Context&) { return makeShotPage(1, QStringLiteral("demo: install"), Engine::Unknown); });
