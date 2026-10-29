// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The seam between the texture-pack services and the running emulator: whether the GPU can use
// the packs, and making a freshly installed pack take effect. Everything else in Catalog knows
// nothing about the GS.

#include <functional>
#include <string>

class SettingsInterface;

namespace Catalog::TexturePackEngine
{
	enum class AstcSupport
	{
		Unknown, ///< No graphics device exists (no game is running), so nothing can be said.
		Supported,
		Unsupported, ///< The GPU cannot sample ASTC: tar+zstd packs would install and do nothing.
	};

	/// Whether the current GPU can use ASTC packs (GSDevice::Features().astc_textures, which the
	/// engine's replacement loader tests before it uses any .ktx or .astc file).
	/// Show a pack with TexturePack::NeedsAstc() with a warning when this says Unsupported;
	/// do not hide it.
	///
	/// The device belongs to the GS thread, so call this there, which is where Big Picture runs.
	/// It returns Unknown when no device exists.
	AstcSupport GetAstcSupportOnGSThread();

	/// The same from any thread. `callback` runs exactly once: on the CPU thread with Unknown if
	/// the VM is not running, otherwise on the GS thread with the answer. Marshal to your own
	/// thread inside it.
	void QueryAstcSupport(std::function<void(AstcSupport)> callback);

	/// Call after a successful install. Texture replacement defaults to off, so without this the
	/// files sit on disk and the game looks exactly the same.
	///
	///  * Turns EmuCore/GS LoadTextureReplacements on in the base settings and saves them if it
	///    was off. (A per-game setting that turns it off for one game still wins there.)
	///  * If a game is running: applies the setting, and reloads the replacement map and drops the
	///    texture cache on the GS thread so the new files are used without a restart.
	///
	/// Safe from any thread, and returns at once (the reload happens later). It takes the settings
	/// lock for a moment, so do not call it while holding Host::GetSettingsLock().
	void ActivateAfterInstall();

	/// For an install made from one game's own settings. A per-game LoadTextureReplacements = false beats the
	/// global setting ActivateAfterInstall() turns on, so the pack would do nothing for that game. If
	/// `game_settings` (the game's own settings layer) sets it to false, this sets it to true there and returns
	/// true; the caller saves the layer and reloads the game's settings. A layer that does not mention the
	/// setting follows the global one and is left alone (false is returned).
	bool EnableInGameLayer(SettingsInterface& game_settings);

	/// The same for a game whose settings file no frontend holds open: edits the file at `game_settings_path`
	/// and, if the edit changed the setting, reloads the settings of the running game so it takes effect
	/// there. Returns whether it changed the file. A frontend that has the file open in memory (Qt's per-game
	/// window) must use EnableInGameLayer() on its own copy instead, or its next save writes over this edit.
	/// Same threading as ActivateAfterInstall().
	bool EnableForGame(const std::string& game_settings_path);

	/// Re-indexes the replacement files of the running game (the "Reload Texture Replacements"
	/// hotkey's work), e.g. after a delete. Does nothing if no game is running or replacement is
	/// off. Same threading as ActivateAfterInstall().
	void ReloadRunningGame();
} // namespace Catalog::TexturePackEngine
