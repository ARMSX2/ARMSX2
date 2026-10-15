// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#include "GSVramMirrorSelfTest.h"

#ifdef ENABLE_VULKAN

#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"
#include "GS/Renderers/Vulkan/GSVramMirrorVK.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using GSVramMirror::Format;

namespace
{
#define ST_PRINT(...) \
	do \
	{ \
		std::printf("GPU-DECODE-SELFTEST " __VA_ARGS__); \
		std::printf("\n"); \
		std::fflush(stdout); \
	} while (0)

	class Rng
	{
	public:
		explicit Rng(u64 seed)
			: m_s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull)
		{
			Next();
		}
		u64 Next()
		{
			m_s ^= m_s >> 12;
			m_s ^= m_s << 25;
			m_s ^= m_s >> 27;
			return m_s * 0x2545F4914F6CDD1Dull;
		}
		u32 Pick(u32 n) { return n ? static_cast<u32>((Next() >> 33) % n) : 0; }
		u32 Range(u32 lo, u32 hi) { return lo + Pick(hi - lo + 1); }

	private:
		u64 m_s;
	};

	u64 g_next_memory_id = 1;

	/// A GSLocalMemory as the mirror sees it: bytes, an id, and a block-dirty bitmap. The writers below
	/// stand in for what the page marks will do: every change sets the changed blocks' bits.
	struct Memory
	{
		Memory(Rng& rng, bool all_dirty)
			: mem(std::make_unique<GSLocalMemory>())
			, id(g_next_memory_id++)
			, dirty(GSVramMirror::kMirrorBlocks / 64, all_dirty ? ~0ull : 0ull)
		{
			u64* p = reinterpret_cast<u64*>(mem->m_vm8);
			for (u32 i = 0; i < GSLocalMemory::m_vmsize / 8; i++)
				p[i] = rng.Next();
		}

		GSVramMirrorVK::Source Source() { return {mem->m_vm8, id, dirty.data()}; }
		bool IsDirty(u32 block) const { return (dirty[block >> 6] >> (block & 63)) & 1; }
		void MarkDirty(u32 block) { dirty[block >> 6] |= 1ull << (block & 63); }

		/// One random write. Marks the blocks it touches unless `mark` is false (the control case).
		void Mutate(Rng& rng, bool mark = true)
		{
			u32 first = rng.Pick(GSVramMirror::kMirrorBlocks), count = 1;
			switch (rng.Pick(4))
			{
				case 0: // a few bytes inside one block: the whole block must restage
				{
					const u32 n = 1 + rng.Pick(8);
					for (u32 i = 0; i < n; i++)
						mem->m_vm8[first * 256 + rng.Pick(256)] = static_cast<u8>(rng.Next());
					break;
				}
				case 1: // one whole block
					FillBlocks(rng, first, 1);
					break;
				case 2: // a run of blocks
					count = rng.Range(2, 40);
					FillBlocks(rng, first, count);
					break;
				default: // a whole page
					first &= ~31u;
					count = 32;
					FillBlocks(rng, first, count);
					break;
			}
			if (mark)
				for (u32 i = 0; i < count; i++)
					MarkDirty((first + i) % GSVramMirror::kMirrorBlocks);
		}

		void FillBlocks(Rng& rng, u32 first, u32 count)
		{
			for (u32 i = 0; i < count; i++)
			{
				u64* p = reinterpret_cast<u64*>(mem->m_vm8 + static_cast<size_t>((first + i) % GSVramMirror::kMirrorBlocks) * 256);
				for (u32 k = 0; k < 32; k++)
					p[k] = rng.Next();
			}
		}

		std::unique_ptr<GSLocalMemory> mem;
		u64 id;
		std::vector<u64> dirty;
	};

	struct WindowSpec
	{
		u32 psm;
		u32 tbp;
		u32 tbw;
		GSVector4i rect;
	};

	constexpr u32 kPsms[GSVramMirror::kFormatCount] = {PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH};
	const char* const kPsmNames[GSVramMirror::kFormatCount] = {"PSMT8", "PSMT4", "PSMT8H", "PSMT4HL", "PSMT4HH"};

	const char* PsmName(u32 psm)
	{
		for (u32 i = 0; i < GSVramMirror::kFormatCount; i++)
			if (kPsms[i] == psm)
				return kPsmNames[i];
		return "?";
	}

	/// A window with any TBP0 (the wrap at 16384 blocks included), any TBW including odd ones, a start
	/// inside a block, a run across pages, and sometimes a right edge past 2048. The H formats get an
	/// odd number of blocks across about half the time.
	WindowSpec RandomWindow(Rng& rng, Format fmt, Memory& m)
	{
		WindowSpec s;
		s.psm = kPsms[static_cast<u32>(fmt)];
		s.tbp = (rng.Pick(5) == 0) ? GSVramMirror::kMirrorBlocks - 1 - rng.Pick(70) : rng.Pick(GSVramMirror::kMirrorBlocks);
		s.tbw = 1 + rng.Pick(32);

		const u32 r = rng.Pick(100);
		const int x0 = r < 55 ? static_cast<int>(rng.Pick(300)) : (r < 75 ? 2047 - static_cast<int>(rng.Pick(120)) : static_cast<int>(rng.Pick(2048)));
		const int y0 = rng.Pick(100) < 70 ? static_cast<int>(rng.Pick(300)) : static_cast<int>(rng.Pick(2048));
		const bool big = rng.Pick(20) == 0;
		int w = 1 + static_cast<int>(rng.Pick(big ? 520 : 260));
		const int h = 1 + static_cast<int>(rng.Pick(big ? 300 : 140));

		const bool h_format = (fmt == Format::T8H || fmt == Format::T4HL || fmt == Format::T4HH);
		if (h_format && rng.Pick(2) == 0)
		{
			for (int tries = 0; tries < 24; tries++)
			{
				GSVramMirror::WindowLayout l;
				const GSOffset off = m.mem->GetOffset(s.tbp, s.tbw, s.psm);
				if (GSVramMirror::ComputeLayout(off, GSVector4i(x0, y0, x0 + w, y0 + h), &l) && (l.blocks_wide & 1))
					break;
				w++;
			}
		}
		s.rect = GSVector4i(x0, y0, x0 + w, y0 + h);
		return s;
	}

	/// What the CPU readers give for the window, exactly as PreloadTexture builds a paletted texture:
	/// rtxP over the block-aligned rect, then the crop. One byte per texel, rect.width() x rect.height().
	std::vector<u8> CpuDecode(Memory& m, const WindowSpec& s)
	{
		const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[s.psm];
		const GSOffset off = m.mem->GetOffset(s.tbp, s.tbw, s.psm);
		const GSVector4i block_rect = s.rect.ralign<Align_Outside>(psm.bs);
		const u32 pitch = (static_cast<u32>(block_rect.width()) + 31u) & ~31u;
		std::vector<u8> raw(static_cast<size_t>(pitch) * block_rect.height() + 64);
		u8* const aligned = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(raw.data()) + 31) & ~uintptr_t(31));
		const GIFRegTEXA texa = {};
		psm.rtxP(*m.mem, off, block_rect, aligned, static_cast<int>(pitch), texa);

		const int w = s.rect.width(), h = s.rect.height();
		std::vector<u8> out(static_cast<size_t>(w) * h);
		for (int y = 0; y < h; y++)
			std::memcpy(&out[static_cast<size_t>(y) * w],
				aligned + static_cast<size_t>(pitch) * (s.rect.top - block_rect.top + y) + (s.rect.left - block_rect.left), w);
		return out;
	}

	struct Pending
	{
		WindowSpec spec;
		GSTextureVK* texture;
		std::vector<u8> expected;
	};

	struct Totals
	{
		u64 windows = 0;
		u64 texels = 0;
		u64 bad_windows = 0;
		u64 bad_texels = 0;
		u64 batches = 0;
		u64 need_submit_retries = 0;
		u64 refused = 0; ///< DecodeWindow said Unsupported (or NeedSubmit twice) for a window that should fit
		u64 mirror_checked = 0;
		u64 mirror_bad = 0;

		void Add(const Totals& o)
		{
			windows += o.windows;
			texels += o.texels;
			bad_windows += o.bad_windows;
			bad_texels += o.bad_texels;
			batches += o.batches;
			need_submit_retries += o.need_submit_retries;
			refused += o.refused;
			mirror_checked += o.mirror_checked;
			mirror_bad += o.mirror_bad;
		}
		bool Clean() const { return bad_windows == 0 && bad_texels == 0 && refused == 0 && mirror_bad == 0; }
	};

	class Test
	{
	public:
		Test(GSDeviceVK* dev, const GSVramMirrorSelfTest::Options& opt)
			: m_dev(dev)
			, m_opt(opt)
			, m_rng(opt.seed)
		{
		}

		int Run();

	private:
		GSTextureVK* NewTexture(const WindowSpec& s)
		{
			return static_cast<GSTextureVK*>(m_dev->CreateTexture(s.rect.width(), s.rect.height(), 1, GSTexture::Format::UNorm8));
		}

		/// Queues a window on `mirror` against `mem`, remembering what the CPU decodes right now. A
		/// NeedSubmit is answered the way the texture cache will: submit the batch, then retry.
		bool Queue(GSVramMirrorVK& mirror, Memory& mem, const WindowSpec& s, std::vector<Pending>& batch, Totals& t)
		{
			Pending p;
			p.spec = s;
			p.texture = NewTexture(s);
			p.expected = CpuDecode(mem, s);
			const GSOffset off = mem.mem->GetOffset(s.tbp, s.tbw, s.psm);
			GSVramMirrorVK::Request rq{&off, s.rect, p.texture, mem.Source()};
			GSVramMirrorVK::Result r = mirror.DecodeWindow(rq);
			if (r == GSVramMirrorVK::Result::NeedSubmit)
			{
				t.need_submit_retries++;
				Check(mirror, batch, t);
				r = mirror.DecodeWindow(rq);
			}
			if (r != GSVramMirrorVK::Result::Queued)
			{
				t.refused++;
				m_dev->Recycle(p.texture);
				return false;
			}
			batch.push_back(std::move(p));
			return true;
		}

		/// Flushes the batch, reads every texture back and compares it with the CPU decode taken when the
		/// window was queued. Empties the batch and returns the textures to the pool.
		void Check(GSVramMirrorVK& mirror, std::vector<Pending>& batch, Totals& t)
		{
			mirror.Flush();
			if (batch.empty())
			{
				m_dev->ExecuteCommandBuffer(true);
				return;
			}

			std::vector<std::unique_ptr<GSDownloadTexture>> downloads;
			for (Pending& p : batch)
			{
				const GSVector4i rc(0, 0, p.spec.rect.width(), p.spec.rect.height());
				downloads.push_back(m_dev->CreateDownloadTexture(rc.width(), rc.height(), GSTexture::Format::UNorm8));
				downloads.back()->CopyFromTexture(rc, p.texture, rc, 0, true);
			}
			for (auto& d : downloads)
				d->Flush();

			for (size_t i = 0; i < batch.size(); i++)
			{
				Pending& p = batch[i];
				const GSVector4i rc(0, 0, p.spec.rect.width(), p.spec.rect.height());
				downloads[i]->Map(rc);
				const u8* got = downloads[i]->GetMapPointer();
				const u32 pitch = downloads[i]->GetMapPitch();
				const int w = rc.width(), h = rc.height();
				u64 bad = 0;
				int first_x = -1, first_y = -1;
				for (int y = 0; y < h; y++)
				{
					for (int x = 0; x < w; x++)
					{
						if (got[static_cast<size_t>(y) * pitch + x] != p.expected[static_cast<size_t>(y) * w + x])
						{
							if (!bad)
							{
								first_x = x;
								first_y = y;
							}
							bad++;
						}
					}
				}
				t.windows++;
				t.texels += static_cast<u64>(w) * h;
				if (bad)
				{
					t.bad_windows++;
					t.bad_texels += bad;
					if (!m_quiet && m_printed++ < 12)
					{
						ST_PRINT("MISMATCH %s tbp=%u tbw=%u rect=(%d,%d,%d,%d): %llu of %llu texels differ, first at (%d,%d): gpu=%u cpu=%u",
							PsmName(p.spec.psm), p.spec.tbp, p.spec.tbw, p.spec.rect.left, p.spec.rect.top, p.spec.rect.right,
							p.spec.rect.bottom, static_cast<unsigned long long>(bad), static_cast<unsigned long long>(static_cast<u64>(w) * h),
							first_x, first_y, got[static_cast<size_t>(first_y) * pitch + first_x],
							p.expected[static_cast<size_t>(first_y) * w + first_x]);
					}
				}
				downloads[i]->Unmap();
			}
			t.batches++;
			for (Pending& p : batch)
				m_dev->Recycle(p.texture);
			batch.clear();
		}

		/// Every block whose dirty bit is clear must be what local memory holds: the mirror's own copy,
		/// read back. Catches a scatter that wrote the wrong version, and a block that went clean without
		/// ever being staged. Submits, so call it with no window pending.
		void VerifyMirror(GSVramMirrorVK& mirror, Memory& mem, Totals& t)
		{
			std::vector<u8> m;
			if (!mirror.DebugReadMirror(&m))
			{
				ST_PRINT("mirror readback failed");
				t.mirror_bad++;
				return;
			}
			for (u32 b = 0; b < GSVramMirror::kMirrorBlocks; b++)
			{
				if (mem.IsDirty(b))
					continue;
				t.mirror_checked++;
				if (std::memcmp(&m[static_cast<size_t>(b) * 256], mem.mem->m_vm8 + static_cast<size_t>(b) * 256, 256) != 0)
				{
					if (t.mirror_bad++ < 4)
						ST_PRINT("MIRROR MISMATCH block %u is clean but differs from local memory", b);
				}
			}
		}

		void Report(const char* phase, const Totals& t, const GSVramMirrorVK::Stats* delta = nullptr)
		{
			ST_PRINT("%s: windows=%llu texels=%llu batches=%llu mismatched_windows=%llu mismatched_texels=%llu refused=%llu "
					 "submit_retries=%llu mirror_blocks_checked=%llu mirror_mismatches=%llu",
				phase, static_cast<unsigned long long>(t.windows), static_cast<unsigned long long>(t.texels),
				static_cast<unsigned long long>(t.batches), static_cast<unsigned long long>(t.bad_windows),
				static_cast<unsigned long long>(t.bad_texels), static_cast<unsigned long long>(t.refused),
				static_cast<unsigned long long>(t.need_submit_retries), static_cast<unsigned long long>(t.mirror_checked),
				static_cast<unsigned long long>(t.mirror_bad));
			if (delta)
			{
				ST_PRINT("%s: table_blocks=%llu from_staging=%llu staged=%llu restaged_in_batch=%llu mixed_windows=%llu flushes=%llu "
						 "scatter_blocks=%llu scratch_flushes=%llu resyncs=%llu",
					phase, static_cast<unsigned long long>(delta->blocks_total), static_cast<unsigned long long>(delta->blocks_from_staging),
					static_cast<unsigned long long>(delta->blocks_staged), static_cast<unsigned long long>(delta->blocks_restaged),
					static_cast<unsigned long long>(delta->mixed_windows), static_cast<unsigned long long>(delta->flushes),
					static_cast<unsigned long long>(delta->scatter_blocks), static_cast<unsigned long long>(delta->scratch_flushes),
					static_cast<unsigned long long>(delta->resyncs));
			}
			m_all.Add(t);
			if (!t.Clean())
				m_ok = false;
		}

		static GSVramMirrorVK::Stats Diff(const GSVramMirrorVK::Stats& b, const GSVramMirrorVK::Stats& a)
		{
			GSVramMirrorVK::Stats d;
			d.windows = b.windows - a.windows;
			d.blocks_total = b.blocks_total - a.blocks_total;
			d.blocks_from_staging = b.blocks_from_staging - a.blocks_from_staging;
			d.blocks_staged = b.blocks_staged - a.blocks_staged;
			d.blocks_restaged = b.blocks_restaged - a.blocks_restaged;
			d.mixed_windows = b.mixed_windows - a.mixed_windows;
			d.flushes = b.flushes - a.flushes;
			d.scatter_blocks = b.scatter_blocks - a.scatter_blocks;
			d.resyncs = b.resyncs - a.resyncs;
			d.scratch_flushes = b.scratch_flushes - a.scratch_flushes;
			d.need_submit = b.need_submit - a.need_submit;
			return d;
		}

		void PhaseFuzz();
		void PhaseControl();
		void PhaseSync();
		void PhasePooled();
		void PhaseCancel();
		void PhaseHooks();
		void PhaseLimits();
		void PhaseTiming();

		GSDeviceVK* m_dev;
		GSVramMirrorSelfTest::Options m_opt;
		Rng m_rng;
		Totals m_all;
		bool m_ok = true;
		u32 m_printed = 0;
		bool m_quiet = false; ///< the control phase mismatches on purpose
	};

	/// The device-owned mirror, every paletted format, random memory and windows, a write between many of
	/// them. Blocks the batch staged and blocks the mirror already held land in the same windows, a block
	/// rewritten after an earlier window staged it gets a new slot while that window keeps the old one,
	/// and every batch is followed by a CPU compare of each texture.
	void Test::PhaseFuzz()
	{
		GSVramMirrorVK& mirror = *m_dev->GetVramMirror();
		Memory mem(m_rng, /*all_dirty=*/true);
		for (u32 f = 0; f < GSVramMirror::kFormatCount; f++)
		{
			Totals t;
			const GSVramMirrorVK::Stats before = mirror.GetStats();
			std::vector<Pending> batch;
			u32 done = 0;
			while (done < m_opt.windows_per_format)
			{
				const u32 k = std::min(m_opt.windows_per_format - done, 1 + m_rng.Pick(40));
				for (u32 i = 0; i < k; i++)
				{
					if (m_rng.Pick(100) < 35)
						mem.Mutate(m_rng); // a write between two windows of one batch
					Queue(mirror, mem, RandomWindow(m_rng, static_cast<Format>(f), mem), batch, t);
				}
				Check(mirror, batch, t);
				done += k;
				if (m_rng.Pick(100) < 40)
					mem.Mutate(m_rng); // and between batches
				if (m_rng.Pick(10) == 0)
					VerifyMirror(mirror, mem, t);
			}
			VerifyMirror(mirror, mem, t);
			char name[48];
			std::snprintf(name, sizeof(name), "fuzz %s", kPsmNames[f]);
			const GSVramMirrorVK::Stats delta = Diff(mirror.GetStats(), before);
			Report(name, t, &delta);
		}
	}

	/// The compare must be able to fail. A write that is NOT marked leaves the mirror stale, and the next
	/// decode of a window over that block has to disagree with the CPU. Separate mirror: it is left corrupt.
	void Test::PhaseControl()
	{
		std::unique_ptr<GSVramMirrorVK> mirror = GSVramMirrorVK::Create();
		if (!mirror)
		{
			m_ok = false;
			ST_PRINT("control: mirror creation failed");
			return;
		}
		Memory mem(m_rng, true);
		Totals warm, control;
		const u32 trials = 20;
		m_quiet = true;
		for (u32 i = 0; i < trials; i++)
		{
			WindowSpec s = RandomWindow(m_rng, static_cast<Format>(i % 2), mem); // PSMT8 / PSMT4
			const GSOffset off = mem.mem->GetOffset(s.tbp, s.tbw, s.psm);
			GSVramMirror::WindowLayout l;
			GSVramMirror::ComputeLayout(off, s.rect, &l);
			s.rect = l.block_rect; // every block fully inside the window, so a changed block shows
			std::vector<Pending> batch;
			Queue(*mirror, mem, s, batch, warm);
			Check(*mirror, batch, warm); // the mirror now holds these blocks, bitmap clean

			std::vector<u32> blocks;
			GSVramMirror::BuildBlockNumbers(off, l, &blocks);
			const u32 stale = blocks[m_rng.Pick(static_cast<u32>(blocks.size()))];
			mem.FillBlocks(m_rng, stale, 1); // NOT marked

			Queue(*mirror, mem, s, batch, control);
			Check(*mirror, batch, control);
			mem.MarkDirty(stale); // repair, so the next trial's warm-up starts from a consistent mirror
		}
		m_quiet = false;
		ST_PRINT("control: %llu of %u unmarked writes were caught by the compare (the warm-up windows had %llu mismatches)",
			static_cast<unsigned long long>(control.bad_windows), trials, static_cast<unsigned long long>(warm.bad_windows));
		if (control.bad_windows != trials || warm.bad_windows != 0)
		{
			m_ok = false;
			ST_PRINT("control: FAIL, the compare does not detect a stale block");
		}
	}

	/// R2: what ties the mirror to the memory it describes. A new mirror over a live memory whose bitmap
	/// is mostly clean (a device lost, the renderer kept), a different memory with an unknown id and a
	/// CLEAN bitmap, and back again.
	void Test::PhaseSync()
	{
		Memory a(m_rng, true);
		Memory b(m_rng, /*all_dirty=*/false); // a memory the mirror has never seen, whose bitmap claims it has
		std::unique_ptr<GSVramMirrorVK> m1 = GSVramMirrorVK::Create();
		if (!m1)
		{
			m_ok = false;
			return;
		}

		const auto round = [&](GSVramMirrorVK& mirror, Memory& mem, const char* what, u64 expect_resyncs) {
			Totals t;
			const GSVramMirrorVK::Stats before = mirror.GetStats();
			std::vector<Pending> batch;
			for (u32 i = 0; i < 60; i++)
			{
				if (i % 5 == 0)
					mem.Mutate(m_rng);
				Queue(mirror, mem, RandomWindow(m_rng, static_cast<Format>(i % GSVramMirror::kFormatCount), mem), batch, t);
				if (i % 12 == 11)
					Check(mirror, batch, t);
			}
			Check(mirror, batch, t);
			VerifyMirror(mirror, mem, t);
			const u64 resyncs = mirror.GetStats().resyncs - before.resyncs;
			char name[64];
			std::snprintf(name, sizeof(name), "sync %s", what);
			Report(name, t);
			ST_PRINT("sync %s: resyncs in this round=%llu expected=%llu", what, static_cast<unsigned long long>(resyncs),
				static_cast<unsigned long long>(expect_resyncs));
			if (resyncs != expect_resyncs)
				m_ok = false;
		};

		round(*m1, a, "first use of a memory", 1);
		round(*m1, a, "same memory again (no resync)", 0);

		// The device is lost and reopened with the renderer kept: a new mirror, the same memory, a bitmap
		// the old mirror had cleared.
		std::unique_ptr<GSVramMirrorVK> m2 = GSVramMirrorVK::Create();
		m1.reset();
		if (!m2)
		{
			m_ok = false;
			return;
		}
		round(*m2, a, "new mirror over the live memory", 1);
		round(*m2, b, "another memory with a clean bitmap", 1);
		round(*m2, a, "back to the first memory", 1);
		m2->ForgetMemory();
		round(*m2, a, "after ForgetMemory", 1);
	}

	/// R3: a pooled texture that an earlier submission sampled, recycled and decoded into by the next
	/// one. The earlier submission is not waited for.
	void Test::PhasePooled()
	{
		std::unique_ptr<GSVramMirrorVK> mirror = GSVramMirrorVK::Create();
		if (!mirror)
		{
			m_ok = false;
			return;
		}
		Memory mem(m_rng, true);
		// Big enough that sampling every texture a few times keeps the GPU busy for milliseconds: the next
		// submission has to be recorded and submitted while this one is still running, or there is no
		// overlap for a missing barrier to lose.
		GSTexture* rt = m_dev->CreateRenderTarget(2048, 2048, GSTexture::Format::Color, true, false);
		Totals t;
		std::unordered_set<VkImage> seen; // by image: a freed texture's address is reused by the next allocation
		u64 reused = 0;
		for (u32 round = 0; round < 12; round++)
		{
			std::vector<Pending> batch;
			for (u32 i = 0; i < 8; i++)
			{
				if (i % 3 == 0)
					mem.Mutate(m_rng);
				WindowSpec s = RandomWindow(m_rng, Format::T8, mem);
				s.rect = GSVector4i(s.rect.left, s.rect.top, s.rect.left + 64, s.rect.top + 64); // one size, so the pool hands them back
				if (Queue(*mirror, mem, s, batch, t) && !seen.insert(batch.back().texture->GetImage()).second)
					reused++;
			}
			mirror->Flush();
			// Sampled by fragment shaders of this submission, which is not waited for.
			for (Pending& p : batch)
				for (u32 pass = 0; pass < 10; pass++)
					m_dev->StretchRect(p.texture, rt, ShaderConvertSelector(ShaderConvert::COPY), Filter::Nearest);
			m_dev->ExecuteCommandBuffer(false);
			// Straight back into the pool, and a frame boundary, so the pool hands them out again: they are
			// decoded into by the next batch's flush while this submission may still be sampling them.
			for (Pending& p : batch)
				m_dev->Recycle(p.texture);
			batch.clear();
			m_dev->AgePool();
		}
		// One last batch that is compared, onto textures that were sampled by the submission before it.
		std::vector<Pending> batch;
		for (u32 i = 0; i < 8; i++)
		{
			WindowSpec s = RandomWindow(m_rng, Format::T8, mem);
			s.rect = GSVector4i(s.rect.left, s.rect.top, s.rect.left + 64, s.rect.top + 64);
			if (Queue(*mirror, mem, s, batch, t) && !seen.insert(batch.back().texture->GetImage()).second)
				reused++;
		}
		Check(*mirror, batch, t);
		m_dev->Recycle(rt);
		VerifyMirror(*mirror, mem, t);
		Report("pooled textures sampled by the previous submission", t);
		ST_PRINT("pooled: %llu texture handouts were an image the pool had taken back", static_cast<unsigned long long>(reused));
		if (reused == 0)
		{
			ST_PRINT("pooled: FAIL, the pool never handed a sampled texture back, so the case did not run");
			m_ok = false;
		}
	}

	/// R12: a pending texture is destroyed before the flush. Its copy is dropped, but the blocks its
	/// window staged still reach the mirror, because their dirty bits were cleared at miss time.
	void Test::PhaseCancel()
	{
		std::unique_ptr<GSVramMirrorVK> mirror = GSVramMirrorVK::Create();
		if (!mirror)
		{
			m_ok = false;
			return;
		}
		Memory mem(m_rng, true);
		Totals t;
		std::vector<Pending> batch;
		for (u32 i = 0; i < 10; i++)
		{
			WindowSpec s = RandomWindow(m_rng, Format::T8, mem);
			if (!Queue(*mirror, mem, s, batch, t))
				continue;
			if (i % 2 == 0)
			{
				// Destroyed before it was decoded: no compare, no copy.
				mirror->CancelTexture(batch.back().texture);
				m_dev->Recycle(batch.back().texture);
				batch.pop_back();
			}
		}
		Check(*mirror, batch, t);
		VerifyMirror(*mirror, mem, t);
		// The cancelled windows' blocks are clean now; decoding them again must read them from the mirror.
		for (u32 i = 0; i < 20; i++)
			Queue(*mirror, mem, RandomWindow(m_rng, static_cast<Format>(i % GSVramMirror::kFormatCount), mem), batch, t);
		Check(*mirror, batch, t);
		VerifyMirror(*mirror, mem, t);
		Report("cancel (a pending texture destroyed before the flush)", t);
		ST_PRINT("cancel: cancelled=%llu", static_cast<unsigned long long>(mirror->GetStats().cancelled));
		if (mirror->GetStats().cancelled == 0)
			m_ok = false;
	}

	/// The hooks that keep a texture with a queued decode from being touched before the decode has run,
	/// on the device-owned mirror (the one the texture cache uses): the submit flushes the batch, a CPU
	/// upload into a pending texture flushes first and then lands on top of the decode, a pending texture
	/// going back to the pool is flushed, destroying one cancels its decode and keeps its blocks, and a
	/// texture already used by the command buffer being built, or already queued, is refused.
	void Test::PhaseHooks()
	{
		GSVramMirrorVK* mirror_ptr = m_dev->GetVramMirror();
		if (!mirror_ptr)
		{
			m_ok = false;
			ST_PRINT("hooks: the device has no mirror");
			return;
		}
		GSVramMirrorVK& mirror = *mirror_ptr;
		Memory mem(m_rng, true);
		Totals t;
		std::vector<Pending> batch;
		u32 checks = 0, failed = 0;
		const auto expect = [&](bool ok, const char* what) {
			checks++;
			if (!ok)
			{
				failed++;
				ST_PRINT("hooks: FAIL: %s", what);
			}
		};
		const auto window = [&]() {
			WindowSpec s = RandomWindow(m_rng, Format::T8, mem);
			s.rect = GSVector4i(s.rect.left, s.rect.top, s.rect.left + 48, s.rect.top + 40);
			return s;
		};

		// (a) SubmitCommandBuffer flushes the batch on its way: nothing calls Flush here.
		{
			Queue(mirror, mem, window(), batch, t);
			GSTextureVK* tex = batch.back().texture;
			expect(tex->IsDecodePending(), "a queued texture is pending");
			expect(mirror.HasPending(), "the batch has the decode");
			m_dev->ExecuteCommandBuffer(true);
			expect(!tex->IsDecodePending(), "the submit flushed the batch");
			expect(!mirror.HasPending(), "nothing is left in the batch after the submit");
			Check(mirror, batch, t);
		}

		// (b) A CPU upload into a pending texture goes after the decode, not before it: the texture ends up
		// holding the uploaded bytes. (If the decode were recorded after the upload it would hold the decode.)
		{
			const WindowSpec s = window();
			Queue(mirror, mem, s, batch, t);
			Pending& p = batch.back();
			std::vector<u8> cpu(static_cast<size_t>(s.rect.width()) * s.rect.height());
			for (size_t i = 0; i < cpu.size(); i++)
				cpu[i] = static_cast<u8>(0x40 + (i * 7) % 0x90);
			const u64 flushes = mirror.GetStats().flushes;
			p.texture->Update(GSVector4i(0, 0, s.rect.width(), s.rect.height()), cpu.data(), s.rect.width());
			expect(!p.texture->IsDecodePending(), "the upload flushed the decode first");
			expect(mirror.GetStats().flushes == flushes + 1, "exactly one flush for the upload");
			p.expected = cpu;
			Check(mirror, batch, t);
		}

		// (c) A pending texture going back to the pool is flushed, and is the decoded texture when the pool
		// hands it out again.
		{
			const WindowSpec s = window();
			Queue(mirror, mem, s, batch, t);
			Pending p = std::move(batch.back());
			batch.pop_back();
			const VkImage image = p.texture->GetImage();
			const u64 flushes = mirror.GetStats().flushes;
			m_dev->Recycle(p.texture);
			expect(!mirror.HasPending() && mirror.GetStats().flushes == flushes + 1, "Recycle flushed the batch");
			m_dev->AgePool();
			GSTextureVK* again = static_cast<GSTextureVK*>(m_dev->CreateTexture(s.rect.width(), s.rect.height(), 1, GSTexture::Format::UNorm8));
			expect(again->GetImage() == image, "the pool handed the recycled image back");
			expect(!again->IsDecodePending(), "and it is not pending");
			p.texture = again;
			batch.push_back(std::move(p));
			Check(mirror, batch, t);
		}

		// (d) Destroying a pending texture cancels its decode and keeps the blocks it staged.
		{
			const WindowSpec s = window();
			Queue(mirror, mem, s, batch, t);
			GSTextureVK* tex = batch.back().texture;
			batch.pop_back();
			const u64 cancelled = mirror.GetStats().cancelled;
			const u64 flushes = mirror.GetStats().flushes;
			delete tex;
			expect(mirror.GetStats().cancelled == cancelled + 1, "destroying the texture cancelled its decode");
			expect(mirror.GetStats().flushes == flushes, "and did not flush");
			expect(mirror.HasPending(), "the staged blocks are still to be scattered");
			mirror.Flush();
			m_dev->ExecuteCommandBuffer(true);
			VerifyMirror(mirror, mem, t); // the cancelled window's blocks are in the mirror, clean
			// Decoding the same window now reads them from the mirror.
			Queue(mirror, mem, s, batch, t);
			Check(mirror, batch, t);
		}

		// (e) A texture a draw or copy of the command buffer being built already used is refused, because its
		// update would belong in the draw buffer; once that buffer is submitted it is not.
		{
			const WindowSpec s = window();
			GSTextureVK* tex = NewTexture(s);
			std::vector<u8> zeros(static_cast<size_t>(s.rect.width()) * s.rect.height(), 0);
			tex->Update(GSVector4i(0, 0, s.rect.width(), s.rect.height()), zeros.data(), s.rect.width());
			GSTexture* rt = m_dev->CreateRenderTarget(64, 64, GSTexture::Format::Color, true, false);
			m_dev->StretchRect(tex, rt, ShaderConvertSelector(ShaderConvert::COPY), Filter::Nearest);
			const GSOffset off = mem.mem->GetOffset(s.tbp, s.tbw, s.psm);
			GSVramMirrorVK::Request rq{&off, s.rect, tex, mem.Source()};
			expect(mirror.DecodeWindow(rq) == GSVramMirrorVK::Result::Unsupported, "a texture used by this command buffer is refused");
			expect(!tex->IsDecodePending() && !mirror.HasPending(), "and nothing was queued");
			m_dev->ExecuteCommandBuffer(true);
			expect(mirror.DecodeWindow(rq) == GSVramMirrorVK::Result::Queued, "after the submit it is accepted");
			// Queued twice is refused too: a decode of the image is already in flight.
			expect(mirror.DecodeWindow(rq) == GSVramMirrorVK::Result::Unsupported, "a pending texture is not queued again");
			Pending p;
			p.spec = s;
			p.texture = tex;
			p.expected = CpuDecode(mem, s);
			batch.push_back(std::move(p));
			Check(mirror, batch, t);
			m_dev->Recycle(rt);
		}

		VerifyMirror(mirror, mem, t);
		Report("hooks (submit, upload, recycle, destroy, refusals)", t);
		ST_PRINT("hooks: %u checks, %u failed", checks, failed);
		if (failed != 0)
			m_ok = false;
	}

	/// Staging overflow is a NeedSubmit, a full scratch is an internal flush, a window larger than the
	/// scratch is refused, and a meta region held by the unsubmitted command buffer is a NeedSubmit.
	void Test::PhaseLimits()
	{
		// Staging: 256 slots of 256 bytes.
		{
			GSVramMirrorVK::Config cfg;
			cfg.staging_slots = 256;
			cfg.meta_regions = 4;
			std::unique_ptr<GSVramMirrorVK> mirror = GSVramMirrorVK::Create(cfg);
			if (!mirror)
			{
				m_ok = false;
				return;
			}
			Memory mem(m_rng, true);
			Totals t;
			std::vector<Pending> batch;
			u64 need_submit = 0, too_big = 0;
			for (u32 i = 0; i < 6; i++)
			{
				WindowSpec s{PSMT8, 4096 + i * 512, 4, GSVector4i(0, 0, 192, 192)}; // 144 blocks, all of them new
				Pending p;
				p.spec = s;
				p.texture = NewTexture(s);
				p.expected = CpuDecode(mem, s);
				const GSOffset off = mem.mem->GetOffset(s.tbp, s.tbw, s.psm);
				GSVramMirrorVK::Request rq{&off, s.rect, p.texture, mem.Source()};
				GSVramMirrorVK::Result r = mirror->DecodeWindow(rq);
				if (r == GSVramMirrorVK::Result::NeedSubmit)
				{
					need_submit++;
					Check(*mirror, batch, t);
					r = mirror->DecodeWindow(rq);
				}
				if (r != GSVramMirrorVK::Result::Queued)
				{
					t.refused++;
					m_dev->Recycle(p.texture);
					continue;
				}
				batch.push_back(std::move(p));
			}
			Check(*mirror, batch, t);
			// 520 x 520 PSMT8 is 1089 blocks: more slots than the ring has.
			{
				WindowSpec s{PSMT8, 0, 8, GSVector4i(0, 0, 520, 520)};
				GSTextureVK* tex = NewTexture(s);
				const GSOffset off = mem.mem->GetOffset(s.tbp, s.tbw, s.psm);
				GSVramMirrorVK::Request rq{&off, s.rect, tex, mem.Source()};
				if (mirror->DecodeWindow(rq) == GSVramMirrorVK::Result::Unsupported)
					too_big++;
				else
					t.refused++;
				m_dev->Recycle(tex);
			}
			VerifyMirror(*mirror, mem, t);
			Report("limits: a 256-slot staging ring", t);
			ST_PRINT("limits: staging NeedSubmit answered=%llu (expected at least 1), window larger than the ring refused=%llu (expected 1)",
				static_cast<unsigned long long>(need_submit), static_cast<unsigned long long>(too_big));
			if (need_submit == 0 || too_big != 1)
				m_ok = false;
		}

		// Scratch: 1 MiB, so five 512 x 512 windows cannot share a batch.
		{
			GSVramMirrorVK::Config cfg;
			cfg.scratch_bytes = 1u << 20;
			cfg.meta_regions = 2;
			std::unique_ptr<GSVramMirrorVK> mirror = GSVramMirrorVK::Create(cfg);
			if (!mirror)
			{
				m_ok = false;
				return;
			}
			Memory mem(m_rng, true);
			Totals t;
			std::vector<Pending> batch;
			const GSVramMirrorVK::Stats before = mirror->GetStats();
			for (u32 i = 0; i < 7; i++)
			{
				WindowSpec s{PSMT8, i * 1024, 8, GSVector4i(0, 0, 512, 512)};
				Queue(*mirror, mem, s, batch, t);
			}
			Check(*mirror, batch, t);
			// Larger than the scratch on its own: the CPU path.
			WindowSpec big{PSMT8, 0, 16, GSVector4i(0, 0, 1100, 1000)};
			GSTextureVK* tex = NewTexture(big);
			const GSOffset off = mem.mem->GetOffset(big.tbp, big.tbw, big.psm);
			GSVramMirrorVK::Request rq{&off, big.rect, tex, mem.Source()};
			const bool refused = mirror->DecodeWindow(rq) == GSVramMirrorVK::Result::Unsupported;
			m_dev->Recycle(tex);
			VerifyMirror(*mirror, mem, t);
			const GSVramMirrorVK::Stats d = Diff(mirror->GetStats(), before);
			Report("limits: a 1 MiB scratch", t, &d);
			ST_PRINT("limits: scratch flushes=%llu (expected at least 1), window larger than the scratch refused=%d (expected 1)",
				static_cast<unsigned long long>(d.scratch_flushes), refused ? 1 : 0);
			if (d.scratch_flushes == 0 || !refused)
				m_ok = false;
		}

		// Meta regions: two of them, so a third flush in one unsubmitted command buffer has none.
		{
			GSVramMirrorVK::Config cfg;
			cfg.meta_regions = 2;
			std::unique_ptr<GSVramMirrorVK> mirror = GSVramMirrorVK::Create(cfg);
			if (!mirror)
			{
				m_ok = false;
				return;
			}
			Memory mem(m_rng, true);
			Totals t;
			std::vector<Pending> batch;
			for (u32 i = 0; i < 2; i++)
			{
				Queue(*mirror, mem, RandomWindow(m_rng, Format::T8, mem), batch, t);
				mirror->Flush();
			}
			WindowSpec s = RandomWindow(m_rng, Format::T8, mem);
			GSTextureVK* tex = NewTexture(s);
			const GSOffset off = mem.mem->GetOffset(s.tbp, s.tbw, s.psm);
			GSVramMirrorVK::Request rq{&off, s.rect, tex, mem.Source()};
			const bool blocked = mirror->DecodeWindow(rq) == GSVramMirrorVK::Result::NeedSubmit;
			m_dev->Recycle(tex);
			Check(*mirror, batch, t); // submit and wait
			Queue(*mirror, mem, s, batch, t); // and now there is a region
			Check(*mirror, batch, t);
			VerifyMirror(*mirror, mem, t);
			Report("limits: two meta regions", t);
			ST_PRINT("limits: third flush in one command buffer answered NeedSubmit=%d (expected 1)", blocked ? 1 : 0);
			if (!blocked)
				m_ok = false;
		}
	}

	/// GPU time of the decode, the scatter and the copies, on windows shaped like the boss fight's: 214
	/// windows of 256 x 256 (14 MB of decoded texels, 55k block-table entries).
	///
	/// Two instruments. The timestamps written between the phases of one flush, and the total of a flush
	/// with the phases taken away one at a time: the copies are dropped by cancelling the textures, and
	/// the scatter is empty when no block is dirty. A driver may write a bottom-of-pipe timestamp before
	/// the work it follows has finished (Honeykrisp does), which would put a phase's time in its
	/// neighbour; the totals do not depend on that, so the per-MB figures come from them.
	void Test::PhaseTiming()
	{
		std::unique_ptr<GSVramMirrorVK> mirror = GSVramMirrorVK::Create();
		if (!mirror || !mirror->EnableTiming(true))
		{
			ST_PRINT("timing: skipped (no GPU timestamps on this device)");
			return;
		}

		struct Sample
		{
			double gpu_total_ms, wall_ms, decode_ts, barrier_ts, scatter_ts, copy_ts, decoded_mb, scattered_mb;
		};
		const auto median = [](std::vector<double> v) {
			std::sort(v.begin(), v.end());
			return v[v.size() / 2];
		};

		for (const Format fmt : {Format::T8, Format::T4, Format::T8H})
		{
			Memory mem(m_rng, true);
			Totals t;
			const u32 psm = kPsms[static_cast<u32>(fmt)];
			const int side = (fmt == Format::T8H) ? 128 : 256; // 8x8-texel blocks: 214 windows of the same count, a quarter of the bytes

			// One timed flush of 214 windows. Cancelling the textures leaves the decode and the scatter and
			// drops the copies and their barriers.
			const auto run = [&](bool all_dirty, u32 dirty_blocks, bool copies, bool compare) -> Sample {
				if (all_dirty)
					std::fill(mem.dirty.begin(), mem.dirty.end(), ~0ull);
				for (u32 i = 0; i < dirty_blocks; i++)
					mem.MarkDirty(m_rng.Pick(GSVramMirror::kMirrorBlocks));
				std::vector<Pending> batch;
				for (u32 w = 0; w < 214; w++)
				{
					Pending p;
					p.spec = WindowSpec{psm, m_rng.Pick(400) * 32, 4, GSVector4i(0, 0, side, side)};
					p.texture = NewTexture(p.spec);
					const GSOffset off = mem.mem->GetOffset(p.spec.tbp, p.spec.tbw, p.spec.psm);
					GSVramMirrorVK::Request rq{&off, p.spec.rect, p.texture, mem.Source()};
					if (compare)
						p.expected = CpuDecode(mem, p.spec);
					if (mirror->DecodeWindow(rq) != GSVramMirrorVK::Result::Queued)
					{
						t.refused++;
						m_dev->Recycle(p.texture);
						continue;
					}
					batch.push_back(std::move(p));
				}
				if (!copies)
					for (Pending& p : batch)
						mirror->CancelTexture(p.texture);

				mirror->Flush();
				const auto t0 = std::chrono::steady_clock::now();
				m_dev->ExecuteCommandBuffer(true);
				const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				Sample out{};
				out.wall_ms = wall;
				GSVramMirrorVK::Timings tm;
				if (mirror->ReadTimings(&tm))
				{
					out.decode_ts = tm.decode_ms;
					out.barrier_ts = tm.barrier_ms;
					out.scatter_ts = tm.scatter_ms;
					out.copy_ts = tm.copy_ms;
					out.gpu_total_ms = tm.decode_ms + tm.barrier_ms + tm.scatter_ms + tm.copy_ms;
					out.decoded_mb = tm.decoded_bytes / 1e6;
					out.scattered_mb = tm.scattered_bytes / 1e6;
				}
				if (compare && copies)
				{
					std::vector<std::unique_ptr<GSDownloadTexture>> downloads;
					for (Pending& p : batch)
					{
						const GSVector4i rc(0, 0, p.spec.rect.width(), p.spec.rect.height());
						downloads.push_back(m_dev->CreateDownloadTexture(rc.width(), rc.height(), GSTexture::Format::UNorm8));
						downloads.back()->CopyFromTexture(rc, p.texture, rc, 0, true);
					}
					for (auto& d : downloads)
						d->Flush();
					for (size_t i = 0; i < batch.size(); i++)
					{
						const GSVector4i rc(0, 0, batch[i].spec.rect.width(), batch[i].spec.rect.height());
						downloads[i]->Map(rc);
						bool same = true;
						for (int y = 0; y < rc.height() && same; y++)
							same = std::memcmp(downloads[i]->GetMapPointer() + static_cast<size_t>(y) * downloads[i]->GetMapPitch(),
										&batch[i].expected[static_cast<size_t>(y) * rc.width()], rc.width()) == 0;
						t.windows++;
						t.texels += static_cast<u64>(rc.width()) * rc.height();
						if (!same)
							t.bad_windows++;
						downloads[i]->Unmap();
					}
				}
				for (Pending& p : batch)
					m_dev->Recycle(p.texture);
				return out;
			};

			struct Case
			{
				const char* name;
				bool all_dirty;
				u32 dirty_blocks;
				bool copies;
			};
			const Case cases[] = {
				{"decode only (nothing dirty, no copies)", false, 0, false},
				{"decode + copies (nothing dirty)", false, 0, true},
				{"decode + scatter of ~2700 blocks (no copies)", false, 2700, false},
				{"decode + scatter of the whole 4 MiB (no copies)", true, 0, false},
			};
			// Cold pass first: it stages the memory and pays the pipeline and first-touch costs.
			run(true, 0, true, false);
			run(false, 0, false, false);

			// What a submit and wait costs with nothing in it: the floor under every wall figure below.
			double empty_wall;
			{
				std::vector<double> w;
				for (u32 r = 0; r < 15; r++)
				{
					const auto t0 = std::chrono::steady_clock::now();
					m_dev->ExecuteCommandBuffer(true);
					w.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
				}
				empty_wall = median(w);
			}

			// Per case: the median of each timestamp phase, of the flush's total, and of the wall time.
			struct Medians
			{
				double decode, barrier, scatter, copy, total, wall;
			} med[4];
			double decoded_mb = 0, scattered_small = 0, scattered_full = 0;
			for (u32 c = 0; c < 4; c++)
			{
				std::vector<double> dec, bar, sca, cop, tot, wal;
				for (u32 r = 0; r < 7; r++)
				{
					const Sample sm = run(cases[c].all_dirty, cases[c].dirty_blocks, cases[c].copies, false);
					dec.push_back(sm.decode_ts);
					bar.push_back(sm.barrier_ts);
					sca.push_back(sm.scatter_ts);
					cop.push_back(sm.copy_ts);
					tot.push_back(sm.gpu_total_ms);
					wal.push_back(sm.wall_ms);
					decoded_mb = sm.decoded_mb;
					if (c == 2)
						scattered_small = sm.scattered_mb;
					if (c == 3)
						scattered_full = sm.scattered_mb;
				}
				med[c] = {median(dec), median(bar), median(sca), median(cop), median(tot), median(wal)};
				ST_PRINT("timing %s: %-48s timestamps (median of 7) decode %.3f barriers %.3f scatter %.3f copies %.3f ms (total %.3f), "
						 "wall %.3f ms",
					kPsmNames[static_cast<u32>(fmt)], cases[c].name, med[c].decode, med[c].barrier, med[c].scatter, med[c].copy,
					med[c].total, med[c].wall);
			}
			// The last pass is compared with the CPU: copies on, 2700 dirty.
			run(false, 2700, true, true);

			const char* n = kPsmNames[static_cast<u32>(fmt)];
			ST_PRINT("timing %s summary (214 windows of %dx%d, %.2f MB decoded per flush):", n, side, side, decoded_mb);
			ST_PRINT("timing %s:   empty submit and wait                  wall %.3f ms", n, empty_wall);
			ST_PRINT("timing %s:   decode   GPU timestamps %.3f ms = %.4f ms/MB;  wall above an empty submit %.3f ms = %.4f ms/MB", n,
				med[0].decode, med[0].decode / decoded_mb, med[0].wall - empty_wall, (med[0].wall - empty_wall) / decoded_mb);
			ST_PRINT("timing %s:   scatter  %.2f MB: GPU timestamps %.3f ms = %.4f ms/MB;  wall over decode only %.3f ms", n,
				scattered_small, med[2].scatter, scattered_small > 0 ? med[2].scatter / scattered_small : 0.0, med[2].wall - med[0].wall);
			ST_PRINT("timing %s:   scatter  %.2f MB: GPU timestamps %.3f ms = %.4f ms/MB;  wall over decode only %.3f ms", n,
				scattered_full, med[3].scatter, scattered_full > 0 ? med[3].scatter / scattered_full : 0.0, med[3].wall - med[0].wall);
			ST_PRINT("timing %s:   buffer->image copies (214)  GPU timestamps %.3f ms;  wall over decode only %.3f ms", n, med[1].copy,
				med[1].wall - med[0].wall);
			char name[48];
			std::snprintf(name, sizeof(name), "timing %s compare", kPsmNames[static_cast<u32>(fmt)]);
			Report(name, t);
		}
	}

	int Test::Run()
	{
		const VkPhysicalDeviceProperties& props = m_dev->GetDeviceProperties();
		ST_PRINT("device=\"%s\" vendor=0x%x driver=\"%s\" seed=%u windows_per_format=%u", props.deviceName, props.vendorID,
			m_dev->GetDriverInfo().c_str(), m_opt.seed, m_opt.windows_per_format);

		if (!m_dev->CreateVramMirror())
		{
			ST_PRINT("RESULT: FAIL the mirror could not be created on this device");
			return 1;
		}

		PhaseFuzz();
		PhaseControl();
		PhaseSync();
		PhasePooled();
		PhaseCancel();
		PhaseHooks();
		PhaseLimits();
		PhaseTiming();

		ST_PRINT("RESULT: %s windows=%llu texels=%llu mismatched_windows=%llu mismatched_texels=%llu refused=%llu mirror_blocks_checked=%llu "
				 "mirror_mismatches=%llu",
			m_ok && m_all.Clean() ? "PASS" : "FAIL", static_cast<unsigned long long>(m_all.windows),
			static_cast<unsigned long long>(m_all.texels), static_cast<unsigned long long>(m_all.bad_windows),
			static_cast<unsigned long long>(m_all.bad_texels), static_cast<unsigned long long>(m_all.refused),
			static_cast<unsigned long long>(m_all.mirror_checked), static_cast<unsigned long long>(m_all.mirror_bad));
		return (m_ok && m_all.Clean()) ? 0 : 1;
	}
} // namespace

int GSVramMirrorSelfTest::Run(const Options& options)
{
	if (!g_gs_device || g_gs_device->GetRenderAPI() != RenderAPI::Vulkan)
	{
		std::printf("GPU-DECODE-SELFTEST RESULT: FAIL the GS device is not Vulkan (run with -renderer vulkan)\n");
		return 1;
	}
	Test test(GSDeviceVK::GetInstance(), options);
	return test.Run();
}

void GSVramMirrorSelfTest::SetVerify(bool enable)
{
	GSVramMirrorVK::SetVerify(enable);
}

bool GSVramMirrorSelfTest::ReportVerify()
{
	const GSVramMirrorVK::VerifyReport r = GSVramMirrorVK::GetVerifyReport();
	std::printf("VRAM-MIRROR-VERIFY windows=%llu texels=%llu bad_windows=%llu bad_texels=%llu flushes=%llu mirror_blocks=%llu "
				"mirror_bad_blocks=%llu mark_blocks=%llu mark_window_blocks=%llu mark_sweeps=%llu stale_blocks=%llu unseen_blocks=%llu\n",
		static_cast<unsigned long long>(r.windows), static_cast<unsigned long long>(r.texels),
		static_cast<unsigned long long>(r.bad_windows), static_cast<unsigned long long>(r.bad_texels),
		static_cast<unsigned long long>(r.flushes), static_cast<unsigned long long>(r.mirror_blocks),
		static_cast<unsigned long long>(r.mirror_bad_blocks), static_cast<unsigned long long>(r.mark_blocks),
		static_cast<unsigned long long>(r.mark_window_blocks), static_cast<unsigned long long>(r.mark_sweeps),
		static_cast<unsigned long long>(r.mark_stale), static_cast<unsigned long long>(r.mark_unseen));
	std::fflush(stdout);
	return r.Clean();
}

#else

int GSVramMirrorSelfTest::Run(const Options&)
{
	std::printf("GPU-DECODE-SELFTEST RESULT: FAIL this build has no Vulkan renderer\n");
	return 1;
}

void GSVramMirrorSelfTest::SetVerify(bool)
{
}

bool GSVramMirrorSelfTest::ReportVerify()
{
	std::printf("VRAM-MIRROR-VERIFY unavailable: this build has no Vulkan renderer\n");
	return false;
}

#endif
