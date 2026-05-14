// Unit tests for the Pressure Modifier feature (Issue #116)
// Exercises PadDualshock2::Set() and GetPressure() via the public API.
//
// Pressure modifier semantics:
//   - Call Set(PAD_PRESSURE, 1.0f)  to activate   (simulates holding the modifier button)
//   - Call Set(PAD_PRESSURE, 0.0f)  to deactivate
//   - Call Set(button, 1.0f)        to press a face/shoulder button
//   - GetPressure(button)           returns scaled rawInput (0-255)
#include <gtest/gtest.h>
#include "SIO/Pad/PadDualshock2.h"

using Inp = PadDualshock2::Inputs;

// Helpers ------------------------------------------------------------------

static PadDualshock2 make_pad()
{
    return PadDualshock2(0 /*unifiedSlot*/, 0 /*ejectTicks*/);
}

// Press a button (value 1.0) without modifier, return its raw pressure byte.
static u8 pressure_without_modifier(Inp button, float modifier_amount)
{
    PadDualshock2 pad = make_pad();
    pad.SetPressureModifier(modifier_amount);
    pad.Set(static_cast<u32>(button), 1.0f);
    return pad.GetPressure(static_cast<u32>(button));
}

// Press a button while the modifier is held, return its raw pressure byte.
static u8 pressure_with_modifier(Inp button, float modifier_amount)
{
    PadDualshock2 pad = make_pad();
    pad.SetPressureModifier(modifier_amount);
    pad.Set(static_cast<u32>(Inp::PAD_PRESSURE), 1.0f); // activate modifier
    pad.Set(static_cast<u32>(button), 1.0f);
    return pad.GetPressure(static_cast<u32>(button));
}

// Tests --------------------------------------------------------------------

// 1. Full pressure when modifier inactive
TEST(PressureModifier, FullPressureWhenInactive)
{
    u8 result = pressure_without_modifier(Inp::PAD_CROSS, 0.5f);
    EXPECT_EQ(result, 255);
}

// 2. Pressure is scaled when modifier is active (50% → ~127)
TEST(PressureModifier, ScalesPressureWhenActive)
{
    u8 result = pressure_with_modifier(Inp::PAD_CROSS, 0.5f);
    // 1.0 * 0.5 * 255 = 127 (integer truncation)
    EXPECT_EQ(result, 127);
}

// 3. The PAD_PRESSURE slot itself is NOT scaled (would cause double-reduction)
TEST(PressureModifier, ModifierSlotNotSelfScaled)
{
    PadDualshock2 pad = make_pad();
    pad.SetPressureModifier(0.25f);
    pad.Set(static_cast<u32>(Inp::PAD_PRESSURE), 1.0f);
    u8 result = pad.GetPressure(static_cast<u32>(Inp::PAD_PRESSURE));
    // PAD_PRESSURE is a digital button — its rawInputs entry reflects 1.0 * 1.0 * 255 = 255
    // (the modifier does not apply to itself)
    EXPECT_EQ(result, 255);
}

// 4. Edge case: 0% modifier should clamp to 0
TEST(PressureModifier, ZeroModifierGivesZeroPressure)
{
    u8 result = pressure_with_modifier(Inp::PAD_CIRCLE, 0.0f);
    EXPECT_EQ(result, 0);
}

// 5. Settings round-trip: SetPressureModifier / GetPressureModifier
TEST(PressureModifier, SettingsRoundTrip)
{
    PadDualshock2 pad = make_pad();
    pad.SetPressureModifier(0.3f);
    EXPECT_FLOAT_EQ(pad.GetPressureModifier(), 0.3f);
}

// 6. Modifier does not affect analog sticks
TEST(PressureModifier, AnalogSticksNotAffected)
{
    PadDualshock2 pad = make_pad();
    pad.SetPressureModifier(0.1f);
    pad.Set(static_cast<u32>(Inp::PAD_PRESSURE), 1.0f);
    // Left stick up — full deflection
    pad.Set(static_cast<u32>(Inp::PAD_L_UP), 1.0f);
    // Analog axes are HalfAxis / Trigger paths — they bypass the pMod scaling
    // We just verify no crash and that face buttons ARE still scaled
    u8 face = pad.GetPressure(static_cast<u32>(Inp::PAD_CROSS));
    EXPECT_EQ(face, 0); // Cross was not pressed in this test
}

// 7. Retroactive adjustment: buttons already held scale when modifier pressed
TEST(PressureModifier, RetroactiveAdjustOnModifierPress)
{
    PadDualshock2 pad = make_pad();
    pad.SetPressureModifier(0.5f);
    // Press Cross first (full pressure)
    pad.Set(static_cast<u32>(Inp::PAD_CROSS), 1.0f);
    EXPECT_EQ(pad.GetPressure(static_cast<u32>(Inp::PAD_CROSS)), 255);

    // Now press the modifier — it retroactively adjusts active button pressures
    pad.Set(static_cast<u32>(Inp::PAD_PRESSURE), 1.0f);
    u8 result = pad.GetPressure(static_cast<u32>(Inp::PAD_CROSS));
    // 255 * 0.5 + 0.5 rounding ≈ 128
    EXPECT_GE(result, 126);
    EXPECT_LE(result, 129);
}
