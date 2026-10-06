// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Tests for the RAISR 2x texture upscaler engine (GS/Renderers/HW/GSTextureUpscaler.h).
//
// The filters under test are the real shipped set, bin/resources/upscale/raisr/ps2. All images
// are generated here; none come from a third party.

#include "GS/Renderers/HW/GSTextureUpscaleSupport.h"
#include "GS/Renderers/HW/GSTextureUpscaler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifndef GS_UPSCALER_RESOURCE_DIR
#error "GS_UPSCALER_RESOURCE_DIR must name bin/resources/upscale/raisr"
#endif

using namespace GSTextureUpscaler;

namespace
{
	constexpr double kPi = 3.14159265358979323846;

	// The set the emulator ships; the 4x mode runs it twice.
	constexpr const char* kShippedSet = "ps2";

	// Quality thresholds, in dB of luma PSNR over bilinear. Set from the measurements printed by
	// BeatsBilinearOnSyntheticImages.
	// Measured on the four scenes (shapes, lines, checker, glyphs): +5.7 +3.8 +3.4 +1.6, mean +3.6.
	// The images and the engine are deterministic, so the headroom is for float differences
	// between compilers, not noise. Deliberately breaking the angle convention drops the mean to
	// +0.4 with b negated and to +1.8 with the gradient axes swapped, so the mean threshold sits
	// above both.
	constexpr double kMinImageGainDb = 0.4;
	constexpr double kMinMeanGainDb = 2.6;

	// -----------------------------------------------------------------------------------------
	//  Helpers
	// -----------------------------------------------------------------------------------------

	std::string SetDir(const char* set) { return std::string(GS_UPSCALER_RESOURCE_DIR) + "/" + set; }

	std::shared_ptr<const FilterSet> LoadSet(const char* set)
	{
		std::string error;
		auto fs = FilterSet::Load(SetDir(set), &error);
		EXPECT_TRUE(fs) << error;
		return fs;
	}

	std::vector<u8> ReadFileBytes(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary);
		return std::vector<u8>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}

	std::string ReadFileText(const std::string& path)
	{
		const auto b = ReadFileBytes(path);
		return std::string(b.begin(), b.end());
	}

	// Deterministic, library-independent generator.
	struct Rng
	{
		u32 s;
		explicit Rng(u32 seed)
			: s(seed ? seed : 1)
		{
		}
		u32 Next()
		{
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			return s;
		}
		double Uniform() { return (Next() >> 8) * (1.0 / 16777216.0); }
	};

	struct Image
	{
		u32 w = 0, h = 0;
		std::vector<u8> px; // RGBA8, tightly packed

		Image() = default;
		Image(u32 w_, u32 h_)
			: w(w_)
			, h(h_)
			, px(static_cast<size_t>(w_) * h_ * 4, 0)
		{
		}
		u8* At(u32 x, u32 y) { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
		const u8* At(u32 x, u32 y) const { return &px[(static_cast<size_t>(y) * w + x) * 4]; }
	};

	Image Constant(u32 w, u32 h, u8 r, u8 g, u8 b, u8 a)
	{
		Image im(w, h);
		for (u32 i = 0; i < w * h; i++)
		{
			im.px[i * 4 + 0] = r;
			im.px[i * 4 + 1] = g;
			im.px[i * 4 + 2] = b;
			im.px[i * 4 + 3] = a;
		}
		return im;
	}

	Image RandomImage(u32 w, u32 h, u32 seed, u8 alpha_lo = 0, u8 alpha_hi = 255)
	{
		Rng rng(seed);
		Image im(w, h);
		for (u32 i = 0; i < w * h; i++)
		{
			im.px[i * 4 + 0] = static_cast<u8>(rng.Next());
			im.px[i * 4 + 1] = static_cast<u8>(rng.Next());
			im.px[i * 4 + 2] = static_cast<u8>(rng.Next());
			im.px[i * 4 + 3] = static_cast<u8>(alpha_lo + rng.Next() % (alpha_hi - alpha_lo + 1u));
		}
		return im;
	}

	Image Upscale(const FilterSet& fs, const Image& src, Impl impl = Impl::Auto, LumaPlanes* planes = nullptr)
	{
		Image out(src.w * 2, src.h * 2);
		UpscaleRGBA8x2Planes(fs, src.px.data(), src.w, src.h, src.w * 4, out.px.data(), out.w * 4, impl, planes);
		return out;
	}

	Image Bilinear(const Image& src)
	{
		Image out(src.w * 2, src.h * 2);
		BilinearRGBA8x2(src.px.data(), src.w, src.h, src.w * 4, out.px.data(), out.w * 4);
		return out;
	}

	double LumaOf(const u8* p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }

	// Luma PSNR (peak 255) between two images of the same size.
	double LumaPsnr(const Image& a, const Image& b)
	{
		EXPECT_EQ(a.w, b.w);
		EXPECT_EQ(a.h, b.h);
		double sum = 0;
		for (size_t i = 0; i < static_cast<size_t>(a.w) * a.h; i++)
		{
			const double d = LumaOf(&a.px[i * 4]) - LumaOf(&b.px[i * 4]);
			sum += d * d;
		}
		const double mse = sum / (static_cast<double>(a.w) * a.h);
		return mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
	}

	// -----------------------------------------------------------------------------------------
	//  Synthetic scenes: anti-aliased shapes drawn at high resolution, then reduced
	// -----------------------------------------------------------------------------------------

	struct Rgb
	{
		double r, g, b;
	};

	using Scene = std::function<Rgb(double x, double y)>; // x, y in HR pixels

	// Average of an 8x8 grid of samples per pixel.
	Image RenderHR(u32 n, const Scene& scene)
	{
		constexpr int ss = 8;
		Image im(n, n);
		for (u32 y = 0; y < n; y++)
		{
			for (u32 x = 0; x < n; x++)
			{
				double r = 0, g = 0, b = 0;
				for (int j = 0; j < ss; j++)
				{
					for (int i = 0; i < ss; i++)
					{
						const Rgb c = scene(x + (i + 0.5) / ss, y + (j + 0.5) / ss);
						r += c.r;
						g += c.g;
						b += c.b;
					}
				}
				const double k = 1.0 / (ss * ss);
				u8* p = im.At(x, y);
				p[0] = static_cast<u8>(std::clamp(std::lround(r * k), 0l, 255l));
				p[1] = static_cast<u8>(std::clamp(std::lround(g * k), 0l, 255l));
				p[2] = static_cast<u8>(std::clamp(std::lround(b * k), 0l, 255l));
				p[3] = 255;
			}
		}
		return im;
	}

	// 2x2 area average, the degradation the filters were trained against (among others).
	Image AreaDownsample2(const Image& hr)
	{
		Image lr(hr.w / 2, hr.h / 2);
		for (u32 y = 0; y < lr.h; y++)
		{
			for (u32 x = 0; x < lr.w; x++)
			{
				for (int c = 0; c < 3; c++)
				{
					const u32 sum = hr.At(2 * x, 2 * y)[c] + hr.At(2 * x + 1, 2 * y)[c] + hr.At(2 * x, 2 * y + 1)[c] + hr.At(2 * x + 1, 2 * y + 1)[c];
					lr.At(x, y)[c] = static_cast<u8>((sum + 2) / 4);
				}
				lr.At(x, y)[3] = 255;
			}
		}
		return lr;
	}

	// Discs and a bar over a colour gradient.
	Scene ShapesScene(double n)
	{
		struct Disc
		{
			double cx, cy, r;
			Rgb c;
		};
		std::vector<Disc> discs;
		Rng rng(7);
		for (int i = 0; i < 9; i++)
		{
			Disc d;
			d.cx = rng.Uniform() * n;
			d.cy = rng.Uniform() * n;
			d.r = n * (0.04 + 0.12 * rng.Uniform());
			d.c = {rng.Uniform() * 255, rng.Uniform() * 255, rng.Uniform() * 255};
			discs.push_back(d);
		}
		return [=](double x, double y) {
			Rgb c = {60 + 120 * x / n, 80 + 90 * y / n, 140 - 60 * (x / n) * (y / n)};
			for (const Disc& d : discs)
			{
				if ((x - d.cx) * (x - d.cx) + (y - d.cy) * (y - d.cy) < d.r * d.r)
					c = d.c;
			}
			// A bar at 35 degrees, 6 pixels wide.
			const double dist = std::fabs(-std::sin(0.61) * (x - n * 0.5) + std::cos(0.61) * (y - n * 0.5));
			if (dist < 3.0)
				c = {245, 235, 40};
			return c;
		};
	}

	// Thin lines through the centre at 12 angles, 1.5 pixels wide.
	Scene LinesScene(double n)
	{
		return [=](double x, double y) {
			Rgb c = {30, 30, 40};
			const double dx = x - n * 0.5, dy = y - n * 0.5;
			if (dx * dx + dy * dy > 0.45 * n * 0.45 * n)
				return c;
			for (int k = 0; k < 12; k++)
			{
				const double phi = k * kPi / 12;
				const double dist = std::fabs(-std::sin(phi) * dx + std::cos(phi) * dy);
				if (dist < 0.75)
					c = {235, 235, 235};
			}
			return c;
		};
	}

	// Axis-aligned cells on the left, cells rotated 30 degrees on the right.
	Scene CheckerScene(double n)
	{
		return [=](double x, double y) {
			double u = x, v = y;
			double cell = 6;
			if (x >= n * 0.5)
			{
				const double ca = std::cos(kPi / 6), sa = std::sin(kPi / 6);
				u = ca * (x - n * 0.5) - sa * y;
				v = sa * (x - n * 0.5) + ca * y;
				cell = 10;
			}
			const long parity = static_cast<long>(std::floor(u / cell)) + static_cast<long>(std::floor(v / cell));
			return (parity & 1) ? Rgb{240, 240, 240} : Rgb{25, 25, 30};
		};
	}

	// Rows of 5x7 block glyphs, 3 pixels per block.
	Scene GlyphScene(double n)
	{
		(void)n;
		return [=](double x, double y) {
			constexpr double scale = 3;
			const double gx = x / scale, gy = y / scale;
			const long col = static_cast<long>(std::floor(gx)), row = static_cast<long>(std::floor(gy));
			const long glyph_col = col / 6, glyph_row = row / 8;
			const long cx = col % 6, cy = row % 8;
			Rgb c = {232, 226, 208};
			if (cx < 5 && cy < 7)
			{
				u32 h = static_cast<u32>(glyph_col * 7919 + glyph_row * 104729 + cx * 31 + cy * 17 + 1);
				h ^= h << 13;
				h ^= h >> 17;
				h ^= h << 5;
				h *= 2654435761u;
				if ((h >> 28) < 9)
					c = {20, 20, 45};
			}
			return c;
		};
	}

	struct SceneCase
	{
		const char* name;
		Scene (*make)(double);
	};

	const SceneCase kScenes[] = {
		{"shapes", ShapesScene},
		{"lines", LinesScene},
		{"checker", CheckerScene},
		{"glyphs", GlyphScene},
	};

	Image MakeLowRes(const SceneCase& sc, u32 hr_size)
	{
		return AreaDownsample2(RenderHR(hr_size, sc.make(hr_size)));
	}
} // namespace

// ---------------------------------------------------------------------------------------------
//  Filter files
// ---------------------------------------------------------------------------------------------

TEST(GSTextureUpscalerFilters, RealFilesLoad)
{
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);

	const auto file = ReadFileBytes(SetDir(kShippedSet) + "/filterbin_2_8");
	ASSERT_EQ(file.size(), 16u + kHashCount * kPixelPhases * kFilterTaps * 4u);

	// Taps land where the file puts them, rows padded to 12 with a zero.
	for (u32 hash : {0u, 1u, 100u, 215u})
	{
		for (u32 phase = 0; phase < kPixelPhases; phase++)
		{
			const float* f = fs->Filter(hash, phase);
			const u8* src = file.data() + 16 + (static_cast<size_t>(hash) * kPixelPhases + phase) * kFilterTaps * 4;
			double dc = 0;
			for (u32 row = 0; row < kPatchSize; row++)
			{
				for (u32 col = 0; col < kPatchSize; col++)
				{
					float expect;
					std::memcpy(&expect, src + (row * kPatchSize + col) * 4, 4);
					ASSERT_EQ(f[row * kFilterRowStride + col], expect);
					dc += expect;
				}
				ASSERT_EQ(f[row * kFilterRowStride + kPatchSize], 0.0f);
			}
			EXPECT_NEAR(dc, 1.0, 0.02) << "hash " << hash << " phase " << phase;
		}
	}

	// Edges as in the Qfactor files: the tertiles of the strength and coherence the training
	// pixels had.
	EXPECT_FLOAT_EQ(fs->StrengthEdges()[0], 0.000045414796f);
	EXPECT_FLOAT_EQ(fs->StrengthEdges()[1], 0.000415970193f);
	EXPECT_FLOAT_EQ(fs->CoherenceEdges()[0], 0.207463458180f);
	EXPECT_FLOAT_EQ(fs->CoherenceEdges()[1], 0.434347659349f);
}

// The shipped files are read by the real loader on every machine, so check them as files: the
// header, the size, every tap and the bucket edges. The loader's own tests use good files made up
// here and say nothing about these.
TEST(GSTextureUpscalerFilters, ShippedSetHasASaneHeaderAndBucketEdges)
{
	const std::string dir = SetDir(kShippedSet);
	const auto file = ReadFileBytes(dir + "/filterbin_2_8");
	ASSERT_EQ(file.size(), 16u + kHashCount * kPixelPhases * kFilterTaps * 4u);
	EXPECT_EQ(0, std::memcmp(file.data(), "fp32", 4));
	auto le32 = [&](size_t off) {
		u32 v;
		std::memcpy(&v, file.data() + off, 4);
		return v;
	};
	EXPECT_EQ(le32(4), kHashCount);
	EXPECT_EQ(le32(8), kPixelPhases);
	EXPECT_EQ(le32(12), kFilterTaps);

	// Every filter: finite taps of a plausible size that add up to about 1, so a flat patch keeps
	// its brightness. A filter of zeros or NaNs is not what we trained.
	double worst_dc = 0, max_tap = 0;
	for (u32 f = 0; f < kHashCount * kPixelPhases; f++)
	{
		double dc = 0;
		for (u32 t = 0; t < kFilterTaps; t++)
		{
			float v;
			std::memcpy(&v, file.data() + 16 + (static_cast<size_t>(f) * kFilterTaps + t) * 4, 4);
			ASSERT_TRUE(std::isfinite(v)) << "filter " << f << " tap " << t;
			dc += v;
			max_tap = std::max(max_tap, std::fabs(static_cast<double>(v)));
		}
		worst_dc = std::max(worst_dc, std::fabs(dc - 1.0));
		ASSERT_NEAR(dc, 1.0, 0.02) << "filter " << f << " (hash " << f / kPixelPhases << " phase " << f % kPixelPhases << ")";
	}
	EXPECT_LT(max_tap, 1.0);
	std::printf("shipped set: worst DC error %.4f, largest tap %.3f\n", worst_dc, max_tap);

	// Bucket edges: two per file, positive and strictly ascending, coherence a ratio below 1. The
	// loader rejects malformed ones; what it cannot know is whether the strength and coherence
	// files were swapped. The two have different scales: strength is gradient energy, whose
	// tertiles on real textures are far below 1, and coherence is spread over 0..1.
	std::string error;
	const auto fs = FilterSet::Load(dir, &error);
	ASSERT_TRUE(fs) << error;
	const float* se = fs->StrengthEdges();
	const float* ce = fs->CoherenceEdges();
	EXPECT_GT(se[0], 0.0f);
	EXPECT_LT(se[0], se[1]);
	EXPECT_LT(se[1], 0.05f);
	EXPECT_GT(ce[0], 0.05f);
	EXPECT_LT(ce[0], ce[1]);
	EXPECT_LT(ce[1], 1.0f);

	// The edges have to split real content into all three strength and all three coherence
	// buckets, or two thirds of the filters would never run.
	bool strength_used[3] = {}, coherence_used[3] = {};
	for (const SceneCase& sc : kScenes)
	{
		LumaPlanes planes;
		Upscale(*fs, MakeLowRes(sc, 128), Impl::Auto, &planes);
		for (const s32 hash : planes.hash)
		{
			strength_used[(hash % 9) / 3] = true;
			coherence_used[hash % 3] = true;
		}
	}
	for (int i = 0; i < 3; i++)
	{
		EXPECT_TRUE(strength_used[i]) << "strength bucket " << i << " is never reached";
		EXPECT_TRUE(coherence_used[i]) << "coherence bucket " << i << " is never reached";
	}
}

TEST(GSTextureUpscalerFilters, RejectsBadFilterBin)
{
	const auto good = ReadFileBytes(SetDir(kShippedSet) + "/filterbin_2_8");
	const std::string str = ReadFileText(SetDir(kShippedSet) + "/Qfactor_strbin_2_8");
	const std::string coh = ReadFileText(SetDir(kShippedSet) + "/Qfactor_cohbin_2_8");
	std::string error;
	ASSERT_TRUE(FilterSet::LoadFromMemory(good, str, coh, &error)) << error;

	auto expect_rejected = [&](const std::vector<u8>& bytes, const char* what) {
		std::string err;
		EXPECT_FALSE(FilterSet::LoadFromMemory(bytes, str, coh, &err)) << what;
		EXPECT_FALSE(err.empty()) << what << ": no error text";
		// A null error pointer must be fine too.
		EXPECT_FALSE(FilterSet::LoadFromMemory(bytes, str, coh, nullptr)) << what;
	};

	expect_rejected({}, "empty");
	expect_rejected(std::vector<u8>(good.begin(), good.begin() + 15), "shorter than the header");
	expect_rejected(std::vector<u8>(good.begin(), good.begin() + 16), "header only");
	expect_rejected(std::vector<u8>(good.begin(), good.end() - 4), "one float short");
	expect_rejected(std::vector<u8>(good.begin(), good.end() - 1), "one byte short");
	{
		auto b = good;
		b.push_back(0);
		expect_rejected(b, "one byte long");
	}
	{
		auto b = good;
		b.insert(b.end(), good.begin() + 16, good.begin() + 16 + 484);
		expect_rejected(b, "one filter long");
	}
	{
		auto b = good;
		std::memcpy(b.data(), "fp16", 4);
		expect_rejected(b, "wrong magic");
	}
	{
		auto b = good;
		b[0] = 'F';
		expect_rejected(b, "wrong magic case");
	}
	for (int field = 0; field < 3; field++)
	{
		auto b = good;
		b[4 + field * 4] ^= 1; // hash count, phases, taps
		expect_rejected(b, "wrong header count");
	}
	{
		auto b = good;
		const float nan = std::nanf("");
		std::memcpy(b.data() + 16 + 4000, &nan, 4);
		expect_rejected(b, "NaN tap");
	}
	{
		auto b = good;
		const float inf = INFINITY;
		std::memcpy(b.data() + 16, &inf, 4);
		expect_rejected(b, "infinite tap");
	}
}

TEST(GSTextureUpscalerFilters, RejectsBadBinEdges)
{
	const auto bin = ReadFileBytes(SetDir(kShippedSet) + "/filterbin_2_8");
	const std::string good_str = "0.001269\n0.022169\n";
	const std::string good_coh = "0.192916\n0.405942\n";
	std::string error;
	ASSERT_TRUE(FilterSet::LoadFromMemory(bin, good_str, good_coh, &error)) << error;

	// which: 0 = bad strength file (coherence file good), 1 = bad coherence file.
	auto rejected = [&](int which, const std::string& s, const char* what) {
		std::string err;
		EXPECT_FALSE(FilterSet::LoadFromMemory(bin, which == 0 ? s : good_str, which == 1 ? s : good_coh, &err)) << what;
		EXPECT_FALSE(err.empty()) << what;
	};
	for (int which = 0; which < 2; which++)
	{
		rejected(which, "", "empty");
		rejected(which, "0.1\n", "one edge");
		rejected(which, "0.1 0.2 0.3\n", "three edges");
		rejected(which, "0.1 abc\n", "not a number");
		rejected(which, "0.1 0.2x\n", "trailing junk");
		rejected(which, "0.1 ..2\n", "double dot");
		rejected(which, "0.1 1e\n", "dangling exponent");
		rejected(which, "0.1 -\n", "bare sign");
		rejected(which, "0.1 1e999\n", "overflow");
		rejected(which, "0.2 0.1\n", "descending");
		rejected(which, "-0.1 0.1\n", "negative");
	}
	rejected(1, "0.5 1.5\n", "coherence above 1");

	// Whitespace and notation that should be accepted.
	std::string err;
	auto fs = FilterSet::LoadFromMemory(bin, "  1.269e-3\r\n+2.2169E-2\t", "0.192916 0.405942", &err);
	ASSERT_TRUE(fs) << err;
	EXPECT_FLOAT_EQ(fs->StrengthEdges()[0], 0.001269f);
	EXPECT_FLOAT_EQ(fs->StrengthEdges()[1], 0.022169f);
	EXPECT_TRUE(FilterSet::LoadFromMemory(bin, "0.1 0.1", "0 1", &err)) << err;
}

TEST(GSTextureUpscalerFilters, LoadReportsMissingFile)
{
	std::string error;
	EXPECT_FALSE(FilterSet::Load(SetDir("no-such-set"), &error));
	EXPECT_NE(error.find("filterbin_2_8"), std::string::npos) << error;
	EXPECT_FALSE(FilterSet::Load(SetDir("no-such-set"), nullptr));
}

// ---------------------------------------------------------------------------------------------
//  Structure tensor and hash
// ---------------------------------------------------------------------------------------------

TEST(GSTextureUpscalerHash, StructureKernelIsIntelsGaussian)
{
	const auto g = StructureKernel();
	double sum = 0;
	for (u32 i = 0; i < kPatchSize; i++)
	{
		EXPECT_EQ(g[i], g[kPatchSize - 1 - i]);
		sum += g[i];
	}
	EXPECT_NEAR(sum, 1.0, 1e-6);
	EXPECT_NEAR(g[5], 0.2006, 1e-4);

	// Intel's 6-digit table gGaussian2DOriginal (Raisr_globals.h): row 0, and the centre row.
	const double row0[11] = {7.76554e-05, 0.000239195, 0.0005738, 0.001072, 0.00155975, 0.00176743, 0.00155975, 0.001072, 0.0005738, 0.000239195, 7.76554e-05};
	const double row5[11] = {0.00176743, 0.00544406, 0.0130596, 0.0243986, 0.0354998, 0.0402265, 0.0354998, 0.0243986, 0.0130596, 0.00544406, 0.00176743};
	const double row3[6] = {0.001072, 0.00330199, 0.00792107, 0.0147985, 0.0215317, 0.0243986};
	for (u32 j = 0; j < 11; j++)
	{
		EXPECT_NEAR(static_cast<double>(g[0]) * g[j], row0[j], row0[j] * 2e-5) << j;
		EXPECT_NEAR(static_cast<double>(g[5]) * g[j], row5[j], row5[j] * 2e-5) << j;
	}
	for (u32 j = 0; j < 6; j++)
		EXPECT_NEAR(static_cast<double>(g[3]) * g[j], row3[j], row3[j] * 2e-5) << j;
}

namespace
{
	struct RefHash
	{
		int hash;
		bool ambiguous; // some decision is within float rounding of a boundary
	};

	int AngleBin(double b, double x)
	{
		double angle = b != 0 ? std::atan2(b, x) : 0.0;
		if (angle < 0)
			angle += kPi;
		return std::clamp(static_cast<int>(std::floor(angle / (kPi / 24))), 0, 23);
	}

	// The definition in DESIGN.md, in double, with the decisions that float rounding could flip
	// marked ambiguous.
	RefHash ReferenceHash(const FilterSet& fs, double a, double b, double d)
	{
		const double T = a + d;
		const double D = a * d - b * b;
		const double s = std::sqrt(std::max(0.0, T * T / 4 - D));
		const double L1 = T / 2 + s;
		const double L2 = std::max(0.0, T / 2 - s);
		RefHash r = {0, false};

		// Angle: ambiguous if nudging x by float cancellation error changes the bin.
		const double x = (b != 0) ? L1 - d : 1.0;
		const double eps = 4e-6 * L1;
		const int bin = AngleBin(b, x);
		if (b != 0 && (AngleBin(b, x + eps) != bin || AngleBin(b, x - eps) != bin))
			r.ambiguous = true;

		int strength = 0;
		for (int i = 0; i < 2; i++)
		{
			const double e = fs.StrengthEdges()[i];
			if (L1 > e)
				strength++;
			if (std::fabs(L1 - e) < 1e-5 * e)
				r.ambiguous = true;
		}

		const double den = std::sqrt(L1) + std::sqrt(L2);
		const double coh = den > 0 ? (std::sqrt(L1) - std::sqrt(L2)) / den : 0.0;
		int coherence = 0;
		for (int i = 0; i < 2; i++)
		{
			const double e = fs.CoherenceEdges()[i];
			if (coh > e)
				coherence++;
			if (std::fabs(coh - e) < 1e-4)
				r.ambiguous = true;
		}
		r.hash = bin * 9 + strength * 3 + coherence;
		return r;
	}
} // namespace

TEST(GSTextureUpscalerHash, MatchesAtan2Definition)
{
	for (const char* set : {kShippedSet})
	{
		SCOPED_TRACE(set);
		const auto fs = LoadSet(set);
		ASSERT_TRUE(fs);
		Rng rng(1234);
		int ambiguous = 0, checked = 0;
		const int total = 300000;
		for (int i = 0; i < total; i++)
		{
			// Energies spread over the range real textures produce (strength edges are 1e-3..2e-2).
			const float a = static_cast<float>(std::exp(std::log(1e-6) + rng.Uniform() * (std::log(0.3) - std::log(1e-6))));
			const float d = static_cast<float>(std::exp(std::log(1e-6) + rng.Uniform() * (std::log(0.3) - std::log(1e-6))));
			float b = static_cast<float>((rng.Uniform() * 2 - 1) * std::sqrt(static_cast<double>(a) * d));
			if (i % 16 == 0)
				b = 0; // axis-aligned gradients
			if (i % 16 == 1)
				b = static_cast<float>(std::sqrt(static_cast<double>(a) * d)); // fully coherent
			const RefHash ref = ReferenceHash(*fs, a, b, d);
			if (ref.ambiguous)
			{
				ambiguous++;
				continue;
			}
			checked++;
			const s32 got = HashTensor(*fs, a, b, d);
			if (got != ref.hash)
			{
				ADD_FAILURE() << "a=" << a << " b=" << b << " d=" << d << " got " << got << " expected " << ref.hash;
				return;
			}
		}
		EXPECT_LT(ambiguous, total / 20) << "too many ambiguous samples; the check is not testing much";
		EXPECT_GT(checked, total * 9 / 10);
	}
}

TEST(GSTextureUpscalerHash, EveryAngleBinIsReachableInOrder)
{
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);
	// A gradient pointing theta from the vertical axis towards the horizontal one has
	// a = cos^2, b = sin cos, d = sin^2 (a is the vertical-gradient energy). Its angle is theta.
	const float strength = 0.25f; // strongest bucket
	for (int k = 0; k < 24; k++)
	{
		const double theta = (k + 0.5) * kPi / 24; // bin centre
		const float a = static_cast<float>(strength * std::cos(theta) * std::cos(theta));
		const float d = static_cast<float>(strength * std::sin(theta) * std::sin(theta));
		const float b = static_cast<float>(strength * std::sin(theta) * std::cos(theta));
		EXPECT_EQ(HashTensor(*fs, a, b, d), k * 9 + 2 * 3 + 2) << "theta " << theta * 180 / kPi;
	}
	// Mirror image (b negated) is the angle pi - theta.
	for (int k = 0; k < 24; k++)
	{
		const double theta = (k + 0.5) * kPi / 24;
		const float a = static_cast<float>(strength * std::cos(theta) * std::cos(theta));
		const float d = static_cast<float>(strength * std::sin(theta) * std::sin(theta));
		const float b = -static_cast<float>(strength * std::sin(theta) * std::cos(theta));
		EXPECT_EQ(HashTensor(*fs, a, b, d), (23 - k) * 9 + 2 * 3 + 2) << "theta " << theta * 180 / kPi;
	}

	// A flat patch is bucket 0.
	EXPECT_EQ(HashTensor(*fs, 0, 0, 0), 0);
	// Strength and coherence buckets: isotropic energy is incoherent, one-sided is coherent.
	EXPECT_EQ(HashTensor(*fs, 0.1f, 0, 0.1f) % 9, 2 * 3 + 0);
	EXPECT_EQ(HashTensor(*fs, 0.00001f, 0, 0.00001f) % 9, 0);
}

// An image whose luma is a plane 128 + m * (sin(theta) * x + cos(theta) * y): a gradient at
// angle theta from the vertical axis, for the engine's own gradient and window code.
TEST(GSTextureUpscalerHash, RampGradientsLandInTheirAngleBin)
{
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);
	constexpr u32 n = 48;
	constexpr double m = 3.0;
	for (int k = 0; k < 24; k++)
	{
		const double theta = (k + 0.5) * kPi / 24;
		Image im(n, n);
		for (u32 y = 0; y < n; y++)
		{
			for (u32 x = 0; x < n; x++)
			{
				const double v = 128 + m * (std::sin(theta) * (x - n / 2.0) + std::cos(theta) * (y - n / 2.0));
				u8* p = im.At(x, y);
				p[0] = p[1] = p[2] = static_cast<u8>(std::clamp(std::lround(v), 0l, 255l));
				p[3] = 255;
			}
		}
		for (Impl impl : {Impl::Scalar, Impl::Auto})
		{
			LumaPlanes planes;
			Upscale(*fs, im, impl, &planes);
			// Away from the border, where the 11x11 window sees only the ramp.
			for (u32 y = 40; y < 56; y++)
			{
				for (u32 x = 40; x < 56; x++)
				{
					const s32 hash = planes.hash[static_cast<size_t>(y) * planes.width + x];
					ASSERT_EQ(hash / 9, k) << "theta " << theta * 180 / kPi << " at " << x << "," << y << " impl " << static_cast<int>(impl);
				}
			}
		}
	}
}

TEST(GSTextureUpscalerHash, AxisAlignedStepEdgesAreAngleBinZero)
{
	// An exactly horizontal or vertical edge has b == 0 exactly, and Intel hashes b == 0 to angle 0
	// (Raisr.cpp:833-848). So both land in bin 0, with the strongest strength and coherence.
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);
	constexpr u32 n = 32;
	Image horizontal(n, n), vertical(n, n);
	for (u32 y = 0; y < n; y++)
	{
		for (u32 x = 0; x < n; x++)
		{
			const u8 hv = y < n / 2 ? 40 : 210;
			const u8 vv = x < n / 2 ? 40 : 210;
			u8* p = horizontal.At(x, y);
			p[0] = p[1] = p[2] = hv;
			p[3] = 255;
			p = vertical.At(x, y);
			p[0] = p[1] = p[2] = vv;
			p[3] = 255;
		}
	}
	for (Impl impl : {Impl::Scalar, Impl::Auto})
	{
		LumaPlanes ph, pv;
		Upscale(*fs, horizontal, impl, &ph);
		Upscale(*fs, vertical, impl, &pv);
		const u32 W = ph.width;
		// The edge is between output rows/columns 31 and 32.
		for (u32 i = 20; i < 44; i++)
		{
			EXPECT_EQ(ph.hash[static_cast<size_t>(31) * W + i], 0 * 9 + 2 * 3 + 2) << i;
			EXPECT_EQ(ph.hash[static_cast<size_t>(32) * W + i], 0 * 9 + 2 * 3 + 2) << i;
			EXPECT_EQ(pv.hash[static_cast<size_t>(i) * W + 31], 0 * 9 + 2 * 3 + 2) << i;
			EXPECT_EQ(pv.hash[static_cast<size_t>(i) * W + 32], 0 * 9 + 2 * 3 + 2) << i;
		}
	}

	// Tilt the vertical edge by one texel every 8 rows: it leaves bin 0 for the near-vertical bins.
	Image tilted(n * 2, n * 2);
	for (u32 y = 0; y < n * 2; y++)
	{
		for (u32 x = 0; x < n * 2; x++)
		{
			const u8 v = (x + y / 8) < n ? 40 : 210;
			u8* p = tilted.At(x, y);
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	}
	LumaPlanes pt;
	Upscale(*fs, tilted, Impl::Scalar, &pt);
	int near_vertical = 0, axis_aligned = 0, total = 0;
	for (u32 y = 40; y < 90; y++)
	{
		for (u32 x = 0; x < pt.width; x++)
		{
			const s32 hash = pt.hash[static_cast<size_t>(y) * pt.width + x];
			if (hash % 9 != 8)
				continue;
			total++;
			const int bin = hash / 9;
			near_vertical += (bin >= 9 && bin <= 15) ? 1 : 0;
			// Where the edge runs straight for a whole 11 row window, b is exactly 0 again.
			axis_aligned += (bin == 0) ? 1 : 0;
		}
	}
	EXPECT_GT(total, 0);
	EXPECT_EQ(near_vertical + axis_aligned, total) << "strong coherent pixels on a near-vertical edge are in bins 9..15, or bin 0 where b is exactly 0";
	EXPECT_GT(near_vertical, total * 7 / 10);
}

// ---------------------------------------------------------------------------------------------
//  Whole-image behaviour
// ---------------------------------------------------------------------------------------------

TEST(GSTextureUpscaler, ConstantImagesStayConstant)
{
	const struct
	{
		u8 r, g, b, a;
	} colours[] = {
		{0, 0, 0, 255},
		{255, 255, 255, 255},
		{1, 1, 1, 255},
		{127, 127, 127, 255},
		{128, 128, 128, 0},
		{254, 254, 254, 255},
		{200, 100, 50, 77},
		{10, 250, 130, 255},
		{255, 0, 0, 255},
		{0, 0, 255, 128},
	};
	for (const char* set : {kShippedSet})
	{
		const auto fs = LoadSet(set);
		ASSERT_TRUE(fs);
		for (const auto& c : colours)
		{
			SCOPED_TRACE(std::string(set) + " " + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a));
			for (u32 size : {1u, 5u, 24u})
			{
				const Image src = Constant(size, size, c.r, c.g, c.b, c.a);
				for (Impl impl : {Impl::Scalar, Impl::Auto})
				{
					LumaPlanes planes;
					const Image out = Upscale(*fs, src, impl, &planes);
					for (u32 i = 0; i < out.w * out.h; i++)
					{
						// The flat bucket's filter adds up to 0.9976, not 1 (the packs it was fitted
						// to are a hair darker than the native textures), so the engine alone can
						// land one level low: 255 comes out 254. Alpha is not filtered.
						ASSERT_NEAR(out.px[i * 4 + 0], c.r, 1) << i;
						ASSERT_NEAR(out.px[i * 4 + 1], c.g, 1) << i;
						ASSERT_NEAR(out.px[i * 4 + 2], c.b, 1) << i;
						ASSERT_EQ(out.px[i * 4 + 3], c.a) << i;
						ASSERT_EQ(planes.hash[i], 0) << i; // no gradient anywhere: the flat bucket
					}
				}

				// The pass the emulator runs clamps to the source texels' range, which puts a flat
				// patch back exactly.
				Image pass(size * 2, size * 2);
				GSTextureUpscaleSupport::UpscalePass2x(*fs, src.px.data(), size, size, size * 4, pass.px.data(), pass.w * 4);
				for (u32 i = 0; i < pass.w * pass.h; i++)
				{
					ASSERT_EQ(pass.px[i * 4 + 0], c.r) << i;
					ASSERT_EQ(pass.px[i * 4 + 1], c.g) << i;
					ASSERT_EQ(pass.px[i * 4 + 2], c.b) << i;
					ASSERT_EQ(pass.px[i * 4 + 3], c.a) << i;
				}
			}
		}
	}
}

TEST(GSTextureUpscaler, AlphaStaysInsideTheSourceRange)
{
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);
	for (const auto& range : {std::pair<u8, u8>{0, 255}, {100, 140}, {200, 201}, {0, 1}, {254, 255}})
	{
		for (u32 size : {3u, 17u, 40u})
		{
			SCOPED_TRACE(std::to_string(range.first) + ".." + std::to_string(range.second) + " size " + std::to_string(size));
			// Strong colour contrast so the luma filter overshoots; alpha must not follow it.
			const Image src = RandomImage(size, size, 99 + size, range.first, range.second);
			const Image out = Upscale(*fs, src);
			const Image bil = Bilinear(src);
			u8 lo = 255, hi = 0;
			for (u32 i = 0; i < size * size; i++)
			{
				lo = std::min(lo, src.px[i * 4 + 3]);
				hi = std::max(hi, src.px[i * 4 + 3]);
			}
			for (u32 i = 0; i < out.w * out.h; i++)
			{
				const u8 a = out.px[i * 4 + 3];
				ASSERT_GE(a, lo) << i;
				ASSERT_LE(a, hi) << i;
				ASSERT_EQ(a, bil.px[i * 4 + 3]) << "alpha is the plain bilinear upscale, pixel " << i;
			}
		}
	}
}

TEST(GSTextureUpscaler, HardEdgeOvershootIsRemovedByTheRangeClamp)
{
	// Two flat colours meeting at a vertical edge, the shape of a swatch in a texture atlas. The
	// engine is a sharpening filter and writes values beyond both colours beside the edge; the
	// clamp the texture cache applies to its output must take every one of them back inside, and
	// leave the flat parts exactly flat.
	constexpr u32 kSize = 24;
	constexpr u8 kDark = 113;
	constexpr u8 kLight = 223;
	Image src(kSize, kSize);
	for (u32 y = 0; y < kSize; y++)
	{
		for (u32 x = 0; x < kSize; x++)
		{
			const u8 v = (x < kSize / 2) ? kDark : kLight;
			u8* p = src.At(x, y);
			p[0] = p[1] = p[2] = v;
			p[3] = 128;
		}
	}

	for (const char* set : {kShippedSet})
	{
		SCOPED_TRACE(set);
		const auto fs = LoadSet(set);
		ASSERT_TRUE(fs);

		Image out = Upscale(*fs, src);
		u8 lo = 255, hi = 0;
		for (u32 i = 0; i < out.w * out.h; i++)
		{
			lo = std::min(lo, out.px[i * 4]);
			hi = std::max(hi, out.px[i * 4]);
		}
		// The set overshoots here. If it ever stopped, the checks below would pass without the
		// clamp doing anything, so say so rather than pass.
		ASSERT_TRUE(lo < kDark || hi > kLight) << "the engine no longer overshoots this edge: " << int(lo) << ".." << int(hi);

		GSTextureUpscaleSupport::ClampUpscaledToSourceRange(src.px.data(), src.w, src.h, src.w * 4, out.px.data(), out.w * 4);

		for (u32 y = 0; y < out.h; y++)
		{
			for (u32 x = 0; x < out.w; x++)
			{
				const u8* p = out.At(x, y);
				ASSERT_GE(p[0], kDark) << x << "," << y;
				ASSERT_LE(p[0], kLight) << x << "," << y;
				// Output columns 23 and 24 are the two whose source texels span the edge.
				if (x < kSize - 1)
					ASSERT_EQ(p[0], kDark) << x << "," << y;
				if (x > kSize)
					ASSERT_EQ(p[0], kLight) << x << "," << y;
				ASSERT_EQ(p[3], 128);
			}
		}
	}
}

TEST(GSTextureUpscaler, NeonMatchesScalar)
{
	if (!NeonAvailable())
		GTEST_SKIP() << "no NEON path in this build";

	std::vector<Image> images;
	// Odd and tiny sizes exercise the vector tails and the padding.
	for (const auto& s : {std::pair<u32, u32>{1, 1}, {1, 7}, {7, 1}, {2, 2}, {3, 5}, {5, 3}, {9, 4}, {11, 11}, {13, 17}, {31, 33}, {64, 70}, {100, 9}})
		images.push_back(RandomImage(s.first, s.second, s.first * 131 + s.second));
	for (const SceneCase& sc : kScenes)
		images.push_back(MakeLowRes(sc, 128));
	images.push_back(Constant(20, 20, 77, 77, 77, 255));
	// More than one band tall, and a width that is not a multiple of 4 after doubling.
	images.push_back(RandomImage(45, 150, 5));

	for (const char* set : {kShippedSet})
	{
		const auto fs = LoadSet(set);
		ASSERT_TRUE(fs);
		for (size_t i = 0; i < images.size(); i++)
		{
			SCOPED_TRACE(std::string(set) + " image " + std::to_string(i) + " " + std::to_string(images[i].w) + "x" + std::to_string(images[i].h));
			LumaPlanes ps, pn;
			const Image a = Upscale(*fs, images[i], Impl::Scalar, &ps);
			const Image b = Upscale(*fs, images[i], Impl::Neon, &pn);
			ASSERT_EQ(ps.cheap.size(), pn.cheap.size());
			// The two paths do the same float operations in the same order, so they agree exactly;
			// the requirement is within 1 in 255, which this is much tighter than.
			EXPECT_EQ(0, std::memcmp(ps.cheap.data(), pn.cheap.data(), ps.cheap.size() * 4)) << "cheap luma";
			EXPECT_EQ(0, std::memcmp(ps.hash.data(), pn.hash.data(), ps.hash.size() * 4)) << "hash";
			EXPECT_EQ(0, std::memcmp(ps.raisr.data(), pn.raisr.data(), ps.raisr.size() * 4)) << "raisr";
			EXPECT_EQ(0, std::memcmp(ps.blended.data(), pn.blended.data(), ps.blended.size() * 4)) << "blended luma";
			int max_diff = 0;
			for (size_t j = 0; j < a.px.size(); j++)
				max_diff = std::max(max_diff, std::abs(static_cast<int>(a.px[j]) - static_cast<int>(b.px[j])));
			EXPECT_LE(max_diff, 1);
			EXPECT_EQ(a.px, b.px);
		}
	}
}

TEST(GSTextureUpscaler, PitchesAreHonoured)
{
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);
	const Image src = RandomImage(21, 13, 4);
	for (Impl impl : {Impl::Scalar, Impl::Auto})
	{
		const Image tight = Upscale(*fs, src, impl);

		const u32 sp = src.w * 4 + 36, dp = tight.w * 4 + 52;
		std::vector<u8> padded_src(static_cast<size_t>(sp) * src.h, 0xAB);
		for (u32 y = 0; y < src.h; y++)
			std::memcpy(&padded_src[static_cast<size_t>(y) * sp], src.At(0, y), src.w * 4);
		std::vector<u8> dst(static_cast<size_t>(dp) * tight.h, 0xCD);
		UpscaleRGBA8x2Planes(*fs, padded_src.data(), src.w, src.h, sp, dst.data(), dp, impl, nullptr);
		for (u32 y = 0; y < tight.h; y++)
		{
			ASSERT_EQ(0, std::memcmp(&dst[static_cast<size_t>(y) * dp], tight.At(0, y), tight.w * 4)) << "row " << y;
			for (u32 i = tight.w * 4; i < dp; i++)
				ASSERT_EQ(dst[static_cast<size_t>(y) * dp + i], 0xCD) << "row " << y << " wrote past the row at byte " << i;
		}
	}

	// Same for the bilinear path.
	const Image bil = Bilinear(src);
	const u32 sp = src.w * 4 + 8, dp = bil.w * 4 + 8;
	std::vector<u8> padded_src(static_cast<size_t>(sp) * src.h, 0xAB);
	for (u32 y = 0; y < src.h; y++)
		std::memcpy(&padded_src[static_cast<size_t>(y) * sp], src.At(0, y), src.w * 4);
	std::vector<u8> dst(static_cast<size_t>(dp) * bil.h, 0xCD);
	BilinearRGBA8x2(padded_src.data(), src.w, src.h, sp, dst.data(), dp);
	for (u32 y = 0; y < bil.h; y++)
	{
		ASSERT_EQ(0, std::memcmp(&dst[static_cast<size_t>(y) * dp], bil.At(0, y), bil.w * 4));
		for (u32 i = bil.w * 4; i < dp; i++)
			ASSERT_EQ(dst[static_cast<size_t>(y) * dp + i], 0xCD);
	}
}

TEST(GSTextureUpscaler, EmptyImageIsANoOp)
{
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);
	u8 byte = 0x5A;
	UpscaleRGBA8x2(*fs, &byte, 0, 4, 0, &byte, 0);
	UpscaleRGBA8x2(*fs, &byte, 4, 0, 16, &byte, 32);
	BilinearRGBA8x2(&byte, 0, 0, 0, &byte, 0);
	EXPECT_EQ(byte, 0x5A);
}

TEST(GSTextureUpscaler, BilinearIsCentreAlignedAndClamped)
{
	// 4x1 source [0, 100, 200, 40] in red; the 2x grid samples at x/2 - 0.25.
	const u8 src[4 * 4] = {0, 0, 0, 255, 100, 0, 0, 255, 200, 0, 0, 255, 40, 0, 0, 255};
	u8 dst[8 * 2 * 4];
	BilinearRGBA8x2(src, 4, 1, 16, dst, 32);
	// Row 0 (y = 0) blends source row 0 with itself (clamped), so it is the horizontal result.
	const int expect[8] = {0, 25, 75, 125, 175, 160, 80, 40}; // (1*a + 3*b)/4 etc, rounded half up
	for (int x = 0; x < 8; x++)
	{
		EXPECT_EQ(dst[x * 4 + 0], expect[x]) << x;
		EXPECT_EQ(dst[x * 4 + 3], 255) << x;
		EXPECT_EQ(dst[32 + x * 4 + 0], expect[x]) << "row 1, " << x; // second output row is the same source row
	}

	// Vertical: 1x2 source [0, 100]: output rows 0, 25, 75, 100.
	const u8 col[2 * 4] = {0, 0, 0, 255, 100, 0, 0, 255};
	u8 out[2 * 4 * 4];
	BilinearRGBA8x2(col, 1, 2, 4, out, 8);
	const int vexpect[4] = {0, 25, 75, 100};
	for (int y = 0; y < 4; y++)
	{
		EXPECT_EQ(out[y * 8 + 0], vexpect[y]) << y;
		EXPECT_EQ(out[y * 8 + 4], vexpect[y]) << y;
	}

	// Rounding is half up: 1x1 of value v blends to v exactly, 2x1 [1, 2] gives 1.25 -> 1, 1.75 -> 2.
	const u8 two[2 * 4] = {1, 0, 0, 255, 2, 0, 0, 255};
	u8 o2[4 * 2 * 4];
	BilinearRGBA8x2(two, 2, 1, 8, o2, 16);
	EXPECT_EQ(o2[0], 1);
	EXPECT_EQ(o2[4], 1); // 1.25
	EXPECT_EQ(o2[8], 2); // 1.75
	EXPECT_EQ(o2[12], 2);
}

TEST(GSTextureUpscaler, ThreadsAgree)
{
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);
	std::vector<Image> srcs;
	std::vector<Image> expected;
	for (u32 i = 0; i < 6; i++)
	{
		srcs.push_back(RandomImage(30 + i * 7, 20 + i * 11, 40 + i));
		expected.push_back(Upscale(*fs, srcs.back()));
	}
	std::vector<std::vector<Image>> results(4);
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; t++)
	{
		threads.emplace_back([&, t] {
			for (int round = 0; round < 3; round++)
			{
				for (size_t i = 0; i < srcs.size(); i++)
					results[t].push_back(Upscale(*fs, srcs[(i + t) % srcs.size()]));
			}
		});
	}
	for (auto& th : threads)
		th.join();
	for (int t = 0; t < 4; t++)
	{
		ASSERT_EQ(results[t].size(), 18u);
		for (size_t k = 0; k < results[t].size(); k++)
			EXPECT_EQ(results[t][k].px, expected[((k % srcs.size()) + t) % srcs.size()].px) << "thread " << t << " result " << k;
	}
}

// ---------------------------------------------------------------------------------------------
//  Quality
// ---------------------------------------------------------------------------------------------

// RAISR has to beat plain bilinear on luma PSNR against the original high-resolution image. The
// margins are the measured ones, with headroom, because the point is to catch a wrong angle
// convention, phase, tap order or window, not to track decimals.
TEST(GSTextureUpscalerQuality, BeatsBilinearOnSyntheticImages)
{
	struct Result
	{
		double bilinear, raisr;
	};
	std::vector<Result> results;
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);

	std::printf("luma PSNR vs the high-resolution original (dB)\n");
	std::printf("  %-8s %9s %9s %9s\n", "image", "bilinear", "raisr", "gain");
	for (const SceneCase& sc : kScenes)
	{
		const Image hr = RenderHR(256, sc.make(256));
		const Image lr = AreaDownsample2(hr);
		Result r;
		r.bilinear = LumaPsnr(hr, Bilinear(lr));
		r.raisr = LumaPsnr(hr, Upscale(*fs, lr));
		std::printf("  %-8s %9.3f %9.3f %+9.3f\n", sc.name, r.bilinear, r.raisr, r.raisr - r.bilinear);
		results.push_back(r);
	}

	double mean_gain = 0;
	for (size_t i = 0; i < results.size(); i++)
	{
		mean_gain += (results[i].raisr - results[i].bilinear) / results.size();
		// No image may come out worse than bilinear by more than a hair.
		EXPECT_GT(results[i].raisr - results[i].bilinear, kMinImageGainDb) << kScenes[i].name;
	}
	std::printf("  mean gain: %+.3f dB\n", mean_gain);
	EXPECT_GT(mean_gain, kMinMeanGainDb);
}

TEST(GSTextureUpscalerTiming, QuarterMegapixelUpscale)
{
	const auto fs = LoadSet(kShippedSet);
	ASSERT_TRUE(fs);
	const Image src = MakeLowRes(kScenes[0], 512); // 256x256
	ASSERT_EQ(src.w, 256u);
	Image out(512, 512);

	auto median_ms = [&](Impl impl) {
		std::vector<double> t;
		for (int i = 0; i < 12; i++)
		{
			const auto t0 = std::chrono::steady_clock::now();
			UpscaleRGBA8x2Planes(*fs, src.px.data(), src.w, src.h, src.w * 4, out.px.data(), out.w * 4, impl, nullptr);
			const auto t1 = std::chrono::steady_clock::now();
			if (i >= 2)
				t.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
		}
		std::sort(t.begin(), t.end());
		return t[t.size() / 2];
	};
	const double scalar = median_ms(Impl::Scalar);
	std::printf("256x256 -> 512x512, single thread, median of 10: scalar %.2f ms", scalar);
	if (NeonAvailable())
		std::printf(", NEON %.2f ms", median_ms(Impl::Neon));
	std::printf("\n");
	// No assertion: wall time on a shared machine is not a test result.
}
