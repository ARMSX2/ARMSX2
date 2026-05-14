// Host-build stub: minimal StateWrapper so PadBase.h and PadDualshock2 compile
#pragma once
#include "common/Pcsx2Defs.h"
#include <string_view>

class StateWrapper
{
public:
    enum class Mode { Read, Write };
    StateWrapper() = default;
    bool IsReading()  const { return false; }
    bool IsWriting()  const { return false; }
    bool HasError()   const { return false; }
    bool DoMarker(const char*) { return true; }
    template<typename T> void Do(T*) {}
    template<typename T> void DoArray(T*, size_t) {}
    template<typename T, size_t N> void DoArray(T (&)[N]) {}
    template<typename T> void DoBitfield(T*) {}
    template<typename T> void DoEnum(T*) {}
};
