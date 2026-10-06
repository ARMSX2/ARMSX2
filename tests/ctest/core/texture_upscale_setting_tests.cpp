// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The texture upscaling setting: EmuCore/GS TextureUpscale, 0 Off, 1 RAISR 2x, 2 RAISR 4x.
//
// The setting's first key was TextureUpscaleMode, where 2 meant Smooth 2x. That key is not read, so
// a leftover value of it cannot turn into a 4x choice, and anything outside 0..2 under the new key
// means Off.

#include <gtest/gtest.h>

#include "Config.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

namespace
{
	GSTextureUpscaleMode Load(MemorySettingsInterface& si)
	{
		Pcsx2Config::GSOptions gs;
		SettingsLoadWrapper wrap(si);
		gs.LoadSave(wrap);
		return gs.TextureUpscale;
	}

	GSTextureUpscaleMode LoadValue(s32 value)
	{
		MemorySettingsInterface si;
		si.SetIntValue("EmuCore/GS", "TextureUpscale", value);
		return Load(si);
	}
} // namespace

TEST(TextureUpscaleSetting, DefaultsToOff)
{
	EXPECT_EQ(Pcsx2Config::GSOptions().TextureUpscale, GSTextureUpscaleMode::Off);
	MemorySettingsInterface si;
	EXPECT_EQ(Load(si), GSTextureUpscaleMode::Off);
}

TEST(TextureUpscaleSetting, ReadsEachMode)
{
	EXPECT_EQ(LoadValue(0), GSTextureUpscaleMode::Off);
	EXPECT_EQ(LoadValue(1), GSTextureUpscaleMode::Raisr2x);
	EXPECT_EQ(LoadValue(2), GSTextureUpscaleMode::Raisr4x);
}

TEST(TextureUpscaleSetting, OutOfRangeValuesAreOff)
{
	// 256 and 257 are the ones a u8 cast would wrap onto Off and 2x.
	for (const s32 value : {3, 4, 255, 256, 257, 258, 65537, -1, -255, 2147483647, -2147483647 - 1})
		EXPECT_EQ(LoadValue(value), GSTextureUpscaleMode::Off) << value;
}

TEST(TextureUpscaleSetting, TheOldKeyIsNotRead)
{
	for (const s32 value : {1, 2, 3})
	{
		MemorySettingsInterface si;
		si.SetIntValue("EmuCore/GS", "TextureUpscaleMode", value);
		EXPECT_EQ(Load(si), GSTextureUpscaleMode::Off) << value;
	}

	// With both present the new key decides.
	MemorySettingsInterface si;
	si.SetIntValue("EmuCore/GS", "TextureUpscaleMode", 2);
	si.SetIntValue("EmuCore/GS", "TextureUpscale", 1);
	EXPECT_EQ(Load(si), GSTextureUpscaleMode::Raisr2x);
}

TEST(TextureUpscaleSetting, SavesUnderTheNewKeyOnly)
{
	for (const GSTextureUpscaleMode mode : {GSTextureUpscaleMode::Off, GSTextureUpscaleMode::Raisr2x, GSTextureUpscaleMode::Raisr4x})
	{
		Pcsx2Config::GSOptions gs;
		gs.TextureUpscale = mode;
		MemorySettingsInterface si;
		SettingsSaveWrapper wrap(si);
		gs.LoadSave(wrap);

		s32 saved = -1;
		ASSERT_TRUE(si.GetIntValue("EmuCore/GS", "TextureUpscale", &saved));
		EXPECT_EQ(saved, static_cast<s32>(mode));
		EXPECT_FALSE(si.ContainsValue("EmuCore/GS", "TextureUpscaleMode"));
		EXPECT_EQ(Load(si), mode);
	}
}

TEST(TextureUpscaleSetting, ChangingItIsASettingsChange)
{
	// The renderer purges its texture cache and swaps filters when this differs between two
	// configs, so OptionsAreEqual has to see it.
	Pcsx2Config::GSOptions a, b;
	EXPECT_TRUE(a.OptionsAreEqual(b));
	b.TextureUpscale = GSTextureUpscaleMode::Raisr2x;
	EXPECT_FALSE(a.OptionsAreEqual(b));
	a.TextureUpscale = GSTextureUpscaleMode::Raisr2x;
	EXPECT_TRUE(a.OptionsAreEqual(b));
	b.TextureUpscale = GSTextureUpscaleMode::Raisr4x;
	EXPECT_FALSE(a.OptionsAreEqual(b));
}
