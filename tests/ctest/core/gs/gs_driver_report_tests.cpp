// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The driver report's pure parts: which driver answered, what was expected of the selected pack,
// the axfl build tag, driverVersion decoding, and the JSON writer and reader.
//
// The served-driver verdict is the report's most important line, and its trap is malisx2: it
// reports Arm's driverID and a stock device name on purpose, so only its driverInfo tells it apart
// from Arm's own driver. Packs released before the rename (when it was called libmali) say
// "libmali" in driverInfo, so both spellings are tested. The strings below are what the devices
// report.

#include "GS/DriverReport/GSDriverReportClassify.h"
#include "GS/DriverReport/GSDriverReportJson.h"
#include "GS/DriverReport/GSDriverReportProfile.h"

#include <gtest/gtest.h>

using namespace GSDriverReport;

namespace
{
	ServedDriverFacts Facts(uint32_t vendor, uint32_t driver_id, const char* name, const char* info, const char* device)
	{
		ServedDriverFacts f;
		f.vendor_id = vendor;
		f.driver_id = driver_id;
		f.driver_name = name;
		f.driver_info = info;
		f.device_name = device;
		return f;
	}
} // namespace

TEST(GSDriverReport, TurnipWithAxflTagIsTurnipAndReportsItsGeneration)
{
	const ServedDriverClassification c = ClassifyServedDriver(
		Facts(0x5143, DriverIdValue::MesaTurnip, "turnip Mesa driver", "Mesa 26.1.2 (git-axfl2-001)", "Turnip Adreno (TM) 740"));
	EXPECT_EQ(c.answered, "turnip");
	EXPECT_EQ(c.axfl_generation, 2u);
	EXPECT_NE(c.evidence.find("git-axfl2-001"), std::string::npos);
}

TEST(GSDriverReport, StockTurnipHasNoAxflGeneration)
{
	const ServedDriverClassification c = ClassifyServedDriver(
		Facts(0x5143, DriverIdValue::MesaTurnip, "turnip Mesa driver", "Mesa 26.1.2", "Turnip Adreno (TM) 650"));
	EXPECT_EQ(c.answered, "turnip");
	EXPECT_EQ(c.axfl_generation, 0u);
}

TEST(GSDriverReport, QualcommBlobIsTheVendorDriver)
{
	const ServedDriverClassification c =
		ClassifyServedDriver(Facts(0x5143, DriverIdValue::QualcommProprietary, "Qualcomm Technologies Inc. Adreno Vulkan Driver",
			"Driver Build: 69e13475cb, I55b9d5d7f8, 1703680405\nDate: 12/27/23", "Adreno (TM) 740"));
	EXPECT_EQ(c.answered, "vendor-qualcomm");
}

TEST(GSDriverReport, MaliSX2IsToldApartByDriverInfoNotDriverId)
{
	// malisx2 presents Arm's driverID and a stock Mali name; only driverInfo differs.
	const ServedDriverClassification sx2 = ClassifyServedDriver(Facts(0x13B5, DriverIdValue::ArmProprietary,
		"Mali-G615", "v1.r44p1-malisx2.0.2.s0123abcd", "Mali-G615"));
	EXPECT_EQ(sx2.answered, "malisx2");

	const ServedDriverClassification arm = ClassifyServedDriver(Facts(0x13B5, DriverIdValue::ArmProprietary,
		"Mali-G615", "v1.r44p1-01eac0.030c4a3fb15fe65f485fb565f5e1b688", "Mali-G615 MC6"));
	EXPECT_EQ(arm.answered, "vendor-arm");
}

TEST(GSDriverReport, PreRenameLibmaliPackStillClassifiesAsMaliSX2)
{
	// Packs released before the rename say "libmali" in driverInfo.
	const ServedDriverClassification lib = ClassifyServedDriver(Facts(0x13B5, DriverIdValue::ArmProprietary,
		"Mali-G615", "v1.r44p1-libmali.0.1.s0123abcd", "Mali-G615"));
	EXPECT_EQ(lib.answered, "malisx2");
}

TEST(GSDriverReport, ArmStockBlobIsNotMaliSX2)
{
	// A stock Arm blob on the same vendorID, driverID and device name as ours.
	const ServedDriverClassification arm = ClassifyServedDriver(Facts(0x13B5, DriverIdValue::ArmProprietary,
		"Mali-G615", "v1.r40p0-01eac0.abcdef", "Mali-G615"));
	EXPECT_EQ(arm.answered, "vendor-arm");
}

TEST(GSDriverReport, IsMaliSX2DriverMatchesEitherSpellingInDriverInfo)
{
	EXPECT_TRUE(IsMaliSX2Driver("v1.r44p1-malisx2.0.2.s0123abcd"));
	EXPECT_TRUE(IsMaliSX2Driver("v1.r44p1-libmali.0.1.s0123abcd"));
	EXPECT_TRUE(IsMaliSX2Driver("malisx2"));
	EXPECT_TRUE(IsMaliSX2Driver("libmali"));

	EXPECT_FALSE(IsMaliSX2Driver("v1.r40p0-01eac0.abcdef"));
	EXPECT_FALSE(IsMaliSX2Driver("v1.r44p1-01eac0.030c4a3fb15fe65f485fb565f5e1b688"));
	EXPECT_FALSE(IsMaliSX2Driver("Mesa 26.1.2 (git-axfl2-001)"));
	EXPECT_FALSE(IsMaliSX2Driver("mali"));
	EXPECT_FALSE(IsMaliSX2Driver(""));
}

TEST(GSDriverReport, PanVKAndUnknownDrivers)
{
	EXPECT_EQ(ClassifyServedDriver(Facts(0x13B5, DriverIdValue::MesaPanVK, "panvk", "Mesa 25.0", "Mali-G52")).answered, "panvk");
	EXPECT_EQ(ClassifyServedDriver(Facts(0x10DE, 4, "NVIDIA", "560.35", "RTX")).answered, "other");
	EXPECT_EQ(ClassifyServedDriver(Facts(0x10005, 26, "Honeykrisp", "Mesa 25.3.6", "Apple M2 Max (G14C B1)")).answered,
		"mesa-honeykrisp");
	EXPECT_EQ(ClassifyServedDriver(Facts(0, 0, "", "", "")).answered, "unknown");
}

TEST(GSDriverReport, ExpectedDriverFromThePack)
{
	EXPECT_EQ(ExpectedDriverForPack(false, "", "", ""), "system");
	EXPECT_EQ(ExpectedDriverForPack(true, "ARMSX2 Turnip axfl2-001", "libvulkan_freedreno.so", ""), "turnip");
	EXPECT_EQ(ExpectedDriverForPack(true, "some-pack", "libvulkan_freedreno.so", ""), "turnip");
	EXPECT_EQ(ExpectedDriverForPack(true, "malisx2 r44p1", "libvulkan_malisx2.so", ""), "malisx2");
	// Packs released before the rename.
	EXPECT_EQ(ExpectedDriverForPack(true, "libmali r44p1", "libvulkan_mali.so", ""), "malisx2");
	EXPECT_EQ(ExpectedDriverForPack(true, "some-pack", "libvulkan_armsx2_mali.so", ""), "malisx2");
	EXPECT_EQ(ExpectedDriverForPack(true, "mystery", "libvulkan_x.so", ""), "custom");
}

TEST(GSDriverReport, MatchFlagsASilentFallback)
{
	EXPECT_TRUE(ServedDriverMatches("turnip", "turnip"));
	// A Turnip pack that fell back to the system loader answers as the Qualcomm blob.
	EXPECT_FALSE(ServedDriverMatches("turnip", "vendor-qualcomm"));
	EXPECT_FALSE(ServedDriverMatches("malisx2", "vendor-arm"));
	EXPECT_TRUE(ServedDriverMatches("malisx2", "malisx2"));
	// A pack of either name is expected to answer with the same label, whichever driverInfo it has.
	EXPECT_TRUE(ServedDriverMatches(ExpectedDriverForPack(true, "libmali r44p1", "libvulkan_mali.so", ""),
		ClassifyServedDriver(Facts(0x13B5, DriverIdValue::ArmProprietary, "Mali-G615", "v1.r44p1-malisx2.0.2.s0123abcd", "Mali-G615")).answered));
	EXPECT_TRUE(ServedDriverMatches(ExpectedDriverForPack(true, "malisx2 v0.0.2", "libvulkan_malisx2.so", ""),
		ClassifyServedDriver(Facts(0x13B5, DriverIdValue::ArmProprietary, "Mali-G615", "v1.r44p1-libmali.0.1.s0123abcd", "Mali-G615")).answered));
	// With the system driver selected, whatever answered is the system driver.
	EXPECT_TRUE(ServedDriverMatches("system", "vendor-qualcomm"));
	EXPECT_TRUE(ServedDriverMatches("system", "turnip"));
	EXPECT_FALSE(ServedDriverMatches("custom", "vendor-arm"));
	EXPECT_TRUE(ServedDriverMatches("custom", "turnip"));
}

TEST(GSDriverReport, AxflTagParseSharesTheProfileParser)
{
	EXPECT_EQ(ParseAxflGeneration("Mesa 26.1.2 (git-axfl1-005)"), 1u);
	EXPECT_EQ(ParseAxflGeneration("Mesa 27.0.0 (git-axfl12-a) extra"), 12u);
	EXPECT_EQ(ParseAxflGeneration("Mesa 26.1.2 (git-axfl-005)"), 0u);
	EXPECT_EQ(ParseAxflGeneration(""), 0u);
}

TEST(GSDriverReport, DriverVersionDecodesPerVendor)
{
	std::string scheme;
	// The Nova's stock blob: 512.676.53.
	const uint32_t qcom = (512u << 22) | (676u << 12) | 53u;
	EXPECT_EQ(DecodeDriverVersion(0x5143, DriverIdValue::QualcommProprietary, qcom, &scheme), "512.676.53");
	EXPECT_NE(scheme.find("qualcomm"), std::string::npos);
	// Mesa 26.2.99.
	const uint32_t mesa = (26u << 22) | (2u << 12) | 99u;
	EXPECT_EQ(DecodeDriverVersion(0x5143, DriverIdValue::MesaTurnip, mesa, &scheme), "26.2.99");
	EXPECT_NE(scheme.find("mesa"), std::string::npos);
	// NVIDIA packs 10.8.8.6.
	const uint32_t nv = (560u << 22) | (35u << 14) | (3u << 6) | 0u;
	EXPECT_EQ(DecodeDriverVersion(0x10DE, 4, nv, &scheme), "560.35.3.0");
	EXPECT_EQ(FormatApiVersion((1u << 22) | (3u << 12) | 128u), "1.3.128");
}

TEST(GSDriverReport, JsonWriterNestsAndEscapes)
{
	JsonWriter w;
	w.BeginObject();
	w.KeyString("s", "a\"b\\c\n\x01");
	w.KeyInt("i", -3);
	w.KeyUInt("u", 18446744073709551615ull);
	w.KeyDouble("nan", 0.0 / 0.0);
	w.KeyHex("h", 0x5143);
	w.Key("a");
	w.BeginArray();
	w.Bool(true);
	w.Null();
	w.BeginObject();
	w.EndObject();
	w.EndArray();
	w.KeyRaw("raw", "{\"x\": 1}");
	w.EndObject();
	ASSERT_TRUE(w.IsComplete());

	std::vector<std::pair<std::string, std::string>> scalars;
	ASSERT_TRUE(ParseJsonObjectScalars(w.GetString(), &scalars)) << w.GetString();
	ASSERT_EQ(scalars.size(), 5u);
	EXPECT_EQ(scalars[0].first, "s");
	EXPECT_EQ(scalars[0].second, "a\"b\\c\n\x01");
	EXPECT_EQ(scalars[1].second, "-3");
	EXPECT_EQ(scalars[2].second, "18446744073709551615");
	EXPECT_EQ(scalars[3].second, "null");
	EXPECT_EQ(scalars[4].second, "0x5143");
}

TEST(GSDriverReport, JsonEscapeReplacesInvalidUtf8)
{
	EXPECT_EQ(EscapeJsonString("ok \xC3\xA9"), "ok \xC3\xA9");
	EXPECT_EQ(EscapeJsonString("bad \xFF!"), "bad \xEF\xBF\xBD!");
	EXPECT_EQ(EscapeJsonString("cut \xE2\x82"), "cut \xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(GSDriverReport, JsonReaderRejectsWhatIsNotOneObject)
{
	EXPECT_TRUE(ParseJsonObjectScalars("{\"name\": \"pack\", \"n\": [1, {\"x\": null}], \"v\": 2.5e3}", nullptr));
	EXPECT_FALSE(ParseJsonObjectScalars("{\"name\": \"pack\",}", nullptr));
	EXPECT_FALSE(ParseJsonObjectScalars("[1]", nullptr));
	EXPECT_FALSE(ParseJsonObjectScalars("{\"a\": 1} trailing", nullptr));
	EXPECT_FALSE(ParseJsonObjectScalars("{\"a\": 01x}", nullptr));
	EXPECT_FALSE(ParseJsonObjectScalars("", nullptr));
}

TEST(GSDriverReport, StepLogRecordsAFailureAndKeepsGoing)
{
	StepLog steps;
	EXPECT_TRUE(steps.Run("ok", [](std::string&) { return true; }));
	EXPECT_FALSE(steps.Run("fails", [](std::string& err) {
		err = "VK_ERROR_INITIALIZATION_FAILED";
		return false;
	}));
	EXPECT_TRUE(steps.Run("after", [](std::string&) { return true; }));
	ASSERT_EQ(steps.GetSteps().size(), 3u);
	EXPECT_FALSE(steps.GetSteps()[1].ok);
	EXPECT_EQ(steps.GetSteps()[1].error, "VK_ERROR_INITIALIZATION_FAILED");
	EXPECT_TRUE(steps.GetSteps()[2].ok);

	JsonWriter w;
	steps.Write(w);
	EXPECT_NE(w.GetString().find("\"error\": null"), std::string::npos);
}

// The report writes every device rule under its field name, true or false, in declaration order.
// The log prints the true ones from the same name table, so the two cannot drift apart.
TEST(GSDriverReport, GpuProfileWritesEveryVulkanDeviceRuleByName)
{
	VulkanDeviceRules rules;
	rules.broken_timestamp_queries = true;
	rules.avoid_push_descriptors = true;
	rules.barrier_road_measured = true;

	JsonWriter w;
	WriteGpuProfile(w, GpuProfileSelection{}, &rules);
	const std::string& json = w.GetString();

	const size_t block = json.find("\"vulkan_device_rules\"");
	ASSERT_NE(block, std::string::npos) << json;
	size_t at = block;
	const char* const expected[] = {
		"\"broken_timestamp_queries\": true",
		"\"avoid_feedback_loop_layout\": false",
		"\"avoid_push_descriptors\": true",
		"\"broken_provoking_vertex\": false",
		"\"broken_colormask_with_depth\": false",
		"\"broken_mad_deinterlace\": false",
		"\"adreno8xx_proprietary\": false",
		"\"self_read_costs_measured\": false",
		"\"barrier_road_measured\": true",
	};
	for (const char* line : expected)
	{
		const size_t found = json.find(line, at);
		ASSERT_NE(found, std::string::npos) << line << " missing or out of order";
		at = found;
	}
}
