// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// armsx2-driverdiag: the GS dump's driver report, for a device with no game running.
//
// Probes the system Vulkan driver and, with --pack, a custom driver pack loaded through
// libadrenotools exactly as the emulator loads one, and writes the same JSON report the emulator
// writes beside a GS dump (minus the renderer's own decisions, which need a running renderer).
// Each driver is probed in a child process with a deadline, so a driver that crashes or hangs is
// reported as such instead of taking the report with it.
//
//   armsx2-driverdiag [--pack <dir>] [--lib <file.so>] [--hook-dir <dir>] [--redirect-dir <dir>]
//                     [--no-system] [--timeout <seconds>] [--out <file>]
//
// Opt-in build target only; never packaged into an application.

#include "GS/DriverReport/GSDriverReportClassify.h"
#include "GS/DriverReport/GSDriverReportJson.h"
#include "GS/DriverReport/GSDriverReportSystem.h"
#include "GS/DriverReport/GSDriverReportVulkan.h"

#include "BuildVersion.h"

#include "common/Error.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace GSDriverReport;

#if defined(__ANDROID__)
// On Android the app supplies these through JNI (content:// URIs, Java-side directory creation). A
// command-line tool has only plain paths, so the file-system layer takes its POSIX road.
namespace FileSystem
{
	int OpenFDFileContent(const char* filename) { return -1; }
	bool CreateDirectoryViaJava(const char* path) { return false; }
	bool CreateFileViaJava(const char* path) { return false; }
} // namespace FileSystem
#endif

namespace
{
	struct Options
	{
		std::string pack_dir;
		std::string lib;
		std::string hook_dir;
		std::string redirect_dir;
		std::string out;
		bool probe_system = true;
		int timeout_s = 20;
	};

	void Usage()
	{
		std::fprintf(stderr,
			"usage: armsx2-driverdiag [--pack <dir>] [--lib <file.so>] [--hook-dir <dir>] [--redirect-dir <dir>]\n"
			"                         [--no-system] [--timeout <seconds>] [--out <file>]\n"
			"  --pack <dir>        an extracted driver pack (meta.json + the driver .so). Probed through\n"
			"                      libadrenotools as the emulator loads it (Android only), and compared with\n"
			"                      the system driver.\n"
			"  --lib <file.so>     the driver library in the pack; default: meta.json's libraryName.\n"
			"  --hook-dir <dir>    where libhook_impl.so and libmain_hook.so are; default: this binary's dir.\n"
			"  --redirect-dir <dir> adrenotools file-redirect directory (optional).\n"
			"  --no-system         skip the system driver.\n"
			"  --timeout <s>       per-driver deadline; a driver past it is reported as hung (default 20).\n"
			"  --out <file>        write the report here instead of stdout.\n");
	}

	std::string ExeDir()
	{
		char buf[4096];
		const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (n <= 0)
			return ".";
		buf[n] = '\0';
		std::string p(buf);
		const size_t slash = p.find_last_of('/');
		return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
	}

	/// Probes one driver in this (child) process and returns the JSON object for it. Top-level
	/// scalars carry the served driver's identity for the parent.
	std::string ProbeDriver(bool custom, const Options& opt)
	{
		StepLog steps;
		JsonWriter w;
		w.BeginObject();
		w.KeyString("kind", custom ? "custom" : "system");

		bool loaded = steps.Run(custom ? "vulkan.custom.load" : "vulkan.system.load", [&](std::string& err) {
#if defined(ARMSX2_USE_ADRENOTOOLS)
			if (custom)
				Vulkan::SetCustomDriverPath(opt.pack_dir.c_str(), opt.lib.c_str(), opt.redirect_dir.c_str(),
					opt.hook_dir.c_str(), true);
#else
			if (custom)
			{
				err = "this build has no libadrenotools; custom packs load on Android only";
				return false;
			}
#endif
			Error e;
			if (!Vulkan::LoadVulkanLibrary(&e))
			{
				err = e.GetDescription();
				return false;
			}
			return true;
		});
		w.KeyBool("library_loaded", loaded);
		const Vulkan::CustomDriverStatus status = Vulkan::GetCustomDriverStatus();
		if (custom)
		{
			w.KeyBool("custom_opened", status.opened);
			w.KeyString("custom_failure", status.failure);
		}

		std::vector<VulkanDeviceSummary> summaries;
		VkInstance instance = VK_NULL_HANDLE;
		if (loaded)
		{
			const uint32_t loader_version = QueryLoaderInstanceVersion();
			steps.Run(custom ? "vulkan.custom.create_instance" : "vulkan.system.create_instance", [&](std::string& err) {
				VkApplicationInfo app = {};
				app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
				app.pApplicationName = "armsx2-driverdiag";
				app.pEngineName = "ARMSX2";
				app.apiVersion = loader_version >= VK_API_VERSION_1_1 ? loader_version : VK_API_VERSION_1_0;
				std::vector<const char*> exts;
				if (loader_version < VK_API_VERSION_1_1)
					exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
				VkInstanceCreateInfo ci = {};
				ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
				ci.pApplicationInfo = &app;
				ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
				ci.ppEnabledExtensionNames = exts.data();
				const VkResult r = vkCreateInstance(&ci, nullptr, &instance);
				if (r != VK_SUCCESS)
				{
					err = "vkCreateInstance: VkResult " + std::to_string(static_cast<int>(r));
					instance = VK_NULL_HANDLE;
					return false;
				}
				Vulkan::LoadVulkanInstanceFunctions(instance);
				return true;
			});
		}

		if (instance != VK_NULL_HANDLE)
		{
			w.Key("vulkan");
			WriteVulkanInstance(w, steps, custom ? "vulkan.custom" : "vulkan.system", instance, VK_NULL_HANDLE, nullptr,
				&summaries);
		}

		w.KeyUInt("physical_device_count", summaries.size());
		if (!summaries.empty())
		{
			const VulkanDeviceSummary& s = summaries.front();
			w.KeyUInt("served_vendor_id", s.vendor_id);
			w.KeyUInt("served_device_id", s.device_id);
			w.KeyUInt("served_driver_id", s.driver_id);
			w.KeyString("served_driver_name", s.driver_name);
			w.KeyString("served_driver_info", s.driver_info);
			w.KeyString("served_device_name", s.device_name);
		}
		w.Key("steps");
		steps.Write(w);
		w.EndObject();

		// Tear down gently where possible; a driver that crashes here still delivered its report.
		if (instance != VK_NULL_HANDLE)
		{
			if (const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
					vkGetInstanceProcAddr(instance, "vkDestroyInstance")))
				destroy(instance, nullptr);
		}
		return w.TakeString();
	}

	struct ChildResult
	{
		bool ok = false;
		std::string json;
		std::string error;
		double ms = 0.0;
	};

	/// Runs ProbeDriver in a child and collects its JSON through a pipe, under a deadline.
	ChildResult ProbeInChild(bool custom, const Options& opt)
	{
		ChildResult res;
		const double start = StepLog::NowMs();
		int fds[2];
		if (pipe(fds) != 0)
		{
			res.error = std::string("pipe: ") + std::strerror(errno);
			return res;
		}
		std::fflush(nullptr);
		const pid_t pid = fork();
		if (pid < 0)
		{
			res.error = std::string("fork: ") + std::strerror(errno);
			close(fds[0]);
			close(fds[1]);
			return res;
		}
		if (pid == 0)
		{
			close(fds[0]);
			const std::string json = ProbeDriver(custom, opt);
			size_t off = 0;
			while (off < json.size())
			{
				const ssize_t n = write(fds[1], json.data() + off, json.size() - off);
				if (n < 0 && errno == EINTR)
					continue;
				if (n <= 0)
					break;
				off += static_cast<size_t>(n);
			}
			close(fds[1]);
			_exit(0);
		}

		close(fds[1]);
		const double deadline = start + opt.timeout_s * 1000.0;
		bool timed_out = false;
		char buf[65536];
		for (;;)
		{
			const double now = StepLog::NowMs();
			if (now >= deadline)
			{
				timed_out = true;
				break;
			}
			pollfd p = {fds[0], POLLIN, 0};
			const int pr = poll(&p, 1, static_cast<int>(deadline - now));
			if (pr < 0 && errno == EINTR)
				continue;
			if (pr == 0)
			{
				timed_out = true;
				break;
			}
			const ssize_t n = read(fds[0], buf, sizeof(buf));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			res.json.append(buf, static_cast<size_t>(n));
		}
		close(fds[0]);

		if (timed_out)
			kill(pid, SIGKILL);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		{
		}
		res.ms = StepLog::NowMs() - start;

		if (timed_out)
			res.error = "hang: no answer within " + std::to_string(opt.timeout_s) + " s; killed";
		else if (WIFSIGNALED(status))
			res.error = "crashed: signal " + std::to_string(WTERMSIG(status)) + " (" + strsignal(WTERMSIG(status)) + ")";
		else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
			res.error = "exited with status " + std::to_string(WEXITSTATUS(status));

		if (!res.json.empty() && !ParseJsonObjectScalars(res.json, nullptr))
		{
			if (res.error.empty())
				res.error = "the probe's output did not parse";
			res.json.clear();
		}
		res.ok = res.error.empty() && !res.json.empty();
		return res;
	}

	std::string Scalar(const std::vector<std::pair<std::string, std::string>>& scalars, std::string_view key)
	{
		for (const auto& [k, v] : scalars)
		{
			if (k == key)
				return v;
		}
		return {};
	}

	std::string DefaultLibrary(const std::string& dir)
	{
		std::string text;
		if (FILE* f = std::fopen((dir + "/meta.json").c_str(), "rb"))
		{
			char buf[4096];
			size_t n;
			while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
				text.append(buf, n);
			std::fclose(f);
			std::vector<std::pair<std::string, std::string>> scalars;
			if (ParseJsonObjectScalars(text, &scalars))
			{
				const std::string lib = Scalar(scalars, "libraryName");
				if (!lib.empty())
					return lib;
			}
		}
		if (DIR* d = opendir(dir.c_str()))
		{
			std::string found;
			while (const dirent* e = readdir(d))
			{
				const std::string name = e->d_name;
				if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0)
				{
					found = name;
					break;
				}
			}
			closedir(d);
			return found;
		}
		return {};
	}
} // namespace

int main(int argc, char* argv[])
{
	Options opt;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		const auto next = [&](std::string* v) {
			if (i + 1 >= argc)
			{
				std::fprintf(stderr, "%s needs a value\n", a.c_str());
				std::exit(2);
			}
			*v = argv[++i];
		};
		if (a == "--pack")
			next(&opt.pack_dir);
		else if (a == "--lib")
			next(&opt.lib);
		else if (a == "--hook-dir")
			next(&opt.hook_dir);
		else if (a == "--redirect-dir")
			next(&opt.redirect_dir);
		else if (a == "--out")
			next(&opt.out);
		else if (a == "--no-system")
			opt.probe_system = false;
		else if (a == "--timeout")
		{
			std::string v;
			next(&v);
			opt.timeout_s = std::max(1, std::atoi(v.c_str()));
		}
		else if (a == "-h" || a == "--help")
		{
			Usage();
			return 0;
		}
		else
		{
			std::fprintf(stderr, "unknown argument '%s'\n", a.c_str());
			Usage();
			return 2;
		}
	}

	const bool custom = !opt.pack_dir.empty();
	if (custom)
	{
		while (opt.pack_dir.size() > 1 && opt.pack_dir.back() == '/')
			opt.pack_dir.pop_back();
		if (opt.lib.empty())
			opt.lib = DefaultLibrary(opt.pack_dir);
		if (opt.hook_dir.empty())
			opt.hook_dir = ExeDir();
	}

	const double start = StepLog::NowMs();
	StepLog steps;

	ChildResult system_probe, custom_probe;
	if (opt.probe_system)
	{
		system_probe = ProbeInChild(false, opt);
		steps.Add("probe.system", system_probe.ok, system_probe.error, system_probe.ms);
	}
	if (custom)
	{
		custom_probe = ProbeInChild(true, opt);
		steps.Add("probe.custom", custom_probe.ok, custom_probe.error, custom_probe.ms);
	}

	std::vector<std::pair<std::string, std::string>> sys_scalars, custom_scalars;
	if (!system_probe.json.empty())
		ParseJsonObjectScalars(system_probe.json, &sys_scalars);
	if (!custom_probe.json.empty())
		ParseJsonObjectScalars(custom_probe.json, &custom_scalars);

	PackInfo pack;
	pack.custom_selected = custom;
	pack.pack_path = opt.pack_dir;
	pack.library_name = opt.lib;
	pack.hook_lib_dir = opt.hook_dir;
	pack.redirect_dir = opt.redirect_dir;
	pack.opened = Scalar(custom_scalars, "custom_opened") == "true";
	pack.failure = Scalar(custom_scalars, "custom_failure");
	pack.required = true;
	ReadPackMeta(&pack, steps);

	JsonWriter w;
	w.BeginObject();
	w.KeyInt("schema", 1);
	w.KeyString("tool", "armsx2-driverdiag");

	w.Key("app");
	w.BeginObject();
	w.KeyString("name", "ARMSX2");
	w.KeyString("version", BuildVersion::GitRev);
	w.KeyString("commit", BuildVersion::GitHash);
	w.KeyString("tag", BuildVersion::GitTag);
	w.KeyString("date", BuildVersion::GitDate);
	w.EndObject();

	w.Key("device");
	WriteDeviceFacts(w, steps);

	w.Key("selected_driver");
	WriteSelectedDriver(w, steps, pack);

	// The kernel before the verdict: an Adreno KGSL node with no Vulkan device on the pack means
	// the pack does not support this GPU.
	std::string kernel_json;
	KernelFacts kfacts;
	{
		JsonWriter kw;
		WriteKernel(kw, steps, &kfacts);
		kernel_json = kw.TakeString();
	}

	w.Key("served_driver");
	w.BeginObject();
	{
		const std::string expected =
			ExpectedDriverForPack(custom, pack.pack_name, pack.library_name, pack.meta_description);
		const auto& scalars = custom ? custom_scalars : sys_scalars;
		const ChildResult& probe = custom ? custom_probe : system_probe;
		w.KeyString("expected", expected);
		const std::string count = Scalar(scalars, "physical_device_count");
		if (!count.empty() && count != "0")
		{
			ServedDriverFacts facts;
			const std::string vendor = Scalar(scalars, "served_vendor_id");
			const std::string driver_id = Scalar(scalars, "served_driver_id");
			const std::string name = Scalar(scalars, "served_driver_name");
			const std::string info = Scalar(scalars, "served_driver_info");
			const std::string dev = Scalar(scalars, "served_device_name");
			facts.vendor_id = static_cast<uint32_t>(std::strtoul(vendor.c_str(), nullptr, 10));
			facts.driver_id = static_cast<uint32_t>(std::strtoul(driver_id.c_str(), nullptr, 10));
			facts.driver_name = name;
			facts.driver_info = info;
			facts.device_name = dev;
			const ServedDriverClassification c = ClassifyServedDriver(facts);
			const bool match = ServedDriverMatches(expected, c.answered);
			w.KeyString("answered", c.answered);
			w.KeyBool("match", match);
			w.KeyString("evidence", c.evidence);
			w.KeyUInt("axfl_generation", c.axfl_generation);
			if (custom && !pack.opened)
				w.KeyString("note", "the pack did not open through libadrenotools: " + pack.failure);
			else if (!match)
				w.KeyString("note", "the driver that answered is not the one selected");
		}
		else
		{
			w.KeyString("answered", "none");
			w.KeyBool("match", false);
			std::string evidence;
			if (!probe.error.empty())
				evidence = "probe " + probe.error;
			else if (custom && !pack.opened)
				evidence = "the pack did not open through libadrenotools: " + pack.failure;
			else
				evidence = "the driver loaded and enumerated no physical device";
			w.KeyString("evidence", evidence);
			if (custom && pack.opened && kfacts.kgsl_present && (kfacts.kgsl_chip_id != 0 || !kfacts.kgsl_gpu_model.empty()))
			{
				char chip[16];
				std::snprintf(chip, sizeof(chip), "0x%08x", kfacts.kgsl_chip_id);
				w.KeyString("note", "unsupported by this Turnip pack: KGSL reports " +
										(kfacts.kgsl_gpu_model.empty() ? std::string("an Adreno") : kfacts.kgsl_gpu_model) +
										" (chip " + chip + ") and the pack found no Vulkan device on it");
			}
			w.KeyUInt("axfl_generation", 0);
		}

		if (custom && opt.probe_system && !sys_scalars.empty())
		{
			w.Key("system_driver");
			w.BeginObject();
			ServedDriverFacts sys;
			const std::string vendor = Scalar(sys_scalars, "served_vendor_id");
			const std::string driver_id = Scalar(sys_scalars, "served_driver_id");
			const std::string name = Scalar(sys_scalars, "served_driver_name");
			const std::string info = Scalar(sys_scalars, "served_driver_info");
			const std::string dev = Scalar(sys_scalars, "served_device_name");
			sys.vendor_id = static_cast<uint32_t>(std::strtoul(vendor.c_str(), nullptr, 10));
			sys.driver_id = static_cast<uint32_t>(std::strtoul(driver_id.c_str(), nullptr, 10));
			sys.driver_name = name;
			sys.driver_info = info;
			sys.device_name = dev;
			const ServedDriverClassification c = ClassifyServedDriver(sys);
			w.KeyString("answered", c.answered);
			w.KeyString("evidence", c.evidence);
			// A pack that answers with the same identity as the system driver is the silent fallback.
			w.KeyBool("custom_answer_identical_to_system",
				!custom_scalars.empty() && Scalar(custom_scalars, "served_driver_info") == info &&
					Scalar(custom_scalars, "served_driver_name") == name);
			w.EndObject();
		}
	}
	w.EndObject();

	w.Key("vulkan");
	w.BeginObject();
	if (!system_probe.json.empty())
		w.KeyRaw("system", system_probe.json);
	if (!custom_probe.json.empty())
		w.KeyRaw("custom", custom_probe.json);
	w.EndObject();

	w.KeyRaw("kernel", kernel_json);

	w.Key("vendor_libraries");
	WriteVendorLibraries(w, steps);

	w.Key("steps");
	steps.Write(w);

	w.Key("collector");
	w.BeginObject();
	w.KeyDouble("ms", StepLog::NowMs() - start);
	w.EndObject();
	w.EndObject();

	std::string json = w.TakeString();
	json.push_back('\n');
	if (opt.out.empty())
	{
		std::fwrite(json.data(), 1, json.size(), stdout);
	}
	else
	{
		FILE* f = std::fopen(opt.out.c_str(), "wb");
		if (!f)
		{
			std::fprintf(stderr, "cannot write %s: %s\n", opt.out.c_str(), std::strerror(errno));
			return 1;
		}
		std::fwrite(json.data(), 1, json.size(), f);
		std::fclose(f);
		std::fprintf(stderr, "wrote %s\n", opt.out.c_str());
	}
	return 0;
}
