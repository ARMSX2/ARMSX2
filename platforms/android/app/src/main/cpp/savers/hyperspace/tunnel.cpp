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


#ifdef WIN32
	#include <windows.h>
#endif
#include "tunnel.h"
#include <math.h>
#include <GL/gl.h>
#include <rsMath/rsMath.h>
#include <Rgbhsl/Rgbhsl.h>


extern int dShaders;


tunnel::tunnel(splinePath* sp, int res){
	int i, j, k;

	path = sp;
	resolution = res;
	radius = 0.1f;
	widthOffset = 0.0f;
	texSpin = 0.0f;

	numSections = path->numPoints - 5;
	section = 0;

	v = new float***[numSections];
	t = new float***[numSections];
	c = new float***[numSections];
	for(i=0; i<numSections; i++){
		v[i] = new float**[resolution+1];
		t[i] = new float**[resolution+1];
		c[i] = new float**[resolution+1];
		for(j=0; j<=resolution; j++){
			v[i][j] = new float*[resolution+1];
			t[i][j] = new float*[resolution+1];
			c[i][j] = new float*[resolution+1];
			for(k=0; k<=resolution; k++){
				v[i][j][k] = new float[3];
				t[i][j][k] = new float[2];
				c[i][j][k] = new float[3];
			}
		}
	}
	
	huelo = 0.0f;
	huehi = 0.0f;
	satlo = 0.0f;
	sathi = 0.0f;
	lumlo = 3.14f;  // tunnel will be invisible at first
	lumhi = 3.14f;
}


tunnel::~tunnel(){
	delete[] v;
	delete[] t;
}


void tunnel::make(float frameTime){
	int i, j, k;

	widthOffset += frameTime * 1.5f;
	while(widthOffset >= RS_PIx2)
		widthOffset -= RS_PIx2;
	texSpin += frameTime * 0.1f;
	while(texSpin >= RS_PIx2)
		texSpin -= RS_PIx2;

	huelo += frameTime * 0.04f;
	huehi += frameTime * 0.15f;
	satlo += frameTime * 0.04f;
	sathi += frameTime;
	lumlo += frameTime * 0.04f;
	lumhi += frameTime * 0.5f;
	while(huelo >= RS_PIx2)
		huelo -= RS_PIx2;
	while(huehi >= RS_PIx2)
		huehi -= RS_PIx2;
	while(satlo >= RS_PIx2)
		satlo -= RS_PIx2;
	while(sathi >= RS_PIx2)
		sathi -= RS_PIx2;
	while(lumlo >= RS_PIx2)
		lumlo -= RS_PIx2;
	while(lumhi >= RS_PIx2)
		lumhi -= RS_PIx2;

	float angle;
	float pos[3], dir[3];
	rsVec vert;
	rsMatrix rotMat;
	float hsl[3];
	const float texcoordmult = (dShaders > 0) ? 1.0f : 2.0f;
	const float max_saturation = (dShaders > 0) ? 0.1f : 0.4f;
	for(k=0; k<numSections; k++){
		// create new vertex data for this section
		for(i=0; i<=resolution; i++){
			angle = 0.0f;
			const float whereOnPath(float(i) / float(resolution));
			path->getPoint(k+2, whereOnPath, pos);
			path->getDirection(k+2, whereOnPath, dir);
			// z component of rotation matrix points in direction of path
			rotMat.m[8] = -dir[0];
			rotMat.m[9] = -dir[1];
			rotMat.m[10] = -dir[2];
			// x component of rotation matrix = cross product of (0,1,0) and z component
			rotMat.m[0] = rotMat.m[10];
			rotMat.m[1] = 0.0f;
			rotMat.m[2] = -rotMat.m[8];
			// y component of rotation matrix = cross product of z component and x component
			rotMat.m[4] = rotMat.m[9] * rotMat.m[2] - rotMat.m[10] * rotMat.m[1];
			rotMat.m[5] = rotMat.m[10] * rotMat.m[0] - rotMat.m[8] * rotMat.m[2];
			rotMat.m[6] = rotMat.m[8] * rotMat.m[1] - rotMat.m[9] * rotMat.m[0];
			for(j=0; j<=resolution; j++){
				angle = float(j) * RS_PIx2 / float(resolution);
				vert[0] = (radius + radius * 0.5f * rsCosf(2.0f * pos[0] + widthOffset)) * rsCosf(angle);
				vert[1] = (radius + radius * 0.5f * rsCosf(pos[2] + widthOffset)) * rsSinf(angle);
				vert[2] = 0.0f;
				vert.transVec(rotMat);
				// set vertex coordinates
				v[k][i][j][0] = pos[0] + vert[0];
				v[k][i][j][1] = pos[1] + vert[1];
				v[k][i][j][2] = pos[2] + vert[2];
				// set texture coordinates
				t[k][i][j][0] = texcoordmult * float(i) / float(resolution);
				t[k][i][j][1] = texcoordmult * float(j) / float(resolution) + rsCosf(texSpin);
				// set colors
				hsl[0] = 2.0f * rsCosf(0.1f * v[k][i][j][0] + huelo) - 1.0f;
				hsl[1] = 0.25f * (rsCosf(0.013f * v[k][i][j][1] - satlo)
					+ rsCosf(v[k][i][j][2] + sathi) + 2.0f);
				hsl[2] = 2.0f * rsCosf(0.01f * v[k][i][j][2] + lumlo)
					+ rsCosf(0.4f * v[k][i][j][0] - lumhi)
					+ 0.3f * rsCosf(4.0f * (v[k][i][j][0] + v[k][i][j][1] + v[k][i][j][2]));
				while(hsl[0] < 0.0f)
					hsl[0] += 1.0f;
				while(hsl[0] > 1.0f)
					hsl[0] -= 1.0f;
				if(hsl[1] < 0.0f)
					hsl[1] = 0.0f;
				if(hsl[1] > max_saturation)
					hsl[1] = max_saturation;
				if(hsl[2] < 0.0f)
					hsl[2] = 0.0f;
				if(hsl[2] > 1.0f)
					hsl[2] = 1.0f;
				hsl2rgb(hsl[0], hsl[1], hsl[2], c[k][i][j][0], c[k][i][j][1], c[k][i][j][2]);
			}
		}
	}
}


void tunnel::draw(){
	int i, j, k;

	for(k=0; k<numSections; k++){
		for(i=0; i<resolution; i++){
			glBegin(GL_TRIANGLE_STRIP);
			for(j=0; j<=resolution; j++){
				glColor3fv(c[k][i+1][j]);
				glTexCoord2fv(t[k][i+1][j]);
				glVertex3fv(v[k][i+1][j]);
				glColor3fv(c[k][i][j]);
				glTexCoord2fv(t[k][i][j]);
				glVertex3fv(v[k][i][j]);
			}
			glEnd();
		}
	}
}


// specialty drawing function for when using shaders.
// lerp goes into color alpha and determines blend between
// texture frames.
void tunnel::draw(float lerp){
	for(int k=0; k<numSections; k++){
		for(int i=0; i<resolution; i++){
			glBegin(GL_TRIANGLE_STRIP);
			for(int j=0; j<=resolution; j++){
				glColor4f(c[k][i+1][j][0], c[k][i+1][j][1], c[k][i+1][j][2], lerp);
				glTexCoord2fv(t[k][i+1][j]);
				glVertex3fv(v[k][i+1][j]);
				glColor4f(c[k][i][j][0], c[k][i][j][1], c[k][i][j][2], lerp);
				glTexCoord2fv(t[k][i][j]);
				glVertex3fv(v[k][i][j]);
			}
			glEnd();
		}
	}
}
