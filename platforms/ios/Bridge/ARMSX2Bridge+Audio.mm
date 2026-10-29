// ARMSX2Bridge+Audio.mm — audio pack import, volume and preview mute
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "VMManager.h"
#include "SPU2/spu2.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/ZipHelpers.h"

#include "IOSRuntime.h"

#include <optional>

// CPU-thread-owned temporary mute state for the Per-Game live-preview overlay.
// The prior user mute state is restored when the preview returns to editing.
static std::optional<bool> s_per_game_preview_original_audio_muted;

enum ARMSX2AudioPackFailure {
    ARMSX2AudioPackBadArgument = 1,
    ARMSX2AudioPackUnreadable,
    ARMSX2AudioPackTooLarge,
    ARMSX2AudioPackInvalidContents,
    ARMSX2AudioPackWriteFailed,
};

static NSArray<NSURL*>* ARMSX2FailAudioPackExtraction(NSError** error, NSInteger code, NSString* message)
{
    if (error) {
        *error = [NSError errorWithDomain:@"ARMSX2AudioPackExtraction"
                                     code:code
                                 userInfo:@{NSLocalizedDescriptionKey: message}];
    }
    NSLog(@"[ARMSX2 iOS Audio Pack] %@", message);
    return @[];
}

static NSArray<NSString*>* ARMSX2RequiredAudioPackRoles()
{
    static NSArray<NSString*>* roles;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        roles = [[NSArray alloc] initWithObjects:@"background", @"navigation", @"select",
                                                  @"switch_toggle_on",
                                                  @"switch_toggle_off", @"return",
                                                  @"stopping_game", @"context_menu",
                                                  @"achievement_toast", @"ui_toast",
                                                  @"tab_transition", @"launch_game",
                                                  @"no_jit", nil];
    });
    return roles;
}

static NSArray<NSString*>* ARMSX2SupportedAudioPackRoles()
{
    static NSArray<NSString*>* roles;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        // startup is intentionally optional and is used only when a custom pack provides it.
        roles = [[NSArray alloc] initWithObjects:@"startup", @"background", @"navigation",
                                                  @"select", @"switch_toggle_on",
                                                  @"switch_toggle_off", @"return",
                                                  @"stopping_game", @"context_menu",
                                                  @"achievement_toast", @"ui_toast",
                                                  @"tab_transition", @"launch_game",
                                                  @"no_jit", nil];
    });
    return roles;
}

@implementation ARMSX2Bridge (Audio)

+ (nonnull NSArray<NSURL *> *)extractAudioPackArchiveAtURL:(nonnull NSURL *)archiveURL
                                               toDirectory:(nonnull NSURL *)destinationDirectory
                                                     error:(NSError * _Nullable * _Nullable)error
{
    static const zip_uint64_t kMaxAudioFileBytes = 64 * 1024 * 1024;
    static const zip_uint64_t kMaxAudioPackBytes = 256 * 1024 * 1024;
    static const zip_int64_t kMaxAudioPackEntries = 128;

    if (error)
        *error = nil;
    if (!archiveURL.isFileURL || !destinationDirectory.isFileURL) {
        return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackBadArgument,
            @"Audio pack extraction needs file URLs.");
    }

    NSFileManager* manager = [NSFileManager defaultManager];
    NSError* directoryError = nil;
    if (![manager createDirectoryAtURL:destinationDirectory
           withIntermediateDirectories:YES
                            attributes:nil
                                 error:&directoryError]) {
        return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackWriteFailed,
            [NSString stringWithFormat:@"Could not create %@: %@", destinationDirectory.path,
                                       directoryError.localizedDescription]);
    }

    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.UTF8String, ZIP_RDONLY, &ze);
    if (!zf) {
        return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackUnreadable,
            [NSString stringWithFormat:@"Could not open %@: %s", archiveURL.lastPathComponent,
                                       zip_error_strerror(&ze)]);
    }

    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    if (count < 0 || count > kMaxAudioPackEntries) {
        return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackTooLarge,
            [NSString stringWithFormat:@"%@ has too many archive entries.", archiveURL.lastPathComponent]);
    }

    NSArray<NSString*>* requiredRoles = ARMSX2RequiredAudioPackRoles();
    NSArray<NSString*>* supportedRoles = ARMSX2SupportedAudioPackRoles();
    NSSet<NSString*>* allowedRoles = [NSSet setWithArray:supportedRoles];
    NSMutableDictionary<NSString*, NSNumber*>* entryIndices = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString*, NSString*>* outputNames = [NSMutableDictionary dictionary];
    zip_uint64_t declaredTotalBytes = 0;

    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(count); i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;

        NSString* entryName = [NSString stringWithUTF8String:stat.name];
        if (entryName.length == 0 || [entryName hasSuffix:@"/"] || ARMSX2IsArchiveJunkName(entryName))
            continue;

        NSString* leafName = entryName.lastPathComponent;
        NSString* fileExtension = leafName.pathExtension.lowercaseString;
        if (![fileExtension isEqualToString:@"mp3"] && ![fileExtension isEqualToString:@"wav"])
            continue;

        NSString* role = leafName.stringByDeletingPathExtension.lowercaseString;
        if (![allowedRoles containsObject:role])
            continue;
        if (entryIndices[role]) {
            return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackInvalidContents,
                [NSString stringWithFormat:@"%@ contains more than one %@ audio file.",
                                           archiveURL.lastPathComponent, role]);
        }

        zip_uint8_t opsys = 0;
        zip_uint32_t attributes = 0;
        if (zip_file_get_external_attributes(zf.get(), i, 0, &opsys, &attributes) == 0 &&
            opsys == ZIP_OPSYS_UNIX && ((attributes >> 16) & S_IFMT) == S_IFLNK) {
            return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackInvalidContents,
                [NSString stringWithFormat:@"%@ contains a symlink for %@.",
                                           archiveURL.lastPathComponent, role]);
        }

        if (!(stat.valid & ZIP_STAT_SIZE) || stat.size == 0 || stat.size > kMaxAudioFileBytes) {
            return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackTooLarge,
                [NSString stringWithFormat:@"The %@ audio file is empty or too large.", role]);
        }
        declaredTotalBytes += stat.size;
        if (declaredTotalBytes > kMaxAudioPackBytes) {
            return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackTooLarge,
                [NSString stringWithFormat:@"%@ unpacks to more than an audio pack should.",
                                           archiveURL.lastPathComponent]);
        }

        entryIndices[role] = @(i);
        outputNames[role] = [NSString stringWithFormat:@"%@.%@", role, fileExtension];
    }

    NSMutableArray<NSString*>* missing = [NSMutableArray array];
    for (NSString* role in requiredRoles) {
        if (!entryIndices[role])
            [missing addObject:role];
    }
    if (missing.count > 0) {
        return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackInvalidContents,
            [NSString stringWithFormat:@"%@ is missing: %@.", archiveURL.lastPathComponent,
                                       [missing componentsJoinedByString:@", "]]);
    }

    NSMutableArray<NSURL*>* extracted = [NSMutableArray arrayWithCapacity:supportedRoles.count];
    zip_uint64_t actualTotalBytes = 0;
    for (NSString* role in supportedRoles) {
        if (!entryIndices[role])
            continue;
        const zip_uint64_t index = entryIndices[role].unsignedLongLongValue;
        auto file = zip_fopen_index_managed(zf.get(), index, ZIP_FL_ENC_GUESS);
        if (!file) {
            for (NSURL* url in extracted)
                [manager removeItemAtURL:url error:nil];
            return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackUnreadable,
                [NSString stringWithFormat:@"Could not read %@ from %@.", role,
                                           archiveURL.lastPathComponent]);
        }

        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->empty() || data->size() > kMaxAudioFileBytes) {
            for (NSURL* url in extracted)
                [manager removeItemAtURL:url error:nil];
            return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackUnreadable,
                [NSString stringWithFormat:@"The %@ audio file could not be read.", role]);
        }
        actualTotalBytes += data->size();
        if (actualTotalBytes > kMaxAudioPackBytes) {
            for (NSURL* url in extracted)
                [manager removeItemAtURL:url error:nil];
            return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackTooLarge,
                [NSString stringWithFormat:@"%@ unpacks to more than an audio pack should.",
                                           archiveURL.lastPathComponent]);
        }

        NSURL* destinationURL = [destinationDirectory
            URLByAppendingPathComponent:outputNames[role] isDirectory:NO];
        NSData* audioData = [NSData dataWithBytes:data->data() length:data->size()];
        if (![audioData writeToURL:destinationURL atomically:YES]) {
            for (NSURL* url in extracted)
                [manager removeItemAtURL:url error:nil];
            return ARMSX2FailAudioPackExtraction(error, ARMSX2AudioPackWriteFailed,
                [NSString stringWithFormat:@"Could not write %@.", destinationURL.path]);
        }
        [extracted addObject:destinationURL];
    }

    return extracted;
}

+ (int)emulatorVolumePercent {
    const int value = ARMSX2HasBaseSettings() ?
        Host::GetBaseIntSettingValue("SPU2/Output", "StandardVolume", ARMSX2DefaultAudioVolumePercent) :
        ARMSX2DefaultAudioVolumePercent;
    return ARMSX2ClampInt(value, 0, ARMSX2DefaultAudioVolumePercent);
}

+ (void)setEmulatorVolumePercent:(int)value {
    if (!ARMSX2HasBaseSettings())
        return;

    const int clampedValue = ARMSX2ClampInt(value, 0, ARMSX2DefaultAudioVolumePercent);
    ARMSX2EditBaseSettings([&](SettingsInterface& si) {
        si.SetIntValue("SPU2/Output", "StandardVolume", clampedValue);
        si.SetIntValue("SPU2/Output", "FastForwardVolume", clampedValue);
    });
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

+ (void)setPerGameLivePreviewAudioMuted:(BOOL)muted {
    Host::RunOnCPUThread([muted]() {
        if (muted) {
            if (!VMManager::HasValidVM())
                return;
            if (!s_per_game_preview_original_audio_muted.has_value())
                s_per_game_preview_original_audio_muted = SPU2::IsOutputMuted();
            SPU2::SetOutputMuted(true);
            return;
        }

        if (!s_per_game_preview_original_audio_muted.has_value())
            return;
        const bool restore_muted = s_per_game_preview_original_audio_muted.value();
        s_per_game_preview_original_audio_muted.reset();
        if (VMManager::HasValidVM())
            SPU2::SetOutputMuted(restore_muted);
    }, false);
}

@end
