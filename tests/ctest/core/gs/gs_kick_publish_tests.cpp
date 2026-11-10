// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The kick handlers a PRIM write publishes.
//
// The packed and A+D handler tables hold, for the XYZ registers and the fused vertex layouts, the
// handlers of the live primitive type. ApplyPRIM publishes them, and republishing the same type is
// a no-op, so it is skipped. These cases pin what must still republish: a handler reset (a settings
// change re-arms the tables and leaves the XYZ entries empty) and a type change and back, with the
// vertices of every packet reaching the draw.
//
// Rides gs_vertex_tests.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "GS/GS.h"
#include "GS/GSState.h"

namespace
{
	class PublishProbe final : public GSState
	{
	public:
		PublishProbe()
			: m_regs_storage(std::make_unique<GSPrivRegSet>())
		{
			std::memset(m_regs_storage.get(), 0, sizeof(GSPrivRegSet));
			m_regs = m_regs_storage.get();

			GSDrawingContext& ctx = m_env.CTXT[0];
			ctx.FRAME.FBP = 0;
			ctx.FRAME.FBW = 8;
			ctx.FRAME.PSM = PSMCT32;
			ctx.ZBUF.ZBP = 0x100;
			ctx.ZBUF.PSM = PSMZ24;
			ctx.ZBUF.ZMSK = 1;
			ctx.SCISSOR.SCAX0 = 0;
			ctx.SCISSOR.SCAY0 = 0;
			ctx.SCISSOR.SCAX1 = 511;
			ctx.SCISSOR.SCAY1 = 415;
			ctx.UpdateScissor();
			m_env.PRIM.CTXT = 0;
			m_nativeres = true;
			UpdateContext();
			ResetHandlers();
		}

		void Draw() override { m_indices += m_index->tail; }
		bool IsCoverageAlphaSupported() override { return false; }

		// One PACKED tag with PRIM in it: NLOOP vertices of {RGBAQ, FOG, XYZ2}, a strip of small triangles. No
		// fused handler takes that layout, so every XYZ2 goes through the per-register table entry PRIM publishes.
		void Feed(u32 prim, u32 vertices)
		{
			std::vector<GIFPackedReg> packet(1 + vertices * 3);
			std::memset(packet.data(), 0, packet.size() * sizeof(GIFPackedReg));
			GIFTag t = {};
			t.NLOOP = vertices;
			t.NREG = 3;
			t.FLG = GIF_FLG_PACKED;
			t.PRE = 1;
			t.PRIM = prim;
			t.REGS = (u64(GIF_REG_RGBA) << 0) | (u64(GIF_REG_FOG) << 4) | (u64(GIF_REG_XYZ2) << 8);
			std::memcpy(&packet[0], &t, sizeof(GIFTag));
			for (u32 i = 0; i < vertices; i++)
			{
				GIFPackedReg* r = &packet[1 + i * 3];
				r[0].U32[0] = r[0].U32[1] = r[0].U32[2] = r[0].U32[3] = 128;
				r[2].U32[0] = (16 + (i / 2) * 20) << 4; // x, 12.4
				r[2].U32[1] = (16 + (i & 1) * 20) << 4; // y
			}
			Transfer<0>(reinterpret_cast<const u8*>(packet.data()), static_cast<u32>(packet.size()));
			Flush(GSFlushReason::CONTEXTCHANGE);
		}

		std::unique_ptr<GSPrivRegSet> m_regs_storage;
		u32 m_indices = 0;
	};

	// A strip of six vertices is four triangles, twelve indices; a list of six is two, six indices.
	TEST(GsKickPublish, SamePrimAfterHandlerResetStillKicks)
	{
		PublishProbe p;
		p.Feed(GS_TRIANGLESTRIP, 6);
		EXPECT_EQ(p.m_indices, 12u);

		// A settings change re-arms the tables; the next tag carries the same primitive type.
		p.m_indices = 0;
		p.ResetHandlers();
		p.Feed(GS_TRIANGLESTRIP, 6);
		EXPECT_EQ(p.m_indices, 12u);
	}

	TEST(GsKickPublish, TypeChangeAndBackKicksEachType)
	{
		PublishProbe p;
		const u32 expect[] = {12, 6, 12, 12, 6, 6};
		const u32 prims[] = {GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_TRIANGLESTRIP, GS_TRIANGLESTRIP, GS_TRIANGLELIST, GS_TRIANGLELIST};
		for (size_t i = 0; i < std::size(prims); i++)
		{
			p.m_indices = 0;
			p.Feed(prims[i], 6);
			EXPECT_EQ(p.m_indices, expect[i]) << "packet " << i;
		}
	}
} // namespace
