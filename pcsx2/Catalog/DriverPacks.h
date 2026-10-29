// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Downloadable GPU driver packs for desktop Linux (Turnip on Adreno): list a feed's packs,
// install a pack, list and delete installed packs, and point the process at the selected pack
// before Vulkan starts.
//
// The Vulkan loader reads VK_DRIVER_FILES / VK_ICD_FILENAMES once, when libvulkan is first used
// in the process. So ApplyEnvironment() must set them in main() before any GS or Vulkan code
// runs, and changing the selection needs a restart (RelaunchAtExit). Nothing is installed
// system-wide.
//
// On disk, per installed pack:
//
//   <DataRoot>/drivers/<id>/
//       meta.json                     name, version, build tag, ...
//       freedreno_icd.aarch64.json    loader manifest, written by the installer with library_path
//                                     set to this pack's library as an absolute path. The archive's
//                                     own manifest points at the builder's device
//                                     (/storage/turnip/...) and is replaced.
//       libvulkan_freedreno.so        the driver
//       ...                           anything else the archive had beside the manifest
//
// Setting: [EmuCore/GS] LinuxDriverPack = <id>; empty or absent = the system driver. Global only,
// since the driver is per process, not per game.
//
// Threading: Manager methods block (network, disk, a test load of the library) and belong on a
// worker thread; two installs never run at once (the second returns InstallStatus::Busy).
// Callbacks run on the calling thread. The free functions are safe from any thread, except that
// ApplyEnvironment() must run before other threads start using Vulkan.
//
// Linux only; on other platforms the API compiles and reports "not available".

#include "Catalog/CancelToken.h"
#include "Catalog/Fetch.h"

#include "common/Pcsx2Types.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DriverPacks
{
	static constexpr const char* SETTING_SECTION = "EmuCore/GS";
	static constexpr const char* SETTING_KEY = "LinuxDriverPack";

	/// The selected pack id from the base settings; empty = system driver. Takes the settings
	/// lock: never call it while holding Host::GetSettingsLock().
	std::string GetSelectedPack();

	/// Stores the selection (empty clears it) and commits the base settings. Does not apply it:
	/// the new value takes effect at the next start (see RelaunchAtExit). Same lock rule.
	void SetSelectedPack(const std::string& id);

	/// "linux-aarch64", "linux-x86_64", ..., or empty where driver packs do not exist (Windows,
	/// macOS, Android, iOS).
	std::string CurrentPlatform();

	/// Best effort, readable before Vulkan is initialised: true if the kernel exposes a Qualcomm
	/// Adreno GPU, i.e. a DRM device bound to the `msm` driver, or a downstream `kgsl` device.
	/// `sysfs_root` exists for tests.
	bool IsAdrenoGpu(const std::string& sysfs_root = "/sys");

	struct Availability
	{
		enum class Reason
		{
			Available,
			UnsupportedPlatform, ///< not desktop Linux
			NoMatchingGpu, ///< packs are Turnip; this machine has no Adreno
		};

		Reason reason = Reason::UnsupportedPlatform;
		/// Ready to show: "Driver packs are not available on this platform." or
		/// "No driver packs for this GPU." (empty when available).
		std::string message;

		bool IsAvailable() const { return reason == Reason::Available; }
	};

	Availability GetAvailability(const std::string& sysfs_root = "/sys");

	/// Where packs come from. Feeds are data: adding a source is adding an entry here.
	struct Feed
	{
		/// Short stable token that starts every pack id from this feed, e.g. "armsx2turnip".
		std::string id_prefix;
		std::string label;
		/// GitHub releases API URL, e.g. https://api.github.com/repos/<owner>/<repo>/releases.
		std::string releases_url;
		/// Glob (`*` any run of characters, `?` one character) the whole asset file name must
		/// match, e.g. "turnip-*-aarch64.tar.gz". The checksum file is the asset of the same name
		/// plus ".sha256". (Not a regular expression: the build has exceptions off, and a bad
		/// std::regex pattern throws.)
		std::string asset_pattern;
		/// Which CurrentPlatform() this feed's assets are for.
		std::string platform;
	};

	/// Today: bmdhacks/armsx2-turnip, assets turnip-<tag>-aarch64.tar.gz for linux-aarch64.
	std::vector<Feed> DefaultFeeds();

	struct RemoteDriver
	{
		/// Install folder name and settings value: "<id_prefix>-<tag>", lowercased, anything
		/// outside [a-z0-9._-] replaced by '_'.
		std::string id;
		std::string feed_label;
		std::string tag; ///< release tag, e.g. "axfl2-002"
		std::string release_name;
		std::string published_at; ///< ISO 8601 as GitHub sends it
		bool prerelease = false;
		std::string asset_name;
		std::string download_url;
		u64 size = 0; ///< bytes, from the API
		/// URL of the "<asset>.sha256" asset; empty if the release has none.
		std::string checksum_url;
		/// Lowercase hex from the API's `"digest": "sha256:..."` field; empty if absent.
		std::string api_digest;
	};

	/// Parses a GitHub releases response for one feed. Drafts are skipped, assets that do not
	/// match the feed's pattern or have no https download URL are skipped, ids are unique (first
	/// wins), order is the API's (newest first). Returns false with `error` if the JSON is not a
	/// releases array.
	bool ParseReleases(const Feed& feed, std::string_view json, std::vector<RemoteDriver>* out, std::string* error);

	struct FeedError
	{
		std::string feed_label;
		std::string message;
	};

	struct RemoteListing
	{
		std::vector<RemoteDriver> drivers;
		/// One per feed that could not be read and had no cached copy to fall back on.
		std::vector<FeedError> errors;
		/// True if any feed's list came from an out-of-date cached copy because the network or
		/// GitHub (rate limit) failed.
		bool used_stale_cache = false;
	};

	struct InstalledDriver
	{
		std::string id;
		std::string name; ///< release title, or the file name for an imported archive
		std::string version; ///< release tag
		std::string feed_label; ///< empty for an imported archive
		std::string build_tag; ///< the pack's own build tag, e.g. "axfl2-002"
		/// The text this driver appends to Vulkan's driverInfo, e.g. "(git-axfl2-002)". It shows
		/// in the GS log line "VK: GPU ... driver='...' (Mesa <ver> (git-<tag>))", so a log proves
		/// which pack loaded. Empty if the pack did not say.
		std::string driver_info_token;
		std::string mesa_version;
		/// Window systems the build supports ("wayland"); a pack built for wayland only cannot
		/// present on an X11 session.
		std::string platforms;
		std::string archive_sha256;
		s64 installed_at = 0; ///< unix time

		std::string directory; ///< <drivers>/<id>
		std::string manifest_path; ///< what VK_DRIVER_FILES points at
		std::string library_path;
	};

	enum class InstallStage
	{
		Preparing,
		Downloading, ///< done/total are bytes of the archive
		Verifying,
		Extracting,
		Checking, ///< test-loading the driver library
		Finishing,
	};

	struct InstallProgress
	{
		InstallStage stage = InstallStage::Preparing;
		u64 done = 0;
		u64 total = 0; ///< 0 when unknown
	};

	using InstallProgressCallback = std::function<void(const InstallProgress&)>;

	enum class InstallStatus
	{
		Ok,
		Cancelled,
		Busy,
		Unsupported, ///< not Linux
		NoSpace,
		DownloadFailed,
		ChecksumMissing, ///< the release publishes no checksum; nothing was installed
		ChecksumMismatch,
		BadArchive, ///< not a .tar.gz, unsafe or oversized contents, no manifest or library
		LoadFailed,
		IoError,
	};

	struct InstallResult
	{
		InstallStatus status = InstallStatus::IoError;
		std::string id; ///< the installed pack on Ok
		/// A sentence for the user, written for display ("libwayland-client.so.0: cannot open
		/// shared object file", "Checksum mismatch: ..."). Empty on Ok.
		std::string message;

		bool Ok() const { return status == InstallStatus::Ok; }
		bool Cancelled() const { return status == InstallStatus::Cancelled; }
	};

	/// Loads `path` the way the Vulkan loader will (dlopen RTLD_NOW | RTLD_LOCAL), then unloads it,
	/// and reports why not if it fails (typically a shared library the system lacks).
	///
	/// The load happens in a forked child, never in this process: a driver's static constructors
	/// run at dlopen, and a crash or hang there must not take the emulator down. The child
	/// disables core dumps and crash handlers, reports through a pipe, and is killed after
	/// `timeout`. Returns false on a dlopen error, a crash (signal named in `error`), or a timeout.
	bool TestLoadLibrary(const std::string& path, std::string* error, std::chrono::milliseconds timeout = std::chrono::seconds(20));

	struct ManagerConfig
	{
		/// <DataRoot>/drivers. Created on first install.
		std::string drivers_dir;
		/// Where feed listings are cached (rate limits: GitHub allows 60 unauthenticated
		/// requests an hour). Empty disables caching.
		std::string cache_dir;
		std::vector<Feed> feeds = DefaultFeeds();
		std::string platform = CurrentPlatform();
		std::string sysfs_root = "/sys";
		/// A cached listing younger than this is used without asking the network.
		std::chrono::seconds listing_max_age = std::chrono::hours(1);
		/// Run on the library before a pack is accepted. Defaults to TestLoadLibrary; tests
		/// replace it.
		std::function<bool(const std::string& path, std::string* error)> library_check;

		/// drivers_dir and cache_dir under EmuFolders::DataRoot / EmuFolders::Cache.
		static ManagerConfig ForUser();
	};

	class Manager
	{
	public:
		Manager(Catalog::Fetcher& fetcher, ManagerConfig config);

		const ManagerConfig& Config() const { return m_config; }

		/// Whether packs make sense on this machine (platform, Adreno). The UI shows the
		/// message when not available; ListAvailable() makes no request in that case.
		Availability GetAvailability() const;

		/// Reads every feed for this platform. Uses the cached listing when it is younger than
		/// listing_max_age unless `force_refresh`; falls back to an older cached copy when the
		/// request fails. A feed that fails with nothing cached is reported in `errors` and the
		/// other feeds still list.
		RemoteListing ListAvailable(const Catalog::CancelToken& cancel, bool force_refresh = false);

		/// Installed packs, sorted by name then id. Folders that are not a complete pack (no
		/// meta.json, manifest or library) and install staging folders are not listed.
		std::vector<InstalledDriver> ListInstalled() const;
		std::optional<InstalledDriver> FindInstalled(std::string_view id) const;

		/// Download, verify, extract, test-load, commit. The checksum comes from the release's
		/// ".sha256" asset (or the API's digest when there is no such asset; if both exist and
		/// disagree, or neither exists, nothing is installed). Replaces an installed pack with
		/// the same id. On any failure the previous state is untouched and the staging folder
		/// is gone.
		InstallResult Install(const RemoteDriver& driver, const InstallProgressCallback& progress, const Catalog::CancelToken& cancel);

		/// Installs a .tar.gz the user already has (no download, no checksum to compare; its
		/// SHA-256 is recorded). The id is "local-<file name>", sanitised as in RemoteDriver::id.
		InstallResult InstallFromFile(const std::string& archive_path, const InstallProgressCallback& progress, const Catalog::CancelToken& cancel);

		/// Removes the pack's folder. Does not touch the setting: if GetSelectedPack() == id the
		/// caller clears it with SetSelectedPack({}). A pack deleted while this process is
		/// running with it keeps working until exit (the library is already mapped).
		bool Delete(std::string_view id, std::string* error = nullptr);

	private:
		struct Source;
		InstallResult InstallArchive(const std::string& archive_path, const std::string& stage_root, const std::string& id,
			const Source& source, const InstallProgressCallback& progress, const Catalog::CancelToken& cancel);

		Catalog::Fetcher& m_fetcher;
		ManagerConfig m_config;
		std::mutex m_install_mutex;
	};

	/// "<prefix>-<tag>" as an id (lowercased, [a-z0-9._-] only). Exposed for tests and UIs that
	/// need to match a RemoteDriver to an InstalledDriver.
	std::string MakeDriverId(std::string_view prefix, std::string_view tag);

	/// Plain folder name: [a-z0-9._-], 1 to 128 characters, not starting with '.'.
	bool IsValidDriverId(std::string_view id);

	std::string DriversDirectory();

	enum class ApplyOutcome
	{
		SystemDriver, ///< no pack selected; environment untouched
		Applied, ///< VK_DRIVER_FILES / VK_ICD_FILENAMES now point at the pack
		EnvironmentAlreadySet, ///< the launcher or user set one of them; a selection is ignored
		PackMissing, ///< selected but not usable on disk; falling back to the system driver
		GpuNotSupported, ///< selected but this machine has no Adreno; falling back
		Unsupported, ///< not Linux
	};

	struct ApplyResult
	{
		ApplyOutcome outcome = ApplyOutcome::SystemDriver;
		std::string pack_id;
		std::string manifest_path; ///< what the variables were set to (Applied)
		std::string message; ///< one log line, empty for SystemDriver / Unsupported
	};

	/// Call from main() right after the base settings are loaded and before anything can load
	/// libvulkan: it reads GetSelectedPack(), and if that pack is installed and this machine has
	/// an Adreno GPU it sets VK_DRIVER_FILES and VK_ICD_FILENAMES to the pack's manifest and
	/// clears VK_ADD_DRIVER_FILES (as the ROCKNIX launcher does, so no other driver can be
	/// picked up beside the pack).
	///
	/// It never overrides the user: if VK_DRIVER_FILES or VK_ICD_FILENAMES is already in the
	/// environment (ROCKNIX's launcher sets both) nothing is changed and the outcome says so. A
	/// selected pack that is missing or unusable falls back to the system driver and clears
	/// nothing. Logs one line to the console. Not thread safe against other threads reading the
	/// environment; call it before starting any.
	ApplyResult ApplyEnvironment();

	/// The same with explicit inputs, for tests and tools.
	ApplyResult ApplyEnvironment(const std::string& drivers_dir, const std::string& pack_id, const std::string& sysfs_root = "/sys");

	/// What the last ApplyEnvironment() in this process decided; for a settings page to explain
	/// "your launcher overrides this".
	ApplyResult GetLastApplyResult();

	/// Inputs to the first-run decision of whether to offer the recommended pack. Plain data, so
	/// the decision is testable.
	struct SetupContext
	{
		Availability availability;
		std::string selected_pack;
		/// VK_DRIVER_FILES or VK_ICD_FILENAMES is in the environment: a launcher (ROCKNIX's) picked
		/// the driver and a selection would be ignored. Read directly, because ApplyEnvironment()
		/// does not record this while nothing is selected.
		bool environment_sets_driver = false;
		size_t installed_count = 0;
		/// The packs present Wayland, so an X11 session cannot show one.
		bool session_is_x11 = false;
	};

	/// True when the setup should offer the recommended pack: this machine can use packs, the player
	/// has neither chosen nor installed one, and no launcher decides the driver.
	bool ShouldOfferInSetup(const SetupContext& context);

	/// Reads the context from this process: the manager's availability and installed packs, the
	/// selection in the base settings (takes the settings lock) and the environment.
	SetupContext GetSetupContext(const Manager& manager, bool session_is_x11);

	/// The pack the setup installs from a listing: the first release that is not a prerelease, else
	/// the first. Listings are newest first. Null for an empty list.
	const RemoteDriver* PickRecommended(const std::vector<RemoteDriver>& drivers);

	struct RelaunchPlan
	{
		/// File to execute. /proc/self/exe, or the image named by $APPIMAGE when running from an
		/// AppImage (the AppImage's own mount point disappears when this process exits).
		std::string executable;
		/// The original argv, argv[0] included (argv[0] is $APPIMAGE for an AppImage).
		std::vector<std::string> argv;
		/// Variables to remove from the new process's environment: the ones ApplyEnvironment()
		/// set in this process, so the restarted program re-reads the selection instead of
		/// treating our own earlier value as one the user chose.
		std::vector<std::string> unset_env;
	};

	/// Builds a plan from this process: /proc/self/cmdline, /proc/self/exe, $APPIMAGE, and what
	/// ApplyEnvironment() set. False with `error` on non-Linux or if /proc is unreadable.
	bool BuildRelaunchPlan(RelaunchPlan* plan, std::string* error);

	/// The pure part of BuildRelaunchPlan, for tests. `cmdline` is /proc/self/cmdline (NUL
	/// separated), `exe_link` the readlink of /proc/self/exe (a trailing " (deleted)", left when
	/// the binary was replaced on disk by an update, is stripped if the file exists without it),
	/// `appimage` the value of $APPIMAGE or empty.
	bool MakeRelaunchPlan(RelaunchPlan* plan, std::string_view cmdline, std::string_view exe_link, std::string_view appimage, std::string* error);

	/// Replaces this process with the plan: removes unset_env, closes every file descriptor above
	/// 2 (a leaked listener or DRM handle would stop the new instance from starting cleanly),
	/// flushes stdio, execve. Returns only on failure.
	bool ExecRelaunchPlan(const RelaunchPlan& plan, std::string* error);

	/// BuildRelaunchPlan + ExecRelaunchPlan. Returns only on failure. This does not shut anything
	/// down: the VM, threads and settings are the caller's to stop first.
	bool RelaunchNow(std::string* error);

	/// The same exit-handler mechanism with a plan of the caller's choosing.
	bool RelaunchAtExit(const RelaunchPlan& plan, std::string* error = nullptr);

	/// The one for "Apply & Restart": captures the plan now (so a failure to build it can be
	/// reported while the app is still open) and registers an exit handler that runs it. The UI
	/// then quits normally (stops the VM, saves settings, joins its threads); when main() returns,
	/// this process is replaced by a fresh one. A second call replaces the plan. Returns false
	/// without doing anything on non-Linux.
	bool RelaunchAtExit(std::string* error = nullptr);
} // namespace DriverPacks
