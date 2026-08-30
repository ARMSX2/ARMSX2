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
	extern HDC hdc;
#endif
#ifdef RS_XSCREENSAVER
#include <rsXScreenSaver/rsXScreenSaver.h>
#endif
#include <iostream>
#include <math.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <rsMath/rsMath.h>

#include "causticTextures.h"

causticTextures::causticTextures(int keys, int frames, int res, int size, float depth, float wa, float rm){
	int i, j, k;
	int xminus, xplus, zminus, zplus;
	int viewport[4];
	unsigned char* bitmap = new unsigned char[size * size * 3];

	// initialize dimensions
	numKeys = keys;
	if(numKeys < 2)
		numKeys = 2;
	numFrames = frames;
	if(numFrames < numKeys * 2)
		numFrames = numKeys * 2;
	geoRes = res;
	if(geoRes < 8)
		geoRes = 8;
	texSize = size;
	if(texSize < 8)
		texSize = 8;
	waveAmp = wa;
	refractionMult = rm;

	caustictex = new GLuint[numFrames];
	glGenTextures(numFrames, caustictex);

	// allocate memory
	x = new float[geoRes + 1];
	z = new float[geoRes + 1];
	y = new float**[numFrames];
	for(k=0; k<numFrames; k++){
		y[k] = new float*[geoRes];
		for(i=0; i<geoRes; i++)
			y[k][i] = new float[geoRes];
	}
	xz = new float**[geoRes + 1];
	for(i=0; i<=geoRes; i++){
		xz[i] = new float*[geoRes + 1];
		for(j=0; j<=geoRes; j++)
			xz[i][j] = new float[2];
	}
	intensity = new float*[geoRes + 1];
	for(i=0; i<=geoRes; i++)
		intensity[i] = new float[geoRes + 1];

	// set x and z geometry positions
	for(i=0; i<=geoRes; i++){
		x[i] = float(i) / float(geoRes);
		z[i] = float(i) / float(geoRes);
	}

	// set y geometry positions (altitudes)
	// fractal altitudes is sort of ugly, so I don't use it
	//makeFractalAltitudes();
	makeTrigAltitudes();

	// prepare to draw textures
	glGetIntegerv(GL_VIEWPORT, viewport);
	glViewport(0, 0, texSize, texSize);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0.0f, 1.0f, 0.0f, 1.0f, -0.5f, 0.5f);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glRotatef(-90.0f, 1, 0, 0);
	glReadBuffer(GL_BACK);
	//glPixelStorei(GL_UNPACK_ROW_LENGTH, texSize);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glDisable(GL_TEXTURE_2D);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glEnable(GL_BLEND);

	// project vertices and create textures
	float recvert = float(geoRes) * 0.5f;  // reciprocal of vertical component of light ray
	for(k=0; k<numFrames; k++){
		// compute projected offsets
		// (this uses surface normals, not actual refractions, but it's faster this way)
		for(i=0; i<geoRes; i++){
			for(j=0; j<geoRes; j++){
				makeIndices(i, &xminus, &xplus);
				xz[i][j][0] = (y[k][xplus][j] - y[k][xminus][j]) * recvert * (depth + y[k][i][j]);
				makeIndices(j, &zminus, &zplus);
				xz[i][j][1] = (y[k][i][zplus] - y[k][i][zminus]) * recvert * (depth + y[k][i][j]);
			}
		}
		
		// copy offsets to edges of xz array
		for(i=0; i<geoRes; i++){
			xz[i][geoRes][0] = xz[i][0][0];
			xz[i][geoRes][1] = xz[i][0][1];
		}
		for(j=0; j<=geoRes; j++){
			xz[geoRes][j][0] = xz[0][j][0];
			xz[geoRes][j][1] = xz[0][j][1];
		}
		
		// compute light intensities
		float space = 1.0f / float(geoRes);
		for(i=0; i<geoRes; i++){
			for(j=0; j<geoRes; j++){
				makeIndices(i, &xminus, &xplus);
				makeIndices(j, &zminus, &zplus);
				// this assumes nominal light intensity is 0.25
				intensity[i][j] = (1.0f / (float(geoRes) * float(geoRes)))
					/ ((myFabs(xz[xplus][j][0] - xz[i][j][0] + space)
					+ myFabs(xz[i][j][0] - xz[xminus][j][0] + space))
					* (myFabs(xz[i][zplus][1] - xz[i][j][1] + space)
					+ myFabs(xz[i][j][1] - xz[i][zminus][1] + space)))
					- 0.125f;
				if(intensity[i][j] > 1.0f)
					intensity[i][j] = 1.0f;
			}
		}
		
		// copy intensities to edges of intensity array
		for(i=0; i<geoRes; i++)
			intensity[i][geoRes] = intensity[i][0];
		for(j=0; j<=geoRes; j++)
			intensity[geoRes][j] = intensity[0][j];

		// draw texture
		glClear(GL_COLOR_BUFFER_BIT);
		// draw most of texture
		draw(0, geoRes, 0, geoRes);
		// draw edges of texture that wrap around from opposite sides
		int numRows = geoRes / 10;
		glPushMatrix();
			glTranslatef(-1.0f, 0.0f, 0.0f);
			draw(geoRes - numRows, geoRes, 0, geoRes);
		glPopMatrix();
		glPushMatrix();
			glTranslatef(1.0f, 0.0f, 0.0f);
			draw(0, numRows, 0, geoRes);
		glPopMatrix();
		glPushMatrix();
			glTranslatef(0.0f, 0.0f, -1.0f);
			draw(0, geoRes, geoRes - numRows, geoRes);
		glPopMatrix();
		glPushMatrix();
			glTranslatef(0.0f, 0.0f, 1.0f);
			draw(0, geoRes, 0, numRows);
		glPopMatrix();
		// draw corners too
		glPushMatrix();
			glTranslatef(-1.0f, 0.0f, -1.0f);
			draw(geoRes - numRows, geoRes, geoRes - numRows, geoRes);
		glPopMatrix();
		glPushMatrix();
			glTranslatef(1.0f, 0.0f, -1.0f);
			draw(0, numRows, geoRes - numRows, geoRes);
		glPopMatrix();
		glPushMatrix();
			glTranslatef(-1.0f, 0.0f, 1.0f);
			draw(geoRes - numRows, geoRes, 0, numRows);
		glPopMatrix();
		glPushMatrix();
			glTranslatef(1.0f, 0.0f, 1.0f);
			draw(0, numRows, 0, numRows);
		glPopMatrix();

		// read back texture
		glReadPixels(0, 0, texSize, texSize, GL_RGB, GL_UNSIGNED_BYTE, bitmap);

		// create texture object
		glBindTexture(GL_TEXTURE_2D, caustictex[k]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		gluBuild2DMipmaps(GL_TEXTURE_2D, 3, texSize, texSize, GL_RGB,
			GL_UNSIGNED_BYTE, bitmap);
		
#ifdef WIN32
		wglSwapLayerBuffers(hdc, WGL_SWAP_MAIN_PLANE);
#endif
#ifdef RS_XSCREENSAVER
		glXSwapBuffers(xdisplay, xwindow);
#endif
	}

	// restore matrix stack
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

	delete[] x;
	delete[] y;
	delete[] z;
	delete[] xz;
	delete[] intensity;
	delete[] bitmap;
}


void causticTextures::makeFractalAltitudes(){
	int i, j, k, a;
	float keySeparation = float(numFrames) / float(numKeys);
	int* keyFrame = new int[numKeys+1];

	// generate keyframes
	float phase = 0.0f;
	for(k=0; k<numKeys; k++){
		keyFrame[k] = int(float(k) * keySeparation);
		// set all keyframe altitudes to 0.0
		for(i=0; i<geoRes; i++){
			for(j=0; j<geoRes; j++)
				y[keyFrame[k]][i][j] = 0.0f;
		}
		// generate altitudes in first positions
		phase = float(k) * RS_PIx2 / float(numKeys);
		y[keyFrame[k]][0][0] = waveAmp * rsCosf(1.5707f + phase);
		y[keyFrame[k]][geoRes/2][0] = waveAmp * rsCosf(1.5707f + phase);
		y[keyFrame[k]][geoRes/2][geoRes/2] = waveAmp * rsCosf(3.1416f + phase);
		y[keyFrame[k]][0][geoRes/2] = waveAmp * rsCosf(4.7124f + phase);
		// recurse to find remaining altitudes
		altitudeSquare(0, geoRes/2, 0, geoRes/2, y[keyFrame[k]]);
		altitudeSquare(geoRes/2, geoRes, 0, geoRes/2, y[keyFrame[k]]);
		altitudeSquare(0, geoRes/2, geoRes/2, geoRes, y[keyFrame[k]]);
		altitudeSquare(geoRes/2, geoRes, geoRes/2, geoRes, y[keyFrame[k]]);
	}

	// interpolate to find remaining frames
	int diff, kk;
	int kf0, kf1, kf2, kf3;  // keyframe indices
	float where;
	keyFrame[numKeys] = numFrames;
	for(k=0; k<numKeys; k++){
		kf1 = keyFrame[k];
		kf2 = keyFrame[k+1];
		diff = kf2 - kf1;
		if(kf2 == numFrames)
			kf2 = 0;
		kk = k - 1;
		if(kk < 0)
			kk = numKeys - 1;
		kf0 = keyFrame[kk];
		kk = k + 2;
		if(kk >= numKeys)
			kk -= numKeys;
		kf3 = keyFrame[kk];
		if(diff > 1){
			for(a=kf1+1; a<kf1+diff; a++){
				where = float(a-kf1) / float(diff);
				for(i=0; i<geoRes; i++)
					for(j=0; j<geoRes; j++)
						y[a][i][j] = interpolate(y[kf0][i][j], y[kf1][i][j],
							y[kf2][i][j], y[kf3][i][j], where);
			}
		}
	}

	delete[] keyFrame;
}


void causticTextures::altitudeSquare(int left, int right, int bottom, int top, float** alt){
	// find wrapped indices
	int rr = right;
	if(rr == geoRes)
		rr = 0;
	int tt = top;
	if(tt == geoRes)
		tt = 0;

	// for determining if there is a gap to be filled
	int hor = right - left;
	int vert = top - bottom;

	int centerHor = (left + right) / 2;
	int centerVert = (bottom + top) / 2;
	float offset;
	if(hor > 1){  // find bottom and top altitudes
		offset = myFabs(waveAmp * float(x[right] - x[left]));
		if(alt[centerHor][bottom] == 0.0f)
			alt[centerHor][bottom] = (alt[left][bottom] + alt[rr][bottom]) * 0.5f
				+ rsRandf(offset+offset) - offset;
		if(alt[centerHor][tt] == 0.0f)
			alt[centerHor][tt] = (alt[left][tt] + alt[rr][tt]) * 0.5f
				+ rsRandf(offset+offset) - offset;
	}
	if(vert > 1){  // find left and right altitudes
		offset = myFabs(waveAmp * float(z[top] - z[bottom]));
		if(alt[left][centerVert] == 0.0f)
			alt[left][centerVert] = (alt[left][bottom] + alt[left][tt]) * 0.5f
				+ rsRandf(offset+offset) - offset;
		if(alt[rr][centerVert] == 0.0f)
			alt[rr][centerVert] = (alt[rr][bottom] + alt[rr][tt]) * 0.5f
				+ rsRandf(offset+offset) - offset;
	}
	if(hor > 1 && vert > 1){  // find center altitude
		offset = waveAmp * 0.5f *
			(myFabs(float(x[right] - x[left]))
			+ myFabs(float(z[top] - z[bottom])));
		alt[centerHor][centerVert] = (alt[left][bottom] + alt[rr][bottom] + alt[left][tt]
			+ alt[rr][tt]) * 0.25f + rsRandf(offset+offset) - offset;
	}

	// keep recursing if necessary
	int quadrant[4] = {0, 0, 0, 0};
	if(centerHor - left > 1){
		quadrant[0] ++;
		quadrant[2] ++;
	}
	if(right - centerHor > 1){
		quadrant[1] ++;
		quadrant[2] ++;
	}
	if(centerVert - bottom > 1){
		quadrant[0] ++;
		quadrant[1] ++;
	}
	if(top - centerVert > 1){
		quadrant[2] ++;
		quadrant[3] ++;
	}
	if(quadrant[0])
		altitudeSquare(left, centerHor, bottom, centerVert, alt);
	if(quadrant[1])
		altitudeSquare(centerHor, right, bottom, centerVert, alt);
	if(quadrant[2])
		altitudeSquare(left, centerHor, centerVert, top, alt);
	if(quadrant[3])
		altitudeSquare(centerHor, right, centerVert, top, alt);

	return;
}


void causticTextures::makeTrigAltitudes(){
	int i, j, k;
	float xx, zz, offset;

	for(k=0; k<numFrames; k++){
		offset = RS_PIx2 * float(k) / float(numFrames);
		for(i=0; i<geoRes; i++){
			xx = RS_PIx2 * float(i) / float(geoRes);
			for(j=0; j<geoRes; j++){
				zz = RS_PIx2 * float(j) / float(geoRes);
				/*y[k][i][j] = waveAmp
					* (0.12f * rsCosf(xx + 2.0f * offset)
					+ 0.08f * rsCosf(-1.0f * xx + 2.0f * zz + offset)
					+ 0.04f * rsCosf(-2.0f * xx - 4.0f * zz + offset)
					+ 0.014f * rsCosf(xx - 7.0f * zz - 2.0f * offset)
					+ 0.014f * rsCosf(3.0f * xx + 5.0f * zz + offset)
					+ 0.014f * rsCosf(9.0f * xx + zz - offset)
					+ 0.007f * rsCosf(11.0f * xx + 7.0f * zz - offset)
					+ 0.007f * rsCosf(4.0f * xx - 13.0f * zz + offset)
					+ 0.007f * rsCosf(19.0f * xx - 9.0f * zz - offset));*/
				y[k][i][j] = waveAmp
					* (0.08f * rsCosf(xx * 2.0f + offset)
					+ 0.06f * rsCosf(-1.0f * xx + 2.0f * zz + offset)
					+ 0.04f * rsCosf(-2.0f * xx - 3.0f * zz + offset)
					+ 0.01f * rsCosf(xx - 7.0f * zz - 2.0f * offset)
					+ 0.01f * rsCosf(3.0f * xx + 5.0f * zz + offset)
					+ 0.01f * rsCosf(9.0f * xx + zz - offset)
					+ 0.005f * rsCosf(11.0f * xx + 7.0f * zz - offset)
					+ 0.005f * rsCosf(4.0f * xx - 13.0f * zz + offset)
					+ 0.003f * rsCosf(19.0f * xx - 9.0f * zz - offset));
			}
		}
	}
}


void causticTextures::draw(int xlo, int xhi, int zlo, int zhi){
	int i, j;
	float mult;

	for(j=zlo; j<zhi; j++){
		// red
		mult = 1.0f - refractionMult / float(geoRes);
		glBegin(GL_TRIANGLE_STRIP);
		for(i=xlo; i<=xhi; i++){
			glColor3f(intensity[i][j+1], 0.0f, 0.0f);
			glVertex3f(x[i] + xz[i][j+1][0] * mult, 0.0f, z[j+1] + xz[i][j+1][1] * mult);
			glColor3f(intensity[i][j], 0.0f, 0.0f);
			glVertex3f(x[i] + xz[i][j][0] * mult, 0.0f, z[j] + xz[i][j][1] * mult);
		}
		glEnd();
		// green
		glBegin(GL_TRIANGLE_STRIP);
		for(i=xlo; i<=xhi; i++){
			glColor3f(0.0f, intensity[i][j+1], 0.0f);
			glVertex3f(x[i] + xz[i][j+1][0], 0.0f, z[j+1] + xz[i][j+1][1]);
			glColor3f(0.0f, intensity[i][j], 0.0f);
			glVertex3f(x[i] + xz[i][j][0], 0.0f, z[j] + xz[i][j][1]);
		}
		glEnd();
		// blue
		mult = 1.0f + refractionMult / float(geoRes);
		glBegin(GL_TRIANGLE_STRIP);
		for(i=xlo; i<=xhi; i++){
			glColor3f(0.0f, 0.0f, intensity[i][j+1]);
			glVertex3f(x[i] + xz[i][j+1][0] * mult, 0.0f, z[j+1] + xz[i][j+1][1] * mult);
			glColor3f(0.0f, 0.0f, intensity[i][j]);
			glVertex3f(x[i] + xz[i][j][0] * mult, 0.0f, z[j] + xz[i][j][1] * mult);
		}
		glEnd();
	}
}


void causticTextures::makeIndices(int index, int* minus, int* plus){
	*minus = index - 1;
	if(*minus < 0)
		*minus = geoRes - 1;
	*plus = index + 1;
	if(*plus >= geoRes)
		*plus = 0;
}


// Here's a little calculus that takes 4 points along a single
// dimension and interpolates smoothly between the second and third
// depending on the value of where which can be 0.0 to 1.0.
// The slope at b is estimated using a and c.  The slope at c
// is estimated using b and d.
float causticTextures::interpolate(float a, float b, float c, float d, float where){
    float q, r, s, t;

    q = (((3.0f * b) + d - a - (3.0f * c)) * (where * where * where)) * 0.5f;
    r = (((2.0f * a) - (5.0f * b) + (4.0f * c) - d) * (where * where)) * 0.5f;
    s = ((c - a) * where) * 0.5f;
    t = b;
    return(q + r + s + t);
}
