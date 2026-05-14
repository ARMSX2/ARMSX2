// Link-time stub implementations for symbols referenced by PadDualshock2.cpp
// that are not exercised in the pressure modifier tests.
#include "fmt/format.h"
#include "pcsx2/Config.h"
#include "SIO/Sio.h"
#include "Input/InputManager.h"
#include "common/Pcsx2Types.h"
#include <tuple>
#include <cstdlib>
#include <cstring>

// -- sioConvertPadToPortAndSlot -------------------------------------------
std::tuple<u32, u32> sioConvertPadToPortAndSlot(u32 index)
{
    return { 0u, 0u };
}

// -- SettingInfo helpers (used when GetInfo() is queried) ------------------
const char* SettingInfo::StringDefaultValue() const  { return default_value ? default_value : ""; }
bool        SettingInfo::BooleanDefaultValue() const { return default_value && std::strcmp(default_value, "true") == 0; }
s32         SettingInfo::IntegerDefaultValue() const { return default_value ? static_cast<s32>(std::atoi(default_value)) : 0; }
s32         SettingInfo::IntegerMinValue()     const { return min_value ? static_cast<s32>(std::atoi(min_value)) : 0; }
s32         SettingInfo::IntegerMaxValue()     const { return max_value ? static_cast<s32>(std::atoi(max_value)) : 100; }
s32         SettingInfo::IntegerStepValue()    const { return step_value ? static_cast<s32>(std::atoi(step_value)) : 1; }
float       SettingInfo::FloatDefaultValue()   const { return default_value ? std::strtof(default_value, nullptr) : 0.0f; }
float       SettingInfo::FloatMinValue()       const { return min_value   ? std::strtof(min_value, nullptr)   : 0.0f; }
float       SettingInfo::FloatMaxValue()       const { return max_value   ? std::strtof(max_value, nullptr)   : 1.0f; }
float       SettingInfo::FloatStepValue()      const { return step_value  ? std::strtof(step_value, nullptr)  : 0.01f; }
void        SettingInfo::SetDefaultValue(SettingsInterface*, const char*, const char*) const {}
void        SettingInfo::CopyValue(SettingsInterface*, const SettingsInterface&, const char*, const char*) const {}

// -- Hotkey list stubs -----------------------------------------------------
const HotkeyInfo g_common_hotkeys[] = {{ nullptr, nullptr, nullptr, nullptr }};
const HotkeyInfo g_gs_hotkeys[]     = {{ nullptr, nullptr, nullptr, nullptr }};
const HotkeyInfo g_host_hotkeys[]   = {{ nullptr, nullptr, nullptr, nullptr }};
