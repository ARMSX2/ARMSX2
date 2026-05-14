// Host-build stub: minimal InputManager for PAD unit tests
#pragma once
#include "common/Pcsx2Types.h"
#include "pcsx2/Config.h"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class InputSourceType : u32 { Keyboard, Pointer, SDL, Count };
enum class InputSubclass   : u32 { None = 0, PointerButton = 0, PointerAxis = 1 };
enum class InputModifier   : u32 { None = 0, Negate, FullAxis };

union InputBindingKey
{
    struct
    {
        InputSourceType source_type   : 4;
        u32 source_index              : 8;
        InputSubclass source_subtype  : 3;
        InputModifier modifier        : 2;
        u32 invert                    : 1;
        u32 needs_migration           : 1;
        u32 unused                    : 13;
        u32 data;
    };
    u64 bits = 0;

    bool operator==(const InputBindingKey& k) const { return bits == k.bits; }
    bool operator!=(const InputBindingKey& k) const { return bits != k.bits; }
    InputBindingKey MaskDirection() const { InputBindingKey r; r.bits = bits; return r; }
};

using InputButtonEventHandler = std::function<void(s32)>;
using InputAxisEventHandler   = std::function<void(InputBindingKey, float)>;

using GenericInputBindingMapping = std::vector<std::pair<GenericInputBinding, std::string>>;

struct HotkeyInfo { const char* name; const char* category; const char* display_name; void (*handler)(s32); };
#define DECLARE_HOTKEY_LIST(name) extern const HotkeyInfo name[]
DECLARE_HOTKEY_LIST(g_common_hotkeys);
DECLARE_HOTKEY_LIST(g_gs_hotkeys);
DECLARE_HOTKEY_LIST(g_host_hotkeys);

class InputSource {};

namespace InputManager
{
    inline GenericInputBindingMapping GetGenericBindingMapping(std::string_view) { return {}; }
    inline const char* InputSourceToString(InputSourceType) { return ""; }
    inline bool GetInputSourceDefaultEnabled(InputSourceType) { return false; }
    inline std::optional<InputSourceType> ParseInputSourceString(std::string_view) { return {}; }
    inline void SetPadVibrationIntensity(u32, float, float) {}
}
