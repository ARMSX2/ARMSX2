// GamepadInput.mm — gamepad state, macros, native D-pad and multitap
// SPDX-License-Identifier: GPL-3.0+

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <SDL3/SDL.h>

#include "common/Console.h"
#include "pcsx2/Config.h"          // EmuConfig, GSConfig
#include "pcsx2/Host.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"

#include "IOSRuntime.h"
#import "GamepadInternal.h"

#pragma mark - Gamepad state
// Touched only by the CPU thread, from inside the pump. Anything that used to
// reach in here from the main queue now works off the caches below instead, so
// a controller can be closed without a queued block finding freed memory.
SDL_Gamepad* s_gamepads[ARMSX2_MAX_IOS_GAMEPADS] = {};
std::atomic<u32> s_pendingGamepadRumble[ARMSX2_MAX_IOS_GAMEPADS];
// SDL_GetTicks value after which the slot gets a stop, or 0 for nothing pending.
std::atomic<u64> s_gamepadRumbleStopDeadlineMs[ARMSX2_MAX_IOS_GAMEPADS];
// Worked out once when the pad is opened, so the name lookup does not have to
// happen on whichever thread is asking.
std::atomic<bool> s_gamepadIsJoyCon[ARMSX2_MAX_IOS_GAMEPADS];
std::atomic<bool> s_gamepadRumbleTestRequested{false};
u32 s_appliedGamepadRumble[ARMSX2_MAX_IOS_GAMEPADS] = {};
bool s_appliedGamepadRumbleValid[ARMSX2_MAX_IOS_GAMEPADS] = {};
static std::atomic<u32> s_loggedPadRumbleCommandCount{0};
static std::atomic<u32> s_loggedIgnoredPadRumbleCount{0};
bool s_loggedMultitapRestartNeeded = false;
// Cached, since the core reaches the multitap read while it holds the settings lock.
static std::atomic<int> s_iosMultitapMode{0};
std::atomic<u8> s_nativeGamepadDpadMask[ARMSX2_MAX_IOS_GAMEPADS];
std::atomic<u8> s_nativeGamepadDpadLatchedMask[ARMSX2_MAX_IOS_GAMEPADS];
std::atomic<u8> s_nativeGamepadAnyDpadMask{0};
std::atomic<u8> s_nativeGamepadAnyDpadLatchedMask{0};
static std::atomic<u32> s_loggedNativeGamepadDpadEvents{0};
static id s_nativeGamepadConnectObserver = nil;
static id s_nativeGamepadDisconnectObserver = nil;

static std::atomic<u32> s_controllerMacroInputMask{0};
static std::atomic<u32> s_controllerMacroModifierMask{0};
std::atomic<u32> s_controllerMacroConsumedMask[ARMSX2_MAX_IOS_GAMEPADS];
static constexpr u64 ARMSX2_CONTROLLER_MACRO_CHORD_GRACE_MS = 110;

ARMSX2ControllerMacroInputGateState
    s_controllerMacroInputGateStates[ARMSX2_MAX_IOS_GAMEPADS];

void ARMSX2ConfigureControllerMacroInput(u32 input_mask, u32 modifier_mask)
{
    input_mask &= ARMSX2_MACRO_INPUT_VALID_MASK;
    s_controllerMacroInputMask.store(input_mask, std::memory_order_relaxed);
    s_controllerMacroModifierMask.store(
        modifier_mask & input_mask,
        std::memory_order_relaxed
    );
}

void ARMSX2ConsumeControllerMacroInput(u32 input_mask)
{
    input_mask &= ARMSX2_MACRO_INPUT_VALID_MASK;
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
        s_controllerMacroConsumedMask[slot].fetch_or(input_mask, std::memory_order_relaxed);
}

u32 ARMSX2ControllerMacroBitForSDLButton(SDL_GamepadButton button)
{
    switch (button)
    {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return ARMSX2_MACRO_INPUT_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ARMSX2_MACRO_INPUT_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ARMSX2_MACRO_INPUT_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ARMSX2_MACRO_INPUT_RIGHT;
        case SDL_GAMEPAD_BUTTON_SOUTH: return ARMSX2_MACRO_INPUT_CROSS;
        case SDL_GAMEPAD_BUTTON_EAST: return ARMSX2_MACRO_INPUT_CIRCLE;
        case SDL_GAMEPAD_BUTTON_WEST: return ARMSX2_MACRO_INPUT_SQUARE;
        case SDL_GAMEPAD_BUTTON_NORTH: return ARMSX2_MACRO_INPUT_TRIANGLE;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ARMSX2_MACRO_INPUT_L1;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ARMSX2_MACRO_INPUT_R1;
        case SDL_GAMEPAD_BUTTON_START: return ARMSX2_MACRO_INPUT_START;
        case SDL_GAMEPAD_BUTTON_BACK: return ARMSX2_MACRO_INPUT_SELECT;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ARMSX2_MACRO_INPUT_L3;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ARMSX2_MACRO_INPUT_R3;
        default: return 0;
    }
}

u32 ARMSX2RawControllerMacroPhysicalInputMask(SDL_Gamepad* gamepad, u8 native_dpad_mask)
{
    auto pressed = [gamepad](SDL_GamepadButton button) {
        return SDL_GetGamepadButton(gamepad, button);
    };

    u32 mask = 0;
    if (pressed(SDL_GAMEPAD_BUTTON_DPAD_UP) || (native_dpad_mask & ARMSX2_MACRO_INPUT_UP))
        mask |= ARMSX2_MACRO_INPUT_UP;
    if (pressed(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || (native_dpad_mask & ARMSX2_MACRO_INPUT_DOWN))
        mask |= ARMSX2_MACRO_INPUT_DOWN;
    if (pressed(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || (native_dpad_mask & ARMSX2_MACRO_INPUT_LEFT))
        mask |= ARMSX2_MACRO_INPUT_LEFT;
    if (pressed(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || (native_dpad_mask & ARMSX2_MACRO_INPUT_RIGHT))
        mask |= ARMSX2_MACRO_INPUT_RIGHT;
    if (pressed(SDL_GAMEPAD_BUTTON_SOUTH)) mask |= ARMSX2_MACRO_INPUT_CROSS;
    if (pressed(SDL_GAMEPAD_BUTTON_EAST)) mask |= ARMSX2_MACRO_INPUT_CIRCLE;
    if (pressed(SDL_GAMEPAD_BUTTON_WEST)) mask |= ARMSX2_MACRO_INPUT_SQUARE;
    if (pressed(SDL_GAMEPAD_BUTTON_NORTH)) mask |= ARMSX2_MACRO_INPUT_TRIANGLE;
    if (pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) mask |= ARMSX2_MACRO_INPUT_L1;
    if (pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) mask |= ARMSX2_MACRO_INPUT_R1;
    if (pressed(SDL_GAMEPAD_BUTTON_START)) mask |= ARMSX2_MACRO_INPUT_START;
    if (pressed(SDL_GAMEPAD_BUTTON_BACK)) mask |= ARMSX2_MACRO_INPUT_SELECT;
    if (pressed(SDL_GAMEPAD_BUTTON_LEFT_STICK)) mask |= ARMSX2_MACRO_INPUT_L3;
    if (pressed(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) mask |= ARMSX2_MACRO_INPUT_R3;

    if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 3277)
        mask |= ARMSX2_MACRO_INPUT_L2;
    if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 3277)
        mask |= ARMSX2_MACRO_INPUT_R2;
    return mask;
}

u32 ARMSX2ResolveControllerMacroGameplayMask(u32 gamepad_index, u32 raw_mask)
{
    if (gamepad_index >= ARMSX2_MAX_IOS_GAMEPADS)
        return raw_mask;

    ARMSX2ControllerMacroInputGateState& state =
        s_controllerMacroInputGateStates[gamepad_index];
    const u32 configured_mask =
        s_controllerMacroInputMask.load(std::memory_order_relaxed);
    const u32 modifier_mask =
        s_controllerMacroModifierMask.load(std::memory_order_relaxed);
    state.consumed_until_release_mask |=
        s_controllerMacroConsumedMask[gamepad_index].exchange(0, std::memory_order_relaxed)
        & configured_mask;
    state.current_replay_mask = 0;

    const u64 now = SDL_GetTicks();
    // Only a modifier needs a grace window; every other input (D-pad, triggers, face/shoulder
    // buttons, Select, stick clicks) is delivered immediately from the raw mask.
    // A recognized chord still consumes its complete input mask until release.
    u32 gameplay_mask = raw_mask & ~modifier_mask;
    for (u32 index = 0; index < 16; index++) {
        const u32 bit = 1u << index;
        const bool is_configured = (configured_mask & bit) != 0;
        const bool is_modifier = (modifier_mask & bit) != 0;
        const bool is_pressed = (raw_mask & bit) != 0;
        const bool was_pressed = (state.previous_raw_mask & bit) != 0;

        if (!is_configured) {
            state.consumed_until_release_mask &= ~bit;
            state.delivered_mask &= ~bit;
            state.replay_eligible_mask &= ~bit;
            state.press_started_ms[index] = 0;
            state.replay_frames[index] = 0;
            if (is_pressed)
                gameplay_mask |= bit;
            continue;
        }

        // Secondary inputs reach the game at once. A chord Swift recognizes sets their consumed
        // bit, which withholds them from the next poll until physical release.
        if (!is_modifier) {
            state.delivered_mask &= ~bit;
            state.replay_eligible_mask &= ~bit;
            state.press_started_ms[index] = 0;
            state.replay_frames[index] = 0;
            if (!is_pressed) {
                state.consumed_until_release_mask &= ~bit;
            } else if (state.consumed_until_release_mask & bit) {
                gameplay_mask &= ~bit;
            } else {
                gameplay_mask |= bit;
            }
            continue;
        }

        if (!is_pressed) {
            const bool was_consumed =
                (state.consumed_until_release_mask & bit) != 0;
            const bool was_delivered = (state.delivered_mask & bit) != 0;
            const bool can_replay = (state.replay_eligible_mask & bit) != 0;
            if (was_pressed && can_replay && !was_consumed && !was_delivered)
                state.replay_frames[index] = 2;

            state.consumed_until_release_mask &= ~bit;
            state.delivered_mask &= ~bit;
            state.replay_eligible_mask &= ~bit;
            state.press_started_ms[index] = 0;
            if (state.replay_frames[index] > 0) {
                gameplay_mask |= bit;
                state.current_replay_mask |= bit;
                state.replay_frames[index]--;
            }
            continue;
        }

        state.replay_frames[index] = 0;
        if (raw_mask & bit)
            state.replay_eligible_mask |= bit;
        if (!was_pressed || state.press_started_ms[index] == 0)
            state.press_started_ms[index] = now;
        if (state.consumed_until_release_mask & bit)
            continue;

        if (state.delivered_mask & bit
            || now - state.press_started_ms[index]
                >= ARMSX2_CONTROLLER_MACRO_CHORD_GRACE_MS) {
            state.delivered_mask |= bit;
            gameplay_mask |= bit;
        }
    }
    state.previous_raw_mask = raw_mask;
    return gameplay_mask;
}

u32 ARMSX2ControllerMacroCurrentReplayMask(u32 gamepad_index)
{
    return gamepad_index < ARMSX2_MAX_IOS_GAMEPADS
        ? s_controllerMacroInputGateStates[gamepad_index].current_replay_mask
        : 0;
}

#pragma mark - Native D-pad

u8 ARMSX2NativeDpadBitForPS2Button(u32 ps2_button)
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

void ARMSX2RecomputeNativeGamepadAnyDpadMask()
{
    u8 any_mask = 0;
    for (u32 slot = 0; slot < ARMSX2_MAX_IOS_GAMEPADS; slot++)
        any_mask |= s_nativeGamepadDpadMask[slot].load(std::memory_order_relaxed);

    s_nativeGamepadAnyDpadMask.store(any_mask, std::memory_order_relaxed);
}

void ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask()
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

void ARMSX2PollNativeGamepadDpadMasks(const char* reason)
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

void ARMSX2RefreshNativeGamepadDpadHandlersOnMain(const char* reason)
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

ARMSX2IOSMultitapMode ARMSX2GetIOSMultitapMode()
{
    if (!s_settings_interface)
        return ARMSX2IOSMultitapMode::Auto;

    const int value = s_iosMultitapMode.load(std::memory_order_relaxed);
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
    const std::string type = Host::GetBaseStringSettingValue(section.c_str(), "Type", "");
    if (type.empty() || type == "None" || type == "NotConnected")
        Host::SetBaseStringSettingValue(section.c_str(), "Type", "DualShock2");
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

    s_iosMultitapMode.store(Host::GetBaseIntSettingValue("ARMSX2iOS/Gamepad", "MultitapMode", 0), std::memory_order_relaxed);
    const ARMSX2IOSMultitapMode mode = ARMSX2GetIOSMultitapMode();
    const u32 detected = ARMSX2DetectedSDLGamepadCount();
    const bool port1 = ARMSX2IOSMultitapUsesPort1(mode, detected);
    const bool port2 = ARMSX2IOSMultitapUsesPort2(mode);

    Host::SetBaseBoolSettingValue("Pad", "MultitapPort1", port1);
    Host::SetBaseBoolSettingValue("Pad", "MultitapPort2", port2);
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

    Host::CommitBaseSettingChanges();
    Console.WriteLn("[ARMSX2 iOS Gamepad] Multitap mode=%s detected=%u port1=%d port2=%d reason=%s",
        ARMSX2IOSMultitapModeName(mode), detected, port1 ? 1 : 0, port2 ? 1 : 0, reason ? reason : "unknown");
}

u32 ARMSX2ConnectedGamepadCount()
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
