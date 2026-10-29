// ARMSX2Bridge+Settings.mm — INI get and set, live apply and graphics hack state
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "VMManager.h"
#include "GameDatabase.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"

#include "IOSRuntime.h"

#include <array>
#include <functional>
#include <mutex>
#include <vector>

static void ARMSX2ApplyLiveTargetSpeedSetting(std::function<void()> update)
{
    if (!VMManager::HasValidVM()) {
        update();
        return;
    }

    Host::RunOnCPUThread([update = std::move(update)]() mutable {
        update();
        VMManager::UpdateTargetSpeed();
    }, false);
}

static float ARMSX2EnforceRetroAchievementsHardcoreFloatSetting(const char* section, const char* key, float value)
{
    if (!ARMSX2RetroAchievementsHardcoreActive())
        return value;

    if (std::strcmp(section, "Framerate") == 0) {
        if (std::strcmp(key, "NominalScalar") == 0 && value < 1.0f) {
            ARMSX2LogRetroAchievementsHardcoreBlock("slowdown_nominal_scalar");
            return 1.0f;
        }
        if (std::strcmp(key, "SlomoScalar") == 0 && value < 1.0f) {
            ARMSX2LogRetroAchievementsHardcoreBlock("slowdown_slomo_scalar");
            return 1.0f;
        }
        // 0 is unlimited, which the core's hardcore clamp keeps too.
        if (std::strcmp(key, "TurboScalar") == 0 && value > 0.0f && value < 1.0f) {
            ARMSX2LogRetroAchievementsHardcoreBlock("slowdown_turbo_scalar");
            return 1.0f;
        }
    } else if (std::strcmp(section, "EmuCore/GS") == 0) {
        if (std::strcmp(key, "FramerateNTSC") == 0 &&
            std::fabs(value - Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC) > 0.001f) {
            ARMSX2LogRetroAchievementsHardcoreBlock("framerate_ntsc_override");
            return Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC;
        }
        if (std::strcmp(key, "FrameratePAL") == 0 &&
            std::fabs(value - Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_PAL) > 0.001f) {
            ARMSX2LogRetroAchievementsHardcoreBlock("framerate_pal_override");
            return Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_PAL;
        }
    }

    return value;
}

static bool ARMSX2ShouldBlockRetroAchievementsHardcoreBoolSetting(const char* section, const char* key, bool value)
{
    if (!value || !ARMSX2RetroAchievementsHardcoreActive())
        return false;

    if (std::strcmp(section, "EmuCore") == 0) {
        if (std::strcmp(key, "EnableCheats") == 0) {
            ARMSX2LogRetroAchievementsHardcoreBlock("enable_cheats");
            return true;
        }
        if (std::strcmp(key, "EnableRecordingTools") == 0 || std::strcmp(key, "EnablePINE") == 0) {
            ARMSX2LogRetroAchievementsHardcoreBlock(key);
            return true;
        }
    }

    return false;
}

// Emulation-speed scalars only. EmuCore/GS is not handled here: every graphics
// setting reloads through the Setting<T> hook -> applyGraphicsSettingsNow.
static void ARMSX2ApplyLiveFloatSetting(const char* section, const char* key, float value)
{
    if (std::strcmp(section, "Framerate") != 0)
        return;

    if (std::strcmp(key, "NominalScalar") == 0) {
        const float clamped = ARMSX2SanitizedNominalScalar(value);
        ARMSX2ApplyLiveTargetSpeedSetting([clamped]() { EmuConfig.EmulationSpeed.NominalScalar = clamped; });
    } else if (std::strcmp(key, "TurboScalar") == 0) {
        ARMSX2ApplyLiveTargetSpeedSetting([value]() { EmuConfig.EmulationSpeed.TurboScalar = value; });
    } else if (std::strcmp(key, "SlomoScalar") == 0) {
        ARMSX2ApplyLiveTargetSpeedSetting([value]() { EmuConfig.EmulationSpeed.SlomoScalar = value; });
    }
}

#pragma mark - Graphics hack state

// Why a hack isn't doing what the screen says. Mirrored in GraphicsSettingsView.
enum class ARMSX2GraphicsHackReason : int
{
    Applied = 0,
    NeedsManualHacks,
    NeedsUpscaling,
    FromGameDatabase,
    NoGame,
    PerGame,
};

struct ARMSX2GraphicsHackDescriptor
{
    const char* ini_key;
    GSUserHackOverride override_id;
    GameDatabaseSchema::GSHWFixId hw_fix_id;
    // Upscaling masks these at native res whatever else is true.
    bool upscaling_only;
    // Bools are written to the INI as true/false, so reading one back as an int just
    // gives you the default and every row looks overridden.
    bool is_bool;
    int (*read)(const Pcsx2Config::GSOptions& gs);
};

struct ARMSX2GraphicsHackState
{
    const char* ini_key = "";
    int effective = 0;
    ARMSX2GraphicsHackReason reason = ARMSX2GraphicsHackReason::NoGame;
    bool pinned = false;
};

static constexpr std::array<ARMSX2GraphicsHackDescriptor, 24> s_graphics_hacks = {{
    {"UserHacks_align_sprite_X", GSUserHackOverride::AlignSprite, GameDatabaseSchema::GSHWFixId::AlignSprite, true, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_AlignSpriteX); }},
    {"UserHacks_merge_pp_sprite", GSUserHackOverride::MergeSprite, GameDatabaseSchema::GSHWFixId::MergeSprite, true, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_MergePPSprite); }},
    {"UserHacks_ForceEvenSpritePosition", GSUserHackOverride::ForceEvenSpritePosition, GameDatabaseSchema::GSHWFixId::ForceEvenSpritePosition, true, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_ForceEvenSpritePosition); }},
    {"UserHacks_NativePaletteDraw", GSUserHackOverride::NativePaletteDraw, GameDatabaseSchema::GSHWFixId::NativePaletteDraw, true, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_NativePaletteDraw); }},
    {"UserHacks_round_sprite_offset", GSUserHackOverride::RoundSprite, GameDatabaseSchema::GSHWFixId::RoundSprite, true, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_RoundSprite); }},
    {"UserHacks_HalfPixelOffset", GSUserHackOverride::HalfPixelOffset, GameDatabaseSchema::GSHWFixId::HalfPixelOffset, true, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_HalfPixelOffset); }},
    {"UserHacks_native_scaling", GSUserHackOverride::NativeScaling, GameDatabaseSchema::GSHWFixId::NativeScaling, true, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_NativeScaling); }},
    {"UserHacks_TCOffsetX", GSUserHackOverride::TextureOffsetX, GameDatabaseSchema::GSHWFixId::Count, true, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_TCOffsetX); }},
    {"UserHacks_TCOffsetY", GSUserHackOverride::TextureOffsetY, GameDatabaseSchema::GSHWFixId::Count, true, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_TCOffsetY); }},
    {"UserHacks_TextureInsideRt", GSUserHackOverride::TextureInsideRt, GameDatabaseSchema::GSHWFixId::TextureInsideRT, false, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_TextureInsideRt); }},
    {"UserHacks_BilinearHack", GSUserHackOverride::BilinearHack, GameDatabaseSchema::GSHWFixId::BilinearUpscale, true, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_BilinearHack); }},
    {"preload_frame_with_gs_data", GSUserHackOverride::PreloadFrameData, GameDatabaseSchema::GSHWFixId::PreloadFrameData, false, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.PreloadFrameWithGSData); }},
    {"UserHacks_DisablePartialInvalidation", GSUserHackOverride::DisablePartialInvalidation, GameDatabaseSchema::GSHWFixId::DisablePartialInvalidation, false, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_DisablePartialInvalidation); }},
    {"paltex", GSUserHackOverride::GPUPaletteConversion, GameDatabaseSchema::GSHWFixId::GPUPaletteConversion, false, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.GPUPaletteConversion); }},
    {"UserHacks_DisableDepthSupport", GSUserHackOverride::DisableDepthSupport, GameDatabaseSchema::GSHWFixId::DisableDepthSupport, false, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_DisableDepthSupport); }},
    {"UserHacks_CPU_FB_Conversion", GSUserHackOverride::CPUFBConversion, GameDatabaseSchema::GSHWFixId::CPUFramebufferConversion, false, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_CPUFBConversion); }},
    {"UserHacks_ReadTCOnClose", GSUserHackOverride::ReadTCOnClose, GameDatabaseSchema::GSHWFixId::Count, false, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_ReadTCOnClose); }},
    {"UserHacks_EstimateTextureRegion", GSUserHackOverride::EstimateTextureRegion, GameDatabaseSchema::GSHWFixId::EstimateTextureRegion, false, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_EstimateTextureRegion); }},
    {"UserHacks_DrawBuffering", GSUserHackOverride::DrawBuffering, GameDatabaseSchema::GSHWFixId::DrawBuffering, false, true,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_DrawBuffering); }},
    {"UserHacks_Limit24BitDepth", GSUserHackOverride::Limit24BitDepth, GameDatabaseSchema::GSHWFixId::Limit24BitDepth, false, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_Limit24BitDepth); }},
    {"UserHacks_CPUSpriteRenderBW", GSUserHackOverride::CPUSpriteRenderBW, GameDatabaseSchema::GSHWFixId::CPUSpriteRenderBW, false, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_CPUSpriteRenderBW); }},
    {"UserHacks_CPUSpriteRenderLevel", GSUserHackOverride::CPUSpriteRenderLevel, GameDatabaseSchema::GSHWFixId::CPUSpriteRenderLevel, false, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_CPUSpriteRenderLevel); }},
    {"UserHacks_CPUCLUTRender", GSUserHackOverride::CPUCLUTRender, GameDatabaseSchema::GSHWFixId::CPUCLUTRender, false, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_CPUCLUTRender); }},
    {"UserHacks_GPUTargetCLUTMode", GSUserHackOverride::GPUTargetCLUT, GameDatabaseSchema::GSHWFixId::GPUTargetCLUT, false, false,
        [](const Pcsx2Config::GSOptions& gs) { return static_cast<int>(gs.UserHacks_GPUTargetCLUTMode); }},
}};

static std::mutex s_graphics_hack_mutex;
static std::vector<ARMSX2GraphicsHackState> s_graphics_hack_state;

static const ARMSX2GraphicsHackDescriptor* ARMSX2FindGraphicsHack(const char* ini_key)
{
    for (const ARMSX2GraphicsHackDescriptor& hack : s_graphics_hacks) {
        if (std::strcmp(hack.ini_key, ini_key) == 0)
            return &hack;
    }
    return nullptr;
}

static bool ARMSX2GameDatabaseSetsHWFix(GameDatabaseSchema::GSHWFixId id)
{
    if (id == GameDatabaseSchema::GSHWFixId::Count)
        return false;

    const GameDatabaseSchema::GameEntry* game = GameDatabase::findGame(VMManager::GetDiscSerial());
    if (!game)
        return false;

    for (const auto& [fix_id, value] : game->gsHWFixes) {
        if (fix_id == id)
            return true;
    }
    return false;
}

// CPU thread only: EmuConfig is its and the masks have only settled by the time an
// apply is finished.
extern "C" void ARMSX2_CaptureGraphicsHackState(void)
{
    std::vector<ARMSX2GraphicsHackState> snapshot;
    snapshot.reserve(s_graphics_hacks.size());

    const bool has_vm = VMManager::HasValidVM();
    const Pcsx2Config::GSOptions& gs = EmuConfig.GS;

    // Resolved before the lock below, deliberately. It walks the game database, and
    // anything that reaches back into settings from under that lock would hang.
    std::array<bool, s_graphics_hacks.size()> database_sets{};
    for (size_t i = 0; i < s_graphics_hacks.size(); i++)
        database_sets[i] = has_vm && ARMSX2GameDatabaseSetsHWFix(s_graphics_hacks[i].hw_fix_id);

    // One lock for the whole sweep, reading the two layers by hand. The Host getters
    // take this same lock per call and it isn't recursive, so nothing in here may call
    // one -- that would hang rather than misreport.
    std::unique_lock<std::mutex> settings_lock = Host::GetSettingsLock();
    const SettingsInterface* base_layer = Host::Internal::GetBaseSettingsLayer();
    const SettingsInterface* game_layer = Host::Internal::GetGameSettingsLayer();

    for (size_t i = 0; i < s_graphics_hacks.size(); i++) {
        const ARMSX2GraphicsHackDescriptor& hack = s_graphics_hacks[i];
        ARMSX2GraphicsHackState state;
        state.ini_key = hack.ini_key;
        state.pinned = gs.IsUserHackPinned(hack.override_id);

        if (!has_vm) {
            state.reason = ARMSX2GraphicsHackReason::NoGame;
            snapshot.push_back(state);
            continue;
        }

        state.effective = hack.read(gs);

        // The graphics screen edits globals, so it shows the base value. The game may be
        // running the per-game one, which is a different number and worth saying out loud.
        const bool from_game_layer = game_layer && game_layer->ContainsValue("EmuCore/GS", hack.ini_key);
        const SettingsInterface* source = from_game_layer ? game_layer : base_layer;
        int requested = 0;
        if (source) {
            requested = hack.is_bool ?
                static_cast<int>(source->GetBoolValue("EmuCore/GS", hack.ini_key, false)) :
                source->GetIntValue("EmuCore/GS", hack.ini_key, 0);
        }

        if (state.effective == requested)
            state.reason = from_game_layer ? ARMSX2GraphicsHackReason::PerGame : ARMSX2GraphicsHackReason::Applied;
        else if (hack.upscaling_only && gs.UpscaleMultiplier <= 1.0f)
            state.reason = ARMSX2GraphicsHackReason::NeedsUpscaling;
        else if (database_sets[i])
            state.reason = ARMSX2GraphicsHackReason::FromGameDatabase;
        else
            state.reason = ARMSX2GraphicsHackReason::NeedsManualHacks;

        snapshot.push_back(state);
    }
    settings_lock.unlock();

    {
        std::lock_guard<std::mutex> lock(s_graphics_hack_mutex);
        s_graphics_hack_state = std::move(snapshot);
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2GraphicsHackStateChanged" object:nil];
    });
}

@implementation ARMSX2Bridge (Settings)

#pragma mark - INI generic getter/setter

+ (int)getINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def {
    if (!ARMSX2HasBaseSettings()) return def;
    return Host::GetBaseIntSettingValue(section.UTF8String, key.UTF8String, def);
}

+ (BOOL)getINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def {
    if (!ARMSX2HasBaseSettings()) return def;
    return Host::GetBaseBoolSettingValue(section.UTF8String, key.UTF8String, def);
}

+ (float)getINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def {
    if (!ARMSX2HasBaseSettings()) return def;
    return Host::GetBaseFloatSettingValue(section.UTF8String, key.UTF8String, def);
}

+ (nonnull NSString *)getINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def {
    if (!ARMSX2HasBaseSettings()) return def;
    std::string val = Host::GetBaseStringSettingValue(section.UTF8String, key.UTF8String, def.UTF8String);
    return [NSString stringWithUTF8String:val.c_str()];
}

+ (void)setINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value {
    if (!ARMSX2HasBaseSettings()) return;
    if (ARMSX2RetroAchievementsHardcoreActive() &&
        std::strcmp(section.UTF8String, "EmuCore/Speedhacks") == 0 &&
        std::strcmp(key.UTF8String, "EECycleRate") == 0 && value < 0) {
        ARMSX2LogRetroAchievementsHardcoreBlock("ee_underclock");
        value = 0;
    }
    Host::SetBaseIntSettingValue(section.UTF8String, key.UTF8String, value);
    ARMSX2ScheduleINISave();
}

+ (void)setINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value {
    if (!ARMSX2HasBaseSettings()) return;
    if (ARMSX2ShouldBlockRetroAchievementsHardcoreBoolSetting(section.UTF8String, key.UTF8String, value))
        value = NO;
    Host::SetBaseBoolSettingValue(section.UTF8String, key.UTF8String, value);
    ARMSX2ScheduleINISave();
}

+ (void)setINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value {
    if (!ARMSX2HasBaseSettings()) return;
    const bool speedScalar = std::strcmp(section.UTF8String, "Framerate") == 0 &&
        (std::strcmp(key.UTF8String, "TurboScalar") == 0 || std::strcmp(key.UTF8String, "SlomoScalar") == 0);
    if (speedScalar && (!std::isfinite(value) || value < 0.0f))
        return;
    float valueToStore = value;
    valueToStore = ARMSX2EnforceRetroAchievementsHardcoreFloatSetting(section.UTF8String, key.UTF8String, valueToStore);
    if (std::strcmp(section.UTF8String, "Framerate") == 0 && std::strcmp(key.UTF8String, "NominalScalar") == 0)
        valueToStore = ARMSX2SanitizedNominalScalar(valueToStore);

    Host::SetBaseFloatSettingValue(section.UTF8String, key.UTF8String, valueToStore);
    ARMSX2ScheduleINISave();
    ARMSX2ApplyLiveFloatSetting(section.UTF8String, key.UTF8String, valueToStore);
}

+ (void)setINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value {
    if (!ARMSX2HasBaseSettings()) return;
    Host::SetBaseStringSettingValue(section.UTF8String, key.UTF8String, value.UTF8String);
    ARMSX2ScheduleINISave();
}

+ (void)clearINISection:(nonnull NSString *)section {
    if (!ARMSX2HasBaseSettings()) return;
    ARMSX2EditBaseSettings([&](SettingsInterface& si) { si.ClearSection(section.UTF8String); });
    ARMSX2ScheduleINISave();
}

// Reloads settings into the running VM and pushes graphics options to the GS
// thread so visual changes take effect without restarting the game. Safe to call
// when no VM/GS is open (it returns without doing anything).
+ (void)applyGraphicsSettingsNow
{
    if (!VMManager::HasValidVM())
        return;

    // ApplySettings (EmuConfig, the JIT caches) and MTGS::ApplySettings (the ring) are CPU
    // thread work and this runs on the UI thread. The push is not redundant: applyOsdPreset and
    // setPerformanceOverlayVisible pre-write EmuConfig.GS, so the reload may skip its own push.
    Host::RunOnCPUThread([]() {
        VMManager::ApplySettings();
        if (MTGS::IsOpen())
            MTGS::ApplySettings();
        ARMSX2_CaptureGraphicsHackState();
    });
}

// The game can run a different value than the player set: MaskUserHacks, MaskUpscalingHacks
// and the GameDB sit in between and settle on the CPU thread, so snapshot there after an
// apply and let the settings screen read the copy.
+ (nonnull NSDictionary<NSString *, id> *)graphicsHackState
{
    NSMutableDictionary<NSString*, id>* result = [NSMutableDictionary dictionary];

    // The snapshot is only meaningful while a game is up. It is taken again on shutdown,
    // but whether the VM is already gone by then decides what it catches, and a leftover
    // "the game database is setting this" back in the menu would be nonsense.
    if (!VMManager::HasValidVM())
        return result;

    std::lock_guard<std::mutex> lock(s_graphics_hack_mutex);
    for (const ARMSX2GraphicsHackState& hack : s_graphics_hack_state) {
        result[@(hack.ini_key)] = @{
            @"effective": @(hack.effective),
            @"reason": @(static_cast<int>(hack.reason)),
            @"pinned": @(hack.pinned),
        };
    }
    return result;
}

// Writes the claim only. The caller asks for the apply, so claiming a hack and changing
// its value in the same gesture coalesce into one instead of applying twice. Bit math
// stays here rather than in Swift because the enum lives in Config.h.
+ (void)setGraphicsHackPinned:(nonnull NSString *)iniKey pinned:(BOOL)pinned
{
    if (!ARMSX2HasBaseSettings())
        return;

    const ARMSX2GraphicsHackDescriptor* descriptor = ARMSX2FindGraphicsHack(iniKey.UTF8String);
    if (!descriptor)
        return;

    const u32 bit = 1u << static_cast<u32>(descriptor->override_id);
    u32 mask = static_cast<u32>(Host::GetBaseIntSettingValue("EmuCore/GS", "UserHackOverrides", 0));
    mask = pinned ? (mask | bit) : (mask & ~bit);
    Host::SetBaseIntSettingValue("EmuCore/GS", "UserHackOverrides", static_cast<int>(mask));
    ARMSX2ScheduleINISave();
}

// Force any deferred base-settings INI write to disk immediately.
+ (void)flushINISettings
{
    ARMSX2FlushINISave();
}

@end
