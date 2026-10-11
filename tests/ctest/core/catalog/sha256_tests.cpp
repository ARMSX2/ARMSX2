// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "catalog_test_support.h"

#include "common/Error.h"
#include "common/SHA256.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>

// The FIPS 180-2 / NIST example vectors.
TEST(SHA256, EmptyMessage)
{
	EXPECT_EQ(SHA256Digest::HashHex(std::string_view()),
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(SHA256, Abc)
{
	EXPECT_EQ(SHA256Digest::HashHex(std::string_view("abc")),
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(SHA256, FourHundredFortyEightBitMessage)
{
	EXPECT_EQ(SHA256Digest::HashHex(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(SHA256, EightHundredNinetySixBitMessage)
{
	EXPECT_EQ(SHA256Digest::HashHex(std::string_view("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
		"cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(SHA256, OneMillionA)
{
	const std::string input(1000000, 'a');
	EXPECT_EQ(SHA256Digest::HashHex(input), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(SHA256, ChunkSizesDoNotMatter)
{
	const std::string input(1000000, 'a');
	const std::string expected = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

	// Chunk sizes on both sides of the 64-byte block boundary exercise the partial-block buffer.
	for (const size_t chunk : {1u, 7u, 63u, 64u, 65u, 1000u, 4096u})
	{
		SHA256Digest sha;
		for (size_t pos = 0; pos < input.size(); pos += chunk)
			sha.Update(input.data() + pos, std::min(chunk, input.size() - pos));
		EXPECT_EQ(sha.FinalHex(), expected) << "chunk size " << chunk;
	}
}

TEST(SHA256, ResetStartsOver)
{
	SHA256Digest sha;
	sha.Update(std::string_view("garbage that must not leak into the next digest"));
	sha.Reset();
	sha.Update(std::string_view("abc"));
	EXPECT_EQ(sha.FinalHex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(SHA256, ToHexIsLowercase)
{
	SHA256Digest::Bytes bytes{};
	bytes[0] = 0xAB;
	bytes[1] = 0x0F;
	bytes[31] = 0xFF;
	const std::string hex = SHA256Digest::ToHex(bytes);
	EXPECT_EQ(hex.size(), 64u);
	EXPECT_EQ(hex.substr(0, 4), "ab0f");
	EXPECT_EQ(hex.substr(62), "ff");
}

TEST(SHA256, HashFile)
{
	std::string hex;
	Error error;
	EXPECT_EQ(SHA256Digest::HashFile(CatalogTest::FixturePath("sha256_abc.txt").c_str(), &hex, &error),
		SHA256Digest::FileResult::Ok);
	EXPECT_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(SHA256, HashFileLargerThanOneChunk)
{
	CatalogTest::ScopedTempDir dir;
	const std::string input(3 * 1024 * 1024 + 17, 'a');
	ASSERT_TRUE(FileSystem::WriteBinaryFile(dir.File("big.bin").c_str(), input.data(), input.size()));

	std::string hex;
	EXPECT_EQ(SHA256Digest::HashFile(dir.File("big.bin").c_str(), &hex), SHA256Digest::FileResult::Ok);
	EXPECT_EQ(hex, SHA256Digest::HashHex(input));
}

TEST(SHA256, HashFileMissing)
{
	std::string hex = "untouched";
	Error error;
	EXPECT_EQ(SHA256Digest::HashFile(CatalogTest::FixturePath("no-such-file").c_str(), &hex, &error),
		SHA256Digest::FileResult::Failed);
	EXPECT_TRUE(error.IsValid());
	EXPECT_EQ(hex, "untouched");
}

TEST(SHA256, HashFileCancelled)
{
	std::string hex = "untouched";
	std::atomic_bool cancel{true};
	EXPECT_EQ(SHA256Digest::HashFile(CatalogTest::FixturePath("sha256_abc.txt").c_str(), &hex, nullptr, &cancel),
		SHA256Digest::FileResult::Cancelled);
	EXPECT_EQ(hex, "untouched");
}
