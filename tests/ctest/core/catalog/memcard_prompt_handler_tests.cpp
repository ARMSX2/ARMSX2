// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Which restore-prompt handler a boot calls when both the host (Qt) and Big Picture have one
// (pcsx2/SIO/Memcard/MemcardBackup.h): the host's wins, whichever was registered first.

#include "SIO/Memcard/MemcardBackup.h"

#include <gtest/gtest.h>

using namespace MemcardBackup;

namespace
{
	// The handlers are process-wide, so each test starts and ends with none.
	struct ScopedNoHandlers
	{
		ScopedNoHandlers()
		{
			SetRestorePromptHandler(RestorePromptHandler());
			SetFallbackRestorePromptHandler(RestorePromptHandler());
		}
		~ScopedNoHandlers()
		{
			SetRestorePromptHandler(RestorePromptHandler());
			SetFallbackRestorePromptHandler(RestorePromptHandler());
		}
	};

	RestorePromptHandler Answering(RestoreAnswer answer)
	{
		return [answer](const RestorePrompt&) { return answer; };
	}

	// What a boot would be told right now.
	std::optional<RestoreAnswer> WouldAnswer()
	{
		const RestorePromptHandler handler = GetRestorePromptHandler();
		if (!handler)
			return std::nullopt;
		return handler(RestorePrompt());
	}
} // namespace

TEST(MemcardPromptHandler, WithNeitherRegisteredThereIsNoHandler)
{
	ScopedNoHandlers guard;
	EXPECT_FALSE(WouldAnswer().has_value());
}

TEST(MemcardPromptHandler, TheFallbackIsUsedWhenTheHostHasNone)
{
	ScopedNoHandlers guard;
	SetFallbackRestorePromptHandler(Answering(RestoreAnswer::Restore));
	EXPECT_EQ(WouldAnswer(), RestoreAnswer::Restore);
}

TEST(MemcardPromptHandler, TheHostHandlerWinsWhenItWasRegisteredFirst)
{
	ScopedNoHandlers guard;
	SetRestorePromptHandler(Answering(RestoreAnswer::StartAnyway));
	SetFallbackRestorePromptHandler(Answering(RestoreAnswer::Restore));
	EXPECT_EQ(WouldAnswer(), RestoreAnswer::StartAnyway);
}

TEST(MemcardPromptHandler, TheHostHandlerWinsWhenItWasRegisteredLast)
{
	ScopedNoHandlers guard;
	SetFallbackRestorePromptHandler(Answering(RestoreAnswer::Restore));
	SetRestorePromptHandler(Answering(RestoreAnswer::StartAnyway));
	EXPECT_EQ(WouldAnswer(), RestoreAnswer::StartAnyway);
}

TEST(MemcardPromptHandler, ClearingTheHostHandlerBringsTheFallbackBack)
{
	ScopedNoHandlers guard;
	SetRestorePromptHandler(Answering(RestoreAnswer::StartAnyway));
	SetFallbackRestorePromptHandler(Answering(RestoreAnswer::Restore));
	SetRestorePromptHandler(RestorePromptHandler());
	EXPECT_EQ(WouldAnswer(), RestoreAnswer::Restore);
}

TEST(MemcardPromptHandler, ClearingTheFallbackLeavesTheHostHandler)
{
	ScopedNoHandlers guard;
	SetRestorePromptHandler(Answering(RestoreAnswer::StartAnyway));
	SetFallbackRestorePromptHandler(Answering(RestoreAnswer::Restore));
	SetFallbackRestorePromptHandler(RestorePromptHandler());
	EXPECT_EQ(WouldAnswer(), RestoreAnswer::StartAnyway);
}
