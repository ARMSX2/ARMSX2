// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_TexturePacksSettingsWidget.h"

#include "SettingsWidget.h"

#include "Catalog/TexturePackEngine.h"
#include "Catalog/TexturePackInstaller.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class TexturePackService;

/// The Texture Packs page: browse the online catalog (Popular, All, My Games, Installed, with a
/// search box), see a pack's details, and download, update or delete it. In a game's settings window
/// it lists only the packs for that game.
///
/// The page is a view. The catalog, the installed packs and the running install live in
/// TexturePackService, so every Texture Packs page shows the same state and an install keeps going
/// when the window that started it is closed.
class TexturePacksSettingsWidget final : public SettingsWidget
{
	Q_OBJECT

public:
	TexturePacksSettingsWidget(SettingsWindow* dialog, QWidget* parent);
	~TexturePacksSettingsWidget() override;

	/// What the GPU said about ASTC (see Catalog::TexturePackEngine). A pack that needs ASTC gets a
	/// warning when the answer is Unsupported. "Unknown" (no game running) never replaces an answer.
	void setAstcSupport(Catalog::TexturePackEngine::AstcSupport support);

protected:
	void showEvent(QShowEvent* event) override;

private Q_SLOTS:
	void onCatalogChanged();
	void onInstalledChanged();
	void onInstallStateChanged();
	void onInstallFinished();
	void onFilterChanged();
	void onSelectionChanged();
	void onInstallClicked();
	void onDeleteClicked();
	void onInstallStripButtonClicked();
	void onRetryClicked();

private:
	enum class Filter
	{
		Popular,
		All,
		MyGames,
		Installed,
	};

	/// One line of the list: a catalog pack, an installed folder, or both. A folder with no catalog
	/// pack is a pack the catalog does not know (copied by hand, or its entry is gone).
	struct Row
	{
		const Catalog::TexturePack* pack = nullptr;
		std::optional<Catalog::TexturePackInstaller::InstalledPack> installed;

		std::string key() const;
	};

	void refreshContext();
	void refreshList();
	void refreshStatus();
	void showDetail(const Row* row);
	void showInstallStrip();
	bool rowMatchesSearch(const Row& row, const QString& needle) const;
	QString statusTextFor(const Row& row) const;
	const Catalog::TexturePack* packForId(const std::string& id) const;
	std::string installSerialFor(const Catalog::TexturePack& pack) const;
	const Row* currentRow() const;
	void queryAstcSupport();

	Ui::TexturePacksSettingsWidget m_ui;
	TexturePackService* m_service;

	Filter m_filter = Filter::Popular;
	bool m_first_show = true;
	bool m_install_was_active = false;
	bool m_install_started_here = false; ///< the install now running or just finished was started by this page

	/// The game the page is about: a game's own window, else the game that is running.
	std::string m_context_serial;
	bool m_per_game;
	std::unordered_set<std::string> m_owned_serials; ///< upper-case "SLUS-21410"

	std::shared_ptr<const std::vector<Catalog::TexturePack>> m_packs; ///< keeps the rows' pack pointers alive
	std::vector<Row> m_rows;
	std::string m_selected_key;

	Catalog::TexturePackEngine::AstcSupport m_astc = Catalog::TexturePackEngine::AstcSupport::Unknown;
};
