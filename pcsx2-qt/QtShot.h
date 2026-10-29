// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Screenshot harness for the Qt UI: renders a settings page, a dialog or the main window to a PNG with
// QWidget::grab() and exits, so a UI change can be looked at with no display. Run armsx2-qt with
// QT_QPA_PLATFORM=offscreen and a throwaway -datapath; -uishot list prints every page and dialog name.
// Nothing here runs unless a -uishot flag is on the command line, and nothing reads the environment.
//
// Command line (all hidden from -help):
//   -uishot <spec> <png>     one shot; repeatable, shots run in order in one launch
//   -uishot-game <path>      game for game:* shots; must be in the game list (a scanned directory)
//   -uishot-size <WxH>       resize every shot's window before grabbing
//   -uishot-wait <ms>        settle time after opening, before actions and the grab (default 300)
//   -uishot-timeout <sec>    exit 2 after this long (default 120)
//   -uishot-any-platform     allow a platform other than offscreen
// Without -datapath (or with -portable), or on another platform without -uishot-any-platform, the
// harness exits 2 at once, so a stray run cannot open windows on a real desktop or use real settings.
//
// A spec is  target ('|' action)*  with
//   target   main                   the main window (game list)
//            settings:<page>        global settings window on <page>
//            game:<page>            per-game settings window on <page> (needs -uishot-game)
//            dialog:<name>          a dialog from the registry below
//            list                   print every name this build knows and exit
//   <page>   the category title or the page widget's class name, ignoring case and punctuation:
//            "Memory Cards", memory-cards and memorycard all select MemoryCardSettingsWidget.
//            Every page added to SettingsWindow is reachable with no entry anywhere.
//   action   focus=<objectName>     reveal a widget (selects its tab and settings page, scrolls to it), focus it
//            tab=<text>             select the tab with this text
//            click=<objectName>     click a button or trigger a QAction; a modal dialog it opens is what gets grabbed
//            wait=<ms>              pause, for content that arrives later
//            size=<WxH>             resize the window
// objectName is the widget's name in its .ui file, or setObjectName() for widgets built in code.
//
// Making a new dialog reachable: put one line in the dialog's own .cpp (or in QtShotEntries.cpp)
//   QTSHOT_DIALOG("texture-packs", "Browse texture packs", [](const QtShot::Context& ctx) { return new TexturePackDialog(nullptr); });
// and take it with  -uishot dialog:texture-packs out.png. A dialog that a button opens needs no entry:
// give the button an objectName and use  settings:graphics|click=browseTexturePacksButton.

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

class QWidget;

namespace QtShot
{
	struct Context
	{
		/// -uishot-game, empty when not given.
		QString game_path;
	};

	/// Builds the widget to grab. The harness shows it, grabs it and deletes it. Return nullptr after
	/// printing a reason to stderr to fail the shot.
	using DialogFactory = std::function<QWidget*(const Context&)>;

	/// Adds a name that `dialog:<name>` resolves. Safe to call from static initialisers.
	void RegisterDialog(const char* name, const char* description, DialogFactory factory);

	struct DialogRegistrar
	{
		DialogRegistrar(const char* name, const char* description, DialogFactory factory)
		{
			RegisterDialog(name, description, std::move(factory));
		}
	};

	/// Consumes a -uishot* argument at `it` (and its parameters), returning true if it was one.
	bool ParseArgument(QStringList::const_iterator& it, QStringList::const_iterator end);

	/// Called once from main() after the main window exists and before the event loop starts. Does nothing
	/// unless -uishot was given; otherwise schedules the shots and makes the event loop exit with their status.
	void Start();
} // namespace QtShot

#define QTSHOT_CONCAT2(a, b) a##b
#define QTSHOT_CONCAT(a, b) QTSHOT_CONCAT2(a, b)
#define QTSHOT_DIALOG(name, description, factory) \
	static const QtShot::DialogRegistrar QTSHOT_CONCAT(s_qtshot_dialog_, __LINE__)(name, description, factory)
