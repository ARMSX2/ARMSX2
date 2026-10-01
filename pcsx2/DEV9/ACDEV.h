// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade hardware, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. Only reached while an arcade game runs (Arcade::IsActive(), common/ARCADE.h).

#pragma once

// Base of the ACDEV area
#define ACDEV_BASE SPD_REGBASE

// from ACDEV_BASE up to this point, rom0:ROMDRV will look for a ROMFS bios filesystem, as requested by rom0:ACDEV
#define ACDEV_ROMDIR_POKE_END (ACDEV_BASE + 0x8000)