// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// A triangle whose setup inverts exactly does not trail the plane.
//
// The scanline's coordinate trails the exact plane by one 16.16 unit on each axis
// a triangle walks forward, which moves the sampled texel wherever the coordinate
// lands exactly on a boundary. Two console captures bracket when it does not.
//
// gs-tclag2 (SCPH-30001) drew the whole space the term could live in -- steps of a
// half, one, one and a half, two and three texels per pixel; NEAREST and LINEAR;
// the STQ plane at Q = 1, the UV register and a live divide; CLAMP and REPEAT; U
// and V -- and the console TRAILS on every arm that walks forward. Only a
// descending walk holds.
//
// Jak 3's draw 706 is one of those shapes and does not trail: a one-to-one blit,
// TRIANGLESTRIP over X 0..16 with S 0..1 and Q exactly 1, TW 4, NEAREST, CLAMP,
// and the console copies its source palette texel for texel, 768 of 768 entries.
//
// gs-tclag3 resolves the two. Draw 706 verbatim plus twenty-one one-variable arms
// against thirteen pre-registered rules, and ONE predicts every arm: twice the
// triangle's area, in 12.4 units squared, is a power of two. Draw 706's is 2^16.
// The primitive class, the sub-pixel placement, the texture size, the extent, the
// wrap mode, the filter, the route, XYOFFSET, the blend, the texture function and
// the scissor are each refuted by an arm that moves them and leaves the reading
// alone.
//
// The cases below are that table in miniature: the same blit at areas that are
// powers of two and at areas that are not, with everything else held. Each draw
// goes through the real GSRasterizer with the real generated setup and scanline,
// so the vertices are the input and the framebuffer is the output.
//
// Rides ARCH_ARM64 like its siblings: the generators are per-architecture.

#include "common/Pcsx2Defs.h"
#include "GS/MultiISA.h"

#if defined(ARCH_ARM64) && !defined(MULTI_ISA_SHARED_COMPILATION)

#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSState.h"
#include "common/HostSys.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef _WIN32
#include <sys/mman.h>
#else
#include "common/RedtapeWindows.h"
#endif

namespace
{
using DrawScanlinePtr = void (*)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);
using SetupPrimPtr = void (*)(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, GSScanlineLocalData& local);

// The naming texture is 64 texels on a side, `1 << (sel.tw + 3)` at tw 3, so one
// texture serves every extent below and CLAMP pins anything past it.
constexpr int kTex = 64;

/// texel (x, y), as a colour no other texel carries.
constexpr u32 Texel(int x, int y)
{
	return 0x80000000u | (static_cast<u32>((x ^ y) * 4) << 16)
	       | (static_cast<u32>(y * 4) << 8) | static_cast<u32>(x * 4);
}

/// Where a NEAREST fetch lands, given the coordinate in texels and whether the
/// axis trails. The trail is one unit of 16.16, so it can only move the texel when
/// the coordinate sits exactly on a boundary; CLAMP pins the result to the
/// texture, which is what saves column zero when it does move.
int SampledTexel(double coord, bool trails)
{
	s32 fixed = static_cast<s32>(std::llround(coord * 65536.0));

	if (trails)
		fixed -= 1;

	return std::min(std::max(fixed >> 16, 0), kTex - 1);
}

class SwStqBlitTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 16 * 1024;
	static constexpr size_t kCodeBufferSize = 8 * kCodeSlotSize;

	static void SetUpTestSuite()
	{
#ifdef _WIN32
		s_code = static_cast<u8*>(VirtualAlloc(nullptr, kCodeBufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#elif defined(__APPLE__)
		s_code = static_cast<u8*>(mmap(nullptr, kCodeBufferSize, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0));
		if (s_code == MAP_FAILED)
			s_code = nullptr;
#else
		s_code = static_cast<u8*>(mmap(nullptr, kCodeBufferSize, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		if (s_code == MAP_FAILED)
			s_code = nullptr;
#endif
		s_code_used = 0;
		s_mem = new GSLocalMemory();
	}

	static void TearDownTestSuite()
	{
		delete s_mem;
		s_mem = nullptr;

		if (s_code)
		{
#ifdef _WIN32
			VirtualFree(s_code, 0, MEM_RELEASE);
#else
			munmap(s_code, kCodeBufferSize);
#endif
			s_code = nullptr;
		}
	}

	static u8* Slot()
	{
		if (!s_code || s_code_used + kCodeSlotSize > kCodeBufferSize)
			return nullptr;

		u8* slot = s_code + s_code_used;
		s_code_used += kCodeSlotSize;
		return slot;
	}

	static DrawScanlinePtr CompileScanline(GSScanlineSelector sel)
	{
		u8* slot = Slot();
		if (!slot)
			return nullptr;

		HostSys::BeginCodeWriteRange(slot, kCodeSlotSize);
		GSDrawScanlineCodeGenerator cg(sel.key, slot, kCodeSlotSize);
		cg.Generate();
		HostSys::EndCodeWriteRange(slot, kCodeSlotSize);
		HostSys::FlushInstructionCache(slot, static_cast<u32>(cg.GetSize()));

		return reinterpret_cast<DrawScanlinePtr>(const_cast<u8*>(cg.GetCode()));
	}

	// The setup generator takes a reduced key, exactly as GetScanlineGlobalData
	// builds it, so this compiles the variant a real draw would get.
	static SetupPrimPtr CompileSetup(GSScanlineSelector full)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.iip = full.iip;
		sel.tfx = full.tfx;
		sel.tcc = full.tcc;
		sel.fst = full.fst;
		sel.uvwalk = full.uvwalk;
		sel.fge = full.fge;
		sel.prim = full.prim;
		sel.fb = full.fb;
		sel.zb = full.zb;
		sel.zoverflow = full.zoverflow;
		sel.zequal = full.zequal;
		sel.notest = full.notest;

		u8* slot = Slot();
		if (!slot)
			return nullptr;

		HostSys::BeginCodeWriteRange(slot, kCodeSlotSize);
		GSSetupPrimCodeGenerator cg(sel.key, slot, kCodeSlotSize);
		cg.Generate();
		HostSys::EndCodeWriteRange(slot, kCodeSlotSize);
		HostSys::FlushInstructionCache(slot, static_cast<u32>(cg.GetSize()));

		return reinterpret_cast<SetupPrimPtr>(const_cast<u8*>(cg.GetCode()));
	}

	// DECAL with TCC on: the stored pixel is the sampled texel and nothing else, so
	// the framebuffer reports the coordinate directly. Draw 706 is MODULATE against
	// a vertex colour of 128, which is that function's identity, so the two store
	// the same word and DECAL keeps the vertex interpolator out of the reading.
	//
	// A coverage test, not `notest`: the strip's two triangles have ragged spans
	// and a scanline compiled without one writes whole vectors past their ends.
	static GSScanlineSelector MakeSelector(bool uv_route, bool linear)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = 0;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.ztst = ZTST_ALWAYS;
		sel.tfx = TFX_DECAL;
		sel.tcc = 1;
		// Both roads bit-cast a 16.16 coordinate; only the UV register walks the
		// console's 12.15 accumulator. GSRendererSW derives the pair the same way:
		// fst is set for a sprite or for Q identically one, uvwalk only for the UV
		// register or a sprite.
		sel.fst = 1;
		sel.uvwalk = uv_route ? 1 : 0;
		sel.ltf = linear ? 1 : 0;
		sel.tlu = 0;
		sel.tw = 3; // 1 << (tw + 3) == 64 texels wide
		sel.wms = CLAMP_CLAMP;
		sel.wmt = CLAMP_CLAMP;
		sel.ababcd = 0xff;
		sel.prim = GS_TRIANGLE_CLASS;
		sel.iip = 0;
		sel.fwrite = 1;
		sel.notest = 0;
		sel.colclamp = 1;
		return sel;
	}

	static const GSPixelOffset4* GetOffsets()
	{
		GIFRegFRAME frame;
		frame.U64 = 0;
		frame.FBP = 0;
		frame.FBW = 1;
		frame.PSM = PSMCT32;

		GIFRegZBUF zbuf;
		zbuf.U64 = 0;
		zbuf.ZBP = 256;
		zbuf.PSM = PSMZ32;

		return s_mem->GetPixelOffset4(frame, zbuf);
	}

	static u32 PixelAddr(int x, int y)
	{
		return GSLocalMemory::m_psm[PSMCT32].info.pa(x, y, 0, 1);
	}

	/// One vertex. `u` and `v` are the texture coordinate in texels.
	static GSVertexSW Vertex(float x, float y, float u, float v, bool uv_route, bool linear)
	{
		GSVertexSW out = GSVertexSW::zero();

		out.p = GSVector4(x, y, 0.0f, 0.0f);
		out.p.F64[1] = 0.0;
		out.c = GSVector4(16384.0f, 16384.0f, 16384.0f, 16384.0f);
		// The UV register is 12.4 and the vertex conversion shifts it up by twelve;
		// the STQ road at Q = 1 multiplies S by 0x10000 << TW where S is u / (1 << TW).
		// Both reach the scanline as the same 16.16 texels.
		out.t = GSVector4(u * 65536.0f, v * 65536.0f, uv_route ? 0.0f : 1.0f, 0.0f);

		if (linear)
		{
			// GSRendererSW folds the linear filter's half-texel straddle into the
			// vertex on the affine road (~1318), because the scanline's own
			// subtraction is gated on the perspective one.
			out.t -= GSVector4(32768.0f, 32768.0f, 0.0f, 0.0f);
		}

		return out;
	}

	/// Draws one strip of two triangles over a `w` x `h` rectangle whose coordinate
	/// starts at `bias` and steps `step` texels per pixel along x and one texel per
	/// ROW along y, and returns what it stored.
	///
	/// Twice each triangle's area, in 12.4 squared, is `w * h * 256` -- which is the
	/// variable every case below moves.
	static bool Blit(bool uv_route, bool linear, float bias, float step, int w, int h,
		std::vector<u32>& out)
	{
		// Compiled once per road and filter: the generators are deterministic, and a
		// pair per case would outrun the code buffer.
		const int slot = (uv_route ? 2 : 0) + (linear ? 1 : 0);
		const GSScanlineSelector sel = MakeSelector(uv_route, linear);

		if (!s_setup[slot])
		{
			s_setup[slot] = CompileSetup(sel);
			s_draw[slot] = CompileScanline(sel);
		}

		if (!s_setup[slot] || !s_draw[slot])
			return false;

		u32* vm32 = s_mem->vm32();
		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
				vm32[PixelAddr(x, y)] = 0;
		}

		alignas(32) static u32 tex[kTex * kTex];
		for (int y = 0; y < kTex; y++)
		{
			for (int x = 0; x < kTex; x++)
				tex[y * kTex + x] = Texel(x, y);
		}

		isa_native::GSRasterizerData data;
		data.primclass = GS_TRIANGLE_CLASS;
		data.scissor = GSVector4i(0, 0, w, h);
		data.bbox = GSVector4i(0, 0, w, h);
		data.scanmsk_value = 0;
		data.setup_prim = s_setup[slot];
		data.draw_scanline = s_draw[slot];
		data.draw_edge = nullptr;

		data.global.sel = sel;
		data.global.vm = s_mem->vm8();
		data.global.fzbr = GetOffsets()->row;
		data.global.fzbc = GetOffsets()->col;
		data.global.fm = GSVector4i(0);
		data.global.zm = GSVector4i(static_cast<int>(0xffffffffu));
		data.global.tex[0] = tex;

		// CLAMP on both axes over sixty-four texels, as GSRendererSW derives it from
		// CLAMP.WMS / WMT: minimum zero, maximum tw - 1, and no repeat mask.
		data.global.t.min = GSVector4i::zero();
		data.global.t.max = GSVector4i::zero();
		data.global.t.max.U16[0] = kTex - 1;
		data.global.t.max.U16[4] = kTex - 1;
		data.global.t.min = data.global.t.min.xxxxlh();
		data.global.t.max = data.global.t.max.xxxxlh();
		data.global.t.mask = GSVector4i::zero();
		data.global.t.invmask = ~data.global.t.mask;

		// Draw 706's own shape: a TRIANGLESTRIP of two triangles over the rectangle,
		// the coordinate running across it.
		const float fw = static_cast<float>(w);
		const float fh = static_cast<float>(h);
		const float uw = bias + step * fw;

		GSVertexSW vertex[4] = {
			Vertex(0.0f, 0.0f, bias, bias, uv_route, linear),
			Vertex(fw, 0.0f, uw, bias, uv_route, linear),
			Vertex(0.0f, fh, bias, bias + fh, uv_route, linear),
			Vertex(fw, fh, uw, bias + fh, uv_route, linear),
		};

		u16 index[6] = {0, 1, 2, 1, 2, 3};

		data.vertex = vertex;
		data.vertex_count = 4;
		data.index = index;
		data.index_count = 6;

		isa_native::GSRasterizer r(nullptr, 0, 1);
		r.Draw(data);

		out.assign(static_cast<size_t>(w) * h, 0);
		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
				out[static_cast<size_t>(y) * w + x] = vm32[PixelAddr(x, y)];
		}

		return true;
	}

	/// One NEAREST case, against the coordinate the console's rule puts it on.
	static void ExpectNearest(float step, int w, int h, bool trails)
	{
		std::vector<u32> got;
		ASSERT_TRUE(Blit(false, false, 0.0f, step, w, h, got));

		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
			{
				const u32 want = Texel(SampledTexel(static_cast<double>(step) * x, trails),
					SampledTexel(y, false));

				EXPECT_EQ(got[static_cast<size_t>(y) * w + x], want)
					<< "pixel (" << x << ", " << y << ")";
			}
		}
	}

	static SetupPrimPtr s_setup[4];
	static DrawScanlinePtr s_draw[4];
	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

SetupPrimPtr SwStqBlitTest::s_setup[4] = {};
DrawScanlinePtr SwStqBlitTest::s_draw[4] = {};
u8* SwStqBlitTest::s_code = nullptr;
size_t SwStqBlitTest::s_code_used = 0;
GSLocalMemory* SwStqBlitTest::s_mem = nullptr;

// ★ Draw 706 itself: sixteen by sixteen, one texel per pixel, so twice the area is
// 256 * 256 = 2^16 and the setup inverts exactly. The console copies its source
// palette texel for texel, 768 of 768 entries on three palettes, and this must
// hold whatever else moves.
TEST_F(SwStqBlitTest, DrawSevenZeroSixHoldsTheTexelItLandsOn)
{
	ExpectNearest(1.0f, 16, 16, false);
}

// ★ gs-tclag3's `tall`: the same blit over sixteen by eighty-eight. Twice the area
// is 256 * 1408, which is not a power of two, and the console TRAILS -- so every
// column but the one CLAMP pins samples the texel below. Nothing about the step,
// the filter, the route, the wrap or the texture has moved.
TEST_F(SwStqBlitTest, ATallStripTrails)
{
	ExpectNearest(1.0f, 16, 88, true);
}

// ★ gs-tclag3's `w15`: fifteen by sixteen. One pixel narrower than draw 706 takes
// twice the area off a power of two, and the console trails.
TEST_F(SwStqBlitTest, AFifteenWideStripTrails)
{
	ExpectNearest(1.0f, 15, 16, true);
}

// gs-tclag3's `wide64`: sixty-four by sixteen, so twice the area is 2^18 and the
// console holds again. Four times draw 706's extent and the reading does not move,
// which is what refutes the extent as the variable.
TEST_F(SwStqBlitTest, ASixtyFourWideStripHolds)
{
	ExpectNearest(1.0f, 64, 16, false);
}

// gs-tclag2's one-and-a-half texels per pixel, at an area that is not a power of
// two: the console trails there, and so must we. Every other column lands on a
// texel boundary, so half the span moves and half does not.
TEST_F(SwStqBlitTest, AThreeHalvesStepTrailsAtAnInexactArea)
{
	ExpectNearest(1.5f, 15, 16, true);
}

// And gs-tclag2's half a texel per pixel, likewise.
TEST_F(SwStqBlitTest, AHalfStepTrailsAtAnInexactArea)
{
	ExpectNearest(0.5f, 15, 16, true);
}

// The same blit on the UV road at the convention a game writes it in: U = 16x + 8
// puts the sample at the texel's centre, half a texel from any boundary, so no
// rule about the trail can reach it. It is here because the two roads are decided
// separately in setup and a fix to one must not move the other.
TEST_F(SwStqBlitTest, AOneToOneUvBlitAtTexelCentresIsUnmoved)
{
	std::vector<u32> got;
	ASSERT_TRUE(Blit(true, false, 0.5f, 1.0f, 16, 16, got));

	for (int y = 0; y < 16; y++)
	{
		for (int x = 0; x < 16; x++)
			EXPECT_EQ(got[static_cast<size_t>(y) * 16 + x], Texel(x, y)) << "pixel (" << x << ", " << y << ")";
	}
}

// And under LINEAR at texel centres, where the half-texel straddle puts the
// coordinate back on the boundary: a filter weight of zero, so the blit is exact.
// Draw 706's area, so it holds; this is what says the exemption is not about the
// nearest sampler.
TEST_F(SwStqBlitTest, AOneToOneStqBlitUnderLinearIsExactAtTexelCentres)
{
	std::vector<u32> got;
	ASSERT_TRUE(Blit(false, true, 0.5f, 1.0f, 16, 16, got));

	for (int y = 0; y < 16; y++)
	{
		for (int x = 0; x < 16; x++)
			EXPECT_EQ(got[static_cast<size_t>(y) * 16 + x], Texel(x, y)) << "pixel (" << x << ", " << y << ")";
	}
}
} // namespace

#endif // ARCH_ARM64 && !MULTI_ISA_SHARED_COMPILATION
