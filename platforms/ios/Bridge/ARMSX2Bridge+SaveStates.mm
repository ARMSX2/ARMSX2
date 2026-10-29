// ARMSX2Bridge+SaveStates.mm — save states, boot restore and memory card backups
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "VMManager.h"
#include "CDVD/CDVD.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Sio.h"
#include "GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ZipHelpers.h"
#include "common/Error.h"

#include "IOSRuntime.h"
#include "IconsFontAwesome.h"

#include <optional>

// A queued task can outlive the game that asked for it and must not touch another game's slots.
static bool ARMSX2SaveStateIdentityMatches(const std::string& serial, u32 crc)
{
    std::string currentSerial;
    u32 currentCRC = 0;
    return ARMSX2GetCurrentSaveStateIdentity(&currentSerial, &currentCRC) &&
        currentSerial == serial && currentCRC == crc;
}

/// Identifies one file on disk, so an undo only touches the file it recorded.
static NSString* ARMSX2FileToken(const std::string& path)
{
    struct stat info{};
    if (path.empty() || stat(path.c_str(), &info) != 0)
        return nil;
    return [NSString stringWithFormat:@"%llu:%ld.%ld", static_cast<unsigned long long>(info.st_ino),
        static_cast<long>(info.st_mtimespec.tv_sec), static_cast<long>(info.st_mtimespec.tv_nsec)];
}

/// Puts a held state back if its slot is still empty. Never replaces a file.
static bool ARMSX2RestoreHeldSaveState(const std::string& path)
{
    bool restored = true;
    for (const std::string& file : {path, path + ".backup"})
    {
        const std::string held = ARMSX2HeldPath(file);
        if (FileSystem::FileExists(held.c_str()))
            restored = renamex_np(held.c_str(), file.c_str(), RENAME_EXCL) == 0 && restored;
    }
    return restored;
}

// The core moves the old state to .backup before it writes, so a failed write leaves the slot
// empty. RENAME_EXCL never replaces a file that is there after all.
static void ARMSX2RestoreSaveStateBackup(const std::string& path)
{
    const std::string backup = path + ".backup";
    if (path.empty() || FileSystem::FileExists(path.c_str()) || !FileSystem::FileExists(backup.c_str()))
        return;
    const bool restored = renamex_np(backup.c_str(), path.c_str(), RENAME_EXCL) == 0;
    NSLog(@"[ARMSX2 iOS SaveState] backup restore path=%@ result=%d",
          ARMSX2NSStringFromStdString(path), restored ? 1 : 0);
}

static NSData* ARMSX2ReadSaveStatePreviewPNG(const std::string& path)
{
    static const zip_uint64_t kMaxPreviewBytes = 8 * 1024 * 1024;

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
    if (!data.has_value() || data->empty() || data->size() > kMaxPreviewBytes)
        return nil;

    return [NSData dataWithBytes:data->data() length:data->size()];
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
        if ([fm copyItemAtPath:sourcePath toPath:targetPath error:&copyError])
            copied++;
        else
            NSLog(@"[ARMSX2 iOS SaveState] memcard backup failed slot=%zu source=%@ error=%@",
                  i + 1, sourcePath, copyError.localizedDescription ?: @"unknown");
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

    if (!VMManager::HasValidVM())
        return true;

    if (MemcardBusy::IsBusy()) {
        NSLog(@"[ARMSX2Bridge] Save-state flush blocked reason=%s memoryCardBusy=1",
              reason ? reason : "unknown");
        return false;
    }

    FileMcd_EmuClose();
    FileMcd_EmuOpen();
    return true;
}

static std::optional<s32> ARMSX2LatestSaveStateSlot(const std::string& serial, u32 crc)
{
    std::optional<s32> latestSlot;
    timespec latestTime{};
    // Include shared autosave/resume files as well as numbered slots, never
    // another game's state or a .backup. No screenshots are decoded.
    for (s32 slot = VMManager::SAVESTATE_SLOT_AUTOSAVE; slot <= VMManager::NUM_SAVE_STATE_SLOTS; ++slot)
    {
        const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot);
        struct stat info{};
        if (path.empty() || !FileSystem::StatFile(path.c_str(), &info) ||
            !S_ISREG(info.st_mode) || info.st_size == 0)
            continue;

        const timespec modified = info.st_mtimespec;
        if (!latestSlot || modified.tv_sec > latestTime.tv_sec ||
            (modified.tv_sec == latestTime.tv_sec && modified.tv_nsec >= latestTime.tv_nsec))
        {
            latestSlot = slot;
            latestTime = modified;
        }
    }
    return latestSlot;
}

void ARMSX2IOSCompleteGameBoot(const std::string& game, bool loadLastSaveState)
{
    if (game.empty() || !ARMSX2HasBaseSettings())
        return; // BIOS-only boot must not replace the remembered game.

    Host::SetBaseStringSettingValue("ARMSX2iOS/Boot", "LastGame", game.c_str());
    Host::CommitBaseSettingChanges();

    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc))
        return;

    // A save cut off mid-write (the app was killed) left only its .backup. Nothing writes
    // before the first Execute(), so this is the one safe point to put it back.
    VMManager::WaitForSaveStateFlush();
    for (s32 slot = VMManager::SAVESTATE_SLOT_AUTOSAVE; slot <= VMManager::NUM_SAVE_STATE_SLOTS; ++slot)
    {
        const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot);
        // A delete whose undo was cut short by a crash is kept, not finished.
        if (!FileSystem::FileExists(path.c_str()))
            ARMSX2RestoreHeldSaveState(path);
        ARMSX2RestoreSaveStateBackup(path);
    }

    if (!loadLastSaveState || !Host::GetBaseBoolSettingValue(
            "ARMSX2iOS/Boot", "AutomaticLoadLastSaveState", false))
        return;

    const auto slot = ARMSX2LatestSaveStateSlot(serial, crc);
    if (!slot)
        return;

    if (ARMSX2RetroAchievementsHardcoreActive())
    {
        Host::AddIconOSDMessage("AutomaticLoadState", ICON_FA_FOLDER_OPEN,
            "Automatic state load skipped: RetroAchievements Hardcore Mode is active.", Host::OSD_QUICK_DURATION);
        return;
    }

    if (!ARMSX2FlushNVRAMAndMemoryCards("automatic-state-load"))
        return;
    ARMSX2BackupAssignedMemoryCards("automatic-state-load", *slot, serial, crc);
    const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, *slot);
    Error error;
    // LoadState resets a partially loaded VM on failure, unlike LoadStateFromSlot.
    // Run here on the CPU thread, before the first VMManager::Execute().
    const bool loaded = VMManager::LoadState(path.c_str(), &error);
    Host::AddIconOSDMessage("AutomaticLoadState", ICON_FA_FOLDER_OPEN,
        loaded ? "Loaded last saved state." : "Could not load the last saved state. Starting normally.",
        Host::OSD_QUICK_DURATION);
}

@implementation ARMSX2Bridge (SaveStates)

+ (BOOL)hasValidSaveStateGame {
    return ARMSX2GetCurrentSaveStateIdentity(nullptr, nullptr);
}

+ (nonnull NSArray<ARMSX2SaveStateSlotInfo *> *)saveStateSlots {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc))
        return @[];

    NSMutableArray<ARMSX2SaveStateSlotInfo *> *slots = [NSMutableArray arrayWithCapacity:VMManager::NUM_SAVE_STATE_SLOTS + 2];
    NSFileManager *fm = [NSFileManager defaultManager];

    // -2 is the core's Auto-save file and 0 is Quick Save. Neither was ever a numbered slot on
    // iOS, so neither can hold an older manual save that an automatic write would replace.
    for (s32 slot = VMManager::SAVESTATE_SLOT_AUTOSAVE; slot <= VMManager::NUM_SAVE_STATE_SLOTS; slot++) {
        if (slot == -1)
            continue;
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

// Manual saves, Quick Save and Auto-save all write here. Auto-save runs during play, so it skips
// any moment a state would catch half done, and zips on its own thread unless the game is closing.
static void ARMSX2WriteSaveState(s32 nativeSlot, bool automatic, bool leaving, void (^callback)(BOOL, NSString*))
{
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        NSLog(@"[ARMSX2 iOS SaveState] save rejected slot=%d validGame=0", nativeSlot);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, nil); });
        return;
    }

    const std::string targetPath = VMManager::GetSaveStateFileName(serial.c_str(), crc, nativeSlot);

    dispatch_async(ARMSX2SaveStateQueue(), ^{
        bool started = false;
        bool existed = false;
        // Why an automatic save didn't run, for emulog.txt: the system log is out of a tester's reach.
        const char* skipped = "the CPU thread never ran it";
        // With zip_on_thread the core reports a failed zip later, from the zip thread.
        auto saveError = std::make_shared<std::string>();
        VMManager::WaitForSaveStateFlush();
        // The live preview runs the game on trial settings; a state saved now would keep them.
        const bool previewing = ARMSX2PerGameLivePreviewTransactions().count > 0;
        if (automatic && previewing)
            skipped = "a per-game live preview was open";
        else {
            Host::RunOnCPUThread([nativeSlot, serial, crc, automatic, leaving, &targetPath, &started, &existed, &skipped, saveError]() {
                if (!ARMSX2SaveStateIdentityMatches(serial, crc)) {
                    NSLog(@"[ARMSX2 iOS SaveState] CPU save rejected slot=%d reason=game-changed", nativeSlot);
                    skipped = "another game was running";
                    return;
                }
                existed = FileSystem::FileExists(targetPath.c_str());
                if (MemcardBusy::IsBusy()) {
                    NSLog(@"[ARMSX2 iOS SaveState] CPU save rejected slot=%d reason=memory-card-busy", nativeSlot);
                    skipped = "the game wrote to a memory card in the last few seconds";
                    return;
                }
                if (automatic && (FileMcd_IsAutoEjecting() || (!leaving && VMManager::GetState() != VMState::Running))) {
                    skipped = "the game was paused or a memory card was ejecting";
                    return;
                }

                if (automatic) {
                    // Closing and reopening the cards, as a manual save does, stalls a running game.
                    cdvdSaveNVRAM();
                    FileMcd_Flush();
                } else if (!ARMSX2FlushNVRAMAndMemoryCards("pre-save-state")) {
                    NSLog(@"[ARMSX2 iOS SaveState] CPU save rejected slot=%d reason=pre-save-flush-failed", nativeSlot);
                    return;
                }

                // SaveState keeps the old state as .backup whatever the INI says, and posts no
                // "slot N" message: the panel and the macros show their own.
                started = true;
                VMManager::SaveState(targetPath.c_str(), automatic && !leaving, true, [saveError](const std::string& error) {
                    *saveError = error;
                });
                if (automatic)
                    Host::RemoveKeyedOSDMessage("MemcardBusy"); // The core's in-game save hint would repeat every interval.
                else if (saveError->empty())
                    ARMSX2FlushNVRAMAndMemoryCards("post-save-state");
            }, true);
        }

        VMManager::WaitForSaveStateFlush();
        const bool result = started && saveError->empty() && FileSystem::FileExists(targetPath.c_str());
        if (!saveError->empty())
            NSLog(@"[ARMSX2 iOS SaveState] save failed slot=%d error=%@", nativeSlot, ARMSX2NSStringFromStdString(*saveError));
        if (!result && existed)
            ARMSX2RestoreSaveStateBackup(targetPath);
        // Only a save over an existing state moves one to .backup; that one is what Undo brings back.
        NSString* backupToken = result && existed ? ARMSX2FileToken(targetPath + ".backup") : nil;

        NSLog(@"[ARMSX2 iOS SaveState] save finished slot=%d result=%d exists=%d",
              nativeSlot, result ? 1 : 0, FileSystem::FileExists(targetPath.c_str()) ? 1 : 0);
        if (automatic && result)
            Console.WriteLn("[SaveState] Auto-save written%s.", leaving ? " on leave" : "");
        else if (automatic)
            Console.WriteLn("[SaveState] Auto-save%s not written: %s.", leaving ? " on leave" : "",
                started ? (saveError->empty() ? "the file is missing" : saveError->c_str()) : skipped);

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO, backupToken); });
    });
}

+ (void)saveStateToSlot:(NSInteger)slot
             completion:(nullable void (^)(BOOL saved, NSString *_Nullable backupToken))completion {
    void (^callback)(BOOL, NSString*) = [completion copy];
    if (slot < 0 || slot > VMManager::NUM_SAVE_STATE_SLOTS) {
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, nil); });
        return;
    }
    ARMSX2WriteSaveState(static_cast<s32>(slot), false, false, callback);
}

+ (void)logAutoSaveSkipped:(nonnull NSString *)reason {
    Console.WriteLn("[SaveState] Auto-save on leave not written: %s.", reason.UTF8String);
}

+ (void)autoSaveLeavingGame:(BOOL)leaving completion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2SaveStateCompletion callback = [completion copy];
    ARMSX2WriteSaveState(VMManager::SAVESTATE_SLOT_AUTOSAVE, true, leaving, ^(BOOL saved, NSString*) {
        if (callback)
            callback(saved);
    });
}

+ (void)loadStateFromSlot:(NSInteger)slot
         expectedModified:(nullable NSDate *)expectedModified
              keepingUndo:(BOOL)keepingUndo
               completion:(nullable void (^)(BOOL loaded, NSString *_Nullable undoPath))completion {
    const s32 nativeSlot = static_cast<s32>(slot);
    void (^callback)(BOOL, NSString*) = [completion copy];
    std::string serial;
    u32 crc = 0;
    if (nativeSlot < VMManager::SAVESTATE_SLOT_AUTOSAVE || nativeSlot == -1 ||
        nativeSlot > VMManager::NUM_SAVE_STATE_SLOTS || !ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        NSLog(@"[ARMSX2 iOS SaveState] load rejected slot=%d validGame=0", nativeSlot);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, nil); });
        return;
    }

    const std::string targetPath = VMManager::GetSaveStateFileName(serial.c_str(), crc, nativeSlot);
    // No serial in the name: Clear Cache removes temporary files that carry one.
    NSString* undoPath = keepingUndo ? [NSTemporaryDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"ARMSX2-undo-load-%@.p2s", [[NSUUID UUID] UUIDString]]] : nil;

    dispatch_async(ARMSX2SaveStateQueue(), ^{
        // By value: the main-queue block below outlives this one.
        auto finish = [callback, undoPath](bool loaded) {
            void (^reply)(BOOL, NSString*) = callback;
            NSString* undo = loaded ? undoPath : nil;
            if (!loaded && undoPath)
                [[NSFileManager defaultManager] removeItemAtPath:undoPath error:nil];
            if (reply)
                dispatch_async(dispatch_get_main_queue(), ^{ reply(loaded ? YES : NO, undo); });
        };
        if (ARMSX2RetroAchievementsHardcoreActive()) {
            NSLog(@"[ARMSX2 iOS SaveState] load rejected slot=%d reason=hardcore-active", nativeSlot);
            finish(false);
            return;
        }

        VMManager::WaitForSaveStateFlush();
        // An automatic save queued ahead of this load may have replaced the state that was shown.
        if (expectedModified) {
            struct stat info{};
            const double modified = stat(targetPath.c_str(), &info) == 0
                ? info.st_mtimespec.tv_sec + info.st_mtimespec.tv_nsec / 1e9 : 0;
            if (std::fabs(modified - expectedModified.timeIntervalSince1970) >= 1) {
                NSLog(@"[ARMSX2 iOS SaveState] load rejected slot=%d reason=state-changed", nativeSlot);
                finish(false);
                return;
            }
        }

        bool flushResult = false;
        Host::RunOnCPUThread([serial, crc, &flushResult]() {
            flushResult = ARMSX2SaveStateIdentityMatches(serial, crc) &&
                ARMSX2FlushNVRAMAndMemoryCards("pre-load-state");
        }, true);
        if (flushResult)
            ARMSX2BackupAssignedMemoryCards("pre-load-state", nativeSlot, serial, crc);

        bool result = false;
        Host::RunOnCPUThread([nativeSlot, serial, crc, flushResult, undoPath, &result]() {
            if (!flushResult || !ARMSX2SaveStateIdentityMatches(serial, crc) || MemcardBusy::IsBusy()) {
                NSLog(@"[ARMSX2 iOS SaveState] CPU load rejected slot=%d", nativeSlot);
                return;
            }
            if (undoPath) {
                std::string undoError;
                VMManager::SaveState(undoPath.fileSystemRepresentation, false, false,
                    [&undoError](const std::string& error) { undoError = error; });
                if (!undoError.empty() || !FileSystem::FileExists(undoPath.fileSystemRepresentation)) {
                    NSLog(@"[ARMSX2 iOS SaveState] CPU load cancelled slot=%d reason=undo-state-failed", nativeSlot);
                    return;
                }
            }
            result = VMManager::LoadStateFromSlot(nativeSlot);
            // Its message names slot -2 or 0; the app says what was loaded.
            Host::RemoveKeyedOSDMessage("LoadStateFromSlot");
            NSLog(@"[ARMSX2 iOS SaveState] CPU load finished slot=%d result=%d", nativeSlot, result ? 1 : 0);
        }, true);
        finish(result);
    });
}

+ (void)undoLoadStateFromPath:(nonnull NSString *)path completion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2SaveStateCompletion callback = [completion copy];
    std::string serial;
    u32 crc = 0;
    const bool validGame = ARMSX2GetCurrentSaveStateIdentity(&serial, &crc);
    dispatch_async(ARMSX2SaveStateQueue(), ^{
        bool result = false;
        VMManager::WaitForSaveStateFlush();
        if (validGame && FileSystem::FileExists(path.fileSystemRepresentation)) {
            // No memory card backup here: undoing a load doubled them and pruned older ones away.
            Host::RunOnCPUThread([serial, crc, path, &result]() {
                if (!ARMSX2SaveStateIdentityMatches(serial, crc) || MemcardBusy::IsBusy() ||
                    !FileSystem::FileExists(path.fileSystemRepresentation))
                    return;
                Error error;
                result = VMManager::LoadState(path.fileSystemRepresentation, &error);
            }, true);
        }
        if (result)
            [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

+ (void)discardUndoLoadStateAtPath:(nonnull NSString *)path {
    dispatch_async(ARMSX2SaveStateQueue(), ^{
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    });
}

static void ARMSX2WithSaveStateSlotPath(NSInteger slot, ARMSX2SaveStateCompletion completion,
    bool (^body)(const std::string& path))
{
    ARMSX2SaveStateCompletion callback = [completion copy];
    std::string serial;
    u32 crc = 0;
    const s32 nativeSlot = static_cast<s32>(slot);
    if (nativeSlot < VMManager::SAVESTATE_SLOT_AUTOSAVE || nativeSlot == -1 ||
        nativeSlot > VMManager::NUM_SAVE_STATE_SLOTS || !ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }
    const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, nativeSlot);
    dispatch_async(ARMSX2SaveStateQueue(), ^{
        VMManager::WaitForSaveStateFlush();
        const bool result = body(path);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

+ (void)deleteSaveStateInSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2WithSaveStateSlotPath(slot, completion, ^bool(const std::string& path) {
        const std::string held = ARMSX2HeldPath(path);
        if (renamex_np(path.c_str(), held.c_str(), RENAME_EXCL) != 0)
            return false;
        const std::string backup = path + ".backup";
        if (FileSystem::FileExists(backup.c_str()))
            renamex_np(backup.c_str(), ARMSX2HeldPath(backup).c_str(), RENAME_EXCL);
        NSLog(@"[ARMSX2 iOS SaveState] delete held path=%@", ARMSX2NSStringFromStdString(path));
        return true;
    });
}

+ (void)restoreDeletedSaveStateInSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2WithSaveStateSlotPath(slot, completion, ^bool(const std::string& path) {
        return !FileSystem::FileExists(path.c_str()) && ARMSX2RestoreHeldSaveState(path);
    });
}

+ (void)finishDeletingSaveStateInSlot:(NSInteger)slot {
    ARMSX2WithSaveStateSlotPath(slot, nil, ^bool(const std::string& path) {
        FileSystem::DeleteFilePath(ARMSX2HeldPath(path).c_str());
        FileSystem::DeleteFilePath(ARMSX2HeldPath(path + ".backup").c_str());
        return true;
    });
}

+ (void)undoSaveOverInSlot:(NSInteger)slot backupToken:(nonnull NSString *)backupToken
                completion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2WithSaveStateSlotPath(slot, completion, ^bool(const std::string& path) {
        const std::string backup = path + ".backup";
        const std::string held = ARMSX2HeldPath(path);
        // Another save since then replaced the backup; putting it back would lose that save.
        if (![ARMSX2FileToken(backup) isEqualToString:backupToken] ||
            renamex_np(path.c_str(), held.c_str(), RENAME_EXCL) != 0)
            return false;
        if (renamex_np(backup.c_str(), path.c_str(), RENAME_EXCL) != 0) {
            renamex_np(held.c_str(), path.c_str(), RENAME_EXCL);
            return false;
        }
        FileSystem::DeleteFilePath(held.c_str());
        return true;
    });
}

+ (double)currentGamePlayedSeconds {
    std::string serial;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, nullptr))
        return 0;
    return static_cast<double>(GameList::GetPlayedTimeForSerial(serial)) +
        static_cast<double>(VMManager::GetSessionPlayedTime());
}

@end
