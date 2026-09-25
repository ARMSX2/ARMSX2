// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Mobile GPU driver-bug database, ported from EmuCoreX (sashkinbro) with his approval.
// Adapted only where our enums differ; the rule table itself is his work.

#include "GS/Renderers/Common/GSGPUProfilePrivate.h"

#include <array>
#include <cctype>
#include <limits>

namespace GpuProfileDetail
{
namespace
{
// VkDriverId values, duplicated so this file needs no Vulkan headers and serves the GL tests.
constexpr u32 DRIVER_ID_IMAGINATION_PROPRIETARY = 7;
constexpr u32 DRIVER_ID_QUALCOMM_PROPRIETARY = 8;
constexpr u32 DRIVER_ID_ARM_PROPRIETARY = 9;
constexpr u32 DRIVER_ID_MESA_TURNIP = 18;
constexpr u32 DRIVER_ID_MESA_PANVK = 20;
constexpr u32 DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA = 25;
constexpr u32 DRIVER_ID_MESA_HONEYKRISP = 26;

constexpr u64 Bug(DriverBug bug)
{
	return u64{1} << static_cast<u8>(bug);
}

constexpr u64 Workaround(DriverWorkaround workaround)
{
	return u64{1} << static_cast<u8>(workaround);
}

struct VersionBound
{
	u16 major = 0;
	u16 minor = 0;
	u16 patch = 0;
	u32 build = 0;
};

struct DriverRule
{
	const char* id;
	MobileGpuApi api = MobileGpuApi::Unknown;
	RuntimeGpuProfile vendor = RuntimeGpuProfile::Unknown;
	MobileGpuDriver driver = MobileGpuDriver::Unknown;
	MobileGpuArchitecture architecture = MobileGpuArchitecture::Unknown;
	u16 model_min = 0;
	u16 model_max = 0;
	u32 exact_raw_version = 0;
	VersionBound min_version;
	VersionBound max_version_exclusive;
	u32 min_android_sdk = 0;
	u32 max_android_sdk = 0;
	bool match_unknown_version = false;
	u64 bugs = 0;
	u64 workarounds = 0;
	/// Match only on a MediaTek SoC (Dimensity/Helio, or a bare mtNNNN part number).
	bool mediatek_soc_only = false;
	/// Lowercase substring that, when present in the hints (SoC and board identity), makes this
	/// rule NOT match. Used to exempt one measured part from a family-wide rule. One-directional:
	/// a device can narrow a rule but never widen it.
	const char* hint_exclude = nullptr;
	/// Lowercase substring that must be present in the hints for this rule to match. Only for a
	/// preference about one measured part; a defect should use a version or model bound instead.
	const char* hint_require = nullptr;
};

/// The SoC hint for the Anbernic RG 477V in the spelling Android ("mt6897") and the Linux
/// device tree ("mediatek,mt6897") share. Three rules key on it: the two destination-read
/// exemptions and the Vulkan preference, which depends on them. One constant keeps them in step.
constexpr const char* MEASURED_SOC_MT6897 = "mt6897";

constexpr int CompareVersion(const MobileDriverVersion& lhs, VersionBound rhs)
{
	if (lhs.major != rhs.major)
		return (lhs.major < rhs.major) ? -1 : 1;
	if (lhs.minor != rhs.minor)
		return (lhs.minor < rhs.minor) ? -1 : 1;
	if (lhs.patch != rhs.patch)
		return (lhs.patch < rhs.patch) ? -1 : 1;
	if (lhs.build != rhs.build)
		return (lhs.build < rhs.build) ? -1 : 1;
	return 0;
}

constexpr bool HasVersionBound(VersionBound bound)
{
	return bound.major != 0 || bound.minor != 0 || bound.patch != 0 || bound.build != 0;
}

static bool ParseUnsigned(std::string_view text, size_t start, u16* value, size_t* end)
{
	if (start >= text.size() || !std::isdigit(static_cast<unsigned char>(text[start])))
		return false;

	u32 parsed = 0;
	size_t pos = start;
	while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
	{
		parsed = parsed * 10 + static_cast<u32>(text[pos++] - '0');
		if (parsed > std::numeric_limits<u16>::max())
			return false;
	}

	*value = static_cast<u16>(parsed);
	*end = pos;
	return true;
}

static bool ParseUnsigned32(std::string_view text, size_t start, u32* value, size_t* end)
{
	if (start >= text.size() || !std::isdigit(static_cast<unsigned char>(text[start])))
		return false;

	u64 parsed = 0;
	size_t pos = start;
	while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
	{
		parsed = parsed * 10 + static_cast<u32>(text[pos++] - '0');
		if (parsed > std::numeric_limits<u32>::max())
			return false;
	}

	*value = static_cast<u32>(parsed);
	*end = pos;
	return true;
}

static MobileDriverVersion ParseOpenGLDriverVersion(std::string_view version_string,
	RuntimeGpuProfile vendor)
{
	MobileDriverVersion version;
	const std::string lowered = ToLowerASCII(version_string);

	// ARM strings commonly contain "r54p1"; this is the only portion that has stable ordering.
	if (vendor == RuntimeGpuProfile::Mali)
	{
		for (size_t r = lowered.find('r'); r != std::string::npos; r = lowered.find('r', r + 1))
		{
			size_t end = r + 1;
			u16 release = 0;
			if (!ParseUnsigned(lowered, r + 1, &release, &end))
				continue;

			u16 patch = 0;
			if (end < lowered.size() && lowered[end] == 'p')
			{
				size_t patch_end = end + 1;
				ParseUnsigned(lowered, end + 1, &patch, &patch_end);
			}

			version.major = release;
			version.minor = patch;
			version.known = true;
			return version;
		}
	}

	// Imagination strings use the form "OpenGL ES 3.2 build 1.9@4850625". The branch and
	// change ID, rather than the leading GLES version, are the ordered driver identity.
	if (vendor == RuntimeGpuProfile::PowerVR)
	{
		const size_t marker = lowered.find("build ");
		if (marker == std::string::npos)
			return version;

		size_t end = marker + 6;
		u16 major = 0;
		if (!ParseUnsigned(lowered, end, &major, &end) || end >= lowered.size() || lowered[end] != '.')
			return version;

		u16 minor = 0;
		if (!ParseUnsigned(lowered, end + 1, &minor, &end))
			return version;

		u32 build = 0;
		if (end < lowered.size() && lowered[end] == '@')
		{
			size_t build_end = end + 1;
			if (!ParseUnsigned32(lowered, end + 1, &build, &build_end))
				return version;
		}

		version.major = major;
		version.minor = minor;
		version.build = build;
		version.known = true;
		return version;
	}

	// Qualcomm strings commonly contain "V@0502".
	size_t start = 0;
	if (vendor == RuntimeGpuProfile::Adreno)
	{
		const size_t marker = lowered.find("v@");
		if (marker != std::string::npos)
			start = marker + 2;
	}

	for (size_t pos = start; pos < lowered.size(); pos++)
	{
		u16 major = 0;
		size_t end = pos;
		if (!ParseUnsigned(lowered, pos, &major, &end))
			continue;

		u16 minor = 0;
		u16 patch = 0;
		if (end < lowered.size() && lowered[end] == '.')
		{
			size_t minor_end = end + 1;
			ParseUnsigned(lowered, end + 1, &minor, &minor_end);
			if (minor_end < lowered.size() && lowered[minor_end] == '.')
			{
				size_t patch_end = minor_end + 1;
				ParseUnsigned(lowered, minor_end + 1, &patch, &patch_end);
			}
		}

		version.major = major;
		version.minor = minor;
		version.patch = patch;
		version.known = true;
		return version;
	}

	return version;
}

static MobileDriverVersion ParseVulkanDriverVersion(const MobileDriverContext& context,
	MobileGpuDriver driver)
{
	MobileDriverVersion version;
	version.raw = context.driver_version;
	if (context.driver_version == 0)
		return version;

	const u32 raw = context.driver_version;
	version.major = static_cast<u16>(raw >> 22);
	version.minor = static_cast<u16>((raw >> 12) & 0x3ff);
	version.patch = static_cast<u16>(raw & 0xfff);

	if (driver == MobileGpuDriver::QualcommProprietary && (raw & 0x80000000u) == 0)
	{
		// Older Qualcomm releases used an undocumented, non-orderable encoding.
		version.known = false;
		version.major = 0;
		version.minor = 0;
		version.patch = 0;
		return version;
	}

	if (driver == MobileGpuDriver::ArmProprietary &&
		(version.patch != 0 || version.major > 100))
	{
		// Old Mali Vulkan releases placed a source hash in driverVersion.
		version.known = false;
		version.legacy_hash = true;
		version.major = 0;
		version.minor = 0;
		version.patch = 0;
		return version;
	}

	version.known = true;
	return version;
}

// Our own Turnip builds identify themselves in driverInfo. Mesa appends MESA_GIT_SHA1_OVERRIDE
// to the version, so a build tagged `axfl1-005` reports "Mesa 26.1.2 (git-axfl1-005)". A tag
// `axfl<G>-` means the build carries generation <G> (decimal, from 1) of the declared-feedback-loop
// ordering fix. Older tags such as `armsx2-001` deliberately do not match; some lack the fix.
//
// A false positive would drop the barriers that keep other drivers correct, so the parse is
// strict: token boundary before `git-`, at least one digit, nonzero value, trailing hyphen.
// Release-tarball distro drivers carry no git sha and cannot match.
static u32 ParseFixGeneration(std::string_view driver_info)
{
	constexpr std::string_view TAG_PREFIX = "git-axfl";
	// Bounds the accumulator on strings that are not a tag.
	constexpr size_t MAX_DIGITS = 4;

	const std::string lowered = ToLowerASCII(driver_info);
	for (size_t at = lowered.find(TAG_PREFIX); at != std::string::npos;
		at = lowered.find(TAG_PREFIX, at + 1))
	{
		if (at > 0 && std::isalnum(static_cast<unsigned char>(lowered[at - 1])))
			continue;

		size_t pos = at + TAG_PREFIX.size();
		u32 generation = 0;
		size_t digits = 0;
		while (pos < lowered.size() && digits < MAX_DIGITS &&
			   std::isdigit(static_cast<unsigned char>(lowered[pos])))
		{
			generation = generation * 10 + static_cast<u32>(lowered[pos++] - '0');
			digits++;
		}

		if (digits == 0 || generation == 0 || pos >= lowered.size() || lowered[pos] != '-')
			continue;

		return generation;
	}

	return 0;
}

static MobileGpuDriver DetectDriver(const GpuProfileSelection& selection,
	const MobileDriverContext& context, std::string_view lowered_hints)
{
	switch (context.driver_id)
	{
		case DRIVER_ID_ARM_PROPRIETARY: return MobileGpuDriver::ArmProprietary;
		case DRIVER_ID_MESA_PANVK: return MobileGpuDriver::MesaPanVK;
		case DRIVER_ID_QUALCOMM_PROPRIETARY: return MobileGpuDriver::QualcommProprietary;
		case DRIVER_ID_MESA_TURNIP: return MobileGpuDriver::MesaTurnip;
		case DRIVER_ID_IMAGINATION_PROPRIETARY: return MobileGpuDriver::ImaginationProprietary;
		case DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA: return MobileGpuDriver::MesaPowerVR;
		default: break;
	}

	const std::string driver_hints = ToLowerASCII(
		std::string(context.driver_name) + " " + std::string(context.driver_info) + " " +
		std::string(lowered_hints));
	if (ContainsAny(driver_hints, {"turnip", "freedreno"}))
		return MobileGpuDriver::MesaTurnip;
	if (ContainsAny(driver_hints, {"panvk", "panfrost"}))
		return MobileGpuDriver::MesaPanVK;
	if (ContainsAny(driver_hints, {"pvr mesa", "powervr mesa"}))
		return MobileGpuDriver::MesaPowerVR;
	if (ContainsAny(driver_hints, {"angle"}))
		return MobileGpuDriver::Angle;

	// With no explicit driver ID, Vulkan and native GLES vendor IDs identify the proprietary stack.
	switch (selection.runtime_profile)
	{
		case RuntimeGpuProfile::Mali:
			return MobileGpuDriver::ArmProprietary;
		case RuntimeGpuProfile::Adreno:
			return MobileGpuDriver::QualcommProprietary;
		case RuntimeGpuProfile::PowerVR:
			return MobileGpuDriver::ImaginationProprietary;
		default:
			return MobileGpuDriver::Unknown;
	}
}

static bool RuleMatches(const DriverRule& rule, const GpuProfileSelection& selection,
	const MobileDriverContext& context, const MobileDriverProfile& profile,
	std::string_view lowered_hints)
{
	if (rule.hint_exclude != nullptr && lowered_hints.find(rule.hint_exclude) != std::string_view::npos)
		return false;
	if (rule.hint_require != nullptr && lowered_hints.find(rule.hint_require) == std::string_view::npos)
		return false;
	if (rule.mediatek_soc_only && !selection.is_mediatek_soc)
		return false;
	if (rule.api != MobileGpuApi::Unknown && rule.api != profile.api)
		return false;
	if (rule.vendor != RuntimeGpuProfile::Unknown && rule.vendor != selection.runtime_profile)
		return false;
	if (rule.driver != MobileGpuDriver::Unknown && rule.driver != profile.driver)
		return false;
	if (rule.architecture != MobileGpuArchitecture::Unknown &&
		rule.architecture != selection.gpu.architecture)
	{
		return false;
	}
	if ((rule.model_min != 0 || rule.model_max != 0) &&
		(selection.gpu.model_number < rule.model_min || selection.gpu.model_number > rule.model_max))
	{
		return false;
	}
	if (rule.exact_raw_version != 0 && profile.version.raw != rule.exact_raw_version)
		return false;
	if (rule.min_android_sdk != 0 && context.android_sdk < rule.min_android_sdk)
		return false;
	if (rule.max_android_sdk != 0 && context.android_sdk > rule.max_android_sdk)
		return false;

	const bool has_version_range =
		HasVersionBound(rule.min_version) || HasVersionBound(rule.max_version_exclusive);
	if (has_version_range && !profile.version.known)
		return rule.match_unknown_version;
	if (HasVersionBound(rule.min_version) && CompareVersion(profile.version, rule.min_version) < 0)
		return false;
	if (HasVersionBound(rule.max_version_exclusive) &&
		CompareVersion(profile.version, rule.max_version_exclusive) >= 0)
	{
		return false;
	}
	return true;
}

// Sources and upstream revisions are mirrored in docs/gpu-driver-database.json. A known bug is
// not automatically an active workaround: expensive fallbacks stay off until they have a bounded,
// tested condition.
static constexpr std::array<DriverRule, 35> s_driver_rules = {{
	{"gl-arm-buffer-stream", MobileGpuApi::OpenGL, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenBufferStreaming) | Bug(DriverBug::BrokenUnsynchronizedMapping) |
			Bug(DriverBug::BrokenVectorBitwiseAnd) | Bug(DriverBug::BrokenVSync),
		Workaround(DriverWorkaround::ScalarizeVectorBitwiseAnd)},
	{"gl-arm-g57-fifo", MobileGpuApi::OpenGL, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 57, 57, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenVSync), Workaround(DriverWorkaround::ForceFifoPresent)},
	// Deliberately NO GL rule for r44p1 in-tile render-target self-read. Do not "complete the
	// pair" with the Vulkan rule below. On GLES, fetch and texture barrier are one capability, so
	// gating it turns every self-referential draw into an RT copy plus a tile flush, which made
	// games unplayable (Shadow of the Colossus 30 -> 7 fps on an RG 477V). The fetch path does
	// corrupt some content on this driver (MGS3); users who need correct output use Vulkan, where
	// the RT copy is an ordinary image copy. A rule re-added here also flips Auto to Vulkan via
	// GSUtil::AndroidAutoPrefersVulkan, which reads this table.
	//
	// MT6897 (Dimensity 8300, Mali-G615 MC6) keeps its working GL fetch path, but Auto is sent to
	// Vulkan because its exemption from the destination-read denies below lets Vulkan read the
	// target in tile memory, which measured faster. A preference, not a defect: no bug bit. Stated
	// as a rule so it keys on the same SoC hint as the exemptions.
	{"gl-mt6897-prefer-vulkan", MobileGpuApi::OpenGL, RuntimeGpuProfile::Mali,
		MobileGpuDriver::Unknown, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		0, Workaround(DriverWorkaround::PreferVulkanRenderer), false, nullptr, MEASURED_SOC_MT6897},
	{"gl-qualcomm-compiler", MobileGpuApi::OpenGL, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::QualcommProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenBufferStreaming) | Bug(DriverBug::BrokenNegatedBoolean) |
			Bug(DriverBug::BrokenPrimitiveRestart),
		Workaround(DriverWorkaround::RewriteBooleanNegation)},
	{"gl-powervr-driver", MobileGpuApi::OpenGL, RuntimeGpuProfile::PowerVR,
		MobileGpuDriver::ImaginationProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenBufferStreaming), 0},
	{"gl-powervr-bitwise-before-1-8-4693462", MobileGpuApi::OpenGL, RuntimeGpuProfile::PowerVR,
		MobileGpuDriver::ImaginationProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0,
		{}, {1, 8, 0, 4693462}, 0, 0, true,
		Bug(DriverBug::BrokenBitwiseOpNegation),
		Workaround(DriverWorkaround::StoreBitwiseNegationInTemporary)},
	// Upstream keys this on a PowerVR Series5 architecture we do not carry; the 500-599 model
	// range selects SGX 5xx just as narrowly.
	{"gl-powervr-sgx-tall-mipmap", MobileGpuApi::OpenGL, RuntimeGpuProfile::PowerVR,
		MobileGpuDriver::ImaginationProprietary, MobileGpuArchitecture::PowerVR, 500, 599, 0,
		{}, {}, 0, 0, false, Bug(DriverBug::BrokenGenerateMipmapTallTexture),
		Workaround(DriverWorkaround::GenerateMipmapManuallyForTallTextures)},
	{"gl-android-shader-serialization", MobileGpuApi::OpenGL, RuntimeGpuProfile::Unknown,
		MobileGpuDriver::Unknown, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 1, 0, false,
		Bug(DriverBug::BrokenMultithreadedShaderCompilation), 0},
	{"vk-android-shader-serialization", MobileGpuApi::Vulkan, RuntimeGpuProfile::Unknown,
		MobileGpuDriver::Unknown, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 1, 0, false,
		Bug(DriverBug::BrokenMultithreadedShaderCompilation), 0},
	{"vk-arm-proprietary", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenPrimitiveRestart) | Bug(DriverBug::BrokenPushDescriptors) |
			Bug(DriverBug::BrokenAttachmentFeedbackLoopLayout) | Bug(DriverBug::BrokenVectorBitwiseAnd),
		Workaround(DriverWorkaround::UseDescriptorSets) |
			Workaround(DriverWorkaround::DisableAttachmentFeedbackLoopLayout) |
			Workaround(DriverWorkaround::ScalarizeVectorBitwiseAnd)},
	// Slow cached readback (from Dolphin's BUG_SLOW_CACHED_READBACK_MEMORY) measured backwards on
	// r44p1 / Mali-G615: a non-coherent cached type with explicit invalidates beat the coherent
	// map about 12x per readback. That does not prove it wrong on older parts, so the preference
	// is split around exactly [r44p1, r44p2), the measured revision; everything else keeps the
	// coherent preference.
	{"vk-arm-slow-cached-readback-before-r44p1", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {44, 1, 0},
		0, 0, true, Bug(DriverBug::SlowCachedReadbackMemory),
		Workaround(DriverWorkaround::PreferCoherentReadback)},
	{"vk-arm-slow-cached-readback-after-r44p1", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {44, 2, 0}, {},
		0, 0, false, Bug(DriverBug::SlowCachedReadbackMemory),
		Workaround(DriverWorkaround::PreferCoherentReadback)},
	{"vk-arm-empty-renderpass", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0xaa9c4b29u, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenEmptyRenderPass), 0},
	{"vk-arm-constant-load-r32-r39", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {32, 0, 0}, {40, 0, 0},
		0, 0, false, Bug(DriverBug::BrokenConstantLoad), 0},
	{"vk-arm-midgard-uniform-indexing", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::MaliMidgard, 830, 880, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenUniformIndexing), Workaround(DriverWorkaround::RewriteUniformIndexing)},
	{"vk-arm-imageless-r38", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {38, 0, 0}, {38, 2, 0},
		0, 0, false, Bug(DriverBug::BrokenImagelessFramebuffer), 0},
	{"vk-arm-extended-dynamic-before-r44p1", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {44, 1, 0},
		0, 0, true, Bug(DriverBug::BrokenExtendedDynamicState), 0},
	{"vk-arm-dynamic-rendering-before-r52", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {52, 0, 0},
		0, 0, true, Bug(DriverBug::BrokenDynamicRendering), 0},
	// r44p1 on Vulkan loses the device (VK_ERROR_DEVICE_LOST at vkWaitForFences) under the
	// in-tile self-read: a crash gate, not a perf trade. Disabling only the feedback-loop layout is
	// not enough (r44p1 does not advertise VK_EXT_attachment_feedback_loop_layout); the fatal read
	// is the ROAA/barrier one, and only reading a separate copy survives.
	//
	// MT6897 is exempt on measurement: its r44p1 runs the in-tile read with no device loss or stale
	// content, where the founding report (Motorola Edge 60 Pro, also r44p1) crashed on nearly every
	// game. No version bound separates the two blobs, so the exemption is per SoC.
	{"vk-arm-r44p1-attachment-self-read", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {44, 1, 0}, {44, 2, 0},
		0, 0, false,
		Bug(DriverBug::BrokenSubpassFeedback) | Bug(DriverBug::BrokenAttachmentFeedbackLoopLayout),
		Workaround(DriverWorkaround::UseRenderTargetCopyForFeedback), false, MEASURED_SOC_MT6897},
	// ROAA destination-read deny list. These parts advertise rasterization-order attachment
	// access and return zero or stale destination colour through it (black or missing textures,
	// not a crash), so the renderer uses the per-primitive texture-barrier path instead.
	//
	// The MediaTek rule is a vendor-wide guess (from sashkinbro/EmuCoreX), not a per-driver fact,
	// and it is costly where wrong: Mali reports dualSrcBlend=false, so every SRC1 draw is
	// software-blended and needs a barrier per primitive. EmuCore/GS/ForceMaliFramebufferFetch lets
	// a user on another MediaTek part lift it. MT6897 is exempt: measured, and the read is correct.
	{"vk-mediatek-mali-roaa-destination-read", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::Unknown, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenRoaaDestinationRead), 0, true, MEASURED_SOC_MT6897},
	// Mali-G57 across SoC vendors, so keyed on the model rather than the SoC.
	{"vk-arm-g57-roaa-destination-read", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::Unknown, MobileGpuArchitecture::Unknown, 57, 57, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenRoaaDestinationRead), 0},
	{"vk-qualcomm-proprietary", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::QualcommProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenPrimitiveRestart) | Bug(DriverBug::BrokenProvokingVertex) |
			Bug(DriverBug::BrokenSubpassFeedback) |
			Bug(DriverBug::BrokenAttachmentFeedbackLoopLayout) |
			Bug(DriverBug::BrokenReversedDepthRange) | Bug(DriverBug::SlowCachedReadbackMemory) |
			Bug(DriverBug::SlowOptimalImageToBufferCopy),
		Workaround(DriverWorkaround::DisableProvokingVertex) |
			Workaround(DriverWorkaround::PreferCoherentReadback) |
			Workaround(DriverWorkaround::UseRenderTargetCopyForFeedback)},
	// Turnip's first HOST_COHERENT type is write-combined, so VKStreamBuffer's rings land there.
	// On Adreno 610 taking the cached non-coherent type plus a clean per commit cut GS-thread time
	// by 20-30% on streaming-heavy titles, output unchanged.
	//
	// Measured on one part only, and must not be widened by memory-table shape: on Mali-G615
	// (MT6897), which also lacks a cached coherent type, the same trade lost on every title. Whether
	// the clean is cheaper than write-combined stores is a CPU/cache property, not visible in the
	// memory table. Other low-tier Adreno 6xx are candidates to measure, not to include. Adreno 650
	// offers a cached coherent type and lost on it too. Turnip only: the proprietary driver's memory
	// table has not been examined.
	{"vk-turnip-a610-cached-stream-rings", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::MesaTurnip, MobileGpuArchitecture::Unknown, 610, 610, 0, {}, {}, 0, 0, false,
		0, Workaround(DriverWorkaround::PreferCachedStreamRingMemory)},
	// Turnip does not share the Qualcomm driver's other defects but has the same broken
	// render-target self-read, so it needs its own rule (vk-qualcomm-proprietary keys on the
	// proprietary driver). Issue #442: with an HD texture pack, Tales of the Abyss loses its 2D text
	// once the RT self-read engages. On Turnip + Adreno 650 both in-pass forms (subpassLoad input
	// attachment and feedback-loop-layout texelFetch) drop the content; a separate RT copy is
	// correct. Hence both bug bits and the copy workaround.
	//
	// Model bounds are 0/0, so this covers every Adreno on Turnip, though the evidence is Adreno 650
	// only; narrowing it would have no better evidence. Where a part has been measured on the
	// declared feedback loop, orders_declared_feedback_loop / prefers_declared_loop_with_barriers
	// outrank this rule in GSSelfReadRoadPolicy.h, because the copy road renders wrong there.
	{"vk-turnip-attachment-self-read", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::MesaTurnip, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenSubpassFeedback) | Bug(DriverBug::BrokenAttachmentFeedbackLoopLayout),
		Workaround(DriverWorkaround::UseRenderTargetCopyForFeedback)},
	// Turnip sometimes applies VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR as if the blend constant
	// were zero, while ONE_MINUS_SRC1_COLOR in the same pass is correct. Visible as Katamari
	// Damacy's ball saturating to white. The submitted state is correct (confirmed in a capture);
	// re-emitting the constant before each draw changes nothing, and identical draw streams replay
	// right or wrong depending on run history.
	//
	// No version bound either way: seen on Adreno 610, 650 and 740 across every Mesa tested up to
	// 26.3-devel, while the Qualcomm driver on the same 740 is correct. Upstream has no fix to bound
	// the top at.
	{"vk-turnip-blend-constant-ignored", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::MesaTurnip, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenBlendConstant), 0},
	// Turnip below Mesa 26.2 hangs the GPU on A6XX_EARLY_Z_LATE_Z + a D32_SFLOAT_S8_UINT
	// depth-stencil attachment + a fragment shader that can discard. With a stencil buffer every
	// depth target is D32S8, and GSDeviceVK::SetupDATE's stencil pre-pass is that draw, so the first
	// DATE draw loses the device. Fixed upstream in 26.2 ("tu/a6xx: Work around D32S8 EARLY_Z_LATE_Z
	// hang", MR !41858, demotes to LATE_Z); not in any 26.1.x.
	//
	// Turnip only: the proprietary driver was never tested for this. vk-adreno5xx-depth-stencil
	// declares BrokenDepthStencilDiscard on its own evidence and deliberately does not take this
	// workaround.
	{"vk-turnip-d32s8-early-z-late-z-hang", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::MesaTurnip, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {26, 2, 0},
		0, 0, true, Bug(DriverBug::BrokenDepthStencilDiscard),
		Workaround(DriverWorkaround::DisableStencilBuffer)},
	{"vk-qualcomm-pre-adreno8-readback", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::QualcommProprietary, MobileGpuArchitecture::Unknown, 200, 799, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::SlowOptimalImageToBufferCopy),
		Workaround(DriverWorkaround::UseStagingImageForReadback)},
	{"vk-adreno5xx-depth-stencil", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::QualcommProprietary, MobileGpuArchitecture::Adreno5xx, 500, 599, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenDepthStencilDiscard) | Bug(DriverBug::BrokenColorWriteMaskWithDepthTest),
		Workaround(DriverWorkaround::EmulateColorWriteMask)},
	{"vk-qualcomm-dynamic-rendering-before-512-801", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::QualcommProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {512, 801, 0},
		0, 0, true, Bug(DriverBug::BrokenDynamicRendering), 0},
	{"vk-qualcomm-imageless-before-512-806", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::QualcommProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {512, 806, 0},
		0, 0, true, Bug(DriverBug::BrokenImagelessFramebuffer), 0},
	{"vk-powervr-proprietary", MobileGpuApi::Vulkan, RuntimeGpuProfile::PowerVR,
		MobileGpuDriver::ImaginationProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenPushDescriptors) | Bug(DriverBug::BrokenAttachmentFeedbackLoopLayout) |
			Bug(DriverBug::Broken16BitTextureFormats) |
			Bug(DriverBug::BrokenDynamicRendering) | Bug(DriverBug::BrokenImagelessFramebuffer) |
			Bug(DriverBug::BrokenPrimitiveTopologyDynamicState) |
			Bug(DriverBug::BrokenGraphicsPipelineLibrary),
		Workaround(DriverWorkaround::UseDescriptorSets) |
			Workaround(DriverWorkaround::DisableAttachmentFeedbackLoopLayout)},
	{"vk-powervr-clear-loadop-1-7-to-1-10", MobileGpuApi::Vulkan, RuntimeGpuProfile::PowerVR,
		MobileGpuDriver::ImaginationProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0,
		{1, 7, 0}, {1, 10, 0}, 0, 0, false, Bug(DriverBug::BrokenClearLoadOpRenderPass),
		Workaround(DriverWorkaround::AvoidClearLoadOpRenderPass)},
	{"vk-powervr-old-swapchain-width", MobileGpuApi::Vulkan, RuntimeGpuProfile::PowerVR,
		MobileGpuDriver::ImaginationProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		0, Workaround(DriverWorkaround::AlignSwapchainWidthTo32)},
	{"vk-powervr-primitive-topology", MobileGpuApi::Vulkan, RuntimeGpuProfile::PowerVR,
		MobileGpuDriver::ImaginationProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenPrimitiveTopologyDynamicState), 0},
	{"vk-arm-jm-r46-r50-extended-dynamic", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {46, 0, 0}, {51, 0, 0},
		0, 0, false, Bug(DriverBug::BrokenExtendedDynamicState), 0},
}};
} // namespace

MobileDriverProfile ResolveDriverProfile(const GpuProfileSelection& selection,
	const MobileDriverContext& context, std::string_view lowered_hints)
{
	MobileDriverProfile profile;
	profile.api = context.api;
	profile.driver = DetectDriver(selection, context, lowered_hints);
	profile.driver_name = context.driver_name.empty() ? std::string() : std::string(context.driver_name);
	profile.version = (context.api == MobileGpuApi::Vulkan) ?
		ParseVulkanDriverVersion(context, profile.driver) :
		ParseOpenGLDriverVersion(context.api_version_string, selection.runtime_profile);
	profile.version.raw = context.driver_version;

	if (selection.runtime_profile != RuntimeGpuProfile::Unknown)
		profile.confidence = selection.gpu.recognized ?
			DriverProfileConfidence::Model : DriverProfileConfidence::Vendor;
	if (profile.driver != MobileGpuDriver::Unknown)
		profile.confidence = DriverProfileConfidence::Driver;
	if (profile.version.known)
		profile.confidence = DriverProfileConfidence::DriverVersion;

	// Two driver facts that do not come from the rule table. Both default false, so an
	// unmeasured driver keeps its current road.
	//
	// Ordering: Turnip with a fix-generation tag, on Adreno 6xx at 650 and up only. The fix changes
	// emission for A6XX only, so a tagged build on a7xx carries nothing to trust. On Adreno 610 the
	// declared road renders inconsistently run to run while the copy road is stable, so parts below
	// 650 keep their barriers and copy road. A Qualcomm driver cannot carry a Mesa git tag.
	profile.declared_loop_fix_generation = ParseFixGeneration(context.driver_info);
	profile.orders_declared_feedback_loop = (profile.declared_loop_fix_generation >= 1) &&
		                                    (context.api == MobileGpuApi::Vulkan) && (profile.driver == MobileGpuDriver::MesaTurnip) &&
		                                    (selection.gpu.architecture == MobileGpuArchitecture::Adreno6xx) &&
		                                    (selection.gpu.model_number >= 650);

	// The a7xx preference is about the part, not the build, so it needs no tag: on Adreno 740 the
	// declared loop with our barriers is correct and stable where the copy road renders wrong.
	// Limited to 730-750: the 740 was measured, the 730 and 750 are the neighbouring a7xx
	// generations in Mesa's freedreno table. 702-725 were never run (Mesa treats the 702 as a6xx
	// family), so they keep the copy road.
	profile.prefers_declared_loop_with_barriers = (context.api == MobileGpuApi::Vulkan) &&
		                                          (profile.driver == MobileGpuDriver::MesaTurnip) &&
		                                          (selection.gpu.architecture == MobileGpuArchitecture::Adreno7xx) &&
		                                          (selection.gpu.model_number >= 730);

	for (const DriverRule& rule : s_driver_rules)
	{
		if (std::string_view(rule.id) == "vk-powervr-old-swapchain-width" &&
			(profile.version.raw == 0 || profile.version.raw >= 0x00582558u))
		{
			continue;
		}
		if (std::string_view(rule.id) == "vk-qualcomm-pre-adreno8-readback" &&
			(selection.gpu.architecture == MobileGpuArchitecture::Adreno8xx ||
				selection.gpu.architecture == MobileGpuArchitecture::AdrenoX ||
				selection.gpu.architecture == MobileGpuArchitecture::Unknown))
		{
			continue;
		}
		if (std::string_view(rule.id) == "vk-arm-midgard-uniform-indexing" &&
			(!profile.version.legacy_hash ||
				(selection.gpu.model_number != 830 && selection.gpu.model_number != 860 &&
					selection.gpu.model_number != 880)))
		{
			continue;
		}
		if (std::string_view(rule.id) == "vk-arm-jm-r46-r50-extended-dynamic" &&
			context.max_draw_indirect_count > 1)
		{
			continue;
		}
		if (!RuleMatches(rule, selection, context, profile, lowered_hints))
			continue;

		profile.bugs |= rule.bugs;
		profile.workarounds |= rule.workarounds;
		profile.matched_rule_count++;
	}

	// Applied last so no rule filter drops it, and outside matched_rule_count, which counts
	// database matches only.
	profile.bugs |= GpuProfileDetector::GetForcedBugs();

	profile.conservative_fallback =
		(selection.runtime_profile == RuntimeGpuProfile::Unknown || profile.driver == MobileGpuDriver::Unknown);
	return profile;
}
} // namespace GpuProfileDetail

namespace
{
u64 s_forced_driver_bugs = 0;
}

void GpuProfileDetector::SetForcedBugs(u64 mask)
{
	s_forced_driver_bugs = mask;
}

u64 GpuProfileDetector::GetForcedBugs()
{
	return s_forced_driver_bugs;
}

u32 GpuProfileDetector::ParseDeclaredLoopFixGeneration(std::string_view driver_info)
{
	return GpuProfileDetail::ParseFixGeneration(driver_info);
}

VulkanDeviceRules GpuProfileDetector::ResolveVulkanDeviceRules(const GpuProfileSelection& selection,
	const MobileDriverContext& context, std::string_view device_name)
{
	using namespace GpuProfileDetail;

	const bool mali = (context.vendor_id == GpuVendorID::ARM);
	const bool adreno = (context.vendor_id == GpuVendorID::Qualcomm);
	const bool qualcomm_driver = (context.driver_id == DRIVER_ID_QUALCOMM_PROPRIETARY);
	const bool turnip = (context.driver_id == DRIVER_ID_MESA_TURNIP);
	const bool honeykrisp = (context.driver_id == DRIVER_ID_MESA_HONEYKRISP);
	const auto name_has = [device_name](std::string_view part) {
		return device_name.find(part) != std::string_view::npos;
	};

	VulkanDeviceRules rules;
	rules.broken_timestamp_queries = mali && name_has("Mali-G615");
	// Matched on the driverInfo string, not the parsed revision the rule table uses, and without the
	// table's MT6897 exemption. MT6897's r44p1 does not advertise the layout extension, so the two
	// agree on every r44p1 device seen.
	rules.avoid_feedback_loop_layout =
		mali && context.driver_info.find("r44p1") != std::string_view::npos;
	rules.avoid_push_descriptors = mali || (adreno && !qualcomm_driver && !turnip);
	rules.broken_provoking_vertex = adreno && qualcomm_driver;
	rules.broken_colormask_with_depth =
		adreno && !turnip && (context.device_id < 0x06000000u || context.driver_version < 0x801EA000u);
	rules.broken_mad_deinterlace = mali && name_has("Mali-G57");
	rules.adreno8xx_proprietary =
		adreno && qualcomm_driver && selection.gpu.architecture == MobileGpuArchitecture::Adreno8xx;
	rules.self_read_costs_measured = turnip || honeykrisp;
	rules.barrier_road_measured = honeykrisp;
	return rules;
}
