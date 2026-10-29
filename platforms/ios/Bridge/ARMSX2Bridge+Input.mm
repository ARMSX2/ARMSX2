// ARMSX2Bridge+Input.mm — pad, sticks, macros, rumble and button mapping
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2BridgeInternal.h"
#import <GameController/GameController.h>

// Xcode names the generated Swift bridge header after the Swift module.
#import "ARMSX2iOS-Swift.h"

#include <SDL3/SDL.h>

#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "VMManager.h"

#include "IOSRuntime.h"

@implementation ARMSX2Bridge (Input)

+ (void)setPadButton:(ARMSX2PadButton)button pressed:(BOOL)pressed {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;

    static const u32 buttonMap[] = {
        PadDualshock2::Inputs::PAD_UP,       // Up
        PadDualshock2::Inputs::PAD_DOWN,     // Down
        PadDualshock2::Inputs::PAD_LEFT,     // Left
        PadDualshock2::Inputs::PAD_RIGHT,    // Right
        PadDualshock2::Inputs::PAD_CROSS,    // Cross
        PadDualshock2::Inputs::PAD_CIRCLE,   // Circle
        PadDualshock2::Inputs::PAD_SQUARE,   // Square
        PadDualshock2::Inputs::PAD_TRIANGLE, // Triangle
        PadDualshock2::Inputs::PAD_L1,       // L1
        PadDualshock2::Inputs::PAD_R1,       // R1
        PadDualshock2::Inputs::PAD_L2,       // L2
        PadDualshock2::Inputs::PAD_R2,       // R2
        PadDualshock2::Inputs::PAD_START,    // Start
        PadDualshock2::Inputs::PAD_SELECT,   // Select
        PadDualshock2::Inputs::PAD_L3,       // L3
        PadDualshock2::Inputs::PAD_R3,       // R3
    };

    if ((int)button < (int)(sizeof(buttonMap)/sizeof(buttonMap[0]))) {
        u32 idx = buttonMap[(int)button];
        pad->Set(idx, pressed ? 1.0f : 0.0f);
        // Update touch state so PumpMessagesOnCPUThread doesn't override
        if (idx < 64) g_touchPadState[idx] = pressed;
    }
}

+ (void)setLeftStickX:(float)x Y:(float)y {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;
    // Convert axis (-1..+1) to individual direction values (0..1)
    const float right = x > 0 ? x : 0.0f;
    const float left = x < 0 ? -x : 0.0f;
    const float down = y > 0 ? y : 0.0f;
    const float up = y < 0 ? -y : 0.0f;
    pad->Set(PadDualshock2::Inputs::PAD_L_RIGHT, right);
    pad->Set(PadDualshock2::Inputs::PAD_L_LEFT, left);
    pad->Set(PadDualshock2::Inputs::PAD_L_DOWN, down);
    pad->Set(PadDualshock2::Inputs::PAD_L_UP, up);
    g_touchPadState[PadDualshock2::Inputs::PAD_L_RIGHT] = right > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_L_LEFT] = left > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_L_DOWN] = down > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_L_UP] = up > 0.01f;
}

+ (void)setRightStickX:(float)x Y:(float)y {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;
    const float right = x > 0 ? x : 0.0f;
    const float left = x < 0 ? -x : 0.0f;
    const float down = y > 0 ? y : 0.0f;
    const float up = y < 0 ? -y : 0.0f;
    pad->Set(PadDualshock2::Inputs::PAD_R_RIGHT, right);
    pad->Set(PadDualshock2::Inputs::PAD_R_LEFT, left);
    pad->Set(PadDualshock2::Inputs::PAD_R_DOWN, down);
    pad->Set(PadDualshock2::Inputs::PAD_R_UP, up);
    g_touchPadState[PadDualshock2::Inputs::PAD_R_RIGHT] = right > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_R_LEFT] = left > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_R_DOWN] = down > 0.01f;
    g_touchPadState[PadDualshock2::Inputs::PAD_R_UP] = up > 0.01f;
}

+ (void)configureControllerMacroInputMask:(uint32_t)inputMask
                             modifierMask:(uint32_t)modifierMask {
    ARMSX2ConfigureControllerMacroInput(inputMask, modifierMask);
}

+ (void)consumeControllerMacroInputMask:(uint32_t)inputMask {
    ARMSX2ConsumeControllerMacroInput(inputMask);
}

+ (void)triggerDeviceHapticLarge:(NSUInteger)large small:(NSUInteger)small {
    if (VMManager::IsEmulationOnlyMode())
        return;

    // GameEventHaptics is @MainActor-isolated; dispatch to the main queue.
    dispatch_async(dispatch_get_main_queue(), ^{
        [SwiftUIHost triggerDeviceHapticWithLarge:large small:small];
    });
}

+ (void)testControllerRumble {
    ARMSX2_iOSTestGamepadRumble();
}

// Gamepad button mapping
+ (void)startButtonCapture {
    s_capturedButton.store(-1);
    s_captureMode.store(true);
}

+ (void)stopButtonCapture {
    s_captureMode.store(false);
}

// Poll SDL gamepad from main thread (for settings screen when VM is not running)
+ (void)pollGamepadForCapture {
    if (!s_captureMode.load()) return;
    SDL_UpdateGamepads();
    // Keep gamepad open across polls to avoid open/close overhead
    static SDL_Gamepad* s_settingsGP = nullptr;
    if (!s_settingsGP) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count > 0) s_settingsGP = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (!s_settingsGP) return;
    if (!SDL_GamepadConnected(s_settingsGP)) {
        SDL_CloseGamepad(s_settingsGP);
        s_settingsGP = nullptr;
        return;
    }
    // SDL_PumpEvents required for GCController input to be processed
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (SDL_GetGamepadButton(s_settingsGP, (SDL_GamepadButton)b)) {
            s_capturedButton.store(b);
            break;
        }
    }
}

+ (int)capturedButton {
    return s_capturedButton.exchange(-1);
}

+ (void)setButtonMapping:(int)ps2Index toSDLButton:(int)sdlButton {
    if (ps2Index >= 0 && ps2Index < 16) {
        s_buttonMap[ps2Index] = sdlButton;
        // Persist to INI
        if (ARMSX2HasBaseSettings()) {
            char key[32];
            snprintf(key, sizeof(key), "Button%d", ps2Index);
            Host::SetBaseIntSettingValue("ARMSX2iOS/GamepadMapping", key, sdlButton);
            Host::CommitBaseSettingChanges();
        }
    }
}

+ (int)getButtonMapping:(int)ps2Index {
    if (ps2Index >= 0 && ps2Index < 16) return s_buttonMap[ps2Index];
    return -1;
}

+ (void)resetButtonMappings {
    static const int defMap[16] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        -1, -1,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    };
    for (int i = 0; i < 16; i++) s_buttonMap[i] = defMap[i];
    if (ARMSX2HasBaseSettings()) {
        ARMSX2EditBaseSettings([](SettingsInterface& si) { si.RemoveSection("ARMSX2iOS/GamepadMapping"); });
        Host::CommitBaseSettingChanges();
    }
}

@end
