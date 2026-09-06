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

#include "GS/GSUtil.h"
#include "GS/Renderers/Common/GSGPUProfile.h"

#include <gtest/gtest.h>

#include <string>

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

GpuProfileSelection ResolveGL(const char* vendor, const char* renderer, const char* version,
	std::string_view platform_hints = std::string_view())
{
	MobileDriverContext context;
	context.api = MobileGpuApi::OpenGL;
	context.driver_name = renderer;
	context.api_version_string = version;
	context.platform_hints = platform_hints;
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

// The RG 477V's own hint string, read off the device over adb on 2026-09-03 and written out in
// full: every ro.* property GSGPUProfile.cpp's BuildHints asks for, in the order it asks for them,
// joined the way AppendHint joins them, with the ones the device leaves empty dropped. The
// constants above are shortened by hand to the part a rule keys on; this one is the whole string
// the app's probe actually hands the resolver, so a property renamed, dropped or reordered on the
// Android side is caught here instead of on the device.
//
// ro.build.fingerprint is deliberately not in it -- BuildHints does not read that property, and a
// hint the resolver never sees would make this pin claim coverage it does not have. Recorded here
// instead, since it is the one string that identifies the exact build the strings came off:
// alps/vext_k6897v1_64/k6897v1_64:14/UP1A.231005.007/V654202605290319:user/test-keys.
//
// Worth seeing in the full string: three properties carry "mt6897" and the board does not.
// ro.product.board is k6897v1_64 -- the part number without the vendor prefix -- so a device that
// reported only its board would not satisfy the rule.
constexpr const char* kRg477vDeviceHints2026_09_03 =
	"ro.soc.manufacturer=Mediatek | ro.soc.model=MT6897 | ro.board.platform=mt6897 | "
	"ro.hardware=mt6897 | ro.product.board=k6897v1_64";
// The same device with one thing changed: a different MediaTek part, spelled through the same five
// properties. Nothing about the GPU or the driver moves, which is the point -- the steering is a
// statement about a measured SoC, and this is the nearest neighbour that was not measured.
constexpr const char* kMt6895BoardHints2026_09_03 =
	"ro.soc.manufacturer=Mediatek | ro.soc.model=MT6895 | ro.board.platform=mt6895 | "
	"ro.hardware=mt6895 | ro.product.board=k6895v1_64";

bool DeniesRoaaDestinationRead(const GpuProfileSelection& sel)
{
	return sel.driver.HasBug(DriverBug::BrokenRoaaDestinationRead);
}

bool TakesTheRenderTargetCopyPath(const GpuProfileSelection& sel)
{
	return sel.driver.UsesWorkaround(DriverWorkaround::UseRenderTargetCopyForFeedback);
}

bool DatabasePrefersVulkan(const GpuProfileSelection& sel)
{
	return sel.driver.UsesWorkaround(DriverWorkaround::PreferVulkanRenderer);
}

// The Auto renderer decision itself, run the way the Android app runs it -- the GL strings the
// device reports, plus the SoC hint the platform would have supplied. Android passes no hints and
// lets the resolver read the system properties; the tests pass them so a device can be pinned from
// a desktop.
bool AutoPrefersVulkan(const char* vendor, const char* renderer, const char* version,
	std::string_view platform_hints = std::string_view())
{
	return GSUtil::AndroidAutoPrefersVulkan(vendor, renderer, version, platform_hints);
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

// ---------------------------------------------------------------------------------------------
// The stream rings' memory preference, which is the other half of GSStreamRingMemoryPolicy: the
// policy asks the database whether the write-combined road is worth leaving here, and this rule is
// the answer for the one part where that was measured.

namespace
{
	bool PrefersCachedStreamRings(const GpuProfileSelection& sel)
	{
		return sel.driver.UsesWorkaround(DriverWorkaround::PreferCachedStreamRingMemory);
	}
} // namespace

// The MQ65's A610 on Turnip: the part the round measured, and the only part that claims this.
TEST(GSGpuDriverProfile, TurnipOnAdreno610PrefersCachedStreamRingMemory)
{
	const GpuProfileSelection sel =
		ResolveAdrenoVK("Adreno (TM) 610", kTurnipDriverId, "turnip", PackVulkanVersion(26, 1, 2));

	EXPECT_EQ(sel.driver.driver, MobileGpuDriver::MesaTurnip);
	EXPECT_TRUE(PrefersCachedStreamRings(sel));
}

// Every other Adreno on the same driver does not, and the A650 is the interesting one: it HAS a
// cached coherent type, the policy used to take that road on the strength of the table alone, and
// the keyless confirmation round then measured it losing legosw +6.16% and ac5 +5.54% on that
// part. So it claims nothing here and stays write-combined. The low tiers next to the 610 -- 605,
// 608, 612, 618, 619, 620 -- are unmeasured, and "no cached coherent type" is precisely the
// argument that failed on Mali, so they are candidates to measure rather than devices to
// include.
TEST(GSGpuDriverProfile, OtherAdrenoPartsMakeNoStreamRingMemoryClaim)
{
	for (const char* device : {"Adreno (TM) 608", "Adreno (TM) 619", "Adreno (TM) 650", "Adreno (TM) 750"})
	{
		const GpuProfileSelection sel =
			ResolveAdrenoVK(device, kTurnipDriverId, "turnip", PackVulkanVersion(26, 1, 2));
		EXPECT_EQ(sel.driver.driver, MobileGpuDriver::MesaTurnip);
		EXPECT_FALSE(PrefersCachedStreamRings(sel)) << device;
	}
}

// The blob on the same silicon does not inherit it: nobody has read its memory table, and its
// cache maintenance is not turnip's.
TEST(GSGpuDriverProfile, ProprietaryQualcommMakesNoStreamRingMemoryClaim)
{
	const GpuProfileSelection sel =
		ResolveAdrenoVK("Adreno (TM) 610", kQualcommProprietaryDriverId, "Qualcomm", 0x801EA000u);

	EXPECT_EQ(sel.driver.driver, MobileGpuDriver::QualcommProprietary);
	EXPECT_FALSE(PrefersCachedStreamRings(sel));
}

// The RG 477V, which is why the rule is one part wide. Its Mali-G615 has no cached coherent type
// either -- the same shape as the A610 -- and taking its cached non-coherent type made every title
// in the suite slower, +2.6% to +12.4%, scaling with the flush count. A rule keyed on "no cached
// coherent type" would have shipped that regression.
TEST(GSGpuDriverProfile, MaliMakesNoStreamRingMemoryClaim)
{
	EXPECT_FALSE(PrefersCachedStreamRings(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0))));
	EXPECT_FALSE(PrefersCachedStreamRings(ResolveMaliVK("Mali-G610", PackVulkanVersion(38, 1, 0))));
}

// ---------------------------------------------------------------------------------------------
// Turnip ignoring the blend constant, and why the rule has no version bound at either end.
//
// A CONST_COLOR / INV_CONST_COLOR blend factor is applied as if the constant were zero on some
// draws, so the term it scales survives at full strength. Katamari Damacy's ball is the visible
// case. The reach is what these tests pin: two Adreno generations, every Mesa we have, and the
// proprietary blob on the same silicon correct.

namespace
{
bool IgnoresTheBlendConstant(const GpuProfileSelection& sel)
{
	return sel.driver.HasBug(DriverBug::BrokenBlendConstant);
}
} // namespace

// The three parts it was reproduced on. The a740 on 26.3.0-devel is the one that fixes the top of
// the window open: it is the newest Turnip anybody here can run, and it is still wrong.
TEST(GSGpuDriverProfile, TurnipIgnoresTheBlendConstantOnEveryAdrenoAndEveryMesa)
{
	EXPECT_TRUE(IgnoresTheBlendConstant(
		ResolveAdrenoVK("Adreno (TM) 650", kTurnipDriverId, "turnip", PackVulkanVersion(26, 1, 2))));
	EXPECT_TRUE(IgnoresTheBlendConstant(
		ResolveAdrenoVK("Adreno (TM) 610", kTurnipDriverId, "turnip", PackVulkanVersion(26, 1, 2))));
	EXPECT_TRUE(IgnoresTheBlendConstant(
		ResolveAdrenoVK("Adreno (TM) 740", kTurnipDriverId, "turnip", PackVulkanVersion(26, 2, 99))));

	// And parts and versions nobody has run, in both directions. A source check of the 259 Turnip
	// commits between 26.1.2 and main found no blend-constant fix and no change to either the factor
	// mapping or the constant emission, so there is nothing that would justify an upper bound; an
	// older Mesa has no claim to being better either.
	EXPECT_TRUE(IgnoresTheBlendConstant(
		ResolveAdrenoVK("Adreno (TM) 750", kTurnipDriverId, "turnip", PackVulkanVersion(27, 0, 0))));
	EXPECT_TRUE(IgnoresTheBlendConstant(
		ResolveAdrenoVK("Adreno (TM) 630", kTurnipDriverId, "turnip", PackVulkanVersion(24, 0, 0))));
}

// The Qualcomm blob renders Katamari correctly on the same Adreno 740, which is what makes this the
// driver's rather than the hardware's. So it claims nothing here, and neither does a GPU on another
// vendor's stack.
TEST(GSGpuDriverProfile, NobodyElseIgnoresTheBlendConstant)
{
	EXPECT_FALSE(IgnoresTheBlendConstant(
		ResolveAdrenoVK("Adreno (TM) 740", kQualcommProprietaryDriverId, "Qualcomm", 0x801EA000u)));
	EXPECT_FALSE(IgnoresTheBlendConstant(
		ResolveAdrenoVK("Adreno (TM) 650", kQualcommProprietaryDriverId, "Qualcomm", 0x801EA000u)));
	EXPECT_FALSE(IgnoresTheBlendConstant(ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0))));
	EXPECT_FALSE(IgnoresTheBlendConstant(ResolveMaliVK("Mali-G610", PackVulkanVersion(38, 1, 0))));
}

// ---------------------------------------------------------------------------------------------
// The Auto renderer on the MT6897, and the rule that steers it.
//
// Auto sent this device to OpenGL for as long as it has existed, and correctly so: its GL driver
// runs GL_ARM_shader_framebuffer_fetch, and the Vulkan side was denied the in-tile destination
// read by two rules above, which left every source-alpha blend copying the render target. Both
// denies now exempt this SoC, so the Vulkan road reads the destination in tile memory and a full
// 22-dump device round came out 21 of 22 titles under budget at p95 (geometric mean frame time
// 6.17 ms against 7.93 before). That makes Vulkan the better default here, and the two facts are
// one decision -- so the steering rule keys on the SAME SoC hint as the exemptions do.
//
// The failure this pins is drift between them: an exemption narrowed or a hint respelled on one
// side and not the other leaves the device on the renderer whose fast path it no longer has, and
// nothing in a log or a frame says so.

// The rule is a preference, not a defect: no bug bit, no copy path, and it is declared on the
// OPENGL side because the Auto decision is made from GL strings before any Vulkan device exists.
TEST(GSGpuDriverProfile, Mt6897CarriesTheVulkanPreferenceOnTheOpenGLPath)
{
	const GpuProfileSelection android =
		ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion, kMt6897AndroidHints);
	const GpuProfileSelection linux_dt =
		ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion, kMt6897LinuxHints);

	EXPECT_TRUE(DatabasePrefersVulkan(android));
	EXPECT_TRUE(DatabasePrefersVulkan(linux_dt));
	// The GL road it leaves behind is untouched: no bug claimed, no render-target copy imposed.
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(android));
	EXPECT_FALSE(DeniesRoaaDestinationRead(android));
}

// The exclusion half. A hint_require that matched too loosely would move every MediaTek Mali
// device -- or every Mali device -- to a renderer none of them was measured on.
TEST(GSGpuDriverProfile, OtherMaliPartsCarryNoVulkanPreference)
{
	EXPECT_FALSE(DatabasePrefersVulkan(
		ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion, kOtherMediaTekHints)));
	EXPECT_FALSE(DatabasePrefersVulkan(
		ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion)));
	// The rule is keyed on the vendor as well as the SoC, and both keys carry weight: a part that
	// is not Mali does not inherit the preference even standing on the measured chipset.
	EXPECT_FALSE(DatabasePrefersVulkan(
		ResolveGL("Qualcomm", "Adreno (TM) 650", "OpenGL ES 3.2 V@0676.0", kMt6897AndroidHints)));
}

// And the Vulkan path never sees it: the rule answers a question only the GL-side resolution is
// ever asked, and a copy of it on the Vulkan side would be a second place for the two to disagree.
TEST(GSGpuDriverProfile, TheVulkanPreferenceDoesNotReachTheVulkanPath)
{
	EXPECT_FALSE(DatabasePrefersVulkan(
		ResolveMaliVK("Mali-G615 MC6", PackVulkanVersion(44, 1, 0), kMt6897AndroidHints)));
}

// The decision as the app makes it, end to end. Both SoC spellings resolve Auto to Vulkan.
TEST(GSGpuDriverProfile, AutoResolvesToVulkanOnMt6897)
{
	EXPECT_TRUE(AutoPrefersVulkan(
		kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion, kMt6897AndroidHints));
	EXPECT_TRUE(AutoPrefersVulkan(
		kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion, kMt6897LinuxHints));
}

// The other polarity, which is the one that costs a whole device class if it is wrong: every other
// Mali part keeps OpenGL, including the same driver revision on a different MediaTek SoC and the
// same strings with no SoC hint at all. Sending an unmeasured Mali to Vulkan re-ships the 2.6.6.5
// complaint in the opposite direction -- the GL fetch path there is the fast one.
TEST(GSGpuDriverProfile, AutoStaysOnOpenGLForEveryOtherMaliPart)
{
	EXPECT_FALSE(AutoPrefersVulkan(
		kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion, kOtherMediaTekHints));
	EXPECT_FALSE(AutoPrefersVulkan(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion));
	EXPECT_FALSE(AutoPrefersVulkan(kMaliR44p1GlVendor, "Mali-G715",
		"OpenGL ES 3.2 v1.r46p0-01eac0.deadbeefdeadbeefdeadbeefdeadbeef", kOtherMediaTekHints));
	EXPECT_FALSE(AutoPrefersVulkan(kMaliR44p1GlVendor, "Mali-G57 MC2",
		"OpenGL ES 3.2 v1.r32p1-01eac0.deadbeefdeadbeefdeadbeefdeadbeef"));
}

// Adreno was steered to Vulkan long before any of this and must still be, for its own reason. The
// risk is ordering: a new term added ahead of the vendor test that answered first would change
// which reason the log reports for a device whose answer did not change.
TEST(GSGpuDriverProfile, AutoStillResolvesToVulkanOnAdrenoForItsOwnReason)
{
	EXPECT_TRUE(AutoPrefersVulkan("Qualcomm", "Adreno (TM) 650", "OpenGL ES 3.2 V@0676.0"));
	EXPECT_NE(std::string(GSUtil::AndroidAutoRendererReason()).find("Adreno"), std::string::npos);

	EXPECT_TRUE(AutoPrefersVulkan(
		kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion, kMt6897AndroidHints));
	EXPECT_NE(std::string(GSUtil::AndroidAutoRendererReason()).find("SoC"), std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// The RG 477V as it actually reports itself, 2026-09-03.
//
// The six cases above were written from an emulog and pin the rule's logic. These two pin the
// device: the GL strings dumpsys SurfaceFlinger prints and the whole ro.* hint string the app's
// JNI probe builds, both verbatim, fed through the same two entry points the app uses. The
// hand-shortened constants and the real ones agree today; if a property is renamed on the Android
// side, or BuildHints stops asking for one, only this pair notices.
//
// The end-to-end launch on the device was not run to confirm this. The debug applicationId equals
// the shipping one, so installing a test build replaces the user's install, and a side-by-side
// package would need the BIOS and a game copied into its own private storage before it could get
// as far as a renderer decision.

// GL vendor "ARM", renderer "Mali-G615 MC6", version "...v1.r44p1-01eac0.030c4a3fb15fe65f485fb565f5e1b688",
// with the device's five non-empty SoC properties: the preference rule matches, and Auto says
// Vulkan for the database's reason rather than for one of the other two.
TEST(GSGpuDriverProfile, Rg477vAdbStrings20260903ResolveAutoToVulkan)
{
	const GpuProfileSelection with_soc = ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer,
		kMaliR44p1GlVersion, kRg477vDeviceHints2026_09_03);
	const GpuProfileSelection without_soc =
		ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion);

	EXPECT_EQ(with_soc.runtime_profile, RuntimeGpuProfile::Mali);
	EXPECT_TRUE(with_soc.is_mediatek_soc);

	// gl-mt6897-prefer-vulkan is the only rule in the table that declares PreferVulkanRenderer, so
	// the bit names the rule. The count says it is an ADDITIONAL match rather than a rule that
	// changed what it claims -- no GL rule excludes this SoC, so the same strings without the hint
	// match everything this one does bar the new rule.
	EXPECT_TRUE(DatabasePrefersVulkan(with_soc));
	EXPECT_EQ(with_soc.driver.matched_rule_count, without_soc.driver.matched_rule_count + 1);

	// It stays a preference: no defect claimed against the GL driver it steers away from.
	EXPECT_FALSE(TakesTheRenderTargetCopyPath(with_soc));
	EXPECT_FALSE(DeniesRoaaDestinationRead(with_soc));

	EXPECT_TRUE(AutoPrefersVulkan(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion,
		kRg477vDeviceHints2026_09_03));
	// Which of the three terms answered matters as much as the answer: this device is not an
	// Adreno, and its GL driver reads the render target in tile memory perfectly well.
	EXPECT_STREQ(GSUtil::AndroidAutoRendererReason(), "the driver database prefers Vulkan on this SoC");
}

// The same strings with the SoC changed to a part nobody measured. This is the case that decides
// how far the flip travels: the GPU, the driver revision and the vendor are identical, so if the
// rule keyed on any of those instead of on the SoC, this device would move too.
TEST(GSGpuDriverProfile, Rg477vAdbStrings20260903OnAnMt6895BoardStayOnOpenGL)
{
	const GpuProfileSelection other_soc = ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer,
		kMaliR44p1GlVersion, kMt6895BoardHints2026_09_03);
	const GpuProfileSelection without_soc =
		ResolveGL(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion);

	EXPECT_EQ(other_soc.runtime_profile, RuntimeGpuProfile::Mali);
	EXPECT_TRUE(other_soc.is_mediatek_soc);

	EXPECT_FALSE(DatabasePrefersVulkan(other_soc));
	EXPECT_EQ(other_soc.driver.matched_rule_count, without_soc.driver.matched_rule_count);

	EXPECT_FALSE(AutoPrefersVulkan(kMaliR44p1GlVendor, kMaliR44p1GlRenderer, kMaliR44p1GlVersion,
		kMt6895BoardHints2026_09_03));
	EXPECT_STREQ(GSUtil::AndroidAutoRendererReason(), "no rule steers this device to Vulkan");
}

// The forced-bug override, which is how a test harness reaches a workaround road on a machine
// whose driver does not have the defect. It replaces a settings key, so what has to hold is that
// it is invisible until set, that it survives the rule loop (a bug nothing in the database grants
// on this device still arrives set), and that it does not pretend to be a database match.
TEST(GSGpuDriverProfile, ForcedBugsRideOnTopOfTheDatabaseAndAreNotCountedAsRules)
{
	// Qualcomm's own blob, not Turnip: the only rule granting BrokenBlendConstant keys on
	// MesaTurnip, so on this device the bit can only have come from the override.
	const auto resolve = [] {
		return ResolveAdrenoVK(
			"Adreno (TM) 650", kQualcommProprietaryDriverId, "Qualcomm", PackVulkanVersion(512, 615, 0));
	};

	const GpuProfileSelection clean = resolve();
	EXPECT_EQ(GpuProfileDetector::GetForcedBugs(), 0u);
	EXPECT_FALSE(clean.driver.HasBug(DriverBug::BrokenBlendConstant));

	GpuProfileDetector::SetForcedBugs(GpuProfileDetector::BugMask(DriverBug::BrokenBlendConstant));
	const GpuProfileSelection forced = resolve();
	EXPECT_TRUE(forced.driver.HasBug(DriverBug::BrokenBlendConstant));
	// Everything the database did say is untouched, and the forced bit is not a match.
	EXPECT_EQ(forced.driver.matched_rule_count, clean.driver.matched_rule_count);
	EXPECT_EQ(forced.driver.workarounds, clean.driver.workarounds);
	EXPECT_EQ(forced.driver.bugs, clean.driver.bugs | GpuProfileDetector::BugMask(DriverBug::BrokenBlendConstant));

	GpuProfileDetector::SetForcedBugs(0);
	EXPECT_FALSE(resolve().driver.HasBug(DriverBug::BrokenBlendConstant));
}
