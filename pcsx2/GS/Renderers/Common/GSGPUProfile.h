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
	/// Apple Silicon. A tiler like the mobile parts, but it must never inherit their workarounds;
	/// without this value desktop GL resolved it to Adreno and ran Adreno-only paths. Distinct
	/// from Unknown so its tiler-ness can be acted on deliberately.
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

// Driver identity and known-bug model, ported from EmuCoreX (sashkinbro) with his approval.
// The GPU family alone does not decide behaviour: the same Mali part behaves differently under
// Arm's driver and under Mesa PanVK. Rules are keyed on driver + version + a bug set, so a new
// device quirk is a table entry rather than another branch.

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
	/// The driver advertises rasterization-order attachment access and returns zero or stale
	/// colour from the destination read (black or missing textures). Distinct from
	/// BrokenSubpassFeedback, where the in-pass self-read loses whole draws or the device.
	BrokenRoaaDestinationRead,
	/// The driver sometimes applies CONST_COLOR / INV_CONST_COLOR as if the constant were zero.
	/// The trigger is run history, not anything the draw carries, so it cannot be probed at
	/// start-up and no emission order avoids it.
	BrokenBlendConstant,
	Count,
};

/// What we do about a bug. Separate from DriverBug because one mitigation answers several
/// defects, and a workaround can be forced on for testing without claiming the bug.
enum class DriverWorkaround : u8
{
	RewriteBooleanNegation,
	ScalarizeVectorBitwiseAnd,
	StoreBitwiseNegationInTemporary,
	UseDescriptorSets,
	DisableProvokingVertex,
	DisableAttachmentFeedbackLoopLayout,
	/// Read the render target from a copy instead of in-pass. Turns texture barriers off, which
	/// also disables framebuffer fetch, so the RT is never bound and sampled at once. Costs a full
	/// RT copy per feedback draw: a last resort for drivers that fail both the input-attachment
	/// and the feedback-loop-layout reads.
	UseRenderTargetCopyForFeedback,
	EmulateColorWriteMask,
	PreferCoherentReadback,
	UseStagingImageForReadback,
	AvoidClearLoadOpRenderPass,
	GenerateMipmapManuallyForTallTextures,
	RewriteUniformIndexing,
	ForceFifoPresent,
	AlignSwapchainWidthTo32,
	/// Report no stencil buffer: depth targets are plain D32_SFLOAT and no stencil DATE pre-pass
	/// is emitted. For drivers that hang on a depth-stencil attachment. DATE falls back to
	/// primitive-ID tracking, then Full, then Off.
	DisableStencilBuffer,
	/// Steer the Auto renderer to Vulkan on this part. Declared by an OpenGL-side rule, because
	/// Auto is decided from the GL strings before any Vulkan device exists. A preference, not a
	/// defect workaround; GSUtil::AndroidAutoPrefersVulkan is its only reader.
	PreferVulkanRenderer,
	/// Allocate the Vulkan stream rings from a HOST_CACHED memory type instead of the
	/// write-combined one VMA picks, for GPUs where write-combined CPU writes cost more than
	/// cached stores plus cache maintenance. The memory table picks coherent if available, else
	/// non-coherent with the clean VKStreamBuffer::CommitMemory already issues. A preference, not
	/// a defect. Without this bit the rings stay write-combined even when a cached coherent type
	/// exists (that road lost on the SD865). See GSStreamRingMemoryPolicy.h.
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

	/// Generation of the declared-feedback-loop ordering fix this driver build carries, from its
	/// driverInfo tag; 0 when there is no tag. See ParseDeclaredLoopFixGeneration.
	u32 declared_loop_fix_generation = 0;

	/// This driver orders overlapping self-reads inside a declared attachment feedback loop.
	/// ⚠️ No extension promises this: stock Turnip emits the ordering mode and does not deliver
	/// it (see GSSelfReadRoadPolicy.h). True only for our own builds, measured byte-identical to
	/// the barrier-keeping reference and tagged in driverInfo; every other driver keeps barriers.
	bool orders_declared_feedback_loop = false;

	/// This driver's best in-pass self-read road is a declared attachment feedback loop with the
	/// per-draw barriers kept: the declaration gives the layout and coherent destination read,
	/// our barriers give the ordering. Weaker than orders_declared_feedback_loop, which lets the
	/// barriers go and wins if both are set.
	///
	/// True for Turnip on Adreno 730 and up (measured on the 740), where the copy road renders
	/// wrong. The barrier-less declared road races on a7xx because Turnip never emits the
	/// ordering state there.
	bool prefers_declared_loop_with_barriers = false;

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

/// PCI vendor IDs as Vulkan reports them in VkPhysicalDeviceProperties::vendorID. Apple silicon
/// reports a different ID per driver, so it is identified by driver instead.
namespace GpuVendorID
{
constexpr u32 AMD = 0x1002;
constexpr u32 NVIDIA = 0x10DE;
constexpr u32 Intel = 0x8086;
constexpr u32 ARM = 0x13B5;
constexpr u32 Qualcomm = 0x5143;
constexpr u32 Imagination = 0x1010;
constexpr u32 Broadcom = 0x14E4;
/// Samsung Xclipse. Not confirmed on a device: a driver reporting another ID leaves every check
/// against this one inert.
constexpr u32 Samsung = 0x144D;
} // namespace GpuVendorID

/// Vulkan device rules keyed on the device's own identity (vendor ID, device name, driver ID,
/// driverInfo) rather than matched in the driver-bug database. Each keeps the exact condition the
/// backend has always applied, which is not always the database's: the push-descriptor rule covers
/// Mali on every driver, where the database names Arm's.
struct VulkanDeviceRules
{
	/// Mali-G615: timestamp queries never resolve, and the present spin that waits on them stalls.
	bool broken_timestamp_queries = false;
	/// Mali on a driver whose driverInfo names r44p1: the attachment-feedback-loop layout is not
	/// used. The rest of the r44p1 workaround is rule vk-arm-r44p1-attachment-self-read.
	bool avoid_feedback_loop_layout = false;
	/// Mali crashes inside vkCmdPushDescriptorSetKHR. Adreno is trusted with push descriptors on the
	/// Qualcomm driver and Turnip only.
	bool avoid_push_descriptors = false;
	/// Adreno on the Qualcomm driver selects the wrong provoking vertex.
	bool broken_provoking_vertex = false;
	/// Adreno 5xx, or a Qualcomm driver older than 0x801EA000, ignores colorWriteMask while a depth
	/// test is active. That version is in the Qualcomm encoding, so Turnip is excluded outright.
	bool broken_colormask_with_depth = false;
	/// Mali-G57: the FastMAD history banks read back stale, so deinterlace uses weave and blend.
	bool broken_mad_deinterlace = false;
	/// Adreno 8xx on the Qualcomm driver: rasterization-order reads return stale colour above
	/// Basic blending. Turnip on the same parts is fine.
	bool adreno8xx_proprietary = false;
	/// Turnip or Honeykrisp: the drivers on which the in-pass self-read was measured. A per-draw
	/// barrier there costs about what a per-draw copy does, and the loop is declared per draw.
	bool self_read_costs_measured = false;
	/// Honeykrisp: the barrier-ordered road's fast stencil shadow and carry were measured there.
	bool barrier_road_measured = false;
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
	/// Also resolves the driver profile. The three-argument form leaves
	/// GpuProfileSelection::driver in its conservative-fallback state.
	static GpuProfileSelection Resolve(std::string_view override_value, std::string_view gpu_vendor,
		std::string_view gpu_renderer_or_name, const MobileDriverContext& driver_context);

	static constexpr u64 BugMask(DriverBug bug) { return u64{1} << static_cast<u8>(bug); }

	/// Bugs OR'd into every resolved profile whatever the database says, so a test harness can
	/// reach a workaround road on a driver without the defect. Set once by gsrunner before the VM
	/// starts. Deliberately not a user setting: users cannot know which bugs their driver has.
	static void SetForcedBugs(u64 mask);
	static u64 GetForcedBugs();

	/// The generation from a `git-axfl<G>-` build tag in a Vulkan driverInfo string, or 0 if none.
	/// Exposed for tests; the resolver publishes it as declared_loop_fix_generation.
	static u32 ParseDeclaredLoopFixGeneration(std::string_view driver_info);

	/// The Vulkan device rules for a device. `selection` is the result of Resolve on the same
	/// context; `device_name` is VkPhysicalDeviceProperties::deviceName.
	static VulkanDeviceRules ResolveVulkanDeviceRules(const GpuProfileSelection& selection,
		const MobileDriverContext& context, std::string_view device_name);
};
