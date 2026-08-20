// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Rule 3 of the TileGpu texel road: a draw samples a MATERIALISED texture source — an ordinary RGBA8
// image the frame built out of the same guest bytes the byte road decodes — through the hardware
// sampler. Four things about that road are contracts between files that cannot see each other, and
// each of them fails silently:
//
//  1. The road bits and the pipeline key. The fragment variant a pass compiles is chosen by a bit
//     mask packed into the pipeline cache's key beside the topology, the depth mode and the blend
//     equation. A third road bit that overlapped one of those would hand a pass another pass's
//     pipeline, which renders — just not what was asked for.
//  2. The sampled-binding key. The executor may not put two different descriptor-array indices in
//     one indirect call, so it cuts the call wherever a draw's (target slot, source slot) pair
//     changes. That pair is packed into one word; a packing that lost a slot would merge two draws
//     that must not share a descriptor.
//  3. The sampler a draw's TEX1/CLAMP maps to. Wrap and filter ride in the descriptor rather than in
//     the shader, so the mapping is the only place the GS registers reach the sampler at all.
//  4. THE ONE THAT COSTS PIXELS: the value a point-sampled texel comes out as. The byte road
//     computes float(k) * (1/255) in the shader; the source road gets the texture unit's unorm8
//     conversion, and those two roundings are not the same number for half the byte values. So the
//     source road recovers the byte and normalises it the byte road's way, and this file proves that
//     recovery is exact whichever conversion the hardware actually does — which is what lets a draw
//     move between the two roads without moving a pixel.

#include "GS/Renderers/Common/GSDevice.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

namespace
{
	// tilegpu.glsl's tilegpu_norm8: the one spelling of "a guest byte, normalised" that every road
	// goes through. The byte road reaches it via tilegpu_unpack.
	float Norm8(float b)
	{
		return b * (1.0f / 255.0f);
	}

	// tilegpu_source_sample's NEAREST arm, transcribed: recover the byte the image holds from
	// whatever the texture unit handed back, then normalise it the byte road's way.
	float ReDeriveByte(float sampled)
	{
		return std::floor(std::fmaf(sampled, 255.0f, 0.5f));
	}

	// The two unorm8 -> float conversions a texture unit could plausibly do. Vulkan says the value is
	// c / (2^b - 1); it does not say the hardware must get there by a division, and a multiply by the
	// reciprocal lands one ULP away for 126 of the 256 bytes.
	float UnormDiv(u32 k)
	{
		return static_cast<float>(k) / 255.0f;
	}
	float UnormMul(u32 k)
	{
		return static_cast<float>(k) * (1.0f / 255.0f);
	}

	bool SameBits(float a, float b)
	{
		u32 ba, bb;
		std::memcpy(&ba, &a, sizeof(ba));
		std::memcpy(&bb, &b, sizeof(bb));
		return ba == bb;
	}
} // namespace

// -- 1. the road bits and the pipeline key -------------------------------------------------------

TEST(TileGpuSourceRoad, RoadBitsAreDistinctAndSourceIsTheThird)
{
	EXPECT_EQ(GSDevice::kGSTileGpuRoadByte, 1u << 0);
	EXPECT_EQ(GSDevice::kGSTileGpuRoadTarget, 1u << 1);
	EXPECT_EQ(GSDevice::kGSTileGpuRoadSource, 1u << 2);
	EXPECT_EQ(GSDevice::kGSTileGpuRoadMaskAll, 7u);
	// The mask has to be exactly the union, or a device normalising a plan's mask against it would
	// quietly drop a road the renderer asked for.
	EXPECT_EQ(GSDevice::kGSTileGpuRoadMaskAll,
		GSDevice::kGSTileGpuRoadByte | GSDevice::kGSTileGpuRoadTarget | GSDevice::kGSTileGpuRoadSource);
}

TEST(TileGpuSourceRoad, RoadMaskFitsThePipelineKeyWithoutCollision)
{
	// GSDeviceVK::GetTileGpuPipeline's key, transcribed:
	//   topology<<0 (2 bits) | depth<<2 (2) | blend<<8 (7) | nocolour<<16 | noalpha<<17 | roads<<18
	// Three road bits occupy 18..20, so the fields below must not reach that far and nothing else may
	// claim 18..20. This is the check that a fourth road axis (M4's mipmapping) has to re-run.
	constexpr u32 kRoadShift = 18;
	constexpr u32 kRoadBits = 3;
	constexpr u32 kRoadField = ((1u << kRoadBits) - 1u) << kRoadShift;

	EXPECT_EQ(GSDevice::kGSTileGpuRoadMaskAll, (1u << kRoadBits) - 1u);

	const u32 topo_field = 3u << 0;
	const u32 depth_field = (GSDevice::kGSTileGpuDepthModes - 1u) << 2;
	const u32 blend_field = (3u * 3u * 3u * 3u - 1u) << 8; // GSDevice::m_blendMap index, < 81
	const u32 nocolor_field = 0x10000u;
	const u32 noalpha_field = 0x20000u;
	const u32 below = topo_field | depth_field | blend_field | nocolor_field | noalpha_field;

	EXPECT_EQ(below & kRoadField, 0u) << "a pipeline key field overlaps the road mask";
	EXPECT_LT(below, 1u << kRoadShift) << "the fields below the road mask no longer fit under it";
	// ...and the top of the road field is bit 20, so 21 up stay free for the next axis.
	EXPECT_EQ(kRoadField, 0x1C0000u);
}

// -- 2. the sampled-binding key ------------------------------------------------------------------

TEST(TileGpuSourceRoad, BindKeyCarriesBothSlotsAndSeparatesEveryPair)
{
	using Plan = GSDevice::GSTileGpuPassPlan;

	// A draw taking neither road is the all-ones key, and it must not collide with any real pair.
	const u32 none = Plan::PackBindKey(Plan::kNoTexSlot, Plan::kNoSourceSlot);
	EXPECT_EQ(none, 0xFFFFFFFFu);

	// Every (target slot, source slot) pair in range is a distinct key -- that is the whole job. A
	// packing that lost either half would let the executor put two draws needing different
	// descriptors into one indirect call, which is exactly the wave-uniformity rule it exists for.
	for (u32 t = 0; t < Plan::kMaxTexSourcesPerPass; t++)
	{
		for (u32 s = 0; s < Plan::kMaxSources; s++)
		{
			const u32 key = Plan::PackBindKey(t, s);
			EXPECT_NE(key, none);
			EXPECT_EQ(key & 0xFFFFu, t);
			EXPECT_EQ(key >> 16, s);
			// Changing either half alone changes the key.
			EXPECT_NE(key, Plan::PackBindKey(t + 1, s));
			EXPECT_NE(key, Plan::PackBindKey(t, s + 1));
		}
	}

	// A rule-2-only draw and a rule-3-only draw at the same numeric slot are different keys.
	EXPECT_NE(Plan::PackBindKey(0, Plan::kNoSourceSlot), Plan::PackBindKey(Plan::kNoTexSlot, 0));
}

TEST(TileGpuSourceRoad, SourceArrayIsFrameWideAndBiggerThanThePerPassTargetArray)
{
	using Plan = GSDevice::GSTileGpuPassPlan;
	// 128, not 64: the corpus census found GT4 and OutRun holding 64 distinct windows per frame with
	// draws refused behind them. The number is a contract -- the fragment shader declares an array of
	// exactly this size -- so it is pinned rather than left as a tunable either side can pick alone.
	EXPECT_EQ(Plan::kMaxSources, 128u);
	// Both slots have to survive PackBindKey's 16 bits.
	EXPECT_LE(Plan::kMaxSources, 0xFFFFu);
	EXPECT_LE(Plan::kMaxTexSourcesPerPass, 0xFFFFu);
	EXPECT_EQ(Plan::kNoSourceSlot, 0xFFFFFFFFu);
}

// -- 3. the sampler a draw's registers map to ----------------------------------------------------

TEST(TileGpuSourceRoad, SamplerSelectorSpellsTheEightRule3Admits)
{
	// GSRendererTileGpu::SourceSamplerKey, transcribed. Rule 3 admits REPEAT and CLAMP only (the two
	// a sampler expresses exactly at tw x th), and mipmapping is off, so eight keys cover the road.
	const auto key = [](u32 wms, u32 wmt, bool ltf) {
		GSHWDrawConfig::SamplerSelector ss;
		ss.tau = (wms == CLAMP_REPEAT) ? 1 : 0;
		ss.tav = (wmt == CLAMP_REPEAT) ? 1 : 0;
		ss.biln = ltf ? 1 : 0;
		return static_cast<u32>(ss.key);
	};

	// The eight are distinct, and they are the low three bits, which is what lets the executor index
	// its sampler table with the key directly.
	bool seen[8] = {};
	for (u32 wms : {static_cast<u32>(CLAMP_REPEAT), static_cast<u32>(CLAMP_CLAMP)})
	{
		for (u32 wmt : {static_cast<u32>(CLAMP_REPEAT), static_cast<u32>(CLAMP_CLAMP)})
		{
			for (bool ltf : {false, true})
			{
				const u32 k = key(wms, wmt, ltf);
				ASSERT_LT(k, 8u);
				EXPECT_FALSE(seen[k]) << "two register settings share a sampler slot";
				seen[k] = true;
			}
		}
	}
	for (bool s : seen)
		EXPECT_TRUE(s);

	// And the bits mean what the plan's SourceBind comment says they mean.
	EXPECT_EQ(key(CLAMP_REPEAT, CLAMP_CLAMP, false) & 1u, 1u);
	EXPECT_EQ(key(CLAMP_CLAMP, CLAMP_REPEAT, false) & 2u, 2u);
	EXPECT_EQ(key(CLAMP_CLAMP, CLAMP_CLAMP, true) & 4u, 4u);
	EXPECT_EQ(key(CLAMP_CLAMP, CLAMP_CLAMP, false), 0u);
}

TEST(TileGpuSourceRoad, SourceSamplersDoNotAskForMipFiltering)
{
	// The level-0 pin. GetSampler clamps max LOD when the selector says mipmapping is off, and rule 3
	// samples level 0 exactly as every TileGpu road does today -- choosing a GS level is M4's, and a
	// sampler that started filtering between levels would change pixels with nothing else moving.
	for (u32 i = 0; i < 8; i++)
	{
		GSHWDrawConfig::SamplerSelector ss;
		ss.tau = (i & 1u) ? 1 : 0;
		ss.tav = (i & 2u) ? 1 : 0;
		ss.biln = (i & 4u) ? 1 : 0;
		EXPECT_FALSE(ss.UseMipmapFiltering());
		EXPECT_FALSE(ss.IsMipFilterLinear());
		// min and mag follow biln alone while mipmapping is off, which is what makes the eight keys
		// exactly "wrap x wrap x filter" and nothing else.
		EXPECT_EQ(ss.IsMinFilterLinear(), (i & 4u) != 0);
		EXPECT_EQ(ss.IsMagFilterLinear(), (i & 4u) != 0);
	}
}

// -- 4. the texel value, which is where pixels get lost -------------------------------------------

TEST(TileGpuSourceRoad, TheTwoUnormConversionsGenuinelyDisagree)
{
	// The premise the re-derivation exists for. If these agreed there would be nothing to fix, and a
	// future reader would be right to delete the fma/floor from the shader -- so the disagreement is
	// pinned as a fact rather than left as an assertion in a comment.
	int differ = 0;
	for (u32 k = 0; k < 256; k++)
	{
		if (!SameBits(UnormDiv(k), UnormMul(k)))
			differ++;
	}
	EXPECT_EQ(differ, 126);
}

TEST(TileGpuSourceRoad, PointSampledSourceReproducesTheByteRoadsTexelExactly)
{
	// The gate the FP pinning bought, at the level of the arithmetic rather than of the corpus: for
	// every byte a materialised source can hold, the NEAREST source road produces the byte road's
	// float BIT for BIT -- and it does so whichever of the two conversions the texture unit performs,
	// which is what makes the identity a property of the shader instead of a property of the device
	// it was measured on.
	for (u32 k = 0; k < 256; k++)
	{
		const float byte_road = Norm8(static_cast<float>(k)); // tilegpu_unpack's value
		for (const float sampled : {UnormDiv(k), UnormMul(k)})
		{
			const float recovered = ReDeriveByte(sampled);
			ASSERT_EQ(recovered, static_cast<float>(k)) << "byte " << k << " did not survive the round trip";
			EXPECT_TRUE(SameBits(Norm8(recovered), byte_road)) << "byte " << k << " normalises differently";
		}
	}
}

TEST(TileGpuSourceRoad, TakingTheSampledValueDirectlyWouldNotHaveWorked)
{
	// The counterfactual, so the cost of dropping the re-derivation is on the record: sampling
	// straight through agrees with the byte road on only half the byte values under one of the two
	// plausible hardware conversions. That is the 1-LSB texel drift that took ten of eighteen corpus
	// dumps with it the last time an unpinned float difference got into this shader.
	int direct_ok = 0;
	for (u32 k = 0; k < 256; k++)
	{
		if (SameBits(UnormDiv(k), Norm8(static_cast<float>(k))))
			direct_ok++;
	}
	EXPECT_EQ(direct_ok, 130);
	EXPECT_LT(direct_ok, 256);
}
