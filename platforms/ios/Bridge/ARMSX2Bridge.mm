// ARMSX2Bridge.mm — ObjC bridge implementation
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "CDVD/CDVDcommon.h"
#include "VMManager.h"
#include "Patch.h"
#include "GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "IOSRuntime.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

@implementation ARMSX2SaveStateSlotInfo
@end

@implementation ARMSX2BIOSInfo
@end

@implementation ARMSX2RetroAchievementsToastInfo
@end

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

static BOOL ARMSX2ExternalGameRecordScanDisabled(NSDictionary* record, NSURL* url)
{
	if (ARMSX2ExternalGameRecordIsCloudProvider(record, url))
		return YES;

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
        if ([fm removeItemAtURL:url error:&error])
            removed++;
        else
            NSLog(@"[ARMSX2Bridge] Failed removing generated game file %@ error=%@", url.path, error.localizedDescription);
    }
    return removed;
}

@implementation ARMSX2Bridge

+ (nullable NSString *)currentISOPath {
    NSString *docsPath = [self documentsDirectory];
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

// Unlike currentGameISOName there is no INI fallback, so an ejected drive reads as empty.
+ (nullable NSString *)discInDriveName {
    if (!VMManager::HasValidVM())
        return nil;
    const std::string discPath = VMManager::GetDiscPath();
    if (discPath.empty())
        return nil;
    NSString *fileName = ARMSX2NSStringFromStringView(Path::GetFileName(discPath));
    return fileName.length > 0 ? fileName : nil;
}

+ (nonnull NSString *)isoDirectory {
    NSString *docsPath = [self documentsDirectory];
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
    NSString *docsPath = [self documentsDirectory];
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
		if (isDirectory && ARMSX2ExternalGameRecordScanDisabled(record, directoryURL))
			continue;

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
    } else {
        NSLog(@"[ARMSX2 iOS Covers] metadata unavailable %@", isoName);
    }

    return metadata;
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

    for (s32 slot = VMManager::SAVESTATE_SLOT_AUTOSAVE; slot <= VMManager::NUM_SAVE_STATE_SLOTS; slot++) {
        const std::string path = VMManager::GetSaveStateFileName(entry.serial.c_str(), entry.crc, slot);
        removePath(path);
        removePath(path + ".backup");
        removePath(ARMSX2HeldPath(path));
        removePath(ARMSX2HeldPath(path + ".backup"));
    }

    removePath(Patch::GetPnachFilename(entry.serial, entry.crc, true));
    removePath(Patch::GetPnachFilename(entry.serial, entry.crc, false));
    removePath(VMManager::GetGameSettingsPath(entry.serial, entry.crc));

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
            result = VMManager::ChangeDisc(CDVD_SourceType::NoDisc, {});
        }, true);
        ejectResult = result;

        [NSThread sleepForTimeInterval:1.25];

        Host::RunOnCPUThread([nativePath, &result]() {
            result = VMManager::ChangeDisc(CDVD_SourceType::Iso, nativePath);
        }, true);

        if (result && ARMSX2HasBaseSettings()) {
            Host::SetBaseStringSettingValue("GameISO", "BootISO", isoName.UTF8String);
            Host::CommitBaseSettingChanges();
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

#pragma mark - ISO boot

+ (BOOL)canResolveISO:(nonnull NSString *)isoName {
	return ARMSX2ResolveISOPath(isoName).length > 0 ? YES : NO;
}

+ (void)bootISO:(nonnull NSString *)isoName {
	if (!ARMSX2HasBaseSettings()) {
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
	bool fastBoot = false;
	ARMSX2EditBaseSettings([&](SettingsInterface& si) {
		si.SetStringValue("GameISO", "BootISO", bootValue.UTF8String);
		fastBoot = si.GetBoolValue("GameISO", "FastBoot", si.GetBoolValue("EmuCore", "EnableFastBoot", false));
		si.SetBoolValue("GameISO", "FastBoot", fastBoot);
		si.SetBoolValue("EmuCore", "EnableFastBoot", fastBoot);
	});
	Host::CommitBaseSettingChanges();
	std::fprintf(stderr, "@@BOOT_FASTBOOT_SET@@ value=%d source=settings\n", fastBoot ? 1 : 0);
	std::fflush(stderr);
}

#pragma mark - Favorites

+ (BOOL)isFavorite:(nonnull NSString *)isoName {
    if (!ARMSX2HasBaseSettings()) return NO;
    return Host::GetBaseBoolSettingValue("Favorites", isoName.UTF8String, false);
}

+ (void)setFavorite:(nonnull NSString *)isoName favorite:(BOOL)favorite {
    if (!ARMSX2HasBaseSettings()) return;
    Host::SetBaseBoolSettingValue("Favorites", isoName.UTF8String, favorite);
    Host::CommitBaseSettingChanges();
}

@end
