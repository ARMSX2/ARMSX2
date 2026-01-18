//
//  HostIOS.mm
//  ARMSX2
//
//  iOS Host implementation for PCSX2
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include "../../../app/src/main/cpp/pcsx2/Host.h"
#include "../../../app/src/main/cpp/pcsx2/GS/GS.h"
#include "../../../app/src/main/cpp/common/FileSystem.h"
#include "../../../app/src/main/cpp/common/Path.h"
#include "../../../app/src/main/cpp/common/Console.h"
#include <string>
#include <vector>
#include <mutex>

// Global settings mutex
static std::mutex s_settings_mutex;
static std::unique_ptr<INISettingsInterface> s_base_settings_interface;
static std::unique_ptr<INISettingsInterface> s_game_settings_interface;

namespace Host
{

// Translation functions
const char* TranslateToCString(const std::string_view context, const std::string_view msg)
{
    // For iOS, just return the English string
    // TODO: Implement proper iOS localization
    static thread_local std::string s_translation_buffer;
    s_translation_buffer = msg;
    return s_translation_buffer.c_str();
}

std::string_view TranslateToStringView(const std::string_view context, const std::string_view msg)
{
    return msg;
}

std::string TranslateToString(const std::string_view context, const std::string_view msg)
{
    return std::string(msg);
}

std::string TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
    return std::string(msg);
}

void ClearTranslationCache()
{
    // Nothing to do
}

// OSD Messages
void AddOSDMessage(std::string message, float duration)
{
    NSLog(@"[ARMSX2-OSD] %s (%.1fs)", message.c_str(), duration);
    // TODO: Display in iOS UI
}

void AddKeyedOSDMessage(std::string key, std::string message, float duration)
{
    NSLog(@"[ARMSX2-OSD] [%s] %s (%.1fs)", key.c_str(), message.c_str(), duration);
    // TODO: Display in iOS UI
}

void AddIconOSDMessage(std::string key, const char* icon, const std::string_view message, float duration)
{
    NSLog(@"[ARMSX2-OSD] [%s] %.*s (%.1fs)", key.c_str(), static_cast<int>(message.length()), message.data(), duration);
    // TODO: Display in iOS UI with icon
}

void RemoveKeyedOSDMessage(std::string key)
{
    NSLog(@"[ARMSX2-OSD] Removing message: %s", key.c_str());
}

void ClearOSDMessages()
{
    NSLog(@"[ARMSX2-OSD] Clearing all messages");
}

// Async messages
void ReportInfoAsync(const std::string_view title, const std::string_view message)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        NSString* titleStr = [NSString stringWithUTF8String:title.data()];
        NSString* msgStr = [NSString stringWithUTF8String:message.data()];
        NSLog(@"[ARMSX2-INFO] %@: %@", titleStr, msgStr);

        // TODO: Show UIAlertController
    });
}

void ReportFormattedInfoAsync(const std::string_view title, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    std::string message = fmt::vsprintf(format, ap);
    va_end(ap);

    ReportInfoAsync(title, message);
}

void ReportErrorAsync(const std::string_view title, const std::string_view message)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        NSString* titleStr = [NSString stringWithUTF8String:title.data()];
        NSString* msgStr = [NSString stringWithUTF8String:message.data()];
        NSLog(@"[ARMSX2-ERROR] %@: %@", titleStr, msgStr);

        // TODO: Show UIAlertController with error style
    });
}

void ReportFormattedErrorAsync(const std::string_view title, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    std::string message = fmt::vsprintf(format, ap);
    va_end(ap);

    ReportErrorAsync(title, message);
}

// Synchronous confirmation
bool ConfirmMessage(const std::string_view title, const std::string_view message)
{
    __block bool result = false;

    dispatch_sync(dispatch_get_main_queue(), ^{
        NSString* titleStr = [NSString stringWithUTF8String:title.data()];
        NSString* msgStr = [NSString stringWithUTF8String:message.data()];

        // TODO: Show UIAlertController and wait for response
        NSLog(@"[ARMSX2-CONFIRM] %@: %@", titleStr, msgStr);
        result = true; // Default to yes for now
    });

    return result;
}

bool ConfirmFormattedMessage(const std::string_view title, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    std::string message = fmt::vsprintf(format, ap);
    va_end(ap);

    return ConfirmMessage(title, message);
}

// Mode checks
bool InBatchMode()
{
    return false;
}

bool InNoGUIMode()
{
    return false;
}

// URL opening
void OpenURL(const std::string_view url)
{
    NSString* urlStr = [NSString stringWithUTF8String:url.data()];
    NSURL* nsurl = [NSURL URLWithString:urlStr];

    if (nsurl) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [[UIApplication sharedApplication] openURL:nsurl options:@{} completionHandler:nil];
        });
    }
}

// Clipboard
bool CopyTextToClipboard(const std::string_view text)
{
    NSString* textStr = [NSString stringWithUTF8String:text.data()];
    [[UIPasteboard generalPasteboard] setString:textStr];
    return true;
}

// Resource directory
bool EnsureResourceSubdirectory(const char* relative_path)
{
    NSString* documentsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString* fullPath = [documentsPath stringByAppendingPathComponent:[NSString stringWithUTF8String:relative_path]];

    NSError* error = nil;
    [[NSFileManager defaultManager] createDirectoryAtPath:fullPath
                                withIntermediateDirectories:YES
                                attributes:nil
                                error:&error];

    return error == nil;
}

// Settings reset
bool RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
    NSLog(@"[ARMSX2-Host] Reset settings requested");
    // TODO: Implement settings reset
    return true;
}

// Display resize
void RequestResizeHostDisplay(s32 width, s32 height)
{
    NSLog(@"[ARMSX2-Host] Resize requested: %dx%d", width, height);
    // iOS handles this automatically
}

// CPU thread execution
void RunOnCPUThread(std::function<void()> function, bool block)
{
    // For iOS, just execute directly for now
    // TODO: Implement proper CPU thread management
    if (block) {
        function();
    } else {
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
            function();
        });
    }
}

// Game list
void RefreshGameListAsync(bool invalidate_cache)
{
    NSLog(@"[ARMSX2-Host] Game list refresh requested");
    // TODO: Implement game list refresh
}

void CancelGameListRefresh()
{
    NSLog(@"[ARMSX2-Host] Game list refresh cancelled");
}

// VM shutdown
void RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
    NSLog(@"[ARMSX2-Host] VM shutdown requested");
    // TODO: Implement proper shutdown
}

// HTTP user agent
std::string GetHTTPUserAgent()
{
    NSString* version = [[UIDevice currentDevice] systemVersion];
    NSString* model = [[UIDevice currentDevice] model];
    return fmt::format("ARMSX2/1.0 (iOS {}; {})", [version UTF8String], [model UTF8String]);
}

// Base settings implementation
std::string GetBaseStringSettingValue(const char* section, const char* key, const char* default_value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return default_value;

    return s_base_settings_interface->GetStringValue(section, key, default_value);
}

SmallString GetBaseSmallStringSettingValue(const char* section, const char* key, const char* default_value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return SmallString(default_value);

    return s_base_settings_interface->GetSmallStringValue(section, key, default_value);
}

TinyString GetBaseTinyStringSettingValue(const char* section, const char* key, const char* default_value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return TinyString(default_value);

    return s_base_settings_interface->GetTinyStringValue(section, key, default_value);
}

bool GetBaseBoolSettingValue(const char* section, const char* key, bool default_value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return default_value;

    return s_base_settings_interface->GetBoolValue(section, key, default_value);
}

int GetBaseIntSettingValue(const char* section, const char* key, int default_value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return default_value;

    return s_base_settings_interface->GetIntValue(section, key, default_value);
}

uint GetBaseUIntSettingValue(const char* section, const char* key, uint default_value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return default_value;

    return s_base_settings_interface->GetUIntValue(section, key, default_value);
}

float GetBaseFloatSettingValue(const char* section, const char* key, float default_value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return default_value;

    return s_base_settings_interface->GetFloatValue(section, key, default_value);
}

double GetBaseDoubleSettingValue(const char* section, const char* key, double default_value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return default_value;

    return s_base_settings_interface->GetDoubleValue(section, key, default_value);
}

std::vector<std::string> GetBaseStringListSetting(const char* section, const char* key)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return std::vector<std::string>();

    return s_base_settings_interface->GetStringList(section, key);
}

// Base settings writing
void SetBaseBoolSettingValue(const char* section, const char* key, bool value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
        s_base_settings_interface->SetBoolValue(section, key, value);
}

void SetBaseIntSettingValue(const char* section, const char* key, int value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
        s_base_settings_interface->SetIntValue(section, key, value);
}

void SetBaseUIntSettingValue(const char* section, const char* key, uint value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
        s_base_settings_interface->SetUIntValue(section, key, value);
}

void SetBaseFloatSettingValue(const char* section, const char* key, float value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
        s_base_settings_interface->SetFloatValue(section, key, value);
}

void SetBaseStringSettingValue(const char* section, const char* key, const char* value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
        s_base_settings_interface->SetStringValue(section, key, value);
}

void SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
        s_base_settings_interface->SetStringList(section, key, values);
}

bool AddBaseValueToStringList(const char* section, const char* key, const char* value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return false;

    return s_base_settings_interface->AddToStringList(section, key, value);
}

bool RemoveBaseValueFromStringList(const char* section, const char* key, const char* value)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return false;

    return s_base_settings_interface->RemoveFromStringList(section, key, value);
}

bool ContainsBaseSettingValue(const char* section, const char* key)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (!s_base_settings_interface)
        return false;

    return s_base_settings_interface->ContainsValue(section, key);
}

void RemoveBaseSettingValue(const char* section, const char* key)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
        s_base_settings_interface->DeleteValue(section, key);
}

void CommitBaseSettingChanges()
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
        s_base_settings_interface->Save();
}

// Runtime settings (same as base for iOS)
std::string GetStringSettingValue(const char* section, const char* key, const char* default_value)
{
    return GetBaseStringSettingValue(section, key, default_value);
}

SmallString GetSmallStringSettingValue(const char* section, const char* key, const char* default_value)
{
    return GetBaseSmallStringSettingValue(section, key, default_value);
}

TinyString GetTinyStringSettingValue(const char* section, const char* key, const char* default_value)
{
    return GetBaseTinyStringSettingValue(section, key, default_value);
}

bool GetBoolSettingValue(const char* section, const char* key, bool default_value)
{
    return GetBaseBoolSettingValue(section, key, default_value);
}

int GetIntSettingValue(const char* section, const char* key, int default_value)
{
    return GetBaseIntSettingValue(section, key, default_value);
}

uint GetUIntSettingValue(const char* section, const char* key, uint default_value)
{
    return GetBaseUIntSettingValue(section, key, default_value);
}

float GetFloatSettingValue(const char* section, const char* key, float default_value)
{
    return GetBaseFloatSettingValue(section, key, default_value);
}

double GetDoubleSettingValue(const char* section, const char* key, double default_value)
{
    return GetBaseDoubleSettingValue(section, key, default_value);
}

std::vector<std::string> GetStringListSetting(const char* section, const char* key)
{
    return GetBaseStringListSetting(section, key);
}

// Settings interface access
std::unique_lock<std::mutex> GetSettingsLock()
{
    return std::unique_lock<std::mutex>(s_settings_mutex);
}

SettingsInterface* GetSettingsInterface()
{
    return s_base_settings_interface.get();
}

// Default UI settings
void SetDefaultUISettings(SettingsInterface& si)
{
    NSLog(@"[ARMSX2-Host] Setting default UI settings");
    // TODO: Set iOS-specific defaults
}

// iOS-specific initialization
void InitializeHostSettings(const char* settings_path)
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    s_base_settings_interface = std::make_unique<INISettingsInterface>(settings_path);
    s_base_settings_interface->Load();

    NSLog(@"[ARMSX2-Host] Settings initialized: %s", settings_path);
}

void ShutdownHostSettings()
{
    std::unique_lock<std::mutex> lock(s_settings_mutex);
    if (s_base_settings_interface)
    {
        s_base_settings_interface->Save();
        s_base_settings_interface.reset();
    }
}

} // namespace Host

// Console output
void Console::WriteLn(ConsoleColors color, std::string_view fmt)
{
    NSLog(@"[PCSX2] %.*s", static_cast<int>(fmt.length()), fmt.data());
}

void Console::SetTitle(std::string_view title)
{
    // iOS doesn't have a console window
}

void Console::Write(ConsoleColors color, std::string_view fmt)
{
    NSLog(@"[PCSX2] %.*s", static_cast<int>(fmt.length()), fmt.data());
}
