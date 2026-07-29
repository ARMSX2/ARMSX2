// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// XGKICK drain paths that the main VU1 XGKICK suite never reaches: the
// wrap at the top of VU1 memory, the cycle-metered XgKickHack drain, and a
// kick still in flight when an E-bit branch ends the program.
//
// The behavioural model:
//
// XGKICK hands the GIF a starting address inside VU1's 16 KB memory and the
// GIF then walks GIFtags from there. VU1 memory is circular, so a packet that
// starts near the top runs off the end and continues at offset 0 — the
// transfer has to be split at that seam. Games do this routinely: the kick
// address is a rolling double-buffer pointer, so straddling the top is normal
// traffic, not a corner case. Split it at the wrong offset and the GS receives
// the right number of bytes from the wrong place.
//
// With the XgKickHack gamefix on (the GameDB forces it for several titles) the
// whole drain changes shape: instead of one transfer at kick time, a C helper
// meters the packet out against accumulated VU cycles, tracking a residual
// size and a rolling address across calls. That helper is ~50 lines of
// hand-maintained state machine and had never been executed by a test — the
// existing XgKickHack case deliberately issues no kick at all, because it is
// about register spilling around the sync site rather than the drain itself.

#include "harness/VuTestHarness.h"

#include "Config.h"
#include "VU.h"

#include <gtest/gtest.h>

#include <vector>

namespace recompiler_tests {

using namespace vu;

namespace {

inline VuOp LowerOnly(u32 lower) { return VuOp{lower, VNOP_U()}; }
inline VuOp BareNopPair() { return VuOp{0, VNOP_U()}; }

// VU1 memory is 16 KB and the kick address is a qword index masked to 10 bits.
constexpr u32 kVu1MemSize = 0x4000;

struct ScopedXgKickHack
{
	bool prev = EmuConfig.Gamefixes.XgKickHack;
	explicit ScopedXgKickHack(bool on) { EmuConfig.Gamefixes.XgKickHack = on; }
	~ScopedXgKickHack() { EmuConfig.Gamefixes.XgKickHack = prev; }
};

// PACKED-mode GIFtag with NREG=1 (A+D) and the given NLOOP. Packet length is
// 16 (tag) + NLOOP * 16 (data), per Gif_Tag::setTag's GIF_FLG_PACKED case.
void WritePackedTag(VuTestHarness& h, u32 addr, u32 nloop, bool eop)
{
	const u32 w0 = (nloop & 0x7FFFu) | (eop ? (1u << 15) : 0u);
	const u32 w1 = (0u << 26) | (1u << 28); // FLG = PACKED, NREG = 1
	const u32 w2 = 0x0000000Eu;             // REGS[0] = A+D
	h.WriteMemU128(addr, w0, w1, w2, 0u);
}

constexpr u32 PackedPacketBytes(u32 nloop) { return 16u + nloop * 16u; }

// EOP-only tag: NLOOP = 0, so the whole packet is the 16-byte tag.
void WriteEopOnlyTag(VuTestHarness& h, u32 addr)
{
	h.WriteMemU128(addr, 0x00008000u, 0u, 0u, 0u);
}

} // namespace

// =========================================================================
//  Wrap at the top of VU1 memory
//
//  Note on what the capture can see: the split's FIRST half goes out through
//  Gif_Path::CopyGSPacketData and the second through
//  Gif_Unit::TransferGSPacketData. Both feed the test sink, so the JIT's
//  captured stream is the whole packet — head, then tail resuming at offset 0.
//  It used to hook only the latter, which made a wrapped kick look like a
//  headless packet carrying no GIFtag at all; the seam arithmetic was then the
//  only thing this could pin, and the tag half was invisible.
// =========================================================================

TEST(Vu1XgkickDrain, PacketWrappingTheTopOfVu1MemorySplitsAtTheSeam)
{
	VuTestHarness h(1);
	h.SetDiffMode(VuDiffMode::XgkickPacketEquivalent);

	// Tag in the last qword of VU1 memory: 16 bytes of tag fit, the two data
	// qwords do not, so the packet continues at offset 0.
	constexpr u32 kTagAddr = kVu1MemSize - 16; // 0x3FF0
	constexpr u32 kNLoop = 2;
	constexpr u32 kPacketBytes = PackedPacketBytes(kNLoop); // 48
	constexpr u32 kDiff = kVu1MemSize - kTagAddr;           // 16
	constexpr u32 kTailBytes = kPacketBytes - kDiff;        // 32

	WritePackedTag(h, kTagAddr, kNLoop, /*eop=*/true);
	// Recognisable payload at the wrap point, so a tail taken from the wrong
	// offset shows up as content rather than only as a length.
	for (u32 i = 0; i < kTailBytes / 16; i++)
	{
		h.WriteMemU128(i * 16, 0xA0000000u + i, 0xB0000000u + i,
			0xC0000000u + i, 0xD0000000u + i);
	}

	h.SetVi(vi::vi5, kTagAddr / 16);
	h.LoadProgram({
		LowerOnly(VXGKICK_L(vi::vi5)),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		EBitNopPair(),
	});
	h.Run();

	const std::vector<u8>& jit = h.Path1PacketBytesJit();
	ASSERT_EQ(jit.size(), kPacketBytes)
		<< "both halves of the split must reach the sink: the head up to the top "
		   "of VU1 memory, then the wrapped remainder";

	// Head is `kDiff` bytes read from the tag address; the tail is the rest,
	// taken from offset 0. A tail lifted from the wrong offset shows up as
	// content, not merely as a length.
	std::vector<u8> expected(kPacketBytes);
	std::memcpy(expected.data(), &vuRegs[1].Mem[kTagAddr], kDiff);
	std::memcpy(expected.data() + kDiff, &vuRegs[1].Mem[0], kTailBytes);
	EXPECT_EQ(jit, expected)
		<< "the head must carry the GIFtag and the wrapped half must resume at "
		   "VU1 memory offset 0";

	// The interpreter drains the same packet in two metered steps, so it
	// reaches the same 48 bytes by a different route.
	EXPECT_EQ(h.Path1PacketBytesInterp().size(), kPacketBytes);
}

// =========================================================================
//  Cycle-metered drain under the XgKickHack gamefix
// =========================================================================

TEST(Vu1XgkickDrain, XgKickHackDrainsTheWholePacket)
{
	ScopedXgKickHack hack(true);

	VuTestHarness h(1);
	h.SetDiffMode(VuDiffMode::XgkickPacketEquivalent);

	constexpr u32 kNLoop = 3;
	constexpr u32 kPacketBytes = PackedPacketBytes(kNLoop); // 64
	WritePackedTag(h, 0, kNLoop, /*eop=*/true);

	h.SetVi(vi::vi5, 0);
	h.LoadProgram({
		LowerOnly(VXGKICK_L(vi::vi5)),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		EBitNopPair(),
	});
	h.Run();

	// The metering may hand the packet over in several chunks, but by the time
	// the program ends every byte must have reached Path 1 exactly once.
	EXPECT_EQ(h.Path1PacketBytesJit().size(), kPacketBytes)
		<< "the XgKickHack drain must deliver the whole packet, no more and no "
		   "less, by end of program";
	EXPECT_EQ(h.Path1PacketBytesJit(), h.Path1PacketBytesInterp());
}

TEST(Vu1XgkickDrain, XgKickHackDrainsAWrappingPacketInTwoChunks)
{
	// The hack's drain is a loop, and a packet that runs off the top of VU1
	// memory is what makes it go round twice: the first pass is clamped to the
	// distance to the top, and the second can only be right if the rolling
	// address wrapped to 0 in between. A single-chunk packet never touches
	// that bookkeeping.
	ScopedXgKickHack hack(true);

	VuTestHarness h(1);
	h.SetDiffMode(VuDiffMode::XgkickPacketEquivalent);

	constexpr u32 kTagAddr = kVu1MemSize - 16;
	constexpr u32 kNLoop = 2;
	constexpr u32 kPacketBytes = PackedPacketBytes(kNLoop); // 48
	constexpr u32 kDiff = kVu1MemSize - kTagAddr;           // 16

	WritePackedTag(h, kTagAddr, kNLoop, /*eop=*/true);
	for (u32 i = 0; i < (kPacketBytes - kDiff) / 16; i++)
	{
		h.WriteMemU128(i * 16, 0xA0000000u + i, 0xB0000000u + i,
			0xC0000000u + i, 0xD0000000u + i);
	}

	h.SetVi(vi::vi5, kTagAddr / 16);
	h.LoadProgram({
		LowerOnly(VXGKICK_L(vi::vi5)),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		EBitNopPair(),
	});
	h.Run();

	// The capture is the whole packet: the tag from the top of memory followed
	// by the two payload qwords from offset 0. The hack reaches that through
	// the metered loop rather than through the split above, which is the point
	// of running the wrap under it as well.
	const std::vector<u8>& jit = h.Path1PacketBytesJit();
	ASSERT_EQ(jit.size(), kPacketBytes);

	std::vector<u8> expected;
	expected.insert(expected.end(), &vuRegs[1].Mem[kTagAddr],
		&vuRegs[1].Mem[kTagAddr] + kDiff);
	expected.insert(expected.end(), &vuRegs[1].Mem[0],
		&vuRegs[1].Mem[0] + (kPacketBytes - kDiff));
	EXPECT_EQ(jit, expected)
		<< "the second chunk must resume at VU1 memory offset 0 — the rolling "
		   "kick address has to wrap between the two passes";
}

TEST(Vu1XgkickDrain, XgKickHackDrainsBackToBackKicks)
{
	// Two kicks in flight exercise the helper's residual-size and rolling-
	// address bookkeeping across calls, which a single kick that fits in one
	// metered chunk never touches.
	ScopedXgKickHack hack(true);

	VuTestHarness h(1);
	h.SetDiffMode(VuDiffMode::XgkickPacketEquivalent);

	WriteEopOnlyTag(h, 0x000);
	WriteEopOnlyTag(h, 0x100);
	h.SetVi(vi::vi5, 0x000 / 16);
	h.SetVi(vi::vi6, 0x100 / 16);
	h.LoadProgram({
		LowerOnly(VXGKICK_L(vi::vi5)),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		LowerOnly(VXGKICK_L(vi::vi6)),
		BareNopPair(), BareNopPair(), BareNopPair(), BareNopPair(),
		EBitNopPair(),
	});
	h.Run();

	EXPECT_EQ(h.Path1PacketBytesJit().size(), 32u);
	EXPECT_EQ(h.Path1PacketBytesJit(), h.Path1PacketBytesInterp());
}

// =========================================================================
//  Kick still in flight when an E-bit branch ends the program
//
//  A kick issued in a branch delay slot is the last thing the block analyses,
//  so its latency never elapses inside the block. The end-of-program path has
//  to notice and drain it — the emit loop's own drain never runs for that
//  pair. Placing the kick in an ordinary E-bit's delay slot doesn't reach it
//  (the appended pair decrements the latency first); it takes a branch that
//  both ends the program and owns the delay slot.
// =========================================================================

TEST(Vu1XgkickDrain, EBitBranchDrainsAKickIssuedInItsDelaySlot)
{
	VuTestHarness h(1);
	h.SetDiffMode(VuDiffMode::XgkickPacketEquivalent);

	WriteEopOnlyTag(h, 0);
	h.SetVi(vi::vi5, 0);
	h.LoadProgram({
		EBit(LowerOnly(VB_L(+2))),      // pair 0: E-bit branch to pair 3
		LowerOnly(VXGKICK_L(vi::vi5)),  // pair 1: delay slot — kick issued here
		BareNopPair(),                  // pair 2: skipped
		BareNopPair(),                  // pair 3: branch target — not executed
		EBitNopPair(),                  // pair 4
	});
	h.Run();

	EXPECT_EQ(h.Path1PacketBytesJit().size(), 16u)
		<< "a kick issued in the delay slot of an E-bit branch must still be "
		   "drained by the end-of-program path";
	EXPECT_EQ(h.Path1PacketBytesJit(), h.Path1PacketBytesInterp());
}

} // namespace recompiler_tests
