// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Which texture bindings each group of a short run pushes when it is recorded with the ordinary calls, and what the
// flush pushes after the waiting groups (GSShortRunTextures.h). GSDeviceVK.cpp asserts that the binding numbers and the
// flag mask used here are its own.

#include <gtest/gtest.h>

#include "GS/Renderers/Common/GSShortRunTextures.h"

#include <array>
#include <bit>
#include <vector>

namespace
{
	constexpr u32 TEX = 1u << GSShortRunTextures::TEXTURE;
	constexpr u32 PAL = 1u << GSShortRunTextures::PALETTE;
	constexpr u32 RT = 1u << GSShortRunTextures::RT;
	constexpr u32 ALL = GSShortRunTextures::ALL;
	constexpr u32 MIN_GROUPS = 3; // a run this long goes through the driver's entry
	constexpr int UNKNOWN = -1;

	/// The device's record (texture flags, current textures) and what set 1 holds on the GPU, as GSDeviceVK and the
	/// driver keep them. Textures are numbers. The entry binds set-1 memory of its own and leaves the push set the next
	/// ordinary push starts from as it was, so after a run through it nothing is known.
	struct Device
	{
		struct Waiting
		{
			u32 flags, reads;
			std::array<int, GSShortRunTextures::BINDINGS> textures;
		};

		u32 dirty = ALL;
		std::array<int, GSShortRunTextures::BINDINGS> current{};
		std::array<int, GSShortRunTextures::BINDINGS> gpu{};
		std::vector<Waiting> waiting;
		// A draw whose state was taken for the run but whose draw has not come (something used the command buffer in
		// between): the flags it took, and the bindings the flush pushed for it.
		bool open = false;
		u32 open_flags = 0;
		u32 open_pushed = 0;

		Device() { gpu.fill(UNKNOWN); }

		void Set(u32 binding, int texture)
		{
			if (current[binding] != texture)
				dirty |= 1u << binding;
			current[binding] = texture;
		}

		/// A draw that joins the run: its state is taken with the flags owed now.
		void Join(u32 reads)
		{
			waiting.push_back({dirty, reads, current});
			dirty = 0;
		}

		/// A draw that would join the run takes its state; its draw has not come yet.
		void Open()
		{
			open = true;
			open_flags = dirty;
			dirty = 0;
		}

		/// FlushRun. An open group is given to the ordinary calls after the waiting groups, with the flags it took less
		/// what the flush pushed.
		void Flush()
		{
			u32 restore = 0;
			if (!waiting.empty())
				restore = RecordWaiting();
			if (open)
			{
				open = false;
				open_pushed = open_flags & ~restore;
				for (u32 bits = open_pushed; bits != 0; bits &= bits - 1)
					gpu[std::countr_zero(bits)] = current[std::countr_zero(bits)];
			}
		}

		u32 RecordWaiting()
		{
			u32 restore;
			if (waiting.size() >= MIN_GROUPS)
			{
				gpu.fill(UNKNOWN);
				restore = GSShortRunTextures::AfterEntry();
			}
			else
			{
				GSShortRunTextures owed;
				for (const Waiting& g : waiting)
				{
					for (u32 bits = owed.Push(g.flags, g.reads); bits != 0; bits &= bits - 1)
						gpu[std::countr_zero(bits)] = g.textures[std::countr_zero(bits)];
				}
				restore = owed.Finish();
			}
			for (u32 bits = restore; bits != 0; bits &= bits - 1)
				gpu[std::countr_zero(bits)] = current[std::countr_zero(bits)];
			waiting.clear();
			return restore;
		}

		/// True when every binding in `reads` holds its current texture.
		bool Holds(u32 reads) const
		{
			for (u32 bits = reads; bits != 0; bits &= bits - 1)
			{
				if (gpu[std::countr_zero(bits)] != current[std::countr_zero(bits)])
					return false;
			}
			return true;
		}

		/// A draw that does not join the run (vertex expansion, say). It reads its flags first; using the command buffer
		/// then records the waiting groups; then it pushes what its flags name. True when every binding it samples holds
		/// its texture.
		bool OrdinaryDraw(u32 reads)
		{
			const u32 flags = dirty;
			dirty = 0;
			Flush();
			for (u32 bits = flags; bits != 0; bits &= bits - 1)
				gpu[std::countr_zero(bits)] = current[std::countr_zero(bits)];
			return Holds(reads);
		}
	};
} // namespace

TEST(GSShortRunTextures, AGroupPushesOnlyWhatItReads)
{
	GSShortRunTextures t;
	EXPECT_EQ(t.Push(ALL, TEX | PAL), TEX | PAL);
	EXPECT_EQ(t.Finish(), ALL & ~(TEX | PAL));
	EXPECT_EQ(t.owed, 0u);
}

// An untextured group cannot push the texture and palette it leaves set; the textured group after it, which was taken
// with nothing owed because the texture did not change, pushes them.
TEST(GSShortRunTextures, AnUntexturedGroupLeavesTheTextureToTheNextGroupThatReadsIt)
{
	GSShortRunTextures t;
	EXPECT_EQ(t.Push(ALL, 0), 0u);
	EXPECT_EQ(t.Push(0, TEX | PAL), TEX | PAL);
	EXPECT_EQ(t.Finish(), ALL & ~(TEX | PAL));
}

TEST(GSShortRunTextures, NothingOwedPushesNothing)
{
	GSShortRunTextures t;
	EXPECT_EQ(t.Push(0, TEX | PAL), 0u);
	EXPECT_EQ(t.Push(0, RT), 0u);
	EXPECT_EQ(t.Finish(), 0u);
}

TEST(GSShortRunTextures, AfterTheEntryEveryBindingIsPushed)
{
	EXPECT_EQ(GSShortRunTextures::AfterEntry(), ALL);
}

// An entry run ends on texture 3 and palette 4. One untextured draw joins a new run and takes every flag. The draw that
// ends that run is vertex-expanded, samples the unchanged texture 3 with a new palette 5, and reads its flags before
// the run is recorded.
TEST(GSShortRunTextures, AVertexExpandedDrawAfterAnUntexturedGroupSamplesItsTexture)
{
	Device d;
	d.Set(GSShortRunTextures::TEXTURE, 3);
	d.Set(GSShortRunTextures::PALETTE, 4);
	for (u32 i = 0; i < MIN_GROUPS; i++)
		d.Join(TEX | PAL);
	EXPECT_TRUE(d.OrdinaryDraw(0));
	d.Join(0);
	d.Set(GSShortRunTextures::PALETTE, 5);
	EXPECT_TRUE(d.OrdinaryDraw(TEX | PAL));
}

// An ordinary draw pushes texture 1. A run through the entry ends on texture 3. The next draw does not join, samples
// texture 3 with a new palette, and reads its flags before the run is recorded.
TEST(GSShortRunTextures, AnOrdinaryDrawAfterAnEntryRunSamplesItsTexture)
{
	Device d;
	d.Set(GSShortRunTextures::TEXTURE, 1);
	EXPECT_TRUE(d.OrdinaryDraw(TEX));
	d.Set(GSShortRunTextures::TEXTURE, 3);
	for (u32 i = 0; i < MIN_GROUPS; i++)
		d.Join(TEX);
	d.Set(GSShortRunTextures::PALETTE, 5);
	EXPECT_TRUE(d.OrdinaryDraw(TEX | PAL));
}

// The same draw joining a short run instead: it pushes only what it owes, so the run before must have left the rest.
TEST(GSShortRunTextures, AGroupAfterAnEntryRunSamplesItsTexture)
{
	Device d;
	d.Set(GSShortRunTextures::TEXTURE, 3);
	for (u32 i = 0; i < MIN_GROUPS; i++)
		d.Join(TEX);
	d.Flush();
	d.Set(GSShortRunTextures::PALETTE, 5);
	d.Join(TEX | PAL);
	d.Flush();
	EXPECT_EQ(d.gpu[GSShortRunTextures::TEXTURE], 3);
	EXPECT_EQ(d.gpu[GSShortRunTextures::PALETTE], 5);
}

// A run through the entry is waiting when the next draw, with a new texture, takes its state; then something uses the
// command buffer before its draw. The flush pushes every binding after the entry, so the open group pushes nothing more,
// and its draw samples the new texture.
TEST(GSShortRunTextures, AnOpenGroupAfterAnEntryRunTakesWhatTheFlushPushed)
{
	Device d;
	d.Set(GSShortRunTextures::TEXTURE, 3);
	for (u32 i = 0; i < MIN_GROUPS; i++)
		d.Join(TEX);
	d.Set(GSShortRunTextures::TEXTURE, 6);
	d.Open();
	d.Flush();
	EXPECT_EQ(d.open_pushed, 0u);
	EXPECT_TRUE(d.Holds(TEX | PAL));
}

// The same after a short run of one untextured group: the flush pushes what the group left owed, the open group pushes
// only the palette it took that the flush did not.
TEST(GSShortRunTextures, AnOpenGroupAfterAShortRunPushesOnlyWhatTheFlushDidNot)
{
	Device d;
	d.Set(GSShortRunTextures::TEXTURE, 3);
	d.Set(GSShortRunTextures::PALETTE, 4);
	EXPECT_TRUE(d.OrdinaryDraw(TEX | PAL));
	d.Set(GSShortRunTextures::RT, 7);
	d.Join(0);
	d.Set(GSShortRunTextures::PALETTE, 5);
	d.Open();
	d.Flush();
	EXPECT_EQ(d.open_pushed, PAL);
	EXPECT_TRUE(d.Holds(TEX | PAL | RT));
}

// With nothing waiting, the open group pushes what it took.
TEST(GSShortRunTextures, AnOpenGroupAloneTakesTheOrdinaryCalls)
{
	Device d;
	d.Set(GSShortRunTextures::TEXTURE, 3);
	EXPECT_TRUE(d.OrdinaryDraw(TEX));
	d.Set(GSShortRunTextures::PALETTE, 5);
	d.Open();
	d.Flush();
	EXPECT_EQ(d.open_pushed, PAL);
	EXPECT_TRUE(d.Holds(TEX | PAL));
}
