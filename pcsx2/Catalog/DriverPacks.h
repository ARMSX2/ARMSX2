// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Downloadable GPU driver packs for desktop Linux (Turnip on Adreno, malisx2 on Arm Mali GPUs
// driven by Arm's kbase kernel driver): list a feed's packs, install a pack, list and delete
// installed packs, and point the process at the selected pack before Vulkan starts.
//
// Every feed and every installed pack is for one GPU family (GpuFamily). A feed is listed, and a
// selected pack applied, only on a machine that has that family's GPU, detected from sysfs and
// /dev at run time (ROCKNIX can switch a Mali board between kbase and panfrost/panthor).
//
// The Vulkan loader reads VK_DRIVER_FILES / VK_ICD_FILENAMES once, when libvulkan is first used
// in the process. So ApplyEnvironment() must set them in main() before any GS or Vulkan code
// runs, and changing the selection needs a restart (RelaunchAtExit). Nothing is installed
// system-wide.
//
// On disk, per installed pack:
//
//   <DataRoot>/drivers/<id>/
//       meta.json                     name, version, build tag, GPU family ("gpu"), ...
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

	/// The kbase product id (arch major, arch minor, arch revision, product major; 4 bits each, as
	/// malisx2 and Mesa use it, e.g. 0x7402 for a Mali-G52 r1) from the hex number at the end of
	/// kbase's `gpuinfo` sysfs file. kbase versions print one of:
	///   - the 16-bit product id itself ("0x7402", "0xB8A3");
	///   - the whole 32-bit GPU_ID register, product id in the top half ("0xA8670005");
	///   - newer kbase: 0xAAmmRRPP, arch major, arch minor, arch revision and product major one byte
	///     each ("0x07040002").
	/// Nothing if the line has no such number or it does not fit any of these.
	std::optional<u16> KbaseProductIdFromGpuInfo(std::string_view gpuinfo);

	/// Which scheduler the kbase kernel driver was built with: the job manager (Bifrost and
	/// first-generation Valhall, arches 6 to 9) or the command-stream frontend (arch 10 and later).
	enum class KbaseFrontend
	{
		Unknown, ///< sysfs does not say; decide by the arch alone
		JobManager,
		CommandStream,
	};

	/// Read from the kbase device's sysfs attributes: a job-manager kbase has js_timeouts and
	/// js_scheduling_period, a command-stream one csg_scheduling_period. Unknown when neither is
	/// there (no kbase, or a vendor kernel that dropped them).
	KbaseFrontend KbaseFrontendFromSysfs(const std::string& sysfs_root = "/sys");

	/// malisx2's rule (mali_device_name and the frontend check at physical-device creation): every
	/// GPU of arch 7 or 9 on a job-manager kbase, and of arch 10 or 11 on a command-stream kbase.
	/// With KbaseFrontend::Unknown the arch alone decides. Arch 6 (G71, G72), Midgard and arch 12
	/// and later are not malisx2's.
	bool IsMalisx2Arch(unsigned arch_major, KbaseFrontend frontend = KbaseFrontend::Unknown);

	/// IsMalisx2Arch on the product id's arch major (its top four bits).
	bool IsMalisx2ProductId(u16 product_id, KbaseFrontend frontend = KbaseFrontend::Unknown);

	/// The decision for one `gpuinfo` line ("Mali-G52 1 cores r1p0 0x07040002"): by the product id
	/// when the hex number reads, else by the arch of the leading name ("Mali-G610"). The hex wins
	/// because kbase prints names it does not know as "(Unknown Mali GPU)" or a code name.
	bool IsMalisx2GpuInfo(std::string_view gpuinfo, KbaseFrontend frontend = KbaseFrontend::Unknown);

	/// Best effort, readable before Vulkan is initialised: true if this machine runs Arm's kbase
	/// kernel driver (`<dev_root>/mali0` exists) and its GPU (`<sysfs_root>/class/misc/mali0/device/
	/// gpuinfo`) is one malisx2 supports on this kbase's frontend (KbaseFrontendFromSysfs). A Mali on
	/// panfrost or panthor has neither file, and malisx2 cannot run there. The roots exist for tests.
	bool IsSupportedKbaseMaliGpu(const std::string& sysfs_root = "/sys", const std::string& dev_root = "/dev");

	/// Which GPU a feed's packs, or an installed pack, are built for.
	enum class GpuFamily
	{
		Unknown, ///< a pack whose metadata names a family this build does not know; never applied
		Adreno, ///< Turnip; Qualcomm Adreno on msm or kgsl
		MaliKbase, ///< malisx2; the Arm Mali GPUs IsSupportedKbaseMaliGpu accepts
	};

	/// "adreno", "mali-kbase", "unknown": the value stored in meta.json and read from a pack's
	/// MANIFEST.json "gpu" key.
	const char* GpuFamilyKey(GpuFamily family);
	/// The inverse of GpuFamilyKey (case-insensitive); an empty string is Adreno, since packs and
	/// metadata written before families existed are Turnip. Anything else unrecognised is Unknown.
	GpuFamily ParseGpuFamily(std::string_view key);
	/// For messages: "Qualcomm Adreno GPU", "Arm Mali GPU that malisx2 supports", "GPU this version of
	/// ARMSX2 does not know".
	const char* GpuFamilyDescription(GpuFamily family);

	/// Whether this machine has a GPU of `family` (IsAdrenoGpu, IsSupportedKbaseMaliGpu).
	bool MachineHasGpuFamily(GpuFamily family, const std::string& sysfs_root = "/sys", const std::string& dev_root = "/dev");

	struct Availability
	{
		enum class Reason
		{
			Available,
			UnsupportedPlatform, ///< not desktop Linux
			NoMatchingGpu, ///< no feed for this platform is for this machine's GPU
		};

		Reason reason = Reason::UnsupportedPlatform;
		/// The family of the first feed this machine matches; Unknown when not available.
		GpuFamily gpu = GpuFamily::Unknown;
		/// Ready to show: "Driver packs are not available on this platform." or
		/// "No driver packs for this GPU." (empty when available).
		std::string message;

		bool IsAvailable() const { return reason == Reason::Available; }
	};

	/// Against DefaultFeeds() and CurrentPlatform().
	Availability GetAvailability(const std::string& sysfs_root = "/sys", const std::string& dev_root = "/dev");

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
		/// Which GPU its packs drive. The feed is read only on a machine that has one.
		GpuFamily gpu = GpuFamily::Adreno;
	};

	/// bmdhacks/armsx2-turnip (assets turnip-<tag>-aarch64.tar.gz, Adreno) and bmdhacks/malisx2
	/// (assets malisx2-<semver>-aarch64.tar.gz, Mali on kbase), both for linux-aarch64.
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
		GpuFamily gpu = GpuFamily::Adreno; ///< the feed's
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
		/// meta.json "gpu"; a pack installed before families were recorded has none and is Adreno.
		GpuFamily gpu = GpuFamily::Adreno;

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
		std::string dev_root = "/dev";
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

		/// Whether packs make sense on this machine: a feed for this platform whose GPU family the
		/// machine has. The UI shows the message when not available; ListAvailable() makes no
		/// request in that case.
		Availability GetAvailability() const;

		/// Reads every feed for this platform whose GPU family the machine has. Uses the cached listing when it is younger than
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
		/// The GPU family comes from the pack's MANIFEST.json "gpu" key, else from the library's
		/// name (libvulkan_malisx2.so is Mali on kbase), else Adreno.
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
		GpuNotSupported, ///< selected, but this machine has no GPU of the pack's family; falling back
		Unsupported, ///< not Linux
	};

	struct ApplyResult
	{
		ApplyOutcome outcome = ApplyOutcome::SystemDriver;
		std::string pack_id;
		std::string manifest_path; ///< what the variables were set to (Applied)
		std::string message; ///< one log line, empty for SystemDriver / Unsupported
		/// The selected pack's GPU family, once its metadata was read (Applied, GpuNotSupported).
		GpuFamily gpu = GpuFamily::Unknown;
	};

	/// Call from main() right after the base settings are loaded and before anything can load
	/// libvulkan: it reads GetSelectedPack(), and if that pack is installed and this machine has
	/// a GPU of the pack's family it sets VK_DRIVER_FILES and VK_ICD_FILENAMES to the pack's manifest and
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
	ApplyResult ApplyEnvironment(const std::string& drivers_dir, const std::string& pack_id, const std::string& sysfs_root = "/sys",
		const std::string& dev_root = "/dev");

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
		/// Turnip packs present on Wayland, so an X11 session cannot show one.
		bool session_is_x11 = false;
		/// malisx2 packs present only on a display of their own (VK_KHR_display) or headless; under
		/// a Wayland compositor only through Arm's separate WSI layer, which may not be installed.
		bool session_is_wayland = false;
	};

	/// True when the setup should offer the recommended pack: this machine can use packs, the player
	/// has neither chosen nor installed one, no launcher decides the driver, and the packs for this
	/// machine's GPU (availability.gpu) can show a picture in this session: Turnip outside X11,
	/// malisx2 outside both X11 and Wayland.
	bool ShouldOfferInSetup(const SetupContext& context);

	/// Reads the context from this process: the manager's availability and installed packs, the
	/// selection in the base settings (takes the settings lock) and the environment.
	SetupContext GetSetupContext(const Manager& manager, bool session_is_x11, bool session_is_wayland = false);

	/// The pack the setup installs from a listing: the first release that is not a prerelease, else
	/// the first. Listings are newest first. With `gpu` other than Unknown, only that family's packs
	/// count (a listing holds only the families the machine has, so the setup passes
	/// Availability::gpu to stay on one feed). Null when nothing qualifies.
	const RemoteDriver* PickRecommended(const std::vector<RemoteDriver>& drivers, GpuFamily gpu = GpuFamily::Unknown);

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
