// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// AutoTuneApplier against a fake VM, the host decisions it relies on, and the controller and applier run
// together against a game whose speed depends on its settings.

#include <gtest/gtest.h>

#include "AutoTune/AutoTuneApplier.h"
#include "Config.h"
#include "GameDatabase.h"
#include "INISettingsInterface.h"
#include "LayeredSettingsInterface.h"
#include "VMManager.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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

// ---------------------------------------------------------------------------------------------- together

namespace
{
	// A game whose speed depends on its settings, run through the controller and the applier. `speed_of` maps
	// the settings in force to a speed. The windows are what the sampler would hand over.
	struct Game
	{
		Fixture fixture;
		AutoTuneController controller;
		double (*speed_of)(const FakeEnv&) = nullptr;
		std::vector<std::string> trace;

		AutoTuneLeverState State() const
		{
			AutoTuneLeverState s;
			s.hw_renderer = true;
			s.resolution = Num(fixture.env.Effective(GS, "upscale_multiplier"), 1.0);
			s.download_mode = AutoTuneDownloadMode::NoReadbacks;
			s.iop_thread_on = IsOn(fixture.env.Effective(SPEED, "iopThread"));
			s.iop_thread_placeable = true;
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
