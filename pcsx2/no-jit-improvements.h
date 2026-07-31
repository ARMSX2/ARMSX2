// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace R5900
{
	struct OPCODE;
}

namespace NoJITImprovements
{
	using ExecuteInstruction = bool (*)(u32 pc, u32 code, const R5900::OPCODE& opcode);

	// Executes a validated block of predecoded EE interpreter instructions.
	// Returns false when the current address cannot be cached safely.
	bool ExecuteEEBlock(u32 pc, ExecuteInstruction execute_instruction);

	// Drops all cached blocks without releasing the fixed cache allocation.
	void ResetEEBlockCache();

	// Invalidates blocks overlapping an EE word range. Cached bytes are also
	// validated before every reuse, so this is an eager optimization rather
	// than the sole self-modifying-code correctness mechanism.
	void InvalidateEEBlockCache(u32 address, u32 size_in_words);
}
