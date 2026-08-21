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


#include "splinePath.h"
#include <rsMath/rsMath.h>
#include <math.h>



splinePath::splinePath(int length){
	int i;

	step = 0.0f;

	numPoints = length;
	// 6 is the minimum number of points necessary for a tunnel to have one segment
	if(numPoints < 6)
		numPoints = 6;

	phase = new float[numPoints];
	rate = new float[numPoints];
	movevec = new float*[numPoints];
	basexyz = new float*[numPoints];
	xyz = new float*[numPoints];
	basedir = new float*[numPoints];
	dir = new float*[numPoints];
	for(i=0; i<numPoints; i++){
		movevec[i] = new float[3];
		basexyz[i] = new float[3];
		xyz[i] = new float[3];
		basedir[i] = new float[3];
		dir[i] = new float[3];
	}

	basexyz[numPoints-1][0] = 0.0f;
	basexyz[numPoints-1][1] = 0.0f;
	basexyz[numPoints-1][2] = 0.0f;
	basexyz[numPoints-2][0] = 0.0f;
	basexyz[numPoints-2][1] = 0.0f;
	basexyz[numPoints-2][2] = 4.0f;

	for(i=0; i<numPoints; i++)
		makeNewPoint();
}


splinePath::~splinePath(){
	delete[] xyz;
}


void splinePath::moveAlongPath(float increment){
	step += increment;
	while(step >= 1.0f){
		step -= 1.0f;
		makeNewPoint();
	}
}


// "section" indicates the pair of points you're between (from 1 to numPoints-1)
// "where" indicates where you are between that pair of points (0.0 - 1.0)
// "position" receives the information about the point you want
void splinePath::getPoint(int section, float where, float* position){
	if(section < 1)
		section = 1;
	if(section > numPoints - 3)
		section = numPoints - 3;

	position[0] = interpolate(xyz[section-1][0], xyz[section][0],
		xyz[section+1][0], xyz[section+2][0], where);
	position[1] = interpolate(xyz[section-1][1], xyz[section][1],
		xyz[section+1][1], xyz[section+2][1], where);
	position[2] = interpolate(xyz[section-1][2], xyz[section][2],
		xyz[section+1][2], xyz[section+2][2], where);
}


// "section" indicates the pair of points you're between (from 1 to numPoints-1)
// "where" indicates where you are between that pair of points (0.0 - 1.0)
// "direction" receives the information about the direction you want
void splinePath::getDirection(int section, float where, float* direction){
	if(section < 1)
		section = 1;
	if(section > numPoints - 3)
		section = numPoints - 3;

	direction[0] = interpolate(dir[section-1][0], dir[section][0],
		dir[section+1][0], dir[section+2][0], where);
	direction[1] = interpolate(dir[section-1][1], dir[section][1],
		dir[section+1][1], dir[section+2][1], where);
	direction[2] = interpolate(dir[section-1][2], dir[section][2],
		dir[section+1][2], dir[section+2][2], where);

	const float normalizer(1.0f / sqrtf(direction[0] * direction[0]
		+ direction[1] * direction[1] + direction[2] * direction[2]));
	direction[0] *= normalizer;
	direction[1] *= normalizer;
	direction[2] *= normalizer;
}


// "section" indicates the pair of points you're between (from 1 to numPoints-1)
// "where" indicates where you are between that pair of points (0.0 - 1.0)
// "direction" receives the information about the direction you want
void splinePath::getBaseDirection(int section, float where, float* direction){
	if(section < 1)
		section = 1;
	if(section > numPoints - 3)
		section = numPoints - 3;

	direction[0] = interpolate(basedir[section-1][0], basedir[section][0],
		basedir[section+1][0], basedir[section+2][0], where);
	direction[1] = interpolate(basedir[section-1][1], basedir[section][1],
		basedir[section+1][1], basedir[section+2][1], where);
	direction[2] = interpolate(basedir[section-1][2], basedir[section][2],
		basedir[section+1][2], basedir[section+2][2], where);

	const float normalizer(1.0f / sqrtf(direction[0] * direction[0]
		+ direction[1] * direction[1] + direction[2] * direction[2]));
	direction[0] *= normalizer;
	direction[1] *= normalizer;
	direction[2] *= normalizer;
}


void splinePath::update(float multiplier){
	int i;

	// calculate xyz positions
	for(i=0; i<numPoints; i++){
		phase[i] += rate[i] * multiplier;
		xyz[i][0] = basexyz[i][0] + movevec[i][0] * cosf(phase[i]);
		xyz[i][1] = basexyz[i][1] + movevec[i][1] * cosf(phase[i]);
		xyz[i][2] = basexyz[i][2] + movevec[i][2] * cosf(phase[i]);
	}

	// calculate direction vectors
	for(i=1; i<numPoints-1; i++){
		dir[i][0] = xyz[i+1][0] - xyz[i-1][0];
		dir[i][1] = xyz[i+1][1] - xyz[i-1][1];
		dir[i][2] = xyz[i+1][2] - xyz[i-1][2];
	}
}


void splinePath::makeNewPoint(){
	int i;

	// shift points to rear of path
	for(i=0; i<numPoints-1; i++){
		basexyz[i][0] = basexyz[i+1][0];
		basexyz[i][1] = basexyz[i+1][1];
		basexyz[i][2] = basexyz[i+1][2];
		movevec[i][0] = movevec[i+1][0];
		movevec[i][1] = movevec[i+1][1];
		movevec[i][2] = movevec[i+1][2];
		xyz[i][0] = xyz[i+1][0];
		xyz[i][1] = xyz[i+1][1];
		xyz[i][2] = xyz[i+1][2];
		phase[i] = phase[i+1];
		rate[i] = rate[i+1];
	}

	// make vector to new point
	int lastPoint = numPoints - 1;
	float tempx = basexyz[lastPoint-1][0] - basexyz[lastPoint-2][0];
	float tempz = basexyz[lastPoint-1][2] - basexyz[lastPoint-2][2];

	// find good angle
	float turnAngle;
	const float pathAngle = atan2f(tempx, tempz);
	const float dist_from_center = sqrtf(basexyz[lastPoint][0] * basexyz[lastPoint][0] + basexyz[lastPoint][2] * basexyz[lastPoint][2]);
	if(dist_from_center > 100.0f){
		const float angleToCenter = atan2f(-basexyz[lastPoint][0], -basexyz[lastPoint][2]);
		turnAngle = angleToCenter - pathAngle;
		if(turnAngle > RS_PI)
			turnAngle -= RS_PIx2;
		if(turnAngle < -RS_PI)
			turnAngle += RS_PIx2;
		if(turnAngle > 0.7f)
			turnAngle = 0.7f;
		if(turnAngle < -0.7f)
			turnAngle = -0.7f;
	}
	else
		turnAngle = rsRandf(1.4f) - 0.7f;

	// rotate new point to some new position
	float ca = cosf(turnAngle);
	float sa = sinf(turnAngle);
	basexyz[lastPoint][0] = tempx * ca + tempz * sa;
	basexyz[lastPoint][1] = 0.0f;
	basexyz[lastPoint][2] = tempx * -sa + tempz * ca;

	// normalize and set length of vector
	// make it at least length 2, which is the grid size of the goo
	float lengthener = (rsRandf(6.0f) + 2.0f) / sqrtf(basexyz[lastPoint][0] * basexyz[lastPoint][0]
		+ basexyz[lastPoint][2] * basexyz[lastPoint][2]);
	basexyz[lastPoint][0] *= lengthener;
	basexyz[lastPoint][2] *= lengthener;

	// make new movement vector proportional to base vector
	movevec[lastPoint][0] = rsRandf(0.25f) * -basexyz[lastPoint][2];
	movevec[lastPoint][1] = 0.3f;
	movevec[lastPoint][2] = rsRandf(0.25f) * basexyz[lastPoint][0];

	// add vector to previous point to get new point
	basexyz[lastPoint][0] += basexyz[lastPoint-1][0];
	basexyz[lastPoint][2] += basexyz[lastPoint-1][2];

	// make new phase and movement rate
	phase[lastPoint] = rsRandf(RS_PIx2);
	rate[lastPoint] = rsRandf(1.0f);

	// reset base direction vectors
	for(i=1; i<numPoints-2; i++){
		basedir[i][0] = basexyz[i+1][0] - basexyz[i-1][0];
		basedir[i][1] = basexyz[i+1][1] - basexyz[i-1][1];
		basedir[i][2] = basexyz[i+1][2] - basexyz[i-1][2];
	}
}


// Here's a little calculus that takes 4 points along a single
// dimension and interpolates smoothly between the second and third
// depending on the value of where which can be 0.0 to 1.0.
// The slope at b is estimated using a and c.  The slope at c
// is estimated using b and d.
float splinePath::interpolate(float a, float b, float c, float d, float where){
    float q, r, s, t;

    q = (((3.0f * b) + d - a - (3.0f * c)) * (where * where * where)) * 0.5f;
    r = (((2.0f * a) - (5.0f * b) + (4.0f * c) - d) * (where * where)) * 0.5f;
    s = ((c - a) * where) * 0.5f;
    t = b;
    return(q + r + s + t);
}
