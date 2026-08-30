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

#ifndef IMPCRAWLPOINT_H
#define IMPCRAWLPOINT_H

#include <vector>

// For making a list of starting points for surface crawling.
class impCrawlPoint
{
public:
	float position[3];

	impCrawlPoint() {};
	~impCrawlPoint() {};

	impCrawlPoint(float x, float y, float z)
	{
		position[0] = x;
		position[1] = y;
		position[2] = z;
	};

	impCrawlPoint(float* p)
	{
		position[0] = p[0];
		position[1] = p[1];
		position[2] = p[2];
	};

	void set(float x, float y, float z)
	{
		position[0] = x;
		position[1] = y;
		position[2] = z;
	};

	void set(float* p)
	{
		position[0] = p[0];
		position[1] = p[1];
		position[2] = p[2];
	};
};

typedef std::vector<impCrawlPoint> impCrawlPointVector;

#endif
