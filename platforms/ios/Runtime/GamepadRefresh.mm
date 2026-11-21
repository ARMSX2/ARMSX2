// GamepadRefresh.mm — SDL gamepad refresh and the stick response
// SPDX-License-Identifier: GPL-3.0+

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

#include "common/Console.h"
#include "pcsx2/Config.h"          // EmuConfig, GSConfig
#include "pcsx2/Host.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"

#include "IOSRuntime.h"
#import "GamepadInternal.h"

static std::atomic<u32> s_loggedNativeGamepadDpadApplyEvents{0};

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
            s_controllerMacroConsumedMask[slot].store(0, std::memory_order_relaxed);
            s_controllerMacroInputGateStates[slot] = {};
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
                s_controllerMacroConsumedMask[slot].store(0, std::memory_order_relaxed);
                s_controllerMacroInputGateStates[slot] = {};
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

struct ARMSX2ControllerStickSettings
{
    float dead_zone = 0.15f;
    bool left_instant_deadzone = false;
    float left_negative_deadzone = -0.08f;
    bool right_instant_deadzone = false;
    float right_negative_deadzone = -0.08f;
    bool invert_left_x = false;
    bool invert_left_y = false;
    bool invert_right_x = false;
    bool invert_right_y = false;
};

static const ARMSX2ControllerStickSettings& ARMSX2GetControllerStickSettings()
{
    // Controller input runs every frame. Refreshing this small in-memory snapshot
    // keeps the hot path lock-free without making setting changes feel delayed.
    static ARMSX2ControllerStickSettings settings;
    static u64 last_refresh = 0;
    static bool valid = false;
    const u64 now = SDL_GetTicks();
    if (valid && now - last_refresh < 100)
        return settings;

    last_refresh = now;
    valid = true;
    auto settings_lock = Host::GetSettingsLock();
    SettingsInterface* const layered_settings = Host::GetSettingsInterface();
    if (!layered_settings)
        return settings;

    settings.dead_zone = std::clamp(
        layered_settings->GetFloatValue("ARMSX2iOS/Gamepad", "StickDeadZone", 0.15f),
        0.0f, 0.25f);
    settings.left_instant_deadzone = layered_settings->GetBoolValue(
        "ARMSX2iOS/Gamepad", "LeftInstantDeadzoneEnabled", false);
    settings.left_negative_deadzone = std::clamp(
        layered_settings->GetFloatValue("ARMSX2iOS/Gamepad", "LeftNegativeDeadzone", -0.08f),
        -0.25f, 0.0f);
    settings.right_instant_deadzone = layered_settings->GetBoolValue(
        "ARMSX2iOS/Gamepad", "RightInstantDeadzoneEnabled", false);
    settings.right_negative_deadzone = std::clamp(
        layered_settings->GetFloatValue("ARMSX2iOS/Gamepad", "RightNegativeDeadzone", -0.08f),
        -0.25f, 0.0f);
    settings.invert_left_x = layered_settings->GetBoolValue(
        "ARMSX2iOS/UI", "InvertLeftStickX", false);
    settings.invert_left_y = layered_settings->GetBoolValue(
        "ARMSX2iOS/UI", "InvertLeftStickY", false);
    settings.invert_right_x = layered_settings->GetBoolValue(
        "ARMSX2iOS/UI", "InvertRightStickX", false);
    settings.invert_right_y = layered_settings->GetBoolValue(
        "ARMSX2iOS/UI", "InvertRightStickY", false);
    return settings;
}

static void ARMSX2ApplyControllerStickResponse(
    float& x, float& y, float dead_zone, bool instant_deadzone, float negative_deadzone)
{
    // Per-axis threshold first; the radial remap below runs only when instant deadzone is on.
    x = (std::abs(x) > dead_zone) ? x : 0.0f;
    y = (std::abs(y) > dead_zone) ? y : 0.0f;
    if (!instant_deadzone)
        return;

    const float magnitude = std::hypot(x, y);
    if (magnitude <= 0.0f)
        return;

    // A negative deadzone offsets the first real input away from zero while
    // preserving the full-scale endpoint.
    const float floor = std::clamp(-negative_deadzone, 0.0f, 0.25f);
    const float clamped_magnitude = std::min(magnitude, 1.0f);
    const float output_magnitude = floor + clamped_magnitude * (1.0f - floor);
    const float scale = output_magnitude / magnitude;
    x = std::clamp(x * scale, -1.0f, 1.0f);
    y = std::clamp(y * scale, -1.0f, 1.0f);
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

    const u32 raw_macro_input_mask =
        ARMSX2RawControllerMacroPhysicalInputMask(gamepad, native_dpad_mask);
    const u32 gameplay_macro_input_mask =
        ARMSX2ResolveControllerMacroGameplayMask(gamepad_index, raw_macro_input_mask);
    const u32 replay_macro_input_mask =
        ARMSX2ControllerMacroCurrentReplayMask(gamepad_index);

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

        const SDL_GamepadButton sdl_button = static_cast<SDL_GamepadButton>(sdlBtn);
        bool pressed = SDL_GetGamepadButton(gamepad, sdl_button);
        const u32 macro_bit = ARMSX2ControllerMacroBitForSDLButton(sdl_button);
        if (macro_bit != 0)
            pressed = (gameplay_macro_input_mask & macro_bit) != 0;

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

    for (const DpadBinding& binding : dpad_bindings) {
        bool pressed = false;
        const int sdlBtn = s_buttonMap[binding.map_index];
        if (sdlBtn >= 0) {
            const SDL_GamepadButton sdl_button = static_cast<SDL_GamepadButton>(sdlBtn);
            const bool raw_mapped_pressed = SDL_GetGamepadButton(gamepad, sdl_button);
            pressed = raw_mapped_pressed;
            const u32 mapped_macro_bit = ARMSX2ControllerMacroBitForSDLButton(sdl_button);
            if (mapped_macro_bit != 0) {
                pressed = (raw_mapped_pressed
                              && (gameplay_macro_input_mask & mapped_macro_bit) != 0)
                    || (replay_macro_input_mask & mapped_macro_bit) != 0;
            }
        }

        const bool raw_native_pressed = (native_dpad_mask & binding.native_bit) != 0;
        const bool native_pressed = (raw_native_pressed
                                        && (gameplay_macro_input_mask & binding.native_bit) != 0)
            || (replay_macro_input_mask & binding.native_bit) != 0;
        pressed = pressed || native_pressed;

        if (raw_native_pressed && native_pressed) {
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

    float l2 = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
    float r2 = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
    if (replay_macro_input_mask & ARMSX2_MACRO_INPUT_L2) {
        l2 = 1.0f;
    } else if ((raw_macro_input_mask & ARMSX2_MACRO_INPUT_L2)
        && !(gameplay_macro_input_mask & ARMSX2_MACRO_INPUT_L2)) {
        l2 = 0.0f;
    }
    if (replay_macro_input_mask & ARMSX2_MACRO_INPUT_R2) {
        r2 = 1.0f;
    } else if ((raw_macro_input_mask & ARMSX2_MACRO_INPUT_R2)
        && !(gameplay_macro_input_mask & ARMSX2_MACRO_INPUT_R2)) {
        r2 = 0.0f;
    }
    if (l2 > 0.1f || !ARMSX2ShouldPreserveTouchState(PadDualshock2::Inputs::PAD_L2, preserve_touch))
        pad->Set(PadDualshock2::Inputs::PAD_L2, l2 > 0.1f ? l2 : 0.0f);
    if (r2 > 0.1f || !ARMSX2ShouldPreserveTouchState(PadDualshock2::Inputs::PAD_R2, preserve_touch))
        pad->Set(PadDualshock2::Inputs::PAD_R2, r2 > 0.1f ? r2 : 0.0f);

    auto axis = [&](SDL_GamepadAxis a) -> float {
        const float v = SDL_GetGamepadAxis(gamepad, a) / 32767.0f;
        return std::clamp(v, -1.0f, 1.0f);
    };
    float lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
    float ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    float ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);

    const ARMSX2ControllerStickSettings& stick_settings = ARMSX2GetControllerStickSettings();
    ARMSX2ApplyControllerStickResponse(
        lx, ly, stick_settings.dead_zone,
        stick_settings.left_instant_deadzone, stick_settings.left_negative_deadzone);
    ARMSX2ApplyControllerStickResponse(
        rx, ry, stick_settings.dead_zone,
        stick_settings.right_instant_deadzone, stick_settings.right_negative_deadzone);
    if (stick_settings.invert_left_x)
        lx = -lx;
    if (stick_settings.invert_left_y)
        ly = -ly;
    if (stick_settings.invert_right_x)
        rx = -rx;
    if (stick_settings.invert_right_y)
        ry = -ry;

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
