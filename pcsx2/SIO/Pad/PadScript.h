// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pad input from a script or a remote client instead of the player, and recording of the player's input.
//
// A pad can be held: while it is, the game reads a held state (buttons, pressures, sticks) and not the
// player's input. The player's input keeps arriving in the pad's own fields untouched, so it is back the
// moment the pad is released. Three things hold pads:
//
//   a client   SetPad(): one state, held until the client changes or releases it (PINE MsgPadSet).
//   a script   StartScript(): lines of (frame, pad, state), see ParseScript (PINE MsgPadScript,
//              pcsx2-eerunner --pad-script).
//   a recorder StartRecording(): the player's own input, sampled once a frame and held for that frame,
//              so what the game read is exactly what was written to the file (PINE MsgPadRecord,
//              pcsx2-eerunner --pad-record).
//
// Frames are counted in vsyncs, so a script lines up the same on every device and at every speed. Frame 0
// is the point the script or recording starts: the savestate load when it is armed for one (frame N is
// then the Nth vsync after the load), or else the first vsync after the request.
//
// A held state changes only at a vsync, and it reaches the pad the way the EE's other once-a-frame IOP jobs
// do: as an item stamped with the EE's cycle that the IOP applies at that point of its own clock. So the
// IOP reads the change at the same emulated moment whether it runs on the EE's thread, in Lockstep or
// Threaded.
//
// Only DualShock 2 pads can be held.

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PadScript
{
	inline constexpr u32 MAX_PADS = 8;
	inline constexpr u32 NUM_BUTTONS = 16;
	inline constexpr u8 AXIS_CENTRE = 0x7f;

	/// What a DualShock 2 reports. Bit i of `pressed` is input i of PadDualshock2::Inputs (PAD_UP .. PAD_R3), and
	/// pressure[i] its pressure, which is 0 for a button that is not pressed.
	struct PadFrame
	{
		u16 pressed = 0;
		std::array<u8, NUM_BUTTONS> pressure = {};
		u8 lx = AXIS_CENTRE;
		u8 ly = AXIS_CENTRE;
		u8 rx = AXIS_CENTRE;
		u8 ry = AXIS_CENTRE;

		bool operator==(const PadFrame&) const = default;
	};

	enum class EventKind : u8
	{
		Set, // the pad is held in `state`
		Release, // the pad goes back to the player
		End, // the script ends and lets go of every pad it held
	};

	struct Event
	{
		u32 frame = 0;
		EventKind kind = EventKind::Set;
		u8 pad = 0; // 0-based; written 1-based in the text
		PadFrame state;

		bool operator==(const Event&) const = default;
	};

	// ---- The text format ------------------------------------------------------------------------------
	//
	// One event a line; '#' starts a comment. Frames never go down.
	//
	//   <frame> <pad> <buttons> [<lx> <ly> <rx> <ry>]    hold pad <pad> (1..8) in this state from <frame>
	//   <frame> <pad> release                            give the pad back to the player
	//   <frame> end                                      end the script; every pad it held is released
	//
	// <buttons> is '-' for none, or names joined by '+': up right down left triangle circle cross square
	// select start l1 l2 r1 r2 l3 r3. A name may carry a pressure, 'cross:96'; without one it is 255.
	// The sticks are 0..255, 127 at rest, and all four default to 127. Up is 0 on ly and ry.
	//
	//   0    1 -
	//   60   1 cross
	//   64   1 - 127 0 127 127
	//   600  end

	/// "<buttons> [<lx> <ly> <rx> <ry>]".
	bool ParseState(std::string_view text, PadFrame* out, std::string* error);

	/// What MsgPadSet carries: "<pad> <buttons> [<lx> <ly> <rx> <ry>]" or "<pad> release". `state` is left
	/// empty for a release.
	bool ParsePadCommand(std::string_view text, u8* pad, std::optional<PadFrame>* state, std::string* error);

	/// A whole script. The error names the line.
	bool ParseScript(std::string_view text, std::vector<Event>* out, std::string* error);

	std::string FormatState(const PadFrame& state);
	std::string FormatEvent(const Event& event);

	/// Steps through a script one frame at a time and says which pads it holds in what state.
	class Sequencer
	{
	public:
		Sequencer() = default;
		explicit Sequencer(std::vector<Event> events);

		/// Applies every event at or before `frame` not applied yet. Nothing after an End.
		void Advance(u32 frame);

		bool Finished() const { return m_finished; }
		/// The state the script holds each pad in; empty where it holds none.
		const std::array<std::optional<PadFrame>, MAX_PADS>& Held() const { return m_held; }
		/// The pads the script has held at some point.
		u32 TouchedMask() const { return m_touched; }
		size_t Position() const { return m_next; }
		size_t Size() const { return m_events.size(); }
		/// The frame of the last event, 0 for an empty script.
		u32 LastFrame() const { return m_events.empty() ? 0 : m_events.back().frame; }

	private:
		std::vector<Event> m_events;
		size_t m_next = 0;
		bool m_finished = false;
		u32 m_touched = 0;
		std::array<std::optional<PadFrame>, MAX_PADS> m_held = {};
	};

	// ---- Requests. Any thread. -----------------------------------------------------------------------
	// A held state changes at the next vsync, or at the next savestate load for a start armed for one.

	/// Holds `pad` in `state`, or releases it when `state` is empty. A client's hold wins over a script's.
	void SetPad(u8 pad, const std::optional<PadFrame>& state);

	/// Replaces any running script. With `at_load` the script waits for the next savestate load.
	void StartScript(std::vector<Event> events, bool at_load);
	void StopScript();

	/// Records the pads in `pad_mask` (0: every DualShock 2 connected at the start) to `path`, which is opened
	/// here. Ends any recording in progress. With `at_load` it waits for the next savestate load.
	bool StartRecording(std::string path, u32 pad_mask, bool at_load, std::string* error);

	/// Ends the recording now and closes the file. Returns what was written as JSON, with "ok" false and an
	/// "error" if the file could not be written.
	std::string StopRecording();

	/// {"frame":..,"script":{..},"record":{..},"held":[..]}
	std::string StatusJson();

	// ---- The CPU thread ------------------------------------------------------------------------------

	/// At every vsync, after the host's input has been polled.
	void OnVsync();
	/// After a savestate load. A script or recording armed for it starts; one already running ends.
	void OnStateLoaded();
	/// The VM is going away: every hold, script and recording ends.
	void OnShutdown();

	// ---- The IOP side --------------------------------------------------------------------------------

	/// Applies the held states the CPU thread latched under `seq`. Runs where the IOP runs.
	void ApplyLatch(u32 seq);
} // namespace PadScript
