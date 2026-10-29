// ARMSX2BridgeInternal.h — helpers the bridge files share
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#import "ARMSX2Bridge.h"
#include "common/Pcsx2Types.h"
#include "pcsx2/Host.h"

#include <atomic>
#include <string>
#include <string_view>

class INISettingsInterface;
namespace GameList { struct Entry; }

static constexpr int ARMSX2DefaultAudioVolumePercent = 100;

extern ARMSX2RetroAchievementsToastInfo* s_pendingRetroAchievementsNotification;
extern std::atomic<uint64_t> s_ARMSX2PerGameSettingsReloadGeneration;

bool ARMSX2HasBaseSettings();

template <typename F>
void ARMSX2EditBaseSettings(F&& edit)
{
    auto lock = Host::GetSettingsLock(); // not recursive: no Host accessor or ARMSX2HasBaseSettings in edit
    if (SettingsInterface* layer = Host::Internal::GetBaseSettingsLayer())
        edit(*layer);
}

void ARMSX2ScheduleINISave();
void ARMSX2FlushINISave();
int ARMSX2ClampInt(int value, int minValue, int maxValue);
NSString* ARMSX2NSStringFromStdString(const std::string& value);
NSString* ARMSX2NSStringFromStringView(std::string_view value);
bool ARMSX2RetroAchievementsHardcoreActive();
void ARMSX2LogRetroAchievementsHardcoreBlock(const char* action);
BOOL ARMSX2GetCurrentSaveStateIdentity(std::string* serial, u32* crc);
std::string ARMSX2HeldPath(const std::string& path);
BOOL ARMSX2ExternalGameAccessAlreadyActive(NSString* path);
NSArray<NSDictionary*>* ARMSX2ExternalGameDirectoryRecords();
BOOL ARMSX2ExternalGameRecordIsDirectory(NSDictionary* record, NSURL* url);
BOOL ARMSX2ExternalGameRecordIsCloudProvider(NSDictionary* record, NSURL* url);
NSURL* ARMSX2ResolveExternalGameDirectoryRecord(NSDictionary* record);
BOOL ARMSX2StartExternalGameDirectoryAccessForPathSafe(NSString* path);
BOOL ARMSX2IsSupportedGameImageAtPath(NSString* path);
NSString* ARMSX2ResolveISOPath(NSString* isoName);
bool ARMSX2PathIsRunningDisc(NSString* resolvedPath);
BOOL ARMSX2PopulateGameListEntryForISO(NSString* isoName, GameList::Entry* entry, NSString** resolvedPath);
dispatch_queue_t ARMSX2SaveStateQueue();
NSMutableDictionary<NSString*, NSDictionary<NSString*, id>*>* ARMSX2PerGameLivePreviewTransactions();
u32 ARMSX2DerivePerGameHackClaims(INISettingsInterface& si);
void ARMSX2StoreDerivedPerGameHackClaims(INISettingsInterface& si);
BOOL ARMSX2PerGameIdentityForISO(NSString* isoName, std::string* serial, u32* crc);
BOOL ARMSX2PerGameIdentityForCurrentGame(std::string* serial, u32* crc);
std::string ARMSX2PerGameSettingsPath(const std::string& serial, u32 crc);
BOOL ARMSX2IsArchiveJunkName(NSString* name);
void ARMSX2RequestPerGameSettingsReload();
