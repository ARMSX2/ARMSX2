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
#include "GS/Renderers/Tile/GSTileTypes.h"
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
	/// ...and the ceiling the DESTINATION-READ arms are held to, which is the hardware cliff itself
	/// with one unit of slack rather than the five the shipping population keeps.
	///
	/// ⚠️ Spending the margin is a deliberate, temporary and NAMED trade, not an oversight. Exactly
	/// one plannable variant needs it -- byte+target+source[IDX4+PALGATHER], a pass that unions all
	/// three texel roads AND takes the four-bit paletted geometry AND a target-gathered palette --
	/// and it is 12,395 words before this road adds anything, six units under the ordinary ceiling
	/// on its own. No dump in the eighteen-title corpus plans it: the two three-road variants the
	/// corpus does plan come out at 94 and 105 units with all three arms compiled, and the widest
	/// paletted one it plans (byte+source[IDX8+PALGATHER]) at 105. The next-worst plannable variant
	/// is byte+source[IDX4+PALGATHER] at 109.
	///
	/// The road admits nothing by default, so nothing ships against this number yet. The FIRST
	/// consumer to admit a draw owes an a650 stats line for the variants its titles actually plan --
	/// and if that variant is ever produced, the answer is a finer axis over the three-road union,
	/// not a bigger budget.
	constexpr u32 kSelfReadCliffUnits = 122;
	constexpr u32 kSelfReadWordCeiling = kSelfReadCliffUnits * kDwordsPerUnit * kWordsPerDwordX100 / 100;

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

	std::string ReadShaderSourceFile(const char* name)
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
		u32 self;
		bool quantise;
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

	/// The masks a planner can actually produce: at least one geometry, a palette-order arm only where
	/// a paletted geometry is present (the device normalises it away otherwise, so a module for it
	/// would never be compiled), and never BOTH palette orders at once.
	///
	/// That last one is the design of the 16-bit arm rather than a shortcut through the enumeration. A
	/// gathered palette's words come off either a 32-bit owner or a 16-bit one; the two entry orders
	/// are alternatives, so a pass carries one arm or the other and the widest paletted variant pays
	/// for one gather either way. The union is measured anyway, in TheMergedClutArmStaysUnderTheCliff,
	/// precisely because it is the shape a renderer change could produce by accident.
	bool Plannable(u32 texel_mask)
	{
		if ((texel_mask & GSDevice::kGSTileGpuTexelGeometryMask) == 0)
			return false;
		if ((texel_mask & GSDevice::kGSTileGpuTexelPalGatherMask) == GSDevice::kGSTileGpuTexelPalGatherMask)
			return false;
		return (texel_mask & GSDevice::kGSTileGpuTexelPalGatherMask) == 0 ||
		       (texel_mask & GSDevice::kGSTileGpuTexelPalettedMask) != 0;
	}

	/// Which of the plannable masks the two CEILINGS below hold, as against the ones that are compiled
	/// and reported only. Two exclusions, for two different reasons:
	///
	///  - a mask spanning two address GEOMETRIES carries both, and which of those a title produces is
	///    a fact about the title (1.6% of the corpus's byte-road passes); pinning a ceiling on it
	///    would be pinning the wrong number. That exclusion is as old as this file.
	///  - the SIXTEEN-BIT gather arm, because nothing sets its bit. It is compiled and reported on the
	///    table above -- and on the two widest three-road unions it is over, see
	///    TheMergedClutArmStaysUnderTheCliff -- but no pass can plan it, so it is not part of the
	///    population the shipping ceilings are denominated in.
	///
	/// ⚠️ The first renderer that sets kGSTileGpuTexelPalGather16 deletes the second clause in the
	/// SAME commit, and this gate then has to pass with it deleted. That is the whole point of the
	/// clause being here rather than the arm being left out of the enumeration: the number is on the
	/// table above, and the day it starts mattering is a line of source away.
	bool Gated(u32 texel_mask)
	{
		return GeometryCount(texel_mask) <= 1 && (texel_mask & GSDevice::kGSTileGpuTexelPalGather16) == 0;
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

	const std::string body = ReadShaderSourceFile("tilegpu.glsl");
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
	// least one geometry (plus, optionally, ONE of the two palette-order arms). 4 + 4 * 87 = 352
	// programs.
	//
	// Crossed with what a pass reads its own destination FOR: zero (the overwhelming majority, and
	// the program every pass compiled before that road existed), each use alone, and the full set.
	// The three uses are additive code, so a pair is bounded by the full set -- enumerating all eight
	// masks would multiply an already slow gate for programs no measurement would read differently.
	// ...and with the 16-bit frame's quantisation, which is a handful of integer ops on the same tail
	// but a variant axis all the same, because a pass whose frame is 32-bit must not carry it. Crossed
	// with the two self masks that bound the space (none, and all three) rather than with all five:
	// the arms are additive, so those two bracket every combination, and a full cross would double an
	// already slow gate for programs no measurement would read differently.
	std::vector<Variant> variants;
	static constexpr u32 kSelfMasks[] = {0, GSDevice::kGSTileGpuSelfDate, GSDevice::kGSTileGpuSelfBlend,
		GSDevice::kGSTileGpuSelfMask, GSDevice::kGSTileGpuSelfMaskAll};
	for (const u32 self : kSelfMasks)
	{
		for (int q = 0; q < 2; q++)
		{
			if (q != 0 && self != 0 && self != GSDevice::kGSTileGpuSelfMaskAll)
				continue;
			for (u32 road = 0; road <= GSDevice::kGSTileGpuRoadMaskAll; road++)
			{
				if ((road & GSDevice::kGSTileGpuRoadByte) == 0)
				{
					variants.push_back({road, 0, self, q != 0, 0});
					continue;
				}
				for (u32 texel = 1u; texel <= GSDevice::kGSTileGpuTexelMaskAll; texel++)
				{
					if (Plannable(texel))
						variants.push_back({road, texel, self, q != 0, 0});
				}
			}
		}
	}

	// Two worsts, because the two populations answer to different ceilings: everything a pass can
	// plan TODAY (no draw is admitted to the destination read yet) is held to the ordinary budget,
	// and the read's own arms to the one beside it.
	u32 worst_single = 0, worst_single_road = 0, worst_single_texel = 0;
	bool worst_single_q = false;
	u32 worst_self = 0, worst_self_road = 0, worst_self_texel = 0, worst_self_self = 0;
	bool worst_self_q = false;
	for (Variant& v : variants)
	{
		std::string error;
		v.words = sc.FragmentWords(
			prologue + GSTileGpuShaderVariant::VariantDefines(v.road, v.texel, v.self, v.quantise) + body, error);
		ASSERT_NE(v.words, 0u) << "variant "
							   << GSTileGpuShaderVariant::VariantName(v.road, v.texel, v.self, v.quantise)
							   << " failed to compile: " << error;
		if (!Gated(v.texel))
			continue;
		if (v.self == 0 && v.words > worst_single)
		{
			worst_single = v.words;
			worst_single_road = v.road;
			worst_single_texel = v.texel;
			worst_single_q = v.quantise;
		}
		if (v.self != 0 && v.words > worst_self)
		{
			worst_self = v.words;
			worst_self_road = v.road;
			worst_self_texel = v.texel;
			worst_self_self = v.self;
			worst_self_q = v.quantise;
		}
	}

	// The table, printed whatever the verdict: the point of a size gate is the numbers, and a device
	// run confirms or refutes the predicted units row by row.
	std::printf("\nTileGpu fragment variant sizes (SPIR-V words, Honeykrisp form; predicted a650 units)\n");
	std::printf("  ceiling %u words = %u units (123 is the cliff; the margin is deliberate)\n", kWordCeiling,
		kBudgetUnits);
	std::printf("  destination-read arms: %u words = %u units (the margin is spent there, on purpose)\n",
		kSelfReadWordCeiling, kSelfReadCliffUnits);
	std::printf("  %-66s %8s %7s %s\n", "variant", "words", "units", "verdict");
	u32 over_cliff = 0, over_cliff_gated = 0;
	for (const Variant& v : variants)
	{
		const u32 units = PredictedUnits(v.words);
		const char* verdict = (units > kCliffUnits) ? "OVER THE CLIFF" : (v.words > kWordCeiling ? "over budget" : "ok");
		const char* why = Gated(v.texel)               ? "" :
		                  (GeometryCount(v.texel) > 1) ? "  (mixed geometries)" :
		                                                 "  (16-bit gather: nothing plans it)";
		over_cliff += (units > kCliffUnits) ? 1 : 0;
		over_cliff_gated += (units > kCliffUnits && Gated(v.texel)) ? 1 : 0;
		std::printf("  %-66s %8u %7u %s%s\n",
			GSTileGpuShaderVariant::VariantName(v.road, v.texel, v.self, v.quantise).c_str(), v.words, units, verdict,
			why);
	}
	std::printf("  %u of %zu variants predicted past the %u-unit cliff, %u of them in the gated population.\n",
		over_cliff, variants.size(), kCliffUnits, over_cliff_gated);

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
		<< GSTileGpuShaderVariant::VariantName(worst_single_road, worst_single_texel, 0, worst_single_q) << " at "
		<< worst_single
		<< " SPIR-V words (~" << PredictedUnits(worst_single) << " a650 units), over the " << kWordCeiling
		<< "-word budget. Something landed in tilegpu.glsl that the Adreno 650 cannot afford: either it belongs "
		   "behind a new variant axis, or the budget needs re-measuring on the device.";

	// ...and the destination-read arms, against the cliff-adjacent ceiling their note above explains.
	EXPECT_LE(worst_self, kSelfReadWordCeiling)
		<< "the largest single-arm TileGpu fragment variant that reads its own destination is "
		<< GSTileGpuShaderVariant::VariantName(worst_self_road, worst_self_texel, worst_self_self, worst_self_q)
		<< " at "
		<< worst_self << " SPIR-V words (~" << PredictedUnits(worst_self) << " a650 units), over the "
		<< kSelfReadWordCeiling << "-word ceiling and so within " << (kCliffUnits - PredictedUnits(worst_self))
		<< " units of the Adreno 650's instruction-size cliff. That margin is already spent; the answer is a finer "
		   "variant axis, not a bigger number.";
}

// The merged CLUT arm's own budget, which is why that arm is behind a MODULE define and not just
// behind its per-draw pal_mode.
//
// TileGpuClutMergeRegions changes the word order a gathered palette lands in, so the fragment stage
// grows a second offset expression, and TileGpuClutMergePages then makes that expression's stride a
// per-draw value. Measured at the landing, on the two variants the gate above names as its worst:
// 304 SPIR-V words for the square arm and 72 more for the stride, together most of a unit of Adreno
// 650 instruction length. That is affordable for a program that actually merges and NOT affordable
// for one that does not, because dead code counts here -- four units of never-executed arithmetic
// once moved Shadow of the Colossus from 21.8 to 30.7 ms with byte-identical frames. So each arm
// compiles in only when its own lever is on, the shipping default program is unchanged, and the two
// ceilings above keep measuring what ships.
//
// What this test pins is the merged programs: they must stay on the safe side of the 123-unit CLIFF.
// They are held to the cliff rather than to the two ceilings above because those carry margin for a
// program every device runs and these run only where a lever was turned on deliberately -- and it is
// the FIRST thing to check if either lever's device A/B comes back slower rather than faster, since
// one unit is exactly the size of the effect this whole file exists to catch.
//
// ⚠️ THREE shapes, because the two levers pay separately. The square merge is a constant stride and
// costs 304 words; the page merge makes the stride per-draw and costs 72 more, which lands the
// destination-read variant TWO WORDS under the cliff. That is the tightest thing in this tree. It is
// not a number to nudge: the next edit to tilegpu_palette_word will trip this test, and the answer
// then is to make the arm cheaper or to take the device stats line that would let the calibration
// above be redone, not to raise the cliff -- the cliff is hardware.
TEST(GSTileGpuShaderBudget, TheMergedClutArmStaysUnderTheCliff)
{
	const GSTileSwizzleForms::FormSet forms = GSTileSwizzleForms::Fit();
	ASSERT_TRUE(forms.valid) << "the page swizzle tables stopped fitting closed forms; there is no shader to size";
	const std::string body = ReadShaderSourceFile("tilegpu.glsl");
	ASSERT_FALSE(body.empty()) << "could not read tilegpu.glsl from " << ARMSX2_SHADER_SOURCE_DIR;

	Shaderc sc;
	if (!sc.Open())
	{
		GTEST_SKIP() << "shaderc is not loadable here, so no program can be sized. This is a SKIP, not a pass: "
						"the merged CLUT arm's size was NOT checked on this machine.";
	}

	// The two the gate above reports as worst: all three read roads, the four-bit paletted geometry,
	// the gathered-palette arm, the 16-bit quantise -- with and without the destination read.
	const u32 road = GSDevice::kGSTileGpuRoadByte | GSDevice::kGSTileGpuRoadTarget | GSDevice::kGSTileGpuRoadSource;
	const u32 texel = GSDevice::kGSTileGpuTexelIndex4 | GSDevice::kGSTileGpuTexelPalGather;
	// ...and the same variant carrying the SIXTEEN-BIT gather order instead, which is the fourth
	// shape: a pass gathering off a 16-bit owner compiles this arm and not modes 1-3. The union of
	// both orders is measured too, as information -- it is not a shape any pass plans (Plannable
	// refuses it above), and it is here so the cost of ever letting one through is a number rather
	// than a guess.
	const u32 texel16 = GSDevice::kGSTileGpuTexelIndex4 | GSDevice::kGSTileGpuTexelPalGather16;
	const u32 texel_both = texel | GSDevice::kGSTileGpuTexelPalGather16;
	const u32 selves[] = {0u, GSDevice::kGSTileGpuSelfMaskAll};

	const auto words_for = [&](bool merge, bool pages, const std::string& defines, std::string& error) {
		return sc.FragmentWords(kStageHeader + GSTileSwizzleForms::ShaderDefines(forms) +
									GSTileGpuShaderVariant::DeviceDefines(true, true, true, true, false, merge, pages) +
									defines + body,
			error);
	};

	for (const u32 self : selves)
	{
		const std::string variant = GSTileGpuShaderVariant::VariantName(road, texel, self, true);
		const std::string defines = GSTileGpuShaderVariant::VariantDefines(road, texel, self, true);
		std::string error;
		const u32 off_words = words_for(false, false, defines, error);
		ASSERT_NE(off_words, 0u) << variant << " (merge off) failed to compile: " << error;
		const u32 sq_words = words_for(true, false, defines, error);
		ASSERT_NE(sq_words, 0u) << variant << " (squares) failed to compile: " << error;
		const u32 pg_words = words_for(true, true, defines, error);
		ASSERT_NE(pg_words, 0u) << variant << " (squares + pages) failed to compile: " << error;

		std::printf("  %-58s %6u off -> %6u squares -> %6u pages words  (%u -> %u -> %u a650 units)\n",
			variant.c_str(), off_words, sq_words, pg_words, PredictedUnits(off_words), PredictedUnits(sq_words),
			PredictedUnits(pg_words));

		// The levers cost nothing where they are off. This is the claim the two ceilings above rest
		// on once these arms exist, so it is asserted rather than assumed.
		EXPECT_EQ(off_words, sc.FragmentWords(kStageHeader + GSTileSwizzleForms::ShaderDefines(forms) +
												  GSTileGpuShaderVariant::DeviceDefines(true, true, true) + defines +
												  body,
								 error))
			<< variant << ": the merge-off program is not what the default DeviceDefines produce";
		// ...and asking for pages without squares is not a shape, so it must not silently produce a
		// third one: the page arm IS the square arm with a wider tile.
		EXPECT_EQ(off_words, words_for(false, true, defines, error))
			<< variant << ": TileGpuClutMergePages compiled an arm with TileGpuClutMergeRegions off";

		EXPECT_LE(PredictedUnits(sq_words), kCliffUnits)
			<< "the merged CLUT arm puts " << variant << " at " << sq_words << " SPIR-V words (~"
			<< PredictedUnits(sq_words) << " a650 units), past the " << kCliffUnits
			<< "-unit instruction-size cliff. Turning TileGpuClutMergeRegions on would then cost more in program "
			   "size than the copy regions it saves; the arm needs a cheaper spelling, not a bigger number.";
		EXPECT_LE(PredictedUnits(pg_words), kCliffUnits)
			<< "the page merge's per-draw stride puts " << variant << " at " << pg_words << " SPIR-V words (~"
			<< PredictedUnits(pg_words) << " a650 units), past the " << kCliffUnits
			<< "-unit instruction-size cliff. This is the tightest program in the tree -- see the note above -- and "
			   "the answer is a cheaper spelling, not a bigger number.";

		// The FOURTH shape: the same variant with the 16-bit gather order in place of the 32-bit one.
		// It is not a merge lever -- it is a texel arm, so it rides the variant defines rather than the
		// device ones, and the two merge levers are OFF underneath it exactly as they are for any pass
		// that does not merge squares. That is what makes this the number the road's own decision
		// rests on: does a pass gathering a 16-bit palette fit, standing alone, on its own axis.
		const std::string variant16 = GSTileGpuShaderVariant::VariantName(road, texel16, self, true);
		const std::string defines16 = GSTileGpuShaderVariant::VariantDefines(road, texel16, self, true);
		const u32 g16_words = words_for(false, false, defines16, error);
		ASSERT_NE(g16_words, 0u) << variant16 << " failed to compile: " << error;

		// ...and, for information only, the union of both gather orders. No pass plans it.
		const std::string defines_both = GSTileGpuShaderVariant::VariantDefines(road, texel_both, self, true);
		const u32 both_words = words_for(false, false, defines_both, error);
		ASSERT_NE(both_words, 0u) << "the two-gather union failed to compile: " << error;

		std::printf("  %-58s %6u palgather16 -> %6u both orders  (%u -> %u a650 units)\n", variant16.c_str(),
			g16_words, both_words, PredictedUnits(g16_words), PredictedUnits(both_words));

		// THE ANSWER, measured rather than argued, and asserted in BOTH directions so it cannot rot
		// into a comment nobody re-reads.
		//
		// The arm costs a FLAT 920-924 SPIR-V words -- about eight a650 units -- on every paletted
		// variant, whatever else that variant carries. Three times what the square merge costs, and
		// for a plain reason: it is two vram_words loads, two packs and a wider offset per tap, inlined
		// at four texels of a bilinear sample. The cost does not vary with the road mask, so the whole
		// question is which variants had eight units of headroom.
		//
		// Most did. Every variant the corpus actually plans fits with room: byte[IDX8+PALGATHER16] q16
		// is 104 units against 96 with the 32-bit order, byte[IDX4+PALGATHER16] q16 is 108, and the
		// widest the corpus plans -- byte+source[IDX8] -- is 109 against 101. Spider-Man 3, the one
		// title this arm exists for, draws through exactly those.
		//
		// Two do not, and both are three-road unions no dump in the corpus plans:
		// byte+target+source[IDX4+PALGATHER16] q16 lands EXACTLY on the 123-unit cliff with nothing
		// left, and its destination-read twin lands five units past it. A ceiling that only holds for
		// the variants eighteen dumps happen to produce is not a ceiling, so those two are the answer
		// to "does the fragment arm fit", and the answer is no.
		//
		// The budget number does not move for it -- the cliff is hardware -- and the finer-axis answer
		// is already spent, because this arm IS its own axis, measured alone with both merge levers
		// off. What is left is a choice for whoever ships the road: refuse the 16-bit gather on a pass
		// that unions all three texel roads (it would never fire on the title the lever is for), or
		// take the entry-order compute gather, which feeds pal_mode 0 and costs the fragment stage
		// nothing at all. The second is a new synchronisation surface, and this campaign has already
		// paid once for one of those.
		//
		// And the union of the two gather orders is never a shape: 145 and 151 units. Plannable()
		// refuses it above; this is what that refusal is worth.
		if (self == 0)
		{
			EXPECT_LE(PredictedUnits(g16_words), kCliffUnits)
				<< variant16 << " at " << g16_words << " words is past the " << kCliffUnits
				<< "-unit cliff even without the destination read. It was exactly AT it when the arm landed, with "
				   "nothing to spare, so anything added to tilegpu_palette_word since then is what moved it.";
		}
		else
		{
			EXPECT_GT(PredictedUnits(g16_words), kCliffUnits)
				<< variant16 << " at " << g16_words << " words now FITS under the " << kCliffUnits
				<< "-unit cliff, where it was 128 units when it was measured. That is good news and this expectation "
				   "is what makes you read this: re-take the number, put it in the dossier, and the fragment road "
				   "(design stage 3a) is open again -- delete this branch when you do.";
		}
	}
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
	// The direct-32 depth pair rides the SAME arm as its colour twin -- one address geometry under a
	// constant block XOR, which is a select inside the arm and not a sixth geometry. A landing that
	// gave it its own bit would double the variant population and cost the budget for nothing.
	EXPECT_EQ(GSDevice::GSTileGpuTexelArm(10), GSDevice::kGSTileGpuTexelDirect32); // PSMZ32 / PSMZ24

	// And against the numbering's own source: the renderer builds index_format out of these three
	// lists, so every format any of them admits has to land on an arm.
	for (u32 psm = 0; psm < 64; psm++)
	{
		const int d32 = GSTileSwizzleForms::Direct32FormatFor(psm);
		if (d32 >= 0)
		{
			const u32 fmt = (d32 == static_cast<int>(GSTileSwizzleForms::Direct32Format::Z32)) ? 10u : 0u;
			EXPECT_EQ(GSDevice::GSTileGpuTexelArm(fmt), GSDevice::kGSTileGpuTexelDirect32)
				<< "PSM " << psm << " is a direct 32-bit format but lands off the direct-32 arm";
		}
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

// Every DEPTH SEED program the device can inject, compiled. A size gate is not what this is for --
// a seed is one texel-free fetch and a depth write -- it is a COMPILE gate, and the failure mode it
// exists for is silence. Only the formats a corpus dump happens to use ever reach the device's
// compile path, and when one fails the executor skips the op and the depth image keeps what it had:
// no error, no VUID, just a Z buffer that was not seeded. Formats nothing in the corpus exercises
// (PSMCT16 and PSMCT16S as a Z buffer's PSM, PSMZ16, PSMZ16S) would sit unbuilt and unbroken until a
// game asked. So all eight are built here, on the Honeykrisp byte-select form for the same reason
// the sizes above use it.
TEST(GSTileGpuShaderBudget, EveryDepthSeedFormatCompiles)
{
	const GSTileSwizzleForms::FormSet forms = GSTileSwizzleForms::Fit();
	ASSERT_TRUE(forms.valid) << "the page swizzle tables stopped fitting closed forms";

	const std::string body = ReadShaderSourceFile("tilegpu_seed.glsl");
	ASSERT_FALSE(body.empty()) << "could not read tilegpu_seed.glsl from " << ARMSX2_SHADER_SOURCE_DIR;

	Shaderc sc;
	if (!sc.Open())
	{
		GTEST_SKIP() << "shaderc is not loadable here, so no program can be compiled. This is a SKIP, not a pass: "
						"the depth seed's eight variants were NOT built on this machine.";
	}

	const std::string prologue = kStageHeader + GSTileSwizzleForms::ShaderDefines(forms) +
								 "#define TILEGPU_STATIC_BYTE_SEL 1\n#define TILEGPU_SEED_DEPTH 1\n";
	for (u32 f = 0; f < kGSTileDepthRoadFormats; f++)
	{
		std::string error;
		const std::string src = prologue + "#define TILEGPU_SEED_FMT " + std::to_string(f) + "\n" + body;
		const u32 words = sc.FragmentWords(src, error);
		EXPECT_NE(words, 0u) << "depth seed format " << f << " failed to compile: " << error;
		std::printf("  depth seed format %u: %u SPIR-V words\n", f, words);
	}

	// ...and the four COLOUR seed programs beside them, because the same file now carries both and a
	// define added for one arm can break the other.
	const std::string cprologue = kStageHeader + GSTileSwizzleForms::ShaderDefines(forms) +
								  "#define TILEGPU_STATIC_BYTE_SEL 1\n";
	for (u32 f = 0; f < kGSTileByteRoadFormats; f++)
	{
		std::string error;
		const std::string src = cprologue + "#define TILEGPU_SEED_FMT " + std::to_string(f) + "\n" + body;
		const u32 words = sc.FragmentWords(src, error);
		EXPECT_NE(words, 0u) << "colour seed format " << f << " failed to compile: " << error;
		std::printf("  colour seed format %u: %u SPIR-V words\n", f, words);
	}
}

// The Mali arm of the fragment program: the one the ARM proprietary compiler gets, in which every
// vector bitwise AND is done a component at a time because that compiler miscompiles the vector
// form. Nobody builds it on a dev box, so nothing here would have said if an overload were missing
// or a call site had a type no overload covers -- the failure would arrive as a Mali-only compile
// error, or, if the site were left unwrapped, as scattered wrong pixels on that driver alone.
//
// Only the variants that carry the wrapped sites are built: the fog unpack is in every program, and
// the byte tail's four -- the blend coefficients, the COLCLAMP mask, the 16-bit quantise and the
// FBMSK merge -- come in with their own gates. The full road/arm set on top of each, so every
// declaration the module can hold is in the compile.
TEST(GSTileGpuShaderBudget, TheScalarizedVectorAndFormCompiles)
{
	const GSTileSwizzleForms::FormSet forms = GSTileSwizzleForms::Fit();
	ASSERT_TRUE(forms.valid) << "the page swizzle tables stopped fitting closed forms";

	const std::string body = ReadShaderSourceFile("tilegpu.glsl");
	ASSERT_FALSE(body.empty()) << "could not read tilegpu.glsl from " << ARMSX2_SHADER_SOURCE_DIR;

	Shaderc sc;
	if (!sc.Open())
	{
		GTEST_SKIP() << "shaderc is not loadable here, so no program can be compiled. This is a SKIP, not a pass: "
						"the scalarized vector-AND form was NOT built on this machine.";
	}

	// The last argument is the workaround; everything before it is the a650 profile the rest of this
	// file measures, so the two prologues differ in exactly one define.
	const std::string plain = kStageHeader + GSTileSwizzleForms::ShaderDefines(forms) +
	                          GSTileGpuShaderVariant::DeviceDefines(true, true, true, true, false);
	const std::string scalar = kStageHeader + GSTileSwizzleForms::ShaderDefines(forms) +
	                           GSTileGpuShaderVariant::DeviceDefines(true, true, true, true, true);

	static constexpr u32 kAllRoads = GSDevice::kGSTileGpuRoadMaskAll;
	static constexpr u32 kAllArms = GSDevice::kGSTileGpuTexelMaskAll;
	const Variant cases[] = {
		{kAllRoads, kAllArms, 0, false, 0}, // fog only
		{kAllRoads, kAllArms, GSDevice::kGSTileGpuSelfBlend, false, 0}, // + the blend equation
		{kAllRoads, kAllArms, GSDevice::kGSTileGpuSelfMask, false, 0}, // + the FBMSK merge
		{kAllRoads, kAllArms, GSDevice::kGSTileGpuSelfMaskAll, true, 0}, // + the 16-bit quantise
		{0, 0, GSDevice::kGSTileGpuSelfMaskAll, true, 0}, // the whole tail with no texture at all
	};

	std::printf("\nTileGpu fragment variants in both vector-AND forms (SPIR-V words)\n");
	std::printf("  %-66s %8s %8s\n", "variant", "plain", "scalar");
	for (const Variant& v : cases)
	{
		const std::string defines = GSTileGpuShaderVariant::VariantDefines(v.road, v.texel, v.self, v.quantise);
		const std::string name = GSTileGpuShaderVariant::VariantName(v.road, v.texel, v.self, v.quantise);
		std::string plain_error, scalar_error;
		const u32 plain_words = sc.FragmentWords(plain + defines + body, plain_error);
		const u32 scalar_words = sc.FragmentWords(scalar + defines + body, scalar_error);
		EXPECT_NE(plain_words, 0u) << "variant " << name << " failed to compile: " << plain_error;
		EXPECT_NE(scalar_words, 0u) << "variant " << name
									<< " failed to compile with TILEGPU_SCALARIZE_VECTOR_AND on: " << scalar_error
									<< "  (the Mali arm -- most likely a wrapped site whose type has no tilegpu_and "
									   "overload)";
		std::printf("  %-66s %8u %8u\n", name.c_str(), plain_words, scalar_words);
	}
}

// The three parties that have to agree about the workaround: the driver database answers it, the
// device puts the answer in the define block, and the shader reads that define. A rename on either
// side of the block would leave the shader compiling its plain arm on the one driver that cannot run
// it -- silently, because an undefined name in an `#if` is zero.
TEST(GSTileGpuShaderBudget, TheScalarizedVectorAndDefineIsTheOneTheShaderReads)
{
	const std::string body = ReadShaderSourceFile("tilegpu.glsl");
	ASSERT_FALSE(body.empty()) << "could not read tilegpu.glsl from " << ARMSX2_SHADER_SOURCE_DIR;

	// The device's half, both ways. Off is the default so a caller that never heard of the workaround
	// emits the program every other driver already gets.
	const std::string off = GSTileGpuShaderVariant::DeviceDefines(true, true, true);
	const std::string on = GSTileGpuShaderVariant::DeviceDefines(true, true, true, true, true);
	EXPECT_NE(off.find("#define TILEGPU_SCALARIZE_VECTOR_AND 0\n"), std::string::npos);
	EXPECT_NE(on.find("#define TILEGPU_SCALARIZE_VECTOR_AND 1\n"), std::string::npos);

	// ...and the shader's, quoted so a rename has to come here.
	EXPECT_NE(body.find("#if TILEGPU_SCALARIZE_VECTOR_AND\n"), std::string::npos);
	EXPECT_NE(body.find("#define tilegpu_and(a, b) ((a) & (b))\n"), std::string::npos);

	// Every vector AND in the file goes through the wrapper, and this is the count of what does: three
	// overloads, the off-arm macro, and seven call sites -- the fog colour unpack, the blend
	// coefficients, the COLCLAMP mask, the 16-bit quantise, the FBMSK keep mask, and the two halves of
	// the FBMSK merge. A new vector AND belongs in the wrapper too; adding one raw is a Mali-only
	// wrong-pixel bug that nothing else in this suite can see, so the number is pinned rather than
	// bounded.
	size_t uses = 0;
	for (size_t at = body.find("tilegpu_and("); at != std::string::npos; at = body.find("tilegpu_and(", at + 1))
		uses++;
	EXPECT_EQ(uses, 11u) << "tilegpu.glsl names tilegpu_and " << uses
						 << " times, not the expected 3 overloads + 1 macro + 7 call sites. If a vector bitwise AND "
							"was added, wrap it and update this count; if one was removed, just update the count.";
}

// The scissor left the shader. It is a vkCmdSetScissor before each indirect call on every device,
// because that is the test the GS applies: an integer rectangle over rasterized pixels, with the
// primitive's interpolation untouched. The clip-plane road that used to serve devices with
// shaderClipDistance did it geometrically, which re-interpolates a cut primitive and moves shaded
// values by amounts that differ per GPU vendor -- so two devices could not be expected to agree on
// a frame.
//
// Pinned as text because the failure mode of a partial revival is silent: an `#if` on a name
// nothing defines is zero, so a shader that regrew the block would simply never write the planes,
// and a define nothing reads would sit in the block unnoticed.
TEST(GSTileGpuShaderBudget, TheScissorIsNotInTheShaderAtAll)
{
	const std::string body = ReadShaderSourceFile("tilegpu.glsl");
	ASSERT_FALSE(body.empty()) << "could not read tilegpu.glsl from " << ARMSX2_SHADER_SOURCE_DIR;

	EXPECT_EQ(body.find("gl_ClipDistance"), std::string::npos)
		<< "tilegpu.glsl names gl_ClipDistance. The scissor is a Vulkan scissor per indirect call; a "
		   "vertex module that merely DECLARES the SPIR-V ClipDistance capability is also one a driver "
		   "without shaderClipDistance may refuse.";
	EXPECT_EQ(body.find("TILEGPU_VS_CLIP"), std::string::npos) << "tilegpu.glsl still reads the deleted clip define";

	// ...and the device's half: the block it injects must not carry the define either, whatever
	// arguments it is given.
	const std::string defines = GSTileGpuShaderVariant::DeviceDefines(true, true, true);
	EXPECT_EQ(defines.find("TILEGPU_VS_CLIP"), std::string::npos)
		<< "DeviceDefines still emits the deleted clip define";
}

// The C++ state row and the shader's state row are one layout with two spellings, and nothing in
// either language can compare them: the shader is read from disk at runtime, so the file compiled is
// not necessarily the file this binary was built beside. The device parses the shader's own
// declaration and refuses to build on a mismatch; this is the gate that says the parse is right about
// the shader in THIS tree, so a member added to one side and not the other fails here rather than on
// a device, where the symptom is every row but the first read from the wrong place.
TEST(GSTileGpuShaderBudget, TheShaderStateRowIsTheBinarysStateRow)
{
	const std::string body = ReadShaderSourceFile("tilegpu.glsl");
	ASSERT_FALSE(body.empty()) << "could not read tilegpu.glsl from " << ARMSX2_SHADER_SOURCE_DIR;

	EXPECT_EQ(GSTileGpuShaderVariant::StateRowWordsIn(body), GSDevice::GSTileGpuPassPlan::kStateRowWords)
		<< "tilegpu.glsl's struct StateRow is not the same number of 32-bit words as the C++ row. Two "
		   "sides on different strides read every row but the first from the wrong place, and nothing "
		   "at runtime fails -- every field lands somewhere plausible.";
}

// ...and that the parser earns that comparison. A parser that answered 0 for everything would pass
// nothing above, but one that answered kStateRowWords for everything would pass it always, so the
// counting is pinned on strings whose answers are known by hand.
TEST(GSTileGpuShaderBudget, TheStateRowParserCountsWords)
{
	using GSTileGpuShaderVariant::StateRowWordsIn;

	EXPECT_EQ(StateRowWordsIn("struct StateRow { vec2 a; uint b; };"), 3u);
	EXPECT_EQ(StateRowWordsIn("struct StateRow { uint a, b, c; };"), 3u) << "a comma list is one word per name";
	EXPECT_EQ(StateRowWordsIn("struct StateRow {\n\tvec4 a; // uint x; y;\n\tuint b;\n};"), 5u)
		<< "a semicolon inside a line comment must not split a declaration";
	EXPECT_EQ(StateRowWordsIn("struct StateRow { /* uint x; */ vec2 a; };"), 2u)
		<< "a semicolon inside a block comment must not split a declaration";

	// The answers that mean "cannot tell", which the device treats as a mismatch rather than a pass.
	EXPECT_EQ(StateRowWordsIn("struct Other { uint a; };"), 0u) << "no StateRow declaration at all";
	EXPECT_EQ(StateRowWordsIn("struct StateRow { mat4 m; };"), 0u) << "a type this cannot size in words";
	EXPECT_EQ(StateRowWordsIn("struct StateRow { uint a; float b[4]; };"), 0u) << "an array member is not a word count";
}
