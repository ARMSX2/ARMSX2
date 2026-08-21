/*
 * Copyright (C) 1999-2010  Terence M. Welsh
 *
 * This file is part of rsMath.
 *
 * rsMath is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * rsMath is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "rsMath.h"

#include <math.h>

rsMatrix::rsMatrix()
{
}

rsMatrix::~rsMatrix()
{
}

void
rsMatrix::identity()
{
	m[0] = 1.0f;
	m[1] = 0.0f;
	m[2] = 0.0f;
	m[3] = 0.0f;
	m[4] = 0.0f;
	m[5] = 1.0f;
	m[6] = 0.0f;
	m[7] = 0.0f;
	m[8] = 0.0f;
	m[9] = 0.0f;
	m[10] = 1.0f;
	m[11] = 0.0f;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;
}

void
rsMatrix::set(float* mat)
{
	int i;
	for (i = 0; i < 16; i++)
		m[i] = mat[i];
}

void
rsMatrix::get(float* mat)
{
	int i;
	for (i = 0; i < 16; i++)
		mat[i] = m[i];
}

float*
rsMatrix::get()
{
	return m;
}

void
rsMatrix::copy(const rsMatrix &mat)
{
	int i;
	for (i = 0; i < 16; i++)
		m[i] = mat[i];
}

void
rsMatrix::preMult(const rsMatrix &postMat)
{
	const float preMat0(m[0]);
	const float preMat1(m[1]);
	const float preMat2(m[2]);
	const float preMat3(m[3]);
	const float preMat4(m[4]);
	const float preMat5(m[5]);
	const float preMat6(m[6]);
	const float preMat7(m[7]);
	const float preMat8(m[8]);
	const float preMat9(m[9]);
	const float preMat10(m[10]);
	const float preMat11(m[11]);
	const float preMat12(m[12]);
	const float preMat13(m[13]);
	const float preMat14(m[14]);
	const float preMat15(m[15]);

	m[0] = preMat0 * postMat[0] + preMat4 * postMat[1] + preMat8  * postMat[2] + preMat12 * postMat[3];
	m[1] = preMat1 * postMat[0] + preMat5 * postMat[1] + preMat9  * postMat[2] + preMat13 * postMat[3];
	m[2] = preMat2 * postMat[0] + preMat6 * postMat[1] + preMat10 * postMat[2] + preMat14 * postMat[3];
	m[3] = preMat3 * postMat[0] + preMat7 * postMat[1] + preMat11 * postMat[2] + preMat15 * postMat[3];
	m[4] = preMat0 * postMat[4] + preMat4 * postMat[5] + preMat8  * postMat[6] + preMat12 * postMat[7];
	m[5] = preMat1 * postMat[4] + preMat5 * postMat[5] + preMat9  * postMat[6] + preMat13 * postMat[7];
	m[6] = preMat2 * postMat[4] + preMat6 * postMat[5] + preMat10 * postMat[6] + preMat14 * postMat[7];
	m[7] = preMat3 * postMat[4] + preMat7 * postMat[5] + preMat11 * postMat[6] + preMat15 * postMat[7];
	m[8] = preMat0 * postMat[8] + preMat4 * postMat[9] + preMat8  * postMat[10] + preMat12 * postMat[11];
	m[9] = preMat1 * postMat[8] + preMat5 * postMat[9] + preMat9  * postMat[10] + preMat13 * postMat[11];
	m[10] = preMat2 * postMat[8] + preMat6 * postMat[9] + preMat10 * postMat[10] + preMat14 * postMat[11];
	m[11] = preMat3 * postMat[8] + preMat7 * postMat[9] + preMat11 * postMat[10] + preMat15 * postMat[11];
	m[12] = preMat0 * postMat[12] + preMat4 * postMat[13] + preMat8  * postMat[14] + preMat12 * postMat[15];
	m[13] = preMat1 * postMat[12] + preMat5 * postMat[13] + preMat9  * postMat[14] + preMat13 * postMat[15];
	m[14] = preMat2 * postMat[12] + preMat6 * postMat[13] + preMat10 * postMat[14] + preMat14 * postMat[15];
	m[15] = preMat3 * postMat[12] + preMat7 * postMat[13] + preMat11 * postMat[14] + preMat15 * postMat[15];
}

void
rsMatrix::postMult(const rsMatrix &preMat)
{
	const float postMat0(m[0]);
	const float postMat1(m[1]);
	const float postMat2(m[2]);
	const float postMat3(m[3]);
	const float postMat4(m[4]);
	const float postMat5(m[5]);
	const float postMat6(m[6]);
	const float postMat7(m[7]);
	const float postMat8(m[8]);
	const float postMat9(m[9]);
	const float postMat10(m[10]);
	const float postMat11(m[11]);
	const float postMat12(m[12]);
	const float postMat13(m[13]);
	const float postMat14(m[14]);
	const float postMat15(m[15]);

	m[0] = preMat[0] * postMat0 + preMat[4] * postMat1 + preMat[8] * postMat2 + preMat[12] * postMat3;
	m[1] = preMat[1] * postMat0 + preMat[5] * postMat1 + preMat[9] * postMat2 + preMat[13] * postMat3;
	m[2] = preMat[2] * postMat0 + preMat[6] * postMat1 + preMat[10] * postMat2 + preMat[14] * postMat3;
	m[3] = preMat[3] * postMat0 + preMat[7] * postMat1 + preMat[11] * postMat2 + preMat[15] * postMat3;
	m[4] = preMat[0] * postMat4 + preMat[4] * postMat5 + preMat[8] * postMat6 + preMat[12] * postMat7;
	m[5] = preMat[1] * postMat4 + preMat[5] * postMat5 + preMat[9] * postMat6 + preMat[13] * postMat7;
	m[6] = preMat[2] * postMat4 + preMat[6] * postMat5 + preMat[10] * postMat6 + preMat[14] * postMat7;
	m[7] = preMat[3] * postMat4 + preMat[7] * postMat5 + preMat[11] * postMat6 + preMat[15] * postMat7;
	m[8] = preMat[0] * postMat8 + preMat[4] * postMat9 + preMat[8] * postMat10 + preMat[12] * postMat11;
	m[9] = preMat[1] * postMat8 + preMat[5] * postMat9 + preMat[9] * postMat10 + preMat[13] * postMat11;
	m[10] = preMat[2] * postMat8 + preMat[6] * postMat9 + preMat[10] * postMat10 + preMat[14] * postMat11;
	m[11] = preMat[3] * postMat8 + preMat[7] * postMat9 + preMat[11] * postMat10 + preMat[15] * postMat11;
	m[12] = preMat[0] * postMat12 + preMat[4] * postMat13 + preMat[8] * postMat14 + preMat[12] * postMat15;
	m[13] = preMat[1] * postMat12 + preMat[5] * postMat13 + preMat[9] * postMat14 + preMat[13] * postMat15;
	m[14] = preMat[2] * postMat12 + preMat[6] * postMat13 + preMat[10] * postMat14 + preMat[14] * postMat15;
	m[15] = preMat[3] * postMat12 + preMat[7] * postMat13 + preMat[11] * postMat14 + preMat[15] * postMat15;
}

void
rsMatrix::makeTranslate(float x, float y, float z)
{
	m[0] = 1.0f;
	m[1] = 0.0f;
	m[2] = 0.0f;
	m[3] = 0.0f;
	m[4] = 0.0f;
	m[5] = 1.0f;
	m[6] = 0.0f;
	m[7] = 0.0f;
	m[8] = 0.0f;
	m[9] = 0.0f;
	m[10] = 1.0f;
	m[11] = 0.0f;
	m[12] = x;
	m[13] = y;
	m[14] = z;
	m[15] = 1.0f;
}

void
rsMatrix::makeTranslate(float *p)
{
	m[0] = 1.0f;
	m[1] = 0.0f;
	m[2] = 0.0f;
	m[3] = 0.0f;
	m[4] = 0.0f;
	m[5] = 1.0f;
	m[6] = 0.0f;
	m[7] = 0.0f;
	m[8] = 0.0f;
	m[9] = 0.0f;
	m[10] = 1.0f;
	m[11] = 0.0f;
	m[12] = p[0];
	m[13] = p[1];
	m[14] = p[2];
	m[15] = 1.0f;
}

void
rsMatrix::makeTranslate(const rsVec &vec)
{
	m[0] = 1.0f;
	m[1] = 0.0f;
	m[2] = 0.0f;
	m[3] = 0.0f;
	m[4] = 0.0f;
	m[5] = 1.0f;
	m[6] = 0.0f;
	m[7] = 0.0f;
	m[8] = 0.0f;
	m[9] = 0.0f;
	m[10] = 1.0f;
	m[11] = 0.0f;
	m[12] = vec[0];
	m[13] = vec[1];
	m[14] = vec[2];
	m[15] = 1.0f;
}

void
rsMatrix::makeScale(float s)
{
	m[0] = s;
	m[1] = 0.0f;
	m[2] = 0.0f;
	m[3] = 0.0f;
	m[4] = 0.0f;
	m[5] = s;
	m[6] = 0.0f;
	m[7] = 0.0f;
	m[8] = 0.0f;
	m[9] = 0.0f;
	m[10] = s;
	m[11] = 0.0f;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;
}

void
rsMatrix::makeScale(float x, float y, float z)
{
	m[0] = x;
	m[1] = 0.0f;
	m[2] = 0.0f;
	m[3] = 0.0f;
	m[4] = 0.0f;
	m[5] = y;
	m[6] = 0.0f;
	m[7] = 0.0f;
	m[8] = 0.0f;
	m[9] = 0.0f;
	m[10] = z;
	m[11] = 0.0f;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;
}

void
rsMatrix::makeScale(float* s)
{
	m[0] = s[0];
	m[1] = 0.0f;
	m[2] = 0.0f;
	m[3] = 0.0f;
	m[4] = 0.0f;
	m[5] = s[1];
	m[6] = 0.0f;
	m[7] = 0.0f;
	m[8] = 0.0f;
	m[9] = 0.0f;
	m[10] = s[2];
	m[11] = 0.0f;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;
}

void
rsMatrix::makeScale(const rsVec &vec)
{
	m[0] = vec[0];
	m[1] = 0.0f;
	m[2] = 0.0f;
	m[3] = 0.0f;
	m[4] = 0.0f;
	m[5] = vec[1];
	m[6] = 0.0f;
	m[7] = 0.0f;
	m[8] = 0.0f;
	m[9] = 0.0f;
	m[10] = vec[2];
	m[11] = 0.0f;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;
}

void
rsMatrix::makeRotate(float a, float x, float y, float z)
{
	rsQuat q;
	q.make(a, x, y, z);
	q.toMat(m);
}

void
rsMatrix::makeRotate(float a, const rsVec &v)
{
	rsQuat q;
	q.make(a, v);
	q.toMat(m);
}

void
rsMatrix::makeRotate(rsQuat &q)
{
	q.toMat(m);
}

void
rsMatrix::translate(float x, float y, float z)
{
	rsMatrix mat;
	mat.makeTranslate(x, y, z);
	this->postMult(mat);
}

void
rsMatrix::translate(float *p)
{
	rsMatrix mat;
	mat.makeTranslate(p);
	this->postMult(mat);
}

void
rsMatrix::translate(const rsVec &vec)
{
	rsMatrix mat;
	mat.makeTranslate(vec);
	this->postMult(mat);
}

void
rsMatrix::scale(float s)
{
	rsMatrix mat;
	mat.makeScale(s);
	this->postMult(mat);
}

void
rsMatrix::scale(float x, float y, float z)
{
	rsMatrix mat;
	mat.makeScale(x, y, z);
	this->postMult(mat);
}

void
rsMatrix::scale(float* s)
{
	rsMatrix mat;
	mat.makeScale(s);
	this->postMult(mat);
}

void
rsMatrix::scale(const rsVec &vec)
{
	rsMatrix mat;
	mat.makeScale(vec);
	this->postMult(mat);
}

void
rsMatrix::rotate(float a, float x, float y, float z)
{
	rsMatrix mat;
	mat.makeRotate(a, x, y, z);
	this->postMult(mat);
}

void
rsMatrix::rotate(float a, const rsVec &v)
{
	rsMatrix mat;
	mat.makeRotate(a, v);
	this->postMult(mat);
}

void
rsMatrix::rotate(rsQuat &q)
{
	rsMatrix mat;
	mat.makeRotate(q);
	this->postMult(mat);
}

float
rsMatrix::determinant3(const float aa, const float ab, const float ac,
	const float ba, const float bb, const float bc,
	const float ca, const float cb, const float cc)
{
	return (aa * bb * cc) + (ab * bc * ca) + (ac * ba * cb)
		- (aa * bc * cb) - (ab * ba * cc) - (ac * bb * ca);
}

bool
rsMatrix::invert()
{
	const float aa(m[0]);
	const float ba(m[1]);
	const float ca(m[2]);
	const float da(m[3]);
	const float ab(m[4]);
	const float bb(m[5]);
	const float cb(m[6]);
	const float db(m[7]);
	const float ac(m[8]);
	const float bc(m[9]);
	const float cc(m[10]);
	const float dc(m[11]);
	const float ad(m[12]);
	const float bd(m[13]);
	const float cd(m[14]);
	const float dd(m[15]);

	// calculate determinant
	const float det3_1(determinant3(bb, bc, bd, cb, cc, cd, db, dc, dd));
	const float det3_2(-determinant3(ab, ac, ad, cb, cc, cd, db, dc, dd));
	const float det3_3(determinant3(ab, ac, ad, bb, bc, bd, db, dc, dd));
	const float det3_4(-determinant3(ab, ac, ad, bb, bc, bd, cb, cc, cd));
	const float det(aa * det3_1 + ba * det3_2 + ca * det3_3 + da * det3_4);

	if (fabs(det) < RS_EPSILON)
		return false;  // matrix is singular, cannot be inverted

	// reciprocal of determinant
	const float rec_det(1.0f / det);

	// calculate inverted matrix
	m[0] = det3_1 * rec_det;
	m[4] = det3_2 * rec_det;
	m[8] = det3_3 * rec_det;
	m[12] = det3_4 * rec_det;
	m[1] = -determinant3(ba, bc, bd, ca, cc, cd, da, dc, dd) * rec_det;
	m[5] = determinant3(aa, ac, ad, ca, cc, cd, da, dc, dd) * rec_det;
	m[9] = -determinant3(aa, ac, ad, ba, bc, bd, da, dc, dd) * rec_det;
	m[13] = determinant3(aa, ac, ad, ba, bc, bd, ca, cc, cd) * rec_det;
	m[2] = determinant3(ba, bb, bd, ca, cb, cd, da, db, dd) * rec_det;
	m[6] = -determinant3(aa, ab, ad, ca, cb, cd, da, db, dd) * rec_det;
	m[10] = determinant3(aa, ab, ad, ba, bb, bd, da, db, dd) * rec_det;
	m[14] = -determinant3(aa, ab, ad, ba, bb, bd, ca, cb, cd) * rec_det;
	m[3] = -determinant3(ba, bb, bc, ca, cb, cc, da, db, dc) * rec_det;
	m[7] = determinant3(aa, ab, ac, ca, cb, cc, da, db, dc) * rec_det;
	m[11] = -determinant3(aa, ab, ac, ba, bb, bc, da, db, dc) * rec_det;
	m[15] = determinant3(aa, ab, ac, ba, bb, bc, ca, cb, cc) * rec_det;

	return true;
}

bool
rsMatrix::invert(const rsMatrix &mat)
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

	if (fabs(det) < RS_EPSILON)
		return false;  // matrix is singular, cannot be inverted

	// reciprocal of determinant
	const float rec_det(1.0f / det);

	// calculate inverted matrix
	m[0] = det3_1 * rec_det;
	m[4] = det3_2 * rec_det;
	m[8] = det3_3 * rec_det;
	m[12] = det3_4 * rec_det;
	m[1] = -determinant3(ba, bc, bd, ca, cc, cd, da, dc, dd) * rec_det;
	m[5] = determinant3(aa, ac, ad, ca, cc, cd, da, dc, dd) * rec_det;
	m[9] = -determinant3(aa, ac, ad, ba, bc, bd, da, dc, dd) * rec_det;
	m[13] = determinant3(aa, ac, ad, ba, bc, bd, ca, cc, cd) * rec_det;
	m[2] = determinant3(ba, bb, bd, ca, cb, cd, da, db, dd) * rec_det;
	m[6] = -determinant3(aa, ab, ad, ca, cb, cd, da, db, dd) * rec_det;
	m[10] = determinant3(aa, ab, ad, ba, bb, bd, da, db, dd) * rec_det;
	m[14] = -determinant3(aa, ab, ad, ba, bb, bd, ca, cb, cd) * rec_det;
	m[3] = -determinant3(ba, bb, bc, ca, cb, cc, da, db, dc) * rec_det;
	m[7] = determinant3(aa, ab, ac, ca, cb, cc, da, db, dc) * rec_det;
	m[11] = -determinant3(aa, ab, ac, ba, bb, bc, da, db, dc) * rec_det;
	m[15] = determinant3(aa, ab, ac, ba, bb, bc, ca, cb, cc) * rec_det;

	return true;
}

void
rsMatrix::rotationInvert(const rsMatrix &mat)
{
	float det = mat[0] * mat[5] * mat[10]
		+ mat[4] * mat[9] * mat[2]
		+ mat[8] * mat[1] * mat[6]
		- mat[2] * mat[5] * mat[8]
		- mat[6] * mat[9] * mat[0]
		- mat[10] * mat[1] * mat[4];

	m[0] = (mat[5] * mat[10] - mat[6] * mat[9]) / det;
	m[1] = (mat[6] * mat[8] - mat[4] * mat[10]) / det;
	m[2] = (mat[4] * mat[9] - mat[5] * mat[8]) / det;
	m[4] = (mat[9] * mat[2] - mat[10] * mat[1]) / det;
	m[5] = (mat[10] * mat[0] - mat[8] * mat[2]) / det;
	m[6] = (mat[8] * mat[1] - mat[9] * mat[0]) / det;
	m[8] = (mat[1] * mat[6] - mat[2] * mat[5]) / det;
	m[9] = (mat[2] * mat[4] - mat[0] * mat[6]) / det;
	m[10] = (mat[0] * mat[5] - mat[1] * mat[4]) / det;
	m[3] = m[7] = m[11] = m[12] = m[13] = m[14] = 0.0f;
	m[15] = 1.0f;
}

void
rsMatrix::fromQuat(const rsQuat &q)
{
	float s, xs, ys, zs, wx, wy, wz, xx, xy, xz, yy, yz, zz;

	// must have an axis
	if (q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f)
	{
		identity();
		return;
	}

	s = 2.0f / (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	xs = q[0] * s;
	ys = q[1] * s;
	zs = q[2] * s;
	wx = q[3] * xs;
	wy = q[3] * ys;
	wz = q[3] * zs;
	xx = q[0] * xs;
	xy = q[0] * ys;
	xz = q[0] * zs;
	yy = q[1] * ys;
	yz = q[1] * zs;
	zz = q[2] * zs;

	m[0] = 1.0f - yy - zz;
	m[1] = xy + wz;
	m[2] = xz - wy;
	m[3] = 0.0f;
	m[4] = xy - wz;
	m[5] = 1.0f - xx - zz;
	m[6] = yz + wx;
	m[7] = 0.0f;
	m[8] = xz + wy;
	m[9] = yz - wx;
	m[10] = 1.0f - xx - yy;
	m[11] = 0.0f;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;
}

rsMatrix&
rsMatrix::operator = (const rsMatrix &mat)
{
	m[0] = mat[0]; m[1] = mat[1]; m[2] = mat[2]; m[3] = mat[3];
	m[4] = mat[4]; m[5] = mat[5]; m[6] = mat[6]; m[7] = mat[7];
	m[8] = mat[8]; m[9] = mat[9]; m[10] = mat[10]; m[11] = mat[11];
	m[12] = mat[12]; m[13] = mat[13]; m[14] = mat[14]; m[15] = mat[15];
	return *this;
}

std::ostream&
rsMatrix::operator << (std::ostream &os)
{
	return os
		<< "| " << m[0] << " " << m[4] << " " << m[8] << " " << m[12] << " |" << std::endl
		<< "| " << m[1] << " " << m[5] << " " << m[9] << " " << m[13] << " |" << std::endl
		<< "| " << m[2] << " " << m[6] << " " << m[10] << " " << m[14] << " |" << std::endl
		<< "| " << m[3] << " " << m[7] << " " << m[11] << " " << m[15] << " |" << std::endl;
}

/*std::ostream & operator << (std::ostream& os, const rsMatrix& mat){
	return os
		<< "| " << mat[0] << " " << mat[4] << " " << mat[8] << " " << mat[12] << " |" << std::endl
		<< "| " << mat[1] << " " << mat[5] << " " << mat[9] << " " << mat[13] << " |" << std::endl
		<< "| " << mat[2] << " " << mat[6] << " " << mat[10] << " " << mat[14] << " |" << std::endl
		<< "| " << mat[3] << " " << mat[7] << " " << mat[11] << " " << mat[15] << " |" << std::endl;
}*/
