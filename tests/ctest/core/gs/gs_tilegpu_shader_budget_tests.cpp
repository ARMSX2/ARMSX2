// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The TileGpu fragment program's SIZE budget, as a suite gate.
//
// The Adreno 650 flips behaviour at one threshold in a fragment program's instruction length: 123
// units is on one side, 124 on the other (1 unit = 32 dwords = 16 instructions; Turnip DMAs the
// program into a 16 KB on-chip instruction RAM and writes SP_PS_INSTR_SIZE, which the prefetcher
// reads). Crossing it swings GPU time by tens of percent on a scene, and DEAD CODE COUNTS -- four
// units of never-executed arithmetic moved SotC from 21.8 to 30.7 ms with byte-identical frames.
//
// That budget has now been breached twice by landings that never took the device stats line, and
// both times nothing said so: the byte-carrying programs drifted from 108-121 units to 260-271 over
// about a week, and the standing "SotC got slower again" was the only symptom. This file is what
// makes the next one loud. It compiles every fragment variant the planner can ask for and fails if
// one is too big.
//
// ⚠️ SPIR-V words are the PROXY, not the truth. The truth is the driver's instrlen, which only an
// a650 can report (`IR3_SHADER_DEBUG=fs`, the `dwords` field of the stats line). SPIR-V is
// pre-lowering and pre-register-allocation, so the two only track each other; the ceiling below is
// calibrated from measured pairs and carries deliberate margin, and a landing that matters still
// owes the device read. What this gate buys is that nothing crosses SILENTLY between device runs.

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Tile/GSTileSwizzleForms.h"
#include "GS/Renderers/TileGpu/GSTileGpuShaderVariant.h"

#include "common/DynamicLibrary.h"
#include "common/Error.h"

#include "shaderc/shaderc.h"

#include "gtest/gtest.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	// ------------------------------------------------------------------------------------------
	// The calibration.
	//
	// Two independent readings of the same relationship, both from byte-carrying programs measured
	// on both sides (SPIR-V words on the host, instrlen dwords on the SD865 under Turnip):
	//
	//   through the origin   28529 words / 8320 dwords = 3.43 words per dword
	//   across two commits   (28529 - 24138) / (8320 - 6944) = 3.19, intercept +1980 words
	//
	// 123 units is 3936 dwords, so the two put the cliff at 13.5k and 14.5k words. The ceiling is set
	// from the CONSERVATIVE reading (3.43) at 118 units rather than 123 -- five units of margin,
	// because the relationship is a fit over programs of one shape and a variant is a different
	// shape. The largest thing it has to admit today is byte+target+source[IDX4+PALGATHER] at 12,395
	// words, which the same reading puts at 112 units, so six units of the budget are headroom for
	// growth and five are the margin. Both halves are deliberately small: a ceiling far above what
	// the tree produces is a ceiling that never fires.
	//
	// ⚠️ The word counts here are the Honeykrisp form: TILEGPU_STATIC_BYTE_SEL is on, which spells
	// the sub-word extract as four constant shifts instead of one computed one. Turnip compiles the
	// smaller form and reads a few hundred words lower, so measuring the bigger one is the safe
	// direction -- but it does mean this gate is stricter than the device it is calibrated for.
	constexpr u32 kWordsPerDwordX100 = 343; // 3.43 SPIR-V words per device dword
	constexpr u32 kDwordsPerUnit = 32;
	constexpr u32 kCliffUnits = 123;
	constexpr u32 kBudgetUnits = 118;
	constexpr u32 kWordCeiling = kBudgetUnits * kDwordsPerUnit * kWordsPerDwordX100 / 100;

	/// The predicted a650 instrlen for a word count, under the conservative calibration. Reported
	/// beside every variant so a device run has something to confirm or refute row by row.
	u32 PredictedUnits(u32 words)
	{
		return words * 100 / kWordsPerDwordX100 / kDwordsPerUnit;
	}

	// ------------------------------------------------------------------------------------------
	// shaderc, loaded the way the Vulkan device loads it: a C ABI out of a shared library, not a
	// linked dependency. No library, no measurement -- and that is reported as a skip rather than a
	// pass, because a gate that quietly measures nothing is the thing this file exists to prevent.
	struct Shaderc
	{
		DynamicLibrary lib;
		shaderc_compiler_t compiler = nullptr;
		decltype(&::shaderc_compiler_initialize) compiler_initialize = nullptr;
		decltype(&::shaderc_compiler_release) compiler_release = nullptr;
		decltype(&::shaderc_compile_options_initialize) options_initialize = nullptr;
		decltype(&::shaderc_compile_options_release) options_release = nullptr;
		decltype(&::shaderc_compile_options_set_source_language) set_source_language = nullptr;
		decltype(&::shaderc_compile_options_set_optimization_level) set_optimization_level = nullptr;
		decltype(&::shaderc_compile_options_set_target_env) set_target_env = nullptr;
		decltype(&::shaderc_compile_into_spv) compile_into_spv = nullptr;
		decltype(&::shaderc_result_release) result_release = nullptr;
		decltype(&::shaderc_result_get_length) result_get_length = nullptr;
		decltype(&::shaderc_result_get_error_message) result_get_error_message = nullptr;
		decltype(&::shaderc_result_get_compilation_status) result_get_compilation_status = nullptr;

		~Shaderc()
		{
			if (compiler)
				compiler_release(compiler);
		}

		bool Open()
		{
			Error error;
			const std::string versioned = DynamicLibrary::GetVersionedFilename("shaderc_shared", 1);
			const std::string debian = DynamicLibrary::GetVersionedFilename("shaderc", 1);
			if (!lib.Open(versioned.c_str(), &error) && !lib.Open(debian.c_str(), &error))
				return false;
#define LOAD(member, name) \
	if (!lib.GetSymbol(#name, &member)) \
		return false;
			LOAD(compiler_initialize, shaderc_compiler_initialize)
			LOAD(compiler_release, shaderc_compiler_release)
			LOAD(options_initialize, shaderc_compile_options_initialize)
			LOAD(options_release, shaderc_compile_options_release)
			LOAD(set_source_language, shaderc_compile_options_set_source_language)
			LOAD(set_optimization_level, shaderc_compile_options_set_optimization_level)
			LOAD(set_target_env, shaderc_compile_options_set_target_env)
			LOAD(compile_into_spv, shaderc_compile_into_spv)
			LOAD(result_release, shaderc_result_release)
			LOAD(result_get_length, shaderc_result_get_length)
			LOAD(result_get_error_message, shaderc_result_get_error_message)
			LOAD(result_get_compilation_status, shaderc_result_get_compilation_status)
#undef LOAD
			compiler = compiler_initialize();
			return compiler != nullptr;
		}

		/// SPIR-V words, or 0 with `out_error` set. Compiled exactly as the device compiles a
		/// non-debug fragment module: GLSL source language, Vulkan target, performance optimisation.
		u32 FragmentWords(const std::string& source, std::string& out_error)
		{
			shaderc_compile_options_t options = options_initialize();
			set_source_language(options, shaderc_source_language_glsl);
			set_target_env(options, shaderc_target_env_vulkan, 0);
			set_optimization_level(options, shaderc_optimization_level_performance);
			const shaderc_compilation_result_t result = compile_into_spv(
				compiler, source.data(), source.length(), shaderc_glsl_fragment_shader, "source", "main", options);
			options_release(options);
			u32 words = 0;
			if (!result)
			{
				out_error = "null result object";
			}
			else
			{
				if (result_get_compilation_status(result) != shaderc_compilation_status_success)
					out_error = result_get_error_message(result);
				else
					words = static_cast<u32>(result_get_length(result) / sizeof(u32));
				result_release(result);
			}
			return words;
		}
	};

	std::string ReadTileGpuShader()
	{
		const std::string path = std::string(ARMSX2_SHADER_SOURCE_DIR) + "/vulkan/tilegpu.glsl";
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

	/// The stage header the Vulkan device puts in front of every utility fragment shader, reduced to
	/// the part tilegpu.glsl can see. The rest of GSDeviceVK::AddShaderHeader is driver-workaround
	/// and feature defines this shader never references, so leaving them out changes nothing about
	/// the program -- and pulling them in would need a live device, which a unit test does not have.
	constexpr const char* kStageHeader = "#version 460 core\n"
										 "#extension GL_EXT_samplerless_texture_functions : require\n"
										 "#define FRAGMENT_SHADER 1\n";

	struct Variant
	{
		u32 road;
		u32 texel;
		u32 words;
	};

	/// How many address GEOMETRIES the mask carries. The palette-order arm is not a geometry — it
	/// rides with whichever paletted geometry asked for it — so it does not make a pass "mixed".
	u32 GeometryCount(u32 texel_mask)
	{
		u32 n = 0;
		for (u32 a = 0; a < GSDevice::kGSTileGpuTexelArms; a++)
			n += ((texel_mask & GSDevice::kGSTileGpuTexelGeometryMask) >> a) & 1u;
		return n;
	}

	/// The masks a planner can actually produce: at least one geometry, and the palette-order arm
	/// only where a paletted geometry is present (the device normalises it away otherwise, so a
	/// module for it would never be compiled).
	bool Plannable(u32 texel_mask)
	{
		if ((texel_mask & GSDevice::kGSTileGpuTexelGeometryMask) == 0)
			return false;
		return (texel_mask & GSDevice::kGSTileGpuTexelPalGather) == 0 ||
		       (texel_mask & GSDevice::kGSTileGpuTexelPalettedMask) != 0;
	}
} // namespace

// Every fragment variant the planner can ask for, compiled and measured. The device profile is the
// one the budget is FOR: an a650 serves both bindless roads and fits the swizzle forms, so this is
// the largest program each mask can produce there -- with the Honeykrisp byte-select on top, which
// makes the reading strict rather than optimistic.
TEST(GSTileGpuShaderBudget, EveryPlannableVariantFitsTheAdreno650Budget)
{
	const GSTileSwizzleForms::FormSet forms = GSTileSwizzleForms::Fit();
	ASSERT_TRUE(forms.valid) << "the page swizzle tables stopped fitting closed forms; there is no shader to size";

	const std::string body = ReadTileGpuShader();
	ASSERT_FALSE(body.empty()) << "could not read tilegpu.glsl from " << ARMSX2_SHADER_SOURCE_DIR;

	Shaderc sc;
	if (!sc.Open())
	{
		GTEST_SKIP() << "shaderc is not loadable here, so no program can be sized. This is a SKIP, not a pass: "
						"the TileGpu size budget was NOT checked on this machine.";
	}

	const std::string prologue = kStageHeader + GSTileSwizzleForms::ShaderDefines(forms) +
	                             GSTileGpuShaderVariant::DeviceDefines(true, true, true);

	// The plannable space: a road mask without the byte bit names no arm, and one with it names at
	// least one geometry (plus, optionally, the palette-order arm). 4 + 4 * 59 = 240 programs.
	std::vector<Variant> variants;
	for (u32 road = 0; road <= GSDevice::kGSTileGpuRoadMaskAll; road++)
	{
		if ((road & GSDevice::kGSTileGpuRoadByte) == 0)
		{
			variants.push_back({road, 0, 0});
			continue;
		}
		for (u32 texel = 1u; texel <= GSDevice::kGSTileGpuTexelMaskAll; texel++)
		{
			if (Plannable(texel))
				variants.push_back({road, texel, 0});
		}
	}

	u32 worst_single = 0, worst_single_road = 0, worst_single_texel = 0;
	for (Variant& v : variants)
	{
		std::string error;
		v.words = sc.FragmentWords(prologue + GSTileGpuShaderVariant::VariantDefines(v.road, v.texel) + body, error);
		ASSERT_NE(v.words, 0u) << "variant " << GSTileGpuShaderVariant::VariantName(v.road, v.texel)
							   << " failed to compile: " << error;
		if (GeometryCount(v.texel) <= 1 && v.words > worst_single)
		{
			worst_single = v.words;
			worst_single_road = v.road;
			worst_single_texel = v.texel;
		}
	}

	// The table, printed whatever the verdict: the point of a size gate is the numbers, and a device
	// run confirms or refutes the predicted units row by row.
	std::printf("\nTileGpu fragment variant sizes (SPIR-V words, Honeykrisp form; predicted a650 units)\n");
	std::printf("  ceiling %u words = %u units (123 is the cliff; the margin is deliberate)\n", kWordCeiling,
		kBudgetUnits);
	std::printf("  %-48s %8s %7s %s\n", "variant", "words", "units", "verdict");
	u32 over_cliff = 0;
	for (const Variant& v : variants)
	{
		const u32 units = PredictedUnits(v.words);
		const bool gated = GeometryCount(v.texel) <= 1;
		const char* verdict = (units > kCliffUnits) ? "OVER THE CLIFF" : (v.words > kWordCeiling ? "over budget" : "ok");
		over_cliff += (units > kCliffUnits) ? 1 : 0;
		std::printf("  %-48s %8u %7u %s%s\n", GSTileGpuShaderVariant::VariantName(v.road, v.texel).c_str(), v.words,
			units, verdict, gated ? "" : "  (mixed geometries)");
	}
	std::printf("  %u of %zu variants predicted past the %u-unit cliff; all of them mix geometries.\n", over_cliff,
		variants.size(), kCliffUnits);

	// The gate. A pass that decodes ONE texel arm is 98.4% of the corpus's byte-road passes, and every
	// pass that takes no byte road at all is trivially smaller -- so this is the population the
	// recovery is denominated in, and it is a hard ceiling.
	//
	// A pass whose draws span two arms carries both, and those are reported above rather than gated:
	// which of them a title produces is a fact about the title, the renderer counts them per frame
	// (texel_mixed_passes), and pinning a ceiling on a program 1.6% of passes reach would be pinning
	// the wrong number. The all-arms variant is the fallback and is over by design -- see the
	// fallback note in GSDeviceVK's TileGpu pipeline setup.
	EXPECT_LE(worst_single, kWordCeiling)
		<< "the largest single-arm TileGpu fragment variant is "
		<< GSTileGpuShaderVariant::VariantName(worst_single_road, worst_single_texel) << " at " << worst_single
		<< " SPIR-V words (~" << PredictedUnits(worst_single) << " a650 units), over the " << kWordCeiling
		<< "-word budget. Something landed in tilegpu.glsl that the Adreno 650 cannot afford: either it belongs "
		   "behind a new variant axis, or the budget needs re-measuring on the device.";
}

// The third party that has to agree about index_format: the renderer derives it, the fragment shader
// switches on it, and GSDevice::GSTileGpuTexelArm decides which arm a module must carry for it. A
// renumbering that moved one and not the others would compile a program without the arm the draw
// needs -- and the shader's guards test their own ranges, so the result is a texture that decodes to
// nothing rather than a crash.
TEST(GSTileGpuShaderBudget, TexelArmMatchesTheIndexFormatNumbering)
{
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(0), GSDevice::kGSTileGpuTexelDirect32); // PSMCT32 / PSMCT24
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(1), GSDevice::kGSTileGpuTexelIndex8); // PSMT8
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(2), GSDevice::kGSTileGpuTexelIndex4); // PSMT4
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(3), GSDevice::kGSTileGpuTexelIndexHi); // PSMT8H
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(4), GSDevice::kGSTileGpuTexelIndexHi); // PSMT4HL
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(5), GSDevice::kGSTileGpuTexelIndexHi); // PSMT4HH
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(6), GSDevice::kGSTileGpuTexelDirect16); // PSMCT16
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(7), GSDevice::kGSTileGpuTexelDirect16); // PSMCT16S
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(8), GSDevice::kGSTileGpuTexelDirect16); // PSMZ16
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(9), GSDevice::kGSTileGpuTexelDirect16); // PSMZ16S

	// And against the numbering's own source: the renderer builds index_format out of these two
	// lists, so every format either list admits has to land on an arm.
	for (u32 psm = 0; psm < 64; psm++)
	{
		const int idx = GSTileSwizzleForms::IndexFormatFor(psm);
		if (idx >= 0)
		{
			const u32 arm = GSDevice::GSTileGpuTexelArm(static_cast<u32>(idx) + 1);
			EXPECT_TRUE(arm == GSDevice::kGSTileGpuTexelIndex8 || arm == GSDevice::kGSTileGpuTexelIndex4 ||
						arm == GSDevice::kGSTileGpuTexelIndexHi)
				<< "PSM " << psm << " is a paletted index format but lands on a non-index arm";
		}
		const int d16 = GSTileSwizzleForms::Direct16FormatFor(psm);
		if (d16 >= 0)
		{
			EXPECT_EQ(GSDevice::GSTileGpuTexelArm(static_cast<u32>(d16) + 6), GSDevice::kGSTileGpuTexelDirect16)
				<< "PSM " << psm << " is a direct 16-bit format but lands off the 16-bit arm";
		}
	}
}
