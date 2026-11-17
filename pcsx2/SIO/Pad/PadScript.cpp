// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Pad/PadScript.h"

#include "IopThread/IopBridge.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadBase.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <cstdio>
#include <mutex>
#include <span>

static_assert(PadScript::MAX_PADS == Pad::NUM_CONTROLLER_PORTS);

namespace PadScript
{
	namespace
	{
		// In PadDualshock2::Inputs order.
		constexpr std::array<std::string_view, NUM_BUTTONS> kButtonNames = {
			"up", "right", "down", "left", "triangle", "circle", "cross", "square",
			"select", "start", "l1", "l2", "r1", "r2", "l3", "r3"};

		std::vector<std::string_view> Tokens(std::string_view line)
		{
			if (const size_t hash = line.find('#'); hash != std::string_view::npos)
				line = line.substr(0, hash);

			std::vector<std::string_view> out;
			size_t i = 0;
			while (i < line.size())
			{
				while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
					i++;
				const size_t start = i;
				while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
					i++;
				if (i > start)
					out.push_back(line.substr(start, i - start));
			}
			return out;
		}

		bool ParseByte(std::string_view text, u8* out)
		{
			const std::optional<u32> value = StringUtil::FromChars<u32>(text);
			if (!value.has_value() || *value > 255)
				return false;
			*out = static_cast<u8>(*value);
			return true;
		}

		bool ParseButtons(std::string_view text, PadFrame* out, std::string* error)
		{
			if (text == "-")
				return true;

			for (const std::string_view item : StringUtil::SplitString(text, '+', false))
			{
				std::string_view name = item;
				u8 pressure = 255;
				if (const size_t colon = item.find(':'); colon != std::string_view::npos)
				{
					name = item.substr(0, colon);
					if (!ParseByte(item.substr(colon + 1), &pressure))
					{
						*error = fmt::format("bad pressure in '{}' (0..255)", item);
						return false;
					}
				}

				u32 index = 0;
				while (index < NUM_BUTTONS && kButtonNames[index] != name)
					index++;
				if (index == NUM_BUTTONS)
				{
					*error = fmt::format("unknown button '{}'", name);
					return false;
				}
				out->pressed |= static_cast<u16>(1u << index);
				out->pressure[index] = pressure;
			}
			return true;
		}

		bool ParseStateTokens(std::span<const std::string_view> tokens, PadFrame* out, std::string* error)
		{
			if (tokens.size() != 1 && tokens.size() != 5)
			{
				*error = "expected <buttons> and either no sticks or all four";
				return false;
			}

			PadFrame state;
			if (!ParseButtons(tokens[0], &state, error))
				return false;
			if (tokens.size() == 5)
			{
				u8* const axes[] = {&state.lx, &state.ly, &state.rx, &state.ry};
				for (size_t i = 0; i < 4; i++)
				{
					if (!ParseByte(tokens[i + 1], axes[i]))
					{
						*error = fmt::format("bad stick value '{}' (0..255)", tokens[i + 1]);
						return false;
					}
				}
			}
			*out = state;
			return true;
		}

		bool ParsePad(std::string_view text, u8* pad, std::string* error)
		{
			const std::optional<u32> value = StringUtil::FromChars<u32>(text);
			if (!value.has_value() || *value < 1 || *value > MAX_PADS)
			{
				*error = fmt::format("bad pad '{}' (1..{})", text, MAX_PADS);
				return false;
			}
			*pad = static_cast<u8>(*value - 1);
			return true;
		}

		std::string JsonEscape(std::string_view str)
		{
			std::string out;
			out.reserve(str.size());
			for (const char c : str)
			{
				if (c == '"' || c == '\\')
					out.push_back('\\');
				out.push_back(c);
			}
			return out;
		}
	} // namespace

	bool ParseState(std::string_view text, PadFrame* out, std::string* error)
	{
		const std::vector<std::string_view> tokens = Tokens(text);
		return ParseStateTokens(tokens, out, error);
	}

	bool ParsePadCommand(std::string_view text, u8* pad, std::optional<PadFrame>* state, std::string* error)
	{
		const std::vector<std::string_view> tokens = Tokens(text);
		if (tokens.empty())
		{
			*error = "empty command";
			return false;
		}
		if (!ParsePad(tokens[0], pad, error))
			return false;
		if (tokens.size() == 2 && tokens[1] == "release")
		{
			state->reset();
			return true;
		}
		PadFrame parsed;
		if (!ParseStateTokens(std::span(tokens).subspan(1), &parsed, error))
			return false;
		*state = parsed;
		return true;
	}

	bool ParseScript(std::string_view text, std::vector<Event>* out, std::string* error)
	{
		std::vector<Event> events;
		u32 line_no = 0;
		u32 last_frame = 0;
		bool ended = false;
		for (const std::string_view line : StringUtil::SplitString(text, '\n', false))
		{
			line_no++;
			const std::vector<std::string_view> tokens = Tokens(line);
			if (tokens.empty())
				continue;

			std::string what;
			const auto fail = [&](std::string_view message) {
				*error = fmt::format("line {}: {}", line_no, message);
				return false;
			};

			if (ended)
				return fail("nothing may follow 'end'");

			Event event;
			const std::optional<u32> frame = StringUtil::FromChars<u32>(tokens[0]);
			if (!frame.has_value())
				return fail(fmt::format("bad frame '{}'", tokens[0]));
			if (*frame < last_frame)
				return fail(fmt::format("frame {} comes before frame {} above it", *frame, last_frame));
			event.frame = last_frame = *frame;

			if (tokens.size() == 2 && tokens[1] == "end")
			{
				event.kind = EventKind::End;
				ended = true;
			}
			else
			{
				if (tokens.size() < 3)
					return fail("expected <frame> <pad> <buttons> [<lx> <ly> <rx> <ry>], <frame> <pad> release or <frame> end");
				if (!ParsePad(tokens[1], &event.pad, &what))
					return fail(what);
				if (tokens.size() == 3 && tokens[2] == "release")
					event.kind = EventKind::Release;
				else if (!ParseStateTokens(std::span(tokens).subspan(2), &event.state, &what))
					return fail(what);
			}
			events.push_back(event);
		}
		*out = std::move(events);
		return true;
	}

	std::string FormatState(const PadFrame& state)
	{
		std::string buttons;
		for (u32 i = 0; i < NUM_BUTTONS; i++)
		{
			if (!(state.pressed & (1u << i)))
				continue;
			if (!buttons.empty())
				buttons += '+';
			buttons += kButtonNames[i];
			if (state.pressure[i] != 255)
				buttons += fmt::format(":{}", state.pressure[i]);
		}
		if (buttons.empty())
			buttons = "-";
		return fmt::format("{} {} {} {} {}", buttons, state.lx, state.ly, state.rx, state.ry);
	}

	std::string FormatEvent(const Event& event)
	{
		switch (event.kind)
		{
			case EventKind::End:
				return fmt::format("{} end", event.frame);
			case EventKind::Release:
				return fmt::format("{} {} release", event.frame, event.pad + 1);
			case EventKind::Set:
			default:
				return fmt::format("{} {} {}", event.frame, event.pad + 1, FormatState(event.state));
		}
	}

	Sequencer::Sequencer(std::vector<Event> events)
		: m_events(std::move(events))
	{
	}

	void Sequencer::Advance(u32 frame)
	{
		while (!m_finished && m_next < m_events.size() && m_events[m_next].frame <= frame)
		{
			const Event& event = m_events[m_next++];
			switch (event.kind)
			{
				case EventKind::Set:
					m_held[event.pad] = event.state;
					m_touched |= 1u << event.pad;
					break;
				case EventKind::Release:
					m_held[event.pad].reset();
					break;
				case EventKind::End:
					m_held = {};
					m_finished = true;
					break;
			}
		}
	}

	// ---- The runtime ---------------------------------------------------------------------------------

	namespace
	{
		enum class Phase : u8
		{
			Idle,
			Armed, // waits for a savestate load
			Pending, // starts at the next vsync
			Running,
		};

		const char* PhaseName(Phase phase)
		{
			switch (phase)
			{
				case Phase::Armed:
					return "armed";
				case Phase::Pending:
					return "pending";
				case Phase::Running:
					return "running";
				case Phase::Idle:
				default:
					return "idle";
			}
		}

		using HeldSet = std::array<std::optional<PadFrame>, MAX_PADS>;

		// What the CPU thread hands the IOP: the pads whose held state changes, and the new states. The packet
		// is written before the item that names it is queued, and the queue's release and acquire make it
		// visible to the IOP. The IOP is at most two frames behind the EE (the largest lag bound), and a packet
		// is written at most once a vsync, so a slot is read long before it is written again.
		struct LatchPacket
		{
			u32 seq = 0;
			u8 changed = 0;
			u8 held = 0;
			std::array<PadFrame, MAX_PADS> frames = {};
		};
		constexpr u32 kLatchRing = 16;
		constexpr u32 kSeqMask = 0xffffff; // the item's arg keeps the low byte for the job number
		std::array<LatchPacket, kLatchRing> s_latch;

		struct State
		{
			std::mutex lock;

			HeldSet client = {};

			Phase script_phase = Phase::Idle;
			Sequencer script;
			u32 script_frame = 0;

			Phase rec_phase = Phase::Idle;
			std::string rec_path;
			std::FILE* rec_file = nullptr;
			u32 rec_mask_requested = 0;
			u32 rec_mask = 0;
			u32 rec_frame = 0;
			u64 rec_lines = 0;
			HeldSet rec_last = {};
			std::string rec_error;

			u32 frames_since_load = 0;

			// The CPU thread's record of what the IOP side has been told.
			HeldSet posted = {};
			std::array<const PadBase*, MAX_PADS> posted_pad = {};
			bool force = false;
			u32 seq = 0;
		};
		State s;

		std::optional<PadFrame> SamplePlayer(u32 pad)
		{
			const PadBase* const base = Pad::GetPad(static_cast<u8>(pad));
			PadFrame frame;
			if (base && base->CapturePlayerInput(&frame))
				return frame;
			return std::nullopt;
		}

		HeldSet Desired()
		{
			HeldSet desired;
			const bool script = (s.script_phase == Phase::Running);
			const bool recording = (s.rec_phase == Phase::Running);
			for (u32 pad = 0; pad < MAX_PADS; pad++)
			{
				if (s.client[pad].has_value())
					desired[pad] = s.client[pad];
				else if (script && s.script.Held()[pad].has_value())
					desired[pad] = s.script.Held()[pad];
				else if (recording && (s.rec_mask & (1u << pad)))
					desired[pad] = SamplePlayer(pad);
			}
			return desired;
		}

		void WriteRecordLine(const std::string& line)
		{
			if (!s.rec_file)
				return;
			if (std::fprintf(s.rec_file, "%s\n", line.c_str()) < 0 && s.rec_error.empty())
				s.rec_error = fmt::format("write to '{}' failed", s.rec_path);
			s.rec_lines++;
		}

		void RecordFrame(const HeldSet& desired)
		{
			for (u32 pad = 0; pad < MAX_PADS; pad++)
			{
				if (!(s.rec_mask & (1u << pad)) || desired[pad] == s.rec_last[pad])
					continue;
				Event event;
				event.frame = s.rec_frame;
				event.pad = static_cast<u8>(pad);
				if (desired[pad].has_value())
					event.state = *desired[pad];
				else
					event.kind = EventKind::Release;
				WriteRecordLine(FormatEvent(event));
				s.rec_last[pad] = desired[pad];
			}
		}

		void BeginRecording(bool at_load)
		{
			s.rec_phase = Phase::Running;
			s.rec_frame = 0;
			s.rec_lines = 0;
			s.rec_last = {};
			s.rec_mask = s.rec_mask_requested;
			if (s.rec_mask == 0)
			{
				for (u32 pad = 0; pad < MAX_PADS; pad++)
				{
					const PadBase* const base = Pad::GetPad(static_cast<u8>(pad));
					if (base && base->GetType() == Pad::ControllerType::DualShock2)
						s.rec_mask |= 1u << pad;
				}
			}

			WriteRecordLine("# ARMSX2 pad script, recorded");
			WriteRecordLine(fmt::format("# game {} crc {:08x}, frame 0 is {}", VMManager::GetDiscSerial(),
				VMManager::GetDiscCRC(), at_load ? "the savestate load" : "the first vsync after the request"));
			s.rec_lines = 0;
		}

		std::string FinishRecording()
		{
			std::string reply;
			if (s.rec_phase == Phase::Running)
				WriteRecordLine(fmt::format("{} end", s.rec_frame + 1));
			if (s.rec_file && std::fclose(s.rec_file) != 0 && s.rec_error.empty())
				s.rec_error = fmt::format("closing '{}' failed", s.rec_path);
			s.rec_file = nullptr;

			const u32 frames = (s.rec_phase == Phase::Running) ? s.rec_frame + 1 : 0;
			if (s.rec_error.empty())
			{
				reply = fmt::format("{{\"ok\":true,\"path\":\"{}\",\"frames\":{},\"lines\":{}}}", JsonEscape(s.rec_path),
					frames, s.rec_lines);
			}
			else
			{
				reply = fmt::format("{{\"ok\":false,\"path\":\"{}\",\"frames\":{},\"lines\":{},\"error\":\"{}\"}}",
					JsonEscape(s.rec_path), frames, s.rec_lines, JsonEscape(s.rec_error));
			}
			s.rec_phase = Phase::Idle;
			return reply;
		}

		/// Works out what each pad is held in, records it, and tells the IOP side about any change. With
		/// `direct` the change is applied here (the caller holds the IOP still); otherwise it is queued at the
		/// EE's cycle.
		void Latch(bool direct)
		{
			const HeldSet desired = Desired();
			if (s.rec_phase == Phase::Running)
				RecordFrame(desired);

			u8 changed = 0;
			for (u32 pad = 0; pad < MAX_PADS; pad++)
			{
				const PadBase* const base = Pad::GetPad(static_cast<u8>(pad));
				if (s.force || desired[pad] != s.posted[pad] || (desired[pad].has_value() && base != s.posted_pad[pad]))
					changed |= static_cast<u8>(1u << pad);
				s.posted_pad[pad] = base;
			}
			s.force = false;
			if (!changed)
				return;

			s.seq = (s.seq + 1) & kSeqMask;
			LatchPacket& packet = s_latch[s.seq % kLatchRing];
			packet.seq = s.seq;
			packet.changed = changed;
			packet.held = 0;
			for (u32 pad = 0; pad < MAX_PADS; pad++)
			{
				if (desired[pad].has_value())
				{
					packet.held |= static_cast<u8>(1u << pad);
					packet.frames[pad] = *desired[pad];
				}
			}
			s.posted = desired;

			if (direct)
				ApplyLatch(s.seq);
			else
				IopBridge::PadInputLatch(s.seq);
		}
	} // namespace

	void SetPad(u8 pad, const std::optional<PadFrame>& state)
	{
		if (pad >= MAX_PADS)
			return;
		std::lock_guard lock(s.lock);
		s.client[pad] = state;
	}

	void StartScript(std::vector<Event> events, bool at_load)
	{
		std::lock_guard lock(s.lock);
		s.script = Sequencer(std::move(events));
		s.script_frame = 0;
		s.script_phase = at_load ? Phase::Armed : Phase::Pending;
	}

	void StopScript()
	{
		std::lock_guard lock(s.lock);
		s.script_phase = Phase::Idle;
		s.script = Sequencer();
	}

	bool StartRecording(std::string path, u32 pad_mask, bool at_load, std::string* error)
	{
		std::lock_guard lock(s.lock);
		if (s.rec_phase != Phase::Idle)
			FinishRecording();

		Error open_error;
		std::FILE* const file = FileSystem::OpenCFile(path.c_str(), "wb", &open_error);
		if (!file)
		{
			*error = fmt::format("cannot open '{}': {}", path, open_error.GetDescription());
			return false;
		}
		s.rec_file = file;
		s.rec_path = std::move(path);
		s.rec_mask_requested = pad_mask;
		s.rec_error.clear();
		s.rec_phase = at_load ? Phase::Armed : Phase::Pending;
		return true;
	}

	std::string StopRecording()
	{
		std::lock_guard lock(s.lock);
		if (s.rec_phase == Phase::Idle && !s.rec_file)
			return "{\"ok\":false,\"error\":\"not recording\"}";
		return FinishRecording();
	}

	std::string StatusJson()
	{
		std::lock_guard lock(s.lock);
		std::string held;
		for (u32 pad = 0; pad < MAX_PADS; pad++)
		{
			if (!s.posted[pad].has_value())
				continue;
			held += fmt::format("{}{{\"pad\":{},\"state\":\"{}\"}}", held.empty() ? "" : ",", pad + 1,
				FormatState(*s.posted[pad]));
		}
		return fmt::format("{{\"frame\":{},"
						   "\"script\":{{\"state\":\"{}\",\"frame\":{},\"next\":{},\"events\":{},\"last_frame\":{}}},"
						   "\"record\":{{\"state\":\"{}\",\"path\":\"{}\",\"frame\":{},\"lines\":{},\"error\":\"{}\"}},"
						   "\"held\":[{}]}}",
			s.frames_since_load, PhaseName(s.script_phase), s.script_frame, s.script.Position(), s.script.Size(),
			s.script.LastFrame(), PhaseName(s.rec_phase), JsonEscape(s.rec_path), s.rec_frame, s.rec_lines,
			JsonEscape(s.rec_error), held);
	}

	void OnVsync()
	{
		std::lock_guard lock(s.lock);
		s.frames_since_load++;

		if (s.script_phase == Phase::Pending)
		{
			s.script_phase = Phase::Running;
			s.script_frame = 0;
			s.script.Advance(0);
		}
		else if (s.script_phase == Phase::Running)
		{
			s.script.Advance(++s.script_frame);
		}
		if (s.script_phase == Phase::Running && s.script.Finished())
			s.script_phase = Phase::Idle;

		if (s.rec_phase == Phase::Pending)
			BeginRecording(false);
		else if (s.rec_phase == Phase::Running)
			s.rec_frame++;

		Latch(false);
	}

	void OnStateLoaded()
	{
		std::lock_guard lock(s.lock);
		s.frames_since_load = 0;
		s.force = true;

		if (s.script_phase == Phase::Armed)
		{
			s.script_phase = Phase::Running;
			s.script_frame = 0;
			s.script.Advance(0);
			if (s.script.Finished())
				s.script_phase = Phase::Idle;
		}
		else if (s.script_phase != Phase::Idle)
		{
			Console.WriteLn("PadScript: a savestate load ends the running script.");
			s.script_phase = Phase::Idle;
		}

		if (s.rec_phase == Phase::Armed)
			BeginRecording(true);
		else if (s.rec_phase != Phase::Idle)
		{
			Console.WriteLn("PadScript: a savestate load ends the recording.");
			FinishRecording();
		}

		// The game has not run since the load, so the IOP is at the load point: frame 0 applies from here.
		IopBridge::EmuHold hold;
		Latch(true);
	}

	void OnShutdown()
	{
		std::lock_guard lock(s.lock);
		if (s.rec_phase != Phase::Idle || s.rec_file)
			FinishRecording();
		s.client = {};
		s.script_phase = Phase::Idle;
		s.script = Sequencer();
		s.posted = {};
		s.posted_pad = {};
		s.frames_since_load = 0;
	}

	void ApplyLatch(u32 seq)
	{
		const LatchPacket& packet = s_latch[seq % kLatchRing];
		if (packet.seq != seq) [[unlikely]]
			return; // overtaken: a savestate load emptied the queue that carried it

		for (u32 pad = 0; pad < MAX_PADS; pad++)
		{
			if (!(packet.changed & (1u << pad)))
				continue;
			if (PadBase* const base = Pad::GetPad(static_cast<u8>(pad)))
				base->SetHeldInput((packet.held & (1u << pad)) ? &packet.frames[pad] : nullptr);
		}
	}
} // namespace PadScript
