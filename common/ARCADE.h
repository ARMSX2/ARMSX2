// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 arcade emulation, ported from PCSX2x6
// (https://github.com/PS2Homebrew-arcade/pcsx2x6) by Matías Israelson (El_isra), Tovarichtch and
// DiscoStarslayer. In PCSX2x6 the whole emulator is an arcade board; here a retail PS2 runs by
// default and the board only exists for the length of an arcade session, behind Arcade::IsActive().

#pragma once
#include <atomic>
#include <string>
#include <string_view>

enum ACMEDIATYPE {
	ACUNK = -1, //Unknown
	ACCD = 0,
	ACDVD,
	ACHDD,
};

#define ACMEDIATYPE_FROM_STRING(s) \
	((s) == "CD"  ? ACMEDIATYPE::ACCD : \
	(s) == "DVD" ? ACMEDIATYPE::ACDVD : \
	(s) == "HDD" ? ACMEDIATYPE::ACHDD : \
	ACMEDIATYPE::ACUNK)

struct ArcadeBootParams {
	std::string cards[2];
	std::string mediapath;
	std::string elf_path;
	std::string sram_path;
	ACMEDIATYPE MediaType;
};

namespace Arcade
{
	/// True from the moment an arcade game starts booting until its VM is gone. Every change the
	/// arcade board makes to the console (memory map, DEV9, timings, memory card handshake...) is
	/// gated on it, so a retail game runs exactly as it would without any of this code.
	/// Set and cleared by VMManager only, on the CPU thread, outside any running VM. Atomic because the GS,
	/// UI and JNI threads read it too; a relaxed load is a plain load, so the hot paths pay nothing.
	extern std::atomic<bool> s_session;
	static inline bool IsActive() { return s_session.load(std::memory_order_relaxed); }

	/// An arcade game ID as an .acgame names it: "NM" and five digits (PCSX2x6's convention).
	static inline bool IsGameId(std::string_view id)
	{
		if (id.size() != 7 || id[0] != 'N' || id[1] != 'M')
			return false;
		for (size_t i = 2; i < 7; i++)
		{
			if (id[i] < '0' || id[i] > '9')
				return false;
		}
		return true;
	}
} // namespace Arcade

/// System 256 region signature to answer sceCdReadILinkId with (the .acgame's 256Region: ASIA4, ASIA5 or
/// JAPAN), empty for none. Arcade sessions only.
extern std::string ArcadeiLinkID;
