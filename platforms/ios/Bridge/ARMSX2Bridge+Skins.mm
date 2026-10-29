// ARMSX2Bridge+Skins.mm — controller skin archives and manifests
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "common/ZipHelpers.h"

static BOOL ARMSX2IsControllerSkinImageName(NSString* name)
{
    NSString* ext = name.pathExtension.lowercaseString;
    return [ext isEqualToString:@"png"] || [ext isEqualToString:@"jpg"] ||
           [ext isEqualToString:@"jpeg"] || [ext isEqualToString:@"webp"];
}

static BOOL ARMSX2IsControllerSkinImportName(NSString* name)
{
    if (ARMSX2IsControllerSkinImageName(name))
        return YES;

    return [name.pathExtension.lowercaseString isEqualToString:@"json"];
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

@implementation ARMSX2Bridge (Skins)

+ (nonnull NSArray<NSURL *> *)extractControllerSkinArchiveAtURL:(nonnull NSURL *)archiveURL
                                                    toDirectory:(nonnull NSURL *)destinationDirectory {
    static const zip_uint64_t kMaxSkinArchiveEntryBytes = 16 * 1024 * 1024;
    static const NSUInteger kMaxSkinArchiveEntries = 64;
    static const zip_int64_t kMaxSkinArchiveTotalEntries = 512;
    // Layout JSON gets its own budget so it cannot take all 64 shared slots.
    static const zip_uint64_t kMaxLooseLayoutBytes = 1024 * 1024;
    static const NSUInteger kMaxLooseLayoutEntries = 8;
    NSUInteger looseLayoutCount = 0;

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
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
        if (extracted.count >= kMaxSkinArchiveEntries)
            break;

        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;
        if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxSkinArchiveEntryBytes)
            continue;

        NSString *entryName = [NSString stringWithUTF8String:stat.name];
        // Skips __MACOSX and dotfiles, whose "._" siblings would otherwise use up the entry budget.
        if ([entryName containsString:@"__MACOSX"] || [entryName.lastPathComponent hasPrefix:@"."])
            continue;
        if (entryName.length == 0 || [entryName hasSuffix:@"/"] || !ARMSX2IsControllerSkinImportName(entryName))
            continue;
        if (!ARMSX2IsControllerSkinImageName(entryName)) {
            if (looseLayoutCount >= kMaxLooseLayoutEntries)
                continue;
            if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxLooseLayoutBytes)
                continue;
            looseLayoutCount++;
        }

        auto file = zip_fopen_index_managed(zf.get(), i, ZIP_FL_ENC_GUESS);
        if (!file)
            continue;

        const zip_uint64_t entryCap = ARMSX2IsControllerSkinImageName(entryName) ? kMaxSkinArchiveEntryBytes : kMaxLooseLayoutBytes;
        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->empty() || data->size() > entryCap)
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

+ (nullable NSData *)peekSkinManifestDataAtURL:(NSURL *)archiveURL {
    static const zip_uint64_t kMaxManifestBytes = 16 * 1024 * 1024;

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
            if ((stat.valid & ZIP_STAT_SIZE) && stat.size > kMaxManifestBytes) {
                continue;
            }
            auto file = zip_fopen_index_managed(zf.get(), i, ZIP_FL_ENC_GUESS);
            if (!file) {
                continue;
            }
            std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
            if (!data.has_value() || data->empty() || data->size() > kMaxManifestBytes) {
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
        if (!data.has_value() || data->empty() || data->size() > kMaxPackageEntryBytes) {
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

@end
