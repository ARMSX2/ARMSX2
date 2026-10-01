// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// armEmitLaneGather32 over its whole domain: each of the 256 four-lane maps is
// emitted, executed on a marked input vector, and diffed against the gather it
// names. Which opening shuffle the emitter picked is not read back from the
// code — the executed result covers what it does, and the returned instruction
// count covers what it cost, which between them leave a wrong pick nowhere to
// hide: a base that gathers the wrong lanes fails the value check, and one that
// leaves more lanes to fix than another in the table fails the count.
//
// The bound the count is checked against, one copy plus one insert per lane
// that moves, is what the emitter did before it had a table to pick from.

#include "common/Pcsx2Defs.h"

#if defined(__aarch64__)

#include "arm64/AsmHelpers.h"

#include "Arm64JitBuffer.h"

#include <gtest/gtest.h>

#include <array>

using namespace vixl::aarch64;

namespace
{
// void gather(const u32 in[4], u32 out[4])
using GatherFn = void (*)(const u32*, u32*);

// Distinct in every byte, so a gather that takes the right lane from the wrong
// half or the wrong register cannot land on the expected word by accident.
constexpr u32 kInput[4] = {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};

int EmitGather(JitBuffer& buf, const int lane[4], GatherFn* fn)
{
	armSetAsmPtr(buf.ptr(), buf.size(), nullptr);
	u8* const code = armStartBlock();
	armAsm->Ldr(q0, MemOperand(x0));
	const int emitted = armEmitLaneGather32(q1, q0, lane);
	armAsm->Str(q1, MemOperand(x1));
	armAsm->Ret();
	armEndBlock();
	*fn = reinterpret_cast<GatherFn>(code);
	return emitted;
}

// What a copy plus an insert per moved lane costs — the emitter's upper bound.
int CopyAndInsertCost(const int lane[4])
{
	int n = 1;
	for (int i = 0; i < 4; i++)
		n += (lane[i] != i);
	return n;
}
} // namespace

TEST(Arm64LaneGather, EveryLaneMapGathersWhatItNames)
{
	JitBuffer buf(4096);
	ASSERT_NE(buf.ptr(), nullptr) << "MAP_JIT allocation failed";

	int total = 0;
	for (int m = 0; m < 256; m++)
	{
		const int lane[4] = {(m >> 6) & 3, (m >> 4) & 3, (m >> 2) & 3, m & 3};
		GatherFn fn = nullptr;
		const int emitted = EmitGather(buf, lane, &fn);
		total += emitted;

		alignas(16) u32 out[4] = {0, 0, 0, 0};
		fn(kInput, out);
		for (int i = 0; i < 4; i++)
		{
			EXPECT_EQ(out[i], kInput[lane[i]])
				<< "lane " << i << " of map " << lane[0] << lane[1] << lane[2] << lane[3];
		}
		EXPECT_LE(emitted, CopyAndInsertCost(lane))
			<< "map " << lane[0] << lane[1] << lane[2] << lane[3]
			<< " costs more than a copy and an insert per moved lane";
	}

	// 1024 instructions over the 256 maps if every one opened with a copy, and
	// no map costs more than three. Any entry added to or dropped from the
	// table of opening shuffles moves this total.
	EXPECT_EQ(total, 644);
}

// The maps the block-link flag reorder spends most of its sites on. Each is one
// instruction here and was a copy and three inserts before the table existed.
TEST(Arm64LaneGather, BroadcastsAndRotatesCostOneInstruction)
{
	JitBuffer buf(4096);
	ASSERT_NE(buf.ptr(), nullptr) << "MAP_JIT allocation failed";

	static constexpr int kOne[][4] = {
		{0, 1, 2, 3},                                       // copy
		{0, 0, 0, 0}, {1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}, // element broadcast
		{0, 1, 0, 1}, {2, 3, 2, 3},                         // doubleword broadcast
		{1, 2, 3, 0}, {2, 3, 0, 1}, {3, 0, 1, 2},           // rotate
	};
	for (const auto& lane : kOne)
	{
		GatherFn fn = nullptr;
		EXPECT_EQ(EmitGather(buf, lane, &fn), 1)
			<< "map " << lane[0] << lane[1] << lane[2] << lane[3];
		alignas(16) u32 out[4] = {0, 0, 0, 0};
		fn(kInput, out);
		for (int i = 0; i < 4; i++)
			EXPECT_EQ(out[i], kInput[lane[i]]);
	}
}

// One insert past a broadcast is what the second-heaviest map costs, and the
// emitter has to leave the source alone to read it: the base writes the whole
// of the destination before the insert reads lane 2 back out of the source.
TEST(Arm64LaneGather, TheSourceSurvivesTheOpeningShuffle)
{
	JitBuffer buf(4096);
	ASSERT_NE(buf.ptr(), nullptr) << "MAP_JIT allocation failed";

	static constexpr int kTwo[][4] = {
		{2, 3, 3, 3}, {0, 1, 1, 1}, {3, 3, 3, 0}, {2, 3, 0, 0}, {0, 0, 1, 2},
	};
	for (const auto& lane : kTwo)
	{
		GatherFn fn = nullptr;
		EXPECT_EQ(EmitGather(buf, lane, &fn), 2)
			<< "map " << lane[0] << lane[1] << lane[2] << lane[3];
		alignas(16) u32 out[4] = {0, 0, 0, 0};
		fn(kInput, out);
		for (int i = 0; i < 4; i++)
		{
			EXPECT_EQ(out[i], kInput[lane[i]])
				<< "lane " << i << " of map " << lane[0] << lane[1] << lane[2] << lane[3];
		}
	}
}

#endif // __aarch64__
