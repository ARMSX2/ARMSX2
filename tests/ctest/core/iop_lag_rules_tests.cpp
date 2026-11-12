// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// LagRules (IopThread/IopLagRules.h): when the EE must not run far ahead of the IOP. The class reads no global, so these tests drive it
// with events and event tests at chosen EE cycles.

#include <gtest/gtest.h>

#include "IopThread/IopLagRules.h"

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
