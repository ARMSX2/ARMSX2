// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/TexturePackEngine.h"

#include "Config.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "Host.h"
#include "MTGS.h"
#include "VMManager.h"

namespace Catalog::TexturePackEngine
{
	namespace
	{
		constexpr const char* SETTINGS_SECTION = "EmuCore/GS";
		constexpr const char* LOAD_KEY = "LoadTextureReplacements";

		// GS thread. What the "Reload Texture Replacements" hotkey does (GS.cpp).
		void ReloadOnGSThread()
		{
			if (!g_gs_renderer || !GSIsHardwareRenderer())
				return;
			GSTextureReplacements::ReloadReplacementMap();
			g_gs_renderer->PurgeTextureCache(true, false, true);
		}
	} // namespace

	AstcSupport GetAstcSupportOnGSThread()
	{
		if (!g_gs_device)
			return AstcSupport::Unknown;
		return g_gs_device->Features().astc_textures ? AstcSupport::Supported : AstcSupport::Unsupported;
	}

	void QueryAstcSupport(std::function<void(AstcSupport)> callback)
	{
		if (!callback)
			return;
		if (!VMManager::HasValidVM())
		{
			callback(AstcSupport::Unknown);
			return;
		}

		Host::RunOnCPUThread([callback = std::move(callback)]() {
			if (!MTGS::IsOpen())
			{
				callback(AstcSupport::Unknown);
				return;
			}
			MTGS::RunOnGSThread([callback]() { callback(GetAstcSupportOnGSThread()); });
		});
	}

	void ActivateAfterInstall()
	{
		// Each Host call takes the settings lock on its own; none is made while holding it.
		const bool was_on = Host::GetBaseBoolSettingValue(SETTINGS_SECTION, LOAD_KEY, false);
		if (!was_on)
		{
			Host::SetBaseBoolSettingValue(SETTINGS_SECTION, LOAD_KEY, true);
			Host::CommitBaseSettingChanges();
		}

		if (!VMManager::HasValidVM())
			return;

		Host::RunOnCPUThread([turned_on = !was_on]() {
			if (!MTGS::IsOpen())
				return;

			if (turned_on)
			{
				// Picks the setting up. When it flips the effective option from off to on, the GS
				// thread reloads the replacement map and drops the texture cache by itself.
				const bool before = EmuConfig.GS.LoadTextureReplacements;
				VMManager::ApplySettings();
				if (!before && EmuConfig.GS.LoadTextureReplacements)
					return;
			}
			MTGS::RunOnGSThread(&ReloadOnGSThread);
		});
	}

	void ReloadRunningGame()
	{
		if (!VMManager::HasValidVM())
			return;

		Host::RunOnCPUThread([]() {
			if (MTGS::IsOpen())
				MTGS::RunOnGSThread(&ReloadOnGSThread);
		});
	}
} // namespace Catalog::TexturePackEngine
