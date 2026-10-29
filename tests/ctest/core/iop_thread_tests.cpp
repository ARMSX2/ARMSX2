// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the IOP thread object (IopThread/IopThread.h): the thread, its pacing loop, the
// watermark and the published position, holds, CallOnEE, the wake-up rules and the Lockstep
// handoff. The IOP is a fake: it advances its position by whatever it is told to run, in blocks of
// a fixed size so it overshoots like the real JIT does, and it records every slice and every item
// it applies. The EE is the test's own thread.
//
// Two things keep a bug in the protocol from hanging the suite:
//  - every test runs under a watchdog. When it fires it raises `aborted`, which the EE-side spin
//    loops see through the stop predicate the object lets the caller inject, so a deadlocked spin
//    bails out and the test fails with a message. If a thread is stuck somewhere no predicate
//    reaches, the watchdog ends the process a few seconds later, which fails the run too;
//  - the fake IOP's own waits (latches, polls) also watch `aborted`.
//
// This file includes nothing from the emulator except IopThread.h, which only needs the common
// threading and floating-point-control headers. It also builds on its own with
// -fsanitize=thread, which is what checks the memory orderings.

#include <gtest/gtest.h>

#include "IopThread/IopThread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#define IOP_THREAD_TEST_CAN_READ_THREAD_NAME 1
#endif

// How long a test may run before its watchdog raises `aborted`. Overridable at compile time; a
// normal run finishes in well under a second per test.
#ifndef IOP_THREAD_TEST_LIMIT_MS
#define IOP_THREAD_TEST_LIMIT_MS 30000
#endif

using namespace IopThread;
using namespace std::chrono_literals;

namespace
{
	using Clock = std::chrono::steady_clock;

	// A lag bound no test reaches, for tests that are about something else.
	constexpr s32 kNeverBinds = 1 << 30;

	class Rng
	{
	public:
		explicit Rng(u64 seed)
			: m_state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull)
		{
			if (m_state == 0)
				m_state = 1;
		}
		u64 Next()
		{
			m_state ^= m_state >> 12;
			m_state ^= m_state << 25;
			m_state ^= m_state >> 27;
			return m_state * 0x2545F4914F6CDD1Dull;
		}
		u32 Below(u32 n) { return static_cast<u32>((Next() >> 32) % n); }

	private:
		u64 m_state;
	};

	// The failures threads other than the test's own find. gtest assertions are not used off the
	// test thread, so the fake IOP and the callbacks report here and the fixture checks at the end.
	class Errors
	{
	public:
		void Add(std::string message)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_messages.size() < 20)
				m_messages.push_back(std::move(message));
		}
		bool Empty() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_messages.empty();
		}
		std::string Text() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			std::string text;
			for (const std::string& m : m_messages)
				text += m + "\n";
			return text;
		}

	private:
		mutable std::mutex m_mutex;
		std::vector<std::string> m_messages;
	};

	// Raises `on_expire` when the limit passes, and ends the process if the test still has not
	// finished a few seconds after that. Disarmed by destruction.
	class Watchdog
	{
	public:
		Watchdog(std::string test_name, std::chrono::milliseconds limit, std::function<void()> on_expire)
			: m_name(std::move(test_name))
		{
			m_thread = std::thread([this, limit, on_expire = std::move(on_expire)] {
				std::unique_lock<std::mutex> lock(m_mutex);
				if (m_cv.wait_for(lock, limit, [this] { return m_disarmed; }))
					return;
				m_expired.store(true);
				on_expire();
				if (m_cv.wait_for(lock, 5s, [this] { return m_disarmed; }))
					return;
				std::fprintf(stderr, "[  HUNG  ] %s: still running 5 s after its watchdog fired; a thread is stuck. Exiting.\n", m_name.c_str());
				std::fflush(stderr);
				std::_Exit(124);
			});
		}

		~Watchdog()
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_disarmed = true;
			}
			m_cv.notify_all();
			m_thread.join();
		}

		Watchdog(const Watchdog&) = delete;
		Watchdog& operator=(const Watchdog&) = delete;

		bool Expired() const { return m_expired.load(); }

	private:
		std::string m_name;
		std::mutex m_mutex;
		std::condition_variable m_cv;
		bool m_disarmed = false;
		std::atomic<bool> m_expired{false};
		std::thread m_thread;
	};

	// A gate a fake IOP waits at until the test opens it.
	class Latch
	{
	public:
		void Open() { m_open.store(true, std::memory_order_release); }
		bool IsOpen() const { return m_open.load(std::memory_order_acquire); }
		// False if `aborted` was raised first.
		bool Wait(const std::atomic<bool>& aborted) const
		{
			while (!IsOpen())
			{
				if (aborted.load(std::memory_order_relaxed))
					return false;
				std::this_thread::sleep_for(100us);
			}
			return true;
		}

	private:
		std::atomic<bool> m_open{false};
	};

	// Stops the thread when it goes out of scope. The IOP thread's callbacks capture the test's local
	// variables by reference, so the thread has to be gone before they are: declare one of these
	// after those locals and before Start(), and it is destroyed first. Without it a test that
	// returns while the IOP is still inside a callback has the IOP reading dead stack.
	class StopOnExit
	{
	public:
		explicit StopOnExit(Thread& thread)
			: m_thread(thread)
		{
		}
		~StopOnExit() { m_thread.Stop(); }
		StopOnExit(const StopOnExit&) = delete;
		StopOnExit& operator=(const StopOnExit&) = delete;

	private:
		Thread& m_thread;
	};

	// The fake IOP. `RunSlice` and `Apply` are what the object under test is given as its run-slice
	// and apply-item functions.
	struct FakeIop
	{
		struct AppliedItem
		{
			E2IItem item;
			u64 pos; // the IOP's position when it applied the item
		};
		struct SliceRecord
		{
			u64 from, target, to;
		};

		// Configuration. Set before Start().
		u32 block = 64; // the IOP ends a slice up to block-1 cycles past the target, like the JIT
		bool check_watermark = true; // a Threaded slice never runs past the published watermark
		bool check_stamps = true; // an item is applied only once the position has reached its stamp
		std::function<void(u64 from, u64 target)> in_slice_hook; // runs on the IOP thread inside a slice

		Errors* errors = nullptr;
		Thread* thread = nullptr;

		// State the IOP thread writes. The EE reads it only while it holds the IOP or after Stop(),
		// which is exactly the visibility the object has to provide.
		u64 pos = 0;
		u64 slice_count = 0;
		bool in_slice = false;
		u64 poke = 0; // the EE writes this under a hold; the IOP samples it at the start of each slice
		u64 poke_at_last_slice = 0;
		std::vector<AppliedItem> applied;
		std::vector<SliceRecord> slices;
		std::vector<u32> kicks;
		std::thread::id iop_tid;

		// Mirrors the EE polls while the IOP runs.
		std::atomic<u64> pos_mirror{0};
		std::atomic<u64> slices_started{0};
		std::atomic<u64> slices_done{0};
		std::atomic<u64> applied_count{0};

		void Reset()
		{
			pos = 0;
			slice_count = 0;
			in_slice = false;
			poke = 0;
			poke_at_last_slice = 0;
			applied.clear();
			slices.clear();
			kicks.clear();
			iop_tid = std::thread::id();
			pos_mirror.store(0);
			slices_started.store(0);
			slices_done.store(0);
			applied_count.store(0);
		}

		void Bind(Params& p)
		{
			p.run_slice = [this](u64 from, u64 target) { return RunSlice(from, target); };
			p.apply_item = [this](const E2IItem& item) { Apply(item); };
			p.apply_kicks = [this](u32 bits) {
				CheckThread("apply_kicks");
				kicks.push_back(bits);
			};
		}

		void Fail(std::string message) { errors->Add(std::move(message)); }

		// Every callback runs on one thread, and it is not the test's.
		void CheckThread(const char* what)
		{
			if (iop_tid == std::thread::id())
				iop_tid = std::this_thread::get_id();
			else if (iop_tid != std::this_thread::get_id())
				Fail(std::string(what) + " ran on a second thread");
		}

		u64 RunSlice(u64 from, u64 target)
		{
			CheckThread("run_slice");
			if (from != pos)
				Fail("a slice started at " + std::to_string(from) + " but the previous one ended at " + std::to_string(pos));
			if (in_slice)
				Fail("a slice started inside a slice");
			if (check_watermark && target > from && thread && target > thread->Watermark())
				Fail("a slice ran to " + std::to_string(target) + ", past the watermark " + std::to_string(thread->Watermark()));

			slices_started.fetch_add(1, std::memory_order_relaxed);
			in_slice = true;
			poke_at_last_slice = poke;
			if (in_slice_hook)
				in_slice_hook(from, target);

			u64 to = from;
			if (target > from)
				to = from + ((target - from + block - 1) / block) * block;

			slices.push_back({from, target, to});
			slice_count++;
			pos = to;
			in_slice = false;
			pos_mirror.store(to, std::memory_order_release);
			slices_done.fetch_add(1, std::memory_order_release);
			return to;
		}

		void Apply(const E2IItem& item)
		{
			CheckThread("apply_item");
			if (check_stamps && pos < item.stamp)
				Fail("item " + std::to_string(item.arg) + " stamped " + std::to_string(item.stamp) + " applied at position " + std::to_string(pos));
			applied.push_back({item, pos});
			applied_count.fetch_add(1, std::memory_order_release);
		}
	};

	class IopThreadObject : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
			watchdog = std::make_unique<Watchdog>(std::string(info->test_suite_name()) + "." + info->name(),
				std::chrono::milliseconds(IOP_THREAD_TEST_LIMIT_MS), [this] { aborted.store(true); });
			fake.errors = &errors;
			fake.thread = thread.get();
		}

		void TearDown() override
		{
			thread->Stop();
			EXPECT_TRUE(errors.Empty()) << errors.Text();
			EXPECT_FALSE(watchdog->Expired()) << "the test hit its time limit: something waited for ever";
			watchdog.reset();
		}

		Params BaseParams(IopThreadMode mode = IopThreadMode::Threaded)
		{
			Params p;
			p.mode = mode;
			p.lag = kNeverBinds;
			fake.check_watermark = (mode == IopThreadMode::Threaded);
			fake.check_stamps = (mode == IopThreadMode::Threaded);
			fake.Bind(p);
			p.ee_cycle = [this] { return ee_cycle; };
			p.should_stop = [this] {
				std::this_thread::yield();
				return aborted.load(std::memory_order_relaxed);
			};
			return p;
		}

		// Polls until `pred` holds. False on timeout or when the watchdog has fired.
		template <class Pred>
		bool WaitFor(Pred&& pred, std::chrono::milliseconds limit = 10s)
		{
			const auto end = Clock::now() + limit;
			while (!pred())
			{
				if (aborted.load(std::memory_order_relaxed) || Clock::now() > end)
					return false;
				std::this_thread::sleep_for(50us);
			}
			return true;
		}

		// WaitFor for the EE thread: each poll wakes a parked IOP, as the EE's next event test would.
		// The one wake-up the protocol accepts losing (iop_parked read just before the IOP sets it) is
		// made up that way, and a test that published once and only polled could hang on it.
		// The tests of the recovery itself do not use this.
		template <class Pred>
		bool WaitEe(Pred&& pred, std::chrono::milliseconds limit = 10s)
		{
			return WaitFor(
				[&] {
					if (thread->Mode() == IopThreadMode::Threaded)
						thread->Wake();
					return pred();
				},
				limit);
		}

		bool WaitParked()
		{
			return WaitEe([&] { return thread->IsParked(); });
		}

		// The IOP has committed to sleeping: it set iop_parked, re-checked for work, found none and
		// counted a park beyond `parks_before`. IsParked() alone is weaker, since the flag is set before
		// the re-check and a delayed thread can still find work the EE published after it saw the flag.
		// Does not wake the IOP: tests of lost wake-ups need one that only a wake-up can move.
		bool WaitAsleep(u64 parks_before = 0)
		{
			return WaitFor([&] { return thread->GetStats().parks > parks_before && thread->IsParked(); });
		}
		bool WaitPos(u64 pos)
		{
			return WaitEe([&] { return fake.pos_mirror.load(std::memory_order_acquire) >= pos; });
		}

		void Publish(u64 cycle)
		{
			ee_cycle = cycle;
			thread->PublishWatermark(cycle);
		}

		std::atomic<bool> aborted{false};
		Errors errors;
		FakeIop fake;
		std::unique_ptr<Thread> thread = std::make_unique<Thread>();
		u64 ee_cycle = 0;
		std::unique_ptr<Watchdog> watchdog;
	};

	class IopThreadObjectLockstep : public IopThreadObject
	{
	};

	TEST_F(IopThreadObject, StartAndStopRepeatedlyInBothModes)
	{
		for (int round = 0; round < 24; round++)
		{
			const IopThreadMode mode = (round & 1) ? IopThreadMode::Lockstep : IopThreadMode::Threaded;
			fake.Reset();
			ee_cycle = 0;
			ASSERT_TRUE(thread->Start(BaseParams(mode))) << "round " << round;
			EXPECT_TRUE(thread->IsRunning());
			EXPECT_EQ(thread->Mode(), mode);
			if (mode == IopThreadMode::Threaded)
			{
				Publish(1000);
				ASSERT_TRUE(WaitPos(1000)) << "round " << round;
			}
			else
			{
				EXPECT_GE(thread->LockstepRunSlice(1000), 1000u);
			}
			thread->Stop();
			EXPECT_FALSE(thread->IsRunning());
		}
	}

	TEST_F(IopThreadObject, StopWithoutStartIsHarmlessAndStartTwiceIsRefused)
	{
		thread->Stop();
		EXPECT_FALSE(thread->IsRunning());
		ASSERT_TRUE(thread->Start(BaseParams()));
		EXPECT_FALSE(thread->Start(BaseParams())) << "a second Start() on a running thread must be refused";
		EXPECT_TRUE(thread->IsRunning());
		thread->Stop();
		thread->Stop();
		EXPECT_FALSE(thread->IsRunning());
	}

	TEST_F(IopThreadObject, StartRefusesOffAndAMissingRunSlice)
	{
		Params off = BaseParams();
		off.mode = IopThreadMode::Off;
		EXPECT_FALSE(thread->Start(off));
		Params nothing_to_run = BaseParams();
		nothing_to_run.run_slice = nullptr;
		EXPECT_FALSE(thread->Start(nothing_to_run));
		EXPECT_FALSE(thread->IsRunning());
	}

	TEST_F(IopThreadObject, DestroyingTheObjectStopsTheThread)
	{
		auto local = std::make_unique<Thread>();
		FakeIop local_fake;
		local_fake.errors = &errors;
		local_fake.thread = local.get();
		Params p;
		p.mode = IopThreadMode::Threaded;
		p.lag = kNeverBinds;
		local_fake.Bind(p);
		p.should_stop = [this] { return aborted.load(); };
		ASSERT_TRUE(local->Start(p));
		local->PublishWatermark(500);
		ASSERT_TRUE(WaitEe([&] { return local_fake.pos_mirror.load() >= 500; }));
		local.reset(); // joins the thread; the fake must outlive it
		const u64 slices = local_fake.slices_done.load();
		std::this_thread::sleep_for(10ms);
		EXPECT_EQ(local_fake.slices_done.load(), slices);
	}

	TEST_F(IopThreadObject, TheThreadStartsParkedWithNothingToRun)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		ASSERT_TRUE(WaitParked());
		std::this_thread::sleep_for(5ms);
		EXPECT_EQ(fake.slices_started.load(), 0u);
		EXPECT_EQ(thread->PublishedPos(), 0u);
		EXPECT_FALSE(thread->OnIopThread()) << "the test thread is not the IOP thread";
	}

	TEST_F(IopThreadObject, TheIopThreadIsNamedAndTakesTheConfiguredFpcr)
	{
		FPControlRegister want = FPControlRegister::GetDefault();
		want.SetFlushToZero(true);
		want.SetRoundMode(FPRoundMode::ChopZero);
		const FPControlRegister ee_before = FPControlRegister::GetCurrent();
		ASSERT_NE(want, ee_before) << "the test needs an FPCR that differs from this thread's";

		std::string name;
		FPControlRegister seen = FPControlRegister::GetDefault();
		bool on_iop_thread = false;
		fake.in_slice_hook = [&](u64, u64) {
#ifdef IOP_THREAD_TEST_CAN_READ_THREAD_NAME
			char buffer[32] = {};
			pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
			name = buffer;
#endif
			seen = FPControlRegister::GetCurrent();
			on_iop_thread = thread->OnIopThread();
		};

		Params p = BaseParams();
		p.fpcr = want;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(100);
		ASSERT_TRUE(WaitPos(100));
		thread->Stop(); // the join orders the plain variables above

#ifdef IOP_THREAD_TEST_CAN_READ_THREAD_NAME
		EXPECT_EQ(name, "IOP");
#endif
		EXPECT_EQ(seen, want) << "the IOP thread must run with the FPCR it was given";
		EXPECT_TRUE(on_iop_thread);
		EXPECT_EQ(FPControlRegister::GetCurrent(), ee_before) << "starting the thread must not touch this thread's FPCR";
	}

	TEST_F(IopThreadObject, ReapplyFpcrReachesTheRunningIopThread)
	{
		FPControlRegister want = FPControlRegister::GetDefault();
		want.SetFlushToZero(true);
		want.SetRoundMode(FPRoundMode::NegativeInfinity);
		const FPControlRegister initial = FPControlRegister::GetDefault();
		ASSERT_NE(want, initial);

		std::atomic<u64> seen{0};
		fake.in_slice_hook = [&](u64, u64) {
			// The bitmask is a u32 on x86 and a u64 on ARM64; widen it.
			seen.store(static_cast<u64>(FPControlRegister::GetCurrent().bitmask), std::memory_order_relaxed);
		};
		Params p = BaseParams();
		p.fpcr = initial;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(100);
		ASSERT_TRUE(WaitPos(100));
		ASSERT_EQ(seen.load(), static_cast<u64>(initial.bitmask));

		thread->ReapplyFpcr(want);
		const bool reached = WaitEe([&] {
			Publish(ee_cycle + 100);
			return seen.load(std::memory_order_relaxed) == static_cast<u64>(want.bitmask);
		});
		EXPECT_TRUE(reached) << "a host command must re-apply the FPCR on the IOP thread";
	}

	TEST_F(IopThreadObject, TheIopRunsUpToTheWatermarkAndNoFurther)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		for (u64 wm : {1000u, 1000u, 4097u, 20000u})
		{
			Publish(wm);
			ASSERT_TRUE(WaitPos(wm)) << "wm " << wm;
			std::this_thread::sleep_for(3ms); // time in which a runaway IOP would have gone further
			const u64 pos = fake.pos_mirror.load();
			EXPECT_GE(pos, wm);
			EXPECT_LT(pos, wm + fake.block) << "the IOP may pass the watermark by one block, no more";
			EXPECT_EQ(thread->PublishedPos(), pos);
		}
	}

	TEST_F(IopThreadObject, WithNothingToDoTheIopIdlesAndRunsNoSlices)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(640);
		ASSERT_TRUE(WaitPos(640));
		ASSERT_TRUE(WaitParked());
		const u64 slices = fake.slices_done.load();
		std::this_thread::sleep_for(10ms);
		EXPECT_EQ(fake.slices_done.load(), slices) << "no budget means no slice";
		Publish(640); // the same watermark again is still no budget
		std::this_thread::sleep_for(5ms);
		EXPECT_EQ(fake.slices_done.load(), slices);
	}

	TEST_F(IopThreadObject, ASliceEndsAtTheNextItemStampWhateverTheWatermark)
	{
		// Hold the IOP inside its first slice while the EE queues everything, so the later slices
		// start with the watermark far ahead of the stamps.
		Latch gate;
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				gate.Wait(aborted);
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
		for (const auto& [stamp, arg] : std::vector<std::pair<u64, u32>>{{100, 0}, {250, 1}, {250, 2}, {700, 3}})
		{
			ee_cycle = stamp;
			ASSERT_TRUE(thread->PostE2I(stamp, 5, arg));
		}
		Publish(1000);
		gate.Open();
		ASSERT_TRUE(WaitPos(1000));
		thread->Stop();

		ASSERT_EQ(fake.slices.size(), 5u);
		EXPECT_EQ(fake.slices[0].target, 1u);
		EXPECT_EQ(fake.slices[1].target, 100u);
		EXPECT_EQ(fake.slices[2].target, 250u);
		EXPECT_EQ(fake.slices[3].target, 700u);
		EXPECT_EQ(fake.slices[4].target, 1000u);
		ASSERT_EQ(fake.applied.size(), 4u);
		for (u32 i = 0; i < 4; i++)
		{
			EXPECT_EQ(fake.applied[i].item.arg, i);
			EXPECT_EQ(fake.applied[i].item.kind, 5u);
			EXPECT_GE(fake.applied[i].pos, fake.applied[i].item.stamp);
			EXPECT_LT(fake.applied[i].pos - fake.applied[i].item.stamp, fake.block);
		}
	}

	TEST_F(IopThreadObject, ItemsAreAppliedInStampOrderAndNeverBeforeTheirStamp)
	{
		fake.block = 48;
		Rng rng(11);
		fake.in_slice_hook = [&, rng2 = Rng(5)](u64, u64) mutable {
			if (rng2.Below(64) == 0)
				std::this_thread::sleep_for(std::chrono::microseconds(20 + rng2.Below(150)));
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));

		u32 posted = 0;
		for (int step = 0; step < 6000; step++)
		{
			ee_cycle += 1 + rng.Below(400);
			if (rng.Below(3) == 0)
			{
				ASSERT_TRUE(thread->PostE2I(ee_cycle, static_cast<u16>(1 + rng.Below(3)), posted++));
				if (rng.Below(5) == 0) // a second item at the same cycle
					ASSERT_TRUE(thread->PostE2I(ee_cycle, 9, posted++));
			}
			thread->PublishWatermark(ee_cycle);
			if (thread->MayHaveI2E())
				thread->DrainI2E();
			ASSERT_TRUE(thread->CheckLag(ee_cycle));
		}
		{
			ScopedHold hold(*thread, HoldKind::Lifecycle);
			ASSERT_EQ(fake.applied.size(), posted) << "a hold must find every item stamped before it applied";
			u64 last_stamp = 0;
			for (u32 i = 0; i < posted; i++)
			{
				const auto& a = fake.applied[i];
				EXPECT_EQ(a.item.arg, i) << "items must be applied in the order they were posted";
				EXPECT_GE(a.item.stamp, last_stamp);
				EXPECT_GE(a.pos, a.item.stamp) << "item " << i << " applied before its stamp";
				EXPECT_LT(a.pos - a.item.stamp, fake.block) << "item " << i << " applied more than a block late";
				last_stamp = a.item.stamp;
			}
		}
	}

	TEST_F(IopThreadObject, AnItemStampedBehindThePositionIsAppliedOnTheNextPass)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1000);
		ASSERT_TRUE(WaitPos(1000));
		ASSERT_TRUE(WaitParked());
		const u64 pos = fake.pos_mirror.load();
		ASSERT_GT(pos, 1000u);
		// Posted at the EE's cycle, which the IOP's overshoot has already passed.
		ASSERT_TRUE(thread->PostE2I(1001, 3, 77));
		ASSERT_TRUE(WaitEe([&] { return fake.applied_count.load(std::memory_order_acquire) == 1; })) << "the item must wake the IOP";
		thread->Stop();
		ASSERT_EQ(fake.applied.size(), 1u);
		EXPECT_EQ(fake.applied[0].item.arg, 77u);
		EXPECT_GE(fake.applied[0].pos, 1001u);
	}

	TEST_F(IopThreadObject, AMinimumSliceKeepsSmallBudgetsFromRunningUnlessSomethingIsDue)
	{
		Params p = BaseParams();
		p.min_slice = 500;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));

		Publish(100);
		ASSERT_TRUE(WaitParked());
		std::this_thread::sleep_for(5ms);
		EXPECT_EQ(fake.slices_started.load(), 0u) << "100 cycles of budget is under the minimum";
		Publish(499);
		std::this_thread::sleep_for(5ms);
		EXPECT_EQ(fake.slices_started.load(), 0u);
		Publish(500);
		ASSERT_TRUE(WaitPos(500)) << "500 cycles of budget meets the minimum";

		// A small budget runs when an item is due at its end.
		const u64 pos = fake.pos_mirror.load();
		ASSERT_TRUE(WaitParked());
		const u64 before = fake.slices_done.load();
		ASSERT_TRUE(thread->PostE2I(pos + 10, 4, 1));
		ASSERT_TRUE(WaitEe([&] { return fake.applied_count.load(std::memory_order_acquire) == 1; })) << "an item due inside a small budget must not wait for the minimum";
		EXPECT_GT(fake.slices_done.load(), before);
	}

	TEST_F(IopThreadObject, AWaitingEeOverridesTheMinimumSlice)
	{
		Params p = BaseParams();
		p.min_slice = 500;
		p.lag = 100; // below the minimum, so the only reason to run is that the EE is waiting
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		ee_cycle = 300;
		thread->PublishWatermark(300);
		EXPECT_TRUE(thread->CheckLag(300)) << "the EE waits for a 300-cycle budget that is under the 500 minimum";
		EXPECT_GE(thread->PublishedPos(), 300u);
		EXPECT_GE(thread->GetStats().lag_spins, 1u);
	}

	TEST_F(IopThreadObject, AnIopInternalEventRunsASliceWithoutAnyBudget)
	{
		std::atomic<bool> event_pending{false};
		Params p = BaseParams();
		p.has_event = [&] { return event_pending.load(std::memory_order_acquire); };
		fake.in_slice_hook = [&](u64, u64) { event_pending.store(false, std::memory_order_release); };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		ASSERT_TRUE(WaitParked());
		event_pending.store(true, std::memory_order_release);
		thread->Wake();
		ASSERT_TRUE(WaitEe([&] { return fake.slices_done.load() == 1; })) << "an internal event is work even with a zero budget";
		EXPECT_EQ(thread->PublishedPos(), 0u);
	}

	TEST_F(IopThreadObject, TheLagBoundHoldsAfterEveryEventTest)
	{
		constexpr s32 kLag = 4096;
		Params p = BaseParams();
		p.lag = kLag;
		fake.in_slice_hook = [rng = Rng(3)](u64, u64) mutable {
			const u32 r = rng.Below(100);
			if (r == 0)
				std::this_thread::sleep_for(300us);
			else if (r < 5)
				for (int i = 0; i < 400; i++)
					asm volatile("" ::: "memory");
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));

		Rng rng(21);
		s64 worst_after = 0;
		for (int step = 0; step < 20000; step++)
		{
			ee_cycle += 1 + rng.Below(kEeWaitCycles);
			thread->PublishWatermark(ee_cycle);
			if (thread->MayHaveI2E())
				thread->DrainI2E();
			ASSERT_TRUE(thread->CheckLag(ee_cycle)) << "step " << step;
			const s64 lag = static_cast<s64>(ee_cycle - thread->PublishedPos());
			worst_after = std::max(worst_after, lag);
			ASSERT_LE(lag, kLag) << "the EE ran past the lag bound after an event test, step " << step;
		}
		EXPECT_GT(thread->GetStats().lag_spins, 0u) << "the EE never had to wait, so the bound was not exercised";
		RecordProperty("worst_lag_after_event_test", static_cast<int>(worst_after));
	}

	TEST_F(IopThreadObject, ASlowIopMakesTheEeSpinAndIsNotADeadlock)
	{
		Params p = BaseParams();
		p.lag = 1000;
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				std::this_thread::sleep_for(30ms);
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		const auto begin = Clock::now();
		Publish(10 * 1000);
		EXPECT_TRUE(thread->CheckLag(10 * 1000));
		EXPECT_GE(Clock::now() - begin, 25ms) << "the EE cannot get past a lag of 10 L until the stalled IOP has caught up";
		const Stats stats = thread->GetStats();
		EXPECT_EQ(stats.lag_spins, 1u);
		EXPECT_GT(stats.lag_spin_iterations, 1024u);
	}

	TEST_F(IopThreadObject, CheckLagReturnsAtOnceWhenTheLagIsWithinTheBound)
	{
		Params p = BaseParams();
		p.lag = 5000;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		EXPECT_TRUE(thread->CheckLag(5000)); // IOP at 0: lag 5000 is exactly the bound
		EXPECT_EQ(thread->GetStats().lag_spins, 0u);
		EXPECT_TRUE(thread->CheckLag(100));
	}

	TEST_F(IopThreadObject, KicksReachTheIopThreadAndCoalesce)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		ASSERT_TRUE(WaitAsleep()); // only the kick's own wake can move it now
		thread->KickIop(E2IKick::Sif0Space);
		ASSERT_TRUE(WaitFor([&] { return thread->GetStats().kicks_applied >= 1; })) << "a kick must wake a parked IOP";
		thread->KickIop(E2IKick::Sif1Data);
		thread->KickIop(E2IKick::Sif0Space | E2IKick::Sif1Data);
		ASSERT_TRUE(WaitEe([&] { return thread->GetStats().kicks_applied >= 2; }));
		thread->Stop();
		u32 all = 0;
		for (u32 k : fake.kicks)
			all |= k;
		EXPECT_EQ(all, E2IKick::Sif0Space | E2IKick::Sif1Data);
		EXPECT_EQ(fake.kicks.front(), E2IKick::Sif0Space);
	}

	TEST_F(IopThreadObject, HostCommandsRunOnTheIopThreadInPostOrderAndWakeAnIdleIop)
	{
		constexpr int kPosters = 3;
		constexpr int kEach = 200;
		struct Ran
		{
			int poster, index;
			bool on_iop;
		};
		std::vector<Ran> ran; // appended only by commands, which run on the IOP thread
		std::vector<u64> last_ticket(kPosters, 0);
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		ASSERT_TRUE(WaitParked());

		std::vector<std::thread> posters;
		for (int poster = 0; poster < kPosters; poster++)
		{
			posters.emplace_back([&, poster] {
				for (int i = 0; i < kEach; i++)
					last_ticket[poster] = thread->PostHostCommand([&, poster, i] { ran.push_back({poster, i, thread->OnIopThread()}); });
			});
		}
		for (std::thread& t : posters)
			t.join();
		const u64 final_ticket = *std::max_element(last_ticket.begin(), last_ticket.end());
		// No pokes from the EE here: posting a command has to wake a sleeping IOP by itself.
		ASSERT_TRUE(WaitFor([&] { return thread->IsHostCommandDone(final_ticket); })) << "the commands must run with no EE activity at all";
		thread->Stop();

		ASSERT_EQ(ran.size(), static_cast<size_t>(kPosters * kEach));
		std::vector<int> next(kPosters, 0);
		for (const Ran& r : ran)
		{
			EXPECT_TRUE(r.on_iop);
			EXPECT_EQ(r.index, next[r.poster]++) << "poster " << r.poster << " ran out of order";
		}
	}

	TEST_F(IopThreadObject, AHoldReachesItsTargetWithAConsistentViewAndFreezesTheIop)
	{
		fake.block = 32;
		Rng rng(7);
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));

		u32 posted = 0;
		u64 poke_before = 0;
		u64 slices_at_release = 0;
		for (int round = 0; round < 300; round++)
		{
			for (u32 steps = 1 + rng.Below(5); steps > 0; steps--)
			{
				ee_cycle += 1 + rng.Below(2000);
				if (rng.Below(2) == 0)
					ASSERT_TRUE(thread->PostE2I(ee_cycle, 2, posted++));
				thread->PublishWatermark(ee_cycle);
				ASSERT_TRUE(thread->CheckLag(ee_cycle));
			}
			const u64 target = ee_cycle;
			{
				ScopedHold hold(*thread, (round & 1) ? HoldKind::Emulation : HoldKind::Lifecycle);
				EXPECT_EQ(thread->HoldDepth(), 1u);

				// What the IOP wrote is visible and complete.
				EXPECT_GE(thread->PublishedPos(), target);
				EXPECT_EQ(fake.pos, thread->PublishedPos());
				EXPECT_FALSE(fake.in_slice);
				EXPECT_EQ(fake.applied.size(), posted) << "every item stamped at or before the hold's cycle must be applied";
				if (round > 0)
				{
					EXPECT_GT(fake.slice_count, slices_at_release) << "the IOP ran no slice since the last release";
					EXPECT_EQ(fake.poke_at_last_slice, poke_before) << "a write the EE made under the last hold must be visible to the IOP after it";
				}

				// The EE writes IOP state and carries on running; the IOP stays where it is.
				poke_before = static_cast<u64>(round) + 1;
				fake.poke = poke_before;
				const u64 slices = fake.slice_count;
				ee_cycle += 500;
				thread->PublishWatermark(ee_cycle);
				std::this_thread::sleep_for(30us);
				EXPECT_EQ(fake.slice_count, slices) << "the IOP ran during a hold";
				slices_at_release = fake.slice_count;
			}
		}
	}

	TEST_F(IopThreadObject, AHoldWaitsForTheSliceInProgress)
	{
		std::atomic<s64> slice_end_ns{0}; // when the stalled slice finished, on the steady clock
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				std::this_thread::sleep_for(25ms);
				slice_end_ns.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1000);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
		{
			ScopedHold hold(*thread, HoldKind::Lifecycle);
			const s64 granted_ns = Clock::now().time_since_epoch().count();
			const s64 ended_ns = slice_end_ns.load(std::memory_order_acquire);
			EXPECT_NE(ended_ns, 0) << "the hold was granted while the slice was still running";
			EXPECT_GE(granted_ns, ended_ns) << "the hold was granted before the slice in progress had ended";
			EXPECT_FALSE(fake.in_slice);
			EXPECT_EQ(fake.slices_done.load(), fake.slices_started.load());
			EXPECT_GE(fake.pos, 1000u);
		}
	}

	TEST_F(IopThreadObject, NestedHoldsHandshakeOnceAndReleaseOnlyWhenTheOutermostEnds)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1000);
		{
			ScopedHold outer(*thread);
			{
				ScopedHold middle(*thread, HoldKind::Emulation);
				ScopedHold inner(*thread);
				EXPECT_EQ(thread->HoldDepth(), 3u);
			}
			EXPECT_EQ(thread->HoldDepth(), 1u);
			EXPECT_EQ(thread->GetStats().hold_requests, 1u) << "nested holds must not repeat the handshake";

			// Still held: more budget does not make the IOP run.
			Publish(ee_cycle + 5000);
			std::this_thread::sleep_for(5ms);
			EXPECT_LT(fake.pos_mirror.load(), 1000u + fake.block + 1) << "the IOP ran while the outer hold was still live";
		}
		EXPECT_EQ(thread->HoldDepth(), 0u);
		ASSERT_TRUE(WaitPos(6000)) << "the IOP must resume when the outermost hold ends";
	}

	TEST_F(IopThreadObject, AHoldIsServedWhileTheIopIsBlockedInCallOnEE)
	{
		bool first = true;
		bool call_result = false;
		bool fn_ran = false;
		std::thread::id fn_thread;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				call_result = thread->CallOnEE([&] {
					fn_ran = true;
					fn_thread = std::this_thread::get_id();
				});
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1000);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
		{
			// The IOP is (or soon will be) blocked in CallOnEE; the hold spin must serve it, and the
			// IOP must then finish its slice and reach the hold.
			ScopedHold hold(*thread, HoldKind::Lifecycle);
			EXPECT_TRUE(fn_ran);
			EXPECT_EQ(fn_thread, std::this_thread::get_id());
			EXPECT_GE(fake.pos, 1000u);
		}
		thread->Stop();
		EXPECT_TRUE(call_result);
	}

	TEST_F(IopThreadObject, AHoldTakenInsideACallOnEEFunctionIsAMarker)
	{
		bool first = true;
		u64 depth = 0;
		u64 requests_inside = ~u64(0);
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				thread->CallOnEE([&] {
					// The IOP is blocked at a known point, so this hold has nothing to wait for. A real
					// handshake here would wait for an IOP that is waiting for this very function.
					const u64 before = thread->GetStats().hold_requests;
					ScopedHold hold(*thread, HoldKind::Emulation);
					depth = thread->HoldDepth();
					requests_inside = thread->GetStats().hold_requests - before;
				});
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1000);
		ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); }));
		EXPECT_EQ(thread->DrainI2E(), 1u);
		ASSERT_TRUE(WaitPos(1000));
		thread->Stop();
		EXPECT_EQ(depth, 1u);
		EXPECT_EQ(requests_inside, 0u);
	}

	TEST_F(IopThreadObject, AHoldInsideAFunctionServedByAHoldSpinDoesNotWithdrawTheOuterHold)
	{
		// The EE asks for a hold. The IOP is blocked in CallOnEE, so the hold's own spin runs the
		// function, and the function takes a hold of its own. That inner hold is a counter; ending it
		// must not end the outer request, which has not been granted yet.
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (!first)
				return;
			first = false;
			thread->CallOnEE([&] {
				ScopedHold inner(*thread, HoldKind::Emulation);
			});
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1000);
		ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); })) << "the IOP never reached CallOnEE";
		{
			ScopedHold outer(*thread, HoldKind::Emulation);
			EXPECT_EQ(thread->HoldDepth(), 1u);
			EXPECT_GE(fake.pos, 1000u);
			const u64 slices = fake.slice_count;
			Publish(ee_cycle + 3000);
			std::this_thread::sleep_for(2ms);
			EXPECT_EQ(fake.slice_count, slices) << "the IOP ran during the outer hold";
		}
		ASSERT_TRUE(WaitPos(4000));
	}

	TEST_F(IopThreadObject, ALifecycleHoldSleepsWhenTheIopIsStuckAndAnEmulationHoldNeverDoes)
	{
		std::atomic<u32> stall_ms{40}; // the next slice to start takes this long, once
		fake.in_slice_hook = [&](u64, u64) {
			const u32 ms = stall_ms.exchange(0);
			if (ms)
				std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1000);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
		{
			ScopedHold hold(*thread, HoldKind::Lifecycle);
		}
		EXPECT_GT(thread->GetStats().hold_sleeps, 0u) << "a lifecycle hold should stop spinning and sleep while the IOP is stuck";

		// The same stall again, taken as an emulation hold.
		stall_ms.store(40);
		Publish(ee_cycle + 3000);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 2; }));
		const u64 sleeps = thread->GetStats().hold_sleeps;
		const u64 spins = thread->GetStats().hold_spin_iterations;
		{
			ScopedHold hold(*thread, HoldKind::Emulation);
		}
		EXPECT_EQ(thread->GetStats().hold_sleeps, sleeps) << "an emulation hold must never sleep";
		EXPECT_GT(thread->GetStats().hold_spin_iterations, spins);
	}

	TEST_F(IopThreadObject, AHoldDoesNotGiveUpWhenTheStopPredicateFires)
	{
		// A hold never gives up when the stop predicate fires, however long the IOP's slice takes: its
		// caller is mid-operation (a SIF pump, the savestate a shutdown writes) and needs the IOP stopped.
		// The lag wait and the full queue wait do give up, since their callers can return to the event test.
		StopOnExit stop_on_exit(*thread);
		for (const HoldKind kind : {HoldKind::Emulation, HoldKind::Lifecycle})
		{
			fake.Reset();
			ee_cycle = 0;
			Latch gate;
			bool first = true;
			fake.in_slice_hook = [&](u64, u64) {
				if (first)
				{
					first = false;
					gate.Wait(aborted);
				}
			};
			std::atomic<u32> checks{0};
			Params p = BaseParams();
			p.should_stop = [&] {
				checks.fetch_add(1);
				return true;
			};
			ASSERT_TRUE(thread->Start(p));
			Publish(1000);
			ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
			std::thread opener([&] {
				std::this_thread::sleep_for(50ms);
				gate.Open();
			});
			{
				ScopedHold hold(*thread, kind);
				EXPECT_EQ(thread->HoldDepth(), 1u) << "the hold gave up while the IOP was still in its slice";
				EXPECT_GE(fake.pos, ee_cycle);
				const u64 slices = fake.slice_count;
				Publish(ee_cycle + 2000);
				std::this_thread::sleep_for(2ms);
				EXPECT_EQ(fake.slice_count, slices) << "the IOP ran during the hold";
			}
			opener.join();
			EXPECT_EQ(checks.load(), 0u) << "a hold must not ask the stop predicate";
			Publish(ee_cycle + 4000);
			ASSERT_TRUE(WaitPos(ee_cycle));
			thread->Stop();
			fake.in_slice_hook = nullptr;
		}
	}

	TEST_F(IopThreadObject, AHoldWithNoThreadRunningIsAMarker)
	{
		{
			ScopedHold hold(*thread);
			EXPECT_EQ(thread->HoldDepth(), 1u);
		}
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		thread->Stop();
		{
			ScopedHold hold(*thread, HoldKind::Emulation);
		}
		EXPECT_EQ(thread->GetStats().hold_requests, 0u);
	}

	TEST_F(IopThreadObject, AThreadStartedInsideAHoldComesUpHeld)
	{
		StopOnExit stop_on_exit(*thread);
		ee_cycle = 2000;
		fake.pos = 2000;
		fake.pos_mirror.store(2000);
		{
			ScopedHold hold(*thread); // nothing is running, so this is a marker
			Params p = BaseParams();
			p.start_ee_cycle = 2000;
			p.start_iop_pos = 2000;
			ASSERT_TRUE(thread->Start(p));
			Publish(9000); // budget the IOP must not use while the EE holds
			std::this_thread::sleep_for(15ms);
			EXPECT_EQ(fake.slices_started.load(), 0u) << "the thread ran before the hold that was live at Start() ended";
		}
		ASSERT_TRUE(WaitPos(9000)) << "the IOP must run once the hold ends";
	}

	TEST_F(IopThreadObject, AGrantedHoldHasEmptiedTheI2EQueueEvenWhenTheIopPostedLast)
	{
		// A Lifecycle hold sleeps between looks once the IOP has kept it waiting. If the IOP posts to
		// the EE at the very end of its slice and is held at once, the hold can be granted while the
		// EE sleeps, with that last item still queued. Only the drain after the grant empties it.
		std::atomic<bool> post_at_end{false};
		std::atomic<u32> posted{0};
		fake.in_slice_hook = [&](u64, u64) {
			if (post_at_end.exchange(false))
			{
				std::this_thread::sleep_for(400us); // long enough for the hold to be sleeping
				if (thread->PostI2E(I2EKind::SbusIrq, posted.load()))
					posted.fetch_add(1);
			}
		};
		u32 received = 0;
		Params p = BaseParams();
		p.handle_i2e = [&](const I2EItem&) { received++; };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		for (u32 round = 1; round <= 40; round++)
		{
			post_at_end.store(true);
			Publish(ee_cycle + 1000);
			ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == round; }));
			ScopedHold hold(*thread, HoldKind::Lifecycle);
			ASSERT_EQ(posted.load(), round);
			ASSERT_EQ(received, round) << "round " << round << ": the IOP's last item was left in the queue by the hold";
		}
	}

	TEST_F(IopThreadObject, HoldsTakenAndReleasedBackToBackKeepTheirOrdering)
	{
		// The EE writes IOP state under each hold and the IOP reads it in the first slice after the
		// release, which comes at once. The release has to carry the write to the IOP.
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		u64 previous_poke = 0;
		u64 slices_at_release = 0;
		for (u64 round = 1; round <= 3000; round++)
		{
			ee_cycle += 100;
			thread->PublishWatermark(ee_cycle);
			ScopedHold hold(*thread, HoldKind::Emulation);
			if (fake.slice_count > slices_at_release)
				ASSERT_EQ(fake.poke_at_last_slice, previous_poke) << "round " << round;
			fake.poke = round;
			previous_poke = round;
			slices_at_release = fake.slice_count;
		}
	}

	TEST_F(IopThreadObject, WhatTheIopDidBeforePublishingItsPositionIsVisibleToTheEeThatReadsIt)
	{
		// Nothing in the protocol depends on this ordering, but PublishedPos() promises it anyway: it
		// loads with acquire what the IOP stored with release. The only thing that orders the plain
		// array below is the position; ThreadSanitizer reports a race if the ordering is gone.
		constexpr u64 kRounds = 4000;
		std::vector<u64> written(kRounds + 2, 0);
		fake.block = 64;
		fake.in_slice_hook = [&](u64, u64 target) { written[target / 64] = target; };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		for (u64 round = 1; round <= kRounds; round++)
		{
			const u64 cycle = round * 64;
			Publish(cycle);
			while (thread->PublishedPos() < cycle)
			{
				ASSERT_FALSE(aborted.load()) << "the IOP stopped publishing, round " << round;
				thread->Wake();
				SpinHint();
			}
			ASSERT_EQ(written[round], cycle) << "round " << round;
		}
	}

	TEST_F(IopThreadObject, WhatTheIopDidBeforePublishingItsPositionIsVisibleThroughTheLagCheck)
	{
		// The same, through CheckLag(), whose inline fast path does its own load. The EE first learns
		// that the slice has ended from a relaxed load of an atomic the fake sets, which orders
		// nothing, and waits long enough for the IOP to have published; then CheckLag() sees the
		// published position and is the only thing that orders the plain array.
		constexpr u64 kRounds = 1500;
		std::vector<u64> written(kRounds + 2, 0);
		fake.block = 64;
		fake.in_slice_hook = [&](u64, u64 target) { written[target / 64] = target; };
		Params p = BaseParams();
		p.lag = 1; // the check passes only once the IOP is within a cycle of the EE, which is exactly there
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		for (u64 round = 1; round <= kRounds; round++)
		{
			const u64 cycle = round * 64;
			Publish(cycle);
			while (fake.pos_mirror.load(std::memory_order_relaxed) < cycle)
			{
				ASSERT_FALSE(aborted.load()) << "the IOP stopped publishing, round " << round;
				thread->Wake();
				SpinHint();
			}
			std::this_thread::sleep_for(30us);
			ASSERT_TRUE(thread->CheckLag(cycle));
			ASSERT_EQ(written[round], cycle) << "round " << round;
		}
	}

	TEST_F(IopThreadObject, ResyncMovesBothClocksUnderAHold)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(5000);
		ASSERT_TRUE(WaitPos(5000));
		{
			ScopedHold hold(*thread);
			// As a savestate load does: both clocks move, here backwards, and the queues are emptied.
			ee_cycle = 800;
			fake.pos = 700;
			fake.pos_mirror.store(700);
			thread->Resync(800, 700);
			EXPECT_EQ(thread->Watermark(), 800u);
			EXPECT_EQ(thread->PublishedPos(), 700u);
		}
		Publish(3000);
		ASSERT_TRUE(WaitPos(3000));
		thread->Stop();
		ASSERT_FALSE(fake.slices.empty());
		bool found = false;
		for (const auto& s : fake.slices)
			if (s.from == 700)
				found = true;
		EXPECT_TRUE(found) << "the IOP must continue from the position it was resynchronised to";
	}

	TEST_F(IopThreadObject, CallOnEEWhileTheEeIsInALagSpinCompletes)
	{
		bool first = true;
		bool call_result = false;
		std::thread::id fn_thread;
		u32 fn_runs = 0;
		fake.in_slice_hook = [&](u64, u64) {
			if (!first)
				return;
			first = false;
			// Do not call until the EE is really spinning, so the spin is what has to serve it.
			WaitFor([&] { return thread->EeSpinning(); });
			call_result = thread->CallOnEE([&] {
				fn_thread = std::this_thread::get_id();
				fn_runs++;
			});
		};
		Params p = BaseParams();
		p.lag = 1000;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(10000);
		EXPECT_TRUE(thread->CheckLag(10000)) << "the spin never ended: CallOnEE was not served";
		EXPECT_GE(thread->PublishedPos(), 9000u);
		thread->Stop();
		EXPECT_TRUE(call_result);
		EXPECT_EQ(fn_runs, 1u);
		EXPECT_EQ(fn_thread, std::this_thread::get_id()) << "the function must run on the EE thread";
		EXPECT_EQ(thread->GetStats().calls_served, 1u);
		EXPECT_EQ(thread->GetStats().lag_spins, 1u);
	}

	TEST_F(IopThreadObject, CallOnEEIsServedByTheEventTestDrainAndTheIopWaitsForIt)
	{
		bool first = true;
		bool call_result = false;
		u64 shared = 0; // plain: the function writes it on the EE thread, the IOP reads it after the call
		u64 seen_after = 0;
		std::atomic<bool> call_returned{false};
		fake.in_slice_hook = [&](u64, u64) {
			if (!first)
				return;
			first = false;
			call_result = thread->CallOnEE([&] { shared = 42; });
			seen_after = shared;
			call_returned.store(true, std::memory_order_release);
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); }));
		std::this_thread::sleep_for(2ms);
		EXPECT_EQ(fake.slices_done.load(), 0u) << "the IOP must be blocked until the EE serves the call";
		EXPECT_EQ(thread->DrainI2E(), 1u);
		// No further wake-up from the EE: finishing the call has to wake the sleeping IOP itself.
		ASSERT_TRUE(WaitFor([&] { return call_returned.load(std::memory_order_acquire); })) << "the IOP was not woken when the call finished";
		ASSERT_TRUE(WaitPos(100));
		thread->Stop();
		EXPECT_TRUE(call_result);
		EXPECT_EQ(seen_after, 42u) << "what the function wrote must be visible when CallOnEE returns";
	}

	TEST_F(IopThreadObject, ManyCallsEachRunOnceInOrder)
	{
		constexpr int kCalls = 400;
		std::vector<int> ran; // written on the EE thread (the functions) only
		std::atomic<int> returned{0};
		fake.in_slice_hook = [&, started = false](u64, u64) mutable {
			if (started)
				return;
			started = true;
			for (int i = 0; i < kCalls; i++)
				if (thread->CallOnEE([&, i] { ran.push_back(i); }))
					returned.fetch_add(1);
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] {
			if (thread->MayHaveI2E())
				thread->DrainI2E();
			return returned.load() == kCalls;
		}));
		ASSERT_EQ(ran.size(), static_cast<size_t>(kCalls));
		for (int i = 0; i < kCalls; i++)
			EXPECT_EQ(ran[i], i);
	}

	TEST_F(IopThreadObject, ShutdownWhileTheIopIsBlockedInCallOnEECancelsTheCall)
	{
		bool first = true;
		bool call_result = true;
		bool fn_ran = false;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				call_result = thread->CallOnEE([&] { fn_ran = true; });
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); })) << "the IOP never reached CallOnEE";
		ASSERT_TRUE(WaitParked());
		thread->Stop(); // nobody serves the call; Stop() must not wait for it
		EXPECT_FALSE(thread->IsRunning());
		EXPECT_FALSE(call_result) << "a call cancelled by shutdown reports it";
		EXPECT_FALSE(fn_ran) << "a cancelled call must never run";
		EXPECT_EQ(thread->GetStats().calls_cancelled, 1u);
		// The call's entry is still in the I2E queue. An EE that drains after the stop takes it, and
		// it must not run the function that was cancelled.
		EXPECT_EQ(thread->DrainI2E(), 1u);
		EXPECT_FALSE(fn_ran) << "draining the stale entry of a cancelled call ran its function";
	}

	TEST_F(IopThreadObject, ShutdownInsideAHoldServesAPendingCallFirst)
	{
		bool first = true;
		bool call_result = false;
		bool fn_ran = false;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				call_result = thread->CallOnEE([&] { fn_ran = true; });
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); }));
		{
			ScopedHold hold(*thread);
			EXPECT_TRUE(fn_ran) << "the shutdown hold must serve the call before the thread is stopped";
			thread->Stop();
		}
		EXPECT_TRUE(call_result);
	}

	TEST_F(IopThreadObject, TheThreadRestartsCleanlyAfterACancelledCall)
	{
		for (int round = 0; round < 3; round++)
		{
			fake.Reset();
			ee_cycle = 0;
			bool first = true;
			bool call_result = true;
			bool fn_ran = false;
			fake.in_slice_hook = [&](u64, u64) {
				if (first)
				{
					first = false;
					call_result = thread->CallOnEE([&] { fn_ran = true; });
				}
			};
			ASSERT_TRUE(thread->Start(BaseParams()));
			Publish(100);
			ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); }));
			if (round == 2)
			{
				EXPECT_EQ(thread->DrainI2E(), 1u);
				ASSERT_TRUE(WaitPos(100));
				thread->Stop();
				EXPECT_TRUE(call_result);
				EXPECT_TRUE(fn_ran);
			}
			else
			{
				ASSERT_TRUE(WaitParked());
				thread->Stop();
				EXPECT_FALSE(call_result);
				EXPECT_FALSE(fn_ran) << "a stale queue entry from the cancelled call must not run anything after a restart";
			}
		}
	}

	TEST_F(IopThreadObject, AMissedWakeUpIsRecoveredByTheSpinsEntryWake)
	{
		// The narrow race: the EE publishes, reads iop_parked as false just before the IOP sets it,
		// and the IOP's re-check misses the new watermark, so the publish's wake is "lost". The fault
		// is injected by dropping that wake.
		Params p = BaseParams();
		p.lag = 4096;
		p.drop_wake = [](WakeSite site) { return site != WakeSite::SpinEntry; }; // only the wake on entering the spin survives
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		ASSERT_TRUE(WaitAsleep());

		Publish(20000);
		std::this_thread::sleep_for(20ms);
		EXPECT_EQ(fake.slices_started.load(), 0u) << "the publish wake was supposed to be dropped";
		EXPECT_GE(thread->GetStats().wakes_dropped, 1u);

		// The EE is now 20000 ahead of a sleeping IOP. Its lag spin must wake it.
		EXPECT_TRUE(thread->CheckLag(20000));
		EXPECT_GE(thread->PublishedPos(), 20000u - 4096u);
		EXPECT_EQ(thread->GetStats().wakes_sent, 1u) << "the entry wake is the only one that was left";
		EXPECT_EQ(thread->GetStats().lag_spins, 1u) << "recovery takes one spin";
	}

	TEST_F(IopThreadObject, AMissedWakeUpIsRecoveredByThePeriodicWakeWhenTheEntryWakeIsAlsoLost)
	{
		Params p = BaseParams();
		p.lag = 4096;
		p.drop_wake = [](WakeSite site) { return site != WakeSite::SpinPeriodic; };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		ASSERT_TRUE(WaitAsleep());
		Publish(20000);
		EXPECT_TRUE(thread->CheckLag(20000));
		const Stats stats = thread->GetStats();
		EXPECT_GE(stats.lag_spin_iterations, static_cast<u64>(kSpinCheckInterval)) << "only the wake every 1024 iterations was left to recover";
		EXPECT_GE(stats.wakes_sent, 1u);
		EXPECT_EQ(stats.lag_spins, 1u);
	}

	TEST_F(IopThreadObject, AWakeIsOnlyEverSentToAParkedIop)
	{
		Latch gate;
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				gate.Wait(aborted);
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
		ASSERT_FALSE(thread->IsParked());
		const u64 before = thread->GetStats().wakes_sent;
		for (int i = 1; i <= 1000; i++)
			Publish(100 + i * 100);
		thread->Wake();
		EXPECT_EQ(thread->GetStats().wakes_sent, before) << "publishing to a running IOP must not notify it";
		gate.Open();
		ASSERT_TRUE(WaitPos(100100));
	}

	TEST_F(IopThreadObject, UnconditionalWakesAreNeverLost)
	{
		// Releasing a hold and shutting down always notify the IOP, whatever the parked flag says, so
		// dropping every wake that is filtered by that flag must not stop either from working. Only
		// the wake on a spin's entry is let through, to get the IOP into each hold.
		Params p = BaseParams();
		p.drop_wake = [](WakeSite site) { return site != WakeSite::SpinEntry; };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		for (int round = 0; round < 200; round++)
		{
			ASSERT_TRUE(WaitParked()) << "round " << round;
			{
				ScopedHold hold(*thread, HoldKind::Lifecycle);
				// Budget arrives while the IOP is held; its publish wake is dropped. Stay long enough
				// (the semaphore spins for SPIN_TIME_NS first) that the IOP is asleep in its hold.
				Publish(ee_cycle + 100);
				std::this_thread::sleep_for(150us);
			}
			ASSERT_TRUE(WaitPos(ee_cycle)) << "the IOP did not resume after the hold ended, round " << round;
		}
		std::this_thread::sleep_for(2ms); // let it fall asleep
		thread->Stop(); // would hang if the shutdown wake were filtered
		EXPECT_FALSE(thread->IsRunning());
	}

	TEST_F(IopThreadObject, TheLagCheckReturnsAtOnceWhileTheIopIsHeldOrBlockedInACallOnEEFunction)
	{
		// A held IOP, and one that is blocked while a CallOnEE function runs, cannot catch up, so a
		// lag spin in either place would never end.
		bool first = true;
		bool inside_result = false;
		u64 spins_inside = ~u64(0);
		fake.in_slice_hook = [&](u64, u64) {
			if (!first)
				return;
			first = false;
			thread->CallOnEE([&] {
				const u64 before = thread->GetStats().lag_spins;
				inside_result = thread->CheckLag(100000000);
				spins_inside = thread->GetStats().lag_spins - before;
			});
		};
		Params p = BaseParams();
		p.lag = 1000;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(500);
		ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); }));
		EXPECT_EQ(thread->DrainI2E(), 1u);
		EXPECT_TRUE(inside_result);
		EXPECT_EQ(spins_inside, 0u);
		ASSERT_TRUE(WaitPos(500));

		{
			ScopedHold hold(*thread);
			ee_cycle += 100000000; // the EE runs on during the hold; the IOP does not
			EXPECT_TRUE(thread->CheckLag(ee_cycle));
		}
		EXPECT_EQ(thread->GetStats().lag_spins, 0u);
	}

	TEST_F(IopThreadObject, APostOnAFullRingUnderAHoldFailsInsteadOfWaiting)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(100);
		ASSERT_TRUE(WaitPos(100));
		u32 accepted = 0;
		u32 refused = 0;
		{
			ScopedHold hold(*thread);
			// The IOP is held, so nothing will ever make room; the post must say so.
			for (u32 i = 0; i < static_cast<u32>(kE2IRingCapacity) + 10; i++)
			{
				ee_cycle = 1000 + 10 * i;
				if (thread->PostE2I(ee_cycle, 6, i))
					accepted++;
				else
					refused++;
			}
		}
		EXPECT_EQ(accepted, static_cast<u32>(kE2IRingCapacity));
		EXPECT_EQ(refused, 10u);
		EXPECT_EQ(thread->GetStats().e2i_full_failfast, 10u);
		EXPECT_EQ(thread->GetStats().e2i_full_spins, 0u);
		const u64 last_accepted_stamp = 1000 + 10 * (static_cast<u64>(kE2IRingCapacity) - 1);
		ASSERT_TRUE(WaitPos(last_accepted_stamp)) << "the IOP must carry on after the hold and run to what was queued";
		ASSERT_TRUE(WaitEe([&] { return fake.applied_count.load(std::memory_order_acquire) == kE2IRingCapacity; }));
	}

	TEST_F(IopThreadObject, ResyncDropsWhatWasQueuedBeforeIt)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		{
			ScopedHold hold(*thread);
			ASSERT_TRUE(thread->PostE2I(900, 9, 99)); // from the timeline that a state load replaces
			ee_cycle = 800;
			thread->Resync(800, 0);
		}
		Publish(3000);
		ASSERT_TRUE(WaitPos(3000));
		thread->Stop();
		EXPECT_TRUE(fake.applied.empty()) << "an item queued before the resynchronisation must not be applied after it";
	}

	TEST_F(IopThreadObject, AFullE2IRingMakesTheEeSpinUntilTheIopDrainsIt)
	{
		Latch gate;
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				gate.Wait(aborted);
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));

		std::thread opener([&] {
			WaitFor([&] { return thread->GetStats().e2i_full_spins >= 1; });
			gate.Open();
		});
		constexpr u32 kItems = static_cast<u32>(kE2IRingCapacity) + 76;
		for (u32 i = 0; i < kItems; i++)
		{
			ee_cycle = 10 + 10 * i;
			ASSERT_TRUE(thread->PostE2I(ee_cycle, 6, i)) << "item " << i;
		}
		opener.join();
		EXPECT_GE(thread->GetStats().e2i_full_spins, 1u);
		{
			ScopedHold hold(*thread);
			ASSERT_EQ(fake.applied.size(), kItems);
			for (u32 i = 0; i < kItems; i++)
				EXPECT_EQ(fake.applied[i].item.arg, i);
		}
	}

	TEST_F(IopThreadObject, AFullE2IRingWhileTheIopIsBlockedInCallOnEEDoesNotDeadlock)
	{
		// The EE waits for the IOP to make room; the IOP waits for the EE to run a function. The
		// spin that waits for room has to serve the function.
		bool first = true;
		bool call_result = false;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				call_result = thread->CallOnEE([] {});
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1);
		ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); })) << "the IOP never reached CallOnEE";
		constexpr u32 kItems = static_cast<u32>(kE2IRingCapacity) + 300;
		for (u32 i = 0; i < kItems; i++)
		{
			ee_cycle = 10 + 10 * i;
			ASSERT_TRUE(thread->PostE2I(ee_cycle, 6, i)) << "item " << i;
		}
		{
			ScopedHold hold(*thread);
			EXPECT_EQ(fake.applied.size(), kItems);
		}
		thread->Stop();
		EXPECT_TRUE(call_result);
		EXPECT_EQ(thread->GetStats().calls_served, 1u);
		EXPECT_GE(thread->GetStats().e2i_full_spins, 1u);
	}

	TEST_F(IopThreadObject, AFullE2IRingBailsOutOnTheStopPredicateEvery1024Iterations)
	{
		Latch gate;
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				gate.Wait(aborted);
			}
		};
		std::atomic<u32> checks{0};
		Params p = BaseParams();
		p.should_stop = [&] { return checks.fetch_add(1) + 1 >= 3 || aborted.load(); };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(1);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
		u32 i = 0;
		for (; i < static_cast<u32>(kE2IRingCapacity); i++)
		{
			ee_cycle = 10 + 10 * i;
			ASSERT_TRUE(thread->PostE2I(ee_cycle, 6, i));
		}
		ee_cycle += 10;
		EXPECT_FALSE(thread->PostE2I(ee_cycle, 6, i)) << "the ring is full and the IOP is stuck";
		EXPECT_EQ(checks.load(), 3u);
		EXPECT_EQ(thread->GetStats().e2i_full_iterations, 3u * kSpinCheckInterval);
		gate.Open();
	}

	TEST_F(IopThreadObject, AFullE2IRingInsideACallOnEEFunctionFailsInsteadOfWaitingForever)
	{
		// While a function runs for CallOnEE the IOP is blocked and cannot make room, so waiting for
		// room would never end. The post must report failure instead.
		bool first = true;
		bool call_result = false;
		u32 accepted = 0;
		u32 refused = 0;
		fake.in_slice_hook = [&](u64, u64) {
			if (!first)
				return;
			first = false;
			call_result = thread->CallOnEE([&] {
				for (u32 i = 0; i < static_cast<u32>(kE2IRingCapacity) + 50; i++)
				{
					if (thread->PostE2I(10 + 10 * i, 6, i))
						accepted++;
					else
						refused++;
				}
			});
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(1);
		ASSERT_TRUE(WaitEe([&] { return thread->MayHaveI2E(); }));
		EXPECT_EQ(thread->DrainI2E(), 1u); // runs the function on this thread
		EXPECT_EQ(accepted, static_cast<u32>(kE2IRingCapacity));
		EXPECT_EQ(refused, 50u);
		EXPECT_GE(thread->GetStats().e2i_full_failfast, 50u);
		ee_cycle = 10 + 10 * static_cast<u64>(kE2IRingCapacity + 50); // the EE has reached the last stamp it used
		{
			ScopedHold hold(*thread);
			EXPECT_EQ(fake.applied.size(), accepted) << "the items that were accepted are still applied";
		}
		thread->Stop();
		EXPECT_TRUE(call_result);
	}

	TEST_F(IopThreadObject, AFullI2EQueueMakesTheIopWaitAndLosesNothing)
	{
		constexpr u32 kItems = static_cast<u32>(kI2EQueueCapacity) * 2 + 88;
		bool all_posted = false;
		fake.in_slice_hook = [&, once = false](u64, u64) mutable {
			if (once)
				return;
			once = true;
			bool ok = true;
			for (u32 i = 0; i < kItems; i++)
				ok = thread->PostI2E(I2EKind::SbusIrq, i) && ok;
			all_posted = ok;
		};
		std::vector<u32> received;
		Params p = BaseParams();
		p.handle_i2e = [&](const I2EItem& item) {
			if (item.kind == I2EKind::SbusIrq)
				received.push_back(item.arg);
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return thread->GetStats().i2e_full_waits >= 1; })) << "the IOP should fill the queue and wait";
		std::this_thread::sleep_for(5ms); // the EE is not draining: the IOP keeps waiting
		EXPECT_GE(thread->DrainI2E(), static_cast<size_t>(kI2EQueueCapacity)); // the IOP may refill while this drains
		ASSERT_TRUE(WaitEe([&] {
			if (thread->MayHaveI2E())
				thread->DrainI2E();
			return received.size() == kItems;
		}));
		ASSERT_TRUE(WaitPos(100));
		thread->Stop();
		EXPECT_TRUE(all_posted);
		ASSERT_EQ(received.size(), kItems);
		for (u32 i = 0; i < kItems; i++)
			EXPECT_EQ(received[i], i) << "the IOP-to-EE queue must keep its order";
	}

	TEST_F(IopThreadObject, ShutdownWhileTheIopWaitsForI2ESpaceCompletes)
	{
		bool all_posted = true;
		fake.in_slice_hook = [&, once = false](u64, u64) mutable {
			if (once)
				return;
			once = true;
			for (u32 i = 0; i < static_cast<u32>(kI2EQueueCapacity) + 50; i++)
				all_posted = thread->PostI2E(I2EKind::SbusIrq, i) && all_posted;
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return thread->GetStats().i2e_full_waits >= 1; }));
		thread->Stop(); // the queue stays full and nobody drains it
		EXPECT_FALSE(all_posted) << "a post cancelled by shutdown reports it";
	}

	TEST_F(IopThreadObject, AHoldDrainsAnI2EQueueTheIopIsWaitingOn)
	{
		constexpr u32 kItems = static_cast<u32>(kI2EQueueCapacity) + 100;
		fake.in_slice_hook = [&, once = false](u64, u64) mutable {
			if (once)
				return;
			once = true;
			for (u32 i = 0; i < kItems; i++)
				thread->PostI2E(I2EKind::SbusIrq, i);
		};
		u32 received = 0;
		Params p = BaseParams();
		p.handle_i2e = [&](const I2EItem&) { received++; };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return thread->GetStats().i2e_full_waits >= 1; }));
		{
			ScopedHold hold(*thread);
			EXPECT_EQ(received, kItems) << "the hold spin must have emptied the queue the IOP was blocked on";
		}
	}

	TEST_F(IopThreadObject, TheLagSpinChecksTheStopPredicateEvery1024Iterations)
	{
		Latch gate;
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				gate.Wait(aborted);
			}
		};
		std::atomic<u32> checks{0};
		Params p = BaseParams();
		p.lag = 1000;
		p.should_stop = [&] { return checks.fetch_add(1) + 1 >= 3 || aborted.load(); };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(100000);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
		EXPECT_FALSE(thread->CheckLag(100000)) << "the predicate fired, so the lag spin gives up";
		EXPECT_EQ(checks.load(), 3u);
		EXPECT_EQ(thread->GetStats().lag_spin_iterations, 3u * kSpinCheckInterval);
		EXPECT_EQ(thread->GetStats().stop_checks, 3u);
		gate.Open();
	}

	TEST_F(IopThreadObject, TheLagSpinServesTheI2EQueueWhileItSpins)
	{
		constexpr u32 kItems = 200;
		fake.in_slice_hook = [&, once = false](u64, u64) mutable {
			if (once)
				return;
			once = true;
			// A slice that takes a while and talks to the EE as it goes.
			for (u32 i = 0; i < kItems; i++)
			{
				thread->PostI2E(I2EKind::SbusIrq, i);
				std::this_thread::sleep_for(50us);
			}
		};
		bool in_spin = false;
		u32 handled_in_spin = 0;
		u32 handled_outside = 0;
		std::vector<u32> order;
		Params p = BaseParams();
		p.lag = 1000;
		p.handle_i2e = [&](const I2EItem& item) {
			(in_spin ? handled_in_spin : handled_outside)++;
			order.push_back(item.arg);
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(50000);
		in_spin = true;
		EXPECT_TRUE(thread->CheckLag(50000));
		in_spin = false;
		EXPECT_EQ(handled_outside, 0u);
		EXPECT_GE(handled_in_spin, kItems / 2) << "the spin must empty the queue as items arrive, not once at the end";
		thread->DrainI2E();
		ASSERT_EQ(order.size(), kItems);
		for (u32 i = 0; i < kItems; i++)
			EXPECT_EQ(order[i], i);
	}

	TEST_F(IopThreadObjectLockstep, HandoffsRunInOrderOnTheIopThreadAndCompleteBeforeReturning)
	{
		const auto run_script = [&](std::vector<FakeIop::SliceRecord>& slices_out, std::vector<u32>& applied_out) {
			fake.Reset();
			Params p = BaseParams(IopThreadMode::Lockstep);
			StopOnExit stop_on_exit(*thread);
			EXPECT_TRUE(thread->Start(p));
			Rng rng(99);
			u64 target = 0;
			u64 stamp = 0;
			size_t slices_expected = 0;
			size_t items_expected = 0;
			for (u32 i = 0; i < 2000; i++)
			{
				if (rng.Below(3) == 0)
				{
					stamp += 1 + rng.Below(500);
					EXPECT_TRUE(thread->PostE2I(stamp, 7, i));
					EXPECT_EQ(thread->LockstepApplyUpTo(stamp), 1u);
					items_expected++;
				}
				else
				{
					target += 1 + rng.Below(3000);
					const u64 reached = thread->LockstepRunSlice(target);
					EXPECT_GE(reached, target);
					EXPECT_LT(reached - target, fake.block);
					slices_expected++;
				}
				// The call returned, so the IOP's records are complete and visible, with no other sync.
				EXPECT_EQ(fake.slices.size(), slices_expected);
				EXPECT_EQ(fake.applied.size(), items_expected);
			}
			thread->Stop();
			slices_out = fake.slices;
			for (const auto& a : fake.applied)
				applied_out.push_back(a.item.arg);
			EXPECT_NE(fake.iop_tid, std::this_thread::get_id());
			EXPECT_NE(fake.iop_tid, std::thread::id());
		};

		std::vector<FakeIop::SliceRecord> slices_a, slices_b;
		std::vector<u32> applied_a, applied_b;
		run_script(slices_a, applied_a);
		run_script(slices_b, applied_b);
		ASSERT_EQ(slices_a.size(), slices_b.size());
		for (size_t i = 0; i < slices_a.size(); i++)
		{
			EXPECT_EQ(slices_a[i].from, slices_b[i].from);
			EXPECT_EQ(slices_a[i].target, slices_b[i].target);
			EXPECT_EQ(slices_a[i].to, slices_b[i].to);
		}
		EXPECT_EQ(applied_a, applied_b) << "the same script must give the same IOP history";
		EXPECT_TRUE(std::is_sorted(applied_a.begin(), applied_a.end()));
	}

	TEST_F(IopThreadObjectLockstep, ApplyUpToStopsAtTheStampAndAppliesRegardlessOfPosition)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams(IopThreadMode::Lockstep)));
		ASSERT_TRUE(thread->PostE2I(10, 1, 0));
		ASSERT_TRUE(thread->PostE2I(20, 1, 1));
		ASSERT_TRUE(thread->PostE2I(30, 1, 2));
		EXPECT_EQ(thread->LockstepApplyUpTo(20), 2u);
		EXPECT_EQ(fake.applied.size(), 2u);
		EXPECT_EQ(fake.pos, 0u) << "Lockstep applies an item at the EE's point of execution, not at the IOP's position";
		EXPECT_EQ(thread->LockstepApplyUpTo(20), 0u);
		EXPECT_EQ(thread->LockstepApplyUpTo(30), 1u);
		ASSERT_EQ(fake.applied.size(), 3u);
		EXPECT_EQ(fake.applied[2].item.arg, 2u);
	}

	TEST_F(IopThreadObjectLockstep, TheEeAndTheIopNeverRunAtTheSameTime)
	{
		// Both threads read and write one plain variable. A handoff that did not order them would be
		// a data race, which ThreadSanitizer reports, and the sum below would come out wrong.
		u64 shared = 0;
		fake.in_slice_hook = [&](u64, u64) {
			for (int i = 0; i < 200; i++)
				shared += 3;
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams(IopThreadMode::Lockstep)));
		u64 expected = 0;
		for (u64 target = 100; target <= 100 * 3000; target += 100)
		{
			thread->LockstepRunSlice(target);
			expected += 600;
			EXPECT_EQ(shared, expected);
			shared += 1; // the EE's turn
			expected += 1;
		}
		EXPECT_EQ(shared, expected);
	}

	TEST_F(IopThreadObjectLockstep, CallOnEERunsDirectlyOnTheIopThread)
	{
		bool result = false;
		std::thread::id fn_thread;
		std::thread::id slice_thread;
		fake.in_slice_hook = [&](u64, u64) {
			slice_thread = std::this_thread::get_id();
			result = thread->CallOnEE([&] { fn_thread = std::this_thread::get_id(); });
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams(IopThreadMode::Lockstep)));
		thread->LockstepRunSlice(100);
		EXPECT_TRUE(result);
		EXPECT_EQ(fn_thread, slice_thread) << "in Lockstep the IOP thread calls the function itself";
		EXPECT_NE(fn_thread, std::this_thread::get_id());
		EXPECT_EQ(thread->GetStats().calls_served, 0u);
	}

	TEST_F(IopThreadObjectLockstep, HostCommandsRunOnlyWhileTheEeIsInAHandoff)
	{
		std::atomic<bool> ee_in_handoff{false};
		std::atomic<int> ran_in_handoff{0};
		std::atomic<int> ran_outside{0};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams(IopThreadMode::Lockstep)));
		ASSERT_TRUE(WaitParked());
		const u64 ticket = thread->PostHostCommand([&] { (ee_in_handoff.load() ? ran_in_handoff : ran_outside).fetch_add(1); });
		std::this_thread::sleep_for(15ms);
		EXPECT_EQ(ran_in_handoff.load() + ran_outside.load(), 0) << "a host command must not run while the EE is running: it would race it";
		EXPECT_FALSE(thread->IsHostCommandDone(ticket));
		ee_in_handoff.store(true);
		thread->LockstepRunSlice(100);
		ee_in_handoff.store(false);
		EXPECT_EQ(ran_in_handoff.load(), 1);
		EXPECT_EQ(ran_outside.load(), 0);
		EXPECT_TRUE(thread->IsHostCommandDone(ticket));
	}

	TEST_F(IopThreadObjectLockstep, AHoldIsOnlyACounter)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams(IopThreadMode::Lockstep)));
		{
			ScopedHold outer(*thread);
			ScopedHold inner(*thread, HoldKind::Emulation);
			EXPECT_EQ(thread->HoldDepth(), 2u);
			EXPECT_TRUE(thread->IsParked() || WaitParked());
		}
		EXPECT_EQ(thread->HoldDepth(), 0u);
		EXPECT_EQ(thread->GetStats().hold_requests, 0u);
	}

	TEST_F(IopThreadObjectLockstep, APostOnAFullRingFailsInsteadOfWaiting)
	{
		// Nothing consumes the ring between handoffs, so waiting for room would never end.
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams(IopThreadMode::Lockstep)));
		u32 accepted = 0;
		u32 refused = 0;
		for (u32 i = 0; i < static_cast<u32>(kE2IRingCapacity) + 5; i++)
			(thread->PostE2I(10 + 10 * i, 6, i) ? accepted : refused)++;
		EXPECT_EQ(accepted, static_cast<u32>(kE2IRingCapacity));
		EXPECT_EQ(refused, 5u);
		EXPECT_EQ(thread->LockstepApplyUpTo(~u64(0) - 1), static_cast<size_t>(kE2IRingCapacity));
	}

	TEST_F(IopThreadObjectLockstep, StopWhileIdleAndAfterManyHandoffsCompletes)
	{
		for (int round = 0; round < 10; round++)
		{
			fake.Reset();
			StopOnExit stop_on_exit(*thread);
			ASSERT_TRUE(thread->Start(BaseParams(IopThreadMode::Lockstep)));
			for (u64 t = 100; t <= 1000; t += 100)
				thread->LockstepRunSlice(t);
			ASSERT_TRUE(WaitParked());
			thread->Stop();
			EXPECT_FALSE(thread->IsRunning());
		}
	}

	TEST_F(IopThreadObject, EverythingTogetherKeepsItsInvariants)
	{
		// Random EE operations against a fake IOP that stalls, calls the EE, posts to it and takes
		// holds from inside CallOnEE functions, with every invariant checked as it goes.
		constexpr s32 kLag = 6144;
		constexpr int kSteps = 12000;
		std::atomic<u32> calls_returned{0};
		std::atomic<u32> i2e_posted{0};
		u32 call_fn_runs = 0; // EE thread only
		std::vector<u32> i2e_received;

		fake.block = 40;
		fake.in_slice_hook = [&, rng = Rng(17)](u64, u64) mutable {
			const u32 r = rng.Below(400);
			if (r == 0)
				std::this_thread::sleep_for(200us);
			else if (r == 1)
				std::this_thread::sleep_for(2ms);
			else if (r < 6)
			{
				if (thread->CallOnEE([&] {
						call_fn_runs++;
						ScopedHold nested(*thread, HoldKind::Emulation);
					}))
					calls_returned.fetch_add(1);
			}
			else if (r < 12)
			{
				if (thread->PostI2E(I2EKind::SbusIrq, i2e_posted.load()))
					i2e_posted.fetch_add(1);
			}
		};
		Params p = BaseParams();
		p.lag = kLag;
		p.handle_i2e = [&](const I2EItem& item) {
			if (item.kind == I2EKind::SbusIrq)
				i2e_received.push_back(item.arg);
		};
		Rng rng(1234);
		u32 posted = 0;
		u32 host_ran = 0; // IOP thread only until the final hold
		u64 last_ticket = 0;
		u32 host_posted = 0;
		StopOnExit stop_on_exit(*thread); // after every local the IOP thread's callbacks capture
		ASSERT_TRUE(thread->Start(p));

		for (int step = 0; step < kSteps; step++)
		{
			const u32 op = rng.Below(100);
			if (op < 70)
			{
				ee_cycle += 1 + rng.Below(kEeWaitCycles);
				thread->PublishWatermark(ee_cycle);
				if (thread->MayHaveI2E())
					thread->DrainI2E();
				ASSERT_TRUE(thread->CheckLag(ee_cycle)) << "step " << step;
				ASSERT_LE(static_cast<s64>(ee_cycle - thread->PublishedPos()), kLag) << "the lag bound broke at step " << step;
			}
			else if (op < 82)
			{
				ee_cycle += rng.Below(60);
				ASSERT_TRUE(thread->PostE2I(ee_cycle, 3, posted++)) << "step " << step;
			}
			else if (op < 88)
			{
				const u32 depth = 1 + rng.Below(2);
				std::vector<std::unique_ptr<ScopedHold>> holds;
				for (u32 d = 0; d < depth; d++)
				{
					holds.push_back(std::make_unique<ScopedHold>(*thread, rng.Below(2) ? HoldKind::Emulation : HoldKind::Lifecycle));
				}
				ASSERT_GE(thread->PublishedPos(), ee_cycle);
				ASSERT_EQ(fake.applied.size(), posted) << "step " << step;
				ASSERT_EQ(fake.pos, thread->PublishedPos());
				ASSERT_FALSE(fake.in_slice);
				while (!holds.empty())
					holds.pop_back();
			}
			else if (op < 94)
			{
				host_posted++;
				last_ticket = thread->PostHostCommand([&] { host_ran++; });
			}
			else if (op < 97)
			{
				thread->Wake();
			}
			else
			{
				thread->KickIop(E2IKick::Sif0Space);
			}
		}

		{
			ScopedHold hold(*thread);
			ASSERT_EQ(fake.applied.size(), posted);
		}
		// Let the IOP finish its host commands; only the IOP thread touches host_ran until then.
		if (last_ticket)
			ASSERT_TRUE(WaitEe([&] {
				Publish(ee_cycle + 10); // the commands run between slices
				if (thread->MayHaveI2E())
					thread->DrainI2E();
				return thread->IsHostCommandDone(last_ticket);
			}));
		ASSERT_TRUE(WaitEe([&] {
			Publish(ee_cycle + 10);
			if (thread->MayHaveI2E())
				thread->DrainI2E();
			return i2e_received.size() == i2e_posted.load();
		}));
		thread->Stop();
		EXPECT_EQ(host_ran, host_posted);
		EXPECT_EQ(call_fn_runs, calls_returned.load());
		for (size_t i = 0; i < i2e_received.size(); i++)
			ASSERT_EQ(i2e_received[i], i) << "IOP-to-EE items must arrive in order";
		for (size_t i = 0; i < fake.applied.size(); i++)
			ASSERT_EQ(fake.applied[i].item.arg, i);
	}

	TEST_F(IopThreadObject, AWaitThatFindsTheIopParkedIsCountedWithItsStallTime)
	{
		// The accepted lost wake-up made visible: the EE publishes, reads iop_parked as false, and the IOP sleeps
		// with budget pending. The next wait the EE has finds it parked, and says so.
		Params p = BaseParams();
		p.lag = 4096;
		p.drop_wake = [](WakeSite site) { return site != WakeSite::SpinEntry; };
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		ASSERT_TRUE(WaitAsleep());

		Publish(20000);
		std::this_thread::sleep_for(20ms);
		EXPECT_TRUE(thread->CheckLag(20000));
		const Stats stats = thread->GetStats();
		EXPECT_EQ(stats.lag_spins, 1u);
		EXPECT_EQ(stats.lag_spins_parked, 1u);
		EXPECT_GT(stats.lag_parked_stall_ns, 0u);
		EXPECT_EQ(stats.lag_stall_ns, stats.lag_parked_stall_ns) << "the only wait there was found the IOP parked";
	}

	TEST_F(IopThreadObject, AWaitThatFindsTheIopRunningIsNotCountedAsParked)
	{
		Latch gate;
		fake.in_slice_hook = [&](u64, u64) { gate.Wait(aborted); };
		Params p = BaseParams();
		p.lag = 4096;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
		ASSERT_FALSE(thread->IsParked());

		std::thread releaser([&] {
			std::this_thread::sleep_for(30ms);
			gate.Open();
		});
		ee_cycle = 20000;
		thread->PublishWatermark(20000);
		EXPECT_TRUE(thread->CheckLag(20000));
		releaser.join();

		const Stats stats = thread->GetStats();
		EXPECT_EQ(stats.lag_spins, 1u);
		EXPECT_EQ(stats.lag_spins_parked, 0u);
		EXPECT_EQ(stats.lag_parked_stall_ns, 0u);
		EXPECT_GE(stats.lag_stall_ns, 20'000'000u) << "the wait lasted about 30 ms";
	}

	TEST_F(IopThreadObject, AnEmulationHoldIsCountedWithItsStallTimeAndALifecycleHoldIsNot)
	{
		Latch gate;
		bool first = true;
		fake.in_slice_hook = [&](u64, u64) {
			if (first)
			{
				first = false;
				gate.Wait(aborted);
			}
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		Publish(100);
		ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));

		std::thread releaser([&] {
			std::this_thread::sleep_for(30ms);
			gate.Open();
		});
		{
			ScopedHold hold(*thread, HoldKind::Emulation);
		}
		releaser.join();
		Stats stats = thread->GetStats();
		EXPECT_EQ(stats.hold_requests, 1u);
		EXPECT_EQ(stats.emu_hold_requests, 1u);
		EXPECT_GE(stats.emu_hold_stall_ns, 20'000'000u);

		{
			ScopedHold hold(*thread, HoldKind::Lifecycle);
			ScopedHold nested(*thread, HoldKind::Emulation); // a marker: nothing to wait for
		}
		stats = thread->GetStats();
		EXPECT_EQ(stats.hold_requests, 2u);
		EXPECT_EQ(stats.emu_hold_requests, 1u) << "only a hold that went through the handshake as Emulation is counted";
	}

	TEST_F(IopThreadObject, TheHoldHookRunsOnTheEeThreadWhenTheIopIsHeldAndForTheOutermostHoldOnly)
	{
		struct Seen
		{
			std::thread::id thread;
			u64 pos_published;
			u64 ee_cycle_at_call;
			u32 calls = 0;
		} seen;
		Params p = BaseParams();
		p.on_hold_acquired = [&] {
			seen.calls++;
			seen.thread = std::this_thread::get_id();
			seen.pos_published = thread->PublishedPos();
			seen.ee_cycle_at_call = ee_cycle;
		};
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Publish(1000);
		ASSERT_TRUE(WaitPos(1000));

		ee_cycle = 5000; // the EE has run on without an event test
		{
			ScopedHold outer(*thread);
			EXPECT_EQ(seen.calls, 1u);
			EXPECT_EQ(seen.thread, std::this_thread::get_id());
			EXPECT_GE(seen.pos_published, 5000u) << "the IOP had run up to the EE's cycle when the hook ran";
			{
				ScopedHold inner(*thread);
				EXPECT_EQ(seen.calls, 1u) << "a nested hold is a counter";
			}
		}
		{
			ScopedHold again(*thread);
			EXPECT_EQ(seen.calls, 2u);
		}
	}

	TEST_F(IopThreadObject, TheLagBoundTheEeWaitsAtIsTheEesToChange)
	{
		// The first slice the IOP runs waits for the gate, which a helper opens once the EE is spinning: whichever of
		// the IOP's wake-up and the EE's check comes first, the IOP has not moved when the EE looks.
		Latch gate;
		fake.in_slice_hook = [&](u64, u64) { gate.Wait(aborted); };
		Params p = BaseParams();
		p.lag = 4096;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		EXPECT_EQ(thread->Lag(), 4096);
		ASSERT_TRUE(WaitAsleep());

		// 15000 ahead of an IOP that has run nothing: beyond 4096, so a wait. With the bound at 40000, none.
		thread->SetLag(40000);
		EXPECT_EQ(thread->Lag(), 40000);
		ee_cycle = 15000;
		thread->PublishWatermark(15000);
		EXPECT_TRUE(thread->CheckLag(15000));
		EXPECT_EQ(thread->GetStats().lag_spins, 0u);

		std::thread opener([&] {
			while (thread->GetStats().lag_spins < 1 && !aborted.load())
				std::this_thread::sleep_for(100us);
			gate.Open();
		});
		thread->SetLag(4096);
		EXPECT_TRUE(thread->CheckLag(15000));
		EXPECT_EQ(thread->GetStats().lag_spins, 1u);
		opener.join();
	}

	TEST_F(IopThreadObject, TheIopIsStoppedWhenThereIsNoThreadAHoldOrACallOnEeFunction)
	{
		EXPECT_TRUE(thread->IopIsStopped()) << "no thread";
		StopOnExit stop_on_exit(*thread);
		Params p = BaseParams();
		bool posted = false;
		Latch served;
		p.handle_i2e = [](const I2EItem&) {};
		ASSERT_TRUE(thread->Start(p));
		EXPECT_FALSE(thread->IopIsStopped());
		Publish(500);
		ASSERT_TRUE(WaitPos(500));
		{
			ScopedHold hold(*thread);
			EXPECT_TRUE(thread->IopIsStopped());
		}
		EXPECT_FALSE(thread->IopIsStopped());

		// Inside a function the IOP asked the EE to run, the IOP is blocked at a known point.
		bool stopped_inside = false, in_call_inside = false;
		fake.in_slice_hook = [&](u64, u64) {
			if (!posted)
			{
				posted = true;
				thread->CallOnEE([&] {
					stopped_inside = thread->IopIsStopped();
					in_call_inside = thread->InCallOnEE();
				});
				served.Open();
			}
		};
		Publish(5000);
		ASSERT_TRUE(WaitEe([&] {
			if (thread->MayHaveI2E())
				thread->DrainI2E();
			return served.IsOpen();
		}));
		EXPECT_TRUE(stopped_inside);
		EXPECT_TRUE(in_call_inside);
		EXPECT_FALSE(thread->IopIsStopped());
		EXPECT_FALSE(thread->InCallOnEE());
	}

	TEST_F(IopThreadObject, EitherSpinHintLetsTheWaitsComplete)
	{
		const SpinHintKind saved = g_spin_hint.load();
		for (const SpinHintKind kind : {SpinHintKind::Yield, SpinHintKind::Isb})
		{
			g_spin_hint.store(kind);
			Latch gate;
			bool first = true;
			fake.in_slice_hook = [&](u64, u64) {
				if (first)
				{
					first = false;
					gate.Wait(aborted);
				}
			};
			Params p = BaseParams();
			p.lag = 4096;
			ASSERT_TRUE(thread->Start(p));
			Publish(100);
			ASSERT_TRUE(WaitEe([&] { return fake.slices_started.load() == 1; }));
			std::thread releaser([&] {
				std::this_thread::sleep_for(10ms);
				gate.Open();
			});
			ee_cycle = 20000;
			thread->PublishWatermark(20000);
			EXPECT_TRUE(thread->CheckLag(20000)) << (kind == SpinHintKind::Isb ? "isb" : "yield");
			releaser.join();
			EXPECT_EQ(thread->GetStats().lag_spins, 1u);
			thread->Stop();
			fake.Reset();
		}
		g_spin_hint.store(saved);
	}

	TEST_F(IopThreadObject, ASifPumpTheEeStartedIsCounted)
	{
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(BaseParams()));
		thread->CountSifPump();
		thread->CountSifPump();
		EXPECT_EQ(thread->GetStats().sif_ee_pumps, 2u);
	}

	TEST_F(IopThreadObject, AnIdleIopSeesEachPublishWithoutAWakeUp)
	{
		// The idle IOP watches the watermark itself for idle_spin_ns before it sleeps, and says it is
		// sleeping only then. While it watches, a publish is a plain store: the EE sends no wake-up, so
		// there is no read-modify-write on the semaphore word on every event test.
		Params p = BaseParams();
		p.idle_spin_ns = 2'000'000'000; // longer than the test: it never sleeps
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		ASSERT_TRUE(WaitFor([&] { return thread->IsParked(); }));
		for (u64 i = 1; i <= 2000; i++)
		{
			Publish(i * 100);
			// WaitFor, not WaitEe: nothing here may wake the IOP.
			ASSERT_TRUE(WaitFor([&] { return thread->PublishedPos() >= i * 100; }, 2s)) << "publish " << i;
		}
		const Stats stats = thread->GetStats();
		EXPECT_EQ(stats.wakes_sent, 0u) << "a publish to an IOP that watches the watermark must not notify it";
		EXPECT_EQ(stats.parks, 0u) << "the IOP never stopped watching";
		EXPECT_FALSE(thread->IsSleeping());
		EXPECT_GE(stats.idle_waits, 1u);
	}

	TEST_F(IopThreadObject, AfterItsIdleSpinTheIopSleepsAndAPublishWakesIt)
	{
		Params p = BaseParams();
		p.idle_spin_ns = 200'000; // 200 us
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		// `sleeping` is announced before the barrier and the re-check, and the sleep is counted after them.
		ASSERT_TRUE(WaitFor([&] { return thread->GetStats().parks >= 1 && thread->IsSleeping(); }));
		EXPECT_TRUE(thread->IsParked()) << "a sleeping IOP is also idle";
		Publish(500);
		ASSERT_TRUE(WaitFor([&] { return thread->PublishedPos() >= 500; }, 2s)) << "the publish's own wake-up must reach a sleeping IOP";
		EXPECT_GE(thread->GetStats().wakes_sent, 1u);
	}

	// The handshake that keeps an EE publish and an IOP going to sleep from missing each other (IopThread.h,
	// "Going to sleep"). Every round the EE publishes once and then waits WITHOUT waking anybody, so a lost
	// wake-up leaves the IOP asleep with budget and the round times out. idle_spin_ns = 0 sends the IOP
	// straight from "no work" to the sleep handshake, and the EE publishes after a short random delay, so the
	// EE's store and the IOP's announcement land close together in most rounds. Run with each barrier: the
	// default (membarrier where the kernel has it) and the plain fence on both sides.
	//
	// The run is sized by what it has covered, not by a round count. The test only means something once
	// the IOP has gone to sleep many times, and under load that happens less often: the IOP's membarrier
	// call takes longer, more publishes arrive while it is inside, and the IOP then finds work instead of
	// sleeping. So the run goes on past kHammerMinRounds until the IOP has slept kHammerTargetSleeps times,
	// up to a wall-clock budget under the fixture's watchdog. Losing a wake-up fails the test at any load.
	// Reaching the budget with fewer than kHammerFloorSleeps sleeps fails it too, since a run that covered
	// almost nothing proves nothing; between the floor and the target it passes and says so.
	constexpr u32 kHammerMinRounds = 200000;
	constexpr u64 kHammerTargetSleeps = kHammerMinRounds / 20;
	constexpr u64 kHammerFloorSleeps = kHammerTargetSleeps / 10;
	constexpr auto kHammerBudget = 20s;
	static_assert(std::chrono::duration_cast<std::chrono::milliseconds>(kHammerBudget).count() < IOP_THREAD_TEST_LIMIT_MS,
		"the hammer's time budget has to end before the fixture's watchdog does");
	struct HammerCase
	{
		SleepBarrier barrier;
		bool through_post; // publish through PostE2I (an item at the new watermark) rather than PublishWatermark
		const char* name;
	};

	class IopThreadSleepHandshake : public IopThreadObject, public ::testing::WithParamInterface<HammerCase>
	{
	};

	TEST_P(IopThreadSleepHandshake, NoPublishIsSleptThrough)
	{
		const HammerCase hc = GetParam();
		Params p = BaseParams();
		p.idle_spin_ns = 0;
		p.sleep_barrier = hc.barrier;
		fake.block = 1;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		Rng rng(0x5eed + static_cast<u64>(hc.barrier) * 2 + (hc.through_post ? 1 : 0));

		u64 cycle = 0;
		u32 lost = 0;
		u64 rounds = 0;
		bool budget_spent = false;
		const auto budget_end = Clock::now() + kHammerBudget;
		for (u64 round = 0; !aborted.load(); round++)
		{
			rounds = round;
			if ((round & 4095) == 0)
			{
				if (round >= kHammerMinRounds && thread->GetStats().parks >= kHammerTargetSleeps)
					break;
				if (Clock::now() > budget_end)
				{
					budget_spent = true;
					break;
				}
			}
			// A random delay of 0 to a few microseconds, so that the publish falls on every phase of the IOP's
			// way to sleep: still deciding, watching (none with idle_spin_ns = 0), announcing, in the barrier,
			// re-checking, in the kernel.
			const u32 spins = rng.Below(4) == 0 ? 0 : rng.Below(64) * rng.Below(64);
			for (u32 s = 0; s < spins; s++)
				SpinHint();
			cycle += 1 + rng.Below(200);
			ee_cycle = cycle;
			if (hc.through_post)
				ASSERT_TRUE(thread->PostE2I(cycle, 0, round));
			else
				thread->PublishWatermark(cycle);
			// Spin, do not sleep, while waiting: the next round's publish has to follow the IOP's last slice
			// by no more than the random delay, so that it lands while the IOP is on its way to sleep.
			bool reached = false;
			const auto end = Clock::now() + 1s;
			for (u32 n = 0;; n++)
			{
				if (thread->PublishedPos() >= cycle)
				{
					reached = true;
					break;
				}
				if ((n & 1023) == 1023 && (Clock::now() > end || aborted.load(std::memory_order_relaxed)))
					break;
				SpinHint();
			}
			if (!reached)
			{
				if (aborted.load(std::memory_order_relaxed))
					break; // the watchdog fired: the fixture reports that, and it is not a lost wake-up
				lost++;
				ADD_FAILURE() << hc.name << ": round " << round << ": the IOP slept through the publish of " << cycle
							  << " (position " << thread->PublishedPos() << ", sleeping " << thread->IsSleeping() << ")";
				if (lost >= 3)
					break;
				thread->Wake(); // recover, to see how often it happens
			}
		}
		const Stats stats = thread->GetStats();
		EXPECT_EQ(lost, 0u);
		// The test only means something if the IOP went to sleep many times and many publishes arrived while
		// it was on its way there. A publish that arrives before the IOP has announced `sleeping` is found
		// without a wake-up (by its loop or by the re-check), so rounds minus wakes counts those.
		if (lost == 0 && !aborted.load())
		{
			EXPECT_GE(stats.parks, budget_spent ? kHammerFloorSleeps : kHammerTargetSleeps)
				<< hc.name << ": too few sleeps to have tested the handshake (" << rounds << " rounds, budget spent " << budget_spent << ")";
		}
		std::printf("[  INFO  ] %s: %llu rounds, %llu sleeps, %llu wakes sent, %llu publishes found without a wake-up%s\n", hc.name,
			static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(stats.parks),
			static_cast<unsigned long long>(stats.wakes_sent),
			static_cast<unsigned long long>(rounds > stats.wakes_sent ? rounds - stats.wakes_sent : 0),
			budget_spent && stats.parks < kHammerTargetSleeps ? " (time budget spent before the sleep target)" : "");
	}

	INSTANTIATE_TEST_SUITE_P(Barriers, IopThreadSleepHandshake,
		::testing::Values(HammerCase{SleepBarrier::Auto, false, "auto-publish"}, HammerCase{SleepBarrier::Auto, true, "auto-post"},
			HammerCase{SleepBarrier::Fence, false, "fence-publish"}, HammerCase{SleepBarrier::Fence, true, "fence-post"}),
		[](const ::testing::TestParamInfo<HammerCase>& info) {
			std::string name = info.param.name;
			std::replace(name.begin(), name.end(), '-', '_');
			return name;
		});

	TEST_F(IopThreadObject, AWaitThatFindsTheIopIdleButAwakeIsCountedAsIdleNotParked)
	{
		// A minimum slice above the EE's lead keeps the IOP idle (watching, awake) after the publish, until the
		// EE's spin tells it to run, so the spin finds it idle every time.
		Params p = BaseParams();
		p.lag = 4096;
		p.min_slice = 1 << 20;
		p.idle_spin_ns = 2'000'000'000;
		StopOnExit stop_on_exit(*thread);
		ASSERT_TRUE(thread->Start(p));
		ASSERT_TRUE(WaitFor([&] { return thread->IsParked(); }));
		Publish(20000);
		EXPECT_TRUE(thread->CheckLag(20000));
		const Stats stats = thread->GetStats();
		EXPECT_EQ(stats.lag_spins, 1u);
		EXPECT_EQ(stats.lag_spins_parked, 0u) << "the IOP was awake, watching the watermark";
		EXPECT_EQ(stats.lag_spins_idle, 1u);
	}
} // namespace
