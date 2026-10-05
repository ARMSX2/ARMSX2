// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// RAISR 2x upscaler. The algorithm and the trained filters are from Intel's Library for Video
// Super Resolution (BSD-3-Clause, https://github.com/OpenVisualCloud/Video-Super-Resolution-Library,
// commit ea37e77): Library/Raisr_OpenCL_kernel.h is the clearest statement of it, Raisr.cpp and
// Raisr_AVX256.cpp the CPU version it was trained against. cppraisr (MIT) is the model for the
// plain-C++ structure. This is a port of the algorithm; none of Intel's AVX or IPP code is used.
//
// Differences from Intel's CPU path, all deliberate:
//  - Every pixel is filtered. Intel leaves a 6 pixel border as plain bilinear; here the luma is
//    treated as edge-replicated beyond the image, so a 32x32 texture is upscaled in full.
//  - The cheap upscale is not rounded to 8 bits before filtering, and the blended luma is not
//    rounded before it is recombined with chroma.
//  - Coherence is 0 for a flat patch (Intel adds 1e-17 to the denominator, which does the same).
//  - The hash bins are found without atan2 or square roots (see HashScalar): the same partition,
//    except exactly on a bin edge, where float rounding decides in Intel's code too.
//
// Structure: the image is processed in bands of output rows. Per band: source rows are converted
// to luma/chroma/alpha and upscaled horizontally, vertically combined into the padded luma L,
// then per row: gradient products -> separable 11 tap blur (the structure tensor) -> hash ->
// 11x11 filter -> range fallback; then a 3x3 census blend and the colour recombine. Scratch is
// per call, so the entry points can run on any number of threads.
//
// The scalar and NEON paths do the same float operations in the same order (the NEON path is the
// scalar path, four pixels wide), so they produce identical bits. Every fused multiply-add is
// explicit (MulAdd / vfmaq); the compiler must not contract anything else, so contraction is off
// for this file.

#include "GS/Renderers/HW/GSTextureUpscaler.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

#if defined(_M_ARM64) || defined(__aarch64__)
#include <arm_neon.h>
#define GSTU_NEON 1
#else
#define GSTU_NEON 0
#endif

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace GSTextureUpscaler
{
	namespace
	{
		// ---------------------------------------------------------------------------------
		//  Constants
		// ---------------------------------------------------------------------------------

		constexpr int kMargin = static_cast<int>(kPatchSize / 2); // filter window radius, 5
		constexpr u32 kPad = 8; // replicated columns left of x = 0 in every padded row
		constexpr u32 kBandRows = 64; // output rows per band
		constexpr u32 kLumaRows = kBandRows + 14; // L rows per band: 5 window + 1 gradient + 1 spare each side
		constexpr u32 kSourceRows = kBandRows / 2 + 12; // source rows a band can touch
		constexpr float kTensorNorm = 1.0f / (255.0f * 255.0f * 2.0f * 2.0f);

		// Intel resizes luma into an 8-bit buffer, so its cheap upscale L is rounded to integers
		// before it is hashed and filtered. Here L stays float. Flip this to compare the two; every
		// consumer of L reads the rows LumaRow produces, so this is the only place that changes.
		constexpr bool kRoundCheapLuma = false;

		// A fused multiply-add on every build the NEON path is compiled for (it uses vfmaq), so the
		// two paths match; elsewhere two roundings, which is cheaper than a libm call.
		inline float MulAdd(float a, float b, float c)
		{
#if GSTU_NEON || defined(__FP_FAST_FMAF)
			return std::fma(a, b, c);
#else
			return a * b + c;
#endif
		}

		inline u32 RoundUp4(u32 v) { return (v + 3) & ~3u; }

		// ---------------------------------------------------------------------------------
		//  Tables
		// ---------------------------------------------------------------------------------

		// Intel createGaussianKernel<float>(11, 2.0), Raisr.cpp:142.
		std::array<float, kPatchSize> MakeGaussian11()
		{
			constexpr int n = static_cast<int>(kPatchSize);
			constexpr int half = n / 2;
			const float scale2X = (-0.5f * 0.25f) / (2.0f * 2.0f);
			float values[half];
			float sum = 0.0f;
			for (int i = 0, x = 1 - n; i < half; i++, x += 2)
			{
				const float t = std::exp(static_cast<float>(x * x) * scale2X);
				values[i] = t;
				sum += t;
			}
			sum *= 2.0f;
			sum += 1.0f;
			const float mul1 = 1.0f / sum;

			std::array<float, kPatchSize> result;
			for (int i = 0; i < half; i++)
			{
				const float t = values[i] * mul1;
				result[i] = t;
				result[n - 1 - i] = t;
			}
			result[half] = 1.0f * mul1;
			return result;
		}

		struct Tables
		{
			float g[kMargin + 1]; // structure window taps, g[m] at distance m from the centre
			float gv[kMargin + 1]; // g[m] with the tensor normalisation folded in (vertical pass)
			// tan(k pi / 24) for k = 1..11, then infinity: the first-quadrant angle edges as slopes.
			alignas(16) float tn[kAngleBins / 2];
		};

		Tables MakeTables()
		{
			Tables t = {};
			const auto g = MakeGaussian11();
			for (int m = 0; m <= kMargin; m++)
			{
				t.g[m] = g[kMargin - m];
				t.gv[m] = t.g[m] * kTensorNorm;
			}
			const double pi = 3.14159265358979323846;
			for (int k = 1; k < static_cast<int>(kAngleBins / 2); k++)
				t.tn[k - 1] = static_cast<float>(std::tan(k * pi / kAngleBins));
			t.tn[kAngleBins / 2 - 1] = std::numeric_limits<float>::infinity();
			return t;
		}

		const Tables& GetTables()
		{
			static const Tables tables = MakeTables();
			return tables;
		}

		// ---------------------------------------------------------------------------------
		//  Hash
		// ---------------------------------------------------------------------------------

		struct HashParams
		{
			float se[2]; // strength edges
			float cr[2]; // coherence edges as L2/L1 ratios, see below
		};

		HashParams MakeHashParams(const FilterSet& f)
		{
			HashParams p;
			for (int i = 0; i < 2; i++)
			{
				p.se[i] = f.StrengthEdges()[i];
				// coherence = (sqrt(L1) - sqrt(L2)) / (sqrt(L1) + sqrt(L2)) is greater than an edge c
				// exactly when L2 < L1 * ((1 - c) / (1 + c))^2, so the bucket needs no square roots
				// or divide. The edges are validated to 0..1.
				const double c = f.CoherenceEdges()[i];
				const double ratio = (1.0 - c) / (1.0 + c);
				p.cr[i] = static_cast<float>(ratio * ratio);
			}
			return p;
		}

		// The hash of a tensor [[a b][b d]] (a = vertical-gradient energy, d = horizontal). Intel's is
		// GetHashValue in Raisr.cpp:814 for the scalar form, Raisr_AVX256.cpp:393 for the live one.
		//
		// The angle is atan2(b, L1 - d), plus pi when negative: the direction of the dominant
		// eigenvector (x, y) = (L1 - d, b), folded into [0, pi). Its bin is floor(angle / (pi/24)).
		// Here the bin is found without atan2. Fold the direction into the first quadrant: its
		// slope t = |y| / |x| says how many of the first quadrant's edges (k*pi/24, k = 1..12) it
		// has passed, c = #(t >= tan(k*pi/24)), and when the folded direction pointed into the
		// second quadrant (angle > pi/2) the bin is mirrored, 23 - c. That is the same partition
		// as atan2, except exactly on an edge, and it vectorises with one divide.
		//
		// b == 0 uses the direction (1, 0), angle 0, as Intel does (Raisr_AVX256.cpp:425-431). That
		// puts a perfectly vertical edge (all gradient horizontal, b exactly 0) in angle bin 0, the
		// same bin as a horizontal one.
		//
		// Strength is L1 and coherence is (sqrt(L1) - sqrt(L2)) / (sqrt(L1) + sqrt(L2)), tested as an
		// L2/L1 ratio (MakeHashParams); a bin index
		// is the number of edges the value is greater than (Intel's searchsorted, which puts a value
		// equal to an edge in the lower bin). L1 and L2 come from sqrt(max(0, T^2/4 - D)) and
		// max(0, T/2 - sqrt) so rounding cannot make them NaN.
		s32 HashScalar(const Tables& t, const HashParams& hp, float a, float b, float d)
		{
			const float T = a + d;
			const float D = a * d - b * b;
			const float q = std::fmax((T * T) * 0.25f - D, 0.0f);
			const float s = std::sqrt(q);
			const float half = T * 0.5f;
			const float L1 = half + s;
			const float L2 = std::fmax(half - s, 0.0f);

			const float x = (b == 0.0f) ? 1.0f : L1 - d;
			const float slope = std::fabs(b) / std::fabs(x);
			s32 angle = 0;
			for (u32 k = 0; k < kAngleBins / 2; k++)
				angle += (slope >= t.tn[k]) ? 1 : 0;
			const bool mirrored = (b < 0.0f) ? (x > 0.0f) : (x < 0.0f);
			if (mirrored)
				angle = static_cast<s32>(kAngleBins) - 1 - angle;

			const s32 strength = (L1 > hp.se[0] ? 1 : 0) + (L1 > hp.se[1] ? 1 : 0);
			// A flat patch (L1 = L2 = 0) is coherence 0: neither test passes.
			const s32 coherence = (L2 < L1 * hp.cr[0] ? 1 : 0) + (L2 < L1 * hp.cr[1] ? 1 : 0);

			return angle * static_cast<s32>(kStrengthBins * kCoherenceBins) + strength * static_cast<s32>(kCoherenceBins) + coherence;
		}

#if GSTU_NEON
		struct HashConsts
		{
			float32x4_t tn[kAngleBins / 2];
			float32x4_t se0, se1, cr0, cr1;

			HashConsts(const Tables& t, const HashParams& hp)
			{
				for (u32 i = 0; i < kAngleBins / 2; i++)
					tn[i] = vdupq_n_f32(t.tn[i]);
				se0 = vdupq_n_f32(hp.se[0]);
				se1 = vdupq_n_f32(hp.se[1]);
				cr0 = vdupq_n_f32(hp.cr[0]);
				cr1 = vdupq_n_f32(hp.cr[1]);
			}
		};

		// HashScalar, four pixels wide, same operations in the same order.
		inline uint32x4_t HashNeon(const HashConsts& c, float32x4_t a, float32x4_t b, float32x4_t d)
		{
			const float32x4_t zero = vdupq_n_f32(0.0f);
			const float32x4_t T = vaddq_f32(a, d);
			const float32x4_t D = vsubq_f32(vmulq_f32(a, d), vmulq_f32(b, b));
			const float32x4_t q = vmaxnmq_f32(vsubq_f32(vmulq_n_f32(vmulq_f32(T, T), 0.25f), D), zero);
			const float32x4_t s = vsqrtq_f32(q);
			const float32x4_t half = vmulq_n_f32(T, 0.5f);
			const float32x4_t L1 = vaddq_f32(half, s);
			const float32x4_t L2 = vmaxnmq_f32(vsubq_f32(half, s), zero);

			const float32x4_t x = vbslq_f32(vceqq_f32(b, zero), vdupq_n_f32(1.0f), vsubq_f32(L1, d));
			const float32x4_t slope = vdivq_f32(vabsq_f32(b), vabsq_f32(x));
			uint32x4_t angle = vdupq_n_u32(0);
			for (u32 k = 0; k < kAngleBins / 2; k++)
				angle = vsubq_u32(angle, vcgeq_f32(slope, c.tn[k]));
			const uint32x4_t mirrored = vbslq_u32(vcltq_f32(b, zero), vcgtq_f32(x, zero), vcltq_f32(x, zero));
			angle = vbslq_u32(mirrored, vsubq_u32(vdupq_n_u32(kAngleBins - 1), angle), angle);

			uint32x4_t strength = vdupq_n_u32(0);
			strength = vsubq_u32(strength, vcgtq_f32(L1, c.se0));
			strength = vsubq_u32(strength, vcgtq_f32(L1, c.se1));

			uint32x4_t coherence = vdupq_n_u32(0);
			coherence = vsubq_u32(coherence, vcltq_f32(L2, vmulq_f32(L1, c.cr0)));
			coherence = vsubq_u32(coherence, vcltq_f32(L2, vmulq_f32(L1, c.cr1)));

			return vaddq_u32(vaddq_u32(vmulq_n_u32(angle, kStrengthBins * kCoherenceBins), vmulq_n_u32(strength, kCoherenceBins)), coherence);
		}
#endif

		// ---------------------------------------------------------------------------------
		//  Source rows: RGBA8 -> Y, Cb, Cr, A (float), then 2x horizontally
		// ---------------------------------------------------------------------------------

		// BT.601 full range. Cb and Cr are centred on 0, not 128.
		void ConvertRowScalar(const u8* src, u32 count, float* y, float* cb, float* cr, float* a)
		{
			for (u32 i = 0; i < count; i++)
			{
				const float r = static_cast<float>(src[i * 4 + 0]);
				const float g = static_cast<float>(src[i * 4 + 1]);
				const float b = static_cast<float>(src[i * 4 + 2]);
				y[i] = MulAdd(b, 0.114f, MulAdd(g, 0.587f, r * 0.299f));
				cb[i] = MulAdd(b, 0.5f, MulAdd(g, -0.331264f, r * -0.168736f));
				cr[i] = MulAdd(b, -0.081312f, MulAdd(g, -0.418688f, r * 0.5f));
				a[i] = static_cast<float>(src[i * 4 + 3]);
			}
		}

		void ConvertRow(bool neon, const u8* src, u32 w, float* y, float* cb, float* cr, float* a)
		{
			u32 i = 0;
#if GSTU_NEON
			if (neon)
			{
				for (; i + 8 <= w; i += 8)
				{
					const uint8x8x4_t px = vld4_u8(src + i * 4);
					const uint16x8_t r16 = vmovl_u8(px.val[0]);
					const uint16x8_t g16 = vmovl_u8(px.val[1]);
					const uint16x8_t b16 = vmovl_u8(px.val[2]);
					const uint16x8_t a16 = vmovl_u8(px.val[3]);
					for (int half = 0; half < 2; half++)
					{
						const float32x4_t r = vcvtq_f32_u32(half ? vmovl_high_u16(r16) : vmovl_u16(vget_low_u16(r16)));
						const float32x4_t g = vcvtq_f32_u32(half ? vmovl_high_u16(g16) : vmovl_u16(vget_low_u16(g16)));
						const float32x4_t b = vcvtq_f32_u32(half ? vmovl_high_u16(b16) : vmovl_u16(vget_low_u16(b16)));
						const float32x4_t al = vcvtq_f32_u32(half ? vmovl_high_u16(a16) : vmovl_u16(vget_low_u16(a16)));
						const u32 o = i + half * 4;
						vst1q_f32(y + o, vfmaq_n_f32(vfmaq_n_f32(vmulq_n_f32(r, 0.299f), g, 0.587f), b, 0.114f));
						vst1q_f32(cb + o, vfmaq_n_f32(vfmaq_n_f32(vmulq_n_f32(r, -0.168736f), g, -0.331264f), b, 0.5f));
						vst1q_f32(cr + o, vfmaq_n_f32(vfmaq_n_f32(vmulq_n_f32(r, 0.5f), g, -0.418688f), b, -0.081312f));
						vst1q_f32(a + o, al);
					}
				}
			}
#endif
			(void)neon;
			ConvertRowScalar(src + i * 4, w - i, y + i, cb + i, cr + i, a + i);
		}

		// s points at element 0 of a source plane row that has at least 4 valid replicated
		// elements before and after, out at output column 0. Output pixel x of the 2x grid sits at
		// source position x/2 - 0.25, so even x blends s[j-1], s[j] by 1/4, 3/4 and odd x blends
		// s[j], s[j+1] by 3/4, 1/4 (j = x / 2), clamped at the ends.
		void UpsampleRow(bool neon, const float* s, u32 w, float* out)
		{
			u32 j = 0;
#if GSTU_NEON
			if (neon)
			{
				const float32x4_t c25 = vdupq_n_f32(0.25f);
				const float32x4_t c75 = vdupq_n_f32(0.75f);
				for (; j < w; j += 4)
				{
					const float32x4_t prev = vld1q_f32(s + j - 1);
					const float32x4_t cur = vld1q_f32(s + j);
					const float32x4_t next = vld1q_f32(s + j + 1);
					const float32x4_t ev = vfmaq_f32(vmulq_f32(prev, c25), cur, c75);
					const float32x4_t od = vfmaq_f32(vmulq_f32(cur, c75), next, c25);
					const float32x4x2_t z = vzipq_f32(ev, od);
					vst1q_f32(out + 2 * j, z.val[0]);
					vst1q_f32(out + 2 * j + 4, z.val[1]);
				}
				return;
			}
#endif
			(void)neon;
			for (; j < w; j++)
			{
				const s32 sj = static_cast<s32>(j);
				const float cur = s[sj];
				out[2 * j] = MulAdd(cur, 0.75f, s[sj - 1] * 0.25f);
				out[2 * j + 1] = MulAdd(s[sj + 1], 0.25f, cur * 0.75f);
			}
		}

		// Replicate the first and last real column into the padding of a padded row.
		void FillPad(float* row, u32 W, u32 stride)
		{
			const float first = row[kPad];
			const float last = row[kPad + W - 1];
			for (u32 i = 0; i < kPad; i++)
				row[i] = first;
			for (u32 i = kPad + W; i < stride; i++)
				row[i] = last;
		}

		// out = r0 * wa + r1 * wb
		void VCombine(bool neon, const float* r0, const float* r1, float wa, float wb, float* out, u32 n)
		{
			u32 i = 0;
#if GSTU_NEON
			if (neon)
			{
				for (; i + 4 <= n; i += 4)
					vst1q_f32(out + i, vfmaq_n_f32(vmulq_n_f32(vld1q_f32(r0 + i), wa), vld1q_f32(r1 + i), wb));
			}
#endif
			(void)neon;
			for (; i < n; i++)
				out[i] = MulAdd(r1[i], wb, r0[i] * wa);
		}

		// floor(v + 0.5) in place.
		[[maybe_unused]] void RoundRow(bool neon, float* row, u32 n)
		{
			u32 i = 0;
#if GSTU_NEON
			if (neon)
			{
				for (; i + 4 <= n; i += 4)
					vst1q_f32(row + i, vrndmq_f32(vaddq_f32(vld1q_f32(row + i), vdupq_n_f32(0.5f))));
			}
#endif
			(void)neon;
			for (; i < n; i++)
				row[i] = std::floor(row[i] + 0.5f);
		}

		// Source rows and weights for output row y (already in 0..H-1): a 2x row sits at source
		// position y/2 - 0.25.
		struct RowTaps
		{
			u32 i0, i1;
			float wa, wb;
		};

		inline RowTaps TapsForRow(u32 y, u32 h)
		{
			RowTaps t;
			if (y & 1)
			{
				t.i0 = y >> 1;
				t.i1 = std::min(t.i0 + 1, h - 1);
				t.wa = 0.75f;
				t.wb = 0.25f;
			}
			else
			{
				t.i0 = (y >> 1) ? (y >> 1) - 1 : 0;
				t.i1 = y >> 1;
				t.wa = 0.25f;
				t.wb = 0.75f;
			}
			return t;
		}

		// One padded row of L (the cheap 2x luma) from the two horizontally upsampled luma rows
		// that bracket it. This is the only producer of L.
		void LumaRow(bool neon, const float* r0, const float* r1, float wa, float wb, float* out, u32 n)
		{
			VCombine(neon, r0, r1, wa, wb, out, n);
			if constexpr (kRoundCheapLuma)
				RoundRow(neon, out, n);
		}

		// ---------------------------------------------------------------------------------
		//  Structure tensor
		// ---------------------------------------------------------------------------------

		// Gradient products at n columns. lm, l0, lp are the rows above, at and below, pointing at
		// the first column; the horizontal gradient reads one column either side.
		//   gx = L[y][x+1] - L[y][x-1] (horizontal), gy = L[y+1][x] - L[y-1][x] (vertical)
		//   pa = gy*gy, pb = gx*gy, pd = gx*gx
		void Products(bool neon, const float* lm, const float* l0, const float* lp, float* pa, float* pb, float* pd, u32 n)
		{
			u32 i = 0;
#if GSTU_NEON
			if (neon)
			{
				for (; i + 4 <= n; i += 4)
				{
					const float32x4_t gx = vsubq_f32(vld1q_f32(l0 + i + 1), vld1q_f32(l0 + i - 1));
					const float32x4_t gy = vsubq_f32(vld1q_f32(lp + i), vld1q_f32(lm + i));
					vst1q_f32(pa + i, vmulq_f32(gy, gy));
					vst1q_f32(pb + i, vmulq_f32(gx, gy));
					vst1q_f32(pd + i, vmulq_f32(gx, gx));
				}
			}
#endif
			(void)neon;
			for (; i < n; i++)
			{
				const s32 si = static_cast<s32>(i);
				const float gx = l0[si + 1] - l0[si - 1];
				const float gy = lp[si] - lm[si];
				pa[i] = gy * gy;
				pb[i] = gx * gy;
				pd[i] = gx * gx;
			}
		}

		// out[x] = sum over m of g[m] * (p[x-m] + p[x+m]) (m = 0 once), x in 0..n. p points at
		// column 0 and must have 5 valid columns either side.
		void HBlur(bool neon, const Tables& t, const float* p, float* out, u32 n)
		{
			u32 x = 0;
#if GSTU_NEON
			if (neon)
			{
				for (; x + 4 <= n; x += 4)
				{
					float32x4_t acc = vmulq_n_f32(vld1q_f32(p + x), t.g[0]);
					for (int m = 1; m <= kMargin; m++)
						acc = vfmaq_n_f32(acc, vaddq_f32(vld1q_f32(p + x - m), vld1q_f32(p + x + m)), t.g[m]);
					vst1q_f32(out + x, acc);
				}
			}
#endif
			(void)neon;
			for (; x < n; x++)
			{
				const s32 sx = static_cast<s32>(x);
				float acc = p[sx] * t.g[0];
				for (int m = 1; m <= kMargin; m++)
					acc = MulAdd(p[sx - m] + p[sx + m], t.g[m], acc);
				out[x] = acc;
			}
		}

		// ---------------------------------------------------------------------------------
		//  Hash and filter, one output row
		// ---------------------------------------------------------------------------------

		struct FilterRowArgs
		{
			const float* ha[kPatchSize]; // horizontally blurred a rows y-5..y+5
			const float* hb[kPatchSize];
			const float* hd[kPatchSize];
			const float* lrows[kPatchSize]; // L rows y-5..y+5, at column -5 (pad included)
			u32 phase_row; // ((y + 1) & 1) * 2
			u32 n; // columns, a multiple of 4
			float* raisr; // out: filter output, n floats
			s32* hash; // out: hash bucket, n entries
		};

		// Vertical pass of the tensor blur, then the hash, for n columns.
		void HashRow(bool neon, const Tables& t, const FilterSet& f, const FilterRowArgs& r)
		{
			const HashParams hp = MakeHashParams(f);
			u32 x = 0;
#if GSTU_NEON
			if (neon)
			{
				const HashConsts hc(t, hp);
				for (; x < r.n; x += 4)
				{
					float32x4_t a = vmulq_n_f32(vld1q_f32(r.ha[kMargin] + x), t.gv[0]);
					float32x4_t b = vmulq_n_f32(vld1q_f32(r.hb[kMargin] + x), t.gv[0]);
					float32x4_t d = vmulq_n_f32(vld1q_f32(r.hd[kMargin] + x), t.gv[0]);
					for (int m = 1; m <= kMargin; m++)
					{
						a = vfmaq_n_f32(a, vaddq_f32(vld1q_f32(r.ha[kMargin - m] + x), vld1q_f32(r.ha[kMargin + m] + x)), t.gv[m]);
						b = vfmaq_n_f32(b, vaddq_f32(vld1q_f32(r.hb[kMargin - m] + x), vld1q_f32(r.hb[kMargin + m] + x)), t.gv[m]);
						d = vfmaq_n_f32(d, vaddq_f32(vld1q_f32(r.hd[kMargin - m] + x), vld1q_f32(r.hd[kMargin + m] + x)), t.gv[m]);
					}
					vst1q_s32(r.hash + x, vreinterpretq_s32_u32(HashNeon(hc, a, b, d)));
				}
				return;
			}
#endif
			(void)neon;
			for (; x < r.n; x++)
			{
				float a = r.ha[kMargin][x] * t.gv[0];
				float b = r.hb[kMargin][x] * t.gv[0];
				float d = r.hd[kMargin][x] * t.gv[0];
				for (int m = 1; m <= kMargin; m++)
				{
					a = MulAdd(r.ha[kMargin - m][x] + r.ha[kMargin + m][x], t.gv[m], a);
					b = MulAdd(r.hb[kMargin - m][x] + r.hb[kMargin + m][x], t.gv[m], b);
					d = MulAdd(r.hd[kMargin - m][x] + r.hd[kMargin + m][x], t.gv[m], d);
				}
				r.hash[x] = HashScalar(t, hp, a, b, d);
			}
		}

		inline u32 PixelPhase(const FilterRowArgs& r, u32 x) { return r.phase_row + ((x + 1) & 1); }

		inline float DotScalar(const float* filter, const FilterRowArgs& r, u32 x)
		{
			// 12 independent lanes (3 vectors x 4), reduced in the order the NEON path reduces.
			float acc[kFilterRowStride] = {};
			for (u32 row = 0; row < kPatchSize; row++)
			{
				const float* d = r.lrows[row] + x;
				const float* f = filter + row * kFilterRowStride;
				for (u32 j = 0; j < kFilterRowStride; j++)
					acc[j] = MulAdd(d[j], f[j], acc[j]);
			}
			float s[4];
			for (int c = 0; c < 4; c++)
				s[c] = (acc[c] + acc[4 + c]) + acc[8 + c];
			return (s[0] + s[1]) + (s[2] + s[3]);
		}

		// The 11x11 filter of the hash chosen for each pixel, dotted with the L window, then the
		// range fallback: outside (0, 255) the filter output is replaced by L itself.
		void DotRow(bool neon, const FilterSet& f, const FilterRowArgs& r)
		{
			u32 x = 0;
#if GSTU_NEON
			if (neon)
			{
				const float32x4_t zero = vdupq_n_f32(0.0f);
				const float32x4_t c255 = vdupq_n_f32(255.0f);
				for (; x < r.n; x += 4)
				{
					const float* fp[4];
					for (u32 p = 0; p < 4; p++)
						fp[p] = f.Filter(static_cast<u32>(r.hash[x + p]), PixelPhase(r, x + p));

					// Four pixels at once, 12 accumulators. Pixel p's window starts p columns
					// after pixel 0's, so its three vectors are EXT of the four loaded vectors.
					float32x4_t acc[4][3];
					for (u32 p = 0; p < 4; p++)
						acc[p][0] = acc[p][1] = acc[p][2] = zero;
					for (u32 row = 0; row < kPatchSize; row++)
					{
						const float* d = r.lrows[row] + x;
						const float32x4_t v0 = vld1q_f32(d);
						const float32x4_t v1 = vld1q_f32(d + 4);
						const float32x4_t v2 = vld1q_f32(d + 8);
						const float32x4_t v3 = vld1q_f32(d + 12);
						const u32 fo = row * kFilterRowStride;
						acc[0][0] = vfmaq_f32(acc[0][0], v0, vld1q_f32(fp[0] + fo));
						acc[0][1] = vfmaq_f32(acc[0][1], v1, vld1q_f32(fp[0] + fo + 4));
						acc[0][2] = vfmaq_f32(acc[0][2], v2, vld1q_f32(fp[0] + fo + 8));
						acc[1][0] = vfmaq_f32(acc[1][0], vextq_f32(v0, v1, 1), vld1q_f32(fp[1] + fo));
						acc[1][1] = vfmaq_f32(acc[1][1], vextq_f32(v1, v2, 1), vld1q_f32(fp[1] + fo + 4));
						acc[1][2] = vfmaq_f32(acc[1][2], vextq_f32(v2, v3, 1), vld1q_f32(fp[1] + fo + 8));
						acc[2][0] = vfmaq_f32(acc[2][0], vextq_f32(v0, v1, 2), vld1q_f32(fp[2] + fo));
						acc[2][1] = vfmaq_f32(acc[2][1], vextq_f32(v1, v2, 2), vld1q_f32(fp[2] + fo + 4));
						acc[2][2] = vfmaq_f32(acc[2][2], vextq_f32(v2, v3, 2), vld1q_f32(fp[2] + fo + 8));
						acc[3][0] = vfmaq_f32(acc[3][0], vextq_f32(v0, v1, 3), vld1q_f32(fp[3] + fo));
						acc[3][1] = vfmaq_f32(acc[3][1], vextq_f32(v1, v2, 3), vld1q_f32(fp[3] + fo + 4));
						acc[3][2] = vfmaq_f32(acc[3][2], vextq_f32(v2, v3, 3), vld1q_f32(fp[3] + fo + 8));
					}
					float32x4_t sum[4];
					for (u32 p = 0; p < 4; p++)
						sum[p] = vaddq_f32(vaddq_f32(acc[p][0], acc[p][1]), acc[p][2]);
					const float32x4_t v = vpaddq_f32(vpaddq_f32(sum[0], sum[1]), vpaddq_f32(sum[2], sum[3]));
					const float32x4_t centre = vld1q_f32(r.lrows[kMargin] + kMargin + x);
					const uint32x4_t inside = vandq_u32(vcgtq_f32(v, zero), vcltq_f32(v, c255));
					vst1q_f32(r.raisr + x, vbslq_f32(inside, v, centre));
				}
				return;
			}
#endif
			(void)neon;
			for (; x < r.n; x++)
			{
				const float v = DotScalar(f.Filter(static_cast<u32>(r.hash[x]), PixelPhase(r, x)), r, x);
				const float centre = r.lrows[kMargin][kMargin + x];
				r.raisr[x] = (v > 0.0f && v < 255.0f) ? v : centre;
			}
		}

		// ---------------------------------------------------------------------------------
		//  Census blend and colour recombine, one output row
		// ---------------------------------------------------------------------------------

		struct BlendRowArgs
		{
			const float* l[3]; // L rows y-1, y, y+1, at column 0 (pad included)
			const float* r[3]; // raisr rows y-1, y, y+1, at column 0 (one valid column each side)
			const float* cb[2]; // chroma/alpha source rows (horizontally upsampled, padded), i0 and i1
			const float* cr[2];
			const float* al[2];
			float wa, wb;
			u32 n; // columns, a multiple of 4
			u32 W; // real columns
			float* yout; // out: blended Y', n floats
			u8* dst; // out: W RGBA8 pixels
		};

		inline u8 ToByte(float v)
		{
			v = std::fmin(std::fmax(v, 0.0f), 255.0f);
			return static_cast<u8>(static_cast<u32>(v + 0.5f));
		}

		void BlendRow(bool neon, const BlendRowArgs& a)
		{
			u32 x = 0;
#if GSTU_NEON
			if (neon)
			{
				const float32x4_t one = vdupq_n_f32(1.0f);
				const float32x4_t zero = vdupq_n_f32(0.0f);
				const float32x4_t c255 = vdupq_n_f32(255.0f);
				const float32x4_t half = vdupq_n_f32(0.5f);
				for (; x < a.n; x += 4)
				{
					const s32 sx = static_cast<s32>(x);
					const float32x4_t lc = vld1q_f32(a.l[1] + kPad + sx);
					const float32x4_t rc = vld1q_f32(a.r[1] + sx);
					uint32x4_t count = vdupq_n_u32(0);
					for (int row = 0; row < 3; row++)
					{
						for (int col = -1; col <= 1; col++)
						{
							if (row == 1 && col == 0)
								continue;
							const uint32x4_t lbit = vcltq_f32(vld1q_f32(a.l[row] + kPad + sx + col), lc);
							const uint32x4_t rbit = vcltq_f32(vld1q_f32(a.r[row] + sx + col), rc);
							count = vsubq_u32(count, veorq_u32(lbit, rbit));
						}
					}
					const float32x4_t weight = vmulq_n_f32(vcvtq_f32_u32(count), 0.125f);
					const float32x4_t yb = vfmaq_f32(vmulq_f32(vsubq_f32(one, weight), rc), weight, lc);
					vst1q_f32(a.yout + x, yb);

					const float32x4_t cb = vfmaq_n_f32(vmulq_n_f32(vld1q_f32(a.cb[0] + kPad + x), a.wa), vld1q_f32(a.cb[1] + kPad + x), a.wb);
					const float32x4_t cr = vfmaq_n_f32(vmulq_n_f32(vld1q_f32(a.cr[0] + kPad + x), a.wa), vld1q_f32(a.cr[1] + kPad + x), a.wb);
					const float32x4_t al = vfmaq_n_f32(vmulq_n_f32(vld1q_f32(a.al[0] + kPad + x), a.wa), vld1q_f32(a.al[1] + kPad + x), a.wb);
					const float32x4_t rr = vfmaq_n_f32(yb, cr, 1.402f);
					const float32x4_t gg = vfmaq_n_f32(vfmaq_n_f32(yb, cb, -0.344136f), cr, -0.714136f);
					const float32x4_t bb = vfmaq_n_f32(yb, cb, 1.772f);

					const uint32x4_t ri = vcvtq_u32_f32(vaddq_f32(vminnmq_f32(vmaxnmq_f32(rr, zero), c255), half));
					const uint32x4_t gi = vcvtq_u32_f32(vaddq_f32(vminnmq_f32(vmaxnmq_f32(gg, zero), c255), half));
					const uint32x4_t bi = vcvtq_u32_f32(vaddq_f32(vminnmq_f32(vmaxnmq_f32(bb, zero), c255), half));
					const uint32x4_t ai = vcvtq_u32_f32(vaddq_f32(vminnmq_f32(vmaxnmq_f32(al, zero), c255), half));
					const uint32x4_t packed = vorrq_u32(vorrq_u32(ri, vshlq_n_u32(gi, 8)), vorrq_u32(vshlq_n_u32(bi, 16), vshlq_n_u32(ai, 24)));
					if (x + 4 <= a.W)
					{
						vst1q_u8(a.dst + x * 4, vreinterpretq_u8_u32(packed));
					}
					else if (x < a.W)
					{
						u32 tmp[4];
						vst1q_u32(tmp, packed);
						std::memcpy(a.dst + x * 4, tmp, (a.W - x) * 4);
					}
				}
				return;
			}
#endif
			(void)neon;
			for (; x < a.n; x++)
			{
				const s32 sx = static_cast<s32>(x);
				const float lc = a.l[1][kPad + sx];
				const float rc = a.r[1][sx];
				u32 count = 0;
				for (int row = 0; row < 3; row++)
				{
					for (int col = -1; col <= 1; col++)
					{
						if (row == 1 && col == 0)
							continue;
						const bool lbit = a.l[row][kPad + sx + col] < lc;
						const bool rbit = a.r[row][sx + col] < rc;
						count += (lbit != rbit) ? 1 : 0;
					}
				}
				const float weight = static_cast<float>(count) * 0.125f;
				const float yb = MulAdd(weight, lc, (1.0f - weight) * rc);
				a.yout[x] = yb;

				const float cb = MulAdd(a.cb[1][kPad + x], a.wb, a.cb[0][kPad + x] * a.wa);
				const float cr = MulAdd(a.cr[1][kPad + x], a.wb, a.cr[0][kPad + x] * a.wa);
				const float al = MulAdd(a.al[1][kPad + x], a.wb, a.al[0][kPad + x] * a.wa);
				const float rr = MulAdd(cr, 1.402f, yb);
				const float gg = MulAdd(cr, -0.714136f, MulAdd(cb, -0.344136f, yb));
				const float bb = MulAdd(cb, 1.772f, yb);
				if (x < a.W)
				{
					u8* o = a.dst + x * 4;
					o[0] = ToByte(rr);
					o[1] = ToByte(gg);
					o[2] = ToByte(bb);
					o[3] = ToByte(al);
				}
			}
		}

		// ---------------------------------------------------------------------------------
		//  Driver
		// ---------------------------------------------------------------------------------

		void Upscale(const FilterSet& filters, const u8* src, u32 w, u32 h, u32 src_pitch, u8* dst, u32 dst_pitch, bool neon, LumaPlanes* planes)
		{
			if (w == 0 || h == 0)
				return;

			const Tables& tables = GetTables();
			const u32 W = w * 2;
			const u32 H = h * 2;
			const u32 Wv = RoundUp4(W); // columns processed; the extra ones are discarded
			const u32 ls = Wv + 24; // row stride of padded planes: kPad + Wv + at least 16
			const u32 rs = Wv + 12; // row stride of the raisr band: 4 + Wv + 8

			if (planes)
			{
				const size_t count = static_cast<size_t>(W) * H;
				planes->width = W;
				planes->height = H;
				planes->cheap.assign(count, 0.0f);
				planes->hash.assign(count, 0);
				planes->raisr.assign(count, 0.0f);
				planes->blended.assign(count, 0.0f);
			}

			// Source row planes with 4 replicated columns before and 16 after, for the upsampler.
			const u32 tmp_stride = w + 24;
			std::vector<float> tmp[4];
			for (auto& v : tmp)
				v.assign(tmp_stride, 0.0f);

			std::vector<float> hbuf[4]; // Y, Cb, Cr, A of the source rows, 2x horizontally, padded
			for (auto& v : hbuf)
				v.assign(static_cast<size_t>(kSourceRows) * ls, 0.0f);
			std::vector<float> lbuf(static_cast<size_t>(kLumaRows) * ls, 0.0f); // L, padded
			std::vector<float> prod[3];
			for (auto& v : prod)
				v.assign(Wv + 16, 0.0f);
			std::vector<float> blur[3]; // horizontally blurred products, per row
			for (auto& v : blur)
				v.assign(static_cast<size_t>(kBandRows + 12) * Wv, 0.0f);
			std::vector<float> rbuf(static_cast<size_t>(kBandRows + 2) * rs, 0.0f); // raisr rows y0-1..y1
			std::vector<s32> hash_row(Wv, 0);
			std::vector<float> yrow(Wv, 0.0f);

			const s32 sH = static_cast<s32>(H);
			const s32 sh = static_cast<s32>(h);

			for (u32 y0 = 0; y0 < H; y0 += kBandRows)
			{
				const s32 Y0 = static_cast<s32>(y0);
				const s32 Y1 = static_cast<s32>(std::min(y0 + kBandRows, H));
				const s32 band = Y1 - Y0;

				// Raisr rows needed: the band and one either side for the census.
				const s32 rowlo = std::max(Y0 - 1, 0);
				const s32 rowhi = std::min(sH - 1, Y1);

				// 1. Source rows feeding L rows Y0-7 .. Y1+6, upsampled horizontally.
				const s32 ymin = std::max(Y0 - 7, 0);
				const s32 ymax = std::min(Y1 + 6, sH - 1);
				const s32 s_lo = std::max((ymin >> 1) - 1, 0);
				const s32 s_hi = std::min((ymax >> 1) + 1, sh - 1);
				for (s32 s = s_lo; s <= s_hi; s++)
				{
					float* t0 = tmp[0].data() + 4;
					float* t1 = tmp[1].data() + 4;
					float* t2 = tmp[2].data() + 4;
					float* t3 = tmp[3].data() + 4;
					ConvertRow(neon, src + static_cast<size_t>(s) * src_pitch, w, t0, t1, t2, t3);
					for (int p = 0; p < 4; p++)
					{
						float* t = tmp[p].data() + 4;
						for (u32 i = 0; i < 4; i++)
							t[-1 - static_cast<s32>(i)] = t[0];
						for (u32 i = 0; i < 20; i++)
							t[w + i] = t[w - 1];
						float* out = hbuf[p].data() + static_cast<size_t>(s - s_lo) * ls;
						UpsampleRow(neon, t, w, out + kPad);
						FillPad(out, W, ls);
					}
				}

				// 2. L rows Y0-7 .. Y1+6 (clamped to the image, which replicates the edge rows).
				for (s32 i = 0; i < band + 14; i++)
				{
					const u32 yc = static_cast<u32>(std::clamp(Y0 - 7 + i, 0, sH - 1));
					const RowTaps tp = TapsForRow(yc, h);
					LumaRow(neon, hbuf[0].data() + static_cast<size_t>(static_cast<s32>(tp.i0) - s_lo) * ls,
						hbuf[0].data() + static_cast<size_t>(static_cast<s32>(tp.i1) - s_lo) * ls,
						tp.wa, tp.wb, lbuf.data() + static_cast<size_t>(i) * ls, ls);
				}

				// 3. Gradient products and their horizontal blur, rows rowlo-5 .. rowhi+5.
				for (s32 yy = rowlo - 5; yy <= rowhi + 5; yy++)
				{
					const float* l0 = lbuf.data() + static_cast<size_t>(yy - Y0 + 7) * ls + kPad - 5;
					Products(neon, l0 - ls, l0, l0 + ls, prod[0].data(), prod[1].data(), prod[2].data(), Wv + 12);
					for (int p = 0; p < 3; p++)
						HBlur(neon, tables, prod[p].data() + 5, blur[p].data() + static_cast<size_t>(yy - (rowlo - 5)) * Wv, Wv);
				}

				// 4. Vertical blur, hash and filter for rows rowlo .. rowhi.
				for (s32 y = rowlo; y <= rowhi; y++)
				{
					FilterRowArgs fa;
					for (int k = 0; k < static_cast<int>(kPatchSize); k++)
					{
						const size_t brow = static_cast<size_t>(y - 5 + k - (rowlo - 5)) * Wv;
						fa.ha[k] = blur[0].data() + brow;
						fa.hb[k] = blur[1].data() + brow;
						fa.hd[k] = blur[2].data() + brow;
						fa.lrows[k] = lbuf.data() + static_cast<size_t>(y - 5 + k - Y0 + 7) * ls + kPad - 5;
					}
					fa.phase_row = static_cast<u32>(((y + 1) & 1) * 2);
					fa.n = Wv;
					float* rrow = rbuf.data() + static_cast<size_t>(y - (Y0 - 1)) * rs + 4;
					fa.raisr = rrow;
					fa.hash = hash_row.data();
					HashRow(neon, tables, filters, fa);
					DotRow(neon, filters, fa);

					for (u32 i = 0; i < 4; i++)
						rrow[-1 - static_cast<s32>(i)] = rrow[0];
					for (u32 i = W; i < rs - 4; i++)
						rrow[i] = rrow[W - 1];

					if (planes && y >= Y0 && y < Y1)
					{
						const size_t o = static_cast<size_t>(y) * W;
						std::memcpy(planes->hash.data() + o, hash_row.data(), W * sizeof(s32));
					}
				}
				// Raisr rows beyond the image replicate the edge row.
				if (Y0 == 0)
					std::memcpy(rbuf.data(), rbuf.data() + rs, rs * sizeof(float));
				if (Y1 == sH)
					std::memcpy(rbuf.data() + static_cast<size_t>(band + 1) * rs, rbuf.data() + static_cast<size_t>(band) * rs, rs * sizeof(float));

				// 5. Census blend and colour recombine.
				for (s32 y = Y0; y < Y1; y++)
				{
					const RowTaps tp = TapsForRow(static_cast<u32>(y), h);
					BlendRowArgs ba;
					const float* lrow = lbuf.data() + static_cast<size_t>(y - Y0 + 7) * ls;
					const float* rrow = rbuf.data() + static_cast<size_t>(y - (Y0 - 1)) * rs + 4;
					for (int k = 0; k < 3; k++)
					{
						ba.l[k] = lrow + (k - 1) * static_cast<s32>(ls);
						ba.r[k] = rrow + (k - 1) * static_cast<s32>(rs);
					}
					const size_t o0 = static_cast<size_t>(static_cast<s32>(tp.i0) - s_lo) * ls;
					const size_t o1 = static_cast<size_t>(static_cast<s32>(tp.i1) - s_lo) * ls;
					ba.cb[0] = hbuf[1].data() + o0;
					ba.cb[1] = hbuf[1].data() + o1;
					ba.cr[0] = hbuf[2].data() + o0;
					ba.cr[1] = hbuf[2].data() + o1;
					ba.al[0] = hbuf[3].data() + o0;
					ba.al[1] = hbuf[3].data() + o1;
					ba.wa = tp.wa;
					ba.wb = tp.wb;
					ba.n = Wv;
					ba.W = W;
					ba.yout = yrow.data();
					ba.dst = dst + static_cast<size_t>(y) * dst_pitch;
					BlendRow(neon, ba);

					if (planes)
					{
						const size_t o = static_cast<size_t>(y) * W;
						std::memcpy(planes->cheap.data() + o, lrow + kPad, W * sizeof(float));
						std::memcpy(planes->raisr.data() + o, rrow, W * sizeof(float));
						std::memcpy(planes->blended.data() + o, yrow.data(), W * sizeof(float));
					}
				}
			}
		}

		// ---------------------------------------------------------------------------------
		//  Filter file parsing
		// ---------------------------------------------------------------------------------

		bool ReadFile(const std::filesystem::path& path, std::vector<u8>* out)
		{
			std::ifstream f(path, std::ios::binary);
			if (!f)
				return false;
			f.seekg(0, std::ios::end);
			const std::streamoff size = f.tellg();
			if (size < 0)
				return false;
			f.seekg(0, std::ios::beg);
			out->resize(static_cast<size_t>(size));
			if (size > 0)
				f.read(reinterpret_cast<char*>(out->data()), size);
			return static_cast<bool>(f);
		}

		inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

		// [+-]digits[.digits][e[+-]digits], nothing else. Independent of the C locale.
		bool ParseDecimal(std::string_view tok, double* out)
		{
			size_t i = 0;
			bool neg = false;
			if (i < tok.size() && (tok[i] == '+' || tok[i] == '-'))
				neg = tok[i++] == '-';
			double mant = 0.0;
			int digits = 0;
			int frac = 0;
			bool dot = false;
			for (; i < tok.size(); i++)
			{
				const char c = tok[i];
				if (c >= '0' && c <= '9')
				{
					mant = mant * 10.0 + (c - '0');
					digits++;
					if (dot)
						frac++;
				}
				else if (c == '.' && !dot)
				{
					dot = true;
				}
				else
				{
					break;
				}
			}
			if (digits == 0)
				return false;
			int exp10 = 0;
			if (i < tok.size())
			{
				if (tok[i] != 'e' && tok[i] != 'E')
					return false;
				i++;
				bool eneg = false;
				if (i < tok.size() && (tok[i] == '+' || tok[i] == '-'))
					eneg = tok[i++] == '-';
				if (i == tok.size())
					return false;
				for (; i < tok.size(); i++)
				{
					if (tok[i] < '0' || tok[i] > '9' || exp10 > 1000)
						return false;
					exp10 = exp10 * 10 + (tok[i] - '0');
				}
				if (eneg)
					exp10 = -exp10;
			}
			double v = mant * std::pow(10.0, exp10 - frac);
			*out = neg ? -v : v;
			return std::isfinite(*out);
		}

		// Exactly two ascending finite edges, both within [lo, hi].
		bool ParseEdges(std::string_view text, double lo, double hi, float* edges, const char* what, std::string* error)
		{
			double values[3];
			int count = 0;
			size_t i = 0;
			while (i < text.size())
			{
				if (IsSpace(text[i]))
				{
					i++;
					continue;
				}
				size_t j = i;
				while (j < text.size() && !IsSpace(text[j]))
					j++;
				if (count == 2 || !ParseDecimal(text.substr(i, j - i), &values[count]))
				{
					if (error)
						*error = std::string(what) + " is not two decimal numbers";
					return false;
				}
				count++;
				i = j;
			}
			if (count != 2)
			{
				if (error)
					*error = std::string(what) + " is not two decimal numbers";
				return false;
			}
			if (!(values[0] <= values[1]))
			{
				if (error)
					*error = std::string(what) + " edges are not ascending";
				return false;
			}
			if (!(values[0] >= lo && values[1] <= hi))
			{
				if (error)
					*error = std::string(what) + " edges are out of range";
				return false;
			}
			edges[0] = static_cast<float>(values[0]);
			edges[1] = static_cast<float>(values[1]);
			return true;
		}

		inline u32 ReadU32LE(const u8* p)
		{
			return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
		}
	} // namespace

	// -------------------------------------------------------------------------------------
	//  FilterSet
	// -------------------------------------------------------------------------------------

	std::shared_ptr<const FilterSet> FilterSet::LoadFromMemory(std::span<const u8> filterbin,
		std::string_view strbin, std::string_view cohbin, std::string* error)
	{
		auto fail = [&](const std::string& msg) -> std::shared_ptr<const FilterSet> {
			if (error)
				*error = msg;
			return nullptr;
		};

		constexpr size_t header_size = 16;
		if (filterbin.size() < header_size)
			return fail("filter file is shorter than its 16 byte header");
		if (std::memcmp(filterbin.data(), "fp32", 4) != 0)
			return fail("filter file does not start with \"fp32\"");
		const u32 hashes = ReadU32LE(filterbin.data() + 4);
		const u32 phases = ReadU32LE(filterbin.data() + 8);
		const u32 taps = ReadU32LE(filterbin.data() + 12);
		if (hashes != kHashCount || phases != kPixelPhases || taps != kFilterTaps)
		{
			return fail("filter file header says " + std::to_string(hashes) + " hashes, " + std::to_string(phases) +
						" phases, " + std::to_string(taps) + " taps; expected " + std::to_string(kHashCount) + ", " +
						std::to_string(kPixelPhases) + ", " + std::to_string(kFilterTaps));
		}
		const size_t payload = static_cast<size_t>(kHashCount) * kPixelPhases * kFilterTaps * sizeof(float);
		if (filterbin.size() != header_size + payload)
		{
			return fail("filter file is " + std::to_string(filterbin.size()) + " bytes; expected " +
						std::to_string(header_size + payload));
		}

		std::shared_ptr<FilterSet> set(new FilterSet());
		set->m_taps.assign(static_cast<size_t>(kHashCount) * kPixelPhases * kFilterStride, 0.0f);
		const u8* p = filterbin.data() + header_size;
		for (u32 f = 0; f < kHashCount * kPixelPhases; f++)
		{
			float* dst = set->m_taps.data() + static_cast<size_t>(f) * kFilterStride;
			for (u32 row = 0; row < kPatchSize; row++)
			{
				for (u32 col = 0; col < kPatchSize; col++)
				{
					const float v = std::bit_cast<float>(ReadU32LE(p));
					p += 4;
					if (!std::isfinite(v))
						return fail("filter file holds a NaN or infinity");
					dst[row * kFilterRowStride + col] = v;
				}
			}
		}

		if (!ParseEdges(strbin, 0.0, std::numeric_limits<double>::max(), set->m_strength_edges, "strength bin file", error))
			return nullptr;
		if (!ParseEdges(cohbin, 0.0, 1.0, set->m_coherence_edges, "coherence bin file", error))
			return nullptr;
		return set;
	}

	std::shared_ptr<const FilterSet> FilterSet::Load(const std::string& dir, std::string* error)
	{
		const std::filesystem::path base(std::u8string(reinterpret_cast<const char8_t*>(dir.data()), dir.size()));
		std::vector<u8> bufs[3];
		const char* names[3] = {"filterbin_2_8", "Qfactor_strbin_2_8", "Qfactor_cohbin_2_8"};
		for (int i = 0; i < 3; i++)
		{
			if (!ReadFile(base / names[i], &bufs[i]))
			{
				if (error)
					*error = std::string("cannot read ") + names[i] + " in " + dir;
				return nullptr;
			}
		}
		return LoadFromMemory(bufs[0],
			std::string_view(reinterpret_cast<const char*>(bufs[1].data()), bufs[1].size()),
			std::string_view(reinterpret_cast<const char*>(bufs[2].data()), bufs[2].size()), error);
	}

	// -------------------------------------------------------------------------------------
	//  Entry points
	// -------------------------------------------------------------------------------------

	bool NeonAvailable()
	{
		return GSTU_NEON != 0;
	}

	void UpscaleRGBA8x2(const FilterSet& filters, const u8* src, u32 w, u32 h, u32 src_pitch, u8* dst, u32 dst_pitch)
	{
		Upscale(filters, src, w, h, src_pitch, dst, dst_pitch, GSTU_NEON != 0, nullptr);
	}

	void UpscaleRGBA8x2Planes(const FilterSet& filters, const u8* src, u32 w, u32 h, u32 src_pitch,
		u8* dst, u32 dst_pitch, Impl impl, LumaPlanes* planes)
	{
		const bool neon = GSTU_NEON != 0 && impl != Impl::Scalar;
		Upscale(filters, src, w, h, src_pitch, dst, dst_pitch, neon, planes);
	}

	void BilinearRGBA8x2(const u8* src, u32 w, u32 h, u32 src_pitch, u8* dst, u32 dst_pitch)
	{
		if (w == 0 || h == 0)
			return;
		// Weights are 1/4 and 3/4 per axis, so the exact result is a multiple of 1/16.
		for (u32 y = 0; y < h * 2; y++)
		{
			const RowTaps ty = TapsForRow(y, h);
			const u32 wy0 = (y & 1) ? 3 : 1;
			const u32 wy1 = 4 - wy0;
			const u8* r0 = src + static_cast<size_t>(ty.i0) * src_pitch;
			const u8* r1 = src + static_cast<size_t>(ty.i1) * src_pitch;
			u8* out = dst + static_cast<size_t>(y) * dst_pitch;
			for (u32 x = 0; x < w * 2; x++)
			{
				const u32 j = x >> 1;
				u32 j0, j1, wx0;
				if (x & 1)
				{
					j0 = j;
					j1 = std::min(j + 1, w - 1);
					wx0 = 3;
				}
				else
				{
					j0 = j ? j - 1 : 0;
					j1 = j;
					wx0 = 1;
				}
				const u32 wx1 = 4 - wx0;
				for (u32 c = 0; c < 4; c++)
				{
					const u32 top = wx0 * r0[j0 * 4 + c] + wx1 * r0[j1 * 4 + c];
					const u32 bot = wx0 * r1[j0 * 4 + c] + wx1 * r1[j1 * 4 + c];
					out[x * 4 + c] = static_cast<u8>((wy0 * top + wy1 * bot + 8) >> 4);
				}
			}
		}
	}

	s32 HashTensor(const FilterSet& filters, float a, float b, float d)
	{
		return HashScalar(GetTables(), MakeHashParams(filters), a, b, d);
	}

	std::array<float, kPatchSize> StructureKernel()
	{
		return MakeGaussian11();
	}
} // namespace GSTextureUpscaler
