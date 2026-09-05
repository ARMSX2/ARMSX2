// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Contract suite for GSState::SetDrawBufferEnv -- the per-draw snapshot the vertex
// kick takes of the drawing environment.
//
// With draw buffering ON the snapshot is the whole 768-byte GSDrawingEnvironment,
// because the buffer list is live and CanBufferNewDraw walks it.
//
// With draw buffering OFF only buffer 0 ever exists, and the snapshot is narrowed to
// the 480 bytes anything reads back: the environment head, BOTH contexts' register
// blocks, and the ACTIVE context's derived scissor and offset. That set is not
// arbitrary -- it is exactly what FlushBuffers restores into m_prev_env, plus the
// PRIM / register-block members the three m_used_buffers_idx loops read
// (GIFRegHandlerTEX0's CLUT flush, CheckWriteOverlap, CheckCLUTValidity).
//
// The two mistakes this suite exists to catch:
//   * narrowing to the ACTIVE context only. FlushBuffers restores both contexts'
//     register blocks, so a snapshot missing the inactive one silently hands the
//     next draw a stale TEX0/FRAME/TEST for the context it is about to switch to.
//   * a layout change. The copy is written in byte literals (88 for the head, 96 for
//     a context's register block) that are only correct while the transfer registers
//     still sit at the tail of the environment and `scissor` still follows the
//     register block. SnapshotLayoutConstants pins both.

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

// The byte literals the narrow copy is written in. Each one is a statement about the
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
// the GameDB fix), and the narrowing must not reach it.
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

// Draw buffering OFF: everything FlushBuffers restores must be in the snapshot, for
// either active context.
TEST(GsDrawEnvSnapshot, NarrowSnapshotCarriesEverythingFlushBuffersRestores)
{
	for (int ctx = 0; ctx < 2; ctx++)
	{
		auto s = std::make_unique<SnapshotProbe>();
		const DrawBufferingGuard guard(false);

		FillEnvironment(s->m_env, static_cast<u8>(0x20 + ctx));
		std::memset(&s->m_env_buffers[0].m_env, 0x5A, sizeof(GSDrawingEnvironment));

		s->KickSnapshot(ctx);

		const GSDrawingEnvironment& snap = s->m_env_buffers[0].m_env;

		EXPECT_EQ(std::memcmp(&snap, &s->m_env, kEnvHead), 0) << "env head, ctx " << ctx;
		EXPECT_EQ(std::memcmp(&snap.CTXT[0], &s->m_env.CTXT[0], kContextRegs), 0)
			<< "context 0 register block, ctx " << ctx;
		EXPECT_EQ(std::memcmp(&snap.CTXT[1], &s->m_env.CTXT[1], kContextRegs), 0)
			<< "context 1 register block, ctx " << ctx;
		EXPECT_EQ(std::memcmp(&snap.CTXT[ctx].scissor, &s->m_env.CTXT[ctx].scissor,
					  sizeof(s->m_env.CTXT[ctx].scissor)),
			0) << "active context scissor, ctx " << ctx;
		EXPECT_EQ(std::memcmp(&snap.CTXT[ctx].offset, &s->m_env.CTXT[ctx].offset,
					  sizeof(s->m_env.CTXT[ctx].offset)),
			0) << "active context offset, ctx " << ctx;
		EXPECT_EQ(s->m_env_buffers[0].m_backed_up_ctx, ctx);
	}
}

// The mistake worth naming: FlushBuffers restores BOTH contexts' register blocks, so
// a snapshot that carries only the active context leaves the other one stale. A guest
// that writes context 1's registers while drawing with context 0 -- ordinary traffic --
// is enough to expose it.
TEST(GsDrawEnvSnapshot, InactiveContextRegisterBlockRidesAlong)
{
	auto s = std::make_unique<SnapshotProbe>();
	const DrawBufferingGuard guard(false);

	FillEnvironment(s->m_env, 0x30);
	std::memset(&s->m_env_buffers[0].m_env, 0x5A, sizeof(GSDrawingEnvironment));

	// Drawing with context 0; context 1's registers were just written by the guest.
	s->m_env.CTXT[1].TEX0.U64 = 0x0123456789ABCDEFull;
	s->m_env.CTXT[1].FRAME.U64 = 0xFEDCBA9876543210ull;
	s->m_env.CTXT[1].TEST.U32[0] = 0xA5A5A5A5u;

	s->KickSnapshot(0);

	const GSDrawingEnvironment& snap = s->m_env_buffers[0].m_env;
	EXPECT_EQ(snap.CTXT[1].TEX0.U64, s->m_env.CTXT[1].TEX0.U64);
	EXPECT_EQ(snap.CTXT[1].FRAME.U64, s->m_env.CTXT[1].FRAME.U64);
	EXPECT_EQ(snap.CTXT[1].TEST.U32[0], s->m_env.CTXT[1].TEST.U32[0]);
}

// A context switch must refresh the derived state of the context that is now active.
// A narrowing that only ever wrote one context's scissor/offset passes the single-draw
// tests above and fails here.
TEST(GsDrawEnvSnapshot, ContextSwitchRefreshesTheNewlyActiveDerivedState)
{
	auto s = std::make_unique<SnapshotProbe>();
	const DrawBufferingGuard guard(false);

	FillEnvironment(s->m_env, 0x40);
	std::memset(&s->m_env_buffers[0].m_env, 0x5A, sizeof(GSDrawingEnvironment));

	s->KickSnapshot(0);

	// The guest switches to context 1 and its scissor is recomputed.
	s->m_env.CTXT[1].scissor.in = GSVector4i(11, 22, 333, 444);
	s->m_env.CTXT[1].scissor.cull = GSVector4i(1, 2, 3, 4);
	s->m_env.CTXT[1].scissor.xyof = GSVector4i(5, 6, 7, 8);

	s->KickSnapshot(1);

	const GSDrawingEnvironment& snap = s->m_env_buffers[0].m_env;
	EXPECT_EQ(std::memcmp(&snap.CTXT[1].scissor, &s->m_env.CTXT[1].scissor,
				  sizeof(s->m_env.CTXT[1].scissor)),
		0);
	EXPECT_EQ(std::memcmp(&snap.CTXT[1].offset, &s->m_env.CTXT[1].offset,
				  sizeof(s->m_env.CTXT[1].offset)),
		0);
	EXPECT_EQ(s->m_env_buffers[0].m_backed_up_ctx, 1);
}
