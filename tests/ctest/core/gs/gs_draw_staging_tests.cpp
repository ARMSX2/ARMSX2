// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Regression suite for the draw-staging arrays (GSState::m_draw_vertex /
// m_draw_index) and their relationship to vertex-buffer growth.
//
// These two arrays are write-then-consume snapshots taken at draw time so a draw
// can read the vertices while the backend also writes them. Their capacity is
// deliberately independent of m_vertex/m_index, which point at a rotating set of
// independently-sized draw slots and pooled draw-node arrays.
//
// A God of War II crash reported on Android (2026-07-29, release 2.6.6) was a
// ~1.19 MB out-of-bounds READ born of coupling the two: GrowVertexBuffer listed
// the staging arrays alongside the real vertex/index buffers and copied
// `sizeof(GSVertex) * m_vertex->tail` bytes out of them, a length with no
// relationship to their allocation. The buffer whose tail was read had grown to
// ~50k vertices while the staging array was still the 10k one from init, so the
// copy ran ~1.1 MB off the end and SIGSEGV'd inside memcpy on the MTGS thread.
//
// The properties pinned here are the two that make that unrepresentable:
//   1. growth of the vertex/index buffers must not touch the staging arrays;
//   2. staging capacity is established at the point of use, covers what was
//      asked for, and never shrinks.
//
// GrowthDoesNotTouchStagingArrays fails on its own (the staging pointers move)
// if the arrays are ever re-listed in GrowVertexBuffer. Building with
// -DUSE_ASAN=ON additionally turns the memset/memcpy probes into real bounds
// checks and reproduces the original fault outright — re-coupling the arrays and
// running this suite under ASan reports, in 2ms:
//
//   ERROR: AddressSanitizer: heap-buffer-overflow
//   READ of size 319904 ... 0 bytes after 128000-byte region
//     #1 GSState::GrowVertexBuffer()
//
// i.e. the same over-read, in the same function, that SIGSEGV'd on the tester's
// device. Without ASan only the pointer-identity assertion catches it, so keep
// that assertion even if the probes look redundant.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "GS/GSState.h"

namespace
{
	// Re-exposes the protected growth/staging surface. GSState itself is concrete,
	// and with the default GSBackThreadMode::Off the constructor stays local: no
	// back thread, no node pool, no GS device.
	class StagingProbe final : public GSState
	{
	public:
		// The only pure virtual; nothing here reaches a draw.
		void Draw() override {}

		using GSState::EnsureDrawStaging;
		using GSState::GrowVertexBuffer;
		using GSState::MAX_DRAW_BUFFERS;
		using GSState::m_draw_index;
		using GSState::m_draw_index_alloc;
		using GSState::m_draw_vertex;
		using GSState::m_draw_vertex_alloc;
		using GSState::m_index;
		using GSState::m_index_buffers;
		using GSState::m_vertex;
		using GSState::m_vertex_buffers;

		// Grow the currently-selected slot once, the way DrawingKick does: tail is
		// at capacity, so the preserved copy of the *real* vertex buffer spans the
		// whole live range.
		void GrowCurrentSlotAtCapacity()
		{
			m_vertex->tail = m_vertex->maxcount;
			GrowVertexBuffer();
		}

		void SelectSlot(int i)
		{
			m_vertex = &m_vertex_buffers[i];
			m_index = &m_index_buffers[i];
		}

		// Touch every byte the caller was promised. Meaningless without ASan,
		// a bounds check with it.
		void ScribbleStaging(u32 vertex_count, u32 index_count)
		{
			if (vertex_count)
				std::memset(m_draw_vertex.buff, 0xAB, sizeof(GSVertex) * vertex_count);
			if (index_count)
				std::memset(m_draw_index.buff, 0xCD, sizeof(u16) * index_count);
		}
	};
} // namespace

// The historical bug, stated directly: growing the vertex/index buffers must
// leave the staging arrays completely alone — same pointers, same capacity.
// Re-coupling them is what produced the out-of-bounds read.
TEST(GsDrawStaging, GrowthDoesNotTouchStagingArrays)
{
	auto s = std::make_unique<StagingProbe>();

	s->EnsureDrawStaging(4000, 9000);
	ASSERT_NE(s->m_draw_vertex.buff, nullptr);
	ASSERT_NE(s->m_draw_index.buff, nullptr);

	GSVertex* const vbuff = s->m_draw_vertex.buff;
	u16* const ibuff = s->m_draw_index.buff;
	const u32 valloc = s->m_draw_vertex_alloc;
	const u32 ialloc = s->m_draw_index_alloc;

	// Several growths, on more than one slot, so the slot capacities diverge from
	// each other and from the staging arrays — the exact state the crash needed.
	for (int round = 0; round < 4; round++)
	{
		s->SelectSlot(0);
		s->GrowCurrentSlotAtCapacity();
	}
	s->SelectSlot(1);
	s->GrowCurrentSlotAtCapacity();

	EXPECT_GT(s->m_vertex_buffers[0].maxcount, s->m_vertex_buffers[1].maxcount)
		<< "slot capacities must be free to diverge for this test to mean anything";

	EXPECT_EQ(s->m_draw_vertex.buff, vbuff);
	EXPECT_EQ(s->m_draw_index.buff, ibuff);
	EXPECT_EQ(s->m_draw_vertex_alloc, valloc);
	EXPECT_EQ(s->m_draw_index_alloc, ialloc);

	// Still exactly as large as it was promised to be.
	s->ScribbleStaging(4000, 9000);
}

// Staging capacity must cover the request and never shrink, however the request
// sizes move around. The shrink case is the one that mattered: the crash needed
// a staging array smaller than the data about to be staged into it.
TEST(GsDrawStaging, StagingCapacityCoversRequestAndNeverShrinks)
{
	auto s = std::make_unique<StagingProbe>();

	s->EnsureDrawStaging(10, 10);
	EXPECT_GE(s->m_draw_vertex_alloc, 10u);
	EXPECT_GE(s->m_draw_index_alloc, 10u);
	s->ScribbleStaging(10, 10);

	// Grow well past it.
	s->EnsureDrawStaging(50000, 300000);
	EXPECT_GE(s->m_draw_vertex_alloc, 50000u);
	EXPECT_GE(s->m_draw_index_alloc, 300000u);
	s->ScribbleStaging(50000, 300000);

	const u32 high_valloc = s->m_draw_vertex_alloc;
	const u32 high_ialloc = s->m_draw_index_alloc;

	// A smaller draw must not hand back the capacity — the next big one would
	// otherwise stage into an array sized for the small one.
	s->EnsureDrawStaging(5, 5);
	EXPECT_EQ(s->m_draw_vertex_alloc, high_valloc);
	EXPECT_EQ(s->m_draw_index_alloc, high_ialloc);
	s->ScribbleStaging(high_valloc, high_ialloc);
}

// The observed failure shape, as a test: staging sized at init-scale (10k
// vertices / 60k indices), then a draw arrives carrying a slot that the parser
// grew to ~50k. Pre-fix this staged 49108 vertices into a 10000-vertex array.
TEST(GsDrawStaging, StagingCoversABufferGrownElsewhere)
{
	auto s = std::make_unique<StagingProbe>();

	// Init-scale staging, matching what ResetDrawBuffers used to leave behind.
	s->EnsureDrawStaging(10000, 60000);
	ASSERT_GE(s->m_draw_vertex_alloc, 10000u);

	// A slot grown far past that, independently of the staging arrays.
	s->SelectSlot(0);
	while (s->m_vertex->maxcount < 49108)
		s->GrowCurrentSlotAtCapacity();

	const u32 live_verts = 49108;
	const u32 live_indices = 85398;
	ASSERT_GE(s->m_vertex->maxcount, live_verts);

	// What SetupIA does before staging.
	s->EnsureDrawStaging(live_verts, live_indices);
	EXPECT_GE(s->m_draw_vertex_alloc, live_verts);
	EXPECT_GE(s->m_draw_index_alloc, live_indices);

	// The staging copies themselves, at full live size.
	std::memcpy(s->m_draw_vertex.buff, s->m_vertex->buff, sizeof(GSVertex) * live_verts);
	std::memcpy(s->m_draw_index.buff, s->m_index->buff, sizeof(u16) * live_indices);
}
