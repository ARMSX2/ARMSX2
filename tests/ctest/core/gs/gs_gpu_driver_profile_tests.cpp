// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The driver-bug database's identity parsing, pinned against real device strings.
//
// The table matches rules on a PARSED driver version, not on a substring of the driver string. That
// is the whole point -- "before r44p1" and "exactly r44p1" are orderable questions a substring
// search cannot ask -- but it means a rule silently matches nothing when the parse does not produce
// the version the rule is written against. A gate that stops firing puts the affected device back
// on the faulting path with no diagnostic, which is strictly worse than the hand-rolled substring
// test it replaced.
//
// So every driver identity we key a rule on gets pinned here from the exact strings the device
// reports, captured from an emulog rather than reconstructed by hand.

#include "GS/Renderers/Common/GSGPUProfile.h"

#include <gtest/gtest.h>

namespace
{
// Anbernic RG 477V -- Mali-G615 MC6, MediaTek MT6897, Arm proprietary blob r44p1. This is the
// device behind the r44p1 self-read rules: the Vulkan copy-path gate that remains, and the GL
// gate that was deliberately lifted (both tests below pin their respective directions).
constexpr const char* kMaliR44p1GlVendor = "ARM";
constexpr const char* kMaliR44p1GlRenderer = "Mali-G615 MC6";
constexpr const char* kMaliR44p1GlVersion = "OpenGL ES 3.2 v1.r44p1-01eac0.030c4a3fb15fe65f485fb565f5e1b688";

// VkPhysicalDeviceDriverProperties reports Arm's revision in the packed Vulkan encoding, so an
// r44p1 blob arrives as major 44, minor 1, patch 0. DRIVER_ID_ARM_PROPRIETARY is 9.
constexpr u32 kArmDriverId = 9;
constexpr u32 kMaliVendorId = 0x13B5u;
constexpr u32 PackVulkanVersion(u32 major, u32 minor, u32 patch)
{
	return (major << 22) | (minor << 12) | patch;
}

GpuProfileSelection ResolveGL(const char* vendor, const char* renderer, const char* version)
{
	MobileDriverContext context;
	context.api = MobileGpuApi::OpenGL;
	context.driver_name = renderer;
	context.api_version_string = version;
	return GpuProfileDetector::Resolve("auto", vendor, renderer, context);
}

GpuProfileSelection ResolveMaliVK(const char* device_name, u32 packed_version)
{
	MobileDriverContext context;
	context.api = MobileGpuApi::Vulkan;
	context.vendor_id = kMaliVendorId;
	context.driver_id = kArmDriverId;
	context.driver_version = packed_version;
	context.driver_name = "ARM proprietary";
	return GpuProfileDetector::Resolve("auto", std::string_view(), device_name, context);
}

bool TakesTheRenderTargetCopyPath(const GpuProfileSelection& sel)
{
	return sel.driver.UsesWorkaround(DriverWorkaround::UseRenderTargetCopyForFeedback);
}
} // namespace

// The GL string carries the Arm driver revision in its vendor-specific tail ("v1.r44p1-..."), and
// that tail -- not the leading GLES version -- is the ordered driver identity. Reading "3.2" out of
// "OpenGL ES 3.2" would make every Arm GL rule match on the API version instead, so a rule written
// for r44p1 would match nothing while a rule written for "before r44p1" would match every Mali
// device ever made.
TEST(GSGpuDriverProfile, MaliOpenGLVersionComesFromTheArmRevisionNotTheGlesVersion)
{
	const GpuProfileSelection sel = ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion);

	EXPECT_EQ(sel.runtime_profile, RuntimeGpuProfile::Mali);
	EXPECT_EQ(sel.driver.driver, MobileGpuDriver::ArmProprietary);
	EXPECT_TRUE(sel.driver.version.known);
	EXPECT_EQ(sel.driver.version.major, 44);
	EXPECT_EQ(sel.driver.version.minor, 1);
}

// r44p1 on GL keeps the ARM framebuffer-fetch path DELIBERATELY -- the 2.6.6.5 rule that put it
// on the copy path collapsed SotC 30 -> 7 fps on the RG 477V and users downgraded en masse to
// 2.6.6.4, whose gate was inert; the full account sits above the GL rules in the database. This
// test pins the restoration: a rule quietly re-matching this device would re-ship the collapse,
// and (via GSUtil::AndroidAutoPrefersVulkan) silently reroute Auto to Vulkan too.
TEST(GSGpuDriverProfile, MaliR44p1KeepsTheInTileReadOnOpenGL)
{
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(
		ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion)));
}

// On Vulkan the same read is a device loss, not a corruption trade, so the copy path stays. The
// risk this asserts against is a parsed-version rule matching nothing while looking healthy -- no
// log line, no assertion, the device just quietly runs the path that kills it. So assert the
// outcome from the real device's packed version, not merely that the version parsed.
TEST(GSGpuDriverProfile, MaliR44p1TakesTheRenderTargetCopyPathOnVulkan)
{
	EXPECT_TRUE(TakesTheRenderTargetCopyPath(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0))));
}

// The coherent-readback preference (Dolphin's slow-cached-readback story) was measured
// BACKWARDS on r44p1/G615 (2026-08-17, crossing-cost probe: cached wins ~12x per readback,
// explicit invalidate included), so exactly the measured revision drops the workaround and
// every other revision — unknown versions included, which resolve as "old" — keeps it. This
// pins both directions: a rule drifting wide re-ships a 12x readback tax on the one Mali we
// measure; a rule drifting narrow silently changes memory types on hardware nobody measured.
TEST(GSGpuDriverProfile, MaliCoherentReadbackPreferenceIsVersionGatedAroundR44p1)
{
	const auto prefers_coherent = [](const GpuProfileSelection& sel) {
		return sel.driver.UsesWorkaround(DriverWorkaround::PreferCoherentReadback);
	};
	EXPECT_FALSE(prefers_coherent(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0))));
	EXPECT_TRUE(prefers_coherent(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 0, 0))));
	EXPECT_TRUE(prefers_coherent(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(43, 0, 0))));
	EXPECT_TRUE(prefers_coherent(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 2, 0))));
	EXPECT_TRUE(prefers_coherent(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(46, 0, 0))));
}

// The other half of the claim, and the one a too-broad rule breaks silently: the copy path costs
// real performance, so every Arm blob that is NOT r44p1 must keep the in-tile read. r44p0 and r44p2
// bracket the window; r38 and r52 are the neighbouring revisions other rules already key on.
TEST(GSGpuDriverProfile, NeighbouringMaliRevisionsKeepTheInTileRead)
{
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 0, 0))));
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 2, 0))));
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(ResolveMaliVK("Mali-G610", PackVulkanVersion(38, 1, 0))));
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(ResolveMaliVK("Mali-G715", PackVulkanVersion(52, 0, 0))));
}

// Same on the GL side, where the revision is read out of the version string's vendor tail. A
// Mali-G615 on a good blob is the case that must not regress: it is the same chip as the RG 477V.
TEST(GSGpuDriverProfile, OtherMaliOpenGLRevisionsKeepTheInTileRead)
{
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(
		ResolveGL("ARM", "Mali-G615 MC6", "OpenGL ES 3.2 v1.r44p0-01eac0.deadbeefdeadbeefdeadbeefdeadbeef")));
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(
		ResolveGL("ARM", "Mali-G615 MC6", "OpenGL ES 3.2 v1.r45p1-01eac0.deadbeefdeadbeefdeadbeefdeadbeef")));
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(
		ResolveGL("ARM", "Mali-G57 MC2", "OpenGL ES 3.2 v1.r32p1-01eac0.deadbeefdeadbeefdeadbeefdeadbeef")));
}

// The old "dynamic rendering broken before r52" window was compiled from public bug databases
// with no cited defect and no measured boundary, and it overreached: r44p1 reports Vulkan 1.3,
// where dynamicRendering is a mandatory, CTS-exercised core feature. Nothing consumes the bit in
// the classic backend (it runs render-pass objects everywhere), but the Tile renderer's per-device
// cost model will, and a false "broken" on r44p1 would silently veto its dynamic-rendering A/B on
// the one Mali device on the bench. The flag survives only below r44p1, where there is no 1.3
// conformance claim to lean on and no hardware here to check.
TEST(GSGpuDriverProfile, MaliR44p1DynamicRenderingIsNotFlaggedBroken)
{
	EXPECT_FALSE(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0))
					 .driver.HasBug(DriverBug::BrokenDynamicRendering));
	// The narrowed window still holds below r44p1, and r52+ was never flagged.
	EXPECT_TRUE(ResolveMaliVK("Mali-G610", PackVulkanVersion(38, 1, 0))
					.driver.HasBug(DriverBug::BrokenDynamicRendering));
	EXPECT_FALSE(ResolveMaliVK("Mali-G715", PackVulkanVersion(52, 0, 0))
					 .driver.HasBug(DriverBug::BrokenDynamicRendering));
}

// The Vulkan device publishes the resolved runtime profile on EVERY platform, not just
// Android: the ARM Linux handhelds run the same Turnip/blob stacks as the phones, and the
// renderer-variant selection reads the profile (an Android-only publish left it Unknown for
// the whole Vulkan lifetime on the SD865 rig -- observed as "profile=Unknown" in the Tile
// identity line on Rocknix). What makes the unconditional publish safe is this property of
// the VK classification: mobile tilers match by name, and desktop GPUs stay strictly
// Unknown -- unlike the GL detector, which calls every non-Mali GPU Adreno and must remain
// Android-only. Strings below are from real emulogs (SD865 Rocknix rig, M2 Asahi dev box).
TEST(GSGpuDriverProfile, VulkanRuntimeProfileClassifiesTilersAndLeavesDesktopUnknown)
{
	const auto resolve_vk = [](const char* device_name, const char* driver_name, u32 vendor_id,
							    u32 driver_id) {
		MobileDriverContext context;
		context.api = MobileGpuApi::Vulkan;
		context.vendor_id = vendor_id;
		context.driver_id = driver_id;
		context.driver_name = driver_name;
		return GpuProfileDetector::Resolve("auto", std::string_view(), device_name, context);
	};

	// Turnip on the SD865 rig (VK_DRIVER_ID_MESA_TURNIP = 18, Qualcomm vendor 0x5143).
	EXPECT_EQ(resolve_vk("Turnip Adreno (TM) 650", "turnip", 0x5143u, 18).runtime_profile,
		RuntimeGpuProfile::Adreno);
	// The proprietary blob names the GPU the same way (VK_DRIVER_ID_QUALCOMM_PROPRIETARY = 7).
	EXPECT_EQ(resolve_vk("Adreno (TM) 650", "Qualcomm Technologies Inc. Adreno Vulkan Driver",
				  0x5143u, 7)
				  .runtime_profile,
		RuntimeGpuProfile::Adreno);
	// The Mali blob on the RG477V (VK_DRIVER_ID_ARM_PROPRIETARY = 9).
	EXPECT_EQ(resolve_vk("Mali-G615 MC6", "ARM proprietary", 0x13B5u, 9).runtime_profile,
		RuntimeGpuProfile::Mali);

	// Desktop and dev-box GPUs must resolve Unknown -- a wrong tiler answer here would feed
	// the variant Auto policy and the per-vendor GS tuning a desktop GPU.
	EXPECT_EQ(resolve_vk("NVIDIA GeForce RTX 4080", "NVIDIA", 0x10DEu, 4).runtime_profile,
		RuntimeGpuProfile::Unknown);
	EXPECT_EQ(resolve_vk("AMD Radeon RX 7900 XTX (RADV NAVI31)", "radv", 0x1002u, 3).runtime_profile,
		RuntimeGpuProfile::Unknown);
	EXPECT_EQ(resolve_vk("Intel(R) Arc(tm) A770 Graphics (DG2)", "Intel open-source Mesa driver",
				  0x8086u, 6)
				  .runtime_profile,
		RuntimeGpuProfile::Unknown);
	EXPECT_EQ(resolve_vk("Apple M2 Max (G14C B1)", "Honeykrisp", 0x106Bu, 0).runtime_profile,
		RuntimeGpuProfile::Unknown);
}
