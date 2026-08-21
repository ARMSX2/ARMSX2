/*
 * Copyright (C) 2005-2010  Terence M. Welsh
 *
 * This file is part of Hyperspace.
 *
 * Hyperspace is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Hyperspace is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#ifndef SPLINEPATH_H
#define SPLINEPATH_H



class splinePath{
public:
	float* phase;
	float* rate;
	float** movevec;
	float** basexyz;
	float** xyz;
	float** basedir;
	float** dir;
	int numPoints;
	float step;

	splinePath(int length);
	~splinePath();
	void moveAlongPath(float increment);
	void getPoint(int section, float where, float* position);
	void getDirection(int section, float where, float* direction);
	void getBaseDirection(int section, float where, float* direction);
	void update(float multiplier);

private:
	void makeNewPoint();
	float interpolate(float a, float b, float c, float d, float where);
};



#endif
