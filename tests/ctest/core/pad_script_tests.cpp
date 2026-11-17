// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pad scripts (SIO/Pad/PadScript.h): the text format, the frame each line takes effect at, what a held pad answers
// to a poll, and that a recording played back gives the game the input it recorded.

#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SIO/Pad/PadScript.h"

#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <unistd.h>
#include <string>
#include <vector>

using PadScript::Event;
using PadScript::EventKind;
using PadScript::PadFrame;

namespace
{
	constexpr u16 Bit(u32 input) { return static_cast<u16>(1u << input); }

	PadFrame Pressed(std::initializer_list<u32> inputs)
	{
		PadFrame frame;
		for (const u32 input : inputs)
		{
			frame.pressed |= Bit(input);
			frame.pressure[input] = 255;
		}
		return frame;
	}

	std::vector<Event> Parse(std::string_view text)
	{
		std::vector<Event> events;
		std::string error;
		EXPECT_TRUE(PadScript::ParseScript(text, &events, &error)) << error;
		return events;
	}

	std::string ParseError(std::string_view text)
	{
		std::vector<Event> events;
		std::string error;
		EXPECT_FALSE(PadScript::ParseScript(text, &events, &error)) << text;
		return error;
	}

	// The two button bytes of a poll as SIO2 drives it, active high: bit i is PadDualshock2's bit i pressed.
	u16 PolledButtons(PadBase& pad)
	{
		pad.SoftReset();
		u8 reply[4];
		reply[0] = pad.SendCommandByte(0x42);
		for (int i = 1; i < 4; i++)
			reply[i] = pad.SendCommandByte(0x00);
		for (int i = 4; i < 24; i++)
			pad.SendCommandByte(0x00);
		return static_cast<u16>(~((reply[2] << 8) | reply[3]));
	}

	// The DualShock 2's bit for each input, as in PadDualshock2::bitmaskMapping.
	constexpr u32 kCrossBit = 6;
	constexpr u32 kCircleBit = 5;
	constexpr u32 kR1Bit = 3;
} // namespace

TEST(PadScript, ParsesEveryLineKind)
{
	const std::vector<Event> events = Parse(
		"# a comment\n"
		"0 1 -\n"
		"\n"
		"60 1 cross+r1:96  # pressed\n"
		"64 2 up+square 127 0 255 127\n"
		"64 1 release\n"
		"600 end\n");
	ASSERT_EQ(events.size(), 5u);

	EXPECT_EQ(events[0].frame, 0u);
	EXPECT_EQ(events[0].kind, EventKind::Set);
	EXPECT_EQ(events[0].pad, 0);
	EXPECT_EQ(events[0].state, PadFrame{});

	PadFrame cross_r1 = Pressed({PadDualshock2::PAD_CROSS});
	cross_r1.pressed |= Bit(PadDualshock2::PAD_R1);
	cross_r1.pressure[PadDualshock2::PAD_R1] = 96;
	EXPECT_EQ(events[1].frame, 60u);
	EXPECT_EQ(events[1].state, cross_r1);

	PadFrame up_square = Pressed({PadDualshock2::PAD_UP, PadDualshock2::PAD_SQUARE});
	up_square.lx = 127;
	up_square.ly = 0;
	up_square.rx = 255;
	up_square.ry = 127;
	EXPECT_EQ(events[2].pad, 1);
	EXPECT_EQ(events[2].state, up_square);

	EXPECT_EQ(events[3].kind, EventKind::Release);
	EXPECT_EQ(events[3].pad, 0);
	EXPECT_EQ(events[4].kind, EventKind::End);
	EXPECT_EQ(events[4].frame, 600u);
}

TEST(PadScript, FormatsWhatItParses)
{
	const std::string text = "0 1 - 127 127 127 127\n"
							 "5 1 up+cross:3+l2:200+r3 0 255 12 127\n"
							 "9 8 select+start 127 127 127 127\n"
							 "9 1 release\n"
							 "12 end\n";
	const std::vector<Event> events = Parse(text);
	std::string formatted;
	for (const Event& event : events)
		formatted += PadScript::FormatEvent(event) + "\n";
	EXPECT_EQ(formatted, text);
}

TEST(PadScript, RejectsMalformedLinesAndNamesTheLine)
{
	EXPECT_EQ(ParseError("0 1 -\n1 1 crss\n"), "line 2: unknown button 'crss'");
	EXPECT_EQ(ParseError("5 1 -\n4 1 -\n"), "line 2: frame 4 comes before frame 5 above it");
	EXPECT_EQ(ParseError("0 9 -\n"), "line 1: bad pad '9' (1..8)");
	EXPECT_EQ(ParseError("0 0 -\n"), "line 1: bad pad '0' (1..8)");
	EXPECT_EQ(ParseError("0 1 - 1 2\n"), "line 1: expected <buttons> and either no sticks or all four");
	EXPECT_EQ(ParseError("0 1 - 1 2 3 256\n"), "line 1: bad stick value '256' (0..255)");
	EXPECT_EQ(ParseError("0 1 cross:300\n"), "line 1: bad pressure in 'cross:300' (0..255)");
	EXPECT_EQ(ParseError("x 1 -\n"), "line 1: bad frame 'x'");
	EXPECT_EQ(ParseError("3 end\n4 1 -\n"), "line 2: nothing may follow 'end'");
	EXPECT_EQ(ParseError("3 1\n"), "line 1: expected <frame> <pad> <buttons> [<lx> <ly> <rx> <ry>], <frame> <pad> release or <frame> end");
}

TEST(PadScript, ParsesThePadCommand)
{
	u8 pad = 0;
	std::optional<PadFrame> state;
	std::string error;
	ASSERT_TRUE(PadScript::ParsePadCommand("2 circle 10 20 30 40", &pad, &state, &error)) << error;
	EXPECT_EQ(pad, 1);
	ASSERT_TRUE(state.has_value());
	PadFrame expected = Pressed({PadDualshock2::PAD_CIRCLE});
	expected.lx = 10;
	expected.ly = 20;
	expected.rx = 30;
	expected.ry = 40;
	EXPECT_EQ(*state, expected);

	ASSERT_TRUE(PadScript::ParsePadCommand("1 release", &pad, &state, &error)) << error;
	EXPECT_EQ(pad, 0);
	EXPECT_FALSE(state.has_value());

	EXPECT_FALSE(PadScript::ParsePadCommand("", &pad, &state, &error));
	EXPECT_FALSE(PadScript::ParsePadCommand("1", &pad, &state, &error));
	EXPECT_FALSE(PadScript::ParsePadCommand("1 jump", &pad, &state, &error));
}

TEST(PadScript, SequencerAppliesEachLineAtItsFrameAndHoldsItUntilTheNext)
{
	PadScript::Sequencer sequencer(Parse("0 1 -\n3 1 cross\n3 2 circle\n5 1 release\n8 end\n"));
	const PadFrame cross = Pressed({PadDualshock2::PAD_CROSS});
	const PadFrame circle = Pressed({PadDualshock2::PAD_CIRCLE});

	struct Expect
	{
		std::optional<PadFrame> pad1, pad2;
		bool finished;
	};
	const Expect expected[] = {
		{PadFrame{}, std::nullopt, false}, // 0
		{PadFrame{}, std::nullopt, false}, // 1
		{PadFrame{}, std::nullopt, false}, // 2
		{cross, circle, false}, // 3
		{cross, circle, false}, // 4
		{std::nullopt, circle, false}, // 5
		{std::nullopt, circle, false}, // 6
		{std::nullopt, circle, false}, // 7
		{std::nullopt, std::nullopt, true}, // 8: end releases everything
		{std::nullopt, std::nullopt, true}, // 9
	};
	for (u32 frame = 0; frame < std::size(expected); frame++)
	{
		sequencer.Advance(frame);
		EXPECT_EQ(sequencer.Held()[0], expected[frame].pad1) << "frame " << frame;
		EXPECT_EQ(sequencer.Held()[1], expected[frame].pad2) << "frame " << frame;
		EXPECT_EQ(sequencer.Finished(), expected[frame].finished) << "frame " << frame;
	}
	EXPECT_EQ(sequencer.TouchedMask(), 3u);
}

TEST(PadScript, SequencerCatchesUpOnSkippedFrames)
{
	PadScript::Sequencer sequencer(Parse("2 1 cross\n4 1 circle\n"));
	sequencer.Advance(10);
	EXPECT_EQ(sequencer.Held()[0], Pressed({PadDualshock2::PAD_CIRCLE}));
	EXPECT_EQ(sequencer.Position(), 2u);
	EXPECT_FALSE(sequencer.Finished());
}

TEST(PadScript, AHeldDualShock2AnswersWithTheHeldStateAndThePlayerComesBack)
{
	PadDualshock2 pad(0, 0);
	pad.Set(PadDualshock2::PAD_CROSS, 1.0f);
	pad.Set(PadDualshock2::PAD_L_RIGHT, 1.0f);
	EXPECT_EQ(PolledButtons(pad), 1u << kCrossBit);

	PadFrame held = Pressed({PadDualshock2::PAD_CIRCLE, PadDualshock2::PAD_R1});
	held.pressure[PadDualshock2::PAD_R1] = 40;
	held.lx = 10;
	held.ly = 20;
	held.rx = 30;
	held.ry = 40;
	pad.SetHeldInput(&held);

	EXPECT_EQ(PolledButtons(pad), (1u << kCircleBit) | (1u << kR1Bit));
	EXPECT_EQ(pad.GetPressure(PadDualshock2::PAD_CIRCLE), 255);
	EXPECT_EQ(pad.GetPressure(PadDualshock2::PAD_R1), 40);
	EXPECT_EQ(pad.GetPressure(PadDualshock2::PAD_CROSS), 0);
	EXPECT_EQ(pad.GetPressure(PadDualshock2::PAD_L_RIGHT), 10);
	EXPECT_EQ(pad.GetPressure(PadDualshock2::PAD_L_UP), 20);
	EXPECT_EQ(pad.GetPressure(PadDualshock2::PAD_R_LEFT), 30);
	EXPECT_EQ(pad.GetPressure(PadDualshock2::PAD_R_DOWN), 40);

	// The player's input keeps arriving underneath and is not seen.
	pad.Set(PadDualshock2::PAD_SQUARE, 1.0f);
	EXPECT_EQ(PolledButtons(pad), (1u << kCircleBit) | (1u << kR1Bit));

	// What the player holds is still readable for a recording.
	PadFrame player;
	ASSERT_TRUE(pad.CapturePlayerInput(&player));
	EXPECT_EQ(player.pressed, Bit(PadDualshock2::PAD_CROSS) | Bit(PadDualshock2::PAD_SQUARE));
	EXPECT_GT(player.lx, PadScript::AXIS_CENTRE);

	pad.SetHeldInput(nullptr);
	EXPECT_EQ(PolledButtons(pad), (1u << kCrossBit) | (1u << 7));
	EXPECT_GT(std::get<0>(pad.GetRawLeftAnalog()), PadScript::AXIS_CENTRE);
}

namespace
{
	class PadScriptRuntime : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			MemorySettingsInterface si;
			Pad::LoadConfig(si);
			pad = Pad::GetPad(0);
			ASSERT_NE(pad, nullptr);
			ASSERT_EQ(pad->GetType(), Pad::ControllerType::DualShock2);
			dir = std::filesystem::temp_directory_path() / ("pad_script_tests_" + std::to_string(::getpid()));
			std::filesystem::create_directories(dir);
		}

		void TearDown() override
		{
			PadScript::OnShutdown();
			Pad::Shutdown();
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
		}

		PadBase* pad = nullptr;
		std::filesystem::path dir;
	};
} // namespace

// The vsync a line names is the vsync its state first reaches the pad, counted from the savestate load, and the
// state at frame 0 is in place before the game runs at all.
TEST_F(PadScriptRuntime, ALineReachesThePadAtTheVsyncItNames)
{
	Pad::SetControllerState(0, PadDualshock2::PAD_SQUARE, 1.0f); // the player holds square throughout

	PadScript::StartScript(Parse("0 1 -\n3 1 cross\n5 1 release\n"), true);
	EXPECT_EQ(PolledButtons(*pad), 1u << 7) << "an armed script waits for the load";

	PadScript::OnStateLoaded();
	EXPECT_EQ(PolledButtons(*pad), 0u) << "frame 0 holds the pad from the load on";

	const u16 expected[] = {0, 0, 1u << kCrossBit, 1u << kCrossBit, 1u << 7, 1u << 7};
	for (u32 frame = 1; frame <= std::size(expected); frame++)
	{
		PadScript::OnVsync();
		EXPECT_EQ(PolledButtons(*pad), expected[frame - 1]) << "frame " << frame;
	}
}

TEST_F(PadScriptRuntime, AScriptStartedWithoutALoadCountsFromTheNextVsync)
{
	PadScript::OnStateLoaded();
	PadScript::OnVsync();
	PadScript::OnVsync();

	PadScript::StartScript(Parse("0 1 circle\n2 end\n"), false);
	EXPECT_EQ(PolledButtons(*pad), 0u);
	PadScript::OnVsync(); // frame 0
	EXPECT_EQ(PolledButtons(*pad), 1u << kCircleBit);
	PadScript::OnVsync(); // frame 1
	EXPECT_EQ(PolledButtons(*pad), 1u << kCircleBit);
	PadScript::OnVsync(); // frame 2: end
	EXPECT_EQ(PolledButtons(*pad), 0u);
}

TEST_F(PadScriptRuntime, AClientHoldWinsOverTheScriptUntilReleased)
{
	PadScript::StartScript(Parse("0 1 cross\n"), true);
	PadScript::OnStateLoaded();
	EXPECT_EQ(PolledButtons(*pad), 1u << kCrossBit);

	PadScript::SetPad(0, Pressed({PadDualshock2::PAD_CIRCLE}));
	EXPECT_EQ(PolledButtons(*pad), 1u << kCrossBit) << "a client's hold waits for the vsync";
	PadScript::OnVsync();
	EXPECT_EQ(PolledButtons(*pad), 1u << kCircleBit);

	PadScript::SetPad(0, std::nullopt);
	PadScript::OnVsync();
	EXPECT_EQ(PolledButtons(*pad), 1u << kCrossBit);
}

TEST_F(PadScriptRuntime, ALoadEndsAScriptThatWasNotArmedForIt)
{
	PadScript::StartScript(Parse("0 1 cross\n"), false);
	PadScript::OnVsync();
	EXPECT_EQ(PolledButtons(*pad), 1u << kCrossBit);
	PadScript::OnStateLoaded();
	EXPECT_EQ(PolledButtons(*pad), 0u);
}

// A recording holds the pad in the sample it writes, so replaying the file gives the game the same input on the
// same frames.
TEST_F(PadScriptRuntime, ARecordingPlayedBackGivesTheGameTheSameInput)
{
	const std::string path = (dir / "rec.txt").string();
	std::string error;
	ASSERT_TRUE(PadScript::StartRecording(path, 0, true, &error)) << error;

	// What the player does at the start of each frame, from the load (frame 0) on.
	struct Step
	{
		u32 input;
		float value;
	};
	const std::vector<std::vector<Step>> player = {
		{},
		{{PadDualshock2::PAD_CROSS, 1.0f}},
		{},
		{{PadDualshock2::PAD_L_UP, 1.0f}, {PadDualshock2::PAD_R2, 0.5f}},
		{{PadDualshock2::PAD_CROSS, 0.0f}},
		{},
		{{PadDualshock2::PAD_L_UP, 0.0f}, {PadDualshock2::PAD_R2, 0.0f}},
	};

	std::vector<u16> seen_buttons;
	std::vector<u8> seen_ly;
	std::vector<u8> seen_r2;
	for (size_t frame = 0; frame < player.size(); frame++)
	{
		for (const Step& step : player[frame])
			Pad::SetControllerState(0, step.input, step.value);
		if (frame == 0)
			PadScript::OnStateLoaded();
		else
			PadScript::OnVsync();
		seen_buttons.push_back(PolledButtons(*pad));
		seen_ly.push_back(pad->GetPressure(PadDualshock2::PAD_L_UP));
		seen_r2.push_back(pad->GetPressure(PadDualshock2::PAD_R2));
	}
	const std::string reply = PadScript::StopRecording();
	EXPECT_NE(reply.find("\"ok\":true"), std::string::npos) << reply;
	EXPECT_NE(reply.find("\"frames\":7"), std::string::npos) << reply;

	std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
	ASSERT_TRUE(text.has_value());
	const std::vector<Event> events = Parse(*text);
	ASSERT_FALSE(events.empty());
	EXPECT_EQ(events.back().kind, EventKind::End);
	EXPECT_EQ(events.back().frame, 7u);

	// The player lets go of everything and plays nothing; the script plays it back.
	Pad::ResetControllerInputs(0);
	PadScript::StartScript(events, true);
	for (size_t frame = 0; frame < player.size(); frame++)
	{
		if (frame == 0)
			PadScript::OnStateLoaded();
		else
			PadScript::OnVsync();
		EXPECT_EQ(PolledButtons(*pad), seen_buttons[frame]) << "frame " << frame;
		EXPECT_EQ(pad->GetPressure(PadDualshock2::PAD_L_UP), seen_ly[frame]) << "frame " << frame;
		EXPECT_EQ(pad->GetPressure(PadDualshock2::PAD_R2), seen_r2[frame]) << "frame " << frame;
	}
	EXPECT_NE(seen_buttons[1], seen_buttons[0]) << "the recording saw the press";
}
