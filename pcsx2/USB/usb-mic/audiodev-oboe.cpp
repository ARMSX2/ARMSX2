// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "USB/usb-mic/audiodev-oboe.h"
#include "USB/USB.h"
#include "Host.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"

#include <oboe/Oboe.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace usb_mic
{
	namespace audiodev_oboe
	{
		OboeAudioDevice::OboeAudioDevice(AudioDir dir, u32 channels, std::string devname, s32 latency)
			: AudioDevice(dir, channels)
			, mLatency(latency > 0 ? static_cast<u32>(latency) : 50u)
			, mDeviceName(std::move(devname))
		{
			static std::atomic<int> s_next_id{1};
			m_id = s_next_id++;
			Console.WriteLn("(audiodev_oboe)[%d] constructed: dir=%s ch=%u dev='%s' latency=%d", m_id, (dir == AUDIODIR_SOURCE) ? "SOURCE" : "SINK", channels, mDeviceName.c_str(), latency);
		}

		OboeAudioDevice::~OboeAudioDevice()
		{
			Stop();
		}

		std::vector<std::pair<std::string, std::string>> OboeAudioDevice::GetDeviceList(bool input)
		{
			// Android has no device enumeration in this path: the handhelds we target have a single
			// built-in microphone and Oboe opens it by default. "cubeb_default" is the backend-neutral
			// marker for "the platform's default device", shared with cubeb so a config written on one
			// backend reads correctly on the other. The Android UI lists devices itself (MicDevices.kt)
			// and pushes them to SetDeviceList; this list only serves backends without that path.
			std::vector<std::pair<std::string, std::string>> ret;
			ret.emplace_back("", TRANSLATE_SV("USB", "Not Connected"));
			ret.emplace_back("cubeb_default", input ? TRANSLATE_SV("USB", "Default Input Device") : TRANSLATE_SV("USB", "Default Output Device"));
			return ret;
		}

		namespace
		{
			std::mutex s_device_list_mutex;
			std::vector<std::pair<std::string, s32>> s_device_list;

			// Linear input gain for every capture stream in the process (1.0 = unity). Set from the settings'
			// decibel value before a game's mic opens and again by the microphone test, so both hear what
			// the game would receive.
			std::atomic<float> s_input_gain{1.0f};
		}

		void OboeAudioDevice::SetDeviceList(std::vector<std::pair<std::string, s32>> devices)
		{
			const std::lock_guard lock(s_device_list_mutex);
			s_device_list = std::move(devices);
		}

		void OboeAudioDevice::SetInputGain(float linear)
		{
			if (linear < 0.0f)
				linear = 0.0f;
			s_input_gain.store(linear, std::memory_order_release);
		}

		bool OboeAudioDevice::Open()
		{
			const std::lock_guard<std::recursive_mutex> guard(m_lock);

			// The voice performance preset first: it is the one for singing into, with no gain control or
			// noise suppression to fight pitch detection (the same choice ARMSX3 makes for its Oboe
			// capture). Generic is the fallback.
			static constexpr oboe::InputPreset kPresets[] = {
				oboe::InputPreset::VoicePerformance,
				oboe::InputPreset::Generic,
			};

			for (const oboe::InputPreset preset : kPresets)
			{
				// First the native path: exactly what we asked for. If the HAL cannot deliver 48 kHz / our
				// channel count natively, retry with AAudio's conversion so the emulated mic still sees
				// 48 kHz samples at the requested layout.
				for (const bool convert : { false, true })
				{
					oboe::AudioStreamBuilder builder;
					builder.setDirection((mAudioDir == AUDIODIR_SOURCE) ? oboe::Direction::Input : oboe::Direction::Output);
					builder.setPerformanceMode(m_perf_mode);
					// Shared, not exclusive: the game's SPU2 output (Host/OboeAudioStream.cpp) and Android's own
					// audio stack hold the same hardware at the same time. Exclusive would fail to open while
					// anything else is playing.
					builder.setSharingMode(oboe::SharingMode::Shared);
					if (mAudioDir == AUDIODIR_SOURCE)
						builder.setInputPreset(preset);
					// S16, like cubeb's CUBEB_SAMPLE_S16LE: the ring buffer and the emulated PS2 mic both speak
					// int16, so no conversion at either end of the data path.
					builder.setFormat(oboe::AudioFormat::I16);
					builder.setSampleRate(mSampleRate);
					builder.setChannelCount(mChannels);
					if (convert)
					{
						builder.setFormatConversionAllowed(true);
						builder.setChannelConversionAllowed(true);
						builder.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
					}
					// The device the settings picked, by name: empty / "cubeb_default" / "Default" let Android
					// choose (the built-in microphone on the handhelds we target); anything else is looked up
					// in the list Java pushed before boot. A saved device that is not plugged in falls back to
					// the default rather than failing the whole mic — it stays listed as "not connected" in the UI.
					s32 device_id = oboe::kUnspecified;
					if (!mDeviceName.empty() && mDeviceName != "cubeb_default" && mDeviceName != "Default")
					{
						const std::lock_guard lock(s_device_list_mutex);
						for (const auto& [name, id] : s_device_list)
							if (name == mDeviceName)
							{
								device_id = id;
								break;
							}
						if (device_id == oboe::kUnspecified)
							Console.Warning("(audiodev_oboe) Device '%s' not in the pushed list (unplugged?); using the default input.", mDeviceName.c_str());
					}
					builder.setDeviceId(device_id);
					// No explicit buffer capacity: let AAudio pick a native-sized one for the performance mode.
					// The negotiated size is read back below and used as mStreamLatency, exactly like cubeb's
					// cubeb_get_min_latency() — forcing a big fixed capacity here would just add latency to every
					// sample before it reaches the ring buffer.
					builder.setDataCallback(this);
					builder.setErrorCallback(this);

					Console.WriteLn("(audiodev_oboe) Opening stream (preset %d, convert=%d)...", static_cast<int>(preset), static_cast<int>(convert));
					oboe::Result result = builder.openStream(m_stream);
					if (result != oboe::Result::OK)
					{
						Console.Error("(audiodev_oboe) openStream() failed: %d", static_cast<int>(result));
						continue;
					}

					// AAudio may negotiate a different rate than requested; the emulated PS2 mic is hardwired to
					// 48 kHz, so anything else would silently resample everything downstream. Fail loudly instead.
					if (m_stream->getSampleRate() != static_cast<int>(mSampleRate))
					{
						Console.Error("(audiodev_oboe) Stream opened at %d Hz, need %u", m_stream->getSampleRate(), mSampleRate);
						m_stream->close();
						m_stream.reset();
						continue;
					}
					if (m_stream->getChannelCount() != static_cast<int>(mChannels))
					{
						Console.Error("(audiodev_oboe) Stream opened with %d channels, need %u", m_stream->getChannelCount(), mChannels);
						m_stream->close();
						m_stream.reset();
						continue;
					}

					// The negotiated buffer size plays the role of cubeb's min-latency query: it is how long one
					// native burst sits inside AAudio before it reaches us, so the ring must hold at least that.
					mStreamLatency = static_cast<u32>(m_stream->getBufferSizeInFrames());

					Console.WriteLn("(audiodev_oboe)[%d] stream opened: %d Hz, ch=%u, bufFrames=%u", m_id, m_stream->getSampleRate(), mChannels, mStreamLatency);
					return true;
				}
			}

			Console.Error("(audiodev_oboe) All open attempts failed.");
			return false;
		}

		bool OboeAudioDevice::StartStream()
		{
			const std::lock_guard<std::recursive_mutex> guard(m_lock);
			if (!m_stream)
				return false;

			m_stop_requested.store(false, std::memory_order_release);

			oboe::Result result = m_stream->requestStart();
			if (result != oboe::Result::OK)
			{
				Console.Error("(audiodev_oboe) requestStart() failed: %d", static_cast<int>(result));
				return false;
			}

			m_playing = true;
			return true;
		}

		bool OboeAudioDevice::Start()
		{
			const std::lock_guard<std::recursive_mutex> guard(m_lock);
			if (m_stream)
				Stop();

			// Resilient open: some devices refuse a low-latency stream at boot and fail requestStart() with
			// ErrorDisconnected — the audio device was reclaimed the instant we tried to start it. Rather than
			// fall straight to permanent silence, retry, and if the fast path keeps failing drop to
			// PerformanceMode::None (the shared slow path) before giving up. Mirrors OboeAudioStream::Initialize().
			static constexpr oboe::PerformanceMode kModes[] = {
				oboe::PerformanceMode::LowLatency,
				oboe::PerformanceMode::None,
			};
			for (const oboe::PerformanceMode mode : kModes)
			{
				m_perf_mode = mode;
				for (int attempt = 0; attempt < 2; attempt++)
				{
					if (!Open())
						break;

					ResetBuffers();

					if (StartStream())
						return true;

					Stop();
					std::this_thread::sleep_for(std::chrono::milliseconds(60));
				}
				Console.Warning("(audiodev_oboe) performance mode %d failed; trying a more compatible mode...", static_cast<int>(mode));
			}

			Console.Error("(audiodev_oboe) All open/start attempts failed.");
			return false;
		}

		void OboeAudioDevice::Stop()
		{
			const std::lock_guard<std::recursive_mutex> guard(m_lock);
			if (!m_stream)
				return;

			Console.WriteLn("(audiodev_oboe)[%d] Stop()", m_id);
			m_stop_requested.store(true, std::memory_order_release);

			oboe::Result result = m_stream->requestStop();
			if (result != oboe::Result::OK)
				Console.Error("(audiodev_oboe) requestStop() failed: %d", static_cast<int>(result));

			m_playing = false;
			m_stream->close();
			m_stream.reset();
		}

		oboe::DataCallbackResult OboeAudioDevice::onAudioReady(oboe::AudioStream* p_audioStream, void* p_audioData, int32_t p_numFrames)
		{
			const size_t bytes = static_cast<size_t>(p_numFrames) * mChannels * sizeof(short);

			std::lock_guard<std::mutex> lk(mMutex);
			if (mAudioDir == AUDIODIR_SOURCE)
			{
				// The microphone signal goes into the ring buffer, optionally boosted by the settings' input
				// gain: the handheld's built-in mic is quieter than the dynamic capsule SingStar games were
				// tuned for. p_audioData must not be modified in place (it faults on some HALs), so a boost
				// scales into mGainScratch with saturation; unity writes straight through.
				const float gain = s_input_gain.load(std::memory_order_acquire);
				if (gain != 1.0f)
				{
					mGainScratch.resize(static_cast<size_t>(p_numFrames) * mChannels);
					const short* src = static_cast<const short*>(p_audioData);
					for (size_t i = 0; i < mGainScratch.size(); i++)
					{
						float v = static_cast<float>(src[i]) * gain;
						if (v > 32767.0f)
							v = 32767.0f;
						else if (v < -32768.0f)
							v = -32768.0f;
						mGainScratch[i] = static_cast<short>(v);
					}
					mBuffer.write(reinterpret_cast<const u8*>(mGainScratch.data()), bytes);
				}
				else
				{
					mBuffer.write(static_cast<const u8*>(p_audioData), bytes);
				}
			}
			else
			{
				const size_t written = mBuffer.read(p_audioData, bytes);
				if (written < bytes)
					std::memset(static_cast<u8*>(p_audioData) + written, 0, bytes - written);
			}

			return oboe::DataCallbackResult::Continue;
		}

		bool OboeAudioDevice::onError(oboe::AudioStream* oboeStream, oboe::Result error)
		{
			Console.Error("(audiodev_oboe) ErrorCB %d", static_cast<int>(error));
			if (error == oboe::Result::ErrorDisconnected && !m_stop_requested.load(std::memory_order_acquire))
			{
				// Held across the whole teardown/rebuild so the CPU thread can't observe (or destroy) a
				// half-open stream partway through. See the m_lock comment.
				const std::lock_guard<std::recursive_mutex> guard(m_lock);
				Console.Error("(audiodev_oboe) Stream disconnected, reopening...");
				Stop();

				if (!Open())
					Console.Error("(audiodev_oboe) Failed to reopen stream after disconnection.");
				else
				{
					ResetBuffers();
					if (!StartStream())
						// Leave the stream open but stopped: a later Start() tears it down and retries with
						// the full ladder. Starting again here would just re-fail on whatever reclaimed the device.
						Console.Error("(audiodev_oboe) Failed to restart stream after disconnection.");
				}
				return true;
			}
			return false;
		}

		uint32_t OboeAudioDevice::GetBuffer(short* buff, uint32_t frames)
		{
			if (!m_stream)
				return 0;

			std::lock_guard<std::mutex> lk(mMutex);
			const size_t read_size = frames * sizeof(buff[0]) * GetChannels();
			const size_t bytes_read = mBuffer.read(buff, read_size);
			return (bytes_read / sizeof(buff[0]) / GetChannels());
		}

		uint32_t OboeAudioDevice::SetBuffer(short* buff, uint32_t frames)
		{
			if (!m_stream)
				return frames;

			std::lock_guard<std::mutex> lk(mMutex);
			size_t nbytes = frames * sizeof(short) * GetChannels();
			mBuffer.write((uint8_t*)buff, nbytes);

			return frames;
		}

		bool OboeAudioDevice::GetFrames(uint32_t* size)
		{
			if (!m_stream)
				return true;

			std::lock_guard<std::mutex> lk(mMutex);
			*size = mBuffer.size() / sizeof(short) / GetChannels();
			return true;
		}

		void OboeAudioDevice::SetResampling(int samplerate)
		{
			const bool was_started = (m_stream != nullptr);

			Console.WriteLn("(audiodev_oboe)[%d] SetResampling(%d), was_started=%d", m_id, samplerate, was_started ? 1 : 0);
			Stop();

			mSampleRate = samplerate;

			if (was_started)
				Start();

			ResetBuffers();
		}

		void OboeAudioDevice::ResetBuffers()
		{
			std::lock_guard<std::mutex> lk(mMutex);
			const u32 samples = std::max(((mSampleRate * mChannels) * mLatency) / 1000u, mStreamLatency * mChannels);
			mBuffer.reset(sizeof(u16) * samples);
			Console.WriteLn("(audiodev_oboe)[%d] ResetBuffers: capacity=%zu", m_id, mBuffer.capacity());
		}
	} // namespace audiodev_oboe
} // namespace usb_mic
