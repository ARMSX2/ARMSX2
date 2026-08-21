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

#include "impSphere.h"

float
impSphere::value(float* position)
{
	const float tx(invmat[12] + position[0]);
	const float ty(invmat[13] + position[1]);
	const float tz(invmat[14] + position[2]);

	// Use thickness instead of relying on scale to be in the matrix
	// because the value computation for a sphere is simplified by
	// using an incomplete matrix.
	return thicknessSquared / (tx * tx + ty * ty + tz * tz + IMP_MIN_DIVISOR);
}
