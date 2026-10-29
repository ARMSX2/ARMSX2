// ARMSX2Bridge+BIOS.mm — BIOS list and default
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Config.h"
#include "ps2/BiosTools.h"
#include "pcsx2/INISettingsInterface.h"

#include "IOSRuntime.h"

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
    info.valid = NO;

    u32 version = 0;
    u32 region = 0;
    std::string description;
    std::string zone;
    if (IsBIOS(info.filePath.UTF8String, version, description, region, zone)) {
        NSString* zoneString = ARMSX2NSStringFromStdString(zone);
        info.valid = YES;
        info.regionName = ARMSX2BIOSDisplayRegionForZone(zoneString);
        info.countryCode = ARMSX2BIOSCountryCodeForZone(zoneString);
        info.descriptionText = ARMSX2NSStringFromStdString(description);
    }

    return info;
}

@implementation ARMSX2Bridge (BIOS)

#pragma mark - BIOS management

+ (nonnull NSString *)biosDirectory {
    NSString *docsPath = [self documentsDirectory];
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
    if (!ARMSX2HasBaseSettings()) return @"";
    std::string val = Host::GetBaseStringSettingValue("Filenames", "BIOS", "");
    return [NSString stringWithUTF8String:val.c_str()];
}

+ (void)setDefaultBIOS:(nonnull NSString *)biosName {
    if (!ARMSX2HasBaseSettings()) return;
    Host::SetBaseStringSettingValue("Filenames", "BIOS", biosName.UTF8String);
    Host::CommitBaseSettingChanges();
    EmuConfig.BaseFilenames.Bios = biosName.UTF8String;
    NSLog(@"setDefaultBIOS: %@", biosName);
}

@end
