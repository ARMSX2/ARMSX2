// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSVertex.h"
#include "common/Pcsx2Defs.h"

// Setting or clearing alpha bit 7 with the colour output stage's logic op instead of a read of the
// render target.
//
// Games use bit 7 of the frame's alpha as a one-bit stencil: a draw with FBMSK 0x7FFFFFFF on a
// 32-bit frame writes only that bit, a later draw tests it with the destination alpha test, and a
// further masked draw writes it back. A mask that keeps part of a channel is emulated in the pixel
// shader, which reads the target. Where that read costs a wait per draw (our generation-2 Turnip on
// Adreno 7xx: the driver cleans the colour cache and waits for idle before every draw that declares
// the feedback loop), Indiana Jones pays it on 292 draws a frame.
//
// Writing one bit while keeping the others is a bitwise operation, and Vulkan's logic op does it on
// the stored bits of a UNORM target. With the shader's alpha at exactly 0x80 (FBA) and only alpha
// written:
//   set   = OR:           0x80 | d
//   clear = AND_INVERTED: ~0x80 & d
// Bits 0..6 of the stored alpha are kept, as the mask keeps them.
//
// Which op a primitive needs comes from its alpha: bit 7 set (or FBA on) sets, clear clears. One
// draw may do both -- Indiana Jones' marks set the bit on some triangles and clear it on the rest,
// in two consecutive runs -- so the draw is issued as up to two runs in submission order, each with
// its own op. The ops within a run are idempotent and commutative, and separate draws are rasterised
// in order, so the result matches the PS2 for any overlap. Three or more runs keep the read.
namespace GSAlphaBitLogicOp
{
	enum : u8
	{
		Off = 0, ///< No logic op (not None or Opposite: X11 defines both as macros).
		SetBit = 1, ///< VK_LOGIC_OP_OR, source alpha 0x80.
		ClearBit = 2, ///< VK_LOGIC_OP_AND_INVERTED, source alpha 0x80.
	};

	constexpr u8 OtherOp(u8 op)
	{
		return (op == SetBit) ? ClearBit : SetBit;
	}

	/// A device takes this road where the logic op exists, where the read it replaces costs a wait
	/// per draw, and where the colour write mask is honoured under a depth test (a driver that wrote
	/// RGB anyway would write ~0 & d = d on a clear, but 0x?? | d on a set with a non-black colour).
	constexpr bool DeviceQualifies(bool logic_op_feature, bool read_costs_per_draw, bool broken_colormask_with_depth)
	{
		return logic_op_feature && read_costs_per_draw && !broken_colormask_with_depth;
	}

	/// How a qualifying draw is issued: `first_op` for the first `first_indices` indices and the
	/// opposite op for the rest. `first_indices == 0` means one run with `first_op` covers the draw.
	/// `first_op == Off` means the draw does not qualify.
	struct Runs
	{
		u8 first_op = Off;
		u32 first_indices = 0;
	};

	/// Triangle lists only. Every primitive must give one op over all its pixels: its alpha has
	/// bits 0..6 clear (the shader's alpha must come out as exactly 0x80 after FBA), and under
	/// Gouraud shading its three vertices agree; flat shading takes the last vertex, the PS2's
	/// provoking vertex.
	inline Runs ClassifyTriangles(const GSVertex* verts, const u16* indices, u32 nindices, bool iip, bool fba)
	{
		if (nindices == 0 || (nindices % 3) != 0)
			return {};

		Runs runs;
		u8 current = Off;
		for (u32 i = 0; i < nindices; i += 3)
		{
			const u8 a = verts[indices[i + 2]].RGBAQ.A;
			if (iip && (verts[indices[i]].RGBAQ.A != a || verts[indices[i + 1]].RGBAQ.A != a))
				return {};
			if ((a & 0x7F) != 0)
				return {};

			const u8 op = (fba || (a & 0x80)) ? SetBit : ClearBit;
			if (current == Off)
			{
				runs.first_op = op;
			}
			else if (op != current)
			{
				if (runs.first_indices != 0)
					return {};
				runs.first_indices = i;
			}
			current = op;
		}
		return runs;
	}

	// The destination-alpha test that follows the marks. On the per-draw-wait road it reads the
	// target too. Stencil DATE does not, but its copy of "does this pixel pass" into the stencil
	// buffer ends the render pass. That copy stays true for the rest of the pass while every draw
	// that writes the target's alpha keeps it true: a DATE draw whose written alpha keeps bit 7 on
	// the passing side, and a logic-op mark that writes the stencil where it writes the bit. So a run
	// of DATE draws interleaved with marks (Indiana Jones: 291 a frame in one pass) pays for one copy.
	// Any other alpha write, a stencil clear, or the end of the pass drops the copy.
	enum DateCopy : u8
	{
		NoDateCopy = 0,
		UsesDateCopy = 1,
	};

	/// Whether a DATE draw leaves every pixel it writes passing, so the copy is as true after it as
	/// before. It writes only where the test passed, so it is enough that the alpha it writes keeps
	/// bit 7 at DATM.
	constexpr bool KeepsDATEResult(bool datm, bool alpha_write, int alpha_min, int alpha_max, bool fba)
	{
		if (!alpha_write)
			return true;
		return datm ? (fba || alpha_min >= 0x80) : (!fba && alpha_max < 0x80);
	}

	/// The stencil value a mark leaves where it writes, for a copy built for `datm` (1 = passes).
	constexpr u8 StencilWriteFor(u8 op, bool datm)
	{
		return ((op == SetBit) == datm) ? 2 : 1; // DepthStencilSelector::alpha_bit_stencil
	}
} // namespace GSAlphaBitLogicOp
