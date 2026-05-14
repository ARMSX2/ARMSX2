// Host-build stub: replaces Common.h without pulling in the PS2 emulator core
#pragma once
#include "common/Pcsx2Defs.h"
#include <algorithm>
#include <string>
#include <string_view>
#include "fmt/format.h"

struct ConsoleImpl
{
    void WriteLn(std::string)      {}
    void WriteLn(std::string_view) {}
    void WriteLn(const char*)      {}
    template<typename... Args>
    void Warning(fmt::format_string<Args...>, Args&&...) {}
    template<typename... Args>
    void Error(fmt::format_string<Args...>, Args&&...)   {}
};
inline ConsoleImpl Console;
