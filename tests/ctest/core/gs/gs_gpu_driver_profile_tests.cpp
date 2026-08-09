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
// device behind the r44p1 render-target self-read gates on both GL and Vulkan.
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

// The gate this replaced was a substring search for "r44p1" in GL_VERSION, and it worked. The whole
// risk of moving it into the table is that a parsed-version rule matches nothing while looking
// perfectly healthy -- no log line, no assertion, the device just quietly runs the path that kills
// it. So assert the outcome from the real string, not merely that the version parsed.
TEST(GSGpuDriverProfile, MaliR44p1TakesTheRenderTargetCopyPathOnOpenGL)
{
	EXPECT_TRUE(TakesTheRenderTargetCopyPath(
		ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion)));
}

TEST(GSGpuDriverProfile, MaliR44p1TakesTheRenderTargetCopyPathOnVulkan)
{
	EXPECT_TRUE(TakesTheRenderTargetCopyPath(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0))));
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
