// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string>

// The GS page swizzle as closed forms a shader can evaluate.
//
// Every intra-page swizzle table in GSTables.cpp — blockTable32/8/4 and
// columnTable32/8/4 — is a GF(2)-LINEAR map of the coordinate bits: each output bit
// is the XOR of a fixed subset of the input bits, with no constant term (measured
// across the whole of every table by the vkswz probe, umbrella gpu-drivers, and
// re-fitted here at runtime). So a fragment shader that must address GS memory —
// the Tile renderer's device-side reinterpretation of one format's window over
// another format's target — needs no table on the GPU: an address is a page term (a
// multiply-add, the one non-linear part) plus a handful of mask-and-XOR terms, and
// the inverse of a bijective linear map is linear too, so the owner side (word in
// page → pixel) is the same shape.
//
// The forms are FITTED from the tree's own tables at runtime rather than transcribed:
// the basis of a linear map is the image of the unit vectors, and linearity is then
// checked over the entire domain. A table that ever stops fitting (an upstream table
// change, a new format) makes the set invalid, which turns the device route off
// rather than letting it address the wrong bytes. The fit is handed to the shader as
// a block of #defines prepended to its source, so the GPU and the CPU readers can
// only ever disagree about arithmetic the unit suite pins — never about a constant.
namespace GSTileSwizzleForms
{
	/// out(x, y) = XOR over set bits b of x of x_basis[b], XOR over set bits b of y of y_basis[b].
	struct XorForm2
	{
		u32 x_basis[6] = {};
		u32 y_basis[6] = {};
		u32 x_bits = 0;
		u32 y_bits = 0;

		constexpr u32 Eval(u32 x, u32 y) const
		{
			u32 v = 0;
			for (u32 b = 0; b < x_bits; b++)
				v ^= ((x >> b) & 1u) ? x_basis[b] : 0u;
			for (u32 b = 0; b < y_bits; b++)
				v ^= ((y >> b) & 1u) ? y_basis[b] : 0u;
			return v;
		}
	};

	/// out(v) = XOR over set bits b of v of basis[b].
	struct XorForm1
	{
		u32 basis[10] = {};
		u32 in_bits = 0;

		constexpr u32 Eval(u32 v) const
		{
			u32 r = 0;
			for (u32 b = 0; b < in_bits; b++)
				r ^= ((v >> b) & 1u) ? basis[b] : 0u;
			return r;
		}
	};

	/// Fit f over [0, 2^x_bits) × [0, 2^y_bits) as a linear form. False if f(0,0) != 0 or
	/// f is not linear anywhere on the domain — the whole domain is checked, not sampled.
	template <typename F>
	bool Fit2(F&& f, u32 x_bits, u32 y_bits, XorForm2& out)
	{
		if (x_bits > 6 || y_bits > 6 || f(0u, 0u) != 0u)
			return false;
		out = {};
		out.x_bits = x_bits;
		out.y_bits = y_bits;
		for (u32 b = 0; b < x_bits; b++)
			out.x_basis[b] = f(1u << b, 0u);
		for (u32 b = 0; b < y_bits; b++)
			out.y_basis[b] = f(0u, 1u << b);
		for (u32 y = 0; y < (1u << y_bits); y++)
		{
			for (u32 x = 0; x < (1u << x_bits); x++)
			{
				if (out.Eval(x, y) != f(x, y))
					return false;
			}
		}
		return true;
	}

	/// Fit f over [0, 2^in_bits) as a linear form on one input. Same contract as Fit2.
	template <typename F>
	bool Fit1(F&& f, u32 in_bits, XorForm1& out)
	{
		if (in_bits > 10 || f(0u) != 0u)
			return false;
		out = {};
		out.in_bits = in_bits;
		for (u32 b = 0; b < in_bits; b++)
			out.basis[b] = f(1u << b);
		for (u32 v = 0; v < (1u << in_bits); v++)
		{
			if (out.Eval(v) != f(v))
				return false;
		}
		return true;
	}

	/// The inverse of a bijective form on (x, y) → v, as a form on v's (x_bits + y_bits)
	/// bits producing the packed coordinate x | (y << x_bits). False if fwd is not a
	/// bijection onto [0, 2^(x_bits+y_bits)) or its inverse is not linear.
	bool Invert2(const XorForm2& fwd, XorForm1& out);

	/// The complete set the reinterpretation shader needs.
	struct FormSet
	{
		bool valid = false;
		XorForm2 block48; ///< blockTable32 (== blockTable8): (bx 0..7, by 0..3) → in-page block 0..31
		XorForm2 block84; ///< blockTable4 (== blockTable16): (bx 0..3, by 0..7) → in-page block 0..31
		XorForm2 block84s; ///< blockTable16S: (bx 0..3, by 0..7) → in-page block 0..31
		XorForm2 col32; ///< columnTable32: (x 0..7, y 0..7) → word in block 0..63
		XorForm2 col16; ///< columnTable16: (x 0..15, y 0..7) → HALFWORD in block 0..127
		XorForm2 col8; ///< columnTable8: (x 0..15, y 0..15) → byte in block 0..255
		XorForm2 col4; ///< columnTable4: (x 0..31, y 0..15) → nibble in block 0..511
		XorForm1 inv_block48; ///< in-page block 0..31 → bx | (by << 3)
		XorForm1 inv_col32; ///< word in block 0..63 → cx | (cy << 3)

		/// The CSM1 32-bit CLUT loaders' word order (GSClut::EntryToWordCSM1_32): entry
		/// e of an eight-bit palette at CSA 0 → source word 0..255; entry e of a
		/// four-bit palette → source word 0..15. Fitted separately (`clut_valid`) so a
		/// loader change turns off only the device-side CLUT gather.
		bool clut_valid = false;
		XorForm1 clut_i8_word;
		XorForm1 clut_i4_word;
	};

	/// Fits every form from the tree's tables. `valid` is false if any swizzle table
	/// fails to fit, or blockTable8 is no longer blockTable32, or blockTable16 is no longer
	/// blockTable4 (the shaders share one form for each pair); `clut_valid` is false if the
	/// CLUT loaders' word order does not fit.
	FormSet Fit();

	/// The fitted constants as GLSL `#define`s (one per basis entry, zero entries
	/// included so the shader's expression is total), for prepending to shader source.
	std::string ShaderDefines(const FormSet& forms);

	// -- The reinterpretation arithmetic, on the CPU -----------------------------------
	// The exact computation the shader performs per output texel, so the unit suite can
	// pin it against GSOffset's own address functions. Kept here rather than in a test
	// because it is the specification the GLSL transcribes.

	/// Source-side texel formats the shader distinguishes.
	enum class IndexFormat : u8
	{
		P8 = 0, ///< PSMT8: 128×64 pages, byte per texel
		P4 = 1, ///< PSMT4: 128×128 pages, nibble per texel
		P8H = 2, ///< PSMT8H: CT32 layout, the alpha byte
		P4HL = 3, ///< PSMT4HL: CT32 layout, low nibble of the alpha byte
		P4HH = 4, ///< PSMT4HH: CT32 layout, high nibble of the alpha byte
	};

	/// Maps a GS PSM to the shader's format, or -1 for one it does not serve.
	int IndexFormatFor(u32 psm);

	/// Pages per texture row for a window of `psm` at TEX0.TBW — the page term every
	/// address form below (and every TileGpu shader) multiplies the row index by. This
	/// is GSOffset's own `bw >> (pageShiftX - 6)`: a 64-texel-wide page (the CT32 family,
	/// which the alpha-byte views PSMT8H/PSMT4HL/PSMT4HH share) takes TBW unchanged, a
	/// 128-wide one (PSMT8/PSMT4) halves it.
	///
	/// ⚠️ A format's page WIDTH decides this, not whether it has a palette. The three
	/// alpha-byte views have a palette and CT32 geometry, so halving their TBW would
	/// address the wrong page in every row but the first.
	///
	/// GSOffset's value exactly, TBW=0 included -- no floor at one page. TileGpu's
	/// direct-32 road applies a max(TBW, 1) of its own at its call site; that floor is
	/// older than this helper and is left where it is rather than folded in here, because
	/// folding it in would silently give every format a page term the CPU readers do not
	/// have.
	u32 PagesPerRow(u32 psm, u32 tbw);

	struct Reinterpretation
	{
		u32 src_bp; ///< block-granular base of the index window
		u32 src_bwpg; ///< the window's width in PAGES, exactly as GSOffset computes it (bw >> (pageShiftX - 6))
		u32 dst_bp; ///< the owner surface's base (page-aligned)
		u32 dst_bwpg; ///< the owner's width in pages
	};

	struct OwnerTexel
	{
		u32 x, y; ///< owner-texture texel (linear-from-base-page pixel space)
		u32 byte; ///< which byte of the RGBA cell (0 = R … 3 = A)
		u32 nibble; ///< which nibble of that byte (P4/P4HL/P4HH), 0 = low
		bool in_range; ///< the block resolved to a non-negative owner offset
	};

	/// The shader's per-texel arithmetic: index texel (u, v) → the owner texel and byte
	/// that hold it. Depends only on the forms and the two layouts.
	///
	/// ⚠️ The OWNER side is CT32-shaped — 64×32 pages of 8×8 blocks, one word per texel — which is
	/// why the roads that use this refuse a 16-bit owner (`gsTileSurfaceHasCt32PixelSpace`). A
	/// 16-bit owner's texture is a different pixel space, and asking this about one gives an answer
	/// that looks reasonable and reads the wrong bytes.
	OwnerTexel Locate(const FormSet& forms, IndexFormat fmt, const Reinterpretation& r, u32 u, u32 v);

	// -- The 16-bit COLOUR byte road ---------------------------------------------------------
	// PSMCT16/PSMCT16S surfaces are 64×64-texel pages of 16×8-texel blocks, two texels to a
	// 32-bit word. tilegpu_writeback.glsl and tilegpu_seed.glsl address them with the forms above
	// (block84 for CT16, block84s for CT16S, col16 for both); this is the same arithmetic on the
	// CPU, kept here rather than in a test because it is the specification the GLSL transcribes.

	struct Texel16
	{
		u32 page; ///< guest page, already wrapped into [0, GS_MAX_PAGES)
		u32 word_in_page; ///< 0..2047 — block-in-page * 64 + halfword-in-block / 2
		u32 half; ///< 0 = low halfword of that word, 1 = high
	};

	/// Where texel (x, y) of a 16-bit colour surface lives, in the surface's own linear-from-base
	/// pixel space. `psm` must be PSMCT16 or PSMCT16S, `bp` page-aligned and `bw` non-zero — the
	/// byte road's own admission conditions; false otherwise, and `out` is left zeroed.
	bool Locate16(const FormSet& forms, u32 psm, u32 bp, u32 bw, u32 x, u32 y, Texel16& out);

	// -- The CLUT block copy: a palette read out of an owner target WITHOUT a gather pass ---
	// A CSM1 32-bit CLUT load reads 256 contiguous words at CBP (four blocks) or 16 (the
	// first quarter of one block). When those words are in a render target's texture, the
	// cheapest way to put them where a byte-road draw can read them is not a gather pass per
	// load — at a thousand loads a frame that is a thousand render passes — but a plain
	// image-to-buffer COPY of the blocks, block by block, into the frame's palette stream.
	// The copy lands each block's 64 words in TEXEL ROW-MAJOR order, which is not entry
	// order, so the consumer applies the CSM1 entry→word order at fetch time instead.

	struct ClutBlockCopy
	{
		static constexpr u32 kMaxRegions = 4;
		u32 region_count; ///< 4 for a 256-entry palette (four blocks), 1 for a 16-entry one
		u32 x[kMaxRegions], y[kMaxRegions]; ///< each region's texel origin in the owner's pixel space
		u32 w, h; ///< 8×8 per region; 8×2 for the single block of a 16-entry palette
	};

	/// Where a CSM1 32-bit CLUT load's source blocks sit in the owner surface's pixel space.
	/// `entries` is 256 or 16; `owner_bp`/`owner_bwpg` are the owner's page-aligned base and
	/// its width in pages. False when the forms did not fit, the entry count is not one of the
	/// two, or a block falls below the owner's base (the caller must have proved CBP ≥ owner_bp).
	bool LocateClutBlocks(const FormSet& forms, u32 cbp, u32 owner_bp, u32 owner_bwpg, u32 entries,
		ClutBlockCopy& out);

	/// Entry e of a CSM1 32-bit palette → its word offset in the image LocateClutBlocks
	/// describes, once its regions have been copied out in order, each row-major. This is the
	/// arithmetic tilegpu.glsl's palette-from-copied-block mode performs per fetch.
	u32 ClutEntryToCopyOffset(const FormSet& forms, u32 entries, u32 e);
} // namespace GSTileSwizzleForms
