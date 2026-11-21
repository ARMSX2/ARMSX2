// GamepadRumble.mm — Core Haptics engine, rumble channels and phone rumble
// SPDX-License-Identifier: GPL-3.0+

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>

#include <algorithm>
#include <cmath>

#include "common/Console.h"
#include "pcsx2/Host.h"

#include "IOSRuntime.h"
#import "GamepadInternal.h"

static constexpr float ARMSX2_GAME_RUMBLE_DEFAULT_STRENGTH = 1.0f;

// The two PS2 motors feel nothing alike, so the phone plays them as two channels.
// Core Haptics calls low sharpness rounded and high sharpness crisp, which maps to
// the heavy motor and the small buzzer.
enum : u32
{
    ARMSX2_RUMBLE_CHANNEL_LARGE = 0,
    ARMSX2_RUMBLE_CHANNEL_SMALL = 1,
    ARMSX2_RUMBLE_CHANNEL_COUNT = 2,
};
static constexpr float ARMSX2_RUMBLE_CHANNEL_SHARPNESS[ARMSX2_RUMBLE_CHANNEL_COUNT] = { 0.18f, 0.82f };
// The PS2 small motor has one speed and no analog control at all, so "on" is just
// a level we pick. Matching the real pad means it does not vary.
static constexpr float ARMSX2_SMALL_MOTOR_LEVEL = 0.52f;
static constexpr double ARMSX2_RUMBLE_LARGE_ATTACK_SECONDS = 0.065;
static constexpr double ARMSX2_RUMBLE_SMALL_ATTACK_SECONDS = 0.028;
static constexpr double ARMSX2_RUMBLE_LARGE_RELEASE_SECONDS = 0.120;
static constexpr double ARMSX2_RUMBLE_SMALL_RELEASE_SECONDS = 0.060;
static constexpr double ARMSX2_RUMBLE_CROSSFADE_SECONDS = 0.045;
static constexpr float ARMSX2_PHONE_RUMBLE_BASELINE_SETTING = 0.25f;
static constexpr float ARMSX2_PHONE_RUMBLE_MAX_GAIN = 3.0f;
static constexpr float ARMSX2_PHONE_RUMBLE_MEDIUM_THRESHOLD = 0.34f;
static constexpr float ARMSX2_PHONE_RUMBLE_HARD_THRESHOLD = 0.67f;

enum class ARMSX2PhoneRumbleClass : u8
{
    Weak,
    Medium,
    Hard,
};

struct ARMSX2RumbleEnvelope
{
    float start_level = 0.0f;
    float target_level = 0.0f;
    double start_time = 0.0;
    double hold_duration = 0.0;
    double duration = 0.0;
};

static CHHapticEngine* s_nativeHapticEngine = nil;
static id<CHHapticAdvancedPatternPlayer> s_nativeHapticPlayer[ARMSX2_RUMBLE_CHANNEL_COUNT] = {};
static ARMSX2RumbleEnvelope s_nativeHapticEnvelope[ARMSX2_RUMBLE_CHANNEL_COUNT];
static float s_nativeHapticReleaseScale[ARMSX2_RUMBLE_CHANNEL_COUNT] = { 1.0f, 1.0f };
static u32 s_nativeHapticStopGeneration[ARMSX2_RUMBLE_CHANNEL_COUNT] = {};
static bool s_nativeHapticEngineRunning = false;
std::atomic<int> s_nativeHapticSourceGamepad{-1};
std::atomic<u32> s_nativeHapticResyncSlotMask{0};
static double s_nativeHapticLastTransientTime = 0.0;
static u32 s_nativeAppliedGamepadRumble = 0;
static bool s_nativeAppliedGamepadRumbleValid = false;
static bool s_loggedNativeGamepadRumbleReady = false;
static bool s_loggedNativeGamepadRumbleUnavailable = false;

#pragma mark - CoreHaptics rumble
u32 ARMSX2PackGamepadRumble(float large_intensity, float small_intensity)
{
    const u32 large = static_cast<u32>(std::clamp(large_intensity, 0.0f, 1.0f) * 65535.0f);
    const u32 small = static_cast<u32>(std::clamp(small_intensity, 0.0f, 1.0f) * 65535.0f);
    return ((large & 0xffffu) << 16) | (small & 0xffffu);
}

float ARMSX2RumbleLargeIntensity(u32 packed)
{
    return static_cast<float>((packed >> 16) & 0xffffu) / 65535.0f;
}

float ARMSX2RumbleSmallIntensity(u32 packed)
{
    return static_cast<float>(packed & 0xffffu) / 65535.0f;
}

float ARMSX2GameRumbleStrength()
{
    const float strength = s_settings_interface
        ? Host::GetBaseFloatSettingValue(
              "ARMSX2iOS/UI", "GameRumbleStrength", ARMSX2_GAME_RUMBLE_DEFAULT_STRENGTH)
        : ARMSX2_GAME_RUMBLE_DEFAULT_STRENGTH;
    return std::clamp(strength, 0.0f, 2.0f);
}

u16 ARMSX2ScaleControllerRumbleMotor(u16 raw_intensity, float strength)
{
    // Cap at the standard maximum first, so 100% leaves the standard output unchanged
    // and 2x reaches twice that.
    const float baseline = static_cast<float>(std::min(raw_intensity, ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY));
    return static_cast<u16>(std::clamp(std::lround(baseline * strength), 0l, 65535l));
}

u32 ARMSX2ScaleNativeRumble(u32 packed, float strength)
{
    return ARMSX2PackGamepadRumble(
        ARMSX2RumbleLargeIntensity(packed) * strength,
        ARMSX2RumbleSmallIntensity(packed) * strength);
}

static void ARMSX2StopRumbleChannelOnMain(u32 channel)
{
    if (channel >= ARMSX2_RUMBLE_CHANNEL_COUNT)
        return;

    // Any delayed release already queued for this channel becomes stale as soon
    // as its generation changes. This keeps a previous fade-out from stopping a
    // newly restarted rumble.
    s_nativeHapticStopGeneration[channel]++;
    if (s_nativeHapticPlayer[channel]) {
        NSError* error = nil;
        [s_nativeHapticPlayer[channel] stopAtTime:CHHapticTimeImmediate error:&error];
        if (error)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Device rumble stop failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        [s_nativeHapticPlayer[channel] release];
        s_nativeHapticPlayer[channel] = nil;
    }

    s_nativeHapticEnvelope[channel] = {};
    s_nativeHapticReleaseScale[channel] = 1.0f;
}

static void ARMSX2StopNativeGamepadRumbleOnMain()
{
    for (u32 channel = 0; channel < ARMSX2_RUMBLE_CHANNEL_COUNT; channel++)
        ARMSX2StopRumbleChannelOnMain(channel);
}

void ARMSX2ResetNativeGamepadRumbleOnMain()
{
    ARMSX2StopNativeGamepadRumbleOnMain();
    if (s_nativeHapticEngine) {
        [s_nativeHapticEngine stopWithCompletionHandler:nil];
        [s_nativeHapticEngine release];
        s_nativeHapticEngine = nil;
    }
    s_nativeHapticEngineRunning = false;
    s_nativeHapticSourceGamepad.store(-1, std::memory_order_relaxed);
    s_nativeHapticResyncSlotMask.store(0, std::memory_order_release);
    s_nativeHapticLastTransientTime = 0.0;
    s_nativeAppliedGamepadRumble = 0;
    s_nativeAppliedGamepadRumbleValid = false;
    s_loggedNativeGamepadRumbleReady = false;
}

GCController* ARMSX2FindNativeHapticController()
{
    for (GCController* controller in [GCController controllers]) {
        if (controller.haptics)
            return controller;
    }

    return nil;
}

// True on a phone with a taptic engine. iPads and the older phones have none, and
// asking them to play a pattern just fails every frame, so check once and remember.
bool ARMSX2DeviceSupportsHaptics()
{
    static const bool supported = [] {
        if (@available(iOS 13.0, *))
            return [CHHapticEngine capabilitiesForHardware].supportsHaptics != NO;
        return false;
    }();
    return supported;
}

static bool ARMSX2EnsureNativeRumbleEngineOnMain()
{
    if (@available(iOS 14.0, *)) {
    } else {
        return false;
    }

    if (!ARMSX2DeviceSupportsHaptics()) {
        if (!s_loggedNativeGamepadRumbleUnavailable) {
            Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptics unavailable on this hardware");
            s_loggedNativeGamepadRumbleUnavailable = true;
        }
        return false;
    }

    if (!s_nativeHapticEngine) {
        // alloc/init, so the static owns it outright and releases on teardown. The
        // controller path next door uses a factory method and has to retain instead.
        NSError* create_error = nil;
        s_nativeHapticEngine = [[CHHapticEngine alloc] initAndReturnError:&create_error];
        if (!s_nativeHapticEngine) {
            if (!s_loggedNativeGamepadRumbleUnavailable) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptic engine creation failed: %s",
                    create_error.localizedDescription.UTF8String ?: "unknown");
                s_loggedNativeGamepadRumbleUnavailable = true;
            }
            return false;
        }

        s_nativeHapticEngine.playsHapticsOnly = YES;
        s_nativeHapticEngine.autoShutdownEnabled = YES;
        // The engine stops itself when idle and when the app backgrounds, so
        // drop both players and let the next rumble rebuild them. CoreHaptics
        // calls back on its own queue, so the work hops to main, where they live.
        s_nativeHapticEngine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
            dispatch_async(dispatch_get_main_queue(), ^{
                s_nativeHapticEngineRunning = false;
                ARMSX2StopNativeGamepadRumbleOnMain();
                s_nativeAppliedGamepadRumble = 0;
                s_nativeAppliedGamepadRumbleValid = false;
                const int source = s_nativeHapticSourceGamepad.load(std::memory_order_relaxed);
                if (source >= 0 && source < static_cast<int>(ARMSX2_MAX_IOS_GAMEPADS))
                    s_nativeHapticResyncSlotMask.fetch_or(1u << source, std::memory_order_release);
            });
            Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptic engine stopped reason=%ld", static_cast<long>(reason));
        };
        s_nativeHapticEngine.resetHandler = ^{
            dispatch_async(dispatch_get_main_queue(), ^{
                s_nativeHapticEngineRunning = false;
                ARMSX2StopNativeGamepadRumbleOnMain();
                s_nativeAppliedGamepadRumble = 0;
                s_nativeAppliedGamepadRumbleValid = false;
                const int source = s_nativeHapticSourceGamepad.load(std::memory_order_relaxed);
                if (source >= 0 && source < static_cast<int>(ARMSX2_MAX_IOS_GAMEPADS))
                    s_nativeHapticResyncSlotMask.fetch_or(1u << source, std::memory_order_release);
            });
            Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptic engine reset");
        };
    }

    if (s_nativeHapticEngineRunning)
        return true;

    NSError* error = nil;
    if (![s_nativeHapticEngine startAndReturnError:&error]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic engine start failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        return false;
    }

    s_nativeHapticEngineRunning = true;
    return true;
}

// Each channel loops one continuous event at full intensity and its own sharpness,
// and the live parameter does the rest. IntensityControl multiplies the base, so a
// base under 1.0 would become a ceiling.
static bool ARMSX2EnsureRumbleChannelOnMain(u32 channel)
{
    if (s_nativeHapticPlayer[channel])
        return true;

    NSError* error = nil;
    NSArray<CHHapticEventParameter*>* params = @[
        [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f] autorelease],
        [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                       value:ARMSX2_RUMBLE_CHANNEL_SHARPNESS[channel]] autorelease]
    ];
    CHHapticEvent* event = [[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                          parameters:params
                                                        relativeTime:0.0
                                                             duration:1.0] autorelease];
    // A looping event has a full 1.0 base because IntensityControl multiplies it.
    // Its initial control value must be zero, however, or starting the player emits
    // a full-power click before the requested envelope reaches Core Haptics.
    CHHapticDynamicParameter* silentStart = [[[CHHapticDynamicParameter alloc]
        initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                      value:0.0f
               relativeTime:0.0] autorelease];
    CHHapticPattern* pattern = [[[CHHapticPattern alloc] initWithEvents:@[event]
                                                                  parameters:@[silentStart]
                                                                       error:&error] autorelease];
    if (!pattern) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pattern failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        return false;
    }

    s_nativeHapticPlayer[channel] = [[s_nativeHapticEngine createAdvancedPlayerWithPattern:pattern error:&error] retain];
    if (!s_nativeHapticPlayer[channel]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic player failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        return false;
    }

    s_nativeHapticPlayer[channel].loopEnabled = YES;
    s_nativeHapticPlayer[channel].loopEnd = 1.0;
    if (![s_nativeHapticPlayer[channel] startAtTime:CHHapticTimeImmediate error:&error]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptic player start failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        [s_nativeHapticPlayer[channel] release];
        s_nativeHapticPlayer[channel] = nil;
        return false;
    }

    s_nativeHapticStopGeneration[channel]++;
    s_nativeHapticEnvelope[channel] = {};

    if (!s_loggedNativeGamepadRumbleReady) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Device rumble active (continuous haptics)");
        s_loggedNativeGamepadRumbleReady = true;
    }

    return true;
}

static double ARMSX2HapticUptime()
{
    return NSProcessInfo.processInfo.systemUptime;
}

static float ARMSX2CurrentRumbleLevel(u32 channel, double now)
{
    const ARMSX2RumbleEnvelope& envelope = s_nativeHapticEnvelope[channel];
    if (envelope.duration <= 0.0)
        return envelope.target_level;

    const double elapsed = now - envelope.start_time;
    if (elapsed <= envelope.hold_duration)
        return envelope.start_level;

    const double fade_duration = envelope.duration - envelope.hold_duration;
    if (fade_duration <= 0.0)
        return envelope.target_level;

    const double progress = std::clamp(
        (elapsed - envelope.hold_duration) / fade_duration, 0.0, 1.0);
    return std::clamp(envelope.start_level +
        ((envelope.target_level - envelope.start_level) * static_cast<float>(progress)), 0.0f, 1.0f);
}

static double ARMSX2RumbleTransitionDuration(u32 channel, float current, float target)
{
    double base_duration;
    if (target <= 0.001f) {
        base_duration = (channel == ARMSX2_RUMBLE_CHANNEL_LARGE)
            ? ARMSX2_RUMBLE_LARGE_RELEASE_SECONDS
            : ARMSX2_RUMBLE_SMALL_RELEASE_SECONDS;
    } else if (current <= 0.001f) {
        base_duration = (channel == ARMSX2_RUMBLE_CHANNEL_LARGE)
            ? ARMSX2_RUMBLE_LARGE_ATTACK_SECONDS
            : ARMSX2_RUMBLE_SMALL_ATTACK_SECONDS;
    } else {
        base_duration = ARMSX2_RUMBLE_CROSSFADE_SECONDS;
    }

    // Small corrections should settle faster than a full stop-to-maximum change.
    const double distance_scale = 0.55 + (0.45 * std::abs(target - current));
    return base_duration * distance_scale;
}

// Expects the engine to be up already. Extended releases hold their current force,
// then fade over the normal duration, so the extra length stays felt instead of
// stretching one weak fade across the whole envelope.
static bool ARMSX2SetRumbleChannelOnMain(u32 channel, float level, float release_scale)
{
    if (channel >= ARMSX2_RUMBLE_CHANNEL_COUNT)
        return false;

    const float target = std::clamp(level, 0.0f, 1.0f);
    if (target > 0.001f)
        s_nativeHapticReleaseScale[channel] = std::clamp(release_scale, 1.0f, 3.0f);
    if (target > 0.001f && !ARMSX2EnsureRumbleChannelOnMain(channel))
        return false;

    if (!s_nativeHapticPlayer[channel]) {
        s_nativeHapticEnvelope[channel] = {};
        return true;
    }

    const double now = ARMSX2HapticUptime();
    const float current = ARMSX2CurrentRumbleLevel(channel, now);
    if (std::abs(current - target) < 0.002f &&
        std::abs(s_nativeHapticEnvelope[channel].target_level - target) < 0.002f)
        return true;

    const double fade_duration = ARMSX2RumbleTransitionDuration(channel, current, target);
    const double hold_duration = (target <= 0.001f)
        ? fade_duration * (static_cast<double>(s_nativeHapticReleaseScale[channel]) - 1.0)
        : 0.0;
    const double duration = hold_duration + fade_duration;
    NSError* error = nil;
    NSMutableArray<CHHapticParameterCurveControlPoint*>* controlPoints = [NSMutableArray arrayWithObject:
        [[[CHHapticParameterCurveControlPoint alloc] initWithRelativeTime:0.0 value:current] autorelease]];
    if (hold_duration > 0.0005) {
        [controlPoints addObject:[[[CHHapticParameterCurveControlPoint alloc]
            initWithRelativeTime:hold_duration value:current] autorelease]];
    }
    [controlPoints addObject:[[[CHHapticParameterCurveControlPoint alloc]
        initWithRelativeTime:duration value:target] autorelease]];
    CHHapticParameterCurve* curve = [[[CHHapticParameterCurve alloc]
        initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
              controlPoints:controlPoints
               relativeTime:0.0] autorelease];

    if (![s_nativeHapticPlayer[channel] scheduleParameterCurve:curve atTime:CHHapticTimeImmediate error:&error]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptic curve failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        ARMSX2StopRumbleChannelOnMain(channel);
        return false;
    }

    s_nativeHapticEnvelope[channel] = { current, target, now, hold_duration, duration };
    const u32 generation = ++s_nativeHapticStopGeneration[channel];
    if (target <= 0.001f) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                           static_cast<int64_t>((duration + 0.015) * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
                if (s_nativeHapticStopGeneration[channel] == generation)
                    ARMSX2StopRumbleChannelOnMain(channel);
            });
    }

    return true;
}

// Short transients give binary small-motor changes and fast game pulses a crisp
// onset. The sustained players then carry the weight and texture without a busy
// timer or a stream of individually allocated pulse patterns.
static void ARMSX2PlayRumbleTransientOnMain(float large, float small)
{
    const double now = ARMSX2HapticUptime();
    if ((now - s_nativeHapticLastTransientTime) < 0.025)
        return;

    const float combined = std::max(large * 0.58f, small * 0.82f);
    if (combined <= 0.02f)
        return;

    const float weight = large + small;
    const float sharpness = (weight > 0.001f)
        ? std::clamp(((large * 0.18f) + (small * 0.92f)) / weight, 0.0f, 1.0f)
        : 0.5f;
    const float intensity = std::clamp(combined, 0.08f, 0.82f);
    NSError* error = nil;
    NSArray<CHHapticEventParameter*>* params = @[
        [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                       value:intensity] autorelease],
        [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                       value:sharpness] autorelease]
    ];
    CHHapticEvent* event = [[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient
                                                          parameters:params
                                                        relativeTime:0.0] autorelease];
    CHHapticPattern* pattern = [[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error] autorelease];
    if (!pattern)
        return;

    id<CHHapticPatternPlayer> player = [s_nativeHapticEngine createPlayerWithPattern:pattern error:&error];
    if (player && [player startAtTime:CHHapticTimeImmediate error:&error])
        s_nativeHapticLastTransientTime = now;
}

static float ARMSX2PhoneRumbleGain(float setting)
{
    const float clamped = std::clamp(setting, 0.0f, 1.0f);
    if (clamped <= ARMSX2_PHONE_RUMBLE_BASELINE_SETTING)
        return clamped / ARMSX2_PHONE_RUMBLE_BASELINE_SETTING;

    // The original synthesis becomes the 25% baseline. The upper three quarters
    // of the slider add gain linearly until 100% reaches three times that baseline.
    const float normalized = (clamped - ARMSX2_PHONE_RUMBLE_BASELINE_SETTING) /
        (1.0f - ARMSX2_PHONE_RUMBLE_BASELINE_SETTING);
    return 1.0f + (normalized * (ARMSX2_PHONE_RUMBLE_MAX_GAIN - 1.0f));
}

// Per channel. The small motor is on or off with nothing between, so classifying off both at once
// let any buzz pin the heavy motor to Hard and give it a tail it never asked for.
static ARMSX2PhoneRumbleClass ARMSX2ClassifyPhoneRumble(float level)
{
    if (level >= ARMSX2_PHONE_RUMBLE_HARD_THRESHOLD)
        return ARMSX2PhoneRumbleClass::Hard;
    if (level >= ARMSX2_PHONE_RUMBLE_MEDIUM_THRESHOLD)
        return ARMSX2PhoneRumbleClass::Medium;
    return ARMSX2PhoneRumbleClass::Weak;
}

static float ARMSX2PhoneRumbleReleaseScale(float setting, ARMSX2PhoneRumbleClass rumble_class)
{
    const float clamped = std::clamp(setting, ARMSX2_PHONE_RUMBLE_BASELINE_SETTING, 1.0f);
    switch (rumble_class) {
        case ARMSX2PhoneRumbleClass::Hard:
            // Hard effects begin gaining inertia above 50%, reach 2x at 75%,
            // and finish at a 3x release envelope at maximum strength.
            return (clamped > 0.50f) ? (1.0f + ((clamped - 0.50f) * 4.0f)) : 1.0f;
        case ARMSX2PhoneRumbleClass::Medium:
            // Medium effects retain their original timing through 75%, then
            // smoothly grow to a 2x release envelope.
            return (clamped > 0.75f) ? (1.0f + ((clamped - 0.75f) * 4.0f)) : 1.0f;
        case ARMSX2PhoneRumbleClass::Weak:
            return 1.0f;
    }

    return 1.0f;
}

static float ARMSX2PromoteWeakPhoneRumble(float level, float setting, ARMSX2PhoneRumbleClass rumble_class)
{
    if (rumble_class != ARMSX2PhoneRumbleClass::Weak || setting <= 0.75f)
        return level;

    // The final quarter progressively turns subtle feedback into a normal hard
    // tap. Its release scale remains 1x, so promotion adds presence, not a long tail.
    const float promotion = std::clamp((setting - 0.75f) * 4.0f, 0.0f, 1.0f);
    const float hard_level = std::pow(ARMSX2_PHONE_RUMBLE_HARD_THRESHOLD, 0.78f);
    const float promoted_level = std::max(level, hard_level);
    return level + ((promoted_level - level) * promotion);
}

void ARMSX2ApplyNativeGamepadRumbleOnMain(u32 packed)
{
	if (s_nativeAppliedGamepadRumbleValid && packed == s_nativeAppliedGamepadRumble)
		return;

    // Straight off the packed value, so the phone gets the whole 0..1 motor range
    // rather than the 0x7000 ceiling the controller motors are held to. Read every
    // time so dragging the slider is felt on the next rumble instead of next launch.
    const float strength_setting = s_settings_interface
        ? Host::GetBaseFloatSettingValue(
              "ARMSX2iOS/UI", "PhoneRumbleStrength", ARMSX2_PHONE_RUMBLE_BASELINE_SETTING)
        : ARMSX2_PHONE_RUMBLE_BASELINE_SETTING;
    const bool increase_duration = s_settings_interface
        ? Host::GetBaseBoolSettingValue(
              "ARMSX2iOS/UI", "IncreaseRumbleDurationAndInterpolation", true)
        : true;
    const float strength = ARMSX2PhoneRumbleGain(strength_setting);
    // Sublinear curve for the actuator's weak low end. Ordering holds at the 25% baseline; above
    // that the gain pushes the top of the range into the clamp, trading ordering for force.
    const float large_raw = ARMSX2RumbleLargeIntensity(packed);
    float large = (large_raw > 0.01f)
        ? std::pow(large_raw, 0.78f) * strength
        : 0.0f;
    const float small_raw = ARMSX2RumbleSmallIntensity(packed);
    const ARMSX2PhoneRumbleClass large_class = ARMSX2ClassifyPhoneRumble(large_raw);
    const ARMSX2PhoneRumbleClass small_class = ARMSX2ClassifyPhoneRumble(small_raw);
    const float large_release = increase_duration
        ? ARMSX2PhoneRumbleReleaseScale(strength_setting, large_class)
        : 1.0f;
    const float small_release = increase_duration
        ? ARMSX2PhoneRumbleReleaseScale(strength_setting, small_class)
        : 1.0f;
    large = (increase_duration && large_raw > 0.01f)
        ? ARMSX2PromoteWeakPhoneRumble(large, strength_setting, large_class)
        : large;
    float small = (small_raw > 0.01f) ? (ARMSX2_SMALL_MOTOR_LEVEL * strength) : 0.0f;
    small = (increase_duration && small_raw > 0.01f)
        ? ARMSX2PromoteWeakPhoneRumble(small, strength_setting, small_class)
        : small;

    // Turning the extension off also cancels a scale retained by the previous
    // active effect, so its next stop uses the original 1x release immediately.
    if (!increase_duration) {
        for (u32 channel = 0; channel < ARMSX2_RUMBLE_CHANNEL_COUNT; channel++)
            s_nativeHapticReleaseScale[channel] = 1.0f;
    }

    if ((large > 0.01f || small > 0.01f) && !ARMSX2EnsureNativeRumbleEngineOnMain())
        return;

    const float previous_large = s_nativeAppliedGamepadRumbleValid
        ? ARMSX2RumbleLargeIntensity(s_nativeAppliedGamepadRumble)
        : 0.0f;
    const float previous_small = s_nativeAppliedGamepadRumbleValid
        ? ARMSX2RumbleSmallIntensity(s_nativeAppliedGamepadRumble)
        : 0.0f;
    const bool large_started = large_raw > 0.01f && previous_large <= 0.01f;
    const bool small_started = small_raw > 0.01f && previous_small <= 0.01f;
    if (large_started || small_started)
        ARMSX2PlayRumbleTransientOnMain(large_started ? large : 0.0f, small_started ? small : 0.0f);

    const bool large_ok = ARMSX2SetRumbleChannelOnMain(
        ARMSX2_RUMBLE_CHANNEL_LARGE, large, large_release);
    const bool small_ok = ARMSX2SetRumbleChannelOnMain(
        ARMSX2_RUMBLE_CHANNEL_SMALL, small, small_release);
    if (large_ok && small_ok) {
        s_nativeAppliedGamepadRumble = packed;
        s_nativeAppliedGamepadRumbleValid = true;
	}
}
