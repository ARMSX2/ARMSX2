// SPDX-FileCopyrightText: 2026 ARMSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VuAotPersist.h"

#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include "xxhash.h"

#include <cstring>

namespace vuaot::persist
{
	namespace
	{
#pragma pack(push, 1)
		struct FileHeader
		{
			u32 magic;
			u32 formatVersion;
			u32 abiVersion; // kVuAotAbiVersion
			u32 mvuAbi;     // kMvuCompilerAbiVersion
			u32 keyBytes;   // kKeyBytes
			u32 tiers;
			u32 settled;
			u32 layoutHash;
			u64 contentHashLo, contentHashHi;
			u64 sentinelLo, sentinelHi;
			u64 buildLo, buildHi;
		};
		static_assert(sizeof(FileHeader) == 80, "FileHeader layout");

		struct RecordHeader
		{
			u8 kind;
			u8 flags;
			u16 reserved;
			u32 pc;
			u32 readCount;
			u8 key[kKeyBytes];
		};
		static_assert(sizeof(RecordHeader) == 12 + kKeyBytes, "RecordHeader layout");

		// The code section of a record with kHasCode: the counts, then the code bytes and each table in
		// this order.
		struct CodeHeader
		{
			u32 codeBytes;
			u32 keys, nodes, exits, pending, blockRefs, tails, ranges, ph, fixups;
			u32 regions, regionNodes, regionMerged, regionExits;
			u64 regionBytes;
		};
		static_assert(sizeof(CodeHeader) == 64, "CodeHeader layout");
#pragma pack(pop)

		template <typename T>
		void Put(std::vector<u8>& out, const T& v)
		{
			const size_t at = out.size();
			out.resize(at + sizeof(T));
			std::memcpy(out.data() + at, &v, sizeof(T));
		}

		template <typename T>
		void PutVec(std::vector<u8>& out, const std::vector<T>& v)
		{
			if (v.empty())
				return;
			const size_t at = out.size();
			out.resize(at + v.size() * sizeof(T));
			std::memcpy(out.data() + at, v.data(), v.size() * sizeof(T));
		}

		struct Reader
		{
			const u8* p;
			const u8* end;
			bool ok = true;

			template <typename T>
			T Get()
			{
				T v{};
				if (static_cast<size_t>(end - p) < sizeof(T))
				{
					ok = false;
					return v;
				}
				std::memcpy(&v, p, sizeof(T));
				p += sizeof(T);
				return v;
			}
			bool Bytes(void* dst, size_t n)
			{
				if (static_cast<size_t>(end - p) < n)
					return ok = false;
				std::memcpy(dst, p, n);
				p += n;
				return true;
			}
			size_t Left() const { return static_cast<size_t>(end - p); }
			// `n` elements, allocated only once the bytes are known to be there.
			template <typename T>
			bool Vec(std::vector<T>& v, u32 n)
			{
				if (Left() / sizeof(T) < n)
					return ok = false;
				v.resize(n);
				return n == 0 || Bytes(v.data(), size_t(n) * sizeof(T));
			}
		};

		void PutCode(std::vector<u8>& out, const RegionCode& c)
		{
			CodeHeader ch = {};
			ch.codeBytes = static_cast<u32>(c.code.size());
			ch.keys = static_cast<u32>(c.keys.size());
			ch.nodes = static_cast<u32>(c.nodes.size());
			ch.exits = static_cast<u32>(c.exits.size());
			ch.pending = static_cast<u32>(c.pending.size());
			ch.blockRefs = static_cast<u32>(c.blockRefs.size());
			ch.tails = static_cast<u32>(c.tails.size());
			ch.ranges = static_cast<u32>(c.ranges.size());
			ch.ph = static_cast<u32>(c.ph.size());
			ch.fixups = static_cast<u32>(c.fixups.size());
			ch.regions = c.regions;
			ch.regionNodes = c.regionNodes;
			ch.regionMerged = c.regionMerged;
			ch.regionExits = c.regionExits;
			ch.regionBytes = c.regionBytes;
			Put(out, ch);
			PutVec(out, c.code);
			PutVec(out, c.keys);
			PutVec(out, c.nodes);
			PutVec(out, c.exits);
			PutVec(out, c.pending);
			PutVec(out, c.blockRefs);
			PutVec(out, c.tails);
			PutVec(out, c.ranges);
			PutVec(out, c.ph);
			PutVec(out, c.fixups);
		}

		// The shape of a code section: everything a fixup or a table names is inside the code or its
		// table, and each fixup's form fits its target.
		bool CodeShapeOk(const RegionCode& c)
		{
			const size_t size = c.code.size();
			if (size == 0 || size > kMaxCodeBytes || (size & 3) || c.nodes.empty())
				return false;
			const auto keyOk = [&](u32 k) { return k < c.keys.size(); };
			for (const CodeNode& n : c.nodes)
				if (!keyOk(n.key) || n.pc >= 0x4000 || (n.pc & 7) || n.entryOff >= size || (n.entryOff & 3))
					return false;
			for (const auto* v : {&c.exits, &c.pending, &c.blockRefs})
				for (const BlockAt& b : *v)
					if (!keyOk(b.key) || b.pc >= 0x4000 || (b.pc & 7))
						return false;
			for (const CodeTail& t : c.tails)
				if (!keyOk(t.key) || t.node >= c.nodes.size())
					return false;
			for (const CodeRange& r : c.ranges)
				if (r.start >= 0x4000 || r.last >= 0x4000 || (r.start & 7) || (r.last & 7))
					return false;
			for (const Placeholder& ph : c.ph)
			{
				const size_t limit = ph.kind == kPhStub ? kMaxStubs : ph.kind == kPhExit ? c.exits.size() :
									 ph.kind == kPhTail ? c.tails.size() : ph.kind == kPhPending ? c.pending.size() : 0;
				if (ph.index >= limit)
					return false;
			}
			for (const Fixup& f : c.fixups)
			{
				if (f.form >= kFormCount || f.target >= kTargetCount)
					return false;
				if (f.form == kFormMovRun && (f.width < 1 || f.width > 4))
					return false;
				const size_t span = (f.form == kFormData64) ? 8 : size_t(RegionCode::Span(f)) * 4;
				if (f.offset > size || size - f.offset < span || (f.form != kFormData64 && (f.offset & 3)))
					return false;
				switch (f.target)
				{
					case kTargetIntra:
						// (an address inside the code, as a full run: the worker emits nothing else for one)
						if (f.form != kFormMovRun || f.width != 4 || f.value < 0 || static_cast<u64>(f.value) >= size)
							return false;
						break;
					case kTargetPlaceholder:
						if (f.form == kFormData64 || f.index >= c.ph.size() || (f.form == kFormMovRun && f.width != 4))
							return false;
						break;
					case kTargetImage:
					case kTargetData:
						if (f.form == kFormData64 || f.value < 0)
							return false;
						break;
					case kTargetBlockRef:
						if (f.form != kFormMovRun || f.index >= c.blockRefs.size())
							return false;
						break;
					case kTargetProgram:
						if (f.form != kFormData64)
							return false;
						break;
					case kTargetNodeHandle:
						if (f.form != kFormData64 || f.index >= c.nodes.size())
							return false;
						break;
				}
			}
			return true;
		}

		bool GetCode(Reader& r, RegionCode& c)
		{
			CodeHeader ch;
			if (!r.Bytes(&ch, sizeof(ch)) || ch.codeBytes > kMaxCodeBytes)
				return false;
			c.regions = ch.regions;
			c.regionNodes = ch.regionNodes;
			c.regionMerged = ch.regionMerged;
			c.regionExits = ch.regionExits;
			c.regionBytes = ch.regionBytes;
			return r.Vec(c.code, ch.codeBytes) && r.Vec(c.keys, ch.keys) && r.Vec(c.nodes, ch.nodes) && r.Vec(c.exits, ch.exits) &&
				   r.Vec(c.pending, ch.pending) && r.Vec(c.blockRefs, ch.blockRefs) && r.Vec(c.tails, ch.tails) &&
				   r.Vec(c.ranges, ch.ranges) && r.Vec(c.ph, ch.ph) && r.Vec(c.fixups, ch.fixups) && CodeShapeOk(c);
		}

		u32 PopCount(const std::vector<u8>& bits)
		{
			u32 n = 0;
			for (u8 b : bits)
				n += static_cast<u32>(__builtin_popcount(b));
			return n;
		}
	} // namespace

	bool Record::MatchesMicro(const u8* micro) const
	{
		if (readBits.size() != kReadBitmapBytes)
			return false;
		const u32* live = reinterpret_cast<const u32*>(micro);
		size_t v = 0;
		for (u32 w = 0; w < kMicroWords; w++)
		{
			if (!((readBits[w >> 3] >> (w & 7)) & 1))
				continue;
			if (v >= readValues.size() || live[w] != readValues[v])
				return false;
			v++;
		}
		return v == readValues.size();
	}

	u32 RegionCode::KeyIndex(const u8* k)
	{
		for (size_t i = 0; i < keys.size(); i++)
			if (std::memcmp(keys[i].data(), k, kKeyBytes) == 0)
				return static_cast<u32>(i);
		Key key;
		std::memcpy(key.data(), k, kKeyBytes);
		keys.push_back(key);
		return static_cast<u32>(keys.size() - 1);
	}

	void Record::SetReadSet(const u8* perWordMask, const u8* micro)
	{
		readBits.assign(kReadBitmapBytes, 0);
		readValues.clear();
		const u32* words = reinterpret_cast<const u32*>(micro);
		for (u32 w = 0; w < kMicroWords; w++)
			if (perWordMask[w])
			{
				readBits[w >> 3] |= static_cast<u8>(1u << (w & 7));
				readValues.push_back(words[w]);
			}
	}

	std::vector<u8> Serialize(const File& f)
	{
		std::vector<u8> out;
		FileHeader h = {};
		h.magic = kMagic;
		h.formatVersion = kFormatVersion;
		h.abiVersion = kVuAotAbiVersion;
		h.mvuAbi = f.id.mvuAbi;
		h.keyBytes = kKeyBytes;
		h.tiers = f.tiers;
		h.settled = f.settled ? 1 : 0;
		h.layoutHash = f.id.layoutHash;
		h.contentHashLo = f.id.contentHashLo;
		h.contentHashHi = f.id.contentHashHi;
		h.sentinelLo = f.id.sentinelLo;
		h.sentinelHi = f.id.sentinelHi;
		h.buildLo = f.id.buildLo;
		h.buildHi = f.id.buildHi;
		Put(out, h);

		Put(out, static_cast<u32>(f.jumpTargets.size()));
		for (const auto& jt : f.jumpTargets)
		{
			Put(out, jt.first);
			Put(out, static_cast<u32>(jt.second.size()));
			for (u32 t : jt.second)
				Put(out, t);
		}

		Put(out, static_cast<u32>(f.records.size()));
		for (const Record& r : f.records)
		{
			RecordHeader rh = {};
			rh.kind = r.kind;
			rh.flags = r.flags;
			rh.pc = r.pc;
			rh.readCount = static_cast<u32>(r.readValues.size());
			std::memcpy(rh.key, r.key, kKeyBytes);
			Put(out, rh);
			const size_t at = out.size();
			out.resize(at + kReadBitmapBytes, 0);
			if (r.readBits.size() == kReadBitmapBytes)
				std::memcpy(out.data() + at, r.readBits.data(), kReadBitmapBytes);
			for (u32 v : r.readValues)
				Put(out, v);
			if (r.HasCode())
				PutCode(out, r.code);
		}

		Put(out, static_cast<u32>(f.declinedRoots.size()));
		for (const DeclinedRoot& d : f.declinedRoots)
		{
			Put(out, d.pc);
			PutVec(out, std::vector<u8>(d.key.begin(), d.key.end()));
		}

		Put(out, static_cast<u64>(XXH3_64bits(out.data(), out.size())));
		return out;
	}

	const char* ParseResultName(ParseResult r)
	{
		switch (r)
		{
			case ParseResult::Ok: return "ok";
			case ParseResult::Truncated: return "truncated";
			case ParseResult::BadMagic: return "bad magic";
			case ParseResult::BadFormatVersion: return "format version";
			case ParseResult::BadAbiVersion: return "vu-aot ABI version";
			case ParseResult::BadMvuAbi: return "microVU ABI version";
			case ParseResult::BadKeySize: return "key size";
			case ParseResult::OtherBuild: return "another build";
			case ParseResult::OtherLayout: return "another struct layout";
			case ParseResult::OtherSentinel: return "another options sentinel";
			case ParseResult::OtherProgram: return "another program";
			case ParseResult::BadChecksum: return "checksum";
			case ParseResult::Malformed: return "malformed";
			default: return "?";
		}
	}

	ParseResult Parse(const u8* data, size_t size, const Identity& expect, File& out)
	{
		if (!data || size < sizeof(FileHeader) + sizeof(u64))
			return ParseResult::Truncated;
		FileHeader h;
		std::memcpy(&h, data, sizeof(h));
		if (h.magic != kMagic)
			return ParseResult::BadMagic;
		if (h.formatVersion != kFormatVersion)
			return ParseResult::BadFormatVersion;
		// The checksum before any other field: a damaged file is "damaged", whatever field the damage hit.
		const size_t body = size - sizeof(u64);
		u64 sum;
		std::memcpy(&sum, data + body, sizeof(sum));
		if (sum != XXH3_64bits(data, body))
			return ParseResult::BadChecksum;
		if (h.abiVersion != kVuAotAbiVersion)
			return ParseResult::BadAbiVersion;
		if (h.mvuAbi != expect.mvuAbi)
			return ParseResult::BadMvuAbi;
		if (h.keyBytes != kKeyBytes)
			return ParseResult::BadKeySize;
		if (h.buildLo != expect.buildLo || h.buildHi != expect.buildHi)
			return ParseResult::OtherBuild;
		if (h.layoutHash != expect.layoutHash)
			return ParseResult::OtherLayout;
		if (h.sentinelLo != expect.sentinelLo || h.sentinelHi != expect.sentinelHi)
			return ParseResult::OtherSentinel;
		if (h.contentHashLo != expect.contentHashLo || h.contentHashHi != expect.contentHashHi)
			return ParseResult::OtherProgram;

		out = File{};
		out.id = expect;
		out.tiers = h.tiers;
		out.settled = h.settled != 0;

		Reader r{data + sizeof(h), data + body};
		const u32 jumps = r.Get<u32>();
		if (!r.ok || jumps > kMaxJumpBlocks)
			return ParseResult::Malformed;
		for (u32 i = 0; i < jumps; i++)
		{
			const u32 pc = r.Get<u32>();
			const u32 n = r.Get<u32>();
			if (!r.ok || n > kMaxTargetsPerBlock || r.Left() < size_t(n) * sizeof(u32) || pc >= 0x4000)
				return ParseResult::Malformed;
			std::vector<u32>& v = out.jumpTargets[pc];
			v.resize(n);
			r.Bytes(v.data(), size_t(n) * sizeof(u32));
			for (u32 t : v)
				if (t >= 0x4000 || (t & 7))
					return ParseResult::Malformed;
		}

		const u32 records = r.Get<u32>();
		if (!r.ok || records > kMaxRecords)
			return ParseResult::Malformed;
		out.records.resize(records);
		for (Record& rec : out.records)
		{
			RecordHeader rh;
			if (!r.Bytes(&rh, sizeof(rh)))
				return ParseResult::Malformed;
			if (rh.kind > kRoot || rh.pc >= 0x4000 || (rh.pc & 7) || rh.readCount > kMicroWords)
				return ParseResult::Malformed;
			rec.kind = rh.kind;
			rec.flags = rh.flags;
			rec.pc = rh.pc;
			std::memcpy(rec.key, rh.key, kKeyBytes);
			rec.readBits.resize(kReadBitmapBytes);
			if (!r.Bytes(rec.readBits.data(), kReadBitmapBytes))
				return ParseResult::Malformed;
			if (PopCount(rec.readBits) != rh.readCount || r.Left() < size_t(rh.readCount) * sizeof(u32))
				return ParseResult::Malformed;
			rec.readValues.resize(rh.readCount);
			r.Bytes(rec.readValues.data(), size_t(rh.readCount) * sizeof(u32));
			if (rec.HasCode() && !GetCode(r, rec.code))
				return ParseResult::Malformed;
		}
		const u32 declined = r.Get<u32>();
		if (!r.ok || declined > kMaxDeclinedRoots || r.Left() / (4 + kKeyBytes) < declined)
			return ParseResult::Malformed;
		out.declinedRoots.resize(declined);
		for (DeclinedRoot& d : out.declinedRoots)
		{
			d.pc = r.Get<u32>();
			r.Bytes(d.key.data(), kKeyBytes);
			if (d.pc >= 0x4000 || (d.pc & 7))
				return ParseResult::Malformed;
		}
		if (!r.ok || r.Left() != 0)
			return ParseResult::Malformed;
		return ParseResult::Ok;
	}
} // namespace vuaot::persist
