// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string>
#include <string_view>

// Seeing Big Picture without a monitor: open a page by name, wait for it to draw, save what a
// user would see as a PNG. Three PINE commands drive it (the opcodes are listed in PINE.cpp):
//
//   0x16 UIScreenshot  [u32 path_len][path][u32 settle_frames]
//                                                draw `settle_frames` frames, then write the next one
//                                                to `path` (.png); the same page at the same count
//                                                gives the same pixels (see SettleAndCapture)
//   0x17 UIOpenPage    [u32 name_len][name]      switch Big Picture to a page (names below)
//   0x18 UIWaitFrames  [u32 frames][u32 ms]      block until `frames` more frames were presented
//                                                or `ms` milliseconds passed (0 = 30000)
//
// Each replies with a length-prefixed JSON string: {"ok":true,...} or {"ok":false,"error":"..."}.
// All three work with no game running. UIOpenPage also works against a normal window; UIScreenshot
// and UIWaitFrames need `armsx2-sdl --headless WxH` (see GSHeadlessPresent.h).
//
// Page names are case-insensitive: landing, setup, startgame, exit, gamelist, gamelistsettings,
// about, settings, and settings:<page> with <page> one of the global SettingsPage names (interface,
// bios, emulation, graphics, osd, audio, memorycard, networkhdd, folders, achievements,
// controller, hotkey, arcade, advanced, drivers). memcardbackups is the memory card backups window and
// memcardprompt the question a boot asks about an unreadable card, with a sample card. Several more set up a
// state instead of naming a page: rescan starts a game list scan and shows the list, gamesettings opens
// the settings of the first game in the list, gamesettings:<page> opens them on a page (summary, emulation,
// patches, cheats, graphics, osd, audio, memorycard, gamefixes), gamesettings:patches:browse and
// gamesettings:cheats:browse also do what a press of that page's Browse Online row does, and loadstate opens
// that game's Load State selector (open rescan first and wait a few frames). The arcade screens have their own
// names: arcadelaunch starts the first arcade game in the list (as selecting it would: with nothing missing it
// boots, else it shows what is), arcadedownload opens settings:arcade and downloads the boot files, and
// arcadepause draws the pause menu's Arcade submenu with no game running.
//
// Two more play input into Big Picture, which needs --headless for the frames to spread presses over. textdialog
// opens the text entry dialog with its on-screen keyboard over whatever is showing (open settings:networkhdd first
// for a settings page behind it), as Local Link's Host Address row does, and plays what follows: textdialog:numeric
// and :ip give the number pads, :long the longest message a caller gives, :text=<start text>, :keys=<pad buttons> (up down left right south east west north
// l1 r1 l2 select start confirm cancel, each optionally *N, e.g. down*2,confirm), :type=<characters typed on a
// keyboard>, :pkeys=<keyboard keys> (backspace delete left right home end enter escape) and :click=<x,y+x,y> in
// that order. The text the dialog's OK key submits appears as a toast. padkeys:<pad buttons> plays pad buttons on
// the page that is showing, so a shot can start from a real row (settings:memorycard, then padkeys:down,up,confirm
// opens Create Memory Card).
//
// Adding a Big Picture page: add a row to kPages in FullscreenUIShot.cpp, so an agent can reach it.
// A new SettingsPage enum value trips a static_assert there until it has a name.
namespace FullscreenUIShot
{
	constexpr u8 OpScreenshot = 0x16;
	constexpr u8 OpOpenPage = 0x17;
	constexpr u8 OpWaitFrames = 0x18;

	/// PINE thread. Parses the arguments of one command at `args` (`avail` bytes remain in the
	/// request), runs it, and fills `reply` with the JSON. `consumed` is how many bytes of
	/// arguments it read. Returns false only if the arguments are malformed.
	bool HandlePineCommand(u8 opcode, const u8* args, u32 avail, u32* consumed, std::string* reply);
} // namespace FullscreenUIShot

namespace FullscreenUI
{
	/// GS thread. Switches Big Picture to the named page, starting it if needed. On failure
	/// returns false and says why in `error`.
	bool OpenPageByName(std::string_view name, std::string* error);

	/// Every name OpenPageByName accepts, comma separated.
	std::string PageNames();
} // namespace FullscreenUI
