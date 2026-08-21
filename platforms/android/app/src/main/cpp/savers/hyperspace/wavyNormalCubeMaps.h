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


#ifndef WAVYNORMALCUBEMAPS_H
#define WAVYNORMALCUBEMAPS_H



#ifdef WIN32
	#include <windows.h>
#endif
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glext.h>



class wavyNormalCubeMaps{
public:
	int numFrames;
	int texSize;

	GLuint* texture;

	// Phase must be 0.0 at frame 0
	// and phase must be 1.0 at frame numFrames
	// so that cubemaps have temporal continuity.
	float phase;

	wavyNormalCubeMaps(int frames, int size);
	~wavyNormalCubeMaps(){}
	// Pass a point on a unit sphere and receive the corresponding
	// normal at that point on the sphere
	void wavyfunc(float* point, float* normal);
	inline void normalize(float* vector);
};



#endif
