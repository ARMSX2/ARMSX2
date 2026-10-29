// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
#include <atomic>
#include <string>
#include <string_view>

class Error;

// Streaming SHA-256 over the 7-Zip implementation vendored in 3rdparty/lzma (public domain), using
// the ARMv8 SHA2 / x86 SHA-NI instructions at runtime when the CPU has them. No 7-Zip header is
// included here.
//
// After Final()/FinalHex() the object holds no usable state until Reset() is called.
class SHA256Digest
{
public:
	static constexpr size_t DIGEST_SIZE = 32;
	using Bytes = std::array<u8, DIGEST_SIZE>;

	SHA256Digest();

	void Reset();
	void Update(const void* data, size_t size);
	void Update(std::string_view data) { Update(data.data(), data.size()); }

	Bytes Final();
	std::string FinalHex();

	static Bytes Hash(const void* data, size_t size);
	static std::string HashHex(const void* data, size_t size);
	static std::string HashHex(std::string_view data) { return HashHex(data.data(), data.size()); }

	/// Lowercase hex digits.
	static std::string ToHex(const Bytes& digest);

	enum class FileResult
	{
		Ok,
		Cancelled,
		Failed,
	};

	/// Hashes a whole file, reading it in 1 MiB chunks. If `cancel` is non-null it is checked
	/// between chunks and the call returns Cancelled once it reads true. On Failed, `error`
	/// (if given) says why. `hex_out` is only written on Ok.
	static FileResult HashFile(const char* path, std::string* hex_out, Error* error = nullptr,
		const std::atomic_bool* cancel = nullptr);

private:
	// Holds a CSha256 from 3rdparty/lzma; SHA256.cpp static_asserts that it fits.
	alignas(8) u8 m_state[128];
};
