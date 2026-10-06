// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSFastStencilShadow.h"
#include "GS/Renderers/Common/GSFramebufferFetchPolicy.h"
#include "GS/Renderers/Common/GSGPUProfile.h"
#include "GS/Renderers/Common/GSSelfReadRoadPolicy.h"
#include "GS/Renderers/Null/GSNullDeviceProfile.h"

#include <gtest/gtest.h>

#include <cstring>

// ---------------------------------------------------------------------------
// The null device's feature profiles.
//
// These are the feature sets `-renderer nullhw` measures a device THROUGH, so every
// bit is a claim about a real part, copied from that part's own start-up banner and
// from the rules in GSGPUDriverProfile.cpp. The whole table is pinned here rather
// than spot-checked, because the failure mode is silent: a wrong bit does not crash
// or look odd, it produces a well-formed count about a device that does not exist.
// Jak II at 2x reported 4,836 draws a frame on the featureless device where the
// SD865 runs 1,479, and nothing said so.
//
// A deliberate edit to the table breaks these and is meant to. Re-derive the new
// value from a device banner, not from what makes the test pass.
// ---------------------------------------------------------------------------

using GSNullDeviceProfile::Id;

TEST(GsNullDeviceProfile, DefaultIsTheSnapdragon865)
{
	// The primary perf target, so a number taken with no -nullhw-profile is about it.
	EXPECT_EQ(Id::Sd865, GSNullDeviceProfile::kDefault);
}

TEST(GsNullDeviceProfile, NamesRoundTripAndUnknownNamesAreRejected)
{
	for (Id id : {Id::Sd865, Id::MaliG615, Id::MaliG615Malisx2, Id::Blank})
	{
		const std::optional<Id> parsed = GSNullDeviceProfile::Parse(GSNullDeviceProfile::Name(id));
		ASSERT_TRUE(parsed.has_value()) << "name " << GSNullDeviceProfile::Name(id);
		EXPECT_EQ(id, parsed.value());
	}

	// No silent fallback to the default: an unrecognised profile has to be an error at
	// the call site, or a run measures a device the caller did not ask for.
	EXPECT_FALSE(GSNullDeviceProfile::Parse("").has_value());
	EXPECT_FALSE(GSNullDeviceProfile::Parse("SD865").has_value()); // case-sensitive
	EXPECT_FALSE(GSNullDeviceProfile::Parse("adreno650").has_value());
	EXPECT_FALSE(GSNullDeviceProfile::Parse("mali").has_value());
	EXPECT_FALSE(GSNullDeviceProfile::Parse("malisx2").has_value());
	EXPECT_FALSE(GSNullDeviceProfile::Parse("mali-g615-malisx").has_value());
}

TEST(GsNullDeviceProfile, Sd865IsTheRenderTargetCopyRoad)
{
	const GSDevice::FeatureSupport f = GSNullDeviceProfile::Features(Id::Sd865);

	// The road: UseRenderTargetCopyForFeedback turns the barriers off, which takes the
	// in-tile fetch with it, and dual-source blending is present.
	EXPECT_FALSE(f.texture_barrier);
	EXPECT_FALSE(f.framebuffer_fetch);
	EXPECT_FALSE(f.framebuffer_fetch_orders_overlap);
	EXPECT_TRUE(f.dual_source_blend);
	// Which is exactly the condition for the alpha stencil counter through the blend
	// unit -- the bit that stops auto-flush cutting Jak II's shadow volume into
	// one-triangle draws. Asked of the rule rather than asserted, so the table cannot
	// drift away from GSFastStencilShadow.
	EXPECT_TRUE(f.fast_stencil_shadow);
	EXPECT_EQ(GSFastStencilShadow::DeviceQualifies({.api = RenderAPI::Vulkan,
				  .dual_source_blend = f.dual_source_blend,
				  .road = GSSelfReadRoad::Copy}),
		f.fast_stencil_shadow);

	EXPECT_FALSE(f.test_and_sample_depth); // texture_barrier && !is_adreno
	EXPECT_FALSE(f.stencil_buffer); // vk-turnip-d32s8-early-z-late-z-hang
	EXPECT_TRUE(f.broken_blend_constant); // vk-turnip-blend-constant-ignored
	EXPECT_FALSE(f.no_ps2_z_quantization); // Mali/Apple only
	EXPECT_FALSE(f.feedback_loop_layout); // ROAA present, so the layout road is out
	EXPECT_FALSE(f.broken_mad_deinterlace); // Mali-G57 only

	// Shared mobile-Vulkan facts.
	EXPECT_TRUE(f.vs_expand);
	EXPECT_TRUE(f.primitive_id);
	EXPECT_TRUE(f.provoking_vertex_last);
	EXPECT_TRUE(f.point_expand);
	EXPECT_TRUE(f.line_expand);
	EXPECT_TRUE(f.prefer_new_textures);
	EXPECT_TRUE(f.astc_textures);
	EXPECT_FALSE(f.dxt_textures);
	EXPECT_FALSE(f.bptc_textures);
	EXPECT_FALSE(f.multidraw_fb_copy);
	EXPECT_FALSE(f.cheap_rt_feedback_read);
	EXPECT_FALSE(f.broken_point_sampler);
	EXPECT_FALSE(f.rov);
	EXPECT_FALSE(f.depth_feedback);
	EXPECT_FALSE(f.aa1);
	EXPECT_FALSE(f.cas_sharpening);
	EXPECT_FALSE(f.fsr1);
	EXPECT_FALSE(f.sgsr);
	EXPECT_FALSE(f.metalfx_spatial);
}

TEST(GsNullDeviceProfile, MaliG615IsTheInTileFetchRoad)
{
	const GSDevice::FeatureSupport f = GSNullDeviceProfile::Features(Id::MaliG615);

	// MT6897 is exempt from both rules that would take the in-tile read away
	// (vk-arm-r44p1-attachment-self-read and vk-mediatek-mali-roaa-destination-read),
	// so the barriers stay on and the ROAA fetch stands. The 2026-08 banner logs from
	// the same part predate those exemptions and are not this row.
	EXPECT_TRUE(f.texture_barrier);
	EXPECT_TRUE(f.framebuffer_fetch);
	EXPECT_TRUE(f.framebuffer_fetch_orders_overlap);
	// Mali Vulkan stacks report dualSrcBlend false, so SRC1 equations are emulated in
	// shader per draw -- and the stencil counter stays on the per-face frame read.
	EXPECT_FALSE(f.dual_source_blend);
	EXPECT_FALSE(f.fast_stencil_shadow);
	// The in-tile read is an InPassOrdered road the device chose for itself, and it declares no
	// feedback loop -- so the counter is declined on the road as well as on dual-source blending.
	EXPECT_EQ(GSFastStencilShadow::DeviceQualifies({.api = RenderAPI::Vulkan,
				  .dual_source_blend = f.dual_source_blend,
				  .road = GSSelfReadRoad::InPassOrdered}),
		f.fast_stencil_shadow);

	EXPECT_TRUE(f.test_and_sample_depth); // barriers on, not Adreno
	EXPECT_FALSE(f.stencil_buffer); // stencil_buffer &= !framebuffer_fetch
	EXPECT_FALSE(f.broken_blend_constant);
	EXPECT_TRUE(f.no_ps2_z_quantization); // skip gl_FragDepth so early-ZS survives
	EXPECT_FALSE(f.feedback_loop_layout); // r44p1 never advertises the extension
	EXPECT_FALSE(f.broken_mad_deinterlace); // G57 only, this is a G615

	EXPECT_TRUE(f.vs_expand);
	EXPECT_TRUE(f.primitive_id);
	EXPECT_TRUE(f.provoking_vertex_last);
	EXPECT_TRUE(f.point_expand);
	EXPECT_TRUE(f.line_expand);
	EXPECT_TRUE(f.prefer_new_textures);
	EXPECT_TRUE(f.astc_textures);
	EXPECT_FALSE(f.dxt_textures);
	EXPECT_FALSE(f.bptc_textures);
	EXPECT_FALSE(f.multidraw_fb_copy);
	EXPECT_FALSE(f.cheap_rt_feedback_read);
	EXPECT_FALSE(f.broken_point_sampler);
	EXPECT_FALSE(f.rov);
	EXPECT_FALSE(f.depth_feedback);
	EXPECT_FALSE(f.aa1);
}

// ---------------------------------------------------------------------------
// mali-g615-malisx2: the same part on our own driver.
// ---------------------------------------------------------------------------

namespace
{
	// VK_DRIVER_ID_ARM_PROPRIETARY. malisx2 reports Arm's vendorID, driverID, 44.1.0 and device name
	// on purpose; only driverInfo names it.
	constexpr u32 kArmDriverId = 9;
	constexpr u32 PackVulkanVersion(u32 major, u32 minor, u32 patch)
	{
		return (major << 22) | (minor << 12) | patch;
	}
	constexpr const char* kMaliSX2DriverInfo = "v1.r44p1-malisx2.0.2.s0123abcd";
	constexpr const char* kMaliR44p1DriverInfo = "v1.r44p1-01eac0.abc";
	// What the RG 477V's own SoC properties hand the resolver (GSGPUProfile.cpp BuildHints).
	constexpr const char* kRg477vHints = "ro.soc.manufacturer=Mediatek | ro.soc.model=MT6897 | "
										 "ro.board.platform=mt6897 | ro.hardware=mt6897 | ro.product.board=k6897v1_64";
	// A MediaTek part nobody measured: the SoC exemptions the stock blob leans on do not cover it.
	constexpr const char* kUnmeasuredMediaTekHints = "ro.soc.manufacturer=Mediatek | ro.soc.model=MT6985 | "
													 "ro.board.platform=mt6985";

	// A Mali-G615 as GSDeviceVK resolves it: the driver-bug database and device rules through the same
	// resolver, then the framebuffer-fetch and self-read-road decisions fed from them. Only the inputs
	// that vary between the cases here are parameters.
	struct ResolvedMali
	{
		GpuProfileSelection selection;
		VulkanDeviceRules rules;
		GSSelfReadRoadDecision road;
	};

	ResolvedMali ResolveMaliG615(const char* driver_info, std::string_view platform_hints, bool roaa, u32 max_push_descriptors)
	{
		MobileDriverContext context;
		context.api = MobileGpuApi::Vulkan;
		context.vendor_id = GpuVendorID::ARM;
		context.driver_id = kArmDriverId;
		context.driver_version = PackVulkanVersion(44, 1, 0);
		context.driver_name = "ARM proprietary";
		context.driver_info = driver_info;
		context.platform_hints = platform_hints;
		context.roaa_color_access = roaa;
		context.max_push_descriptors = max_push_descriptors;

		ResolvedMali out;
		out.selection = GpuProfileDetector::Resolve("auto", std::string_view(), "Mali-G615", context);
		out.rules = GpuProfileDetector::ResolveVulkanDeviceRules(out.selection, context, "Mali-G615");

		const MobileDriverProfile& driver = out.selection.driver;
		GSVulkanFramebufferFetchInputs fetch_inputs;
		fetch_inputs.roaa_available = roaa;
		fetch_inputs.is_mali = true;
		fetch_inputs.broken_destination_read = driver.HasBug(DriverBug::BrokenRoaaDestinationRead);
		const GSVulkanFramebufferFetchDecision fetch = DecideVulkanFramebufferFetch(fetch_inputs);

		GSSelfReadRoadInputs road_inputs;
		road_inputs.in_tile_read_available = fetch.enabled;
		// r44p1 does not advertise the layout extension, and avoid_feedback_loop_layout would clear it.
		road_inputs.layout_road_available = false;
		road_inputs.roaa_available = roaa;
		road_inputs.rt_self_read_is_broken = driver.UsesWorkaround(DriverWorkaround::UseRenderTargetCopyForFeedback);
		road_inputs.driver_orders_declared_loop = driver.orders_declared_feedback_loop &&
		                                          (!driver.declared_loop_orders_overlap_on_request || roaa);
		road_inputs.driver_prefers_declared_loop_with_barriers = driver.prefers_declared_loop_with_barriers;
		out.road = DecideSelfReadRoad(road_inputs);
		return out;
	}
} // namespace

TEST(GsNullDeviceProfile, MaliG615Malisx2IsTheInTileFetchRoadOnOurDriver)
{
	const GSDevice::FeatureSupport f = GSNullDeviceProfile::Features(Id::MaliG615Malisx2);

	// The RG 477V banner with the malisx2 pack: ROAA=yes fbfetch=yes(in-tile) texbarrier=on dualSrc=NO
	// pushdesc=on, provoking vertex supported.
	EXPECT_TRUE(f.texture_barrier);
	EXPECT_TRUE(f.framebuffer_fetch);
	EXPECT_TRUE(f.framebuffer_fetch_orders_overlap);
	EXPECT_FALSE(f.dual_source_blend);
	EXPECT_TRUE(f.provoking_vertex_last);
	EXPECT_TRUE(f.line_expand);
	// The feedback-loop layout stays avoided (r44p1 rule kept by decision).
	EXPECT_FALSE(f.feedback_loop_layout);
	EXPECT_FALSE(f.declared_feedback_loop_orders_overlap);
	// Dual-source blending absent and an in-tile read: the stencil counter keeps the frame read.
	EXPECT_FALSE(f.fast_stencil_shadow);
	EXPECT_TRUE(f.test_and_sample_depth);
	EXPECT_TRUE(f.no_ps2_z_quantization);
	EXPECT_FALSE(f.stencil_buffer);
	EXPECT_FALSE(f.broken_blend_constant);
	EXPECT_FALSE(f.broken_mad_deinterlace);
	EXPECT_TRUE(f.feedback_loops());
}

TEST(GsNullDeviceProfile, MaliG615Malisx2HasEveryBitTheStockProfileHas)
{
	// Push descriptors, bresenham lines, the memory budget and device fault are the things malisx2
	// changes on this part, and none of them is a FeatureSupport bit. The stock row already stands on
	// MT6897's exemptions, so the two tables agree bit for bit and only the name differs. If a bit is
	// ever made to differ, it is a decision about one of these devices: say which in the commit, and
	// edit this test.
	const GSDevice::FeatureSupport stock = GSNullDeviceProfile::Features(Id::MaliG615);
	const GSDevice::FeatureSupport sx2 = GSNullDeviceProfile::Features(Id::MaliG615Malisx2);
	EXPECT_EQ(0, std::memcmp(&stock, &sx2, sizeof(stock)));

	EXPECT_STRNE(GSNullDeviceProfile::Name(Id::MaliG615), GSNullDeviceProfile::Name(Id::MaliG615Malisx2));
	EXPECT_STRNE(GSNullDeviceProfile::Description(Id::MaliG615), GSNullDeviceProfile::Description(Id::MaliG615Malisx2));
}

TEST(GsNullDeviceProfile, MaliG615Malisx2AgreesWithWhatTheResolverGivesTheRg477v)
{
	// The device as the banner showed it: MT6897 hints, ROAA advertised, 32 push descriptors.
	const ResolvedMali dev = ResolveMaliG615(kMaliSX2DriverInfo, kRg477vHints, true, 32);

	EXPECT_EQ(GpuProfileDetector::DescribeMatchedRules(dev.selection.driver),
		"vk-arm-proprietary, vk-arm-dynamic-rendering-before-r52");
	EXPECT_EQ(GpuProfileDetector::DescribeDeviceRules(dev.rules),
		"broken_timestamp_queries, avoid_feedback_loop_layout, exempt_malisx2_push_descriptors");
	EXPECT_FALSE(dev.rules.avoid_push_descriptors);

	// The road those rules put the device on is the one the profile's bits describe.
	const GSDevice::FeatureSupport f = GSNullDeviceProfile::Features(Id::MaliG615Malisx2);
	EXPECT_EQ(GSSelfReadRoad::InPassOrdered, dev.road.road);
	EXPECT_EQ(f.texture_barrier, dev.road.texture_barrier);
	EXPECT_EQ(f.framebuffer_fetch, dev.road.in_tile_read);
	EXPECT_EQ(f.framebuffer_fetch_orders_overlap, dev.road.in_tile_read);
	EXPECT_EQ(f.feedback_loop_layout, dev.road.force_feedback_loop_layout);
	EXPECT_EQ(f.declared_feedback_loop_orders_overlap, dev.road.orders_overlapping_prims);
	EXPECT_EQ(GSSelfReadRoadFromPublishedBits(f.framebuffer_fetch, f.texture_barrier,
				  f.declared_feedback_loop_orders_overlap),
		dev.road.road);
}

TEST(GsNullDeviceProfile, MaliG615Malisx2DoesNotNeedTheMt6897Exemptions)
{
	// The stock profile is the in-tile road only because MT6897 is exempt from the r44p1 row and the
	// MediaTek destination-read row. Off that SoC the stock blob takes the render-target copy road...
	const ResolvedMali stock = ResolveMaliG615(kMaliR44p1DriverInfo, kUnmeasuredMediaTekHints, true, 32);
	EXPECT_EQ(GSSelfReadRoad::Copy, stock.road.road);
	EXPECT_TRUE(stock.rules.avoid_push_descriptors);

	// ...and malisx2 stays on the in-tile road, by rule: it is exempt from the r44p1 row and, with ROAA,
	// from the MediaTek row.
	const ResolvedMali sx2 = ResolveMaliG615(kMaliSX2DriverInfo, kUnmeasuredMediaTekHints, true, 32);
	EXPECT_EQ(GSSelfReadRoad::InPassOrdered, sx2.road.road);
	EXPECT_TRUE(sx2.road.in_tile_read);
	EXPECT_TRUE(sx2.road.texture_barrier);
	EXPECT_EQ(GpuProfileDetector::DescribeMatchedRules(sx2.selection.driver),
		"vk-arm-proprietary, vk-arm-dynamic-rendering-before-r52, "
		"vk-arm-r44p1-attachment-self-read (exempt: malisx2), "
		"vk-mediatek-mali-roaa-destination-read (exempt: malisx2)");

	// So the malisx2 profile describes the device on any Mali-G615 it ships on, where the stock one
	// describes the RG 477V only.
	const GSDevice::FeatureSupport f = GSNullDeviceProfile::Features(Id::MaliG615Malisx2);
	EXPECT_EQ(f.framebuffer_fetch, sx2.road.in_tile_read);
	EXPECT_EQ(f.texture_barrier, sx2.road.texture_barrier);
}

TEST(GsNullDeviceProfile, BlankIsExactlyTheFeatureSupportDefault)
{
	// The pre-profile null device, kept so a 20-dump table and a per-title table
	// taken before profiles existed stay reproducible. It must be the constructor's own answer and nothing
	// else -- including dual_source_blend, which that constructor sets true.
	const GSDevice::FeatureSupport blank = GSNullDeviceProfile::Features(Id::Blank);
	const GSDevice::FeatureSupport ctor;
	EXPECT_EQ(0, std::memcmp(&blank, &ctor, sizeof(blank)));
	EXPECT_TRUE(blank.dual_source_blend);
	EXPECT_FALSE(blank.fast_stencil_shadow);
	EXPECT_FALSE(blank.vs_expand);
	EXPECT_FALSE(blank.texture_barrier);
}

TEST(GsNullDeviceProfile, TheTwoDeviceProfilesDisagreeWhereTheHardwareDoes)
{
	// The point of having two: they must not quietly become the same table. These four
	// are the bits that move counts between the Adreno and Mali roads.
	const GSDevice::FeatureSupport a = GSNullDeviceProfile::Features(Id::Sd865);
	const GSDevice::FeatureSupport m = GSNullDeviceProfile::Features(Id::MaliG615);

	EXPECT_NE(a.texture_barrier, m.texture_barrier);
	EXPECT_NE(a.framebuffer_fetch, m.framebuffer_fetch);
	EXPECT_NE(a.dual_source_blend, m.dual_source_blend);
	EXPECT_NE(a.fast_stencil_shadow, m.fast_stencil_shadow);

	// feedback_loops() is the helper ~20 GSRendererHW sites branch on: false on the
	// Adreno road (every frame read is a copy) and true on the Mali one.
	EXPECT_FALSE(a.feedback_loops());
	EXPECT_TRUE(m.feedback_loops());
}
