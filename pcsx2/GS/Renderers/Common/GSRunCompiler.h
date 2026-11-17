// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSRunsPolicy.h"

#include <array>
#include <vector>

/// The pass compiler: at a run's close, the order its rows are drawn in and the groups they are drawn as. A group is
/// either rows that share one host draw of the run program, or one row drawn with its classic program. No device:
/// everything the decision needs is in the rows, which the row writer fills from the row's state entry and texture.
///
/// The join rules are the run machine's (GSRunsVK: SameFixed, Matches, SplitAtDear, the texture array), applied to
/// consecutive rows in the final order, so with no twins the groups are the runs the machine forms today, less its
/// sleeping and priming rules: a group of one row is drawn classically, which is what priming bought.
namespace GSRunCompiler
{
	/// A row's role in the twin sort.
	enum Role : u8
	{
		ROLE_NONE = 0,
		/// The row's state can be a twin base: it tests GREATER, writes depth, writes every colour channel with blending
		/// elided and has no alpha test.
		ROLE_BASE = 1,
		/// TWIN: the row's state is an allowed twin of the previous row's (it tests EQUAL and writes no depth, way 2, no
		/// alpha test), the previous row is a base, and every position and index equals the previous row's.
		ROLE_TWIN = 2,
	};

	static constexpr u8 NO_TEXTURE = 0xFF;
	static constexpr u32 MAX_TEXTURE_SLOTS = 256;

	/// The compiler's view of one row.
	struct Row
	{
		u32 fixed = 0;   ///< the SameFixed class: rows share a group only with equal values (the caller numbers them)
		u32 strict = 0;  ///< GSRunsPolicy::StrictKey of the row's flags
		u32 flags = 0;   ///< GSRunsPolicy::Draw::flags
		u32 way = 0;     ///< GSRunsPolicy::Draw::way
		u64 area = 0;    ///< draw-rectangle pixels
		u8 tex = NO_TEXTURE; ///< slot in the run's texture list
		u8 role = ROLE_NONE;
		bool cand = false;   ///< the run program can draw it (GSRunsPolicy::Classify refused nothing)
		bool pinned = false; ///< never moved by the sort (a second-pass row, a self-read)
	};

	struct Group
	{
		u32 first = 0; ///< position in the order
		u32 count = 0;
		bool merged = false; ///< one host draw of the run program; else one row, classic
	};

	struct Rules
	{
		bool one_call = false; ///< DriverBug::BrokenMixedSamplingDerivatives: one lod field per group (a boundary)
		const GSRunsPolicy::DeviceClass* device = &GSRunsPolicy::DEVICE_CLASS_DEFAULT;
		u32 max_draws = 8184;  ///< rows per run-program group (the table window)
		u32 max_textures = 32; ///< textures per run-program group (its descriptor array)
		bool sort_twins = true;
	};

	/// The twin sort. order receives a permutation of 0..n-1. A window of consecutive pairs (base, twin, base, twin, ...)
	/// becomes all its bases in their order, then all its twins in theirs. Exact where the bases test GREATER:
	/// a moved twin meets a later base's depth only where that base passed, which is strictly
	/// nearer than the twin's own depth, so the twin fails there, as the base would have covered it in the game's order.
	inline void SortTwins(const Row* rows, u32 n, u32* order, bool sort)
	{
		u32 out = 0;
		u32 i = 0;
		while (i < n)
		{
			// A window: pairs (i, i+1) with i a base and i+1 its twin, neither pinned, back to back.
			u32 j = i;
			while (sort && j + 1 < n && rows[j].role == ROLE_BASE && rows[j + 1].role == ROLE_TWIN && !rows[j].pinned &&
				   !rows[j + 1].pinned)
				j += 2;
			const u32 pairs = (j - i) / 2;
			if (pairs >= 2)
			{
				for (u32 p = 0; p < pairs; p++)
					order[out++] = i + 2 * p;
				for (u32 p = 0; p < pairs; p++)
					order[out++] = i + 2 * p + 1;
				i = j;
			}
			else
			{
				order[out++] = i++;
			}
		}
	}

	/// The groups of rows in `order`.
	inline void MakeGroups(const Row* rows, const u32* order, u32 n, const Rules& rules, std::vector<Group>& groups)
	{
		groups.clear();
		struct Open
		{
			u32 fixed, strict, ways, lod, dear, ntex;
			u64 area;
			bool dear_mixed;
			std::array<u64, MAX_TEXTURE_SLOTS / 64> tex;
		} g{};
		bool open = false;

		const auto start = [&](u32 pos, const Row& r) {
			groups.push_back({pos, 1, false});
			open = r.cand;
			if (!open)
				return;
			g.fixed = r.fixed;
			g.strict = r.strict;
			g.ways = 1u << r.way;
			g.lod = GSRunsPolicy::Textured(r.flags) ? GSRunsPolicy::Field(r.flags, GSRunsPolicy::STATE_LOD) : GSRunsPolicy::LOD_ANY;
			g.dear = GSRunsPolicy::Dear(r.flags);
			g.dear_mixed = false;
			g.area = r.area;
			g.tex = {};
			g.ntex = 0;
			if (r.tex != NO_TEXTURE)
			{
				g.tex[r.tex >> 6] |= u64(1) << (r.tex & 63);
				g.ntex = 1;
			}
		};

		for (u32 pos = 0; pos < n; pos++)
		{
			const Row& r = rows[order[pos]];
			if (open && r.cand)
			{
				Group& cur = groups.back();
				const bool new_tex = r.tex != NO_TEXTURE && !(g.tex[r.tex >> 6] & (u64(1) << (r.tex & 63)));
				const u32 dear = GSRunsPolicy::Dear(r.flags);
				const bool joins = r.fixed == g.fixed && r.strict == g.strict && cur.count < rules.max_draws &&
				                   GSRunsPolicy::WaysShareAnEquation(g.ways | (1u << r.way)) &&
				                   GSRunsPolicy::LodJoins(rules.one_call, g.lod, r.flags) &&
				                   (g.dear_mixed || dear == g.dear || !GSRunsPolicy::SplitAtDear(*rules.device, g.area, r.area)) &&
				                   (!new_tex || g.ntex < rules.max_textures);
				if (joins)
				{
					cur.count++;
					cur.merged = true;
					g.ways |= 1u << r.way;
					if (GSRunsPolicy::Textured(r.flags))
						g.lod = GSRunsPolicy::Field(r.flags, GSRunsPolicy::STATE_LOD);
					if (dear != g.dear)
						g.dear_mixed = true;
					g.area += r.area;
					if (new_tex)
					{
						g.tex[r.tex >> 6] |= u64(1) << (r.tex & 63);
						g.ntex++;
					}
					continue;
				}
			}
			start(pos, r);
		}
	}
} // namespace GSRunCompiler
