// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0

#include "GS/Renderers/SW/GSSetupPrimCodeGenerator.arm64.h"
#include "GS/Renderers/SW/GSBlockWalk.h"
#include "GS/Renderers/SW/GSVertexSW.h"

#include "common/StringUtil.h"
#include "common/Perf.h"

#include <cstdint>

// On iOS dual-map JIT, write through the RW alias (rx + g_code_rw_offset).
// Identity no-op elsewhere. Mirrors armGetWritableCodePtr in pcsx2/arm64/AsmHelpers.cpp.
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

	// GSVector4 t = dscan.t;

	armAsm->Ldr(v0, MemOperand(_dscan, offsetof(GSVertexSW, t)));
	armAsm->Fmul(v1.V4S(), v0.V4S(), v3.V4S());

	// The coordinate a triangle samples at trails the exact plane in the direction
	// the walk is going, by less than a sixteenth of a texel. Console-measured; the
	// reasoning is on CSetupPrim in GSDrawScanline.cpp. Sprites take nothing.
	//
	// A float compare against zero leaves all-ones -- integer -1 -- in the lanes
	// that walk forward, so negating it gives the one unit the scanline subtracts
	// and leaves the still and backward axes at zero.
	if (m_sel.prim != GS_SPRITE_CLASS)
	{
		armAsm->Dup(_vscratch.V4S(), v0.V4S(), 0);
		armAsm->Fcmgt(_vscratch.V4S(), _vscratch.V4S(), 0.0);
		armAsm->Neg(_vscratch.V4S(), _vscratch.V4S());
		armAsm->Str(_vscratch, _local(tclag.u));

		armAsm->Dup(_vscratch.V4S(), v0.V4S(), 1);
		armAsm->Fcmgt(_vscratch.V4S(), _vscratch.V4S(), 0.0);
		armAsm->Neg(_vscratch.V4S(), _vscratch.V4S());
		armAsm->Str(_vscratch, _local(tclag.v));
	}

	// The multiply above is by m_shift[0], four pixels -- one VECTOR, deliberately
	// not one block. Colour and fog take the eight-wide block, in the tables
	// GSDrawScanline::SetupColourWalkTables builds; the coordinate does not, for
	// the reason GSBlockWalk.h gives, and the pin is
	// TheCoordinateStepStaysOneVector. Taking the block step here would advance
	// the coordinate eight pixels every four.

	if (m_sel.fst)
	{
		// m_local.d4.stq = GSVector4i(t * 4.0f);
		//
		// Truncating the step and accumulating it is the hardware's shape rather
		// than a lossy stand-in for an exact plane. It is identity on a gradient
		// that is a power of two per pixel, which is every sprite gradient any
		// capture we own draws.
		armAsm->Fcvtzs(v1.V4S(), v1.V4S());
		armAsm->Str(v1, MemOperand(_locals, offsetof(GSScanlineLocalData, d4.stq)));
	}
	else
	{
		// m_local.d4.stq = t * 4.0f;
		armAsm->Str(v1, MemOperand(_locals, offsetof(GSScanlineLocalData, d4.stq)));
	}

	for (int j = 0, k = m_sel.fst ? 2 : 3; j < k; j++)
	{
		// GSVector4 ds = t.xxxx();
		// GSVector4 dt = t.yyyy();
		// GSVector4 dq = t.zzzz();

		armAsm->Dup(v1.V4S(), v0.V4S(), j);

		for (int i = 0; i < (m_sel.notest ? 1 : 4); i++)
		{
			// GSVector4 v = ds/dt * m_shift[i];

			armAsm->Fmul(v2.V4S(), v1.V4S(), VRegister(4 + i, 128, 4));

			if (m_sel.fst)
			{
				// m_local.d[i].s/t = GSVector4i(v);

				armAsm->Fcvtzs(v2.V4S(), v2.V4S());

				switch (j)
				{
					case 0: armAsm->Str(v2, _local(d[i].s)); break;
					case 1: armAsm->Str(v2, _local(d[i].t)); break;
				}
			}
			else
			{
				// m_local.d[i].s/t/q = v;

				switch (j)
				{
					case 0: armAsm->Str(v2, _local(d[i].s)); break;
					case 1: armAsm->Str(v2, _local(d[i].t)); break;
					case 2: armAsm->Str(v2, _local(d[i].q)); break;
				}
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
