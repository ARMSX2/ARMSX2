// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Autosave rotation (pcsx2/Autosave.h): the file moves, every step failing in turn, the save
// failing in each way, the interval timer and the listing. No VM is needed: the rotation works on
// file names and the save itself is a function the test supplies.

#include "catalog_test_support.h"

#include "Autosave.h"
#include "Config.h"

#include "common/MemorySettingsInterface.h"
#include "common/SettingsWrapper.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using CatalogTest::ScopedTempDir;

namespace
{
	namespace fs = std::filesystem;

	void Write(const std::string& path, const std::string& content)
	{
		std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
	}

	std::string Read(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	// Everything in the folder: name -> content.
	std::map<std::string, std::string> Snapshot(const std::string& dir)
	{
		std::map<std::string, std::string> files;
		for (const auto& entry : fs::directory_iterator(dir))
			files[entry.path().filename().string()] = Read(entry.path().string());
		return files;
	}

	// The real file system, except that the Nth rename fails (0 = none fail).
	class FailingOps final : public Catalog::FileOps
	{
	public:
		explicit FailingOps(int fail_rename)
			: m_fail(fail_rename)
		{
		}

		bool Exists(const std::string& path) override { return m_real.Exists(path); }
		bool Remove(const std::string& path) override { return m_real.Remove(path); }
		bool Rename(const std::string& from, const std::string& to) override
		{
			m_renames++;
			if (m_renames == m_fail)
				return false;
			return m_real.Rename(from, to);
		}

		int Renames() const { return m_renames; }

	private:
		Catalog::RealFileOps m_real;
		int m_fail;
		int m_renames = 0;
	};

	struct Fixture
	{
		ScopedTempDir dir;
		const std::string newest = dir.File("SLUS-12345 (ABCD1234).autosave.p2s");

		std::string At(int n) const { return Autosave::FileNameAt(newest, n); }
		std::string Drop(int n) const { return At(n) + ".drop"; }
		bool Has(const std::string& path) const { return fs::exists(path); }

		// Autosaves 1..count, with content "gen<n>".
		void Populate(int count)
		{
			for (int n = 1; n <= count; n++)
				Write(At(n), "gen" + std::to_string(n));
		}
	};
} // namespace

// ---- names ------------------------------------------------------------------------------

TEST(Autosave, NamesTheNewestAndTheOlderOnes)
{
	const std::string newest = "/sstates/SLUS-12345 (ABCD1234).autosave.p2s";
	EXPECT_EQ(Autosave::FileNameAt(newest, 1), newest);
	EXPECT_EQ(Autosave::FileNameAt(newest, 0), newest);
	EXPECT_EQ(Autosave::FileNameAt(newest, 2), "/sstates/SLUS-12345 (ABCD1234).autosave.2.p2s");
	EXPECT_EQ(Autosave::FileNameAt(newest, 5), "/sstates/SLUS-12345 (ABCD1234).autosave.5.p2s");
	// Not a .p2s name: there are no numbered neighbours.
	EXPECT_EQ(Autosave::FileNameAt("/x/state.bin", 3), "/x/state.bin");
}

// ---- the rotation, success --------------------------------------------------------------

TEST(AutosaveRotation, FirstAutosaveMovesNothing)
{
	Fixture f;
	Autosave::Rotation rotation(f.newest, 3);
	ASSERT_TRUE(rotation.Begin());
	Write(f.newest, "new");
	rotation.Commit();
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 1u);
	EXPECT_EQ(Read(f.newest), "new");
}

TEST(AutosaveRotation, ShiftsEveryAutosaveOnePlaceOlderAndDropsTheOldest)
{
	Fixture f;
	f.Populate(3); // gen1 newest, gen2, gen3 oldest

	Autosave::Rotation rotation(f.newest, 3);
	ASSERT_TRUE(rotation.Begin());
	// The oldest is set aside, not deleted, until the save is known to have worked.
	EXPECT_FALSE(f.Has(f.newest));
	EXPECT_EQ(Read(f.At(2)), "gen1");
	EXPECT_EQ(Read(f.At(3)), "gen2");
	EXPECT_EQ(Read(f.Drop(3)), "gen3");

	Write(f.newest, "new");
	rotation.Commit();

	EXPECT_EQ(Read(f.newest), "new");
	EXPECT_EQ(Read(f.At(2)), "gen1");
	EXPECT_EQ(Read(f.At(3)), "gen2");
	EXPECT_FALSE(f.Has(f.Drop(3)));
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 3u);
}

TEST(AutosaveRotation, ShorterHistoryThanKeepJustGrows)
{
	Fixture f;
	f.Populate(1);

	Autosave::Rotation rotation(f.newest, 4);
	ASSERT_TRUE(rotation.Begin());
	Write(f.newest, "new");
	rotation.Commit();

	EXPECT_EQ(Read(f.newest), "new");
	EXPECT_EQ(Read(f.At(2)), "gen1");
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 2u);
}

TEST(AutosaveRotation, AGapInTheHistoryIsKept)
{
	Fixture f;
	Write(f.newest, "gen1");
	Write(f.At(3), "gen3");

	Autosave::Rotation rotation(f.newest, 4);
	ASSERT_TRUE(rotation.Begin());
	Write(f.newest, "new");
	rotation.Commit();

	EXPECT_EQ(Read(f.newest), "new");
	EXPECT_EQ(Read(f.At(2)), "gen1");
	EXPECT_FALSE(f.Has(f.At(3)));
	EXPECT_EQ(Read(f.At(4)), "gen3");
}

TEST(AutosaveRotation, KeepOneReplacesTheOnlyAutosaveWithoutLeavingABackup)
{
	Fixture f;
	f.Populate(1);

	Autosave::Rotation rotation(f.newest, 1);
	ASSERT_TRUE(rotation.Begin());
	EXPECT_FALSE(f.Has(f.newest));
	EXPECT_EQ(Read(f.Drop(1)), "gen1");
	Write(f.newest, "new");
	rotation.Commit();

	EXPECT_EQ(Read(f.newest), "new");
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 1u);
}

TEST(AutosaveRotation, CommitDeletesAutosavesPastTheKeepCountButBeginDoesNot)
{
	Fixture f;
	f.Populate(5);

	Autosave::Rotation rotation(f.newest, 2);
	ASSERT_TRUE(rotation.Begin());
	// Still there: a save that fails must not have cost the user states.
	EXPECT_TRUE(f.Has(f.At(4)));
	EXPECT_TRUE(f.Has(f.At(5)));
	Write(f.newest, "new");
	rotation.Commit();

	EXPECT_EQ(Read(f.newest), "new");
	EXPECT_EQ(Read(f.At(2)), "gen1");
	EXPECT_FALSE(f.Has(f.At(3)));
	EXPECT_FALSE(f.Has(f.At(4)));
	EXPECT_FALSE(f.Has(f.At(5)));
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 2u);
}

TEST(AutosaveRotation, KeepIsClampedToOneThroughFive)
{
	Fixture f;
	f.Populate(5);
	{
		Autosave::Rotation rotation(f.newest, 99);
		ASSERT_TRUE(rotation.Begin());
		Write(f.newest, "new");
		rotation.Commit();
	}
	EXPECT_EQ(Read(f.At(5)), "gen4");
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 5u);

	Fixture g;
	g.Populate(3);
	{
		Autosave::Rotation rotation(g.newest, -4);
		ASSERT_TRUE(rotation.Begin());
		Write(g.newest, "new");
		rotation.Commit();
	}
	// Keep 1: only the new autosave remains.
	EXPECT_EQ(Snapshot(g.dir.Dir()).size(), 1u);
	EXPECT_EQ(Read(g.newest), "new");
}

TEST(AutosaveRotation, AStaleSetAsideFileFromAnInterruptedRunIsRemoved)
{
	// Their slots are filled again (the run got as far as moving the next autosave up), so what they hold
	// was already due to be dropped.
	Fixture f;
	f.Populate(3);
	Write(f.At(5), "gen5");
	Write(f.Drop(3), "stale");
	Write(f.Drop(5), "stale");

	Autosave::Rotation rotation(f.newest, 3);
	ASSERT_TRUE(rotation.Begin());
	EXPECT_FALSE(f.Has(f.Drop(5)));
	EXPECT_EQ(Read(f.At(5)), "gen5"); // past keep, but only a Commit removes it
	Write(f.newest, "new");
	rotation.Commit();
	EXPECT_FALSE(f.Has(f.Drop(3)));
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 3u);
}

TEST(AutosaveRotation, TheOnlyAutosaveSetAsideByACrashedRunComesBack)
{
	// keep 1: the run set the autosave aside as .drop and died before the new one was written. Nothing is at
	// the newest name, so the .drop is the only autosave there is.
	Fixture f;
	Write(f.Drop(1), "the only autosave");

	Autosave::Rotation rotation(f.newest, 1);
	ASSERT_TRUE(rotation.Begin());
	rotation.Rollback(); // this run's save failed too
	EXPECT_EQ(Read(f.newest), "the only autosave");
	EXPECT_FALSE(f.Has(f.Drop(1)));
}

TEST(AutosaveRotation, ASetAsideFileWhoseSlotIsEmptyIsPutBackWhateverTheKeepCount)
{
	// keep 3: the oldest was set aside and the run died before anything moved up into its place.
	Fixture f;
	f.Populate(2);
	Write(f.Drop(3), "gen3");

	Autosave::Rotation rotation(f.newest, 3);
	ASSERT_TRUE(rotation.Begin());
	Write(f.newest, "new");
	rotation.Commit();

	// Nothing was lost: the three newest are the new one, gen1 and gen2, and gen3 was due to go.
	EXPECT_EQ(Read(f.At(1)), "new");
	EXPECT_EQ(Read(f.At(2)), "gen1");
	EXPECT_EQ(Read(f.At(3)), "gen2");
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 3u);

	Fixture g;
	g.Populate(2);
	Write(g.Drop(3), "gen3");
	Autosave::Rotation rolled_back(g.newest, 3);
	ASSERT_TRUE(rolled_back.Begin());
	rolled_back.Rollback();
	EXPECT_EQ(Read(g.At(3)), "gen3");
	EXPECT_EQ(Snapshot(g.dir.Dir()).size(), 3u);
}

TEST(AutosaveRotation, ACrashedKeepOneSaveThatFailsAgainLosesNothing)
{
	Fixture f;
	Write(f.Drop(1), "the only autosave");

	std::string error;
	const auto result = Autosave::RotatedSave(
		f.newest, 1, [](const std::string&, std::string* why) {
			*why = "disk full";
			return false;
		},
		&error);
	EXPECT_EQ(result, Autosave::SaveResult::SaveFailed);
	EXPECT_EQ(Read(f.newest), "the only autosave");
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 1u);
}

// ---- the rotation, rollback -------------------------------------------------------------

TEST(AutosaveRotation, RollbackPutsEverythingBackAndRemovesTheHalfWrittenFile)
{
	Fixture f;
	f.Populate(3);
	const auto before = Snapshot(f.dir.Dir());

	Autosave::Rotation rotation(f.newest, 3);
	ASSERT_TRUE(rotation.Begin());
	Write(f.newest, "half a sta"); // the save died part-way
	rotation.Rollback();

	EXPECT_EQ(Snapshot(f.dir.Dir()), before);
	EXPECT_FALSE(rotation.IsOpen());
}

TEST(AutosaveRotation, RollbackWithKeepOneRestoresThePreviousAutosave)
{
	Fixture f;
	f.Populate(1);

	Autosave::Rotation rotation(f.newest, 1);
	ASSERT_TRUE(rotation.Begin());
	Write(f.newest, "half");
	rotation.Rollback();

	EXPECT_EQ(Read(f.newest), "gen1");
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 1u);
}

TEST(AutosaveRotation, RollbackWithNoPreviousAutosaveRemovesTheNewFile)
{
	Fixture f;
	Autosave::Rotation rotation(f.newest, 3);
	ASSERT_TRUE(rotation.Begin());
	Write(f.newest, "half");
	rotation.Rollback();
	EXPECT_TRUE(Snapshot(f.dir.Dir()).empty());
}

TEST(AutosaveRotation, RollbackKeepsAutosavesPastTheKeepCount)
{
	Fixture f;
	f.Populate(5);
	const auto before = Snapshot(f.dir.Dir());

	Autosave::Rotation rotation(f.newest, 2);
	ASSERT_TRUE(rotation.Begin());
	rotation.Rollback();
	EXPECT_EQ(Snapshot(f.dir.Dir()), before);
}

TEST(AutosaveRotation, DestroyedWithoutCommitIsARollback)
{
	Fixture f;
	f.Populate(3);
	const auto before = Snapshot(f.dir.Dir());
	{
		Autosave::Rotation rotation(f.newest, 3);
		ASSERT_TRUE(rotation.Begin());
		Write(f.newest, "half");
	}
	EXPECT_EQ(Snapshot(f.dir.Dir()), before);
}

TEST(AutosaveRotation, CommitAfterRollbackAndRollbackAfterCommitDoNothing)
{
	Fixture f;
	f.Populate(2);
	Autosave::Rotation rotation(f.newest, 3);
	ASSERT_TRUE(rotation.Begin());
	Write(f.newest, "new");
	rotation.Commit();
	const auto after = Snapshot(f.dir.Dir());
	rotation.Rollback();
	rotation.Commit();
	EXPECT_EQ(Snapshot(f.dir.Dir()), after);
}

// ---- the rotation, a step failing -------------------------------------------------------

// Fails each rename of Begin() in turn; the folder must come back exactly as it was.
TEST(AutosaveRotation, AFailedStepUndoesTheStepsBeforeIt)
{
	for (const int keep : {1, 2, 3, 4, 5})
	{
		// Count the renames of a clean run.
		int total_renames = 0;
		{
			Fixture f;
			f.Populate(keep);
			FailingOps ops(0);
			Autosave::Rotation rotation(f.newest, keep, ops);
			ASSERT_TRUE(rotation.Begin());
			total_renames = ops.Renames();
			rotation.Rollback();
		}
		ASSERT_GE(total_renames, 1);

		for (int fail_at = 1; fail_at <= total_renames; fail_at++)
		{
			Fixture f;
			f.Populate(keep);
			const auto before = Snapshot(f.dir.Dir());

			FailingOps ops(fail_at);
			Autosave::Rotation rotation(f.newest, keep, ops);
			EXPECT_FALSE(rotation.Begin()) << "keep " << keep << " failing rename " << fail_at;
			EXPECT_FALSE(rotation.IsOpen());
			EXPECT_EQ(Snapshot(f.dir.Dir()), before) << "keep " << keep << " failing rename " << fail_at;
		}
	}
}

TEST(AutosaveRotation, AFailedStepWithAGapInTheHistoryUndoesToo)
{
	Fixture f;
	Write(f.newest, "gen1");
	Write(f.At(3), "gen3");
	Write(f.At(5), "gen5");
	const auto before = Snapshot(f.dir.Dir());

	// keep 5: oldest->drop, 4 absent, 3->4, 2 absent, newest->2.
	for (int fail_at = 1; fail_at <= 3; fail_at++)
	{
		FailingOps ops(fail_at);
		Autosave::Rotation rotation(f.newest, 5, ops);
		EXPECT_FALSE(rotation.Begin()) << fail_at;
		EXPECT_EQ(Snapshot(f.dir.Dir()), before) << fail_at;
	}
}

TEST(AutosaveRotation, ANameThatCannotBeRotatedIsRefused)
{
	ScopedTempDir dir;
	Autosave::Rotation rotation(dir.File("state.bin"), 3);
	EXPECT_FALSE(rotation.Begin());
	Autosave::Rotation empty("", 3);
	EXPECT_FALSE(empty.Begin());
}

// ---- RotatedSave ------------------------------------------------------------------------

TEST(AutosaveSave, SavesAndRotates)
{
	Fixture f;
	f.Populate(2);
	int calls = 0;
	std::string error;
	const auto result = Autosave::RotatedSave(
		f.newest, 3,
		[&](const std::string& path, std::string*) {
			calls++;
			Write(path, "new");
			return true;
		},
		&error);

	EXPECT_EQ(result, Autosave::SaveResult::Saved);
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(Read(f.newest), "new");
	EXPECT_EQ(Read(f.At(2)), "gen1");
	EXPECT_EQ(Read(f.At(3)), "gen2");
}

TEST(AutosaveSave, ASaveThatFailsWithAnErrorLosesNothing)
{
	Fixture f;
	f.Populate(3);
	const auto before = Snapshot(f.dir.Dir());
	std::string error;
	const auto result = Autosave::RotatedSave(
		f.newest, 3,
		[](const std::string&, std::string* why) {
			*why = "disk full";
			return false;
		},
		&error);

	EXPECT_EQ(result, Autosave::SaveResult::SaveFailed);
	EXPECT_EQ(error, "disk full");
	EXPECT_EQ(Snapshot(f.dir.Dir()), before);
}

TEST(AutosaveSave, ASaveThatLeavesHalfAFileLosesNothingAndLeavesNoJunk)
{
	Fixture f;
	f.Populate(3);
	const auto before = Snapshot(f.dir.Dir());
	const auto result = Autosave::RotatedSave(f.newest, 3, [](const std::string& path, std::string* why) {
		Write(path, "half");
		*why = "write error";
		return false;
	});

	EXPECT_EQ(result, Autosave::SaveResult::SaveFailed);
	EXPECT_EQ(Snapshot(f.dir.Dir()), before);
}

TEST(AutosaveSave, ASaveThatClaimsSuccessButWritesNothingIsAFailure)
{
	Fixture f;
	f.Populate(3);
	const auto before = Snapshot(f.dir.Dir());
	std::string error;
	const auto result = Autosave::RotatedSave(
		f.newest, 3, [](const std::string&, std::string*) { return true; }, &error);

	EXPECT_EQ(result, Autosave::SaveResult::SaveFailed);
	EXPECT_FALSE(error.empty());
	EXPECT_EQ(Snapshot(f.dir.Dir()), before);
}

TEST(AutosaveSave, AFailedRotationDoesNotAttemptTheSave)
{
	Fixture f;
	f.Populate(3);
	const auto before = Snapshot(f.dir.Dir());
	FailingOps ops(2);
	int calls = 0;
	std::string error;
	const auto result = Autosave::RotatedSave(
		f.newest, 3,
		[&](const std::string& path, std::string*) {
			calls++;
			Write(path, "new");
			return true;
		},
		&error, ops);

	EXPECT_EQ(result, Autosave::SaveResult::RotationFailed);
	EXPECT_EQ(calls, 0);
	EXPECT_FALSE(error.empty());
	EXPECT_EQ(Snapshot(f.dir.Dir()), before);
}

TEST(AutosaveSave, NoGameNameIsNoGame)
{
	EXPECT_EQ(Autosave::RotatedSave("", 3, [](const std::string&, std::string*) { return true; }), Autosave::SaveResult::NoGame);
}

TEST(AutosaveSave, WorksTwentyTimesInARowKeepingTheLastThree)
{
	Fixture f;
	for (int i = 1; i <= 20; i++)
	{
		const std::string content = "gen" + std::to_string(i);
		ASSERT_EQ(Autosave::RotatedSave(f.newest, 3,
					  [&](const std::string& path, std::string*) {
						  Write(path, content);
						  return true;
					  }),
			Autosave::SaveResult::Saved);
	}
	EXPECT_EQ(Read(f.newest), "gen20");
	EXPECT_EQ(Read(f.At(2)), "gen19");
	EXPECT_EQ(Read(f.At(3)), "gen18");
	EXPECT_EQ(Snapshot(f.dir.Dir()).size(), 3u);
}

// ---- the timer --------------------------------------------------------------------------

namespace
{
	// Plays `seconds` of one second frames; true if the timer was due at the last of them.
	bool Play(Autosave::IntervalTimer& timer, int seconds, int interval_minutes)
	{
		bool due = false;
		for (int i = 0; i < seconds; i++)
			due = timer.Advance(1.0, interval_minutes);
		return due;
	}
} // namespace

TEST(AutosaveTimer, IsOffAtZeroMinutes)
{
	Autosave::IntervalTimer timer;
	for (int i = 0; i < 100000; i++)
		ASSERT_FALSE(timer.Advance(1.0, 0));
	EXPECT_EQ(timer.Elapsed(), 0.0);
}

TEST(AutosaveTimer, IsDueOnceTheIntervalHasPassed)
{
	Autosave::IntervalTimer timer;
	EXPECT_FALSE(Play(timer, 119, 2));
	EXPECT_FALSE(timer.Advance(0.5, 2));
	EXPECT_TRUE(timer.Advance(0.5, 2));
}

TEST(AutosaveTimer, TimeNotRunningDoesNotCount)
{
	Autosave::IntervalTimer timer;
	// A ten minute pause before the first frame back counts for nothing.
	EXPECT_FALSE(timer.Advance(600.0, 1));
	EXPECT_EQ(timer.Elapsed(), 0.0);
	EXPECT_FALSE(Play(timer, 59, 1));
	// None of these is a frame gap.
	EXPECT_FALSE(timer.Advance(Autosave::IntervalTimer::MAX_COUNTED_GAP_SECONDS + 0.001, 1));
	EXPECT_FALSE(timer.Advance(3600.0, 1));
	EXPECT_FALSE(timer.Advance(-5.0, 1));
	EXPECT_TRUE(timer.Advance(1.0, 1));
}

TEST(AutosaveTimer, AFinishedSaveRestartsTheInterval)
{
	Autosave::IntervalTimer timer;
	ASSERT_TRUE(Play(timer, 300, 5));
	timer.OnResult(Autosave::SaveResult::Saved, 5);
	EXPECT_EQ(timer.Elapsed(), 0.0);
	EXPECT_FALSE(Play(timer, 299, 5));
	EXPECT_TRUE(Play(timer, 1, 5));
}

TEST(AutosaveTimer, AFailedSaveWaitsAWholeInterval)
{
	Autosave::IntervalTimer timer;
	for (const auto result : {Autosave::SaveResult::SaveFailed, Autosave::SaveResult::RotationFailed, Autosave::SaveResult::NoGame})
	{
		Play(timer, 100, 5);
		timer.OnResult(result, 5);
		EXPECT_EQ(timer.Elapsed(), 0.0);
	}
}

TEST(AutosaveTimer, ARefusedSaveBecauseTheCardIsBusyRetriesAfterThirtySeconds)
{
	Autosave::IntervalTimer timer;
	ASSERT_TRUE(Play(timer, 600, 10));
	timer.OnResult(Autosave::SaveResult::CardBusy, 10);
	// Not another 10 minutes: due again after 30 more seconds of play.
	EXPECT_FALSE(Play(timer, 29, 10));
	EXPECT_TRUE(Play(timer, 1, 10));

	// And again every 30 seconds while the card stays busy.
	timer.OnResult(Autosave::SaveResult::CardBusy, 10);
	EXPECT_FALSE(Play(timer, 29, 10));
	EXPECT_TRUE(Play(timer, 1, 10));
}

TEST(AutosaveTimer, ABusyRetryNeverWaitsLongerThanTheInterval)
{
	Autosave::IntervalTimer timer;
	// A one minute interval and a thirty second retry: due thirty seconds on.
	timer.OnResult(Autosave::SaveResult::CardBusy, 1);
	EXPECT_FALSE(Play(timer, 29, 1));
	EXPECT_TRUE(Play(timer, 1, 1));
}

TEST(AutosaveTimer, ChangingTheIntervalTakesEffectAtOnce)
{
	Autosave::IntervalTimer timer;
	EXPECT_FALSE(Play(timer, 100, 30));
	// Lowered to one minute while 100 seconds have already run: due now.
	EXPECT_TRUE(timer.Advance(1.0, 1));
}

// ---- listing ----------------------------------------------------------------------------

namespace
{
	void SetFileTime(const std::string& path, std::time_t t)
	{
		const auto system_now = std::chrono::system_clock::now();
		const auto file_now = fs::file_time_type::clock::now();
		fs::last_write_time(path, file_now + (std::chrono::system_clock::from_time_t(t) - system_now));
	}
} // namespace

TEST(AutosaveList, ListsWhatExistsNewestFirstWithTimes)
{
	Fixture f;
	Write(f.newest, "a");
	Write(f.At(2), "bb");
	Write(f.At(4), "cccc");
	Write(f.Drop(4), "not listed");
	Write(f.dir.File("SLUS-12345 (ABCD1234).01.p2s"), "a numbered slot is not an autosave");
	const std::time_t base = 1'700'000'000;
	SetFileTime(f.newest, base);
	SetFileTime(f.At(2), base - 600);
	SetFileTime(f.At(4), base - 1800);

	const auto entries = Autosave::List(f.newest);
	ASSERT_EQ(entries.size(), 3u);
	EXPECT_EQ(entries[0].index, 1);
	EXPECT_EQ(entries[0].path, f.newest);
	EXPECT_EQ(entries[0].size, 1);
	EXPECT_EQ(entries[1].index, 2);
	EXPECT_EQ(entries[1].path, f.At(2));
	EXPECT_EQ(entries[2].index, 4);
	EXPECT_EQ(entries[2].size, 4);
	EXPECT_NEAR(static_cast<double>(entries[0].modified), static_cast<double>(base), 2.0);
	EXPECT_NEAR(static_cast<double>(entries[1].modified), static_cast<double>(base - 600), 2.0);
	EXPECT_NEAR(static_cast<double>(entries[2].modified), static_cast<double>(base - 1800), 2.0);
}

TEST(AutosaveList, ASetAsideAutosaveWhoseSlotIsEmptyIsPutBackAndListed)
{
	Fixture f;
	Write(f.Drop(1), "the only autosave");
	Write(f.Drop(2), "older");
	Write(f.At(2), "slot two is filled, so its .drop is not an autosave");

	const auto entries = Autosave::List(f.newest);
	ASSERT_EQ(entries.size(), 2u);
	EXPECT_EQ(entries[0].index, 1);
	EXPECT_EQ(Read(entries[0].path), "the only autosave");
	EXPECT_EQ(entries[1].index, 2);
	EXPECT_EQ(Read(entries[1].path), "slot two is filled, so its .drop is not an autosave");
	EXPECT_FALSE(f.Has(f.Drop(1)));
	EXPECT_TRUE(f.Has(f.Drop(2))); // a read-only listing never deletes
}

TEST(AutosaveList, LeavesTheFilesOfASaveThatIsRunningAlone)
{
	// During a save the oldest autosave sits at .drop with its slot empty, which is not a crash.
	Fixture f;
	Write(f.newest, "previous");
	Autosave::Rotation rotation(f.newest, 1);
	ASSERT_TRUE(rotation.Begin());

	EXPECT_TRUE(Autosave::List(f.newest).empty());
	EXPECT_TRUE(f.Has(f.Drop(1)));
	EXPECT_FALSE(f.Has(f.newest));

	rotation.Rollback();
	EXPECT_EQ(Read(f.newest), "previous");
	ASSERT_EQ(Autosave::List(f.newest).size(), 1u);
}

TEST(AutosaveList, IsEmptyWithoutFilesOrAName)
{
	Fixture f;
	EXPECT_TRUE(Autosave::List(f.newest).empty());
	EXPECT_TRUE(Autosave::List(std::string()).empty());
}

TEST(AutosaveList, ANameWithoutP2sListsItselfOnce)
{
	ScopedTempDir dir;
	Write(dir.File("state.bin"), "x");
	const auto entries = Autosave::List(dir.File("state.bin"));
	ASSERT_EQ(entries.size(), 1u);
	EXPECT_EQ(entries[0].index, 1);
}

TEST(AutosaveList, AGameWithoutASerialHasNoAutosaves)
{
	EXPECT_TRUE(Autosave::List("", 0).empty());
}

// ---- settings ---------------------------------------------------------------------------

namespace
{
	Pcsx2Config LoadConfig(MemorySettingsInterface& si)
	{
		Pcsx2Config config;
		SettingsLoadWrapper wrap(si);
		config.LoadSaveCore(wrap);
		return config;
	}
} // namespace

TEST(AutosaveSettings, DefaultsAreOffThreeKeptNothingOnShutdown)
{
	MemorySettingsInterface si;
	const Pcsx2Config config = LoadConfig(si);
	EXPECT_EQ(config.AutosaveIntervalMinutes, 0);
	EXPECT_EQ(config.AutosaveKeep, 3);
	EXPECT_FALSE(config.AutosaveOnShutdown);
	EXPECT_EQ(Pcsx2Config().AutosaveIntervalMinutes, 0);
	EXPECT_EQ(Pcsx2Config().AutosaveKeep, 3);
}

TEST(AutosaveSettings, ReadFromTheEmuCoreSection)
{
	MemorySettingsInterface si;
	si.SetIntValue("EmuCore", "AutosaveIntervalMinutes", 15);
	si.SetIntValue("EmuCore", "AutosaveKeep", 5);
	si.SetBoolValue("EmuCore", "AutosaveOnShutdown", true);
	const Pcsx2Config config = LoadConfig(si);
	EXPECT_EQ(config.AutosaveIntervalMinutes, 15);
	EXPECT_EQ(config.AutosaveKeep, 5);
	EXPECT_TRUE(config.AutosaveOnShutdown);
}

TEST(AutosaveSettings, SaveStateOnShutdownIsIndependent)
{
	MemorySettingsInterface si;
	si.SetBoolValue("EmuCore", "SaveStateOnShutdown", true);
	Pcsx2Config config = LoadConfig(si);
	EXPECT_TRUE(config.SaveStateOnShutdown);
	EXPECT_FALSE(config.AutosaveOnShutdown);

	MemorySettingsInterface other;
	other.SetBoolValue("EmuCore", "AutosaveOnShutdown", true);
	config = LoadConfig(other);
	EXPECT_TRUE(config.AutosaveOnShutdown);
	EXPECT_FALSE(config.SaveStateOnShutdown);
}
