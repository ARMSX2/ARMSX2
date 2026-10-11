// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The PINE commands that screenshot Big Picture (ImGui/FullscreenUIShot.h): what they do with a
// malformed request, and what they say when the host did not start with --headless. The commands
// that reach the GS need a running device and are exercised by running armsx2-sdl --headless and driving it over PINE.

#include <gtest/gtest.h>

#include "GS/Renderers/Common/GSHeadlessPresent.h"
#include "ImGui/FullscreenUIShot.h"

#include "common/Error.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	void AppendU32(std::vector<u8>& out, u32 value)
	{
		const u8* p = reinterpret_cast<const u8*>(&value);
		out.insert(out.end(), p, p + sizeof(value));
	}

	void AppendString(std::vector<u8>& out, std::string_view s)
	{
		AppendU32(out, static_cast<u32>(s.size()));
		out.insert(out.end(), s.begin(), s.end());
	}

	bool HandleCommand(u8 opcode, const std::vector<u8>& args, u32* consumed, std::string* reply)
	{
		return FullscreenUIShot::HandlePineCommand(opcode, args.data(), static_cast<u32>(args.size()), consumed, reply);
	}
} // namespace

TEST(FullscreenUIShot, NoArgumentsFailsEveryCommand)
{
	for (const u8 opcode : {FullscreenUIShot::OpScreenshot, FullscreenUIShot::OpOpenPage, FullscreenUIShot::OpWaitFrames})
	{
		u32 consumed = 0;
		std::string reply;
		EXPECT_FALSE(HandleCommand(opcode, {}, &consumed, &reply)) << "opcode " << static_cast<int>(opcode);
	}
}

TEST(FullscreenUIShot, StringLongerThanTheRequestFails)
{
	std::vector<u8> args;
	AppendU32(args, 1000);
	args.push_back('x');

	u32 consumed = 0;
	std::string reply;
	EXPECT_FALSE(HandleCommand(FullscreenUIShot::OpOpenPage, args, &consumed, &reply));
}

TEST(FullscreenUIShot, ScreenshotNeedsItsSettleCount)
{
	std::vector<u8> args;
	AppendString(args, "/tmp/shot.png");

	u32 consumed = 0;
	std::string reply;
	EXPECT_FALSE(HandleCommand(FullscreenUIShot::OpScreenshot, args, &consumed, &reply));
}

TEST(FullscreenUIShot, UnknownOpcodeFails)
{
	u32 consumed = 0;
	std::string reply;
	EXPECT_FALSE(HandleCommand(0x7E, {}, &consumed, &reply));
}

TEST(FullscreenUIShot, WaitFramesWithoutHeadlessSaysSoAndConsumesOnlyItsArguments)
{
	ASSERT_FALSE(GSHeadless::IsEnabled());

	std::vector<u8> args;
	AppendU32(args, 3);
	AppendU32(args, 1);
	args.push_back(0xAB); // the next command in a batched request

	u32 consumed = 0;
	std::string reply;
	ASSERT_TRUE(HandleCommand(FullscreenUIShot::OpWaitFrames, args, &consumed, &reply));
	EXPECT_EQ(consumed, 8u);
	EXPECT_NE(reply.find("\"ok\":false"), std::string::npos) << reply;
	EXPECT_NE(reply.find("--headless"), std::string::npos) << reply;
}

TEST(GSHeadless, CaptureWithoutHeadlessSaysHowToTurnItOn)
{
	ASSERT_FALSE(GSHeadless::IsEnabled());

	GSHeadless::Frame frame;
	Error error;
	EXPECT_FALSE(GSHeadless::CaptureFreshFrame(&frame, &error));
	EXPECT_NE(error.GetDescription().find("--headless"), std::string::npos) << error.GetDescription();
	EXPECT_TRUE(frame.pixels.empty());
}

TEST(GSHeadless, PngPathMustEndInPng)
{
	u32 width = 0, height = 0;
	Error error;
	EXPECT_FALSE(GSHeadless::CaptureFreshFrameToPNG("/tmp/shot.jpg", &width, &height, &error));
	EXPECT_NE(error.GetDescription().find(".png"), std::string::npos) << error.GetDescription();
}
