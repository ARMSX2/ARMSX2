// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/DriverReport/GSDriverReportSystem.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#define DRIVER_REPORT_LINUX 1
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "Sha256.h"

extern "C" int mali_kbase_probe(char* json, size_t cap);

namespace GSDriverReport
{
	static std::string ErrnoString(int err)
	{
		return std::string(std::strerror(err)) + " (errno " + std::to_string(err) + ")";
	}

	static std::string Trim(std::string s)
	{
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\0'))
			s.pop_back();
		return s;
	}

#ifdef DRIVER_REPORT_LINUX
	/// Reads at most `max` bytes. /proc and /sys files report size 0, so read until EOF.
	static bool ReadFileBytes(const char* path, size_t max, std::string* out, std::string* err)
	{
		const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0)
		{
			if (err)
				*err = ErrnoString(errno);
			return false;
		}
		out->clear();
		char buf[4096];
		while (out->size() < max)
		{
			const ssize_t n = read(fd, buf, std::min(sizeof(buf), max - out->size()));
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				if (err)
					*err = ErrnoString(errno);
				close(fd);
				return false;
			}
			if (n == 0)
				break;
			out->append(buf, static_cast<size_t>(n));
		}
		close(fd);
		return true;
	}

	static bool PathExists(const char* path)
	{
		struct stat st;
		return stat(path, &st) == 0;
	}

	static std::vector<std::string> ListDir(const char* dir)
	{
		std::vector<std::string> names;
		if (DIR* d = opendir(dir))
		{
			while (const dirent* e = readdir(d))
			{
				if (e->d_name[0] != '.')
					names.emplace_back(e->d_name);
			}
			closedir(d);
		}
		std::sort(names.begin(), names.end());
		return names;
	}
#endif

#if defined(__ANDROID__)
	static std::string GetProp(const char* name)
	{
		std::string value;
#if __ANDROID_API__ >= 26
		if (const prop_info* pi = __system_property_find(name))
		{
			__system_property_read_callback(
				pi,
				[](void* cookie, const char*, const char* v, uint32_t) {
					*static_cast<std::string*>(cookie) = v ? v : "";
				},
				&value);
		}
#else
		char buf[PROP_VALUE_MAX] = {};
		if (__system_property_get(name, buf) > 0)
			value = buf;
#endif
		return value;
	}
#endif

	void WriteDeviceFacts(JsonWriter& w, StepLog& steps)
	{
		w.BeginObject();

#if defined(__ANDROID__)
		steps.Run("device.properties", [&](std::string&) {
			w.KeyString("model", GetProp("ro.product.model"));
			w.KeyString("manufacturer", GetProp("ro.product.manufacturer"));
			w.KeyString("board", GetProp("ro.product.board"));
			w.KeyString("platform", GetProp("ro.board.platform"));
			w.KeyString("soc_model", GetProp("ro.soc.model"));
			const std::string sdk = GetProp("ro.build.version.sdk");
			w.KeyInt("sdk", std::atoi(sdk.c_str()));

			static const char* const props[] = {
				"ro.product.brand",
				"ro.product.device",
				"ro.product.name",
				"ro.soc.manufacturer",
				"ro.hardware",
				"ro.hardware.vulkan",
				"ro.hardware.egl",
				"ro.hardware.gralloc",
				"ro.gfx.driver.0",
				"ro.gfx.driver.1",
				"ro.gfx.angle.supported_apps",
				"ro.opengles.version",
				"ro.product.cpu.abilist",
				"ro.build.fingerprint",
				"ro.build.id",
				"ro.build.display.id",
				"ro.build.type",
				"ro.build.tags",
				"ro.build.version.release",
				"ro.build.version.incremental",
				"ro.build.version.security_patch",
				"ro.build.date",
				"ro.vendor.build.fingerprint",
				"ro.vendor.build.security_patch",
				"ro.vendor.build.date",
				"ro.vendor.api_level",
				"ro.vndk.version",
				"ro.board.first_api_level",
				"ro.product.first_api_level",
				"ro.bootimage.build.fingerprint",
				"ro.kernel.version",
				"persist.graphics.vulkan.disable",
				"debug.hwui.renderer",
				"debug.vulkan.layers",
			};
			w.Key("properties");
			w.BeginObject();
			for (const char* p : props)
				w.KeyString(p, GetProp(p));
			w.EndObject();
			return true;
		});
#else
		w.KeyNull("model");
		w.KeyNull("manufacturer");
		w.KeyNull("board");
		w.KeyNull("platform");
		w.KeyNull("soc_model");
		w.KeyNull("sdk");
#endif

#ifdef DRIVER_REPORT_LINUX
		steps.Run("device.uname", [&](std::string& err) {
			struct utsname u;
			if (uname(&u) != 0)
			{
				err = ErrnoString(errno);
				w.KeyNull("kernel");
				return false;
			}
			w.KeyString("kernel", std::string(u.sysname) + " " + u.release + " " + u.version + " " + u.machine);
			w.Key("uname");
			w.BeginObject();
			w.KeyString("sysname", u.sysname);
			w.KeyString("nodename", u.nodename);
			w.KeyString("release", u.release);
			w.KeyString("version", u.version);
			w.KeyString("machine", u.machine);
			w.EndObject();
			return true;
		});
		w.KeyInt("page_size", sysconf(_SC_PAGESIZE));

		steps.Run("device.proc_version", [&](std::string& err) {
			std::string text;
			if (!ReadFileBytes("/proc/version", 4096, &text, &err))
			{
				w.KeyNull("proc_version");
				return false;
			}
			w.KeyString("proc_version", Trim(text));
			return true;
		});

#if !defined(__ANDROID__)
		// Linux handhelds: the device tree names the board, os-release names the distribution.
		steps.Run("device.devicetree", [&](std::string& err) {
			std::string model, compatible;
			const bool have_model = ReadFileBytes("/proc/device-tree/model", 1024, &model, &err);
			ReadFileBytes("/proc/device-tree/compatible", 4096, &compatible, nullptr);
			std::replace(compatible.begin(), compatible.end(), '\0', ' ');
			w.KeyString("devicetree_model", Trim(model));
			w.KeyString("devicetree_compatible", Trim(compatible));
			return have_model;
		});
		steps.Run("device.os_release", [&](std::string& err) {
			std::string text;
			if (!ReadFileBytes("/etc/os-release", 16384, &text, &err))
				return false;
			w.KeyString("os_release", text);
			return true;
		});
#endif

		steps.Run("device.cpus", [&](std::string&) {
			w.Key("cpus");
			w.BeginArray();
			for (int cpu = 0; cpu < 64; cpu++)
			{
				char path[160];
				std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);
				if (!PathExists(path))
					break;
				w.BeginObject();
				w.KeyInt("index", cpu);
				std::string v;
				std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", cpu);
				if (ReadFileBytes(path, 64, &v, nullptr))
					w.KeyString("midr", Trim(v));
				std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
				if (ReadFileBytes(path, 64, &v, nullptr))
					w.KeyInt("max_khz", std::atoll(v.c_str()));
				w.EndObject();
			}
			w.EndArray();
			return true;
		});
#else
		w.KeyNull("kernel");
		w.KeyNull("page_size");
#endif

		w.EndObject();
	}

	// ---------------------------------------------------------------------------------------------
	// Vendor libraries

#ifdef DRIVER_REPORT_LINUX
	namespace
	{
		std::vector<std::string> VendorLibraryPaths()
		{
			std::vector<std::string> paths;
			const auto add_matching = [&paths](const char* dir, std::string_view prefix, std::string_view suffix) {
				for (const std::string& name : ListDir(dir))
				{
					if (name.compare(0, prefix.size(), prefix) == 0 && name.size() > suffix.size() &&
						name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
						paths.push_back(std::string(dir) + "/" + name);
				}
			};
#if defined(__ANDROID__)
			add_matching("/vendor/lib64/hw", "vulkan.", ".so");
			add_matching("/vendor/lib64/egl", "lib", ".so");
			for (const char* p : {"/vendor/lib64/libGLES_mali.so", "/vendor/lib64/libllvm-glnext.so",
					 "/vendor/lib64/libllvm-qgl.so", "/vendor/lib64/libgsl.so", "/system/lib64/libvulkan.so"})
			{
				if (PathExists(p))
					paths.emplace_back(p);
			}
#else
			// Desktop and handheld Linux: the Vulkan ICD manifests name the driver libraries.
			for (const char* dir : {"/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d"})
				add_matching(dir, "", ".json");
#endif
			return paths;
		}
	} // namespace
#endif

	void WriteVendorLibraries(JsonWriter& w, StepLog& steps)
	{
		// Identity only: path, size and time. The libraries' embedded strings were tried and gave
		// noise; the exact vendor release is in the served driver's own driverInfo.
		w.BeginObject();
#if defined(__ANDROID__)
		w.Key("properties");
		w.BeginObject();
		for (const char* p : {"ro.hardware.vulkan", "ro.hardware.egl", "ro.hardware.gralloc", "ro.hardware",
				 "ro.gfx.driver.0", "ro.gfx.driver.1"})
			w.KeyString(p, GetProp(p));
		w.EndObject();
#endif
#ifdef DRIVER_REPORT_LINUX
		steps.Run("vendor_libraries", [&](std::string&) {
			w.Key("files");
			w.BeginArray();
			for (const std::string& path : VendorLibraryPaths())
			{
				w.BeginObject();
				w.KeyString("path", path);
				struct stat st;
				if (stat(path.c_str(), &st) == 0)
				{
					w.KeyInt("size", static_cast<int64_t>(st.st_size));
					w.KeyInt("mtime", static_cast<int64_t>(st.st_mtime));
				}
				else
				{
					w.KeyString("error", ErrnoString(errno));
				}
				w.EndObject();
			}
			w.EndArray();
			return true;
		});
#else
		(void)steps;
#endif
		w.EndObject();
	}

	// ---------------------------------------------------------------------------------------------
	// The selected pack

	void ReadPackMeta(PackInfo* pack, StepLog& steps)
	{
		if (!pack->custom_selected || pack->pack_path.empty())
			return;

		// The directory's own name is the fallback name: the app keys packs by directory.
		{
			std::string dir = pack->pack_path;
			while (!dir.empty() && dir.back() == '/')
				dir.pop_back();
			const size_t slash = dir.find_last_of('/');
			pack->pack_name = (slash == std::string::npos) ? dir : dir.substr(slash + 1);
		}

#ifdef DRIVER_REPORT_LINUX
		steps.Run("pack.meta_json", [&](std::string& err) {
			std::string path = pack->pack_path;
			if (!path.empty() && path.back() != '/')
				path.push_back('/');
			path += "meta.json";
			std::string text;
			if (!ReadFileBytes(path.c_str(), 64 * 1024, &text, &err))
			{
				pack->meta_error = path + ": " + err;
				return false;
			}
			std::vector<std::pair<std::string, std::string>> scalars;
			if (!ParseJsonObjectScalars(text, &scalars))
			{
				err = "meta.json does not parse as a JSON object";
				pack->meta_error = err;
				return false;
			}
			pack->meta_json = Trim(text);
			for (const auto& [k, v] : scalars)
			{
				if (k == "name" && !v.empty())
					pack->pack_name = v;
				else if (k == "description")
					pack->meta_description = v;
				else if (k == "vendor")
					pack->meta_vendor = v;
				else if (k == "driverVersion")
					pack->meta_driver_version = v;
			}
			return true;
		});
#endif
	}

#ifdef DRIVER_REPORT_LINUX
	static bool Sha256File(const std::string& path, std::string* hex, int64_t* size, std::string* err)
	{
		const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0)
		{
			*err = ErrnoString(errno);
			return false;
		}
		CSha256 sha;
		Sha256_Init(&sha);
		std::vector<unsigned char> buf(1 << 20);
		int64_t total = 0;
		for (;;)
		{
			const ssize_t n = read(fd, buf.data(), buf.size());
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0)
			{
				*err = ErrnoString(errno);
				close(fd);
				return false;
			}
			if (n == 0)
				break;
			Sha256_Update(&sha, buf.data(), static_cast<size_t>(n));
			total += n;
		}
		close(fd);
		unsigned char digest[SHA256_DIGEST_SIZE];
		Sha256_Final(&sha, digest);
		static constexpr char digits[] = "0123456789abcdef";
		hex->clear();
		for (unsigned char b : digest)
		{
			hex->push_back(digits[b >> 4]);
			hex->push_back(digits[b & 15]);
		}
		*size = total;
		return true;
	}
#endif

	void WriteSelectedDriver(JsonWriter& w, StepLog& steps, const PackInfo& pack)
	{
		w.BeginObject();
		w.KeyString("kind", pack.custom_selected ? "custom" : "system");
		if (!pack.custom_selected)
		{
			w.KeyNull("pack_name");
			w.KeyNull("pack_path");
			w.EndObject();
			return;
		}

		w.KeyString("pack_name", pack.pack_name);
		w.KeyString("pack_path", pack.pack_path);
		w.KeyString("library_name", pack.library_name);
		w.KeyBool("opened", pack.opened);
		w.KeyBool("required", pack.required);
		if (!pack.failure.empty())
			w.KeyString("open_failure", pack.failure);
		w.KeyString("redirect_dir", pack.redirect_dir);
		if (!pack.meta_json.empty())
			w.KeyRaw("meta", pack.meta_json);
		else
			w.KeyString("meta_error", pack.meta_error);

#ifdef DRIVER_REPORT_LINUX
		// Hashing a driver of tens of MB takes a while, so remember the answer per file identity.
		static std::mutex s_mutex;
		static std::map<std::string, std::pair<std::string, int64_t>> s_hash_cache;

		std::string path = pack.pack_path;
		if (!path.empty() && path.back() != '/')
			path.push_back('/');
		path += pack.library_name;
		steps.Run("pack.driver_sha256", [&](std::string& err) {
			struct stat st;
			if (stat(path.c_str(), &st) != 0)
			{
				err = path + ": " + ErrnoString(errno);
				return false;
			}
			const std::string key = path + "|" + std::to_string(st.st_size) + "|" + std::to_string(st.st_mtime);
			std::string hex;
			int64_t size = 0;
			bool cached = false;
			{
				std::lock_guard lock(s_mutex);
				if (auto it = s_hash_cache.find(key); it != s_hash_cache.end())
				{
					hex = it->second.first;
					size = it->second.second;
					cached = true;
				}
			}
			if (!cached)
			{
				if (!Sha256File(path, &hex, &size, &err))
					return false;
				std::lock_guard lock(s_mutex);
				s_hash_cache[key] = {hex, size};
			}
			w.Key("driver_file");
			w.BeginObject();
			w.KeyString("path", path);
			w.KeyInt("size", size);
			w.KeyInt("mtime", static_cast<int64_t>(st.st_mtime));
			w.KeyString("sha256", hex);
			w.EndObject();
			return true;
		});

		steps.Run("pack.adrenotools_hooks", [&](std::string& err) {
			w.Key("adrenotools");
			w.BeginObject();
			w.KeyString("source", "vendored bylaws/libadrenotools (no release tag)");
			w.KeyString("hook_lib_dir", pack.hook_lib_dir);
			w.Key("hooks");
			w.BeginArray();
			bool all = true;
			for (const char* hook : {"libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so"})
			{
				std::string p = pack.hook_lib_dir;
				if (!p.empty() && p.back() != '/')
					p.push_back('/');
				p += hook;
				struct stat st;
				const bool exists = stat(p.c_str(), &st) == 0;
				w.BeginObject();
				w.KeyString("name", hook);
				w.KeyBool("present", exists);
				if (exists)
					w.KeyInt("size", static_cast<int64_t>(st.st_size));
				w.EndObject();
				// The first two are the ones the loader checks for.
				if (!exists && (std::strcmp(hook, "libhook_impl.so") == 0 || std::strcmp(hook, "libmain_hook.so") == 0))
				{
					all = false;
					err += std::string(hook) + " missing; ";
				}
			}
			w.EndArray();
			w.KeyBool("driver_opened", pack.opened);
			w.EndObject();
			return all;
		});
#endif
		w.EndObject();
	}

	// ---------------------------------------------------------------------------------------------
	// Kernel

#ifdef DRIVER_REPORT_LINUX
	namespace kgsl
	{
		// A minimal subset of the public msm_kgsl.h UAPI.
		constexpr unsigned IOC_TYPE = 0x09;

		struct device_getproperty
		{
			unsigned int type;
			void* value;
			size_t sizebytes;
		};

		constexpr unsigned long IOCTL_GETPROPERTY = _IOWR(IOC_TYPE, 0x2, struct device_getproperty);

		constexpr unsigned int PROP_DEVICE_INFO = 0x1;
		constexpr unsigned int PROP_VERSION = 0x8;
		constexpr unsigned int PROP_UCHE_GMEM_VADDR = 0x13;
		constexpr unsigned int PROP_UCODE_VERSION = 0x15;
		constexpr unsigned int PROP_GPMU_VERSION = 0x16;
		constexpr unsigned int PROP_HIGHEST_BANK_BIT = 0x17;
		constexpr unsigned int PROP_DEVICE_BITNESS = 0x18;
		constexpr unsigned int PROP_MIN_ACCESS_LENGTH = 0x1A;
		constexpr unsigned int PROP_UBWC_MODE = 0x1B;
		constexpr unsigned int PROP_SECURE_CTXT_SUPPORT = 0x24;
		constexpr unsigned int PROP_SPEED_BIN = 0x25;
		constexpr unsigned int PROP_GAMING_BIN = 0x26;
		constexpr unsigned int PROP_GPU_MODEL = 0x29;
		constexpr unsigned int PROP_VK_DEVICE_ID = 0x2A;
		constexpr unsigned int PROP_GPU_VA64_SIZE = 0x2C;

		struct devinfo
		{
			unsigned int device_id;
			unsigned int chip_id;
			unsigned int mmu_enabled;
			unsigned long gmem_gpubaseaddr;
			unsigned int gpu_id;
			size_t gmem_sizebytes;
		};

		struct version
		{
			unsigned int drv_major;
			unsigned int drv_minor;
			unsigned int dev_major;
			unsigned int dev_minor;
		};

		struct ucode_version
		{
			unsigned int pfp;
			unsigned int pm4;
		};

		struct gpmu_version
		{
			unsigned int major;
			unsigned int minor;
			unsigned int features;
		};

		struct gpu_model
		{
			char gpu_model[32];
		};

		bool GetProperty(int fd, unsigned int type, void* value, size_t size, std::string* err)
		{
			device_getproperty gp = {};
			gp.type = type;
			gp.value = value;
			gp.sizebytes = size;
			if (ioctl(fd, IOCTL_GETPROPERTY, &gp) != 0)
			{
				*err = ErrnoString(errno);
				return false;
			}
			return true;
		}
	} // namespace kgsl

	static void WriteKgsl(JsonWriter& w, StepLog& steps, KernelFacts* facts)
	{
		static const char* const node = "/dev/kgsl-3d0";
		w.BeginObject();
		w.KeyString("node", node);

		int fd = -1;
		const bool opened = steps.Run("kernel.kgsl.open", [&](std::string& err) {
			fd = open(node, O_RDWR | O_CLOEXEC);
			if (fd < 0)
			{
				err = ErrnoString(errno);
				return false;
			}
			return true;
		});
		w.KeyBool("opened", opened);
		if (facts)
			facts->kgsl_opened = opened;

		if (opened)
		{
			steps.Run("kernel.kgsl.device_info", [&](std::string& err) {
				kgsl::devinfo info = {};
				if (!kgsl::GetProperty(fd, kgsl::PROP_DEVICE_INFO, &info, sizeof(info), &err))
					return false;
				w.Key("device_info");
				w.BeginObject();
				w.KeyUInt("device_id", info.device_id);
				w.KeyHex("chip_id", info.chip_id);
				w.KeyUInt("gpu_id", info.gpu_id);
				w.KeyUInt("mmu_enabled", info.mmu_enabled);
				w.KeyHex("gmem_gpubaseaddr", info.gmem_gpubaseaddr);
				w.KeyUInt("gmem_sizebytes", info.gmem_sizebytes);
				w.KeyDouble("gmem_kib", static_cast<double>(info.gmem_sizebytes) / 1024.0);
				w.EndObject();
				if (facts)
				{
					facts->kgsl_chip_id = info.chip_id;
					facts->kgsl_gpu_id = info.gpu_id;
				}
				return true;
			});
			steps.Run("kernel.kgsl.version", [&](std::string& err) {
				kgsl::version v = {};
				if (!kgsl::GetProperty(fd, kgsl::PROP_VERSION, &v, sizeof(v), &err))
					return false;
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%u.%u", v.drv_major, v.drv_minor);
				w.KeyString("driver_version", buf);
				std::snprintf(buf, sizeof(buf), "%u.%u", v.dev_major, v.dev_minor);
				w.KeyString("device_version", buf);
				return true;
			});
			steps.Run("kernel.kgsl.gpu_model", [&](std::string& err) {
				kgsl::gpu_model m = {};
				if (!kgsl::GetProperty(fd, kgsl::PROP_GPU_MODEL, &m, sizeof(m), &err))
					return false;
				const std::string model(m.gpu_model, strnlen(m.gpu_model, sizeof(m.gpu_model)));
				w.KeyString("gpu_model", model);
				if (facts)
					facts->kgsl_gpu_model = model;
				return true;
			});

			const auto u32_prop = [&](const char* step, const char* key, unsigned int type, bool hex) {
				steps.Run(std::string("kernel.kgsl.") + step, [&](std::string& err) {
					unsigned int v = 0;
					if (!kgsl::GetProperty(fd, type, &v, sizeof(v), &err))
						return false;
					if (hex)
						w.KeyHex(key, v);
					else
						w.KeyUInt(key, v);
					return true;
				});
			};
			const auto u64_prop = [&](const char* step, const char* key, unsigned int type) {
				steps.Run(std::string("kernel.kgsl.") + step, [&](std::string& err) {
					uint64_t v = 0;
					if (!kgsl::GetProperty(fd, type, &v, sizeof(v), &err))
						return false;
					w.KeyHex(key, v);
					return true;
				});
			};
			u64_prop("uche_gmem_vaddr", "uche_gmem_vaddr", kgsl::PROP_UCHE_GMEM_VADDR);
			u32_prop("highest_bank_bit", "highest_bank_bit", kgsl::PROP_HIGHEST_BANK_BIT, false);
			u32_prop("ubwc_mode", "ubwc_mode", kgsl::PROP_UBWC_MODE, false);
			u32_prop("min_access_length", "min_access_length", kgsl::PROP_MIN_ACCESS_LENGTH, false);
			u32_prop("device_bitness", "device_bitness", kgsl::PROP_DEVICE_BITNESS, false);
			u32_prop("speed_bin", "speed_bin", kgsl::PROP_SPEED_BIN, true);
			u32_prop("gaming_bin", "gaming_bin", kgsl::PROP_GAMING_BIN, true);
			u32_prop("vk_device_id", "vk_device_id", kgsl::PROP_VK_DEVICE_ID, true);
			u32_prop("secure_ctxt_support", "secure_ctxt_support", kgsl::PROP_SECURE_CTXT_SUPPORT, false);
			u64_prop("gpu_va64_size", "gpu_va64_size", kgsl::PROP_GPU_VA64_SIZE);
			steps.Run("kernel.kgsl.ucode_version", [&](std::string& err) {
				kgsl::ucode_version v = {};
				if (!kgsl::GetProperty(fd, kgsl::PROP_UCODE_VERSION, &v, sizeof(v), &err))
					return false;
				w.Key("ucode_version");
				w.BeginObject();
				w.KeyHex("pfp", v.pfp);
				w.KeyHex("pm4", v.pm4);
				w.EndObject();
				return true;
			});
			steps.Run("kernel.kgsl.gpmu_version", [&](std::string& err) {
				kgsl::gpmu_version v = {};
				if (!kgsl::GetProperty(fd, kgsl::PROP_GPMU_VERSION, &v, sizeof(v), &err))
					return false;
				w.Key("gpmu_version");
				w.BeginObject();
				w.KeyUInt("major", v.major);
				w.KeyUInt("minor", v.minor);
				w.KeyHex("features", v.features);
				w.EndObject();
				return true;
			});
			close(fd);
		}

		// sysfs, where SELinux lets us read it.
		steps.Run("kernel.kgsl.sysfs", [&](std::string&) {
			w.Key("sysfs");
			w.BeginObject();
			for (const char* f : {"gpu_model", "max_gpuclk", "gpuclk", "devfreq/governor", "gpu_available_frequencies"})
			{
				std::string path = std::string("/sys/class/kgsl/kgsl-3d0/") + f;
				std::string v, e;
				if (ReadFileBytes(path.c_str(), 1024, &v, &e))
					w.KeyString(f, Trim(v));
				else
					w.KeyString(f, "unreadable: " + e);
			}
			w.EndObject();
			return true;
		});

		w.EndObject();
	}

	static void WriteMali(JsonWriter& w, StepLog& steps)
	{
		// The probe is not reentrant.
		static std::mutex s_mutex;
		std::vector<char> buf(16 * 1024);
		int rc = 0;
		steps.Run("kernel.mali.probe", [&](std::string& err) {
			std::lock_guard lock(s_mutex);
			rc = mali_kbase_probe(buf.data(), buf.size());
			if (rc != 0)
			{
				err = "mali_kbase_probe returned " + std::to_string(rc);
				return false;
			}
			return true;
		});
		const std::string json(buf.data(), strnlen(buf.data(), buf.size()));
		if (rc == 0 && ParseJsonObjectScalars(json, nullptr))
			w.Raw(json);
		else
		{
			w.BeginObject();
			w.KeyInt("probe_rc", rc);
			w.KeyString("unparsed_output", json);
			w.EndObject();
		}
	}

	static void WriteDrm(JsonWriter& w, StepLog& steps)
	{
		steps.Run("kernel.drm", [&](std::string&) {
			w.BeginArray();
			for (const std::string& name : ListDir("/sys/class/drm"))
			{
				if (name.compare(0, 4, "card") != 0 && name.compare(0, 7, "renderD") != 0)
					continue;
				if (name.find('-') != std::string::npos)
					continue; // connectors such as card0-HDMI-A-1
				const std::string link = "/sys/class/drm/" + name + "/device/driver";
				char target[512];
				const ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
				w.BeginObject();
				w.KeyString("node", name);
				if (n > 0)
				{
					target[n] = '\0';
					const char* slash = std::strrchr(target, '/');
					w.KeyString("driver", slash ? slash + 1 : target);
				}
				else
				{
					w.KeyNull("driver");
				}
				w.EndObject();
			}
			w.EndArray();
			return true;
		});
	}
#endif

	void WriteKernel(JsonWriter& w, StepLog& steps, KernelFacts* facts)
	{
		w.BeginObject();
#ifdef DRIVER_REPORT_LINUX
		const bool kgsl = PathExists("/dev/kgsl-3d0");
		const bool mali = PathExists("/dev/mali0");
		if (facts)
		{
			facts->kgsl_present = kgsl;
			facts->mali_present = mali;
		}
		if (kgsl)
		{
			w.Key("kgsl");
			WriteKgsl(w, steps, facts);
		}
		if (mali)
		{
			w.Key("mali");
			WriteMali(w, steps);
		}
		w.Key("drm");
		WriteDrm(w, steps);
#else
		(void)steps;
		(void)facts;
#endif
		w.EndObject();
	}
} // namespace GSDriverReport
