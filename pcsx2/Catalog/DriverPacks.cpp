// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Catalog/DriverPacks.h"

#include "Catalog/FileUtil.h"
#include "Catalog/Json.h"
#include "Catalog/ResponseCache.h"
#include "Catalog/TarGz.h"

#include "Config.h"
#include "Host.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SHA256.h"

#include "fmt/format.h"
#include "rapidjson/document.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>

#if defined(__linux__) && !defined(__ANDROID__)
#define ARMSX2_DRIVER_PACKS_SUPPORTED 1
#endif

#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;

namespace DriverPacks
{
	namespace
	{
		constexpr size_t MAX_ID_LENGTH = 128;
		constexpr size_t MAX_LISTING_BYTES = 4u << 20;
		constexpr size_t MAX_CHECKSUM_BYTES = 4096;
		constexpr u64 MAX_ARCHIVE_BYTES = 512ull << 20;
		constexpr const char* META_FILE = "meta.json";
		constexpr const char* STAGING_PREFIX = ".install-";

		std::string Lowercase(std::string_view text)
		{
			std::string out(text);
			std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		std::string JsonString(const rapidjson::Value& object, const char* key)
		{
			if (!object.IsObject())
				return {};
			const auto it = object.FindMember(key);
			if (it == object.MemberEnd() || !it->value.IsString())
				return {};
			return std::string(it->value.GetString(), it->value.GetStringLength());
		}

		bool IsHexDigest(std::string_view text)
		{
			return text.size() == 64 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
		}

		/// `*` any run, `?` one character; the whole text must match.
		bool GlobMatch(std::string_view pattern, std::string_view text)
		{
			size_t p = 0, t = 0;
			size_t star = std::string_view::npos, star_text = 0;
			while (t < text.size())
			{
				if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
				{
					p++;
					t++;
				}
				else if (p < pattern.size() && pattern[p] == '*')
				{
					star = p++;
					star_text = t;
				}
				else if (star != std::string_view::npos)
				{
					p = star + 1;
					t = ++star_text;
				}
				else
					return false;
			}
			while (p < pattern.size() && pattern[p] == '*')
				p++;
			return p == pattern.size();
		}

		/// First 64-hex-digit token in a `sha256sum` style file ("<hex>  <name>").
		std::string ParseChecksumFile(std::string_view text)
		{
			size_t pos = 0;
			while (pos < text.size())
			{
				while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
					pos++;
				size_t end = pos;
				while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
					end++;
				const std::string_view token = text.substr(pos, end - pos);
				if (IsHexDigest(token))
					return Lowercase(token);
				pos = end;
			}
			return {};
		}

		struct ScopeCleanup
		{
			std::function<void()> fn;
			~ScopeCleanup()
			{
				if (fn)
					fn();
			}
		};

		void RemoveTree(const std::string& path)
		{
			std::error_code ec;
			fs::remove_all(fs::path(path), ec);
		}

		std::string HumanBytes(u64 bytes)
		{
			if (bytes >= (1ull << 30))
				return fmt::format("{:.1f} GB", static_cast<double>(bytes) / (1 << 30));
			return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1 << 20));
		}

		// ---- last Apply state --------------------------------------------------------------------

		std::mutex s_apply_mutex;
		ApplyResult s_last_apply;
		/// Variables ApplyEnvironment set in this process and the values it gave them.
		std::vector<std::pair<std::string, std::string>> s_applied_env;
	} // namespace

	// =============================================================================================
	// Settings
	// =============================================================================================

	std::string GetSelectedPack()
	{
		// A caller that runs before the settings exist (an early main) gets the system driver
		// instead of a null dereference inside Host.
		if (!Host::Internal::GetBaseSettingsLayer())
			return {};
		std::string value = Host::GetBaseStringSettingValue(SETTING_SECTION, SETTING_KEY, "");
		const size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos)
			return {};
		const size_t last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	void SetSelectedPack(const std::string& id)
	{
		if (id.empty())
			Host::RemoveBaseSettingValue(SETTING_SECTION, SETTING_KEY);
		else
			Host::SetBaseStringSettingValue(SETTING_SECTION, SETTING_KEY, id.c_str());
		Host::CommitBaseSettingChanges();
	}

	// =============================================================================================
	// Platform and GPU gating
	// =============================================================================================

	std::string CurrentPlatform()
	{
#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		struct utsname name;
		if (uname(&name) != 0)
			return {};
		return std::string("linux-") + name.machine;
#else
		return {};
#endif
	}

	namespace
	{
		bool IsDrmDeviceName(const std::string& name)
		{
			const auto all_digits = [](std::string_view s) {
				return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
			};
			if (name.rfind("card", 0) == 0)
				return all_digits(std::string_view(name).substr(4));
			if (name.rfind("renderD", 0) == 0)
				return all_digits(std::string_view(name).substr(7));
			return false;
		}
	} // namespace

	bool IsAdrenoGpu(const std::string& sysfs_root)
	{
		std::error_code ec;

		// Downstream Qualcomm kernels expose the GPU as kgsl.
		if (fs::exists(fs::path(sysfs_root) / "class" / "kgsl" / "kgsl-3d0", ec))
			return true;

		// Mainline: the display/GPU node is a DRM device bound to the msm driver (Adreno 6xx and
		// later, including Snapdragon X). cardN-<connector> entries are connectors, not devices.
		const fs::path drm = fs::path(sysfs_root) / "class" / "drm";
		for (fs::directory_iterator it(drm, ec), end; !ec && it != end; it.increment(ec))
		{
			if (!IsDrmDeviceName(it->path().filename().string()))
				continue;
			std::error_code link_ec;
			const fs::path target = fs::read_symlink(it->path() / "device" / "driver", link_ec);
			if (link_ec)
				continue;
			const std::string driver = target.filename().string();
			if (driver == "msm" || driver == "msm_drm")
				return true;
		}
		return false;
	}

	namespace
	{
		// `platform` is what the caller is running on; empty means packs are not supported there.
		Availability AvailabilityFor(const std::string& platform, const std::string& sysfs_root)
		{
			Availability result;
			if (platform.empty())
			{
				result.reason = Availability::Reason::UnsupportedPlatform;
				result.message = "Driver packs are not available on this platform.";
			}
			else if (!IsAdrenoGpu(sysfs_root))
			{
				result.reason = Availability::Reason::NoMatchingGpu;
				result.message = "No driver packs for this GPU.";
			}
			else
				result.reason = Availability::Reason::Available;
			return result;
		}
	}

	Availability GetAvailability(const std::string& sysfs_root)
	{
		return AvailabilityFor(CurrentPlatform(), sysfs_root);
	}

	// =============================================================================================
	// Feeds
	// =============================================================================================

	std::vector<Feed> DefaultFeeds()
	{
		Feed turnip;
		turnip.id_prefix = "armsx2turnip";
		turnip.label = "ARMSX2 Turnip";
		turnip.releases_url = "https://api.github.com/repos/bmdhacks/armsx2-turnip/releases";
		turnip.asset_pattern = "turnip-*-aarch64.tar.gz";
		turnip.platform = "linux-aarch64";
		return {std::move(turnip)};
	}

	std::string MakeDriverId(std::string_view prefix, std::string_view tag)
	{
		std::string id = Lowercase(prefix);
		if (!prefix.empty() && !tag.empty())
			id.push_back('-');
		id += Lowercase(tag);
		for (char& c : id)
		{
			const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
			if (!ok)
				c = '_';
		}
		if (id.size() > MAX_ID_LENGTH)
			id.resize(MAX_ID_LENGTH);
		if (!id.empty() && id.front() == '.')
			id.front() = '_';
		return id;
	}

	bool IsValidDriverId(std::string_view id)
	{
		if (id.empty() || id.size() > MAX_ID_LENGTH || id.front() == '.')
			return false;
		return std::all_of(id.begin(), id.end(), [](char c) {
			return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		});
	}

	bool ParseReleases(const Feed& feed, std::string_view json, std::vector<RemoteDriver>* out, std::string* error)
	{
		rapidjson::Document doc;
		std::string parse_error;
		if (!Catalog::ParseJson(json, &doc, &parse_error))
		{
			if (error)
				*error = "The release list is not valid JSON (" + parse_error + ").";
			return false;
		}
		if (!doc.IsArray())
		{
			if (error)
				*error = "The release list has an unexpected shape.";
			return false;
		}

		std::set<std::string> seen;
		for (const auto& id : *out)
			seen.insert(id.id);

		for (const rapidjson::Value& release : doc.GetArray())
		{
			if (!release.IsObject())
				continue;
			const auto draft = release.FindMember("draft");
			if (draft != release.MemberEnd() && draft->value.IsBool() && draft->value.GetBool())
				continue;

			std::string tag = JsonString(release, "tag_name");
			std::string release_name = JsonString(release, "name");
			if (tag.empty())
				tag = release_name;
			if (release_name.empty())
				release_name = tag;
			if (tag.empty())
				continue;

			const auto assets_it = release.FindMember("assets");
			if (assets_it == release.MemberEnd() || !assets_it->value.IsArray())
				continue;
			const auto& assets = assets_it->value;

			const auto find_asset = [&](const std::string& name) -> const rapidjson::Value* {
				for (const rapidjson::Value& asset : assets.GetArray())
				{
					if (JsonString(asset, "name") == name)
						return &asset;
				}
				return nullptr;
			};

			bool prerelease = false;
			const auto pre = release.FindMember("prerelease");
			if (pre != release.MemberEnd() && pre->value.IsBool())
				prerelease = pre->value.GetBool();

			for (const rapidjson::Value& asset : assets.GetArray())
			{
				const std::string asset_name = JsonString(asset, "name");
				const std::string url = JsonString(asset, "browser_download_url");
				if (asset_name.empty() || !GlobMatch(feed.asset_pattern, asset_name) || !Catalog::IsHttpsUrl(url))
					continue;

				RemoteDriver driver;
				driver.id = MakeDriverId(feed.id_prefix, tag);
				if (!IsValidDriverId(driver.id) || !seen.insert(driver.id).second)
					continue;

				driver.feed_label = feed.label;
				driver.tag = tag;
				driver.release_name = release_name;
				driver.published_at = JsonString(release, "published_at");
				driver.prerelease = prerelease;
				driver.asset_name = asset_name;
				driver.download_url = url;
				const auto size = asset.FindMember("size");
				if (size != asset.MemberEnd() && size->value.IsUint64())
					driver.size = size->value.GetUint64();

				if (const rapidjson::Value* sum = find_asset(asset_name + ".sha256"))
				{
					const std::string sum_url = JsonString(*sum, "browser_download_url");
					if (Catalog::IsHttpsUrl(sum_url))
						driver.checksum_url = sum_url;
				}
				const std::string digest = JsonString(asset, "digest");
				if (digest.rfind("sha256:", 0) == 0 && IsHexDigest(std::string_view(digest).substr(7)))
					driver.api_digest = Lowercase(std::string_view(digest).substr(7));

				out->push_back(std::move(driver));
			}
		}
		return true;
	}

	// =============================================================================================
	// Test load
	// =============================================================================================

#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
	namespace
	{
		void WriteAll(int fd, const char* data, size_t size)
		{
			while (size > 0)
			{
				const ssize_t n = write(fd, data, size);
				if (n < 0)
				{
					if (errno == EINTR)
						continue;
					return;
				}
				data += n;
				size -= static_cast<size_t>(n);
			}
		}

		/// Closes every descriptor above stderr except `keep`. The forked child inherits all the
		/// emulator has open (the game image, save states, memory cards, sockets, a log), and the
		/// driver it loads is code from a download that runs constructors; none of that is its business.
		void CloseDescriptorsExcept(int keep)
		{
#ifdef SYS_close_range
			// close_range(2), Linux 5.9: [3, keep) and (keep, everything).
			const unsigned int all = ~0u;
			bool closed = true;
			if (keep > 3)
				closed = syscall(SYS_close_range, 3u, static_cast<unsigned int>(keep - 1), 0u) == 0;
			if (closed)
				closed = syscall(SYS_close_range, static_cast<unsigned int>(keep < 3 ? 3 : keep + 1), all, 0u) == 0;
			if (closed)
				return;
#endif
			// An older kernel: what /proc says is open. Closing while walking can skip entries, so walk
			// until a pass closes nothing; the directory's own descriptor is not ours to close.
			if (DIR* dir = opendir("/proc/self/fd"))
			{
				for (int pass = 0; pass < 8; pass++)
				{
					bool any = false;
					rewinddir(dir);
					while (const dirent* entry = readdir(dir))
					{
						char* end = nullptr;
						const long fd = strtol(entry->d_name, &end, 10);
						if (end == entry->d_name || *end != '\0' || fd < 3 || fd == keep || fd == dirfd(dir))
							continue;
						close(static_cast<int>(fd));
						any = true;
					}
					if (!any)
						break;
				}
				closedir(dir);
				return;
			}

			// No /proc either: try every number the process may have.
			struct rlimit limit;
			long last = 65535;
			if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY && static_cast<long>(limit.rlim_cur) - 1 < last)
				last = static_cast<long>(limit.rlim_cur) - 1;
			for (long fd = 3; fd <= last; fd++)
			{
				if (fd != keep)
					close(static_cast<int>(fd));
			}
		}

		/// Runs in the forked child. Touches nothing of the parent's state: no allocation of our
		/// own, no locks of ours, and `_exit` so no atexit handler or stdio flush runs twice.
		[[noreturn]] void TestLoadChild(const char* path, int result_fd)
		{
			// The parent may have crash handlers installed (the emulator's backtrace writer, the
			// fastmem fault handler). A crash in here must be a plain death the parent can read from
			// the wait status, not a second crash report.
			const int fatal_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};
			for (const int sig : fatal_signals)
			{
				struct sigaction sa;
				std::memset(&sa, 0, sizeof(sa));
				sa.sa_handler = SIG_DFL;
				sigemptyset(&sa.sa_mask);
				sigaction(sig, &sa, nullptr);
			}
			sigset_t none;
			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, nullptr);

			// No core files: a bad driver is expected here and a core is tens of megabytes.
			const struct rlimit no_core = {0, 0};
			setrlimit(RLIMIT_CORE, &no_core);
			prctl(PR_SET_DUMPABLE, 0);

			CloseDescriptorsExcept(result_fd);

			dlerror();
			void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
			if (!handle)
			{
				const char* text = dlerror();
				WriteAll(result_fd, "E", 1);
				if (text)
					WriteAll(result_fd, text, strnlen(text, 2000));
				_exit(1);
			}
			if (dlclose(handle) != 0)
			{
				const char* text = dlerror();
				WriteAll(result_fd, "Edlclose failed: ", 17);
				if (text)
					WriteAll(result_fd, text, strnlen(text, 2000));
				_exit(1);
			}
			WriteAll(result_fd, "K", 1);
			_exit(0);
		}
	} // namespace
#endif

	bool TestLoadLibrary(const std::string& path, std::string* error, std::chrono::milliseconds timeout)
	{
#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		const auto fail = [&](std::string message) {
			if (error)
				*error = std::move(message);
			return false;
		};

		if (access(path.c_str(), R_OK) != 0)
			return fail(fmt::format("The driver library \"{}\" cannot be read.", path));

		int fds[2];
		if (pipe2(fds, O_CLOEXEC) != 0)
			return fail("Could not start the driver check (pipe failed).");

		std::fflush(nullptr); // buffered output must not be written twice, once by each process
		const pid_t pid = fork();
		if (pid < 0)
		{
			close(fds[0]);
			close(fds[1]);
			return fail("Could not start the driver check (fork failed).");
		}
		if (pid == 0)
		{
			close(fds[0]);
			TestLoadChild(path.c_str(), fds[1]);
		}

		close(fds[1]);
		std::string reply;
		bool timed_out = false;
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;)
		{
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			if (left.count() <= 0)
			{
				timed_out = true;
				break;
			}
			struct pollfd pfd = {fds[0], POLLIN, 0};
			const int rc = poll(&pfd, 1, static_cast<int>(std::min<long long>(left.count(), 1000000)));
			if (rc < 0)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			if (rc == 0)
				continue; // re-evaluates the deadline
			char buffer[512];
			const ssize_t n = read(fds[0], buffer, sizeof(buffer));
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break; // the child closed its end: it exited (or crashed)
			reply.append(buffer, static_cast<size_t>(n));
			if (reply.size() > 4096)
				break;
		}
		close(fds[0]);

		if (timed_out)
			kill(pid, SIGKILL);

		int status = 0;
		bool have_status = false;
		for (;;)
		{
			const pid_t waited = waitpid(pid, &status, 0);
			if (waited == pid)
			{
				have_status = true;
				break;
			}
			if (waited < 0 && errno == EINTR)
				continue;
			break; // ECHILD: someone else (a SIGCHLD handler) already reaped it
		}

		if (timed_out)
			return fail(fmt::format("Loading the driver library did not finish within {} seconds and was stopped.", std::chrono::duration_cast<std::chrono::seconds>(timeout).count()));

		if (have_status && WIFSIGNALED(status))
		{
			const int sig = WTERMSIG(status);
			return fail(fmt::format("Loading the driver library crashed ({}). This pack does not work on this system.", strsignal(sig)));
		}

		if (!reply.empty() && reply[0] == 'K' && (!have_status || (WIFEXITED(status) && WEXITSTATUS(status) == 0)))
			return true;

		if (!reply.empty() && reply[0] == 'E')
		{
			std::string text = reply.substr(1);
			std::string message = fmt::format("The driver library could not be loaded: {}", text);
			if (text.find("cannot open shared object file") != std::string::npos)
				message += " (a shared library it needs is not installed on this system)";
			return fail(std::move(message));
		}

		return fail("The driver check ended without a result.");
#else
		if (error)
			*error = "Driver packs are not available on this platform.";
		return false;
#endif
	}

	// =============================================================================================
	// Installed packs
	// =============================================================================================

	namespace
	{
		std::string DriversDirAbsolute(const std::string& dir)
		{
			std::error_code ec;
			fs::path absolute = fs::absolute(fs::path(dir), ec);
			if (ec)
				return dir;
			return absolute.lexically_normal().string();
		}

		/// Reads <drivers>/<id>/meta.json and checks that the manifest and library it names exist.
		std::optional<InstalledDriver> ReadInstalledDriver(const std::string& drivers_dir, const std::string& id)
		{
			if (!IsValidDriverId(id))
				return std::nullopt;

			InstalledDriver driver;
			driver.id = id;
			driver.directory = Path::Combine(drivers_dir, id);

			const std::optional<std::string> text = FileSystem::ReadFileToString(Path::Combine(driver.directory, META_FILE).c_str());
			if (!text.has_value())
				return std::nullopt;
			rapidjson::Document meta;
			if (!Catalog::ParseJson(*text, &meta) || !meta.IsObject())
				return std::nullopt;

			// meta.json is ours but lives where a user can edit it: keep both names inside the folder.
			std::string manifest_rel, library_rel;
			if (!Catalog::NormalizeArchivePath(JsonString(meta, "manifest"), &manifest_rel) || manifest_rel.empty() ||
				!Catalog::NormalizeArchivePath(JsonString(meta, "library"), &library_rel) || library_rel.empty())
				return std::nullopt;

			driver.manifest_path = Path::Combine(driver.directory, manifest_rel);
			driver.library_path = Path::Combine(driver.directory, library_rel);
			if (!FileSystem::FileExists(driver.manifest_path.c_str()) || !FileSystem::FileExists(driver.library_path.c_str()))
				return std::nullopt;

			driver.name = JsonString(meta, "name");
			if (driver.name.empty())
				driver.name = id;
			driver.version = JsonString(meta, "version");
			driver.feed_label = JsonString(meta, "feed");
			driver.build_tag = JsonString(meta, "buildTag");
			driver.driver_info_token = JsonString(meta, "driverInfoToken");
			driver.mesa_version = JsonString(meta, "mesaVersion");
			driver.platforms = JsonString(meta, "platforms");
			driver.archive_sha256 = JsonString(meta, "archiveSha256");
			const auto installed_at = meta.FindMember("installedAt");
			if (installed_at != meta.MemberEnd() && installed_at->value.IsInt64())
				driver.installed_at = installed_at->value.GetInt64();
			return driver;
		}
	} // namespace

	std::string DriversDirectory()
	{
		return Path::Combine(EmuFolders::DataRoot, "drivers");
	}

	ManagerConfig ManagerConfig::ForUser()
	{
		ManagerConfig config;
		config.drivers_dir = DriversDirectory();
		config.cache_dir = Path::Combine(EmuFolders::Cache, "driver-packs");
		return config;
	}

	Manager::Manager(Catalog::Fetcher& fetcher, ManagerConfig config)
		: m_fetcher(fetcher)
		, m_config(std::move(config))
	{
		m_config.drivers_dir = DriversDirAbsolute(m_config.drivers_dir);
		if (!m_config.library_check)
			m_config.library_check = [](const std::string& path, std::string* error) { return TestLoadLibrary(path, error); };
	}

	Availability Manager::GetAvailability() const
	{
		return AvailabilityFor(m_config.platform, m_config.sysfs_root);
	}

	std::vector<InstalledDriver> Manager::ListInstalled() const
	{
		std::vector<InstalledDriver> drivers;
		std::error_code ec;
		for (fs::directory_iterator it(fs::path(m_config.drivers_dir), ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code type_ec;
			if (!it->is_directory(type_ec))
				continue;
			const std::string name = it->path().filename().string();
			if (name.empty() || name.front() == '.')
				continue;
			if (std::optional<InstalledDriver> driver = ReadInstalledDriver(m_config.drivers_dir, name))
				drivers.push_back(std::move(*driver));
		}

		std::sort(drivers.begin(), drivers.end(), [](const InstalledDriver& a, const InstalledDriver& b) {
			const std::string la = Lowercase(a.name), lb = Lowercase(b.name);
			return la != lb ? la < lb : a.id < b.id;
		});
		return drivers;
	}

	std::optional<InstalledDriver> Manager::FindInstalled(std::string_view id) const
	{
		return ReadInstalledDriver(m_config.drivers_dir, std::string(id));
	}

	bool Manager::Delete(std::string_view id, std::string* error)
	{
		const auto fail = [&](std::string message) {
			if (error)
				*error = std::move(message);
			return false;
		};

		if (!IsValidDriverId(id))
			return fail("That is not a driver pack name.");
		std::unique_lock<std::mutex> lock(m_install_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return fail("A driver is being installed; try again when it finishes.");

		const std::string directory = Path::Combine(m_config.drivers_dir, id);
		std::error_code ec;
		fs::remove_all(fs::path(directory), ec);
		if (ec)
			return fail(fmt::format("Could not delete \"{}\": {}", directory, ec.message()));
		RemoveTree(Path::Combine(m_config.drivers_dir, std::string(STAGING_PREFIX) + std::string(id)));
		return true;
	}

	// =============================================================================================
	// Listing
	// =============================================================================================

	RemoteListing Manager::ListAvailable(const Catalog::CancelToken& cancel, bool force_refresh)
	{
		RemoteListing listing;
		if (!GetAvailability().IsAvailable())
			return listing;

		std::optional<Catalog::ResponseCache> cache;
		if (!m_config.cache_dir.empty())
			cache.emplace(m_config.cache_dir);

		for (const Feed& feed : m_config.feeds)
		{
			if (feed.platform != m_config.platform)
				continue;
			if (cancel.IsCancelled())
				break;

			std::string cache_name = "releases-" + MakeDriverId(feed.id_prefix, {}) + ".json";

			// Parses `body` into this feed's entries; leaves `listing` alone if it is not a release list.
			const auto try_body = [&](const std::string& body, std::string* why) {
				std::vector<RemoteDriver> parsed;
				if (!ParseReleases(feed, body, &parsed, why))
					return false;
				for (RemoteDriver& d : parsed)
				{
					const bool duplicate = std::any_of(listing.drivers.begin(), listing.drivers.end(), [&](const RemoteDriver& have) { return have.id == d.id; });
					if (!duplicate)
						listing.drivers.push_back(std::move(d));
				}
				return true;
			};

			if (cache && !force_refresh && cache->IsFresh(cache_name, m_config.listing_max_age))
			{
				if (auto entry = cache->Load(cache_name))
				{
					std::string ignored;
					if (try_body(entry->body, &ignored))
						continue;
				}
			}

			Catalog::FetchOptions options;
			options.cancel = cancel;
			options.headers.push_back({"Accept", "application/vnd.github+json"});
			const Catalog::FetchResponse response = m_fetcher.Get(feed.releases_url, MAX_LISTING_BYTES, options);

			std::string problem;
			if (response.result == Catalog::FetchResult::Cancelled)
				break;
			if (response.IsSuccess())
			{
				if (try_body(response.body, &problem))
				{
					if (cache)
						cache->Store(cache_name, response.body);
					continue;
				}
			}
			else if (response.result == Catalog::FetchResult::Ok)
			{
				problem = (response.status == 403 || response.status == 429) ?
				              "GitHub is limiting requests from this network; try again later." :
				              fmt::format("GitHub answered with HTTP {}.", response.status);
			}
			else
				problem = response.error.empty() ? "The request failed." : response.error;

			// Offline, rate limited or garbled: an older copy beats nothing.
			if (cache)
			{
				if (auto entry = cache->Load(cache_name))
				{
					std::string ignored;
					if (try_body(entry->body, &ignored))
					{
						listing.used_stale_cache = true;
						continue;
					}
				}
			}
			listing.errors.push_back({feed.label, std::move(problem)});
		}
		return listing;
	}

	// =============================================================================================
	// Install
	// =============================================================================================

	struct Manager::Source
	{
		std::string name;
		std::string version;
		std::string feed_label;
		std::string source_url;
		std::string archive_sha256;
	};

	namespace
	{
		struct IcdManifest
		{
			std::string path; ///< relative to the extraction root
			std::string dir; ///< its folder, relative to the extraction root ("" = the root)
			std::string file_name;
			rapidjson::Document doc;
		};

		int PathDepth(const std::string& path)
		{
			return static_cast<int>(std::count(path.begin(), path.end(), '/'));
		}

		bool FindIcdManifest(const std::string& root, const std::vector<std::string>& files, IcdManifest* out, std::string* error)
		{
			std::vector<std::string> candidates;
			for (const std::string& file : files)
			{
				const std::string name = Lowercase(Path::GetFileName(file));
				if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0 && name.find("icd") != std::string::npos)
					candidates.push_back(file);
			}
			// The shallowest manifest is the pack's own; ties go alphabetically so the choice is stable.
			std::sort(candidates.begin(), candidates.end(), [](const std::string& a, const std::string& b) {
				const int da = PathDepth(a), db = PathDepth(b);
				return da != db ? da < db : a < b;
			});

			for (const std::string& candidate : candidates)
			{
				const std::optional<std::string> text = FileSystem::ReadFileToString(Path::Combine(root, candidate).c_str());
				if (!text.has_value())
					continue;
				rapidjson::Document doc;
				if (!Catalog::ParseJson(*text, &doc) || !doc.IsObject())
					continue;
				const auto icd = doc.FindMember("ICD");
				if (icd == doc.MemberEnd() || !icd->value.IsObject() || JsonString(icd->value, "library_path").empty())
					continue;

				out->path = candidate;
				const size_t slash = candidate.rfind('/');
				out->dir = (slash == std::string::npos) ? std::string() : candidate.substr(0, slash);
				out->file_name = (slash == std::string::npos) ? candidate : candidate.substr(slash + 1);
				out->doc = std::move(doc);
				return true;
			}

			if (error)
				*error = "The archive has no Vulkan driver manifest (an ICD .json file with ICD.library_path).";
			return false;
		}

		/// Finds the library the manifest names inside the extracted tree: as a path relative to
		/// the manifest's folder if it has one, else by file name beside the manifest. The shipped
		/// path is the builder's device path and is never used.
		bool ResolveLibrary(const std::string& root, const IcdManifest& manifest, std::string* library_rel, std::string* error)
		{
			const auto icd = manifest.doc.FindMember("ICD");
			const std::string named = JsonString(icd->value, "library_path");

			std::vector<std::string> candidates;
			if (!named.empty() && named.front() != '/')
			{
				std::string normalised;
				if (Catalog::NormalizeArchivePath(named, &normalised) && !normalised.empty())
					candidates.push_back(normalised);
			}
			const std::string base(Path::GetFileName(named));
			if (!base.empty())
				candidates.push_back(base);

			for (const std::string& candidate : candidates)
			{
				const std::string full = Path::Combine(manifest.dir.empty() ? root : Path::Combine(root, manifest.dir), candidate);
				std::error_code ec;
				if (fs::is_regular_file(fs::path(full), ec) && fs::file_size(fs::path(full), ec) > 0)
				{
					*library_rel = candidate;
					return true;
				}
			}
			if (error)
				*error = fmt::format("The manifest names \"{}\" but the archive does not contain it.", base.empty() ? named : base);
			return false;
		}
	} // namespace

	InstallResult Manager::InstallArchive(const std::string& archive_path, const std::string& stage_root, const std::string& id,
		const Source& source, const InstallProgressCallback& progress, const Catalog::CancelToken& cancel)
	{
		InstallResult result;
		result.id = id;
		const auto done = [&](InstallStatus status, std::string message) {
			result.status = status;
			result.message = std::move(message);
			return result;
		};
		const auto report = [&](InstallStage stage, u64 bytes = 0, u64 total = 0) {
			if (progress)
				progress({stage, bytes, total});
		};

		// ---- extract ---------------------------------------------------------------------------
		report(InstallStage::Extracting);
		const std::string extract_dir = Path::Combine(stage_root, "extract");
		Catalog::TarExtractInfo extracted;
		std::string tar_error;
		const Catalog::TarResult tar_result = Catalog::ExtractTarGz(archive_path, extract_dir, Catalog::TarLimits(), &extracted, &tar_error, cancel.Flag());
		if (tar_result == Catalog::TarResult::Cancelled)
			return done(InstallStatus::Cancelled, {});
		if (tar_result != Catalog::TarResult::Ok)
			return done(InstallStatus::BadArchive, tar_error);

		// ---- find the pack inside it -----------------------------------------------------------
		IcdManifest manifest;
		std::string problem;
		if (!FindIcdManifest(extract_dir, extracted.files, &manifest, &problem))
			return done(InstallStatus::BadArchive, problem);
		std::string library_rel;
		if (!ResolveLibrary(extract_dir, manifest, &library_rel, &problem))
			return done(InstallStatus::BadArchive, problem);

		// The pack is the manifest's folder; the wrapper folders around it ("turnip/axfl2-002/")
		// are dropped.
		const std::string pack_dir = Path::Combine(stage_root, "pack");
		std::error_code ec;
		fs::rename(fs::path(manifest.dir.empty() ? extract_dir : Path::Combine(extract_dir, manifest.dir)), fs::path(pack_dir), ec);
		if (ec)
			return done(InstallStatus::IoError, fmt::format("Could not prepare the pack folder: {}", ec.message()));

		// ---- would it load? --------------------------------------------------------------------
		if (cancel.IsCancelled())
			return done(InstallStatus::Cancelled, {});
		report(InstallStage::Checking);
		std::string load_error;
		if (!m_config.library_check(Path::Combine(pack_dir, library_rel), &load_error))
			return done(InstallStatus::LoadFailed, load_error);

		// ---- write our manifest and meta.json ---------------------------------------------------
		if (cancel.IsCancelled())
			return done(InstallStatus::Cancelled, {});
		report(InstallStage::Finishing);

		const std::string final_dir = Path::Combine(m_config.drivers_dir, id);
		const std::string final_library = Path::Combine(final_dir, library_rel);
		{
			rapidjson::Document& doc = manifest.doc;
			auto& icd = doc.FindMember("ICD")->value;
			icd.FindMember("library_path")->value.SetString(final_library.c_str(), static_cast<rapidjson::SizeType>(final_library.size()), doc.GetAllocator());
			Error write_error;
			if (!Catalog::WriteJsonFileAtomic(Path::Combine(pack_dir, manifest.file_name), doc, &write_error))
				return done(InstallStatus::IoError, fmt::format("Could not write the driver manifest: {}", write_error.GetDescription()));
		}

		// What the pack says about itself (turnip packs ship a MANIFEST.json beside the library).
		std::string build_tag, info_token, mesa_version, platforms;
		if (const auto text = FileSystem::ReadFileToString(Path::Combine(pack_dir, "MANIFEST.json").c_str()))
		{
			rapidjson::Document about;
			if (Catalog::ParseJson(*text, &about) && about.IsObject())
			{
				build_tag = JsonString(about, "build_tag");
				info_token = JsonString(about, "driver_info_token");
				mesa_version = JsonString(about, "mesa_version");
				platforms = JsonString(about, "platforms");
			}
		}

		{
			rapidjson::Document meta;
			meta.SetObject();
			auto& alloc = meta.GetAllocator();
			const auto put = [&](const char* key, const std::string& value) {
				meta.AddMember(rapidjson::StringRef(key), rapidjson::Value(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc), alloc);
			};
			meta.AddMember("schemaVersion", 1, alloc);
			put("id", id);
			put("name", source.name.empty() ? id : source.name);
			put("version", source.version);
			put("feed", source.feed_label);
			put("sourceUrl", source.source_url);
			put("archiveSha256", source.archive_sha256);
			meta.AddMember("installedAt", static_cast<int64_t>(std::time(nullptr)), alloc);
			put("manifest", manifest.file_name);
			put("library", library_rel);
			put("buildTag", build_tag);
			put("driverInfoToken", info_token);
			put("mesaVersion", mesa_version);
			put("platforms", platforms);
			Error write_error;
			if (!Catalog::WriteJsonFileAtomic(Path::Combine(pack_dir, META_FILE), meta, &write_error))
				return done(InstallStatus::IoError, fmt::format("Could not write the driver's metadata: {}", write_error.GetDescription()));
		}

		// ---- commit -----------------------------------------------------------------------------
		// Move any pack with this id aside first so a failure puts it back.
		const std::string old_dir = Path::Combine(stage_root, "previous");
		bool moved_old = false;
		if (fs::exists(fs::path(final_dir), ec))
		{
			fs::rename(fs::path(final_dir), fs::path(old_dir), ec);
			if (ec)
				return done(InstallStatus::IoError, fmt::format("Could not replace the installed copy: {}", ec.message()));
			moved_old = true;
		}
		fs::rename(fs::path(pack_dir), fs::path(final_dir), ec);
		if (ec)
		{
			const std::string message = fmt::format("Could not move the driver into place: {}", ec.message());
			if (moved_old)
				fs::rename(fs::path(old_dir), fs::path(final_dir), ec);
			return done(InstallStatus::IoError, message);
		}

		Console.WriteLnFmt("DriverPacks: installed '{}' to {}", id, final_dir);
		result.status = InstallStatus::Ok;
		result.message.clear();
		return result;
	}

	InstallResult Manager::Install(const RemoteDriver& driver, const InstallProgressCallback& progress, const Catalog::CancelToken& cancel)
	{
		InstallResult result;
		result.id = driver.id;
		const auto done = [&](InstallStatus status, std::string message) {
			result.status = status;
			result.message = std::move(message);
			return result;
		};
		const auto report = [&](InstallStage stage, u64 bytes = 0, u64 total = 0) {
			if (progress)
				progress({stage, bytes, total});
		};

#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		if (m_config.platform.empty())
			return done(InstallStatus::Unsupported, "Driver packs are not available on this platform.");
		if (!IsValidDriverId(driver.id))
			return done(InstallStatus::BadArchive, "The driver has an invalid name.");
		if (!Catalog::IsHttpsUrl(driver.download_url))
			return done(InstallStatus::DownloadFailed, "The driver's download address is not https.");

		std::unique_lock<std::mutex> lock(m_install_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return done(InstallStatus::Busy, "Another driver is being installed.");

		report(InstallStage::Preparing);

		std::error_code ec;
		fs::create_directories(fs::path(m_config.drivers_dir), ec);
		if (ec)
			return done(InstallStatus::IoError, fmt::format("Could not create \"{}\": {}", m_config.drivers_dir, ec.message()));

		// Room for the download and its extraction (a packed .so compresses about 4:1) plus slack.
		const u64 slack = 64ull << 20;
		const u64 needed = (driver.size > (UINT64_MAX - slack) / 6) ? UINT64_MAX : driver.size * 6 + slack;
		const fs::space_info space = fs::space(fs::path(m_config.drivers_dir), ec);
		if (!ec && space.available < needed)
			return done(InstallStatus::NoSpace, fmt::format("Not enough free space in \"{}\": about {} needed, {} available.", m_config.drivers_dir, HumanBytes(needed), HumanBytes(space.available)));

		const std::string stage_root = Path::Combine(m_config.drivers_dir, std::string(STAGING_PREFIX) + driver.id);
		RemoveTree(stage_root);
		fs::create_directories(fs::path(stage_root), ec);
		if (ec)
			return done(InstallStatus::IoError, fmt::format("Could not create \"{}\": {}", stage_root, ec.message()));
		ScopeCleanup cleanup{[&]() { RemoveTree(stage_root); }};

		// ---- what the archive must hash to -------------------------------------------------------
		// Fetched before the archive so a release without one costs no bandwidth.
		std::string expected;
		if (!driver.checksum_url.empty())
		{
			Catalog::FetchOptions options;
			options.cancel = cancel;
			const Catalog::FetchResponse sum = m_fetcher.Get(driver.checksum_url, MAX_CHECKSUM_BYTES, options);
			if (sum.result == Catalog::FetchResult::Cancelled)
				return done(InstallStatus::Cancelled, {});
			if (!sum.IsSuccess())
				return done(InstallStatus::DownloadFailed, fmt::format("Could not download the checksum file ({}).", sum.result == Catalog::FetchResult::Ok ? fmt::format("HTTP {}", sum.status) : sum.error));
			expected = ParseChecksumFile(sum.body);
			if (expected.empty())
				return done(InstallStatus::ChecksumMissing, "The checksum file is not readable; nothing was installed.");
		}
		if (!driver.api_digest.empty())
		{
			if (expected.empty())
				expected = driver.api_digest;
			else if (expected != driver.api_digest)
				return done(InstallStatus::ChecksumMismatch, "The release's checksum file and GitHub's digest for the download disagree; nothing was installed.");
		}
		if (expected.empty())
			return done(InstallStatus::ChecksumMissing, "This release does not publish a checksum, so the download cannot be verified; nothing was installed.");

		// ---- download, hashing as it arrives -------------------------------------------------------
		const std::string archive_path = Path::Combine(stage_root, "archive.tar.gz");
		SHA256Digest sha;
		Catalog::FetchOptions options;
		options.cancel = cancel;
		options.max_file_bytes = MAX_ARCHIVE_BYTES;
		options.on_data = [&sha](const u8* data, size_t size) { sha.Update(data, size); };
		options.on_progress = [&](u64 bytes, u64 total) { report(InstallStage::Downloading, bytes, total ? total : driver.size); };
		report(InstallStage::Downloading, 0, driver.size);
		const Catalog::FetchResponse download = m_fetcher.GetToFile(driver.download_url, archive_path, options);
		if (download.result == Catalog::FetchResult::Cancelled)
			return done(InstallStatus::Cancelled, {});
		if (!download.IsSuccess())
		{
			if (download.result == Catalog::FetchResult::Ok)
				return done(InstallStatus::DownloadFailed, fmt::format("The download failed (HTTP {}).", download.status));
			return done(InstallStatus::DownloadFailed, download.error.empty() ? "The download failed." : "The download failed: " + download.error);
		}
		if (driver.size != 0 && download.bytes_received != driver.size)
			return done(InstallStatus::DownloadFailed, fmt::format("The download is incomplete ({} of {} bytes).", download.bytes_received, driver.size));

		report(InstallStage::Verifying);
		const std::string actual = sha.FinalHex();
		if (actual != expected)
			return done(InstallStatus::ChecksumMismatch, fmt::format("Checksum mismatch: the download hashes to {} but the release says {}. The file was discarded.", actual, expected));

		Source source;
		source.name = driver.release_name;
		source.version = driver.tag;
		source.feed_label = driver.feed_label;
		source.source_url = driver.download_url;
		source.archive_sha256 = actual;
		return InstallArchive(archive_path, stage_root, driver.id, source, progress, cancel);
#else
		(void)report;
		(void)progress;
		(void)cancel;
		return done(InstallStatus::Unsupported, "Driver packs are not available on this platform.");
#endif
	}

	InstallResult Manager::InstallFromFile(const std::string& archive_path, const InstallProgressCallback& progress, const Catalog::CancelToken& cancel)
	{
		InstallResult result;
		const auto done = [&](InstallStatus status, std::string message) {
			result.status = status;
			result.message = std::move(message);
			return result;
		};

#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		if (m_config.platform.empty())
			return done(InstallStatus::Unsupported, "Driver packs are not available on this platform.");

		std::string stem(Path::GetFileName(archive_path));
		for (const char* suffix : {".tar.gz", ".tgz"})
		{
			const size_t n = std::strlen(suffix);
			if (stem.size() > n && Lowercase(stem).compare(stem.size() - n, n, suffix) == 0)
			{
				stem.resize(stem.size() - n);
				break;
			}
		}
		result.id = MakeDriverId("local", stem);
		if (!IsValidDriverId(result.id))
			return done(InstallStatus::BadArchive, "That file name cannot be used as a driver name.");

		std::unique_lock<std::mutex> lock(m_install_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return done(InstallStatus::Busy, "Another driver is being installed.");

		if (progress)
			progress({InstallStage::Verifying, 0, 0});
		std::string hex;
		Error hash_error;
		const SHA256Digest::FileResult hashed = SHA256Digest::HashFile(archive_path.c_str(), &hex, &hash_error, cancel.Flag());
		if (hashed == SHA256Digest::FileResult::Cancelled)
			return done(InstallStatus::Cancelled, {});
		if (hashed != SHA256Digest::FileResult::Ok)
			return done(InstallStatus::IoError, fmt::format("Could not read \"{}\": {}", archive_path, hash_error.GetDescription()));

		std::error_code ec;
		fs::create_directories(fs::path(m_config.drivers_dir), ec);
		if (ec)
			return done(InstallStatus::IoError, fmt::format("Could not create \"{}\": {}", m_config.drivers_dir, ec.message()));
		const std::string stage_root = Path::Combine(m_config.drivers_dir, std::string(STAGING_PREFIX) + result.id);
		RemoveTree(stage_root);
		fs::create_directories(fs::path(stage_root), ec);
		if (ec)
			return done(InstallStatus::IoError, fmt::format("Could not create \"{}\": {}", stage_root, ec.message()));
		ScopeCleanup cleanup{[&]() { RemoveTree(stage_root); }};

		Source source;
		source.name = std::string(Path::GetFileName(archive_path));
		source.version = "imported";
		source.archive_sha256 = hex;
		return InstallArchive(archive_path, stage_root, result.id, source, progress, cancel);
#else
		(void)progress;
		(void)cancel;
		(void)archive_path;
		return done(InstallStatus::Unsupported, "Driver packs are not available on this platform.");
#endif
	}

	// =============================================================================================
	// Applying a pack to this process
	// =============================================================================================

	namespace
	{
		bool EnvIsSet(const char* name)
		{
			const char* value = std::getenv(name);
			return value && *value;
		}

		ApplyResult RecordApply(ApplyResult result)
		{
			std::lock_guard lock(s_apply_mutex);
			s_last_apply = result;
			return result;
		}
	} // namespace

	ApplyResult ApplyEnvironment(const std::string& drivers_dir, const std::string& pack_id, const std::string& sysfs_root)
	{
		ApplyResult result;
		result.pack_id = pack_id;

		{
			std::lock_guard lock(s_apply_mutex);
			s_applied_env.clear();
		}

#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		if (pack_id.empty())
		{
			result.outcome = ApplyOutcome::SystemDriver;
			return RecordApply(result);
		}

		if (EnvIsSet("VK_DRIVER_FILES") || EnvIsSet("VK_ICD_FILENAMES"))
		{
			result.outcome = ApplyOutcome::EnvironmentAlreadySet;
			result.message = fmt::format("DriverPacks: VK_DRIVER_FILES or VK_ICD_FILENAMES is already set in the environment; the selected driver pack '{}' is ignored.", pack_id);
			Console.WriteLnFmt("{}", result.message);
			return RecordApply(result);
		}

		if (!IsAdrenoGpu(sysfs_root))
		{
			result.outcome = ApplyOutcome::GpuNotSupported;
			result.message = fmt::format("DriverPacks: driver pack '{}' is selected but this machine has no Adreno GPU; using the system driver.", pack_id);
			Console.WarningFmt("{}", result.message);
			return RecordApply(result);
		}

		const std::optional<InstalledDriver> installed = ReadInstalledDriver(DriversDirAbsolute(drivers_dir), pack_id);
		if (!installed.has_value())
		{
			result.outcome = ApplyOutcome::PackMissing;
			result.message = fmt::format("DriverPacks: driver pack '{}' is selected but is not installed under {}; using the system driver.", pack_id, drivers_dir);
			Console.WarningFmt("{}", result.message);
			return RecordApply(result);
		}

		setenv("VK_DRIVER_FILES", installed->manifest_path.c_str(), 1);
		setenv("VK_ICD_FILENAMES", installed->manifest_path.c_str(), 1);
		unsetenv("VK_ADD_DRIVER_FILES");
		{
			std::lock_guard lock(s_apply_mutex);
			s_applied_env.push_back({"VK_DRIVER_FILES", installed->manifest_path});
			s_applied_env.push_back({"VK_ICD_FILENAMES", installed->manifest_path});
		}

		result.outcome = ApplyOutcome::Applied;
		result.manifest_path = installed->manifest_path;
		result.message = fmt::format("DriverPacks: using driver pack '{}' ({}){}; VK_DRIVER_FILES={}", pack_id, installed->name,
			installed->driver_info_token.empty() ? std::string() : fmt::format(", expect {} in the VK driver line", installed->driver_info_token), installed->manifest_path);
		Console.WriteLnFmt("{}", result.message);
		return RecordApply(result);
#else
		(void)drivers_dir;
		(void)sysfs_root;
		result.outcome = ApplyOutcome::Unsupported;
		return RecordApply(result);
#endif
	}

	ApplyResult ApplyEnvironment()
	{
		return ApplyEnvironment(DriversDirectory(), GetSelectedPack());
	}

	ApplyResult GetLastApplyResult()
	{
		std::lock_guard lock(s_apply_mutex);
		return s_last_apply;
	}

	// =============================================================================================
	// First-run setup
	// =============================================================================================

	bool ShouldOfferInSetup(const SetupContext& context)
	{
		return context.availability.IsAvailable() && !context.session_is_x11 && context.selected_pack.empty() &&
		       context.installed_count == 0 && !context.environment_sets_driver;
	}

	SetupContext GetSetupContext(const Manager& manager, bool session_is_x11)
	{
		SetupContext context;
		context.availability = manager.GetAvailability();
		context.selected_pack = GetSelectedPack();
		context.environment_sets_driver = EnvIsSet("VK_DRIVER_FILES") || EnvIsSet("VK_ICD_FILENAMES");
		context.installed_count = manager.ListInstalled().size();
		context.session_is_x11 = session_is_x11;
		return context;
	}

	const RemoteDriver* PickRecommended(const std::vector<RemoteDriver>& drivers)
	{
		for (const RemoteDriver& driver : drivers)
		{
			if (!driver.prerelease)
				return &driver;
		}
		return drivers.empty() ? nullptr : &drivers.front();
	}

	// =============================================================================================
	// Restart
	// =============================================================================================

	namespace
	{
		std::mutex s_relaunch_mutex;
		bool s_relaunch_registered = false;
		RelaunchPlan s_relaunch_plan;
		bool s_relaunch_armed = false;

#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		/// /proc files report a size of 0, so FileSystem::ReadFileToString (which sizes its buffer
		/// from fstat) returns nothing for them; read until EOF instead.
		std::optional<std::string> ReadProcFile(const char* path)
		{
			std::FILE* fp = std::fopen(path, "rb");
			if (!fp)
				return std::nullopt;
			std::string data;
			char buffer[4096];
			size_t n;
			while ((n = std::fread(buffer, 1, sizeof(buffer), fp)) > 0 && data.size() < (1u << 20))
				data.append(buffer, n);
			std::fclose(fp);
			return data;
		}

		void MarkDescriptorsCloseOnExec()
		{
#ifdef SYS_close_range
			// CLOSE_RANGE_CLOEXEC (4) needs Linux 5.11; older kernels answer EINVAL or ENOSYS.
			if (syscall(SYS_close_range, 3u, ~0u, 4u) == 0)
				return;
#endif
			std::vector<int> fds;
			if (DIR* dir = opendir("/proc/self/fd"))
			{
				const int dir_fd = dirfd(dir);
				while (const dirent* entry = readdir(dir))
				{
					char* end = nullptr;
					const long fd = std::strtol(entry->d_name, &end, 10);
					if (end != entry->d_name && *end == '\0' && fd >= 3 && fd != dir_fd)
						fds.push_back(static_cast<int>(fd));
				}
				closedir(dir);
			}
			else
			{
				for (int fd = 3; fd < 1024; fd++)
					fds.push_back(fd);
			}
			for (const int fd : fds)
				fcntl(fd, F_SETFD, FD_CLOEXEC);
		}
#endif

		void RelaunchExitHandler()
		{
			RelaunchPlan plan;
			{
				std::lock_guard lock(s_relaunch_mutex);
				if (!s_relaunch_armed)
					return;
				plan = s_relaunch_plan;
			}
			std::string error;
			if (!ExecRelaunchPlan(plan, &error))
				std::fprintf(stderr, "Could not restart: %s\n", error.c_str());
		}
	} // namespace

	bool MakeRelaunchPlan(RelaunchPlan* plan, std::string_view cmdline, std::string_view exe_link, std::string_view appimage, std::string* error)
	{
		*plan = {};
		const auto fail = [&](const char* message) {
			if (error)
				*error = message;
			return false;
		};

		// NUL terminated arguments; an empty argument is two NULs in a row.
		size_t pos = 0;
		while (pos < cmdline.size())
		{
			size_t end = cmdline.find('\0', pos);
			if (end == std::string_view::npos)
				end = cmdline.size();
			plan->argv.emplace_back(cmdline.substr(pos, end - pos));
			pos = end + 1;
		}
		if (plan->argv.empty())
			return fail("Could not read this program's command line.");

		std::error_code ec;
		if (!appimage.empty() && fs::exists(fs::path(std::string(appimage)), ec))
		{
			// Running from an AppImage: the binary lives on a temporary mount that goes away with this
			// process. The image file is what starts it again.
			plan->executable = std::string(appimage);
			plan->argv[0] = plan->executable;
			return true;
		}

		std::string exe(exe_link);
		static constexpr std::string_view deleted = " (deleted)";
		if (exe.size() > deleted.size() && exe.compare(exe.size() - deleted.size(), deleted.size(), deleted) == 0)
		{
			// The file was replaced on disk while we ran (an update). Start whatever is there now.
			std::string current = exe.substr(0, exe.size() - deleted.size());
			if (fs::exists(fs::path(current), ec))
				exe = std::move(current);
			else
				exe = "/proc/self/exe";
		}
		if (exe.empty())
			return fail("Could not find this program's executable.");
		plan->executable = std::move(exe);
		return true;
	}

	bool BuildRelaunchPlan(RelaunchPlan* plan, std::string* error)
	{
#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		const std::optional<std::string> cmdline = ReadProcFile("/proc/self/cmdline");
		if (!cmdline.has_value() || cmdline->empty())
		{
			if (error)
				*error = "Could not read /proc/self/cmdline.";
			return false;
		}
		char link[4096];
		const ssize_t n = readlink("/proc/self/exe", link, sizeof(link) - 1);
		if (n <= 0)
		{
			if (error)
				*error = "Could not read /proc/self/exe.";
			return false;
		}
		const char* appimage = std::getenv("APPIMAGE");
		if (!MakeRelaunchPlan(plan, *cmdline, std::string_view(link, static_cast<size_t>(n)), appimage ? appimage : "", error))
			return false;

		std::lock_guard lock(s_apply_mutex);
		for (const auto& [name, value] : s_applied_env)
		{
			// Only if it is still what we set; something else may have changed it since.
			const char* current = std::getenv(name.c_str());
			if (current && value == current)
				plan->unset_env.push_back(name);
		}
		return true;
#else
		(void)plan;
		if (error)
			*error = "Restarting is not available on this platform.";
		return false;
#endif
	}

	bool ExecRelaunchPlan(const RelaunchPlan& plan, std::string* error)
	{
#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		const auto fail = [&](std::string message) {
			if (error)
				*error = std::move(message);
			return false;
		};
		if (plan.executable.empty() || plan.argv.empty())
			return fail("Nothing to restart.");

		std::vector<char*> argv;
		for (const std::string& arg : plan.argv)
			argv.push_back(const_cast<char*>(arg.c_str()));
		argv.push_back(nullptr);

		std::vector<std::string> env_storage;
		for (char** e = environ; e && *e; e++)
		{
			const std::string_view entry(*e);
			const size_t eq = entry.find('=');
			const std::string_view name = entry.substr(0, eq);
			if (std::find(plan.unset_env.begin(), plan.unset_env.end(), name) == plan.unset_env.end())
				env_storage.emplace_back(entry);
		}
		std::vector<char*> envp;
		for (std::string& entry : env_storage)
			envp.push_back(entry.data());
		envp.push_back(nullptr);

		std::fflush(nullptr);

		// Everything above stderr must not reach the new process: a listener socket, a DRM handle or a
		// lock file inherited by it would keep it from starting cleanly. Marking close-on-exec,
		// instead of closing now, leaves this process whole if the exec fails.
		MarkDescriptorsCloseOnExec();

		execve(plan.executable.c_str(), argv.data(), envp.data());
		return fail(fmt::format("Could not start \"{}\": {}", plan.executable, std::strerror(errno)));
#else
		(void)plan;
		if (error)
			*error = "Restarting is not available on this platform.";
		return false;
#endif
	}

	bool RelaunchNow(std::string* error)
	{
		RelaunchPlan plan;
		if (!BuildRelaunchPlan(&plan, error))
			return false;
		return ExecRelaunchPlan(plan, error);
	}

	bool RelaunchAtExit(const RelaunchPlan& plan, std::string* error)
	{
#ifdef ARMSX2_DRIVER_PACKS_SUPPORTED
		std::lock_guard lock(s_relaunch_mutex);
		s_relaunch_plan = plan;
		s_relaunch_armed = true;
		if (!s_relaunch_registered)
		{
			if (std::atexit(&RelaunchExitHandler) != 0)
			{
				s_relaunch_armed = false;
				if (error)
					*error = "Could not register the restart.";
				return false;
			}
			s_relaunch_registered = true;
		}
		return true;
#else
		(void)plan;
		if (error)
			*error = "Restarting is not available on this platform.";
		return false;
#endif
	}

	bool RelaunchAtExit(std::string* error)
	{
		RelaunchPlan plan;
		if (!BuildRelaunchPlan(&plan, error))
			return false;
		return RelaunchAtExit(plan, error);
	}
} // namespace DriverPacks
