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

		void Init(bool stretch)
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
			BaseInitialize(readers[static_cast<size_t>(m_parameters.expansion_mode)], stretch);
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
	TestStream s(DefaultParams());
	s.Init(true);
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
		call_us.reserve(chunks);

		const double cpu0 = ThreadCpuUs();
		const double proc0 = ProcessCpuUs();
		const auto t0 = std::chrono::steady_clock::now();
		for (u64 i = 0; i < chunks; i++)
		{
			FillChunk(chunk.data(), i);
			std::this_thread::sleep_until(t0 + std::chrono::nanoseconds(static_cast<long long>(i * chunk_period_us * 1e3)));
			const auto a = std::chrono::steady_clock::now();
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
		std::printf("BENCH speed=%.2f chunks=%llu WriteChunk_cpu_us mean=%.2f p50=%.2f p99=%.2f max=%.2f | producer_thread_cpu_us_per_chunk=%.2f (incl. the fill) | process_cpu_us_per_chunk=%.2f consumer_cpu_us_per_chunk=%.2f other_threads_us_per_chunk=%.2f\n",
			speed, static_cast<unsigned long long>(chunks), sum / call_us.size(), sorted[sorted.size() / 2],
			sorted[sorted.size() * 99 / 100], sorted.back(), producer_cpu / chunks, proc_cpu / chunks,
			consumer_cpu_us.load() / chunks, (proc_cpu - producer_cpu - consumer_cpu_us.load()) / chunks);
	}
}

#endif // AUDIO_STREAM_TEST_HAS_CPU_CLOCKS
