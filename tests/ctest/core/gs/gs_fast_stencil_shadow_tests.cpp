// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the fast stencil shadow road (GS/Renderers/Common/GSFastStencilShadow.h): which devices
// take it, which draws take it, and which engine leaves the counter unsplit.
//
// The device rule has no setting behind it, so these cases are the whole contract. It is on for
// Vulkan with texture barriers off and dual-source blending, and off when any one of the three is
// missing. The case that matters most is D3D11: it also runs without texture barriers, and reading
// "no barriers" as "frame reads are expensive" would put a draw the D3D11 shader cannot express on
// a backend where the read it replaces is cheap.
//
// Rides gs_vertex_tests.

#include "GS/GS.h"
#include "GS/GSState.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSFastStencilShadow.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

namespace
{
	constexpr RenderAPI kAllApis[] = {
		RenderAPI::None, RenderAPI::D3D11, RenderAPI::Metal, RenderAPI::D3D12, RenderAPI::Vulkan, RenderAPI::OpenGL};
} // namespace

// The Adreno shape: Vulkan, the RT-copy workaround has turned barriers off, and the blend unit has
// a second input.
TEST(GSFastStencilShadow, OnForVulkanWithoutBarriersWithDualSource)
{
	EXPECT_TRUE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, false, true));
}

TEST(GSFastStencilShadow, OffWhenTextureBarriersAreOn)
{
	// Desktop Vulkan, and OverrideTextureBarriers=1 on an Adreno part.
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, true, true));
}

TEST(GSFastStencilShadow, OffWithoutDualSourceBlend)
{
	// A Mali part on the RT-copy workaround.
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, false, false));
}

TEST(GSFastStencilShadow, OffOnD3D11WithoutBarriers)
{
	EXPECT_FALSE(GSFastStencilShadow::DeviceQualifies(RenderAPI::D3D11, false, true));
}

// Every combination: on exactly when all three facts hold.
TEST(GSFastStencilShadow, OnExactlyWhenAllThreeHold)
{
	for (RenderAPI api : kAllApis)
	{
		for (bool texture_barrier : {false, true})
		{
			for (bool dual_source : {false, true})
			{
				const bool expected = api == RenderAPI::Vulkan && !texture_barrier && dual_source;
				EXPECT_EQ(GSFastStencilShadow::DeviceQualifies(api, texture_barrier, dual_source), expected)
					<< "api " << static_cast<int>(api) << " texture_barrier " << texture_barrier << " dual_source "
					<< dual_source;
			}
		}
	}
}

// Backends other than Vulkan never assign the bit, so it has to start off.
TEST(GSFastStencilShadow, FeatureBitStartsOff)
{
	const GSDevice::FeatureSupport features;
	EXPECT_FALSE(features.fast_stencil_shadow);
}

// Which draws take the road. The registers are checked by IsCounterShape and the vertex bounds by
// VerticesQualify; a draw needs both.

namespace
{
	// Jak II's shadow counter, register for register as the census logged it: a triangle fan into
	// the 0x3300 frame, textured from that frame.
	struct CounterRegs
	{
		GIFRegPRIM prim{};
		GIFRegTEX0 tex0{};
		GIFRegTEX1 tex1{};
		GIFRegTEST test{};
		GIFRegFRAME frame{};
		GIFRegZBUF zbuf{};
		GIFRegFBA fba{};

		CounterRegs()
		{
			prim.PRIM = GS_TRIANGLEFAN;
			prim.TME = 1;
			frame.FBP = 0x3300 >> 5;
			frame.PSM = PSMCT32;
			frame.FBMSK = 0x00FFFFFF;
			tex0.TBP0 = frame.Block();
			tex0.PSM = PSMCT32;
			tex0.TFX = TFX_MODULATE;
			tex0.TCC = 1;
			test.ATE = 1;
			test.ATST = ATST_ALWAYS;
			zbuf.ZMSK = 1;
		}

		bool Matches() const { return GSFastStencilShadow::IsCounterShape(prim, tex0, tex1, test, frame, zbuf, fba); }
	};
} // namespace

TEST(GSFastStencilShadow, ShapeMatchesTheCounter)
{
	CounterRegs r;
	EXPECT_TRUE(r.Matches());

	// No alpha test at all passes everything too.
	r.test.ATE = 0;
	r.test.ATST = ATST_GEQUAL;
	EXPECT_TRUE(r.Matches());
}

// Ratchet & Clank: Up Your Arsenal's effect counter, as the renderer sees it. Its alpha test never
// passes and fails to the frame only, so it cannot change what is written, and the renderer's cached
// registers have already dropped it (ATE cleared, ATST and AFAIL left as written). The renderer checks
// the shape on that copy, so the counter takes the road, accepting a hidden counter that can drift a
// few levels low from 128.
TEST(GSFastStencilShadow, ShapeMatchesRatchetsCounterAfterTheAlphaTestIsDropped)
{
	CounterRegs r;
	r.test.ATE = 0;
	r.test.ATST = ATST_NEVER;
	r.test.AFAIL = AFAIL_FB_ONLY;
	EXPECT_TRUE(r.Matches());
}

// One register away from the counter is not the counter. Each of these changes what the draw writes
// or what it reads, so the blend equation would no longer be the draw.
TEST(GSFastStencilShadow, ShapeRejectsEveryOneRegisterChange)
{
	struct Change
	{
		const char* name;
		void (*apply)(CounterRegs&);
	};
	const Change changes[] = {
		{"untextured", [](CounterRegs& r) { r.prim.TME = 0; }},
		{"gouraud", [](CounterRegs& r) { r.prim.IIP = 1; }},
		{"fog", [](CounterRegs& r) { r.prim.FGE = 1; }},
		{"aa1", [](CounterRegs& r) { r.prim.AA1 = 1; }},
		{"24-bit frame", [](CounterRegs& r) { r.frame.PSM = PSMCT24; }},
		{"colour written", [](CounterRegs& r) { r.frame.FBMSK = 0; }},
		{"alpha masked too", [](CounterRegs& r) { r.frame.FBMSK = 0xFFFFFFFF; }},
		{"texture is another buffer", [](CounterRegs& r) { r.tex0.TBP0 = 0; }},
		{"16-bit texture", [](CounterRegs& r) { r.tex0.PSM = PSMCT16; }},
		{"decal", [](CounterRegs& r) { r.tex0.TFX = TFX_DECAL; }},
		{"texture alpha off", [](CounterRegs& r) { r.tex0.TCC = 0; }},
		{"linear magnification", [](CounterRegs& r) { r.tex1.MMAG = 1; }},
		{"linear minification", [](CounterRegs& r) { r.tex1.MMIN = 1; }},
		{"destination alpha test", [](CounterRegs& r) { r.test.DATE = 1; }},
		{"alpha test that can fail", [](CounterRegs& r) { r.test.ATST = ATST_GEQUAL; }},
		// Ratchet & Clank's counter with its alpha test as written. The auto-flush predicate sees these
		// registers, so it keeps splitting that counter; the renderer sees the copy with the test dropped
		// and takes it (ShapeMatchesRatchetsCounterAfterTheAlphaTestIsDropped).
		{"alpha test never passes, fails to the frame only",
			[](CounterRegs& r) {
				r.test.ATST = ATST_NEVER;
				r.test.AFAIL = AFAIL_FB_ONLY;
			}},
		{"depth written", [](CounterRegs& r) { r.zbuf.ZMSK = 0; }},
		{"fba", [](CounterRegs& r) { r.fba.FBA = 1; }},
	};

	for (const Change& c : changes)
	{
		CounterRegs r;
		c.apply(r);
		EXPECT_FALSE(r.Matches()) << c.name;
	}
}

TEST(GSFastStencilShadow, VerticesQualifyOnlyOnTheCounterSteps)
{
	const GSVector4 p0(10.5f, 20.5f, 0.0f, 0.0f);
	const GSVector4 p1(200.5f, 180.5f, 0.0f, 0.0f);

	EXPECT_TRUE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0, p1));
	EXPECT_TRUE(GSFastStencilShadow::VerticesQualify(130, 130, p0, p1, p0, p1));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(126, 130, p0, p1, p0, p1));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(127, 131, p0, p1, p0, p1));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(64, 64, p0, p1, p0, p1));
}

TEST(GSFastStencilShadow, VerticesQualifyOnlyWhenSamplingTheirOwnPixel)
{
	const GSVector4 p0(10.5f, 20.5f, 0.0f, 0.0f);
	const GSVector4 p1(200.5f, 180.5f, 0.0f, 0.0f);
	const GSVector4 half(0.5f, 0.5f, 0.0f, 0.0f);
	const GSVector4 one_x(1.0f, 0.0f, 0.0f, 0.0f);
	const GSVector4 one_y(0.0f, 1.0f, 0.0f, 0.0f);

	// Texel centres half a pixel off the pixel positions still read the same pixel.
	EXPECT_TRUE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0 - half, p1 - half));

	// A whole pixel off on either axis, at either corner, reads a neighbour.
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0 + one_x, p1));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0, p1 - one_y));
	EXPECT_FALSE(GSFastStencilShadow::VerticesQualify(127, 130, p0, p1, p0 - one_x, p1 - one_x));
}

// Which engine leaves the counter unsplit.
//
// Auto-flush splits a self-texturing draw in GSState, before any renderer sees it. The hardware
// renderer's blend unit orders the counter's triangles inside one draw, so on that engine the split
// only costs draws. The software renderer takes one texture snapshot per draw, and the split is what
// makes each face read the one before it, so on that engine it must stay. Both engines reach the same
// predicate, and the auto-flush keys do not say which engine is asking. So the exemption is keyed on
// the engine, and these cases sweep both keys and expect the engine alone to decide.
//
// Driven through the real GIF parse road: PACKED {STQ, RGBA, XYZ2} records in one packet through
// Transfer, and the draw count out. SpritesOnly is left out of the sweeps: at that level no triangle
// reaches the split on any engine, before this exemption is consulted.

namespace
{
	struct Vertex
	{
		int x, y; // pixels
		u32 alpha;
	};

	void EncodeVertex(GIFPackedReg* r, const Vertex& v)
	{
		std::memset(r, 0, sizeof(GIFPackedReg) * 3);

		// S and T track X and Y over a 512x512 window with Q = 1, so every vertex reads the pixel under it.
		r[0].STQ.S = static_cast<float>(v.x) / 512.0f;
		r[0].STQ.T = static_cast<float>(v.y) / 512.0f;
		r[0].STQ.Q = 1.0f;

		r[1].RGBA.R = 128;
		r[1].RGBA.G = 128;
		r[1].RGBA.B = 128;
		r[1].RGBA.A = static_cast<u8>(v.alpha);

		r[2].XYZ2.X = static_cast<u16>(v.x << 4); // 12.4
		r[2].XYZ2.Y = static_cast<u16>(v.y << 4);
	}

	GIFTag MakeTag(u32 vertices)
	{
		GIFTag t = {};
		t.NLOOP = vertices;
		t.NREG = 3;
		t.FLG = GIF_FLG_PACKED;
		t.REGS = (u64(GIF_REG_STQ) << 0) | (u64(GIF_REG_RGBA) << 4) | (u64(GIF_REG_XYZ2) << 8);
		return t;
	}

	// GSConfig is a global; restore what was there.
	class AutoFlushKeys
	{
	public:
		AutoFlushKeys(GSHWAutoFlushLevel hardware, bool software)
			: m_hardware(GSConfig.UserHacks_AutoFlush)
			, m_software(GSConfig.AutoFlushSW)
		{
			GSConfig.UserHacks_AutoFlush = hardware;
			GSConfig.AutoFlushSW = software;
		}
		~AutoFlushKeys()
		{
			GSConfig.UserHacks_AutoFlush = m_hardware;
			GSConfig.AutoFlushSW = m_software;
		}

	private:
		GSHWAutoFlushLevel m_hardware;
		bool m_software;
	};

	using EnvChange = void (*)(GSDrawingEnvironment&);

	class CounterProbe final : public GSState
	{
	public:
		CounterProbe()
			: m_regs_storage(std::make_unique<GSPrivRegSet>())
		{
			// A flush walks the privileged registers looking for the display buffer; nothing
			// constructs them for a bare GSState.
			std::memset(m_regs_storage.get(), 0, sizeof(GSPrivRegSet));
			m_regs = m_regs_storage.get();
		}

		void Draw() override { m_draws++; }

		// The engine asks for the split on every primitive.
		GSHWAutoFlushLevel GetAutoFlushLevel() const override { return GSHWAutoFlushLevel::Enabled; }

		// The base asserts "not implemented"; the kick reaches it for AA1 prims.
		bool IsCoverageAlphaSupported() override { return true; }

		// Stands in for the engine: GSRendererHW's constructor sets this from its device, and the
		// software renderer never does.
		void SetUnsplitEngine(bool unsplit) { m_unsplit_stencil_counter = unsplit; }

		u32 m_draws = 0;

		// The counter at Jak 3's frame size: flat triangles into a 512x416 PSMCT32 frame, textured
		// nearest from its own pages, modulating with alpha, writing alpha only, never writing depth.
		void Configure(u32 prim, EnvChange change)
		{
			GSDrawingContext& ctx = m_env.CTXT[0];

			ctx.FRAME.FBP = 0;
			ctx.FRAME.FBW = 8;
			ctx.FRAME.PSM = PSMCT32;
			ctx.FRAME.FBMSK = 0x00FFFFFF;
			ctx.ZBUF.ZBP = 0x100;
			ctx.ZBUF.PSM = PSMZ24;
			ctx.ZBUF.ZMSK = 1;
			ctx.TEX0.TBP0 = 0;
			ctx.TEX0.TBW = 8;
			ctx.TEX0.PSM = PSMCT32;
			ctx.TEX0.TW = 9;
			ctx.TEX0.TH = 9;
			ctx.TEX0.TCC = 1;
			ctx.TEX0.TFX = TFX_MODULATE;
			ctx.TEX1.MXL = 0;
			ctx.TEX1.MMAG = 0;
			ctx.TEX1.MMIN = 0;
			ctx.TEX1.LCM = 0;
			ctx.TEST.ATE = 1;
			ctx.TEST.ATST = ATST_ALWAYS;
			ctx.FBA.FBA = 0;
			ctx.CLAMP.WMS = CLAMP_REPEAT;
			ctx.CLAMP.WMT = CLAMP_REPEAT;
			ctx.SCISSOR.SCAX0 = 0;
			ctx.SCISSOR.SCAY0 = 0;
			ctx.SCISSOR.SCAX1 = 511;
			ctx.SCISSOR.SCAY1 = 415;
			ctx.XYOFFSET.OFX = 0;
			ctx.XYOFFSET.OFY = 0;

			m_env.PRIM.CTXT = 0;
			m_env.PRIM.PRIM = prim;
			m_env.PRIM.IIP = 0;
			m_env.PRIM.TME = 1;
			m_env.PRIM.FST = 0;
			m_env.PRIM.FGE = 0;
			m_env.PRIM.AA1 = 0;

			if (change)
				change(m_env);

			ctx.UpdateScissor();
			m_nativeres = true;
			temp_draw_rect = GSVector4i::zero();
			UpdateContext();
			ResetHandlers();
			UpdateVertexKick();
		}

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

	private:
		std::unique_ptr<GSPrivRegSet> m_regs_storage;
	};

	// A shadow volume in miniature: a fan whose rim walks the frame in wide steps, so later triangles
	// draw over earlier ones, as a closed volume's far faces land on its near faces' pixels. Faces
	// alternate between the counter's two steps.
	std::vector<Vertex> OverlappingFan()
	{
		std::vector<Vertex> v;
		v.push_back({256, 208, 130});
		const int rim[6][2] = {{496, 208}, {62, 323}, {330, 23}, {330, 393}, {62, 93}, {496, 208}};
		for (const auto& p : rim)
			v.push_back({p[0], p[1], (v.size() & 1) ? 130u : 127u});
		return v;
	}

	u32 DrawsFor(bool unsplit_engine, GSHWAutoFlushLevel hardware_key, bool software_key, EnvChange change = nullptr)
	{
		const AutoFlushKeys keys(hardware_key, software_key);
		auto p = std::make_unique<CounterProbe>();
		p->SetUnsplitEngine(unsplit_engine);
		p->Configure(GS_TRIANGLEFAN, change);
		p->Feed(OverlappingFan());
		p->FinishDraw();
		return p->m_draws;
	}

	constexpr GSHWAutoFlushLevel kSplittingKeys[] = {GSHWAutoFlushLevel::Disabled, GSHWAutoFlushLevel::Enabled};
} // namespace

TEST(GSFastStencilShadow, HardwareEngineDrawsTheCounterUnsplit)
{
	EXPECT_EQ(DrawsFor(true, GSHWAutoFlushLevel::Enabled, true), 1u);
}

// The same volume on the software engine is split, however either key is set.
TEST(GSFastStencilShadow, SoftwareEngineSplitsTheCounterWhateverTheKeys)
{
	for (GSHWAutoFlushLevel hardware_key : kSplittingKeys)
	{
		for (bool software_key : {false, true})
		{
			EXPECT_GT(DrawsFor(false, hardware_key, software_key), 1u)
				<< "hardware key " << static_cast<int>(hardware_key) << " software key " << software_key;
		}
	}
}

// Nor can the keys take the exemption away from the hardware engine.
TEST(GSFastStencilShadow, HardwareEngineExemptionIgnoresTheKeys)
{
	for (GSHWAutoFlushLevel hardware_key : kSplittingKeys)
	{
		for (bool software_key : {false, true})
		{
			EXPECT_EQ(DrawsFor(true, hardware_key, software_key), 1u)
				<< "hardware key " << static_cast<int>(hardware_key) << " software key " << software_key;
		}
	}
}

// A self-texturing draw that is not the counter still splits on the hardware engine.
TEST(GSFastStencilShadow, HardwareEngineSplitsOtherSelfTexturingDraws)
{
	EXPECT_GT(DrawsFor(true, GSHWAutoFlushLevel::Enabled, true, [](GSDrawingEnvironment& env) { env.PRIM.IIP = 1; }), 1u)
		<< "gouraud";
	EXPECT_GT(DrawsFor(true, GSHWAutoFlushLevel::Enabled, true,
				  [](GSDrawingEnvironment& env) { env.CTXT[0].FRAME.FBMSK = 0; }),
		1u)
		<< "colour written";
}

// ── The measurement override (gsrunner -no-fast-stencil-shadow) ──────────────────────
//
// It exists because declaring the feedback loop turns texture barriers ON, and
// DeviceQualifies requires them OFF -- so the declared road disables this counter as a
// side effect and a base-vs-declared A/B moves two things at once. These pin the two
// properties the decomposition rests on: inert unless asked, and above the device rule
// when asked.

// Default state is off, so every build that never calls the setter answers exactly as it
// did before the switch existed. Runs first by name within the suite, before any test
// below can set it.
TEST(GSFastStencilShadowOverride, AAA_DefaultsToInert)
{
	EXPECT_FALSE(GSFastStencilShadow::IsForcedOff());
}

// The switch does not touch DeviceQualifies -- that stays a pure function of the device
// rule. The override is applied by the backend at the point it resolves the feature bit,
// which is what lets the road and the spelling stay exactly where the device put them.
TEST(GSFastStencilShadowOverride, DoesNotAlterTheDeviceRule)
{
	GSFastStencilShadow::SetForcedOff(true);
	EXPECT_TRUE(GSFastStencilShadow::DeviceQualifies(RenderAPI::Vulkan, false, true));
	GSFastStencilShadow::SetForcedOff(false);
}

// The backend's expression, as GSDeviceVK::CheckFeatures spells it. Asked for, it beats a
// qualifying device; left alone, it changes nothing on any of the four device rows.
TEST(GSFastStencilShadowOverride, BeatsAQualifyingDeviceAndIsOtherwiseInvisible)
{
	const auto resolved = [](RenderAPI api, bool texture_barrier, bool dual_source) {
		return !GSFastStencilShadow::IsForcedOff() &&
			   GSFastStencilShadow::DeviceQualifies(api, texture_barrier, dual_source);
	};

	GSFastStencilShadow::SetForcedOff(true);
	EXPECT_FALSE(resolved(RenderAPI::Vulkan, false, true)) << "the one qualifying row must go off";
	EXPECT_FALSE(resolved(RenderAPI::Vulkan, true, true));
	EXPECT_FALSE(resolved(RenderAPI::Vulkan, false, false));
	EXPECT_FALSE(resolved(RenderAPI::D3D11, false, true));

	GSFastStencilShadow::SetForcedOff(false);
	EXPECT_TRUE(resolved(RenderAPI::Vulkan, false, true)) << "and come back when not asked";
	EXPECT_FALSE(resolved(RenderAPI::Vulkan, true, true));
	EXPECT_FALSE(resolved(RenderAPI::Vulkan, false, false));
	EXPECT_FALSE(resolved(RenderAPI::D3D11, false, true));
}

// Setting it twice, or clearing it when it was never set, is not a state machine.
TEST(GSFastStencilShadowOverride, IsIdempotentAndRestorable)
{
	GSFastStencilShadow::SetForcedOff(true);
	GSFastStencilShadow::SetForcedOff(true);
	EXPECT_TRUE(GSFastStencilShadow::IsForcedOff());
	GSFastStencilShadow::SetForcedOff(false);
	GSFastStencilShadow::SetForcedOff(false);
	EXPECT_FALSE(GSFastStencilShadow::IsForcedOff());
}

// ── The force-ON override (gsrunner -force-fast-stencil-shadow) ──────────────────────
//
// E4d's inversion made this necessary: the declared road substitutes for most of what
// the counter provides rather than stacking on losing it, so "counter on AND loop
// declared" is the cell that decides whether the coupling is accidental. These pin what
// the override may and may not lift.

TEST(GSFastStencilShadowOverride, AAB_ForceOnDefaultsToInert)
{
	EXPECT_FALSE(GSFastStencilShadow::IsForcedOn());
}

// It lifts ONLY the barrier term. The Vulkan and dual-source terms decide whether the
// counter can be drawn at all, not whether it is worth drawing, so forcing past them
// would draw it wrong -- the D3D11 mistake DeviceQualifies warns about, in reverse.
TEST(GSFastStencilShadowOverride, ForceOnLiftsOnlyTheBarrierTerm)
{
	// The cell E4d could not reach: barriers ON (as the declared road leaves them).
	EXPECT_FALSE(GSFastStencilShadow::Resolve(false, false, RenderAPI::Vulkan, true, true))
		<< "device rule declines it";
	EXPECT_TRUE(GSFastStencilShadow::Resolve(false, true, RenderAPI::Vulkan, true, true))
		<< "forced on, and the backend can draw it";

	// Still gated on what the backend can draw.
	EXPECT_FALSE(GSFastStencilShadow::Resolve(false, true, RenderAPI::Vulkan, true, false))
		<< "no dual-source blending: the second factor has nowhere to go";
	EXPECT_FALSE(GSFastStencilShadow::Resolve(false, true, RenderAPI::D3D11, false, true))
		<< "no counter block in that backend's shader";
	EXPECT_FALSE(GSFastStencilShadow::Resolve(false, true, RenderAPI::D3D11, true, true));
}

// Asking for both is a harness mistake; resolve to the one that changes least from the
// shipped picture.
TEST(GSFastStencilShadowOverride, ForceOffBeatsForceOn)
{
	EXPECT_FALSE(GSFastStencilShadow::Resolve(true, true, RenderAPI::Vulkan, true, true));
	EXPECT_FALSE(GSFastStencilShadow::Resolve(true, true, RenderAPI::Vulkan, false, true))
		<< "even on the row the device rule would have allowed";
}

// With neither override asked, Resolve is DeviceQualifies on every row -- so a build
// that never calls either setter answers exactly as it did before both existed.
TEST(GSFastStencilShadowOverride, ResolveIsTheDeviceRuleWhenNeitherIsAsked)
{
	for (RenderAPI api : {RenderAPI::Vulkan, RenderAPI::D3D11, RenderAPI::OpenGL, RenderAPI::Metal})
	{
		for (bool barrier : {false, true})
		{
			for (bool dual : {false, true})
			{
				EXPECT_EQ(GSFastStencilShadow::Resolve(false, false, api, barrier, dual),
					GSFastStencilShadow::DeviceQualifies(api, barrier, dual))
					<< "api " << static_cast<int>(api) << " barrier " << barrier << " dual " << dual;
			}
		}
	}
}
