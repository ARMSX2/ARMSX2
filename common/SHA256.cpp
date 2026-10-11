// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "common/SHA256.h"
#include "common/Error.h"
#include "common/FileSystem.h"

// 7-Zip's Sha256.h, found through the LZMA::LZMA include directory. Angle brackets are required:
// a quoted include looks beside this file first, and on a case-insensitive filesystem (Windows,
// macOS, iOS) "Sha256.h" is our own SHA256.h, already included above, so none of the 7-Zip
// declarations would exist.
#include <Sha256.h>

#include <memory>
#include <mutex>
#include <new>

static_assert(SHA256_DIGEST_SIZE == SHA256Digest::DIGEST_SIZE);

static CSha256* StateOf(u8* storage)
{
	return std::launder(reinterpret_cast<CSha256*>(storage));
}

SHA256Digest::SHA256Digest()
{
	Reset();
}

void SHA256Digest::Reset()
{
	static_assert(sizeof(CSha256) <= sizeof(m_state) && alignof(CSha256) <= alignof(SHA256Digest),
		"SHA256Digest::m_state cannot hold a CSha256");

	// The hardware code paths are chosen once per process. Until Sha256Prepare() has run,
	// Sha256_Init() selects the (much slower) portable one.
	static std::once_flag prepared;
	std::call_once(prepared, []() { Sha256Prepare(); });

	CSha256* sha = ::new (static_cast<void*>(m_state)) CSha256;
	Sha256_Init(sha);
}

void SHA256Digest::Update(const void* data, size_t size)
{
	if (size > 0)
		Sha256_Update(StateOf(m_state), static_cast<const Byte*>(data), size);
}

SHA256Digest::Bytes SHA256Digest::Final()
{
	Bytes out;
	Sha256_Final(StateOf(m_state), out.data());
	return out;
}

std::string SHA256Digest::FinalHex()
{
	return ToHex(Final());
}

SHA256Digest::Bytes SHA256Digest::Hash(const void* data, size_t size)
{
	SHA256Digest sha;
	sha.Update(data, size);
	return sha.Final();
}

std::string SHA256Digest::HashHex(const void* data, size_t size)
{
	return ToHex(Hash(data, size));
}

std::string SHA256Digest::ToHex(const Bytes& digest)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string hex;
	hex.reserve(DIGEST_SIZE * 2);
	for (const u8 b : digest)
	{
		hex.push_back(digits[b >> 4]);
		hex.push_back(digits[b & 0xf]);
	}
	return hex;
}

SHA256Digest::FileResult SHA256Digest::HashFile(const char* path, std::string* hex_out, Error* error, const std::atomic_bool* cancel)
{
	FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(path, "rb", error);
	if (!fp)
		return FileResult::Failed;

	constexpr size_t CHUNK_SIZE = 1024 * 1024;
	const std::unique_ptr<u8[]> buffer = std::make_unique<u8[]>(CHUNK_SIZE);

	SHA256Digest sha;
	for (;;)
	{
		if (cancel && cancel->load(std::memory_order_acquire))
			return FileResult::Cancelled;

		const size_t n = std::fread(buffer.get(), 1, CHUNK_SIZE, fp.get());
		if (n > 0)
			sha.Update(buffer.get(), n);

		if (n < CHUNK_SIZE)
		{
			if (std::ferror(fp.get()))
			{
				Error::SetStringView(error, "Read error while hashing file.");
				return FileResult::Failed;
			}
			break;
		}
	}

	if (hex_out)
		*hex_out = sha.FinalHex();
	return FileResult::Ok;
}
