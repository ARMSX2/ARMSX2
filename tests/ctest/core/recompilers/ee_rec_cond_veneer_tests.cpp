// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The EE recompiler's conditional-branch veneers.
//
// B.cond and CBZ reach +/-1MB. The dispatchers sit at the base of a code
// cache tens of megabytes deep, so blocks past its first megabyte reach them
// through a veneer pair emitted in front of a block. A test cache never grows
// that far, so recEeForceCondVeneers routes every event check and every
// delay-slot bracket through the pair whatever the distance, and
// recEeCondVeneerHops counts what took that route.
//
// recEeExecuteBlock enters with nextEventCycle == cycle, so the cycle delta is
// already non-negative at the first block tail: the event check below is taken
// on the first exit of every program here, and the veneer it hops through is
// executed, not merely emitted.

#include "harness/EeRecTestHarness.h"

#include "Config.h"
#include "R5900.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

using namespace recompiler_tests;
using namespace mips;

extern void recEeForceCondVeneers(bool on);
extern u32 recEeCondVeneerHops();
extern bool recEeBlockHostInfo(u32 pc_query, uptr* fnptr, u32* host_size, uptr* lut_fnptr);
extern void recEeDispatcherAddrs(const void** dispatcher_event, const void** dispatcher_reg);

namespace {

constexpr u32 kCauseSys = 0x20; // ExcCode=8 (Sys), << 2

constexpr u32 kFuncA = RecompilerTestEnvironment::kProgramPc + 0x100;
constexpr u32 kPark = RecompilerTestEnvironment::kParkingPc;

class ForcedVeneers
{
public:
	ForcedVeneers()
		: before_(recEeCondVeneerHops())
	{
		recEeForceCondVeneers(true);
	}
	~ForcedVeneers() { recEeForceCondVeneers(false); }

	u32 Hops() const { return recEeCondVeneerHops() - before_; }

private:
	u32 before_;
};

// AArch64 branch decoding, enough to walk a block's conditional exits.
constexpr u32 kCondGe = 0xA;

s32 SignExtend(u32 field, u32 bits)
{
	const u32 sign = 1u << (bits - 1);
	return static_cast<s32>((field ^ sign) - sign);
}

const u32* BranchTargetImm19(const u32* at)
{
	return at + SignExtend((*at >> 5) & 0x7FFFF, 19);
}

bool IsBCond(u32 w, u32 cond)
{
	return (w & 0xFF000010u) == 0x54000000u && (w & 0xFu) == cond;
}

bool IsCbz32(u32 w)
{
	return (w & 0xFF000000u) == 0x34000000u;
}

// A veneer is one B. Returns where it goes, or nullptr if `at` is not one.
const void* VeneerDestination(const u32* at)
{
	if ((*at >> 26) != 0x05u)
		return nullptr;
	return at + SignExtend(*at & 0x3FFFFFFu, 26);
}

// Every conditional exit of the block at `pc` that leaves the block, paired
// with what the veneer it branches to branches to.
std::vector<std::pair<u32, const void*>> ConditionalExitDestinations(u32 pc)
{
	uptr fnptr = 0, lut = 0;
	u32 host_size = 0;
	std::vector<std::pair<u32, const void*>> out;
	if (!recEeBlockHostInfo(pc, &fnptr, &host_size, &lut))
		return out;
	const u32* begin = reinterpret_cast<const u32*>(fnptr);
	const u32* end = begin + host_size / 4;
	for (const u32* at = begin; at < end; at++)
	{
		if (!IsBCond(*at, kCondGe) && !IsCbz32(*at))
			continue;
		const u32* target = BranchTargetImm19(at);
		if (target >= begin && target < end)
			continue; // an exit, not a branch inside the block
		out.emplace_back(*at, VeneerDestination(target));
	}
	return out;
}

} // namespace

// The plain block tail: pc store, cycle update, event check. Forced, the
// check is `b.ge veneer` rather than `b.lt` over a `b`, and it is taken.
TEST(EeRecCondVeneer, EventCheckExitsThroughTheVeneer)
{
	ForcedVeneers forced;
	EeRecTestHarness h;
	h.SetGpr64(reg::a0, 3);
	h.LoadProgram({
		ADDIU(reg::v0, reg::zero, 5),
		ADDU(reg::v1, reg::v0, reg::a0),
		BEQ(reg::zero, reg::zero, 1),
		ADDIU(reg::t0, reg::zero, 11), // delay slot
		ADDIU(reg::t1, reg::zero, 22), // branch target
	});
	h.Run();
	ASSERT_GT(forced.Hops(), 0u) << "nothing was routed through a veneer";
	h.ExpectGpr64(reg::v0, 5ull);
	h.ExpectGpr64(reg::v1, 8ull);
	h.ExpectGpr64(reg::t0, 11ull);
	h.ExpectGpr64(reg::t1, 22ull);
}

// The delay-slot exception bracket's epilogue: forced, `cbz w8, veneer`
// replaces `cbnz` over a `b`, and the raise takes it to DispatcherReg.
// Mirrors EeRecTraps.SyscallInDelaySlotSetsCauseBdAndBranchEpc.
TEST(EeRecCondVeneer, DelaySlotRaiseDivertsThroughTheVeneer)
{
	ForcedVeneers forced;
	EeRecTestHarness h;
	h.LoadProgram({
		BEQ(reg::zero, reg::zero, 2),
		SYSCALL_(), // delay slot — raises
		ADDIU(reg::v0, reg::zero, 99),
		ADDIU(reg::v1, reg::zero, 77),
	});
	h.Run();
	ASSERT_GT(forced.Hops(), 0u) << "nothing was routed through a veneer";
	h.ExpectGpr64(reg::v0, 0ull);
	h.ExpectGpr64(reg::v1, 0ull);
	EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, kCauseSys);
	EXPECT_NE(h.GetCp0Jit(13) & 0x80000000u, 0u) << "JIT CAUSE.BD";
	EXPECT_EQ(h.GetCp0Jit(14), RecompilerTestEnvironment::kProgramPc)
		<< "JIT EPC must be the branch, not the delay slot";
}

// A guest call: the ring push sits between the tail flush and the event check,
// so the veneer branch lands at the end of the longest tail the rec emits, and
// the guest JR-$ra return has its own.
TEST(EeRecCondVeneer, CallTailExitsThroughTheVeneer)
{
	ForcedVeneers forced;
	EeRecTestHarness h;
	h.LoadProgramNoTerm({
		ADDIU(reg::v0, reg::zero, 1),
		JAL(kFuncA),
		NOP,
		ADDIU(reg::v1, reg::zero, 9),
		J(kPark),
		NOP,
	});
	h.WriteU32(kFuncA + 0, ADDIU(reg::v0, reg::v0, 10));
	h.WriteU32(kFuncA + 4, JR(reg::ra));
	h.WriteU32(kFuncA + 8, NOP);
	h.Run();
	ASSERT_GT(forced.Hops(), 0u) << "nothing was routed through a veneer";
	h.ExpectGpr64(reg::v0, 11ull);
	h.ExpectGpr64(reg::v1, 9ull);
}

// The pair displaces the entry armStartBlock had aligned to 16; the padding
// behind it puts the entry back, so what recBlocks links against is what it
// would have been without a veneer in front.
TEST(EeRecCondVeneer, AVeneerPairLeavesTheBlockEntryAligned)
{
	ForcedVeneers forced;
	EeRecTestHarness h;
	h.LoadProgram({
		ADDIU(reg::v0, reg::zero, 5),
		ADDIU(reg::v1, reg::zero, 6),
	});
	h.Run();
	ASSERT_GT(forced.Hops(), 0u) << "nothing was routed through a veneer";
	h.ExpectGpr64(reg::v0, 5ull);

	uptr fnptr = 0, lut = 0;
	u32 host_size = 0;
	ASSERT_TRUE(recEeBlockHostInfo(RecompilerTestEnvironment::kProgramPc, &fnptr, &host_size, &lut));
	EXPECT_EQ(fnptr & 15u, 0u);
	EXPECT_EQ(lut, fnptr);
}

// What the harness cannot see: which dispatcher a hop arrives at. Read it off
// the emitted code instead -- every conditional exit of the block goes to a
// veneer, and the veneer's single B says which one.
TEST(EeRecCondVeneer, TheVeneerAnEventCheckHopsThroughBranchesToDispatcherEvent)
{
	ForcedVeneers forced;
	EeRecTestHarness h;
	h.LoadProgram({
		ADDIU(reg::v0, reg::zero, 5),
		ADDIU(reg::v1, reg::zero, 6),
	});
	h.Run();
	ASSERT_GT(forced.Hops(), 0u) << "nothing was routed through a veneer";

	const void* dispatcher_event = nullptr;
	const void* dispatcher_reg = nullptr;
	recEeDispatcherAddrs(&dispatcher_event, &dispatcher_reg);

	const auto exits = ConditionalExitDestinations(RecompilerTestEnvironment::kProgramPc);
	ASSERT_FALSE(exits.empty()) << "block has no conditional exit to check";
	for (const auto& [word, dest] : exits)
	{
		ASSERT_TRUE(IsBCond(word, kCondGe)) << "a straight-line block's only "
											   "conditional exit is the event check";
		EXPECT_EQ(dest, dispatcher_event);
	}
}

TEST(EeRecCondVeneer, TheVeneerADelaySlotBracketHopsThroughBranchesToDispatcherReg)
{
	ForcedVeneers forced;
	EeRecTestHarness h;
	h.LoadProgram({
		BEQ(reg::zero, reg::zero, 2),
		SYSCALL_(),
		ADDIU(reg::v0, reg::zero, 99),
		ADDIU(reg::v1, reg::zero, 77),
	});
	h.Run();
	ASSERT_GT(forced.Hops(), 0u) << "nothing was routed through a veneer";

	const void* dispatcher_event = nullptr;
	const void* dispatcher_reg = nullptr;
	recEeDispatcherAddrs(&dispatcher_event, &dispatcher_reg);

	const auto exits = ConditionalExitDestinations(RecompilerTestEnvironment::kProgramPc);
	u32 brackets = 0, checks = 0;
	for (const auto& [word, dest] : exits)
	{
		if (IsCbz32(word))
		{
			brackets++;
			EXPECT_EQ(dest, dispatcher_reg);
		}
		else
		{
			checks++;
			EXPECT_EQ(dest, dispatcher_event);
		}
	}
	EXPECT_GT(brackets, 0u) << "the delay slot raised no bracket";
	EXPECT_GT(checks, 0u) << "the block tail emitted no event check";
}

// The control: unforced, a test cache reaches both dispatchers directly, so
// the same programs must take no veneer at all. Without this the assertions
// above would pass on a build where forcing did nothing.
TEST(EeRecCondVeneer, UnforcedTestCacheReachesTheDispatchersDirectly)
{
	const u32 before = recEeCondVeneerHops();
	{
		EeRecTestHarness h;
		h.LoadProgram({
			BEQ(reg::zero, reg::zero, 2),
			SYSCALL_(),
			ADDIU(reg::v0, reg::zero, 99),
			ADDIU(reg::v1, reg::zero, 77),
		});
		h.Run();
		EXPECT_EQ(h.GetCp0Jit(13) & 0xFFu, kCauseSys);
	}
	EXPECT_EQ(recEeCondVeneerHops(), before);
}
