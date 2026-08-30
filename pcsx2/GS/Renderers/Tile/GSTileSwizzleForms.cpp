// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTileSwizzleForms.h"

#include "GS/GSClut.h"
#include "GS/GSLocalMemory.h"
#include "GS/GSRegs.h"
#include "GS/GSTables.h"

#include "fmt/format.h"

#include <vector>

namespace GSTileSwizzleForms
{
	bool Invert2(const XorForm2& fwd, XorForm1& out)
	{
		const u32 in_bits = fwd.x_bits + fwd.y_bits;
		if (in_bits > 10)
			return false;
		const u32 domain = 1u << in_bits;

		// The inverse table, checking the forward map is a bijection onto [0, domain).
		std::vector<u32> inv(domain, 0xFFFFFFFFu);
		for (u32 y = 0; y < (1u << fwd.y_bits); y++)
		{
			for (u32 x = 0; x < (1u << fwd.x_bits); x++)
			{
				const u32 v = fwd.Eval(x, y);
				if (v >= domain || inv[v] != 0xFFFFFFFFu)
					return false;
				inv[v] = x | (y << fwd.x_bits);
			}
		}

		out = {};
		out.in_bits = in_bits;
		for (u32 b = 0; b < in_bits; b++)
			out.basis[b] = inv[1u << b];
		for (u32 v = 0; v < domain; v++)
		{
			if (out.Eval(v) != inv[v])
				return false;
		}
		return true;
	}

	FormSet Fit()
	{
		FormSet f;
		bool ok = true;

		// blockTable8 must BE blockTable32, and blockTable16 must BE blockTable4 — the shaders
		// evaluate one form for each pair, so a table that stops agreeing has to invalidate the set
		// rather than quietly address the wrong block.
		for (u32 by = 0; by < 4 && ok; by++)
			for (u32 bx = 0; bx < 8; bx++)
				ok &= blockTable32.lookup(bx, by) == blockTable8.lookup(bx, by);
		for (u32 by = 0; by < 8 && ok; by++)
			for (u32 bx = 0; bx < 4; bx++)
				ok &= blockTable4.lookup(bx, by) == blockTable16.lookup(bx, by);

		ok &= Fit2([](u32 bx, u32 by) { return static_cast<u32>(blockTable32.lookup(bx, by)); }, 3, 2, f.block48);
		ok &= Fit2([](u32 bx, u32 by) { return static_cast<u32>(blockTable4.lookup(bx, by)); }, 2, 3, f.block84);
		ok &= Fit2([](u32 bx, u32 by) { return static_cast<u32>(blockTable16S.lookup(bx, by)); }, 2, 3, f.block84s);
		ok &= Fit2([](u32 x, u32 y) { return static_cast<u32>(columnTable32[y][x]); }, 3, 3, f.col32);
		ok &= Fit2([](u32 x, u32 y) { return static_cast<u32>(columnTable16[y][x]); }, 4, 3, f.col16);
		ok &= Fit2([](u32 x, u32 y) { return static_cast<u32>(columnTable8[y][x]); }, 4, 4, f.col8);
		ok &= Fit2([](u32 x, u32 y) { return static_cast<u32>(columnTable4[y][x]); }, 5, 4, f.col4);
		ok &= Invert2(f.block48, f.inv_block48);
		ok &= Invert2(f.col32, f.inv_col32);

		// The depth 16-bit formats as their colour twins plus one constant XOR. Read the constant
		// off GSOffset (bp 0, so the block number IS the XOR at the origin) and then hold the whole
		// page to the relation, both families -- a table or a blockAddressXor change that broke it
		// would otherwise send every depth-16 texture read to the wrong block with no complaint.
		{
			const GSOffset c16 = GSOffset::fromKnownPSM(0, 1, PSMCT16);
			const GSOffset z16 = GSOffset::fromKnownPSM(0, 1, PSMZ16);
			const GSOffset c16s = GSOffset::fromKnownPSM(0, 1, PSMCT16S);
			const GSOffset z16s = GSOffset::fromKnownPSM(0, 1, PSMZ16S);
			f.z16_block_xor = z16.bn(0, 0);
			for (int y = 0; y < 64 && ok; y += 8)
			{
				for (int x = 0; x < 64; x += 16)
				{
					ok &= z16.bn(x, y) == (c16.bn(x, y) ^ f.z16_block_xor);
					ok &= z16s.bn(x, y) == (c16s.bn(x, y) ^ f.z16_block_xor);
				}
			}
			if (!ok)
				f.z16_block_xor = 0;
		}

		// The same relation for the 32-bit families, asked of its own GSSwizzleInfo pair rather than
		// assumed from the 16-bit answer. Both 24-bit members ride their 32-bit twin's tables, so the
		// check covers all four formats: a blockAddressXor change on swizzle32Z alone would otherwise
		// leave the 16-bit check green while every depth-32 texture read went to the wrong block.
		{
			const GSOffset c32 = GSOffset::fromKnownPSM(0, 1, PSMCT32);
			const GSOffset z32 = GSOffset::fromKnownPSM(0, 1, PSMZ32);
			const GSOffset c24 = GSOffset::fromKnownPSM(0, 1, PSMCT24);
			const GSOffset z24 = GSOffset::fromKnownPSM(0, 1, PSMZ24);
			f.z32_block_xor = z32.bn(0, 0);
			for (int y = 0; y < 32 && ok; y += 8)
			{
				for (int x = 0; x < 64; x += 8)
				{
					ok &= z32.bn(x, y) == (c32.bn(x, y) ^ f.z32_block_xor);
					ok &= z24.bn(x, y) == (c24.bn(x, y) ^ f.z32_block_xor);
				}
			}
			// The XOR has to stay INSIDE the page, and that is load-bearing rather than incidental:
			// the writeback and the seed apply it to the in-page block index and leave the page term
			// alone, which is the same map only while the constant is under 32.
			ok &= f.z32_block_xor < 32;
			if (!ok)
				f.z32_block_xor = 0;
		}

		f.valid = ok;

		// The two 16-bit block tables' inverses, for the CLUT gather off a 16-bit owner. Their own
		// flag: `valid` gates the whole byte road, and an inverse that stopped fitting has to turn off
		// only the gather. Gated on `ok` in the other direction because these invert forms fitted
		// above -- Invert2 of a form that did not fit succeeds trivially on a zero-bit domain and
		// answers "block 0" for every block, which is worse than refusing.
		bool clut16_ok = ok;
		clut16_ok &= Invert2(f.block84, f.inv_block84);
		clut16_ok &= Invert2(f.block84s, f.inv_block84s);
		f.clut16_valid = clut16_ok;

		u16 i8[256];
		u16 i4[16];
		GSClut::EntryToWordCSM1_32(i8, i4);
		bool clut_ok = Fit1([&](u32 e) { return static_cast<u32>(i8[e]); }, 8, f.clut_i8_word);
		clut_ok &= Fit1([&](u32 e) { return static_cast<u32>(i4[e]); }, 4, f.clut_i4_word);
		f.clut_valid = clut_ok;
		return f;
	}

	std::string ShaderDefines(const FormSet& forms)
	{
		std::string s;
		const auto form2 = [&s](const char* name, const XorForm2& f) {
			for (u32 b = 0; b < 6; b++)
				s += fmt::format("#define TILE_SWZ_{}_X{} 0x{:x}u\n", name, b, b < f.x_bits ? f.x_basis[b] : 0u);
			for (u32 b = 0; b < 6; b++)
				s += fmt::format("#define TILE_SWZ_{}_Y{} 0x{:x}u\n", name, b, b < f.y_bits ? f.y_basis[b] : 0u);
		};
		const auto form1 = [&s](const char* name, const XorForm1& f) {
			for (u32 b = 0; b < 10; b++)
				s += fmt::format("#define TILE_SWZ_{}_{} 0x{:x}u\n", name, b, b < f.in_bits ? f.basis[b] : 0u);
		};
		form2("B48", forms.block48);
		form2("B84", forms.block84);
		form2("B84S", forms.block84s);
		form2("C32", forms.col32);
		form2("C16", forms.col16);
		form2("C8", forms.col8);
		form2("C4", forms.col4);
		form1("IB48", forms.inv_block48);
		form1("IC32", forms.inv_col32);
		form1("CLUT8", forms.clut_i8_word);
		form1("CLUT4", forms.clut_i4_word);
		// Not forms: the two constants that turn a colour block address into its depth twin's, one per
		// storage width. Separate defines because they are separate facts -- see the FormSet fields.
		s += fmt::format("#define TILE_SWZ_Z16XOR 0x{:x}u\n", forms.z16_block_xor);
		s += fmt::format("#define TILE_SWZ_Z32XOR 0x{:x}u\n", forms.z32_block_xor);
		return s;
	}

	int IndexFormatFor(u32 psm)
	{
		switch (psm)
		{
			case PSMT8:
				return static_cast<int>(IndexFormat::P8);
			case PSMT4:
				return static_cast<int>(IndexFormat::P4);
			case PSMT8H:
				return static_cast<int>(IndexFormat::P8H);
			case PSMT4HL:
				return static_cast<int>(IndexFormat::P4HL);
			case PSMT4HH:
				return static_cast<int>(IndexFormat::P4HH);
			default:
				return -1;
		}
	}

	int Direct32FormatFor(u32 psm)
	{
		switch (psm)
		{
			case PSMCT32:
			case PSMCT24:
				return static_cast<int>(Direct32Format::CT32);
			case PSMZ32:
			case PSMZ24:
				return static_cast<int>(Direct32Format::Z32);
			default:
				return -1;
		}
	}

	int Direct16FormatFor(u32 psm)
	{
		switch (psm)
		{
			case PSMCT16:
				return static_cast<int>(Direct16Format::CT16);
			case PSMCT16S:
				return static_cast<int>(Direct16Format::CT16S);
			case PSMZ16:
				return static_cast<int>(Direct16Format::Z16);
			case PSMZ16S:
				return static_cast<int>(Direct16Format::Z16S);
			default:
				return -1;
		}
	}

	bool Address16(const FormSet& forms, u32 psm, u32 tbp0, u32 tbw, u32 u, u32 v, u32& byte_addr)
	{
		byte_addr = 0;
		const int fmt = Direct16FormatFor(psm);
		if (!forms.valid || fmt < 0)
			return false;

		// The two bits of the format number: bit 0 picks the strided block table, bit 1 the depth
		// XOR. The shader reads them the same way, off the state row's index_format minus six.
		const bool strided = (fmt & 1) != 0;
		const u32 zxor = (fmt & 2) != 0 ? forms.z16_block_xor : 0u;

		// A 16-bit page is 64x64 texels of 16x8-texel blocks; the block XOR lands on the absolute
		// block, after the base and the page term, exactly as GSOffset's bn() applies it.
		const u32 page = (v >> 6) * tbw + (u >> 6);
		const u32 blk = ((tbp0 + page * 32 + (strided ? forms.block84s.Eval((u >> 4) & 3, (v >> 3) & 7) :
														forms.block84.Eval((u >> 4) & 3, (v >> 3) & 7))) ^
							zxor) %
						GS_MAX_BLOCKS;
		byte_addr = blk * 256 + forms.col16.Eval(u & 15, v & 7) * 2;
		return true;
	}

	bool Address32(const FormSet& forms, u32 psm, u32 tbp0, u32 tbw, u32 u, u32 v, u32& byte_addr)
	{
		byte_addr = 0;
		const int fmt = Direct32FormatFor(psm);
		if (!forms.valid || fmt < 0)
			return false;

		// One bit of format number, and it is the depth XOR. A CT32 page is 64x32 texels of 8x8-texel
		// blocks, one word per texel; the XOR lands on the ABSOLUTE block, after the base and the page
		// term, exactly as GSOffset's bn() applies it -- distributing it into the in-page index would
		// be right only for a page-aligned TBP0, and a texture window's often is not.
		const u32 zxor = (fmt != static_cast<int>(Direct32Format::CT32)) ? forms.z32_block_xor : 0u;
		const u32 page = (v >> 5) * tbw + (u >> 6);
		const u32 blk = ((tbp0 + page * 32 + forms.block48.Eval((u >> 3) & 7, (v >> 3) & 3)) ^ zxor) % GS_MAX_BLOCKS;
		byte_addr = blk * 256 + forms.col32.Eval(u & 7, v & 7) * 4;
		return true;
	}

	u32 PagesPerRow(u32 psm, u32 tbw)
	{
		// GSOffset: bw >> (pageShiftX - 6), pageShiftX = ilog2(page width in texels). Read off
		// the psm table rather than listed per format, so a format whose page geometry the table
		// already knows cannot be given the wrong term by being left out of a switch -- which is
		// exactly how the alpha-byte views would have been served TBW >> 1 for sharing the word
		// "paletted" with PSMT8.
		return (GSLocalMemory::m_psm[psm].pgs.x >= 128) ? (tbw >> 1) : tbw;
	}

	OwnerTexel Locate(const FormSet& forms, IndexFormat fmt, const Reinterpretation& r, u32 u, u32 v)
	{
		// Source side: the absolute block the texel lives in (base + page term + the
		// in-page block form) and the unit within that block (the column form). This is
		// GSOffset's own decomposition — bn() and the intra-block column table — with the
		// page term as GSOffset computes it: pages per row is bw >> (pageShiftX - 6).
		u32 blk;
		u32 byte_in_block;
		u32 nibble = 0;
		switch (fmt)
		{
			case IndexFormat::P8:
			{
				const u32 page = (v >> 6) * r.src_bwpg + (u >> 7);
				blk = r.src_bp + page * 32 + forms.block48.Eval((u >> 4) & 7, (v >> 4) & 3);
				byte_in_block = forms.col8.Eval(u & 15, v & 15);
				break;
			}
			case IndexFormat::P4:
			{
				const u32 page = (v >> 7) * r.src_bwpg + (u >> 7);
				blk = r.src_bp + page * 32 + forms.block84.Eval((u >> 5) & 3, (v >> 4) & 7);
				const u32 nib = forms.col4.Eval(u & 31, v & 15);
				byte_in_block = nib >> 1;
				nibble = nib & 1;
				break;
			}
			default: // the CT32-shaped palette views: the alpha byte of the cell
			{
				const u32 page = (v >> 5) * r.src_bwpg + (u >> 6);
				blk = r.src_bp + page * 32 + forms.block48.Eval((u >> 3) & 7, (v >> 3) & 3);
				byte_in_block = forms.col32.Eval(u & 7, v & 7) * 4 + 3;
				nibble = (fmt == IndexFormat::P4HH) ? 1 : 0;
				break;
			}
		}

		// Owner side: the same absolute block relative to a page-aligned CT32 owner,
		// then the inverse forms turn (block in page, word in block) into pixel
		// coordinates in the owner's linear-from-base-page pixel space.
		OwnerTexel o = {};
		const s32 rel = static_cast<s32>(blk) - static_cast<s32>(r.dst_bp);
		o.in_range = rel >= 0;
		const u32 urel = static_cast<u32>(rel < 0 ? 0 : rel);
		const u32 pg = urel >> 5;
		const u32 bip = urel & 31;
		const u32 bpk = forms.inv_block48.Eval(bip);
		const u32 word = byte_in_block >> 2;
		const u32 cpk = forms.inv_col32.Eval(word);
		const u32 bwpg = r.dst_bwpg ? r.dst_bwpg : 1;
		o.x = (pg % bwpg) * 64 + (bpk & 7) * 8 + (cpk & 7);
		o.y = (pg / bwpg) * 32 + (bpk >> 3) * 8 + (cpk >> 3);
		o.byte = byte_in_block & 3;
		o.nibble = nibble;
		return o;
	}

	bool Locate16(const FormSet& forms, u32 psm, u32 bp, u32 bw, u32 x, u32 y, Texel16& out)
	{
		out = {};
		if (!forms.valid || bw == 0 || (bp & 31) != 0 || (psm != PSMCT16 && psm != PSMCT16S))
			return false;

		// A 16-bit page is 64x64 texels; pool page (row, col) is physical page base + row*bw + col,
		// wrapping at the end of memory exactly as the shaders' `& 511` does.
		const u32 rel = (y >> 6) * bw + (x >> 6);
		out.page = ((bp >> 5) + rel) & (GS_MAX_PAGES - 1);

		// Block within the page: 16x8-texel blocks, four across and eight down. CT16 and CT16S
		// differ here and nowhere else -- the column arrangement inside a block is shared.
		const u32 bib = (psm == PSMCT16) ? forms.block84.Eval((x >> 4) & 3, (y >> 3) & 7) :
										   forms.block84s.Eval((x >> 4) & 3, (y >> 3) & 7);
		// ...and the halfword within the block's 128. Its low bit is bit 3 of x, which is what puts
		// texel x and texel x+8 in the two halves of one word.
		const u32 hw = forms.col16.Eval(x & 15, y & 7);
		out.word_in_page = bib * 64 + (hw >> 1);
		out.half = hw & 1;
		return true;
	}

	bool LocateClutBlocks(const FormSet& forms, u32 cbp, u32 owner_bp, u32 owner_bwpg, u32 entries,
		bool merge, ClutBlockCopy& out)
	{
		out = {};
		if (!forms.valid || !forms.clut_valid)
			return false;
		if (entries == 256)
		{
			// 256 words = four blocks, contiguous in memory from CBP -- or, where those four are
			// 4-aligned and the caller asked for it, the single 16x16 square they form. See the
			// merge note in the header for why 4-alignment is exactly the condition.
			out.merged = merge && (cbp & 3) == 0;
			out.region_count = out.merged ? 1 : 4;
			out.w = out.merged ? 16 : 8;
			out.h = out.w;
		}
		else if (entries == 16)
		{
			// 16 words = the first two texel rows of the block at CBP. columnTable32 puts
			// words 0..15 in rows 0 and 1 and nothing else there, so an 8×2 rect is exactly
			// the loaded words -- no more, no less.
			out.region_count = 1;
			out.w = 8;
			out.h = 2;
		}
		else
		{
			return false;
		}

		out.stride = out.merged ? out.w : 0;

		if (cbp < owner_bp)
			return false;
		const u32 bwpg = owner_bwpg ? owner_bwpg : 1;
		for (u32 b = 0; b < out.region_count; b++)
		{
			// The same owner-side arithmetic Locate performs, at block rather than texel
			// granularity: the block relative to the owner's page-aligned base, then the
			// inverse block form for its position inside its page. Merged, the one region
			// is the block at CBP itself, and that block IS the square's top-left corner:
			// 4-alignment clears the two interleave bits that carry x bit 0 and y bit 0.
			const u32 rel = (cbp + b) - owner_bp;
			const u32 pg = rel >> 5;
			const u32 bpk = forms.inv_block48.Eval(rel & 31);
			out.x[b] = (pg % bwpg) * 64 + (bpk & 7) * 8;
			out.y[b] = (pg / bwpg) * 32 + (bpk >> 3) * 8;
		}
		return true;
	}

	u32 ClutEntryToCopyOffset(const FormSet& forms, u32 entries, u32 e)
	{
		// Entry -> the source word the CSM1 loader took it from, then that word's place in the
		// copied image. inv_col32 packs (x, y) as x | (y << 3), which for an 8-wide region IS
		// the row-major offset, so the two halves compose with no repacking.
		const u32 w = (entries == 256) ? forms.clut_i8_word.Eval(e) : forms.clut_i4_word.Eval(e);
		return (w >> 6) * 64 + forms.inv_col32.Eval(w & 63);
	}

	u32 ClutEntryToMergedOffset(const FormSet& forms, u32 entries, u32 stride, u32 e)
	{
		// The same two halves as ClutEntryToCopyOffset -- entry -> the source word the CSM1 loader
		// took it from, then that word's place in the copied image -- but the image is now one tile
		// `stride` words per row instead of a run of 64-word blocks, so the word's block and its
		// position inside that block have to be composed as texel coordinates rather than
		// concatenated. Block b of a 4-aligned group sits at square-relative texel
		// (8*(b&1), 8*((b>>1)&1)) -- x bit 0 is block bit 0 and y bit 0 is block bit 1 -- and
		// inv_col32 packs the intra-block (cx, cy) as cx | (cy << 3). So
		//   X = ((b & 1) << 3) + (c & 7)
		//   Y = ((b & 2) << 2) + (c >> 3)
		// and the offset is Y * stride + X. The palette's own origin inside the tile is the
		// caller's: a 16x16 square starts at 0, a palette inside a copied page at its square's
		// page-relative origin.
		const u32 w = (entries == 256) ? forms.clut_i8_word.Eval(e) : forms.clut_i4_word.Eval(e);
		const u32 b = w >> 6;
		const u32 c = forms.inv_col32.Eval(w & 63);
		return (((b & 2) << 2) + (c >> 3)) * stride + ((b & 1) << 3) + (c & 7);
	}

	bool LocateClutBlocks16(const FormSet& forms, u32 psm, u32 cbp, u32 owner_bp, u32 owner_bwpg, u32 entries,
		ClutBlockCopy& out)
	{
		out = {};
		if (!forms.valid || !forms.clut_valid || !forms.clut16_valid)
			return false;
		if (psm != PSMCT16 && psm != PSMCT16S)
			return false;

		if (entries == 256)
		{
			// 512 cells = four blocks from CBP, and a CT16 block is 16x8 texels. 4-aligned they are a
			// 2x2 block square, so one 32x16 region says the whole palette; otherwise they are four
			// separate 16x8 regions that still land as the four quadrants of one 32x16 tile, through
			// copy_off. Either way the consumer reads ONE 32-wide tile and needs no second word order.
			out.region_count = ((cbp & 3) == 0) ? 1u : 4u;
			out.w = (out.region_count == 1) ? 32u : 16u;
			out.h = (out.region_count == 1) ? 16u : 8u;
			out.stride = 32;
		}
		else if (entries == 16)
		{
			// 32 cells = the first two texel ROWS of the block at CBP, all sixteen columns wide.
			// columnTable16 puts halfwords 0..31 -- words 0..15 -- in rows 0 and 1 and nothing else
			// there, so a 16x2 rect is exactly the loaded words, no more and no fewer.
			out.region_count = 1;
			out.w = 16;
			out.h = 2;
			out.stride = 16;
		}
		else
		{
			return false;
		}

		if (cbp < owner_bp)
			return false;
		const u32 bwpg = owner_bwpg ? owner_bwpg : 1;
		const XorForm1& inv_block = (psm == PSMCT16) ? forms.inv_block84 : forms.inv_block84s;
		for (u32 b = 0; b < out.region_count; b++)
		{
			// The owner-side arithmetic at block granularity, as LocateClutBlocks does it, with the
			// three numbers a 16-bit surface changes: the block inverse is the 16-bit table's (packed
			// bx | (by << 2), because block84 has two x bits and three y bits), a block is 16x8 texels
			// rather than 8x8, and a page is 64 texels TALL rather than 32.
			const u32 rel = (cbp + b) - owner_bp;
			const u32 pg = rel >> 5;
			const u32 bpk = inv_block.Eval(rel & 31);
			out.x[b] = (pg % bwpg) * 64 + (bpk & 3) * 16;
			out.y[b] = (pg / bwpg) * 64 + (bpk >> 2) * 8;
			// Where region b lands in the tile. Relative block j of a 4-aligned group sits at
			// (bx0 + (j >> 1), by0 + (j & 1)) in blocks -- bit 1 is x, bit 0 is y, the opposite way
			// round from CT32 -- so in texels that is (16 * (j >> 1), 8 * (j & 1)), which is the same
			// quadrant ClutEntryToCt16Offset's block bits select. The single-region forms start at 0.
			out.copy_off[b] = (b & 1) * 8 * out.stride + (b >> 1) * 16;
		}
		return true;
	}

	u32 ClutEntryToCt16Offset(const FormSet& forms, u32 entries, u32 stride, u32 e)
	{
		// The same two halves as ClutEntryToMergedOffset -- entry -> the source word the CSM1 loader
		// took it from, then that word's place in the copied image -- against a 16-bit owner, where a
		// word is two texels rather than one. inv_col32 still serves: columnTable16's halfword index
		// is 2*columnTable32's word index for x < 8 and that plus one for x + 8, so the intra-block
		// (cx, cy) of the word's LOW cell is exactly what the 32-bit column inverse answers, and the
		// high cell is the texel eight to its right.
		//
		// The block bits are the ones that differ. blockTable16 and blockTable16S both put block bit 0
		// on y and bit 1 on x -- the OPPOSITE of blockTable32 -- and a CT16 block is 16x8 texels, so
		// block b of the four sits at square-relative texel (16 * ((b >> 1) & 1), 8 * (b & 1)):
		//   X = ((b >> 1) & 1) * 16 + (c & 7)
		//   Y =  (b       & 1) *  8 + (c >> 3)
		// and the offset is Y * stride + X. The palette's own origin inside the tile is the caller's.
		const u32 w = (entries == 256) ? forms.clut_i8_word.Eval(e) : forms.clut_i4_word.Eval(e);
		const u32 b = w >> 6;
		const u32 c = forms.inv_col32.Eval(w & 63);
		return ((b & 1) * 8 + (c >> 3)) * stride + ((b >> 1) & 1) * 16 + (c & 7);
	}
} // namespace GSTileSwizzleForms
