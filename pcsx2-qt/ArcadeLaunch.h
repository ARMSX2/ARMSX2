// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Starting a Namco System 246/256 arcade game from the Qt UI.
//
// An arcade game is an .acgame file that names files around it (a boot program, an image, a dongle). The core
// boots one on its own when everything is where the .acgame says, but a player's files are rarely that tidy: the
// boot program comes from the downloaded Proverb boot files, the dongle sits beside the image and has to be copied
// into the memory cards folder, and Soul Calibur II needs its Conquest card. ArcadeSetup::Service::Prepare() does
// that. PrepareForBoot() runs it before the VM is started, from the one place every start in the Qt window goes
// through (EmuThread::startVM): a game list double click, Start File, a dropped file, and a path given on the
// command line (which is how Armada launches a game). Big Picture starts games on its own path, with its own call.

#include "pcsx2/Arcade/ArcadeSetup.h"

#include <QtCore/QCoreApplication>

class QMessageBox;
class QWidget;
struct VMBootParameters;

class ArcadeLaunch
{
	Q_DECLARE_TR_FUNCTIONS(ArcadeLaunch)

public:
	/// For an arcade game in [params] (an .acgame, or an arcade image the game list knows, as the file name, and no boot
	/// program given by the host yet): gets its files ready and fills the boot program, image and SRAM into [params]
	/// (PrepareResult::ApplyTo). Anything else is left alone and passes. When the game is not ready, nothing has been
	/// changed on disk; the player is shown what is missing (with a button to open Arcade Setup) and false comes back, so
	/// the game must not be started. Call on the UI thread. A busy dialog shows if getting ready takes a moment (copying
	/// a dongle off a slow card).
	static bool PrepareForBoot(QWidget* parent, VMBootParameters* params);

	/// The report of what [result] lacks, not shown yet: the game's name, a line per missing part saying what it is
	/// and where it goes, and an "Open Arcade Setup" button (objectName openArcadeSetupButton).
	static QMessageBox* CreateMissingReport(QWidget* parent, const ArcadeSetup::PrepareResult& result);

	/// Shows the report and, if the player asks for it, opens Arcade Setup.
	static void ShowMissingReport(QWidget* parent, const ArcadeSetup::PrepareResult& result);

	/// Runs the Arcade Setup dialog until it is closed.
	static void ShowSetup(QWidget* parent);
};
