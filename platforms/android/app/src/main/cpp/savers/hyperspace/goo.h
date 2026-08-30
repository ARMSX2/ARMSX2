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


#ifndef GOO_H
#define GOO_H



#ifdef WIN32
	#include <windows.h>
#endif
#include <GL/gl.h>
#include <rsMath/rsMath.h>
#include <list>
#include <Implicit/impCubeVolume.h>
#include <Implicit/impCrawlPoint.h>




class goo{
public:
	int resolution;
	float radius;
	float unitSize;
	float volumeSize;

	static float c[4];  // constants
	float cp[4];  // constants phase
	float cs[4];  // constants speed

	static float camx, camz;
	float centerx, centerz;
	static float shiftx, shiftz;
	int arraySize;
	impCubeVolume* volume;
	impSurface*** surface;
	bool** useSurface;

	// normals of planes for culling impSurfaces
	float clip[3][2];

	goo(int res, float rad);
	~goo();
	void update(float x, float z, float heading, float fov);
	static float function(float* position);
	void draw();
};




#endif
