// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "TexturePackService.h"

#include "Catalog/CoverAutoDownload.h"
#include "Catalog/Fetch.h"
#include "Catalog/TexturePackEngine.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>

namespace
{
	using Catalog::TexturePackInstaller;

	/// The installer and its record, for one worker task. The record file and the size cache are
	/// shared by file, so every task builds its own.
	struct Tools
	{
		Catalog::TexturePackState state = Catalog::TexturePackState::InDataRoot();
		TexturePackInstaller installer{Catalog::GetDefaultFetcher(), state, TexturePackInstaller::DefaultConfig()};
	};
} // namespace

TexturePackService& TexturePackService::instance()
{
	// Created on first use, on the UI thread, as a child of the application so it is destroyed with it.
	static TexturePackService* s_instance = new TexturePackService();
	return *s_instance;
}

TexturePackService::TexturePackService()
	: QObject(QCoreApplication::instance())
{
	m_pool.setMaxThreadCount(4);

	// An install or a request in flight is cancelled and awaited before the application goes: its
	// results would be posted to an object that no longer exists.
	connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &TexturePackService::shutdown);
}

TexturePackService::~TexturePackService()
{
	shutdown();
}

void TexturePackService::shutdown()
{
	m_catalog_cancel.Cancel();
	m_install_cancel.Cancel();
	m_pool.waitForDone();
}

void TexturePackService::loadCatalog(bool force_refresh)
{
	if (m_catalog.status == CatalogStatus::Loading)
		return;

	m_catalog.status = CatalogStatus::Loading;
	m_catalog.popular_loaded = false;
	m_catalog.popular.reset();
	m_catalog_cancel = Catalog::CancelToken();
	emit catalogChanged();

	const Catalog::CancelToken cancel = m_catalog_cancel;
	m_pool.start([this, force_refresh, cancel]() {
		Catalog::TexturePackCatalog catalog(Catalog::GetDefaultFetcher(), Catalog::TexturePackCatalog::DefaultCache());
		Catalog::TexturePackCatalog::LoadResult result = catalog.Load(force_refresh, cancel);
		if (result.cancelled)
			return;

		QMetaObject::invokeMethod(this, [this, result = std::move(result)]() mutable {
			m_catalog.source = result.source;
			m_catalog.fetched_at = result.fetched_at;
			m_catalog.error = result.error;
			if (result.IsOk())
			{
				m_catalog.packs = std::make_shared<const std::vector<Catalog::TexturePack>>(std::move(result.packs));
				m_catalog.status = CatalogStatus::Ready;
			}
			else
			{
				m_catalog.packs.reset();
				m_catalog.status = CatalogStatus::Unavailable;
			}
			emit catalogChanged();
			loadPopular(); }, Qt::QueuedConnection);
	});
}

void TexturePackService::loadPopular()
{
	const Catalog::CancelToken cancel = m_catalog_cancel;
	m_pool.start([this, cancel]() {
		std::optional<std::vector<std::string>> popular = Catalog::FetchPopularPackIds(Catalog::GetDefaultFetcher(), cancel);
		if (cancel.IsCancelled())
			return;

		QMetaObject::invokeMethod(this, [this, popular = std::move(popular)]() mutable {
			m_catalog.popular = std::move(popular);
			m_catalog.popular_loaded = true;
			emit catalogChanged(); }, Qt::QueuedConnection);
	});
}

void TexturePackService::refreshInstalled()
{
	if (m_installed_refreshing)
		return;
	m_installed_refreshing = true;

	m_pool.start([this]() {
		Tools tools;
		// Also finishes deleting packs an earlier delete could not delete completely, and drops the
		// records of packs that are gone from disk.
		InstalledData data;
		data.packs = tools.installer.ListInstalled();
		data.records = tools.state.All();
		data.loaded = true;

		QMetaObject::invokeMethod(this, [this, data = std::move(data)]() mutable {
			m_installed = std::move(data);
			m_installed_refreshing = false;
			emit installedChanged(); }, Qt::QueuedConnection);
	});
}

Catalog::InstallAction TexturePackService::actionFor(const Catalog::TexturePack& pack) const
{
	const auto it = m_installed.records.find(pack.id);
	return Catalog::TexturePackState::ActionFor(it != m_installed.records.end() ? std::optional<Catalog::TexturePackRecord>(it->second) : std::nullopt, pack);
}

void TexturePackService::clearOutcome()
{
	if (!m_outcome.valid)
		return;
	m_outcome = InstallOutcome();
	emit installStateChanged();
}

bool TexturePackService::startInstall(const Catalog::TexturePack& pack, const std::string& serial)
{
	if (m_install.active || TexturePackInstaller::IsInstalling())
		return false;

	m_install = InstallState();
	m_install.active = true;
	m_install.pack_id = pack.id;
	m_install.pack_name = QString::fromStdString(pack.name);
	m_outcome = InstallOutcome();
	m_install_cancel = Catalog::CancelToken();
	{
		std::lock_guard<std::mutex> lock(m_progress_mutex);
		m_pending_progress = TexturePackInstaller::Progress();
		m_progress_queued = false;
	}
	emit installStateChanged();

	const Catalog::CancelToken cancel = m_install_cancel;
	m_pool.start([this, pack, serial, cancel]() {
		Tools tools;
		const TexturePackInstaller::InstallResult result = tools.installer.Install(
			pack, serial,
			[this](const TexturePackInstaller::Progress& progress) {
				{
					std::lock_guard<std::mutex> lock(m_progress_mutex);
					m_pending_progress = progress;
					if (m_progress_queued)
						return;
					m_progress_queued = true;
				}
				QMetaObject::invokeMethod(this, &TexturePackService::applyProgress, Qt::QueuedConnection);
			},
			cancel);

		if (result.IsOk())
		{
			// Texture replacement is off by default: without this the files sit on disk and the game
			// looks the same. It also makes a running game reload them.
			Catalog::TexturePackEngine::ActivateAfterInstall();

			// The popularity counter hears about an install only if the player allows online features.
			Catalog::PostInstallHit(Catalog::GetDefaultFetcher(), pack.id, Catalog::AutoCovers::IsEnabled());
		}

		QMetaObject::invokeMethod(this, [this, result, name = QString::fromStdString(pack.name)]() { finishInstall(result, name); }, Qt::QueuedConnection);
	});
	return true;
}

void TexturePackService::applyProgress()
{
	{
		std::lock_guard<std::mutex> lock(m_progress_mutex);
		m_install.progress = m_pending_progress;
		m_progress_queued = false;
	}
	if (m_install.active)
		emit installStateChanged();
}

void TexturePackService::cancelInstall()
{
	if (!m_install.active)
		return;
	m_install.cancelling = true;
	m_install_cancel.Cancel();
	emit installStateChanged();
}

void TexturePackService::finishInstall(const TexturePackInstaller::InstallResult& result, const QString& pack_name)
{
	m_install.active = false;
	m_install.cancelling = false;

	m_outcome.valid = true;
	m_outcome.ok = result.IsOk();
	m_outcome.cancelled = result.status == TexturePackInstaller::Status::Cancelled;
	m_outcome.pack_name = pack_name;
	m_outcome.message = QString::fromStdString(result.error);

	emit installStateChanged();
	emit installFinished();
	refreshInstalled();
}

void TexturePackService::removeInstalled(const std::string& folder_name)
{
	m_pool.start([this, folder_name]() {
		Tools tools;
		const bool removed = tools.installer.Remove(folder_name);
		if (removed)
			Catalog::TexturePackEngine::ReloadRunningGame();

		QMetaObject::invokeMethod(this, [this, removed, folder = QString::fromStdString(folder_name)]() {
			emit removeFinished(removed, folder);
			refreshInstalled(); }, Qt::QueuedConnection);
	});
}

#include "moc_TexturePackService.cpp"
