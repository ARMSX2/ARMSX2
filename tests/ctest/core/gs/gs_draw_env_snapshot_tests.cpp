// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Contract suite for GSState::SetDrawBufferEnv -- the per-draw snapshot the vertex
// kick takes of the drawing environment.
//
// With draw buffering ON the snapshot is the whole 768-byte GSDrawingEnvironment,
// because the buffer list is live and CanBufferNewDraw walks it. With it OFF the
// draw's environment lives in m_prev_env alone and nothing is copied into the buffer.
//
// The environment is otherwise copied in byte literals (88 for the head, 96 for a
// context's register block: the kick's m_prev_env snapshot and FlushBuffers' restore)
// that are only correct while the transfer registers still sit at the tail of the
// environment and `scissor` still follows the register block.
// SnapshotLayoutConstants pins both.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "GS/GS.h"
#include "GS/GSState.h"

namespace
{
	class SnapshotProbe final : public GSState
	{
	public:
		void Draw() override {}

		using GSState::m_backed_up_ctx;
		using GSState::m_current_buffer_idx;
		using GSState::m_env;
		using GSState::m_env_buffers;
		using GSState::SetDrawBufferEnv;

		// Stand in for the vertex kick: it sets m_backed_up_ctx from PRIM.CTXT
		// immediately before taking the snapshot, and the snapshot's derived state is
		// indexed by that.
		void KickSnapshot(int ctx)
		{
			m_env.PRIM.CTXT = ctx;
			m_backed_up_ctx = ctx;
			SetDrawBufferEnv();
		}
	};

	// Draw buffering is a global; every test restores it.
	class DrawBufferingGuard
	{
	public:
		explicit DrawBufferingGuard(bool on)
			: m_saved(GSConfig.UserHacks_DrawBuffering)
		{
			GSConfig.UserHacks_DrawBuffering = on;
		}
		~DrawBufferingGuard() { GSConfig.UserHacks_DrawBuffering = m_saved; }

	private:
		bool m_saved;
	};

	// A distinguishable environment. The bytes are never interpreted -- the offset
	// members hold pointers that are only ever copied, never dereferenced -- so a
	// pattern fill is both legal and the sharpest probe: any byte the snapshot fails
	// to carry shows up against the buffer's own fill.
	void FillEnvironment(GSDrawingEnvironment& env, u8 seed)
	{
		u8* const p = reinterpret_cast<u8*>(&env);
		for (size_t i = 0; i < sizeof(GSDrawingEnvironment); i++)
			p[i] = static_cast<u8>(seed + (i * 7));
	}

	constexpr size_t kEnvHead = 88;
	constexpr size_t kContextRegs = 96;

	// offsetof is not standard-layout-clean on these types (GSOffset carries a private
	// base), so the offsets are taken off a real object instead.
	size_t OffsetOf(const void* base, const void* member)
	{
		return static_cast<size_t>(reinterpret_cast<const u8*>(member) - reinterpret_cast<const u8*>(base));
	}
} // namespace

// The byte literals the environment is copied in. Each one is a statement about the
// layout, and each is silently wrong if a register is added in the wrong place.
TEST(GsDrawEnvSnapshot, SnapshotLayoutConstants)
{
	const auto env = std::make_unique<GSDrawingEnvironment>();

	// The head stops where the transfer registers begin: BITBLTBUF, TRXDIR, TRXPOS
	// and TRXREG are not draw state and nothing reads them out of a snapshot.
	EXPECT_EQ(OffsetOf(env.get(), &env->BITBLTBUF), kEnvHead);
	EXPECT_EQ(OffsetOf(env.get(), &env->CTXT[0]), size_t(128));
	EXPECT_EQ(sizeof(GSDrawingEnvironment), size_t(768));

	// 96 is a context's register block only while scissor still follows it.
	EXPECT_EQ(OffsetOf(&env->CTXT[0], &env->CTXT[0].scissor), kContextRegs);
	EXPECT_EQ(OffsetOf(&env->CTXT[0], &env->CTXT[0].offset), size_t(144));
	EXPECT_EQ(sizeof(GSDrawingContext), size_t(320));
}

// Draw buffering ON: the snapshot is the whole environment, byte for byte. This is
// the path two of the twenty-two corpus dumps take (Stuntman and Armored Core 3 carry
// the GameDB fix).
TEST(GsDrawEnvSnapshot, DrawBufferingOnCopiesTheWholeEnvironment)
{
	auto s = std::make_unique<SnapshotProbe>();
	const DrawBufferingGuard guard(true);

	FillEnvironment(s->m_env, 0x11);
	std::memset(&s->m_env_buffers[0].m_env, 0x5A, sizeof(GSDrawingEnvironment));

	s->KickSnapshot(1);

	EXPECT_EQ(std::memcmp(&s->m_env_buffers[0].m_env, &s->m_env, sizeof(GSDrawingEnvironment)), 0)
		<< "with draw buffering on the snapshot must still be a whole-environment copy";
	EXPECT_EQ(s->m_env_buffers[0].m_backed_up_ctx, 1);
}
