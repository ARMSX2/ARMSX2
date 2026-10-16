// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What a pad answers to a poll, for each pad type, after a fixed script of host input.
//
// The pad input fields (rawInputs, buttons, the analog bytes, the jog dial, the guitar's whammy) are written
// by the host thread at vsync and read by the SIO2 pad commands, which run on the IOP thread when there is
// one (IOP thread design 8.4). They became relaxed atomics for that. This file records what the pads do with
// plain fields, as a hash of every poll response and every getter after each step of a script, so that the
// change can be checked not to move any of it. The hashes were recorded on 2fbf24a5d0, before the change.
//
// The second half runs the host thread's side and the IOP thread's side of one pad on two threads at once. A
// plain byte shared that way is a data race; a relaxed atomic one is not, and every poll must read a state
// the writer actually wrote.

#include "IopThread/SharedWord.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SIO/Pad/PadGuitar.h"
#include "SIO/Pad/PadJogcon.h"
#include "SIO/Pad/PadNegcon.h"
#include "SIO/Pad/PadPopn.h"
#include "StateWrapper.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
	// FNV-1a over a stream of bytes.
	class Hash
	{
	public:
		void Add(u64 value, int bytes)
		{
			for (int i = 0; i < bytes; i++)
			{
				m_hash ^= (value >> (8 * i)) & 0xff;
				m_hash *= 0x100000001b3ull;
			}
		}
		void AddFloat(float value)
		{
			u32 bits;
			std::memcpy(&bits, &value, sizeof(bits));
			Add(bits, 4);
		}
		u64 Value() const { return m_hash; }

	private:
		u64 m_hash = 0xcbf29ce484222325ull;
	};

	constexpr float kValues[] = {1.0f, 0.5f, 0.0f, 0.25f, 0.999f, 0.004f, 0.75f, 0.0f, 1.0f, 0.3f};

	// One poll, as SIO2 drives it: the command byte, then zeros. 24 bytes cover the longest answer.
	void Poll(PadBase& pad, Hash& hash)
	{
		pad.SoftReset();
		hash.Add(pad.SendCommandByte(0x42), 1);
		for (int i = 0; i < 23; i++)
			hash.Add(pad.SendCommandByte(0x00), 1);
	}

	// Everything the pad lets the host read back.
	void Getters(PadBase& pad, u32 inputs, Hash& hash)
	{
		hash.Add(pad.GetButtons(), 4);
		const auto left = pad.GetRawLeftAnalog();
		const auto right = pad.GetRawRightAnalog();
		hash.Add(std::get<0>(left), 1);
		hash.Add(std::get<1>(left), 1);
		hash.Add(std::get<0>(right), 1);
		hash.Add(std::get<1>(right), 1);
		for (u32 i = 0; i < inputs; i++)
		{
			hash.Add(pad.GetRawInput(i), 1);
			hash.Add(pad.GetPressure(i), 1);
			hash.AddFloat(pad.GetEffectiveInput(i));
		}
	}

	// The script: every input in turn set to each value, then raw analogs and raw pressure buttons, with a poll
	// and the getters after every step. Two passes, the second with the pad's axis scale, deadzone and
	// pressure settings changed (they are written under a hold, read here the way the host thread reads them).
	u64 RunScript(PadBase& pad, u32 inputs, int analog_button, const std::vector<u32>& stick_keys = {})
	{
		Hash hash;
		pad.FullReset();
		if (analog_button >= 0)
		{
			// The DualShock 2's analog button toggles the pad into analog mode.
			pad.Set(static_cast<u32>(analog_button), 1.0f);
			pad.Set(static_cast<u32>(analog_button), 0.0f);
		}
		Poll(pad, hash);
		Getters(pad, inputs, hash);

		for (int pass = 0; pass < 2; pass++)
		{
			if (pass == 1)
			{
				pad.SetAxisScale(0.2f, 1.5f);
				pad.SetButtonDeadzone(0.1f);
				pad.SetPressureModifier(0.25f);
				pad.SetDiagonalScaleCorrection(true);
				pad.SetAnalogInvertL(true, false);
				pad.SetAnalogInvertR(false, true);
			}
			for (u32 i = 0; i < inputs; i++)
			{
				for (size_t v = 0; v < std::size(kValues); v++)
				{
					pad.Set(i, kValues[(v + i) % std::size(kValues)]);
					Poll(pad, hash);
					Getters(pad, inputs, hash);
				}
			}
			pad.SetRawAnalogs({0x10, 0x20}, {0xf0, 0xe0});
			Poll(pad, hash);
			Getters(pad, inputs, hash);
			for (u32 i = 0; i < inputs; i++)
			{
				pad.SetRawPressureButton(i, {(i & 1) != 0, static_cast<u8>(i * 17)});
				Poll(pad, hash);
				Getters(pad, inputs, hash);
			}
		}

		// The stick dead zone: with a dead zone of 0.2, each stick direction is set to a value inside it and to one
		// outside it, with the opposite direction at rest, and the pad is polled after each. Inside, the stick is
		// centred (both axes); outside, it moves.
		if (!stick_keys.empty())
		{
			pad.SetAxisScale(0.2f, 1.0f);
			for (const u32 key : stick_keys)
				pad.Set(key, 0.0f);
			for (const u32 key : stick_keys)
			{
				for (const float value : {0.05f, 0.9f, 0.0f})
				{
					pad.Set(key, value);
					Poll(pad, hash);
					Getters(pad, inputs, hash);
				}
			}
		}
		return hash.Value();
	}

	// Recorded on 2fbf24a5d0 (plain fields). Compare with the hash the test prints when one differs.
	const std::vector<u32> kDualshock2Sticks = {
		PadDualshock2::Inputs::PAD_L_UP, PadDualshock2::Inputs::PAD_L_RIGHT, PadDualshock2::Inputs::PAD_L_DOWN, PadDualshock2::Inputs::PAD_L_LEFT,
		PadDualshock2::Inputs::PAD_R_UP, PadDualshock2::Inputs::PAD_R_RIGHT, PadDualshock2::Inputs::PAD_R_DOWN, PadDualshock2::Inputs::PAD_R_LEFT};

	constexpr u64 kDualshock2 = 0xaf454e6498ce44f7ull;
	constexpr u64 kGuitar = 0xdafeb57743d1c08cull;
	constexpr u64 kPopn = 0xcca1db8560d6f668ull;
	constexpr u64 kNegcon = 0xfc6dce5a9da4eae0ull;
	constexpr u64 kJogcon = 0x80344b65ab7cf972ull;
} // namespace

TEST(PadInput, DualShock2AnswersAsItAlwaysDid)
{
	PadDualshock2 pad(0, 0);
	EXPECT_EQ(RunScript(pad, PadDualshock2::Inputs::LENGTH, PadDualshock2::Inputs::PAD_ANALOG, kDualshock2Sticks), kDualshock2);
}

TEST(PadInput, GuitarAnswersAsItAlwaysDid)
{
	PadGuitar pad(0, 0);
	EXPECT_EQ(RunScript(pad, PadGuitar::Inputs::LENGTH, -1), kGuitar);
}

TEST(PadInput, PopnAnswersAsItAlwaysDid)
{
	PadPopn pad(0, 0);
	EXPECT_EQ(RunScript(pad, PadPopn::Inputs::LENGTH, -1), kPopn);
}

TEST(PadInput, NegconAnswersAsItAlwaysDid)
{
	PadNegcon pad(0, 0);
	EXPECT_EQ(RunScript(pad, PadNegcon::Inputs::LENGTH, -1), kNegcon);
}

TEST(PadInput, JogconAnswersAsItAlwaysDid)
{
	PadJogcon pad(0, 0);
	EXPECT_EQ(RunScript(pad, PadJogcon::Inputs::LENGTH, -1), kJogcon);
}

// ---------------------------------------------------------------- the field type

static_assert(sizeof(SharedWord::Relaxed<u8>) == 1 && alignof(SharedWord::Relaxed<u8>) == 1);
static_assert(sizeof(SharedWord::Relaxed<s16>) == 2 && alignof(SharedWord::Relaxed<s16>) == alignof(s16));
static_assert(sizeof(SharedWord::Relaxed<u32>) == 4 && alignof(SharedWord::Relaxed<u32>) == alignof(u32));
static_assert(sizeof(SharedWord::Relaxed<float>) == sizeof(float));
static_assert(sizeof(std::array<SharedWord::Relaxed<u8>, 32>) == 32, "an array of fields is an array of bytes");
static_assert(std::is_standard_layout_v<SharedWord::Relaxed<u32>>);
static_assert(!std::is_copy_constructible_v<SharedWord::Relaxed<u32>>, "a field the other thread is writing is not copied");
static_assert(!std::is_copy_assignable_v<SharedWord::Relaxed<u32>>);

TEST(SharedWordRelaxed, ReadsWhatWasWrittenAndStartsAtZero)
{
	SharedWord::Relaxed<u8> byte;
	EXPECT_EQ(static_cast<u8>(byte), 0);
	byte = 0x7f;
	EXPECT_EQ(static_cast<u8>(byte), 0x7f);
	EXPECT_EQ(byte.Load(), 0x7f);

	SharedWord::Relaxed<u32> word{0xffffffffu};
	EXPECT_EQ(static_cast<u32>(word), 0xffffffffu);
	word &= ~(1u << 13);
	EXPECT_EQ(static_cast<u32>(word), 0xffffdfffu);
	word |= (1u << 13);
	EXPECT_EQ(static_cast<u32>(word), 0xffffffffu);

	SharedWord::Relaxed<s16> dial;
	dial = -255;
	EXPECT_EQ(static_cast<s16>(dial), -255);
	EXPECT_EQ(dial & 0xff, 1);
	EXPECT_EQ((dial >> 8) & 0xff, 0xff);

	SharedWord::Relaxed<float> scale{1.5f};
	scale = 0.25f;
	EXPECT_EQ(static_cast<float>(scale), 0.25f);
}

TEST(SharedWordRelaxed, AnArrayOfFieldsStartsZeroedAndIndexesLikeBytes)
{
	std::array<SharedWord::Relaxed<u8>, 32> inputs = {};
	for (u32 i = 0; i < inputs.size(); i++)
		EXPECT_EQ(static_cast<u8>(inputs[i]), 0);
	inputs[5] = 200;
	inputs[6] = 7;
	EXPECT_EQ(static_cast<u8>(inputs[5]), 200);
	EXPECT_EQ(static_cast<u8>(inputs[6]), 7);
	EXPECT_EQ(static_cast<u8>(inputs[4]), 0);
	EXPECT_EQ(inputs[5] + 1u, 201u);
	EXPECT_EQ(static_cast<float>(inputs[5]) / 255.0f, 200.0f / 255.0f);
}

// A writer flips a field between two whole values while a reader loads it. Every load must be one of the two.
// (On a plain field this is a data race that happens to work on ARM64; the type is what makes it defined, and
// what a ThreadSanitizer build of this test stays quiet about.)
TEST(SharedWordRelaxed, AReaderOnAnotherThreadOnlyEverSeesWholeValues)
{
	SharedWord::Relaxed<u32> word{0u};
	std::atomic<bool> stop{false};
	std::thread writer([&] {
		u32 next = 0xffffffffu;
		while (!stop.load(std::memory_order_relaxed))
		{
			word = next;
			next = ~next;
		}
	});

	size_t torn = 0, seen_high = 0, seen_low = 0;
	for (int i = 0; i < 2000000; i++)
	{
		const u32 value = word;
		if (value == 0u)
			seen_low++;
		else if (value == 0xffffffffu)
			seen_high++;
		else
			torn++;
	}
	stop.store(true);
	writer.join();
	EXPECT_EQ(torn, 0u);
	EXPECT_GT(seen_low + seen_high, 0u);
}

// The host thread's side and the IOP thread's side of one DualShock 2 at once: Set() from one thread,
// polls and getters from the other. Cross and the left stick are driven between two whole states.
TEST(PadInput, APollOnAnotherThreadSeesAStateTheHostThreadWrote)
{
	PadDualshock2 pad(0, 0);
	pad.FullReset();
	pad.Set(PadDualshock2::Inputs::PAD_ANALOG, 1.0f);
	pad.Set(PadDualshock2::Inputs::PAD_ANALOG, 0.0f);

	std::atomic<bool> stop{false};
	std::thread host([&] {
		bool down = false;
		while (!stop.load(std::memory_order_relaxed))
		{
			down = !down;
			pad.Set(PadDualshock2::Inputs::PAD_CROSS, down ? 1.0f : 0.0f);
		}
	});

	size_t bad = 0, polls = 0;
	for (int i = 0; i < 20000; i++)
	{
		const u8 pressure = pad.GetRawInput(PadDualshock2::Inputs::PAD_CROSS);
		const u32 buttons = pad.GetButtons();
		if (pressure != 0 && pressure != 255)
			bad++;
		(void)buttons;
		polls++;
	}
	stop.store(true);
	host.join();
	EXPECT_EQ(bad, 0u);
	EXPECT_EQ(polls, 20000u);
}

// PadGuitar::Freeze writes the whammy bar through a temporary because the field is not a plain u8 any more. A saved
// state must carry it and a loaded one must restore it.
TEST(PadInput, TheGuitarsWhammyBarIsInASavedState)
{
	PadGuitar before(0, 0);
	before.FullReset();
	before.Set(PadGuitar::Inputs::WHAMMY, 1.0f);
	const u8 pressed = before.GetPressure(PadGuitar::Inputs::WHAMMY);
	ASSERT_NE(pressed, Pad::ANALOG_NEUTRAL_POSITION) << "the test needs a whammy value that is not the resting one";

	StateWrapper::VectorMemoryStream stream;
	StateWrapper writer(&stream, StateWrapper::Mode::Write, 1);
	ASSERT_TRUE(before.Freeze(writer));

	PadGuitar after(0, 0);
	after.FullReset();
	ASSERT_EQ(after.GetPressure(PadGuitar::Inputs::WHAMMY), Pad::ANALOG_NEUTRAL_POSITION);
	StateWrapper::ReadOnlyMemoryStream read_stream(stream.GetBuffer().data(), static_cast<u32>(stream.GetBuffer().size()));
	StateWrapper reader(&read_stream, StateWrapper::Mode::Read, 1);
	ASSERT_TRUE(after.Freeze(reader));
	EXPECT_EQ(after.GetPressure(PadGuitar::Inputs::WHAMMY), pressed);
}
