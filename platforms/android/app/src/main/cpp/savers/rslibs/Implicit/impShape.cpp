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

#include "impShape.h"

#include <string.h>

impShape::impShape()
{
	mat[0] = mat[5] = mat[10] = mat[15]
	            = invmat[0] = invmat[5] = invmat[10] = invmat[15] = 1.0f;
	mat[1] = mat[2] = mat[3] = mat[4]
	            = mat[6] = mat[7] = mat[8] = mat[9]
	                            = mat[11] = mat[12] = mat[13] = mat[14]
	                                    = invmat[1] = invmat[2] = invmat[3] = invmat[4]
	                                            = invmat[6] = invmat[7] = invmat[8] = invmat[9]
	                                                    = invmat[11] = invmat[12] = invmat[13] = invmat[14] = 0.0f;

	thickness = 0.1f;
	thicknessSquared = thickness * thickness;
}

void
impShape::setPosition(float x, float y, float z)
{
	mat[12] = x;
	mat[13] = y;
	mat[14] = z;
	invmat[12] = -x;
	invmat[13] = -y;
	invmat[14] = -z;
	invtrmat[3] = -x;
	invtrmat[7] = -y;
	invtrmat[11] = -z;
#ifdef __SSE__
	((float*)(&(invtrmatrow[0])))[3] = -x;
	((float*)(&(invtrmatrow[1])))[3] = -y;
	((float*)(&(invtrmatrow[2])))[3] = -z;
#endif
}

// Don't need to set this for simple spheres.
// A whole matrix is only necessary for weird asymmetric objects.
void
impShape::setMatrix(float* m)
{
	memcpy(mat, m, 16 * sizeof(float));

	invertMatrix();

	invtrmat[0] = invmat[0];
	invtrmat[1] = invmat[4];
	invtrmat[2] = invmat[8];
	invtrmat[3] = invmat[12];
	invtrmat[4] = invmat[1];
	invtrmat[5] = invmat[5];
	invtrmat[6] = invmat[9];
	invtrmat[7] = invmat[13];
	invtrmat[8] = invmat[2];
	invtrmat[9] = invmat[6];
	invtrmat[10] = invmat[10];
	invtrmat[11] = invmat[14];
	invtrmat[12] = invmat[3];
	invtrmat[13] = invmat[7];
	invtrmat[14] = invmat[11];
	invtrmat[15] = invmat[15];
#ifdef __SSE__
	invtrmatrow[0] = _mm_loadu_ps(invtrmat);
	invtrmatrow[1] = _mm_loadu_ps(&(invtrmat[4]));
	invtrmatrow[2] = _mm_loadu_ps(&(invtrmat[8]));
	invtrmatrow[3] = _mm_loadu_ps(&(invtrmat[12]));
#endif
}

float
impShape::determinant3(const float aa, const float ab, const float ac,
    const float ba, const float bb, const float bc,
    const float ca, const float cb, const float cc)
{
	return (aa * bb * cc) + (ab * bc * ca) + (ac * ba * cb)
	    - (aa * bc * cb) - (ab * ba * cc) - (ac * bb * ca);
}

bool
impShape::invertMatrix()
{
	const float aa(mat[0]);
	const float ba(mat[1]);
	const float ca(mat[2]);
	const float da(mat[3]);
	const float ab(mat[4]);
	const float bb(mat[5]);
	const float cb(mat[6]);
	const float db(mat[7]);
	const float ac(mat[8]);
	const float bc(mat[9]);
	const float cc(mat[10]);
	const float dc(mat[11]);
	const float ad(mat[12]);
	const float bd(mat[13]);
	const float cd(mat[14]);
	const float dd(mat[15]);

	// calculate determinant
	const float det3_1(determinant3(bb, bc, bd, cb, cc, cd, db, dc, dd));
	const float det3_2(-determinant3(ab, ac, ad, cb, cc, cd, db, dc, dd));
	const float det3_3(determinant3(ab, ac, ad, bb, bc, bd, db, dc, dd));
	const float det3_4(-determinant3(ab, ac, ad, bb, bc, bd, cb, cc, cd));
	const float det(aa * det3_1 + ba * det3_2 + ca * det3_3 + da * det3_4);

	if (fabs(det) < 0.000001f)
		return false;  // matrix is singular, cannot be inverted

	// reciprocal of determinant
	const float rec_det(1.0f / det);

	// calculate inverted matrix
	invmat[0] = det3_1 * rec_det;
	invmat[4] = det3_2 * rec_det;
	invmat[8] = det3_3 * rec_det;
	invmat[12] = det3_4 * rec_det;
	invmat[1] = -determinant3(ba, bc, bd, ca, cc, cd, da, dc, dd) * rec_det;
	invmat[5] = determinant3(aa, ac, ad, ca, cc, cd, da, dc, dd) * rec_det;
	invmat[9] = -determinant3(aa, ac, ad, ba, bc, bd, da, dc, dd) * rec_det;
	invmat[13] = determinant3(aa, ac, ad, ba, bc, bd, ca, cc, cd) * rec_det;
	invmat[2] = determinant3(ba, bb, bd, ca, cb, cd, da, db, dd) * rec_det;
	invmat[6] = -determinant3(aa, ab, ad, ca, cb, cd, da, db, dd) * rec_det;
	invmat[10] = determinant3(aa, ab, ad, ba, bb, bd, da, db, dd) * rec_det;
	invmat[14] = -determinant3(aa, ab, ad, ba, bb, bd, ca, cb, cd) * rec_det;
	invmat[3] = -determinant3(ba, bb, bc, ca, cb, cc, da, db, dc) * rec_det;
	invmat[7] = determinant3(aa, ab, ac, ca, cb, cc, da, db, dc) * rec_det;
	invmat[11] = -determinant3(aa, ab, ac, ba, bb, bc, da, db, dc) * rec_det;
	invmat[15] = determinant3(aa, ab, ac, ba, bb, bc, ca, cb, cc) * rec_det;

	return true;
}

float
impShape::value(float* position)
{
	return 0.0f;
}

void
impShape::center(float* position)
{
	position[0] = mat[12];
	position[1] = mat[13];
	position[2] = mat[14];
}

void
impShape::addCrawlPoint(impCrawlPointVector& cpv)
{
	cpv.push_back(impCrawlPoint(&(mat[12])));
}
