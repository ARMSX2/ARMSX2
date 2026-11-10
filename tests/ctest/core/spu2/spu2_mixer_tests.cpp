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
#if defined(__aarch64__) || defined(_M_ARM64)
#include "SPU2/spu2_neon.h"
#endif

#include <gtest/gtest.h>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#define SPU2_TEST_HAS_PMU 1
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

void ReferenceSpu2Mix();
void CurrentSpu2Mix();
s32 ReferenceReverbDownsample(V_Core& core, bool right);
StereoOut32 ReferenceReverbUpsample(V_Core& core);

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
			// Reverb filter taps: usually well inside the 16-bit range, and in one state in four the whole range, so that the
			// resamplers' saturating adds and the reverb's own clamps are reached.
			const bool full_range = r.Chance(25);
			for (auto& ch : c.RevbDownBuf)
				for (s16& s : ch)
					s = full_range ? static_cast<s16>(r.U32()) : static_cast<s16>(r.Below(40000) - 20000);
			for (auto& ch : c.RevbUpBuf)
				for (s16& s : ch)
					s = full_range ? static_cast<s16>(r.U32()) : static_cast<s16>(r.Below(40000) - 20000);
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
				if (pick < 40)
					vc.ADSR.Phase = V_ADSR::PHASE_STOPPED;
				else if (pick < 52)
					vc.ADSR.Phase = V_ADSR::PHASE_ATTACK;
				else if (pick < 62)
					vc.ADSR.Phase = V_ADSR::PHASE_DECAY;
				else if (pick < 82)
					vc.ADSR.Phase = V_ADSR::PHASE_SUSTAIN;
				else
					vc.ADSR.Phase = V_ADSR::PHASE_RELEASE;
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
		if (pm < 80)
			PlayMode = 0;
		else if (pm < 85)
			PlayMode = 1;
		else if (pm < 90)
			PlayMode = 2;
		else if (pm < 95)
			PlayMode = 4;
		else
			PlayMode = 8;
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

TEST_F(Spu2Mixer, ReverbResamplersMatchTheFrozenOnesAtEveryBufferPosition)
{
	// The 39-tap filters, from the buffer contents at every write position, with taps drawn from the whole 16-bit range
	// (so the saturating adds are hit), from the extremes only, and from small values.
	Rng r(77);
	V_Core& core = Cores[0];
	for (int round = 0; round < 3000; round++)
	{
		const u32 mode = round % 3;
		auto tap = [&]() -> s16 {
			if (mode == 0)
				return static_cast<s16>(r.U32());
			if (mode == 1)
				return r.Chance(50) ? INT16_MAX : INT16_MIN;
			return static_cast<s16>(static_cast<s32>(r.Below(2001)) - 1000);
		};
		for (auto& ch : core.RevbDownBuf)
			for (s16& s : ch)
				s = tap();
		for (auto& ch : core.RevbUpBuf)
			for (s16& s : ch)
				s = tap();
		for (u32 pos = 0; pos < 64; pos++)
		{
			core.RevbSampleBufPos = pos;
			for (const bool right : {false, true})
				ASSERT_EQ(ReferenceReverbDownsample(core, right), ReverbDownsample(core, right)) << "round " << round << " pos " << pos;
			const StereoOut32 ref = ReferenceReverbUpsample(core);
			const StereoOut32 cur = ReverbUpsample(core);
			ASSERT_EQ(ref.Left, cur.Left) << "round " << round << " pos " << pos;
			ASSERT_EQ(ref.Right, cur.Right) << "round " << round << " pos " << pos;
		}
	}
}

#if defined(__aarch64__) || defined(_M_ARM64)
// The opt-in NEON resamplers ("SPU2/NeonReverbSIMD", SPU2::RegisterNEONBackend) against the same frozen ones, from the
// same three kinds of buffer contents.
TEST_F(Spu2Mixer, TheOptInNeonReverbResamplersMatchTheFrozenOnes)
{
	struct Restore
	{
		StereoOut32 (*up)(V_Core&) = ReverbUpsample;
		s32 (*down)(V_Core&, bool) = ReverbDownsample;
		~Restore()
		{
			ReverbUpsample = up;
			ReverbDownsample = down;
		}
	} restore;
	SPU2::RegisterNEONBackend();

	Rng r(78);
	V_Core& core = Cores[0];
	for (int round = 0; round < 1500; round++)
	{
		const u32 mode = round % 3;
		auto tap = [&]() -> s16 {
			if (mode == 0)
				return static_cast<s16>(r.U32());
			if (mode == 1)
				return r.Chance(50) ? INT16_MAX : INT16_MIN;
			return static_cast<s16>(static_cast<s32>(r.Below(2001)) - 1000);
		};
		for (auto& ch : core.RevbDownBuf)
			for (s16& s : ch)
				s = tap();
		for (auto& ch : core.RevbUpBuf)
			for (s16& s : ch)
				s = tap();
		for (u32 pos = 0; pos < 64; pos++)
		{
			core.RevbSampleBufPos = pos;
			for (const bool right : {false, true})
				ASSERT_EQ(ReferenceReverbDownsample(core, right), ReverbDownsample(core, right)) << "round " << round << " pos " << pos;
			const StereoOut32 ref = ReferenceReverbUpsample(core);
			const StereoOut32 cur = ReverbUpsample(core);
			ASSERT_EQ(ref.Left, cur.Left) << "round " << round << " pos " << pos;
			ASSERT_EQ(ref.Right, cur.Right) << "round " << round << " pos " << pos;
		}
	}
}
#endif

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

// In bypass mode (PlayMode bit 1) core 0's input does not go through the SPU2, and the mixers add silence for it. V_Core::ReadInput
// used to return its result uninitialized on that path, so the mixers added whatever the caller's registers held. That is why a
// random state with PlayMode 2 compared differently from run to run, and from one build of the test to the next: the frozen mixer
// and the current one both call the same ReadInput and each read a different leftover.
TEST_F(Spu2Mixer, BypassModeInputOfCoreZeroIsSilent)
{
	Cores[0].Init(0);
	Cores[1].Init(1);
	for (u32 i = 0x2000; i < 0x2800; i++)
		_spu2mem[i] = static_cast<s16>(0x1234 + i);
	OutPos = 5; // not one of the positions where ReadInput refills the AutoDMA buffer

	PlayMode = 0;
	const StereoOut32 core0 = Cores[0].ReadInput();
	EXPECT_EQ(core0.Left, _spu2mem[0x2000 + 5]);
	EXPECT_EQ(core0.Right, _spu2mem[0x2200 + 5]);

	PlayMode = 2;
	const StereoOut32 bypassed = Cores[0].ReadInput();
	EXPECT_EQ(bypassed.Left, 0);
	EXPECT_EQ(bypassed.Right, 0);

	// Core 1 is not bypassed.
	const StereoOut32 core1 = Cores[1].ReadInput();
	EXPECT_EQ(core1.Left, _spu2mem[0x2400 + 5]);
	EXPECT_EQ(core1.Right, _spu2mem[0x2600 + 5]);
}

#ifdef SPU2_TEST_HAS_PMU

namespace
{
	// User-space instruction and cycle counters for the calling thread. On the Apple cores under Asahi the generic hardware
	// events only exist per core type, so the performance and efficiency core PMUs are both opened and whichever one the
	// thread ran on has the counts.
	class Pmu
	{
	public:
		Pmu()
		{
			static const char* const pmus[] = {"apple_avalanche_pmu", "apple_blizzard_pmu"};
			bool any = false;
			for (const char* name : pmus)
			{
				char path[128];
				std::snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type", name);
				std::FILE* f = std::fopen(path, "r");
				if (!f)
					continue;
				u32 type = 0;
				const int got = std::fscanf(f, "%u", &type);
				std::fclose(f);
				if (got == 1)
				{
					Open(type, 0x8c);
					Open(type, 0x2);
					// A generic event on one core type's PMU: the PMU's type goes in the upper half of the config.
					Open(PERF_TYPE_HARDWARE, (static_cast<u64>(type) << 32) | PERF_COUNT_HW_BRANCH_MISSES);
					any = true;
				}
			}
			if (!any)
			{
				Open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
				Open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
			}
		}

		~Pmu()
		{
			for (const Event& e : m_events)
				close(e.fd);
		}

		void Start()
		{
			for (const Event& e : m_events)
			{
				ioctl(e.fd, PERF_EVENT_IOC_RESET, 0);
				ioctl(e.fd, PERF_EVENT_IOC_ENABLE, 0);
			}
		}

		void Stop(u64* instructions, u64* cycles, u64* branch_misses)
		{
			*instructions = 0;
			*cycles = 0;
			*branch_misses = 0;
			for (const Event& e : m_events)
			{
				ioctl(e.fd, PERF_EVENT_IOC_DISABLE, 0);
				u64 v = 0;
				if (read(e.fd, &v, sizeof(v)) != sizeof(v))
					v = 0;
				if (e.kind == 0)
					*instructions += v;
				else if (e.kind == 1)
					*cycles += v;
				else
					*branch_misses += v;
			}
		}

		bool Ok() const { return !m_events.empty(); }

	private:
		struct Event
		{
			int fd;
			int kind; // 0 instructions, 1 cycles, 2 branch misses
		};

		void Open(u32 type, u64 config)
		{
			perf_event_attr attr = {};
			attr.size = sizeof(attr);
			attr.type = type;
			attr.config = config;
			attr.disabled = 1;
			attr.exclude_kernel = 1;
			attr.exclude_hv = 1;
			const int fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
			if (fd < 0)
				return;
			int kind = 1;
			if (type == PERF_TYPE_HARDWARE)
			{
				const u64 generic = config & 0xffffffff;
				if (generic == PERF_COUNT_HW_INSTRUCTIONS)
					kind = 0;
				else if (generic == PERF_COUNT_HW_BRANCH_MISSES)
					kind = 2;
			}
			else if (config == 0x8c)
				kind = 0;
			m_events.push_back({fd, kind});
		}

		std::vector<Event> m_events;
	};

	// A state shaped like a game's: some voices playing, the rest stopped but still running with a pitch set, reverb on, no
	// IRQ, no slides.
	void ShapeState(Rng& r, u32 active0, u32 active1)
	{
		RandomState(r);
		for (size_t blk = 0; blk < 0x100000; blk += 8)
		{
			u16 header = static_cast<u16>(_spu2mem[blk]);
			header = static_cast<u16>((header & 0xf8ff) | (r.Chance(5) ? (2u << 8) : 0u));
			_spu2mem[blk] = static_cast<s16>(header);
		}
		const u32 active[2] = {active0, active1};
		for (int ci = 0; ci < 2; ci++)
		{
			V_Core& c = Cores[ci];
			c.IRQEnable = false;
			c.Mute = false;
			c.FxEnable = true;
			c.EffectsStartA = 0x20000;
			c.EffectsEndA = 0x20000 + 0x8000;
			for (V_VoiceGates& g : c.VoiceGates)
				g.DryL = g.DryR = g.WetL = g.WetR = -1;
			c.MasterVol.Left.Reg_VOL = c.MasterVol.Right.Reg_VOL = 0x3fff;
			c.MasterVol.Left.Value = c.MasterVol.Right.Value = 0x3fff;
			for (u32 vi = 0; vi < V_Core::NumVoices; vi++)
			{
				V_Voice& vc = c.Voices[vi];
				vc.Volume.Left.Reg_VOL &= 0x7fff;
				vc.Volume.Right.Reg_VOL &= 0x7fff;
				vc.Modulated = false;
				vc.Noise = false;
				vc.LoopMode = 0;
				if (vi < active[ci])
				{
					vc.ADSR.Phase = V_ADSR::PHASE_SUSTAIN;
					vc.ADSR.Value = 0x2000 + r.Below(0x5000);
					vc.ADSR.reg32 = 0xdfff1fc0 & ~0x40000000u; // sustain at the current level: shift 0x1f, rate field all ones
					vc.ADSR.UpdateCache();
					vc.Volume.Left.Value = static_cast<s16>(r.Below(0x6000)) - 0x3000;
					vc.Volume.Right.Value = static_cast<s16>(r.Below(0x6000)) - 0x3000;
					vc.Pitch = static_cast<u16>(0x800 + r.Below(0x1800));
				}
				else
				{
					vc.ADSR.Phase = V_ADSR::PHASE_STOPPED;
					vc.ADSR.Value = 0;
					static const u16 idle[] = {0x800, 0x1000, 0x1000, 0x1400};
					vc.Pitch = idle[r.Below(4)];
				}
			}
		}
		PlayMode = 0;
	}

	template <typename Fn>
	void BenchMix(const char* shape, const char* impl, const Snapshot& snap, Fn mix, Pmu& pmu)
	{
		const u32 warm = 400, n = 6000;
		std::vector<StereoOut32> out;
		out.reserve(warm + n + 8);
		snap.Restore();
		s_capture = &out;
		for (u32 i = 0; i < warm; i++)
		{
			Cycles++;
			mix();
		}
		out.clear();
		u64 instr = 0, cyc = 0, bm = 0;
		pmu.Start();
		for (u32 i = 0; i < n; i++)
		{
			Cycles++;
			mix();
		}
		pmu.Stop(&instr, &cyc, &bm);
		s_capture = nullptr;
		std::printf("BENCH2 shape=%s impl=%s mixes=%u instructions_per_mix=%.1f cycles_per_mix=%.1f ipc=%.2f branch_misses_per_mix=%.1f\n", shape, impl, n,
			static_cast<double>(instr) / n, static_cast<double>(cyc) / n, cyc ? static_cast<double>(instr) / cyc : 0.0, static_cast<double>(bm) / n);
	}
} // namespace

// Instructions and cycles one spu2Mix() takes, user space only, for states with a game-like split of playing and stopped
// voices: the numbers of playing voices on core 0 and core 1 are
//   busy 20+20, mixed 15+7 (God of War II), quiet 0+2 (Metal Gear Solid 3, Dragon Quest VIII).
// Prints the frozen reference too, for scale; it is the old mixer with the three helpers it called compiled into the
// same file, not the old mixer as it was linked. Disabled by default:
//   spu2_tests --gtest_also_run_disabled_tests --gtest_filter='*DISABLED_InstructionsPerMix*'
// Pin the thread (taskset) to pick the core type.
TEST_F(Spu2Mixer, DISABLED_InstructionsPerMix)
{
	Pmu pmu;
	ASSERT_TRUE(pmu.Ok()) << "perf_event_open failed";
	struct Shape
	{
		const char* name;
		u32 a0, a1;
	};
	for (const Shape sh : {Shape{"busy", 20, 20}, Shape{"mixed", 15, 7}, Shape{"quiet", 0, 2}})
	{
		Rng r(4242);
		ShapeState(r, sh.a0, sh.a1);
		auto snap = std::make_unique<Snapshot>();
		snap->Take();
		BenchMix(sh.name, "current", *snap, CurrentSpu2Mix, pmu);
		BenchMix(sh.name, "reference", *snap, ReferenceSpu2Mix, pmu);
	}
}

// The same count with the data cache emptied between mixes, which is closer to what the IOP thread sees: it runs the IOP
// recompiler's blocks, the counters and the event tests between two mixes, and the mixer's own state (about 15 KB of voice
// structures, a line of SPU2 RAM per voice, the reverb's working addresses) is not in the first-level cache when it starts.
// Each mix is followed by reading POLLUTE bytes of unrelated memory; the cost of that reading is measured on its own and
// subtracted. Prints the difference per mix. Disabled by default:
//   spu2_tests --gtest_also_run_disabled_tests --gtest_filter='*DISABLED_ColdCache*'
TEST_F(Spu2Mixer, DISABLED_ColdCache)
{
	Pmu pmu;
	ASSERT_TRUE(pmu.Ok()) << "perf_event_open failed";
	const char* kb_env = std::getenv("SPU2_TEST_POLLUTE_KB");
	const size_t pollute = (kb_env ? std::strtoul(kb_env, nullptr, 10) : 256) * 1024;
	struct Shape
	{
		const char* name;
		u32 a0, a1;
	};
	std::vector<u8> junk(16 << 20, 1);
	for (const Shape sh : {Shape{"busy", 20, 20}, Shape{"mixed", 15, 7}, Shape{"quiet", 0, 2}})
	{
		Rng r(4242);
		ShapeState(r, sh.a0, sh.a1);
		auto snap = std::make_unique<Snapshot>();
		snap->Take();
		const u32 warm = 400, n = 4000;
		std::vector<StereoOut32> out;
		out.reserve(warm + n + 8);
		auto sweep = [&](size_t& at) {
			u32 sum = 0;
			for (size_t b = 0; b < pollute; b += 64)
				sum += junk[(at + b) % junk.size()];
			at = (at + pollute) % junk.size();
			return sum;
		};
		volatile u32 sink = 0;
		double warm_instr = 0, warm_cyc = 0, cold_instr = 0, cold_cyc = 0;
		for (int pass = 0; pass < 3; pass++)
		{
			// 0: mix only (warm), 1: pollution only, 2: mix then pollution.
			snap->Restore();
			out.clear();
			s_capture = &out;
			size_t at = 0;
			for (u32 i = 0; i < warm; i++)
			{
				Cycles++;
				if (pass != 1)
					CurrentSpu2Mix();
				if (pass != 0)
					sink = sink + sweep(at);
			}
			u64 instr = 0, cyc = 0, bm = 0;
			pmu.Start();
			for (u32 i = 0; i < n; i++)
			{
				Cycles++;
				if (pass != 1)
					CurrentSpu2Mix();
				if (pass != 0)
					sink = sink + sweep(at);
			}
			pmu.Stop(&instr, &cyc, &bm);
			s_capture = nullptr;
			static double pollute_instr, pollute_cyc;
			if (pass == 0)
			{
				warm_instr = static_cast<double>(instr) / n;
				warm_cyc = static_cast<double>(cyc) / n;
			}
			else if (pass == 1)
			{
				pollute_instr = static_cast<double>(instr) / n;
				pollute_cyc = static_cast<double>(cyc) / n;
			}
			else
			{
				cold_instr = static_cast<double>(instr) / n - pollute_instr;
				cold_cyc = static_cast<double>(cyc) / n - pollute_cyc;
			}
		}
		std::printf("COLD shape=%s pollute_kb=%zu warm: instructions_per_mix=%.0f cycles_per_mix=%.0f | cold: instructions_per_mix=%.0f cycles_per_mix=%.0f (+%.0f%%)\n", sh.name,
			pollute / 1024, warm_instr, warm_cyc, cold_instr, cold_cyc, 100.0 * (cold_cyc - warm_cyc) / warm_cyc);
	}
}

// Writes the three game-shaped states (busy, mixed, quiet) to $SPU2_TEST_SNAPSHOT_DIR/<shape>.snap, for tools/devlib_bench, which
// runs the spu2Mix of a device build of libemucore on them. The file is: a header of four u32 (magic 'SPU2', sizeof(Cores),
// pcm cache entries, sizeof(PcmCacheEntry)), the bytes of Cores with each voice's SBuffer turned into a byte offset into the
// decoded-block cache (-1 for null), SPU2 RAM (4 MB), the decoded-block cache, then Spdif (14 bytes), OutPos, InputPos (u16),
// Cycles (u32), PlayMode (s32), and last the expected result: a hash of SPU2 RAM below 0x2800 and OutPos after 100 mixes
// by the frozen reference mixer, so that the tool can tell its mixer computes the same thing.
// Disabled by default:
//   SPU2_TEST_SNAPSHOT_DIR=/path spu2_tests --gtest_also_run_disabled_tests --gtest_filter='*DISABLED_DumpSnapshots*'
TEST_F(Spu2Mixer, DISABLED_DumpSnapshots)
{
	const char* dir = std::getenv("SPU2_TEST_SNAPSHOT_DIR");
	ASSERT_NE(dir, nullptr);
	struct Shape
	{
		const char* name;
		u32 a0, a1;
	};
	for (const Shape sh : {Shape{"busy", 20, 20}, Shape{"mixed", 15, 7}, Shape{"quiet", 0, 2}})
	{
		Rng r(4242);
		ShapeState(r, sh.a0, sh.a1);
		auto snap = std::make_unique<Snapshot>();
		snap->Take();

		std::vector<StereoOut32> out;
		out.reserve(128);
		snap->Restore();
		s_capture = &out;
		for (int i = 0; i < 100; i++)
		{
			Cycles++;
			ReferenceSpu2Mix();
		}
		s_capture = nullptr;
		const u64 expect = Hash(_spu2mem, 0x2800 * sizeof(s16), Hash(&OutPos, sizeof(OutPos)));
		snap->Restore();

		std::vector<u8> cores(sizeof(Cores));
		std::memcpy(cores.data(), Cores, sizeof(Cores));
		for (int c = 0; c < 2; c++)
		{
			for (u32 v = 0; v < V_Core::NumVoices; v++)
			{
				const V_Voice& vc = Cores[c].Voices[v];
				const size_t field = reinterpret_cast<const u8*>(&vc.SBuffer) - reinterpret_cast<const u8*>(Cores);
				const s64 off = vc.SBuffer ? (reinterpret_cast<const u8*>(vc.SBuffer) - reinterpret_cast<const u8*>(pcm_cache_data)) : -1;
				std::memcpy(cores.data() + field, &off, sizeof(off));
			}
		}
		char path[512];
		std::snprintf(path, sizeof(path), "%s/%s.snap", dir, sh.name);
		std::FILE* f = std::fopen(path, "wb");
		ASSERT_NE(f, nullptr) << path;
		const u32 header[4] = {0x32555053u, static_cast<u32>(sizeof(Cores)), static_cast<u32>(pcm_BlockCount), static_cast<u32>(sizeof(PcmCacheEntry))};
		std::fwrite(header, sizeof(header), 1, f);
		std::fwrite(cores.data(), cores.size(), 1, f);
		std::fwrite(_spu2mem, sizeof(_spu2mem), 1, f);
		std::fwrite(pcm_cache_data, sizeof(PcmCacheEntry), pcm_BlockCount, f);
		std::fwrite(&Spdif, sizeof(Spdif), 1, f);
		std::fwrite(&OutPos, sizeof(OutPos), 1, f);
		std::fwrite(&InputPos, sizeof(InputPos), 1, f);
		std::fwrite(&Cycles, sizeof(Cycles), 1, f);
		std::fwrite(&PlayMode, sizeof(PlayMode), 1, f);
		std::fwrite(&expect, sizeof(expect), 1, f);
		std::fclose(f);
		std::printf("SNAPSHOT %s sizeof(Cores)=%zu sizeof(PcmCacheEntry)=%zu sizeof(V_Voice)=%zu offsetof(SBuffer)=%zu expected hash %016llx\n", path, sizeof(Cores),
			sizeof(PcmCacheEntry), sizeof(V_Voice), offsetof(V_Voice, SBuffer), static_cast<unsigned long long>(expect));
	}
}

// A driver for `valgrind --tool=cachegrind`, to count the data cache misses of one spu2Mix() under a small cache. On a device
// the IOP thread runs other code between two mixes (the IOP recompiler's blocks, the counters, the event tests), which evicts
// part of what the mixer used last time; SPU2_TEST_POLLUTE_KB is how much other memory is read between two mixes to stand in
// for it, so 0 is the mixer with a cache to itself and a value at or above the cache size is the mixer starting cold.
// Disabled by default:
//   SPU2_TEST_POLLUTE_KB=64 valgrind --tool=cachegrind --D1=32768,4,64 --LL=262144,8,64 spu2_tests --gtest_also_run_disabled_tests --gtest_filter='*DISABLED_CacheModelDriver*'
TEST_F(Spu2Mixer, DISABLED_CacheModelDriver)
{
	const char* kb_env = std::getenv("SPU2_TEST_POLLUTE_KB");
	const size_t pollute_bytes = (kb_env ? std::strtoul(kb_env, nullptr, 10) : 0) * 1024;
	const char* mixes_env = std::getenv("SPU2_TEST_MIXES");
	const u32 mixes = mixes_env ? static_cast<u32>(std::strtoul(mixes_env, nullptr, 10)) : 1500;

	Rng r(4242);
	ShapeState(r, 15, 7);
	auto snap = std::make_unique<Snapshot>();
	snap->Take();
	snap->Restore();

	std::vector<StereoOut32> out;
	out.reserve(mixes + 8);
	s_capture = &out;
	std::vector<u8> junk(8 << 20, 1);
	size_t at = 0;
	volatile u32 sink = 0;
	for (u32 i = 0; i < mixes; i++)
	{
		Cycles++;
		CurrentSpu2Mix();
		u32 sum = 0;
		for (size_t b = 0; b < pollute_bytes; b += 64)
			sum += junk[(at + b) % junk.size()];
		at = (at + pollute_bytes) % junk.size();
		sink = sink + sum;
	}
	s_capture = nullptr;
	std::printf("CACHEMODEL mixes=%u pollute_kb=%zu\n", mixes, pollute_bytes / 1024);
}

#endif // SPU2_TEST_HAS_PMU
