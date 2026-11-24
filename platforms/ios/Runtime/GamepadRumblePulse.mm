// GamepadRumblePulse.mm — controller rumble pulse, Joy-Con and the rumble test
// SPDX-License-Identifier: GPL-3.0+

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>

#include <algorithm>
#include <atomic>

#include <SDL3/SDL.h>

#include "common/Console.h"
#include "pcsx2/Host.h"

#include "IOSRuntime.h"
#import "ARMSX2Bridge.h"
#import "GamepadInternal.h"

static float s_appliedGamepadRumbleStrength[ARMSX2_MAX_IOS_GAMEPADS] = {};
static bool s_loggedGamepadRumbleFailure = false;
static bool s_loggedSDLGamepadRumble = false;
static bool s_loggedSDLGamepadRumbleForceStop = false;
static constexpr u32 ARMSX2_GAMEPAD_RUMBLE_DURATION_MS = 220;
static constexpr double ARMSX2_GAMEPAD_RUMBLE_FORCE_STOP_SECONDS = 0.30;
static constexpr u32 ARMSX2_GAMEPAD_RUMBLE_TEST_DURATION_MS = 1000;
static constexpr double ARMSX2_GAMEPAD_RUMBLE_TEST_DURATION_SECONDS = 1.0;
static CHHapticEngine* s_nativePulseHapticEngine[ARMSX2_MAX_IOS_GAMEPADS] = {};
static std::atomic<u32> s_nativePulseHapticStopGeneration[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<u32> s_loggedNativePulseHapticEvents{0};
static std::atomic<u32> s_loggedJoyConRumbleSkipped{0};
// Cached, since the rumble pulse runs on the CPU thread and must not take the settings lock.
static std::atomic<bool> s_hapticFeedbackEnabled{true};

void ARMSX2SetHapticFeedbackEnabled(bool enabled)
{
    s_hapticFeedbackEnabled.store(enabled, std::memory_order_relaxed);
}

void ARMSX2StopNativeGamepadRumblePulseOnMain(u32 slot)
{
    if (slot >= ARMSX2_MAX_IOS_GAMEPADS)
        return;

    s_nativePulseHapticStopGeneration[slot].fetch_add(1, std::memory_order_relaxed);
    if (s_nativePulseHapticEngine[slot]) {
        @try {
            [s_nativePulseHapticEngine[slot] stopWithCompletionHandler:nil];
        } @catch (NSException* exception) {
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic stop exception slot=%u name=%s reason=%s",
                slot + 1,
                exception.name.UTF8String ?: "unknown",
                exception.reason.UTF8String ?: "unknown");
        }
        s_nativePulseHapticEngine[slot] = nil;
    }
}

// Only the controller in this slot. Falling back to another controller would hand
// player 2's rumble to player 1, and one Joy-Con could kill rumble for every slot.
static GCController* ARMSX2FindNativeHapticControllerForSlot(u32 slot)
{
    NSArray<GCController*>* controllers = [GCController controllers];
    if (slot >= controllers.count)
        return nil;

    GCController* controller = controllers[slot];
    return controller.haptics ? controller : nil;
}

static GCController* ARMSX2FindNativeControllerForSlot(u32 slot)
{
    NSArray<GCController*>* controllers = [GCController controllers];
    return (slot < controllers.count) ? controllers[slot] : nil;
}

static bool ARMSX2NativeControllerLooksLikeJoyCon(GCController* controller)
{
    if (!controller)
        return false;

    NSString* vendor = controller.vendorName ?: @"";
    NSString* category = @"";
    if ([controller respondsToSelector:@selector(productCategory)])
        category = controller.productCategory ?: @"";

    NSString* descriptor = [[NSString stringWithFormat:@"%@ %@", vendor, category] lowercaseString];
    return [descriptor containsString:@"joy-con"] ||
           [descriptor containsString:@"joycon"] ||
           [descriptor containsString:@"joy con"];
}

static bool ARMSX2CStringLooksLikeJoyCon(const char* value)
{
    if (!value || !*value)
        return false;

    NSString* descriptor = [NSString stringWithUTF8String:value];
    if (!descriptor)
        return false;

    descriptor = descriptor.lowercaseString;
    return [descriptor containsString:@"joy-con"] ||
           [descriptor containsString:@"joycon"] ||
           [descriptor containsString:@"joy con"];
}

bool ARMSX2SDLGamepadLooksLikeJoyCon(SDL_Gamepad* gamepad)
{
    return gamepad && ARMSX2CStringLooksLikeJoyCon(SDL_GetGamepadName(gamepad));
}

static bool ARMSX2NativeControllerSlotLooksLikeJoyCon(u32 slot)
{
    return ARMSX2NativeControllerLooksLikeJoyCon(ARMSX2FindNativeControllerForSlot(slot));
}

static bool ARMSX2GamepadSlotLooksLikeJoyCon(u32 slot)
{
    if (ARMSX2NativeControllerSlotLooksLikeJoyCon(slot))
        return true;

    // Cached rather than read off s_gamepads, because this gets asked from the
    // main queue and the pad it would be naming can be closed at any moment.
    return slot < ARMSX2_MAX_IOS_GAMEPADS && s_gamepadIsJoyCon[slot].load(std::memory_order_relaxed);
}

static bool ARMSX2ApplyNativeGamepadRumblePulseOnMain(
    u32 slot, u32 packed, const char* reason, double duration_seconds = 0.18)
{
    if (slot >= ARMSX2_MAX_IOS_GAMEPADS)
        return false;

    if (@available(iOS 14.0, *)) {
    } else {
        return false;
    }

    if (ARMSX2GamepadSlotLooksLikeJoyCon(slot)) {
        const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 16)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con native rumble hard-disabled slot=%u reason=%s",
                slot + 1, reason ? reason : "unknown");
        return false;
    }

    const float large = ARMSX2RumbleLargeIntensity(packed);
    const float small = ARMSX2RumbleSmallIntensity(packed);
    const float raw_intensity = std::max(large, small);
    if (raw_intensity <= 0.01f) {
        ARMSX2StopNativeGamepadRumblePulseOnMain(slot);
        return true;
    }

    GCController* controller = ARMSX2FindNativeHapticControllerForSlot(slot);
    if (ARMSX2NativeControllerLooksLikeJoyCon(controller)) {
        const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 16) {
            NSString* vendor = controller.vendorName ?: @"unknown";
            NSString* product = @"";
            if ([controller respondsToSelector:@selector(productCategory)])
                product = controller.productCategory ?: @"";
            Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con native rumble skipped slot=%u controller=%s category=%s reason=%s",
                slot + 1, vendor.UTF8String, product.UTF8String, reason ? reason : "unknown");
        }
        return false;
    }

    if (!controller || !controller.haptics) {
        const u32 log_index = s_loggedNativePulseHapticEvents.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 16) {
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse unavailable slot=%u reason=%s controllers=%u",
                slot + 1, reason ? reason : "unknown", static_cast<unsigned>([GCController controllers].count));
        }
        return false;
    }

    ARMSX2StopNativeGamepadRumblePulseOnMain(slot);

    GCHapticsLocality locality = GCHapticsLocalityDefault;
    NSSet<GCHapticsLocality>* localities = controller.haptics.supportedLocalities;
    if ([localities containsObject:GCHapticsLocalityAll])
        locality = GCHapticsLocalityAll;

    CHHapticEngine* engine = [controller.haptics createEngineWithLocality:locality];
    if (!engine) {
        const u32 log_index = s_loggedNativePulseHapticEvents.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 16) {
            NSString* vendor = controller.vendorName ?: @"unknown";
            Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse engine failed slot=%u controller=%s locality=%s reason=%s",
                slot + 1, vendor.UTF8String, locality.UTF8String, reason ? reason : "unknown");
        }
        return false;
    }

    engine.playsHapticsOnly = YES;
    engine.autoShutdownEnabled = YES;

    NSError* error = nil;
    if (![engine startAndReturnError:&error]) {
        NSString* vendor = controller.vendorName ?: @"unknown";
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse start failed slot=%u controller=%s: %s",
            slot + 1, vendor.UTF8String, error.localizedDescription.UTF8String ?: "unknown");
        return false;
    }

    const float intensity = std::clamp(raw_intensity, 0.10f, 0.55f);
    const float sharpness = std::clamp(0.25f + (small * 0.45f), 0.20f, 0.65f);
    NSArray<CHHapticEventParameter*>* params = @[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness]
    ];
    CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                         parameters:params
                                                       relativeTime:0.0
                                                            duration:duration_seconds];
    CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
    if (!pattern) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse pattern failed slot=%u: %s",
            slot + 1, error.localizedDescription.UTF8String ?: "unknown");
        [engine stopWithCompletionHandler:nil];
        return false;
    }

    id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&error];
    if (!player || ![player startAtTime:CHHapticTimeImmediate error:&error]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse player failed slot=%u: %s",
            slot + 1, error.localizedDescription.UTF8String ?: "unknown");
        [engine stopWithCompletionHandler:nil];
        return false;
    }

    s_nativePulseHapticEngine[slot] = engine;
    const u32 stop_generation = s_nativePulseHapticStopGeneration[slot].fetch_add(1, std::memory_order_relaxed) + 1;
    const u32 log_index = s_loggedNativePulseHapticEvents.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 16) {
        NSString* vendor = controller.vendorName ?: @"unknown";
        NSString* product = @"";
        if ([controller respondsToSelector:@selector(productCategory)])
            product = controller.productCategory ?: @"";
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic pulse accepted slot=%u controller=%s category=%s locality=%s reason=%s intensity=%.2f",
            slot + 1, vendor.UTF8String, product.UTF8String, locality.UTF8String, reason ? reason : "unknown", intensity);
    }

    const double force_stop_seconds = std::max(
        ARMSX2_GAMEPAD_RUMBLE_FORCE_STOP_SECONDS, duration_seconds);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(force_stop_seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (slot >= ARMSX2_MAX_IOS_GAMEPADS ||
            s_nativePulseHapticStopGeneration[slot].load(std::memory_order_relaxed) != stop_generation)
            return;

        if (s_nativePulseHapticEngine[slot]) {
            @try {
                [s_nativePulseHapticEngine[slot] stopWithCompletionHandler:nil];
            } @catch (NSException* exception) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic delayed stop exception slot=%u name=%s reason=%s",
                    slot + 1,
                    exception.name.UTF8String ?: "unknown",
                    exception.reason.UTF8String ?: "unknown");
            }
            s_nativePulseHapticEngine[slot] = nil;
        }
    });

    return true;
}

static bool ARMSX2ApplyNativeGamepadRumblePulseForJoyConOnMain(u32 slot, u32 packed, const char* reason)
{
    GCController* controller = ARMSX2FindNativeControllerForSlot(slot);
    if (!ARMSX2NativeControllerLooksLikeJoyCon(controller))
        return false;

    const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 16) {
        NSString* vendor = controller.vendorName ?: @"unknown";
        NSString* product = @"";
        if ([controller respondsToSelector:@selector(productCategory)])
            product = controller.productCategory ?: @"";
        Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con rumble skipped slot=%u controller=%s category=%s reason=%s",
            slot + 1, vendor.UTF8String, product.UTF8String, reason ? reason : "unknown");
    }
    return false;
}

void ARMSX2ApplyPendingGamepadRumble(unsigned int gamepad_index)
{
    if (gamepad_index >= ARMSX2_MAX_IOS_GAMEPADS)
        return;

    // Ahead of the unchanged-value bail below, so a held rumble still gets
    // its stop. The pump runs every frame, so a deadline is enough and no
    // pad pointer is kept.
    const u64 stop_deadline = s_gamepadRumbleStopDeadlineMs[gamepad_index].load(std::memory_order_relaxed);
    if (stop_deadline != 0 && SDL_GetTicks() >= stop_deadline) {
        s_gamepadRumbleStopDeadlineMs[gamepad_index].store(0, std::memory_order_relaxed);
        if (s_gamepads[gamepad_index]) {
            SDL_RumbleGamepad(s_gamepads[gamepad_index], 0, 0, 0);
            SDL_RumbleGamepadTriggers(s_gamepads[gamepad_index], 0, 0, 0);
        }
        if (!s_loggedSDLGamepadRumbleForceStop) {
            Console.WriteLn("[ARMSX2 iOS Gamepad] SDL controller rumble force-stopped");
            s_loggedSDLGamepadRumbleForceStop = true;
        }
    }

    const u32 packed = s_pendingGamepadRumble[gamepad_index].load(std::memory_order_relaxed);
    const float game_rumble_strength = ARMSX2GameRumbleStrength();
    const u32 native_packed = ARMSX2ScaleNativeRumble(packed, game_rumble_strength);
    const u32 slot_bit = 1u << gamepad_index;
    const bool native_resync =
        (s_nativeHapticResyncSlotMask.fetch_and(~slot_bit, std::memory_order_acq_rel) & slot_bit) != 0;
    if (s_appliedGamepadRumbleValid[gamepad_index] &&
        packed == s_appliedGamepadRumble[gamepad_index] &&
        game_rumble_strength == s_appliedGamepadRumbleStrength[gamepad_index] && !native_resync)
        return;

    const u16 large = ARMSX2ScaleControllerRumbleMotor(
        static_cast<u16>((packed >> 16) & 0xffffu), game_rumble_strength);
    const u16 small = ARMSX2ScaleControllerRumbleMotor(
        static_cast<u16>(packed & 0xffffu), game_rumble_strength);
    const bool wants_rumble = (large != 0 || small != 0);

    if (!wants_rumble) {
        // The zero goes out through the normal path below, so the deadline has
        // nothing left to catch.
        s_gamepadRumbleStopDeadlineMs[gamepad_index].store(0, std::memory_order_relaxed);
        const u32 slot = gamepad_index;
        const u32 native_packed_stop = native_packed;
        dispatch_async(dispatch_get_main_queue(), ^{
            ARMSX2StopNativeGamepadRumblePulseOnMain(slot);
            // The device engine loops until it is told otherwise, so the zero has to
            // reach it or the phone keeps buzzing after the game has stopped asking.
            ARMSX2ApplyNativeGamepadRumbleOnMain(native_packed_stop);
        });
    }

    if (wants_rumble && ARMSX2NativeControllerSlotLooksLikeJoyCon(gamepad_index)) {
        const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 16)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con rumble request ignored safely slot=%u", gamepad_index + 1);
        s_appliedGamepadRumble[gamepad_index] = packed;
        s_appliedGamepadRumbleStrength[gamepad_index] = game_rumble_strength;
        s_appliedGamepadRumbleValid[gamepad_index] = true;
        return;
    }

    if (wants_rumble && ARMSX2GamepadSlotLooksLikeJoyCon(gamepad_index)) {
        const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
        if (log_index < 16)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Joy-Con SDL rumble request ignored safely slot=%u name=%s",
                gamepad_index + 1,
                s_gamepads[gamepad_index] ? (SDL_GetGamepadName(s_gamepads[gamepad_index]) ?: "unknown") : "unknown");
        s_appliedGamepadRumble[gamepad_index] = packed;
        s_appliedGamepadRumbleStrength[gamepad_index] = game_rumble_strength;
        s_appliedGamepadRumbleValid[gamepad_index] = true;
        return;
    }

    if (s_gamepads[gamepad_index]) {
        if (SDL_RumbleGamepad(s_gamepads[gamepad_index], large, small, ARMSX2_GAMEPAD_RUMBLE_DURATION_MS)) {
            if (!s_loggedSDLGamepadRumble) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] SDL controller rumble accepted");
                s_loggedSDLGamepadRumble = true;
            }
            if (wants_rumble) {
                const u32 slot = gamepad_index;
                const u32 native_packed_for_slot = native_packed;
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ApplyNativeGamepadRumblePulseForJoyConOnMain(slot, native_packed_for_slot, "joycon-sdl-mirror");
                });
            }
            if (wants_rumble) {
                // Each new value pushes the deadline out, so a held rumble keeps
                // going and a game that goes quiet gets stopped. The CoreHaptics
                // pulse stops itself.
                s_gamepadRumbleStopDeadlineMs[gamepad_index].store(
                    SDL_GetTicks() + static_cast<u64>(ARMSX2_GAMEPAD_RUMBLE_FORCE_STOP_SECONDS * 1000.0),
                    std::memory_order_relaxed);
            }
        } else {
            if (!s_loggedGamepadRumbleFailure) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] SDL controller %u rumble unavailable: %s", gamepad_index + 1, SDL_GetError());
                s_loggedGamepadRumbleFailure = true;
            }
            if (wants_rumble) {
                const u32 slot = gamepad_index;
                const u32 native_packed_for_slot = native_packed;
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ApplyNativeGamepadRumblePulseOnMain(slot, native_packed_for_slot, "sdl-fallback");
                });
            }
        }
    } else if (wants_rumble) {
        const u32 slot = gamepad_index;
        const u32 native_packed_for_slot = native_packed;
        dispatch_async(dispatch_get_main_queue(), ^{
            ARMSX2ApplyNativeGamepadRumblePulseOnMain(slot, native_packed_for_slot, "no-sdl-gamepad");
        });
        // No SDL gamepad and no haptic controller: rumble the phone itself, as on
        // handheld grips (e.g. Kishi 3). The engine tracks intensity; the one-shot
        // tap covers an iPad or a phone with no taptic engine.
        if (!ARMSX2FindNativeHapticController()) {
            if (ARMSX2DeviceSupportsHaptics()) {
                const u32 native_packed_device = s_hapticFeedbackEnabled.load(std::memory_order_relaxed) ? native_packed : 0;
                s_nativeHapticSourceGamepad.store(static_cast<int>(gamepad_index), std::memory_order_relaxed);
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ApplyNativeGamepadRumbleOnMain(native_packed_device);
                });
            } else {
                // Unclamped on purpose. 0x7000 is the ceiling the controller motors are
                // held to, and the tap on the other side divides by the full 16 bit
                // range, so passing the clamped pair capped this path at 44 percent.
                [ARMSX2Bridge triggerDeviceHapticLarge:((native_packed >> 16) & 0xffffu) small:(native_packed & 0xffffu)];
            }
        }
    }

    s_appliedGamepadRumble[gamepad_index] = packed;
    s_appliedGamepadRumbleStrength[gamepad_index] = game_rumble_strength;
    s_appliedGamepadRumbleValid[gamepad_index] = true;
}

void ARMSX2ServiceGamepadRumbleTest()
{
    Console.WriteLn("[ARMSX2 iOS Gamepad] Test controller rumble requested");

    SDL_PumpEvents();
    SDL_UpdateGamepads();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL detected=%d", count);
    if (ids) {
        for (int id_index = 0; id_index < count; id_index++) {
            bool already_open = false;
            for (SDL_Gamepad* gamepad : s_gamepads) {
                if (gamepad && SDL_GetGamepadID(gamepad) == ids[id_index]) {
                    already_open = true;
                    break;
                }
            }
            if (already_open)
                continue;

            for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
                if (!s_gamepads[slot]) {
                    s_gamepads[slot] = SDL_OpenGamepad(ids[id_index]);
                    if (s_gamepads[slot])
                        s_gamepadIsJoyCon[slot].store(ARMSX2SDLGamepadLooksLikeJoyCon(s_gamepads[slot]), std::memory_order_relaxed);
                    Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL open slot=%u id=%d result=%s",
                        slot + 1, ids[id_index],
                        s_gamepads[slot] ? (SDL_GetGamepadName(s_gamepads[slot]) ?: "unknown") : SDL_GetError());
                    break;
                }
            }
        }
        SDL_free(ids);
    }

    bool anySDLGamepad = false;
    bool anyNativeFallback = false;
    const float game_rumble_strength = ARMSX2GameRumbleStrength();
    const u32 test_packed = ARMSX2PackGamepadRumble(
        0.55f * game_rumble_strength, 0.55f * game_rumble_strength);
    const u16 test_motor_strength = ARMSX2ScaleControllerRumbleMotor(
        ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY, game_rumble_strength);
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        if (!s_gamepads[slot])
            continue;

        if (ARMSX2GamepadSlotLooksLikeJoyCon(slot)) {
            const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
            if (log_index < 16)
                Console.WriteLn("[ARMSX2 iOS Gamepad] Test Joy-Con rumble hard-disabled slot=%u name=%s",
                    slot + 1, SDL_GetGamepadName(s_gamepads[slot]) ?: "unknown");
            continue;
        }

        anySDLGamepad = true;
        const bool ok = SDL_RumbleGamepad(s_gamepads[slot], test_motor_strength, test_motor_strength,
            ARMSX2_GAMEPAD_RUMBLE_TEST_DURATION_MS);
        Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL controller %u rumble %s%s%s",
            slot + 1, ok ? "accepted" : "failed", ok ? "" : ": ", ok ? "" : SDL_GetError());
        if (!ok) {
            const u32 native_slot = slot;
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2ApplyNativeGamepadRumblePulseOnMain(native_slot, test_packed,
                    "test-sdl-fallback", ARMSX2_GAMEPAD_RUMBLE_TEST_DURATION_SECONDS);
            });
            anyNativeFallback = true;
        }
    }
    if (!anySDLGamepad) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL rumble skipped: no SDL gamepad open");
        for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
            if (ARMSX2NativeControllerSlotLooksLikeJoyCon(slot)) {
                const u32 log_index = s_loggedJoyConRumbleSkipped.fetch_add(1, std::memory_order_relaxed);
                if (log_index < 16)
                    Console.WriteLn("[ARMSX2 iOS Gamepad] Test native Joy-Con rumble skipped slot=%u", slot + 1);
                continue;
            }
            const u32 native_slot = slot;
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2ApplyNativeGamepadRumblePulseOnMain(native_slot, test_packed,
                    "test-no-sdl-gamepad", ARMSX2_GAMEPAD_RUMBLE_TEST_DURATION_SECONDS);
            });
        }

        // The pulse path above needs a real controller, so this plays the phone's
        // own rumble from settings: step the heavy motor up, then buzz the small
        // one, to check both channels and the strength slider.
        if (!ARMSX2FindNativeHapticController() && ARMSX2DeviceSupportsHaptics()) {
            static constexpr struct { double delay; float large; float small; } ramp[] = {
                { 0.00, 0.20f, 0.0f },
                { 0.25, 0.60f, 0.0f },
                { 0.50, 1.00f, 0.0f },
                { 0.75, 0.00f, 1.0f },
                { ARMSX2_GAMEPAD_RUMBLE_TEST_DURATION_SECONDS, 0.00f, 0.0f },
            };
            for (const auto& step : ramp) {
                const u32 step_packed = ARMSX2PackGamepadRumble(step.large, step.small);
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(step.delay * NSEC_PER_SEC)),
                    dispatch_get_main_queue(), ^{
                        ARMSX2ApplyNativeGamepadRumbleOnMain(step_packed);
                    });
            }
            Console.WriteLn("[ARMSX2 iOS Gamepad] Test device rumble ramp queued");
        }
        anyNativeFallback = true;
    }

    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        s_pendingGamepadRumble[slot].store(0, std::memory_order_relaxed);
        s_appliedGamepadRumble[slot] = 0;
        s_appliedGamepadRumbleValid[slot] = false;
    }

    Console.WriteLn("[ARMSX2 iOS Gamepad] Native CoreHaptics pulse fallback %s", anyNativeFallback ? "queued when needed" : "not queued");

    // The SDL half of the stop rides the same deadline the pump already checks,
    // so nothing off this thread ends up holding a gamepad pointer. The
    // CoreHaptics half has to be on main, and touches no SDL.
    const u64 deadline = SDL_GetTicks() + ARMSX2_GAMEPAD_RUMBLE_TEST_DURATION_MS;
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
        s_gamepadRumbleStopDeadlineMs[slot].store(deadline, std::memory_order_relaxed);

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                       static_cast<int64_t>(ARMSX2_GAMEPAD_RUMBLE_TEST_DURATION_SECONDS * NSEC_PER_SEC)),
        dispatch_get_main_queue(), ^{
        for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
            ARMSX2StopNativeGamepadRumblePulseOnMain(slot);
        Console.WriteLn("[ARMSX2 iOS Gamepad] Test controller rumble stopped");
    });
}

extern "C" void ARMSX2_iOSTestGamepadRumble(void)
{
    // Called from SwiftUI on the main thread. The pump owns s_gamepads and
    // may close a pad while we open one, so hand the work over while it
    // runs. With no VM there is no pump to race or to wait for.
    if (!s_vmThreadActive.load(std::memory_order_relaxed)) {
        ARMSX2ServiceGamepadRumbleTest();
        return;
    }

    s_gamepadRumbleTestRequested.store(true, std::memory_order_relaxed);
    // A paused VM may not be pumping, and settings is reachable from the pause
    // menu, so do not let the button quietly do nothing. If the flag is still
    // sitting there a moment later, nobody is coming for it.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.25 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (s_gamepadRumbleTestRequested.exchange(false, std::memory_order_relaxed))
            ARMSX2ServiceGamepadRumbleTest();
    });
}
