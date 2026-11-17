// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The row queue (GS/Renderers/Common/GSRowQueue.h) through GSDevice::DrawRow on the deviceless None backend: a twin that
// follows its base waits until a device entry point could observe it, and later bases on the same targets are drawn
// ahead of it; every other row is drawn at once, after the held twins. Uploads, clears and copies of what a held twin
// or the remembered base uses emit the twins first and forget the base. The device records the tag each row carries in
// its first vertex's fog word.

#include <gtest/gtest.h>

#include "GS/Renderers/Common/GSMeasurementOverrides.h"
#include "GS/Renderers/Common/GSRowQueue.h"
#include "GS/Renderers/Null/GSDeviceNone.h"

#include <cstring>
#include <initializer_list>
#include <memory>
#include <vector>

namespace
{
	class RowDevice final : public GSDeviceNone
	{
	public:
		std::vector<u32> drawn;

		RowDevice() { m_rows_defer = true; }

		void DoDrawRow(GSHWDrawConfig& config, GSRowDeviceCache&) override { drawn.push_back(config.verts[0].FOG); }
		void DoRenderHW(GSHWDrawConfig& config) override { drawn.push_back(0x80000000u | config.verts[0].FOG); }
	};

	std::unique_ptr<GSTexture> Target()
	{
		return std::make_unique<GSTextureNone>(GSTexture::RenderTarget, 64, 64, 1, GSTexture::Format::Color);
	}

	std::unique_ptr<GSTexture> Texture()
	{
		return std::make_unique<GSTextureNone>(GSTexture::Texture, 16, 16, 1, GSTexture::Format::Color);
	}

	/// One triangle; `tag` goes in the first vertex's fog word, `z` in every vertex's depth.
	struct Row
	{
		Row(GSTexture* rt, u32 tag, u32 z = 0, GSTexture* tex = nullptr)
		{
			std::memset(static_cast<void*>(&config), 0, sizeof(config));
			for (u32 i = 0; i < 3; i++)
			{
				verts[i].XYZ.X = static_cast<u16>(16 * i);
				verts[i].XYZ.Y = 16;
				verts[i].XYZ.Z = z;
			}
			verts[0].FOG = tag;
			config.rt = rt;
			config.tex = tex;
			config.verts = verts;
			config.nverts = 3;
			config.indices = indices;
			config.nindices = 3;
			config.colormask.wrgba = 0xF;
		}

		/// A twin base: plain full-colour write, depth written under GREATER.
		Row& Base()
		{
			config.depth.ztst = ZTST_GREATER;
			config.depth.zwe = 1;
			return *this;
		}

		/// A twin: GEQUAL, no depth write.
		Row& Twin()
		{
			config.depth.ztst = ZTST_GEQUAL;
			config.depth.zwe = 0;
			return *this;
		}

		GSHWDrawConfig config;
		GSVertex verts[3] = {};
		u16 indices[3] = {0, 1, 2};
		GSRowDeviceCache cache = {};
	};

	/// A base (tag 10 + p) and its twin (tag 20 + p), both at depth 100 * (p + 1).
	struct Pair
	{
		Pair(GSTexture* rt, u32 p)
			: base(rt, 10 + p, 100 * (p + 1))
			, twin(rt, 20 + p, 100 * (p + 1))
		{
			base.Base();
			twin.Twin();
		}
		Row base, twin;
	};

	class GSRowQueueTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			g_gs_device = std::make_unique<RowDevice>();
			dev = static_cast<RowDevice*>(g_gs_device.get());
		}
		void TearDown() override
		{
			dev->Destroy();
			g_gs_device.reset();
		}

		/// Draws each row in turn.
		void Draw(std::initializer_list<Row*> rows)
		{
			for (Row* r : rows)
				dev->DrawRow(r->config, r->cache);
		}

		RowDevice* dev = nullptr;
	};
} // namespace

TEST_F(GSRowQueueTest, ABaseIsDrawnAtOnce)
{
	auto rt = Target();
	Row a(rt.get(), 1);
	a.Base();
	dev->DrawRow(a.config, a.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
}

TEST_F(GSRowQueueTest, TwinsWaitUntilSomethingCouldSeeThem)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11}));
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 20, 21}));
}

TEST_F(GSRowQueueTest, OnePairThenABaseDrawsTheTwinLast)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	Draw({&p0.base, &p0.twin, &b});
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 20}));
}

TEST_F(GSRowQueueTest, EmittedAndMovedCountTwins)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	const u64 emitted = GSRowQueue::Emitted();
	const u64 moved = GSRowQueue::Moved();
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	dev->FlushBeforeReadback();
	EXPECT_EQ(GSRowQueue::Emitted() - emitted, 2u);
	EXPECT_EQ(GSRowQueue::Moved() - moved, 1u); // the first twin, drawn after the second base
}

TEST_F(GSRowQueueTest, WithoutTheSortNoTwinIsHeld)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	g_gs_measurement_overrides.rows_unsorted = true;
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	g_gs_measurement_overrides.rows_unsorted = false;
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11, 21}));
}

TEST_F(GSRowQueueTest, GeometryIsCopied)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Draw({&p0.base, &p0.twin});
	p0.twin.verts[0].FOG = 99; // the next draw reuses the caller's buffers
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowQueueTest, TheBasesPositionsAreCopied)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	dev->DrawRow(p0.base.config, p0.base.cache);
	for (GSVertex& v : p0.base.verts)
		v.XYZ.Z = 999; // the caller's buffers hold the next draw by the time the twin comes
	dev->DrawRow(p0.twin.config, p0.twin.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10}));
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowQueueTest, AColourClipRowIsDrawnAtOnceAfterTheTwins)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	b.config.colclip_mode = GSHWDrawConfig::ColClipMode::ConvertOnly;
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowQueueTest, ABaseOnAnotherTargetEmitsTheTwinsFirst)
{
	auto rt = Target();
	auto rt2 = Target();
	Pair p0(rt.get(), 0);
	Row b(rt2.get(), 11, 200);
	b.Base();
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowQueueTest, ABaseThatSamplesTheTargetEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200, rt.get());
	b.Base();
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowQueueTest, ABaseThatReadsTheTargetThroughABarrierEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	b.config.require_one_barrier = true;
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowQueueTest, ABaseThatBlendsInASecondPassEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	b.config.blend_multi_pass.enable = true;
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowQueueTest, ABaseWithALogicOpEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	b.config.colormask.logic_op = GSAlphaBitLogicOp::SetBit;
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowQueueTest, ADepthClearBetweenABaseAndItsTwinMeansTheTwinIsNotHeld)
{
	auto rt = Target();
	auto ds = std::make_unique<GSTextureNone>(GSTexture::DepthStencil, 64, 64, 1, GSTexture::Format::DepthStencil);
	Pair p0(rt.get(), 0);
	p0.base.config.ds = ds.get();
	p0.twin.config.ds = ds.get();
	dev->DrawRow(p0.base.config, p0.base.cache);
	dev->ClearDepth(ds.get(), 0.0f);
	dev->DrawRow(p0.twin.config, p0.twin.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowQueueTest, AnOrdinaryDrawBetweenABaseAndItsTwinMeansTheTwinIsNotHeld)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row x(rt.get(), 2);
	dev->DrawRow(p0.base.config, p0.base.cache);
	dev->RenderHW(x.config);
	dev->DrawRow(p0.twin.config, p0.twin.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 0x80000002u, 20}));
}

TEST_F(GSRowQueueTest, AnUploadIntoASampledTextureEmitsFirst)
{
	auto rt = Target();
	auto tex = Texture();
	auto other = Texture();
	Pair p0(rt.get(), 0);
	p0.twin.config.tex = tex.get();
	Draw({&p0.base, &p0.twin});
	u32 pixels[16 * 16] = {};
	other->Update(GSVector4i(0, 0, 16, 16), pixels, 16 * 4);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10}));
	tex->Update(GSVector4i(0, 0, 16, 16), pixels, 16 * 4);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowQueueTest, AClearOfTheTargetEmitsFirst)
{
	auto rt = Target();
	auto other = Target();
	Pair p0(rt.get(), 0);
	Draw({&p0.base, &p0.twin});
	dev->ClearRenderTarget(other.get(), 0);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10}));
	dev->ClearRenderTarget(rt.get(), 0);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowQueueTest, ARowThatIsNeitherIsDrawnAtOnceAfterTheTwins)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 2); // tests nothing, writes no depth: neither a base nor a twin
	Draw({&p0.base, &p0.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10}));
	dev->DrawRow(b.config, b.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 2}));
}

TEST_F(GSRowQueueTest, AnOrdinaryDrawEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 2);
	Draw({&p0.base, &p0.twin});
	dev->RenderHW(b.config);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 0x80000002u}));
}

TEST_F(GSRowQueueTest, ATwinWindowDrawsItsBasesFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1), p2(rt.get(), 2);
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin, &p2.base, &p2.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 12}));
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 12, 20, 21, 22}));
}

TEST_F(GSRowQueueTest, ATwinMustMatchItsBasePositionForPosition)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	p1.twin.verts[2].XYZ.Z = 201; // one depth differs: not a twin, so drawn at once after the held twin
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 20, 21}));
}

TEST_F(GSRowQueueTest, ABlendedBaseIsNotATwinBase)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	p1.base.config.blend.enable = true; // the later base no longer covers what the earlier twin added
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11, 21}));
}

TEST_F(GSRowQueueTest, ACopyOutOfTheTargetEmitsFirst)
{
	auto rt = Target();
	auto dst = Target();
	Pair p0(rt.get(), 0);
	Draw({&p0.base, &p0.twin});
	dev->CopyRect(rt.get(), dst.get(), GSVector4i(0, 0, 16, 16), 0, 0);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowQueueTest, ADepthClearAndAnInvalidationOfQueuedTexturesEmitFirst)
{
	auto rt = Target();
	auto ds = std::make_unique<GSTextureNone>(GSTexture::DepthStencil, 64, 64, 1, GSTexture::Format::DepthStencil);
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	p0.base.config.ds = ds.get();
	p0.twin.config.ds = ds.get();
	Draw({&p0.base, &p0.twin});
	dev->ClearDepth(ds.get(), 0.0f);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
	Draw({&p1.base, &p1.twin});
	dev->InvalidateRenderTarget(rt.get());
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11, 21}));
}

TEST_F(GSRowQueueTest, ARecycledSampledTextureWaitsForTheQueue)
{
	auto rt = Target();
	GSTexture* tex = Texture().release(); // the pool owns it once recycled
	Pair p0(rt.get(), 0);
	p0.twin.config.tex = tex;
	Draw({&p0.base, &p0.twin});
	dev->Recycle(tex);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10})); // held back rather than flushed for
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowQueueTest, ABaseThatFloorsItsDepthStillHasATwin)
{
	// Adreno floors a depth-writing base's depth and not its twin's; the pair is still a twin.
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	p0.base.config.ps.zfloor = 1;
	p1.base.config.ps.zfloor = 1;
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11}));
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 20, 21}));
}
