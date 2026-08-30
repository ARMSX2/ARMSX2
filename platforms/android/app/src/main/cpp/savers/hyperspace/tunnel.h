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


#ifndef TUNNEL_H
#define TUNNEL_H



#include "splinePath.h"



class tunnel{
public:
	splinePath* path;
	int resolution;
	int numSections;
	int section;
	float**** v;  // vertex data
	float**** t;  // texture coordinate data
	float**** c;  // color data

	float radius;
	// offset for tunnel width
	float widthOffset;

	// spin texture map around tunnel
	float texSpin;

	// offsets for low and high frequency 3D color waves
	float huelo, huehi;
	float satlo, sathi;
	float lumlo, lumhi;

	tunnel(splinePath* sp, int res);
	~tunnel();
	void make(float frameTime);
	void draw();
	void draw(float lerp);
};



#endif
