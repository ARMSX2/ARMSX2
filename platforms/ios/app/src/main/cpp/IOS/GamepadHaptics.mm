// GamepadHaptics.mm — iOS gamepad + haptics subsystem.

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <SDL3/SDL.h>

#include "common/Console.h"
#include "pcsx2/Config.h"          // EmuConfig, GSConfig
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"

#include "IOSRuntime.h"
#import "ARMSX2Bridge.h"

#pragma mark - Gamepad state
// Touched only by the CPU thread, from inside the pump. Anything that used to
// reach in here from the main queue now works off the caches below instead, so
// a controller can be closed without a queued block finding freed memory.
SDL_Gamepad* s_gamepads[ARMSX2_MAX_IOS_GAMEPADS] = {};
static std::atomic<u32> s_pendingGamepadRumble[ARMSX2_MAX_IOS_GAMEPADS];
// SDL_GetTicks value after which the slot gets a stop, or 0 for nothing pending.
static std::atomic<u64> s_gamepadRumbleStopDeadlineMs[ARMSX2_MAX_IOS_GAMEPADS];
// Worked out once when the pad is opened, so the name lookup does not have to
// happen on whichever thread is asking.
static std::atomic<bool> s_gamepadIsJoyCon[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<bool> s_gamepadRumbleTestRequested{false};
static u32 s_appliedGamepadRumble[ARMSX2_MAX_IOS_GAMEPADS] = {};
static bool s_appliedGamepadRumbleValid[ARMSX2_MAX_IOS_GAMEPADS] = {};
static bool s_loggedGamepadRumbleFailure = false;
static bool s_loggedSDLGamepadRumble = false;
static bool s_loggedSDLGamepadRumbleForceStop = false;
static std::atomic<u32> s_loggedPadRumbleCommandCount{0};
static std::atomic<u32> s_loggedIgnoredPadRumbleCount{0};
static bool s_loggedMultitapRestartNeeded = false;
static constexpr u32 ARMSX2_GAMEPAD_RUMBLE_DURATION_MS = 220;
static constexpr double ARMSX2_GAMEPAD_RUMBLE_FORCE_STOP_SECONDS = 0.30;
static constexpr u16 ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY = 0x7000;

// The two PS2 motors feel nothing alike, so the phone plays them as two channels
// rather than flattening them into one number. Taptic Engine sharpness runs 80 Hz
// at 0.0 up to 230 Hz at 1.0, hitting hardest around 0.73, so the heavy motor sits
// below that and the buzzer above it.
enum : u32
{
    ARMSX2_RUMBLE_CHANNEL_LARGE = 0,
    ARMSX2_RUMBLE_CHANNEL_SMALL = 1,
    ARMSX2_RUMBLE_CHANNEL_COUNT = 2,
};
static constexpr float ARMSX2_RUMBLE_CHANNEL_SHARPNESS[ARMSX2_RUMBLE_CHANNEL_COUNT] = { 0.35f, 0.85f };
// The PS2 small motor has one speed and no analog control at all, so "on" is just
// a level we pick. Matching the real pad means it does not vary.
static constexpr float ARMSX2_SMALL_MOTOR_LEVEL = 0.55f;
// Down at the bottom of the range the engine barely moves, so a faint rumble reads
// as nothing at all. Lift the floor without touching the top.
static constexpr float ARMSX2_RUMBLE_FLOOR = 0.12f;

static CHHapticEngine* s_nativePulseHapticEngine[ARMSX2_MAX_IOS_GAMEPADS] = {};
static std::atomic<u32> s_nativePulseHapticStopGeneration[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<u32> s_loggedNativePulseHapticEvents{0};
static CHHapticEngine* s_nativeHapticEngine = nil;
static id<CHHapticAdvancedPatternPlayer> s_nativeHapticPlayer[ARMSX2_RUMBLE_CHANNEL_COUNT] = {};
static u32 s_nativeAppliedGamepadRumble = 0;
static bool s_nativeAppliedGamepadRumbleValid = false;
static bool s_loggedNativeGamepadRumbleReady = false;
static bool s_loggedNativeGamepadRumbleUnavailable = false;
static std::atomic<u8> s_nativeGamepadDpadMask[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<u8> s_nativeGamepadDpadLatchedMask[ARMSX2_MAX_IOS_GAMEPADS];
static std::atomic<u8> s_nativeGamepadAnyDpadMask{0};
static std::atomic<u8> s_nativeGamepadAnyDpadLatchedMask{0};
static std::atomic<u32> s_loggedNativeGamepadDpadEvents{0};
static std::atomic<u32> s_loggedNativeGamepadDpadApplyEvents{0};
static std::atomic<u32> s_loggedJoyConRumbleSkipped{0};
static id s_nativeGamepadConnectObserver = nil;
static id s_nativeGamepadDisconnectObserver = nil;

#pragma mark - Native D-pad
enum : u8
{
    ARMSX2_NATIVE_DPAD_UP = 1 << 0,
    ARMSX2_NATIVE_DPAD_DOWN = 1 << 1,
    ARMSX2_NATIVE_DPAD_LEFT = 1 << 2,
    ARMSX2_NATIVE_DPAD_RIGHT = 1 << 3,
};

static u8 ARMSX2NativeDpadBitForPS2Button(u32 ps2_button)
{
    switch (ps2_button)
    {
        case PadDualshock2::Inputs::PAD_UP:
            return ARMSX2_NATIVE_DPAD_UP;
        case PadDualshock2::Inputs::PAD_DOWN:
            return ARMSX2_NATIVE_DPAD_DOWN;
        case PadDualshock2::Inputs::PAD_LEFT:
            return ARMSX2_NATIVE_DPAD_LEFT;
        case PadDualshock2::Inputs::PAD_RIGHT:
            return ARMSX2_NATIVE_DPAD_RIGHT;
        default:
            return 0;
    }
}

static void ARMSX2RecomputeNativeGamepadAnyDpadMask()
{
    u8 any_mask = 0;
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
        any_mask |= s_nativeGamepadDpadMask[slot].load(std::memory_order_relaxed);

    s_nativeGamepadAnyDpadMask.store(any_mask, std::memory_order_relaxed);
}

static void ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask()
{
    u8 any_mask = 0;
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
        any_mask |= s_nativeGamepadDpadLatchedMask[slot].load(std::memory_order_relaxed);

    s_nativeGamepadAnyDpadLatchedMask.store(any_mask, std::memory_order_relaxed);
}

static u8 ARMSX2NativeDpadMaskForDirectionPad(GCControllerDirectionPad* dpad)
{
    if (!dpad)
        return 0;

    u8 mask = 0;
    if (dpad.up.pressed || dpad.yAxis.value > 0.35f)
        mask |= ARMSX2_NATIVE_DPAD_UP;
    if (dpad.down.pressed || dpad.yAxis.value < -0.35f)
        mask |= ARMSX2_NATIVE_DPAD_DOWN;
    if (dpad.left.pressed || dpad.xAxis.value < -0.35f)
        mask |= ARMSX2_NATIVE_DPAD_LEFT;
    if (dpad.right.pressed || dpad.xAxis.value > 0.35f)
        mask |= ARMSX2_NATIVE_DPAD_RIGHT;

    return mask;
}

static GCControllerDirectionPad* ARMSX2NativeDpadForController(GCController* controller)
{
    if (!controller)
        return nil;

    GCExtendedGamepad* extended = controller.extendedGamepad;
    if (extended && extended.dpad)
        return extended.dpad;

    GCPhysicalInputProfile* profile = controller.physicalInputProfile;
    if (profile && [profile respondsToSelector:@selector(dpads)]) {
        NSDictionary<NSString*, GCControllerDirectionPad*>* dpads = profile.dpads;
        for (NSString* key in dpads) {
            GCControllerDirectionPad* dpad = dpads[key];
            if (dpad)
                return dpad;
        }
    }

    return nil;
}

static void ARMSX2SetNativeGamepadDpadBit(u32 slot, u8 bit, bool pressed, const char* direction)
{
    if (slot >= ARMSX2_MAX_IOS_GAMEPADS || bit == 0)
        return;

    if (pressed) {
        s_nativeGamepadDpadLatchedMask[slot].fetch_or(bit, std::memory_order_relaxed);
        ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
    }

    const u8 old_mask = s_nativeGamepadDpadMask[slot].load(std::memory_order_relaxed);
    const u8 new_mask = pressed ? (old_mask | bit) : (old_mask & ~bit);
    if (new_mask == old_mask)
        return;

    s_nativeGamepadDpadMask[slot].store(new_mask, std::memory_order_relaxed);
    ARMSX2RecomputeNativeGamepadAnyDpadMask();
    const u32 log_index = s_loggedNativeGamepadDpadEvents.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 24) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native dpad slot=%u dir=%s pressed=%u mask=0x%02x",
            slot + 1, direction ? direction : "unknown", pressed ? 1 : 0, new_mask);
    }
}

static void ARMSX2PollNativeGamepadDpadMasks(const char* reason)
{
    NSArray<GCController*>* controllers = [GCController controllers];
    u8 any_mask = 0;
    u32 slot = 0;
    for (GCController* controller in controllers) {
        if (slot >= ARMSX2_MAX_IOS_GAMEPADS)
            break;

        const u8 mask = ARMSX2NativeDpadMaskForDirectionPad(ARMSX2NativeDpadForController(controller));
        s_nativeGamepadDpadMask[slot].store(mask, std::memory_order_relaxed);
        if (mask != 0)
            s_nativeGamepadDpadLatchedMask[slot].fetch_or(mask, std::memory_order_relaxed);
        any_mask |= mask;
        slot++;
    }

    for (; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        s_nativeGamepadDpadMask[slot].store(0, std::memory_order_relaxed);
        s_nativeGamepadDpadLatchedMask[slot].store(0, std::memory_order_relaxed);
    }

    s_nativeGamepadAnyDpadMask.store(any_mask, std::memory_order_relaxed);
    ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();

    static std::atomic<u32> s_loggedNativeGamepadPolls{0};
    const u32 log_index = s_loggedNativeGamepadPolls.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 8) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native dpad poll reason=%s controllers=%u any=0x%02x",
            reason ? reason : "poll", static_cast<unsigned>(controllers.count), any_mask);
    }
}

static void ARMSX2RefreshNativeGamepadDpadHandlersOnMain(const char* reason)
{
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        s_nativeGamepadDpadMask[slot].store(0, std::memory_order_relaxed);
        s_nativeGamepadDpadLatchedMask[slot].store(0, std::memory_order_relaxed);
    }
    s_nativeGamepadAnyDpadMask.store(0, std::memory_order_relaxed);
    s_nativeGamepadAnyDpadLatchedMask.store(0, std::memory_order_relaxed);

    NSArray<GCController*>* controllers = [GCController controllers];
    u32 slot = 0;
    for (GCController* controller in controllers) {
        if (slot >= ARMSX2_MAX_IOS_GAMEPADS)
            break;

        GCControllerDirectionPad* dpad = ARMSX2NativeDpadForController(controller);
        if (!dpad) {
            slot++;
            continue;
        }

        const u32 controller_slot = slot;
        const u8 initial_mask = ARMSX2NativeDpadMaskForDirectionPad(dpad);
        s_nativeGamepadDpadMask[controller_slot].store(initial_mask, std::memory_order_relaxed);
        if (initial_mask != 0)
            s_nativeGamepadDpadLatchedMask[controller_slot].store(initial_mask, std::memory_order_relaxed);

        dpad.up.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_UP, pressed, "up");
        };
        dpad.down.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_DOWN, pressed, "down");
        };
        dpad.left.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_LEFT, pressed, "left");
        };
        dpad.right.pressedChangedHandler = ^(GCControllerButtonInput* button, float value, BOOL pressed) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_RIGHT, pressed, "right");
        };
        dpad.valueChangedHandler = ^(GCControllerDirectionPad* directionPad, float xValue, float yValue) {
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_UP, yValue > 0.35f, "up-axis");
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_DOWN, yValue < -0.35f, "down-axis");
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_LEFT, xValue < -0.35f, "left-axis");
            ARMSX2SetNativeGamepadDpadBit(controller_slot, ARMSX2_NATIVE_DPAD_RIGHT, xValue > 0.35f, "right-axis");
        };

        NSString* vendor = controller.vendorName ?: @"unknown";
        NSString* product = @"";
        if ([controller respondsToSelector:@selector(productCategory)])
            product = controller.productCategory ?: @"";
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native dpad fallback slot=%u vendor=%s category=%s reason=%s",
            controller_slot + 1, vendor.UTF8String, product.UTF8String, reason ? reason : "refresh");

        slot++;
    }

    ARMSX2RecomputeNativeGamepadAnyDpadMask();
    ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
}

void ARMSX2InstallNativeGamepadDpadObserversOnMain()
{
    if (s_nativeGamepadConnectObserver)
        return;

    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    s_nativeGamepadConnectObserver = [center addObserverForName:GCControllerDidConnectNotification
                                                         object:nil
                                                          queue:[NSOperationQueue mainQueue]
                                                     usingBlock:^(NSNotification* notification) {
        ARMSX2RefreshNativeGamepadDpadHandlersOnMain("native-connect");
    }];
    s_nativeGamepadDisconnectObserver = [center addObserverForName:GCControllerDidDisconnectNotification
                                                            object:nil
                                                             queue:[NSOperationQueue mainQueue]
                                                        usingBlock:^(NSNotification* notification) {
        ARMSX2RefreshNativeGamepadDpadHandlersOnMain("native-disconnect");
    }];
    ARMSX2RefreshNativeGamepadDpadHandlersOnMain("observer-install");
}

#pragma mark - Multitap
enum class ARMSX2IOSMultitapMode : int
{
    Auto = 0,
    Disabled = 1,
    Port1 = 2,
    Port2 = 3,
    Both = 4,
};

static ARMSX2IOSMultitapMode ARMSX2GetIOSMultitapMode()
{
    if (!s_settings_interface)
        return ARMSX2IOSMultitapMode::Auto;

    const int value = s_settings_interface->GetIntValue("ARMSX2iOS/Gamepad", "MultitapMode", 0);
    switch (value)
    {
        case 1:
            return ARMSX2IOSMultitapMode::Disabled;
        case 2:
            return ARMSX2IOSMultitapMode::Port1;
        case 3:
            return ARMSX2IOSMultitapMode::Port2;
        case 4:
            return ARMSX2IOSMultitapMode::Both;
        default:
            return ARMSX2IOSMultitapMode::Auto;
    }
}

static const char* ARMSX2IOSMultitapModeName(ARMSX2IOSMultitapMode mode)
{
    switch (mode)
    {
        case ARMSX2IOSMultitapMode::Disabled:
            return "Disabled";
        case ARMSX2IOSMultitapMode::Port1:
            return "Port 1";
        case ARMSX2IOSMultitapMode::Port2:
            return "Port 2";
        case ARMSX2IOSMultitapMode::Both:
            return "Port 1 + Port 2";
        case ARMSX2IOSMultitapMode::Auto:
        default:
            return "Auto";
    }
}

static bool ARMSX2IOSMultitapUsesPort1(ARMSX2IOSMultitapMode mode, u32 detected_controllers)
{
    switch (mode)
    {
        case ARMSX2IOSMultitapMode::Auto:
            return detected_controllers > 2;
        case ARMSX2IOSMultitapMode::Port1:
        case ARMSX2IOSMultitapMode::Both:
            return true;
        default:
            return false;
    }
}

static bool ARMSX2IOSMultitapUsesPort2(ARMSX2IOSMultitapMode mode)
{
    return mode == ARMSX2IOSMultitapMode::Port2 || mode == ARMSX2IOSMultitapMode::Both;
}

static bool ARMSX2IOSMapsPort1Multitap(ARMSX2IOSMultitapMode mode)
{
    if (mode == ARMSX2IOSMultitapMode::Auto)
        return EmuConfig.Pad.MultitapPort0_Enabled;

    return mode == ARMSX2IOSMultitapMode::Port1 || mode == ARMSX2IOSMultitapMode::Both;
}

static bool ARMSX2IOSMapsPort2Multitap(ARMSX2IOSMultitapMode mode)
{
    if (mode == ARMSX2IOSMultitapMode::Auto)
        return false;

    return ARMSX2IOSMultitapUsesPort2(mode);
}

static void ARMSX2EnsureIOSPadType(u32 unified_slot)
{
    if (!s_settings_interface || unified_slot >= Pad::NUM_CONTROLLER_PORTS)
        return;

    const std::string section = Pad::GetConfigSection(unified_slot);
    const std::string type = s_settings_interface->GetStringValue(section.c_str(), "Type", "");
    if (type.empty() || type == "None" || type == "NotConnected")
        s_settings_interface->SetStringValue(section.c_str(), "Type", "DualShock2");
}

static u32 ARMSX2DetectedSDLGamepadCount()
{
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    SDL_free(ids);
    return static_cast<u32>(std::max(count, 0));
}

void ARMSX2ApplyIOSMultitapConfig(const char* reason)
{
    if (!s_settings_interface)
        return;

    const ARMSX2IOSMultitapMode mode = ARMSX2GetIOSMultitapMode();
    const u32 detected = ARMSX2DetectedSDLGamepadCount();
    const bool port1 = ARMSX2IOSMultitapUsesPort1(mode, detected);
    const bool port2 = ARMSX2IOSMultitapUsesPort2(mode);

    s_settings_interface->SetBoolValue("Pad", "MultitapPort1", port1);
    s_settings_interface->SetBoolValue("Pad", "MultitapPort2", port2);
    EmuConfig.Pad.MultitapPort0_Enabled = port1;
    EmuConfig.Pad.MultitapPort1_Enabled = port2;
    s_loggedMultitapRestartNeeded = false;

    for (u32 controller = 0; controller < std::min<u32>(detected, ARMSX2_MAX_IOS_GAMEPADS); controller++) {
        u32 unified_slot = controller;
        if (port1) {
            unified_slot = (controller == 0) ? 0 : controller + 1;
        } else if (port2) {
            unified_slot = (controller <= 1) ? controller : controller + 3;
        } else if (controller > 1) {
            continue;
        }

        ARMSX2EnsureIOSPadType(unified_slot);
    }

    s_settings_interface->Save();
    Console.WriteLn("[ARMSX2 iOS Gamepad] Multitap mode=%s detected=%u port1=%d port2=%d reason=%s",
        ARMSX2IOSMultitapModeName(mode), detected, port1 ? 1 : 0, port2 ? 1 : 0, reason ? reason : "unknown");
}

#pragma mark - CoreHaptics rumble
static u32 ARMSX2PackGamepadRumble(float large_intensity, float small_intensity)
{
    const u32 large = static_cast<u32>(std::clamp(large_intensity, 0.0f, 1.0f) * 65535.0f);
    const u32 small = static_cast<u32>(std::clamp(small_intensity, 0.0f, 1.0f) * 65535.0f);
    return ((large & 0xffffu) << 16) | (small & 0xffffu);
}

static float ARMSX2RumbleLargeIntensity(u32 packed)
{
    return static_cast<float>((packed >> 16) & 0xffffu) / 65535.0f;
}

static float ARMSX2RumbleSmallIntensity(u32 packed)
{
    return static_cast<float>(packed & 0xffffu) / 65535.0f;
}

static u32 ARMSX2ConnectedGamepadCount()
{
    u32 count = 0;
    for (SDL_Gamepad* gamepad : s_gamepads)
    {
        if (gamepad && SDL_GamepadConnected(gamepad))
            count++;
    }
    return count;
}

unsigned int ARMSX2PadSlotForGamepadIndex(unsigned int gamepad_index)
{
    if (gamepad_index == 0)
        return 0;

    const ARMSX2IOSMultitapMode mode = ARMSX2GetIOSMultitapMode();

    // Two controllers should behave like normal PS2 ports 1/2. Three or four
    // controllers default to Port 1 multitap, which maps to 1A/1B/1C/1D.
    if (ARMSX2IOSMapsPort1Multitap(mode))
        return gamepad_index + 1;

    // Port 2 multitap is an escape hatch for games that look there instead:
    // controller 2 remains 2A, controller 3/4 become 2B/2C.
    if (ARMSX2IOSMapsPort2Multitap(mode)) {
        if (gamepad_index == 1)
            return 1;
        return gamepad_index + 3;
    }

    return (gamepad_index <= 1) ? gamepad_index : 0xffffffffu;
}

static int ARMSX2GamepadIndexForPadSlot(u32 pad_index)
{
    if (pad_index == 0)
        return 0;

    const ARMSX2IOSMultitapMode mode = ARMSX2GetIOSMultitapMode();
    if (ARMSX2IOSMapsPort1Multitap(mode)) {
        if (pad_index >= 2 && pad_index <= 4)
            return static_cast<int>(pad_index - 1);
        return -1;
    }

    if (ARMSX2IOSMapsPort2Multitap(mode)) {
        if (pad_index == 1)
            return 1;
        if (pad_index >= 5 && pad_index <= 6)
            return static_cast<int>(pad_index - 3);
        return -1;
    }

    return (pad_index == 1) ? 1 : -1;
}

extern "C" void ARMSX2_iOSUpdatePadVibration(u32 pad_index, float large_intensity, float small_intensity)
{
    const int gamepad_index = ARMSX2GamepadIndexForPadSlot(pad_index);
    if (gamepad_index < 0 || static_cast<u32>(gamepad_index) >= ARMSX2_MAX_IOS_GAMEPADS) {
        const u32 count = s_loggedIgnoredPadRumbleCount.fetch_add(1, std::memory_order_relaxed);
        if (count < 4)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Ignoring rumble for unmapped pad=%u large=%.3f small=%.3f", pad_index, large_intensity, small_intensity);
        return;
    }

    const u32 packed = ARMSX2PackGamepadRumble(large_intensity, small_intensity);
    if (packed != 0) {
        const u32 count = s_loggedPadRumbleCommandCount.fetch_add(1, std::memory_order_relaxed);
        if (count < 12)
            Console.WriteLn("[ARMSX2 iOS Gamepad] Queued rumble pad=%u controller=%d large=%.3f small=%.3f",
                pad_index, gamepad_index + 1, large_intensity, small_intensity);
    }

    s_pendingGamepadRumble[gamepad_index].store(packed, std::memory_order_relaxed);
}

static void ARMSX2StopRumbleChannelOnMain(u32 channel)
{
    if (channel >= ARMSX2_RUMBLE_CHANNEL_COUNT || !s_nativeHapticPlayer[channel])
        return;

    NSError* error = nil;
    [s_nativeHapticPlayer[channel] stopAtTime:CHHapticTimeImmediate error:&error];
    if (error)
        Console.WriteLn("[ARMSX2 iOS Gamepad] Device rumble stop failed: %s", error.localizedDescription.UTF8String ?: "unknown");
    [s_nativeHapticPlayer[channel] release];
    s_nativeHapticPlayer[channel] = nil;
}

static void ARMSX2StopNativeGamepadRumbleOnMain()
{
    for (u32 channel = 0; channel < ARMSX2_RUMBLE_CHANNEL_COUNT; channel++)
        ARMSX2StopRumbleChannelOnMain(channel);
}

static void ARMSX2ResetNativeGamepadRumbleOnMain()
{
    ARMSX2StopNativeGamepadRumbleOnMain();
    if (s_nativeHapticEngine) {
        [s_nativeHapticEngine stopWithCompletionHandler:nil];
        [s_nativeHapticEngine release];
        s_nativeHapticEngine = nil;
    }
    s_nativeAppliedGamepadRumble = 0;
    s_nativeAppliedGamepadRumbleValid = false;
    s_loggedNativeGamepadRumbleReady = false;
}

static GCController* ARMSX2FindNativeHapticController()
{
    for (GCController* controller in [GCController controllers]) {
        if (controller.haptics)
            return controller;
    }

    return nil;
}

// True on a phone with a taptic engine. iPads and the older phones have none, and
// asking them to play a pattern just fails every frame, so check once and remember.
static bool ARMSX2DeviceSupportsHaptics()
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
        // The engine shuts itself down when idle and again when the app backgrounds.
        // Drop both players so the next rumble rebuilds them; the engine restarts above.
        // CoreHaptics calls these back on a queue of its own choosing, and everything
        // else here touches the players from main. Hop before releasing them, or the
        // release races the main thread still using them.
        s_nativeHapticEngine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2StopNativeGamepadRumbleOnMain();
                s_nativeAppliedGamepadRumble = 0;
                s_nativeAppliedGamepadRumbleValid = false;
            });
            Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptic engine stopped reason=%ld", static_cast<long>(reason));
        };
        s_nativeHapticEngine.resetHandler = ^{
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2StopNativeGamepadRumbleOnMain();
                s_nativeAppliedGamepadRumble = 0;
                s_nativeAppliedGamepadRumbleValid = false;
            });
            Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptic engine reset");
        };
    }

    NSError* error = nil;
    if (![s_nativeHapticEngine startAndReturnError:&error]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Native haptic engine start failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        return false;
    }

    return true;
}

// Each channel loops one continuous event at full intensity and its own fixed
// sharpness, and the live parameter below does the rest. The base has to be 1.0:
// IntensityControl multiplies it rather than replacing it, so anything less than
// full here becomes a ceiling nothing can get past.
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
    CHHapticPattern* pattern = [[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error] autorelease];
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

    if (!s_loggedNativeGamepadRumbleReady) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Device rumble active (continuous haptics)");
        s_loggedNativeGamepadRumbleReady = true;
    }

    return true;
}

// Expects the engine to already be up, which is the caller's job.
static bool ARMSX2SetRumbleChannelOnMain(u32 channel, float level)
{
    if (channel >= ARMSX2_RUMBLE_CHANNEL_COUNT)
        return false;

    if (level <= 0.01f) {
        ARMSX2StopRumbleChannelOnMain(channel);
        return true;
    }

    if (!ARMSX2EnsureRumbleChannelOnMain(channel))
        return false;

    const float intensity = std::clamp(level, 0.0f, 1.0f);
    NSError* error = nil;
    NSArray<CHHapticDynamicParameter*>* dynamicParams = @[
        [[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                                                         value:intensity relativeTime:0.0] autorelease]
    ];

    if (![s_nativeHapticPlayer[channel] sendParameters:dynamicParams atTime:CHHapticTimeImmediate error:&error]) {
        Console.WriteLn("[ARMSX2 iOS Gamepad] Device haptic update failed: %s", error.localizedDescription.UTF8String ?: "unknown");
        ARMSX2StopRumbleChannelOnMain(channel);
        return false;
    }

    return true;
}

static void ARMSX2ApplyNativeGamepadRumbleOnMain(u32 packed)
{
	if (s_nativeAppliedGamepadRumbleValid && packed == s_nativeAppliedGamepadRumble)
		return;

    // Straight off the packed value, so the phone gets the whole 0..1 motor range
    // rather than the 0x7000 ceiling the controller motors are held to. Read every
    // time so dragging the slider is felt on the next rumble instead of next launch.
    const float strength = s_settings_interface
        ? std::clamp(s_settings_interface->GetFloatValue("ARMSX2iOS/UI", "PhoneRumbleStrength", 1.0f), 0.0f, 1.0f)
        : 1.0f;
    // The heavy motor carries everything the game varies, so it drives its own
    // channel and keeps its range. The buzzer is on or off on a real pad, so it gets
    // a fixed level. Maxing the two together, which is what used to happen here,
    // meant one touch of the buzzer pinned the lot to full and buried the heavy motor.
    const float large_raw = ARMSX2RumbleLargeIntensity(packed);
    // Lift the game's value clear of the floor before the slider scales it, so a
    // faint rumble is felt as faint while the slider can still take it to nothing.
    const float large = (large_raw > 0.01f)
        ? (ARMSX2_RUMBLE_FLOOR + ((1.0f - ARMSX2_RUMBLE_FLOOR) * large_raw)) * strength
        : 0.0f;
    const float small = (ARMSX2RumbleSmallIntensity(packed) > 0.01f) ? (ARMSX2_SMALL_MOTOR_LEVEL * strength) : 0.0f;

    if ((large > 0.01f || small > 0.01f) && !ARMSX2EnsureNativeRumbleEngineOnMain())
        return;

    const bool large_ok = ARMSX2SetRumbleChannelOnMain(ARMSX2_RUMBLE_CHANNEL_LARGE, large);
    const bool small_ok = ARMSX2SetRumbleChannelOnMain(ARMSX2_RUMBLE_CHANNEL_SMALL, small);
    if (large_ok && small_ok) {
        s_nativeAppliedGamepadRumble = packed;
        s_nativeAppliedGamepadRumbleValid = true;
	}
}

static void ARMSX2StopNativeGamepadRumblePulseOnMain(u32 slot)
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
		[s_nativePulseHapticEngine[slot] release];
		s_nativePulseHapticEngine[slot] = nil;
	}
}

// Only ever the controller sitting in this slot. There used to be a couple of
// fallbacks here, one for a single connected controller and one that took any
// controller with haptics, and both of them answered for slots that have no
// controller at all. That put player 2's rumble in player 1's hands, and it let
// one Joy-Con make every slot test positive and kill rumble for everybody.
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

static bool ARMSX2SDLGamepadLooksLikeJoyCon(SDL_Gamepad* gamepad)
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

static bool ARMSX2ApplyNativeGamepadRumblePulseOnMain(u32 slot, u32 packed, const char* reason)
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
	// All four of these were leaking. This file is MRC and a rumbling game comes
	// through here on every change of value, so it was a steady drip for as long
	// as a controller was connected.
	NSArray<CHHapticEventParameter*>* params = @[
		[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity] autorelease],
		[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness] autorelease]
	];
	CHHapticEvent* event = [[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
	                                                      parameters:params
	                                                    relativeTime:0.0
	                                                         duration:0.18] autorelease];
	CHHapticPattern* pattern = [[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error] autorelease];
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

	// createEngineWithLocality: hands back an autoreleased engine and this file is MRC,
	// so without the retain the pool drains it at the end of this run loop turn and the
	// delayed stop below messages freed memory.
	[s_nativePulseHapticEngine[slot] release];
	s_nativePulseHapticEngine[slot] = [engine retain];
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

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ARMSX2_GAMEPAD_RUMBLE_FORCE_STOP_SECONDS * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
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
			[s_nativePulseHapticEngine[slot] release];
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

    // Ahead of the unchanged-value bail below, so a held rumble still gets its
    // stop. This used to be a dispatch_after onto the main queue holding an
    // SDL_Gamepad pointer for 300 ms, which is a long time to hope nobody
    // unplugs anything. The pump runs every frame, so a deadline covers it.
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
    if (s_appliedGamepadRumbleValid[gamepad_index] && packed == s_appliedGamepadRumble[gamepad_index])
        return;

    const u16 large = std::min<u16>(static_cast<u16>((packed >> 16) & 0xffffu), ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY);
    const u16 small = std::min<u16>(static_cast<u16>(packed & 0xffffu), ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY);
    const bool wants_rumble = (large != 0 || small != 0);

    if (!wants_rumble) {
        // The zero goes out through the normal path below, so the deadline has
        // nothing left to catch.
        s_gamepadRumbleStopDeadlineMs[gamepad_index].store(0, std::memory_order_relaxed);
        const u32 slot = gamepad_index;
        const u32 native_packed_stop = packed;
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
                const u32 native_packed = packed;
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ApplyNativeGamepadRumblePulseForJoyConOnMain(slot, native_packed, "joycon-sdl-mirror");
                });
            }
            if (wants_rumble) {
                // Each new value pushes the deadline out, so a game holding a
                // rumble keeps it and a game that goes quiet gets stopped. The
                // CoreHaptics pulse that used to be torn down alongside this
                // already schedules its own guarded stop, so it is left to it.
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
                const u32 native_packed = packed;
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ApplyNativeGamepadRumblePulseOnMain(slot, native_packed, "sdl-fallback");
                });
            }
        }
    } else if (wants_rumble) {
        const u32 slot = gamepad_index;
        const u32 native_packed = packed;
        dispatch_async(dispatch_get_main_queue(), ^{
            ARMSX2ApplyNativeGamepadRumblePulseOnMain(slot, native_packed, "no-sdl-gamepad");
        });
        // No SDL gamepad and no native haptic controller: rumble the phone itself
        // so it is felt on handheld grips (e.g. Kishi 3). The continuous engine
        // sustains and tracks intensity; the one-shot tap is what is left on an
        // iPad or an older phone with no taptic engine.
        if (!ARMSX2FindNativeHapticController()) {
            if (ARMSX2DeviceSupportsHaptics()) {
                const u32 native_packed_device = packed;
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ApplyNativeGamepadRumbleOnMain(native_packed_device);
                });
            } else {
                // Unclamped on purpose. 0x7000 is the ceiling the controller motors are
                // held to, and the tap on the other side divides by the full 16 bit
                // range, so passing the clamped pair capped this path at 44 percent.
                [ARMSX2Bridge triggerDeviceHapticLarge:((packed >> 16) & 0xffffu) small:(packed & 0xffffu)];
            }
        }
    }

    s_appliedGamepadRumble[gamepad_index] = packed;
    s_appliedGamepadRumbleValid[gamepad_index] = true;
}

static void ARMSX2ServiceGamepadRumbleTest()
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
    const u32 test_packed = ARMSX2PackGamepadRumble(0.55f, 0.55f);
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
        const bool ok = SDL_RumbleGamepad(s_gamepads[slot], ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY, ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY, 250);
        Console.WriteLn("[ARMSX2 iOS Gamepad] Test SDL controller %u rumble %s%s%s",
            slot + 1, ok ? "accepted" : "failed", ok ? "" : ": ", ok ? "" : SDL_GetError());
        if (!ok) {
            const u32 native_slot = slot;
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2ApplyNativeGamepadRumblePulseOnMain(native_slot, test_packed, "test-sdl-fallback");
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
                ARMSX2ApplyNativeGamepadRumblePulseOnMain(native_slot, test_packed, "test-no-sdl-gamepad");
            });
        }

        // The pulse path above wants a real controller and quietly does nothing
        // without one, so there was no way to feel the phone's own rumble short of
        // launching a game. Step the heavy motor up, then buzz the small one, so
        // both channels and the strength slider can be checked from settings.
        if (!ARMSX2FindNativeHapticController() && ARMSX2DeviceSupportsHaptics()) {
            static constexpr struct { double delay; float large; float small; } ramp[] = {
                { 0.00, 0.20f, 0.0f },
                { 0.30, 0.60f, 0.0f },
                { 0.60, 1.00f, 0.0f },
                { 0.95, 0.00f, 1.0f },
                { 1.25, 0.00f, 0.0f },
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
    const u64 deadline = SDL_GetTicks() + 300;
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
        s_gamepadRumbleStopDeadlineMs[slot].store(deadline, std::memory_order_relaxed);

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(0.30 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
            ARMSX2StopNativeGamepadRumblePulseOnMain(slot);
        Console.WriteLn("[ARMSX2 iOS Gamepad] Test controller rumble stopped");
    });
}

extern "C" void ARMSX2_iOSTestGamepadRumble(void)
{
    // Called from SwiftUI, so this is the main thread. The pump owns s_gamepads
    // and will happily close a pad while we are opening one, so hand the work
    // over when it is running. With no VM there is no pump and nothing to race,
    // and waiting for one that will never come would just break the button.
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

#pragma mark - SDL gamepad refresh
void ARMSX2RefreshIOSGamepads()
{
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    ARMSX2PollNativeGamepadDpadMasks("sdl-refresh");

    if (s_gamepadRumbleTestRequested.exchange(false, std::memory_order_relaxed))
        ARMSX2ServiceGamepadRumbleTest();

    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++) {
        SDL_Gamepad* gamepad = s_gamepads[slot];
        if (!gamepad)
            continue;

        if (!SDL_GamepadConnected(gamepad)) {
            Console.WriteLn("[Files] MFi gamepad %u disconnected", slot + 1);
            s_pendingGamepadRumble[slot].store(0, std::memory_order_relaxed);
            s_gamepadRumbleStopDeadlineMs[slot].store(0, std::memory_order_relaxed);
            s_gamepadIsJoyCon[slot].store(false, std::memory_order_relaxed);
            s_appliedGamepadRumble[slot] = 0;
            s_appliedGamepadRumbleValid[slot] = false;
            s_nativeGamepadDpadMask[slot].store(0, std::memory_order_relaxed);
            s_nativeGamepadDpadLatchedMask[slot].store(0, std::memory_order_relaxed);
            ARMSX2RecomputeNativeGamepadAnyDpadMask();
            ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
            const u32 disconnected_slot = slot;
            dispatch_async(dispatch_get_main_queue(), ^{
                ARMSX2StopNativeGamepadRumblePulseOnMain(disconnected_slot);
            });
            if (slot == 0) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2ResetNativeGamepadRumbleOnMain();
                });
            }
            SDL_CloseGamepad(gamepad);
            s_gamepads[slot] = nullptr;
        }
    }

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids)
        return;

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
            if (s_gamepads[slot])
                continue;

            s_gamepads[slot] = SDL_OpenGamepad(ids[id_index]);
            if (s_gamepads[slot]) {
                s_gamepadIsJoyCon[slot].store(ARMSX2SDLGamepadLooksLikeJoyCon(s_gamepads[slot]), std::memory_order_relaxed);
                dispatch_async(dispatch_get_main_queue(), ^{
                    ARMSX2RefreshNativeGamepadDpadHandlersOnMain("sdl-open");
                });
                const u32 pad_slot = ARMSX2PadSlotForGamepadIndex(slot);
                if (pad_slot == 0xffffffffu || pad_slot >= Pad::NUM_CONTROLLER_PORTS) {
                    Console.WriteLn("[Files] MFi gamepad %u connected but ignored by current multitap mode: %s",
                        slot + 1, SDL_GetGamepadName(s_gamepads[slot]));
                } else {
                    Console.WriteLn("[Files] MFi gamepad %u connected to PS2 pad slot %u: %s",
                        slot + 1, pad_slot + 1, SDL_GetGamepadName(s_gamepads[slot]));
                }
                if (!s_loggedMultitapRestartNeeded && s_vmThreadActive.load() &&
                    ARMSX2GetIOSMultitapMode() == ARMSX2IOSMultitapMode::Auto &&
                    ARMSX2ConnectedGamepadCount() > 2 && !EmuConfig.Pad.MultitapPort0_Enabled) {
                    Console.Warning("[ARMSX2 iOS Gamepad] 3+ controllers connected after boot; restart/reset with controllers connected to enable multitap.");
                    s_loggedMultitapRestartNeeded = true;
                }
            }
            break;
        }
    }

    SDL_free(ids);
}

static bool ARMSX2ShouldPreserveTouchState(u32 ps2_button, bool preserve_touch)
{
    return preserve_touch && ps2_button < (sizeof(g_touchPadState) / sizeof(g_touchPadState[0])) && g_touchPadState[ps2_button];
}

void ARMSX2ApplyIOSGamepadInput(unsigned int gamepad_index, SDL_Gamepad* gamepad, PadBase* pad, bool preserve_touch)
{
    if (!gamepad || !pad)
        return;

    if (s_captureMode.load()) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
            if (SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(b))) {
                s_capturedButton.store(b);
                break;
            }
        }
    }

    static const u32 ps2Buttons[] = {
        PadDualshock2::Inputs::PAD_UP, PadDualshock2::Inputs::PAD_DOWN,
        PadDualshock2::Inputs::PAD_LEFT, PadDualshock2::Inputs::PAD_RIGHT,
        PadDualshock2::Inputs::PAD_CROSS, PadDualshock2::Inputs::PAD_CIRCLE,
        PadDualshock2::Inputs::PAD_SQUARE, PadDualshock2::Inputs::PAD_TRIANGLE,
        PadDualshock2::Inputs::PAD_L1, PadDualshock2::Inputs::PAD_R1,
        0, 0, // L2/R2 handled as analog
        PadDualshock2::Inputs::PAD_START, PadDualshock2::Inputs::PAD_SELECT,
        PadDualshock2::Inputs::PAD_L3, PadDualshock2::Inputs::PAD_R3,
    };

    for (int i = 0; i < 16; i++) {
        const int sdlBtn = s_buttonMap[i];
        if (sdlBtn < 0)
            continue;

        const u32 ps2Button = ps2Buttons[i];
        if (ps2Button == 0)
            continue;

        if (ARMSX2NativeDpadBitForPS2Button(ps2Button) != 0)
            continue;

        bool pressed = SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(sdlBtn));

        if (pressed)
            pad->Set(ps2Button, 1.0f);
        else if (!ARMSX2ShouldPreserveTouchState(ps2Button, preserve_touch))
            pad->Set(ps2Button, 0.0f);
    }

    struct DpadBinding
    {
        int map_index;
        u32 ps2_button;
        u8 native_bit;
    };
    static constexpr DpadBinding dpad_bindings[] = {
        {0, PadDualshock2::Inputs::PAD_UP, ARMSX2_NATIVE_DPAD_UP},
        {1, PadDualshock2::Inputs::PAD_DOWN, ARMSX2_NATIVE_DPAD_DOWN},
        {2, PadDualshock2::Inputs::PAD_LEFT, ARMSX2_NATIVE_DPAD_LEFT},
        {3, PadDualshock2::Inputs::PAD_RIGHT, ARMSX2_NATIVE_DPAD_RIGHT},
    };

    u8 native_dpad_mask = 0;
    u8 slot_latched_mask = 0;
    u8 any_latched_mask = 0;
    if (gamepad_index < ARMSX2_MAX_IOS_GAMEPADS) {
        slot_latched_mask = s_nativeGamepadDpadLatchedMask[gamepad_index].exchange(0, std::memory_order_relaxed);
        native_dpad_mask = s_nativeGamepadDpadMask[gamepad_index].load(std::memory_order_relaxed) | slot_latched_mask;
        if (gamepad_index == 0 && ARMSX2ConnectedGamepadCount() <= 1) {
            any_latched_mask = s_nativeGamepadAnyDpadLatchedMask.exchange(0, std::memory_order_relaxed);
            native_dpad_mask |= s_nativeGamepadAnyDpadMask.load(std::memory_order_relaxed) | any_latched_mask;
        }
        ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
    }

    for (const DpadBinding& binding : dpad_bindings) {
        bool pressed = false;
        const int sdlBtn = s_buttonMap[binding.map_index];
        if (sdlBtn >= 0)
            pressed = SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(sdlBtn));

        const bool native_pressed = ((native_dpad_mask & binding.native_bit) != 0);
        pressed = pressed || native_pressed;

        if (native_pressed) {
            const u32 log_index = s_loggedNativeGamepadDpadApplyEvents.fetch_add(1, std::memory_order_relaxed);
            if (log_index < 48) {
                Console.WriteLn("[ARMSX2 iOS Gamepad] Native dpad applied gamepad=%u ps2=0x%08x slot_mask=0x%02x slot_latched=0x%02x any_mask=0x%02x any_latched=0x%02x",
                    gamepad_index + 1, binding.ps2_button,
                    s_nativeGamepadDpadMask[gamepad_index].load(std::memory_order_relaxed),
                    slot_latched_mask,
                    s_nativeGamepadAnyDpadMask.load(std::memory_order_relaxed),
                    any_latched_mask);
            }
        }

        if (pressed)
            pad->Set(binding.ps2_button, 1.0f);
        else if (!ARMSX2ShouldPreserveTouchState(binding.ps2_button, preserve_touch))
            pad->Set(binding.ps2_button, 0.0f);
    }

    const float l2 = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
    const float r2 = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
    if (l2 > 0.1f || !ARMSX2ShouldPreserveTouchState(PadDualshock2::Inputs::PAD_L2, preserve_touch))
        pad->Set(PadDualshock2::Inputs::PAD_L2, l2 > 0.1f ? l2 : 0.0f);
    if (r2 > 0.1f || !ARMSX2ShouldPreserveTouchState(PadDualshock2::Inputs::PAD_R2, preserve_touch))
        pad->Set(PadDualshock2::Inputs::PAD_R2, r2 > 0.1f ? r2 : 0.0f);

    auto axis = [&](SDL_GamepadAxis a) -> float {
        const float v = SDL_GetGamepadAxis(gamepad, a) / 32767.0f;
        return (v > 0.15f || v < -0.15f) ? v : 0.0f;
    };
    const float lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
    const float ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    const float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    const float ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    auto set_axis = [&](u32 input, float value) {
        if (value > 0.0f || !ARMSX2ShouldPreserveTouchState(input, preserve_touch))
            pad->Set(input, value);
    };
    set_axis(PadDualshock2::Inputs::PAD_L_RIGHT, lx > 0 ? lx : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_L_LEFT,  lx < 0 ? -lx : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_L_DOWN,  ly > 0 ? ly : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_L_UP,    ly < 0 ? -ly : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_R_RIGHT, rx > 0 ? rx : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_R_LEFT,  rx < 0 ? -rx : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_R_DOWN,  ry > 0 ? ry : 0.0f);
    set_axis(PadDualshock2::Inputs::PAD_R_UP,    ry < 0 ? -ry : 0.0f);
}
