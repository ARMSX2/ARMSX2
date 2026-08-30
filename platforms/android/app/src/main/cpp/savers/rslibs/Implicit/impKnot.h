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

#ifndef IMPKNOT_H
#define IMPKNOT_H

#include "impShape.h"

class impKnot : public impShape
{
private:
	float radius1;
	float radius2;
	int coils, twists;  // integer number of coils and twists, respectively
	float coilsf, twistsf;  // floating point versions of n and m
	float twistsOverCoils, lat_offset;  // precomputed variables

public:
	impKnot()
	{
		radius1 = 1.0f;
		radius2 = 0.5f;
		coils = 3;
		coilsf = float(coils);
		twists = 2;
		twistsf = float(twists);
		twistsOverCoils = twistsf / coilsf;
		lat_offset = 6.28318530718f / coilsf;
	}
	~impKnot() {};

	// position is an array of 3 floats
	// returns the field strenth of this sphere at a given position
	void setRadius1(float r) { radius1 = r; }
	float getRadius1() { return radius1; }
	void setRadius2(float r) { radius2 = r; }
	float getRadius2() { return radius2; }

	void setNumCoils(int c)
	{
		coils = (c < 1 ? 1 : c);  // coils must be greater than 1
		coilsf = float(coils);
		twistsOverCoils = twistsf / coilsf;
		lat_offset = 6.28318530718f / coilsf;
	}

	int getNumCoils() { return coils; }

	void setNumTwists(int t)
	{
		twists = t;
		twistsf = float(twists);
		twistsOverCoils = twistsf / coilsf;
	}
	int getNumTwists() { return twists; }

	virtual float value(float* position);
	virtual void center(float* position);
	virtual void addCrawlPoint(impCrawlPointVector& cpv);
};

#endif
