// ARMSX2Bridge+Diagnostics.mm — core defaults as INI text, for the report's settings file
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#import <ImageIO/ImageIO.h>

#include "VMManager.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/MTGS.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "pcsx2/DebugTools/Debug.h"

#include <atomic>
#include <memory>
#include <vector>

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

+ (void)setVerboseLogging:(BOOL)enabled
{
    if (!ARMSX2HasBaseSettings())
        return;
    for (NSString* key in @[ @"EnableVerbose", @"EnableEEConsole", @"EnableIOPConsole" ])
        [self setINIBool:@"Logging" key:key value:enabled];
    // The CPU thread reads these flags on every console print; UpdateLoggingSettings is private, so this repeats its rules.
    Host::RunOnCPUThread([enabled]() {
        const bool sinks = Log::IsFileOutputEnabled() || Log::IsConsoleOutputEnabled();
        ConsoleLogging.eeConsole.Enabled = sinks && enabled;
        ConsoleLogging.iopConsole.Enabled = sinks && enabled;
        // UpdateLoggingSettings ignores a verbose flip while file logging is already on, so the level moves here.
        if (Log::IsFileOutputEnabled())
            Log::SetFileOutputLevel((IsDevBuild || enabled) ? LOGLEVEL_DEV : LOGLEVEL_INFO, Path::Combine(EmuFolders::Logs, "emulog.txt"));
    });
}

+ (void)capturePausedFrameWithCompletion:(nonnull void (^)(NSData *_Nullable png))completion
{
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto finish = [fired, completion](NSData* png) {
        if (!fired->exchange(true))
            completion(png);
    };
    if (!VMManager::HasValidVM()) {
        finish(nil);
        return;
    }
    // The GS thread can stall on a lost device; the caller must never wait on it.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ finish(nil); });
    Host::RunOnCPUThread([finish]() {
        u32 width = 0, height = 0;
        std::vector<u32> pixels;
        if (!VMManager::HasValidVM() || !MTGS::SaveMemorySnapshot(0, 0, true, false, &width, &height, &pixels) ||
            width == 0 || height == 0 || pixels.size() < static_cast<size_t>(width) * height) {
            finish(nil);
            return;
        }
        // The core does the same before it writes a state screenshot.
        for (u32& p : pixels)
            p |= 0xFF000000u;
        NSData* raw = [NSData dataWithBytes:pixels.data() length:static_cast<size_t>(width) * height * sizeof(u32)];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)raw);
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, space, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault,
                provider, nullptr, false, kCGRenderingIntentDefault);
            NSMutableData* png = [NSMutableData data];
            bool ok = false;
            if (image) {
                if (CGImageDestinationRef dest = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)png, CFSTR("public.png"), 1, nullptr)) {
                    CGImageDestinationAddImage(dest, image, nullptr);
                    ok = CGImageDestinationFinalize(dest);
                    CFRelease(dest);
                }
                CGImageRelease(image);
            }
            CGColorSpaceRelease(space);
            CGDataProviderRelease(provider);
            finish(ok ? png : nil);
        });
    });
}

@end
