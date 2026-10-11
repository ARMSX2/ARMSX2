// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Catalog::BlockingAnswer (pcsx2/Catalog/BlockingAnswer.h): the hand-off that lets the CPU thread
// wait for an answer the GS thread collects. The property that matters is that Wait() always
// returns, so each way a question can be abandoned has a test.

#include "Catalog/BlockingAnswer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace
{
	using Answer = Catalog::BlockingAnswer<int>;

	constexpr int FALLBACK = -1;

	std::shared_ptr<Answer> MakeAnswer()
	{
		return std::make_shared<Answer>();
	}
} // namespace

TEST(BlockingAnswer, AnAnswerGivenBeforeWaitingIsReturnedWithoutPolling)
{
	auto answer = MakeAnswer();
	EXPECT_TRUE(answer->Set(7));

	int polls = 0;
	EXPECT_EQ(answer->Wait(FALLBACK, 1ms, [&polls] { polls++; return true; }), 7);
	EXPECT_EQ(polls, 0);
}

TEST(BlockingAnswer, AnAnswerFromAnotherThreadWakesTheWaiter)
{
	auto answer = MakeAnswer();
	Answer::Responder responder(answer, FALLBACK);

	std::thread other([responder] {
		std::this_thread::sleep_for(30ms);
		responder(42);
	});

	EXPECT_EQ(answer->Wait(FALLBACK, 5ms, [] { return true; }), 42);
	other.join();
}

TEST(BlockingAnswer, TheFirstAnswerWins)
{
	auto answer = MakeAnswer();
	EXPECT_TRUE(answer->Set(1));
	EXPECT_FALSE(answer->Set(2));
	EXPECT_EQ(answer->Wait(FALLBACK, 1ms, nullptr), 1);
}

TEST(BlockingAnswer, TheKeepWaitingFunctionRunsOnTheWaitingThreadWhileWaiting)
{
	auto answer = MakeAnswer();
	Answer::Responder responder(answer, FALLBACK); // held, never used: the wait must end some other way

	const std::thread::id waiter = std::this_thread::get_id();
	int polls = 0;
	bool on_waiting_thread = true;
	const int result = answer->Wait(FALLBACK, 2ms, [&] {
		on_waiting_thread = on_waiting_thread && (std::this_thread::get_id() == waiter);
		return ++polls < 5;
	});

	EXPECT_EQ(result, FALLBACK);
	EXPECT_EQ(polls, 5);
	EXPECT_TRUE(on_waiting_thread);
}

TEST(BlockingAnswer, GivingUpRecordsTheFallbackSoALateAnswerIsIgnored)
{
	auto answer = MakeAnswer();
	Answer::Responder responder(answer, FALLBACK);

	EXPECT_EQ(answer->Wait(FALLBACK, 1ms, [] { return false; }), FALLBACK);

	responder(5); // the user clicked after boot moved on
	EXPECT_EQ(answer->Wait(99, 1ms, nullptr), FALLBACK);
}

TEST(BlockingAnswer, DestroyingTheResponderWithoutAnsweringGivesTheFallback)
{
	auto answer = MakeAnswer();
	auto responder = std::make_unique<Answer::Responder>(answer, FALLBACK);

	// The dialog was replaced: its callback, holding the only copy, is destroyed on the GS thread.
	std::thread other([&responder] {
		std::this_thread::sleep_for(30ms);
		responder.reset();
	});

	// No keep-waiting function at all: only the destruction can end this wait.
	EXPECT_EQ(answer->Wait(FALLBACK, 5ms, nullptr), FALLBACK);
	other.join();
}

TEST(BlockingAnswer, TheFallbackIsOnlyGivenWhenTheLastCopyOfTheResponderIsGone)
{
	auto answer = MakeAnswer();
	auto first = std::make_unique<Answer::Responder>(answer, FALLBACK);
	auto copy = std::make_unique<Answer::Responder>(*first);

	first.reset();
	EXPECT_FALSE(answer->IsAnswered()); // the copy can still answer

	(*copy)(8);
	copy.reset();
	EXPECT_EQ(answer->Wait(FALLBACK, 1ms, nullptr), 8);
}

TEST(BlockingAnswer, AnAnswerInsideTheKeepWaitingFunctionIsReturned)
{
	auto answer = MakeAnswer();
	Answer::Responder responder(answer, FALLBACK);

	int polls = 0;
	const int result = answer->Wait(FALLBACK, 1ms, [&] {
		if (++polls == 3)
			responder(11);
		return true;
	});
	EXPECT_EQ(result, 11);
}

TEST(BlockingAnswer, AnAnswerThatLosesTheRaceWithGivingUpStillGivesOneConsistentResult)
{
	// Whichever lands first is the answer, and every reader sees that same value.
	for (int i = 0; i < 200; i++)
	{
		auto answer = MakeAnswer();
		Answer::Responder responder(answer, FALLBACK);
		std::atomic_bool go{false};

		std::thread other([&] {
			while (!go.load())
				std::this_thread::yield();
			responder(3);
		});

		go.store(true);
		const int result = answer->Wait(FALLBACK, 1ms, [] { return false; });
		other.join();

		EXPECT_TRUE(result == 3 || result == FALLBACK);
		EXPECT_EQ(answer->Wait(99, 1ms, nullptr), result);
	}
}

// ---- the bounded wait -------------------------------------------------------------------

TEST(BlockingAnswerWaitFor, GivesUpAtTheDeadlineWithTheFallback)
{
	auto answer = MakeAnswer();
	const auto start = std::chrono::steady_clock::now();
	EXPECT_EQ(answer->WaitFor(80ms, FALLBACK, 5ms, [] { return true; }), FALLBACK);
	const auto waited = std::chrono::steady_clock::now() - start;
	EXPECT_GE(waited, 80ms);
	EXPECT_LT(waited, 5s);

	// It is recorded: an answer after giving up changes nothing.
	EXPECT_FALSE(answer->Set(3));
	EXPECT_EQ(answer->WaitFor(1s, 0, 1ms, nullptr), FALLBACK);
}

TEST(BlockingAnswerWaitFor, AnAnswerInTimeIsReturned)
{
	auto answer = MakeAnswer();
	Answer::Responder responder(answer, FALLBACK);
	std::thread other([responder] {
		std::this_thread::sleep_for(30ms);
		responder(9);
	});
	EXPECT_EQ(answer->WaitFor(10s, FALLBACK, 5ms, [] { return true; }), 9);
	other.join();
}

TEST(BlockingAnswerWaitFor, TheKeepWaitingFunctionRunsUntilTheDeadlineAndCanEndItEarly)
{
	{
		auto answer = MakeAnswer();
		int polls = 0;
		EXPECT_EQ(answer->WaitFor(60ms, FALLBACK, 5ms, [&polls] { polls++; return true; }), FALLBACK);
		EXPECT_GE(polls, 2);
	}
	{
		auto answer = MakeAnswer();
		const auto start = std::chrono::steady_clock::now();
		EXPECT_EQ(answer->WaitFor(10s, FALLBACK, 5ms, [] { return false; }), FALLBACK);
		EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
	}
}
