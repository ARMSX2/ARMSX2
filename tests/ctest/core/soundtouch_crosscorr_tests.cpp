// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// SoundTouch's cross-correlation against a double-precision reference, with a float tolerance.

#include "TDStretch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace
{
	class Probe final : public soundtouch::TDStretch
	{
	public:
		Probe(int ch, int overlap)
		{
			channels = ch;
			overlapLength = overlap;
		}

		double Corr(const float* mixing, const float* compare, double& norm) { return calcCrossCorr(mixing, compare, norm); }
		double CorrAccumulate(const float* mixing, const float* compare, double& norm)
		{
			return calcCrossCorrAccumulate(mixing, compare, norm);
		}
	};

	// What both functions compute over the first (channels * overlap) & -8 samples.
	void Reference(const float* mixing, const float* compare, int length, double& corr, double& norm)
	{
		corr = norm = 0.0;
		for (int i = 0; i < length; i++)
		{
			corr += static_cast<double>(mixing[i]) * compare[i];
			norm += static_cast<double>(mixing[i]) * mixing[i];
		}
	}

	std::vector<float> Noise(size_t n, unsigned seed)
	{
		std::mt19937 rng(seed);
		std::uniform_real_distribution<float> d(-1.0f, 1.0f);
		std::vector<float> v(n);
		for (float& s : v)
			s = d(rng);
		return v;
	}

	TEST(SoundTouchCrossCorr, MatchesADoubleReferenceForMonoAndStereoAndOddOverlaps)
	{
		for (const int ch : {1, 2})
		{
			for (const int overlap : {8, 11, 64, 177, 441, 882})
			{
				Probe p(ch, overlap);
				const int length = (ch * overlap) & -8;
				const std::vector<float> mixing = Noise(length + 64, 1000 + overlap * ch);
				const std::vector<float> compare = Noise(length + 64, 2000 + overlap * ch);

				double norm = 0.0;
				const double got = p.Corr(mixing.data(), compare.data(), norm);
				double ref_corr, ref_norm;
				Reference(mixing.data(), compare.data(), length, ref_corr, ref_norm);
				const double want = ref_corr / std::sqrt(ref_norm < 1e-9 ? 1.0 : ref_norm);

				EXPECT_NEAR(norm, ref_norm, 1e-4 * std::max(1.0, ref_norm)) << "ch " << ch << " overlap " << overlap;
				EXPECT_NEAR(got, want, 1e-4) << "ch " << ch << " overlap " << overlap;
			}
		}
	}

	// The full seek calls calcCrossCorr at the first offset and calcCrossCorrAccumulate at each next one, which drops the
	// samples that left the window from `norm` and adds the ones that entered it.
	TEST(SoundTouchCrossCorr, TheAccumulatingFormFollowsTheWindowAcrossASeek)
	{
		const int ch = 2;
		const int overlap = 441;
		Probe p(ch, overlap);
		const int length = (ch * overlap) & -8;
		const std::vector<float> ref = Noise(length + 2 * 200 + 64, 7);
		const std::vector<float> mid = Noise(length + 64, 8);

		double norm = 0.0;
		p.Corr(ref.data(), mid.data(), norm);
		for (int step = 1; step < 200; step++)
		{
			const float* pos = ref.data() + ch * step;
			const double got = p.CorrAccumulate(pos, mid.data(), norm);
			double ref_corr, ref_norm;
			Reference(pos, mid.data(), length, ref_corr, ref_norm);
			ASSERT_NEAR(norm, ref_norm, 1e-3 * std::max(1.0, ref_norm)) << "step " << step;
			ASSERT_NEAR(got, ref_corr / std::sqrt(ref_norm), 1e-3) << "step " << step;
		}
	}
} // namespace
