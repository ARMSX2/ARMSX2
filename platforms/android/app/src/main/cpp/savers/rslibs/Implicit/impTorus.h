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

#ifndef IMPTORUS_H
#define IMPTORUS_H

#include "impShape.h"

class  impTorus : public impShape
{
private:
	float radius;

public:
	impTorus() { radius = 1.0f; }
	~impTorus() {};

	void setRadius(float r) { radius = r; }
	float getRadius() { return radius; }

	// position is an array of 3 floats
	// returns the field strenth of this sphere at a given position
	virtual float value(float* position);
	virtual void center(float* position);
	virtual void addCrawlPoint(impCrawlPointVector& cpv);
};

#endif
