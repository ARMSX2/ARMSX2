// ARMSX2Bridge.mm — ObjC bridge implementation
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2Bridge.h"

// Xcode names the generated Swift bridge header after the Swift module.
#if __has_include("ARMSX2iOS-Swift.h")
#import "ARMSX2iOS-Swift.h"
#define ARMSX2_HAS_SWIFTUI_HOST 1
#elif __has_include("ARMSX2-Swift.h")
#import "ARMSX2-Swift.h"
#define ARMSX2_HAS_SWIFTUI_HOST 1
#else
#define ARMSX2_HAS_SWIFTUI_HOST 0
#endif

// MetalFX spatial upscaler is iOS 16+ device and weak-linked (see PCSX2 CMake).
// The iOS Simulator SDK does not ship the MetalFX framework, so the import is
// gated off when targeting the sim; isMetalFXSupported then returns NO without
// referencing MTLFXSpatialScalerDescriptor.
#import <Metal/Metal.h>
#if !TARGET_OS_SIMULATOR
	#import <MetalFX/MetalFX.h>
	#define ARMSX2_HAS_METALFX 1
#else
	#define ARMSX2_HAS_METALFX 0
#endif

// Only the preset API is used here, to read a preset's parameters. That is runtime-agnostic,
// so a plain include suffices — no LIBRA_RUNTIME_* opt-in of the kind GSDeviceMTL.mm needs.
#ifdef ARMSX2_HAS_LIBRASHADER
#include "librashader.h"
#endif

#include "common/Darwin/DarwinMisc.h"
#include <SDL3/SDL.h>

extern "C" void ARMSX2_SetSDLFullscreen(bool enabled);
extern "C" bool ARMSX2_IsSDLFullscreen();
extern "C" void ARMSX2_iOSCopyDeviceStats(int* outBatteryPercent, int* outThermalState,
                                          double* outRamGB, bool* outLowPower);
#include "Common.h"
#include "Config.h"
#include "CDVD/CDVD.h"
#include "CDVD/CDVDcommon.h"
#include "VMManager.h"
#include "pcsx2/MTGS.h"
#include "Patch.h"
#include "Achievements.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Sio.h"
#include "Counters.h"
#include "GS/GS.h"
#include "GS/GSState.h"
#include "SPU2/spu2.h"
#include "GameList.h"
#include "GameDatabase.h"
#include "ps2/BiosTools.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/PerformanceMetrics.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ZipHelpers.h"
#include "common/Error.h"
#include "common/MRCHelpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <stdlib.h>
#include <sys/stat.h>

// Access the global settings interface from ios_main.mm
extern INISettingsInterface* g_p44_settings_interface;
extern "C" void ARMSX2_PrepareGameRenderViewForCurrentRenderer(const char* reason);
extern "C" void ARMSX2_PostRuntimeMenuStateChanged(void);
extern "C" void ARMSX2_ApplyEffectivePresentFPSCap(void);
extern "C" void ARMSX2_iOSTestGamepadRumble(void);
extern "C" bool ARMSX2_IsIdleVMPrewarmResolved(void);

// Coalesce base-settings INI writes so rapid changes (slider drags, preset bursts,
// repeated toggles) persist to disk once per short window instead of once per call.
// The in-memory interface is updated immediately, so reads always observe the latest
// value; only disk persistence is deferred. ARMSX2FlushINISave() forces a write now.
static BOOL s_ini_save_scheduled = NO;
static NSUInteger s_ini_save_generation = 0;

static void ARMSX2ScheduleINISave()
{
    if (!g_p44_settings_interface || s_ini_save_scheduled)
        return;
    s_ini_save_scheduled = YES;
    s_ini_save_generation++;
    const NSUInteger scheduled_generation = s_ini_save_generation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)),
        dispatch_get_main_queue(),
        ^{
            s_ini_save_scheduled = NO;
            if (scheduled_generation == s_ini_save_generation && g_p44_settings_interface)
                g_p44_settings_interface->Save();
        });
}

static void ARMSX2FlushINISave()
{
    s_ini_save_generation++;
    s_ini_save_scheduled = NO;
    if (g_p44_settings_interface)
        g_p44_settings_interface->Save();
}

static NSDate* s_lastNVMSaveDate = nil;
static ARMSX2RetroAchievementsToastInfo* s_pendingRetroAchievementsNotification = nil;

// This file has no ARC, so a static holding an object has to own it. Both writers below
// are handed autoreleased objects, and a raw assignment leaves the static pointing at
// freed memory once the pool drains.
static void ARMSX2SetLastNVMSaveDate(NSDate* date)
{
#if __has_feature(objc_arc)
    s_lastNVMSaveDate = date;
#else
    [s_lastNVMSaveDate release];
    s_lastNVMSaveDate = [date retain];
#endif
}

@implementation ARMSX2SaveStateSlotInfo
@end

@implementation ARMSX2BIOSInfo
@end

@implementation ARMSX2RetroAchievementsToastInfo
#if !__has_feature(objc_arc)
- (void)dealloc
{
    [_title release];
    [_message release];
    [_badgePath release];
    [super dealloc];
}
#endif
@end

static void ARMSX2SetPendingRetroAchievementsNotification(ARMSX2RetroAchievementsToastInfo* toast)
{
#if __has_feature(objc_arc)
    s_pendingRetroAchievementsNotification = toast;
#else
    [s_pendingRetroAchievementsNotification release];
    s_pendingRetroAchievementsNotification = [toast retain];
#endif
}

static void ARMSX2ClearPendingRetroAchievementsNotification()
{
#if __has_feature(objc_arc)
    s_pendingRetroAchievementsNotification = nil;
#else
    [s_pendingRetroAchievementsNotification release];
    s_pendingRetroAchievementsNotification = nil;
#endif
}

static NSString* const ARMSX2CompatibilityProfileOff = @"off";
static NSString* const ARMSX2CompatibilityProfileCOP1 = @"cop1";
static NSString* const ARMSX2CompatibilityProfileLoadStore = @"loadstore";
static NSString* const ARMSX2CompatibilityProfileMMI = @"mmi";
static NSString* const ARMSX2CompatibilityProfileCOP2VU = @"cop2vu";
static NSString* const ARMSX2CompatibilityProfileMultDiv = @"multdiv";
static NSString* const ARMSX2CompatibilityProfileShifts = @"shifts";
static NSString* const ARMSX2CompatibilityProfileMoves = @"moves";
static NSString* const ARMSX2CompatibilityProfileIntegerALU = @"integeralu";
static NSString* const ARMSX2CompatibilityProfileBranches = @"branches";
static NSString* const ARMSX2CompatibilityProfileCustom = @"custom";
static constexpr int ARMSX2UseGlobalIntSentinel = -1;
// "Use global" markers. Out of band for their ranges: upscale is positive, the int keys start
// at 0, AspectRatio uses an empty string.
static constexpr float ARMSX2UseGlobalFloatSentinel = -1.0f;
static constexpr int ARMSX2TriFilterUseGlobalSentinel = std::numeric_limits<int>::min();
static constexpr int ARMSX2DefaultAudioVolumePercent = 100;

static int ARMSX2ClampInt(int value, int minValue, int maxValue)
{
    return std::min(std::max(value, minValue), maxValue);
}

static NSString* ARMSX2NSStringFromStdString(const std::string& value);

static NSString* ARMSX2RegionFallbackForSerial(const std::string& serial)
{
    std::string normalized;
    normalized.reserve(serial.size());
    for (const char ch : serial)
    {
        if (ch != '-' && ch != '_' && ch != ' ')
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }

    auto startsWith = [&normalized](const char* prefix) {
        return normalized.rfind(prefix, 0) == 0;
    };

    if (startsWith("SLUS") || startsWith("SCUS") || startsWith("PBPX"))
        return @"NTSC-U";
    if (startsWith("SLES") || startsWith("SCES") || startsWith("SLED") || startsWith("SCED"))
        return @"PAL";
    if (startsWith("SLPS") || startsWith("SLPM") || startsWith("SCPS") || startsWith("PCPX") || startsWith("SCAJ"))
        return @"NTSC-J";
    if (startsWith("SLKA") || startsWith("SCKA"))
        return @"NTSC-K";
    if (startsWith("SCCS"))
        return @"NTSC-C";
    if (startsWith("SLAJ"))
        return @"NTSC-HK";

    return nil;
}

static NSString* ARMSX2BIOSDisplayRegionForZone(NSString* zone)
{
    if ([zone isEqualToString:@"USA"])
        return @"North America";
    if ([zone length] > 0)
        return zone;
    return @"Unknown Region";
}

static NSString* ARMSX2BIOSCountryCodeForZone(NSString* zone)
{
    if ([zone isEqualToString:@"Japan"])
        return @"JP";
    if ([zone isEqualToString:@"USA"])
        return @"US";
    if ([zone isEqualToString:@"Europe"])
        return @"EU";
    if ([zone isEqualToString:@"Asia"])
        return @"HK";
    if ([zone isEqualToString:@"China"])
        return @"CN";
    return @"";
}

static ARMSX2BIOSInfo* ARMSX2MakeBIOSInfo(NSString* fileName, NSString* directory)
{
    ARMSX2BIOSInfo* info = [ARMSX2BIOSInfo new];
    info.fileName = fileName ?: @"";
    info.filePath = directory ? [directory stringByAppendingPathComponent:fileName ?: @""] : @"";
    info.regionName = @"Unknown Region";
    info.countryCode = @"";
    info.descriptionText = @"Region unavailable";
    info.regionCode = -1;
    info.valid = NO;

    u32 version = 0;
    u32 region = 0;
    std::string description;
    std::string zone;
    if (IsBIOS(info.filePath.UTF8String, version, description, region, zone)) {
        NSString* zoneString = ARMSX2NSStringFromStdString(zone);
        info.valid = YES;
        info.regionCode = static_cast<NSInteger>(region);
        info.regionName = ARMSX2BIOSDisplayRegionForZone(zoneString);
        info.countryCode = ARMSX2BIOSCountryCodeForZone(zoneString);
        info.descriptionText = ARMSX2NSStringFromStdString(description);
    }

    return info;
}

static int* ARMSX2JITBisectFlagPtr(NSString* key)
{
    if ([key isEqualToString:@"COP1EverythingOnly"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_ONLY;
    if ([key isEqualToString:@"COP1EverythingPlusLoadStore"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_LOADSTORE;
    if ([key isEqualToString:@"COP1EverythingPlusMMI"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MMI;
    if ([key isEqualToString:@"COP1EverythingPlusCOP2VU"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_COP2_VU;
    if ([key isEqualToString:@"COP1EverythingPlusMultDiv"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MULTDIV;
    if ([key isEqualToString:@"COP1EverythingPlusShifts"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_SHIFTS;
    if ([key isEqualToString:@"COP1EverythingPlusMoves"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MOVES;
    if ([key isEqualToString:@"COP1EverythingPlusIntegerALU"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_INTEGER_ALU;
    if ([key isEqualToString:@"COP1EverythingPlusBranches"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_BRANCHES;
    return nullptr;
}

static void ARMSX2ApplyJITBisectFlag(NSString* key, BOOL enabled)
{
    if (int* flag = ARMSX2JITBisectFlagPtr(key))
        *flag = enabled ? 1 : 0;
}

static NSArray<NSString*>* ARMSX2JITBisectFlagKeys()
{
    static NSArray<NSString*>* keys;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        keys = [[NSArray alloc] initWithObjects:
            @"COP1EverythingOnly",
            @"COP1EverythingPlusLoadStore",
            @"COP1EverythingPlusMMI",
            @"COP1EverythingPlusCOP2VU",
            @"COP1EverythingPlusMultDiv",
            @"COP1EverythingPlusShifts",
            @"COP1EverythingPlusMoves",
            @"COP1EverythingPlusIntegerALU",
            @"COP1EverythingPlusBranches",
            nil];
    });
    return keys;
}

static NSString* ARMSX2CompatibilityProfileFlagKey(NSString* profile)
{
    if ([profile isEqualToString:ARMSX2CompatibilityProfileCOP1]) return @"COP1EverythingOnly";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileLoadStore]) return @"COP1EverythingPlusLoadStore";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileMMI]) return @"COP1EverythingPlusMMI";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileCOP2VU]) return @"COP1EverythingPlusCOP2VU";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileMultDiv]) return @"COP1EverythingPlusMultDiv";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileShifts]) return @"COP1EverythingPlusShifts";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileMoves]) return @"COP1EverythingPlusMoves";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileIntegerALU]) return @"COP1EverythingPlusIntegerALU";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileBranches]) return @"COP1EverythingPlusBranches";
    return @"";
}

static NSString* ARMSX2NormalizeCompatibilityProfile(NSString* profile)
{
    NSString* normalized = [profile.lowercaseString stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if ([normalized isEqualToString:ARMSX2CompatibilityProfileCOP1] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileLoadStore] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileMMI] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileCOP2VU] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileMultDiv] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileShifts] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileMoves] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileIntegerALU] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileBranches] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileCustom])
        return normalized;

    return ARMSX2CompatibilityProfileOff;
}

static NSString* ARMSX2CurrentCompatibilityProfileFromSettings()
{
    if (!g_p44_settings_interface)
        return ARMSX2CompatibilityProfileOff;

    std::string stored = g_p44_settings_interface->GetStringValue("ARMSX2/JITBisect", "Profile", "");
    NSString* storedProfile = ARMSX2NormalizeCompatibilityProfile(ARMSX2NSStringFromStdString(stored));
    if (![storedProfile isEqualToString:ARMSX2CompatibilityProfileOff] && ![storedProfile isEqualToString:ARMSX2CompatibilityProfileCustom])
        return storedProfile;

    NSString* activeProfile = ARMSX2CompatibilityProfileOff;
    int activeCount = 0;
    for (NSString* key in ARMSX2JITBisectFlagKeys()) {
        if (g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", key.UTF8String, false)) {
            activeCount++;
            NSString* profile = ARMSX2CompatibilityProfileOff;
            if ([key isEqualToString:@"COP1EverythingOnly"]) profile = ARMSX2CompatibilityProfileCOP1;
            else if ([key isEqualToString:@"COP1EverythingPlusLoadStore"]) profile = ARMSX2CompatibilityProfileLoadStore;
            else if ([key isEqualToString:@"COP1EverythingPlusMMI"]) profile = ARMSX2CompatibilityProfileMMI;
            else if ([key isEqualToString:@"COP1EverythingPlusCOP2VU"]) profile = ARMSX2CompatibilityProfileCOP2VU;
            else if ([key isEqualToString:@"COP1EverythingPlusMultDiv"]) profile = ARMSX2CompatibilityProfileMultDiv;
            else if ([key isEqualToString:@"COP1EverythingPlusShifts"]) profile = ARMSX2CompatibilityProfileShifts;
            else if ([key isEqualToString:@"COP1EverythingPlusMoves"]) profile = ARMSX2CompatibilityProfileMoves;
            else if ([key isEqualToString:@"COP1EverythingPlusIntegerALU"]) profile = ARMSX2CompatibilityProfileIntegerALU;
            else if ([key isEqualToString:@"COP1EverythingPlusBranches"]) profile = ARMSX2CompatibilityProfileBranches;
            activeProfile = profile;
        }
    }

    return activeCount == 0 ? ARMSX2CompatibilityProfileOff : (activeCount == 1 ? activeProfile : ARMSX2CompatibilityProfileCustom);
}

static void ARMSX2ApplyCompatibilityProfile(NSString* profile, BOOL persistSettings, NSString* reason)
{
    NSString* normalized = ARMSX2NormalizeCompatibilityProfile(profile);
    if ([normalized isEqualToString:ARMSX2CompatibilityProfileCustom]) {
        if (persistSettings && g_p44_settings_interface) {
            g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", normalized.UTF8String);
            g_p44_settings_interface->Save();
        }

        NSLog(@"[ARMSX2Bridge] Compatibility preset=custom reason=%@ flags preserved", reason ?: @"manual");
        std::fprintf(stderr, "@@IOS_JIT_PROFILE_APPLY@@ profile=custom reason=\"%s\" persisted=%d flags_preserved=1\n",
            reason ? reason.UTF8String : "manual", persistSettings ? 1 : 0);
        std::fflush(stderr);
        return;
    }

    NSString* activeFlag = ARMSX2CompatibilityProfileFlagKey(normalized);

    for (NSString* key in ARMSX2JITBisectFlagKeys()) {
        const BOOL enabled = activeFlag.length > 0 && [key isEqualToString:activeFlag];
        ARMSX2ApplyJITBisectFlag(key, enabled);
        if (persistSettings && g_p44_settings_interface)
            g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", key.UTF8String, enabled);
    }

    if (persistSettings && g_p44_settings_interface) {
        g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", normalized.UTF8String);
        g_p44_settings_interface->Save();
    }

    NSLog(@"[ARMSX2Bridge] Compatibility preset=%@ reason=%@", normalized, reason ?: @"manual");
    std::fprintf(stderr,
        "@@IOS_JIT_PROFILE_APPLY@@ profile=%s flag=%s reason=\"%s\" persisted=%d cop1=%d ls=%d mmi=%d cop2vu=%d multdiv=%d shifts=%d moves=%d ialu=%d branches=%d\n",
        normalized.UTF8String, activeFlag.UTF8String, reason ? reason.UTF8String : "manual", persistSettings ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_ONLY ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_LOADSTORE ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MMI ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_COP2_VU ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MULTDIV ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_SHIFTS ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MOVES ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_INTEGER_ALU ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_BRANCHES ? 1 : 0);
    std::fflush(stderr);
}

static NSString* ARMSX2CompatibilityCustomFlagSection(NSString* identity)
{
    return [NSString stringWithFormat:@"ARMSX2/JITBisectGamePresetFlags/%@", identity ?: @""];
}

static void ARMSX2SaveCompatibilityCustomFlagsForIdentity(NSString* identity)
{
    if (!g_p44_settings_interface || identity.length == 0)
        return;

    NSString* section = ARMSX2CompatibilityCustomFlagSection(identity);
    g_p44_settings_interface->SetStringValue("ARMSX2/JITBisectGamePresets", identity.UTF8String, ARMSX2CompatibilityProfileCustom.UTF8String);
    for (NSString* key in ARMSX2JITBisectFlagKeys()) {
        BOOL enabled = NO;
        if (int* flag = ARMSX2JITBisectFlagPtr(key))
            enabled = (*flag != 0) ? YES : NO;
        else
            enabled = g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", key.UTF8String, false) ? YES : NO;

        g_p44_settings_interface->SetBoolValue(section.UTF8String, key.UTF8String, enabled ? true : false);
    }
    g_p44_settings_interface->Save();
    NSLog(@"[ARMSX2Bridge] Compatibility custom flags saved identity=%@", identity);
}

static BOOL ARMSX2LoadCompatibilityCustomFlagsForIdentity(NSString* identity)
{
    if (!g_p44_settings_interface || identity.length == 0)
        return NO;

    NSString* section = ARMSX2CompatibilityCustomFlagSection(identity);
    bool foundAny = false;
    bool anyEnabled = false;

    for (NSString* key in ARMSX2JITBisectFlagKeys()) {
        bool enabled = false;
        if (g_p44_settings_interface->GetBoolValue(section.UTF8String, key.UTF8String, &enabled))
            foundAny = true;
        if (enabled)
            anyEnabled = true;

        ARMSX2ApplyJITBisectFlag(key, enabled ? YES : NO);
        g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", key.UTF8String, enabled);
    }

    if (!foundAny || !anyEnabled)
    {
        std::fprintf(stderr, "@@IOS_JIT_PROFILE_CUSTOM@@ identity=\"%s\" found=%d enabled=0 action=ignore_empty_custom\n",
            identity ? identity.UTF8String : "", foundAny ? 1 : 0);
        std::fflush(stderr);
        return NO;
    }

    g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", ARMSX2CompatibilityProfileCustom.UTF8String);
    g_p44_settings_interface->Save();
    NSLog(@"[ARMSX2Bridge] Compatibility custom flags loaded identity=%@", identity);
    std::fprintf(stderr,
        "@@IOS_JIT_PROFILE_CUSTOM@@ identity=\"%s\" found=1 enabled=1 cop1=%d ls=%d mmi=%d cop2vu=%d multdiv=%d shifts=%d moves=%d ialu=%d branches=%d\n",
        identity ? identity.UTF8String : "",
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_ONLY ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_LOADSTORE ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MMI ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_COP2_VU ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MULTDIV ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_SHIFTS ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MOVES ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_INTEGER_ALU ? 1 : 0,
        DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_BRANCHES ? 1 : 0);
    std::fflush(stderr);
    return YES;
}

static void ARMSX2ClearCompatibilityCustomFlagsForIdentity(NSString* identity)
{
    if (!g_p44_settings_interface || identity.length == 0)
        return;

    NSString* section = ARMSX2CompatibilityCustomFlagSection(identity);
    g_p44_settings_interface->ClearSection(section.UTF8String);
}

static NSString* ARMSX2NSStringFromStdString(const std::string& value)
{
    if (value.empty())
        return @"";

    NSString* string = [NSString stringWithUTF8String:value.c_str()];
    return string ?: @"";
}

static NSString* ARMSX2NSStringFromStringView(std::string_view value)
{
    if (value.empty())
        return @"";

    NSString* string = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    return string ?: @"";
}

extern "C" void ARMSX2_PostRetroAchievementsStateChanged(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2RetroAchievementsStateChanged" object:nil];
    });
}

extern "C" void ARMSX2_PostRetroAchievementsNotification(const char* title, const char* message,
	const char* badgePath, float duration)
{
    NSString* titleString = title ? [NSString stringWithUTF8String:title] : nil;
    if (titleString.length == 0)
        return;

    NSString* messageString = message ? [NSString stringWithUTF8String:message] : nil;
    NSString* badgePathString = badgePath ? [NSString stringWithUTF8String:badgePath] : nil;
    if (!messageString)
        messageString = @"";
    if (!badgePathString)
        badgePathString = @"";

    // A non-positive duration means "use the SwiftUI default"; the key is omitted so the
    // receiver falls back to its own configured display time.
    NSNumber* durationNumber = (duration > 0.0f) ? @(duration) : nil;

    dispatch_async(dispatch_get_main_queue(), ^{
        ARMSX2RetroAchievementsToastInfo* toast = [[ARMSX2RetroAchievementsToastInfo alloc] init];
        toast.title = titleString;
        toast.message = messageString;
        toast.badgePath = badgePathString;
        toast.duration = durationNumber != nil ? durationNumber.doubleValue : 0.0;
        ARMSX2SetPendingRetroAchievementsNotification(toast);
#if !__has_feature(objc_arc)
        [toast release];
#endif

        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2RetroAchievementsNotification"
                                                           object:nil];
    });
}

static dispatch_queue_t ARMSX2RetroAchievementsQueue()
{
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("org.armsx2.ios.retroachievements", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

static constexpr bool ARMSX2RetroAchievementsAvailable = true;
static constexpr bool ARMSX2RetroAchievementsHardcoreAvailable = true;

static NSString* ARMSX2RetroAchievementsUnavailableMessage()
{
    return @"RetroAchievements is temporarily unavailable in this build.";
}

static void ARMSX2SaveBaseSettingBool(const char* section, const char* key, bool value);

static void ARMSX2ForceRetroAchievementsHardcoreOff()
{
    Pcsx2Config::AchievementsOptions old_config = EmuConfig.Achievements;
    EmuConfig.Achievements.HardcoreMode = false;
    ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", false);

    if (Achievements::IsActive())
        Achievements::UpdateSettings(old_config);
}

static bool ARMSX2EnsureAchievementsClientInitialized()
{
    if (!EmuConfig.Achievements.Enabled)
        return false;

    if (!Achievements::IsActive())
        return Achievements::Initialize();

    return true;
}

static bool ARMSX2RetroAchievementsHardcoreActive()
{
    return EmuConfig.Achievements.Enabled && Achievements::IsHardcoreModeActive();
}

static void ARMSX2LogRetroAchievementsHardcoreBlock(const char* action)
{
    NSLog(@"[ARMSX2Bridge] RetroAchievements Hardcore blocked action=%s", action ? action : "unknown");
}

static void ARMSX2SaveBaseSettingBool(const char* section, const char* key, bool value)
{
    Host::SetBaseBoolSettingValue(section, key, value);
    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetBoolValue(section, key, value);
        g_p44_settings_interface->Save();
    }
}

static void ARMSX2UpdateAchievementsSettings(void (^mutate)())
{
    Pcsx2Config::AchievementsOptions old_config = EmuConfig.Achievements;
    mutate();
    Achievements::UpdateSettings(old_config);
    ARMSX2_PostRetroAchievementsStateChanged();
}

static BOOL ARMSX2GetCurrentSaveStateIdentity(std::string* serial, u32* crc)
{
    if (!VMManager::HasValidVM())
        return NO;

    const std::string currentSerial = VMManager::GetDiscSerial();
    const u32 currentCRC = VMManager::GetDiscCRC();
    if (currentSerial.empty() || currentCRC == 0)
        return NO;

    if (serial)
        *serial = currentSerial;
    if (crc)
        *crc = currentCRC;
	return YES;
}

static NSString* const ARMSX2ExternalGameDirectoriesDefaultsKey = @"ARMSX2iOSExternalGameDirectories";

static BOOL ARMSX2IsPathInsideDirectory(NSString* path, NSString* directory)
{
	if (path.length == 0 || directory.length == 0)
		return NO;

	NSString* normalizedPath = path.stringByStandardizingPath;
	NSString* normalizedDirectory = directory.stringByStandardizingPath;
	if ([normalizedPath isEqualToString:normalizedDirectory])
		return YES;

	NSString* prefix = [normalizedDirectory hasSuffix:@"/"] ? normalizedDirectory : [normalizedDirectory stringByAppendingString:@"/"];
	return [normalizedPath hasPrefix:prefix];
}

static NSMutableArray<NSURL*>* ARMSX2ActiveExternalGameAccessURLs()
{
	static NSMutableArray<NSURL*>* activeAccess = nil;
	static dispatch_once_t onceToken;
	dispatch_once(&onceToken, ^{
		activeAccess = [[NSMutableArray alloc] init];
	});
	return activeAccess;
}

static BOOL ARMSX2ExternalGameAccessAlreadyActive(NSString* path)
{
	NSMutableArray<NSURL*>* activeAccess = ARMSX2ActiveExternalGameAccessURLs();
	@synchronized(activeAccess) {
		for (NSURL* activeURL in activeAccess) {
			if (![activeURL isKindOfClass:NSURL.class])
				continue;

			NSString* activePath = activeURL.path;
			if (activePath.length > 0 && ARMSX2IsPathInsideDirectory(path, activePath))
				return YES;
		}
	}

	return NO;
}

static void ARMSX2RememberExternalGameAccess(NSURL* url)
{
	if (!url)
		return;

	NSString* normalizedPath = url.path.stringByStandardizingPath;
	if (normalizedPath.length == 0)
		return;

	NSMutableArray<NSURL*>* activeAccess = ARMSX2ActiveExternalGameAccessURLs();
	@synchronized(activeAccess) {
		for (NSURL* activeURL in activeAccess) {
			if (![activeURL isKindOfClass:NSURL.class])
				continue;
			if ([activeURL.path.stringByStandardizingPath isEqualToString:normalizedPath])
				return;
		}

		[activeAccess addObject:url];
	}
}

static BOOL ARMSX2IsSupportedGameImageAtPath(NSString* path);

static NSArray<NSDictionary*>* ARMSX2ExternalGameDirectoryRecords()
{
	id rawRecords = [[NSUserDefaults standardUserDefaults] objectForKey:ARMSX2ExternalGameDirectoriesDefaultsKey];
	if (![rawRecords isKindOfClass:NSArray.class]) {
		if (rawRecords)
			NSLog(@"[ARMSX2Bridge] External game folder records ignored unexpectedClass=%@", [rawRecords class]);
		return @[];
	}

	NSMutableArray<NSDictionary*>* records = [NSMutableArray array];
	for (id rawRecord in (NSArray*)rawRecords) {
		if ([rawRecord isKindOfClass:NSDictionary.class])
			[records addObject:(NSDictionary*)rawRecord];
		else
			NSLog(@"[ARMSX2Bridge] External game folder record ignored unexpectedClass=%@", [rawRecord class]);
	}

	return records;
}

static BOOL ARMSX2ExternalGameRecordIsDirectory(NSDictionary* record, NSURL* url)
{
	id isDirectoryValue = record[@"isDirectory"];
	if ([isDirectoryValue isKindOfClass:NSNumber.class])
		return [(NSNumber*)isDirectoryValue boolValue];

	NSString* kind = [record[@"kind"] isKindOfClass:NSString.class] ? record[@"kind"] : nil;
	if ([kind isEqualToString:@"file"])
		return NO;
	if ([kind isEqualToString:@"folder"])
		return YES;

	NSNumber* isDirectory = nil;
	if ([url getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil])
		return isDirectory.boolValue;

	return !ARMSX2IsSupportedGameImageAtPath(url.path);
}

static BOOL ARMSX2ExternalGameRecordIsCloudProvider(NSDictionary* record, NSURL* url)
{
	NSString* path = (url.path ?: @"").lowercaseString;
	NSString* displayName = [record[@"displayName"] isKindOfClass:NSString.class] ? [(NSString*)record[@"displayName"] lowercaseString] : @"";
	return [path containsString:@"google"] || [displayName containsString:@"google"];
}

static BOOL ARMSX2ExternalGameRecordScanDisabled(NSDictionary* record, NSURL* url)
{
	if (ARMSX2ExternalGameRecordIsCloudProvider(record, url))
		return YES;

	id scanDisabledValue = record[@"scanDisabled"];
	if ([scanDisabledValue isKindOfClass:NSNumber.class] && [(NSNumber*)scanDisabledValue boolValue])
		NSLog(@"[ARMSX2Bridge] Ignoring stale external scanDisabled flag for non-cloud path=%@", url.path);

	return NO;
}

static NSURL* ARMSX2ResolveExternalGameDirectoryRecord(NSDictionary* record)
{
	NSString* path = [record[@"path"] isKindOfClass:NSString.class] ? record[@"path"] : nil;
	NSData* bookmarkData = [record[@"bookmarkData"] isKindOfClass:NSData.class] ? record[@"bookmarkData"] : nil;
	if (bookmarkData.length > 0) {
		BOOL stale = NO;
		NSError* error = nil;
		NSURL* url = [NSURL URLByResolvingBookmarkData:bookmarkData
		                                       options:0
		                                 relativeToURL:nil
		                           bookmarkDataIsStale:&stale
		                                         error:&error];
		if (url) {
			if (stale)
				NSLog(@"[ARMSX2Bridge] External game folder bookmark is stale path=%@", url.path);
			return url;
		}

		NSLog(@"[ARMSX2Bridge] External game folder bookmark failed path=%@ error=%@",
		      path ?: @"", error.localizedDescription ?: @"");
	}

	if (path.length > 0) {
		BOOL isDirectory = YES;
		id isDirectoryValue = record[@"isDirectory"];
		NSString* kind = [record[@"kind"] isKindOfClass:NSString.class] ? record[@"kind"] : nil;
		if ([isDirectoryValue isKindOfClass:NSNumber.class])
			isDirectory = [(NSNumber*)isDirectoryValue boolValue];
		else if ([kind isEqualToString:@"file"])
			isDirectory = NO;
		return [NSURL fileURLWithPath:path isDirectory:isDirectory];
	}

	return nil;
}

static BOOL ARMSX2StartExternalGameDirectoryAccessForPath(NSString* path)
{
	if (path.length == 0 || !path.isAbsolutePath)
		return NO;

	if (ARMSX2ExternalGameAccessAlreadyActive(path))
		return YES;

	for (NSDictionary* record in ARMSX2ExternalGameDirectoryRecords()) {
		NSURL* directoryURL = ARMSX2ResolveExternalGameDirectoryRecord(record);
		if (!directoryURL)
			continue;
		if (ARMSX2ExternalGameRecordIsCloudProvider(record, directoryURL)) {
			NSLog(@"[ARMSX2Bridge] External game cloud provider direct access skipped path=%@", directoryURL.path);
			continue;
		}

		BOOL isDirectory = ARMSX2ExternalGameRecordIsDirectory(record, directoryURL);
		NSString* normalizedRecordPath = directoryURL.path.stringByStandardizingPath;
		NSString* normalizedPath = path.stringByStandardizingPath;
		BOOL matches = isDirectory ? ARMSX2IsPathInsideDirectory(path, directoryURL.path) : [normalizedPath isEqualToString:normalizedRecordPath];
		if (!matches)
			continue;

		BOOL granted = [directoryURL startAccessingSecurityScopedResource];
		if (granted) {
			ARMSX2RememberExternalGameAccess(directoryURL);
			NSLog(@"[ARMSX2Bridge] External game %@ access active path=%@", isDirectory ? @"folder" : @"file", directoryURL.path);
		} else {
			NSLog(@"[ARMSX2Bridge] External game %@ access not granted path=%@", isDirectory ? @"folder" : @"file", directoryURL.path);
		}
		return granted;
	}

	return NO;
}

static BOOL ARMSX2StartExternalGameDirectoryAccessForPathSafe(NSString* path)
{
	@try {
		return ARMSX2StartExternalGameDirectoryAccessForPath(path);
	} @catch (NSException* exception) {
		NSLog(@"[ARMSX2Bridge] External game folder access exception path=%@ name=%@ reason=%@",
		      path ?: @"", exception.name ?: @"", exception.reason ?: @"");
		return NO;
	}
}

extern "C" bool ARMSX2_StartExternalGameDirectoryAccess(const char* path)
{
	if (!path || path[0] == '\0')
		return false;

	NSString* nsPath = [NSString stringWithUTF8String:path];
	return ARMSX2StartExternalGameDirectoryAccessForPathSafe(nsPath) ? true : false;
}

static BOOL ARMSX2IsSupportedGameImageAtPath(NSString* path)
{
	NSString* ext = path.pathExtension.lowercaseString;
	if ([ext isEqualToString:@"iso"] || [ext isEqualToString:@"img"] || [ext isEqualToString:@"chd"] ||
	    [ext isEqualToString:@"cso"] || [ext isEqualToString:@"zso"] || [ext isEqualToString:@"gz"] ||
	    [ext isEqualToString:@"elf"])
		return YES;

	if ([ext isEqualToString:@"bin"]) {
		NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
		return [attrs fileSize] > 50 * 1024 * 1024;
	}

	return NO;
}

static void ARMSX2EnumerateLocalGameImages(NSString* root, void (^block)(NSString* absolutePath, NSString* relativeName))
{
	NSFileManager* fm = [NSFileManager defaultManager];
	BOOL isDir = NO;
	if (root.length == 0 || block == nil || ![fm fileExistsAtPath:root isDirectory:&isDir] || !isDir)
		return;

	NSString* prefix = [root.stringByStandardizingPath stringByAppendingString:@"/"];
	for (NSURL* url in [fm enumeratorAtURL:[NSURL fileURLWithPath:root isDirectory:YES]
	               includingPropertiesForKeys:nil
	                                  options:NSDirectoryEnumerationSkipsHiddenFiles
	                             errorHandler:nil]) {
		NSString* path = url.path;
		if (!ARMSX2IsSupportedGameImageAtPath(path))
			continue;

		NSString* full = path.stringByStandardizingPath;
		NSString* rel = [full hasPrefix:prefix] ? [full substringFromIndex:prefix.length] : full.lastPathComponent;
		if ([rel containsString:@"/"] && ![rel.pathExtension.lowercaseString isEqualToString:@"elf"])
			continue;

		block(path, rel);
	}
}

static NSString* ARMSX2ResolveISOPath(NSString* isoName)
{
	if (isoName.length == 0)
		return nil;

	NSFileManager* fm = [NSFileManager defaultManager];
	if (isoName.isAbsolutePath) {
		if ([fm fileExistsAtPath:isoName])
			return isoName;

		if (ARMSX2StartExternalGameDirectoryAccessForPathSafe(isoName) && [fm fileExistsAtPath:isoName])
			return isoName;
	}

	NSString* isoPath = [[ARMSX2Bridge isoDirectory] stringByAppendingPathComponent:isoName];
	if ([fm fileExistsAtPath:isoPath])
		return isoPath;

    NSString* docsPath = [[ARMSX2Bridge documentsDirectory] stringByAppendingPathComponent:isoName];
    if ([fm fileExistsAtPath:docsPath])
        return docsPath;

    return nil;
}

static BOOL ARMSX2PerGameIdentityForCurrentGame(std::string* serial, u32* crc);

// There is one process-wide InputIsoFile, shared between the running VM and every
// metadata scan, so scanning the disc a game is playing from closes it out from
// under the game. GameList.h says as much above PopulateEntryFromPath: do not call
// it while the system is running. Everything after that reads zero blocks and the
// game starves while the emulator carries on at full speed, which is a miserable
// thing to debug from a bug report.
static bool ARMSX2PathIsRunningDisc(NSString* resolvedPath)
{
    if (resolvedPath.length == 0 || !VMManager::HasValidVM())
        return false;

    const std::string running = VMManager::GetDiscPath();
    return !running.empty() && running == resolvedPath.UTF8String;
}

static void ARMSX2NoteRunningDiscScan(NSString* path)
{
    // Once is enough. This went unnoticed for a long time precisely because it was
    // silent; if something finds a new way in, it should show up in an ordinary log
    // rather than needing a special build with a backtrace in it.
    static bool warned = false;
    if (warned)
        return;

    warned = true;
    Console.Warning("Not scanning '%s' while a VM is running; using the game list cache instead.",
        path.UTF8String);
}

static BOOL ARMSX2PopulateGameListEntryForISO(NSString* isoName, GameList::Entry* entry, NSString** resolvedPath)
{
    NSString* path = ARMSX2ResolveISOPath(isoName);
    if (resolvedPath)
        *resolvedPath = path;

    if (path.length == 0 || !entry)
        return NO;

    // Any VM, not just one playing this particular file. InputIsoFile::Open closes
    // whatever is already open before it opens anything, so scanning some unrelated
    // image while a game is running kills that game's disc just the same.
    if (VMManager::HasValidVM())
    {
        ARMSX2NoteRunningDiscScan(path);

        // Cache only, and a miss fails rather than falling through to a scan. The
        // worst case is a missing cover or title while a game is up; the
        // alternative is killing the disc out from under it.
        const auto lock = GameList::GetLock();
        const GameList::Entry* cached = GameList::GetEntryForPath(path.UTF8String);
        if (!cached)
            return NO;

        *entry = *cached;
        return YES;
    }

    return GameList::PopulateEntryFromPath(path.UTF8String, entry) ? YES : NO;
}

static NSString* ARMSX2CompatibilityIdentityKey(NSString* serial, u32 crc);

static NSArray<NSString*>* ARMSX2GameDataTokensForEntry(NSString* isoName, const GameList::Entry& entry)
{
    NSMutableOrderedSet<NSString*>* tokens = [NSMutableOrderedSet orderedSet];
    NSString* baseName = isoName.stringByDeletingPathExtension ?: isoName;
    if (baseName.length > 0)
        [tokens addObject:baseName.lowercaseString];
    if (!entry.serial.empty())
        [tokens addObject:ARMSX2NSStringFromStdString(entry.serial).lowercaseString];
    if (entry.crc != 0)
        [tokens addObject:[[NSString stringWithFormat:@"%08X", entry.crc] lowercaseString]];
    return tokens.array;
}

static NSInteger ARMSX2RemoveMatchingGeneratedFiles(NSString* directory, NSArray<NSString*>* tokens)
{
    if (directory.length == 0 || tokens.count == 0)
        return 0;

    NSFileManager* fm = [NSFileManager defaultManager];
    BOOL isDirectory = NO;
    if (![fm fileExistsAtPath:directory isDirectory:&isDirectory] || !isDirectory)
        return 0;

    NSMutableArray<NSURL*>* matches = [NSMutableArray array];
    NSURL* rootURL = [NSURL fileURLWithPath:directory isDirectory:YES];
    NSDirectoryEnumerator<NSURL*>* enumerator =
        [fm enumeratorAtURL:rootURL
 includingPropertiesForKeys:@[NSURLIsDirectoryKey]
                    options:NSDirectoryEnumerationSkipsHiddenFiles
               errorHandler:^BOOL(NSURL* url, NSError* error) {
                   NSLog(@"[ARMSX2Bridge] Game data scan skipped %@ error=%@", url.path, error.localizedDescription);
                   return YES;
               }];

    for (NSURL* url in enumerator) {
        NSString* name = url.lastPathComponent.lowercaseString;
        for (NSString* token in tokens) {
            if (token.length > 3 && [name containsString:token]) {
                [matches addObject:url];
                [enumerator skipDescendants];
                break;
            }
        }
    }

    [matches sortUsingComparator:^NSComparisonResult(NSURL* lhs, NSURL* rhs) {
        return lhs.path.length > rhs.path.length ? NSOrderedAscending : NSOrderedDescending;
    }];

    NSInteger removed = 0;
    for (NSURL* url in matches) {
        NSError* error = nil;
        if ([fm removeItemAtURL:url error:&error]) {
            removed++;
            NSLog(@"[ARMSX2Bridge] Removed generated game file %@", url.path);
        } else {
            NSLog(@"[ARMSX2Bridge] Failed removing generated game file %@ error=%@", url.path, error.localizedDescription);
        }
    }
    return removed;
}

static NSString* ARMSX2CompatibilityIdentityForISOName(NSString* isoName, GameList::Entry* entryOut = nullptr)
{
    GameList::Entry entry;
    NSString* resolvedPath = nil;
    if (!ARMSX2PopulateGameListEntryForISO(isoName, &entry, &resolvedPath) || entry.crc == 0)
        return @"";

    if (entryOut)
        *entryOut = entry;

    return ARMSX2CompatibilityIdentityKey(ARMSX2NSStringFromStdString(entry.serial), entry.crc);
}

static NSString* ARMSX2CompatibilityIdentityKey(NSString* serial, u32 crc)
{
    NSString* normalizedSerial = [[serial ?: @"" stringByReplacingOccurrencesOfString:@"_" withString:@"-"] uppercaseString];
    normalizedSerial = [normalizedSerial stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (normalizedSerial.length > 0)
        return normalizedSerial;

    if (crc != 0)
        return [NSString stringWithFormat:@"CRC-%08X", crc];

    return @"";
}

static NSString* ARMSX2CurrentCompatibilityIdentityKey()
{
    if (!VMManager::HasValidVM())
        return @"";

    return ARMSX2CompatibilityIdentityKey(ARMSX2NSStringFromStdString(VMManager::GetDiscSerial()), VMManager::GetDiscCRC());
}

static NSString* ARMSX2CompatibilityBuiltInPreset(NSString* title, NSString* serial)
{
    return ARMSX2CompatibilityProfileOff;
}

static NSString* ARMSX2SavedCompatibilityPreset(NSString* identity)
{
    if (!g_p44_settings_interface || identity.length == 0)
        return @"";

    std::string value = g_p44_settings_interface->GetStringValue("ARMSX2/JITBisectGamePresets", identity.UTF8String, "");
    if (value.empty())
        return @"";

    return ARMSX2NormalizeCompatibilityProfile(ARMSX2NSStringFromStdString(value));
}

static NSString* ARMSX2ResolvedCompatibilityPreset(NSString* identity, NSString* title)
{
    if (!g_p44_settings_interface)
        return ARMSX2CompatibilityProfileOff;

    const bool autoPresets = g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", "AutoGamePresets", true);
    if (!autoPresets)
        return ARMSX2CurrentCompatibilityProfileFromSettings();

    NSString* saved = ARMSX2SavedCompatibilityPreset(identity);
    if (saved.length > 0)
        return saved;

    NSString* builtIn = ARMSX2CompatibilityBuiltInPreset(title, identity);
    if (builtIn.length > 0)
        return builtIn;

    return ARMSX2CompatibilityProfileOff;
}

static void ARMSX2ApplyCompatibilityPresetForISOName(NSString* isoName)
{
    NSString* identity = @"";
    NSString* title = isoName.stringByDeletingPathExtension ?: isoName;
    NSString* path = ARMSX2ResolveISOPath(isoName);

    if (path.length > 0) {
        GameList::Entry entry;
        if (GameList::PopulateEntryFromPath(path.UTF8String, &entry)) {
            identity = ARMSX2CompatibilityIdentityKey(ARMSX2NSStringFromStdString(entry.serial), entry.crc);
            title = ARMSX2NSStringFromStdString(entry.GetTitle(false));
            if (title.length == 0)
                title = isoName.stringByDeletingPathExtension ?: isoName;
        }
    }

    const bool autoPresets = g_p44_settings_interface ?
        g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", "AutoGamePresets", true) : false;
    NSString* saved = ARMSX2SavedCompatibilityPreset(identity);
    NSString* builtIn = ARMSX2CompatibilityBuiltInPreset(title, identity);
    NSString* profile = ARMSX2ResolvedCompatibilityPreset(identity, title);
    std::fprintf(stderr,
        "@@IOS_JIT_PRESET_RESOLVE@@ iso=\"%s\" path=\"%s\" identity=\"%s\" title=\"%s\" auto=%d saved=\"%s\" builtin=\"%s\" profile=\"%s\"\n",
        isoName ? isoName.UTF8String : "", path ? path.UTF8String : "", identity ? identity.UTF8String : "",
        title ? title.UTF8String : "", autoPresets ? 1 : 0, saved ? saved.UTF8String : "",
        builtIn ? builtIn.UTF8String : "", profile ? profile.UTF8String : "");
    std::fflush(stderr);
    if ([profile isEqualToString:ARMSX2CompatibilityProfileCustom]) {
        if (ARMSX2LoadCompatibilityCustomFlagsForIdentity(identity)) {
            NSLog(@"[ARMSX2Bridge] Compatibility preset=custom identity=%@ reason=boot %@", identity ?: @"", title ?: @"");
            std::fprintf(stderr,
                "@@IOS_JIT_PROFILE_APPLY@@ profile=custom reason=\"boot %s %s\" persisted=1 flags_preserved=0 loaded_custom=1\n",
                identity ? identity.UTF8String : "", title ? title.UTF8String : "");
            std::fflush(stderr);
            return;
        }

        if (builtIn.length > 0) {
            g_p44_settings_interface->DeleteValue("ARMSX2/JITBisectGamePresets", identity.UTF8String);
            ARMSX2ClearCompatibilityCustomFlagsForIdentity(identity);
            profile = builtIn;
            std::fprintf(stderr,
                "@@IOS_JIT_PROFILE_STALE_CUSTOM_IGNORED@@ identity=\"%s\" title=\"%s\" fallback=\"%s\"\n",
                identity ? identity.UTF8String : "", title ? title.UTF8String : "", profile.UTF8String);
            std::fflush(stderr);
        } else {
            profile = ARMSX2CompatibilityProfileOff;
        }
    }
    ARMSX2ApplyCompatibilityProfile(profile, YES, [NSString stringWithFormat:@"boot %@ %@", identity ?: @"", title ?: @""]);
}

static NSString* ARMSX2SanitizedMemoryCardName(NSString* name)
{
    NSString* trimmed = [name stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (trimmed.length == 0)
        return @"";

    NSMutableString* sanitized = [NSMutableString stringWithCapacity:trimmed.length];
    NSCharacterSet* invalid = [NSCharacterSet characterSetWithCharactersInString:@"/\\:?%*|\"<>"];
    for (NSUInteger i = 0; i < trimmed.length; i++) {
        unichar ch = [trimmed characterAtIndex:i];
        [sanitized appendString:[invalid characterIsMember:ch] ? @"_" : [NSString stringWithCharacters:&ch length:1]];
    }

    while ([sanitized containsString:@".."])
        [sanitized replaceOccurrencesOfString:@".." withString:@"_" options:0 range:NSMakeRange(0, sanitized.length)];

    if (sanitized.pathExtension.length == 0)
        [sanitized appendString:@".ps2"];

    return sanitized;
}

static MemoryCardFileType ARMSX2MemoryCardFileTypeForSizeMB(NSInteger sizeMB)
{
    switch (sizeMB) {
    case 8:
        return MemoryCardFileType::PS2_8MB;
    case 16:
        return MemoryCardFileType::PS2_16MB;
    case 32:
        return MemoryCardFileType::PS2_32MB;
    case 64:
        return MemoryCardFileType::PS2_64MB;
    default:
        return MemoryCardFileType::Unknown;
    }
}

static NSData* ARMSX2ReadSaveStatePreviewPNG(const std::string& path)
{
    if (path.empty())
        return nil;

    zip_error_t ze = {};
    auto zf = zip_open_managed(path.c_str(), ZIP_RDONLY, &ze);
    if (!zf)
        return nil;

    auto zff = zip_fopen_managed(zf.get(), "Screenshot.png", 0);
    if (!zff)
        return nil;

    std::optional<std::vector<u8>> data = ReadBinaryFileInZip(zff.get());
    if (!data.has_value() || data->empty())
        return nil;

    return [NSData dataWithBytes:data->data() length:data->size()];
}

static dispatch_queue_t ARMSX2SaveStateQueue()
{
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("org.armsx2.ios.savestates", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

static NSString* ARMSX2SanitizedBackupPathComponent(NSString* value)
{
    if (value.length == 0)
        return @"unknown";

    NSMutableString* sanitized = [NSMutableString stringWithCapacity:value.length];
    NSCharacterSet* invalid = [NSCharacterSet characterSetWithCharactersInString:@"/\\:?%*|\"<> "];
    for (NSUInteger i = 0; i < value.length; i++) {
        const unichar ch = [value characterAtIndex:i];
        [sanitized appendString:[invalid characterIsMember:ch] ? @"_" : [NSString stringWithCharacters:&ch length:1]];
    }

    return sanitized.length > 0 ? sanitized : @"unknown";
}

static NSString* ARMSX2MemcardBackupRoot()
{
    const std::string root = Path::Combine(EmuFolders::DataRoot, "memcard-state-backups");
    return ARMSX2NSStringFromStdString(root);
}

static void ARMSX2PruneOldMemcardBackups(NSString* backupRoot, NSUInteger keepCount)
{
    if (backupRoot.length == 0 || keepCount == 0)
        return;

    NSFileManager* fm = [NSFileManager defaultManager];
    NSArray<NSURL*>* entries = [fm contentsOfDirectoryAtURL:[NSURL fileURLWithPath:backupRoot]
                                includingPropertiesForKeys:@[NSURLContentModificationDateKey]
                                                   options:NSDirectoryEnumerationSkipsHiddenFiles
                                                     error:nil];
    if (entries.count <= keepCount)
        return;

    NSArray<NSURL*>* sorted = [entries sortedArrayUsingComparator:^NSComparisonResult(NSURL* lhs, NSURL* rhs) {
        NSDate* leftDate = nil;
        NSDate* rightDate = nil;
        [lhs getResourceValue:&leftDate forKey:NSURLContentModificationDateKey error:nil];
        [rhs getResourceValue:&rightDate forKey:NSURLContentModificationDateKey error:nil];
        NSDate* lhsDate = leftDate ?: [NSDate distantPast];
        NSDate* rhsDate = rightDate ?: [NSDate distantPast];
        return [rhsDate compare:lhsDate];
    }];

    for (NSUInteger i = keepCount; i < sorted.count; i++) {
        NSError* error = nil;
        if (![fm removeItemAtURL:sorted[i] error:&error]) {
            NSLog(@"[ARMSX2 iOS SaveState] memcard backup prune failed path=%@ error=%@",
                  sorted[i].path, error.localizedDescription ?: @"unknown");
        }
    }
}

static NSInteger ARMSX2BackupAssignedMemoryCards(const char* reason, s32 stateSlot, const std::string& serial, u32 crc)
{
    NSString* backupRoot = ARMSX2MemcardBackupRoot();
    if (backupRoot.length == 0)
        return 0;

    NSFileManager* fm = [NSFileManager defaultManager];
    NSError* mkdirError = nil;
    if (![fm createDirectoryAtPath:backupRoot withIntermediateDirectories:YES attributes:nil error:&mkdirError]) {
        NSLog(@"[ARMSX2 iOS SaveState] memcard backup root failed path=%@ error=%@",
              backupRoot, mkdirError.localizedDescription ?: @"unknown");
        return 0;
    }

    NSString* safeSerial = ARMSX2SanitizedBackupPathComponent(ARMSX2NSStringFromStdString(serial));
    const long long timestamp = static_cast<long long>(llround([[NSDate date] timeIntervalSince1970] * 1000.0));
    NSString* backupDirName = [NSString stringWithFormat:@"%lld-%@-%08X-slot%02d-%s",
                                                        timestamp, safeSerial, crc, stateSlot, reason ? reason : "state"];
    NSString* backupDir = [backupRoot stringByAppendingPathComponent:backupDirName];
    if (![fm createDirectoryAtPath:backupDir withIntermediateDirectories:YES attributes:nil error:&mkdirError]) {
        NSLog(@"[ARMSX2 iOS SaveState] memcard backup directory failed path=%@ error=%@",
              backupDir, mkdirError.localizedDescription ?: @"unknown");
        return 0;
    }

    NSInteger copied = 0;
    constexpr size_t numMemoryCardSlots = sizeof(EmuConfig.Mcd) / sizeof(EmuConfig.Mcd[0]);
    for (size_t i = 0; i < numMemoryCardSlots; i++) {
        if (!EmuConfig.Mcd[i].Enabled || EmuConfig.Mcd[i].Filename.empty())
            continue;

        const std::string source = EmuConfig.FullpathToMcd(static_cast<uint>(i));
        NSString* sourcePath = ARMSX2NSStringFromStdString(source);
        BOOL isDirectory = NO;
        if (sourcePath.length == 0 || ![fm fileExistsAtPath:sourcePath isDirectory:&isDirectory])
            continue;

        NSString* sourceName = ARMSX2SanitizedBackupPathComponent(sourcePath.lastPathComponent);
        NSString* targetName = [NSString stringWithFormat:@"slot%zu-%@", i + 1, sourceName];
        NSString* targetPath = [backupDir stringByAppendingPathComponent:targetName];
        NSError* copyError = nil;
        if ([fm copyItemAtPath:sourcePath toPath:targetPath error:&copyError]) {
            copied++;
            NSLog(@"[ARMSX2 iOS SaveState] memcard backup copied slot=%zu path=%@",
                  i + 1, targetPath);
        } else {
            NSLog(@"[ARMSX2 iOS SaveState] memcard backup failed slot=%zu source=%@ error=%@",
                  i + 1, sourcePath, copyError.localizedDescription ?: @"unknown");
        }
    }

    if (copied == 0) {
        [fm removeItemAtPath:backupDir error:nil];
    } else {
        ARMSX2PruneOldMemcardBackups(backupRoot, 6);
        NSLog(@"[ARMSX2 iOS SaveState] memcard backup complete reason=%s slot=%d copied=%ld dir=%@",
              reason ? reason : "state", stateSlot, static_cast<long>(copied), backupDir);
    }

    return copied;
}

static bool ARMSX2FlushNVRAMAndMemoryCards(const char* reason)
{
    cdvdSaveNVRAM();
    ARMSX2SetLastNVMSaveDate([NSDate date]);

    if (!VMManager::HasValidVM()) {
        NSLog(@"[ARMSX2Bridge] Save-state flush skipped memory cards reason=%s validVM=0",
              reason ? reason : "unknown");
        return true;
    }

    if (MemcardBusy::IsBusy()) {
        NSLog(@"[ARMSX2Bridge] Save-state flush blocked reason=%s memoryCardBusy=1",
              reason ? reason : "unknown");
        return false;
    }

    FileMcd_EmuClose();
    FileMcd_EmuOpen();
    NSLog(@"[ARMSX2Bridge] Save-state flush complete reason=%s nvmDate=%@",
          reason ? reason : "unknown", s_lastNVMSaveDate);
    return true;
}

static BOOL ARMSX2IsControllerSkinImageName(NSString* name)
{
    NSString* ext = name.pathExtension.lowercaseString;
    return [ext isEqualToString:@"png"] || [ext isEqualToString:@"jpg"] ||
           [ext isEqualToString:@"jpeg"] || [ext isEqualToString:@"webp"];
}

static NSString* ARMSX2ControllerSkinJSONImportKey(NSString* name)
{
    NSString* last = name.lastPathComponent.lowercaseString;
    if (last.length == 0 || ![last.pathExtension.lowercaseString isEqualToString:@"json"])
        return nil;

    return last;
}

static BOOL ARMSX2IsControllerSkinImportName(NSString* name, NSSet<NSString*>* allowedJSONNames)
{
    if (ARMSX2IsControllerSkinImageName(name))
        return YES;

    NSString* key = ARMSX2ControllerSkinJSONImportKey(name);
    return key.length > 0 && [allowedJSONNames containsObject:key];
}

// Skin authors hand-edit manifests and a raw tab inside a string is enough to
// fail every JSON parser. Substituting a space keeps the length, and no byte
// below 0x20 can be a UTF-8 continuation byte or part of a "\t" pair, so
// multi-byte text and real escapes come through untouched.
//
// Swift has to do the same thing after extraction, so there is a second copy in
// SkinManifestImporter.repairedJSON. Change one, change the other.
static NSData* ARMSX2RepairedJSONData(NSData* data)
{
    NSMutableData* repaired = [data mutableCopy];
    uint8_t* bytes = static_cast<uint8_t*>(repaired.mutableBytes);
    const NSUInteger length = repaired.length;
    BOOL inString = NO;
    BOOL escaped = NO;
    BOOL changed = NO;

    for (NSUInteger i = 0; i < length; i++) {
        const uint8_t byte = bytes[i];
        if (!inString) {
            if (byte == 0x22)
                inString = YES;
            continue;
        }

        if (escaped)
            escaped = NO;
        else if (byte == 0x5C)
            escaped = YES;
        else if (byte == 0x22)
            inString = NO;
        else if (byte < 0x20) {
            bytes[i] = 0x20;
            changed = YES;
        }
    }
    return changed ? repaired : nil;
}

static NSMutableSet<NSString*>* ARMSX2AllowedControllerSkinJSONNames(zip_t* zf, zip_int64_t count)
{
    static const zip_uint64_t kMaxLooseLayoutBytes = 1024 * 1024;
    static const NSUInteger kMaxLooseLayoutEntries = 8;

    NSMutableSet<NSString*>* allowedJSONNames = [NSMutableSet setWithObject:@"manifest.json"];
    NSMutableSet<NSString*>* namedLayoutKeys = [NSMutableSet set];
    const zip_uint64_t entryCount = static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0));
    for (zip_uint64_t i = 0; i < entryCount; i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf, i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;

        NSString* entryName = [NSString stringWithUTF8String:stat.name];
        if ([entryName containsString:@"__MACOSX"] || [entryName.lastPathComponent hasPrefix:@"."])
            continue;
        if (![ARMSX2ControllerSkinJSONImportKey(entryName) isEqualToString:@"manifest.json"])
            continue;

        auto file = zip_fopen_index_managed(zf, i, ZIP_FL_ENC_GUESS);
        if (!file)
            continue;

        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->empty())
            continue;

        NSData* manifestData = [NSData dataWithBytes:data->data() length:data->size()];
        id manifestObject = [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:nil];
        if (![manifestObject isKindOfClass:NSDictionary.class]) {
            NSData* repaired = ARMSX2RepairedJSONData(manifestData);
            manifestObject = repaired ? [NSJSONSerialization JSONObjectWithData:repaired options:0 error:nil] : nil;
            if (![manifestObject isKindOfClass:NSDictionary.class])
                continue;
        }

        id layoutValue = [(NSDictionary*)manifestObject objectForKey:@"layout"];
        if (![layoutValue isKindOfClass:NSString.class])
            continue;

        NSString* layoutKey = ARMSX2ControllerSkinJSONImportKey((NSString*)layoutValue);
        if (layoutKey.length > 0) {
            [allowedJSONNames addObject:layoutKey];
            [namedLayoutKeys addObject:layoutKey];
        }
    }

    // Naming a layout is not the same as shipping one. If the named file is really
    // in there we are done; if it is not, fall through and let the loose pass find
    // whatever the author actually shipped.
    for (zip_uint64_t i = 0; i < entryCount && namedLayoutKeys.count > 0; i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf, i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;
        NSString* entryName = [NSString stringWithUTF8String:stat.name];
        if ([entryName containsString:@"__MACOSX"] || [entryName.lastPathComponent hasPrefix:@"."])
            continue;
        if ([namedLayoutKeys containsObject:ARMSX2ControllerSkinJSONImportKey(entryName)])
            return allowedJSONNames;
    }

    // Nothing named, nothing readable to name it, or the named file is absent. Let
    // the other jsons through so Swift can work out which one is the layout, but
    // keep it bounded: too many candidates and it has no way to choose.
    NSSet<NSString*>* manifestKeys = [NSSet setWithArray:@[@"manifest.json", @"info.json", @"manifest-v2.json"]];
    NSUInteger looseCount = 0;
    for (zip_uint64_t i = 0; i < entryCount && looseCount < kMaxLooseLayoutEntries; i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf, i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;
        if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxLooseLayoutBytes)
            continue;

        NSString* entryName = [NSString stringWithUTF8String:stat.name];
        if ([entryName containsString:@"__MACOSX"] || [entryName.lastPathComponent hasPrefix:@"."])
            continue;

        NSString* key = ARMSX2ControllerSkinJSONImportKey(entryName);
        if (key.length == 0 || [manifestKeys containsObject:key] || [allowedJSONNames containsObject:key])
            continue;

        [allowedJSONNames addObject:key];
        looseCount++;
    }
    return allowedJSONNames;
}

static NSString* ARMSX2SanitizedSkinFileName(NSString* name)
{
    NSString* last = name.lastPathComponent;
    if (last.length == 0)
        return nil;

    NSMutableString* sanitized = [NSMutableString stringWithCapacity:last.length];
    NSCharacterSet* allowed = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"];
    for (NSUInteger i = 0; i < last.length; i++) {
        unichar ch = [last characterAtIndex:i];
        [sanitized appendString:[allowed characterIsMember:ch] ? [NSString stringWithCharacters:&ch length:1] : @"_"];
    }
    return sanitized;
}

static void ARMSX2ApplyLiveTargetSpeedSetting(std::function<void()> update, const char* section, const char* key, float value)
{
    const std::string sectionName(section ? section : "");
    const std::string keyName(key ? key : "");

    if (!VMManager::HasValidVM()) {
        update();
        NSLog(@"[ARMSX2Bridge] target speed setting stored for next boot %s/%s=%0.3f",
              sectionName.c_str(), keyName.c_str(), value);
        return;
    }

    Host::RunOnCPUThread([update = std::move(update), sectionName, keyName, value]() mutable {
        update();
        VMManager::UpdateTargetSpeed();
        NSLog(@"[ARMSX2Bridge] target speed updated on CPU thread %s/%s=%0.3f",
              sectionName.c_str(), keyName.c_str(), value);
    }, false);
}

static float ARMSX2NormalizeIOSNominalScalar(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.05f, 10.0f) : 1.0f;
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

    const float clamped = std::isfinite(value) ? std::clamp(value, 0.05f, 10.0f) : 1.0f;
    if (std::strcmp(key, "NominalScalar") == 0) {
        const float normalized = ARMSX2NormalizeIOSNominalScalar(value);
        if (std::fabs(normalized - clamped) > 0.001f)
            NSLog(@"[ARMSX2Bridge] clamping unsupported NominalScalar %.3f -> %.3f", clamped, normalized);
        ARMSX2ApplyLiveTargetSpeedSetting([normalized]() { EmuConfig.EmulationSpeed.NominalScalar = normalized; }, section, key, normalized);
    } else if (std::strcmp(key, "TurboScalar") == 0) {
        ARMSX2ApplyLiveTargetSpeedSetting([clamped]() { EmuConfig.EmulationSpeed.TurboScalar = clamped; }, section, key, clamped);
    } else if (std::strcmp(key, "SlomoScalar") == 0) {
        ARMSX2ApplyLiveTargetSpeedSetting([clamped]() { EmuConfig.EmulationSpeed.SlomoScalar = clamped; }, section, key, clamped);
    }
}

// Builds the per-game settings dictionary seeded with the current global values.
static NSMutableDictionary<NSString*, id>* ARMSX2BuildGlobalGameSettingsResult()
{
    const float globalUpscale = g_p44_settings_interface ? g_p44_settings_interface->GetFloatValue("EmuCore/GS", "upscale_multiplier", 1.0f) : 1.0f;
    const std::string globalAspect = g_p44_settings_interface ? g_p44_settings_interface->GetStringValue("EmuCore/GS", "AspectRatio", "Auto 4:3/3:2") : std::string("Auto 4:3/3:2");
    const int globalTextureFiltering = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "filter", 2) : 2;
    const bool globalHardwareMipmapping = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore/GS", "hw_mipmap", true) : true;
    const int globalBlendingAccuracy = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "accurate_blending_unit", 1) : 1;
    // 0 is GSInterlaceMode::Automatic. Not 7, whatever the old picker labelled it.
    const int globalInterlaceMode = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "deinterlace_mode", 0) : 0;
    const int globalTrilinearFiltering = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "TriFilter", -1) : -1;
    const int globalHalfPixelOffset = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", 0) : 0;
    const int globalRoundSprite = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "UserHacks_round_sprite_offset", 0) : 0;
    const bool globalAlignSprite = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore/GS", "UserHacks_align_sprite_X", false) : false;
    const bool globalMergeSprite = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore/GS", "UserHacks_merge_pp_sprite", false) : false;
    const bool globalWildArmsOffset = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", false) : false;
    const int globalTextureOffsetX = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "UserHacks_TCOffsetX", 0) : 0;
    const int globalTextureOffsetY = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "UserHacks_TCOffsetY", 0) : 0;
    const int globalSkipDrawStart = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "UserHacks_SkipDraw_Start", 0) : 0;
    const int globalSkipDrawEnd = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "UserHacks_SkipDraw_End", 0) : 0;
    const bool globalEnableCheats = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore", "EnableCheats", false) : false;
    const bool globalEnablePatches = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore", "EnablePatches", true) : true;
    const bool globalEnableGameFixes = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore", "EnableGameFixes", true) : true;
    const bool globalEnableGameDBHardwareFixes = g_p44_settings_interface ? !g_p44_settings_interface->GetBoolValue("EmuCore/GS", "UserHacks", false) : true;
    const int globalEECoreType = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/CPU", "CoreType", 2) : 2;
    const bool globalMTVU = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore/Speedhacks", "vuThread", true) : true;
    const int globalEECycleRate = ARMSX2ClampInt(
        g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/Speedhacks", "EECycleRate", 0) : 0,
        -3,
        3);
    const bool globalFastBoot = g_p44_settings_interface ?
        g_p44_settings_interface->GetBoolValue(
            "EmuCore", "EnableFastBoot",
            g_p44_settings_interface->GetBoolValue("GameISO", "FastBoot", false)) : false;
    const int globalVolumePercent = ARMSX2ClampInt(
        g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("SPU2/Output", "StandardVolume", ARMSX2DefaultAudioVolumePercent) : ARMSX2DefaultAudioVolumePercent,
        0,
        ARMSX2DefaultAudioVolumePercent);
    return [@{
        @"enabled": @NO,
        @"path": @"",
        @"serial": @"",
        @"crc": @"",
        // has*Override so an untouched setting writes nothing.
        @"upscaleMultiplier": @(globalUpscale),
        @"hasUpscaleMultiplierOverride": @NO,
        @"aspectRatio": ARMSX2NSStringFromStdString(globalAspect),
        @"hasAspectRatioOverride": @NO,
        @"textureFiltering": @(globalTextureFiltering),
        @"hasTextureFilteringOverride": @NO,
        @"hardwareMipmapping": @(globalHardwareMipmapping),
        @"hasHardwareMipmappingOverride": @NO,
        @"blendingAccuracy": @(globalBlendingAccuracy),
        @"hasBlendingAccuracyOverride": @NO,
        @"interlaceMode": @(globalInterlaceMode),
        @"hasInterlaceModeOverride": @NO,
        @"trilinearFiltering": @(globalTrilinearFiltering),
        @"hasTrilinearFilteringOverride": @NO,
        @"halfPixelOffset": @(globalHalfPixelOffset),
        @"hasHalfPixelOffsetOverride": @NO,
        @"roundSprite": @(globalRoundSprite),
        @"hasRoundSpriteOverride": @NO,
        @"alignSprite": @(globalAlignSprite),
        @"hasAlignSpriteOverride": @NO,
        @"mergeSprite": @(globalMergeSprite),
        @"hasMergeSpriteOverride": @NO,
        @"wildArmsOffset": @(globalWildArmsOffset),
        @"hasWildArmsOffsetOverride": @NO,
        @"textureOffsetX": @(ARMSX2ClampInt(globalTextureOffsetX, -4096, 4096)),
        @"hasTextureOffsetXOverride": @NO,
        @"textureOffsetY": @(ARMSX2ClampInt(globalTextureOffsetY, -4096, 4096)),
        @"hasTextureOffsetYOverride": @NO,
        @"skipDrawStart": @(ARMSX2ClampInt(globalSkipDrawStart, 0, 5000)),
        @"hasSkipDrawStartOverride": @NO,
        @"skipDrawEnd": @(ARMSX2ClampInt(globalSkipDrawEnd, 0, 5000)),
        @"hasSkipDrawEndOverride": @NO,
        @"enableCheats": @(globalEnableCheats),
        @"enablePatches": @(globalEnablePatches),
        @"enableGameFixes": @(globalEnableGameFixes),
        @"enableGameDBHardwareFixes": @(globalEnableGameDBHardwareFixes),
        @"eeCoreType": @(globalEECoreType),
        @"mtvu": @(globalMTVU),
        @"globalEECycleRate": @(globalEECycleRate),
        @"eeCycleRate": @(globalEECycleRate),
        @"hasEECycleRateOverride": @NO,
        @"globalFastBoot": @(globalFastBoot),
        @"fastBoot": @(globalFastBoot),
        @"hasFastBootOverride": @NO,
        @"globalVolumePercent": @(globalVolumePercent),
        @"volumePercent": @(globalVolumePercent),
        @"hasVolumeOverride": @NO,
    } mutableCopy];
}

// Overlays per-game INI overrides for the given serial/crc onto a globals-seeded result.
// Sourcing serial/crc from the caller avoids re-scanning the disc image (which is unsafe
// while the VM is actively reading the same disc).
// Every pin-backed hack key with its claim bit, so a file's mask is derivable
// from which of these keys it holds rather than stored ahead of them.
static constexpr struct { const char* key; GSUserHackOverride hack; } s_pinned_hack_keys[] = {
    {"UserHacks_align_sprite_X", GSUserHackOverride::AlignSprite},
    {"UserHacks_merge_pp_sprite", GSUserHackOverride::MergeSprite},
    {"UserHacks_round_sprite_offset", GSUserHackOverride::RoundSprite},
    {"UserHacks_HalfPixelOffset", GSUserHackOverride::HalfPixelOffset},
    {"UserHacks_ForceEvenSpritePosition", GSUserHackOverride::ForceEvenSpritePosition},
    {"UserHacks_native_scaling", GSUserHackOverride::NativeScaling},
    {"UserHacks_NativePaletteDraw", GSUserHackOverride::NativePaletteDraw},
    {"UserHacks_BilinearHack", GSUserHackOverride::BilinearHack},
    {"UserHacks_TCOffsetX", GSUserHackOverride::TextureOffsetX},
    {"UserHacks_AutoFlushLevel", GSUserHackOverride::AutoFlush},
    {"UserHacks_TextureInsideRt", GSUserHackOverride::TextureInsideRt},
    {"UserHacks_TCOffsetY", GSUserHackOverride::TextureOffsetY},
    {"preload_frame_with_gs_data", GSUserHackOverride::PreloadFrameData},
    {"UserHacks_DisablePartialInvalidation", GSUserHackOverride::DisablePartialInvalidation},
    {"paltex", GSUserHackOverride::GPUPaletteConversion},
    {"UserHacks_DisableDepthSupport", GSUserHackOverride::DisableDepthSupport},
    {"UserHacks_CPU_FB_Conversion", GSUserHackOverride::CPUFBConversion},
    {"UserHacks_ReadTCOnClose", GSUserHackOverride::ReadTCOnClose},
    {"UserHacks_Limit24BitDepth", GSUserHackOverride::Limit24BitDepth},
    {"UserHacks_EstimateTextureRegion", GSUserHackOverride::EstimateTextureRegion},
    {"UserHacks_DrawBuffering", GSUserHackOverride::DrawBuffering},
    {"UserHacks_CPUSpriteRenderBW", GSUserHackOverride::CPUSpriteRenderBW},
    {"UserHacks_CPUSpriteRenderLevel", GSUserHackOverride::CPUSpriteRenderLevel},
    {"UserHacks_CPUCLUTRender", GSUserHackOverride::CPUCLUTRender},
    {"UserHacks_GPUTargetCLUTMode", GSUserHackOverride::GPUTargetCLUT},
};

static u32 ARMSX2DerivePerGameHackClaims(INISettingsInterface& si)
{
    u32 claims = 0;
    for (const auto& entry : s_pinned_hack_keys) {
        if (si.ContainsValue("EmuCore/GS", entry.key))
            claims |= 1u << static_cast<u32>(entry.hack);
    }
    return claims;
}

static void ARMSX2StoreDerivedPerGameHackClaims(INISettingsInterface& si)
{
    const u32 claims = ARMSX2DerivePerGameHackClaims(si);
    if (claims != 0)
        si.SetIntValue("EmuCore/GS", "UserHackOverrides", static_cast<int>(claims));
    else
        si.DeleteValue("EmuCore/GS", "UserHackOverrides");
}

// The generic per-game helpers write hack keys too, so they keep the mask in step.
static void ARMSX2SyncClaimsIfPinnedHackKey(INISettingsInterface& si, NSString* section, NSString* key)
{
    if (![section isEqualToString:@"EmuCore/GS"])
        return;
    for (const auto& entry : s_pinned_hack_keys) {
        if ([key isEqualToString:@(entry.key)]) {
            ARMSX2StoreDerivedPerGameHackClaims(si);
            return;
        }
    }
}

static void ARMSX2ApplyPerGameSettingsOverrides(NSMutableDictionary<NSString*, id>* result, const std::string& serial, u32 crc)
{
    const std::string settingsPath = VMManager::GetGameSettingsPath(serial, crc);
    result[@"path"] = ARMSX2NSStringFromStdString(settingsPath);
    result[@"serial"] = ARMSX2NSStringFromStdString(serial);
    result[@"crc"] = [NSString stringWithFormat:@"%08X", crc];

    INISettingsInterface si(settingsPath);
    if (!si.Load())
        return;

    if (si.ContainsValue("EmuCore/Speedhacks", "vuThread") &&
        !si.GetBoolValue("EmuCore/Speedhacks", "vuThread", true) &&
        (!si.GetBoolValue("ARMSX2iOS/PerGame", "ManualMTVU", false) ||
            si.GetIntValue("ARMSX2iOS/PerGame", "ManualMTVUVersion", 0) < 3)) {
        si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVU");
        si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVUVersion");
        si.DeleteValue("EmuCore/Speedhacks", "vuThread");
        Error saveError;
        const bool saved = si.Save(&saveError);
        std::fprintf(stderr, "@@IOS_PERGAME_MTVU_REPAIR@@ file=\"%s\" ui_read=1 removed_stale_false=1 saved=%d error=\"%s\"\n",
            settingsPath.c_str(), saved ? 1 : 0, saveError.GetDescription().c_str());
        std::fflush(stderr);
    }

    // Older saves froze the global claim mask into this file, where it went stale.
    const u32 derivedClaims = ARMSX2DerivePerGameHackClaims(si);
    const u32 storedClaims = static_cast<u32>(si.GetIntValue("EmuCore/GS", "UserHackOverrides", 0));
    if (storedClaims != derivedClaims) {
        ARMSX2StoreDerivedPerGameHackClaims(si);
        si.RemoveEmptySections();
        Error claimError;
        const bool saved = si.Save(&claimError);
        std::fprintf(stderr, "@@IOS_PERGAME_CLAIM_REPAIR@@ file=\"%s\" stored=%u derived=%u saved=%d\n",
            settingsPath.c_str(), storedClaims, derivedClaims, saved ? 1 : 0);
        std::fflush(stderr);
    }

    const bool hasKnownOverride =
        si.GetBoolValue("ARMSX2iOS/PerGame", "Enabled", false) ||
        si.ContainsValue("EmuCore/GS", "upscale_multiplier") ||
        si.ContainsValue("EmuCore/GS", "AspectRatio") ||
        si.ContainsValue("EmuCore/GS", "filter") ||
        si.ContainsValue("EmuCore/GS", "hw_mipmap") ||
        si.ContainsValue("EmuCore/GS", "accurate_blending_unit") ||
        si.ContainsValue("EmuCore/GS", "deinterlace_mode") ||
        si.ContainsValue("EmuCore/GS", "TriFilter") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_HalfPixelOffset") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_round_sprite_offset") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_align_sprite_X") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_merge_pp_sprite") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_DisableDepthSupport") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_TCOffsetX") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_TCOffsetY") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_SkipDraw_Start") ||
        si.ContainsValue("EmuCore/GS", "UserHacks_SkipDraw_End") ||
        si.ContainsValue("EmuCore", "EnableCheats") ||
        si.ContainsValue("EmuCore", "EnablePatches") ||
        si.ContainsValue("EmuCore", "EnableGameFixes") ||
        si.ContainsValue("EmuCore/GS", "UserHacks") ||
        si.ContainsValue("EmuCore/CPU", "CoreType") ||
        si.ContainsValue("EmuCore/CPU", "UseArm64Dynarec") ||
        si.ContainsValue("EmuCore/Speedhacks", "vuThread") ||
        si.ContainsValue("EmuCore/Speedhacks", "EECycleRate") ||
        si.ContainsValue("EmuCore", "EnableFastBoot") ||
        si.ContainsValue("SPU2/Output", "StandardVolume") ||
        si.ContainsValue("SPU2/Output", "FastForwardVolume") ||
        si.ContainsValue("ARMSX2iOS/UI", "InvertLeftStickX") ||
        si.ContainsValue("ARMSX2iOS/UI", "InvertLeftStickY") ||
        si.ContainsValue("ARMSX2iOS/UI", "InvertRightStickX") ||
        si.ContainsValue("ARMSX2iOS/UI", "InvertRightStickY");

    result[@"enabled"] = @(hasKnownOverride);
    // StandardVolume only. Falling back to FastForwardVolume made the main slider show an
    // override nobody set.
    const bool hasVolumeOverride = si.ContainsValue("SPU2/Output", "StandardVolume");
    const int inheritedVolumePercent = [result[@"volumePercent"] intValue];
    const int volumePercent = hasVolumeOverride ?
        si.GetIntValue("SPU2/Output", "StandardVolume", inheritedVolumePercent) :
        inheritedVolumePercent;
    result[@"hasVolumeOverride"] = @(hasVolumeOverride);
    result[@"volumePercent"] = @(ARMSX2ClampInt(volumePercent, 0, ARMSX2DefaultAudioVolumePercent));
    NSString* currentAspect = [result[@"aspectRatio"] isKindOfClass:NSString.class] ? result[@"aspectRatio"] : @"Auto 4:3/3:2";
    result[@"hasUpscaleMultiplierOverride"] = @(si.ContainsValue("EmuCore/GS", "upscale_multiplier"));
    result[@"upscaleMultiplier"] = @(si.GetFloatValue("EmuCore/GS", "upscale_multiplier", [result[@"upscaleMultiplier"] floatValue]));
    result[@"hasAspectRatioOverride"] = @(si.ContainsValue("EmuCore/GS", "AspectRatio"));
    result[@"aspectRatio"] = ARMSX2NSStringFromStdString(si.GetStringValue("EmuCore/GS", "AspectRatio", currentAspect.UTF8String));
    result[@"hasTextureFilteringOverride"] = @(si.ContainsValue("EmuCore/GS", "filter"));
    result[@"textureFiltering"] = @(si.GetIntValue("EmuCore/GS", "filter", [result[@"textureFiltering"] intValue]));
    result[@"hasHardwareMipmappingOverride"] = @(si.ContainsValue("EmuCore/GS", "hw_mipmap"));
    result[@"hardwareMipmapping"] = @(si.GetBoolValue("EmuCore/GS", "hw_mipmap", [result[@"hardwareMipmapping"] boolValue]));
    result[@"hasBlendingAccuracyOverride"] = @(si.ContainsValue("EmuCore/GS", "accurate_blending_unit"));
    result[@"blendingAccuracy"] = @(si.GetIntValue("EmuCore/GS", "accurate_blending_unit", [result[@"blendingAccuracy"] intValue]));
    result[@"hasInterlaceModeOverride"] = @(si.ContainsValue("EmuCore/GS", "deinterlace_mode"));
    result[@"interlaceMode"] = @(si.GetIntValue("EmuCore/GS", "deinterlace_mode", [result[@"interlaceMode"] intValue]));
    result[@"hasTrilinearFilteringOverride"] = @(si.ContainsValue("EmuCore/GS", "TriFilter"));
    result[@"trilinearFiltering"] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "TriFilter", [result[@"trilinearFiltering"] intValue]), -1, 2));
    result[@"hasHalfPixelOffsetOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_HalfPixelOffset"));
    result[@"halfPixelOffset"] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", [result[@"halfPixelOffset"] intValue]), 0, 5));
    result[@"hasRoundSpriteOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_round_sprite_offset"));
    result[@"roundSprite"] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_round_sprite_offset", [result[@"roundSprite"] intValue]), 0, 2));
    result[@"hasAlignSpriteOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_align_sprite_X"));
    result[@"alignSprite"] = @(si.GetBoolValue("EmuCore/GS", "UserHacks_align_sprite_X", [result[@"alignSprite"] boolValue]));
    result[@"hasMergeSpriteOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_merge_pp_sprite"));
    result[@"mergeSprite"] = @(si.GetBoolValue("EmuCore/GS", "UserHacks_merge_pp_sprite", [result[@"mergeSprite"] boolValue]));
    result[@"hasWildArmsOffsetOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition"));
    result[@"wildArmsOffset"] = @(si.GetBoolValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", [result[@"wildArmsOffset"] boolValue]));
    result[@"hasTextureOffsetXOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_TCOffsetX"));
    result[@"textureOffsetX"] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_TCOffsetX", [result[@"textureOffsetX"] intValue]), -4096, 4096));
    result[@"hasTextureOffsetYOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_TCOffsetY"));
    result[@"textureOffsetY"] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_TCOffsetY", [result[@"textureOffsetY"] intValue]), -4096, 4096));
    result[@"hasSkipDrawStartOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_SkipDraw_Start"));
    result[@"skipDrawStart"] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_SkipDraw_Start", [result[@"skipDrawStart"] intValue]), 0, 5000));
    result[@"hasSkipDrawEndOverride"] = @(si.ContainsValue("EmuCore/GS", "UserHacks_SkipDraw_End"));
    result[@"skipDrawEnd"] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/GS", "UserHacks_SkipDraw_End", [result[@"skipDrawEnd"] intValue]), 0, 5000));
    result[@"enableCheats"] = @(si.GetBoolValue("EmuCore", "EnableCheats", [result[@"enableCheats"] boolValue]));
    result[@"enablePatches"] = @(si.GetBoolValue("EmuCore", "EnablePatches", [result[@"enablePatches"] boolValue]));
    result[@"enableGameFixes"] = @(si.GetBoolValue("EmuCore", "EnableGameFixes", [result[@"enableGameFixes"] boolValue]));
    result[@"enableGameDBHardwareFixes"] = @(!si.GetBoolValue("EmuCore/GS", "UserHacks", ![result[@"enableGameDBHardwareFixes"] boolValue]));
    result[@"eeCoreType"] = @(si.GetIntValue("EmuCore/CPU", "CoreType", [result[@"eeCoreType"] intValue]));
    result[@"mtvu"] = @(si.GetBoolValue("EmuCore/Speedhacks", "vuThread", [result[@"mtvu"] boolValue]));
    result[@"hasEECycleRateOverride"] = @(si.ContainsValue("EmuCore/Speedhacks", "EECycleRate"));
    result[@"eeCycleRate"] = @(ARMSX2ClampInt(si.GetIntValue("EmuCore/Speedhacks", "EECycleRate", [result[@"eeCycleRate"] intValue]), -3, 3));
    result[@"hasFastBootOverride"] = @(si.ContainsValue("EmuCore", "EnableFastBoot"));
    result[@"fastBoot"] = @(si.GetBoolValue("EmuCore", "EnableFastBoot", [result[@"fastBoot"] boolValue]));

    // Per-game compatibility overrides. Returning these from the single INI that is
    // already loaded here lets the settings panel open without re-parsing the file
    // once per override key. Only present overrides are included; absent keys fall
    // back to the global value on the caller side.
    {
        NSMutableDictionary<NSString*, NSNumber*>* perGameFixes = [NSMutableDictionary dictionary];
        static constexpr const char* kARMSX2GameFixKeys[] = {
            "VuAddSubHack", "XgKickHack", "EETimingHack", "InstantDMAHack",
            "SoftwareRendererFMVHack", "SkipMPEGHack", "OPHFlagHack", "DMABusyHack",
            "VIF1StallHack", "GIFFIFOHack", "GoemonTlbHack", "IbitHack", "VUSyncHack",
            "VUOverflowHack", "BlitInternalFPSHack", "FullVU0SyncHack"
        };
        for (const char* gameFixKey : kARMSX2GameFixKeys)
        {
            if (si.ContainsValue("EmuCore/Gamefixes", gameFixKey))
            {
                perGameFixes[[NSString stringWithUTF8String:gameFixKey]] =
                    @(si.GetBoolValue("EmuCore/Gamefixes", gameFixKey, false) ? 1 : 0);
            }
        }
        result[@"perGameFixes"] = perGameFixes;

        const bool hasPerGameAAT = si.ContainsValue("EmuCore/GS", "HWAccurateAlphaTest");
        result[@"hasPerGameAAT"] = @(hasPerGameAAT);
        result[@"perGameAAT"] = @((hasPerGameAAT && si.GetBoolValue("EmuCore/GS", "HWAccurateAlphaTest", false)) ? 1 : 0);

        const bool hasPerGameTextureInsideRt = si.ContainsValue("EmuCore/GS", "UserHacks_TextureInsideRt");
        result[@"hasPerGameTextureInsideRt"] = @(hasPerGameTextureInsideRt);
        result[@"perGameTextureInsideRt"] =
            @(hasPerGameTextureInsideRt ? si.GetIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", 0) : 0);

        const bool hasPerGameDisableDepth = si.ContainsValue("EmuCore/GS", "UserHacks_DisableDepthSupport");
        result[@"hasPerGameDisableDepth"] = @(hasPerGameDisableDepth);
        result[@"perGameDisableDepth"] =
            @(hasPerGameDisableDepth ? si.GetBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", false) : NO);

        const bool hasPerGameRenderer = si.ContainsValue("EmuCore/GS", "Renderer");
        result[@"hasPerGameRenderer"] = @(hasPerGameRenderer);
        result[@"perGameRenderer"] = @(hasPerGameRenderer ? si.GetIntValue("EmuCore/GS", "Renderer", 17) : 17);

        const bool hasPerGameFXAA = si.ContainsValue("EmuCore/GS", "fxaa");
        result[@"hasPerGameFXAA"] = @(hasPerGameFXAA);
        result[@"perGameFXAA"] = @((hasPerGameFXAA && si.GetBoolValue("EmuCore/GS", "fxaa", false)) ? 1 : 0);

        const bool hasPerGameShadeBoost = si.ContainsValue("EmuCore/GS", "ShadeBoost");
        result[@"hasPerGameShadeBoost"] = @(hasPerGameShadeBoost);
        result[@"perGameShadeBoost"] = @((hasPerGameShadeBoost && si.GetBoolValue("EmuCore/GS", "ShadeBoost", false)) ? 1 : 0);

        const bool hasPerGameTVShader = si.ContainsValue("EmuCore/GS", "TVShader");
        result[@"hasPerGameTVShader"] = @(hasPerGameTVShader);
        result[@"perGameTVShader"] = @(hasPerGameTVShader ? si.GetIntValue("EmuCore/GS", "TVShader", 0) : 0);

        const bool hasPerGameCASMode = si.ContainsValue("EmuCore/GS", "CASMode");
        result[@"hasPerGameCASMode"] = @(hasPerGameCASMode);
        result[@"perGameCASMode"] = @(hasPerGameCASMode ? si.GetIntValue("EmuCore/GS", "CASMode", 0) : 0);

        // MetalFX Spatial upscaler (Off = 0, MetalFXSpatial = 1).
        const bool hasPerGameUpscaler = si.ContainsValue("EmuCore/GS", "Upscaler");
        result[@"hasPerGameUpscaler"] = @(hasPerGameUpscaler);
        result[@"perGameUpscaler"] = @(hasPerGameUpscaler ? si.GetIntValue("EmuCore/GS", "Upscaler", 0) : 0);

        const bool hasPerGameMaxAnisotropy = si.ContainsValue("EmuCore/GS", "MaxAnisotropy");
        result[@"hasPerGameMaxAnisotropy"] = @(hasPerGameMaxAnisotropy);
        result[@"perGameMaxAnisotropy"] = @(hasPerGameMaxAnisotropy ? si.GetIntValue("EmuCore/GS", "MaxAnisotropy", 0) : 0);

        const bool hasPerGameCASSharpness = si.ContainsValue("EmuCore/GS", "CASSharpness");
        result[@"hasPerGameCASSharpness"] = @(hasPerGameCASSharpness);
        result[@"perGameCASSharpness"] = @(hasPerGameCASSharpness ? si.GetIntValue("EmuCore/GS", "CASSharpness", 50) : 50);

        const bool hasPerGamePCRTCOffsets = si.ContainsValue("EmuCore/GS", "pcrtc_offsets");
        result[@"hasPerGamePCRTCOffsets"] = @(hasPerGamePCRTCOffsets);
        result[@"perGamePCRTCOffsets"] = @((hasPerGamePCRTCOffsets && si.GetBoolValue("EmuCore/GS", "pcrtc_offsets", false)) ? 1 : 0);

        const bool hasPerGameIntegerScaling = si.ContainsValue("EmuCore/GS", "IntegerScaling");
        result[@"hasPerGameIntegerScaling"] = @(hasPerGameIntegerScaling);
        result[@"perGameIntegerScaling"] = @((hasPerGameIntegerScaling && si.GetBoolValue("EmuCore/GS", "IntegerScaling", false)) ? 1 : 0);

        const bool hasPerGameSkipDupFrames = si.ContainsValue("EmuCore/GS", "SkipDuplicateFrames");
        result[@"hasPerGameSkipDupFrames"] = @(hasPerGameSkipDupFrames);
        result[@"perGameSkipDupFrames"] = @((hasPerGameSkipDupFrames && si.GetBoolValue("EmuCore/GS", "SkipDuplicateFrames", true)) ? 1 : 0);

        const bool hasPerGamePCRTCOverscan = si.ContainsValue("EmuCore/GS", "pcrtc_overscan");
        result[@"hasPerGamePCRTCOverscan"] = @(hasPerGamePCRTCOverscan);
        result[@"perGamePCRTCOverscan"] = @((hasPerGamePCRTCOverscan && si.GetBoolValue("EmuCore/GS", "pcrtc_overscan", false)) ? 1 : 0);

        const bool hasPerGamePCRTCAntiBlur = si.ContainsValue("EmuCore/GS", "pcrtc_antiblur");
        result[@"hasPerGamePCRTCAntiBlur"] = @(hasPerGamePCRTCAntiBlur);
        result[@"perGamePCRTCAntiBlur"] = @((hasPerGamePCRTCAntiBlur && si.GetBoolValue("EmuCore/GS", "pcrtc_antiblur", true)) ? 1 : 0);

        const bool hasPerGameDisableInterlaceOffset = si.ContainsValue("EmuCore/GS", "disable_interlace_offset");
        result[@"hasPerGameDisableInterlaceOffset"] = @(hasPerGameDisableInterlaceOffset);
        result[@"perGameDisableInterlaceOffset"] = @((hasPerGameDisableInterlaceOffset && si.GetBoolValue("EmuCore/GS", "disable_interlace_offset", false)) ? 1 : 0);
    }
}

// Older builds stamped deinterlace_mode 7 into every per-game file with overrides on. Nobody
// picked that deliberately, Blend BFF was offered as 6, so drop it and let Automatic apply again.
// Exact 7 only.
void ARMSX2MigratePerGameDeinterlaceBlend(SettingsInterface* si)
{
    if (!si || si->GetBoolValue("ARMSX2iOS/Migrations", "PerGameDeinterlaceBlendV1", false))
        return;

    FileSystem::FindResultsArray files;
    FileSystem::FindFiles(EmuFolders::GameSettings.c_str(), "*.ini",
        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &files);

    u32 repaired = 0;
    for (const FILESYSTEM_FIND_DATA& fd : files) {
        INISettingsInterface game_si(fd.FileName);
        if (!game_si.Load())
            continue;
        if (!game_si.ContainsValue("EmuCore/GS", "deinterlace_mode") ||
            game_si.GetIntValue("EmuCore/GS", "deinterlace_mode", 0) != 7)
            continue;

        game_si.DeleteValue("EmuCore/GS", "deinterlace_mode");
        game_si.RemoveEmptySections();
        if (game_si.Save())
            repaired++;
    }

    si->SetBoolValue("ARMSX2iOS/Migrations", "PerGameDeinterlaceBlendV1", true);
    si->Save();
    std::fprintf(stderr, "@@IOS_DEINTERLACE_MIGRATION@@ scanned=%zu repaired=%u\n", files.size(), repaired);
    std::fflush(stderr);
}

static void ARMSX2WriteGameSettingsForIdentity(const std::string& serial,
                                                u32 crc,
                                                BOOL enabled,
                                                float upscaleMultiplier,
                                                NSString* aspectRatio,
                                                int textureFiltering,
                                                int hardwareMipmapping,
                                                int blendingAccuracy,
                                                int interlaceMode,
                                                int trilinearFiltering,
                                                int halfPixelOffset,
                                                int roundSprite,
                                                int alignSprite,
                                                int mergeSprite,
                                                int wildArmsOffset,
                                                BOOL textureOffsetXOverride,
                                                int textureOffsetX,
                                                BOOL textureOffsetYOverride,
                                                int textureOffsetY,
                                                BOOL skipDrawStartOverride,
                                                int skipDrawStart,
                                                BOOL skipDrawEndOverride,
                                                int skipDrawEnd,
                                                BOOL volumeOverride,
                                                int volumePercent,
                                                int eeCoreType,
                                                BOOL mtvu,
                                                BOOL eeCycleRateOverride,
                                                int eeCycleRate,
                                                BOOL fastBootOverride,
                                                BOOL fastBoot,
                                                BOOL enableCheats,
                                                BOOL enablePatches,
                                                BOOL enableGameFixes,
                                                BOOL enableGameDBHardwareFixes)
{
    FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), false);
    if (enableCheats && (ARMSX2RetroAchievementsHardcoreActive() || EmuConfig.Achievements.HardcoreMode)) {
        ARMSX2LogRetroAchievementsHardcoreBlock("per_game_enable_cheats");
        enableCheats = NO;
    }

    const std::string settingsPath = VMManager::GetGameSettingsPath(serial, crc);
    INISettingsInterface si(settingsPath);
    si.Load();

    if (enabled) {
        si.SetBoolValue("ARMSX2iOS/PerGame", "Enabled", true);
        // Only write what was actually overridden. Copying the global in froze it against later
        // edits, and for deinterlace_mode invented a value the global INI never held.
        if (upscaleMultiplier <= ARMSX2UseGlobalFloatSentinel)
            si.DeleteValue("EmuCore/GS", "upscale_multiplier");
        else
            si.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscaleMultiplier);
        if (aspectRatio.length == 0)
            si.DeleteValue("EmuCore/GS", "AspectRatio");
        else
            si.SetStringValue("EmuCore/GS", "AspectRatio", aspectRatio.UTF8String);
        if (textureFiltering == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "filter");
        else
            si.SetIntValue("EmuCore/GS", "filter", textureFiltering);
        if (hardwareMipmapping == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "hw_mipmap");
        else
            si.SetBoolValue("EmuCore/GS", "hw_mipmap", hardwareMipmapping != 0);
        if (blendingAccuracy == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "accurate_blending_unit");
        else
            si.SetIntValue("EmuCore/GS", "accurate_blending_unit", blendingAccuracy);
        if (interlaceMode == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "deinterlace_mode");
        else
            si.SetIntValue("EmuCore/GS", "deinterlace_mode", interlaceMode);
        if (trilinearFiltering == ARMSX2TriFilterUseGlobalSentinel)
            si.DeleteValue("EmuCore/GS", "TriFilter");
        else
            si.SetIntValue("EmuCore/GS", "TriFilter", ARMSX2ClampInt(trilinearFiltering, -1, 2));

        if (halfPixelOffset == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_HalfPixelOffset");
        else
            si.SetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", ARMSX2ClampInt(halfPixelOffset, 0, 5));

        if (roundSprite == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_round_sprite_offset");
        else
            si.SetIntValue("EmuCore/GS", "UserHacks_round_sprite_offset", ARMSX2ClampInt(roundSprite, 0, 2));

        if (alignSprite == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_align_sprite_X");
        else
            si.SetBoolValue("EmuCore/GS", "UserHacks_align_sprite_X", alignSprite != 0);

        if (mergeSprite == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_merge_pp_sprite");
        else
            si.SetBoolValue("EmuCore/GS", "UserHacks_merge_pp_sprite", mergeSprite != 0);

        if (wildArmsOffset == ARMSX2UseGlobalIntSentinel)
            si.DeleteValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition");
        else
            si.SetBoolValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition", wildArmsOffset != 0);

        if (textureOffsetXOverride)
            si.SetIntValue("EmuCore/GS", "UserHacks_TCOffsetX", ARMSX2ClampInt(textureOffsetX, -4096, 4096));
        else
            si.DeleteValue("EmuCore/GS", "UserHacks_TCOffsetX");

        if (textureOffsetYOverride)
            si.SetIntValue("EmuCore/GS", "UserHacks_TCOffsetY", ARMSX2ClampInt(textureOffsetY, -4096, 4096));
        else
            si.DeleteValue("EmuCore/GS", "UserHacks_TCOffsetY");

        // Derived from the keys just written, so the GameDB stops writing them for
        // this game. The core folds the global claims back in at load.
        ARMSX2StoreDerivedPerGameHackClaims(si);

        if (skipDrawStartOverride)
            si.SetIntValue("EmuCore/GS", "UserHacks_SkipDraw_Start", ARMSX2ClampInt(skipDrawStart, 0, 5000));
        else
            si.DeleteValue("EmuCore/GS", "UserHacks_SkipDraw_Start");

        if (skipDrawEndOverride)
            si.SetIntValue("EmuCore/GS", "UserHacks_SkipDraw_End", ARMSX2ClampInt(skipDrawEnd, 0, 5000));
        else
            si.DeleteValue("EmuCore/GS", "UserHacks_SkipDraw_End");

        // StandardVolume only. The audio tab owns FastForwardVolume and writes it later in the
        // same save, so touching it here just meant last writer won.
        if (volumeOverride)
            si.SetIntValue("SPU2/Output", "StandardVolume", ARMSX2ClampInt(volumePercent, 0, ARMSX2DefaultAudioVolumePercent));
        else
            si.DeleteValue("SPU2/Output", "StandardVolume");

        // No use-global marker on these, so compare against the global and write nothing when
        // they agree, same as vuThread below.
        const auto writeIfDifferent = [&si](const char* section, const char* key, bool value, bool global_value) {
            if (value == global_value)
                si.DeleteValue(section, key);
            else
                si.SetBoolValue(section, key, value);
        };
        const bool globalEnableCheats = g_p44_settings_interface ?
            g_p44_settings_interface->GetBoolValue("EmuCore", "EnableCheats", false) : false;
        const bool globalEnablePatches = g_p44_settings_interface ?
            g_p44_settings_interface->GetBoolValue("EmuCore", "EnablePatches", true) : true;
        const bool globalEnableGameFixes = g_p44_settings_interface ?
            g_p44_settings_interface->GetBoolValue("EmuCore", "EnableGameFixes", true) : true;
        const bool globalUserHacks = g_p44_settings_interface ?
            g_p44_settings_interface->GetBoolValue("EmuCore/GS", "UserHacks", false) : false;
        const int globalEECoreType = g_p44_settings_interface ?
            g_p44_settings_interface->GetIntValue("EmuCore/CPU", "CoreType", 2) : 2;
        writeIfDifferent("EmuCore", "EnableCheats", enableCheats, globalEnableCheats);
        writeIfDifferent("EmuCore", "EnablePatches", enablePatches, globalEnablePatches);
        writeIfDifferent("EmuCore", "EnableGameFixes", enableGameFixes, globalEnableGameFixes);
        writeIfDifferent("EmuCore/GS", "UserHacks", !enableGameDBHardwareFixes, globalUserHacks);
        if (eeCoreType == globalEECoreType) {
            si.DeleteValue("EmuCore/CPU", "CoreType");
            si.DeleteValue("EmuCore/CPU", "UseArm64Dynarec");
        } else {
            si.SetIntValue("EmuCore/CPU", "CoreType", eeCoreType);
            si.SetBoolValue("EmuCore/CPU", "UseArm64Dynarec", eeCoreType == 2);
        }
        const bool globalMTVU = g_p44_settings_interface ?
            g_p44_settings_interface->GetBoolValue("EmuCore/Speedhacks", "vuThread", true) : true;
        if (mtvu == globalMTVU) {
            si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVU");
            si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVUVersion");
            si.DeleteValue("EmuCore/Speedhacks", "vuThread");
        } else {
            si.SetBoolValue("ARMSX2iOS/PerGame", "ManualMTVU", true);
            si.SetIntValue("ARMSX2iOS/PerGame", "ManualMTVUVersion", 3);
            si.SetBoolValue("EmuCore/Speedhacks", "vuThread", mtvu);
        }

        if (eeCycleRateOverride) {
            int clampedEECycleRate = ARMSX2ClampInt(eeCycleRate, -3, 3);
            if (ARMSX2RetroAchievementsHardcoreActive() && clampedEECycleRate < 0) {
                ARMSX2LogRetroAchievementsHardcoreBlock("per_game_ee_underclock");
                clampedEECycleRate = 0;
            }
            si.SetIntValue("EmuCore/Speedhacks", "EECycleRate", clampedEECycleRate);
        } else {
            si.DeleteValue("EmuCore/Speedhacks", "EECycleRate");
        }

        if (fastBootOverride)
            si.SetBoolValue("EmuCore", "EnableFastBoot", fastBoot);
        else
            si.DeleteValue("EmuCore", "EnableFastBoot");
    } else {
        si.DeleteValue("ARMSX2iOS/PerGame", "Enabled");
        si.DeleteValue("EmuCore/GS", "upscale_multiplier");
        si.DeleteValue("EmuCore/GS", "AspectRatio");
        si.DeleteValue("EmuCore/GS", "filter");
        si.DeleteValue("EmuCore/GS", "hw_mipmap");
        si.DeleteValue("EmuCore/GS", "accurate_blending_unit");
        si.DeleteValue("EmuCore/GS", "deinterlace_mode");
        si.DeleteValue("EmuCore/GS", "TriFilter");
        si.DeleteValue("EmuCore/GS", "UserHacks_HalfPixelOffset");
        si.DeleteValue("EmuCore/GS", "UserHacks_round_sprite_offset");
        si.DeleteValue("EmuCore/GS", "UserHacks_align_sprite_X");
        si.DeleteValue("EmuCore/GS", "UserHacks_merge_pp_sprite");
        si.DeleteValue("EmuCore/GS", "UserHacks_ForceEvenSpritePosition");
        si.DeleteValue("EmuCore/GS", "UserHacks_TCOffsetX");
        si.DeleteValue("EmuCore/GS", "UserHacks_TCOffsetY");
        si.DeleteValue("EmuCore/GS", "UserHacks_SkipDraw_Start");
        si.DeleteValue("EmuCore/GS", "UserHacks_SkipDraw_End");
        si.DeleteValue("EmuCore", "EnableCheats");
        si.DeleteValue("EmuCore", "EnablePatches");
        si.DeleteValue("EmuCore", "EnableGameFixes");
        si.DeleteValue("EmuCore/GS", "UserHacks");
        si.DeleteValue("EmuCore/GS", "UserHackOverrides");
        si.DeleteValue("EmuCore/CPU", "CoreType");
        si.DeleteValue("EmuCore/CPU", "UseArm64Dynarec");
        si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVU");
        si.DeleteValue("ARMSX2iOS/PerGame", "ManualMTVUVersion");
        si.DeleteValue("EmuCore/Speedhacks", "vuThread");
        si.DeleteValue("EmuCore/Speedhacks", "EECycleRate");
        si.DeleteValue("EmuCore", "EnableFastBoot");
        si.DeleteValue("SPU2/Output", "StandardVolume");
        si.DeleteValue("SPU2/Output", "FastForwardVolume");
        // hasKnownOverride counts these and the pad tab writes them without consulting the master
        // toggle, so not clearing them let a flipped stick latch overrides on.
        si.DeleteValue("ARMSX2iOS/UI", "InvertLeftStickX");
        si.DeleteValue("ARMSX2iOS/UI", "InvertLeftStickY");
        si.DeleteValue("ARMSX2iOS/UI", "InvertRightStickX");
        si.DeleteValue("ARMSX2iOS/UI", "InvertRightStickY");
        si.RemoveEmptySections();
    }

    Error error;
    const bool saved = si.Save(&error);
    NSLog(@"[ARMSX2Bridge] Game settings %@ serial=%@ crc=%08X path=%@ result=%d",
          enabled ? @"saved" : @"cleared", ARMSX2NSStringFromStdString(serial),
          crc, ARMSX2NSStringFromStdString(settingsPath), saved ? 1 : 0);
    if (!saved)
        NSLog(@"[ARMSX2Bridge] Game settings save error: %@", ARMSX2NSStringFromStdString(error.GetDescription()));
}

static NSArray<NSString*>* ARMSX2PatchEnableListForIdentity(const std::string& serial, u32 crc,
                                                             NSString* section, NSString* key)
{
    if (serial.empty() || crc == 0)
        return @[];

    const std::string settingsPath = VMManager::GetGameSettingsPath(serial, crc);
    INISettingsInterface si(settingsPath);
    if (!si.Load())
        return @[];

    const std::vector<std::string> values = si.GetStringList(section.UTF8String ?: "", key.UTF8String ?: "");
    NSMutableArray<NSString*>* result = [NSMutableArray arrayWithCapacity:values.size()];
    for (const std::string& value : values)
    {
        NSString* name = ARMSX2NSStringFromStdString(value);
        if (name.length > 0)
            [result addObject:name];
    }
    return result;
}

static void ARMSX2SetPatchEnableListForIdentity(NSArray<NSString*>* values, const std::string& serial, u32 crc,
                                                NSString* section, NSString* key)
{
    if (serial.empty() || crc == 0)
        return;

    FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), false);
    const std::string settingsPath = VMManager::GetGameSettingsPath(serial, crc);
    INISettingsInterface si(settingsPath);
    si.Load();

    std::vector<std::string> list;
    list.reserve(values.count);
    for (NSString* value in values)
    {
        if (value.length > 0)
            list.push_back(value.UTF8String);
    }

    if (list.empty())
        si.DeleteValue(section.UTF8String ?: "", key.UTF8String ?: "");
    else
        si.SetStringList(section.UTF8String ?: "", key.UTF8String ?: "", list);

    Error error;
    si.Save(&error);
    // NSLog(@"[ARMSX2Bridge] Patch enable list saved serial=%@ crc=%08X section=%@ count=%lu",
    //       ARMSX2NSStringFromStdString(serial), (unsigned int)crc, section, (unsigned long)list.size());
}

// Resolve a per-game identity (serial, crc) for a library ISO. Returns NO if the ISO
// cannot be resolved or has no CRC. ELFs have no serial (empty), matching the
// game-settings writer.
static BOOL ARMSX2PerGameIdentityForISO(NSString* isoName, std::string* serial, u32* crc)
{
    // The VM already knows what it booted, so asking the disc is both slower and,
    // until this check existed, destructive. Taking it here rather than relying on
    // the cache below also means this keeps working when the game list has no
    // entry for the running game.
    if (ARMSX2PathIsRunningDisc(ARMSX2ResolveISOPath(isoName)))
        return ARMSX2PerGameIdentityForCurrentGame(serial, crc);

    GameList::Entry entry;
    NSString* resolvedPath = nil;
    if (!ARMSX2PopulateGameListEntryForISO(isoName, &entry, &resolvedPath) || entry.crc == 0)
        return NO;
    *serial = (entry.type == GameList::EntryType::ELF) ? std::string() : entry.serial;
    *crc = entry.crc;
    return YES;
}

// Resolve a per-game identity for the running game. Returns NO with no valid VM/CRC.
static BOOL ARMSX2PerGameIdentityForCurrentGame(std::string* serial, u32* crc)
{
    if (!VMManager::HasValidVM())
        return NO;
    *serial = VMManager::GetSerialForGameSettings();
    *crc = VMManager::GetDiscCRC();
    return *crc != 0;
}

static std::string ARMSX2PerGameSettingsPath(const std::string& serial, u32 crc)
{
    FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), false);
    return VMManager::GetGameSettingsPath(serial, crc);
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

enum ARMSX2ShaderPackFailure {
    ARMSX2ShaderPackBadArgument = 1,
    ARMSX2ShaderPackUnreadable,
    ARMSX2ShaderPackTooLarge,
    ARMSX2ShaderPackEscapingEntry,
    ARMSX2ShaderPackWriteFailed,
};

static NSArray<NSURL*>* ARMSX2FailShaderPackExtraction(NSError** error, NSInteger code, NSString* message)
{
    if (error) {
        *error = [NSError errorWithDomain:@"ARMSX2ShaderPackExtraction"
                                     code:code
                                 userInfo:@{NSLocalizedDescriptionKey: message}];
    }
    NSLog(@"[ARMSX2 iOS Shaders] %@", message);
    return @[];
}

static BOOL ARMSX2IsArchiveJunkName(NSString* name)
{
    if ([name.pathComponents containsObject:@"__MACOSX"])
        return YES;

    NSString* last = name.lastPathComponent;
    return [last isEqualToString:@".DS_Store"] || [last hasPrefix:@"._"];
}

static BOOL ARMSX2IsShaderPackImportName(NSString* name)
{
    static NSSet<NSString*>* allowed;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        // This file is MRC, so a convenience constructor here dies with the pool and every
        // later call reads freed memory
        allowed = [[NSSet alloc] initWithArray:@[@"slangp", @"slang", @"glslp", @"glsl", @"cgp",
                                                 @"cg", @"inc", @"h", @"params", @"png", @"jpg",
                                                 @"jpeg", @"tga", @"bmp", @"txt", @"md"]];
    });
    return [allowed containsObject:name.pathExtension.lowercaseString];
}

static NSString* ARMSX2ContainedRelativePath(NSArray<NSString*>* components)
{
    NSMutableArray<NSString*>* kept = [NSMutableArray arrayWithCapacity:components.count];
    for (NSString* component in components) {
        if (component.length == 0 || [component isEqualToString:@"."])
            continue;
        if ([component isEqualToString:@".."] || [component isEqualToString:@"/"])
            return nil;
        [kept addObject:component];
    }
    return kept.count > 0 ? [NSString pathWithComponents:kept] : nil;
}

static NSString* ARMSX2CommonArchiveRoot(NSArray<NSString*>* names)
{
    NSString* root = names.firstObject.pathComponents.firstObject;
    if (names.firstObject.pathComponents.count < 2 || root.length == 0)
        return nil;

    for (NSString* name in names) {
        NSArray<NSString*>* components = name.pathComponents;
        if (components.count < 2 || ![components.firstObject isEqualToString:root])
            return nil;
    }
    return root;
}

static void ARMSX2RollBackShaderPack(NSArray<NSURL*>* files, NSArray<NSURL*>* directories)
{
    NSFileManager* manager = [NSFileManager defaultManager];
    for (NSURL* url in files)
        [manager removeItemAtURL:url error:nil];
    for (NSURL* url in directories.reverseObjectEnumerator)
        [manager removeItemAtURL:url error:nil];
}

@implementation ARMSX2Bridge

+ (UIView *)gameRenderView {
    extern UIView* g_gameRenderView;
    return g_gameRenderView;
}

+ (void)prepareGameRenderViewForCurrentRenderer {
    ARMSX2_PrepareGameRenderViewForCurrentRenderer("swift_preboot");
}

+ (void)saveNVRAM {
    cdvdSaveNVRAM();
    ARMSX2SetLastNVMSaveDate([NSDate date]);
    NSLog(@"[ARMSX2Bridge] NVM saved at %@", s_lastNVMSaveDate);
}

+ (void)saveMemoryCards {
    Host::RunOnCPUThread([]() {
        const bool flushed = ARMSX2FlushNVRAMAndMemoryCards("manual-save-memory-cards");
        NSLog(@"[ARMSX2Bridge] Memory card save requested result=%d", flushed ? 1 : 0);
    }, false);
}

+ (void)saveAllState {
    [self saveNVRAM];
    [self saveMemoryCards];
}

+ (BOOL)isRunning {
    return VMManager::GetState() == VMState::Running;
}

+ (void)setPadButton:(ARMSX2PadButton)button pressed:(BOOL)pressed {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;

    static const u32 buttonMap[] = {
        PadDualshock2::Inputs::PAD_UP,       // Up
        PadDualshock2::Inputs::PAD_DOWN,     // Down
        PadDualshock2::Inputs::PAD_LEFT,     // Left
        PadDualshock2::Inputs::PAD_RIGHT,    // Right
        PadDualshock2::Inputs::PAD_CROSS,    // Cross
        PadDualshock2::Inputs::PAD_CIRCLE,   // Circle
        PadDualshock2::Inputs::PAD_SQUARE,   // Square
        PadDualshock2::Inputs::PAD_TRIANGLE, // Triangle
        PadDualshock2::Inputs::PAD_L1,       // L1
        PadDualshock2::Inputs::PAD_R1,       // R1
        PadDualshock2::Inputs::PAD_L2,       // L2
        PadDualshock2::Inputs::PAD_R2,       // R2
        PadDualshock2::Inputs::PAD_START,    // Start
        PadDualshock2::Inputs::PAD_SELECT,   // Select
        PadDualshock2::Inputs::PAD_L3,       // L3
        PadDualshock2::Inputs::PAD_R3,       // R3
    };

    if ((int)button < (int)(sizeof(buttonMap)/sizeof(buttonMap[0]))) {
        u32 idx = buttonMap[(int)button];
        pad->Set(idx, pressed ? 1.0f : 0.0f);
        // Update touch state so PumpMessagesOnCPUThread doesn't override
        extern bool g_touchPadState[64];
        if (idx < 64) g_touchPadState[idx] = pressed;
    }
}

+ (void)setLeftStickX:(float)x Y:(float)y {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;
    // Convert axis (-1..+1) to individual direction values (0..1)
    const float right = x > 0 ? x : 0.0f;
    const float left = x < 0 ? -x : 0.0f;
    const float down = y > 0 ? y : 0.0f;
    const float up = y < 0 ? -y : 0.0f;
    pad->Set(PadDualshock2::Inputs::PAD_L_RIGHT, right);
    pad->Set(PadDualshock2::Inputs::PAD_L_LEFT, left);
    pad->Set(PadDualshock2::Inputs::PAD_L_DOWN, down);
    pad->Set(PadDualshock2::Inputs::PAD_L_UP, up);
    extern bool g_touchPadState[64];
    g_touchPadState[PadDualshock2::Inputs::PAD_L_RIGHT] = right > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_L_LEFT] = left > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_L_DOWN] = down > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_L_UP] = up > 0.01f;
}

+ (void)setRightStickX:(float)x Y:(float)y {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;
    const float right = x > 0 ? x : 0.0f;
    const float left = x < 0 ? -x : 0.0f;
    const float down = y > 0 ? y : 0.0f;
    const float up = y < 0 ? -y : 0.0f;
    pad->Set(PadDualshock2::Inputs::PAD_R_RIGHT, right);
    pad->Set(PadDualshock2::Inputs::PAD_R_LEFT, left);
    pad->Set(PadDualshock2::Inputs::PAD_R_DOWN, down);
    pad->Set(PadDualshock2::Inputs::PAD_R_UP, up);
    extern bool g_touchPadState[64];
    g_touchPadState[PadDualshock2::Inputs::PAD_R_RIGHT] = right > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_R_LEFT] = left > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_R_DOWN] = down > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_R_UP] = up > 0.01f;
}

+ (nonnull NSString *)biosName {
    return @"PS2";
}

+ (void)requestVMStop {
    extern std::atomic<bool> s_requestVMStop;
    s_requestVMStop.store(true);
    NSLog(@"[ARMSX2Bridge] VM stop requested");
}

+ (void)setVMPaused:(BOOL)paused {
    const bool hasValidVM = VMManager::HasValidVM();
    std::fprintf(stderr, "@@IOS_VM_PAUSE_REQUEST@@ paused=%d valid=%d block=0 state=%d\n",
        paused ? 1 : 0, hasValidVM ? 1 : 0,
        hasValidVM ? static_cast<int>(VMManager::GetState()) : -1);
    std::fflush(stderr);

    if (!hasValidVM)
        return;

    Host::RunOnCPUThread([paused]() {
        if (!VMManager::HasValidVM())
            return;

        const VMState state = VMManager::GetState();
        if (paused && state == VMState::Running) {
            VMManager::SetPaused(true);
            Console.WriteLn("@@IOS_VM_PAUSE@@ paused=1 reason=swiftui-menu");
        } else if (!paused && state == VMState::Paused) {
            VMManager::SetPaused(false);
            Console.WriteLn("@@IOS_VM_PAUSE@@ paused=0 reason=swiftui-menu");
        }
        Console.WriteLn("@@IOS_VM_PAUSE_APPLY@@ requested=%d before=%d after=%d",
            paused ? 1 : 0, static_cast<int>(state), static_cast<int>(VMManager::GetState()));
    }, false);
}

+ (void)setFullScreen:(BOOL)enabled {
    ARMSX2_SetSDLFullscreen(enabled ? true : false);
}

+ (BOOL)isSDLFullscreen {
    return ARMSX2_IsSDLFullscreen() ? YES : NO;
}

+ (nonnull NSString *)buildVersion {
    NSString *ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    return [NSString stringWithFormat:@"ARMSX2 iOS v%@", ver];
}

+ (BOOL)isJITAvailable {
    return DarwinMisc::IsJITAvailable();
}

+ (BOOL)isNoJITFallbackActive {
    return DarwinMisc::iPSX2_FORCE_EE_INTERP != 0;
}

+ (BOOL)isIdleVMPrewarmResolved {
    return ARMSX2_IsIdleVMPrewarmResolved() ? YES : NO;
}

+ (nonnull NSArray<NSURL *> *)extractControllerSkinArchiveAtURL:(nonnull NSURL *)archiveURL
                                                    toDirectory:(nonnull NSURL *)destinationDirectory {
    static const zip_uint64_t kMaxSkinArchiveEntryBytes = 16 * 1024 * 1024;
    static const NSUInteger kMaxSkinArchiveEntries = 64;
    static const zip_int64_t kMaxSkinArchiveTotalEntries = 512;

    NSMutableArray<NSURL *> *extracted = [NSMutableArray array];
    if (!archiveURL.isFileURL || !destinationDirectory.isFileURL)
        return extracted;

    NSError *directoryError = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:destinationDirectory
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&directoryError]) {
        NSLog(@"[ARMSX2 iOS Skins] Could not create extraction directory %@: %@",
              destinationDirectory.path, directoryError.localizedDescription);
        return extracted;
    }

    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.UTF8String, ZIP_RDONLY, &ze);
    if (!zf) {
        NSLog(@"[ARMSX2 iOS Skins] Could not open skin archive %@: %s",
              archiveURL.lastPathComponent, zip_error_strerror(&ze));
        return extracted;
    }

    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    if (count > kMaxSkinArchiveTotalEntries) {
        NSLog(@"[ARMSX2 iOS Skins] Skin archive has too many entries (%lld); skipping %@.",
              static_cast<long long>(count), archiveURL.lastPathComponent);
        return extracted;
    }
    NSSet<NSString*>* allowedJSONNames = ARMSX2AllowedControllerSkinJSONNames(zf.get(), count);
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
        if (extracted.count >= kMaxSkinArchiveEntries)
            break;

        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;
        if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxSkinArchiveEntryBytes)
            continue;

        NSString *entryName = [NSString stringWithUTF8String:stat.name];
        // Every file in a mac-built zip has a "._" sibling, and they were eating
        // the entry budget one-for-one with the real art. Skips dotfiles in
        // general, which a skin has no business shipping anyway.
        if ([entryName containsString:@"__MACOSX"] || [entryName.lastPathComponent hasPrefix:@"."])
            continue;
        if (entryName.length == 0 || [entryName hasSuffix:@"/"] || !ARMSX2IsControllerSkinImportName(entryName, allowedJSONNames))
            continue;

        auto file = zip_fopen_index_managed(zf.get(), i, ZIP_FL_ENC_GUESS);
        if (!file)
            continue;

        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->empty())
            continue;

        NSString *safeName = ARMSX2SanitizedSkinFileName(entryName);
        if (safeName.length == 0)
            continue;

        NSURL *destinationURL = [destinationDirectory URLByAppendingPathComponent:safeName];
        NSData *imageData = [NSData dataWithBytes:data->data() length:data->size()];
        if ([imageData writeToURL:destinationURL atomically:YES])
            [extracted addObject:destinationURL];
    }

    NSLog(@"[ARMSX2 iOS Skins] Extracted %lu skin file(s) from %@",
          static_cast<unsigned long>(extracted.count), archiveURL.lastPathComponent);
    return extracted;
}

+ (nonnull NSArray<NSURL *> *)extractShaderPackArchiveAtURL:(nonnull NSURL *)archiveURL toDirectory:(nonnull NSURL *)destinationDirectory error:(NSError * _Nullable * _Nullable)error
{
    // Sized for the stock RetroArch pack, which is thousands of text stages and a few
    // lookup images; the controller-skin caps of 64 and 512 would truncate it in silence.
    static const zip_uint64_t kMaxShaderPackEntryBytes = 8 * 1024 * 1024;
    static const zip_uint64_t kMaxShaderPackTotalBytes = 512 * 1024 * 1024;
    static const zip_int64_t kMaxShaderPackEntries = 32768;

    if (error)
        *error = nil;

    if (!archiveURL.isFileURL || !destinationDirectory.isFileURL)
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackBadArgument, @"Shader pack extraction needs file URLs.");

    NSFileManager *manager = [NSFileManager defaultManager];
    NSError *directoryError = nil;
    if (![manager createDirectoryAtURL:destinationDirectory
           withIntermediateDirectories:YES
                            attributes:nil
                                 error:&directoryError]) {
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackWriteFailed,
            [NSString stringWithFormat:@"Could not create %@: %@", destinationDirectory.path, directoryError.localizedDescription]);
    }

    char rootBuffer[PATH_MAX] = {};
    if (!realpath(destinationDirectory.path.fileSystemRepresentation, rootBuffer))
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackWriteFailed, @"Could not resolve the shader pack destination.");

    NSString *resolvedRoot = [manager stringWithFileSystemRepresentation:rootBuffer length:strlen(rootBuffer)];
    NSString *guardPrefix = [resolvedRoot stringByAppendingString:@"/"];

    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.UTF8String, ZIP_RDONLY, &ze);
    if (!zf) {
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackUnreadable,
            [NSString stringWithFormat:@"Could not open %@: %s", archiveURL.lastPathComponent, zip_error_strerror(&ze)]);
    }

    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    if (count > kMaxShaderPackEntries) {
        return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackTooLarge,
            [NSString stringWithFormat:@"%@ has %lld entries, more than a shader pack should.", archiveURL.lastPathComponent, static_cast<long long>(count)]);
    }

    NSMutableArray<NSString *> *names = [NSMutableArray array];
    NSMutableArray<NSNumber *> *indices = [NSMutableArray array];
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;

        NSString *entryName = [NSString stringWithUTF8String:stat.name];
        if (entryName.length == 0 || [entryName hasSuffix:@"/"])
            continue;
        // Junk is dropped here rather than during extraction because the common-root test
        // below asks whether EVERY entry shares a root: one surviving __MACOSX/ makes the
        // answer no, the strip is skipped, and the pack lands one directory too deep.
        if (ARMSX2IsArchiveJunkName(entryName))
            continue;

        [names addObject:entryName];
        [indices addObject:@(i)];
    }

    NSString *commonRoot = ARMSX2CommonArchiveRoot(names);
    NSMutableArray<NSURL *> *extracted = [NSMutableArray array];
    NSMutableArray<NSURL *> *createdDirectories = [NSMutableArray array];
    zip_uint64_t totalBytes = 0;

    for (NSUInteger n = 0; n < names.count; n++) {
        NSString *entryName = names[n];
        NSArray<NSString *> *components = entryName.pathComponents;
        if (commonRoot && [components.firstObject isEqualToString:commonRoot])
            components = [components subarrayWithRange:NSMakeRange(1, components.count - 1)];

        NSString *relative = ARMSX2ContainedRelativePath(components);
        if (!relative) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackEscapingEntry,
                [NSString stringWithFormat:@"%@ contains an entry that escapes its directory: %@", archiveURL.lastPathComponent, entryName]);
        }
        if (!ARMSX2IsShaderPackImportName(relative))
            continue;

        const zip_uint64_t index = indices[n].unsignedLongLongValue;
        zip_uint8_t opsys = 0;
        zip_uint32_t attributes = 0;
        if (zip_file_get_external_attributes(zf.get(), index, 0, &opsys, &attributes) == 0 &&
            opsys == ZIP_OPSYS_UNIX && ((attributes >> 16) & S_IFMT) == S_IFLNK) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackEscapingEntry,
                [NSString stringWithFormat:@"%@ contains a symlink entry: %@", archiveURL.lastPathComponent, entryName]);
        }

        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), index, ZIP_FL_ENC_GUESS, &stat) != 0)
            continue;
        if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxShaderPackEntryBytes)
            continue;

        NSArray<NSString *> *relativeComponents = relative.pathComponents;
        NSURL *parentURL = destinationDirectory;
        for (NSUInteger c = 0; c + 1 < relativeComponents.count; c++) {
            parentURL = [parentURL URLByAppendingPathComponent:relativeComponents[c] isDirectory:YES];
            if ([manager fileExistsAtPath:parentURL.path])
                continue;
            if (![manager createDirectoryAtURL:parentURL
                   withIntermediateDirectories:NO
                                    attributes:nil
                                         error:&directoryError]) {
                ARMSX2RollBackShaderPack(extracted, createdDirectories);
                return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackWriteFailed,
                    [NSString stringWithFormat:@"Could not create %@: %@", parentURL.path, directoryError.localizedDescription]);
            }
            [createdDirectories addObject:parentURL];
        }

        // Flattening is what makes the skin extractor safe by construction, and preserving
        // the tree gives that up, so containment is proven canonically and on a component
        // boundary: <dest>-evil shares a string prefix with <dest> and is not inside it.
        char parentBuffer[PATH_MAX] = {};
        NSString *resolvedParent = realpath(parentURL.path.fileSystemRepresentation, parentBuffer) ?
            [manager stringWithFileSystemRepresentation:parentBuffer length:strlen(parentBuffer)] : nil;
        if (![resolvedParent isEqualToString:resolvedRoot] && ![resolvedParent hasPrefix:guardPrefix]) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackEscapingEntry,
                [NSString stringWithFormat:@"%@ contains an entry that escapes its directory: %@", archiveURL.lastPathComponent, entryName]);
        }

        auto file = zip_fopen_index_managed(zf.get(), index, ZIP_FL_ENC_GUESS);
        if (!file)
            continue;

        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->size() > kMaxShaderPackEntryBytes)
            continue;

        totalBytes += data->size();
        if (totalBytes > kMaxShaderPackTotalBytes) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackTooLarge,
                [NSString stringWithFormat:@"%@ unpacks to more than a shader pack should.", archiveURL.lastPathComponent]);
        }

        NSURL *destinationURL = [parentURL URLByAppendingPathComponent:relativeComponents.lastObject isDirectory:NO];
        NSData *bytes = [NSData dataWithBytes:data->data() length:data->size()];
        if (![bytes writeToURL:destinationURL atomically:YES]) {
            ARMSX2RollBackShaderPack(extracted, createdDirectories);
            return ARMSX2FailShaderPackExtraction(error, ARMSX2ShaderPackWriteFailed,
                [NSString stringWithFormat:@"Could not write %@", destinationURL.path]);
        }
        [extracted addObject:destinationURL];
    }

    NSLog(@"[ARMSX2 iOS Shaders] Extracted %lu file(s) from %@",
          static_cast<unsigned long>(extracted.count), archiveURL.lastPathComponent);
    return extracted;
}

+ (nullable NSData *)peekSkinManifestDataAtURL:(NSURL *)archiveURL {
    if (!archiveURL.isFileURL) {
        return nil;
    }

    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.UTF8String, ZIP_RDONLY, &ze);
    if (!zf) {
        return nil;
    }

    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    if (count > 512) {
        return nil;
    }

    // Prefer info.json, then manifest.json. Read raw bytes without extracting.
    for (NSString* wanted in @[@"info.json", @"manifest.json"]) {
        for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
            zip_stat_t stat = {};
            if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name) {
                continue;
            }
            NSString* entryName = [NSString stringWithUTF8String:stat.name];
            if (entryName.length == 0 || [entryName hasSuffix:@"/"]) {
                continue;
            }
            if ([entryName containsString:@"__MACOSX"] || [entryName containsString:@".."]) {
                continue;
            }
            if (![entryName.lastPathComponent.lowercaseString isEqualToString:wanted]) {
                continue;
            }
            if ((stat.valid & ZIP_STAT_SIZE) && stat.size > 16 * 1024 * 1024) {
                continue;
            }
            auto file = zip_fopen_index_managed(zf.get(), i, ZIP_FL_ENC_GUESS);
            if (!file) {
                continue;
            }
            std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
            if (!data.has_value() || data->empty()) {
                continue;
            }
            return [NSData dataWithBytes:data->data() length:data->size()];
        }
    }
    return nil;
}

+ (nonnull NSArray<NSURL *> *)extractSkinPackageArchiveAtURL:(NSURL *)archiveURL
                                                  toDirectory:(NSURL *)destinationDirectory {
    static const zip_uint64_t kMaxPackageEntryBytes = 16 * 1024 * 1024;
    static const NSUInteger kMaxPackageExtractedEntries = 128;
    static const zip_int64_t kMaxPackageTotalEntries = 512;
    static NSArray<NSString*>* kAllowedPackageExtensions;

    static dispatch_once_t once;
    dispatch_once(&once, ^{
        kAllowedPackageExtensions = [[NSArray alloc] initWithObjects:@"png", @"jpg", @"jpeg",
                                                                    @"webp", @"pdf", @"json", nil];
    });

    NSMutableArray<NSURL*>* extracted = [NSMutableArray array];
    if (!archiveURL.isFileURL || !destinationDirectory.isFileURL) {
        return extracted;
    }

    NSError* directoryError = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:destinationDirectory
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&directoryError]) {
        NSLog(@"[ARMSX2 iOS Skins] Could not create package directory %@: %@",
              destinationDirectory.path, directoryError.localizedDescription);
        return extracted;
    }

    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.UTF8String, ZIP_RDONLY, &ze);
    if (!zf) {
        NSLog(@"[ARMSX2 iOS Skins] Could not open skin package %@: %s",
              archiveURL.lastPathComponent, zip_error_strerror(&ze));
        return extracted;
    }

    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    if (count > kMaxPackageTotalEntries) {
        NSLog(@"[ARMSX2 iOS Skins] Skin package has too many entries (%lld); skipping %@.",
              static_cast<long long>(count), archiveURL.lastPathComponent);
        return extracted;
    }

    NSString* basePath = destinationDirectory.path;
    NSCharacterSet* allowed = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"];

    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
        if (extracted.count >= kMaxPackageExtractedEntries) {
            break;
        }

        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name) {
            continue;
        }
        if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxPackageEntryBytes) {
            continue;
        }

        NSString* entryName = [NSString stringWithUTF8String:stat.name];
        if (entryName.length == 0 || [entryName hasSuffix:@"/"]) {
            continue;
        }
        if ([entryName containsString:@"__MACOSX"]) {
            continue;
        }

        NSString* extension = entryName.pathExtension.lowercaseString;
        if (extension.length == 0 || ![kAllowedPackageExtensions containsObject:extension]) {
            continue;
        }

        // Build a safe relative path: reject any absolute/".."/"."/hidden
        // component, then sanitize each component to filesystem-safe characters.
        NSArray* components = [entryName componentsSeparatedByString:@"/"];
        BOOL rejected = NO;
        for (NSString* component in components) {
            if ([component isEqualToString:@".."] || [component isEqualToString:@"."] || [component hasPrefix:@"."]) {
                rejected = YES;
                break;
            }
        }
        if (rejected) {
            continue;
        }

        NSURL* destinationURL = destinationDirectory;
        for (NSString* component in components) {
            NSMutableString* sanitized = [NSMutableString stringWithCapacity:component.length];
            for (NSUInteger c = 0; c < component.length; c++) {
                unichar ch = [component characterAtIndex:c];
                [sanitized appendString:[allowed characterIsMember:ch] ? [NSString stringWithCharacters:&ch length:1] : @"_"];
            }
            if (sanitized.length > 0) {
                destinationURL = [destinationURL URLByAppendingPathComponent:sanitized];
            }
        }

        // Defense-in-depth against path traversal: the resolved path must
        // remain inside the destination directory.
        NSString* resolvedPath = destinationURL.path;
        if (![resolvedPath hasPrefix:[basePath stringByAppendingString:@"/"]]) {
            continue;
        }

        auto file = zip_fopen_index_managed(zf.get(), i, ZIP_FL_ENC_GUESS);
        if (!file) {
            continue;
        }
        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->empty()) {
            continue;
        }

        NSString* parentPath = destinationURL.URLByDeletingLastPathComponent.path;
        [[NSFileManager defaultManager] createDirectoryAtPath:parentPath
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        NSData* nsdata = [NSData dataWithBytes:data->data() length:data->size()];
        if ([nsdata writeToURL:destinationURL atomically:YES]) {
            [extracted addObject:destinationURL];
        }
    }

    NSLog(@"[ARMSX2 iOS Skins] Extracted %lu package file(s) from %@",
          static_cast<unsigned long>(extracted.count), archiveURL.lastPathComponent);
    return extracted;
}

+ (nullable NSString *)extractMemoryCardArchiveAtURL:(nonnull NSURL *)archiveURL {
    static const zip_uint64_t kMaxMemcardEntryBytes = 128 * 1024 * 1024;
    static const zip_int64_t kMaxMemcardTotalEntries = 64;

    if (!archiveURL.isFileURL) {
        return nil;
    }

    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.UTF8String, ZIP_RDONLY, &ze);
    if (!zf) {
        NSLog(@"[ARMSX2 iOS Memcards] Could not open archive %@: %s",
              archiveURL.lastPathComponent, zip_error_strerror(&ze));
        return nil;
    }

    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    if (count > kMaxMemcardTotalEntries) {
        NSLog(@"[ARMSX2 iOS Memcards] Archive has too many entries (%lld); skipping %@.",
              static_cast<long long>(count), archiveURL.lastPathComponent);
        return nil;
    }

    NSString *memcardDir = [self memoryCardDirectory];
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;
        if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxMemcardEntryBytes)
            continue;

        NSString *entryName = [NSString stringWithUTF8String:stat.name];
        if (entryName.length == 0 || [entryName hasSuffix:@"/"])
            continue;
        if ([entryName containsString:@"__MACOSX"] || [entryName containsString:@".."])
            continue;
        if (![entryName.pathExtension.lowercaseString isEqualToString:@"ps2"])
            continue;

        NSString *safeName = entryName.lastPathComponent;
        if ([safeName hasPrefix:@"."])
            continue;

        auto file = zip_fopen_index_managed(zf.get(), i, ZIP_FL_ENC_GUESS);
        if (!file)
            continue;
        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->empty())
            continue;

        NSString *destinationPath = [memcardDir stringByAppendingPathComponent:safeName];
        if (![destinationPath hasPrefix:[memcardDir stringByAppendingString:@"/"]])
            continue;

        NSData *nsdata = [NSData dataWithBytes:data->data() length:data->size()];
        if ([nsdata writeToFile:destinationPath atomically:YES]) {
            NSLog(@"[ARMSX2 iOS Memcards] Extracted %@ from %@", safeName, archiveURL.lastPathComponent);
            return safeName;
        }
    }

    NSLog(@"[ARMSX2 iOS Memcards] No .ps2 memory card found in %@",
          archiveURL.lastPathComponent);
    return nil;
}

+ (nullable NSString *)currentISOPath {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *iniPath = [docsPath stringByAppendingPathComponent:@"ARMSX2-iOS.ini"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:iniPath])
        iniPath = [docsPath stringByAppendingPathComponent:@"PCSX2-iOS.ini"];
    // Read BootISO from INI
    FILE *f = fopen(iniPath.UTF8String, "r");
    if (!f) return nil;
    char line[512];
    bool inSection = false;
    NSString *result = nil;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "[GameISO]")) { inSection = true; continue; }
        if (line[0] == '[') { inSection = false; continue; }
        if (inSection && strstr(line, "BootISO")) {
            char *eq = strchr(line, '=');
            if (eq) {
                eq++;
                while (*eq == ' ') eq++;
                // Remove trailing newline
                char *nl = strchr(eq, '\n'); if (nl) *nl = 0;
                char *cr = strchr(eq, '\r'); if (cr) *cr = 0;
                if (strlen(eq) > 0) result = [NSString stringWithUTF8String:eq];
            }
        }
    }
    fclose(f);
    return result;
}

+ (nullable NSString *)currentGameISOName {
    if (VMManager::HasValidVM()) {
        const std::string discPath = VMManager::GetDiscPath();
        if (!discPath.empty()) {
            NSString *fileName = ARMSX2NSStringFromStringView(Path::GetFileName(discPath));
            if (fileName.length > 0)
                return fileName;
        }
    }

    NSString *currentPath = [self currentISOPath];
    return currentPath.length > 0 ? currentPath.lastPathComponent : nil;
}

+ (nonnull NSString *)isoDirectory {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *isoDir = [docsPath stringByAppendingPathComponent:@"iso"];
    [[NSFileManager defaultManager] createDirectoryAtPath:isoDir withIntermediateDirectories:YES attributes:nil error:nil];
    return isoDir;
}

+ (nonnull NSString *)documentsDirectory {
    return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
}

+ (nonnull NSArray<NSString *> *)availableISOs {
	NSFileManager *fm = [NSFileManager defaultManager];
	NSMutableSet *seen = [NSMutableSet set];
	NSMutableArray *isos = [NSMutableArray array];

    // Helper block: scan a directory for ISO files
    void (^scanDir)(NSString *) = ^(NSString *dir) {
        NSArray *files = [fm contentsOfDirectoryAtPath:dir error:nil];
        for (NSString *file in files) {
            if ([seen containsObject:file]) continue;
            NSString *ext = file.pathExtension.lowercaseString;
            if ([ext isEqualToString:@"iso"] || [ext isEqualToString:@"img"] || [ext isEqualToString:@"chd"] ||
                [ext isEqualToString:@"cso"] || [ext isEqualToString:@"zso"] || [ext isEqualToString:@"gz"] ||
                [ext isEqualToString:@"elf"]) {
                [isos addObject:file];
                [seen addObject:file];
            } else if ([ext isEqualToString:@"bin"]) {
// .bin > 50MB treated as game image
                NSString *fullPath = [dir stringByAppendingPathComponent:file];
                NSDictionary *attrs = [fm attributesOfItemAtPath:fullPath error:nil];
                if ([attrs fileSize] > 50 * 1024 * 1024) {
                    [isos addObject:file];
                    [seen addObject:file];
                }
            }
        }
    };

    ARMSX2EnumerateLocalGameImages([self isoDirectory], ^(NSString* absolutePath, NSString* relativeName) {
        if ([seen containsObject:relativeName]) return;
        [isos addObject:relativeName];
        [seen addObject:relativeName];
    });
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    scanDir(docsPath);

	return isos;
}

+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)availableISOEntries {
	NSFileManager* fm = [NSFileManager defaultManager];
	NSMutableSet<NSString*>* seenPaths = [NSMutableSet set];
	NSMutableArray<NSDictionary<NSString*, id>*>* entries = [NSMutableArray array];

	void (^addPathWithName)(NSString*, NSString*, BOOL, NSString*, BOOL) = ^(NSString* path, NSString* displayName, BOOL external, NSString* source, BOOL forceGameFile) {
		if (path.length == 0 || (!forceGameFile && !ARMSX2IsSupportedGameImageAtPath(path)))
			return;

		NSString* normalizedPath = path.stringByStandardizingPath;
		NSString* entryName = displayName.length > 0 ? displayName : normalizedPath.lastPathComponent;
		NSString* key = normalizedPath.lowercaseString;
		if ([seenPaths containsObject:key])
			return;

		[seenPaths addObject:key];
		[entries addObject:@{
			@"name": entryName ?: @"",
			@"path": normalizedPath,
			@"external": @(external),
			@"source": source ?: (external ? @"External" : @"On My iPhone"),
		}];
	};

	void (^addPath)(NSString*, BOOL, NSString*) = ^(NSString* path, BOOL external, NSString* source) {
		addPathWithName(path, nil, external, source, NO);
	};

	void (^scanLocalDir)(NSString*, NSString*) = ^(NSString* dir, NSString* source) {
		NSArray<NSString*>* files = [fm contentsOfDirectoryAtPath:dir error:nil];
		for (NSString* file in files) {
			addPath([dir stringByAppendingPathComponent:file], NO, source);
		}
	};

	ARMSX2EnumerateLocalGameImages([self isoDirectory], ^(NSString* absolutePath, NSString* relativeName) {
		addPathWithName(absolutePath, relativeName, NO, @"On My iPhone", YES);
	});
	scanLocalDir([self documentsDirectory], @"On My iPhone");

	for (NSDictionary* record in ARMSX2ExternalGameDirectoryRecords()) {
		NSURL* directoryURL = ARMSX2ResolveExternalGameDirectoryRecord(record);
		if (!directoryURL)
			continue;
		if (ARMSX2ExternalGameRecordIsCloudProvider(record, directoryURL)) {
			NSLog(@"[ARMSX2Bridge] External game cloud provider list entry skipped path=%@", directoryURL.path);
			continue;
		}

		BOOL isDirectory = ARMSX2ExternalGameRecordIsDirectory(record, directoryURL);
		if (isDirectory && ARMSX2ExternalGameRecordScanDisabled(record, directoryURL)) {
			NSLog(@"[ARMSX2Bridge] External game folder scan disabled path=%@", directoryURL.path);
			continue;
		}

		BOOL alreadyActive = ARMSX2ExternalGameAccessAlreadyActive(directoryURL.path);
		BOOL startedAccess = alreadyActive ? NO : [directoryURL startAccessingSecurityScopedResource];

		NSString* source = [record[@"displayName"] isKindOfClass:NSString.class] ? record[@"displayName"] : directoryURL.lastPathComponent;
		if (!isDirectory) {
			addPathWithName(directoryURL.path, source, YES, source, YES);
			if (startedAccess)
				[directoryURL stopAccessingSecurityScopedResource];
			continue;
		}

		NSError* contentsError = nil;
		NSArray<NSURL*>* urls = [fm contentsOfDirectoryAtURL:directoryURL
		                         includingPropertiesForKeys:@[NSURLIsRegularFileKey, NSURLIsDirectoryKey]
		                                            options:NSDirectoryEnumerationSkipsHiddenFiles
		                                              error:&contentsError];
		if (!urls) {
			NSLog(@"[ARMSX2Bridge] External game folder scan failed path=%@ error=%@",
			      directoryURL.path, contentsError.localizedDescription ?: @"");
			if (startedAccess)
				[directoryURL stopAccessingSecurityScopedResource];
			continue;
		}

		for (NSURL* url in urls) {
			NSNumber* isDirectory = nil;
			if ([url getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil] && isDirectory.boolValue)
				continue;

			addPath(url.path, YES, source);
		}

		if (startedAccess)
			[directoryURL stopAccessingSecurityScopedResource];
	}

	return entries;
}

+ (nonnull NSDictionary<NSString *, NSString *> *)gameMetadataForISO:(nonnull NSString *)isoName {
	if (isoName.length == 0)
		return @{};

	NSFileManager *fm = [NSFileManager defaultManager];
	NSString *path = ARMSX2ResolveISOPath(isoName);
	NSString *fileName = path.length > 0 ? path.lastPathComponent : isoName.lastPathComponent;

	NSMutableDictionary<NSString *, NSString *> *metadata = [NSMutableDictionary dictionary];
	metadata[@"fileTitle"] = fileName.stringByDeletingPathExtension ?: fileName;

	if (![fm fileExistsAtPath:path]) {
		return metadata;
	}

    GameList::Entry entry;
    if (GameList::PopulateEntryFromPath(path.UTF8String, &entry)) {
        NSString *title = ARMSX2NSStringFromStdString(entry.GetTitle(false));
        NSString *serial = ARMSX2NSStringFromStdString(entry.serial);
        const char *regionText = GameList::RegionToString(entry.region, false);
        NSString *region = (regionText && *regionText) ? @(regionText) : nil;
        if (!region || [region isEqualToString:@"Other"]) {
            NSString *fallbackRegion = ARMSX2RegionFallbackForSerial(entry.serial);
            if (fallbackRegion.length > 0)
                region = fallbackRegion;
        }

        if (title.length > 0)
            metadata[@"title"] = title;
        if (serial.length > 0)
            metadata[@"serial"] = serial;
        if (region.length > 0)
            metadata[@"region"] = region;
        if (entry.crc != 0)
            metadata[@"crc"] = [NSString stringWithFormat:@"%08X", entry.crc];

        NSLog(@"[ARMSX2 iOS Covers] metadata %@ title=%@ serial=%@ region=%@",
              isoName, metadata[@"title"] ?: @"", metadata[@"serial"] ?: @"", metadata[@"region"] ?: @"");
    } else {
        NSLog(@"[ARMSX2 iOS Covers] metadata unavailable %@", isoName);
    }

    return metadata;
}

+ (nonnull NSDictionary<NSString *, id> *)gameSettingsForISO:(nonnull NSString *)isoName {
    NSMutableDictionary<NSString*, id>* result = ARMSX2BuildGlobalGameSettingsResult();

    GameList::Entry entry;
    NSString* resolvedPath = nil;
    if (!ARMSX2PopulateGameListEntryForISO(isoName, &entry, &resolvedPath) || entry.crc == 0) {
        NSLog(@"[ARMSX2Bridge] Game settings unavailable for %@ path=%@", isoName, resolvedPath ?: @"");
        return result;
    }

    const std::string settingsSerial = (entry.type == GameList::EntryType::ELF) ? std::string() : entry.serial;
    ARMSX2ApplyPerGameSettingsOverrides(result, settingsSerial, entry.crc);
    return result;
}

// VM-safe per-game settings for the running title. Reads the serial/crc the VM already
// holds in memory instead of re-scanning the disc image, which is what previously
// disturbed audio/loading when the runtime panel was opened over an active game.
+ (nullable NSDictionary<NSString *, id> *)gameSettingsForCurrentGame {
    if (!VMManager::HasValidVM())
        return nil;

    NSMutableDictionary<NSString*, id>* result = ARMSX2BuildGlobalGameSettingsResult();
    const std::string serial = VMManager::GetSerialForGameSettings();
    const u32 crc = VMManager::GetDiscCRC();
    if (serial.empty() && crc == 0)
        return result;

    ARMSX2ApplyPerGameSettingsOverrides(result, serial, crc);
    return result;
}

+ (void)setGameSettingsForISO:(nonnull NSString *)isoName
                       enabled:(BOOL)enabled
             upscaleMultiplier:(float)upscaleMultiplier
                   aspectRatio:(nonnull NSString *)aspectRatio
              textureFiltering:(int)textureFiltering
            hardwareMipmapping:(int)hardwareMipmapping
              blendingAccuracy:(int)blendingAccuracy
               interlaceMode:(int)interlaceMode
        trilinearFiltering:(int)trilinearFiltering
          halfPixelOffset:(int)halfPixelOffset
              roundSprite:(int)roundSprite
              alignSprite:(int)alignSprite
              mergeSprite:(int)mergeSprite
           wildArmsOffset:(int)wildArmsOffset
    textureOffsetXOverride:(BOOL)textureOffsetXOverride
           textureOffsetX:(int)textureOffsetX
    textureOffsetYOverride:(BOOL)textureOffsetYOverride
           textureOffsetY:(int)textureOffsetY
     skipDrawStartOverride:(BOOL)skipDrawStartOverride
            skipDrawStart:(int)skipDrawStart
       skipDrawEndOverride:(BOOL)skipDrawEndOverride
              skipDrawEnd:(int)skipDrawEnd
         volumeOverride:(BOOL)volumeOverride
           volumePercent:(int)volumePercent
                    eeCoreType:(int)eeCoreType
                          mtvu:(BOOL)mtvu
           eeCycleRateOverride:(BOOL)eeCycleRateOverride
                   eeCycleRate:(int)eeCycleRate
               fastBootOverride:(BOOL)fastBootOverride
                       fastBoot:(BOOL)fastBoot
                  enableCheats:(BOOL)enableCheats
                 enablePatches:(BOOL)enablePatches
              enableGameFixes:(BOOL)enableGameFixes
    enableGameDBHardwareFixes:(BOOL)enableGameDBHardwareFixes {
    GameList::Entry entry;
    NSString* resolvedPath = nil;
    if (!ARMSX2PopulateGameListEntryForISO(isoName, &entry, &resolvedPath) || entry.crc == 0) {
        NSLog(@"[ARMSX2Bridge] Game settings save rejected for %@ path=%@", isoName, resolvedPath ?: @"");
        return;
    }

    const std::string settingsSerial = (entry.type == GameList::EntryType::ELF) ? std::string() : entry.serial;
    ARMSX2WriteGameSettingsForIdentity(settingsSerial, entry.crc, enabled, upscaleMultiplier, aspectRatio,
                                        textureFiltering, hardwareMipmapping, blendingAccuracy, interlaceMode,
                                        trilinearFiltering, halfPixelOffset, roundSprite, alignSprite,
                                        mergeSprite, wildArmsOffset, textureOffsetXOverride, textureOffsetX,
                                        textureOffsetYOverride, textureOffsetY, skipDrawStartOverride,
                                        skipDrawStart, skipDrawEndOverride, skipDrawEnd,
                                        volumeOverride, volumePercent, eeCoreType, mtvu,
                                        eeCycleRateOverride, eeCycleRate, fastBootOverride, fastBoot,
                                        enableCheats, enablePatches, enableGameFixes, enableGameDBHardwareFixes);
}

+ (void)setGameSettingsForCurrentGameWithEnabled:(BOOL)enabled
                               upscaleMultiplier:(float)upscaleMultiplier
                                     aspectRatio:(nonnull NSString *)aspectRatio
                                textureFiltering:(int)textureFiltering
                              hardwareMipmapping:(int)hardwareMipmapping
                                blendingAccuracy:(int)blendingAccuracy
                                   interlaceMode:(int)interlaceMode
                              trilinearFiltering:(int)trilinearFiltering
                                 halfPixelOffset:(int)halfPixelOffset
                                     roundSprite:(int)roundSprite
                                     alignSprite:(int)alignSprite
                                     mergeSprite:(int)mergeSprite
                                  wildArmsOffset:(int)wildArmsOffset
                           textureOffsetXOverride:(BOOL)textureOffsetXOverride
                                  textureOffsetX:(int)textureOffsetX
                           textureOffsetYOverride:(BOOL)textureOffsetYOverride
                                  textureOffsetY:(int)textureOffsetY
                            skipDrawStartOverride:(BOOL)skipDrawStartOverride
                                   skipDrawStart:(int)skipDrawStart
                              skipDrawEndOverride:(BOOL)skipDrawEndOverride
                                     skipDrawEnd:(int)skipDrawEnd
                                   volumeOverride:(BOOL)volumeOverride
                                     volumePercent:(int)volumePercent
                                      eeCoreType:(int)eeCoreType
                                            mtvu:(BOOL)mtvu
                             eeCycleRateOverride:(BOOL)eeCycleRateOverride
                                     eeCycleRate:(int)eeCycleRate
                                 fastBootOverride:(BOOL)fastBootOverride
                                         fastBoot:(BOOL)fastBoot
                                    enableCheats:(BOOL)enableCheats
                                   enablePatches:(BOOL)enablePatches
                                 enableGameFixes:(BOOL)enableGameFixes
                      enableGameDBHardwareFixes:(BOOL)enableGameDBHardwareFixes {
    if (!VMManager::HasValidVM()) {
        NSLog(@"[ARMSX2Bridge] Current game settings save rejected: no valid VM");
        return;
    }

    const std::string serial = VMManager::GetSerialForGameSettings();
    const u32 crc = VMManager::GetDiscCRC();
    if (crc == 0) {
        NSLog(@"[ARMSX2Bridge] Current game settings save rejected serial=%@ crc=%08X",
              ARMSX2NSStringFromStdString(serial), crc);
        return;
    }

    ARMSX2WriteGameSettingsForIdentity(serial, crc, enabled, upscaleMultiplier, aspectRatio,
                                        textureFiltering, hardwareMipmapping, blendingAccuracy, interlaceMode,
                                        trilinearFiltering, halfPixelOffset, roundSprite, alignSprite,
                                        mergeSprite, wildArmsOffset, textureOffsetXOverride, textureOffsetX,
                                        textureOffsetYOverride, textureOffsetY, skipDrawStartOverride,
                                        skipDrawStart, skipDrawEndOverride, skipDrawEnd,
                                        volumeOverride, volumePercent, eeCoreType, mtvu,
                                        eeCycleRateOverride, eeCycleRate, fastBootOverride, fastBoot,
                                        enableCheats, enablePatches, enableGameFixes, enableGameDBHardwareFixes);

    // EmuConfig and the MTGS ring are the CPU thread's; this runs on the UI thread.
    Host::RunOnCPUThread([]() {
        if (!VMManager::HasValidVM())
            return;
        VMManager::ReloadGameSettings();
        if (MTGS::IsOpen())
            MTGS::ApplySettings();
        ARMSX2_CaptureGraphicsHackState();
    });
}

+ (nullable NSString *)linkedDiscPathForELF:(nonnull NSString *)elfName {
    NSString* resolvedPath = ARMSX2ResolveISOPath(elfName);
    if (resolvedPath.length == 0)
        return nil;

    const std::string discPath = VMManager::GetDiscOverrideFromGameSettings(resolvedPath.UTF8String);
    return discPath.empty() ? nil : ARMSX2NSStringFromStdString(discPath);
}

+ (void)setLinkedDiscPath:(nullable NSString *)discPath forELF:(nonnull NSString *)elfName {
    GameList::Entry entry;
    if (!ARMSX2PopulateGameListEntryForISO(elfName, &entry, nil) || entry.crc == 0)
        return;

    FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), false);
    INISettingsInterface si(VMManager::GetGameSettingsPath(std::string_view(), entry.crc));
    si.Load();

    NSString* trimmed = [discPath stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (trimmed.length > 0)
    {
        NSString* root = [ARMSX2NSStringFromStdString(EmuFolders::DataRoot).stringByStandardizingPath stringByAppendingString:@"/"];
        NSString* full = trimmed.stringByStandardizingPath;
        NSString* rel = [full hasPrefix:root] ? [full substringFromIndex:root.length] : trimmed;
        si.SetStringValue("EmuCore", "DiscPath", rel.UTF8String);
    }
    else
    {
        si.DeleteValue("EmuCore", "DiscPath");
    }

    si.Save();
}

+ (nonnull NSString *)clearCacheForISO:(nonnull NSString *)isoName {
    GameList::Entry entry;
    NSString* resolvedPath = nil;
    if (!ARMSX2PopulateGameListEntryForISO(isoName, &entry, &resolvedPath) || entry.crc == 0) {
        NSLog(@"[ARMSX2Bridge] Clear cache unavailable for %@ path=%@", isoName, resolvedPath ?: @"");
        return @"Cache not cleared: game identity was not found.";
    }

    NSArray<NSString*>* tokens = ARMSX2GameDataTokensForEntry(isoName, entry);
    NSInteger removed = 0;
    removed += ARMSX2RemoveMatchingGeneratedFiles(ARMSX2NSStringFromStdString(EmuFolders::Cache), tokens);
    removed += ARMSX2RemoveMatchingGeneratedFiles(NSTemporaryDirectory(), tokens);

    NSLog(@"[ARMSX2Bridge] Clear cache iso=%@ serial=%@ crc=%08X removed=%ld",
          isoName, ARMSX2NSStringFromStdString(entry.serial), entry.crc, (long)removed);
    return [NSString stringWithFormat:@"Cleared %ld generated cache item%@ for %@.",
            (long)removed, removed == 1 ? @"" : @"s", isoName.stringByDeletingPathExtension ?: isoName];
}

+ (nonnull NSString *)deleteGameDataForISO:(nonnull NSString *)isoName {
    GameList::Entry entry;
    NSString* resolvedPath = nil;
    if (!ARMSX2PopulateGameListEntryForISO(isoName, &entry, &resolvedPath) || entry.crc == 0) {
        NSLog(@"[ARMSX2Bridge] Delete game data unavailable for %@ path=%@", isoName, resolvedPath ?: @"");
        return @"Game data was not deleted: game identity was not found.";
    }

    NSInteger removed = 0;
    auto removePath = [&removed](const std::string& path) {
        if (path.empty() || !FileSystem::FileExists(path.c_str()))
            return;
        if (FileSystem::DeleteFilePath(path.c_str()))
            removed++;
    };

    for (s32 slot = -1; slot <= VMManager::NUM_SAVE_STATE_SLOTS; slot++) {
        removePath(VMManager::GetSaveStateFileName(entry.serial.c_str(), entry.crc, slot));
        removePath(VMManager::GetSaveStateFileName(entry.serial.c_str(), entry.crc, slot, true));
    }

    removePath(Patch::GetPnachFilename(entry.serial, entry.crc, true));
    removePath(Patch::GetPnachFilename(entry.serial, entry.crc, false));
    removePath(VMManager::GetGameSettingsPath(entry.serial, entry.crc));

    NSString* identity = ARMSX2CompatibilityIdentityKey(ARMSX2NSStringFromStdString(entry.serial), entry.crc);
    if (g_p44_settings_interface && identity.length > 0) {
        g_p44_settings_interface->DeleteValue("ARMSX2/JITBisectGamePresets", identity.UTF8String);
        ARMSX2ClearCompatibilityCustomFlagsForIdentity(identity);
        g_p44_settings_interface->Save();
    }

    NSArray<NSString*>* tokens = ARMSX2GameDataTokensForEntry(isoName, entry);
    removed += ARMSX2RemoveMatchingGeneratedFiles(ARMSX2NSStringFromStdString(EmuFolders::Cache), tokens);
    removed += ARMSX2RemoveMatchingGeneratedFiles(NSTemporaryDirectory(), tokens);

    NSLog(@"[ARMSX2Bridge] Delete game data iso=%@ serial=%@ crc=%08X removed=%ld",
          isoName, ARMSX2NSStringFromStdString(entry.serial), entry.crc, (long)removed);
    return [NSString stringWithFormat:@"Deleted %ld game-data item%@ for %@. Memory card contents were left intact.",
            (long)removed, removed == 1 ? @"" : @"s", isoName.stringByDeletingPathExtension ?: isoName];
}

+ (BOOL)deleteISO:(nonnull NSString *)isoName deleteGameData:(BOOL)deleteGameData {
    NSString* isoPath = ARMSX2ResolveISOPath(isoName);
    if (isoPath.length == 0)
        return NO;

    if (deleteGameData)
        [self deleteGameDataForISO:isoName];

    NSError* error = nil;
    BOOL removed = [[NSFileManager defaultManager] removeItemAtPath:isoPath error:&error];
    NSLog(@"[ARMSX2Bridge] Delete ISO iso=%@ path=%@ result=%d error=%@",
          isoName, isoPath, removed ? 1 : 0, error.localizedDescription ?: @"");
    return removed;
}

+ (void)changeDiscToISO:(nonnull NSString *)isoName completion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2SaveStateCompletion callback = [completion copy];
    NSString* isoPath = ARMSX2ResolveISOPath(isoName);
    if (!isoPath || !VMManager::HasValidVM()) {
        NSLog(@"[ARMSX2Bridge] ChangeDisc rejected iso=%@ path=%@ validVM=%d", isoName, isoPath ?: @"", VMManager::HasValidVM() ? 1 : 0);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }

    const std::string nativePath(isoPath.UTF8String ?: "");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        bool ejectResult = false;
        bool result = false;
        Host::RunOnCPUThread([&result]() {
            const CDVD_SourceType oldSource = CDVDsys_GetSourceType();
            const std::string oldPathForLog = CDVDsys_GetFile(oldSource);
            NSLog(@"[ARMSX2Bridge] ChangeDisc eject phase oldSource=%d oldPath=%@",
                  static_cast<int>(oldSource), ARMSX2NSStringFromStdString(oldPathForLog));
            result = VMManager::ChangeDisc(CDVD_SourceType::NoDisc, {});
        }, true);
        ejectResult = result;

        [NSThread sleepForTimeInterval:1.25];

        Host::RunOnCPUThread([nativePath, &result]() {
            result = VMManager::ChangeDisc(CDVD_SourceType::Iso, nativePath);
            NSLog(@"[ARMSX2Bridge] ChangeDisc insert phase newSource=%d newPath=%@ result=%d",
                  static_cast<int>(CDVDsys_GetSourceType()),
                  ARMSX2NSStringFromStdString(CDVDsys_GetFile(CDVDsys_GetSourceType())),
                  result ? 1 : 0);
        }, true);

        if (result && g_p44_settings_interface) {
            g_p44_settings_interface->SetStringValue("GameISO", "BootISO", isoName.UTF8String);
            g_p44_settings_interface->Save();
        }

        ARMSX2_PostRuntimeMenuStateChanged();
        NSLog(@"[ARMSX2Bridge] ChangeDisc iso=%@ ejectResult=%d result=%d", isoName, ejectResult ? 1 : 0, result ? 1 : 0);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

+ (void)ejectDiscWithCompletion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2SaveStateCompletion callback = [completion copy];
    if (!VMManager::HasValidVM()) {
        NSLog(@"[ARMSX2Bridge] EjectDisc rejected validVM=0");
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        bool result = false;
        Host::RunOnCPUThread([&result]() {
            result = VMManager::ChangeDisc(CDVD_SourceType::NoDisc, {});
        }, true);

        ARMSX2_PostRuntimeMenuStateChanged();
        NSLog(@"[ARMSX2Bridge] EjectDisc result=%d", result ? 1 : 0);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

// Toggle overlay visibility via position (None vs TopRight).
// Individual OSD flags are controlled by preset in SettingsStore, not here.
+ (void)setPerformanceOverlayVisible:(BOOL)visible {
    // Hidden in the config means the user never picked a corner, so give them one.
    OsdOverlayPos pos = OsdOverlayPos::None;
    if (visible) {
        pos = EmuConfig.GS.OsdPerformancePos;
        if (pos == OsdOverlayPos::None)
            pos = OsdOverlayPos::TopRight;
    }

    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetIntValue("EmuCore/GS", "OsdPerformancePos", static_cast<int>(pos));
        g_p44_settings_interface->Save();
    }

    // GSConfig belongs to the GS thread and EmuConfig to the CPU thread; this is
    // called from the UI. Both live in a bitfield, so an off-thread write can drop
    // a neighbouring flag -- including the ones that decide whether GSUpdateConfig
    // tears the device down. So write only the one this thread owns and let the
    // normal push carry it over. The position is not among the flags ImGuiOverlays
    // re-copies every frame, so without the push it would not arrive at all.
    Host::RunOnCPUThread([pos]() {
        EmuConfig.GS.OsdPerformancePos = pos;
        if (MTGS::IsOpen())
            MTGS::ApplySettings();
    });
}

+ (BOOL)isPerformanceOverlayVisible {
    // Read the INI, not GSConfig: the setter writes the INI now and hands the
    // config update to the CPU thread, so GSConfig lags a toggle by a hop and a
    // UI read-back would bounce the switch.
    if (g_p44_settings_interface) {
        return g_p44_settings_interface->GetIntValue("EmuCore/GS", "OsdPerformancePos",
            static_cast<int>(OsdOverlayPos::None)) != static_cast<int>(OsdOverlayPos::None);
    }
    return EmuConfig.GS.OsdPerformancePos != OsdOverlayPos::None;
}

+ (nonnull NSDictionary<NSString *, id> *)deviceStatsForAccessibility {
    int battery = -1, severity = 0;
    double ramGB = 0.0;
    bool lowPower = false;
    ARMSX2_iOSCopyDeviceStats(&battery, &severity, &ramGB, &lowPower);
    NSString* thermal;
    switch (severity) {
        case 2:  thermal = @"Serious"; break;
        case 1:  thermal = @"Fair"; break;
        default: thermal = @"Nominal"; break;
    }
    return @{
        @"battery": @(battery),
        @"thermalState": thermal,
        @"ramGB": @(ramGB),
        @"lowPower": @(lowPower),
    };
}

+ (void)triggerDeviceHapticLarge:(NSUInteger)large small:(NSUInteger)small {
    if (VMManager::IsEmulationOnlyMode())
        return;

    // GameEventHaptics is @MainActor-isolated; dispatch to the main queue.
    dispatch_async(dispatch_get_main_queue(), ^{
#if ARMSX2_HAS_SWIFTUI_HOST
        [SwiftUIHost triggerDeviceHapticWithLarge:large small:small];
#else
        (void)large;
        (void)small;
#endif
    });
}

+ (void)releaseNonEmulationResources:(NSUInteger)releaseFlags {
    if (releaseFlags & VMManager::EMULATION_ONLY_RELEASE_ACHIEVEMENTS) {
        void (^clearPendingNotification)(void) = ^{
            ARMSX2ClearPendingRetroAchievementsNotification();
        };
        if ([NSThread isMainThread])
            clearPendingNotification();
        else
            dispatch_async(dispatch_get_main_queue(), clearPendingNotification);
    }

    Host::RunOnCPUThread([releaseFlags]() {
        if (!VMManager::HasValidVM())
            return;

        VMManager::ReleaseNonEssentialRuntimeResources(static_cast<u32>(releaseFlags));
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter]
                postNotificationName:@"ARMSX2iOSEmulationOnlyResourcesReleased"
                object:nil];
        });
    }, false);
}

+ (BOOL)isEmulationOnlyModeActive {
    return VMManager::IsEmulationOnlyMode();
}

// Apply OSD preset — sets ALL GSConfig flags to match the preset
+ (void)applyOsdPreset:(int)preset {
    // 1 simple: clean player readout, plus the device stats line the overlay draws.
    // 2 detail: performance and renderer diagnostics.
    // 3 full: closest to Android's full stats section. 0 is off.
    const bool simple = (preset == 1);
    const bool detail = (preset == 2);
    const bool full = (preset == 3);

    const bool fps = simple || detail || full;
    const bool vps = detail || full;
    const bool speed = simple || detail || full;
    const bool cpu = simple || detail || full;
    const bool gpu = detail || full;
    const bool resolution = detail || full;
    const bool indicators = detail || full;
    const bool version = simple || detail || full;
    const bool gsStats = full;
    const bool frameTimes = full;
    const bool hardwareInfo = full;
    const bool settings = full;
    const bool inputs = full;

    // Same ownership problem as setPerformanceOverlayVisible: these are bitfield
    // members of the CPU and GS threads' configs, written here from the UI. Only the
    // CPU thread's copy gets written now. No push either, deliberately: ImGuiOverlays
    // re-copies every one of these out of EmuConfig.GS on the GS thread each frame, so
    // pushing would only add a queue drain per tap of the preset picker.
    Host::RunOnCPUThread([=]() {
        EmuConfig.GS.OsdShowFPS = fps;
        EmuConfig.GS.OsdShowVPS = vps;
        EmuConfig.GS.OsdShowSpeed = speed;
        EmuConfig.GS.OsdShowCPU = cpu;
        EmuConfig.GS.OsdShowGPU = gpu;
        EmuConfig.GS.OsdShowResolution = resolution;
        EmuConfig.GS.OsdShowGSStats = gsStats;
        EmuConfig.GS.OsdShowFrameTimes = frameTimes;
        EmuConfig.GS.OsdShowVersion = version;
        EmuConfig.GS.OsdShowHardwareInfo = hardwareInfo;
        EmuConfig.GS.OsdShowIndicators = indicators;
        EmuConfig.GS.OsdShowSettings = settings;
        EmuConfig.GS.OsdShowInputs = inputs;
        EmuConfig.GS.OsdShowVideoCapture = false;
        EmuConfig.GS.OsdShowInputRec = false;
    });
}

+ (int)emulatorVolumePercent {
    const int value = g_p44_settings_interface ?
        g_p44_settings_interface->GetIntValue("SPU2/Output", "StandardVolume", ARMSX2DefaultAudioVolumePercent) :
        ARMSX2DefaultAudioVolumePercent;
    return ARMSX2ClampInt(value, 0, ARMSX2DefaultAudioVolumePercent);
}

+ (void)setEmulatorVolumePercent:(int)value {
    if (!g_p44_settings_interface)
        return;

    const int clampedValue = ARMSX2ClampInt(value, 0, ARMSX2DefaultAudioVolumePercent);
    g_p44_settings_interface->SetIntValue("SPU2/Output", "StandardVolume", clampedValue);
    g_p44_settings_interface->SetIntValue("SPU2/Output", "FastForwardVolume", clampedValue);
    ARMSX2ScheduleINISave();

    if (!VMManager::HasValidVM())
        return;

    Host::RunOnCPUThread([clampedValue]() {
        if (!VMManager::HasValidVM())
            return;

        const std::string serial = VMManager::GetDiscSerial();
        const u32 crc = VMManager::GetDiscCRC();
        if (crc != 0) {
            INISettingsInterface si(VMManager::GetGameSettingsPath(serial, crc));
            if (si.Load() &&
                (si.ContainsValue("SPU2/Output", "StandardVolume") ||
                 si.ContainsValue("SPU2/Output", "FastForwardVolume"))) {
                return;
            }
        }

        const Pcsx2Config oldConfig(EmuConfig);
        EmuConfig.SPU2.StandardVolume = clampedValue;
        EmuConfig.SPU2.FastForwardVolume = clampedValue;
        SPU2::CheckForConfigChanges(oldConfig);
    }, false);
}

// ============================================================
// ISO / BIOS / Settings management
// ============================================================

#pragma mark - ISO boot

+ (BOOL)canResolveISO:(nonnull NSString *)isoName {
	return ARMSX2ResolveISOPath(isoName).length > 0 ? YES : NO;
}

+ (void)bootISO:(nonnull NSString *)isoName {
	if (!g_p44_settings_interface) {
		std::fprintf(stderr, "@@BOOT_SET_ISO@@ status=no_settings input=\"%s\"\n", isoName ? isoName.UTF8String : "");
		std::fflush(stderr);
		return;
	}
	ARMSX2FlushINISave(); // persist deferred base-setting writes before booting
	NSString* resolvedPath = ARMSX2ResolveISOPath(isoName);
	NSString* bootValue = isoName.isAbsolutePath ? (resolvedPath ?: isoName) : isoName;
	if (bootValue.isAbsolutePath) {
		BOOL accessActive = ARMSX2StartExternalGameDirectoryAccessForPathSafe(bootValue);
		NSLog(@"[ARMSX2Bridge] bootISO external access path=%@ active=%d", bootValue, accessActive ? 1 : 0);
	}
	std::fprintf(stderr, "@@BOOT_SET_ISO@@ input=\"%s\" boot=\"%s\" resolved=\"%s\" resolved_exists=%d absolute=%d\n",
		isoName ? isoName.UTF8String : "", bootValue ? bootValue.UTF8String : "", resolvedPath ? resolvedPath.UTF8String : "",
		resolvedPath.length > 0 ? 1 : 0, bootValue.isAbsolutePath ? 1 : 0);
	std::fflush(stderr);
	g_p44_settings_interface->SetStringValue("GameISO", "BootISO", bootValue.UTF8String);
	const bool fastBoot = g_p44_settings_interface->GetBoolValue(
		"GameISO", "FastBoot",
		g_p44_settings_interface->GetBoolValue("EmuCore", "EnableFastBoot", false));
	g_p44_settings_interface->SetBoolValue("GameISO", "FastBoot", fastBoot);
	g_p44_settings_interface->SetBoolValue("EmuCore", "EnableFastBoot", fastBoot);
	g_p44_settings_interface->Save();
	ARMSX2ApplyCompatibilityPresetForISOName(bootValue);
	std::fprintf(stderr, "@@BOOT_FASTBOOT_SET@@ value=%d source=settings\n", fastBoot ? 1 : 0);
	std::fflush(stderr);
	NSLog(@"bootISO: set BootISO=%@ resolved=%@", bootValue, resolvedPath ?: @"");
}

#pragma mark - BIOS management

+ (nonnull NSString *)biosDirectory {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *biosDir = [docsPath stringByAppendingPathComponent:@"bios"];
    [[NSFileManager defaultManager] createDirectoryAtPath:biosDir withIntermediateDirectories:YES attributes:nil error:nil];
    return biosDir;
}

+ (nonnull NSArray<ARMSX2BIOSInfo *> *)availableBIOSInfos {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableSet *seen = [NSMutableSet set];
    NSMutableArray<ARMSX2BIOSInfo *> *bioses = [NSMutableArray array];

    // Helper block: list all imported BIOS candidates, including small companion ROMs.
    void (^scanDir)(NSString *) = ^(NSString *dir) {
        NSArray *files = [fm contentsOfDirectoryAtPath:dir error:nil];
        for (NSString *file in files) {
            if ([seen containsObject:file]) continue;
            NSString *ext = file.pathExtension.lowercaseString;
            if ([ext isEqualToString:@"bin"] || [ext isEqualToString:@"rom"]) {
                NSString *fullPath = [dir stringByAppendingPathComponent:file];
                NSDictionary *attrs = [fm attributesOfItemAtPath:fullPath error:nil];
                unsigned long long sz = [attrs fileSize];
                if (sz > 0 && sz <= 50 * 1024 * 1024) {
                    [bioses addObject:ARMSX2MakeBIOSInfo(file, dir)];
                    [seen addObject:file];
                }
            }
        }
    };

    scanDir([self biosDirectory]);
    [bioses sortUsingComparator:^NSComparisonResult(ARMSX2BIOSInfo *lhs, ARMSX2BIOSInfo *rhs) {
        if (lhs.valid != rhs.valid)
            return lhs.valid ? NSOrderedAscending : NSOrderedDescending;
        return [lhs.fileName localizedCaseInsensitiveCompare:rhs.fileName];
    }];
    return bioses;
}

+ (nonnull NSString *)defaultBIOSName {
    if (!g_p44_settings_interface) return @"";
    std::string val = g_p44_settings_interface->GetStringValue("Filenames", "BIOS", "");
    return [NSString stringWithUTF8String:val.c_str()];
}

+ (void)setDefaultBIOS:(nonnull NSString *)biosName {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetStringValue("Filenames", "BIOS", biosName.UTF8String);
    g_p44_settings_interface->Save();
    EmuConfig.BaseFilenames.Bios = biosName.UTF8String;
    NSLog(@"setDefaultBIOS: %@", biosName);
}

#pragma mark - Favorites

+ (BOOL)isFavorite:(nonnull NSString *)isoName {
    if (!g_p44_settings_interface) return NO;
    return g_p44_settings_interface->GetBoolValue("Favorites", isoName.UTF8String, false);
}

+ (void)setFavorite:(nonnull NSString *)isoName favorite:(BOOL)favorite {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetBoolValue("Favorites", isoName.UTF8String, favorite);
    g_p44_settings_interface->Save();
}

#pragma mark - INI generic getter/setter

+ (int)getINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def {
    if (!g_p44_settings_interface) return def;
    return g_p44_settings_interface->GetIntValue(section.UTF8String, key.UTF8String, def);
}

+ (BOOL)getINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def {
    if (!g_p44_settings_interface) return def;
    return g_p44_settings_interface->GetBoolValue(section.UTF8String, key.UTF8String, def);
}

+ (float)getINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def {
    if (!g_p44_settings_interface) return def;
    return g_p44_settings_interface->GetFloatValue(section.UTF8String, key.UTF8String, def);
}

+ (nonnull NSString *)getINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def {
    if (!g_p44_settings_interface) return def;
    std::string val = g_p44_settings_interface->GetStringValue(section.UTF8String, key.UTF8String, def.UTF8String);
    return [NSString stringWithUTF8String:val.c_str()];
}

+ (void)setINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value {
    if (!g_p44_settings_interface) return;
    if (ARMSX2RetroAchievementsHardcoreActive() &&
        std::strcmp(section.UTF8String, "EmuCore/Speedhacks") == 0 &&
        std::strcmp(key.UTF8String, "EECycleRate") == 0 && value < 0) {
        ARMSX2LogRetroAchievementsHardcoreBlock("ee_underclock");
        value = 0;
    }
    g_p44_settings_interface->SetIntValue(section.UTF8String, key.UTF8String, value);
    ARMSX2ScheduleINISave();
}

+ (void)setINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value {
    if (!g_p44_settings_interface) return;
    if (ARMSX2ShouldBlockRetroAchievementsHardcoreBoolSetting(section.UTF8String, key.UTF8String, value))
        value = NO;
    g_p44_settings_interface->SetBoolValue(section.UTF8String, key.UTF8String, value);
    ARMSX2ScheduleINISave();
}

+ (void)setINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value {
    if (!g_p44_settings_interface) return;
    float valueToStore = value;
    valueToStore = ARMSX2EnforceRetroAchievementsHardcoreFloatSetting(section.UTF8String, key.UTF8String, valueToStore);
    if (std::strcmp(section.UTF8String, "Framerate") == 0 && std::strcmp(key.UTF8String, "NominalScalar") == 0)
        valueToStore = ARMSX2NormalizeIOSNominalScalar(valueToStore);

    g_p44_settings_interface->SetFloatValue(section.UTF8String, key.UTF8String, valueToStore);
    ARMSX2ScheduleINISave();
    ARMSX2ApplyLiveFloatSetting(section.UTF8String, key.UTF8String, valueToStore);
}

+ (void)setINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetStringValue(section.UTF8String, key.UTF8String, value.UTF8String);
    ARMSX2ScheduleINISave();
}

+ (void)clearINISection:(nonnull NSString *)section {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->ClearSection(section.UTF8String);
    ARMSX2ScheduleINISave();
}

// Reloads settings into the running VM and pushes graphics options to the GS
// thread so visual changes take effect without restarting the game. Safe to call
// when no VM/GS is open (it returns without doing anything).
+ (void)applyGraphicsSettingsNow
{
    if (!VMManager::HasValidVM())
        return;

    // ApplySettings owns EmuConfig and resets the JIT caches, and MTGS::ApplySettings pushes to
    // the single-producer ring — both the CPU thread's, and this runs on the UI thread.
    //
    // The push looks redundant (ApplySettings ends in CheckForGSConfigChanges, which pushes
    // for us) but it is not: applyOsdPreset and setPerformanceOverlayVisible still pre-write
    // EmuConfig.GS, so the reload can find nothing changed and skip its own push. They are
    // queued onto this same thread now, so ordering is defined, but the pre-write remains.
    Host::RunOnCPUThread([]() {
        VMManager::ApplySettings();
        if (MTGS::IsOpen())
            MTGS::ApplySettings();
        ARMSX2_CaptureGraphicsHackState();
    });
}

// The player's value and the value the game is running are two different things, and
// the settings screen could only ever see the first one. MaskUserHacks,
// MaskUpscalingHacks and the GameDB all sit in between and all of them settle on the
// CPU thread, so snapshot there after an apply and let the UI read the copy.
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
    if (!g_p44_settings_interface)
        return;

    const ARMSX2GraphicsHackDescriptor* descriptor = ARMSX2FindGraphicsHack(iniKey.UTF8String);
    if (!descriptor)
        return;

    const u32 bit = 1u << static_cast<u32>(descriptor->override_id);
    u32 mask = static_cast<u32>(g_p44_settings_interface->GetIntValue("EmuCore/GS", "UserHackOverrides", 0));
    mask = pinned ? (mask | bit) : (mask & ~bit);
    g_p44_settings_interface->SetIntValue("EmuCore/GS", "UserHackOverrides", static_cast<int>(mask));
    ARMSX2ScheduleINISave();
}

// Force any deferred base-settings INI write to disk immediately.
+ (void)flushINISettings
{
    ARMSX2FlushINISave();
}

// Probes whether MetalFX Spatial upscaling is available on this device. This is
// a standalone check that works from the main menu before any GS device exists,
// so the settings UI can decide whether to show the Upscaler section at all. It
// returns NO on pre-iOS-16, the simulator (statically compiled out), and any
// GPU that fails the framework capability probe.
+ (BOOL)isMetalFXSupported {
#if ARMSX2_HAS_METALFX
	if (@available(iOS 16.0, *)) {
		MRCOwned<id<MTLDevice>> device = MRCTransfer(MTLCreateSystemDefaultDevice());
		if (!device)
			return NO;
		return [MTLFXSpatialScalerDescriptor supportsDevice:device];
	}
	return NO;
#else
	// iOS Simulator build: MetalFX framework absent at compile time.
	return NO;
 #endif
 }

// Whether librashader was compiled into this build at all. Read off the define rather
// than probed: a build can carry the bundled presets and no librashader, so a preset
// file on disk says nothing, and a failed apply says it far too late. Swift cannot see
// a C++ define, so this is the only way the settings UI learns to leave the shader
// section out entirely.
+ (BOOL)isShaderChainSupported {
#ifdef ARMSX2_HAS_LIBRASHADER
	return YES;
#else
	return NO;
#endif
}

#pragma mark - Shader chain parameters

// Loads and frees ITS OWN preset handle, because creating a filter chain consumes the preset
// outright — reusing the renderer's would leave nothing to enumerate. Reading a preset is pure
// file parsing, so this needs no Metal device and no running VM.
+ (nullable NSString *)shaderPresetParametersAtPath:(nonnull NSString *)path {
#ifndef ARMSX2_HAS_LIBRASHADER
    return nil;
#else
    const char* filename = path.UTF8String;
    if (!filename)
        return nil;

    libra_shader_preset_t preset = nullptr;
    libra_error_t err = libra_preset_create(filename, &preset);
    if (err)
    {
        libra_error_free(&err);
        return nil;
    }

    libra_preset_param_list_t params = {};
    err = libra_preset_get_runtime_params(&preset, &params);
    if (err)
    {
        libra_error_free(&err);
        libra_preset_free(&preset);
        return nil;
    }

    const auto escape = [](const char* s) {
        std::string out;
        if (!s)
            return out;
        for (const char* p = s; *p; ++p)
        {
            switch (*p)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(*p) >= 0x20)
                        out += *p;
                    break;
            }
        }
        return out;
    };

    // A shader author's range can be non-finite, and "%g" would spell that "nan" or "inf",
    // which is not valid JSON and would cost the whole list rather than the one number.
    const auto number = [](float value) {
        if (!std::isfinite(value))
            return std::string("null");
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
        return std::string(buffer);
    };

    std::string json("[");
    for (uint64_t i = 0; i < params.length; i++)
    {
        const libra_preset_param_t& p = params.parameters[i];
        if (i)
            json += ',';
        json += "{\"name\":\"" + escape(p.name);
        json += "\",\"description\":\"" + escape(p.description);
        json += "\",\"initial\":" + number(p.initial);
        json += ",\"minimum\":" + number(p.minimum);
        json += ",\"maximum\":" + number(p.maximum);
        json += ",\"step\":" + number(p.step) + "}";
    }
    json += ']';

    // free_runtime_params takes the list BY VALUE, and the preset is still ours to free —
    // unlike chain creation, reading the parameters does not consume it.
    libra_preset_free_runtime_params(params);
    libra_preset_free(&preset);

    return [NSString stringWithUTF8String:json.c_str()];
#endif
}

// Deliberately NOT behind ARMSX2_HAS_LIBRASHADER: the store is plain values and its consumer
// is already stubbed out in a build without librashader, so guarding here would only add a
// second way for the feature to vanish silently.
+ (void)setShaderChainParameters:(nonnull NSDictionary<NSString *, NSNumber *> *)params forPreset:(nonnull NSString *)preset {
    std::vector<std::pair<std::string, float>> values;
    values.reserve(params.count);
    for (NSString* name in params)
    {
        const char* utf8 = name.UTF8String;
        if (!utf8)
            continue;
        values.emplace_back(utf8, params[name].floatValue);
    }

    const char* path = preset.UTF8String;
    GSDevice::SetShaderChainParams(path ? std::string(path) : std::string(), std::move(values));
}

#pragma mark - Frame-time history

// Returns the 150-sample PerformanceMetrics frame-time history (read-only).
// Each sample is boxed as an NSNumber so Swift sees `[NSNumber]`.
+ (nonnull NSArray<NSNumber *> *)frameTimeHistory {
    const PerformanceMetrics::FrameTimeHistory& history = PerformanceMetrics::GetFrameTimeHistory();
    NSMutableArray<NSNumber *>* result = [NSMutableArray arrayWithCapacity:history.size()];
    for (size_t i = 0; i < history.size(); i++) {
        [result addObject:@(history[i])];
    }
    return result;
}

// Current write cursor inside the ring buffer, so callers can read the most
// recent N samples (those just before the cursor) rather than treating the
// array as a linear window.
+ (NSUInteger)frameTimeHistoryPos {
    return (NSUInteger)PerformanceMetrics::GetFrameTimeHistoryPos();
}

#pragma mark - Per-game INI getter/setter
// Reads/writes the per-game INI at VMManager::GetGameSettingsPath(serial,crc), the
// same file the game-settings and patch-enable-list helpers use. The "for current
// game" write/delete variants live-apply via VMManager::ReloadGameSettings().

+ (BOOL)hasPerGameINIValue:(nonnull NSString *)section key:(nonnull NSString *)key forISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return NO;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return NO;
    return si.ContainsValue(section.UTF8String, key.UTF8String);
}

+ (int)getPerGameINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def forISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return def;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return def;
    return si.GetIntValue(section.UTF8String, key.UTF8String, def);
}

+ (BOOL)getPerGameINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def forISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return def;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return def;
    return si.GetBoolValue(section.UTF8String, key.UTF8String, def);
}

// The per-game panel writes every field it owns when you press Save, and each write
// used to queue a reload of its own. One tap came out the other side as seventy-odd
// full config reloads, each re-reading the INI, re-running GameDB and rebuilding the
// GS config. Let the last write in a burst be the one that reloads.
static void ARMSX2RequestPerGameSettingsReload()
{
    static std::atomic<uint64_t> s_generation{0};
    const uint64_t mine = s_generation.fetch_add(1, std::memory_order_relaxed) + 1;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.05 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
            // Someone wrote after us, so they own the reload.
            if (s_generation.load(std::memory_order_relaxed) != mine)
                return;

            // EmuConfig and the MTGS ring are the CPU thread's; this runs on the UI thread.
            Host::RunOnCPUThread([]() {
                VMManager::ReloadGameSettings();
                ARMSX2_ApplyEffectivePresentFPSCap();
                if (MTGS::IsOpen())
                    MTGS::ApplySettings();
                ARMSX2_CaptureGraphicsHackState();
            });
        });
}

+ (void)setPerGameINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value forISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    si.Load();
    si.SetIntValue(section.UTF8String, key.UTF8String, value);
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
}

+ (void)setPerGameINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value forISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    si.Load();
    si.SetBoolValue(section.UTF8String, key.UTF8String, value);
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
}

+ (void)deletePerGameINIValue:(nonnull NSString *)section key:(nonnull NSString *)key forISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return;
    si.DeleteValue(section.UTF8String, key.UTF8String);
    si.RemoveEmptySections();
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
}

+ (BOOL)hasPerGameINIValueForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return NO;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return NO;
    return si.ContainsValue(section.UTF8String, key.UTF8String);
}

+ (int)getPerGameINIIntForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return def;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return def;
    return si.GetIntValue(section.UTF8String, key.UTF8String, def);
}

+ (BOOL)getPerGameINIBoolForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return def;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return def;
    return si.GetBoolValue(section.UTF8String, key.UTF8String, def);
}

+ (void)setPerGameINIIntForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    si.Load();
    si.SetIntValue(section.UTF8String, key.UTF8String, value);
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
    ARMSX2RequestPerGameSettingsReload();
}

+ (void)setPerGameINIBoolForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    si.Load();
    si.SetBoolValue(section.UTF8String, key.UTF8String, value);
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
    ARMSX2RequestPerGameSettingsReload();
}

+ (float)getPerGameINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def forISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return def;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return def;
    return si.GetFloatValue(section.UTF8String, key.UTF8String, def);
}

+ (void)setPerGameINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value forISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    si.Load();
    si.SetFloatValue(section.UTF8String, key.UTF8String, value);
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
}

+ (float)getPerGameINIFloatForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return def;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return def;
    return si.GetFloatValue(section.UTF8String, key.UTF8String, def);
}

+ (void)setPerGameINIFloatForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    si.Load();
    si.SetFloatValue(section.UTF8String, key.UTF8String, value);
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
    ARMSX2RequestPerGameSettingsReload();
}

+ (void)deletePerGameINIValueForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return;
    si.DeleteValue(section.UTF8String, key.UTF8String);
    si.RemoveEmptySections();
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
    ARMSX2RequestPerGameSettingsReload();
}

+ (nonnull NSString *)perGameIdentityKeyForCurrentGame {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return @"";
    return [NSString stringWithFormat:@"%s_%08X", serial.c_str(), (unsigned int)crc];
}

+ (nonnull NSString *)perGameIdentityKeyForISO:(nonnull NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return @"";
    return [NSString stringWithFormat:@"%s_%08X", serial.c_str(), (unsigned int)crc];
}

+ (int)limiterMode
{
    if (!VMManager::HasValidVM())
        return static_cast<int>(LimiterModeType::Nominal);

    return static_cast<int>(VMManager::GetLimiterMode());
}

+ (void)setLimiterMode:(int)mode
{
    const bool hasValidVM = VMManager::HasValidVM();
    std::fprintf(stderr, "@@LIMITER_MODE_REQUEST@@ mode=%d valid=%d state=%d\n",
        mode, hasValidVM ? 1 : 0, static_cast<int>(VMManager::GetState()));
    std::fflush(stderr);

    if (!hasValidVM)
        return;

    LimiterModeType limiterMode = LimiterModeType::Nominal;
    switch (mode) {
    case static_cast<int>(LimiterModeType::Turbo):
        limiterMode = LimiterModeType::Turbo;
        break;
    case static_cast<int>(LimiterModeType::Slomo):
        limiterMode = LimiterModeType::Slomo;
        break;
    case static_cast<int>(LimiterModeType::Unlimited):
        limiterMode = LimiterModeType::Unlimited;
        break;
    default:
        break;
    }

    if (ARMSX2RetroAchievementsHardcoreActive() && limiterMode == LimiterModeType::Slomo) {
        ARMSX2LogRetroAchievementsHardcoreBlock("slomo_limiter_mode");
        limiterMode = LimiterModeType::Nominal;
    }

    Host::RunOnCPUThread([limiterMode]() {
        if (!VMManager::HasValidVM())
            return;

        const LimiterModeType previousMode = VMManager::GetLimiterMode();
        VMManager::SetLimiterMode(limiterMode);
        const LimiterModeType appliedMode = VMManager::GetLimiterMode();
        // Update cap suspension after the VM mode changes on the same CPU-thread
        // task. This avoids a settings reload racing Turbo and restoring a cap
        // using the previous limiter mode.
        GSSetPresentCapSuspended(
            appliedMode == LimiterModeType::Turbo && GSGetMaxPresentInterval() != 0);
        std::fprintf(stderr,
            "@@LIMITER_MODE@@ before=%d after=%d target=%.3f nominal=%.3f turbo=%.3f slomo=%.3f\n",
            static_cast<int>(previousMode), static_cast<int>(appliedMode), VMManager::GetTargetSpeed(),
            EmuConfig.EmulationSpeed.NominalScalar, EmuConfig.EmulationSpeed.TurboScalar,
            EmuConfig.EmulationSpeed.SlomoScalar);
        std::fflush(stderr);
    }, false);
}

static void ARMSX2SetPresentFPSCapValue(double fps)
{
    const double requestedFPS = std::isfinite(fps) ? std::clamp(static_cast<double>(fps), 0.0, 1000.0) : 0.0;
    const u32 requestedMilliFPS = requestedFPS > 0.0 ?
        static_cast<u32>(std::llround(requestedFPS * 1000.0)) : 0u;

    // 60 FPS and higher use the native/default path rather than a custom
    // presentation cap. Publishing a zero interval preserves master's original
    // VM pacing, rendering, and submission behavior. Fractional rates below 60,
    // such as 59.970, remain explicit caps.
    const bool customCapActive = requestedMilliFPS != 0 && requestedMilliFPS < 60000;
    const u32 displayFPS = customCapActive ? static_cast<u32>(std::lround(requestedFPS)) : 0u;
    const u32 milliFPS = customCapActive ? requestedMilliFPS : 0u;
    const u64 interval = customCapActive
        ? static_cast<u64>(std::llround(static_cast<double>(GetTickFrequency()) / requestedFPS))
        : 0u;

    GSSetMaxPresentFps(displayFPS, interval, milliFPS);
    GSSetPresentCapRenderSkip(customCapActive);
    GSSetPresentCapSuspended(customCapActive && VMManager::HasValidVM() &&
        VMManager::GetLimiterMode() == LimiterModeType::Turbo);
}

// Older iOS per-game profiles encoded their presentation target in
// Framerate/NominalScalar. Convert the active profile once so loading it no
// longer slows CPU/audio timing, while retaining the selected display cadence.
// This runs on the CPU thread before the effective cap is read.
static bool ARMSX2MigrateLegacyPerGamePresentFPSCap()
{
    if (!VMManager::HasValidVM())
        return false;

    bool migrated = false;
    {
        auto lock = Host::GetSettingsLock();
        SettingsInterface* const game_layer = Host::Internal::GetGameSettingsLayer();
        if (!game_layer ||
            !game_layer->ContainsValue("Framerate", "NominalScalar") ||
            game_layer->ContainsValue("ARMSX2iOS/FramePacing", "TargetFPS"))
        {
            return false;
        }

        const float scalar = game_layer->GetFloatValue("Framerate", "NominalScalar", 1.0f);
        if (!std::isfinite(scalar) || scalar >= 5.0f || std::abs(scalar - 1.0f) < 0.002f)
            return false;

        SettingsInterface* const layered = Host::GetSettingsInterface();
        const float base_fps = layered ?
            layered->GetFloatValue("EmuCore/GS", "FramerateNTSC", 59.94f) : 59.94f;
        const float target_fps = std::clamp(scalar * std::max(base_fps, 1.0f), 15.0f, 120.0f);

        game_layer->SetFloatValue("ARMSX2iOS/FramePacing", "TargetFPS", target_fps);
        game_layer->SetFloatValue("Framerate", "NominalScalar", 1.0f);
        Error error;
        if (!game_layer->Save(&error))
        {
            Console.Error("Failed to migrate per-game presentation FPS cap: %s", error.GetDescription().c_str());
            return false;
        }
        migrated = true;
    }

    if (migrated)
        VMManager::ReloadGameSettings();
    return migrated;
}

extern "C" void ARMSX2_ApplyEffectivePresentFPSCap(void)
{
    ARMSX2MigrateLegacyPerGamePresentFPSCap();

    float nominalScalar = 1.0f;
    float targetFPS = 60.0f;
    {
        auto lock = Host::GetSettingsLock();
        SettingsInterface* const si = Host::GetSettingsInterface();
        if (si)
        {
            nominalScalar = si->GetFloatValue("Framerate", "NominalScalar", 1.0f);
            targetFPS = si->GetFloatValue("ARMSX2iOS/FramePacing", "TargetFPS", 60.0f);
        }
    }

    const bool limiterEnabled = !std::isfinite(nominalScalar) || nominalScalar < 5.0f;
    ARMSX2SetPresentFPSCapValue(limiterEnabled ? (std::isfinite(targetFPS) ? targetFPS : 60.0f) : 0.0f);
    if (!VMManager::HasValidVM())
        GSSetPresentCapSuspended(false);
}

+ (void)setPresentFPSCap:(float)fps
{
    // With a running VM the layered settings interface is authoritative: a
    // per-game TargetFPS must continue to win when the global value changes.
    if (VMManager::HasValidVM())
        Host::RunOnCPUThread([]() { ARMSX2_ApplyEffectivePresentFPSCap(); }, false);
    else
        ARMSX2SetPresentFPSCapValue(fps);
}

#pragma mark - Compatibility Lab

+ (BOOL)getJITBisectFlag:(nonnull NSString *)key defaultValue:(BOOL)def
{
    BOOL value = def;
    if (g_p44_settings_interface)
        value = g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", key.UTF8String, def);

    ARMSX2ApplyJITBisectFlag(key, value);
    return value;
}

+ (void)setJITBisectFlag:(nonnull NSString *)key value:(BOOL)value
{
    ARMSX2ApplyJITBisectFlag(key, value);
    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", key.UTF8String, value);
        g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", ARMSX2CompatibilityProfileCustom.UTF8String);
        g_p44_settings_interface->Save();
    }
    NSString* identity = ARMSX2CurrentCompatibilityIdentityKey();
    if (identity.length > 0)
        ARMSX2SaveCompatibilityCustomFlagsForIdentity(identity);
    NSLog(@"[ARMSX2Bridge] Compatibility Lab %@ %@", key, value ? @"ON" : @"OFF");
}

+ (nonnull NSString *)compatibilityPresetForCurrentGame
{
    return ARMSX2CurrentCompatibilityProfileFromSettings();
}

+ (nonnull NSString *)compatibilityIdentityForCurrentGame
{
    return ARMSX2CurrentCompatibilityIdentityKey();
}

+ (nonnull NSString *)compatibilityPresetForISO:(nonnull NSString *)isoName
{
    GameList::Entry entry;
    NSString* identity = ARMSX2CompatibilityIdentityForISOName(isoName, &entry);
    if (identity.length == 0)
        return ARMSX2CompatibilityProfileOff;

    NSString* title = ARMSX2NSStringFromStdString(entry.GetTitle(false));
    if (title.length == 0)
        title = isoName.stringByDeletingPathExtension ?: isoName;

    return ARMSX2ResolvedCompatibilityPreset(identity, title);
}

+ (nonnull NSString *)compatibilityIdentityForISO:(nonnull NSString *)isoName
{
    return ARMSX2CompatibilityIdentityForISOName(isoName);
}

+ (BOOL)isCompatibilityAutoGamePresetsEnabled
{
    if (!g_p44_settings_interface)
        return YES;
    return g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", "AutoGamePresets", true) ? YES : NO;
}

+ (void)setCompatibilityAutoGamePresetsEnabled:(BOOL)enabled
{
    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", "AutoGamePresets", enabled ? true : false);
        g_p44_settings_interface->Save();
    }
    NSLog(@"[ARMSX2Bridge] Compatibility auto game presets %@", enabled ? @"ON" : @"OFF");
}

+ (void)setCompatibilityPreset:(nonnull NSString *)preset forISO:(nonnull NSString *)isoName
{
    if (!g_p44_settings_interface)
        return;

    NSString* normalized = ARMSX2NormalizeCompatibilityProfile(preset);
    NSString* identity = ARMSX2CompatibilityIdentityForISOName(isoName);
    if (identity.length == 0) {
        NSLog(@"[ARMSX2Bridge] Compatibility preset save rejected iso=%@", isoName);
        return;
    }

    g_p44_settings_interface->SetStringValue("ARMSX2/JITBisectGamePresets", identity.UTF8String, normalized.UTF8String);
    if (![normalized isEqualToString:ARMSX2CompatibilityProfileCustom])
        ARMSX2ClearCompatibilityCustomFlagsForIdentity(identity);
    g_p44_settings_interface->Save();
    NSLog(@"[ARMSX2Bridge] Compatibility saved preset=%@ identity=%@ iso=%@", normalized, identity, isoName);
}

+ (BOOL)compatibilityFlag:(nonnull NSString *)flag forISO:(nonnull NSString *)isoName
{
    if (!g_p44_settings_interface)
        return NO;

    NSString* identity = ARMSX2CompatibilityIdentityForISOName(isoName);
    NSString* key = ARMSX2CompatibilityProfileFlagKey(ARMSX2NormalizeCompatibilityProfile(flag));
    if (identity.length == 0 || key.length == 0)
        return NO;

    NSString* section = ARMSX2CompatibilityCustomFlagSection(identity);
    bool value = false;
    if (g_p44_settings_interface->GetBoolValue(section.UTF8String, key.UTF8String, &value))
        return value ? YES : NO;

    NSString* activeKey = ARMSX2CompatibilityProfileFlagKey(ARMSX2ResolvedCompatibilityPreset(identity, identity));
    return (activeKey.length > 0 && [activeKey isEqualToString:key]) ? YES : NO;
}

+ (void)setCompatibilityFlag:(nonnull NSString *)flag enabled:(BOOL)enabled forISO:(nonnull NSString *)isoName
{
    if (!g_p44_settings_interface)
        return;

    NSString* identity = ARMSX2CompatibilityIdentityForISOName(isoName);
    NSString* key = ARMSX2CompatibilityProfileFlagKey(ARMSX2NormalizeCompatibilityProfile(flag));
    if (identity.length == 0 || key.length == 0) {
        NSLog(@"[ARMSX2Bridge] Compatibility custom flag rejected flag=%@ iso=%@", flag, isoName);
        return;
    }

    NSString* section = ARMSX2CompatibilityCustomFlagSection(identity);
    g_p44_settings_interface->SetStringValue("ARMSX2/JITBisectGamePresets", identity.UTF8String, ARMSX2CompatibilityProfileCustom.UTF8String);
    g_p44_settings_interface->SetBoolValue(section.UTF8String, key.UTF8String, enabled ? true : false);

    NSString* currentIdentity = ARMSX2CurrentCompatibilityIdentityKey();
    if ([identity isEqualToString:currentIdentity]) {
        ARMSX2ApplyJITBisectFlag(key, enabled);
        g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", key.UTF8String, enabled ? true : false);
        g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", ARMSX2CompatibilityProfileCustom.UTF8String);
    }

    g_p44_settings_interface->Save();
    NSLog(@"[ARMSX2Bridge] Compatibility custom flag %@ %@ identity=%@ iso=%@", key, enabled ? @"ON" : @"OFF", identity, isoName);
}

+ (void)setCompatibilityPreset:(nonnull NSString *)preset rememberForCurrentGame:(BOOL)rememberForCurrentGame
{
    NSString* normalized = ARMSX2NormalizeCompatibilityProfile(preset);
    ARMSX2ApplyCompatibilityProfile(normalized, YES, rememberForCurrentGame ? @"remember current game" : @"manual preset");

    if (rememberForCurrentGame && g_p44_settings_interface) {
        NSString* identity = ARMSX2CurrentCompatibilityIdentityKey();
        if (identity.length > 0) {
            g_p44_settings_interface->SetStringValue("ARMSX2/JITBisectGamePresets", identity.UTF8String, normalized.UTF8String);
            g_p44_settings_interface->Save();
            NSLog(@"[ARMSX2Bridge] Compatibility remembered preset=%@ identity=%@", normalized, identity);
        }
    }
}

+ (void)forgetCompatibilityPresetForCurrentGame
{
    if (!g_p44_settings_interface)
        return;

    NSString* identity = ARMSX2CurrentCompatibilityIdentityKey();
    if (identity.length == 0)
        return;

    g_p44_settings_interface->DeleteValue("ARMSX2/JITBisectGamePresets", identity.UTF8String);
    ARMSX2ClearCompatibilityCustomFlagsForIdentity(identity);
    g_p44_settings_interface->Save();
    NSString* profile = ARMSX2ResolvedCompatibilityPreset(identity, identity);
    ARMSX2ApplyCompatibilityProfile(profile, YES, [NSString stringWithFormat:@"forget %@", identity]);
    NSLog(@"[ARMSX2Bridge] Compatibility forgot preset identity=%@", identity);
}

+ (void)forgetCompatibilityPresetForISO:(nonnull NSString *)isoName
{
    if (!g_p44_settings_interface)
        return;

    NSString* identity = ARMSX2CompatibilityIdentityForISOName(isoName);
    if (identity.length == 0)
        return;

    g_p44_settings_interface->DeleteValue("ARMSX2/JITBisectGamePresets", identity.UTF8String);
    ARMSX2ClearCompatibilityCustomFlagsForIdentity(identity);
    g_p44_settings_interface->Save();
    NSLog(@"[ARMSX2Bridge] Compatibility forgot preset identity=%@ iso=%@", identity, isoName);
}

#pragma mark - VM lifecycle

+ (BOOL)isVMRunning {
    VMState st = VMManager::GetState();
    return st == VMState::Running || st == VMState::Paused;
}

+ (BOOL)hasBIOS {
    if (EmuConfig.BaseFilenames.Bios.empty()) return NO;
    std::string fullPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
    return FileSystem::FileExists(fullPath.c_str());
}

+ (void)requestVMBoot {
	std::string bootISO;
	if (g_p44_settings_interface)
		bootISO = g_p44_settings_interface->GetStringValue("GameISO", "BootISO", "");
	const std::string biosPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
	std::fprintf(stderr, "@@BOOT_REQUEST@@ posted=1 has_bios=%d bios=\"%s\" boot_iso=\"%s\"\n",
		(!EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(biosPath.c_str())) ? 1 : 0,
		EmuConfig.BaseFilenames.Bios.c_str(), bootISO.c_str());
	std::fflush(stderr);
    [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSRequestVMBoot" object:nil];
}

+ (void)requestVMShutdown {
    [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSRequestVMShutdown" object:nil];
}

+ (void)testControllerRumble {
    ARMSX2_iOSTestGamepadRumble();
}

#pragma mark - Save states

+ (BOOL)hasValidSaveStateGame {
    return ARMSX2GetCurrentSaveStateIdentity(nullptr, nullptr);
}

+ (nonnull NSArray<ARMSX2SaveStateSlotInfo *> *)saveStateSlots {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc))
        return @[];

    NSMutableArray<ARMSX2SaveStateSlotInfo *> *slots = [NSMutableArray arrayWithCapacity:VMManager::NUM_SAVE_STATE_SLOTS];
    NSFileManager *fm = [NSFileManager defaultManager];

    for (s32 slot = 1; slot <= VMManager::NUM_SAVE_STATE_SLOTS; slot++) {
        const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot);
        const BOOL occupied = !path.empty() && FileSystem::FileExists(path.c_str());
        NSString *nsPath = ARMSX2NSStringFromStdString(path);

        ARMSX2SaveStateSlotInfo *info = [ARMSX2SaveStateSlotInfo new];
        info.slot = slot;
        info.occupied = occupied;
        info.filePath = nsPath;
        info.fileName = ARMSX2NSStringFromStringView(Path::GetFileName(path));

        if (occupied) {
            NSDictionary<NSFileAttributeKey, id> *attrs = [fm attributesOfItemAtPath:nsPath error:nil];
            info.modifiedDate = attrs[NSFileModificationDate];
            info.previewPNGData = ARMSX2ReadSaveStatePreviewPNG(path);
        }

        [slots addObject:info];
    }

    return slots;
}

+ (void)saveStateToSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion {
    const s32 nativeSlot = static_cast<s32>(slot);
    ARMSX2SaveStateCompletion callback = [completion copy];
    std::string serial;
    u32 crc = 0;
    if (nativeSlot < 1 || nativeSlot > VMManager::NUM_SAVE_STATE_SLOTS || !ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        NSLog(@"[ARMSX2 iOS SaveState] save rejected slot=%d validGame=0", nativeSlot);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }

    const std::string targetPath = VMManager::GetSaveStateFileName(serial.c_str(), crc, nativeSlot);
    NSLog(@"[ARMSX2 iOS SaveState] save requested slot=%d path=%@", nativeSlot, ARMSX2NSStringFromStdString(targetPath));

    dispatch_async(ARMSX2SaveStateQueue(), ^{
        bool result = false;
        VMManager::WaitForSaveStateFlush();
        Host::RunOnCPUThread([nativeSlot, &result]() {
            NSLog(@"[ARMSX2 iOS SaveState] CPU save start slot=%d", nativeSlot);
            if (MemcardBusy::IsBusy()) {
                NSLog(@"[ARMSX2 iOS SaveState] CPU save rejected slot=%d reason=memory-card-busy", nativeSlot);
                result = false;
                return;
            }

            if (!ARMSX2FlushNVRAMAndMemoryCards("pre-save-state")) {
                NSLog(@"[ARMSX2 iOS SaveState] CPU save rejected slot=%d reason=pre-save-flush-failed", nativeSlot);
                result = false;
                return;
            }

            std::string saveError;
            VMManager::SaveStateToSlot(nativeSlot, false, [&saveError](const std::string& error) {
                saveError = error;
            });
            result = saveError.empty();
            if (result)
                ARMSX2FlushNVRAMAndMemoryCards("post-save-state");
            else
                NSLog(@"[ARMSX2 iOS SaveState] CPU save failed slot=%d error=%@", nativeSlot, ARMSX2NSStringFromStdString(saveError));
            NSLog(@"[ARMSX2 iOS SaveState] CPU save finished slot=%d result=%d", nativeSlot, result ? 1 : 0);
        }, true);

        if (result) {
            VMManager::WaitForSaveStateFlush();
            result = targetPath.empty() ? result : FileSystem::FileExists(targetPath.c_str());
        }

        NSLog(@"[ARMSX2 iOS SaveState] save finished slot=%d result=%d exists=%d",
              nativeSlot, result ? 1 : 0, (!targetPath.empty() && FileSystem::FileExists(targetPath.c_str())) ? 1 : 0);

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

+ (void)loadStateFromSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion {
    const s32 nativeSlot = static_cast<s32>(slot);
    ARMSX2SaveStateCompletion callback = [completion copy];
    std::string serial;
    u32 crc = 0;
    if (nativeSlot < 1 || nativeSlot > VMManager::NUM_SAVE_STATE_SLOTS || !ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        NSLog(@"[ARMSX2 iOS SaveState] load rejected slot=%d validGame=0", nativeSlot);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }

    const std::string targetPath = VMManager::GetSaveStateFileName(serial.c_str(), crc, nativeSlot);
    NSLog(@"[ARMSX2 iOS SaveState] load requested slot=%d path=%@ exists=%d",
          nativeSlot, ARMSX2NSStringFromStdString(targetPath), (!targetPath.empty() && FileSystem::FileExists(targetPath.c_str())) ? 1 : 0);

    dispatch_async(ARMSX2SaveStateQueue(), ^{
        if (ARMSX2RetroAchievementsHardcoreActive()) {
            NSLog(@"[ARMSX2 iOS SaveState] load rejected slot=%d reason=hardcore-active", nativeSlot);
            std::fprintf(stderr, "@@IOS_SAVESTATE_LOAD_BLOCKED@@ slot=%d reason=hardcore-active\n", nativeSlot);
            std::fflush(stderr);
            if (callback)
                dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
            return;
        }

        bool result = false;
        bool flushResult = false;
        VMManager::WaitForSaveStateFlush();
        Host::RunOnCPUThread([&flushResult]() {
            flushResult = ARMSX2FlushNVRAMAndMemoryCards("pre-load-state");
        }, true);

        NSInteger backupCount = 0;
        if (flushResult)
            backupCount = ARMSX2BackupAssignedMemoryCards("pre-load-state", nativeSlot, serial, crc);

        Host::RunOnCPUThread([nativeSlot, flushResult, &result]() {
            NSLog(@"[ARMSX2 iOS SaveState] CPU load start slot=%d", nativeSlot);
            if (!flushResult) {
                NSLog(@"[ARMSX2 iOS SaveState] CPU load rejected slot=%d reason=pre-load-flush-failed", nativeSlot);
                result = false;
                return;
            }

            if (MemcardBusy::IsBusy()) {
                NSLog(@"[ARMSX2 iOS SaveState] CPU load rejected slot=%d reason=memory-card-busy", nativeSlot);
                result = false;
                return;
            }

            result = VMManager::LoadStateFromSlot(nativeSlot);
            NSLog(@"[ARMSX2 iOS SaveState] CPU load finished slot=%d result=%d", nativeSlot, result ? 1 : 0);
        }, true);

        NSLog(@"[ARMSX2 iOS SaveState] load callback slot=%d result=%d memcardBackups=%ld",
              nativeSlot, result ? 1 : 0, static_cast<long>(backupCount));

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

#pragma mark - PNACH cheats/patches

+ (nullable NSString *)pnachPathForCurrentGameAsCheat:(BOOL)asCheat {
    // Note: Hardcore Mode does not block locating/creating cheat files here. Cheat
    // download/import only stores the file; the PCSX2 core refuses to apply cheats
    // while Hardcore is active, and the Swift toggle gates enabling them.

    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        NSLog(@"[ARMSX2Bridge] PNACH path unavailable: no current game identity");
        return nil;
    }

    return ARMSX2NSStringFromStdString(Patch::GetPnachFilename(serial, crc, asCheat));
}

+ (nullable NSString *)pnachPathForISO:(nonnull NSString *)isoName asCheat:(BOOL)asCheat {
    // Note: Hardcore Mode does not block locating/creating cheat files here. See
    // pnachPathForCurrentGameAsCheat: above; the core gates application, not storage.

    NSString* path = ARMSX2ResolveISOPath(isoName);
    if (path.length == 0) {
        NSLog(@"[ARMSX2Bridge] PNACH path unavailable for %@: ISO not found", isoName);
        return nil;
    }

    GameList::Entry entry;
    if (!GameList::PopulateEntryFromPath(path.UTF8String, &entry) || entry.crc == 0) {
        NSLog(@"[ARMSX2Bridge] PNACH path unavailable for %@: metadata missing", isoName);
        return nil;
    }

    return ARMSX2NSStringFromStdString(Patch::GetPnachFilename(entry.serial, entry.crc, asCheat));
}

+ (void)reloadPatches {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc))
        return;

    Host::RunOnCPUThread([serial, crc]() {
        Patch::ReloadPatches(serial, crc, true, true, true, true);
        Patch::UpdateActivePatches(true, true, true, true);
    }, false);
}

+ (NSArray<NSString *> *)patchEnableListForISO:(NSString *)isoName section:(NSString *)section key:(NSString *)key {
    NSString* path = ARMSX2ResolveISOPath(isoName);
    if (path.length == 0) return @[];

    GameList::Entry entry;
    if (!GameList::PopulateEntryFromPath(path.UTF8String, &entry) || entry.crc == 0) return @[];
    return ARMSX2PatchEnableListForIdentity(entry.serial, entry.crc, section, key);
}

+ (NSArray<NSString *> *)patchEnableListForCurrentGameSection:(NSString *)section key:(NSString *)key {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) return @[];
    return ARMSX2PatchEnableListForIdentity(serial, crc, section, key);
}

+ (void)setPatchEnableList:(NSArray<NSString *> *)values forISO:(NSString *)isoName section:(NSString *)section key:(NSString *)key {
    NSString* path = ARMSX2ResolveISOPath(isoName);
    if (path.length == 0) return;

    GameList::Entry entry;
    if (!GameList::PopulateEntryFromPath(path.UTF8String, &entry) || entry.crc == 0) return;
    ARMSX2SetPatchEnableListForIdentity(values, entry.serial, entry.crc, section, key);
}

+ (void)setPatchEnableListForCurrentGame:(NSArray<NSString *> *)values section:(NSString *)section key:(NSString *)key {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) return;
    ARMSX2SetPatchEnableListForIdentity(values, serial, crc, section, key);
}

#pragma mark - Memory cards

+ (nonnull NSString *)memoryCardDirectory {
    FileSystem::CreateDirectoryPath(EmuFolders::MemoryCards.c_str(), false);
    return ARMSX2NSStringFromStdString(EmuFolders::MemoryCards);
}

+ (nonnull NSArray<NSString *> *)availableMemoryCards {
    [self memoryCardDirectory];

    std::vector<AvailableMcdInfo> cards = FileMcd_GetAvailableCards(true);
    NSMutableArray<NSString *> *names = [NSMutableArray arrayWithCapacity:cards.size()];
    for (const AvailableMcdInfo& card : cards) {
        NSString* name = ARMSX2NSStringFromStdString(card.name);
        if (name.length > 0)
            [names addObject:name];
    }

    return names;
}

+ (nullable NSString *)memoryCardNameForSlot:(NSInteger)slot {
    if (slot < 1 || slot > 8)
        return nil;

    const uint nativeSlot = static_cast<uint>(slot - 1);
    char key[32];
    std::snprintf(key, sizeof(key), "Slot%u_Filename", nativeSlot + 1);

    std::string value = EmuConfig.Mcd[nativeSlot].Filename;
    if (g_p44_settings_interface)
        value = g_p44_settings_interface->GetStringValue("MemoryCards", key, value.c_str());

    return ARMSX2NSStringFromStdString(value);
}

+ (void)setMemoryCardName:(nonnull NSString *)name forSlot:(NSInteger)slot enabled:(BOOL)enabled {
    if (slot < 1 || slot > 8)
        return;

    const uint nativeSlot = static_cast<uint>(slot - 1);
    const std::string nativeName(name.UTF8String ?: "");
    char enableKey[32];
    char fileKey[32];
    std::snprintf(enableKey, sizeof(enableKey), "Slot%u_Enable", nativeSlot + 1);
    std::snprintf(fileKey, sizeof(fileKey), "Slot%u_Filename", nativeSlot + 1);

    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetBoolValue("MemoryCards", enableKey, enabled);
        g_p44_settings_interface->SetStringValue("MemoryCards", fileKey, nativeName.c_str());
        g_p44_settings_interface->Save();
    }

    EmuConfig.Mcd[nativeSlot].Enabled = enabled ? true : false;
    EmuConfig.Mcd[nativeSlot].Filename = nativeName;
    if (!enabled || nativeName.empty()) {
        EmuConfig.Mcd[nativeSlot].Type = MemoryCardType::Empty;
    } else if (const std::optional<AvailableMcdInfo> cardInfo = FileMcd_GetCardInfo(nativeName)) {
        EmuConfig.Mcd[nativeSlot].Type = cardInfo->type;
    } else {
        EmuConfig.Mcd[nativeSlot].Type = MemoryCardType::File;
    }

    NSLog(@"[ARMSX2Bridge] MemoryCard slot=%ld enabled=%d name=%@", static_cast<long>(slot), enabled ? 1 : 0, name);
}

+ (BOOL)createMemoryCardNamed:(nonnull NSString *)name sizeMB:(NSInteger)sizeMB folder:(BOOL)folder {
    [self memoryCardDirectory];

    NSString* sanitized = ARMSX2SanitizedMemoryCardName(name);
    if (sanitized.length == 0)
        return NO;

    const std::string nativeName(sanitized.UTF8String ?: "");
    const std::string fullPath(Path::Combine(EmuFolders::MemoryCards, nativeName));
    if (FileSystem::FileExists(fullPath.c_str()) || FileSystem::DirectoryExists(fullPath.c_str())) {
        NSLog(@"[ARMSX2Bridge] MemoryCard create refused, already exists: %@", sanitized);
        return NO;
    }

    const MemoryCardType cardType = folder ? MemoryCardType::Folder : MemoryCardType::File;
    const MemoryCardFileType fileType = folder ? MemoryCardFileType::Unknown : ARMSX2MemoryCardFileTypeForSizeMB(sizeMB);
    if (!folder && fileType == MemoryCardFileType::Unknown)
        return NO;

    const bool result = FileMcd_CreateNewCard(nativeName, cardType, fileType);
    NSLog(@"[ARMSX2Bridge] MemoryCard create name=%@ folder=%d size=%ld result=%d",
          sanitized, folder ? 1 : 0, static_cast<long>(sizeMB), result ? 1 : 0);
    return result ? YES : NO;
}

+ (BOOL)deleteMemoryCardNamed:(nonnull NSString *)name {
    [self memoryCardDirectory];

    const std::string nativeName(name.UTF8String ?: "");
    // Reject anything that looks like a path; names are single on-disk entries in the cards dir.
    if (nativeName.empty() ||
        nativeName.find('/') != std::string::npos ||
        nativeName.find('\\') != std::string::npos ||
        nativeName.find("..") != std::string::npos) {
        return NO;
    }

    const std::string fullPath(Path::Combine(EmuFolders::MemoryCards, nativeName));

    bool ok = false;
    if (FileSystem::FileExists(fullPath.c_str()))
        ok = FileSystem::DeleteFilePath(fullPath.c_str());
    else if (FileSystem::DirectoryExists(fullPath.c_str()))
        ok = FileSystem::RecursiveDeleteDirectory(fullPath.c_str());
    else
        return YES; // already gone — treat as success

    // Self-heal: clear any slot still pointing at the deleted card so it does not
    // reference a stale filename at the next VM boot.
    if (ok) {
        for (uint slot = 0; slot < sizeof(EmuConfig.Mcd) / sizeof(EmuConfig.Mcd[0]); slot++) {
            if (EmuConfig.Mcd[slot].Filename == nativeName)
                [self setMemoryCardName:@"" forSlot:static_cast<NSInteger>(slot + 1) enabled:NO];
        }
    }

    NSLog(@"[ARMSX2Bridge] MemoryCard delete name=%@ result=%d", name, ok ? 1 : 0);
    return ok ? YES : NO;
}

#pragma mark - DEV9 / Network

+ (nonnull NSArray<NSString *> *)dev9NetworkAdapters {
    NSMutableOrderedSet<NSString *> *adapters = [NSMutableOrderedSet orderedSetWithObject:@"Auto"];

    struct ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (struct ifaddrs *ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr)
                continue;

            const sa_family_t family = ifa->ifa_addr->sa_family;
            if (family != AF_INET)
                continue;

            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0)
                continue;

            [adapters addObject:[NSString stringWithUTF8String:ifa->ifa_name]];
        }
        freeifaddrs(interfaces);
    }

    return adapters.array;
}

#pragma mark - RetroAchievements

+ (nonnull NSDictionary<NSString *, id> *)retroAchievementsState {
    if (!ARMSX2RetroAchievementsHardcoreAvailable && EmuConfig.Achievements.HardcoreMode)
        ARMSX2ForceRetroAchievementsHardcoreOff();

    std::string username;
    std::string displayName;
    std::string avatarPath;
    std::string gameTitle;
    std::string richPresence;
    std::string gameIconPath;
    std::string gameIconURL;
    bool loggedIn = false;
    bool hasGame = false;
    bool active = false;
    bool hardcoreActive = false;
    bool hasAchievements = false;
    bool hasLeaderboards = false;
    bool hasRichPresence = false;
    u32 gameId = 0;
    u32 points = 0;
    u32 softcorePoints = 0;
    u32 unreadMessages = 0;
    u32 unlockedAchievements = 0;
    u32 totalAchievements = 0;
    u32 unlockedPoints = 0;
    u32 totalPoints = 0;
    const std::string savedUsernameValue = Host::GetBaseStringSettingValue("Achievements", "Username");
    const bool savedUsername = !savedUsernameValue.empty();
    const bool savedToken = !Host::GetStringSettingValue("Achievements", "Token").empty();

    Achievements::UserStats userStats;
    Achievements::GameStats gameStats;
    const bool haveUserStats = Achievements::GetCurrentUserStats(&userStats);
    const bool haveGameStats = Achievements::GetCurrentGameStats(&gameStats);

    {
        auto lock = Achievements::GetLock();
        active = Achievements::IsActive();
        if (haveUserStats) {
            username = userStats.username;
            displayName = userStats.display_name;
            avatarPath = userStats.avatar_path;
            points = userStats.points;
            softcorePoints = userStats.softcore_points;
            unreadMessages = userStats.unread_messages;
            loggedIn = !username.empty();
        } else if (active) {
            if (const char* loggedInUser = Achievements::GetLoggedInUserName()) {
                username = loggedInUser;
                displayName = username;
                loggedIn = !username.empty();
            }
        }
        if (active && loggedIn && avatarPath.empty())
            avatarPath = Achievements::GetLoggedInUserBadgePath();
        hasGame = Achievements::HasActiveGame();
        if (haveGameStats) {
            hasGame = true;
            gameTitle = gameStats.title;
            richPresence = gameStats.rich_presence;
            gameIconPath = gameStats.icon_path;
            gameIconURL = gameStats.icon_url;
            gameId = gameStats.game_id;
            unlockedAchievements = gameStats.unlocked_achievements;
            totalAchievements = gameStats.total_achievements;
            unlockedPoints = gameStats.unlocked_points;
            totalPoints = gameStats.total_points;
            hasAchievements = gameStats.has_achievements;
            hasLeaderboards = gameStats.has_leaderboards;
            hasRichPresence = gameStats.has_rich_presence;
        } else if (hasGame) {
            gameTitle = Achievements::GetGameTitle();
            richPresence = Achievements::GetRichPresenceString();
            gameIconURL = Achievements::GetGameIconURL();
            gameId = Achievements::GetGameID();
            hasAchievements = Achievements::HasAchievements();
            hasLeaderboards = Achievements::HasLeaderboards();
            hasRichPresence = Achievements::HasRichPresence();
        }
        hardcoreActive = Achievements::IsHardcoreModeActive();
    }

    const bool savedLogin = savedUsername && savedToken;
    const bool loginPending = EmuConfig.Achievements.Enabled && active && !loggedIn && savedLogin;
    if (username.empty() && savedUsername) {
        username = savedUsernameValue;
        displayName = savedUsernameValue;
    }

    return @{
        @"supported": @(ARMSX2RetroAchievementsAvailable),
        @"hardcoreSupported": @(ARMSX2RetroAchievementsHardcoreAvailable),
        @"unavailableMessage": ARMSX2RetroAchievementsUnavailableMessage(),
        @"enabled": @(EmuConfig.Achievements.Enabled),
        @"active": @(active),
        @"loggedIn": @(loggedIn),
        @"savedLogin": @(savedLogin),
        @"loginPending": @(loginPending),
        @"username": ARMSX2NSStringFromStdString(username),
        @"displayName": ARMSX2NSStringFromStdString(displayName.empty() ? username : displayName),
        @"avatarPath": ARMSX2NSStringFromStdString(avatarPath),
        @"points": @(points),
        @"softcorePoints": @(softcorePoints),
        @"unreadMessages": @(unreadMessages),
        @"hardcorePreference": @(EmuConfig.Achievements.HardcoreMode),
        @"hardcoreActive": @(hardcoreActive),
        @"notifications": @(EmuConfig.Achievements.Notifications),
        @"leaderboardNotifications": @(EmuConfig.Achievements.LeaderboardNotifications),
        @"overlays": @(EmuConfig.Achievements.Overlays),
        @"hasActiveGame": @(hasGame),
        @"gameTitle": ARMSX2NSStringFromStdString(gameTitle),
        @"richPresence": ARMSX2NSStringFromStdString(richPresence),
        @"gameIconPath": ARMSX2NSStringFromStdString(gameIconPath),
        @"gameIconURL": ARMSX2NSStringFromStdString(gameIconURL),
        @"unlockedAchievements": @(unlockedAchievements),
        @"totalAchievements": @(totalAchievements),
        @"unlockedPoints": @(unlockedPoints),
        @"totalPoints": @(totalPoints),
        @"gameId": @(gameId),
        @"hasAchievements": @(hasAchievements),
        @"hasLeaderboards": @(hasLeaderboards),
        @"hasRichPresence": @(hasRichPresence),
    };
}

+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)retroAchievementsForCurrentGame {
    std::vector<Achievements::AchievementInfo> achievements;
    if (!Achievements::GetCurrentAchievementList(&achievements))
        return @[];

    NSMutableArray<NSDictionary<NSString *, id> *> *result = [NSMutableArray arrayWithCapacity:achievements.size()];
    for (const Achievements::AchievementInfo& achievement : achievements) {
        [result addObject:@{
            @"id": @(achievement.id),
            @"title": ARMSX2NSStringFromStdString(achievement.title),
            @"description": ARMSX2NSStringFromStdString(achievement.description),
            @"badgePath": ARMSX2NSStringFromStdString(achievement.badge_path),
            @"measuredProgress": ARMSX2NSStringFromStdString(achievement.measured_progress),
            @"points": @(achievement.points),
            @"unlockTime": @(achievement.unlock_time),
            @"state": @(achievement.state),
            @"category": @(achievement.category),
            @"bucket": @(achievement.bucket),
            @"unlocked": @(achievement.unlocked),
            @"measuredPercent": @(achievement.measured_percent),
            @"rarity": @(achievement.rarity),
            @"rarityHardcore": @(achievement.rarity_hardcore),
        }];
    }

    return result;
}

+ (nullable ARMSX2RetroAchievementsToastInfo *)consumePendingRetroAchievementsNotification {
    __block ARMSX2RetroAchievementsToastInfo* pending = nil;
    void (^consume)(void) = ^{
        // Hands the static's reference to `pending`, which is why this clears the pointer
        // by hand instead of calling ARMSX2ClearPendingRetroAchievementsNotification().
        // That one releases, and the autorelease at the end of this function is already
        // paying for the reference. Using it here would over-release.
        pending = s_pendingRetroAchievementsNotification;
        s_pendingRetroAchievementsNotification = nil;
    };

    if ([NSThread isMainThread]) {
        consume();
    } else {
        dispatch_sync(dispatch_get_main_queue(), consume);
    }

#if __has_feature(objc_arc)
    return pending;
#else
    return [pending autorelease];
#endif
}

+ (BOOL)isRetroAchievementsHardcoreActive {
    return ARMSX2RetroAchievementsHardcoreActive() ? YES : NO;
}

+ (void)setRetroAchievementsEnabled:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        if (EmuConfig.Achievements.Enabled == enable) {
            ARMSX2SaveBaseSettingBool("Achievements", "Enabled", enable);
            if (!enable || !ARMSX2RetroAchievementsHardcoreAvailable)
                ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", false);
            ARMSX2_PostRetroAchievementsStateChanged();
            return;
        }

        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Enabled = enable;
            if (!enable || !ARMSX2RetroAchievementsHardcoreAvailable)
                EmuConfig.Achievements.HardcoreMode = false;
            ARMSX2SaveBaseSettingBool("Achievements", "Enabled", enable);
            if (!enable || !ARMSX2RetroAchievementsHardcoreAvailable)
                ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", false);
        });
        NSLog(@"[ARMSX2Bridge] RetroAchievements enabled=%d", enable ? 1 : 0);
    });
}

+ (void)setRetroAchievementsHardcore:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        if (!ARMSX2RetroAchievementsHardcoreAvailable) {
            ARMSX2ForceRetroAchievementsHardcoreOff();
            ARMSX2_PostRetroAchievementsStateChanged();
            if (enable)
                NSLog(@"[ARMSX2Bridge] RetroAchievements hardcore rejected: unavailable");
            return;
        }

        if (EmuConfig.Achievements.HardcoreMode == enable) {
            ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", enable);
            ARMSX2_PostRetroAchievementsStateChanged();
            return;
        }

        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.HardcoreMode = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", enable);
        });
        NSLog(@"[ARMSX2Bridge] RetroAchievements hardcore=%d", enable ? 1 : 0);
    });
}

+ (void)setRetroAchievementsNotifications:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Notifications = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "Notifications", enable);
        });
    });
}

+ (void)setRetroAchievementsLeaderboards:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.LeaderboardNotifications = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "LeaderboardNotifications", enable);
        });
    });
}

+ (void)setRetroAchievementsOverlays:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Overlays = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "Overlays", enable);
        });
    });
}

+ (void)loginRetroAchievementsWithUsername:(nonnull NSString *)username password:(nonnull NSString *)password completion:(nullable ARMSX2RetroAchievementsCompletion)completion {
    NSString* trimmedUsername = [username stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSString* nativePassword = password ?: @"";
    ARMSX2RetroAchievementsCompletion callback = [completion copy];

    if (trimmedUsername.length == 0 || nativePassword.length == 0) {
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, @"Enter your RetroAchievements username and password."); });
        return;
    }

    std::string user(trimmedUsername.UTF8String ?: "");
    std::string pass(nativePassword.UTF8String ?: "");

    NSLog(@"@@RA_LOGIN_NATIVE@@ requested username=%@ enabled=%d active=%d",
        trimmedUsername, EmuConfig.Achievements.Enabled ? 1 : 0, Achievements::IsActive() ? 1 : 0);

    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        @autoreleasepool {
            if (!EmuConfig.Achievements.Enabled) {
                Pcsx2Config::AchievementsOptions old_config = EmuConfig.Achievements;
                EmuConfig.Achievements.Enabled = true;
                ARMSX2SaveBaseSettingBool("Achievements", "Enabled", true);
                Achievements::UpdateSettings(old_config);
            }

            if (!ARMSX2EnsureAchievementsClientInitialized()) {
                NSString* message = @"RetroAchievements could not initialize its network client.";
                NSLog(@"[ARMSX2Bridge] RetroAchievements login username=%@ result=0 message=%@", trimmedUsername, message);
                ARMSX2_PostRetroAchievementsStateChanged();
                if (callback)
                    dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, message); });
                return;
            }

            Error error;
            const bool result = Achievements::Login(user.c_str(), pass.c_str(), &error);
            NSString* message = result ? @"RetroAchievements login successful." :
                (error.IsValid() ? ARMSX2NSStringFromStdString(error.GetDescription()) : @"RetroAchievements login failed.");

            if (result && g_p44_settings_interface)
                g_p44_settings_interface->Save();

            NSLog(@"@@RA_LOGIN_NATIVE@@ result=%d message=%@", result ? 1 : 0, message);
            NSLog(@"[ARMSX2Bridge] RetroAchievements login username=%@ result=%d message=%@", trimmedUsername, result ? 1 : 0, message);
            ARMSX2_PostRetroAchievementsStateChanged();

            if (callback)
                dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO, message); });
        }
    });
}

+ (void)logoutRetroAchievements {
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        Achievements::Logout();
        if (g_p44_settings_interface)
            g_p44_settings_interface->Save();
        NSLog(@"[ARMSX2Bridge] RetroAchievements logout");
        ARMSX2_PostRetroAchievementsStateChanged();
    });
}

// Gamepad button mapping
extern std::atomic<bool> s_captureMode;
extern std::atomic<int>  s_capturedButton;
extern int s_buttonMap[16];

+ (void)startButtonCapture {
    s_capturedButton.store(-1);
    s_captureMode.store(true);
}

+ (void)stopButtonCapture {
    s_captureMode.store(false);
}

// Poll SDL gamepad from main thread (for settings screen when VM is not running)
+ (void)pollGamepadForCapture {
    if (!s_captureMode.load()) return;
    SDL_UpdateGamepads();
    // Keep gamepad open across polls to avoid open/close overhead
    static SDL_Gamepad* s_settingsGP = nullptr;
    if (!s_settingsGP) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count > 0) s_settingsGP = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (!s_settingsGP) return;
    if (!SDL_GamepadConnected(s_settingsGP)) {
        SDL_CloseGamepad(s_settingsGP);
        s_settingsGP = nullptr;
        return;
    }
    // SDL_PumpEvents required for GCController input to be processed
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (SDL_GetGamepadButton(s_settingsGP, (SDL_GamepadButton)b)) {
            s_capturedButton.store(b);
            break;
        }
    }
}

+ (int)capturedButton {
    return s_capturedButton.exchange(-1);
}

+ (void)setButtonMapping:(int)ps2Index toSDLButton:(int)sdlButton {
    if (ps2Index >= 0 && ps2Index < 16) {
        s_buttonMap[ps2Index] = sdlButton;
        // Persist to INI
        if (g_p44_settings_interface) {
            char key[32];
            snprintf(key, sizeof(key), "Button%d", ps2Index);
            g_p44_settings_interface->SetIntValue("ARMSX2iOS/GamepadMapping", key, sdlButton);
            g_p44_settings_interface->Save();
        }
    }
}

+ (int)getButtonMapping:(int)ps2Index {
    if (ps2Index >= 0 && ps2Index < 16) return s_buttonMap[ps2Index];
    return -1;
}

+ (void)resetButtonMappings {
    static const int defMap[16] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        -1, -1,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    };
    for (int i = 0; i < 16; i++) s_buttonMap[i] = defMap[i];
    if (g_p44_settings_interface) {
        g_p44_settings_interface->RemoveSection("ARMSX2iOS/GamepadMapping");
        g_p44_settings_interface->Save();
    }
}

@end
