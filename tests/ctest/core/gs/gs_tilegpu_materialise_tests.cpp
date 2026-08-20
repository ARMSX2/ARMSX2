// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Rule 3 of the TileGpu texel road samples a texture window out of an ordinary RGBA8 image, and
// bin/resources/shaders/vulkan/tilegpu_materialise.glsl is the pass that fills it: one fragment per
// texel, reading guest bytes through the GS's own page/block/column swizzle. That shader carries its
// OWN copy of the address arithmetic — it is a separate file from the draw shader, and a divergence
// between the two is invisible on screen, because a wrong address produces perfectly plausible
// texture from the wrong place.
//
// So the shader is transcribed here and pinned twice over:
//
//  1. Its addresses against GSLocalMemory::PixelAddress32 — the addresses the software renderer and
//     every CPU reader use, and therefore the ones the rest of the emulator believes in.
//  2. Its whole output, texel by texel, against the CPU deswizzler the source cache's other build
//     road calls (psm.rtx). That covers what an address check cannot: the channel order, and the
//     PSMCT24 TEXA/AEM expansion the materialise bakes into alpha at BUILD time where the byte road
//     applies it per tap at draw time. Those two have to land on the same byte or a draw moved from
//     one road to the other changes colour.
//
// The window sizes and bases cross page boundaries in both axes and include a base that sits blocks
// into a page; the memory is filled with a pattern that includes exact-zero RGB texels, which is the
// only input that tells the AEM leg apart from the plain one.

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace GSTileSwizzleForms;

namespace
{
	// -- the shader, transcribed -------------------------------------------------------
	// tilegpu_materialise.glsl's main(), split at the point where it stops being addressing
	// and starts being colour. `tbw` is pages per texture row, which for a direct-colour
	// window is max(TBW, 1) — what the renderer puts in the op.

	u32 MaterialiseWord(const FormSet& f, u32 u, u32 v, u32 tbp0, u32 tbw)
	{
		const u32 page = (v >> 5) * tbw + (u >> 6);
		const u32 blk = tbp0 + page * 32 + f.block48.Eval((u >> 3) & 7, (v >> 3) & 3);
		return blk * 64 + f.col32.Eval(u & 7, v & 7);
	}

	// The fragment's output as it lands in the RGBA8 target. The shader writes float(byte)/255 per
	// channel, which a UNORM store round-trips to the same byte, so the integer form below is the
	// exact texel — not an approximation of it.
	u32 MaterialiseTexel(u32 w, u32 src_fmt, u32 texa)
	{
		if (src_fmt == 0) // PSMCT32: the guest word IS the texel
			return w;
		// PSMCT24: the top byte is not this texture's, so TEXA supplies alpha and AEM makes an
		// all-zero-RGB texel transparent.
		if ((texa & 1u) == 0u)
			return w;
		const bool aem_zero = (texa & 2u) != 0u && (w & 0x00FFFFFFu) == 0u;
		return (w & 0x00FFFFFFu) | (aem_zero ? 0u : (((texa >> 8) & 0xFFu) << 24));
	}

	// How the renderer packs TEXA into the prep op (GSRendererTileGpu::EmitMaterialiseOp), which is
	// also how the state row packs it for the byte road.
	u32 PackTexa(const GIFRegTEXA& TEXA)
	{
		return 1u | (TEXA.AEM ? 2u : 0u) | (static_cast<u32>(TEXA.TA0) << 8);
	}

	class TileGpuMaterialiseTest : public ::testing::Test
	{
	protected:
		static void SetUpTestSuite()
		{
			s_mem = new GSLocalMemory();

			// A pattern with structure, so a swapped coordinate shows up as a shifted image rather
			// than as noise that happens to differ; plus exact-zero words at a stride coprime with
			// every block dimension, which is what exercises AEM.
			u32* const vm = s_mem->vm32();
			u64 x = 0x9E3779B97F4A7C15ull;
			for (u32 i = 0; i < 4 * 1024 * 1024 / 4; i++)
			{
				x ^= x << 13;
				x ^= x >> 7;
				x ^= x << 17;
				vm[i] = ((i % 37) == 0) ? 0u : static_cast<u32>(x);
			}
		}

		static void TearDownTestSuite()
		{
			delete s_mem;
			s_mem = nullptr;
		}

		static GSLocalMemory* s_mem;
	};

	GSLocalMemory* TileGpuMaterialiseTest::s_mem = nullptr;
} // namespace

TEST_F(TileGpuMaterialiseTest, AddressMatchesPixelAddress32)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);

	for (const u32 tbw : {1u, 2u, 4u, 8u})
	{
		for (const u32 tbp0 : {0u, 32u, 37u, 0x1400u})
		{
			for (u32 v = 0; v < 72; v++)
			{
				for (u32 u = 0; u < tbw * 64 + 8; u++)
				{
					const u32 want = GSLocalMemory::PixelAddress32(static_cast<int>(u), static_cast<int>(v), tbp0, tbw);
					ASSERT_EQ(MaterialiseWord(f, u, v, tbp0, tbw), want)
						<< "u=" << u << " v=" << v << " tbp0=" << tbp0 << " tbw=" << tbw;
				}
			}
		}
	}
}

// The whole pass against the CPU deswizzler: every texel of a window, both formats, and for the
// 24-bit one every combination of the TEXA bits that shape its alpha.
TEST_F(TileGpuMaterialiseTest, WindowMatchesCpuDeswizzle)
{
	const FormSet f = Fit();
	ASSERT_TRUE(f.valid);
	const u32* const vm = s_mem->vm32();

	struct Window
	{
		u32 tbp0;
		u32 tbw; // the TEX0 register value
		int tw;
		int th;
	};
	// Bases block-into-a-page and page-aligned, widths that make the window span several page
	// columns, and heights that span several page rows.
	static constexpr Window kWindows[] = {
		{0, 1, 64, 64},
		{32, 2, 128, 96},
		{37, 2, 128, 64},
		{0x1400, 4, 256, 128},
		{96, 8, 512, 64},
	};

	struct TexaCase
	{
		u32 aem;
		u32 ta0;
	};
	static constexpr TexaCase kTexa[] = {{0, 0x00}, {0, 0x80}, {1, 0x80}, {1, 0x00}, {1, 0x40}};

	std::vector<u8> cpu;
	for (const u32 psm : {static_cast<u32>(PSMCT32), static_cast<u32>(PSMCT24)})
	{
		const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
		const u32 src_fmt = (psm == PSMCT24) ? 1u : 0u;
		for (const Window& w : kWindows)
		{
			// The op's pages-per-row, exactly as the renderer computes it for a direct-colour
			// window.
			const u32 op_bw = std::max<u32>(w.tbw, 1);
			const u32 pitch = static_cast<u32>(w.tw) * 4;
			cpu.assign(pitch * static_cast<u32>(w.th), 0);

			for (const TexaCase& tc : kTexa)
			{
				GIFRegTEXA TEXA = {};
				TEXA.AEM = tc.aem;
				TEXA.TA0 = tc.ta0;
				TEXA.TA1 = 0;

				const GSOffset off = s_mem->GetOffset(w.tbp0, w.tbw, psm);
				p.rtx(*s_mem, off, GSVector4i(0, 0, w.tw, w.th), cpu.data(), static_cast<int>(pitch), TEXA);

				const u32 texa_word = PackTexa(TEXA);
				for (int v = 0; v < w.th; v++)
				{
					const u32* const row = reinterpret_cast<const u32*>(cpu.data() + static_cast<u32>(v) * pitch);
					for (int u = 0; u < w.tw; u++)
					{
						const u32 addr = MaterialiseWord(f, static_cast<u32>(u), static_cast<u32>(v), w.tbp0, op_bw);
						const u32 got = MaterialiseTexel(vm[addr], src_fmt, texa_word);
						ASSERT_EQ(got, row[u])
							<< "psm=" << psm << " tbp0=" << w.tbp0 << " tbw=" << w.tbw << " u=" << u << " v=" << v
							<< " AEM=" << tc.aem << " TA0=" << tc.ta0;
					}
				}

				// PSMCT32 ignores TEXA entirely; one pass over it is the whole story.
				if (src_fmt == 0)
					break;
			}
		}
	}
}
