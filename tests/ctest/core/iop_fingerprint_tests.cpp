// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ee_divtrace::FingerprintIop is the IOP half of the whole-machine fingerprint that the
// IOP-thread work is gated on. Two properties matter: taking it must not change the
// emulation it describes, and each of its four hashes must move when the state it covers
// moves. The twin run gate shows the first on real games; these show both without a game.

#ifdef PCSX2_RECOMPILER_TESTS

#include "Common.h"
#include "CDVD/CDVD.h"
#include "SIO/Sio0.h"
#include "SIO/Sio2.h"
#include "SPU2/defs.h"
#include "Sif.h"
#include "ee_divtrace.h"

#include <gtest/gtest.h>

#include <cstring>
#include <deque>
#include <vector>

namespace
{
	using ee_divtrace::FingerprintIop;
	using ee_divtrace::IopFingerprint;

	bool Equal(const IopFingerprint& a, const IopFingerprint& b)
	{
		return a.spu2 == b.spu2 && a.sif == b.sif && a.sio == b.sio && a.cdvd == b.cdvd;
	}

	// Flips one thing, takes the fingerprint, and puts the thing back.
	template <typename Poke, typename Unpoke>
	IopFingerprint WithPoke(Poke&& poke, Unpoke&& unpoke)
	{
		poke();
		const IopFingerprint fp = FingerprintIop();
		unpoke();
		return fp;
	}
} // namespace

TEST(IopFingerprint, SameStateGivesTheSameHashes)
{
	const IopFingerprint a = FingerprintIop();
	const IopFingerprint b = FingerprintIop();
	EXPECT_TRUE(Equal(a, b));
}

TEST(IopFingerprint, TakingItLeavesTheStateItReadsAlone)
{
	// Everything it reads, copied before and compared after. The SPU2 freeze copies Cores and
	// rewrites pointers in the copy only; a freeze that rewrote them in place, or ran the mixer,
	// would show here.
	const _sif sif0_before = sif0, sif1_before = sif1, sif2_before = sif2;
	const cdvdStruct cdvd_before = cdvd;
	const std::vector<u8> cores_before(reinterpret_cast<const u8*>(Cores), reinterpret_cast<const u8*>(Cores) + sizeof(Cores));
	const V_SPDIF spdif_before = Spdif;
	const std::vector<s16> mem_before(_spu2mem, _spu2mem + std::size(_spu2mem));
	const std::vector<s16> regs_before(spu2regs, spu2regs + std::size(spu2regs));
	const u16 out_pos = OutPos, in_pos = InputPos;
	const u32 cycles = Cycles;
	const int play_mode = PlayMode;
	const std::deque<u8> fifo_in = g_Sio2FifoIn, fifo_out = g_Sio2FifoOut;
	const u8 sio_command = g_Sio0.sioCommand;

	FingerprintIop();

	EXPECT_EQ(0, std::memcmp(&sif0_before, &sif0, sizeof(_sif)));
	EXPECT_EQ(0, std::memcmp(&sif1_before, &sif1, sizeof(_sif)));
	EXPECT_EQ(0, std::memcmp(&sif2_before, &sif2, sizeof(_sif)));
	EXPECT_EQ(0, std::memcmp(&cdvd_before, &cdvd, sizeof(cdvdStruct)));
	EXPECT_EQ(0, std::memcmp(cores_before.data(), Cores, sizeof(Cores)));
	EXPECT_EQ(0, std::memcmp(&spdif_before, &Spdif, sizeof(V_SPDIF)));
	EXPECT_EQ(0, std::memcmp(mem_before.data(), _spu2mem, sizeof(_spu2mem)));
	EXPECT_EQ(0, std::memcmp(regs_before.data(), spu2regs, sizeof(spu2regs)));
	EXPECT_EQ(out_pos, OutPos);
	EXPECT_EQ(in_pos, InputPos);
	EXPECT_EQ(cycles, Cycles);
	EXPECT_EQ(play_mode, PlayMode);
	EXPECT_EQ(fifo_in, g_Sio2FifoIn);
	EXPECT_EQ(fifo_out, g_Sio2FifoOut);
	EXPECT_EQ(sio_command, g_Sio0.sioCommand);
}

TEST(IopFingerprint, Spu2HashMovesOnlyWithSpu2State)
{
	const IopFingerprint base = FingerprintIop();

	// A voice register of the second core.
	const IopFingerprint pitch = WithPoke([] { Cores[1].Voices[23].Pitch ^= 0x100; }, [] { Cores[1].Voices[23].Pitch ^= 0x100; });
	EXPECT_NE(base.spu2, pitch.spu2);
	EXPECT_EQ(base.sif, pitch.sif);
	EXPECT_EQ(base.sio, pitch.sio);
	EXPECT_EQ(base.cdvd, pitch.cdvd);

	// One word of sample memory.
	const IopFingerprint mem = WithPoke([] { _spu2mem[0x0FFFFF] ^= 1; }, [] { _spu2mem[0x0FFFFF] ^= 1; });
	EXPECT_NE(base.spu2, mem.spu2);

	// One raw register.
	const IopFingerprint reg = WithPoke([] { spu2regs[0x40] ^= 1; }, [] { spu2regs[0x40] ^= 1; });
	EXPECT_NE(base.spu2, reg.spu2);

	// The mixing clock.
	const IopFingerprint clock = WithPoke([] { Cycles ^= 1; }, [] { Cycles ^= 1; });
	EXPECT_NE(base.spu2, clock.spu2);

	EXPECT_TRUE(Equal(base, FingerprintIop())) << "the pokes were not undone";
}

TEST(IopFingerprint, Spu2HashSeesWhichCacheBlockAVoiceReadsButNotWhereTheCacheLives)
{
	// V_Voice::SBuffer is a host pointer into pcm_cache_data. The hash has to follow which block
	// it names (so it moves), and not the address of the cache (so it matches across processes).
	// Only the first can be checked in one process: two different blocks, and null.
	s16* const saved = Cores[0].Voices[0].SBuffer;
	Cores[0].Voices[0].SBuffer = nullptr;
	const IopFingerprint null_buffer = FingerprintIop();
	Cores[0].Voices[0].SBuffer = pcm_cache_data[5].Sampledata;
	const IopFingerprint block5 = FingerprintIop();
	Cores[0].Voices[0].SBuffer = pcm_cache_data[6].Sampledata;
	const IopFingerprint block6 = FingerprintIop();
	Cores[0].Voices[0].SBuffer = pcm_cache_data[5].Sampledata;
	const IopFingerprint block5_again = FingerprintIop();
	Cores[0].Voices[0].SBuffer = saved;

	EXPECT_NE(null_buffer.spu2, block5.spu2);
	EXPECT_NE(block5.spu2, block6.spu2);
	EXPECT_EQ(block5.spu2, block5_again.spu2);
}

TEST(IopFingerprint, SifHashCoversAllThreeChannelsIncludingSif2)
{
	const IopFingerprint base = FingerprintIop();

	const IopFingerprint s0 = WithPoke([] { sif0.fifo.size ^= 1; }, [] { sif0.fifo.size ^= 1; });
	const IopFingerprint s1 = WithPoke([] { sif1.iop.counter ^= 1; }, [] { sif1.iop.counter ^= 1; });
	const IopFingerprint s2 = WithPoke([] { sif2.iop.cycles ^= 1; }, [] { sif2.iop.cycles ^= 1; });
	const IopFingerprint ee = WithPoke([] { sif1.ee.busy = !sif1.ee.busy; }, [] { sif1.ee.busy = !sif1.ee.busy; });
	const IopFingerprint tag = WithPoke([] { sif0.iop.data.tag_hi._u32 ^= 1; }, [] { sif0.iop.data.tag_hi._u32 ^= 1; });

	for (const IopFingerprint* fp : {&s0, &s1, &s2, &ee, &tag})
	{
		EXPECT_NE(base.sif, fp->sif);
		EXPECT_EQ(base.spu2, fp->spu2);
		EXPECT_EQ(base.sio, fp->sio);
		EXPECT_EQ(base.cdvd, fp->cdvd);
	}
	EXPECT_NE(s0.sif, s1.sif);
	EXPECT_NE(s1.sif, s2.sif);
}

TEST(IopFingerprint, SioHashMovesWithSio0AndSio2)
{
	const IopFingerprint base = FingerprintIop();

	const IopFingerprint sio0 = WithPoke([] { g_Sio0.sioCommand ^= 1; }, [] { g_Sio0.sioCommand ^= 1; });
	EXPECT_NE(base.sio, sio0.sio);
	EXPECT_EQ(base.spu2, sio0.spu2);
	EXPECT_EQ(base.cdvd, sio0.cdvd);

	const IopFingerprint sio2 = WithPoke([] { g_Sio2FifoIn.push_back(0x42); }, [] { g_Sio2FifoIn.pop_back(); });
	EXPECT_NE(base.sio, sio2.sio);
	EXPECT_NE(sio0.sio, sio2.sio);

	EXPECT_TRUE(Equal(base, FingerprintIop())) << "the pokes were not undone";
}

TEST(IopFingerprint, CdvdHashMovesWithTheCdvdStruct)
{
	const IopFingerprint base = FingerprintIop();

	const IopFingerprint status = WithPoke([] { cdvd.Status ^= 1; }, [] { cdvd.Status ^= 1; });
	EXPECT_NE(base.cdvd, status.cdvd);
	EXPECT_EQ(base.spu2, status.spu2);
	EXPECT_EQ(base.sif, status.sif);
	EXPECT_EQ(base.sio, status.sio);

	// Deep in the struct (the 64 KB MECHACON buffer), and the reading flag the loading branch of
	// cdvdFreeze would act on: the hash must not trigger that branch.
	const IopFingerprint buffer = WithPoke([] { cdvd.mg_buffer[40000] ^= 1; }, [] { cdvd.mg_buffer[40000] ^= 1; });
	EXPECT_NE(base.cdvd, buffer.cdvd);

	const int reading = cdvd.Reading;
	const int read_err = cdvd.ReadErr;
	cdvd.Reading = 1;
	FingerprintIop();
	EXPECT_EQ(read_err, cdvd.ReadErr) << "the fingerprint ran cdvdFreeze's loading branch";
	cdvd.Reading = reading;
}

#endif // PCSX2_RECOMPILER_TESTS
