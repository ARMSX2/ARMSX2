// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AutoTuneApplier against a fake VM, the host decisions it relies on, and the controller and applier run
// together against a game whose speed depends on its settings.

#include <gtest/gtest.h>

#include "AutoTune/AutoTuneApplier.h"
#include "Config.h"
#include "EECycleRate.h"
#include "GameDatabase.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSBackThreadPolicy.h"
#include "INISettingsInterface.h"
#include "common/Console.h"
#include "LayeredSettingsInterface.h"
#include "VMManager.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <tuple>

namespace
{
	using Kind = AutoTuneAction::Kind;
	constexpr const char* GS = "EmuCore/GS";
	constexpr const char* SPEED = "EmuCore/Speedhacks";

	// A VM that does nothing but remember what it was asked. The settings the "emulator" would run with
	// are the trial layer over the game's file, as in the real layer stack.
	class FakeEnv final : public AutoTuneEnv
	{
	public:
		FakeEnv() { m_stack.SetLayer(LayeredSettingsInterface::LAYER_GAME, &game_file); }

		void Attach(SettingsInterface* trial) { m_stack.SetLayer(LayeredSettingsInterface::LAYER_AUTOTUNE, trial); }

		void ApplySettings() override
		{
			applies++;
			if (on_apply)
				on_apply();
		}

		bool EditGameFile(const std::function<void(SettingsInterface&)>& edit) override
		{
			if (!has_file)
				return false;
			edit(game_file);
			edits++;
			return true;
		}

		void ReloadGameSettings() override
		{
			reloads++;
			if (on_reload)
				on_reload();
		}

		void ShowMessage(const std::string& text) override { messages.push_back(text); }
		void Log(const std::string& text) override { logs.push_back(text); }

		void NotifyCommitted(const char* section, const char* key, const std::string& value) override
		{
			committed.emplace_back(section, key, value);
		}

		// What the emulator would read for a key right now.
		std::string Effective(const char* section, const char* key) const { return m_stack.GetStringValue(section, key, "<none>"); }

		MemorySettingsInterface game_file;
		bool has_file = true;
		int applies = 0;
		int reloads = 0;
		int edits = 0;
		std::function<void()> on_apply;
		std::function<void()> on_reload;
		std::vector<std::string> messages;
		std::vector<std::string> logs;
		std::vector<std::tuple<std::string, std::string, std::string>> committed;

	private:
		LayeredSettingsInterface m_stack;
	};

	// Exceptions are off in this binary, so no std::stof on text that may not be a number.
	double Num(const std::string& text, double fallback)
	{
		char* end = nullptr;
		const double v = std::strtod(text.c_str(), &end);
		return (end != text.c_str() && *end == '\0') ? v : fallback;
	}

	// A memory interface writes a boolean as "1", an INI file as "true".
	bool IsOn(const std::string& text)
	{
		return text == "1" || text == "true";
	}

	AutoTuneAction Action(Kind kind, AutoTuneLever lever, double value)
	{
		AutoTuneAction a;
		a.kind = kind;
		a.lever = lever;
		a.value = value;
		a.bottleneck = AutoTuneBottleneck::Gpu;
		a.speed_before = 50.0;
		a.speed_after = 56.0;
		a.gain = 0.12;
		a.measured = true;
		return a;
	}

	struct Fixture
	{
		FakeEnv env;
		AutoTuneApplier applier{env};
		Fixture() { env.Attach(applier.TrialLayer()); }
	};
} // namespace

// ---------------------------------------------------------------------------------------------- the actions

TEST(AutoTuneApplier, ATrialIsInTheLayerBeforeTheSettingsAreApplied)
{
	Fixture f;
	f.env.game_file.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	std::string seen_at_apply;
	f.env.on_apply = [&] { seen_at_apply = f.env.Effective(GS, "upscale_multiplier"); };

	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 2.0));

	EXPECT_EQ(f.env.applies, 1);
	EXPECT_EQ(Num(seen_at_apply, -1.0), 2.0) << "the trial wins over the game's file";
	EXPECT_TRUE(f.applier.TrialActive());
	EXPECT_EQ(f.env.edits, 0) << "a trial does not touch the game's file";
	ASSERT_EQ(f.env.messages.size(), 1u);
	EXPECT_EQ(f.env.messages[0], "Auto-tune: trying resolution 2x...");
}

TEST(AutoTuneApplier, KeepMovesTheValueFromTheTrialIntoTheGamesFile)
{
	Fixture f;
	f.env.game_file.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 2.0));

	// By the time the game layer is re-read the file already holds the value, so it is never missing.
	std::string effective_at_reload;
	f.env.on_reload = [&] { effective_at_reload = f.env.Effective(GS, "upscale_multiplier"); };

	f.applier.Execute(Action(Kind::Keep, AutoTuneLever::Resolution, 2.0));

	EXPECT_FALSE(f.applier.TrialActive());
	EXPECT_EQ(f.env.game_file.GetFloatValue(GS, "upscale_multiplier", 0.0f), 2.0f);
	EXPECT_FALSE(f.applier.TrialLayer()->ContainsValue(GS, "upscale_multiplier")) << "the trial layer is empty again";
	EXPECT_EQ(f.env.reloads, 1);
	EXPECT_EQ(Num(effective_at_reload, -1.0), 2.0);

	ASSERT_EQ(f.env.committed.size(), 1u);
	EXPECT_EQ(std::get<0>(f.env.committed[0]), GS);
	EXPECT_EQ(std::get<1>(f.env.committed[0]), "upscale_multiplier");
	EXPECT_TRUE(AutoTune::SameValue(std::get<2>(f.env.committed[0]), "2"));

	EXPECT_EQ(f.env.messages.back(), "Auto-tune: resolution 2x kept (+12%)");

	// And the file says what it was.
	const std::vector<AutoTune::Change> changes = AutoTune::ReadChanges(f.env.game_file);
	ASSERT_EQ(changes.size(), 1u);
	EXPECT_TRUE(AutoTune::SameValue(*changes[0].original, "3"));
}

TEST(AutoTuneApplier, TheGovernorsKeepMessageSaysTheEmulatedCpuIsUnderclockedToHoldGameSpeed)
{
	// "EE governor kept (+69%)" read like a frame-rate gain. It is game speed: the governor lowers the emulated
	// EE's clock so the game keeps up with real time, at the same number of pictures drawn.
	Fixture f;
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::EeGovernor, 1.0));
	AutoTuneAction keep = Action(Kind::Keep, AutoTuneLever::EeGovernor, 1.0);
	keep.gain = 0.69;
	f.applier.Execute(keep);

	EXPECT_EQ(f.env.messages.back(),
		"Auto-tune: EE governor kept. The emulated CPU is underclocked to hold game speed (game speed +69%, not more frames).");

	// Another lever keeps its plain message.
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::IopThread, 1.0));
	f.applier.Execute(Action(Kind::Keep, AutoTuneLever::IopThread, 1.0));
	EXPECT_EQ(f.env.messages.back(), "Auto-tune: IOP thread kept (+12%)");
}

TEST(AutoTuneApplier, AKeptMessageNeverCarriesANegativeNumber)
{
	// A trial can be kept without a speed gain: it reached full speed with the late frames gone, on a game
	// that was at full speed all along. The speed can then read a little under the baseline, and "kept (-1%)"
	// reads as a loss. The message, the game's record and the list of changes show 0 at the least.
	Fixture f;
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::IopThread, 1.0));
	AutoTuneAction keep = Action(Kind::Keep, AutoTuneLever::IopThread, 1.0);
	keep.gain = -0.012;
	f.applier.Execute(keep);
	EXPECT_EQ(f.env.messages.back(), "Auto-tune: IOP thread kept (+0%)");

	const std::vector<AutoTune::Change> changes = AutoTune::ReadChanges(f.env.game_file);
	ASSERT_EQ(changes.size(), 1u);
	EXPECT_GE(changes[0].gain, 0.0);
	for (const std::string& line : AutoTune::Describe(f.env.game_file))
		EXPECT_EQ(line.find("(-"), std::string::npos) << line;

	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::EeGovernor, 1.0));
	keep = Action(Kind::Keep, AutoTuneLever::EeGovernor, 1.0);
	keep.gain = -0.004;
	f.applier.Execute(keep);
	EXPECT_NE(f.env.messages.back().find("game speed +0%"), std::string::npos) << f.env.messages.back();
}

TEST(AutoTuneApplier, KeepWithNoSettingsFileHoldsTheValueForTheSession)
{
	Fixture f;
	f.env.has_file = false;
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::IopThread, 1.0));
	f.applier.Execute(Action(Kind::Keep, AutoTuneLever::IopThread, 1.0));

	EXPECT_FALSE(f.applier.TrialActive());
	EXPECT_EQ(f.env.reloads, 0);
	EXPECT_TRUE(f.env.committed.empty()) << "nothing was written, so the host has nothing to mirror";
	EXPECT_TRUE(IsOn(f.env.Effective(SPEED, "iopThread"))) << "still in force this session";
	EXPECT_EQ(f.env.messages.back(), "Auto-tune: IOP thread kept (+12%)");
}

TEST(AutoTuneApplier, ARevertedTrialRestoresWhatTheLayerHeldBefore)
{
	Fixture f;
	f.env.has_file = false;

	// 2x was kept for the session only; the next trial is 1x and does not help.
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 2.0));
	f.applier.Execute(Action(Kind::Keep, AutoTuneLever::Resolution, 2.0));
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 1.0));
	EXPECT_EQ(Num(f.env.Effective(GS, "upscale_multiplier"), -1.0), 1.0);

	f.applier.Execute(Action(Kind::Revert, AutoTuneLever::Resolution, 1.0));
	EXPECT_EQ(Num(f.env.Effective(GS, "upscale_multiplier"), -1.0), 2.0) << "back to the kept value, not to nothing";
}

TEST(AutoTuneApplier, ARevertClearsTheTrialAppliesAndRemembersTheFailure)
{
	Fixture f;
	f.env.game_file.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 2.0));
	f.env.applies = 0;

	f.applier.Execute(Action(Kind::Revert, AutoTuneLever::Resolution, 2.0));

	EXPECT_FALSE(f.applier.TrialActive());
	EXPECT_EQ(Num(f.env.Effective(GS, "upscale_multiplier"), -1.0), 3.0);
	EXPECT_EQ(f.env.applies, 1);
	EXPECT_EQ(f.env.game_file.GetFloatValue(GS, "upscale_multiplier", 0.0f), 3.0f) << "the file was never changed";
	ASSERT_EQ(f.applier.Rejections().size(), 1u);
	EXPECT_EQ(f.applier.Rejections()[0].lever, AutoTuneLever::Resolution);
	EXPECT_EQ(f.applier.Rejections()[0].value, 2.0);

	const std::vector<AutoTuneRejection> persisted = AutoTune::ReadRejected(f.env.game_file);
	ASSERT_EQ(persisted.size(), 1u) << "remembered across sessions";
	EXPECT_EQ(f.env.messages.back(), "Auto-tune: resolution 2x did not help, undone");
}

TEST(AutoTuneApplier, ACancelledTrialIsNotRememberedAsAFailure)
{
	Fixture f;
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::GsBackThread, 1.0));
	AutoTuneAction cancel = Action(Kind::Revert, AutoTuneLever::GsBackThread, 1.0);
	cancel.measured = false;
	f.applier.Execute(cancel);

	EXPECT_TRUE(f.applier.Rejections().empty());
	EXPECT_TRUE(AutoTune::ReadRejected(f.env.game_file).empty());
	EXPECT_EQ(f.env.Effective(GS, "GSBackThreadMode"), "<none>");
	EXPECT_EQ(f.env.applies, 2) << "applied once to start, once to take back";
}

TEST(AutoTuneApplier, AnInconclusiveTrialIsUndoneSaidSoAndNeverWrittenToTheFile)
{
	Fixture f;
	f.env.game_file.SetFloatValue(GS, "upscale_multiplier", 3.0f);
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 2.0));

	AutoTuneAction undo = Action(Kind::Revert, AutoTuneLever::Resolution, 2.0);
	undo.measured = false;
	undo.inconclusive = true;
	undo.gain = 0.04;
	f.applier.Execute(undo);

	EXPECT_FALSE(f.applier.TrialActive());
	EXPECT_EQ(Num(f.env.Effective(GS, "upscale_multiplier"), -1.0), 3.0);
	EXPECT_TRUE(AutoTune::ReadRejected(f.env.game_file).empty()) << "a result that could be noise must not be written for good";
	EXPECT_TRUE(f.applier.Rejections().empty());
	EXPECT_EQ(f.env.messages.back(), "Auto-tune: resolution 2x made no clear difference, undone");
	EXPECT_NE(f.env.logs.back().find("inconclusive"), std::string::npos) << f.env.logs.back();
	EXPECT_NE(f.env.logs.back().find("not remembered"), std::string::npos) << f.env.logs.back();
}

TEST(AutoTuneApplier, ATrialUndoneBecauseTheGameChangedIsNeitherRememberedNorHeldBack)
{
	Fixture f;
	f.env.game_file.SetFloatValue(GS, "upscale_multiplier", 5.0f);
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 4.0));

	AutoTuneAction undo = Action(Kind::Revert, AutoTuneLever::Resolution, 4.0);
	undo.measured = false;
	undo.inconclusive = true;
	undo.retry = true;
	undo.gain = -0.11;
	f.applier.Execute(undo);

	EXPECT_FALSE(f.applier.TrialActive());
	EXPECT_EQ(Num(f.env.Effective(GS, "upscale_multiplier"), -1.0), 5.0);
	EXPECT_TRUE(AutoTune::ReadRejected(f.env.game_file).empty());
	EXPECT_TRUE(f.applier.Rejections().empty()) << "the same step comes round again";
	EXPECT_EQ(f.env.messages.back(), "Auto-tune: the game changed while resolution 4x was tried, undone; measuring again");
	EXPECT_NE(f.env.logs.back().find("the game changed"), std::string::npos) << f.env.logs.back();
	EXPECT_NE(f.env.logs.back().find("not remembered"), std::string::npos) << f.env.logs.back();
}

TEST(AutoTuneApplier, TheLogSaysWhatTheTargetedCostDid)
{
	Fixture f;
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Blending, 4.0));
	AutoTuneAction keep = Action(Kind::Keep, AutoTuneLever::Blending, 4.0);
	keep.cost_before = 127.0;
	keep.cost_after = 4.75;
	keep.cost_gain = 0.9626;
	f.applier.Execute(keep);

	bool found = false;
	for (const std::string& line : f.env.logs)
		found = found || line.find("target cost 127.00 -> 4.75 ms per frame (-96%)") != std::string::npos;
	EXPECT_TRUE(found) << "the keep line carries the cost that earned it";
}

TEST(AutoTuneApplier, CancelTrialAppliesOnlyWhenAsked)
{
	Fixture f;
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::IopThread, 1.0));
	f.env.applies = 0;

	f.applier.CancelTrial(false);
	EXPECT_FALSE(f.applier.TrialActive());
	EXPECT_EQ(f.env.Effective(SPEED, "iopThread"), "<none>");
	EXPECT_EQ(f.env.applies, 0);

	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::IopThread, 1.0));
	f.env.applies = 0;
	f.applier.CancelTrial(true);
	EXPECT_EQ(f.env.applies, 1);

	f.applier.CancelTrial(true);
	EXPECT_EQ(f.env.applies, 1) << "nothing to cancel";
}

TEST(AutoTuneApplier, ATrialThatIsNotKeptNeverReachesTheGamesFileOrTheHost)
{
	// A trial value lives in the trial layer and nowhere else until it is kept. The Android app keeps its own copy
	// of the per-game settings and is told about one thing, a committed key, so the value of a trial that was
	// undone, cancelled or cut short must not appear in the game's file as a setting or in what the host was told.
	struct Trial
	{
		AutoTuneLever lever;
		double value;
	};
	const Trial trials[] = {
		{AutoTuneLever::IopThread, 1.0},
		{AutoTuneLever::EeGovernor, 1.0},
		{AutoTuneLever::Resolution, 2.0},
		{AutoTuneLever::GsBackThread, 1.0},
		{AutoTuneLever::DownloadMode, AutoTuneDownloadMode::Asynchronous},
		{AutoTuneLever::Blending, 0.0},
	};

	for (const Trial& t : trials)
	{
		const AutoTune::LeverKey key = AutoTune::KeyForLever(t.lever);
		const auto ends = {0, 1, 2, 3, 4}; // measured, inconclusive, retry, cancelled, CancelTrial()
		for (const int end : ends)
		{
			Fixture f;
			f.applier.Execute(Action(Kind::StartTrial, t.lever, t.value));
			ASSERT_TRUE(f.applier.TrialLayer()->ContainsValue(key.section, key.key)) << AutoTuneLeverName(t.lever);

			if (end == 4)
			{
				f.applier.CancelTrial(true);
			}
			else
			{
				AutoTuneAction undo = Action(Kind::Revert, t.lever, t.value);
				undo.measured = end == 0;
				undo.inconclusive = end == 1 || end == 2;
				undo.retry = end == 2;
				f.applier.Execute(undo);
			}

			EXPECT_FALSE(f.applier.TrialLayer()->ContainsValue(key.section, key.key)) << AutoTuneLeverName(t.lever) << " ended " << end;
			EXPECT_FALSE(f.env.game_file.ContainsValue(key.section, key.key)) << AutoTuneLeverName(t.lever) << " ended " << end;
			EXPECT_TRUE(f.env.committed.empty()) << "the host is told about a kept key only: " << AutoTuneLeverName(t.lever) << " ended " << end;
			EXPECT_TRUE(AutoTune::ReadChanges(f.env.game_file).empty()) << "no change was recorded";
			EXPECT_EQ(f.env.Effective(key.section, key.key), "<none>") << "what the emulator reads is what it read before";
		}
	}
}

TEST(AutoTuneApplier, BeginEmptiesTheLayerAndLoadsTheRememberedFailures)
{
	Fixture f;
	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 2.0));
	f.applier.Begin({{AutoTuneLever::Blending, 0.0}});

	EXPECT_FALSE(f.applier.TrialActive());
	EXPECT_FALSE(f.applier.TrialLayer()->ContainsValue(GS, "upscale_multiplier"));
	ASSERT_EQ(f.applier.Rejections().size(), 1u);
	EXPECT_EQ(f.applier.Rejections()[0].lever, AutoTuneLever::Blending);

	f.applier.Execute(Action(Kind::StartTrial, AutoTuneLever::Resolution, 2.0));
	f.applier.End();
	EXPECT_TRUE(f.applier.TrialLayer()->IsEmpty());
}

TEST(AutoTuneApplier, EveryLeverWritesTheKeyTheConfigurationReads)
{
	Fixture f;
	f.env.has_file = false; // kept for the session, so each value stays in the layer
	const auto run = [&](AutoTuneLever lever, double value) {
		f.applier.Execute(Action(Kind::StartTrial, lever, value));
		f.applier.Execute(Action(Kind::Keep, lever, value));
	};
	run(AutoTuneLever::IopThread, 1.0);
	run(AutoTuneLever::GsBackThread, 1.0);
	run(AutoTuneLever::DownloadMode, AutoTuneDownloadMode::Asynchronous);
	run(AutoTuneLever::Blending, 1.0);
	run(AutoTuneLever::EeGovernor, 1.0);

	EXPECT_TRUE(IsOn(f.env.Effective(SPEED, "iopThread")));
	EXPECT_EQ(f.env.Effective(GS, "GSBackThreadMode"), "1");
	EXPECT_EQ(f.env.Effective(GS, "HWDownloadMode"), "5");
	EXPECT_EQ(f.env.Effective(GS, "accurate_blending_unit"), "1");
	EXPECT_TRUE(IsOn(f.env.Effective(SPEED, "DynamicEECycleRate")));
}

TEST(AutoTuneApplier, ExhaustedAndReportsSayTheBottleneck)
{
	Fixture f;
	AutoTuneAction a;
	a.kind = Kind::Exhausted;
	a.bottleneck = AutoTuneBottleneck::GsCpu;
	f.applier.Execute(a);
	EXPECT_EQ(f.env.messages.back(), "Auto-tune: nothing left to try (bottleneck: GS (CPU))");

	a.kind = Kind::ReportBottleneck;
	a.bottleneck = AutoTuneBottleneck::Vu1;
	f.applier.Execute(a);
	EXPECT_NE(f.env.messages.back().find("VU1"), std::string::npos);

	a.bottleneck = AutoTuneBottleneck::Iop;
	f.applier.Execute(a);
	EXPECT_NE(f.env.messages.back().find("IOP thread"), std::string::npos);

	const size_t messages = f.env.messages.size();
	a.kind = Kind::Unclassified;
	f.applier.Execute(a);
	EXPECT_EQ(f.env.messages.size(), messages) << "logged only";
	EXPECT_FALSE(f.env.logs.empty());
}

TEST(AutoTuneApplier, NothingIsAppliedForNoAction)
{
	Fixture f;
	f.applier.Execute(AutoTuneAction());
	EXPECT_EQ(f.env.applies, 0);
	EXPECT_TRUE(f.env.messages.empty());
}

// ---------------------------------------------------------------------------------------------- host decisions

TEST(AutoTuneHost, TheGameDatabaseForcesWhatItNames)
{
	GameDatabaseSchema::GameEntry entry;
	EXPECT_FALSE(AutoTune::ForcedByGameDb(&entry).iop_thread);

	entry.speedHacks = {{SpeedHack::IOPThread, 0}};
	entry.gsHWFixes = {{GameDatabaseSchema::GSHWFixId::HWDownloadMode, 2},
		{GameDatabaseSchema::GSHWFixId::MinimumBlendingLevel, 3},
		{GameDatabaseSchema::GSHWFixId::MaximumBlendingLevel, 4}};
	AutoTune::GameDbForced forced = AutoTune::ForcedByGameDb(&entry);
	EXPECT_TRUE(forced.iop_thread);
	EXPECT_FALSE(forced.ee_governor);
	EXPECT_TRUE(forced.download_mode);
	EXPECT_EQ(forced.min_blending, 3) << "only the minimum matters; auto-tune only lowers";

	entry.speedHacks = {{SpeedHack::EECycleRate, -1}};
	forced = AutoTune::ForcedByGameDb(&entry);
	EXPECT_FALSE(forced.iop_thread);
	EXPECT_TRUE(forced.ee_governor);
}

TEST(AutoTuneHost, NoGameEntryForcesNothing)
{
	const AutoTune::GameDbForced forced = AutoTune::ForcedByGameDb(nullptr);
	EXPECT_FALSE(forced.iop_thread);
	EXPECT_FALSE(forced.ee_governor);
	EXPECT_FALSE(forced.download_mode);
	EXPECT_EQ(forced.min_blending, 0);
}

TEST(AutoTuneHost, TheIopThreadIsPlaceableWhereResolveModePlacesIt)
{
	IopThread::HostCpuInfo host;
	host.is_arm64 = true;
	host.total_cores = 8;
	host.performance_cores = 4;
	const IopThread::VmConditions vm;

	EXPECT_TRUE(AutoTune::IopThreadPlaceable(false, false, host, vm, false));
	EXPECT_TRUE(AutoTune::IopThreadPlaceable(true, false, host, vm, false)) << "EE, GS, IOP, MTVU = 4 performance cores";

	host.is_arm64 = false;
	EXPECT_FALSE(AutoTune::IopThreadPlaceable(false, false, host, vm, false));
}

TEST(AutoTuneHost, TheGsBackThreadCountsAsABusyCore)
{
	IopThread::HostCpuInfo host;
	host.is_arm64 = true;
	host.total_cores = 6;
	host.performance_cores = 3;
	const IopThread::VmConditions vm;

	// EE, GS and IOP fit on three performance cores, and six cores leave two for the rest...
	EXPECT_TRUE(AutoTune::IopThreadPlaceable(false, false, host, vm, false));
	// ...but not once the back thread has one of them.
	EXPECT_FALSE(AutoTune::IopThreadPlaceable(false, false, host, vm, true));

	// MTVU makes it four threads.
	host.total_cores = 8;
	host.performance_cores = 4;
	EXPECT_TRUE(AutoTune::IopThreadPlaceable(true, false, host, vm, false));
	EXPECT_FALSE(AutoTune::IopThreadPlaceable(true, false, host, vm, true));
}

TEST(AutoTuneHost, TheGsBackThreadIsNotOfferedWhereItWouldShareABusyCore)
{
	// MQ65 with the IOP thread on: EE, MTVU, the front and the IOP thread hold the four big cores, and the back
	// thread would be placed on one of them. A/B on the device: 66.6% without it, 53.7% with it.
	GSBackThreadInputs in;
	in.requested = true;
	in.hardware_renderer = true;
	in.vulkan = true;
	in.download_mode = GSHardwareDownloadMode::Enabled;

	EXPECT_TRUE(AutoTune::GsBackThreadOffered(in, false));
	EXPECT_FALSE(AutoTune::GsBackThreadOffered(in, true));

	// The renderer and the download mode rule it out first, as for the setting itself.
	in.vulkan = false;
	EXPECT_FALSE(AutoTune::GsBackThreadOffered(in, false));
	in.vulkan = true;
	in.download_mode = GSHardwareDownloadMode::Unsynchronized;
	EXPECT_FALSE(AutoTune::GsBackThreadOffered(in, false));
}

TEST(AutoTuneHost, TheBackThreadIsJudgedByTheLiveDeviceNotByTheConfiguredRenderer)
{
	// After a renderer reopen the configured renderer reads Auto (GSreopen passes the unresolved setting on), so
	// asking it whether this is Vulkan said no for the rest of the session and the lever vanished. The device
	// that is open knows what it is.
	const auto offered = [](RenderAPI api) {
		const GSBackThreadInputs in = AutoTune::BackThreadInputs(true, api, GSHardwareDownloadMode::Enabled);
		return AutoTune::GsBackThreadOffered(in, false);
	};
	EXPECT_TRUE(offered(RenderAPI::Vulkan));
	EXPECT_FALSE(offered(RenderAPI::OpenGL));
	EXPECT_FALSE(offered(RenderAPI::None)) << "no device is open";

	// The software renderer queues on any API.
	EXPECT_TRUE(AutoTune::GsBackThreadOffered(AutoTune::BackThreadInputs(false, RenderAPI::OpenGL, GSHardwareDownloadMode::Enabled), false));

	// With no VM there is no device to ask.
	EXPECT_EQ(GSGetCurrentRenderAPI(), RenderAPI::None);
}

TEST(AutoTuneHost, ACoreCountThatIsUnknownIsNotPlaceable)
{
	IopThread::HostCpuInfo host;
	host.is_arm64 = true;
	host.total_cores = 8;
	host.performance_cores = 0; // the cores cannot be told apart
	const IopThread::VmConditions vm;
	EXPECT_FALSE(AutoTune::IopThreadPlaceable(false, false, host, vm, false));
	EXPECT_FALSE(AutoTune::IopThreadPlaceable(false, false, host, vm, true));
}

TEST(AutoTuneHost, LockstepAndVmConditionsRuleItOut)
{
	IopThread::HostCpuInfo host;
	host.is_arm64 = true;
	host.total_cores = 8;
	host.performance_cores = 4;

	EXPECT_FALSE(AutoTune::IopThreadPlaceable(false, true, host, IopThread::VmConditions(), false)) << "lockstep is a test mode";

	IopThread::VmConditions vm;
	vm.arcade = true;
	EXPECT_FALSE(AutoTune::IopThreadPlaceable(false, false, host, vm, false));
	vm = IopThread::VmConditions();
	vm.ps1_mode = true;
	EXPECT_FALSE(AutoTune::IopThreadPlaceable(false, false, host, vm, false));
}

// ---------------------------------------------------------------------------------------------- the governor lever

namespace
{
	// A VM running normally with nothing suspending the governor, nobody claiming the rate, and the governor
	// off: the state an auto-tune trial starts from.
	EECycleRate::EligibilityInputs Plain()
	{
		EECycleRate::EligibilityInputs in;
		in.configured = 0;
		in.limiter_nominal = true;
		in.vm_running = true;
		return in;
	}

	bool Available(const EECycleRate::EligibilityInputs& in)
	{
		return AutoTune::GovernorAvailableFrom(EECycleRate::ResolveGovernorStatus(in, in.configured), in.dynamic_setting);
	}
} // namespace

TEST(AutoTuneGovernor, IsAvailableWhereTheGovernorsOwnResolverWouldRunIt)
{
	EXPECT_TRUE(Available(Plain()));

	// Already on: nothing for a trial to switch, but nothing that makes it unavailable either.
	EECycleRate::EligibilityInputs on = Plain();
	on.dynamic_setting = true;
	EXPECT_TRUE(Available(on));
}

TEST(AutoTuneGovernor, IsNotAvailableWhatEverSuspendsIt)
{
	const auto without = [](auto&& tweak) {
		EECycleRate::EligibilityInputs in = Plain();
		tweak(in);
		return Available(in);
	};

	EXPECT_FALSE(without([](auto& in) { in.hardcore = true; }));
	EXPECT_FALSE(without([](auto& in) { in.ee_cycle_skip = 1; }));
	EXPECT_FALSE(without([](auto& in) { in.limiter_nominal = false; }));
	EXPECT_FALSE(without([](auto& in) { in.paced_by_host_vsync = true; })) << "Throttle() never measures a frame";
	EXPECT_FALSE(without([](auto& in) { in.gs_dump_replay = true; }));
	EXPECT_FALSE(without([](auto& in) { in.frame_advancing = true; }));
	EXPECT_FALSE(without([](auto& in) { in.vm_running = false; }));
}

TEST(AutoTuneGovernor, NeedsARateToLowerFrom)
{
	// The floor is two steps under the player's rate, never under -3. At -3 there is nothing to give back.
	EECycleRate::EligibilityInputs in = Plain();
	in.configured = -3;
	EXPECT_FALSE(Available(in));
	in.configured = -2;
	EXPECT_TRUE(Available(in));
	in.configured = 3;
	EXPECT_TRUE(Available(in));
}

TEST(AutoTuneGovernor, ASwitchThatIsOnButHeldOffByAPinnedRateIsNotATrialThatWouldChangeAnything)
{
	// The global switch is on and the game's file pins the rate: the resolver keeps the governor out. A trial
	// writes the same "on" the global switch already says, so the configuration would not change, the caches
	// would not reset, and the sampler would never be told to start.
	EECycleRate::EligibilityInputs in = Plain();
	in.dynamic_setting = true;
	in.pergame_claims_rate = true;
	EXPECT_FALSE(Available(in));

	// With the switch off, the same pin is no obstacle: a trial turns it on, and the trial layer counts as a
	// per-game claim on it (VMManager::LoadCoreSettings), as the kept key would.
	in.dynamic_setting = false;
	EXPECT_TRUE(Available(in));
}

TEST(AutoTuneGovernor, ATrialOnlyCountsWhenTheGovernorLowersTheRate)
{
	using L = AutoTuneLever;
	// The governor acts by lowering the EE rate. A trial in which it never did measured a game it did not touch.
	EXPECT_FALSE(AutoTune::TrialWasMeasured(L::EeGovernor, 4, 4));
	EXPECT_TRUE(AutoTune::TrialWasMeasured(L::EeGovernor, 4, 5));
	EXPECT_TRUE(AutoTune::TrialWasMeasured(L::EeGovernor, 4, 6));

	// Every other lever is measured by its own effect on the speed.
	EXPECT_TRUE(AutoTune::TrialWasMeasured(L::IopThread, 4, 4));
	EXPECT_TRUE(AutoTune::TrialWasMeasured(L::Resolution, 4, 4));
}

// ---------------------------------------------------------------------------------------------- together

namespace
{
	// A game whose speed depends on its settings, run through the controller and the applier. `speed_of` maps
	// the settings in force to a speed. The windows are what the sampler would hand over.
	struct Game
	{
		Fixture fixture;
		AutoTuneController controller;
		std::function<double(const FakeEnv&)> speed_of;
		std::vector<std::string> trace;
		double now = 0.0; // seconds played
		bool governor_available = false;

		AutoTuneLeverState State() const
		{
			AutoTuneLeverState s;
			s.hw_renderer = true;
			s.resolution = Num(fixture.env.Effective(GS, "upscale_multiplier"), 1.0);
			s.download_mode = AutoTuneDownloadMode::NoReadbacks;
			s.iop_thread_on = IsOn(fixture.env.Effective(SPEED, "iopThread"));
			s.iop_thread_placeable = true;
			s.ee_governor_on = IsOn(fixture.env.Effective(SPEED, "DynamicEECycleRate"));
			s.ee_governor_eligible = governor_available;
			s.rejected = fixture.applier.Rejections();
			return s;
		}

		void Play(const AutoTuneWindow& shape, double seconds)
		{
			for (double t = 0.0; t < seconds; t += 0.5)
			{
				AutoTuneWindow w = shape;
				w.usable = true;
				w.speed_pct = speed_of(fixture.env);
				const AutoTuneAction a = controller.OnWindow(w, State());
				fixture.applier.Execute(a);
				now += 0.5;
			}
		}
	};
} // namespace

TEST(AutoTuneTogether, AnEeBoundGameGetsTheIopThreadAndKeepsItInTheFile)
{
	Game game;
	game.fixture.env.game_file.SetBoolValue("EmuCore/Gamefixes", "XgKickHack", true);
	game.speed_of = [](const FakeEnv& env) { return IsOn(env.Effective(SPEED, "iopThread")) ? 99.0 : 70.0; };

	AutoTuneWindow ee_bound;
	ee_bound.ee_pct = 98.0;
	ee_bound.mtgs_pct = 30.0;
	game.Play(ee_bound, 60.0);

	EXPECT_EQ(game.fixture.env.game_file.GetBoolValue(SPEED, "iopThread", false), true);
	EXPECT_EQ(game.fixture.env.game_file.GetBoolValue("EmuCore/Gamefixes", "XgKickHack", false), true);
	EXPECT_FALSE(game.fixture.applier.TrialLayer()->ContainsValue(SPEED, "iopThread"));
	ASSERT_EQ(game.fixture.env.committed.size(), 1u);
	EXPECT_EQ(std::get<1>(game.fixture.env.committed[0]), "iopThread");

	const std::vector<std::string> lines = AutoTune::Describe(game.fixture.env.game_file);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("IOP thread: default -> on"), std::string::npos) << lines[0];

	ASSERT_GE(game.fixture.env.messages.size(), 2u);
	EXPECT_EQ(game.fixture.env.messages.front(), "Auto-tune: trying IOP thread...");
	EXPECT_NE(game.fixture.env.messages.back().find("IOP thread kept (+41%)"), std::string::npos) << game.fixture.env.messages.back();
	EXPECT_EQ(game.fixture.env.reloads, 1);
}

TEST(AutoTuneTogether, AnEeBoundGameWithTheIopThreadAlreadyOnGetsTheGovernorAndKeepsItInTheFile)
{
	Game game;
	game.governor_available = true;
	game.fixture.env.game_file.SetBoolValue(SPEED, "iopThread", true); // the IOP thread is on already

	// The governor takes its time: it warms up for two seconds of game time, waits two windows, steps down,
	// cools down for two more, and steps again. Nothing changes on the wall until about 12 s after the switch.
	double governor_on_at = -1.0;
	game.speed_of = [&](const FakeEnv& env) {
		if (!IsOn(env.Effective(SPEED, "DynamicEECycleRate")))
		{
			governor_on_at = -1.0;
			return 70.0;
		}
		if (governor_on_at < 0.0)
			governor_on_at = game.now;
		return (game.now - governor_on_at) < 12.0 ? 70.0 : 99.0;
	};

	AutoTuneWindow ee_bound;
	ee_bound.ee_pct = 98.0;
	ee_bound.mtgs_pct = 30.0;
	game.Play(ee_bound, 80.0);

	EXPECT_EQ(game.fixture.env.game_file.GetBoolValue(SPEED, "DynamicEECycleRate", false), true) << "kept in the file";
	EXPECT_FALSE(game.fixture.applier.TrialLayer()->ContainsValue(SPEED, "DynamicEECycleRate"));
	EXPECT_EQ(game.fixture.env.game_file.GetBoolValue(SPEED, "iopThread", false), true) << "the IOP thread was not touched";
	ASSERT_EQ(game.fixture.env.committed.size(), 1u);
	EXPECT_EQ(std::get<1>(game.fixture.env.committed[0]), "DynamicEECycleRate");

	const std::vector<std::string> lines = AutoTune::Describe(game.fixture.env.game_file);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("EE cycle-rate governor"), std::string::npos) << lines[0];
	EXPECT_TRUE(AutoTune::ReadRejected(game.fixture.env.game_file).empty());
	EXPECT_EQ(game.fixture.env.messages.front(), "Auto-tune: trying EE governor...");
}

TEST(AutoTuneTogether, AGovernorThatSettlesTooShortWouldHaveBeenRejected)
{
	// The same game with the governor's clocks not honoured (a settle of 6 s and four windows, as for the
	// other levers): the trial measures the game before the governor has acted and calls it useless. This is
	// what the longer settle is for.
	Game game;
	game.governor_available = true;
	game.fixture.env.game_file.SetBoolValue(SPEED, "iopThread", true);

	AutoTunePolicy hasty;
	hasty.governor_settle_game_seconds = 0.0;
	hasty.governor_measure_windows = hasty.measure_windows;
	game.controller = AutoTuneController(hasty);

	double governor_on_at = -1.0;
	game.speed_of = [&](const FakeEnv& env) {
		if (!IsOn(env.Effective(SPEED, "DynamicEECycleRate")))
		{
			governor_on_at = -1.0;
			return 70.0;
		}
		if (governor_on_at < 0.0)
			governor_on_at = game.now;
		return (game.now - governor_on_at) < 12.0 ? 70.0 : 99.0;
	};

	AutoTuneWindow ee_bound;
	ee_bound.ee_pct = 98.0;
	ee_bound.mtgs_pct = 30.0;
	game.Play(ee_bound, 30.0);

	EXPECT_FALSE(game.fixture.env.game_file.ContainsValue(SPEED, "DynamicEECycleRate"));
	const std::vector<AutoTuneRejection> persisted = AutoTune::ReadRejected(game.fixture.env.game_file);
	ASSERT_EQ(persisted.size(), 1u);
	EXPECT_EQ(persisted[0].lever, AutoTuneLever::EeGovernor);
}

TEST(AutoTuneTogether, AGpuBoundGameWalksResolutionDownUntilItIsFastEnough)
{
	Game game;
	game.fixture.env.game_file.SetFloatValue(GS, "upscale_multiplier", 4.0f);
	game.speed_of = [](const FakeEnv& env) {
		const double res = Num(env.Effective(GS, "upscale_multiplier"), 1.0);
		return std::min(100.0, 160.0 / res); // 4x: 40%, 3x: 53%, 2x: 80%, 1x: 100%
	};

	AutoTuneWindow gpu_bound;
	gpu_bound.ee_pct = 50.0;
	gpu_bound.mtgs_pct = 40.0;
	gpu_bound.gpu_pct = 96.0;
	gpu_bound.has_gpu_timer = true;
	game.Play(gpu_bound, 200.0);

	EXPECT_EQ(game.fixture.env.game_file.GetFloatValue(GS, "upscale_multiplier", 0.0f), 1.0f);
	const std::vector<AutoTune::Change> changes = AutoTune::ReadChanges(game.fixture.env.game_file);
	ASSERT_EQ(changes.size(), 1u);
	EXPECT_TRUE(AutoTune::SameValue(*changes[0].original, "4"));
	EXPECT_GT(changes[0].gain, 1.0) << "40% to 100%";
	EXPECT_EQ(game.fixture.env.reloads, 3) << "one keep per notch: 4 to 3 to 2 to 1";
}

TEST(AutoTuneTogether, ALeverThatDoesNotHelpIsUndoneAndNeverTriedAgain)
{
	Game game;
	game.speed_of = [](const FakeEnv&) { return 70.0; }; // nothing changes the speed

	AutoTuneWindow ee_bound;
	ee_bound.ee_pct = 98.0;
	ee_bound.mtgs_pct = 30.0;
	game.Play(ee_bound, 300.0);

	EXPECT_FALSE(game.fixture.env.game_file.ContainsValue(SPEED, "iopThread"));
	EXPECT_FALSE(game.fixture.applier.TrialLayer()->ContainsValue(SPEED, "iopThread"));
	const std::vector<AutoTuneRejection> persisted = AutoTune::ReadRejected(game.fixture.env.game_file);
	ASSERT_EQ(persisted.size(), 1u);
	EXPECT_EQ(persisted[0].lever, AutoTuneLever::IopThread);

	const auto count = [&](const std::string& text) {
		return std::count_if(game.fixture.env.messages.begin(), game.fixture.env.messages.end(),
			[&](const std::string& m) { return m.find(text) != std::string::npos; });
	};
	EXPECT_EQ(count("trying IOP thread"), 1);
	EXPECT_EQ(count("did not help"), 1);
	EXPECT_EQ(count("nothing left to try"), 1);
}

// ---------------------------------------------------------------------------------------------- the on-screen hints

namespace
{
	// Everything the log said while it was alive, host output only.
	std::vector<std::string> g_log_lines;

	void CaptureLog(LOGLEVEL, ConsoleColors, std::string_view message)
	{
		g_log_lines.emplace_back(message);
	}

	class LogCapture
	{
	public:
		LogCapture()
		{
			g_log_lines.clear();
			Log::SetHostOutputLevel(LOGLEVEL_INFO, &CaptureLog);
		}
		~LogCapture() { Log::SetHostOutputLevel(LOGLEVEL_NONE, nullptr); }

		static bool Said(const std::string& osd_key)
		{
			const std::string needle = "OSD [" + osd_key + "]";
			return std::any_of(g_log_lines.begin(), g_log_lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; });
		}
	};

	// A game whose database entry recommends more than the configuration has: Full blending, AA1.
	void ApplyRecommendationsTo(Pcsx2Config::GSOptions& gs)
	{
		GameDatabaseSchema::GameEntry entry;
		entry.gsHWFixes.emplace_back(GameDatabaseSchema::GSHWFixId::RecommendedBlendingLevel, static_cast<int>(AccBlendLevel::Full));
		entry.gsHWFixes.emplace_back(GameDatabaseSchema::GSHWFixId::RecommendedHWAA1, 1);
		gs.AccurateBlendingUnit = AccBlendLevel::Basic;
		gs.HWAA1 = false;
		EmuConfig.GS.AccurateBlendingUnit = AccBlendLevel::Basic;
		entry.applyGSHardwareFixes(gs, {}, GameDatabaseSchema::ApplyMode::Live);
	}
}

TEST(AutoTuneHints, TheGameDatabaseSaysWhatItRecommendsWhenThePlayerAppliesSettings)
{
	LogCapture capture;
	Pcsx2Config::GSOptions gs;
	ApplyRecommendationsTo(gs);
	EXPECT_TRUE(LogCapture::Said("HWBlendingWarning"));
	EXPECT_TRUE(LogCapture::Said("HWAA1Warning"));
}

TEST(AutoTuneHints, TheyStayQuietWhileAutoTuneAppliesATrialOrAnUndo)
{
	// Every trial and every undo is an apply, and a convergence is six to nine of them: a stack of the same two
	// toasts on top of auto-tune's own message.
	LogCapture capture;
	{
		AutoTune::ApplyScope applying;
		Pcsx2Config::GSOptions gs;
		ApplyRecommendationsTo(gs);
	}
	EXPECT_FALSE(LogCapture::Said("HWBlendingWarning"));
	EXPECT_FALSE(LogCapture::Said("HWAA1Warning"));

	// And the next apply that is not auto-tune's says them again.
	Pcsx2Config::GSOptions gs;
	ApplyRecommendationsTo(gs);
	EXPECT_TRUE(LogCapture::Said("HWBlendingWarning"));
}

TEST(AutoTuneHints, TheScopeNestsAndEndsWithTheOutermost)
{
	EXPECT_FALSE(AutoTune::IsApplyingSettings());
	{
		AutoTune::ApplyScope outer;
		EXPECT_TRUE(AutoTune::IsApplyingSettings());
		{
			AutoTune::ApplyScope inner;
			EXPECT_TRUE(AutoTune::IsApplyingSettings());
		}
		EXPECT_TRUE(AutoTune::IsApplyingSettings()) << "a keep applies inside an apply";
	}
	EXPECT_FALSE(AutoTune::IsApplyingSettings());
}

// ---------------------------------------------------------------------------------------------- the game's file

namespace
{
	// Points the game settings folder at a scratch directory for the length of a test.
	class GameSettingsDir
	{
	public:
		GameSettingsDir()
			: m_saved(EmuFolders::GameSettings)
			, m_dir(std::filesystem::temp_directory_path() /
					("autotune_gamefile_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
		{
			std::filesystem::create_directories(m_dir);
			EmuFolders::GameSettings = m_dir.string();
		}

		~GameSettingsDir()
		{
			EmuFolders::GameSettings = m_saved;
			std::filesystem::remove_all(m_dir);
		}

	private:
		std::string m_saved;
		std::filesystem::path m_dir;
	};

	constexpr u32 kCrc = 0x2F123FD8;
	constexpr const char* kSerial = "SCUS-97481";
} // namespace

TEST(AutoTuneGameFile, DescribeAndResetWorkOnTheFileWithoutAVm)
{
	GameSettingsDir dir;
	const std::string path = VMManager::GetGameSettingsPath(kSerial, kCrc);
	{
		INISettingsInterface ini(path);
		ini.SetFloatValue(GS, "upscale_multiplier", 3.0f);
		ini.SetBoolValue("EmuCore/Gamefixes", "XgKickHack", true);
		AutoTune::Commit(ini, AutoTuneLever::Resolution, 2.0, 0.12);
		AutoTune::Commit(ini, AutoTuneLever::IopThread, 1.0, 0.18);
		ASSERT_TRUE(ini.Save());
	}

	const std::vector<std::string> lines = AutoTune::DescribeForGame(kSerial, kCrc);
	ASSERT_EQ(lines.size(), 2u);
	EXPECT_EQ(lines[0], "Resolution: 3x -> 2x (+12% speed)");

	const AutoTune::ResetResult result = AutoTune::ResetForGame(kSerial, kCrc);
	EXPECT_TRUE(result.had_record);
	EXPECT_EQ(result.restored.size(), 2u);

	INISettingsInterface after(path);
	ASSERT_TRUE(after.Load());
	EXPECT_EQ(after.GetFloatValue(GS, "upscale_multiplier", 0.0f), 3.0f);
	EXPECT_FALSE(after.ContainsValue(SPEED, "iopThread"));
	EXPECT_TRUE(after.GetBoolValue("EmuCore/Gamefixes", "XgKickHack", false));
	EXPECT_TRUE(AutoTune::DescribeForGame(kSerial, kCrc).empty());

	// A second reset finds nothing to do.
	EXPECT_FALSE(AutoTune::ResetForGame(kSerial, kCrc).had_record);
}

TEST(AutoTuneGameFile, AKeyThePlayerEditedSinceSurvivesTheReset)
{
	GameSettingsDir dir;
	const std::string path = VMManager::GetGameSettingsPath(kSerial, kCrc);
	{
		INISettingsInterface ini(path);
		ini.SetFloatValue(GS, "upscale_multiplier", 3.0f);
		AutoTune::Commit(ini, AutoTuneLever::Resolution, 2.0, 0.12);
		AutoTune::Commit(ini, AutoTuneLever::Blending, 1.0, 0.05);
		// The player opens the game's properties and sets the resolution to 6x.
		ini.SetFloatValue(GS, "upscale_multiplier", 6.0f);
		ASSERT_TRUE(ini.Save());
	}

	const AutoTune::ResetResult result = AutoTune::ResetForGame(kSerial, kCrc);
	EXPECT_EQ(result.restored.size(), 1u);
	ASSERT_EQ(result.left_alone.size(), 1u);
	EXPECT_EQ(result.left_alone[0].key, "upscale_multiplier");

	INISettingsInterface after(path);
	ASSERT_TRUE(after.Load());
	EXPECT_EQ(after.GetFloatValue(GS, "upscale_multiplier", 0.0f), 6.0f);
	EXPECT_FALSE(after.ContainsValue(GS, "accurate_blending_unit"));
}

TEST(AutoTuneGameFile, AFileThatLostTheKeptKeyReportsItAsMissing)
{
	// What the Android app leaves behind after it writes a game's file again: the record, without the resolution.
	GameSettingsDir dir;
	const std::string path = VMManager::GetGameSettingsPath(kSerial, kCrc);
	{
		INISettingsInterface ini(path);
		ini.SetFloatValue(GS, "upscale_multiplier", 8.0f);
		AutoTune::Commit(ini, AutoTuneLever::Resolution, 3.0, 0.28);
		ini.DeleteValue(GS, "upscale_multiplier");
		ASSERT_TRUE(ini.Save());
	}

	const AutoTune::ResetResult result = AutoTune::ResetForGame(kSerial, kCrc);
	EXPECT_TRUE(result.had_record);
	EXPECT_TRUE(result.restored.empty());
	ASSERT_EQ(result.left_alone.size(), 1u);
	EXPECT_EQ(result.left_alone[0].key, "upscale_multiplier");
	EXPECT_TRUE(result.left_alone[0].absent);
	EXPECT_TRUE(AutoTune::SameValue(result.left_alone[0].value, "3"));

	INISettingsInterface after(path);
	ASSERT_TRUE(after.Load());
	EXPECT_FALSE(after.ContainsValue(GS, "upscale_multiplier")) << "the file is not given a value it did not have";
}

TEST(AutoTuneGameFile, AGameWithNoFileHasNothingToDescribeOrReset)
{
	GameSettingsDir dir;
	EXPECT_TRUE(AutoTune::DescribeForGame(kSerial, kCrc).empty());
	EXPECT_FALSE(AutoTune::ResetForGame(kSerial, kCrc).had_record);
	EXPECT_FALSE(std::filesystem::exists(VMManager::GetGameSettingsPath(kSerial, kCrc))) << "a reset must not create a file";
}

TEST(AutoTuneGameFile, AGameWhoseSettingsLiveInTheOldCrcFileIsFoundThere)
{
	GameSettingsDir dir;
	const std::string legacy = VMManager::GetGameSettingsPath({}, kCrc);
	{
		INISettingsInterface ini(legacy);
		AutoTune::Commit(ini, AutoTuneLever::IopThread, 1.0, 0.18);
		ASSERT_TRUE(ini.Save());
	}
	EXPECT_EQ(AutoTune::DescribeForGame(kSerial, kCrc).size(), 1u);
	EXPECT_TRUE(AutoTune::ResetForGame(kSerial, kCrc).had_record);
}
