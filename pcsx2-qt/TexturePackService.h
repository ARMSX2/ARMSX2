// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Catalog/CancelToken.h"
#include "Catalog/TexturePackCatalog.h"
#include "Catalog/TexturePackInstaller.h"
#include "Catalog/TexturePackState.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QThreadPool>

#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/// The texture pack catalog, the installed packs and the one running install, for every Texture Packs
/// page in the process (the global settings window and each game's window show the same state).
///
/// Everything that blocks (the catalog and popularity requests, listing installed packs, an install,
/// a delete) runs on a pool thread of its own and reports back on the UI thread, where this object
/// lives; all the state below is read and written on the UI thread only. An install belongs to the
/// application, not to a page: closing a settings window does not stop it, and a page opened later
/// shows its progress.
class TexturePackService final : public QObject
{
	Q_OBJECT

public:
	static TexturePackService& instance();

	// ---- Catalog ---------------------------------------------------------------------------------

	enum class CatalogStatus
	{
		NotLoaded,
		Loading,
		Ready,
		Unavailable, ///< Nothing could be loaded and there is no saved copy.
	};

	struct CatalogData
	{
		CatalogStatus status = CatalogStatus::NotLoaded;
		/// The packs of the last good load; the pointers stay valid as long as this is held.
		std::shared_ptr<const std::vector<Catalog::TexturePack>> packs;
		Catalog::TexturePackCatalog::Source source = Catalog::TexturePackCatalog::Source::None;
		std::time_t fetched_at = 0;
		std::string error;

		/// The popularity counter: not loaded yet, unreachable (nullopt), or the ids, most popular first.
		bool popular_loaded = false;
		std::optional<std::vector<std::string>> popular;
	};

	const CatalogData& catalog() const { return m_catalog; }

	/// Loads the catalog and the popularity list. A saved copy under six hours old is used without
	/// asking the network unless `force_refresh`. Does nothing while a load is running.
	void loadCatalog(bool force_refresh);

	// ---- Installed packs ----------------------------------------------------------------------------

	struct InstalledData
	{
		bool loaded = false;
		std::vector<Catalog::TexturePackInstaller::InstalledPack> packs;
		std::map<std::string, Catalog::TexturePackRecord> records; ///< by pack id
	};

	const InstalledData& installed() const { return m_installed; }
	void refreshInstalled();

	/// What the catalog row of `pack` should offer, given the packs installed now.
	Catalog::InstallAction actionFor(const Catalog::TexturePack& pack) const;

	// ---- Install and delete ----------------------------------------------------------------------------

	struct InstallState
	{
		bool active = false;
		bool cancelling = false;
		std::string pack_id;
		QString pack_name;
		Catalog::TexturePackInstaller::Progress progress;
	};

	const InstallState& install() const { return m_install; }

	/// The last install's outcome, kept until the next one starts so a page opened later can show it.
	struct InstallOutcome
	{
		bool valid = false;
		bool ok = false;
		bool cancelled = false;
		QString pack_name;
		QString message; ///< The error, when it failed.
	};

	const InstallOutcome& lastOutcome() const { return m_outcome; }
	void clearOutcome();

	/// Starts installing `pack` into `serial`'s folder. False if an install is already running.
	bool startInstall(const Catalog::TexturePack& pack, const std::string& serial);
	void cancelInstall();

	/// Deletes the installed pack in the folder named `folder_name` (as InstalledPack::serial).
	void removeInstalled(const std::string& folder_name);

Q_SIGNALS:
	void catalogChanged();
	void installedChanged();
	void installStateChanged();
	void installFinished();
	void removeFinished(bool ok, const QString& folder_name);

private:
	TexturePackService();
	~TexturePackService() override;

	void loadPopular();
	void applyProgress();
	void finishInstall(const Catalog::TexturePackInstaller::InstallResult& result, const QString& pack_name);
	void shutdown();

	QThreadPool m_pool;

	CatalogData m_catalog;
	Catalog::CancelToken m_catalog_cancel;

	InstalledData m_installed;
	bool m_installed_refreshing = false;

	InstallState m_install;
	InstallOutcome m_outcome;
	Catalog::CancelToken m_install_cancel;

	// The install thread's latest report. At most one update is queued to the UI thread at a time.
	std::mutex m_progress_mutex;
	Catalog::TexturePackInstaller::Progress m_pending_progress;
	bool m_progress_queued = false;
};
