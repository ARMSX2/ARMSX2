// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The host-to-local image writers (GSLocalMemory::m_psm[psm].wi, which GSState::ExecTransferRecord calls)
// are handed `len` bytes of source and must read no more than that.
//
// A read past the end is invisible while the byte after the source happens to be mapped, which is nearly
// always: the transfer buffer is much larger than most transfers, and a heap block is followed by another
// block. It faults when the source ends where a mapping does. Here every source ends exactly at the end of
// a mapped page, the page after it is inaccessible, and a read past the end is caught and reported with the
// transfer that made it. This is what killed gs_vertex_tests with a bus error on the macOS Nightly run of
// 0d525e4bdb: GSLocalMemoryWriteStamps.TheRectangleOfAnImageWriteCoversEveryPageItChanges hands a
// std::vector's worth of bytes to every writer, and a 24-bit write read one byte past it.
//
// The 4-bit writers (PSMT4HL and PSMT4HH on an odd width, and PSMT4 on a slice) still read past their source.
// The cases for them are DISABLED_ below, because what they should do instead is not settled.
//
// POSIX only: the guard page is an mmap and the fault is a signal.

#include "GS/GSLocalMemory.h"

#include <gtest/gtest.h>

#if !defined(_WIN32)

#include <csetjmp>
#include <csignal>
#include <cstring>
#include <iterator>
#include <memory>
#include <random>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

namespace
{
	// Every format a surface can have in local memory.
	constexpr u32 kPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};

	// The formats whose writers keep to their source on every transfer below.
	constexpr u32 kBoundedPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};

	// The ones that do not, on an odd width: the 4-bit formats that share a word with other data.
	constexpr u32 kFourBitHighPsms[] = {PSMT4HL, PSMT4HH};

	// The formats whose writers keep to a slice of a transfer. The 4-bit ones do not.
	constexpr u32 kSlicedPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT8H, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
	constexpr u32 kFourBitPsms[] = {PSMT4, PSMT4HL, PSMT4HH};

	sigjmp_buf s_jump;
	volatile sig_atomic_t s_armed = 0;

	void OnFault(int sig, siginfo_t*, void*)
	{
		if (s_armed)
			siglongjmp(s_jump, sig);

		// Not ours: a fault outside a guarded call is a real crash.
		std::signal(sig, SIG_DFL);
		std::raise(sig);
	}

	/// Turns SIGSEGV and SIGBUS into a return value for the length of one scope. macOS reports a
	/// protection fault as SIGBUS and Linux as SIGSEGV.
	class FaultCatcher
	{
	public:
		FaultCatcher()
		{
			struct sigaction sa = {};
			sa.sa_sigaction = OnFault;
			sa.sa_flags = SA_SIGINFO | SA_NODEFER;
			sigemptyset(&sa.sa_mask);
			sigaction(SIGSEGV, &sa, &m_old_segv);
			sigaction(SIGBUS, &sa, &m_old_bus);
		}
		~FaultCatcher()
		{
			sigaction(SIGSEGV, &m_old_segv, nullptr);
			sigaction(SIGBUS, &m_old_bus, nullptr);
		}
		FaultCatcher(const FaultCatcher&) = delete;
		FaultCatcher& operator=(const FaultCatcher&) = delete;

		/// Runs fn(ctx); returns 0, or the signal that stopped it.
		static int Call(void (*fn)(void*), void* ctx)
		{
			const int sig = sigsetjmp(s_jump, 1);
			if (sig != 0)
			{
				s_armed = 0;
				return sig;
			}
			s_armed = 1;
			fn(ctx);
			s_armed = 0;
			return 0;
		}

	private:
		struct sigaction m_old_segv = {};
		struct sigaction m_old_bus = {};
	};

	/// `len` bytes that end exactly where the mapping does, with an inaccessible page after them.
	class EdgeBuffer
	{
	public:
		explicit EdgeBuffer(size_t len)
		{
			const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
			const size_t body = (len + page - 1) / page * page;
			m_total = body + page;
			void* base = mmap(nullptr, m_total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			if (base == MAP_FAILED)
				return;
			m_base = static_cast<u8*>(base);
			if (mprotect(m_base + body, page, PROT_NONE) != 0)
				return;
			m_data = m_base + body - len;
			std::memset(m_data, 0xFF, len);
		}
		~EdgeBuffer()
		{
			if (m_base)
				munmap(m_base, m_total);
		}
		EdgeBuffer(const EdgeBuffer&) = delete;
		EdgeBuffer& operator=(const EdgeBuffer&) = delete;

		/// Null when the buffer could not be set up.
		const u8* data() const { return m_data; }

	private:
		u8* m_base = nullptr;
		u8* m_data = nullptr;
		size_t m_total = 0;
	};

	struct Transfer
	{
		u32 psm;
		u32 bp, bw;
		int x0, y0, w, h;
		/// How much of the image the call is handed: all of it when 0, else a slice from the start.
		size_t slice_bytes = 0;

		size_t ImageLen() const { return (static_cast<size_t>(w) * h * GSLocalMemory::m_psm[psm].trbpp + 7) / 8; }
		size_t Len() const { return slice_bytes ? slice_bytes : ImageLen(); }

		std::string Describe() const
		{
			return "psm " + std::to_string(psm) + " bp " + std::to_string(bp) + " bw " + std::to_string(bw) + " rect " + std::to_string(x0) + "," +
			       std::to_string(y0) + " " + std::to_string(w) + "x" + std::to_string(h) + (slice_bytes ? " slice " + std::to_string(slice_bytes) : "");
		}
	};

	struct WriterCall
	{
		GSLocalMemory* mem;
		const Transfer* t;
		const u8* src;
	};

	void RunWriter(void* ctx)
	{
		const WriterCall& c = *static_cast<const WriterCall*>(ctx);
		const Transfer& t = *c.t;

		GIFRegBITBLTBUF blit = {};
		blit.DBP = t.bp;
		blit.DBW = t.bw;
		blit.DPSM = t.psm;
		GIFRegTRXPOS pos = {};
		pos.DSAX = t.x0;
		pos.DSAY = t.y0;
		GIFRegTRXREG reg = {};
		reg.RRW = t.w;
		reg.RRH = t.h;

		int tx = t.x0, ty = t.y0;
		GSLocalMemory::m_psm[t.psm].wi(*c.mem, tx, ty, c.src, static_cast<int>(t.Len()), blit, pos, reg);
	}

	class GSLocalMemoryWriteSourceBounds : public ::testing::Test
	{
	protected:
		void SetUp() override { m_mem = std::make_unique<GSLocalMemory>(); }

		/// Runs the writer on a source that ends at a guard page; true when it stayed inside it. A read
		/// past the end is reported as a failure that names the transfer.
		bool ExpectInsideItsSource(const Transfer& t)
		{
			const EdgeBuffer buf(t.Len());
			if (!buf.data())
			{
				ADD_FAILURE() << "could not map the source buffer for " << t.Describe();
				return false;
			}

			WriterCall call{m_mem.get(), &t, buf.data()};
			const int sig = FaultCatcher::Call(RunWriter, &call);
			if (sig != 0)
				ADD_FAILURE() << t.Describe() << ": the writer read past the " << t.Len() << " bytes it was handed (signal " << sig << ")";
			return sig == 0;
		}

		/// Bw counts 64-pixel columns; 8 and 4-bit pages are 128 wide and take it in pairs.
		static u32 Bw(u32 psm, u32 columns) { return GSLocalMemory::m_psm[psm].pgs.x == 128 ? columns * 2 : columns; }

		// An odd size in both directions at an address that is not a block boundary: the per-pixel path, and for
		// 4-bit formats the odd width, whose rows end half way through a byte.
		template <size_t N>
		void OddSized(const u32 (&psms)[N])
		{
			for (const u32 psm : psms)
				ExpectInsideItsSource({psm, 0x123, Bw(psm, 4), 5, 3, 133, 71});
		}

		// A transfer that arrives in slices hands each call only part of the image, and the rectangle is still
		// the whole image's. The call must stop where its bytes do.
		template <size_t N>
		void Sliced(const u32 (&psms)[N])
		{
			for (const u32 psm : psms)
			{
				Transfer t{psm, 0x123, Bw(psm, 4), 5, 3, 133, 71};
				t.slice_bytes = (t.ImageLen() / 3) & ~static_cast<size_t>(15); // whole quadwords, as the GIF delivers them
				ExpectInsideItsSource(t);
			}
		}

		// A single pixel, and a single row: the shortest sources there are.
		template <size_t N>
		void Smallest(const u32 (&psms)[N])
		{
			for (const u32 psm : psms)
			{
				ExpectInsideItsSource({psm, 0, Bw(psm, 1), 0, 0, 1, 1});
				ExpectInsideItsSource({psm, 0, Bw(psm, 1), 3, 7, 5, 1});
				ExpectInsideItsSource({psm, 0, Bw(psm, 1), 0, 0, 3, 3});
			}
		}

		// The sweep GSLocalMemoryWriteStamps.TheRectangleOfAnImageWriteCoversEveryPageItChanges makes, which is
		// where the bus error was: random formats, block addresses, widths and rectangles.
		template <size_t N>
		void Random(const u32 (&psms)[N])
		{
			std::mt19937_64 rng(1234);
			const auto random = [&](u32 n) { return static_cast<u32>(rng() % n); };

			for (int i = 0; i < 1500; i++)
			{
				Transfer t;
				t.psm = psms[random(N)];
				t.bp = random(GS_MAX_BLOCKS);
				t.bw = Bw(t.psm, 1 + random(16));
				t.x0 = static_cast<int>(random(300));
				t.y0 = static_cast<int>(random(300));
				t.w = 1 + static_cast<int>(random(200));
				t.h = 1 + static_cast<int>(random(200));

				// One failure names the transfer; thousands of repeats of it would only bury it.
				if (!ExpectInsideItsSource(t))
					break;
			}
		}

		FaultCatcher m_catcher;
		std::unique_ptr<GSLocalMemory> m_mem;
	};

	// One transfer per format that is page aligned, block aligned, and a whole number of blocks: the fast
	// paths, which read the source a block at a time.
	TEST_F(GSLocalMemoryWriteSourceBounds, ABlockAlignedTransferOfEachFormat)
	{
		for (const u32 psm : kPsms)
			ExpectInsideItsSource({psm, 0, Bw(psm, 1), 0, 0, 64, 64});
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, AnOddSizedTransferOfEachFormatAtAnUnalignedAddress)
	{
		OddSized(kBoundedPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, ASliceShorterThanItsTransferOfEachFormat)
	{
		Sliced(kSlicedPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, TheSmallestTransfersOfEachFormat)
	{
		Smallest(kBoundedPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, RandomTransfersOfEveryFormat)
	{
		Random(kBoundedPsms);
	}

	// DISABLED: the 4-bit writers still read past the end of a slice, and past the end of an odd-width image.
	// What they should do there instead is open, so these cases are not enabled.
	TEST_F(GSLocalMemoryWriteSourceBounds, DISABLED_AnOddSizedTransferOfEachFourBitHighFormat)
	{
		OddSized(kFourBitHighPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, DISABLED_ASliceShorterThanItsTransferOfEachFourBitFormat)
	{
		Sliced(kFourBitPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, DISABLED_TheSmallestTransfersOfEachFourBitHighFormat)
	{
		Smallest(kFourBitHighPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, DISABLED_RandomTransfersOfEachFourBitHighFormat)
	{
		Random(kFourBitHighPsms);
	}
} // namespace

#else

TEST(GSLocalMemoryWriteSourceBounds, NeedsAGuardPage)
{
	GTEST_SKIP() << "the guard page is an mmap and the fault a signal";
}

#endif
