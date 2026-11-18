// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// What a draw recorded with raw calls must bind after the draw before it in the same batch (GSRawBindCache.h): the row
// emitter's held classic twins, recorded back to back.

#include <gtest/gtest.h>

#include "GS/Renderers/Common/GSRawBindCache.h"

namespace
{
	GSRawBindCache::State Twin()
	{
		GSRawBindCache::State s;
		s.pipeline = 1;
		s.constants_set = 2;
		s.constants[0] = 256;
		s.constants[1] = 512;
		s.textures[0] = 3;
		s.textures[1] = 4;
		s.viewport[0] = 640;
		s.viewport[1] = 448;
		s.scissor[2] = 640;
		s.scissor[3] = 448;
		s.index_buffer = 5;
		s.vertex_buffer = 6;
		return s;
	}
} // namespace

TEST(GSRawBindCache, TheFirstDrawOfABatchBindsEverythingItUses)
{
	GSRawBindCache c;
	EXPECT_EQ(c.Needed(Twin()), GSRawBindCache::ALL & ~GSRawBindCache::BLEND);
	GSRawBindCache::State b = Twin();
	b.blend = 0x100 | 64;
	GSRawBindCache d;
	EXPECT_EQ(d.Needed(b), GSRawBindCache::ALL);
}

TEST(GSRawBindCache, ATwinLikeTheOneBeforeBindsNothing)
{
	GSRawBindCache c;
	c.Needed(Twin());
	EXPECT_EQ(c.Needed(Twin()), 0u);
}

TEST(GSRawBindCache, EachDifferenceBindsItsOwnState)
{
	GSRawBindCache c;
	c.Needed(Twin());
	GSRawBindCache::State s = Twin();
	s.constants[0] = 512;
	EXPECT_EQ(c.Needed(s), GSRawBindCache::CONSTANTS);
	s.textures[0] = 7;
	EXPECT_EQ(c.Needed(s), GSRawBindCache::TEXTURES);
	s.pipeline = 8;
	s.scissor[0] = 16;
	EXPECT_EQ(c.Needed(s), GSRawBindCache::PIPELINE | GSRawBindCache::SCISSOR);
	s.viewport[0] = 1280;
	s.vertex_buffer = 9;
	EXPECT_EQ(c.Needed(s), GSRawBindCache::VIEWPORT | GSRawBindCache::VERTEX);
	s.index_buffer = 10;
	EXPECT_EQ(c.Needed(s), GSRawBindCache::INDEX);
}

TEST(GSRawBindCache, ABlendConstantIsBoundOnlyWhereItIsUsed)
{
	GSRawBindCache c;
	GSRawBindCache::State a = Twin();
	a.blend = 0x100 | 64;
	c.Needed(a);
	GSRawBindCache::State b = Twin(); // does not use it
	EXPECT_EQ(c.Needed(b), 0u);
	EXPECT_EQ(c.Needed(a), 0u); // still bound from the first draw
	a.blend = 0x100 | 32;
	EXPECT_EQ(c.Needed(a), GSRawBindCache::BLEND);
}

TEST(GSRawBindCache, ResetStartsANewBatch)
{
	GSRawBindCache c;
	c.Needed(Twin());
	c.Reset();
	EXPECT_EQ(c.Needed(Twin()), GSRawBindCache::ALL & ~GSRawBindCache::BLEND);
}
