/*
 * Copyright (C) 2001-2010  Terence M. Welsh
 *
 * This file is part of Implicit.
 *
 * Implicit is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Implicit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "impTorus.h"

float
impTorus::value(float* position)
{
	/*#ifdef __SSE__
		__m128 pos = _mm_loadu_ps(position);

		__m128 tmp = _mm_mul_ps(pos, invtrmatrow[0]);
		__m128 tx = _mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(0,0,0,0)), _mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(1,1,1,1)),
			_mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(2,2,2,2)), _mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(3,3,3,3)))));
		tmp = _mm_mul_ps(pos, invtrmatrow[1]);
		__m128 ty = _mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(0,0,0,0)), _mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(1,1,1,1)),
			_mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(2,2,2,2)), _mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(3,3,3,3)))));
		tmp = _mm_mul_ps(pos, invtrmatrow[2]);
		__m128 tz = _mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(0,0,0,0)), _mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(1,1,1,1)),
			_mm_add_ps(_mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(2,2,2,2)), _mm_shuffle_ps(tmp, tmp, _MM_SHUFFLE(3,3,3,3)))));

		__m128 xxyy = _mm_shuffle_ps(ty, tx, _MM_SHUFFLE(0,0,0,0));
		__m128 xxyy_sq = _mm_mul_ps(xxyy, xxyy);
		__m128 add = _mm_add_ps(_mm_shuffle_ps(xxyy_sq, xxyy_sq, _MM_SHUFFLE(2,2,2,2)), _mm_shuffle_ps(xxyy_sq, xxyy_sq, _MM_SHUFFLE(0,0,0,0)));
		__m128 root = _mm_sqrt_ps(add);
		__m128 sub = _mm_sub_ps(root, _mm_set1_ps(radius));

		__m128 sszz = _mm_shuffle_ps(tz, sub, _MM_SHUFFLE(0,0,0,0));
		__m128 sszz_sq = _mm_mul_ps(sszz, sszz);
		add = _mm_add_ps(_mm_shuffle_ps(sszz_sq, sszz_sq, _MM_SHUFFLE(2,2,2,2)), _mm_shuffle_ps(sszz_sq, sszz_sq, _MM_SHUFFLE(0,0,0,0)));
		return _mm_cvtss_f32(_mm_div_ps(_mm_set1_ps(thicknessSquared), add));
	#else*/

	const float& x(position[0]);
	const float& y(position[1]);
	const float& z(position[2]);

	const float tx(x * invtrmat[0] + y * invtrmat[1] + z * invtrmat[2] + invtrmat[3]);
	const float ty(x * invtrmat[4] + y * invtrmat[5] + z * invtrmat[6] + invtrmat[7]);
	const float tz(x * invtrmat[8] + y * invtrmat[9] + z * invtrmat[10] + invtrmat[11]);

	const float temp(sqrtf(tx * tx + ty * ty) - radius);
	return thicknessSquared / (temp * temp + tz * tz + IMP_MIN_DIVISOR);

	//#endif
}

// Finding a point inside a torus is trickier than
// finding a point inside a sphere or ellipsoid.
void
impTorus::center(float* position)
{
	position[0] = mat[0] * radius + mat[12];
	position[1] = mat[1] * radius + mat[13];
	position[2] = mat[2] * radius + mat[14];
}

void
impTorus::addCrawlPoint(impCrawlPointVector& cpv)
{
	cpv.push_back(impCrawlPoint(mat[0] * radius + mat[12],
	        mat[1] * radius + mat[13],
	        mat[2] * radius + mat[14]));
}
