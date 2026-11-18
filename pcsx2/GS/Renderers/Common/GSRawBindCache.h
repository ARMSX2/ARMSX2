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
	u32 Needed(const State& s)
	{
		u32 need = ALL;
		if (m_valid)
		{
			need = 0;
			if (s.pipeline != m_last.pipeline)
				need |= PIPELINE;
			if (s.constants_set != m_last.constants_set || s.constants[0] != m_last.constants[0] ||
				s.constants[1] != m_last.constants[1])
				need |= CONSTANTS;
			if (std::memcmp(s.textures, m_last.textures, sizeof(s.textures)) != 0)
				need |= TEXTURES;
			if (s.viewport[0] != m_last.viewport[0] || s.viewport[1] != m_last.viewport[1])
				need |= VIEWPORT;
			if (std::memcmp(s.scissor, m_last.scissor, sizeof(s.scissor)) != 0)
				need |= SCISSOR;
			if ((s.blend & 0x100) && s.blend != m_last.blend)
				need |= BLEND;
			if (s.index_buffer != m_last.index_buffer)
				need |= INDEX;
			if (s.vertex_buffer != m_last.vertex_buffer)
				need |= VERTEX;
		}
		if (!(s.blend & 0x100))
		{
			need &= ~BLEND;
			const u32 kept = m_valid ? m_last.blend : 0;
			m_last = s;
			m_last.blend = kept;
		}
		else
		{
			m_last = s;
		}
		m_valid = true;
		return need;
	}

private:
	State m_last;
	bool m_valid = false;
};
