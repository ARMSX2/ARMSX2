/*
 * Copyright (C) 2005-2010  Terence M. Welsh
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

#include "impKnot.h"

#include <rsMath/rsMath.h>

float
impKnot::value(float* position)
{
	const float& x(position[0]);
	const float& y(position[1]);
	const float& z(position[2]);

	const float tx(x * invtrmat[0] + y * invtrmat[1] + z * invtrmat[2] + invtrmat[3]);
	const float ty(x * invtrmat[4] + y * invtrmat[5] + z * invtrmat[6] + invtrmat[7]);
	const float tz(x * invtrmat[8] + y * invtrmat[9] + z * invtrmat[10] + invtrmat[11]);

	const float temp(rsSqrtf(tx * tx + ty * ty) - radius1);
	const float lat(rsAtan2f(ty, tx) * twistsOverCoils);
	float retval(0.0f);

	for (int i = 0; i < coils; ++i)
	{
		const float lon(lat + lat_offset * float(i));
		const float hor(temp - rsCosf(lon) * radius2);
		const float ver(tz - rsSinf(lon) * radius2);
		retval += thicknessSquared / (hor * hor + ver * ver + IMP_MIN_DIVISOR);
	}

	return retval;
}

// Finding a point inside a knot is trickier than
// finding a point inside a sphere or ellipsoid.
void
impKnot::center(float* position)
{
	position[0] = mat[0] * (radius1 + radius2) + mat[12];
	position[1] = mat[1] * (radius1 + radius2) + mat[13];
	position[2] = mat[2] * (radius1 + radius2) + mat[14];
}

void
impKnot::addCrawlPoint(impCrawlPointVector& cpv)
{
	const float step(6.28318530718f / float(coils));
	for (int i = 0; i < coils; ++i)
	{
		const float x(radius1 + cosf(float(i) * step) * radius2);
		const float z(sinf(float(i) * step) * radius2);
		cpv.push_back(impCrawlPoint(mat[0] * x + mat[8] * z + mat[12],
		        mat[1] * x + mat[9] * z + mat[13],
		        mat[2] * x + mat[10] * z + mat[14]));
	}
}
