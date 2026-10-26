// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "USB/deviceproxy.h"

#include <string_view>

namespace usb_lightgun
{
	class GunCon2Device final : public DeviceProxy
	{
	public:
		USBDevice* CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const override;
		const char* Name() const override;
		const char* TypeName() const override;
		const char* IconName() const override;
		bool Freeze(USBDevice* dev, StateWrapper& sw) const override;
		void UpdateSettings(USBDevice* dev, SettingsInterface& si) const override;
		float GetBindingValue(const USBDevice* dev, u32 bind_index) const override;
		void SetBindingValue(USBDevice* dev, u32 bind_index, float value) const override;
		std::span<const InputBindingInfo> Bindings(u32 subtype) const override;
		std::span<const SettingInfo> Settings(u32 subtype) const override;
	};

	// A frontend that drives each port's gun itself (for example VR controllers as light guns),
	// instead of through the shared mouse pointer and input bindings. Buttons are a mask of these bits.
	enum HostButton : u32
	{
		HOST_C = 1u << 1,
		HOST_B = 1u << 2,
		HOST_A = 1u << 3,
		HOST_DPAD_UP = 1u << 4,
		HOST_DPAD_RIGHT = 1u << 5,
		HOST_DPAD_DOWN = 1u << 6,
		HOST_DPAD_LEFT = 1u << 7,
		HOST_TRIGGER = 1u << 13,
		HOST_SELECT = 1u << 14,
		HOST_START = 1u << 15,
		HOST_SHOOT_OFFSCREEN = 1u << 16,
		HOST_RECALIBRATE = 1u << 17,
	};
	// While active, the gun on `port` aims at (window_x, window_y), in the same window pixels as the
	// mouse pointer (off the picture when !aim_valid), with `buttons` held. Any thread; the gun reads
	// it whenever the game polls it, so it survives the device being plugged in or out.
	void SetHostState(u32 port, bool active, bool aim_valid, float window_x, float window_y, u32 buttons);
	// How many times the game has set the parameters of the gun on `port` (Set Param: its own
	// calibration offset, which games send after a calibration shot, and some at start), and the
	// last offset. Any thread.
	u32 GetGameParams(u32 port, s16* x, s16* y);
	// Whether the game is a known GunCon 2 game (it has a screen calibration entry).
	bool IsKnownGame(std::string_view serial);
} // namespace usb_lightgun
