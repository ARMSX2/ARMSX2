//
//  InputIOS.h
//  ARMSX2
//
//  iOS Input handling for PCSX2
//

#ifndef InputIOS_h
#define InputIOS_h

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#ifdef __cplusplus
extern "C" {
#endif

/// PS2 Controller buttons
typedef NS_ENUM(NSInteger, PS2Button) {
    PS2Button_Triangle = 0,
    PS2Button_Circle = 1,
    PS2Button_Cross = 2,
    PS2Button_Square = 3,
    PS2Button_L1 = 4,
    PS2Button_R1 = 5,
    PS2Button_L2 = 6,
    PS2Button_R2 = 7,
    PS2Button_L3 = 8,
    PS2Button_R3 = 9,
    PS2Button_Select = 10,
    PS2Button_Start = 11,
    PS2Button_DPad_Up = 12,
    PS2Button_DPad_Down = 13,
    PS2Button_DPad_Left = 14,
    PS2Button_DPad_Right = 15
};

/// Initialize iOS input system
void iOS_Input_Initialize(void);

/// Shutdown iOS input system
void iOS_Input_Shutdown(void);

/// Update input state (call every frame)
void iOS_Input_Update(void);

/// Set button state
void iOS_Input_SetButton(int controller, PS2Button button, bool pressed);

/// Set analog stick state (-1.0 to 1.0)
void iOS_Input_SetLeftStick(int controller, float x, float y);
void iOS_Input_SetRightStick(int controller, float x, float y);

/// Set trigger state (0.0 to 1.0)
void iOS_Input_SetL2Trigger(int controller, float value);
void iOS_Input_SetR2Trigger(int controller, float value);

/// Get button state
bool iOS_Input_GetButton(int controller, PS2Button button);

/// Get number of connected controllers
int iOS_Input_GetControllerCount(void);

#ifdef __cplusplus
}
#endif

#endif /* InputIOS_h */
