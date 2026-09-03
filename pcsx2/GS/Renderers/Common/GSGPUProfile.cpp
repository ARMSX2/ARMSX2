// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSGPUProfilePrivate.h"

#include <array>
#include <cctype>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#elif defined(__linux__)
#include <cstdio>
#endif

namespace GpuProfileDetail
{
std::string ToLowerASCII(std::string_view value)
{
	std::string lowered;
	lowered.reserve(value.size());

	for (const char ch : value)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));

	return lowered;
}

static bool Contains(std::string_view haystack, std::string_view needle)
{
	return (haystack.find(needle) != std::string_view::npos);
}

bool ContainsAny(std::string_view haystack, std::initializer_list<const char*> needles)
{
	for (const char* needle : needles)
	{
		if (Contains(haystack, needle))
			return true;
	}

	return false;
}

MobileGsTuning MakeMobileGsTuning(u32 pooled_targets, u32 target_age, u32 pooled_textures, u32 texture_age,
	bool prefer_new_textures)
{
	MobileGsTuning tuning;
	tuning.pooled_targets = pooled_targets;
	tuning.target_age = target_age;
	tuning.pooled_textures = pooled_textures;
	tuning.texture_age = texture_age;
	tuning.constrained = (pooled_targets < 128 || pooled_textures < 128);
	tuning.prefer_new_textures = prefer_new_textures;
	return tuning;
}

MobileGsTuning MakeConservativeMobileGsTuning()
{
	return MakeMobileGsTuning(96, 8, 96, 6, false);
}
} // namespace GpuProfileDetail

namespace
{
static void AppendHint(std::string& hints, std::string_view key, std::string_view value)
{
	if (value.empty())
		return;

	if (!hints.empty())
		hints.append(" | ");

	if (!key.empty())
	{
		hints.append(key);
		hints.push_back('=');
	}

	hints.append(value);
}

#if defined(__ANDROID__)
static std::string GetAndroidProperty(const char* name)
{
	std::array<char, PROP_VALUE_MAX> value = {};
	const int length = __system_property_get(name, value.data());
	return (length > 0) ? std::string(value.data(), static_cast<size_t>(length)) : std::string();
}
#elif defined(__linux__)
// The SoC identity on a Linux handheld (ROCKNIX and its relatives), where there is no Android
// property service. /proc/device-tree/compatible is a NUL-separated list of "vendor,board" strings
// ordered most specific first, and on a MediaTek part it names the chipset -- an Anbernic RG 477V
// reads "anbernic,rg477v" then "mediatek,mt6897". Nothing else on this platform reports the SoC
// without shelling out or parsing a distribution-specific file, and every driver string the Vulkan
// and GL APIs hand us describes the GPU rather than the chip it sits in.
//
// Read once per device creation, four hundred bytes at most, off a file that either exists or does
// not -- systems without a device tree simply contribute no hint.
static std::string GetDeviceTreeCompatible()
{
	std::FILE* file = std::fopen("/proc/device-tree/compatible", "rb");
	if (!file)
		return std::string();

	std::array<char, 512> buffer = {};
	const size_t length = std::fread(buffer.data(), 1, buffer.size(), file);
	std::fclose(file);

	// Separators become spaces so a substring search cannot match across two entries, and so the
	// result is one printable token list like every other hint.
	std::string compatible;
	compatible.reserve(length);
	for (size_t i = 0; i < length; i++)
		compatible.push_back((buffer[i] == '\0') ? ' ' : buffer[i]);

	while (!compatible.empty() && compatible.back() == ' ')
		compatible.pop_back();

	return compatible;
}
#endif

static std::string BuildHints(std::string_view gpu_vendor, std::string_view gpu_renderer_or_name,
	const MobileDriverContext& driver_context)
{
	std::string hints;
	AppendHint(hints, "gpu_vendor", gpu_vendor);
	AppendHint(hints, "gpu", gpu_renderer_or_name);
	AppendHint(hints, "driver_name", driver_context.driver_name);
	AppendHint(hints, "driver_info", driver_context.driver_info);
	AppendHint(hints, "api_version", driver_context.api_version_string);
	AppendHint(hints, "platform", driver_context.platform_hints);

#if defined(__ANDROID__)
	static constexpr const char* property_names[] = {
		"ro.soc.manufacturer",
		"ro.soc.model",
		"ro.soc.platform",
		"ro.board.platform",
		"ro.hardware",
		"ro.hardware.chipname",
		"ro.chipname",
		"ro.product.board",
		"ro.product.manufacturer",
		"ro.product.model",
		"ro.vendor.product.manufacturer",
		"ro.vendor.product.model",
		"ro.mediatek.platform",
		"ro.vendor.mediatek.platform",
		"ro.product.cpu.abi",
		"ro.vendor.product.cpu.abilist",
	};

	for (const char* property_name : property_names)
		AppendHint(hints, property_name, GetAndroidProperty(property_name));
#elif defined(__linux__)
	AppendHint(hints, "dt_compatible", GetDeviceTreeCompatible());
#endif

	return hints;
}

static bool LooksLikeMediaTekSoc(std::string_view lowered_hints)
{
	if (GpuProfileDetail::ContainsAny(lowered_hints, {"mediatek", "dimensity", "helio"}))
		return true;

	// MediaTek board/platform properties commonly use compact part numbers such as mt6877 or
	// mt6989z without spelling out the vendor. Require a token boundary and four digits to avoid
	// treating an unrelated occurrence of "mt" as a chipset identifier.
	for (size_t i = 0; i + 6 <= lowered_hints.size(); i++)
	{
		if (lowered_hints[i] != 'm' || lowered_hints[i + 1] != 't' ||
			(i > 0 && std::isalnum(static_cast<unsigned char>(lowered_hints[i - 1]))))
		{
			continue;
		}

		bool has_four_digits = true;
		for (size_t digit = i + 2; digit < i + 6; digit++)
			has_four_digits &= (std::isdigit(static_cast<unsigned char>(lowered_hints[digit])) != 0);

		if (has_four_digits)
			return true;
	}

	return false;
}
} // namespace

GpuProfileOverride GpuProfileDetector::ParseOverride(std::string_view value)
{
	const std::string lowered = GpuProfileDetail::ToLowerASCII(value);
	if (lowered == "mali")
		return GpuProfileOverride::Mali;
	if (lowered == "adreno")
		return GpuProfileOverride::Adreno;
	if (lowered == "powervr")
		return GpuProfileOverride::PowerVR;
	if (lowered == "xclipse")
		return GpuProfileOverride::Xclipse;

	return GpuProfileOverride::Auto;
}

const char* GpuProfileDetector::OverrideToConfigString(GpuProfileOverride value)
{
	switch (value)
	{
		case GpuProfileOverride::Mali:
			return "mali";
		case GpuProfileOverride::Adreno:
			return "adreno";
		case GpuProfileOverride::PowerVR:
			return "powervr";
		case GpuProfileOverride::Xclipse:
			return "xclipse";
		case GpuProfileOverride::Auto:
		default:
			return "auto";
	}
}

const char* GpuProfileDetector::OverrideToString(GpuProfileOverride value)
{
	switch (value)
	{
		case GpuProfileOverride::Mali:
			return "Force Mali";
		case GpuProfileOverride::Adreno:
			return "Force Adreno";
		case GpuProfileOverride::PowerVR:
			return "Force PowerVR";
		case GpuProfileOverride::Xclipse:
			return "Force Xclipse";
		case GpuProfileOverride::Auto:
		default:
			return "Auto";
	}
}

const char* GpuProfileDetector::RuntimeProfileToString(RuntimeGpuProfile value)
{
	switch (value)
	{
		case RuntimeGpuProfile::Mali:
			return "Mali";
		case RuntimeGpuProfile::PowerVR:
			return "PowerVR";
		case RuntimeGpuProfile::Adreno:
			return "Adreno";
		case RuntimeGpuProfile::Xclipse:
			return "Xclipse";
		case RuntimeGpuProfile::Apple:
			return "Apple";
		case RuntimeGpuProfile::Unknown:
		default:
			return "Unknown";
	}
}

const char* GpuProfileDetector::ArchitectureToString(MobileGpuArchitecture value)
{
	switch (value)
	{
		case MobileGpuArchitecture::Adreno2xx: return "Adreno 2xx";
		case MobileGpuArchitecture::Adreno3xx: return "Adreno 3xx";
		case MobileGpuArchitecture::Adreno4xx: return "Adreno 4xx";
		case MobileGpuArchitecture::Adreno5xx: return "Adreno 5xx";
		case MobileGpuArchitecture::Adreno6xx: return "Adreno 6xx";
		case MobileGpuArchitecture::Adreno7xx: return "Adreno 7xx";
		case MobileGpuArchitecture::Adreno8xx: return "Adreno 8xx";
		case MobileGpuArchitecture::AdrenoX: return "Adreno X";
		case MobileGpuArchitecture::MaliUtgard: return "Mali Utgard";
		case MobileGpuArchitecture::MaliMidgard: return "Mali Midgard";
		case MobileGpuArchitecture::MaliBifrost: return "Mali Bifrost";
		case MobileGpuArchitecture::MaliValhall1: return "Mali Valhall (1st Gen)";
		case MobileGpuArchitecture::MaliValhall2: return "Mali Valhall (2nd Gen)";
		case MobileGpuArchitecture::MaliValhall3: return "Mali Valhall (3rd Gen)";
		case MobileGpuArchitecture::MaliFifthGen: return "Arm 5th Gen";
		case MobileGpuArchitecture::MaliG1: return "Arm Mali G1";
		case MobileGpuArchitecture::PowerVR: return "PowerVR";
		case MobileGpuArchitecture::Unknown:
		default:
			return "Unknown";
	}
}

static void ApplyResolvedProfile(GpuProfileSelection& selection, RuntimeGpuProfile runtime_profile,
	GpuProfileDetail::ResolvedGpuProfile&& resolved)
{
	selection.runtime_profile = runtime_profile;
	selection.gpu = std::move(resolved.gpu);
	selection.gs_tuning = resolved.tuning;
}

const char* GpuProfileDetector::ApiToString(MobileGpuApi value)
{
	switch (value)
	{
		case MobileGpuApi::OpenGL: return "OpenGL";
		case MobileGpuApi::Vulkan: return "Vulkan";
		case MobileGpuApi::Unknown:
		default: return "Unknown";
	}
}

const char* GpuProfileDetector::DriverToString(MobileGpuDriver value)
{
	switch (value)
	{
		case MobileGpuDriver::ArmProprietary: return "ARM proprietary";
		case MobileGpuDriver::MesaPanVK: return "Mesa PanVK";
		case MobileGpuDriver::QualcommProprietary: return "Qualcomm proprietary";
		case MobileGpuDriver::MesaTurnip: return "Mesa Turnip";
		case MobileGpuDriver::ImaginationProprietary: return "Imagination proprietary";
		case MobileGpuDriver::MesaPowerVR: return "Mesa PowerVR";
		case MobileGpuDriver::Angle: return "ANGLE";
		case MobileGpuDriver::Unknown:
		default: return "Unknown";
	}
}

const char* GpuProfileDetector::BugToString(DriverBug value)
{
	switch (value)
	{
		case DriverBug::BrokenBufferStreaming: return "BrokenBufferStreaming";
		case DriverBug::BrokenUnsynchronizedMapping: return "BrokenUnsynchronizedMapping";
		case DriverBug::BrokenNegatedBoolean: return "BrokenNegatedBoolean";
		case DriverBug::BrokenVectorBitwiseAnd: return "BrokenVectorBitwiseAnd";
		case DriverBug::BrokenBitwiseOpNegation: return "BrokenBitwiseOpNegation";
		case DriverBug::BrokenPrimitiveRestart: return "BrokenPrimitiveRestart";
		case DriverBug::BrokenPushDescriptors: return "BrokenPushDescriptors";
		case DriverBug::BrokenProvokingVertex: return "BrokenProvokingVertex";
		case DriverBug::BrokenAttachmentFeedbackLoopLayout: return "BrokenAttachmentFeedbackLoopLayout";
		case DriverBug::BrokenSubpassFeedback: return "BrokenSubpassFeedback";
		case DriverBug::BrokenColorWriteMaskWithDepthTest: return "BrokenColorWriteMaskWithDepthTest";
		case DriverBug::BrokenDepthStencilDiscard: return "BrokenDepthStencilDiscard";
		case DriverBug::BrokenReversedDepthRange: return "BrokenReversedDepthRange";
		case DriverBug::SlowCachedReadbackMemory: return "SlowCachedReadbackMemory";
		case DriverBug::SlowOptimalImageToBufferCopy: return "SlowOptimalImageToBufferCopy";
		case DriverBug::BrokenClearLoadOpRenderPass: return "BrokenClearLoadOpRenderPass";
		case DriverBug::Broken16BitTextureFormats: return "Broken16BitTextureFormats";
		case DriverBug::BrokenGenerateMipmapTallTexture: return "BrokenGenerateMipmapTallTexture";
		case DriverBug::BrokenEmptyRenderPass: return "BrokenEmptyRenderPass";
		case DriverBug::BrokenConstantLoad: return "BrokenConstantLoad";
		case DriverBug::BrokenUniformIndexing: return "BrokenUniformIndexing";
		case DriverBug::BrokenVSync: return "BrokenVSync";
		case DriverBug::BrokenMultithreadedShaderCompilation: return "BrokenMultithreadedShaderCompilation";
		case DriverBug::BrokenDynamicRendering: return "BrokenDynamicRendering";
		case DriverBug::BrokenImagelessFramebuffer: return "BrokenImagelessFramebuffer";
		case DriverBug::BrokenExtendedDynamicState: return "BrokenExtendedDynamicState";
		case DriverBug::BrokenPrimitiveTopologyDynamicState: return "BrokenPrimitiveTopologyDynamicState";
		case DriverBug::BrokenGraphicsPipelineLibrary: return "BrokenGraphicsPipelineLibrary";
		case DriverBug::BrokenRoaaDestinationRead: return "BrokenRoaaDestinationRead";
		case DriverBug::Count:
		default: return "Unknown";
	}
}

const char* GpuProfileDetector::WorkaroundToString(DriverWorkaround value)
{
	switch (value)
	{
		case DriverWorkaround::RewriteBooleanNegation: return "RewriteBooleanNegation";
		case DriverWorkaround::ScalarizeVectorBitwiseAnd: return "ScalarizeVectorBitwiseAnd";
		case DriverWorkaround::StoreBitwiseNegationInTemporary: return "StoreBitwiseNegationInTemporary";
		case DriverWorkaround::UseDescriptorSets: return "UseDescriptorSets";
		case DriverWorkaround::DisableProvokingVertex: return "DisableProvokingVertex";
		case DriverWorkaround::DisableAttachmentFeedbackLoopLayout: return "DisableAttachmentFeedbackLoopLayout";
		case DriverWorkaround::UseRenderTargetCopyForFeedback: return "UseRenderTargetCopyForFeedback";
		case DriverWorkaround::EmulateColorWriteMask: return "EmulateColorWriteMask";
		case DriverWorkaround::PreferCoherentReadback: return "PreferCoherentReadback";
		case DriverWorkaround::UseStagingImageForReadback: return "UseStagingImageForReadback";
		case DriverWorkaround::AvoidClearLoadOpRenderPass: return "AvoidClearLoadOpRenderPass";
		case DriverWorkaround::GenerateMipmapManuallyForTallTextures: return "GenerateMipmapManuallyForTallTextures";
		case DriverWorkaround::RewriteUniformIndexing: return "RewriteUniformIndexing";
		case DriverWorkaround::ForceFifoPresent: return "ForceFifoPresent";
		case DriverWorkaround::AlignSwapchainWidthTo32: return "AlignSwapchainWidthTo32";
		case DriverWorkaround::DisableStencilBuffer: return "DisableStencilBuffer";
		case DriverWorkaround::PreferVulkanRenderer: return "PreferVulkanRenderer";
		case DriverWorkaround::PreferCachedStreamRingMemory: return "PreferCachedStreamRingMemory";
		case DriverWorkaround::Count:
		default: return "Unknown";
	}
}


GpuProfileSelection GpuProfileDetector::Resolve(std::string_view override_value, std::string_view gpu_vendor,
	std::string_view gpu_renderer_or_name)
{
	// No driver context: the driver profile stays in its conservative-fallback state, so callers
	// that have not been taught to pass one behave exactly as before.
	return Resolve(override_value, gpu_vendor, gpu_renderer_or_name, MobileDriverContext{});
}

GpuProfileSelection GpuProfileDetector::Resolve(std::string_view override_value, std::string_view gpu_vendor,
	std::string_view gpu_renderer_or_name, const MobileDriverContext& driver_context)
{
	GpuProfileSelection selection;
	selection.override_mode = ParseOverride(override_value);
	selection.hints = BuildHints(gpu_vendor, gpu_renderer_or_name, driver_context);
	const std::string lowered_hints = GpuProfileDetail::ToLowerASCII(selection.hints);
	const std::string lowered_override = GpuProfileDetail::ToLowerASCII(override_value);
	selection.is_mediatek_soc = (lowered_override == "mediatek") || LooksLikeMediaTekSoc(lowered_hints);
	selection.gs_tuning = GpuProfileDetail::MakeConservativeMobileGsTuning();
	// Attached on every exit path so the driver profile is always populated, whether the family
	// came from an override, from detection, or from nothing at all.
	const auto finalize = [&]() {
		selection.driver = GpuProfileDetail::ResolveDriverProfile(selection, driver_context, lowered_hints);
		return selection;
	};

	if (selection.override_mode == GpuProfileOverride::Mali)
	{
		ApplyResolvedProfile(selection, RuntimeGpuProfile::Mali, GpuProfileDetail::ResolveMaliProfile(lowered_hints));
		return finalize();
	}

	if (selection.override_mode == GpuProfileOverride::Adreno)
	{
		ApplyResolvedProfile(selection, RuntimeGpuProfile::Adreno, GpuProfileDetail::ResolveAdrenoProfile(lowered_hints));
		return finalize();
	}

	if (selection.override_mode == GpuProfileOverride::PowerVR)
	{
		ApplyResolvedProfile(selection, RuntimeGpuProfile::PowerVR, GpuProfileDetail::ResolvePowerVRProfile(lowered_hints));
		return finalize();
	}

	if (selection.override_mode == GpuProfileOverride::Xclipse)
	{
		// Samsung Xclipse (Exynos, AMD-RDNA2) has no dedicated resolver — there is no reliable
		// SoC-property signature and its one hardware quirk (broken ROAA framebuffer fetch) is
		// keyed off the Vulkan vendorID (GSDeviceVK::IsDeviceXclipse). Setting the runtime
		// profile is enough for GSDeviceVK to force fbfetch off; keep the conservative GS
		// tuning already assigned above.
		selection.runtime_profile = RuntimeGpuProfile::Xclipse;
		return finalize();
	}

	if (GpuProfileDetail::LooksLikeAdreno(lowered_hints))
	{
		ApplyResolvedProfile(selection, RuntimeGpuProfile::Adreno, GpuProfileDetail::ResolveAdrenoProfile(lowered_hints));
	}
	else if (GpuProfileDetail::LooksLikePowerVR(lowered_hints))
	{
		ApplyResolvedProfile(selection, RuntimeGpuProfile::PowerVR, GpuProfileDetail::ResolvePowerVRProfile(lowered_hints));
	}
	else if (GpuProfileDetail::LooksLikeMali(lowered_hints))
	{
		ApplyResolvedProfile(selection, RuntimeGpuProfile::Mali, GpuProfileDetail::ResolveMaliProfile(lowered_hints));
	}

	return finalize();
}
