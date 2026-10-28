// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include "GS/GSCacheFile.h"
#include "GS/Renderers/OpenGL/GLProgram.h"

#include <array>
#include <memory>
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
	/// Writes what changed within the close deadline and drops the packs.
	void Close();

	/// Opens the program pack of the game the GS runs now (an empty serial: none), reading it whole.
	void SetGame(const std::string& serial, u32 crc);

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

	/// Program binaries by source: the device's own programs, and the running game's. GS thread,
	/// apart from the writer's thread writing them.
	GSCacheFile::PackWriter m_writer;
	GSCacheFile::Stamp m_stamp;
	std::string m_stem;
	std::shared_ptr<GSCacheFile::Pack> m_device_pack;
	std::shared_ptr<GSCacheFile::Pack> m_game_pack;
	bool m_program_binary_supported = false;
};
