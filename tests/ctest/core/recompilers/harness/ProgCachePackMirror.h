// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The layout of a VU program cache pack (pcsx2/arm64/microVU_ProgCache-arm64.inl), mirrored so tests can
// read the records a session wrote and append records of their own (the newest record of a kind for a hash
// wins), without a backdoor in production. mvu_progcache_versioning_tests.cpp pins the layout against the
// writer.

#include "arm64/microVU_ProgCache-arm64.h"
#include "common/FileSystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace progcache_pack
{
	constexpr u8 kProgram = 1;
	constexpr u8 kSidecar = 2;
	constexpr size_t kHeaderBytes = 64;
	constexpr u32 kRecordMagic = 0x43455256u; // 'VREC'

#pragma pack(push, 1)
	struct RecordHeader
	{
		u32 magic;
		u8 kind;
		u8 reserved[3];
		u32 length;
		u32 reserved2;
		XXH128_hash_t hash;
		u64 checksum;
	};
#pragma pack(pop)
	static_assert(sizeof(RecordHeader) == 40, "RecordHeader mirror drifted");

	struct Record
	{
		u8 kind;
		XXH128_hash_t hash;
		std::vector<u8> bytes;
	};

	// Every pack under `dir`.
	inline std::vector<std::string> FindPacks(const std::string& dir)
	{
		std::vector<std::string> out;
		std::error_code ec;
		for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code sec;
			if (it->is_regular_file(sec) && it->path().extension() == ".pack")
				out.push_back(it->path().string());
		}
		return out;
	}

	// The records of a pack, in order, up to the first that does not fit.
	inline std::vector<Record> ReadRecords(const std::string& path)
	{
		std::vector<Record> out;
		const std::optional<std::vector<u8>> b = FileSystem::ReadBinaryFile(path.c_str());
		if (!b.has_value() || b->size() < kHeaderBytes)
			return out;
		size_t off = kHeaderBytes;
		while (b->size() - off >= sizeof(RecordHeader))
		{
			RecordHeader h;
			std::memcpy(&h, b->data() + off, sizeof(h));
			if (h.magic != kRecordMagic || h.length > b->size() - off - sizeof(h))
				break;
			const u8* data = b->data() + off + sizeof(h);
			out.push_back({h.kind, h.hash, std::vector<u8>(data, data + h.length)});
			off += sizeof(h) + h.length;
		}
		return out;
	}

	inline size_t CountRecords(const std::string& dir, u8 kind)
	{
		size_t n = 0;
		for (const std::string& pack : FindPacks(dir))
			for (const Record& r : ReadRecords(pack))
				n += r.kind == kind;
		return n;
	}

	// Appends a record, checksummed as the writer does it.
	inline bool AppendRecord(const std::string& path, u8 kind, const XXH128_hash_t& hash, const std::vector<u8>& bytes)
	{
		RecordHeader h = {};
		h.magic = kRecordMagic;
		h.kind = kind;
		h.length = static_cast<u32>(bytes.size());
		h.hash = hash;
		h.checksum = XXH3_64bits_withSeed(bytes.data(), bytes.size(), XXH3_64bits(&h, sizeof(h)));
		std::FILE* fp = std::fopen(path.c_str(), "ab");
		if (!fp)
			return false;
		const bool ok = std::fwrite(&h, sizeof(h), 1, fp) == 1 && (bytes.empty() || std::fwrite(bytes.data(), bytes.size(), 1, fp) == 1);
		return std::fclose(fp) == 0 && ok;
	}

	// A sidecar as a test names it: "<pack>#<hash hex>", the pack and the program it belongs to.
	inline std::string SidecarRef(const std::string& pack, const XXH128_hash_t& hash)
	{
		char hex[40];
		std::snprintf(hex, sizeof(hex), "#%016llx%016llx", static_cast<unsigned long long>(hash.high64),
			static_cast<unsigned long long>(hash.low64));
		return pack + hex;
	}

	inline bool ParseRef(const std::string& ref, std::string& pack, XXH128_hash_t& hash)
	{
		const size_t at = ref.rfind('#');
		if (at == std::string::npos || ref.size() - at != 33)
			return false;
		pack = ref.substr(0, at);
		hash.high64 = std::strtoull(ref.substr(at + 1, 16).c_str(), nullptr, 16);
		hash.low64 = std::strtoull(ref.substr(at + 17, 16).c_str(), nullptr, 16);
		return true;
	}

	// Every program with a sidecar in the packs under `dir`, one ref each.
	inline std::vector<std::string> FindSidecars(const std::string& dir)
	{
		std::vector<std::string> out;
		for (const std::string& pack : FindPacks(dir))
			for (const Record& r : ReadRecords(pack))
			{
				const std::string ref = SidecarRef(pack, r.hash);
				if (r.kind == kSidecar && std::find(out.begin(), out.end(), ref) == out.end())
					out.push_back(ref);
			}
		return out;
	}

	// The newest sidecar a ref names.
	inline std::optional<std::vector<u8>> ReadSidecar(const std::string& ref)
	{
		std::string pack;
		XXH128_hash_t hash;
		if (!ParseRef(ref, pack, hash))
			return std::nullopt;
		std::optional<std::vector<u8>> out;
		for (Record& r : ReadRecords(pack))
			if (r.kind == kSidecar && r.hash.low64 == hash.low64 && r.hash.high64 == hash.high64)
				out = std::move(r.bytes);
		return out;
	}

	// Replaces the sidecar a ref names, as the next session sees it.
	inline bool WriteSidecar(const std::string& ref, const void* data, size_t size)
	{
		std::string pack;
		XXH128_hash_t hash;
		if (!ParseRef(ref, pack, hash))
			return false;
		const u8* p = static_cast<const u8*>(data);
		return AppendRecord(pack, kSidecar, hash, std::vector<u8>(p, p + size));
	}
} // namespace progcache_pack
