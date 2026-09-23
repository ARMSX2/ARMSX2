// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A GSRendererHW on the deviceless None backend, fed GIF packets through the renderer's own parser,
// with the last draw's GSHWDrawConfig kept for the test to read. No graphics API is touched: the
// texture cache hands out RAM-backed stub textures and every CPU-side pass runs for real.
//
// The same bring-up as gs_sprite_pass_order_tests.cpp, shared so a test that needs to see what the
// backend is handed does not have to copy it. Tests using it ride gs_vertex_tests, which links
// StubHost.cpp and the whole PCSX2 library.

#pragma once

#include <gtest/gtest.h>

#include "GS/GS.h"
#include "GS/GSState.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/HW/GSRendererHW.h"
#include "GS/Renderers/Null/GSDeviceNone.h"

#include <cstring>
#include <memory>
#include <vector>

namespace GSHWDrawHarness
{
	/// A GIF packet under construction: A+D register writes, then PACKED vertex records. The
	/// vertex records are {UV, XYZ2}; a primitive without a texture ignores the UV.
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

	/// The None backend with the last submitted draw kept.
	class CaptureDevice final : public GSDeviceNone
	{
	public:
		void DoRenderHW(GSHWDrawConfig& config) override
		{
			m_draws++;
			m_verts.assign(config.verts, config.verts + config.nverts);
			m_cb_ps = config.cb_ps;
			m_ps = config.ps;
			m_topology = config.topology;
		}

		// What the device claims to be. The back thread only engages behind a Vulkan device, so a
		// test that needs the pipelined front parser has this say Vulkan.
		RenderAPI GetRenderAPI() const override { return m_api; }

		RenderAPI m_api = RenderAPI::None;
		u32 m_draws = 0;
		std::vector<GSVertex> m_verts;
		GSHWDrawConfig::PSConstantBuffer m_cb_ps;
		GSHWDrawConfig::PSSelector m_ps;
		GSHWDrawConfig::Topology m_topology = GSHWDrawConfig::Topology::Triangle;
	};

	class Renderer final : public GSRendererHW
	{
	};

	class Fixture : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_saved_config = GSConfig;

			// The renderer reads GSConfig in its constructor, so a test that wants a different value
			// sets it before calling BringUp().
			GSConfig.Renderer = GSRendererType::VK;
			GSConfig.UpscaleMultiplier = 2.0f;
			GSConfig.HWDownloadMode = GSHardwareDownloadMode::Disabled;
			GSConfig.BackThreadMode = GSBackThreadMode::Off;
			GSConfig.CoalesceRenderPasses = false; // so DoRenderHW runs inside the draw, not later
			GSConfig.UserHacks_MergePPSprite = false;
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
			device->m_api = m_device_api;
			m_device = device.get();
			g_gs_device = std::move(device);
			ASSERT_TRUE(g_gs_device->Create(GSVSyncMode::Disabled, false));

			m_priv_regs = std::make_unique<GSPrivRegSet>();
			std::memset(m_priv_regs.get(), 0, sizeof(GSPrivRegSet));

			auto renderer = std::make_unique<Renderer>();
			m_gs = renderer.get();
			g_gs_renderer = std::move(renderer);

			// The texture cache reaches the renderer through the g_gs_renderer global rather than
			// through the object it is handed, so this one has to BE the installed renderer.
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

		/// A 640x448 frame at `fbp` with no depth test, no alpha test and no blending, and one
		/// vertex colour for the batch.
		static void Environment(Packet& p, u32 fbp, u32 frame_psm)
		{
			GIFReg r = {};

			r.U64 = 0;
			p.Reg(GIF_A_D_REG_XYOFFSET_1, r);

			r.U64 = 0;
			r.SCISSOR.SCAX1 = 639;
			r.SCISSOR.SCAY1 = 447;
			p.Reg(GIF_A_D_REG_SCISSOR_1, r);

			r.U64 = 0;
			r.FRAME.FBP = fbp;
			r.FRAME.FBW = 10;
			r.FRAME.PSM = frame_psm;
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

			r.U64 = 0;
			r.RGBAQ.R = 0x40;
			r.RGBAQ.G = 0x60;
			r.RGBAQ.B = 0x20;
			r.RGBAQ.A = 0x80;
			r.RGBAQ.Q = 1.0f;
			p.Reg(GIF_A_D_REG_RGBAQ, r);
		}

		/// Nearest-neighbour sampling of a 512x512 texture at block `tbp`.
		static void Texture(Packet& p, u32 tbp, u32 tex_psm)
		{
			GIFReg r = {};

			r.U64 = 0;
			r.TEX0.TBP0 = tbp;
			r.TEX0.TBW = 8;
			r.TEX0.PSM = tex_psm;
			r.TEX0.TW = 9;
			r.TEX0.TH = 9;
			r.TEX0.TCC = 1;
			r.TEX0.TFX = TFX_DECAL;
			p.Reg(GIF_A_D_REG_TEX0_1, r);

			r.U64 = 0;
			p.Reg(GIF_A_D_REG_TEX1_1, r);

			r.U64 = 0;
			r.CLAMP.WMS = CLAMP_CLAMP;
			r.CLAMP.WMT = CLAMP_CLAMP;
			p.Reg(GIF_A_D_REG_CLAMP_1, r);
		}

		std::unique_ptr<GSPrivRegSet> m_priv_regs;
		Pcsx2Config::GSOptions m_saved_config;
		RenderAPI m_device_api = RenderAPI::None;
		Renderer* m_gs = nullptr;
		CaptureDevice* m_device = nullptr;
	};
} // namespace GSHWDrawHarness
