// GamepadInternal.h — state and helpers the gamepad files share
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#import <GameController/GameController.h>

#include <array>
#include <atomic>

#include <SDL3/SDL.h>

#include "IOSRuntime.h"

static constexpr u16 ARMSX2_GAMEPAD_RUMBLE_MAX_INTENSITY = 0x7000;

// Physical input bits intentionally match ARMSX2PadButton. Swift publishes
// the buttons which participate in user-configured chords, while this CPU-
// thread gate prevents SDL from delivering the same edge to the emulated pad.
enum : u32
{
    ARMSX2_MACRO_INPUT_UP = 1u << 0,
    ARMSX2_MACRO_INPUT_DOWN = 1u << 1,
    ARMSX2_MACRO_INPUT_LEFT = 1u << 2,
    ARMSX2_MACRO_INPUT_RIGHT = 1u << 3,
    ARMSX2_MACRO_INPUT_CROSS = 1u << 4,
    ARMSX2_MACRO_INPUT_CIRCLE = 1u << 5,
    ARMSX2_MACRO_INPUT_SQUARE = 1u << 6,
    ARMSX2_MACRO_INPUT_TRIANGLE = 1u << 7,
    ARMSX2_MACRO_INPUT_L1 = 1u << 8,
    ARMSX2_MACRO_INPUT_R1 = 1u << 9,
    ARMSX2_MACRO_INPUT_L2 = 1u << 10,
    ARMSX2_MACRO_INPUT_R2 = 1u << 11,
    ARMSX2_MACRO_INPUT_START = 1u << 12,
    ARMSX2_MACRO_INPUT_SELECT = 1u << 13,
    ARMSX2_MACRO_INPUT_L3 = 1u << 14,
    ARMSX2_MACRO_INPUT_R3 = 1u << 15,
    ARMSX2_MACRO_INPUT_VALID_MASK = 0xffffu,
};

struct ARMSX2ControllerMacroInputGateState
{
    u32 previous_raw_mask = 0;
    u32 consumed_until_release_mask = 0;
    u32 delivered_mask = 0;
    u32 replay_eligible_mask = 0;
    u32 current_replay_mask = 0;
    std::array<u64, 16> press_started_ms{};
    std::array<u8, 16> replay_frames{};
};

enum : u8
{
    ARMSX2_NATIVE_DPAD_UP = 1 << 0,
    ARMSX2_NATIVE_DPAD_DOWN = 1 << 1,
    ARMSX2_NATIVE_DPAD_LEFT = 1 << 2,
    ARMSX2_NATIVE_DPAD_RIGHT = 1 << 3,
};

enum class ARMSX2IOSMultitapMode : int
{
    Auto = 0,
    Disabled = 1,
    Port1 = 2,
    Port2 = 3,
    Both = 4,
};

extern std::atomic<u32> s_pendingGamepadRumble[ARMSX2_MAX_IOS_GAMEPADS];
extern std::atomic<u64> s_gamepadRumbleStopDeadlineMs[ARMSX2_MAX_IOS_GAMEPADS];
extern std::atomic<bool> s_gamepadIsJoyCon[ARMSX2_MAX_IOS_GAMEPADS];
extern std::atomic<bool> s_gamepadRumbleTestRequested;
extern u32 s_appliedGamepadRumble[ARMSX2_MAX_IOS_GAMEPADS];
extern bool s_appliedGamepadRumbleValid[ARMSX2_MAX_IOS_GAMEPADS];
extern bool s_loggedMultitapRestartNeeded;
extern std::atomic<u8> s_nativeGamepadDpadMask[ARMSX2_MAX_IOS_GAMEPADS];
extern std::atomic<u8> s_nativeGamepadDpadLatchedMask[ARMSX2_MAX_IOS_GAMEPADS];
extern std::atomic<u8> s_nativeGamepadAnyDpadMask;
extern std::atomic<u8> s_nativeGamepadAnyDpadLatchedMask;
extern std::atomic<u32> s_controllerMacroConsumedMask[ARMSX2_MAX_IOS_GAMEPADS];
extern ARMSX2ControllerMacroInputGateState s_controllerMacroInputGateStates[ARMSX2_MAX_IOS_GAMEPADS];
extern std::atomic<int> s_nativeHapticSourceGamepad;
extern std::atomic<u32> s_nativeHapticResyncSlotMask;

u32 ARMSX2ControllerMacroBitForSDLButton(SDL_GamepadButton button);
u32 ARMSX2RawControllerMacroPhysicalInputMask(SDL_Gamepad* gamepad, u8 native_dpad_mask);
u32 ARMSX2ResolveControllerMacroGameplayMask(u32 gamepad_index, u32 raw_mask);
u32 ARMSX2ControllerMacroCurrentReplayMask(u32 gamepad_index);
u8 ARMSX2NativeDpadBitForPS2Button(u32 ps2_button);
void ARMSX2RecomputeNativeGamepadAnyDpadMask();
void ARMSX2RecomputeNativeGamepadAnyDpadLatchedMask();
void ARMSX2PollNativeGamepadDpadMasks(const char* reason);
void ARMSX2RefreshNativeGamepadDpadHandlersOnMain(const char* reason);
ARMSX2IOSMultitapMode ARMSX2GetIOSMultitapMode();
u32 ARMSX2ConnectedGamepadCount();
void ARMSX2ResetNativeGamepadRumbleOnMain();
void ARMSX2StopNativeGamepadRumblePulseOnMain(u32 slot);
bool ARMSX2SDLGamepadLooksLikeJoyCon(SDL_Gamepad* gamepad);
void ARMSX2ServiceGamepadRumbleTest();
u32 ARMSX2PackGamepadRumble(float large_intensity, float small_intensity);
float ARMSX2RumbleLargeIntensity(u32 packed);
float ARMSX2RumbleSmallIntensity(u32 packed);
float ARMSX2GameRumbleStrength();
u16 ARMSX2ScaleControllerRumbleMotor(u16 raw_intensity, float strength);
u32 ARMSX2ScaleNativeRumble(u32 packed, float strength);
GCController* ARMSX2FindNativeHapticController();
bool ARMSX2DeviceSupportsHaptics();
void ARMSX2ApplyNativeGamepadRumbleOnMain(u32 packed);
