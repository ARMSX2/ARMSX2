/*
 * Copyright (C) 2010  Terence M. Welsh
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

#ifndef IMPROUNDEDHEXAHEDRON_H
#define IMPROUNDEDHEXAHEDRON_H

#include "impShape.h"

// An impRoundedHexahedron is a rectangular solid with rounded corners.
// It is defined as the inverse square falloff from a rectangular solid.
class impRoundedHexahedron : public impShape
{
	float width, height, length;  // dimension on x-, y-, and z-axes

public:
	impRoundedHexahedron() { width = height = length = 1.0f; };
	~impRoundedHexahedron() {};

	void setSize(float w, float h, float l)
	{
		width = w;
		height = h;
		length = l;
	}

	virtual float value(float* position);
};

#endif
