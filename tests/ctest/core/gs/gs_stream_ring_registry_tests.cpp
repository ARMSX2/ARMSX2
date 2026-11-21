// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The set of stream rings the device cleans before each submit (GS/Renderers/Common/GSStreamRingRegistry.h).
// A ring joins it when it is created and leaves when it is destroyed, so a ring added anywhere (the run machine's
// index and table rings were not in the device's hand-written list) is covered without anyone listing it.

#include "GS/Renderers/Common/GSStreamRingRegistry.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{
	struct FakeRing
	{
		int id;
		int flushes = 0;
	};

	std::vector<int> Visit(const GSStreamRingRegistry<FakeRing>& reg)
	{
		std::vector<int> ids;
		reg.ForEach([&](FakeRing& r) { ids.push_back(r.id); });
		return ids;
	}
} // namespace

TEST(GSStreamRingRegistry, EveryAddedRingIsVisitedOnce)
{
	GSStreamRingRegistry<FakeRing> reg;
	FakeRing a{1}, b{2}, c{3};
	reg.Add(&a);
	reg.Add(&b);
	reg.Add(&c);
	reg.Add(&b); // a second Create of the same ring
	EXPECT_EQ(Visit(reg), (std::vector<int>{1, 2, 3}));
	reg.ForEach([](FakeRing& r) { r.flushes++; });
	EXPECT_EQ(a.flushes, 1);
	EXPECT_EQ(b.flushes, 1);
	EXPECT_EQ(c.flushes, 1);
}

TEST(GSStreamRingRegistry, RemovedRingIsNotVisited)
{
	GSStreamRingRegistry<FakeRing> reg;
	FakeRing a{1}, b{2};
	reg.Add(&a);
	reg.Add(&b);
	reg.Remove(&a);
	reg.Remove(&a); // a destructor after Destroy
	EXPECT_EQ(Visit(reg), (std::vector<int>{2}));
	EXPECT_EQ(reg.Size(), 1u);
}

TEST(GSStreamRingRegistry, MovedRingIsVisitedAtItsNewAddress)
{
	GSStreamRingRegistry<FakeRing> reg;
	FakeRing a{1}, moved_to{1};
	reg.Add(&a);
	reg.Replace(&a, &moved_to);
	reg.ForEach([](FakeRing& r) { r.flushes++; });
	EXPECT_EQ(a.flushes, 0);
	EXPECT_EQ(moved_to.flushes, 1);

	// Moving a ring that was never created registers nothing.
	FakeRing never{9}, dst{9};
	reg.Replace(&never, &dst);
	EXPECT_EQ(reg.Size(), 1u);
}

TEST(GSStreamRingRegistry, SwapOfTwoRegisteredRingsKeepsBoth)
{
	// Move assignment swaps the two rings' buffers: both addresses stay registered.
	GSStreamRingRegistry<FakeRing> reg;
	FakeRing a{1}, b{2};
	reg.Add(&a);
	reg.Add(&b);
	reg.Swap(&a, &b);
	EXPECT_EQ(reg.Size(), 2u);
	reg.Remove(&a);
	EXPECT_EQ(Visit(reg), (std::vector<int>{2}));
}

TEST(GSStreamRingRegistry, SwapWithAnUnregisteredRingMovesTheEntry)
{
	GSStreamRingRegistry<FakeRing> reg;
	FakeRing a{1}, empty{0};
	reg.Add(&a);
	reg.Swap(&empty, &a); // `empty = std::move(a)`: empty now holds a's buffer, a holds nothing
	EXPECT_EQ(reg.Size(), 1u);
	std::vector<FakeRing*> seen;
	reg.ForEach([&](FakeRing& r) { seen.push_back(&r); });
	ASSERT_EQ(seen.size(), 1u);
	EXPECT_EQ(seen[0], &empty);
}
