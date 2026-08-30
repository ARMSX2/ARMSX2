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
#include "stretchedParticle.h"
#include <stdio.h>
#include <math.h>
#include <GL/gl.h>
#include <GL/glu.h>



//extern float billboardMat[16];
extern float unroll;
extern double modelMat[16];
extern double projMat[16];
extern int viewport[4];



stretchedParticle::stretchedParticle(){
	pos[0] = pos[1] = pos[2] = 0.0f;
	lastPos[0] = lastPos[1] = lastPos[2] = 0.0f;
	drawPos[0] = drawPos[1] = drawPos[2] = 0.0f;
	
	radius = 0.03f;
	color[0] = color[1] = color[2] = 1.0f;
}


void stretchedParticle::draw(float* eyepoint){
	static double winx, winy, winz;
	gluProject(pos[0], pos[1], pos[2],
		modelMat, projMat, viewport,
		&winx, &winy, &winz);
	if(winz > 0.0f){
		screenPos[0] = winx;
		screenPos[1] = winy;
	}

	drawPos[0] = (pos[0] + lastPos[0]) * 0.5f;
	drawPos[1] = (pos[1] + lastPos[1]) * 0.5f;
	drawPos[2] = (pos[2] + lastPos[2]) * 0.5f;
	lastPos[0] = pos[0];
	lastPos[1] = pos[1];
	lastPos[2] = pos[2];

	float sd[2], pd[3];  // screen difference, position difference
	sd[0] = float(screenPos[0] - lastScreenPos[0]);
	sd[1] = float(screenPos[1] - lastScreenPos[1]);
	pd[0] = eyepoint[0] - drawPos[0];
	pd[1] = eyepoint[1] - drawPos[1];
	pd[2] = eyepoint[2] - drawPos[2];

	float bbMat[16];
	bbMat[0] = bbMat[5] = bbMat[10] = bbMat[15] = 1.0f;
	bbMat[1] = bbMat[2] = bbMat[3] = bbMat[4] =
	bbMat[6] = bbMat[7] = bbMat[8] = bbMat[9] =
	bbMat[11] = bbMat[12] = bbMat[13] = bbMat[14] = 0.0f;
	float n =  sqrtf(pd[0] * pd[0] + pd[1] * pd[1] + pd[2] * pd[2]);
	bbMat[8] = pd[0] / n;
	bbMat[9] = pd[1] / n;
	bbMat[10] = pd[2] / n;
	bbMat[0] = bbMat[10];
	bbMat[2] = -bbMat[8];
	bbMat[4] = bbMat[9] * bbMat[2] - bbMat[1] * bbMat[10];
	bbMat[5] = bbMat[10] * bbMat[0] - bbMat[2] * bbMat[8];
	bbMat[6] = bbMat[8] * bbMat[1] - bbMat[0] * bbMat[9];

	float stretch = 0.0015f * sqrtf(sd[0] * sd[0] + sd[1] * sd[1]) * n / radius;
	if(stretch < 1.0f)
		stretch = 1.0f;
	if(stretch > 0.5f / radius)
		stretch = 0.5f / radius;
	glPushMatrix();
		glTranslatef(drawPos[0], drawPos[1], drawPos[2]);
		glMultMatrixf(bbMat);
		glRotatef(57.2957795131f * atan2f(sd[1], sd[0]) + unroll, 0, 0, 1);
		glScalef(stretch, 1.0f, 1.0f);
		float darkener = stretch * 0.3f;
		if(darkener < 1.0f)
			darkener = 1.0f;
		// draw colored aura
		glColor3f(color[0] / darkener, color[1] / darkener, color[2] / darkener);
		glBegin(GL_TRIANGLE_STRIP);
			glTexCoord2f(0.0f, 0.0f);
			glVertex3f(-radius, -radius, 0.0f);
			glTexCoord2f(1.0f, 0.0f);
			glVertex3f(radius, -radius, 0.0f);
			glTexCoord2f(0.0f, 1.0f);
			glVertex3f(-radius, radius, 0.0f);
			glTexCoord2f(1.0f, 1.0f);
			glVertex3f(radius, radius, 0.0f);
		glEnd();
		// draw white center
		glColor3f(1.0f / darkener, 1.0f / darkener, 1.0f / darkener);
		glBegin(GL_TRIANGLE_STRIP);
			glTexCoord2f(0.0f, 0.0f);
			glVertex3f(-radius*0.3f, -radius*0.3f, 0.0f);
			glTexCoord2f(1.0f, 0.0f);
			glVertex3f(radius*0.3f, -radius*0.3f, 0.0f);
			glTexCoord2f(0.0f, 1.0f);
			glVertex3f(-radius*0.3f, radius*0.3f, 0.0f);
			glTexCoord2f(1.0f, 1.0f);
			glVertex3f(radius*0.3f, radius*0.3f, 0.0f);
		glEnd();
	glPopMatrix();
	
	lastScreenPos[0] = screenPos[0];
	lastScreenPos[1] = screenPos[1];
}
