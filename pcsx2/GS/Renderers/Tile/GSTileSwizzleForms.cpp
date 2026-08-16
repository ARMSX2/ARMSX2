// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Tile/GSTileSwizzleForms.h"

#include "GS/GSClut.h"
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

		// blockTable8 must BE blockTable32 — the shader evaluates one form for both.
		for (u32 by = 0; by < 4 && ok; by++)
			for (u32 bx = 0; bx < 8; bx++)
				ok &= blockTable32.lookup(bx, by) == blockTable8.lookup(bx, by);

		ok &= Fit2([](u32 bx, u32 by) { return static_cast<u32>(blockTable32.lookup(bx, by)); }, 3, 2, f.block48);
		ok &= Fit2([](u32 bx, u32 by) { return static_cast<u32>(blockTable4.lookup(bx, by)); }, 2, 3, f.block84);
		ok &= Fit2([](u32 x, u32 y) { return static_cast<u32>(columnTable32[y][x]); }, 3, 3, f.col32);
		ok &= Fit2([](u32 x, u32 y) { return static_cast<u32>(columnTable8[y][x]); }, 4, 4, f.col8);
		ok &= Fit2([](u32 x, u32 y) { return static_cast<u32>(columnTable4[y][x]); }, 5, 4, f.col4);
		ok &= Invert2(f.block48, f.inv_block48);
		ok &= Invert2(f.col32, f.inv_col32);

		f.valid = ok;

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
		form2("C32", forms.col32);
		form2("C8", forms.col8);
		form2("C4", forms.col4);
		form1("IB48", forms.inv_block48);
		form1("IC32", forms.inv_col32);
		form1("CLUT8", forms.clut_i8_word);
		form1("CLUT4", forms.clut_i4_word);
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
} // namespace GSTileSwizzleForms
