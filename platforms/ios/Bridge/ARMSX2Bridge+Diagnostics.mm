// ARMSX2Bridge+Diagnostics.mm — core defaults as INI text, for the report's settings file
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "VMManager.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"

// SetDefaultSettings links this one; the call below passes ui=false, so it never runs.
void Host::SetDefaultUISettings(SettingsInterface&) {}

@implementation ARMSX2Bridge (Diagnostics)

+ (nonnull NSString *)defaultSettingsINIText
{
    const std::string path = std::string(NSTemporaryDirectory().UTF8String) + "/armsx2-defaults-" +
        NSUUID.UUID.UUIDString.UTF8String + ".ini";
    NSString* text = @"";
    {
        INISettingsInterface si(path);
        VMManager::SetDefaultSettings(si, false, true, false, false, false);
        if (si.Save())
        {
            if (const auto contents = FileSystem::ReadFileToString(path.c_str()))
                text = [[NSString alloc] initWithBytes:contents->data() length:contents->size() encoding:NSUTF8StringEncoding] ?: @"";
        }
    }
    FileSystem::DeleteFilePath(path.c_str());
    return text;
}

@end
