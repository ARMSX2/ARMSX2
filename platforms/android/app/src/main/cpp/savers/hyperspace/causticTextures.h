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


#ifndef CAUSTICS_H
#define CAUSTICS_H


#ifdef WIN32
#include <windows.h>
#endif
#include <GL/gl.h>


class causticTextures{
public:
	int numKeys;
	int numFrames;
	int geoRes;
	int texSize;
	float waveAmp;
	float refractionMult;

	// textures indices for OpenGL texture objects
	GLuint* caustictex;

	// space for storing geometry of water surface
	float* x;  // x and z are the same for each frame
	float* z;
	float*** y;  // y (altitude) is different

	float*** xz;  // projected vertex positions
	float** intensity;  // projected light intensity

	GLubyte** bitmap;

	// constructor takes the following parameters:
	// num keyframes (only for fractal heightfield generation),
	// number of frames, geometry resolution, texture resolution, water depth, wave amplitude,
	// refraction multiplier (sort of like index of refraction)
	causticTextures(int keys, int frames, int res, int size, float depth, float wa, float rm);
	~causticTextures(){}

private:
	void makeFractalAltitudes();
	void makeTrigAltitudes();
	void altitudeSquare(int left, int right, int bottom, int top, float** alt);
	void draw(int xlo, int xhi, int zlo, int zhi);
	void makeIndices(int index, int* minus, int* plus);
	float myFabs(float x){if(x<0) return -x; return x;};
	float interpolate(float a, float b, float c, float d, float where);
};


#endif
