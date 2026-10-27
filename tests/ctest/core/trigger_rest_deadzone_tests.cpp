// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The desktop input sources trim the bottom of an analog trigger's travel before it reaches any binding,
// the same way the Android frontend does (sendTrigger/shapeTrigger in MainActivityRuntime.kt). Without it a
// pad whose triggers rest a little above zero holds L2/R2 forever: the Retroid Pocket Flip 2 under ROCKNIX
// rests R2 at 12/255 and L2 at 6/255, and Shadow of the Colossus sat zoomed in because R2 is its zoom.

#include "Input/InputManager.h"

#include <gtest/gtest.h>

namespace
{
	// The Android frontend's formula, written out the way MainActivityRuntime.kt evaluates it, in float.
	float AndroidShapeTrigger(float raw)
	{
		constexpr float TRIGGER_DEAD = 0.06f;
		return (raw <= TRIGGER_DEAD) ? 0.0f : (raw - TRIGGER_DEAD) / (1.0f - TRIGGER_DEAD);
	}
} // namespace

TEST(TriggerRestDeadzone, RestingTriggerReadsAsReleased)
{
	EXPECT_EQ(InputManager::ApplyTriggerRestDeadzone(0.0f), 0.0f);
	EXPECT_EQ(InputManager::ApplyTriggerRestDeadzone(12.0f / 255.0f), 0.0f); // Flip 2 R2 at rest
	EXPECT_EQ(InputManager::ApplyTriggerRestDeadzone(6.0f / 255.0f), 0.0f); // Flip 2 L2 at rest
	EXPECT_EQ(InputManager::ApplyTriggerRestDeadzone(0.06f), 0.0f);
}

TEST(TriggerRestDeadzone, FullTravelStaysFull)
{
	EXPECT_EQ(InputManager::ApplyTriggerRestDeadzone(1.0f), 1.0f);
}

TEST(TriggerRestDeadzone, OutOfRangeInputIsClamped)
{
	EXPECT_EQ(InputManager::ApplyTriggerRestDeadzone(-0.5f), 0.0f);
	EXPECT_EQ(InputManager::ApplyTriggerRestDeadzone(1.5f), 1.0f);
}

TEST(TriggerRestDeadzone, MatchesAndroidAcrossTheTravel)
{
	// Every value an SDL trigger axis can report (0..32767), and every value an XInput trigger can (0..255).
	for (int v = 0; v <= 32767; v++)
	{
		const float raw = static_cast<float>(v) / 32767.0f;
		ASSERT_EQ(InputManager::ApplyTriggerRestDeadzone(raw), AndroidShapeTrigger(raw)) << "SDL value " << v;
	}
	for (int v = 0; v <= 255; v++)
	{
		const float raw = static_cast<float>(v) / 255.0f;
		ASSERT_EQ(InputManager::ApplyTriggerRestDeadzone(raw), AndroidShapeTrigger(raw)) << "XInput value " << v;
	}
}

TEST(TriggerRestDeadzone, NeverDecreasesAsTheTriggerIsPulled)
{
	float last = 0.0f;
	for (int v = 0; v <= 32767; v++)
	{
		const float out = InputManager::ApplyTriggerRestDeadzone(static_cast<float>(v) / 32767.0f);
		ASSERT_GE(out, last) << "SDL value " << v;
		last = out;
	}
}
