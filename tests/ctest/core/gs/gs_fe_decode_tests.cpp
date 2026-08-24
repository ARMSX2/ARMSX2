// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The front-end decode recorder (GS/GSFeDecode.h) is the gate instrument for the
// GIF-decoder swap: a replacement front end is "done" when its recording is
// byte-identical to the shipping decode's. Two things have to hold for that gate
// to mean anything, and both are pinned here.
//
//   1. The record layout is FIXED. Every offset and size below is written out as
//      a literal rather than derived from the struct, because deriving it from
//      the struct is exactly the check that cannot fail. A layout change that
//      slips through silently makes an old recording unreadable-but-plausible,
//      which is a green gate over garbage. The format version is pinned the same
//      way, so changing the layout without bumping it breaks this suite.
//
//   2. The differ can FAIL. An instrument that always says "identical" proves
//      nothing, so the perturbation cases here flip exactly one byte -- inside a
//      vertex, inside a register word, in the record count -- and require the
//      locator to name the right event, the right field and the right offset.

#include "GS/GSFeDecode.h"
#include "GS/Renderers/Common/GSVertex.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace GSFeDecode;

namespace
{
	std::string ScratchPath(const char* name)
	{
		return (std::filesystem::temp_directory_path() / name).string();
	}

	// A synthetic draw: deterministic register words, deterministic vertices, so
	// a perturbation is the only thing that can move a byte.
	struct SynthDraw
	{
		DrawBody body = {};
		std::vector<u8> tail;
	};

	SynthDraw MakeDraw(u64 serial, u32 vertex_count, u32 index_count)
	{
		SynthDraw d;
		d.body.draw_serial = serial;
		for (u32 i = 0; i < kEnvSnapshotWords; i++)
		{
			d.body.draw_env.w[i] = 0x1000000000000000ull + (serial << 32) + i;
			d.body.next_env.w[i] = 0x2000000000000000ull + (serial << 32) + i;
		}
		for (u32 i = 0; i < 8; i++)
			d.body.next_v[i] = 0xA0000000u + i;
		for (u32 i = 0; i < 4; i++)
			d.body.draw_rect[i] = static_cast<s32>(10 + i);
		d.body.backed_up_ctx = 1;
		d.body.dirty_gs_regs = 0xDEADBEEFu;
		d.body.flush_reason = 4;
		d.body.channel_shuffle_finish = 1;
		d.body.packed_uv_hack_flag = 0;
		d.body.prim = 0x0000000000000015ull;
		d.body.ctxt = 1;
		d.body.vertex_head = 0;
		d.body.vertex_tail = vertex_count;
		d.body.vertex_next = vertex_count;
		d.body.vertex_count = vertex_count;
		d.body.index_count = index_count;

		d.tail.resize(vertex_count * sizeof(GSVertex) + index_count * 2);
		for (size_t i = 0; i < d.tail.size(); i++)
			d.tail[i] = static_cast<u8>((i * 7 + serial * 13) & 0xFF);
		return d;
	}

	TransferBody MakeTransfer(u64 serial)
	{
		TransferBody t = {};
		t.blit = 0x1111111100000000ull + serial;
		t.env_blit = 0x2222222200000000ull + serial;
		t.pos = 0x3333333300000000ull + serial;
		t.reg = 0x4444444400000000ull + serial;
		t.draw_serial = serial;
		t.payload_digest = 0x5555555500000000ull + serial;
		for (u32 i = 0; i < 4; i++)
			t.rect[i] = static_cast<s32>(i);
		t.len = 512;
		t.stat_len = 512;
		t.end = 512;
		t.total = 1024;
		t.init_x = 3;
		t.init_y = 4;
		t.first_slice = 1;
		for (u32 i = 0; i < kPayloadHeadBytes; i++)
			t.payload_head[i] = static_cast<u8>(i);
		return t;
	}

	void SubmitDraw(const SynthDraw& d)
	{
		Submit(RecordType::Draw, &d.body, sizeof(d.body), d.tail.data(), static_cast<u32>(d.tail.size()));
	}

	// One canonical stream: a frame boundary, an upload, a move, a CLUT load, a
	// readback request and two draws. `perturb` is called on the second draw
	// before it is submitted, which is how the failure cases inject their byte.
	template <typename PerturbFn>
	void EmitStreamPerturbed(PerturbFn perturb, u32 draw_count)
	{
		VsyncBody v = {};
		v.draw_serial = 0;
		v.field = 1;
		v.registers_written = 1;
		v.idle_frame = 0;
		SubmitFixed(RecordType::Vsync, v);

		SubmitFixed(RecordType::Transfer, MakeTransfer(1));

		MoveBody m = {};
		m.blit = 0xABCDEF0011223344ull;
		m.pos = 0x1;
		m.reg = 0x2;
		m.draw_serial = 1;
		SubmitFixed(RecordType::Move, m);

		ClutLoadBody c = {};
		c.tex0 = 0x0102030405060708ull;
		c.texclut = 0x1112131415161718ull;
		SubmitFixed(RecordType::ClutLoad, c);

		LocalToHostBody r = {};
		r.blit = 0x9;
		r.pos = 0xA;
		r.reg = 0xB;
		r.draw_serial = 1;
		r.len = 256;
		r.total = 256;
		r.end = 0;
		SubmitFixed(RecordType::LocalToHost, r);

		for (u32 i = 0; i < draw_count; i++)
		{
			SynthDraw d = MakeDraw(100 + i, 6, 12);
			if (i == 1)
				perturb(d);
			SubmitDraw(d);
		}
	}

	void EmitStream(u32 draw_count = 2)
	{
		EmitStreamPerturbed([](SynthDraw&) {}, draw_count);
	}
} // namespace

// ----------------------------------------------------------------------------
// 1. Layout
// ----------------------------------------------------------------------------

TEST(GSFeDecode, TheRecordLayoutIsPinned)
{
	// Bump these together with kFormatVersion, never separately.
	EXPECT_EQ(kFormatVersion, 1u);
	EXPECT_EQ(kEnvRegWords, 15u);
	EXPECT_EQ(kCtxRegWords, 12u);
	EXPECT_EQ(kEnvSnapshotWords, 39u);
	EXPECT_EQ(kPayloadHeadBytes, 32u);
	EXPECT_EQ(kCanonicalPrefixBytes, 16u);

	EXPECT_EQ(sizeof(StreamHeader), 64u);
	EXPECT_EQ(offsetof(StreamHeader, magic), 0u);
	EXPECT_EQ(offsetof(StreamHeader, format_version), 8u);
	EXPECT_EQ(offsetof(StreamHeader, header_bytes), 12u);
	EXPECT_EQ(offsetof(StreamHeader, record_header_bytes), 16u);
	EXPECT_EQ(offsetof(StreamHeader, vertex_stride), 20u);
	EXPECT_EQ(offsetof(StreamHeader, env_reg_words), 24u);
	EXPECT_EQ(offsetof(StreamHeader, ctx_reg_words), 28u);
	EXPECT_EQ(offsetof(StreamHeader, build), 32u);

	EXPECT_EQ(sizeof(RecordHeader), 32u);
	EXPECT_EQ(offsetof(RecordHeader, type), 0u);
	EXPECT_EQ(offsetof(RecordHeader, body_bytes), 4u);
	EXPECT_EQ(offsetof(RecordHeader, event_index), 8u);
	EXPECT_EQ(offsetof(RecordHeader, record_digest), 16u);
	EXPECT_EQ(offsetof(RecordHeader, stream_digest), 24u);

	EXPECT_EQ(sizeof(EnvSnapshot), 312u);

	EXPECT_EQ(sizeof(DrawBody), 728u);
	EXPECT_EQ(offsetof(DrawBody, draw_serial), 0u);
	EXPECT_EQ(offsetof(DrawBody, draw_env), 8u);
	EXPECT_EQ(offsetof(DrawBody, next_env), 320u);
	EXPECT_EQ(offsetof(DrawBody, next_v), 632u);
	EXPECT_EQ(offsetof(DrawBody, draw_rect), 664u);
	EXPECT_EQ(offsetof(DrawBody, backed_up_ctx), 680u);
	EXPECT_EQ(offsetof(DrawBody, dirty_gs_regs), 684u);
	EXPECT_EQ(offsetof(DrawBody, flush_reason), 688u);
	EXPECT_EQ(offsetof(DrawBody, channel_shuffle_finish), 692u);
	EXPECT_EQ(offsetof(DrawBody, packed_uv_hack_flag), 693u);
	EXPECT_EQ(offsetof(DrawBody, prim), 696u);
	EXPECT_EQ(offsetof(DrawBody, ctxt), 704u);
	EXPECT_EQ(offsetof(DrawBody, vertex_head), 708u);
	EXPECT_EQ(offsetof(DrawBody, vertex_tail), 712u);
	EXPECT_EQ(offsetof(DrawBody, vertex_next), 716u);
	EXPECT_EQ(offsetof(DrawBody, vertex_count), 720u);
	EXPECT_EQ(offsetof(DrawBody, index_count), 724u);

	EXPECT_EQ(sizeof(TransferBody), 128u);
	EXPECT_EQ(offsetof(TransferBody, blit), 0u);
	EXPECT_EQ(offsetof(TransferBody, payload_digest), 40u);
	EXPECT_EQ(offsetof(TransferBody, rect), 48u);
	EXPECT_EQ(offsetof(TransferBody, len), 64u);
	EXPECT_EQ(offsetof(TransferBody, first_slice), 88u);
	EXPECT_EQ(offsetof(TransferBody, payload_head), 92u);

	EXPECT_EQ(sizeof(MoveBody), 32u);
	EXPECT_EQ(sizeof(ClutLoadBody), 16u);
	EXPECT_EQ(sizeof(LocalToHostBody), 48u);
	EXPECT_EQ(sizeof(VsyncBody), 16u);
	EXPECT_EQ(offsetof(VsyncBody, draw_serial), 0u);
	EXPECT_EQ(offsetof(VsyncBody, field), 8u);
	EXPECT_EQ(offsetof(VsyncBody, registers_written), 12u);
	EXPECT_EQ(offsetof(VsyncBody, idle_frame), 13u);

	// The vertex payload is raw GSVertex bytes; a stride change would silently
	// reinterpret every recording ever taken.
	EXPECT_EQ(sizeof(GSVertex), 32u);
}

// ----------------------------------------------------------------------------
// 2. The file header
// ----------------------------------------------------------------------------

TEST(GSFeDecode, TheStreamHeaderNamesTheFormat)
{
	const std::string path = ScratchPath("armsx2_fedecode_header.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EXPECT_TRUE(IsActive());
	EmitStream();
	End();
	EXPECT_FALSE(IsActive());

	StreamHeader header = {};
	std::FILE* f = std::fopen(path.c_str(), "rb");
	ASSERT_NE(f, nullptr);
	ASSERT_EQ(std::fread(&header, sizeof(header), 1, f), 1u);
	std::fclose(f);

	EXPECT_EQ(std::memcmp(header.magic, "ARMSFED", 8), 0);
	EXPECT_EQ(header.format_version, kFormatVersion);
	EXPECT_EQ(header.header_bytes, sizeof(StreamHeader));
	EXPECT_EQ(header.record_header_bytes, sizeof(RecordHeader));
	EXPECT_EQ(header.vertex_stride, sizeof(GSVertex));
	EXPECT_EQ(header.env_reg_words, kEnvRegWords);
	EXPECT_EQ(header.ctx_reg_words, kCtxRegWords);
	EXPECT_STREQ(header.build, "unit-test-build");

	std::filesystem::remove(path);
}

TEST(GSFeDecode, AForeignHeaderIsRefusedRatherThanReinterpreted)
{
	const std::string path = ScratchPath("armsx2_fedecode_version.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream();
	End();

	// A version this build does not know.
	{
		std::FILE* f = std::fopen(path.c_str(), "r+b");
		ASSERT_NE(f, nullptr);
		ASSERT_EQ(std::fseek(f, offsetof(StreamHeader, format_version), SEEK_SET), 0);
		const u32 bogus = kFormatVersion + 1;
		ASSERT_EQ(std::fwrite(&bogus, sizeof(bogus), 1, f), 1u);
		std::fclose(f);
	}
	EXPECT_FALSE(BeginDiff(path));
	EXPECT_FALSE(IsActive());

	// A file that is not a decode stream at all.
	{
		std::FILE* f = std::fopen(path.c_str(), "r+b");
		ASSERT_NE(f, nullptr);
		ASSERT_EQ(std::fwrite("NOTAFEDC", 8, 1, f), 1u);
		std::fclose(f);
	}
	EXPECT_FALSE(BeginDiff(path));
	EXPECT_FALSE(IsActive());

	std::filesystem::remove(path);
}

// ----------------------------------------------------------------------------
// 3. The differ agrees with itself
// ----------------------------------------------------------------------------

TEST(GSFeDecode, AnIdenticalStreamDiffsGreen)
{
	const std::string path = ScratchPath("armsx2_fedecode_green.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream();
	const u64 recorded = EventCount();
	End();
	EXPECT_EQ(recorded, 7u); // vsync + transfer + move + clut + read + 2 draws

	ASSERT_TRUE(BeginDiff(path));
	EmitStream();
	EXPECT_EQ(EventCount(), recorded);
	EXPECT_FALSE(Diverged());
	End();
	EXPECT_FALSE(Diverged());

	std::filesystem::remove(path);
}

// ----------------------------------------------------------------------------
// 4. The differ can fail, and says where
// ----------------------------------------------------------------------------

TEST(GSFeDecode, AFlippedVertexByteIsLocated)
{
	const std::string path = ScratchPath("armsx2_fedecode_vertex.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream();
	End();

	// Vertex 3's XYZ.Z is at byte 20 of a 32-byte vertex.
	const u32 vertex_index = 3;
	const u32 in_vertex = 20;
	const u32 tail_offset = vertex_index * static_cast<u32>(sizeof(GSVertex)) + in_vertex;

	ASSERT_TRUE(BeginDiff(path));
	EmitStreamPerturbed([&](SynthDraw& d) { d.tail[tail_offset] ^= 0x40; }, 2);
	End();

	ASSERT_TRUE(Diverged());
	const std::string& report = DivergenceReport();
	EXPECT_NE(report.find("record type  : Draw"), std::string::npos) << report;
	EXPECT_NE(report.find("event index  : 6"), std::string::npos) << report;
	EXPECT_NE(report.find("draw serial  : 101"), std::string::npos) << report;
	EXPECT_NE(report.find("vertices[3].XYZ.Z+0"), std::string::npos) << report;
	// Body offset = the fixed part plus the tail offset.
	EXPECT_NE(report.find(std::to_string(sizeof(DrawBody) + tail_offset)), std::string::npos) << report;
	// The frame counter comes off the one Vsync record in the stream.
	EXPECT_NE(report.find("frame        : 1"), std::string::npos) << report;

	std::filesystem::remove(path);
}

TEST(GSFeDecode, AFlippedIndexIsLocated)
{
	const std::string path = ScratchPath("armsx2_fedecode_index.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream();
	End();

	const u32 tail_offset = 6 * static_cast<u32>(sizeof(GSVertex)) + 5 * 2; // indices[5], low byte

	ASSERT_TRUE(BeginDiff(path));
	EmitStreamPerturbed([&](SynthDraw& d) { d.tail[tail_offset] ^= 0x01; }, 2);
	End();

	ASSERT_TRUE(Diverged());
	EXPECT_NE(DivergenceReport().find("indices[5]+0"), std::string::npos) << DivergenceReport();

	std::filesystem::remove(path);
}

TEST(GSFeDecode, AFlippedRegisterWordIsLocated)
{
	const std::string path = ScratchPath("armsx2_fedecode_reg.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream();
	End();

	// CTXT[1].TEST is env word 15 + 12 + 8 = 35.
	const u32 word = kEnvRegWords + kCtxRegWords + 8;

	ASSERT_TRUE(BeginDiff(path));
	EmitStreamPerturbed([&](SynthDraw& d) { d.body.draw_env.w[word] ^= 0x0000010000000000ull; }, 2);
	End();

	ASSERT_TRUE(Diverged());
	const std::string& report = DivergenceReport();
	EXPECT_NE(report.find("draw_env.CTXT[1].TEST+5"), std::string::npos) << report;
	EXPECT_NE(report.find(std::to_string(offsetof(DrawBody, draw_env) + word * 8 + 5)), std::string::npos) << report;

	std::filesystem::remove(path);
}

TEST(GSFeDecode, AFlippedTransferFieldIsLocated)
{
	const std::string path = ScratchPath("armsx2_fedecode_transfer.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream();
	End();

	ASSERT_TRUE(BeginDiff(path));
	{
		VsyncBody v = {};
		v.field = 1;
		v.registers_written = 1;
		SubmitFixed(RecordType::Vsync, v);

		TransferBody t = MakeTransfer(1);
		t.payload_digest ^= 0x1ull;
		SubmitFixed(RecordType::Transfer, t);
	}
	End();

	ASSERT_TRUE(Diverged());
	const std::string& report = DivergenceReport();
	EXPECT_NE(report.find("record type  : Transfer"), std::string::npos) << report;
	EXPECT_NE(report.find("event index  : 1"), std::string::npos) << report;
	EXPECT_NE(report.find("payload_digest"), std::string::npos) << report;

	std::filesystem::remove(path);
}

TEST(GSFeDecode, AWrongRecordTypeIsNamedOnBothSides)
{
	const std::string path = ScratchPath("armsx2_fedecode_type.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream();
	End();

	ASSERT_TRUE(BeginDiff(path));
	{
		// The recording opens with a Vsync; hand the differ a Move instead.
		MoveBody m = {};
		SubmitFixed(RecordType::Move, m);
	}
	End();

	ASSERT_TRUE(Diverged());
	const std::string& report = DivergenceReport();
	EXPECT_NE(report.find("expected Vsync"), std::string::npos) << report;
	EXPECT_NE(report.find("got Move"), std::string::npos) << report;

	std::filesystem::remove(path);
}

TEST(GSFeDecode, AShortReplayIsADivergence)
{
	const std::string path = ScratchPath("armsx2_fedecode_short.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream(4);
	End();

	ASSERT_TRUE(BeginDiff(path));
	EmitStream(1);
	EXPECT_FALSE(Diverged()); // everything compared so far still matches
	End(); // the shortfall is only visible at the end
	ASSERT_TRUE(Diverged());
	EXPECT_NE(DivergenceReport().find("stream ended early"), std::string::npos) << DivergenceReport();

	std::filesystem::remove(path);
}

TEST(GSFeDecode, ALongReplayIsADivergence)
{
	const std::string path = ScratchPath("armsx2_fedecode_long.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream(1);
	End();

	ASSERT_TRUE(BeginDiff(path));
	EmitStream(4);
	ASSERT_TRUE(Diverged());
	EXPECT_NE(DivergenceReport().find("recording ran out"), std::string::npos) << DivergenceReport();
	End();

	std::filesystem::remove(path);
}

TEST(GSFeDecode, AShorterDrawIsADivergence)
{
	const std::string path = ScratchPath("armsx2_fedecode_size.fedec");
	ASSERT_TRUE(BeginRecord(path, "unit-test-build"));
	EmitStream();
	End();

	ASSERT_TRUE(BeginDiff(path));
	{
		VsyncBody v = {};
		v.field = 1;
		v.registers_written = 1;
		SubmitFixed(RecordType::Vsync, v);
		SubmitFixed(RecordType::Transfer, MakeTransfer(1));
		MoveBody m = {};
		m.blit = 0xABCDEF0011223344ull;
		m.pos = 0x1;
		m.reg = 0x2;
		m.draw_serial = 1;
		SubmitFixed(RecordType::Move, m);
		ClutLoadBody c = {};
		c.tex0 = 0x0102030405060708ull;
		c.texclut = 0x1112131415161718ull;
		SubmitFixed(RecordType::ClutLoad, c);
		LocalToHostBody r = {};
		r.blit = 0x9;
		r.pos = 0xA;
		r.reg = 0xB;
		r.draw_serial = 1;
		r.len = 256;
		r.total = 256;
		SubmitFixed(RecordType::LocalToHost, r);
		SubmitDraw(MakeDraw(100, 5, 12)); // one vertex short
	}
	End();

	ASSERT_TRUE(Diverged());
	EXPECT_NE(DivergenceReport().find("body bytes   : expected"), std::string::npos) << DivergenceReport();

	std::filesystem::remove(path);
}

// ----------------------------------------------------------------------------
// 5. The locator, directly
// ----------------------------------------------------------------------------

TEST(GSFeDecode, DescribeOffsetNamesEveryRegion)
{
	const SynthDraw d = MakeDraw(1, 4, 6);
	const void* body = &d.body;
	const u32 body_bytes = static_cast<u32>(sizeof(DrawBody) + d.tail.size());

	EXPECT_EQ(DescribeOffset(RecordType::Draw, 0, body, body_bytes), "draw_serial");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, offsetof(DrawBody, draw_env), body, body_bytes), "draw_env.PRIM+0");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, offsetof(DrawBody, draw_env) + 14 * 8 + 3, body, body_bytes),
		"draw_env.TRXREG+3");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, offsetof(DrawBody, draw_env) + 15 * 8, body, body_bytes),
		"draw_env.CTXT[0].XYOFFSET+0");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, offsetof(DrawBody, next_env) + 26 * 8, body, body_bytes),
		"next_env.CTXT[0].ZBUF+0");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, offsetof(DrawBody, next_env) + 38 * 8 + 7, body, body_bytes),
		"next_env.CTXT[1].ZBUF+7");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, offsetof(DrawBody, vertex_count), body, body_bytes), "vertex_count");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, offsetof(DrawBody, draw_rect) + 4, body, body_bytes), "draw_rect+4");

	// Tail: 4 vertices, then 6 indices.
	EXPECT_EQ(DescribeOffset(RecordType::Draw, sizeof(DrawBody) + 0, body, body_bytes), "vertices[0].ST.S+0");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, sizeof(DrawBody) + 2 * 32 + 26, body, body_bytes), "vertices[2].V+0");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, sizeof(DrawBody) + 3 * 32 + 31, body, body_bytes), "vertices[3].FOG+3");
	EXPECT_EQ(DescribeOffset(RecordType::Draw, sizeof(DrawBody) + 4 * 32 + 2, body, body_bytes), "indices[1]+0");
	// 4 vertices (128 B) + 6 indices (12 B) = 140, padded to 144: the last 4 bytes
	// are padding and the locator says so rather than inventing an index.
	EXPECT_EQ(DescribeOffset(RecordType::Draw, sizeof(DrawBody) + 140, body, body_bytes), "tail_pad+0");

	const TransferBody t = MakeTransfer(1);
	EXPECT_EQ(DescribeOffset(RecordType::Transfer, offsetof(TransferBody, env_blit), &t, sizeof(t)), "env_blit");
	EXPECT_EQ(DescribeOffset(RecordType::Transfer, offsetof(TransferBody, payload_head) + 9, &t, sizeof(t)),
		"payload_head+9");

	EXPECT_STREQ(TypeName(RecordType::Draw), "Draw");
	EXPECT_STREQ(TypeName(RecordType::LocalToHost), "LocalToHost");
}
