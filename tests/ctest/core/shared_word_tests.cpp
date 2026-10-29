// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// SharedWord, the one idiom for a word two threads both touch. It is built on the compiler's atomic
// builtins rather than std::atomic_ref, which the Android NDK's libc++ does not have.

#include <gtest/gtest.h>

#include "IopThread/SharedWord.h"

#include <array>
#include <thread>

TEST(SharedWord, PlainLoadsAndStoresRoundTripEveryWidthTheEmulatorUses)
{
	u8 b = 0xAB;
	u16 h = 0xBEEF;
	s16 s = -1234;
	u32 w = 0xDEADBEEFu;
	u64 q = 0x0123456789ABCDEFull;
	float f = 1.5f;

	EXPECT_EQ(SharedWord::LoadRelaxed(b), 0xAB);
	EXPECT_EQ(SharedWord::LoadRelaxed(h), 0xBEEF);
	EXPECT_EQ(SharedWord::LoadRelaxed(s), -1234);
	EXPECT_EQ(SharedWord::LoadRelaxed(w), 0xDEADBEEFu);
	EXPECT_EQ(SharedWord::LoadRelaxed(q), 0x0123456789ABCDEFull);
	EXPECT_EQ(SharedWord::LoadRelaxed(f), 1.5f);

	SharedWord::StoreRelaxed<u8>(b, 0x12);
	SharedWord::StoreRelaxed<u16>(h, 0x3456);
	SharedWord::StoreRelaxed<s16>(s, 77);
	SharedWord::StoreRelaxed<u32>(w, 0x89ABCDEFu);
	SharedWord::StoreRelaxed<u64>(q, 0xFEDCBA9876543210ull);
	SharedWord::StoreRelaxed<float>(f, -0.25f);
	EXPECT_EQ(b, 0x12);
	EXPECT_EQ(h, 0x3456);
	EXPECT_EQ(s, 77);
	EXPECT_EQ(w, 0x89ABCDEFu);
	EXPECT_EQ(q, 0xFEDCBA9876543210ull);
	EXPECT_EQ(f, -0.25f);
}

TEST(SharedWord, AConstWordCanBeLoaded)
{
	const u32 w = 0x1234u;
	EXPECT_EQ(SharedWord::LoadRelaxedConst(w), 0x1234u);
	const float f = 2.0f;
	EXPECT_EQ(SharedWord::LoadRelaxedConst(f), 2.0f);
}

TEST(SharedWord, AcquireAndReleaseMoveTheValue)
{
	u32 w = 7;
	SharedWord::StoreRelease(w, 9);
	EXPECT_EQ(SharedWord::LoadAcquire(w), 9u);
}

TEST(SharedWord, FetchOrAndFetchAndReturnTheOldValue)
{
	u32 w = 0x0000F0F0u;
	EXPECT_EQ(SharedWord::FetchOr(w, 0x00000F00u), 0x0000F0F0u);
	EXPECT_EQ(w, 0x0000FFF0u);
	EXPECT_EQ(SharedWord::FetchAnd(w, 0xFFFFFF0Fu), 0x0000FFF0u);
	EXPECT_EQ(w, 0x0000FF00u);
}

TEST(SharedWord, UpdateAppliesTheFunctionAndReturnsTheOldValue)
{
	u32 w = 10;
	const u32 old = SharedWord::Update(w, [](u32 v) { return v * 3 + 1; });
	EXPECT_EQ(old, 10u);
	EXPECT_EQ(w, 31u);
}

TEST(SharedWord, UpdateRunsTheFunctionAgainWhenTheWordChangedUnderIt)
{
	// The other thread's store lands inside the function, between the load and the exchange.
	u32 w = 5;
	int calls = 0;
	const u32 old = SharedWord::Update(w, [&](u32 v) {
		if (calls++ == 0)
			SharedWord::StoreRelaxed<u32>(w, 100);
		return v + 1;
	});
	EXPECT_EQ(calls, 2);
	EXPECT_EQ(old, 100u);
	EXPECT_EQ(w, 101u);
}

TEST(SharedWord, TwoThreadsSettingDifferentBitsLoseNone)
{
	u32 w = 0;
	constexpr int kRounds = 200000;
	std::thread low([&] {
		for (int i = 0; i < kRounds; i++)
		{
			SharedWord::FetchOr(w, 0x1u);
			SharedWord::FetchAnd(w, ~0x1u);
			SharedWord::FetchOr(w, 0x1u);
		}
	});
	std::thread high([&] {
		for (int i = 0; i < kRounds; i++)
		{
			SharedWord::FetchOr(w, 0x80000000u);
			SharedWord::FetchAnd(w, ~0x80000000u);
			SharedWord::FetchOr(w, 0x80000000u);
		}
	});
	low.join();
	high.join();
	EXPECT_EQ(w, 0x80000001u);
}

TEST(SharedWord, TwoThreadsUpdatingOneWordLoseNoIncrement)
{
	u32 w = 0;
	constexpr int kRounds = 200000;
	const auto work = [&] {
		for (int i = 0; i < kRounds; i++)
			SharedWord::Update(w, [](u32 v) { return v + 1; });
	};
	std::thread a(work), b(work);
	a.join();
	b.join();
	EXPECT_EQ(w, static_cast<u32>(2 * kRounds));
}

TEST(SharedWord, RelaxedFieldsOfEveryTypeHoldTheirValue)
{
	std::array<SharedWord::Relaxed<u8>, 4> bytes = {};
	bytes[2] = 0xEE;
	EXPECT_EQ(static_cast<u8>(bytes[2]), 0xEE);
	EXPECT_EQ(static_cast<u8>(bytes[1]), 0);
	SharedWord::Relaxed<float> f{0.5f};
	f = 0.75f;
	EXPECT_EQ(static_cast<float>(f), 0.75f);
	SharedWord::Relaxed<u32> w{0xF0F0F0F0u};
	w &= 0x00FF00FFu;
	w |= 0x01000000u;
	EXPECT_EQ(static_cast<u32>(w), 0x01F000F0u);
}
