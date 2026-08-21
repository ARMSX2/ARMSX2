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

#ifndef IMPSHAPE_H
#define IMPSHAPE_H

// This is a generic class from which specific elements are
// derived.  An element would be a sphere or torus or anything
// that defines an implicit surface.
#include <math.h>
#ifdef __SSE__
#include <malloc.h>
#include <xmmintrin.h>
#endif

#include "impCrawlPoint.h"

// Minimum divisor to be used when computing inverse square falloffs in various impShapes.
#define IMP_MIN_DIVISOR 0.0001f

class impShape
{
public:
#ifdef __SSE__
	__m128 invtrmatrow[4];  // inverted and then transposed
#endif
	float mat[16];
	float invmat[16];  // inverted
	float invtrmat[16];  // inverted and then transposed

	// Thickness is only necessary in torii and knots, but can be used in other shapes.
	// Thickness is different from scale (which can be applied using an element's
	// matrix) because it only scales the width of appendages and not their relative
	// distances.
	// thicknessSquared is often useful in the value() function, so it is precomputed.
	float thickness, thicknessSquared;

	impShape();
	~impShape() {};

	void setPosition(float x, float y, float z);
	void setPosition(float* position) { setPosition(position[0], position[1], position[2]); }
	void setMatrix(float* m);

	float determinant3(const float aa, const float ab, const float ac,
	    const float ba, const float bb, const float bc,
	    const float ca, const float cb, const float cc);
	bool invertMatrix();

	void setThickness(float t)
	{
		thickness = t;
		thicknessSquared = thickness * thickness;
	}

	float getThickness() { return thickness; }

	// returns the value of this shape at a given position
	// "position" is an array of 3 floats
	virtual float value(float* position);

	// assigns a center of the element's volume to "position"
	virtual void center(float* position);

	// adds surface crawler start position(s) to given crawlPointVector
	virtual void addCrawlPoint(impCrawlPointVector& cpv);

	// When using SSE, __m128 will not be byte aligned when used in a class.  Overriding
	// new and delete ensures this whole class gets aligned to a 16-byte boundary.
#ifdef __SSE__
	void* operator new(size_t size)
	{
#ifdef WIN32
		return _aligned_malloc(size, 16);
#else
		return memalign(16, size);
#endif
	}
	void operator delete (void* mem)
	{
#ifdef WIN32
		_aligned_free(mem);
#else
		free(mem);
#endif
	}
#endif
};

#endif
