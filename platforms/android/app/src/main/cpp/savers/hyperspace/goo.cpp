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


#include "goo.h"



extern float frameTime, simulationTime;



goo::goo(int res, float rad){
	int i, j;

	for(i=0; i<4; ++i){
		cp[i] = float(i);
		cs[i] = 0.1f + rsRandf(0.4f);
	}

	volumeSize = 2.0f;
	if(res < 5)
		res = 5;
	resolution = res;
	radius = rad;
	unitSize = volumeSize / float(res);
	arraySize = 2 * int(0.99f + radius / volumeSize);

	// Init implicit surfaces
	volume = new impCubeVolume;
	volume->init(resolution, resolution, resolution, unitSize);
	// Using exact normals instead of fast normals.  This should be slower, but it is faster
	// in this case because the surface function is so ridiculously fast.
	volume->useFastNormals(true);
	volume->setCrawlFromSides(true);
	volume->function = function;
	volume->setSurfaceValue(0.4f);
	surface = new impSurface**[arraySize];
	useSurface = new bool*[arraySize];
	for(i=0; i<arraySize; i++){
		surface[i] = new impSurface*[arraySize];
		useSurface[i] = new bool[arraySize];
		for(j=0; j<arraySize; j++){
			surface[i][j] = new impSurface;
			useSurface[i][j] = false;
		}
	}
}


goo::~goo(){
	delete[] surface;
	delete[] useSurface;
	delete volume;
}


void goo::update(float x, float z, float heading, float fov){
	int i, j;

	// update goo function constants
	for(i=0; i<4; i++){
		cp[i] += cs[i] * frameTime;
		if(cp[i] >= RS_PIx2)
			cp[i] -= RS_PIx2;
		c[i] = 0.25f * cosf(cp[i]);
	}

	float halfFov = 0.5f * fov;

	camx = x;
	camz = z;
	centerx = unitSize * float(int(0.5f + x / unitSize));
	centerz = unitSize * float(int(0.5f + z / unitSize));

	clip[0][0] = cosf(heading + halfFov);
	clip[0][1] = -sinf(heading + halfFov);
	clip[1][0] = -cosf(heading - halfFov);
	clip[1][1] = sinf(heading - halfFov);
	clip[2][0] = sinf(heading);
	clip[2][1] = -cosf(heading);

	// Empty crawl point vector.
	// This is only needed so that the right version of impCubeVolume::makeSurface() is called.
	impCrawlPointVector cpv;

	for(i=0; i<arraySize; i++){
		for(j=0; j<arraySize; j++){
			shiftx = volumeSize * (0.5f + float(i - arraySize / 2));
			shiftz = volumeSize * (0.5f + float(j - arraySize / 2));
			if(shiftx * clip[0][0] + shiftz * clip[0][1] > volumeSize * -1.41421f){
				if(shiftx * clip[1][0] + shiftz * clip[1][1] > volumeSize * -1.41421f){
					if(shiftx * clip[2][0] + shiftz * clip[2][1] < radius + volumeSize * 1.41421f){
						shiftx += centerx;
						shiftz += centerz;
						volume->setSurface(surface[i][j]);
						surface[i][j]->reset();
						volume->makeSurface(cpv);
						useSurface[i][j] = true;
					}
				}
			}
		}
	}
}


float goo::c[4];
float goo::shiftx;
float goo::shiftz;
float goo::camx;
float goo::camz;

float goo::function(float* position){
	const float px(position[0] + shiftx);
	const float pz(position[2] + shiftz);
	const float x(px - camx);
	const float z(pz - camz);

	return
		// This first term defines upper and lower surfaces.
		position[1] * position[1] * 1.25f
		// These terms make the surfaces wavy.
		+ c[0] * rsCosf(px - 2.71f * position[1])
		+ c[1] * rsCosf(4.21f * position[1] + pz)
		+ c[2] * rsCosf(1.91f * px - 1.67f * pz)
		+ c[3] * rsCosf(1.53f * px + 1.11f * position[1] + 2.11f * pz)
		// The last term creates a bubble around the eyepoint so it doesn't
		// punch through the surface.
		- 0.1f / (x * x + position[1] * position[1] + z * z);
}


void goo::draw(){
	int i, j;

	for(i=0; i<arraySize; i++){
		for(j=0; j<arraySize; j++){
			if(useSurface[i][j]){
				shiftx = centerx + volumeSize * (0.5f + float(i - arraySize / 2));
				shiftz = centerz + volumeSize * (0.5f + float(j - arraySize / 2));
				glPushMatrix();
				glTranslatef(shiftx, 0.0f, shiftz);
				surface[i][j]->draw();
				glPopMatrix();
				useSurface[i][j] = false;
			}
		}
	}
}
