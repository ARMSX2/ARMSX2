// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0

#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSBlockWalk.h"
#include "GS/Renderers/SW/GSCoordinateWalk.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include "common/StringUtil.h"
#include "common/Perf.h"

#include <cstdint>

// On iOS dual-map JIT, write through the RW alias (rx + g_code_rw_offset).
// Identity no-op elsewhere. Mirrors armGetWritableCodePtr in pcsx2/arm64/AsmHelpers.cpp.
// TargetConditionals.h defines TARGET_OS_IPHONE; without it the gate below reads
// as false on iOS and the RW-alias write path silently never engages.
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
#include "common/Darwin/DarwinMisc.h"
static void* gsGetWritableCodePtr(void* rx_ptr)
{
	return static_cast<u8*>(rx_ptr) + DarwinMisc::g_code_rw_offset;
}
#else
static void* gsGetWritableCodePtr(void* rx_ptr) { return rx_ptr; }
#endif

MULTI_ISA_UNSHARED_IMPL;

using namespace vixl::aarch64;

static const auto& _vertex = x0;
static const auto& _index = x1;
static const auto& _dscan = x2;
static const auto& _locals = x3;
static const auto& _scratchaddr = x7;
static const auto& _vscratch = v31;

static constexpr const GSScanlineConstantData128B& g_const = g_const_128b;

// Yay, you can't offsetof with non-constant array indices in GCC
#define OFFSETOF(base, field) (reinterpret_cast<uptr>(&reinterpret_cast<base*>(0)->field))
#define _local(field) MemOperand(_locals, OFFSETOF(GSScanlineLocalData, field))
#define armAsm (&m_emitter)

GSSetupPrimCodeGenerator::GSSetupPrimCodeGenerator(u64 key, void* code, size_t maxsize)
	: m_emitter(static_cast<vixl::byte*>(gsGetWritableCodePtr(code)), maxsize, vixl::aarch64::PositionDependentCode)
	, m_sel(key)
	, m_code_rx(static_cast<const u8*>(code))
{
	m_en.z = m_sel.zb ? 1 : 0;
	m_en.f = m_sel.fb && m_sel.fge ? 1 : 0;
	m_en.t = m_sel.fb && m_sel.tfx != TFX_NONE ? 1 : 0;
	m_en.c = m_sel.fb && !(m_sel.tfx == TFX_DECAL && m_sel.tcc) ? 1 : 0;
}

void GSSetupPrimCodeGenerator::Generate()
{
	// Colour and fog no longer need the shift tables: their lane offsets and
	// their steps are built by GSDrawScanline::SetupColourWalkTables, which the
	// rasterizer calls right after this code runs. Depth and the texture
	// coordinate still walk one vector at a time and still need them.
	const bool needs_shift = (m_en.z && m_sel.prim != GS_SPRITE_CLASS) || m_en.t;
	if (needs_shift)
	{
		armAsm->Mov(x4, reinterpret_cast<intptr_t>(g_const.m_shift));
		for (int i = 0; i < (m_sel.notest ? 2 : 5); i++)
		{
			armAsm->Ldr(VRegister(3 + i, kFormat16B), MemOperand(x4, i * sizeof(g_const.m_shift[0])));
		}
	}

	Depth();

	Texture();

	Color();

	armAsm->Ret();

	armAsm->FinalizeCode();

	Perf::any.RegisterKey(GetCode(), GetSize(), "GSSetupPrim_", m_sel.key);
}

void GSSetupPrimCodeGenerator::Depth()
{
	if (!m_en.z && !m_en.f)
	{
		return;
	}

	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		// Fog's lane table and step used to be built here. They come from the
		// primitive's colour walk now, like colour's -- see GSColourWalk.h.

		if (m_en.z)
		{
			// VectorF dz = VectorF::broadcast64(&dscan.p.z)
			armAsm->Add(_scratchaddr, _dscan, offsetof(GSVertexSW, p.z));
			armAsm->Ld1r(_vscratch.V2D(), MemOperand(_scratchaddr));

			// m_local.d4.z = dz.mul64(GSVector4::f32to64(shift));
			armAsm->Fcvtl(v1.V2D(), v3.V2S());
			armAsm->Fmul(v1.V2D(), v1.V2D(), _vscratch.V2D());
			armAsm->Str(v1.V2D(), _local(d4.z));

			armAsm->Fcvtn(v0.V2S(), _vscratch.V2D());
			armAsm->Fcvtn2(v0.V4S(), _vscratch.V2D());

			for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
			{
				// m_local.d[i].z0 = dz.mul64(VectorF::f32to64(half_shift[2 * i + 2]));
				// m_local.d[i].z1 = dz.mul64(VectorF::f32to64(half_shift[2 * i + 3]));

				armAsm->Fmul(v1.V4S(), v0.V4S(), VRegister(4 + i, kFormat4S));
				armAsm->Str(v1.V4S(), _local(d[i].z));
			}
		}
	}
	else
	{
		// GSVector4 p = vertex[index[1]].p;

		armAsm->Ldrh(w4, MemOperand(_index, sizeof(u16)));
		armAsm->Lsl(w4, w4, 6); // * sizeof(GSVertexSW)
		armAsm->Add(x4, _vertex, x4);

		if (m_en.f)
		{
			// m_local.p.f = GSVector4i(p).zzzzh().zzzz();

			armAsm->Ldr(v0, MemOperand(x4, offsetof(GSVertexSW, p)));

			armAsm->Fcvtzs(v1.V4S(), v0.V4S());
			armAsm->Dup(v1.V8H(), v1.V8H(), 6);

			armAsm->Str(v1, MemOperand(_locals, offsetof(GSScanlineLocalData, p.f)));
		}

		if (m_en.z)
		{
			// uint32 z is bypassed in t.w

			armAsm->Add(_scratchaddr, x4, offsetof(GSVertexSW, t.w));
			armAsm->Ld1r(v0.V4S(), MemOperand(_scratchaddr));
			armAsm->Str(v0, MemOperand(_locals, offsetof(GSScanlineLocalData, p.z)));
		}
	}
}

void GSSetupPrimCodeGenerator::Texture()
{
	if (!m_en.t)
	{
		return;
	}

	// Twice the triangle's area, in 12.4 units squared, as an exact integer, and
	// from it the mask the lag is gated on: all-ones where the trail is taken, zero
	// where the setup inverts exactly. GSCoordinateWalk.h carries the reading.
	//
	// The words are the vertices' own: the position lane is the 12.4 word over
	// sixteen, so FCVTZS at four fractional bits recovers it, and the cross runs in
	// integers from there. Nothing here is formed in floating point, and the C++
	// reference forms the identical integer the identical way.
	//
	// Lines and points have no area to ask about and keep the lag they always had.
	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		if (m_sel.prim == GS_TRIANGLE_CLASS)
		{
			const VRegister vp[3] = {d0, d1, d2};

			for (int k = 0; k < 3; k++)
			{
				armAsm->Ldrh(w4, MemOperand(_index, sizeof(u16) * k));
				armAsm->Lsl(w4, w4, 6); // * sizeof(GSVertexSW)
				armAsm->Add(x4, _vertex, x4);
				armAsm->Ldr(vp[k], MemOperand(x4, offsetof(GSVertexSW, p)));
				armAsm->Fcvtzs(vp[k].V2S(), vp[k].V2S(), 4);
			}

			// d1 = p1 - p0, d2 = p2 - p0, then the cross as (d1.x*d2.y, d1.y*d2.x)
			// and the difference of the pair.
			armAsm->Sub(v1.V2S(), v1.V2S(), v0.V2S());
			armAsm->Sub(v2.V2S(), v2.V2S(), v0.V2S());
			armAsm->Rev64(v2.V2S(), v2.V2S());
			armAsm->Smull(v0.V2D(), v1.V2S(), v2.V2S());
			armAsm->Ext(v1.V16B(), v0.V16B(), v0.V16B(), 8);
			armAsm->Sub(v0.V2D(), v0.V2D(), v1.V2D());
			armAsm->Abs(v0.V2D(), v0.V2D());
			armAsm->Fmov(x4, d0);

			// A power of two clears every bit below its own, and zero is not one.
			armAsm->Sub(x5, x4, 1);
			armAsm->Tst(x4, x5);
			armAsm->Cset(w5, eq);
			armAsm->Cmp(x4, 0);
			armAsm->Cset(w6, ne);
			armAsm->And(w5, w5, w6);
			armAsm->Cmp(w5, 0);
			armAsm->Csetm(w6, eq);
		}
		else
		{
			armAsm->Mov(w6, -1);
		}
	}

	// GSVector4 t = dscan.t;

	armAsm->Ldr(v0, MemOperand(_dscan, offsetof(GSVertexSW, t)));

	// The coordinate a triangle samples at trails the exact plane in the direction
	// the walk is going, by less than a sixteenth of a texel. Console-measured; the
	// reasoning is on CSetupPrim in GSDrawScanline.cpp. Sprites take nothing.
	//
	// A compare against zero leaves all-ones -- integer -1 -- in the lanes that
	// walk forward, so negating it gives the one unit the scanline subtracts and
	// leaves the still and backward axes at zero. The compare is on the step the
	// walk actually takes, which on the affine route is the FLOORED one: a
	// gradient below a grid unit per pixel walks nowhere, and a still coordinate
	// does not trail.
	//

	if (m_sel.uvwalk)
	{
		// The console's texel accumulator is seed + n * floor(step) on a 12.15
		// grid -- GSCoordinateWalk.h. So the per-pixel step is floored ONCE,
		// here, and every lane and vector offset below is an integer multiple of
		// it. Truncating each product instead is floor(n * step), a different
		// sequence, and it is the one that disagrees with the console.
		//
		// FCVTMS rounds toward minus infinity, which is what dropping the bits
		// below a fixed-point register does; the BIC puts the result on the
		// eleven-bits-below-the-sixteenth grid the width fit pinned.

		// GSVector4i step = GSVector4i(t.floor()) & GS_UV_GRID_MASK;
		armAsm->Fcvtms(v1.V4S(), v0.V4S());
		armAsm->Bic(v1.V4S(), (1 << GS_UV_GRID_SHIFT) - 1, 0);

		if (m_sel.prim != GS_SPRITE_CLASS)
		{
			// v0 held dscan.t and is spent: the step lives in v1 from here on, and
			// w6 carries the area gate from the top of this function.
			armAsm->Dup(v0.V4S(), w6);

			for (int j = 0; j < 2; j++)
			{
				armAsm->Dup(_vscratch.V4S(), v1.V4S(), j);
				armAsm->Cmgt(_vscratch.V4S(), _vscratch.V4S(), 0);
				armAsm->And(_vscratch.V16B(), _vscratch.V16B(), v0.V16B());
				armAsm->Neg(_vscratch.V4S(), _vscratch.V4S());
				armAsm->Str(_vscratch, j == 0 ? _local(tclag.u) : _local(tclag.v));
			}
		}

		// m_local.d4.stq = step * 4;
		armAsm->Shl(v2.V4S(), v1.V4S(), 2);
		armAsm->Str(v2, MemOperand(_locals, offsetof(GSScanlineLocalData, d4.stq)));

		armAsm->Mov(_scratchaddr, reinterpret_cast<intptr_t>(g_const.m_lane));

		for (int j = 0; j < 2; j++)
		{
			// GSVector4i ds = step.xxxx();
			// GSVector4i dt = step.yyyy();

			armAsm->Dup(v2.V4S(), v1.V4S(), j);

			for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
			{
				// m_local.d[i].s/t = ds/dt * m_lane[i];

				armAsm->Ldr(_vscratch, MemOperand(_scratchaddr, i * sizeof(g_const.m_lane[0])));
				armAsm->Mul(v0.V4S(), v2.V4S(), _vscratch.V4S());

				switch (j)
				{
					case 0: armAsm->Str(v0, _local(d[i].s)); break;
					case 1: armAsm->Str(v0, _local(d[i].t)); break;
				}
			}
		}

		return;
	}

	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		// A constant-Q triangle's own ST plane and a live divide take the same
		// rule: a live divide at a power-of-two area does not trail either. v0
		// keeps dscan.t for the step below; v1 carries the area gate from the top
		// of this function.
		armAsm->Dup(v1.V4S(), w6);

		for (int j = 0; j < 2; j++)
		{
			armAsm->Dup(_vscratch.V4S(), v0.V4S(), j);
			armAsm->Fcmgt(_vscratch.V4S(), _vscratch.V4S(), 0.0);
			armAsm->And(_vscratch.V16B(), _vscratch.V16B(), v1.V16B());
			armAsm->Neg(_vscratch.V4S(), _vscratch.V4S());
			armAsm->Str(_vscratch, j == 0 ? _local(tclag.u) : _local(tclag.v));
		}
	}

	// The multiply is by m_shift[0], four pixels -- one VECTOR, deliberately not
	// one block. Colour and fog take the eight-wide block, in the tables
	// GSDrawScanline::SetupColourWalkTables builds; the coordinate does not, for
	// the reason GSBlockWalk.h gives, and the pin is
	// TheCoordinateStepStaysOneVector. Taking the block step here would advance
	// the coordinate eight pixels every four.

	// A constant-Q triangle's ST plane arrives as a 16.16 integer too and the
	// scanline reads it the same way, but it does not take the accumulator's grid
	// -- the console refuses it there. So this road keeps the float step it had.
	//
	// m_local.d4.stq = GSVector4i(t * 4.0f) or t * 4.0f;
	armAsm->Fmul(v1.V4S(), v0.V4S(), v3.V4S());

	if (m_sel.fst)
		armAsm->Fcvtzs(v1.V4S(), v1.V4S());

	armAsm->Str(v1, MemOperand(_locals, offsetof(GSScanlineLocalData, d4.stq)));

	for (int j = 0, k = m_sel.fst ? 2 : 3; j < k; j++)
	{
		// GSVector4 ds = t.xxxx();
		// GSVector4 dt = t.yyyy();
		// GSVector4 dq = t.zzzz();

		armAsm->Dup(v1.V4S(), v0.V4S(), j);

		for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
		{
			// m_local.d[i].s/t/q = ds/dt/dq * m_shift[i];

			armAsm->Fmul(v2.V4S(), v1.V4S(), VRegister(4 + i, 128, 4));

			if (m_sel.fst)
				armAsm->Fcvtzs(v2.V4S(), v2.V4S());

			switch (j)
			{
				case 0: armAsm->Str(v2, _local(d[i].s)); break;
				case 1: armAsm->Str(v2, _local(d[i].t)); break;
				case 2: armAsm->Str(v2, _local(d[i].q)); break;
			}
		}
	}
}

void GSSetupPrimCodeGenerator::Color()
{
	if (!m_en.c)
	{
		return;
	}

	if (m_sel.iip)
	{
		// A gouraud primitive's colour lane table and step come from the walk the
		// setup decided for it, built by GSDrawScanline::SetupColourWalkTables
		// which the rasterizer calls right after this code runs. See
		// GSColourWalk.h for the model.
		return;
	}

	{
		// GSVector4i c = GSVector4i(vertex[index[last].c);

		int last = 0;

		switch (m_sel.prim)
		{
			case GS_POINT_CLASS:    last = 0; break;
			case GS_LINE_CLASS:     last = 1; break;
			case GS_TRIANGLE_CLASS: last = 2; break;
			case GS_SPRITE_CLASS:   last = 1; break;
		}

		if (!(m_sel.prim == GS_SPRITE_CLASS && (m_en.z || m_en.f))) // if this is a sprite, the last vertex was already loaded in Depth()
		{
			armAsm->Ldrh(w4, MemOperand(_index, sizeof(u16) * last));
			armAsm->Lsl(w4, w4, 6); // * sizeof(GSVertexSW)
			armAsm->Add(x4, _vertex, x4);
		}

		armAsm->Ldr(v0, MemOperand(x4, offsetof(GSVertexSW, c)));
		armAsm->Fcvtzs(v0.V4S(), v0.V4S());

		// c = c.upl16(c.zwxy());

		armAsm->Ext(v1.V16B(), v0.V16B(), v0.V16B(), 8);
		armAsm->Zip1(v0.V8H(), v0.V8H(), v1.V8H());

		// if (!tme) c = c.srl16(7);

		if (m_sel.tfx == TFX_NONE)
			armAsm->Ushr(v0.V8H(), v0.V8H(), 7);

		// m_local.c.rb = c.xxxx();
		// m_local.c.ga = c.zzzz();

		armAsm->Dup(v1.V4S(), v0.V4S(), 0);
		armAsm->Dup(v2.V4S(), v0.V4S(), 2);

		armAsm->Str(v1, _local(c.rb));
		armAsm->Str(v2, _local(c.ga));
	}
}
