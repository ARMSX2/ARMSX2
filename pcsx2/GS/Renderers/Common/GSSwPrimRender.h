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

// Per-renderer scratch for running a draw through the software scanline core. Each renderer that
// uses the path owns one.
//
// The rasterizer is type-erased on purpose: GSSingleRasterizer is declared inside
// MULTI_ISA_UNSHARED_START, and this header is included by translation units compiled ONCE in the
// x86 multi-ISA configuration. Only the multi-ISA implementation may name the concrete type.
struct GSSwPrimRenderState
{
	std::vector<GSVertexSW> vertex_buffer;
	std::unique_ptr<GSTextureCacheSW::Texture> texture[7 + 1];
	std::unique_ptr<GSVirtualAlignedClass<32>> rasterizer;
};

// The rectangle the scanline core walks and the caller accounts for in guest memory. One
// definition, computed by the caller and passed in, so decisions made before the draw match what
// the core writes; two versions would disagree on a pixel at a page boundary.
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
// into the renderer's GSLocalMemory. Returns false when the draw writes nothing (24-bit DATE, or
// neither colour nor depth survives the masks); the caller then owes no bookkeeping.
//
// Renderer-agnostic: reads only GSState. It does not report which bytes landed; the callers do
// that bookkeeping themselves.
MULTI_ISA_DEF(bool GSSwPrimRenderRun(GSRenderer& renderer, GSSwPrimRenderState& sw, const GSVector4i& bbox);)
