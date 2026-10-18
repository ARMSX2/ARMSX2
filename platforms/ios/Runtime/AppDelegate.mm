// AppDelegate.mm — iOS app delegate, directory bootstrap, and main().

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

#include "common/Console.h"
#include "common/Path.h"

#include "pcsx2/Config.h"            // EmuFolders
#include "pcsx2/ImGui/ImGuiManager.h"

#include "common/Darwin/DarwinMisc.h"

#import <UIKit/UIKit.h>
#import <GameController/GameController.h>

#import "ARMSX2iOS-Swift.h"
#import "ARMSX2Bridge.h"

#include "IOSRuntime.h"
#import "PCSX2AppDelegate.h"
#import "PCSX2SceneDelegate.h"

// Written fresh on every build, see cmake/WriteGitHash.cmake.
#include "armsx2_git_hash.h"

@interface ARMSX2Application : UIApplication
@end

@implementation ARMSX2Application

- (void)sendEvent:(UIEvent *)event
{
    if ([event isKindOfClass:[UIPressesEvent class]] &&
        [SwiftUIHost handleControllerPressesEvent:(UIPressesEvent *)event])
    {
        return;
    }
    [super sendEvent:event];
}

@end

#pragma mark - SetupIOSDirectories
static void SetupIOSDirectories(const std::string& dataRoot)
{
    const char* dirs[] = {"bios", "iso", "logs", "memcards", "savestates",
                          "snaps", "cheats", "patches", "cache", "covers",
                          "gamesettings", "textures", "inputprofiles", "videos",
                          "inis", "resources"};
    mkdir(dataRoot.c_str(), 0755);
    for (auto d : dirs)
        mkdir((dataRoot + "/" + d).c_str(), 0755);

    EmuFolders::DataRoot = dataRoot;
    EmuFolders::Settings = dataRoot + "/inis";
    EmuFolders::Bios = dataRoot + "/bios";
    EmuFolders::Logs = dataRoot + "/logs";
    EmuFolders::Savestates = dataRoot + "/savestates";
    EmuFolders::MemoryCards = dataRoot + "/memcards";
    EmuFolders::Snapshots = dataRoot + "/snaps";
    EmuFolders::Cheats = dataRoot + "/cheats";
    EmuFolders::Patches = dataRoot + "/patches";
    EmuFolders::Cache = dataRoot + "/cache";
    EmuFolders::Covers = dataRoot + "/covers";
    EmuFolders::GameSettings = dataRoot + "/gamesettings";
    EmuFolders::Textures = dataRoot + "/textures";
    EmuFolders::InputProfiles = dataRoot + "/inputprofiles";
    EmuFolders::UserResources = dataRoot + "/resources";
}


@implementation PCSX2AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // Override point for customization after application launch.
    [UIDevice currentDevice].batteryMonitoringEnabled = YES;
    
    // --- Setup PCSX2 Environment (Moved from SceneDelegate) ---
    NSString *documentsDirectory = [ARMSX2Bridge documentsDirectory];
    NSString *resourcePath = [[NSBundle mainBundle] resourcePath];
    
    std::string dataRoot = [documentsDirectory UTF8String];
    SetupIOSDirectories(dataRoot);
    EmuFolders::AppRoot = [resourcePath UTF8String];
    EmuFolders::Resources = [resourcePath UTF8String];

    // platform-dirs gates Library/Caches on macOS alone, so iOS takes its XDG branch and drops a relative value (platform-dirs-0.3.0/src/lib.rs:37,88).
    NSArray *cachePaths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString *cachesDirectory = [cachePaths firstObject];
    if (cachesDirectory.length > 0)
        setenv("XDG_CACHE_HOME", [cachesDirectory UTF8String], 1);

    // --- Unified Logging Redirection ---
    // Force stderr and stdout to pcsx2_log.txt
    std::string logPath = dataRoot + "/pcsx2_log.txt";
    
    // Redirect stderr to file
    if (freopen(logPath.c_str(), "w", stderr) == NULL) { // "w" clears old logs
        printf("Reopen stderr failed\n");
    }
    
    // Redirect stdout to stderr
    if (dup2(fileno(stderr), fileno(stdout)) == -1) {
        fprintf(stderr, "Redirection of stdout failed\n");
    }
    
    // Disable buffering
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    
    // [iPSX2] Register File Descriptor for Signal Handler
    // We use the raw file descriptor of stderr (which is now our log file)
    DarwinMisc::SetCrashLogFD(fileno(stderr));
    
    // Log Proof Tag
    fprintf(stderr, "@@LOG_SINK@@ unified=1 path=%s pid=%d\n", logPath.c_str(), getpid());
    NSString* bundleID = [[NSBundle mainBundle] bundleIdentifier];
    fprintf(stderr, "@@BUNDLE_ID@@ %s\n", bundleID ? [bundleID UTF8String] : "(null)");
#ifndef ARMSX2_VERSION_STR
#define ARMSX2_VERSION_STR "dev"
#endif
    fprintf(stderr, "@@BUILD_ID@@ ARMSX2_iOS v%s %s %s %s\n",
        ARMSX2_VERSION_STR, ARMSX2_GIT_HASH, __DATE__, __TIME__);
    ARMSX2ConfigureImGuiFonts("app-launch");

    fflush(stderr);

    // Enable PCSX2 Console Output only (std::cout/cerr will now go to file)
    Log::SetConsoleOutputLevel(LOGLEVEL::LOGLEVEL_INFO);

    Console.WriteLn("PCSX2 iOS: resource root %s", EmuFolders::Resources.c_str());

    const char* cacheHome = getenv("XDG_CACHE_HOME");
    const char* homeRoot = getenv("HOME");
    const std::string legacyCache = Path::Combine(homeRoot ? homeRoot : "", ".cache/librashader");
    const std::string intendedCache = Path::Combine(cacheHome ? cacheHome : "", "librashader");
    Console.WriteLn("PCSX2 iOS: shader cache home=%s legacy=%s intended=%s", cacheHome ? cacheHome : "(unset)",
        access(legacyCache.c_str(), F_OK) == 0 ? "present" : "absent",
        access(intendedCache.c_str(), F_OK) == 0 ? "present" : "absent");

    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession options:(UISceneConnectionOptions *)options {
    const BOOL external = [connectingSceneSession.role isEqualToString:UIWindowSceneSessionRoleExternalDisplayNonInteractive];
    return [UISceneConfiguration configurationWithName:(external ? @"External Display" : @"Default Configuration")
        sessionRole:connectingSceneSession.role];
}

- (void)application:(UIApplication *)application didDiscardSceneSessions:(NSSet<UISceneSession *> *)sceneSessions {
}

@end


int main(int argc, char * argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv,
                                 NSStringFromClass([ARMSX2Application class]),
                                 NSStringFromClass([PCSX2AppDelegate class]));
    }
}
