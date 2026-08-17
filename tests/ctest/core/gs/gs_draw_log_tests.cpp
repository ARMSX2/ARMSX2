// SPDX-FileCopyrightText: 2026 ARMSX2 contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GS.h"
#include "GS/Renderers/HW/GSDrawLog.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The draw ledger is an instrument, and an instrument's own defects are the expensive
// kind: they do not announce themselves, they produce a well-formed CSV that every
// downstream census reads confidently and wrongly.
//
// Two failure modes are gated here.
//
// The first is COLUMN MISALIGNMENT. The serialiser writes the header as one string
// literal and each row as a series of fprintf groups, with a matching run of bare commas
// on the branch where a group does not apply. Adding a column means editing at least
// three places, and getting the empty branch wrong shifts every later field by one on
// exactly the rows that took that branch -- so the file still parses, every value is
// still a legal value for some column, and a census silently reports the wrong quantity.
//
// The second is a column that CANNOT CARRY DATA. Ledger columns are deliberately added
// ahead of the machinery that fills them, so "always zero" is the expected reading for a
// while and is indistinguishable from a column wired up wrong. These tests drive the
// recording API directly so the columns are exercised before any renderer calls them.

namespace
{
	struct Csv
	{
		std::vector<std::string> header;
		std::vector<std::vector<std::string>> rows;

		size_t Column(const std::string& name) const
		{
			for (size_t i = 0; i < header.size(); i++)
			{
				if (header[i] == name)
					return i;
			}
			return SIZE_MAX;
		}

		const std::string& At(size_t row, const std::string& name) const
		{
			const size_t col = Column(name);
			EXPECT_NE(col, SIZE_MAX) << "no column named " << name;
			return rows[row][col];
		}
	};

	std::vector<std::string> SplitRow(const std::string& line)
	{
		// The ledger writes no quoted fields, so a plain split is exact. Note the split
		// must keep trailing empties: a row ending in empty cells is precisely the case
		// the alignment test is looking for.
		std::vector<std::string> out;
		std::string field;
		std::istringstream ss(line);
		while (std::getline(ss, field, ','))
			out.push_back(field);
		if (!line.empty() && line.back() == ',')
			out.push_back(std::string());
		return out;
	}

	/// Records rows through the public API and returns the parsed CSV. The ledger holds
	/// process-wide state, so every test starts from Reset().
	///
	/// Recording is gated on the SETTING rather than on Start(), deliberately: the ledger
	/// has to work when DumpDrawLog is already true at GS open, where there is no config
	/// edge to notice. So the setting is what a test has to drive -- Start() alone records
	/// nothing.
	Csv Record(const std::function<void()>& record_rows)
	{
		const bool prev = GSConfig.DumpDrawLog;
		GSConfig.DumpDrawLog = true;

		GSDrawLog::Reset();
		GSDrawLog::Start();
		record_rows();
		GSDrawLog::Stop();
		GSConfig.DumpDrawLog = prev;

		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / "armsx2_gs_draw_log_test.csv";
		EXPECT_TRUE(GSDrawLog::WriteCSV(path.string()));

		Csv csv;
		std::ifstream in(path);
		std::string line;
		if (std::getline(in, line))
			csv.header = SplitRow(line);
		while (std::getline(in, line))
		{
			if (!line.empty())
				csv.rows.push_back(SplitRow(line));
		}
		in.close();
		std::filesystem::remove(path);
		GSDrawLog::Reset();
		return csv;
	}

	GSDrawLog::Record MakeState()
	{
		GSDrawLog::Record rec = {};
		rec.frame = 1;
		rec.draw = 1;
		rec.packet = GSDrawLog::PacketNone;
		rec.prim_count = 1;
		return rec;
	}

	/// A minimal Tile row: one draw, realized natively, with whatever sync the caller adds.
	void TileRow(const std::function<void()>& add_sync)
	{
		GSDrawLog::BeginDraw(MakeState());
		add_sync();
		GSDrawLog::NoteTileDraw(false, 0, 0, GSDrawLog::TileFallbackNone, GSVector4i(0, 0, 16, 16), 0,
			/*blend_leg=*/0, /*blend_src1=*/0, /*carrier_refusal=*/0);
		GSDrawLog::FinishDraw();
	}
} // namespace

// The alignment gate. Exercised across all three row shapes, because each takes a
// different set of empty-cell branches: a Tile row fills the tile group and leaves the
// backend group empty, a skipped row leaves both empty, and a software row fills only the
// draw rect.
TEST(GSDrawLog, EveryRowHasExactlyAsManyFieldsAsTheHeader)
{
	const Csv csv = Record([] {
		TileRow([] { GSDrawLog::NoteTileSync(4, 1, GSDrawLog::TileSyncUploadSource); });

		// A draw that returned before submit: opened, never completed.
		GSDrawLog::BeginDraw(MakeState());
		GSDrawLog::FinishDraw();

		GSDrawLog::BeginDraw(MakeState());
		GSDrawLog::NoteSWDraw(GSVector4i(0, 0, 8, 8));
		GSDrawLog::FinishDraw();
	});

	ASSERT_EQ(csv.rows.size(), 3u);
	for (size_t i = 0; i < csv.rows.size(); i++)
		EXPECT_EQ(csv.rows[i].size(), csv.header.size()) << "row " << i << " is misaligned";
}

// One sync, one stall. The reason must come out as a single set bit -- a sole-reason row
// is what tells a census that fixing that reason RETIRES the stall rather than shrinking
// it, and that reading depends entirely on no other bit being set spuriously.
TEST(GSDrawLog, ASingleSyncIsASoleReasonRow)
{
	const Csv csv = Record([] {
		TileRow([] { GSDrawLog::NoteTileSync(3, 1, GSDrawLog::TileSyncTextureOnTarget); });
	});

	ASSERT_EQ(csv.rows.size(), 1u);
	EXPECT_EQ(csv.At(0, "stalls"), "1");
	EXPECT_EQ(csv.At(0, "sync_pages"), "3");

	const unsigned reason = std::stoul(csv.At(0, "sync_reason"), nullptr, 16);
	EXPECT_EQ(reason, static_cast<unsigned>(GSDrawLog::TileSyncTextureOnTarget));
	EXPECT_EQ(reason & (reason - 1), 0u) << "a single sync must set exactly one reason bit";
}

// A quiescent pull — pages copied off the GPU whose producing submission had already
// retired — records its pages with ZERO stalls. The two costs are different (a copy is
// paid either way; a drain only sometimes), and folding them was how a spilling title
// once produced an all-zero column that read as a dead hook.
TEST(GSDrawLog, AQuiescentPullRecordsPagesWithoutAStall)
{
	const Csv csv = Record([] {
		TileRow([] { GSDrawLog::NoteTileSync(6, 0, GSDrawLog::TileSyncFloorSpill); });
	});

	ASSERT_EQ(csv.rows.size(), 1u);
	EXPECT_EQ(csv.At(0, "stalls"), "0");
	EXPECT_EQ(csv.At(0, "sync_pages"), "6");
	EXPECT_EQ(std::stoul(csv.At(0, "sync_reason"), nullptr, 16),
		static_cast<unsigned>(GSDrawLog::TileSyncFloorSpill));
}

// Several syncs on one draw accumulate rather than overwrite. This is the property that
// makes the column a count of stalls instead of a count of draws-that-stalled, and the
// two answer different questions: only the former can be divided into sync_pages to get
// the batching headroom.
TEST(GSDrawLog, RepeatedSyncsAccumulateAndReasonsOr)
{
	const Csv csv = Record([] {
		TileRow([] {
			GSDrawLog::NoteTileSync(2, 1, GSDrawLog::TileSyncUploadSource);
			GSDrawLog::NoteTileSync(5, 1, GSDrawLog::TileSyncStealSurface);
			GSDrawLog::NoteTileSync(1, 1, GSDrawLog::TileSyncUploadSource);
		});
	});

	ASSERT_EQ(csv.rows.size(), 1u);
	EXPECT_EQ(csv.At(0, "stalls"), "3");
	EXPECT_EQ(csv.At(0, "sync_pages"), "8");

	const unsigned reason = std::stoul(csv.At(0, "sync_reason"), nullptr, 16);
	EXPECT_EQ(reason,
		static_cast<unsigned>(GSDrawLog::TileSyncUploadSource | GSDrawLog::TileSyncStealSurface));
	// Two distinct reasons: not a sole-reason row, so fixing either one only shrinks it.
	EXPECT_NE(reason & (reason - 1), 0u);
}

// A row that never syncs must read zero, not empty and not garbage -- the census divides
// by this column.
TEST(GSDrawLog, ADrawThatNeverSyncsReadsZero)
{
	const Csv csv = Record([] { TileRow([] {}); });

	ASSERT_EQ(csv.rows.size(), 1u);
	EXPECT_EQ(csv.At(0, "stalls"), "0");
	EXPECT_EQ(csv.At(0, "sync_pages"), "0");
	EXPECT_EQ(csv.At(0, "sync_reason"), "0");
}

// The stall counter saturates rather than wraps. A draw syncing more than 65535 times has
// a problem the exact count would not help with, but a wrapped count reports it as a
// HEALTHY draw -- the pathological case masquerading as the best case is the one reading
// a ledger must never produce.
TEST(GSDrawLog, TheStallCounterSaturatesInsteadOfWrapping)
{
	const Csv csv = Record([] {
		TileRow([] {
			for (int i = 0; i < 70000; i++)
				GSDrawLog::NoteTileSync(0, 1, GSDrawLog::TileSyncUploadSource);
		});
	});

	ASSERT_EQ(csv.rows.size(), 1u);
	EXPECT_EQ(csv.At(0, "stalls"), "65535");
}

// Non-Tile rows must serialise the sync group as empty cells, not as zeros. A Classic row
// reading "0 stalls" would be a measurement it never made, and averaging it in is how a
// renderer that does not have the concept ends up flattering the one that does.
TEST(GSDrawLog, NonTileRowsLeaveTheSyncColumnsEmpty)
{
	const Csv csv = Record([] {
		GSDrawLog::BeginDraw(MakeState());
		GSDrawLog::NoteSWDraw(GSVector4i(0, 0, 8, 8));
		GSDrawLog::FinishDraw();
	});

	ASSERT_EQ(csv.rows.size(), 1u);
	EXPECT_EQ(csv.At(0, "stalls"), "");
	EXPECT_EQ(csv.At(0, "sync_pages"), "");
	EXPECT_EQ(csv.At(0, "sync_reason"), "");
}
