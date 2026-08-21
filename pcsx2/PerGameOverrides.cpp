// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Per-game settings precedence over the game database. Feature contributed by
// bmdhacks (PR #593).

#include "PerGameOverrides.h"

#include "GameDatabase.h"

#include "common/Assertions.h"
#include "common/SettingsInterface.h"

#include <array>
#include <cstring>

using GSHWFixId = GameDatabaseSchema::GSHWFixId;

static_assert(static_cast<u32>(GSHWFixId::Count) <= 64, "PerGameOverrides::gs_fixes has a bit per GS hardware fix");
static_assert(static_cast<u32>(GSUserHackOverride::MaxCount) <= 32, "GSOptions::UserHackOverrides has a bit per user hack");
static_assert(static_cast<u32>(SpeedHack::MaxCount) <= 32, "PerGameOverrides::speedhacks has a bit per speedhack");

namespace
{
	// One row per `EmuCore/GS` key the player can set that the game database also has
	// an opinion about.
	//
	// `fix` is what applyGSHardwareFixes() iterates and is the reason a row exists.
	// `hack` is the older, narrower vocabulary that MaskUserHacks() and the iOS
	// frontend already speak; most rows carry both, and the two that carry only a
	// hack are the texture-offset pair, which no database entry ever sets but which
	// MaskUserHacks() would otherwise strip out from under a player who set it.
	struct GSKeyRow
	{
		const char* key;
		GSHWFixId fix; // GSHWFixId::Count when the database never writes this one
		GSUserHackOverride hack; // GSUserHackOverride::MaxCount when it is not a user hack
	};

	constexpr GSKeyRow s_gs_keys[] = {
		{"UserHacks_align_sprite_X", GSHWFixId::AlignSprite, GSUserHackOverride::AlignSprite},
		{"UserHacks_merge_pp_sprite", GSHWFixId::MergeSprite, GSUserHackOverride::MergeSprite},
		{"UserHacks_round_sprite_offset", GSHWFixId::RoundSprite, GSUserHackOverride::RoundSprite},
		{"UserHacks_HalfPixelOffset", GSHWFixId::HalfPixelOffset, GSUserHackOverride::HalfPixelOffset},
		{"UserHacks_ForceEvenSpritePosition", GSHWFixId::ForceEvenSpritePosition, GSUserHackOverride::ForceEvenSpritePosition},
		{"UserHacks_native_scaling", GSHWFixId::NativeScaling, GSUserHackOverride::NativeScaling},
		{"UserHacks_NativePaletteDraw", GSHWFixId::NativePaletteDraw, GSUserHackOverride::NativePaletteDraw},
		{"UserHacks_BilinearHack", GSHWFixId::BilinearUpscale, GSUserHackOverride::BilinearHack},
		{"UserHacks_TCOffsetX", GSHWFixId::Count, GSUserHackOverride::TextureOffsetX},
		{"UserHacks_TCOffsetY", GSHWFixId::Count, GSUserHackOverride::TextureOffsetY},
		{"UserHacks_AutoFlushLevel", GSHWFixId::AutoFlush, GSUserHackOverride::AutoFlush},
		{"UserHacks_TextureInsideRt", GSHWFixId::TextureInsideRT, GSUserHackOverride::TextureInsideRt},
		{"preload_frame_with_gs_data", GSHWFixId::PreloadFrameData, GSUserHackOverride::PreloadFrameData},
		{"UserHacks_DisablePartialInvalidation", GSHWFixId::DisablePartialInvalidation, GSUserHackOverride::DisablePartialInvalidation},
		{"paltex", GSHWFixId::GPUPaletteConversion, GSUserHackOverride::GPUPaletteConversion},
		{"UserHacks_DisableDepthSupport", GSHWFixId::DisableDepthSupport, GSUserHackOverride::DisableDepthSupport},
		{"UserHacks_CPU_FB_Conversion", GSHWFixId::CPUFramebufferConversion, GSUserHackOverride::CPUFBConversion},
		{"UserHacks_ReadTCOnClose", GSHWFixId::FlushTCOnClose, GSUserHackOverride::ReadTCOnClose},
		{"UserHacks_Limit24BitDepth", GSHWFixId::Limit24BitDepth, GSUserHackOverride::Limit24BitDepth},
		{"UserHacks_EstimateTextureRegion", GSHWFixId::EstimateTextureRegion, GSUserHackOverride::EstimateTextureRegion},
		{"UserHacks_DrawBuffering", GSHWFixId::DrawBuffering, GSUserHackOverride::DrawBuffering},
		{"UserHacks_CPUSpriteRenderBW", GSHWFixId::CPUSpriteRenderBW, GSUserHackOverride::CPUSpriteRenderBW},
		{"UserHacks_CPUSpriteRenderLevel", GSHWFixId::CPUSpriteRenderLevel, GSUserHackOverride::CPUSpriteRenderLevel},
		{"UserHacks_CPUCLUTRender", GSHWFixId::CPUCLUTRender, GSUserHackOverride::CPUCLUTRender},
		{"UserHacks_GPUTargetCLUTMode", GSHWFixId::GPUTargetCLUT, GSUserHackOverride::GPUTargetCLUT},

		// Settings the database contends that were never part of the user-hack
		// vocabulary. They have no hack bit because MaskUserHacks() does not strip
		// them; a claim on these only has to reach applyGSHardwareFixes().
		{"hw_mipmap", GSHWFixId::Mipmap, GSUserHackOverride::MaxCount},
		{"HWAccurateAlphaTest", GSHWFixId::AccurateAlphaTest, GSUserHackOverride::MaxCount},
		{"pcrtc_offsets", GSHWFixId::PCRTCOffsets, GSUserHackOverride::MaxCount},
		{"pcrtc_overscan", GSHWFixId::PCRTCOverscan, GSUserHackOverride::MaxCount},
		{"CoalesceRenderPasses", GSHWFixId::CoalesceRenderPasses, GSUserHackOverride::MaxCount},
		{"TriFilter", GSHWFixId::TrilinearFiltering, GSUserHackOverride::MaxCount},
		{"UserHacks_SkipDraw_Start", GSHWFixId::SkipDrawStart, GSUserHackOverride::MaxCount},
		{"UserHacks_SkipDraw_End", GSHWFixId::SkipDrawEnd, GSUserHackOverride::MaxCount},
		{"texture_preloading", GSHWFixId::TexturePreloading, GSUserHackOverride::MaxCount},
		{"deinterlace_mode", GSHWFixId::Deinterlace, GSUserHackOverride::MaxCount},
		{"HWDownloadMode", GSHWFixId::HWDownloadMode, GSUserHackOverride::MaxCount},

		// One control, two database fixes: the database clamps the blend level from
		// both ends, so claiming the setting has to silence both clamps or the player
		// still gets moved.
		{"accurate_blending_unit", GSHWFixId::MinimumBlendingLevel, GSUserHackOverride::MaxCount},
		{"accurate_blending_unit", GSHWFixId::MaximumBlendingLevel, GSUserHackOverride::MaxCount},
	};

	// ⚠️ These are the settings keys, NOT GamefixOptions::GetGameFixName(), which
	// returns the game database's YAML spelling. The database calls the first one
	// "FpuMul" and the INI calls it "FpuMulHack". Order follows GamefixId.
	constexpr const char* s_gamefix_keys[GamefixId_COUNT] = {
		"FpuMulHack", // Fix_FpuMultiply
		"GoemonTlbHack", // Fix_GoemonTlbMiss
		"SoftwareRendererFMVHack", // Fix_SoftwareRendererFMV
		"SkipMPEGHack", // Fix_SkipMpeg
		"OPHFlagHack", // Fix_OPHFlag
		"EETimingHack", // Fix_EETiming
		"InstantDMAHack", // Fix_InstantDMA
		"DMABusyHack", // Fix_DMABusy
		"GIFFIFOHack", // Fix_GIFFIFO
		"VIFFIFOHack", // Fix_VIFFIFO
		"VIF1StallHack", // Fix_VIF1Stall
		"VuAddSubHack", // Fix_VuAddSub
		"IbitHack", // Fix_Ibit
		"VUSyncHack", // Fix_VUSync
		"VUOverflowHack", // Fix_VUOverflow
		"XgKickHack", // Fix_XGKick
		"BlitInternalFPSHack", // Fix_BlitInternalFPS
		"FullVU0SyncHack", // Fix_FullVU0Sync
	};

	constexpr const char* s_speedhack_keys[static_cast<u32>(SpeedHack::MaxCount)] = {
		"vuFlagHack", // SpeedHack::MVUFlag
		"vu1Instant", // SpeedHack::InstantVU1
		"vuThread", // SpeedHack::MTVU
		"EECycleRate", // SpeedHack::EECycleRate
	};
} // namespace

PerGameOverrideKeys::CoreKnobKeys PerGameOverrideKeys::ForCoreKnob(CoreGameDBKnob knob)
{
	// A clamp mode is one picker over several booleans, and the settings screens
	// write every one of them together and delete every one together — see
	// AdvancedSettingsWidget::setClampingMode and DrawClampingModeSetting. So any one
	// of them being present is an exact test for "the player set this mode".
	switch (knob)
	{
		case CoreGameDBKnob::EERoundMode:
			return {"EmuCore/CPU", {"FPU.Roundmode"}, 1};
		case CoreGameDBKnob::EEDivRoundMode:
			return {"EmuCore/CPU", {"FPUDiv.Roundmode"}, 1};
		case CoreGameDBKnob::VU0RoundMode:
			return {"EmuCore/CPU", {"VU0.Roundmode"}, 1};
		case CoreGameDBKnob::VU1RoundMode:
			return {"EmuCore/CPU", {"VU1.Roundmode"}, 1};
		case CoreGameDBKnob::EEClampMode:
			return {"EmuCore/CPU/Recompiler", {"fpuOverflow", "fpuExtraOverflow", "fpuFullMode", "fpuExactMode"}, 4};
		case CoreGameDBKnob::VU0ClampMode:
			return {"EmuCore/CPU/Recompiler", {"vu0Overflow", "vu0ExtraOverflow", "vu0SignOverflow"}, 3};
		case CoreGameDBKnob::VU1ClampMode:
			return {"EmuCore/CPU/Recompiler", {"vu1Overflow", "vu1ExtraOverflow", "vu1SignOverflow"}, 3};
		default:
			return {nullptr, {}, 0};
	}
}

const char* PerGameOverrideKeys::ForGamefix(GamefixId id)
{
	if (id < GamefixId_FIRST || id >= GamefixId_COUNT)
		return nullptr;

	return s_gamefix_keys[static_cast<size_t>(id)];
}

const char* PerGameOverrideKeys::ForSpeedHack(SpeedHack hack)
{
	if (hack >= SpeedHack::MaxCount)
		return nullptr;

	return s_speedhack_keys[static_cast<u32>(hack)];
}

const char* PerGameOverrideKeys::ForGSHWFix(GSHWFixId id)
{
	for (const GSKeyRow& row : s_gs_keys)
	{
		if (row.fix == id)
			return row.key;
	}

	return nullptr;
}

bool PerGameOverrideKeys::ClaimsAGameDBSetting(const char* section, const char* key)
{
	if (std::strcmp(section, "EmuCore/GS") == 0)
	{
		for (const GSKeyRow& row : s_gs_keys)
		{
			if (std::strcmp(row.key, key) == 0)
				return true;
		}

		return false;
	}

	if (std::strcmp(section, "EmuCore/Gamefixes") == 0)
	{
		for (const char* gamefix : s_gamefix_keys)
		{
			if (std::strcmp(gamefix, key) == 0)
				return true;
		}

		return false;
	}

	if (std::strcmp(section, "EmuCore/Speedhacks") == 0)
	{
		for (const char* speedhack : s_speedhack_keys)
		{
			if (std::strcmp(speedhack, key) == 0)
				return true;
		}

		return false;
	}

	for (u32 i = 0; i < static_cast<u32>(CoreGameDBKnob::MaxCount); i++)
	{
		// A knob added to the enum but not to the table lands here with no section.
		// The drift tests are what catch that; this only keeps it from being a crash.
		const CoreKnobKeys keys = ForCoreKnob(static_cast<CoreGameDBKnob>(i));
		if (!keys.section || std::strcmp(section, keys.section) != 0)
			continue;

		for (u32 k = 0; k < keys.count; k++)
		{
			if (std::strcmp(keys.keys[k], key) == 0)
				return true;
		}
	}

	return false;
}

PerGameOverrides ComputePerGameOverrides(const SettingsInterface& game_layer)
{
	PerGameOverrides ov;

	for (const GSKeyRow& row : s_gs_keys)
	{
		if (!game_layer.ContainsValue("EmuCore/GS", row.key))
			continue;

		if (row.fix != GSHWFixId::Count)
			ov.gs_fixes |= u64(1) << static_cast<u64>(row.fix);
		if (row.hack != GSUserHackOverride::MaxCount)
			ov.gs_hacks |= 1u << static_cast<u32>(row.hack);
	}

	for (u32 i = 0; i < static_cast<u32>(CoreGameDBKnob::MaxCount); i++)
	{
		const PerGameOverrideKeys::CoreKnobKeys keys = PerGameOverrideKeys::ForCoreKnob(static_cast<CoreGameDBKnob>(i));
		if (!keys.section)
			continue;

		for (u32 k = 0; k < keys.count; k++)
		{
			if (!game_layer.ContainsValue(keys.section, keys.keys[k]))
				continue;

			ov.core |= 1u << i;
			break;
		}
	}

	for (u32 i = 0; i < static_cast<u32>(SpeedHack::MaxCount); i++)
	{
		if (game_layer.ContainsValue("EmuCore/Speedhacks", s_speedhack_keys[i]))
			ov.speedhacks |= 1u << i;
	}

	for (u32 i = GamefixId_FIRST; i < GamefixId_COUNT; i++)
	{
		if (game_layer.ContainsValue("EmuCore/Gamefixes", s_gamefix_keys[i]))
			ov.gamefixes.set(i);
	}

	return ov;
}
