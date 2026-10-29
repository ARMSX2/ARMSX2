// ARMSX2Bridge+LivePreview.mm — per-game live preview
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "VMManager.h"
#include "pcsx2/MTGS.h"
#include "SIO/Sio.h"
#include "pcsx2/Host.h"
#include "common/FileSystem.h"
#include "common/Error.h"

#include "IOSRuntime.h"

static bool ARMSX2RestorePerGameLivePreviewSettings(NSDictionary<NSString*, id>* transaction)
{
    NSString* settingsPath = transaction[@"settingsPath"];
    id originalSettings = transaction[@"originalSettings"];
    if (settingsPath.length == 0 || !originalSettings)
        return false;

    NSFileManager* fm = [NSFileManager defaultManager];
    if ([originalSettings isKindOfClass:NSData.class])
        return [(NSData*)originalSettings writeToFile:settingsPath atomically:YES];

    if ([fm fileExistsAtPath:settingsPath])
        return [fm removeItemAtPath:settingsPath error:nil];
    return true;
}

static void ARMSX2RemovePerGameLivePreviewTransaction(NSString* token, NSDictionary<NSString*, id>* transaction)
{
    NSString* statePath = transaction[@"statePath"];
    if (statePath.length > 0)
        [[NSFileManager defaultManager] removeItemAtPath:statePath error:nil];
    [ARMSX2PerGameLivePreviewTransactions() removeObjectForKey:token];
}

@implementation ARMSX2Bridge (LivePreview)

+ (void)beginPerGameLivePreviewWithCompletion:(nullable ARMSX2TemporaryStateCreationCompletion)completion
{
    ARMSX2TemporaryStateCreationCompletion callback = [completion copy];
    std::string serial;
    u32 crc = 0;
    if (ARMSX2RetroAchievementsHardcoreActive() ||
        !ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
    {
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(nil); });
        return;
    }

    NSString* token = [[NSUUID UUID] UUIDString];
    NSString* statePath = [NSTemporaryDirectory()
        stringByAppendingPathComponent:[NSString stringWithFormat:@"ARMSX2-per-game-preview-%@.p2s", token]];
    NSString* settingsPath = ARMSX2NSStringFromStdString(ARMSX2PerGameSettingsPath(serial, crc));
    NSData* originalSettings = [NSData dataWithContentsOfFile:settingsPath];
    id originalSettingsRecord = originalSettings ?: NSNull.null;

    dispatch_async(ARMSX2SaveStateQueue(), ^{
        bool saved = false;
        Host::RunOnCPUThread([&saved, statePath]() {
            if (!VMManager::HasValidVM() || MemcardBusy::IsBusy())
                return;

            std::string error;
            VMManager::SaveState(statePath.fileSystemRepresentation, false, false,
                [&error](const std::string& message) { error = message; });
            saved = error.empty() && FileSystem::FileExists(statePath.fileSystemRepresentation);
        }, true);

        if (saved)
        {
            ARMSX2PerGameLivePreviewTransactions()[token] = @{
                @"statePath": statePath,
                @"settingsPath": settingsPath,
                @"originalSettings": originalSettingsRecord,
            };
        }
        else
        {
            [[NSFileManager defaultManager] removeItemAtPath:statePath error:nil];
        }

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(saved ? token : nil); });
    });
}

+ (void)refreshPerGameLivePreviewBaselineForToken:(nonnull NSString *)token
                                        afterSave:(BOOL)afterSave
{
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2PerGameIdentityForCurrentGame(&serial, &crc))
        return;

    // Read before Swift writes the next preview, so the baseline is the committed file,
    // including a Save or a Pad tab write made since the last preview.
    NSString* settingsPath = ARMSX2NSStringFromStdString(ARMSX2PerGameSettingsPath(serial, crc));
    id committed = [NSData dataWithContentsOfFile:settingsPath] ?: NSNull.null;
    dispatch_async(ARMSX2SaveStateQueue(), ^{
        NSDictionary<NSString*, id>* transaction = ARMSX2PerGameLivePreviewTransactions()[token];
        if (![transaction[@"settingsPath"] isEqualToString:settingsPath] ||
            (!afterSave && [transaction[@"restorePending"] boolValue]))
            return;
        NSMutableDictionary<NSString*, id>* updated = [transaction mutableCopy];
        updated[@"originalSettings"] = committed;
        // A Save is committed even after a failed restore. Write it again here: an
        // apply still running when Save wrote the file has put older bytes back.
        updated[@"restorePending"] = @(afterSave && !ARMSX2RestorePerGameLivePreviewSettings(updated));
        ARMSX2PerGameLivePreviewTransactions()[token] = updated;
    });
}

+ (void)applyPerGameLivePreviewForToken:(nonnull NSString *)token
                              completion:(nullable ARMSX2SaveStateCompletion)completion
{
    ARMSX2SaveStateCompletion callback = [completion copy];
    dispatch_async(ARMSX2SaveStateQueue(), ^{
        NSDictionary<NSString*, id>* transaction = ARMSX2PerGameLivePreviewTransactions()[token];
        bool applied = false;
        if (transaction)
        {
            // Invalidate the delayed reloads requested by the individual compatibility
            // keys. The single synchronous reload below sees the complete preview.
            s_ARMSX2PerGameSettingsReloadGeneration.fetch_add(1, std::memory_order_relaxed);
            Host::RunOnCPUThread([&applied]() {
                if (!VMManager::HasValidVM())
                    return;
                VMManager::ReloadGameSettings();
                ARMSX2_ApplyEffectivePresentFPSCap();
                if (MTGS::IsOpen())
                    MTGS::ApplySettings();
                ARMSX2_CaptureGraphicsHackState();
                applied = true;
            }, true);

            // Preview values belong to the running VM only. Put the committed file back
            // at once so an interrupted preview cannot become a save; finish retries a miss.
            const bool restored = ARMSX2RestorePerGameLivePreviewSettings(transaction);
            const bool pending = !restored;
            if (pending != [transaction[@"restorePending"] boolValue])
            {
                NSMutableDictionary<NSString*, id>* updated = [transaction mutableCopy];
                updated[@"restorePending"] = @(pending);
                ARMSX2PerGameLivePreviewTransactions()[token] = updated;
            }
            applied = restored && applied;
        }

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(applied ? YES : NO); });
    });
}

+ (void)restorePerGameLivePreviewStateForToken:(nonnull NSString *)token
                                      completion:(nullable ARMSX2SaveStateCompletion)completion
{
    ARMSX2SaveStateCompletion callback = [completion copy];
    dispatch_async(ARMSX2SaveStateQueue(), ^{
        NSDictionary<NSString*, id>* transaction = ARMSX2PerGameLivePreviewTransactions()[token];
        NSString* statePath = transaction[@"statePath"];
        bool restored = false;
        if (statePath.length > 0)
        {
            Host::RunOnCPUThread([&restored, statePath]() {
                if (!VMManager::HasValidVM())
                    return;
                Error error;
                restored = VMManager::LoadState(statePath.fileSystemRepresentation, &error);
            }, true);
        }

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(restored ? YES : NO); });
    });
}

+ (void)finishPerGameLivePreviewForToken:(nonnull NSString *)token
                               completion:(nullable ARMSX2SaveStateCompletion)completion
{
    ARMSX2SaveStateCompletion callback = [completion copy];
    dispatch_async(ARMSX2SaveStateQueue(), ^{
        NSDictionary<NSString*, id>* transaction = ARMSX2PerGameLivePreviewTransactions()[token];
        bool finished = transaction != nil;
        if (transaction)
        {
            s_ARMSX2PerGameSettingsReloadGeneration.fetch_add(1, std::memory_order_relaxed);
            if ([transaction[@"restorePending"] boolValue])
                finished = ARMSX2RestorePerGameLivePreviewSettings(transaction);

            NSString* statePath = transaction[@"statePath"];
            Host::RunOnCPUThread([&finished, statePath]() {
                if (!finished || !VMManager::HasValidVM())
                {
                    finished = false;
                    return;
                }

                // Every apply put the committed file back, so reloading it drops the
                // preview; the state load restores the gameplay moment before exit.
                VMManager::ReloadGameSettings();
                ARMSX2_ApplyEffectivePresentFPSCap();
                if (MTGS::IsOpen())
                    MTGS::ApplySettings();
                ARMSX2_CaptureGraphicsHackState();
                Error error;
                finished = VMManager::LoadState(statePath.fileSystemRepresentation, &error);
            }, true);
            ARMSX2RemovePerGameLivePreviewTransaction(token, transaction);
        }

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(finished ? YES : NO); });
    });
}

@end
