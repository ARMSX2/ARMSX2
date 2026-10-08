// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// GPU identity parsing: the Adreno and Mali model tables (GSGPUProfileAdreno.cpp,
// GSGPUProfileMali.cpp) and the driver-version parse the driver-bug database matches on.
//
// The model parsers are called directly, on lowered hint strings, because Resolve also folds in the
// host's own platform identity (the device tree on Linux), and a test must not depend on the
// machine it runs on. Where a test goes through Resolve it forces the vendor with the override.

#include "GS/Renderers/Common/GSGPUProfile.h"
#include "GS/Renderers/Common/GSGPUProfilePrivate.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
using GpuProfileDetail::ResolvedGpuProfile;

ResolvedGpuProfile Adreno(std::string_view renderer)
{
	const std::string lowered = GpuProfileDetail::ToLowerASCII(renderer);
	EXPECT_TRUE(GpuProfileDetail::LooksLikeAdreno(lowered)) << renderer;
	return GpuProfileDetail::ResolveAdrenoProfile(lowered);
}

ResolvedGpuProfile Mali(std::string_view renderer)
{
	const std::string lowered = GpuProfileDetail::ToLowerASCII(renderer);
	EXPECT_TRUE(GpuProfileDetail::LooksLikeMali(lowered)) << renderer;
	return GpuProfileDetail::ResolveMaliProfile(lowered);
}

void ExpectTuning(const MobileGsTuning& t, u32 pool, u32 target_age, u32 texture_age, bool prefer_new)
{
	EXPECT_EQ(t.pooled_targets, pool);
	EXPECT_EQ(t.pooled_textures, pool);
	EXPECT_EQ(t.target_age, target_age);
	EXPECT_EQ(t.texture_age, texture_age);
	EXPECT_EQ(t.prefer_new_textures, prefer_new);
	EXPECT_EQ(t.constrained, pool < 128);
}

constexpr u32 PackVulkanVersion(u32 major, u32 minor, u32 patch)
{
	return (major << 22) | (minor << 12) | patch;
}

// VkDriverId values.
constexpr u32 kQualcommDriver = 8;
constexpr u32 kArmDriver = 9;
constexpr u32 kTurnipDriver = 18;
constexpr u32 kPanVKDriver = 20;

GpuProfileSelection ResolveVK(const char* override_mode, const char* device_name, u32 vendor_id, u32 driver_id,
	u32 driver_version, const char* driver_name = "", const char* driver_info = "")
{
	MobileDriverContext context;
	context.api = MobileGpuApi::Vulkan;
	context.vendor_id = vendor_id;
	context.driver_id = driver_id;
	context.driver_version = driver_version;
	context.driver_name = driver_name;
	context.driver_info = driver_info;
	return GpuProfileDetector::Resolve(override_mode, std::string_view(), device_name, context);
}

GpuProfileSelection ResolveGL(const char* override_mode, const char* renderer, const char* version)
{
	MobileDriverContext context;
	context.api = MobileGpuApi::OpenGL;
	context.driver_name = renderer;
	context.api_version_string = version;
	return GpuProfileDetector::Resolve(override_mode, std::string_view(), renderer, context);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Adreno

TEST(GSGpuProfileAdreno, EightHundredSeriesPartsAreAdreno8xx)
{
	for (const char* name : {"Adreno (TM) 830", "Turnip Adreno (TM) 830", "Adreno (TM) 840", "Adreno (TM) 825"})
	{
		const ResolvedGpuProfile p = Adreno(name);
		EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::Adreno8xx) << name;
		EXPECT_TRUE(p.gpu.recognized) << name;
	}
	const ResolvedGpuProfile a830 = Adreno("Adreno (TM) 830");
	EXPECT_EQ(a830.gpu.model_number, 830);
	EXPECT_EQ(a830.gpu.name, "Adreno 830");
	ExpectTuning(a830.tuning, 160, 12, 8, true);
}

TEST(GSGpuProfileAdreno, AnUnlistedEightHundredPartKeepsItsGeneration)
{
	// A part newer than the table: the generation still comes from the hundreds digit, with that
	// generation's fallback tuning, and it is not claimed as recognised.
	const ResolvedGpuProfile p = Adreno("Adreno (TM) 850");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::Adreno8xx);
	EXPECT_EQ(p.gpu.model_number, 850);
	EXPECT_FALSE(p.gpu.recognized);
	ExpectTuning(p.tuning, 144, 10, 8, true);
}

TEST(GSGpuProfileAdreno, SevenHundredSeriesPartsAreAdreno7xx)
{
	for (const char* name : {"Adreno (TM) 702", "Adreno (TM) 710", "Adreno (TM) 725", "Adreno (TM) 730",
			 "Turnip Adreno (TM) 740", "Adreno (TM) 750"})
	{
		const ResolvedGpuProfile p = Adreno(name);
		EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::Adreno7xx) << name;
		EXPECT_TRUE(p.gpu.recognized) << name;
	}
	EXPECT_EQ(Adreno("Adreno (TM) 740").gpu.model_number, 740);
	ExpectTuning(Adreno("Adreno (TM) 740").tuning, 160, 12, 8, true);
	// The low 7xx parts are constrained.
	ExpectTuning(Adreno("Adreno (TM) 702").tuning, 88, 7, 6, false);
}

TEST(GSGpuProfileAdreno, TheAdreno650IsAnUnconstrainedAdreno6xx)
{
	const ResolvedGpuProfile p = Adreno("Adreno (TM) 650");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::Adreno6xx);
	EXPECT_EQ(p.gpu.model_number, 650);
	EXPECT_TRUE(p.gpu.recognized);
	EXPECT_EQ(p.gpu.name, "Adreno 650");
	ExpectTuning(p.tuning, 144, 10, 8, true);
	// Turnip prefixes its own name; the model is the same.
	EXPECT_EQ(Adreno("Turnip Adreno (TM) 650").gpu.model_number, 650);
}

TEST(GSGpuProfileAdreno, TheAdreno610IsAConstrainedAdreno6xx)
{
	const ResolvedGpuProfile p = Adreno("Adreno (TM) 610");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::Adreno6xx);
	EXPECT_EQ(p.gpu.model_number, 610);
	EXPECT_TRUE(p.gpu.recognized);
	ExpectTuning(p.tuning, 80, 6, 5, false);
}

TEST(GSGpuProfileAdreno, TheLSuffixSelectsItsOwnEntry)
{
	const ResolvedGpuProfile l = Adreno("Adreno (TM) 619L");
	const ResolvedGpuProfile plain = Adreno("Adreno (TM) 619");
	EXPECT_EQ(l.gpu.name, "Adreno 619L");
	EXPECT_EQ(plain.gpu.name, "Adreno 619");
	EXPECT_EQ(l.gpu.model_number, plain.gpu.model_number);
	ExpectTuning(l.tuning, 88, 7, 6, false);
	ExpectTuning(plain.tuning, 96, 8, 6, false);
}

TEST(GSGpuProfileAdreno, OlderGenerationsResolveByTheHundredsDigit)
{
	EXPECT_EQ(Adreno("Adreno (TM) 530").gpu.architecture, MobileGpuArchitecture::Adreno5xx);
	EXPECT_EQ(Adreno("Adreno (TM) 430").gpu.architecture, MobileGpuArchitecture::Adreno4xx);
	EXPECT_EQ(Adreno("Adreno (TM) 330").gpu.architecture, MobileGpuArchitecture::Adreno3xx);
}

TEST(GSGpuProfileAdreno, SnapdragonXPartsAreAdrenoX)
{
	const ResolvedGpuProfile p = Adreno("Adreno X1-85");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::AdrenoX);
	EXPECT_EQ(p.gpu.model_number, 185);
	EXPECT_EQ(p.gpu.name, "Adreno X1-85");
}

TEST(GSGpuProfileAdreno, ANameWithoutAModelIsAnUnknownAdreno)
{
	const ResolvedGpuProfile p = Adreno("Adreno (TM)");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::Unknown);
	EXPECT_FALSE(p.gpu.recognized);
	EXPECT_EQ(p.gpu.name, "Unknown Adreno");
	ExpectTuning(p.tuning, 96, 8, 6, false);
}

// ---------------------------------------------------------------------------------------------
// Mali

TEST(GSGpuProfileMali, TheMaliG615IsValhall3WithItsCoreCount)
{
	const ResolvedGpuProfile p = Mali("Mali-G615 MC6");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::MaliValhall3);
	EXPECT_EQ(p.gpu.model_number, 615);
	EXPECT_EQ(p.gpu.core_count, 6);
	EXPECT_TRUE(p.gpu.recognized);
	EXPECT_EQ(p.gpu.name, "Mali-G615 MC6");
	ExpectTuning(p.tuning, 112, 8, 7, false);
}

TEST(GSGpuProfileMali, AFewCoresCapTheTuning)
{
	// Same part, two cores: the pool and ages are capped to the small-part limits.
	const ResolvedGpuProfile p = Mali("Mali-G615 MC2");
	EXPECT_EQ(p.gpu.core_count, 2);
	ExpectTuning(p.tuning, 64, 5, 5, false);
}

TEST(GSGpuProfileMali, TheMaliG57IsValhall1)
{
	const ResolvedGpuProfile p = Mali("Mali-G57 MC2");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::MaliValhall1);
	EXPECT_EQ(p.gpu.model_number, 57);
	EXPECT_EQ(p.gpu.core_count, 2);
	EXPECT_EQ(p.gpu.name, "Mali-G57 MC2");
	ExpectTuning(p.tuning, 64, 5, 5, false);
	// No core count in the name: the table's own tuning, uncapped.
	const ResolvedGpuProfile bare = Mali("Mali-G57");
	EXPECT_EQ(bare.gpu.core_count, 0);
	ExpectTuning(bare.tuning, 72, 6, 5, false);
}

TEST(GSGpuProfileMali, TheMaliG52IsBifrost)
{
	const ResolvedGpuProfile p = Mali("Mali-G52 MC2");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::MaliBifrost);
	EXPECT_EQ(p.gpu.model_number, 52);
	EXPECT_TRUE(p.gpu.recognized);
}

TEST(GSGpuProfileMali, ValhallGenerationsAreTakenFromTheTable)
{
	EXPECT_EQ(Mali("Mali-G610 MC4").gpu.architecture, MobileGpuArchitecture::MaliValhall2);
	EXPECT_EQ(Mali("Mali-G710 MC10").gpu.architecture, MobileGpuArchitecture::MaliValhall2);
	EXPECT_EQ(Mali("Mali-G720 MC7").gpu.architecture, MobileGpuArchitecture::MaliFifthGen);
	// Ten cores is above every cap, so the table's unconstrained tuning stands.
	ExpectTuning(Mali("Mali-G710 MC10").tuning, 136, 10, 8, true);
}

TEST(GSGpuProfileMali, ImmortalisIsNamedAsSuch)
{
	const ResolvedGpuProfile p = Mali("Immortalis-G715 MC11");
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::MaliValhall3);
	EXPECT_EQ(p.gpu.model_number, 715);
	EXPECT_EQ(p.gpu.name, "Immortalis-G715 MC11");
}

// ---------------------------------------------------------------------------------------------
// Valhall architecture number, as Mesa's panfrost model table counts it (pan_model.c keys each
// Mali on PAN_PROD_ID(arch, ...); the arch is the first field). Resolved here from GL_RENDERER
// strings, the way Auto's decision sees them.
//
//   v9   G57 (pan_model.c 9,0,1 and 9,0,3), G68 (9,2,4); G77 and G78 are not in that table but are
//        arch 9 in Mesa's bifrost compiler table and in Arm's kbase (GPU_ID2_MODEL_MAKE(9, n))
//   v10  G610 (10,8,7), G310 (10,12,4); G710 and G510 are kbase arch 10
//   v11  G715 (11,8,2), G615 (11,8,3)
//   12+  G720 is 12, G725 is 13, G1 is 14; G620 is kbase arch 12. None of them is v11.

u32 ValhallArchOf(std::string_view renderer)
{
	return GpuProfileDetector::MaliValhallArch(Mali(renderer).gpu.architecture);
}

// Every spelling a v9 part is known by: the stock GL_RENDERER with MC<n> or MP<n> (Arm's blobs use
// both for the same part), and the bare names our malisx2 driver reports as the Vulkan deviceName
// for product ids 0x9001..0x9005. The G77 and G57 core-count spellings are malisx2's naming for
// product 0x9000 and unknown arch-9 ids.
TEST(GSGpuProfileMali, ValhallV9IsG57G68G77G78)
{
	for (const char* renderer : {"Mali-G57", "Mali-G57 MC2", "Mali-G57 MC4", "Mali-G57 MC6", "Mali-G68",
			 "Mali-G68 MC4", "Mali-G77 MC7", "Mali-G77 MC9", "Mali-G78", "Mali-G78 MC14", "Mali-G78 MP14",
			 "Mali-G78AE", "Mali-G78AE MC10"})
	{
		EXPECT_EQ(ValhallArchOf(renderer), 9u) << renderer;
	}
}

TEST(GSGpuProfileMali, ValhallV10IsG310G510G610G710)
{
	EXPECT_EQ(ValhallArchOf("Mali-G310 MC2"), 10u);
	EXPECT_EQ(ValhallArchOf("Mali-G510 MC4"), 10u);
	EXPECT_EQ(ValhallArchOf("Mali-G610 MC6"), 10u);
	EXPECT_EQ(ValhallArchOf("Mali-G710 MC10"), 10u);
}

TEST(GSGpuProfileMali, ValhallV11IsG615G715AndImmortalisG715)
{
	// The bare names are what malisx2 reports for v11; the MC<n> forms are Arm's stock strings.
	for (const char* renderer : {"Mali-G615", "Mali-G615 MC6", "Mali-G615 MC2", "Mali-G715", "Mali-G715 MC7"})
		EXPECT_EQ(ValhallArchOf(renderer), 11u) << renderer;
	// The two spellings the Arm driver and a name rebuilt from the model number produce.
	EXPECT_EQ(ValhallArchOf("Mali-G715-Immortalis MC11"), 11u);
	EXPECT_EQ(ValhallArchOf("Immortalis-G715 MC11"), 11u);
}

// Names that read like v11 and are not: G720 is arch 12 and G725 arch 13 in Mesa's table, G620 is
// arch 12 in kbase, and G625 and G925 sit with G725.
TEST(GSGpuProfileMali, TheFifthGenPartsAreNotValhall)
{
	for (const char* renderer : {"Mali-G620 MC4", "Mali-G720 MC7", "Mali-G720-Immortalis MC10",
			 "Mali-G625 MC6", "Mali-G725 MC6", "Mali-G925-Immortalis MC12", "Immortalis-G925 MC12",
			 "Mali-G1-Ultra MC12"})
	{
		EXPECT_EQ(ValhallArchOf(renderer), 0u) << renderer;
	}
}

TEST(GSGpuProfileMali, BifrostAndOlderAreNotValhall)
{
	for (const char* renderer : {"Mali-G31 MP2", "Mali-G51 MP4", "Mali-G52 MC2", "Mali-G71 MP20",
			 "Mali-G72 MP12", "Mali-G76 MC12", "Mali-T880 MP12", "Mali-450 MP"})
	{
		EXPECT_EQ(ValhallArchOf(renderer), 0u) << renderer;
	}
}

// The model number is read whole. A reader that stopped at two digits would put G71 and G715 (or
// G31 and G310, G72 and G720, G51 and G510) in the same place; one that matched G61x loosely would
// treat G610 and G615 alike.
TEST(GSGpuProfileMali, TheWholeModelNumberDecidesTheArchitecture)
{
	EXPECT_EQ(ValhallArchOf("Mali-G71 MP8"), 0u);
	EXPECT_EQ(ValhallArchOf("Mali-G715 MC7"), 11u);
	EXPECT_EQ(ValhallArchOf("Mali-G710 MC10"), 10u);
	EXPECT_EQ(ValhallArchOf("Mali-G31 MP2"), 0u);
	EXPECT_EQ(ValhallArchOf("Mali-G310 MC2"), 10u);
	EXPECT_EQ(ValhallArchOf("Mali-G51 MP4"), 0u);
	EXPECT_EQ(ValhallArchOf("Mali-G510 MC4"), 10u);
	EXPECT_EQ(ValhallArchOf("Mali-G72 MP12"), 0u);
	EXPECT_EQ(ValhallArchOf("Mali-G720 MC7"), 0u);
	EXPECT_EQ(ValhallArchOf("Mali-G610 MC6"), 10u);
	EXPECT_EQ(ValhallArchOf("Mali-G615 MC6"), 11u);
	EXPECT_EQ(ValhallArchOf("Mali-G57 MC2"), 9u);
	EXPECT_EQ(ValhallArchOf("Mali-G77 MC9"), 9u);
}

// Only the Valhall architectures carry a number; every other value, including a part nothing
// resolved, reads as not Valhall.
TEST(GSGpuProfileMali, OnlyTheValhallArchitecturesHaveAValhallNumber)
{
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::Unknown), 0u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::MaliMidgard), 0u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::MaliBifrost), 0u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::MaliValhall1), 9u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::MaliValhall2), 10u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::MaliValhall3), 11u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::MaliFifthGen), 0u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::MaliG1), 0u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::Adreno6xx), 0u);
	EXPECT_EQ(GpuProfileDetector::MaliValhallArch(MobileGpuArchitecture::PowerVR), 0u);
}

TEST(GSGpuProfileMali, MidgardAndUtgardParse)
{
	const ResolvedGpuProfile t880 = Mali("Mali-T880 MP12");
	EXPECT_EQ(t880.gpu.architecture, MobileGpuArchitecture::MaliMidgard);
	EXPECT_EQ(t880.gpu.core_count, 12);
	EXPECT_EQ(t880.gpu.name, "Mali-T880 MC12");
	const ResolvedGpuProfile m450 = Mali("Mali-450 MP");
	EXPECT_EQ(m450.gpu.architecture, MobileGpuArchitecture::MaliUtgard);
	EXPECT_EQ(m450.gpu.name, "Mali-450");
}

TEST(GSGpuProfileMali, AnUnlistedModelFallsBackByNumber)
{
	const ResolvedGpuProfile p = Mali("Mali-G99");
	EXPECT_FALSE(p.gpu.recognized);
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::MaliBifrost);
}

TEST(GSGpuProfileMali, AVendorHintBeforeTheRendererDoesNotSupplyTheSeries)
{
	// "gpu_vendor=ARM Mali | gpu=Mali-G615 MC6": the 'g' of "gpu=" must not be read as the series.
	const ResolvedGpuProfile p = Mali("gpu_vendor=ARM Mali | gpu=Mali-G615 MC6");
	EXPECT_EQ(p.gpu.model_number, 615);
	EXPECT_EQ(p.gpu.architecture, MobileGpuArchitecture::MaliValhall3);
}

// ---------------------------------------------------------------------------------------------
// Driver identity and version

TEST(GSGpuProfileDriver, VulkanMaliReportsItsRevisionAsMajorAndMinor)
{
	const GpuProfileSelection sel =
		ResolveVK("mali", "Mali-G615 MC6", GpuVendorID::ARM, kArmDriver, PackVulkanVersion(44, 1, 0));
	EXPECT_EQ(sel.runtime_profile, RuntimeGpuProfile::Mali);
	EXPECT_EQ(sel.driver.driver, MobileGpuDriver::ArmProprietary);
	EXPECT_TRUE(sel.driver.version.known);
	EXPECT_EQ(sel.driver.version.major, 44);
	EXPECT_EQ(sel.driver.version.minor, 1);
	EXPECT_EQ(sel.driver.confidence, DriverProfileConfidence::DriverVersion);
}

TEST(GSGpuProfileDriver, AnOldMaliSourceHashIsNotAVersion)
{
	// Old Arm drivers put a hash in driverVersion; a nonzero patch field gives it away.
	const GpuProfileSelection sel = ResolveVK("mali", "Mali-G52 MC2", GpuVendorID::ARM, kArmDriver, 0xaa9c4b29u);
	EXPECT_FALSE(sel.driver.version.known);
	EXPECT_TRUE(sel.driver.version.legacy_hash);
	EXPECT_EQ(sel.driver.version.raw, 0xaa9c4b29u);
}

TEST(GSGpuProfileDriver, QualcommVersionsAreOrderableOnlyWithTheTopBit)
{
	const GpuProfileSelection current =
		ResolveVK("adreno", "Adreno (TM) 830", GpuVendorID::Qualcomm, kQualcommDriver, 0x80000000u | (744u << 12));
	EXPECT_EQ(current.driver.driver, MobileGpuDriver::QualcommProprietary);
	EXPECT_TRUE(current.driver.version.known);
	EXPECT_EQ(current.driver.version.major, 512);
	EXPECT_EQ(current.driver.version.minor, 744);
	EXPECT_EQ(current.gpu.architecture, MobileGpuArchitecture::Adreno8xx);

	const GpuProfileSelection old =
		ResolveVK("adreno", "Adreno (TM) 530", GpuVendorID::Qualcomm, kQualcommDriver, PackVulkanVersion(1, 2, 3));
	EXPECT_FALSE(old.driver.version.known);
}

TEST(GSGpuProfileDriver, TurnipReportsMesasVersion)
{
	const GpuProfileSelection sel = ResolveVK("adreno", "Turnip Adreno (TM) 650", GpuVendorID::Qualcomm,
		kTurnipDriver, 0x06801002u, "turnip Mesa driver", "Mesa 26.1.2 (git-axfl1-005)");
	EXPECT_EQ(sel.driver.driver, MobileGpuDriver::MesaTurnip);
	EXPECT_EQ(sel.driver.version.major, 26);
	EXPECT_EQ(sel.driver.version.minor, 1);
	EXPECT_EQ(sel.driver.version.patch, 2);
	EXPECT_EQ(sel.driver.declared_loop_fix_generation, 1u);
	EXPECT_EQ(sel.gpu.model_number, 650);
}

TEST(GSGpuProfileDriver, WithoutADriverIdTheDriverComesFromItsName)
{
	EXPECT_EQ(ResolveVK("adreno", "Adreno (TM) 650", GpuVendorID::Qualcomm, 0, 0, "turnip Mesa driver").driver.driver,
		MobileGpuDriver::MesaTurnip);
	EXPECT_EQ(ResolveVK("mali", "Mali-G610", GpuVendorID::ARM, 0, 0, "panvk").driver.driver,
		MobileGpuDriver::MesaPanVK);
	// No name to go on: the vendor's own driver.
	EXPECT_EQ(ResolveVK("adreno", "Adreno (TM) 650", GpuVendorID::Qualcomm, 0, 0).driver.driver,
		MobileGpuDriver::QualcommProprietary);
	EXPECT_EQ(ResolveVK("mali", "Mali-G52 MC2", GpuVendorID::ARM, 0, 0).driver.driver,
		MobileGpuDriver::ArmProprietary);
	EXPECT_EQ(ResolveVK("mali", "Mali-G610", GpuVendorID::ARM, kPanVKDriver, 0).driver.driver,
		MobileGpuDriver::MesaPanVK);
}

TEST(GSGpuProfileDriver, OpenGLVersionsComeFromEachVendorsBuildTag)
{
	const GpuProfileSelection adreno = ResolveGL("adreno", "Adreno (TM) 650", "OpenGL ES 3.2 V@0502.0 (GIT@abc)");
	EXPECT_TRUE(adreno.driver.version.known);
	EXPECT_EQ(adreno.driver.version.major, 502);
	EXPECT_EQ(adreno.driver.version.minor, 0);

	const GpuProfileSelection mali =
		ResolveGL("mali", "Mali-G57 MC2", "OpenGL ES 3.2 v1.r32p1-01bet0.cc7ae0ca8d5dbe1b8fa6a9c3b5c2c6c9");
	EXPECT_EQ(mali.driver.version.major, 32);
	EXPECT_EQ(mali.driver.version.minor, 1);

	const GpuProfileSelection powervr = ResolveGL("powervr", "PowerVR Rogue GE8320", "OpenGL ES 3.2 build 1.9@4850625");
	EXPECT_EQ(powervr.driver.version.major, 1);
	EXPECT_EQ(powervr.driver.version.minor, 9);
	EXPECT_EQ(powervr.driver.version.build, 4850625u);
}
