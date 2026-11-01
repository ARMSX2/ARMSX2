// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "USB/shared/ringbuffer.h"
#include "USB/USB.h"
#include "audiodev.h"

#include <oboe/Oboe.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace usb_mic
{
	namespace audiodev_oboe
	{
		class OboeAudioDevice final : public AudioDevice, oboe::AudioStreamDataCallback, oboe::AudioStreamErrorCallback
		{
		public:
			OboeAudioDevice(AudioDir dir, u32 channels, std::string devname, s32 latency);
			~OboeAudioDevice();

			static std::vector<std::pair<std::string, std::string>> GetDeviceList(bool input);

			// Android lists its input devices to Java only: the app hands them here under the names the
			// settings store (MicDevices.kt), one "<Android device id>\t<name>" per line. A device gets a
			// new id each time it is plugged in, so the list goes again before every game starts and
			// whenever one comes or goes.
			static void SetDeviceList(std::vector<std::pair<std::string, s32>> devices);

			// Input gain for capture instances, as a linear factor (1.0 = unity). The handheld's built-in mic is
			// quieter than the dynamic capsule SingStar games were tuned for, so the settings may boost what
			// reaches the emulated device; they store it in decibels and push it here before any stream opens.
			static void SetInputGain(float linear);

			uint32_t GetBuffer(short* buff, uint32_t frames) override;
			uint32_t SetBuffer(short* buff, uint32_t frames) override;
			bool GetFrames(uint32_t* size) override;
			void SetResampling(int samplerate) override;
			bool Start() override;
			void Stop() override;

		protected:
			void ResetBuffers();

			oboe::DataCallbackResult onAudioReady(oboe::AudioStream* p_audioStream, void* p_audioData, int32_t p_numFrames) override;
			bool onError(oboe::AudioStream* oboeStream, oboe::Result error) override;

		private:
			// ★ Serialises the stream lifecycle. onError() runs on OBOE'S OWN callback thread and tears
			// the stream down and back up (Stop/Open/Start), while the CPU thread can be inside
			// Start()/Stop() on the very same object. Stop()'s `if (m_stream)` followed by
			// `m_stream->requestStop()` is not atomic against onError's `m_stream.reset()`, so the
			// stream could be destroyed between the null check and the dereference — a use-after-free.
			// Same rationale as OboeAudioStream::m_lock; recursive because Start()/onError() re-enter
			// through Open().
			std::recursive_mutex m_lock;

			bool m_playing = false;
			// Written by Start()/Stop() on the CPU thread, read by onError() on the callback thread.
			std::atomic<bool> m_stop_requested{false};

			std::shared_ptr<oboe::AudioStream> m_stream;

			// Performance mode the stream is (re)opened with. Starts at LowLatency; Start() downgrades it
			// to None if the device refuses the fast path (some Adreno/AAudio devices fail requestStart()
			// with ErrorDisconnected at boot). onError()'s reopen then reuses whatever mode actually worked.
			oboe::PerformanceMode m_perf_mode = oboe::PerformanceMode::LowLatency;

			bool Open();
			bool StartStream();

			u32 mSampleRate = 48000;
			u32 mLatency = 50;
			u32 mStreamLatency = 0;
			std::string mDeviceName;

			// Distinguishes this instance from any other OboeAudioDevice in the process (the mic-test
			// device) when reading logs.
			int m_id = 0;

			RingBuffer mBuffer;
			std::mutex mMutex;

			// Scratch for the input gain: p_audioData must not be modified in place, and the ring buffer must
			// never see out-of-range samples. Touched only from onAudioReady's callback thread.
			std::vector<short> mGainScratch;
		};
	} // namespace audiodev_oboe
} // namespace usb_mic
