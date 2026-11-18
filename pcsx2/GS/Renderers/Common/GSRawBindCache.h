// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <cstring>

/// What a draw recorded with raw API calls must bind, given the draw recorded just before it in the same batch. The row
/// emitter records its held classic twins back to back (GSRunEmitterVK::RecordAll); consecutive twins mostly share their
/// pipeline, textures, viewport, scissor and buffers, and a driver pays for every bind. Nothing else may record between
/// two draws of a batch; the batch starts with Reset, and anything recorded in between (a run-program group) resets it.
class GSRawBindCache
{
public:
	enum : u32
	{
		PIPELINE = 1u << 0,
		CONSTANTS = 1u << 1, ///< the constant buffers' set and its offsets
		TEXTURES = 1u << 2,
		VIEWPORT = 1u << 3,
		SCISSOR = 1u << 4,
		BLEND = 1u << 5, ///< the blend constant
		INDEX = 1u << 6,
		VERTEX = 1u << 7,
		ALL = (1u << 8) - 1,
	};

	/// One draw's bindings, as opaque handles and values.
	struct State
	{
		u64 pipeline = 0;
		u64 constants_set = 0;
		u32 constants[2] = {};
		u64 textures[4] = {}; ///< texture, sampler, palette, and an allocated set where the textures are not pushed
		s32 viewport[2] = {}; ///< width, height (the origin is always 0, 0)
		s32 scissor[4] = {};
		u32 blend = 0; ///< the constant, with bit 8 set when the draw uses it
		u64 index_buffer = 0;
		u64 vertex_buffer = 0;
	};

	/// The batch starts: the next draw binds everything.
	void Reset() { m_valid = false; }

	/// The bindings the draw needs, and remembers them as what is bound. A blend constant the draw does not use is never
	/// bound, and does not count as bound for the next draw.
	u32 Needed(const State&) { return ALL; }

private:
	State m_last;
	bool m_valid = false;
};
