// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Big Picture rows for features brought over from the Android app. The other FullscreenUI files
// call in here with one line each (declarations at the end of FullscreenUI_Internal.h); anything
// longer than a row lives here, so an upstream rework of those pages touches these features in a
// handful of places.

#include "Achievements.h"
#include "Config.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSTexture.h"
#include "GS/GS.h"
#include "GameList.h"
#include "Host.h"
#include "INISettingsInterface.h"
#include "ImGui/FullscreenUI_Internal.h"
#include "ImGui/ImGuiFullscreen.h"

#include "common/SettingsInterface.h"

#include "IconsFontAwesome.h"

//////////////////////////////////////////////////////////////////////////
// Texture upscaler
//////////////////////////////////////////////////////////////////////////

void FullscreenUI::DrawTextureUpscaleSetting(SettingsInterface* bsi)
{
	// Labels as in the Qt widget (Graphics > Texture Replacement). Its help text is five sentences,
	// which does not fit here: MenuButtonWithValue reserves too little width for the value, so a
	// summary longer than one line is drawn under it. This is a one-line version of the help.
	// The values are GSTextureUpscaleMode: 0 Off, 1 RAISR 2x, 2 RAISR 4x.
	static constexpr const char* s_options[] = {
		FSUI_NSTR("Off (Default)"),
		FSUI_NSTR("RAISR 2x"),
		FSUI_NSTR("RAISR 4x"),
	};

	DrawIntListSetting(bsi, FSUI_ICONSTR(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Upscale Textures"),
		FSUI_CSTR("Upscales game textures on the CPU in the background using RAISR. Uses more memory and CPU. Hardware renderers only."),
		"EmuCore/GS", "TextureUpscale", static_cast<int>(GSTextureUpscaleMode::Off), s_options, std::size(s_options), true);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Translation String Area
// Same scheme as FullscreenUI.cpp and FullscreenUI_Settings.cpp: tools/generate_fullscreen_ui_translation_strings.py
// regenerates the block below from the FSUI_* macros above.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if 0
// TRANSLATION-STRING-AREA-BEGIN
TRANSLATE_NOOP("FullscreenUI", "Upscales game textures on the CPU in the background using RAISR. Uses more memory and CPU. Hardware renderers only.");
TRANSLATE_NOOP("FullscreenUI", "Off (Default)");
TRANSLATE_NOOP("FullscreenUI", "RAISR 2x");
TRANSLATE_NOOP("FullscreenUI", "RAISR 4x");
TRANSLATE_NOOP("FullscreenUI", "Upscale Textures");
// TRANSLATION-STRING-AREA-END
#endif
