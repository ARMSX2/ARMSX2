// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLShaderCache.h"
#include "GS/GS.h"
#include "GS/GSShaderCompileIndicator.h"

#include "Config.h"
#include "ShaderCacheVersion.h"

#include "common/Console.h"
#include "common/Path.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <cstring>

GLShaderCache::GLShaderCache() = default;

GLShaderCache::~GLShaderCache()
{
	Close();
}

bool GLShaderCache::Open(bool is_gles)
{
	m_program_binary_supported = GLAD_GL_ARB_get_program_binary || is_gles;
	GLint num_formats = 0;
	if (m_program_binary_supported)
	{
		// check that there's at least one format and the extension isn't being "faked"
		glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &num_formats);
		Console.WriteLn("%u program binary formats supported by driver", num_formats);
		m_program_binary_supported = (num_formats > 0);
	}

	if (!m_program_binary_supported)
	{
		Console.Warning("Your GL driver does not support program binaries. Hopefully it has a built-in cache.");
		return true;
	}

	if (GSConfig.DisableShaderCache)
		return true;

	// Program binaries are driver binaries, valid only for the driver that produced them: a foreign
	// one (the device's own GLES driver, then ANGLE) crashes some drivers on the first cached draw
	// instead of failing to load. So the driver's strings and binary formats are the stamp.
	//
	// No build identity: an entry's key is the full vertex and fragment source text, and programs
	// linked with a pre-link callback (link state the source does not show) are never cached, so
	// the same key always means the same program.
	const auto gl_string = [](GLenum name) {
		const char* str = reinterpret_cast<const char*>(glGetString(name));
		return std::string_view(str ? str : "");
	};
	std::vector<GLint> formats(static_cast<size_t>(num_formats));
	glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, formats.data());

	GSCacheFile::Stamp stamp;
	stamp.Add("shader_cache_version", SHADER_CACHE_VERSION);
	stamp.Add("api", is_gles ? "gles" : "gl");
	stamp.Add("vendor", gl_string(GL_VENDOR));
	stamp.Add("renderer", gl_string(GL_RENDERER));
	stamp.Add("version", gl_string(GL_VERSION));
	stamp.Add("glsl_version", gl_string(GL_SHADING_LANGUAGE_VERSION));
	stamp.AddHex("binary_formats", formats.data(), formats.size() * sizeof(GLint));

	// Named for the driver, so switching between two (the device's GLES driver and ANGLE) keeps both.
	GSCacheFile::CleanStaleTempFiles(EmuFolders::Cache);
	const std::string stem = "gl_programs_" + GSCacheFile::ShortName(stamp.GetDigest());
	GSCacheFile::PruneOtherIdentities(EmuFolders::Cache, "gl_programs", stem, 3);
	if (!m_store.Open(Path::Combine(EmuFolders::Cache, stem), GSCacheFile::KIND_GL_PROGRAMS, stamp, KEY_SIZE))
		Console.Warning("GL: running without a program cache");

	return true;
}

void GLShaderCache::Close()
{
	m_store.Close();
}

GLShaderCache::CacheKey GLShaderCache::GetCacheKey(
	const std::string_view vertex_shader, const std::string_view fragment_shader, bool compute)
{
	CacheKey key = {};
	const GSCacheFile::Digest vs = GSCacheFile::Hash128(vertex_shader.data(), vertex_shader.size());
	const GSCacheFile::Digest fs = GSCacheFile::Hash128(fragment_shader.data(), fragment_shader.size());
	const u32 vs_length = static_cast<u32>(vertex_shader.size());
	const u32 fs_length = static_cast<u32>(fragment_shader.size());
	const u32 type = compute ? 1 : 0;
	u8* p = key.data();
	std::memcpy(p, vs.bytes, sizeof(vs.bytes));
	std::memcpy(p + 16, &vs_length, sizeof(u32));
	std::memcpy(p + 20, fs.bytes, sizeof(fs.bytes));
	std::memcpy(p + 36, &fs_length, sizeof(u32));
	std::memcpy(p + 40, &type, sizeof(u32));
	return key;
}

std::optional<GLProgram> GLShaderCache::GetCachedProgram(const CacheKey& key)
{
	std::vector<u8> data;
	u32 format = 0;
	if (!m_store.Lookup(key.data(), &data, &format))
		return std::nullopt;

#ifdef PCSX2_DEVBUILD
	Common::Timer timer;
#endif

	GLProgram prog;
	if (prog.CreateFromBinary(data.data(), static_cast<u32>(data.size()), format))
	{
#ifdef PCSX2_DEVBUILD
		Console.WriteLn("Time to create program from binary: %.2fms", timer.GetTimeMilliseconds());
#endif
		return std::optional<GLProgram>(std::move(prog));
	}

	// The entry passed its checksum, so the driver itself refused a binary it made: it changed
	// without its strings changing. Nothing else in the store can be trusted either.
	Console.Warning(
		"Failed to create program from binary, this may be due to a driver or GPU Change. Recreating cache.");
	m_store.Recreate();
	return std::nullopt;
}

std::optional<GLProgram> GLShaderCache::GetProgram(
	const std::string_view vertex_shader, const std::string_view fragment_shader, const PreLinkCallback& callback)
{
	// A pre-link callback sets link state the source does not show, so such a program is not
	// content-addressed by its key and is never cached.
	if (!m_program_binary_supported || !m_store.IsOpen() || callback)
	{
#ifdef PCSX2_DEVBUILD
		Common::Timer timer;
#endif

		std::optional<GLProgram> res = CompileProgram(vertex_shader, fragment_shader, callback, false);

#ifdef PCSX2_DEVBUILD
		Console.WriteLn("Time to compile shader without caching: %.2fms", timer.GetTimeMilliseconds());
#endif
		return res;
	}

	const CacheKey key = GetCacheKey(vertex_shader, fragment_shader, false);
	if (std::optional<GLProgram> cached = GetCachedProgram(key))
		return cached;

	std::optional<GLProgram> prog = CompileProgram(vertex_shader, fragment_shader, callback, true);
	if (prog.has_value())
		AddProgram(key, *prog);
	return prog;
}

bool GLShaderCache::GetProgram(GLProgram* out_program, const std::string_view vertex_shader,
	const std::string_view fragment_shader, const PreLinkCallback& callback /* = */)
{
	auto prog = GetProgram(vertex_shader, fragment_shader, callback);
	if (!prog)
		return false;

	*out_program = std::move(*prog);
	return true;
}

void GLShaderCache::AddProgram(const CacheKey& key, GLProgram& prog)
{
	if (!m_store.IsWritable())
		return;

	std::vector<u8> prog_data;
	u32 prog_format = 0;
	if (!prog.GetBinary(&prog_data, &prog_format))
		return;

	m_store.Insert(key.data(), prog_data.data(), prog_data.size(), prog_format);
}

std::optional<GLProgram> GLShaderCache::CompileProgram(const std::string_view vertex_shader,
	const std::string_view fragment_shader, const PreLinkCallback& callback, bool set_retrievable)
{
	const GSShaderCompileIndicator::CompileTimer compile_timer;

	GLProgram prog;
	if (!prog.Compile(vertex_shader, fragment_shader))
		return std::nullopt;

	if (callback)
		callback(prog);

	if (set_retrievable)
		prog.SetBinaryRetrievableHint();

	if (!prog.Link())
		return std::nullopt;

	return std::optional<GLProgram>(std::move(prog));
}

std::optional<GLProgram> GLShaderCache::CompileComputeProgram(
	const std::string_view glsl, const PreLinkCallback& callback, bool set_retrievable)
{
	const GSShaderCompileIndicator::CompileTimer compile_timer;

	GLProgram prog;
	if (!prog.CompileCompute(glsl))
		return std::nullopt;

	if (callback)
		callback(prog);

	if (set_retrievable)
		prog.SetBinaryRetrievableHint();

	if (!prog.Link())
		return std::nullopt;

	return std::optional<GLProgram>(std::move(prog));
}

std::optional<GLProgram> GLShaderCache::GetComputeProgram(const std::string_view glsl, const PreLinkCallback& callback)
{
	if (!m_program_binary_supported || !m_store.IsOpen() || callback)
	{
#ifdef PCSX2_DEVBUILD
		Common::Timer timer;
#endif

		std::optional<GLProgram> res = CompileComputeProgram(glsl, callback, false);

#ifdef PCSX2_DEVBUILD
		Console.WriteLn("Time to compile shader without caching: %.2fms", timer.GetTimeMilliseconds());
#endif
		return res;
	}

	const CacheKey key = GetCacheKey(glsl, std::string_view(), true);
	if (std::optional<GLProgram> cached = GetCachedProgram(key))
		return cached;

	std::optional<GLProgram> prog = CompileComputeProgram(glsl, callback, true);
	if (prog.has_value())
		AddProgram(key, *prog);
	return prog;
}

bool GLShaderCache::GetComputeProgram(
	GLProgram* out_program, const std::string_view glsl, const PreLinkCallback& callback)
{
	auto prog = GetComputeProgram(glsl, callback);
	if (!prog)
		return false;

	*out_program = std::move(*prog);
	return true;
}
