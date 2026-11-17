// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The row queue (GS/Renderers/Common/GSRowQueue.h) through GSDevice::DrawRow on the deviceless None backend: rows the
// twin sort could move wait until a device entry point could observe them, other rows, a colour-clip row and a row for
// another target pair do not wait,
// uploads and clears of what a row uses emit the queue first, and the twin sort draws a window's bases before its
// twins. The device records the tag each row carries in its first vertex's fog word.

#include <gtest/gtest.h>

#include "GS/Renderers/Common/GSRowQueue.h"
#include "GS/Renderers/Null/GSDeviceNone.h"

#include <cstring>
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

		RowDevice* dev = nullptr;
	};
} // namespace

TEST_F(GSRowQueueTest, RowsWaitUntilSomethingCouldSeeThem)
{
	auto rt = Target();
	Row a(rt.get(), 1), b(rt.get(), 2);
	a.Base();
	b.Base();
	dev->DrawRow(a.config, a.cache);
	dev->DrawRow(b.config, b.cache);
	EXPECT_TRUE(dev->drawn.empty());
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1, 2}));
}

TEST_F(GSRowQueueTest, GeometryIsCopied)
{
	auto rt = Target();
	Row a(rt.get(), 1);
	a.Base();
	dev->DrawRow(a.config, a.cache);
	a.verts[0].FOG = 99; // the next draw reuses the caller's buffers
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
}

TEST_F(GSRowQueueTest, AColourClipRowIsDrawnAtOnceAfterTheQueue)
{
	auto rt = Target();
	Row a(rt.get(), 1), b(rt.get(), 2);
	a.Base();
	b.Base();
	b.config.colclip_mode = GSHWDrawConfig::ColClipMode::ConvertOnly;
	dev->DrawRow(a.config, a.cache);
	dev->DrawRow(b.config, b.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1, 2}));
}

TEST_F(GSRowQueueTest, AnotherTargetEmitsTheQueueFirst)
{
	auto rt = Target();
	auto rt2 = Target();
	Row a(rt.get(), 1), b(rt2.get(), 2);
	a.Base();
	b.Base();
	dev->DrawRow(a.config, a.cache);
	dev->DrawRow(b.config, b.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1, 2}));
}

TEST_F(GSRowQueueTest, AnUploadIntoASampledTextureEmitsFirst)
{
	auto rt = Target();
	auto tex = Texture();
	auto other = Texture();
	Row a(rt.get(), 1, 0, tex.get());
	a.Base();
	dev->DrawRow(a.config, a.cache);
	u32 pixels[16 * 16] = {};
	other->Update(GSVector4i(0, 0, 16, 16), pixels, 16 * 4);
	EXPECT_TRUE(dev->drawn.empty());
	tex->Update(GSVector4i(0, 0, 16, 16), pixels, 16 * 4);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
}

TEST_F(GSRowQueueTest, AClearOfTheTargetEmitsFirst)
{
	auto rt = Target();
	auto other = Target();
	Row a(rt.get(), 1);
	a.Base();
	dev->DrawRow(a.config, a.cache);
	dev->ClearRenderTarget(other.get(), 0);
	EXPECT_TRUE(dev->drawn.empty());
	dev->ClearRenderTarget(rt.get(), 0);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
}

TEST_F(GSRowQueueTest, ARowTheSortCannotMoveIsDrawnAtOnceAfterTheQueue)
{
	auto rt = Target();
	Row a(rt.get(), 1), b(rt.get(), 2);
	a.Base();
	dev->DrawRow(a.config, a.cache);
	EXPECT_TRUE(dev->drawn.empty());
	dev->DrawRow(b.config, b.cache); // tests nothing, writes no depth: neither a base nor a twin
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1, 2}));
}

TEST_F(GSRowQueueTest, AnOrdinaryDrawEmitsTheQueueFirst)
{
	auto rt = Target();
	Row a(rt.get(), 1), b(rt.get(), 2);
	a.Base();
	dev->DrawRow(a.config, a.cache);
	dev->RenderHW(b.config);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1, 0x80000002u}));
}

TEST_F(GSRowQueueTest, ATwinWindowDrawsItsBasesFirst)
{
	auto rt = Target();
	std::vector<std::unique_ptr<Row>> rows;
	for (u32 p = 0; p < 3; p++)
	{
		rows.push_back(std::make_unique<Row>(rt.get(), 10 + p, 100 * (p + 1)));
		rows.back()->Base();
		rows.push_back(std::make_unique<Row>(rt.get(), 20 + p, 100 * (p + 1)));
		rows.back()->Twin();
	}
	for (auto& r : rows)
		dev->DrawRow(r->config, r->cache);
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 12, 20, 21, 22}));
}

TEST_F(GSRowQueueTest, ATwinMustMatchItsBasePositionForPosition)
{
	auto rt = Target();
	Row b1(rt.get(), 10, 100), t1(rt.get(), 20, 100), b2(rt.get(), 11, 200), t2(rt.get(), 21, 201);
	b1.Base();
	t1.Twin();
	b2.Base();
	t2.Twin(); // one depth differs: not a twin, so the window is one pair
	for (Row* r : {&b1, &t1, &b2, &t2})
		dev->DrawRow(r->config, r->cache);
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11, 21}));
}

TEST_F(GSRowQueueTest, ABlendedBaseIsNotATwinBase)
{
	auto rt = Target();
	Row b1(rt.get(), 10, 100), t1(rt.get(), 20, 100), b2(rt.get(), 11, 200), t2(rt.get(), 21, 200);
	b1.Base();
	t1.Twin();
	b2.Base();
	t2.Twin();
	b2.config.blend.enable = true; // the later base no longer covers what the earlier twin added
	for (Row* r : {&b1, &t1, &b2, &t2})
		dev->DrawRow(r->config, r->cache);
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11, 21}));
}

TEST_F(GSRowQueueTest, ACopyOutOfTheTargetEmitsFirst)
{
	auto rt = Target();
	auto dst = Target();
	Row a(rt.get(), 1);
	a.Base();
	dev->DrawRow(a.config, a.cache);
	dev->CopyRect(rt.get(), dst.get(), GSVector4i(0, 0, 16, 16), 0, 0);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
}

TEST_F(GSRowQueueTest, ADepthClearAndAnInvalidationOfQueuedTexturesEmitFirst)
{
	auto rt = Target();
	auto ds = std::make_unique<GSTextureNone>(GSTexture::DepthStencil, 64, 64, 1, GSTexture::Format::DepthStencil);
	Row a(rt.get(), 1), b(rt.get(), 2);
	a.Base();
	a.config.ds = ds.get();
	dev->DrawRow(a.config, a.cache);
	dev->ClearDepth(ds.get(), 0.0f);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
	b.Base();
	dev->DrawRow(b.config, b.cache);
	dev->InvalidateRenderTarget(rt.get());
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1, 2}));
}

TEST_F(GSRowQueueTest, ARecycledSampledTextureWaitsForTheQueue)
{
	auto rt = Target();
	GSTexture* tex = Texture().release(); // the pool owns it once recycled
	Row a(rt.get(), 1, 0, tex);
	a.Base();
	dev->DrawRow(a.config, a.cache);
	dev->Recycle(tex);
	EXPECT_TRUE(dev->drawn.empty()); // held back rather than flushed for
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
}
