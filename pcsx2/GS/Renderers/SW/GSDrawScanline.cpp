// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSLevelOfDetail.h"
#include "GS/Renderers/SW/GSTextureCacheSW.h"
#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSBlockWalk.h"
#include "GS/Renderers/SW/GSCoordinateWalk.h"
#include "GS/Renderers/SW/GSRasterizer.h"
#include "Memory.h"

#include "common/Console.h"

#include <fstream>

// Comment to disable all dynamic code generation.
#define ENABLE_JIT_RASTERIZER

#if MULTI_ISA_COMPILE_ONCE
// Lack of a better home
constexpr GSScanlineConstantData256B g_const_256b;
constexpr GSScanlineConstantData128B g_const_128b;
#endif

MULTI_ISA_UNSHARED_IMPL;

static __forceinline const GSScanlineGlobalData& GlobalFromLocal(const GSScanlineLocalData& local)
{
	return *local.gd;
}

GSDrawScanline::GSDrawScanline()
	: m_sp_map("GSSetupPrim")
	, m_ds_map("GSDrawScanline")
{
	if (SysMemory::HasCodeMemory())
		GSCodeReserve::ResetMemory();
}

GSDrawScanline::~GSDrawScanline()
{
	if (SysMemory::HasCodeMemory())
	{
		if (const size_t used = GSCodeReserve::GetMemoryUsed(); used > 0)
			DevCon.WriteLn("SW JIT generated %zu bytes of code", used);
	}
}

bool GSDrawScanline::ShouldUseCDrawScanline(u64 key)
{
	static std::map<u64, bool> s_use_c_draw_scanline;
	static std::mutex s_use_c_draw_scanline_mutex;

	static const char* const fname = getenv("USE_C_DRAW_SCANLINE");
	if (!fname)
		return false;

	std::lock_guard<std::mutex> l(s_use_c_draw_scanline_mutex);

	if (s_use_c_draw_scanline.empty())
	{
		std::ifstream file(fname);
		if (file)
		{
			for (std::string str; std::getline(file, str);)
			{
				u64 key;
				char yn;
				if (sscanf(str.c_str(), "%" PRIx64 " %c", &key, &yn) == 2)
				{
					if (yn != 'Y' && yn != 'N' && yn != 'y' && yn != 'n')
						Console.Warning("Failed to parse %s: Not y/n", str.c_str());
					s_use_c_draw_scanline[key] = (yn == 'Y' || yn == 'y') ? true : false;
				}
				else
				{
					Console.Warning("Failed to process line %s", str.c_str());
				}
			}
		}
	}

	auto idx = s_use_c_draw_scanline.find(key);
	if (idx == s_use_c_draw_scanline.end())
	{
		s_use_c_draw_scanline[key] = false;
		// Rewrite file
		FILE* file = fopen(fname, "w");
		if (file)
		{
			for (const auto& pair : s_use_c_draw_scanline)
			{
				fprintf(file, "%016" PRIX64 " %c %s\n", pair.first, pair.second ? 'Y' : 'N', GSScanlineSelector(pair.first).to_string().c_str());
			}
			fclose(file);
		}
		else
		{
			Console.Warning("Failed to write C draw scanline usage config: %s", strerror(errno));
		}
		return false;
	}

	return idx->second;
}

void GSDrawScanline::BeginDraw(const GSRasterizerData& data, GSScanlineLocalData& local)
{
	const GSScanlineGlobalData& global = data.global;
	local.gd = &global;

	if (global.sel.mmin && global.sel.lcm)
	{
		GSVector4i v = global.t.minmax.srl16(global.lod.i.extract32<0>()); //.x);

		v = v.upl16(v);

		local.temp.uv_minmax[0] = v.upl32(v);
		local.temp.uv_minmax[1] = v.uph32(v);
	}
}

void GSDrawScanline::ResetCodeCache()
{
	if (!SysMemory::HasCodeMemory())
		return;

	Console.Warning("GS Software JIT cache overflow, resetting.");
	m_sp_map.Clear();
	m_ds_map.Clear();
	GSCodeReserve::ResetMemory();
}

bool GSDrawScanline::SetupDraw(GSRasterizerData& data, bool allow_compile)
{
	const GSScanlineGlobalData& global = data.global;

	if (!SysMemory::HasCodeMemory())
	{
		data.setup_prim = &GSDrawScanline::CSetupPrim;
		data.draw_scanline = &GSDrawScanline::CDrawScanline;
		data.draw_edge = global.sel.aa1 ? &GSDrawScanline::CDrawEdge : nullptr;
		return true;
	}

#ifdef ENABLE_JIT_RASTERIZER
	data.draw_scanline = m_ds_map.Lookup(global.sel, allow_compile);
	if (!data.draw_scanline) [[unlikely]]
		return false;

	if (global.sel.aa1)
	{
		GSScanlineSelector sel;

		sel.key = global.sel.key;
		sel.zwrite = 0;
		sel.edge = 1;

		data.draw_edge = m_ds_map.Lookup(sel, allow_compile);
		if (!data.draw_edge) [[unlikely]]
			return false;
	}
	else
	{
		data.draw_edge = nullptr;
	}

	// doesn't need all bits => less functions generated

	GSScanlineSelector sel;

	sel.key = 0;

	sel.iip = global.sel.iip;
	sel.tfx = global.sel.tfx;
	sel.tcc = global.sel.tcc;
	sel.fst = global.sel.fst;
	sel.uvwalk = global.sel.uvwalk;
	sel.fge = global.sel.fge;
	sel.prim = global.sel.prim;
	sel.fb = global.sel.fb;
	sel.zb = global.sel.zb;
	sel.zoverflow = global.sel.zoverflow;
	sel.zequal = global.sel.zequal;
	sel.notest = global.sel.notest;

	return (data.setup_prim = m_sp_map.Lookup(sel, allow_compile)) != nullptr;
#else
	data.setup_prim = &GSDrawScanline::CSetupPrim;
	data.draw_scanline = &GSDrawScanline::CDrawScanline;
	data.draw_edge = global.sel.aa1 ? &GSDrawScanline::CDrawEdge : nullptr;
	return true;
#endif
}

void GSDrawScanline::UpdateDrawStats(u64 frame, u64 ticks, int actual, int total, int prims)
{
	m_ds_map.UpdateStats(frame, ticks, actual, total, prims);
}

void GSDrawScanline::PrintStats()
{
	m_ds_map.PrintStats();
}

#if _M_SSE >= 0x501
typedef GSVector8i VectorI;
typedef GSVector8  VectorF;
#define LOCAL_STEP local.d8
#else
typedef GSVector4i VectorI;
typedef GSVector4  VectorF;
#define LOCAL_STEP local.d4
#endif

// The sixteenth-of-a-texel index that the sampler splits into a texel and a
// filter weight is formed by TRUNCATING the coordinate toward zero. An
// arithmetic shift floors instead, which is the same function on a non-negative
// coordinate and one sixteenth lower on a negative one.
//
// Measured on real hardware rather than assumed, with a texture that names the
// console's own reading -- red alternating by V parity and green by U parity, so
// a bilinear blend returns the weight itself while blue and alpha name the texel
// pair. One stored word therefore fixes the texel and the weight outright, with
// no model of either renderer's walk in between. Every arm whose U stays positive
// reads our own coordinate; the two whose U is negative read one sixteenth higher,
// and applying the same displacement WITHOUT the sign test destroys the positive
// arms. The sign is the whole rule rather than a detail of it.
//
// Both halves of the split read bit 12 and up -- the texel index is the
// arithmetic shift by sixteen, the weight is bits 12 to 15 -- so adding a
// sixteenth less one to a negative coordinate turns the floor that follows into
// the truncation, and can disturb nothing else.
//
// Two things here are the model written down rather than measured. V: every arm
// held V positive, so V is covered because both axes go through this one
// expression, which is a fact about our code and not about silicon. And the ORDER
// against the linear half-texel bias -- the two readings differ only for a
// coordinate inside the first half texel, which nothing drew. The truncation is
// taken BEFORE the bias: the console forms its sixteenth on the coordinate, and
// the half-texel straddle is our own sampler's step onto the tap pair, not part
// of that coordinate. The DDA's lag goes the other way and is taken BEFORE the
// truncation, because the lag IS the console's own walk trailing the exact plane;
// nothing measured separates the lag's two orders.
static __forceinline VectorI GSTruncateCoordinate(const VectorI& c)
{
	return c + c.sra32<31>().srl32<20>();
}

// The formed coordinate lives in a SIGNED 12.4 FIELD and saturates into it, so a
// coordinate at or above 2,047.9375 texels samples texel 2,047 and one at or below
// -2,048 samples texel -2,048. Measured on real hardware; GSCoordinateWalk.h
// carries the reading, the families it refutes, and what it does not pin.
//
// Written as the field it is: the sixteenth, saturated to sixteen signed bits, put
// back. The bits below the sixteenth go with it, which nothing downstream reads --
// the texel is bits 16 and up and the weight bits 12 to 15. The ARM64 generator
// emits this same shape, one Sqxtn, so the two roads cannot drift apart on it.
//
// It sits after the truncation to sixteenths and after the linear filter's
// half-texel step, because the console's reading is the field's top value and not
// half a texel below it, and before the tap pair and the wrap addressing.
static __forceinline VectorI GSSaturateCoordinate(const VectorI& c)
{
	return c.sra32<GS_COORD_SIXTEENTH_SHIFT>()
	    .max_i32(VectorI(GS_COORD_SIXTEENTH_MIN))
	    .min_i32(VectorI(GS_COORD_SIXTEENTH_MAX))
	    .sll32<GS_COORD_SIXTEENTH_SHIFT>();
}

// The GS does not divide the texture coordinate by Q. It multiplies by a
// RECIPROCAL that is truncated to about thirteen fractional bits, so a
// perspective coordinate is systematically a little short of the true quotient.
//
// Measured on real hardware rather than assumed. Every reading bounds the
// hardware's own reciprocal from both sides, and the ones that bind all force it
// strictly BELOW the true value; not one forces it above. So the grid truncates,
// and computing an exact quotient -- what we did before the grid landed -- is
// being MORE correct than the hardware, and differs from the console on about a
// fifth of ordinary perspective readings for that reason alone.
//
// The WIDTH of the grid took a second pass to get right. Thirteen mantissa bits
// fits the bound above, but it is not what the console keeps. A band walking a
// constant quotient of 8193/16384 across 512 pixels -- a texel boundary plus a
// sliver -- reads the same 512 sixteenths on the console, and a thirteen-bit grid
// trails far enough at the end of each reciprocal plateau to drop a quarter of
// them a sixteenth low. Fourteen bits clears all 512, and so does anything wider:
// 14 is the narrowest grid the measurements permit, not a fitted value, and
// nothing we hold separates it from wider.
//
// float32 carries 23 explicit mantissa bits, so clearing the low nine leaves
// fourteen and rounds toward zero, which is the side silicon is never on the
// wrong side of.
//
// ⚠️ THIS IS AN APPROXIMATION, and knowing which part is approximate matters if
// you are the one who improves it. The truncation and its width are measured.
// The SHAPE is not: this evaluates a plane and divides, and the console walks
// the coordinate instead. Geometry that visits the identical set of Q values at
// four different per-pixel steps must read the same level at all four if the
// result is a function of Q -- every plane-with-a-reciprocal candidate does, and
// the console does not. A second construction with nothing in common says the
// same: pin the exact coordinate at every rung of a step ladder and the console's
// hit rate still halves as the Q step doubles.
//
// So the right shape is a fixed-point walk of S and Q, and nobody has fitted it
// yet -- the two-grid families that have been swept are refuted by the same
// baseline movement that refutes the plane. Until that lands, this is the best
// approximation measured: it improved every capture and every console frame it
// was scored on, which is the only claim being made for it.
__forceinline static VectorF GSPerspectiveRecip(const VectorF& q)
{
	return VectorF::cast(VectorI::cast(VectorF(1.0f) / q) & VectorI(0xfffffe00));
}

// The texture function multiplies the eight-bit vertex colour the GS STORES, not
// the wider value its interpolator carries. Ours is fixed point with seven
// fractional bits, and feeding all fifteen to the multiply is wrong by up to one
// unit wherever the colour has a fraction -- which is everywhere on a gouraud
// gradient, and invisible to a flat-shaded corpus.
//
// Measured on real hardware with no model of either interpolator: the same colour
// read back through four different multipliers brackets the value the hardware
// holds, and that bracket excludes the product of the stored byte on 0 of 24,576
// readings, where our own arms exclude it on about one reading in five.
//
// Drop the fraction for the multiply only. The DDA keeps it, or the gradient
// stops stepping; the byte goes back on the seven-bit grid so modulate16<1> still
// lines up.
/// The walk's carried colour, as the byte the GS stores.
///
/// The lane is signed 16-bit 8.7 fixed point and the walk is allowed to carry a
/// negative value -- clamping what it CARRIES was tried and the console refused
/// it. What the console does not tolerate is a negative reaching the STORE: a
/// plain logical shift turns -1 into 511, which saturates to white, and a frame
/// with an additively accumulated untextured gouraud strip in it shows dozens of
/// channels driven to exactly 255 against a console that leaves the destination
/// unchanged.
///
/// So the saturation goes here, at the pack, and the walk is untouched. The
/// max-with-zero is exact rather than merely safe: it and a plain shift differ
/// only on negative inputs, where the answer the model asks for is 0, and the top
/// needs no clamp because 32767 >> 7 is already 255.
__forceinline static VectorI GSWalkColorByte(const VectorI& c)
{
	return c.max_i16(VectorI::zero()).srl16<7>();
}

__forceinline static VectorI GSStoredVertexColor(const VectorI& c)
{
	return GSWalkColorByte(c).sll16<7>();
}

void GSDrawScanline::SetupColourWalkTables(GSScanlineLocalData& local, int y)
{
	// The scanline's colour and fog tables, built from the primitive's own walk.
	//
	// GSColourWalk.h: measured from an absolute base that is a multiple of eight,
	// lane i sits off[i] = i*gc + dw*floor((i - phase) / W) from it, and eight
	// pixels advance by 8g whether that is one block or two. A span seeded at
	// `left` starts s = left & 7 lanes in, so the lane-init entry for s holds
	// off[(s & ~(vlen-1)) + l] - off[s] in lane l, and
	//
	//     off[i] - off[s] = gc*(i - s) + dw*(floor((i - phase)/W) - floor((s - phase)/W))
	//
	// which is a broadcast gradient times a lane vector -- the same shape the
	// shift tables had, with the block index in place of the vector offset. The
	// floor division is an arithmetic shift because W is a power of two, and at
	// W = 4 the whole table repeats with period four, so the eight entries serve
	// a four-wide block as well as an eight-wide one.
	//
	// The packing is unchanged: r and b interleaved as the two halves of each
	// 32-bit lane, g and a likewise, fog replicated in both halves. The mask has
	// already put every lane in 0..65535, so the unsigned pack is the identity
	// and a descending gradient keeps its negative offset; the signed pack would
	// turn everything from 32768 up into garbage the whole scanline carries.
	constexpr int vlen = sizeof(VectorF) / sizeof(float);
	constexpr VectorI mask16 = VectorI::cxpr(0xFFFF);

	const GSColourWalk& w = local.cwalk;

	if (!w.live)
	{
		// Lines, points, sprites and the AA1 edge pass have no walk of their own,
		// and their dscan is zero anyway. The zeros are the same on every row, so
		// the first row of the primitive writes them and the rest find them.
		if (w.tables.state == GSColourWalkTablesZero)
			return;

		local.cwalk.tables.state = GSColourWalkTablesZero;

		for (int s = 0; s < 8; s++)
		{
			local.d[s].rb = VectorI::zero();
			local.d[s].ga = VectorI::zero();
			local.d[s].f = VectorI::zero();
		}

#if _M_SSE >= 0x501
		local.d8.c.rb = 0;
		local.d8.c.ga = 0;
		local.d8.p.f = 0;
#else
		local.d4.c = GSVector4i::zero();
		local.d4.f = GSVector4i::zero();

		for (int s = 0; s < 8; s++)
		{
			for (int ph = 0; ph < 2; ph++)
			{
				local.dw[s][ph].rb = GSVector4i::zero();
				local.dw[s][ph].ga = GSVector4i::zero();
				local.dw[s][ph].f = GSVector4i::zero();
			}
		}
#endif

		return;
	}

	// ⚠️ Built per ROW, not per primitive, and that is the point.
	//
	// The walk is ONE floor at every pixel. Writing the value as its whole terms
	// plus a fraction -- gc and gyc are multiples of EIGHT units so their terms
	// are whole, P sits on the 1/8 grid as P_int + p, and dw*j is D_int + h with
	// h zero or a half -- gives
	//
	//     V(x) = [whole terms] + floor(p + h(x))
	//
	// and a seed floored once at the span's first pixel carries floor(p + h(left))
	// everywhere instead. The difference is inert where dw is whole, which is
	// every eight-wide block, and measurable at four.
	//
	// So the jumps below are floored WITH the row's own fraction in them, which
	// makes the table's offsets the closed form's differences exactly. p is a
	// property of the row pair, so the table follows the row; the alternative --
	// a parity table gated by one bit per row -- keeps the per-primitive build but
	// costs the scanline an extra masked add per vector, and this costs it
	// nothing.
	const int yf = w.top_anchor ? (y & ~1) : (y | 1);
	const GSVector4 rowc = w.c.pa + GSColourWalkTruncUnit(w.c.gy * GSVector4(static_cast<float>(yf) - w.yr));
	const GSVector4 rowf = w.f.pa + GSColourWalkTruncUnit(w.f.gy * GSVector4(static_cast<float>(yf) - w.yr));
	const GSVector4 cfrac = rowc - rowc.floor();
	const GSVector4 ffrac = rowf - rowf.floor();

	// The row reaches the tables below ONLY through these two fractions, so a row
	// that repeats them wants the bytes that are already there. It repeats them
	// whenever the row pair does -- yf is the pair's row, so every second row is
	// free -- and it keeps repeating them for as long as the vertical gradient
	// takes to walk a whole colour unit, which on a shallow gradient is many rows.
	// The tables cannot have been left by a different primitive: SetupPrim clears
	// the state, and nothing between two rows of one primitive writes d[] or dw[].
	if (w.tables.state == GSColourWalkTablesBuilt
		&& (w.tables.cfrac == cfrac).alltrue()
		&& (w.tables.ffrac == ffrac).alltrue())
	{
		return;
	}

	local.cwalk.tables.cfrac = cfrac;
	local.cwalk.tables.ffrac = ffrac;
	local.cwalk.tables.state = GSColourWalkTablesBuilt;

	const VectorF gcv(w.c.gc);
	const VectorF dwv(w.c.dw);
	const VectorF pfv(cfrac);

	const VectorF gcr = gcv.xxxx(), gcg = gcv.yyyy(), gcb = gcv.zzzz(), gca = gcv.wwww();
	const VectorF dwr = dwv.xxxx(), dwg = dwv.yyyy(), dwb = dwv.zzzz(), dwa = dwv.wwww();
	const VectorF pfr = pfv.xxxx(), pfg = pfv.yyyy(), pfb = pfv.zzzz(), pfa = pfv.wwww();
	const VectorF fgc(VectorF(w.f.gc).xxxx());
	const VectorF fdw(VectorF(w.f.dw).xxxx());
	const VectorF fpf(VectorF(ffrac).xxxx());

	alignas(32) float kbuf[8];
	alignas(32) float hibuf[8];
	alignas(32) float lobuf[8];

	// How many whole blocks pixel p sits from the anchor's grid origin, signed
	// along the walk -- the same expression the row seed uses for its own jump
	// term, so that the seed and these tables cannot anchor the truncation on
	// opposite parities. The shift is a floor for a negative numerator too, which
	// is what a pixel behind the origin gives.
	const auto blk = [&w](const GSColourWalkGradient& a, int p) {
		return w.d * ((w.d * (p - w.S)) >> a.wshift);
	};

	// One channel's table entry: the ramp, plus the accumulated jump at the far end
	// minus the accumulated jump at the near end. The two jumps are rounded
	// SEPARATELY and then subtracted, because floor(dw*b) is what a whole-unit lane
	// can carry at block b: at W = 4 the jump dw is half a unit, and the successive
	// differences of floor(dw*b) alternate between floor(dw) and ceil(dw) exactly
	// as the value does, and any two of them a block apart sum to 2*dw. Rounding
	// the DIFFERENCE instead would give floor(dw) every time and lose half a unit
	// per block for the length of the span.
	//
	// FLOOR, not the conversion's own truncation toward zero. A block index is
	// negative for a pixel behind the grid's origin, and toward zero the alternation
	// breaks exactly at the sign change -- floor(dw*-1) and trunc(dw*-1) differ,
	// and the walk gains a unit crossing zero.
	const auto entry = [](const VectorF& gc, const VectorF& dw, const VectorF& kv,
						   const VectorF& hi, const VectorF& lo, const VectorF& pf) {
		return VectorI(gc * kv) + (VectorI((dw * hi + pf).floor()) - VectorI((dw * lo + pf).floor()));
	};

	// The colour lane and the fog lane can be walking different block widths, so
	// each one's block indices are built from its own.
	const auto blocks = [&](const GSColourWalkGradient& a, int base, int s, int lanes) {
		const float sblk = static_cast<float>(blk(a, s));
		for (int l = 0; l < lanes; l++)
		{
			hibuf[l] = static_cast<float>(blk(a, base + l));
			lobuf[l] = sblk;
		}
	};
	const auto steps = [&](const GSColourWalkGradient& a, int base) {
		for (int l = 0; l < 4; l++)
		{
			hibuf[l] = static_cast<float>(blk(a, base + 4 + l));
			lobuf[l] = static_cast<float>(blk(a, base + l));
		}
	};
	const auto pack = [&mask16](const VectorI& x, const VectorI& y) {
		return (x & mask16).pu32().upl16((y & mask16).pu32());
	};

	// The lane-init tables: lane l of entry s holds the walk's offset from the
	// span's first pixel to the pixel l lanes into the vector s starts in.
	for (int s = 0; s < 8; s++)
	{
		const int base = s & ~(vlen - 1);

		for (int l = 0; l < vlen; l++)
			kbuf[l] = static_cast<float>(base + l - s);

		const VectorF kv = VectorF::template load<true>(kbuf);

		blocks(w.c, base, s, vlen);
		const VectorF hi = VectorF::template load<true>(hibuf);
		const VectorF lo = VectorF::template load<true>(lobuf);

		local.d[s].rb = pack(entry(gcr, dwr, kv, hi, lo, pfr), entry(gcb, dwb, kv, hi, lo, pfb));
		local.d[s].ga = pack(entry(gcg, dwg, kv, hi, lo, pfg), entry(gca, dwa, kv, hi, lo, pfa));

		blocks(w.f, base, s, vlen);
		const VectorF fhi = VectorF::template load<true>(hibuf);
		const VectorF flo = VectorF::template load<true>(lobuf);

		local.d[s].f = entry(fgc, fdw, kv, fhi, flo, fpf).xxzzlh();
	}

#if _M_SSE >= 0x501
	// One vector is eight pixels here, so the per-vector step is the whole 8g --
	// one block or two, and either way an exact whole number of units.
	GSVector4i::storel(&local.d8.c, (GSVector4i(w.c.g8) & GSVector4i::cxpr(0xFFFF)).xzyw().pu32());
	local.d8.p.f = GSVector4i(w.f.g8).extract32<3>();
#else
	// 4g, for the four-lane x86 SSE4 path, whose generators still step one vector
	// without a block. ARM64 and the C++ reference take the alternating pair below.
	local.d4.c = (GSVector4i(w.c.g * GSVector4::cxpr(4.0f)) & GSVector4i::cxpr(0xFFFF)).xzyw().pu32();
	local.d4.f = GSVector4i(w.f.g * GSVector4::cxpr(4.0f)).zzzzh().wwww();

	// The per-vector step, per starting position: the step out of the vector s
	// starts in is off[base+4+l] - off[base+l] lane by lane, and the one after it
	// is what is left of the eight-pixel step, because eight pixels advance by 8g
	// however the vectors divide them.
	//
	// It has to be computed per s rather than once and chosen by which half of the
	// eight s falls in. That shortcut is right at W = 8, where the pair is the two
	// halves of one block, and wrong at W = 4, where the blocks are aligned to the
	// PHASE and not to the vector: with a phase of 1, x = 2 and x = 4 sit in the
	// same block and must take the same first step, and picking by s < 4 gives
	// them opposite ones.
	//
	// The partner is derived in WHOLE UNITS, so the pair sums to 8g by
	// construction and cannot drift however the halves round.
	const GSVector4i g8c = GSVector4i(w.c.g8);
	const GSVector4i g8f = GSVector4i(w.f.g8).xxxx();

	// The ramp term of a step is gc times the whole vector, wherever in the vector
	// the span begins, so this one is constant across the table.
	for (int l = 0; l < vlen; l++)
		kbuf[l] = static_cast<float>(vlen);

	const GSVector4 kv = GSVector4::load<true>(kbuf);

	// The step out of a vector is a property of the VECTOR, not of the pixel
	// inside it that the span happens to start on: steps() reads `base` and
	// nothing else, and the ramp above is constant. So the four entries of a
	// vector all hold the same pair, and the pair is computed once per vector
	// rather than once per entry.
	for (int base = 0; base < 8; base += vlen)
	{
		steps(w.c, base);
		const GSVector4 hi = GSVector4::load<true>(hibuf);
		const GSVector4 lo = GSVector4::load<true>(lobuf);

		const GSVector4i sr = entry(gcr, dwr, kv, hi, lo, pfr);
		const GSVector4i sg = entry(gcg, dwg, kv, hi, lo, pfg);
		const GSVector4i sb = entry(gcb, dwb, kv, hi, lo, pfb);
		const GSVector4i sa = entry(gca, dwa, kv, hi, lo, pfa);

		steps(w.f, base);
		const GSVector4 fhi = GSVector4::load<true>(hibuf);
		const GSVector4 flo = GSVector4::load<true>(lobuf);

		const GSVector4i sf = entry(fgc, fdw, kv, fhi, flo, fpf);

		const GSVector4i rb0 = pack(sr, sb);
		const GSVector4i ga0 = pack(sg, sa);
		const GSVector4i f0 = sf.xxzzlh();
		const GSVector4i rb1 = pack(g8c.xxxx() - sr, g8c.zzzz() - sb);
		const GSVector4i ga1 = pack(g8c.yyyy() - sg, g8c.wwww() - sa);
		const GSVector4i f1 = (g8f - sf).xxzzlh();

		for (int s = base; s < base + vlen; s++)
		{
			local.dw[s][0].rb = rb0;
			local.dw[s][0].ga = ga0;
			local.dw[s][0].f = f0;
			local.dw[s][1].rb = rb1;
			local.dw[s][1].ga = ga1;
			local.dw[s][1].f = f1;
		}
	}
#endif
}

void GSDrawScanline::CSetupPrim(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, GSScanlineLocalData& local)
{
	const GSScanlineGlobalData& global = GlobalFromLocal(local);
	GSScanlineSelector sel = global.sel;

	bool has_z = sel.zb != 0;
	bool has_f = sel.fb && sel.fge;
	bool has_t = sel.fb && sel.tfx != TFX_NONE;
	bool has_c = sel.fb && !(sel.tfx == TFX_DECAL && sel.tcc);

	constexpr int vlen = sizeof(VectorF) / sizeof(float);

	// Colour and fog no longer come from here at all: SetupColourWalkTables above
	// builds their lane tables and their steps from the primitive's own walk, and
	// the rasterizer calls it right after this. What is left is depth, the
	// texture coordinate and the flat-colour constants.
#if _M_SSE >= 0x501
	auto load_shift = [](int i) { return GSVector8::load<false>(&g_const_256b.m_shift[8 - i]); };
	// One vector IS one block here, so the coordinate's step and the block step
	// are the same number.
	const GSVector4 coord_step_shift = GSVector4::broadcast32(&g_const_256b.m_shift[0]);
#else
	static const GSVector4* shift = reinterpret_cast<const GSVector4*>(g_const_128b.m_shift);
	auto load_shift = [](int i) { return shift[1 + i]; };
	// The texture coordinate steps one VECTOR, not one block -- GSBlockWalk.h says
	// why, and the pin is TheCoordinateStepStaysOneVector. Taking the block step
	// here would advance the coordinate eight pixels every four and sample the
	// texture twice as fast as the draw walks.
	const GSVector4 coord_step_shift = shift[0];
#endif

	if (has_z || has_f)
	{
		if (sel.prim != GS_SPRITE_CLASS)
		{
			if (has_z && !sel.zequal)
			{
				const VectorF dzf(static_cast<float>(dscan.p.F64[1]));
#if _M_SSE >= 0x501
				double dz = dscan.p.F64[1] * g_const_256b.m_shift[0];
				memcpy(&local.d8.p.z, &dz, sizeof(dz));
#else
				const GSVector4 dz = GSVector4::broadcast64(&dscan.p.z);
				local.d4.z = dz.mul64(GSVector4::f32to64(shift));
#endif
				for (int i = 0; i < vlen; i++)
				{
					local.d[i].z = dzf * load_shift(i);
				}
			}
		}
		else
		{
			if (has_f)
			{
#if _M_SSE >= 0x501
				local.p.f = GSVector4i(vertex[index[1]].p).extract32<3>();
#else
				local.p.f = GSVector4i(vertex[index[1]].p).zzzzh().zzzz();
#endif
			}

			if (has_z)
			{
				local.p.z = vertex[index[1]].t.U32[3]; // u32 z is bypassed in t.w
			}
		}
	}

	if (has_t)
	{
		// The coordinate a triangle samples at trails the exact plane, by less than
		// a sixteenth of a texel, in the direction the walk is going. Console-
		// measured (gs-shade, SCPH-30001): where the exact coordinate lands ON a
		// sixteenth and the walk is forward, silicon reports the sixteenth BELOW it
		// -- 3,840 of 3,840 readings on a gradient of four sixteenths per pixel,
		// and never the other way. Where the walk is backward the same lag puts
		// silicon just above the boundary, which floors where we already do, and
		// that section is identical to the console on every reading. A still
		// coordinate is exact in both, which is what makes the lag attributable to
		// the step rather than the seed.
		//
		// A sprite's coordinate is exact on silicon (gs-interp, 3,072 of 3,072,
		// negative gradients included) and exact in ours, so sprites take nothing.
		// Lines and points were not measured; they ride the triangle rule because
		// it is the same walk, and a point has no gradient to lag anyway.
		//
		// Where the lag comes from in the hardware's walk is unfitted; one unit of
		// our own 16.16 coordinate is the smallest bias that reproduces every
		// reading, and it can only move a pixel that lands exactly on a boundary.
		//
		// ⚠️ The axis has to be tested on the step the walk actually takes, not on
		// the exact plane's. On the affine route that step is floored onto the
		// accumulator's grid, so a gradient below one grid unit per pixel walks
		// NOWHERE -- and a still coordinate does not trail, by this rule's own
		// evidence. Reading the sign off the plane instead moves a still
		// coordinate a sixteenth backwards.

		// The colour and fog steps above take the block; this one does not. The
		// coordinate keeps a per-vector step whatever the block width is, which is
		// the same footing depth is on a few lines up.
		// Twice the triangle's area, in 12.4 squared: a power of two means the
		// setup's divide by it is exact, and such a triangle trails on no axis.
		// Lines and points have no area to ask about and keep the lag they always
		// had -- nothing measured draws one.
		const bool inverts_exactly = sel.prim == GS_TRIANGLE_CLASS
		    && GSSetupInvertsExactly(GSTriangleTwiceArea(
		           vertex[index[0]].p, vertex[index[1]].p, vertex[index[2]].p));

		if (sel.uvwalk)
		{
			// The console's texel accumulator is seed + n * floor(step) on a 12.15
			// grid -- GSCoordinateWalk.h -- so the per-pixel step is floored ONCE
			// and every lane and vector offset is a multiple of it. Truncating the
			// product instead (floor(n * step)) is a different sequence and is
			// what our arm did, and it is the one that disagrees with the console.
			const s32 du = GSAffineCoordinateOnGrid(dscan.t.x);
			const s32 dv = GSAffineCoordinateOnGrid(dscan.t.y);

			if (sel.prim != GS_SPRITE_CLASS)
			{
				local.tclag.u = VectorI(GSCoordinateStepTrails(du, inverts_exactly) ? 1 : 0);
				local.tclag.v = VectorI(GSCoordinateStepTrails(dv, inverts_exactly) ? 1 : 0);
			}

			LOCAL_STEP.stq = GSVector4::cast(GSVector4i(du * vlen, dv * vlen, 0, 0));

			for (int i = 0; i < vlen; i++)
			{
				alignas(sizeof(VectorI)) s32 lu[vlen], lv[vlen];

				for (int l = 0; l < vlen; l++)
				{
					// Lane l covers the pixel l - i away from the span's anchor,
					// the same offsets g_const's m_shift / m_lane tables carry.
					lu[l] = du * (l - i);
					lv[l] = dv * (l - i);
				}

				local.d[i].s = VectorF::cast(VectorI::template load<true>(lu));
				local.d[i].t = VectorF::cast(VectorI::template load<true>(lv));
			}
		}
		else
		{
			if (sel.prim != GS_SPRITE_CLASS)
			{
				// A constant-Q triangle's own ST plane and a live divide take the
				// same rule: a live divide at a power-of-two area does not trail
				// either, so the exemption is the setup's and not the road's.
				local.tclag.u = VectorI(GSCoordinateStepTrails(dscan.t.x > 0.0f ? 1 : 0, inverts_exactly) ? 1 : 0);
				local.tclag.v = VectorI(GSCoordinateStepTrails(dscan.t.y > 0.0f ? 1 : 0, inverts_exactly) ? 1 : 0);
			}

			const GSVector4 coord_tstep = dscan.t * coord_step_shift;

			// A constant-Q TRIANGLE's ST plane arrives here as a 16.16 integer
			// too, and the scanline reads it the same way -- but it does not take
			// the accumulator's grid. That was measured both ways: flooring such a
			// plane's step onto the grid loses words that were exact without it.
			LOCAL_STEP.stq = sel.fst ? GSVector4::cast(GSVector4i(coord_tstep)) : coord_tstep;

			const VectorF dt(dscan.t);

			for (int j = 0, k = sel.fst ? 2 : 3; j < k; j++)
			{
				VectorF dstq;

				switch (j)
				{
					case 0: dstq = dt.xxxx(); break;
					case 1: dstq = dt.yyyy(); break;
					case 2: dstq = dt.zzzz(); break;
				}

				for (int i = 0; i < vlen; i++)
				{
					const VectorF v = dstq * load_shift(i);

					if (sel.fst)
					{
						switch (j)
						{
							case 0: local.d[i].s = VectorF::cast(VectorI(v)); break;
							case 1: local.d[i].t = VectorF::cast(VectorI(v)); break;
						}
					}
					else
					{
						switch (j)
						{
							case 0: local.d[i].s = v; break;
							case 1: local.d[i].t = v; break;
							case 2: local.d[i].q = v; break;
						}
					}
				}
			}
		}
	}

	if (has_c)
	{
		if (!sel.iip)
		{
			int last = 0;

			switch (sel.prim)
			{
				case GS_POINT_CLASS:    last = 0; break;
				case GS_LINE_CLASS:     last = 1; break;
				case GS_TRIANGLE_CLASS: last = 2; break;
				case GS_SPRITE_CLASS:   last = 1; break;
			}

			VectorI c = VectorI(VectorF(vertex[index[last]].c));

			c = c.upl16(c.zwxy());

			if (sel.tfx == TFX_NONE)
				c = c.srl16<7>();

			local.c.rb = c.xxxx();
			local.c.ga = c.zzzz();
		}
	}
}

template <class T>
__ri static bool TestAlpha(T& test, T& fm, T& zm, const T& ga, const GSScanlineGlobalData& global)
{
	GSScanlineSelector sel = global.sel;

	switch (sel.afail)
	{
		case AFAIL_FB_ONLY:
			if (!sel.zwrite)
				return true;
			break;

		case AFAIL_ZB_ONLY:
			if (!sel.fwrite)
				return true;
			break;

		case AFAIL_RGB_ONLY:
			if (!sel.zwrite && sel.fpsm == 1)
				return true;
			break;
	}

	T t;

	switch (sel.atst)
	{
		case ATST_NEVER:
			t = GSVector4i::xffffffff();
			break;

		case ATST_ALWAYS:
			return true;

		case ATST_LESS:
		case ATST_LEQUAL:
			t = (ga >> 16) > T(global.aref);
			break;

		case ATST_EQUAL:
			t = (ga >> 16) != T(global.aref);
			break;

		case ATST_GEQUAL:
		case ATST_GREATER:
			t = (ga >> 16) < T(global.aref);
			break;

		case ATST_NOTEQUAL:
			t = (ga >> 16) == T(global.aref);
			break;

		default:
			ASSUME(0);
	}

	switch (sel.afail)
	{
		case AFAIL_KEEP:
			test |= t;
			if (test.alltrue())
				return false;
			break;

		case AFAIL_FB_ONLY:
			zm |= t;
			break;

		case AFAIL_ZB_ONLY:
			fm |= t;
			break;

		case AFAIL_RGB_ONLY:
			zm |= t;
			// Only reachable with a 32-bit frame: GetAFAIL degrades RGB_ONLY to
			// FB_ONLY on every other format (console-measured, gs-test capture).
			fm |= t & T::xff000000();
			break;

		default:
			ASSUME(0);
	}

	return true;
}

static const int s_offsets[] = {0, 2, 8, 10, 16, 18, 24, 26}; // columnTable16[0]

template <class T>
__ri static void WritePixel(const T& src, int addr, int i, u32 psm, const GSScanlineGlobalData& global)
{
	u8* dst = (u8*)global.vm + addr * 2 + s_offsets[i] * 2;

	switch (psm)
	{
		case 0:
			*(u32*)dst = src.U32[i];
			break;
		case 1:
			*(u32*)dst = (src.U32[i] & 0xffffff) | (*(u32*)dst & 0xff000000);
			break;
		case 2:
			*(u16*)dst = src.U16[i * 2];
			break;
	}
}

void GSDrawScanline::CDrawScanline(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local)
{
	CDrawScanline(pixels, left, top, scan, local, GlobalFromLocal(local).sel);
}

__ri void GSDrawScanline::CDrawScanline(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local, GSScanlineSelector sel)
{
	const GSScanlineGlobalData& global = GlobalFromLocal(local);

	constexpr int vlen = sizeof(VectorF) / sizeof(float);

#if _M_SSE < 0x501
	const GSVector4i* const_test = (GSVector4i*)g_const_128b.m_test;
#endif
	VectorI test;
	VectorF z0, z1;
	VectorI f;
	VectorF s, t, q;
	VectorI uf, vf;
	VectorI rbf, gaf;
	VectorI cov;

	// Init

	int skip, steps;

	// Where this span starts inside its eight-pixel BLOCK -- not inside its
	// vector -- is what indexes the colour and fog lane tables, and on a
	// four-lane host it also decides which of the two alternating steps the walk
	// begins on. Read off the true left edge, before the vector alignment rounds
	// it down. See GSColourWalk.h.
	const int cskip = left & 7;

#if _M_SSE < 0x501
	const bool block_split = GSBlockWalkIsSplit(vlen);
	const GSScanlineLocalData::blockstep* const dw = local.dw[cskip];
	int dwphase = 0;
#endif

	if (!sel.notest)
	{
		skip = left & (vlen - 1);
		steps = pixels + skip - vlen;
		left -= skip;
#if _M_SSE >= 0x501
		test = GSVector8i::i8to32(&g_const_256b.m_test[16 - skip]) | GSVector8i::i8to32(&g_const_256b.m_test[0 - (steps & (steps >> 31))]);
#else
		test = const_test[skip] | const_test[7 + (steps & (steps >> 31))];
#endif
	}
	else
	{
		skip = 0;
		steps = pixels - vlen;
	}

	pxAssert((left & (vlen - 1)) == 0);

	const GSVector2i* fza_base = &global.fzbr[top];
	const GSVector2i* fza_offset = &global.fzbc[left >> 2];

	if (sel.prim != GS_SPRITE_CLASS)
	{
		if (sel.fwrite && sel.fge)
		{
#if _M_SSE >= 0x501
			f = GSVector8i::broadcast16(GSVector4i(scan.t).srl<12>()).add16(local.d[cskip].f);
#else
			f = GSVector4i(scan.t).zzzzh().zzzz().add16(local.d[cskip].f);
#endif
		}

		if (sel.zb)
		{
			if (sel.zequal)
			{
				u32 z = static_cast<u32>(scan.p.F64[1]);
				z0 = VectorF::cast(VectorI(z));
			}
			else
			{
				VectorF zbase = VectorF::broadcast64(&scan.p.z);
				z0 = zbase.add64(VectorF::f32to64(&local.d[skip].z.F32[0]));
				z1 = zbase.add64(VectorF::f32to64(&local.d[skip].z.F32[vlen/2]));
			}
		}
	}

	if (sel.fb)
	{
		if (sel.edge)
		{
#if _M_SSE >= 0x501
			cov = GSVector8i::broadcast16(GSVector4i::cast(scan.p)).srl16<9>();
#else
			cov = GSVector4i::cast(scan.p).xxxxl().xxxx().srl16<9>();
#endif
		}

		if (sel.tfx != TFX_NONE)
		{
			if (sel.fst)
			{
				// A span that walks the accumulator seeds on its grid, floored
				// below the exact plane (GSCoordinateWalk.h); a triangle's own ST
				// plane keeps the conversion it always had.
				VectorI vt = VectorI::broadcast128(sel.uvwalk
						? (GSVector4i(scan.t.floor()) & GSVector4i(GS_UV_GRID_MASK))
						: GSVector4i(scan.t));

				VectorI u = vt.xxxx() + VectorI::cast(local.d[skip].s);
				VectorI v = vt.yyyy();

				if (sel.prim != GS_SPRITE_CLASS || sel.mmin)
				{
					v += VectorI::cast(local.d[skip].t);
				}
				else if (sel.ltf)
				{
					vf = GSSaturateCoordinate(GSTruncateCoordinate(v)).xxzzlh().srl16<12>();
				}

				s = VectorF::cast(u);
				t = VectorF::cast(v);
			}
			else
			{
#if _M_SSE >= 0x501
				s = GSVector8::broadcast32(&scan.t.x) + local.d[skip].s;
				t = GSVector8::broadcast32(&scan.t.y) + local.d[skip].t;
				q = GSVector8::broadcast32(&scan.t.z) + local.d[skip].q;
#else
				s = scan.t.xxxx() + local.d[skip].s;
				t = scan.t.yyyy() + local.d[skip].t;
				q = scan.t.zzzz() + local.d[skip].q;
#endif
			}
		}

		if (!(sel.tfx == TFX_DECAL && sel.tcc))
		{
			if (sel.iip)
			{
				GSVector4i c(scan.c);

				c = c.upl16(c.zwxy());

#if _M_SSE >= 0x501
				rbf = GSVector8i::broadcast32(&c.x).add16(local.d[cskip].rb);
				gaf = GSVector8i::broadcast32(&c.z).add16(local.d[cskip].ga);
#else
				rbf = c.xxxx().add16(local.d[cskip].rb);
				gaf = c.zzzz().add16(local.d[cskip].ga);
#endif
			}
			else
			{
				rbf = local.c.rb;
				gaf = local.c.ga;
			}
		}
	}

	while (1)
	{
		do
		{
			int fa = 0, za = 0;
			VectorI fd, zs, zd;
			VectorI fm, zm;
			VectorI rb, ga;

			// TestZ

			if (sel.zb)
			{
				za = (fza_base->y + fza_offset->y) % HALF_VM_SIZE;

				if (sel.prim != GS_SPRITE_CLASS)
				{
					if (sel.zequal)
					{
						zs = VectorI::cast(z0);
					}
					else if (sel.zoverflow)
					{
						// SSE only has double to int32 conversion, no double to uint32
						// Work around this by subtracting 0x80000000 before converting, then adding it back after
						// Since we've subtracted 0x80000000, truncating now rounds up for numbers less than 0x80000000
						// So approximate the truncation by subtracting an extra (0.5 - ulp) and rounding instead
						GSVector4i zl = z0.add64(VectorF::m_xc1e00000000fffff).f64toi32(false);
						GSVector4i zh = z1.add64(VectorF::m_xc1e00000000fffff).f64toi32(false);
#if _M_SSE >= 0x501
						zs = GSVector8i(zl, zh);
#else
						zs = zl.upl64(zh);
#endif
						zs += VectorI::x80000000();
					}
					else
					{
#if _M_SSE >= 0x501
						zs = GSVector8i(z0.f64toi32(), z1.f64toi32());
#else
						zs = z0.f64toi32().upl64(z1.f64toi32());
#endif
					}

					if (sel.zclamp)
						zs = zs.min_u32(VectorI(static_cast<int>(0xFFFFFFFFu >> (sel.zpsm * 8))));
				}
				else
				{
					zs = local.p.z;
				}

				if (sel.ztest)
				{
#if _M_SSE >= 0x501
					zd = GSVector8i::load(
						(u8*)global.vm + za * 2     , (u8*)global.vm + za * 2 + 16,
						(u8*)global.vm + za * 2 + 32, (u8*)global.vm + za * 2 + 48);
#else
					zd = GSVector4i::load((u8*)global.vm + za * 2, (u8*)global.vm + za * 2 + 16);
#endif

					VectorI zso = zs;
					VectorI zdo = zd;

					switch (sel.zpsm)
					{
						case 1: zdo = zdo.sll32< 8>().srl32<8>(); break;
						case 2: zdo = zdo.sll32<16>().srl32<16>(); break;
						default: break;
					}

					if (sel.zpsm == 0)
					{
						zso -= VectorI::x80000000();
						zdo -= VectorI::x80000000();
					}

					switch (sel.ztst)
					{
						case ZTST_GEQUAL:  test |= zso <  zdo; break;
						case ZTST_GREATER: test |= zso <= zdo; break;
					}

					if (test.alltrue())
						continue;
				}
			}

			// SampleTexture

			if (sel.fb && sel.tfx != TFX_NONE)
			{
				VectorI u, v, uv[2];
				VectorI lodi, lodf;
				VectorI minuv, maxuv;
				VectorI addr00, addr01, addr10, addr11;
				VectorI c00, c01, c10, c11;

				if (sel.mmin)
				{
					if (!sel.fst)
					{
						const VectorF r = GSPerspectiveRecip(q);

						u = VectorI(s * r);
						v = VectorI(t * r);
					}
					else
					{
						u = VectorI::cast(s);
						v = VectorI::cast(t);
					}

					// The DDA's lag, taken before the level shift divides it away.
					if (sel.prim != GS_SPRITE_CLASS)
					{
						u -= local.tclag.u;
						v -= local.tclag.v;
					}

					if (!sel.lcm)
					{
						// The console's logarithm is a 128-entry table read on Q's
						// own mantissa, not a curve -- GSLevelOfDetail.h carries the
						// measurement, the four tables and the one entry that steps
						// backwards on hardware. The level comes out in SIXTEENTHS of a level,
						// so it shifts up by twelve to reach the 16.16 the rest of
						// this path already speaks: the round-off `+ 0x8000` below
						// is then exactly the console's `(LOD16 + 8) >> 4`, ties up,
						// and the trilinear weight the sampler takes from the top
						// four bits of the fraction is exactly `LOD16 & 15`.
						VectorI lod;

						for (int i = 0; i < vlen; i++)
						{
							const s32 lod16 = GSLevelOfDetail16(q.F32[i], global.lodtab,
								global.lodk, global.lodshift);

							lod.I32[i] = std::min(std::max(lod16 << 12, 0), global.lodmxl);
						}

						if (sel.mmin == 1) // round-off mode
						{
							lod += 0x8000;
						}

						lodi = lod.srl32<16>();

						if (sel.mmin == 2) // trilinear mode
						{
							lodf = lod.xxzzlh();
						}

						// shift u/v by (int)lod

#if _M_SSE >= 0x501
						u = u.srav32(lodi);
						v = v.srav32(lodi);

						uv[0] = u;
						uv[1] = v;

						GSVector8i tmin = GSVector8i::broadcast128(global.t.min);
						GSVector8i tminu = tmin.upl16().srlv32(lodi);
						GSVector8i tminv = tmin.uph16().srlv32(lodi);

						GSVector8i tmax = GSVector8i::broadcast128(global.t.max);
						GSVector8i tmaxu = tmax.upl16().srlv32(lodi);
						GSVector8i tmaxv = tmax.uph16().srlv32(lodi);

						minuv = tminu.pu32(tminv);
						maxuv = tmaxu.pu32(tmaxv);
#else
						GSVector4i aabb = u.upl32(v);
						GSVector4i ccdd = u.uph32(v);

						GSVector4i aaxx = aabb.sra32(lodi.x);
						GSVector4i xxbb = aabb.sra32(lodi.y);
						GSVector4i ccxx = ccdd.sra32(lodi.z);
						GSVector4i xxdd = ccdd.sra32(lodi.w);

						GSVector4i acac = aaxx.upl32(ccxx);
						GSVector4i bdbd = xxbb.uph32(xxdd);

						u = acac.upl32(bdbd);
						v = acac.uph32(bdbd);

						uv[0] = u;
						uv[1] = v;

						GSVector4i minmax = global.t.minmax;

						GSVector4i v0 = minmax.srl16(lodi.x);
						GSVector4i v1 = minmax.srl16(lodi.y);
						GSVector4i v2 = minmax.srl16(lodi.z);
						GSVector4i v3 = minmax.srl16(lodi.w);

						v0 = v0.upl16(v1);
						v2 = v2.upl16(v3);

						minuv = v0.upl32(v2);
						maxuv = v0.uph32(v2);
#endif
					}
					else
					{
						lodi = global.lod.i;

#if _M_SSE >= 0x501
						u = u.srav32(lodi);
						v = v.srav32(lodi);
#else
						u = u.sra32(lodi.x);
						v = v.sra32(lodi.x);
#endif

						uv[0] = u;
						uv[1] = v;

						minuv = local.temp.uv_minmax[0];
						maxuv = local.temp.uv_minmax[1];
					}

					u = GSTruncateCoordinate(u);
					v = GSTruncateCoordinate(v);

					if (sel.ltf)
					{
						u -= 0x8000;
						v -= 0x8000;
					}

					u = GSSaturateCoordinate(u);
					v = GSSaturateCoordinate(v);

					if (sel.ltf)
					{
						uf = u.xxzzlh().srl16<12>();
						vf = v.xxzzlh().srl16<12>();
					}

					VectorI uv0 = u.sra32<16>().ps32(v.sra32<16>());
					VectorI uv1 = uv0;

					{
						VectorI repeat = (uv0 & minuv) | maxuv;
						VectorI clamp = uv0.sat_i16(minuv, maxuv);

						uv0 = clamp.blend8(repeat, VectorI::broadcast128(global.t.mask));
					}

					if (sel.ltf)
					{
						uv1 = uv1.add16(VectorI::x0001());

						VectorI repeat = (uv1 & minuv) | maxuv;
						VectorI clamp = uv1.sat_i16(minuv, maxuv);

						uv1 = clamp.blend8(repeat, VectorI::broadcast128(global.t.mask));
					}

					VectorI y0 = uv0.uph16() << (sel.tw + 3);
					VectorI x0 = uv0.upl16();

					if (sel.ltf)
					{
						VectorI y1 = uv1.uph16() << (sel.tw + 3);
						VectorI x1 = uv1.upl16();

						addr00 = y0 + x0;
						addr01 = y0 + x1;
						addr10 = y1 + x0;
						addr11 = y1 + x1;

						if (sel.tlu)
						{
							for (int i = 0; i < vlen; i++)
							{
								const u8* tex = (const u8*)global.tex[lodi.U32[i]];

								c00.U32[i] = global.clut[tex[addr00.U32[i]]];
								c01.U32[i] = global.clut[tex[addr01.U32[i]]];
								c10.U32[i] = global.clut[tex[addr10.U32[i]]];
								c11.U32[i] = global.clut[tex[addr11.U32[i]]];
							}
						}
						else
						{
							for (int i = 0; i < vlen; i++)
							{
								const u32* tex = (const u32*)global.tex[lodi.U32[i]];

								c00.U32[i] = tex[addr00.U32[i]];
								c01.U32[i] = tex[addr01.U32[i]];
								c10.U32[i] = tex[addr10.U32[i]];
								c11.U32[i] = tex[addr11.U32[i]];
							}
						}

						VectorI rb00 = c00.sll16<8>().srl16<8>();
						VectorI ga00 = c00.srl16<8>();
						VectorI rb01 = c01.sll16<8>().srl16<8>();
						VectorI ga01 = c01.srl16<8>();

						rb00 = rb00.lerp16_4(rb01, uf);
						ga00 = ga00.lerp16_4(ga01, uf);

						VectorI rb10 = c10.sll16<8>().srl16<8>();
						VectorI ga10 = c10.srl16<8>();
						VectorI rb11 = c11.sll16<8>().srl16<8>();
						VectorI ga11 = c11.srl16<8>();

						rb10 = rb10.lerp16_4(rb11, uf);
						ga10 = ga10.lerp16_4(ga11, uf);

						rb = rb00.lerp16_4(rb10, vf);
						ga = ga00.lerp16_4(ga10, vf);
					}
					else
					{
						addr00 = y0 + x0;

						if (sel.tlu)
						{
							for (int i = 0; i < vlen; i++)
							{
								c00.U32[i] = global.clut[((const u8*)global.tex[lodi.U32[i]])[addr00.U32[i]]];
							}
						}
						else
						{
							for (int i = 0; i < vlen; i++)
							{
								c00.U32[i] = ((const u32*)global.tex[lodi.U32[i]])[addr00.U32[i]];
							}
						}

						rb = c00.sll16<8>().srl16<8>();
						ga = c00.srl16<8>();
					}

					if (sel.mmin != 1) // !round-off mode
					{
						VectorI rb2, ga2;

						lodi += VectorI::x00000001();

						u = uv[0].sra32<1>();
						v = uv[1].sra32<1>();

						minuv = minuv.srl16<1>();
						maxuv = maxuv.srl16<1>();

						u = GSTruncateCoordinate(u);
						v = GSTruncateCoordinate(v);

						if (sel.ltf)
						{
							u -= 0x8000;
							v -= 0x8000;
						}

						u = GSSaturateCoordinate(u);
						v = GSSaturateCoordinate(v);

						if (sel.ltf)
						{
							uf = u.xxzzlh().srl16<12>();
							vf = v.xxzzlh().srl16<12>();
						}

						VectorI uv0 = u.sra32<16>().ps32(v.sra32<16>());
						VectorI uv1 = uv0;

						{
							VectorI repeat = (uv0 & minuv) | maxuv;
							VectorI clamp = uv0.sat_i16(minuv, maxuv);

							uv0 = clamp.blend8(repeat, VectorI::broadcast128(global.t.mask));
						}

						if (sel.ltf)
						{
							uv1 = uv1.add16(VectorI::x0001());

							VectorI repeat = (uv1 & minuv) | maxuv;
							VectorI clamp = uv1.sat_i16(minuv, maxuv);

							uv1 = clamp.blend8(repeat, VectorI::broadcast128(global.t.mask));
						}

						VectorI y0 = uv0.uph16() << (sel.tw + 3);
						VectorI x0 = uv0.upl16();

						if (sel.ltf)
						{
							VectorI y1 = uv1.uph16() << (sel.tw + 3);
							VectorI x1 = uv1.upl16();

							addr00 = y0 + x0;
							addr01 = y0 + x1;
							addr10 = y1 + x0;
							addr11 = y1 + x1;

							if (sel.tlu)
							{
								for (int i = 0; i < vlen; i++)
								{
									const u8* tex = (const u8*)global.tex[lodi.U32[i]];

									c00.U32[i] = global.clut[tex[addr00.U32[i]]];
									c01.U32[i] = global.clut[tex[addr01.U32[i]]];
									c10.U32[i] = global.clut[tex[addr10.U32[i]]];
									c11.U32[i] = global.clut[tex[addr11.U32[i]]];
								}
							}
							else
							{
								for (int i = 0; i < vlen; i++)
								{
									const u32* tex = (const u32*)global.tex[lodi.U32[i]];

									c00.U32[i] = tex[addr00.U32[i]];
									c01.U32[i] = tex[addr01.U32[i]];
									c10.U32[i] = tex[addr10.U32[i]];
									c11.U32[i] = tex[addr11.U32[i]];
								}
							}

							VectorI rb00 = c00.sll16<8>().srl16<8>();
							VectorI ga00 = c00.srl16<8>();
							VectorI rb01 = c01.sll16<8>().srl16<8>();
							VectorI ga01 = c01.srl16<8>();

							rb00 = rb00.lerp16_4(rb01, uf);
							ga00 = ga00.lerp16_4(ga01, uf);

							VectorI rb10 = c10.sll16<8>().srl16<8>();
							VectorI ga10 = c10.srl16<8>();
							VectorI rb11 = c11.sll16<8>().srl16<8>();
							VectorI ga11 = c11.srl16<8>();

							rb10 = rb10.lerp16_4(rb11, uf);
							ga10 = ga10.lerp16_4(ga11, uf);

							rb2 = rb00.lerp16_4(rb10, vf);
							ga2 = ga00.lerp16_4(ga10, vf);
						}
						else
						{
							addr00 = y0 + x0;

							if (sel.tlu)
							{
								for (int i = 0; i < vlen; i++)
								{
									c00.U32[i] = global.clut[((const u8*)global.tex[lodi.U32[i]])[addr00.U32[i]]];
								}
							}
							else
							{
								for (int i = 0; i < vlen; i++)
								{
									c00.U32[i] = ((const u32*)global.tex[lodi.U32[i]])[addr00.U32[i]];
								}
							}

							rb2 = c00.sll16<8>().srl16<8>();
							ga2 = c00.srl16<8>();
						}

						if (sel.lcm)
							lodf = global.lod.f;

						// The console blends two mip levels on a FOUR-BIT weight, so a
						// trilinear blend has sixteen steps. Measured directly: across one
						// level boundary silicon returns exactly 32 distinct values over two
						// level pairs 36 apart, which is 36/16 per step. Blending on the full
						// 16-bit fraction gives about 250 and is visibly finer than hardware.
						// Truncate to the top four bits before the fraction becomes a weight.
						lodf = lodf.srl16<12>().sll16<12>();

						lodf = lodf.srl16<1>();

						rb = rb.lerp16<0>(rb2, lodf);
						ga = ga.lerp16<0>(ga2, lodf);
					}
				}
				else
				{
					// Per-pixel MMAG/MMIN choice. lod > 0 is exactly Q < the crossing
					// constant, so one compare names the pixels on the minifying side;
					// ltfx_ge flips which side takes the linear filter. All-ones means
					// "this pixel filters linearly".
					VectorI lin;

					if (sel.ltfx)
					{
						lin = VectorI::cast(q < global.ltfx_q);

						if (sel.ltfx_ge)
							lin = ~lin;
					}

					if (!sel.fst)
					{
						const VectorF r = GSPerspectiveRecip(q);

						u = VectorI(s * r);
						v = VectorI(t * r);
					}
					else
					{
						u = VectorI::cast(s);
						v = VectorI::cast(t);
					}

					// The DDA's lag. Zero on any axis that is not walking forward,
					// so this only moves a coordinate that lands exactly on a
					// sixteenth. It is the console's own walk trailing the plane, so
					// it is part of the coordinate the truncation below acts on.
					if (sel.prim != GS_SPRITE_CLASS)
					{
						u -= local.tclag.u;
						v -= local.tclag.v;
					}

					u = GSTruncateCoordinate(u);
					v = GSTruncateCoordinate(v);

					if (!sel.fst && sel.ltf)
					{
						// The two filters do not sample the same point: nearest reads
						// at the coordinate, linear straddles the pair half a texel
						// back. So the bias is taken only where linear wins.
						//
						// It is exactly eight sixteenths, so it moves the texel index
						// and never the weight, and it is our own step onto the tap
						// pair rather than part of the console's coordinate -- which
						// is why it comes after the truncation and not before it.
						if (sel.ltfx)
						{
							const VectorI half = VectorI(0x8000) & lin;

							u -= half;
							v -= half;
						}
						else
						{
							u -= 0x8000;
							v -= 0x8000;
						}
					}

					u = GSSaturateCoordinate(u);
					v = GSSaturateCoordinate(v);

					if (sel.ltf)
					{
						uf = u.xxzzlh().srl16<12>();

						if (sel.prim != GS_SPRITE_CLASS)
						{
							vf = v.xxzzlh().srl16<12>();
						}

						// A zero weight turns the four-tap blend back into the nearest
						// tap, so the nearest side needs no separate path.
						if (sel.ltfx)
						{
							const VectorI lin16 = lin.xxzzlh();

							uf &= lin16;

							if (sel.prim != GS_SPRITE_CLASS)
								vf &= lin16;
						}
					}

					VectorI uv0 = u.sra32<16>().ps32(v.sra32<16>());
					VectorI uv1 = uv0;

					VectorI tmin = VectorI::broadcast128(global.t.min);
					VectorI tmax = VectorI::broadcast128(global.t.max);

					{
						VectorI repeat = (uv0 & tmin) | tmax;
						VectorI clamp = uv0.sat_i16(tmin, tmax);

						uv0 = clamp.blend8(repeat, VectorI::broadcast128(global.t.mask));
					}

					if (sel.ltf)
					{
						uv1 = uv1.add16(VectorI::x0001());

						VectorI repeat = (uv1 & tmin) | tmax;
						VectorI clamp = uv1.sat_i16(tmin, tmax);

						uv1 = clamp.blend8(repeat, VectorI::broadcast128(global.t.mask));
					}

					VectorI y0 = uv0.uph16() << (sel.tw + 3);
					VectorI x0 = uv0.upl16();

					if (sel.ltf)
					{
						VectorI y1 = uv1.uph16() << (sel.tw + 3);
						VectorI x1 = uv1.upl16();

						addr00 = y0 + x0;
						addr01 = y0 + x1;
						addr10 = y1 + x0;
						addr11 = y1 + x1;

						if (sel.tlu)
						{
							const u8* tex = (const u8*)global.tex[0];

							c00 = addr00.gather32_32(tex, global.clut);
							c01 = addr01.gather32_32(tex, global.clut);
							c10 = addr10.gather32_32(tex, global.clut);
							c11 = addr11.gather32_32(tex, global.clut);
						}
						else
						{
							const u32* tex = (const u32*)global.tex[0];

							c00 = addr00.gather32_32(tex);
							c01 = addr01.gather32_32(tex);
							c10 = addr10.gather32_32(tex);
							c11 = addr11.gather32_32(tex);
						}

						VectorI rb00 = c00.sll16<8>().srl16<8>();
						VectorI ga00 = c00.srl16<8>();
						VectorI rb01 = c01.sll16<8>().srl16<8>();
						VectorI ga01 = c01.srl16<8>();

						rb00 = rb00.lerp16_4(rb01, uf);
						ga00 = ga00.lerp16_4(ga01, uf);

						VectorI rb10 = c10.sll16<8>().srl16<8>();
						VectorI ga10 = c10.srl16<8>();
						VectorI rb11 = c11.sll16<8>().srl16<8>();
						VectorI ga11 = c11.srl16<8>();

						rb10 = rb10.lerp16_4(rb11, uf);
						ga10 = ga10.lerp16_4(ga11, uf);

						rb = rb00.lerp16_4(rb10, vf);
						ga = ga00.lerp16_4(ga10, vf);
					}
					else
					{
						addr00 = y0 + x0;

						if (sel.tlu)
						{
							c00 = addr00.gather32_32((const u8*)global.tex[0], global.clut);
						}
						else
						{
							c00 = addr00.gather32_32((const u32*)global.tex[0]);
						}

						rb = c00.sll16<8>().srl16<8>();
						ga = c00.srl16<8>();
					}
				}
			}

			// AlphaTFX

			if (sel.fb)
			{
				switch (sel.tfx)
				{
					case TFX_MODULATE:
						ga = ga.modulate16<1>(GSStoredVertexColor(gaf)).clamp8();
						if (!sel.tcc)
							ga = ga.mix16(GSWalkColorByte(gaf));
						break;
					case TFX_DECAL:
						if (!sel.tcc)
							ga = ga.mix16(GSWalkColorByte(gaf));
						break;
					case TFX_HIGHLIGHT:
						ga = ga.mix16(!sel.tcc ? GSWalkColorByte(gaf) : ga.addus8(GSWalkColorByte(gaf)));
						break;
					case TFX_HIGHLIGHT2:
						if (!sel.tcc)
							ga = ga.mix16(GSWalkColorByte(gaf));
						break;
					case TFX_NONE:
						ga = sel.iip ? GSWalkColorByte(gaf) : gaf;
						break;
				}

				if (sel.aa1)
				{
					VectorI x00800080(0x00800080);

					VectorI a = sel.edge ? cov : x00800080;

					if (!sel.abe)
					{
						ga = ga.mix16(a);
					}
					else
					{
						ga = ga.blend8(a, ga.eq16(x00800080).srl32<16>().sll32<16>());
					}
				}
			}

			// ReadMask

			if (sel.fwrite)
			{
				fm = global.fm;
			}

			if (sel.zwrite)
			{
				zm = global.zm;
			}

			// TestAlpha

			if (!TestAlpha(test, fm, zm, ga, global))
				continue;

			// ColorTFX

			if (sel.fwrite)
			{
				VectorI af;

				switch (sel.tfx)
				{
					case TFX_MODULATE:
						rb = rb.modulate16<1>(GSStoredVertexColor(rbf)).clamp8();
						break;
					case TFX_DECAL:
						break;
					case TFX_HIGHLIGHT:
					case TFX_HIGHLIGHT2:
						af = GSWalkColorByte(gaf.yywwlh());
						rb = rb.modulate16<1>(GSStoredVertexColor(rbf)).add16(af).clamp8();
						ga = ga.modulate16<1>(GSStoredVertexColor(gaf)).add16(af).clamp8().mix16(ga);
						break;
					case TFX_NONE:
						rb = sel.iip ? GSWalkColorByte(rbf) : rbf;
						break;
				}
			}

			// Fog
			//
			//     stored = Cf + floor((Cv - Cf) * F / 256)
			//
			// which is (F*Cv + (256 - F)*Cf) >> 8 -- the fog colour's weight is
			// 256 - F, NOT 255 - F, and the multiply floors. Measured on real
			// hardware over a flat ladder of every F value in four channels; the
			// documented 255 - F rule misses almost every fill. The two agree
			// exactly wherever the fog colour is zero, which is why it went unseen
			// for so long: separating them needs a non-zero, per-channel FOGCOL.
			//
			// lerp16<0> is already that rule: modulate16<0> is (a << 1) * f taken
			// from the high half of the signed 32-bit product, an arithmetic shift
			// and so a floor, and adding it to Cf makes the 256 - F weight. What
			// this needed was the OTHER half of the same measurement.
			//
			// THE BLEND RECEIVES AN INTEGER F. The fog lane walks in the colour
			// unit, 1/128 of a level, so the value carries a fraction; silicon
			// truncates it to eight bits before the multiply. The truncating form
			// is exact on every fog reading we hold and the fractional one is
			// nowhere near, so this is not a tie -- the low seven bits have to go
			// before the multiply, not after.
			if (sel.fwrite && sel.fge)
			{
#if _M_SSE >= 0x501
				GSVector8i fog = sel.prim != GS_SPRITE_CLASS ? f : GSVector8i::broadcast16(&local.p.f);

				GSVector8i frb((int)global.frb);
				GSVector8i fga((int)global.fga);
#else
				GSVector4i fog = sel.prim != GS_SPRITE_CLASS ? f : local.p.f;

				GSVector4i frb = global.frb;
				GSVector4i fga = global.fga;
#endif

				// floor(F), put back on the walk's own scale so that modulate16's
				// >> 15 lands on the >> 8 the rule asks for -- and through the same
				// pack the colour lanes go through, so a fog value the walk has
				// carried below zero saturates to 0 rather than reading back as a
				// large positive. Fog rides the colour DDA; it saturates like one.
				fog = GSStoredVertexColor(fog);

				rb = frb.lerp16<0>(rb, fog);
				ga = fga.lerp16<0>(ga, fog).mix16(ga);
			}

			// ReadFrame

			if (sel.fb)
			{
				fa = (fza_base->x + fza_offset->x) % HALF_VM_SIZE;

				if (sel.rfb)
				{
#if _M_SSE >= 0x501
					fd = GSVector8i::load(
						(u8*)global.vm + fa * 2     , (u8*)global.vm + fa * 2 + 16,
						(u8*)global.vm + fa * 2 + 32, (u8*)global.vm + fa * 2 + 48);
#else
					fd = GSVector4i::load((u8*)global.vm + fa * 2, (u8*)global.vm + fa * 2 + 16);
#endif
				}
			}

			// TestDestAlpha

			if (sel.date && (sel.fpsm == 0 || sel.fpsm == 2))
			{
				if (sel.datm)
				{
					if (sel.fpsm == 2)
					{
						// test |= fd.srl32(15) == VectorI::zero();
						test |= fd.sll32<16>().sra32<31>() == VectorI::zero();
					}
					else
					{
						test |= (~fd).sra32<31>();
					}
				}
				else
				{
					if (sel.fpsm == 2)
					{
						test |= fd.sll32<16>().sra32<31>(); // == VectorI::xffffffff();
					}
					else
					{
						test |= fd.sra32<31>();
					}
				}

				if (test.alltrue())
					continue;
			}

			// WriteMask

			int fzm = 0;

			if (!sel.notest)
			{
				if (sel.fwrite)
				{
					fm |= test;
				}

				if (sel.zwrite)
				{
					zm |= test;
				}

				if (sel.fwrite && sel.zwrite)
				{
					fzm = ~(fm == VectorI::xffffffff()).ps32(zm == VectorI::xffffffff()).mask();
				}
				else if (sel.fwrite)
				{
					fzm = ~(fm == VectorI::xffffffff()).ps32().mask();
				}
				else if (sel.zwrite)
				{
					fzm = ~(zm == VectorI::xffffffff()).ps32().mask();
				}
			}

			// WriteZBuf

			if (sel.zwrite)
			{
				if (sel.ztest && sel.zpsm < 2)
				{
					zs = zs.blend8(zd, zm);
				}

				bool fast = sel.ztest ? sel.zpsm < 2 : sel.zpsm == 0 && sel.notest;

				if (sel.notest)
				{
					if (fast)
					{
#if _M_SSE >= 0x501
						GSVector4i::storel((u8*)global.vm + za * 2     , zs.extract<0>());
						GSVector4i::storeh((u8*)global.vm + za * 2 + 16, zs.extract<0>());
						GSVector4i::storel((u8*)global.vm + za * 2 + 32, zs.extract<1>());
						GSVector4i::storeh((u8*)global.vm + za * 2 + 48, zs.extract<1>());
#else
						GSVector4i::storel((u8*)global.vm + za * 2     , zs);
						GSVector4i::storeh((u8*)global.vm + za * 2 + 16, zs);
#endif
					}
					else
					{
						WritePixel(zs, za, 0, sel.zpsm, global);
						WritePixel(zs, za, 1, sel.zpsm, global);
						WritePixel(zs, za, 2, sel.zpsm, global);
						WritePixel(zs, za, 3, sel.zpsm, global);
#if _M_SSE >= 0x501
						WritePixel(zs, za, 4, sel.zpsm, global);
						WritePixel(zs, za, 5, sel.zpsm, global);
						WritePixel(zs, za, 6, sel.zpsm, global);
						WritePixel(zs, za, 7, sel.zpsm, global);
#endif
					}
				}
				else
				{
					if (fast)
					{
#if _M_SSE >= 0x501
						if (fzm & 0x00000f00) GSVector4i::storel((u8*)global.vm + za * 2     , zs.extract<0>());
						if (fzm & 0x0000f000) GSVector4i::storeh((u8*)global.vm + za * 2 + 16, zs.extract<0>());
						if (fzm & 0x0f000000) GSVector4i::storel((u8*)global.vm + za * 2 + 32, zs.extract<1>());
						if (fzm & 0xf0000000) GSVector4i::storeh((u8*)global.vm + za * 2 + 48, zs.extract<1>());
#else
						if (fzm & 0x0f00) GSVector4i::storel((u8*)global.vm + za * 2     , zs);
						if (fzm & 0xf000) GSVector4i::storeh((u8*)global.vm + za * 2 + 16, zs);
#endif
					}
					else
					{
						if (fzm & 0x00000300) WritePixel(zs, za, 0, sel.zpsm, global);
						if (fzm & 0x00000c00) WritePixel(zs, za, 1, sel.zpsm, global);
						if (fzm & 0x00003000) WritePixel(zs, za, 2, sel.zpsm, global);
						if (fzm & 0x0000c000) WritePixel(zs, za, 3, sel.zpsm, global);
#if _M_SSE >= 0x501
						if (fzm & 0x03000000) WritePixel(zs, za, 4, sel.zpsm, global);
						if (fzm & 0x0c000000) WritePixel(zs, za, 5, sel.zpsm, global);
						if (fzm & 0x30000000) WritePixel(zs, za, 6, sel.zpsm, global);
						if (fzm & 0xc0000000) WritePixel(zs, za, 7, sel.zpsm, global);
#endif
					}
				}
			}

			// AlphaBlend

			if (sel.fwrite && (sel.abe || sel.aa1))
			{
				VectorI rbs = rb, gas = ga, rbd, gad, a, mask;

				if ((sel.aba != sel.abb && (sel.aba == 1 || sel.abb == 1 || sel.abc == 1)) || sel.abd == 1)
				{
					switch (sel.fpsm)
					{
						case 0:
						case 1:
							rbd = fd.sll16<8>().srl16<8>();
							gad = fd.srl16<8>();
							break;
						case 2:
							rbd = ((fd & 0x7c00) << 9) | ((fd & 0x001f) << 3);
							gad = ((fd & 0x8000) << 8) | ((fd & 0x03e0) >> 2);
							break;
					}
				}

				if (sel.aba != sel.abb)
				{
					switch(sel.aba)
					{
						case 0: break;
						case 1: rb = rbd; break;
						case 2: rb = VectorI::zero(); break;
					}

					switch(sel.abb)
					{
						case 0: rb = rb.sub16(rbs); break;
						case 1: rb = rb.sub16(rbd); break;
						case 2: break;
					}

					if (!(sel.fpsm == 1 && sel.abc == 1))
					{
						switch(sel.abc)
						{
							case 0: a = gas.yywwlh().sll16<7>(); break;
							case 1: a = gad.yywwlh().sll16<7>(); break;
							case 2: a = global.afix; break;
						}

						rb = rb.modulate16<1>(a);
					}

					switch (sel.abd)
					{
						case 0: rb = rb.add16(rbs); break;
						case 1: rb = rb.add16(rbd); break;
						case 2: break;
					}
				}
				else
				{
					switch (sel.abd)
					{
						case 0: break;
						case 1: rb = rbd; break;
						case 2: rb = VectorI::zero(); break;
					}
				}

				if (sel.pabe)
				{
					mask = (gas << 8).sra32<31>();

					rb = rbs.blend8(rb, mask);
				}

				if (sel.aba != sel.abb)
				{
					switch(sel.aba)
					{
						case 0: break;
						case 1: ga = gad; break;
						case 2: ga = VectorI::zero(); break;
					}

					switch(sel.abb)
					{
						case 0: ga = ga.sub16(gas); break;
						case 1: ga = ga.sub16(gad); break;
						case 2: break;
					}

					if (!(sel.fpsm == 1 && sel.abc == 1))
					{
						ga = ga.modulate16<1>(a);
					}

					switch (sel.abd)
					{
						case 0: ga = ga.add16(gas); break;
						case 1: ga = ga.add16(gad); break;
						case 2: break;
					}
				}
				else
				{
					switch (sel.abd)
					{
						case 0: break;
						case 1: ga = gad; break;
						case 2: ga = VectorI::zero(); break;
					}
				}

				if (sel.pabe)
				{
					ga = gas.blend8(ga, mask >> 16);
				}
				else
				{
					if (sel.fpsm != 1)
					{
						ga = ga.mix16(gas);
					}
				}
			}

			// WriteFrame

			if (sel.fwrite)
			{
				// Dither is not a 16-bit-only feature. Silicon adds DM[y & 3][x & 3]
				// to the eight-bit colour on a 32-bit and on a 24-bit destination
				// as well, with the same matrix and the same indexing -- 4080 of
				// 4096 pixels move on each (gs-dither, SCPH-30001, 2026-08-11).
				// The 16-bit gate was ours, not the hardware's.
				//
				// fmt 3 is not a frame-buffer format and the capture did not sweep
				// one. The add lands before the colour clamp, which is where it
				// already sat and where the capture puts it (1024/1024 against the
				// alternative order), and after the blend (6144/6144).
				//
				// Mirrored in both scanline code generators; the three must agree.
				if (sel.dthe && sel.fpsm != 3)
				{
					int y = (top & 3) << 1;

					rb = rb.add16(VectorI::broadcast128(global.dimx[0 + y]));
					ga = ga.add16(VectorI::broadcast128(global.dimx[1 + y]));
				}

				if (sel.colclamp == 0)
				{
					rb &= VectorI::x00ff();
					ga &= VectorI::x00ff();
				}

				VectorI fs = rb.upl16(ga).pu16(rb.uph16(ga));

				if (sel.fba && sel.fpsm != 1)
				{
					fs |= VectorI::x80000000();
				}

				if (sel.fpsm == 2)
				{
					VectorI rb = fs & 0x00f800f8;
					VectorI ga = fs & 0x8000f800;

					fs = (ga >> 16) | (rb >> 9) | (ga >> 6) | (rb >> 3);
				}

				if (sel.rfb)
				{
					fs = fs.blend(fd, fm);
				}

				bool fast = sel.rfb ? sel.fpsm < 2 : sel.fpsm == 0 && sel.notest;

				if (sel.notest)
				{
					if (fast)
					{
#if _M_SSE >= 0x501
						GSVector4i::storel((u8*)global.vm + fa * 2     , fs.extract<0>());
						GSVector4i::storeh((u8*)global.vm + fa * 2 + 16, fs.extract<0>());
						GSVector4i::storel((u8*)global.vm + fa * 2 + 32, fs.extract<1>());
						GSVector4i::storeh((u8*)global.vm + fa * 2 + 48, fs.extract<1>());
#else
						GSVector4i::storel((u8*)global.vm + fa * 2     , fs);
						GSVector4i::storeh((u8*)global.vm + fa * 2 + 16, fs);
#endif
					}
					else
					{
						WritePixel(fs, fa, 0, sel.fpsm, global);
						WritePixel(fs, fa, 1, sel.fpsm, global);
						WritePixel(fs, fa, 2, sel.fpsm, global);
						WritePixel(fs, fa, 3, sel.fpsm, global);
#if _M_SSE >= 0x501
						WritePixel(fs, fa, 4, sel.fpsm, global);
						WritePixel(fs, fa, 5, sel.fpsm, global);
						WritePixel(fs, fa, 6, sel.fpsm, global);
						WritePixel(fs, fa, 7, sel.fpsm, global);
#endif
					}
				}
				else
				{
					if (fast)
					{
#if _M_SSE >= 0x501
						if (fzm & 0x0000000f) GSVector4i::storel((u8*)global.vm + fa * 2     , fs.extract<0>());
						if (fzm & 0x000000f0) GSVector4i::storeh((u8*)global.vm + fa * 2 + 16, fs.extract<0>());
						if (fzm & 0x000f0000) GSVector4i::storel((u8*)global.vm + fa * 2 + 32, fs.extract<1>());
						if (fzm & 0x00f00000) GSVector4i::storeh((u8*)global.vm + fa * 2 + 48, fs.extract<1>());
#else
						if (fzm & 0x000f) GSVector4i::storel((u8*)global.vm + fa * 2     , fs);
						if (fzm & 0x00f0) GSVector4i::storeh((u8*)global.vm + fa * 2 + 16, fs);
#endif
					}
					else
					{
						if (fzm & 0x00000003) WritePixel(fs, fa, 0, sel.fpsm, global);
						if (fzm & 0x0000000c) WritePixel(fs, fa, 1, sel.fpsm, global);
						if (fzm & 0x00000030) WritePixel(fs, fa, 2, sel.fpsm, global);
						if (fzm & 0x000000c0) WritePixel(fs, fa, 3, sel.fpsm, global);
#if _M_SSE >= 0x501
						if (fzm & 0x00030000) WritePixel(fs, fa, 4, sel.fpsm, global);
						if (fzm & 0x000c0000) WritePixel(fs, fa, 5, sel.fpsm, global);
						if (fzm & 0x00300000) WritePixel(fs, fa, 6, sel.fpsm, global);
						if (fzm & 0x00c00000) WritePixel(fs, fa, 7, sel.fpsm, global);
#endif
					}
				}
			}
		} while (0);

		if (sel.edge)
			break;

		if (steps <= 0)
			break;

		// Step

		steps -= vlen;

		fza_offset += vlen / 4;

		if (sel.prim != GS_SPRITE_CLASS)
		{
			if (sel.zb && !sel.zequal)
			{
#if _M_SSE >= 0x501
				GSVector8 add = GSVector8::broadcast64(&local.d8.p.z);
#else
				GSVector4 add = local.d4.z;
#endif
				z0 = z0.add64(add);
				z1 = z1.add64(add);
			}

			if (sel.fwrite && sel.fge)
			{
#if _M_SSE >= 0x501
				f = f.add16(GSVector8i::broadcast16(&local.d8.p.f));
#else
				f = f.add16(block_split ? dw[dwphase].f : local.d4.f);
#endif
			}
		}

		if (sel.fb)
		{
			if (sel.tfx != TFX_NONE)
			{
				if (sel.fst)
				{
					VectorI stq = VectorI::cast(VectorF(LOCAL_STEP.stq));

					s = VectorF::cast(VectorI::cast(s) + stq.xxxx());

					if (sel.prim != GS_SPRITE_CLASS || sel.mmin)
					{
						t = VectorF::cast(VectorI::cast(t) + stq.yyyy());
					}
				}
				else
				{
					VectorF stq(LOCAL_STEP.stq);

					s += stq.xxxx();
					t += stq.yyyy();
					q += stq.zzzz();
				}
			}
		}

		if (!(sel.tfx == TFX_DECAL && sel.tcc))
		{
			if (sel.iip)
			{
#if _M_SSE >= 0x501
				GSVector8i c = GSVector8i::broadcast64(&local.d8.c);
				rbf = rbf.add16(c.xxxx()).max_i16(VectorI::zero());
				gaf = gaf.add16(c.yyyy()).max_i16(VectorI::zero());
#else
				if (block_split)
				{
					rbf = rbf.add16(dw[dwphase].rb).max_i16(VectorI::zero());
					gaf = gaf.add16(dw[dwphase].ga).max_i16(VectorI::zero());
				}
				else
				{
					GSVector4i c = local.d4.c;
					rbf = rbf.add16(c.xxxx()).max_i16(VectorI::zero());
					gaf = gaf.add16(c.yyyy()).max_i16(VectorI::zero());
				}
#endif
			}
		}

#if _M_SSE < 0x501
		dwphase ^= 1;
#endif

		if (!sel.notest)
		{
#if _M_SSE >= 0x501
			test = GSVector8i::i8to32(&g_const_256b.m_test[0 - (steps & (steps >> 31))]);
#else
			test = const_test[7 + (steps & (steps >> 31))];
#endif
		}
	}
}

void GSDrawScanline::CDrawEdge(int pixels, int left, int top, const GSVertexSW& scan, GSScanlineLocalData& local)
{
	GSScanlineSelector sel = local.gd->sel;
	sel.zwrite = 0;
	sel.edge = 1;
	CDrawScanline(pixels, left, top, scan, local, sel);
}

template <class T, bool masked>
__ri static void FillRect(const GSOffset& off, const GSVector4i& r, u32 c, u32 m, GSScanlineLocalData& local)
{
	if (r.x >= r.z)
		return;

	T* vm = (T*)GlobalFromLocal(local).vm;

	for (int y = r.y; y < r.w; y++)
	{
		GSOffset::PAHelper pa = off.paMulti(0, y);

		for (int x = r.x; x < r.z; x++)
		{
			T& d = vm[pa.value(x)];
			d = (T)(!masked ? c : (c | (d & m)));
		}
	}
}

#if _M_SSE >= 0x501

template <class T, bool masked>
__ri static void FillBlock(const GSOffset& off, const GSVector4i& r, const GSVector8i& c, const GSVector8i& m, GSScanlineLocalData& local)
{
	if (r.x >= r.z)
		return;

	T* vm = (T*)GlobalFromLocal(local).vm;

	for (int y = r.y; y < r.w; y += 8)
	{
		for (int x = r.x; x < r.z; x += 8 * 4 / sizeof(T))
		{
			GSVector8i* RESTRICT p = (GSVector8i*)&vm[off.pa(x, y)];

			p[0] = !masked ? c : (c | (p[0] & m));
			p[1] = !masked ? c : (c | (p[1] & m));
			p[2] = !masked ? c : (c | (p[2] & m));
			p[3] = !masked ? c : (c | (p[3] & m));
			p[4] = !masked ? c : (c | (p[4] & m));
			p[5] = !masked ? c : (c | (p[5] & m));
			p[6] = !masked ? c : (c | (p[6] & m));
			p[7] = !masked ? c : (c | (p[7] & m));
		}
	}
}

#else

template <class T, bool masked>
__ri static void FillBlock(const GSOffset& off, const GSVector4i& r, const GSVector4i& c, const GSVector4i& m, GSScanlineLocalData& local)
{
	if (r.x >= r.z)
		return;

	T* vm = (T*)GlobalFromLocal(local).vm;

	for (int y = r.y; y < r.w; y += 8)
	{
		GSOffset::PAHelper pa = off.paMulti(0, y);

		for (int x = r.x; x < r.z; x += 8 * 4 / sizeof(T))
		{
			GSVector4i* RESTRICT p = (GSVector4i*)&vm[pa.value(x)];

			for (int i = 0; i < 16; i += 4)
			{
				p[i + 0] = !masked ? c : (c | (p[i + 0] & m));
				p[i + 1] = !masked ? c : (c | (p[i + 1] & m));
				p[i + 2] = !masked ? c : (c | (p[i + 2] & m));
				p[i + 3] = !masked ? c : (c | (p[i + 3] & m));
			}
		}
	}
}

#endif

template <class T, bool masked>
__ri static void DrawRectT(const GSOffset& off, const GSVector4i& r, u32 c, u32 m, GSScanlineLocalData& local)
{
	if (m == 0xffffffff)
		return;

#if _M_SSE >= 0x501

	GSVector8i color((int)c);
	GSVector8i mask((int)m);

#else

	GSVector4i color((int)c);
	GSVector4i mask((int)m);

#endif

	if (sizeof(T) == sizeof(u16))
	{
		color = color.xxzzlh();
		mask = mask.xxzzlh();
		c = (c & 0xffff) | (c << 16);
		m = (m & 0xffff) | (m << 16);
	}

	color = color.andnot(mask);
	c = c & (~m);

	if (masked)
		pxAssert(mask.U32[0] != 0);

	GSVector4i br = r.ralign<Align_Inside>(GSVector2i(8 * 4 / sizeof(T), 8));

	if (!br.rempty())
	{
		FillRect<T, masked>(off, GSVector4i(r.x, r.y, r.z, br.y), c, m, local);
		FillRect<T, masked>(off, GSVector4i(r.x, br.w, r.z, r.w), c, m, local);

		if (r.x < br.x || br.z < r.z)
		{
			FillRect<T, masked>(off, GSVector4i(r.x, br.y, br.x, br.w), c, m, local);
			FillRect<T, masked>(off, GSVector4i(br.z, br.y, r.z, br.w), c, m, local);
		}

		FillBlock<T, masked>(off, br, color, mask, local);
	}
	else
	{
		FillRect<T, masked>(off, r, c, m, local);
	}
}

void GSDrawScanline::DrawRect(const GSVector4i& r, const GSVertexSW& v, GSScanlineLocalData& local)
{
	const GSScanlineGlobalData& global = GlobalFromLocal(local);
	pxAssert(r.y >= 0);
	pxAssert(r.w >= 0);

	// FIXME: sometimes the frame and z buffer may overlap, the outcome is undefined

	u32 m;

#if _M_SSE >= 0x501
	m = global.zm;
#else
	m = global.zm.U32[0];
#endif

	if (m != 0xffffffff)
	{
		u32 z = v.t.U32[3]; // (u32)v.p.z;

		if (global.sel.zpsm != 2)
		{
			if (m == 0)
			{
				DrawRectT<u32, false>(global.zbo, r, z, m, local);
			}
			else
			{
				DrawRectT<u32, true>(global.zbo, r, z, m, local);
			}
		}
		else
		{
			if ((m & 0xffff) == 0)
			{
				DrawRectT<u16, false>(global.zbo, r, z, m, local);
			}
			else
			{
				DrawRectT<u16, true>(global.zbo, r, z, m, local);
			}
		}
	}

#if _M_SSE >= 0x501
	m = global.fm;
#else
	m = global.fm.U32[0];
#endif

	if (m != 0xffffffff)
	{
		u32 c = (GSVector4i(v.c) >> 7).rgba32();

		if (global.sel.fba)
		{
			c |= 0x80000000;
		}

		if (global.sel.fpsm != 2)
		{
			if (m == 0)
			{
				DrawRectT<u32, false>(global.fbo, r, c, m, local);
			}
			else
			{
				DrawRectT<u32, true>(global.fbo, r, c, m, local);
			}
		}
		else
		{
			c = ((c & 0xf8) >> 3) | ((c & 0xf800) >> 6) | ((c & 0xf80000) >> 9) | ((c & 0x80000000) >> 16);

			if ((m & 0xffff) == 0)
			{
				DrawRectT<u16, false>(global.fbo, r, c, m, local);
			}
			else
			{
				DrawRectT<u16, true>(global.fbo, r, c, m, local);
			}
		}
	}
}
