// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins WHERE the sprite coverage correction runs in the hardware draw path, by driving a real
// GSRendererHW draw and reading back the geometry the backend is handed.
//
// The correction -- AlignSpriteX, the pixel-grid snap and RoundSprite, together
// GSRendererHW::CorrectSpriteCoverageForUpscale -- pushes a sprite's far edge outwards by up to
// half a pixel so an upscaled sprite keeps the device column the GS gives it at native. Two passes
// that used to run after it rebuild the whole vertex buffer out of m_vt.m_min / m_vt.m_max:
// MergeSprite (the paving merge, GameDB mergeSprite) and ConvertSpriteTextureShuffleImpl. Those
// bounds are taken once, by m_vt.Update before any correction runs, and nothing refreshes them --
// so on a draw either rebuild touched, every correction was silently thrown away. Ace Combat 5 is
// the visible case and the batch below is its batch.
//
// gs_sprite_edge_snap_tests.cpp pins the arithmetic of each rule, but it calls the rules directly
// and re-implements the batch walk locally, so it stays green whatever order the renderer runs the
// passes in. That is the gap this file closes. Nothing here re-implements anything: the fixture
// builds GIF packets, hands them to the renderer's own parser, and asserts on m_conf.verts exactly
// as GSDevice::DoRenderHW receives it. Moving the correction back above MergeSprite turns the
// first case red -- 8184 (x = 511.5) instead of 8192 (x = 512.0) on the merged far edge.
//
// Bring-up: GSRendererHW with the deviceless None backend. No graphics API is touched; the texture
// cache hands out RAM-backed stub textures and every CPU-side pass runs for real. Rides
// gs_vertex_tests, which already links StubHost.cpp and the whole PCSX2 library.

#include <gtest/gtest.h>

#include "GS/GS.h"
#include "GS/GSState.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/Null/GSDeviceNone.h"

#include <cstring>
#include <memory>
#include <vector>

namespace
{
	// ------------------------------------------------------------------ packets ---

	// A GIF packet under construction: A+D register writes followed by PACKED {UV, XYZ2} vertex
	// records, which is how a game hands a textured FST sprite batch to the GIF. Going through the
	// parser rather than writing m_env and the vertex buffer by hand keeps the register side
	// effects (ApplyTEX0, the context update) and the vertex kick the renderer's own, so m_vt is
	// computed by the code under test rather than by this file.
	class Packet
	{
	public:
		void Reg(u8 addr, const GIFReg& r)
		{
			GIFPackedReg packed = {};
			packed.A_D.DATA = r.U64;
			packed.A_D.ADDR = addr;
			m_regs.push_back(packed);
		}

		// Positions and UVs are raw 1/16 units, which is how the vertex buffer holds them.
		void Vertex(int x, int y, u32 z, int u, int v)
		{
			GIFPackedReg uv = {};
			uv.U32[0] = static_cast<u32>(u);
			uv.U32[1] = static_cast<u32>(v);
			m_verts.push_back(uv);

			GIFPackedReg xyz = {};
			xyz.U32[0] = static_cast<u32>(x);
			xyz.U32[1] = static_cast<u32>(y);
			xyz.U32[2] = z;
			m_verts.push_back(xyz);
		}

		void Send(GSState& gs, const GIFRegPRIM& prim)
		{
			std::vector<GIFPackedReg> buf;

			if (!m_regs.empty())
			{
				GIFTag tag = {};
				tag.NLOOP = static_cast<u32>(m_regs.size());
				tag.EOP = m_verts.empty() ? 1 : 0;
				tag.FLG = GIF_FLG_PACKED;
				tag.NREG = 1;
				tag.REGS = GIF_REG_A_D;
				buf.push_back(AsPackedReg(tag));
				buf.insert(buf.end(), m_regs.begin(), m_regs.end());
			}

			if (!m_verts.empty())
			{
				GIFTag tag = {};
				tag.NLOOP = static_cast<u32>(m_verts.size() / 2);
				tag.EOP = 1;
				tag.PRE = 1;
				tag.PRIM = static_cast<u32>(prim.U64 & 0x7FF);
				tag.FLG = GIF_FLG_PACKED;
				tag.NREG = 2;
				tag.REGS = static_cast<u64>(GIF_REG_UV) | (static_cast<u64>(GIF_REG_XYZ2) << 4);
				buf.push_back(AsPackedReg(tag));
				buf.insert(buf.end(), m_verts.begin(), m_verts.end());
			}

			gs.Transfer<0>(reinterpret_cast<const u8*>(buf.data()), static_cast<u32>(buf.size()));
			gs.FlushPrim();
		}

	private:
		static GIFPackedReg AsPackedReg(const GIFTag& tag)
		{
			GIFPackedReg r = {};
			std::memcpy(&r, &tag, sizeof(r));
			return r;
		}

		std::vector<GIFPackedReg> m_regs;
		std::vector<GIFPackedReg> m_verts;
	};

	// ------------------------------------------------------------------- device ---

	// The None backend with the submitted draw kept. DoRenderHW is the last thing DrawPrims does,
	// so whatever arrives here has been through every geometry pass: a real backend would be
	// uploading exactly these vertices.
	class CaptureDevice final : public GSDeviceNone
	{
	public:
		void DoRenderHW(GSHWDrawConfig& config) override
		{
			m_draws++;
			m_verts.assign(config.verts, config.verts + config.nverts);
		}

		u32 m_draws = 0;
		std::vector<GSVertex> m_verts;
	};

	class DrawProbe final : public GSRendererHW
	{
	};

	// ------------------------------------------------------------------ fixture ---

	// FRAME.FBP counts 2048-word pages, TEX0.TBP0 counts 64-word blocks; the factor of 32 between
	// them is why the texture below names kSrcFBP * 32.
	constexpr u32 kSrcFBP = 0x00;
	constexpr u32 kDstFBP = 0x40;

	class GSSpritePassOrder : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_saved_config = GSConfig;

			// Everything in the draw path these cases care about, pinned. The renderer reads
			// GSConfig in its constructor, so a test that wants a different value sets it before
			// calling BringUp().
			GSConfig.Renderer = GSRendererType::VK;
			GSConfig.UpscaleMultiplier = 2.0f;
			GSConfig.HWDownloadMode = GSHardwareDownloadMode::Disabled;
			GSConfig.BackThreadModeResolved = GSBackThreadMode::Off;
			GSConfig.CoalesceRenderPasses = false; // so DoRenderHW runs inside the draw, not later
			GSConfig.UserHacks_MergePPSprite = true;
			GSConfig.UserHacks_AlignSpriteX = false;
			GSConfig.UserHacks_RoundSprite = 0;
			GSConfig.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
			GSConfig.UserHacks_AutoFlush = GSHWAutoFlushLevel::Disabled;
			GSConfig.UserHacks_ForceEvenSpritePosition = false;
			GSConfig.UserHacks_DrawBuffering = false;
			GSConfig.Dithering = 0;
			GSConfig.HWMipmap = false;
		}

		void BringUp()
		{
			auto device = std::make_unique<CaptureDevice>();
			m_device = device.get();
			g_gs_device = std::move(device);
			ASSERT_TRUE(g_gs_device->Create(GSVSyncMode::Disabled, false));

			m_priv_regs = std::make_unique<GSPrivRegSet>();
			std::memset(m_priv_regs.get(), 0, sizeof(GSPrivRegSet));

			auto probe = std::make_unique<DrawProbe>();
			m_gs = probe.get();
			g_gs_renderer = std::move(probe);

			// The texture cache reaches the renderer through the g_gs_renderer global rather than
			// through the object it is handed, so the probe has to BE the installed renderer.
			ASSERT_EQ(GSRendererHW::GetInstance(), m_gs);

			g_gs_renderer->SetRegsMem(reinterpret_cast<u8*>(m_priv_regs.get()));
			g_gs_renderer->ResetPCRTC();
		}

		void TearDown() override
		{
			m_gs = nullptr;
			m_device = nullptr;
			if (g_gs_renderer)
			{
				g_gs_renderer->Destroy();
				g_gs_renderer.reset();
			}
			if (g_gs_device)
			{
				g_gs_device->Destroy();
				g_gs_device.reset();
			}
			GSConfig = m_saved_config;
		}

		// The drawing environment every draw below shares. No alpha test, no depth test, no
		// blending: each of those can take a draw out of the submission path for reasons that have
		// nothing to do with pass order.
		static void Environment(Packet& p, u32 fbp, u32 frame_psm, u32 fbmsk = 0)
		{
			GIFReg r = {};

			r.U64 = 0;
			r.XYOFFSET.OFX = 0;
			r.XYOFFSET.OFY = 0;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);

			r.U64 = 0;
			r.SCISSOR.SCAX0 = 0;
			r.SCISSOR.SCAY0 = 0;
			r.SCISSOR.SCAX1 = 639;
			r.SCISSOR.SCAY1 = 447;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);

			r.U64 = 0;
			r.FRAME.FBP = fbp;
			r.FRAME.FBW = 8;
			r.FRAME.PSM = frame_psm;
			r.FRAME.FBMSK = fbmsk;
			p.Reg(GIF_A_D_REG_FRAME_1, r);

			r.U64 = 0;
			r.ZBUF.ZBP = 0x100;
			r.ZBUF.PSM = PSMZ32;
			r.ZBUF.ZMSK = 1;
			p.Reg(GIF_A_D_REG_ZBUF_1, r);

			r.U64 = 0;
			r.TEST.ZTE = 1;
			r.TEST.ZTST = ZTST_ALWAYS;
			p.Reg(GIF_A_D_REG_TEST_1, r);

			r.U64 = 0;
			r.PRMODECONT.AC = 1;
			p.Reg(GIF_A_D_REG_PRMODECONT, r);

			r.U64 = 0;
			r.COLCLAMP.CLAMP = 1;
			p.Reg(GIF_A_D_REG_COLCLAMP, r);

			// One colour for the whole batch. MergeSprite's paving test wants RGBA, Z and FOG
			// equal across the batch -- (m_vt.m_eq.value & 0xCFFFF) == 0xCFFFF.
			r.U64 = 0;
			r.RGBAQ.R = 0x40;
			r.RGBAQ.G = 0x60;
			r.RGBAQ.B = 0x20;
			r.RGBAQ.A = 0x80;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);
		}

		// Nearest-neighbour sampling of whatever lives at kSrcFBP.
		static void SourceTexture(Packet& p, u32 tex_psm)
		{
			GIFReg r = {};

			r.U64 = 0;
			r.TEX0.TBP0 = kSrcFBP * 32;
			r.TEX0.TBW = 8;
			r.TEX0.PSM = tex_psm;
			r.TEX0.TW = 9;
			r.TEX0.TH = 9;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
			p.Reg(GIF_A_D_REG_TEX0_1, r);

			r.U64 = 0;
			r.TEX1.MMAG = 0;
			r.TEX1.MMIN = 0;
			p.Reg(GIF_A_D_REG_TEX1_1, r);

			r.U64 = 0;
			r.CLAMP.WMS = CLAMP_CLAMP;
			r.CLAMP.WMT = CLAMP_CLAMP;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);
		}

		// One untextured sprite over the whole source rectangle. Its only job is to leave an
		// upscaled render target in the texture cache at kSrcFBP, so the draw that follows samples
		// a target: MergeSprite refuses a source that is not one, and a texture shuffle needs one
		// too.
		void SeedSourceTarget(u32 psm)
		{
			Packet p;
			Environment(p, kSrcFBP, psm);
			p.Vertex(0, 0, 1, 0, 0);
			p.Vertex(512 * 16, 448 * 16, 1, 0, 0);

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			p.Send(*m_gs, prim);
		}

		// Ace Combat 5's shape. A 448-line frame is squeezed into a 224-line display buffer by
		// eight full-width strips, 28 lines each, whose far X sits half a pixel short of the right
		// edge at native x = 511.5. Every strip is the same width with the same U span, so
		// MergeSprite's paving test passes on both axes and the batch collapses to one sprite
		// spanning the vertex trace's bounds.
		void DrawAceCombatStrips()
		{
			Packet p;
			Environment(p, kDstFBP, PSMCT32);
			SourceTexture(p, PSMCT32);

			for (int k = 0; k < 8; k++)
			{
				p.Vertex(0, 448 * k, 1, 0, 896 * k);
				p.Vertex(kShortFarX, 448 * (k + 1), 1, kShortFarX, 896 * (k + 1));
			}

			GIFRegPRIM prim = {};
			prim.PRIM = GS_SPRITE;
			prim.TME = 1;
			prim.FST = 1;
			p.Send(*m_gs, prim);
		}

		// The sprites the backend was handed, as whole quads. The None backend declines vertex
		// shader expansion, so SetupIA runs Lines2Sprites and every sprite arrives as four
		// vertices: index 0 is the sprite's near corner and index 3 its far corner (the other two
		// are the mixed corners).
		u32 SubmittedSprites() const { return static_cast<u32>(m_device->m_verts.size()) / 4; }
		const GSVertex& NearCorner(u32 sprite) const { return m_device->m_verts[sprite * 4]; }
		const GSVertex& FarCorner(u32 sprite) const { return m_device->m_verts[sprite * 4 + 3]; }

		// x = 511.5 and x = 512.0 in the vertex buffer's 1/16 units.
		static constexpr int kShortFarX = 511 * 16 + 8;
		static constexpr int kWholeFarX = 512 * 16;

		std::unique_ptr<GSPrivRegSet> m_priv_regs;
		Pcsx2Config::GSOptions m_saved_config;
		DrawProbe* m_gs = nullptr;
		CaptureDevice* m_device = nullptr;
	};
} // namespace

// The case the ordering exists for. Run the correction before MergeSprite and the merge rebuilds
// the batch from bounds taken before the snap, putting 511.5 back; at 2x that loses the rightmost
// device column of the frame, which is what was black on both Ace Combat 5 captures.
TEST_F(GSSpritePassOrder, TheMergedSpriteKeepsTheSnappedFarEdge)
{
	BringUp();
	SeedSourceTarget(PSMCT32);

	const u32 before = m_device->m_draws;
	DrawAceCombatStrips();

	ASSERT_EQ(m_device->m_draws, before + 1) << "the strip draw never reached the backend";
	ASSERT_EQ(SubmittedSprites(), 1u) << "MergeSprite did not collapse the batch";

	EXPECT_EQ(FarCorner(0).XYZ.X, kWholeFarX);
	EXPECT_EQ(FarCorner(0).U, kWholeFarX) << "the snap slides UV with the position on an FST sprite";
	EXPECT_EQ(NearCorner(0).XYZ.X, 0);
	EXPECT_EQ(FarCorner(0).XYZ.Y, 224 * 16) << "the merge should span the full strip height";
}

// The control. With mergeSprite off nothing rebuilds the batch, so the same eight strips reach the
// backend one by one, every one of them snapped. This is the answer the merged draw above has to
// agree with, and it is what the correction did all along on draws neither rebuild touched.
TEST_F(GSSpritePassOrder, AnUnmergedBatchGetsTheSameFarEdge)
{
	GSConfig.UserHacks_MergePPSprite = false;
	BringUp();
	SeedSourceTarget(PSMCT32);

	const u32 before = m_device->m_draws;
	DrawAceCombatStrips();

	ASSERT_EQ(m_device->m_draws, before + 1);
	ASSERT_EQ(SubmittedSprites(), 8u);

	for (u32 k = 0; k < 8; k++)
	{
		EXPECT_EQ(FarCorner(k).XYZ.X, kWholeFarX) << "strip " << k;
		EXPECT_EQ(FarCorner(k).U, kWholeFarX) << "strip " << k;
	}
}

// Native resolution is untouched by the whole block, which is what makes every 1x byte-identity
// gate in this programme hold trivially. CanUpscale() is false at 1x, so neither the merge nor the
// correction runs and the game's own 511.5 reaches the backend.
TEST_F(GSSpritePassOrder, NativeResolutionLeavesTheFarEdgeAlone)
{
	GSConfig.UpscaleMultiplier = 1.0f;
	BringUp();
	SeedSourceTarget(PSMCT32);

	const u32 before = m_device->m_draws;
	DrawAceCombatStrips();

	ASSERT_EQ(m_device->m_draws, before + 1);
	ASSERT_EQ(SubmittedSprites(), 8u) << "MergeSprite must not run at native resolution";

	for (u32 k = 0; k < 8; k++)
		EXPECT_EQ(FarCorner(k).XYZ.X, kShortFarX) << "strip " << k;
}

// The other rebuild. A texture shuffle replaces the batch with one quad on whole native pixels,
// built from integer rectangles, so the snap and AlignSpriteX find nothing to move there -- but
// RoundSprite is not a no-op on whole-pixel geometry, and it would rewrite the shuffle quad's UV.
// The correction is gated off shuffle draws for exactly that reason, and this pins it. Two 8x8
// quads whose position and texcoord move on opposite axes are a 32-bit-source swizzle shuffle, and
// the quad that reaches the backend carries the rebuild's own coordinates. Drop the
// !m_texture_shuffle clause from the gate and the far V comes out 208 instead of 256 --
// RoundSprite resampling a rebuild that was already exact.
TEST_F(GSSpritePassOrder, ATextureShuffleReachesTheBackendUnrounded)
{
	GSConfig.UserHacks_RoundSprite = 2; // on for every sprite, linear filtering or not
	BringUp();
	SeedSourceTarget(PSMCT32);

	const u32 before = m_device->m_draws;
	{
		Packet p;
		Environment(p, kDstFBP, PSMCT16, 0xFFFF);
		SourceTexture(p, PSMCT32);
		p.Vertex(0, 0, 1, 0, 0);
		p.Vertex(8 * 16, 8 * 16, 1, 8 * 16, 8 * 16);
		p.Vertex(8 * 16, 0, 1, 0, 8 * 16);
		p.Vertex(16 * 16, 8 * 16, 1, 8 * 16, 16 * 16);

		GIFRegPRIM prim = {};
		prim.PRIM = GS_SPRITE;
		prim.TME = 1;
		prim.FST = 1;
		p.Send(*m_gs, prim);
	}

	ASSERT_EQ(m_device->m_draws, before + 1) << "the shuffle draw never reached the backend";
	ASSERT_EQ(SubmittedSprites(), 1u) << "the shuffle rebuild should leave one quad";

	// Whole native pixels and whole texels on every corner: the rebuild's signature.
	for (const GSVertex& v : m_device->m_verts)
	{
		EXPECT_EQ(v.XYZ.X % 16, 0);
		EXPECT_EQ(v.XYZ.Y % 16, 0);
		EXPECT_EQ(v.U % 16, 0);
		EXPECT_EQ(v.V % 16, 0);
	}

	EXPECT_EQ(NearCorner(0).XYZ.X, 0);
	EXPECT_EQ(NearCorner(0).XYZ.Y, 0);
	EXPECT_EQ(FarCorner(0).XYZ.X, 16 * 16);
	EXPECT_EQ(FarCorner(0).XYZ.Y, 4 * 16);
	EXPECT_EQ(NearCorner(0).U, 0);
	EXPECT_EQ(NearCorner(0).V, 0);
	EXPECT_EQ(FarCorner(0).U, 16 * 16);
	EXPECT_EQ(FarCorner(0).V, 16 * 16) << "RoundSprite must not run on a texture shuffle";
}
