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

rsVec::rsVec()
{
}

rsVec::rsVec(float xx, float yy, float zz)
{
	v[0] = xx;
	v[1] = yy;
	v[2] = zz;
}

rsVec::~rsVec()
{
}

void
rsVec::set(float xx, float yy, float zz)
{
	v[0] = xx;
	v[1] = yy;
	v[2] = zz;
}

float
rsVec::length()
{
	return(float(sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])));
}

float
rsVec::normalize()
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (length == 0.0f)
	{
		v[1] = 1.0f;
		return(0.0f);
	}
	const float normalizer(1.0f / length);
	v[0] *= normalizer;
	v[1] *= normalizer;
	v[2] *= normalizer;
	return(length);
}

float
rsVec::dot(rsVec vec1)
{
	return(v[0] * vec1[0] + v[1] * vec1[1] + v[2] * vec1[2]);
}

void
rsVec::cross(rsVec vec1, rsVec vec2)
{
	v[0] = vec1[1] * vec2[2] - vec2[1] * vec1[2];
	v[1] = vec1[2] * vec2[0] - vec2[2] * vec1[0];
	v[2] = vec1[0] * vec2[1] - vec2[0] * vec1[1];
}

void
rsVec::scale(float scale)
{
	v[0] *= scale;
	v[1] *= scale;
	v[2] *= scale;
}

void
rsVec::transPoint(const rsMatrix &m)
{
	float x = v[0];
	float y = v[1];
	float z = v[2];
	v[0] = x * m[0] + y * m[4] + z * m[8] + m[12];
	v[1] = x * m[1] + y * m[5] + z * m[9] + m[13];
	v[2] = x * m[2] + y * m[6] + z * m[10] + m[14];
}

void
rsVec::transVec(const rsMatrix &m)
{
	float x = v[0];
	float y = v[1];
	float z = v[2];
	v[0] = x * m[0] + y * m[4] + z * m[8];
	v[1] = x * m[1] + y * m[5] + z * m[9];
	v[2] = x * m[2] + y * m[6] + z * m[10];
}

int
rsVec::almostEqual(rsVec vec, float tolerance)
{
	if (sqrtf((v[0] - vec[0])*(v[0] - vec[0])
		+ (v[1] - vec[1])*(v[1] - vec[1])
		+ (v[2] - vec[2])*(v[2] - vec[2]))
		<= tolerance)
		return 1;
	else
		return 0;
}

// Generic vector math functions
float
rsLength(const float *xyz)
{
	return(float(sqrt(xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2])));
}

float
rsNormalize(float *xyz)
{
	float length = float(sqrt(xyz[0] * xyz[0] + xyz[1] * xyz[1] + xyz[2] * xyz[2]));
	if (length == 0.0f)
		return(0.0f);

	float reciprocal = 1.0f / length;
	xyz[0] *= reciprocal;
	xyz[1] *= reciprocal;
	xyz[2] *= reciprocal;

	return(length);
}

float
rsDot(const float *xyz1, const float *xyz2)
{
	return(xyz1[0] * xyz2[0] + xyz1[1] * xyz2[1] + xyz1[2] * xyz2[2]);
}

void
rsCross(const float *xyz1, const float *xyz2, float *xyzOut)
{
	xyzOut[0] = xyz1[1] * xyz2[2] - xyz2[1] * xyz1[2];
	xyzOut[1] = xyz1[2] * xyz2[0] - xyz2[2] * xyz1[0];
	xyzOut[2] = xyz1[0] * xyz2[1] - xyz2[0] * xyz1[1];
}

void
rsScaleVec(float *xyz, float scale)
{
	xyz[0] *= scale;
	xyz[1] *= scale;
	xyz[2] *= scale;
}
