// Host-build stub
#pragma once
#include "common/Pcsx2Types.h"

struct Sio0Stub
{
    void SetAcknowledge(bool) {}
};
inline Sio0Stub g_Sio0;
