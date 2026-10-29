// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What auto-tune records in a game's settings file: the kept values, what they replaced, and the reset
// that puts them back.

#include <gtest/gtest.h>

#include "AutoTune/AutoTuneStore.h"
#include "INISettingsInterface.h"

#include "common/MemorySettingsInterface.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace AutoTune;

namespace
{
	constexpr const char* GS = "EmuCore/GS";
	constexpr const char* SPEED = "EmuCore/Speedhacks";

	std::string ReadAll(const std::string& path)
	{
		std::ifstream in(path);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	class TempIni
	{
	public:
		TempIni()
			: m_path((std::filesystem::temp_directory_path() / ("autotune_store_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini")).string())
		{
			std::filesystem::remove(m_path);
		}
		~TempIni() { std::filesystem::remove(m_path); }
		const std::string& Path() const { return m_path; }

	private:
		std::string m_path;
	};
} // namespace

TEST(AutoTuneStore, EachLeverWritesItsOwnKeyAsTheRightType)
{
	MemorySettingsInterface si;

	Commit(si, AutoTuneLever::IopThread, 1.0, 0.1);
	EXPECT_TRUE(si.GetBoolValue(SPEED, "iopThread", false));

	Commit(si, AutoTuneLever::EeGovernor, 1.0, 0.1);
	EXPECT_TRUE(si.GetBoolValue(SPEED, "DynamicEECycleRate", false));

	Commit(si, AutoTuneLever::Resolution, 2.0, 0.1);
	EXPECT_EQ(si.GetFloatValue(GS, "upscale_multiplier", 0.0f), 2.0f);

	Commit(si, AutoTuneLever::GsBackThread, 1.0, 0.1);
	EXPECT_EQ(si.GetIntValue(GS, "GSBackThreadMode", 0), 1);

	Commit(si, AutoTuneLever::DownloadMode, AutoTuneDownloadMode::Asynchronous, 0.1);
	EXPECT_EQ(si.GetIntValue(GS, "HWDownloadMode", -1), AutoTuneDownloadMode::Asynchronous);

	Commit(si, AutoTuneLever::Blending, 1.0, 0.1);
	EXPECT_EQ(si.GetIntValue(GS, "accurate_blending_unit", -1), 1);
}

TEST(AutoTuneStore, CommitRecordsWhatWasThereBefore)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);

	const Change c = Commit(si, AutoTuneLever::Resolution, 2.0, 0.121);
	ASSERT_TRUE(c.original.has_value());
	EXPECT_TRUE(SameValue(*c.original, "3"));
	EXPECT_EQ(c.section, GS);
	EXPECT_EQ(c.key, "upscale_multiplier");

	const std::vector<Change> read = ReadChanges(si);
	ASSERT_EQ(read.size(), 1u);
	EXPECT_EQ(read[0].lever, AutoTuneLever::Resolution);
	ASSERT_TRUE(read[0].original.has_value());
	EXPECT_TRUE(SameValue(*read[0].original, "3"));
	EXPECT_TRUE(SameValue(read[0].value, "2"));
	EXPECT_NEAR(read[0].gain, 0.121, 1e-4);
}

TEST(AutoTuneStore, AKeyTheFileDidNotHaveIsRecordedAsAbsent)
{
	MemorySettingsInterface si;
	const Change c = Commit(si, AutoTuneLever::IopThread, 1.0, 0.18);
	EXPECT_FALSE(c.original.has_value());

	const std::vector<Change> read = ReadChanges(si);
	ASSERT_EQ(read.size(), 1u);
	EXPECT_FALSE(read[0].original.has_value());
}

TEST(AutoTuneStore, SeveralStepsOnOneKeyKeepTheFirstOriginalAndCompoundTheGain)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 4.0f);

	Commit(si, AutoTuneLever::Resolution, 3.0, 0.10);
	Commit(si, AutoTuneLever::Resolution, 2.0, 0.20);

	const std::vector<Change> read = ReadChanges(si);
	ASSERT_EQ(read.size(), 1u) << "one record per key";
	ASSERT_TRUE(read[0].original.has_value());
	EXPECT_TRUE(SameValue(*read[0].original, "4"));
	EXPECT_TRUE(SameValue(read[0].value, "2"));
	EXPECT_NEAR(read[0].gain, 1.1 * 1.2 - 1.0, 1e-4);
}

TEST(AutoTuneStore, ACommitAfterThePlayerEditedTheKeyStartsFromTheirValue)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 4.0f);
	Commit(si, AutoTuneLever::Resolution, 2.0, 0.10);

	// The player raises it to 6x in the game's properties. Their value is the one to go back to.
	si.SetFloatValue(GS, "upscale_multiplier", 6.0f);
	Commit(si, AutoTuneLever::Resolution, 5.0, 0.07);

	const std::vector<Change> read = ReadChanges(si);
	ASSERT_EQ(read.size(), 1u);
	EXPECT_TRUE(SameValue(*read[0].original, "6"));
	EXPECT_NEAR(read[0].gain, 0.07, 1e-4);
}

TEST(AutoTuneStore, ResetPutsEveryKeyBackAndDropsTheSection)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	si.SetStringValue("EmuCore", "Unrelated", "keep me");
	Commit(si, AutoTuneLever::Resolution, 2.0, 0.12);
	Commit(si, AutoTuneLever::IopThread, 1.0, 0.18);
	RecordRejected(si, {AutoTuneLever::Blending, 0.0});

	const ResetResult r = Reset(si);
	EXPECT_TRUE(r.had_record);
	EXPECT_EQ(r.restored.size(), 2u);
	EXPECT_TRUE(r.left_alone.empty());

	EXPECT_EQ(si.GetFloatValue(GS, "upscale_multiplier", 0.0f), 3.0f);
	EXPECT_FALSE(si.ContainsValue(SPEED, "iopThread")) << "a key that was absent is deleted, not set to the old default";
	EXPECT_EQ(si.GetStringValue("EmuCore", "Unrelated", ""), "keep me");
	EXPECT_TRUE(si.GetKeyValueList(kStoreSection).empty());
	EXPECT_TRUE(ReadChanges(si).empty());
	EXPECT_TRUE(ReadRejected(si).empty());
}

TEST(AutoTuneStore, ResetLeavesAKeyThePlayerChangedSince)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	Commit(si, AutoTuneLever::Resolution, 2.0, 0.12);
	Commit(si, AutoTuneLever::IopThread, 1.0, 0.18);

	// The player sets the resolution to 5x after auto-tune lowered it.
	si.SetFloatValue(GS, "upscale_multiplier", 5.0f);

	const ResetResult r = Reset(si);
	ASSERT_EQ(r.restored.size(), 1u);
	EXPECT_EQ(r.restored[0].key, "iopThread");
	ASSERT_EQ(r.left_alone.size(), 1u);
	EXPECT_EQ(r.left_alone[0].key, "upscale_multiplier");

	EXPECT_EQ(si.GetFloatValue(GS, "upscale_multiplier", 0.0f), 5.0f) << "the player's value survives";
	EXPECT_FALSE(si.ContainsValue(SPEED, "iopThread"));
	EXPECT_TRUE(si.GetKeyValueList(kStoreSection).empty()) << "the record is gone either way";
}

TEST(AutoTuneStore, ResetLeavesAKeyThePlayerDeleted)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	Commit(si, AutoTuneLever::Resolution, 2.0, 0.12);
	si.DeleteValue(GS, "upscale_multiplier");

	const ResetResult r = Reset(si);
	EXPECT_TRUE(r.restored.empty());
	EXPECT_EQ(r.left_alone.size(), 1u);
	EXPECT_FALSE(si.ContainsValue(GS, "upscale_multiplier"));
}

TEST(AutoTuneStore, ResetSaysWhichLeftAloneKeysAreMissingFromTheFile)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	Commit(si, AutoTuneLever::Resolution, 2.0, 0.12);
	Commit(si, AutoTuneLever::IopThread, 1.0, 0.18);
	Commit(si, AutoTuneLever::Blending, 0.0, 0.05);

	// The Android app writes the file again from its own copy and the resolution is not among the keys it writes,
	// while the player turned the IOP thread off by hand.
	si.DeleteValue(GS, "upscale_multiplier");
	si.SetBoolValue(SPEED, "iopThread", false);

	const ResetResult r = Reset(si);
	ASSERT_EQ(r.restored.size(), 1u);
	EXPECT_EQ(r.restored[0].key, "accurate_blending_unit");
	ASSERT_EQ(r.left_alone.size(), 2u);
	for (const LeftAlone& left : r.left_alone)
	{
		if (left.key == "upscale_multiplier")
		{
			EXPECT_TRUE(left.absent) << "the key is gone from the file";
			EXPECT_TRUE(SameValue(left.value, "2")) << "the value auto-tune wrote is still reported: " << left.value;
		}
		else
		{
			EXPECT_EQ(left.key, "iopThread");
			EXPECT_FALSE(left.absent) << "the file holds another value for it";
			EXPECT_FALSE(left.original.has_value());
		}
	}
}

TEST(AutoTuneStore, ResetOnAFileAutoTuneNeverTouchedDoesNothing)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	const ResetResult r = Reset(si);
	EXPECT_FALSE(r.had_record);
	EXPECT_TRUE(r.restored.empty());
	EXPECT_EQ(si.GetFloatValue(GS, "upscale_multiplier", 0.0f), 3.0f);
}

TEST(AutoTuneStore, ATextTheFileRewroteInAnotherFormStillMatches)
{
	// Android regenerates the file from its own store and may write "2.0" where we wrote "2".
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	Commit(si, AutoTuneLever::Resolution, 2.0, 0.12);
	si.SetStringValue(GS, "upscale_multiplier", "2.000000");
	Commit(si, AutoTuneLever::IopThread, 1.0, 0.1);
	si.SetStringValue(SPEED, "iopThread", "1");

	const ResetResult r = Reset(si);
	EXPECT_EQ(r.restored.size(), 2u);
	EXPECT_TRUE(r.left_alone.empty());
}

TEST(AutoTuneStore, RejectedStepsAreRememberedOnceAndRoundTrip)
{
	MemorySettingsInterface si;
	RecordRejected(si, {AutoTuneLever::Blending, 0.0});
	RecordRejected(si, {AutoTuneLever::Blending, 0.0});
	RecordRejected(si, {AutoTuneLever::Resolution, 2.0});
	RecordRejected(si, {AutoTuneLever::DownloadMode, 5.0});

	const std::vector<AutoTuneRejection> r = ReadRejected(si);
	ASSERT_EQ(r.size(), 3u);
	EXPECT_EQ(r[0].lever, AutoTuneLever::Blending);
	EXPECT_EQ(r[0].value, 0.0);
	EXPECT_EQ(r[1].lever, AutoTuneLever::Resolution);
	EXPECT_EQ(r[1].value, 2.0);
	EXPECT_EQ(r[2].lever, AutoTuneLever::DownloadMode);
	EXPECT_EQ(r[2].value, 5.0);
}

TEST(AutoTuneStore, ACommitDoesNotLoseTheRejectedSteps)
{
	MemorySettingsInterface si;
	RecordRejected(si, {AutoTuneLever::Blending, 0.0});
	Commit(si, AutoTuneLever::IopThread, 1.0, 0.18);
	EXPECT_EQ(ReadRejected(si).size(), 1u);
	EXPECT_EQ(ReadChanges(si).size(), 1u);
	RecordRejected(si, {AutoTuneLever::Resolution, 2.0});
	EXPECT_EQ(ReadChanges(si).size(), 1u);
	EXPECT_EQ(ReadRejected(si).size(), 2u);
}

TEST(AutoTuneStore, MalformedLinesAreIgnored)
{
	MemorySettingsInterface si;
	si.SetStringValue(kStoreSection, "Change.0", "garbage");
	si.SetStringValue(kStoreSection, "Change.1", "nonsense|a|b|=1|2|0.1");
	si.SetStringValue(kStoreSection, "Change.2", "resolution|EmuCore/GS|upscale_multiplier|?|2|0.1");
	si.SetStringValue(kStoreSection, "Rejected.0", "resolution");
	si.SetStringValue(kStoreSection, "Rejected.1", "bogus@2");
	si.SetStringValue(kStoreSection, "Unrelated", "x");
	EXPECT_TRUE(ReadChanges(si).empty());
	EXPECT_TRUE(ReadRejected(si).empty());
	Reset(si);
	EXPECT_TRUE(si.GetKeyValueList(kStoreSection).empty());
}

TEST(AutoTuneStore, SurvivesTheRoundTripThroughAnIniFile)
{
	TempIni file;

	{
		INISettingsInterface ini(file.Path());
		ini.SetFloatValue(GS, "upscale_multiplier", 3.0f);
		ini.SetBoolValue("EmuCore/Gamefixes", "XgKickHack", true);
		Commit(ini, AutoTuneLever::Resolution, 2.0, 0.121);
		Commit(ini, AutoTuneLever::IopThread, 1.0, 0.183);
		RecordRejected(ini, {AutoTuneLever::Blending, 0.0});
		ASSERT_TRUE(ini.Save());
	}

	const std::string text = ReadAll(file.Path());
	EXPECT_NE(text.find("[AutoTune]"), std::string::npos);
	EXPECT_NE(text.find("upscale_multiplier"), std::string::npos);

	{
		INISettingsInterface ini(file.Path());
		ASSERT_TRUE(ini.Load());
		const std::vector<Change> changes = ReadChanges(ini);
		ASSERT_EQ(changes.size(), 2u);
		EXPECT_EQ(changes[0].lever, AutoTuneLever::Resolution);
		EXPECT_TRUE(SameValue(*changes[0].original, "3"));
		EXPECT_TRUE(SameValue(changes[0].value, "2"));
		EXPECT_EQ(changes[1].lever, AutoTuneLever::IopThread);
		EXPECT_FALSE(changes[1].original.has_value());
		EXPECT_EQ(ReadRejected(ini).size(), 1u);

		const ResetResult r = Reset(ini);
		EXPECT_EQ(r.restored.size(), 2u);
		ASSERT_TRUE(ini.Save());
	}

	{
		INISettingsInterface ini(file.Path());
		ASSERT_TRUE(ini.Load());
		EXPECT_EQ(ini.GetFloatValue(GS, "upscale_multiplier", 0.0f), 3.0f);
		EXPECT_FALSE(ini.ContainsValue(SPEED, "iopThread"));
		EXPECT_TRUE(ini.GetBoolValue("EmuCore/Gamefixes", "XgKickHack", false));
		EXPECT_TRUE(ReadChanges(ini).empty());
	}
	EXPECT_EQ(ReadAll(file.Path()).find("[AutoTune]"), std::string::npos);
}

TEST(AutoTuneStore, DescribeReadsAsSentences)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	Commit(si, AutoTuneLever::Resolution, 2.0, 0.121);
	Commit(si, AutoTuneLever::IopThread, 1.0, 0.183);
	si.SetIntValue(GS, "accurate_blending_unit", 2);
	Commit(si, AutoTuneLever::Blending, 1.0, 0.07);
	RecordRejected(si, {AutoTuneLever::Blending, 0.0});
	RecordRejected(si, {AutoTuneLever::DownloadMode, 5.0});

	const std::vector<std::string> lines = Describe(si);
	ASSERT_EQ(lines.size(), 5u);
	EXPECT_EQ(lines[0], "Resolution: 3x -> 2x (+12% speed)");
	EXPECT_EQ(lines[1], "IOP thread: default -> on (+18% speed)");
	EXPECT_EQ(lines[2], "Blending accuracy: Medium -> Basic (+7% speed)");
	EXPECT_EQ(lines[3], "Tried without benefit: Blending accuracy -> Minimum");
	EXPECT_EQ(lines[4], "Tried without benefit: Hardware download mode -> Asynchronous");
}

TEST(AutoTuneStore, DescribeOfAnUntouchedFileIsEmpty)
{
	MemorySettingsInterface si;
	si.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	EXPECT_TRUE(Describe(si).empty());
}

TEST(AutoTuneStore, SameValueComparesMeaningNotSpelling)
{
	EXPECT_TRUE(SameValue("2", "2.000000"));
	EXPECT_TRUE(SameValue("2.0", "2"));
	EXPECT_TRUE(SameValue("true", "1"));
	EXPECT_TRUE(SameValue("false", "0"));
	EXPECT_TRUE(SameValue("abc", "abc"));
	EXPECT_FALSE(SameValue("2", "3"));
	EXPECT_FALSE(SameValue("true", "0"));
	EXPECT_FALSE(SameValue("abc", "abd"));
	EXPECT_FALSE(SameValue("", "0"));
}

TEST(AutoTuneStore, LeverLabelsNameTheValue)
{
	EXPECT_EQ(LeverLabel(AutoTuneLever::Resolution, 2.0), "resolution 2x");
	EXPECT_EQ(LeverLabel(AutoTuneLever::DownloadMode, 5.0), "download mode Asynchronous");
	EXPECT_EQ(LeverLabel(AutoTuneLever::Blending, 0.0), "blending Minimum");
	EXPECT_EQ(LeverLabel(AutoTuneLever::IopThread, 1.0), "IOP thread");
}

TEST(AutoTuneManualChange, OnlyTheKeysAutoTuneWritesAreLeverKeys)
{
	for (int i = 0; i < static_cast<int>(AutoTuneLever::Count); i++)
	{
		const LeverKey k = KeyForLever(static_cast<AutoTuneLever>(i));
		EXPECT_TRUE(IsLeverKey(k.section, k.key)) << k.section << "/" << k.key;
	}

	EXPECT_FALSE(IsLeverKey(GS, "filter"));
	EXPECT_FALSE(IsLeverKey("EmuCore", "AutoTune"));
	// The right key in the wrong section is another setting.
	EXPECT_FALSE(IsLeverKey(SPEED, "upscale_multiplier"));
	EXPECT_FALSE(IsLeverKey(GS, "iopThread"));
}

TEST(AutoTuneManualChange, ChangingALeverTurnsAutoTuneOffForThatGame)
{
	// Each lever, with the global switch on and the game's file saying nothing about it.
	for (int i = 0; i < static_cast<int>(AutoTuneLever::Count); i++)
	{
		const LeverKey k = KeyForLever(static_cast<AutoTuneLever>(i));
		MemorySettingsInterface game;
		EXPECT_TRUE(DisableForManualChange(game, k.section, k.key, true)) << k.key;
		bool value = true;
		ASSERT_TRUE(game.GetBoolValue("EmuCore", "AutoTune", &value)) << k.key;
		EXPECT_FALSE(value) << k.key;
	}
}

TEST(AutoTuneManualChange, AnyOtherSettingLeavesAutoTuneAlone)
{
	MemorySettingsInterface game;
	EXPECT_FALSE(DisableForManualChange(game, GS, "filter", true));
	EXPECT_FALSE(DisableForManualChange(game, "EmuCore", "AutoTune", true));
	EXPECT_FALSE(game.ContainsValue("EmuCore", "AutoTune"));
}

TEST(AutoTuneManualChange, WhenItIsAlreadyOffNothingIsWritten)
{
	// Off in the game's own file.
	MemorySettingsInterface off_here;
	off_here.SetBoolValue("EmuCore", "AutoTune", false);
	EXPECT_FALSE(DisableForManualChange(off_here, GS, "upscale_multiplier", true));
	EXPECT_FALSE(off_here.GetBoolValue("EmuCore", "AutoTune", true));

	// Off globally, and the game's file follows the global setting: writing "off" into it would only
	// pin the game to a value the global setting already gives it.
	MemorySettingsInterface follows_global;
	EXPECT_FALSE(DisableForManualChange(follows_global, GS, "upscale_multiplier", false));
	EXPECT_FALSE(follows_global.ContainsValue("EmuCore", "AutoTune"));
}

TEST(AutoTuneManualChange, AGameThatTurnedItOnAgainstAGlobalOffIsTurnedOff)
{
	MemorySettingsInterface game;
	game.SetBoolValue("EmuCore", "AutoTune", true);
	EXPECT_TRUE(DisableForManualChange(game, SPEED, "iopThread", false));
	EXPECT_FALSE(game.GetBoolValue("EmuCore", "AutoTune", true));
}

TEST(AutoTuneManualChange, TheRecordOfWhatAutoTuneChangedIsKept)
{
	// Turning it off stops further tuning; the changes it already made stay, and so does the way back.
	MemorySettingsInterface game;
	Commit(game, AutoTuneLever::Resolution, 2.0, 0.1);
	game.SetFloatValue(GS, "upscale_multiplier", 4.0f); // the player's change
	EXPECT_TRUE(DisableForManualChange(game, GS, "upscale_multiplier", true));

	ASSERT_EQ(ReadChanges(game).size(), 1u);
	EXPECT_EQ(game.GetFloatValue(GS, "upscale_multiplier", 0.0f), 4.0f);
}
