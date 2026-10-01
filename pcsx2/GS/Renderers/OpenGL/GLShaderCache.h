// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "GS/GSCacheFile.h"
#include "GS/Renderers/OpenGL/GLProgram.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class GLShaderCache
{
public:
	using PreLinkCallback = std::function<void(GLProgram&)>;

	GLShaderCache();
	~GLShaderCache();

	bool Open(bool is_gles);
	void Close();

	std::optional<GLProgram> GetProgram(const std::string_view vertex_shader, const std::string_view fragment_shader,
		const PreLinkCallback& callback = {});
	bool GetProgram(GLProgram* out_program, const std::string_view vertex_shader,
		const std::string_view fragment_shader, const PreLinkCallback& callback = {});

	std::optional<GLProgram> GetComputeProgram(const std::string_view glsl, const PreLinkCallback& callback = {});
	bool GetComputeProgram(GLProgram* out_program, const std::string_view glsl, const PreLinkCallback& callback = {});

private:
	static constexpr u32 KEY_SIZE = 2 * (sizeof(GSCacheFile::Digest) + sizeof(u32)) + sizeof(u32);
	using CacheKey = std::array<u8, KEY_SIZE>;

	static CacheKey GetCacheKey(const std::string_view vertex_shader, const std::string_view fragment_shader, bool compute);

	std::optional<GLProgram> GetCachedProgram(const CacheKey& key);

	std::optional<GLProgram> CompileProgram(const std::string_view vertex_shader,
		const std::string_view fragment_shader, const PreLinkCallback& callback, bool set_retrievable);
	std::optional<GLProgram> CompileComputeProgram(
		const std::string_view glsl, const PreLinkCallback& callback, bool set_retrievable);
	void AddProgram(const CacheKey& key, GLProgram& prog);

	GSCacheFile::BlobStore m_store;
	bool m_program_binary_supported = false;
};
