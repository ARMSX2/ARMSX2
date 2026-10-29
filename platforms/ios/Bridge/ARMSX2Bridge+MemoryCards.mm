// ARMSX2Bridge+MemoryCards.mm — memory cards and ZIP import
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ZipHelpers.h"

#include "IOSRuntime.h"

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

@implementation ARMSX2Bridge (MemoryCards)

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
        if (!data.has_value() || data->empty() || data->size() > kMaxMemcardEntryBytes)
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
    if (ARMSX2HasBaseSettings())
        value = Host::GetBaseStringSettingValue("MemoryCards", key, value.c_str());

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

    if (ARMSX2HasBaseSettings()) {
        ARMSX2EditBaseSettings([&](SettingsInterface& si) {
            si.SetBoolValue("MemoryCards", enableKey, enabled);
            si.SetStringValue("MemoryCards", fileKey, nativeName.c_str());
        });
        Host::CommitBaseSettingChanges();
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

@end
