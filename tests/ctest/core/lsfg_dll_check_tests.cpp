// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// GSLsfg::LooksLikeLosslessDll is what stands between an import and the config: the Android
// picker accepts or rejects the user's file on its verdict, and a wrong answer either loses a
// legitimate Lossless.dll or lets a bogus one sit in the config to fail mid-frame. The check
// itself is unconditional (only GetUnavailableReason is gated on ARMSX2_HAS_LSFG), so it can be
// pinned on any host.

#include "GS/Renderers/Vulkan/GSLsfg.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	// A file that is deleted when the test scope ends, whatever the assertions did.
	class ScopedTestFile
	{
	public:
		explicit ScopedTestFile(const char* name, const std::vector<u8>& bytes)
		{
			m_path = Path::Combine(FileSystem::GetWorkingDirectory(), name);
			m_ok = FileSystem::WriteBinaryFile(m_path.c_str(), bytes.data(), bytes.size());
		}
		~ScopedTestFile() { FileSystem::DeleteFilePath(m_path.c_str()); }

		const std::string& GetPath() const { return m_path; }
		bool IsValid() const { return m_ok; }

	private:
		std::string m_path;
		bool m_ok = false;
	};

	std::vector<u8> MinimalPE(u32 e_lfanew, const char* signature = "PE\0\0")
	{
		// At least the 0x40-byte DOS header, so a small e_lfanew is rejected for pointing
		// inside it rather than for the file being short.
		std::vector<u8> bytes(std::max<size_t>(e_lfanew + 4, 0x40), 0);
		bytes[0] = 'M';
		bytes[1] = 'Z';
		std::memcpy(&bytes[0x3C], &e_lfanew, sizeof(e_lfanew));
		std::memcpy(&bytes[e_lfanew], signature, 4);
		return bytes;
	}
} // namespace

TEST(LsfgDllCheck, RejectsMissingFile)
{
	ASSERT_FALSE(GSLsfg::LooksLikeLosslessDll("/nonexistent/Lossless.dll"));
}

TEST(LsfgDllCheck, RejectsEmptyFile)
{
	ScopedTestFile file("lsfg_test_empty.bin", {});
	ASSERT_TRUE(file.IsValid());
	ASSERT_FALSE(GSLsfg::LooksLikeLosslessDll(file.GetPath()));
}

TEST(LsfgDllCheck, RejectsPlainText)
{
	const char* text = "This is not a portable executable, whatever its extension says.";
	ScopedTestFile file("lsfg_test_text.bin",
		std::vector<u8>(text, text + std::strlen(text)));
	ASSERT_TRUE(file.IsValid());
	ASSERT_FALSE(GSLsfg::LooksLikeLosslessDll(file.GetPath()));
}

TEST(LsfgDllCheck, RejectsTruncatedDosHeader)
{
	// "MZ" alone, shorter than the 0x40-byte DOS header the check reads.
	ScopedTestFile file("lsfg_test_truncated.bin", {'M', 'Z'});
	ASSERT_TRUE(file.IsValid());
	ASSERT_FALSE(GSLsfg::LooksLikeLosslessDll(file.GetPath()));
}

TEST(LsfgDllCheck, RejectsPeOffsetInsideDosHeader)
{
	ScopedTestFile file("lsfg_test_low_lfanew.bin", MinimalPE(0x20));
	ASSERT_TRUE(file.IsValid());
	ASSERT_FALSE(GSLsfg::LooksLikeLosslessDll(file.GetPath()));
}

TEST(LsfgDllCheck, RejectsWrongPeSignature)
{
	ScopedTestFile file("lsfg_test_badsig.bin", MinimalPE(0x80, "PF\0\0"));
	ASSERT_TRUE(file.IsValid());
	ASSERT_FALSE(GSLsfg::LooksLikeLosslessDll(file.GetPath()));
}

TEST(LsfgDllCheck, RejectsMzWithTruncatedPeSignature)
{
	// e_lfanew points past the end of the file.
	std::vector<u8> bytes(0x40, 0);
	bytes[0] = 'M';
	bytes[1] = 'Z';
	const u32 e_lfanew = 0x1000;
	std::memcpy(&bytes[0x3C], &e_lfanew, sizeof(e_lfanew));
	ScopedTestFile file("lsfg_test_pe_past_eof.bin", bytes);
	ASSERT_TRUE(file.IsValid());
	ASSERT_FALSE(GSLsfg::LooksLikeLosslessDll(file.GetPath()));
}

TEST(LsfgDllCheck, AcceptsStructurallyValidPe)
{
	// The real Lossless.dll has e_lfanew = 0x130; any offset past the DOS header works.
	ScopedTestFile file("lsfg_test_valid.bin", MinimalPE(0x130));
	ASSERT_TRUE(file.IsValid());
	ASSERT_TRUE(GSLsfg::LooksLikeLosslessDll(file.GetPath()));
}

TEST(LsfgDllPath, GetReturnsWhatSetStored)
{
	GSLsfg::SetDllPath("/some/Lossless.dll");
	ASSERT_EQ(GSLsfg::GetDllPath(), "/some/Lossless.dll");
	GSLsfg::SetDllPath(std::string());
	ASSERT_EQ(GSLsfg::GetDllPath(), std::string());
}

TEST(LsfgDllPath, InvalidateIsSafeWithoutAPath)
{
	// The importer calls this unconditionally, including after a failed copy; it must be a
	// harmless no-op with no path set. The cached verdict itself is only observable on a build
	// with ARMSX2_HAS_LSFG, so behaviour past "does not crash" is pinned by the Android flavour.
	GSLsfg::SetDllPath(std::string());
	GSLsfg::InvalidateDllVerdict();
	ASSERT_EQ(GSLsfg::GetDllPath(), std::string());
}
