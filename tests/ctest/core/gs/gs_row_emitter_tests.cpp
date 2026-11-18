// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The row emitter's order (GS/Renderers/Common/GSRowEmitter.h) through GSDevice::DrawRow, on the deviceless None
// backend with a backend of its hooks that records tags: recording order is draw order and only twins are held. A twin
// that follows its base waits until a device entry point could observe it, and later bases on the same targets are
// recorded ahead of it; every other row is recorded at once, after the held twins. A row may stay in the backend's open
// group of the bases' side, which is recorded before anything else. Uploads, clears and copies of what a pending row
// uses record it first and forget the base. Each row carries a tag in its first vertex's fog word.
//
// Also the streak rule that decides when a run-program group opens, and its break-even length (GSRunsPolicy.h).

#include <gtest/gtest.h>

#include "GS/Renderers/Common/GSRowEmitter.h"
#include "GS/Renderers/Common/GSRunsPolicy.h"
#include "GS/Renderers/Null/GSDeviceNone.h"

#include <cstring>
#include <initializer_list>
#include <memory>
#include <vector>

namespace
{
	/// A tag with this bit joins the backend's open group of the bases' side.
	constexpr u32 GROUP = 0x100;

	class RowDevice final : public GSDeviceNone
	{
	public:
		std::vector<u32> drawn; ///< tags in recording order; 0x80000000 | tag for a draw that is not a row
		std::vector<u32> group; ///< the open group of the bases' side
		std::vector<u32> held;  ///< held twins
		bool plain = true;      ///< what RowsMayGoAhead says
		bool hold = true;       ///< whether RowsHoldTwin holds

		RowDevice()
		{
			m_rows_defer = true;
			m_max_texture_size = 8192; // what Create would set: surfaces from the pool keep their size
		}

		void DoDrawRow(GSHWDrawConfig& config, GSRowDeviceCache&) override { drawn.push_back(config.verts[0].FOG); }
		void DoRenderHW(GSHWDrawConfig& config) override { drawn.push_back(0x80000000u | config.verts[0].FOG); }
		bool RowsJoin(GSHWDrawConfig& config, GSRowDeviceCache&) override
		{
			if (!(config.verts[0].FOG & GROUP))
				return false;
			group.push_back(config.verts[0].FOG);
			return true;
		}
		void RowsRecordOpen() override
		{
			drawn.insert(drawn.end(), group.begin(), group.end());
			group.clear();
		}
		bool RowsHoldTwin(GSHWDrawConfig& config, GSRowDeviceCache&) override
		{
			if (!hold)
				return false;
			held.push_back(config.verts[0].FOG);
			return true;
		}
		bool RowsMayGoAhead(const GSHWDrawConfig&) override { return plain; }
		void RowsEmit() override
		{
			RowsRecordOpen();
			drawn.insert(drawn.end(), held.begin(), held.end());
			held.clear();
		}
		void RowsDrop() override
		{
			group.clear();
			held.clear();
		}
		bool RowsOpen() const override { return !group.empty(); }
		size_t Parked() const { return m_deferred_recycle.size(); }
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
		Pair(GSTexture* rt, u32 p, u32 group = 0)
			: base(rt, (10 + p) | group, 100 * (p + 1))
			, twin(rt, 20 + p, 100 * (p + 1))
		{
			base.Base();
			twin.Twin();
		}
		Row base, twin;
	};

	class GSRowEmitterTest : public ::testing::Test
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

// The twin hold, as it was before the emitter, ported.

TEST_F(GSRowEmitterTest, ABaseIsDrawnAtOnce)
{
	auto rt = Target();
	Row a(rt.get(), 1);
	a.Base();
	dev->DrawRow(a.config, a.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1}));
}

TEST_F(GSRowEmitterTest, TwinsWaitUntilSomethingCouldSeeThem)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11}));
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 20, 21}));
}

TEST_F(GSRowEmitterTest, OnePairThenABaseDrawsTheTwinLast)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	Draw({&p0.base, &p0.twin, &b});
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 20}));
}

TEST_F(GSRowEmitterTest, EmittedAndMovedCountTwins)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	const u64 emitted = GSRowEmitter::Emitted();
	const u64 moved = GSRowEmitter::Moved();
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	dev->FlushBeforeReadback();
	EXPECT_EQ(GSRowEmitter::Emitted() - emitted, 2u);
	EXPECT_EQ(GSRowEmitter::Moved() - moved, 1u); // the first twin, drawn after the second base
}

TEST_F(GSRowEmitterTest, ATwinTheBackendDoesNotHoldIsDrawnAtOnce)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	dev->hold = false;
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11, 21}));
}

TEST_F(GSRowEmitterTest, TheBasesPositionsAreCopied)
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

TEST_F(GSRowEmitterTest, AColourClipRowIsDrawnAtOnceAfterTheTwins)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	b.config.colclip_mode = GSHWDrawConfig::ColClipMode::ConvertOnly;
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowEmitterTest, ABaseOnAnotherTargetEmitsTheTwinsFirst)
{
	auto rt = Target();
	auto rt2 = Target();
	Pair p0(rt.get(), 0);
	Row b(rt2.get(), 11, 200);
	b.Base();
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowEmitterTest, ABaseThatSamplesTheTargetEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200, rt.get());
	b.Base();
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowEmitterTest, ABaseThatReadsTheTargetThroughABarrierEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	b.config.require_one_barrier = true;
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowEmitterTest, ABaseThatMayLeavePixelsUnwrittenHasNoTwin)
{
	// The twin may move only where its base wrote depth at every pixel it passed: a discard leaves an older, farther
	// depth there that a later base could pass with a depth the twin passes too.
	auto rt = Target();
	const auto discards = {
		+[](GSHWDrawConfig& c) { c.destination_alpha = GSHWDrawConfig::DestinationAlphaMode::Stencil; },
		+[](GSHWDrawConfig& c) { c.ps.date = 1; },
		+[](GSHWDrawConfig& c) { c.ps.atst = GSShader::PS_ATST::GEQUAL; },
		+[](GSHWDrawConfig& c) { c.ps.scanmsk = 2; },
		+[](GSHWDrawConfig& c) { c.ps.aa1 = GSShader::PS_AA1::TRIANGLE_SW_Z; },
	};
	for (const auto discard : discards)
	{
		Pair p0(rt.get(), 0);
		discard(p0.base.config);
		dev->drawn.clear();
		Draw({&p0.base, &p0.twin});
		EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
		dev->FlushBeforeReadback();
	}
}

TEST_F(GSRowEmitterTest, ABaseThatBlendsInASecondPassEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	b.config.blend_multi_pass.enable = true;
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowEmitterTest, ABaseWithALogicOpEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 11, 200);
	b.Base();
	b.config.colormask.logic_op = GSAlphaBitLogicOp::SetBit;
	Draw({&p0.base, &p0.twin, &b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowEmitterTest, ADepthClearBetweenABaseAndItsTwinMeansTheTwinIsNotHeld)
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
	EXPECT_TRUE(dev->held.empty());
}

TEST_F(GSRowEmitterTest, AnOrdinaryDrawBetweenABaseAndItsTwinMeansTheTwinIsNotHeld)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row x(rt.get(), 2);
	dev->DrawRow(p0.base.config, p0.base.cache);
	dev->RenderHW(x.config);
	dev->DrawRow(p0.twin.config, p0.twin.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 0x80000002u, 20}));
}

TEST_F(GSRowEmitterTest, AnUploadIntoASampledTextureEmitsFirst)
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

TEST_F(GSRowEmitterTest, AClearOfTheTargetEmitsFirst)
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

TEST_F(GSRowEmitterTest, ARowThatIsNeitherIsDrawnAtOnceAfterTheTwins)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 2); // tests nothing, writes no depth: neither a base nor a twin
	Draw({&p0.base, &p0.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10}));
	dev->DrawRow(b.config, b.cache);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 2}));
}

TEST_F(GSRowEmitterTest, AnOrdinaryDrawEmitsTheTwinsFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0);
	Row b(rt.get(), 2);
	Draw({&p0.base, &p0.twin});
	dev->RenderHW(b.config);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 0x80000002u}));
}

TEST_F(GSRowEmitterTest, ATwinWindowDrawsItsBasesFirst)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1), p2(rt.get(), 2);
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin, &p2.base, &p2.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 12}));
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 12, 20, 21, 22}));
}

TEST_F(GSRowEmitterTest, ATwinMustMatchItsBasePositionForPosition)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	p1.twin.verts[2].XYZ.Z = 201; // one depth differs: not a twin, so drawn at once after the held twin
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 11, 20, 21}));
}

TEST_F(GSRowEmitterTest, ABlendedBaseIsNotATwinBase)
{
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	p1.base.config.blend.enable = true; // the later base no longer covers what the earlier twin added
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11, 21}));
}

TEST_F(GSRowEmitterTest, ACopyOutOfTheTargetEmitsFirst)
{
	auto rt = Target();
	auto dst = Target();
	Pair p0(rt.get(), 0);
	Draw({&p0.base, &p0.twin});
	dev->CopyRect(rt.get(), dst.get(), GSVector4i(0, 0, 16, 16), 0, 0);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowEmitterTest, ADepthClearAndAnInvalidationOfQueuedTexturesEmitFirst)
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

TEST_F(GSRowEmitterTest, ARecycledSampledTextureWaitsForTheQueue)
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

TEST_F(GSRowEmitterTest, ABaseThatFloorsItsDepthStillHasATwin)
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

// What the emitter adds: the bases' open group, and the rules that keep it in draw order.

TEST_F(GSRowEmitterTest, ABlendedGreaterWriterArrivingWhileTwinsAreHeldIsDrawnAfterThem)
{
	// It tests GREATER and writes depth, but blends: where it drew, the game drew the earlier twin first.
	auto rt = Target();
	Pair p0(rt.get(), 0, GROUP);
	Row d(rt.get(), 11 | GROUP, 200);
	d.Base();
	d.config.blend.enable = true;
	Draw({&p0.base, &p0.twin, &d});
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10 | GROUP, 20, 11 | GROUP}));
}

TEST_F(GSRowEmitterTest, AnAlphaTestedGreaterWriterArrivingWhileTwinsAreHeldIsDrawnAfterThem)
{
	auto rt = Target();
	Pair p0(rt.get(), 0, GROUP);
	Row d(rt.get(), 11 | GROUP, 200);
	d.Base();
	d.config.ps.atst = GSShader::PS_ATST::GEQUAL;
	Draw({&p0.base, &p0.twin, &d});
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10 | GROUP, 20, 11 | GROUP}));
}

TEST_F(GSRowEmitterTest, ANonPlainBaseArrivingWhileTwinsAreHeldIsDrawnAfterThem)
{
	// A base the backend cannot draw without changing the pass (a pending clear on its target, say) takes the full
	// path, which may end the pass the held twins were decided for.
	auto rt = Target();
	Pair p0(rt.get(), 0), p1(rt.get(), 1);
	Draw({&p0.base, &p0.twin});
	dev->plain = false;
	Draw({&p1.base});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20, 11}));
}

TEST_F(GSRowEmitterTest, EqualDepthBasesKeepTheirOrderAcrossAnOpenGroup)
{
	// Two bases at equal depth, both GREATER and depth-writing: the first waits in the open group, the second is drawn
	// at once while a twin is held. Equal depths resolve by order, so the group is recorded before the second base.
	auto rt = Target();
	Pair p0(rt.get(), 0, GROUP);
	Row b1(rt.get(), 11, 100);
	b1.Base();
	Draw({&p0.base, &p0.twin, &b1});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10 | GROUP, 11}));
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10 | GROUP, 11, 20}));
}

TEST_F(GSRowEmitterTest, TheOpenGroupIsRecordedBeforeTheHeldTwins)
{
	auto rt = Target();
	Pair p0(rt.get(), 0, GROUP), p1(rt.get(), 1, GROUP);
	Draw({&p0.base, &p0.twin, &p1.base, &p1.twin});
	EXPECT_TRUE(dev->drawn.empty());
	dev->FlushBeforeReadback();
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10 | GROUP, 11 | GROUP, 20, 21}));
}

TEST_F(GSRowEmitterTest, ARowAfterTheOpenGroupThatCannotJoinIsDrawnAfterIt)
{
	auto rt = Target();
	Row a(rt.get(), 1 | GROUP), b(rt.get(), 2 | GROUP), c(rt.get(), 3);
	Draw({&a, &b, &c});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1 | GROUP, 2 | GROUP, 3}));
}

TEST_F(GSRowEmitterTest, AnOpenGroupIsPendingForEveryEntryPoint)
{
	auto rt = Target();
	auto tex = Texture();
	auto other = Texture();
	Row a(rt.get(), 1 | GROUP, 0, tex.get());
	Draw({&a});
	u32 pixels[16 * 16] = {};
	other->Update(GSVector4i(0, 0, 16, 16), pixels, 16 * 4);
	EXPECT_TRUE(dev->drawn.empty());
	tex->Update(GSVector4i(0, 0, 16, 16), pixels, 16 * 4);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1 | GROUP}));

	Row b(rt.get(), 2 | GROUP);
	Draw({&b});
	dev->ClearRenderTarget(rt.get(), 0);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1 | GROUP, 2 | GROUP}));

	Row c(rt.get(), 3 | GROUP), x(rt.get(), 4);
	Draw({&c});
	dev->RenderHW(x.config);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1 | GROUP, 2 | GROUP, 3 | GROUP, 0x80000004u}));
}

TEST_F(GSRowEmitterTest, AHeldTwinsPaletteUploadEmitsFirst)
{
	auto rt = Target();
	auto tex = Texture();
	auto pal = std::make_unique<GSTextureNone>(GSTexture::Texture, 256, 1, 1, GSTexture::Format::Color);
	Pair p0(rt.get(), 0);
	p0.twin.config.tex = tex.get();
	p0.twin.config.pal = pal.get();
	Draw({&p0.base, &p0.twin});
	u32 pixels[256] = {};
	pal->Update(GSVector4i(0, 0, 256, 1), pixels, 256 * 4);
	EXPECT_EQ(dev->drawn, (std::vector<u32>{10, 20}));
}

TEST_F(GSRowEmitterTest, DestroyDropsWhatIsPending)
{
	auto rt = Target();
	Pair p0(rt.get(), 0, GROUP), p1(rt.get(), 1, GROUP);
	Draw({&p0.base, &p0.twin, &p1.base});
	dev->Destroy();
	EXPECT_TRUE(dev->drawn.empty());
	EXPECT_TRUE(dev->group.empty());
	EXPECT_TRUE(dev->held.empty());
}

// The streak rule: when a run-program group opens.

namespace
{
	constexpr u32 BASES = 0, TWINS = 1;

	/// How many of `n` consecutive rows of class `cls` on `side` find a group open for them.
	u32 Opened(GSRunsPolicy::Streaks& s, u32 side, u64 cls, u32 n, u32 B)
	{
		u32 open = 0;
		for (u32 i = 0; i < n; i++)
			open += s.Note(side, cls, B);
		return open;
	}
} // namespace

TEST(GSRunsPolicyStreaks, AGroupOpensAtTheBreakEvenRowOfANewClass)
{
	GSRunsPolicy::Streaks s;
	EXPECT_FALSE(s.Note(BASES, 7, 4));
	EXPECT_FALSE(s.Note(BASES, 7, 4));
	EXPECT_FALSE(s.Note(BASES, 7, 4));
	EXPECT_TRUE(s.Note(BASES, 7, 4));
	EXPECT_TRUE(s.Note(BASES, 7, 4));
}

TEST(GSRunsPolicyStreaks, EachSideCountsItsOwnStreak)
{
	// Sly 3 arrives base, twin, base, twin: a twin between two bases does not break the bases' streak.
	GSRunsPolicy::Streaks s;
	u32 bases = 0, twins = 0;
	for (u32 i = 0; i < 6; i++)
	{
		bases += s.Note(BASES, 1, 4);
		twins += s.Note(TWINS, 2, 4);
	}
	EXPECT_EQ(bases, 3u);
	EXPECT_EQ(twins, 3u);
}

TEST(GSRunsPolicyStreaks, AnotherClassEndsTheStreak)
{
	GSRunsPolicy::Streaks s;
	EXPECT_EQ(Opened(s, BASES, 1, 3, 4), 0u);
	EXPECT_FALSE(s.Note(BASES, 2, 4));
	EXPECT_EQ(s.Last(1), 3u);
	EXPECT_EQ(Opened(s, BASES, 1, 3, 4), 0u); // its last streak (3) did not reach B: B rows again
}

TEST(GSRunsPolicyStreaks, AClassThatReachedBreakEvenOpensAtItsSecondRow)
{
	GSRunsPolicy::Streaks s;
	EXPECT_EQ(Opened(s, BASES, 1, 5, 4), 2u);
	s.Break(BASES);
	EXPECT_EQ(s.Last(1), 5u);
	EXPECT_FALSE(s.Note(BASES, 1, 4));
	EXPECT_TRUE(s.Note(BASES, 1, 4));
}

TEST(GSRunsPolicyStreaks, AClassDrawnClassicallyStillLearns)
{
	// The remembered length is the streak's, whether or not a group formed: a class whose streaks reach B only once
	// it is counted, not once it has grouped.
	GSRunsPolicy::Streaks s;
	EXPECT_EQ(Opened(s, TWINS, 9, 2, 4), 0u);
	s.Break(TWINS);
	EXPECT_EQ(s.Last(9), 2u);
	EXPECT_EQ(Opened(s, TWINS, 9, 4, 4), 1u);
	s.Break(TWINS);
	EXPECT_EQ(s.Last(9), 4u);
	EXPECT_EQ(Opened(s, TWINS, 9, 2, 4), 1u); // priming now
}

TEST(GSRunsPolicyStreaks, AfterASplitTheNextRowOfTheClassOpensAtOnce)
{
	GSRunsPolicy::Streaks s;
	EXPECT_EQ(Opened(s, BASES, 3, 4, 4), 1u);
	s.Split(BASES);
	EXPECT_TRUE(s.Note(BASES, 3, 4));
	s.Split(BASES);
	EXPECT_FALSE(s.Note(BASES, 4, 4)); // a split does not carry to another class
}

TEST(GSRunsPolicyStreaks, ASplitOnOneSideLeavesTheOther)
{
	GSRunsPolicy::Streaks s;
	EXPECT_EQ(Opened(s, TWINS, 5, 1, 4), 0u);
	s.Split(BASES);
	EXPECT_FALSE(s.Note(TWINS, 5, 4));
}

TEST(GSRunsPolicyStreaks, BreakEvenPricesCPUAndGPUApart)
{
	// The Adreno 650 (P4): opening a run-program group costs about 1.0 us of GPU, and each row in it saves about 1.1 us,
	// so on the GPU's account a group pays from its first row.
	EXPECT_EQ(GSRunsPolicy::BreakEven(0.0f, 0.0f, 1000.0f, 1100.0f), 1u);
	EXPECT_EQ(GSRunsPolicy::BreakEven(3000.0f, 1000.0f, 1000.0f, 1100.0f), 3u);
	EXPECT_EQ(GSRunsPolicy::BreakEven(3000.0f, 1000.0f, 5000.0f, 1100.0f), 5u);
	EXPECT_EQ(GSRunsPolicy::BreakEven(0.0f, 0.0f, 1000.0f, -100.0f), GSRunsPolicy::NEVER);
	EXPECT_EQ(GSRunsPolicy::BreakEven(0.0f, 0.0f, 0.0f, 0.0f), 1u);
	EXPECT_EQ(GSRunsPolicy::INTERIM_OPENING_ROWS, 4u);
}

TEST_F(GSRowEmitterTest, ATextureParkedForAGroupComesBackOnceTheGroupIsRecorded)
{
	// The group is recorded before a row that cannot join it, which leaves nothing deferred; the texture it sampled was
	// parked when the cache recycled it, and asking for a surface of its shape must return it to the pool first.
	auto rt = Target();
	GSTexture* tex = Texture().release(); // the pool owns it once recycled
	Row a(rt.get(), 1 | GROUP, 0, tex), b(rt.get(), 2);
	Draw({&a});
	dev->Recycle(tex);
	EXPECT_EQ(dev->Parked(), 1u);
	Draw({&b});
	EXPECT_EQ(dev->drawn, (std::vector<u32>{1 | GROUP, 2}));
	GSTexture* again = dev->CreateTexture(16, 16, 1, GSTexture::Format::Color, true);
	EXPECT_EQ(dev->Parked(), 0u);
	dev->Recycle(again);
}
