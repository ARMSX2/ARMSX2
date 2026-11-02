// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AudioStream: the buffer between the thread that produces SPU2 output and the audio backend's callback.
//
// With neither time stretching nor surround expansion a chunk is copied straight into the ring buffer. With either,
// the chunk goes through the stretcher or the expander first. The tests drive a stream with no audio device (a
// subclass that only calls BaseInitialize), so the producer side is the test thread and the consumer side is
// PullFrames(), which is what the libretro frontend uses and is ReadFrames() underneath.
//
// The tests that feed a stream and then drain it without a concurrent consumer are deterministic: the stretcher's
// tempo is a function of the ring's fullness, and the ring only changes when the producer writes.

#include "Host/AudioStream.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <time.h>
#define AUDIO_STREAM_TEST_HAS_CPU_CLOCKS 1
#endif

namespace
{
	constexpr u32 kRate = 48000;

	// An AudioStream with no device: whoever calls PullFrames() is the backend's callback.
	class TestStream final : public AudioStream
	{
	public:
		TestStream(const AudioStreamParameters& params)
			: AudioStream(kRate, params)
		{
		}

		// threaded = false is the reference: the same expansion and stretching, run on the calling thread.
		void Init(bool stretch, bool threaded = true)
		{
			static constexpr const std::array<SampleReader, static_cast<size_t>(AudioExpansionMode::Count)> readers = {{
				&StereoSampleReaderImpl,
				&SampleReaderImpl<AudioExpansionMode::StereoLFE, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
					READ_CHANNEL_LFE>,
				&SampleReaderImpl<AudioExpansionMode::Quadraphonic, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
					READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
				&SampleReaderImpl<AudioExpansionMode::QuadraphonicLFE, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
					READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
				&SampleReaderImpl<AudioExpansionMode::Surround51, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
					READ_CHANNEL_FRONT_CENTER, READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
				&SampleReaderImpl<AudioExpansionMode::Surround71, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
					READ_CHANNEL_FRONT_CENTER, READ_CHANNEL_LFE, READ_CHANNEL_SIDE_LEFT, READ_CHANNEL_SIDE_RIGHT,
					READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
			}};
			BaseInitialize(readers[static_cast<size_t>(m_parameters.expansion_mode)], stretch, threaded);
		}

		u32 OutputChannels() const { return m_output_channels; }

		// Everything the stream can hand out right now, in order.
		std::vector<float> Drain()
		{
			std::vector<float> out;
			std::vector<float> block(2048 * MAX_OUTPUT_CHANNELS);
			for (;;)
			{
				const u32 got = PullFrames(block.data(), 2048);
				if (got == 0)
					break;
				out.insert(out.end(), block.begin(), block.begin() + static_cast<size_t>(got) * m_output_channels);
			}
			return out;
		}
	};

	// Music-like and not periodic over the test's length: a few sines plus noise, so the stretcher's overlap search
	// has something to find.
	void FillChunk(float* dst, u64 chunk_index)
	{
		u64 rng = 0x9E3779B97F4A7C15ull ^ (chunk_index * 0xD1B54A32D192ED03ull);
		for (u32 i = 0; i < AudioStream::CHUNK_SIZE; i++)
		{
			const double t = static_cast<double>(chunk_index * AudioStream::CHUNK_SIZE + i) / kRate;
			rng ^= rng << 13;
			rng ^= rng >> 7;
			rng ^= rng << 17;
			const float noise = (static_cast<float>(rng >> 40) / static_cast<float>(1 << 24) - 0.5f) * 0.1f;
			dst[i * 2 + 0] = static_cast<float>(0.25 * std::sin(2 * M_PI * 220.0 * t) + 0.15 * std::sin(2 * M_PI * 1330.0 * t)) + noise;
			dst[i * 2 + 1] = static_cast<float>(0.25 * std::sin(2 * M_PI * 277.0 * t) + 0.15 * std::sin(2 * M_PI * 910.0 * t)) - noise;
		}
	}

	AudioStreamParameters DefaultParams()
	{
		AudioStreamParameters p;
		p.expansion_mode = AudioExpansionMode::Disabled;
		p.buffer_ms = 50;
		return p;
	}

	// Waits until the stretch worker has processed everything written so far.
	bool WaitUntilProcessed(const AudioStream& s)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (s.GetPendingInputChunks() != 0)
		{
			if (std::chrono::steady_clock::now() > deadline)
				return false;
			std::this_thread::yield();
		}
		return true;
	}

	// Feeds the same chunks to a stream run inline and to one run by the worker, with no consumer reading in
	// between, and returns both outputs. Without a reader the stretcher's inputs (the chunks and the ring's
	// fullness) do not depend on timing, so the two must agree to the bit.
	struct Pair
	{
		std::vector<float> inline_out;
		std::vector<float> threaded_out;
	};

	Pair FeedBoth(const AudioStreamParameters& params, bool stretch, u64 chunks)
	{
		TestStream inline_stream(params);
		TestStream threaded_stream(params);
		inline_stream.Init(stretch, false);
		threaded_stream.Init(stretch, true);

		std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
		for (u64 i = 0; i < chunks; i++)
		{
			FillChunk(chunk.data(), i);
			inline_stream.WriteChunk(chunk.data());
			threaded_stream.WriteChunk(chunk.data());
		}
		EXPECT_TRUE(WaitUntilProcessed(threaded_stream));
		return {inline_stream.Drain(), threaded_stream.Drain()};
	}
} // namespace

TEST(AudioStream, WithoutStretchAChunkIsCopiedThroughUnchanged)
{
	TestStream s(DefaultParams());
	s.Init(false);

	// More than the ring holds in total, drained as we go, so the write position wraps.
	std::vector<float> expected;
	std::vector<float> got;
	std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
	for (u64 i = 0; i < 300; i++)
	{
		FillChunk(chunk.data(), i);
		expected.insert(expected.end(), chunk.begin(), chunk.end());
		s.WriteChunk(chunk.data());
		if (i % 5 == 4)
		{
			const std::vector<float> d = s.Drain();
			got.insert(got.end(), d.begin(), d.end());
		}
	}
	const std::vector<float> d = s.Drain();
	got.insert(got.end(), d.begin(), d.end());

	ASSERT_EQ(got.size(), expected.size());
	EXPECT_EQ(0, std::memcmp(got.data(), expected.data(), got.size() * sizeof(float)));
}

TEST(AudioStream, WithoutStretchAFullRingDropsTheNewChunksAndKeepsTheOldOnes)
{
	TestStream s(DefaultParams());
	s.Init(false);

	std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
	std::vector<float> first;
	for (u64 i = 0; i < 400; i++)
	{
		FillChunk(chunk.data(), i);
		if (i == 0)
			first = chunk;
		s.WriteChunk(chunk.data());
	}
	// 50 ms at 48 kHz, aligned up to a chunk: a ring of 2432 frames that refuses a chunk it could not fit with room to spare.
	const u32 buffered = s.GetBufferedFramesRelaxed();
	EXPECT_LE(buffered, 2432u);
	EXPECT_GT(buffered, 2000u);

	const std::vector<float> got = s.Drain();
	ASSERT_GE(got.size(), first.size());
	EXPECT_EQ(0, std::memcmp(got.data(), first.data(), first.size() * sizeof(float)));
}

TEST(AudioStream, StretchedOutputTracksTheInputWhenTheConsumerKeepsUp)
{
	// Inline, so that each chunk has been stretched by the time the consumer looks at the ring.
	TestStream s(DefaultParams());
	s.Init(true, false);
	ASSERT_TRUE(s.IsStretchEnabled());

	// A consumer that leaves the target amount buffered and then takes exactly what the producer gives it. The
	// stretcher should settle at a tempo near 1, so what comes out is about as long as what went in, and it is
	// finite and within range.
	std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
	std::vector<float> block(AudioStream::CHUNK_SIZE * 2);
	u64 in_frames = 0;
	u64 out_frames = 0;
	bool finite = true;
	auto check = [&](u32 got) {
		out_frames += got;
		for (u32 k = 0; k < got * 2; k++)
			finite = finite && std::isfinite(block[k]) && std::fabs(block[k]) < 2.0f;
	};
	for (u64 i = 0; i < 1500; i++)
	{
		FillChunk(chunk.data(), i);
		s.WriteChunk(chunk.data());
		in_frames += AudioStream::CHUNK_SIZE;
		if (i >= 60)
			check(s.PullFrames(block.data(), AudioStream::CHUNK_SIZE));
	}
	// What is still buffered counts as delivered.
	out_frames += s.GetBufferedFramesRelaxed();
	EXPECT_TRUE(finite);
	EXPECT_GT(out_frames, in_frames * 9 / 10);
	EXPECT_LT(out_frames, in_frames * 11 / 10);
}

TEST(AudioStream, TheWorkerStretchesToTheSameBitsAsTheInlinePath)
{
	// 400 chunks is 0.5 s of input and fits the ring; the stretcher is active the whole time because the ring
	// starts empty.
	const Pair p = FeedBoth(DefaultParams(), true, 400);
	ASSERT_FALSE(p.inline_out.empty());
	ASSERT_EQ(p.inline_out.size(), p.threaded_out.size());
	EXPECT_EQ(0, std::memcmp(p.inline_out.data(), p.threaded_out.data(), p.inline_out.size() * sizeof(float)));
}

TEST(AudioStream, AQueueFullOfChunksMakesTheProducerWaitInsteadOfDroppingThem)
{
	// Far more chunks than the worker's queue holds, written as fast as the producer can, so the producer has to wait
	// for room many times. Nothing may be lost or reordered: the result matches the inline path.
	const Pair p = FeedBoth(DefaultParams(), true, 6000);
	ASSERT_FALSE(p.inline_out.empty());
	ASSERT_EQ(p.inline_out.size(), p.threaded_out.size());
	EXPECT_EQ(0, std::memcmp(p.inline_out.data(), p.threaded_out.data(), p.inline_out.size() * sizeof(float)));
}

TEST(AudioStream, TheWorkerExpandsToTheSameBitsAsTheInlinePath)
{
	for (const bool stretch : {false, true})
	{
		AudioStreamParameters params = DefaultParams();
		params.expansion_mode = AudioExpansionMode::Quadraphonic;
		params.expand_block_size = 512;
		const Pair p = FeedBoth(params, stretch, 800);
		ASSERT_FALSE(p.inline_out.empty()) << "stretch " << stretch;
		ASSERT_EQ(p.inline_out.size(), p.threaded_out.size()) << "stretch " << stretch;
		EXPECT_EQ(0, std::memcmp(p.inline_out.data(), p.threaded_out.data(), p.inline_out.size() * sizeof(float)))
			<< "stretch " << stretch;
	}
}

TEST(AudioStream, EmptyBufferThrowsAwayQueuedInputAndTheStreamKeepsWorking)
{
	TestStream s(DefaultParams());
	s.Init(true);

	std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
	for (u64 i = 0; i < 100; i++)
	{
		FillChunk(chunk.data(), i);
		s.WriteChunk(chunk.data());
	}
	s.EmptyBuffer();
	// Nothing queued before the call survives it: whatever the worker had not got to is gone and the ring is empty.
	EXPECT_EQ(s.GetPendingInputChunks(), 0u);
	EXPECT_EQ(s.GetBufferedFramesRelaxed(), 0u);

	// Chunks written afterwards flow as before.
	for (u64 i = 100; i < 300; i++)
	{
		FillChunk(chunk.data(), i);
		s.WriteChunk(chunk.data());
	}
	ASSERT_TRUE(WaitUntilProcessed(s));
	EXPECT_GT(s.GetBufferedFramesRelaxed(), 0u);
	const std::vector<float> out = s.Drain();
	EXPECT_FALSE(out.empty());
	for (const float v : out)
		ASSERT_TRUE(std::isfinite(v));
}

TEST(AudioStream, TurningStretchOffAndOnMovesTheWriterBetweenTheWorkerAndTheCaller)
{
	TestStream s(DefaultParams());
	s.Init(true);
	std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);

	for (u64 i = 0; i < 50; i++)
	{
		FillChunk(chunk.data(), i);
		s.WriteChunk(chunk.data());
	}
	ASSERT_TRUE(WaitUntilProcessed(s));
	EXPECT_GT(s.GetBufferedFramesRelaxed(), 0u);

	// Off: the ring is rebuilt empty and chunks are copied straight in, bit for bit.
	s.SetStretchEnabled(false);
	EXPECT_FALSE(s.IsStretchEnabled());
	EXPECT_EQ(s.GetBufferedFramesRelaxed(), 0u);
	FillChunk(chunk.data(), 1000);
	s.WriteChunk(chunk.data());
	EXPECT_EQ(s.GetPendingInputChunks(), 0u);
	std::vector<float> got = s.Drain();
	ASSERT_EQ(got.size(), chunk.size());
	EXPECT_EQ(0, std::memcmp(got.data(), chunk.data(), chunk.size() * sizeof(float)));

	// On again: a new worker.
	s.SetStretchEnabled(true);
	EXPECT_TRUE(s.IsStretchEnabled());
	for (u64 i = 0; i < 50; i++)
	{
		FillChunk(chunk.data(), 2000 + i);
		s.WriteChunk(chunk.data());
	}
	ASSERT_TRUE(WaitUntilProcessed(s));
	EXPECT_GT(s.GetBufferedFramesRelaxed(), 0u);
}

TEST(AudioStream, AfterALongPauseTheIdleWorkerWakesForTheNextChunks)
{
	// The worker polls on a timer while chunks are flowing and, after a while with none, sleeps with no timer until the
	// producer wakes it. Chunks written after such a pause must still get processed, and must give the same output as the
	// inline path given the same input.
	TestStream threaded(DefaultParams());
	TestStream inline_stream(DefaultParams());
	threaded.Init(true, true);
	inline_stream.Init(true, false);

	std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
	u64 index = 0;
	for (int round = 0; round < 4; round++)
	{
		for (int i = 0; i < 12; i++, index++)
		{
			FillChunk(chunk.data(), index);
			threaded.WriteChunk(chunk.data());
			inline_stream.WriteChunk(chunk.data());
		}
		ASSERT_TRUE(WaitUntilProcessed(threaded)) << "round " << round;
		// Long enough for the worker to find nothing on every poll it makes and go to sleep.
		std::this_thread::sleep_for(std::chrono::milliseconds(120));
	}

	const std::vector<float> a = inline_stream.Drain();
	const std::vector<float> b = threaded.Drain();
	ASSERT_EQ(a.size(), b.size());
	EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(float)));
}

TEST(AudioStream, ABurstLargerThanTheHighWaterMarkIsProcessedWithoutWaitingForTheNextPoll)
{
	// 40 chunks at once is far over the mark at which the producer nudges the worker. Whatever the timing, all of them must
	// come out, in order.
	const Pair p = FeedBoth(DefaultParams(), true, 40);
	ASSERT_FALSE(p.inline_out.empty());
	ASSERT_EQ(p.inline_out.size(), p.threaded_out.size());
	EXPECT_EQ(0, std::memcmp(p.inline_out.data(), p.threaded_out.data(), p.inline_out.size() * sizeof(float)));
}

TEST(AudioStream, DestroyingAStreamWithInputStillQueuedStopsTheWorker)
{
	std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
	for (int round = 0; round < 20; round++)
	{
		TestStream s(DefaultParams());
		s.Init(true);
		for (u64 i = 0; i < 120; i++)
		{
			FillChunk(chunk.data(), i);
			s.WriteChunk(chunk.data());
		}
		// Destroyed here with the worker mid-queue; the test passes by returning.
	}
	SUCCEED();
}

TEST(AudioStream, AConsumerThreadReadingWhileTheWorkerWritesGetsFiniteAudio)
{
	TestStream s(DefaultParams());
	s.Init(true);

	std::atomic<bool> stop{false};
	std::atomic<bool> bad{false};
	std::atomic<u64> got_frames{0};
	std::thread consumer([&]() {
		std::vector<float> block(480 * 2);
		while (!stop.load(std::memory_order_acquire))
		{
			const u32 got = s.PullFrames(block.data(), 480);
			for (u32 k = 0; k < got * 2; k++)
				if (!std::isfinite(block[k]) || std::fabs(block[k]) > 2.0f)
					bad.store(true);
			got_frames.fetch_add(got, std::memory_order_relaxed);
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	});

	std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
	for (u64 i = 0; i < 3000; i++)
	{
		FillChunk(chunk.data(), i);
		s.WriteChunk(chunk.data());
		if (i % 8 == 7)
			std::this_thread::sleep_for(std::chrono::microseconds(300));
	}
	EXPECT_TRUE(WaitUntilProcessed(s));
	stop.store(true, std::memory_order_release);
	consumer.join();
	EXPECT_FALSE(bad.load());
	EXPECT_GT(got_frames.load(), 10000u);
}

#ifdef AUDIO_STREAM_TEST_HAS_CPU_CLOCKS

namespace
{
	double ThreadCpuUs()
	{
		timespec ts;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
		return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
	}

	double ProcessCpuUs()
	{
		timespec ts;
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
		return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
	}
} // namespace

// What a WriteChunk() costs the thread that calls it, in the regime where the stretcher works hardest: a producer
// that runs a little slower or faster than a real-time consumer. Prints one BENCH line per speed.
//
// Disabled by default because it runs for real time (a few seconds per speed). Run it with
//   core_test --gtest_also_run_disabled_tests --gtest_filter='AudioStream.DISABLED_ProducerCost*'
// Pin it with taskset to measure a particular core class. The process CPU time minus the producer's and the
// consumer's is the work done on any other thread (the stretch worker once there is one).
TEST(AudioStream, DISABLED_ProducerCostPerChunkAtSeveralSpeeds)
{
	const double seconds = 6.0;
	for (const double speed : {1.00, 0.93, 1.10})
	{
		TestStream s(DefaultParams());
		s.Init(true);

		std::atomic<bool> stop{false};
		std::atomic<u64> consumed{0};
		std::atomic<double> consumer_cpu_us{0};
		std::thread consumer([&]() {
			std::vector<float> block(1024 * 2);
			auto next = std::chrono::steady_clock::now();
			while (!stop.load(std::memory_order_acquire))
			{
				next += std::chrono::milliseconds(10);
				std::this_thread::sleep_until(next);
				// The backend asks for 10 ms and gets silence plus an underrun note when the ring is short.
				s.PullFrames(block.data(), 480);
				consumed.fetch_add(480, std::memory_order_relaxed);
			}
			consumer_cpu_us.store(ThreadCpuUs());
		});

		const double chunk_period_us = 1e6 * AudioStream::CHUNK_SIZE / (kRate * speed);
		const u64 chunks = static_cast<u64>(seconds * 1e6 / chunk_period_us);
		std::vector<float> chunk(AudioStream::CHUNK_SIZE * 2);
		std::vector<double> call_us;
		std::vector<u32> backlog;
		call_us.reserve(chunks);
		backlog.reserve(chunks);

		const double cpu0 = ThreadCpuUs();
		const double proc0 = ProcessCpuUs();
		const auto t0 = std::chrono::steady_clock::now();
		for (u64 i = 0; i < chunks; i++)
		{
			FillChunk(chunk.data(), i);
			std::this_thread::sleep_until(t0 + std::chrono::nanoseconds(static_cast<long long>(i * chunk_period_us * 1e3)));
			const auto a = std::chrono::steady_clock::now();
			// Chunks the stretch worker has not finished when this one is handed over: the delay the queue adds is this
			// many chunks of audio (1.33 ms each) plus the time to stretch them.
			backlog.push_back(s.GetPendingInputChunks());
			const double c0 = ThreadCpuUs();
			s.WriteChunk(chunk.data());
			const double c1 = ThreadCpuUs();
			const auto b = std::chrono::steady_clock::now();
			(void)a;
			(void)b;
			call_us.push_back(c1 - c0);
		}
		const double producer_cpu = ThreadCpuUs() - cpu0;
		stop.store(true, std::memory_order_release);
		consumer.join();
		const double proc_cpu = ProcessCpuUs() - proc0;

		std::vector<double> sorted = call_us;
		std::sort(sorted.begin(), sorted.end());
		double sum = 0;
		for (double v : call_us)
			sum += v;
		std::vector<u32> sorted_backlog = backlog;
		std::sort(sorted_backlog.begin(), sorted_backlog.end());
		double backlog_sum = 0;
		for (u32 v : backlog)
			backlog_sum += v;
		std::printf("BENCH speed=%.2f chunks=%llu WriteChunk_cpu_us mean=%.2f p50=%.2f p99=%.2f max=%.2f | producer_thread_cpu_us_per_chunk=%.2f (incl. the fill) | process_cpu_us_per_chunk=%.2f consumer_cpu_us_per_chunk=%.2f other_threads_us_per_chunk=%.2f | backlog_chunks mean=%.2f p99=%u max=%u\n",
			speed, static_cast<unsigned long long>(chunks), sum / call_us.size(), sorted[sorted.size() / 2],
			sorted[sorted.size() * 99 / 100], sorted.back(), producer_cpu / chunks, proc_cpu / chunks,
			consumer_cpu_us.load() / chunks, (proc_cpu - producer_cpu - consumer_cpu_us.load()) / chunks,
			backlog_sum / backlog.size(), sorted_backlog[sorted_backlog.size() * 99 / 100], sorted_backlog.back());
	}
}

#endif // AUDIO_STREAM_TEST_HAS_CPU_CLOCKS
