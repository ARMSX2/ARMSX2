// Host-build stub: replaces architecture-specific FPControl.h
#pragma once
#include "common/Pcsx2Types.h"

struct FPControlRegister
{
    u32 bitmask = 0;
    static FPControlRegister GetCurrent() { return {}; }
    static void SetCurrent(FPControlRegister) {}
};

struct FPControlRegisterBackup
{
    FPControlRegister m_prev_val;
    explicit FPControlRegisterBackup(FPControlRegister r) : m_prev_val(r) {}
    ~FPControlRegisterBackup() {}
};
