// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the kept target copy of the copy road (GS/Renderers/Common/GSRunSnapshot.h): which draws
// may sample a copy taken for an earlier draw, and what each draw does to the copy's standing.

#include "GS/Renderers/Common/GSRunSnapshot.h"

#include <gtest/gtest.h>

#include <cstring>

namespace
{
	using GSRunSnapshot::Copy;
	using GSRunSnapshot::Reads;

	GSHWDrawConfig Plain()
	{
		GSHWDrawConfig c;
		std::memset(static_cast<void*>(&c), 0, sizeof(c));
		c.colormask.wrgba = 0xF;
		return c;
	}

	/// A draw whose emulated mask keeps `mask_alpha` of the alpha byte and nothing of the colour.
	GSHWDrawConfig Masked(u8 mask_alpha)
	{
		GSHWDrawConfig c = Plain();
		c.ps.fbmask = 1;
		c.cb_ps.FbMask = GSVector4i(0, 0, 0, mask_alpha);
		c.require_one_barrier = true;
		return c;
	}

	/// The same with the destination-alpha test done in the shader.
	GSHWDrawConfig MaskedDATE(u8 mask_alpha, u32 datm)
	{
		GSHWDrawConfig c = Masked(mask_alpha);
		c.ps.date = 5 + datm;
		c.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Full;
		return c;
	}

	/// The backend's bookkeeping, without the textures.
	struct Model
	{
		bool live = false;
		/// False while a clear of the target is marked on the texture and not yet recorded.
		bool target_dirty = true;
		u8 stale = 0;
		GSVector4i area = GSVector4i::zero();
		u32 copies = 0;

		Copy Draw(const GSHWDrawConfig& c, const GSVector4i& drawarea, bool reads_copy = true)
		{
			const u8 stale_before = stale;
			if (live)
				stale |= GSRunSnapshot::DrawWrites(c);
			live = GSRunSnapshot::Live(live, true, target_dirty, false);
			// The draw's own pass setup records the pending clear, after the decision below.
			const bool clear_lands = !target_dirty;
			target_dirty = true;
			if (!reads_copy)
				return Copy::Reuse;

			const bool covered = live && area.rintersect(drawarea).eq(drawarea);
			const Copy copy = GSRunSnapshot::Decide(live, stale_before, covered, GSRunSnapshot::DrawReads(c));
			if (copy != Copy::Reuse)
				copies++;
			if (copy == Copy::DrawArea || copy == Copy::Target)
			{
				live = true;
				area = (copy == Copy::Target) ? GSVector4i(0, 0, 640, 448) : drawarea;
				stale = GSRunSnapshot::DrawWrites(c);
			}
			if (clear_lands)
				stale = 0xFF;
			return copy;
		}
	};
} // namespace

TEST(GSRunSnapshot, MaskedDrawReadsOnlyTheBitsItKeeps)
{
	EXPECT_EQ(GSRunSnapshot::DrawReads(Masked(0x81)), (Reads{true, 0x81}));
	EXPECT_EQ(GSRunSnapshot::DrawReads(Masked(0x01)), (Reads{true, 0x01}));
	EXPECT_EQ(GSRunSnapshot::DrawWrites(Masked(0x81)), 0x7E);
}

TEST(GSRunSnapshot, ShaderDestinationAlphaTestReadsBitSeven)
{
	EXPECT_EQ(GSRunSnapshot::DrawReads(MaskedDATE(0x01, 0)), (Reads{true, 0x81}));
	EXPECT_EQ(GSRunSnapshot::DrawReads(MaskedDATE(0x81, 1)), (Reads{true, 0x81}));

	// Without a mask the draw writes the bit it tests.
	GSHWDrawConfig c = Plain();
	c.ps.date = 5;
	c.require_one_barrier = true;
	EXPECT_EQ(GSRunSnapshot::DrawReads(c), (Reads{true, 0x80}));
	EXPECT_EQ(GSRunSnapshot::DrawWrites(c), 0xFF);
}

TEST(GSRunSnapshot, EveryOtherReadTakesItsOwnCopy)
{
	// Blending in the shader reads the destination colour.
	GSHWDrawConfig c = Masked(0x81);
	c.ps.blend_a = 1;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);

	// A mask on a colour channel.
	c = Masked(0x81);
	c.cb_ps.FbMask = GSVector4i(0x0F, 0, 0, 0x81);
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);

	// The target as the draw's texture.
	c = Masked(0x81);
	c.tex_hazard = GSHWDrawConfig::TEX_HAZARD_RT;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);
	c = Masked(0x81);
	c.ps.tex_is_fb = 1;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);
	c = Masked(0x81);
	c.offset_read_hits_write = true;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);

	// The alpha-fail read, scaled alpha, a shuffle, a second pass.
	c = Masked(0x81);
	c.ps.afail = GSShader::PS_AFAIL::RGB_ONLY;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);
	c = Masked(0x81);
	c.ps.rta_correction = 1;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);
	c = Masked(0x81);
	c.ps.shuffle = 1;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);
	c = Masked(0x81);
	c.alpha_second_pass.enable = true;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);
	c = Masked(0x81);
	c.blend_multi_pass.enable = true;
	EXPECT_FALSE(GSRunSnapshot::DrawReads(c).alpha_only);
}

TEST(GSRunSnapshot, WritesKeepTheMaskOnlyWhenAlphaReachesTheTargetAsWritten)
{
	EXPECT_EQ(GSRunSnapshot::DrawWrites(Plain()), 0xFF);

	GSHWDrawConfig c = Plain();
	c.colormask.wrgba = 0x7;
	EXPECT_EQ(GSRunSnapshot::DrawWrites(c), 0x00);

	// Blended colour with alpha passed through: the mask holds.
	c = Masked(0x81);
	c.blend = {true, GSDevice::SRC1_ALPHA, GSDevice::INV_SRC1_ALPHA, GSDevice::OP_ADD, GSDevice::CONST_ONE,
		GSDevice::CONST_ZERO, false, 0};
	EXPECT_EQ(GSRunSnapshot::DrawWrites(c), 0x7E);

	// Alpha through the blend unit, a second pass, a logic op, scaled alpha: assume everything.
	c.blend.src_factor_alpha = GSDevice::SRC1_ALPHA;
	EXPECT_EQ(GSRunSnapshot::DrawWrites(c), 0xFF);
	c = Masked(0x81);
	c.alpha_second_pass.enable = true;
	EXPECT_EQ(GSRunSnapshot::DrawWrites(c), 0xFF);
	c = Masked(0x81);
	c.colormask.logic_op = GSAlphaBitLogicOp::SetBit;
	EXPECT_EQ(GSRunSnapshot::DrawWrites(c), 0xFF);
	c = Masked(0x81);
	c.ps.rta_correction = 1;
	EXPECT_EQ(GSRunSnapshot::DrawWrites(c), 0xFF);

	// The second pass alone writing alpha still counts.
	c = Plain();
	c.colormask.wrgba = 0x7;
	c.alpha_second_pass.enable = true;
	c.alpha_second_pass.colormask.wrgba = 0x8;
	EXPECT_EQ(GSRunSnapshot::DrawWrites(c), 0xFF);
}

// Beyond Good & Evil's water: draws that keep bits 7 and 0 and test bit 7 in alternating
// polarity, anywhere on the target. Two copies serve the run.
TEST(GSRunSnapshot, ARunOfMaskedTestsTakesTwoCopies)
{
	Model m;
	EXPECT_EQ(m.Draw(MaskedDATE(0x81, 1), GSVector4i(10, 10, 40, 40)), Copy::DrawArea);
	EXPECT_EQ(m.Draw(MaskedDATE(0x81, 0), GSVector4i(10, 10, 40, 40)), Copy::Reuse);
	EXPECT_EQ(m.Draw(MaskedDATE(0x81, 1), GSVector4i(300, 200, 400, 300)), Copy::Target);
	for (int i = 0; i < 280; i++)
		EXPECT_EQ(m.Draw(MaskedDATE(0x81, i & 1), GSVector4i(i, i, i + 50, i + 50)), Copy::Reuse);
	EXPECT_EQ(m.copies, 2u);
	EXPECT_EQ(m.stale, 0x7E);
}

// A draw between them that writes the kept bits ends the run; one that does not write alpha, or
// keeps the same bits, does not.
TEST(GSRunSnapshot, OnlyAWriteOfTheBitsReadEndsTheRun)
{
	Model m;
	m.Draw(Masked(0x81), GSVector4i(0, 0, 640, 448));

	GSHWDrawConfig rgb_only = Plain();
	rgb_only.colormask.wrgba = 0x7;
	m.Draw(rgb_only, GSVector4i(0, 0, 640, 448), false);
	EXPECT_EQ(m.Draw(Masked(0x81), GSVector4i(5, 5, 9, 9)), Copy::Reuse);

	// Keeps bit 0 only, so it writes bit 7: a reader of bit 0 goes on, a reader of bit 7 starts over.
	EXPECT_EQ(m.Draw(Masked(0x01), GSVector4i(5, 5, 9, 9)), Copy::Reuse);
	EXPECT_EQ(m.Draw(Masked(0x01), GSVector4i(5, 5, 9, 9)), Copy::Reuse);
	EXPECT_EQ(m.Draw(Masked(0x81), GSVector4i(5, 5, 9, 9)), Copy::DrawArea);

	m.Draw(Plain(), GSVector4i(0, 0, 640, 448), false);
	EXPECT_EQ(m.Draw(Masked(0x01), GSVector4i(5, 5, 9, 9)), Copy::DrawArea);
}

// A reader of something else between two run members copies for itself and leaves the run's
// copy standing, as long as it keeps the run's bits.
TEST(GSRunSnapshot, AnotherReaderDoesNotEndTheRun)
{
	Model m;
	m.Draw(Masked(0x81), GSVector4i(0, 0, 640, 448));

	GSHWDrawConfig sw_blend = Masked(0x81);
	sw_blend.ps.blend_a = 1;
	EXPECT_EQ(m.Draw(sw_blend, GSVector4i(0, 0, 100, 100)), Copy::Private);
	EXPECT_EQ(m.Draw(Masked(0x81), GSVector4i(50, 50, 80, 80)), Copy::Reuse);
	EXPECT_EQ(m.copies, 2u);
}

// A clear of the target is only marked on the texture and recorded when the next draw sets up its
// pass, which is after that draw has chosen its copy. The kept copy predates the clear.
TEST(GSRunSnapshot, APendingClearEndsTheRun)
{
	Model m;
	m.Draw(Masked(0x81), GSVector4i(0, 0, 640, 448));
	EXPECT_EQ(m.Draw(Masked(0x81), GSVector4i(5, 5, 9, 9)), Copy::Reuse);

	m.target_dirty = false;
	EXPECT_EQ(m.Draw(MaskedDATE(0x81, 0), GSVector4i(5, 5, 9, 9)), Copy::DrawArea);
	// The clear landed after that copy, so the next reader starts over too.
	EXPECT_EQ(m.Draw(MaskedDATE(0x81, 1), GSVector4i(5, 5, 9, 9)), Copy::DrawArea);
	EXPECT_EQ(m.Draw(MaskedDATE(0x81, 0), GSVector4i(5, 5, 9, 9)), Copy::Reuse);
}

TEST(GSRunSnapshot, LiveNeedsTheSamePassAndASettledTarget)
{
	EXPECT_TRUE(GSRunSnapshot::Live(true, true, true, false));
	EXPECT_FALSE(GSRunSnapshot::Live(false, true, true, false));
	EXPECT_FALSE(GSRunSnapshot::Live(true, false, true, false));
	EXPECT_FALSE(GSRunSnapshot::Live(true, true, false, false));
	EXPECT_FALSE(GSRunSnapshot::Live(true, true, true, true));
}
