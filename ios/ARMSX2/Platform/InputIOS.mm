//
//  InputIOS.mm
//  ARMSX2
//
//  iOS Input implementation using GameController framework
//

#import "InputIOS.h"
#import <GameController/GameController.h>
#include <map>
#include <mutex>
#include <vector>

// Controller state
struct ControllerState {
    bool buttons[16];
    float leftStickX;
    float leftStickY;
    float rightStickX;
    float rightStickY;
    float l2;
    float r2;
};

static std::map<int, ControllerState> s_controller_states;
static std::vector<GCController*> s_controllers;
static std::mutex s_input_mutex;
static bool s_initialized = false;

// Controller connection notifications
static void OnControllerConnected(GCController* controller)
{
    NSLog(@"[ARMSX2-Input] Controller connected: %@", controller.vendorName ?: @"Unknown");

    std::lock_guard<std::mutex> lock(s_input_mutex);
    s_controllers.push_back(controller);

    // Initialize state
    int index = (int)s_controllers.size() - 1;
    s_controller_states[index] = ControllerState{};
}

static void OnControllerDisconnected(GCController* controller)
{
    NSLog(@"[ARMSX2-Input] Controller disconnected: %@", controller.vendorName ?: @"Unknown");

    std::lock_guard<std::mutex> lock(s_input_mutex);

    auto it = std::find(s_controllers.begin(), s_controllers.end(), controller);
    if (it != s_controllers.end()) {
        int index = (int)(it - s_controllers.begin());
        s_controllers.erase(it);
        s_controller_states.erase(index);

        // Reindex remaining controllers
        std::map<int, ControllerState> newStates;
        for (size_t i = 0; i < s_controllers.size(); i++) {
            if (s_controller_states.count(i + 1)) {
                newStates[i] = s_controller_states[i + 1];
            }
        }
        s_controller_states = newStates;
    }
}

// Map MFi controller input to PS2 buttons
static void MapControllerInput(int index, GCController* controller)
{
    if (!controller) return;

    auto& state = s_controller_states[index];

    // Extended gamepad (most modern controllers)
    if (controller.extendedGamepad) {
        GCExtendedGamepad* gamepad = controller.extendedGamepad;

        // D-Pad
        state.buttons[PS2Button_DPad_Up] = gamepad.dpad.up.pressed;
        state.buttons[PS2Button_DPad_Down] = gamepad.dpad.down.pressed;
        state.buttons[PS2Button_DPad_Left] = gamepad.dpad.left.pressed;
        state.buttons[PS2Button_DPad_Right] = gamepad.dpad.right.pressed;

        // Face buttons (map to PS2 buttons)
        state.buttons[PS2Button_Cross] = gamepad.buttonA.pressed;      // A -> Cross
        state.buttons[PS2Button_Circle] = gamepad.buttonB.pressed;     // B -> Circle
        state.buttons[PS2Button_Square] = gamepad.buttonX.pressed;     // X -> Square
        state.buttons[PS2Button_Triangle] = gamepad.buttonY.pressed;   // Y -> Triangle

        // Shoulder buttons
        state.buttons[PS2Button_L1] = gamepad.leftShoulder.pressed;
        state.buttons[PS2Button_R1] = gamepad.rightShoulder.pressed;

        // Triggers
        state.l2 = gamepad.leftTrigger.value;
        state.r2 = gamepad.rightTrigger.value;
        state.buttons[PS2Button_L2] = state.l2 > 0.5f;
        state.buttons[PS2Button_R2] = state.r2 > 0.5f;

        // Analog sticks
        state.leftStickX = gamepad.leftThumbstick.xAxis.value;
        state.leftStickY = gamepad.leftThumbstick.yAxis.value;
        state.rightStickX = gamepad.rightThumbstick.xAxis.value;
        state.rightStickY = gamepad.rightThumbstick.yAxis.value;

        // Stick buttons (L3/R3)
        if (gamepad.leftThumbstickButton) {
            state.buttons[PS2Button_L3] = gamepad.leftThumbstickButton.pressed;
        }
        if (gamepad.rightThumbstickButton) {
            state.buttons[PS2Button_R3] = gamepad.rightThumbstickButton.pressed;
        }

        // Menu buttons
        if (gamepad.buttonMenu) {
            state.buttons[PS2Button_Start] = gamepad.buttonMenu.pressed;
        }
        if (gamepad.buttonOptions) {
            state.buttons[PS2Button_Select] = gamepad.buttonOptions.pressed;
        }
    }
    // Standard gamepad (older controllers)
    else if (controller.microGamepad) {
        GCMicroGamepad* gamepad = controller.microGamepad;

        state.buttons[PS2Button_Cross] = gamepad.buttonA.pressed;
        state.buttons[PS2Button_Circle] = gamepad.buttonX.pressed;

        // Use touchpad as d-pad
        state.buttons[PS2Button_DPad_Up] = gamepad.dpad.up.pressed;
        state.buttons[PS2Button_DPad_Down] = gamepad.dpad.down.pressed;
        state.buttons[PS2Button_DPad_Left] = gamepad.dpad.left.pressed;
        state.buttons[PS2Button_DPad_Right] = gamepad.dpad.right.pressed;
    }
}

extern "C" {

void iOS_Input_Initialize()
{
    if (s_initialized) return;

    NSLog(@"[ARMSX2-Input] Initializing iOS input system...");

    // Register for controller notifications
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification
                                                       object:nil
                                                        queue:[NSOperationQueue mainQueue]
                                                   usingBlock:^(NSNotification* note) {
        GCController* controller = note.object;
        OnControllerConnected(controller);
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidDisconnectNotification
                                                       object:nil
                                                        queue:[NSOperationQueue mainQueue]
                                                   usingBlock:^(NSNotification* note) {
        GCController* controller = note.object;
        OnControllerDisconnected(controller);
    }];

    // Add already connected controllers
    for (GCController* controller in [GCController controllers]) {
        OnControllerConnected(controller);
    }

    // Start wireless controller discovery
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:^{
        NSLog(@"[ARMSX2-Input] Wireless controller discovery completed");
    }];

    s_initialized = true;
    NSLog(@"[ARMSX2-Input] Input system initialized, %zu controller(s) connected",
          s_controllers.size());
}

void iOS_Input_Shutdown()
{
    if (!s_initialized) return;

    NSLog(@"[ARMSX2-Input] Shutting down input system...");

    [[NSNotificationCenter defaultCenter] removeObserver:nil
                                                    name:GCControllerDidConnectNotification
                                                  object:nil];
    [[NSNotificationCenter defaultCenter] removeObserver:nil
                                                    name:GCControllerDidDisconnectNotification
                                                  object:nil];

    [GCController stopWirelessControllerDiscovery];

    std::lock_guard<std::mutex> lock(s_input_mutex);
    s_controllers.clear();
    s_controller_states.clear();
    s_initialized = false;

    NSLog(@"[ARMSX2-Input] Input system shut down");
}

void iOS_Input_Update()
{
    if (!s_initialized) return;

    std::lock_guard<std::mutex> lock(s_input_mutex);

    // Update all controller states
    for (size_t i = 0; i < s_controllers.size(); i++) {
        MapControllerInput((int)i, s_controllers[i]);
    }
}

void iOS_Input_SetButton(int controller, PS2Button button, bool pressed)
{
    std::lock_guard<std::mutex> lock(s_input_mutex);

    if (s_controller_states.count(controller)) {
        s_controller_states[controller].buttons[button] = pressed;
    }
}

void iOS_Input_SetLeftStick(int controller, float x, float y)
{
    std::lock_guard<std::mutex> lock(s_input_mutex);

    if (s_controller_states.count(controller)) {
        s_controller_states[controller].leftStickX = x;
        s_controller_states[controller].leftStickY = y;
    }
}

void iOS_Input_SetRightStick(int controller, float x, float y)
{
    std::lock_guard<std::mutex> lock(s_input_mutex);

    if (s_controller_states.count(controller)) {
        s_controller_states[controller].rightStickX = x;
        s_controller_states[controller].rightStickY = y;
    }
}

void iOS_Input_SetL2Trigger(int controller, float value)
{
    std::lock_guard<std::mutex> lock(s_input_mutex);

    if (s_controller_states.count(controller)) {
        s_controller_states[controller].l2 = value;
        s_controller_states[controller].buttons[PS2Button_L2] = value > 0.5f;
    }
}

void iOS_Input_SetR2Trigger(int controller, float value)
{
    std::lock_guard<std::mutex> lock(s_input_mutex);

    if (s_controller_states.count(controller)) {
        s_controller_states[controller].r2 = value;
        s_controller_states[controller].buttons[PS2Button_R2] = value > 0.5f;
    }
}

bool iOS_Input_GetButton(int controller, PS2Button button)
{
    std::lock_guard<std::mutex> lock(s_input_mutex);

    if (s_controller_states.count(controller)) {
        return s_controller_states[controller].buttons[button];
    }
    return false;
}

int iOS_Input_GetControllerCount()
{
    std::lock_guard<std::mutex> lock(s_input_mutex);
    return (int)s_controllers.size();
}

} // extern "C"
