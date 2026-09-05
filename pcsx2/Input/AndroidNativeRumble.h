// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef __ANDROID__

// The one declaration of the JNI rumble bridge, so the core's caller and every
// frontend that has to define it agree on the signature.
//
// The Android pad is fed by the custom JNI input path rather than by an input
// source with motor bindings, so InputManager's vibration array is empty and
// its normal loop never reaches a motor. Intensity changes go straight here
// instead; the APK's native-lib.cpp routes them to that player's controller (or
// the handheld's own haptic), and a frontend with no JVM -- pcsx2-gsrunner
// under adb shell -- stubs it out.
//
// pad is the unified slot (0 = P1, 1 = P2); the motors are 0..255.
namespace Native
{
	void onPadRumble(int pad, int largeMotor, int smallMotor);
}

#endif
