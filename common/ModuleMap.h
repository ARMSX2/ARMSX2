// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// The loaded modules (the executable and its shared libraries), kept in a fixed
// table so a signal handler can name the module an address is in without
// calling into the dynamic linker. Linux and Android only; elsewhere Refresh
// does nothing and Find finds nothing.
//
// Refresh rebuilds the table from dl_iterate_phdr. It runs at load time only:
// once at startup (RecCrashReport::Install), after every library the emulator
// loads through DynamicLibrary, and after the Vulkan instance and device are
// made, when the loader brings in the driver. A library loaded some other way
// is in the table from the next refresh on.
//
// Find is async-signal-safe: one atomic load, a linear search of the table and
// a copy. Refresh writes the copy of the table that Find is not reading, then
// switches the two; a sequence count on each copy lets Find detect the rare
// case of two refreshes overlapping its search, and retry.
namespace ModuleMap
{
	static constexpr u32 kNameSize = 64; // longer names are cut short
	static constexpr u32 kMaxBuildIdSize = 32;
	// Past this many, the earliest after the first few are left out (ModuleMap.cpp).
	static constexpr u32 kMaxModules = 256;

	struct Hit
	{
		char name[kNameSize] = {}; // the file name, without its directory
		uptr offset = 0; // the address minus the load bias: an ELF address, what addr2line takes
		u8 build_id[kMaxBuildIdSize] = {}; // the GNU build id note
		u32 build_id_size = 0; // 0: the module has none
	};

	void Refresh();

	// The module whose loaded segments span the address. Async-signal-safe.
	bool Find(uptr address, Hit* hit);

	// The modules in the table, and the ones that did not fit.
	u32 Count();
	u32 Dropped();
} // namespace ModuleMap
