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

GpuProfileSelection ResolveMaliVK(const char* device_name, u32 packed_version,
	std::string_view platform_hints = std::string_view())
{
	MobileDriverContext context;
	context.api = MobileGpuApi::Vulkan;
	context.vendor_id = kMaliVendorId;
	context.driver_id = kArmDriverId;
	context.driver_version = packed_version;
	context.driver_name = "ARM proprietary";
	context.platform_hints = platform_hints;
	return GpuProfileDetector::Resolve("auto", std::string_view(), device_name, context);
}

// What the platform actually reports for the SoC, in both spellings the resolver can see it in.
// Android hands over system properties (ro.soc.model / ro.board.platform); a Linux handheld has no
// property service, so the identity comes from the device tree's compatible list. Passing them in
// through MobileDriverContext::platform_hints exercises the same string the real device produces
// without depending on the machine the test runs on.
constexpr const char* kMt6897AndroidHints = "ro.soc.manufacturer=Mediatek | ro.soc.model=MT6897 | "
										   "ro.board.platform=mt6897";
constexpr const char* kMt6897LinuxHints = "anbernic,rg477v mediatek,mt6897";
// A MediaTek part that is NOT the one we measured: the deny list still applies there.
constexpr const char* kOtherMediaTekHints = "ro.soc.manufacturer=Mediatek | ro.soc.model=MT6985 | "
										   "ro.board.platform=mt6985";

bool DeniesRoaaDestinationRead(const GpuProfileSelection& sel)
{
	return sel.driver.HasBug(DriverBug::BrokenRoaaDestinationRead);
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

// The exemption, and the whole point of step 2.2: the RG 477V's SoC is excluded from the r44p1
// crash rule, so at default settings it keeps the in-tile read instead of the render-target copy.
// The rule was written from a Motorola Edge 60 Pro; this device runs the same nominal driver
// revision and does not fault, so the exclusion is per SoC rather than per version. Both spellings
// of the SoC identity are pinned because the two runner paths see different ones.
TEST(GSGpuDriverProfile, Mt6897IsExemptFromTheR44p1SelfReadRule)
{
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(
		ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0), kMt6897AndroidHints)));
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(
		ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0), kMt6897LinuxHints)));
}

// The other half, and the one an over-eager exclusion breaks silently: every OTHER r44p1 device
// still takes the copy path. A hint_exclude that matched too much would put the founding device
// back on the read that loses it.
TEST(GSGpuDriverProfile, OtherR44p1DevicesStillTakeTheRenderTargetCopyPath)
{
	EXPECT_TRUE(TakesTheRenderTargetCopyPath(
		ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0), kOtherMediaTekHints)));
	EXPECT_TRUE(TakesTheRenderTargetCopyPath(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0),
		"ro.product.manufacturer=motorola | ro.product.model=edge 60 pro")));
	EXPECT_TRUE(TakesTheRenderTargetCopyPath(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0))));
}

// The MediaTek ROAA deny list, which used to be an inline vendor test in the Vulkan backend. Same
// scope as before on every part except the measured one.
TEST(GSGpuDriverProfile, MediaTekMaliDeniesTheRoaaDestinationReadExceptOnMt6897)
{
	EXPECT_TRUE(DeniesRoaaDestinationRead(
		ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0), kOtherMediaTekHints)));
	EXPECT_FALSE(DeniesRoaaDestinationRead(
		ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0), kMt6897AndroidHints)));
	EXPECT_FALSE(DeniesRoaaDestinationRead(
		ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0), kMt6897LinuxHints)));
}

// A Mali part on a SoC that is not MediaTek was never on the deny list and must not join it now --
// the inline test this replaced asked IsMediaTekSoC(), and the rule has to be exactly as narrow.
TEST(GSGpuDriverProfile, NonMediaTekMaliKeepsTheRoaaDestinationRead)
{
	EXPECT_FALSE(DeniesRoaaDestinationRead(ResolveMaliVK("Mali-G715", PackVulkanVersion(46, 0, 0),
		"ro.soc.manufacturer=Samsung | ro.board.platform=s5e9925")));
	EXPECT_FALSE(DeniesRoaaDestinationRead(ResolveMaliVK("Mali-G610", PackVulkanVersion(38, 1, 0))));
}

// Mali-G57 is denied on its model number, across SoC vendors, which is what the inline
// deviceName search did. Neighbouring models must not be caught by it.
TEST(GSGpuDriverProfile, MaliG57DeniesTheRoaaDestinationReadOnAnySoC)
{
	EXPECT_TRUE(DeniesRoaaDestinationRead(ResolveMaliVK("Mali-G57 MC2", PackVulkanVersion(32, 1, 0))));
	EXPECT_TRUE(DeniesRoaaDestinationRead(
		ResolveMaliVK("Mali-G57 MC2", PackVulkanVersion(32, 1, 0), kMt6897AndroidHints)));
	EXPECT_FALSE(DeniesRoaaDestinationRead(ResolveMaliVK("Mali-G52 MC2", PackVulkanVersion(32, 1, 0))));
	EXPECT_FALSE(DeniesRoaaDestinationRead(ResolveMaliVK("Mali-G77 MC9", PackVulkanVersion(32, 1, 0))));
}

// The deny list is a Vulkan rule about rasterization-order attachment access. The GL backend's
// fetch comes from GL_ARM_shader_framebuffer_fetch, which is a different mechanism on a different
// code path, and GSUtil::AndroidAutoPrefersVulkan asks the table through the GL path -- so a
// Vulkan-only rule leaking into it would silently reroute Auto for every MediaTek Mali device.
TEST(GSGpuDriverProfile, TheRoaaDenyListDoesNotReachTheOpenGLPath)
{
	MobileDriverContext context;
	context.api = MobileGpuApi::OpenGL;
	context.driver_name = kMaliR44p1GlRenderer;
	context.api_version_string = kMaliR44p1GlVersion;
	context.platform_hints = kOtherMediaTekHints;
	const GpuProfileSelection sel =
		GpuProfileDetector::Resolve("auto", kMaliR44p1GlVendor, kMaliR44p1GlRenderer, context);

	EXPECT_FALSE(DeniesRoaaDestinationRead(sel));
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(sel));
}

// ---------------------------------------------------------------------------------------------
// Turnip's D32S8 EARLY_Z_LATE_Z hang, and the version window that ends it.
//
// The gate this pins used to be `if (is_adreno) stencil_buffer = false;` in
// GSDeviceVK::CheckFeatures -- vendor-wide and driver-unbounded. Round 20260903-0135 turned it
// into a fact (A650 / turnip 26.1.2, 8 of 8 titles lost the device, devcoredump latching
// Z_MODE = A6XX_EARLY_Z_LATE_Z with DEPTH6_32 + SEPARATE_STENCIL) and Mesa a70d2af590d / MR !41858
// ended it in 26.2, so the clause became a driver rule with a version window.
//
// Both edges are load-bearing. Too wide and a fixed driver keeps paying the PrimID DATE road for
// a bug it no longer has; too narrow and an A650 on 26.1.x goes back to hanging the GPU within
// seconds of the first DATE draw, with no diagnostic beyond a kernel hangcheck.
namespace
{
// DRIVER_ID_MESA_TURNIP is 18, DRIVER_ID_QUALCOMM_PROPRIETARY is 8. Turnip reports Mesa's own
// version in driverVersion, so 26.1.2 arrives as major 26, minor 1, patch 2 (raw 0x06801002).
constexpr u32 kTurnipDriverId = 18;
constexpr u32 kQualcommProprietaryDriverId = 8;
constexpr u32 kAdrenoVendorId = 0x5143u;

GpuProfileSelection ResolveAdrenoVK(const char* device_name, u32 driver_id, const char* driver_name,
	u32 packed_version)
{
	MobileDriverContext context;
	context.api = MobileGpuApi::Vulkan;
	context.vendor_id = kAdrenoVendorId;
	context.driver_id = driver_id;
	context.driver_version = packed_version;
	context.driver_name = driver_name;
	return GpuProfileDetector::Resolve("auto", std::string_view(), device_name, context);
}

bool KillsTheStencilBuffer(const GpuProfileSelection& sel)
{
	return sel.driver.UsesWorkaround(DriverWorkaround::DisableStencilBuffer);
}
} // namespace

// The device the round ran on, at the version it ran at. This is the "did the rule stop firing"
// guard: nothing else turns the stencil buffer off on an A650, so a rule that quietly matches
// nothing puts the device straight back on the state that wedges it.
TEST(GSGpuDriverProfile, TurnipBefore26_2KillsTheStencilBufferOnAdreno650)
{
	const GpuProfileSelection sel =
		ResolveAdrenoVK("Adreno (TM) 650", kTurnipDriverId, "turnip", PackVulkanVersion(26, 1, 2));

	EXPECT_EQ(sel.runtime_profile, RuntimeGpuProfile::Adreno);
	EXPECT_EQ(sel.driver.driver, MobileGpuDriver::MesaTurnip);
	EXPECT_TRUE(sel.driver.version.known);
	EXPECT_EQ(sel.driver.version.major, 26);
	EXPECT_EQ(sel.driver.version.minor, 1);
	EXPECT_TRUE(sel.driver.HasBug(DriverBug::BrokenDepthStencilDiscard));
	EXPECT_TRUE(KillsTheStencilBuffer(sel));
}

// The upper edge. 26.2.0 is the first release carrying a70d2af590d, so it is the first release
// that gets its stencil buffer -- and with it the one-quad stencil DATE road -- back.
TEST(GSGpuDriverProfile, TurnipFrom26_2KeepsTheStencilBuffer)
{
	EXPECT_FALSE(KillsTheStencilBuffer(
		ResolveAdrenoVK("Adreno (TM) 650", kTurnipDriverId, "turnip", PackVulkanVersion(26, 2, 0))));
	EXPECT_FALSE(KillsTheStencilBuffer(
		ResolveAdrenoVK("Adreno (TM) 650", kTurnipDriverId, "turnip", PackVulkanVersion(26, 3, 0))));
	EXPECT_FALSE(KillsTheStencilBuffer(
		ResolveAdrenoVK("Adreno (TM) 750", kTurnipDriverId, "turnip", PackVulkanVersion(27, 0, 0))));
}

// Older turnip is inside the window too, and a turnip that reports no usable version resolves as
// "old" (match_unknown_version), because the failure mode on the wrong side of that guess is a
// GPU hang rather than a slower DATE path.
TEST(GSGpuDriverProfile, OlderAndUnversionedTurnipStayInsideTheWindow)
{
	EXPECT_TRUE(KillsTheStencilBuffer(
		ResolveAdrenoVK("Adreno (TM) 650", kTurnipDriverId, "turnip", PackVulkanVersion(25, 3, 6))));
	EXPECT_TRUE(KillsTheStencilBuffer(
		ResolveAdrenoVK("Adreno (TM) 650", kTurnipDriverId, "turnip", 0)));
}

// The narrowing this rule makes, stated so it cannot happen by accident. The founding commit
// (05998bc5c4) left the kill on the Adreno vendor ID and said why in its own notes: turnip was the
// only Adreno driver we shipped against. The blob was never tested for this hang, the decoded
// evidence is entirely turnip's, and an untested driver does not inherit another driver's bug --
// so the proprietary stack keeps its stencil buffer. If a blob device ever reproduces the hang it
// gets its own rule with its own evidence, not a widened version of this one.
TEST(GSGpuDriverProfile, ProprietaryQualcommKeepsItsStencilBuffer)
{
	const GpuProfileSelection sel = ResolveAdrenoVK(
		"Adreno (TM) 650", kQualcommProprietaryDriverId, "Qualcomm", 0x801EA000u);

	EXPECT_EQ(sel.driver.driver, MobileGpuDriver::QualcommProprietary);
	EXPECT_FALSE(KillsTheStencilBuffer(sel));
}
