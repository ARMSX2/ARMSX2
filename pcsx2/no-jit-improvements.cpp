// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "no-jit-improvements.h"

#include "Config.h"
#include "R5900OpcodeTables.h"
#include "vtlb.h"

#include <array>
#include <limits>

namespace NoJITImprovements
{
	namespace
	{
		// Short fixed blocks avoid runtime allocation on the EE execution path.
		// Branch delay slots remain on the original interpreter path because the
		// branch handlers execute them directly.
		static constexpr u32 BLOCK_INSTRUCTION_CAPACITY = 16;
		static constexpr u32 BLOCK_CACHE_SIZE = 4096;
		static constexpr u32 INVALID_PC = std::numeric_limits<u32>::max();

		struct alignas(64) EEBlock
		{
			u32 start_pc = INVALID_PC;
			u8 instruction_count = 0;
			std::array<u32, BLOCK_INSTRUCTION_CAPACITY> code{};
			std::array<const R5900::OPCODE*, BLOCK_INSTRUCTION_CAPACITY> opcodes{};
		};

		static std::array<EEBlock, BLOCK_CACHE_SIZE> s_ee_blocks;

		static __fi u32 GetCacheIndex(u32 pc)
		{
			const u32 instruction_address = pc >> 2;
			return (instruction_address ^ (instruction_address >> 11) ^ (instruction_address >> 19)) &
				   (BLOCK_CACHE_SIZE - 1);
		}

		static bool IsBlockCurrent(const EEBlock& block)
		{
			if (block.instruction_count == 0)
				return false;

			const u32 byte_count = static_cast<u32>(block.instruction_count) * sizeof(u32);
			return vtlb_memSafeCmpBytes(block.start_pc, block.code.data(), byte_count) == 0;
		}

		static bool BuildBlock(EEBlock& block, u32 pc)
		{
			block.start_pc = INVALID_PC;
			block.instruction_count = 0;

			u32 instruction_pc = pc;
			for (u32 i = 0; i < BLOCK_INSTRUCTION_CAPACITY; i++, instruction_pc += sizeof(u32))
			{
				bool valid = false;
				const u32 code = vtlb_ramRead<mem32_t>(instruction_pc, &valid);
				if (!valid)
					break;

				const R5900::OPCODE& opcode = R5900::GetInstruction(code);
				block.code[i] = code;
				block.opcodes[i] = &opcode;
				block.instruction_count++;

				// A store can modify a later instruction in the current block.
				// COP0 operations can alter address translation. End immediately
				// after either so the next block validates the new memory view.
				const bool changes_memory_view =
					(opcode.flags & (IS_BRANCH | IS_STORE)) != 0 || (code >> 26) == 0x10;
				if (changes_memory_view || instruction_pc > (INVALID_PC - sizeof(u32)))
					break;
			}

			if (block.instruction_count == 0)
				return false;

			// Publish the key last so a partially rebuilt slot is never treated
			// as a valid hit if execution is interrupted by a VM exit.
			block.start_pc = pc;
			return true;
		}

		static bool IsCacheEnabled()
		{
#if defined(PCSX2_DEVBUILD) || defined(PCSX2_RECOMPILER_TESTS)
			// Debug stepping and the interpreter oracle require the original
			// one-instruction fetch path.
			return false;
#else
			// EE cache emulation can intentionally expose instruction bytes
			// which differ from RAM, so side-effect-free RAM validation cannot
			// be used while that compatibility option is active.
			return !CHECK_EEREC && !CHECK_CACHE;
#endif
		}
	}

	bool ExecuteEEBlock(u32 pc, ExecuteInstruction execute_instruction)
	{
		if (!execute_instruction || !IsCacheEnabled())
			return false;

		EEBlock& block = s_ee_blocks[GetCacheIndex(pc)];
		if (block.start_pc != pc || !IsBlockCurrent(block))
		{
			if (!BuildBlock(block, pc))
				return false;
		}

		for (u32 i = 0; i < block.instruction_count; i++)
		{
			if (!execute_instruction(pc + (i * sizeof(u32)), block.code[i], *block.opcodes[i]))
				break;
		}

		return true;
	}

	void ResetEEBlockCache()
	{
		for (EEBlock& block : s_ee_blocks)
		{
			block.start_pc = INVALID_PC;
			block.instruction_count = 0;
		}
	}

	void InvalidateEEBlockCache(u32 address, u32 size_in_words)
	{
		if (size_in_words == 0)
			return;

		// Large clears are cheaper to handle as a complete cache reset.
		if (size_in_words >= BLOCK_CACHE_SIZE)
		{
			ResetEEBlockCache();
			return;
		}

		const u64 clear_start = address;
		const u64 clear_end = clear_start + (static_cast<u64>(size_in_words) * sizeof(u32));
		if (clear_end > (static_cast<u64>(std::numeric_limits<u32>::max()) + 1))
		{
			ResetEEBlockCache();
			return;
		}

		// A cached block can begin up to fifteen instructions before the
		// cleared range. Probe only those possible direct-mapped slots instead
		// of scanning the complete cache.
		const u64 lookbehind = (BLOCK_INSTRUCTION_CAPACITY - 1) * sizeof(u32);
		const u64 candidate_start = (clear_start > lookbehind) ? (clear_start - lookbehind) : 0;
		for (u64 candidate_pc = candidate_start & ~static_cast<u64>(3);
			 candidate_pc < clear_end; candidate_pc += sizeof(u32))
		{
			EEBlock& block = s_ee_blocks[GetCacheIndex(static_cast<u32>(candidate_pc))];
			if (block.instruction_count == 0)
				continue;

			const u64 block_start = block.start_pc;
			const u64 block_end =
				block_start + (static_cast<u64>(block.instruction_count) * sizeof(u32));
			if (clear_start < block_end && block_start < clear_end)
			{
				block.start_pc = INVALID_PC;
				block.instruction_count = 0;
			}
		}
	}
}
