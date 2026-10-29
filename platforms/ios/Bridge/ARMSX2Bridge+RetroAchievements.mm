// ARMSX2Bridge+RetroAchievements.mm — RetroAchievements state, login, toggles and toasts
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"

#include "Achievements.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/Error.h"

#include "IOSRuntime.h"

extern "C" void ARMSX2_PostRetroAchievementsStateChanged(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2RetroAchievementsStateChanged" object:nil];
    });
}

extern "C" void ARMSX2_PostRetroAchievementsNotification(const char* title, const char* message,
	const char* badgePath, float duration)
{
    NSString* titleString = title ? [NSString stringWithUTF8String:title] : nil;
    if (titleString.length == 0)
        return;

    NSString* messageString = message ? [NSString stringWithUTF8String:message] : nil;
    NSString* badgePathString = badgePath ? [NSString stringWithUTF8String:badgePath] : nil;
    if (!messageString)
        messageString = @"";
    if (!badgePathString)
        badgePathString = @"";

    // A non-positive duration means "use the SwiftUI default"; the key is omitted so the
    // receiver falls back to its own configured display time.
    NSNumber* durationNumber = (duration > 0.0f) ? @(duration) : nil;

    dispatch_async(dispatch_get_main_queue(), ^{
        ARMSX2RetroAchievementsToastInfo* toast = [[ARMSX2RetroAchievementsToastInfo alloc] init];
        toast.title = titleString;
        toast.message = messageString;
        toast.badgePath = badgePathString;
        toast.duration = durationNumber != nil ? durationNumber.doubleValue : 0.0;
        s_pendingRetroAchievementsNotification = toast;

        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2RetroAchievementsNotification"
                                                           object:nil];
    });
}

static dispatch_queue_t ARMSX2RetroAchievementsQueue()
{
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("org.armsx2.ios.retroachievements", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

static constexpr bool ARMSX2RetroAchievementsAvailable = true;
static constexpr bool ARMSX2RetroAchievementsHardcoreAvailable = true;

static NSString* ARMSX2RetroAchievementsUnavailableMessage()
{
    return @"RetroAchievements is temporarily unavailable in this build.";
}

static void ARMSX2SaveBaseSettingBool(const char* section, const char* key, bool value);

static void ARMSX2ForceRetroAchievementsHardcoreOff()
{
    Pcsx2Config::AchievementsOptions old_config = EmuConfig.Achievements;
    EmuConfig.Achievements.HardcoreMode = false;
    ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", false);

    if (Achievements::IsActive())
        Achievements::UpdateSettings(old_config);
}

static bool ARMSX2EnsureAchievementsClientInitialized()
{
    if (!EmuConfig.Achievements.Enabled)
        return false;

    if (!Achievements::IsActive())
        return Achievements::Initialize();

    return true;
}

static void ARMSX2SaveBaseSettingBool(const char* section, const char* key, bool value)
{
    if (!ARMSX2HasBaseSettings())
        return;
    Host::SetBaseBoolSettingValue(section, key, value);
    Host::CommitBaseSettingChanges();
}

static void ARMSX2UpdateAchievementsSettings(void (^mutate)())
{
    Pcsx2Config::AchievementsOptions old_config = EmuConfig.Achievements;
    mutate();
    Achievements::UpdateSettings(old_config);
    ARMSX2_PostRetroAchievementsStateChanged();
}

@implementation ARMSX2Bridge (RetroAchievements)

+ (nonnull NSDictionary<NSString *, id> *)retroAchievementsState {
    if (!ARMSX2RetroAchievementsHardcoreAvailable && EmuConfig.Achievements.HardcoreMode)
        ARMSX2ForceRetroAchievementsHardcoreOff();

    std::string username;
    std::string displayName;
    std::string avatarPath;
    std::string gameTitle;
    std::string richPresence;
    std::string gameIconPath;
    std::string gameIconURL;
    bool loggedIn = false;
    bool hasGame = false;
    bool active = false;
    bool hardcoreActive = false;
    bool hasAchievements = false;
    bool hasLeaderboards = false;
    bool hasRichPresence = false;
    u32 gameId = 0;
    u32 points = 0;
    u32 softcorePoints = 0;
    u32 unreadMessages = 0;
    u32 unlockedAchievements = 0;
    u32 totalAchievements = 0;
    u32 unlockedPoints = 0;
    u32 totalPoints = 0;
    const std::string savedUsernameValue = ARMSX2HasBaseSettings() ?
        Host::GetBaseStringSettingValue("Achievements", "Username") : std::string();
    const bool savedUsername = !savedUsernameValue.empty();
    const bool savedToken = !Host::GetStringSettingValue("Achievements", "Token").empty();

    Achievements::UserStats userStats;
    Achievements::GameStats gameStats;
    const bool haveUserStats = Achievements::GetCurrentUserStats(&userStats);
    const bool haveGameStats = Achievements::GetCurrentGameStats(&gameStats);

    {
        auto lock = Achievements::GetLock();
        active = Achievements::IsActive();
        if (haveUserStats) {
            username = userStats.username;
            displayName = userStats.display_name;
            avatarPath = userStats.avatar_path;
            points = userStats.points;
            softcorePoints = userStats.softcore_points;
            unreadMessages = userStats.unread_messages;
            loggedIn = !username.empty();
        } else if (active) {
            if (const char* loggedInUser = Achievements::GetLoggedInUserName()) {
                username = loggedInUser;
                displayName = username;
                loggedIn = !username.empty();
            }
        }
        if (active && loggedIn && avatarPath.empty())
            avatarPath = Achievements::GetLoggedInUserBadgePath();
        hasGame = Achievements::HasActiveGame();
        if (haveGameStats) {
            hasGame = true;
            gameTitle = gameStats.title;
            richPresence = gameStats.rich_presence;
            gameIconPath = gameStats.icon_path;
            gameIconURL = gameStats.icon_url;
            gameId = gameStats.game_id;
            unlockedAchievements = gameStats.unlocked_achievements;
            totalAchievements = gameStats.total_achievements;
            unlockedPoints = gameStats.unlocked_points;
            totalPoints = gameStats.total_points;
            hasAchievements = gameStats.has_achievements;
            hasLeaderboards = gameStats.has_leaderboards;
            hasRichPresence = gameStats.has_rich_presence;
        } else if (hasGame) {
            gameTitle = Achievements::GetGameTitle();
            richPresence = Achievements::GetRichPresenceString();
            gameIconURL = Achievements::GetGameIconURL();
            gameId = Achievements::GetGameID();
            hasAchievements = Achievements::HasAchievements();
            hasLeaderboards = Achievements::HasLeaderboards();
            hasRichPresence = Achievements::HasRichPresence();
        }
        hardcoreActive = Achievements::IsHardcoreModeActive();
    }

    const bool savedLogin = savedUsername && savedToken;
    const bool loginPending = EmuConfig.Achievements.Enabled && active && !loggedIn && savedLogin;
    if (username.empty() && savedUsername) {
        username = savedUsernameValue;
        displayName = savedUsernameValue;
    }

    return @{
        @"supported": @(ARMSX2RetroAchievementsAvailable),
        @"hardcoreSupported": @(ARMSX2RetroAchievementsHardcoreAvailable),
        @"unavailableMessage": ARMSX2RetroAchievementsUnavailableMessage(),
        @"enabled": @(EmuConfig.Achievements.Enabled),
        @"active": @(active),
        @"loggedIn": @(loggedIn),
        @"savedLogin": @(savedLogin),
        @"loginPending": @(loginPending),
        @"username": ARMSX2NSStringFromStdString(username),
        @"displayName": ARMSX2NSStringFromStdString(displayName.empty() ? username : displayName),
        @"avatarPath": ARMSX2NSStringFromStdString(avatarPath),
        @"points": @(points),
        @"softcorePoints": @(softcorePoints),
        @"unreadMessages": @(unreadMessages),
        @"hardcorePreference": @(EmuConfig.Achievements.HardcoreMode),
        @"hardcoreActive": @(hardcoreActive),
        @"notifications": @(EmuConfig.Achievements.Notifications),
        @"leaderboardNotifications": @(EmuConfig.Achievements.LeaderboardNotifications),
        @"overlays": @(EmuConfig.Achievements.Overlays),
        @"hasActiveGame": @(hasGame),
        @"gameTitle": ARMSX2NSStringFromStdString(gameTitle),
        @"richPresence": ARMSX2NSStringFromStdString(richPresence),
        @"gameIconPath": ARMSX2NSStringFromStdString(gameIconPath),
        @"gameIconURL": ARMSX2NSStringFromStdString(gameIconURL),
        @"unlockedAchievements": @(unlockedAchievements),
        @"totalAchievements": @(totalAchievements),
        @"unlockedPoints": @(unlockedPoints),
        @"totalPoints": @(totalPoints),
        @"gameId": @(gameId),
        @"hasAchievements": @(hasAchievements),
        @"hasLeaderboards": @(hasLeaderboards),
        @"hasRichPresence": @(hasRichPresence),
    };
}

+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)retroAchievementsForCurrentGame {
    std::vector<Achievements::AchievementInfo> achievements;
    if (!Achievements::GetCurrentAchievementList(&achievements))
        return @[];

    NSMutableArray<NSDictionary<NSString *, id> *> *result = [NSMutableArray arrayWithCapacity:achievements.size()];
    for (const Achievements::AchievementInfo& achievement : achievements) {
        [result addObject:@{
            @"id": @(achievement.id),
            @"title": ARMSX2NSStringFromStdString(achievement.title),
            @"description": ARMSX2NSStringFromStdString(achievement.description),
            @"badgePath": ARMSX2NSStringFromStdString(achievement.badge_path),
            @"measuredProgress": ARMSX2NSStringFromStdString(achievement.measured_progress),
            @"points": @(achievement.points),
            @"state": @(achievement.state),
            @"category": @(achievement.category),
            @"bucket": @(achievement.bucket),
            @"measuredPercent": @(achievement.measured_percent),
        }];
    }

    return result;
}

+ (nullable ARMSX2RetroAchievementsToastInfo *)consumePendingRetroAchievementsNotification {
    __block ARMSX2RetroAchievementsToastInfo* pending = nil;
    void (^consume)(void) = ^{
        pending = s_pendingRetroAchievementsNotification;
        s_pendingRetroAchievementsNotification = nil;
    };

    if ([NSThread isMainThread]) {
        consume();
    } else {
        dispatch_sync(dispatch_get_main_queue(), consume);
    }

    return pending;
}

+ (BOOL)isRetroAchievementsHardcoreActive {
    return ARMSX2RetroAchievementsHardcoreActive() ? YES : NO;
}

+ (void)setRetroAchievementsEnabled:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        if (EmuConfig.Achievements.Enabled == enable) {
            ARMSX2SaveBaseSettingBool("Achievements", "Enabled", enable);
            if (!enable || !ARMSX2RetroAchievementsHardcoreAvailable)
                ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", false);
            ARMSX2_PostRetroAchievementsStateChanged();
            return;
        }

        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Enabled = enable;
            if (!enable || !ARMSX2RetroAchievementsHardcoreAvailable)
                EmuConfig.Achievements.HardcoreMode = false;
            ARMSX2SaveBaseSettingBool("Achievements", "Enabled", enable);
            if (!enable || !ARMSX2RetroAchievementsHardcoreAvailable)
                ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", false);
        });
        NSLog(@"[ARMSX2Bridge] RetroAchievements enabled=%d", enable ? 1 : 0);
    });
}

+ (void)setRetroAchievementsHardcore:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        if (!ARMSX2RetroAchievementsHardcoreAvailable) {
            ARMSX2ForceRetroAchievementsHardcoreOff();
            ARMSX2_PostRetroAchievementsStateChanged();
            if (enable)
                NSLog(@"[ARMSX2Bridge] RetroAchievements hardcore rejected: unavailable");
            return;
        }

        if (EmuConfig.Achievements.HardcoreMode == enable) {
            ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", enable);
            ARMSX2_PostRetroAchievementsStateChanged();
            return;
        }

        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.HardcoreMode = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", enable);
        });
        NSLog(@"[ARMSX2Bridge] RetroAchievements hardcore=%d", enable ? 1 : 0);
    });
}

+ (void)setRetroAchievementsNotifications:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Notifications = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "Notifications", enable);
        });
    });
}

+ (void)setRetroAchievementsLeaderboards:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.LeaderboardNotifications = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "LeaderboardNotifications", enable);
        });
    });
}

+ (void)setRetroAchievementsOverlays:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Overlays = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "Overlays", enable);
        });
    });
}

+ (void)loginRetroAchievementsWithUsername:(nonnull NSString *)username password:(nonnull NSString *)password completion:(nullable ARMSX2RetroAchievementsCompletion)completion {
    NSString* trimmedUsername = [username stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSString* nativePassword = password ?: @"";
    ARMSX2RetroAchievementsCompletion callback = [completion copy];

    if (trimmedUsername.length == 0 || nativePassword.length == 0) {
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, @"Enter your RetroAchievements username and password."); });
        return;
    }

    std::string user(trimmedUsername.UTF8String ?: "");
    std::string pass(nativePassword.UTF8String ?: "");

    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        @autoreleasepool {
            if (!EmuConfig.Achievements.Enabled) {
                Pcsx2Config::AchievementsOptions old_config = EmuConfig.Achievements;
                EmuConfig.Achievements.Enabled = true;
                ARMSX2SaveBaseSettingBool("Achievements", "Enabled", true);
                Achievements::UpdateSettings(old_config);
            }

            if (!ARMSX2EnsureAchievementsClientInitialized()) {
                NSString* message = @"RetroAchievements could not initialize its network client.";
                NSLog(@"[ARMSX2Bridge] RetroAchievements login result=0 message=%@", message);
                ARMSX2_PostRetroAchievementsStateChanged();
                if (callback)
                    dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, message); });
                return;
            }

            Error error;
            const bool result = Achievements::Login(user.c_str(), pass.c_str(), &error);
            NSString* message = result ? @"RetroAchievements login successful." :
                (error.IsValid() ? ARMSX2NSStringFromStdString(error.GetDescription()) : @"RetroAchievements login failed.");

            if (result && ARMSX2HasBaseSettings())
                Host::CommitBaseSettingChanges();

            NSLog(@"[ARMSX2Bridge] RetroAchievements login result=%d message=%@", result ? 1 : 0, message);
            ARMSX2_PostRetroAchievementsStateChanged();

            if (callback)
                dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO, message); });
        }
    });
}

+ (void)logoutRetroAchievements {
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        Achievements::Logout();
        if (ARMSX2HasBaseSettings())
            Host::CommitBaseSettingChanges();
        NSLog(@"[ARMSX2Bridge] RetroAchievements logout");
        ARMSX2_PostRetroAchievementsStateChanged();
    });
}

@end
