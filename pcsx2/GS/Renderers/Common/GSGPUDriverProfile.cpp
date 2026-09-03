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
// VkDriverId values. Duplicating the numeric ABI values keeps this portable file independent from
// Vulkan headers and lets the same resolver serve OpenGL unit tests.
constexpr u32 DRIVER_ID_IMAGINATION_PROPRIETARY = 7;
constexpr u32 DRIVER_ID_QUALCOMM_PROPRIETARY = 8;
constexpr u32 DRIVER_ID_ARM_PROPRIETARY = 9;
constexpr u32 DRIVER_ID_MESA_TURNIP = 18;
constexpr u32 DRIVER_ID_MESA_PANVK = 20;
constexpr u32 DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA = 25;

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
};

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
	const MobileDriverContext& context, const MobileDriverProfile& profile)
{
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

// Sources and exact upstream revisions are mirrored in docs/gpu-driver-database.json. A known
// driver bug is not automatically an active workaround: expensive renderer fallbacks stay disabled
// until their PCSX2 integration has a bounded, tested condition.
static constexpr std::array<DriverRule, 30> s_driver_rules = {{
	{"gl-arm-buffer-stream", MobileGpuApi::OpenGL, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenBufferStreaming) | Bug(DriverBug::BrokenUnsynchronizedMapping) |
			Bug(DriverBug::BrokenVectorBitwiseAnd) | Bug(DriverBug::BrokenVSync),
		Workaround(DriverWorkaround::ScalarizeVectorBitwiseAnd)},
	{"gl-arm-g57-fifo", MobileGpuApi::OpenGL, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 57, 57, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenVSync), Workaround(DriverWorkaround::ForceFifoPresent)},
	// r44p1 and the in-tile render-target self-read, on GL: there is deliberately NO rule here,
	// and that is a product decision made on field evidence, not an oversight — read this before
	// "completing the pair" with the Vulkan rule below.
	//
	// A gl-arm-r44p1-attachment-self-read rule shipped in 2.6.6.5 (mirroring a hand-rolled gate
	// that had existed since 2.6.6.3). Through 2.6.6.4 that gate was INERT: the Mali profile
	// block re-enabled the ARM fetch backend moments after the gate turned it off (the
	// contradiction GSFramebufferFetchPolicy.h documents), so the entire r44p1 user base shipped
	// on GL + GL_ARM_shader_framebuffer_fetch. 2.6.6.5 fixed the contradiction, the gate finally
	// engaged, and on GLES — where fetch and the texture barrier are one capability — every
	// self-referential draw became an RT copy plus a tile flush: Shadow of the Colossus went
	// 30 -> 7 fps on an Anbernic RG 477V and users downgraded to 2.6.6.4 en masse.
	//
	// The fetch path DOES corrupt some content on this blob (MGS3 was the observed case; the
	// likely mechanism is the driver granting the tile-read slot per attachment format and
	// silently degrading denied reads to memory fetches inside a live feedback loop). But that
	// is the exact trade 2.6.6.4 shipped, the field overwhelmingly chose it, and Vulkan — where
	// the RT-copy path costs an ordinary image copy instead of a tile flush — remains the
	// correct-rendering choice for the affected games. Restored 2026-08-12, this time on
	// purpose. If a rule is ever re-added here, it also flips Auto back to Vulkan for these
	// devices via GSUtil::AndroidAutoPrefersVulkan, which asks this table.
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
	// Upstream keys this on a PowerVRSeries5 architecture value we do not carry — our enum has a
	// single PowerVR entry. The 500-599 model range is what actually selects SGX 5xx here, so the
	// rule stays exactly as narrow without splitting the architecture enum.
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
	// The slow-cached-readback story (Dolphin's BUG_SLOW_CACHED_READBACK_MEMORY, ported as a
	// blanket Mali rule) was MEASURED BACKWARDS on r44p1 / Mali-G615 / MT6897 2026-08-17: a
	// crossing-cost probe (umbrella gpu-drivers CROSSING-COST-RESULT.md) allocating a genuinely
	// non-coherent cached type and paying the explicit invalidate per slot — the exact kernel
	// cost the workaround is about — still beats the coherent map ~12× per 512×448 readback
	// (4,021 → 329 µs), because a sequential CPU pass over the uncached map runs at ~244 MB/s.
	// That measurement does not prove Dolphin wrong on older parts, so the preference is
	// NARROWED by driver version rather than deleted: exactly the [44.1, 44.2) revision the
	// probe ran keeps cached readbacks; every other revision keeps the coherent preference it
	// always had. Same version bounds as the r44p1 self-read rule above, same reasoning: change
	// nothing on hardware nobody measured.
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
	// r44p1 on Vulkan loses the device outright under the in-tile self-read (VK_ERROR_DEVICE_LOST
	// at vkWaitForFences — Rogue Galaxy and others), so this rule is a crash gate, not a perf
	// trade, and it stays even though its former GL twin was deliberately lifted (see the account
	// above the GL rules). Disabling the feedback-loop LAYOUT alone was tried first and proved
	// nothing: r44p1 never advertises VK_EXT_attachment_feedback_loop_layout, so the disable was
	// vacuous — the read that kills the device is the ROAA/barrier one, and nothing short of
	// reading a separate copy survives.
	{"vk-arm-r44p1-attachment-self-read", MobileGpuApi::Vulkan, RuntimeGpuProfile::Mali,
		MobileGpuDriver::ArmProprietary, MobileGpuArchitecture::Unknown, 0, 0, 0, {44, 1, 0}, {44, 2, 0},
		0, 0, false,
		Bug(DriverBug::BrokenSubpassFeedback) | Bug(DriverBug::BrokenAttachmentFeedbackLoopLayout),
		Workaround(DriverWorkaround::UseRenderTargetCopyForFeedback)},
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
	// Turnip shares none of the blob's other defects but inherits the same broken render-target
	// self-read, so it needs its own rule rather than the vk-qualcomm-proprietary one (which is
	// keyed on MobileGpuDriver::QualcommProprietary).
	//
	// ARMSX2 #442: with an HD texture pack, Tales of the Abyss loses its entire 2D text layer the
	// moment the replacement's alpha range flips those draws to require_one_barrier and the RT
	// self-read engages. Device A/B on Turnip/Mesa 26.1.2 + Adreno 650 established that BOTH
	// in-pass forms drop the content — the subpassLoad input attachment AND the
	// feedback-loop-layout texelFetch sampler — while reading a separate RT copy renders
	// correctly. Hence both bug bits and the expensive workaround. The reporter sees the same
	// failure on the proprietary blob.
	{"vk-turnip-attachment-self-read", MobileGpuApi::Vulkan, RuntimeGpuProfile::Adreno,
		MobileGpuDriver::MesaTurnip, MobileGpuArchitecture::Unknown, 0, 0, 0, {}, {}, 0, 0, false,
		Bug(DriverBug::BrokenSubpassFeedback) | Bug(DriverBug::BrokenAttachmentFeedbackLoopLayout),
		Workaround(DriverWorkaround::UseRenderTargetCopyForFeedback)},
	// Turnip below Mesa 26.2 wedges the GPU on A6XX_EARLY_Z_LATE_Z + a VK_FORMAT_D32_SFLOAT_S8_UINT
	// depth-stencil attachment + a fragment shader that can discard. Enabling the stencil buffer
	// makes every depth target D32S8, and GSDeviceVK::SetupDATE's stencil pre-pass quad is that
	// draw exactly, so the first DATE draw of a run is enough. Round 20260903-0135 on an A650 /
	// turnip 26.1.2: 8 of 8 titles lost the device in 6-9 s, and the devcoredump latches
	// Z_MODE = A6XX_EARLY_Z_LATE_Z, DEPTH_FORMAT = DEPTH6_32 + SEPARATE_STENCIL and our DATM
	// stencil op set -- decode in umbrella
	// devs/bmdhacks/campaigns/gs-classic-tiler/phase3-adreno-stencil-date/CRASHDEC.md. Fixed by
	// Mesa a70d2af590d ("tu/a6xx: Work around D32S8 EARLY_Z_LATE_Z hang", MR !41858), which
	// demotes the z-mode to LATE_Z; in main and 26.2, in no 26.1.x.
	//
	// Turnip-bounded on purpose. This replaces a vendorID-wide `is_adreno` clause in
	// GSDeviceVK::CheckFeatures whose own commit (05998bc5c4) recorded that it was left vendor-wide
	// only because turnip was the only Adreno driver we shipped against; the proprietary blob was
	// never tested for this and does not inherit turnip's bug. vk-adreno5xx-depth-stencil declares
	// BrokenDepthStencilDiscard on its own evidence and deliberately does not take this workaround.
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
		if (!RuleMatches(rule, selection, context, profile))
			continue;

		profile.bugs |= rule.bugs;
		profile.workarounds |= rule.workarounds;
		profile.matched_rule_count++;
	}

	profile.conservative_fallback =
		(selection.runtime_profile == RuntimeGpuProfile::Unknown || profile.driver == MobileGpuDriver::Unknown);
	return profile;
}
} // namespace GpuProfileDetail
