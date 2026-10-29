// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the blend carry-over decision (GS/Renderers/Common/GSBlendCarry.h): a draw whose blend was
// elided as the identity (Cs - Cd) * As + Cd with As = 1.0 takes the blend of the blended draw of the
// same equation submitted just before it, when the two agree on everything else that reaches the
// output. The carry is exact only while that agreement holds, so the tests cover which draws can be
// a donor, which can take one, and that every difference outside the blend bits refuses.
//
// Rides gs_vertex_tests -- the decision is header-only.

#include "GS/Renderers/Common/GSBlendCarry.h"

#include <cstring>
#include <gtest/gtest.h>

namespace
{
	using GSBlendCarry::Donor;
	using PSS = GSHWDrawConfig::PSSelector;

	constexpr u32 kAlpha = GSBlendCarry::kIdentityAlpha;
	int s_rt, s_ds, s_other;

	GSHWDrawConfig Zeroed()
	{
		GSHWDrawConfig c;
		std::memset(static_cast<void*>(&c), 0, sizeof(c));
		c.colclip_mode = GSHWDrawConfig::ColClipMode::NoModify;
		c.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Off;
		c.tex_hazard = GSHWDrawConfig::TEX_HAZARD_NONE;
		return c;
	}

	// A plain textured draw with the identity equation blended through the blend-mix form.
	GSHWDrawConfig Blended()
	{
		GSHWDrawConfig c = Zeroed();
		c.ps.aem_fmt = 1;
		c.ps.blend_mix = 1;
		c.ps.blend_a = 0;
		c.ps.blend_b = 1;
		c.ps.blend_d = 1;
		c.ps.no_color1 = 1;
		c.blend.enable = true;
		c.blend.op = 0;
		c.blend.src_factor = 1;
		c.blend.dst_factor = 5;
		c.colormask.wr = c.colormask.wg = c.colormask.wb = c.colormask.wa = 1;
		c.depth.ztst = 1;
		return c;
	}

	GSHWDrawConfig Elided()
	{
		GSHWDrawConfig c = Blended();
		c.ps.blend_mix = 0;
		c.ps.blend_a = c.ps.blend_b = c.ps.blend_d = 0;
		c.ps.no_color1 = 0;
		c.blend = {};
		return c;
	}

	Donor DonorOf(const GSHWDrawConfig& c)
	{
		return GSBlendCarry::MakeDonor(c, &s_rt, &s_ds, kAlpha);
	}
} // namespace

TEST(GSBlendCarry, WithoutBlendBitsClearsOnlyTheBlendBits)
{
	PSS all;
	all.key_lo = ~0ull;
	all.key_hi = ~0ull;
	const PSS stripped = GSBlendCarry::WithoutBlendBits(all);
	EXPECT_EQ(stripped.blend_a, 0u);
	EXPECT_EQ(stripped.blend_b, 0u);
	EXPECT_EQ(stripped.blend_c, 0u);
	EXPECT_EQ(stripped.blend_d, 0u);
	EXPECT_EQ(stripped.blend_hw, 0u);
	EXPECT_EQ(stripped.a_masked, 0u);
	EXPECT_EQ(stripped.blend_mix, 0u);
	EXPECT_EQ(stripped.round_inv, 0u);
	EXPECT_EQ(stripped.pabe, 0u);
	EXPECT_EQ(stripped.no_color1, 0u);
	EXPECT_EQ(stripped.blend_factor_in_alpha, 0u);
	EXPECT_EQ(stripped.af_in_src1, 0u);

	// Nothing else is touched.
	EXPECT_EQ(stripped.aem_fmt, 3u);
	EXPECT_EQ(stripped.dst_fmt, 3u);
	EXPECT_EQ(stripped.dither, 3u);
	EXPECT_EQ(stripped.fbmask, 1u);
	EXPECT_EQ(stripped.rta_correction, 1u);
	EXPECT_EQ(stripped.rta_source_correction, 1u);
	EXPECT_EQ(stripped.colclip, 1u);

	// The blended and the elided form of one draw differ only in those bits.
	EXPECT_TRUE(GSBlendCarry::WithoutBlendBits(Blended().ps) == GSBlendCarry::WithoutBlendBits(Elided().ps));
}

TEST(GSBlendCarry, DonorAndElidedClasses)
{
	EXPECT_TRUE(GSBlendCarry::IsDonorClass(Blended(), kAlpha));
	EXPECT_FALSE(GSBlendCarry::IsDonorClass(Blended(), GSBlendCarry::PackAlpha(0, 1, 0, 2))) << "another equation";
	EXPECT_FALSE(GSBlendCarry::IsDonorClass(Elided(), kAlpha)) << "no blend to carry";
	EXPECT_TRUE(GSBlendCarry::IsElidedIdentity(Elided()));
	EXPECT_FALSE(GSBlendCarry::IsElidedIdentity(Blended()));

	GSHWDrawConfig c = Blended();
	c.ps.blend_c = 2;
	EXPECT_FALSE(GSBlendCarry::IsDonorClass(c, kAlpha)) << "a fixed factor is not the As form";
	c = Blended();
	c.ps.blend_hw = 1;
	EXPECT_FALSE(GSBlendCarry::IsDonorClass(c, kAlpha));
	c = Blended();
	c.ps.blend_mix = 2;
	EXPECT_FALSE(GSBlendCarry::IsDonorClass(c, kAlpha));
	c = Blended();
	c.ps.pabe = 1;
	EXPECT_FALSE(GSBlendCarry::IsDonorClass(c, kAlpha));

	// An elided draw that already carries a blend-set bit is not the elided form.
	c = Elided();
	c.ps.a_masked = 1;
	EXPECT_FALSE(GSBlendCarry::IsElidedIdentity(c));
}

TEST(GSBlendCarry, PlainShapeRefusesEveryReadOfTheTarget)
{
	using Mut = void (*)(GSHWDrawConfig&);
	const Mut refusals[] = {
		[](GSHWDrawConfig& c) { c.ps.dst_fmt = 2; },
		[](GSHWDrawConfig& c) { c.ps.colclip = 1; },
		[](GSHWDrawConfig& c) { c.ps.colclip_hw = 1; },
		[](GSHWDrawConfig& c) { c.ps.dither = 1; },
		[](GSHWDrawConfig& c) { c.ps.fbmask = 1; },
		[](GSHWDrawConfig& c) { c.ps.shuffle = 1; },
		[](GSHWDrawConfig& c) { c.ps.tex_is_fb = 1; },
		[](GSHWDrawConfig& c) { c.ps.no_color = 1; },
		[](GSHWDrawConfig& c) { c.ps.abe = 1; },
		[](GSHWDrawConfig& c) { c.ps.stencil_counter = 1; },
		[](GSHWDrawConfig& c) { c.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil; },
		[](GSHWDrawConfig& c) { c.require_one_barrier = true; },
		[](GSHWDrawConfig& c) { c.require_full_barrier = true; },
		[](GSHWDrawConfig& c) { c.alpha_second_pass.enable = true; },
		[](GSHWDrawConfig& c) { c.blend_multi_pass.enable = true; },
		[](GSHWDrawConfig& c) { c.colclip_mode = GSHWDrawConfig::ColClipMode::EarlyResolve; },
		[](GSHWDrawConfig& c) { c.tex_hazard = GSHWDrawConfig::TEX_HAZARD_RT; },
		[](GSHWDrawConfig& c) { c.line_expand = true; },
		[](GSHWDrawConfig& c) { c.colormask.logic_op = 1; },
		[](GSHWDrawConfig& c) { c.date_copy = 1; },
		[](GSHWDrawConfig& c) { c.logic_op_split = 3; },
	};

	for (size_t i = 0; i < std::size(refusals); i++)
	{
		GSHWDrawConfig b = Blended();
		refusals[i](b);
		EXPECT_FALSE(GSBlendCarry::IsDonorClass(b, kAlpha)) << "refusal " << i;
		GSHWDrawConfig e = Elided();
		refusals[i](e);
		EXPECT_FALSE(GSBlendCarry::IsElidedIdentity(e)) << "refusal " << i;
	}
}

TEST(GSBlendCarry, AlphaCorrectionDoesNotRefuse)
{
	// A target with alpha correction is one pixel shader selector value on both draws, which Matches compares.
	GSHWDrawConfig b = Blended();
	b.ps.rta_correction = 1;
	b.ps.rta_source_correction = 1;
	GSHWDrawConfig e = Elided();
	e.ps.rta_correction = 1;
	e.ps.rta_source_correction = 1;
	EXPECT_TRUE(GSBlendCarry::IsDonorClass(b, kAlpha));
	EXPECT_TRUE(GSBlendCarry::IsElidedIdentity(e));
	EXPECT_TRUE(GSBlendCarry::Matches(DonorOf(b), e, &s_rt, &s_ds, kAlpha));

	// ...and a donor with it does not serve an elided draw without it.
	EXPECT_FALSE(GSBlendCarry::Matches(DonorOf(b), Elided(), &s_rt, &s_ds, kAlpha));
}

TEST(GSBlendCarry, AdoptGivesTheElidedDrawTheDonorsBlendAndNothingElse)
{
	const GSHWDrawConfig donor_conf = Blended();
	const Donor d = DonorOf(donor_conf);
	GSHWDrawConfig e = Elided();
	ASSERT_TRUE(GSBlendCarry::Matches(d, e, &s_rt, &s_ds, kAlpha));

	const PSS before = e.ps;
	GSBlendCarry::Adopt(d, e);
	EXPECT_TRUE(e.ps == donor_conf.ps) << "the pipeline is the donor's";
	EXPECT_EQ(e.blend.key, donor_conf.blend.key);
	EXPECT_TRUE(GSBlendCarry::WithoutBlendBits(e.ps) == GSBlendCarry::WithoutBlendBits(before)) << "only blend bits moved";

	// The donor stays the donor: a second elided draw takes the same blend.
	GSHWDrawConfig e2 = Elided();
	ASSERT_TRUE(GSBlendCarry::Matches(d, e2, &s_rt, &s_ds, kAlpha));
	GSBlendCarry::Adopt(d, e2);
	EXPECT_TRUE(e2.ps == e.ps);
}

TEST(GSBlendCarry, MatchesRefusesAnythingElseThatDiffers)
{
	const Donor d = DonorOf(Blended());
	const GSHWDrawConfig e = Elided();
	EXPECT_TRUE(GSBlendCarry::Matches(d, e, &s_rt, &s_ds, kAlpha));

	EXPECT_FALSE(GSBlendCarry::Matches(d, e, &s_other, &s_ds, kAlpha)) << "another render target";
	EXPECT_FALSE(GSBlendCarry::Matches(d, e, &s_rt, &s_other, kAlpha)) << "another depth target";
	EXPECT_FALSE(GSBlendCarry::Matches(d, e, &s_rt, &s_ds, GSBlendCarry::PackAlpha(1, 0, 0, 1))) << "another equation";

	Donor invalid = d;
	invalid.valid = false;
	EXPECT_FALSE(GSBlendCarry::Matches(invalid, e, &s_rt, &s_ds, kAlpha));

	GSHWDrawConfig c = e;
	c.colormask.wa = 0;
	EXPECT_FALSE(GSBlendCarry::Matches(d, c, &s_rt, &s_ds, kAlpha)) << "another colour mask";
	c = e;
	c.depth.zwe = 1;
	EXPECT_FALSE(GSBlendCarry::Matches(d, c, &s_rt, &s_ds, kAlpha)) << "another depth state";
	c = e;
	c.ps.aem_fmt = 2;
	EXPECT_FALSE(GSBlendCarry::Matches(d, c, &s_rt, &s_ds, kAlpha)) << "another pixel shader";
	c = e;
	c.ps.dither = 1;
	EXPECT_FALSE(GSBlendCarry::Matches(d, c, &s_rt, &s_ds, kAlpha)) << "a pixel shader bit outside the blend bits";
}
