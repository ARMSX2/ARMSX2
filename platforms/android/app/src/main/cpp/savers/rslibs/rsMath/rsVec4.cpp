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

rsVec4::rsVec4()
{
}

rsVec4::rsVec4(float x, float y, float z, float w)
{
	v[0] = x;
	v[1] = y;
	v[2] = z;
	v[3] = w;
}

rsVec4::~rsVec4()
{
}

void
rsVec4::set(float x, float y, float z, float w)
{
	v[0] = x;
	v[1] = y;
	v[2] = z;
	v[3] = w;
}

float
rsVec4::length()
{
	return(float(sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3])));
}

float
rsVec4::normalize()
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
	if (length == 0.0f)
	{
		v[1] = 1.0f;

		return(0.0f);
	}

	const float normalizer(1.0f / length);
	v[0] *= normalizer;
	v[1] *= normalizer;
	v[2] *= normalizer;
	v[3] *= normalizer;

	return(length);
}

float
rsVec4::dot(rsVec4 vec1)
{
	return(v[0] * vec1[0] + v[1] * vec1[1] + v[2] * vec1[2] + v[3] * vec1[3]);
}

void
rsVec4::cross(rsVec4 vec1, rsVec4 vec2)
{
	v[0] = vec1[1] * vec2[2] - vec2[1] * vec1[2];
	v[1] = vec1[2] * vec2[3] - vec2[2] * vec1[3];
	v[2] = vec1[3] * vec2[0] - vec2[3] * vec1[0];
	v[3] = vec1[0] * vec2[1] - vec2[0] * vec1[1];
}

void
rsVec4::scale(float scale)
{
	v[0] *= scale;
	v[1] *= scale;
	v[2] *= scale;
	v[3] *= scale;
}

void
rsVec4::transPoint(const rsMatrix &m)
{
	float x = v[0];
	float y = v[1];
	float z = v[2];
	float w = v[3];

	v[0] = x * m[0] + y * m[4] + z * m[8] + w * m[12];
	v[1] = x * m[1] + y * m[5] + z * m[9] + w * m[13];
	v[2] = x * m[2] + y * m[6] + z * m[10] + w * m[14];
	v[3] = x * m[3] + y * m[7] + z * m[11] + w * m[15];
}

void
rsVec4::transVec(const rsMatrix &m)
{
	float x = v[0];
	float y = v[1];
	float z = v[2];

	v[0] = x * m[0] + y * m[4] + z * m[8];
	v[1] = x * m[1] + y * m[5] + z * m[9];
	v[2] = x * m[2] + y * m[6] + z * m[10];
}

int
rsVec4::almostEqual(rsVec4 vec, float tolerance)
{
	if (sqrtf((v[0] - vec[0])*(v[0] - vec[0])
		+ (v[1] - vec[1])*(v[1] - vec[1])
		+ (v[2] - vec[2])*(v[2] - vec[2])
		+ (v[3] - vec[3])*(v[3] - vec[3]))
		<= tolerance)
		return 1;
	else
		return 0;
}
