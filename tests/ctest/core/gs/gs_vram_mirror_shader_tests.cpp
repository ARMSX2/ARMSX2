// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The swizzle tables of the GPU texture decode.
//
// bin/resources/shaders/vulkan/vram_mirror_swizzle.glsl is generated: it holds, for each paletted
// format, the constant-shift expressions that pull a column's texels out of its sixteen 32-bit
// words. They are rendered here from GSTables.cpp (columnTable8, columnTable4, columnTable32) rather
// than copied by hand, so they cannot drift from the tables the CPU swizzle uses, and this test
// fails when the committed file is not what the tables produce.
//
// Rendering the text is not enough on its own, so the same plan that is rendered is also run on the
// CPU against the real readers (GSLocalMemory's rtxbP block reader and rtxP window reader). That
// makes the plan, the block table builder and the scratch layout of GSVramMirrorLayout.h agree with
// ReadTexture before a GPU is involved; the device self-test (gsrunner -gpu-decode-selftest) then
// checks the GLSL itself.
//
// To regenerate the include after a table change:
//   GS_VRAM_MIRROR_UPDATE=1 ./gs_vram_mirror_shader_tests --gtest_filter=*CommittedInclude*

#include "GS/GSLocalMemory.h"
#include "GS/GSTables.h"
#include "GS/Renderers/Common/GSVramMirrorLayout.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifndef GS_VRAM_MIRROR_SHADER_DIR
#error "GS_VRAM_MIRROR_SHADER_DIR must name bin/resources/shaders/vulkan"
#endif

using GSVramMirror::Format;

namespace
{
	/// Where one texel of a column sits in the column's sixteen 32-bit words.
	struct TexelSource
	{
		u32 word;  // 0-15
		u32 shift; // right shift that brings the texel's low bit to bit 0
	};

	/// One decoded format: a block is four 64-byte columns, each a few rows of texels.
	struct FormatPlan
	{
		Format format;
		u32 psm;
		const char* glsl_name;
		u32 rows;         // texel rows per column
		u32 texels;       // texels per row of one block
		u32 mask;         // 0xff or 0xf, the width of a texel
		const char* kind; // GLSL element type of an output row: uvec4 or uvec2
		TexelSource (*source)(u32 col, u32 row, u32 x);
	};

	TexelSource SourceT8(u32 col, u32 row, u32 x)
	{
		const u32 off = columnTable8[col * 4 + row][x];
		EXPECT_EQ(off / 64, col) << "PSMT8 column " << col << " row " << row << " x " << x << " leaves its column";
		return {(off & 63) >> 2, (off & 3) * 8};
	}

	TexelSource SourceT4(u32 col, u32 row, u32 x)
	{
		const u32 nib = columnTable4[col * 4 + row][x];
		EXPECT_EQ(nib / 128, col) << "PSMT4 column " << col << " row " << row << " x " << x << " leaves its column";
		// Nibble n is byte n >> 1; an even n is the LOW nibble (GSLocalMemory::ReadPixel4).
		const u32 byte = (nib & 127) >> 1;
		return {byte >> 2, (byte & 3) * 8 + (nib & 1) * 4};
	}

	template <u32 Shift>
	TexelSource SourceH(u32 col, u32 row, u32 x)
	{
		const u32 word = columnTable32[col * 2 + row][x];
		EXPECT_EQ(word / 16, col) << "32-bit column " << col << " row " << row << " x " << x << " leaves its column";
		return {word & 15, Shift};
	}

	const FormatPlan kPlans[] = {
		{Format::T8, PSMT8, "swz_t8", 4, 16, 0xff, "uvec4", &SourceT8},
		{Format::T4, PSMT4, "swz_t4", 4, 32, 0xf, "uvec4", &SourceT4},
		{Format::T8H, PSMT8H, "swz_t8h", 2, 8, 0xff, "uvec2", &SourceH<24>},
		{Format::T4HL, PSMT4HL, "swz_t4hl", 2, 8, 0xf, "uvec2", &SourceH<24>},
		{Format::T4HH, PSMT4HH, "swz_t4hh", 2, 8, 0xf, "uvec2", &SourceH<28>},
	};

	/// Output words per row (four texels, one byte each, per word).
	u32 WordsPerRow(const FormatPlan& p) { return p.texels / 4; }

	/// Texel value `x` of `row` of column `col`, taken from the column's words `s`.
	u32 Texel(const FormatPlan& p, u32 col, u32 row, u32 x, const u32* s)
	{
		const TexelSource t = p.source(col, row, x);
		return (s[t.word] >> t.shift) & p.mask;
	}

	/// The GLSL expression for one texel, already shifted into its byte of the output word.
	std::string TexelExpr(const FormatPlan& p, const TexelSource& t, u32 byte_in_word)
	{
		char buf[96];
		const u32 bits = (p.mask == 0xff) ? 8 : 4;
		std::string e;
		if (t.shift == 0)
			std::snprintf(buf, sizeof(buf), "(s[%u] & 0x%xu)", t.word, p.mask);
		else if (t.shift + bits == 32)
			std::snprintf(buf, sizeof(buf), "(s[%u] >> %uu)", t.word, t.shift); // the top bits: no mask needed
		else
			std::snprintf(buf, sizeof(buf), "((s[%u] >> %uu) & 0x%xu)", t.word, t.shift, p.mask);
		e = buf;
		if (byte_in_word != 0)
		{
			std::snprintf(buf, sizeof(buf), " << %uu", byte_in_word * 8);
			e = "(" + e + buf + ")";
		}
		return e;
	}

	std::string RenderRow(const FormatPlan& p, u32 col, u32 row)
	{
		std::string out = std::string(p.kind) + "(";
		for (u32 w = 0; w < WordsPerRow(p); w++)
		{
			if (w)
				out += ", ";
			for (u32 k = 0; k < 4; k++)
			{
				if (k)
					out += " | ";
				out += TexelExpr(p, p.source(col, row, w * 4 + k), k);
			}
		}
		return out + ")";
	}

	std::string RenderFunction(const FormatPlan& p)
	{
		const bool wide = (std::string(p.kind) == "uvec4");
		const u32 out_rows = wide ? p.rows * (WordsPerRow(p) / 4) : p.rows;
		std::ostringstream o;
		o << "void " << p.glsl_name << "(uint col, uint s[16], out " << p.kind << " row[" << out_rows << "])\n{\n";
		for (u32 col = 0; col < 4; col++)
		{
			o << (col == 0 ? "\tif (col == 0u)\n" : (col == 3 ? "\telse\n" : "\telse if (col == " + std::to_string(col) + "u)\n"));
			o << "\t{\n";
			for (u32 r = 0; r < p.rows; r++)
			{
				if (!wide)
				{
					o << "\t\trow[" << r << "] = " << RenderRow(p, col, r) << ";\n";
					continue;
				}
				// A row wider than one uvec4 (PSMT4) is stored as consecutive uvec4s.
				const u32 per_row = WordsPerRow(p) / 4;
				for (u32 h = 0; h < per_row; h++)
				{
					std::string v = "uvec4(";
					for (u32 w = 0; w < 4; w++)
					{
						if (w)
							v += ", ";
						for (u32 k = 0; k < 4; k++)
						{
							if (k)
								v += " | ";
							v += TexelExpr(p, p.source(col, r, (h * 4 + w) * 4 + k), k);
						}
					}
					o << "\t\trow[" << (r * per_row + h) << "] = " << v << ");\n";
				}
			}
			o << "\t}\n";
		}
		o << "}\n";
		return o.str();
	}

	std::string RenderInclude()
	{
		std::ostringstream o;
		o << "// GENERATED FILE. Do not edit.\n"
		     "//\n"
		     "// Rendered from pcsx2/GS/GSTables.cpp (columnTable8, columnTable4, columnTable32) by\n"
		     "// tests/ctest/core/gs/gs_vram_mirror_shader_tests.cpp, which fails when this file is not what the\n"
		     "// tables produce. Regenerate with GS_VRAM_MIRROR_UPDATE=1 (see that file).\n"
		     "//\n"
		     "// Each function turns one 64-byte column of a block, given as sixteen 32-bit words s[], into\n"
		     "// the decoded index rows of that column, one byte per texel. Every shift is a constant: a\n"
		     "// dynamic sub-word extract from a storage buffer is miscompiled by Honeykrisp, and constant\n"
		     "// shifts also keep the column out of a register array on Mali.\n"
		     "//\n"
		     "//   swz_t8    PSMT8     4 rows of 16 texels (4 uvec4)\n"
		     "//   swz_t4    PSMT4     4 rows of 32 texels (8 uvec4; nibble n is byte n >> 1, even n low)\n"
		     "//   swz_t8h   PSMT8H    2 rows of  8 texels (2 uvec2; (w >> 24) & 0xff)\n"
		     "//   swz_t4hl  PSMT4HL   2 rows of  8 texels (2 uvec2; (w >> 24) & 0xf)\n"
		     "//   swz_t4hh  PSMT4HH   2 rows of  8 texels (2 uvec2; (w >> 28) & 0xf)\n";
		for (const FormatPlan& p : kPlans)
			o << "\n" << RenderFunction(p);
		return o.str();
	}

	std::string ShaderPath(const char* name) { return std::string(GS_VRAM_MIRROR_SHADER_DIR) + "/" + name; }

	bool ReadFile(const std::string& path, std::string* out)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f)
			return false;
		std::ostringstream ss;
		ss << f.rdbuf();
		*out = ss.str();
		return true;
	}

	TEST(GSVramMirrorShader, CommittedIncludeMatchesTheTables)
	{
		const std::string expected = RenderInclude();
		const std::string path = ShaderPath("vram_mirror_swizzle.glsl");

		if (std::getenv("GS_VRAM_MIRROR_UPDATE"))
		{
			std::ofstream f(path, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(f.good()) << path;
			f << expected;
			f.close();
			std::fprintf(stderr, "rewrote %s (%zu bytes)\n", path.c_str(), expected.size());
		}

		std::string committed;
		ASSERT_TRUE(ReadFile(path, &committed)) << "cannot read " << path;
		if (committed != expected)
		{
			// Leave the expected text next to the build so the difference can be diffed.
			const std::string dump = "vram_mirror_swizzle.glsl.expected";
			std::ofstream f(dump, std::ios::binary | std::ios::trunc);
			f << expected;
			ADD_FAILURE() << path << " is not what GSTables.cpp renders to. The expected text (" << expected.size()
			              << " bytes, committed " << committed.size() << ") was written to ./" << dump
			              << "; rerun with GS_VRAM_MIRROR_UPDATE=1 to overwrite the committed file.";
		}
	}

	TEST(GSVramMirrorShader, MainShaderPullsInTheGeneratedFileAndNamesBothEntryPoints)
	{
		std::string src;
		ASSERT_TRUE(ReadFile(ShaderPath("vram_mirror.glsl"), &src));
		size_t includes = 0;
		for (size_t pos = 0; (pos = src.find("#include \"vram_mirror_swizzle.glsl\"", pos)) != std::string::npos; pos++)
			includes++;
		EXPECT_EQ(includes, 1u) << "the host replaces this line with the generated file";
		EXPECT_NE(src.find("VRAM_MIRROR_DECODE"), std::string::npos);
		EXPECT_NE(src.find("VRAM_MIRROR_SCATTER"), std::string::npos);
		// The functions the main shader calls must exist in the generated file.
		const std::string gen = RenderInclude();
		for (const FormatPlan& p : kPlans)
		{
			EXPECT_NE(gen.find(std::string("void ") + p.glsl_name + "("), std::string::npos) << p.glsl_name;
			EXPECT_NE(src.find(std::string(p.glsl_name) + "("), std::string::npos) << p.glsl_name << " is never called";
		}
	}

	TEST(GSVramMirrorShader, EveryTexelOfAColumnHasItsOwnSource)
	{
		for (const FormatPlan& p : kPlans)
		{
			for (u32 col = 0; col < 4; col++)
			{
				std::vector<u32> seen(16 * 32, 0);
				for (u32 r = 0; r < p.rows; r++)
				{
					for (u32 x = 0; x < p.texels; x++)
					{
						const TexelSource t = p.source(col, r, x);
						ASSERT_LT(t.word, 16u);
						ASSERT_LT(t.shift, 32u);
						ASSERT_LE(t.shift + (p.mask == 0xff ? 8u : 4u), 32u) << "texel runs off its word";
						seen[t.word * 32 + t.shift]++;
					}
				}
				u32 distinct = 0;
				for (u32 n : seen)
				{
					ASSERT_LE(n, 1u) << p.glsl_name << " column " << col << " reads one texel twice";
					distinct += n;
				}
				ASSERT_EQ(distinct, p.rows * p.texels);
			}
		}
	}

	class GSVramMirrorCpuModel : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			m_mem = std::make_unique<GSLocalMemory>();
			m_rng.seed(0x5eed4);
			for (u32 i = 0; i < GSLocalMemory::m_vmsize; i += 4)
			{
				const u32 v = static_cast<u32>(m_rng());
				std::memcpy(m_mem->m_vm8 + i, &v, 4);
			}
		}

		u32 Random(u32 n) { return static_cast<u32>(m_rng() % n); }

		/// What the shader computes for a window, on the CPU: the block numbers GSVramMirrorLayout.h builds,
		/// the plan above for the inside of each block, scattered into a scratch image with the same
		/// pitch and row order the shader uses.
		std::vector<u8> EmulateScratch(const GSOffset& off, const GSVramMirror::WindowLayout& w, const FormatPlan& p) const
		{
			std::vector<u32> blocks;
			GSVramMirror::BuildBlockNumbers(off, w, &blocks);
			EXPECT_EQ(blocks.size(), w.BlockCount());

			std::vector<u8> scratch(w.scratch_bytes, 0xcd);
			for (u32 by = 0; by < w.blocks_high; by++)
			{
				for (u32 bx = 0; bx < w.blocks_wide; bx++)
				{
					const u8* block = m_mem->m_vm8 + static_cast<size_t>(blocks[by * w.blocks_wide + bx] & (GSVramMirror::kMirrorBlocks - 1)) * 256;
					for (u32 col = 0; col < 4; col++)
					{
						u32 s[16];
						std::memcpy(s, block + col * 64, 64);
						for (u32 r = 0; r < p.rows; r++)
						{
							const u32 y = by * w.block_texels_h + col * p.rows + r;
							for (u32 x = 0; x < p.texels; x++)
								scratch[y * w.pitch + bx * w.block_texels_w + x] = static_cast<u8>(Texel(p, col, r, x, s));
						}
					}
				}
			}
			return scratch;
		}

		std::unique_ptr<GSLocalMemory> m_mem;
		std::mt19937_64 m_rng;
	};

	TEST_F(GSVramMirrorCpuModel, PlanMatchesTheBlockReaders)
	{
		const GIFRegTEXA texa = {};
		for (const FormatPlan& p : kPlans)
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[p.psm];
			ASSERT_EQ(static_cast<u32>(psm.bs.x), p.format == Format::T8 ? 16u : (p.format == Format::T4 ? 32u : 8u));

			for (int trial = 0; trial < 200; trial++)
			{
				const u32 bp = Random(GSVramMirror::kMirrorBlocks);
				alignas(32) u8 image[16 * 32];
				std::memset(image, 0xa5, sizeof(image));
				const u32 pitch = 32;
				psm.rtxbP(*m_mem, bp, image, pitch, texa);

				const u8* block = m_mem->m_vm8 + static_cast<size_t>(bp) * 256;
				for (u32 col = 0; col < 4; col++)
				{
					u32 s[16];
					std::memcpy(s, block + col * 64, 64);
					for (u32 r = 0; r < p.rows; r++)
						for (u32 x = 0; x < p.texels; x++)
						{
							const u32 y = col * p.rows + r;
							ASSERT_EQ(image[y * pitch + x], Texel(p, col, r, x, s))
								<< p.glsl_name << " block " << bp << " col " << col << " row " << r << " x " << x;
						}
				}
			}
		}
	}

	TEST_F(GSVramMirrorCpuModel, WindowThroughBlockTableAndPlanMatchesReadTexture)
	{
		const GIFRegTEXA texa = {};
		u32 windows = 0;
		for (const FormatPlan& p : kPlans)
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[p.psm];
			for (int trial = 0; trial < 400; trial++)
			{
				// Any block as TBP0 (the wrap at 16384 included), any TBW including odd ones, windows
				// that start mid-block and run across pages, and some that end past 2048.
				const bool near_wrap = (trial % 8) == 0;
				const u32 tbp = near_wrap ? GSVramMirror::kMirrorBlocks - 1 - Random(70) : Random(GSVramMirror::kMirrorBlocks);
				const u32 tbw = 1 + Random(32);
				const GSOffset off = m_mem->GetOffset(tbp, tbw, p.psm);

				const int x0 = (trial % 5 == 0) ? 2048 - 1 - static_cast<int>(Random(120)) : static_cast<int>(Random(300));
				const int y0 = static_cast<int>(Random(300));
				const int w = 1 + static_cast<int>(Random(260));
				const int h = 1 + static_cast<int>(Random(140));
				const GSVector4i rect(x0, y0, x0 + w, y0 + h);

				GSVramMirror::WindowLayout layout;
				ASSERT_TRUE(GSVramMirror::ComputeLayout(off, rect, &layout));
				ASSERT_EQ(layout.pitch % 16, 0u);

				// The CPU reference, exactly as PreloadTexture does it.
				const int read_w = layout.block_rect.width();
				const u32 cpu_pitch = (static_cast<u32>(read_w) + 31u) & ~31u;
				std::vector<u8> cpu_raw(static_cast<size_t>(cpu_pitch) * layout.block_rect.height() + 64);
				u8* cpu = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(cpu_raw.data()) + 31) & ~uintptr_t(31));
				psm.rtxP(*m_mem, off, layout.block_rect, cpu, static_cast<int>(cpu_pitch), texa);

				const std::vector<u8> scratch = EmulateScratch(off, layout, p);
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++)
					{
						const u8 got = scratch[layout.CopyOffset() + static_cast<u32>(y) * layout.pitch + static_cast<u32>(x)];
						const u8 want = cpu[(layout.crop_y + y) * cpu_pitch + layout.crop_x + x];
						ASSERT_EQ(got, want) << p.glsl_name << " tbp " << tbp << " tbw " << tbw << " rect " << x0 << "," << y0
						                     << " " << w << "x" << h << " texel " << x << "," << y;
					}
				windows++;
			}
		}
		EXPECT_EQ(windows, 5u * 400u);
	}
} // namespace
