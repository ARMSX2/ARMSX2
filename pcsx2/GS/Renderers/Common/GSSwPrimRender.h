// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSAlignedClass.h"
#include "GS/GSVector.h"
#include "GS/MultiISA.h"
#include "GS/Renderers/Common/GSVertexTrace.h"
#include "GS/Renderers/SW/GSTextureCacheSW.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include <memory>
#include <vector>

class GSRenderer;

// The scratch one renderer needs to run a draw through the software scanline core, and nothing
// else. It used to be three members of GSRendererHW; it is a struct so a second renderer taking
// the same road owns its own scratch rather than growing a separate copy of the setup.
//
// The rasterizer is type-erased on purpose: GSSingleRasterizer is declared inside
// MULTI_ISA_UNSHARED_START, and this header is included by translation units that are compiled
// ONCE in the x86 multi-ISA configuration. The concrete type is recovered in the multi-ISA
// implementation, which is the only place that may name it.
struct GSSwPrimRenderState
{
	std::vector<GSVertexSW> vertex_buffer;
	std::unique_ptr<GSTextureCacheSW::Texture> texture[7 + 1];
	std::unique_ptr<GSVirtualAlignedClass<32>> rasterizer;

	// What rebuilding the software texture cost the draw that just ran, summed over the
	// mip levels it touched: bytes a buffer allocation cleared, and blocks unswizzled out
	// of local memory. Per draw, because the road rebuilds the texture on every one.
	u32 last_tex_clear_bytes = 0;
	u32 last_tex_blocks = 0;
};

// The rectangle the scanline core walks, and the rectangle the caller must account for in guest
// memory. ONE definition, called by the caller and handed back in: a caller that has to decide
// something about the pixels BEFORE the draw runs must ask the same question the core answers
// after it, and two spellings of "which pixels" would disagree by the pixel that lands on a page
// boundary.
//
// Points and lines may have a zero-area bbox (a single horizontal line is 0,0 - 256,0), so each
// degenerate axis is widened to one pixel.
inline GSVector4i GSSwPrimRenderBBox(const GSVertexTrace& vt, const GSVector4i& scissor)
{
	GSVector4i bbox = GSVector4i(vt.m_min.p.floor().xyxy(vt.m_max.p.ceil())).rintersect(scissor);

	if (vt.m_primclass == GS_POINT_CLASS || vt.m_primclass == GS_LINE_CLASS)
	{
		if (bbox.x == bbox.z)
			bbox.z++;
		if (bbox.y == bbox.w)
			bbox.w++;
	}

	return bbox;
}

MULTI_ISA_DEF(class GSSwPrimRenderFunctions;)

// Run the current draw through the software scanline core, writing native-resolution PS2 bytes
// into the renderer's own GSLocalMemory. Returns false where the draw writes nothing at all (a
// 24-bit DATE, or neither colour nor depth surviving the masks), in which case nothing was
// written and the caller owes no bookkeeping.
//
// Renderer-agnostic: everything it reads is GSState's (the vertex trace, the drawing context, the
// environment, the vertex and index buffers, the local memory). What it does NOT do is tell
// anybody the bytes landed -- the two callers learn that differently, so the bookkeeping stays
// with them.
MULTI_ISA_DEF(bool GSSwPrimRenderRun(GSRenderer& renderer, GSSwPrimRenderState& sw, const GSVector4i& bbox);)
