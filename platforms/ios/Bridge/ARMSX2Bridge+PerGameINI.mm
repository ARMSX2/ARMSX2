// ARMSX2Bridge+PerGameINI.mm — per-game INI keys, identity and patch lists
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "VMManager.h"
#include "Patch.h"
#include "PerGameOverrides.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Error.h"

#include <vector>

// The generic per-game helpers write hack keys too, so they keep the mask in step.
static void ARMSX2SyncClaimsIfPinnedHackKey(INISettingsInterface& si, NSString* section, NSString* key)
{
    if (PerGameOverrideKeys::ClaimsAGameDBSetting([section UTF8String], [key UTF8String]))
        ARMSX2StoreDerivedPerGameHackClaims(si);
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
    if (!si.Save(&error))
        NSLog(@"[ARMSX2Bridge] Patch list save error: %@", ARMSX2NSStringFromStdString(error.GetDescription()));
}

@implementation ARMSX2Bridge (PerGameINI)

template <typename R, typename F>
static R ARMSX2ReadPerGameINI(NSString* isoName, R defaultValue, F&& read)
{
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return defaultValue;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    if (!si.Load())
        return defaultValue;
    return read(si);
}

template <typename F>
static void ARMSX2MutatePerGameINI(NSString* isoName, NSString* section, NSString* key, F&& mutate)
{
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return;
    INISettingsInterface si(ARMSX2PerGameSettingsPath(serial, crc));
    si.Load();
    mutate(si);
    ARMSX2SyncClaimsIfPinnedHackKey(si, section, key);
    Error error;
    si.Save(&error);
    if (isoName.length == 0 || ARMSX2PathIsRunningDisc(ARMSX2ResolveISOPath(isoName)))
        ARMSX2RequestPerGameSettingsReload();
}

+ (BOOL)hasPerGameINIValue:(nonnull NSString *)section key:(nonnull NSString *)key forISO:(nullable NSString *)isoName {
    return ARMSX2ReadPerGameINI(isoName, NO, [&](const INISettingsInterface& si) {
        return si.ContainsValue(section.UTF8String, key.UTF8String) ? YES : NO;
    });
}

+ (nullable NSNumber *)perGameINIBoolIfPresent:(nonnull NSString *)section key:(nonnull NSString *)key forISO:(nullable NSString *)isoName {
    return ARMSX2ReadPerGameINI<NSNumber*>(isoName, nil, [&](const INISettingsInterface& si) -> NSNumber* {
        if (!si.ContainsValue(section.UTF8String, key.UTF8String))
            return nil;
        return @(si.GetBoolValue(section.UTF8String, key.UTF8String, false));
    });
}

+ (int)getPerGameINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def forISO:(nullable NSString *)isoName {
    return ARMSX2ReadPerGameINI(isoName, def, [&](const INISettingsInterface& si) {
        return si.GetIntValue(section.UTF8String, key.UTF8String, def);
    });
}

+ (BOOL)getPerGameINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def forISO:(nullable NSString *)isoName {
    return ARMSX2ReadPerGameINI(isoName, def, [&](const INISettingsInterface& si) {
        return si.GetBoolValue(section.UTF8String, key.UTF8String, def ? true : false) ? YES : NO;
    });
}

+ (float)getPerGameINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def forISO:(nullable NSString *)isoName {
    return ARMSX2ReadPerGameINI(isoName, def, [&](const INISettingsInterface& si) {
        return si.GetFloatValue(section.UTF8String, key.UTF8String, def);
    });
}

+ (nonnull NSString *)getPerGameINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def forISO:(nullable NSString *)isoName {
    return ARMSX2ReadPerGameINI(isoName, def, [&](const INISettingsInterface& si) {
        return ARMSX2NSStringFromStdString(si.GetStringValue(section.UTF8String, key.UTF8String, def.UTF8String));
    });
}

+ (void)setPerGameINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value forISO:(nullable NSString *)isoName {
    ARMSX2MutatePerGameINI(isoName, section, key, [&](INISettingsInterface& si) {
        si.SetIntValue(section.UTF8String, key.UTF8String, value);
    });
}

+ (void)setPerGameINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value forISO:(nullable NSString *)isoName {
    ARMSX2MutatePerGameINI(isoName, section, key, [&](INISettingsInterface& si) {
        si.SetBoolValue(section.UTF8String, key.UTF8String, value ? true : false);
    });
}

+ (void)setPerGameINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value forISO:(nullable NSString *)isoName {
    ARMSX2MutatePerGameINI(isoName, section, key, [&](INISettingsInterface& si) {
        si.SetFloatValue(section.UTF8String, key.UTF8String, value);
    });
}

+ (void)setPerGameINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value forISO:(nullable NSString *)isoName {
    ARMSX2MutatePerGameINI(isoName, section, key, [&](INISettingsInterface& si) {
        si.SetStringValue(section.UTF8String, key.UTF8String, value.UTF8String);
    });
}

+ (void)deletePerGameINIValue:(nonnull NSString *)section key:(nonnull NSString *)key forISO:(nullable NSString *)isoName {
    ARMSX2MutatePerGameINI(isoName, section, key, [&](INISettingsInterface& si) {
        si.DeleteValue(section.UTF8String, key.UTF8String);
        si.RemoveEmptySections();
    });
}

+ (BOOL)hasPerGameINIValueForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key {
    return [self hasPerGameINIValue:section key:key forISO:nil];
}

+ (BOOL)getPerGameINIBoolForCurrentGame:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def {
    return [self getPerGameINIBool:section key:key defaultValue:def forISO:nil];
}

+ (nonnull NSString *)perGameIdentityKeyForISO:(nullable NSString *)isoName {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc))
        return @"";
    return [NSString stringWithFormat:@"%s_%08X", serial.c_str(), (unsigned int)crc];
}

+ (nonnull NSString *)perGameIdentityKeyForCurrentGame {
    return [self perGameIdentityKeyForISO:nil];
}

#pragma mark - PNACH cheats/patches

+ (nullable NSString *)pnachPathForISO:(nullable NSString *)isoName asCheat:(BOOL)asCheat {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc)) {
        NSLog(@"[ARMSX2Bridge] PNACH path unavailable: identity resolution failed");
        return nil;
    }
    return ARMSX2NSStringFromStdString(Patch::GetPnachFilename(serial, crc, asCheat));
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

+ (NSArray<NSString *> *)patchEnableListForISO:(nullable NSString *)isoName section:(NSString *)section key:(NSString *)key {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc)) return @[];
    return ARMSX2PatchEnableListForIdentity(serial, crc, section, key);
}

+ (void)setPatchEnableList:(NSArray<NSString *> *)values forISO:(nullable NSString *)isoName section:(NSString *)section key:(NSString *)key {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForISO(isoName, &serial, &crc)) return;
    ARMSX2SetPatchEnableListForIdentity(values, serial, crc, section, key);
}

@end
