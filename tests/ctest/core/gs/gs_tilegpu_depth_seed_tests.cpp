// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The DEPTH seed: guest bytes -> a Z buffer's depth attachment.
//
// It exists because a game that clears its Z buffer by WRITING BYTES over it left TileGpu's depth
// image untouched. Beyond Good & Evil draws zero-coloured pixels over the Z buffer's own address as
// a PSMCT32 FRAME; the model correctly invalidated the Z plane of those pages, but a Depth surface
// had no consumer for the bytes, so with persistent targets the image kept every earlier frame's
// GEQUAL high-water mark and rejected the scene -- 18,054 black pixels by frame 3.
//
// Two things have to be right and both are pinned here.
//
// WHICH PAGES. Seeding a page whose real depth is in the IMAGE, not in the bytes, destroys it. A
// blanket "clear the depth image whenever any page is unservable" collapsed BGE's error (mad 13.34
// -> 2.61) and tripled OutRun 2006's (4.55 -> 12.87) for exactly that reason. The admission is two
// predicates: gsTilePageByteTruthReachable (will a compose put the page's REAL bytes in the ring?)
// and gsTileDepthSeedSourceMatches (were those bytes written through this Z buffer's own pixel
// space?). OutRun renders 16-bit colour over its Z buffer's address at the same base and stride --
// reachable, but a foreign view -- and it is the second predicate that refuses it.
//
// WHAT VALUE. The guest word has to land on the depth attachment exactly as the DRAW ROAD puts a
// vertex Z there, or every depth comparison after the seed is shifted. tilegpu.glsl's vertex stage
// is the definition: `float(raw_z) * exp2(-32)`, with a z that lands on the far plane pulled off it.
// The shader-text gates below hold the seed to it, and to convert.glsl's uint_to_depth24/16, which
// are the classic renderer's spelling of the same map.

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTileTypes.h"
#include "GS/Renderers/TileGpu/GSRendererTileGpu.h"

#include "gtest/gtest.h"

#include <cstdio>
#include <string>

namespace
{
	constexpr u32 kFull = GSVramModel::kFullBlockMask;
	constexpr GSTileSurfaceId kColour = 1;
	constexpr GSTileSurfaceId kDepth = 2;

	/// Four planes in one state, then poke the ones a case is about. Defaults to the ordinary "a
	/// colour target with a byte road holds this page and has not written it back yet".
	struct Planes
	{
		GSTileRingPlaneState p[kGSTilePlaneCount];

		Planes(GSTileSurfaceId owner = kColour, u32 mask = kFull, bool synced = false, bool road = true)
		{
			for (GSTileRingPlaneState& s : p)
				s = GSTileRingPlaneState{owner, mask, synced, road};
		}
		Planes& Set(u32 pi, GSTileSurfaceId owner, u32 mask, bool synced = false, bool road = true)
		{
			p[pi] = GSTileRingPlaneState{owner, mask, synced, road};
			return *this;
		}
		bool Reachable() const { return gsTilePageByteTruthReachable(p); }
	};

	constexpr GSTileSurfaceLayout Z(u32 bp, u8 bw, u8 psm)
	{
		return GSTileSurfaceLayout{bp, bw, psm, GSTileSurfaceKind::Depth};
	}
	constexpr GSTileSurfaceLayout C(u32 bp, u8 bw, u8 psm)
	{
		return GSTileSurfaceLayout{bp, bw, psm, GSTileSurfaceKind::Color};
	}

	std::string ReadShader(const char* name)
	{
		const std::string path = std::string(ARMSX2_SHADER_SOURCE_DIR) + "/vulkan/" + name;
		std::FILE* fp = std::fopen(path.c_str(), "rb");
		if (!fp)
			return {};
		std::string out;
		char buf[8192];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
			out.append(buf, n);
		std::fclose(fp);
		return out;
	}
} // namespace

// -- which pages: is the page's byte truth reachable at all? ------------------------------------

TEST(TileGpuDepthSeed, EveryPlaneCpuNewestIsReachable)
{
	// No owner anywhere: the CPU shadow is the truth and the slot's prefill carries it.
	EXPECT_TRUE(Planes(kGSTileNoSurface, 0, /*synced=*/false, /*road=*/false).Reachable());
}

TEST(TileGpuDepthSeed, AColourOwnerWithARoadIsReachable)
{
	// Its writeback composes the page. This is BGE's case: the Z plane is owned by the colour
	// surface that drew zeros over the Z buffer's address.
	EXPECT_TRUE(Planes().Reachable());
}

TEST(TileGpuDepthSeed, AnOwnerWithoutAByteRoadIsNotReachable)
{
	// A depth surface, or a base that is not page-aligned: nothing carries its pixels into the ring,
	// so the slot keeps the CPU shadow's stale bytes. Seeding from those would overwrite correct GPU
	// depth with a lie, which is the whole reason this predicate exists.
	EXPECT_FALSE(Planes(kDepth, kFull, /*synced=*/false, /*road=*/false).Reachable());
}

TEST(TileGpuDepthSeed, OneRoadlessPlaneAmongFourIsEnoughToRefuse)
{
	Planes pl;
	pl.Set(3, kDepth, kFull, /*synced=*/false, /*road=*/false);
	EXPECT_FALSE(pl.Reachable());
}

TEST(TileGpuDepthSeed, APartialClaimByARoadlessOwnerIsNotReachable)
{
	// The blocks it holds live only in its image, and a seed writes whole pages -- so seeding would
	// overwrite the good blocks with the prefill's stale ones.
	EXPECT_FALSE(Planes(kDepth, 0x0000FFFFu, /*synced=*/false, /*road=*/false).Reachable());
}

TEST(TileGpuDepthSeed, ASyncedRoadlessOwnerIsReachableBecauseTheStoreAlreadyClaimsIt)
{
	// Deliberate: once a lossy compose has marked the page synced the model's own answer is "the
	// bytes are in the store". The depth road does not undo that; what keeps it from acting on the
	// lie is the SOURCE test below, not this one.
	EXPECT_TRUE(Planes(kDepth, kFull, /*synced=*/true, /*road=*/false).Reachable());
}

TEST(TileGpuDepthSeed, ReachabilityIsExactlyTheComposeLossyRule)
{
	// gsTileComposableBlocks and this answer the same four plane states from opposite ends: a page
	// whose blocks the compose covers in full is a page nothing was lost on. Pinned together so the
	// two cannot drift into disagreeing about one owner.
	Planes ok;
	EXPECT_EQ(gsTileComposableBlocks(ok.p), kFull);
	EXPECT_TRUE(ok.Reachable());

	Planes roadless(kDepth, kFull, /*synced=*/false, /*road=*/false);
	EXPECT_EQ(gsTileComposableBlocks(roadless.p), 0u);
	EXPECT_FALSE(roadless.Reachable());
}

// -- which pages: were the bytes written through this Z buffer's pixel space? --------------------

TEST(TileGpuDepthSeed, TheSameBaseStrideAndWidthMatches)
{
	// BGE: a PSMCT32 FRAME at the Z buffer's own base and stride. Different swizzle universe, same
	// cells -- which is the case the whole road exists for.
	EXPECT_TRUE(gsTileDepthSeedSourceMatches(C(0x1cc0, 10, PSMCT32), Z(0x1cc0, 10, PSMZ32)));
	EXPECT_TRUE(gsTileDepthSeedSourceMatches(C(0x1cc0, 10, PSMCT32), Z(0x1cc0, 10, PSMZ24)));
	EXPECT_TRUE(gsTileDepthSeedSourceMatches(C(0x1cc0, 10, PSMCT16), Z(0x1cc0, 10, PSMZ16)));
	EXPECT_TRUE(gsTileDepthSeedSourceMatches(C(0x1cc0, 10, PSMCT16S), Z(0x1cc0, 10, PSMZ16S)));
}

TEST(TileGpuDepthSeed, OutRunsSixteenBitViewOfItsOwnZBufferDoesNotMatch)
{
	// The regression this predicate was written for. OutRun 2006 renders PSMCT16 colour at 0x12c0
	// bw 10 -- the same base and stride as its PSMZ24 Z buffer, but 64x64 pages against the Z
	// buffer's 64x32. Seeding depth from it put the player's car behind a page of colour.
	EXPECT_FALSE(gsTileDepthSeedSourceMatches(C(0x12c0, 10, PSMCT16), Z(0x12c0, 10, PSMZ24)));
}

TEST(TileGpuDepthSeed, AnotherDepthSurfaceNeverMatchesHoweverWellItsLayoutLinesUp)
{
	// Its pixels are a depth attachment and nothing carries them into bytes, so the ring would hold
	// the CPU shadow's stale copy. Reachability catches that while the loss HAPPENS; it stops
	// catching it once the compose has marked the page synced, and this is what covers the rest.
	EXPECT_FALSE(gsTileDepthSeedSourceMatches(Z(0x1cc0, 10, PSMZ24), Z(0x1cc0, 10, PSMZ32)));
	EXPECT_FALSE(gsTileDepthSeedSourceMatches(Z(0x1cc0, 10, PSMZ32), Z(0x1cc0, 10, PSMZ32)));
}

TEST(TileGpuDepthSeed, ADifferentBaseOrStrideDoesNotMatch)
{
	EXPECT_FALSE(gsTileDepthSeedSourceMatches(C(0x2580, 10, PSMCT32), Z(0x12c0, 10, PSMZ24)));
	EXPECT_FALSE(gsTileDepthSeedSourceMatches(C(0x12c0, 5, PSMCT32), Z(0x12c0, 10, PSMZ24)));
}

// -- which programs exist ------------------------------------------------------------------------

TEST(TileGpuDepthSeed, EveryDepthFormatAZBufferCanCarryHasItsOwnRoadIndex)
{
	// Both families, because GSState swaps ZBUF.PSM's 0x30 bit when FRAME's PSM is a depth format
	// (the Powerdrome behaviour), so a real Z buffer legitimately arrives PSMCT-swizzled.
	const u32 psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
	bool seen[kGSTileDepthRoadFormats] = {};
	for (const u32 psm : psms)
	{
		const u32 f = gsTileDepthRoadFormat(psm);
		ASSERT_LT(f, kGSTileDepthRoadFormats) << "psm " << psm;
		EXPECT_FALSE(seen[f]) << "two formats share road index " << f;
		seen[f] = true;
	}
	for (u32 f = 0; f < kGSTileDepthRoadFormats; f++)
		EXPECT_TRUE(seen[f]) << "road index " << f << " names no format";
}

TEST(TileGpuDepthSeed, AFormatNoZBufferCanCarryGetsAnOutOfRangeIndex)
{
	// Out of range rather than 0, so a format that slipped past the admission test compiles nothing
	// instead of running the CT32 program over 8-bit bytes.
	EXPECT_EQ(gsTileDepthRoadFormat(PSMT8), kGSTileDepthRoadFormats);
	EXPECT_EQ(gsTileDepthRoadFormat(PSMT4), kGSTileDepthRoadFormats);
	EXPECT_EQ(gsTileDepthRoadFormat(PSMT8H), kGSTileDepthRoadFormats);
}

TEST(TileGpuDepthSeed, TheRoadTakesADepthSurfaceWithAPageAlignedBase)
{
	EXPECT_TRUE(gsTileSurfaceHasDepthSeedRoad(Z(0x1cc0, 10, PSMZ32)));
	EXPECT_TRUE(gsTileSurfaceHasDepthSeedRoad(Z(0x12c0, 10, PSMZ24)));
	EXPECT_TRUE(gsTileSurfaceHasDepthSeedRoad(Z(0x0000, 1, PSMZ16S)));
	// A base that is not page-aligned: the seed derives a page from (row, col) off the base page,
	// which is the guest page only when the base IS one.
	EXPECT_FALSE(gsTileSurfaceHasDepthSeedRoad(Z(0x1cc1, 10, PSMZ32)));
	// A colour surface never takes it, whatever its PSM -- it has the colour seed instead.
	EXPECT_FALSE(gsTileSurfaceHasDepthSeedRoad(C(0x1cc0, 10, PSMZ32)));
	EXPECT_FALSE(gsTileSurfaceHasDepthSeedRoad(C(0x1cc0, 10, PSMCT32)));
}

TEST(TileGpuDepthSeed, TheWritebackHalfOfTheDepthGapStaysShut)
{
	// The seed direction exists now; the other one does not. Nothing may start writing a depth
	// attachment back into guest bytes on the strength of the seed road existing.
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Z(0x1cc0, 10, PSMZ32)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Z(0x1cc0, 10, PSMCT32)));
	EXPECT_FALSE(gsTileSurfaceHasByteRoad(Z(0x1cc0, 10, PSMZ16)));
}

// -- what value: the guest word -> depth map, held to the draw road's ----------------------------

TEST(TileGpuDepthSeed, TheSeedScalesTheGuestWordExactlyAsTheVertexStageDoes)
{
	const std::string draw = ReadShader("tilegpu.glsl");
	const std::string seed = ReadShader("tilegpu_seed.glsl");
	ASSERT_FALSE(draw.empty()) << "tilegpu.glsl not readable from " << ARMSX2_SHADER_SOURCE_DIR;
	ASSERT_FALSE(seed.empty());

	// The draw road's two statements, quoted so a change to either has to come here.
	EXPECT_NE(draw.find("gl_Position.z *= exp2(-32.0f);"), std::string::npos)
		<< "the vertex stage's integer-depth scale moved; the seed below mirrors it";
	EXPECT_NE(draw.find("if (gl_Position.z == gl_Position.w)\n\t\tgl_Position.z *= 0.999999f;"), std::string::npos)
		<< "the vertex stage's far-plane nudge moved; the seed below mirrors it";

	// ...and the seed's, which must be the same two numbers.
	EXPECT_NE(seed.find("const float d = float(z) * exp2(-32.0f);"), std::string::npos);
	EXPECT_NE(seed.find("gl_FragDepth = (d == 1.0f) ? (d * 0.999999f) : d;"), std::string::npos);
}

TEST(TileGpuDepthSeed, TheTwentyFourBitValueMaskIsTheClassicRenderersOwn)
{
	const std::string conv = ReadShader("convert.glsl");
	const std::string seed = ReadShader("tilegpu_seed.glsl");
	ASSERT_FALSE(conv.empty());
	ASSERT_FALSE(seed.empty());

	// convert.glsl's uint_to_depth24/16 are how the classic renderer turns the same guest words into
	// the same depth attachment. The seed masks in the same place, to the same width.
	EXPECT_NE(conv.find("return float(i & 0xFFFFFFu) * exp2(-32.0f);"), std::string::npos);
	EXPECT_NE(conv.find("return float(i & 0xFFFFu) * exp2(-32.0f);"), std::string::npos);
	EXPECT_NE(seed.find("#define TILEGPU_SEED_ZMASK 0x00FFFFFFu"), std::string::npos);
	// The 16-bit arms take a halfword out of the ring, so their width is the extract, not a mask --
	// and the shader must therefore NOT be masking them to 24 bits.
	EXPECT_NE(seed.find("#define TILEGPU_SEED_ZMASK 0xFFFFFFFFu"), std::string::npos);
}

TEST(TileGpuDepthSeed, TheShaderDistinguishesEveryDepthRoadIndexTheDeviceCanInject)
{
	// A one-off between the C++ enumeration and the shader's #if ladder would silently address one
	// format's bytes through another's tables. Every index the C++ side can emit has to be named in
	// the shader's depth block.
	const std::string seed = ReadShader("tilegpu_seed.glsl");
	ASSERT_FALSE(seed.empty());
	// The shader carries a range guard naming every index it knows; a ninth format would #error
	// rather than silently take every #else arm. Read it back here so the two lists stay one list.
	EXPECT_NE(seed.find("#error \"TILEGPU_SEED_FMT is not a gsTileDepthRoadFormat index\""), std::string::npos);
	for (u32 f = 0; f < kGSTileDepthRoadFormats; f++)
	{
		const std::string needle = "TILEGPU_SEED_FMT != " + std::to_string(f);
		EXPECT_NE(seed.find(needle), std::string::npos) << "depth road index " << f << " is not in the range guard";
	}
}

// -- the executor's contract ---------------------------------------------------------------------

TEST(TileGpuDepthSeed, TheDepthSeedIsItsOwnPrepKind)
{
	// Not a flag on Seed: the destination attachment, the pipeline and the render pass all differ,
	// and the executor picks the format enumeration off the kind.
	EXPECT_NE(GSDevice::GSTileGpuPrepKind::SeedDepth, GSDevice::GSTileGpuPrepKind::Seed);
	EXPECT_EQ(static_cast<u32>(GSDevice::GSTileGpuPrepKind::SeedDepth), 7u);
}
