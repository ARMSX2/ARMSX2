// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GameDatabase.h"
#include "GS/GS.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "vtlb.h"

#include "common/Console.h"
#include "common/EnumOps.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SettingsInterface.h"
#include "common/StringUtil.h"
#include "common/Timer.h"
#include "common/YAML.h"

#include <sstream>
#include "fmt/format.h"
#include "fmt/ranges.h"
#include <fstream>
#include <cstdio>
#include <mutex>
#include <optional>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace GameDatabaseSchema
{
	static const char* getHWFixName(GSHWFixId id);
	static std::optional<GSHWFixId> parseHWFixName(const std::string_view name);
	static bool isUserHackHWFix(GSHWFixId id);
} // namespace GameDatabaseSchema

namespace GameDatabase
{
	static void parseAndInsert(const std::string_view serial, const ryml::NodeRef& node);
	static void initDatabase();
} // namespace GameDatabase

static constexpr char GAMEDB_YAML_FILE_NAME[] = "GameIndex.yaml";

static std::unordered_map<std::string, GameDatabaseSchema::GameEntry> s_game_db;
static std::once_flag s_load_once_flag;

std::string GameDatabaseSchema::GameEntry::memcardFiltersAsString() const
{
	return fmt::to_string(fmt::join(memcardFilters, "/"));
}

const std::string* GameDatabaseSchema::GameEntry::findPatch(u32 crc) const
{
	if (crc == 0)
		return nullptr;

	auto it = patches.find(crc);
	if (it != patches.end())
		return &it->second;

	it = patches.find(0);
	if (it != patches.end())
		return &it->second;

	return nullptr;
}

const char* GameDatabaseSchema::GameEntry::compatAsString() const
{
	switch (compat)
	{
		case GameDatabaseSchema::Compatibility::Perfect:
			return "Perfect";
		case GameDatabaseSchema::Compatibility::Playable:
			return "Playable";
		case GameDatabaseSchema::Compatibility::InGame:
			return "In-Game";
		case GameDatabaseSchema::Compatibility::Menu:
			return "Menu";
		case GameDatabaseSchema::Compatibility::Intro:
			return "Intro";
		case GameDatabaseSchema::Compatibility::Nothing:
			return "Nothing";
		default:
			return "Unknown";
	}
}

void GameDatabase::parseAndInsert(const std::string_view serial, const ryml::NodeRef& node)
{
	GameDatabaseSchema::GameEntry gameEntry;
	if (node.has_child("name"))
	{
		node["name"] >> gameEntry.name;
	}
	if (node.has_child("name-sort"))
	{
		node["name-sort"] >> gameEntry.name_sort;
	}
	if (node.has_child("name-en"))
	{
		node["name-en"] >> gameEntry.name_en;
	}
	if (node.has_child("region"))
	{
		node["region"] >> gameEntry.region;
	}
	if (node.has_child("compat"))
	{
		int val = 0;
		node["compat"] >> val;
		gameEntry.compat = static_cast<GameDatabaseSchema::Compatibility>(val);
	}
	if (node.has_child("roundModes"))
	{
		if (node["roundModes"].has_child("eeRoundMode"))
		{
			int eeVal = -1;
			node["roundModes"]["eeRoundMode"] >> eeVal;
			if (eeVal >= 0 && eeVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.eeRoundMode = static_cast<FPRoundMode>(eeVal);
			else
				Console.Error(fmt::format("GameDB: Invalid EE round mode '{}', specified for serial: '{}'.", eeVal, serial));
		}
		if (node["roundModes"].has_child("eeDivRoundMode"))
		{
			int eeVal = -1;
			node["roundModes"]["eeDivRoundMode"] >> eeVal;
			if (eeVal >= 0 && eeVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.eeDivRoundMode = static_cast<FPRoundMode>(eeVal);
			else
				Console.Error(fmt::format("GameDB: Invalid EE division round mode '{}', specified for serial: '{}'.", eeVal, serial));
		}
		if (node["roundModes"].has_child("vuRoundMode"))
		{
			int vuVal = -1;
			node["roundModes"]["vuRoundMode"] >> vuVal;
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
			{
				gameEntry.vu0RoundMode = static_cast<FPRoundMode>(vuVal);
				gameEntry.vu1RoundMode = static_cast<FPRoundMode>(vuVal);
			}
			else
			{
				Console.Error(fmt::format("GameDB: Invalid VU round mode '{}', specified for serial: '{}'.", vuVal, serial));
			}
		}
		if (node["roundModes"].has_child("vu0RoundMode"))
		{
			int vuVal = -1;
			node["roundModes"]["vu0RoundMode"] >> vuVal;
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.vu0RoundMode = static_cast<FPRoundMode>(vuVal);
			else
				Console.Error(fmt::format("GameDB: Invalid VU0 round mode '{}', specified for serial: '{}'.", vuVal, serial));
		}
		if (node["roundModes"].has_child("vu1RoundMode"))
		{
			int vuVal = -1;
			node["roundModes"]["vu1RoundMode"] >> vuVal;
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.vu1RoundMode = static_cast<FPRoundMode>(vuVal);
			else
				Console.Error(fmt::format("GameDB: Invalid VU1 round mode '{}', specified for serial: '{}'.", vuVal, serial));
		}
	}
	if (node.has_child("clampModes"))
	{
		if (node["clampModes"].has_child("eeClampMode"))
		{
			int eeVal = -1;
			node["clampModes"]["eeClampMode"] >> eeVal;
			gameEntry.eeClampMode = static_cast<GameDatabaseSchema::ClampMode>(eeVal);
		}
		if (node["clampModes"].has_child("vuClampMode"))
		{
			int vuVal = -1;
			node["clampModes"]["vuClampMode"] >> vuVal;
			gameEntry.vu0ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
			gameEntry.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
		if (node["clampModes"].has_child("vu0ClampMode"))
		{
			int vuVal = -1;
			node["clampModes"]["vu0ClampMode"] >> vuVal;
			gameEntry.vu0ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
		if (node["clampModes"].has_child("vu1ClampMode"))
		{
			int vuVal = -1;
			node["clampModes"]["vu1ClampMode"] >> vuVal;
			gameEntry.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
	}

	// Validate game fixes, invalid ones will be dropped!
	if (node.has_child("gameFixes") && node["gameFixes"].has_children())
	{
		for (const auto& n : node["gameFixes"].children())
		{
			bool fixValidated = false;
			auto fix = std::string(n.val().str, n.val().len);

			// Enum values don't end with Hack, but gamedb does, so remove it before comparing.
			if (fix.ends_with("Hack"))
			{
				fix.erase(fix.size() - 4);
				for (GamefixId id = GamefixId_FIRST; id < GamefixId_COUNT; id = static_cast<GamefixId>(enum_cast(id) + 1))
				{
					if (fix.compare(Pcsx2Config::GamefixOptions::GetGameFixName(id)) == 0 &&
						std::find(gameEntry.gameFixes.begin(), gameEntry.gameFixes.end(), id) == gameEntry.gameFixes.end())
					{
						gameEntry.gameFixes.push_back(id);
						fixValidated = true;
						break;
					}
				}
			}

			if (!fixValidated)
			{
				Console.Error(fmt::format("GameDB: Invalid gamefix: '{}', specified for serial: '{}'. Dropping!", fix, serial));
			}
		}
	}

	if (node.has_child("speedHacks") && node["speedHacks"].has_children())
	{
		for (const auto& n : node["speedHacks"].children())
		{
			const std::string_view id_view = std::string_view(n.key().str, n.key().len);
			const std::string_view value_view = std::string_view(n.val().str, n.val().len);
			const std::optional<SpeedHack> id = Pcsx2Config::SpeedhackOptions::ParseSpeedHackName(id_view);
			const std::optional<int> value = StringUtil::FromChars<int>(value_view);

			if (id.has_value() && value.has_value() &&
				std::none_of(gameEntry.speedHacks.begin(), gameEntry.speedHacks.end(),
					[&id](const auto& it) { return it.first == id.value(); }))
			{
				gameEntry.speedHacks.emplace_back(id.value(), value.value());
			}
			else
			{
				Console.Error(fmt::format("GameDB: Invalid speedhack: '{}={}', specified for serial: '{}'. Dropping!",
					id_view, value_view, serial));
			}
		}
	}

	if (node.has_child("gsHWFixes"))
	{
		for (const auto& n : node["gsHWFixes"].children())
		{
			const std::string_view id_name(n.key().data(), n.key().size());
			std::optional<GameDatabaseSchema::GSHWFixId> id = GameDatabaseSchema::parseHWFixName(id_name);
			std::optional<s32> value;
			if (id.has_value() && (id.value() == GameDatabaseSchema::GSHWFixId::GetSkipCount ||
									  id.value() == GameDatabaseSchema::GSHWFixId::BeforeDraw ||
									  id.value() == GameDatabaseSchema::GSHWFixId::MoveHandler))
			{
				const std::string_view str_value(n.has_val() ? std::string_view(n.val().data(), n.val().size()) : std::string_view());
				if (id.value() == GameDatabaseSchema::GSHWFixId::GetSkipCount)
					value = GSLookupGetSkipCountFunctionId(str_value);
				else if (id.value() == GameDatabaseSchema::GSHWFixId::BeforeDraw)
					value = GSLookupBeforeDrawFunctionId(str_value);
				else if (id.value() == GameDatabaseSchema::GSHWFixId::MoveHandler)
					value = GSLookupMoveHandlerFunctionId(str_value);

				if (value.value_or(-1) < 0)
				{
					Console.Error(fmt::format("GameDB: Invalid GS HW Fix Value for '{}' in '{}': '{}'", id_name, serial, str_value));
					continue;
				}
			}
			else
			{
				value = n.has_val() ? StringUtil::FromChars<s32>(std::string_view(n.val().data(), n.val().size())) : 1;
			}
			if (!id.has_value() || !value.has_value())
			{
				Console.Error(fmt::format("GameDB: Invalid GS HW Fix: '{}' specified for serial '{}'. Dropping!", id_name, serial));
				continue;
			}

			gameEntry.gsHWFixes.emplace_back(id.value(), value.value());
		}
	}

	// Memory Card Filters - Store as a vector to allow flexibility in the future
	// - currently they are used as a '\n' delimited string in the app
	if (node.has_child("memcardFilters") && node["memcardFilters"].has_children())
	{
		for (const auto& n : node["memcardFilters"].children())
		{
			auto memcardFilter = std::string(n.val().str, n.val().len);
			gameEntry.memcardFilters.emplace_back(std::move(memcardFilter));
		}
	}

	// Game Patches
	if (node.has_child("patches") && node["patches"].has_children())
	{
		for (const auto& n : node["patches"].children())
		{
			// use a crc of 0 for default patches
			const std::string_view crc_str(n.key().str, n.key().len);
			const std::optional<u32> crc = (StringUtil::compareNoCase(crc_str, "default")) ? std::optional<u32>(0) : StringUtil::FromChars<u32>(crc_str, 16);
			if (!crc.has_value())
			{
				Console.Error(fmt::format("GameDB: Invalid CRC '{}' found for serial: '{}'. Skipping!", crc_str, serial));
				continue;
			}
			if (gameEntry.patches.find(crc.value()) != gameEntry.patches.end())
			{
				Console.Error(fmt::format("GameDB: Duplicate CRC '{}' found for serial: '{}'. Skipping, CRCs are case-insensitive!", crc_str, serial));
				continue;
			}

			std::string patch;
			if (n.has_child("content"))
				n["content"] >> patch;
			gameEntry.patches.emplace(crc.value(), std::move(patch));
		}
	}

	if (node.has_child("dynaPatches") && node["dynaPatches"].has_children())
	{
		for (const auto& n : node["dynaPatches"].children())
		{
			Patch::DynamicPatch patch;

			if (n.has_child("pattern") && n["pattern"].has_children())
			{
				for (const auto& db_pattern : n["pattern"].children())
				{
					Patch::DynamicPatchEntry entry;
					db_pattern["offset"] >> entry.offset;
					db_pattern["value"] >> entry.value;

					patch.pattern.push_back(entry);
				}
				for (const auto& db_replacement : n["replacement"].children())
				{
					Patch::DynamicPatchEntry entry;
					db_replacement["offset"] >> entry.offset;
					db_replacement["value"] >> entry.value;

					patch.replacement.push_back(entry);
				}
			}
			gameEntry.dynaPatches.push_back(patch);
		}
	}

	s_game_db.emplace(std::move(serial), std::move(gameEntry));
}

static const char* s_round_modes[static_cast<u32>(FPRoundMode::MaxCount)] = {
	"Nearest",
	"NegativeInfinity",
	"PositiveInfinity",
	"Chop"};

static const char* s_gs_hw_fix_names[] = {
	"autoFlush",
	"cpuFramebufferConversion",
	"readTCOnClose",
	"disableDepthSupport",
	"preloadFrameData",
	"disablePartialInvalidation",
	"textureInsideRT",
	"limit24BitDepth",
	"alignSprite",
	"mergeSprite",
	"mipmap",
	"accurateAlphaTest",
	"forceEvenSpritePosition",
	"bilinearUpscale",
	"nativePaletteDraw",
	"estimateTextureRegion",
	"drawBuffering",
	"PCRTCOffsets",
	"PCRTCOverscan",
	"trilinearFiltering",
	"skipDrawStart",
	"skipDrawEnd",
	"halfPixelOffset",
	"roundSprite",
	"nativeScaling",
	"texturePreloading",
	"deinterlace",
	"cpuSpriteRenderBW",
	"cpuSpriteRenderLevel",
	"cpuCLUTRender",
	"gpuTargetCLUT",
	"gpuPaletteConversion",
	"minimumBlendingLevel",
	"maximumBlendingLevel",
	"recommendedBlendingLevel",
	"recommendedAccurateAlphaTest",
	"recommendedHWAA1",
	"getSkipCount",
	"beforeDraw",
	"moveHandler",
};
static_assert(std::size(s_gs_hw_fix_names) == static_cast<u32>(GameDatabaseSchema::GSHWFixId::Count), "HW fix name lookup is correct size");

const char* GameDatabaseSchema::getHWFixName(GSHWFixId id)
{
	return s_gs_hw_fix_names[static_cast<u32>(id)];
}

static std::optional<GameDatabaseSchema::GSHWFixId> GameDatabaseSchema::parseHWFixName(const std::string_view name)
{
	for (u32 i = 0; i < std::size(s_gs_hw_fix_names); i++)
	{
		if (name.compare(s_gs_hw_fix_names[i]) == 0)
			return static_cast<GameDatabaseSchema::GSHWFixId>(i);
	}

	return std::nullopt;
}

bool GameDatabaseSchema::isUserHackHWFix(GSHWFixId id)
{
	switch (id)
	{
		case GSHWFixId::Deinterlace:
		case GSHWFixId::Mipmap:
		case GSHWFixId::TexturePreloading:
		case GSHWFixId::TrilinearFiltering:
		case GSHWFixId::MinimumBlendingLevel:
		case GSHWFixId::MaximumBlendingLevel:
		case GSHWFixId::RecommendedBlendingLevel:
		case GSHWFixId::PCRTCOffsets:
		case GSHWFixId::PCRTCOverscan:
		case GSHWFixId::GetSkipCount:
		case GSHWFixId::BeforeDraw:
		case GSHWFixId::MoveHandler:
			return false;
		default:
			return true;
	}
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
static bool IsIOSBurnoutRevengeGame(const std::string& name)
{
	return name.find("Burnout Revenge") != std::string::npos;
}

static bool IsIOSBurnout3Game(const std::string& name)
{
	return name.find("Burnout 3") != std::string::npos;
}

static bool IsIOSBurnoutMetalCallbackGame(const std::string& name)
{
	return IsIOSBurnoutRevengeGame(name) || IsIOSBurnout3Game(name);
}

static bool IsIOSBlackGame(const std::string& name)
{
	return name == "Black";
}

static bool IsIOSSonicUnleashedGame(const std::string& name)
{
	return name == "Sonic Unleashed";
}

// Midnight Club 3 (DUB Edition + DUB Edition Remix). Its GameDB entry uses the
// GSC_MidnightClub3 skip-count callback to drop broken bloom draws. The callback
// only ever *skips* draws (no extra barriers or copies), which makes it one of
// the safest callback classes for Metal — and without it the light blooms render
// wrongly on iOS while desktop PCSX2 looks correct (reported on SLUS-21355).
static bool IsIOSMidnightClub3Game(const std::string& name)
{
	return name.rfind("Midnight Club 3", 0) == 0;
}

static std::string GetIOSCompatibilityLabProfileForGSPolicy()
{
	SettingsInterface* si = Host::GetSettingsInterface();
	if (!si)
		return "unknown";

	std::string profile = si->GetStringValue("ARMSX2/JITBisect", "Profile", "off");
	if (!profile.empty() && !StringUtil::compareNoCase(profile, "off") && !StringUtil::compareNoCase(profile, "custom"))
		return profile;

	static constexpr const char* flag_keys[] = {
		"COP1EverythingOnly",
		"COP1EverythingPlusLoadStore",
		"COP1EverythingPlusMMI",
		"COP1EverythingPlusCOP2VU",
		"COP1EverythingPlusMultDiv",
		"COP1EverythingPlusShifts",
		"COP1EverythingPlusMoves",
		"COP1EverythingPlusIntegerALU",
		"COP1EverythingPlusBranches",
	};

	int active_flags = 0;
	for (const char* key : flag_keys)
		active_flags += si->GetBoolValue("ARMSX2/JITBisect", key, false) ? 1 : 0;

	if (active_flags == 0)
		return "off";

	return (active_flags == 1) ? "single-flag" : "custom";
}

static void ClearIOSMetalCompatLabOffGSHWFixes(Pcsx2Config::GSOptions& config)
{
	auto clear_int = [](auto& field, auto default_value, const char* name) {
		if (field != default_value)
		{
			Console.Warning("iOS Metal CompatLabOff: cleared manual GS HW fix %s", name);
			field = default_value;
		}
	};

#define CLEAR_IOS_METAL_BOOL(field, name) \
	do \
	{ \
		if (config.field) \
		{ \
			Console.Warning("iOS Metal CompatLabOff: cleared manual GS HW fix %s", name); \
			config.field = false; \
		} \
	} while (false)

	clear_int(config.UserHacks_AutoFlush, GSHWAutoFlushLevel::Disabled, "autoFlush");
	CLEAR_IOS_METAL_BOOL(UserHacks_CPUFBConversion, "cpuFramebufferConversion");
	CLEAR_IOS_METAL_BOOL(UserHacks_ReadTCOnClose, "readTCOnClose");
	CLEAR_IOS_METAL_BOOL(UserHacks_DisableDepthSupport, "disableDepthSupport");
	CLEAR_IOS_METAL_BOOL(PreloadFrameWithGSData, "preloadFrameData");
	CLEAR_IOS_METAL_BOOL(UserHacks_DisablePartialInvalidation, "disablePartialInvalidation");
	clear_int(config.UserHacks_TextureInsideRt, GSTextureInRtMode::Disabled, "textureInsideRT");
	CLEAR_IOS_METAL_BOOL(UserHacks_AlignSpriteX, "alignSprite");
	CLEAR_IOS_METAL_BOOL(UserHacks_MergePPSprite, "mergeSprite");
	CLEAR_IOS_METAL_BOOL(UserHacks_ForceEvenSpritePosition, "forceEvenSpritePosition");
	clear_int(config.UserHacks_BilinearHack, GSBilinearDirtyMode::Automatic, "bilinearUpscale");
	CLEAR_IOS_METAL_BOOL(UserHacks_NativePaletteDraw, "nativePaletteDraw");
	CLEAR_IOS_METAL_BOOL(UserHacks_EstimateTextureRegion, "estimateTextureRegion");
	clear_int(config.SkipDrawStart, 0, "skipDrawStart");
	clear_int(config.SkipDrawEnd, 0, "skipDrawEnd");
	clear_int(config.UserHacks_HalfPixelOffset, GSHalfPixelOffset::Off, "halfPixelOffset");
	clear_int(config.UserHacks_RoundSprite, static_cast<s8>(0), "roundSprite");
	clear_int(config.UserHacks_NativeScaling, GSNativeScaling::Off, "nativeScaling");
	clear_int(config.UserHacks_TCOffsetX, static_cast<s32>(0), "tcOffsetX");
	clear_int(config.UserHacks_TCOffsetY, static_cast<s32>(0), "tcOffsetY");
	clear_int(config.UserHacks_CPUSpriteRenderBW, static_cast<u8>(0), "cpuSpriteRenderBW");
	clear_int(config.UserHacks_CPUSpriteRenderLevel, static_cast<u8>(0), "cpuSpriteRenderLevel");
	clear_int(config.UserHacks_CPUCLUTRender, static_cast<u8>(0), "cpuCLUTRender");
	clear_int(config.UserHacks_GPUTargetCLUTMode, GSGPUTargetCLUTMode::Disabled, "gpuTargetCLUT");
	clear_int(config.GetSkipCountFunctionId, static_cast<s16>(-1), "getSkipCount");
	clear_int(config.BeforeDrawFunctionId, static_cast<s16>(-1), "beforeDraw");
	clear_int(config.MoveHandlerFunctionId, static_cast<s16>(-1), "moveHandler");
	config.ManualUserHacks = false;

#undef CLEAR_IOS_METAL_BOOL
}

static bool ShouldBlockIOSMetalGSHardwareFixes(const Pcsx2Config::GSOptions& config)
{
	if (config.Renderer != GSRendererType::Metal)
		return false;

	// ARMSX2 iOS exposes GameDB Core Fixes and Graphics Fixes separately. When either
	// compatibility path is off, do not allow GameDB/manual hardware hacks to survive
	// the ELF CRC settings reload and break strict Metal rendering.
	const std::string compat_profile = GetIOSCompatibilityLabProfileForGSPolicy();
	return StringUtil::compareNoCase(compat_profile, "off") || !EmuConfig.EnableGameFixes || config.ManualUserHacks;
}

static bool IsIOSMetalHighRiskAutoGSHWFix(GameDatabaseSchema::GSHWFixId id)
{
	switch (id)
	{
		case GameDatabaseSchema::GSHWFixId::GetSkipCount:
		case GameDatabaseSchema::GSHWFixId::BeforeDraw:
		case GameDatabaseSchema::GSHWFixId::MoveHandler:
			return true;
		default:
			return false;
	}
}

static bool IsIOSMetalAllowedAutoGSHWCallback(const GameDatabaseSchema::GameEntry& entry, GameDatabaseSchema::GSHWFixId id, int value)
{
	switch (id)
	{
		case GameDatabaseSchema::GSHWFixId::GetSkipCount:
		{
			static const s16 burnout_games = GSLookupGetSkipCountFunctionId("GSC_BurnoutGames");
			static const s16 burnout_sky = GSLookupGetSkipCountFunctionId("GSC_BlackAndBurnoutSky");
			static const s16 midnight_club3 = GSLookupGetSkipCountFunctionId("GSC_MidnightClub3");
			return (IsIOSBurnoutMetalCallbackGame(entry.name) && value == burnout_games) ||
				(IsIOSBurnoutMetalCallbackGame(entry.name) && value == burnout_sky) ||
				(IsIOSMidnightClub3Game(entry.name) && value == midnight_club3);
		}

		case GameDatabaseSchema::GSHWFixId::BeforeDraw:
		{
			static const s16 burnout_games = GSLookupBeforeDrawFunctionId("OI_BurnoutGames");
			static const s16 sonic_unleashed = GSLookupBeforeDrawFunctionId("OI_SonicUnleashed");
			return ((IsIOSBurnoutMetalCallbackGame(entry.name) || IsIOSBlackGame(entry.name)) && value == burnout_games) ||
				(IsIOSSonicUnleashedGame(entry.name) && value == sonic_unleashed);
		}

		default:
			return false;
	}
}

static bool IsIOSMetalAllowedCompatLabOffGSHWFix(const GameDatabaseSchema::GameEntry& entry, GameDatabaseSchema::GSHWFixId id, int value)
{
	if (!IsIOSMetalHighRiskAutoGSHWFix(id))
		return true;

	switch (id)
	{
		case GameDatabaseSchema::GSHWFixId::GetSkipCount:
		{
			static const s16 burnout_games = GSLookupGetSkipCountFunctionId("GSC_BurnoutGames");
			static const s16 burnout_sky = GSLookupGetSkipCountFunctionId("GSC_BlackAndBurnoutSky");
			static const s16 midnight_club3 = GSLookupGetSkipCountFunctionId("GSC_MidnightClub3");
			return (IsIOSBurnoutMetalCallbackGame(entry.name) && value == burnout_games) ||
				(IsIOSBurnoutMetalCallbackGame(entry.name) && value == burnout_sky) ||
				(IsIOSMidnightClub3Game(entry.name) && value == midnight_club3);
		}

		case GameDatabaseSchema::GSHWFixId::BeforeDraw:
		{
			static const s16 burnout_games = GSLookupBeforeDrawFunctionId("OI_BurnoutGames");
			static const s16 sonic_unleashed = GSLookupBeforeDrawFunctionId("OI_SonicUnleashed");
			return ((IsIOSBurnoutMetalCallbackGame(entry.name) || IsIOSBlackGame(entry.name)) && value == burnout_games) ||
				(IsIOSSonicUnleashedGame(entry.name) && value == sonic_unleashed);
		}

		default:
			return false;
	}
}

static void ClearIOSMetalHighRiskCallbacks(Pcsx2Config::GSOptions& config, const char* reason)
{
	auto clear_callback = [reason](auto& field, auto default_value, const char* name) {
		if (field != default_value)
		{
			Console.Warning("@@IOS_METAL_GS_CALLBACK_CLEAR@@ fix=%s old=%d reason=%s",
				name, static_cast<int>(field), reason);
			field = default_value;
		}
	};

	clear_callback(config.GetSkipCountFunctionId, static_cast<s16>(-1), "getSkipCount");
	clear_callback(config.BeforeDrawFunctionId, static_cast<s16>(-1), "beforeDraw");
	clear_callback(config.MoveHandlerFunctionId, static_cast<s16>(-1), "moveHandler");
}

static const char* IOSBool(bool value)
{
	return value ? "on" : "off";
}

static std::string FormatIOSGameFixList(const GameDatabaseSchema::GameEntry& entry)
{
	std::string out;
	for (const GamefixId fix : entry.gameFixes)
	{
		fmt::format_to(std::back_inserter(out), "{}{}",
			out.empty() ? "" : ",", Pcsx2Config::GamefixOptions::GetGameFixName(fix));
	}
	return out.empty() ? "none" : out;
}

static std::string FormatIOSSpeedHackList(const GameDatabaseSchema::GameEntry& entry)
{
	std::string out;
	for (const auto& [hack, value] : entry.speedHacks)
	{
		fmt::format_to(std::back_inserter(out), "{}{}={}",
			out.empty() ? "" : ",", Pcsx2Config::SpeedhackOptions::GetSpeedHackName(hack), value);
	}
	return out.empty() ? "none" : out;
}

static std::string FormatIOSGSHWFixList(const GameDatabaseSchema::GameEntry& entry)
{
	std::string out;
	for (const auto& [id, value] : entry.gsHWFixes)
	{
		fmt::format_to(std::back_inserter(out), "{}{}={}",
			out.empty() ? "" : ",", GameDatabaseSchema::getHWFixName(id), value);
	}
	return out.empty() ? "none" : out;
}

static void LogIOSGameFixSnapshot(
	const GameDatabaseSchema::GameEntry& entry, const Pcsx2Config::GSOptions& gs_config, const char* stage)
{
#ifdef PCSX2_ARM64_DYNAREC
	const int use_arm64_dynarec = EmuConfig.Cpu.UseArm64Dynarec ? 1 : 0;
#else
	const int use_arm64_dynarec = -1;
#endif

	Console.WriteLn("@@IOS_GAMEFIX_SNAPSHOT@@ stage=%s game=\"%s\" region=\"%s\" compat=\"%s\" renderer=%s gamedb_entries=%zu",
		stage, entry.name.c_str(), entry.region.c_str(), entry.compatAsString(),
		Pcsx2Config::GSOptions::GetRendererName(gs_config.Renderer), GameDatabase::entryCount());
	Console.WriteLn("@@IOS_GAMEFIX_CPU@@ UseArm64Dynarec=%d EE=%s IOP=%s VU0=%s VU1=%s Fastmem=%s EECache=%s EERound=%u EEDivRound=%u VU0Round=%u VU1Round=%u EEClamp=%u VUClamp=%u",
		use_arm64_dynarec, IOSBool(EmuConfig.Cpu.Recompiler.EnableEE),
		IOSBool(EmuConfig.Cpu.Recompiler.EnableIOP), IOSBool(EmuConfig.Cpu.Recompiler.EnableVU0),
		IOSBool(EmuConfig.Cpu.Recompiler.EnableVU1), IOSBool(EmuConfig.Cpu.Recompiler.EnableFastmem),
		IOSBool(EmuConfig.Cpu.Recompiler.EnableEECache), static_cast<unsigned>(EmuConfig.Cpu.FPUFPCR.GetRoundMode()),
		static_cast<unsigned>(EmuConfig.Cpu.FPUDivFPCR.GetRoundMode()), static_cast<unsigned>(EmuConfig.Cpu.VU0FPCR.GetRoundMode()),
		static_cast<unsigned>(EmuConfig.Cpu.VU1FPCR.GetRoundMode()), EmuConfig.Cpu.Recompiler.GetEEClampMode(),
		EmuConfig.Cpu.Recompiler.GetVUClampMode());
	Console.WriteLn("@@IOS_GAMEFIX_SPEED@@ nominal=%.3f turbo=%.3f slomo=%.3f ntsc=%.3f pal=%.3f mtvu=%s mvuFlag=%s instantVU1=%s eeCycleRate=%d eeCycleSkip=%u vsyncQueue=%d",
		EmuConfig.EmulationSpeed.NominalScalar, EmuConfig.EmulationSpeed.TurboScalar, EmuConfig.EmulationSpeed.SlomoScalar,
		gs_config.FramerateNTSC, gs_config.FrameratePAL, IOSBool(EmuConfig.Speedhacks.vuThread),
		IOSBool(EmuConfig.Speedhacks.vuFlagHack), IOSBool(EmuConfig.Speedhacks.vu1Instant),
		static_cast<int>(EmuConfig.Speedhacks.EECycleRate), static_cast<unsigned>(EmuConfig.Speedhacks.EECycleSkip),
		gs_config.VsyncQueueSize);
	Console.WriteLn("@@IOS_GAMEFIX_PATCHES@@ GameFixes=%s PNACH=%s Cheats=%s Widescreen=%s NoInterlace=%s RetroAchievements=%s",
		IOSBool(EmuConfig.EnableGameFixes), IOSBool(EmuConfig.EnablePatches), IOSBool(EmuConfig.EnableCheats),
		IOSBool(EmuConfig.EnableWideScreenPatches), IOSBool(EmuConfig.EnableNoInterlacingPatches),
		IOSBool(EmuConfig.Achievements.Enabled));
	Console.WriteLn("@@IOS_GAMEFIX_GS@@ upscale=%.2f manualUserHacks=%s autoFlush=%d textureInsideRT=%d halfPixelOffset=%d nativeScaling=%d alignSprite=%s cpuSpriteBW=%u cpuSpriteLevel=%u cpuCLUT=%u gpuTargetCLUT=%d skipStart=%d skipEnd=%d getSkipCount=%d beforeDraw=%d moveHandler=%d texLoad=%s texDump=%s",
		gs_config.UpscaleMultiplier, IOSBool(gs_config.ManualUserHacks), static_cast<int>(gs_config.UserHacks_AutoFlush),
		static_cast<int>(gs_config.UserHacks_TextureInsideRt), static_cast<int>(gs_config.UserHacks_HalfPixelOffset),
		static_cast<int>(gs_config.UserHacks_NativeScaling), IOSBool(gs_config.UserHacks_AlignSpriteX),
		static_cast<unsigned>(gs_config.UserHacks_CPUSpriteRenderBW), static_cast<unsigned>(gs_config.UserHacks_CPUSpriteRenderLevel),
		static_cast<unsigned>(gs_config.UserHacks_CPUCLUTRender), static_cast<int>(gs_config.UserHacks_GPUTargetCLUTMode),
		gs_config.SkipDrawStart, gs_config.SkipDrawEnd, static_cast<int>(gs_config.GetSkipCountFunctionId),
		static_cast<int>(gs_config.BeforeDrawFunctionId), static_cast<int>(gs_config.MoveHandlerFunctionId),
		IOSBool(gs_config.LoadTextureReplacements), IOSBool(gs_config.DumpReplaceableTextures));
	Console.WriteLn("@@IOS_GAMEDB_REQUESTS@@ gamefixes=\"%s\" speedhacks=\"%s\" gs_hw=\"%s\" patches=%zu dyna_patches=%zu",
		FormatIOSGameFixList(entry).c_str(), FormatIOSSpeedHackList(entry).c_str(),
		FormatIOSGSHWFixList(entry).c_str(), entry.patches.size(), entry.dynaPatches.size());

	const std::string gamefixes = FormatIOSGameFixList(entry);
	const std::string speedhacks = FormatIOSSpeedHackList(entry);
	const std::string gs_hw_fixes = FormatIOSGSHWFixList(entry);
	std::fprintf(stderr,
		"@@IOS_GAMEFIX_SNAPSHOT_STDERR@@ stage=%s game=\"%s\" region=\"%s\" compat=\"%s\" renderer=%s gamedb_entries=%zu enableGameFixes=%d manualUserHacks=%d upscale=%.2f\n",
		stage, entry.name.c_str(), entry.region.c_str(), entry.compatAsString(),
		Pcsx2Config::GSOptions::GetRendererName(gs_config.Renderer), GameDatabase::entryCount(),
		EmuConfig.EnableGameFixes ? 1 : 0, gs_config.ManualUserHacks ? 1 : 0, gs_config.UpscaleMultiplier);
	std::fprintf(stderr,
		"@@IOS_GAMEFIX_GS_STDERR@@ autoFlush=%d textureInsideRT=%d halfPixelOffset=%d nativeScaling=%d alignSprite=%d cpuSpriteBW=%u cpuSpriteLevel=%u cpuCLUT=%u gpuTargetCLUT=%d skipStart=%d skipEnd=%d getSkipCount=%d beforeDraw=%d moveHandler=%d\n",
		static_cast<int>(gs_config.UserHacks_AutoFlush), static_cast<int>(gs_config.UserHacks_TextureInsideRt),
		static_cast<int>(gs_config.UserHacks_HalfPixelOffset), static_cast<int>(gs_config.UserHacks_NativeScaling),
		gs_config.UserHacks_AlignSpriteX ? 1 : 0, static_cast<unsigned>(gs_config.UserHacks_CPUSpriteRenderBW),
		static_cast<unsigned>(gs_config.UserHacks_CPUSpriteRenderLevel), static_cast<unsigned>(gs_config.UserHacks_CPUCLUTRender),
		static_cast<int>(gs_config.UserHacks_GPUTargetCLUTMode), gs_config.SkipDrawStart, gs_config.SkipDrawEnd,
		static_cast<int>(gs_config.GetSkipCountFunctionId), static_cast<int>(gs_config.BeforeDrawFunctionId),
		static_cast<int>(gs_config.MoveHandlerFunctionId));
	std::fprintf(stderr,
		"@@IOS_GAMEDB_REQUESTS_STDERR@@ gamefixes=\"%s\" speedhacks=\"%s\" gs_hw=\"%s\" patches=%zu dyna_patches=%zu\n",
		gamefixes.c_str(), speedhacks.c_str(), gs_hw_fixes.c_str(), entry.patches.size(), entry.dynaPatches.size());
	std::fflush(stderr);
}
#endif

void GameDatabaseSchema::GameEntry::applyGameFixes(Pcsx2Config& config, bool applyAuto) const
{
	// Only apply core game fixes if the user has enabled them.
	if (!applyAuto)
		Console.Warning("GameDB: Game Fixes are disabled");

	if (eeRoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			Console.WriteLn("GameDB: Changing EE/FPU roundmode to %d [%s]", eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
			config.Cpu.FPUFPCR.SetRoundMode(eeRoundMode);
		}
		else
		{
			Console.Warning("GameDB: Skipping changing EE/FPU roundmode to %d [%s]", eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
		}
	}

	if (eeDivRoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			Console.WriteLn("GameDB: Changing EE/FPU divison roundmode to %d [%s]", eeRoundMode, s_round_modes[static_cast<u8>(eeDivRoundMode)]);
			config.Cpu.FPUDivFPCR.SetRoundMode(eeDivRoundMode);
		}
		else
		{
			Console.Warning("GameDB: Skipping changing EE/FPU roundmode to %d [%s]", eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
		}
	}

	if (vu0RoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			Console.WriteLn("GameDB: Changing VU0 roundmode to %d [%s]", vu0RoundMode, s_round_modes[static_cast<u8>(vu0RoundMode)]);
			config.Cpu.VU0FPCR.SetRoundMode(vu0RoundMode);
		}
		else
		{
			Console.Warning("GameDB: Skipping changing VU0 roundmode to %d [%s]", vu0RoundMode, s_round_modes[static_cast<u8>(vu0RoundMode)]);
		}
	}

	if (vu1RoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			Console.WriteLn("GameDB: Changing VU1 roundmode to %d [%s]", vu1RoundMode, s_round_modes[static_cast<u8>(vu1RoundMode)]);
			config.Cpu.VU1FPCR.SetRoundMode(vu1RoundMode);
		}
		else
		{
			Console.Warning("GameDB: Skipping changing VU1 roundmode to %d [%s]", vu1RoundMode, s_round_modes[static_cast<u8>(vu1RoundMode)]);
		}
	}

	if (eeClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(eeClampMode);
		if (applyAuto)
		{
			Console.WriteLn("GameDB: Changing EE/FPU clamp mode [mode=%d]", clampMode);
			config.Cpu.Recompiler.fpuOverflow = (clampMode >= 1);
			config.Cpu.Recompiler.fpuExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.fpuFullMode = (clampMode >= 3);
		}
		else
			Console.Warning("GameDB: Skipping changing EE/FPU clamp mode [mode=%d]", clampMode);
	}

	if (vu0ClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(vu0ClampMode);
		if (applyAuto)
		{
			Console.WriteLn("GameDB: Changing VU0 clamp mode [mode=%d]", clampMode);
			config.Cpu.Recompiler.vu0Overflow = (clampMode >= 1);
			config.Cpu.Recompiler.vu0ExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.vu0SignOverflow = (clampMode >= 3);
		}
		else
			Console.Warning("GameDB: Skipping changing VU0 clamp mode [mode=%d]", clampMode);
	}

	if (vu1ClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(vu1ClampMode);
		if (applyAuto)
		{
			Console.WriteLn("GameDB: Changing VU1 clamp mode [mode=%d]", clampMode);
			config.Cpu.Recompiler.vu1Overflow = (clampMode >= 1);
			config.Cpu.Recompiler.vu1ExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.vu1SignOverflow = (clampMode >= 3);
		}
		else
			Console.Warning("GameDB: Skipping changing VU1 clamp mode [mode=%d]", clampMode);
	}

	// TODO - config - this could be simplified with maps instead of bitfields and enums
	for (const auto& it : speedHacks)
	{
		if (!applyAuto)
		{
			Console.Warning("GameDB: Skipping setting Speedhack '%s' to [mode=%d]",
				Pcsx2Config::SpeedhackOptions::GetSpeedHackName(it.first), it.second);
			continue;
		}
		// Legacy note - speedhacks are setup in the GameDB as integer values, but
		// are effectively booleans like the gamefixes
		config.Speedhacks.Set(it.first, it.second);
		Console.WriteLn("GameDB: Setting Speedhack '%s' to [mode=%d]",
			Pcsx2Config::SpeedhackOptions::GetSpeedHackName(it.first), it.second);
	}

	// TODO - config - this could be simplified with maps instead of bitfields and enums
	for (const GamefixId id : gameFixes)
	{
		if (!applyAuto)
		{
			Console.Warning("GameDB: Skipping Gamefix: %s", Pcsx2Config::GamefixOptions::GetGameFixName(id));
			continue;
		}
		// if the fix is present, it is said to be enabled
		config.Gamefixes.Set(id, true);
		Console.WriteLn("GameDB: Enabled Gamefix: %s", Pcsx2Config::GamefixOptions::GetGameFixName(id));

		// The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
		if (id == Fix_GoemonTlbMiss && true)
			vtlb_Alloc_Ppmap();
	}
}

bool GameDatabaseSchema::GameEntry::configMatchesHWFix(const Pcsx2Config::GSOptions& config, GSHWFixId id, int value)
{
	switch (id)
	{
		case GSHWFixId::AutoFlush:
			return (static_cast<int>(config.UserHacks_AutoFlush) == value);

		case GSHWFixId::CPUFramebufferConversion:
			return (static_cast<int>(config.UserHacks_CPUFBConversion) == value);

		case GSHWFixId::FlushTCOnClose:
			return (static_cast<int>(config.UserHacks_ReadTCOnClose) == value);

		case GSHWFixId::DisableDepthSupport:
			return (static_cast<int>(config.UserHacks_DisableDepthSupport) == value);

		case GSHWFixId::PreloadFrameData:
			return (static_cast<int>(config.PreloadFrameWithGSData) == value);

		case GSHWFixId::DisablePartialInvalidation:
			return (static_cast<int>(config.UserHacks_DisablePartialInvalidation) == value);

		case GSHWFixId::TextureInsideRT:
			return (static_cast<int>(config.UserHacks_TextureInsideRt) == value);

		case GSHWFixId::Limit24BitDepth:
			return (static_cast<int>(config.UserHacks_Limit24BitDepth) == value);

		case GSHWFixId::AlignSprite:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_AlignSpriteX) == value);

		case GSHWFixId::MergeSprite:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_MergePPSprite) == value);

		case GSHWFixId::ForceEvenSpritePosition:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_ForceEvenSpritePosition) == value);

		case GSHWFixId::BilinearUpscale:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_BilinearHack) == value);

		case GSHWFixId::NativePaletteDraw:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_NativePaletteDraw) == value);

		case GSHWFixId::EstimateTextureRegion:
			return (static_cast<int>(config.UserHacks_EstimateTextureRegion) == value);

		case GSHWFixId::DrawBuffering:
			return (static_cast<int>(config.UserHacks_DrawBuffering) == value);

		case GSHWFixId::PCRTCOffsets:
			return (static_cast<int>(config.PCRTCOffsets) == value);

		case GSHWFixId::PCRTCOverscan:
			return (static_cast<int>(config.PCRTCOverscan) == value);

		case GSHWFixId::Mipmap:
			return (static_cast<int>(config.HWMipmap) == value);

		case GSHWFixId::AccurateAlphaTest:
			return (static_cast<int>(config.HWAccurateAlphaTest) == value);

		case GSHWFixId::TrilinearFiltering:
			return (config.TriFilter == TriFiltering::Automatic || static_cast<int>(config.TriFilter) == value);

		case GSHWFixId::SkipDrawStart:
			return (config.SkipDrawStart == value);

		case GSHWFixId::SkipDrawEnd:
			return (config.SkipDrawEnd == value);

		case GSHWFixId::HalfPixelOffset:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_HalfPixelOffset == static_cast<GSHalfPixelOffset>(value));

		case GSHWFixId::RoundSprite:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_RoundSprite == value);

		case GSHWFixId::NativeScaling:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_NativeScaling) == value);

		case GSHWFixId::TexturePreloading:
			return (static_cast<int>(config.TexturePreloading) <= value);

		case GSHWFixId::Deinterlace:
			return (config.InterlaceMode == GSInterlaceMode::Automatic || static_cast<int>(config.InterlaceMode) == value);

		case GSHWFixId::CPUSpriteRenderBW:
			return (config.UserHacks_CPUSpriteRenderBW == value);

		case GSHWFixId::CPUSpriteRenderLevel:
			return (config.UserHacks_CPUSpriteRenderLevel == value);

		case GSHWFixId::CPUCLUTRender:
			return (config.UserHacks_CPUCLUTRender == value);

		case GSHWFixId::GPUTargetCLUT:
			return (static_cast<int>(config.UserHacks_GPUTargetCLUTMode) == value);

		case GSHWFixId::GPUPaletteConversion:
			return (config.GPUPaletteConversion == ((value > 1) ? (config.TexturePreloading == TexturePreloadingLevel::Full) : (value != 0)));

		case GSHWFixId::MinimumBlendingLevel:
			return (static_cast<int>(config.AccurateBlendingUnit) >= value);

		case GSHWFixId::MaximumBlendingLevel:
			return (static_cast<int>(config.AccurateBlendingUnit) <= value);

		case GSHWFixId::RecommendedBlendingLevel:
			return true;

		case GSHWFixId::GetSkipCount:
			return (static_cast<int>(config.GetSkipCountFunctionId) == value);

		case GSHWFixId::BeforeDraw:
			return (static_cast<int>(config.BeforeDrawFunctionId) == value);

		case GSHWFixId::MoveHandler:
			return (static_cast<int>(config.MoveHandlerFunctionId) == value);

		default:
			return false;
	}
}

void GameDatabaseSchema::GameEntry::applyGSHardwareFixes(Pcsx2Config::GSOptions& config) const
{
	std::string disabled_fixes;
	bool apply_auto_fixes = !config.ManualUserHacks;

#if defined(__APPLE__) && TARGET_OS_IPHONE
	const bool block_ios_metal_gs_fixes = ShouldBlockIOSMetalGSHardwareFixes(config);
	if (block_ios_metal_gs_fixes)
	{
		const std::string compat_profile = GetIOSCompatibilityLabProfileForGSPolicy();
		Console.Warning("@@IOS_METAL_GS_POLICY@@ compat_lab=%s game=\"%s\" renderer=%s enable_game_fixes=%d manual_user_hacks=%d action=sanitize_high_risk_callbacks",
			compat_profile.c_str(), name.c_str(), Pcsx2Config::GSOptions::GetRendererName(config.Renderer),
			EmuConfig.EnableGameFixes ? 1 : 0, config.ManualUserHacks ? 1 : 0);
		Console.Warning("iOS Metal CompatLabOff: allowing GameDB GS HW fixes; high-risk callbacks require the iOS Metal allowlist");
		ClearIOSMetalCompatLabOffGSHWFixes(config);
		apply_auto_fixes = false;
	}

	if (!block_ios_metal_gs_fixes && config.Renderer == GSRendererType::Metal && apply_auto_fixes)
		ClearIOSMetalHighRiskCallbacks(config, "pre-auto-apply");
#else
	constexpr bool block_ios_metal_gs_fixes = false;
#endif

	// Only apply GS HW fixes if the user hasn't manually enabled HW fixes.
	const bool is_sw_renderer = EmuConfig.GS.Renderer == GSRendererType::SW;
	if (!apply_auto_fixes && !block_ios_metal_gs_fixes)
		Console.Warning("GameDB: Manual GS hardware renderer fixes are enabled, not using automatic hardware renderer fixes from GameDB.");

	for (const auto& [id, value] : gsHWFixes)
	{
		bool force_apply_ios_metal_fix = false;
#if defined(__APPLE__) && TARGET_OS_IPHONE
		if (block_ios_metal_gs_fixes)
		{
			if (IsIOSMetalAllowedCompatLabOffGSHWFix(*this, id, value))
			{
				Console.Warning("@@IOS_METAL_GS_FIX_ALLOW@@ game=\"%s\" fix=%s requested=%d reason=%s",
					name.c_str(), getHWFixName(id), value,
					IsIOSMetalHighRiskAutoGSHWFix(id) ? "callback_allowlist" : "safe_gamedb_fix");
				force_apply_ios_metal_fix = true;
			}
			else
			{
				Console.Warning("@@IOS_METAL_GS_SKIP@@ game=\"%s\" fix=%s requested=%d reason=callback_fix_not_safe_on_metal",
					name.c_str(), getHWFixName(id), value);
				continue;
			}
		}

		if (!block_ios_metal_gs_fixes && config.Renderer == GSRendererType::Metal && apply_auto_fixes && IsIOSMetalHighRiskAutoGSHWFix(id))
		{
			if (IsIOSMetalAllowedAutoGSHWCallback(*this, id, value))
			{
				Console.Warning("@@IOS_METAL_GS_CALLBACK_ALLOW@@ game=\"%s\" fix=%s requested=%d reason=ios_metal_callback_allowlist",
					name.c_str(), getHWFixName(id), value);
			}
			else
			{
				Console.Warning("@@IOS_METAL_GS_SKIP@@ game=\"%s\" fix=%s requested=%d reason=callback_fix_not_safe_on_metal",
					name.c_str(), getHWFixName(id), value);
				ClearIOSMetalHighRiskCallbacks(config, getHWFixName(id));
				continue;
			}
		}
#endif

		if (isUserHackHWFix(id) && !apply_auto_fixes && !force_apply_ios_metal_fix)
		{
			if (configMatchesHWFix(config, id, value))
				continue;

			Console.Warning("GameDB: Skipping GS Hardware Fix: %s to [mode=%d]", getHWFixName(id), value);
			fmt::format_to(std::back_inserter(disabled_fixes), "{} {} = {}", disabled_fixes.empty() ? "  " : "\n  ", getHWFixName(id), value);
			continue;
		}

		int applied_log_value = value;

		switch (id)
		{
			case GSHWFixId::AutoFlush:
			{
				int applied_value = value;
#if defined(__APPLE__) && TARGET_OS_IPHONE
				if (IsIOSBurnoutRevengeGame(name) && applied_value == static_cast<int>(GSHWAutoFlushLevel::Enabled))
				{
					Console.Warning("@@IOS_BURNOUT_AUTOFLUSH_CLAMP@@ game=\"%s\" requested=%d applied=%d reason=metal_glow_probe",
						name.c_str(), applied_value, static_cast<int>(GSHWAutoFlushLevel::SpritesOnly));
					applied_value = static_cast<int>(GSHWAutoFlushLevel::SpritesOnly);
				}
#endif
				if (applied_value >= 0 && applied_value <= static_cast<int>(GSHWAutoFlushLevel::Enabled))
				{
					config.UserHacks_AutoFlush = static_cast<GSHWAutoFlushLevel>(applied_value);
					applied_log_value = applied_value;
				}
			}
			break;

			case GSHWFixId::CPUFramebufferConversion:
				config.UserHacks_CPUFBConversion = (value > 0);
				break;

			case GSHWFixId::FlushTCOnClose:
				config.UserHacks_ReadTCOnClose = (value > 0);
				break;

			case GSHWFixId::DisableDepthSupport:
				config.UserHacks_DisableDepthSupport = (value > 0);
				break;

			case GSHWFixId::PreloadFrameData:
				config.PreloadFrameWithGSData = (value > 0);
				break;

			case GSHWFixId::DisablePartialInvalidation:
				config.UserHacks_DisablePartialInvalidation = (value > 0);
				break;

			case GSHWFixId::TextureInsideRT:
			{
				if (value >= 0 && value <= static_cast<int>(GSTextureInRtMode::MergeTargets))
					config.UserHacks_TextureInsideRt = static_cast<GSTextureInRtMode>(value);
			}
			break;

			case GSHWFixId::Limit24BitDepth:
			{
				if (value >= 0 && value <= static_cast<int>(GSLimit24BitDepth::PrioritizeLower))
					config.UserHacks_Limit24BitDepth = static_cast<GSLimit24BitDepth>(value);
			}
			break;

			case GSHWFixId::AlignSprite:
				config.UserHacks_AlignSpriteX = (value > 0);
				break;

			case GSHWFixId::MergeSprite:
				config.UserHacks_MergePPSprite = (value > 0);
				break;

			case GSHWFixId::ForceEvenSpritePosition:
				config.UserHacks_ForceEvenSpritePosition = (value > 0);
				break;

			case GSHWFixId::BilinearUpscale:
			{
				if (value >= 0 && value < static_cast<int>(GSBilinearDirtyMode::MaxCount))
					config.UserHacks_BilinearHack = static_cast<GSBilinearDirtyMode>(value);
			}
			break;

			case GSHWFixId::NativePaletteDraw:
				config.UserHacks_NativePaletteDraw = (value > 0);
				break;

			case GSHWFixId::EstimateTextureRegion:
				config.UserHacks_EstimateTextureRegion = (value > 0);
				break;

			case GSHWFixId::DrawBuffering:
				config.UserHacks_DrawBuffering = (value > 0);
				break;

			case GSHWFixId::PCRTCOffsets:
				config.PCRTCOffsets = (value > 0);
				break;

			case GSHWFixId::PCRTCOverscan:
				config.PCRTCOverscan = (value > 0);
				break;

			case GSHWFixId::Mipmap:
				config.HWMipmap = (value > 0);
				break;

			case GSHWFixId::AccurateAlphaTest:
				config.HWAccurateAlphaTest = (value > 0);
				break;

			case GSHWFixId::TrilinearFiltering:
			{
				if (value >= 0 && value <= static_cast<int>(TriFiltering::Forced))
				{
					if (config.TriFilter == TriFiltering::Automatic)
						config.TriFilter = static_cast<TriFiltering>(value);
					else if (config.TriFilter > TriFiltering::Off)
						Console.Warning("GameDB: Game requires trilinear filtering to be disabled.");
				}
			}
			break;

			case GSHWFixId::SkipDrawStart:
				config.SkipDrawStart = value;
				break;

			case GSHWFixId::SkipDrawEnd:
				config.SkipDrawEnd = value;
				break;

			case GSHWFixId::HalfPixelOffset:
			{
				if (value >= 0 && value < static_cast<int>(GSHalfPixelOffset::MaxCount))
					config.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(value);
			}
			break;

			case GSHWFixId::RoundSprite:
				config.UserHacks_RoundSprite = value;
				break;

			case GSHWFixId::NativeScaling:
			{
				int applied_value = value;
#if defined(__APPLE__) && TARGET_OS_IPHONE
				if (applied_value >= static_cast<int>(GSNativeScaling::Aggressive))
				{
					Console.Warning("@@IOS_GAMEDB_GS_CLAMP@@ game=\"%s\" fix=nativeScaling requested=%d applied=%d",
						name.c_str(), applied_value, static_cast<int>(GSNativeScaling::Normal));
					applied_value = static_cast<int>(GSNativeScaling::Normal);
				}
#endif
				if (applied_value >= 0 && applied_value < static_cast<int>(GSNativeScaling::MaxCount))
					config.UserHacks_NativeScaling = static_cast<GSNativeScaling>(applied_value);
			}
			break;

			case GSHWFixId::TexturePreloading:
			{
				if (value >= 0 && value <= static_cast<int>(TexturePreloadingLevel::Full))
					config.TexturePreloading = std::min(config.TexturePreloading, static_cast<TexturePreloadingLevel>(value));
			}
			break;

			case GSHWFixId::Deinterlace:
			{
				if (value >= static_cast<int>(GSInterlaceMode::Automatic) && value < static_cast<int>(GSInterlaceMode::Count))
				{
					if (config.InterlaceMode == GSInterlaceMode::Automatic)
						config.InterlaceMode = static_cast<GSInterlaceMode>(value);
					else
						Console.Warning("GameDB: Game requires different deinterlace mode but it has been overridden by user setting.");
				}
			}
			break;

			case GSHWFixId::CPUSpriteRenderBW:
				config.UserHacks_CPUSpriteRenderBW = value;
				break;

			case GSHWFixId::CPUSpriteRenderLevel:
				config.UserHacks_CPUSpriteRenderLevel = value;
				break;

			case GSHWFixId::CPUCLUTRender:
				config.UserHacks_CPUCLUTRender = value;
				break;

			case GSHWFixId::GPUTargetCLUT:
			{
				if (value >= 0 && value <= static_cast<int>(GSGPUTargetCLUTMode::InsideTarget))
					config.UserHacks_GPUTargetCLUTMode = static_cast<GSGPUTargetCLUTMode>(value);
			}
			break;

			case GSHWFixId::GPUPaletteConversion:
			{
				// if 2, enable paltex when preloading is full, otherwise leave as-is
				if (value > 1)
					config.GPUPaletteConversion = (config.TexturePreloading == TexturePreloadingLevel::Full) ? true : config.GPUPaletteConversion;
				else
					config.GPUPaletteConversion = (value != 0);
			}
			break;

			case GSHWFixId::MinimumBlendingLevel:
			{
				if (value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum))
					config.AccurateBlendingUnit = std::max(config.AccurateBlendingUnit, static_cast<AccBlendLevel>(value));
			}
			break;

			case GSHWFixId::MaximumBlendingLevel:
			{
				if (value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum))
					config.AccurateBlendingUnit = std::min(config.AccurateBlendingUnit, static_cast<AccBlendLevel>(value));
			}
			break;

			case GSHWFixId::RecommendedBlendingLevel:
			{
#if defined(__APPLE__) && TARGET_OS_IPHONE
				// On iOS, honor the GameDB blending recommendation automatically
				// (upstream only shows an OSD hint the user has to act on). Games
				// like Silent Hill 2 (recommends Full) render their core effects —
				// the flashlight darkness mask, menu transparency — wrong at the
				// Basic default, and iOS users rarely dig into per-game settings.
				// Capped at Full (never Maximum), only raises the level, and never
				// when the user runs manual hardware hacks (their blending choice
				// wins). Applies in BOTH iOS policy modes — auto-apply and the
				// compat-lab-off "blocked" mode (this case is only reached there
				// when allowlisted as a safe non-callback fix); the Metal renderer
				// has full texture-barrier support so all levels are functional.
				if (!is_sw_renderer && !config.ManualUserHacks &&
					value > 0 && value <= static_cast<int>(AccBlendLevel::Maximum) &&
					static_cast<int>(config.AccurateBlendingUnit) < value)
				{
					const AccBlendLevel bumped =
						static_cast<AccBlendLevel>(std::min(value, static_cast<int>(AccBlendLevel::Full)));
					if (config.AccurateBlendingUnit < bumped)
					{
						Console.Warning("@@IOS_BLEND_BUMP@@ game=\"%s\" old=%d recommended=%d applied=%d",
							name.c_str(), static_cast<int>(config.AccurateBlendingUnit), value,
							static_cast<int>(bumped));
						config.AccurateBlendingUnit = bumped;
					}
					break;
				}
#endif
				if (!is_sw_renderer && value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum) && static_cast<int>(EmuConfig.GS.AccurateBlendingUnit) < value)
				{
					static constexpr std::array<const char*, static_cast<u8>(AccBlendLevel::MaxCount)> s_blending_option_names = {{
						TRANSLATE_NOOP("GameDatabase", "Minimum"),
						TRANSLATE_NOOP("GameDatabase", "Basic"),
						TRANSLATE_NOOP("GameDatabase", "Medium"),
						TRANSLATE_NOOP("GameDatabase", "High"),
						TRANSLATE_NOOP("GameDatabase", "Full"),
						TRANSLATE_NOOP("GameDatabase", "Maximum"),
					}};

					Host::AddKeyedOSDMessage("HWBlendingWarning",
						fmt::format(TRANSLATE_FS("GameDatabase",
										"{0} Current Blending Accuracy is {1}.\n"
										"Recommended Blending Accuracy for this game is {2}.\n"
										"You can adjust the blending level in Game Properties to improve\n"
										"graphical quality, but this will increase system requirements."),
							ICON_FA_PAINTBRUSH,
							s_blending_option_names[static_cast<u8>(EmuConfig.GS.AccurateBlendingUnit)],
							s_blending_option_names[static_cast<u8>(value)]),
						Host::OSD_WARNING_DURATION);
				}
				else
				{
					Host::RemoveKeyedOSDMessage("HWBlendingWarning");
				}
			}
			break;

			case GSHWFixId::RecommendedAccurateAlphaTest:
			{
				if (!is_sw_renderer && value >= 0 && value <= 1 &&
					static_cast<int>(config.HWAccurateAlphaTest) < value)
				{
					Host::AddKeyedOSDMessage("HWAlphaTestWarning",
						fmt::format(TRANSLATE_FS("GameDatabase",
										"{0} Accurate Alpha Test is currently disabled.\n"
										"This game recommends enabling Accurate Alpha Test.\n"
										"You can enable it in Game Properties to improve graphical\n"
										"accuracy, but this may increase system requirements."),
							ICON_FA_PAINTBRUSH),
						Host::OSD_WARNING_DURATION);
				}
				else
				{
					Host::RemoveKeyedOSDMessage("HWAlphaTestWarning");
				}
			}
			break;

			case GSHWFixId::RecommendedHWAA1:
			{
				if (!is_sw_renderer && value >= 0 && value <= 1 &&
					static_cast<int>(config.HWAA1) < value)
				{
					Host::AddKeyedOSDMessage("HWAA1Warning",
						fmt::format(TRANSLATE_FS("GameDatabase",
										"{0} AA1 is currently disabled.\n"
										"This game recommends enabling AA1.\n"
										"You can enable it in Game Properties to improve graphical\n"
										"accuracy, but this may increase system requirements."),
							ICON_FA_PAINTBRUSH),
						Host::OSD_WARNING_DURATION);
				}
				else
				{
					Host::RemoveKeyedOSDMessage("HWAA1Warning");
				}
			}
			break;

			case GSHWFixId::GetSkipCount:
				config.GetSkipCountFunctionId = static_cast<s16>(value);
				break;

			case GSHWFixId::BeforeDraw:
				config.BeforeDrawFunctionId = static_cast<s16>(value);
				break;

			case GSHWFixId::MoveHandler:
				config.MoveHandlerFunctionId = static_cast<s16>(value);
				break;

			default:
				break;
		}

		if (applied_log_value != value)
			Console.WriteLn("GameDB: Enabled GS Hardware Fix: %s requested [mode=%d] applied [mode=%d]",
				getHWFixName(id), value, applied_log_value);
		else
			Console.WriteLn("GameDB: Enabled GS Hardware Fix: %s to [mode=%d]", getHWFixName(id), value);
	}

	// fixup skipdraw range just in case the db has a bad range (but the linter should catch this)
	config.SkipDrawEnd = std::max(config.SkipDrawStart, config.SkipDrawEnd);

	if (!is_sw_renderer && !disabled_fixes.empty())
	{
		Host::AddKeyedOSDMessage("HWFixesWarning",
			fmt::format(ICON_FA_WAND_MAGIC_SPARKLES " {}\n{}",
				TRANSLATE_SV("GameDatabase", "Manual GS hardware renderer fixes are enabled, automatic fixes were not applied:"),
				disabled_fixes),
			Host::OSD_ERROR_DURATION);
	}
	else
	{
		Host::RemoveKeyedOSDMessage("HWFixesWarning");
	}

#if defined(__APPLE__) && TARGET_OS_IPHONE
	if (config.Renderer == GSRendererType::Metal)
		LogIOSGameFixSnapshot(*this, config,
			block_ios_metal_gs_fixes ? "blocked-ios-metal" : (apply_auto_fixes ? "applied-ios-metal" : "manual-ios-metal"));
#endif
}

void GameDatabase::initDatabase()
{
	const std::string path(Path::Combine(EmuFolders::Resources, GAMEDB_YAML_FILE_NAME));
	const std::string name(GAMEDB_YAML_FILE_NAME);

	const std::optional<std::string> buffer = FileSystem::ReadFileToString(path.c_str());
	if (!buffer.has_value())
	{
		Console.Error("GameDB: Unable to open GameDB file, file does not exist.");
		return;
	}

	const ryml::csubstr yaml = ryml::to_csubstr(*buffer);

	Error error;
	std::optional<ryml::Tree> tree = ParseYAMLFromString(yaml, ryml::to_csubstr(name), &error);
	if (!tree.has_value())
	{
		Console.ErrorFmt("GameDB: Failed to parse game database file {}:", path);
		Console.Error(error.GetDescription());
		return;
	}

	ryml::NodeRef root = tree->rootref();

	for (const ryml::NodeRef& n : root.children())
	{
		auto serial = StringUtil::toLower(std::string(n.key().str, n.key().len));

		// Serials and CRCs must be inserted as lower-case, as that is how they are retrieved
		// this is because the application may pass a lowercase CRC or serial along
		//
		// However, YAML's keys are as expected case-sensitive, so we have to explicitly do our own duplicate checking
		if (s_game_db.count(serial) == 1)
		{
			Console.ErrorFmt("GameDB: Duplicate serial '{}' found in GameDB. Skipping, Serials are case-insensitive!", serial);
			continue;
		}

		if (n.is_map())
		{
			parseAndInsert(serial, n);
		}
	}
}

void GameDatabase::ensureLoaded()
{
	std::call_once(s_load_once_flag, []() {
		Common::Timer timer;
		Console.WriteLn(fmt::format("GameDB: Has not been initialized yet, initializing..."));
		initDatabase();
		Console.WriteLn("GameDB: %zu games on record (loaded in %.2fms)", s_game_db.size(), timer.GetTimeMilliseconds());
	});
}

size_t GameDatabase::entryCount()
{
	GameDatabase::ensureLoaded();
	return s_game_db.size();
}

const GameDatabaseSchema::GameEntry* GameDatabase::findGame(const std::string_view serial)
{
	GameDatabase::ensureLoaded();

	auto iter = s_game_db.find(StringUtil::toLower(serial));
	return (iter != s_game_db.end()) ? &iter->second : nullptr;
}

bool GameDatabase::TrackHash::parseHash(const std::string_view str)
{
	constexpr u32 expected_length = SIZE * 2;
	if (str.length() != expected_length)
		return false;

	std::memset(data, 0, sizeof(data));
	for (u32 i = 0; i < SIZE * 2; i++)
	{
		const char ch = str[i];
		u8 b;
		if (ch >= '0' && ch <= '9')
			b = static_cast<u8>(ch - '0');
		else if (ch >= 'a' && ch <= 'f')
			b = static_cast<u8>(ch - 'a') + 0xa;
		else if (ch >= 'A' && ch <= 'F')
			b = static_cast<u8>(ch - 'A') + 0xa;
		else
			return false;

		data[i / 2] |= ((i % 2) == 0) ? (b << 4) : b;
	}

	return true;
}

std::string GameDatabase::TrackHash::toString() const
{
	return fmt::format(
		"{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
		data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
		data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
}

struct TrackHashHasher
{
	size_t operator()(const GameDatabase::TrackHash& hash) const
	{
		return std::hash<std::string_view>()(std::string_view(reinterpret_cast<const char*>(hash.data),
			GameDatabase::TrackHash::SIZE));
	}
};

static constexpr char HASHDB_YAML_FILE_NAME[] = "RedumpDatabase.yaml";
std::unordered_map<GameDatabase::TrackHash, u32, TrackHashHasher> s_track_hash_to_entry_map;
std::vector<GameDatabase::HashDatabaseEntry> s_hash_database;

static bool parseHashDatabaseEntry(const ryml::NodeRef& node)
{
	if (!node.has_child("name") || !node.has_child("hashes"))
	{
		Console.Warning("[HashDatabase] Incomplete entry found.");
		return false;
	}

	GameDatabase::HashDatabaseEntry entry;
	node["name"] >> entry.name;
	if (node.has_child("version"))
		node["version"] >> entry.version;
	if (node.has_child("serial"))
		node["serial"] >> entry.serial;

	const u32 index = static_cast<u32>(s_hash_database.size());
	for (const ryml::ConstNodeRef& n : node["hashes"].children())
	{
		if (!n.is_map() || !n.has_child("size") || !n.has_child("md5"))
		{
			Console.ErrorFmt("[HashDatabase] Incomplete hash definition in {}", entry.name);
			return false;
		}

		GameDatabase::TrackHash th;
		std::string md5;
		n["md5"] >> md5;
		n["size"] >> th.size;

		if (!th.parseHash(md5))
		{
			Console.ErrorFmt("[HashDatabase] Failed to parse hash in {}: '{}'", entry.name, md5);
			return false;
		}

		if (entry.tracks.empty() && s_track_hash_to_entry_map.find(th) != s_track_hash_to_entry_map.end())
			Console.WarningFmt("[HashDatabase] Duplicate first track hash in {}", entry.name);

		entry.tracks.push_back(th);
		s_track_hash_to_entry_map.emplace(th, index);
	}

	s_hash_database.push_back(std::move(entry));
	return true;
}

bool GameDatabase::loadHashDatabase()
{
	if (!s_hash_database.empty())
		return true;

	Common::Timer load_timer;

	const std::string path(Path::Combine(EmuFolders::Resources, HASHDB_YAML_FILE_NAME));
	const std::string name(HASHDB_YAML_FILE_NAME);

	std::optional<std::string> buffer = FileSystem::ReadFileToString(path.c_str());
	if (!buffer.has_value())
	{
		Console.Error("[HashDatabase] Unable to open hash database file, file does not exist.");
		return false;
	}

	ryml::csubstr yaml = ryml::to_csubstr(*buffer);

	Error error;
	std::optional<ryml::Tree> tree = ParseYAMLFromString(yaml, ryml::to_csubstr(name), &error);
	if (!tree.has_value())
	{
		Console.ErrorFmt("[HashDatabase] Failed to parse hash database file {}:", path);
		Console.Error(error.GetDescription());
		return false;
	}

	ryml::NodeRef root = tree->rootref();

	bool okay = true;
	for (const ryml::NodeRef& n : root.children())
	{
		if (!parseHashDatabaseEntry(n))
		{
			okay = false;
			break;
		}
	}

	if (!okay)
	{
		s_track_hash_to_entry_map.clear();
		s_hash_database.clear();
		return false;
	}

	Console.WriteLn(Color_StrongGreen, "[HashDatabase] Loaded YAML in %.0f ms", load_timer.GetTimeMilliseconds());
	return true;
}

void GameDatabase::unloadHashDatabase()
{
	s_track_hash_to_entry_map.clear();
	s_hash_database.clear();
}

static size_t getTrackIndex(const GameDatabase::TrackHash* tracks, size_t num_tracks, const GameDatabase::TrackHash& track)
{
	for (size_t i = 0; i < num_tracks; i++)
	{
		if (tracks[i] == track)
			return i;
	}
	return num_tracks;
}

const GameDatabase::HashDatabaseEntry* GameDatabase::lookupHash(
	const TrackHash* tracks, size_t num_tracks, bool* tracks_matched, std::string* match_error)
{
	loadHashDatabase();

	if (num_tracks == 0)
	{
		*match_error = TRANSLATE_STR("GameDatabase", "No tracks provided.");
		std::memset(tracks_matched, 0, sizeof(bool) * num_tracks);
		return nullptr;
	}

	// match the first track, for DVDs this will be all there is anyway
	const auto data_iter = s_track_hash_to_entry_map.find(tracks[0]);
	if (data_iter == s_track_hash_to_entry_map.end())
	{
		*match_error = fmt::format(TRANSLATE_FS("GameDatabase", "Hash {} is not in database."), tracks[0].toString());
		std::memset(tracks_matched, 0, sizeof(bool) * num_tracks);
		return nullptr;
	}

	// make sure they're not missing the data track
	const GameDatabase::HashDatabaseEntry* candidate = &s_hash_database[data_iter->second];
	if (getTrackIndex(candidate->tracks.data(), candidate->tracks.size(), tracks[0]) != 0)
	{
		*match_error = TRANSLATE_STR("GameDatabase", "Data track number does not match data track in database.");
		std::memset(tracks_matched, 0, sizeof(bool) * num_tracks);
		return nullptr;
	}

	// first track is okay!
	tracks_matched[0] = true;
	match_error->clear();

	// now check any audio tracks...
	bool all_okay = true;
	for (size_t track = 1; track < num_tracks; track++)
	{
		const auto audio_iter = s_track_hash_to_entry_map.find(tracks[track]);
		if (audio_iter != s_track_hash_to_entry_map.end())
		{
			fmt::format_to(std::back_inserter(*match_error),
				TRANSLATE_FS("GameDatabase", "Track {0} with hash {1} is not found in database.\n"), track + 1,
				tracks[track].toString());
			tracks_matched[track] = false;
			all_okay = false;
			continue;
		}

		// same game?
		if (audio_iter->second != data_iter->second)
		{
			fmt::format_to(std::back_inserter(*match_error),
				TRANSLATE_FS("GameDatabase", "Track {0} with hash {1} is for a different game ({2}).\n"), track + 1,
				tracks[track].toString(), s_hash_database[audio_iter->second].name);
			tracks_matched[track] = false;
			all_okay = false;
			continue;
		}

		// make sure it's the correct track number
		if (getTrackIndex(candidate->tracks.data(), candidate->tracks.size(), tracks[track]) != track)
		{
			fmt::format_to(std::back_inserter(*match_error),
				TRANSLATE_FS("GameDatabase", "Track {0} with hash {1} does not match database track.\n"), track + 1,
				tracks[track].toString());
			tracks_matched[track] = false;
			all_okay = false;
			continue;
		}

		tracks_matched[track] = true;
	}

	if (!match_error->empty() && match_error->back() == '\n')
		match_error->pop_back();

	return all_okay ? candidate : nullptr;
}
