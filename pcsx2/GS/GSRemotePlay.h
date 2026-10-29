// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Remote play: what the renderer and the sound mixer hand to the app's streaming threads
// (platforms/android/app/src/main/cpp/remote_play.cpp), which send the game to a friend's device
// where they play as player 2. Only the Android app turns it on; elsewhere nothing captures.
//
// Pictures: while remote play runs, the renderer draws every presented frame on the GPU, at the
// stream's size, into a slot of a four-slot ring, and signals when the frame is in: Vulkan has the GPU
// write the slot's sequence number to markers[i], OpenGL leaves a fence for is_done. The app's video
// thread then takes the frame, which is one of two kinds, chosen by the renderer as it records:
//  - hardware: the slot's AHardwareBuffer, filled with RGBA. The app hands it to the video encoder as
//    a GL texture and gives the slot back when the encoder has drawn it, so the frame never passes
//    through the CPU.
//  - readback: RGBA in the slot's mapped buffer, which the app converts to the encoder's NV12 as it
//    copies it out. For a device that cannot share buffers, or an app that could not import one.
// The renderer only ever moves a slot from free to recorded; the app's side moves it on and back,
// so neither waits on the other.
//
// Sound: SPU2's mixed output, as 48 kHz stereo, in a ring the app's audio thread reads from.

#include "common/Pcsx2Defs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>

struct AHardwareBuffer;

namespace GSRemotePlay
{
	/// The stream's frame size, packed (width << 16) | height, while remote play runs; 0 otherwise.
	/// The renderer captures every presented frame while it is set, and lets its capture resources
	/// go once it is 0.
	inline std::atomic<u32> g_max_size{0};

	/// Frames may go out as shareable buffers, unless the app has said it cannot import them.
	inline std::atomic<bool> g_hardware{false};

	/// Set by a renderer that can capture, for as long as it exists. Remote play does not start
	/// without one: the friend would get sound and no picture.
	inline std::atomic<bool> g_renderer_captures{false};

	/// Counted by the renderer while capturing: the frames it presented, and the ones it could not
	/// capture because the GPU or the app still held every slot. Part of the host's stream stats.
	inline std::atomic<u32> g_frames{0};
	inline std::atomic<u32> g_skipped{0};

	enum SlotState : u32
	{
		SlotFree = 0, ///< the renderer may record into it
		SlotRecorded = 1, ///< the GPU is filling it, or has: markers[i] == seq says which
		SlotHeld = 2, ///< hardware frame: the app has it until it gives the slot back
		SlotPending = 3, ///< the renderer's still: a readback it has yet to copy out (OpenGL)
	};

	struct Slot
	{
		std::atomic<u32> state{SlotFree};
		u32 seq = 0;
		u32 width = 0;
		u32 height = 0;

		/// When the renderer recorded the frame (steady clock, ns), for how long the GPU usually takes.
		u64 recorded_ns = 0;

		/// Readback frames: RGBA rows of width * 4 bytes, mapped for as long as the slot exists.
		const u8* data = nullptr;

		/// Hardware frames: the buffer, and a number that changes whenever it is replaced, so the app
		/// knows to import it again (from g_generation, so it never repeats within the process). The
		/// buffer pointer only changes under g_mutex, on a free slot. hardware says which kind the
		/// recorded frame is.
		AHardwareBuffer* hardware_buffer = nullptr;
		u32 generation = 0;
		bool hardware = false;
	};

	struct Ring
	{
		std::array<Slot, 4> slots;

		/// One per slot, written by the GPU: markers[i] == seq once slot i's frame is in (Vulkan).
		const volatile u32* markers = nullptr;

		/// Without markers: whether slot index's recorded frame is in, as its fence says (OpenGL).
		/// Called under g_mutex.
		bool (*is_done)(void* context, u32 index) = nullptr;

		/// Makes a readback slot's data visible to the CPU, where its memory is not coherent. Called
		/// under g_mutex before the data is read; may be null.
		void (*prepare_read)(void* context, u32 index) = nullptr;
		void* context = nullptr;
	};

	/// The ring in use, or null. The renderer publishes and withdraws it under the mutex, and the
	/// app's side holds the mutex while it reads a slot, so a ring is never torn down mid-read.
	inline std::mutex g_mutex;
	inline Ring* g_ring = nullptr;

	/// Hardware buffer generations, process-wide, so a renderer that comes back never repeats one the
	/// app has already imported.
	inline std::atomic<u32> g_generation{0};

	/// Counts the frames the renderer has recorded. The app's video thread sleeps on it until there is
	/// a frame to wait for, instead of checking every millisecond in between.
	inline std::mutex g_recorded_mutex;
	inline std::condition_variable g_recorded_cv;
	inline u32 g_recorded = 0; ///< under g_recorded_mutex

	inline void NotifyRecorded()
	{
		{
			std::lock_guard lock(g_recorded_mutex);
			g_recorded++;
		}
		g_recorded_cv.notify_all();
	}

	inline constexpr u32 AUDIO_SAMPLE_RATE = 48000;

	/// About a third of a second. A power of two, so positions wrap with it.
	inline constexpr u32 AUDIO_RING_FRAMES = 16384;

	/// SPU2 copies its output here while this is set.
	inline std::atomic<bool> g_audio_on{false};

	/// Interleaved stereo. Frame n is at (n % AUDIO_RING_FRAMES) * 2.
	inline std::array<s16, AUDIO_RING_FRAMES * 2> g_audio_ring{};

	/// Frames written since remote play started, wrapping. Only SPU2 writes it.
	inline std::atomic<u32> g_audio_written{0};

	/// The reader waits for g_audio_written to reach g_audio_target. SPU2 wakes it once it has, and
	/// only then: every block it mixes is a little over a millisecond, far more often than the reader
	/// wants to hear about.
	inline std::mutex g_audio_mutex;
	inline std::condition_variable g_audio_cv;
	inline std::atomic<bool> g_audio_waiting{false};
	inline std::atomic<u32> g_audio_target{0};

	/// SPU2's thread: one block of its output, interleaved stereo floats in [-1, 1].
	inline void PushAudio(const float* samples, u32 frames)
	{
		u32 written = g_audio_written.load(std::memory_order_relaxed);
		for (u32 i = 0; i < frames; i++)
		{
			const u32 at = ((written + i) & (AUDIO_RING_FRAMES - 1)) * 2;
			g_audio_ring[at] = static_cast<s16>(std::clamp(samples[i * 2] * 32767.0f, -32768.0f, 32767.0f));
			g_audio_ring[at + 1] = static_cast<s16>(std::clamp(samples[i * 2 + 1] * 32767.0f, -32768.0f, 32767.0f));
		}
		written += frames;
		g_audio_written.store(written, std::memory_order_seq_cst);

		if (g_audio_waiting.load(std::memory_order_seq_cst) &&
			static_cast<s32>(written - g_audio_target.load(std::memory_order_relaxed)) >= 0)
		{
			// Under the lock, so the reader is either asleep or has not looked yet: never between.
			std::lock_guard lock(g_audio_mutex);
			g_audio_cv.notify_one();
		}
	}
} // namespace GSRemotePlay
