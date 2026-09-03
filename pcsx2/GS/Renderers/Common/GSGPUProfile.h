// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>

enum class GpuProfileOverride : u8
{
	Auto,
	Mali,
	Adreno,
	PowerVR,
	Xclipse,
};

enum class RuntimeGpuProfile : u8
{
	Unknown,
	Mali,
	Adreno,
	PowerVR,
	Xclipse,
	/// Apple Silicon (M-series / A-series). A TBDR like the mobile parts, but it is NOT one of
	/// them and must never inherit their workarounds — before this existed, desktop GL resolved
	/// anything not-Mali to Adreno, so an M2 ran Adreno-only paths (reported by bmd: "GL: Adreno -
	/// routing depth feedback through the depth sampler"). Distinct from Unknown so the tiler-ness
	/// can be acted on deliberately later rather than by accident.
	Apple,
};

enum class MobileGpuArchitecture : u8
{
	Unknown,
	Adreno2xx,
	Adreno3xx,
	Adreno4xx,
	Adreno5xx,
	Adreno6xx,
	Adreno7xx,
	Adreno8xx,
	AdrenoX,
	MaliUtgard,
	MaliMidgard,
	MaliBifrost,
	MaliValhall1,
	MaliValhall2,
	MaliValhall3,
	MaliFifthGen,
	MaliG1,
	PowerVR,
};

// ---------------------------------------------------------------------------------------------
// Driver identity and known-bug model, ported from EmuCoreX (sashkinbro) with his approval.
//
// The GPU family alone is not enough to decide behaviour: the same Mali part behaves differently
// under Arm's proprietary driver than under Mesa PanVK, which is a lesson this tree learned the
// expensive way (the r44p1 DEVICE_LOST fix had to be gated on driverID, not vendorID, and the
// 8 Elite push-descriptor disable likewise). Recording it as driver + version + a bug set means
// the next device-specific quirk is a table entry rather than another bespoke branch.
// ---------------------------------------------------------------------------------------------

enum class MobileGpuApi : u8
{
	Unknown,
	OpenGL,
	Vulkan,
};

// Deliberately independent of VkDriverId so profile resolution stays unit-testable without
// pulling in Vulkan headers, and so the OpenGL path can use the same table.
enum class MobileGpuDriver : u8
{
	Unknown,
	ArmProprietary,
	MesaPanVK,
	QualcommProprietary,
	MesaTurnip,
	ImaginationProprietary,
	MesaPowerVR,
	Angle,
};

/// How specifically a profile was matched. A rule matched on the exact driver version is worth
/// more than one matched on the vendor alone, so a broad entry never overrides a precise one.
enum class DriverProfileConfidence : u8
{
	Unknown,
	Vendor,
	Model,
	Driver,
	DriverVersion,
};

/// Observed driver defects. Naming is descriptive of the DEFECT, not of the fix, so one bug can
/// drive several workarounds and the table stays readable.
enum class DriverBug : u8
{
	BrokenBufferStreaming,
	BrokenUnsynchronizedMapping,
	BrokenNegatedBoolean,
	BrokenVectorBitwiseAnd,
	BrokenBitwiseOpNegation,
	BrokenPrimitiveRestart,
	BrokenPushDescriptors,
	BrokenProvokingVertex,
	BrokenAttachmentFeedbackLoopLayout,
	BrokenSubpassFeedback,
	BrokenColorWriteMaskWithDepthTest,
	BrokenDepthStencilDiscard,
	BrokenReversedDepthRange,
	SlowCachedReadbackMemory,
	SlowOptimalImageToBufferCopy,
	BrokenClearLoadOpRenderPass,
	Broken16BitTextureFormats,
	BrokenGenerateMipmapTallTexture,
	BrokenEmptyRenderPass,
	BrokenConstantLoad,
	BrokenUniformIndexing,
	BrokenVSync,
	BrokenMultithreadedShaderCompilation,
	BrokenDynamicRendering,
	BrokenImagelessFramebuffer,
	BrokenExtendedDynamicState,
	BrokenPrimitiveTopologyDynamicState,
	BrokenGraphicsPipelineLibrary,
	/// The driver advertises an in-tile destination read (Vulkan rasterization-order attachment
	/// access) and returns zero or stale colour from it -- black or intermittently missing
	/// textures rather than a crash. Distinct from BrokenSubpassFeedback, which is about the
	/// in-pass self-read losing whole draws or the device.
	BrokenRoaaDestinationRead,
	Count,
};

/// What we actually DO about a bug. Kept separate from [DriverBug] because the same mitigation
/// answers several defects, and because a workaround can be forced on for testing without
/// claiming the device has the bug.
enum class DriverWorkaround : u8
{
	RewriteBooleanNegation,
	ScalarizeVectorBitwiseAnd,
	StoreBitwiseNegationInTemporary,
	UseDescriptorSets,
	DisableProvokingVertex,
	DisableAttachmentFeedbackLoopLayout,
	/// Read the render target from a separate COPY instead of in-pass, for drivers where no form
	/// of attachment self-read works. Turns texture barriers off, which also disables framebuffer
	/// fetch (it is the same in-tile read), so the RT is never bound as an attachment and sampled
	/// at once. Expensive — a full render-target copy per feedback draw — so it is a last resort
	/// for drivers that fail BOTH the input-attachment and feedback-loop-layout reads.
	UseRenderTargetCopyForFeedback,
	EmulateColorWriteMask,
	PreferCoherentReadback,
	UseStagingImageForReadback,
	AvoidClearLoadOpRenderPass,
	GenerateMipmapManuallyForTallTextures,
	RewriteUniformIndexing,
	ForceFifoPresent,
	AlignSwapchainWidthTo32,
	/// Report no stencil buffer, so depth targets are created as plain D32_SFLOAT and neither a
	/// stencil attachment nor the stencil DATE pre-pass is ever emitted. For drivers that hang on
	/// a depth-stencil attachment rather than merely rendering it wrong; DATE falls back to
	/// primitive-ID tracking, then Full, then Off.
	DisableStencilBuffer,
	/// Steer the Auto renderer to Vulkan on this part. Declared by a rule on the OPENGL side,
	/// because the Auto decision asks the database through the GL strings the app probes at
	/// startup -- there is no Vulkan device yet when it is made.
	///
	/// The odd one out in this enum: it answers "which of the device's two roads is the better
	/// one", not "what do we do about a defect". It lives here anyway because the Auto decision
	/// already reads a workaround bit (UseRenderTargetCopyForFeedback: a driver whose GL cannot
	/// read the target in tile memory is better served by Vulkan), so this is the same mechanism
	/// with a different reason rather than a second one. Nothing in either backend consumes it;
	/// GSUtil::AndroidAutoPrefersVulkan is its only reader.
	PreferVulkanRenderer,
	/// Allocate the Vulkan stream rings from a HOST_CACHED memory type even when the only one the
	/// device offers is non-coherent, paying the per-region cache clean VKStreamBuffer::CommitMemory
	/// already issues. For GPUs whose host-visible write-combined memory makes CPU writes into a
	/// ring far more expensive than the clean.
	///
	/// The second preference in this enum rather than a defect: nothing renders differently either
	/// way, and the flush it turns on is the one the Vulkan spec requires of any non-coherent
	/// mapping. It never applies where the device offers a cached COHERENT type -- that road is
	/// better than both and is taken without asking the database. See GSStreamRingMemoryPolicy.h.
	PreferCachedStreamRingMemory,
	Count,
};

struct MobileDriverVersion
{
	u32 raw = 0;
	u16 major = 0;
	u16 minor = 0;
	u16 patch = 0;
	u32 build = 0;
	bool known = false;
	bool legacy_hash = false;
};

/// Everything the resolver is allowed to look at. Filled from VkPhysicalDeviceProperties on the
/// Vulkan path and from the GL strings otherwise.
struct MobileDriverContext
{
	MobileGpuApi api = MobileGpuApi::Unknown;
	u32 vendor_id = 0;
	u32 device_id = 0;
	u32 driver_version = 0;
	u32 driver_id = 0;
	u32 api_version = 0;
	u32 android_sdk = 0;
	u32 max_draw_indirect_count = 0;
	std::string_view driver_name;
	std::string_view driver_info;
	std::string_view api_version_string;
	/// Platform identity from outside the graphics API -- the SoC and board strings. The resolver
	/// reads these itself where the platform offers them (Android system properties, the Linux
	/// device tree); a caller that already knows them, or a test pinning a specific device without
	/// one, supplies them here and they are folded into the same hint string the rules match on.
	std::string_view platform_hints;
};

struct MobileDriverProfile
{
	static constexpr u32 DATABASE_VERSION = 1;

	MobileGpuApi api = MobileGpuApi::Unknown;
	MobileGpuDriver driver = MobileGpuDriver::Unknown;
	MobileDriverVersion version;
	u64 bugs = 0;
	u64 workarounds = 0;
	u32 matched_rule_count = 0;
	DriverProfileConfidence confidence = DriverProfileConfidence::Unknown;
	/// True when nothing in the table matched and the safe defaults are in force.
	bool conservative_fallback = true;
	std::string driver_name;

	constexpr bool HasBug(DriverBug bug) const
	{
		return (bugs & (u64{1} << static_cast<u8>(bug))) != 0;
	}

	constexpr bool UsesWorkaround(DriverWorkaround workaround) const
	{
		return (workarounds & (u64{1} << static_cast<u8>(workaround))) != 0;
	}
};

// Both sets are u64 bitfields, so neither enum may exceed 64 entries without widening them.
static_assert(static_cast<u8>(DriverBug::Count) <= 64);
static_assert(static_cast<u8>(DriverWorkaround::Count) <= 64);

struct MobileGsTuning
{
	bool constrained = true;
	bool prefer_new_textures = false;
	u32 pooled_targets = 96;
	u32 target_age = 8;
	u32 pooled_textures = 96;
	u32 texture_age = 6;
};

struct MobileGpuIdentity
{
	MobileGpuArchitecture architecture = MobileGpuArchitecture::Unknown;
	u16 model_number = 0;
	u8 core_count = 0;
	bool recognized = false;
	std::string name = "Unknown";
};

struct GpuProfileSelection
{
	GpuProfileOverride override_mode = GpuProfileOverride::Auto;
	RuntimeGpuProfile runtime_profile = RuntimeGpuProfile::Unknown;
	bool is_mediatek_soc = false;
	MobileGpuIdentity gpu;
	MobileGsTuning gs_tuning;
	MobileDriverProfile driver;
	std::string hints;
};

class GpuProfileDetector
{
public:
	static GpuProfileOverride ParseOverride(std::string_view value);
	static const char* OverrideToConfigString(GpuProfileOverride value);
	static const char* OverrideToString(GpuProfileOverride value);
	static const char* RuntimeProfileToString(RuntimeGpuProfile value);
	static const char* ArchitectureToString(MobileGpuArchitecture value);
	static const char* ApiToString(MobileGpuApi value);
	static const char* DriverToString(MobileGpuDriver value);
	static const char* BugToString(DriverBug value);
	static const char* WorkaroundToString(DriverWorkaround value);

	static GpuProfileSelection Resolve(std::string_view override_value, std::string_view gpu_vendor,
		std::string_view gpu_renderer_or_name);
	/// Overload that also resolves the driver profile. The three-argument form keeps working and
	/// simply leaves GpuProfileSelection::driver in its conservative-fallback state.
	static GpuProfileSelection Resolve(std::string_view override_value, std::string_view gpu_vendor,
		std::string_view gpu_renderer_or_name, const MobileDriverContext& driver_context);
};
