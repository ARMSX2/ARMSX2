// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// A VU1 pack under a temporary cache folder, for a scope: the chunk store's
// bodies are written to it, and after a reset they come back from it. The
// caller turns recording on first (the pack's name follows the options).

#include "Config.h"
#include "arm64/microVU_ProgCache-arm64.h"

#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

namespace recompiler_tests
{
struct ChunkPackScope
{
	std::string root;
	const std::string prevCache = EmuFolders::Cache;
	const bool prevSetting = EmuConfig.Cpu.Recompiler.EnableVUProgramCache;

	ChunkPackScope()
	{
		char tmpl[] = "/tmp/vu1_chunk_pack_scope_XXXXXX";
		if (const char* p = ::mkdtemp(tmpl))
			root = p;
		EmuFolders::Cache = root;
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = true;
		mVUProgCache::ResetForTest(1);
		mVUProgCache::TestReinitFromLiveSentinel(1);
	}
	~ChunkPackScope()
	{
		mVUProgCache::ResetForTest(1);
		EmuConfig.Cpu.Recompiler.EnableVUProgramCache = prevSetting;
		EmuFolders::Cache = prevCache;
		if (!root.empty())
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}
	}
	bool Up() const { return mVUProgCache::GetStats(1).enabled; }
};
} // namespace recompiler_tests
