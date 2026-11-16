// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// LagRules (IopThread/IopLagRules.h): when the EE must not run far ahead of the IOP. The class reads no global, so these tests drive it
// with events and event tests at chosen EE cycles.

#include <gtest/gtest.h>

#include "IopThread/IopLagRules.h"

#include <array>

using namespace IopThread;

namespace
{
	constexpr s64 kLoose = 4915200;
	constexpr s32 kTight = 24576;
	constexpr u64 kBase = 10'000'000;

	LagRulesConfig Config(u32 rules)
	{
		LagRulesConfig c;
		c.mask = rules;
		c.tight = kTight;
		c.window = 100000;
		c.mailbox_window = 200000;
		c.poll_window = 50000;
		c.outstanding_max_age = 1'000'000;
		c.boot_frames = 10;
		c.stream_bits = StreamBit::SpuAdmaCore0 | StreamBit::SpuAdmaCore1 | StreamBit::CdvdBusy; // all of them, whatever the default says
		return c;
	}

	s64 At(LagRules& r, u64 cycle, bool idle = false, u32 streams = 0, u32 frame = 100)
	{
		return r.Bound(cycle, idle, streams, frame, kLoose);
	}
} // namespace

TEST(LagRules, WithNoRuleOnTheBoundIsTheLooseBound)
{
	LagRules r;
	r.Configure(Config(0));
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	r.OnMailboxAccess(kBase);
	r.OnEePoll(kBase);
	EXPECT_EQ(At(r, kBase + 10, true, 0xff, 0), kLoose);
	EXPECT_EQ(r.LastRule(), -1);
	EXPECT_FALSE(r.Live(kLoose));
}

TEST(LagRules, ALooseBoundAtOrBelowTheTightBoundIsLeftAlone)
{
	LagRules r;
	r.Configure(Config(LagRule::kAll));
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	EXPECT_FALSE(r.Live(kTight));
	EXPECT_EQ(r.Bound(kBase + 10, true, 0xff, 0, kTight), kTight);
	EXPECT_EQ(r.Bound(kBase + 10, true, 0xff, 0, kTight - 1), kTight - 1);
	EXPECT_TRUE(r.Live(kTight + 1));
}

TEST(LagRules, AWindowOpensAtAnEeStartedTransferAndEndsAfterItsLength)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::Window)));
	r.Reset();
	EXPECT_EQ(At(r, kBase - 5), kLoose);
	r.OnEeSifStart(kBase, 1);
	EXPECT_EQ(At(r, kBase + 3072), kTight);
	EXPECT_EQ(r.LastRule(), static_cast<s32>(LagRule::Window));
	EXPECT_EQ(At(r, kBase + 99999), kTight);
	EXPECT_EQ(At(r, kBase + 100000), kLoose);
	EXPECT_EQ(At(r, kBase + 200000), kLoose);
	EXPECT_EQ(r.Stats().activations[LagRule::Window], 1u);
}

TEST(LagRules, ATransferFromTheIopEndsTheWindowAtOnce)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::Window)));
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	EXPECT_EQ(At(r, kBase + 3072), kTight);
	r.OnIopReply(kBase + 5000);
	EXPECT_EQ(At(r, kBase + 6000), kLoose);
	EXPECT_EQ(r.Stats().window_closed_by_reply, 1u);
	// A reply with no window open changes nothing.
	r.OnIopReply(kBase + 7000);
	EXPECT_EQ(r.Stats().window_closed_by_reply, 1u);
}

TEST(LagRules, AReplyDoesNotEndTheWindowWhenTheConfigurationSaysSo)
{
	LagRulesConfig c = Config(LagRule::Bit(LagRule::Window));
	c.close_on_reply = false;
	LagRules r;
	r.Configure(c);
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	r.OnIopReply(kBase + 5000);
	EXPECT_EQ(At(r, kBase + 6000), kTight);
}

TEST(LagRules, AReplyEndsTheOutstandingRequestButNotAWindowSetToRunItsLength)
{
	LagRulesConfig c = Config(LagRule::Bit(LagRule::Window) | LagRule::Bit(LagRule::IdleOutstanding));
	c.window_close_on_reply = false;
	LagRules r;
	r.Configure(c);
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	r.OnIopReply(kBase + 5000);
	EXPECT_EQ(r.Stats().window_closed_by_reply, 0u);
	// The window is still open, with the EE running.
	EXPECT_EQ(At(r, kBase + 6000), kTight);
	EXPECT_EQ(r.LastRule(), static_cast<s32>(LagRule::Window));
	// Past the window the request is no longer outstanding: an idle EE is not held.
	EXPECT_EQ(At(r, kBase + 100001, true), kLoose);
}

TEST(LagRules, TheWindowChannelsSayWhichStartsOpenIt)
{
	LagRulesConfig c = Config(LagRule::Bit(LagRule::Window));
	c.window_channels = 2; // SIF1 only
	LagRules r;
	r.Configure(c);
	r.Reset();
	r.OnEeSifStart(kBase, 0);
	EXPECT_EQ(At(r, kBase + 10), kLoose);
	r.OnEeSifStart(kBase + 20, 1);
	EXPECT_EQ(At(r, kBase + 30), kTight);
}

TEST(LagRules, ARequestOutstandingHoldsTheBoundTightOnlyInAnIdleEventTest)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::IdleOutstanding)));
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	EXPECT_EQ(At(r, kBase + 3072, false), kLoose) << "a busy EE keeps its overlap";
	EXPECT_EQ(At(r, kBase + 6144, true), kTight);
	EXPECT_EQ(r.LastRule(), static_cast<s32>(LagRule::IdleOutstanding));
	r.OnIopReply(kBase + 7000);
	EXPECT_EQ(At(r, kBase + 9216, true), kLoose) << "the reply came: nothing is outstanding";
}

TEST(LagRules, OnlyAnEeStartedSendOnSif1MakesARequestOutstanding)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::IdleOutstanding)));
	r.Reset();
	r.OnEeSifStart(kBase, 0); // the EE arming SIF0 to receive is not a request
	EXPECT_EQ(At(r, kBase + 3072, true), kLoose);
}

TEST(LagRules, AnOldRequestIsNoLongerOutstanding)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::IdleOutstanding)));
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	EXPECT_EQ(At(r, kBase + 1'000'000, true), kTight);
	EXPECT_EQ(At(r, kBase + 1'000'001, true), kLoose);
	// A newer send makes it outstanding again.
	r.OnEeSifStart(kBase + 2'000'000, 1);
	EXPECT_EQ(At(r, kBase + 2'000'100, true), kTight);
}

TEST(LagRules, ANewSendRenewsTheAgeOfTheOutstandingRequest)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::IdleOutstanding)));
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	r.OnEeSifStart(kBase + 900'000, 1);
	EXPECT_EQ(At(r, kBase + 1'000'001, true), kTight) << "the second send is the request now; the first is forgotten";
	EXPECT_EQ(At(r, kBase + 1'900'001, true), kLoose);
}

TEST(LagRules, IdleAloneHoldsTheBoundTightInAnIdleEventTestOnly)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::IdleAny)));
	r.Reset();
	EXPECT_EQ(At(r, kBase, false), kLoose);
	EXPECT_EQ(At(r, kBase + 3072, true), kTight);
	EXPECT_EQ(r.LastRule(), static_cast<s32>(LagRule::IdleAny));
}

TEST(LagRules, AStreamHoldsTheBoundTightWhileItsBitIsSet)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::Stream)));
	r.Reset();
	EXPECT_EQ(At(r, kBase, false, 0), kLoose);
	EXPECT_EQ(At(r, kBase + 3072, false, StreamBit::SpuAdmaCore1), kTight);
	EXPECT_EQ(At(r, kBase + 6144, false, StreamBit::CdvdBusy), kTight);
	EXPECT_EQ(At(r, kBase + 9216, false, 0), kLoose);
	EXPECT_EQ(r.Stats().activations[LagRule::Stream], 1u) << "counted when the stream starts: the two bits ran back to back";
	EXPECT_EQ(At(r, kBase + 12288, false, StreamBit::CdvdBusy), kTight);
	EXPECT_EQ(r.Stats().activations[LagRule::Stream], 2u);
}

TEST(LagRules, OnlyTheStreamBitsTheConfigurationNamesCount)
{
	LagRulesConfig c = Config(LagRule::Bit(LagRule::Stream));
	c.stream_bits = StreamBit::SpuAdmaCore0;
	LagRules r;
	r.Configure(c);
	r.Reset();
	EXPECT_EQ(At(r, kBase, false, StreamBit::CdvdBusy), kLoose);
	EXPECT_EQ(At(r, kBase + 3072, false, StreamBit::SpuAdmaCore0), kTight);
}

TEST(LagRules, TheBootFramesAreTightFromTheFrameTheRulesWereResetAt)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::Boot)));
	r.Reset(50);
	EXPECT_EQ(At(r, kBase, false, 0, 50), kTight);
	EXPECT_EQ(At(r, kBase + 3072, false, 0, 59), kTight);
	EXPECT_EQ(At(r, kBase + 6144, false, 0, 60), kLoose);
	EXPECT_EQ(r.Stats().activations[LagRule::Boot], 1u);
}

TEST(LagRules, AMailboxAccessAndAPollOpenWindowsOfTheirOwn)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::Mailbox) | LagRule::Bit(LagRule::Poll)));
	r.Reset();
	r.OnMailboxAccess(kBase);
	EXPECT_EQ(At(r, kBase + 199999), kTight);
	EXPECT_EQ(r.LastRule(), static_cast<s32>(LagRule::Mailbox));
	EXPECT_EQ(At(r, kBase + 200000), kLoose);
	r.OnEePoll(kBase + 300000);
	EXPECT_EQ(At(r, kBase + 349999), kTight);
	EXPECT_EQ(r.LastRule(), static_cast<s32>(LagRule::Poll));
	EXPECT_EQ(At(r, kBase + 350000), kLoose);
}

TEST(LagRules, ARuleThatIsOffNeverFires)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::Stream)));
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	r.OnMailboxAccess(kBase);
	r.OnEePoll(kBase);
	EXPECT_EQ(At(r, kBase + 10, true, 0, 0), kLoose);
	EXPECT_EQ(r.Stats().activations[LagRule::Window], 0u);
	EXPECT_EQ(r.Stats().activations[LagRule::Mailbox], 0u);
	EXPECT_EQ(r.Stats().activations[LagRule::Poll], 0u);
}

TEST(LagRules, AnEventTestIsCreditedToTheFirstRuleThatHoldsAndItsSpanIsCounted)
{
	LagRules r;
	r.Configure(Config(LagRule::Bit(LagRule::Window) | LagRule::Bit(LagRule::IdleAny)));
	r.Reset();
	At(r, kBase); // the first event test has no span
	r.OnEeSifStart(kBase + 100, 1);
	EXPECT_EQ(At(r, kBase + 3172, true), kTight);
	EXPECT_EQ(r.Stats().event_tests[LagRule::Window], 1u);
	EXPECT_EQ(r.Stats().event_tests[LagRule::IdleAny], 0u);
	EXPECT_EQ(r.Stats().cycles[LagRule::Window], 3172u);
	EXPECT_EQ(r.Stats().total_event_tests, 2u);
	EXPECT_EQ(r.Stats().total_cycles, 3172u);
}

TEST(LagRules, TheTightBoundNeverGoesBelowTheMinimumLag)
{
	LagRulesConfig c = Config(LagRule::Bit(LagRule::Window));
	c.tight = 100;
	LagRules r;
	r.Configure(c);
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	EXPECT_EQ(At(r, kBase + 10), kMinLag);
}

TEST(LagRules, AResetForgetsEverything)
{
	LagRules r;
	r.Configure(Config(LagRule::kAll));
	r.Reset();
	r.OnEeSifStart(kBase, 1);
	r.OnMailboxAccess(kBase);
	At(r, kBase + 10);
	r.Reset(1000);
	EXPECT_EQ(r.Stats().total_event_tests, 0u);
	EXPECT_EQ(At(r, kBase + 20, false, 0, 5000), kLoose) << "no window, no outstanding request, past the boot frames";
}

TEST(LagRules, TheDefaultStreamBitsAreTheSpuAutoDmaAndNotTheDisc)
{
	LagRulesConfig c;
	c.mask = LagRule::Bit(LagRule::Stream);
	LagRules r;
	r.Configure(c);
	r.Reset();
	EXPECT_EQ(At(r, kBase, false, StreamBit::CdvdBusy), kLoose) << "a disc read alone is not a stream: a God of War II state reads the disc all the time";
	EXPECT_EQ(At(r, kBase + 3072, false, StreamBit::SpuAdmaCore0), static_cast<s64>(c.tight));
	EXPECT_EQ(At(r, kBase + 6144, false, StreamBit::SpuAdmaCore1 | StreamBit::CdvdBusy), static_cast<s64>(c.tight));
}

namespace
{
	// IOP addresses of two RPC servers' data; the first word of each is the server's id.
	constexpr u32 kMcServer = 0x000a1230;
	constexpr u32 kOtherServer = 0x000b4560;
	u32 ReadIop(u32 addr)
	{
		if (addr == kMcServer)
			return McServerCalls::kServerId;
		if (addr == kOtherServer)
			return 0x80000001;
		return 0;
	}

	// Where the IOP's SIF0 transfers land in EE RAM: the EE's command buffer (the end packets), and the EE's copy of a call packet.
	constexpr u32 kEeCmdBuffer = 0x00090000;
	constexpr u32 kEeCallPacket = 0x00470000;

	// The SIF command packets as they sit in IOP memory: an RPC call from the EE (64 bytes, as the game's library sends it) and the
	// server's RPC end.
	std::array<u32, 16> CallPacket(u32 client, u32 server, u32 function, u32 rmode = 1)
	{
		return {0x40, 0x0008f000, McServerCalls::kCmdRpcCall, 0, 1, kEeCallPacket, 7, client, function, 0x400, 0x00480000, 0x400, rmode, server, 0, 0};
	}
	std::array<u32, 12> EndPacket(u32 client, u32 server)
	{
		return {0x30, 0x00470000, McServerCalls::kCmdRpcEnd, 0, 1, 0x00470000, 7, client, McServerCalls::kCmdRpcCall, server, 0x00480000, 0};
	}
} // namespace

TEST(LagRules, ACallToTheMemoryCardServerIsInProgressFromItsPacketToItsEnd)
{
	McServerCalls calls;
	EXPECT_FALSE(calls.InProgress(kBase));

	const auto call = CallPacket(0x0047a900, kMcServer, 5);
	calls.OnSif1Transfer(call.data(), 16, ReadIop);
	EXPECT_TRUE(calls.InProgress(kBase + 100));
	EXPECT_TRUE(calls.InProgress(kBase + 4'000'000));

	// The end of another client's call, and data, change nothing.
	const auto other_end = EndPacket(0x00093600, kOtherServer);
	calls.OnSif0Transfer(other_end.data(), 12, kEeCmdBuffer);
	const std::array<u32, 12> data = {0x40, 0, McServerCalls::kCmdRpcCall, 0, 0, 0, 0, 0x0047a900, 0, 0, 0, 0};
	calls.OnSif0Transfer(data.data(), 12, kEeCmdBuffer);
	EXPECT_TRUE(calls.InProgress(kBase + 4'000'100));

	const auto end = EndPacket(0x0047a900, kMcServer);
	calls.OnSif0Transfer(end.data(), 12, kEeCmdBuffer);
	EXPECT_FALSE(calls.InProgress(kBase + 4'000'200));
}

TEST(LagRules, ACallToAnotherServerIsNotTracked)
{
	McServerCalls calls;
	const auto call = CallPacket(0x00093600, kOtherServer, 1);
	calls.OnSif1Transfer(call.data(), 16, ReadIop);
	EXPECT_FALSE(calls.InProgress(kBase));
	// Nor is a packet too short to be a call, or one that is not a call.
	auto bind = CallPacket(0x0047a900, kMcServer, 0);
	bind[2] = 0x80000009;
	calls.OnSif1Transfer(bind.data(), 16, ReadIop);
	const auto mc = CallPacket(0x0047a900, kMcServer, 5);
	calls.OnSif1Transfer(mc.data(), 12, ReadIop);
	EXPECT_FALSE(calls.InProgress(kBase));
}

TEST(LagRules, AMemoryCardCallWhoseEndNeverComesIsForgotten)
{
	McServerCalls calls;
	const auto call = CallPacket(0x0047a900, kMcServer, 5);
	calls.OnSif1Transfer(call.data(), 16, ReadIop);
	EXPECT_TRUE(calls.InProgress(kBase));
	EXPECT_TRUE(calls.InProgress(kBase + McServerCalls::kMaxAge));
	EXPECT_FALSE(calls.InProgress(kBase + McServerCalls::kMaxAge + 1)) << "the IOP was rebooted under it";

	calls.OnSif1Transfer(call.data(), 16, ReadIop);
	EXPECT_TRUE(calls.InProgress(kBase));
	calls.Reset();
	EXPECT_FALSE(calls.InProgress(kBase)) << "a state load or a VM reset";
}

// A call made without the no-wait mode gets no RPC end packet: the IOP writes the end straight into the EE's copy of the call packet.
TEST(LagRules, AMemoryCardCallWithNoEndPacketEndsWhenTheIopWritesItsPacketBack)
{
	McServerCalls calls;
	const auto call = CallPacket(0x0047a900, kMcServer, 2, 0);
	calls.OnSif1Transfer(call.data(), 16, ReadIop);
	const std::array<u32, 12> reply = {};
	calls.OnSif0Transfer(reply.data(), 12, 0x00480000); // the result, to the call's receive buffer
	EXPECT_TRUE(calls.InProgress(kBase));
	calls.OnSif0Transfer(reply.data(), 12, kEeCallPacket | 0x20000000); // the end, through the uncached mapping
	EXPECT_FALSE(calls.InProgress(kBase));
}

TEST(LagRules, WithEverySlotTakenANewMemoryCardCallPushesOutTheOldest)
{
	McServerCalls calls;
	for (u32 client = 1; client <= 5; client++)
	{
		const auto call = CallPacket(client, kMcServer, 5);
		calls.OnSif1Transfer(call.data(), 16, ReadIop);
	}
	EXPECT_TRUE(calls.InProgress(kBase));
	for (u32 client = 2; client <= 5; client++)
	{
		const auto end = EndPacket(client, kMcServer);
		calls.OnSif0Transfer(end.data(), 12, kEeCmdBuffer);
	}
	EXPECT_FALSE(calls.InProgress(kBase)) << "client 1's call was pushed out";
}

// A game loads its save: open, read, close, each a call to the memory card server that it polls for, with its own work between them.
// With the default rules the bound is the tight bound while each call is in progress, whatever the EE is doing, and the loose bound
// between calls and after the last. The calls a game makes twice a second to see whether the card was changed hold it tight only for
// as long as each takes.
TEST(LagRules, AMemoryCardCallHoldsTheBoundTightUntilItsEnd)
{
	LagRulesConfig c; // the default stream bits
	c.mask = LagRule::Bit(LagRule::Stream);
	LagRules r;
	r.Configure(c);
	r.Reset();
	McServerCalls calls;
	const s64 tight = c.tight;
	constexpr u64 kFrame = 4915200;
	constexpr u32 kClient = 0x0047a900;

	// The IOP samples its devices at the end of each slice; the EE's event test reads what it published last.
	const auto step = [&](u64 cycle) {
		return At(r, cycle, false, calls.InProgress(cycle) ? StreamBit::McServerCall : 0);
	};

	u64 t = kBase;
	for (const u32 function : {2u /* open */, 5u /* read */, 3u /* close */})
	{
		EXPECT_EQ(step(t), kLoose) << "the game's own work before call " << function;
		const auto call = CallPacket(kClient, kMcServer, function);
		calls.OnSif1Transfer(call.data(), 16, ReadIop);
		for (u64 in = 3072; in < kFrame; in += 3072)
			ASSERT_EQ(step(t + in), tight) << "call " << function << ", " << in << " cycles in";
		const auto end = EndPacket(kClient, kMcServer);
		calls.OnSif0Transfer(end.data(), 12, kEeCmdBuffer);
		t += kFrame + kFrame / 2;
	}
	EXPECT_EQ(step(t), kLoose);
	EXPECT_EQ(r.Stats().activations[LagRule::Stream], 3u) << "one stretch per call";
}
