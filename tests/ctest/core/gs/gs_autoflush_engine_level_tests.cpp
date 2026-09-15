// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The auto-flush split is decided by the draw engine's level, not the hardware key.
//
// A self-texturing draw reads the frame buffer it is writing. The console's GS buffers
// one page of the texture, so a primitive reads what the primitives before it wrote, and
// the auto-flush split is how both renderers reproduce that: the batch is flushed when an
// incoming primitive textures from the area the batch has already drawn, and the next
// primitive re-reads the source.
//
// Which draws get split is a property of the engine consuming them. GSRendererSW::
// GetAutoFlushLevel answers Enabled or Disabled from the SW key; a hardware renderer
// answers the hardware key, which has a third value, SpritesOnly, narrowing the split to
// sprites. ResetHandlers already arms the parse handlers from that virtual. IsAutoFlushDraw
// did not: it read the hardware key directly, so a title whose GameDB entry asks for
// SpritesOnly refused the split for every non-sprite primitive on the SOFTWARE renderer as
// well, where SpritesOnly is not a level that engine has.
//
// Jak 3's shadow volume is that draw. Its GameDB entry sets autoFlush to SpritesOnly, and
// frame 0 draw 478 is one 5,001-vertex TRIANGLEFAN texturing from the frame buffer's own
// pages into alpha, counting shadow faces by modulating what the face before it wrote.
// The console (SCPH-30001, three runs byte-identical) leaves alpha holding 96/95/94/93/92
// after it; a renderer that never splits can only produce 96/95/97, because every face
// read the same source. The handlers were armed for the split and the predicate refused
// all 5,973 calls.
//
// The cases below are that seam in miniature, driven through the real GIF parse road:
// the same context, the same packed layout, the vertices in a real packet through
// Transfer, and the draw count out.
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
	// One PACKED {STQ, RGBAQ, XYZ2} record, the layout Jak 3's shadow volume arrives in.
	struct Vertex
	{
		int x, y;      // pixels
		float s, t;    // texture coordinate, Q is always 1
		u32 alpha;
		bool adc = false;
	};

	constexpr u32 kTexShift = 9; // TW / TH: a 512x512 window over the frame buffer

	void EncodeVertex(GIFPackedReg* r, const Vertex& v)
	{
		std::memset(r, 0, sizeof(GIFPackedReg) * 3);

		const float q = 1.0f;
		std::memcpy(&r[0].U32[0], &v.s, sizeof(float));
		std::memcpy(&r[0].U32[1], &v.t, sizeof(float));
		std::memcpy(&r[0].U32[2], &q, sizeof(float));

		r[1].U32[0] = 128;
		r[1].U32[1] = 128;
		r[1].U32[2] = 128;
		r[1].U32[3] = v.alpha;

		r[2].U32[0] = static_cast<u32>(v.x) << 4; // 12.4
		r[2].U32[1] = static_cast<u32>(v.y) << 4;
		r[2].U32[2] = 0;
		if (v.adc)
			r[2].U32[3] |= 0x8000;
	}

	// The tag the records arrive under: NREG 3, {STQ, RGBAQ, XYZ2}, PACKED.
	GIFTag MakeTag(u32 vertices)
	{
		GIFTag t = {};
		t.NLOOP = vertices;
		t.NREG = 3;
		t.FLG = GIF_FLG_PACKED;
		t.REGS = (u64(GIF_REG_STQ) << 0) | (u64(GIF_REG_RGBA) << 4) | (u64(GIF_REG_XYZ2) << 8);
		return t;
	}

	// GSConfig is a global; every test restores what it found.
	class HardwareKeyGuard
	{
	public:
		explicit HardwareKeyGuard(GSHWAutoFlushLevel level)
			: m_saved(GSConfig.UserHacks_AutoFlush)
		{
			GSConfig.UserHacks_AutoFlush = level;
		}
		~HardwareKeyGuard() { GSConfig.UserHacks_AutoFlush = m_saved; }

	private:
		GSHWAutoFlushLevel m_saved;
	};

	class AutoFlushProbe final : public GSState
	{
	public:
		AutoFlushProbe()
			: m_regs_storage(std::make_unique<GSPrivRegSet>())
		{
			// A flush walks the privileged registers looking for the display buffer;
			// nothing constructs them for a bare GSState.
			std::memset(m_regs_storage.get(), 0, sizeof(GSPrivRegSet));
			m_regs = m_regs_storage.get();
		}

		// Counting draws is the whole measurement: one draw means the batch was never
		// split, so every primitive read the same source.
		void Draw() override
		{
			m_draws++;
			m_indices.push_back(m_index->tail);
		}

		// Stands in for GSRendererSW's override -- the engine's rule, whatever the
		// process renderer type is.
		GSHWAutoFlushLevel GetAutoFlushLevel() const override { return m_engine_level; }

		// The base asserts "not implemented"; the kick reaches it for AA1 prims.
		bool IsCoverageAlphaSupported() override { return true; }

		GSHWAutoFlushLevel m_engine_level = GSHWAutoFlushLevel::Enabled;
		u32 m_draws = 0;
		std::vector<u32> m_indices;

		// Jak 3 draw 478's context, at its own size: a 512x416 frame buffer at PSMCT32
		// textured from its own pages, TCC on so alpha is live, MODULATE, alpha-only
		// writes, depth tested and never written.
		void Configure(u32 prim)
		{
			GSDrawingContext& ctx = m_env.CTXT[0];

			ctx.FRAME.FBP = 0;
			ctx.FRAME.FBW = 8;
			ctx.FRAME.PSM = PSMCT32;
			ctx.FRAME.FBMSK = 0x00FFFFFF; // alpha only, as the shadow counter writes
			ctx.ZBUF.ZBP = 0x100;
			ctx.ZBUF.PSM = PSMZ24;
			ctx.ZBUF.ZMSK = 1;
			ctx.TEX0.TBP0 = 0; // the frame buffer's own pages
			ctx.TEX0.TBW = 8;
			ctx.TEX0.PSM = PSMCT32;
			ctx.TEX0.TW = kTexShift;
			ctx.TEX0.TH = kTexShift;
			ctx.TEX0.TCC = 1;
			ctx.TEX0.TFX = TFX_MODULATE;
			ctx.TEX1.MXL = 0;
			ctx.TEX1.MMIN = 0;
			ctx.TEX1.LCM = 0;
			ctx.TEST.ATE = 0;
			ctx.CLAMP.WMS = CLAMP_REPEAT;
			ctx.CLAMP.WMT = CLAMP_REPEAT;
			ctx.SCISSOR.SCAX0 = 0;
			ctx.SCISSOR.SCAY0 = 0;
			ctx.SCISSOR.SCAX1 = 511;
			ctx.SCISSOR.SCAY1 = 415;
			ctx.XYOFFSET.OFX = 0;
			ctx.XYOFFSET.OFY = 0;
			ctx.UpdateScissor();

			m_env.PRIM.CTXT = 0;
			m_env.PRIM.PRIM = prim;
			m_env.PRIM.IIP = 1;
			m_env.PRIM.TME = 1;
			m_env.PRIM.FST = 0; // the STQ road, as the volume arrives
			m_env.PRIM.AA1 = 0;

			m_nativeres = true;
			temp_draw_rect = GSVector4i::zero();
			UpdateContext();

			// The handlers are armed from the engine's level, so the level has to be
			// set before this runs -- which is the ordering GSRendererSW's constructor
			// exists to get right.
			ResetHandlers();
			UpdateVertexKick();
		}

		// One GIF packet, tag and all, through the road a title's vertices take.
		void Feed(const std::vector<Vertex>& verts)
		{
			std::vector<GIFPackedReg> packet(1 + verts.size() * 3);
			const GIFTag tag = MakeTag(static_cast<u32>(verts.size()));
			std::memcpy(&packet[0], &tag, sizeof(GIFTag));
			for (size_t i = 0; i < verts.size(); i++)
				EncodeVertex(&packet[1 + i * 3], verts[i]);

			Transfer<0>(reinterpret_cast<const u8*>(packet.data()), static_cast<u32>(packet.size()));
		}

		// Draws the batch that is still open, the way a state change would.
		void FinishDraw() { Flush(GSFlushReason::CONTEXTCHANGE); }

		std::unique_ptr<GSPrivRegSet> m_regs_storage;
	};

	Vertex At(int x, int y, u32 alpha)
	{
		// UV tracks XY one-to-one, which is what makes the draw read the pixel under it:
		// with Q = 1 and TW = 9, U = 512 * S.
		return Vertex{x, y, static_cast<float>(x) / 512.0f, static_cast<float>(y) / 512.0f, alpha, false};
	}

	// Jak 3's shadow volume in miniature: a fan whose rim walks the frame in wide steps,
	// so each triangle covers a large part of it and later triangles draw over earlier
	// ones. A closed volume seen from the front does the same thing -- the far faces land
	// on the same pixels as the near ones, which is the whole reason the count works.
	//
	// A rim that walks round in small steps would NOT do: consecutive triangles of such a
	// fan only meet along their shared edge, and the auto-flush test compares rectangles,
	// which for two such triangles intersect in a zero-width strip.
	std::vector<Vertex> OverlappingFan()
	{
		std::vector<Vertex> v;
		v.push_back(At(256, 208, 130)); // the apex, shared by every triangle
		const int rim[6][2] = {
			{496, 208}, {62, 323}, {330, 23}, {330, 393}, {62, 93}, {496, 208}};
		for (const auto& p : rim)
			v.push_back(At(p[0], p[1], (v.size() & 1) ? 130 : 127));
		return v;
	}

	// The scope guard's geometry: a triangle list of small triangles laid out along a row,
	// none of which touches another and none of which textures from what the batch drew.
	std::vector<Vertex> DisjointTriangles()
	{
		std::vector<Vertex> v;
		for (int i = 0; i < 8; i++)
		{
			const int x = 8 + i * 60;
			v.push_back(At(x, 8, 130));
			v.push_back(At(x + 40, 8, 130));
			v.push_back(At(x, 24, 130));
		}
		return v;
	}

	// Two sprites over the same rectangle: the second reads what the first wrote. This is
	// the shape SpritesOnly exists for, and it must split at every level that is not
	// Disabled.
	std::vector<Vertex> OverlappingSprites()
	{
		std::vector<Vertex> v;
		for (int i = 0; i < 4; i++)
		{
			v.push_back(At(64, 64, 130));
			v.push_back(At(320, 320, 130));
		}
		return v;
	}

	u32 DrawsFor(GSHWAutoFlushLevel engine, GSHWAutoFlushLevel hardware_key, u32 prim,
		const std::vector<Vertex>& verts)
	{
		const HardwareKeyGuard guard(hardware_key);
		auto p = std::make_unique<AutoFlushProbe>();
		p->m_engine_level = engine;
		p->Configure(prim);
		p->Feed(verts);
		p->FinishDraw();
		return p->m_draws;
	}
} // namespace

// The defect. The SW engine asks for auto-flush; the hardware key says SpritesOnly, which
// is what Jak 3's GameDB entry ships. A self-texturing triangle fan whose primitives draw
// over each other has to split, because on the console each face reads what the face
// before it wrote.
TEST(GsAutoFlushEngineLevel, ASelfTexturingFanSplitsWhenTheEngineAsksForIt)
{
	const u32 draws = DrawsFor(GSHWAutoFlushLevel::Enabled, GSHWAutoFlushLevel::SpritesOnly,
		GS_TRIANGLEFAN, OverlappingFan());

	EXPECT_GT(draws, 1u) << "the batch was never split, so every primitive read the same source";
}

// And at the level the engine really does have. Nothing about the hardware key should
// reach this decision at all, so the same geometry splits whatever the key says.
TEST(GsAutoFlushEngineLevel, TheHardwareKeyDoesNotDecideTheSplit)
{
	const std::vector<Vertex> fan = OverlappingFan();

	const u32 with_sprites_only = DrawsFor(GSHWAutoFlushLevel::Enabled,
		GSHWAutoFlushLevel::SpritesOnly, GS_TRIANGLEFAN, fan);
	const u32 with_enabled = DrawsFor(GSHWAutoFlushLevel::Enabled,
		GSHWAutoFlushLevel::Enabled, GS_TRIANGLEFAN, fan);
	const u32 with_disabled = DrawsFor(GSHWAutoFlushLevel::Enabled,
		GSHWAutoFlushLevel::Disabled, GS_TRIANGLEFAN, fan);

	EXPECT_EQ(with_sprites_only, with_enabled);
	EXPECT_EQ(with_sprites_only, with_disabled);
}

// The scope guard. Only draws that actually read their own batch's pixels may split; a
// self-texturing draw whose primitives never overlap has to stay one draw, or every
// title with a framebuffer-sourced pass pays for a split it does not need.
TEST(GsAutoFlushEngineLevel, ANonOverlappingSelfTexturingDrawStaysOneDraw)
{
	const u32 draws = DrawsFor(GSHWAutoFlushLevel::Enabled, GSHWAutoFlushLevel::SpritesOnly,
		GS_TRIANGLELIST, DisjointTriangles());

	EXPECT_EQ(draws, 1u) << "a draw whose primitives never read what it wrote must not split";
}

// The narrowing is not deleted, it moves to the level that owns it. A hardware renderer
// running at SpritesOnly still refuses a triangle fan...
TEST(GsAutoFlushEngineLevel, ASpritesOnlyEngineStillRefusesATriangleFan)
{
	const u32 draws = DrawsFor(GSHWAutoFlushLevel::SpritesOnly, GSHWAutoFlushLevel::Enabled,
		GS_TRIANGLEFAN, OverlappingFan());

	EXPECT_EQ(draws, 1u) << "SpritesOnly must still narrow the split to sprites";
}

// ...and still splits the sprites it is named for.
TEST(GsAutoFlushEngineLevel, ASpritesOnlyEngineStillSplitsOverlappingSprites)
{
	const u32 draws = DrawsFor(GSHWAutoFlushLevel::SpritesOnly, GSHWAutoFlushLevel::Enabled,
		GS_SPRITE, OverlappingSprites());

	EXPECT_GT(draws, 1u) << "SpritesOnly is the level that splits sprites";
}

// Disabled means disabled on either side of the seam: nothing splits.
TEST(GsAutoFlushEngineLevel, ADisabledEngineNeverSplits)
{
	EXPECT_EQ(DrawsFor(GSHWAutoFlushLevel::Disabled, GSHWAutoFlushLevel::Enabled,
				  GS_TRIANGLEFAN, OverlappingFan()),
		1u);
	EXPECT_EQ(DrawsFor(GSHWAutoFlushLevel::Disabled, GSHWAutoFlushLevel::Enabled,
				  GS_SPRITE, OverlappingSprites()),
		1u);
}
