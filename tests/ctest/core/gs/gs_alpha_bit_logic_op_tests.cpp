// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the alpha-bit logic op (GSAlphaBitLogicOp.h): a draw that writes only alpha bit 7 sets or
// clears it with a logic op instead of reading the target, where that read costs a wait per draw.
// The classifier is tested on its own, then through the renderer on the None backend.

#include "gs_hw_draw_harness.h"

#include "GS/Renderers/Common/GSAlphaBitLogicOp.h"

#include <initializer_list>
#include <vector>

using namespace GSHWDrawHarness;

namespace
{
	/// Flat triangles, one alpha per triangle, all three vertices carrying it.
	struct Tris
	{
		std::vector<GSVertex> v;
		std::vector<u16> i;

		explicit Tris(std::initializer_list<u8> alphas)
		{
			for (const u8 a : alphas)
			{
				for (int k = 0; k < 3; k++)
				{
					GSVertex vtx = {};
					vtx.RGBAQ.A = a;
					i.push_back(static_cast<u16>(v.size()));
					v.push_back(vtx);
				}
			}
		}

		GSAlphaBitLogicOp::Runs Classify(bool iip = false, bool fba = false) const
		{
			return GSAlphaBitLogicOp::ClassifyTriangles(v.data(), i.data(), static_cast<u32>(i.size()), iip, fba);
		}
	};
} // namespace

TEST(GSAlphaBitLogicOpClassify, OneOpPerDrawIsOneRun)
{
	const GSAlphaBitLogicOp::Runs set = Tris({0x80, 0x80, 0x80}).Classify();
	EXPECT_EQ(set.first_op, GSAlphaBitLogicOp::SetBit);
	EXPECT_EQ(set.first_indices, 0u);

	const GSAlphaBitLogicOp::Runs clear = Tris({0x00, 0x00}).Classify();
	EXPECT_EQ(clear.first_op, GSAlphaBitLogicOp::ClearBit);
	EXPECT_EQ(clear.first_indices, 0u);
}

// Indiana Jones' marks: set on the first triangles, clear on the rest. Split at the first clear.
TEST(GSAlphaBitLogicOpClassify, SetThenClearSplitsAtTheFirstClear)
{
	const GSAlphaBitLogicOp::Runs r = Tris({0x80, 0x80, 0x00, 0x00, 0x00}).Classify();
	EXPECT_EQ(r.first_op, GSAlphaBitLogicOp::SetBit);
	EXPECT_EQ(r.first_indices, 6u);

	const GSAlphaBitLogicOp::Runs c = Tris({0x00, 0x80}).Classify();
	EXPECT_EQ(c.first_op, GSAlphaBitLogicOp::ClearBit);
	EXPECT_EQ(c.first_indices, 3u);
}

// A third run would need a third draw; the read stays.
TEST(GSAlphaBitLogicOpClassify, ThreeRunsDoNotQualify)
{
	EXPECT_EQ(Tris({0x80, 0x00, 0x80}).Classify().first_op, GSAlphaBitLogicOp::Off);
}

// The op writes 0x80 into the stored alpha; any of bits 0..6 in the source would be ORed in too.
TEST(GSAlphaBitLogicOpClassify, AlphaWithLowBitsDoesNotQualify)
{
	EXPECT_EQ(Tris({0x80, 0x81}).Classify().first_op, GSAlphaBitLogicOp::Off);
	EXPECT_EQ(Tris({0x40}).Classify().first_op, GSAlphaBitLogicOp::Off);
}

// FBA forces bit 7 on, so every primitive sets it.
TEST(GSAlphaBitLogicOpClassify, FbaSetsEverywhere)
{
	const GSAlphaBitLogicOp::Runs r = Tris({0x00, 0x80, 0x00}).Classify(false, true);
	EXPECT_EQ(r.first_op, GSAlphaBitLogicOp::SetBit);
	EXPECT_EQ(r.first_indices, 0u);
}

// Flat shading reads the last vertex; Gouraud shading needs all three to agree.
TEST(GSAlphaBitLogicOpClassify, GouraudNeedsAgreeingVertices)
{
	Tris t({0x80});
	t.v[0].RGBAQ.A = 0x00;
	EXPECT_EQ(t.Classify(false).first_op, GSAlphaBitLogicOp::SetBit);
	EXPECT_EQ(t.Classify(true).first_op, GSAlphaBitLogicOp::Off);
}

TEST(GSAlphaBitLogicOpClassify, DeviceRule)
{
	EXPECT_TRUE(GSAlphaBitLogicOp::DeviceQualifies(true, true, false));
	EXPECT_FALSE(GSAlphaBitLogicOp::DeviceQualifies(false, true, false)); // no logicOp feature
	EXPECT_FALSE(GSAlphaBitLogicOp::DeviceQualifies(true, false, false)); // the read is cheap here
	EXPECT_FALSE(GSAlphaBitLogicOp::DeviceQualifies(true, true, true)); // RGB would be written
}

namespace
{
	constexpr u32 kFBP = 0x40;

	class GSAlphaBitLogicOpDraw : public Fixture
	{
	protected:
		/// The Adreno 740 on our generation-2 Turnip, as far as this draw is concerned.
		void Device(bool logic_op)
		{
			GSDevice::FeatureSupport& f = m_device->MutableFeatures();
			f.texture_barrier = true;
			f.framebuffer_fetch = false;
			f.feedback_loop_layout = true;
			f.declared_feedback_loop_orders_overlap = true;
			f.declared_loop_overlap_needs_raster_order = true;
			f.ordered_read_costs_per_draw = true;
			f.alpha_bit_logic_op = logic_op;
		}

		/// One flat triangle-list draw on a 32-bit frame with only alpha bit 7 writable, one triangle
		/// per alpha. Every register goes through A+D, so each triangle can carry its own colour.
		void MarkDraw(std::initializer_list<u8> alphas)
		{
			Packet p;
			Environment(p, kFBP, PSMCT32);

			GIFReg r = {};
			r.FRAME.FBP = kFBP;
			r.FRAME.FBW = 10;
			r.FRAME.PSM = PSMCT32;
			r.FRAME.FBMSK = 0x7FFFFFFF;
			p.Reg(GIF_A_D_REG_FRAME_1, r);

			r.U64 = 0;
			r.PRIM.PRIM = GS_TRIANGLELIST;
			p.Reg(GIF_A_D_REG_PRIM, r);

			int n = 0;
			for (const u8 a : alphas)
			{
				r.U64 = 0;
				r.RGBAQ.A = a;
				r.RGBAQ.Q = 1.0f;
				p.Reg(GIF_A_D_REG_RGBAQ, r);

				const int x = 16 + 40 * n++;
				const int xy[3][2] = {{x, 16}, {x + 200, 16}, {x, 216}};
				for (const auto& c : xy)
				{
					r.U64 = 0;
					r.XYZ.X = static_cast<u16>(c[0] * 16);
					r.XYZ.Y = static_cast<u16>(c[1] * 16);
					r.XYZ.Z = 1;
					p.Reg(GIF_A_D_REG_XYZ2, r);
				}
			}

			const u32 before = m_device->m_draws;
			p.Send(*m_gs, GIFRegPRIM{});
			ASSERT_EQ(m_device->m_draws, before + 1);
		}

		void ExpectRead() const
		{
			EXPECT_EQ(m_device->m_colormask.logic_op, GSAlphaBitLogicOp::Off);
			EXPECT_EQ(m_device->m_logic_op_split, 0u);
			EXPECT_TRUE(m_device->m_ps.fbmask);
		}
	};
} // namespace

// Set on two overlapping triangles, clear on the third: two runs, no read, no barrier.
TEST_F(GSAlphaBitLogicOpDraw, SetThenClearTakesTwoLogicOpRuns)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	Device(true);
	MarkDraw({0x80, 0x80, 0x00});

	EXPECT_EQ(m_device->m_colormask.logic_op, GSAlphaBitLogicOp::SetBit);
	EXPECT_EQ(m_device->m_logic_op_split, 6u);
	EXPECT_EQ(m_device->m_colormask.wrgba, 0x8u);
	EXPECT_FALSE(m_device->m_ps.fbmask);
	EXPECT_TRUE(m_device->m_ps.fba);
	EXPECT_FALSE(m_device->m_require_one_barrier);
	EXPECT_FALSE(m_device->m_require_full_barrier);
}

// The same draw on a device where the read does not cost a wait keeps the shader's mask.
TEST_F(GSAlphaBitLogicOpDraw, WithoutTheDeviceRuleTheMaskReads)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	Device(false);
	MarkDraw({0x80, 0x80, 0x00});
	ExpectRead();
}

TEST_F(GSAlphaBitLogicOpDraw, ThreeRunsKeepTheRead)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	Device(true);
	MarkDraw({0x80, 0x00, 0x80});
	ExpectRead();
}

TEST_F(GSAlphaBitLogicOpDraw, AlphaWithLowBitsKeepsTheRead)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	Device(true);
	MarkDraw({0x90, 0x90});
	ExpectRead();
}
