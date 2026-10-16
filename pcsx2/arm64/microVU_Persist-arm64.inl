// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Implementation of microVU_Persist-arm64.h. Included at the bottom of
// microVU-arm64.cpp (same pattern as microVU_ProgCache-arm64.inl) so it sees
// the full microVU/microProgram/microBlock types plus the TU-local helpers
// (mVUopenCodeCache / mVUcloseCodeCache / mVUcreateProg / mVUcomputeProgramHash).

#include "arm64/microVU_Persist-arm64.h"
#include "Memory.h"

#ifdef __linux__
#include <link.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mVUPersist
{
	// A fixup is an encoding form (how the operand sits in the code) and a
	// target (what it points at). Every target is resolved against this
	// process at hydration, so nothing in a payload depends on the addresses
	// of the process that recorded it.
	enum : u8
	{
		kFormMovRun = 0, // MOVZ + MOVK×(width-1) over halfwords 0..width-1, one Rd
		kFormAdrp,       // ADRP page21; the page offset rides in the next ADD/ORR
		kFormRel26,      // B / BL imm26
		kFormRel19,      // B.cond / CBZ / CBNZ imm19
		kFormCount,
	};

	enum : u8
	{
		kTargetBlockObject = 0, // log.blocks[index]'s microBlock + offset (heap)
		kTargetBlockEntry,      // log.blocks[index]'s code entry
		kTargetStub,            // per-VU dispatcher stub, id = index
		kTargetImage,           // image base + offset (code, data, bss of the emulator module)
		kTargetData,            // data arena base + offset
		kTargetCount,
	};

	enum : u8
	{
		kStubStartFunct = 0,
		kStubExitFunct,
		kStubStartFunctXG,
		kStubExitFunctXG,
		kStubWaitMTVU,
		kStubCopyPLState,
		kStubEndFlagsA,
		kStubEndFlagsB,
		kStubResumeXG,
		kStubCycleBreak,
		// mVUModelStubCount ids follow, one per model stub.
		kStubModelBase,
		// Past the model stubs, so adding one here does not renumber the ids
		// already recorded against them.
		kStubExitFunctEBit = kStubModelBase + mVUModelStubCount,
		kStubCount,
	};

	struct PersistFixup
	{
		u32 codeOffset; // chunk-relative offset of the first patched insn
		u8 form;
		u8 target;
		u8 width; // kFormMovRun: instruction count (1..4)
		u8 _pad0;
		u32 index; // block index (block targets) or stub id
		u32 _pad1;
		s64 offset; // byte offset from the target's base (block object, image, data arena)
	};
	static_assert(sizeof(PersistFixup) == 24);

	// Instructions a fixup's patcher rewrites, starting at codeOffset.
	static u32 FixupSpanInsns(const PersistFixup& f)
	{
		return (f.form == kFormMovRun) ? f.width : 1;
	}

	struct PersistChunk
	{
		std::vector<u8> code;
		std::vector<PersistFixup> fixups;
		const u8* liveBase = nullptr; // where this chunk runs in this process; not serialized
	};

	struct PersistBlockRec
	{
		u32 chunkIndex;
		u32 entryOffset; // within chunk
		u32 startPC; // microMem byte offset
		u32 hasJumpCache;
		microRegInfo pState;
		microRegInfo pStateEnd;
		microBlock* live; // manager's copy; not serialized
	};
} // namespace mVUPersist

// Global-scope definition (fwd-declared in the header so microProgram can
// hold a pointer without pulling the implementation types).
struct MvuPersistLog
{
	std::vector<mVUPersist::PersistChunk> chunks;
	std::vector<mVUPersist::PersistBlockRec> blocks;
	// hostEntry → blocks[] index, for cross-chunk branch resolution. Keyed on
	// the manager's hostEntry (what armEmitJmp call sites actually target).
	std::unordered_map<const void*, u32> blockByEntry;
};

namespace mVUPersist
{
	static bool s_recordingEnabled = false;
	// When true, mVUinit/mVUreset's SyncRecordingFromConfig is a no-op so the
	// unit-test harness keeps manual control of recording (ABI-digest pins,
	// in-process round-trips, disk round-trips). Default false = production,
	// where recording follows the program cache setting at every reset.
	static bool s_manualRecording = false;
	static u64 s_blockCompiles[2] = {};
	static Stats s_stats[2] = {};

	// Process-wide determinism kill switch for the offline tools (pcsx2-vurunner
	// --no-progcache). When set, both the on-disk program cache and emit-time
	// recording are forced off for the whole process, so a JIT-vs-interp diff run
	// is byte-reproducible. Replaces the former PCSX2_VU_PROGCACHE_DISABLE env var
	// — no env gate in production code. Default false = no effect.
	static bool s_processDisabled = false;

	//------------------------------------------------------------------
	// Address classification
	//------------------------------------------------------------------

	// The two address regions a block may embed besides its own program's
	// blocks and the dispatcher stubs: the emulator's module image (its code,
	// data and bss; for Android that is the shared library, not the app
	// process) and the data arena. Each region moves as a unit under ASLR, so
	// an address in it is recorded as an offset from the region's base and
	// re-based at hydration. The bases are page-aligned, which keeps the low 12
	// bits an ADRP + ADD pair splits off unchanged by any move.
	struct Regions
	{
		uptr imgBegin = 0, imgEnd = 0;
		uptr dataBegin = 0, dataEnd = 0;
		uptr imageBase = 0; // imgBegin rounded down to 4KB
		uptr dataBase = 0;
		bool valid = false;
	};

#if defined(__linux__)
	struct ModuleSearch
	{
		uptr probe;
		uptr begin, end;
		bool found;
	};

	static int FindModuleCallback(dl_phdr_info* info, size_t, void* data)
	{
		ModuleSearch* ms = static_cast<ModuleSearch*>(data);
		uptr lo = ~static_cast<uptr>(0), hi = 0;
		bool contains = false;
		for (int i = 0; i < info->dlpi_phnum; i++)
		{
			const auto& ph = info->dlpi_phdr[i];
			if (ph.p_type != PT_LOAD || ph.p_memsz == 0)
				continue;
			const uptr b = static_cast<uptr>(info->dlpi_addr + ph.p_vaddr);
			const uptr e = b + static_cast<uptr>(ph.p_memsz);
			lo = std::min(lo, b);
			hi = std::max(hi, e);
			if (ms->probe >= b && ms->probe < e)
				contains = true;
		}
		if (!contains)
			return 0;
		ms->begin = lo;
		ms->end = hi;
		ms->found = true;
		return 1;
	}
#endif

	// The extent of the loaded module that holds `probe`: the union of its
	// PT_LOAD segments (bss included, as p_memsz) on Linux and Android, of its
	// mapped segments on Apple platforms.
	static bool ResolveModuleRange(const void* probe, uptr& out_begin, uptr& out_end)
	{
#if defined(__linux__)
		ModuleSearch ms = {reinterpret_cast<uptr>(probe), 0, 0, false};
		dl_iterate_phdr(FindModuleCallback, &ms);
		if (!ms.found || ms.end <= ms.begin)
			return false;
		out_begin = ms.begin;
		out_end = ms.end;
		return true;
#elif defined(__APPLE__)
		const uptr p = reinterpret_cast<uptr>(probe);
		const u32 count = _dyld_image_count();
		for (u32 img = 0; img < count; img++)
		{
			const mach_header_64* hdr =
				reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(img));
			if (!hdr || hdr->magic != MH_MAGIC_64)
				continue;
			const intptr_t slide = _dyld_get_image_vmaddr_slide(img);
			uptr begin = ~static_cast<uptr>(0), end = 0;
			const load_command* lc = reinterpret_cast<const load_command*>(hdr + 1);
			for (u32 i = 0; i < hdr->ncmds; i++)
			{
				if (lc->cmd == LC_SEGMENT_64)
				{
					const segment_command_64* seg = reinterpret_cast<const segment_command_64*>(lc);
					// __PAGEZERO is the unmapped 4 GB null-deref guard at vmaddr 0;
					// absorbing it would classify most of the address space as
					// image.
					if (seg->vmsize && std::strcmp(seg->segname, SEG_PAGEZERO) != 0)
					{
						begin = std::min(begin, static_cast<uptr>(seg->vmaddr) + slide);
						end = std::max(end, static_cast<uptr>(seg->vmaddr + seg->vmsize) + slide);
					}
				}
				lc = reinterpret_cast<const load_command*>(
					reinterpret_cast<const u8*>(lc) + lc->cmdsize);
			}
			if (end > begin && p >= begin && p < end)
			{
				out_begin = begin;
				out_end = end;
				return true;
			}
		}
		return false;
#else
		return false;
#endif
	}

	static const Regions& GetRegions()
	{
		static Regions r = []() {
			Regions out;
			// microVU0 lives in the module that holds the JIT and every C
			// helper and global it calls or addresses.
			if (!ResolveModuleRange(&microVU0, out.imgBegin, out.imgEnd))
			{
				DevCon.Warning("mVUPersist: could not resolve the emulator module's range — recording will mark all programs non-persistable");
				return out;
			}
			out.imageBase = out.imgBegin & ~static_cast<uptr>(0xFFF);
			out.dataBegin = reinterpret_cast<uptr>(SysMemory::GetDataPtr(0));
			out.dataEnd = out.dataBegin + HostMemoryMap::MainSize;
			out.dataBase = out.dataBegin & ~static_cast<uptr>(0xFFF);
			out.valid = true;
			DevCon.WriteLn("mVUPersist: emulator module %p-%p, data arena %p",
				reinterpret_cast<void*>(out.imgBegin), reinterpret_cast<void*>(out.imgEnd),
				reinterpret_cast<void*>(out.dataBegin));
			return out;
		}();
		return r;
	}

	// Which region an address lies in: kTargetImage, kTargetData, or
	// kTargetCount for neither.
	static u8 RegionOf(const void* addr)
	{
		const Regions& r = GetRegions();
		if (!r.valid)
			return kTargetCount;
		const uptr a = reinterpret_cast<uptr>(addr);
		if (a >= r.imgBegin && a < r.imgEnd)
			return kTargetImage;
		if (a >= r.dataBegin && a < r.dataEnd)
			return kTargetData;
		return kTargetCount;
	}

	static uptr RegionBase(u8 target)
	{
		const Regions& r = GetRegions();
		return (target == kTargetImage) ? r.imageBase : r.dataBase;
	}

	//------------------------------------------------------------------
	// Build identity
	//------------------------------------------------------------------

#if defined(__linux__)
	struct BuildIdSearch
	{
		uptr probe;
		std::vector<u8> id;
	};

	// The NT_GNU_BUILD_ID note of the module that holds `probe`, from its PT_NOTE segments.
	static int FindBuildIdCallback(dl_phdr_info* info, size_t, void* data)
	{
		BuildIdSearch* bs = static_cast<BuildIdSearch*>(data);
		bool contains = false;
		for (int i = 0; i < info->dlpi_phnum && !contains; i++)
		{
			const auto& ph = info->dlpi_phdr[i];
			const uptr b = static_cast<uptr>(info->dlpi_addr + ph.p_vaddr);
			contains = ph.p_type == PT_LOAD && bs->probe >= b && bs->probe < b + ph.p_memsz;
		}
		if (!contains)
			return 0;
		for (int i = 0; i < info->dlpi_phnum; i++)
		{
			const auto& ph = info->dlpi_phdr[i];
			if (ph.p_type != PT_NOTE)
				continue;
			const u8* p = reinterpret_cast<const u8*>(info->dlpi_addr + ph.p_vaddr);
			const u8* const end = p + ph.p_memsz;
			while (p + 12 <= end)
			{
				u32 namesz, descsz, type;
				std::memcpy(&namesz, p, 4);
				std::memcpy(&descsz, p + 4, 4);
				std::memcpy(&type, p + 8, 4);
				const u8* name = p + 12;
				const u8* desc = name + ((namesz + 3) & ~3u);
				const u8* next = desc + ((descsz + 3) & ~3u);
				if (next > end)
					break;
				if (type == 3 /* NT_GNU_BUILD_ID */ && namesz == 4 && std::memcmp(name, "GNU", 4) == 0 && descsz)
				{
					bs->id.assign(desc, desc + descsz);
					return 1;
				}
				p = next;
			}
		}
		return 1;
	}
#endif

	static std::vector<u8> ReadBuildIdBytes()
	{
#if defined(__linux__)
		BuildIdSearch bs{reinterpret_cast<uptr>(&microVU0), {}};
		dl_iterate_phdr(FindBuildIdCallback, &bs);
		if (!bs.id.empty())
			return bs.id;
#elif defined(__APPLE__)
		const uptr p = reinterpret_cast<uptr>(&microVU0);
		const u32 count = _dyld_image_count();
		for (u32 img = 0; img < count; img++)
		{
			const mach_header_64* hdr = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(img));
			if (!hdr || hdr->magic != MH_MAGIC_64)
				continue;
			uptr begin = 0, end = 0;
			if (!ResolveModuleRange(hdr, begin, end) || p < begin || p >= end)
				continue;
			const load_command* lc = reinterpret_cast<const load_command*>(hdr + 1);
			for (u32 i = 0; i < hdr->ncmds; i++)
			{
				if (lc->cmd == LC_UUID)
				{
					const uuid_command* u = reinterpret_cast<const uuid_command*>(lc);
					return std::vector<u8>(u->uuid, u->uuid + sizeof(u->uuid));
				}
				lc = reinterpret_cast<const load_command*>(reinterpret_cast<const u8*>(lc) + lc->cmdsize);
			}
		}
#endif
		static const char kWhen[] = __DATE__ " " __TIME__;
		return std::vector<u8>(kWhen, kWhen + sizeof(kWhen));
	}

#ifdef PCSX2_RECOMPILER_TESTS
	static bool s_buildIdOverride = false;
	static u64 s_buildIdOverrideLo = 0, s_buildIdOverrideHi = 0;
	void TestOverrideBuildId(bool on, u64 lo, u64 hi)
	{
		s_buildIdOverride = on;
		s_buildIdOverrideLo = lo;
		s_buildIdOverrideHi = hi;
	}
#endif

	void GetBuildId(u64& lo, u64& hi)
	{
#ifdef PCSX2_RECOMPILER_TESTS
		if (s_buildIdOverride)
		{
			lo = s_buildIdOverrideLo;
			hi = s_buildIdOverrideHi;
			return;
		}
#endif
		static const XXH128_hash_t id = []() {
			const std::vector<u8> bytes = ReadBuildIdBytes();
			return XXH3_128bits(bytes.data(), bytes.size());
		}();
		lo = id.low64;
		hi = id.high64;
	}

	// The build's identity folded to the 64 bits a payload header carries.
	static u64 BuildTag()
	{
		u64 lo, hi;
		GetBuildId(lo, hi);
		return lo ^ hi;
	}

	//------------------------------------------------------------------
	// Dispatcher stub table
	//------------------------------------------------------------------

	static const void* StubAddress(microVU& mVU, u8 id)
	{
		switch (id)
		{
			case kStubStartFunct:   return mVU.startFunct;
			case kStubExitFunct:    return mVU.exitFunct;
			case kStubStartFunctXG: return mVU.startFunctXG;
			case kStubExitFunctXG:  return mVU.exitFunctXG;
			case kStubWaitMTVU:     return mVU.waitMTVU;
			case kStubCopyPLState:  return mVU.copyPLState;
			case kStubEndFlagsA:    return mVU.endProgramFlagsA;
			case kStubEndFlagsB:    return mVU.endProgramFlagsB;
			case kStubResumeXG:     return mVU.resumePtrXG;
			case kStubCycleBreak:   return mVU.cycleBreak;
			case kStubExitFunctEBit: return mVU.exitFunctEBit;
			default:
				if (id >= kStubModelBase && id < kStubExitFunctEBit)
					return mVU.modelStubs[id - kStubModelBase];
				return nullptr;
		}
	}

	static bool ResolveStub(microVU& mVU, const void* target, u8& out_id)
	{
		for (u8 i = 0; i < kStubCount; i++)
		{
			if (StubAddress(mVU, i) == target && target)
			{
				out_id = i;
				return true;
			}
		}
		return false;
	}

#ifdef PCSX2_RECOMPILER_TESTS
	//------------------------------------------------------------------
	// Address-operand audit (test tooling)
	//------------------------------------------------------------------
	//
	// Decodes a recorded chunk as it sits in the slab and reports every
	// instruction that carries a host address no fixup covers: a PC-relative
	// operand (B/BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, ADR/ADRP, LDR literal) whose
	// target is outside the chunk, or a MOVZ/MOVN/ORR-immediate + MOVK run
	// whose value lands in a mapped region. This is independent of the emit
	// helpers, so it also catches code that bypasses them. Counts go into a
	// census keyed by "<form>-><region>".

	static bool s_auditEnabled = false;
	static std::mutex s_auditMutex;
	struct AuditCensus
	{
		std::map<std::string, u64> uncovered;
		std::map<std::string, u64> covered;
		std::vector<std::string> examples;
		u64 chunks = 0;
		u64 insns = 0;
		u64 data32 = 0; // W-form immediate runs whose value happens to be a mapped address
	};
	static AuditCensus* s_audit = nullptr;

	struct MapRange
	{
		uptr begin, end;
	};
	static std::vector<MapRange> s_auditMaps;

	static void AuditRefreshMaps()
	{
		s_auditMaps.clear();
#ifdef __linux__
		std::FILE* f = std::fopen("/proc/self/maps", "r");
		if (!f)
			return;
		char line[1024];
		while (std::fgets(line, sizeof(line), f))
		{
			unsigned long long b = 0, e = 0;
			if (std::sscanf(line, "%llx-%llx", &b, &e) == 2)
				s_auditMaps.push_back({static_cast<uptr>(b), static_cast<uptr>(e)});
		}
		std::fclose(f);
#endif
	}

	static bool AuditIsMapped(uptr v)
	{
		for (int pass = 0; pass < 2; pass++)
		{
			for (const MapRange& m : s_auditMaps)
				if (v >= m.begin && v < m.end)
					return true;
			if (pass == 0)
				AuditRefreshMaps(); // the heap may have grown since the last parse
		}
		return false;
	}

	static const char* AuditRegion(microVU& mVU, const MvuPersistLog& log, uptr v)
	{
		const Regions& r = GetRegions();
		if (r.valid && v >= r.imgBegin && v < r.imgEnd)
			return "image";
		if (r.valid && v >= r.dataBegin && v < r.dataEnd)
			return "data";
		const uptr slab_begin = reinterpret_cast<uptr>(mVU.cache);
		const uptr slab_end = reinterpret_cast<uptr>(mVU.prog.x86end) + mVUcacheSafeZone * _1mb;
		if (v >= slab_begin && v < slab_end)
		{
			u8 stub = 0;
			if (ResolveStub(mVU, reinterpret_cast<const void*>(v), stub))
				return "stub";
			if (log.blockByEntry.count(reinterpret_cast<const void*>(v)))
				return "block-entry";
			return "slab";
		}
		const uptr code_begin = reinterpret_cast<uptr>(SysMemory::GetCodePtr(0));
		if (v >= code_begin && v < code_begin + HostMemoryMap::CodeSize)
			return "code-arena";
		for (const PersistBlockRec& b : log.blocks)
		{
			const uptr blk = reinterpret_cast<uptr>(b.live);
			if (v >= blk && v < blk + sizeof(microBlock))
				return "block-object";
		}
		if (AuditIsMapped(v))
			return "mapped-other";
		return nullptr;
	}

	static void AuditChunk(microVU& mVU, const MvuPersistLog& log, const PersistChunk& chunk, AuditCensus& out)
	{
		namespace a64 = vixl::aarch64;
		const u8* base = chunk.liveBase;
		const u32 n = static_cast<u32>(chunk.code.size() / 4);
		std::vector<u8> covered(n, 0);
		for (const PersistFixup& f : chunk.fixups)
		{
			const u32 first = f.codeOffset / 4;
			for (u32 i = 0; i < FixupSpanInsns(f) && first + i < n; i++)
				covered[first + i] = 1;
		}
		out.chunks++;
		out.insns += n;

		auto note = [&](u32 idx, const char* form, uptr value) {
			const char* region = AuditRegion(mVU, log, value);
			if (!region)
				return;
			const std::string key = std::string(form) + "->" + region;
			if (covered[idx])
			{
				out.covered[key]++;
				return;
			}
			out.uncovered[key]++;
			if (out.examples.size() < 64)
			{
				char buf[160];
				std::snprintf(buf, sizeof(buf), "vu%u %s at chunk+0x%x insn %08x value %p",
					mVU.index, key.c_str(), idx * 4,
					*reinterpret_cast<const u32*>(base + idx * 4), reinterpret_cast<void*>(value));
				out.examples.emplace_back(buf);
			}
		};

		const uptr chunk_begin = reinterpret_cast<uptr>(base);
		const uptr chunk_end = chunk_begin + chunk.code.size();
		for (u32 i = 0; i < n; i++)
		{
			const a64::Instruction* insn = reinterpret_cast<const a64::Instruction*>(base + i * 4);
			const u32 bits = insn->GetInstructionBits();

			const char* pcrel_form = nullptr;
			if (insn->IsUncondBranchImm())
				pcrel_form = (bits & 0x80000000u) ? "bl" : "b";
			else if (insn->IsCondBranchImm())
				pcrel_form = "b.cond";
			else if (insn->IsCompareBranch())
				pcrel_form = "cbz";
			else if (insn->IsTestBranch())
				pcrel_form = "tbz";
			else if (insn->IsLoadLiteral())
				pcrel_form = "ldr-literal";
			else if (insn->IsPCRelAddressing())
				pcrel_form = (bits & 0x80000000u) ? "adrp" : "adr";
			if (pcrel_form)
			{
				uptr target = reinterpret_cast<uptr>(insn->GetImmPCOffsetTarget());
				if ((bits & 0x9F000000u) == 0x90000000u && i + 1 < n)
				{
					// ADRP + ADD/ORR: classify the full address, not just the page.
					const u32 next = *reinterpret_cast<const u32*>(base + (i + 1) * 4);
					if ((next & 0xFF800000u) == 0x91000000u && ((next >> 5) & 31) == (bits & 31))
						target += ((next >> 10) & 0xFFFu) << (((next >> 22) & 1) ? 12 : 0);
				}
				const bool in_chunk = target >= chunk_begin && target < chunk_end;
				if (!in_chunk)
					note(i, pcrel_form, target);
				continue;
			}

			// Immediate-move runs: MOVZ/MOVN or ORR Rd, ZR, #imm, then MOVKs to
			// the same Rd.
			const bool sf = (bits & 0x80000000u) != 0;
			const u32 rd = bits & 31;
			u64 value = 0;
			bool start = false;
			if ((bits & 0x7F800000u) == 0x52800000u) // MOVZ
			{
				value = static_cast<u64>((bits >> 5) & 0xFFFFu) << (16 * ((bits >> 21) & 3));
				start = true;
			}
			else if ((bits & 0x7F800000u) == 0x12800000u) // MOVN
			{
				value = ~(static_cast<u64>((bits >> 5) & 0xFFFFu) << (16 * ((bits >> 21) & 3)));
				start = true;
			}
			else if ((bits & 0x7F800000u) == 0x32000000u && ((bits >> 5) & 31) == 31) // ORR imm, ZR
			{
				value = insn->GetImmLogical();
				start = true;
			}
			if (!start)
				continue;
			u32 j = i + 1;
			for (; j < n; j++)
			{
				const u32 k = *reinterpret_cast<const u32*>(base + j * 4);
				if ((k & 0x7F800000u) != 0x72800000u || (k & 31) != rd || ((k & 0x80000000u) != 0) != sf)
					break;
				const u32 sh = 16 * ((k >> 21) & 3);
				value = (value & ~(0xFFFFull << sh)) | (static_cast<u64>((k >> 5) & 0xFFFFu) << sh);
			}
			// A 32-bit run cannot carry a pointer above 4GB, and below it a
			// non-PIE image range collides with float/data constants, so W-form
			// runs are counted apart and never fail an audit. Small values are
			// data (cycle counts, offsets, masks), not pointers.
			if (!sf)
			{
				value &= 0xFFFFFFFFull;
				if (value >= 0x10000 && AuditRegion(mVU, log, static_cast<uptr>(value)))
					out.data32++;
			}
			else if (value >= 0x10000)
				note(i, "movseq", static_cast<uptr>(value));
			i = j - 1;
		}
	}

	static void AuditChunkIfEnabled(microVU& mVU, const MvuPersistLog& log, const PersistChunk& chunk)
	{
		if (!s_auditEnabled)
			return;
		std::lock_guard<std::mutex> lock(s_auditMutex);
		if (!s_audit)
			s_audit = new AuditCensus();
		AuditChunk(mVU, log, chunk, *s_audit);
	}

	void TestSetAuditEnabled(bool enabled)
	{
		std::lock_guard<std::mutex> lock(s_auditMutex);
		s_auditEnabled = enabled;
		delete s_audit;
		s_audit = enabled ? new AuditCensus() : nullptr;
	}

	u64 TestAuditUncoveredCount()
	{
		std::lock_guard<std::mutex> lock(s_auditMutex);
		u64 total = 0;
		if (s_audit)
			for (const auto& kv : s_audit->uncovered)
				total += kv.second;
		return total;
	}

	std::string TestAuditReport()
	{
		std::lock_guard<std::mutex> lock(s_auditMutex);
		std::string s;
		if (!s_audit)
			return s;
		char buf[192];
		std::snprintf(buf, sizeof(buf), "[audit] chunks=%llu insns=%llu data32=%llu\n",
			(unsigned long long)s_audit->chunks, (unsigned long long)s_audit->insns,
			(unsigned long long)s_audit->data32);
		s += buf;
		for (const auto& kv : s_audit->uncovered)
		{
			std::snprintf(buf, sizeof(buf), "[audit] uncovered %-28s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
			s += buf;
		}
		for (const auto& kv : s_audit->covered)
		{
			std::snprintf(buf, sizeof(buf), "[audit] covered   %-28s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
			s += buf;
		}
		for (const std::string& e : s_audit->examples)
			s += "[audit] example " + e + "\n";
		return s;
	}
#endif // PCSX2_RECOMPILER_TESTS

	//------------------------------------------------------------------
	// Emit-time recorder
	//------------------------------------------------------------------

	class Recorder final : public ArmAddressRecorder
	{
	public:
		struct EpisodeBlock
		{
			microBlock* live;
			u32 entryOffset;
			u32 startPC;
		};

		microVU* mvu = nullptr;
		u8* chunkBase = nullptr;
		microProgram* prog = nullptr;
		MvuPersistLog* log = nullptr;
		std::vector<PersistFixup> fixups;
		std::vector<EpisodeBlock> episodeBlocks;
		bool active = false;
		bool failed = false;
		const char* failReason = nullptr;

		void Begin(microVU& m, u8* base)
		{
			pxAssert(!active);
			mvu = &m;
			chunkBase = base;
			prog = nullptr;
			log = nullptr;
			fixups.clear();
			episodeBlocks.clear();
			active = true;
			failed = false;
			failReason = nullptr;
		}

		void Fail(const char* reason)
		{
			if (!failed)
			{
				failed = true;
				failReason = reason;
			}
		}

		bool InSlab(const void* addr) const
		{
			// Slab extends to the PHYSICAL rec-region end, mVUcacheSafeZone
			// past the x86end reset threshold — compile sessions may
			// legitimately overshoot x86end into the safe zone, and chunks
			// recorded there still reference intra-slab targets.
			const u8* a = static_cast<const u8*>(addr);
			return a >= mvu->cache && a < mvu->prog.x86end + (mVUcacheSafeZone * _1mb);
		}

		bool InChunk(const void* addr) const
		{
			const u8* a = static_cast<const u8*>(addr);
			return a >= chunkBase && a < armGetCurrentCodePointer();
		}

		u32 OffsetOf(u8* at) const
		{
			pxAssert(at >= chunkBase);
			return static_cast<u32>(at - chunkBase);
		}

		// Index a block of this episode will get once committed.
		u32 ProvisionalIndex(size_t episode_pos) const
		{
			return static_cast<u32>((log ? log->blocks.size() : 0) + episode_pos);
		}

		// --- ArmAddressRecorder ---

		// The episode block whose microBlock object holds `addr`, or -1.
		int EpisodeBlockHolding(const void* addr) const
		{
			const u8* a = static_cast<const u8*>(addr);
			for (size_t i = 0; i < episodeBlocks.size(); i++)
			{
				const u8* blk = reinterpret_cast<const u8*>(episodeBlocks[i].live);
				if (a >= blk && a < blk + sizeof(microBlock))
					return static_cast<int>(i);
			}
			return -1;
		}

		AddressClass ClassifyAddress(const void* addr) override
		{
			AddressClass c;
			if (EpisodeBlockHolding(addr) >= 0)
			{
				// Heap: the whole address moves, low bits included, so no ADRP.
				// The run is as wide as this pointer needs (two halfwords for a
				// non-PIE brk heap below 4GB, three for a PIE heap or a thread's
				// mmap arena, four for an Android tagged pointer). A process
				// whose allocator hands out a wider pointer for the same program
				// is rare (the heap a VU compiles on is fixed by the build and by
				// MTVU, which is in the options sentinel); hydration then
				// discards the program and it recompiles.
				c.relocatable = true;
				c.allowAdrp = false;
				c.minMovWidth = 1;
				return c;
			}
			if (RegionOf(addr) != kTargetCount)
			{
				c.relocatable = true;
				c.allowAdrp = true;
				c.minMovWidth = 1;
				return c;
			}
			if (InSlab(addr))
				Fail("address-of-code materialized outside branch helpers");
			else
				Fail("unclassifiable pointer materialized (heap?)");
			return c;
		}

		void OnMovRun(u8* at, u32 width, const void* addr) override
		{
			PersistFixup f = {};
			f.codeOffset = OffsetOf(at);
			f.form = kFormMovRun;
			f.width = static_cast<u8>(width);
			const int bi = EpisodeBlockHolding(addr);
			if (bi >= 0)
			{
				f.target = kTargetBlockObject;
				f.index = ProvisionalIndex(static_cast<size_t>(bi));
				f.offset = static_cast<const u8*>(addr) - reinterpret_cast<const u8*>(episodeBlocks[bi].live);
				fixups.push_back(f);
				return;
			}
			const u8 region = RegionOf(addr);
			if (region != kTargetCount)
			{
				f.target = region;
				f.offset = static_cast<s64>(reinterpret_cast<uptr>(addr) - RegionBase(region));
				fixups.push_back(f);
				return;
			}
			Fail("move target lost between classify and emit");
		}

		void OnAdrp(u8* at, const void* addr) override
		{
			const u8 region = RegionOf(addr);
			if (region != kTargetCount)
			{
				PersistFixup f = {};
				f.codeOffset = OffsetOf(at);
				f.form = kFormAdrp;
				f.target = region;
				f.offset = static_cast<s64>(reinterpret_cast<uptr>(addr) - RegionBase(region));
				fixups.push_back(f);
				return;
			}
			Fail("ADRP to an address outside the image and data arena");
		}

		void OnBranch(u8* at, const void* target, BranchForm form) override
		{
			if (InChunk(target))
				return; // relative distance preserved on relocation

			PersistFixup f = {};
			f.codeOffset = OffsetOf(at);
			f.form = (form == BranchForm::Rel19) ? kFormRel19 : kFormRel26;

			u8 stub_id = 0;
			if (ResolveStub(*mvu, target, stub_id))
			{
				f.target = kTargetStub;
				f.index = stub_id;
				fixups.push_back(f);
				return;
			}

			if (log)
			{
				const auto it = log->blockByEntry.find(target);
				if (it != log->blockByEntry.end())
				{
					f.target = kTargetBlockEntry;
					f.index = it->second;
					fixups.push_back(f);
					return;
				}
			}

			const u8 region = RegionOf(target);
			if (region != kTargetCount)
			{
				f.target = region;
				f.offset = static_cast<s64>(reinterpret_cast<uptr>(target) - RegionBase(region));
				fixups.push_back(f);
				return;
			}

			Fail(InSlab(target) ? "direct branch to unrecorded slab target"
			                    : "direct branch outside the slab, image and data arena");
		}

		// --- episode lifecycle ---

		void RegisterBlock(microBlock* block, u8* entry, u32 startPC_bytes)
		{
			if (failed)
				return;

			microProgram* cur = mvu->prog.cur;
			if (!cur)
				return;
			// A program the vu-aot live tier has taken over compiles through
			// vu-aot's regions, whose code bakes raw block pointers the fixup
			// recorder does not see (bail records). Nothing of it is persisted:
			// the log keeps the microVU graph recorded before the tier-up,
			// which is a valid image of its own.
			if (cur->vuaot)
			{
				Fail("vu-aot program");
				return;
			}
			if (!prog)
			{
				prog = cur;
				if (!prog->persist)
					prog->persist = new MvuPersistLog();
				log = prog->persist;
			}
			else if (prog != cur)
			{
				// mVUsearchProg switched programs mid-episode (JR/JALR slow
				// path). Splitting the chunk at the switch is possible but
				// not worth it — drop the episode.
				Fail("program switch mid-episode");
				return;
			}

			episodeBlocks.push_back({block, OffsetOf(entry), startPC_bytes});
		}

		void End(u8* chunkEnd)
		{
			pxAssert(active);
			active = false;

			const u32 vu = mvu->index & 1;
			if (episodeBlocks.empty() && fixups.empty())
				return; // nothing emitted (pure lookup episode / hydration)

			if (failed || episodeBlocks.empty() || !log)
			{
				s_stats[vu].chunksDropped++;
				if (failed)
					DevCon.WriteLn("mVUPersist: VU%u chunk dropped (%s)", vu, failReason);
				return;
			}

			// add() may have deduped a block against an existing variant (the
			// manager then keeps the OLD entry and this episode's copy of the
			// code is unreachable). Skipping just that record would shift the
			// provisional indices already baked into block-object fixups —
			// drop the whole episode instead. Rare corner, conservative.
			for (const EpisodeBlock& eb : episodeBlocks)
			{
				if (eb.live->hostEntry != chunkBase + eb.entryOffset)
				{
					s_stats[vu].chunksDropped++;
					DevCon.WriteLn("mVUPersist: VU%u chunk dropped (block manager deduped a variant)", vu);
					return;
				}
			}

			const u32 chunkIndex = static_cast<u32>(log->chunks.size());
			for (const EpisodeBlock& eb : episodeBlocks)
			{
				PersistBlockRec rec = {};
				rec.chunkIndex = chunkIndex;
				rec.entryOffset = eb.entryOffset;
				rec.startPC = eb.startPC;
				rec.hasJumpCache = (eb.live->jumpCache != nullptr) ? 1 : 0;
				std::memcpy(&rec.pState, &eb.live->pState, sizeof(microRegInfo));
				std::memcpy(&rec.pStateEnd, &eb.live->pStateEnd, sizeof(microRegInfo));
				rec.live = eb.live;
				log->blockByEntry.emplace(eb.live->hostEntry, static_cast<u32>(log->blocks.size()));
				log->blocks.push_back(rec);
				s_stats[vu].blocksRecorded++;
			}

			PersistChunk chunk;
			chunk.code.assign(chunkBase, chunkEnd);
			chunk.fixups = std::move(fixups);
			chunk.liveBase = chunkBase;
			s_stats[vu].fixupsRecorded += chunk.fixups.size();
			log->chunks.push_back(std::move(chunk));
			s_stats[vu].chunksRecorded++;
#ifdef PCSX2_RECOMPILER_TESTS
			AuditChunkIfEnabled(*mvu, *log, log->chunks.back());
#endif
		}
	};

	// VU0 episodes run on the EE thread, VU1 on the MTVU thread — same
	// affinity as armAsm itself, so per-VU recorder objects are safe.
	static Recorder s_recorder[2];

	void SetRecordingEnabled(bool enabled)
	{
		// The offline-tooling determinism gate trumps every enable request:
		// recording changes emitted code forms, and SetProcessDisable(true)
		// (pcsx2-vurunner --no-progcache) promises a byte-reproducible JIT.
		if (enabled && s_processDisabled)
			enabled = false;
		if (enabled == s_recordingEnabled)
			return;
		s_recordingEnabled = enabled;

		// Recording state is part of program identity (fixed-shape MOVZ/MOVK
		// runs change the bytes a program compiles to),
		// so flipping it invalidates both VUs' options sentinels. The next
		// consumer — mVUcomputeProgramHash at dispatch, mVUbuildOptionsSentinel
		// at init/reset, the ProgCache VERSION handshake — rebuilds with the
		// new state mixed in. Benign race: an MTVU compile between this store
		// and the recompiler reset that follows a config toggle hashes against
		// the new sentinel inside a cache dir keyed to the old one; the entry
		// is unreachable and the dir is evicted on the next handshake.
		microVU0.optionsSentinelValid = false;
		microVU1.optionsSentinelValid = false;
	}
	bool IsRecordingEnabled() { return s_recordingEnabled; }

	void SetProcessDisable(bool disable) { s_processDisabled = disable; }
	bool IsProcessDisabled() { return s_processDisabled; }

	void SyncRecordingFromConfig(bool config_enabled)
	{
		// Production: recording follows the program cache setting (EnableVUProgramCache
		// or EnableVUAot: UseVUProgramCache), called from
		// mVUinit/mVUreset before the options sentinel is rebuilt. The
		// test-manual override lets the recompiler-test harness drive
		// recording itself without these reset-time syncs clobbering it.
		if (s_manualRecording)
			return;
		const bool was = s_recordingEnabled;
		SetRecordingEnabled(config_enabled);
		if (s_recordingEnabled != was)
		{
			// One line per state change — the authoritative signal that the
			// persisted-JIT cache will (or won't) write .vuprog payloads this
			// session. Cheap to grep in emulog when diagnosing a "no payloads"
			// report. DevCon so it stays out of production logs but remains
			// greppable in Devel / raised-loglevel during bring-up.
			DevCon.WriteLn(Color_StrongBlue,
				"mVUProgCache: persisted-JIT recording %s (VuJitProgramCache=%d)",
				s_recordingEnabled ? "ENABLED" : "disabled",
				config_enabled ? 1 : 0);
		}
	}

	void SetTestManualRecording(bool manual) { s_manualRecording = manual; }

	void BeginEpisode(microVU& mVU, u8* chunkBase)
	{
		if (!s_recordingEnabled)
			return;
		Recorder& r = s_recorder[mVU.index & 1];
		r.Begin(mVU, chunkBase);
		armAddressRecorder = &r;
	}

	void EndEpisode(microVU& mVU, u8* chunkStart, u8* chunkEnd)
	{
		s_stats[mVU.index & 1].emittedCodeBytes += static_cast<u64>(chunkEnd - chunkStart);
		Recorder& r = s_recorder[mVU.index & 1];
		if (!r.active)
			return;
		pxAssert(armAddressRecorder == &r);
		armAddressRecorder = nullptr;
		r.End(chunkEnd);
	}

	void OnBlockCompiled(microVU& mVU, microBlock* block, u8* entry, u32 startPC_bytes)
	{
		const u32 vu = mVU.index & 1;
		s_blockCompiles[vu]++;
		Recorder& r = s_recorder[vu];
		if (r.active && armAddressRecorder == &r)
			r.RegisterBlock(block, entry, startPC_bytes);
	}

	void OnProgramDeleted(microProgram& prog)
	{
		delete prog.persist;
		prog.persist = nullptr;
	}

	//------------------------------------------------------------------
	// Serialization
	//------------------------------------------------------------------

	static constexpr u32 kImageMagic = 0x5055564Du; // 'MVUP'
	// 2: fixups as form + target, image and data-arena addresses recorded as
	//    region offsets; the checksum covers the header.
	// 3: the header names the build that recorded the payload (buildTag). An
	//    image fixup is an offset into the emulator module, which any other
	//    build lays out differently, so a payload is only good for the build
	//    that made it.
	static constexpr u32 kImageVersion = 3;

	struct ImageHeader
	{
		u32 magic;
		u32 version;
		u32 vuIndex;
		u32 flags;
		u64 hashLo;
		u64 hashHi;
		// The recording process's region bases. Informational only: every
		// address in the payload is a fixup resolved against the hydrating
		// process, so none of these has to match.
		u64 imageBase;
		u64 dataBase;
		u64 codeArena;
		u32 progStartPC; // microMem byte offset of the creating entry
		u32 rangeCount;
		u32 blockCount;
		u32 chunkCount;
		u64 payloadHash; // XXH3-64 of the whole image with this field zeroed —
		                 // guards the on-disk .vuprog against bit rot / partial
		                 // writes the structural checks can't see (a flipped
		                 // CODE byte is valid structure but wrong code)
		u64 buildTag;    // BuildTag() of the recording build
	};
	static_assert(sizeof(ImageHeader) == 88);

	struct DiskRange
	{
		s32 start;
		s32 end;
	};

	struct DiskBlock
	{
		u32 chunkIndex;
		u32 entryOffset;
		u32 startPC;
		u32 hasJumpCache;
		u8 pState[sizeof(microRegInfo)];
		u8 pStateEnd[sizeof(microRegInfo)];
	};

	struct DiskChunkHeader
	{
		u32 codeSize;
		u32 fixupCount;
	};

	template <typename T>
	static void AppendPod(std::vector<u8>& out, const T& v)
	{
		const u8* p = reinterpret_cast<const u8*>(&v);
		out.insert(out.end(), p, p + sizeof(T));
	}

	bool SerializeProgram(microVU& mVU, const microProgram& prog, std::vector<u8>& out)
	{
		const MvuPersistLog* log = prog.persist;
		if (!log || log->blocks.empty() || !prog.contentHashValid)
			return false;

		out.clear();
		ImageHeader hdr = {};
		hdr.magic = kImageMagic;
		hdr.version = kImageVersion;
		hdr.vuIndex = mVU.index;
		hdr.hashLo = prog.contentHash.low64;
		hdr.hashHi = prog.contentHash.high64;
		hdr.imageBase = GetRegions().imageBase;
		hdr.dataBase = GetRegions().dataBase;
		hdr.codeArena = reinterpret_cast<u64>(SysMemory::GetCodePtr(0));
		hdr.buildTag = BuildTag();
		hdr.progStartPC = static_cast<u32>(prog.startPC) * 8u;
		hdr.rangeCount = prog.ranges ? static_cast<u32>(prog.ranges->size()) : 0;
		hdr.blockCount = static_cast<u32>(log->blocks.size());
		hdr.chunkCount = static_cast<u32>(log->chunks.size());
		AppendPod(out, hdr);

		if (prog.ranges)
		{
			for (const microRange& mr : *prog.ranges)
				AppendPod(out, DiskRange{mr.start, mr.end});
		}

		for (const PersistBlockRec& rec : log->blocks)
		{
			DiskBlock db = {};
			db.chunkIndex = rec.chunkIndex;
			db.entryOffset = rec.entryOffset;
			db.startPC = rec.startPC;
			db.hasJumpCache = rec.hasJumpCache;
			std::memcpy(db.pState, &rec.pState, sizeof(microRegInfo));
			std::memcpy(db.pStateEnd, &rec.pStateEnd, sizeof(microRegInfo));
			AppendPod(out, db);
		}

		for (const PersistChunk& chunk : log->chunks)
		{
			AppendPod(out, DiskChunkHeader{static_cast<u32>(chunk.code.size()),
				static_cast<u32>(chunk.fixups.size())});
			for (const PersistFixup& f : chunk.fixups)
				AppendPod(out, f);
			out.insert(out.end(), chunk.code.begin(), chunk.code.end());
		}

		// Stamp the checksum into the already-appended header (computed with
		// the field still zero).
		const u64 payload_hash = XXH3_64bits(out.data(), out.size());
		std::memcpy(out.data() + offsetof(ImageHeader, payloadHash),
			&payload_hash, sizeof(payload_hash));
		return true;
	}

	// Checksum of a serialized image as SerializeProgram computed it.
	static u64 ImageChecksum(const u8* data, size_t size)
	{
		std::vector<u8> copy(data, data + size);
		std::memset(copy.data() + offsetof(ImageHeader, payloadHash), 0, sizeof(u64));
		return XXH3_64bits(copy.data(), copy.size());
	}

	//------------------------------------------------------------------
	// Instruction patchers
	//------------------------------------------------------------------

	enum class PatchResult
	{
		Ok,
		BadEncoding, // the code at the site is not the form the fixup names
		OutOfRange,  // the target cannot be encoded from this placement
	};

	// Encode `value` into the fixup's instruction(s). `at` is the address the
	// code executes from (the PC for relative forms); `insn` is where its words
	// are read and written (the RW alias under iOS dual mapping). With `write`
	// false it only checks, so a placement can be validated before anything is
	// installed.
	static PatchResult PatchSite(u32* insn, const u8* at, const PersistFixup& f, u64 value, bool write)
	{
		switch (f.form)
		{
			case kFormMovRun:
			{
				// movz xN, #imm16 (hw=0), then movk xN, #imm16, lsl 16*i, one Rd.
				const u32 w = f.width;
				const u32 rd = insn[0] & 31u;
				for (u32 i = 0; i < w; i++)
				{
					const u32 expect_op = (i == 0) ? 0xD2800000u : 0xF2800000u;
					if ((insn[i] & 0xFF800000u) != expect_op || ((insn[i] >> 21) & 3u) != i || (insn[i] & 31u) != rd)
						return PatchResult::BadEncoding;
				}
				if (w < 4 && (value >> (16 * w)) != 0)
					return PatchResult::OutOfRange;
				if (write)
				{
					for (u32 i = 0; i < w; i++)
						insn[i] = (insn[i] & ~(0xFFFFu << 5)) | (static_cast<u32>((value >> (16 * i)) & 0xFFFFu) << 5);
				}
				return PatchResult::Ok;
			}
			case kFormAdrp:
			{
				// ADRP = 0x90000000 | immlo[30:29] | immhi[23:5].
				if ((*insn & 0x9F000000u) != 0x90000000u)
					return PatchResult::BadEncoding;
				const s64 page_disp = static_cast<s64>(value >> 12) -
				                      static_cast<s64>(reinterpret_cast<uptr>(at) >> 12);
				if (!vixl::IsInt21(page_disp))
					return PatchResult::OutOfRange;
				if (write)
				{
					const u32 immlo = static_cast<u32>(page_disp) & 3u;
					const u32 immhi = (static_cast<u32>(page_disp) >> 2) & 0x7FFFFu;
					*insn = (*insn & 0x9F00001Fu) | (immlo << 29) | (immhi << 5);
				}
				return PatchResult::Ok;
			}
			case kFormRel26:
			{
				// B = 0x14000000, BL = 0x94000000; imm26 in [25:0].
				if ((*insn & 0x7C000000u) != 0x14000000u)
					return PatchResult::BadEncoding;
				const s64 disp = GetPCDisplacement(at, reinterpret_cast<const void*>(value));
				if (!vixl::IsInt26(disp))
					return PatchResult::OutOfRange;
				if (write)
					*insn = (*insn & 0xFC000000u) | (static_cast<u32>(disp) & 0x03FFFFFFu);
				return PatchResult::Ok;
			}
			case kFormRel19:
			{
				// B.cond (0x54000000, bit 4 clear) or CBZ/CBNZ (0x34000000); imm19 in [23:5].
				const bool bcond = (*insn & 0xFF000010u) == 0x54000000u;
				const bool cbz = (*insn & 0x7E000000u) == 0x34000000u;
				if (!bcond && !cbz)
					return PatchResult::BadEncoding;
				const s64 disp = GetPCDisplacement(at, reinterpret_cast<const void*>(value));
				if (!vixl::IsInt19(disp))
					return PatchResult::OutOfRange;
				if (write)
					*insn = (*insn & ~(0x7FFFFu << 5)) | ((static_cast<u32>(disp) & 0x7FFFFu) << 5);
				return PatchResult::Ok;
			}
			default:
				return PatchResult::BadEncoding;
		}
	}

	//------------------------------------------------------------------
	// Hydration
	//------------------------------------------------------------------

#ifdef PCSX2_RECOMPILER_TESTS
	// Placement of the most recent hydration, for TestVerifyHydratedCode.
	// Test-only state — never written in a hooks-off Release build.
	static std::vector<u8*> s_lastHydratedChunkBase[2];
#endif

	template <typename T>
	static bool ReadPod(const u8*& p, const u8* end, T& out)
	{
		if (static_cast<size_t>(end - p) < sizeof(T))
			return false;
		std::memcpy(&out, p, sizeof(T));
		p += sizeof(T);
		return true;
	}

	// Take a just-created program back out: it is in the content map only
	// (the caller has not pushed it into any per-PC list yet). Its code stays
	// in the slab, unreferenced, until the next cache reset.
	static void DiscardHydratedProgram(microVU& mVU, microProgram*& prog)
	{
		const auto it = mVU.mvuContentMap.find(prog->contentHash);
		if (it != mVU.mvuContentMap.end() && it->second == prog)
			mVU.mvuContentMap.erase(it);
		mVUdeleteProg(mVU, prog);
		prog = nullptr;
	}

	microProgram* HydrateProgram(microVU& mVU, const u8* data, size_t size)
	{
		const u32 vu = mVU.index & 1;
		const u8* p = data;
		const u8* const end = data + size;

		ImageHeader hdr;
		if (!ReadPod(p, end, hdr) || hdr.magic != kImageMagic || hdr.version != kImageVersion ||
			hdr.vuIndex != mVU.index)
		{
			s_stats[vu].hydrationRejects++;
			return nullptr;
		}

		// A payload from another build: its image fixups are offsets into that build's module, so
		// patching them here would call whatever sits at those offsets in this one.
		if (hdr.buildTag != BuildTag())
		{
			DevCon.Warning("mVUPersist: VU%u hydration rejected — payload recorded by another build", vu);
			s_stats[vu].hydrationRejects++;
			return nullptr;
		}

		// Image integrity. The structural checks below validate shape, not
		// content — a flipped code byte parses fine and runs wrong.
		if (ImageChecksum(data, size) != hdr.payloadHash)
		{
			DevCon.Warning("mVUPersist: VU%u hydration rejected — payload checksum mismatch", vu);
			s_stats[vu].hydrationRejects++;
			return nullptr;
		}

		// Without the region bases no image or data address can be resolved.
		if (!GetRegions().valid)
		{
			s_stats[vu].hydrationRejects++;
			return nullptr;
		}

		// Identity: the image must describe the program bytes currently in
		// micro memory.
		const XXH128_hash_t live_hash = mVUcomputeProgramHash(mVU);
		if (hdr.hashLo != live_hash.low64 || hdr.hashHi != live_hash.high64)
		{
			s_stats[vu].hydrationRejects++;
			return nullptr;
		}

		std::vector<DiskRange> ranges(hdr.rangeCount);
		for (DiskRange& r : ranges)
			if (!ReadPod(p, end, r))
				return nullptr;

		std::vector<DiskBlock> blocks(hdr.blockCount);
		for (DiskBlock& b : blocks)
			if (!ReadPod(p, end, b))
				return nullptr;

		struct ParsedChunk
		{
			const u8* code;
			u32 codeSize;
			std::vector<PersistFixup> fixups;
		};
		std::vector<ParsedChunk> chunks(hdr.chunkCount);
		size_t total_code = 0;
		for (ParsedChunk& c : chunks)
		{
			DiskChunkHeader ch;
			if (!ReadPod(p, end, ch))
				return nullptr;
			c.fixups.resize(ch.fixupCount);
			for (PersistFixup& f : c.fixups)
				if (!ReadPod(p, end, f))
					return nullptr;
			if (static_cast<size_t>(end - p) < ch.codeSize)
				return nullptr;
			c.code = p;
			c.codeSize = ch.codeSize;
			p += ch.codeSize;
			total_code += ch.codeSize + 16;
		}

		// Structural validation before any side effects.
		for (const DiskBlock& b : blocks)
		{
			if (b.chunkIndex >= chunks.size() || b.startPC >= mVU.microMemSize ||
				(b.startPC & 7) || b.entryOffset >= chunks[b.chunkIndex].codeSize)
				return nullptr;
		}
		for (const ParsedChunk& c : chunks)
		{
			for (const PersistFixup& f : c.fixups)
			{
				if (f.form >= kFormCount || f.target >= kTargetCount)
					return nullptr;
				if (f.form == kFormMovRun && (f.width < 1 || f.width > 4))
					return nullptr;
				if ((f.codeOffset & 3) || f.codeOffset + FixupSpanInsns(f) * 4 > c.codeSize)
					return nullptr;
				if ((f.target == kTargetBlockObject || f.target == kTargetBlockEntry) &&
					f.index >= blocks.size())
					return nullptr;
				if (f.target == kTargetBlockObject && (f.offset < 0 || f.offset >= static_cast<s64>(sizeof(microBlock))))
					return nullptr;
				if (f.target == kTargetStub && (f.index >= kStubCount || !StubAddress(mVU, static_cast<u8>(f.index))))
					return nullptr;
			}
		}

		// Capacity: refuse rather than trip the mid-hydration cache-full reset.
		// Signed math: x86ptr may legitimately sit PAST x86end when an earlier
		// session in this execution overshot into the safe zone — an unsigned
		// subtraction would wrap huge and let the hydration through instead of
		// refusing it.
		const ptrdiff_t cache_free = mVU.prog.x86end - mVU.prog.x86ptr;
		if (cache_free < 0 ||
			total_code + (mVUcacheSafeZone * _1mb) > static_cast<size_t>(cache_free))
		{
			s_stats[vu].hydrationRejects++;
			return nullptr;
		}

		pxAssert(!armAsm); // caller must not have an open emission episode
		mVUopenCodeCache(mVU);

		// Where each chunk will land: the copy loop below pads the cursor
		// offset (from x86start, which need not itself be aligned) to 16 bytes
		// before each chunk, exactly as here.
		std::vector<u8*> chunk_base(chunks.size());
		{
			ptrdiff_t cursor = armAsm->GetCursorOffset();
			for (size_t i = 0; i < chunks.size(); i++)
			{
				cursor = (cursor + 15) & ~static_cast<ptrdiff_t>(15);
				chunk_base[i] = mVU.prog.x86start + cursor;
				cursor += chunks[i].codeSize;
			}
		}

		// Every target except a block object (heap, known only once the blocks
		// are registered) resolves now, against this process.
		auto resolve = [&](const PersistFixup& f, u64& out) -> bool {
			switch (f.target)
			{
				case kTargetBlockEntry:
				{
					const DiskBlock& b = blocks[f.index];
					out = reinterpret_cast<u64>(chunk_base[b.chunkIndex] + b.entryOffset);
					return true;
				}
				case kTargetStub:
					out = reinterpret_cast<u64>(StubAddress(mVU, static_cast<u8>(f.index)));
					return true;
				case kTargetImage:
				case kTargetData:
					out = static_cast<u64>(RegionBase(f.target)) + static_cast<u64>(f.offset);
					return true;
				default:
					return false;
			}
		};

		// Validate every placement-dependent encoding against the original
		// bytes before anything is installed: a target out of reach (a far data
		// arena for an ADRP, a narrower MOVZ/MOVK run than a new address needs)
		// rejects the payload cleanly.
		for (size_t i = 0; i < chunks.size(); i++)
		{
			for (const PersistFixup& f : chunks[i].fixups)
			{
				u64 value = 0;
				if (!resolve(f, value))
					continue;
				u32 words[4] = {};
				std::memcpy(words, chunks[i].code + f.codeOffset, FixupSpanInsns(f) * 4);
				const PatchResult r = PatchSite(words, chunk_base[i] + f.codeOffset, f, value, false);
				if (r != PatchResult::Ok)
				{
					DevCon.Warning("mVUPersist: VU%u hydration rejected — fixup (form %u target %u) %s at the new placement",
						vu, f.form, f.target, (r == PatchResult::OutOfRange) ? "out of range" : "does not match its code");
					mVUcloseCodeCache(mVU);
					s_stats[vu].hydrationRejects++;
					return nullptr;
				}
			}
		}

		microProgram* prog = mVUcreateProg(mVU, static_cast<int>(hdr.progStartPC / 8), live_hash);

		// Restore compiled ranges + the range-compare image (mVUcmpProg under
		// !doWholeProgCompare diffs prog.data inside these windows; the live
		// microMem is identity-verified above, so copying from it is exact).
		prog->ranges->clear();
		for (const DiskRange& r : ranges)
		{
			prog->ranges->push_back(microRange{r.start, r.end});
			if (!doWholeProgCompare && r.start >= 0 && r.end > r.start &&
				static_cast<u32>(r.end) <= mVU.microMemSize)
			{
				std::memcpy(reinterpret_cast<u8*>(prog->data) + r.start,
					mVU.regs().Micro + r.start, static_cast<size_t>(r.end - r.start));
			}
		}

		// Copy each chunk to the current cursor (16-aligned, as recorded).
		for (size_t i = 0; i < chunks.size(); i++)
		{
			while (armAsm->GetCursorOffset() & 15)
				armAsm->Nop();
			pxAssertRel(mVU.prog.x86start + armAsm->GetCursorOffset() == chunk_base[i],
				"mVUPersist: hydration placement differs from the validated one");
			{
				vixl::CodeBufferCheckScope scope(armAsm, chunks[i].codeSize,
					vixl::CodeBufferCheckScope::kReserveBufferSpace,
					vixl::CodeBufferCheckScope::kNoAssert);
				armAsm->GetBuffer()->EmitData(chunks[i].code, chunks[i].codeSize);
			}
		}

		// Register every block with the program's managers. add() dedupes by
		// pState, mirroring compile-time behavior.
		std::vector<microBlock*> final_block(blocks.size());
		for (size_t i = 0; i < blocks.size(); i++)
		{
			const DiskBlock& b = blocks[i];
			const u32 slot = b.startPC / 8;
			if (!prog->block[slot])
				prog->block[slot] = new microBlockManager();

			microBlock tmp;
			std::memcpy(&tmp.pState, b.pState, sizeof(microRegInfo));
			std::memcpy(&tmp.pStateEnd, b.pStateEnd, sizeof(microRegInfo));
			tmp.x86ptrStart = chunk_base[b.chunkIndex] + b.entryOffset;
			tmp.hostEntry = tmp.x86ptrStart;
			tmp.jumpCache = nullptr;

			microBlock* installed = prog->block[slot]->add(mVU, &tmp);
			// mVUcompileJIT dereferences jumpCache unguarded (it was allocated
			// at compile time by normJumpCompile) — restore it eagerly.
			if (b.hasJumpCache && !installed->jumpCache)
				installed->jumpCache = new microJumpCache[mProgSize / 2];
			final_block[i] = installed;
		}

		// Patch fixups directly in the slab (still inside the BeginCodeWrite
		// window; mVUcloseCodeCache flushes the icache over the whole span).
		// Nothing can run these blocks yet: the program is reachable only
		// through the content map, from this thread.
		for (size_t i = 0; i < chunks.size(); i++)
		{
			for (const PersistFixup& f : chunks[i].fixups)
			{
				u8* at = chunk_base[i] + f.codeOffset;
				// iOS dual-map W^X: `at` is the RX address the displacement math
				// must use; the store itself goes through the RW alias. Identity
				// on every other platform.
				u32* insn = reinterpret_cast<u32*>(armGetWritableCodePtr(at));
				u64 value = 0;
				if (f.target == kTargetBlockObject)
					value = reinterpret_cast<u64>(final_block[f.index]) + static_cast<u64>(f.offset);
				else if (f.target == kTargetBlockEntry)
					value = reinterpret_cast<u64>(final_block[f.index]->hostEntry);
				else
					resolve(f, value);

				const PatchResult r = PatchSite(insn, at, f, value, true);
				if (r != PatchResult::Ok)
				{
					// Only a block object can fail here (the rest was validated
					// above): an allocator that hands out a pointer wider than the
					// recorded run. Take the program back out and recompile.
					DevCon.Warning("mVUPersist: VU%u hydration rejected — block pointer %p does not fit a %u-instruction run",
						vu, reinterpret_cast<void*>(value), f.width);
					DiscardHydratedProgram(mVU, prog);
					mVUcloseCodeCache(mVU);
					s_stats[vu].hydrationRejects++;
					return nullptr;
				}
			}
		}

		mVUcloseCodeCache(mVU);

		// Rebuild the persist log so the hydrated program can grow new chunks
		// and be re-serialized. The recorded bytes (not the patched ones) are
		// kept: every address operand in them is a fixup, resolved afresh by
		// whichever process hydrates the re-saved payload.
		delete prog->persist;
		prog->persist = new MvuPersistLog();
		MvuPersistLog* log = prog->persist;
		for (size_t i = 0; i < chunks.size(); i++)
		{
			PersistChunk pc;
			pc.code.assign(chunks[i].code, chunks[i].code + chunks[i].codeSize);
			pc.fixups = chunks[i].fixups;
			pc.liveBase = chunk_base[i];
			log->chunks.push_back(std::move(pc));
		}
		for (size_t i = 0; i < blocks.size(); i++)
		{
			PersistBlockRec rec = {};
			rec.chunkIndex = blocks[i].chunkIndex;
			rec.entryOffset = blocks[i].entryOffset;
			rec.startPC = blocks[i].startPC;
			rec.hasJumpCache = blocks[i].hasJumpCache;
			std::memcpy(&rec.pState, blocks[i].pState, sizeof(microRegInfo));
			std::memcpy(&rec.pStateEnd, blocks[i].pStateEnd, sizeof(microRegInfo));
			rec.live = final_block[i];
			log->blockByEntry.emplace(final_block[i]->hostEntry, static_cast<u32>(i));
			log->blocks.push_back(rec);
		}

#ifdef PCSX2_RECOMPILER_TESTS
		s_lastHydratedChunkBase[vu] = chunk_base;
		for (const PersistChunk& pc : log->chunks)
			AuditChunkIfEnabled(mVU, *log, pc);
#endif
		s_stats[vu].programsHydrated++;
		s_stats[vu].blocksHydrated += blocks.size();
		return prog;
	}

	//------------------------------------------------------------------
	// Test hooks
	//------------------------------------------------------------------

	bool TestSerializeNewestProgram(u32 vu_index, std::vector<u8>& out)
	{
		microVU& mVU = (vu_index & 1) ? microVU1 : microVU0;
		microProgram* newest = nullptr;
		for (const auto& entry : mVU.mvuContentMap)
		{
			if (!newest || entry.second->idx > newest->idx)
				newest = entry.second;
		}
		if (!newest)
			return false;
		return SerializeProgram(mVU, *newest, out);
	}

	bool TestHydrate(u32 vu_index, const u8* data, size_t size)
	{
		microVU& mVU = (vu_index & 1) ? microVU1 : microVU0;
		return HydrateProgram(mVU, data, size) != nullptr;
	}

#ifdef PCSX2_RECOMPILER_TESTS
	bool TestVerifyHydratedCode(u32 vu_index, const u8* image, size_t size)
	{
		const u32 vu = vu_index & 1;
		const std::vector<u8*>& placed = s_lastHydratedChunkBase[vu];
		if (placed.empty())
			return false;

		// Re-parse the image (cheap, and avoids holding parse state).
		const u8* p = image;
		const u8* const end = image + size;
		ImageHeader hdr;
		if (!ReadPod(p, end, hdr) || hdr.chunkCount != placed.size())
			return false;
		// Bounds-check each section advance before adding (mirror ReadPod):
		// an unchecked p += count*size can step past end, after which the
		// per-chunk `end - p` guard below underflows to a huge size_t.
		if (static_cast<size_t>(end - p) < static_cast<size_t>(hdr.rangeCount) * sizeof(DiskRange))
			return false;
		p += static_cast<size_t>(hdr.rangeCount) * sizeof(DiskRange);
		if (static_cast<size_t>(end - p) < static_cast<size_t>(hdr.blockCount) * sizeof(DiskBlock))
			return false;
		p += static_cast<size_t>(hdr.blockCount) * sizeof(DiskBlock);

		for (u32 i = 0; i < hdr.chunkCount; i++)
		{
			DiskChunkHeader ch;
			if (!ReadPod(p, end, ch))
				return false;
			std::vector<PersistFixup> fixups(ch.fixupCount);
			for (PersistFixup& f : fixups)
				if (!ReadPod(p, end, f))
					return false;
			if (static_cast<size_t>(end - p) < ch.codeSize)
				return false;
			const u8* expect = p;
			p += ch.codeSize;

			std::vector<u8> mask(ch.codeSize, 0);
			for (const PersistFixup& f : fixups)
			{
				const u32 span = FixupSpanInsns(f) * 4;
				for (u32 b = 0; b < span && f.codeOffset + b < ch.codeSize; b++)
					mask[f.codeOffset + b] = 1;
			}

			const u8* live = placed[i];
			for (u32 b = 0; b < ch.codeSize; b++)
			{
				if (!mask[b] && live[b] != expect[b])
				{
					DevCon.Error("mVUPersist: hydrated code mismatch at chunk %u offset 0x%x (live %02x != image %02x)",
						i, b, live[b], expect[b]);
					return false;
				}
			}
		}
		return true;
	}
#endif // PCSX2_RECOMPILER_TESTS

#ifdef PCSX2_RECOMPILER_TESTS
	u64 TestAuditCoveredCount(const char* key)
	{
		std::lock_guard<std::mutex> lock(s_auditMutex);
		if (!s_audit)
			return 0;
		const auto it = s_audit->covered.find(key);
		return (it != s_audit->covered.end()) ? it->second : 0;
	}

	void TestGetRegionBases(u64& image_base, u64& data_base)
	{
		image_base = GetRegions().imageBase;
		data_base = GetRegions().dataBase;
	}

	static void RestampChecksum(std::vector<u8>& image)
	{
		std::memset(image.data() + offsetof(ImageHeader, payloadHash), 0, sizeof(u64));
		const u64 h = XXH3_64bits(image.data(), image.size());
		std::memcpy(image.data() + offsetof(ImageHeader, payloadHash), &h, sizeof(h));
	}

	bool TestEditFirstFixup(std::vector<u8>& image, u8 target, u8 width, s64 offset_delta)
	{
		u8* p = image.data();
		u8* const end = p + image.size();
		ImageHeader hdr;
		const u8* cp = p;
		if (!ReadPod(cp, end, hdr))
			return false;
		size_t pos = sizeof(ImageHeader) + static_cast<size_t>(hdr.rangeCount) * sizeof(DiskRange) +
		             static_cast<size_t>(hdr.blockCount) * sizeof(DiskBlock);
		for (u32 c = 0; c < hdr.chunkCount; c++)
		{
			if (pos + sizeof(DiskChunkHeader) > image.size())
				return false;
			DiskChunkHeader ch;
			std::memcpy(&ch, p + pos, sizeof(ch));
			pos += sizeof(ch);
			for (u32 i = 0; i < ch.fixupCount; i++, pos += sizeof(PersistFixup))
			{
				if (pos + sizeof(PersistFixup) > image.size())
					return false;
				PersistFixup f;
				std::memcpy(&f, p + pos, sizeof(f));
				if (f.target != target)
					continue;
				if (width)
					f.width = width;
				f.offset += offset_delta;
				std::memcpy(p + pos, &f, sizeof(f));
				RestampChecksum(image);
				return true;
			}
			pos += ch.codeSize;
		}
		return false;
	}

	bool TestOverwriteHeaderBases(std::vector<u8>& image, u64 value)
	{
		if (image.size() < sizeof(ImageHeader))
			return false;
		std::memcpy(image.data() + offsetof(ImageHeader, imageBase), &value, sizeof(value));
		std::memcpy(image.data() + offsetof(ImageHeader, dataBase), &value, sizeof(value));
		std::memcpy(image.data() + offsetof(ImageHeader, codeArena), &value, sizeof(value));
		RestampChecksum(image);
		return true;
	}
#endif

	u64 GetBlockCompileCount(u32 vu_index) { return s_blockCompiles[vu_index & 1]; }
	Stats GetStats(u32 vu_index) { return s_stats[vu_index & 1]; }

	//------------------------------------------------------------------
	// ABI-digest guard support
	//------------------------------------------------------------------

	// Zero every operand field that may legitimately differ between two
	// correct emissions of the same program (addresses + PC-relative
	// displacements to relocatable or layout-dependent targets). Everything
	// left — opcode bits, register fields, data immediates, instruction
	// order — is the emitter's shape.
	static u32 CanonicalizeInsn(u32 insn)
	{
		// 64-bit MOVZ/MOVN/MOVK (sf=1): address materialization. 32-bit
		// forms carry data constants (cycle counts etc.) and stay intact.
		const u32 mov_op = insn & 0xFF800000u;
		if (mov_op == 0xD2800000u || mov_op == 0xF2800000u || mov_op == 0x92800000u)
			return insn & ~(0xFFFFu << 5); // clear imm16, keep hw + Rd
		// B / BL: stub + cross-chunk displacements depend on placement.
		if ((insn & 0x7C000000u) == 0x14000000u)
			return insn & 0xFC000000u;
		// ADRP: page displacement is placement-dependent by construction.
		if ((insn & 0x9F000000u) == 0x90000000u)
			return insn & ~((3u << 29) | (0x7FFFFu << 5));
		return insn;
	}

	bool TestComputeEmitDigest(u32 vu_index, u64& out_digest)
	{
		out_digest = 0;
		std::vector<u8> image;
		if (!TestSerializeNewestProgram(vu_index, image))
			return false;

		const u8* p = image.data();
		const u8* const end = p + image.size();
		ImageHeader hdr;
		if (!ReadPod(p, end, hdr))
			return false;

		// Canonical buffer: structure counts + ranges + block geometry +
		// fixup records (sans address payloads) + canonicalized code.
		// Header hashes/bases and microRegInfo bytes are excluded — the
		// former are per-run, the latter may carry padding.
		std::vector<u8> canon;
		AppendPod(canon, hdr.vuIndex);
		AppendPod(canon, hdr.progStartPC);
		AppendPod(canon, hdr.rangeCount);
		AppendPod(canon, hdr.blockCount);
		AppendPod(canon, hdr.chunkCount);

		for (u32 i = 0; i < hdr.rangeCount; i++)
		{
			DiskRange r;
			if (!ReadPod(p, end, r))
				return false;
			AppendPod(canon, r);
		}
		for (u32 i = 0; i < hdr.blockCount; i++)
		{
			DiskBlock b;
			if (!ReadPod(p, end, b))
				return false;
			AppendPod(canon, b.chunkIndex);
			AppendPod(canon, b.entryOffset);
			AppendPod(canon, b.startPC);
			AppendPod(canon, b.hasJumpCache);
		}
		for (u32 i = 0; i < hdr.chunkCount; i++)
		{
			DiskChunkHeader ch;
			if (!ReadPod(p, end, ch))
				return false;
			AppendPod(canon, ch);
			// Image and data-arena offsets are left out: they move with any
			// change anywhere in the binary, not with the emitter's shape.
			std::vector<u8> rel19_site;
			for (u32 f = 0; f < ch.fixupCount; f++)
			{
				PersistFixup fx;
				if (!ReadPod(p, end, fx))
					return false;
				AppendPod(canon, fx.codeOffset);
				AppendPod(canon, fx.form);
				AppendPod(canon, fx.target);
				AppendPod(canon, fx.width);
				AppendPod(canon, fx.index);
				if (fx.target == kTargetBlockObject)
					AppendPod(canon, fx.offset);
				if (fx.form == kFormRel19)
				{
					if (rel19_site.size() < ch.codeSize / 4)
						rel19_site.resize(ch.codeSize / 4, 0);
					if (fx.codeOffset / 4 < rel19_site.size())
						rel19_site[fx.codeOffset / 4] = 1;
				}
			}
			if (static_cast<size_t>(end - p) < ch.codeSize || (ch.codeSize & 3))
				return false;
			for (u32 off = 0; off < ch.codeSize; off += 4)
			{
				u32 insn;
				std::memcpy(&insn, p + off, 4);
				u32 c = CanonicalizeInsn(insn);
				// A recorded B.cond / CBZ across chunks: its displacement depends
				// on what was compiled between the chunks.
				if (off / 4 < rel19_site.size() && rel19_site[off / 4])
					c &= ~(0x7FFFFu << 5);
				AppendPod(canon, c);
			}
			p += ch.codeSize;
		}

		out_digest = XXH3_64bits(canon.data(), canon.size());
		return true;
	}
} // namespace mVUPersist
