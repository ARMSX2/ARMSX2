// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// TODO: JIT Draw* (flags: depth, texture, color (+iip), scissor)

#include "GS/Renderers/SW/GSRasterizer.h"
#include "GS/Renderers/SW/GSDrawScanline.h"
#include "GS/Renderers/SW/GSBlockWalk.h"
#include "GS/Renderers/SW/GSColourWalk.h"
#include "GS/Renderers/SW/GSDepthWalk.h"
#include "GS/Renderers/SW/GSCoordinateWalk.h"
#include "GS/GSExtra.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"

#include "common/AlignedMalloc.h"
#include "common/Console.h"
#include "common/StringUtil.h"
#include <cstring>

#define ENABLE_DRAW_STATS 0

MULTI_ISA_UNSHARED_IMPL;

// The GS steps depth by the exact gradient truncated onto a 2^-10 grid, not by
// the exact gradient. Walking the exact value is more precise than the hardware
// and lands one unit off on long spans, which is the whole margin a depth test
// between consecutive Z levels decides on.
//
// Truncation is toward zero, not floor (a sign-magnitude divider's behaviour).
// The two agree on rising gradients; on falling ones a floor makes the step
// steeper and the error accumulates to two units.
//
// Applied where the gradient is formed so every SetupPrim backend inherits it.
__forceinline static void TruncateDepthGradient(GSVector4& p)
{
	p.F64[1] = std::trunc(p.F64[1] * 1024.0) / 1024.0;
}

// The GS does not divide to form a colour or fog gradient. It multiplies by the
// reciprocal of the setup's cross product, read from a table eight significant
// bits wide and truncated (not rounded). The denominator is the cross product,
// not an edge length: the two only coincide for power-of-two heights.
//
// Computed in double so the reciprocal's own rounding is negligible, then the
// mantissa is truncated toward zero to eight significant bits (the low 45
// fraction bits of a binary64 cleared). The conversion to float is exact.
//
// -ffp-contract=fast cannot touch the truncation: it is an integer mask.
__forceinline static GSVector4 TruncatedSetupReciprocal(const GSVector4& cross)
{
	double r = 1.0 / static_cast<double>(cross.x);
	u64 bits;

	std::memcpy(&bits, &r, sizeof(bits));
	bits &= ~((static_cast<u64>(1) << 45) - 1);
	std::memcpy(&r, &bits, sizeof(r));

	return GSVector4(static_cast<float>(r));
}

// The depth gradients are formed from the plane in double, not from the float32
// coefficients the colour and texture lanes use. The float32 coefficient's
// rounding error changes sign with the triangle's shape, so the 2^-10 truncation
// could land a step above or below the true gradient depending on where the span
// began. The hardware lands integer depths one below the plane's value.
//
// In double the numerators are exact (32-bit z delta times a 12.4 position delta
// fits 53 bits) and the one division is correctly rounded, so the truncated step
// depends only on the plane.
__forceinline static void FormDepthGradients(const GSVector4& dv0p, double dv0z, const GSVector4& dv1p, double dv1z, double& dscan_z, double& dedge_z)
{
	const double d0x = dv0p.x, d0y = dv0p.y, d1x = dv1p.x, d1y = dv1p.y;
	// The setup's "cross" is the negated cross product; keep its sign so the two
	// gradients keep theirs.
	const double cross = d0y * d1x - d0x * d1y;
	dscan_z = (dv1z * d0y - dv0z * d1y) / cross;
	dedge_z = (dv0z * d1x - dv1z * d0x) / cross;
}

int GSRasterizerData::s_counter = 0;

static int compute_best_thread_height(int threads)
{
	// - for more threads screen segments should be smaller to better distribute the pixels
	// - but not too small to keep the threading overhead low
	// - ideal value between 3 and 5, or log2(64 / number of threads)

	int th = GSConfig.SWExtraThreadsHeight;

	if (th > 0 && th < 9)
		return th;
	else
		return 4;
}

GSRasterizer::GSRasterizer(GSDrawScanline* ds, int id, int threads)
	: m_ds(ds)
	, m_id(id)
	, m_threads(threads)
	, m_scanmsk_value(0)
{
	memset(&m_pixels, 0, sizeof(m_pixels));
	m_primcount = 0;

	m_thread_height = compute_best_thread_height(threads);

	m_edge.buff = static_cast<GSVertexSW*>(_aligned_malloc(sizeof(GSVertexSW) * 2048, VECTOR_ALIGNMENT));
	m_edge.count = 0;
	if (!m_edge.buff)
		pxFailRel("failed to allocate storage for m_edge.buff");

	int rows = (2048 >> m_thread_height) + 16;
	m_scanline = (u8*)_aligned_malloc(rows, 64);

	for (int i = 0; i < rows; i++)
	{
		m_scanline[i] = (i % threads) == id ? 1 : 0;
	}
}

GSRasterizer::~GSRasterizer()
{
	_aligned_free(m_scanline);
	_aligned_free(m_edge.buff);
}

static void __fi AddScanlineInfo(GSVertexSW* e, int pixels, int left, int top)
{
	e->_pad.I32[0] = pixels;
	e->_pad.I32[1] = left;
	e->_pad.I32[2] = top;
}

bool GSRasterizer::IsOneOfMyScanlines(int top) const
{
	pxAssert(top >= 0 && top < 2048);

	return m_scanline[top >> m_thread_height] != 0;
}

bool GSRasterizer::IsOneOfMyScanlines(int top, int bottom) const
{
	pxAssert(top >= 0 && top < 2048 && bottom >= 0 && bottom < 2048);

	top = top >> m_thread_height;
	bottom = (bottom + (1 << m_thread_height) - 1) >> m_thread_height;

	while (top < bottom)
	{
		if (m_scanline[top++])
		{
			return true;
		}
	}

	return false;
}

int GSRasterizer::FindMyNextScanline(int top) const
{
	int i = top >> m_thread_height;

	if (m_scanline[i] == 0)
	{
		while (m_scanline[++i] == 0)
			;

		top = i << m_thread_height;
	}

	return top;
}

int GSRasterizer::GetPixels(bool reset)
{
	int pixels = m_pixels.sum;

	if (reset)
	{
		m_pixels.sum = 0;
	}

	return pixels;
}

void GSRasterizer::Draw(GSRasterizerData& data)
{
	if ((data.vertex && data.vertex_count == 0) || (data.index && data.index_count == 0))
		return;

	m_pixels.actual = 0;
	m_pixels.total = 0;
	m_primcount = 0;

	if constexpr (ENABLE_DRAW_STATS)
		data.start = GetCPUTicks();

	m_setup_prim = data.setup_prim;
	m_draw_scanline = data.draw_scanline;
	m_draw_edge = data.draw_edge;
	GSDrawScanline::BeginDraw(data, m_local);

	const GSVertexSW* vertex = data.vertex;
	const GSVertexSW* vertex_end = data.vertex + data.vertex_count;

	const u16* index = data.index;
	const u16* index_end = data.index + data.index_count;

	static constexpr u16 tmp_index[] = {0, 1, 2};

	bool scissor_test = !data.bbox.eq(data.bbox.rintersect(data.scissor));

	m_scissor = data.scissor;
	m_fscissor_x = GSVector4(data.scissor).xzxz();
	m_fscissor_y = GSVector4(data.scissor).ywyw();
	m_scanmsk_value = data.scanmsk_value;

	switch (data.primclass)
	{
		case GS_POINT_CLASS:

			if (scissor_test)
			{
				DrawPoint<true>(vertex, data.vertex_count, index, data.index_count);
			}
			else
			{
				DrawPoint<false>(vertex, data.vertex_count, index, data.index_count);
			}

			break;

		case GS_LINE_CLASS:

			if (index != NULL)
			{
				do
				{
					DrawLine(vertex, index);
					index += 2;
				} while (index < index_end);
			}
			else
			{
				do
				{
					DrawLine(vertex, tmp_index);
					vertex += 2;
				} while (vertex < vertex_end);
			}

			break;

		case GS_TRIANGLE_CLASS:

			if (index != NULL)
			{
				do
				{
					DrawTriangle(vertex, index);
					index += 3;
				} while (index < index_end);
			}
			else
			{
				do
				{
					DrawTriangle(vertex, tmp_index);
					vertex += 3;
				} while (vertex < vertex_end);
			}

			break;

		case GS_SPRITE_CLASS:

			if (index != NULL)
			{
				do
				{
					DrawSprite(vertex, index);
					index += 2;
				} while (index < index_end);
			}
			else
			{
				do
				{
					DrawSprite(vertex, tmp_index);
					vertex += 2;
				} while (vertex < vertex_end);
			}

			break;

		default:
			ASSUME(0);
	}

#if _M_SSE >= 0x501
	_mm256_zeroupper();
#endif

	data.pixels = m_pixels.actual;

	m_pixels.sum += m_pixels.actual;

	if constexpr (ENABLE_DRAW_STATS)
		m_ds->UpdateDrawStats(data.frame, GetCPUTicks() - data.start, m_pixels.actual, m_pixels.total, m_primcount);
}

template <bool scissor_test>
void GSRasterizer::DrawPoint(const GSVertexSW* vertex, int vertex_count, const u16* index, int index_count)
{
	m_primcount++;

	if (index)
	{
		for (int i = 0; i < index_count; i++, index++)
		{
			const GSVertexSW& v = vertex[*index];

			GSVector4i p(v.p + GSVector4(0.5f));

			if (!scissor_test || (m_scissor.left <= p.x && p.x < m_scissor.right && m_scissor.top <= p.y && p.y < m_scissor.bottom))
			{
				if (IsOneOfMyScanlines(p.y))
				{
					SetupPrim(vertex, index, GSVertexSW::zero(), false);

					DrawScanline(1, p.x, p.y, v);
				}
			}
		}
	}
	else
	{
		static constexpr u16 tmp_index[1] = {0};

		for (int i = 0; i < vertex_count; i++, vertex++)
		{
			const GSVertexSW& v = vertex[0];

			GSVector4i p(v.p + GSVector4(0.5f));

			if (!scissor_test || (m_scissor.left <= p.x && p.x < m_scissor.right && m_scissor.top <= p.y && p.y < m_scissor.bottom))
			{
				if (IsOneOfMyScanlines(p.y))
				{
					SetupPrim(vertex, tmp_index, GSVertexSW::zero(), false);

					DrawScanline(1, p.x, p.y, v);
				}
			}
		}
	}
}

// Note: this should only be used for the edge drawing functions.
__forceinline static GSVertexSW ClampVertex(GSVertexSW v, int zpsm)
{
	v.c = v.c.sat(255.0f * 128.0f); // RGBA

	v.t = v.t.blend32<8>(v.t.sat(255.0f * 128.0f)); // F

	v.p.F64[1] = std::clamp(v.p.F64[1], 0.0, zpsm ? 0xFFFFFF.0p0 : 0xFFFFFFFF.0p0); // Z

	return v;
}

template <bool step_x, bool pos_x, bool pos_y, bool tl, bool side>
void GSRasterizer::DrawEdgeTriangle(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& dv,
	const GSVector4i& efun1, const GSVector4i& efun2)
{
	constexpr int dxi = pos_x ? 1 : -1;
	constexpr int dyi = pos_y ? 1 : -1;

	const float delta_x = dv.p.x;
	const float delta_y = dv.p.y;

	if (delta_x == 0.0f && delta_y == 0.0f)
		return;

	const float x0 = v0.p.x;
	const float y0 = v0.p.y;
	const float x1 = v1.p.x;
	const float y1 = v1.p.y;

	const float rx0 = pos_x ? std::ceil(x0 - 1.0f) : std::floor(x0 + 1.0f);
	const float ry0 = pos_y ? std::ceil(y0 - 1.0f) : std::floor(y0 + 1.0f);
	const float rx1 = pos_x ? std::floor(x1 + 1.0f) : std::ceil(x1 - 1.0f);
	const float ry1 = pos_y ? std::floor(y1 + 1.0f) : std::ceil(y1 - 1.0f);

	const int rxi0 = static_cast<int>(rx0);
	const int ryi0 = static_cast<int>(ry0);
	const int rxi1 = static_cast<int>(rx1);
	const int ryi1 = static_cast<int>(ry1);

	// AA1 widens a side by one pixel along its steeper axis, so a vertical side puts
	// a zero-coverage column one pixel outside the x extent just as a horizontal side
	// puts a zero-coverage row outside the y extent. Both bounds carry the same slack,
	// and no further.
	int bxi0 = static_cast<int>(std::ceil(std::min(x0, x1) - 1.0f));
	int byi0 = static_cast<int>(std::ceil(std::min(y0, y1) - 1.0f));
	int bxi1 = static_cast<int>(std::floor(std::max(x0, x1) + 1.0f));
	int byi1 = static_cast<int>(std::floor(std::max(y0, y1) + 1.0f));

	// Combine with scissor region.
	bxi0 = std::max(bxi0, m_scissor.x);
	byi0 = std::max(byi0, m_scissor.y);
	bxi1 = std::min(bxi1, m_scissor.z - 1); // b has inclusive coordinates.
	byi1 = std::min(byi1, m_scissor.w - 1); // b has inclusive coordinates.

	const GSVertexSW dedge = dv / GSVector4(std::abs(step_x ? delta_x : delta_y));

	GSVertexSW edge(v0);

	GSVertexSW* RESTRICT e = &m_edge.buff[m_edge.count];

	// Decision value for stepping the dependent direction.
	// D is the fractional part of dependent coordinate scaled by scaleD.
	constexpr bool pos_D = step_x ? pos_y : pos_x;
	const int scaleD = static_cast<int>(2 * 16 * 16 * std::abs(step_x ? delta_x : delta_y));
	const float scaleDf = static_cast<float>(scaleD);
	const int dD = static_cast<int>(2 * 16 * 16 * (step_x ? delta_y : delta_x));
	int D = static_cast<int>(scaleD * (step_x ? (y0 - ry0) : (x0 - rx0)));

	// Stepping variables
	int xi = rxi0;
	int yi = ryi0;
	int e1 = efun1.x * xi + efun1.y * yi + efun1.z;
	int e2 = efun2.x * xi + efun2.y * yi + efun2.z;

	const auto StepDependent = [&]<int sign>() {
		D -= scaleD * sign;
		xi += (step_x ? 0 : 1) * sign;
		yi += (step_x ? 1 : 0) * sign;
		e1 += (step_x ? efun1.y : efun1.x) * sign;
		e2 += (step_x ? efun2.y : efun2.x) * sign;
	};

	// Pre-steps
	const float prestep = step_x ? dxi * (rx0 - x0) : dyi * (ry0 - y0);
	edge += dedge * GSVector4(prestep);
	D += static_cast<int>(dD * prestep);
	
	while (D >= scaleD / 2)
		StepDependent.template operator()<1>();

	while (D < -scaleD / 2)
		StepDependent.template operator()<-1>();

	const int zpsm = m_local.gd->sel.zpsm;

	while (true)
	{
		const float d = static_cast<float>(D) / scaleDf;

		// Coverage and coordinates for anti-aliased point.
		int cov, xi2, yi2, e12, e22;

		const auto GetOffsetVars = [&]<int offset>() {
			xi2 = xi + (step_x ? 0 : offset);
			yi2 = yi + (step_x ? offset : 0);
			e12 = e1 + (step_x ? efun1.y : efun1.x) * offset;
			e22 = e2 + (step_x ? efun2.y : efun2.x) * offset;
		};

		if (d > 0.0f)
		{
			cov = static_cast<int>(0xffff * (side ? 1.0 - d : d));
			[[maybe_unused]] constexpr int offset = (side ? 0 : 1);
			GetOffsetVars.template operator()<offset>();
		}
		else if (d < 0.0f)
		{
			cov = static_cast<int>(0xffff * (side ? -d : 1.0 + d));
			[[maybe_unused]] constexpr int offset = (side ? -1 : 0);
			GetOffsetVars.template operator()<offset>();
		}
		else // d == 0.0f
		{
			// When exactly on the pixel center, top-left edges can create 0 coverage points and
			// bottom-right edges can create full coverage points (with some rounding error).
			cov = tl ? 0 : 0xffff;
			[[maybe_unused]] constexpr int offset = tl ? (side ? -1 : 1) : 0;
			GetOffsetVars.template operator()<offset>();
		}

		if (e12 > 0 && e22 > 0 &&
			bxi0 <= xi2 && xi2 <= bxi1 &&
			byi0 <= yi2 && yi2 <= byi1 &&
			IsOneOfMyScanlines(yi2))
		{
			// Clamping here as some values may be extrapolated outside
			// the allowed ranges. This is suggested by hardware tests though it
			// may not be totally accurate to do it here.
			AddScanline(e, 1, xi2, yi2, ClampVertex(edge, zpsm));

			e->p.U32[0] = std::clamp(cov, 0, 0xffff);

			e++;
		}
		
		if (step_x ? (xi == rxi1) : (yi == ryi1))
			break;

		// Step driving axis.
		edge += dedge;
		D += dD;
		xi += step_x ? dxi : 0;
		yi += step_x ? 0 : dyi;
		e1 += step_x ? (dxi * efun1.x) : (dyi * efun1.y);
		e2 += step_x ? (dxi * efun2.x) : (dyi * efun2.y);

		// Step dependent axis.
		if constexpr (pos_D)
		{
			if (D >= scaleD / 2)
				StepDependent.template operator()<1>();
		}
		else
		{
			if (D < -scaleD / 2)
				StepDependent.template operator()<-1>();
		}
	}

	m_edge.count += e - &m_edge.buff[m_edge.count];
}

template <bool step_x, bool pos_x, bool pos_y, bool aa>
void GSRasterizer::DrawEdgeLine(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& dv)
{
	constexpr int dxi = pos_x ? 1 : -1;
	constexpr int dyi = pos_y ? 1 : -1;

	const float delta_x = dv.p.x;
	const float delta_y = dv.p.y;

	const float x0 = v0.p.x;
	const float y0 = v0.p.y;
	const float x1 = v1.p.x;
	const float y1 = v1.p.y;

	float rx0 = std::floor(x0 + 0.5f);
	float ry0 = std::floor(y0 + 0.5f);
	float rx1 = std::floor(x1 + 0.5f);
	float ry1 = std::floor(y1 + 0.5f);

	// Diamond exit rule for determining coverage of first/last pixel.
	const auto TestEndpoint = [](float dx, float dy) -> bool {
		float dist = std::abs(dx) + std::abs(dy);
		if (dist < 0.5f)
			return false;
		if constexpr (step_x)
		{
			const bool x_good = pos_x ? (dx > 0.0f) : (dx < 0.0f);
			return x_good && (dist > 0.5f || dy >= 0.0f);
		}
		else
		{
			const bool y_good = pos_y ? (dy > 0.0f) : (dy < 0.0f);
			return y_good && (dist > 0.5f || dx >= 0.0f);
		}
	};

	const bool draw_first = !TestEndpoint(x0 - rx0, y0 - ry0);
	const bool draw_last = TestEndpoint(x1 - rx1, y1 - ry1);

	if (!draw_first)
	{
		rx0 += step_x ? dxi : 0.0f;
		ry0 += step_x ? 0.0f : dyi;
	}

	if (!draw_last)
	{
		rx1 -= step_x ? dxi : 0.0f;
		ry1 -= step_x ? 0.0f : dyi;
	}

	if ((step_x ? (dxi * (rx1 - rx0)) : (dyi * (ry1 - ry0))) < 0.0f)
		return;

	const int rxi0 = static_cast<int>(rx0);
	const int ryi0 = static_cast<int>(ry0);
	const int rxi1 = static_cast<int>(rx1);
	const int ryi1 = static_cast<int>(ry1);

	// Early exit for horizontal lines.
	if (delta_y == 0.0f && !IsOneOfMyScanlines(ryi0) && !aa)
		return;

	const GSVertexSW dedge = dv / GSVector4(std::abs(step_x ? delta_x : delta_y));
	
	GSVertexSW edge(v0);

	GSVertexSW* RESTRICT e = &m_edge.buff[m_edge.count];

	// Decision value for stepping the dependent direction.
	// D is the fractional part of dependent coordinate scaled by scaleD.
	constexpr bool pos_D = step_x ? pos_y : pos_x;
	const int scaleD = static_cast<int>(2 * 16 * 16 * std::abs(step_x ? delta_x : delta_y));
	const float scaleDf = static_cast<float>(scaleD);
	const int dD = static_cast<int>(2 * 16 * 16 * (step_x ? delta_y : delta_x));
	int D = static_cast<int>(scaleD * (step_x ? (y0 - ry0) : (x0 - rx0)));

	// Stepping variables
	int xi = rxi0;
	int yi = ryi0;

	const auto StepDependent = [&]<int sign>() {
		D -= scaleD * sign;
		xi += (step_x ? 0 : 1) * sign;
		yi += (step_x ? 1 : 0) * sign;
	};

	// Pre-steps
	const float prestep = step_x ? dxi * (rx0 - x0) : dyi * (ry0 - y0);
	edge += dedge * GSVector4(prestep);
	D += static_cast<int>(dD * prestep);

	while (D >= scaleD / 2)
		StepDependent.template operator()<1>();
	
	while (D < -scaleD / 2)
		StepDependent.template operator()<-1>();

	const int zpsm = m_local.gd->sel.zpsm;

	const auto AddScanlineStepEdge = [&](int x, int y, int cov = 0) {
		if (m_scissor.left <= x && x < m_scissor.right &&
			m_scissor.top <= y && y < m_scissor.bottom &&
			IsOneOfMyScanlines(y))
		{
			// Clamping here as some values may be extrapolated outside
			// the allowed ranges. This is suggested by hardware tests though it
			// may not be totally accurate to do it here.
			AddScanline(e, 1, x, y, ClampVertex(edge, zpsm));

			if constexpr (aa)
				e->p.U32[0] = cov;

			e++;
		}
	};

	while (true)
	{
		if constexpr (aa)
		{
			const float cov = 0xffff * std::abs(static_cast<float>(D) / scaleDf);
			const int covi = std::clamp(static_cast<int>(cov), 0, 0xffff);
			const int offset = D >= 0 ? 1 : -1;

			AddScanlineStepEdge(xi, yi, 0xffff - covi);
			AddScanlineStepEdge(xi + (step_x ? 0 : offset), yi + (step_x ? offset : 0), covi);
		}
		else
		{
			AddScanlineStepEdge(xi, yi);
		}

		if (step_x ? (xi == rxi1) : (yi == ryi1))
			break;

		// Step driving axis.
		edge += dedge;
		D += dD;
		xi += step_x ? dxi : 0;
		yi += step_x ? 0 : dyi;

		// Step dependent axis.
		if constexpr (pos_D)
		{
			if (D >= scaleD / 2)
				StepDependent.template operator()<1>();
		}
		else
		{
			if (D < -scaleD / 2)
				StepDependent.template operator()<-1>();
		}
	}

	m_edge.count += e - &m_edge.buff[m_edge.count];
}

void GSRasterizer::DrawEdgeTriangle(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& dv, const GSVector4i& efun1, const GSVector4i& efun2, bool tl)
{
	const bool step_x = std::abs(dv.p.x) >= std::abs(dv.p.y);
	const bool pos_x = dv.p.x >= 0.0f;
	const bool pos_y = dv.p.y >= 0.0f;
	
	// side == true => outside of triangle is towards top or left.
	// side == false => outside of triangle is towards bottom or right.
	const bool side = tl ^ (step_x && (dv.p.y != 0.0f) && (pos_x == pos_y));

	(this->*m_draw_edge_triangle[step_x][pos_x][pos_y][tl][side])(v0, v1, dv, efun1, efun2);
}

void GSRasterizer::DrawEdgeLine(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& dv, bool has_edge)
{
	const bool step_x = std::abs(dv.p.x) >= std::abs(dv.p.y);
	const bool pos_x = dv.p.x >= 0.0f;
	const bool pos_y = dv.p.y >= 0.0f;

	(this->*m_draw_edge_line[step_x][pos_x][pos_y][has_edge])(v0, v1, dv);

	return;
}

void GSRasterizer::DrawLine(const GSVertexSW* vertex, const u16* index)
{
	m_primcount++;

	const GSVertexSW& v0 = vertex[index[0]];
	const GSVertexSW& v1 = vertex[index[1]];

	GSVertexSW dv = v1 - v0;

	DrawEdgeLine(v0, v1, dv, HasEdge());

	Flush(vertex, index, GSVertexSW::zero(), false, HasEdge());

	return;
}

static const u8 s_ysort[8][4] =
{
	{0, 1, 2, 0}, // y0 <= y1 <= y2
	{1, 0, 2, 0}, // y1 < y0 <= y2
	{0, 0, 0, 0},
	{1, 2, 0, 0}, // y1 <= y2 < y0
	{0, 2, 1, 0}, // y0 <= y2 < y1
	{0, 0, 0, 0},
	{2, 0, 1, 0}, // y2 < y0 <= y1
	{2, 1, 0, 0}, // y2 < y1 < y0
};

#if _M_SSE >= 0x501

void GSRasterizer::DrawTriangle(const GSVertexSW* vertex, const u16* index)
{
	m_primcount++;

	GSVertexSW2 edge;
	GSVertexSW2 dedge;
	GSVertexSW2 dscan;
	// The section's left edge's top vertex: the point the attribute plane is
	// evaluated from.
	GSVertexSW2 ledge;

	// Stages 1 and 2 of the primitive-grain rule (GSCoordinateWalk.h), mirroring
	// the scalar path. Per primitive, not per draw: the grain comes from the largest
	// of these three exponents, and strip neighbours need not share a grain.
	GSVertexSW grained[3];
	static constexpr u16 grained_index[3] = {0, 1, 2};
	GSCoordinateGrain grain;
	const bool takes_grain = (m_local.gd->coord_grain_floor[0] != 0);

	if (takes_grain)
	{
		const float half = m_local.gd->sel.ltf ? 32768.0f : 0.0f;

		grained[0] = vertex[index[0]];
		grained[1] = vertex[index[1]];
		grained[2] = vertex[index[2]];

		grain = GSCoordinateGrainOfPrimitive(grained[0].t, grained[1].t, grained[2].t,
			m_local.gd->coord_grain_floor, half);

		for (int j = 0; j < 3; j++)
			grained[j].t = GSCoordinateOnGrain(grained[j].t, grain, half);

		vertex = grained;
		index = grained_index;
	}

	GSVector4 y0011 = vertex[index[0]].p.yyyy(vertex[index[1]].p);
	GSVector4 y1221 = vertex[index[1]].p.yyyy(vertex[index[2]].p).xzzx();

	int m1 = (y0011 > y1221).mask() & 7;

	int i[3];

	i[0] = index[s_ysort[m1][0]];
	i[1] = index[s_ysort[m1][1]];
	i[2] = index[s_ysort[m1][2]];

	const GSVertexSW2* _v = (const GSVertexSW2*)vertex;

	const GSVertexSW2& v0 = _v[i[0]];
	const GSVertexSW2& v1 = _v[i[1]];
	const GSVertexSW2& v2 = _v[i[2]];

	y0011 = v0.p.yyyy(v1.p);
	y1221 = v1.p.yyyy(v2.p).xzzx();

	m1 = (y0011 == y1221).mask() & 7;

	// if (i == 0) => y0 < y1 < y2
	// if (i == 1) => y0 == y1 < y2
	// if (i == 4) => y0 < y1 == y2

	if (m1 == 7) // y0 == y1 == y2
		return;

	GSVector4 tbf = y0011.xzxz(y1221).ceil();
	GSVector4 tbmax = tbf.max(m_fscissor_y);
	GSVector4 tbmin = tbf.min(m_fscissor_y);
	GSVector4i tb = GSVector4i(tbmax.xzyw(tbmin)); // max(y0, t) max(y1, t) min(y1, b) min(y2, b)

	// Unclipped on purpose: the depth bias applies after the primitive's first
	// scanline, and the scissor rejects pixels without reseeding the walk. Using
	// the clipped top would make stored depth depend on the scissor.
	const int prim_top = GSVector4i(tbf).x;

	GSVertexSW2 dv0 = v1 - v0;
	GSVertexSW2 dv1 = v2 - v0;
	GSVertexSW2 dv2 = v2 - v1;

	GSVector4 cross = GSVector4::loadl(&dv0.p) * GSVector4::loadl(&dv1.p).yxwz();

	cross = (cross - cross.yxwz()).yyyy(); // select the second component, the negated cross product
	// the longest horizontal span would be cross.x / dv1.p.y, but we don't need its actual value

	int m2 = cross.upl(cross == GSVector4::zero()).mask();

	if (m2 & 2)
		return;

	m2 &= 1;

	GSVector4 dxy01 = dv0.p.xyxy(dv1.p);

	GSVector4 dx = dxy01.xzxy(dv2.p);
	GSVector4 dy = dxy01.ywyx(dv2.p);

	GSVector4 ddx[3];

	ddx[0] = dx / dy;
	ddx[1] = ddx[0].yxzw();
	ddx[2] = ddx[0].xzyw();

	// Precision is important here. Don't use reciprocal, it will break Jak3/Xenosaga1
	GSVector8 dxy01c(dxy01 / cross);

	dscan = dv1 * dxy01c.yyyy() - dv0 * dxy01c.wwww();
	dedge = dv0 * dxy01c.zzzz() - dv1 * dxy01c.xxxx();

	// ⚠️ AVX2 path, not built on ARM64. Everything from here to
	// TruncateDepthGradient must mirror the scalar path lane for lane.
	//
	// Colour and fog take the truncated reciprocal; s, t, q and position keep the
	// exact quotient. tc is t in lanes 0-3 and c in lanes 4-7, so the blend takes
	// lane 3 (fog) and lanes 4-7 (colour).
	{
		const GSVector8 dxy01r(dxy01 * TruncatedSetupReciprocal(cross));
		const GSVector8 scan_r = dv1.tc * dxy01r.yyyy() - dv0.tc * dxy01r.wwww();
		const GSVector8 edge_r = dv0.tc * dxy01r.zzzz() - dv1.tc * dxy01r.xxxx();

		dscan.tc = dscan.tc.blend32<0xf8>(scan_r);
		dedge.tc = dedge.tc.blend32<0xf8>(edge_r);

		// Then truncated to an eighth of a colour unit (GSColourWalk.h), blended
		// back into lanes 3-7 the same way.
		const GSVector8 trunc_s(GSColourWalkTruncUnit(dscan.tc.extract<0>()),
			GSColourWalkTruncUnit(dscan.tc.extract<1>()));
		const GSVector8 trunc_e(GSColourWalkTruncUnit(dedge.tc.extract<0>()),
			GSColourWalkTruncUnit(dedge.tc.extract<1>()));

		dscan.tc = dscan.tc.blend32<0xf8>(trunc_s);
		dedge.tc = dedge.tc.blend32<0xf8>(trunc_e);
	}

	// Stage 3, as in the scalar path: the gradient pushed down onto a grid a
	// thousandth of the grain. Only the low half of tc (s, t, q, fog) is touched.
	if (takes_grain)
	{
		const GSVector4 stq = GSCoordinateGradientOnGrain(dscan.tc.extract<0>(), grain,
			GSSetupInvertsExactly(GSTriangleTwiceArea(v0.p, v1.p, v2.p)));

		dscan.tc = GSVector8(stq, dscan.tc.extract<1>());
	}

	// GSVertexSW2 has the same layout as GSVertexSW, so the walk setup reads it
	// through the scalar view. Block width: GSBlockWalk.h.
	GSSetupColourWalk(vertex[i[0]], vertex[i[1]], vertex[i[2]],
		reinterpret_cast<const GSVertexSW&>(dscan), reinterpret_cast<const GSVertexSW&>(dedge),
		GSBlockWalkWidth(m_local.gd->sel.tfx != TFX_NONE, m_local.gd->sel.fge != 0,
			m_local.gd->sel.aa1 != 0),
		m_local.cwalk);

	FormDepthGradients(dv0.p, dv0.p.F64[1], dv1.p, dv1.p.F64[1], dscan.p.F64[1], dedge.p.F64[1]);
	TruncateDepthGradient(dscan.p);

	if (m1 & 1)
	{
		if (tb.y < tb.w)
		{
			edge = _v[i[1 - m2]];

			edge.p.y = vertex[i[m2]].p.x;
			dedge.p = ddx[!m2 << 1].yzzw(dedge.p);

			ledge = _v[i[1 - m2]];

			DrawTriangleSection(tb.x, tb.w, prim_top, edge, dedge, dscan, vertex[i[1 - m2]].p, ledge);
		}
	}
	else
	{
		if (tb.x < tb.z)
		{
			edge = v0;

			edge.p.y = edge.p.x;
			dedge.p = ddx[m2].xyzw(dedge.p);

			// Both edges leave v0, so the left one's top vertex is v0 as well.
			ledge = v0;

			DrawTriangleSection(tb.x, tb.z, prim_top, edge, dedge, dscan, v0.p, ledge);
		}

		if (tb.y < tb.w)
		{
			edge = v1;

			edge.p = (v0.p.xxxx() + ddx[m2] * dv0.p.yyyy()).xyzw(edge.p);
			dedge.p = ddx[!m2 << 1].yzzw(dedge.p);

			// m2 == 0: the left edge is v1->v2, top vertex v1. m2 == 1: v1 is on
			// the right and the left edge is the long v0->v2 one, top vertex v0.
			ledge = m2 ? v0 : v1;

			DrawTriangleSection(tb.y, tb.w, prim_top, edge, dedge, dscan, v1.p, ledge);
		}
	}

	Flush(vertex, index, (GSVertexSW&)dscan, true);

	if (HasEdge())
	{
		const bool clockwise = (cross < GSVector4::zero()).mask();

		const bool tl0 = (v0.p.y == v1.p.y) || !clockwise;
		const bool tl1 = clockwise;
		const bool tl2 = (v1.p.y != v2.p.y) && !clockwise;

		const GSVector4i xy0 = GSVector4i(v0.p * GSVector4::cxpr(16.0f));
		const GSVector4i xy1 = GSVector4i(v1.p * GSVector4::cxpr(16.0f));
		const GSVector4i xy2 = GSVector4i(v2.p * GSVector4::cxpr(16.0f));

		GSVector4i f0 = (xy1 - xy0).yxyx().upl32(xy0 - xy1).sll32<4>();
		GSVector4i f1 = (xy0 - xy2).yxyx().upl32(xy2 - xy0).sll32<4>();
		GSVector4i f2 = (xy2 - xy1).yxyx().upl32(xy1 - xy2).sll32<4>();

		f0 = f0.insert32<2>(xy1.x * xy0.y - xy0.x * xy1.y);
		f1 = f1.insert32<2>(xy0.x * xy2.y - xy2.x * xy0.y);
		f2 = f2.insert32<2>(xy2.x * xy1.y - xy1.x * xy2.y);

		if (clockwise)
		{
			f0 = GSVector4i::cxpr(0) - f0;
			f1 = GSVector4i::cxpr(0) - f1;
			f2 = GSVector4i::cxpr(0) - f2;
		}

		// Bias for top-left edges.
		f0 += GSVector4i(0, 0, tl0, 0);
		f1 += GSVector4i(0, 0, tl1, 0);
		f2 += GSVector4i(0, 0, tl2, 0);

		DrawEdgeTriangle((GSVertexSW&)v0, (GSVertexSW&)v1, (GSVertexSW&)dv0, f1, f2, tl0);
		DrawEdgeTriangle((GSVertexSW&)v0, (GSVertexSW&)v2, (GSVertexSW&)dv1, f2, f0, tl1);
		DrawEdgeTriangle((GSVertexSW&)v1, (GSVertexSW&)v2, (GSVertexSW&)dv2, f0, f1, tl2);

		Flush(vertex, index, GSVertexSW::zero(), false, true);
	}
}

void GSRasterizer::DrawTriangleSection(int top, int bottom, int prim_top, GSVertexSW2& RESTRICT edge, const GSVertexSW2& RESTRICT dedge, const GSVertexSW2& RESTRICT dscan, const GSVector4& RESTRICT p0, const GSVertexSW2& RESTRICT ledge)
{
	pxAssert(top < bottom);
	pxAssert(edge.p.x <= edge.p.y);

	GSVertexSW* RESTRICT e = &m_edge.buff[m_edge.count];

	GSVector4 scissor = m_fscissor_x;

	top = FindMyNextScanline(top);

	while (top < bottom)
	{
		const float dy = static_cast<float>(top) - p0.y;
		GSVector8 dyv(dy);

		GSVector4 xy = GSVector4::loadl(&edge.p) + GSVector4::loadl(&dedge.p) * dyv.extract<0>();

		GSVector4 lrf = xy.ceil();
		GSVector4 l = lrf.max(scissor);
		GSVector4 r = lrf.min(scissor);
		GSVector4i lr = GSVector4i(l.xxyy(r));

		int left = lr.extract32<0>();
		int right = lr.extract32<2>();

		int pixels = right - left;

		if (pixels > 0)
		{
			float prestep = l.x - p0.x;

			// s, t and q are evaluated from the left edge's top vertex.
			GSVector8 ldyv(static_cast<float>(top) - ledge.p.y);
			GSVector8 lprestepv(l.x - ledge.p.x);

			// Colour and fog come from the primitive's walk (GSColourWalk.h). The
			// blend takes lane 3 (fog) and lanes 4-7 (colour).
			const GSColourWalk& cwalk = m_local.cwalk;
			const GSVector8 seed(GSColourWalkRowSeed(cwalk, cwalk.f, left, top),
				GSColourWalkRowSeed(cwalk, cwalk.c, left, top));

			reinterpret_cast<GSVertexSW2*>(e)->p.F64[1] = edge.p.F64[1] + dedge.p.F64[1] * dy + dscan.p.F64[1] * prestep
			                                             - GSDepthWalkBias(dscan.p.F64[1], dedge.p.F64[1], top != prim_top);
			reinterpret_cast<GSVertexSW2*>(e)->tc =
				(ledge.tc + dedge.tc * ldyv + dscan.tc * lprestepv).blend32<0xf8>(seed);

			AddScanlineInfo(e++, pixels, left, top);
		}

		top++;

		if (!IsOneOfMyScanlines(top))
		{
			top += (m_threads - 1) << m_thread_height;
		}
	}

	m_edge.count += e - &m_edge.buff[m_edge.count];
}

#else

// DrawTriangle's setup in one __noinline body so its arithmetic is compiled once.
// FMA contraction can move the z gradient by an ULP, which flips the truncated
// 2^-10 step by a unit, so a second copy of this code could disagree with it.
struct GSTriangleSetup
{
	GSVertexSW edge[2];
	GSVertexSW dedge[2];
	GSVertexSW dscan;
	GSVector4 p0[2];
	// The section's left edge's top vertex: the point the attribute plane is
	// evaluated from.
	GSVertexSW ledge[2];
	int top[2];
	int bottom[2];
	int nsections;
	int i[3];       // y-sorted vertex indices
	int top_prim;   // ceil(y) of the sorted top vertex, before the scissor clamp
	GSVector4 cross; // the (negated, broadcast) cross product, for the edge-AA orientation
};

__noinline static bool SetupTriangle(const GSVertexSW* vertex, const u16* index, const GSVector4& fscissor_y,
	GSTriangleSetup& out, GSColourWalk& cwalk, int block_width, const GSCoordinateGrain* grain)
{
	GSVector4 y0011 = vertex[index[0]].p.yyyy(vertex[index[1]].p);
	GSVector4 y1221 = vertex[index[1]].p.yyyy(vertex[index[2]].p).xzzx();

	int m1 = (y0011 > y1221).mask() & 7;

	out.i[0] = index[s_ysort[m1][0]];
	out.i[1] = index[s_ysort[m1][1]];
	out.i[2] = index[s_ysort[m1][2]];
	const int* i = out.i;

	const GSVertexSW& v0 = vertex[i[0]];
	const GSVertexSW& v1 = vertex[i[1]];
	const GSVertexSW& v2 = vertex[i[2]];

	y0011 = v0.p.yyyy(v1.p);
	y1221 = v1.p.yyyy(v2.p).xzzx();

	m1 = (y0011 == y1221).mask() & 7;

	// if (i == 0) => y0 < y1 < y2
	// if (i == 1) => y0 == y1 < y2
	// if (i == 4) => y0 < y1 == y2

	if (m1 == 7)
		return false; // y0 == y1 == y2

	GSVector4 tbf = y0011.xzxz(y1221).ceil();
	GSVector4 tbmax = tbf.max(fscissor_y);
	GSVector4 tbmin = tbf.min(fscissor_y);
	GSVector4i tb = GSVector4i(tbmax.xzyw(tbmin)); // max(y0, t) max(y1, t) min(y1, b) min(y2, b)

	// Unclipped on purpose: the depth bias applies after the primitive's first
	// scanline, and the scissor rejects pixels without reseeding the walk. Using
	// the clipped top would make stored depth depend on the scissor.
	out.top_prim = GSVector4i(tbf).extract32<0>(); // geometric first scanline, pre-scissor

	GSVertexSW dv0 = v1 - v0;
	GSVertexSW dv1 = v2 - v0;
	GSVertexSW dv2 = v2 - v1;

	GSVector4 cross = GSVector4::loadl(&dv0.p) * GSVector4::loadl(&dv1.p).yxwz();

	cross = (cross - cross.yxwz()).yyyy(); // select the second component, the negated cross product
	// the longest horizontal span would be cross.x / dv1.p.y, but we don't need its actual value

	int m2 = cross.upl(cross == GSVector4::zero()).mask();

	if (m2 & 2)
		return false;

	m2 &= 1;

	out.cross = cross;

	GSVector4 dxy01 = dv0.p.xyxy(dv1.p);

	GSVector4 dx = dxy01.xzxy(dv2.p);
	GSVector4 dy = dxy01.ywyx(dv2.p);

	GSVector4 ddx[3];

	ddx[0] = dx / dy;
	ddx[1] = ddx[0].yxzw();
	ddx[2] = ddx[0].xzyw();

	// Precision is important here. Don't use reciprocal, it will break Jak3/Xenosaga1
	GSVector4 dxy01c = dxy01 / cross;

	out.dscan = dv1 * dxy01c.yyyy() - dv0 * dxy01c.wwww();
	GSVertexSW dedge = dv0 * dxy01c.zzzz() - dv1 * dxy01c.xxxx();

	// Colour and fog are formed again on the truncated reciprocal; s, t, q and
	// position keep the exact quotient above. On hardware only colour and fog show
	// the truncated reciprocal; texture coordinates and depth do not. Depth is
	// overwritten from the plane below anyway; s, t, q must be kept apart from fog,
	// which shares their vector.
	{
		const GSVector4 dxy01r = dxy01 * TruncatedSetupReciprocal(cross);
		const GSVector4 scan_t = dv1.t * dxy01r.yyyy() - dv0.t * dxy01r.wwww();
		const GSVector4 edge_t = dv0.t * dxy01r.zzzz() - dv1.t * dxy01r.xxxx();

		out.dscan.c = dv1.c * dxy01r.yyyy() - dv0.c * dxy01r.wwww();
		dedge.c = dv0.c * dxy01r.zzzz() - dv1.c * dxy01r.xxxx();
		// blend32<8> keeps x, y, z -- s, t and q -- and takes only w, the fog.
		out.dscan.t = out.dscan.t.blend32<8>(scan_t);
		dedge.t = dedge.t.blend32<8>(edge_t);

		// Then truncated toward zero to an eighth of a colour unit
		// (GSColourWalk.h).
		out.dscan.c = GSColourWalkTruncUnit(out.dscan.c);
		dedge.c = GSColourWalkTruncUnit(dedge.c);
		out.dscan.t = out.dscan.t.blend32<8>(GSColourWalkTruncUnit(out.dscan.t));
		dedge.t = dedge.t.blend32<8>(GSColourWalkTruncUnit(dedge.t));
	}

	// Stage 3: the gradient pushed down onto a grid a thousandth of the grain --
	// strictly below where twice the area is not a power of two, plain floor where
	// it is. Done here so the scanline seed and both SetupPrim backends read one value.
	if (grain)
	{
		out.dscan.t = GSCoordinateGradientOnGrain(out.dscan.t, *grain,
			GSSetupInvertsExactly(GSTriangleTwiceArea(v0.p, v1.p, v2.p)));
	}

	// One anchor, walk direction and block grid for the whole primitive, both
	// sections included (GSColourWalk.h).
	GSSetupColourWalk(v0, v1, v2, out.dscan, dedge, block_width, cwalk);

	FormDepthGradients(dv0.p, dv0.p.F64[1], dv1.p, dv1.p.F64[1], out.dscan.p.F64[1], dedge.p.F64[1]);
	TruncateDepthGradient(out.dscan.p);

	out.nsections = 0;

	if (m1 & 1)
	{
		if (tb.y < tb.w)
		{
			GSVertexSW& edge = out.edge[0];

			edge = vertex[i[1 - m2]];

			edge.p.y = vertex[i[m2]].p.x;
			out.dedge[0] = dedge;
			out.dedge[0].p = ddx[!m2 << 1].yzzw(dedge.p);

			out.p0[0] = vertex[i[1 - m2]].p;
			out.ledge[0] = vertex[i[1 - m2]];
			out.top[0] = tb.x;
			out.bottom[0] = tb.w;
			out.nsections = 1;
		}
	}
	else
	{
		if (tb.x < tb.z)
		{
			const int n = out.nsections;
			GSVertexSW& edge = out.edge[n];

			edge = v0;

			edge.p.y = edge.p.x;
			out.dedge[n] = dedge;
			out.dedge[n].p = ddx[m2].xyzw(dedge.p);

			out.p0[n] = v0.p;
			// Both edges leave v0, so the left one's top vertex is v0 as well.
			out.ledge[n] = v0;
			out.top[n] = tb.x;
			out.bottom[n] = tb.z;
			out.nsections = n + 1;
		}

		if (tb.y < tb.w)
		{
			const int n = out.nsections;
			GSVertexSW& edge = out.edge[n];

			edge = v1;

			edge.p = (v0.p.xxxx() + ddx[m2] * dv0.p.yyyy()).xyzw(edge.p);
			out.dedge[n] = dedge;
			out.dedge[n].p = ddx[!m2 << 1].yzzw(dedge.p);

			out.p0[n] = v1.p;
			// m2 == 0: the left edge is v1->v2, top vertex v1. m2 == 1: v1 is on
			// the right and the left edge is the long v0->v2 one, top vertex v0.
			out.ledge[n] = m2 ? v0 : v1;
			out.top[n] = tb.y;
			out.bottom[n] = tb.w;
			out.nsections = n + 1;
		}
	}

	return true;
}

void GSRasterizer::DrawTriangle(const GSVertexSW* vertex, const u16* index)
{
	m_primcount++;

	// The block is eight pixels wide only when texturing, fog and AA1 are all off,
	// else four (GSBlockWalk.h). Fog uses the same walk as colour.
	const GSScanlineSelector sel = m_local.gd->sel;
	const int block_width = GSBlockWalkWidth(sel.tfx != TFX_NONE, sel.fge != 0, sel.aa1 != 0);

	// Stages 1 and 2 of the primitive-grain rule (GSCoordinateWalk.h), on copies of
	// this primitive's three vertices: the grain comes from the largest of these
	// three exponents, and strip neighbours need not share a grain.
	GSVertexSW grained[3];
	static constexpr u16 grained_index[3] = {0, 1, 2};
	GSCoordinateGrain grain;
	const bool takes_grain = (m_local.gd->coord_grain_floor[0] != 0);

	if (takes_grain)
	{
		// The linear filter's half texel is already off the vertex; the rule keys on
		// the coordinate, so it goes back on for the truncation and comes off after.
		const float half = sel.ltf ? 32768.0f : 0.0f;

		grained[0] = vertex[index[0]];
		grained[1] = vertex[index[1]];
		grained[2] = vertex[index[2]];

		grain = GSCoordinateGrainOfPrimitive(grained[0].t, grained[1].t, grained[2].t,
			m_local.gd->coord_grain_floor, half);

		for (int j = 0; j < 3; j++)
			grained[j].t = GSCoordinateOnGrain(grained[j].t, grain, half);

		vertex = grained;
		index = grained_index;
	}

	GSTriangleSetup s;
	if (!SetupTriangle(vertex, index, m_fscissor_y, s, m_local.cwalk, block_width,
			takes_grain ? &grain : nullptr))
		return;

	for (int n = 0; n < s.nsections; n++)
	{
		DrawTriangleSection(s.top[n], s.bottom[n], s.top_prim, s.edge[n], s.dedge[n], s.dscan, s.p0[n],
			s.ledge[n]);
	}

	Flush(vertex, index, s.dscan, true);

	if (HasEdge())
	{
		const GSVertexSW& v0 = vertex[s.i[0]];
		const GSVertexSW& v1 = vertex[s.i[1]];
		const GSVertexSW& v2 = vertex[s.i[2]];

		// Plain single-op subtractions: safe to recompute (no contraction to diverge on).
		const GSVertexSW dv0 = v1 - v0;
		const GSVertexSW dv1 = v2 - v0;
		const GSVertexSW dv2 = v2 - v1;

		const GSVector4 cross = s.cross;

		const bool clockwise = (cross < GSVector4::zero()).mask();

		const bool tl0 = (v0.p.y == v1.p.y) || !clockwise;
		const bool tl1 = clockwise;
		const bool tl2 = (v1.p.y != v2.p.y) && !clockwise;

		const GSVector4i xy0 = GSVector4i(v0.p * GSVector4::cxpr(16.0f));
		const GSVector4i xy1 = GSVector4i(v1.p * GSVector4::cxpr(16.0f));
		const GSVector4i xy2 = GSVector4i(v2.p * GSVector4::cxpr(16.0f));

		GSVector4i f0 = (xy1 - xy0).yxyx().upl32(xy0 - xy1).sll32<4>();
		GSVector4i f1 = (xy0 - xy2).yxyx().upl32(xy2 - xy0).sll32<4>();
		GSVector4i f2 = (xy2 - xy1).yxyx().upl32(xy1 - xy2).sll32<4>();

		f0 = f0.insert32<2>(xy1.x * xy0.y - xy0.x * xy1.y);
		f1 = f1.insert32<2>(xy0.x * xy2.y - xy2.x * xy0.y);
		f2 = f2.insert32<2>(xy2.x * xy1.y - xy1.x * xy2.y);

		if (clockwise)
		{
			f0 = GSVector4i::cxpr(0) - f0;
			f1 = GSVector4i::cxpr(0) - f1;
			f2 = GSVector4i::cxpr(0) - f2;
		}

		// Bias for top-left edges.
		f0 += GSVector4i(0, 0, tl0, 0);
		f1 += GSVector4i(0, 0, tl1, 0);
		f2 += GSVector4i(0, 0, tl2, 0);

		DrawEdgeTriangle(v0, v1, dv0, f1, f2, tl0);
		DrawEdgeTriangle(v0, v2, dv1, f2, f0, tl1);
		DrawEdgeTriangle(v1, v2, dv2, f0, f1, tl2);

		Flush(vertex, index, GSVertexSW::zero(), false, true);
	}
}

void GSRasterizer::DrawTriangleSection(int top, int bottom, int prim_top, GSVertexSW& RESTRICT edge, const GSVertexSW& RESTRICT dedge, const GSVertexSW& RESTRICT dscan, const GSVector4& RESTRICT p0, const GSVertexSW& RESTRICT ledge)
{
	pxAssert(top < bottom);
	pxAssert(edge.p.x <= edge.p.y);

	GSVertexSW* RESTRICT e = &m_edge.buff[m_edge.count];

	GSVector4 scissor = m_fscissor_x;

	top = FindMyNextScanline(top);

	while (top < bottom)
	{
		const float dy = static_cast<float>(top) - p0.y;

		GSVector4 xy = GSVector4::loadl(&edge.p) + GSVector4::loadl(&dedge.p) * dy;

		GSVector4 lrf = xy.ceil();
		GSVector4 l = lrf.max(scissor);
		GSVector4 r = lrf.min(scissor);
		GSVector4i lr = GSVector4i(l.xxyy(r));

		int left = lr.extract32<0>();
		int right = lr.extract32<2>();

		int pixels = right - left;

		if (pixels > 0)
		{
			const float prestep = l.x - p0.x;

			// s, t and q are evaluated from the left edge's top vertex.
			const float ldy = static_cast<float>(top) - ledge.p.y;
			const float lprestep = l.x - ledge.p.x;

			// Colour and fog come from the primitive's anchor, walked to this pixel
			// on the block grid (GSColourWalk.h). blend32<8> takes only w, the fog.
			const GSColourWalk& cwalk = m_local.cwalk;

			e->p.F64[1] = edge.p.F64[1] + dedge.p.F64[1] * dy + dscan.p.F64[1] * prestep
			              - GSDepthWalkBias(dscan.p.F64[1], dedge.p.F64[1], top != prim_top);
			e->t = (ledge.t + dedge.t * ldy + dscan.t * lprestep)
			           .blend32<8>(GSColourWalkRowSeed(cwalk, cwalk.f, left, top));
			e->c = GSColourWalkRowSeed(cwalk, cwalk.c, left, top);

			AddScanlineInfo(e++, pixels, left, top);
		}

		top++;

		if (!IsOneOfMyScanlines(top))
		{
			top += (m_threads - 1) << m_thread_height;
		}
	}

	m_edge.count += e - &m_edge.buff[m_edge.count];
}

#endif

void GSRasterizer::DrawSprite(const GSVertexSW* vertex, const u16* index)
{
	m_primcount++;

	const GSVertexSW& v0 = vertex[index[0]];
	const GSVertexSW& v1 = vertex[index[1]];

	GSVector4 mask = (v0.p < v1.p).xyzw(GSVector4::zero());

	GSVertexSW v[2];

	v[0].p = v1.p.blend32(v0.p, mask);
	v[0].t = v1.t.blend32(v0.t, mask);
	v[0].c = v1.c;

	v[1].p = v0.p.blend32(v1.p, mask);
	v[1].t = v0.t.blend32(v1.t, mask);

	GSVector4i r(v[0].p.xyxy(v[1].p).ceil());

	// The sprite's own extent before scissoring; the ramp term keys on it
	// (GSCoordinateWalk.h).
	const GSVector4i extent = r;

	r = r.rintersect(m_scissor);

	if (r.rempty())
		return;

	GSVertexSW scan = v[0];

	if ((m_scanmsk_value & 2) == 0 && m_local.gd->sel.IsSolidRect())
	{
		if (m_threads == 1)
		{
			GSDrawScanline::DrawRect(r, scan, m_local);

			int pixels = r.width() * r.height();

			m_pixels.actual += pixels;
			m_pixels.total += pixels;
		}
		else
		{
			int top = FindMyNextScanline(r.top);
			int bottom = r.bottom;

			while (top < bottom)
			{
				r.top = top;
				r.bottom = std::min<int>((top + (1 << m_thread_height)) & ~((1 << m_thread_height) - 1), bottom);

				GSDrawScanline::DrawRect(r, scan, m_local);

				int pixels = r.width() * r.height();

				m_pixels.actual += pixels;
				m_pixels.total += pixels;

				top = r.bottom + ((m_threads - 1) << m_thread_height);
			}
		}

		return;
	}

	GSVector4 dxy = v[1].p - v[0].p;
	GSVector4 duv = v[1].t - v[0].t;

	GSVector4 dt = duv / dxy;

	GSVertexSW dedge;
	GSVertexSW dscan;

	dedge.t = GSVector4::zero().insert32<1, 1>(dt);
	dscan.t = GSVector4::zero().insert32<0, 0>(dt);

	GSVector4 prestep = GSVector4(r.left, r.top) - scan.p;

	scan.t = (scan.t + dt * prestep).xyzw(scan.t);

	// A UV sprite's coordinate ramp runs a sixteenth of a texel low on any axis
	// whose extent is not a power of two, from that axis's second pixel on
	// (GSCoordinateWalk.h).
	const float ramp_u = m_local.gd->sel.fst ? GSSpriteRampBias(dt.x, extent.width()) : 0.0f;
	const float ramp_v = m_local.gd->sel.fst ? GSSpriteRampBias(dt.y, extent.height()) : 0.0f;

	SetupPrim(vertex, index, dscan, false);

	while (1)
	{
		if (IsOneOfMyScanlines(r.top))
		{
			GSVertexSW row = scan;

			// Only the sprite's own first row is exempt, so a scissored-off top
			// leaves the term on every drawn row.
			if (ramp_v != 0.0f && r.top != extent.y)
				row.t -= GSVector4(0.0f, ramp_v, 0.0f, 0.0f);

			if (ramp_u == 0.0f)
			{
				DrawScanline(r.width(), r.left, r.top, row);
			}
			else if (r.left != extent.x)
			{
				// The sprite's own first column is outside the scissor, so every
				// pixel this row draws is past it.
				row.t -= GSVector4(ramp_u, 0.0f, 0.0f, 0.0f);
				DrawScanline(r.width(), r.left, r.top, row);
			}
			else if (m_local.gd->sel.notest)
			{
				// ⚠️ A notest scanline indexes its frame/depth address table by
				// `left >> 2` and requires an aligned left, so the span cannot be
				// split here. The term goes in the seed, leaving the first column a
				// sixteenth low (hardware has it exact). Accepted inaccuracy.
				row.t -= GSVector4(ramp_u, 0.0f, 0.0f, 0.0f);
				DrawScanline(r.width(), r.left, r.top, row);
			}
			else
			{
				// The first column exact, then the rest with the term in the seed.
				DrawScanline(1, r.left, r.top, row);

				if (r.width() > 1)
				{
					row.t += dscan.t;
					row.t -= GSVector4(ramp_u, 0.0f, 0.0f, 0.0f);
					DrawScanline(r.width() - 1, r.left + 1, r.top, row);
				}
			}
		}

		if (++r.top >= r.bottom)
			break;

		scan.t += dedge.t;
	}
}

void GSRasterizer::DrawEdge(const GSVertexSW& v0, const GSVertexSW& v1, const GSVertexSW& dv, int orientation, int side)
{
	// orientation:
	// - true: |dv.p.y| > |dv.p.x|
	// - false |dv.p.x| > |dv.p.y|
	// side:
	// - true: top/left edge
	// - false: bottom/right edge

	// TODO: bit slow and too much duplicated code
	// TODO: inner pre-step is still missing (hardly noticable)
	// TODO: it does not always line up with the edge of the surrounded triangle

	GSVertexSW* RESTRICT e = &m_edge.buff[m_edge.count];

	if (orientation)
	{
		GSVector4 tbf = v0.p.yyyy(v1.p).ceil(); // t t b b
		GSVector4 tbmax = tbf.max(m_fscissor_y); // max(t, st) max(t, sb) max(b, st) max(b, sb)
		GSVector4 tbmin = tbf.min(m_fscissor_y); // min(t, st) min(t, sb) min(b, st) min(b, sb)
		GSVector4i tb = GSVector4i(tbmax.xzyw(tbmin)); // max(t, st) max(b, sb) min(t, st) min(b, sb)

		int top, bottom;

		GSVertexSW edge, dedge;

		if (dv.p.y >= 0)
		{
			top    = tb.extract32<0>(); // max(t, st)
			bottom = tb.extract32<3>(); // min(b, sb)

			if (top >= bottom)
				return;

			edge = v0;
			dedge = dv / dv.p.yyyy();

			edge += dedge * (tbmax.xxxx() - edge.p.yyyy());
		}
		else
		{
			top    = tb.extract32<1>(); // max(b, st)
			bottom = tb.extract32<2>(); // min(t, sb)

			if (top >= bottom)
				return;

			edge = v1;
			dedge = dv / dv.p.yyyy();

			edge += dedge * (tbmax.zzzz() - edge.p.yyyy());
		}

		GSVector4i p = GSVector4i(edge.p.upl(dedge.p) * 0x10000);

		int x = p.extract32<0>();
		int dx = p.extract32<1>();

		if (side)
		{
			while (1)
			{
				int xi = x >> 16;
				int xf = x & 0xffff;

				if (m_scissor.left <= xi && xi < m_scissor.right && IsOneOfMyScanlines(top))
				{
					AddScanline(e, 1, xi, top, edge);

					e->p.U32[0] = (0x10000 - xf) & 0xffff;

					e++;
				}

				if (++top >= bottom)
					break;

				edge += dedge;
				x += dx;
			}
		}
		else
		{
			while (1)
			{
				int xi = (x >> 16) + 1;
				int xf = x & 0xffff;

				if (m_scissor.left <= xi && xi < m_scissor.right && IsOneOfMyScanlines(top))
				{
					AddScanline(e, 1, xi, top, edge);

					e->p.U32[0] = xf;

					e++;
				}

				if (++top >= bottom)
					break;

				edge += dedge;
				x += dx;
			}
		}
	}
	else
	{
		GSVector4 lrf = v0.p.xxxx(v1.p).ceil(); // l l r r
		GSVector4 lrmax = lrf.max(m_fscissor_x); // max(l, sl) max(l, sr) max(r, sl) max(r, sr)
		GSVector4 lrmin = lrf.min(m_fscissor_x); // min(l, sl) min(l, sr) min(r, sl) min(r, sr)
		GSVector4i lr = GSVector4i(lrmax.xzyw(lrmin)); // max(l, sl) max(r, sl) min(l, sr) min(r, sr)

		int left, right;

		GSVertexSW edge, dedge;

		if ((dv.p >= GSVector4::zero()).mask() & 1)
		{
			left  = lr.extract32<0>(); // max(l, sl)
			right = lr.extract32<3>(); // min(r, sr)

			if (left >= right)
				return;

			edge = v0;
			dedge = dv / dv.p.xxxx();

			edge += dedge * (lrmax.xxxx() - edge.p.xxxx());
		}
		else
		{
			left  = lr.extract32<1>(); // max(r, sl)
			right = lr.extract32<2>(); // min(l, sr)

			if (left >= right)
				return;

			edge = v1;
			dedge = dv / dv.p.xxxx();

			edge += dedge * (lrmax.zzzz() - edge.p.xxxx());
		}

		GSVector4i p = GSVector4i(edge.p.upl(dedge.p) * 0x10000);

		int y = p.extract32<2>();
		int dy = p.extract32<3>();

		if (side)
		{
			while (1)
			{
				int yi = y >> 16;
				int yf = y & 0xffff;

				if (m_scissor.top <= yi && yi < m_scissor.bottom && IsOneOfMyScanlines(yi))
				{
					AddScanline(e, 1, left, yi, edge);

					e->p.U32[0] = (0x10000 - yf) & 0xffff;

					e++;
				}

				if (++left >= right)
					break;

				edge += dedge;
				y += dy;
			}
		}
		else
		{
			while (1)
			{
				int yi = (y >> 16) + 1;
				int yf = y & 0xffff;

				if (m_scissor.top <= yi && yi < m_scissor.bottom && IsOneOfMyScanlines(yi))
				{
					AddScanline(e, 1, left, yi, edge);

					e->p.U32[0] = yf;

					e++;
				}

				if (++left >= right)
					break;

				edge += dedge;
				y += dy;
			}
		}
	}

	m_edge.count += e - &m_edge.buff[m_edge.count];
}

void GSRasterizer::AddScanline(GSVertexSW* e, int pixels, int left, int top, const GSVertexSW& scan)
{
	*e = scan;
	AddScanlineInfo(e, pixels, left, top);
}

void GSRasterizer::SetupPrim(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, bool cwalk_live)
{
	m_local.cwalk.live = cwalk_live ? 1 : 0;
	// A walk's tables belong to the previous primitive. A walkless primitive
	// wants zero tables, and tables already marked zero stay valid across it:
	// only SetupColourWalkTables writes them non-zero. (A setup that writes the
	// colour and fog steps derives them from dscan, whose colour and fog are zero
	// for every walkless primitive; sprites skip both.)
	if (cwalk_live)
		m_local.cwalk.tables.state = GSColourWalkTablesStale;

	m_setup_prim(vertex, index, dscan, m_local);
}

void GSRasterizer::Flush(const GSVertexSW* vertex, const u16* index, const GSVertexSW& dscan, bool cwalk_live, bool edge /* = false */)
{
	// TODO: on win64 this could be the place where xmm6-15 are preserved (not by each DrawScanline)

	int count = m_edge.count;

	if (count > 0)
	{
		SetupPrim(vertex, index, dscan, cwalk_live);

		const GSVertexSW* RESTRICT e = m_edge.buff;
		const GSVertexSW* RESTRICT ee = e + count;

		if (!edge)
		{
			do
			{
				int pixels = e->_pad.I32[0];
				int left = e->_pad.I32[1];
				int top = e->_pad.I32[2];

				DrawScanline(pixels, left, top, *e++);
			} while (e < ee);
		}
		else
		{
			do
			{
				int pixels = e->_pad.I32[0];
				int left = e->_pad.I32[1];
				int top = e->_pad.I32[2];

				DrawEdge(pixels, left, top, *e++);
			} while (e < ee);
		}

		m_edge.count = 0;
	}
}

#if _M_SSE >= 0x501
#define PIXELS_PER_LOOP 8
#else
#define PIXELS_PER_LOOP 4
#endif

void GSRasterizer::DrawScanline(int pixels, int left, int top, const GSVertexSW& scan)
{
	if ((m_scanmsk_value & 2) && (m_scanmsk_value & 1) == (top & 1)) return;
	m_pixels.actual += pixels;
	m_pixels.total += ((left + pixels + (PIXELS_PER_LOOP - 1)) & ~(PIXELS_PER_LOOP - 1)) - (left & ~(PIXELS_PER_LOOP - 1));
	//m_pixels.total += ((left + pixels + (PIXELS_PER_LOOP - 1)) & ~(PIXELS_PER_LOOP - 1)) - left;

	pxAssert(m_pixels.actual <= m_pixels.total);

	// The colour walk's tables are per row: their jumps are floored with the row's
	// fractional part included (GSDrawScanline.cpp). Zero tables for a walkless
	// primitive are already what every row wants.
	if (m_local.cwalk.live || m_local.cwalk.tables.state != GSColourWalkTablesZero)
		GSDrawScanline::SetupColourWalkTables(m_local, top);

	m_draw_scanline(pixels, left, top, scan, m_local);
}

void GSRasterizer::DrawEdge(int pixels, int left, int top, const GSVertexSW& scan)
{
	if ((m_scanmsk_value & 2) && (m_scanmsk_value & 1) == (top & 1)) return;
	m_pixels.actual += 1;
	m_pixels.total += PIXELS_PER_LOOP - 1;

	pxAssert(m_pixels.actual <= m_pixels.total);

	m_draw_edge(pixels, left, top, scan, m_local);
}

//

GSSingleRasterizer::GSSingleRasterizer()
	: m_r(&m_ds, 0, 1)
{
}

GSSingleRasterizer::~GSSingleRasterizer() = default;

void GSSingleRasterizer::Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data)
{
	Draw(*data.get());
}

void GSSingleRasterizer::Draw(GSRasterizerData& data)
{
	// Nothing else runs this code, so generating on the spot is fine here.
	if (!m_ds.SetupDraw(data, true)) [[unlikely]]
	{
		m_ds.ResetCodeCache();
		m_ds.SetupDraw(data, true);
	}

	m_r.Draw(data);
}

void GSSingleRasterizer::Sync()
{
}

bool GSSingleRasterizer::IsSynced() const
{
	return true;
}

int GSSingleRasterizer::GetPixels(bool reset /*= true*/)
{
	return m_r.GetPixels(reset);
}

void GSSingleRasterizer::PrintStats()
{
#ifdef ENABLE_DRAW_STATS
	m_ds.PrintStats();
#endif
}

//

GSRasterizerList::GSRasterizerList(int threads)
{
	m_thread_height = compute_best_thread_height(threads);

	const int rows = (2048 >> m_thread_height) + 16;
	m_scanline = static_cast<u8*>(_aligned_malloc(rows, 64));

	for (int i = 0; i < rows; i++)
	{
		m_scanline[i] = static_cast<u8>(i % threads);
	}

	m_serial = std::unique_ptr<GSRasterizer>(new GSRasterizer(&m_ds, 0, 1));

	PerformanceMetrics::SetGSSWThreadCount(threads);
}

GSRasterizerList::~GSRasterizerList()
{
	PerformanceMetrics::SetGSSWThreadCount(0);
	_aligned_free(m_scanline);
}

void GSRasterizerList::OnWorkerStartup(int i, u64 affinity)
{
	Threading::SetNameOfCurrentThread(StringUtil::StdStringFromFormat("GS-SW-%d", i).c_str());

	Threading::ThreadHandle handle(Threading::ThreadHandle::GetForCallingThread());
	if (affinity != 0)
	{
		INFO_LOG("Pinning GS thread {} to CPU {} (0x{:x})", i, std::countr_zero(affinity), affinity);
		handle.SetAffinity(affinity);
	}

	PerformanceMetrics::SetGSSWThread(i, std::move(handle));
}

void GSRasterizerList::OnWorkerShutdown(int i)
{
}

void GSRasterizerList::Queue(const GSRingHeap::SharedPtr<GSRasterizerData>& data)
{
	GSVector4i r = data->bbox.rintersect(data->scissor);

	// Probe first. Generating a routine makes its pages writable while the
	// workers may be executing from them, so drain them first. Running out of
	// code space needs the same sync before the reset.
	if (!m_ds.SetupDraw(*data.get(), false)) [[unlikely]]
	{
		Sync();

		if (!m_ds.SetupDraw(*data.get(), true))
		{
			m_ds.ResetCodeCache();
			m_ds.SetupDraw(*data.get(), true);
		}
	}

	pxAssert(r.top >= 0 && r.top <= 2048 && r.bottom >= 0 && r.bottom <= 2048);

	if (data->serial) [[unlikely]]
	{
		// This draw's scanlines alias each other's memory, so it cannot be split.
		// Drain the workers and run every row here, alone.
		Sync();

		m_serial->Draw(*data.get());

		return;
	}

	int top = r.top >> m_thread_height;
	int bottom = std::min<int>((r.bottom + (1 << m_thread_height) - 1) >> m_thread_height, top + (int)m_workers.size());

	while (top < bottom)
	{
		m_workers[m_scanline[top++]]->Push(data);
	}
}

void GSRasterizerList::Sync()
{
	if (!IsSynced())
	{
		for (size_t i = 0; i < m_workers.size(); i++)
		{
			m_workers[i]->Wait();
		}

		g_perfmon.Put(GSPerfMon::SyncPoint, 1);
	}
}

bool GSRasterizerList::IsSynced() const
{
	for (size_t i = 0; i < m_workers.size(); i++)
	{
		if (!m_workers[i]->IsEmpty())
		{
			return false;
		}
	}

	return true;
}

int GSRasterizerList::GetPixels(bool reset)
{
	int pixels = 0;

	for (size_t i = 0; i < m_workers.size(); i++)
	{
		pixels += m_r[i]->GetPixels(reset);
	}

	pixels += m_serial->GetPixels(reset);

	return pixels;
}

bool GSRasterizerList::RowsFoldAcrossWorkers(int page_height) const
{
	// Rows fold by one page height, bands are 1 << m_thread_height rows, and a
	// band's worker is its index modulo the worker count. The fold returns to the
	// same worker only when the page is a whole number of bands and the worker
	// count divides that number.
	//
	// A band taller than a page is the worst case, not a safe one: folded rows
	// straddle a band edge somewhere whatever the worker count. SWExtraThreadsHeight
	// can make bands 256 rows against 32-row pages.
	const int workers = static_cast<int>(m_workers.size());

	if (workers <= 1)
		return false;

	if (page_height < (1 << m_thread_height))
		return true;

	return ((page_height >> m_thread_height) % workers) != 0;
}

std::unique_ptr<IRasterizer> GSRasterizerList::Create(int threads)
{
	threads = std::max<int>(threads, 0);

	if (threads == 0)
	{
		return std::make_unique<GSSingleRasterizer>();
	}

	std::unique_ptr<GSRasterizerList> rl(new GSRasterizerList(threads));

	const std::vector<u32>& procs = VMManager::Internal::GetSoftwareRendererProcessorList();
	const bool pin = (EmuConfig.EnableThreadPinning && static_cast<size_t>(threads) <= procs.size());
	if (EmuConfig.EnableThreadPinning && !pin)
		WARNING_LOG("Not pinning SW threads, we need {} processors, but only have {}", threads, procs.size());

	for (int i = 0; i < threads; i++)
	{
		const u64 affinity = pin ? (static_cast<u64>(1u) << procs[i]) : 0;
		rl->m_r.push_back(std::unique_ptr<GSRasterizer>(new GSRasterizer(&rl->m_ds, i, threads)));
		auto& r = *rl->m_r[i];
		rl->m_workers.push_back(std::unique_ptr<GSWorker>(new GSWorker(
			[i, affinity]() { GSRasterizerList::OnWorkerStartup(i, affinity); },
			[&r](GSRingHeap::SharedPtr<GSRasterizerData>& item) { r.Draw(*item.get()); },
			[i]() { GSRasterizerList::OnWorkerShutdown(i); })));
	}

	return rl;
}

void GSRasterizerList::PrintStats()
{
}

#define INIT4(x0, x1, x2, x3, x4) static_cast<DrawEdgeTrianglePtr>(&GSRasterizer::DrawEdgeTriangle<x0, x1, x2, x3, x4>)
#define INIT3(x0, x1, x2, x3) { INIT4(x0, x1, x2, x3, false)    , INIT4(x0, x1, x2, x3, true) } 
#define INIT2(x0, x1, x2)     { INIT3(x0, x1, x2, false)        , INIT3(x0, x1, x2, true)     } 
#define INIT1(x0, x1)         { INIT2(x0, x1, false)            , INIT2(x0, x1, true)         }
#define INIT0(x0)             { INIT1(x0, false)                , INIT1(x0, true)             }

const GSRasterizer::DrawEdgeTrianglePtr GSRasterizer::m_draw_edge_triangle[2][2][2][2][2] = {
	INIT0(false),
	INIT0(true)
};

#undef INIT0
#undef INIT1
#undef INIT2
#undef INIT3
#undef INIT4

#define INIT3(x0, x1, x2, x3) static_cast<DrawEdgeLinePtr>(&GSRasterizer::DrawEdgeLine<x0, x1, x2, x3>)
#define INIT2(x0, x1, x2)     { INIT3(x0, x1, x2, false)        , INIT3(x0, x1, x2, true)     } 
#define INIT1(x0, x1)         { INIT2(x0, x1, false)            , INIT2(x0, x1, true)         }
#define INIT0(x0)             { INIT1(x0, false)                , INIT1(x0, true)             }

const GSRasterizer::DrawEdgeLinePtr GSRasterizer::m_draw_edge_line[2][2][2][2] = {
	INIT0(false),
	INIT0(true)
};

#undef INIT0
#undef INIT1
#undef INIT2
#undef INIT3