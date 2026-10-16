// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Remote play, core side: hands the running game's picture and sound to the app, which streams them
// to a friend's device, where the friend plays as player 2 (com.armsx2.remoteplay).
//
// Nothing here talks to the network. The app's streaming threads pull frames and sound out through
// the NativeApp.remotePlay* calls below; pulling rather than calling into Java keeps JNI off the GS
// and SPU2 threads, which must never wait on the app.
//
// Frames come from the renderer's capture (GS/GSRemotePlay.h), drawn on the GPU at the stream's size
// and picked up here, on the app's video thread, as soon as the GPU marks them done. Where the device
// can share buffers, a frame is an AHardwareBuffer the app hands straight to the video encoder and
// gives back afterwards (remotePlayReleaseFrame), so it never passes through the CPU; otherwise it is
// RGBA read back, which is converted to NV12 here as it is copied out. Sound is SPU2's output, as it
// leaves the mixer.

#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/log.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "common/Pcsx2Defs.h"
#include "pcsx2/GS/GSRemotePlay.h"
#include "pcsx2/VMManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#define RP_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RemotePlay", __VA_ARGS__)

namespace
{
	std::atomic<bool> s_running{false};

	// Frames handed to the app, and finished frames passed over because a newer one was ready.
	std::atomic<u32> s_delivered{0};
	std::atomic<u32> s_superseded{0};

	// How long the GPU usually takes from recording a frame to finishing it, in us, averaged. Only the
	// app's video thread uses it, to sleep through most of that wait instead of polling.
	u64 s_gpu_delay_us = 8000;

	// Where the app's audio thread has read up to in GSRemotePlay's ring. Only that thread polls.
	std::mutex s_audio_poll_mutex;
	u32 s_audio_read = 0;

	u64 SteadyNs()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// RGBA rows to NV12 (the Y plane, then interleaved UV, rows packed): BT.601 limited range, as the
	// encoder expects, with each chroma sample from the average of its 2x2 block.
	void RgbaToNv12(const u8* src, u32 width, u32 height, u8* dst)
	{
		u8* const y_plane = dst;
		u8* const uv_plane = dst + static_cast<size_t>(width) * height;
		const size_t stride = static_cast<size_t>(width) * 4;

		for (u32 y = 0; y < height; y++)
		{
			const u8* row = src + stride * y;
			u8* out = y_plane + static_cast<size_t>(width) * y;
			for (u32 x = 0; x < width; x++)
			{
				const s32 r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
				out[x] = static_cast<u8>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
			}
		}

		for (u32 y = 0; y < height / 2; y++)
		{
			const u8* top = src + stride * (y * 2);
			const u8* bottom = top + stride;
			u8* out = uv_plane + static_cast<size_t>(width) * y;
			for (u32 x = 0; x < width / 2; x++)
			{
				const u32 i = x * 8;
				const s32 r = (top[i] + top[i + 4] + bottom[i] + bottom[i + 4] + 2) >> 2;
				const s32 g = (top[i + 1] + top[i + 5] + bottom[i + 1] + bottom[i + 5] + 2) >> 2;
				const s32 b = (top[i + 2] + top[i + 6] + bottom[i + 2] + bottom[i + 6] + 2) >> 2;
				out[x * 2] = static_cast<u8>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
				out[x * 2 + 1] = static_cast<u8>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
			}
		}
	}

	// The newest frame the GPU has finished. Finished frames older than it are freed unread: for a
	// live stream they are already late. A hardware frame stays with the app (its slot held) until
	// remotePlayReleaseFrame; a readback frame is converted into dst and its slot freed at once.
	// Returns 2 with a hardware frame (info: width, height, slot, generation, seq), 1 with an NV12
	// frame in dst (info: width, height), 0 when none is finished yet, -2 when cap is too small for
	// one. pending_since: when the oldest frame still on the GPU was recorded, or 0 for none.
	int TakeFrame(u8* dst, size_t cap, s32* info, u64& pending_since)
	{
		std::lock_guard lock(GSRemotePlay::g_mutex);

		pending_since = 0;
		GSRemotePlay::Ring* const ring = GSRemotePlay::g_ring;
		if (!ring || (!ring->markers && !ring->is_done))
			return 0;

		GSRemotePlay::Slot* newest = nullptr;
		u32 newest_index = 0;
		for (u32 i = 0; i < ring->slots.size(); i++)
		{
			GSRemotePlay::Slot& slot = ring->slots[i];
			if (slot.state.load(std::memory_order_acquire) != GSRemotePlay::SlotRecorded)
				continue;

			const bool done = ring->markers ? ring->markers[i] == slot.seq : ring->is_done(ring->context, i);
			if (!done)
			{
				if (!pending_since || slot.recorded_ns < pending_since)
					pending_since = slot.recorded_ns;
				continue;
			}

			if (newest && static_cast<s32>(slot.seq - newest->seq) < 0)
			{
				slot.state.store(GSRemotePlay::SlotFree, std::memory_order_release);
				s_superseded.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			if (newest)
			{
				newest->state.store(GSRemotePlay::SlotFree, std::memory_order_release);
				s_superseded.fetch_add(1, std::memory_order_relaxed);
			}

			newest = &slot;
			newest_index = i;
		}

		if (!newest)
			return 0;

		// The marker or fence was read before the frame; keep it that way.
		std::atomic_thread_fence(std::memory_order_acquire);

		// How long that one took, for the next wait.
		const u64 now = SteadyNs();
		if (now > newest->recorded_ns)
		{
			const u64 took_us = std::min<u64>((now - newest->recorded_ns) / 1000, 100'000);
			s_gpu_delay_us = (s_gpu_delay_us * 7 + took_us) / 8;
		}

		info[0] = static_cast<s32>(newest->width);
		info[1] = static_cast<s32>(newest->height);
		info[4] = static_cast<s32>(newest->seq);
		info[5] = 0;

		if (newest->hardware)
		{
			info[2] = static_cast<s32>(newest_index);
			info[3] = static_cast<s32>(newest->generation);
			newest->state.store(GSRemotePlay::SlotHeld, std::memory_order_release);
			s_delivered.fetch_add(1, std::memory_order_relaxed);
			return 2;
		}

		info[2] = -1;
		info[3] = 0;

		const size_t size = static_cast<size_t>(newest->width) * newest->height * 3 / 2;
		int result = -2;
		if (cap >= size && newest->data)
		{
			if (ring->prepare_read)
				ring->prepare_read(ring->context, newest_index);
			RgbaToNv12(newest->data, newest->width, newest->height, dst);
			s_delivered.fetch_add(1, std::memory_order_relaxed);
			result = 1;
		}

		newest->state.store(GSRemotePlay::SlotFree, std::memory_order_release);
		return result;
	}

	struct RemotePlayEgl
	{
		PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getNativeClientBuffer = nullptr;
		PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
		PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
		PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;

		bool Load()
		{
			if (!createImage)
			{
				getNativeClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
					eglGetProcAddress("eglGetNativeClientBufferANDROID"));
				createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
				destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
				imageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
					eglGetProcAddress("glEGLImageTargetTexture2DOES"));
			}
			return getNativeClientBuffer && createImage && destroyImage && imageTargetTexture;
		}
	};

	RemotePlayEgl s_egl;

	// The AHardwareBuffer behind a hardware frame's slot, with a reference the caller releases, or null.
	AHardwareBuffer* AcquireSlotBuffer(int slot)
	{
		std::lock_guard lock(GSRemotePlay::g_mutex);
		GSRemotePlay::Ring* const ring = GSRemotePlay::g_ring;
		if (!ring || slot < 0 || static_cast<u32>(slot) >= ring->slots.size())
			return nullptr;

		AHardwareBuffer* const buffer = ring->slots[slot].hardware_buffer;
		if (buffer)
			AHardwareBuffer_acquire(buffer);
		return buffer;
	}
} // namespace

// Starts handing frames and sound to the app, up to maxWidth x maxHeight. Only while a game runs, and
// only on a renderer that can capture: the friend would otherwise get sound and no picture.
extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayStart(JNIEnv*, jclass, jint maxWidth, jint maxHeight)
{
	if (!VMManager::HasValidVM())
	{
		RP_LOG("remote play: no game running");
		return JNI_FALSE;
	}
	if (!GSRemotePlay::g_renderer_captures.load(std::memory_order_acquire))
	{
		RP_LOG("remote play: this renderer cannot capture frames");
		return JNI_FALSE;
	}

	{
		std::lock_guard lock(s_audio_poll_mutex);
		s_audio_read = GSRemotePlay::g_audio_written.load(std::memory_order_acquire);
	}

	GSRemotePlay::g_audio_on.store(true, std::memory_order_release);
	GSRemotePlay::g_max_size.store((static_cast<u32>(std::clamp(maxWidth, 16, 4096)) << 16) |
								   static_cast<u32>(std::clamp(maxHeight, 16, 4096)),
		std::memory_order_release);
	s_running.store(true, std::memory_order_release);
	RP_LOG("remote play: capture started, up to %dx%d", maxWidth, maxHeight);
	return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayStop(JNIEnv*, jclass)
{
	const bool was_running = s_running.exchange(false, std::memory_order_acq_rel);

	// The renderer lets its capture buffers go at its next frame once this is 0.
	GSRemotePlay::g_max_size.store(0, std::memory_order_release);
	GSRemotePlay::g_audio_on.store(false, std::memory_order_release);

	// The app's threads may be asleep waiting for a frame or sound: wake them to see the stop.
	GSRemotePlay::NotifyRecorded();
	{
		std::lock_guard lock(GSRemotePlay::g_audio_mutex);
		GSRemotePlay::g_audio_cv.notify_all();
	}

	if (was_running)
		RP_LOG("remote play: capture stopped");
}

// Waits up to timeoutMs for a frame. The app's video thread. buffer: a direct ByteBuffer for an NV12
// frame (the Y plane, then interleaved UV, rows packed). info (6 ints): width, height, slot,
// generation, seq, unused. Returns:
//  2: a hardware frame (nothing written to buffer). The slot is the app's until
//     remotePlayReleaseFrame(slot, seq); remotePlayImportFrame binds its buffer to a texture.
//  1: an NV12 frame written to buffer. info: width, height, -1, 0, seq.
//  0: timeout. -1: capture is not running. -2: buffer is too small (info holds the size; the frame
//     is dropped).
extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayPollVideo(JNIEnv* env, jclass, jobject buffer, jintArray info, jint timeoutMs)
{
	if (!s_running.load(std::memory_order_acquire) || !buffer || !info || env->GetArrayLength(info) < 6)
		return -1;

	u8* const dst = static_cast<u8*>(env->GetDirectBufferAddress(buffer));
	const jlong cap = env->GetDirectBufferCapacity(buffer);
	if (!dst || cap <= 0)
		return -1;

	s32 out[6] = {};
	int result = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));

	while (true)
	{
		// Read before looking, so a frame recorded in between still wakes the wait below.
		u32 recorded;
		{
			std::lock_guard lock(GSRemotePlay::g_recorded_mutex);
			recorded = GSRemotePlay::g_recorded;
		}

		u64 pending_since = 0;
		result = TakeFrame(dst, static_cast<size_t>(cap), out, pending_since);
		if (result != 0)
			break;

		if (!s_running.load(std::memory_order_acquire))
		{
			result = -1;
			break;
		}

		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline)
			break;

		if (pending_since)
		{
			// Recorded and still on the GPU, which says nothing when it is done. Sleep through most of
			// the time it usually takes, then look every millisecond.
			const u64 waited_us = (SteadyNs() - pending_since) / 1000;
			const u64 sleep_us = waited_us + 1500 < s_gpu_delay_us ? s_gpu_delay_us - waited_us - 1000 : 1000;
			const u64 left_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count());
			std::this_thread::sleep_for(std::chrono::microseconds(std::min<u64>({sleep_us, left_us, 20'000})));
		}
		else
		{
			// Nothing recorded: sleep until the renderer records a frame.
			std::unique_lock lock(GSRemotePlay::g_recorded_mutex);
			GSRemotePlay::g_recorded_cv.wait_until(lock, deadline, [recorded] {
				return GSRemotePlay::g_recorded != recorded || !s_running.load(std::memory_order_acquire);
			});
		}
	}

	env->SetIntArrayRegion(info, 0, 6, out);
	return result;
}

// Gives a hardware frame's slot back to the renderer, once the encoder has drawn it. seq is the
// frame's, so a late release from an earlier session cannot free a slot that now holds another.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayReleaseFrame(JNIEnv*, jclass, jint slot, jint seq)
{
	std::lock_guard lock(GSRemotePlay::g_mutex);
	GSRemotePlay::Ring* const ring = GSRemotePlay::g_ring;
	if (!ring || slot < 0 || static_cast<u32>(slot) >= ring->slots.size() ||
		ring->slots[slot].seq != static_cast<u32>(seq))
	{
		return;
	}

	u32 held = GSRemotePlay::SlotHeld;
	ring->slots[slot].state.compare_exchange_strong(held, GSRemotePlay::SlotFree, std::memory_order_acq_rel);
}

// Whether frames may go out as shared buffers. The app turns it off when it cannot import them; the
// renderer then reads frames back from its next free slot on.
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlaySetHardware(JNIEnv*, jclass, jboolean enabled)
{
	GSRemotePlay::g_hardware.store(enabled == JNI_TRUE, std::memory_order_release);
}

// Binds a hardware frame slot's buffer to texture (GL_TEXTURE_EXTERNAL_OES). Needs a current EGL
// context on the calling thread. Returns the EGLImage to destroy later with remotePlayDestroyImage,
// or 0 when the slot has no buffer or the import failed; the app then asks for NV12 frames instead.
extern "C" JNIEXPORT jlong JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayImportFrame(JNIEnv*, jclass, jint slot, jint texture)
{
	if (!s_egl.Load())
		return 0;

	AHardwareBuffer* const buffer = AcquireSlotBuffer(slot);
	if (!buffer)
		return 0;

	const EGLDisplay display = eglGetCurrentDisplay();
	const EGLClientBuffer client = s_egl.getNativeClientBuffer(buffer);
	const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
	EGLImageKHR image = EGL_NO_IMAGE_KHR;
	if (display != EGL_NO_DISPLAY && client)
		image = s_egl.createImage(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attributes);

	// The image holds the buffer from here on.
	AHardwareBuffer_release(buffer);

	if (image == EGL_NO_IMAGE_KHR)
		return 0;

	// Errors left over from earlier GL work on this context would read as a failed bind.
	for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++)
	{
	}

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLuint>(texture));
	s_egl.imageTargetTexture(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(image));
	const bool bound = glGetError() == GL_NO_ERROR;
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

	if (!bound)
	{
		s_egl.destroyImage(display, image);
		return 0;
	}

	return reinterpret_cast<jlong>(image);
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayDestroyImage(JNIEnv*, jclass, jlong image)
{
	if (image != 0 && s_egl.Load())
		s_egl.destroyImage(eglGetCurrentDisplay(), reinterpret_cast<EGLImageKHR>(image));
}

// out (4 ints): running totals of frames presented while capturing, frames not captured (no free
// slot), frames handed to the app and finished frames passed over for a newer one.
extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayCounters(JNIEnv* env, jclass, jintArray out)
{
	if (!out || env->GetArrayLength(out) < 4)
		return JNI_FALSE;

	const jint values[4] = {
		static_cast<jint>(GSRemotePlay::g_frames.load(std::memory_order_relaxed)),
		static_cast<jint>(GSRemotePlay::g_skipped.load(std::memory_order_relaxed)),
		static_cast<jint>(s_delivered.load(std::memory_order_relaxed)),
		static_cast<jint>(s_superseded.load(std::memory_order_relaxed)),
	};
	env->SetIntArrayRegion(out, 0, 4, values);
	return JNI_TRUE;
}

// Waits up to timeoutMs for capFrames of sound and writes them as stereo s16 at 48 kHz into buffer (a
// direct ByteBuffer). Full chunks, so each network packet carries capFrames rather than whatever the
// mixer last made. On timeout it returns what there is. Returns the frames written, 0 when there
// were none, -1 when capture is not running.
extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayPollAudio(JNIEnv* env, jclass, jobject buffer, jint capFrames, jint timeoutMs)
{
	if (!s_running.load(std::memory_order_acquire) || !buffer || capFrames <= 0)
		return -1;

	s16* const dst = static_cast<s16*>(env->GetDirectBufferAddress(buffer));
	const jlong cap = env->GetDirectBufferCapacity(buffer);
	if (!dst || cap < 4)
		return -1;

	const u32 want = static_cast<u32>(std::min<jlong>(capFrames, cap / 4));
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));

	std::lock_guard poll(s_audio_poll_mutex);
	u32 available = 0;
	while (true)
	{
		const u32 written = GSRemotePlay::g_audio_written.load(std::memory_order_acquire);
		available = written - s_audio_read;

		// Far behind (the network thread stalled): what was not sent in time is late now, and part of
		// it is already overwritten. Carry on from the newest.
		if (available > GSRemotePlay::AUDIO_RING_FRAMES / 2)
		{
			s_audio_read = written - want;
			available = want;
		}

		if (available >= want)
			break;
		if (!s_running.load(std::memory_order_acquire))
			return -1;
		if (std::chrono::steady_clock::now() >= deadline)
			break;

		// The mixer makes a block every millisecond or so; sleep until there is a chunk's worth.
		std::unique_lock lock(GSRemotePlay::g_audio_mutex);
		const u32 target = s_audio_read + want;
		GSRemotePlay::g_audio_target.store(target, std::memory_order_relaxed);
		GSRemotePlay::g_audio_waiting.store(true, std::memory_order_seq_cst);
		if (static_cast<s32>(GSRemotePlay::g_audio_written.load(std::memory_order_seq_cst) - target) < 0 &&
			s_running.load(std::memory_order_acquire))
		{
			GSRemotePlay::g_audio_cv.wait_until(lock, deadline);
		}
		GSRemotePlay::g_audio_waiting.store(false, std::memory_order_relaxed);
	}

	const u32 frames = std::min(available, want);
	for (u32 i = 0; i < frames; i++)
	{
		const u32 at = ((s_audio_read + i) & (GSRemotePlay::AUDIO_RING_FRAMES - 1)) * 2;
		dst[i * 2] = GSRemotePlay::g_audio_ring[at];
		dst[i * 2 + 1] = GSRemotePlay::g_audio_ring[at + 1];
	}
	s_audio_read += frames;
	return static_cast<jint>(frames);
}

// Whether the game is paused: the friend's device says so rather than showing a frozen picture.
extern "C" JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_remotePlayPaused(JNIEnv*, jclass)
{
	return VMManager::GetState() == VMState::Paused ? JNI_TRUE : JNI_FALSE;
}
