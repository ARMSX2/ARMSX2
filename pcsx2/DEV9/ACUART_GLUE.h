// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#pragma once
#include "common/Pcsx2Types.h"
#include <string>

class ACUARTDevice
{
public:
    virtual ~ACUARTDevice() = default;

    virtual void Reset() {}
    virtual void Init() {}
    virtual void TxByte(u8 value) {}
    virtual bool RxByte(u8& value){return false;}
    virtual void Tick(u32 cycles) {}
    virtual bool HasData() const {return false;}
};