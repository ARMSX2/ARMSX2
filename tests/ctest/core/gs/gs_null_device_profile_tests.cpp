// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSFastStencilShadow.h"
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
	for (Id id : {Id::Sd865, Id::MaliG615, Id::Blank})
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
