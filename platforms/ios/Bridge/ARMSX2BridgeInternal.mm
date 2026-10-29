// ARMSX2BridgeInternal.mm — helpers the bridge files share
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "VMManager.h"
#include "pcsx2/MTGS.h"
#include "Achievements.h"
#include "GameList.h"
#include "PerGameOverrides.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"

#include "IOSRuntime.h"

#include <algorithm>
#include <mutex>

bool ARMSX2HasBaseSettings()
{
    auto lock = Host::GetSettingsLock();
    return Host::Internal::GetBaseSettingsLayer() != nullptr;
}

// Coalesce base-settings INI writes so a slider drag or a burst of toggles reaches disk
// once per short window. The in-memory interface updates at once, so reads see the
// latest value; ARMSX2FlushINISave() forces a write now.
static BOOL s_ini_save_scheduled = NO;
static NSUInteger s_ini_save_generation = 0;

void ARMSX2ScheduleINISave()
{
    if (!ARMSX2HasBaseSettings() || s_ini_save_scheduled)
        return;
    s_ini_save_scheduled = YES;
    s_ini_save_generation++;
    const NSUInteger scheduled_generation = s_ini_save_generation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * NSEC_PER_SEC)),
        dispatch_get_main_queue(),
        ^{
            s_ini_save_scheduled = NO;
            if (scheduled_generation == s_ini_save_generation && ARMSX2HasBaseSettings())
                Host::CommitBaseSettingChanges();
        });
}

void ARMSX2FlushINISave()
{
    s_ini_save_generation++;
    s_ini_save_scheduled = NO;
    if (ARMSX2HasBaseSettings())
        Host::CommitBaseSettingChanges();
}

ARMSX2RetroAchievementsToastInfo* s_pendingRetroAchievementsNotification = nil;

int ARMSX2ClampInt(int value, int minValue, int maxValue)
{
    return std::min(std::max(value, minValue), maxValue);
}

NSString* ARMSX2NSStringFromStdString(const std::string& value)
{
    if (value.empty())
        return @"";

    NSString* string = [NSString stringWithUTF8String:value.c_str()];
    return string ?: @"";
}

NSString* ARMSX2NSStringFromStringView(std::string_view value)
{
    if (value.empty())
        return @"";

    NSString* string = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    return string ?: @"";
}

bool ARMSX2RetroAchievementsHardcoreActive()
{
    return EmuConfig.Achievements.Enabled && Achievements::IsHardcoreModeActive();
}

void ARMSX2LogRetroAchievementsHardcoreBlock(const char* action)
{
    NSLog(@"[ARMSX2Bridge] RetroAchievements Hardcore blocked action=%s", action ? action : "unknown");
}

BOOL ARMSX2GetCurrentSaveStateIdentity(std::string* serial, u32* crc)
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

/// A deleted state waits under this name until its undo runs out. No lookup matches it.
std::string ARMSX2HeldPath(const std::string& path)
{
    return path + ".held";
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

BOOL ARMSX2ExternalGameAccessAlreadyActive(NSString* path)
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

NSArray<NSDictionary*>* ARMSX2ExternalGameDirectoryRecords()
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

BOOL ARMSX2ExternalGameRecordIsDirectory(NSDictionary* record, NSURL* url)
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

BOOL ARMSX2ExternalGameRecordIsCloudProvider(NSDictionary* record, NSURL* url)
{
	NSString* path = (url.path ?: @"").lowercaseString;
	NSString* displayName = [record[@"displayName"] isKindOfClass:NSString.class] ? [(NSString*)record[@"displayName"] lowercaseString] : @"";
	return [path containsString:@"google"] || [displayName containsString:@"google"];
}

NSURL* ARMSX2ResolveExternalGameDirectoryRecord(NSDictionary* record)
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
		if (ARMSX2ExternalGameRecordIsCloudProvider(record, directoryURL))
			continue;

		BOOL isDirectory = ARMSX2ExternalGameRecordIsDirectory(record, directoryURL);
		NSString* normalizedRecordPath = directoryURL.path.stringByStandardizingPath;
		NSString* normalizedPath = path.stringByStandardizingPath;
		BOOL matches = isDirectory ? ARMSX2IsPathInsideDirectory(path, directoryURL.path) : [normalizedPath isEqualToString:normalizedRecordPath];
		if (!matches)
			continue;

		BOOL granted = [directoryURL startAccessingSecurityScopedResource];
		if (granted)
			ARMSX2RememberExternalGameAccess(directoryURL);
		else
			NSLog(@"[ARMSX2Bridge] External game %@ access not granted path=%@", isDirectory ? @"folder" : @"file", directoryURL.path);
		return granted;
	}

	return NO;
}

BOOL ARMSX2StartExternalGameDirectoryAccessForPathSafe(NSString* path)
{
	@try {
		return ARMSX2StartExternalGameDirectoryAccessForPath(path);
	} @catch (NSException* exception) {
		NSLog(@"[ARMSX2Bridge] External game folder access exception path=%@ name=%@ reason=%@",
		      path ?: @"", exception.name ?: @"", exception.reason ?: @"");
		return NO;
	}
}

BOOL ARMSX2IsSupportedGameImageAtPath(NSString* path)
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

NSString* ARMSX2ResolveISOPath(NSString* isoName)
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

// The running VM and every metadata scan share one InputIsoFile, so scanning the disc a
// game plays from closes it under the game and later reads return zero blocks.
// GameList.h says PopulateEntryFromPath must not be called while the system is running.
bool ARMSX2PathIsRunningDisc(NSString* resolvedPath)
{
    if (resolvedPath.length == 0 || !VMManager::HasValidVM())
        return false;

    const std::string running = VMManager::GetDiscPath();
    return !running.empty() && running == resolvedPath.UTF8String;
}

static void ARMSX2NoteRunningDiscScan(NSString* path)
{
    static bool warned = false;
    if (warned)
        return;

    warned = true;
    Console.Warning("Not scanning '%s' while a VM is running; using the game list cache instead.",
        path.UTF8String);
}

BOOL ARMSX2PopulateGameListEntryForISO(NSString* isoName, GameList::Entry* entry, NSString** resolvedPath)
{
    NSString* path = ARMSX2ResolveISOPath(isoName);
    if (resolvedPath)
        *resolvedPath = path;

    if (path.length == 0 || !entry)
        return NO;

    // Any VM: InputIsoFile::Open closes whatever is open, so scanning an unrelated image kills
    // a running game's disc too. Hold the boot gate through the check; the worker holds it from
    // Initialize to the end of Shutdown, and HasValidVM turns false before the disc closes.
    const std::lock_guard<std::mutex> bootLock(s_vmMutex);
    if (s_vmThreadActive.load(std::memory_order_acquire)
        || s_requestVMBoot.load(std::memory_order_acquire)
        || VMManager::HasValidVM())
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

dispatch_queue_t ARMSX2SaveStateQueue()
{
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("org.armsx2.ios.savestates", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

NSMutableDictionary<NSString*, NSDictionary<NSString*, id>*>* ARMSX2PerGameLivePreviewTransactions()
{
    static NSMutableDictionary<NSString*, NSDictionary<NSString*, id>*>* transactions;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        transactions = [[NSMutableDictionary alloc] init];
    });
    return transactions;
}

// A file's mask is derivable from which keys it holds rather than stored ahead of
// them. The key table lives in PerGameOverrides so the core and this bridge cannot
// disagree about which settings a player is allowed to claim.
u32 ARMSX2DerivePerGameHackClaims(INISettingsInterface& si)
{
    return ComputePerGameOverrides(si).gs_hacks;
}

void ARMSX2StoreDerivedPerGameHackClaims(INISettingsInterface& si)
{
    const u32 claims = ARMSX2DerivePerGameHackClaims(si);
    if (claims != 0)
        si.SetIntValue("EmuCore/GS", "UserHackOverrides", static_cast<int>(claims));
    else
        si.DeleteValue("EmuCore/GS", "UserHackOverrides");
}

// Resolve a per-game identity (serial, crc) for a library ISO. Returns NO if the ISO
// cannot be resolved or has no CRC. ELFs have no serial (empty), matching the
// game-settings writer.
BOOL ARMSX2PerGameIdentityForISO(NSString* isoName, std::string* serial, u32* crc)
{
    // The VM already knows what it booted, and asking the disc is slower and was once
    // destructive. Taking it here rather than from the cache below also works when the
    // game list has no entry for the running game.
    if (isoName.length == 0 || ARMSX2PathIsRunningDisc(ARMSX2ResolveISOPath(isoName)))
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
BOOL ARMSX2PerGameIdentityForCurrentGame(std::string* serial, u32* crc)
{
    if (!VMManager::HasValidVM())
        return NO;
    *serial = VMManager::GetSerialForGameSettings();
    *crc = VMManager::GetDiscCRC();
    return *crc != 0;
}

std::string ARMSX2PerGameSettingsPath(const std::string& serial, u32 crc)
{
    FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), false);
    return VMManager::GetGameSettingsPath(serial, crc);
}

BOOL ARMSX2IsArchiveJunkName(NSString* name)
{
    if ([name.pathComponents containsObject:@"__MACOSX"])
        return YES;

    NSString* last = name.lastPathComponent;
    return [last isEqualToString:@".DS_Store"] || [last hasPrefix:@"._"];
}

// Save in the per-game panel writes every field it owns, and each write used to queue its
// own full config reload, seventy-odd per tap. Let the last write in a burst be the one
// that reloads.
std::atomic<uint64_t> s_ARMSX2PerGameSettingsReloadGeneration{0};

void ARMSX2RequestPerGameSettingsReload()
{
    const uint64_t mine = s_ARMSX2PerGameSettingsReloadGeneration.fetch_add(1, std::memory_order_relaxed) + 1;

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.05 * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
            // Someone wrote after us, so they own the reload.
            if (s_ARMSX2PerGameSettingsReloadGeneration.load(std::memory_order_relaxed) != mine)
                return;

            Host::RunOnCPUThread([]() {
                VMManager::ReloadGameSettings();
                ARMSX2_ApplyEffectivePresentFPSCap();
                if (MTGS::IsOpen())
                    MTGS::ApplySettings();
                ARMSX2_CaptureGraphicsHackState();
            });
        });
}
