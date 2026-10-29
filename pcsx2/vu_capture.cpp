// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vu_capture.h"

#ifdef PCSX2_RECOMPILER_TESTS

#include "Config.h"   // EmuConfig — SnapshotConfig reads the live effective config
#include "VMManager.h" // disc serial/CRC for capture provenance
#include "MTVU.h"      // vu1Thread.vifRegs — VU1's copy of the VIF registers under MTVU
#include "Vif.h"
#include "VU.h"
#include "VUmicro.h"  // VU0_PROGSIZE / VU1_PROGSIZE / VU0_MEMSIZE / VU1_MEMSIZE
#include "R5900.h"    // cpuRegs.cycle — the EE clock, logged in trajectory mode

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace vu_capture
{
	// Only used to salt filenames/reports so concurrent processes don't
	// collide — any process-unique integer will do.
	static int capture_pid()
	{
#ifdef _WIN32
		return ::_getpid();
#else
		return ::getpid();
#endif
	}
	namespace
	{
		// Single mutex covers all WriteToFile callers so concurrent VU0/VU1
		// dispatcher probes can't interleave bytes within one file. (Different
		// files would be safe to write in parallel, but the cost of one-mutex
		// is trivial and the simplicity is worth it.)
		std::mutex& WriterMutex()
		{
			static std::mutex m;
			return m;
		}

		u32 ExpectedSizeFor(u8 vu_index)
		{
			return vu_index ? VU1_PROGSIZE : VU0_PROGSIZE;  // PROG == MEM size
		}

		// FNV-1a over a byte range. Same construction as the vurunner digest so
		// trajectory hashes are comparable in spirit (values need not match the
		// runner's — only self-consistency across two trajectory runs matters).
		u64 Fnv1a(const void* data, size_t len, u64 seed = 0xcbf29ce484222325ull)
		{
			const u8* p = static_cast<const u8*>(data);
			u64 h = seed;
			for (size_t i = 0; i < len; ++i)
			{
				h ^= p[i];
				h *= 0x100000001b3ull;
			}
			return h;
		}
	} // namespace

	bool WriteToFile(const std::string& path, const CaptureRecord& rec)
	{
		const u32 expected = ExpectedSizeFor(rec.vu_index);
		if (rec.microcode.size() != expected || rec.vumem.size() != expected)
			return false;

		std::lock_guard lock(WriterMutex());

		std::FILE* f = std::fopen(path.c_str(), "wb");
		if (!f)
			return false;

		FileHeader hdr{};
		std::memcpy(hdr.magic, kMagic, sizeof(hdr.magic));
		hdr.version = kVersion;
		hdr.vu_index = rec.vu_index;
		hdr.start_pc = rec.start_pc;
		hdr.cycle_budget = rec.cycle_budget;
		hdr.microcode_size = static_cast<u32>(rec.microcode.size());
		hdr.vumem_size = static_cast<u32>(rec.vumem.size());

		bool ok = true;
		ok &= (std::fwrite(&hdr, sizeof(hdr), 1, f) == 1);
		ok &= (std::fwrite(&rec.config, sizeof(rec.config), 1, f) == 1);
		ok &= (std::fwrite(&rec.external, sizeof(rec.external), 1, f) == 1);
		ok &= (std::fwrite(rec.microcode.data(), 1, rec.microcode.size(), f) == rec.microcode.size());
		ok &= (std::fwrite(rec.vumem.data(), 1, rec.vumem.size(), f) == rec.vumem.size());
		ok &= (std::fwrite(&rec.state, sizeof(rec.state), 1, f) == 1);

		std::fclose(f);
		return ok;
	}

	bool ReadFromFile(const std::string& path, CaptureRecord& rec_out)
	{
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f)
			return false;

		FileHeader hdr{};
		if (std::fread(&hdr, sizeof(hdr), 1, f) != 1)
		{
			std::fclose(f);
			return false;
		}
		if (std::memcmp(hdr.magic, kMagic, sizeof(hdr.magic)) != 0 ||
			hdr.version < kMinReadVersion || hdr.version > kVersion)
		{
			std::fclose(f);
			return false;
		}
		if (hdr.vu_index > 1)
		{
			std::fclose(f);
			return false;
		}
		const u32 expected = ExpectedSizeFor(hdr.vu_index);
		if (hdr.microcode_size != expected || hdr.vumem_size != expected)
		{
			std::fclose(f);
			return false;
		}

		rec_out.vu_index = hdr.vu_index;
		rec_out.start_pc = hdr.start_pc;
		rec_out.cycle_budget = hdr.cycle_budget;
		rec_out.microcode.assign(hdr.microcode_size, 0);
		rec_out.vumem.assign(hdr.vumem_size, 0);
		rec_out.config = CapturedConfig{}; // v1 files carry no snapshot
		rec_out.external = CapturedExternal{}; // nor do v1/v2 files carry VIF state

		bool ok = true;
		if (hdr.version >= 2)
			ok &= (std::fread(&rec_out.config, sizeof(rec_out.config), 1, f) == 1);
		if (hdr.version >= 3)
			ok &= (std::fread(&rec_out.external, sizeof(rec_out.external), 1, f) == 1);
		ok &= (std::fread(rec_out.microcode.data(), 1, hdr.microcode_size, f) == hdr.microcode_size);
		ok &= (std::fread(rec_out.vumem.data(), 1, hdr.vumem_size, f) == hdr.vumem_size);
		ok &= (std::fread(&rec_out.state, sizeof(rec_out.state), 1, f) == 1);

		std::fclose(f);
		return ok;
	}

	void SnapshotConfig(CapturedConfig& out)
	{
		std::memset(&out, 0, sizeof(out));
		out.flags = kConfigValid;

		// Disc identity — provenance. Empty/zero in headless environments
		// (recompiler_tests, vurunner re-capture) where no VM is running;
		// the snapshot stays valid, the config bits are the truth.
		const std::string serial = VMManager::GetDiscSerial();
		std::memcpy(out.serial, serial.data(),
			std::min(serial.size(), sizeof(out.serial) - 1));
		out.disc_crc = VMManager::GetDiscCRC();

		for (int i = GamefixId_FIRST; i < GamefixId_COUNT; ++i)
		{
			if (EmuConfig.Gamefixes.Get(static_cast<GamefixId>(i)))
				out.gamefixes |= 1u << i;
		}

		if (EmuConfig.Speedhacks.vuFlagHack)
			out.speedhacks |= kSpeedhackVuFlagHack;
		if (EmuConfig.Speedhacks.vuThread)
			out.speedhacks |= kSpeedhackVuThread;
		if (EmuConfig.Speedhacks.vu1Instant)
			out.speedhacks |= kSpeedhackVu1Instant;

		const auto& rec = EmuConfig.Cpu.Recompiler;
		if (rec.vu0Overflow)
			out.vu_clamp |= kClampVu0Overflow;
		if (rec.vu0ExtraOverflow)
			out.vu_clamp |= kClampVu0ExtraOverflow;
		if (rec.vu0SignOverflow)
			out.vu_clamp |= kClampVu0SignOverflow;
		if (rec.vu0ExactMode)
			out.vu_clamp |= kClampVu0ExactMode;
		if (rec.vu1Overflow)
			out.vu_clamp |= kClampVu1Overflow;
		if (rec.vu1ExtraOverflow)
			out.vu_clamp |= kClampVu1ExtraOverflow;
		if (rec.vu1SignOverflow)
			out.vu_clamp |= kClampVu1SignOverflow;
		if (rec.vu1ExactMode)
			out.vu_clamp |= kClampVu1ExactMode;

		const auto encode_fpcr = [](const FPControlRegister& r) -> u32 {
			return (static_cast<u32>(r.GetRoundMode()) & kFpcrRoundMask) |
				   (r.GetFlushToZero() ? kFpcrFlushToZero : 0) |
				   (r.GetDenormalsAreZero() ? kFpcrDenormalsAreZero : 0);
		};
		out.vu0_fpcr = encode_fpcr(EmuConfig.Cpu.VU0FPCR);
		out.vu1_fpcr = encode_fpcr(EmuConfig.Cpu.VU1FPCR);
	}

	void SnapshotState(const VURegs& regs, CapturedState& out)
	{
		for (int i = 0; i < 32; ++i)
		{
			out.VF[i][0] = regs.VF[i].UL[0];
			out.VF[i][1] = regs.VF[i].UL[1];
			out.VF[i][2] = regs.VF[i].UL[2];
			out.VF[i][3] = regs.VF[i].UL[3];
			out.VI[i] = regs.VI[i].UL;
		}
		out.ACC[0] = regs.ACC.UL[0];
		out.ACC[1] = regs.ACC.UL[1];
		out.ACC[2] = regs.ACC.UL[2];
		out.ACC[3] = regs.ACC.UL[3];
		out.q = regs.q.UL;
		out.p = regs.p.UL;
		out.pending_q = regs.pending_q;
		out.pending_p = regs.pending_p;
		std::memcpy(out.micro_macflags, regs.micro_macflags, sizeof(out.micro_macflags));
		std::memcpy(out.micro_clipflags, regs.micro_clipflags, sizeof(out.micro_clipflags));
		std::memcpy(out.micro_statusflags, regs.micro_statusflags, sizeof(out.micro_statusflags));
		out.xgkickaddr = regs.xgkickaddr;
		out.xgkickdiff = regs.xgkickdiff;
		out.xgkicksizeremaining = regs.xgkicksizeremaining;
		out.xgkicklastcycle = regs.xgkicklastcycle;
		out.xgkickcyclecount = regs.xgkickcyclecount;
		out.xgkickenable = regs.xgkickenable;
		out.xgkickendpacket = regs.xgkickendpacket;
	}

	void RestoreState(const CapturedState& state, VURegs& regs)
	{
		for (int i = 0; i < 32; ++i)
		{
			regs.VF[i].UL[0] = state.VF[i][0];
			regs.VF[i].UL[1] = state.VF[i][1];
			regs.VF[i].UL[2] = state.VF[i][2];
			regs.VF[i].UL[3] = state.VF[i][3];
			regs.VI[i].UL = state.VI[i];
		}
		regs.ACC.UL[0] = state.ACC[0];
		regs.ACC.UL[1] = state.ACC[1];
		regs.ACC.UL[2] = state.ACC[2];
		regs.ACC.UL[3] = state.ACC[3];
		regs.q.UL = state.q;
		regs.p.UL = state.p;
		regs.pending_q = state.pending_q;
		regs.pending_p = state.pending_p;
		std::memcpy(regs.micro_macflags, state.micro_macflags, sizeof(state.micro_macflags));
		std::memcpy(regs.micro_clipflags, state.micro_clipflags, sizeof(state.micro_clipflags));
		std::memcpy(regs.micro_statusflags, state.micro_statusflags, sizeof(state.micro_statusflags));

		// The INTERPRETER's live flag accumulators. CapturedState carries only
		// microVU's four-deep shadows above, so without this the interpreter
		// pass of a replay inherits whatever the previous program left in
		// VURegs -- and one field of it is never recomputed, so it survives all
		// the way into the architectural result:
		//
		//   _vuFMACAdd  snapshots VU->statusflag into fmac[i].statusflag
		//   _vuFMACflush ORs (fmac[i].statusflag & 0xFC0) into VI[REG_STATUS_FLAG]
		//
		// 0xFC0 is the STICKY field (ZS/SS/US/OS/IS/DS). Every op recomputes the
		// 0xF cause nibble, but nothing clears the sticky bits except FSSET, so
		// a stale one lands in VI[REG_STATUS_FLAG] as a phantom flag the JIT --
		// which derives its status purely from the restored micro_statusflags --
		// never produces. Measured: with VU0.statusflag = 0x80 carried in, a
		// replay of a VADD of all-positive operands came out
		// `vi16: JIT=0x0 INTERP=0x80`.
		//
		// Seeded from VI rather than zeroed, because that is the exact inverse
		// of the flush above (VI[REG_MAC_FLAG] = fmac[i].macflag, and STATUS
		// takes the sticky field plus the cause nibble). A capture taken with
		// sticky flags already raised therefore replays with them, instead of
		// silently losing them. No format bump: this is derived from VI[], which
		// CapturedState already carries in full.
		regs.statusflag = state.VI[REG_STATUS_FLAG];
		regs.macflag = state.VI[REG_MAC_FLAG];
		regs.clipflag = state.VI[REG_CLIP_FLAG];

		regs.xgkickaddr = state.xgkickaddr;
		regs.xgkickdiff = state.xgkickdiff;
		regs.xgkicksizeremaining = state.xgkicksizeremaining;
		regs.xgkicklastcycle = state.xgkicklastcycle;
		regs.xgkickcyclecount = state.xgkickcyclecount;
		regs.xgkickenable = state.xgkickenable;
		regs.xgkickendpacket = state.xgkickendpacket;
	}

	// VU0's view of VU1's registers at 0x4000 is VF[32] followed by VI[32],
	// each 16 bytes, from the start of VURegs (see _vuGetAddr in VUops.cpp).
	static_assert(offsetof(VURegs, VF) == 0 && sizeof(VECTOR) == 16 &&
		offsetof(VURegs, VI) == 0x200 && sizeof(REG_VI) == 16,
		"CapturedExternal::vu1_regs assumes VF[32] then VI[32] at the start of VURegs");

	void SnapshotExternal(int vu_index, CapturedExternal& out)
	{
		std::memset(&out, 0, sizeof(out));
		out.flags = kExternalValid;

		// The registers XTOP/XITOP read: getVifRegs() in microVU and
		// _vuXTOP/_vuXITOP in the interpreter make the same choice.
		const VIFregisters& vif = vu_index ? (THREAD_VU1 ? vu1Thread.vifRegs : vif1Regs) : vif0Regs;
		out.vif_top = vif.top;
		out.vif_itop = vif.itop;
		out.vif_tops = vif.tops;
		out.vif_itops = vif.itops;
		out.vif_base = vif.base;
		out.vif_ofst = vif.ofst;
		out.vif_stat = vif.stat._u32;

		if (vu_index == 0)
			std::memcpy(out.vu1_regs, &vuRegs[1].VF[0], sizeof(out.vu1_regs));
	}

	void RestoreExternal(int vu_index, const CapturedExternal& ext)
	{
		// Only TOP and ITOP are written back: they are the only VIF registers
		// a microprogram reads, and leaving the rest alone keeps a replay from
		// disturbing VIF state other tests in the same process rely on.
		const bool valid = (ext.flags & kExternalValid) != 0;
		const u32 top = valid ? ext.vif_top : 0;
		const u32 itop = valid ? ext.vif_itop : 0;
		VIFregisters& vif = vu_index ? vif1Regs : vif0Regs;
		vif.top = top;
		vif.itop = itop;
		if (vu_index && THREAD_VU1)
		{
			vu1Thread.vifRegs.top = top;
			vu1Thread.vifRegs.itop = itop;
		}

		if (valid && vu_index == 0)
			std::memcpy(&vuRegs[1].VF[0], ext.vu1_regs, sizeof(ext.vu1_regs));
	}

	u32 ScanExternalReads(const CaptureRecord& rec)
	{
		const u32 size = static_cast<u32>(rec.microcode.size());
		if (size < 8 || (size & (size - 1)) != 0)
			return 0;
		const u32 limit = size - 1;
		const auto read_pair = [&](u32 pc, u32& lo, u32& up) {
			std::memcpy(&lo, rec.microcode.data() + pc, 4);
			std::memcpy(&up, rec.microcode.data() + pc + 4, 4);
		};

		u32 found = 0;
		// Lower-word reads of outside state, skipped when the I bit makes the
		// lower word an immediate. XTOP is a NOP on VU0.
		const auto reads_vif = [&](u32 lo, u32 up) -> u32 {
			if (up & 0x80000000u)
				return 0;
			const u32 t3 = lo & 0xFE0007FFu;
			if (t3 == 0x800006BCu && rec.vu_index == 1)
				return kScanReadsXtop;
			if (t3 == 0x800006BDu)
				return kScanReadsXitop;
			return 0;
		};
		const auto scan_lower = [&](u32 lo, u32 up) { found |= reads_vif(lo, up); };

		std::vector<bool> visited(size / 8, false);
		std::vector<u32> work{rec.start_pc & limit & ~7u};
		while (!work.empty())
		{
			u32 pc = work.back();
			work.pop_back();
			for (;;)
			{
				if (visited[pc / 8])
					break;
				visited[pc / 8] = true;
				u32 lo, up;
				read_pair(pc, lo, up);
				scan_lower(lo, up);

				const u32 op7 = (up & 0x80000000u) ? 0 : (lo >> 25);
				const bool is_branch = op7 == 0x20 || op7 == 0x21 || op7 == 0x24 || op7 == 0x25 ||
					op7 == 0x28 || op7 == 0x29 || (op7 >= 0x2C && op7 <= 0x2F);
				const bool ebit = (up & 0x40000000u) != 0;
				if (!is_branch && !ebit)
				{
					pc = (pc + 8) & limit;
					continue;
				}

				// The pair after a branch or an E-bit is its delay slot and
				// always executes.
				const u32 slot = (pc + 8) & limit;
				u32 slot_lo, slot_up;
				read_pair(slot, slot_lo, slot_up);
				scan_lower(slot_lo, slot_up);
				if (ebit)
					break;

				const u32 after = (pc + 16) & limit;
				s32 imm = static_cast<s32>(lo & 0x7FFu);
				if (imm & 0x400)
					imm -= 0x800;
				const u32 target = static_cast<u32>(static_cast<s32>(pc) + 8 + imm * 8) & limit;
				switch (op7)
				{
					case 0x20: // B
						work.push_back(target);
						break;
					case 0x21: // BAL: the callee usually returns past the delay slot
						work.push_back(target);
						work.push_back(after);
						break;
					case 0x24: // JR
						found |= kScanIndirectJump;
						break;
					case 0x25: // JALR
						found |= kScanIndirectJump;
						work.push_back(after);
						break;
					default: // IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ
						work.push_back(target);
						work.push_back(after);
						break;
				}
				break;
			}
		}

		if (found & kScanIndirectJump)
		{
			for (u32 pc = 0; pc < size; pc += 8)
			{
				if (visited[pc / 8])
					continue;
				u32 lo, up;
				read_pair(pc, lo, up);
				if (reads_vif(lo, up))
				{
					found |= kScanUnreachedRead;
					break;
				}
			}
		}
		return found;
	}

	bool NeedsUnrecordedVifState(const CaptureRecord& rec)
	{
		if (rec.external.flags & kExternalValid)
			return false;
		const u32 scan = ScanExternalReads(rec);
		return (scan & (kScanReadsXtop | kScanReadsXitop | kScanUnreachedRead)) != 0;
	}

	// ---- Capture probe ---------------------------------------------------

	namespace
	{
		std::atomic<bool> g_active{false};
		bool g_capture_active = false;
		bool g_rank_active = false;
		std::string g_dir;
		std::string g_rank_out;
		u32 g_max_per_key = 32;

		// Trajectory mode: ordered per-dispatch log (see header).
		bool g_traj_active = false;
		std::FILE* g_traj_file = nullptr;
		std::mutex g_traj_mutex;
		std::atomic<u64> g_traj_seq{0};

		void CloseTrajAtExit()
		{
			std::lock_guard lock(g_traj_mutex);
			if (g_traj_file)
			{
				std::fclose(g_traj_file);
				g_traj_file = nullptr;
			}
		}

		std::mutex g_state_mutex;
		// Capture-mode: per-key count of executions seen so far. Files are
		// named with seq = slot index in [0, max), reused on replacement.
		std::unordered_map<u64, u32> g_count_seen;
		// Rank-mode: total executions per (vu_index, start_pc).
		std::unordered_map<u64, u64> g_rank_counts;
		std::mt19937_64 g_rng{0x5EEDu ^ static_cast<u64>(capture_pid())};

		void DumpRankReportAtExit()
		{
			std::lock_guard lock(g_state_mutex);
			if (g_rank_counts.empty() || g_rank_out.empty())
				return;
			std::FILE* f = std::fopen(g_rank_out.c_str(), "w");
			if (!f)
			{
				Console.Error("vu_capture: rank dump failed to open %s", g_rank_out.c_str());
				return;
			}

			std::vector<std::pair<u64, u64>> sorted(g_rank_counts.begin(), g_rank_counts.end());
			std::sort(sorted.begin(), sorted.end(),
				[](const auto& a, const auto& b) { return a.second > b.second; });

			std::fprintf(f, "# vu_capture rank report — pid %d\n", capture_pid());
			std::fprintf(f, "# %-3s %-10s %16s\n", "vu", "start_pc", "executions");
			for (const auto& [key, count] : sorted)
			{
				const u32 vu_index = static_cast<u32>(key >> 32);
				const u32 start_pc = static_cast<u32>(key);
				std::fprintf(f, "  %-3u 0x%08X %16llu\n",
					vu_index, start_pc, (unsigned long long)count);
			}
			std::fclose(f);
		}

		void InitFromEnv()
		{
			const char* dir = std::getenv("PCSX2_VU_CAPTURE_DIR");
			const char* rank_out = std::getenv("PCSX2_VU_RANK_OUT");

			if (dir && *dir)
			{
				std::error_code ec;
				std::filesystem::create_directories(dir, ec);
				if (ec)
					Console.Error("vu_capture: failed to create %s: %s",
						dir, ec.message().c_str());
				else
				{
					g_dir = dir;
					g_capture_active = true;
					if (const char* m = std::getenv("PCSX2_VU_CAPTURE_MAX"); m && *m)
					{
						const long parsed = std::strtol(m, nullptr, 10);
						if (parsed > 0 && parsed < (1 << 20))
							g_max_per_key = static_cast<u32>(parsed);
					}
				}
			}

			if (rank_out && *rank_out)
			{
				g_rank_out = rank_out;
				g_rank_active = true;
				std::atexit(&DumpRankReportAtExit);
			}

			if (const char* traj = std::getenv("PCSX2_VU_TRAJ_OUT"); traj && *traj)
			{
				g_traj_file = std::fopen(traj, "w");
				if (!g_traj_file)
					Console.Error("vu_capture: failed to open trajectory file %s", traj);
				else
				{
					g_traj_active = true;
					std::setvbuf(g_traj_file, nullptr, _IOLBF, 0);  // line-buffered
					std::fprintf(g_traj_file,
						"# vu_capture trajectory — pid %d\n"
						"# seq vu pc budget cpu_cycle vu_cycle state_hash vumem_hash core_hash obs_hash\n",
						capture_pid());
					std::atexit(&CloseTrajAtExit);
				}
			}

			if (g_capture_active || g_rank_active || g_traj_active)
			{
				g_active.store(true, std::memory_order_relaxed);
				Console.WriteLn("vu_capture: capture=%s rank=%s traj=%s",
					g_capture_active ? g_dir.c_str() : "off",
					g_rank_active ? g_rank_out.c_str() : "off",
					g_traj_active ? "on" : "off");
			}
		}

		std::string MakeSlotPath(int vu_index, u32 start_pc, u32 seq)
		{
			char buf[256];
			std::snprintf(buf, sizeof(buf), "%s/vu%d_pc%08X_seq%03u.vucap",
				g_dir.c_str(), vu_index, start_pc, seq);
			return std::string(buf);
		}
	} // namespace

	void MaybeCapture(int vu_index, u32 start_pc, u32 cycle_budget,
		const u8* microcode_ptr, u32 microcode_size,
		const u8* vumem_ptr, u32 vumem_size,
		const VURegs& regs)
	{
		static std::once_flag init_once;
		std::call_once(init_once, &InitFromEnv);

		if (!g_active.load(std::memory_order_relaxed)) [[likely]]
			return;

		// Trajectory mode: one ordered line per dispatch, EVERY call (no
		// reservoir). Hash the architectural surface + VU memory so two runs
		// from the same save-state are line-diffable (see header). cpu_cycle is
		// the EE clock at dispatch — a matching state_hash with a drifting
		// cpu_cycle isolates a timing wedge from a carried-state divergence.
		if (g_traj_active)
		{
			CapturedState st{};
			SnapshotState(regs, st);
			const u64 state_hash = Fnv1a(&st, sizeof(st));
			// obs_hash: state with the sticky STATUS bits the exactness contract frees (relaxation 3)
			// masked out: bits 6-7 and 10-11 of the register, the low byte and bits 24-25 of its
			// ring copies.
			CapturedState obs = st;
			obs.VI[REG_STATUS_FLAG] &= ~0xCC0u;
			for (u32& w : obs.micro_statusflags)
				w &= ~0x30000FFu;
			const u64 obs_hash = Fnv1a(&obs, sizeof(obs));
			const u64 vumem_hash = Fnv1a(vumem_ptr, vumem_size);
			// core_hash covers ONLY pure arithmetic/control state: VF[32] +
			// integer VI[0..15] + ACC. It deliberately excludes the cycle-timed
			// fields (Q/P pipeline results in VI[16..31], pending_q/p, the flag
			// pipelines, and all xgkick* incl. the xgkicklastcycle timestamp),
			// which differ between two runs purely from the VU cycle-count model
			// (the -3 JIT-vs-interp gap) even when the arithmetic is identical.
			// A core_hash divergence at matched cpu_cycle is therefore a REAL
			// arithmetic/control divergence, not a cycle-model artifact.
			u64 core = Fnv1a(st.VF, sizeof(st.VF));          // VF[32][4]
			core = Fnv1a(st.VI, 16 * sizeof(st.VI[0]), core); // VI[0..15] (int regs)
			core = Fnv1a(st.ACC, sizeof(st.ACC), core);       // ACC
			// PCSX2_VU_TRAJ_DUMP_HASH=<hex>[,<hex>...]: write VU data memory to
			// $PCSX2_VU_TRAJ_OUT.<hex>.mem the first time a dispatch's state_hash
			// matches, to diff two runs whose vumem_hash differs.
			static const std::vector<u64> dump_hashes = [] {
				std::vector<u64> v;
				if (const char* e = std::getenv("PCSX2_VU_TRAJ_DUMP_HASH"))
					for (const char* p = e; *p;)
					{
						char* end = nullptr;
						v.push_back(std::strtoull(p, &end, 16));
						p = (*end == ',') ? end + 1 : end;
						if (end == p && *p)
							break;
					}
				return v;
			}();
			if (!dump_hashes.empty() && vu_index == 1)
			{
				for (const u64 h : dump_hashes)
				{
					if (h != state_hash)
						continue;
					const char* base = std::getenv("PCSX2_VU_TRAJ_OUT");
					char path[512];
					std::snprintf(path, sizeof(path), "%s.%016llx.mem", base ? base : "traj", (unsigned long long)h);
					if (std::FILE* f = std::fopen(path, "wbx"))
					{
						std::fwrite(vumem_ptr, 1, vumem_size, f);
						std::fclose(f);
					}
				}
			}
			const u64 seq = g_traj_seq.fetch_add(1, std::memory_order_relaxed);
			std::lock_guard lock(g_traj_mutex);
			if (g_traj_file)
				std::fprintf(g_traj_file, "%llu %d 0x%08X %u %llu %llu %016llx %016llx %016llx %016llx\n",
					(unsigned long long)seq, vu_index, start_pc, cycle_budget,
					(unsigned long long)cpuRegs.cycle, (unsigned long long)regs.cycle,
					(unsigned long long)state_hash, (unsigned long long)vumem_hash,
					(unsigned long long)core, (unsigned long long)obs_hash);
		}

		// Decide slot under the state lock; do the heavy I/O after releasing
		// it so concurrent VU0 / VU1 captures don't serialize on the file
		// write. (WriteToFile takes its own writer mutex internally.)
		const u64 key = (static_cast<u64>(vu_index) << 32) | start_pc;
		u32 slot = 0;
		bool write_this = false;
		{
			std::lock_guard lock(g_state_mutex);
			if (g_rank_active)
				++g_rank_counts[key];
			if (g_capture_active)
			{
				u32& seen = g_count_seen[key];
				if (seen < g_max_per_key)
				{
					slot = seen;
					write_this = true;
				}
				else
				{
					// Standard reservoir replacement: pick j uniformly in
					// [0, seen+1); if j < g_max_per_key, replace slot j.
					std::uniform_int_distribution<u64> dist(0, seen);
					const u64 j = dist(g_rng);
					if (j < g_max_per_key)
					{
						slot = static_cast<u32>(j);
						write_this = true;
					}
				}
				++seen;
			}
		}

		if (!write_this)
			return;

		CaptureRecord rec;
		rec.vu_index = static_cast<u8>(vu_index);
		rec.start_pc = start_pc;
		rec.cycle_budget = cycle_budget;
		rec.microcode.assign(microcode_ptr, microcode_ptr + microcode_size);
		rec.vumem.assign(vumem_ptr, vumem_ptr + vumem_size);
		SnapshotState(regs, rec.state);
		SnapshotConfig(rec.config);
		SnapshotExternal(vu_index, rec.external);

		const std::string path = MakeSlotPath(vu_index, start_pc, slot);
		if (!WriteToFile(path, rec))
			Console.Error("vu_capture: write failed: %s", path.c_str());
	}

} // namespace vu_capture

#endif // PCSX2_RECOMPILER_TESTS
