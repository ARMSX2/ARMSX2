// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Console-conformance pins for the SW scanline's destination-alpha test.
//
// The gs-test hardware capture (SCPH-30001, 2026-08-11) measured DATE against
// silicon: the destination alpha bit -- bit 31 at 32 bits per pixel, bit 15 at
// 16 -- selects per pixel, and DATM picks which bit value passes. The ARM64
// JIT scanline never passed any pixel at 16-bit DATM=1: TestDestAlpha isolated
// the alpha bit into a lane and then CMEQ'd the zero constant it had just
// materialized -- not the isolated bit -- against zero, so every lane always
// read "fail". The x86 generator compares the isolated bit (pcmpeqd xym1,
// xym0); the ARM64 port transposed the operands. That one selector shape was
// the SW renderer's single miss in the capture's 663 cases.
//
// These tests compile the JIT scanline directly for the four DATE selector
// shapes (32/16 bit x DATM 0/1) over a seeded frame and pin exactly which
// pixels the draw may touch. No rasterizer, no renderer: the scanline function
// is called the way GSRasterizer calls it, with the state GSRendererSW's
// selector derivation would produce for an untextured flat sprite with no
// depth buffer and no blending.

#include "common/Pcsx2Defs.h"

#ifdef ARCH_ARM64

#include "GS/Renderers/SW/GSDrawScanlineCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSVertexSW.h"
#include "GS/GSLocalMemory.h"
#include "common/HostSys.h"

#include <gtest/gtest.h>

#include <cstring>

#ifndef _WIN32
#include <sys/mman.h>
#else
#include "common/RedtapeWindows.h"
#endif

namespace
{
using DrawScanlinePtr = void (*)(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local);

// The flat draw colour, chosen so its 5551 projection has the alpha bit clear
// and collides with neither seed value below.
constexpr u32 kDrawR = 0x40, kDrawG = 0x80, kDrawB = 0xC0, kDrawA = 0x00;
constexpr u32 kWritten32 = kDrawR | (kDrawG << 8) | (kDrawB << 16) | (kDrawA << 24);
constexpr u16 kWritten16 = ((kDrawB & 0xF8) << 7) | ((kDrawG & 0xF8) << 2) | (kDrawR >> 3);

class SwScanlineDateTest : public ::testing::Test
{
protected:
	static constexpr size_t kCodeSlotSize = 16 * 1024;
	static constexpr size_t kCodeBufferSize = 16 * kCodeSlotSize;

	static void SetUpTestSuite()
	{
#ifdef _WIN32
		s_code = static_cast<u8*>(VirtualAlloc(nullptr, kCodeBufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#elif defined(__APPLE__)
		s_code = static_cast<u8*>(mmap(nullptr, kCodeBufferSize, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0));
		if (s_code == MAP_FAILED)
			s_code = nullptr;
#else
		s_code = static_cast<u8*>(mmap(nullptr, kCodeBufferSize, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
		if (s_code == MAP_FAILED)
			s_code = nullptr;
#endif
		s_code_used = 0;

		// Also populates the static m_psm swizzle tables on first construction.
		s_mem = new GSLocalMemory();
	}

	static void TearDownTestSuite()
	{
		delete s_mem;
		s_mem = nullptr;

		if (s_code)
		{
#ifdef _WIN32
			VirtualFree(s_code, 0, MEM_RELEASE);
#else
			munmap(s_code, kCodeBufferSize);
#endif
			s_code = nullptr;
		}
	}

	static DrawScanlinePtr Compile(GSScanlineSelector sel)
	{
		if (!s_code || s_code_used + kCodeSlotSize > kCodeBufferSize)
			return nullptr;

		u8* slot = s_code + s_code_used;
		s_code_used += kCodeSlotSize;

		HostSys::BeginCodeWriteRange(slot, kCodeSlotSize);
		GSDrawScanlineCodeGenerator cg(sel.key, slot, kCodeSlotSize);
		cg.Generate();
		HostSys::EndCodeWriteRange(slot, kCodeSlotSize);
		HostSys::FlushInstructionCache(slot, static_cast<u32>(cg.GetSize()));

		return reinterpret_cast<DrawScanlinePtr>(const_cast<u8*>(cg.GetCode()));
	}

	// Mirrors GSRendererSW::GetScanlineGlobalData's derivation for an
	// untextured flat sprite: no depth buffer, alpha test ALWAYS, no blend,
	// FBMSK zero. DATE forces the tested frame path (ftest, rfb).
	static GSScanlineSelector MakeDateSelector(u32 fpsm, bool datm)
	{
		GSScanlineSelector sel;
		sel.key = 0;
		sel.fpsm = fpsm;
		sel.zpsm = 3;
		sel.atst = ATST_ALWAYS;
		sel.tfx = TFX_NONE;
		sel.ababcd = 0xff;
		sel.prim = GS_SPRITE_CLASS;
		sel.fwrite = 1;
		sel.ftest = 1;
		sel.rfb = 1;
		sel.date = 1;
		sel.datm = datm ? 1 : 0;
		sel.colclamp = 1;
		return sel;
	}

	// FBP 0, FBW 1 (64 pixels), depth side unused but the offset table needs a
	// well-formed ZBUF.
	static const GSPixelOffset4* GetOffsets(u32 frame_psm)
	{
		GIFRegFRAME frame;
		frame.U64 = 0;
		frame.FBP = 0;
		frame.FBW = 1;
		frame.PSM = frame_psm;

		GIFRegZBUF zbuf;
		zbuf.U64 = 0;
		zbuf.ZBP = 256;
		zbuf.PSM = PSMZ32;

		return s_mem->GetPixelOffset4(frame, zbuf);
	}

	static u32 PixelAddr(u32 psm, int x, int y)
	{
		return GSLocalMemory::m_psm[psm].info.pa(x, y, 0, 1);
	}

	// Runs one 4-pixel scanline at (0..3, 0) through a freshly compiled
	// scanline function. fm_value is the frame write mask in the frame
	// format's in-memory shape (GSRendererSW projects FBMSK for 16-bit).
	static void RunScanline(GSScanlineSelector sel, const GSPixelOffset4* off, u32 fm_value)
	{
		DrawScanlinePtr fn = Compile(sel);
		ASSERT_NE(fn, nullptr);

		// Value-init keeps the constant tables the ARM64 generator loads from
		// global data (a memset would wipe their default initializers).
		alignas(32) GSScanlineGlobalData global{};
		alignas(32) GSScanlineLocalData local = {};

		global.sel = sel;
		global.vm = s_mem->vm8();
		global.fzbr = off->row;
		global.fzbc = off->col;
		global.fm = GSVector4i(static_cast<int>(fm_value));
		global.zm = GSVector4i(static_cast<int>(0xffffffffu));

		local.gd = &global;
		// Flat colour as CSetupPrim leaves it for TFX_NONE: r|b and g|a pairs.
		local.c.rb = GSVector4i(static_cast<int>(kDrawR | (kDrawB << 16)));
		local.c.ga = GSVector4i(static_cast<int>(kDrawG | (kDrawA << 16)));

		const GSVertexSW scan = GSVertexSW::zero();
		fn(4, 0, 0, scan, local);
	}

	static u8* s_code;
	static size_t s_code_used;
	static GSLocalMemory* s_mem;
};

u8* SwScanlineDateTest::s_code = nullptr;
size_t SwScanlineDateTest::s_code_used = 0;
GSLocalMemory* SwScanlineDateTest::s_mem = nullptr;

// At 32 bits per pixel the destination alpha bit is bit 31. DATM=0 writes
// where it is clear, DATM=1 where it is set; untouched pixels keep their
// seeds exactly (console-measured, all 22 DATE cases of the capture).
TEST_F(SwScanlineDateTest, Date32WritesExactlyTheDatmSelectedPixels)
{
	const GSPixelOffset4* off = GetOffsets(PSMCT32);
	u32* vm32 = s_mem->vm32();

	const u32 seed[4] = {0x80001111, 0x00002222, 0x80003333, 0x00004444};

	for (int datm = 0; datm <= 1; datm++)
	{
		for (int x = 0; x < 4; x++)
			vm32[PixelAddr(PSMCT32, x, 0)] = seed[x];

		RunScanline(MakeDateSelector(0, datm != 0), off, 0);

		for (int x = 0; x < 4; x++)
		{
			const bool dest_alpha_set = (seed[x] & 0x80000000u) != 0;
			const bool expect_write = (datm != 0) ? dest_alpha_set : !dest_alpha_set;
			EXPECT_EQ(vm32[PixelAddr(PSMCT32, x, 0)], expect_write ? kWritten32 : seed[x])
				<< "x=" << x << " datm=" << datm;
		}
	}
}

// At 16 bits per pixel the destination alpha bit is bit 15. Console-measured:
// DATM=1 writes exactly the pixels whose bit 15 is set -- the configuration
// the ARM64 generator failed everything on (the SW renderer's one miss in the
// gs-test capture's 663 cases).
TEST_F(SwScanlineDateTest, Date16WritesExactlyTheDatmSelectedPixels)
{
	const GSPixelOffset4* off = GetOffsets(PSMCT16);
	u16* vm16 = s_mem->vm16();

	// Alpha bit alternates: set, clear, set, clear. The low bits differ per
	// pixel so a write landing on the wrong pixel is also caught.
	const u16 seed[4] = {0x8111, 0x0222, 0x8333, 0x0444};

	// FBMSK 0 projected to the 5551 in-memory shape (GSRendererSW keeps the
	// unused upper half masked).
	const u32 fm16 = 0xffff0000;

	for (int datm = 0; datm <= 1; datm++)
	{
		for (int x = 0; x < 4; x++)
			vm16[PixelAddr(PSMCT16, x, 0)] = seed[x];

		RunScanline(MakeDateSelector(2, datm != 0), off, fm16);

		for (int x = 0; x < 4; x++)
		{
			const bool dest_alpha_set = (seed[x] & 0x8000) != 0;
			const bool expect_write = (datm != 0) ? dest_alpha_set : !dest_alpha_set;
			EXPECT_EQ(vm16[PixelAddr(PSMCT16, x, 0)], expect_write ? kWritten16 : seed[x])
				<< "x=" << x << " datm=" << datm;
		}
	}
}

} // namespace

#endif // ARCH_ARM64
