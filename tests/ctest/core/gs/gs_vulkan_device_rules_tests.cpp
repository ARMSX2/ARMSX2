// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The Vulkan device rules keyed on a device's own identity (GpuProfileDetector::
// ResolveVulkanDeviceRules). Each rule is pinned from both sides: the parts it names, and the
// neighbours it must leave alone.

#include "GS/Renderers/Common/GSGPUProfile.h"

#include <gtest/gtest.h>

namespace
{
// VkDriverId values.
constexpr u32 kQualcommDriver = 8;
constexpr u32 kArmDriver = 9;
constexpr u32 kTurnipDriver = 18;
constexpr u32 kPanVKDriver = 20;
constexpr u32 kHoneykrispDriver = 26;
constexpr u32 kRadvDriver = 3;
constexpr u32 kImaginationDriver = 7;

constexpr u32 PackVulkanVersion(u32 major, u32 minor, u32 patch)
{
	return (major << 22) | (minor << 12) | patch;
}

// Qualcomm's driverVersion carries the top bit, so 512.744.0 packs to 0x802E8000.
constexpr u32 QualcommVersion(u32 minor)
{
	return 0x80000000u | (minor << 12);
}

struct Device
{
	u32 vendor = 0;
	u32 driver_id = 0;
	const char* name = "";
	const char* driver_info = "";
	u32 device_id = 0;
	u32 driver_version = 0;
	// VkPhysicalDevicePushDescriptorPropertiesKHR::maxPushDescriptors; 0 is "extension not enabled".
	u32 max_push_descriptors = 0;
};

VulkanDeviceRules Rules(const Device& d)
{
	MobileDriverContext context;
	context.api = MobileGpuApi::Vulkan;
	context.vendor_id = d.vendor;
	context.driver_id = d.driver_id;
	context.device_id = d.device_id;
	context.driver_version = d.driver_version;
	context.driver_info = d.driver_info;
	context.max_push_descriptors = d.max_push_descriptors;
	const GpuProfileSelection selection = GpuProfileDetector::Resolve("auto", std::string_view(), d.name, context);
	return GpuProfileDetector::ResolveVulkanDeviceRules(selection, context, d.name);
}

const Device kRg477v = {GpuVendorID::ARM, kArmDriver, "Mali-G615 MC6",
	"v1.r44p1-01eac0.030c4a3fb15fe65f485fb565f5e1b688", 0, PackVulkanVersion(44, 1, 0)};
const Device kMaliG615R47 = {GpuVendorID::ARM, kArmDriver, "Mali-G615 MC2", "v1.r47p0-00eac0", 0,
	PackVulkanVersion(47, 0, 0)};
const Device kMaliG57 = {GpuVendorID::ARM, kArmDriver, "Mali-G57 MC2", "v1.r32p1-01bet0", 0,
	PackVulkanVersion(32, 1, 0)};
const Device kMaliG52 = {GpuVendorID::ARM, kArmDriver, "Mali-G52 MC2", "v1.r26p0-01eac0", 0,
	PackVulkanVersion(26, 0, 0)};
// malisx2 reports Arm's vendorID, driverID, device name and 44.1.0 on purpose; only driverInfo says
// otherwise. The G615 build and the G57 build, as they report themselves.
constexpr const char* kMaliSX2DriverInfo = "v1.r44p1-malisx2.0.2.s0123abcd";
const Device kMaliSX2G615 = {GpuVendorID::ARM, kArmDriver, "Mali-G615 MC6", kMaliSX2DriverInfo, 0,
	PackVulkanVersion(44, 1, 0)};
const Device kMaliSX2G57 = {GpuVendorID::ARM, kArmDriver, "Mali-G57 MC2", kMaliSX2DriverInfo, 0,
	PackVulkanVersion(44, 1, 0)};
const Device kMaliG610PanVK = {GpuVendorID::ARM, kPanVKDriver, "Mali-G610 (Panfrost)", "Mesa 25.2.0", 0,
	PackVulkanVersion(25, 2, 0)};
const Device kAdreno650Turnip = {GpuVendorID::Qualcomm, kTurnipDriver, "Turnip Adreno (TM) 650", "Mesa 26.1.2",
	0x06050001u, PackVulkanVersion(26, 1, 2)};
const Device kAdreno740Turnip = {GpuVendorID::Qualcomm, kTurnipDriver, "Turnip Adreno (TM) 740", "Mesa 26.1.2",
	0x43050a01u, PackVulkanVersion(26, 1, 2)};
const Device kAdreno830Turnip = {GpuVendorID::Qualcomm, kTurnipDriver, "Turnip Adreno (TM) 830", "Mesa 26.1.2",
	0x44050a31u, PackVulkanVersion(26, 1, 2)};
const Device kAdreno830Qualcomm = {GpuVendorID::Qualcomm, kQualcommDriver, "Adreno (TM) 830", "", 0x44050a31u,
	QualcommVersion(800)};
const Device kAdreno740Qualcomm = {GpuVendorID::Qualcomm, kQualcommDriver, "Adreno (TM) 740", "", 0x43050a01u,
	QualcommVersion(744)};
const Device kAdreno650QualcommOld = {GpuVendorID::Qualcomm, kQualcommDriver, "Adreno (TM) 650", "", 0x06050001u,
	QualcommVersion(415)};
const Device kAdreno530Qualcomm = {GpuVendorID::Qualcomm, kQualcommDriver, "Adreno (TM) 530", "", 0x05030004u,
	QualcommVersion(744)};
const Device kAdrenoUnknownDriver = {GpuVendorID::Qualcomm, 0, "Adreno (TM) 650", "", 0x06050001u,
	QualcommVersion(744)};
const Device kPowerVR = {GpuVendorID::Imagination, kImaginationDriver, "PowerVR B-Series BXM-8-256", "", 0,
	0x00582558u};
const Device kAppleM2 = {0x10005u, kHoneykrispDriver, "Apple M2 Max (G14C B1)", "Mesa 25.3.0", 0,
	PackVulkanVersion(25, 3, 0)};
const Device kRadv = {GpuVendorID::AMD, kRadvDriver, "AMD Radeon RX 7900 XTX (RADV NAVI31)", "Mesa 25.0", 0,
	PackVulkanVersion(25, 0, 0)};
} // namespace

TEST(GSVulkanDeviceRules, OnlyTheMaliG615HasBrokenTimestampQueries)
{
	EXPECT_TRUE(Rules(kRg477v).broken_timestamp_queries);
	// A name match, so every G615 on every driver revision.
	EXPECT_TRUE(Rules(kMaliG615R47).broken_timestamp_queries);
	EXPECT_FALSE(Rules(kMaliG57).broken_timestamp_queries);
	EXPECT_FALSE(Rules(kMaliG610PanVK).broken_timestamp_queries);
	EXPECT_FALSE(Rules(kAdreno650Turnip).broken_timestamp_queries);
	// The name alone is not enough: the vendor must be Arm.
	Device renamed = kAdreno650Turnip;
	renamed.name = "Mali-G615 MC6";
	EXPECT_FALSE(Rules(renamed).broken_timestamp_queries);
}

TEST(GSVulkanDeviceRules, OnlyAnR44p1DriverAvoidsTheFeedbackLoopLayout)
{
	EXPECT_TRUE(Rules(kRg477v).avoid_feedback_loop_layout);
	EXPECT_FALSE(Rules(kMaliG615R47).avoid_feedback_loop_layout);
	EXPECT_FALSE(Rules(kMaliG57).avoid_feedback_loop_layout);
	// No driverInfo (VK_KHR_driver_properties absent): nothing to match.
	Device no_info = kRg477v;
	no_info.driver_info = "";
	EXPECT_FALSE(Rules(no_info).avoid_feedback_loop_layout);
	Device adreno_with_tag = kAdreno650Turnip;
	adreno_with_tag.driver_info = "r44p1";
	EXPECT_FALSE(Rules(adreno_with_tag).avoid_feedback_loop_layout);
}

TEST(GSVulkanDeviceRules, PushDescriptorsAreAvoidedOnMaliAndOnUnknownAdrenoDrivers)
{
	// Mali on any driver, Arm's or PanVK.
	EXPECT_TRUE(Rules(kRg477v).avoid_push_descriptors);
	EXPECT_TRUE(Rules(kMaliG52).avoid_push_descriptors);
	EXPECT_TRUE(Rules(kMaliG610PanVK).avoid_push_descriptors);
	EXPECT_FALSE(Rules(kAdreno650Turnip).avoid_push_descriptors);
	EXPECT_FALSE(Rules(kAdreno830Qualcomm).avoid_push_descriptors);
	EXPECT_TRUE(Rules(kAdrenoUnknownDriver).avoid_push_descriptors);
	EXPECT_FALSE(Rules(kPowerVR).avoid_push_descriptors);
	EXPECT_FALSE(Rules(kAppleM2).avoid_push_descriptors);
	EXPECT_FALSE(Rules(kRadv).avoid_push_descriptors);
}

// maxPushDescriptors the backend needs to bind its texture set by push (GSDeviceVK::NUM_TFX_TEXTURES).
constexpr u32 kEnoughPushDescriptors = VULKAN_PUSH_DESCRIPTORS_REQUIRED;

// malisx2 is not the blob whose vkCmdPushDescriptorSetKHR crashes, so it is exempt from the Mali
// avoid, but only where it advertises the extension with as many descriptors as the backend binds.
// Without that the backend's own maxPushDescriptors check would turn them off anyway; the rule just
// has to agree with it.
TEST(GSVulkanDeviceRules, MaliSX2IsExemptFromThePushDescriptorAvoidWithEnoughDescriptors)
{
	for (Device d : {kMaliSX2G615, kMaliSX2G57})
	{
		d.max_push_descriptors = kEnoughPushDescriptors;
		const VulkanDeviceRules rules = Rules(d);
		EXPECT_FALSE(rules.avoid_push_descriptors) << d.name;
		EXPECT_TRUE(rules.exempt_malisx2_push_descriptors) << d.name;

		d.max_push_descriptors = 32;
		EXPECT_FALSE(Rules(d).avoid_push_descriptors) << d.name;
	}
}

TEST(GSVulkanDeviceRules, MaliSX2KeepsThePushDescriptorAvoidWithoutTheExtensionOrWithTooFewDescriptors)
{
	for (Device d : {kMaliSX2G615, kMaliSX2G57})
	{
		// VK_KHR_push_descriptor not enabled: a pack without it behaves as it always did.
		d.max_push_descriptors = 0;
		VulkanDeviceRules rules = Rules(d);
		EXPECT_TRUE(rules.avoid_push_descriptors) << d.name;
		EXPECT_FALSE(rules.exempt_malisx2_push_descriptors) << d.name;

		// One short of the texture set.
		d.max_push_descriptors = kEnoughPushDescriptors - 1;
		rules = Rules(d);
		EXPECT_TRUE(rules.avoid_push_descriptors) << d.name;
		EXPECT_FALSE(rules.exempt_malisx2_push_descriptors) << d.name;
	}
}

// Everything else on Mali stays avoided however many descriptors it reports: Arm's own blob, with
// the same device name and revision, and PanVK.
TEST(GSVulkanDeviceRules, OtherMaliDriversKeepThePushDescriptorAvoid)
{
	for (Device d : {kRg477v, kMaliG52, kMaliG610PanVK})
	{
		d.max_push_descriptors = 32;
		const VulkanDeviceRules rules = Rules(d);
		EXPECT_TRUE(rules.avoid_push_descriptors) << d.name;
		EXPECT_FALSE(rules.exempt_malisx2_push_descriptors) << d.name;
	}
}

// The exemption is about Mali. A device that is not Arm's cannot pick it up from its driverInfo.
TEST(GSVulkanDeviceRules, TheMaliSX2PushDescriptorExemptionNeedsAnArmDevice)
{
	Device renamed = kAdreno650Turnip;
	renamed.driver_info = kMaliSX2DriverInfo;
	renamed.max_push_descriptors = 32;
	EXPECT_FALSE(Rules(renamed).exempt_malisx2_push_descriptors);

	Device unknown_adreno = kAdrenoUnknownDriver;
	unknown_adreno.driver_info = kMaliSX2DriverInfo;
	unknown_adreno.max_push_descriptors = 32;
	EXPECT_TRUE(Rules(unknown_adreno).avoid_push_descriptors);
}

TEST(GSVulkanDeviceRules, OnlyTheQualcommDriverBreaksTheProvokingVertex)
{
	EXPECT_TRUE(Rules(kAdreno740Qualcomm).broken_provoking_vertex);
	EXPECT_TRUE(Rules(kAdreno830Qualcomm).broken_provoking_vertex);
	EXPECT_FALSE(Rules(kAdreno740Turnip).broken_provoking_vertex);
	EXPECT_FALSE(Rules(kAdrenoUnknownDriver).broken_provoking_vertex);
	EXPECT_FALSE(Rules(kRg477v).broken_provoking_vertex);
}

TEST(GSVulkanDeviceRules, TheColorMaskBugNeedsAnAdreno5xxOrAnOldQualcommDriver)
{
	EXPECT_TRUE(Rules(kAdreno530Qualcomm).broken_colormask_with_depth);
	EXPECT_TRUE(Rules(kAdreno650QualcommOld).broken_colormask_with_depth);
	EXPECT_FALSE(Rules(kAdreno740Qualcomm).broken_colormask_with_depth);
	// The threshold is inclusive of the fixed build.
	Device at_threshold = kAdreno650QualcommOld;
	at_threshold.driver_version = 0x801EA000u;
	EXPECT_FALSE(Rules(at_threshold).broken_colormask_with_depth);
	at_threshold.driver_version = 0x801E9FFFu;
	EXPECT_TRUE(Rules(at_threshold).broken_colormask_with_depth);
	// Mesa's version is always below the Qualcomm threshold, so Turnip is excluded by driver.
	EXPECT_FALSE(Rules(kAdreno650Turnip).broken_colormask_with_depth);
	Device turnip_a5xx = kAdreno650Turnip;
	turnip_a5xx.device_id = 0x05030004u;
	EXPECT_FALSE(Rules(turnip_a5xx).broken_colormask_with_depth);
	// An Adreno with no known driver takes the rule on its version like the Qualcomm one.
	EXPECT_FALSE(Rules(kAdrenoUnknownDriver).broken_colormask_with_depth);
	EXPECT_FALSE(Rules(kRg477v).broken_colormask_with_depth);
}

TEST(GSVulkanDeviceRules, OnlyTheMaliG57FallsBackFromMotionAdaptiveDeinterlace)
{
	EXPECT_TRUE(Rules(kMaliG57).broken_mad_deinterlace);
	EXPECT_FALSE(Rules(kMaliG52).broken_mad_deinterlace);
	EXPECT_FALSE(Rules(kRg477v).broken_mad_deinterlace);
	Device renamed = kAdreno650Turnip;
	renamed.name = "Mali-G57 MC2";
	EXPECT_FALSE(Rules(renamed).broken_mad_deinterlace);
}

TEST(GSVulkanDeviceRules, OnlyAdreno8xxOnTheQualcommDriverIsFlagged)
{
	EXPECT_TRUE(Rules(kAdreno830Qualcomm).adreno8xx_proprietary);
	EXPECT_FALSE(Rules(kAdreno830Turnip).adreno8xx_proprietary);
	EXPECT_FALSE(Rules(kAdreno740Qualcomm).adreno8xx_proprietary);
	EXPECT_FALSE(Rules(kAdreno650Turnip).adreno8xx_proprietary);
}

TEST(GSVulkanDeviceRules, SelfReadCostsWereMeasuredOnTurnipAndHoneykrispOnly)
{
	EXPECT_TRUE(Rules(kAdreno650Turnip).self_read_costs_measured);
	EXPECT_TRUE(Rules(kAdreno830Turnip).self_read_costs_measured);
	EXPECT_TRUE(Rules(kAppleM2).self_read_costs_measured);
	EXPECT_FALSE(Rules(kAdreno740Qualcomm).self_read_costs_measured);
	EXPECT_FALSE(Rules(kRg477v).self_read_costs_measured);
	EXPECT_FALSE(Rules(kMaliG610PanVK).self_read_costs_measured);
	EXPECT_FALSE(Rules(kRadv).self_read_costs_measured);

	EXPECT_TRUE(Rules(kAppleM2).barrier_road_measured);
	EXPECT_FALSE(Rules(kAdreno650Turnip).barrier_road_measured);
	EXPECT_FALSE(Rules(kRadv).barrier_road_measured);
}

// The rules that are true, by name, as the device-creation log prints them. Declaration order, not
// the order they were set in.
TEST(GSVulkanDeviceRules, DescribeNamesTheRulesThatAreTrue)
{
	EXPECT_EQ(GpuProfileDetector::DescribeDeviceRules(VulkanDeviceRules{}), "none");
	EXPECT_EQ(GpuProfileDetector::DescribeDeviceRules(Rules(kRg477v)),
		"broken_timestamp_queries, avoid_feedback_loop_layout, avoid_push_descriptors");
	EXPECT_EQ(GpuProfileDetector::DescribeDeviceRules(Rules(kAppleM2)),
		"self_read_costs_measured, barrier_road_measured");
	EXPECT_EQ(GpuProfileDetector::DescribeDeviceRules(Rules(kRadv)), "none");

	// The exemption is named, so a log shows it where the avoid is missing.
	Device malisx2 = kMaliSX2G615;
	malisx2.max_push_descriptors = kEnoughPushDescriptors;
	EXPECT_EQ(GpuProfileDetector::DescribeDeviceRules(Rules(malisx2)),
		"broken_timestamp_queries, avoid_feedback_loop_layout, exempt_malisx2_push_descriptors");
	EXPECT_EQ(GpuProfileDetector::DescribeDeviceRules(Rules(kMaliSX2G615)),
		"broken_timestamp_queries, avoid_feedback_loop_layout, avoid_push_descriptors");
}

// Each flag has its own entry in the name table. A flag added to VulkanDeviceRules without one is
// caught by the static_assert beside the table; this catches two entries naming the same flag.
TEST(GSVulkanDeviceRules, EveryRuleHasItsOwnNameInTheTable)
{
	for (const VulkanDeviceRuleName& entry : VULKAN_DEVICE_RULE_NAMES)
	{
		VulkanDeviceRules rules;
		rules.*entry.flag = true;
		EXPECT_EQ(GpuProfileDetector::DescribeDeviceRules(rules), entry.name);
	}
}
