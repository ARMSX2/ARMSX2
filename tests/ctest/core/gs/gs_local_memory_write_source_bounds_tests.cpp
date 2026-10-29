// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// The host-to-local image writers (GSLocalMemory::m_psm[psm].wi, which GSState::ExecTransferRecord calls)
// are handed `len` bytes of source and must read no more than that.
//
// A read past the end is invisible while the byte after the source happens to be mapped, which is nearly
// always: the transfer buffer is much larger than most transfers, and a heap block is followed by another
// block. It faults when the source ends where a mapping does. Here every source ends exactly at the end of
// a mapped page, the page after it is inaccessible, and a read past the end is caught and reported with the
// transfer that made it.
//
// The same is done for the start: every source is also placed so that it begins exactly at the start of a mapped
// page, with an inaccessible page before it, so a read of the byte before the source is caught too. PSMT4's
// block-aligned path did that for a start x that is odd: it read the source at byte x / 2 from a base one nibble out.
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
	constexpr u32 kPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};

	// Every format but PSMT4HL and PSMT4HH, which have cases of their own below.
	constexpr u32 kBoundedPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMZ32, PSMZ24, PSMZ16, PSMZ16S};

	// The 4-bit formats that keep their nibble in a word that holds other data.
	constexpr u32 kFourBitHighPsms[] = {PSMT4HL, PSMT4HH};

	// The slice cases, in two lists: the 4-bit formats have a case of their own below.
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

	/// Which end of the source sits against an inaccessible page.
	enum class Edge
	{
		End, ///< the source ends exactly where the mapping does: a read past its last byte faults
		Start, ///< the source begins exactly where the mapping does: a read before its first byte faults
	};

	/// `len` bytes that touch an inaccessible page at one end, and are followed (End) or preceded (Start) by mapped
	/// memory of 0xFF at the other.
	class EdgeBuffer
	{
	public:
		EdgeBuffer(size_t len, Edge edge)
		{
			const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
			const size_t body = (len + page - 1) / page * page;
			m_total = page + body + page;
			void* base = mmap(nullptr, m_total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
			if (base == MAP_FAILED)
				return;
			m_base = static_cast<u8*>(base);
			if (mprotect(m_base, page, PROT_NONE) != 0 || mprotect(m_base + page + body, page, PROT_NONE) != 0)
				return;
			m_data = edge == Edge::End ? m_base + page + body - len : m_base + page;
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

		/// Runs the writer on a source that ends at a guard page, and again on one that starts at a guard page; true when
		/// it stayed inside the source both times. A violation is reported as a failure that names the transfer.
		bool ExpectInsideItsSource(const Transfer& t)
		{
			for (const Edge edge : {Edge::End, Edge::Start})
			{
				const EdgeBuffer buf(t.Len(), edge);
				if (!buf.data())
				{
					ADD_FAILURE() << "could not map the source buffer for " << t.Describe();
					return false;
				}

				WriterCall call{m_mem.get(), &t, buf.data()};
				const int sig = FaultCatcher::Call(RunWriter, &call);
				if (sig != 0)
				{
					const char* where = edge == Edge::End ? "past the end of" : "before the start of";
					ADD_FAILURE() << t.Describe() << ": the writer read " << where << " the " << t.Len() << " bytes it was handed (signal " << sig << ")";
					return false;
				}
			}
			return true;
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

		// The sweep GSLocalMemoryWriteStamps.TheRectangleOfAnImageWriteCoversEveryPageItChanges makes: random
		// formats, block addresses, widths and rectangles.
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

		// PSMT4 with an even width of 34 or more from an odd start x: the rectangle holds a whole 32-pixel block column, so
		// the block-aligned path takes it, and some hold whole 16-row block rows too. The rows start half way through a
		// byte of the source; the writer must not read the byte before the source to make up for it, nor stop a nibble
		// short of the end.
		void WideFromOddStartX()
		{
			const u32 bw = Bw(PSMT4, 4);
			ExpectInsideItsSource({PSMT4, 0x123, bw, 5, 3, 130, 71});
			ExpectInsideItsSource({PSMT4, 0x123, bw, 31, 5, 34, 1}); // the narrowest way in: pixels 31 | 32..63 | 64
			ExpectInsideItsSource({PSMT4, 0x123, bw, 61, 38, 102, 4});
			ExpectInsideItsSource({PSMT4, 0x123, bw, 1, 16, 66, 33});
			ExpectInsideItsSource({PSMT4, 0, bw, 33, 0, 64, 16});
			ExpectInsideItsSource({PSMT4, 0x123, bw, 5, 3, 128, 1});
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

	// The 4-bit formats: an odd width shares a byte between the end of one row and the start of the next, and a slice
	// of a transfer holds only part of the image. See gs_local_memory_four_bit_stream_tests.cpp for where the pixels go.
	TEST_F(GSLocalMemoryWriteSourceBounds, AnOddSizedTransferOfEachFourBitHighFormat)
	{
		OddSized(kFourBitHighPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, ASliceShorterThanItsTransferOfEachFourBitFormat)
	{
		Sliced(kFourBitPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, TheSmallestTransfersOfEachFourBitHighFormat)
	{
		Smallest(kFourBitHighPsms);
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, RandomTransfersOfEachFourBitHighFormat)
	{
		Random(kFourBitHighPsms);
	}

	// PSMT4 from an odd start x with a rectangle wide enough for the block-aligned path. See
	// gs_local_memory_four_bit_stream_tests.cpp for where the pixels go.
	TEST_F(GSLocalMemoryWriteSourceBounds, AWideTransferIntoPSMT4FromAnOddStartX)
	{
		WideFromOddStartX();
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, ASliceShorterThanItsTransferIntoPSMT4FromAnOddStartX)
	{
		const u32 bw = Bw(PSMT4, 4);
		for (const Transfer shape : {Transfer{PSMT4, 0x123, bw, 5, 3, 130, 71}, Transfer{PSMT4, 0x123, bw, 61, 38, 102, 40}, Transfer{PSMT4, 0, bw, 33, 0, 64, 32}})
		{
			Transfer t = shape;
			t.slice_bytes = (t.ImageLen() / 3) & ~static_cast<size_t>(15); // whole quadwords, as the GIF delivers them
			ExpectInsideItsSource(t);
		}
	}

	TEST_F(GSLocalMemoryWriteSourceBounds, RandomWideTransfersIntoPSMT4FromAnOddStartX)
	{
		std::mt19937_64 rng(5678);
		const auto random = [&](u32 n) { return static_cast<u32>(rng() % n); };

		for (int i = 0; i < 1500; i++)
		{
			Transfer t;
			t.psm = PSMT4;
			t.bp = random(GS_MAX_BLOCKS);
			t.bw = Bw(PSMT4, 1 + random(16));
			t.x0 = static_cast<int>(random(300)) | 1;
			t.y0 = static_cast<int>(random(300));
			t.w = 34 + 2 * static_cast<int>(random(100));
			t.h = 1 + static_cast<int>(random(80));
			if (random(3) == 0)
				t.slice_bytes = (t.ImageLen() / 2) & ~static_cast<size_t>(15);

			if (!ExpectInsideItsSource(t))
				break;
		}
	}
} // namespace

#else

TEST(GSLocalMemoryWriteSourceBounds, NeedsAGuardPage)
{
	GTEST_SKIP() << "the guard page is an mmap and the fault a signal";
}

#endif
