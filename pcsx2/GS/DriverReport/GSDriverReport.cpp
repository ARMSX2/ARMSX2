// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/DriverReport/GSDriverReport.h"
#include "GS/DriverReport/GSDriverReportClassify.h"
#include "GS/DriverReport/GSDriverReportProfile.h"
#include "GS/DriverReport/GSDriverReportSystem.h"

#include "GS/GS.h"
#include "BuildVersion.h"
#include "Config.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/VKLoader.h"
#endif

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace GSDriverReport
{
	/// How long the GS thread waits for the file-system and kernel part. Generous: the first
	/// report of a run hashes the driver and scans the vendor libraries, which is slow on a cold
	/// cache. Past this the report is written without those sections.
	static constexpr std::chrono::milliseconds SYSTEM_PART_DEADLINE{10000};

	std::string SidecarPath(const std::string& dump_base)
	{
		return dump_base + ".driver.json";
	}

	void WriteFeatureSupport(JsonWriter& w, const GSDevice::FeatureSupport& f)
	{
		w.BeginObject();
#define FEATURE(name) w.KeyBool(#name, f.name)
		FEATURE(broken_point_sampler);
		FEATURE(vs_expand);
		FEATURE(primitive_id);
		FEATURE(texture_barrier);
		FEATURE(multidraw_fb_copy);
		FEATURE(cheap_rt_feedback_read);
		FEATURE(fast_stencil_shadow);
		FEATURE(provoking_vertex_last);
		FEATURE(point_expand);
		FEATURE(line_expand);
		FEATURE(prefer_new_textures);
		FEATURE(dxt_textures);
		FEATURE(bptc_textures);
		FEATURE(astc_textures);
		FEATURE(framebuffer_fetch);
		FEATURE(feedback_loop_layout);
		FEATURE(framebuffer_fetch_orders_overlap);
		FEATURE(declared_feedback_loop_orders_overlap);
		FEATURE(declared_loop_overlap_needs_raster_order);
		FEATURE(ordered_read_costs_per_draw);
		FEATURE(barrier_read_costs_per_draw);
		FEATURE(stencil_buffer);
		FEATURE(cas_sharpening);
		FEATURE(test_and_sample_depth);
		FEATURE(no_ps2_z_quantization);
		FEATURE(depth_feedback);
		FEATURE(aa1);
		FEATURE(rov);
		FEATURE(metalfx_spatial);
		FEATURE(fsr1);
		FEATURE(sgsr);
		FEATURE(dual_source_blend);
		FEATURE(broken_mad_deinterlace);
		FEATURE(broken_blend_constant);
#undef FEATURE
		w.KeyBool("feedback_loops", f.feedback_loops());
		w.KeyInt("feedback_carry", static_cast<int64_t>(f.feedback_carry));
		w.EndObject();
	}
} // namespace GSDriverReport

void GSDevice::CollectDriverReport(GSDriverReport::BackendReport& out) const
{
	using namespace GSDriverReport;
	out.api = RenderAPIToString(GetRenderAPI());
	out.steps.Run("backend.driver_info_text", [&](std::string&) {
		out.driver_info_text = GetDriverInfo();
		return true;
	});

	JsonWriter w;
	w.BeginObject();
	w.KeyString("device_name", m_name);
	w.KeyUInt("max_texture_size", m_max_texture_size);
	w.Key("features");
	WriteFeatureSupport(w, m_features);

	// The profile as the device holds it. Backends that keep the whole resolver result (Vulkan)
	// write that instead, with its hints and device rules.
	GpuProfileSelection held;
	held.override_mode = GpuProfileDetector::ParseOverride(GSConfig.AndroidGpuProfileOverride);
	held.runtime_profile = m_runtime_gpu_profile;
	held.gpu = m_mobile_gpu_identity;
	held.gs_tuning = m_mobile_gs_tuning;
	held.driver = m_mobile_driver_profile;
	w.Key("profile");
	WriteGpuProfile(w, held, nullptr);
	w.EndObject();
	out.backend_json = w.TakeString();
}

namespace GSDriverReport
{
	namespace
	{
		/// Everything the helper thread produces. Shared with it so a helper that outlives its
		/// deadline writes into memory that still exists, and nobody reads.
		struct SystemPart
		{
			PackInfo pack;

			std::mutex mutex;
			std::condition_variable cv;
			bool done = false;

			StepLog steps;
			std::string device_json;
			std::string selected_driver_json;
			std::string vendor_libraries_json;
			std::string kernel_json;
			KernelFacts kernel;
		};

		void RunSystemPart(const std::shared_ptr<SystemPart>& part)
		{
			StepLog steps;
			std::string device, selected, libs, kernel;
			KernelFacts kfacts;
			PackInfo pack = part->pack;
			{
				JsonWriter w;
				WriteDeviceFacts(w, steps);
				device = w.TakeString();
			}
			ReadPackMeta(&pack, steps);
			{
				JsonWriter w;
				WriteSelectedDriver(w, steps, pack);
				selected = w.TakeString();
			}
			{
				JsonWriter w;
				WriteKernel(w, steps, &kfacts);
				kernel = w.TakeString();
			}
			{
				JsonWriter w;
				WriteVendorLibraries(w, steps);
				libs = w.TakeString();
			}

			std::lock_guard lock(part->mutex);
			part->pack = std::move(pack);
			part->steps = std::move(steps);
			part->device_json = std::move(device);
			part->selected_driver_json = std::move(selected);
			part->vendor_libraries_json = std::move(libs);
			part->kernel_json = std::move(kernel);
			part->kernel = kfacts;
			part->done = true;
			part->cv.notify_all();
		}

		/// Runs RunSystemPart on a detached thread. Returns false if no thread could be started;
		/// the emulator builds without exceptions, so std::thread would abort instead.
		bool StartDetached(const std::shared_ptr<SystemPart>& part)
		{
#ifdef _WIN32
			std::thread(RunSystemPart, part).detach();
			return true;
#else
			auto* arg = new std::shared_ptr<SystemPart>(part);
			pthread_attr_t attr;
			if (pthread_attr_init(&attr) != 0)
			{
				delete arg;
				return false;
			}
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			pthread_t thread;
			const int rc = pthread_create(
				&thread, &attr,
				[](void* p) -> void* {
					std::unique_ptr<std::shared_ptr<SystemPart>> owned(static_cast<std::shared_ptr<SystemPart>*>(p));
					RunSystemPart(*owned);
					return nullptr;
				},
				arg);
			pthread_attr_destroy(&attr);
			if (rc != 0)
			{
				delete arg;
				return false;
			}
			return true;
#endif
		}

		void WriteSettings(JsonWriter& w)
		{
			// The effective GS settings, per-game overrides and game fixes applied, serialised the way
			// the INI would store them. Saved from a copy: LoadSave normalises a few fields as it goes.
			Pcsx2Config::GSOptions copy = GSConfig;
			MemorySettingsInterface si;
			SettingsSaveWrapper wrap(si);
			copy.LoadSave(wrap);
			w.BeginObject();
			for (const auto& [key, value] : si.GetKeyValueList("EmuCore/GS"))
				w.KeyString(key, value);
			w.EndObject();
		}
	} // namespace

	static std::string BuildReport(const std::string& dump_base)
	{
		const double start = StepLog::NowMs();
		StepLog steps;

		// The selected driver, as the Vulkan loader recorded it.
		PackInfo pack;
		BackendReport backend;
		const bool have_device = static_cast<bool>(g_gs_device);
		if (have_device)
		{
			steps.Run("backend.collect", [&](std::string&) {
				g_gs_device->CollectDriverReport(backend);
				return true;
			});
		}
		else
		{
			steps.Add("backend.collect", false, "no GS device", 0.0);
		}
		const bool is_vulkan = (backend.api == "Vulkan");
#ifdef ENABLE_VULKAN
		if (is_vulkan)
		{
			const Vulkan::CustomDriverStatus status = Vulkan::GetCustomDriverStatus();
			pack.custom_selected = status.requested;
			pack.opened = status.opened;
			pack.required = status.required;
			pack.pack_path = status.dir;
			pack.library_name = status.name;
			pack.redirect_dir = status.redirect_dir;
			pack.hook_lib_dir = status.hook_lib_dir;
			pack.failure = status.failure;
		}
#endif

		// Files and the kernel on a helper thread, under a deadline.
		auto part = std::make_shared<SystemPart>();
		part->pack = pack;
		bool timed_out = false;
		if (StartDetached(part))
		{
			std::unique_lock lock(part->mutex);
			timed_out = !part->cv.wait_for(lock, SYSTEM_PART_DEADLINE, [&part] { return part->done; });
		}
		else
		{
			// Could not start a thread: run it here instead.
			steps.Add("system_part.thread", false, "could not start a helper thread; ran inline", 0.0);
			RunSystemPart(part);
		}

		std::unique_lock lock(part->mutex);
		if (!timed_out)
			pack = part->pack;

		JsonWriter w;
		w.BeginObject();
		w.KeyInt("schema", 1);

		w.Key("app");
		w.BeginObject();
		w.KeyString("name", "ARMSX2");
		w.KeyString("version", BuildVersion::GitRev);
		w.KeyString("commit", BuildVersion::GitHash);
		w.KeyString("tag", BuildVersion::GitTag);
		w.KeyString("date", BuildVersion::GitDate);
		w.EndObject();

		w.Key("game");
		w.BeginObject();
		steps.Run("game", [&](std::string&) {
			w.KeyString("serial", VMManager::GetDiscSerial());
			w.KeyHex("crc", VMManager::GetDiscCRC());
			w.KeyString("title", VMManager::GetTitle(true));
			return true;
		});
		w.KeyString("dump", dump_base);
		w.EndObject();

		if (timed_out || part->device_json.empty())
			w.KeyNull("device");
		else
			w.KeyRaw("device", part->device_json);

		if (timed_out || part->selected_driver_json.empty())
		{
			w.Key("selected_driver");
			w.BeginObject();
			w.KeyString("kind", pack.custom_selected ? "custom" : "system");
			w.KeyString("pack_name", pack.pack_name);
			w.KeyString("pack_path", pack.pack_path);
			w.EndObject();
		}
		else
		{
			w.KeyRaw("selected_driver", part->selected_driver_json);
		}

		// The served-driver verdict.
		w.Key("served_driver");
		w.BeginObject();
		{
			const std::string expected = is_vulkan ?
			                                 ExpectedDriverForPack(pack.custom_selected, pack.pack_name, pack.library_name, pack.meta_description) :
			                                 "system";
			w.KeyString("expected", expected);
			if (backend.has_served_facts)
			{
				ServedDriverFacts facts;
				facts.vendor_id = backend.vendor_id;
				facts.driver_id = backend.driver_id;
				facts.driver_name = backend.driver_name;
				facts.driver_info = backend.driver_info;
				facts.device_name = backend.device_name;
				const ServedDriverClassification c = ClassifyServedDriver(facts);
				const bool match = ServedDriverMatches(expected, c.answered);
				w.KeyString("answered", c.answered);
				w.KeyBool("match", match);
				w.KeyString("evidence", c.evidence);
				w.KeyUInt("axfl_generation", c.axfl_generation);
				if (pack.custom_selected && !pack.opened)
					w.KeyString("note", "the custom driver did not open; the system loader answered: " + pack.failure);
				else if (!match)
					w.KeyString("note", "the driver that answered is not the one selected");
			}
			else
			{
				// OpenGL and the others have no driver identity beyond their strings.
				w.KeyString("answered", backend.api.empty() ? "unknown" : backend.api);
				w.KeyNull("match");
				w.KeyString("evidence", backend.driver_info_text);
				w.KeyUInt("axfl_generation", 0);
			}
		}
		w.EndObject();

		w.Key("vulkan");
		w.BeginObject();
		if (!backend.vulkan_json.empty())
			w.KeyRaw((pack.custom_selected && pack.opened) ? "custom" : "system", backend.vulkan_json);
		w.EndObject();

		w.Key("renderer");
		w.BeginObject();
		w.KeyString("api", backend.api);
		w.KeyString("configured", Pcsx2Config::GSOptions::GetRendererName(GSConfig.Renderer));
		w.KeyBool("hardware", GSIsHardwareRenderer());
		w.KeyDouble("upscale_multiplier", GSConfig.UpscaleMultiplier);
		w.KeyInt("blending_accuracy", static_cast<int64_t>(GSConfig.AccurateBlendingUnit));
		w.KeyString("driver_info_text", backend.driver_info_text);
		if (!backend.backend_json.empty())
			w.KeyRaw("device", backend.backend_json);
		steps.Run("renderer.settings", [&](std::string&) {
			w.Key("settings");
			WriteSettings(w);
			return true;
		});
		w.EndObject();

		if (!backend.gl_json.empty())
			w.KeyRaw("gl", backend.gl_json);

		if (timed_out || part->kernel_json.empty())
			w.KeyNull("kernel");
		else
			w.KeyRaw("kernel", part->kernel_json);

		if (timed_out || part->vendor_libraries_json.empty())
			w.KeyNull("vendor_libraries");
		else
			w.KeyRaw("vendor_libraries", part->vendor_libraries_json);

		// Steps last among the data so every step above is in it.
		StepLog all;
		all.Append(backend.steps);
		all.Append(steps);
		if (timed_out)
			all.Add("system_part", false, "timed out: the file-system and kernel checks did not finish in time", 0.0);
		else
			all.Append(part->steps);
		w.Key("steps");
		all.Write(w);

		w.Key("collector");
		w.BeginObject();
		w.KeyDouble("ms", StepLog::NowMs() - start);
		w.KeyBool("system_part_timed_out", timed_out);
		w.EndObject();

		w.EndObject();
		return w.TakeString();
	}

	void WriteSidecarForDump(const std::string& dump_base)
	{
		const std::string path = SidecarPath(dump_base);
		const std::string json = BuildReport(dump_base);
		if (!FileSystem::WriteStringToFile(path.c_str(), json))
			Console.Warning("GS: could not write the driver report '%s'.", path.c_str());
	}
} // namespace GSDriverReport
