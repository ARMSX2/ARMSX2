// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The SPU2 mixer must give the same answers whatever is done to make it faster: the same output samples, and the same
// SPU2 state afterwards, which includes what the guest can see (ENDX, the IRQ flags, the envelope and volume registers,
// the capture areas the mixer writes into SPU2 RAM) and what a savestate carries (the voices' decode FIFOs, their positions
// in the ADPCM stream, the decoded-block cache).
//
// spu2_mixer_reference.inc is the mixer as it was before it was optimized, compiled as isa_reference. spu2_mixer_current.cpp
// is pcsx2/SPU2/Mixer.cpp compiled as isa_current. Each test builds a random SPU2 (random voices in every envelope phase,
// random pitches and loop flags, IRQ addresses that the voices walk into, reverb settings, volumes and gates, ADPCM data),
// runs both mixers from the same snapshot for a few dozen output samples, and compares after every sample.

#include "Common.h"
#include "Config.h"
#include "IopHw.h"
#include "IopMem.h"
#include "SPU2/defs.h"
#include "SPU2/spu2.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

void ReferenceSpu2Mix();
void CurrentSpu2Mix();

namespace
{
	std::vector<StereoOut32>* s_capture = nullptr;
}

// The mixers hand each finished sample to these instead of the audio stream.
void Spu2TestOutputReference(StereoOut32 out)
{
	s_capture->push_back(out);
}

void Spu2TestOutputCurrent(StereoOut32 out)
{
	s_capture->push_back(out);
}

namespace
{
	struct Rng
	{
		u64 s;
		explicit Rng(u64 seed)
			: s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull)
		{
			for (int i = 0; i < 4; i++)
				Next();
		}
		u64 Next()
		{
			s ^= s << 13;
			s ^= s >> 7;
			s ^= s << 17;
			return s;
		}
		u32 U32() { return static_cast<u32>(Next() >> 16); }
		// [0, n)
		u32 Below(u32 n) { return static_cast<u32>((Next() >> 20) % n); }
		bool Chance(u32 percent) { return Below(100) < percent; }
	};

	struct Snapshot
	{
		std::unique_ptr<u8[]> cores = std::make_unique<u8[]>(sizeof(Cores));
		std::unique_ptr<s16[]> regs = std::make_unique<s16[]>(sizeof(spu2regs) / sizeof(s16));
		std::unique_ptr<s16[]> mem = std::make_unique<s16[]>(sizeof(_spu2mem) / sizeof(s16));
		std::unique_ptr<PcmCacheEntry[]> cache = std::make_unique<PcmCacheEntry[]>(pcm_BlockCount);
		V_SPDIF spdif;
		u16 outpos, inputpos;
		u32 cycles;
		int playmode;

		void Take()
		{
			std::memcpy(cores.get(), Cores, sizeof(Cores));
			std::memcpy(regs.get(), spu2regs, sizeof(spu2regs));
			std::memcpy(mem.get(), _spu2mem, sizeof(_spu2mem));
			std::memcpy(cache.get(), pcm_cache_data, sizeof(PcmCacheEntry) * pcm_BlockCount);
			spdif = Spdif;
			outpos = OutPos;
			inputpos = InputPos;
			cycles = Cycles;
			playmode = PlayMode;
		}

		void Restore() const
		{
			std::memcpy(Cores, cores.get(), sizeof(Cores));
			std::memcpy(spu2regs, regs.get(), sizeof(spu2regs));
			std::memcpy(_spu2mem, mem.get(), sizeof(_spu2mem));
			std::memcpy(pcm_cache_data, cache.get(), sizeof(PcmCacheEntry) * pcm_BlockCount);
			Spdif = spdif;
			OutPos = outpos;
			InputPos = inputpos;
			Cycles = cycles;
			PlayMode = playmode;
		}
	};

	u64 Hash(const void* data, size_t size, u64 h = 0xcbf29ce484222325ull)
	{
		const u8* p = static_cast<const u8*>(data);
		size_t i = 0;
		for (; i + 8 <= size; i += 8)
		{
			u64 v;
			std::memcpy(&v, p + i, 8);
			h = (h ^ v) * 0x9E3779B97F4A7C15ull;
			h ^= h >> 29;
		}
		for (; i < size; i++)
			h = (h ^ p[i]) * 0x100000001b3ull;
		return h;
	}

	// Everything that can differ after one mixed sample, cheap enough to take after every sample. The decoded-block
	// cache and the whole of SPU2 RAM are compared once at the end instead.
	struct Step
	{
		StereoOut32 out;
		u64 cores;
		u64 low_ram; // the dynamic range: where the mixer writes its captures and the reverb works
		u16 outpos;
		u16 spdif_info;
		u32 istat;
	};

	// What the random states actually exercised, summed over the reference runs; printed at the end so that a change to the
	// generator that stops covering something shows up.
	struct Coverage
	{
		u64 samples = 0, nonzero_out = 0, clipped_out = 0, irq_samples = 0, endx_sets = 0, voices_stopped = 0, voices_started_by_loop = 0;
		u64 phase_samples[5] = {};
		u64 slide_voices = 0, modulated_voices = 0, noise_voices = 0, cache_hits_possible = 0;
		void Print() const
		{
			std::printf("SPU2 mixer test coverage: samples %llu, nonzero output %llu, clipped %llu, IRQ raised in %llu, ENDX bits set %llu, "
						"voices stopped by the mixer %llu | voice-samples by phase: stopped %llu attack %llu decay %llu sustain %llu release %llu | "
						"slide voice-samples %llu, modulated %llu, noise %llu\n",
				(unsigned long long)samples, (unsigned long long)nonzero_out, (unsigned long long)clipped_out, (unsigned long long)irq_samples,
				(unsigned long long)endx_sets, (unsigned long long)voices_stopped, (unsigned long long)phase_samples[0],
				(unsigned long long)phase_samples[1], (unsigned long long)phase_samples[2], (unsigned long long)phase_samples[3],
				(unsigned long long)phase_samples[4], (unsigned long long)slide_voices, (unsigned long long)modulated_voices,
				(unsigned long long)noise_voices);
		}
	};
	Coverage s_cov;

	void RandomVolumeSlide(Rng& r, V_VolumeSlide& v)
	{
		v.Reg_VOL = static_cast<u16>(r.U32());
		if (!r.Chance(12))
			v.Reg_VOL &= 0x7fff; // slides are rare in real use, keep them rare
		v.Counter = r.Below(0x9000);
		v.Value = static_cast<s16>(r.U32());
	}

	void RandomState(Rng& r)
	{
		Cores[0].Init(0);
		Cores[1].Init(1);

		// ADPCM data, headers with a plausible spread of loop flags.
		for (size_t i = 0; i < 0x100000; i++)
			_spu2mem[i] = static_cast<s16>(r.U32());
		for (size_t blk = 0; blk < 0x100000; blk += 8)
		{
			u32 flags;
			const u32 pick = r.Below(100);
			if (pick < 70)
				flags = 0;
			else if (pick < 80)
				flags = 2; // LOOP
			else if (pick < 90)
				flags = 3; // LOOP | END
			else if (pick < 95)
				flags = 4; // LOOP_START
			else
				flags = 1; // END alone: the voice mutes
			u16 header = static_cast<u16>(_spu2mem[blk]);
			header = static_cast<u16>((header & 0xf8ff) | (flags << 8));
			_spu2mem[blk] = static_cast<s16>(header);
		}
		std::memset(pcm_cache_data, 0, sizeof(PcmCacheEntry) * pcm_BlockCount);
		// Some cache entries valid and matching what a voice carries, so the cache-hit path runs.
		for (int i = 0; i < 4000; i++)
		{
			PcmCacheEntry& e = pcm_cache_data[r.Below(pcm_BlockCount)];
			e.Validated = true;
			e.Prev1 = static_cast<s16>(r.U32());
			e.Prev2 = static_cast<s16>(r.U32());
			for (s16& s : e.Sampledata)
				s = static_cast<s16>(r.U32());
		}

		static const u16 s_pitches[] = {0, 0, 0x200, 0x800, 0x1000, 0x1000, 0x1400, 0x2000, 0x3fff, 0x4000, 0xffff};

		for (V_Core& c : Cores)
		{
			c.IRQEnable = r.Chance(30);
			c.FxEnable = r.Chance(85);
			c.Mute = r.Chance(10);
			c.NoiseClk = static_cast<u8>(r.Below(64));
			c.NoiseCnt = r.U32() & 0x3fffff;
			c.NoiseOut = r.U32();
			RandomVolumeSlide(r, c.MasterVol.Left);
			RandomVolumeSlide(r, c.MasterVol.Right);
			c.ExtVol = V_VolumeLR(static_cast<s16>(r.U32()));
			c.InpVol = V_VolumeLR(static_cast<s16>(r.U32()));
			c.FxVol = V_VolumeLR(static_cast<s16>(r.U32()));
			c.Regs.ENDX = r.U32() & 0xffffff;

			for (V_VoiceGates& g : c.VoiceGates)
			{
				g.DryL = r.Chance(85) ? -1 : 0;
				g.DryR = r.Chance(85) ? -1 : 0;
				g.WetL = r.Chance(60) ? -1 : 0;
				g.WetR = r.Chance(60) ? -1 : 0;
			}
			for (V_CoreGates* g : {&c.DryGate, &c.WetGate})
			{
				g->InpL = r.Chance(70) ? -1 : 0;
				g->InpR = r.Chance(70) ? -1 : 0;
				g->SndL = r.Chance(85) ? -1 : 0;
				g->SndR = r.Chance(85) ? -1 : 0;
				g->ExtL = r.Chance(70) ? -1 : 0;
				g->ExtR = r.Chance(70) ? -1 : 0;
			}

			// Reverb: an effects area inside RAM, working addresses inside it.
			if (r.Chance(80))
			{
				c.EffectsStartA = 0x20000 + (r.Below(0x100) << 8);
				c.EffectsEndA = c.EffectsStartA + 0x400 + r.Below(0x30000);
			}
			else
			{
				c.EffectsStartA = r.Below(0x40000);
				c.EffectsEndA = r.Below(0x40000);
			}
			const u32 span = (c.EffectsEndA > c.EffectsStartA) ? (c.EffectsEndA - c.EffectsStartA) : 0x1000;
			auto addr = [&]() { return r.Below(span ? span : 1); };
			V_Reverb& rv = c.Revb;
			rv.IN_COEF_L = static_cast<s16>(r.U32());
			rv.IN_COEF_R = static_cast<s16>(r.U32());
			rv.APF1_SIZE = r.Below(0x400);
			rv.APF2_SIZE = r.Below(0x400);
			rv.APF1_VOL = static_cast<s16>(r.U32());
			rv.APF2_VOL = static_cast<s16>(r.U32());
			rv.SAME_L_SRC = addr();
			rv.SAME_R_SRC = addr();
			rv.DIFF_L_SRC = addr();
			rv.DIFF_R_SRC = addr();
			// A destination of 0 makes the "previous" address (dst - 1) negative: the indexer's unsigned wrap.
			rv.SAME_L_DST = r.Chance(10) ? 0 : addr();
			rv.SAME_R_DST = r.Chance(10) ? 0 : addr();
			rv.DIFF_L_DST = r.Chance(10) ? 0 : addr();
			rv.DIFF_R_DST = r.Chance(10) ? 0 : addr();
			rv.IIR_VOL = static_cast<s16>(r.U32());
			rv.WALL_VOL = static_cast<s16>(r.U32());
			rv.COMB1_L_SRC = addr();
			rv.COMB1_R_SRC = addr();
			rv.COMB2_L_SRC = addr();
			rv.COMB2_R_SRC = addr();
			rv.COMB3_L_SRC = addr();
			rv.COMB3_R_SRC = addr();
			rv.COMB4_L_SRC = addr();
			rv.COMB4_R_SRC = addr();
			rv.COMB1_VOL = static_cast<s16>(r.U32());
			rv.COMB2_VOL = static_cast<s16>(r.U32());
			rv.COMB3_VOL = static_cast<s16>(r.U32());
			rv.COMB4_VOL = static_cast<s16>(r.U32());
			rv.APF1_L_DST = r.Chance(10) ? 0 : addr();
			rv.APF1_R_DST = r.Chance(10) ? 0 : addr();
			rv.APF2_L_DST = r.Chance(10) ? 0 : addr();
			rv.APF2_R_DST = r.Chance(10) ? 0 : addr();
			for (auto& ch : c.RevbDownBuf)
				for (s16& s : ch)
					s = static_cast<s16>(r.Below(40000) - 20000);
			for (auto& ch : c.RevbUpBuf)
				for (s16& s : ch)
					s = static_cast<s16>(r.Below(40000) - 20000);
			c.RevbSampleBufPos = r.Below(64);

			for (u32 vi = 0; vi < V_Core::NumVoices; vi++)
			{
				V_Voice& vc = c.Voices[vi];
				RandomVolumeSlide(r, vc.Volume.Left);
				RandomVolumeSlide(r, vc.Volume.Right);

				vc.ADSR.reg32 = r.U32();
				vc.ADSR.UpdateCache();
				vc.ADSR.Counter = r.Below(0x8000);
				vc.ADSR.Value = r.Below(0x8000);
				const u32 pick = r.Below(100);
				vc.ADSR.Phase = pick < 40 ? V_ADSR::PHASE_STOPPED : pick < 52 ? V_ADSR::PHASE_ATTACK : pick < 62 ? V_ADSR::PHASE_DECAY : pick < 82 ? V_ADSR::PHASE_SUSTAIN : V_ADSR::PHASE_RELEASE;
				if (r.Chance(35))
				{
					// The values the envelope's comparisons and clamps turn on, and a counter about to overflow.
					const V_ADSR::CachedADSR& p = vc.ADSR.CachedPhases[std::min<u32>(vc.ADSR.Phase, V_ADSR::ADSR_PHASES - 1)];
					const s32 edge[] = {0, 1, 0x5fff, 0x6000, 0x6001, 0x7ffe, 0x7fff, p.Target - 1, p.Target, p.Target + 1};
					vc.ADSR.Value = std::clamp(edge[r.Below(sizeof(edge) / sizeof(edge[0]))], 0, 0x7fff);
					const u32 counters[] = {0, 1, 0x7fff, 0x7ffe, 0x7f00, 0x7ff0};
					vc.ADSR.Counter = counters[r.Below(sizeof(counters) / sizeof(counters[0]))];
				}
				if (vc.ADSR.Phase == V_ADSR::PHASE_STOPPED)
					vc.ADSR.Value = 0;

				vc.Pitch = s_pitches[r.Below(sizeof(s_pitches) / sizeof(s_pitches[0]))];
				vc.Modulated = r.Chance(10);
				vc.Noise = r.Chance(5);
				vc.LoopMode = static_cast<s8>(r.Chance(15) ? 1 : 0);
				vc.LoopFlags = static_cast<s8>(r.U32());

				// Positions: mostly in the main RAM, some in the dynamic range the mixer itself writes, some at the very end.
				u32 base;
				const u32 where = r.Below(100);
				if (where < 8)
					base = r.Below(0x2800);
				else if (where < 14)
					base = 0xFFFF8 - (r.Below(4) << 3);
				else
					base = 0x2800 + r.Below(0x100000 - 0x2800);
				base &= 0xFFFF8;
				vc.StartA = base;
				vc.LoopStartA = (r.Chance(50) ? base : (r.Below(0x100000) & 0xFFFF8));
				vc.NextA = base + 1 + r.Below(7);
				vc.Prev1 = static_cast<s16>(r.U32());
				vc.Prev2 = static_cast<s16>(r.U32());
				vc.SP = r.Below(0x1000);
				vc.OutX = static_cast<s16>(r.U32());
				vc.SBuffer = r.Chance(70) ? pcm_cache_data[vc.NextA / pcm_WordsPerBlock].Sampledata : nullptr;
				for (s32& f : vc.DecodeFifo)
					f = static_cast<s16>(r.U32());
				vc.DecPosWrite = r.Below(250) * 4;
				const s32 ahead = static_cast<s32>(r.Below(46)) - 4;
				vc.DecPosRead = vc.DecPosWrite - ahead;
			}

			// IRQ: an address one of the voices is about to read, sometimes the capture areas, sometimes anywhere.
			const u32 which = r.Below(100);
			if (which < 45)
			{
				const V_Voice& v = Cores[r.Below(2)].Voices[r.Below(V_Core::NumVoices)];
				c.IRQA = (v.NextA + r.Below(3)) & 0xFFFFF;
				if (r.Chance(50))
					c.IRQA &= 0xFFFF8;
			}
			else if (which < 60)
				c.IRQA = (0x400 + (r.Below(4) << 9) + r.Below(0x200)) & 0xFFFFF;
			else if (which < 70)
				c.IRQA = (0x2000 + (r.Below(2) << 10) + r.Below(0x200)) & 0xFFFFF;
			else
				c.IRQA = r.Below(0x100000);
		}

		// Every fourth state is loud: full-scale volumes and open gates, so that the 16-bit saturation of the voice sums, the
		// core outputs and the reverb is reached.
		if ((r.Next() & 3) == 0)
		{
			for (V_Core& c : Cores)
			{
				c.MasterVol.Left.Reg_VOL = c.MasterVol.Right.Reg_VOL = 0x3fff;
				c.MasterVol.Left.Value = c.MasterVol.Right.Value = 0x7fff;
				c.FxVol = V_VolumeLR(0x7fff);
				for (V_VoiceGates& g : c.VoiceGates)
					g.DryL = g.DryR = g.WetL = g.WetR = -1;
				for (V_Voice& vc : c.Voices)
				{
					vc.Volume.Left.Reg_VOL = vc.Volume.Right.Reg_VOL = 0x3fff;
					vc.Volume.Left.Value = vc.Volume.Right.Value = r.Chance(50) ? 0x7fff : -0x8000;
					if (vc.ADSR.Phase != V_ADSR::PHASE_STOPPED)
						vc.ADSR.Value = 0x7000 + r.Below(0xfff);
				}
			}
		}

		const u32 pm = r.Below(100);
		PlayMode = pm < 80 ? 0 : pm < 85 ? 1 : pm < 90 ? 2 : pm < 95 ? 4 : 8;
		OutPos = static_cast<u16>(r.Below(0x200));
		InputPos = 0;
		Cycles = r.Chance(30) ? (0xFFFFFFF0u - r.Below(64)) : r.U32();
		Spdif = {};
	}

	template <typename Fn>
	std::vector<Step> Run(const Snapshot& snap, Fn mix, u32 samples, std::vector<StereoOut32>& out, bool track)
	{
		snap.Restore();
		out.clear();
		s_capture = &out;
		std::vector<Step> steps;
		steps.reserve(samples);
		for (u32 i = 0; i < samples; i++)
		{
			// What TimeUpdate does around a sample, and the guest acknowledging the IRQ between samples so that a voice
			// that is still on the IRQ address raises it again.
			Cycles++;
			Spdif.Info = 0;
			psxHu32(HW_ISTAT) = 0;
			u8 phase_before[2][V_Core::NumVoices];
			u32 endx_before[2];
			for (int c = 0; c < 2; c++)
			{
				endx_before[c] = Cores[c].Regs.ENDX;
				for (u32 v = 0; v < V_Core::NumVoices; v++)
					phase_before[c][v] = Cores[c].Voices[v].ADSR.Phase;
			}
			mix();
			if (track)
			{
				s_cov.samples++;
				if (out.back().Left || out.back().Right)
					s_cov.nonzero_out++;
				if (std::abs(out.back().Left) >= 32767 || std::abs(out.back().Right) >= 32767)
					s_cov.clipped_out++;
				if (Spdif.Info & 0xc)
					s_cov.irq_samples++;
				for (int c = 0; c < 2; c++)
				{
					s_cov.endx_sets += __builtin_popcount(Cores[c].Regs.ENDX & ~endx_before[c]);
					for (u32 v = 0; v < V_Core::NumVoices; v++)
					{
						const V_Voice& vc = Cores[c].Voices[v];
						s_cov.phase_samples[std::min<u32>(phase_before[c][v], 4)]++;
						if (phase_before[c][v] != V_ADSR::PHASE_STOPPED && vc.ADSR.Phase == V_ADSR::PHASE_STOPPED)
							s_cov.voices_stopped++;
						if (vc.Volume.Left.Enable || vc.Volume.Right.Enable)
							s_cov.slide_voices++;
						if (vc.Modulated)
							s_cov.modulated_voices++;
						if (vc.Noise)
							s_cov.noise_voices++;
					}
				}
			}
			Step st;
			st.out = out.back();
			st.cores = Hash(Cores, sizeof(Cores));
			st.low_ram = Hash(_spu2mem, 0x2800 * sizeof(s16));
			st.outpos = OutPos;
			st.spdif_info = Spdif.Info;
			st.istat = psxHu32(HW_ISTAT);
			steps.push_back(st);
		}
		s_capture = nullptr;
		return steps;
	}

	void CompareRuns(u64 seed, u32 samples)
	{
		Rng r(seed);
		RandomState(r);
		auto snap = std::make_unique<Snapshot>();
		snap->Take();

		std::vector<StereoOut32> out_ref, out_cur;
		const std::vector<Step> ref = Run(*snap, ReferenceSpu2Mix, samples, out_ref, true);
		const u64 ram_ref = Hash(_spu2mem, sizeof(_spu2mem));
		const u64 cache_ref = Hash(pcm_cache_data, sizeof(PcmCacheEntry) * pcm_BlockCount);
		const u64 regs_ref = Hash(spu2regs, sizeof(spu2regs));

		const std::vector<Step> cur = Run(*snap, CurrentSpu2Mix, samples, out_cur, false);
		const u64 ram_cur = Hash(_spu2mem, sizeof(_spu2mem));
		const u64 cache_cur = Hash(pcm_cache_data, sizeof(PcmCacheEntry) * pcm_BlockCount);
		const u64 regs_cur = Hash(spu2regs, sizeof(spu2regs));

		ASSERT_EQ(ref.size(), cur.size());
		for (size_t i = 0; i < ref.size(); i++)
		{
			ASSERT_EQ(ref[i].out.Left, cur[i].out.Left) << "seed " << seed << " sample " << i;
			ASSERT_EQ(ref[i].out.Right, cur[i].out.Right) << "seed " << seed << " sample " << i;
			ASSERT_EQ(ref[i].cores, cur[i].cores) << "SPU2 core state differs: seed " << seed << " sample " << i;
			ASSERT_EQ(ref[i].low_ram, cur[i].low_ram) << "SPU2 RAM below 0x2800 differs: seed " << seed << " sample " << i;
			ASSERT_EQ(ref[i].outpos, cur[i].outpos) << "seed " << seed << " sample " << i;
			ASSERT_EQ(ref[i].spdif_info, cur[i].spdif_info) << "IRQ flags differ: seed " << seed << " sample " << i;
			ASSERT_EQ(ref[i].istat, cur[i].istat) << "IOP interrupt status differs: seed " << seed << " sample " << i;
		}
		ASSERT_EQ(ram_ref, ram_cur) << "SPU2 RAM differs: seed " << seed;
		ASSERT_EQ(cache_ref, cache_cur) << "decoded-block cache differs: seed " << seed;
		ASSERT_EQ(regs_ref, regs_cur) << "seed " << seed;
	}
} // namespace

class Spu2Mixer : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// The IOP's hardware register page is a static array; it only needs to be zero so that an SPU2 IRQ sets its bit and
		// iopTestIntc() finds interrupts disabled.
		std::memset(iopHw, 0, Ps2MemSize::IopHardware);
		ReverbDownsample = isa_native::ReverbDownsample;
		ReverbUpsample = isa_native::ReverbUpsample;
		EmuConfig.SPU2.LightweightMode = false;
	}
};

TEST_F(Spu2Mixer, CurrentMixerMatchesTheFrozenOneOnRandomStates)
{
	for (u64 seed = 1; seed <= 150; seed++)
	{
		CompareRuns(seed, 96);
		if (HasFatalFailure())
			return;
	}
	s_cov.Print();
}

TEST_F(Spu2Mixer, CurrentMixerMatchesWithTheLightweightReverbBypass)
{
	EmuConfig.SPU2.LightweightMode = true;
	for (u64 seed = 1000; seed <= 1030; seed++)
	{
		CompareRuns(seed, 64);
		if (HasFatalFailure())
			break;
	}
	EmuConfig.SPU2.LightweightMode = false;
}
