// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

/// The per-draw oracle: the software rasterizer run in lockstep against the Tile
/// renderer's native route, one draw at a time, both arms starting from byte-identical
/// inputs.
///
/// Frame scoring answers "how much of this frame is wrong". It cannot answer "which
/// draw broke it", and every attribution round on the native path so far has had to
/// infer that from percentages -- which is how a class ends up with a mechanism written
/// onto it that measurement later excludes. This names the draw.
///
/// The mechanism lives in GSRendererTile::Draw: sync the draw's inputs to CPU truth,
/// snapshot the write footprint, run the native draw, read its result back, restore the
/// snapshot, run GSRendererSW over the same inputs, compare. Because both arms start
/// from the same bytes, a divergence is authored by THAT draw and never inherited from
/// an earlier one -- which is what makes the ledger readable all the way down rather
/// than only at its first row.
///
/// The comparison is three-way -- against the pre-draw state as well as between the
/// arms -- and that split is the instrument's whole point:
///
///   - both arms wrote, values differ  -> ARITHMETIC (shading, blending, sampling)
///   - SW wrote, native left it alone  -> coverage lost, or a page/plane the native
///                                        route never claimed
///   - native wrote, SW left it alone  -> coverage gained, or a claim reaching too far
///
/// A single "pixels differing" percentage sums three unrelated defects, and the
/// campaign has twice spent a round attributing the sum to one of them.
namespace GSTileOracle
{
	/// How to price the gap between two stored pixels. The unit reported is an 8-bit
	/// level for both colour metrics, so a 16-bit destination's numbers stay comparable
	/// with a 32-bit one's (and with the frame scorer's threshold).
	enum class Metric : u8
	{
		Rgba8, ///< PSMCT32 / PSMCT24: four 8-bit lanes
		Rgba5551, ///< PSMCT16 / PSMCT16S: 5/5/5/1, deltas scaled up to 8-bit levels
		Raw, ///< depth: one unsigned integer, reported in its own units
	};

	/// One rect's divergence for one plane, decomposed.
	struct Tally
	{
		u32 compared = 0; ///< pixels examined
		u32 sw_only = 0; ///< SW moved the pixel off `pre`, the native route did not
		u32 gpu_only = 0; ///< the native route moved it, SW did not
		u32 both = 0; ///< both moved it, to different values
		u32 max_delta = 0; ///< worst gap between the arms, in the metric's units

		/// The first pixel the two arms disagree on, scanning rows then columns.
		/// x is -1 when they agree everywhere.
		s32 first_x = -1;
		s32 first_y = -1;
		u32 first_pre = 0;
		u32 first_sw = 0;
		u32 first_gpu = 0;

		u32 differing() const { return sw_only + gpu_only + both; }
	};

	/// Compare one rect of one plane. The three arrays hold the same pixels read out of
	/// the three memory states, row-major over w*h starting at (x0, y0) -- x0/y0 are
	/// carried only so the reported first-divergent pixel is in draw coordinates.
	///
	/// Pure: no device, no renderer state. `gs_tile_oracle_tests` sweeps it.
	Tally Compare(const u32* pre, const u32* sw, const u32* gpu, int w, int h, int x0, int y0, Metric metric);

	/// The metric a destination format's stored pixels are priced under.
	Metric MetricForPsm(u32 psm);

	/// Whether the oracle is recording. Read per draw, so it stays a config bit test.
	bool IsActive();

	/// One compared draw. Both arms ran; the tallies say whether they agreed.
	struct Row
	{
		u32 frame;
		u32 draw;
		u8 prim_type; ///< PRIM->PRIM, so the ledger names it the way the drawlog does
		u16 prim_count;
		s16 rect_x, rect_y, rect_z, rect_w;

		u32 fb_bp;
		u8 fb_bw;
		u8 fb_psm;
		u32 fbmsk;
		u8 colormask;

		u32 z_bp;
		u8 z_psm;
		u8 z_write;
		u8 ztst;

		/// Draw shape, so a divergent population can be sorted without joining the
		/// drawlog. Bit 0 TME, 1 IIP, 2 ABE, 3 FGE, 4 mipmapped, 5 FST, 6 ATE, 7 DATE.
		u8 shape;
		u8 tex_psm;

		/// Pages the input sync had to pull before the SW arm could run. ZERO MEANS THE
		/// ROW IS PERTURBATION-FREE: the oracle changed nothing about this draw's inputs,
		/// so its verdict cannot be an artifact of the instrument's own synchronisation.
		/// Non-zero rows are still evidence, but they are the ones to re-check first.
		u16 sync_pages;

		u16 fp_pages; ///< pages in the compared write footprint
		u16 diff_pages; ///< footprint pages differing between the arms, anywhere
		u32 diff_bytes; ///< bytes differing between the arms across the whole footprint

		Tally colour;
		Tally depth;

		bool diverged() const { return colour.differing() != 0 || depth.differing() != 0 || diff_bytes != 0; }
	};

	void AddRow(const Row& row);

	/// Per-pixel detail for a divergent draw, written beside the ledger. Bounded: a
	/// microscope that dumps every pixel of every bad draw produces a file nobody opens.
	/// Returns how many pixel rows this draw may still record (0 = quota spent).
	u32 OpenPixelDetail();
	void AddPixel(u32 frame, u32 draw, bool depth, s32 x, s32 y, u32 pre, u32 sw, u32 gpu);

	/// Serialises the ledger to `path` and the pixel detail to `<path>.pixels.csv`.
	bool WriteCSV(const std::string& path);

	size_t GetRowCount();
	size_t GetDivergentCount();
	bool WasTruncated();
	void Reset();
} // namespace GSTileOracle
