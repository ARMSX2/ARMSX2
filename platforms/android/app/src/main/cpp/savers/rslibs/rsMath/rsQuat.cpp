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

rsQuat::rsQuat()
{
	q[0] = 0.0f;
	q[1] = 0.0f;
	q[2] = 0.0f;
	q[3] = 1.0f;
}

rsQuat::rsQuat(float x, float y, float z, float w)
{
	q[0] = x;
	q[1] = y;
	q[2] = z;
	q[3] = w;
}

rsQuat::~rsQuat()
{
	return;
}

void
rsQuat::set(float x, float y, float z, float w)
{
	q[0] = x;
	q[1] = y;
	q[2] = z;
	q[3] = w;
}

void
rsQuat::copy(rsQuat copyquat)
{
	q[0] = copyquat[0];
	q[1] = copyquat[1];
	q[2] = copyquat[2];
	q[3] = copyquat[3];
}

void
rsQuat::make(float a, float x, float y, float z)
{
	if (a < RS_EPSILON && a > -RS_EPSILON)
	{
		q[0] = 0.0f;
		q[1] = 0.0f;
		q[2] = 0.0f;
		q[3] = 1.0f;
	}
	else
	{
		a *= 0.5f;
		float sintheta = sinf(a);
		q[0] = sintheta * x;
		q[1] = sintheta * y;
		q[2] = sintheta * z;
		q[3] = cosf(a);
	}
}

void
rsQuat::make(float a, const rsVec &v)
{
	if (a < RS_EPSILON && a > -RS_EPSILON)
	{
		q[0] = 0.0f;
		q[1] = 0.0f;
		q[2] = 0.0f;
		q[3] = 1.0f;
	}
	else
	{
		a *= 0.5f;
		float sintheta = sinf(a);
		q[0] = sintheta * v[0];
		q[1] = sintheta * v[1];
		q[2] = sintheta * v[2];
		q[3] = cosf(a);
	}
}

void
rsQuat::normalize()
{
	float length = float(sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]));

	q[0] /= length;
	q[1] /= length;
	q[2] /= length;
	q[3] /= length;
}

void
rsQuat::preMult(rsQuat &postQuat)
{
	// q1q2 = s1v2 + s2v1 + v1xv2, s1s2 - v1.v2
	float tempx = q[0];
	float tempy = q[1];
	float tempz = q[2];
	float tempw = q[3];

	q[0] = tempw * postQuat[0] + postQuat[3] * tempx
		+ tempy * postQuat[2] - postQuat[1] * tempz;
	q[1] = tempw * postQuat[1] + postQuat[3] * tempy
		+ tempz * postQuat[0] - postQuat[2] * tempx;
	q[2] = tempw * postQuat[2] + postQuat[3] * tempz
		+ tempx * postQuat[1] - postQuat[0] * tempy;
	q[3] = tempw * postQuat[3]
		- tempx * postQuat[0]
		- tempy * postQuat[1]
		- tempz * postQuat[2];
}

void
rsQuat::postMult(rsQuat &preQuat)
{
	float tempx = q[0];
	float tempy = q[1];
	float tempz = q[2];
	float tempw = q[3];

	q[0] = preQuat[3] * tempx + tempw * preQuat[0]
		+ preQuat[1] * tempz - tempy * preQuat[2];
	q[1] = preQuat[3] * tempy + tempw * preQuat[1]
		+ preQuat[2] * tempx - tempz * preQuat[0];
	q[2] = preQuat[3] * tempz + tempw * preQuat[2]
		+ preQuat[0] * tempy - tempx * preQuat[1];
	q[3] = preQuat[3] * tempw
		- preQuat[0] * tempx
		- preQuat[1] * tempy
		- preQuat[2] * tempz;
}

void
rsQuat::toMat(float *mat)
{
	float s, xs, ys, zs, wx, wy, wz, xx, xy, xz, yy, yz, zz;

	// must have an axis
	if (q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f)
	{
		mat[0] = 1.0f;
		mat[1] = 0.0f;
		mat[2] = 0.0f;
		mat[3] = 0.0f;
		mat[4] = 0.0f;
		mat[5] = 1.0f;
		mat[6] = 0.0f;
		mat[7] = 0.0f;
		mat[8] = 0.0f;
		mat[9] = 0.0f;
		mat[10] = 1.0f;
		mat[11] = 0.0f;
		mat[12] = 0.0f;
		mat[13] = 0.0f;
		mat[14] = 0.0f;
		mat[15] = 1.0f;
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

	mat[0] = 1.0f - yy - zz;
	mat[1] = xy + wz;
	mat[2] = xz - wy;
	mat[3] = 0.0f;
	mat[4] = xy - wz;
	mat[5] = 1.0f - xx - zz;
	mat[6] = yz + wx;
	mat[7] = 0.0f;
	mat[8] = xz + wy;
	mat[9] = yz - wx;
	mat[10] = 1.0f - xx - yy;
	mat[11] = 0.0f;
	mat[12] = 0.0f;
	mat[13] = 0.0f;
	mat[14] = 0.0f;
	mat[15] = 1.0f;
}

void
rsQuat::fromMat(float* mat)
{
	float a, b;
	int i;

	a = mat[0] + mat[5] + mat[10];
	if (a > 0.0)
	{
		b = float(sqrt(a + 1.0f));
		q[3] = b * 0.5f;
		b = 0.5f / b;

		q[0] = (mat[6] - mat[9]) * b;
		q[1] = (mat[8] - mat[2]) * b;
		q[2] = (mat[1] - mat[4]) * b;
	}
	else
	{
		i = 0;
		if (mat[5] > mat[0])
			i = 1;
		if (mat[10] > mat[5])
			i = 2;

		if (i == 0)
		{
			b = float(sqrt(mat[0] - mat[5] - mat[10] + 1.0f));
			q[0] *= 0.5f;
			b = 0.5f / b;
			q[3] = (mat[6] - mat[9]) * b;
			q[1] = (mat[1] - mat[4]) * b;
			q[2] = (mat[2] - mat[8]) * b;
		}
		if (i == 1)
		{
			b = float(sqrt(mat[5] - mat[10] - mat[0] + 1.0f));
			q[1] *= 0.5f;
			b = 0.5f / b;
			q[3] = (mat[8] - mat[2]) * b;
			q[2] = (mat[6] - mat[9]) * b;
			q[0] = (mat[4] - mat[1]) * b;
		}
		if (i == 2)
		{
			b = float(sqrt(mat[10] - mat[0] - mat[5] + 1.0f));
			q[2] *= 0.5f;
			b = 0.5f / b;
			q[3] = (mat[1] - mat[4]) * b;
			q[0] = (mat[8] - mat[2]) * b;
			q[1] = (mat[9] - mat[6]) * b;
		}
	}
}

void
rsQuat::fromEuler(float yaw, float pitch, float roll)
{
	float cy, cp, cr, sy, sp, sr, cpcy, spsy;

	cy = cosf(yaw * 0.5f);
	cp = cosf(pitch * 0.5f);
	cr = cosf(roll * 0.5f);

	sy = sinf(yaw * 0.5f);
	sp = sinf(pitch * 0.5f);
	sr = sinf(roll * 0.5f);

	cpcy = cp * cy;
	spsy = sp * sy;

	q[3] = cr * cpcy + sr * spsy;
	q[0] = sr * cpcy - cr * spsy;
	q[1] = cr * sp * cy + sr * cp * sy;
	q[2] = cr * cp * sy - sr * sp * cy;
}

void
rsQuat::slerp(rsQuat a, rsQuat b, float t)
{
	float n, cn, sn, scalea, scaleb;

	cn = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if ((1.0f + cn) > RS_EPSILON)
	{
		if ((1.0f - cn) > RS_EPSILON)
		{
			n = acosf(cn);
			sn = sinf(n);
			scalea = sinf((1.0f - t) * n) / sn;
			scaleb = sinf(t * n) / sn;
		}
		else
		{
			scalea = 1.0f - t;
			scaleb = t;
		}
		q[0] = scalea * a[0] + scaleb * b[0];
		q[1] = scalea * a[1] + scaleb * b[1];
		q[2] = scalea * a[2] + scaleb * b[2];
		q[3] = scalea * a[3] + scaleb * b[3];
	}
	else
	{
		scalea = sinf((1.0f - t) * RS_PIo2);
		scaleb = sinf(t * RS_PIo2);
		q[0] = scalea * a[0] - scaleb * q[1];
		q[1] = scalea * a[1] + scaleb * q[0];
		q[2] = scalea * a[2] - scaleb * q[3];
		q[3] = scalea * a[3] + scaleb * q[2];
	}
}
